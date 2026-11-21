cmake_minimum_required(VERSION 3.20)
if (TARGET Datasystem::client_static)
    message(FATAL_ERROR "A DataSystem static SDK is already present in this build")
endif()
if (CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The embedded static SDK requires a native Linux toolchain")
endif()

set(_jobs "${DS_PRIVATE_SDK_JOBS}")
if (_jobs STREQUAL "")
    set(_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
endif()
if (_jobs STREQUAL "")
    set(_jobs 8)
endif()
if (NOT _jobs MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "DS_PRIVATE_SDK_JOBS requires a positive integer, got '${_jobs}'")
endif()

get_filename_component(_source "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_producer "${CMAKE_CURRENT_BINARY_DIR}/producer")
set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/sdk")
set(_env "${CMAKE_COMMAND}" -E env --unset=CFLAGS --unset=CXXFLAGS --unset=CPPFLAGS --unset=LDFLAGS
    --unset=CPATH --unset=CPLUS_INCLUDE_PATH --unset=C_INCLUDE_PATH --unset=LIBRARY_PATH
    --unset=CMAKE_PREFIX_PATH --unset=CMAKE_TOOLCHAIN_FILE --unset=PKG_CONFIG_PATH
    "CMAKE_BUILD_PARALLEL_LEVEL=${_jobs}")
set(_options "-DCMAKE_INSTALL_PREFIX=${_prefix}" "-DBUILD_THREAD_NUM=${_jobs}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}" "-DDS_GLIBCXX_USE_CXX11_ABI=${DS_GLIBCXX_USE_CXX11_ABI}")
if (CMAKE_C_COMPILER)
    list(APPEND _options "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if (DS_OPENSOURCE_DIR)
    list(APPEND _options "-DDS_OPENSOURCE_DIR=${DS_OPENSOURCE_DIR}")
endif()
execute_process(COMMAND ${_env} "${CMAKE_COMMAND}" -S "${_source}" -B "${_producer}" -G "Unix Makefiles"
    -DDS_BUILD_STATIC_SDK=ON -DCMAKE_BUILD_TYPE=Release ${_options} COMMAND_ERROR_IS_FATAL ANY)

file(GLOB_RECURSE _recipes CONFIGURE_DEPENDS "${_source}/cmake/*.cmake" "${_source}/cmake/*.cmake.in"
    "${_source}/src/CMakeLists.txt" "${_source}/third_party/patches/*.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}/CMakeLists.txt" ${_recipes})

set(_package "${_producer}/private-sdk-package")
file(STRINGS "${_package}/archives.txt" _archives)
list(TRANSFORM _archives PREPEND "${_prefix}/lib/datasystem-private/")
file(MAKE_DIRECTORY "${_prefix}/include")
find_package(Threads REQUIRED)
set(_DS_PRIVATE_PREFIX "${_prefix}")
set(_DS_PRIVATE_IMPORT_SCOPE GLOBAL)
include("${_package}/DatasystemPrivateTargets.cmake")
add_custom_target(datasystem_private_sdk_install
    COMMAND ${_env} "${CMAKE_COMMAND}" --build "${_producer}" --parallel "${_jobs}" --target install
    BYPRODUCTS ${_archives} "${_prefix}/lib/datasystem-private/tls.o"
    VERBATIM)
add_dependencies(Datasystem::client_static datasystem_private_sdk_install)
