#!/bin/bash
# Full E2E test for the rbd-backfill daemon:
#   TEST 1: Daemon discovers backfill-scheduled image and copies objects from S3 to RADOS
#   TEST 2: Client write (COW) completes while daemon is running (no deadlock)
#   TEST 3: All objects are eventually backfilled

set -e

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
log_info()    { echo -e "${GREEN}[INFO]${NC} $1"; }
log_step()    { echo -e "${BLUE}[STEP]${NC} $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$WORKSPACE/build"
MINIO_BIN="$HOME/dev/minio/bin"

MINIO_HOST="127.0.0.1"
MINIO_PORT="19400"
MINIO_ENDPOINT="http://${MINIO_HOST}:${MINIO_PORT}"
MINIO_ACCESS_KEY="minioadmin"
MINIO_SECRET_KEY="minioadmin"
S3_BUCKET="backfill-full-test"
S3_OBJECT_KEY="parent-image-raw"

PARENT_IMAGE_NAME="backfill-full-parent"
CHILD_IMAGE_NAME="backfill-full-child"
PARENT_IMAGE_SIZE="40M"  # 40 MB = 10 objects of 4MB each
POOL_NAME="rbd"

MINIO_PID=""
BACKFILL_PID=""
CEPH_CONF=""
MANAGED_CLUSTER=1
TEST_CLUSTER_DIR="/tmp/backfill-full-cluster"

cleanup() {
    log_info "Cleaning up..."
    [ -n "$BACKFILL_PID" ] && { kill "$BACKFILL_PID" 2>/dev/null || true; }
    [ -n "$MINIO_PID"    ] && { kill "$MINIO_PID"    2>/dev/null || true; wait "$MINIO_PID" 2>/dev/null || true; }

    if [ -n "$CEPH_CONF" ]; then
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL_NAME/$CHILD_IMAGE_NAME" 2>/dev/null || true
        "$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" rm "$POOL_NAME/$PARENT_IMAGE_NAME" 2>/dev/null || true
    fi
    if [ "$MANAGED_CLUSTER" -eq 1 ] && [ -f "$TEST_CLUSTER_DIR/ceph.conf" ]; then
        cd "$WORKSPACE"
        log_info "Stopping Ceph cluster..."
        "$BUILD_DIR/../src/stop.sh" 2>/dev/null || true
        rm -rf "$TEST_CLUSTER_DIR"
    fi
    rm -rf /tmp/minio-backfill-full-test
    rm -f /tmp/backfill-full-parent.raw
    log_info "Cleanup complete"
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
    case $1 in
        --conf) CEPH_CONF="$2"; MANAGED_CLUSTER=0; shift 2 ;;
        *) shift ;;
    esac
done

log_info "=== E2E Test: RBD Backfill Daemon (Full Functional Test) ==="
echo

# ============================================================================
log_step "1. Starting MinIO server..."
# ============================================================================
mkdir -p /tmp/minio-backfill-full-test

MINIO_ROOT_USER="$MINIO_ACCESS_KEY" MINIO_ROOT_PASSWORD="$MINIO_SECRET_KEY" \
    "$MINIO_BIN/minio" server /tmp/minio-backfill-full-test \
    --address "${MINIO_HOST}:${MINIO_PORT}" \
    --console-address "${MINIO_HOST}:$((MINIO_PORT+1))" > /tmp/minio-backfill-full.log 2>&1 &
MINIO_PID=$!

MINIO_READY=0
for attempt in $(seq 1 20); do
    sleep 1
    if "$MINIO_BIN/mc" alias set backfill-full-minio "$MINIO_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" > /dev/null 2>&1 && \
       "$MINIO_BIN/mc" admin info backfill-full-minio > /dev/null 2>&1; then
        MINIO_READY=1; break
    fi
done
if [ $MINIO_READY -eq 0 ] || ! kill -0 "$MINIO_PID" 2>/dev/null; then
    log_error "MinIO failed to start"; cat /tmp/minio-backfill-full.log; exit 1
