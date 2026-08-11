#!/usr/bin/env python3
"""Generate the Python protobuf bindings of the nvmeof mgr module.

The cmake build generates them with the protoc of the build host (see
src/pybind/mgr/nvmeof/CMakeLists.txt); this script is the tox/manual
path, generating them with grpcio-tools so the gencode matches the
protobuf runtime installed in the test environment.

Exits 0 without generating when the ceph-nvmeof submodule or
grpcio-tools is unavailable, so lint-only environments keep working;
tests that import the bindings will then fail with a plain
ImportError on nvmeof.proto.gateway_pb2.
"""
import shutil
import sys
from pathlib import Path

CEPH_ROOT = Path(__file__).resolve().parents[2]
PROTO_SRC = CEPH_ROOT / 'src/nvmeof/gateway/control/proto/gateway.proto'
MGR_DIR = CEPH_ROOT / 'src/pybind/mgr'
OUT_DIR = MGR_DIR / 'nvmeof/proto'


def main() -> int:
    if not PROTO_SRC.exists():
        print(f'{PROTO_SRC} not found (submodule not initialized); '
              'not generating the nvmeof protobuf bindings', file=sys.stderr)
        return 0
    try:
        from grpc_tools import protoc
    except ImportError:
        print('grpcio-tools is not installed; not generating the nvmeof '
              'protobuf bindings', file=sys.stderr)
        return 0
    shutil.copyfile(PROTO_SRC, OUT_DIR / 'gateway.proto')
    ret = protoc.main([
        'protoc',
        f'--proto_path={MGR_DIR}',
        f'--python_out={MGR_DIR}',
        f'--grpc_python_out={MGR_DIR}',
        '--experimental_allow_proto3_optional',
        str(OUT_DIR / 'gateway.proto'),
    ])
    if ret == 0:
        print(f'generated nvmeof protobuf bindings in {OUT_DIR}')
    return ret


if __name__ == '__main__':
    sys.exit(main())
