/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef KVTEST_BENCHMARK_PROFILE_H
#define KVTEST_BENCHMARK_PROFILE_H

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/resource.h>
#include <utility>

namespace kvtest_profile {
enum class Stage : size_t {
    DS_SET, DS_GET, DS_RPC_CREATE, DS_RPC_PUBLISH, DS_RPC_QUERY, DS_RPC_GET,
    DS_SERVER_CREATE, DS_SERVER_PUBLISH, DS_SERVER_QUERY, DS_SERVER_GET,
    DS_RPC_OVERHEAD_CREATE, DS_RPC_OVERHEAD_PUBLISH, DS_RPC_OVERHEAD_QUERY, DS_RPC_OVERHEAD_GET,
    DS_RPC_RELEASE, DS_SERVER_RELEASE, DS_RPC_OVERHEAD_RELEASE,
    DS_RPC_CREATE_META, DS_SERVER_CREATE_META, DS_RPC_OVERHEAD_CREATE_META,
    DS_RPC_QUERY_META, DS_SERVER_QUERY_META, DS_RPC_OVERHEAD_QUERY_META,
    DS_SHM_WRITE_MAP, DS_SHM_READ_MAP, DS_SHM_LOOKUP, DS_SHM_FETCH_MAP, DS_COPY, DS_UB_ALLOC,
    DS_UB_WRITE, DS_UB_SUBMIT, DS_UB_WAIT,
    MC_PUT, MC_GET, MC_RPC_PUT_START, MC_RPC_PUT_END, MC_RPC_QUERY,
    MC_SERVER_PUT_START, MC_SERVER_PUT_END, MC_SERVER_QUERY,
    MC_PUT_ALLOC, MC_GET_ALLOC, MC_COPY, MC_TE_READ, MC_TE_WRITE, MC_TE_SUBMIT, MC_TE_WAIT, MC_LOCAL_COPY,
    DS_SET_CONTEXT,
    DS_SET_IMPL,
    DS_SET_ACCESS,
    DS_GET_CONTEXT,
    DS_GET_IMPL,
    DS_GET_ACCESS,
    DS_GET_RETURN,
    DS_SET_ROUTE,
    DS_SET_CREATE,
    DS_SET_PUBLISH,
    DS_SET_CLEANUP,
    DS_GET_ROUTE,
    DS_GET_TRANSPORT,
    DS_GET_MATERIALIZE,
    DS_QUERY_PREPARE,
    DS_QUERY_ATTEMPT_PREPARE,
    DS_QUERY_ATTEMPT,
    DS_QUERY_RETRY,
    DS_QUERY_APPLY,
    DS_GET_UB_PREPARE,
    DS_CREATE_RPC_PREPARE,
    DS_CREATE_RPC_SIGN,
    DS_CREATE_RPC_CALL,
    DS_CREATE_RPC_RESULT,
    DS_PUBLISH_RPC_PREPARE,
    DS_PUBLISH_RPC_SIGN,
    DS_PUBLISH_RPC_CALL,
    DS_PUBLISH_RPC_RESULT,
    DS_QUERY_RPC_PREPARE,
    DS_QUERY_RPC_SIGN,
    DS_QUERY_RPC_CALL,
    DS_QUERY_RPC_RESULT,
    DS_W_CREATE_AUTH,
    DS_W_CREATE_LOOKUP,
    DS_W_CREATE_RESERVE,
    DS_W_CREATE_ALLOC,
    DS_W_CREATE_RESPONSE,
    DS_W_CREATE_REF,
    DS_W_PUBLISH_AUTH,
    DS_W_PUBLISH_VALIDATE,
    DS_W_PUBLISH_LOCK,
    DS_W_PUBLISH_PREPARE,
    DS_W_PUBLISH_META,
    DS_W_PUBLISH_COMMIT,
    DS_W_PUBLISH_CLEANUP,
    DS_W_QUERY_AUTH,
    DS_W_QUERY_LOCAL,
    DS_W_QUERY_META,
    DS_W_QUERY_DELIVERY,
    DS_W_QUERY_ACQUIRE,
    DS_W_QUERY_UB_LATCH,
    DS_W_QUERY_UB_PREPARE,
    DS_W_QUERY_UB_WRITE,
    DS_W_QUERY_UB_RESULT,
    DS_W_QUERY_ENCODE_SHM,
    DS_UB_POLL_SLEEP,
    DS_UB_NOTIFY,
    DS_UB_WAIT_EVENT,
    DS_UB_WAIT_RESULT,
    MC_PUT_PREPARE,
    MC_PUT_CLIENT_PUT,
    MC_GET_PREPARE,
    MC_GET_CLIENT_GET,
    MC_GET_QUERY_CALL,
    MC_GET_RESULT,
    MC_PUT_RESULT,
    DS_SET_ADMISSION,
    DS_SET_EXECUTE,
    DS_SET_POSTPROCESS,
    DS_GET_INPUT,
    DS_GET_EXECUTE,
    DS_GET_RESULT,
    DS_UB_WAKE_SCHED,
    DS_UB_COMPLETION_OBSERVE,
    DS_UB_EVENT_PROCESS,
    DS_W_PUBLISH_POSTPROCESS,
    D_DS_DETAIL_SHARED_CONNECTION_GET_TRANSPORTER,
    D_DS_DETAIL_SHARED_CONNECTION_GET_TOTAL,
    D_DS_DETAIL_SHARED_CONNECTION_GET_ENTRY,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_TRANSPORTER,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_ALLOCATE,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_LOCK_ACQUIRE_WITH_GUARD_ALLOCATION,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_VALIDATE,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_RESULT,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_TOTAL,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_ENTRY,
    D_DS_DETAIL_SHARED_CONNECTION_ENTRY_TOTAL,
    D_DS_DETAIL_SHARED_CONNECTION_ENTRY_ADMISSION,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_READ_LOCK_ACQUIRE,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_VALIDATE,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_TOTAL,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_PREPARE,
    D_DS_DETAIL_GET_UB_ADMISSION_ENTRY_LOOKUP,
    D_DS_DETAIL_GET_UB_ADMISSION_MISS_HANDLING,
    D_DS_DETAIL_GET_UB_ADMISSION_LOCK_ACQUIRE,
    D_DS_DETAIL_GET_UB_ADMISSION_VALIDATE,
    D_DS_DETAIL_GET_UB_ADMISSION_TOTAL,
    D_DS_DETAIL_GET_UB_ADMISSION_PREPARE,
    D_DS_DETAIL_GET_UB_PREPARE_ADMISSION,
    D_DS_DETAIL_GET_UB_PREPARE_CONNECTION,
    D_DS_DETAIL_GET_UB_PREPARE_BUFFERS,
    D_DS_DETAIL_GET_UB_PREPARE_RESULT,
    D_DS_DETAIL_GET_UB_PREPARE_TOTAL,
    D_DS_DETAIL_GET_UB_PREPARE_VALIDATE,
    D_DS_DETAIL_GET_UB_BUFFERS_ITEMS,
    D_DS_DETAIL_GET_UB_BUFFERS_TOTAL,
    D_DS_DETAIL_GET_UB_BUFFERS_RESERVE,
    D_DS_DETAIL_GET_SHM_PREPARE_CONNECTION,
    D_DS_DETAIL_GET_SHM_PREPARE_CAST,
    D_DS_DETAIL_GET_SHM_PREPARE_SESSION,
    D_DS_DETAIL_GET_SHM_PREPARE_RESULT,
    D_DS_DETAIL_GET_SHM_PREPARE_TOTAL,
    D_DS_DETAIL_GET_SHM_PREPARE_VALIDATE,
    D_DS_DETAIL_GET_SHM_RESULT_TOTAL,
    D_DS_DETAIL_GET_SHM_RESULT_PROCESS,
    D_DS_DETAIL_GET_UB_RESULT_TOTAL,
    D_DS_DETAIL_GET_UB_RESULT_PROCESS,
    D_DS_DETAIL_GET_TCP_RESULT_TOTAL,
    D_DS_DETAIL_GET_TCP_RESULT_PROCESS,
    D_DS_DETAIL_GET_QUERY_INITIALIZE,
    D_DS_DETAIL_GET_QUERY_RPC_WITH_RETRY,
    D_DS_DETAIL_GET_QUERY_APPLY_AND_CLEANUP,
    D_DS_DETAIL_GET_QUERY_TOTAL,
    D_DS_DETAIL_GET_QUERY_VALIDATE,
    D_DS_DETAIL_GET_REQUEST_TOTAL,
    D_DS_DETAIL_GET_REQUEST_BUILD,
    D_DS_DETAIL_GET_RETRY_TOTAL,
    D_DS_DETAIL_GET_RETRY_PREPARE,
    D_DS_DETAIL_SET_SDK_CREATE,
    D_DS_DETAIL_SET_SDK_COPY,
    D_DS_DETAIL_SET_SDK_PUBLISH_PREPARE,
    D_DS_DETAIL_SET_SDK_PUBLISH,
    D_DS_DETAIL_SET_SDK_FEEDBACK_AND_CLEANUP,
    D_DS_DETAIL_SET_SDK_TOTAL,
    D_DS_DETAIL_SET_SDK_CONTEXT,
    D_DS_DETAIL_SET_UB_CREATE_TOTAL,
    D_DS_DETAIL_SET_UB_CREATE_PREPARE,
    D_DS_DETAIL_SET_UB_PUBLISH_TOTAL,
    D_DS_DETAIL_SET_UB_PUBLISH_PREPARE,
    D_DS_DETAIL_SET_UB_PUBLISH_RPC_TOTAL,
    D_DS_DETAIL_SET_UB_PUBLISH_RPC_CALL,
    D_DS_DETAIL_SET_WORKER_CREATE_OBJECT_LOOKUP,
    D_DS_DETAIL_SET_WORKER_CREATE_EXISTING_OBJECT_LOCK_AND_CHECK,
    D_DS_DETAIL_SET_WORKER_CREATE_RESERVE,
    D_DS_DETAIL_SET_WORKER_CREATE_SHM_UNIT_ALLOCATE,
    D_DS_DETAIL_SET_WORKER_CREATE_MEMORY_ALLOCATE,
    D_DS_DETAIL_SET_WORKER_CREATE_RESPONSE,
    D_DS_DETAIL_SET_WORKER_CREATE_CLIENT_LOOKUP,
    D_DS_DETAIL_SET_WORKER_CREATE_REFERENCE,
    D_DS_DETAIL_SET_WORKER_CREATE_CLEANUP,
    D_DS_DETAIL_SET_WORKER_CREATE_TOTAL,
    D_DS_DETAIL_SET_WORKER_CREATE_NAMESPACE,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_CLIENT_LOOKUP,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_OBJECT_REF,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_SHM_LOOKUP_INSERT,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_OWNER_INSERT,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_PAGE_METRICS,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_EXPIRY,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_METRICS_CLEANUP,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_TOTAL,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_PREPARE,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_CLIENT_LOOKUP,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_SHM_LOOKUP,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_UPDATE,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_EXPIRY,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_METRICS_CLEANUP,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_TOTAL,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_PREPARE,
    D_DS_DETAIL_RELEASE_WORKER_UPDATE_PAGE_METRICS,
    D_DS_DETAIL_RELEASE_WORKER_UPDATE_OWNER_ERASE,
    D_DS_DETAIL_RELEASE_WORKER_UPDATE_TOTAL,
    D_DS_DETAIL_RELEASE_WORKER_UPDATE_OBJECT_REF,
    D_DS_DETAIL_SET_MASTER_REQUEST_ROUTE,
    D_DS_DETAIL_SET_MASTER_REQUEST_CREATE,
    D_DS_DETAIL_SET_MASTER_REQUEST_RESULT,
    D_DS_DETAIL_SET_MASTER_REQUEST_TOTAL,
    D_DS_DETAIL_SET_MASTER_REQUEST_PREPARE,
    D_DS_DETAIL_SET_MASTER_CREATE_DISPATCH_AND_CLEANUP,
    D_DS_DETAIL_SET_MASTER_CREATE_TOTAL,
    D_DS_DETAIL_SET_MASTER_CREATE_HEAVY_OPERATION,
    D_DS_DETAIL_SET_MASTER_BINARY_SHARD_LOCK_ACQUIRE,
    D_DS_DETAIL_SET_MASTER_BINARY_TABLE_INSERT,
    D_DS_DETAIL_SET_MASTER_BINARY_EXISTENCE_CHECK,
    D_DS_DETAIL_SET_MASTER_BINARY_EXISTING_OBJECT,
    D_DS_DETAIL_SET_MASTER_BINARY_NEW_OBJECT,
    D_DS_DETAIL_SET_MASTER_BINARY_SUBSCRIPTION,
    D_DS_DETAIL_SET_MASTER_BINARY_NESTED_REFERENCES,
    D_DS_DETAIL_SET_MASTER_BINARY_EXPIRY,
    D_DS_DETAIL_SET_MASTER_BINARY_TOTAL,
    D_DS_DETAIL_SET_MASTER_BINARY_PREPARE,
    D_DS_DETAIL_SET_MASTER_PERSIST_FILL_METADATA,
    D_DS_DETAIL_SET_MASTER_PERSIST_COPY_METADATA,
    D_DS_DETAIL_SET_MASTER_PERSIST_SERIALIZE,
    D_DS_DETAIL_SET_MASTER_PERSIST_STORE,
    D_DS_DETAIL_SET_MASTER_PERSIST_ACCESSOR_RELEASE,
    D_DS_DETAIL_SET_MASTER_PERSIST_TOTAL,
    D_DS_DETAIL_SET_MASTER_PERSIST_PREPARE,
    D_DS_DETAIL_SET_MASTER_FIRST_PERSIST,
    D_DS_DETAIL_SET_MASTER_FIRST_TOTAL,
    D_DS_DETAIL_SET_MASTER_FIRST_EXPIRY,
    D_DS_DETAIL_SET_UB_CREATE_RPC,
    D_DS_DETAIL_SET_UB_CREATE_INFO_ALLOCATE,
    D_DS_DETAIL_SET_UB_CREATE_BUFFER_ALLOCATE,
    D_DS_DETAIL_SET_UB_CREATE_INFO_FILL,
    D_DS_DETAIL_SET_UB_CREATE_OWNER_ALLOCATE,
    D_DS_DETAIL_SET_UB_CREATE_BUFFER_CONSTRUCT,
    D_DS_DETAIL_SET_SHM_MAP_UNIT_ALLOCATE_FILL,
    D_DS_DETAIL_SET_SHM_MAP_LOOKUP_MMAP,
    D_DS_DETAIL_SET_SHM_MAP_FD_LOOKUP,
    D_DS_DETAIL_SET_SHM_MAP_REFERENCE,
    D_DS_DETAIL_SET_SHM_MAP_INFO_FILL,
    D_DS_DETAIL_SET_SHM_MAP_OWNER_ALLOCATE,
    D_DS_DETAIL_SET_SHM_MAP_TOTAL,
    D_DS_DETAIL_SET_SHM_MAP_VALIDATE,
    D_DS_DETAIL_GET_SHM_MAP_TOTAL,
    D_DS_DETAIL_GET_SHM_MAP_VALIDATE,
    D_DS_DETAIL_GET_SHM_OWNER_TOTAL,
    D_DS_DETAIL_GET_SHM_OWNER_CONSTRUCT,
    D_DS_DETAIL_SET_CREATE_TRANSPORT_HINT,
    D_DS_DETAIL_SET_CREATE_ALLOCATION_ID,
    D_DS_DETAIL_SET_CREATE_ATTEMPT,
    D_DS_DETAIL_SET_CREATE_RETRY_AND_CLEANUP,
    D_DS_DETAIL_SET_CREATE_TOTAL,
    D_DS_DETAIL_SET_CREATE_VALIDATE,
    D_DS_DETAIL_SET_CREATE_ATTEMPT_TOTAL,
    D_DS_DETAIL_SET_CREATE_ATTEMPT_EXECUTE,
    D_DS_DETAIL_SET_ROUTE_SELECT,
    D_DS_DETAIL_SET_ROUTE_TOTAL,
    D_DS_DETAIL_SET_ROUTE_EXCLUSIONS,
    D_DS_DETAIL_SET_ROUTE_CONTEXT_TOTAL,
    D_DS_DETAIL_SET_ROUTE_CONTEXT_BUILD,
    D_DS_DETAIL_GET_WORKER_DELIVERY_EXECUTE,
    D_DS_DETAIL_GET_WORKER_DELIVERY_TOTAL,
    D_DS_DETAIL_GET_WORKER_DELIVERY_PREPARE,
    D_DS_DETAIL_GET_UB_WORKER_GUARD_CONSTRUCT,
    D_DS_DETAIL_GET_UB_WORKER_READ_LATCH_ACQUIRE,
    D_DS_DETAIL_GET_UB_WORKER_SEGMENT_PREPARE,
    D_DS_DETAIL_GET_UB_WORKER_BANDWIDTH_BEGIN,
    D_DS_DETAIL_GET_UB_WORKER_WRITE,
    D_DS_DETAIL_GET_UB_WORKER_RESULT_AND_CLEANUP,
    D_DS_DETAIL_GET_UB_WORKER_TOTAL,
    D_DS_DETAIL_GET_UB_WORKER_VALIDATE,
    D_DS_DETAIL_GET_SHM_WORKER_REFERENCE,
    D_DS_DETAIL_GET_SHM_WORKER_LOG,
    D_DS_DETAIL_GET_SHM_WORKER_ENCODE,
    D_DS_DETAIL_GET_SHM_WORKER_TOTAL,
    D_DS_DETAIL_GET_SHM_WORKER_PREPARE,
    D_DS_DETAIL_SET_WORKER_MASTER_TOTAL,
    D_DS_DETAIL_SET_WORKER_MASTER_PREPARE,
    D_DS_DETAIL_SET_WORKER_CREATE_META_TOTAL,
    D_DS_DETAIL_SET_WORKER_CREATE_META_PREPARE,
    D_DS_DETAIL_SHARED_CONNECTION_ENTRY_LOOKUP,
    D_DS_DETAIL_SHARED_CONNECTION_ENTRY_INSERT_ALLOCATE,
    D_DS_DETAIL_GET_WORKER_DELIVERY_HEALTH_SUMMARY,
    D_DS_DETAIL_GET_WORKER_DELIVERY_RESPONSE_WRITE,
    D_DS_DETAIL_GET_WORKER_DELIVERY_PAYLOAD,
    D_DS_DETAIL_GET_WORKER_DELIVERY_COMPLETE,
    D_DS_DETAIL_GET_WORKER_DELIVERY_CLEANUP,
    D_DS_DETAIL_SHARED_REFERENCE_OBJECT_KEYS_PREPARE,
    D_DS_DETAIL_SHARED_REFERENCE_OBJECT_KEYS_EMPLACE,
    D_DS_DETAIL_SHARED_REFERENCE_OBJECT_KEYS_UPDATE,
    D_DS_DETAIL_SHARED_REFERENCE_OBJECT_KEYS_TOTAL,
    D_DS_DETAIL_SHARED_REFERENCE_OBJECT_KEYS_LOCK_ACQUIRE,
    D_DS_DETAIL_SHARED_MASTER_HEAVY_OP_INSERT,
    D_DS_DETAIL_SHARED_MASTER_HEAVY_OP_TOTAL,
    D_DS_DETAIL_SHARED_MASTER_HEAVY_OP_LOCK_ACQUIRE,
    D_DS_DETAIL_SET_RELEASE_POOL,
    D_DS_DETAIL_SET_RELEASE_ENQUEUE_AND_CLEANUP,
    D_DS_DETAIL_SET_RELEASE_TOTAL,
    D_DS_DETAIL_SET_RELEASE_VALIDATE,
    D_DS_DETAIL_SET_RELEASE_QUEUE_WAIT,
    D_DS_DETAIL_SET_RELEASE_TASK_TOTAL,
    D_DS_DETAIL_SET_RELEASE_TASK_PREPARE,
    D_DS_DETAIL_SET_RELEASE_TASK_RPC,
    D_DS_DETAIL_GET_SHM_RELEASE_POOL,
    D_DS_DETAIL_GET_SHM_RELEASE_ENQUEUE_AND_CLEANUP,
    D_DS_DETAIL_GET_SHM_RELEASE_TOTAL,
    D_DS_DETAIL_GET_SHM_RELEASE_VALIDATE,
    D_DS_DETAIL_GET_SHM_RELEASE_QUEUE_WAIT,
    D_DS_DETAIL_GET_SHM_RELEASE_TASK_TOTAL,
    D_DS_DETAIL_GET_SHM_RELEASE_TASK_PREPARE,
    D_DS_DETAIL_GET_SHM_RELEASE_TASK_RPC,
    D_MC_DETAIL_GET_MASTER_IDENTITY,
    D_MC_DETAIL_GET_MASTER_ACCESSOR,
    D_MC_DETAIL_GET_MASTER_VALIDATE,
    D_MC_DETAIL_GET_MASTER_DESCRIPTORS,
    D_MC_DETAIL_GET_MASTER_LEASE,
    D_MC_DETAIL_GET_MASTER_OPTIONAL_POLICIES,
    D_MC_DETAIL_GET_MASTER_TOTAL,
    D_MC_DETAIL_GET_MASTER_SNAPSHOT_LOCK_ACQUIRE,
    D_MC_DETAIL_SET_MASTER_START_OBJECT_LOCK_ACQUIRE,
    D_MC_DETAIL_SET_MASTER_START_PREPARE,
    D_MC_DETAIL_SET_MASTER_START_CLIENT_LOCK_ACQUIRE,
    D_MC_DETAIL_SET_MASTER_START_SNAPSHOT_LOCK_ACQUIRE,
    D_MC_DETAIL_SET_MASTER_START_CLIENTS_COPY,
    D_MC_DETAIL_SET_MASTER_START_SHARD_ROUTE,
    D_MC_DETAIL_SET_MASTER_START_SHARD_LOCK_ACQUIRE,
    D_MC_DETAIL_SET_MASTER_START_TENANT,
    D_MC_DETAIL_SET_MASTER_START_QUOTA,
    D_MC_DETAIL_SET_MASTER_START_METADATA_LOOKUP,
    D_MC_DETAIL_SET_MASTER_START_EXISTING_OR_ALLOCATE,
    D_MC_DETAIL_SET_MASTER_START_TOTAL,
    D_MC_DETAIL_SET_MASTER_START_VALIDATE,
    D_MC_DETAIL_SET_MASTER_END_SNAPSHOT_LOCK_ACQUIRE,
    D_MC_DETAIL_SET_MASTER_END_IDENTITY,
    D_MC_DETAIL_SET_MASTER_END_ACCESSOR,
    D_MC_DETAIL_SET_MASTER_END_VALIDATE,
    D_MC_DETAIL_SET_MASTER_END_REPLICA_UPDATE,
    D_MC_DETAIL_SET_MASTER_END_QUOTA_AND_OPTIONAL_TASKS,
    D_MC_DETAIL_SET_MASTER_END_PROCESSING_ERASE,
    D_MC_DETAIL_SET_MASTER_END_ACCOUNTING,
    D_MC_DETAIL_SET_MASTER_END_LEASE,
    D_MC_DETAIL_SET_MASTER_END_EVENT,
    D_MC_DETAIL_SET_MASTER_END_OPLOG_AND_CLEANUP,
    D_MC_DETAIL_SET_MASTER_END_TOTAL,
    D_MC_DETAIL_SET_MASTER_END_PREPARE,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_QUOTA,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_PREPARE,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_SEGMENT_LOCK_ACQUIRE,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_PLACEMENT,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_MEMORY,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_DESCRIPTORS,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_SOFT_PIN,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_GROUP_REGISTER,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_PROCESSING_INSERT,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_TOTAL,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_TENANT_AND_CHECKS,
    D_MC_DETAIL_SHARED_MASTER_ROUTE_GROUP_LOOKUP,
    D_MC_DETAIL_SHARED_MASTER_ROUTE_TOTAL,
    D_MC_DETAIL_SHARED_MASTER_ROUTE_LOCK_ACQUIRE,
    D_MC_DETAIL_SHARED_BUFFER_ALLOCATE,
    D_MC_DETAIL_SHARED_BUFFER_HANDLE_CONSTRUCT,
    D_MC_DETAIL_SHARED_BUFFER_TOTAL,
    D_MC_DETAIL_SHARED_BUFFER_VALIDATE,
    D_MC_DETAIL_SHARED_TE_PREPARE_OPEN_SEGMENT,
    D_MC_DETAIL_SHARED_TE_PREPARE_REQUESTS,
    D_MC_DETAIL_SHARED_TE_PREPARE_SUBMIT,
    D_MC_DETAIL_SHARED_TE_PREPARE_TOTAL,
    D_MC_DETAIL_SHARED_TE_PREPARE_VALIDATE,
    D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_READ_LOCK_ACQUIRE,
    D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_LOOKUP,
    D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_FETCH,
    D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_WRITE_LOCK_ACQUIRE,
    D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_TOTAL,
    D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_PREPARE,
    D_MC_DETAIL_SHARED_TE_SEGMENT_ID_READ_LOCK_ACQUIRE,
    D_MC_DETAIL_SHARED_TE_SEGMENT_ID_LOOKUP,
    D_MC_DETAIL_SHARED_TE_SEGMENT_ID_FETCH,
    D_MC_DETAIL_SHARED_TE_SEGMENT_ID_WRITE_LOCK_ACQUIRE,
    D_MC_DETAIL_SHARED_TE_SEGMENT_ID_TOTAL,
    D_MC_DETAIL_SHARED_TE_SEGMENT_ID_PREPARE,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_FIFO_LOOKUP,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_FIFO_RESULT,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_FIFO_TOTAL,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_FIFO_LOCK_ACQUIRE,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_SIEVE_LOOKUP,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_SIEVE_RESULT,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_SIEVE_TOTAL,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_SIEVE_LOCK_ACQUIRE,
    D_DS_DETAIL_GET_WORKER_OBJECT_READ_LOCK_ACQUIRE,
    D_DS_DETAIL_GET_WORKER_OBJECT_VALIDATE,
    D_DS_DETAIL_GET_WORKER_OBJECT_PARAMS_ALLOCATE,
    D_DS_DETAIL_GET_WORKER_OBJECT_TOTAL,
    D_DS_DETAIL_GET_WORKER_OBJECT_LOOKUP,
    D_DS_DETAIL_SET_WORKER_CREATE_META_REQUEST,
    D_DS_DETAIL_SET_WORKER_CREATE_META_ROUTE_AND_CALL,
    D_DS_DETAIL_SET_WORKER_CREATE_META_RESULT,
    D_DS_DETAIL_SET_MASTER_API_LOCAL_TOTAL,
    D_DS_DETAIL_SET_MASTER_API_LOCAL_PREPARE,
    D_DS_DETAIL_SET_MASTER_API_REMOTE_TOTAL,
    D_DS_DETAIL_SET_MASTER_API_REMOTE_PREPARE,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_TENANT,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_METADATA,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_PROCESSING,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_REPLICATION,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_TENANT,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_METADATA,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_PROCESSING,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_REPLICATION,
    D_MC_DETAIL_SHARED_MASTER_SHARD_RW_TOTAL,
    D_MC_DETAIL_SHARED_MASTER_SHARD_RW_LOCK_ACQUIRE,
    D_MC_DETAIL_SHARED_MASTER_SHARD_RW_HELD,
    D_MC_DETAIL_SHARED_MASTER_SHARD_RO_TOTAL,
    D_MC_DETAIL_SHARED_MASTER_SHARD_RO_LOCK_ACQUIRE,
    D_MC_DETAIL_SHARED_MASTER_SHARD_RO_HELD,
    D_MC_DETAIL_SHARED_MASTER_LEASE_UPDATE,
    D_MC_DETAIL_SHARED_MASTER_LEASE_TOTAL,
    D_MC_DETAIL_SHARED_MASTER_LEASE_LOCK_ACQUIRE,
    D_DS_DETAIL_SET_ROUTE_CHOOSE_WORKER,
    D_DS_DETAIL_SET_ROUTE_CONTEXT,
    D_DS_DETAIL_SET_ROUTE_CONTEXT_LOCK_ACQUIRE,
    D_DS_DETAIL_SET_ROUTE_CONTEXT_IDENTITY,
    D_DS_DETAIL_SET_ROUTE_CONTEXT_GUARD_ALLOCATE,
    D_DS_DETAIL_SET_ROUTE_CONTEXT_CONNECTION_CHECK,
    D_DS_DETAIL_SET_CREATE_ATTEMPT_CONNECTION,
    D_DS_DETAIL_SET_CREATE_ATTEMPT_ADMISSION,
    D_DS_DETAIL_SET_CREATE_ATTEMPT_TRANSPORT,
    D_DS_DETAIL_SET_CREATE_ATTEMPT_RESULT,
    D_DS_DETAIL_SET_MASTER_API_LOCAL_SIGN,
    D_DS_DETAIL_SET_MASTER_API_LOCAL_CALL_WITH_RETRY,
    D_DS_DETAIL_GET_RPC_WRAPPER_SIGN,
    D_DS_DETAIL_GET_RPC_WRAPPER_OPTIONS,
    D_DS_DETAIL_GET_RPC_WRAPPER_CALL,
    D_DS_DETAIL_GET_RPC_WRAPPER_HEALTH_AND_RESULT,
    D_DS_DETAIL_GET_RPC_WRAPPER_TOTAL,
    D_DS_DETAIL_GET_RPC_WRAPPER_VALIDATE,
    D_DS_DETAIL_SHARED_CONNECTION_ENTRY_HIT_COUNT,
    D_DS_DETAIL_SHARED_CONNECTION_ENTRY_MISS_COUNT,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_CLIENT_MISS_COUNT,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_SHM_MISS_COUNT,
    D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_FETCH_COUNT,
    D_DS_DETAIL_SET_SDK_UB_BUFFER_TOTAL,
    D_DS_DETAIL_SET_SDK_SHM_BUFFER_TOTAL,
    D_DS_DETAIL_SET_SDK_TCP_BUFFER_TOTAL,
    D_DS_DETAIL_GET_QUERY_SHM_REQUEST_TOTAL,
    D_DS_DETAIL_GET_QUERY_UB_REQUEST_TOTAL,
    D_DS_DETAIL_GET_QUERY_TCP_REQUEST_TOTAL,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_METADATA_INSERT,
    D_DS_DETAIL_GET_UB_BUFFERS_ALLOCATE,
    D_DS_DETAIL_GET_UB_BUFFERS_VALIDATE,
    D_DS_DETAIL_GET_UB_BUFFERS_EMPLACE,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_HELD,
    D_MC_DETAIL_SHARED_RPC_METRICS_PREPARE,
    D_MC_DETAIL_SHARED_RPC_DISPATCH_AND_WAIT,
    D_MC_DETAIL_SHARED_RPC_COROUTINE_TOTAL,
    D_MC_DETAIL_SHARED_RPC_COROUTINE_POOL_DISPATCH,
    D_MC_DETAIL_SHARED_RPC_COROUTINE_RESPONSE_WAIT,
    D_MC_DETAIL_SHARED_RPC_COROUTINE_RESULT,
    D_MC_DETAIL_SHARED_RPC_TOTAL,
    D_MC_DETAIL_SHARED_RPC_POOL,
    D_DS_RPC_STAGE_CREATE_CLIENT_PREPARE,
    D_DS_RPC_STAGE_CREATE_CLIENT_RESULT,
    D_DS_RPC_STAGE_CREATE_SERVER_REQUEST_QUEUE,
    D_DS_RPC_STAGE_CREATE_SERVER_RESPONSE_QUEUE,
    D_DS_RPC_STAGE_CREATE_COMMUNICATION_RESIDUAL,
    D_DS_RPC_STAGE_PUBLISH_CLIENT_PREPARE,
    D_DS_RPC_STAGE_PUBLISH_CLIENT_RESULT,
    D_DS_RPC_STAGE_PUBLISH_SERVER_REQUEST_QUEUE,
    D_DS_RPC_STAGE_PUBLISH_SERVER_RESPONSE_QUEUE,
    D_DS_RPC_STAGE_PUBLISH_COMMUNICATION_RESIDUAL,
    D_DS_RPC_STAGE_QUERY_CLIENT_PREPARE,
    D_DS_RPC_STAGE_QUERY_CLIENT_RESULT,
    D_DS_RPC_STAGE_QUERY_SERVER_REQUEST_QUEUE,
    D_DS_RPC_STAGE_QUERY_SERVER_RESPONSE_QUEUE,
    D_DS_RPC_STAGE_QUERY_COMMUNICATION_RESIDUAL,
    D_DS_RPC_STAGE_RELEASE_CLIENT_PREPARE,
    D_DS_RPC_STAGE_RELEASE_CLIENT_RESULT,
    D_DS_RPC_STAGE_RELEASE_SERVER_REQUEST_QUEUE,
    D_DS_RPC_STAGE_RELEASE_SERVER_RESPONSE_QUEUE,
    D_DS_RPC_STAGE_RELEASE_COMMUNICATION_RESIDUAL,
    D_DS_RPC_STAGE_CREATE_META_CLIENT_PREPARE,
    D_DS_RPC_STAGE_CREATE_META_CLIENT_RESULT,
    D_DS_RPC_STAGE_CREATE_META_SERVER_REQUEST_QUEUE,
    D_DS_RPC_STAGE_CREATE_META_SERVER_RESPONSE_QUEUE,
    D_DS_RPC_STAGE_CREATE_META_COMMUNICATION_RESIDUAL,
    D_DS_DETAIL_SHARED_CONNECTION_GET_TOTAL_CALLS,
    D_DS_DETAIL_SHARED_CONNECTION_LEASE_TOTAL_CALLS,
    D_DS_DETAIL_SHARED_CONNECTION_ENTRY_TOTAL_CALLS,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_TOTAL_CALLS,
    D_DS_DETAIL_GET_UB_ADMISSION_TOTAL_CALLS,
    D_DS_DETAIL_GET_UB_PREPARE_TOTAL_CALLS,
    D_DS_DETAIL_GET_UB_BUFFERS_TOTAL_CALLS,
    D_DS_DETAIL_GET_SHM_PREPARE_TOTAL_CALLS,
    D_DS_DETAIL_GET_SHM_RESULT_TOTAL_CALLS,
    D_DS_DETAIL_GET_UB_RESULT_TOTAL_CALLS,
    D_DS_DETAIL_GET_TCP_RESULT_TOTAL_CALLS,
    D_DS_DETAIL_GET_QUERY_TOTAL_CALLS,
    D_DS_DETAIL_GET_REQUEST_TOTAL_CALLS,
    D_DS_DETAIL_GET_RETRY_TOTAL_CALLS,
    D_DS_DETAIL_SET_SDK_TOTAL_CALLS,
    D_DS_DETAIL_SET_UB_CREATE_TOTAL_CALLS,
    D_DS_DETAIL_SET_UB_PUBLISH_TOTAL_CALLS,
    D_DS_DETAIL_SET_UB_PUBLISH_RPC_TOTAL_CALLS,
    D_DS_DETAIL_SET_WORKER_CREATE_TOTAL_CALLS,
    D_DS_DETAIL_SHARED_REFERENCE_ADD_TOTAL_CALLS,
    D_DS_DETAIL_RELEASE_WORKER_REFERENCE_TOTAL_CALLS,
    D_DS_DETAIL_RELEASE_WORKER_UPDATE_TOTAL_CALLS,
    D_DS_DETAIL_SET_MASTER_REQUEST_TOTAL_CALLS,
    D_DS_DETAIL_SET_MASTER_CREATE_TOTAL_CALLS,
    D_DS_DETAIL_SET_MASTER_BINARY_TOTAL_CALLS,
    D_DS_DETAIL_SET_MASTER_PERSIST_ACCESSOR_RELEASE_CALLS,
    D_DS_DETAIL_SET_MASTER_PERSIST_TOTAL_CALLS,
    D_DS_DETAIL_SET_MASTER_FIRST_TOTAL_CALLS,
    D_DS_DETAIL_SET_SHM_MAP_TOTAL_CALLS,
    D_DS_DETAIL_GET_SHM_MAP_TOTAL_CALLS,
    D_DS_DETAIL_GET_SHM_OWNER_TOTAL_CALLS,
    D_DS_DETAIL_SET_CREATE_TOTAL_CALLS,
    D_DS_DETAIL_SET_CREATE_ATTEMPT_TOTAL_CALLS,
    D_DS_DETAIL_SET_ROUTE_TOTAL_CALLS,
    D_DS_DETAIL_SET_ROUTE_CONTEXT_TOTAL_CALLS,
    D_DS_DETAIL_GET_WORKER_DELIVERY_TOTAL_CALLS,
    D_DS_DETAIL_GET_UB_WORKER_TOTAL_CALLS,
    D_DS_DETAIL_GET_SHM_WORKER_TOTAL_CALLS,
    D_DS_DETAIL_SET_WORKER_MASTER_TOTAL_CALLS,
    D_DS_DETAIL_SET_WORKER_CREATE_META_TOTAL_CALLS,
    D_DS_DETAIL_SHARED_REFERENCE_OBJECT_KEYS_TOTAL_CALLS,
    D_DS_DETAIL_SHARED_MASTER_HEAVY_OP_TOTAL_CALLS,
    D_DS_DETAIL_SET_RELEASE_TOTAL_CALLS,
    D_DS_DETAIL_SET_RELEASE_TASK_TOTAL_CALLS,
    D_DS_DETAIL_GET_SHM_RELEASE_TOTAL_CALLS,
    D_DS_DETAIL_GET_SHM_RELEASE_TASK_TOTAL_CALLS,
    D_MC_DETAIL_GET_MASTER_TOTAL_CALLS,
    D_MC_DETAIL_SET_MASTER_START_TOTAL_CALLS,
    D_MC_DETAIL_SET_MASTER_END_TOTAL_CALLS,
    D_MC_DETAIL_SET_MASTER_ALLOCATE_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ROUTE_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_BUFFER_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_TE_PREPARE_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_TE_SEGMENT_ID_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_FIFO_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_TE_ENDPOINT_SIEVE_TOTAL_CALLS,
    D_DS_DETAIL_GET_WORKER_OBJECT_TOTAL_CALLS,
    D_DS_DETAIL_SET_MASTER_API_LOCAL_TOTAL_CALLS,
    D_DS_DETAIL_SET_MASTER_API_REMOTE_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_TENANT_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_METADATA_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_PROCESSING_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_REPLICATION_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_TENANT_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_METADATA_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_PROCESSING_CALLS,
    D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_REPLICATION_CALLS,
    D_MC_DETAIL_SHARED_MASTER_SHARD_RW_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_MASTER_SHARD_RO_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_MASTER_LEASE_TOTAL_CALLS,
    D_DS_DETAIL_GET_RPC_WRAPPER_TOTAL_CALLS,
    D_DS_DETAIL_SET_SDK_UB_BUFFER_TOTAL_CALLS,
    D_DS_DETAIL_SET_SDK_SHM_BUFFER_TOTAL_CALLS,
    D_DS_DETAIL_SET_SDK_TCP_BUFFER_TOTAL_CALLS,
    D_DS_DETAIL_GET_QUERY_SHM_REQUEST_TOTAL_CALLS,
    D_DS_DETAIL_GET_QUERY_UB_REQUEST_TOTAL_CALLS,
    D_DS_DETAIL_GET_QUERY_TCP_REQUEST_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_RPC_COROUTINE_TOTAL_CALLS,
    D_MC_DETAIL_SHARED_RPC_TOTAL_CALLS,
    D_DS_DETAIL_SET_SDK_UB_BUFFER_CREATE,
    D_DS_DETAIL_SET_SDK_UB_BUFFER_COPY,
    D_DS_DETAIL_SET_SDK_UB_BUFFER_PUBLISH_PREPARE,
    D_DS_DETAIL_SET_SDK_UB_BUFFER_PUBLISH,
    D_DS_DETAIL_SET_SDK_UB_BUFFER_FEEDBACK_AND_CLEANUP,
    D_DS_DETAIL_SET_SDK_UB_BUFFER_CONTEXT,
    D_DS_DETAIL_SET_SDK_SHM_BUFFER_CREATE,
    D_DS_DETAIL_SET_SDK_SHM_BUFFER_COPY,
    D_DS_DETAIL_SET_SDK_SHM_BUFFER_PUBLISH_PREPARE,
    D_DS_DETAIL_SET_SDK_SHM_BUFFER_PUBLISH,
    D_DS_DETAIL_SET_SDK_SHM_BUFFER_FEEDBACK_AND_CLEANUP,
    D_DS_DETAIL_SET_SDK_SHM_BUFFER_CONTEXT,
    D_DS_DETAIL_SET_SDK_TCP_BUFFER_CREATE,
    D_DS_DETAIL_SET_SDK_TCP_BUFFER_COPY,
    D_DS_DETAIL_SET_SDK_TCP_BUFFER_PUBLISH_PREPARE,
    D_DS_DETAIL_SET_SDK_TCP_BUFFER_PUBLISH,
    D_DS_DETAIL_SET_SDK_TCP_BUFFER_FEEDBACK_AND_CLEANUP,
    D_DS_DETAIL_SET_SDK_TCP_BUFFER_CONTEXT,
    D_DS_DETAIL_GET_QUERY_UB_REQUEST_INITIALIZE,
    D_DS_DETAIL_GET_QUERY_UB_REQUEST_RPC_WITH_RETRY,
    D_DS_DETAIL_GET_QUERY_UB_REQUEST_APPLY_AND_CLEANUP,
    D_DS_DETAIL_GET_QUERY_UB_REQUEST_VALIDATE,
    D_DS_DETAIL_GET_QUERY_SHM_REQUEST_INITIALIZE,
    D_DS_DETAIL_GET_QUERY_SHM_REQUEST_RPC_WITH_RETRY,
    D_DS_DETAIL_GET_QUERY_SHM_REQUEST_APPLY_AND_CLEANUP,
    D_DS_DETAIL_GET_QUERY_SHM_REQUEST_VALIDATE,
    D_DS_DETAIL_GET_QUERY_TCP_REQUEST_INITIALIZE,
    D_DS_DETAIL_GET_QUERY_TCP_REQUEST_RPC_WITH_RETRY,
    D_DS_DETAIL_GET_QUERY_TCP_REQUEST_APPLY_AND_CLEANUP,
    D_DS_DETAIL_GET_QUERY_TCP_REQUEST_VALIDATE,
    D_DS_DETAIL_SET_API_SET_TRACE_CONTEXT,
    D_DS_DETAIL_SET_API_SET_PERF_AND_ACCESS_PREPARE,
    D_DS_DETAIL_SET_API_SET_IMPLEMENTATION,
    D_DS_DETAIL_SET_API_SET_METRICS,
    D_DS_DETAIL_SET_API_SET_ACCESS_RECORD,
    D_DS_DETAIL_SET_API_SET_TOTAL,
    D_DS_DETAIL_SET_API_SET_CONTEXT,
    D_DS_DETAIL_SET_API_SET_CLEANUP,
    D_DS_DETAIL_GET_API_GET_TRACE_CONTEXT,
    D_DS_DETAIL_GET_API_GET_PERF_AND_ACCESS_PREPARE,
    D_DS_DETAIL_GET_API_GET_IMPLEMENTATION,
    D_DS_DETAIL_GET_API_GET_METRICS,
    D_DS_DETAIL_GET_API_GET_ACCESS_RECORD,
    D_DS_DETAIL_GET_API_GET_TOTAL,
    D_DS_DETAIL_GET_API_GET_CONTEXT,
    D_DS_DETAIL_GET_API_GET_CLEANUP,
    D_DS_DETAIL_GET_API_GET_READONLY_TRACE_CONTEXT,
    D_DS_DETAIL_GET_API_GET_READONLY_PERF_AND_ACCESS_PREPARE,
    D_DS_DETAIL_GET_API_GET_READONLY_IMPLEMENTATION,
    D_DS_DETAIL_GET_API_GET_READONLY_METRICS,
    D_DS_DETAIL_GET_API_GET_READONLY_ACCESS_RECORD,
    D_DS_DETAIL_GET_API_GET_READONLY_TOTAL,
    D_DS_DETAIL_GET_API_GET_READONLY_CONTEXT,
    D_DS_DETAIL_GET_API_GET_READONLY_CLEANUP,
    D_DS_DETAIL_SET_API_SET_TOTAL_CALLS,
    D_DS_DETAIL_GET_API_GET_TOTAL_CALLS,
    D_DS_DETAIL_GET_API_GET_READONLY_TOTAL_CALLS,
    D_DS_DETAIL_SET_MASTER_API_OWNER_API,
    D_DS_DETAIL_SET_MASTER_API_OWNER_TOTAL,
    D_DS_DETAIL_SET_MASTER_API_OWNER_RESOLVE,
    D_DS_DETAIL_SHARED_MASTER_API_BUILD_ALLOCATE,
    D_DS_DETAIL_SHARED_MASTER_API_BUILD_INITIALIZE,
    D_DS_DETAIL_SHARED_MASTER_API_BUILD_RESULT,
    D_DS_DETAIL_SHARED_MASTER_API_BUILD_TOTAL,
    D_DS_DETAIL_SHARED_MASTER_API_BUILD_VALIDATE,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_WRITE_LOCK_ACQUIRE,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_RPC_CLIENT,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_TRANSPORTER,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_LOG_READY,
    D_DS_DETAIL_SHARED_CONNECTION_BUILD_CACHE_HIT_COUNT,
    D_DS_DETAIL_SET_MASTER_API_OWNER_TOTAL_CALLS,
    D_DS_DETAIL_SHARED_MASTER_API_BUILD_TOTAL_CALLS,
    COUNT
};
inline constexpr const char *STAGE_NAMES[] = {
    "ds.api.set", "ds.api.get", "ds.rpc.create", "ds.rpc.publish", "ds.rpc.query", "ds.rpc.get",
    "ds.server.create", "ds.server.publish", "ds.server.query", "ds.server.get",
    "ds.rpc_overhead.create", "ds.rpc_overhead.publish", "ds.rpc_overhead.query", "ds.rpc_overhead.get",
    "ds.rpc.release", "ds.server.release", "ds.rpc_overhead.release",
    "ds.rpc.create_meta", "ds.server.create_meta", "ds.rpc_overhead.create_meta",
    "ds.rpc.query_meta", "ds.server.query_meta", "ds.rpc_overhead.query_meta",
    "ds.shm.write_map", "ds.shm.read_map", "ds.shm.lookup", "ds.shm.fetch_map", "ds.copy", "ds.ub.alloc",
    "ds.ub.write", "ds.ub.submit", "ds.ub.wait",
    "mc.api.put", "mc.api.get", "mc.rpc.put_start", "mc.rpc.put_end", "mc.rpc.query",
    "mc.server.put_start", "mc.server.put_end", "mc.server.query",
    "mc.alloc.put", "mc.alloc.get", "mc.copy", "mc.te.read", "mc.te.write", "mc.te.submit", "mc.te.wait",
    "mc.local_copy",
    "ds.sdk.set.context",
    "ds.sdk.set.impl",
    "ds.sdk.set.access",
    "ds.sdk.get.context",
    "ds.sdk.get.impl",
    "ds.sdk.get.access",
    "ds.sdk.get.return",
    "ds.sdk.set.route",
    "ds.sdk.set.create",
    "ds.sdk.set.publish",
    "ds.sdk.set.cleanup",
    "ds.sdk.get.route",
    "ds.sdk.get.transport",
    "ds.sdk.get.materialize",
    "ds.sdk.get.query_prepare",
    "ds.sdk.get.query_attempt_prepare",
    "ds.sdk.get.query_attempt",
    "ds.sdk.get.query_retry",
    "ds.sdk.get.query_apply",
    "ds.sdk.get.ub_prepare",
    "ds.sdk.create.prepare",
    "ds.sdk.create.sign",
    "ds.sdk.create.call",
    "ds.sdk.create.result",
    "ds.sdk.publish.prepare",
    "ds.sdk.publish.sign",
    "ds.sdk.publish.call",
    "ds.sdk.publish.result",
    "ds.sdk.query.prepare",
    "ds.sdk.query.sign",
    "ds.sdk.query.call",
    "ds.sdk.query.result",
    "ds.worker.create.auth",
    "ds.worker.create.lookup",
    "ds.worker.create.reserve",
    "ds.worker.create.alloc",
    "ds.worker.create.response",
    "ds.worker.create.reference",
    "ds.worker.publish.auth",
    "ds.worker.publish.validate",
    "ds.worker.publish.lock",
    "ds.worker.publish.prepare",
    "ds.worker.publish.metadata",
    "ds.worker.publish.commit",
    "ds.worker.publish.cleanup",
    "ds.worker.query.auth",
    "ds.worker.query.local",
    "ds.worker.query.metadata",
    "ds.worker.query.delivery",
    "ds.worker.query.acquire",
    "ds.worker.query.ub_latch",
    "ds.worker.query.ub_prepare",
    "ds.worker.query.ub_write",
    "ds.worker.query.ub_result",
    "ds.worker.query.encode_shm",
    "ds.ub.poll_sleep",
    "ds.ub.notify",
    "ds.ub.wait_event",
    "ds.ub.wait_result",
    "mc.sdk.put.prepare",
    "mc.sdk.put.client_put",
    "mc.sdk.get.prepare",
    "mc.sdk.get.client_get",
    "mc.sdk.get.query_call",
    "mc.sdk.get.result",
    "mc.sdk.put.result",
    "ds.sdk.set.admission",
    "ds.sdk.set.execute",
    "ds.sdk.set.postprocess",
    "ds.sdk.get.input",
    "ds.sdk.get.execute",
    "ds.sdk.get.result",
    "ds.ub.wake_sched",
    "ds.ub.completion_observe",
    "ds.ub.event_process",
    "ds.worker.publish.postprocess",
    "ds.detail.shared.connection.get.transporter",
    "ds.detail.shared.connection.get.total",
    "ds.detail.shared.connection.get.entry",
    "ds.detail.shared.connection.lease.transporter",
    "ds.detail.shared.connection.lease.allocate",
    "ds.detail.shared.connection.lease.lock_acquire_with_guard_allocation",
    "ds.detail.shared.connection.lease.validate",
    "ds.detail.shared.connection.lease.result",
    "ds.detail.shared.connection.lease.total",
    "ds.detail.shared.connection.lease.entry",
    "ds.detail.shared.connection.entry.total",
    "ds.detail.shared.connection.entry.admission",
    "ds.detail.shared.connection.build.read_lock_acquire",
    "ds.detail.shared.connection.build.validate",
    "ds.detail.shared.connection.build.total",
    "ds.detail.shared.connection.build.prepare",
    "ds.detail.get.ub.admission.entry_lookup",
    "ds.detail.get.ub.admission.miss_handling",
    "ds.detail.get.ub.admission.lock_acquire",
    "ds.detail.get.ub.admission.validate",
    "ds.detail.get.ub.admission.total",
    "ds.detail.get.ub.admission.prepare",
    "ds.detail.get.ub.prepare.admission",
    "ds.detail.get.ub.prepare.connection",
    "ds.detail.get.ub.prepare.buffers",
    "ds.detail.get.ub.prepare.result",
    "ds.detail.get.ub.prepare.total",
    "ds.detail.get.ub.prepare.validate",
    "ds.detail.get.ub.buffers.items",
    "ds.detail.get.ub.buffers.total",
    "ds.detail.get.ub.buffers.reserve",
    "ds.detail.get.shm.prepare.connection",
    "ds.detail.get.shm.prepare.cast",
    "ds.detail.get.shm.prepare.session",
    "ds.detail.get.shm.prepare.result",
    "ds.detail.get.shm.prepare.total",
    "ds.detail.get.shm.prepare.validate",
    "ds.detail.get.shm.result.total",
    "ds.detail.get.shm.result.process",
    "ds.detail.get.ub.result.total",
    "ds.detail.get.ub.result.process",
    "ds.detail.get.tcp.result.total",
    "ds.detail.get.tcp.result.process",
    "ds.detail.get.query.initialize",
    "ds.detail.get.query.rpc_with_retry",
    "ds.detail.get.query.apply_and_cleanup",
    "ds.detail.get.query.total",
    "ds.detail.get.query.validate",
    "ds.detail.get.request.total",
    "ds.detail.get.request.build",
    "ds.detail.get.retry.total",
    "ds.detail.get.retry.prepare",
    "ds.detail.set.sdk.create",
    "ds.detail.set.sdk.copy",
    "ds.detail.set.sdk.publish_prepare",
    "ds.detail.set.sdk.publish",
    "ds.detail.set.sdk.feedback_and_cleanup",
    "ds.detail.set.sdk.total",
    "ds.detail.set.sdk.context",
    "ds.detail.set.ub.create.total",
    "ds.detail.set.ub.create.prepare",
    "ds.detail.set.ub.publish.total",
    "ds.detail.set.ub.publish.prepare",
    "ds.detail.set.ub.publish_rpc.total",
    "ds.detail.set.ub.publish_rpc.call",
    "ds.detail.set.worker.create.object_lookup",
    "ds.detail.set.worker.create.existing_object_lock_and_check",
    "ds.detail.set.worker.create.reserve",
    "ds.detail.set.worker.create.shm_unit_allocate",
    "ds.detail.set.worker.create.memory_allocate",
    "ds.detail.set.worker.create.response",
    "ds.detail.set.worker.create.client_lookup",
    "ds.detail.set.worker.create.reference",
    "ds.detail.set.worker.create.cleanup",
    "ds.detail.set.worker.create.total",
    "ds.detail.set.worker.create.namespace",
    "ds.detail.shared.reference.add.client_lookup",
    "ds.detail.shared.reference.add.object_ref",
    "ds.detail.shared.reference.add.shm_lookup_insert",
    "ds.detail.shared.reference.add.owner_insert",
    "ds.detail.shared.reference.add.page_metrics",
    "ds.detail.shared.reference.add.expiry",
    "ds.detail.shared.reference.add.metrics_cleanup",
    "ds.detail.shared.reference.add.total",
    "ds.detail.shared.reference.add.prepare",
    "ds.detail.release.worker.reference.client_lookup",
    "ds.detail.release.worker.reference.shm_lookup",
    "ds.detail.release.worker.reference.update",
    "ds.detail.release.worker.reference.expiry",
    "ds.detail.release.worker.reference.metrics_cleanup",
    "ds.detail.release.worker.reference.total",
    "ds.detail.release.worker.reference.prepare",
    "ds.detail.release.worker.update.page_metrics",
    "ds.detail.release.worker.update.owner_erase",
    "ds.detail.release.worker.update.total",
    "ds.detail.release.worker.update.object_ref",
    "ds.detail.set.master.request.route",
    "ds.detail.set.master.request.create",
    "ds.detail.set.master.request.result",
    "ds.detail.set.master.request.total",
    "ds.detail.set.master.request.prepare",
    "ds.detail.set.master.create.dispatch_and_cleanup",
    "ds.detail.set.master.create.total",
    "ds.detail.set.master.create.heavy_operation",
    "ds.detail.set.master.binary.shard_lock_acquire",
    "ds.detail.set.master.binary.table_insert",
    "ds.detail.set.master.binary.existence_check",
    "ds.detail.set.master.binary.existing_object",
    "ds.detail.set.master.binary.new_object",
    "ds.detail.set.master.binary.subscription",
    "ds.detail.set.master.binary.nested_references",
    "ds.detail.set.master.binary.expiry",
    "ds.detail.set.master.binary.total",
    "ds.detail.set.master.binary.prepare",
    "ds.detail.set.master.persist.fill_metadata",
    "ds.detail.set.master.persist.copy_metadata",
    "ds.detail.set.master.persist.serialize",
    "ds.detail.set.master.persist.store",
    "ds.detail.set.master.persist.accessor_release",
    "ds.detail.set.master.persist.total",
    "ds.detail.set.master.persist.prepare",
    "ds.detail.set.master.first.persist",
    "ds.detail.set.master.first.total",
    "ds.detail.set.master.first.expiry",
    "ds.detail.set.ub.create.rpc",
    "ds.detail.set.ub.create.info_allocate",
    "ds.detail.set.ub.create.buffer_allocate",
    "ds.detail.set.ub.create.info_fill",
    "ds.detail.set.ub.create.owner_allocate",
    "ds.detail.set.ub.create.buffer_construct",
    "ds.detail.set.shm.map.unit_allocate_fill",
    "ds.detail.set.shm.map.lookup_mmap",
    "ds.detail.set.shm.map.fd_lookup",
    "ds.detail.set.shm.map.reference",
    "ds.detail.set.shm.map.info_fill",
    "ds.detail.set.shm.map.owner_allocate",
    "ds.detail.set.shm.map.total",
    "ds.detail.set.shm.map.validate",
    "ds.detail.get.shm.map.total",
    "ds.detail.get.shm.map.validate",
    "ds.detail.get.shm.owner.total",
    "ds.detail.get.shm.owner.construct",
    "ds.detail.set.create.transport_hint",
    "ds.detail.set.create.allocation_id",
    "ds.detail.set.create.attempt",
    "ds.detail.set.create.retry_and_cleanup",
    "ds.detail.set.create.total",
    "ds.detail.set.create.validate",
    "ds.detail.set.create_attempt.total",
    "ds.detail.set.create_attempt.execute",
    "ds.detail.set.route.select",
    "ds.detail.set.route.total",
    "ds.detail.set.route.exclusions",
    "ds.detail.set.route_context.total",
    "ds.detail.set.route_context.build",
    "ds.detail.get.worker.delivery.execute",
    "ds.detail.get.worker.delivery.total",
    "ds.detail.get.worker.delivery.prepare",
    "ds.detail.get.ub.worker.guard_construct",
    "ds.detail.get.ub.worker.read_latch_acquire",
    "ds.detail.get.ub.worker.segment_prepare",
    "ds.detail.get.ub.worker.bandwidth_begin",
    "ds.detail.get.ub.worker.write",
    "ds.detail.get.ub.worker.result_and_cleanup",
    "ds.detail.get.ub.worker.total",
    "ds.detail.get.ub.worker.validate",
    "ds.detail.get.shm.worker.reference",
    "ds.detail.get.shm.worker.log",
    "ds.detail.get.shm.worker.encode",
    "ds.detail.get.shm.worker.total",
    "ds.detail.get.shm.worker.prepare",
    "ds.detail.set.worker.master.total",
    "ds.detail.set.worker.master.prepare",
    "ds.detail.set.worker.create_meta.total",
    "ds.detail.set.worker.create_meta.prepare",
    "ds.detail.shared.connection.entry.lookup",
    "ds.detail.shared.connection.entry.insert_allocate",
    "ds.detail.get.worker.delivery.health_summary",
    "ds.detail.get.worker.delivery.response_write",
    "ds.detail.get.worker.delivery.payload",
    "ds.detail.get.worker.delivery.complete",
    "ds.detail.get.worker.delivery.cleanup",
    "ds.detail.shared.reference.object_keys.prepare",
    "ds.detail.shared.reference.object_keys.emplace",
    "ds.detail.shared.reference.object_keys.update",
    "ds.detail.shared.reference.object_keys.total",
    "ds.detail.shared.reference.object_keys.lock_acquire",
    "ds.detail.shared.master.heavy_op.insert",
    "ds.detail.shared.master.heavy_op.total",
    "ds.detail.shared.master.heavy_op.lock_acquire",
    "ds.detail.set.release.pool",
    "ds.detail.set.release.enqueue_and_cleanup",
    "ds.detail.set.release.total",
    "ds.detail.set.release.validate",
    "ds.detail.set.release.queue_wait",
    "ds.detail.set.release.task.total",
    "ds.detail.set.release.task.prepare",
    "ds.detail.set.release.task.rpc",
    "ds.detail.get.shm.release.pool",
    "ds.detail.get.shm.release.enqueue_and_cleanup",
    "ds.detail.get.shm.release.total",
    "ds.detail.get.shm.release.validate",
    "ds.detail.get.shm.release.queue_wait",
    "ds.detail.get.shm.release.task.total",
    "ds.detail.get.shm.release.task.prepare",
    "ds.detail.get.shm.release.task.rpc",
    "mc.detail.get.master.identity",
    "mc.detail.get.master.accessor",
    "mc.detail.get.master.validate",
    "mc.detail.get.master.descriptors",
    "mc.detail.get.master.lease",
    "mc.detail.get.master.optional_policies",
    "mc.detail.get.master.total",
    "mc.detail.get.master.snapshot_lock_acquire",
    "mc.detail.set.master.start.object_lock_acquire",
    "mc.detail.set.master.start.prepare",
    "mc.detail.set.master.start.client_lock_acquire",
    "mc.detail.set.master.start.snapshot_lock_acquire",
    "mc.detail.set.master.start.clients_copy",
    "mc.detail.set.master.start.shard_route",
    "mc.detail.set.master.start.shard_lock_acquire",
    "mc.detail.set.master.start.tenant",
    "mc.detail.set.master.start.quota",
    "mc.detail.set.master.start.metadata_lookup",
    "mc.detail.set.master.start.existing_or_allocate",
    "mc.detail.set.master.start.total",
    "mc.detail.set.master.start.validate",
    "mc.detail.set.master.end.snapshot_lock_acquire",
    "mc.detail.set.master.end.identity",
    "mc.detail.set.master.end.accessor",
    "mc.detail.set.master.end.validate",
    "mc.detail.set.master.end.replica_update",
    "mc.detail.set.master.end.quota_and_optional_tasks",
    "mc.detail.set.master.end.processing_erase",
    "mc.detail.set.master.end.accounting",
    "mc.detail.set.master.end.lease",
    "mc.detail.set.master.end.event",
    "mc.detail.set.master.end.oplog_and_cleanup",
    "mc.detail.set.master.end.total",
    "mc.detail.set.master.end.prepare",
    "mc.detail.set.master.allocate.quota",
    "mc.detail.set.master.allocate.prepare",
    "mc.detail.set.master.allocate.segment_lock_acquire",
    "mc.detail.set.master.allocate.placement",
    "mc.detail.set.master.allocate.memory",
    "mc.detail.set.master.allocate.descriptors",
    "mc.detail.set.master.allocate.soft_pin",
    "mc.detail.set.master.allocate.group_register",
    "mc.detail.set.master.allocate.processing_insert",
    "mc.detail.set.master.allocate.total",
    "mc.detail.set.master.allocate.tenant_and_checks",
    "mc.detail.shared.master.route.group_lookup",
    "mc.detail.shared.master.route.total",
    "mc.detail.shared.master.route.lock_acquire",
    "mc.detail.shared.buffer.allocate",
    "mc.detail.shared.buffer.handle_construct",
    "mc.detail.shared.buffer.total",
    "mc.detail.shared.buffer.validate",
    "mc.detail.shared.te.prepare.open_segment",
    "mc.detail.shared.te.prepare.requests",
    "mc.detail.shared.te.prepare.submit",
    "mc.detail.shared.te.prepare.total",
    "mc.detail.shared.te.prepare.validate",
    "mc.detail.shared.te.segment_name.read_lock_acquire",
    "mc.detail.shared.te.segment_name.lookup",
    "mc.detail.shared.te.segment_name.fetch",
    "mc.detail.shared.te.segment_name.write_lock_acquire",
    "mc.detail.shared.te.segment_name.total",
    "mc.detail.shared.te.segment_name.prepare",
    "mc.detail.shared.te.segment_id.read_lock_acquire",
    "mc.detail.shared.te.segment_id.lookup",
    "mc.detail.shared.te.segment_id.fetch",
    "mc.detail.shared.te.segment_id.write_lock_acquire",
    "mc.detail.shared.te.segment_id.total",
    "mc.detail.shared.te.segment_id.prepare",
    "mc.detail.shared.te.endpoint_fifo.lookup",
    "mc.detail.shared.te.endpoint_fifo.result",
    "mc.detail.shared.te.endpoint_fifo.total",
    "mc.detail.shared.te.endpoint_fifo.lock_acquire",
    "mc.detail.shared.te.endpoint_sieve.lookup",
    "mc.detail.shared.te.endpoint_sieve.result",
    "mc.detail.shared.te.endpoint_sieve.total",
    "mc.detail.shared.te.endpoint_sieve.lock_acquire",
    "ds.detail.get.worker.object.read_lock_acquire",
    "ds.detail.get.worker.object.validate",
    "ds.detail.get.worker.object.params_allocate",
    "ds.detail.get.worker.object.total",
    "ds.detail.get.worker.object.lookup",
    "ds.detail.set.worker.create_meta.request",
    "ds.detail.set.worker.create_meta.route_and_call",
    "ds.detail.set.worker.create_meta.result",
    "ds.detail.set.master_api.local.total",
    "ds.detail.set.master_api.local.prepare",
    "ds.detail.set.master_api.remote.total",
    "ds.detail.set.master_api.remote.prepare",
    "mc.detail.shared.master.accessor_rw.tenant",
    "mc.detail.shared.master.accessor_rw.metadata",
    "mc.detail.shared.master.accessor_rw.processing",
    "mc.detail.shared.master.accessor_rw.replication",
    "mc.detail.shared.master.accessor_ro.tenant",
    "mc.detail.shared.master.accessor_ro.metadata",
    "mc.detail.shared.master.accessor_ro.processing",
    "mc.detail.shared.master.accessor_ro.replication",
    "mc.detail.shared.master.shard_rw.total",
    "mc.detail.shared.master.shard_rw.lock_acquire",
    "mc.detail.shared.master.shard_rw.held",
    "mc.detail.shared.master.shard_ro.total",
    "mc.detail.shared.master.shard_ro.lock_acquire",
    "mc.detail.shared.master.shard_ro.held",
    "mc.detail.shared.master.lease.update",
    "mc.detail.shared.master.lease.total",
    "mc.detail.shared.master.lease.lock_acquire",
    "ds.detail.set.route.choose_worker",
    "ds.detail.set.route.context",
    "ds.detail.set.route_context.lock_acquire",
    "ds.detail.set.route_context.identity",
    "ds.detail.set.route_context.guard_allocate",
    "ds.detail.set.route_context.connection_check",
    "ds.detail.set.create_attempt.connection",
    "ds.detail.set.create_attempt.admission",
    "ds.detail.set.create_attempt.transport",
    "ds.detail.set.create_attempt.result",
    "ds.detail.set.master_api.local.sign",
    "ds.detail.set.master_api.local.call_with_retry",
    "ds.detail.get.rpc_wrapper.sign",
    "ds.detail.get.rpc_wrapper.options",
    "ds.detail.get.rpc_wrapper.call",
    "ds.detail.get.rpc_wrapper.health_and_result",
    "ds.detail.get.rpc_wrapper.total",
    "ds.detail.get.rpc_wrapper.validate",
    "ds.detail.shared.connection.entry.hit.count",
    "ds.detail.shared.connection.entry.miss.count",
    "ds.detail.release.worker.reference.client_miss.count",
    "ds.detail.release.worker.reference.shm_miss.count",
    "mc.detail.shared.te.segment_name.fetch.count",
    "ds.detail.set.sdk.ub_buffer.total",
    "ds.detail.set.sdk.shm_buffer.total",
    "ds.detail.set.sdk.tcp_buffer.total",
    "ds.detail.get.query.shm_request.total",
    "ds.detail.get.query.ub_request.total",
    "ds.detail.get.query.tcp_request.total",
    "mc.detail.set.master.allocate.metadata_insert",
    "ds.detail.get.ub.buffers.allocate",
    "ds.detail.get.ub.buffers.validate",
    "ds.detail.get.ub.buffers.emplace",
    "ds.detail.shared.connection.lease.held",
    "mc.detail.shared.rpc.metrics_prepare",
    "mc.detail.shared.rpc.dispatch_and_wait",
    "mc.detail.shared.rpc.coroutine.total",
    "mc.detail.shared.rpc.coroutine.pool_dispatch",
    "mc.detail.shared.rpc.coroutine.response_wait",
    "mc.detail.shared.rpc.coroutine.result",
    "mc.detail.shared.rpc.total",
    "mc.detail.shared.rpc.pool",
    "ds.rpc_stage.create.client_prepare",
    "ds.rpc_stage.create.client_result",
    "ds.rpc_stage.create.server_request_queue",
    "ds.rpc_stage.create.server_response_queue",
    "ds.rpc_stage.create.communication_residual",
    "ds.rpc_stage.publish.client_prepare",
    "ds.rpc_stage.publish.client_result",
    "ds.rpc_stage.publish.server_request_queue",
    "ds.rpc_stage.publish.server_response_queue",
    "ds.rpc_stage.publish.communication_residual",
    "ds.rpc_stage.query.client_prepare",
    "ds.rpc_stage.query.client_result",
    "ds.rpc_stage.query.server_request_queue",
    "ds.rpc_stage.query.server_response_queue",
    "ds.rpc_stage.query.communication_residual",
    "ds.rpc_stage.release.client_prepare",
    "ds.rpc_stage.release.client_result",
    "ds.rpc_stage.release.server_request_queue",
    "ds.rpc_stage.release.server_response_queue",
    "ds.rpc_stage.release.communication_residual",
    "ds.rpc_stage.create_meta.client_prepare",
    "ds.rpc_stage.create_meta.client_result",
    "ds.rpc_stage.create_meta.server_request_queue",
    "ds.rpc_stage.create_meta.server_response_queue",
    "ds.rpc_stage.create_meta.communication_residual",
    "ds.detail.shared.connection.get.total.calls",
    "ds.detail.shared.connection.lease.total.calls",
    "ds.detail.shared.connection.entry.total.calls",
    "ds.detail.shared.connection.build.total.calls",
    "ds.detail.get.ub.admission.total.calls",
    "ds.detail.get.ub.prepare.total.calls",
    "ds.detail.get.ub.buffers.total.calls",
    "ds.detail.get.shm.prepare.total.calls",
    "ds.detail.get.shm.result.total.calls",
    "ds.detail.get.ub.result.total.calls",
    "ds.detail.get.tcp.result.total.calls",
    "ds.detail.get.query.total.calls",
    "ds.detail.get.request.total.calls",
    "ds.detail.get.retry.total.calls",
    "ds.detail.set.sdk.total.calls",
    "ds.detail.set.ub.create.total.calls",
    "ds.detail.set.ub.publish.total.calls",
    "ds.detail.set.ub.publish_rpc.total.calls",
    "ds.detail.set.worker.create.total.calls",
    "ds.detail.shared.reference.add.total.calls",
    "ds.detail.release.worker.reference.total.calls",
    "ds.detail.release.worker.update.total.calls",
    "ds.detail.set.master.request.total.calls",
    "ds.detail.set.master.create.total.calls",
    "ds.detail.set.master.binary.total.calls",
    "ds.detail.set.master.persist.accessor_release.calls",
    "ds.detail.set.master.persist.total.calls",
    "ds.detail.set.master.first.total.calls",
    "ds.detail.set.shm.map.total.calls",
    "ds.detail.get.shm.map.total.calls",
    "ds.detail.get.shm.owner.total.calls",
    "ds.detail.set.create.total.calls",
    "ds.detail.set.create_attempt.total.calls",
    "ds.detail.set.route.total.calls",
    "ds.detail.set.route_context.total.calls",
    "ds.detail.get.worker.delivery.total.calls",
    "ds.detail.get.ub.worker.total.calls",
    "ds.detail.get.shm.worker.total.calls",
    "ds.detail.set.worker.master.total.calls",
    "ds.detail.set.worker.create_meta.total.calls",
    "ds.detail.shared.reference.object_keys.total.calls",
    "ds.detail.shared.master.heavy_op.total.calls",
    "ds.detail.set.release.total.calls",
    "ds.detail.set.release.task.total.calls",
    "ds.detail.get.shm.release.total.calls",
    "ds.detail.get.shm.release.task.total.calls",
    "mc.detail.get.master.total.calls",
    "mc.detail.set.master.start.total.calls",
    "mc.detail.set.master.end.total.calls",
    "mc.detail.set.master.allocate.total.calls",
    "mc.detail.shared.master.route.total.calls",
    "mc.detail.shared.buffer.total.calls",
    "mc.detail.shared.te.prepare.total.calls",
    "mc.detail.shared.te.segment_name.total.calls",
    "mc.detail.shared.te.segment_id.total.calls",
    "mc.detail.shared.te.endpoint_fifo.total.calls",
    "mc.detail.shared.te.endpoint_sieve.total.calls",
    "ds.detail.get.worker.object.total.calls",
    "ds.detail.set.master_api.local.total.calls",
    "ds.detail.set.master_api.remote.total.calls",
    "mc.detail.shared.master.accessor_rw.tenant.calls",
    "mc.detail.shared.master.accessor_rw.metadata.calls",
    "mc.detail.shared.master.accessor_rw.processing.calls",
    "mc.detail.shared.master.accessor_rw.replication.calls",
    "mc.detail.shared.master.accessor_ro.tenant.calls",
    "mc.detail.shared.master.accessor_ro.metadata.calls",
    "mc.detail.shared.master.accessor_ro.processing.calls",
    "mc.detail.shared.master.accessor_ro.replication.calls",
    "mc.detail.shared.master.shard_rw.total.calls",
    "mc.detail.shared.master.shard_ro.total.calls",
    "mc.detail.shared.master.lease.total.calls",
    "ds.detail.get.rpc_wrapper.total.calls",
    "ds.detail.set.sdk.ub_buffer.total.calls",
    "ds.detail.set.sdk.shm_buffer.total.calls",
    "ds.detail.set.sdk.tcp_buffer.total.calls",
    "ds.detail.get.query.shm_request.total.calls",
    "ds.detail.get.query.ub_request.total.calls",
    "ds.detail.get.query.tcp_request.total.calls",
    "mc.detail.shared.rpc.coroutine.total.calls",
    "mc.detail.shared.rpc.total.calls",
    "ds.detail.set.sdk.ub_buffer.create",
    "ds.detail.set.sdk.ub_buffer.copy",
    "ds.detail.set.sdk.ub_buffer.publish_prepare",
    "ds.detail.set.sdk.ub_buffer.publish",
    "ds.detail.set.sdk.ub_buffer.feedback_and_cleanup",
    "ds.detail.set.sdk.ub_buffer.context",
    "ds.detail.set.sdk.shm_buffer.create",
    "ds.detail.set.sdk.shm_buffer.copy",
    "ds.detail.set.sdk.shm_buffer.publish_prepare",
    "ds.detail.set.sdk.shm_buffer.publish",
    "ds.detail.set.sdk.shm_buffer.feedback_and_cleanup",
    "ds.detail.set.sdk.shm_buffer.context",
    "ds.detail.set.sdk.tcp_buffer.create",
    "ds.detail.set.sdk.tcp_buffer.copy",
    "ds.detail.set.sdk.tcp_buffer.publish_prepare",
    "ds.detail.set.sdk.tcp_buffer.publish",
    "ds.detail.set.sdk.tcp_buffer.feedback_and_cleanup",
    "ds.detail.set.sdk.tcp_buffer.context",
    "ds.detail.get.query.ub_request.initialize",
    "ds.detail.get.query.ub_request.rpc_with_retry",
    "ds.detail.get.query.ub_request.apply_and_cleanup",
    "ds.detail.get.query.ub_request.validate",
    "ds.detail.get.query.shm_request.initialize",
    "ds.detail.get.query.shm_request.rpc_with_retry",
    "ds.detail.get.query.shm_request.apply_and_cleanup",
    "ds.detail.get.query.shm_request.validate",
    "ds.detail.get.query.tcp_request.initialize",
    "ds.detail.get.query.tcp_request.rpc_with_retry",
    "ds.detail.get.query.tcp_request.apply_and_cleanup",
    "ds.detail.get.query.tcp_request.validate",
    "ds.detail.set.api_set.trace_context",
    "ds.detail.set.api_set.perf_and_access_prepare",
    "ds.detail.set.api_set.implementation",
    "ds.detail.set.api_set.metrics",
    "ds.detail.set.api_set.access_record",
    "ds.detail.set.api_set.total",
    "ds.detail.set.api_set.context",
    "ds.detail.set.api_set.cleanup",
    "ds.detail.get.api_get.trace_context",
    "ds.detail.get.api_get.perf_and_access_prepare",
    "ds.detail.get.api_get.implementation",
    "ds.detail.get.api_get.metrics",
    "ds.detail.get.api_get.access_record",
    "ds.detail.get.api_get.total",
    "ds.detail.get.api_get.context",
    "ds.detail.get.api_get.cleanup",
    "ds.detail.get.api_get_readonly.trace_context",
    "ds.detail.get.api_get_readonly.perf_and_access_prepare",
    "ds.detail.get.api_get_readonly.implementation",
    "ds.detail.get.api_get_readonly.metrics",
    "ds.detail.get.api_get_readonly.access_record",
    "ds.detail.get.api_get_readonly.total",
    "ds.detail.get.api_get_readonly.context",
    "ds.detail.get.api_get_readonly.cleanup",
    "ds.detail.set.api_set.total.calls",
    "ds.detail.get.api_get.total.calls",
    "ds.detail.get.api_get_readonly.total.calls",
    "ds.detail.set.master_api.owner.api",
    "ds.detail.set.master_api.owner.total",
    "ds.detail.set.master_api.owner.resolve",
    "ds.detail.shared.master_api.build.allocate",
    "ds.detail.shared.master_api.build.initialize",
    "ds.detail.shared.master_api.build.result",
    "ds.detail.shared.master_api.build.total",
    "ds.detail.shared.master_api.build.validate",
    "ds.detail.shared.connection.build.write_lock_acquire",
    "ds.detail.shared.connection.build.rpc_client",
    "ds.detail.shared.connection.build.transporter",
    "ds.detail.shared.connection.build.log_ready",
    "ds.detail.shared.connection.build.cache_hit.count",
    "ds.detail.set.master_api.owner.total.calls",
    "ds.detail.shared.master_api.build.total.calls"
};
inline constexpr std::array<uint64_t, 22> BUCKET_NS = {
    100, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000, 64000, 128000, 256000,
    512000, 1000000, 2000000, 4000000, 8000000, 16000000, 64000000, 256000000,
    1000000000, std::numeric_limits<uint64_t>::max()
};
static_assert(sizeof(STAGE_NAMES) / sizeof(*STAGE_NAMES) == static_cast<size_t>(Stage::COUNT));

inline bool Enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("KVTEST_STAGE_PROFILE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

inline uint64_t DetailSampleEvery();

namespace detail {
inline uint64_t NowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct alignas(64) Slot {
    std::atomic<uint64_t> sum{0}, max{0}, bytes{0}, failed{0}, unknown{0};
    std::array<std::atomic<uint64_t>, BUCKET_NS.size()> buckets{};
};

struct DetailSample {
    Stage group{Stage::COUNT};
    uint64_t start{0}, duration{0};
    size_t size{0};
    bool overflow{false};
    int outcome{-1};
    std::array<Stage, 32> stages{};
    std::array<uint64_t, 32> durations{};
};

class DetailSamples {
public:
    void Save(const DetailSample &sample)
    {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || size_ == samples_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        samples_[size_++] = sample;
    }

    void Flush(FILE *output, const std::string &instance, uint64_t seq, bool final)
    {
        std::array<DetailSample, 256> pending;
        size_t size;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            size = size_;
            for (size_t i = 0; i < size; ++i) pending[i] = samples_[i];
            size_ = 0;
        }
        for (size_t i = 0; i < size; ++i) {
            const auto &s = pending[i];
            std::fprintf(output, "[KVTEST_DETAIL_SAMPLE] {\"schema\":1,\"instance\":\"%s\",\"seq\":%llu,"
                "\"final\":%s,\"id\":%llu,\"group\":\"%s\",\"start_ns\":%llu,\"duration_ns\":%llu,"
                "\"overflow\":%s,\"outcome\":%d,\"steps\":[", instance.c_str(),
                static_cast<unsigned long long>(seq), final ? "true" : "false",
                static_cast<unsigned long long>(++sampleId_), STAGE_NAMES[static_cast<size_t>(s.group)],
                static_cast<unsigned long long>(s.start), static_cast<unsigned long long>(s.duration),
                s.overflow ? "true" : "false", s.outcome);
            for (size_t j = 0; j < s.size; ++j) {
                std::fprintf(output, "%s{\"phase\":\"%s\",\"duration_ns\":%llu}", j == 0 ? "" : ",",
                    STAGE_NAMES[static_cast<size_t>(s.stages[j])],
                    static_cast<unsigned long long>(s.durations[j]));
            }
            std::fprintf(output, "]}\n");
        }
        std::fprintf(output, "[KVTEST_DETAIL_STATUS] {\"schema\":1,\"instance\":\"%s\",\"seq\":%llu,"
            "\"final\":%s,\"dropped_lifetime\":%llu}\n", instance.c_str(),
            static_cast<unsigned long long>(seq), final ? "true" : "false",
            static_cast<unsigned long long>(dropped_.load(std::memory_order_relaxed)));
    }
private:
    std::mutex mutex_;
    std::array<DetailSample, 256> samples_{};
    size_t size_{0};
    uint64_t sampleId_{0};
    std::atomic<uint64_t> dropped_{0};
};

inline DetailSamples *GetDetailSamples()
{
    static DetailSamples *const samples = new DetailSamples();
    return samples;
}

inline void SaveDetailSample(Stage group, uint64_t start, uint64_t duration,
                             const std::array<Stage, 32> &stages,
                             const std::array<uint64_t, 32> &durations,
                             size_t size, bool overflow, int outcome) noexcept
{
    static const bool enabled = [] {
        const char *value = std::getenv("KVTEST_DETAIL_SAMPLES");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (!enabled) return;
    try {
        DetailSample sample;
        sample.group = group; sample.start = start; sample.duration = duration;
        sample.stages = stages; sample.durations = durations; sample.size = size;
        sample.overflow = overflow; sample.outcome = outcome;
        GetDetailSamples()->Save(sample);
    } catch (...) {
        // Optional diagnostics must not affect the request result or cleanup.
    }
}

class Recorder {
public:
    Recorder()
    {
        char host[256]{};
        if (gethostname(host, sizeof(host) - 1) != 0) std::strcpy(host, "unknown");
        for (char &c : host) {
            if (c == '\0') break;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '.')) c = '_';
        }
        instance_ = std::string(host) + ":" + std::to_string(getpid()) + ":" + std::to_string(NowNs());
        const char *prefix = std::getenv("KVTEST_STAGE_PROFILE_OUTPUT");
        if (prefix != nullptr && *prefix != '\0') {
            const std::string path = std::string(prefix) + "." + std::to_string(getpid())
                + "." + std::to_string(NowNs()) + ".log";
            output_ = std::fopen(path.c_str(), "w");
            if (output_ == nullptr) std::fprintf(stderr, "KVTEST_STAGE_PROFILE: cannot open output file\n");
        } else {
            output_ = stderr;
        }
        try {
            reporter_ = std::thread([this] {
                std::unique_lock<std::mutex> lock(stopMutex_);
                while (!stopping_) {
                    if (stopCv_.wait_for(lock, std::chrono::seconds(10), [this] { return stopping_; })) break;
                    Flush(false);
                }
            });
        } catch (...) {
            if (output_ != nullptr && output_ != stderr) std::fclose(output_);
            throw;
        }
    }

    ~Recorder() { Stop(); }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(stopMutex_);
            if (stopping_) return;
            stopping_ = true;
        }
        stopCv_.notify_one();
        reporter_.join();
        Flush(true);
    }

    Slot &GetSlot(Stage stage)
    {
        // Shards are independent accumulators; bthread migration does not require thread affinity.
        static std::atomic<size_t> next{0};
        thread_local const size_t shard = next.fetch_add(1, std::memory_order_relaxed) % SHARDS;
        return slots_[static_cast<size_t>(stage)][shard];
    }

    void Flush(bool final) noexcept
    {
        try {
            FlushImpl(final);
        } catch (...) {
            std::fprintf(stderr, "KVTEST_STAGE_PROFILE: snapshot failed\n");
        }
    }

    void FlushImpl(bool final)
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        if (output_ == nullptr) return;
        ++sequence_;
        for (size_t stage = 0; stage < slots_.size(); ++stage) {
            std::array<uint64_t, BUCKET_NS.size()> buckets{};
            uint64_t sum = 0, max = 0, bytes = 0, failed = 0, unknown = 0, count = 0;
            for (const auto &slot : slots_[stage]) {
                sum += slot.sum.load(std::memory_order_relaxed);
                bytes += slot.bytes.load(std::memory_order_relaxed);
                failed += slot.failed.load(std::memory_order_relaxed);
                unknown += slot.unknown.load(std::memory_order_relaxed);
                const auto value = slot.max.load(std::memory_order_relaxed);
                if (value > max) max = value;
                for (size_t i = 0; i < buckets.size(); ++i) buckets[i] += slot.buckets[i].load(std::memory_order_relaxed);
            }
            for (auto value : buckets) count += value;
            if (count == 0) continue;
            std::string histogram;
            for (size_t i = 0; i < buckets.size(); ++i) {
                if (i != 0) histogram += ',';
                histogram += std::to_string(buckets[i]);
            }
            std::fprintf(output_, "[KVTEST_STAGE_PROFILE] {\"schema\":1,\"instance\":\"%s\",\"seq\":%llu,"
                "\"final\":%s,\"detail_sample_every\":%llu,\"phase\":\"%s\",\"count\":%llu,\"sum_ns\":%llu,\"max_ns\":%llu,"
                "\"requested_bytes\":%llu,\"failed\":%llu,\"unknown\":%llu,\"buckets\":[%s]}\n",
                instance_.c_str(), static_cast<unsigned long long>(sequence_), final ? "true" : "false",
                static_cast<unsigned long long>(DetailSampleEvery()), STAGE_NAMES[stage], static_cast<unsigned long long>(count), static_cast<unsigned long long>(sum),
                static_cast<unsigned long long>(max), static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(failed), static_cast<unsigned long long>(unknown), histogram.c_str());
        }
        const char *detailSamples = std::getenv("KVTEST_DETAIL_SAMPLES");
        if (detailSamples != nullptr && std::strcmp(detailSamples, "1") == 0) {
            GetDetailSamples()->Flush(output_, instance_, sequence_, final);
        }
        rusage resources{};
        if (getrusage(RUSAGE_SELF, &resources) == 0) {
            const auto userUs = static_cast<uint64_t>(resources.ru_utime.tv_sec) * 1000000
                                + resources.ru_utime.tv_usec;
            const auto systemUs = static_cast<uint64_t>(resources.ru_stime.tv_sec) * 1000000
                                  + resources.ru_stime.tv_usec;
            std::fprintf(output_, "[KVTEST_PROFILE_RESOURCE] {\"schema\":1,\"instance\":\"%s\",\"seq\":%llu,"
                "\"final\":%s,\"monotonic_ns\":%llu,\"user_cpu_us\":%llu,\"system_cpu_us\":%llu,"
                "\"voluntary_switches\":%llu,\"involuntary_switches\":%llu,\"minor_faults\":%llu,\"major_faults\":%llu}\n",
                instance_.c_str(), static_cast<unsigned long long>(sequence_), final ? "true" : "false",
                static_cast<unsigned long long>(NowNs()), static_cast<unsigned long long>(userUs),
                static_cast<unsigned long long>(systemUs), static_cast<unsigned long long>(resources.ru_nvcsw),
                static_cast<unsigned long long>(resources.ru_nivcsw), static_cast<unsigned long long>(resources.ru_minflt),
                static_cast<unsigned long long>(resources.ru_majflt));
        }
        if (std::fflush(output_) != 0) std::fprintf(stderr, "KVTEST_STAGE_PROFILE: output write failed\n");
    }

