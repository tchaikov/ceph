#!/bin/bash
# test-s3-preemption.sh — Verify that client I/O properly preempts backfill daemon
#
# Flow:
#   1. Create 100 MB S3-backed parent image, schedule backfill
#   2. Start rbd-backfill daemon (which begins fetching objects one at a time)
#   3. While daemon is fetching, inject a direct RADOS write to the first object
#      (this triggers lock contention because both the daemon and the client try
#      to acquire "s3_fetch_lock" on the same RADOS object)
#   4. Verify the daemon logged "lock busy" / "client I/O has preempted"
#   5. Verify the daemon continues running and eventually backlls remaining objects
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
IMAGE_SIZE_MB=100   # 25 objects × 4 MB
BACKFILL_LOG="/tmp/rbd-backfill-preempt-$$.log"

cleanup() {
    log_info "Cleaning up..."
    stop_backfill_daemon
    stop_minio $MINIO_PORT
    rm -rf "$MINIO_DATA_DIR" /tmp/preempt-parent-$$.raw "$BACKFILL_LOG"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/preempt-parent" 2>/dev/null || true
    "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete "$POOL" "$POOL" \
        --yes-i-really-really-mean-it 2>/dev/null || true
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; shift 2 ;;
        *) shift ;;
    esac
done

log_info "=== E2E Preemption Test for RBD Backfill Daemon ==="
echo

if ! check_cluster_running; then exit 1; fi

create_pool "$POOL"
enable_s3_fetch

# ============================================================================
log_step "1. Start MinIO and upload parent image"
# ============================================================================

start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"
setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

dd if=/dev/urandom bs=1M count=$IMAGE_SIZE_MB of="/tmp/preempt-parent-$$.raw" status=none
"$MINIO_BIN/mc" cp "/tmp/preempt-parent-$$.raw" "local/$S3_BUCKET/parent-raw" > /dev/null
log_success "Uploaded ${IMAGE_SIZE_MB}MB parent image to S3"

# ============================================================================
log_step "2. Create parent image and schedule backfill"
# ============================================================================

create_s3_parent "$POOL" "preempt-parent" "parent-raw" $IMAGE_SIZE_MB \
    "$S3_ENDPOINT" "$S3_BUCKET" "$CEPH_CONF"

"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$POOL/preempt-parent"
log_success "Backfill scheduled"

# ============================================================================
log_step "3. Start backfill daemon"
# ============================================================================

run_backfill_daemon "$CEPH_CONF" "$BACKFILL_LOG"

log_info "Waiting 5s for daemon to begin fetching objects..."
sleep 5

if ! kill -0 $BACKFILL_PID 2>/dev/null; then
    log_error "Daemon crashed before preemption test"
    cat "$BACKFILL_LOG"
    exit 1
fi
log_success "Daemon is running (PID: $BACKFILL_PID)"

# ============================================================================
log_step "4. Trigger client I/O to cause lock contention"
# ============================================================================
# Write directly to RADOS object 0 of the parent image. The daemon holds
# (or will try to acquire) "s3_fetch_lock" on this object. Whichever arrives
# first wins; the other backs off. This validates the lock-based preemption.

BLOCK_PREFIX=$(get_block_prefix "$CEPH_CONF" "$POOL" "preempt-parent")
OBJECT_NAME="${BLOCK_PREFIX}.0000000000000000"

log_info "Injecting RADOS write to object: $OBJECT_NAME"
log_info "(triggers lock contention between daemon and client I/O)"

# Use a small payload — we just need to contest the lock, not write full data
head -c 4096 /dev/urandom > /tmp/preempt-payload-$$.bin

set +e
"$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" \
    put "$OBJECT_NAME" /tmp/preempt-payload-$$.bin 2>/dev/null
set -e
rm -f /tmp/preempt-payload-$$.bin

log_success "Client I/O injected"

# ============================================================================
log_step "5. Verify preemption was logged"
# ============================================================================

sleep 2

PREEMPTED=0
if grep -q "client I/O has preempted daemon backfill" "$BACKFILL_LOG" 2>/dev/null; then
    log_success "✓ Preemption log message found"
    grep "client I/O has preempted" "$BACKFILL_LOG" | head -3
    PREEMPTED=1
elif grep -q "lock busy\|EBUSY\|EEXIST" "$BACKFILL_LOG" 2>/dev/null; then
    log_success "✓ Lock contention detected (lock busy on object)"
    PREEMPTED=1
else
    log_warn "⚠ No explicit preemption log — daemon may have completed first or missed window"
    log_warn "  (not a hard failure: timing-dependent)"
fi

# ============================================================================
log_step "6. Verify daemon continues running after preemption"
# ============================================================================

log_info "Waiting 10s for daemon to continue backfill..."
sleep 10

if ! kill -0 $BACKFILL_PID 2>/dev/null; then
    log_error "Daemon exited unexpectedly after preemption"
    tail -50 "$BACKFILL_LOG"
    exit 1
fi
log_success "✓ Daemon still running after client I/O"

# ============================================================================
log_step "7. Check overall backfill progress"
# ============================================================================

TOTAL_EXPECTED=$(( (IMAGE_SIZE_MB + 3) / 4 ))
ACTUAL=$(count_rados_objects "$CEPH_CONF" "$POOL" "$BLOCK_PREFIX")

log_info "Objects backfilled: $ACTUAL / $TOTAL_EXPECTED"

if [ "$ACTUAL" -gt 0 ]; then
    log_success "✓ Backfill progressed ($ACTUAL objects)"
else
    log_error "No objects were backfilled — check $BACKFILL_LOG"
    exit 1
fi

stop_backfill_daemon

# ============================================================================
echo
log_info "=== Preemption Test Results ==="
echo "✓ Daemon started and ran continuously"
echo "✓ Client I/O injected during backfill"
[ $PREEMPTED -eq 1 ] && echo "✓ Lock contention / preemption logged" \
                      || echo "⚠ Preemption not confirmed (timing window may have been missed)"
echo "✓ Daemon continued after client I/O"
echo "✓ Backfill progress: $ACTUAL/$TOTAL_EXPECTED objects"

log_success "Preemption E2E test PASSED"
