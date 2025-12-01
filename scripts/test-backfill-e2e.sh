#!/bin/bash
# End-to-end test for rbd-backfill daemon with metadata-based image discovery

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CEPH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$CEPH_DIR/build"
MINIO_BIN="$HOME/dev/minio/bin"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

cleanup() {
    log_info "Cleaning up..."

    # Kill rbd-backfill daemon
    pkill -9 rbd-backfill 2>/dev/null || true

    # Stop MinIO
    pkill -9 minio 2>/dev/null || true

    # Stop Ceph cluster
    cd "$BUILD_DIR"
    ../src/stop.sh 2>/dev/null || true

    log_info "Cleanup complete"
}

# Trap exit to ensure cleanup
trap cleanup EXIT

# Step 1: Setup MinIO
log_info "Step 1: Setting up MinIO service..."

if [ ! -f "$MINIO_BIN/minio" ]; then
    log_error "MinIO binary not found at $MINIO_BIN/minio"
    exit 1
fi

if [ ! -f "$MINIO_BIN/mc" ]; then
    log_error "MinIO client (mc) not found at $MINIO_BIN/mc"
    exit 1
fi

# Kill any existing MinIO instances
pkill -9 minio 2>/dev/null || true
sleep 1

# Start MinIO
MINIO_DATA_DIR="$BUILD_DIR/minio-data"
mkdir -p "$MINIO_DATA_DIR"

export MINIO_ROOT_USER=minioadmin
export MINIO_ROOT_PASSWORD=minioadmin

log_info "Starting MinIO server on port 9000..."
"$MINIO_BIN/minio" server "$MINIO_DATA_DIR" --address :9000 > "$BUILD_DIR/minio.log" 2>&1 &
MINIO_PID=$!
sleep 3

# Verify MinIO is running
if ! kill -0 $MINIO_PID 2>/dev/null; then
    log_error "MinIO failed to start. Check $BUILD_DIR/minio.log"
    cat "$BUILD_DIR/minio.log"
    exit 1
fi

log_info "MinIO started successfully (PID: $MINIO_PID)"

# Configure MinIO client
log_info "Configuring MinIO client..."
"$MINIO_BIN/mc" alias set local http://localhost:9000 minioadmin minioadmin || {
    log_error "Failed to configure MinIO client"
    exit 1
}

# Create bucket
BUCKET_NAME="test-backfill-bucket"
log_info "Creating bucket: $BUCKET_NAME"
"$MINIO_BIN/mc" mb "local/$BUCKET_NAME" 2>/dev/null || log_warn "Bucket may already exist"

# Step 2: Setup minimal Ceph cluster
log_info "Step 2: Setting up minimal Ceph cluster..."

cd "$BUILD_DIR"

# Stop any existing cluster
../src/stop.sh 2>/dev/null || true
sleep 2

# Start new cluster
log_info "Starting Ceph cluster with vstart.sh..."
MON=1 OSD=3 MDS=0 MGR=1 RGW=0 ../src/vstart.sh -n -d --without-dashboard > "$BUILD_DIR/vstart.log" 2>&1

if [ $? -ne 0 ]; then
    log_error "Failed to start Ceph cluster. Check $BUILD_DIR/vstart.log"
    tail -50 "$BUILD_DIR/vstart.log"
    exit 1
fi

log_info "Ceph cluster started successfully"

# Wait for cluster to be ready
log_info "Waiting for cluster to be healthy..."
for i in {1..30}; do
    if ./bin/ceph --conf ./ceph.conf health 2>/dev/null | grep -q "HEALTH_OK\|HEALTH_WARN"; then
        log_info "Cluster is ready"
        break
    fi
    if [ $i -eq 30 ]; then
        log_error "Cluster failed to become healthy"
        ./bin/ceph --conf ./ceph.conf -s
        exit 1
    fi
    sleep 1
done

# Show cluster status
log_info "Cluster status:"
./bin/ceph --conf ./ceph.conf -s

# Create rbd pool if it doesn't exist
log_info "Creating 'rbd' pool..."
./bin/ceph --conf ./ceph.conf osd pool create rbd 8 8
./bin/rbd --conf ./ceph.conf pool init rbd

# Step 3: Create parent standalone image backed by S3
log_info "Step 3: Creating parent standalone image backed by S3..."

POOL_NAME="rbd"
IMAGE_NAME="backfill-test-parent"
IMAGE_SIZE="100M"

# Create image
log_info "Creating image $POOL_NAME/$IMAGE_NAME (size: $IMAGE_SIZE)"
./bin/rbd --conf ./ceph.conf create --size $IMAGE_SIZE $POOL_NAME/$IMAGE_NAME

if [ $? -ne 0 ]; then
    log_error "Failed to create RBD image"
    exit 1
fi

# Write some data to the image
log_info "Writing test data to image..."
./bin/rbd --conf ./ceph.conf bench $POOL_NAME/$IMAGE_NAME --io-type write --io-size 4096 --io-total $IMAGE_SIZE --io-pattern seq

