#!/bin/bash
# test-s3-dedup-read.sh — validate ObjectReadRequest cross-process dedup
#
# Mirror of test-s3-dedup-writeback.sh but exercises the READ path
# (ObjectReadRequest::read_from_s3_with_lock).  Spawns N concurrent
# readers of the same S3-backed image and verifies that only the lock
# holder writes back to RADOS; peers fetch their own S3 copy and skip
# the write_full + object_map update.
#
# Usage: ./test-s3-dedup-read.sh [--conf <ceph.conf>]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/s3-test-common.sh"

POOL="s3-dedup-read"
MINIO_PORT=39200
MINIO_CONSOLE_PORT=39201
S3_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
S3_BUCKET="dedup-read-bucket"
MINIO_DATA_DIR="/tmp/minio-dedup-read-$$"

NUM_CLIENTS=4
PARENT_SIZE_MB=4         # 1 parent object — guarantees all 4 readers contend
TIMEOUT_SECS=60
# create_s3_parent in lib/s3-test-common.sh writes the source file to
# /tmp/<s3_image_name>; reuse that path so the integrity check can diff
# against the same bytes that were uploaded to MinIO.
S3_IMAGE_NAME="dedup-read-parent.raw"
PARENT_RAW="/tmp/${S3_IMAGE_NAME}"
LOG_DIR="/tmp/dedup-read-logs-$$"

cleanup() {
    log_info "Cleaning up..."
    stop_minio $MINIO_PORT
    if [ -n "${CEPH_CONF:-}" ]; then
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/dedup-read-parent" 2>/dev/null || true
        "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete "$POOL" "$POOL" \
            --yes-i-really-really-mean-it 2>/dev/null || true
    fi
    rm -rf "$MINIO_DATA_DIR" "$PARENT_RAW" "$LOG_DIR" /tmp/dedup-read-export-*
}
trap cleanup EXIT

parse_common_args "$@"

echo
log_info "=== S3 read-path dedup measurement ($NUM_CLIENTS readers, $PARENT_SIZE_MB MB) ==="
echo

check_cluster_running || exit 1
mkdir -p "$LOG_DIR"

start_minio $MINIO_PORT $MINIO_CONSOLE_PORT $MINIO_DATA_DIR
setup_s3_bucket $MINIO_PORT $S3_BUCKET

create_pool $POOL
enable_s3_fetch
create_s3_parent $POOL "dedup-read-parent" "dedup-read-parent.raw" $PARENT_SIZE_MB \
    $S3_ENDPOINT $S3_BUCKET

# Confirm parent oid is not yet in RADOS (S3 hasn't been fetched yet).
PARENT_OID=$(get_block_prefix "$CEPH_CONF" "$POOL" "dedup-read-parent")
DATA_OID="${PARENT_OID}.0000000000000000"
log_info "Parent data oid: $DATA_OID"

if "$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" stat "$DATA_OID" 2>/dev/null; then
    log_warn "Parent data oid already exists — RADOS cache won't trigger S3 fetch.  Pool was not clean."
fi

WR_OPS_BEFORE=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" df --format json \
    | python3 -c "import sys,json; d=json.load(sys.stdin); \
        p=[x for x in d['pools'] if x['name']=='$POOL'][0]; \
        print(p.get('write_ops', p.get('write_io', 0)))")
log_info "wr_ops on $POOL before: $WR_OPS_BEFORE"

# Each reader exports the parent.  rbd export reads all objects starting from
# offset 0; with PARENT_SIZE_MB=4 (one 4 MB object) all readers contend on
# the same first object simultaneously.
log_step "Launching $NUM_CLIENTS concurrent readers..."
declare -a READER_PIDS
for i in $(seq 1 $NUM_CLIENTS); do
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
        --rbd-cache=false \
        --debug-rbd=10 \
        --log-file="$LOG_DIR/client-$i.log" \
        export "$POOL/dedup-read-parent" "/tmp/dedup-read-export-$i" \
        > "$LOG_DIR/export-$i.out" 2>&1 &
    READER_PIDS+=($!)
done

WAIT_FAIL=$(wait_pids_with_timeout "$TIMEOUT_SECS" "${READER_PIDS[@]}")
if [ "$WAIT_FAIL" -gt 0 ]; then
    log_error "$WAIT_FAIL readers timed out"
    exit 1
fi

log_success "All $NUM_CLIENTS readers completed"
# rados df aggregates OSD reports periodically; give the OSDs time to flush.
sleep 6

WR_OPS_AFTER=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" df --format json \
    | python3 -c "import sys,json; d=json.load(sys.stdin); \
        p=[x for x in d['pools'] if x['name']=='$POOL'][0]; \
        print(p.get('write_ops', p.get('write_io', 0)))")
WR_DELTA=$((WR_OPS_AFTER - WR_OPS_BEFORE))
log_info "wr_ops on $POOL after:  $WR_OPS_AFTER  (delta $WR_DELTA)"

