# RPC 与数据面阶段对比

本功能用于 rc44 KVTest 与官方 Mooncake `v0.3.13.post1`（`719735896c86b56fabec6cf3e825fb2ea640597a`）的
单 key Set/Put、GetBuffer 对比。两侧使用相同聚合器、纳秒累计值及直方图边界，不改变 RPC 协议、重试或数据路径。
它是基准诊断开关，默认关闭。现有端到端 API CSV、access 日志继续用于检查成功率和完整延迟分布。

## 统计边界

| 阶段 | DataSystem | Mooncake | 包含什么 |
|---|---|---|---|
| API | `ds.api.set/get` | `mc.api.put/get` | SDK 单次调用；Get 为 Buffer 接口，不含 KVTest 校验和调用方释放 |
| 控制 RPC | `ds.rpc.create/publish/query/get` | `mc.rpc.put_start/put_end/query` | 同步往返，含框架、等待和服务端工作；DS 的 get 包含 Get / GetObjectRemote |
| 服务端执行 | `ds.server.*` | `mc.server.*` | DS 复用返回的 BRPC ServerExecStart→End；MC 为 WrappedMasterService 方法体 |
| RPC 执行以外开销 | `ds.rpc_overhead.*` | 通过匹配窗口的客户端/服务端聚合比较 | DS 对每次带完整服务端时间戳的 RPC 计算 e2e−server_exec；含队列、编解码、收发、唤醒，不能视为纯网络 |
| 内存准备 | `ds.ub.alloc`、`ds.shm.*` | `mc.alloc.put/get` | UB 池分配，SHM 映射/结果构造，MC 池分配 |
| 本地拷贝 | `ds.copy` | `mc.copy` | 应用数据到发送缓冲；MC 为 put 内 gather 调用，不含 TE |
| 数据面完整调用 | `ds.ub.write` | `mc.te.read/write` | 准备、提交、完成等待；DS blocking=false 时只测到提交返回 |
| 数据面提交 | `ds.ub.submit` | `mc.te.submit` | DS UrmaWriteImpl，包括 lane/事件/分块提交；MC allocateBatchID→submitTransfer→状态对象构造 |
| 数据面等待 | `ds.ub.wait` | `mc.te.wait` | DS 每事件 WaitToFinish；MC Client::TransferData 的 future.get；完成可能在进入 wait 前发生 |
| 其他路径 | `ds.rpc.release/create_meta/query_meta` 及对应 server/overhead | `mc.local_copy` | DS 异步引用释放、Worker→Master；MC 本地 memcpy 策略的提交，不能当作 RDMA |

`ds.shm.write_map/read_map` 是 routed SHM 的 MmapWriteRegion/BuildResult；
`ds.shm.lookup` 是两种 SDK 路径共用的 LookupUnitsAndMmapFds，命中缓存也计时；
`ds.shm.fetch_map` 仅实际缺失 FD 时的接收及 mmap。它们存在嵌套，不可相加。
SHM GetBuffer 交还共享内存视图，没有与 RDMA Read 对等的网络传输；应观察引用/映射与 RPC。

RPC 窗口：DS 为生成桩的 BRPC ClientStart→End（CallMethod 与响应附件处理），MC 为 invoke_rpc 内
syncAwait→返回（包含池内等待、协程发送和响应解码）。调用方的签名/请求构造不保证包含在这两个窗口，
因此还要看 API 总耗时与 RPC 以外的剩余耗时，不能把二者称为完全相同的函数入口计时。

DS 的 server/overhead 由调用方根据返回时间戳记录，source 列表示观测进程。
正常 KVTest 客户端有 `*.api.*` 记录，解析器将其归为 client，其他进程归为 service；
`--group-by instance` 可保留每个进程，避免把 SDK Set 写与 Worker Get 回写混在一起。

## 构建

DataSystem 使用当前分支正常重新构建 SDK 和 Worker。聚合器为 header-only：CMake 在 metrics 源清单登记头文件，沿用现有线程链接；
Bazel 补充 `benchmark_profile` 头文件目标及 mmap/buffer/URMA 的直接依赖。

