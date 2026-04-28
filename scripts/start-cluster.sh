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

# Each cluster owns its own state directory under /tmp/$CLUSTER/.  Both
# containers bind-mount the same host /ceph/ (so they share the binaries,
# headers, libraries, etc.), but writing per-cluster mon/osd state to
# /ceph/build/{dev,out} would cause cluster2's `vstart -n` to wipe
# cluster1's data files (rm -rf dev) and overwrite ceph.conf.
#
# Pointing CEPH_DEV_DIR / CEPH_OUT_DIR / CEPH_CONF_PATH at /tmp/$CLUSTER/
# isolates the writable state per container.  vstart.sh honours these env
# vars (see src/vstart.sh ~line 100, the
#   [ -z "$CEPH_DEV_DIR" ] && CEPH_DEV_DIR="$CEPH_DIR/dev"
# fallbacks), so once set it writes ceph.conf, keyring, dev/, out/, and
# asok files all under /tmp/$CLUSTER/.
docker exec -u cephdev "ceph-$CLUSTER" bash -c "
    set -e

    export CEPH_CONF_PATH=/tmp/$CLUSTER
    export CEPH_DEV_DIR=/tmp/$CLUSTER/dev
    export CEPH_OUT_DIR=/tmp/$CLUSTER/out
    export CEPH_ASOK_DIR=/tmp/$CLUSTER/out

    mkdir -p \$CEPH_CONF_PATH \$CEPH_DEV_DIR \$CEPH_OUT_DIR

    cd /ceph/build

    # Stop any prior cluster *for this container*.  killall is scoped to
    # this container's PID namespace, so it can't harm cluster1's daemons
    # from cluster2's container — but the stale dev/out content under
    # /tmp/\$CLUSTER/ from a previous run could.
    ../src/stop.sh || true
    rm -rf \$CEPH_OUT_DIR/* \$CEPH_DEV_DIR/*

    # Start a new cluster.  vstart honours the CEPH_*_DIR env vars set above.
    MON=1 OSD=3 MDS=0 MGR=1 RGW=0 ../src/vstart.sh -n -d --without-dashboard

    # Sanity check: verify ceph.conf landed in our per-cluster path (not
    # in /ceph/build/).
    if [ ! -f \$CEPH_CONF_PATH/ceph.conf ]; then
        echo 'ERROR: ceph.conf not at \$CEPH_CONF_PATH after vstart' >&2
        exit 1
    fi

    sleep 5
    ./bin/ceph --conf \$CEPH_CONF_PATH/ceph.conf -s

    echo ''
    echo '=== $CLUSTER started successfully ==='
    echo 'Cluster status:'
    ./bin/ceph --conf \$CEPH_CONF_PATH/ceph.conf -s
"

echo ""
echo "=== $CLUSTER is ready ==="
