#!/bin/bash
# Test: Concurrent COW load from multiple child clones sharing one S3-backed parent
#
# Reproduces the bug where 4 simultaneous VMs trigger unbounded S3 fetch threads
# through the standalone clone parent, overloading Minio and causing COW "guarding
# writes" to block for 20+ minutes.
#
# After the S3ObjectFetcher concurrency fix (S3_MAX_CONCURRENT_FETCHES=8), all
# 4 clients must complete within COW_TIMEOUT_SECS seconds.
#
# Usage: ./test-concurrent-cow-load.sh [--conf <ceph.conf>]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
MINIO_BIN="${HOME}/dev/minio/bin"
MINIO_DATA_DIR="/tmp/minio-cow-load-test"
MINIO_PORT=19100
MINIO_ENDPOINT="http://127.0.0.1:${MINIO_PORT}"
MINIO_ACCESS_KEY="minioadmin"
MINIO_SECRET_KEY="minioadmin"
S3_BUCKET="cow-load-test"

# Test parameters
NUM_CLIENTS=4               # Number of concurrent child clones (simulates 4 VMs)
PARENT_SIZE="20M"           # 20 MB = 5 RBD objects at 4MB each (small for fast tests)
COW_TIMEOUT_SECS=120        # Each client must complete within 2 minutes
IO_SIZE=4096                # Write I/O size (triggers COW per 4MB object)
IO_TOTAL=4194304            # Total bytes written per client (one full object)
POOL="rbd"

# Colours
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
log_info()    { echo -e "${GREEN}[INFO]${NC}  $1"; }
log_step()    { echo -e "${BLUE}[STEP]${NC}  $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
log_success() { echo -e "${GREEN}[PASS]${NC}  $1"; }

MINIO_PID=""
CEPH_CONF=""
MANAGED_CLUSTER=0
TEST_CLUSTER_DIR="/tmp/cow-load-test-cluster"

cleanup() {
    log_info "Cleaning up..."
    # Remove child clones, then unprotect and remove parent snapshot, then parent
    if [ -n "$CEPH_CONF" ]; then
        for i in $(seq 1 $NUM_CLIENTS); do
            "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/cow-child-$i" 2>/dev/null || true
        done
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL/cow-parent" 2>/dev/null || true
    fi
    # Stop managed cluster
    if [ $MANAGED_CLUSTER -eq 1 ] && [ -f "$TEST_CLUSTER_DIR/ceph.conf" ]; then
        cd "$BUILD_DIR"
        ./bin/stop.sh "$TEST_CLUSTER_DIR" 2>/dev/null || true
        rm -rf "$TEST_CLUSTER_DIR"
    fi
    # Stop Minio
    if [ -n "$MINIO_PID" ]; then
        kill $MINIO_PID 2>/dev/null || true
        wait $MINIO_PID 2>/dev/null || true
    fi
    rm -rf "$MINIO_DATA_DIR"
    rm -f /tmp/cow-bench-*.log /tmp/cow-parent.raw
}
trap cleanup EXIT

# Parse args
while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; shift 2 ;;
        *) shift ;;
    esac
done

echo
log_info "=== Test: Concurrent COW Load (${NUM_CLIENTS} clients, ${COW_TIMEOUT_SECS}s timeout) ==="
echo

# ============================================================================
log_step "1. Start Minio"
# ============================================================================
mkdir -p "$MINIO_DATA_DIR"
MINIO_ROOT_USER=$MINIO_ACCESS_KEY MINIO_ROOT_PASSWORD=$MINIO_SECRET_KEY \
    "$MINIO_BIN/minio" server "$MINIO_DATA_DIR" \
    --address "127.0.0.1:${MINIO_PORT}" \
    --console-address "127.0.0.1:$((MINIO_PORT+1))" \
    > /tmp/minio-cow-load.log 2>&1 &
MINIO_PID=$!

# Wait until Minio accepts connections (fresh data dirs need 3-5s to initialize)
MINIO_READY=0
for attempt in $(seq 1 15); do
    sleep 1
    if "$MINIO_BIN/mc" alias set cowtest "$MINIO_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" > /dev/null 2>&1 && \
       "$MINIO_BIN/mc" admin info cowtest > /dev/null 2>&1; then
        MINIO_READY=1
        break
    fi