Mooncake 修改位于其 `mooncake-store/src/{real_client,master_client,rpc_service,client_service,transfer_task}.cpp`
与 `mooncake-store/include/benchmark_profile.h`。KVTest 目录同时保存完整补丁和内容校验清单，便于远端重建。

当前 KVTest 镜像构建流程可用：

```bash
MOONCAKE_SOURCE_PROFILE=official-v0.3.13.post1 \
MOONCAKE_STAGE_PROFILE=ON MOONCAKE_USE_UB=OFF MOONCAKE_USE_TENT=OFF \
bash tests/kvtest/tools/mooncake/build_kvtest_store_image.sh \
  <base-image> <profile-image-tag> 8
```

`MOONCAKE_STAGE_PROFILE=ON` 是构建开关，脚本只接受原始固定提交或内容逐文件匹配的完整埋点补丁。
重复应用不修改已有埋点，遇到额外源码改动、部分补丁、不同提交或暂存区改动会报错。使用自己的 Mooncake
构建入口时，可先运行 `python3 tests/kvtest/tools/mooncake/stage_profile.py <mooncake-source>`，然后正常构建。
已应用补丁的源码无需再次运行 prepare_source.sh；该下载脚本仍会保护脏工作区。
镜像默认沿用上游 TE/RDMA 支持；运行时仍需选择 `rdma`，Master RPC 日志应保持 `protocol=tcp`。

重新导出 Mooncake C++ SDK 后，必须重新链接 KVTest。埋点属于静态库/原生模块实现，单独替换头文件不会生效。
本功能没有验证两侧完整 Linux 构建或 UB/RDMA 实机行为；下面的聚合器测试不能替代这些验证。

## 启用、采集及关闭

在四类进程启动前都设置 `KVTEST_STAGE_PROFILE=1`：DS SDK 客户端、DS Worker、Mooncake SDK 客户端、Mooncake Master。
有独立 Store 进程时也设置它。使用 deploy_compare_ub_rdma 的
`components.datasystem.{client,worker}.env` 与 `components.mooncake.{client,master,store}.env`，或现有 Worker `--env` / Mooncake `--env-json` 传入。

建议把输出前缀放在已有日志采集目录：

```json
{
  "KVTEST_STAGE_PROFILE": "1",
  "KVTEST_STAGE_PROFILE_OUTPUT": "<existing-log-dir>/kvtest-stage-profile"
}
```

目录必须预先存在且可写。每进程创建 `<prefix>.<pid>.log`，内容是带 `[KVTEST_STAGE_PROFILE]` 前缀的 JSON 行；
没有设置输出前缀时写 stderr。不要让不同进程/容器共享同一个实际文件路径。文件无滚动，限于受控时长基准使用。
原有 `*.log` 采集包含这些文件。文件打开失败会在 stderr 诊断，解析器无记录时会报错，不会填零。

仅保留阶段聚合所需环境，关闭 `DATASYSTEM_PERF_TRACE_LOG` 及逐请求 INFO 诊断，双方使用一致日志强度。
先运行一组关闭埋点的基线，再开启埋点检查观察开销；固定 CPU 配额、亲和性、并发、对象长度、路由、主副本与轮询模式。
先用 1B/1KB 观察控制面，再用 256KB/1MB 观察数据面。保持每组成功率 100%，保留失败组用于错误诊断。

每 10 秒输出累计值，正常进程退出补 final 快照；SIGKILL、部分 SIGTERM 或崩溃可能没有 final，解析器会提示尾段缺失。
四类进程均保留至少两个稳定期 tick。启动时可能有聚合器初始化开销，可通过 `--start-seq 1` 排除第一个 tick。
这按各进程开始记录后的 tick 定义，不能代替严格对齐的跨进程时间窗口。异步释放须等待排空后再比较计数。

