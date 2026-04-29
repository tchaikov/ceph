#!/bin/bash
# Shared utilities for S3-backed RBD clone testing

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Logging functions
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_success() { echo -e "${GREEN}[✓]${NC} $1"; }
log_fail() { echo -e "${RED}[✗]${NC} $1"; }

# Default configuration
WORKSPACE="${WORKSPACE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
MINIO_BIN="${MINIO_BIN:-${HOME}/dev/minio/bin}"
BUILD_DIR="${BUILD_DIR:-${WORKSPACE}/build}"
CEPH_CONF="${CEPH_CONF:-${BUILD_DIR}/ceph.conf}"

# MinIO management
start_minio() {
    local port=${1:-9000}
    local console_port=${2:-9001}
    local data_dir=${3:-/tmp/minio-test-$$}

    log_info "Starting MinIO on port $port..."

    mkdir -p "$data_dir"

    # Start MinIO — credentials must be env vars on the server process, not just exported
    MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin \
    "$MINIO_BIN/minio" server "$data_dir" \
        --address "127.0.0.1:$port" \
        --console-address "127.0.0.1:$console_port" \
        > /tmp/minio-$port.log 2>&1 &

    local minio_pid=$!
    echo $minio_pid > /tmp/minio-$port.pid

    # Wait for MinIO to be ready using mc admin info (reliable across Minio versions)
    log_info "Waiting for MinIO to be ready..."
    for i in $(seq 1 30); do
        sleep 1
        if "$MINIO_BIN/mc" alias set local "http://127.0.0.1:$port" minioadmin minioadmin > /dev/null 2>&1 && \
           "$MINIO_BIN/mc" admin info local > /dev/null 2>&1; then
            log_success "MinIO started (PID: $minio_pid)"
            return 0
        fi
        if ! kill -0 $minio_pid 2>/dev/null; then
            log_error "MinIO process died"
            cat /tmp/minio-$port.log
            return 1
        fi
    done

    log_error "MinIO failed to start within 30 seconds"
    return 1
}

stop_minio() {
    local port=${1:-9000}

    if [ -f "/tmp/minio-$port.pid" ]; then
        local pid=$(cat /tmp/minio-$port.pid)
        if kill -0 $pid 2>/dev/null; then
            log_info "Stopping MinIO (PID: $pid)"
            kill $pid 2>/dev/null || true
            sleep 2
        fi
        rm -f /tmp/minio-$port.pid
    fi

    # Force kill any remaining MinIO processes on this port
    pkill -9 -f "minio.*:$port" 2>/dev/null || true
}

setup_s3_bucket() {
    local port=${1:-9000}
    local bucket=${2:-test-bucket}

    log_info "Setting up S3 bucket: $bucket"

    "$MINIO_BIN/mc" alias set local "http://127.0.0.1:$port" minioadmin minioadmin 2>&1 | grep -v "^mc:" || true
    "$MINIO_BIN/mc" mb "local/$bucket" 2>&1 | grep -v "^mc:" || log_info "Bucket already exists"
    "$MINIO_BIN/mc" anonymous set download "local/$bucket" 2>&1 | grep -v "^mc:"

    log_success "S3 bucket ready: $bucket"
}

upload_to_s3() {
    local file=$1
    local bucket=$2
    local key=$3
    local port=${4:-9000}

    log_info "Uploading $file to s3://$bucket/$key"
    "$MINIO_BIN/mc" cp "$file" "local/$bucket/$key" 2>&1 | grep -v "^mc:"
    log_success "Upload complete"
}

# Ceph cluster helpers
check_cluster_running() {
    local conf=${1:-$CEPH_CONF}

    if [ ! -f "$conf" ]; then
        log_error "Ceph config not found: $conf"
        log_error "Please start cluster first:"
        log_error "  cd $BUILD_DIR && MON=1 OSD=3 MDS=0 MGR=1 RGW=0 ../src/vstart.sh -n -d --without-dashboard"
        return 1
    fi

    if ! "$BUILD_DIR/bin/ceph" --conf "$conf" status >/dev/null 2>&1; then
        log_error "Ceph cluster not running"
        return 1
    fi

    log_success "Ceph cluster is running"
    return 0
}

create_pool() {
    local pool=$1
    local conf=${2:-$CEPH_CONF}

    log_info "Creating pool: $pool"
    "$BUILD_DIR/bin/ceph" --conf "$conf" osd pool create "$pool" 8 2>&1 | grep -v "successfully created" || true
    "$BUILD_DIR/bin/ceph" --conf "$conf" osd pool application enable "$pool" rbd 2>&1 || true
    log_success "Pool ready: $pool"
}

