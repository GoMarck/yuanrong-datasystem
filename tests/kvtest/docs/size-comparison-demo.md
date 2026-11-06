# 数据大小对比演示

## 已确认设计与实施计划

两节点各1worker/store、2client，17QPS/client、1写1读，依次运行4MB、6MB、8MB、16MB、32MB。每档每系统默认120秒。复用现有whl/dscli部署与原生rc44客户端，不修改SDK、读写接口、限速、线程池、通知或停止逻辑。

1. 新增 `tools/run_size_compare_demo.py`：从现有reference生成五档配置，固定4写/8读线程、随机NUMA、关闭DS local cache。两边存储池均256GiB，worker/store CPU request/limit32/32、内存272/288GiB，client CPU4/32、内存4/20GiB。仅本轮worker模板副本修改shared_memory_size_mb的value为262144，其余模板内容保持不变。MC传输参数沿用现有reference。容量校验失败时在部署前停止。
2. 新增 `kvtest_comparison_demo.py`：复用现有结果读取、进度、子进程控制及表格布局。录屏主屏与汇总HTML显示“DataSystem”和“对比系统”，分别展示读写Avg/P99和时延降低百分比。原始日志、配置、JSON证据和逐档详细报告保留真实后端名称。
3. 百分比为 `(对比系统时延 - DataSystem时延) / 对比系统时延 * 100`；负值如实保留，零基准显示空值。只从成功完成且样本有效的控制器结果取数；不把client P99做平均。两者统计边界不同：DS用SDK access日志，对比系统用KVTest接口外侧采样，完整报告保留样本覆盖率与实际读写数。
4. 聚焦验证：离线生成五档并核对负载、资源、容量；用标明为合成的临时结果检查百分比正负、零基准、失败与缺样本；检查主屏不泄漏真实对比名称、进度/中断处理复用现有机制。无需真实集群或全量测试。改动只在Python演示层，无请求热路径、共享状态或持久化协议变化。回退为不用新入口。

## 执行

在 `tests/kvtest` 下执行，控制节点与两个发压节点来自源scenario（默认master、worker02、worker03）：

```bash
python3 -u tools/run_size_compare_demo.py \
  --source-scenario comparison/scenario.2-nodes-1clients.json \
  --ds-whl "${DS_WHL:?请设置whl路径}" \
  --mc-image "${MC_IMAGE:-mooncake-store-kvtest:official-v0.3.13.post1-rdma-event}" \
  --duration 120 \
  --theme light
```

可用 `--nodes <node-a> <node-b>` 和 `--control-node <control>` 覆盖节点，`--prepare-only` 只生成配置，`--output` 指定尚不存在的结果目录，`--plain` 禁用ANSI刷新。结果入口为 `summary.html`，同时保存CSV/JSON、逐档报告与控制器日志。失败停止后续档位并保留现场；不会自动清理其他测试的命名空间。外部CPU/带宽监控保持默认关闭。

32MB档120秒计划写入量为 `4 * 17 * 120 * 32MiB = 255GiB`，低于两边各512GiB总池预算的75%。不以TTL抵扣预检容量，不承诺满载下的性能；结果为实际单轮观测。两系统串行执行，五档共10分钟压测时间，另加部署与采集耗时。
