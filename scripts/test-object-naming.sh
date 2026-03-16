#!/bin/bash
# Test to verify that rbd-backfill creates objects with correct hex-formatted names
# This validates the critical bug fix for object naming (decimal vs hex)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
MINIO_BIN="$HOME/dev/minio/bin"
MINIO_PORT=29200
MINIO_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
MINIO_ACCESS_KEY="minioadmin"
MINIO_SECRET_KEY="minioadmin"
BUCKET="object-name-test"
S3_OBJECT="parent-raw"
POOL="rbd"
PARENT_IMAGE="naming-parent"

MINIO_PID=""
DAEMON_PID=""
CEPH_CONF=""
MANAGED_CLUSTER=1
TEST_CLUSTER_DIR="/tmp/objname-test-cluster"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${GREEN}[INFO]${NC}    $1"; }
log_step()    { echo -e "${BLUE}[STEP]${NC}    $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC}   $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }

cleanup() {
    log_info "Cleaning up..."

    # Kill backfill daemon
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        log_info "Stopping daemon (PID: $DAEMON_PID)"
        kill -TERM "$DAEMON_PID"
        wait "$DAEMON_PID" 2>/dev/null || true
    fi

    # Remove test images
    if [ -n "$CEPH_CONF" ]; then
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$PARENT_IMAGE" 2>/dev/null || true
    fi

    # Stop managed cluster
    if [ "$MANAGED_CLUSTER" -eq 1 ] && [ -f "$TEST_CLUSTER_DIR/ceph.conf" ]; then
        "$BUILD_DIR/../src/stop.sh" 2>/dev/null || true
        rm -rf "$TEST_CLUSTER_DIR"
    fi

    # Stop MinIO
    if [ -n "$MINIO_PID" ] && kill -0 "$MINIO_PID" 2>/dev/null; then
        kill "$MINIO_PID" 2>/dev/null || true
        wait "$MINIO_PID" 2>/dev/null || true
    fi

    rm -rf /tmp/minio-data-objname
    rm -f /tmp/parent-data-objname
}

trap cleanup EXIT

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; MANAGED_CLUSTER=0; shift 2 ;;
        *) shift ;;
    esac
done

log_info "=== Object Naming Test for RBD Backfill Daemon ==="
echo

# ============================================================================
log_step "1. Start MinIO"
# ============================================================================
mkdir -p /tmp/minio-data-objname
MINIO_ROOT_USER=$MINIO_ACCESS_KEY MINIO_ROOT_PASSWORD=$MINIO_SECRET_KEY \
    "$MINIO_BIN/minio" server /tmp/minio-data-objname \
    --address "127.0.0.1:${MINIO_PORT}" \
    --console-address "127.0.0.1:$((MINIO_PORT + 1))" \
    > /tmp/minio-objname.log 2>&1 &
MINIO_PID=$!

MINIO_READY=0
for attempt in $(seq 1 15); do
    sleep 1
    if "$MINIO_BIN/mc" alias set objname-test "$MINIO_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" > /dev/null 2>&1 && \
       "$MINIO_BIN/mc" admin info objname-test > /dev/null 2>&1; then
        MINIO_READY=1; break
    fi
done
if [ $MINIO_READY -eq 0 ] || ! kill -0 "$MINIO_PID" 2>/dev/null; then
    log_error "MinIO failed to start"; cat /tmp/minio-objname.log; exit 1
fi
log_info "MinIO ready (PID: $MINIO_PID, took ${attempt}s)"
"$MINIO_BIN/mc" mb "objname-test/$BUCKET" > /dev/null 2>&1 || true

# ============================================================================
log_step "2. Create parent image data and upload to S3"
# ============================================================================
dd if=/dev/urandom of=/tmp/parent-data-objname bs=1M count=20 status=none
"$MINIO_BIN/mc" cp /tmp/parent-data-objname "objname-test/$BUCKET/$S3_OBJECT" > /dev/null
log_info "Uploaded 20MB parent image to S3"

# ============================================================================
log_step "3. Start Ceph cluster (if not provided)"
# ============================================================================
if [ "$MANAGED_CLUSTER" -eq 0 ]; then
    log_info "Using external cluster: $CEPH_CONF"
