if (NOT DS_RPC_GENERATOR_TARGET)
    set(DS_RPC_GENERATOR_TARGET rpc_generator)
endif()
if (NOT DS_ZMQ_PLUGIN_TARGET)
    set(DS_ZMQ_PLUGIN_TARGET zmq_plugin)
endif()
if (NOT DS_BUILD_CLIENT_ONLY OR NOT CMAKE_CROSSCOMPILING)
# protoc plugin
set(ZMQ_PLUGIN_DEPEND_LIBS
        protobuf::libprotobuf
        protobuf::libprotoc
        common_util
        utils_protos
        rpc_option_protos
        )

set(ZMQ_RPC_GENERATOR_SRC
        plugin_generator/rpc_generator.cpp
        plugin_generator/service_cpp_generator.cpp
        plugin_generator/service_header_generator.cpp
        plugin_generator/stub_cpp_generator.cpp
        plugin_generator/stub_header_generator.cpp
        plugin_generator/brpc_service_generator.cpp
        plugin_generator/brpc_stub_generator.cpp
)

if (DS_BUILD_CLIENT_ONLY)
    add_library(${DS_RPC_GENERATOR_TARGET} STATIC ${ZMQ_RPC_GENERATOR_SRC})
else()
    add_library(${DS_RPC_GENERATOR_TARGET} ${ZMQ_RPC_GENERATOR_SRC})
endif()
target_link_libraries(${DS_RPC_GENERATOR_TARGET} PRIVATE ${ZMQ_PLUGIN_DEPEND_LIBS})
add_dependencies(${DS_RPC_GENERATOR_TARGET} utils_protos rpc_option_protos)

add_executable(${DS_ZMQ_PLUGIN_TARGET} plugin_generator/zmq_plugin.cpp)
target_link_libraries(${DS_ZMQ_PLUGIN_TARGET} PRIVATE ${DS_RPC_GENERATOR_TARGET})

if (NOT DS_BUILD_CLIENT_ONLY)
    add_executable(rpc_plugin plugin_generator/rpc_plugin.cpp)
    target_link_libraries(rpc_plugin PRIVATE ${DS_RPC_GENERATOR_TARGET})
endif()

endif()

# zmq server library
set(COMMON_RPC_ZMQ_SRCS
        api_deadline.cpp
        api_deadline_helpers.cpp
        brpc_factory.cpp
        fanout_collector.cpp
        network_latency_estimator.cpp
        rpc_auth_key_manager.cpp
        rpc_channel.cpp
        rpc_options.cpp
        rpc_server.cpp
        rpc_service_cfg.cpp
        rpc_auth_keys.cpp
        mem_view.cpp
        unix_sock_fd.cpp
        brpc_stream_close_helper.cpp

















        rpc_auth_key_manager_server.cpp)

set(COMMON_RPC_ZMQ_DEPENDS_LIBS
        protobuf::libprotobuf
        nlohmann_json::nlohmann_json
        ${SECUREC_LIBRARY}
        ${BRPC_LIBRARIES}
        common_log
        common_perf
        common_metrics
        common_encrypt
        utils_protos
        common_util
        common_event_loop
        rpc_option_protos)

set(COMMON_RPC_ZMQ_CLIENT_DEPENDS_LIBS
        protobuf::libprotobuf
        nlohmann_json::nlohmann_json
        ${SECUREC_LIBRARY}
        ${BRPC_LIBRARIES}
        common_log
        common_perf
        common_util
        utils_protos_client
        rpc_option_protos
        common_event_loop
        common_encrypt)

if (BUILD_WITH_URMA)
        list(APPEND COMMON_RPC_ZMQ_CLIENT_DEPENDS_LIBS
                common_rdma)
endif()

if (NOT DS_BUILD_CLIENT_ONLY)
add_library(common_rpc_zmq STATIC ${COMMON_RPC_ZMQ_SRCS})
target_link_libraries(common_rpc_zmq PUBLIC ${COMMON_RPC_ZMQ_DEPENDS_LIBS})
add_dependencies(common_rpc_zmq rpc_option_protos)

endif()

add_library(common_rpc_zmq_client STATIC ${COMMON_RPC_ZMQ_SRCS})
target_link_libraries(common_rpc_zmq_client PUBLIC ${COMMON_RPC_ZMQ_CLIENT_DEPENDS_LIBS})
add_dependencies(common_rpc_zmq_client rpc_option_protos)
if (DS_BUILD_CLIENT_ONLY)
    add_library(common_rpc_zmq ALIAS common_rpc_zmq_client)
endif()

set(RPC_STUB_CACHE_MGR_SRCS
        rpc_stub_cache_mgr.cpp
)

set(RPC_STUB_CACHE_MGR_DEPENDS_LIBS
        coordinator_protos
        master_object_protos
        worker_object_protos
        master_stream_protos
        worker_stream_protos
)

add_library(rpc_stub_cache_mgr STATIC ${RPC_STUB_CACHE_MGR_SRCS})
target_link_libraries(rpc_stub_cache_mgr PUBLIC ${RPC_STUB_CACHE_MGR_DEPENDS_LIBS})
