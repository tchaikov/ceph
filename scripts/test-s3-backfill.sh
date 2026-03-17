#!/bin/bash
# test-s3-backfill.sh — Consolidated backfill tests for S3-backed standalone clones
#
# Tests:
#   1. test_backfill_lifecycle       — daemon discovers scheduled image, fetches from S3,
#                                      all objects restored to RADOS
#   2. test_backfill_data_integrity  — md5sum of original image matches export after backfill
#   3. test_backfill_object_naming   — RADOS objects use hex-formatted names (not decimal)
#   4. test_backfill_no_conf_fails_fast — omitting --conf exits <10s, does not hang
#   5. test_backfill_foreground_stderr  — --foreground produces visible stderr output
#
# Requires: running Ceph cluster (check_cluster_running), MinIO.
# Usage: ./test-s3-backfill.sh [--conf <ceph.conf>]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib/s3-test-common.sh"

POOL="s3-backfill-test"
MINIO_PORT=29300
MINIO_CONSOLE_PORT=29301
S3_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
S3_BUCKET="backfill-tests"
MINIO_DATA_DIR="/tmp/minio-backfill-$$"
BACKFILL_LOG="/tmp/rbd-backfill-$$.log"

cleanup() {
    log_info "Cleaning up..."
    stop_backfill_daemon
    stop_minio $MINIO_PORT
    rm -rf "$MINIO_DATA_DIR" /tmp/backfill-test-*.raw "$BACKFILL_LOG" \
           /tmp/rbd-backfill-noconf-$$.log /tmp/rbd-backfill-stderr-$$.log
    for img in lifecycle-parent integrity-parent naming-parent; do
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$img" 2>/dev/null || true
    done
    "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete "$POOL" "$POOL" \
        --yes-i-really-really-mean-it 2>/dev/null || true
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; shift 2 ;;
        *) shift ;;
    esac
done

log_info "=== S3-Backed Standalone Clone — Backfill Tests ==="
echo

if ! check_cluster_running; then exit 1; fi

create_pool "$POOL"
enable_s3_fetch

# Start a shared MinIO instance for all tests that need S3
start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"
setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

# ============================================================================
run_test() {
    local name=$1; shift
    local start=$(date +%s)
    local result="FAILED"
    log_step ">>> $name"
    if "$@"; then result="PASSED"; fi
    local end=$(date +%s)
    record_test_result "$name" "$result" $((end - start))
    log_success "$name: $result"
    echo
}

# ============================================================================
test_backfill_lifecycle() {
    # Daemon discovers a backfill-scheduled image and copies all objects from
    # S3 to RADOS.  Verifies: image discovered, thread started, object count
    # matches expected.

    local img="$POOL/lifecycle-parent"
    local size_mb=20
    local raw_file="/tmp/backfill-test-lifecycle-$$.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true

    # Create and upload a test image
    create_test_image_with_pattern $size_mb "$raw_file"
    "$MINIO_BIN/mc" cp "$raw_file" "local/$S3_BUCKET/lifecycle.raw" 2>&1 | grep -v "^mc:" || true

    # Create the parent image with S3 config
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$img" \
        --s3-endpoint   "$S3_ENDPOINT" \
        --s3-bucket     "$S3_BUCKET" \
        --s3-image-name "lifecycle.raw" \
        --s3-access-key minioadmin \
        --s3-secret-key minioadmin

    # Schedule backfill
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img"

    local scheduled
    scheduled=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
        image-meta get "$img" backfill_scheduled 2>/dev/null || echo "")
    if [ "$scheduled" != "true" ]; then
        log_fail "backfill_scheduled metadata not set"
        return 1
    fi

    # Start daemon and wait for objects
    run_backfill_daemon "$CEPH_CONF" "$BACKFILL_LOG"
    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "lifecycle-parent")
    local expected=$(( (size_mb + 3) / 4 ))  # ceil(size_mb / 4MB object size)

    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" "$expected" 60; then
        log_fail "Backfill did not complete within 60s"
        tail -30 "$BACKFILL_LOG"
        stop_backfill_daemon
        return 1
    fi

    stop_backfill_daemon
    rm -f "$raw_file"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
}

# ============================================================================
test_backfill_data_integrity() {
    # After backfill, export the parent image and compare md5sum with the
    # original raw file that was uploaded to S3.

    local img="$POOL/integrity-parent"
    local size_mb=16  # 4 objects × 4 MB
    local raw_file="/tmp/backfill-test-integrity-orig-$$.raw"
    local exp_file="/tmp/backfill-test-integrity-exp-$$.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw_file"
    "$MINIO_BIN/mc" cp "$raw_file" "local/$S3_BUCKET/integrity.raw" 2>&1 | grep -v "^mc:" || true

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$img" \
        --s3-endpoint   "$S3_ENDPOINT" \
        --s3-bucket     "$S3_BUCKET" \
        --s3-image-name "integrity.raw" \
        --s3-access-key minioadmin \
        --s3-secret-key minioadmin

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img"

    local blog="/tmp/rbd-backfill-integrity-$$.log"
    run_backfill_daemon "$CEPH_CONF" "$blog"
    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "integrity-parent")
    local expected=$(( size_mb / 4 ))

    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" "$expected" 60; then
        log_fail "Backfill did not complete"
        stop_backfill_daemon
        return 1
    fi
    stop_backfill_daemon

    # Export and compare
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$img" "$exp_file"

    if ! verify_checksum "$raw_file" "$exp_file"; then
        rm -f "$raw_file" "$exp_file" "$blog"
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
        return 1
    fi

    rm -f "$raw_file" "$exp_file" "$blog"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
}

