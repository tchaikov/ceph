#!/bin/bash
# Test to verify that rbd-backfill creates objects with correct hex-formatted names
# This validates the critical bug fix for object naming (decimal vs hex)

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

cleanup() {
    log_info "Cleaning up..."

    # Kill backfill daemon
    if [ ! -z "$DAEMON_PID" ] && kill -0 $DAEMON_PID 2>/dev/null; then
        log_info "Stopping daemon (PID: $DAEMON_PID)"
        kill -TERM $DAEMON_PID
        wait $DAEMON_PID 2>/dev/null || true
    fi

    # Stop MinIO
    if [ ! -z "$MINIO_PID" ] && kill -0 $MINIO_PID 2>/dev/null; then
        log_info "Stopping MinIO (PID: $MINIO_PID)"
        kill -TERM $MINIO_PID
        wait $MINIO_PID 2>/dev/null || true
    fi

    # Stop vstart cluster
    if [ -d build/out ]; then
        cd build
        ../src/stop.sh 2>/dev/null || true
        cd ..
    fi

    log_success "Cleanup complete"
}

trap cleanup EXIT

# Test configuration
IMAGE_SIZE=$((20 * 1024 * 1024))  # 20MB (5 objects at 4MB each)
MINIO_PORT=29200
BUCKET="object-name-test"
S3_OBJECT="parent-raw"

log_info "=== Object Naming Test for RBD Backfill Daemon ==="
echo

# ============================================================================
log_step "1. Setup: Start MinIO and Ceph cluster"
# ============================================================================

# Start MinIO
log_info "Starting MinIO server..."
mkdir -p /tmp/minio-data-objname
$HOME/dev/minio/bin/minio server /tmp/minio-data-objname \
    --address "127.0.0.1:${MINIO_PORT}" \
    --console-address "127.0.0.1:$((MINIO_PORT + 1))" \
    > /tmp/minio-objname.log 2>&1 &
MINIO_PID=$!

sleep 2
if ! kill -0 $MINIO_PID 2>/dev/null; then
    log_error "MinIO failed to start"
    cat /tmp/minio-objname.log
    exit 1
fi

# Configure mc client
export MINIO_ROOT_USER=minioadmin
export MINIO_ROOT_PASSWORD=minioadmin
$HOME/dev/minio/bin/mc alias set objname-test http://127.0.0.1:${MINIO_PORT} \
    ${MINIO_ROOT_USER} ${MINIO_ROOT_PASSWORD} > /dev/null 2>&1

log_success "MinIO ready"

# Create bucket
$HOME/dev/minio/bin/mc mb objname-test/${BUCKET} 2>/dev/null || true
log_success "Bucket created: ${BUCKET}"

# Start Ceph cluster
log_info "Starting Ceph cluster with vstart..."
cd build
../src/stop.sh 2>/dev/null || true
rm -rf out dev
MON=1 OSD=1 MDS=0 MGR=0 RGW=0 ../src/vstart.sh -n -x \
    --memstore > /tmp/vstart-objname.log 2>&1
cd ..

sleep 3
if ! build/bin/ceph -s --conf build/ceph.conf >/dev/null 2>&1; then
    log_error "Cluster failed to start"
    tail -50 /tmp/vstart-objname.log
    exit 1
fi

log_success "Cluster started successfully"

# Create RBD pool
log_info "Creating rbd pool..."
build/bin/ceph --conf build/ceph.conf osd pool create rbd 8 >/dev/null 2>&1
build/bin/rbd --conf build/ceph.conf pool init rbd >/dev/null 2>&1
log_success "RBD pool created"

# ============================================================================
log_step "2. Setup: Create parent image and upload to S3"
# ============================================================================

# Create parent data
dd if=/dev/urandom of=/tmp/parent-data-objname bs=1M count=20 >/dev/null 2>&1

# Upload to S3
$HOME/dev/minio/bin/mc cp /tmp/parent-data-objname objname-test/${BUCKET}/${S3_OBJECT} \
    > /dev/null 2>&1
log_success "Uploaded 20MB parent image to S3"

# Create parent image in RBD
build/bin/rbd --conf build/ceph.conf create rbd/naming-parent \
    --size ${IMAGE_SIZE} --image-feature layering,exclusive-lock >/dev/null 2>&1

# Configure S3 metadata
build/bin/rbd --conf build/ceph.conf image-meta set rbd/naming-parent \
    s3.endpoint "http://127.0.0.1:${MINIO_PORT}" >/dev/null 2>&1
build/bin/rbd --conf build/ceph.conf image-meta set rbd/naming-parent \
    s3.bucket "${BUCKET}" >/dev/null 2>&1
build/bin/rbd --conf build/ceph.conf image-meta set rbd/naming-parent \
    s3.image_name "${S3_OBJECT}" >/dev/null 2>&1
build/bin/rbd --conf build/ceph.conf image-meta set rbd/naming-parent \
    s3.region "us-east-1" >/dev/null 2>&1
build/bin/rbd --conf build/ceph.conf image-meta set rbd/naming-parent \
    s3.access_key "${MINIO_ROOT_USER}" >/dev/null 2>&1
