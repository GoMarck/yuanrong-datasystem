# Mooncake v0.3.11.post1：A3 multi-buffer benchmark

## 测量范围

使用 `MooncakeDistributedStore`，默认 **Ascend transport + NPU HBM buffer**：
写入源和读取目标都是 `torch.uint8` NPU tensor，按参考 vLLM-Ascend backend
选择初始化和注册路径。需要目标版本 Mooncake（启用 Ascend Direct 支持）、
PyTorch、torch_npu、匹配的 CANN/驱动及通信环境。
显式指定 `--protocol rdma/tcp` 则保留旧的 ctypes Host DRAM 模式，
不依赖 torch；Host 结果不能当作 NPU 结果。

| 项目 | 默认值 / 定义 |
|---|---|
| 模式 | `put/get` 使用 HTTP 协调；`put-get` 在一个进程中先写再读 |
| 协议 / 设备 | `ascend` / `--device-id 0` |
| 写入场景 | `put` 默认 `local`，可指定 `random` |
| API | `batch_put_from_multi_buffers`、`batch_get_into_multi_buffers` |
| 每次调用 key 数 | 写读分别设置 `--put-keys-per-batch` 和 `--get-keys-per-batch`，默认均为 8 |
| 每个 key 字节数 | 7,096,320 |
| 每个 key block 数 | 128，每 block 55,440 bytes |
| 每批有效载荷 | 56,770,560 bytes，约 54.14 MiB |
| 并发 | `get` 默认 1、4；`put` 默认 1；共享 Store、独立 buffer |
| 测量批数 | 默认共 3200 key，即 400 批，由 `--num-keys` 决定 |
| 预热 | 每线程 5 批，不计入结果 |
| 时延 | 整次 batch API 调用耗时，ms，avg/p50/p90/p99/max |
| 吞吐量 | 成功批/s、key/s、有效载荷 GB/s（十进制）和 GiB/s（二进制） |

两个 C++ API 的 Python binding 会释放 GIL；线程模式允许同步调用重叠。
Python 参数封送、调度仍然属于实际 Python 调用开销。不是跨机器总并发，
也不是固定到达率的开环压测。

### 场景语义与正确性

- `random`：不指定 preferred segment，使用 master 的默认分配策略。
  默认随机分配器可选中本节点；**不保证均匀随机，也不等于全部远端**。
  应在同一 master 下提前挂载多个节点的内存段，并确认 master 策略。
  JSON 的 `placement` 保存每个实际 endpoint 的 key 数。
- `local`：`preferred_segment = store.get_hostname()`。该版本的 preferred
  segment 可能回退到其他节点，因此在计时外检查**全部 key 的 replica endpoint**；
  任何远端落点都会报错，不输出该 case 的有效性能结果。
  本地指当前 Store 实例挂载的 segment，不是同 IP 上任意进程的 segment。
- 不把 `prefer_alloc_in_same_node=True` 当成本地约束：
  该选项主要让一批 key 跟随首个 key 的落点，并启用对应批传输路径。
  Ascend 模式默认 True，与参考 backend 相同；`random` 也可能整批落同一节点。
  `--no-prefer-alloc-in-same-node` 可关闭；Host 模式默认 False。
- 固定单副本内存存储，hard pin 默认 False，与参考 backend 默认配置相同。
  可用 `--hard-pin` 防止测量数据被淘汰；不启用时需要隔离其他负载、留足空闲空间。
  请使用专用测试集群，关闭磁盘溢写等可能改变数据路径的配置。
- `put/get` 使用指定前缀，
  读取前打乱全部 key，一轮无重复、无遗漏；不反复 put 已存在 key。
- put 逐 key 要求返回 0；get 逐 key 要求返回完整 `key_bytes`。
  最后一个读取 batch 对每个线程做完整内容比较，其余批检查返回长度。
  发生错误则失败退出，JSON `status=failed`；不把失败请求算作成功吞吐量。

## 在 A3 上运行

双节点测试命令见 `README_mooncake_http.md`：B 使用 `--http-listen-port`，
A 使用 `--http-server`。两端各自的 `--concurrency` 控制各自的读/写并发，
B 先挂载内存，A 再执行 `local/random` 写入。需要同时复制
`mooncake_bench_http.py`。测试数据参数和两个批大小都由 A 配置，通过 HTTP 交给 B。

