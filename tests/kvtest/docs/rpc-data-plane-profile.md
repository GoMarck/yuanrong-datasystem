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

目录必须预先存在且可写。每个 recorder 创建 `<prefix>.<pid>.<monotonic-ns>.log`，避免同进程不同动态库覆盖文件；收集该前缀的所有日志。内容是带 `[KVTEST_STAGE_PROFILE]` 前缀的 JSON 行；
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


## 第三层打点：一次采集 Set/Get 内部处理

两边重新使用本轮源码构建。元戎需要 SDK 和 Worker 同时更新（master 在 Worker 中）；Mooncake 需要 SDK、Master 和链接的 TE 同时更新，不能只替换 Store 而继续使用旧 TE。分支名保持原名。固定官方源码使用更新后的 `stage_profile.patch` / manifest；已包含补丁的 fork 分支按该分支正常构建，不再叠加旧补丁。

四类进程都在**启动前**设置下面的环境变量，日志目录预先创建：

```bash
export KVTEST_STAGE_PROFILE=1
export KVTEST_DETAIL_PROFILE=1
export KVTEST_DETAIL_SAMPLE_EVERY=64
export KVTEST_DETAIL_SAMPLES=1
export KVTEST_STAGE_PROFILE_OUTPUT=<existing-log-dir>/kvtest-stage-profile
```

`KVTEST_DETAIL_PROFILE=0` 单独关闭第三层；原有第一、二层保留。未设置时跟随 `KVTEST_STAGE_PROFILE=1` 开启。默认每类函数在每个线程的每64次进入中选择一次；分块内采样位置由固定散列确定，避免固定第64次总是落到交替的同一路径。不是按整条分布式请求统一采样。`KVTEST_DETAIL_SAMPLE_EVERY=1` 是全量诊断，会增加耗时，不能用来声明无打点性能。非法间隔回退64，允许范围1–1000000。

新增覆盖清单在 `tests/kvtest/detail_profile_catalog.json`，记录函数和阶段名。重点是：

| 流程 | 新增观察内容 |
|---|---|
| Set SDK | API上下文、trace、实现调用、指标/access、退出清理；路由健康排除、选worker、身份锁和guard；Create连接及分配ID；UB响应信息、接收池/owner/Buffer构造；SHM映射、FD查询、引用及owner；Publish和反馈清理 |
| Set Worker / Master | Create对象查询、已有对象锁与检查、分配预留、ShmUnit/内存分配、响应、引用；Publish到master的请求构造、owner解析、API对象构造/Init和路由调用；本地master API的签名；master路由、heavyOp锁/插入、分片锁、metadata表插入、状态校验、元数据填充/复制/序列化/存储、订阅及过期处理 |
| Get SDK | API外层及退出清理；SHM连接/会话；UB恢复检查、连接、缓冲申请/校验/临时表插入；连接entry查询、读锁、状态检查、lease分配、lease锁获取和跨RPC持有；请求构造、签名、响应健康信息和结果接管 |
| Get Worker | 对象表查询、对象读锁、状态检查、返回参数构造；UB读锁、地址准备、带宽统计、数据回写及清理；SHM引用和响应编码；响应健康信息、Write、SendPayload、完成响应 |
| 引用释放 | Set与SHM Get分别记录owner释放、入队、队列等待、任务执行和RPC；worker记录client查询、shm查询、引用更新、owner删除和清理。额外记录查找未命中次数 |
| Mooncake | PutStart/PutEnd/GetReplicaList内部阶段、对象/快照/分片锁、tenant/metadata/processing/replication查表、内存副本分配和metadata插入、lease；client内存池；RPC池获取及协程派发/响应等待；TE的segment名称/ID缓存和FIFO/SIEVE endpoint锁与查表 |
| RPC已有时间戳 | `ds.rpc_stage.<create/publish/query/release/create_meta>.*`：client准备、client结果处理、服务端请求队列、服务端响应阶段、通信剩余耗时。保留原RPC耗时定义，不将通信剩余耗时称为纯网络耗时 |

第一、二层及 `ds.rpc_stage.*` 保持全量。第三层的 `.total.calls` / `.count` 是全量事件计数，均值为0，没有耗时含义；第三层其他计时为采样。`sample_every` 在新汇总结果中标明间隔。**不能继续把采样阶段的 sum_ns 直接除以全量API次数**。先比较采样均值；同函数内阶段按该函数采样次数归一化，或使用下述同一次调用的样本汇总。共同函数标为 `shared`，不能直接按Set或Get摊销。

