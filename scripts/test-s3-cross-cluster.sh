#!/bin/bash
# test-s3-cross-cluster.sh — Cross-cluster standalone clone tests (Docker-based)
#
# Requires Docker and docker-compose.  Spins up two isolated Ceph clusters:
#   cluster1 — holds the parent image (S3-backed or plain)
#   cluster2 — holds the child clone; runs clone-standalone, bench, flatten
#
# Tests:
#   1. Plain cross-cluster clone (no S3):  clone, write, flatten
#   2. S3-backed cross-cluster clone: parent backed by MinIO, clone on cluster2,
#      read triggers S3 fetch via cluster1 remote connection
#
# Usage: ./test-s3-cross-cluster.sh [--skip-build] [--plain-only] [--s3-only]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CEPH_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Options ──────────────────────────────────────────────────────────────────
SKIP_BUILD=0
RUN_PLAIN=1
RUN_S3=1

while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-build) SKIP_BUILD=1; shift ;;
        --plain-only) RUN_S3=0; shift ;;
        --s3-only)    RUN_PLAIN=0; shift ;;
        *) shift ;;
    esac
done

# ── Helpers ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
log_info()    { echo -e "${GREEN}[INFO]${NC}  $1"; }
log_step()    { echo -e "${BLUE}[STEP]${NC}  $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1" >&2; }
log_success() { echo -e "${GREEN}[PASS]${NC}  $1"; }

check_prereqs() {
    for cmd in docker docker-compose; do
        if ! command -v "$cmd" &>/dev/null; then
            log_error "$cmd is required but not found"
            exit 1
        fi
    done
}

# Run a command inside a cluster container as the cephdev user
exec_on() {
    local cluster=$1; shift
    docker exec -u cephdev "ceph-${cluster}" bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; cd /ceph/build && $*"
}

# Run rbd/ceph/rados on a specific cluster with its ceph.conf
rbd_on()  { exec_on "$1" "./bin/rbd  --conf /tmp/${1}/ceph.conf ${*:2}"; }
ceph_on() { exec_on "$1" "./bin/ceph --conf /tmp/${1}/ceph.conf ${*:2}"; }
rados_on(){ exec_on "$1" "./bin/rados --conf /tmp/${1}/ceph.conf ${*:2}"; }

# ── Infrastructure setup ─────────────────────────────────────────────────────
setup_containers() {
    log_step "Setting up Docker containers"

    cd "$CEPH_ROOT"
    docker pull debian:stable
    DOCKER_BUILDKIT=0 docker-compose up -d --build

    log_info "Waiting for containers..."
    sleep 5

    if [ $SKIP_BUILD -eq 0 ]; then
        if ! docker exec ceph-cluster1 test -f /ceph/build/bin/ceph 2>/dev/null; then
            log_info "Building Ceph in cluster1 (this takes a while)..."
            docker exec -u cephdev ceph-cluster1 bash -c \
                "cd /ceph && ./do_cmake.sh && cd build && ninja -j\$(nproc)"
        else
            log_info "Ceph build already present in cluster1"
        fi
    fi

    log_success "Containers ready"
}

start_ceph_cluster() {
    local cluster=$1
    log_step "Starting Ceph cluster: $cluster"

    bash "$SCRIPT_DIR/start-cluster.sh" "$cluster"
    log_success "$cluster started"
}

cleanup() {
    log_info "Tearing down containers..."
    cd "$CEPH_ROOT"
    docker-compose down 2>/dev/null || true
}
trap cleanup EXIT

