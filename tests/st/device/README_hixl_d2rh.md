# HeteroClient HIXL D2RH 手工 ST

本改动尚未编译或运行测试。以下用例需要在真实 Ascend/HiXL 环境执行，不启用 ACL mock。

## 环境与开关

- 编译 SDK、Worker 和 ST 时启用已有 HIXL/CANN 构建支持；故障注入用例还需要 `WITH_TESTS`。
- 先部署本版本 Worker，再启用新客户端。Worker 需要 `--enable_hixl_d2rh=true`、
  `--remote_h2d_link_type=HCCS`、`--remote_h2d_device_ids=<单个设备号>` 和
  `--remote_h2d_hccs_buffer_pool=<count>:<size>`（两个数都为正，取实际环境已经验证的配置）。
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
| AsyncRejected | enableLocalCache=false 的 AsyncMSetD2H 返回 K_NOT_SUPPORTED |
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

当前只支持同步、单源设备、固定预注册源 arena、非空 Blob、HiXL buffer pool。远端写不能静默降级为 TCP。
正常场景在客户端关闭后释放 arena。出现 WRITE 错误时，SDK 保留该 transport/注册直到进程退出，应用也必须
保留源 arena，停止复用并重启客户端进程；不能只重新创建 HeteroClient 后释放旧 arena。

Worker 使用现有分配与发布流程，在现有引用表中额外保留未确认写入的 ShmUnit 强引用。
成功 WRITE 后通过 MultiPublish 的完成标志解除该保护。没有完成确认的分配即使客户端引用已释放，也保留到
Worker 进程退出；每个 Worker 允许的保护总量为 256 MiB、1024 个分配（包含正常在途写）。超出预算时拒绝
新分配，避免无限增长。预算在 allocator/驱逐之前预留，失败时撤回；HIXL 分配禁用聚合，避免小切片持有整块
owner 却只计少量字节。因此成功链路的批次也必须在此预算内（计入内部 metadata）。

应额外手工覆盖：WRITE 中途杀客户端、RPC 丢响应、Worker 重启/旧连接、真实超时、并发写和共享内存压力。
检查未完成对象不可见、目标内存未提前复用、达到保护预算后明确拒绝、成功写入后预算恢复。
提交前注入错误的 ST **不证明**真实 DMA 超时、设备侧停止访问、断连或进程重启的 fence 语义；这些需要结合
实际 HiXL/驱动版本确认。回滚时先停止新客户端写入并关闭 `enable_hixl_d2rh`；有未确认写时，先终止源进程，
再按实际设备停止访问保证重启目标 Worker，不要仅关闭开关就认为内存已经可安全回收。