private:
    static constexpr size_t SHARDS = 16;
    std::array<std::array<Slot, SHARDS>, static_cast<size_t>(Stage::COUNT)> slots_{};
    FILE *output_{nullptr};
    std::string instance_;
    uint64_t sequence_{0};
    std::mutex stopMutex_, outputMutex_;
    std::condition_variable stopCv_;
    bool stopping_{false};
    std::thread reporter_;
};

inline Recorder *GetRecorder() noexcept
{
    // Keep counters alive for SDK cleanup in later static destructors; stop the exporter at exit.
    static Recorder *const recorder = []() noexcept -> Recorder * {
        try {
            auto value = std::make_unique<Recorder>();
            if (std::atexit([] { if (auto *recorder = GetRecorder()) recorder->Stop(); }) != 0) return nullptr;
            return value.release();
        } catch (...) {
            std::fprintf(stderr, "KVTEST_STAGE_PROFILE: recorder initialization failed\n");
            return nullptr;
        }
    }();
    return recorder;
}

inline Slot *GetSlot(Stage stage) noexcept
{
    if (stage == Stage::COUNT || !Enabled()) return nullptr;
    auto *recorder = GetRecorder();
    return recorder == nullptr ? nullptr : &recorder->GetSlot(stage);
}

inline void Record(Slot &slot, uint64_t ns, uint64_t bytes, int outcome)
{
    slot.sum.fetch_add(ns, std::memory_order_relaxed);
    if (bytes != 0) slot.bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (outcome == 0) slot.failed.fetch_add(1, std::memory_order_relaxed);
    if (outcome < 0) slot.unknown.fetch_add(1, std::memory_order_relaxed);
    auto max = slot.max.load(std::memory_order_relaxed);
    while (ns > max && !slot.max.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {}
    size_t bucket = 0;
    while (ns > BUCKET_NS[bucket]) ++bucket;
    slot.buckets[bucket].fetch_add(1, std::memory_order_relaxed);
}
}  // namespace detail

