# Yuanrong A3 multi-buffer benchmark

`yuanrong_multi_buffer_bench.py` 参考用户提供的 vLLM-Ascend
`ascend_store/backend/yuanrong_backend.py`，测量 **NPU HBM → worker Host（put）**
与 **worker Host → NPU HBM（get）**。Mooncake 工具现在也默认使用 Ascend/NPU，
对比时仍需统一硬件、数据落点和链路配置；Mooncake 的显式 rdma/tcp 模式使用 Host buffer。
脚本复用相邻 `mooncake_multi_buffer_bench.py` 的 key 分组和统计函数；
两文件需放在同一目录，但运行 Yuanrong 不需要安装 Mooncake 或 vLLM。

## API 一致性

| 项目 | 实现 |
|---|---|
| Client | `HeteroClient(..., data_placement_policy=DataPlacementPolicy.PREFERRED_META_OWNER)`，然后 `init()` |
| put | `mset_d2h_from_multi_buffers(keys, addrs, sizes, set_param)`，成功返回 None，失败抛异常 |
| get | `mget_h2d_from_multi_buffers(keys, addrs, sizes, get_sub_timeout_ms)`，返回失败 key 列表；非空即失败 |
| 写参数 | `WriteMode.NONE_L2_CACHE_EVICT` + `ExistenceOpt.NX`，与 backend 相同 |
| 设备上下文 | 主线程和每个压测线程都设置 `torch.npu.set_device(device_id)` |
| 预注册 | put 始终注册；get 在 remote H2D + HIXL + 非 fabric 时注册；源/目的都是 SDK 本地 NPU 内存 |
| 内存存活 | 每线程独立 NPU tensor；预注册的 tensor 保持到 client 使用结束，不在不同并发 case 间释放 |
| 错误策略 | 不像服务 backend 只记录日志；测试立即报错，不输出该阶段成功吞吐量 |

默认性能模式使用 SDK 同步接口时延，不在每次调用中额外执行 torch 同步。
初始化填充在计时之外；--verify-data 的生成、poison、同步和内容检查在 API 时延计时之外，但计入阶段墙钟吞吐量。
每个 key 的 128 个 block 是一个连续 NPU allocation 中不同的子区间，
不复现真实 vLLM 各层 KVCache 的离散分布；默认性能模式固定内容为 `0x01`。

## 配置

需要在 A/B 节点安装匹配 CANN/驱动的 PyTorch、torch_npu 和
openyuanrong-datasystem。SDK 必须提供上述 multi-buffer 方法：
**旧 SDK 即使可以 import HeteroClient，也不代表支持这些新方法。**
预先启动服务端 worker，并配置合适的 Host 内存、远端传输和 fabric 环境。

直接使用 vLLM-Ascend 的 Yuanrong 配置文件：

```json
{
  "worker_addr": "192.168.1.11:31501",
  "enable_remote_h2d": false,
  "remote_h2d_transport_backend": "HIXL",
  "enable_fabric_mem": false,
  "connect_timeout_ms": 9000,
  "request_timeout_ms": 0,
  "get_sub_timeout_ms": 0,
  "enable_dev_mem_pregister": false,
  "data_placement_policy": "PREFERRED_META_OWNER",
  "local_ip": "<SDK可达IP>"
}
```

通过 `--config yuanrong.json` 或环境变量 `YR_CONFIG_PATH` 加载；
显式 CLI 参数覆盖对应 JSON 字段。不指定配置文件时，可仅用 `--worker-addr`。
支持 `--enable-remote-h2d` / `--no-enable-remote-h2d` 等布尔开关。
HIXL 且非 fabric 时，脚本在导入原生 SDK 前设置 DS_RH2D_LINK_TYPE=HCCS；
DS_RH2D_HCCS_BUFFER_POOL 默认 4:8、DS_HIXL_CS_ENABLE 默认 false，已有环境值保留。
`--local-ip` / JSON local_ip 设置 DS_HIXL_D2RH_LOCAL_IP。目标 Worker 仍须自行配置
`--enable_hixl_d2rh true --remote_h2d_link_type HCCS --remote_h2d_device_ids ...`
以及非零 buffer pool，SDK wheel 和 Worker 同步升级。put 不支持 fabric/非 HIXL。
legacy enable_dev_mem_pregister 保留兼容解析，但 false 不会关闭上述必要注册。
每线程分配一个 2MiB 对齐且注册长度向上对齐的固定 arena，一次注册后复用。
注册失败立即退出；传输异常保留客户端和 arena 到进程退出，不能原地重试。
不支持 layerwise 路径；配置文件的 `use_layerwise` 不改变本工具的 whole-key 测量。