```bash
python3 tests/kvtest/summarize_stage_profile.py <collected-client-logs> <collected-service-logs> \
  --start-seq 1 --group-by instance --json stage-profile.json --csv stage-profile.csv
```

同一数据可通过默认 `--group-by role` 合并；均使用总耗时/总次数加权，不平均进程的均值。
不要混合不同对象大小、不同运行或不同路由的目录。同一快照多次采集会去重，冲突内容会报错。
`--start-seq` 后仍保留的 max 标为 lifetime max，因为累计最大值不能做减法。

运行时关闭：删除 `KVTEST_STAGE_PROFILE` 或设置为 `0` 后重启进程。关闭时不初始化 reporter，也不读取阶段时钟。
移除 Mooncake 构建补丁：先运行 `stage_profile.py <source> --action remove`，再以 `MOONCAKE_STAGE_PROFILE=OFF` 重建。
移除前同样校验完整内容，不覆盖额外用户修改。

## 判断差异

1. 先核对端到端 API CSV 的成功率及均值，检查本次阶段记录是否覆盖稳定期。phase 缺失表示未观测到，不是 0us。
2. Set：比较 Create+Publish 与 PutStart+PutEnd 的总耗时/成功 API 次数，再比较拷贝、池分配、客户端 UB 写与 TE 写。
   只有确认无重试、无混合 API、无额外客户端且计数一致时，这种按 API 次数归一化的均值拆分才有意义。
3. Get：比较 RPC 往返及服务端执行、DS Worker UB 写、MC TE Read。DS 的 UB 写可能在 QueryAndGet/Get RPC 内，不能重复相加。
4. DS RPC overhead 大而 server 小，优先查框架调度、编解码、连接与唤醒；server 大则继续查元数据/引用处理和嵌套传输。
   MC 的 client−server 只能用隔离基准的匹配计数/窗口作均值估计，没有新增逐请求时间戳回传。
5. UB/TE 完整调用大而 submit 小，结合 wait、原有 CQE/睡眠指标判断完成路径；wait 只是调用方的剩余等待，不是 NIC 传输时延。

所有分位数是统一直方图的桶上界，溢出桶显示 overflow、JSON/CSV 分位数留空；均值使用精确纳秒累计值换算为 us。分位数不能相加或相减。
周期快照逐原子读取，在请求更新时可能存在小幅跨字段偏差；静止后的 final 快照更适合核对完整计数。
`failed` 为显式标记失败，`unknown` 为 RAII 退出但未标记结果，两者均包含在总次数、耗时和分位数中；
unknown 不表示成功，必须结合 API 成功率。`requested_bytes` 是标注阶段的请求字节量，不是已完成流量或线上 RPC 包长。
本功能没有请求级关联，因此能定位均值与各阶段分布的差距，不能证明某个 API 的 P99 恰由哪一条内部跨度造成。

## 第二层打点：Set

保持 `KVTEST_STAGE_PROFILE=1`，无需额外的细分开关。SDK 和 Worker 都需要新构建产物。

| 观测位置 | phase | 边界 |
|---|---|---|
| SDK API | `ds.sdk.set.context/impl/access` | 请求上下文准备 / ObjectClientImpl 调用 / metrics、access记录与退出清理 |
| SDK Put | `ds.sdk.set.admission/execute/postprocess` | shutdown读锁、输入和UB准入检查 / Set流程 / 延迟汇总及退出处理 |
| SDK 路由 | `ds.sdk.set.route` | 选Worker及构造SetRouteContext |
| SDK transport | `ds.sdk.set.create/publish/cleanup` | Create与本地缓冲准备 / transport Set（含UB写和Publish）/ 反馈与buffer析构、释放入队 |
| SDK RPC | `ds.sdk.create.*`、`ds.sdk.publish.*` | `prepare` 准备；`sign` 签名；`call` RPC桩调用；`result` 响应处理 |
| Worker Create | `ds.worker.create.auth/lookup/reserve/alloc/response/reference` | 鉴权与路由验证 / 命名空间和对象表查询及已有对象读锁 / allocation ID保留 / ShmUnit及内存分配 / 响应与URMA信息 / 引用登记与退出清理 |
| Worker Publish | `ds.worker.publish.auth/validate/lock/prepare/metadata/commit/postprocess/cleanup` | 鉴权 / 引用检查和构造参数 / 对象表保留及写锁 / 发布准备 / 请求Master（可走本地捷径）/ 数据保存、通知和状态提交 / 发布后处理与解锁 / 引用清理及返回处理 |
| Mooncake SDK | `mc.sdk.put.prepare/client_put/result` | 参数检查和切片准备 / Client::Put（嵌套RPC和TE）/ 结果处理与临时对象析构；原alloc、copy保留 |

