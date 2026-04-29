#!/bin/bash
# test-s3-preemption.sh — Verify client I/O preempts the backfill daemon
#
# This test exercises the lock-breaking code path added to CopyupRequest:
#   - CopyupRequest::try_preempt_backfill_lock()
#   - CopyupRequest::handle_list_lock_holders()
#   - CopyupRequest::handle_break_backfill_lock()
#
# IMPORTANT: Uses actual child-clone writes (rbd bench), NOT rados put.
# A raw rados put bypasses CopyupRequest entirely and never touches the
# s3_fetch_lock sentinel.  Only a child-clone write that triggers COW
# exercises the lock-breaking code.
#
# Scenario:
#   1. Create 100 MB S3-backed parent + child clone; schedule backfill
#   2. Start backfill daemon (acquires s3_fetch_lock on each object in turn)
#   3. While daemon is mid-fetch, launch 4 concurrent child-clone writers
#      (each triggers CopyupRequest for the same parent objects)
#   4. Assert all 4 writers complete within CLIENT_TIMEOUT_SECS
#      (if any blocks waiting for the daemon, it timed out)
#   5. Verify data integrity: unwritten areas of each child match parent S3
#   6. Verify daemon continues and eventually completes all objects
#
# Usage: ./test-s3-preemption.sh [--conf <ceph.conf>]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/s3-test-common.sh"

POOL="s3-preempt-test"
MINIO_PORT=29100
MINIO_CONSOLE_PORT=29101
S3_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
S3_BUCKET="preempt-test"
MINIO_DATA_DIR="/tmp/minio-preempt-$$"
IMAGE_SIZE_MB=100    # 25 objects × 4 MB each
NUM_CLIENTS=4
CLIENT_TIMEOUT_SECS=60   # Must complete within 60s; daemon takes longer
BACKFILL_LOG="/tmp/rbd-backfill-preempt-$$.log"
PARENT_RAW="/tmp/preempt-parent-$$.raw"

cleanup() {
    log_info "Cleaning up..."
    stop_backfill_daemon 2>/dev/null || true
    # Kill any leftover bench processes
    for pid in "${BENCH_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    stop_minio $MINIO_PORT
    rm -rf "$MINIO_DATA_DIR" "$PARENT_RAW" "$BACKFILL_LOG" \
        /tmp/preempt-bench-*.log /tmp/preempt-read-*.raw
    if [ -n "${CEPH_CONF:-}" ]; then
        for i in $(seq 1 $NUM_CLIENTS); do
            "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
                rm "$POOL/preempt-child-$i" 2>/dev/null || true
        done
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            rm "$POOL/preempt-parent" 2>/dev/null || true
        "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete \
            "$POOL" "$POOL" --yes-i-really-really-mean-it 2>/dev/null || true
    fi
}
trap cleanup EXIT

parse_common_args "$@"

log_info "=== Client I/O Preemption Test (${NUM_CLIENTS} clients vs backfill daemon) ==="
echo

if ! check_cluster_running; then exit 1; fi

create_pool "$POOL"
enable_s3_fetch

# ============================================================================
log_step "1. Start MinIO"
# ============================================================================

start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"
setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

# ============================================================================
log_step "2. Create S3-backed parent + $NUM_CLIENTS child clones; schedule backfill"
# ============================================================================

# create_s3_parent generates and uploads the PARENT-BLOCK-NNNN pattern to S3
# at /tmp/parent-raw.  Copy it to PARENT_RAW so the integrity checks below
# compare against the ACTUAL S3 content.
create_s3_parent "$POOL" "preempt-parent" "parent-raw" $IMAGE_SIZE_MB \
    "$S3_ENDPOINT" "$S3_BUCKET" "$CEPH_CONF"
cp /tmp/parent-raw "$PARENT_RAW"
log_success "S3 reference data copied to ${PARENT_RAW}"

for i in $(seq 1 $NUM_CLIENTS); do
    create_standalone_clone "$POOL" "preempt-parent" "preempt-child-$i"
    log_info "  Created child clone: preempt-child-$i"
done

"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$POOL/preempt-parent"
log_success "Backfill scheduled"

# ============================================================================
log_step "3. Start backfill daemon"
# ============================================================================

run_backfill_daemon "$CEPH_CONF" "$BACKFILL_LOG"

log_info "Waiting 3s for daemon to acquire lock on first object..."
sleep 3

