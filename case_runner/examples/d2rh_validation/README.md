# 两节点 Yuanrong D2RH/RH2D 数据校验

预先启动同一集群中的 Worker A/B，并安装匹配的 SDK、torch_npu。修改 case.json 的 SSH 别名、Worker 地址、SDK IP；容器场景为 targets 增加 container。Worker 必须配置 enable_hixl_d2rh=true、HCCS、非零 buffer pool、可用 NPU ID。两物理节点发布不同 host ID，同节点 SDK 能访问绑定 Worker 的 UDS/SHM。

```bash
python3 case_runner/runner.py case_runner/examples/d2rh_validation --check
python3 case_runner/runner.py case_runner/examples/d2rh_validation --output case-runs
```

运行器分发整个 bench 目录，先 A put+读回校验，成功后 B get+逐批校验。前缀来自同次运行的公共远端目录名；两端 seed、布局和 --verify-data 相同。失败退出码自动使 case 失败，JSON 位于回传 artifacts；不需要修改 runner 内部。正式 key 保留，可用精确 key 列表清理。

这是内容正确性校验，不保证某一批必然走远端 HiXL；PREFERRED_META_OWNER 允许本地和远端混合。确认链路仍需单独检查路由/HiXL 证据，不能用校验通过替代链路覆盖。验证吞吐量含生成数据、读回、比较开销，不用于性能对比。
