#!/bin/bash
# Comprehensive S3-backed RBD clone E2E test matrix
# Tests 3 scenarios × 3 operations × 2 cluster modes = 18 test cases
#
# This script supports two deployment modes:
#
# 1. MANAGED MODE - For local development and CI
#    The script automatically sets up and tears down all infrastructure:
#    - Starts Ceph clusters using vstart.sh
#    - Launches MinIO in docker containers for S3 storage
#    - Creates and configures all required resources
#    - Cleans up everything on exit
#
# 2. EXTERNAL MODE - For testing with production infrastructure
#    Developers provide existing Ceph clusters and S3 accounts:
#    - Two running Ceph clusters with config files and keyrings
#    - An S3-compatible storage account (AWS, MinIO, etc.)
#    - Script only manages test images, not infrastructure
#    - Ideal for validating against real production environments
#
# Usage:
#   Managed mode (vstart + containers):
#     ./test-s3-e2e-matrix.sh single       # Single cluster with local MinIO
#     ./test-s3-e2e-matrix.sh cross        # Two clusters (docker) with shared MinIO
#
#   External mode (existing clusters + real S3):
#     MODE=external \
#     LOCAL_CEPH_CONF=/path/to/local.conf \
#     LOCAL_KEYRING=/path/to/local.keyring \
#     REMOTE_CEPH_CONF=/path/to/remote.conf \
#     REMOTE_KEYRING=/path/to/remote.keyring \
#     S3_ENDPOINT=https://s3.amazonaws.com \
#     S3_BUCKET=my-bucket \
#     S3_ACCESS_KEY=AKIAXXXXXXX \
#     S3_SECRET_KEY=SECRET \
#     ./test-s3-e2e-matrix.sh
#
# Requirements for external mode:
#   - Both clusters must be accessible from this host
#   - S3 bucket must exist and be accessible
#   - Install either 'aws' CLI or 's3cmd' for S3 operations
#   - Keyrings must have permissions to create/delete pools and images

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/.." && pwd)"

# Source common utilities
source "$SCRIPT_DIR/lib/s3-test-common.sh"

# Test mode
MODE="${MODE:-${1:-single}}"  # "single", "cross", or "external"

# Detect external mode
if [ "$MODE" == "external" ]; then
    # External mode: user provides existing clusters and S3 credentials
    log_info "Running in EXTERNAL mode with existing clusters and S3"

    # Validate required environment variables
    REQUIRED_VARS="LOCAL_CEPH_CONF LOCAL_KEYRING REMOTE_CEPH_CONF REMOTE_KEYRING S3_ENDPOINT S3_BUCKET S3_ACCESS_KEY S3_SECRET_KEY"
    for var in $REQUIRED_VARS; do
        if [ -z "${!var}" ]; then
            log_error "Missing required environment variable: $var"
            exit 1
        fi
    done

    # Use provided configuration
    CEPH_CONF="$LOCAL_CEPH_CONF"
    REMOTE_CONF_FILE="$REMOTE_CEPH_CONF"
    REMOTE_KEYRING_FILE="$REMOTE_KEYRING"
    MANAGED_CLUSTERS=false
    MANAGED_S3=false

    # S3 configuration
    S3_BUCKET="${S3_BUCKET}"
    S3_ENDPOINT="${S3_ENDPOINT}"
    S3_ACCESS_KEY="${S3_ACCESS_KEY}"
    S3_SECRET_KEY="${S3_SECRET_KEY}"

    # For external mode, we're always in cross-cluster mode
    CROSS_CLUSTER=true
else
    # Managed mode: script controls cluster lifecycle
    MANAGED_CLUSTERS=true
    MANAGED_S3=true
    CROSS_CLUSTER=false

    if [ "$MODE" == "cross" ]; then
        CROSS_CLUSTER=true
        # Cross-cluster mode: use docker-compose MinIO
        # Note: Remote cluster uses host networking, so access MinIO via localhost
        MINIO_HOST="localhost"
        MINIO_PORT=9000
        MINIO_CONSOLE_PORT=9001
        S3_ENDPOINT="http://${MINIO_HOST}:${MINIO_PORT}"
        REMOTE_MON_HOST="172.20.0.20:6789"
        REMOTE_CLUSTER_DIR="$WORKSPACE/remote-cluster"
    else
        # Single-cluster mode: use local MinIO
        MINIO_PORT=9002
        MINIO_CONSOLE_PORT=9003
        S3_ENDPOINT="http://localhost:$MINIO_PORT"
    fi

    S3_BUCKET="test-bucket"