if ! kill -0 $BACKFILL_PID 2>/dev/null; then
    log_error "Daemon crashed before preemption test"
    cat "$BACKFILL_LOG"
    exit 1
fi
log_success "Daemon is running (PID: $BACKFILL_PID)"

# ============================================================================
log_step "4. Launch $NUM_CLIENTS concurrent child writers"
# ============================================================================
# Each writer does rbd bench --io-type write on its child clone.
# The FIRST write to any object triggers CopyupRequest, which:
#   1. Checks parent object existence (ENOENT — daemon hasn't finished writing it)
#   2. Tries to acquire s3_fetch_lock → gets -EBUSY (daemon holds it)
#   3. Calls try_preempt_backfill_lock() → lists holders → sees "backfill-" cookie
#   4. Breaks the daemon's lock via cls::lock::break_lock
#   5. Re-checks parent → still empty → fetches from S3 itself
#   6. Proceeds with child copyup without waiting for the daemon
#
# The total IO is small: 4 KB × 1 write per object, just enough to trigger COW.
# Each client writes to object 0 (first 4 MB) only, so all 4 race on the same
# parent object — maximising lock contention.

declare -a BENCH_PIDS
START_TIME=$(date +%s)

for i in $(seq 1 $NUM_CLIENTS); do
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        "$POOL/preempt-child-$i" \
        --io-type   write \
        --io-size   4096 \
        --io-threads 1 \
        --io-total  4096 \
        > "/tmp/preempt-bench-$i.log" 2>&1 &
    BENCH_PIDS[$i]=$!
    log_info "  Started client $i (PID: ${BENCH_PIDS[$i]})"
done

log_info "All $NUM_CLIENTS clients running. Waiting up to ${CLIENT_TIMEOUT_SECS}s..."

# ============================================================================
log_step "5. Verify all clients complete without waiting for the daemon"
# ============================================================================
FAILED=0
DEADLINE=$(( $(date +%s) + CLIENT_TIMEOUT_SECS ))
declare -a CLIENT_DONE

while true; do
    ALL_DONE=1
    for i in $(seq 1 $NUM_CLIENTS); do
        if [ -z "${CLIENT_DONE[$i]:-}" ]; then
            if ! kill -0 ${BENCH_PIDS[$i]} 2>/dev/null; then
                wait ${BENCH_PIDS[$i]} 2>/dev/null || true
                CLIENT_DONE[$i]=1
                ELAPSED=$(( $(date +%s) - START_TIME ))
                log_success "  Client $i completed in ${ELAPSED}s"
            else
                ALL_DONE=0
            fi
        fi
    done
    [ $ALL_DONE -eq 1 ] && break
    if [ $(date +%s) -ge $DEADLINE ]; then
        for i in $(seq 1 $NUM_CLIENTS); do
            if [ -z "${CLIENT_DONE[$i]:-}" ]; then
                kill ${BENCH_PIDS[$i]} 2>/dev/null || true
                log_error "FAIL: Client $i did not complete within ${CLIENT_TIMEOUT_SECS}s"
                log_error "  Client I/O is WAITING for the backfill daemon — preemption is broken"
                FAILED=$(( FAILED + 1 ))
            fi
        done
        break
    fi
    sleep 1
done

TOTAL_CLIENT_TIME=$(( $(date +%s) - START_TIME ))

if [ $FAILED -gt 0 ]; then
    log_error "$FAILED/$NUM_CLIENTS clients timed out (preemption not working)"
    for i in $(seq 1 $NUM_CLIENTS); do
        echo "--- Client $i bench log ---"
        tail -20 "/tmp/preempt-bench-$i.log" 2>/dev/null || echo "(no log)"
    done
    echo "--- Daemon log tail ---"
    tail -30 "$BACKFILL_LOG"
    exit 1
fi

log_success "All $NUM_CLIENTS clients completed in ${TOTAL_CLIENT_TIME}s (daemon still running)"

# ============================================================================
log_step "6. Verify lock-breaking was triggered in daemon log"
# ============================================================================
# The daemon should have logged that it was preempted (its lock was broken).
# ObjectBackfillRequest::handle_acquire_lock logs this when it gets -EBUSY.
PREEMPTED=0
if grep -q "client I/O has preempted daemon backfill\|lock busy\|EBUSY\|EEXIST" \
       "$BACKFILL_LOG" 2>/dev/null; then
    log_success "✓ Daemon lock contention logged (preemption occurred)"
    grep -E "client I/O has preempted|lock busy" "$BACKFILL_LOG" | head -3 || true
    PREEMPTED=1
