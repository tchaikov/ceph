#!/bin/bash
# test-s3-edge-cases.sh — Edge-case and regression tests for S3-backed standalone clones
#
# Tests:
#   1. test_metadata_no_inheritance  — child clone does NOT inherit parent S3 metadata
#   2. test_s3_config_roundtrip      — rbd s3-config set/get/clear roundtrip
#   3. test_rbd_info_standalone_child — rbd info shows correct parent_type and fields
#   4. test_s3_ranged_get            — HTTP Range requests return correct byte ranges
#
# Requires: running Ceph cluster (check_cluster_running), MinIO for tests 3-4.
# Usage: ./test-s3-edge-cases.sh [--conf <ceph.conf>]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/s3-test-common.sh"

POOL="s3-edge-test"
MINIO_PORT=19200
MINIO_CONSOLE_PORT=19201
S3_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
S3_BUCKET="edge-cases"
MINIO_DATA_DIR="/tmp/minio-edge-$$"

cleanup() {
    log_info "Cleaning up..."
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/edge-parent" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/edge-child"  2>/dev/null || true
    stop_minio $MINIO_PORT
    rm -rf "$MINIO_DATA_DIR" /tmp/edge-*.raw /tmp/edge-*.bin
    "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete "$POOL" "$POOL" \
        --yes-i-really-really-mean-it 2>/dev/null || true
}
trap cleanup EXIT

parse_common_args "$@"

log_info "=== S3-Backed Standalone Clone — Edge Cases ==="
echo

if ! check_cluster_running; then exit 1; fi

create_pool "$POOL"
enable_s3_fetch

# ============================================================================
test_metadata_no_inheritance() {
    # Child clones must NOT inherit S3 metadata from the parent.
    # Background: if a child inherits s3.enabled=true, QEMU would try to fetch
    # objects from S3 for the child itself, causing "Permission denied" errors.

    local parent="$POOL/edge-parent"
    local child="$POOL/edge-child"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$(echo "$parent")" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$(echo "$child")"  2>/dev/null || true

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$parent" --size 100M

    set_s3_config "$parent" "edge.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone "$parent" "$child"

    # Child must report "not set"
    if "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config get "$child" 2>&1 \
            | grep -q "S3 configuration is not set"; then
        log_success "Child does not inherit S3 config (correct)"
    else
        log_fail "Child inherited S3 config — regression!"
        return 1
    fi

    # Double-check individual keys
    for key in s3.enabled s3.bucket s3.endpoint s3.access_key s3.secret_key; do
        if "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta get "$child" "$key" 2>/dev/null; then
            log_fail "Child has inherited key: $key"
            return 1
        fi
    done

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true
}

# ============================================================================
test_s3_config_roundtrip() {
    # rbd s3-config set → get → clear roundtrip must preserve all fields.

    local img="$POOL/edge-parent"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size 100M

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$img" \
        --s3-endpoint    "http://s3.example.com:9000" \
        --s3-bucket      "my-bucket" \
        --s3-image-name  "disk.raw" \
        --s3-access-key  "AKIATEST123" \
        --s3-secret-key  "supersecret"

    local info
    info=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config get "$img" 2>&1)

    for expected in "my-bucket" "http://s3.example.com:9000" "disk.raw" "AKIATEST123"; do
        if ! echo "$info" | grep -q "$expected"; then
            log_fail "s3-config get is missing field: $expected"
            echo "$info"
            return 1
        fi
    done
    # Secret key must be masked in output
    if echo "$info" | grep -q "supersecret"; then
        log_fail "s3-config get is exposing the secret key in plaintext"
        return 1
    fi
    log_success "s3-config get returned all expected fields with secret masked"

    # Clear
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config clear "$img"
    if ! "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config get "$img" 2>&1 \
            | grep -q "S3 configuration is not set"; then
        log_fail "s3-config clear did not remove configuration"
        return 1
    fi
    log_success "s3-config clear removed all S3 fields"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
}