Set先看 `context + impl + access` 与API总量是否接近，再下钻 `impl`；
`publish` 包含UB写和Publish RPC，不与两者再次相加。`create` 包含Create RPC。

## 第二层打点：Get

| 观测位置 | phase | 边界 |
|---|---|---|
| SDK API | `ds.sdk.get.context/impl/access/return` | 上下文 / ObjectClientImpl调用 / metrics与access / Buffer交还或ReadOnlyBuffer构造；末段包含退出清理 |
| SDK Get | `ds.sdk.get.input/execute/result` | 输入、就绪和UB准入检查 / routed或bound Get / 输出Buffer转换及延迟汇总 |
| SDK routed读 | `ds.sdk.get.route/transport/materialize` | 构造读请求和路由 / transport Get / 结果应用及物化 |
| SDK Query | `ds.sdk.get.query_prepare/ub_prepare/query_attempt_prepare/query_attempt/query_retry/query_apply` | inline传输准备 / UB准入、连接和接收池准备 / 每次请求构造 / 发起Query / 失败重试准备 / 响应解析及引用接管 |
| SDK RPC | `ds.sdk.query.prepare/sign/call/result` | 前置检查与选项 / 签名 / QueryAndGet RPC / 响应和UB健康反馈 |
| Worker Query | `ds.worker.query.auth/local/metadata/delivery` | 读取鉴权 / 本地结果构造（包含下层回写）/ miss元数据 / Write、SendPayload、完成响应 |
| Worker 本地取数 | `ds.worker.query.acquire/encode_shm` | TryAcquireLocalObject / SHM引用登记及响应编码 |
| Worker UB回写 | `ds.worker.query.ub_latch/ub_prepare/ub_write/ub_result` | 容量、准入和读锁检查 / 段地址及带宽跟踪准备 / UrmaWritePayload（含完成等待）/ 状态、带宽及退出处理 |
| Mooncake SDK | `mc.sdk.get.prepare/query_call/client_get/result` | 检查、副本选择与切片构造 / Client::Query / Client::Get / 结果和退出；原alloc、TE和RPC保留 |

`ds.worker.query.local` 包含UB或SHM下层阶段；`ds.worker.query.ub_write` 包含 `ds.ub.write`。
Get服务端回写可能处于Query RPC内，禁止重复求和。Mooncake的 `client_get` 包含TE Read，
`query_call` 包含控制面Query。磁盘等非内存分支不属于本轮对比，不保证同样细分。

`Span::Next` 结束前一阶段并启动下一阶段；没有显式标记结果的阶段仍为unknown，不推断成功。
同一函数内的checkpoint阶段不重叠，跨函数层级仍会嵌套。`prepare` 等阶段可能一次请求出现多段，
重试也会增加次数；不能直接相加各阶段均值。按 **阶段sum_ns / 对应API count / 1000**
归一化后再核对同层时间，同时检查失败、重试、阶段计数和窗口。观测本身增加时钟/原子开销，
checkpoint之间的记录开销及部分函数间隙没有归入子阶段，不能宣称精确逐请求时间守恒。

## PollJFC sleep 对照开关

SDK和Worker都支持启动环境变量：