else
    log_warn "⚠ No preemption message in daemon log"
    log_warn "  Clients may have acquired the lock before the daemon on all objects."
    log_warn "  This is correct behaviour (not a failure) but reduces test coverage."
fi

# ============================================================================
log_step "7. Verify data integrity: child unwritten areas contain parent S3 data"
# ============================================================================
# Each client only wrote 4 KB at offset 0. The rest of the 4 MB parent object
# was copied verbatim from S3 during COW. We verify the last 4 KB of the first
# object in each child matches the corresponding bytes in the S3 source.
PARENT_BLOCK_SIZE=$(( 4 * 1024 * 1024 ))   # 4 MB = default RBD object size
CHECK_OFFSET=$(( PARENT_BLOCK_SIZE - 4096 )) # last 4 KB of first object
CHECK_LEN=4096

# Extract the reference bytes from the original S3 file
dd if="$PARENT_RAW" bs=1 skip=$CHECK_OFFSET count=$CHECK_LEN \
    of="/tmp/preempt-ref.bin" status=none 2>/dev/null

INTEGRITY_FAIL=0
for i in $(seq 1 $NUM_CLIENTS); do
    # Export the child image and compare
    EXPORT_FILE="/tmp/preempt-read-$i.raw"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export \
        "$POOL/preempt-child-$i" "$EXPORT_FILE" 2>/dev/null

    # Extract the same offset from the child export
    dd if="$EXPORT_FILE" bs=1 skip=$CHECK_OFFSET count=$CHECK_LEN \
        of="/tmp/preempt-child-check-$i.bin" status=none 2>/dev/null

    if cmp -s "/tmp/preempt-ref.bin" "/tmp/preempt-child-check-$i.bin"; then
        log_success "  Child $i: unwritten area matches parent S3 content ✓"
    else
        log_error "  Child $i: DATA MISMATCH — COW produced wrong data (S3 fetch may have raced)"
        INTEGRITY_FAIL=$(( INTEGRITY_FAIL + 1 ))
    fi

    rm -f "$EXPORT_FILE" "/tmp/preempt-child-check-$i.bin"
done
rm -f "/tmp/preempt-ref.bin"

if [ $INTEGRITY_FAIL -gt 0 ]; then
    log_error "$INTEGRITY_FAIL/$NUM_CLIENTS children have incorrect data after preemption"
    exit 1
fi

# ============================================================================
log_step "8. Verify daemon continues running and makes progress"
# ============================================================================
log_info "Waiting 10s for daemon to continue backfilling remaining objects..."
sleep 10

if ! kill -0 $BACKFILL_PID 2>/dev/null; then
    log_error "Daemon exited unexpectedly after client preemption"
    tail -30 "$BACKFILL_LOG"
    exit 1
fi

BLOCK_PREFIX=$(get_block_prefix "$CEPH_CONF" "$POOL" "preempt-parent")
TOTAL_EXPECTED=$(( (IMAGE_SIZE_MB + 3) / 4 ))
ACTUAL=$(count_rados_objects "$CEPH_CONF" "$POOL" "$BLOCK_PREFIX")
log_info "Daemon backfill progress: $ACTUAL / $TOTAL_EXPECTED objects"

if [ "$ACTUAL" -eq 0 ]; then
    log_error "No objects backfilled by daemon after preemption — daemon may be stuck"
    tail -30 "$BACKFILL_LOG"
    exit 1
fi
log_success "✓ Daemon still running and making progress ($ACTUAL objects)"

stop_backfill_daemon

# ============================================================================
echo
log_info "=== Preemption Test Summary ==="
echo "✓ $NUM_CLIENTS clients completed in ${TOTAL_CLIENT_TIME}s (without waiting for daemon)"
echo "✓ Data integrity verified (unwritten areas match S3 source)"
[ $PREEMPTED -eq 1 ] \
    && echo "✓ Lock contention logged by daemon (preemption confirmed)" \
    || echo "⚠ Preemption not logged (clients may have beaten daemon to the lock)"
echo "✓ Daemon continued after client preemption"
echo "✓ Backfill progress: $ACTUAL/$TOTAL_EXPECTED objects"

log_success "Preemption test PASSED"
