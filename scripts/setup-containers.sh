#!/bin/bash
# Script to set up both Ceph clusters for cross-cluster testing

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== Setting up containerized multi-cluster Ceph environment ==="

# Check if docker-compose is available
if ! command -v docker-compose &> /dev/null; then
    echo "Error: docker-compose not found. Please install docker-compose."
    exit 1
fi

# Pull base image first
echo "Pulling debian:stable base image..."
docker pull debian:stable

# Build and start containers (disable BuildKit)
echo "Building and starting containers..."
cd "$SCRIPT_DIR/.."
DOCKER_BUILDKIT=0 docker-compose up -d --build

# Wait for containers to be ready
echo "Waiting for containers to be ready..."
sleep 5

# Function to execute command in a container
exec_in_cluster() {
    local cluster=$1
    shift
    docker exec -u cephdev "ceph-$cluster" "$@"
}

# Build Ceph in cluster1 if not already built
echo "=== Checking build in cluster1 ==="
if ! exec_in_cluster cluster1 test -f /ceph/build/bin/ceph &>/dev/null; then
    echo "Building Ceph in cluster1 (this may take a while)..."
    exec_in_cluster cluster1 bash -c "cd /ceph && ./do_cmake.sh && cd build && ninja -j\$(nproc)"
else
    echo "Ceph already built in cluster1"
fi

# Build Ceph in cluster2 (can reuse build from cluster1 due to shared volume)
echo "=== Checking build in cluster2 ==="
if exec_in_cluster cluster2 test -f /ceph/build/bin/ceph &>/dev/null; then
    echo "Build already available in cluster2 (shared volume)"
else
    echo "WARNING: cluster2 cannot see the build — shared volume not configured?"
fi

echo ""
echo "=== Container setup complete ==="
echo ""
echo "Next steps:"
echo "1. Start cluster1: ./scripts/start-cluster.sh cluster1"
echo "2. Start cluster2: ./scripts/start-cluster.sh cluster2"
echo "3. Run cross-cluster test: ./scripts/test-cross-cluster.sh"
echo ""
echo "Or run all at once: ./scripts/run-all.sh"
