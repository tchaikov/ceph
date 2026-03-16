#!/bin/bash
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }

# Directories
WORKSPACE="/home/kefu/dev/ceph-nautilus"
TEST_CLUSTER_DIR="$WORKSPACE/test-backfill-cluster"
BUILD_DIR="$WORKSPACE/build"
MINIO_BIN="$HOME/dev/minio/bin"

# MinIO configuration
MINIO_HOST="127.0.0.1"
MINIO_PORT="19000"  # Use different port to avoid conflicts
MINIO_ENDPOINT="http://${MINIO_HOST}:${MINIO_PORT}"
MINIO_ACCESS_KEY="minioadmin"
MINIO_SECRET_KEY="minioadmin"
S3_BUCKET="backfill-test"
S3_OBJECT_KEY="parent-image-raw"

# Test configuration
PARENT_IMAGE_NAME="backfill-parent"
PARENT_IMAGE_SIZE="40M"  # 40 MB = 10 objects of 4MB each
POOL_NAME="rbd"
OBJECT_SIZE=4194304  # 4MB RBD object size

# Ceph configuration (may be overridden by --conf)
CEPH_CONF=""
MANAGED_CLUSTER=1

# Process tracking
MINIO_PID=""
BACKFILL_PID=""

cleanup() {
    log_info "Cleaning up..."

    # Kill background processes
    if [ -n "$BACKFILL_PID" ]; then
        log_info "Stopping rbd-backfill daemon (PID: $BACKFILL_PID)..."
        kill $BACKFILL_PID 2>/dev/null || true
        wait $BACKFILL_PID 2>/dev/null || true
    fi

    if [ -n "$MINIO_PID" ]; then
        log_info "Stopping MinIO server (PID: $MINIO_PID)..."
        kill $MINIO_PID 2>/dev/null || true
        wait $MINIO_PID 2>/dev/null || true
    fi

    # Remove test RBD image (always, regardless of cluster ownership)
    if [ -n "$CEPH_CONF" ]; then
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL_NAME/$PARENT_IMAGE_NAME" 2>/dev/null || true
    fi

    # Stop cluster (only if we started it)
    if [ "$MANAGED_CLUSTER" -eq 1 ] && [ -f "$TEST_CLUSTER_DIR/ceph.conf" ]; then
        cd "$WORKSPACE"
        log_info "Stopping Ceph cluster..."
        "$BUILD_DIR/bin/stop.sh" "$TEST_CLUSTER_DIR" 2>/dev/null || true
        rm -rf "$TEST_CLUSTER_DIR"
    fi

    # Remove test directories
    rm -rf /tmp/minio-backfill-test

    log_info "Cleanup complete"
}

trap cleanup EXIT

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; MANAGED_CLUSTER=0; shift 2 ;;
        *) shift ;;
    esac
done

log_info "=== E2E Test: RBD Backfill Daemon with S3 Backend ==="
echo

# ============================================================================
log_step "1. Starting MinIO server..."
# ============================================================================

# Create MinIO data directory
mkdir -p /tmp/minio-backfill-test

# Start MinIO in background
log_info "Launching MinIO on port $MINIO_PORT..."
MINIO_ROOT_USER="$MINIO_ACCESS_KEY" MINIO_ROOT_PASSWORD="$MINIO_SECRET_KEY" \
    "$MINIO_BIN/minio" server /tmp/minio-backfill-test \
    --address "${MINIO_HOST}:${MINIO_PORT}" \
    --console-address "${MINIO_HOST}:19001" > /tmp/minio-backfill.log 2>&1 &
MINIO_PID=$!

log_info "MinIO PID: $MINIO_PID"

# Wait for MinIO to be fully ready (mc admin info confirms write-readiness,
# not just HTTP liveness; avoids "unformatted drive" write failures)
log_info "Waiting for MinIO to be ready..."
MINIO_READY=0
for attempt in $(seq 1 20); do
    sleep 1
    if "$MINIO_BIN/mc" alias set backfill-minio "$MINIO_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" > /dev/null 2>&1 && \
       "$MINIO_BIN/mc" admin info backfill-minio > /dev/null 2>&1; then
        MINIO_READY=1
        break
    fi
