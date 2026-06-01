#!/bin/bash
# test-s3-backfill.sh — Consolidated backfill tests for S3-backed standalone clones
#
# Tests:
#   1. test_backfill_lifecycle         — daemon discovers scheduled image, fetches from S3,
#                                        all objects restored to RADOS
#   2. test_backfill_data_integrity    — md5sum of original image matches export after backfill
#   3. test_backfill_object_naming     — RADOS objects use hex-formatted names (not decimal)
#   4. test_backfill_cache_hit         — after backfill, reads succeed with MinIO stopped
#                                        (verifies parent RADOS acts as cache for user I/O)
#   5. test_backfill_restart_recovery  — kill daemon mid-backfill, verify restart completes
#                                        without re-fetching already-cached objects
#   6. test_backfill_no_conf_fails_fast — omitting --conf exits <10s, does not hang
#   7. test_backfill_foreground_stderr  — --foreground produces visible stderr output
#   8. test_backfill_status_transitions — state machine: scheduled→absent, status→complete
#   9. test_backfill_status_on_failure  — S3 unreachable ⇒ status=failed, scheduled cleared
#  10. test_parent_du_after_backfill    — rbd du parent: 0 before, provisioned_size after backfill
#  11. test_parent_du_after_child_writeback — child COW write-back registers in parent rbd du
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
           /tmp/rbd-backfill-*.log
    for img in lifecycle-parent integrity-parent naming-parent \
               cache-hit-child cache-hit-parent restart-parent \
               status-trans-parent status-fail-parent \
               du-backfill-parent du-writeback-parent du-writeback-child \
               watcher-release-parent clone-bf-parent clone-bf-child \
               list-bf-true list-bf-inprog list-bf-done \
               zero-trust-parent zero-trust-child \
               partial-trust-parent partial-trust-child \
               clone-parent-opfeat-base clone-parent-opfeat-child; do
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/$img" 2>/dev/null || true
    done
    "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool delete "$POOL" "$POOL" \
        --yes-i-really-really-mean-it 2>/dev/null || true
}
trap cleanup EXIT

parse_common_args "$@"

log_info "=== S3-Backed Standalone Clone — Backfill Tests ==="
echo

if ! check_cluster_running; then exit 1; fi

create_pool "$POOL"
enable_s3_fetch

# Start a shared MinIO instance for all tests that need S3
start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"
setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

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
    upload_to_s3 "$raw_file" "$S3_BUCKET" "lifecycle.raw"

    # Create the parent image with S3 config
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    set_s3_config "$img" "lifecycle.raw"

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
    upload_to_s3 "$raw_file" "$S3_BUCKET" "integrity.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    set_s3_config "$img" "integrity.raw"

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
    upload_to_s3 "$raw_file" "$S3_BUCKET" "naming.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    set_s3_config "$img" "naming.raw"

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
test_backfill_cache_hit() {
    # Verifies the parent RADOS cache actually works: once backfill is complete,
    # reads from a child clone must succeed even with MinIO stopped.
    # This is the critical property — the parent RADOS image acts as a cache so
    # that user I/O never needs to reach S3 after the cache is warm.

    local parent_img="$POOL/cache-hit-parent"
    local child_img="$POOL/cache-hit-child"
    local size_mb=16   # 4 objects × 4 MB
    local raw_file="/tmp/backfill-test-cache-hit-orig-$$.raw"
    local exp_before="/tmp/backfill-test-cache-hit-before-$$.raw"
    local exp_after="/tmp/backfill-test-cache-hit-after-$$.raw"
    local blog="/tmp/rbd-backfill-cache-hit-$$.log"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child_img"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent_img" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw_file"
    upload_to_s3 "$raw_file" "$S3_BUCKET" "cache-hit.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$parent_img" --size ${size_mb}M
    set_s3_config "$parent_img" "cache-hit.raw"

    # Create a child clone from the parent
    create_standalone_clone "$POOL" "cache-hit-parent" "cache-hit-child"

    # Warm the parent cache via backfill
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$parent_img"
    run_backfill_daemon "$CEPH_CONF" "$blog"
    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "cache-hit-parent")
    local expected=$(( size_mb / 4 ))

    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" "$expected" 60; then
        log_fail "Backfill did not complete within 60s"
        stop_backfill_daemon
        return 1
    fi
    stop_backfill_daemon

    # Export child while MinIO is still up — baseline
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$child_img" "$exp_before"
    if ! verify_checksum "$raw_file" "$exp_before"; then
        log_fail "Child data mismatch before stopping MinIO"
        rm -f "$raw_file" "$exp_before" "$blog"
        return 1
    fi
    log_info "Child data correct with MinIO running"

    # Stop MinIO — any further read that hits S3 would fail.
    # User I/O from the child should be served entirely from the parent RADOS
    # cache.  If the cache-hit path is broken, rbd export below will return
    # EIO or zero-data and the checksum will mismatch.
    stop_minio $MINIO_PORT

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$child_img" "$exp_after"

    # Restart MinIO before asserting so cleanup can proceed regardless of result
    start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"
    setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

    if ! verify_checksum "$raw_file" "$exp_after"; then
        log_fail "Child data mismatch after stopping MinIO — parent cache is NOT serving reads"
        rm -f "$raw_file" "$exp_before" "$exp_after" "$blog"
        return 1
    fi
    log_success "Child reads served entirely from parent RADOS cache (MinIO was down)"

    rm -f "$raw_file" "$exp_before" "$exp_after" "$blog"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child_img"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent_img" 2>/dev/null || true
}

