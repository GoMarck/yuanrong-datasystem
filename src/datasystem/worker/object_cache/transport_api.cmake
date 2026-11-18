# Lightweight library for transport API
set(WORKER_TRANSPORT_API_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/worker_worker_transport_api.cpp")

set(WORKER_TRANSPORT_API_DEPEND_LIBS
        common_log
        common_metrics
        common_util
        common_request_context
        common_inject
        common_rpc_zmq
        common_rdma
        rpc_stub_cache_mgr
        worker_object_protos)

add_library(worker_transport_api STATIC ${WORKER_TRANSPORT_API_SRCS})
target_link_libraries(worker_transport_api PRIVATE ${WORKER_TRANSPORT_API_DEPEND_LIBS})
add_dependencies(worker_transport_api worker_object_protos)
