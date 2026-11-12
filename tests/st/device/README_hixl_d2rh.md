# HeteroClient HIXL D2RH 手工 ST

本改动尚未编译或运行测试。以下用例需要在真实 Ascend/HiXL 环境执行，不启用 ACL mock。

## Release 包 Python 端到端用例（双节点）

节点 A 运行 Python SDK，节点 B 运行一个外部 Worker 即可，不需要第三台机器。
使用独立的单 Worker 集群，确保所有路由目标都在节点 B；节点 A 不启动 Worker。
脚本为同目录的 `hixl_d2rh_release_e2e.py`，单卡 SDK 验证只需复制这一个文件到节点 A，不依赖源码树、
ST 二进制、`WITH_TESTS` 或故障注入。尚未执行真实环境验证。

SDK wheel 和 Worker 都需要使用包含本功能的 release 构建，不能使用尚未包含本功能的历史发行包。
SDK 还需包含本次补充的 D2H access 日志 `transportType` 字段，否则脚本会明确失败。
两端加载已验证兼容的 CANN/HiXL/驱动和 DataSystem 动态库环境；节点 A 需要可导入的 PyACL（`acl`）。
本用例不要求 PyTorch 或 NumPy。

节点 B 在原有 Worker 启动参数上增加：

```text
--enable_hixl_d2rh true
--remote_h2d_link_type HCCS
--remote_h2d_device_ids <Worker设备列表，例如0,1,...,15>
--remote_h2d_hccs_buffer_pool <count>:<size>
--access_sample_rate 1
```

需要 CS 的环境还需 `--hixl_cs_enable true`。保留原有集群发现、认证、共享内存等基础配置。
buffer pool 使用该环境已验证的非零配置。

D2RH 默认每个 Worker 允许最多 256 MiB 的受保护在途写内存，所有 SDK 进程和 NPU 共享，
单批也受此限制，计入对象头和元数据。大批量写入可在启动 Worker 前配置，例如：

```bash
export DS_HIXL_D2RH_PROTECTED_MEMORY_MB=2048
```

也可使用 Worker 参数 `--hixl_d2rh_protected_memory_mb 2048`；环境变量优先于启动参数/JSON 配置。
值必须是正整数 MiB，不能超过 `17592186044415`，无不限额模式；非法值会导致 Worker 初始化失败。
只需更新 Worker 二进制；使用新增 dscli 参数时还需更新 CLI 的 `worker_config.json`。
配置在初始化时固定，修改后需重启 Worker；撤销环境变量和自定义参数可恢复默认值。
额度不预分配内存，也不扩大 `shared_memory_size_mb` 或 HiXL buffer pool；应结合 Worker 可用共享内存、
并发批次和异常写保留内存设定。1024 个受保护 allocation 的数量限制保持不变。
启动 INFO 会打印 `HIXL D2RH protected memory budget`；拒绝批次/在途写时会打印请求和限制字节数。

节点 A 安装本次 release wheel，清除指向源码 `python/` 的 `PYTHONPATH`，然后运行：

```bash
rtk proxy python3 -m pip install --force-reinstall /path/to/release.whl
export DS_RH2D_HCCS_BUFFER_POOL='<count>:<size>'
# 需要 CS 时设置；开启认证时同时设置 AK/SK，按需设置租户。
export DS_HIXL_CS_ENABLE=true
export DS_D2RH_TEST_ACCESS_KEY='<AK>'
export DS_D2RH_TEST_SECRET_KEY='<SK>'
export DS_D2RH_TEST_TENANT_ID='<tenant>'

rtk proxy python3 /path/to/hixl_d2rh_release_e2e.py \
  --worker-host '<节点B的Worker IP>' --worker-port <Worker端口> \
  --local-ip '<节点A可被B访问的IP>' --device-id 0 --rounds 4
```

认证关闭时不要设置 AK/SK/tenant 占位值；不需要 CS 时省略该变量。
运行机器没有 RTK 时省略命令中的 `rtk proxy` 即可。

脚本打印实际导入的 SDK 文件位置和独立日志目录。每轮生成一个新的随机 UUID key，复用
6 MiB 源/目的 arena，每个对象含两个不同长度的 Blob（2 MiB、4 MiB）。两种 Python 写接口交替使用。
arena 通过额外分配后向上取整保证 2 MiB 对齐，成功清理时释放原始分配地址。
对象必须超过 QueryMeta 的 512 KiB 内联数据上限：默认开启数据复制时，元数据所在 Worker 可随查询
直接返回小对象 payload，绑定 Worker 将其存入 SHM，绕过 RH2D。仅增加随机轮数不能避免这条路径。
每个样本都执行逐字节读回、元数据布局、NX 保留旧值、删除及可见性检查；数据或接口失败立即退出，
不通过重试掩盖错误。默认 32 个样本；`--rounds` 控制随机采样预算。

