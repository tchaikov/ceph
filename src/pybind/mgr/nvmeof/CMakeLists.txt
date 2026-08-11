# Generate the Python bindings for the NVMe-oF gateway API from the
# proto file in the ceph-nvmeof submodule, using the protobuf the build
# resolved so the generated code always matches the protobuf runtime the
# target distro ships. The output lands in the source tree, as the
# dashboard frontend build does, so that the module install rule in
# ../CMakeLists.txt and vstart environments both find it.
#
# protoc runs from the mgr directory with the module-relative path so
# that the generated grpc stub imports gateway_pb2 through the
# "nvmeof.proto" package instead of a bare top-level import.
# Generate with the protoc the rest of the build resolved rather than
# whatever turns up first on PATH, so the bindings cannot come from a
# different protobuf than ceph itself was built against. src/CMakeLists.txt
# looks protobuf up the same way for the gateway monitor client, but it
# does so after this directory has been processed, so ask here too - the
# results are cached, so the second lookup costs nothing.
find_package(Protobuf CONFIG)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
if(CMAKE_CROSSCOMPILING)
  # protobuf::protoc is built for the target, and this has to run on the
  # host, so take the host's protoc as src/CMakeLists.txt does
  find_program(PROTOC_EXECUTABLE protoc REQUIRED)
else()
  set(PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
endif()
# grpc_python_plugin has no imported target to borrow: ceph finds gRPC
# through pkg-config on distros that ship no cmake config for it, and
# that path defines no targets. The plugin protocol is stable across
# versions, so the host's plugin is fine with the protoc chosen above.
find_program(GRPC_PYTHON_PLUGIN_EXECUTABLE grpc_python_plugin REQUIRED)

get_filename_component(mgr_source_dir ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
get_filename_component(nvmeof_proto_src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../nvmeof/gateway/control/proto/gateway.proto
  ABSOLUTE)
set(nvmeof_pb2_outputs
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/gateway.proto
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/gateway_pb2.py
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/gateway_pb2_grpc.py)

add_custom_command(
  OUTPUT ${nvmeof_pb2_outputs}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${nvmeof_proto_src}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/gateway.proto
  COMMAND ${PROTOC_EXECUTABLE}
    --proto_path=.
    --python_out=.
    --grpc_python_out=.
    --plugin=protoc-gen-grpc_python=${GRPC_PYTHON_PLUGIN_EXECUTABLE}
    --experimental_allow_proto3_optional
    nvmeof/proto/gateway.proto
  WORKING_DIRECTORY ${mgr_source_dir}
  # depend on the tools too: a protoc upgrade must regenerate the
  # bindings so the gencode keeps matching the distro runtime
  DEPENDS ${nvmeof_proto_src}
    ${PROTOC_EXECUTABLE}
    ${GRPC_PYTHON_PLUGIN_EXECUTABLE}
  COMMENT "Generating nvmeof mgr module protobuf bindings"
  VERBATIM)

add_custom_target(nvmeof-python-protobufs ALL
  DEPENDS ${nvmeof_pb2_outputs})
