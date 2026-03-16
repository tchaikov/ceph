#!/bin/bash
# Quick S3-backed flatten test: standalone parent → child clone → flatten → verify data
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
MINIO_BIN="${HOME}/dev/minio/bin"
MINIO_PORT=9002
MINIO_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
MINIO_ACCESS_KEY="minioadmin"
MINIO_SECRET_KEY="minioadmin"
BUCKET="flatten-quick-test"
S3_OBJECT="parent.raw"
POOL="rbd"
PARENT_IMAGE="s3flatten-parent"
CHILD_IMAGE="s3flatten-child"

MINIO_PID=""
CEPH_CONF=""
MANAGED_CLUSTER=1
TEST_CLUSTER_DIR="/tmp/s3-flatten-quick-cluster"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_success() { echo -e "${GREEN}[PASS]${NC} $1"; }

cleanup() {
    log_info "Cleaning up..."
    if [ -n "$CEPH_CONF" ]; then
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$CHILD_IMAGE"  2>/dev/null || true
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$PARENT_IMAGE" 2>/dev/null || true
    fi
    if [ "$MANAGED_CLUSTER" -eq 1 ] && [ -f "$TEST_CLUSTER_DIR/ceph.conf" ]; then
        "$BUILD_DIR/../src/stop.sh" 2>/dev/null || true
        rm -rf "$TEST_CLUSTER_DIR"
    fi
    if [ -n "$MINIO_PID" ]; then
        kill "$MINIO_PID" 2>/dev/null || true
        wait "$MINIO_PID" 2>/dev/null || true
    fi
    rm -rf /tmp/minio-flatten-quick
    rm -f /tmp/s3flatten-parent.raw /tmp/s3flatten-child-export.raw
}
trap cleanup EXIT

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; MANAGED_CLUSTER=0; shift 2 ;;
        *) shift ;;
    esac
done

log_info "=== Quick S3-Backed Flatten Test ==="
echo

# ============================================================================
log_step "1. Start MinIO"
# ============================================================================
mkdir -p /tmp/minio-flatten-quick
MINIO_ROOT_USER=$MINIO_ACCESS_KEY MINIO_ROOT_PASSWORD=$MINIO_SECRET_KEY \
    "$MINIO_BIN/minio" server /tmp/minio-flatten-quick \
    --address "127.0.0.1:${MINIO_PORT}" \
    --console-address "127.0.0.1:$((MINIO_PORT + 1))" \
    > /tmp/minio-flatten-quick.log 2>&1 &
MINIO_PID=$!

MINIO_READY=0
for attempt in $(seq 1 15); do
    sleep 1
    if "$MINIO_BIN/mc" alias set flatten-quick "$MINIO_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" > /dev/null 2>&1 && \
       "$MINIO_BIN/mc" admin info flatten-quick > /dev/null 2>&1; then
        MINIO_READY=1; break
    fi
done
if [ $MINIO_READY -eq 0 ] || ! kill -0 "$MINIO_PID" 2>/dev/null; then
    log_error "MinIO failed to start"; cat /tmp/minio-flatten-quick.log; exit 1
fi
log_info "MinIO ready (PID: $MINIO_PID, took ${attempt}s)"
"$MINIO_BIN/mc" mb "flatten-quick/$BUCKET" > /dev/null 2>&1 || true

# ============================================================================
log_step "2. Create parent image data and upload to S3"
# ============================================================================
# 20MB image: 5 objects × 4MB, each with a distinct marker at the start
dd if=/dev/zero of=/tmp/s3flatten-parent.raw bs=1M count=20 status=none
for i in {0..4}; do
    printf "BLOCK-%d-DATA" "$i" | dd of=/tmp/s3flatten-parent.raw bs=4M seek="$i" conv=notrunc status=none
done
"$MINIO_BIN/mc" cp /tmp/s3flatten-parent.raw "flatten-quick/$BUCKET/$S3_OBJECT" > /dev/null
log_info "Parent image uploaded to S3"

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
        CEPH_DIR="$TEST_CLUSTER_DIR" > /tmp/s3flatten-vstart.log 2>&1 || {
        log_error "Failed to start Ceph cluster"; tail -30 /tmp/s3flatten-vstart.log; exit 1
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
log_step "4. Create S3-backed standalone parent + child clone"
# ============================================================================
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create --size 20M "$POOL/$PARENT_IMAGE"

"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$POOL/$PARENT_IMAGE" \
    --s3-endpoint   "$MINIO_ENDPOINT" \
    --s3-bucket     "$BUCKET" \
    --s3-image-name "$S3_OBJECT" \
    --s3-access-key "$MINIO_ACCESS_KEY" \
    --s3-secret-key "$MINIO_SECRET_KEY"
log_info "S3-backed parent created"

"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone "$POOL/$PARENT_IMAGE" "$POOL/$CHILD_IMAGE"
log_info "Standalone child clone created"

# ============================================================================
log_step "5. Flatten child image (reads all objects from S3 via parent)"
# ============================================================================
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" flatten "$POOL/$CHILD_IMAGE" 2>&1 | grep -v "^$" || true
log_info "Flatten completed"

# ============================================================================
log_step "6. Verify parent reference removed"
# ============================================================================
if "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$POOL/$CHILD_IMAGE" | grep -q "parent:"; then
    log_error "Parent reference still exists after flatten!"
    exit 1
fi
log_success "Parent reference removed after flatten"

# ============================================================================
log_step "7. Verify data integrity"
# ============================================================================
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$POOL/$CHILD_IMAGE" /tmp/s3flatten-child-export.raw 2>/dev/null
if cmp /tmp/s3flatten-parent.raw /tmp/s3flatten-child-export.raw; then
    log_success "Data integrity verified — flattened child matches original S3 parent"
else
    log_error "Data mismatch between original S3 parent and flattened child"
    exit 1
fi

log_success "S3-Backed Flatten Test PASSED"