### 单进程：每批写 8 key，每批读 460 key

`--operation put-get` 不启动 HTTP，也不等待其他进程；同一个 Store 实例完成
预热写、正式写、落点校验、预热读、正式读、内容校验和清理，然后自动退出。
无需 `mooncake_bench_http.py`。Store 所连接的 master 仍需预先启动。

```bash
ASCEND_BUFFER_POOL=4:8 python3 mooncake_multi_buffer_bench.py \
  --operation put-get \
  --local-hostname 192.168.0.231 \
  --master 192.168.0.231:50188 \
  --protocol ascend --device-id 0 \
  --scenarios local \
  --key-prefix mc_single_1 --num-keys 920 \
  --put-keys-per-batch 8 --get-keys-per-batch 460 \
  --key-bytes 7096320 --blocks-per-key 128 \
  --concurrency 1 --warmup 5 --seed 42 \
  --segment-bytes 34359738368 \
  --output mc_single_1.json
```

| 项目 | 单进程约定 |
|---|---|
| 并发 | `--concurrency` 只接受一个值，写阶段和读阶段均使用此值，默认 1 |
| 批大小 | 写/读独立；总 key 数同时被两个批大小整除，每个读写线程至少有一批 |
| 场景 | `local` 指本实例 segment；`random` 由 master 分配，可能包含远端，实际落点写入结果 |
| 读取顺序 | 按 seed 打乱完整 key 列表，每个 key 正式读取一次 |
| buffer | 写 buffer 在读 buffer 分配之前注销释放，避免同时占用两套 NPU buffer |
| 预热 | 写使用独立临时 key 并删除；读复用正式数据集，两者均不计入结果 |
| 数据保护 | 写前检查整个 key 范围，已有任一 key 就失败；不覆盖或清理原有数据 |
| 清理 | 成功或失败后清理本次已确认写入成功的 key，不删除集群其他数据 |
| 输出 | JSON 分别保存 put/get 指标、各自批大小、实际落点和清理状态 |

该示例正式写 115 批、正式读 2 批，只用于验证流程，不能用于稳定的 p99 评估。
并发 4 时至少使用 `--num-keys 1840`；需要更多样本时相应增加数据量和存储容量。
按默认 key 大小，460-key 读 buffer 每线程约 3.04 GiB，4 线程约 12.16 GiB，
另外需预留 CANN、Store、内容校验的资源。`local` 场景是本节点读，
不等同于 A 写 B 读的跨节点场景。切换 `random` 不保证一定选中远端。

先启动与 v0.3.11.post1 匹配的 Mooncake master 和存储节点。
部署参数取决于现场网络，本脚本不自动启动/修改集群。
使用已验证支持现场 Ascend 环境的 `mooncake-transfer-engine==0.3.11.post1`
安装包。有发行包元数据时，脚本检查版本必须为 `0.3.11.post1`；
源码/定制部署若没有版本元数据，但 `mooncake.store` 可导入且所需 API 存在，
会警告并继续，JSON 记录 `version=null`、`version_verified=false` 和实际模块路径。
这不表示已经确认版本兼容，需由部署方核实构建版本；不能把它记成已验证的 post1。
模块导入失败、所需 API 缺失或已知版本不匹配仍会报错。
预先设置必要的动态库路径、设备权限、
`/etc/hccn.conf` 等通信配置。`--device-id` 是当前进程可见的逻辑设备号；
主线程及每个压测线程都会绑定该设备。

### Ascend 初始化路径

与参考 backend 一样，优先检查 fabric，其次检查独立 TE 配置：

| 环境 | setup / 注册行为 |
|---|---|
| 未启用下列配置 | 创建外部 TransferEngine，以 P2PHANDSHAKE/ascend 初始化；把 `IP:TE_RPC端口` 和 engine 传给 Store；通过 TE.register_memory 注册 NPU buffer |
| `ASCEND_GLOBAL_RESOURCE_CONFIG` 非空，且未启用 fabric | Store 自行初始化，local buffer=0；通过 Store.register_buffer 注册 NPU buffer |
| `ASCEND_ENABLE_USE_FABRIC_MEM=1` | Store 自行初始化，local buffer=0；跳过显式注册，与参考 backend 相同 |