done
if [ $MINIO_READY -eq 0 ] || ! kill -0 $MINIO_PID 2>/dev/null; then
    log_error "MinIO failed to start or become ready"
    cat /tmp/minio-backfill.log
    exit 1
fi
log_success "MinIO is ready! (took ${attempt}s)"

# ============================================================================
log_step "2. Setting up MinIO S3 bucket..."
# ============================================================================

# Configure mc client
log_info "Configuring mc client..."
"$MINIO_BIN/mc" alias set backfill-minio "$MINIO_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" > /dev/null

# Create bucket
log_info "Creating S3 bucket: $S3_BUCKET"
"$MINIO_BIN/mc" mb "backfill-minio/$S3_BUCKET" 2>/dev/null || log_warn "Bucket may already exist"

# ============================================================================
log_step "3. Starting local Ceph cluster..."
# ============================================================================

if [ $MANAGED_CLUSTER -eq 0 ]; then
    log_info "Using external cluster: $CEPH_CONF"
else
cd "$WORKSPACE"

# Create cluster directory
rm -rf "$TEST_CLUSTER_DIR"
mkdir -p "$TEST_CLUSTER_DIR"

log_info "Initializing Ceph cluster in $TEST_CLUSTER_DIR..."

# Create minimal ceph.conf
cat > "$CEPH_CONF" << EOF
[global]
fsid = $(uuidgen)
mon initial members = a
mon host = 127.0.0.1:16789
auth cluster required = none
auth service required = none
auth client required = none
osd pool default size = 1
osd pool default min size = 1
osd crush chooseleaf type = 0
debug_ms = 0
log_to_stderr = true
err_to_stderr = true
admin socket = $TEST_CLUSTER_DIR/asok/\$name.asok
run dir = $TEST_CLUSTER_DIR/run

[mon]
mon allow pool delete = true
mon data = $TEST_CLUSTER_DIR/mon.\$id

[osd]
osd journal size = 100
osd data = $TEST_CLUSTER_DIR/dev/osd\$id
osd objectstore = filestore

[mgr]
mgr data = $TEST_CLUSTER_DIR/mgr.\$id
EOF

log_info "Starting Ceph services..."
cd "$WORKSPACE"

# Create mon, mgr, osd data directories
mkdir -p "$TEST_CLUSTER_DIR/mon.a"
mkdir -p "$TEST_CLUSTER_DIR/mgr.x"
mkdir -p "$TEST_CLUSTER_DIR/dev/osd0"
mkdir -p "$TEST_CLUSTER_DIR/asok"
mkdir -p "$TEST_CLUSTER_DIR/run"

# Create mon keyring
"$BUILD_DIR/bin/ceph-authtool" --create-keyring "$TEST_CLUSTER_DIR/keyring" \
    --gen-key -n client.admin --cap mon 'allow *' --cap osd 'allow *' --cap mds 'allow *' --cap mgr 'allow *'

"$BUILD_DIR/bin/ceph-authtool" --create-keyring "$TEST_CLUSTER_DIR/mon.keyring" \
    --gen-key -n mon. --cap mon 'allow *'

cat "$TEST_CLUSTER_DIR/keyring" >> "$TEST_CLUSTER_DIR/mon.keyring"

# Create monmap
"$BUILD_DIR/bin/monmaptool" --create --add a 127.0.0.1:16789 --fsid $(grep fsid "$CEPH_CONF" | awk '{print $3}') \
    "$TEST_CLUSTER_DIR/monmap"

# Initialize mon
"$BUILD_DIR/bin/ceph-mon" --mkfs -i a --monmap "$TEST_CLUSTER_DIR/monmap" \
    --keyring "$TEST_CLUSTER_DIR/mon.keyring" -c "$CEPH_CONF"

# Start mon
"$BUILD_DIR/bin/ceph-mon" -i a -c "$CEPH_CONF" --daemonize

# Wait for mon
sleep 3

# Create mgr keyring
"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" auth get-or-create mgr.x \
    mon 'allow profile mgr' osd 'allow *' mds 'allow *' > "$TEST_CLUSTER_DIR/mgr.keyring"