每次 API 只传一个 key，避免混合放置批次的最终 `transportType` 被误当成批内每个 key 的链路证据。
API 执行与数据验证先连续完成，随后集中读取 access 日志，最多等 60 秒；不会逐请求等待刷盘。
默认 access 批量提交周期为 30 秒，`DATASYSTEM_LOG_ASYNC_ENABLE=false` 不会取消这层缓冲。
记录按唯一 key、API 和调用顺序分别关联新写、首次读、NX 写、NX 读，要求成功码、字节数和传输字段有效。
NX 写是可能不传数据的操作，不计入 D2RH 覆盖；NX 读也不增加首次 RH2D 样本数。

默认模式至少需要 `--min-hixl-samples` 个新写使用 HiXL（默认 2），并覆盖两种 Python 写接口。
本地 SHM 或 TCP 样本可以数据验证成功，但不计入 HiXL 覆盖。预算内不足时返回非零，不能把全部回退视为通过。
报告包含：

- `verified_samples.json`：已通过字节、布局、NX、删除检查的随机样本。
- `coverage.json`：通过状态、D2RH/RH2D 样本数、同 key 双向数、两种接口覆盖、实际路径分布，
  以及各次 access 记录的 key、操作阶段、trace ID、字节数和耗时。

读回默认可以使用既有 H2D 路径，默认模式不宣称 RH2D 已覆盖。
成功时先销毁客户端再释放设备 arena。异常时不重试，也不主动释放 arena/reset/finalize，
直接非零退出；对象设置 300 秒 TTL。该脚本不覆盖真实 DMA 超时、断连或重启的 fence。

## RH2D 与 D2RH 同时开启，Worker 绑定 16 卡

Worker 使用同一个 `remote_h2d_device_ids` 列表初始化每卡 engine，两种方向共用这些 engine。
同一 SDK engine 的 RH2D 和 D2RH 使用相同 communicator UUID，Worker 将其固定映射到设备列表中的一张卡。
不同 SDK 按 UUID 的哈希分布到各卡；一个 SDK 不再随请求轮询 Worker 的多张卡，也不保证有限进程覆盖全部卡。
本次新增 MultiCreate 的 UUID 请求与确认字段，SDK wheel 和所有 Worker 必须一起更新；回退时也一起回退。
SDK 保持每进程一张卡；一个客户端设置 `enable_remote_h2d=True`，无需配置 `enable_local_cache`。
预注册时一次传入该进程的源和目的 arena。同步 `MSetD2H` 自动沿 placement 路由写入；
`MGetH2D` 自动复用既有 bound Worker 的元数据查询与 RH2D 读回流程，等价于读取时启用 local cache。
读写复用一个客户端实例，策略在构造时固定，不在请求中切换共享配置。普通 ObjectClient/KVClient 不变。
`AsyncMSetD2H` 保留既有 bound Worker 路径，不提供异步 HiXL D2RH。

先在 B 启动一个 Coordinator 和 Worker，双向验证时再在 A 增加 Worker。
启动参数均使用 `--参数 值`，不要写成 `--参数=值`。
两端使用同一 CS 设置和 buffer pool。初始化或建链失败时，按同次 PID 的首条 HiXL/HCCL 错误定位。

节点 B（替换 IP，Coordinator 已启动时仅运行 Worker 部分）：

```bash
B_IP='<Worker节点IP>'
export DS_TEST_HOST_ID='ds-test-node-b'
rtk proxy dscli start -c --coordinator_address "${B_IP}:31511"
rtk proxy dscli start -w \
  --worker_address "${B_IP}:31501" --coordinator_address "${B_IP}:31511" \
  --shared_memory_size_mb 1024 --arena_per_tenant 1 --host_id_env_name DS_TEST_HOST_ID \
  --enable_hixl_d2rh true --remote_h2d_link_type HCCS \
  --remote_h2d_device_ids "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15" \
  --remote_h2d_hccs_buffer_pool "4:8" --hixl_cs_enable false --access_sample_rate 1
```

