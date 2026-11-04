# Both pinned Mooncake profiles omit gflags.h in real_client.cpp when glog
# uses WITH_GFLAGS=OFF. Inject it only into this translation unit without
# modifying the upstream checkout.
if(CMAKE_VERSION VERSION_LESS 3.19)
    message(FATAL_ERROR "Mooncake Store build compatibility requires CMake >= 3.19")
endif()

function(mooncake_store_gflags_compat)
    if(NOT WITH_STORE)
        return()
    endif()
    set(store_src "${CMAKE_SOURCE_DIR}/mooncake-store/src")
    if(NOT EXISTS "${store_src}/real_client.cpp")
        message(FATAL_ERROR "Pinned Mooncake real_client.cpp is missing")
    endif()
    set_property(SOURCE "${store_src}/real_client.cpp"
                 DIRECTORY "${store_src}"
                 APPEND PROPERTY COMPILE_OPTIONS -include gflags/gflags.h)
    message(STATUS "Mooncake compatibility: include gflags/gflags.h for real_client.cpp")
endfunction()

# The project() hook runs before add_subdirectory; wait for the Store directory.
cmake_language(DEFER CALL mooncake_store_gflags_compat)