# Start mgr
"$BUILD_DIR/bin/ceph-mgr" -i x -c "$CEPH_CONF" --daemonize

# Create OSD
"$BUILD_DIR/bin/ceph-osd" -i 0 --mkfs --mkkey -c "$CEPH_CONF"

# Add OSD key
"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" auth add osd.0 osd 'allow *' mon 'allow profile osd' \
    -i "$TEST_CLUSTER_DIR/dev/osd0/keyring"

# Add OSD to crush map
"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd crush add 0 1.0 host=localhost

# Start OSD
"$BUILD_DIR/bin/ceph-osd" -i 0 -c "$CEPH_CONF" --daemonize

# Wait for cluster to be ready
log_info "Waiting for cluster to be ready..."
for i in {1..30}; do
    if "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" health | grep -q "HEALTH_OK\|HEALTH_WARN"; then
        log_success "Cluster is ready!"
        break
    fi
    if [ $i -eq 30 ]; then
        log_error "Cluster failed to start"
        "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" status
        exit 1
    fi
    sleep 1
done

"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" status
    CEPH_CONF="$CEPH_CONF"
fi  # end managed cluster setup

# ============================================================================
log_step "4. Creating test image with S3 parent configuration..."
# ============================================================================

# Create parent image
log_info "Creating parent image: $PARENT_IMAGE_NAME (size: $PARENT_IMAGE_SIZE)"
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$POOL_NAME/$PARENT_IMAGE_NAME" \
    --size "$PARENT_IMAGE_SIZE" --image-feature layering

# Configure S3 on the parent image (sets dotted keys + base64-encodes secret)
log_info "Setting S3 configuration..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$POOL_NAME/$PARENT_IMAGE_NAME" \
    --s3-endpoint  "$MINIO_ENDPOINT" \
    --s3-bucket    "$S3_BUCKET" \
    --s3-image-name "$S3_OBJECT_KEY" \
    --s3-access-key "$MINIO_ACCESS_KEY" \
    --s3-secret-key "$MINIO_SECRET_KEY"

# Schedule the image for backfill (sets backfill_scheduled=true metadata)
log_info "Scheduling backfill..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$POOL_NAME/$PARENT_IMAGE_NAME"

# Verify
log_info "Verifying S3 metadata and backfill schedule:"
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta list "$POOL_NAME/$PARENT_IMAGE_NAME"

# ============================================================================
log_step "5. Creating test data and uploading to S3..."
# ============================================================================

# Create test data file with recognizable pattern
log_info "Creating test data file..."
dd if=/dev/zero of=/tmp/test-parent-image bs=1M count=40 2>/dev/null
# Write a recognizable pattern at the beginning
echo "RBD_BACKFILL_TEST_DATA" | dd of=/tmp/test-parent-image conv=notrunc 2>/dev/null

# Upload to S3
log_info "Uploading test data to S3 (bucket: $S3_BUCKET, key: $S3_OBJECT_KEY)..."
"$MINIO_BIN/mc" cp /tmp/test-parent-image "backfill-minio/$S3_BUCKET/$S3_OBJECT_KEY"

# Verify upload
S3_SIZE=$("$MINIO_BIN/mc" stat "backfill-minio/$S3_BUCKET/$S3_OBJECT_KEY" | grep Size | awk '{print $3}')
log_success "S3 object uploaded successfully (size: $S3_SIZE bytes)"

# ============================================================================
log_step "6. Starting rbd-backfill daemon..."
# ============================================================================

log_info "Launching rbd-backfill daemon..."

# Run daemon in foreground mode with logs
"$BUILD_DIR/bin/rbd-backfill" \
    --conf "$CEPH_CONF" \
    --foreground > /tmp/rbd-backfill.log 2>&1 &

BACKFILL_PID=$!
log_info "rbd-backfill daemon PID: $BACKFILL_PID"

# Wait a bit for daemon to start
sleep 2