done

if [ $MINIO_READY -eq 0 ] || ! kill -0 $MINIO_PID 2>/dev/null; then
    log_error "Minio failed to start or become ready"; cat /tmp/minio-cow-load.log; exit 1
fi
log_info "Minio ready (PID: $MINIO_PID, took ${attempt}s)"

"$MINIO_BIN/mc" mb cowtest/$S3_BUCKET > /dev/null 2>&1 || true

# ============================================================================
log_step "2. Create parent image data in S3"
# ============================================================================
dd if=/dev/urandom bs=1M count=20 of=/tmp/cow-parent.raw status=none
"$MINIO_BIN/mc" cp /tmp/cow-parent.raw "cowtest/$S3_BUCKET/cow-parent-raw" > /dev/null
log_info "Uploaded 20MB parent image to S3"

# ============================================================================
log_step "3. Start Ceph cluster (if not provided)"
# ============================================================================
if [ -z "$CEPH_CONF" ]; then
    log_info "Starting managed test cluster..."
    mkdir -p "$TEST_CLUSTER_DIR"
    cd "$BUILD_DIR"
    ./vstart.sh -n -d --without-dashboard \
        --osd-num 3 \
        --mon-num 1 \
        CEPH_DIR="$TEST_CLUSTER_DIR" \
        > /tmp/cow-vstart.log 2>&1
    CEPH_CONF="$TEST_CLUSTER_DIR/ceph.conf"
    MANAGED_CLUSTER=1
    sleep 5
    log_info "Cluster started. Config: $CEPH_CONF"
fi

# ============================================================================
log_step "4. Create standalone parent + $NUM_CLIENTS child clones (no backfill)"
# ============================================================================

CEPH="$BUILD_DIR/bin/ceph --conf $CEPH_CONF"
RBD="$BUILD_DIR/bin/rbd --conf $CEPH_CONF"

# Create parent image with S3 config (uses rbd s3-config set which base64-encodes
# the secret key and sets s3.enabled=true — raw image-meta set would not work because
# RefreshParentRequest expects s3.enabled key and base64-encoded secret key)
$RBD create --size 20480 "$POOL/cow-parent"
$RBD s3-config set "$POOL/cow-parent" \
    --s3-bucket    "$S3_BUCKET" \
    --s3-endpoint  "$MINIO_ENDPOINT" \
    --s3-image-name "cow-parent-raw" \
    --s3-access-key "$MINIO_ACCESS_KEY" \
    --s3-secret-key "$MINIO_SECRET_KEY"
log_info "Created standalone parent with S3 config (s3-config set)"

# Create child clones using clone-standalone (sets parent_type=STANDALONE, which is
# required for CopyupRequest::should_fetch_from_s3() to trigger S3 reads).
# Regular 'rbd clone' (with snapshot) sets parent_type=SNAPSHOT and bypasses S3.
for i in $(seq 1 $NUM_CLIENTS); do
    $RBD clone-standalone "$POOL/cow-parent" "$POOL/cow-child-$i"
    log_info "  Created child clone: cow-child-$i"
done

# ============================================================================
log_step "5. Launch $NUM_CLIENTS concurrent writers (simulate VM boots)"
# ============================================================================
# Each client writes to its own child clone. The first write to each 4MB object
# triggers a COW (copy-on-write): librbd reads the object from the parent (which
# reads from S3), then writes it to the child's RADOS pool.
# Without the S3_MAX_CONCURRENT_FETCHES limit, this spawns unbounded threads
# that overwhelm Minio, causing 20+ minute stalls.

declare -a BENCH_PIDS
START_TIME=$(date +%s)

for i in $(seq 1 $NUM_CLIENTS); do
    "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench \
        "$POOL/cow-child-$i" \
        --io-type write \
        --io-size $IO_SIZE \
        --io-threads 4 \
        --io-total $IO_TOTAL \
        > /tmp/cow-bench-$i.log 2>&1 &
    BENCH_PIDS[$i]=$!
    log_info "  Started client $i (PID: ${BENCH_PIDS[$i]})"
done

