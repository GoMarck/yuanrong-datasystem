foreach(_library absl re2 zlib libzmq jemalloc securec tbb nlohmann_json openssl libcurl protobuf grpc
        spdlog gflags leveldb brpc)
    include("${DS_SOURCE_DIR}/cmake/external_libs/${_library}.cmake")
    get_property(_prefixes GLOBAL PROPERTY DS_PRIVATE_SDK_PREFIXES)
    set(CMAKE_PREFIX_PATH ${_prefixes})
endforeach()
include("${DS_SOURCE_DIR}/cmake/external_libs/etcdapi.cmake")