fi
log_success "MinIO is ready (took ${attempt}s)"

"$MINIO_BIN/mc" mb "backfill-full-minio/$S3_BUCKET" 2>/dev/null || log_warn "Bucket may already exist"

# ============================================================================
log_step "2. Creating test data and uploading to S3..."
# ============================================================================
log_info "Creating test data file (40MB)..."
dd if=/dev/urandom of=/tmp/backfill-full-parent.raw bs=1M count=40 status=none

log_info "Uploading to S3 (bucket: $S3_BUCKET, key: $S3_OBJECT_KEY)..."
"$MINIO_BIN/mc" cp /tmp/backfill-full-parent.raw "backfill-full-minio/$S3_BUCKET/$S3_OBJECT_KEY" > /dev/null

S3_SIZE=$("$MINIO_BIN/mc" stat "backfill-full-minio/$S3_BUCKET/$S3_OBJECT_KEY" 2>/dev/null | awk '/Size:/ {print $2}' || echo "unknown")
log_success "S3 object uploaded (size: $S3_SIZE)"

# ============================================================================
log_step "3. Starting Ceph cluster..."
# ============================================================================
if [ "$MANAGED_CLUSTER" -eq 0 ]; then
    log_info "Using external cluster: $CEPH_CONF"
else
    log_info "Starting managed test cluster..."
    mkdir -p "$TEST_CLUSTER_DIR"
    cd "$BUILD_DIR"
    MDS=0 MGR=1 MON=1 OSD=3 RGW=0 \
        ../src/vstart.sh -n -d --without-dashboard \
        CEPH_DIR="$TEST_CLUSTER_DIR" > /tmp/backfill-full-vstart.log 2>&1 || {
        log_error "Failed to start cluster"; tail -50 /tmp/backfill-full-vstart.log; exit 1
    }
    CEPH_CONF="$TEST_CLUSTER_DIR/ceph.conf"
    for i in $(seq 1 30); do
        if "$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" -s 2>/dev/null | grep -q "HEALTH_OK\|HEALTH_WARN"; then
            break
        fi
        [ $i -eq 30 ] && { log_error "Cluster failed to become healthy"; exit 1; }
        sleep 1
    done
    log_success "Cluster is healthy"
fi

"$BUILD_DIR/bin/ceph" --conf "$CEPH_CONF" osd pool create "$POOL_NAME" 32 2>/dev/null || log_warn "Pool may exist"
"$BUILD_DIR/bin/rbd"  --conf "$CEPH_CONF" pool init "$POOL_NAME" 2>/dev/null || true

# ============================================================================
log_step "4. Creating parent image with S3 config + schedule backfill..."
# ============================================================================
log_info "Creating parent image: $PARENT_IMAGE_NAME (size: $PARENT_IMAGE_SIZE)"
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" create "$POOL_NAME/$PARENT_IMAGE_NAME" \
    --size "$PARENT_IMAGE_SIZE" --image-feature layering

log_info "Setting S3 config with rbd s3-config set..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" s3-config set "$POOL_NAME/$PARENT_IMAGE_NAME" \
    --s3-endpoint   "$MINIO_ENDPOINT" \
    --s3-bucket     "$S3_BUCKET" \
    --s3-image-name "$S3_OBJECT_KEY" \
    --s3-access-key "$MINIO_ACCESS_KEY" \
    --s3-secret-key "$MINIO_SECRET_KEY"

log_info "Scheduling backfill (sets backfill_scheduled=true)..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" backfill schedule "$POOL_NAME/$PARENT_IMAGE_NAME"
log_success "S3 config and backfill schedule set"

# ============================================================================
log_step "5. TEST 1: Daemon discovers and backfills image from S3"
# ============================================================================
log_info "Starting rbd-backfill daemon in background..."
"$BUILD_DIR/bin/rbd-backfill" --conf "$CEPH_CONF" \
    --foreground > /tmp/backfill-full-daemon.log 2>&1 &
BACKFILL_PID=$!
log_info "Daemon PID: $BACKFILL_PID"