# ============================================================================
test_backfill_restart_recovery() {
    # Kill the daemon mid-backfill, then restart it.  Verify:
    #   (a) The restart completes the remaining objects.
    #   (b) Already-populated RADOS objects are NOT re-fetched from S3
    #       (stat-before-fetch optimization, Fix #2).
    #   (c) Final data integrity matches the original S3 content.

    local img="$POOL/restart-parent"
    local size_mb=40   # 10 objects × 4 MB — large enough to interrupt mid-flight
    local raw_file="/tmp/backfill-test-restart-orig-$$.raw"
    local exp_file="/tmp/backfill-test-restart-exp-$$.raw"
    local blog1="/tmp/rbd-backfill-restart-1-$$.log"
    local blog2="/tmp/rbd-backfill-restart-2-$$.log"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw_file"
    upload_to_s3 "$raw_file" "$S3_BUCKET" "restart.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    set_s3_config "$img" "restart.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img"
    run_backfill_daemon "$CEPH_CONF" "$blog1"

    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "restart-parent")

    # Wait until at least 3 objects are written, then kill daemon
    local waited=0
    while true; do
        local count
        count=$(count_rados_objects "$CEPH_CONF" "$POOL" "$prefix")
        log_info "  objects in RADOS so far: $count"
        if [ "$count" -ge 3 ]; then break; fi
        sleep 1
        waited=$((waited + 1))
        if [ $waited -ge 30 ]; then
            log_fail "Daemon did not write 3 objects within 30s"
            stop_backfill_daemon
            return 1
        fi
    done

    log_info "Interrupting daemon after $count objects..."
    stop_backfill_daemon

    # Re-schedule: reset to "true" so daemon discovers it again on restart
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$img" backfill_scheduled "true"

    local count_after_kill
    count_after_kill=$(count_rados_objects "$CEPH_CONF" "$POOL" "$prefix")
    log_info "Objects in RADOS after kill: $count_after_kill (of $((size_mb / 4)) total)"

    # Restart the daemon — it should skip already-populated objects (stat check)
    # and only fetch the remaining ones
    local t_restart_start=$(date +%s)
    run_backfill_daemon "$CEPH_CONF" "$blog2"
    local expected=$(( size_mb / 4 ))

    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" "$expected" 90; then
        log_fail "Daemon restart did not complete backfill within 90s"
        tail -20 "$blog2"
        stop_backfill_daemon
        return 1
    fi
    local t_restart_end=$(date +%s)
    stop_backfill_daemon

    local remaining=$(( expected - count_after_kill ))
    local elapsed=$(( t_restart_end - t_restart_start ))
    log_info "Restart fetched $remaining remaining objects in ${elapsed}s"

    # Verify final data integrity
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$img" "$exp_file"
    if ! verify_checksum "$raw_file" "$exp_file"; then
        log_fail "Data integrity check failed after daemon restart"
        rm -f "$raw_file" "$exp_file" "$blog1" "$blog2"
        return 1
    fi

    log_success "Daemon restart completed successfully; data integrity verified"
    rm -f "$raw_file" "$exp_file" "$blog1" "$blog2"
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
test_backfill_status_transitions() {
    # Verify the backfill state-machine ordering:
    #   backfill_scheduled: "true" → absent
    #   backfill_status:    absent → "complete"
    #
    # Also exercises the crash-safe write order: status is set BEFORE
    # backfill_scheduled is removed, so a crash between the two leaves the
    # image re-queueable on restart.

    local img="$POOL/status-trans-parent"
    local size_mb=4  # 1 object — fast test
    local raw_file="/tmp/backfill-test-status-$$.raw"
    local blog="/tmp/rbd-backfill-status-$$.log"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw_file"
    upload_to_s3 "$raw_file" "$S3_BUCKET" "status-trans.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    set_s3_config "$img" "status-trans.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img"

    # Pre-condition: scheduled=true, status absent
    local sched
    sched=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
        image-meta get "$img" backfill_scheduled 2>/dev/null || echo "")
    if [ "$sched" != "true" ]; then
        log_fail "pre-condition: backfill_scheduled not 'true'; got: '$sched'"
        rm -f "$raw_file" "$blog"
        return 1
    fi

    run_backfill_daemon "$CEPH_CONF" "$blog"

    # Poll until backfill_status=complete (up to 60s)
    local done=0
    for i in $(seq 1 60); do
        local status
        status=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            image-meta get "$img" backfill_status 2>/dev/null || echo "")
        if [ "$status" = "complete" ]; then
            done=1
            log_success "backfill_status=complete after ${i}s"
            break
        fi
        sleep 1
    done

    stop_backfill_daemon

    if [ "$done" -eq 0 ]; then
        log_fail "backfill_status never reached 'complete' within 60s"
        cat "$blog"
        rm -f "$raw_file" "$blog"
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
        return 1
    fi

    # Post-condition: backfill_scheduled must be absent after completion
    local sched_after
    sched_after=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
        image-meta get "$img" backfill_scheduled 2>/dev/null || echo "")
    if [ -n "$sched_after" ]; then
        log_fail "backfill_scheduled not cleared after completion; value='$sched_after'"
        rm -f "$raw_file" "$blog"
        return 1
    fi
    log_success "backfill_scheduled cleared after completion"

    rm -f "$raw_file" "$blog"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
}