Set SDK按最终Buffer的SHM/UB/TCP类别分组；Get Query按重试后最终请求的传输类别分组（`shm_request/ub_request/tcp_request`），实际是否命中并传输还要核对Worker的EncodeShm/EncodeUb及SDK结果阶段。不能把“请求UB”自动认定成“实际完成UB传输”。

普通锁阶段以取得锁为边界。`lease.lock_acquire_with_guard_allocation` 同时包含锁guard的分配，不能叫纯锁等待；TBB查找/插入包括容器查询与内部同步，也不能把整个耗时叫锁等待。lease held记录跨RPC保护时间。Mooncake分片锁的held记录实际持有范围，期间的子阶段包含在其中，不能再次相加。

可选的 `[KVTEST_DETAIL_SAMPLE]` 是**同一个函数调用**内连续checkpoint，不是跨SDK/Worker关联的请求trace；不会使用thread_local保存当前请求。每个frame最多32段，最多缓存256个样本，每10秒随快照导出。缓存满或try-lock失败只丢样本并记录 `dropped_lifetime`，不丢累计阶段统计；frame溢出也显式标记。样本选择可能不代表尾延迟，不据此推断API P99。

`[KVTEST_PROFILE_RESOURCE]` 周期记录进程累计user/system CPU、上下文切换和缺页。汇总按进程去重，`average_cpu_cores` 表示观察区间内平均使用多少个CPU核，不是CPU配额利用率；没有cgroup限流和整机调度数据时，不能据此认定CPU资源不足。

测试结束应正常退出SDK并保留服务端最后一个周期快照；同时打包所有前缀日志、原access/性能结果、运行参数、源码版本以及PollJFC开关值。服务端不正常退出时缺少final尾部，汇总会提示。

```bash
python3 tests/kvtest/summarize_stage_profile.py <collected-log-directory> \
  --start-seq 1 --require-details \
  --json stages.json --csv stages.csv \
  --details-json checkpoints.json --samples-jsonl samples.jsonl
```

`stages.json` 包含计时、全量事件次数及进程资源；`checkpoints.json` 在每个函数内按耗时从高到低排列，给出每次采样调用的平均耗时、出现次数和占该函数比例。分支没执行的阶段按该函数全部采样次数摊销；重复经过同一阶段的次数单列。丢样本会提示，不能把样本文件的计数当作全部请求数。`start-seq` 对样本按导出批次筛选，不承诺与累计计数的起止时刻完全重合。

打点会增加时钟、原子统计、栈空间和周期输出开销。若需要将结果作为最终性能结论，应在同一批测试任务中保留关闭打点的基线；单独一份开启打点的数据用于定位，不足以证明无打点时的精确可优化收益。

### CPU 时间与表规模诊断

可选 `KVTEST_DETAIL_CPU=1` 为采样 checkpoint 增加 `cpu_ns`、`cpu_valid`、`begin_tid` 和 `end_tid`。
默认关闭；需要 Linux `CLOCK_THREAD_CPUTIME_ID`。跨原生线程的区间标记为无效，不能相减。
同一线程中的其他 bthread 执行也可能计入线程 CPU 时间，因此服务端有协程切换的区间不能视为该请求独占 CPU。
CPU clock 与采样元数据分配有观测开销，应分别采集关闭、普通打点、CPU 打点三组校准数据。

采样记录的 `attributes` 可包含 `table_size`（当前表元素数）、`shard_size`（当前分片/租户分片元素数）、
`bucket_count`、`shard_index`、`key_bytes`、`hit`、`item_count`。
属性只在已有保护范围内按 O(1) 读取；不会遍历表或为统计另取业务锁。
它们是采样时的近似规模，并非整条分布式请求的强一致快照。不要把 worker 槽容量、分片元素数或累计写入数当成全局存活对象数。

新增本地诊断覆盖路由选择、负载调度的槽查询/状态更新/决策记录、TCP Create/Publish，
以及 SafeTable 查找与未命中 Status 构造。正常业务返回值与同步方式保持原状。
共享头使用版本命名空间，避免同一进程加载不同版本 SDK 时 profiler 内部类型发生符号冲突。

### 固定对象规模和并发诊断

`cmake --build <kvtest-build> --target kvtest_table_probe` 构建可选诊断程序。
它复用 KVTest 的 SDK 连接配置及 Mooncake `put/get_buffer` 适配器；不修改正常 KVTest 用例。
运行：`kvtest_table_probe <client-config.json> <plan.json>`，环境变量由启动器显式设置，方式与正常 KVTest 相同。

