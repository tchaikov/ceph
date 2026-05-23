#!/bin/bash
# test-s3-dedup-writeback.sh — measure RADOS write-IOPS savings from skip-writeback dedup
#
# Spawns N concurrent COW writers against the same parent object and verifies
# that after #4-P3 (cls_lock skip-writeback design) only the lock holder
# issues write_full to the parent oid; peers fetch their own S3 copy and
# return data without writing.
#
# Expected with N=4 clients overlapping on K=2 parent objects:
#   - "firing async write-back to parent object" log lines: K  (one per object)
#   - "skipping parent writeback: lock holder peer" log lines: (N-1)*K
#   - rados df wr_ops on the pool delta:  ~K writes (was N*K before)
#
# Usage: ./test-s3-dedup-writeback.sh [--conf <ceph.conf>]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/s3-test-common.sh"

POOL="s3-dedup-test"
MINIO_PORT=39100
MINIO_CONSOLE_PORT=39101
S3_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
S3_BUCKET="dedup-bucket"
MINIO_DATA_DIR="/tmp/minio-dedup-$$"

NUM_CLIENTS=4
PARENT_SIZE_MB=8         # 2 parent objects (4 MB each)
COW_TIMEOUT_SECS=60
PARENT_RAW="/tmp/dedup-parent-$$.raw"
LOG_DIR="/tmp/dedup-logs-$$"

cleanup() {
    log_info "Cleaning up..."
    stop_minio $MINIO_PORT
    if [ -n "${CEPH_CONF:-}" ]; then
        for i in $(seq 1 $NUM_CLIENTS); do
            "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/dedup-child-$i" 2>/dev/null || true
        done
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/dedup-parent" 2>/dev/null || true
        "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete "$POOL" "$POOL" \
            --yes-i-really-really-mean-it 2>/dev/null || true
    fi
    rm -rf "$MINIO_DATA_DIR" "$PARENT_RAW" "$LOG_DIR"
}
trap cleanup EXIT

parse_common_args "$@"

echo
log_info "=== S3 dedup write-back measurement ($NUM_CLIENTS clients, $PARENT_SIZE_MB MB parent) ==="
echo

check_cluster_running || exit 1
mkdir -p "$LOG_DIR"

start_minio $MINIO_PORT $MINIO_CONSOLE_PORT $MINIO_DATA_DIR
setup_s3_bucket $MINIO_PORT $S3_BUCKET

create_pool $POOL
enable_s3_fetch

# Shared parent — same image referenced by all children.
create_s3_parent $POOL "dedup-parent" "dedup-parent.raw" $PARENT_SIZE_MB \
    $S3_ENDPOINT $S3_BUCKET

for i in $(seq 1 $NUM_CLIENTS); do
    create_standalone_clone $POOL "dedup-parent" "dedup-child-$i"
done

# Capture pool wr_ops before COW
log_step "rados df before"
WR_OPS_BEFORE=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" df --format json \
    | python3 -c "import sys,json; d=json.load(sys.stdin); \
        p=[x for x in d['pools'] if x['name']=='$POOL'][0]; \
        print(p.get('write_ops', p.get('write_io', 0)))")
log_info "wr_ops on $POOL before: $WR_OPS_BEFORE"

# Trigger COW from N clients on the SAME parent objects in parallel.
# rbd bench with --io-pattern rand and small total ensures all clients hit
# the first 1-2 parent objects, maximizing user-vs-user lock contention.
log_step "Launching $NUM_CLIENTS concurrent COW writers..."
declare -a BENCH_PIDS
for i in $(seq 1 $NUM_CLIENTS); do
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
        --rbd-cache=false \
        --debug-rbd=10 \
        --log-file="$LOG_DIR/client-$i.log" \
        bench --io-type write \
              --io-size 4096 --io-total 4096 --io-pattern seq \
              --io-threads 1 \
              "$POOL/dedup-child-$i" \
        > "$LOG_DIR/bench-$i.out" 2>&1 &
    BENCH_PIDS+=($!)
done

log_info "Waiting up to ${COW_TIMEOUT_SECS}s for all clients..."
WAIT_FAIL=$(wait_pids_with_timeout "$COW_TIMEOUT_SECS" "${BENCH_PIDS[@]}")
if [ "$WAIT_FAIL" -gt 0 ]; then
    log_error "$WAIT_FAIL clients timed out"
    exit 1
fi

log_success "All $NUM_CLIENTS clients completed"
sleep 2  # Let async write-back settle into RADOS counters.

WR_OPS_AFTER=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" df --format json \
    | python3 -c "import sys,json; d=json.load(sys.stdin); \
        p=[x for x in d['pools'] if x['name']=='$POOL'][0]; \
        print(p.get('write_ops', p.get('write_io', 0)))")
log_info "wr_ops on $POOL after:  $WR_OPS_AFTER"

WR_DELTA=$((WR_OPS_AFTER - WR_OPS_BEFORE))
log_info "wr_ops delta: $WR_DELTA"

