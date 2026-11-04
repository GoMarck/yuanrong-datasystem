# rc44 基线与 Mooncake 最小接入

基线为主仓 `v0.9.2.rc44`，提交 `2ff37e2f7c5fb45f3feea7c3fb969b8f84b920d6`。原有分支保持可回退；本分支不修改 DataSystem SDK、worker、coordinator 或其他产品源码。

## 已确认的移植边界

- 保留原有 DataSystem 操作、ExecutePipeline、指标 Record、cache 模式、限速、通知和停止语义。
- Mooncake 仅在启动时绑定独立的 `put` / `get_buffer` 操作回调，复用 rc44 的写循环、通知队列和 ExecutePipeline，不把 DS 操作重定向到 Mooncake。
- 默认构建仍是 DataSystem；Mooncake 编译为显式可选能力。需要基准复核时使用不链接 Mooncake 的原生 DS 构建，避免把动态依赖差异归因于代码逻辑。
- DS 统计读取收集后的 SDK access 日志，保留原有打点边界；MC 在 SDK 调用外计时并异步保存原始样本。报告标明来源与样本覆盖率，不平均 client P99 来冒充全局 P99。
- 不移植后续 cache 增强、原始 DS CSV 打点、写窗口截止、readiness RPC、低 QPS 调度修复或停止流程改写。
- Python 对比入口负责计时、部署、归档和报告；通知不足仅报告实测比例，不强制补足。外部监控默认关闭。

## 验证与风险

通过源码基线守卫检查 DS 关键函数/文件未改变，运行 rc44 原有 C++ 测试和新增 Mooncake 回调、失败、校验、样本导出测试，再验证 JSON 配置生成与日志解析。不会以本地 mock 测试宣称真实 UB/RDMA 性能。

MC 回调拥有 SDK client，执行器仅持有启动期设置的非拥有指针；生命周期覆盖写线程和通知线程停止。采样队列有界，溢出或写盘失败会写入诊断证据，不能将缺失样本解释为完整统计。DataSystem Record 不增加锁、分支、分配或 IO。

rc44 原有低 QPS 线程分配和停止丢弃待处理通知行为保留，读写比例可能不足；不将这些基线行为在本分支悄悄修复。SDK access 日志采样、额外调用或缺失记录会影响统计覆盖率，必须在报告中显示。配置中的 SDK/whl 版本、服务启动参数、缓存与 NUMA 设置仍需与旧环境对齐。

对比脚本与随附场景默认设置 `components.datasystem.client.config.enable_local_cache=false`，DS Get 按 metadata owner 走 Transport 读路径；显式设为 `true` 可恢复绑定 Worker 的读路径。DS 与 Mooncake client 默认使用 KVTest 原有的随机 NUMA：`client_numa_node` 省略或为 `null` 时生成 `random_numa_node=true`；非负整数（例如 `0`）仍固定到指定 NUMA。已有自定义 JSON 中显式的 `true` 或固定 NUMA 值保持生效，需要自行更新；不修改 worker/store 的 NUMA 策略，也不改动 C++ 读写、通知或统计逻辑。

## 实施检查表

- [x] 从主仓 tag 创建并仅推送私仓分支。
- [x] 验证未修改基线的 C++ 构建与 CUDA CTest。
- [x] 接入仅启动期选择的 Mooncake 操作及可选构建。
- [x] 移植 Python 编排，改为外部计时并读取 DS SDK 日志。
- [x] 保留原生文档/测试并补充基线守卫与运行指南。
- [x] 完成本地编译测试、语法与配置检查，记录基线既有失败。
- [ ] 在 Linux 使用真实 SDK 完整链接，并进行 UB/RDMA 集群性能复核（本机缺少环境）。

## 构建与运行

在 Linux master 的新分支目录操作。已有的 DS SDK/whl、服务镜像和 Mooncake SDK 导出目录继续由使用者提供，不能用本地 mock 测试替代。
比较旧性能时，DS SDK、whl 和 worker/coordinator 服务版本应相互匹配，并记录其提交；只恢复工具代码不能证明服务版本造成的差异。