单 Worker B 场景先使用前面的默认命令验证 D2RH 写入与读回一致性，不加 `--enable-remote-h2d`。
`enable_remote_h2d=True` 只启用 RH2D 能力，不能保证每次 MGetH2D 都走 HiXL。
当前原有 RH2D 路径需要绑定 Worker 从另一 Worker 获取对象并返回远端 host_info；
SDK 直接绑定 B 且对象也存于 B 时，B 本地命中在无共享内存场景返回 TCP payload。
因此仅“SDK 在 A、Worker 在 B”不足以验证 RH2D READ。

双向验证在节点 A 增加 Worker A，并与 Worker B 加入同一 Coordinator。SDK 绑定 Worker A，
B 保留上述 16 卡配置。A 的 Worker 至少配置一张可用 NPU；以下使用 device 0：

```bash
A_IP='<SDK及Worker A节点IP>'
B_IP='<Coordinator及Worker B节点IP>'
export DS_TEST_HOST_ID='ds-test-node-a'
rtk proxy dscli start -w \
  --worker_address "${A_IP}:31501" --coordinator_address "${B_IP}:31511" \
  --shared_memory_size_mb 1024 --arena_per_tenant 1 --host_id_env_name DS_TEST_HOST_ID \
  --enable_hixl_d2rh true --remote_h2d_link_type HCCS \
  --remote_h2d_device_ids "0" --remote_h2d_hccs_buffer_pool "4:8" \
  --hixl_cs_enable false --access_sample_rate 1

export DS_RH2D_HCCS_BUFFER_POOL='4:8'
export DS_HIXL_CS_ENABLE=false
rtk proxy python3 /path/to/hixl_d2rh_release_e2e.py \
  --worker-host "${A_IP}" --worker-port 31501 --local-ip "${A_IP}" \
  --device-id 0 --rounds 32 --min-hixl-samples 2 --enable-remote-h2d
```

两个节点的 `DS_TEST_HOST_ID` 必须不同，同一物理节点上的 Worker 使用相同值。
仅设置环境变量不够，Worker 还必须带 `--host_id_env_name DS_TEST_HOST_ID` 才会发布 host ID。
直连 SDK 在确认绑定 Worker 的 UDS 同机性后采用其 host ID；SDK 也必须能访问 Worker A 的 UDS/SHM。
双向模式启动时会输出 `ROUTING OK`。若 SDK host ID 为空或未匹配 Worker A，脚本立即报配置问题，
避免随机落到 A 的 key 被错误送入同卡 HiXL 建链。

脚本通过 `PREFERRED_META_OWNER` 和新随机 key 采样放置，不需要手动指定 key 所在 Worker。
落到 A 的样本通常走本地路径，只记录结果而不立即失败；落到远端且成功走 HiXL 的样本才计数。
双向模式要求至少两个不同 key 的新写和首次读都为 `HIXL`，且这批同 key 双向样本覆盖两种 Python 写接口。
任一方向、任一写接口覆盖不足，或者日志缺失、字节不一致，最终都失败。
不能保证有限随机预算一定命中远端；可将 `--rounds` 增至 128。若始终全本地或全 TCP，须检查拓扑和配置。
这属于顺序读写的双向覆盖验证，不证明读写同时在途或覆盖了 Worker 的全部 16 张卡。

终端会分别打印 API 耗时、数据验证结果和最终路径统计，例如：

```text
Coverage: D2RH=17 RH2D=17 same-key-bidirectional=17 / 32 samples; report=.../coverage.json
PASS randomized Python HiXL E2E; logs and coverage: ...
```

SDK Connect 日志含对端 endpoint，Worker 初始化日志含设备号及 endpoint；可以另行核对各卡覆盖。
如果节点 A 有多张 SDK 卡，将两个脚本放在同一目录，设置：

```bash
WORKER_HOST='<Worker A节点IP>' WORKER_PORT=31501 SDK_LOCAL_IP='<SDK节点本地IP>' \
  SDK_DEVICE_IDS='0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15' ROUNDS=32 MIN_HIXL_SAMPLES=2 \
  bash /path/to/run_hixl_d2rh_multidevice.sh
```

启动器每卡启动一个独立 SDK 进程，每个进程都必须达到双向覆盖要求，最后汇总退出码。
节点 A 只有一张卡时使用 `SDK_DEVICE_IDS=0`，不影响 B 的 16 卡 Worker；无需在 ACL 初始化后 fork。
单 Worker B 部署只能用 Python 默认模式验证 D2RH，不能满足启动器的双向要求。

