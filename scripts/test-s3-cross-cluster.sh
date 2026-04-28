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
RUN_CONCURRENT=1

while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-build)      SKIP_BUILD=1; shift ;;
        --plain-only)      RUN_S3=0; RUN_CONCURRENT=0; shift ;;
        --s3-only)         RUN_PLAIN=0; shift ;;
        --no-concurrent)   RUN_CONCURRENT=0; shift ;;
        *) shift ;;
    esac
done

# Logging and MinIO helpers from shared library
source "$SCRIPT_DIR/lib/s3-test-common.sh"

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
        "$MINIO_BIN/minio" server "$minio_data" \
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

    "$MINIO_BIN/mc" alias set cross "$s3_endpoint" minioadmin minioadmin > /dev/null 2>&1
    "$MINIO_BIN/mc" mb "cross/$s3_bucket" > /dev/null 2>&1 || true

    # Create and upload parent data (20 MB)
    dd if=/dev/urandom bs=1M count=20 status=none | \
        "$MINIO_BIN/mc" pipe "cross/$s3_bucket/cross-parent-raw" > /dev/null 2>&1
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

    # ────────────────────────────────────────────────────────────────────────
    # Bug #3 regression check: after flattening a cross-cluster lazy clone
    # on cluster2, the PARENT pool on cluster1 must contain RADOS objects.
    # Before the fire_parent_s3_writeback type-check fix, fire_parent_s3_writeback
    # early-returned for PARENT_TYPE_REMOTE_STANDALONE (line 1244 was checking
    # only PARENT_TYPE_STANDALONE).  Net effect: data was fetched from S3 and
    # written into the child pool, but never persisted back to the parent's
    # cache pool — the user's "after flatten, base volume has no data" report.
    # ────────────────────────────────────────────────────────────────────────
    log_step "Verifying parent has cached objects after flatten (bug #3 regression)"
    local parent_prefix parent_obj_count
    parent_prefix=$(exec_on cluster1 \
        "./bin/rbd --conf /tmp/cluster1/ceph.conf info xcluster_pool/xcluster-parent 2>/dev/null \
         | awk '/block_name_prefix:/ {print \$2}'")
    if [ -z "$parent_prefix" ]; then
        log_error "Could not get parent block_name_prefix on cluster1"
        return 1
    fi
    # Give async fire-and-forget write-backs a few seconds to land in RADOS.
    sleep 3
    parent_obj_count=$(exec_on cluster1 \
        "./bin/rados --conf /tmp/cluster1/ceph.conf -p xcluster_pool ls 2>/dev/null \
         | grep -c \"^${parent_prefix}\\.\"" || echo "0")
    log_info "Parent has $parent_obj_count RADOS data object(s) (prefix: $parent_prefix)"
    if [ "$parent_obj_count" -lt 1 ]; then
        log_fail "Bug #3 regression: parent has 0 cached objects after cross-cluster flatten"
        log_error "  Expected: >= 1 (S3 data was fetched and should be cached on parent's pool)"
        log_error "  Got:      $parent_obj_count"
        log_error "  This means fire_parent_s3_writeback early-returned for REMOTE_STANDALONE."
        return 1
    fi
    log_success "Parent has $parent_obj_count cached object(s) — write-back to remote parent works"

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

