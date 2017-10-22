Building
========

Seastar
-------

Clone and build Seastar in an external location, i.e. ~/seastar.

    $ git clone https://github.com/scylladb/seastar.git
    $ cd seastar
    $ git submodule update --init
    $ ./install_dependencies.sh
    $ ./configure.py
    $ ninja

Ceph
----

Ceph relies on the pkg-config output file seastar.pc for the correct compiler and linker flags. Use PKG_CONFIG_PATH to point at the desired Seastar build configuration (~/seastar/build/release or ~/seastar/build/debug).

    $ cd ~/ceph/build
    $ PKG_CONFIG_PATH=~/seastar/build/release cmake -DWITH_SEASTAR=ON ..
    $ make unittest_seastar_messenger -j7 && bin/unittest_seastar_messenger
