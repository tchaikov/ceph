#!/bin/bash
# Simplified E2E test for flatten with S3-backed parent
# Self-contained: starts its own Minio instance and Ceph cluster (or accepts --conf)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
MINIO_BIN="${HOME}/dev/minio/bin"
MINIO_PORT=9002
MINIO_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
MINIO_ACCESS_KEY="minioadmin"
MINIO_SECRET_KEY="minioadmin"
BUCKET="test-bucket"
S3_FILE="test.raw"
POOL="rbd"
PARENT="test-parent"
CHILD="test-child"

MINIO_PID=""
CEPH_CONF=""
MANAGED_CLUSTER=1
TEST_CLUSTER_DIR="/tmp/flatten-s3-simple-cluster"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

cleanup() {
    if [ -n "$CEPH_CONF" ]; then
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$CHILD"  2>/dev/null || true
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$PARENT" 2>/dev/null || true
    fi
    if [ "$MANAGED_CLUSTER" -eq 1 ] && [ -f "$TEST_CLUSTER_DIR/ceph.conf" ]; then
        "$BUILD_DIR/../src/stop.sh" 2>/dev/null || true
        rm -rf "$TEST_CLUSTER_DIR"
    fi
    if [ -n "$MINIO_PID" ]; then
        kill "$MINIO_PID" 2>/dev/null || true
        wait "$MINIO_PID" 2>/dev/null || true
    fi
    rm -rf /tmp/minio-flatten-simple
    rm -f "/tmp/$S3_FILE"
}
trap cleanup EXIT

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; MANAGED_CLUSTER=0; shift 2 ;;
        *) shift ;;
    esac
done

echo "=========================================="
echo "Simplified S3 Flatten Test"
echo "=========================================="
echo ""

# Start MinIO
mkdir -p /tmp/minio-flatten-simple
MINIO_ROOT_USER=$MINIO_ACCESS_KEY MINIO_ROOT_PASSWORD=$MINIO_SECRET_KEY \
    "$MINIO_BIN/minio" server /tmp/minio-flatten-simple \
    --address "127.0.0.1:${MINIO_PORT}" \
    --console-address "127.0.0.1:$((MINIO_PORT + 1))" \
    > /tmp/minio-flatten-simple.log 2>&1 &
MINIO_PID=$!

MINIO_READY=0
for attempt in $(seq 1 15); do
    sleep 1
    if "$MINIO_BIN/mc" alias set localminio "$MINIO_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" > /dev/null 2>&1 && \
       "$MINIO_BIN/mc" admin info localminio > /dev/null 2>&1; then
        MINIO_READY=1; break
    fi
done
if [ $MINIO_READY -eq 0 ] || ! kill -0 "$MINIO_PID" 2>/dev/null; then
    echo -e "${RED}MinIO failed to start${NC}"; cat /tmp/minio-flatten-simple.log; exit 1
fi
"$MINIO_BIN/mc" mb "localminio/$BUCKET" > /dev/null 2>&1 || true

# Start Ceph cluster if not provided
if [ "$MANAGED_CLUSTER" -eq 0 ]; then
    echo "Using external cluster: $CEPH_CONF"
else
    mkdir -p "$TEST_CLUSTER_DIR"
    cd "$BUILD_DIR"
    MDS=0 MGR=1 MON=1 OSD=3 RGW=0 \
        ../src/vstart.sh -n -d --without-dashboard \
        CEPH_DIR="$TEST_CLUSTER_DIR" > /tmp/flatten-s3-simple-vstart.log 2>&1
    CEPH_CONF="$TEST_CLUSTER_DIR/ceph.conf"
    for i in $(seq 1 30); do
        "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" health 2>/dev/null | grep -q "HEALTH_OK\|HEALTH_WARN" && break
        [ $i -eq 30 ] && { echo "Cluster failed to start"; exit 1; }
        sleep 1
    done
fi

"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool create "$POOL" 16 2>/dev/null || true
"$BUILD_DIR/bin/rbd"  --conf "$CEPH_CONF" pool init "$POOL" 2>/dev/null || true

# Test: Flatten with all data from S3
echo -e "${YELLOW}=== TEST: Flatten Clean Slate (All from S3) ===${NC}"

# Cleanup stale images
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$PARENT" 2>/dev/null || true
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$CHILD"  2>/dev/null || true

echo "1. Creating parent image (20MB)..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$POOL/$PARENT" --size 20M

echo "2. Configuring S3..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$POOL/$PARENT" \
    --s3-bucket     "$BUCKET" \
    --s3-endpoint   "$MINIO_ENDPOINT" \
    --s3-image-name "$S3_FILE" \
    --s3-access-key "$MINIO_ACCESS_KEY" \
    --s3-secret-key "$MINIO_SECRET_KEY"

echo "3. Uploading 20MB test data to S3..."
dd if=/dev/urandom of="/tmp/$S3_FILE" bs=1M count=20 status=none
"$MINIO_BIN/mc" cp "/tmp/$S3_FILE" "localminio/$BUCKET/$S3_FILE" > /dev/null

echo "4. Creating standalone clone..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone "$POOL/$PARENT" "$POOL/$CHILD"

echo ""
echo "5. Initial state (before flatten):"
PARENT_BEFORE=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" du "$POOL/$PARENT" --format=json | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['images'][0]['used_size'])")
CHILD_BEFORE=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" du "$POOL/$CHILD" --format=json | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['images'][0]['used_size'])")
echo "   Parent: $PARENT_BEFORE bytes (expected: 0)"
echo "   Child:  $CHILD_BEFORE bytes (expected: 0)"

echo ""
echo "6. Flattening child (should fetch all 5 objects from S3)..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" flatten "$POOL/$CHILD"

echo ""
echo "7. Final state (after flatten):"
CHILD_AFTER=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" du "$POOL/$CHILD" --format=json | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['images'][0]['used_size'])")
echo "   Child:  $CHILD_AFTER bytes (expected: 20971520)"

# Verify child has no parent reference
if "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$POOL/$CHILD" 2>/dev/null | grep -q "parent:"; then
    echo -e "${RED}✗ TEST FAILED: parent reference still present after flatten${NC}"
    exit 1
fi

if [ "$CHILD_AFTER" -eq 20971520 ]; then
    echo -e "${GREEN}✓ TEST PASSED${NC}"
    echo "  All 5 objects successfully fetched from S3 during flatten"
    echo "  Child correctly populated (20971520 bytes)"
else
    echo -e "${RED}✗ TEST FAILED${NC}"
    echo "  Expected child: 20971520 bytes, got: $CHILD_AFTER"
    exit 1
fi

echo ""
echo -e "${GREEN}=== TEST PASSED ===${NC}"