inline void RecordDuration(Stage stage, uint64_t ns, bool success)
{
    if (auto *slot = detail::GetSlot(stage)) detail::Record(*slot, ns, 0, success ? 1 : 0);
}

class Span {
public:
    explicit Span(Stage stage, uint64_t requestedBytes = 0)
        : slot_(detail::GetSlot(stage)), bytes_(requestedBytes),
          start_(slot_ == nullptr ? 0 : detail::NowNs())
    {
    }
    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;
    ~Span() { FinishUnknown(); }

    void Next(Stage stage)
    {
        FinishUnknown();
        slot_ = detail::GetSlot(stage);
        bytes_ = 0;
        start_ = slot_ == nullptr ? 0 : detail::NowNs();
    }

    void Finish(bool success)
    {
        Record(success ? 1 : 0);
    }

private:
    void FinishUnknown() { Record(-1); }
    void Record(int outcome)
    {
        if (slot_ == nullptr) return;
        const uint64_t ns = detail::NowNs() - start_;
        detail::Record(*slot_, ns, bytes_, outcome);
        slot_ = nullptr;
    }
    detail::Slot *slot_;
    uint64_t bytes_, start_;
};

inline Stage DetailCallStage(Stage stage)
{
    switch (stage) {
        case Stage::D_DS_DETAIL_SHARED_CONNECTION_GET_TOTAL: return Stage::D_DS_DETAIL_SHARED_CONNECTION_GET_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SHARED_CONNECTION_LEASE_TOTAL: return Stage::D_DS_DETAIL_SHARED_CONNECTION_LEASE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SHARED_CONNECTION_ENTRY_TOTAL: return Stage::D_DS_DETAIL_SHARED_CONNECTION_ENTRY_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SHARED_CONNECTION_BUILD_TOTAL: return Stage::D_DS_DETAIL_SHARED_CONNECTION_BUILD_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_UB_ADMISSION_TOTAL: return Stage::D_DS_DETAIL_GET_UB_ADMISSION_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_UB_PREPARE_TOTAL: return Stage::D_DS_DETAIL_GET_UB_PREPARE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_UB_BUFFERS_TOTAL: return Stage::D_DS_DETAIL_GET_UB_BUFFERS_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_SHM_PREPARE_TOTAL: return Stage::D_DS_DETAIL_GET_SHM_PREPARE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_SHM_RESULT_TOTAL: return Stage::D_DS_DETAIL_GET_SHM_RESULT_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_UB_RESULT_TOTAL: return Stage::D_DS_DETAIL_GET_UB_RESULT_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_TCP_RESULT_TOTAL: return Stage::D_DS_DETAIL_GET_TCP_RESULT_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_QUERY_TOTAL: return Stage::D_DS_DETAIL_GET_QUERY_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_REQUEST_TOTAL: return Stage::D_DS_DETAIL_GET_REQUEST_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_RETRY_TOTAL: return Stage::D_DS_DETAIL_GET_RETRY_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_SDK_TOTAL: return Stage::D_DS_DETAIL_SET_SDK_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_UB_CREATE_TOTAL: return Stage::D_DS_DETAIL_SET_UB_CREATE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_UB_PUBLISH_TOTAL: return Stage::D_DS_DETAIL_SET_UB_PUBLISH_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_UB_PUBLISH_RPC_TOTAL: return Stage::D_DS_DETAIL_SET_UB_PUBLISH_RPC_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_WORKER_CREATE_TOTAL: return Stage::D_DS_DETAIL_SET_WORKER_CREATE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SHARED_REFERENCE_ADD_TOTAL: return Stage::D_DS_DETAIL_SHARED_REFERENCE_ADD_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_RELEASE_WORKER_REFERENCE_TOTAL: return Stage::D_DS_DETAIL_RELEASE_WORKER_REFERENCE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_RELEASE_WORKER_UPDATE_TOTAL: return Stage::D_DS_DETAIL_RELEASE_WORKER_UPDATE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_REQUEST_TOTAL: return Stage::D_DS_DETAIL_SET_MASTER_REQUEST_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_CREATE_TOTAL: return Stage::D_DS_DETAIL_SET_MASTER_CREATE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_BINARY_TOTAL: return Stage::D_DS_DETAIL_SET_MASTER_BINARY_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_PERSIST_ACCESSOR_RELEASE: return Stage::D_DS_DETAIL_SET_MASTER_PERSIST_ACCESSOR_RELEASE_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_PERSIST_TOTAL: return Stage::D_DS_DETAIL_SET_MASTER_PERSIST_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_FIRST_TOTAL: return Stage::D_DS_DETAIL_SET_MASTER_FIRST_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_SHM_MAP_TOTAL: return Stage::D_DS_DETAIL_SET_SHM_MAP_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_SHM_MAP_TOTAL: return Stage::D_DS_DETAIL_GET_SHM_MAP_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_SHM_OWNER_TOTAL: return Stage::D_DS_DETAIL_GET_SHM_OWNER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_CREATE_TOTAL: return Stage::D_DS_DETAIL_SET_CREATE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_CREATE_ATTEMPT_TOTAL: return Stage::D_DS_DETAIL_SET_CREATE_ATTEMPT_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_ROUTE_TOTAL: return Stage::D_DS_DETAIL_SET_ROUTE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_ROUTE_CONTEXT_TOTAL: return Stage::D_DS_DETAIL_SET_ROUTE_CONTEXT_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_WORKER_DELIVERY_TOTAL: return Stage::D_DS_DETAIL_GET_WORKER_DELIVERY_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_UB_WORKER_TOTAL: return Stage::D_DS_DETAIL_GET_UB_WORKER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_SHM_WORKER_TOTAL: return Stage::D_DS_DETAIL_GET_SHM_WORKER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_WORKER_MASTER_TOTAL: return Stage::D_DS_DETAIL_SET_WORKER_MASTER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_WORKER_CREATE_META_TOTAL: return Stage::D_DS_DETAIL_SET_WORKER_CREATE_META_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SHARED_REFERENCE_OBJECT_KEYS_TOTAL: return Stage::D_DS_DETAIL_SHARED_REFERENCE_OBJECT_KEYS_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SHARED_MASTER_HEAVY_OP_TOTAL: return Stage::D_DS_DETAIL_SHARED_MASTER_HEAVY_OP_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_RELEASE_TOTAL: return Stage::D_DS_DETAIL_SET_RELEASE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_RELEASE_TASK_TOTAL: return Stage::D_DS_DETAIL_SET_RELEASE_TASK_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_SHM_RELEASE_TOTAL: return Stage::D_DS_DETAIL_GET_SHM_RELEASE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_SHM_RELEASE_TASK_TOTAL: return Stage::D_DS_DETAIL_GET_SHM_RELEASE_TASK_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_GET_MASTER_TOTAL: return Stage::D_MC_DETAIL_GET_MASTER_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SET_MASTER_START_TOTAL: return Stage::D_MC_DETAIL_SET_MASTER_START_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SET_MASTER_END_TOTAL: return Stage::D_MC_DETAIL_SET_MASTER_END_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SET_MASTER_ALLOCATE_TOTAL: return Stage::D_MC_DETAIL_SET_MASTER_ALLOCATE_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ROUTE_TOTAL: return Stage::D_MC_DETAIL_SHARED_MASTER_ROUTE_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_BUFFER_TOTAL: return Stage::D_MC_DETAIL_SHARED_BUFFER_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_TE_PREPARE_TOTAL: return Stage::D_MC_DETAIL_SHARED_TE_PREPARE_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_TOTAL: return Stage::D_MC_DETAIL_SHARED_TE_SEGMENT_NAME_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_TE_SEGMENT_ID_TOTAL: return Stage::D_MC_DETAIL_SHARED_TE_SEGMENT_ID_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_TE_ENDPOINT_FIFO_TOTAL: return Stage::D_MC_DETAIL_SHARED_TE_ENDPOINT_FIFO_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_TE_ENDPOINT_SIEVE_TOTAL: return Stage::D_MC_DETAIL_SHARED_TE_ENDPOINT_SIEVE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_WORKER_OBJECT_TOTAL: return Stage::D_DS_DETAIL_GET_WORKER_OBJECT_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_API_LOCAL_TOTAL: return Stage::D_DS_DETAIL_SET_MASTER_API_LOCAL_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_API_REMOTE_TOTAL: return Stage::D_DS_DETAIL_SET_MASTER_API_REMOTE_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_TENANT: return Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_TENANT_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_METADATA: return Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_METADATA_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_PROCESSING: return Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_PROCESSING_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_REPLICATION: return Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RW_REPLICATION_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_TENANT: return Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_TENANT_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_METADATA: return Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_METADATA_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_PROCESSING: return Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_PROCESSING_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_REPLICATION: return Stage::D_MC_DETAIL_SHARED_MASTER_ACCESSOR_RO_REPLICATION_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_SHARD_RW_TOTAL: return Stage::D_MC_DETAIL_SHARED_MASTER_SHARD_RW_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_SHARD_RO_TOTAL: return Stage::D_MC_DETAIL_SHARED_MASTER_SHARD_RO_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_MASTER_LEASE_TOTAL: return Stage::D_MC_DETAIL_SHARED_MASTER_LEASE_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_RPC_WRAPPER_TOTAL: return Stage::D_DS_DETAIL_GET_RPC_WRAPPER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_TOTAL: return Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_TOTAL: return Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_TOTAL: return Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_QUERY_SHM_REQUEST_TOTAL: return Stage::D_DS_DETAIL_GET_QUERY_SHM_REQUEST_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_QUERY_UB_REQUEST_TOTAL: return Stage::D_DS_DETAIL_GET_QUERY_UB_REQUEST_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_QUERY_TCP_REQUEST_TOTAL: return Stage::D_DS_DETAIL_GET_QUERY_TCP_REQUEST_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_RPC_COROUTINE_TOTAL: return Stage::D_MC_DETAIL_SHARED_RPC_COROUTINE_TOTAL_CALLS;
        case Stage::D_MC_DETAIL_SHARED_RPC_TOTAL: return Stage::D_MC_DETAIL_SHARED_RPC_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_API_SET_TOTAL: return Stage::D_DS_DETAIL_SET_API_SET_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_API_GET_TOTAL: return Stage::D_DS_DETAIL_GET_API_GET_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_GET_API_GET_READONLY_TOTAL: return Stage::D_DS_DETAIL_GET_API_GET_READONLY_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SET_MASTER_API_OWNER_TOTAL: return Stage::D_DS_DETAIL_SET_MASTER_API_OWNER_TOTAL_CALLS;
        case Stage::D_DS_DETAIL_SHARED_MASTER_API_BUILD_TOTAL: return Stage::D_DS_DETAIL_SHARED_MASTER_API_BUILD_TOTAL_CALLS;
        default: return Stage::COUNT;
    }
}

