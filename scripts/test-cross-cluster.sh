#!/bin/bash
# Test cross-cluster standalone clone:
#   - Parent image lives in cluster1
#   - Child image is created in cluster2
#   - clone-standalone is run from cluster2 with cluster1 as --remote-cluster-conf
#
# NOTE: clone-standalone requires parent_pool to exist on BOTH clusters
# (the local pool name is used to find the pool on the remote cluster).

set -e

echo "=== Testing Cross-Cluster Standalone Clone ==="

CLUSTER1_CONF="/tmp/cluster1/ceph.conf"
CLUSTER2_CONF="/tmp/cluster2/ceph.conf"

exec_on() {
    local cluster=$1
    local conf="/tmp/${cluster}/ceph.conf"
    shift
    docker exec -u cephdev "ceph-${cluster}" bash -c \
        "source /home/cephdev/.ceph_env && cd /ceph/build && $* --conf '${conf}'"
}

cleanup() {
    echo "=== Cleanup ==="
    exec_on cluster2 "./bin/rbd rm child_pool/child_image" 2>/dev/null || true
    exec_on cluster1 "./bin/rbd rm parent_pool/parent_image" 2>/dev/null || true
    exec_on cluster1 "./bin/ceph osd pool delete parent_pool parent_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    exec_on cluster2 "./bin/ceph osd pool delete parent_pool parent_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    exec_on cluster2 "./bin/ceph osd pool delete child_pool child_pool --yes-i-really-really-mean-it" 2>/dev/null || true
    echo "Cleanup done"
}
trap cleanup EXIT

# ============================================================================
echo ""
echo "=== Step 1: Create parent_pool in cluster1 and parent_image ==="
exec_on cluster1 "./bin/ceph osd pool create parent_pool 32" 2>&1 || true
exec_on cluster1 "./bin/rbd pool init parent_pool"
exec_on cluster1 "./bin/rbd create --size 100M parent_pool/parent_image"
echo "Created parent_pool/parent_image on cluster1"

# ============================================================================
echo ""
echo "=== Step 2: Create pools on cluster2 ==="
# parent_pool must exist on cluster2 (local IoCtx placeholder for cross-cluster)
exec_on cluster2 "./bin/ceph osd pool create parent_pool 32" 2>&1 || true
exec_on cluster2 "./bin/ceph osd pool create child_pool 32"  2>&1 || true
exec_on cluster2 "./bin/rbd pool init parent_pool"
exec_on cluster2 "./bin/rbd pool init child_pool"
echo "Created parent_pool (placeholder) and child_pool on cluster2"

# ============================================================================
echo ""
echo "=== Step 3: Exchange cluster connection info ==="
CLUSTER1_MON=$(docker exec ceph-cluster1 bash -c \
    "source /home/cephdev/.ceph_env && \
    /ceph/build/bin/ceph --conf /tmp/cluster1/ceph.conf mon dump --format json 2>/dev/null \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[\"mons\"][0][\"addr\"].split(\"/\")[0])'")
CLUSTER1_KEY=$(docker exec ceph-cluster1 bash -c \
    "source /home/cephdev/.ceph_env && \
    /ceph/build/bin/ceph --conf /tmp/cluster1/ceph.conf auth get-key client.admin")
echo "Cluster1 monitor: $CLUSTER1_MON"

docker exec -u cephdev ceph-cluster2 bash -c "mkdir -p /home/cephdev/.ceph
cat > /home/cephdev/.ceph/cluster1.conf << 'EOF2'
[global]
mon_host = ${CLUSTER1_MON}
EOF2
cat > /home/cephdev/.ceph/cluster1.keyring << 'EOF3'
[client.admin]
key = ${CLUSTER1_KEY}
EOF3
chmod 600 /home/cephdev/.ceph/cluster1.keyring"
echo "Wrote cluster1 access config into cluster2"

# ============================================================================
echo ""
echo "=== Step 4: Create cross-cluster standalone clone ==="
# Run from cluster2 (local=child cluster), parent is in cluster1 (remote)
docker exec -u cephdev ceph-cluster2 bash -c \
    "source /home/cephdev/.ceph_env && cd /ceph/build && \
    ./bin/rbd --conf /tmp/cluster2/ceph.conf clone-standalone \
        --remote-cluster-conf /home/cephdev/.ceph/cluster1.conf \
        --remote-keyring /home/cephdev/.ceph/cluster1.keyring \
        parent_pool/parent_image \
        child_pool/child_image 2>&1"
echo "Cross-cluster standalone clone created"

# ============================================================================
echo ""
echo "=== Step 5: Verify child image in cluster2 ==="
exec_on cluster2 "./bin/rbd info child_pool/child_image"

# ============================================================================
echo ""
echo "=== Step 6: Write to child (triggers COW — parent read from cluster1) ==="
exec_on cluster2 \
    "./bin/rbd bench child_pool/child_image \
    --io-type write --io-size 4M --io-total 4M --io-threads 1"
echo "Write to child completed"

# ============================================================================
echo ""
echo "=== Step 7: Verify RADOS objects created in cluster2 ==="
BLOCK_PREFIX=$(docker exec -u cephdev ceph-cluster2 bash -c \
    "source /home/cephdev/.ceph_env && \
    /ceph/build/bin/rbd --conf /tmp/cluster2/ceph.conf info child_pool/child_image 2>/dev/null \
    | awk '/block_name_prefix:/ {print \$2}'")
OBJ_COUNT=$(docker exec -u cephdev ceph-cluster2 bash -c \
    "source /home/cephdev/.ceph_env && \
    /ceph/build/bin/rados --conf /tmp/cluster2/ceph.conf -p child_pool ls 2>/dev/null \
    | grep -c \"^${BLOCK_PREFIX}\.\"" || echo "0")
echo "Child has $OBJ_COUNT RADOS objects (prefix: $BLOCK_PREFIX)"

# ============================================================================
echo ""
echo "=== Step 8: Flatten the child (makes it independent of cluster1) ==="
exec_on cluster2 "./bin/rbd flatten child_pool/child_image"
echo "Flatten completed"

# ============================================================================
echo ""
echo "=== Step 9: Verify child has no parent after flatten ==="
exec_on cluster2 "./bin/rbd info child_pool/child_image"

echo ""
echo "=== Cross-Cluster Standalone Clone Test PASSED ==="