C++ 手工回归入口为 `HeteroD2RHWithRH2DTest.DISABLED_MultiDeviceWorkerRoundTrip`，
需要 `DS_RH2D_LINK_TYPE=HCCS`、上述 SDK 本地 IP / buffer / CS 配置，运行时连接 16 卡 Worker。
该 ST 原有的单 Worker 拓扑仍需调整才能验证 RH2D READ，不能仅凭开启开关视为双向覆盖。
无需硬件的补充回归为 `HixlTransportPluginTest.SixteenWorkerDevicesShareRoundRobinEndpoints`，
检查 16 个独立 endpoint、轮询回绕和每卡 engine 释放；这些用例均尚未执行。

## 设备侧 16666 监听失败

HCCL 日志出现 `socket type[0]`、`specific port[16666] fail` 时，失败发生在设备网卡的参数面监听。
当前 SDK 同时开启 RH2D/D2RH 时共用同设备 engine；仅开启 D2RH 时单独初始化 engine。
旧版分别初始化两种 engine 时，若都使用 HiXL 默认设备端口，存在端口冲突风险。
其他使用同一卡的进程也可能占用该端口，单凭这条日志不能确认占用者。
端口配置依据：[HiXL Initialize 文档](https://www.hiascend.com/doc_center/source/en/CANNCommunityEdition/910/commlib/hixlug/llmcrv2_42_25.html)。

本版本为每个 SDK engine 显式配置 `comm_resource_config.listen_port`，数值采用该 engine 已分配的
endpoint 端口；Worker 每卡保持 26666。需重新构建、安装修正后的 SDK，Python 脚本和调用参数不变。
SDK INFO 的 `Hixl::Initialize start` 同时打印 `endpoint` 和 `deviceCommPort`，可核对每个 SDK engine
使用自己的端口。该端口属于设备侧；脚本 `--local-ip` 仍填写 SDK 主机可达 IP。
真实硬件仍需重新执行上述双向 E2E；fake-plugin 端口用例只覆盖初始化选项，不证明建链成功。

## 环境与开关

- 编译 SDK、Worker 和 ST 时启用已有 HIXL/CANN 构建支持；故障注入用例还需要 `WITH_TESTS`。
- 先部署本版本 Worker，再启用新客户端。Worker 需要 `--enable_hixl_d2rh true`、
  `--remote_h2d_link_type HCCS`、`--remote_h2d_device_ids <设备列表>` 和
  `--remote_h2d_hccs_buffer_pool <count>:<size>`（两个数都为正，取实际环境已经验证的配置）。
- SDK 进程设置 `DS_RH2D_HCCS_BUFFER_POOL=<count>:<size>`；需要 CS 的环境设置 `DS_HIXL_CS_ENABLE=true`。
  `DS_HIXL_D2RH_LOCAL_IP` 必须是 SDK 所在机器可被对端访问的本地 IP。
  不设置该变量时沿用所连接 Worker 的 IP，只适用于连接本机 Worker 的拓扑。
  上述环境配置必须在创建客户端前设置；D2RH 读取实例快照，不改写 RH2D 全局 flags。
- `DS_D2RH_TEST_WORKER=<ip>:<port>` 指向已启动的外部 Worker；`DS_TEST_DEVICE_ID` 指定 SDK 设备，默认 0。
  如需签名认证，可设置 `DS_D2RH_TEST_ACCESS_KEY`、`DS_D2RH_TEST_SECRET_KEY`。
- 远端用例要求 SDK 与所有目标 Worker 不在同一主机。推荐专用单 Worker 集群，避免键路由到本机。
  同机用例则使用本机单 Worker 集群和可用 SHM。用例会断言 `HIXL` 或 `SHM`，防止误测链路。
- ST 的 CMake 入口是 `ds_device_llt`（已有 device/*.cpp 自动收集）；Bazel 入口是
  `//tests/st/device:hetero_d2rh_test`。所有新增用例默认 `DISABLED_`。

## 执行

在上述环境设置完成后，使用环境中实际构建出的 `ds_device_llt` 路径执行：

```bash
rtk proxy <ds_device_llt路径> --gtest_also_run_disabled_tests \
  --gtest_filter='HeteroD2RHTest.*-HeteroD2RHTest.DISABLED_SameHostRoundTrip'
```

切换到本机单 Worker 后单独执行：

```bash
rtk proxy <ds_device_llt路径> --gtest_also_run_disabled_tests \
  --gtest_filter='HeteroD2RHTest.DISABLED_SameHostRoundTrip'
```

建议每个故障用例独立进程运行，失败后重新启动测试进程。可用同样的 `--gtest_filter` 单独选择表中的用例。

| 用例（HeteroD2RHTest.DISABLED_ 前缀） | 检查点 |
| --- | --- |
| ConcurrentClientRegistration | 两个客户端并发注册各自 arena，检查初始化状态隔离 |
| MultiBlobRoundTrip | 2 个对象，每个对象 2 个 4 KiB Blob；新发布确认、HIXL 路径、逐字节往返 |
| ReuseRegisteredArena | 同一个固定预注册 arena 连续写入 4 批新对象，逐批校验 |
| NxKeepsExistingValue | 首次写 a，随后 NX 写 b；旧值仍为 a，新增确认列表为空 |
| UnregisteredSourceRejected | 使用注册范围外的源地址返回 K_INVALID，键不可见 |
| AsyncBoundRoundTrip | AsyncMSetD2H 保留绑定 Worker 路径，并验证读回一致 |
| WriteFailureDoesNotPublish | 在 WRITE 提交前注入错误；不发布、不返回确认，后续写被拒绝 |
| SameHostRoundTrip | 不注册 HIXL 源内存，使用 SHM+D2H 往返 |

## 不需要真实 HiXL 的回归用例

以下用例已留在现有测试入口，尚未执行：

- `tests/python/test_hetero_client_multi_buffer.py`：扩展导入、旧 native 构造参数、新 Python 路由参数；
  可用 `rtk proxy python3 -m unittest discover -s tests/python -p test_hetero_client_multi_buffer.py`。
- `ObjRefTableTest.Hixl*`：预算提前预留、并发上限、所有权保留、部分插入 bad_alloc 回滚及预算恢复。
- `HixlTransportPluginTest.ExplicitOptionsDoNotMutateProcessFlags`、`ConnectHonorsCallerBudget`、
  `InvalidExplicitPoolDoesNotCreateEngine`：实例配置隔离、建链超时参数及非法配置拒绝。

C++ 用例使用现有 UT 二进制/对应 Bazel target 按 gtest filter 选择，不需要启动外部 Worker。
SDK 等待该实例串行锁与冷建链都使用同一剩余请求预算；引用清理单独保留最多 1 秒的既有 RPC 清理预算。

## 验证边界与补充压力场景

当前支持同步、SDK 每进程单源设备、Worker 多设备（包括 16 卡）、固定预注册 arena、非空 Blob、HiXL buffer pool。远端写不能静默降级为 TCP。
正常场景在客户端关闭后释放 arena。出现 WRITE 错误时，SDK 保留该 transport/注册直到进程退出，应用也必须
保留源 arena，停止复用并重启客户端进程；不能只重新创建 HeteroClient 后释放旧 arena。

Worker 复用 RH2D 已初始化的每卡 HiXL engine，READ 和 WRITE 共用同一轮询 endpoint 序列，
不再为 D2RH 创建第二组 engine。设备列表必须非空，且设备号在本机有效。
Worker 使用现有分配与发布流程，在现有引用表中额外保留未确认写入的 ShmUnit 强引用。
成功 WRITE 后通过 MultiPublish 的完成标志解除该保护。没有完成确认的分配即使客户端引用已释放，也保留到
Worker 进程退出；每个 Worker 允许的保护总量默认为 256 MiB、1024 个分配（包含正常在途写），
字节额度可通过上述启动配置调整。超出预算时拒绝
新分配，避免无限增长。预算在 allocator/驱逐之前预留，失败时撤回；HIXL 分配禁用聚合，避免小切片持有整块
owner 却只计少量字节。因此成功链路的批次也必须在此预算内（计入内部 metadata）。

应额外手工覆盖：WRITE 中途杀客户端、RPC 丢响应、Worker 重启/旧连接、真实超时、并发写和共享内存压力。
检查未完成对象不可见、目标内存未提前复用、达到保护预算后明确拒绝、成功写入后预算恢复。
提交前注入错误的 ST **不证明**真实 DMA 超时、设备侧停止访问、断连或进程重启的 fence 语义；这些需要结合
实际 HiXL/驱动版本确认。回滚时先停止新客户端写入并关闭 `enable_hixl_d2rh`；有未确认写时，先终止源进程，
再按实际设备停止访问保证重启目标 Worker，不要仅关闭开关就认为内存已经可安全回收。
