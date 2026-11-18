function(ds_proto_include_args output)
    get_target_property(_includes protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
    set(_args)
    foreach(_include ${_includes} ${DS_PROTO_IMPORT_DIRS})
        if (_include MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
            set(_include "${CMAKE_MATCH_1}")
        elseif (_include MATCHES "^\\$<INSTALL_INTERFACE:")
            continue()
        endif()
        if (_include AND NOT _include MATCHES "-NOTFOUND$")
            list(APPEND _args -I "${_include}")
        endif()
    endforeach()
    set(${output} ${_args} PARENT_SCOPE)
endfunction()

function(ds_protoc_command output)
    if (NOT DS_PROTOC_TARGET)
        set(DS_PROTOC_TARGET protobuf::protoc)
    endif()
    if (DS_USE_EXTERNAL_DEPS)
        set(${output} "$<TARGET_FILE:${DS_PROTOC_TARGET}>" PARENT_SCOPE)
    else()
        set(${output} ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=${gRPC_LIB_PATH}:${Protobuf_LIB_PATH}:${absl_LIB_PATH}:$ENV{LD_LIBRARY_PATH}"
            "$<TARGET_FILE:${DS_PROTOC_TARGET}>" PARENT_SCOPE)
    endif()
endfunction()

function(GENERATE_ZMQ_CPP ZMQ_PROTO_LIB_DEPEND source_files header_files target_directory proto_src_directory)
    if (NOT ARGN)
        message(SEND_ERROR "Error: ZMQ_GENERATE_CPP() called without any proto files")
        return()
    endif ()

    set(${source_files})
    set(${header_files})
    foreach (file ${ARGN})
        get_filename_component(abs_file ${file} ABSOLUTE)
        get_filename_component(file_name ${file} NAME_WE)

        file(RELATIVE_PATH _REL_PATH ${DS_SOURCE_DIR}/src ${abs_file})
        get_filename_component(_REL_DIR ${_REL_PATH} DIRECTORY)

        if(_REL_DIR)
          set(_OUTPUT_PREFIX "${target_directory}/${_REL_DIR}")
        else()
          set(_OUTPUT_PREFIX "${target_directory}")
        endif()

        file(MAKE_DIRECTORY ${_OUTPUT_PREFIX})

        list(APPEND ${source_files} "${_OUTPUT_PREFIX}/${file_name}.pb.cc")
        list(APPEND ${header_files} "${_OUTPUT_PREFIX}/${file_name}.pb.h")
        list(APPEND ${source_files} "${_OUTPUT_PREFIX}/${file_name}.service.rpc.pb.cc")
        list(APPEND ${header_files} "${_OUTPUT_PREFIX}/${file_name}.service.rpc.pb.h")
        list(APPEND ${source_files} "${_OUTPUT_PREFIX}/${file_name}.stub.rpc.pb.cc")
        list(APPEND ${header_files} "${_OUTPUT_PREFIX}/${file_name}.stub.rpc.pb.h")
        list(APPEND ${header_files} "${_OUTPUT_PREFIX}/${file_name}.irpc.pb.h")
        list(APPEND ${source_files} "${_OUTPUT_PREFIX}/${file_name}.brpc.pb.cc")
        list(APPEND ${header_files} "${_OUTPUT_PREFIX}/${file_name}.brpc.pb.h")
        list(APPEND ${source_files} "${_OUTPUT_PREFIX}/${file_name}.brpc.stub.pb.cc")
        list(APPEND ${header_files} "${_OUTPUT_PREFIX}/${file_name}.brpc.stub.pb.h")

        ds_protoc_command(_ds_protoc)
        ds_proto_include_args(_ds_proto_include_args)
        if (NOT DS_ZMQ_PLUGIN_TARGET)
            set(DS_ZMQ_PLUGIN_TARGET zmq_plugin)
        endif()
        file(GLOB_RECURSE _ds_imports CONFIGURE_DEPENDS "${DS_SOURCE_DIR}/src/datasystem/protos/*.proto")
        add_custom_command(
            OUTPUT "${_OUTPUT_PREFIX}/${file_name}.pb.cc"
            "${_OUTPUT_PREFIX}/${file_name}.pb.h"
            "${_OUTPUT_PREFIX}/${file_name}.service.rpc.pb.cc"
            "${_OUTPUT_PREFIX}/${file_name}.service.rpc.pb.h"
            "${_OUTPUT_PREFIX}/${file_name}.stub.rpc.pb.cc"
            "${_OUTPUT_PREFIX}/${file_name}.stub.rpc.pb.h"
            "${_OUTPUT_PREFIX}/${file_name}.irpc.pb.h"
            "${_OUTPUT_PREFIX}/${file_name}.brpc.pb.cc"
            "${_OUTPUT_PREFIX}/${file_name}.brpc.pb.h"
            "${_OUTPUT_PREFIX}/${file_name}.brpc.stub.pb.cc"
            "${_OUTPUT_PREFIX}/${file_name}.brpc.stub.pb.h"
            COMMAND ${_ds_protoc}
            ARGS -I ${DS_SOURCE_DIR}/src -I ${proto_src_directory} ${_ds_proto_include_args}
                 --zmq_out ${target_directory}
                 --cpp_out ${target_directory}
                 --plugin=protoc-gen-zmq=$<TARGET_FILE:${DS_ZMQ_PLUGIN_TARGET}>
                 ${abs_file}
            DEPENDS ${abs_file} ${_ds_imports} ${DS_PROTOC_TARGET} ${DS_ZMQ_PLUGIN_TARGET}
            COMMENT "Running c++ ZMQ compiler on ${file}" VERBATIM)
        add_custom_target(ZMQ_PROTO_LIB_DEPEND_${file_name} DEPENDS
                "${_OUTPUT_PREFIX}/${file_name}.pb.cc"
                "${_OUTPUT_PREFIX}/${file_name}.pb.h"
                "${_OUTPUT_PREFIX}/${file_name}.service.rpc.pb.cc"
                "${_OUTPUT_PREFIX}/${file_name}.service.rpc.pb.h"
                "${_OUTPUT_PREFIX}/${file_name}.stub.rpc.pb.cc"
                "${_OUTPUT_PREFIX}/${file_name}.stub.rpc.pb.h"
                "${_OUTPUT_PREFIX}/${file_name}.irpc.pb.h"
                "${_OUTPUT_PREFIX}/${file_name}.brpc.pb.cc"
                "${_OUTPUT_PREFIX}/${file_name}.brpc.pb.h"
                "${_OUTPUT_PREFIX}/${file_name}.brpc.stub.pb.cc"
                "${_OUTPUT_PREFIX}/${file_name}.brpc.stub.pb.h")
        add_dependencies(${ZMQ_PROTO_LIB_DEPEND} ZMQ_PROTO_LIB_DEPEND_${file_name})
    endforeach ()

    set_source_files_properties(${${source_files}} ${${header_files}} PROPERTIES GENERATED TRUE)
    set(${source_files} ${${source_files}} PARENT_SCOPE)
    set(${header_files} ${${header_files}} PARENT_SCOPE)
endfunction()

# Generate gRPC protobuf cc files.
#
# SRCS is the output variable of the protobuf source files.
#
# HDRS is the output variable of the protobuf header files.
#
# TARGET_DIR is the generate cc files target directory.
#
# Additional optional arguments:
#
#   PROTO_FILES <file1> <file2> ...
#       gRPC protobuf source files to be compiled.
#
#   SOURCE_ROOT <dir>
#       gRPC protobuf source files root directory, default is ${CMAKE_CURRENT_SOURCE_DIR},
#       if protobuf source files are not in ${DS_SOURCE_DIR}, this variable must be set.
#
#   PROTO_DEPEND <target>
#       If the generated cc files need to depend some target this variable must be set.
function(GENERATE_GRPC_CPP SRCS HDRS TARGET_DIR)
    set(options)
    set(one_value_args SOURCE_ROOT PROTO_DEPEND)
    set(multi_value_args PROTO_FILES)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if (NOT ARG_PROTO_FILES)
        message(FATAL_ERROR "GENERATE_GRPC_CPP() called without any proto files")
    endif ()

    if (NOT ARG_SOURCE_ROOT)
        set(ARG_SOURCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    set(${SRCS})
    set(${HDRS})
    set(_PROTO_IMPORT_ARGS -I "${ARG_SOURCE_ROOT}")

    # Add protobuf import dir to avoid import report by protoc compiler.
    foreach (_PROTO_FILE ${ARG_PROTO_FILES})
        get_filename_component(_ABS_FILE ${_PROTO_FILE} ABSOLUTE)
        get_filename_component(_ABS_PATH ${_ABS_FILE} PATH)
        list(FIND _PROTO_IMPORT_ARGS ${_ABS_PATH} _IMPORT_EXIST)
        if (${_IMPORT_EXIST} EQUAL -1)
            list(APPEND _PROTO_IMPORT_ARGS -I ${_ABS_PATH})
        endif()
    endforeach()

    foreach (_PROTO_FILE ${ARG_PROTO_FILES})
        get_filename_component(_ABS_FILE   ${_PROTO_FILE} ABSOLUTE)
        get_filename_component(_ABS_DIR    ${_PROTO_FILE} DIRECTORY)
        get_filename_component(_PROTO_NAME ${_PROTO_FILE} NAME_WE)
        get_filename_component(_PROTO_DIR  ${_PROTO_FILE} PATH)
        file(RELATIVE_PATH _REL_DIR ${ARG_SOURCE_ROOT} ${_ABS_DIR})
        file(MAKE_DIRECTORY ${TARGET_DIR}/${_REL_DIR})
        list(APPEND ${SRCS} ${TARGET_DIR}/${_REL_DIR}/${_PROTO_NAME}.grpc.pb.cc)
        list(APPEND ${HDRS} ${TARGET_DIR}/${_REL_DIR}/${_PROTO_NAME}.grpc.pb.h)
        ds_protoc_command(_ds_protoc)
        ds_proto_include_args(_ds_proto_include_args)
        if (NOT DS_GRPC_PLUGIN_TARGET)
            set(DS_GRPC_PLUGIN_TARGET gRPC::grpc_cpp_plugin)
        endif()
        file(GLOB_RECURSE _ds_imports CONFIGURE_DEPENDS "${ARG_SOURCE_ROOT}/*.proto")
        add_custom_command(
            OUTPUT "${TARGET_DIR}/${_REL_DIR}/${_PROTO_NAME}.grpc.pb.cc" "${TARGET_DIR}/${_REL_DIR}/${_PROTO_NAME}.grpc.pb.h"
            COMMAND ${_ds_protoc}
            ARGS ${_PROTO_IMPORT_ARGS} ${_ds_proto_include_args}
                 -I ${_PROTO_DIR}
                 --grpc_out ${TARGET_DIR}
                 --plugin=protoc-gen-grpc=$<TARGET_FILE:${DS_GRPC_PLUGIN_TARGET}>
                 ${_ABS_FILE}
            DEPENDS ${_ABS_FILE} ${_ds_imports} ${DS_PROTOC_TARGET} ${DS_GRPC_PLUGIN_TARGET}
            COMMENT "Running c++ grpc protocol compiler on ${_PROTO_FILE}" VERBATIM)

        if (ARG_PROTO_DEPEND)
            add_custom_target(GRPC_LIB_DEPEND_${_PROTO_NAME} DEPENDS
                            "${TARGET_DIR}/${_PROTO_NAME}.grpc.pb.cc"
                            "${TARGET_DIR}/${_PROTO_NAME}.grpc.pb.h")
            add_dependencies(${ARG_PROTO_DEPEND} GRPC_LIB_DEPEND_${_PROTO_NAME})
        endif()
    endforeach ()

    set_source_files_properties(${${SRCS}} ${${HDRS}} PROPERTIES GENERATED TRUE)
    set(${SRCS} ${${SRCS}} PARENT_SCOPE)
    set(${HDRS} ${${HDRS}} PARENT_SCOPE)
endfunction()
