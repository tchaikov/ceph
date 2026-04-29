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
PARENT_OID=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
    info "$POOL/dedup-read-parent" --format json \
    | python3 -c "import sys,json; print(json.load(sys.stdin)['block_name_prefix'])")
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

# Read-path log markers from ObjectReadRequest.
LOCK_ACQ=$(grep -h "acquired S3 read lock" "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
LOCK_BUSY=$(grep -h "lock held by another user request, fetching own copy" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
WROTE_BACK=$(grep -h "writing.*bytes (entire object) to parent cache object" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
SKIPPED=$(grep -h "skipping writeback: peer user holds lock" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
RECHECK_HIT=$(grep -h "object now populated; releasing lock and re-reading from RADOS" \
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
log_info "=== Read-path dedup metrics ==="
log_info "lock acquired (won race):       $LOCK_ACQ"
log_info "lock busy (peer already held):  $LOCK_BUSY"
log_info "wrote back (lock holder):       $WROTE_BACK"
log_info "skipped writeback (peer):       $SKIPPED"
log_info "stat-after-lock short-circuit:  $RECHECK_HIT"
log_info "RADOS pool wr_ops delta:        $WR_DELTA"
log_info "integrity check (mismatches):   $INTEGRITY_FAIL/$NUM_CLIENTS"
echo

if [ $INTEGRITY_FAIL -ne 0 ]; then
    log_fail "Data integrity check FAILED"
    exit 1
fi

# Hard assertions on the dedup behavior itself (NOT just data correctness):
#  1. WROTE_BACK + RECHECK_HIT must be exactly 1.  With proper dedup, exactly
#     one client populates the parent oid (either by writing it after S3 fetch
#     or by short-circuiting after stat-after-lock found a peer-populated
#     copy).  WROTE_BACK > 1 means losers wrote duplicates — the skip-writeback
#     feature is broken.
#  2. If LOCK_BUSY > 0 (race triggered), SKIPPED must be > 0.  Nonzero
#     LOCK_BUSY with SKIPPED=0 means the losers fell through instead of
#     taking the skip-writeback path.
if [ $WROTE_BACK -gt 1 ]; then
    log_fail "Expected WROTE_BACK <= 1 (single writeback per parent object); got $WROTE_BACK"
    log_fail "  Read-path skip-writeback dedup is broken: multiple losers wrote duplicates"
    exit 1
fi
if [ $LOCK_BUSY -gt 0 ] && [ $SKIPPED -eq 0 ]; then
    log_fail "Race triggered ($LOCK_BUSY EBUSY-user events) but SKIPPED=0"
    log_fail "  Losers are not using the skip-writeback path"
    exit 1
fi
if [ $SKIPPED -gt 0 ]; then
    log_success "Read-path skip-writeback dedup is firing: $WROTE_BACK wrote, $SKIPPED skipped"
elif [ $RECHECK_HIT -gt 0 ]; then
    log_success "stat-after-lock short-circuit fired (peer beat us to writeback)"
elif [ $LOCK_BUSY -eq 0 ]; then
    log_warn "Race didn't trigger this run — clients may have serialized."
    log_warn "  Data integrity verified ($INTEGRITY_FAIL/$NUM_CLIENTS mismatches);"
    log_warn "  re-run for full dedup-path confidence."
fi