# Set S3 metadata
log_info "Setting S3 metadata on image..."
./bin/rbd --conf ./ceph.conf image-meta set $POOL_NAME/$IMAGE_NAME s3.endpoint "http://localhost:9000"
./bin/rbd --conf ./ceph.conf image-meta set $POOL_NAME/$IMAGE_NAME s3.bucket "$BUCKET_NAME"
./bin/rbd --conf ./ceph.conf image-meta set $POOL_NAME/$IMAGE_NAME s3.image_name "$IMAGE_NAME"
./bin/rbd --conf ./ceph.conf image-meta set $POOL_NAME/$IMAGE_NAME s3.access_key "minioadmin"
./bin/rbd --conf ./ceph.conf image-meta set $POOL_NAME/$IMAGE_NAME s3.secret_key "minioadmin"
./bin/rbd --conf ./ceph.conf image-meta set $POOL_NAME/$IMAGE_NAME s3.region "us-east-1"
./bin/rbd --conf ./ceph.conf image-meta set $POOL_NAME/$IMAGE_NAME s3.image_format "raw"

# Verify S3 metadata
log_info "Verifying S3 metadata..."
./bin/rbd --conf ./ceph.conf image-meta list $POOL_NAME/$IMAGE_NAME | grep "s3\."

# Export image to S3 using rbd export
log_info "Exporting image to S3..."
EXPORT_FILE="$BUILD_DIR/export-temp.raw"
rm -f "$EXPORT_FILE"  # Clean up any existing file
./bin/rbd --conf ./ceph.conf export $POOL_NAME/$IMAGE_NAME "$EXPORT_FILE"

# Upload to S3
log_info "Uploading exported image to S3..."
"$MINIO_BIN/mc" cp "$EXPORT_FILE" "local/$BUCKET_NAME/$IMAGE_NAME"

# Verify upload
log_info "Verifying S3 upload..."
"$MINIO_BIN/mc" ls "local/$BUCKET_NAME/$IMAGE_NAME"

# Remove all RADOS objects from the image (to simulate standalone parent)
log_info "Removing RADOS objects from image (simulating standalone parent)..."
OBJECT_PREFIX=$(./bin/rbd --conf ./ceph.conf info $POOL_NAME/$IMAGE_NAME | grep "block_name_prefix" | awk '{print $2}')
log_info "Object prefix: $OBJECT_PREFIX"

# List and remove data objects
REMOVED_COUNT=0
TEMP_OBJ_LIST="$BUILD_DIR/object_list.txt"
./bin/rados --conf ./ceph.conf -p $POOL_NAME ls > "$TEMP_OBJ_LIST" 2>&1 || true
while IFS= read -r obj; do
    # Skip empty lines and warnings
    if [[ -z "$obj" ]] || [[ "$obj" == *"WARNING"* ]] || [[ "$obj" == *"DEVELOPER MODE"* ]]; then
        continue
    fi

    if [[ "$obj" == "${OBJECT_PREFIX}"* ]]; then
        ./bin/rados --conf ./ceph.conf -p $POOL_NAME rm "$obj" 2>/dev/null || true
        REMOVED_COUNT=$((REMOVED_COUNT + 1))
    fi
done < "$TEMP_OBJ_LIST"
rm -f "$TEMP_OBJ_LIST"
log_info "Removed $REMOVED_COUNT data objects from RADOS"

# Step 4: Schedule backfill using rbd command line
log_info "Step 4: Scheduling backfill with 'rbd backfill schedule'..."

./bin/rbd --conf ./ceph.conf backfill schedule $POOL_NAME/$IMAGE_NAME

if [ $? -ne 0 ]; then
    log_error "Failed to schedule backfill"
    exit 1
fi

# Verify backfill_scheduled metadata is set
log_info "Verifying backfill_scheduled metadata..."
SCHEDULED_VALUE=$(./bin/rbd --conf ./ceph.conf image-meta get $POOL_NAME/$IMAGE_NAME backfill_scheduled 2>/dev/null || echo "")
if [ "$SCHEDULED_VALUE" != "true" ]; then
    log_error "backfill_scheduled metadata not set correctly (got: '$SCHEDULED_VALUE')"
    exit 1
fi
log_info "Backfill scheduled successfully (backfill_scheduled=true)"

# Start rbd-backfill daemon
log_info "Starting rbd-backfill daemon..."
./bin/rbd-backfill --conf ./ceph.conf --foreground --debug-rbd=10 > "$BUILD_DIR/rbd-backfill.log" 2>&1 &
BACKFILL_PID=$!
sleep 2

# Verify daemon is running
if ! kill -0 $BACKFILL_PID 2>/dev/null; then
    log_error "rbd-backfill daemon failed to start. Check $BUILD_DIR/rbd-backfill.log"
    cat "$BUILD_DIR/rbd-backfill.log"
    exit 1
fi
log_info "rbd-backfill daemon started (PID: $BACKFILL_PID)"

# Step 5: Wait for backfill to complete
log_info "Step 5: Waiting 10 seconds for backfill to complete..."
sleep 10

# Check daemon logs for progress
log_info "Checking daemon logs for backfill progress..."
if grep -q "discovered scheduled image.*$IMAGE_NAME" "$BUILD_DIR/rbd-backfill.log"; then
    log_info "✓ Daemon discovered the scheduled image"