共享 TE 路径的 local buffer 默认 1 GiB，可通过 `--local-buffer-bytes` 调整；
两端都挂载存储段。独立 TE/fabric 路径始终使用 0，记录在结果 JSON 的
`effective_local_buffer_bytes`，不会把传入但未生效的值冒充实际配置。
`--local-hostname` 请提供本机可达 IP/主机名，不附带端口；共享 TE 会追加实际端口。
TE 自身的 metadata 固定 P2PHANDSHAKE，与参考 global TE 相同；
`--metadata-server` 控制 Store 的 metadata。

是否启用 fabric、HCCS 或 RoCE 取决于现场硬件和 CANN/驱动支持，不由脚本猜测。
启用 fabric 时请确认对应版本条件；脚本不会自动修改环境来启用它。
JSON `memory=npu`、`setup_path=shared_te/store_independent_te/fabric` 标明实际路径。

### Workload 与生命周期

先启动 B，看到 `STORAGE_READY` 后启动 A。A 写入并核验落点后通知 B；
B 按自己的并发列表逐项完整读取数据集，批大小使用 A 传入的 get 配置。
读取前整体打乱 key 列表，再分批分线程；线程间完成顺序不固定。
A 获取读取结果后自动清理本轮 key 并退出，B 保持存储段和 HTTP 服务存活，
可以接受下一轮。B 的 Ctrl+C/SIGTERM 会等待活动任务清理完成后才退出。
若 B 添加 `--exit-after-get`，则在本轮全部读取结束且 A 确认清理完成后自动退出，
不再接受下一轮。A 失联或清理未确认时仍会等待，避免过早关闭存储段。

| 参数/行为 | 约定 |
|---|---|
| `--num-keys` | A 配置正式数据集总数；必须同时被 put/get 批大小整除，且每个读写线程至少一批 |
| `--put-keys-per-batch` | A 配置每次写入对象数 |
| `--get-keys-per-batch` | A 配置每次读取对象数，传递给 B |
| `--warmup` | 每线程预热批数；写入用独立 UUID 临时 key，正式写入前删除；读取复用已有 key |
| `--scenarios` | A 指定一个场景；B 接收后检查实际落点 |
| 写入并发 | 一次 put 只能指定一个并发；对比并发 1/4 时分别运行并使用新前缀 |
| 前缀冲突 | 写前检查完整范围，已有任一 key 就报错，不写入也不删除旧数据 |
| 数据内容 | 固定 `0x01` 填充；最终批做完整内容检查，其余批检查返回长度，不验证错 key 但同内容的情况 |

**一个前缀同一时间只能由一个 writer 使用。** 前置存在性检查不是分布式锁，
不要并发启动同前缀 writer，也不要让其他程序修改这组 key。
发生异常时仅清理返回成功的写入；进程崩溃、RPC 结果未知或强制终止可能留下
key，请依据前缀检查处理，不能对共享集群执行全量删除。

存储有效载荷为 `num_keys × key_bytes`：默认约 **21.15 GiB**，
A/B 默认各挂载 32 GiB。默认批大小为 8 时，并发 4 的 NPU buffer 约 **216.56 MiB**，
另需 local buffer、内容比较的 Host 临时对象和 Store 自身内存。
增加样本数使用 `--num-keys`，同步增加 A 存储段/集群容量。
key 构造、打乱、预热、检查、等待及清理不计入正式接口时延。

如果部署使用 HTTP metadata，请替换 `--metadata-server`。
旧 Host 模式可显式使用 `--protocol rdma --rdma-devices <现场网卡>` 或 `--protocol tcp`；
这些模式不会创建 NPU buffer，结果 JSON 标记 `memory=host`。

## 统计口径

写进程执行预热写并清理预热 key，再正式写入、核验落点；
读进程执行预热读、正式读取、内容校验。正式数据由写进程在读取结束后清理。