# ============================================================================
test_backfill_status_on_failure() {
    # When S3 is unreachable, the daemon must set backfill_status="failed"
    # (not leave it absent or stale) and clear backfill_scheduled so the
    # operator knows the backfill attempt completed (unsuccessfully).

    local img="$POOL/status-fail-parent"
    local size_mb=4  # 1 object
    local blog="/tmp/rbd-backfill-fail-$$.log"
    # Use a port that is guaranteed to refuse connections immediately.
    local bad_endpoint="http://127.0.0.1:19997"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    set_s3_config "$img" "no-such-image.raw" "$bad_endpoint" "no-such-bucket"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img"

    run_backfill_daemon "$CEPH_CONF" "$blog"

    # Poll until backfill_status is non-empty (either "complete" or "failed")
    local done=0
    for i in $(seq 1 60); do
        local status
        status=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            image-meta get "$img" backfill_status 2>/dev/null || echo "")
        if [ -n "$status" ]; then
            done=1
            log_info "backfill_status='$status' after ${i}s"
            break
        fi
        sleep 1
    done

    stop_backfill_daemon

    if [ "$done" -eq 0 ]; then
        log_fail "backfill_status never set within 60s"
        cat "$blog"
        rm -f "$blog"
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
        return 1
    fi

    local final_status
    final_status=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
        image-meta get "$img" backfill_status 2>/dev/null || echo "")
    if [ "$final_status" != "failed" ]; then
        log_fail "expected backfill_status='failed', got: '$final_status'"
        rm -f "$blog"
        return 1
    fi
    log_success "backfill_status=failed as expected"

    # backfill_scheduled must be cleared even on failure
    local sched
    sched=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
        image-meta get "$img" backfill_scheduled 2>/dev/null || echo "")
    if [ -n "$sched" ]; then
        log_fail "backfill_scheduled not cleared after failure; value='$sched'"
        rm -f "$blog"
        return 1
    fi
    log_success "backfill_scheduled cleared after failure"

    rm -f "$blog"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
}

# ============================================================================
test_parent_du_after_backfill() {
    # rbd du on the parent must report used_size=0 before backfill and
    # used_size==provisioned_size after backfill completes.  Exercises both
    # the ObjectBackfillRequest write path and its object_map_update step.

    local img="$POOL/du-backfill-parent"
    local size_mb=16  # 4 objects × 4 MiB — small but multi-object
    local raw_file="/tmp/backfill-test-du-$$.raw"
    local blog="/tmp/rbd-backfill-du-$$.log"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw_file"
    upload_to_s3 "$raw_file" "$S3_BUCKET" "du-backfill.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    set_s3_config "$img" "du-backfill.raw"

    # Before backfill: freshly created image has no RADOS objects → used_size=0
    local used_before
    used_before=$(get_image_used_size "$CEPH_CONF" "$img")
    if [ "$used_before" -ne 0 ]; then
        log_fail "parent used_size before backfill should be 0, got: $used_before"
        rm -f "$raw_file" "$blog"
        return 1
    fi
    log_success "parent used_size before backfill: 0 bytes (correct)"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img"
    run_backfill_daemon "$CEPH_CONF" "$blog"
    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "du-backfill-parent")
    local expected=$(( size_mb / 4 ))

    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" "$expected" 60; then
        log_fail "Backfill did not complete within 60s"
        stop_backfill_daemon
        rm -f "$raw_file" "$blog"
        return 1
    fi
    stop_backfill_daemon

    # After backfill: every object is written → used_size must equal provisioned_size
    local provisioned=$(( size_mb * 1024 * 1024 ))
    local used_after
    used_after=$(get_image_used_size "$CEPH_CONF" "$img")
    if [ "$used_after" -ne "$provisioned" ]; then
        log_fail "parent used_size after backfill: expected ${provisioned}B, got ${used_after}B"
        rm -f "$raw_file" "$blog"
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
        return 1
    fi
    log_success "parent used_size after backfill: ${used_after}B == provisioned_size (correct)"

    rm -f "$raw_file" "$blog"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true
}