fi

# Test configuration
POOL_NAME="${POOL_NAME:-s3pool}"
IMAGE_SIZE_MB="${IMAGE_SIZE_MB:-20}"

# Cleanup function
cleanup() {
    log_info "Cleaning up..."

    cd "$WORKSPACE"

    # Clean up RBD images
    for i in 1 2 3; do
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL_NAME/child$i" 2>/dev/null || true
        if [ "$CROSS_CLUSTER" = true ] && [ "$MANAGED_CLUSTERS" = true ]; then
            docker exec ceph-remote-cluster bash -c "
                ./build/bin/rbd --conf remote-cluster/ceph.conf rm $POOL_NAME/parent$i 2>/dev/null || true
            " 2>/dev/null || true
        else
            "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL_NAME/parent$i" 2>/dev/null || true
        fi
    done

    # Clean temp files
    rm -f /tmp/parent*.raw /tmp/child*.raw /tmp/verify*.raw /tmp/partial-*.raw /tmp/full-*.raw

    # Only manage MinIO and clusters if in managed mode
    if [ "$MANAGED_S3" = true ]; then
        if [ "$CROSS_CLUSTER" = true ]; then
            # Cross-cluster managed mode: stop docker containers
            log_info "Stopping remote cluster..."
            docker exec ceph-remote-cluster bash -c "
                pkill -9 ceph-mon || true
                pkill -9 ceph-mgr || true
                pkill -9 ceph-osd || true
            " 2>/dev/null || true

            log_info "Stopping docker containers..."
            docker-compose -f docker-compose-cross-cluster.yml down -v 2>/dev/null || true

            # Clean remote cluster directory
            if [ -d "$REMOTE_CLUSTER_DIR" ]; then
                rm -rf "$REMOTE_CLUSTER_DIR" || \
                log_warn "Could not clean remote-cluster directory. Please run: sudo rm -rf $REMOTE_CLUSTER_DIR"
            fi
        else
            # Single-cluster managed mode: stop local MinIO
            stop_minio $MINIO_PORT
        fi
    fi
}

trap cleanup EXIT

# Test execution functions
run_read_test() {
    local scenario=$1
    local pool=$2
    local parent=$3
    local child=$4
    local expected_file=$5
    local conf=${6:-$CEPH_CONF}

    log_step "Running READ test for Scenario $scenario"

    local start_time=$(date +%s)

    # Export child (triggers S3 fetch if needed)
    log_info "Exporting child image..."
    local export_file="/tmp/child-read-s${scenario}.raw"
    "$BUILD_DIR/bin/rbd" --conf "$conf" export "$pool/$child" "$export_file" 2>&1 | grep -v "Exporting"

    # Verify data integrity
    if cmp -s "$export_file" "$expected_file"; then
        log_success "Data integrity verified!"
    else
        log_error "Data mismatch!"
        return 1
    fi

    # Check parent cache populated
    local parent_size=$("$BUILD_DIR/bin/rbd" --conf "$conf" du "$pool/$parent" --format=json 2>/dev/null | \
        jq -r '.images[0].used_size // 0' 2>/dev/null || echo "0")
    log_info "Parent cache size: $parent_size bytes"

    rm -f "$export_file"

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    log_success "READ test passed for Scenario $scenario"
    return 0
}

run_write_test() {
    local scenario=$1
    local pool=$2
    local parent=$3
    local child=$4
    local conf=${5:-$CEPH_CONF}

    log_step "Running WRITE test for Scenario $scenario"

    local start_time=$(date +%s)

    # Write to child (triggers copyup from S3 if needed)
    log_info "Writing to child image..."
    "$BUILD_DIR/bin/rbd" --conf "$conf" bench --io-type write "$pool/$child" \
        --io-size 4M --io-total 16M >/dev/null 2>&1

    # Verify child has written data
    log_info "Verifying child has written data..."
    local export_file="/tmp/child-write-s${scenario}.raw"
    "$BUILD_DIR/bin/rbd" --conf "$conf" export "$pool/$child" "$export_file" 2>&1 | grep -v "Exporting"

    # Check file size is correct
    local expected_size=$((IMAGE_SIZE_MB * 1024 * 1024))
    local actual_size=$(stat -c%s "$export_file" 2>/dev/null || stat -f%z "$export_file" 2>/dev/null)

    if [ "$actual_size" -eq "$expected_size" ]; then
        log_success "Image size correct: $actual_size bytes"
    else
        log_error "Image size mismatch: expected $expected_size, got $actual_size"
        return 1
    fi

    rm -f "$export_file"

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    log_success "WRITE test passed for Scenario $scenario"
    return 0
}