enable_s3_fetch() {
    local conf=${1:-$CEPH_CONF}

    if ! grep -q "rbd_s3_fetch_enabled" "$conf"; then
        log_info "Enabling S3 fetch in config"
        echo "rbd_s3_fetch_enabled = true" >> "$conf"
    fi
}

# Image creation
create_s3_parent() {
    local pool=$1
    local parent_name=$2
    local s3_image_name=$3
    local size_mb=$4
    local s3_endpoint=${5:-http://localhost:9000}
    local s3_bucket=${6:-test-bucket}
    local conf=${7:-$CEPH_CONF}
    local s3_access_key=${8:-minioadmin}
    local s3_secret_key=${9:-minioadmin}

    log_info "Creating S3-backed parent: $pool/$parent_name ($size_mb MB)"

    # Create parent image file with recognizable pattern
    local temp_file="/tmp/${s3_image_name}"
    create_test_image_with_pattern "$size_mb" "$temp_file"

    # Upload to S3
    upload_to_s3 "$temp_file" "$s3_bucket" "$s3_image_name"

    # Create RBD parent image
    "$BUILD_DIR/bin/rbd" --conf "$conf" rm "$pool/$parent_name" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$conf" create "$pool/$parent_name" --size ${size_mb}M --object-size 4M

    # Configure S3 via s3-config set (sets s3.enabled, base64-encodes secret key, uses correct key names)
    "$BUILD_DIR/bin/rbd" --conf "$conf" s3-config set "$pool/$parent_name" \
        --s3-endpoint   "$s3_endpoint" \
        --s3-bucket     "$s3_bucket" \
        --s3-image-name "$s3_image_name" \
        --s3-access-key "$s3_access_key" \
        --s3-secret-key "$s3_secret_key"

    log_success "S3-backed parent created: $pool/$parent_name"
}

create_standalone_clone() {
    local pool=$1
    local parent=$2
    local child=$3
    local conf=${4:-$CEPH_CONF}

    log_info "Creating standalone clone: $pool/$child from $pool/$parent"
    "$BUILD_DIR/bin/rbd" --conf "$conf" rm "$pool/$child" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$conf" clone-standalone "$pool/$parent" "$pool/$child"
    log_success "Standalone clone created: $pool/$child"
}

create_cross_cluster_clone() {
    local local_pool=$1
    local child_name=$2
    local remote_pool=$3
    local parent_name=$4
    local remote_mon_host=$5
    local s3_endpoint=$6
    local s3_bucket=$7
    local s3_image_name=$8
    local size=$9
    local local_conf=${10:-$CEPH_CONF}

    log_info "Creating cross-cluster clone: $local_pool/$child_name"

    # Create child image
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" create "$local_pool/$child_name" --size "$size"

    # Set parent metadata
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.pool "$remote_pool"
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.image "$parent_name"
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.snap -
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.type remote_standalone
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.cluster remote
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.mon_host "$remote_mon_host"

    # Copy S3 configuration
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.s3.enabled true
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.s3.endpoint "$s3_endpoint"
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.s3.bucket "$s3_bucket"
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.s3.image_name "$s3_image_name"
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.s3.image_format "raw"
    "$BUILD_DIR/bin/rbd" --conf "$local_conf" image-meta set "$local_pool/$child_name" parent.s3.verify_ssl false

    log_success "Cross-cluster clone created: $local_pool/$child_name"
}

# Scenario setup functions
setup_scenario_1_clean_slate() {
    local pool=$1
    local parent=$2
    local child=$3
    local conf=${4:-$CEPH_CONF}

    log_info "Setting up Scenario 1: Clean Slate (no blocks cached)"
    # Nothing to do - parent and child are already empty
    log_success "Scenario 1 ready: All blocks must come from S3"
}

setup_scenario_2_partial_blocks() {
    local pool=$1
    local parent=$2
    local child=$3
    local conf=${4:-$CEPH_CONF}

    log_info "Setting up Scenario 2: Partial blocks cached"

    # Pre-populate the first object in the parent via a child COW write at
    # offset 0.  S3 fetches only fire through CopyupRequest (the child COW
    # path) — exporting the parent directly reads sparse RADOS objects as
    # zeros and never touches S3 or populates the parent cache.
    log_info "Pre-populating first parent object via child COW..."
    "$BUILD_DIR/bin/rbd" --conf "$conf" bench --io-type write "$pool/$child" \
        --io-size 4096 --io-total 4096 --io-pattern seq --io-offset 0 >/dev/null 2>&1 || true
    # Give the async parent write-back time to complete before we proceed
    sleep 1

    log_success "Scenario 2 ready: First parent object cached, remaining must come from S3"
}

setup_scenario_3_full_cache() {
    local pool=$1
    local parent=$2
    local child=$3
    local conf=${4:-$CEPH_CONF}

    log_info "Setting up Scenario 3: Full cache (all blocks in parent via backfill daemon)"

    # The backfill daemon is the correct way to pre-warm all parent RADOS
    # objects.  rbd export of the parent reads sparse objects as zeros and
    # never fetches from S3; only CopyupRequest (child COW) and the backfill
    # daemon actually populate the parent cache.
    local prefix
    prefix=$(get_block_prefix "$conf" "$pool" "$parent")
    local num_objects
    num_objects=$("$BUILD_DIR/bin/rbd" --conf "$conf" info "$pool/$parent" --format json 2>/dev/null \
        | python3 -c "import sys,json; d=json.load(sys.stdin); print((d['size']+d['object_size']-1)//d['object_size'])" 2>/dev/null || echo "5")

    log_info "Scheduling backfill for $pool/$parent ($num_objects objects)..."
    "$BUILD_DIR/bin/rbd" --conf "$conf" backfill schedule "$pool/$parent" 2>&1 | grep -v "WARNING\|developer" || true

    local blog="/tmp/scenario3-backfill-$$.log"
    run_backfill_daemon "$conf" "$blog"

    if ! wait_for_backfill_complete "$conf" "$pool" "$prefix" "$num_objects" 60; then
        stop_backfill_daemon
        log_fail "Scenario 3 setup: backfill did not complete within 60s"
        return 1
    fi
    stop_backfill_daemon
    rm -f "$blog"

    log_success "Scenario 3 ready: All $num_objects parent objects cached in RADOS, no S3 fetch needed"
}

# Verification functions
verify_data_integrity() {
    local pool=$1
    local image=$2
    local expected_file=$3
    local conf=${4:-$CEPH_CONF}

    log_info "Verifying data integrity for $pool/$image..."

    local export_file="/tmp/verify-${image}-$$.raw"
    "$BUILD_DIR/bin/rbd" --conf "$conf" export "$pool/$image" "$export_file" 2>&1 | grep -v "Exporting"

    if cmp -s "$export_file" "$expected_file"; then
        log_success "Data integrity verified!"
        rm -f "$export_file"
        return 0
    else
        log_fail "Data mismatch!"
        log_error "Expected: $expected_file"
        log_error "Got: $export_file"
        return 1
    fi
}

verify_parent_removed() {
    local pool=$1
    local image=$2
    local conf=${3:-$CEPH_CONF}

    log_info "Verifying parent reference removed from $pool/$image..."

    if "$BUILD_DIR/bin/rbd" --conf "$conf" info "$pool/$image" | grep -q "parent:"; then
        log_fail "Parent reference still exists!"
        return 1
    else
        log_success "Parent reference removed"
        return 0
    fi
}

verify_s3_independence() {
    local pool=$1
    local image=$2
    local conf=${3:-$CEPH_CONF}

    log_info "Verifying $pool/$image is independent of S3..."

    # Try to export without S3 (assumes MinIO is stopped)
    local export_file="/tmp/verify-independent-${image}-$$.raw"
    if "$BUILD_DIR/bin/rbd" --conf "$conf" export "$pool/$image" "$export_file" 2>&1 | grep -v "Exporting"; then
        log_success "Image is independent of S3!"
        rm -f "$export_file"
        return 0
    else
        log_fail "Image still depends on S3!"
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Backfill daemon helpers
# ---------------------------------------------------------------------------

BACKFILL_PID=""

# Start the rbd-backfill daemon in foreground mode; sets BACKFILL_PID.
# Usage: run_backfill_daemon <conf> [log_file]
run_backfill_daemon() {
    local conf=${1:-$CEPH_CONF}
    local log_file=${2:-/tmp/rbd-backfill-$$.log}

    log_info "Starting rbd-backfill daemon (conf: $conf)..."
    "$BUILD_DIR/bin/rbd-backfill" --conf "$conf" --foreground > "$log_file" 2>&1 &
    BACKFILL_PID=$!

    sleep 2
    if ! kill -0 $BACKFILL_PID 2>/dev/null; then
        log_error "rbd-backfill daemon failed to start"
        cat "$log_file"
        return 1
    fi
    log_success "rbd-backfill started (PID: $BACKFILL_PID, log: $log_file)"
}

# Stop the backfill daemon gracefully.
stop_backfill_daemon() {
    if [ -n "$BACKFILL_PID" ] && kill -0 $BACKFILL_PID 2>/dev/null; then
        log_info "Stopping rbd-backfill (PID: $BACKFILL_PID)..."
        kill $BACKFILL_PID 2>/dev/null || true
        # Wait up to 10s for graceful exit; fall back to SIGKILL.  Without this
        # cap a daemon-side teardown bug could hang the test harness for >20
        # minutes (see #35: librbd ManagedLock complete_shutdown vs rados
        # shutdown ordering race surfaced as a Mutex::lock assertion).
        local deadline=$(( $(date +%s) + 10 ))
        while kill -0 "$BACKFILL_PID" 2>/dev/null; do
            if [ $(date +%s) -ge $deadline ]; then
                log_warn "rbd-backfill did not exit within 10s of SIGTERM; sending SIGKILL"
                kill -9 "$BACKFILL_PID" 2>/dev/null || true
                break
            fi
            sleep 0.5
        done
        wait $BACKFILL_PID 2>/dev/null || true
    fi
    BACKFILL_PID=""
}

# Poll RADOS until at least $expected_count objects with $prefix exist, or timeout.
# Usage: wait_for_backfill_complete <conf> <pool> <prefix> <expected_count> [timeout_secs]
wait_for_backfill_complete() {
    local conf=$1
    local pool=$2
    local prefix=$3
    local expected=$4
    local timeout=${5:-60}

    log_info "Waiting up to ${timeout}s for $expected objects with prefix $prefix..."
    local count=0
    for i in $(seq 1 $timeout); do
        count=$(count_rados_objects "$conf" "$pool" "$prefix")
        if [ "$count" -ge "$expected" ]; then
            log_success "Backfill complete: $count/$expected objects present (${i}s)"
            return 0
        fi
        sleep 1
    done
    log_fail "Backfill timeout: only $count/$expected objects after ${timeout}s"
    return 1
}

# ---------------------------------------------------------------------------
# Object/image introspection helpers
# ---------------------------------------------------------------------------

# Extract block_name_prefix from `rbd info` output.
# Usage: get_block_prefix <conf> <pool> <image>
get_block_prefix() {
    local conf=$1
    local pool=$2
    local image=$3
    "$BUILD_DIR/bin/rbd" --conf "$conf" info "$pool/$image" 2>/dev/null \
        | awk '/block_name_prefix:/ {print $2}'
}

# Count RADOS objects whose name starts with "<prefix>.".
# Usage: count_rados_objects <conf> <pool> <prefix>
count_rados_objects() {
    local conf=$1
    local pool=$2
    local prefix=$3
    "$BUILD_DIR/bin/rados" --conf "$conf" -p "$pool" ls 2>/dev/null \
        | grep -c "^${prefix}\." 2>/dev/null || echo "0"
}

# ---------------------------------------------------------------------------
# Data helpers
# ---------------------------------------------------------------------------

# Create a raw image file of <size_mb> MB with a recognizable block pattern
# (each 4 MB chunk starts with "PARENT-BLOCK-NNNN"). Writes to <out_file>.
# Usage: create_test_image_with_pattern <size_mb> <out_file>
create_test_image_with_pattern() {
    local size_mb=$1
    local out_file=$2

    dd if=/dev/zero of="$out_file" bs=1M count="$size_mb" status=none
    local num_blocks=$((size_mb / 4))
    for i in $(seq 0 $((num_blocks - 1))); do
        printf "PARENT-BLOCK-%04d" $i | dd of="$out_file" bs=4M seek=$i conv=notrunc status=none
    done
}

# Create a raw image where 4 MB blocks alternate between pattern and all-zero.
# Even-numbered blocks (0, 2, 4, ...) get "PARENT-BLOCK-NNNN" header; odd-numbered
# blocks remain all-zero.  Used to test zero-object handling consistency across
# the daemon, read, and write S3 fetch paths.
# Usage: create_test_image_zero_alternating <size_mb> <out_file>
create_test_image_zero_alternating() {
    local size_mb=$1
    local out_file=$2

    dd if=/dev/zero of="$out_file" bs=1M count="$size_mb" status=none
    local num_blocks=$((size_mb / 4))
    for i in $(seq 0 $((num_blocks - 1))); do
        if [ $((i % 2)) -eq 0 ]; then
            printf "PARENT-BLOCK-%04d" $i | dd of="$out_file" bs=4M seek=$i conv=notrunc status=none
        fi
    done
}

# Create a sparse raw image: only the first <num_filled> 4 MB blocks contain a
# pattern; the rest are zero.  Used to test that empty tail blocks don't trigger
# unnecessary S3 round-trips.
# Usage: create_test_image_sparse <size_mb> <num_filled_blocks> <out_file>
create_test_image_sparse() {
    local size_mb=$1
    local num_filled=$2
    local out_file=$3

    dd if=/dev/zero of="$out_file" bs=1M count="$size_mb" status=none
    local i
    for i in $(seq 0 $((num_filled - 1))); do
        printf "PARENT-BLOCK-%04d" $i | dd of="$out_file" bs=4M seek=$i conv=notrunc status=none
    done
}

# Create a raw image where block 0 is all-zero and blocks 1..N have a pattern.
# Lets tests that probe "zero block handling" target the well-known offset 0
# (rbd bench has no --io-offset, so we have to put the zero block first).
# Usage: create_test_image_zero_first <size_mb> <out_file>
create_test_image_zero_first() {
    local size_mb=$1
    local out_file=$2

    dd if=/dev/zero of="$out_file" bs=1M count="$size_mb" status=none
    local num_blocks=$((size_mb / 4))
    local i
    for i in $(seq 1 $((num_blocks - 1))); do
        printf "PARENT-BLOCK-%04d" $i | dd of="$out_file" bs=4M seek=$i conv=notrunc status=none
    done
}

# Verify two files are byte-for-byte identical (md5sum comparison).
# Usage: verify_checksum <file_a> <file_b>
verify_checksum() {
    local file_a=$1
    local file_b=$2

    local sum_a sum_b
    sum_a=$(md5sum "$file_a" | awk '{print $1}')
    sum_b=$(md5sum "$file_b" | awk '{print $1}')

    if [ "$sum_a" = "$sum_b" ]; then
        log_success "Checksum match: $sum_a"
        return 0
    else
        log_fail "Checksum mismatch: $sum_a vs $sum_b"
        return 1
    fi
}

# ---------------------------------------------------------------------------
# Test result tracking
# ---------------------------------------------------------------------------

# Parse the standard --conf flag shared by most s3 tests.  Sets CEPH_CONF.
# Usage: parse_common_args "$@"
parse_common_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --conf) CEPH_CONF="$2"; shift 2 ;;
            *) shift ;;
        esac
    done
}