# ============================================================================
test_parent_du_after_child_writeback() {
    # When child I/O reads an uncached object, ObjectReadRequest fetches it
    # from S3, writes it to the parent RADOS pool (write_back_s3_data), and
    # updates the parent's object map.  Verified via rbd du.

    local parent_img="$POOL/du-writeback-parent"
    local child_img="$POOL/du-writeback-child"
    local size_mb=4  # 1 object — fast test
    local raw_file="/tmp/backfill-test-du-wb-$$.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child_img"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent_img" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw_file"
    upload_to_s3 "$raw_file" "$S3_BUCKET" "du-writeback.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$parent_img" --size ${size_mb}M
    set_s3_config "$parent_img" "du-writeback.raw"

    create_standalone_clone "$POOL" "du-writeback-parent" "du-writeback-child"

    # Trigger S3 fetch by reading from the child.  A read on a fresh clone
    # hits ObjectReadRequest → write_back_s3_data() which writes the fetched
    # object to the parent RADOS pool and updates the object map.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$child_img" - >/dev/null 2>&1 || true

    # Poll until parent used_size > 0 (write-back is async but completes quickly)
    local done=0
    for i in $(seq 1 15); do
        local used
        used=$(get_image_used_size "$CEPH_CONF" "$parent_img")
        if [ "$used" -gt 0 ]; then
            done=1
            log_success "parent used_size after child write-back: ${used}B (after ${i}s)"
            break
        fi
        sleep 1
    done

    if [ "$done" -eq 0 ]; then
        log_fail "parent used_size never became > 0 after child write-back (15s timeout)"
        rm -f "$raw_file"
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child_img"  2>/dev/null || true
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent_img" 2>/dev/null || true
        return 1
    fi

    # The write-back writes a full 4 MiB object regardless of write size
    local expected_used=$PARENT_BLOCK_SIZE
    local final_used
    final_used=$(get_image_used_size "$CEPH_CONF" "$parent_img")
    if [ "$final_used" -ne "$expected_used" ]; then
        log_fail "parent used_size: expected ${expected_used}B (1×4MiB), got ${final_used}B"
        rm -f "$raw_file"
        return 1
    fi
    log_success "parent used_size = ${final_used}B (1 × 4 MiB object, correct)"

    rm -f "$raw_file"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child_img"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent_img" 2>/dev/null || true
}

# ============================================================================
test_backfill_rescan_picks_up_new_image() {
    # Verifies that BackfillDaemon's periodic rescan picks up images scheduled
    # AFTER daemon startup.  Before this fix, the daemon was a one-shot scanner:
    # `rbd backfill schedule` invocations after init were ignored until the
    # daemon was restarted.  With rbd_backfill_rescan_interval > 0, scheduled
    # images are claimed within one rescan tick.

    local size_mb=8        # 2 objects per image — quick to backfill
    local img1="$POOL/rescan-img1"
    local img2="$POOL/rescan-img2"
    local raw1="/tmp/backfill-test-rescan1-$$.raw"
    local raw2="/tmp/backfill-test-rescan2-$$.raw"
    local interval=5       # short interval so the test finishes quickly

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img1" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img2" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw1"
    create_test_image_with_pattern $size_mb "$raw2"
    upload_to_s3 "$raw1" "$S3_BUCKET" "rescan-img1.raw"
    upload_to_s3 "$raw2" "$S3_BUCKET" "rescan-img2.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img1" --size ${size_mb}M
    set_s3_config "$img1" "rescan-img1.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img2" --size ${size_mb}M
    set_s3_config "$img2" "rescan-img2.raw"

    # Schedule img1 BEFORE daemon starts.  img2 will be scheduled afterward
    # to exercise the rescan path.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img1"

    # Start daemon with a short rescan interval via --rbd-backfill-rescan-interval.
    log_info "Starting daemon with rescan_interval=${interval}s"
    "$BUILD_DIR/bin/rbd-backfill" --conf "$CEPH_CONF" --foreground \
        --rbd-backfill-rescan-interval "$interval" \
        > "$BACKFILL_LOG" 2>&1 &
    BACKFILL_PID=$!
    sleep 2
    if ! kill -0 "$BACKFILL_PID" 2>/dev/null; then
        log_fail "rbd-backfill failed to start with --rbd-backfill-rescan-interval"
        cat "$BACKFILL_LOG"
        return 1
    fi

    local prefix1
    prefix1=$(get_block_prefix "$CEPH_CONF" "$POOL" "rescan-img1")
    local expected_objs=$(( (size_mb + 3) / 4 ))

    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix1" "$expected_objs" 30; then
        log_fail "img1 (scheduled before daemon start) was not backfilled"
        stop_backfill_daemon
        return 1
    fi
    log_success "img1 backfilled (initial discovery)"

    # Now schedule img2 AFTER daemon is running.  Without rescan, this would
    # never be picked up.  With rescan, the daemon should claim it within
    # the next tick (≤ ${interval}s) and complete shortly after.
    log_info "Scheduling img2 while daemon is running (testing rescan)"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img2"

    local prefix2
    prefix2=$(get_block_prefix "$CEPH_CONF" "$POOL" "rescan-img2")

    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix2" "$expected_objs" $((interval * 4)); then
        log_fail "img2 (scheduled after daemon start) was NOT picked up — rescan broken"
        tail -40 "$BACKFILL_LOG"
        stop_backfill_daemon
        return 1
    fi
    log_success "img2 backfilled via periodic rescan"

    stop_backfill_daemon
    rm -f "$raw1" "$raw2"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img1" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img2" 2>/dev/null || true
}

