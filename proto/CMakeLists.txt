# Schema-first worker interface contract. Generates C++ from worker_link.proto;
# the Rust API (zm-api) generates its own side from the same .proto via prost.
find_package(protobuf CONFIG REQUIRED)

# Export the exact protobuf include dir used to generate the code, so consumers
# (zmcore) can put it AHEAD of any other protobuf on the include path (e.g. a
# newer Homebrew protobuf pulled in by onnxruntime, whose headers are ABI-
# incompatible with the generated sources).
get_target_property(_zm_pb_inc protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
set(ZM_PROTOBUF_INCLUDE_DIR "${_zm_pb_inc}" CACHE INTERNAL "protobuf include used for codegen")

add_library(zm_proto STATIC worker_link.proto)
target_link_libraries(zm_proto PUBLIC protobuf::libprotobuf)
# Generated worker_link.pb.h lands here; expose it to consumers (e.g. zmcore).
target_include_directories(zm_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(zm_proto PUBLIC cxx_std_17)

# protobuf 5.x generated code is not -Wall/-Werror clean; keep this target lenient.
protobuf_generate(
    TARGET zm_proto
    LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)