时延仅覆盖 API 调用，包含 Python binding 转换和返回，不包含 key 创建、
buffer 初始化/注册、落点校验、内容比较及删除。百分位采用排序后的线性插值，
不把单批时延除以 key 数。
NPU 初始化填充完成后先同步再压测；设备绑定在各线程进入启动 barrier 前完成。
每次同步 Store API 调用中不额外插入 torch 同步；最终内容检查在计时外搬回 Host。

吞吐量使用整个并发阶段的墙钟时间：从放行线程到最后一个线程完成。
包含线程调度、循环及返回码检查，不包含线程池创建、落点校验和清理。
不能用各线程 GB/s 相加或使用 `总字节 / 平均单次时延` 代替。
读写分阶段执行，输出分别对应纯写、纯读，不是混合读写压力。

终端输出 Markdown 表格；JSON 保存参数、平台、版本、run_id、状态、
每行统计、落点分布和原始纳秒时延。真实数据必须在 A3 上运行后获取。

## 与 vLLM-Ascend MooncakeBackend 的一致性边界

核对来源：用户提供的
`vllm_ascend/distributed/kv_transfer/kv_pool/ascend_store/backend/mooncake_backend.py`。
默认 Ascend 模式已对齐 whole-key API 的 NPU 内存、设备绑定和三条 TE/注册路径。
这仍是合成 workload，不是完整 vLLM 推理负载。

| 维度 | vLLM-Ascend backend | 本工具 |
|---|---|---|
| 写/读接口 | `batch_put_from_multi_buffers` / `batch_get_into_multi_buffers` | 相同；二维地址和长度列表 |
| 协议/内存 | 只允许 ascend；使用模型提供的 NPU KVCache 地址 | 默认 ascend、NPU tensor；可显式选择旧 Host 模式 |
| 初始化 | 普通路径共享外部 TransferEngine；fabric/独立 TE 路径由 Store 初始化，local buffer 为 0 | 按相同环境变量选择上述路径 |
| 注册 | 普通路径 TE.register_memory；独立 TE 路径 store.register_buffer；fabric 路径跳过显式注册 | 相同；注销后释放，TE 晚于 Store 关闭 |
| 写入配置 | 默认 prefer_alloc_in_same_node=True；preferred_segment 由配置控制 | Ascend 默认 True，可关闭；local 场景指定并核验当前 segment |
| 驱逐策略 | ReplicateConfig 默认策略 | 默认 hard pin=False；可显式开启 |
| 读取参数 | 三个参数，使用默认 same-node=False | 显式传 False，语义一致 |
| 错误判断 | get 负值记失败，正值转换成成功码；put 记录异常 | put 必须逐 key 返回 0，get 必须返回完整长度；失败退出 |
| 其他特性 | tenant、QoS、SSD、layerwise session/range 支持 | 不覆盖这些配置接口；使用环境变量时由底层读取其通信配置，不做 vLLM extra_config 注入 |

buffer 的 block 是连续 allocation 的切片，不模拟各层 KVCache 的真实离散布局。
与 Yuanrong 对比时还需统一硬件、数据集、并发、worker/Store 数据落点及链路配置；
数据源同为 NPU 不表示底层存储/传输机制完全一致。Yuanrong 说明见
`README_yuanrong_multi_buffer.md`。

## 本地语法/参数检查

```bash
python3 -m py_compile benchmarks/storage_benchmark/mooncake_multi_buffer_bench.py
python3 benchmarks/storage_benchmark/mooncake_multi_buffer_bench.py --help
```

这些检查不证明实际 Ascend/RDMA 性能或部署可用性。原有测试文件已按要求删除。

## 全内容校验开关

在 put / put-get 命令增加 --verify-data。HTTP get 的校验开关和 seed 从 put 自动传递，
两侧脚本与 buffer_validation.py 必须一起更新（控制协议新增 verify_data 布尔字段）。
校验模式按 key/字节位置生成不同内容，每批 put 后 poison+读回验证，每批 get 前 poison、后全内容比较；
失败返回非零，JSON 记录错误。开启后不再用最后一批 0x01 检查覆盖新数据模式。
API 时延不含校验，墙钟吞吐量包含生成/读回/比较开销，JSON verification 标识该口径。
默认关闭；不能将内容验证通过等同为远端 HiXL 路径验证。