# ============================================================================
test_backfill_watcher_released_after_complete() {
    # Regression for the user-reported bug 1: after backfill completes, the
    # daemon used to cache the ImageBackfiller in its m_backfillers map
    # forever, keeping the ImageCtx open with an active RADOS watcher.  The
    # operator's `rbd rm <base>` then failed because RADOS refuses removal
    # while a watcher is registered.
    #
    # Fix landed in 5fbd0384643: ImageBackfiller closes its ImageCtx
    # PROACTIVELY right after the metadata update at the end of run_backfill,
    # releasing the watcher even though the backfiller thread continues to
    # idle waiting for stop().  This test verifies the watcher is gone
    # while the daemon is STILL RUNNING.

    local img="$POOL/watcher-release-parent"
    local size_mb=8        # 2 objects — quick to backfill
    local raw_file="/tmp/backfill-test-watcher-$$.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null || true

    create_test_image_with_pattern $size_mb "$raw_file"
    upload_to_s3 "$raw_file" "$S3_BUCKET" "watcher-release.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$img" --size ${size_mb}M
    set_s3_config "$img" "watcher-release.raw"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$img"

    # Look up the header oid BEFORE the watcher races away so the rados
    # listwatchers below references the right object.
    local image_id
    image_id=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$img" 2>/dev/null \
        | awk '/id:/ {print $2; exit}')
    if [ -z "$image_id" ]; then
        log_fail "could not extract image id from rbd info"
        return 1
    fi
    local header_oid="rbd_header.${image_id}"

    run_backfill_daemon "$CEPH_CONF" "$BACKFILL_LOG"

    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "watcher-release-parent")
    local expected=$(( (size_mb + 3) / 4 ))
    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" "$expected" 60; then
        log_fail "backfill did not complete in 60s"
        stop_backfill_daemon
        return 1
    fi

    # Poll for backfill_scheduled cleared — this is the LAST metadata write
    # done by run_backfill, and the proactive close fires immediately after.
    local cleared=0
    for i in $(seq 1 30); do
        local sched
        sched=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            image-meta get "$img" backfill_scheduled 2>/dev/null || echo "")
        if [ -z "$sched" ]; then
            cleared=1
            break
        fi
        sleep 1
    done
    if [ "$cleared" -ne 1 ]; then
        log_fail "backfill_scheduled not cleared within 30s of object backfill complete"
        stop_backfill_daemon
        return 1
    fi

    # Watcher should release within ~1s of close_image_if_open() returning.
    # Poll up to 5s as a generous grace window.
    local watchers=""
    local empty=0
    for i in $(seq 1 5); do
        watchers=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" \
            listwatchers "$header_oid" 2>/dev/null || true)
        # Empty output (no watchers) means the daemon released its watcher.
        # rados listwatchers prints "watcher=<addr>/...client.NNN ..." per watcher.
        if [ -z "$(echo "$watchers" | grep -E '^watcher=')" ]; then
            empty=1
            break
        fi
        sleep 1
    done
    if [ "$empty" -ne 1 ]; then
        log_fail "RADOS watcher on $header_oid was not released after backfill complete"
        log_fail "  (daemon still running; pre-fix behaviour kept watcher until shutdown)"
        echo "$watchers" | sed 's/^/    /'
        stop_backfill_daemon
        return 1
    fi
    log_success "watcher on $header_oid released after backfill complete (daemon still running)"

    # Belt-and-braces: rm must succeed even though daemon is still alive.
    if ! "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$img" 2>/dev/null; then
        log_fail "rbd rm $img failed even though listwatchers reported empty"
        stop_backfill_daemon
        return 1
    fi
    log_success "rbd rm $img succeeded with daemon still running"

    stop_backfill_daemon
    rm -f "$raw_file"
}

# ============================================================================
test_clone_standalone_drops_backfill_metadata() {
    # Regression for user-reported bug 2a: after `rbd backfill schedule
    # basev11`, `rbd clone-standalone basev11 lazyv02-bv11` used to leave
    # the new clone with the parent's backfill_* metadata keys.  The daemon's
    # discovery loop then claimed the clone as a separate scheduled image
    # and tried to backfill it — even though the clone has no S3 backing
    # of its own.
    #
    # Fix landed in fc2a67d32ed (CloneRequest.cc): metadata filter now
    # excludes both the "s3." and "backfill_" namespaces.  This test sets
    # the marks on the parent directly (no daemon needed — we only exercise
    # the clone metadata-copy path) and verifies the child has NO
    # backfill_* keys after the clone.

    local parent="$POOL/clone-bf-parent"
    local child="$POOL/clone-bf-child"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$parent" --size 4M --object-size 4M
    # We do NOT need a working S3 config for this test -- only the metadata
    # filter behaviour is under test.  Set the backfill_* keys directly.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$parent" backfill_scheduled true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$parent" backfill_status scheduled
    # Sanity: also drop a non-backfill key so we can confirm the filter only
    # affects backfill keys, not ALL metadata.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$parent" user_marker hello

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone "$parent" "$child"

    # Child must NOT have either backfill_* key.
    for key in backfill_scheduled backfill_status; do
        local v
        v=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta get "$child" "$key" 2>/dev/null || echo "")
        if [ -n "$v" ]; then
            log_fail "child inherited $key='$v' from parent (filter regression)"
            return 1
        fi
    done
    log_success "child does not inherit backfill_scheduled or backfill_status"

    # Non-backfill keys SHOULD still propagate — confirming we filter only
    # the backfill namespace, not all metadata.  This catches a future
    # over-eager filter that accidentally strips user metadata too.
    local user_v
    user_v=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta get "$child" user_marker 2>/dev/null || echo "")
    if [ "$user_v" != "hello" ]; then
        log_fail "child did NOT inherit user_marker (filter over-broad); got '$user_v'"
        return 1
    fi
    log_success "child correctly inherits non-backfill user metadata"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true
}