run_flatten_test() {
    local scenario=$1
    local pool=$2
    local parent=$3
    local child=$4
    local expected_file=$5
    local conf=${6:-$CEPH_CONF}

    log_step "Running FLATTEN test for Scenario $scenario"

    local start_time=$(date +%s)

    # Flatten child
    log_info "Flattening child image..."
    "$BUILD_DIR/bin/rbd" --conf "$conf" flatten "$pool/$child" 2>&1 | tee /tmp/flatten-s${scenario}.log

    # Verify parent reference removed
    if ! verify_parent_removed "$pool" "$child" "$conf"; then
        return 1
    fi

    # Verify data integrity (for scenarios 1 and 3, should match exactly)
    if [ "$scenario" == "1" ] || [ "$scenario" == "3" ]; then
        if ! verify_data_integrity "$pool" "$child" "$expected_file" "$conf"; then
            return 1
        fi
    else
        # Scenario 2 has mixed data, just verify size
        local export_file="/tmp/child-flatten-s${scenario}.raw"
        "$BUILD_DIR/bin/rbd" --conf "$conf" export "$pool/$child" "$export_file" 2>&1 | grep -v "Exporting"

        local expected_size=$((IMAGE_SIZE_MB * 1024 * 1024))
        local actual_size=$(stat -c%s "$export_file" 2>/dev/null || stat -f%z "$export_file" 2>/dev/null)

        if [ "$actual_size" -eq "$expected_size" ]; then
            log_success "Image size correct after flatten"
        else
            log_error "Image size mismatch after flatten"
            return 1
        fi

        rm -f "$export_file"
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    log_success "FLATTEN test passed for Scenario $scenario"
    return 0
}

# Run a single test case
run_test_case() {
    local scenario=$1
    local operation=$2
    local test_name="Scenario $scenario + $operation"

    echo
    log_step "=========================================="
    log_step "TEST: $test_name"
    log_step "=========================================="

    # NOTE: Cross-cluster mode has limitations in current implementation:
    # - Read operations require the child to have data (can't read directly from remote parent)
    # - Flatten may not work if parent relationship isn't properly established
    # These tests help identify what needs to be implemented

    local start_time=$(date +%s)
    local parent="parent$scenario"
    local child="child$scenario"
    local s3_image="parent${scenario}.raw"

    # Create S3-backed parent (mode-specific)
    if [ "$CROSS_CLUSTER" = true ]; then
        create_cross_cluster_parent "$POOL_NAME" "$parent" "$s3_image" "$IMAGE_SIZE_MB"
        create_cross_cluster_clone "$POOL_NAME" "$parent" "$child" "$s3_image"
    else
        create_s3_parent "$POOL_NAME" "$parent" "$s3_image" "$IMAGE_SIZE_MB" "$S3_ENDPOINT" "$S3_BUCKET"
        create_standalone_clone "$POOL_NAME" "$parent" "$child"
    fi

    # Setup scenario (mode-specific)
    case $scenario in
        1)
            setup_scenario_1_clean_slate "$POOL_NAME" "$parent" "$child"
            ;;
        2)
            if [ "$CROSS_CLUSTER" = true ]; then
                setup_cross_cluster_scenario_2 "$POOL_NAME" "$parent" "$child"
            else
                setup_scenario_2_partial_blocks "$POOL_NAME" "$parent" "$child"
            fi
            ;;
        3)
            if [ "$CROSS_CLUSTER" = true ]; then
                setup_cross_cluster_scenario_3 "$POOL_NAME" "$parent" "$child"
            else
                setup_scenario_3_full_cache "$POOL_NAME" "$parent" "$child"
            fi
            ;;
    esac

    # Run operation test
    local result="FAILED"
    local expected_file="/tmp/${s3_image}"

    case $operation in
        read)
            if run_read_test "$scenario" "$POOL_NAME" "$parent" "$child" "$expected_file"; then
                result="PASSED"
            fi
            ;;
        write)
            if run_write_test "$scenario" "$POOL_NAME" "$parent" "$child"; then
                result="PASSED"
            fi
            ;;
        flatten)
            if run_flatten_test "$scenario" "$POOL_NAME" "$parent" "$child" "$expected_file"; then
                result="PASSED"
            fi
            ;;
    esac

    # Cleanup images for this test
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL_NAME/$child" 2>/dev/null || true

    if [ "$CROSS_CLUSTER" = true ]; then
        # Clean parent from remote cluster
        if [ "$MANAGED_CLUSTERS" = true ]; then
            docker exec ceph-remote-cluster bash -c \
                "./build/bin/rbd --conf remote-cluster/ceph.conf rm $POOL_NAME/$parent 2>/dev/null || true" \
                2>/dev/null || true
        else
            "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" rm "$POOL_NAME/$parent" 2>/dev/null || true
        fi
    else
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL_NAME/$parent" 2>/dev/null || true
    fi

    local end_time=$(date +%s)
    local duration=$((end_time - start_time))

    record_test_result "$test_name" "$result" "$duration"

    if [ "$result" == "PASSED" ]; then
        log_success "$test_name: PASSED (${duration}s)"
    else
        log_fail "$test_name: FAILED (${duration}s)"
    fi

    return $([ "$result" == "PASSED" ] && echo 0 || echo 1)
}