```json
{"prefix":"tp1111","resident_keys":4096,"seed_begin":0,"seed_threads":4,
 "key_bytes":32,"value_bytes":1,"threads":[1,4,16,32],"seconds":20,"warmup_seconds":3}
```

这是独立诊断组：种子 Set 使用 `NONE_L2_CACHE_EVICT`、TTL=0 固定存活规模；Get 使用同一客户端的原生线程，
依次运行均匀随机键与单热键。正常基线的 TTL、线程模型和读写混合不受影响，二者绝对时延不能直接混用。
同一服务集群从 N 增长至 M 时沿用 prefix，设置 `seed_begin=N,resident_keys=M`，仅写新增键；
服务应从空数据状态开始，并核对采样表规模、淘汰和 API 失败数。`threads:[]` 可单独采集种子 Set。
种子 Set 输出精确 API 平均耗时/P99 和增长范围；Get 输出各阶段时间窗口、精确平均/P50/P99、失败及全量数据验证数。
API 计时不含返回 Buffer 的验证和析构，与 KVTest 主用例一致；析构引起的后台处理仍会影响下一次调用。
先关闭打点测吞吐，再独立采集细节；高吞吐时增加采样间隔并核对样本丢弃数。采样耗时不能除以全量请求数。
不要将 `resident_keys` 计划值当成已测的某张表大小：以具体表/分片属性和实际成功计数核对。

Get 默认在某阶段出现 API 失败时终止。诊断过载/重试时可显式设置 `fail_on_api_error:false`，
程序保留失败计数并继续后续阶段；平均耗时及分位数包含失败调用，报告必须同时呈现失败率。
种子 Set 失败、数据验证失败、空样本和采样容量溢出仍然终止，不能把失败样本或无效对象规模当成成功结果。

## RPC implementation isolation

`ds.rpc_native.create/publish/query/release/create_meta` bracket only the native brpc
`CallMethod`. `ds.detail.rpc.stub.*` separates controller/options, Trace, framing,
diagnostics and metrics/deadline handling. Read/Write adapter frames distinguish
protobuf copying, Trace/trailer work and native completion. Existing E2E/server-exec
marks retain their original boundaries. These native intervals still contain remote
business/data and are not pure framework CPU.

Mooncake `mc.rpc_native.*` excludes its metric updates from the library pool/send/wait
interval. Its server helper frame separates metrics from the business callback.
The `rpc_method`/byte attributes and standalone no-op experiment are documented in
`tests/perf/rpc_compare/isolated/README.md`. Keep Set/Get separate, compare full matched
frames, and report measurement-on/off controls before attributing small differences.

Generation 6 additionally records `ds.detail.rpc.server.adapter.*` for plain, send-payload and unary-socket KV calls. `dispatch` includes the invoked worker/master function and is not pure RPC framework time. UnarySocket Read/Write/attachment frames use a generated numeric method identifier, independent of its trace string. Method 4 includes the actual `DecreaseReference` RPC; its native interval is `ds.rpc_native.release`.

## UB接收内存分配诊断

保持原KVTEST开关，另设`KVTEST_ALLOC_DETAIL_BYTES=10485760`仅抽样指定大小的分配（0或未设关闭），新增`ds.alloc.detail.*`记录provider、handle、ShmUnit、Allocator、ArenaGroup、jemalloc和NUMA查询。NUMA子帧不按大小筛选，覆盖进程所有分配；Get归因以按大小筛选的Arena NUMA阶段为准。各子帧独立抽样，禁止直接相减成严格预算。

仅用于进程级诊断的`KVTEST_ALLOC_NUMA_EXPERIMENT`要求`KVTEST_STAGE_PROFILE=1`：未设或`normal`保持原逻辑；`no_warning`仅跳过NUMA查询失败WARNING；`skip_empty`使空NUMA表直接返回INVALID_NUMA_ID且成功，避免重复构造Status，非空表照常查询。显式记录实验模式，不作为生产优化或公平基线。移除变量恢复原行为。必须对照`KVTEST_ALLOC_DETAIL_BYTES=0`评估细打点扰动；接收容量、监控、调度、日志级别及SHM/UB比例不变。

`python3 tests/kvtest/tools/ub_numa_mapping_probe.py`在Linux目标上用4MB独立映射复现mock的MAP_FIXED替换：先触页并查询NUMA，替换为新memfd后查询，再触页查询。同时输出mincore驻留页数；它只验证初始化失败的触发机制，不计入API时延或模拟传输性能。
