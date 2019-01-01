#!/bin/bash

#PATH=/usr/lib/ccache:$PATH $DPKG_EXTRA_ENV dpkg-buildpackage $j -uc -us
export DEB_CFLAGS_SET="-Os" 
export DEB_CXXFLAGS_SET="-Os"
export DEB_CPPFLAGS_SET="-Os"
export CEPH_EXTRA_CMAKE_ARGS="-DCMAKE_BUILD_TYPE=\"Release\" -DCMAKE_C_FLAGS_RELEASE=\"-Os\" -DCMAKE_CXX_FLAGS_RELEASE=\"-Os\""

time ./make-debs.sh