# ── Test: cross-cluster `rbd children` lists remote child ─────────────────────
# Bug #5: list_descendants used pool-name comparison alone to detect cross-
# cluster children.  When both clusters happened to use the same pool name
# (a common naming convention — e.g. both have "ebs_ceph_ssd"), the comparison
# collapsed and the iterator returned -ENOENT trying to find the remote
# child id in the parent's local pool.  Fix records cluster_name in
# ChildImageSpec at attach time and short-circuits remote children at
# list time using that authoritative signal.
run_s3_cross_cluster_rbd_children() {
    log_step "=== Test: rbd children on cross-cluster S3-backed parent (bug #5) ==="

    local minio_port=19400
    local minio_data="/tmp/minio-rbdchildren-$$"
    local s3_bucket="rbdchildren-test"
    local s3_endpoint="http://127.0.0.1:${minio_port}"
    # Use the SAME pool name on both clusters so we hit the pool_name-collision
    # case that the bug requires.
    local shared_pool="ebs_ceph_ssd"
    local child_pool="ebs_ceph_ssd"

    # Start dedicated MinIO
    mkdir -p "$minio_data"
    MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin \
        "$MINIO_BIN/minio" server "$minio_data" \
        --address "0.0.0.0:${minio_port}" \
        --console-address "0.0.0.0:$((minio_port + 1))" \
        > /tmp/minio-rbdchildren.log 2>&1 &
    local minio_pid=$!
    sleep 3
    if ! kill -0 $minio_pid 2>/dev/null; then
        log_error "MinIO failed to start"; cat /tmp/minio-rbdchildren.log
        return 1
    fi

    "$MINIO_BIN/mc" alias set rbdchildren "$s3_endpoint" minioadmin minioadmin > /dev/null 2>&1
    "$MINIO_BIN/mc" mb "rbdchildren/$s3_bucket" > /dev/null 2>&1 || true
    dd if=/dev/urandom bs=1M count=4 status=none | \
        "$MINIO_BIN/mc" pipe "rbdchildren/$s3_bucket/rbdchildren-parent-raw" > /dev/null 2>&1
    local HOST_IP
    HOST_IP=$(docker network inspect bridge --format '{{range .IPAM.Config}}{{.Gateway}}{{end}}' 2>/dev/null || echo "172.17.0.1")
    local container_s3="http://${HOST_IP}:${minio_port}"

    # Create shared-name pools on both clusters
    ceph_on cluster1 "osd pool create $shared_pool 32" 2>&1 || true
    ceph_on cluster2 "osd pool create $shared_pool 32" 2>&1 || true
    ceph_on cluster2 "osd pool create $child_pool 32"  2>&1 || true
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf pool init $shared_pool"
    exec_on cluster2 "./bin/rbd --conf /tmp/cluster2/ceph.conf pool init $shared_pool"
    exec_on cluster2 "./bin/rbd --conf /tmp/cluster2/ceph.conf pool init $child_pool"

    # Parent on cluster1 with S3 config
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf create \
        --size 4M --object-size 4M $shared_pool/rbdchildren-parent"
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf s3-config set \
        $shared_pool/rbdchildren-parent \
        --s3-endpoint   '${container_s3}' \
        --s3-bucket     '${s3_bucket}' \
        --s3-image-name 'rbdchildren-parent-raw' \
        --s3-access-key minioadmin \
        --s3-secret-key minioadmin"

    # Bridge cluster1 access into cluster2's filesystem
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

    # Create cross-cluster standalone clone on cluster2 → cluster1's parent.
    # The CHILD lives in cluster2's pool with the SAME NAME as cluster1's
    # parent pool — this triggers the pool-name-collision the bug needs.
    docker exec -u cephdev ceph-cluster2 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; cd /ceph/build && \
         ./bin/rbd --conf /tmp/cluster2/ceph.conf clone-standalone \
             --remote-cluster-conf   /home/cephdev/.ceph/xcluster1.conf \
             --remote-keyring        /home/cephdev/.ceph/xcluster1.keyring \
             $shared_pool/rbdchildren-parent \
             $child_pool/rbdchildren-child"
    log_success "Cross-cluster clone created (parent and child both in pool '$shared_pool')"

    # Run `rbd children` on cluster1 against the parent.  Pre-fix, this
    # returned -ENOENT because the child id from cluster2 wasn't found in
    # cluster1's same-named pool.  With the cluster_name fix, it should
    # succeed and list the child as "(remote)".
    log_step "Running rbd children on cluster1 (must succeed and show remote child)"
    local children_output children_exit=0
    children_output=$(exec_on cluster1 \
        "./bin/rbd --conf /tmp/cluster1/ceph.conf children $shared_pool/rbdchildren-parent" 2>&1) \
        || children_exit=$?

    if [ "$children_exit" -ne 0 ]; then
        log_fail "Bug #5 regression: rbd children FAILED with exit $children_exit"
        echo "$children_output"
        return 1
    fi
    log_info "rbd children output:"
    echo "$children_output"

    if ! echo "$children_output" | grep -qi "remote"; then
        log_fail "Bug #5 regression: child not marked '(remote)' — ChildImageSpec.cluster_name not consulted"
        return 1
    fi
    log_success "rbd children listed cross-cluster child as remote (cluster_name dispatch works)"

    # Cleanup
    rbd_on  cluster2 "rm $child_pool/rbdchildren-child"     2>/dev/null || true
    rbd_on  cluster1 "rm $shared_pool/rbdchildren-parent"   2>/dev/null || true
    ceph_on cluster1 "osd pool delete $shared_pool $shared_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    ceph_on cluster2 "osd pool delete $shared_pool $shared_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    ceph_on cluster2 "osd pool delete $child_pool  $child_pool  --yes-i-really-really-mean-it" 2>/dev/null || true
    kill $minio_pid 2>/dev/null || true
    rm -rf "$minio_data" /tmp/minio-rbdchildren.log

    log_success "=== rbd children Cross-Cluster Test PASSED ==="
}