# ============================================================================
test_standalone_clone_rm_clears_clone_parent() {
    # Regression for user-reported Bug #3: after removing the last standalone
    # clone child, `rbd info <base>` still showed `op_features: clone-parent`.
    # Root cause: child_detach must erase the child entry from the parent
    # header AND trigger set_op_features(0, CLONE_PARENT) when the entry set
    # becomes empty.  This test guards the same-cluster path.  The cross-cluster
    # path is covered by run_plain_cross_cluster_direct_rm in
    # test-s3-cross-cluster.sh (which tests that the base is removable, which
    # requires CLONE_PARENT to be gone too).

    local base="$POOL/clone-parent-opfeat-base"
    local child="$POOL/clone-parent-opfeat-child"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$base"  2>/dev/null || true

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$base" --size 4M --object-size 4M

    local before
    before=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$base" 2>/dev/null \
             | grep op_features || true)
    if echo "$before" | grep -q "clone-parent"; then
        log_fail "base shows clone-parent BEFORE any clone — unexpected state"
        return 1
    fi
    log_success "base has no clone-parent before clone (as expected)"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone "$base" "$child"

    local after_clone
    after_clone=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$base" 2>/dev/null \
                  | grep op_features || true)
    if ! echo "$after_clone" | grep -q "clone-parent"; then
        log_fail "base does NOT show clone-parent after clone — child_attach regression"
        return 1
    fi
    log_success "base shows clone-parent after clone (expected)"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child" 2>/dev/null

    local after_rm
    after_rm=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$base" 2>/dev/null \
               | grep op_features || true)
    if echo "$after_rm" | grep -q "clone-parent"; then
        log_fail "Bug #3 regression: base still shows clone-parent after last child rm"
        log_error "  child_detach likely returned -ENOENT without updating parent"
        return 1
    fi
    log_success "base no longer shows clone-parent after child rm — CLONE_PARENT cleared"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$base" 2>/dev/null || true
}

# ============================================================================
test_backfill_list_shows_in_progress() {
    # Regression for user-reported bug 2b: `rbd backfill list` filtered on
    # backfill_scheduled == "true" only, so once the daemon transitioned the
    # value to "in_progress" (the second daemon claim race-guard), the list
    # command stopped showing the image — operators saw zero entries while
    # the daemon was actively backfilling, making them think their schedule
    # request was lost.
    #
    # Fix landed in fc2a67d32ed (Backfill.cc execute_list): accept BOTH
    # "true" and "in_progress" as visible-to-list.  This test exercises
    # the filter path directly by setting backfill_scheduled to each value
    # and verifying the image is enumerated.

    local true_img="$POOL/list-bf-true"
    local prog_img="$POOL/list-bf-inprog"
    local done_img="$POOL/list-bf-done"

    for i in "$true_img" "$prog_img" "$done_img"; do
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$i" 2>/dev/null || true
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$i" --size 4M --object-size 4M
    done
    # true: visible
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$true_img" backfill_scheduled true
    # in_progress: visible (the bug 2b fix)
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$prog_img" backfill_scheduled in_progress
    # complete (scheduled cleared, status=complete): NOT visible
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta set "$done_img" backfill_status complete

    local list_out
    list_out=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill list --pool "$POOL" 2>&1 \
                | grep -v -E '^WARNING|^2026|^\*\*\*' || true)

    if ! echo "$list_out" | grep -q "list-bf-true"; then
        log_fail "image with backfill_scheduled=true MISSING from rbd backfill list"
        echo "$list_out" | sed 's/^/    /'
        return 1
    fi
    log_success "image with scheduled=true visible in rbd backfill list"

    if ! echo "$list_out" | grep -q "list-bf-inprog"; then
        log_fail "image with backfill_scheduled=in_progress MISSING from rbd backfill list"
        log_fail "  (this is the bug 2b regression — pre-fix the filter only matched 'true')"
        echo "$list_out" | sed 's/^/    /'
        return 1
    fi
    log_success "image with scheduled=in_progress visible in rbd backfill list"

    if echo "$list_out" | grep -q "list-bf-done"; then
        log_fail "image with cleared backfill_scheduled was SHOWN in list — over-broad filter"
        echo "$list_out" | sed 's/^/    /'
        return 1
    fi
    log_success "image with cleared backfill_scheduled correctly excluded from list"

    for i in "$true_img" "$prog_img" "$done_img"; do
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$i" 2>/dev/null || true
    done
}

