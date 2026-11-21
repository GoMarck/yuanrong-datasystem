# Build the libs grpc depends
# c-ares
set(c-ares_VERSION 1.19.1)
if (NOT "$ENV{DS_LOCAL_LIBS_DIR}" STREQUAL "")
  set(c-ares_URL "$ENV{DS_LOCAL_LIBS_DIR}/opensource_third_party/cares-1_19_1.zip")
else()
  set(c-ares_URL "https://gitee.com/mirrors/c-ares/repository/archive/cares-1_19_1.zip")
endif()
set(c-ares_SHA256 "edcaac184aff0e6b6eb7b9ede7a55f36c7fc04085d67fecff2434779155dd8ce")

set(c-ares_CMAKE_OPTIONS
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DCARES_SHARED:BOOL=OFF
    -DCARES_STATIC:BOOL=ON
    -DCARES_STATIC_PIC:BOOL=ON
    -DHAVE_LIBNSL:BOOL=OFF)

set(c-ares_CXX_FLAGS ${THIRDPARTY_SAFE_FLAGS})

add_thirdparty_lib(c-ares
  URL ${c-ares_URL}
  SHA256 ${c-ares_SHA256}
  FAKE_SHA256 ${c-ares_FAKE_SHA256}
  VERSION ${c-ares_VERSION}
  CONF_OPTIONS ${c-ares_CMAKE_OPTIONS}
  CXX_FLAGS ${c-ares_CXX_FLAGS})

if (EXISTS ${c-ares_ROOT}/lib64)
  set(c-ares_PKG_PATH ${c-ares_ROOT}/lib64/cmake/c-ares)
else()
  set(c-ares_PKG_PATH ${c-ares_ROOT}/lib/cmake/c-ares)
endif()

# grpc
set(gRPC_VERSION 1.65.4)
if (NOT "$ENV{DS_LOCAL_LIBS_DIR}" STREQUAL "")
  set(gRPC_URL "$ENV{DS_LOCAL_LIBS_DIR}/opensource_third_party/v1.65.4.tar.gz")
else()
  set(gRPC_URL "https://gitee.com/mirrors/grpc/repository/archive/v1.65.4.tar.gz")
endif()
set(gRPC_SHA256 "dd60131b74bd1ecb3ffd29a31a6d68d6bb009106bd22c4be140e40f5d2baf2f6")

if (EXISTS ${Protobuf_ROOT}/lib64)
  set(Protobuf_PKG_PATH ${Protobuf_ROOT}/lib64/cmake/protobuf)
  set(utf8_range_PKG_PATH ${Protobuf_ROOT}/lib64/cmake/utf8_range)
else()
  set(Protobuf_PKG_PATH ${Protobuf_ROOT}/lib/cmake/protobuf)
  set(utf8_range_PKG_PATH ${Protobuf_ROOT}/lib/cmake/utf8_range)
endif()

set(_ORG_LD_PATH $ENV{LD_LIBRARY_PATH})
set(ENV{LD_LIBRARY_PATH} "${Protobuf_LIB_PATH}:${_ORG_LD_PATH}")

set(gRPC_CMAKE_OPTIONS
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DBUILD_SHARED_LIBS:BOOL=ON
    -DgRPC_INSTALL:BOOL=ON
    -DgRPC_DOWNLOAD_ARCHIVES:BOOL=OFF
    -DgRPC_BUILD_TESTS:BOOL=OFF
    -DgRPC_PROTOBUF_PROVIDER:STRING=package
    -Dutf8_range_DIR:PATH=${utf8_range_PKG_PATH}
    -DProtobuf_DIR:PATH=${Protobuf_PKG_PATH}
    -DgRPC_ABSL_PROVIDER:STRING=package
    -Dabsl_DIR:PATH=${absl_PKG_PATH}
    -DgRPC_CARES_PROVIDER:STRING=package
    -Dc-ares_DIR:PATH=${c-ares_PKG_PATH}
    -DgRPC_RE2_PROVIDER:STRING=package
    -Dre2_DIR:PATH=${re2_PKG_PATH}
    -DgRPC_SSL_PROVIDER:STRING=package
    -DOPENSSL_ROOT_DIR=${OpenSSL_ROOT}
    -DgRPC_ZLIB_PROVIDER:STRING=package
    -DZLIB_ROOT:PATH=${ZLIB_ROOT}
    -DCMAKE_CXX_STANDARD=17)

if (gRPC_VERSION STREQUAL "1.54.2")
  set(gRPC_CMAKE_OPTIONS
      ${gRPC_CMAKE_OPTIONS}
      -DgRPC_BUILD_ENVOY_API:BOOL=OFF
      -DgRPC_BUILD_GOOGLEAPIS:BOOL=OFF
      -DgRPC_BUILD_OPENCENSUS_PROTO:BOOL=OFF
      -DgRPC_BUILD_XDS:BOOL=OFF)
endif()

if (USE_SANITIZER)
    set(gRPC_CXX_FLAGS "${THIRDPARTY_SAFE_FLAGS} ${SANITIZER_FLAGS}")
    set(gRPC_C_FLAGS "${THIRDPARTY_SAFE_FLAGS} ${SANITIZER_FLAGS}")
    set(gRPC_LINK_FLAGS "${THIRDPARTY_SAFE_FLAGS} ${SANITIZER_FLAGS}")
else ()
    set(gRPC_CXX_FLAGS ${THIRDPARTY_SAFE_FLAGS})
    set(gRPC_C_FLAGS ${THIRDPARTY_SAFE_FLAGS})
    set(gRPC_LINK_FLAGS "${THIRDPARTY_SAFE_FLAGS} -Wl,-Bsymbolic")
endif ()

set(gRPC_EXTRA_MSGS
    ${c-ares_ROOT}
    ${re2_ROOT}
    ${absl_ROOT}
    ${Protobuf_ROOT}
    ${ZLIB_ROOT}
    ${OpenSSL_ROOT})

if (gRPC_VERSION STREQUAL "1.65.4")
  set(grpc_PATCHES
          ${DS_SOURCE_DIR}/third_party/patches/grpc/1.65.4/grpc_1_65_4_support_gcc_7_3.patch
  )
endif()

add_thirdparty_lib(gRPC
  URL ${gRPC_URL}
  SHA256 ${gRPC_SHA256}
  FAKE_SHA256 ${gRPC_FAKE_SHA256}
  VERSION ${gRPC_VERSION}
  CONF_OPTIONS ${gRPC_CMAKE_OPTIONS}
  CXX_FLAGS  ${gRPC_CXX_FLAGS}
  C_FLAGS  ${gRPC_C_FLAGS}
  LINK_FLAGS ${gRPC_LINK_FLAGS}
  EXTRA_MSGS ${gRPC_EXTRA_MSGS}
  PATCHES ${grpc_PATCHES})

set(ENV{LD_LIBRARY_PATH} "${_ORG_LD_PATH}")

set(gRPC_DIR ${gRPC_ROOT})
find_package(gRPC ${gRPC_VERSION} REQUIRED)

get_property(gRPC_INCLUDE_DIR TARGET gRPC::grpc++ PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
include_directories(SYSTEM ${gRPC_INCLUDE_DIR})