# Wait up to <timeout_s> for all listed pids; SIGTERMs any that overrun.
# Echoes the count of timed-out pids so the caller can fail the test.
# Usage: failures=$(wait_pids_with_timeout <timeout_s> <pid1> <pid2> ...)
wait_pids_with_timeout() {
    local timeout=$1; shift
    local deadline=$(( $(date +%s) + timeout ))
    local failed=0 pid
    for pid in "$@"; do
        while kill -0 "$pid" 2>/dev/null; do
            if [ $(date +%s) -ge $deadline ]; then
                kill "$pid" 2>/dev/null || true
                failed=$((failed + 1))
                break
            fi
            sleep 0.5
        done
        wait "$pid" 2>/dev/null || true
    done
    echo "$failed"
}

# Echo the integer used_size (bytes) reported by `rbd du --format json`,
# or 0 if the field is missing (e.g. rbd is too old or image is unknown).
# Usage: bytes=$(get_image_used_size <conf> <pool>/<image>)
get_image_used_size() {
    local conf=$1
    local img=$2
    local v
    v=$("$BUILD_DIR/bin/rbd" --conf "$conf" du "$img" --format json 2>/dev/null \
        | grep -o '"used_size":[0-9]*' | head -1 | cut -d: -f2)
    echo "${v:-0}"
}

declare -A TEST_RESULTS
declare -A TEST_TIMES

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
    [[ "$result" == "PASSED" ]]
}

record_test_result() {
    local test_name=$1
    local result=$2  # "PASSED" or "FAILED"
    local duration=$3

    TEST_RESULTS["$test_name"]=$result
    TEST_TIMES["$test_name"]=$duration
}

print_test_summary() {
    local total=0
    local passed=0
    local failed=0

    echo
    echo "=========================================="
    echo "           TEST SUMMARY"
    echo "=========================================="
    echo

    for test_name in "${!TEST_RESULTS[@]}"; do
        total=$((total + 1))
        local result=${TEST_RESULTS[$test_name]}
        local duration=${TEST_TIMES[$test_name]}

        if [ "$result" == "PASSED" ]; then
            passed=$((passed + 1))
            log_success "$test_name: PASSED (${duration}s)"
        else
            failed=$((failed + 1))
            log_fail "$test_name: FAILED (${duration}s)"
        fi
    done

    echo
    echo "=========================================="
    if [ $failed -eq 0 ]; then
        log_success "ALL $total TESTS PASSED!"
    else
        log_error "$failed of $total tests FAILED"
    fi
    echo "=========================================="
    echo

    return $failed
}