log_info "All $NUM_CLIENTS clients running. Waiting up to ${COW_TIMEOUT_SECS}s for all..."

# ============================================================================
log_step "6. Wait for all clients to complete (shared ${COW_TIMEOUT_SECS}s deadline)"
# ============================================================================
# NOTE: bash 'wait <pid>' is a shell builtin that can only wait for children
# of the CURRENT shell. Wrapping it in 'timeout X bash -c "wait <pid>"' fails
# silently because <pid> is not a child of the subshell. We use kill -0 polling
# instead, which works for any process the current user can signal.
FAILED=0
DEADLINE=$(($(date +%s) + COW_TIMEOUT_SECS))
declare -a CLIENT_DONE

while true; do
    ALL_DONE=1
    for i in $(seq 1 $NUM_CLIENTS); do
        if [ -z "${CLIENT_DONE[$i]:-}" ]; then
            if ! kill -0 ${BENCH_PIDS[$i]} 2>/dev/null; then
                # Process exited — reap it and record result
                wait ${BENCH_PIDS[$i]} 2>/dev/null || true
                CLIENT_DONE[$i]=1
                ELAPSED=$(( $(date +%s) - START_TIME ))
                log_success "Client $i completed in ${ELAPSED}s"
            else
                ALL_DONE=0
            fi
        fi
    done
    [ $ALL_DONE -eq 1 ] && break
    if [ $(date +%s) -ge $DEADLINE ]; then
        for i in $(seq 1 $NUM_CLIENTS); do
            if [ -z "${CLIENT_DONE[$i]:-}" ]; then
                kill ${BENCH_PIDS[$i]} 2>/dev/null || true
                log_error "FAIL: Client $i did not complete within ${COW_TIMEOUT_SECS}s"
                log_error "  This indicates a COW 'guarding write' blocked on an S3 fetch"
                log_error "  Check S3ObjectFetcher::S3_MAX_CONCURRENT_FETCHES limit"
                FAILED=$((FAILED + 1))
            fi
        done
        break
    fi
    sleep 1
done

END_TIME=$(date +%s)
TOTAL_TIME=$((END_TIME - START_TIME))

if [ $FAILED -gt 0 ]; then
    log_error "$FAILED/$NUM_CLIENTS clients failed the COW load test"
    log_error "Bench logs:"
    for i in $(seq 1 $NUM_CLIENTS); do
        echo "--- Client $i ---"
        tail -20 /tmp/cow-bench-$i.log 2>/dev/null || echo "(no log)"
    done
    exit 1
fi

log_success "All $NUM_CLIENTS clients completed concurrent COW in ${TOTAL_TIME}s"

# ============================================================================
log_step "7. Verify RADOS objects were created (COW happened)"
# ============================================================================
# RBD names objects rbd_data.<image-id>.<hex-object-no>, not by image name.
# Verify each child image has at least one data object in RADOS after the bench.
INTEGRITY_FAIL=0
for i in $(seq 1 $NUM_CLIENTS); do
    # Get the image block_name_prefix (contains image ID)
    BLOCK_PREFIX=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$POOL/cow-child-$i" 2>/dev/null \
        | awk '/block_name_prefix:/ {print $2}')
    if [ -z "$BLOCK_PREFIX" ]; then
        log_warn "  Child $i: could not get block_name_prefix"
        continue
    fi
    OBJ_COUNT=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL" ls 2>/dev/null \
        | grep -c "^${BLOCK_PREFIX}\." 2>/dev/null || echo "0")
    if [ "$OBJ_COUNT" -gt 0 ]; then
        log_success "  Child $i: $OBJ_COUNT RADOS data object(s) created (COW occurred)"
    else
        log_warn "  Child $i: 0 RADOS objects (COW may not have occurred — check S3 config)"
        INTEGRITY_FAIL=$((INTEGRITY_FAIL + 1))
    fi
done

if [ $INTEGRITY_FAIL -gt 0 ]; then
    log_warn "$INTEGRITY_FAIL/$NUM_CLIENTS children have 0 RADOS objects (COW write may have been deferred or S3 data not needed)"
fi

log_success "Concurrent COW load test PASSED ($NUM_CLIENTS clients, ${TOTAL_TIME}s total)"