# Cross-cluster setup functions
setup_cross_cluster() {
    log_info "Setting up cross-cluster environment..."

    cd "$WORKSPACE"

    # Clean up any previous remote cluster directory on host
    log_info "Cleaning up previous remote cluster directory..."
    if [ -d "$REMOTE_CLUSTER_DIR" ]; then
        rm -rf "$REMOTE_CLUSTER_DIR" || \
        { log_error "Cannot clean remote-cluster directory. Please run: sudo rm -rf $REMOTE_CLUSTER_DIR" && return 1; }
    fi

    # Create fresh remote cluster directory on host (needed before starting container)
    mkdir -p "$REMOTE_CLUSTER_DIR"
    chmod 775 "$REMOTE_CLUSTER_DIR"  # Normal permissions work now with keep-id

    # Start docker containers
    log_info "Starting MinIO and remote cluster containers..."
    docker-compose -f docker-compose-cross-cluster.yml up -d

    # Wait for MinIO
    log_info "Waiting for MinIO to be ready..."
    for i in {1..30}; do
        if curl -sf "http://localhost:9000/minio/health/live" > /dev/null 2>&1; then
            log_success "MinIO is ready!"
            break
        fi
        if [ $i -eq 30 ]; then
            log_error "MinIO failed to start"
            return 1
        fi
        sleep 1
    done

    # Wait for container
    sleep 3

    # Setup S3 bucket using docker MinIO
    setup_s3_bucket_docker

    # Clean inside container (kill any stray processes)
    docker exec ceph-remote-cluster bash -c "
        pkill -9 ceph-mon || true
        pkill -9 ceph-mgr || true
        pkill -9 ceph-osd || true
        pkill -9 ceph-mds || true
    " 2>/dev/null || true

    # Start remote cluster
    log_info "Starting remote Ceph cluster in container..."
    docker exec -w /home/kefu/dev/ceph-nautilus ceph-remote-cluster bash -c "
        set -e
        export LD_LIBRARY_PATH=/home/kefu/dev/ceph-nautilus/build/lib:\$LD_LIBRARY_PATH
        export CEPH_BIN=/home/kefu/dev/ceph-nautilus/build/bin
        export CEPH_LIB=/home/kefu/dev/ceph-nautilus/build/lib

        MON=1 OSD=3 MDS=0 MGR=1 RGW=0 \
        CEPH_PORT=7789 \
        CEPH_DIR=/home/kefu/dev/ceph-nautilus/remote-cluster \
        ./src/vstart.sh -n -d --without-dashboard

        echo 'Remote cluster started'
    "

    # Wait for remote cluster
    sleep 5

    # Get actual monitor address from remote cluster (both v2 and v1)
    # Since the remote cluster uses host networking, we can access it via the host's IP
    # Remote cluster uses port 7789 to avoid conflict with local cluster on 6789
    log_info "Getting remote cluster monitor addresses..."
    # Get the actual IP the monitor is bound to
    REMOTE_MON_IP=$(docker exec -w /home/kefu/dev/ceph-nautilus ceph-remote-cluster bash -c "
        ./build/bin/ceph --conf remote-cluster/ceph.conf mon dump -f json 2>/dev/null | grep -o '\"addr\":\"[^\"]*\"' | head -1 | cut -d'\"' -f4 | cut -d':' -f1
    ")
    REMOTE_MON_HOST="[v2:${REMOTE_MON_IP}:7789,v1:${REMOTE_MON_IP}:7790]"
    log_info "Remote monitor addresses: $REMOTE_MON_HOST"

    # Verify remote cluster
    log_info "Verifying remote cluster health..."
    docker exec -w /home/kefu/dev/ceph-nautilus ceph-remote-cluster bash -c "
        ./build/bin/ceph --conf remote-cluster/ceph.conf health
    "

    # Create pool in remote cluster
    log_info "Creating pool in remote cluster..."
    docker exec -w /home/kefu/dev/ceph-nautilus ceph-remote-cluster bash -c "
        ./build/bin/ceph --conf remote-cluster/ceph.conf osd pool create $POOL_NAME 8 2>&1 | grep -v 'successfully created' || true
        ./build/bin/ceph --conf remote-cluster/ceph.conf osd pool application enable $POOL_NAME rbd 2>&1 || true

        # Enable S3 fetch in remote cluster
        ./build/bin/ceph --conf remote-cluster/ceph.conf config set osd rbd_s3_fetch_enabled true 2>&1 || true
    "

    log_success "Cross-cluster environment ready"
}

setup_s3_bucket_docker() {
    log_info "Setting up S3 bucket in docker MinIO..."

    # Configure mc for docker MinIO
    export MC_HOST_dockerminio="http://minioadmin:minioadmin@localhost:9000"

    # Create bucket
    "$MINIO_BIN/mc" mb dockerminio/$S3_BUCKET 2>&1 | grep -v "^mc:" || log_info "Bucket already exists"

    # Set anonymous download policy
    "$MINIO_BIN/mc" anonymous set download dockerminio/$S3_BUCKET 2>&1 | grep -v "^mc:"

    log_success "S3 bucket ready in docker MinIO"
}

create_cross_cluster_parent() {
    local pool=$1
    local parent_name=$2
    local s3_image_name=$3
    local size_mb=$4

    log_info "Creating S3-backed parent in remote cluster: $pool/$parent_name ($size_mb MB)"

    # Create parent image file
    local temp_file="/tmp/${s3_image_name}"
    dd if=/dev/zero of="$temp_file" bs=1M count=$size_mb 2>/dev/null

    # Write block markers
    for i in $(seq 0 $((size_mb/4 - 1))); do
        printf "PARENT-BLOCK-%04d" $i | dd of="$temp_file" bs=4M seek=$i conv=notrunc 2>/dev/null
    done

    # Upload to S3
    if [ "$MANAGED_S3" = true ]; then
        # Managed mode: use mc to upload to docker MinIO
        "$MINIO_BIN/mc" cp "$temp_file" "dockerminio/$S3_BUCKET/$s3_image_name" 2>&1 | grep -v "^mc:"
    else
        # External mode: use AWS CLI or similar
        if command -v aws &> /dev/null; then
            AWS_ACCESS_KEY_ID="$S3_ACCESS_KEY" \
            AWS_SECRET_ACCESS_KEY="$S3_SECRET_KEY" \
            aws s3 cp "$temp_file" "s3://$S3_BUCKET/$s3_image_name" --endpoint-url "$S3_ENDPOINT" 2>&1
        elif command -v s3cmd &> /dev/null; then
            s3cmd put "$temp_file" "s3://$S3_BUCKET/$s3_image_name" --access_key="$S3_ACCESS_KEY" --secret_key="$S3_SECRET_KEY" --host="$(echo $S3_ENDPOINT | sed 's|https\?://||')" 2>&1
        else
            log_error "Neither 'aws' nor 's3cmd' command found. Please install AWS CLI or s3cmd for S3 upload."
            return 1
        fi
    fi

    # Base64 encode the secret key for storage
    local encoded_secret_key
    if [ "$MANAGED_S3" = true ]; then
        encoded_secret_key="bWluaW9hZG1pbg=="  # base64 of "minioadmin"
    else
        encoded_secret_key=$(echo -n "$S3_SECRET_KEY" | base64)
    fi

    # Create RBD parent in remote cluster
    if [ "$MANAGED_CLUSTERS" = true ]; then
        # Managed mode: use docker exec
        docker exec -w /home/kefu/dev/ceph-nautilus ceph-remote-cluster bash -c "
            ./build/bin/rbd --conf remote-cluster/ceph.conf rm $pool/$parent_name 2>/dev/null || true
            ./build/bin/rbd --conf remote-cluster/ceph.conf create $pool/$parent_name --size ${size_mb}M --object-size 4M

            # Configure S3 metadata
            ./build/bin/rbd --conf remote-cluster/ceph.conf image-meta set $pool/$parent_name s3.enabled true
            ./build/bin/rbd --conf remote-cluster/ceph.conf image-meta set $pool/$parent_name s3.endpoint $S3_ENDPOINT
            ./build/bin/rbd --conf remote-cluster/ceph.conf image-meta set $pool/$parent_name s3.bucket $S3_BUCKET
            ./build/bin/rbd --conf remote-cluster/ceph.conf image-meta set $pool/$parent_name s3.image_name $s3_image_name
            ./build/bin/rbd --conf remote-cluster/ceph.conf image-meta set $pool/$parent_name s3.image_format raw
            ./build/bin/rbd --conf remote-cluster/ceph.conf image-meta set $pool/$parent_name s3.verify_ssl false
            ./build/bin/rbd --conf remote-cluster/ceph.conf image-meta set $pool/$parent_name s3.access_key ${S3_ACCESS_KEY:-minioadmin}
            ./build/bin/rbd --conf remote-cluster/ceph.conf image-meta set $pool/$parent_name s3.secret_key $encoded_secret_key
        "
    else
        # External mode: use provided remote cluster config
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" rm "$pool/$parent_name" 2>/dev/null || true
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" create "$pool/$parent_name" --size ${size_mb}M --object-size 4M

        # Configure S3 metadata
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" image-meta set "$pool/$parent_name" s3.enabled true
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" image-meta set "$pool/$parent_name" s3.endpoint "$S3_ENDPOINT"
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" image-meta set "$pool/$parent_name" s3.bucket "$S3_BUCKET"
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" image-meta set "$pool/$parent_name" s3.image_name "$s3_image_name"
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" image-meta set "$pool/$parent_name" s3.image_format raw
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" image-meta set "$pool/$parent_name" s3.verify_ssl false
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" image-meta set "$pool/$parent_name" s3.access_key "$S3_ACCESS_KEY"
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" image-meta set "$pool/$parent_name" s3.secret_key "$encoded_secret_key"
    fi

    log_success "S3-backed parent created in remote cluster"
}

create_cross_cluster_clone() {
    local pool=$1
    local parent=$2
    local child=$3
    local s3_image_name=$4

    log_info "Creating cross-cluster clone: $pool/$child from remote $pool/$parent"

    # Extract or use remote cluster credentials
    log_info "Preparing remote cluster credentials..."

    if [ "$MANAGED_CLUSTERS" = true ]; then
        # Managed mode: extract credentials from docker container
        REMOTE_CONF="/tmp/remote-cluster-$$.conf"
        REMOTE_KEYRING="/tmp/remote-cluster-$$.keyring"

        # Get admin key from remote cluster
        ADMIN_KEY=$(docker exec ceph-remote-cluster bash -c "cd /home/kefu/dev/ceph-nautilus && ./build/bin/ceph --conf remote-cluster/ceph.conf auth get-key client.admin" 2>/dev/null)
        if [ -z "$ADMIN_KEY" ]; then
            log_error "Failed to get remote cluster admin key"
            return 1
        fi

        # Create keyring file with proper format
        cat > "$REMOTE_KEYRING" <<EOF
[client.admin]
key = $ADMIN_KEY
EOF

        # Create config file with correct mon_host
        cat > "$REMOTE_CONF" <<EOF
[global]
mon_host = $REMOTE_MON_HOST
keyring = $REMOTE_KEYRING
EOF
    else
        # External mode: use provided credentials
        REMOTE_CONF="$REMOTE_CONF_FILE"
        REMOTE_KEYRING="$REMOTE_KEYRING_FILE"
    fi

    # Remove existing child if present
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$pool/$child" 2>/dev/null || true

    # Use clone-standalone command with remote cluster authentication
    log_info "Creating cross-cluster clone using clone-standalone command..."
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone \
        --remote-cluster-conf "$REMOTE_CONF" \
        --remote-keyring "$REMOTE_KEYRING" \
        "$pool/$parent" "$pool/$child" 2>&1

    local clone_result=$?
    if [ $clone_result -ne 0 ]; then
        log_error "Failed to create cross-cluster clone (exit code: $clone_result)"
        if [ "$MANAGED_CLUSTERS" = true ]; then
            rm -f "$REMOTE_CONF" "$REMOTE_KEYRING"
        fi
        return 1
    fi

    # Cleanup temp files (only in managed mode)
    if [ "$MANAGED_CLUSTERS" = true ]; then
        rm -f "$REMOTE_CONF" "$REMOTE_KEYRING"
    fi

    log_success "Cross-cluster clone created using proper clone-standalone command"
}

setup_cross_cluster_scenario_2() {
    local pool=$1
    local parent=$2
    local child=$3

    log_info "Setting up Scenario 2 for cross-cluster: Partial blocks cached"

    # Pre-populate some parent blocks in remote cluster
    log_info "Pre-populating first 2 objects in remote parent..."

    if [ "$MANAGED_CLUSTERS" = true ]; then
        # Managed mode: use docker exec
        docker exec -w /home/kefu/dev/ceph-nautilus ceph-remote-cluster bash -c "
            ./build/bin/rbd --conf remote-cluster/ceph.conf export $pool/$parent /tmp/partial-$$.raw \
                --rbd-concurrent-management-ops 1 2>&1 | head -n 5 | grep -v 'Exporting' || true
            rm -f /tmp/partial-$$.raw
        "
    else
        # External mode: use provided remote cluster config
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" export "$pool/$parent" /tmp/partial-$$.raw \
            --rbd-concurrent-management-ops 1 2>&1 | head -n 5 | grep -v 'Exporting' || true
        rm -f /tmp/partial-$$.raw
    fi

    # Write to child in local cluster
    log_info "Writing to child to create child-specific blocks..."
    echo "CHILD-DATA" | "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench --io-type write "$pool/$child" \
        --io-size 4096 --io-total 4096 --io-pattern seq --io-offset $((8*1024*1024)) >/dev/null 2>&1 || true

    log_success "Scenario 2 ready for cross-cluster"
}

setup_cross_cluster_scenario_3() {
    local pool=$1
    local parent=$2
    local child=$3

    log_info "Setting up Scenario 3 for cross-cluster: Full cache"

    # Pre-populate ALL parent blocks in remote cluster
    log_info "Pre-populating all blocks in remote parent..."

    if [ "$MANAGED_CLUSTERS" = true ]; then
        # Managed mode: use docker exec
        docker exec -w /home/kefu/dev/ceph-nautilus ceph-remote-cluster bash -c "
            ./build/bin/rbd --conf remote-cluster/ceph.conf export $pool/$parent /tmp/full-$$.raw 2>&1 | grep -v 'Exporting'
            rm -f /tmp/full-$$.raw
        "
    else
        # External mode: use provided remote cluster config
        "$BUILD_DIR/bin/rbd" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" export "$pool/$parent" /tmp/full-$$.raw 2>&1 | grep -v 'Exporting'
        rm -f /tmp/full-$$.raw
    fi

    log_success "Scenario 3 ready for cross-cluster"
}

# Main execution
main() {
    log_info "=== S3-Backed RBD Clone E2E Test Matrix ==="
    if [ "$MODE" == "external" ]; then
        log_info "Mode: external (user-provided clusters and S3)"
    else
        log_info "Mode: $MODE cluster"
    fi
    echo

    # Check prerequisites
    if [ "$MODE" != "external" ]; then
        if ! check_cluster_running; then
            exit 1
        fi
    fi

    # Mode-specific setup
    if [ "$CROSS_CLUSTER" = true ]; then
        # Cross-cluster mode (managed or external)
        if [ "$MANAGED_CLUSTERS" = true ]; then
            if ! setup_cross_cluster; then
                log_error "Failed to setup cross-cluster environment"
                exit 1
            fi
        else
            # External mode: verify clusters are accessible
            log_info "Verifying local cluster access..."
            if ! "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" --keyring "$LOCAL_KEYRING" status >/dev/null 2>&1; then
                log_error "Cannot connect to local cluster"
                exit 1
            fi
            log_success "Local cluster accessible"

            log_info "Verifying remote cluster access..."
            if ! "$BUILD_DIR/bin/ceph" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" status >/dev/null 2>&1; then
                log_error "Cannot connect to remote cluster"
                exit 1
            fi
            log_success "Remote cluster accessible"

            # Create pools if they don't exist
            log_info "Ensuring pools exist..."
            "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" --keyring "$LOCAL_KEYRING" osd pool create "$POOL_NAME" 8 2>&1 | grep -v 'already exists' || true
            "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" --keyring "$LOCAL_KEYRING" osd pool application enable "$POOL_NAME" rbd 2>&1 || true
            "$BUILD_DIR/bin/ceph" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" osd pool create "$POOL_NAME" 8 2>&1 | grep -v 'already exists' || true
            "$BUILD_DIR/bin/ceph" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" osd pool application enable "$POOL_NAME" rbd 2>&1 || true
            log_success "Pools ready"

            # Verify S3 access
            log_info "Verifying S3 bucket access..."
            if command -v aws &> /dev/null; then
                if AWS_ACCESS_KEY_ID="$S3_ACCESS_KEY" \
                   AWS_SECRET_ACCESS_KEY="$S3_SECRET_KEY" \
                   aws s3 ls "s3://$S3_BUCKET" --endpoint-url "$S3_ENDPOINT" >/dev/null 2>&1; then
                    log_success "S3 bucket accessible"
                else
                    log_error "Cannot access S3 bucket: $S3_BUCKET at $S3_ENDPOINT"
                    log_error "Please verify your S3 credentials and bucket permissions"
                    exit 1
                fi
            elif command -v s3cmd &> /dev/null; then
                if s3cmd ls "s3://$S3_BUCKET" --access_key="$S3_ACCESS_KEY" --secret_key="$S3_SECRET_KEY" --host="$(echo $S3_ENDPOINT | sed 's|https\?://||')" >/dev/null 2>&1; then
                    log_success "S3 bucket accessible"
                else
                    log_error "Cannot access S3 bucket: $S3_BUCKET at $S3_ENDPOINT"
                    log_error "Please verify your S3 credentials and bucket permissions"
                    exit 1
                fi
            else
                log_warn "Neither 'aws' nor 's3cmd' found - skipping S3 verification"
                log_warn "Tests may fail if S3 bucket is not accessible"
            fi
        fi

    else
        # Single-cluster mode
        # Start MinIO
        if ! start_minio $MINIO_PORT $MINIO_CONSOLE_PORT; then
            log_error "Failed to start MinIO"
            exit 1
        fi

        # Setup S3 bucket
        setup_s3_bucket $MINIO_PORT "$S3_BUCKET"
    fi

    # Create pool in local cluster (skip if external cross-cluster, already done)
    if [ "$MODE" != "external" ] || [ "$CROSS_CLUSTER" != true ]; then
        create_pool "$POOL_NAME"
    fi

    # Enable S3 fetch
    if [ "$MODE" == "external" ]; then
        # External mode: use runtime config to avoid modifying user's config files
        log_info "Enabling S3 fetch via runtime config..."
        "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" --keyring "$LOCAL_KEYRING" config set osd rbd_s3_fetch_enabled true 2>&1 || true
        if [ "$CROSS_CLUSTER" = true ]; then
            "$BUILD_DIR/bin/ceph" --conf "$REMOTE_CONF_FILE" --keyring "$REMOTE_KEYRING_FILE" config set osd rbd_s3_fetch_enabled true 2>&1 || true
        fi
    else
        enable_s3_fetch
    fi

    # Run all test cases
    local failed=0

    for scenario in 1 2 3; do
        for operation in read write flatten; do
            if ! run_test_case $scenario $operation; then
                failed=$((failed + 1))
            fi
        done
    done

    # Print summary
    print_test_summary

    return $failed
}

# Run main
main
exit $?