原生 DS 构建默认仍支持 Bazel：`bash build.sh -b bazel -M on`。需要与 Mooncake 共用外置 DS SDK 时，下面的 CMake 步骤保存两份二进制。
Mooncake 组合构建需要 C++20 和动态 C++ runtime；原生 DS 构建保留 rc44 的 C++17、静态 gflags/stdlib 链接选项。
请自行填写前两行实际目录：

```bash
cd tests/kvtest
DS_SDK=/absolute/path/to/datasystem/cpp
SDK_OUTPUT=/absolute/path/to/mooncake-output-v0.3.13.post1
DS_PACKAGE="output-rc44-ds-$(date +%Y%m%d-%H%M%S)"
MC_PACKAGE="output-rc44-mc-$(date +%Y%m%d-%H%M%S)"

bash build.sh -b cmake -s "$DS_SDK" -c -j 16
cp -a output "$DS_PACKAGE"

bash build.sh -b cmake -s "$DS_SDK" --with-mooncake --mooncake-output "$SDK_OUTPUT" -c -j 16
cp -a output "$MC_PACKAGE"
```

`--with-mooncake` 需要已导出的官方 SDK；其构建与导出步骤见 [Mooncake 构建指南](mooncake-build-for-kvtest.md)。
新增系统库依赖仍包括 `ibverbs`、`numa` 和 `uring` 等，缺失时 CMake 直接说明缺失库。

可在 scenario 的 `artifacts` 中配置两个包的二进制，旧的单一路径 `kvtest_binary` 仍兼容。以下命令生成独立场景，保留原文件：

```bash
python3 - "$DS_PACKAGE" "$MC_PACKAGE" <<'PY'
import json
import sys
from pathlib import Path

source = Path('comparison/scenario.2-nodes-1clients.json')
scenario = json.loads(source.read_text())
scenario.setdefault('artifacts', {})['kvtest_binaries'] = {
    'datasystem': str(Path(sys.argv[1]) / 'kvtest'),
    'mooncake': str(Path(sys.argv[2]) / 'kvtest'),
}
Path('comparison/scenario.rc44-local.json').write_text(json.dumps(scenario, indent=2) + '\n')
PY

SCENARIO=comparison/scenario.rc44-local.json
RUN="$PWD/results/compare-rc44-$(date +%Y%m%d-%H%M%S)"
python3 -u deploy_compare_ub_rdma.py \
  --scenario "$SCENARIO" \
  --systems both \
  --duration 180 \
  --mc-image mooncake-store-kvtest:official-v0.3.13.post1-rdma-event \
  --output "$RUN"
```

模板、DS 镜像与 whl 继续按原 scenario 配置；可以使用 `--ds-image`、`--ds-whl` 明确指定。运行前加 `--validate-only` 可检查实际规模。
示例为两节点、每节点一 client，配置通知上限 10，但原生 KVTest 排除自身并对 peer 去重，因此**实际每次写只通知另一 client 读一次**。
本分支不把两 client 自动扩展成十次读取。

Python 先完成原生部署，再计时，达到运行时长后使用原生 stop（允许 60 秒退出）。C++ 不增加写窗口、读写屏障或排空状态。
各实例启动存在时间差，实际写入数不保证等于 QPS×duration；原生启动期间通知丢失、停止丢队列均可能存在，报告显示实际读写比例。

外部 CPU/带宽监控默认关闭，Mooncake SDK 自带 metrics 日志由组件 env 保留。常规对比不执行压力期间的逐 Pod procfs 快照。
组件分布、Pod 资源、生成配置与执行命令均归档；没有实际进程快照时报告明确提示，不能把生成配置冒充实际 /proc 参数。

## 输出与计时口径

