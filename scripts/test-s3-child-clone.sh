#!/bin/bash
# Test reading from a child clone with S3-backed standalone parent
# Verifies that data flows correctly: child → parent → S3

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
MINIO_BIN="$HOME/dev/minio/bin"
MINIO_PORT=19200
MINIO_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
MINIO_ACCESS_KEY="minioadmin"
MINIO_SECRET_KEY="minioadmin"
S3_BUCKET="child-clone-test"

POOL="rbd"
PARENT_IMAGE="s3child-parent"
CHILD_IMAGE="s3child-child"

MINIO_PID=""
CEPH_CONF=""
MANAGED_CLUSTER=1
TEST_CLUSTER_DIR="/tmp/s3-child-clone-cluster"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
log_info()    { echo -e "${GREEN}[INFO]${NC}  $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
log_step()    { echo -e "${BLUE}[STEP]${NC}  $1"; }
log_success() { echo -e "${GREEN}[PASS]${NC}  $1"; }

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
    rm -rf /tmp/minio-s3-child-test
    rm -f /tmp/s3child-*.raw
}
trap cleanup EXIT

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; MANAGED_CLUSTER=0; shift 2 ;;
        *) shift ;;
    esac
done

echo
log_info "=== Test: S3-backed Child Clone Reads ==="
echo

# ============================================================================
log_step "1. Start Minio"
# ============================================================================
mkdir -p /tmp/minio-s3-child-test
MINIO_ROOT_USER=$MINIO_ACCESS_KEY MINIO_ROOT_PASSWORD=$MINIO_SECRET_KEY \
    "$MINIO_BIN/minio" server /tmp/minio-s3-child-test \
    --address "127.0.0.1:${MINIO_PORT}" \
    --console-address "127.0.0.1:$((MINIO_PORT+1))" \
    > /tmp/minio-s3-child.log 2>&1 &
MINIO_PID=$!

MINIO_READY=0
for attempt in $(seq 1 15); do
    sleep 1
    if "$MINIO_BIN/mc" alias set s3child-minio "$MINIO_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" > /dev/null 2>&1 && \
       "$MINIO_BIN/mc" admin info s3child-minio > /dev/null 2>&1; then
        MINIO_READY=1; break
    fi
done
if [ $MINIO_READY -eq 0 ] || ! kill -0 "$MINIO_PID" 2>/dev/null; then
    log_error "Minio failed to start"; cat /tmp/minio-s3-child.log; exit 1
fi
log_info "Minio ready (PID: $MINIO_PID, took ${attempt}s)"
"$MINIO_BIN/mc" mb "s3child-minio/$S3_BUCKET" > /dev/null 2>&1 || true

# ============================================================================
log_step "2. Create S3 parent image with known byte patterns"
# ============================================================================
# 4MB image: bytes 0-4095 = 0xCC (1 full page), bytes 4096-8191 = 0xDD, rest = 0x00
dd if=/dev/zero  of=/tmp/s3child-parent.raw bs=4M count=1 status=none
printf '\xcc%.0s' $(seq 1 4096) | dd of=/tmp/s3child-parent.raw bs=4096 count=1 conv=notrunc status=none
printf '\xdd%.0s' $(seq 1 4096) | dd of=/tmp/s3child-parent.raw bs=4096 count=1 seek=1 conv=notrunc status=none
"$MINIO_BIN/mc" cp /tmp/s3child-parent.raw "s3child-minio/$S3_BUCKET/parent-image" > /dev/null
log_info "Uploaded 4MB parent image (0xCC first 4K, 0xDD next 4K) to S3"

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
        CEPH_DIR="$TEST_CLUSTER_DIR" > /tmp/s3child-vstart.log 2>&1 || {
        log_error "Failed to start Ceph cluster"; tail -30 /tmp/s3child-vstart.log; exit 1
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
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create --size 4M "$POOL/$PARENT_IMAGE"

"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$POOL/$PARENT_IMAGE" \
    --s3-bucket     "$S3_BUCKET" \
    --s3-endpoint   "$MINIO_ENDPOINT" \
    --s3-image-name "parent-image" \
    --s3-access-key "$MINIO_ACCESS_KEY" \
    --s3-secret-key "$MINIO_SECRET_KEY"
log_info "Standalone parent created with S3 config"

"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone "$POOL/$PARENT_IMAGE" "$POOL/$CHILD_IMAGE"
log_info "Child clone created via clone-standalone"

# ============================================================================
log_step "5. Trigger a COW write on the child (forces read from parent → S3)"
# ============================================================================
# Write 1 byte to the child at offset 0. This triggers a COW: librbd reads the
# full parent object from S3, stores it in RADOS, then applies the write.
# After this, both the parent object (full 4MB) and the modified child object
# should be in RADOS.
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench "$POOL/$CHILD_IMAGE" \
    --io-type write --io-size 4096 --io-total 4096 --io-threads 1 > /dev/null 2>&1
log_info "COW write completed — parent object should now be cached in RADOS"

# ============================================================================
log_step "6. Export parent to verify S3 data was correctly fetched and cached"
# ============================================================================
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$POOL/$PARENT_IMAGE" /tmp/s3child-exported.raw 2>/dev/null
log_info "Exported parent image from RADOS"

# The parent should have the full 4MB cached from S3. Verify byte patterns.
PATTERN1=$(dd if=/tmp/s3child-exported.raw bs=4096 count=1 skip=0 status=none | od -An -tx1 | head -1 | tr -d ' ')
PATTERN2=$(dd if=/tmp/s3child-exported.raw bs=4096 count=1 skip=1 status=none | od -An -tx1 | head -1 | tr -d ' ')

log_info "Pattern at offset 0-4095 (expect 0xCC): ${PATTERN1:0:20}..."
log_info "Pattern at offset 4096-8191 (expect 0xDD): ${PATTERN2:0:20}..."

# ============================================================================
log_step "7. Verify RADOS objects created (COW occurred)"
# ============================================================================
BLOCK_PREFIX=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$POOL/$CHILD_IMAGE" 2>/dev/null \
    | awk '/block_name_prefix:/ {print $2}')
OBJ_COUNT=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls 2>/dev/null \
    | grep -c "^${BLOCK_PREFIX}\." || true)

FAILED=0
if [ "${OBJ_COUNT:-0}" -gt 0 ]; then
    log_success "Child has $OBJ_COUNT RADOS data object(s) (COW occurred)"
else
    log_error "Child has no RADOS objects — COW did not occur"
    FAILED=$((FAILED + 1))
fi

if [[ "$PATTERN1" =~ ^cc+.* ]]; then
    log_success "Parent page 0 (bytes 0-4095): CORRECT (0xCC from S3)"
else
    log_error "Parent page 0 WRONG: got '${PATTERN1:0:20}...', expected cccc..."
    FAILED=$((FAILED + 1))
fi

if [[ "$PATTERN2" =~ ^dd+.* ]]; then
    log_success "Parent page 1 (bytes 4096-8191): CORRECT (0xDD from S3)"
else
    log_error "Parent page 1 WRONG: got '${PATTERN2:0:20}...', expected dddd..."
    log_error "This may indicate the sparse-object bug: S3 fetch only cached the first page"
    FAILED=$((FAILED + 1))
fi

[ $FAILED -gt 0 ] && { log_error "TEST FAILED"; exit 1; }
log_success "S3-backed child clone reads test PASSED"
