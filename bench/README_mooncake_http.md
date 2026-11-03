# Mooncake 双节点 HTTP 协调测试

先启动 B 的 get 服务，再启动 A 的 put。HTTP 只交换控制参数、状态和结果；
KV 数据仍走 Mooncake。需要把 `mooncake_multi_buffer_bench.py` 和
`mooncake_bench_http.py` 放在同一目录。

## 命令

B：使用 `local-hostname` 作为 HTTP 监听 IP，`http-listen-port` 作为端口。
必须挂载非零内存段，默认 32 GiB。读并发由 **B 自己的 `--concurrency`** 决定。

```bash
ASCEND_BUFFER_POOL=4:8 python3 mooncake_multi_buffer_bench.py \
  --operation get \
  --http-listen-port 18080 \
  --local-hostname 192.168.0.213 \
  --master 192.168.0.231:50188 \
  --protocol ascend --device-id 0 \
  --segment-bytes 34359738368 \
  --concurrency 1 4 \
  --output mc_server.json
```

B 输出 `STORAGE_READY` 后，A 启动测试。写并发由 **A 自己的 `--concurrency`** 决定。

```bash
ASCEND_BUFFER_POOL=4:8 python3 mooncake_multi_buffer_bench.py \
  --operation put \
  --http-server http://192.168.0.213:18080 \
  --local-hostname 192.168.0.231 \
  --master 192.168.0.231:50188 \
  --protocol ascend --device-id 0 \
  --scenarios random \
  --key-prefix mc_a3_random_1 --num-keys 1840 \
  --put-keys-per-batch 8 --get-keys-per-batch 460 \
  --key-bytes 7096320 --blocks-per-key 128 \
  --concurrency 1 --warmup 5 --seed 42 \
  --segment-bytes 34359738368 \
  --output mc_random_1.json
```

无需手动传递 segment 或通知 B 开始读。A 返回前自动等待读取完成并清理本轮 key。
B 保持服务存活，继续接受下一轮；不同轮建议使用不同前缀和输出文件名。
若只运行一轮，在 B 的 get 命令添加 `--exit-after-get`。
开启后会执行本轮全部 `--concurrency` 配置，并在 A 获取结果、清理数据、
发送清理完成通知后自动关闭 HTTP/Store 并退出。不会在最后一次 get 返回时
立即断开，以免 A 无法获取结果或 B 的存储段提前释放。
若本轮读取失败，仍在 A 确认清理后退出；若任务接受后 A 放弃写入并完成清理，
该轮也会结束服务。参数校验失败且未接受任务不会触发退出。
若 A 失联或清理失败，B 保留服务等待，不强行退出；该参数只允许用于 get，
默认关闭，put 命令不需要改变。
`get` 必须指定监听端口，`put` 必须指定 server 地址。
若要同一个进程先写再读，使用 `--operation put-get`，不传 HTTP 参数；
单进程示例见 `README_mooncake_multi_buffer.md`。

## 参数归属与落点

| 配置 | 来源 |
|---|---|
| 每端 IP、device-id、存储段容量、Ascend 环境 | 每端启动参数/环境 |
| 写并发 | A 的 `--concurrency`，一次写入只能指定一个值 |
| 读并发 | B 的 `--concurrency`，可指定 `1 4` |
| 每次写入 key 数 | A 的 `--put-keys-per-batch`，默认 8 |
| 每次读取 key 数 | A 的 `--get-keys-per-batch`，默认 8，通过 HTTP 传给 B |
| 前缀、key 总数、key 大小、block 数、预热、seed | A 提交，B 校验后使用；HTTP 服务模式下不需要 B 重复指定 |
| master、metadata、protocol | 两端必须一致，否则服务端拒绝本轮任务 |
| 写入场景 | 严格使用 A 的 `--scenarios local/random`；未指定时沿用 put 的默认 local，HTTP 不改变场景 |

get 禁止传入 `--num-keys`、`--put-keys-per-batch`、`--get-keys-per-batch`、
`--key-bytes`、`--blocks-per-key`、`--warmup`、`--key-prefix`、`--seed`、
`--scenarios`，即使值与默认值相同也会报错。这些参数统一由 put 配置。
原 `--keys-per-batch` 参数已删除，两端都不接受。

例如在 A 指定 `--put-keys-per-batch 8 --get-keys-per-batch 460`。
目前每批固定大小，总 key 数必须同时被两端批大小整除；A 解析参数时检查，
B 接受任务时还会校验读并发所需批数，不满足则在 A 开始写入前拒绝。
若 B 的并发为 `1 4`，可设置 A 的
`--num-keys 1840`：写入共 230 批，每种读并发各读取 4 批。
这仅适合流程验证，百分位样本很少；正式测试需要更多数据并预留足够存储容量。
按默认 key 大小，B 每个 460-key buffer 约 3.04 GiB，并发 4 约 12.16 GiB NPU 内存。
结果每行记录各自的 `keys_per_batch`；写时延对应 8 key，读时延对应 460 key，
吞吐量按各自实际批大小计算。更新此协议时请同时更新 A/B 脚本。