inline uint64_t DetailSampleEvery()
{
    static const uint64_t every = [] {
        const char *enabled = std::getenv("KVTEST_DETAIL_PROFILE");
        if (!Enabled() || (enabled != nullptr && std::strcmp(enabled, "0") == 0)) return uint64_t{0};
        const char *value = std::getenv("KVTEST_DETAIL_SAMPLE_EVERY");
        if (value == nullptr) return uint64_t{64};
        char *end = nullptr;
        const auto parsed = std::strtoull(value, &end, 10);
        return end != value && *end == '\0' && parsed > 0 && parsed <= 1000000
                   ? static_cast<uint64_t>(parsed) : uint64_t{64};
    }();
    return every;
}

inline bool DetailSelected(Stage stage)
{
    const auto every = DetailSampleEvery();
    if (every == 0 || stage == Stage::COUNT) return false;
    // Sampling counters are per OS thread; no request state is kept across bthread migration.
    thread_local std::array<uint64_t, static_cast<size_t>(Stage::COUNT)> visits{};
    const auto visit = visits[static_cast<size_t>(stage)]++;
    uint64_t offset = visit / every + static_cast<size_t>(stage) * uint64_t{0x9e3779b97f4a7c15};
    offset = (offset ^ (offset >> 30)) * uint64_t{0xbf58476d1ce4e5b9};
    offset = (offset ^ (offset >> 27)) * uint64_t{0x94d049bb133111eb};
    offset ^= offset >> 31;
    return visit % every == offset % every;
}