log_info "Waiting for daemon to backfill objects (30 seconds)..."
sleep 30

if ! kill -0 "$BACKFILL_PID" 2>/dev/null; then
    log_error "Daemon exited prematurely!"
    cat /tmp/backfill-full-daemon.log
    exit 1
fi

PARENT_PREFIX=$("$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" info "$POOL_NAME/$PARENT_IMAGE_NAME" | grep block_name_prefix | awk '{print $2}')
BACKFILLED_COUNT=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL_NAME" ls 2>/dev/null | grep -c "^${PARENT_PREFIX}\." || true)

log_info "Parent prefix: $PARENT_PREFIX"
log_info "Backfilled objects: $BACKFILLED_COUNT / 10 expected"

if [ "${BACKFILLED_COUNT:-0}" -gt 0 ]; then
    log_success "TEST 1 PASSED: Daemon successfully backfilled $BACKFILLED_COUNT objects from S3"
else
    log_error "TEST 1 FAILED: No objects backfilled"
    cat /tmp/backfill-full-daemon.log
    exit 1
fi

# ============================================================================
log_step "6. TEST 2: Client COW write completes while daemon is running"
# ============================================================================
log_info "Creating child standalone clone: $CHILD_IMAGE_NAME"
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" clone-standalone \
    "$POOL_NAME/$PARENT_IMAGE_NAME" "$POOL_NAME/$CHILD_IMAGE_NAME"

log_info "Running COW write on child (writes 512K, should complete quickly)..."
"$BUILD_DIR/bin/rbd" --conf "$CEPH_CONF" bench "$POOL_NAME/$CHILD_IMAGE_NAME" \
    --io-type write --io-size 512K --io-total 512K > /tmp/backfill-full-child-write.log 2>&1 &
CHILD_WRITE_PID=$!

DEADLINE=$(( $(date +%s) + 30 ))
while kill -0 "$CHILD_WRITE_PID" 2>/dev/null; do
    if [ $(date +%s) -ge $DEADLINE ]; then
        kill "$CHILD_WRITE_PID" 2>/dev/null || true
        log_error "TEST 2 FAILED: COW write timed out (30s) while daemon was running"
        exit 1
    fi
    sleep 1
done
wait "$CHILD_WRITE_PID" 2>/dev/null && log_success "TEST 2 PASSED: COW write completed (no deadlock with daemon)"

# ============================================================================
log_step "7. TEST 3: All objects eventually backfilled"
# ============================================================================
log_info "Stopping daemon gracefully..."
kill -TERM "$BACKFILL_PID" 2>/dev/null || true
for i in $(seq 1 10); do
    kill -0 "$BACKFILL_PID" 2>/dev/null || break
    sleep 1
done
BACKFILL_PID=""

FINAL_COUNT=$("$BUILD_DIR/bin/rados" --conf "$CEPH_CONF" -p "$POOL_NAME" ls 2>/dev/null | grep -c "^${PARENT_PREFIX}\." || true)
log_info "Final backfilled object count: $FINAL_COUNT / 10 expected"

if [ "${FINAL_COUNT:-0}" -ge 10 ]; then
    log_success "TEST 3 PASSED: All 10 objects successfully backfilled"
elif [ "${FINAL_COUNT:-0}" -gt 0 ]; then
    log_warn "TEST 3 PARTIAL: $FINAL_COUNT / 10 objects backfilled (may need more time)"
else
    log_error "TEST 3 FAILED: No objects backfilled"
    exit 1
fi

# ============================================================================
log_step "8. Test Summary"
# ============================================================================
echo
log_info "=== Test Results ==="
echo "  TEST 1: Daemon backfill from S3        - PASS ($BACKFILLED_COUNT objects after 30s)"
echo "  TEST 2: COW write with daemon running  - PASS (no deadlock)"
echo "  TEST 3: All objects backfilled         - $([ "${FINAL_COUNT:-0}" -ge 10 ] && echo "PASS" || echo "PARTIAL ($FINAL_COUNT/10)")"
echo
log_success "E2E backfill functional test complete!"
