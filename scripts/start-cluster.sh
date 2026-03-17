#!/bin/bash
# Script to start a Ceph cluster in a container

set -e

CLUSTER=$1

if [ -z "$CLUSTER" ]; then
    echo "Usage: $0 <cluster1|cluster2>"
    exit 1
fi

if [ "$CLUSTER" != "cluster1" ] && [ "$CLUSTER" != "cluster2" ]; then
    echo "Error: Cluster must be 'cluster1' or 'cluster2'"
    exit 1
fi

echo "=== Starting Ceph $CLUSTER ==="

# Execute vstart in the container
docker exec -u cephdev "ceph-$CLUSTER" bash -c "
    cd /ceph/build

    # Clean up any existing cluster
    ../src/stop.sh || true
    rm -rf out dev

    # Start new cluster
    MON=1 OSD=3 MDS=0 MGR=1 RGW=0 ../src/vstart.sh -n -d

    # Wait for cluster to be healthy
    sleep 5
    ./bin/ceph -s

    echo ''
    echo '=== $CLUSTER started successfully ==='
    echo 'Cluster status:'
    ./bin/ceph -s
"

echo ""
echo "=== $CLUSTER is ready ==="