# ============================================================================
test_post_backfill_zero_region_no_s3() {
    # Regression for the user-reported post-backfill random-read slowness.
    # ObjectBackfillRequest skips RADOS write_full + object_map update for
    # all-zero blocks (sparseness optimization).  Pre-fix, the parent's
    # read_object short-circuit was suppressed for S3-backed parents (to
    # keep cold-cache reads working), so reads of those backfilled-as-zero
    # blocks ALWAYS went aio_operate -> ENOENT -> should_read_from_s3 ->
    # S3 fetch, even though the read should have been a trivial zero-fill.
    # Under 14-VM concurrency this drove infinite refetch loops via the
    # throttler's over-cap drops.
    #
    # The fix (commit 9a6d00e5bda): after backfill_status=complete, the
    # parent's in-memory object_map is trusted -- NONEXISTENT means
    # known-zero, short-circuit to read_parent -> -ENOENT -> librbd
    # zero-fills at the child level, no S3 traffic.
    #
    # Verify by stopping MinIO AFTER backfill complete and checking the
    # child read still succeeds -- a successful read with MinIO down
    # proves no S3 fetch happened.  Pre-fix the read would EIO on the
    # zero blocks.  We use create_test_image_zero_alternating which
    # produces blocks {data, zero, data, zero, ...} so the test
    # exercises both the data-block path (RADOS hit) AND the zero-block
    # path (short-circuit) in one read.

    local parent="$POOL/zero-trust-parent"
    local child="$POOL/zero-trust-child"
    local size_mb=16   # 4 blocks: data, zero, data, zero
    local raw_file="/tmp/backfill-test-zero-trust-orig-$$.raw"
    local exp_file="/tmp/backfill-test-zero-trust-exp-$$.raw"
    local blog="/tmp/rbd-backfill-zero-trust-$$.log"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true

    # Alternating zero/data: blocks 1 and 3 are entirely zero and will be
    # skipped by ObjectBackfillRequest::write_rados.
    create_test_image_zero_alternating "$size_mb" "$raw_file"
    upload_to_s3 "$raw_file" "$S3_BUCKET" "zero-trust.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$parent" --size ${size_mb}M
    set_s3_config "$parent" "zero-trust.raw"
    create_standalone_clone "$POOL" "zero-trust-parent" "zero-trust-child"

    # Run backfill to completion -- daemon will write blocks 0,2 to RADOS
    # and SKIP blocks 1,3 (is_zero check).  backfill_status flips to
    # "complete" at the end of run_backfill.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$parent"
    run_backfill_daemon "$CEPH_CONF" "$blog"
    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "zero-trust-parent")
    # Only 2 RADOS objects expected (blocks 0 and 2; blocks 1 and 3 are zero).
    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" 2 60; then
        log_fail "backfill did not write the expected 2 non-zero objects within 60s"
        stop_backfill_daemon
        rm -f "$raw_file" "$blog"
        return 1
    fi

    # Wait for backfill_status=complete -- the trust signal for the fix.
    local status_ok=0
    for i in $(seq 1 15); do
        local s
        s=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            image-meta get "$parent" backfill_status 2>/dev/null || echo "")
        if [ "$s" = "complete" ]; then
            status_ok=1
            break
        fi
        sleep 1
    done
    stop_backfill_daemon
    if [ "$status_ok" -ne 1 ]; then
        log_fail "backfill_status never reached 'complete' within 15s"
        rm -f "$raw_file" "$blog"
        return 1
    fi

    # Stop MinIO.  Any S3 fetch the child triggers now will fail with
    # EIO / connection refused.  If the fix is broken, reading the zero
    # blocks will fail and the export below will produce mismatched data.
    stop_minio $MINIO_PORT

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$child" "$exp_file"
    local export_rc=$?

    # Restart MinIO so cleanup at the end of this test (and the broader
    # cleanup trap) can talk to it again.
    start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"
    setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

    if [ "$export_rc" -ne 0 ]; then
        log_fail "rbd export of child failed with MinIO down -- zero-region path tried S3"
        rm -f "$raw_file" "$exp_file" "$blog"
        return 1
    fi

    if ! verify_checksum "$raw_file" "$exp_file"; then
        log_fail "child export checksum mismatch with MinIO down -- zero blocks NOT served"
        rm -f "$raw_file" "$exp_file" "$blog"
        return 1
    fi
    log_success "child read of zero+data regions succeeded with MinIO down (no S3 fetch)"

    rm -f "$raw_file" "$exp_file" "$blog"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true
}