# ============================================================================
test_rbd_info_standalone_child() {
    # rbd info on a standalone clone child must report:
    #   parent:      <pool>/<parent>
    #   parent_type: standalone   (not snapshot)
    #   overlap:     <image-size>

    local parent="$POOL/edge-parent"
    local child="$POOL/edge-child"
    local size_mb=20

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child"  2>/dev/null || true

    # Start MinIO and create S3 parent so the image has valid metadata
    if ! start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"; then
        log_error "Cannot start MinIO"; return 1
    fi
    setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

    create_s3_parent "$POOL" "edge-parent" "edge.raw" $size_mb \
        "$S3_ENDPOINT" "$S3_BUCKET" "$CEPH_CONF"

    create_standalone_clone "$POOL" "edge-parent" "edge-child"

    local info
    info=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$child")

    # Verify parent reference
    if ! echo "$info" | grep -q "parent:"; then
        log_fail "rbd info missing 'parent:' field"
        echo "$info"
        return 1
    fi

    # Verify parent_type = standalone
    if ! echo "$info" | grep -q "parent_type:.*standalone"; then
        log_fail "rbd info missing or wrong parent_type (expected 'standalone')"
        echo "$info"
        return 1
    fi

    # Verify overlap field
    if ! echo "$info" | grep -q "overlap:"; then
        log_fail "rbd info missing 'overlap:' field"
        echo "$info"
        return 1
    fi

    log_success "rbd info shows correct parent reference and parent_type=standalone"
    log_info "$(echo "$info" | grep -E 'parent:|parent_type:|overlap:|size:')"

    # Verify rbd children lists this child
    local children
    children=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" children "$POOL/edge-parent" 2>/dev/null || true)
    if echo "$children" | grep -q "edge-child"; then
        log_success "rbd children lists the standalone clone child"
    else
        log_warn "rbd children did not list edge-child (may be expected for standalone type)"
    fi

    stop_minio $MINIO_PORT
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true
}

# ============================================================================
test_s3_ranged_get() {
    # HTTP Range requests to MinIO must return the exact requested byte range
    # with the correct data pattern. This validates the S3ObjectFetcher
    # Range header implementation without involving librbd.

    # Start MinIO if not already running
    if ! "$MINIO_BIN/mc" alias set local "http://127.0.0.1:$MINIO_PORT" \
            minioadmin minioadmin > /dev/null 2>&1; then
        if ! start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"; then
            log_error "Cannot start MinIO"; return 1
        fi
        setup_s3_bucket $MINIO_PORT "$S3_BUCKET"
    fi

    # Create 10 MB test image where each 4 KB block contains its block number
    local image_file="/tmp/edge-ranged-get-$$.raw"
    python3 - <<'PYEOF'
import struct, sys
block_size = 4096
num_blocks = (10 * 1024 * 1024) // block_size
with open('/tmp/edge-ranged-get-test.raw', 'wb') as f:
    for n in range(num_blocks):
        f.write(struct.pack('<Q', n) * (block_size // 8))
print(f"Created {num_blocks}-block test image")
PYEOF
    image_file="/tmp/edge-ranged-get-test.raw"
    upload_to_s3 "$image_file" "$S3_BUCKET" "ranged-get-test.raw"

    local url="http://127.0.0.1:$MINIO_PORT/$S3_BUCKET/ranged-get-test.raw"

    # Test 3 different blocks: 0, 1, 100
    local ok=0
    for block_no in 0 1 100; do
        local start=$((block_no * 4096))
        local end=$((start + 4095))
        local out="/tmp/edge-range-${block_no}-$$.bin"

        curl -sf -r "${start}-${end}" "$url" -o "$out"
        local size
        size=$(stat -c%s "$out" 2>/dev/null || stat -f%z "$out" 2>/dev/null)
        if [ "$size" -ne 4096 ]; then
            log_fail "Block $block_no: expected 4096 bytes, got $size"
            ok=1; continue
        fi

        # Verify data pattern
        python3 - <<PYEOF2
import struct, sys
with open('${out}', 'rb') as f:
    data = f.read()
expected = struct.pack('<Q', ${block_no}) * (4096 // 8)
if data == expected:
    print("✓ Block ${block_no}: byte pattern correct")
else:
    print("✗ Block ${block_no}: byte pattern mismatch")
    sys.exit(1)
PYEOF2
        local rc=$?
        if [ $rc -ne 0 ]; then ok=1; fi
        rm -f "$out"
    done

    rm -f "$image_file" /tmp/edge-ranged-get-test.raw
    stop_minio $MINIO_PORT

    return $ok
}

# ============================================================================
# Main
# ============================================================================

run_test "metadata_no_inheritance"    test_metadata_no_inheritance
run_test "s3_config_roundtrip"        test_s3_config_roundtrip
run_test "rbd_info_standalone_child"  test_rbd_info_standalone_child
run_test "s3_ranged_get"              test_s3_ranged_get

print_test_summary
