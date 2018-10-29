#!/usr/bin/env bash
set -ex

############################################
#			Helper functions
############################################
function install() {
    for package in "$@" ; do
        install_one $package
    done
    return 0
}

function install_one() {
    case $(lsb_release -si) in
        Ubuntu|Debian|Devuan)
            sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y "$@"
            ;;
        CentOS|Fedora|RedHatEnterpriseServer)
            sudo yum install -y "$@"
            ;;
        *SUSE*)
            sudo zypper --non-interactive install "$@"
            ;;
        *)
            echo "$(lsb_release -si) is unknown, $@ will have to be installed manually."
            ;;
    esac
}
############################################
#			Install required tools
############################################
echo "Install required tools"
install git cmake

CURRENT_PATH=`pwd`

############################################
#			Compile&Start RocksDB
############################################
# install prerequisites
# for rocksdb
case $(lsb_release -si) in
	Ubuntu|Debian|Devuan)
		install g++ libsnappy-dev zlib1g-dev libbz2-dev libradospp-dev
		;;
	CentOS|Fedora|RedHatEnterpriseServer)
		install gcc-c++.x86_64 snappy-devel zlib zlib-devel bzip2 bzip2-devel libradospp-devel.x86_64
		;;
	*)
        echo "$(lsb_release -si) is unknown, $@ will have to be installed manually."
        ;;
esac

# # gflags
# sudo yum install gflags-devel
# 
# wget https://github.com/schuhschuh/gflags/archive/master.zip
# unzip master.zip
# cd gflags-master
# mkdir build && cd build
# export CXXFLAGS="-fPIC" && cmake .. && make VERBOSE=1
# make && make install

# # snappy-devel


echo "Compile rocksdb"
if [ -e rocksdb ]; then
	rm -fr rocksdb
fi
git clone https://github.com/facebook/rocksdb.git --depth 1

# compile code
cd rocksdb
patch -p1 <<EOF
From 97cca51a0def5a85773b76bd7d63b10f4b15fbda Mon Sep 17 00:00:00 2001
From: Kefu Chai <tchaikov@gmail.com>
Date: Mon, 29 Oct 2018 17:37:32 +0800
Subject: [PATCH] cmake: add Findrados.cmake and use it

ceph has extracted libradospp out from librados. the former offers the
C++ API, while the latter offers the C API.

Signed-off-by: Kefu Chai <tchaikov@gmail.com>
---
 CMakeLists.txt                |  3 ++-
 cmake/modules/Findrados.cmake | 30 ++++++++++++++++++++++++++++++
 2 files changed, 32 insertions(+), 1 deletion(-)
 create mode 100644 cmake/modules/Findrados.cmake

diff --git a/CMakeLists.txt b/CMakeLists.txt
index 6cb80cd10..48c61f928 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -731,7 +731,8 @@ option(WITH_LIBRADOS "Build with librados" OFF)
 if(WITH_LIBRADOS)
   list(APPEND SOURCES
     utilities/env_librados.cc)
-  list(APPEND THIRDPARTY_LIBS rados)
+  find_package(radospp REQUIRED)
+  list(APPEND THIRDPARTY_LIBS rados::radospp)
 endif()

 if(WIN32)
diff --git a/cmake/modules/Findrados.cmake b/cmake/modules/Findrados.cmake
new file mode 100644
index 000000000..e68e176ed
--- /dev/null
+++ b/cmake/modules/Findrados.cmake
@@ -0,0 +1,30 @@
+# - Find RADOS
+# Find librados library and includes
+#
+# RADOS_INCLUDE_DIR - where to find librados.hpp.
+# RADOS_LIBRARIES - List of libraries when using radospp.
+# rados_FOUND - True if radospp found.
+
+find_path(RADOSPP_INCLUDE_DIR
+  NAMES librados.hpp
+  HINTS ${LIBRADOS_ROOT}/include)
+
+find_library(RADOSPP_LIBRARIES
+  NAMES radospp
+  HINTS ${LIBRADOS_ROOT}/lib)
+
+include(FindPackageHandleStandardArgs)
+find_package_handle_standard_args(radospp
+  DEFAULT_MSG RADOSPP_INCLUDE_DIR RADOSPP_LIBRARIES)
+
+mark_as_advanced(
+  RADOSPP_INCLUDE_DIR
+  RADOSPP_LIBRARIES)
+
+if(radospp_FOUND AND NOT (TARGET rados::radospp))
+  add_library(rados::radospp UNKNOWN IMPORTED)
+  set_target_properties(rados::radospp PROPERTIES
+    INTERFACE_INCLUDE_DIRECTORIES "${RADOSPP_INCLUDE_DIR}"
+    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
+    IMPORTED_LOCATION "${RADOSPP_LIBRARIES}")
+endif()
--
2.17.0
EOF

mkdir build && cd build && cmake -DWITH_LIBRADOS=ON -DWITH_SNAPPY=ON -DWITH_GFLAGS=OFF -DFAIL_ON_WARNINGS=OFF ..
make rocksdb_env_librados_test -j8

echo "Copy ceph.conf"
# prepare ceph.conf
mkdir -p ../ceph/src/
if [ -f "/etc/ceph/ceph.conf" ]; then
    cp /etc/ceph/ceph.conf ../ceph/src/
elif [ -f "/etc/ceph/ceph/ceph.conf" ]; then
	cp /etc/ceph/ceph/ceph.conf ../ceph/src/
else 
	echo "/etc/ceph/ceph/ceph.conf doesn't exist"
fi

echo "Run EnvLibrados test"
# run test
if [ -f "../ceph/src/ceph.conf" ]
	then
	cp env_librados_test ~/cephtest/archive
	./env_librados_test
else 
	echo "../ceph/src/ceph.conf doesn't exist"
fi
cd ${CURRENT_PATH}