build/bin/rbd --conf build/ceph.conf image-meta set rbd/naming-parent \
    s3.secret_key "${MINIO_ROOT_PASSWORD}" >/dev/null 2>&1
build/bin/rbd --conf build/ceph.conf image-meta set rbd/naming-parent \
    s3.image_format "raw" >/dev/null 2>&1

log_success "Parent image configured with S3 backend"

# Get the block name prefix for this image
BLOCK_PREFIX=$(build/bin/rbd --conf build/ceph.conf info rbd/naming-parent | \
    grep block_name_prefix | awk '{print $2}')
log_info "Block name prefix: ${BLOCK_PREFIX}"

# ============================================================================
log_step "3. Run backfill daemon and wait for completion"
# ============================================================================

log_info "Starting backfill daemon..."
build/bin/rbd-backfill --conf build/ceph.conf \
    --rbd_backfill_max_concurrent=2 \
    --pool rbd --image naming-parent > /tmp/backfill-objname.log 2>&1 &
DAEMON_PID=$!

log_info "Daemon started (PID: $DAEMON_PID)"
sleep 2

if ! kill -0 $DAEMON_PID 2>/dev/null; then
    log_error "Daemon crashed immediately"
    cat /tmp/backfill-objname.log
    exit 1
fi

log_success "Daemon is running"

# Wait for backfill to complete (should be fast with only 20MB)
log_info "Waiting for backfill to complete..."
sleep 10

# ============================================================================
log_step "4. Verify object naming format"
# ============================================================================

log_info "Listing objects in RADOS pool..."
RADOS_OBJECTS=$(build/bin/rados --conf build/ceph.conf -p rbd ls | grep "rbd_data" | sort)

if [ -z "$RADOS_OBJECTS" ]; then
    log_error "No rbd_data objects found!"
    echo "All pool objects:"
    build/bin/rados --conf build/ceph.conf -p rbd ls
    exit 1
fi

log_success "Found backfilled objects:"
echo "$RADOS_OBJECTS"
echo

# ============================================================================
log_step "5. Validate hex formatting"
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
log_step "6. Verify object count"
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
log_step "7. Verify specific object names"
# ============================================================================

log_info "Checking for expected object names..."

# Check for object 0 (should be .0000000000000000 in hex format 2 or .000000000000 in format 1)
EXPECTED_OBJ_0="${BLOCK_PREFIX}.0000000000000000"
EXPECTED_OBJ_0_ALT="${BLOCK_PREFIX}.000000000000"

if echo "$RADOS_OBJECTS" | grep -q "rbd_data.${EXPECTED_OBJ_0}"; then
    log_success "✓ Found object 0 with format 2: rbd_data.${EXPECTED_OBJ_0}"
elif echo "$RADOS_OBJECTS" | grep -q "rbd_data.${EXPECTED_OBJ_0_ALT}"; then
    log_success "✓ Found object 0 with format 1: rbd_data.${EXPECTED_OBJ_0_ALT}"
else
    log_error "✗ Object 0 not found with expected hex format"
    log_error "  Expected: rbd_data.${EXPECTED_OBJ_0} or rbd_data.${EXPECTED_OBJ_0_ALT}"
    PASS=false
fi

# Check for object 1 (should be .0000000000000001 or .000000000001)
EXPECTED_OBJ_1="${BLOCK_PREFIX}.0000000000000001"
EXPECTED_OBJ_1_ALT="${BLOCK_PREFIX}.000000000001"

if echo "$RADOS_OBJECTS" | grep -q "rbd_data.${EXPECTED_OBJ_1}"; then
    log_success "✓ Found object 1 with format 2: rbd_data.${EXPECTED_OBJ_1}"
elif echo "$RADOS_OBJECTS" | grep -q "rbd_data.${EXPECTED_OBJ_1_ALT}"; then
    log_success "✓ Found object 1 with format 1: rbd_data.${EXPECTED_OBJ_1_ALT}"
else
    log_error "✗ Object 1 not found with expected hex format"
    log_error "  Expected: rbd_data.${EXPECTED_OBJ_1} or rbd_data.${EXPECTED_OBJ_1_ALT}"
    PASS=false
fi

# ============================================================================
log_step "8. Verify no decimal-formatted objects"
# ============================================================================

log_info "Checking for incorrect decimal-formatted objects..."

# Look for objects ending in single digits (0-9) without leading zeros
# This would indicate the bug (e.g., rbd_data.xxx.0 instead of rbd_data.xxx.0000000000000000)
DECIMAL_OBJECTS=$(echo "$RADOS_OBJECTS" | grep -E '\.[0-9]$')

if [ -z "$DECIMAL_OBJECTS" ]; then
    log_success "✓ No decimal-formatted objects found (bug is fixed!)"
else
    log_error "✗ Found decimal-formatted objects (bug still present):"
    echo "$DECIMAL_OBJECTS"
    PASS=false
fi

# ============================================================================
log_step "9. Test Summary"
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
    echo "��� One or more validation checks failed"
    echo
    log_error "OBJECT NAMING TEST FAILED - Please review errors above"
    exit 1
fi