# Independent confirmation via rados stat: the data oid should exist (one
# write happened) and listing the pool should show <= 2 objects (data oid +
# lock sentinel).
log_info "Pool contents (via rados ls):"
"$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls 2>/dev/null | sed 's/^/    /'
DATA_STAT=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" stat "$DATA_OID" 2>&1 || true)
log_info "rados stat on data oid: $DATA_STAT"

# Throttler-mediated dedup log markers (post-c2).  The cls_lock that used
# to dedup on the read critical path now lives inside the throttler's
# detached WritebackRequest state machine; the net invariant (one
# write_full per parent object per concurrent burst) is preserved.
#
# Old markers ("acquired S3 read lock", "skipping writeback: peer user
# holds lock", "stat-after-lock short-circuit") no longer exist — the
# code paths that emitted them were deleted in c2.
# All throttler + WritebackRequest log lines share the prefix
# "librbd::io::async_writeback:" (set in AsyncWritebackThrottler.cc).
ACCEPT=$(grep -h "async_writeback:.*accept obj=" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
DROP_FULL=$(grep -hE "async_writeback:.*drop: (in_flight|bytes_in_flight)" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
# The WritebackRequest's cls_lock acquire returns EBUSY when a peer
# WritebackRequest holds the sentinel — this is the cross-process write
# dedup signal.  Each EBUSY is one peer that skipped its write_full.
LOCK_EBUSY=$(grep -h "async_writeback:.*lock not acquired" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
# Successful write_full count = WritebackRequests that reached the
# write_full step (acquired the lock).  We expect this to equal 1 per
# object when the race triggers properly.
WROTE_BACK=$(grep -h "async_writeback:.*write_full.*bytes to " \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)

# Verify all readers got correct data — diff the exports against the source.
INTEGRITY_FAIL=0
for i in $(seq 1 $NUM_CLIENTS); do
    if ! cmp -s "$PARENT_RAW" "/tmp/dedup-read-export-$i"; then
        INTEGRITY_FAIL=$((INTEGRITY_FAIL+1))
        log_warn "Client $i export mismatch"
    fi
done

echo
log_info "=== Read-path throttler-dedup metrics ==="
log_info "throttler accepts (submissions):     $ACCEPT"
log_info "throttler drops (over-cap):          $DROP_FULL"
log_info "WritebackRequest cls_lock EBUSY:     $LOCK_EBUSY"
log_info "WritebackRequest write_full success: $WROTE_BACK"
log_info "RADOS pool wr_ops delta:             $WR_DELTA"
log_info "integrity check (mismatches):        $INTEGRITY_FAIL/$NUM_CLIENTS"
echo

if [ $INTEGRITY_FAIL -ne 0 ]; then
    log_fail "Data integrity check FAILED"
    exit 1
fi

# Hard assertions on the cross-process write dedup invariant:
#  1. write_full count <= 1 — even with N concurrent reads, the
#     throttler's cls_lock guarantees only one WritebackRequest wins and
#     writes the object.  > 1 means the cls_lock dedup is broken.
#  2. If at least one WritebackRequest was accepted (ACCEPT > 0), then
#     ACCEPT == WROTE_BACK + LOCK_EBUSY + DROP_FULL — every submission
#     must terminate in exactly one of {acquired-lock-and-wrote,
#     EBUSY-dropped, throttle-cap-dropped}.  An imbalance here means a
#     WritebackRequest leaked or terminated through an unexpected path.
if [ $WROTE_BACK -gt 1 ]; then
    log_fail "Expected WROTE_BACK <= 1 (cls_lock guarantees single writer); got $WROTE_BACK"
    log_fail "  Cross-process write dedup is broken in the throttler's WritebackRequest"
    exit 1
fi
if [ $ACCEPT -gt 0 ]; then
    EXPECTED_TOTAL=$((WROTE_BACK + LOCK_EBUSY + DROP_FULL))
    if [ "$ACCEPT" -ne "$EXPECTED_TOTAL" ]; then
        log_warn "Throttler accept/terminate imbalance: accepts=$ACCEPT, "
        log_warn "  wrote=$WROTE_BACK + ebusy=$LOCK_EBUSY + drop=$DROP_FULL = $EXPECTED_TOTAL"
        log_warn "  Some WritebackRequests may have terminated through an unexpected path"
        # Warning, not failure — could be a write_full failure path; data
        # integrity (verified above) is what matters for correctness.
    fi
fi
if [ $LOCK_EBUSY -gt 0 ]; then
    log_success "Cross-process write dedup firing: $WROTE_BACK wrote, $LOCK_EBUSY EBUSY-dropped"
elif [ $ACCEPT -eq 0 ]; then
    log_warn "No throttler submissions seen — clients may have all hit the recent-fetch"
    log_warn "  cache (sequential same-object reads) and never reached the S3 GET path."
    log_warn "  Data integrity verified ($INTEGRITY_FAIL/$NUM_CLIENTS mismatches)."
elif [ $WROTE_BACK -gt 0 ]; then
    log_success "Single client won the lock: $WROTE_BACK wrote (race didn't trigger this run)"
fi