# ============================================================================
test_backfill_object_naming() {
    # RADOS objects created by the backfill daemon must use hex-formatted names
    # (e.g. rbd_data.<id>.0000000000000000) not decimal
    # (rbd_data.<id>.0, rbd_data.<id>.1, …).

    local img="$POOL/naming-parent"
    local size_mb=20   # 5 objects × 4 MB
    local raw_file="/tmp/backfill-test-naming-$$.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw_file"
    "$MINIO_BIN/mc" cp "$raw_file" "local/$S3_BUCKET/naming.raw" 2>&1 | grep -v "^mc:" || true

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$img" \
        --s3-endpoint   "$S3_ENDPOINT" \
        --s3-bucket     "$S3_BUCKET" \
        --s3-image-name "naming.raw" \
        --s3-access-key minioadmin \
        --s3-secret-key minioadmin

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img"

    local blog="/tmp/rbd-backfill-naming-$$.log"
    run_backfill_daemon "$CEPH_CONF" "$blog"
    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "naming-parent")
    local expected=$(( (size_mb + 3) / 4 ))

    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" "$expected" 60; then
        log_fail "Backfill did not complete"
        stop_backfill_daemon
        return 1
    fi
    stop_backfill_daemon

    # List objects and verify naming format
    local objects
    objects=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls \
        | grep "^${prefix}\." | sort)

    if [ -z "$objects" ]; then
        log_fail "No RADOS objects found with prefix $prefix"
        return 1
    fi

    local decimal_names=0
    local hex_names=0
    while IFS= read -r obj; do
        local suffix="${obj##*.}"
        # Check hex format first: exactly 16 hex chars (properly zero-padded).
        # This must come before the decimal check because hex suffixes composed
        # entirely of digits (e.g. "0000000000000000") also match ^[0-9]+$.
        if echo "$suffix" | grep -qE '^[0-9a-f]{16}$'; then
            hex_names=$((hex_names + 1))
        # Decimal suffix: short unpadded number (old bug, e.g. "0", "1", "12")
        elif echo "$suffix" | grep -qE '^[0-9]+$'; then
            log_warn "Decimal-named object (unpadded): $obj"
            decimal_names=$((decimal_names + 1))
        fi
    done <<< "$objects"

    if [ $decimal_names -gt 0 ]; then
        log_fail "Found $decimal_names decimal-named objects — object naming regression!"
        return 1
    fi

    if [ $hex_names -eq 0 ]; then
        log_fail "No hex-named objects found — unexpected format: $objects"
        return 1
    fi

    log_success "All $hex_names objects use correct hex naming format"
    rm -f "$raw_file" "$blog"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
}

# ============================================================================
test_backfill_no_conf_fails_fast() {
    # Omitting --conf must cause the daemon to exit within 10 seconds.
    # Previously the daemon would block indefinitely waiting for unreachable
    # monitors when no config file was specified.

    local logfile="/tmp/rbd-backfill-noconf-$$.log"
    log_info "Running rbd-backfill WITHOUT --conf (expect exit <10s)..."

    set +e
    timeout 10 "$BUILD_DIR/bin/rbd-backfill" --foreground > "$logfile" 2>&1
    local exit_code=$?
    set -e

    rm -f "$logfile"

    if [ $exit_code -eq 124 ]; then
        log_fail "rbd-backfill hung >10s without --conf (Rados::connect blocked indefinitely)"
        return 1
    fi

    log_success "rbd-backfill exited quickly without --conf (exit code: $exit_code)"
}

# ============================================================================
test_backfill_foreground_stderr() {
    # --foreground must produce output on stderr.
    # Regression: early versions sent all log output to a file even with
    # --foreground because log_to_stderr was not set when daemonize=false.

    local logfile="/tmp/rbd-backfill-stderr-$$.log"
    log_info "Running rbd-backfill --foreground, capturing stderr for 3s..."

    set +e
    "$BUILD_DIR/bin/rbd-backfill" --conf "$CEPH_CONF" --foreground \
        > "$logfile" 2>&1 &
    local test_pid=$!
    sleep 3
    kill $test_pid 2>/dev/null
    wait $test_pid 2>/dev/null || true
    set -e

    local line_count
    line_count=$(wc -l < "$logfile")

    if [ "$line_count" -eq 0 ]; then
        log_fail "--foreground produced no stderr output"
        rm -f "$logfile"
        return 1
    fi

    if ! grep -q "rbd-backfill" "$logfile"; then
        log_fail "Expected startup message not found in stderr"
        cat "$logfile"
        rm -f "$logfile"
        return 1
    fi

    log_success "--foreground produced $line_count lines of stderr output"
    rm -f "$logfile"
}

# ============================================================================
# Main
# ============================================================================

run_test "backfill_lifecycle"         test_backfill_lifecycle
run_test "backfill_data_integrity"    test_backfill_data_integrity
run_test "backfill_object_naming"     test_backfill_object_naming
run_test "backfill_no_conf_fails_fast" test_backfill_no_conf_fails_fast
run_test "backfill_foreground_stderr" test_backfill_foreground_stderr

print_test_summary
