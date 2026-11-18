set(spdlog_VERSION 1.12.0)
if (NOT "$ENV{DS_LOCAL_LIBS_DIR}" STREQUAL "")
  set(spdlog_URL "$ENV{DS_LOCAL_LIBS_DIR}/opensource_third_party/v1.12.0.zip")
else()
  set(spdlog_URL "https://gitee.com/mirror-luyi/spdlog/repository/archive/v1.12.0.zip")
endif()
set(spdlog_SHA256 "a0a22ed8e4932cf5f7abc354fc96d20217a8b45471e732d5e672cc01979fe588")

set(spdlog_CXX_FLAGS ${THIRDPARTY_SAFE_FLAGS})
set(spdlog_LINK_FLAGS ${THIRDPARTY_SAFE_FLAGS})

set(spdlog_CMAKE_OPTIONS
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DSPDLOG_BUILD_SHARED:BOOL=ON)

set(spdlog_PATCHES
    ${DS_SOURCE_DIR}/third_party/patches/spdlog/change-filename.patch
    ${DS_SOURCE_DIR}/third_party/patches/spdlog/change-namespace.patch
    ${DS_SOURCE_DIR}/third_party/patches/spdlog/change-rotating-file-sink.patch
    ${DS_SOURCE_DIR}/third_party/patches/spdlog/recover-from-broken-sink.patch)

add_thirdparty_lib(SPDLOG
    URL ${spdlog_URL}
    SHA256 ${spdlog_SHA256}
    FAKE_SHA256 ${spdlog_FAKE_SHA256}
    VERSION ${spdlog_VERSION}
    CXX_FLAGS ${spdlog_CXX_FLAGS}
    LINK_FLAGS ${spdlog_LINK_FLAGS}
    CONF_OPTIONS ${spdlog_CMAKE_OPTIONS}
    PATCHES ${spdlog_PATCHES})

if (DS_BUILD_CLIENT_ONLY)
    file(GLOB _DS_SPDLOG_CONFIG "${SPDLOG_ROOT}/lib*/cmake/spdlog/spdlogConfig.cmake")
    if (NOT _DS_SPDLOG_CONFIG)
        message(FATAL_ERROR "Patched ds_spdlog package config missing under ${SPDLOG_ROOT}")
    endif()
    list(GET _DS_SPDLOG_CONFIG 0 _DS_SPDLOG_CONFIG_FILE)
    include("${_DS_SPDLOG_CONFIG_FILE}")
else()
    find_library(SPDLOG
        NAMES ds-spdlog
        PATHS ${SPDLOG_ROOT}
        PATH_SUFFIXES lib lib64
        REQUIRED
        NO_DEFAULT_PATH)
    set(CMAKE_PREFIX_PATH ${SPDLOG_ROOT})
    find_package(spdlog ${spdlog_VERSION} REQUIRED)
endif()
get_property(spdlog_INCLUDE_DIR TARGET ds_spdlog::spdlog PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
include_directories(SYSTEM ${spdlog_INCLUDE_DIR})
