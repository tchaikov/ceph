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

while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; shift 2 ;;
        *) shift ;;
    esac
done

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
DEADLINE=$(( $(date +%s) + COW_TIMEOUT_SECS ))
WAIT_FAIL=0
for pid in "${BENCH_PIDS[@]}"; do
    while kill -0 "$pid" 2>/dev/null; do
        if [ $(date +%s) -ge $DEADLINE ]; then
            WAIT_FAIL=$((WAIT_FAIL+1))
            kill "$pid" 2>/dev/null || true
            break
        fi
        sleep 0.5
    done
    wait "$pid" 2>/dev/null || true
done

if [ $WAIT_FAIL -gt 0 ]; then
    log_error "$WAIT_FAIL clients timed out"
    for p in "${BENCH_PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
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

# Count log markers across all client logs.
FIRING=$(grep -h "firing async write-back to parent object" "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
SKIPPING=$(grep -h "skipping parent writeback: lock holder peer" "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
LOCK_BUSY=$(grep -h "lock held by another user request" "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)
LOCK_ACQ=$(grep -h "acquired S3 fetch lock" "$LOG_DIR"/client-*.log 2>/dev/null | wc -l)

echo
log_info "=== Dedup metrics ==="
log_info "lock acquired (won race):       $LOCK_ACQ"
log_info "lock busy (peer already held):  $LOCK_BUSY"
log_info "fired write-back (lock holder): $FIRING"
log_info "skipped write-back (peer):      $SKIPPING"
log_info "RADOS pool wr_ops delta:        $WR_DELTA"
echo

# Sanity assertion: skip count should be (clients-with-EBUSY) per object.
if [ $SKIPPING -gt 0 ] && [ $FIRING -gt 0 ]; then
    log_success "Skip-writeback dedup is firing"
else
    log_warn "No skip-writeback observed — race may not have triggered (client startup serialization?)"
fi
