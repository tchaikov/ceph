#!/bin/bash
# Test that S3 metadata is not inherited by child clones
# This test verifies the fix for the QEMU "Permission denied" issue

set -e

CONF_PATH="${CONF_PATH:-/home/kefu/dev/ceph-nautilus/ceph.conf}"
POOL="rbd"
PARENT_IMAGE="parent-with-s3"
CHILD_IMAGE="child-clone"

echo "=== Test: S3 Metadata Inheritance Prevention ==="
echo "This test verifies that child clones do not inherit S3 metadata from parent images"
echo ""

# Clean up any existing images
echo "Cleaning up any existing test images..."
rbd --conf "$CONF_PATH" rm "$POOL/$PARENT_IMAGE" 2>/dev/null || true
rbd --conf "$CONF_PATH" rm "$POOL/$CHILD_IMAGE" 2>/dev/null || true

# Create parent image
echo "Creating parent image: $PARENT_IMAGE (1GB)..."
rbd --conf "$CONF_PATH" create "$POOL/$PARENT_IMAGE" --size 1G

# Set S3 metadata on parent image
echo "Setting S3 metadata on parent image..."
rbd --conf "$CONF_PATH" s3-config set "$POOL/$PARENT_IMAGE" \
    --s3-bucket test-bucket \
    --s3-endpoint http://localhost:9000 \
    --s3-region us-east-1 \
    --s3-image-name parent.raw \
    --s3-image-format raw \
    --s3-access-key test-access-key \
    --s3-secret-key test-secret-key

# Verify S3 metadata is set on parent
echo ""
echo "Verifying S3 metadata on parent image:"
rbd --conf "$CONF_PATH" s3-config get "$POOL/$PARENT_IMAGE"

# Create standalone clone from parent
echo ""
echo "Creating standalone clone: $CHILD_IMAGE..."
rbd --conf "$CONF_PATH" clone-standalone "$POOL/$PARENT_IMAGE" "$POOL/$CHILD_IMAGE"

# Check if child has S3 metadata (it should NOT have it)
echo ""
echo "Checking S3 metadata on child image (should not be present):"
if rbd --conf "$CONF_PATH" s3-config get "$POOL/$CHILD_IMAGE" 2>&1 | grep -q "S3 configuration is not set"; then
    echo "✓ SUCCESS: Child image does NOT have S3 metadata (as expected)"
    TEST_RESULT="PASS"
else
    echo "✗ FAILURE: Child image has S3 metadata (unexpected - it should NOT inherit it)"
    TEST_RESULT="FAIL"
fi

# Also check individual metadata keys to be thorough
echo ""
echo "Checking individual S3 metadata keys on child image:"
S3_KEYS=("s3.enabled" "s3.bucket" "s3.endpoint" "s3.access_key" "s3.secret_key")
FOUND_S3_KEY=false

for key in "${S3_KEYS[@]}"; do
    if rbd --conf "$CONF_PATH" image-meta get "$POOL/$CHILD_IMAGE" "$key" 2>/dev/null; then
        echo "  ✗ Found S3 key in child: $key"
        FOUND_S3_KEY=true
    fi
done

if [ "$FOUND_S3_KEY" = false ]; then
    echo "  ✓ No S3 metadata keys found in child image (correct)"
else
    echo "  ✗ Some S3 metadata keys were found in child image (incorrect)"
    TEST_RESULT="FAIL"
fi

# Clean up
echo ""
echo "Cleaning up test images..."
rbd --conf "$CONF_PATH" rm "$POOL/$CHILD_IMAGE"
rbd --conf "$CONF_PATH" rm "$POOL/$PARENT_IMAGE"

echo ""
echo "=== Test Result: $TEST_RESULT ==="
if [ "$TEST_RESULT" = "PASS" ]; then
    echo "The fix is working correctly - child clones do not inherit S3 metadata"
    exit 0
else
    echo "Test failed - child clones are still inheriting S3 metadata"
    exit 1
fi
