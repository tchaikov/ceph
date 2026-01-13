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
WORKSPACE="${WORKSPACE:-/home/kefu/dev/ceph-nautilus}"
MINIO_BIN="${MINIO_BIN:-${HOME}/dev/minio/bin}"
BUILD_DIR="${BUILD_DIR:-${WORKSPACE}/build}"
CEPH_CONF="${CEPH_CONF:-${BUILD_DIR}/ceph.conf}"

# MinIO management
start_minio() {
    local port=${1:-9000}
    local console_port=${2:-9001}
    local data_dir=${3:-/tmp/minio-test-$$}

    log_info "Starting MinIO on port $port..."

    # Kill any existing MinIO on this port
    pkill -9 -f "minio.*:$port" 2>/dev/null || true
    sleep 1

    # Create data directory
    mkdir -p "$data_dir"

    # Start MinIO
    export MINIO_ROOT_USER=minioadmin
    export MINIO_ROOT_PASSWORD=minioadmin

    "$MINIO_BIN/minio" server "$data_dir" \
        --address ":$port" \
        --console-address ":$console_port" \
        > /tmp/minio-$port.log 2>&1 &

    local minio_pid=$!
    echo $minio_pid > /tmp/minio-$port.pid

    # Wait for MinIO to be ready
    log_info "Waiting for MinIO to be ready..."
    for i in {1..30}; do
        if curl -sf "http://localhost:$port/minio/health/live" > /dev/null 2>&1; then
            log_success "MinIO started (PID: $minio_pid)"
            return 0
        fi
        if ! kill -0 $minio_pid 2>/dev/null; then
            log_error "MinIO process died"
            cat /tmp/minio-$port.log
            return 1
        fi
        sleep 1
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

    # Configure mc alias
    "$MINIO_BIN/mc" alias set local http://localhost:$port minioadmin minioadmin 2>&1 | grep -v "^mc:" || true

    # Create bucket
    "$MINIO_BIN/mc" mb "local/$bucket" 2>&1 | grep -v "^mc:" || log_info "Bucket already exists"

    # Set anonymous download policy
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

    log_info "Creating S3-backed parent: $pool/$parent_name ($size_mb MB)"

    # Create parent image file with recognizable pattern
    local temp_file="/tmp/${s3_image_name}"
    dd if=/dev/zero of="$temp_file" bs=1M count=$size_mb 2>/dev/null

    # Write block markers for verification
    for i in $(seq 0 $((size_mb/4 - 1))); do
        printf "PARENT-BLOCK-%04d" $i | dd of="$temp_file" bs=4M seek=$i conv=notrunc 2>/dev/null
    done

    # Upload to S3
    upload_to_s3 "$temp_file" "$s3_bucket" "$s3_image_name"

    # Create RBD parent image
    "$BUILD_DIR/bin/rbd" --conf "$conf" rm "$pool/$parent_name" 2>/dev/null || true
    "$BUILD_DIR/bin/rbd" --conf "$conf" create "$pool/$parent_name" --size ${size_mb}M --object-size 4M

    # Configure S3 metadata
    "$BUILD_DIR/bin/rbd" --conf "$conf" image-meta set "$pool/$parent_name" s3.enabled true
    "$BUILD_DIR/bin/rbd" --conf "$conf" image-meta set "$pool/$parent_name" s3.endpoint "$s3_endpoint"
    "$BUILD_DIR/bin/rbd" --conf "$conf" image-meta set "$pool/$parent_name" s3.bucket "$s3_bucket"
    "$BUILD_DIR/bin/rbd" --conf "$conf" image-meta set "$pool/$parent_name" s3.image_name "$s3_image_name"
    "$BUILD_DIR/bin/rbd" --conf "$conf" image-meta set "$pool/$parent_name" s3.image_format "raw"
    "$BUILD_DIR/bin/rbd" --conf "$conf" image-meta set "$pool/$parent_name" s3.verify_ssl false

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

    # Pre-populate some parent blocks by reading them
    log_info "Pre-populating first 2 objects in parent..."
    "$BUILD_DIR/bin/rbd" --conf "$conf" export "$pool/$parent" /tmp/partial-$$.raw \
        --rbd-concurrent-management-ops 1 2>&1 | head -n 5 | grep -v "Exporting" || true
    rm -f /tmp/partial-$$.raw

    # Write to child to create some child-specific data
    log_info "Writing to child to create child-specific blocks..."
    echo "CHILD-DATA" | "$BUILD_DIR/bin/rbd" --conf "$conf" bench --io-type write "$pool/$child" \
        --io-size 4096 --io-total 4096 --io-pattern seq --io-offset $((8*1024*1024)) >/dev/null 2>&1 || true

    log_success "Scenario 2 ready: Mixed blocks (parent cache + child writes + S3)"
}

setup_scenario_3_full_cache() {
    local pool=$1
    local parent=$2
    local child=$3
    local conf=${4:-$CEPH_CONF}

    log_info "Setting up Scenario 3: Full cache (all blocks in parent)"

    # Pre-populate ALL parent blocks by reading entire image
    log_info "Pre-populating all blocks in parent..."
    "$BUILD_DIR/bin/rbd" --conf "$conf" export "$pool/$parent" /tmp/full-$$.raw 2>&1 | grep -v "Exporting"
    rm -f /tmp/full-$$.raw

    log_success "Scenario 3 ready: All blocks cached, no S3 fetch needed"
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

# Test result tracking
declare -A TEST_RESULTS
declare -A TEST_TIMES

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
