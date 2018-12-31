#!/bin/sh -x
git submodule update --init --recursive
if test -e build; then
    echo 'build dir already exists; rm -rf build and re-run'
    exit 1
fi

PYBUILD="2"
source /etc/os-release
case "$ID" in
    fedora)
        if [ "$VERSION_ID" -ge "29" ] ; then
            PYBUILD="3"
        fi
        ;;
    rhel|centos)
        MAJOR_VER=$(echo "$VERSION_ID" | sed -e 's/\..*$//')
        if [ "$MAJOR_VER" -ge "8" ] ; then
            PYBUILD="3"
        fi
        ;;
    opensuse*|suse|sles)
        PYBUILD="3"
        ;;
esac
if [ "$PYBUILD" = "3" ] ; then
    ARGS="$ARGS -DWITH_PYTHON2=OFF -DWITH_PYTHON3=ON -DMGR_PYTHON_VERSION=3"
fi

#ARGS="${ARGS} -DCMAKE_BUILD_TYPE=Release"
args_build_type=$(echo $ARGS | sed 's/^.*\?-DCMAKE_BUILD_TYPE=\(\w\+\).*\?$/\1/')
if [ "$args_build_type" = "" ]; then
	echo "No cmake build type provided in ARGS, using default => Debug"
	ARGS="-DCMAKE_BUILD_TYPE=Debug $ARGS"
else
	echo "Using cmake build type $args_build_type"
fi

if type ccache > /dev/null 2>&1 ; then
    echo "enabling ccache"
    ARGS="$ARGS -DWITH_CCACHE=ON"
fi

mkdir build
cd build
if type cmake3 > /dev/null 2>&1 ; then
    CMAKE=cmake3
else
    CMAKE=cmake
fi
${CMAKE} $ARGS "$@" .. || exit 1

# minimal config to find plugins
cat <<EOF > ceph.conf
plugin dir = lib
erasure code dir = lib
EOF

echo done.
cat <<EOF

****
WARNING: do_cmake.sh now creates debug builds by default. Performance
may be severely affected. Please use -DCMAKE_BUILD_TYPE=RelWithDebInfo
if a performance sensitive build is required.
****
EOF
