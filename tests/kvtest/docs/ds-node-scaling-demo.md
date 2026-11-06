# DataSystem 节点扩展示范

## 设计与实现计划

目标是在16～32台已配置机器上串行运行2、4、8、16、32、64、128、256、512个分组，每组一个worker与两个client。主屏统一使用“节点数”，完整部署事实保存在配置与逐档报告中。固定2MB、17QPS/client、1写1读，默认每档180秒。保留rc44读写、限速、通知、停止和传输策略。

1. 新增 `tools/run_ds_node_demo.py`：读取节点清单与制品路径，生成九档JSON，复用逻辑HOST_IP分组、严格worker/coordinator模板与whl/dscli模式。最大档检查设备、CPU、内存、Pod配额，支持只生成配置。
2. 新增 `kvtest_demo.py`：终端大屏只显示节点数、数据大小、档位/阶段进度、压测倒计时与Set/Get时延表。进度取自实际控制器输出，未知时长阶段不估算百分比。原始输出进入日志，非TTY退化为阶段事件。
3. 复用现有 `api_latency_summary.json`，生成汇总JSON/CSV/HTML。读取全client合并后的SDK access日志样本Avg/P99，保留样本数、覆盖率、实际读写数和状态；不生成演示时延或自动改动采样。
4. 大规模部署仅增加可配置的控制命令超时下限，默认行为不变；不修改C++、SDK超时或reference模板。
5. 仅做语法、配置生成、终端渲染与子进程接口的聚焦检查，不运行全量测试或冒充真实集群性能验证。失败停止后续档位并保留现场；Ctrl+C传递给控制器，保存中断状态。

风险边界：同组HOST_IP相同允许原有SHM路径。共享机器资源不能等同独立机器资源。16台机器的512档每台32worker+64client，需96份UB配额、256CPU与1152GiB内存调度请求（另加控制服务和现有负载）。原生通知启动/停止可能造成Get/Set不完全相等，汇总保留真实计数。详细报告含部署事实和样本覆盖率，主屏不展示部署分布字段。

回退：不使用新入口即可恢复原流程；不删除旧脚本、旧结果或其他命名空间。

## 执行

在 `tests/kvtest` 目录准备16或32节点清单。清单支持 `kubectl get nodes -o json` 输出、节点名称JSON数组或每行一个节点名。清单顺序决定低档位先使用哪些节点；档位不超过清单长度时每机一组，更高档位均匀分布在清单全部机器上。

```bash
kubectl get nodes -o json > comparison/demo-nodes.json

python3 -u tools/run_ds_node_demo.py \
  --nodes-file comparison/demo-nodes.json \
  --source-scenario comparison/scenario.2-nodes-1clients.json \
  --ds-whl "${DS_WHL:?请先设置whl路径}" \
  --pod-image "${DS_DEMO_IMAGE:-datasystem-kvtest:rc44-demo-v1}" \
  --duration 180
```

镜像仅作为DS容器底座，不运行Mooncake。源scenario仅复用artifacts、控制节点与UB资源名称，worker/coordinator配置使用当前reference模板。client保持4写/8读线程、随机NUMA、关闭local cache、20ms SDK超时、512MB快速传输池及既定DS环境变量。CPU request/limit为worker4/32、client2/32；worker每实例24GiB池、Pod内存request/limit28/32GiB，client4/20GiB。只修改本演示的预算，其他对比套件不变。

DS演示默认镜像名为 `datasystem-kvtest:rc44-demo-v1`。在有原镜像的节点加标签即可，不重建镜像；DS版本仍由传入whl决定：

```bash
nerdctl -n k8s.io tag \
  mooncake-store-kvtest:official-v0.3.13.post1-rdma-event \
  datasystem-kvtest:rc44-demo-v1
```

导出、分发时使用新标签。`--pod-image` 会覆盖源scenario中的pod镜像字段，避免旧标签继续生效。其他Mooncake对比入口的默认镜像不变。

容器沿用 `imagePullPolicy=Never`，运行前需将底座镜像导入所有选中节点。预检不修改设备插件、镜像、节点污点或现有工作负载；资源不足会停止执行并输出日志。

可选参数：`--control-node` 指定控制节点；`--prepare-only` 仅生成与校验配置，不访问集群；`--plain` 禁用ANSI刷新；`--theme light|dark` 选择终端强调色（默认light）；`--output` 指定一个尚不存在的结果目录。输出包含 `summary.html`、`summary.csv`、`summary.json`、`scenarios/`、`logs/` 及逐档完整报告。没有结果的档位显示空值或失败状态，已完成档位保留实测结果。原始运行日志和控制器状态保存失败原因及保留的命名空间。

## 录屏配色

两节点可先指定部分档位试跑，界面相同：

```bash
printf '%s\n' worker02 worker03 > comparison/demo-nodes.two.txt
python3 -u tools/run_ds_node_demo.py \
  --nodes-file comparison/demo-nodes.two.txt \
  --counts 2 4 \
  --source-scenario comparison/scenario.2-nodes-1clients.json \
  --ds-whl "${DS_WHL:?请先设置whl路径}" \
  --pod-image "${DS_DEMO_IMAGE:-datasystem-kvtest:rc44-demo-v1}" \
  --duration 60 \
  --theme light
```

该试跑的每节点worker/client分别为1/2、2/4；最大档每节点需要6份空闲UB配额，调度请求16CPU、72GiB内存。`--duration 60` 用于预览流程，正式取数可改为180。节点清单可含2～32个节点，指定 `--counts` 后会均匀分配所选档位；不指定时要求至少16个节点并运行九档。

16台机器上，256档每台16worker+32client，需要48份空闲UB配额、128CPU和576GiB内存调度请求；512档每台32worker+64client，需要96份空闲UB配额、256CPU和1152GiB内存调度请求。控制服务与现有负载另计，仍必须通过空闲资源预检。32台机器的512档每台需求与上述256档相同。

终端背景与正文字体颜色在MobaXterm的 Settings → Configuration → Terminal → Customize 中设置；现有SSH会话也可在会话的 Terminal settings 中覆盖。建议背景 `#F5F7FA`（245,247,250）、正文 `#1F2937`（31,41,55）、选中背景 `#DBEAFE`（219,234,254）、光标 `#1D4ED8`（29,78,216）。字体建议Consolas 18～20号，UTF-8，终端宽度至少96列，录屏推荐120列、28行以上。脚本的默认浅色强调色为深蓝 `#1D4ED8`、完成深绿 `#166534`、失败深红 `#B91C1C`；不改动终端背景。深色终端用 `--theme dark`；不支持彩色时用 `--plain`。汇总HTML使用浅色背景。

设置参考：[MobaXterm官方文档](https://mobaxterm.mobatek.net/documentation.html)。配色为此演示的建议值。

大规模入口按client数量配置 `run.command_timeout_floor_seconds`，只延长控制命令的外层等待时间；不改变SDK请求时限、压测窗口或原生停止语义。

## 2MB容量预算

每worker对应两个client，180秒计划写入量为 `2 × 17 × 180 × 2MiB ≈ 11.95GiB`，低于24GiB池的75%预算18GiB。仍复用保守容量预检，不依赖TTL抵扣容量；延长时长超过预算会被拒绝。CPU limit保留32，仅降低调度请求，并非为每实例独占32核。

脚本在本次结果的 `scenarios/worker.reference.json` 生成专用模板，只替换原模板 `shared_memory_size_mb` 的value为24576，参数数量、顺序、描述与其余value保持不变；仓库公共reference模板不修改。
