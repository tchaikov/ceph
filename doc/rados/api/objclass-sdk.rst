===========================
SDK for Ceph Object Classes
===========================

`Ceph` can be extended by creating shared object classes called `Ceph Object 
Classes`. The existing framework to build these object classes has dependencies 
on the internal functionality of `Ceph`, which restricts users to build object 
classes within the tree. The aim of this project is to create an independent 
object class interface, which can be used to build object classes outside the 
`Ceph` tree. This allows us to have two types of object classes, 1) those that 
have in-tree dependencies and reside in the tree and 2) those that can make use 
of the `Ceph Object Class SDK framework` and can be built outside of the `Ceph` 
tree because they do not depend on any internal implementation of `Ceph`. This 
project decouples object class development from Ceph and encourages creation 
and distribution of object classes as packages.

In order to demonstrate the use of this framework, we have two examples. The 
first one is called ``cls_sdk``, which is a very simple object class that 
makes use of the SDK framework. This object class resides in the ``src/cls`` 
directory. 

The second example is a project called, 
`ZLog <http://noahdesu.github.io/zlog/#>`_ , which is a strongly 
consistent shared-log designed to run at high-performance over distributed 
storage systems. `ZLog` makes use of `Ceph` as a storage backend to provide 
high performance and reliability. `ZLog` was earlier dependent on `Ceph's` 
``encoding.h`` interface. Now, this dependency has been eliminated by making 
use of serialization and deserialization methods provided by `Google Protocol 
Buffers`. `ZLog` can now be built using `Ceph's Object Class SDK framework` 
and users only need to setup the appropriate `Protobuf Packages 
<https://github.com/google/protobuf>`_, based on their platform. The 
``cls_zlog`` plugin resides in the ``src/cls/`` directory as an example of an 
object class that can be built by making using of the SDK interface. The 
`ZLog` project and unittests reside 
`here <https://github.com/noahdesu/zlog>`_. This project is evolving 
and we are hoping to see a slightly different version of the plugin soon. 

Installing objclass.h
---------------------

The object class interface that enables out-of-tree development of object 
classes resides in ``src/include/rados/`` and gets installed with `Ceph` 
installation. After running ``make install``, you should be able to see it 
in ``<prefix>/include/rados``. ::

        ls /usr/local/include/rados

Using the SDK examples
----------------------

1. The ``cls_sdk`` object class resides in ``src/cls/sdk/``. This gets built and 
loaded into Ceph, with the Ceph build process. You can run the 
``ceph_test_cls_sdk`` unittest, which resides in ``src/test/cls_sdk/``, 
to test this class.

2. As for the ``cls_zlog`` object class, the plugin gets built and loaded into 
`Ceph` with its build process. The unittests can be run separately by cloning 
and building https://github.com/noahdesu/zlog and following the steps provided 
in the documentation.