- `report.html` / `report.md`：本轮数据、部署、配置及统计边界。
- `datasystem/api_latency_summary.*`：SDK 原生 `DS_KV_CLIENT_SET/GET` access 日志中的微秒耗时。
- `mooncake/api_latency_summary.*`：独立操作层的 `put/get_buffer` SDK 外侧微秒耗时。
- `<system>/latency-source.json`：每 client 实际操作数、样本数、来源文件和覆盖率。DS 可能因 SDK 采样少于操作数，不填充样本。
- MC 的 `metrics_*/api_latency_samples.status.txt`：JSON 格式的导出计数和丢样/IO 状态。缺失、丢样、IO 失败或摘要不匹配会报错。
- `metrics.json` 和原生 `run_summary.txt` 仍是 rc44 原有的外侧操作统计，与 DS access 日志汇总分别保留。

DS SDK 原生 access 日志必须开启；worker 模板的 access 采样开关不等同于 client SDK。日志关闭或完全缺失时，脚本明确报错，不切换到另一个计时口径。
全局分位数基于合并后的真实样本计算，不能以 client P99 平均值代替。时序曲线仍可用 `python3 plot_latency_time.py --help` 查看原生 CSV 用法。

## 本地验证记录与限制

- WSL/Linux 构建：18 个 CTest（17 CUDA + 1 MC 独立管线测试可执行程序）通过；MC 可执行程序覆盖原生执行器复用、失败短路、数据校验及配置拒绝 DS 别名。
- 针对修改过的部署与日志路径，285 项 Python 测试运行完成（1 项跳过），另有 9 项 Windows 适配/源码守卫测试通过（部分用例与前述集合重叠）。
- Windows 下源码守卫通过：DS 热路径关键文件完全一致，worker/notify 构造函数以后的代码完全一致，SDK/worker/coordinator 产品源码没有改动。
- 原生 C++ 全量用例有 `StopDrainsTasks` 失败（252 通过、1 失败），未修改 tag 在同机也复现；为保留 rc44 语义没有修改该逻辑或测试。
- 原生 Python 全量用例有 33 条既有失败，未修改 tag 与适配分支失败集合相同。WSL 不能直接读取 Windows linked-worktree 的 Git 路径，源码守卫需在 Windows Git 环境执行，其他 Python 用例在 Linux 执行。
- 已用真实仓库头文件完成 main、worker、notify 和 HTTP 控制面的 C++17 语法检查；未在本机完成带真实 Mooncake SDK 的链接和真实 UB/RDMA 集群运行，本机没有对应 SDK/网卡/集群。

## 回退

旧分支 `codex/mooncake-kvtest-adapter` 保持原样。新分支只承载本次 rc44 适配；切回旧分支后使用对应的旧二进制包和 scenario。
不需要在主仓创建或推送任何分支。


本地验证命令（在 `tests/kvtest` 目录）：

```bash
cmake -S tests/cxx -B /tmp/kvtest-rc44-cxx
cmake --build /tmp/kvtest-rc44-cxx -j8
ctest --test-dir /tmp/kvtest-rc44-cxx -E native_kvtest --output-on-failure
PYTHONPATH=.:tests/python python3 -m unittest test_rc44_comparison test_deploy_client test_deploy_common test_deploy_worker test_deploy_coordinator test_log_collect test_collect_targets test_collect_compression
python3 -m unittest discover -s tests/python -p test_rc44_source_guard.py
```

源码守卫需要当前 Git checkout 可被所用 Python 的 Git 命令读取；Windows linked worktree 可在 Windows 执行这一项。
`ctest -R native_kvtest` 可单独复现上述原生全量测试失败；保留该失败用于基线核对，没有删除或改写原测试。


## JF 独立部署对照

DS 新增可选 Python 部署模式 `datasystem_deployment.mode=standalone_jf`，使用原有 rc44 独立程序和 JF mock。默认 dscli 模式保留，C++ 核心不变。配置与完整命令见 [JF 独立部署性能对照](rc44-jf-standalone.md)。