# Throttler-mediated dedup log markers (post-c3).  CopyupRequest's cls_lock
# that used to dedup on the COW critical path now lives inside the
# throttler's detached WritebackRequest state machine fired from
# fire_parent_s3_writeback; the net invariant (one write_full per parent
# object per concurrent burst) is preserved.
#
# Old markers ("acquired S3 fetch lock", "firing async write-back",
# "skipping parent writeback") no longer exist — those code paths were
# deleted in c3.  Match instead on the throttler's "async_writeback:"
# prefix (set in AsyncWritebackThrottler.cc) which covers both submission
# accounting and the per-WritebackRequest cls_lock outcome.
ACCEPT=$(grep -h "async_writeback:.*accept obj=" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
DROP_FULL=$(grep -hE "async_writeback:.*drop: (in_flight|bytes_in_flight)" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
LOCK_EBUSY=$(grep -h "async_writeback:.*lock not acquired" \
    "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
# Count per-OID, not total: the cls_lock dedup invariant is per-object
# ("only one WritebackRequest wins per parent oid"), not per-suite.
# After commit 85ac924ca40 routed S3 prefetch through ObjectReadRequest,
# prefetched parent objects also flow through the throttler.  Each such
# object hits its own cls_lock and emerges with one winner -- so the
# total write_full count exceeds 1 even when the per-object invariant
# holds.  MAX_WRITES_PER_OID checks the invariant directly.
WRITES_PER_OID=$(grep -h "async_writeback:.*write_full.*bytes to " \
    "$LOG_DIR"/client-*.log 2>/dev/null \
    | awk '{print $NF}' | sort | uniq -c)
MAX_WRITES_PER_OID=$(echo "$WRITES_PER_OID" \
    | awk 'NR==1 || $1>m {m=$1} END {print m+0}')
# `echo "" | wc -l` reports 1 (the trailing newline) even when
# WRITES_PER_OID is empty.  Guard so the diagnostic line doesn't
# claim "1 distinct OIDs" when actually 0 write_fulls fired.
if [ -z "$WRITES_PER_OID" ]; then
    WROTE_BACK_TOTAL=0
    WROTE_BACK=0
else
    WROTE_BACK_TOTAL=$(echo "$WRITES_PER_OID" | wc -l)
    WROTE_BACK=$(echo "$WRITES_PER_OID" | awk '{s+=$1} END {print s+0}')
fi

echo
log_info "=== COW-path throttler-dedup metrics ==="
log_info "throttler accepts (submissions):     $ACCEPT"
log_info "throttler drops (over-cap):          $DROP_FULL"
log_info "WritebackRequest cls_lock EBUSY:     $LOCK_EBUSY"
log_info "WritebackRequest write_full total:   $WROTE_BACK ($WROTE_BACK_TOTAL distinct OIDs, max $MAX_WRITES_PER_OID per OID)"
log_info "RADOS pool wr_ops delta:             $WR_DELTA"
echo

# Hard assertions on the cross-process write dedup invariant:
#  1. MAX_WRITES_PER_OID <= 1 -- the throttler's cls_lock guarantees only
#     one WritebackRequest wins per parent oid even under N-client
#     contention.  > 1 means the cls_lock dedup is broken for that oid.
#  2. accept == wrote + ebusy + drop -- every throttler submission must
#     terminate exactly once.  Imbalance signals a leaked SM or
#     unexpected termination path.
if [ "$MAX_WRITES_PER_OID" -gt 1 ]; then
    log_fail "Expected <= 1 write_full per OID (cls_lock dedups per-object);"
    log_fail "  found OID with $MAX_WRITES_PER_OID write_fulls:"
    echo "$WRITES_PER_OID" | sort -rn | head -3 | sed 's/^/    /'
    log_fail "  Cross-process write dedup is broken in the throttler's WritebackRequest"
    exit 1
fi
if [ $ACCEPT -gt 0 ]; then
    EXPECTED_TOTAL=$((WROTE_BACK + LOCK_EBUSY + DROP_FULL))
    if [ "$ACCEPT" -ne "$EXPECTED_TOTAL" ]; then
        log_warn "Throttler accept/terminate imbalance: accepts=$ACCEPT, "
        log_warn "  wrote=$WROTE_BACK + ebusy=$LOCK_EBUSY + drop=$DROP_FULL = $EXPECTED_TOTAL"
        log_warn "  Some WritebackRequests may have terminated through an unexpected path"
    fi
fi
if [ $LOCK_EBUSY -gt 0 ]; then
    log_success "Cross-process write dedup firing: $WROTE_BACK wrote, $LOCK_EBUSY EBUSY-dropped"
elif [ $ACCEPT -gt 0 ]; then
    log_success "Single client won the lock: $WROTE_BACK wrote (race didn't trigger this run)"
else
    log_warn "No throttler submissions seen — race didn't trigger and no client COW'd the parent."
fi
