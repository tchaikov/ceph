#!/bin/bash
# Test that overwrites to existing child objects do NOT populate parent cache
# This verifies the fix for the overwrite bug

set -e

CONF_PATH="${CONF_PATH:-/home/kefu/dev/ceph-nautilus/build/ceph.conf}"
POOL="rbd"
PARENT_IMAGE="s3-parent-overwrite-test"
CHILD_IMAGE="child-overwrite-test"
MINIO_BIN="${HOME}/dev/minio/bin"

echo "=== Test: Overwrites Should NOT Populate Parent Cache ===="
echo "This test verifies that:"
echo "  1. First write (full objects) creates child objects, parent cache empty"
echo "  2. Second write (partial overwrite) does NOT populate parent cache"
echo ""

# Clean up any existing images
echo "Cleaning up any existing test images..."
rbd --conf "$CONF_PATH" rm "$POOL/$PARENT_IMAGE" 2>/dev/null || true
rbd --conf "$CONF_PATH" rm "$POOL/$CHILD_IMAGE" 2>/dev/null || true

# Create parent image with S3 backend
echo "Creating parent image: $PARENT_IMAGE (100MB)..."
rbd --conf "$CONF_PATH" create "$POOL/$PARENT_IMAGE" --size 100M

# Set S3 metadata on parent
echo "Setting S3 metadata on parent image..."
rbd --conf "$CONF_PATH" s3-config set "$POOL/$PARENT_IMAGE" \
    --s3-bucket test-bucket \
    --s3-endpoint http://localhost:9000 \
    --s3-region us-east-1 \
    --s3-image-name parent-overwrite-test.raw \
    --s3-image-format raw \
    --s3-access-key minioadmin \
    --s3-secret-key minioadmin

# Prepare S3 data (create a 100MB raw image file)
echo "Preparing S3 test data..."
TEST_DATA_FILE="/tmp/parent-overwrite-test-data.raw"
dd if=/dev/urandom of="$TEST_DATA_FILE" bs=1M count=100 2>/dev/null

# Upload to MinIO
echo "Uploading test data to MinIO..."
"${MINIO_BIN}/mc" alias set localminio http://localhost:9000 minioadmin minioadmin 2>/dev/null || true
"${MINIO_BIN}/mc" mb localminio/test-bucket 2>/dev/null || true
"${MINIO_BIN}/mc" cp "$TEST_DATA_FILE" localminio/test-bucket/parent-overwrite-test.raw

# Create standalone clone
echo ""
echo "Creating standalone clone: $CHILD_IMAGE..."
rbd --conf "$CONF_PATH" clone-standalone "$POOL/$PARENT_IMAGE" "$POOL/$CHILD_IMAGE"

# First write: 4M full objects (should create child objects, parent stays empty)
echo ""
echo "=== FIRST WRITE: 4M x 2 = 8MB (full objects) ==="
rbd --conf "$CONF_PATH" bench "$POOL/$CHILD_IMAGE" --io-type write --io-size 4M --io-total 8M --io-threads 1

# Check parent and child sizes after first write
echo ""
echo "After first write (4M full objects):"
PARENT_USED_1=$(rbd --conf "$CONF_PATH" du "$POOL/$PARENT_IMAGE" --format=json | jq '.images[0].used_size')
CHILD_USED_1=$(rbd --conf "$CONF_PATH" du "$POOL/$CHILD_IMAGE" --format=json | jq '.images[0].used_size')
echo "  Parent USED: $PARENT_USED_1 bytes (should be 0)"
echo "  Child  USED: $CHILD_USED_1 bytes (should be 8MB)"

# Second write: 1M partial overwrites (should NOT populate parent cache)
echo ""
echo "=== SECOND WRITE: 1M x 8 = 8MB (partial overwrites to same location) ==="
rbd --conf "$CONF_PATH" bench "$POOL/$CHILD_IMAGE" --io-type write --io-size 1M --io-total 8M --io-threads 1

# Wait a bit for any background operations
echo "Waiting for any background operations..."
sleep 3

# Check parent and child sizes after second write
echo ""
echo "After second write (1M partial overwrites):"
PARENT_USED_2=$(rbd --conf "$CONF_PATH" du "$POOL/$PARENT_IMAGE" --format=json | jq '.images[0].used_size')
CHILD_USED_2=$(rbd --conf "$CONF_PATH" du "$POOL/$CHILD_IMAGE" --format=json | jq '.images[0].used_size')
echo "  Parent USED: $PARENT_USED_2 bytes (should STILL be 0!)"
echo "  Child  USED: $CHILD_USED_2 bytes (should still be 8MB)"

# Test result
echo ""
TEST_RESULT="PASS"

if [ "$PARENT_USED_1" -ne 0 ]; then
    echo "✗ FAILURE: Parent cache was populated after first write (should be 0)"
    echo "  Expected: 0 bytes"
    echo "  Got:      $PARENT_USED_1 bytes"
    TEST_RESULT="FAIL"
fi

if [ "$PARENT_USED_2" -ne 0 ]; then
    echo "✗ FAILURE: Parent cache was populated after overwrite (BUG!)"
    echo "  Expected: 0 bytes"
    echo "  Got:      $PARENT_USED_2 bytes"
    TEST_RESULT="FAIL"
fi

if [ "$TEST_RESULT" = "PASS" ]; then
    echo "✓ SUCCESS: Parent cache was NOT populated during overwrites"
    echo "  First write:  Parent = $PARENT_USED_1 bytes ✓"
    echo "  Overwrite:    Parent = $PARENT_USED_2 bytes ✓"
fi

# Clean up
echo ""
echo "Cleaning up..."
rbd --conf "$CONF_PATH" rm "$POOL/$CHILD_IMAGE"
rbd --conf "$CONF_PATH" rm "$POOL/$PARENT_IMAGE"
"${MINIO_BIN}/mc" rm localminio/test-bucket/parent-overwrite-test.raw
rm -f "$TEST_DATA_FILE"

echo ""
echo "=== Test Result: $TEST_RESULT ==="
if [ "$TEST_RESULT" = "PASS" ]; then
    echo "The fix is working correctly:"
    echo "  - Overwrites do NOT populate parent cache"
    echo "  - Parent cache only populated on real cache misses"
    exit 0
else
    echo "Test failed - overwrites are still populating parent cache"
    exit 1
fi