## A 写、B 随机读

**A：写入完成后自动退出。** 下列地址和端口只是示例：

```bash
python3 bench/yuanrong_multi_buffer_bench.py \
  --operation put --key-prefix yr_a3_test --num-keys 3200 \
  --worker-addr 192.168.1.11:31501 --device-id 0 \
  --keys-per-batch 8 --key-bytes 7096320 --blocks-per-key 128 \
  --concurrency 4 --warmup 5 --output yr_put.json
```

**A 成功退出（退出码 0、JSON status=ok）后启动 B：生成相同 key，
随机打乱后分别测并发 1/4，读取完成后自动退出。**

```bash
python3 bench/yuanrong_multi_buffer_bench.py \
  --operation get --key-prefix yr_a3_test --num-keys 3200 \
  --worker-addr 192.168.1.12:31501 --device-id 0 \
  --keys-per-batch 8 --key-bytes 7096320 --blocks-per-key 128 \
  --concurrency 1 4 --seed 42 --warmup 5 --output yr_get.json
```

A/B 的 worker 必须属于同一可访问数据集的集群。B 也可按现场支持的连接方式
指定 A 的 worker；远端 H2D 则需另行确认 worker 与 NPU 链路配置。
工具支持 --data-placement-policy（同名 JSON 字段），默认 PREFERRED_META_OWNER；
可显式选择 PREFERRED_SAME_NODE / REQUIRED_SAME_NODE 对比本地路径。
默认策略按 key 哈希分组，有限固定 key 可能全落本地；同机识别须正确配置 host ID。
无需 enable_local_cache=false 或 enable_cross_node_connection=true；仅同步 multi-buffer 写支持 D2RH。
工具不提供 Mooncake 的 `local/random` placement 选项：
**连接本机 worker 不等于工具已经核验数据一定只在本机存储**。
数据落点由 SDK placement 及集群路由决定，结果标记为 `worker_managed`，
不虚构随机分布或本地约束，也不提供 replica 落点分布统计。

写入 key 是 `yr_a3_test_1` 到 `yr_a3_test_3200`。每个并发读取配置完整读取一遍，
无重复、无遗漏；seed 控制打乱列表，不控制线程完成顺序。
预热写使用独立临时前缀并清理，预热读使用已有 key。`--warmup 0` 禁用预热。

**PUT/GET 均在本次测试结束、保存 JSON 后自动退出，不需要 Ctrl+C。**
成功 PUT 的正式数据保留在 worker，JSON 标记 `put_data_retained=true`；
GET 不删除数据，可以再次读取。Yuanrong 数据由 worker 持有，不依赖
writer 客户端继续存活；但 worker 必须保持可用，数据仍可能被驱逐。
正式测试完成后，需要用 DataSystem 的 `HeteroClient.delete(keys)` 按本次前缀
生成的精确 key 列表清理（每次最多 10,000 key），不要对共享集群全量删除。
重复写性能测试应更换前缀或先清理旧数据，否则前置检查会拒绝已有 key。
写入失败时仍尝试清理本轮已尝试的 key；预热 key 无论成功失败都尝试清理。
SIGKILL/节点故障可能中断清理，留下的数据需人工核实处理。

## 正确性与性能口径

- 写前检查完整 key 范围，冲突就报错，防止 NX 跳过已有 key 导致虚高。
  **每个前缀只能有一个 writer，其他程序不可修改该范围**；存在性检查不是分布式锁。
- MSet 抛异常时可能部分写入：清理已经尝试的独占前缀 key；
  清理失败会报错，不保证网络故障后的数据一定已删除。
- 参考 backend 的 `NONE_L2_CACHE_EVICT` 允许驱逐。需要足够空闲内存、
  隔离其他负载；数据缺失导致读失败。Mooncake 当前也默认不 hard pin，
  但可通过 `--hard-pin` 开启；对比时应明确这一策略差异。