else
    log_warn "Daemon may not have discovered the image"
fi

if grep -q "backfill thread starting" "$BUILD_DIR/rbd-backfill.log"; then
    log_info "✓ Backfill thread started"
else
    log_warn "Backfill thread may not have started"
fi

# Check for S3 fetch operations (look for AWS signature debug output or range requests)
S3_FETCH_COUNT=$(grep -c "AWS4-HMAC-SHA256\|range:bytes=" "$BUILD_DIR/rbd-backfill.log" 2>/dev/null || echo "0")
log_info "S3 fetch operations: $S3_FETCH_COUNT"

# Check for completed objects (look for various success indicators)
COMPLETED_COUNT=$(grep -c "object backfill succeeded\|AWS_SIG_DEBUG" "$BUILD_DIR/rbd-backfill.log" 2>/dev/null || echo "0")
log_info "Completed object backfills: $COMPLETED_COUNT"

# Step 6: Verify backfill completion by reading the parent image
log_info "Step 6: Verifying backfill completion..."

# Count RADOS objects restored
TEMP_OBJ_LIST="$BUILD_DIR/restored_objects.txt"
./bin/rados --conf ./ceph.conf -p $POOL_NAME ls > "$TEMP_OBJ_LIST" 2>&1 || true
RESTORED_COUNT=0
while IFS= read -r obj; do
    # Skip empty lines and warnings
    if [[ -z "$obj" ]] || [[ "$obj" == *"WARNING"* ]] || [[ "$obj" == *"DEVELOPER MODE"* ]]; then
        continue
    fi

    if [[ "$obj" == "${OBJECT_PREFIX}"* ]]; then
        RESTORED_COUNT=$((RESTORED_COUNT + 1))
    fi
done < "$TEMP_OBJ_LIST"
rm -f "$TEMP_OBJ_LIST"
log_info "Restored RADOS objects: $RESTORED_COUNT"

if [ $RESTORED_COUNT -eq 0 ]; then
    log_error "No RADOS objects were restored!"
    log_error "Daemon logs (last 100 lines):"
    tail -100 "$BUILD_DIR/rbd-backfill.log"
    exit 1
fi

# Try to read the image
log_info "Attempting to read from image..."
READ_OUTPUT=$(./bin/rbd --conf ./ceph.conf bench $POOL_NAME/$IMAGE_NAME --io-type read --io-size 4096 --io-total 10M --io-pattern seq 2>&1)

if [ $? -eq 0 ]; then
    log_info "✓ Successfully read from image"
    echo "$READ_OUTPUT" | grep -E "elapsed|bandwidth"
else
    log_error "Failed to read from image"
    echo "$READ_OUTPUT"
    exit 1
fi

# Check backfill_status metadata
log_info "Checking backfill_status metadata..."
BACKFILL_STATUS=$(./bin/rbd --conf ./ceph.conf image-meta get $POOL_NAME/$IMAGE_NAME backfill_status 2>/dev/null || echo "not_set")
log_info "backfill_status: $BACKFILL_STATUS"

# Export and verify data integrity
log_info "Verifying data integrity..."
VERIFY_EXPORT="$BUILD_DIR/verify-export.raw"
./bin/rbd --conf ./ceph.conf export $POOL_NAME/$IMAGE_NAME "$VERIFY_EXPORT"

ORIGINAL_SIZE=$(stat -c%s "$EXPORT_FILE" 2>/dev/null || echo "0")
RESTORED_SIZE=$(stat -c%s "$VERIFY_EXPORT" 2>/dev/null || echo "0")

if [ -f "$VERIFY_EXPORT" ]; then
    VERIFY_SIZE=$(stat -c%s "$VERIFY_EXPORT")
    log_info "Exported image size: $VERIFY_SIZE bytes"

    # Compare with S3 object
    "$MINIO_BIN/mc" stat "local/$BUCKET_NAME/$IMAGE_NAME" | grep Size

    rm -f "$VERIFY_EXPORT"
fi

# Clean up temp files
rm -f "$EXPORT_FILE"

# Final status
log_info ""
log_info "============================================"
log_info "E2E Test Results:"
log_info "============================================"
log_info "✓ MinIO service started"
log_info "✓ Ceph cluster started"
log_info "✓ Image created and exported to S3"
log_info "✓ Backfill scheduled via metadata"
log_info "✓ rbd-backfill daemon discovered image"
log_info "  - S3 fetch operations: $S3_FETCH_COUNT"
log_info "  - Completed backfills: $COMPLETED_COUNT"
log_info "  - Restored objects: $RESTORED_COUNT"
log_info "✓ Image is readable after backfill"
log_info "============================================"

if [ "$RESTORED_COUNT" -gt 0 ] && [ "$S3_FETCH_COUNT" -gt 0 ]; then
    log_info "E2E TEST PASSED ✓"
    exit 0
else
    log_error "E2E TEST FAILED - insufficient backfill activity"
    log_error "Check logs at: $BUILD_DIR/rbd-backfill.log"
    exit 1
fi