inline void DetailCount(Stage stage)
{
    if (DetailSampleEvery() == 0) return;
    if (auto *slot = detail::GetSlot(stage)) slot->buckets[0].fetch_add(1, std::memory_order_relaxed);
}

inline Stage DetailRouteStage(Stage group, Stage phase)
{
    switch (group) {
        case Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_TOTAL:
            switch (phase) {
                case Stage::D_DS_DETAIL_SET_SDK_CREATE: return Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_CREATE;
                case Stage::D_DS_DETAIL_SET_SDK_COPY: return Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_COPY;
                case Stage::D_DS_DETAIL_SET_SDK_PUBLISH_PREPARE: return Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_PUBLISH_PREPARE;
                case Stage::D_DS_DETAIL_SET_SDK_PUBLISH: return Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_PUBLISH;
                case Stage::D_DS_DETAIL_SET_SDK_FEEDBACK_AND_CLEANUP: return Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_FEEDBACK_AND_CLEANUP;
                case Stage::D_DS_DETAIL_SET_SDK_CONTEXT: return Stage::D_DS_DETAIL_SET_SDK_UB_BUFFER_CONTEXT;
                default: return phase;
            }
        case Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_TOTAL:
            switch (phase) {
                case Stage::D_DS_DETAIL_SET_SDK_CREATE: return Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_CREATE;
                case Stage::D_DS_DETAIL_SET_SDK_COPY: return Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_COPY;
                case Stage::D_DS_DETAIL_SET_SDK_PUBLISH_PREPARE: return Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_PUBLISH_PREPARE;
                case Stage::D_DS_DETAIL_SET_SDK_PUBLISH: return Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_PUBLISH;
                case Stage::D_DS_DETAIL_SET_SDK_FEEDBACK_AND_CLEANUP: return Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_FEEDBACK_AND_CLEANUP;
                case Stage::D_DS_DETAIL_SET_SDK_CONTEXT: return Stage::D_DS_DETAIL_SET_SDK_SHM_BUFFER_CONTEXT;
                default: return phase;
            }
        case Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_TOTAL:
            switch (phase) {
                case Stage::D_DS_DETAIL_SET_SDK_CREATE: return Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_CREATE;
                case Stage::D_DS_DETAIL_SET_SDK_COPY: return Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_COPY;
                case Stage::D_DS_DETAIL_SET_SDK_PUBLISH_PREPARE: return Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_PUBLISH_PREPARE;
                case Stage::D_DS_DETAIL_SET_SDK_PUBLISH: return Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_PUBLISH;
                case Stage::D_DS_DETAIL_SET_SDK_FEEDBACK_AND_CLEANUP: return Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_FEEDBACK_AND_CLEANUP;
                case Stage::D_DS_DETAIL_SET_SDK_CONTEXT: return Stage::D_DS_DETAIL_SET_SDK_TCP_BUFFER_CONTEXT;
                default: return phase;
            }
        case Stage::D_DS_DETAIL_GET_QUERY_UB_REQUEST_TOTAL:
            switch (phase) {
                case Stage::D_DS_DETAIL_GET_QUERY_INITIALIZE: return Stage::D_DS_DETAIL_GET_QUERY_UB_REQUEST_INITIALIZE;
                case Stage::D_DS_DETAIL_GET_QUERY_RPC_WITH_RETRY: return Stage::D_DS_DETAIL_GET_QUERY_UB_REQUEST_RPC_WITH_RETRY;
                case Stage::D_DS_DETAIL_GET_QUERY_APPLY_AND_CLEANUP: return Stage::D_DS_DETAIL_GET_QUERY_UB_REQUEST_APPLY_AND_CLEANUP;
                case Stage::D_DS_DETAIL_GET_QUERY_VALIDATE: return Stage::D_DS_DETAIL_GET_QUERY_UB_REQUEST_VALIDATE;
                default: return phase;
            }
        case Stage::D_DS_DETAIL_GET_QUERY_SHM_REQUEST_TOTAL:
            switch (phase) {
                case Stage::D_DS_DETAIL_GET_QUERY_INITIALIZE: return Stage::D_DS_DETAIL_GET_QUERY_SHM_REQUEST_INITIALIZE;
                case Stage::D_DS_DETAIL_GET_QUERY_RPC_WITH_RETRY: return Stage::D_DS_DETAIL_GET_QUERY_SHM_REQUEST_RPC_WITH_RETRY;
                case Stage::D_DS_DETAIL_GET_QUERY_APPLY_AND_CLEANUP: return Stage::D_DS_DETAIL_GET_QUERY_SHM_REQUEST_APPLY_AND_CLEANUP;
                case Stage::D_DS_DETAIL_GET_QUERY_VALIDATE: return Stage::D_DS_DETAIL_GET_QUERY_SHM_REQUEST_VALIDATE;
                default: return phase;
            }
        case Stage::D_DS_DETAIL_GET_QUERY_TCP_REQUEST_TOTAL:
            switch (phase) {
                case Stage::D_DS_DETAIL_GET_QUERY_INITIALIZE: return Stage::D_DS_DETAIL_GET_QUERY_TCP_REQUEST_INITIALIZE;
                case Stage::D_DS_DETAIL_GET_QUERY_RPC_WITH_RETRY: return Stage::D_DS_DETAIL_GET_QUERY_TCP_REQUEST_RPC_WITH_RETRY;
                case Stage::D_DS_DETAIL_GET_QUERY_APPLY_AND_CLEANUP: return Stage::D_DS_DETAIL_GET_QUERY_TCP_REQUEST_APPLY_AND_CLEANUP;
                case Stage::D_DS_DETAIL_GET_QUERY_VALIDATE: return Stage::D_DS_DETAIL_GET_QUERY_TCP_REQUEST_VALIDATE;
                default: return phase;
            }
        default: return phase;
    }
}