- get 的返回值是失败 key 列表，不是每 key 的字节数，不能沿用 Mooncake 返回码判断。
  默认性能模式仅检查每线程最后一批的 0x01 内容，其他批依赖 SDK 成功/失败契约；
  --verify-data 才逐批检查 key/字节模式。
- 默认 3200 key 共约 **21.15 GiB Host 数据**；每端并发 4 的 NPU buffer
  约 **216.56 MiB**，另需 SDK、CANN、内容检查时的 Host 临时内存。
- 输出整次 batch API 时延（ms）avg/p50/p90/p99/max，
  成功 batch/s、key/s、GB/s、GiB/s 以及原始纳秒样本。
  吞吐量按并发阶段墙钟计算；失败阶段不输出成功性能行。
- 这是 closed-loop、同一 NPU 上多线程、whole-key、同步 API 压测，
  不是跨卡并发、层流水线或完整 vLLM 推理性能。

## 检查

```bash
python3 bench/yuanrong_multi_buffer_bench.py --help
python3 -m py_compile bench/yuanrong_multi_buffer_bench.py
```

`--help` 和语法检查无需 torch_npu/数据系统运行环境。
实际吞吐量及 transport 可用性必须在 A3 真机验证。

## 可选全内容验证

put 和 get 两端都加 `--verify-data`，使用相同 seed、key 前缀和布局（批大小、并发可以不同）。
脚本按 seed/key/字节位置生成可重复数据；每次 put 完成后将缓冲区 poison，再读回并逐字节比较；
每次 get 前也 poison，完成后验证本批全部 key，发现错 key、错 block、尾部损坏或未拷贝立即失败。
错误包含 key/block/字节偏移，JSON status=failed，退出码非零。旧的全 0x01 数据集不能用于此模式。

```bash
python3 bench/yuanrong_multi_buffer_bench.py --operation put --worker-addr "<WorkerA_IP>:19491" \
  --local-ip "<SDK_A_IP>" --enable-remote-h2d --key-prefix check_unique --num-keys 64 \
  --key-bytes 6291456 --blocks-per-key 2 --keys-per-batch 8 --warmup 0 --verify-data --output put.json
# put 成功后，在另一节点运行；保持同一集群、seed/前缀/布局
python3 bench/yuanrong_multi_buffer_bench.py --operation get --worker-addr "<WorkerB_IP>:19491" \
  --local-ip "<SDK_B_IP>" --enable-remote-h2d --key-prefix check_unique --num-keys 64 \
  --key-bytes 6291456 --blocks-per-key 2 --keys-per-batch 8 --warmup 0 --verify-data --output get.json
```

默认不开启校验，保留 0x01 性能模式。开启后 API 时延只统计待测 put/get；阶段墙钟 GB/s 包含
数据生成、验证读回、D2H、比较与同步，不能与纯性能吞吐量直接比较。每行 JSON 的 verification
包含 enabled/verified_keys/pattern/wall_includes_validation。put 校验读回会改变缓存状态。
内容正确不等于走了 HiXL：顶层 access 的单值 transportType 会覆盖混合分组，不能用于逐 key 覆盖统计。
通过 --enable-remote-h2d 启用能力也不保证每次 get 都走 RH2D。

两节点运行器示例见 case_runner/examples/d2rh_validation；公共目录包含相邻校验模块，复制脚本时也须一起复制。

```bash
python3 -m unittest discover -s bench -p 'test_buffer_validation.py' -v
```

这些是标准库 Host fake 回归，不导入 NPU/原生 SDK，不代表真实链路和性能已验证。

校验实现按每个线程当前 batch 缓存一份期望数据，fill/poison/compare 不重复生成 SHAKE256；
NPU 按整批上传和下载，put 后在设备侧取反 poison。数据模式仍为 shake256-key-offset-v1，
旧的 --verify-data 数据集保持兼容。额外 Host 内存按并发 × batch 大小增长，不缓存整个数据集；
整批比较时还需下载缓冲区及 bytes 副本。VERIFY_PROGRESS 输出 worker/batch/stage，首批逐阶段、
后续约每 5 秒及末批完成时输出；阻塞中的原生调用返回前不会产生新的进度。