else
    log_info "Starting managed test cluster..."
    mkdir -p "$TEST_CLUSTER_DIR"
    cd "$BUILD_DIR"
    MDS=0 MGR=1 MON=1 OSD=3 RGW=0 \
        ../src/vstart.sh -n -d --without-dashboard \
        CEPH_DIR="$TEST_CLUSTER_DIR" > /tmp/objname-vstart.log 2>&1 || {
        log_error "Failed to start Ceph cluster"; tail -30 /tmp/objname-vstart.log; exit 1
    }
    CEPH_CONF="$TEST_CLUSTER_DIR/ceph.conf"
    for i in $(seq 1 30); do
        if "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" health 2>/dev/null | grep -q "HEALTH_OK\|HEALTH_WARN"; then
            break
        fi
        [ $i -eq 30 ] && { log_error "Cluster failed to become healthy"; exit 1; }
        sleep 1
    done
    log_info "Cluster started. Config: $CEPH_CONF"
fi

"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool create "$POOL" 16 2>/dev/null || true
"$BUILD_DIR/bin/rbd"  --conf "$CEPH_CONF" pool init "$POOL" 2>/dev/null || true

# ============================================================================
log_step "4. Create S3-backed standalone parent image"
# ============================================================================
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create --size 20M "$POOL/$PARENT_IMAGE"

"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$POOL/$PARENT_IMAGE" \
    --s3-endpoint   "$MINIO_ENDPOINT" \
    --s3-bucket     "$BUCKET" \
    --s3-image-name "$S3_OBJECT" \
    --s3-access-key "$MINIO_ACCESS_KEY" \
    --s3-secret-key "$MINIO_SECRET_KEY"
log_info "Parent image configured with S3 backend"

# Schedule image for backfill (daemon only processes scheduled images)
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$POOL/$PARENT_IMAGE"
log_info "Image scheduled for backfill"

# Get the block name prefix for this image
BLOCK_PREFIX=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$POOL/$PARENT_IMAGE" | \
    awk '/block_name_prefix:/ {print $2}')
log_info "Block name prefix: ${BLOCK_PREFIX}"

# ============================================================================
log_step "5. Run backfill daemon and wait for completion"
# ============================================================================

log_info "Starting backfill daemon..."
"$BUILD_DIR/bin/rbd-backfill" --conf "$CEPH_CONF" \
    --foreground > /tmp/backfill-objname.log 2>&1 &
DAEMON_PID=$!

log_info "Daemon started (PID: $DAEMON_PID)"
sleep 2

if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    log_error "Daemon crashed immediately"
    cat /tmp/backfill-objname.log
    exit 1
fi

log_success "Daemon is running"

# Wait for backfill to complete (20MB / 4MB objects = 5 objects; should be fast)
log_info "Waiting for backfill to complete (up to 30s)..."
for i in $(seq 1 30); do
    sleep 1
    OBJ_COUNT=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls 2>/dev/null \
        | grep -c "^${BLOCK_PREFIX}\." || true)
    if [ "${OBJ_COUNT:-0}" -ge 5 ]; then
        log_info "All 5 objects backfilled after ${i}s"
        break
    fi
    [ $i -eq 30 ] && log_error "Backfill did not complete within 30s (got ${OBJ_COUNT:-0} objects)"
done

# Stop daemon gracefully
kill -TERM "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""

# ============================================================================
log_step "6. Verify object naming format"
# ============================================================================

log_info "Listing objects in RADOS pool..."
RADOS_OBJECTS=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls | grep "^${BLOCK_PREFIX}\." | sort)

if [ -z "$RADOS_OBJECTS" ]; then
    log_error "No rbd_data objects found!"
    echo "All pool objects:"
    "$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls
    exit 1
fi

log_success "Found backfilled objects:"
echo "$RADOS_OBJECTS"
echo

# ============================================================================
log_step "7. Validate hex formatting"
# ============================================================================

log_info "Validating object name format..."

# Expected format: rbd_data.<prefix>.0000000000000000 (16-digit hex for format 2)
# or: rbd_data.<prefix>.000000000000 (12-digit hex for format 1)

PASS=true
OBJECT_COUNT=0