```bash
# 默认保持现有空轮询sleep；不配置也等价于true
export DATASYSTEM_URMA_POLL_SLEEP_ENABLED=true

# 对照组：空轮询不sleep，立即继续poll
export DATASYSTEM_URMA_POLL_SLEEP_ENABLED=false
```

该值在UrmaManager初始化、启动PollJFC线程前读取一次，使用现有bool环境解析器；非法值回退true。
修改后重启对应进程。SDK和Worker可独立设置，不会互相下发覆盖。仅影响非event mode的
`PollJfcWait` 空轮询分支；event mode的等待、CQE检查、错误处理、通知和退出逻辑保持原状。
false在空闲时也持续轮询，会增加每个URMA进程的CPU消耗；它是受控性能实验开关。
恢复true或删除变量并重启即可回滚，与阶段打点是否开启无关。

在compare场景中分别修改 `components.datasystem.client.env` 和
`components.datasystem.worker.env`，不要只在本地部署shell设置。保留其余env项，例如：

```json
"env": {
  "KVTEST_STAGE_PROFILE": "1",
  "KVTEST_STAGE_PROFILE_OUTPUT": "<existing-log-dir>/kvtest-stage-profile",
  "DATASYSTEM_URMA_POLL_SLEEP_ENABLED": "false"
}
```

| 组别 | SDK | Worker | 用途 |
|---|---|---|---|
| baseline | true | true | 复现现有路径 |
| sdk-no-sleep | false | true | 定位SDK发起UB写的完成路径 |
| worker-no-sleep | true | false | 定位Worker Get回写的完成路径 |
| both-no-sleep | false | false | 观察两侧合并效果及资源竞争 |

四组固定相同构建、CPU配额/亲和性、QPS、并发、对象大小、路由和日志强度。Set、Get分别统计，
并同时收集CPU利用率/限流和API失败率。每组独立目录，先重复baseline确认波动，再交错重复对照。

新增完成路径阶段：

- `ds.ub.poll_sleep`：有在途事件时的实际nanosleep调用（不记录空闲sleep），每次sleep一条；
  关闭sleep时该阶段应无新增记录，缺失不能冒充测得0us，须结合有效配置判断。
- `ds.ub.notify`：完成集合非空时的CheckAndNotify批次，一批可能含多次请求。
- `ds.ub.wait_event/wait_result`：WaitForUrmaEvent调用 / 事件状态处理与退出；包含于原 `ds.ub.wait`。
- `ds.ub.completion_observe/wake_sched/event_process`：复用UrmaEvent已记录的us值，转ns累计，
  只在非timeout路径记录。分别为通知到调用方观测、有效唤醒调度、顺序等待中的事件处理与等待残余；
  预完成/分块路径可能是0或复用前一事件的有效唤醒值，不表示独立、不重叠的硬件阶段。

优先比较SDK/Worker各自的 `ds.ub.wait` 与上述阶段，再看Set/Get API是否同步改善。
若sleep消失但API没有改善，检查RPC、其他等待与busy poll的CPU竞争；不直接认定CQE或网卡为根因。

直接构建已提交埋点的Mooncake分支时正常编译即可，不要再向它运行 `stage_profile.py`；
该工具仅面向原始固定提交加完整bundle补丁的镜像构建路径。

## 最小验证

```bash
cmake -S tests/kvtest/tests/cxx -B /tmp/kvtest-stage-profile-tests
cmake --build /tmp/kvtest-stage-profile-tests --target kvtest_stage_profile_tests
ctest --test-dir /tmp/kvtest-stage-profile-tests -R '^stage_profile_' --output-on-failure
python3 -m unittest discover -s tests/kvtest/tests/python -p test_stage_profile.py
```

验证聚合器关闭路径、并发计数/直方图、重复 Finish、累计快照去重、加权均值、warmup 差分与 Mooncake 补丁保护。
完整 SDK/Worker/Master 的编译、成功请求计数匹配、UB/SHM/RDMA 路径及观测开销，需要部署环境验证。