test_partial_backfill_zero_region_no_s3() {
    # Companion to test_post_backfill_zero_region_no_s3, exercising the
    # per-object backfill-visited bitmap (rbd_backfill_visited.<id>) added
    # to handle the PARTIAL backfill case.  Even when backfill_status is
    # NOT "complete" (so the whole-image s3_backfill_complete short-circuit
    # is disabled), the parent's reader should still skip S3 GETs for zero
    # blocks the daemon has already visited and marked VISITED_ZERO.
    #
    # Test strategy: run backfill to full completion (fills the bitmap),
    # then DELETE the backfill_status metadata key.  This isolates the
    # per-object bitmap path from the whole-image trust flag -- the next
    # ImageCtx open sees s3_backfill_complete=false but a fully-populated
    # bitmap.  With MinIO stopped, the export must still succeed: data
    # blocks via RADOS hit, zero blocks via bitmap short-circuit.

    local parent="$POOL/partial-trust-parent"
    local child="$POOL/partial-trust-child"
    local size_mb=16
    local raw_file="/tmp/backfill-test-partial-trust-orig-$$.raw"
    local exp_file="/tmp/backfill-test-partial-trust-exp-$$.raw"
    local blog="/tmp/rbd-backfill-partial-trust-$$.log"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true

    create_test_image_zero_alternating "$size_mb" "$raw_file"
    upload_to_s3 "$raw_file" "$S3_BUCKET" "partial-trust.raw"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$parent" --size ${size_mb}M
    set_s3_config "$parent" "partial-trust.raw"
    create_standalone_clone "$POOL" "partial-trust-parent" "partial-trust-child"

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$parent"
    run_backfill_daemon "$CEPH_CONF" "$blog"
    local prefix
    prefix=$(get_block_prefix "$CEPH_CONF" "$POOL" "partial-trust-parent")
    if ! wait_for_backfill_complete "$CEPH_CONF" "$POOL" "$prefix" 2 60; then
        log_fail "backfill did not write the expected 2 non-zero objects within 60s"
        stop_backfill_daemon
        rm -f "$raw_file" "$blog"
        return 1
    fi

    local status_ok=0
    for i in $(seq 1 15); do
        local s
        s=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            image-meta get "$parent" backfill_status 2>/dev/null || echo "")
        if [ "$s" = "complete" ]; then
            status_ok=1
            break
        fi
        sleep 1
    done
    stop_backfill_daemon
    if [ "$status_ok" -ne 1 ]; then
        log_fail "backfill_status never reached 'complete' within 15s"
        rm -f "$raw_file" "$blog"
        return 1
    fi

    # Delete backfill_status so apply_metadata sets s3_backfill_complete=false
    # on the next parent open.  The bitmap stays intact (it lives in its own
    # RADOS object, not in image metadata).  This is the partial-backfill
    # state from the reader's perspective: per-object bitmap exists, whole-
    # image trust flag does not.
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" image-meta remove "$parent" backfill_status
    local sleft
    sleft=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" \
            image-meta get "$parent" backfill_status 2>/dev/null || echo "")
    if [ -n "$sleft" ]; then
        log_fail "backfill_status still present after remove: '$sleft'"
        rm -f "$raw_file" "$blog"
        return 1
    fi

    # Stop MinIO.  Any S3 fetch the child triggers now will fail.
    stop_minio $MINIO_PORT

    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" export "$child" "$exp_file"
    local export_rc=$?

    start_minio $MINIO_PORT $MINIO_CONSOLE_PORT "$MINIO_DATA_DIR"
    setup_s3_bucket $MINIO_PORT "$S3_BUCKET"

    if [ "$export_rc" -ne 0 ]; then
        log_fail "rbd export of child failed with MinIO down and backfill_status cleared -- bitmap short-circuit did not fire"
        rm -f "$raw_file" "$exp_file" "$blog"
        return 1
    fi

    if ! verify_checksum "$raw_file" "$exp_file"; then
        log_fail "child export checksum mismatch with MinIO down (partial-backfill path)"
        rm -f "$raw_file" "$exp_file" "$blog"
        return 1
    fi
    log_success "child read via per-object bitmap (no s3_backfill_complete) succeeded with MinIO down"

    rm -f "$raw_file" "$exp_file" "$blog"
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$child"  2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$parent" 2>/dev/null || true
}

# ============================================================================
# Main
# ============================================================================

run_test "backfill_lifecycle"          test_backfill_lifecycle
run_test "backfill_data_integrity"    test_backfill_data_integrity
run_test "backfill_object_naming"     test_backfill_object_naming
run_test "backfill_cache_hit"         test_backfill_cache_hit
run_test "backfill_restart_recovery"  test_backfill_restart_recovery
run_test "backfill_no_conf_fails_fast" test_backfill_no_conf_fails_fast
run_test "backfill_foreground_stderr" test_backfill_foreground_stderr
run_test "backfill_status_transitions" test_backfill_status_transitions
run_test "backfill_status_on_failure"  test_backfill_status_on_failure
run_test "parent_du_after_backfill"         test_parent_du_after_backfill
run_test "parent_du_after_child_writeback"  test_parent_du_after_child_writeback
run_test "backfill_rescan_picks_up_new_image" test_backfill_rescan_picks_up_new_image
run_test "backfill_watcher_released_after_complete" test_backfill_watcher_released_after_complete
run_test "clone_standalone_drops_backfill_metadata" test_clone_standalone_drops_backfill_metadata
run_test "standalone_clone_rm_clears_clone_parent" test_standalone_clone_rm_clears_clone_parent
run_test "backfill_list_shows_in_progress" test_backfill_list_shows_in_progress
run_test "post_backfill_zero_region_no_s3"  test_post_backfill_zero_region_no_s3
run_test "partial_backfill_zero_region_no_s3"  test_partial_backfill_zero_region_no_s3

print_test_summary