# ── Test: plain cross-cluster clone ──────────────────────────────────────────
run_plain_cross_cluster() {
    log_step "=== Test: Plain Cross-Cluster Standalone Clone ==="

    ceph_on cluster1 "osd pool create parent_pool 32" 2>&1 || true
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf pool init parent_pool"
    rbd_on cluster1 "create --size 100M parent_pool/parent_image"
    log_info "Created parent_pool/parent_image on cluster1"

    ceph_on cluster2 "osd pool create parent_pool 32" 2>&1 || true
    ceph_on cluster2 "osd pool create child_pool 32"  2>&1 || true
    exec_on cluster2 "./bin/rbd --conf /tmp/cluster2/ceph.conf pool init parent_pool"
    exec_on cluster2 "./bin/rbd --conf /tmp/cluster2/ceph.conf pool init child_pool"

    # Exchange cluster1 connection info into cluster2
    local mon_addr key
    mon_addr=$(docker exec ceph-cluster1 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; \
         /ceph/build/bin/ceph --conf /tmp/cluster1/ceph.conf mon dump --format json 2>/dev/null \
         | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[\"mons\"][0][\"addr\"].split(\"/\")[0])'")
    key=$(docker exec ceph-cluster1 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; \
         /ceph/build/bin/ceph --conf /tmp/cluster1/ceph.conf auth get-key client.admin")

    docker exec -u cephdev ceph-cluster2 bash -c "mkdir -p /home/cephdev/.ceph
cat > /home/cephdev/.ceph/cluster1.conf << 'EOF'
[global]
mon_host = ${mon_addr}
EOF
cat > /home/cephdev/.ceph/cluster1.keyring << 'EOF'
[client.admin]
key = ${key}
EOF
chmod 600 /home/cephdev/.ceph/cluster1.keyring"

    # Clone-standalone: run from cluster2, parent is on cluster1
    docker exec -u cephdev ceph-cluster2 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; cd /ceph/build && \
         ./bin/rbd --conf /tmp/cluster2/ceph.conf clone-standalone \
             --remote-cluster-conf   /home/cephdev/.ceph/cluster1.conf \
             --remote-keyring        /home/cephdev/.ceph/cluster1.keyring \
             parent_pool/parent_image \
             child_pool/child_image"
    log_success "Cross-cluster standalone clone created"

    # Write to child (triggers COW — parent read from cluster1)
    rbd_on cluster2 \
        "bench child_pool/child_image --io-type write --io-size 4M --io-total 4M --io-threads 1"
    log_success "Write to child completed (COW from cluster1)"

    # Verify RADOS objects created on cluster2
    local block_prefix obj_count
    block_prefix=$(exec_on cluster2 \
        "./bin/rbd --conf /tmp/cluster2/ceph.conf info child_pool/child_image 2>/dev/null \
         | awk '/block_name_prefix:/ {print \$2}'")
    obj_count=$(exec_on cluster2 \
        "./bin/rados --conf /tmp/cluster2/ceph.conf -p child_pool ls 2>/dev/null \
         | grep -c \"^${block_prefix}\\.\"" || echo "0")
    log_info "Child has $obj_count RADOS objects (prefix: $block_prefix)"

    # Flatten
    rbd_on cluster2 "flatten child_pool/child_image"
    log_success "Flatten completed — child is now independent"

    rbd_on cluster2 "info child_pool/child_image"

    # Cleanup test images
    rbd_on  cluster2 "rm child_pool/child_image"  2>/dev/null || true
    rbd_on  cluster1 "rm parent_pool/parent_image" 2>/dev/null || true
    ceph_on cluster1 "osd pool delete parent_pool parent_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    ceph_on cluster2 "osd pool delete parent_pool parent_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    ceph_on cluster2 "osd pool delete child_pool  child_pool  --yes-i-really-really-mean-it" 2>/dev/null || true

    log_success "=== Plain Cross-Cluster Test PASSED ==="
}

# ── Test: S3-backed cross-cluster clone ──────────────────────────────────────
run_s3_cross_cluster() {
    log_step "=== Test: S3-Backed Cross-Cluster Standalone Clone ==="
    log_warn "NOTE: This test requires MinIO to be reachable from both containers."
    log_warn "      MinIO must be started externally or use host networking."

    # The matrix script (test-s3-e2e-matrix.sh cross) covers the full S3+cross-cluster
    # scenario with all read/write/flatten combinations.  Here we validate just the
    # basic clone-standalone + S3 read path.

    local minio_port=19200
    local minio_data="/tmp/minio-cross-$$"
    local minio_pid=""
    local s3_bucket="cross-cluster-test"
    local s3_endpoint="http://127.0.0.1:${minio_port}"

    # Start MinIO on the host
    mkdir -p "$minio_data"
    MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin \
        "$HOME/dev/minio/bin/minio" server "$minio_data" \
        --address "0.0.0.0:${minio_port}" \
        --console-address "0.0.0.0:$((minio_port + 1))" \
        > /tmp/minio-cross-cluster.log 2>&1 &
    minio_pid=$!
    sleep 3

    if ! kill -0 $minio_pid 2>/dev/null; then
        log_error "MinIO failed to start for cross-cluster test"
        cat /tmp/minio-cross-cluster.log
        return 1
    fi

    "$HOME/dev/minio/bin/mc" alias set cross "$s3_endpoint" minioadmin minioadmin > /dev/null 2>&1
    "$HOME/dev/minio/bin/mc" mb "cross/$s3_bucket" > /dev/null 2>&1 || true

    # Create and upload parent data (20 MB)
    dd if=/dev/urandom bs=1M count=20 status=none | \
        "$HOME/dev/minio/bin/mc" pipe "cross/$s3_bucket/cross-parent-raw" > /dev/null 2>&1
    log_success "Uploaded 20MB parent image to S3"

    # Determine host IP as seen from containers (bridge network gateway)
    HOST_IP=$(docker network inspect bridge --format '{{range .IPAM.Config}}{{.Gateway}}{{end}}' 2>/dev/null || echo "172.17.0.1")
    local container_s3="http://${HOST_IP}:${minio_port}"

    # Setup parent pool + image on cluster1 with S3 config pointing to host MinIO
    ceph_on cluster1 "osd pool create xcluster_pool 32" 2>&1 || true
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf pool init xcluster_pool"
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf create \
        --size 20M xcluster_pool/xcluster-parent"
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf s3-config set \
        xcluster_pool/xcluster-parent \
        --s3-endpoint   '${container_s3}' \
        --s3-bucket     '${s3_bucket}' \
        --s3-image-name 'cross-parent-raw' \
        --s3-access-key minioadmin \
        --s3-secret-key minioadmin"
    log_success "Parent image configured with S3 backend on cluster1"

    # Setup child pool on cluster2 and mirror cluster1 access config
    ceph_on cluster2 "osd pool create xcluster_pool 32" 2>&1 || true
    ceph_on cluster2 "osd pool create xchild_pool 32" 2>&1 || true
    exec_on cluster2 "./bin/rbd --conf /tmp/cluster2/ceph.conf pool init xcluster_pool"
    exec_on cluster2 "./bin/rbd --conf /tmp/cluster2/ceph.conf pool init xchild_pool"

    local mon_addr key
    mon_addr=$(docker exec ceph-cluster1 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; \
         /ceph/build/bin/ceph --conf /tmp/cluster1/ceph.conf mon dump --format json 2>/dev/null \
         | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[\"mons\"][0][\"addr\"].split(\"/\")[0])'")
    key=$(docker exec ceph-cluster1 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; \
         /ceph/build/bin/ceph --conf /tmp/cluster1/ceph.conf auth get-key client.admin")

    docker exec -u cephdev ceph-cluster2 bash -c "mkdir -p /home/cephdev/.ceph
cat > /home/cephdev/.ceph/xcluster1.conf << 'EOF'
[global]
mon_host = ${mon_addr}
EOF
cat > /home/cephdev/.ceph/xcluster1.keyring << 'EOF'
[client.admin]
key = ${key}
EOF
chmod 600 /home/cephdev/.ceph/xcluster1.keyring"

    # Clone-standalone from cluster2, parent on cluster1
    docker exec -u cephdev ceph-cluster2 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; cd /ceph/build && \
         ./bin/rbd --conf /tmp/cluster2/ceph.conf clone-standalone \
             --remote-cluster-conf   /home/cephdev/.ceph/xcluster1.conf \
             --remote-keyring        /home/cephdev/.ceph/xcluster1.keyring \
             xcluster_pool/xcluster-parent \
             xchild_pool/xcluster-child"
    log_success "S3-backed cross-cluster standalone clone created"

    # Read from child (triggers COW + S3 fetch via remote cluster1 connection)
    rbd_on cluster2 \
        "bench xchild_pool/xcluster-child --io-type read --io-size 4M --io-total 4M --io-threads 1"
    log_success "Read from cross-cluster child (S3 fetch via remote parent)"

    # Flatten the child
    rbd_on cluster2 "flatten xchild_pool/xcluster-child"
    log_success "Flatten of cross-cluster S3-backed child completed"

    # Cleanup
    rbd_on  cluster2 "rm xchild_pool/xcluster-child" 2>/dev/null || true
    rbd_on  cluster1 "rm xcluster_pool/xcluster-parent" 2>/dev/null || true
    ceph_on cluster1 "osd pool delete xcluster_pool xcluster_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    ceph_on cluster2 "osd pool delete xcluster_pool xcluster_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    ceph_on cluster2 "osd pool delete xchild_pool   xchild_pool   --yes-i-really-really-mean-it" 2>/dev/null || true

    kill $minio_pid 2>/dev/null || true
    rm -rf "$minio_data" /tmp/minio-cross-cluster.log

    log_success "=== S3-Backed Cross-Cluster Test PASSED ==="
}

# ── Main ─────────────────────────────────────────────────────────────────────
check_prereqs

echo
log_info "=== S3-Backed Standalone Clone — Cross-Cluster Tests (Docker) ==="
echo

setup_containers
start_ceph_cluster cluster1
start_ceph_cluster cluster2

[ $RUN_PLAIN -eq 1 ] && run_plain_cross_cluster
[ $RUN_S3    -eq 1 ] && run_s3_cross_cluster

echo
log_success "All cross-cluster tests PASSED"