while IFS= read -r obj; do
    OBJECT_COUNT=$((OBJECT_COUNT + 1))

    # Extract the object number portion (everything after the last dot)
    OBJ_NUM=$(echo "$obj" | awk -F'.' '{print $NF}')

    # Check if it's hex formatted with zero-padding
    if [[ "$OBJ_NUM" =~ ^[0-9a-f]{12,16}$ ]]; then
        log_success "✓ $obj - VALID (hex-formatted: $OBJ_NUM)"
    else
        log_error "✗ $obj - INVALID (expected hex, got: $OBJ_NUM)"
        PASS=false
    fi
done <<< "$RADOS_OBJECTS"

echo

# ============================================================================
log_step "8. Verify object count"
# ============================================================================

# 20MB with 4MB objects = 5 objects expected
EXPECTED_COUNT=5

log_info "Expected objects: $EXPECTED_COUNT"
log_info "Found objects: $OBJECT_COUNT"

if [ "$OBJECT_COUNT" -eq "$EXPECTED_COUNT" ]; then
    log_success "✓ Object count matches expected"
else
    log_error "✗ Object count mismatch (expected $EXPECTED_COUNT, got $OBJECT_COUNT)"
    PASS=false
fi

# ============================================================================
log_step "9. Verify specific object names"
# ============================================================================

log_info "Checking for expected object names..."

# Check for object 0 (should be .0000000000000000 in hex format 2 or .000000000000 in format 1)
EXPECTED_OBJ_0="${BLOCK_PREFIX}.0000000000000000"
EXPECTED_OBJ_0_ALT="${BLOCK_PREFIX}.000000000000"

if echo "$RADOS_OBJECTS" | grep -q "${EXPECTED_OBJ_0}$"; then
    log_success "✓ Found object 0 with format 2: ${EXPECTED_OBJ_0}"
elif echo "$RADOS_OBJECTS" | grep -q "${EXPECTED_OBJ_0_ALT}$"; then
    log_success "✓ Found object 0 with format 1: ${EXPECTED_OBJ_0_ALT}"
else
    log_error "✗ Object 0 not found with expected hex format"
    log_error "  Expected: ${EXPECTED_OBJ_0} or ${EXPECTED_OBJ_0_ALT}"
    PASS=false
fi

# Check for object 1 (should be .0000000000000001 or .000000000001)
EXPECTED_OBJ_1="${BLOCK_PREFIX}.0000000000000001"
EXPECTED_OBJ_1_ALT="${BLOCK_PREFIX}.000000000001"

if echo "$RADOS_OBJECTS" | grep -q "${EXPECTED_OBJ_1}$"; then
    log_success "✓ Found object 1 with format 2: ${EXPECTED_OBJ_1}"
elif echo "$RADOS_OBJECTS" | grep -q "${EXPECTED_OBJ_1_ALT}$"; then
    log_success "✓ Found object 1 with format 1: ${EXPECTED_OBJ_1_ALT}"
else
    log_error "✗ Object 1 not found with expected hex format"
    log_error "  Expected: ${EXPECTED_OBJ_1} or ${EXPECTED_OBJ_1_ALT}"
    PASS=false
fi

# ============================================================================
log_step "10. Verify no decimal-formatted objects"
# ============================================================================

log_info "Checking for incorrect decimal-formatted objects..."

# Look for objects ending in single digits (0-9) without leading zeros
# This would indicate the bug (e.g., rbd_data.xxx.0 instead of rbd_data.xxx.0000000000000000)
DECIMAL_OBJECTS=$(echo "$RADOS_OBJECTS" | grep -E '\.[0-9]$' || true)

if [ -z "$DECIMAL_OBJECTS" ]; then
    log_success "✓ No decimal-formatted objects found (bug is fixed!)"
else
    log_error "✗ Found decimal-formatted objects (bug still present):"
    echo "$DECIMAL_OBJECTS"
    PASS=false
fi

# ============================================================================
log_step "11. Test Summary"
# ============================================================================

echo
log_info "=== Test Results ==="

if [ "$PASS" = true ]; then
    echo "✓ All objects use correct hex formatting"
    echo "✓ Object count matches expected ($OBJECT_COUNT/$EXPECTED_COUNT)"
    echo "✓ No decimal-formatted objects found"
    echo "✓ Specific object names validated"
    echo
    log_success "OBJECT NAMING TEST PASSED - Critical bug fix verified!"
    exit 0
else
    echo "✗ One or more validation checks failed"
    echo
    log_error "OBJECT NAMING TEST FAILED - Please review errors above"
    exit 1
fi