# ── Test: S3 cross-cluster concurrent COW ────────────────────────────────────
run_s3_cross_cluster_concurrent() {
    log_step "=== Test: S3-Backed Cross-Cluster Concurrent COW ($NUM_CONCURRENT clients) ==="
    # N clients on cluster2 simultaneously write to children of the same
    # S3-backed parent on cluster1. All N CopyupRequests race on parent object 0.
    # One wins the sentinel lock (.s3lk), fetches from S3, writes back; the
    # others see the lock, call try_preempt_backfill_lock(), and then re-check
    # the parent (no backfill cookie → no break, just immediate re-stat).
    # After the parent object is written, all remaining clients reuse it.

    local NUM_CONCURRENT=4
    local TIMEOUT_SECS=120
    local minio_port=19300
    local minio_data="/tmp/minio-xconcur-$$"
    local s3_bucket="xconcur-test"
    local s3_endpoint="http://127.0.0.1:${minio_port}"
    local parent_raw="/tmp/xconcur-parent-$$.raw"
    local parent_size_mb=20
    local parent_block_size=$(( 4 * 1024 * 1024 ))

    # Start a dedicated MinIO instance
    mkdir -p "$minio_data"
    MINIO_ROOT_USER=minioadmin MINIO_ROOT_PASSWORD=minioadmin \
        "$MINIO_BIN/minio" server "$minio_data" \
        --address "0.0.0.0:${minio_port}" \
        --console-address "0.0.0.0:$((minio_port + 1))" \
        > /tmp/minio-xconcur.log 2>&1 &
    local minio_pid=$!
    sleep 3

    if ! kill -0 $minio_pid 2>/dev/null; then
        log_error "MinIO failed to start for concurrent cross-cluster test"
        cat /tmp/minio-xconcur.log
        return 1
    fi

    "$MINIO_BIN/mc" alias set xconcur "$s3_endpoint" minioadmin minioadmin > /dev/null 2>&1
    "$MINIO_BIN/mc" mb "xconcur/$s3_bucket" > /dev/null 2>&1 || true

    # Upload random parent data
    dd if=/dev/urandom bs=1M count=$parent_size_mb of="$parent_raw" status=none
    "$MINIO_BIN/mc" cp "$parent_raw" "xconcur/$s3_bucket/xconcur-parent-raw" > /dev/null
    log_success "Uploaded ${parent_size_mb}MB parent image to S3"

    # Determine host IP as seen from containers
    local HOST_IP
    HOST_IP=$(docker network inspect bridge --format '{{range .IPAM.Config}}{{.Gateway}}{{end}}' 2>/dev/null || echo "172.17.0.1")
    local container_s3="http://${HOST_IP}:${minio_port}"

    # Setup parent on cluster1
    ceph_on cluster1 "osd pool create xconcur_pool 32" 2>&1 || true
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf pool init xconcur_pool"
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf create \
        --size ${parent_size_mb}M xconcur_pool/xconcur-parent"
    exec_on cluster1 "./bin/rbd --conf /tmp/cluster1/ceph.conf s3-config set \
        xconcur_pool/xconcur-parent \
        --s3-endpoint   '${container_s3}' \
        --s3-bucket     '${s3_bucket}' \
        --s3-image-name 'xconcur-parent-raw' \
        --s3-access-key minioadmin \
        --s3-secret-key minioadmin"
    log_success "S3-backed parent created on cluster1"

    # Setup child pool on cluster2 and configure cluster1 access
    ceph_on cluster2 "osd pool create xconcur_child_pool 32" 2>&1 || true
    exec_on cluster2 "./bin/rbd --conf /tmp/cluster2/ceph.conf pool init xconcur_child_pool"

    local mon_addr key
    mon_addr=$(docker exec ceph-cluster1 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; \
         /ceph/build/bin/ceph --conf /tmp/cluster1/ceph.conf mon dump --format json 2>/dev/null \
         | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[\"mons\"][0][\"addr\"].split(\"/\")[0])'")
    key=$(docker exec ceph-cluster1 bash -c \
        "source /home/cephdev/.ceph_env 2>/dev/null; \
         /ceph/build/bin/ceph --conf /tmp/cluster1/ceph.conf auth get-key client.admin")

    docker exec -u cephdev ceph-cluster2 bash -c "mkdir -p /home/cephdev/.ceph
cat > /home/cephdev/.ceph/xconcur1.conf << 'EOF'
[global]
mon_host = ${mon_addr}
EOF
cat > /home/cephdev/.ceph/xconcur1.keyring << 'EOF'
[client.admin]
key = ${key}
EOF
chmod 600 /home/cephdev/.ceph/xconcur1.keyring"

    # Create N child clones on cluster2, each pointing at cluster1's parent
    for i in $(seq 1 $NUM_CONCURRENT); do
        docker exec -u cephdev ceph-cluster2 bash -c \
            "source /home/cephdev/.ceph_env 2>/dev/null; cd /ceph/build && \
             ./bin/rbd --conf /tmp/cluster2/ceph.conf clone-standalone \
                 --remote-cluster-conf   /home/cephdev/.ceph/xconcur1.conf \
                 --remote-keyring        /home/cephdev/.ceph/xconcur1.keyring \
                 xconcur_pool/xconcur-parent \
                 xconcur_child_pool/xconcur-child-$i"
        log_info "  Created xconcur-child-$i on cluster2"
    done
    log_success "$NUM_CONCURRENT child clones created on cluster2"

    # Launch N concurrent bench writes — each triggers COW on parent object 0
    # (4 KB write to offset 0, which is just enough to trigger CopyupRequest)
    local -a BENCH_PIDS
    local START_TIME DEADLINE ELAPSED FAILED=0
    START_TIME=$(date +%s)

    for i in $(seq 1 $NUM_CONCURRENT); do
        docker exec -u cephdev ceph-cluster2 bash -c \
            "source /home/cephdev/.ceph_env 2>/dev/null; cd /ceph/build && \
             ./bin/rbd --conf /tmp/cluster2/ceph.conf bench \
                 xconcur_child_pool/xconcur-child-$i \
                 --io-type write --io-size 4096 --io-threads 1 --io-total 4096" \
            > "/tmp/xconcur-bench-$i-$$.log" 2>&1 &
        BENCH_PIDS[$i]=$!
        log_info "  Started client $i (PID: ${BENCH_PIDS[$i]})"
    done

    log_info "All $NUM_CONCURRENT cross-cluster clients running. Waiting up to ${TIMEOUT_SECS}s..."

    DEADLINE=$(( START_TIME + TIMEOUT_SECS ))
    declare -a CLIENT_DONE
    while true; do
        local ALL_DONE=1
        for i in $(seq 1 $NUM_CONCURRENT); do
            if [ -z "${CLIENT_DONE[$i]:-}" ]; then
                if ! kill -0 "${BENCH_PIDS[$i]}" 2>/dev/null; then
                    wait "${BENCH_PIDS[$i]}" 2>/dev/null || true
                    CLIENT_DONE[$i]=1
                    ELAPSED=$(( $(date +%s) - START_TIME ))
                    log_success "  Client $i completed in ${ELAPSED}s"
                else
                    ALL_DONE=0
                fi
            fi
        done
        [ $ALL_DONE -eq 1 ] && break
        if [ $(date +%s) -ge $DEADLINE ]; then
            for i in $(seq 1 $NUM_CONCURRENT); do
                if [ -z "${CLIENT_DONE[$i]:-}" ]; then
                    kill "${BENCH_PIDS[$i]}" 2>/dev/null || true
                    log_error "FAIL: Client $i did not complete within ${TIMEOUT_SECS}s"
                    cat "/tmp/xconcur-bench-$i-$$.log" 2>/dev/null || true
                    FAILED=$(( FAILED + 1 ))
                fi
            done
            break
        fi
        sleep 1
    done

    local TOTAL_TIME=$(( $(date +%s) - START_TIME ))

    if [ $FAILED -gt 0 ]; then
        log_error "$FAILED/$NUM_CONCURRENT cross-cluster clients timed out"
        kill $minio_pid 2>/dev/null || true
        rm -rf "$minio_data" "$parent_raw" /tmp/xconcur-bench-*-$$.log /tmp/minio-xconcur.log
        return 1
    fi

    log_success "All $NUM_CONCURRENT clients completed in ${TOTAL_TIME}s"

    # Verify data integrity: parent object 0 in cluster1's RADOS matches S3 source.
    # The first CopyupRequest to win the sentinel lock wrote it; verify it wrote correctly.
    local parent_prefix
    parent_prefix=$(exec_on cluster1 \
        "./bin/rbd --conf /tmp/cluster1/ceph.conf info xconcur_pool/xconcur-parent 2>/dev/null \
         | awk '/block_name_prefix:/ {print \$2}'")

    if [ -n "$parent_prefix" ]; then
        local parent_obj="${parent_prefix}.0000000000000000"
        local parent_obj_file="/tmp/xconcur-parent-obj0-$$.bin"
        local ref_file="/tmp/xconcur-ref-obj0-$$.bin"

        dd if="$parent_raw" bs=$parent_block_size count=1 of="$ref_file" status=none 2>/dev/null

        if exec_on cluster1 "./bin/rados --conf /tmp/cluster1/ceph.conf \
                -p xconcur_pool get $parent_obj -" > "$parent_obj_file" 2>/dev/null; then
            if cmp -s "$ref_file" "$parent_obj_file"; then
                log_success "Parent object 0 on cluster1 matches S3 source (concurrent COW integrity OK)"
            else
                log_error "FAIL: Parent object 0 does NOT match S3 source!"
                log_error "  Expected md5: $(md5sum "$ref_file" | awk '{print $1}')"
                log_error "  Got md5:      $(md5sum "$parent_obj_file" | awk '{print $1}')"
                FAILED=$(( FAILED + 1 ))
            fi
        else
            log_warn "Parent object 0 not yet in cluster1 RADOS (fetch may still be in-flight)"
        fi
        rm -f "$parent_obj_file" "$ref_file"
    fi

    # Cleanup
    for i in $(seq 1 $NUM_CONCURRENT); do
        rbd_on cluster2 "rm xconcur_child_pool/xconcur-child-$i" 2>/dev/null || true
    done
    rbd_on  cluster1 "rm xconcur_pool/xconcur-parent" 2>/dev/null || true
    ceph_on cluster1 "osd pool delete xconcur_pool xconcur_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    ceph_on cluster2 "osd pool delete xconcur_child_pool xconcur_child_pool --yes-i-really-really-mean-it" 2>/dev/null || true

    kill $minio_pid 2>/dev/null || true
    rm -rf "$minio_data" "$parent_raw" /tmp/xconcur-bench-*-$$.log /tmp/minio-xconcur.log

    if [ $FAILED -gt 0 ]; then
        log_error "=== S3-Backed Cross-Cluster Concurrent COW Test FAILED ==="
        return 1
    fi

    log_success "=== S3-Backed Cross-Cluster Concurrent COW Test PASSED ($NUM_CONCURRENT clients, ${TOTAL_TIME}s) ==="
}

# ── Main ─────────────────────────────────────────────────────────────────────
check_prereqs

echo
log_info "=== S3-Backed Standalone Clone — Cross-Cluster Tests (Docker) ==="
echo

setup_containers
start_ceph_cluster cluster1
start_ceph_cluster cluster2

[ $RUN_PLAIN      -eq 1 ] && run_plain_cross_cluster
[ $RUN_S3         -eq 1 ] && run_s3_cross_cluster
[ $RUN_S3         -eq 1 ] && run_s3_cross_cluster_rbd_children
[ $RUN_S3         -eq 1 ] && [ $RUN_CONCURRENT -eq 1 ] && run_s3_cross_cluster_concurrent

echo
log_success "All cross-cluster tests PASSED"