- `random` 不指定 preferred segment，由 master 分配。A/B 都已挂载内存，
  因此都可能承载数据；**不保证均匀或每端占一半**。如果集群还有其他段，
  也可能分配到其他节点。结果记录全部实际 endpoint 的 key 数。
- `local` 指定并核验 A 自己的 segment，B 读取整轮数据都是远端读取。
- B 按 seed 打乱完整 key 列表，再分批分线程，每个并发配置完整读一遍。
  JSON 的 `local_keys` 是落在 B 自身 segment 的 key 数，`remote_keys` 是其余数量。
  这是读取前的元数据快照，不是网卡流量或实际链路字节计数。
- 本节点指当前 Store 实例的 segment，不等于同 IP 的其他进程 segment。
- 默认 Ascend 的 `prefer_alloc_in_same_node=True` 会让同一批 key 尽量共置；
  不表示客户端本地约束。可在 A 指定 `--no-prefer-alloc-in-same-node`。

## 顺序与结果

1. B 完成 Store 初始化、挂载内存后才开放 HTTP。
2. A 获取 B 的 segment，提交并保留一个 run；B 同时只允许一轮。
3. A 检查前缀冲突，预热、写入、检查实际落点。
4. A 通知 B；B 再检查数据及落点，执行预热、正式 get 和最终批内容校验。
5. A 查询结果，汇总写/读性能，清理自己的 key，再通知 B 本轮结束。

接口时延和吞吐量统计不包含 HTTP、轮询、落点检查及清理。
A 的 `--output` 保存本轮写读汇总、run_id、B 配置摘要及清理状态。
B 每轮结果保存为 `<output文件名主干>.<run_id>.json`；
B 正常退出时还保存服务历史。历史最多保留 64 轮，满后需空闲时重启服务。

## 失败和安全边界

- HTTP 单次请求超时由 `--http-timeout` 控制（默认 10 秒），不改变 Ascend 传输超时。
- 创建和开始读取的请求可幂等重试，不重复启动 workload。
- HTTP 断开不等于 B 已停止读取。进入读取协调后，A 会继续查询并保留 Store，
  **不会因为网络超时立即清理或退出**；无法确认状态时可能持续等待。
- B 明确失败且读取线程已结束，A 才能清理。清理失败或通知失败会写入 A 的报告，
  B 可能保持忙碌；需先核实读线程结束和数据清理状态再处理，不要强行复用。
- B 收到 Ctrl+C/SIGTERM 后停止接受新任务，等待活动任务收到清理完成通知后退出。
  若 A 崩溃，B 可能一直等待；本工具不提供故障自动接管或强行中断底层调用。
- 不要在读取时强制杀死 A/B：两端都可能持有数据段。
- 同一前缀只允许一个 writer 使用；存在性检查不是分布式锁。
- 这是可信私网测试服务，不是多租户公共服务。可以在两端设置相同的
  `MC_BENCH_HTTP_TOKEN` 环境变量启用 Bearer 校验；不要把 token 写入可提交配置。
  HTTP 不加密，仍应使用私网和防火墙限制来源。未配置 token 时会打印警告。
- 没有额外的可配置资源配额。启动前自行确认 NPU buffer 和 Store 的容量：
  B 每个并发配置需要 `并发 × 每批key数 × key字节数` 的 NPU buffer，
  存储有效载荷为 `num_keys × key_bytes`，随机分布下每端需求不固定。
- HTTP body 保留 64 KiB 的协议边界，非法布局、重复占用、读取中清理会被拒绝。
- HTTP 协调不能修复底层 Ascend/HCCL 建链问题；真实 A3 通信仍需现场验证。

## 全内容校验开关

在 put / put-get 命令增加 --verify-data。HTTP get 的校验开关和 seed 从 put 自动传递，
两侧脚本与 buffer_validation.py 必须一起更新（控制协议新增 verify_data 布尔字段）。
校验模式按 key/字节位置生成不同内容，每批 put 后 poison+读回验证，每批 get 前 poison、后全内容比较；
失败返回非零，JSON 记录错误。开启后不再用最后一批 0x01 检查覆盖新数据模式。
API 时延不含校验，墙钟吞吐量包含生成/读回/比较开销，JSON verification 标识该口径。
默认关闭；不能将内容验证通过等同为远端 HiXL 路径验证。

校验实现按每个线程当前 batch 缓存一份期望数据，fill/poison/compare 不重复生成 SHAKE256；
NPU 按整批上传和下载，put 后在设备侧取反 poison。数据模式仍为 shake256-key-offset-v1，
旧的 --verify-data 数据集保持兼容。额外 Host 内存按并发 × batch 大小增长，不缓存整个数据集；
整批比较时还需下载缓冲区及 bytes 副本。VERIFY_PROGRESS 输出 worker/batch/stage，首批逐阶段、
后续约每 5 秒及末批完成时输出；阻塞中的原生调用返回前不会产生新的进度。
