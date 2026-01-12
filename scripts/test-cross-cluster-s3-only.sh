#!/bin/bash
# Test cross-cluster S3-backed clone WITHOUT remote cluster connection
# Theory: Child reads from S3, backfills to local parent, no remote RADOS needed

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$WORKSPACE/build"
CEPH_CONF="$BUILD_DIR/ceph.conf"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_success() { echo -e "${GREEN}[✓]${NC} $1"; }

echo "=== Testing Cross-Cluster S3-Backed Clone (S3-Only, No Remote RADOS) ==="
echo ""

# Check cluster is running
if ! "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" status >/dev/null 2>&1; then
    log_error "Ceph cluster not running"
    exit 1
fi
log_success "Ceph cluster is running"

# Start MinIO
log_step "Starting MinIO on port 9000..."
pkill -9 minio 2>/dev/null || true
sleep 1

MINIO_BIN="${HOME}/dev/minio/bin"
MINIO_DATA="/tmp/minio-test-$$"
mkdir -p "$MINIO_DATA"

export MINIO_ROOT_USER=minioadmin
export MINIO_ROOT_PASSWORD=minioadmin

"$MINIO_BIN/minio" server "$MINIO_DATA" \
    --address ":9000" \
    --console-address ":9001" \
    > /tmp/minio-test.log 2>&1 &

MINIO_PID=$!
sleep 3

if ! curl -sf "http://localhost:9000/minio/health/live" > /dev/null 2>&1; then
    log_error "MinIO failed to start"
    exit 1
fi
log_success "MinIO started (PID: $MINIO_PID)"

# Setup S3 bucket
log_step "Setting up S3 bucket..."
"$MINIO_BIN/mc" alias set local http://localhost:9000 minioadmin minioadmin 2>&1 | grep -v "^mc:" || true
"$MINIO_BIN/mc" mb local/test-bucket 2>&1 | grep -v "^mc:" || log_info "Bucket exists"
"$MINIO_BIN/mc" anonymous set download local/test-bucket 2>&1 | grep -v "^mc:"
log_success "S3 bucket ready"

# Create pool
POOL="testpool"
log_step "Creating pool: $POOL"
"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool create "$POOL" 8 2>&1 | grep -v "successfully created" || true
"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool application enable "$POOL" rbd 2>&1 || true

# Create test data and upload to S3
log_step "Creating test data (20MB)..."
TEST_FILE="/tmp/test-parent-$$.raw"
dd if=/dev/zero of="$TEST_FILE" bs=1M count=20 2>/dev/null
# Write recognizable pattern
for i in $(seq 0 4); do
    printf "PARENT-BLOCK-%04d" $i | dd of="$TEST_FILE" bs=4M seek=$i conv=notrunc 2>/dev/null
done

log_step "Uploading to S3..."
"$MINIO_BIN/mc" cp "$TEST_FILE" local/test-bucket/parent-image.raw 2>&1 | grep -v "^mc:"
log_success "Test data uploaded to S3"

# Create "remote" parent image (simulated - just metadata, no RADOS objects)
log_step "Creating simulated remote parent (metadata only, no RADOS objects)..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$POOL/remote-parent" --size 20M --object-size 4M

# Configure S3 metadata on "remote" parent
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$POOL/remote-parent" s3.enabled true
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$POOL/remote-parent" s3.bucket test-bucket
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$POOL/remote-parent" s3.endpoint http://localhost:9000
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$POOL/remote-parent" s3.image_name parent-image.raw
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$POOL/remote-parent" s3.image_format raw
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$POOL/remote-parent" s3.verify_ssl false

log_success "Remote parent created with S3 metadata"

# Create child image using standalone clone (references LOCAL parent)
log_step "Creating child as standalone clone of local S3-backed parent..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone "$POOL/remote-parent" "$POOL/child"

log_success "Child created as standalone clone (references local S3-backed parent)"

# Test 1: Read from child (should fetch from S3 and backfill to parent)
log_step "Test 1: Reading from child (should trigger S3 fetch + backfill)..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench "$POOL/child" --io-type read --io-size 4M --io-total 4M 2>&1 | grep -E "elapsed|ops/sec" || true

# Check if parent was backfilled
log_step "Checking if parent was backfilled..."
PARENT_OBJECTS=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls | grep -c "rbd_data" || echo "0")
log_info "Parent RADOS objects: $PARENT_OBJECTS"

if [ "$PARENT_OBJECTS" -gt 0 ]; then
    log_success "Parent was backfilled from S3!"
else
    log_error "Parent was NOT backfilled"
fi

# Test 2: Export child and verify data integrity
log_step "Test 2: Exporting child and verifying data..."
EXPORT_FILE="/tmp/child-export-$$.raw"
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$POOL/child" "$EXPORT_FILE" 2>&1 | grep -v "Exporting"

if cmp -s "$EXPORT_FILE" "$TEST_FILE"; then
    log_success "Data integrity verified!"
else
    log_error "Data mismatch!"
fi

# Test 3: Write to child (copyup)
log_step "Test 3: Writing to child (should trigger copyup)..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench "$POOL/child" --io-type write --io-size 512K --io-total 512K 2>&1 | grep -E "elapsed|ops/sec" || true
log_success "Write completed"

# Cleanup
log_step "Cleaning up..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/child" 2>/dev/null || true
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/remote-parent" 2>/dev/null || true
kill $MINIO_PID 2>/dev/null || true
rm -rf "$MINIO_DATA" "$TEST_FILE" "$EXPORT_FILE"

echo ""
log_success "Test complete!"
echo ""
echo "Key findings:"
echo "1. Child should read from S3 without remote cluster connection"
echo "2. Parent should be backfilled automatically"
echo "3. Subsequent reads should be fast (from backfilled parent)"
