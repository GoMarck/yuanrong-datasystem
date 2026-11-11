cmake_minimum_required(VERSION 3.18)
project(rpc_isolation_bench LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gflags CONFIG REQUIRED)
find_path(BRPC_INCLUDE_DIR brpc/channel.h REQUIRED HINTS "${BRPC_ROOT}/include")
find_library(BRPC_LIBRARY brpc REQUIRED HINTS "${BRPC_ROOT}/lib64" "${BRPC_ROOT}/lib")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rpc_isolation.pb.cc" "${CMAKE_CURRENT_BINARY_DIR}/rpc_isolation.pb.h"
    COMMAND protobuf::protoc "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}" "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/rpc_isolation.proto"
    DEPENDS rpc_isolation.proto)
add_executable(brpc_isolation_bench brpc_isolation_bench.cpp "${CMAKE_CURRENT_BINARY_DIR}/rpc_isolation.pb.cc")
target_include_directories(brpc_isolation_bench PRIVATE "${BRPC_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(brpc_isolation_bench PRIVATE "${BRPC_LIBRARY}" protobuf::libprotobuf gflags::gflags Threads::Threads)