class DetailFrame {
public:
    DetailFrame(Stage total, Stage first, uint64_t bytes = 0)
        : sampled_(DetailSelected(total)), total_(total), stage_(first), bytes_(bytes)
    {
        if (DetailSampleEvery() != 0) DetailCount(DetailCallStage(total));
        if (sampled_) {
            detail::GetRecorder();
            stages_.fill(Stage::COUNT);
            durations_.fill(0);
            start_ = checkpoint_ = detail::NowNs();
        }
    }
    DetailFrame(const DetailFrame &) = delete;
    DetailFrame &operator=(const DetailFrame &) = delete;
    ~DetailFrame() { Finish(-1); }

    void Next(Stage stage)
    {
        if (!sampled_) return;
        const auto now = detail::NowNs();
        Append(now);
        stage_ = stage;
    }

    void SetGroup(Stage total)
    {
        if (total_ == total) return;
        if (DetailSampleEvery() != 0) DetailCount(DetailCallStage(total));
        total_ = total;
    }

    void Finish(int outcome)
    {
        if (!sampled_) return;
        const auto end = detail::NowNs();
        Append(end);
        if (auto *slot = detail::GetSlot(total_)) detail::Record(*slot, end - start_, bytes_, outcome);
        for (size_t i = 0; i < size_; ++i) {
            stages_[i] = DetailRouteStage(total_, stages_[i]);
            if (auto *slot = detail::GetSlot(stages_[i])) detail::Record(*slot, durations_[i], 0, outcome);
        }
        detail::SaveDetailSample(total_, start_, end - start_, stages_, durations_, size_, overflow_, outcome);
        sampled_ = false;
    }

    static uint64_t CaptureTime(Stage stage)
    {
        if (!DetailSelected(stage)) return 0;
        detail::GetRecorder();
        return detail::NowNs();
    }
    static void RecordElapsed(Stage stage, uint64_t start)
    {
        if (start == 0) return;
        if (auto *slot = detail::GetSlot(stage)) detail::Record(*slot, detail::NowNs() - start, 0, -1);
    }

private:
    void Append(uint64_t now)
    {
        if (stage_ != Stage::COUNT) {
            if (size_ < stages_.size()) {
                stages_[size_] = stage_;
                durations_[size_++] = now - checkpoint_;
            } else {
                overflow_ = true;
            }
        }
        checkpoint_ = now;
    }
    bool sampled_, overflow_{false};
    Stage total_, stage_;
    uint64_t bytes_, start_{0}, checkpoint_{0};
    size_t size_{0};
    std::array<Stage, 32> stages_;
    std::array<uint64_t, 32> durations_;
};

template <typename Function>
decltype(auto) DetailMeasure(Stage stage, Function &&function)
{
    DetailFrame frame(stage, Stage::COUNT);
    return std::forward<Function>(function)();
}

}  // namespace kvtest_profile
#endif
