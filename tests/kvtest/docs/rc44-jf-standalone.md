# rc44 JF 独立部署性能对照

用户已确认使用 rc44 自带 JF mock：master 上增加一个 JF Pod，coordinator_test 注册并维持心跳，worker_test 和 kvtest client 经 JF 发现 coordinator。当前两节点、8KB、105 QPS/client、线程数及模板参数保持不变，先只运行 DS。

## 实施计划

- [x] 新增仅 Python 侧的 `datasystem_deployment.mode=standalone_jf`，缺省仍是 dscli。
- [x] 复用 `deploy_jf.py`、独立 coordinator/worker 安装与启动入口，使用 DS 二进制旁的原生服务程序和 lib。
- [x] worker/coordinator 模板文本只替换原有地址值，保留参数、说明和顺序；不修改 C++ 服务发现及 Set/Get 热路径。
- [x] client 配置只使用 jf_address/jf_service，部署后确认 coordinator 已注册再启动 worker/client。
- [x] 报告记录部署模式、JF 节点/地址、注册结果、独立程序路径，并收集 JF 日志；失败保留命名空间。
- [x] 新增参数校验、命令路由、模板原样传递及报告测试，运行相关原生回归和源码守卫。

## 风险与验证边界

JF 心跳及发现线程属于已有 rc44 行为。仅改变部署与发现路径，不改写限速、读写、通知或性能打点。JF 使用命名空间内独立服务名和 Pod，不操作已有服务。
组件 env 仍通过 Pod 模板注入，HOST_IP 保持节点地址。服务注册确认发生在压测前，外部 CPU/带宽监控仍默认关闭。
本机无法进入实际 UB 集群；本地验证使用已有部署测试和模拟命令，不能宣称已获得真实性能数据。回退时删除 scenario 中的 datasystem_deployment，并将 client.config.service_discovery 恢复为 coordinator，回到默认 dscli 路径。


## JSON 开关

独立场景：`comparison/scenario.2-nodes-1clients-jf.json`。

```json
"datasystem_deployment": {
  "mode": "standalone_jf",
  "jf": {
    "node": "master",
    "port": 9999,
    "service": "kvcache_coordinator",
    "ttl_seconds": 30
  }
}
```

同时将 `components.datasystem.client.config.service_discovery` 配置为 `jf`。默认 `dscli` 场景继续使用 `coordinator`。
本轮为两节点各 1 client、8KB、105 QPS/client、16 写/16 读线程、180 秒。保持原生通知上限 10，但只有一个其他 client，因此有效读/写目标是 1。
JF Pod 固定 1 CPU/1Gi，无 UB/RDMA 设备请求；worker、client 的设备、CPU 和内存预算沿用原场景。

独立模式不安装 whl：直接上传 `kvtest` 所在包里的 `coordinator_test`、`worker_test` 和 `lib/`。
建议使用前面保存的原生 DS 包，以便与 dscli 场景使用相同 SDK。无需因本次 Python 改动重新编译 C++。

## 执行命令

在 master 已配置好 kubectl 且镜像已导入的前提下，更新本分支。以下自动选择最近保存的 `output-rc44-ds-*` 原生包；也可直接填写实际包目录。

```bash
cd /home/jkx/mooncaketest/0.9.2.rc44/yuanrong-datasystem_jkx
git pull --ff-only origin codex/kvtest-rc44-mooncake-minimal
cd tests/kvtest

kubectl get nodes

DS_PACKAGE=$(python3 - <<'PY'
from pathlib import Path
packages = sorted(
    (p for p in Path.cwd().glob('output-rc44-ds-*') if
     all((p / name).is_file() for name in ('kvtest', 'coordinator_test', 'worker_test'))),
    key=lambda p: p.stat().st_mtime, reverse=True)
if not packages:
    raise SystemExit('没有找到 output-rc44-ds-*，请先编译并保存原生 DS 包')
print(packages[0].resolve())
PY
)
```

确认上一段没有报错，再生成本地运行配置并执行：

```bash
python3 - "$DS_PACKAGE" <<'PY'
import json
import sys
from pathlib import Path
package = Path(sys.argv[1]).resolve()
for name in ('kvtest', 'worker_test', 'coordinator_test', 'lib/libdatasystem.so',
             'lib/libdatasystem_worker.so', 'lib/libdatasystem_coordinator.so'):
    if not (package / name).is_file():
        raise SystemExit('缺少产物: ' + str(package / name))
config = json.loads(Path('comparison/scenario.2-nodes-1clients-jf.json').read_text())
config.setdefault('artifacts', {})['kvtest_binaries'] = {'datasystem': str(package / 'kvtest')}
Path('comparison/scenario.rc44-jf-local.json').write_text(json.dumps(config, indent=2) + '\n')
print('使用原生 DS 包:', package)
PY

SCENARIO=comparison/scenario.rc44-jf-local.json
RUN="$PWD/results/ds-jf-8kb-105qps-$(date +%Y%m%d-%H%M%S)"

python3 -u deploy_compare_ub_rdma.py \
  --scenario "$SCENARIO" --systems datasystem --duration 180 --validate-only

python3 -u deploy_compare_ub_rdma.py \
  --scenario "$SCENARIO" --systems datasystem --duration 180 --output "$RUN"

echo "HTML报告：$RUN/report.html"
```

脚本顺序：节点/配额/原生包预检 → 创建新命名空间 → JF mock → 独立 coordinator → 确认注册 → 独立 worker → 生成 JF client 配置 → 启动前服务快照 → 原生 client 压测 → 采集/报告 → 成功清理命名空间。失败保留现场。

## 证据与验证

- `datasystem/service-discovery.json`：模式、JF地址、服务名、注册实例、原生程序包。
- `datasystem/service-startup.json`：压测前 coordinator_test / worker_test / JF 的实际进程参数。
- `datasystem/jf-logs/`：注册、发现、心跳事件。
- worker/coordinator 日志同时收集配置 log_dir 和各自 `/data/standalone/<role>/stdout.log`。
- DS 接口表仍来自 SDK access 日志；原生 `run_summary.txt` 和时序 CSV 保留。外部 CPU/带宽监控默认关闭。

本地已通过针对性部署、JF mock 真实子进程/HTTP、日志及模板测试和源码守卫。真实 master `141.62.33.73:22` 连接超时，未执行集群性能测试，也没有真实 Avg/P99 结论。
现有 SDK 在 standalone `DataWorkerOptions` 路径中使用传入的 coordinatorDiscovery（`src/datasystem/worker/data_worker.cpp`），模板中保留的 coordinator_address 不会把该测试改回静态发现。


验证命令（`tests/kvtest` 目录）：

```bash
PYTHONPATH=.:tests/python python3 -m unittest test_rc44_comparison test_rc44_jf_comparison test_deploy_client test_deploy_common test_deploy_worker test_deploy_coordinator test_deploy_jf test_mock_jf_server test_log_collect test_collect_targets test_collect_compression
python3 -m unittest discover -s tests/python -p test_rc44_source_guard.py
```

针对性集合 303 项完成、1 项跳过；Windows 的 rc44 适配/源码守卫集合 19 项通过。没有对本次未运行的真实服务 SDK 链接、集群启动或性能作通过声明。