# Check if daemon is still running
if ! kill -0 $BACKFILL_PID 2>/dev/null; then
    log_error "rbd-backfill daemon failed to start!"
    cat /tmp/rbd-backfill.log
    exit 1
fi

log_success "rbd-backfill daemon started successfully"

# ============================================================================
log_step "7. Monitoring backfill progress..."
# ============================================================================

log_info "Watching daemon logs for 10 seconds..."
sleep 10

# Show daemon logs
log_info "Daemon logs:"
tail -50 /tmp/rbd-backfill.log

# ============================================================================
log_step "8. Verifying backfill results..."
# ============================================================================

# Check if any objects were created in the parent image
log_info "Checking RADOS objects for parent image..."

# Get image info to find pool ID
IMAGE_INFO=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$POOL_NAME/$PARENT_IMAGE_NAME" --format json)
log_info "Image info: $IMAGE_INFO"

# List objects in the pool
log_info "RADOS objects in pool '$POOL_NAME':"
"$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL_NAME" ls | grep -E "rbd_data|rbd_header" | head -20

# Count data objects
OBJECT_COUNT=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL_NAME" ls | grep "rbd_data" | wc -l)
log_info "Found $OBJECT_COUNT RBD data objects"


# ============================================================================
log_step "9. Regression: missing --conf must fail fast (not hang)"
# ============================================================================
# Verifies that omitting --conf causes a quick failure, not an indefinite hang
# waiting for unreachable monitors. (timeout 10 = 10-second deadline)

log_info "Running rbd-backfill WITHOUT --conf (expect failure within 10s)..."
set +e
timeout 10 "$BUILD_DIR/bin/rbd-backfill" --foreground 2>/tmp/rbd-backfill-noconf.log
NO_CONF_EXIT=$?
set -e

if [ $NO_CONF_EXIT -eq 124 ]; then
    log_error "FAIL: rbd-backfill hung >10s without --conf (Rados::connect blocked indefinitely)"
    exit 1
fi
log_success "PASS: rbd-backfill exited quickly without --conf (exit code: $NO_CONF_EXIT)"

# ============================================================================
log_step "10. Regression: --foreground must produce stderr output"
# ============================================================================
# Verifies that --foreground causes log output to appear on stderr.
# After the main.cc fix, log_to_stderr is set automatically when daemonize=false.

log_info "Running rbd-backfill --foreground and capturing stderr..."
set +e
"$BUILD_DIR/bin/rbd-backfill" \
    --conf "$CEPH_CONF" \
    --foreground 2>/tmp/rbd-backfill-stderr-test.log &
STDERR_TEST_PID=$!
sleep 3
kill $STDERR_TEST_PID 2>/dev/null
wait $STDERR_TEST_PID 2>/dev/null || true
set -e

STDERR_LINE_COUNT=$(wc -l < /tmp/rbd-backfill-stderr-test.log)
if [ "$STDERR_LINE_COUNT" -eq 0 ]; then
    log_error "FAIL: --foreground produced no stderr output — logs going to file instead"
    exit 1
fi
if ! grep -q "rbd-backfill starting" /tmp/rbd-backfill-stderr-test.log; then
    log_error "FAIL: startup message not found in stderr output"
    cat /tmp/rbd-backfill-stderr-test.log
    exit 1
fi
log_success "PASS: --foreground produced $STDERR_LINE_COUNT lines of stderr output"

# ============================================================================
log_step "11. Test Summary"
# ============================================================================

echo
log_info "=== Test Summary ==="
echo "MinIO Status: Running (PID: $MINIO_PID)"
echo "Backfill Daemon Status: Running (PID: $BACKFILL_PID)"
echo "S3 Object Size: $S3_SIZE bytes"
echo "RBD Data Objects: $OBJECT_COUNT"
echo
log_info "Test completed. Review logs at:"
echo "  - MinIO: /tmp/minio-backfill.log"
echo "  - Backfill: /tmp/rbd-backfill.log"
echo
log_info "Cluster config: $TEST_CLUSTER_DIR/ceph.conf"
echo

# Wait a bit before cleanup
log_info "Waiting 5 seconds before cleanup..."
sleep 5

log_success "E2E test completed successfully!"
