# RH2D最佳实践

远端主机到设备数据传输（Remote Host to Device，RH2D）是一种基于昇腾（Ascend）NPU（Neural Processing Unit）的，支持从远端节点主机侧共享内存到设备侧 HBM 内存的跨节点数据传输机制。通过 NPU 驱动及 CANN 工具包的支持，RH2D 提供了一个高效的数据传输通道，支持异构计算单元间的高效协同，支持大规模的并行计算、AI 训练等高性能计算任务。

openYuanrong datasystem 支持 RH2D over P2P-Transfer RoCE（RDMA over Converged Ethernet）和 RH2D over HIXL。P2P-Transfer RoCE 是默认链路，适用于配置了 RoCE 网络的跨节点场景；HIXL 未配置协议环境变量时在 Atlas A2 上默认使用 RoCE、在 Atlas A3 上默认使用 HCCS，也可通过环境变量强制使用 RoCE。两种链路对上层 MGetH2D/MSetD2H API 透明，均由 Client 主动将远端 Worker 共享内存中的数据读取到本地 NPU HBM。

## 链路选择与约束

| 项目 | RH2D over P2P-Transfer RoCE | RH2D over HIXL |
| --- | --- | --- |
| 适用环境 | 配置并打通 RoCE 网络的 Ascend 节点 | HIXL 可用且 HCCS 或 RoCE 可达的 Atlas A2/A3 环境 |
| Worker链路参数 | `--remote_h2d_link_type "ROCE"`，默认值 | `--remote_h2d_link_type "HCCS"` |
| Client链路参数 | `DS_RH2D_LINK_TYPE=ROCE`，默认值 | `DS_RH2D_LINK_TYPE=HCCS` |
| Worker Host内存 | 注册到 NPU，需要锁页 | HIXL buffer-pool relay 模式不注册、不锁页；RoCE direct 模式需要注册；FabricMem 模式通过 HIXL `MallocMem(MEM_HOST)` 分配预申请物理内存的共享内存 arena，并注册为 HIXL `MEM_HOST` |
| 传输后端及依赖 | P2P-Transfer及RoCE运行环境 | HIXL及HCCS/RoCE运行环境；需要CANN HIXL头文件、`libcann_hixl.so`、`libmetadef.so` |

使用时需满足以下约束：

1. 同一个 RH2D 通信链路两端必须使用相同的 `remote_h2d_link_type`；HIXL 场景的 buffer-pool 参数、`HCCL_INTRA_ROCE_ENABLE` 和 FabricMem 开关等内存模式配置也应保持一致。
2. 链路类型是进程级配置，必须在 Worker 启动前或 Client 第一次 RH2D 操作前设置，运行期间不能动态切换。
3. Client 进程仅使用一个 NPU device id。Worker 可以配置多个 device id；FabricMem 按对象内存所属 engine 返回 endpoint，其他 HIXL 模式按 Client communicator 固定选择 engine，原有 P2P-Transfer 后端按连接分配设备。
4. HCCS 场景要求构建产物包含 HIXL 支持，且使用的 `cann_hixl` 需支持 `comm_resource_config.listen_port`。构建时未发现 HIXL 头文件、`libcann_hixl.so` 或 `libmetadef.so`，将无法使用 `HCCS` 链路。
5. HCCS 使用 Worker 地址以及 Client 所连接本地 Worker 的地址作为 HIXL endpoint IP。请配置 HCCS 环境中可达的实际 IP，避免使用 `127.0.0.1` 或 `0.0.0.0`。
6. HIXL 在未设置 `HCCL_INTRA_ROCE_ENABLE` 时由平台选择默认协议：Atlas A2 使用 RoCE，Atlas A3 使用 HCCS；设置为 `1` 时 HIXL 使用 RoCE 链路。`remote_h2d_hccs_buffer_pool`/`DS_RH2D_HCCS_BUFFER_POOL` 为 `0:0` 时使用 RoCE direct，配置正数值时使用 RoCE buffer-pool relay；设置 `remote_h2d_hccs_enable_fabric_mem`/`DS_RH2D_HCCS_ENABLE_FABRIC_MEM` 时进入 FabricMem。buffer-pool relay、RoCE direct 和 FabricMem 三种模式应按进程启动前配置固定下来，不要混用；buffer-pool 参数仅在未启用 FabricMem 时生效。
7. HIXL 默认使用 `TransferSync()`。FabricMem 通过 `DS_HIXL_TRANSFER_SYNC_CHUNK_COUNT` 控制每个任务的对象数（默认 1），通过 `DS_HIXL_TRANSFER_SYNC_THREAD_NUM` 控制共享同步线程池大小（默认 4）。Get 的多个远端 endpoint 分组受 `DS_HIXL_REMOTE_GROUP_PARALLELISM` 限制；增加 worker engine 数量不等于所有 engine 同时满载。
8. FabricMem 依赖 CANN HIXL/ADXL 头文件提供 `OPTION_ENABLE_USE_FABRIC_MEM`、`AdxlEngine::MallocMem` 和 `AdxlEngine::FreeMem` 等接口，并依赖 `libllm_datadist.so` 提供 ADXL 实现。构建环境缺少这些接口或库时仍可使用普通 HCCS buffer-pool 模式，但运行时不能启用 FabricMem。

## Client 无本地 Worker 的 Pipeline H2D

Client-direct Pipeline H2D 允许 Client 所在节点不部署本地 Worker。Client 复用普通 Get 的元数据查询和
副本选择流程，直接连接对象所在 Worker；Client 侧使用 fast transport 内存池作为接收内存，并在初始化时
一次性完成 Host 内存注册，避免在每次 `MGetH2D` 中重复注册。

C++ 配置示例：

```cpp
ConnectOptions options;
options.host = "192.168.1.10";
options.port = 18481;
options.deviceId = "0";
options.fastTransportMemSize = 1024ULL * 1024 * 1024;
options.enableClientDirectPipelineH2D = true;
options.clientDirectPipelineH2DThreadNum = 64;

KVClient client(options);
RETURN_IF_NOT_OK(client.Init());
```

配置与运行规则：

1. `enableClientDirectPipelineH2D` 默认值为 `false`；仅在显式开启时初始化 Client-direct Pipeline H2D 资源。
2. `clientDirectPipelineH2DThreadNum` 默认值为 `64`，仅在上述开关开启时生效；有效范围为 `[8, 128]`，
   配置超出该范围时使用默认值 `64`。
3. 如果 Client 可连接本地 Worker，即使开启 Client-direct 能力，也优先使用原有本地 Worker RH2D 路径。
4. 对象大小小于等于默认分片长度 2 MiB 时，不启动 MLCacheDirect，直接通过一次 URMA write 写入 Client
   接收内存并执行 H2D；大于 2 MiB 时使用 MLCacheDirect 分片传输。
5. 大对象的 MLCacheDirect receiver 启动失败时，自动回退到一次 URMA write 后执行 H2D。
6. 请求超时、失败 key 和副本重试语义与普通 Get 保持一致。

## 源码编译安装

源码编译安装前请确保编译环境中在正常编译依赖的基础上具备如下软件依赖：

|软件名称|版本|作用|
|-|-|-|
|Ascend-hdk-npu-driver|25.5.0+；26.0.0+（HIXL HCCS FabricMem）|Ascend驱动依赖|
|Ascend-cann-toolkit|8.2.RC1+（P2P-Transfer RoCE）；8.5.2+（HIXL HCCS）；9.0.0+（HIXL HCCS FabricMem）|Ascend工具包依赖|

### 下载源码

```bash
git clone https://gitcode.com/openeuler/yuanrong-datasystem.git
```

### 编译

RH2D需要异构能力的编译。默认配置下数据系统会启用异构能力的编译，需要编译环境中具备 CANN 依赖。

```bash
bash build.sh
```

构建 HIXL HCCS 能力时，CANN 安装目录中必须包含 `include/hixl/hixl.h`、`include/hixl/hixl_types.h`、`lib64/libcann_hixl.so` 和 `lib64/libmetadef.so`。缺少任一依赖时，构建产物不会包含 HCCS transport，只能使用默认 P2P-Transfer RoCE 链路。FabricMem 模式还要求 CANN 9.0.0+ 和 HDK 26.0.0+，并要求 CANN HIXL/ADXL 头文件提供 `OPTION_ENABLE_USE_FABRIC_MEM`、`AdxlEngine::MallocMem` 和 `AdxlEngine::FreeMem` 等接口，同时要求 `lib64/libllm_datadist.so` 提供 ADXL 实现；否则 HCCS transport 可以编译，但启用 FabricMem 会在 FabricMem arena 初始化阶段返回错误。

> 注意：RH2D over HIXL HCCS 的 Worker 侧会通过 HIXL `OPTION_GLOBAL_RESOURCE_CONFIG` 设置 `comm_resource_config.listen_port`，因此仅满足 CANN toolkit 版本要求并不代表在复杂场景可用。已知 CANN 9.0.0 自带的 `cann_hixl` 仍不支持该配置项；新版 HIXL FabricMem 可能默认启用 `aicpu_unfold` 路径，请确保运行时 HIXL 包包含对应实现。使用 HCCS/FabricMem 链路前，建议手动编译并安装支持这些能力的新版本 HIXL，并确保编译期和运行期加载的都是升级后的 `libcann_hixl.so`、`libllm_datadist.so` 等库。以 aarch64 run 包为例：
>
> ```bash
> bash cann-hixl-9.2.0_linux-aarch64.run --upgrade --quiet --pylocal
> ```
>
> 安装后重新执行 `bash build.sh`，让构建过程重新探测新版本 HIXL。若在容器中运行，也需要挂载升级后的 CANN/HIXL 安装目录，避免运行期仍加载旧版本 `libcann_hixl.so` 或缺少 FabricMem 所需的 HIXL/ADXL 实现库。

编译成功后，会在output目录下产生如下编译产物：

```text
output/
├── openyuanrong_datasystem-x.x.x-cp311-cp311-manylinux_2_34_x86_64.whl
└── yr-datasystem-vx.x.x.tar.gz
```

### 安装

```bash
pip install output/openyuanrong_datasystem-*.whl --force-reinstall
```

## 部署指南

部署前，请确保满足以下必要条件：
1. 节点准备：须至少在 2 个 已配备 NPU 硬件并安装相应驱动、固件和工具包的节点上部署服务端组件。
2. 集群依赖：openYuanrong datasystem 的集群管理功能依赖于 ETCD，因此需预先搭建并确保一个稳定可用的 ETCD 集群。

ETCD部署命令样例如下：

```bash
# etcd启动指令
etcd --name etcd-single --data-dir /root/.datasystem/etcd-data \
     --listen-client-urls http://0.0.0.0:2345 --advertise-client-urls http://0.0.0.0:2345 \
     --listen-peer-urls http://0.0.0.0:2346 --initial-advertise-peer-urls http://0.0.0.0:2346 \
     --initial-cluster etcd-single=http://0.0.0.0:2346 > /dev/null 2>&1 &
# etcd状态检查
etcdctl --endpoints "http://0.0.0.0:2345" endpoint health
```

::::{tab-set}

:::{tab-item}  进程部署

openYuanrong datasystem 进程部署主要通过 dscli 工具，在使用前请确保在两个节点中已安装 openYuanrong datasystem wheel 包。

> **容器内进程部署注意事项**
>
> 🔔 **重要提示**：在拉起容器时，请挂载如下宿主机目录，确保容器内可以使用驱动依赖及NPU相关二进制：
> | 宿主机路径 | 容器挂载路径 |
> |--|--|
> | /usr/local/Ascend/driver | /usr/local/Ascend/driver |
> | /usr/local/bin/npu-smi | /usr/local/bin/npu-smi |
> | /usr/local/Ascend/driver/tools/hccn_tool | /usr/local/bin/hccn_tool |
> | /etc/hccn.conf | /etc/hccn.conf |
> | /etc/ascend_install.info | /etc/ascend_install.info |
>
>  并配置设备（需要根据实际情况配置，例如如果设备上没有NPU 7，那么/dev/davinci7不存在），确保容器内可以使用NPU设备：
> | 设备样例 |
> |--|
> | /dev/davinci0 |
> | /dev/davinci1 |
> | /dev/davinci2 |
> | /dev/davinci3 |
> | /dev/davinci4 |
> | /dev/davinci5 |
> | /dev/davinci6 |
> | /dev/davinci7 |
> | /dev/davinci_manager |
> | /dev/devmm_svm |
> | /dev/hisi_hdc |
>
> 以及注意针对大页配置和锁定内存需要**特权容器**配置。
>
> **容器启动样例命令如下**：
>
> ```bash
> docker run -itd -u root --ipc host --net host --privileged=true \
>   --device=/dev/davinci0 --device=/dev/davinci1 --device=/dev/davinci2 \
>   --device=/dev/davinci3 --device=/dev/davinci4 --device=/dev/davinci5 \
>   --device=/dev/davinci6 --device=/dev/davinci7 --device=/dev/davinci_manager \
>   --device=/dev/devmm_svm --device=/dev/hisi_hdc \
>   -v /usr/local/Ascend/driver:/usr/local/Ascend/driver \
>   -v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi \
>   -v /usr/local/Ascend/driver/tools/hccn_tool:/usr/local/bin/hccn_tool \
>   -v /etc/hccn.conf:/etc/hccn.conf \
>   -v /etc/ascend_install.info:/etc/ascend_install.info \
>   --name "<docker name>" <image_name> /bin/bash
> ```

分别在两个节点执行如下命令。非空的 `remote_h2d_device_ids` 会启用 Worker 侧 RH2D。

P2P-Transfer RoCE 链路使用以下命令：

```bash
# 根据共享内存大小提升最大锁定内存大小
ulimit -l unlimited
# 启动datasystem worker
dscli start \
    -w \
    --worker_address "${node_address}" \
    --etcd_address "${etcd_address}" \
    --arena_per_tenant 1 \
    --shared_memory_size_mb 51200 \
    --remote_h2d_device_ids "0" \
    --remote_h2d_link_type "ROCE"
```

HIXL HCCS 链路使用以下命令：

```bash
dscli start \
    -w \
    --worker_address "${node_address}" \
    --etcd_address "${etcd_address}" \
    --arena_per_tenant 1 \
    --shared_memory_size_mb 51200 \
    --remote_h2d_device_ids "0" \
    --remote_h2d_link_type "HCCS" \
    --remote_h2d_hccs_buffer_pool "4:8" \
    --hixl_cs_enable false
```

如果启用 HIXL FabricMem 模式，Worker 侧使用以下命令。FabricMem 与 HIXL buffer-pool relay 互斥，开启后 `remote_h2d_hccs_buffer_pool` 不生效。

```bash
dscli start \
    -w \
    --worker_address "${node_address}" \
    --etcd_address "${etcd_address}" \
    --arena_per_tenant 1 \
    --remote_h2d_device_ids "0,4,8,12" \
    --remote_h2d_link_type "HCCS" \
    --remote_h2d_hccs_enable_fabric_mem true \
    --sc_regular_socket_num 0 \
    --sc_stream_socket_num 0 \
    --oc_metadata_header false \
    --enable_shm_decrease_ref_queue false \
    --enable_pipeline_h2d false
```

Worker 参数说明：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `worker_address` | - | 当前节点通信地址，格式为 `IP:Port`，例如：`192.168.0.1:31501`。HCCS 场景中的 IP 还会作为 HIXL endpoint IP |
| `etcd_address` | - | ETCD 集群访问地址列表，多个 `IP:Port` 使用逗号分隔 |
| `arena_per_tenant` | - | 不应超过系统内存资源限制，避免初始化失败，初始建议值为 `1`。FabricMem 模式下会按 `remote_h2d_device_ids` 的数量自动调整 memory arena 数量 |
| `shared_memory_size_mb` | `1024` | Worker 可用于缓存数据的共享内存上限，单位为 MB；RH2D 场景推荐值为 `51200`，普通场景可使用默认值。FabricMem 模式下单个 arena 大小必须 1GB 对齐；多 arena 模式还必须能被 arena 数量整除 |
| `remote_h2d_device_ids` | 空 | 非空时启用 Worker RH2D；多个有效且不重复的 device id 使用逗号分隔，例如 `0,4,8,12`。FabricMem 始终按完整列表放置 arena；默认只在第一个设备创建 engine，开启多 engine 后每个设备创建一个 engine |
| `remote_h2d_link_type` | `ROCE` | 支持 `ROCE`、`HCCS`，区分大小写 |
| `remote_h2d_hccs_buffer_pool` | `0:0` | HIXL buffer-pool 参数，格式为两个正整数 `<count>:<size>`；默认关闭，配置正数值（例如 `4:8`）时启用 |
| `hixl_cs_enable` | `False` | 是否由 Worker 通过 HIXL `LocalCommRes` version 1.3 启用 HIXL CS；仅 HCCS 链路使用。启用后支持同节点跨进程、同 NPU 卡建链，并利用 CS 针对小数据传输的专项优化降低通信时延 |
| `remote_h2d_hccs_enable_fabric_mem` | `false` | HIXL FabricMem 模式开关，仅 HCCS 使用。设置为 `true` 时不使用 HIXL buffer-pool relay |
| `remote_h2d_hccs_fabric_mem_multi_engine` | `false` | Worker 启动开关，要求 HCCS、FabricMem 和非空设备列表。开启后每块 arena 只注册到其所属设备的 engine，对象读写使用该 engine 的 endpoint。关闭时所有 arena 注册到唯一 engine。不能动态切换 |
| `sc_regular_socket_num` | `16` | FabricMem 模式必须设置为 `0`，关闭 StreamCache regular ShmCircularQueue |
| `sc_stream_socket_num` | `16` | FabricMem 模式必须设置为 `0`，关闭 StreamCache stream ShmCircularQueue |
| `oc_metadata_header` | `true` | FabricMem 模式必须设置为 `false`，避免在 FabricMem arena 内写入传统 mmap metadata header |
| `enable_shm_decrease_ref_queue` | `true` | FabricMem 模式必须设置为 `false`，关闭基于共享内存 ref count 的 decrease-reference queue |
| `enable_pipeline_h2d` | `false` | FabricMem 模式必须保持为 `false`，避免启用传统 Pipeline H2D 共享内存队列 |


:::

::::

### Client配置

Client 必须通过 API 参数启用 RH2D。Python `HeteroClient` 和 `DsTensorClient` 使用 `enable_remote_h2d=True`，C++ Client 使用 `ConnectOptions.enableRemoteH2D=true`。Client 使用的 device id 来自本次操作的 `DeviceBlobList`。

P2P-Transfer RoCE 是 Client 默认链路，无需额外环境变量。HIXL HCCS 场景需要在启动 Client 进程前设置：

```bash
export DS_RH2D_LINK_TYPE=HCCS
export DS_RH2D_HCCS_BUFFER_POOL=4:8
export DS_HIXL_CS_ENABLE=0
```

如果使用 HIXL RoCE，请在 Worker 和 Client 进程启动前都设置 `HCCL_INTRA_ROCE_ENABLE=1`，并要求两端 RoCE 网络已正确配置且可达。`remote_h2d_hccs_buffer_pool`/`DS_RH2D_HCCS_BUFFER_POOL` 为 `0:0` 时不使用 HIXL buffer-pool relay；配置正数值时会按该 buffer-pool 配置初始化 HIXL。

如果使用 HIXL CS RoCE，请在 Worker 设置 `--hixl_cs_enable=true`，并在 Client 设置 `DS_HIXL_CS_ENABLE=1`，同时保持两端 `HCCL_INTRA_ROCE_ENABLE=1`。HIXL 会根据各进程本地配置选择通信 Engine，因此 Worker 和 Client 的 CS 配置必须一致。CS RoCE 除了支持普通同节点通信无法覆盖的“跨进程、同 NPU 卡”场景，还针对小数据传输进行了专项优化，可降低通信时延。

如果使用 HIXL FabricMem 模式，请在 Worker 启动参数中设置 `--remote_h2d_hccs_enable_fabric_mem true`，并在 Client 进程启动前设置：

```bash
export DS_RH2D_LINK_TYPE=HCCS
export DS_RH2D_HCCS_ENABLE_FABRIC_MEM=1
# 可选：单个 FabricMem TransferSync descriptor batch 的并发切分数，默认 4，上限 16
export DS_HIXL_TRANSFER_SYNC_PARALLELISM=4
# 可选：一次 MGetH2D 命中多个远端来源 group 时的 group 并发数，默认 4，上限 16
export DS_HIXL_REMOTE_GROUP_PARALLELISM=4
```

FabricMem 模式不使用 HIXL buffer-pool relay，`DS_RH2D_HCCS_BUFFER_POOL`/`remote_h2d_hccs_buffer_pool` 不生效；Client 侧也不需要配置 device 内存预注册。

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `enable_remote_h2d` | `False` | Client API 的 RH2D 开关 |
| `DS_RH2D_LINK_TYPE` | `ROCE` | Client 链路类型，支持 `ROCE`、`HCCS`，必须与 Worker 一致 |
| `DS_RH2D_HCCS_BUFFER_POOL` | `0:0` | Client HIXL buffer-pool 参数，应与 Worker 一致；默认 `0:0` 关闭，配置正数值时启用 |
| `DS_HIXL_CS_ENABLE` | `0` | 是否通过 HIXL `LocalCommRes` version 1.3 启用 HIXL CS，必须与 Worker 的 `hixl_cs_enable` 一致 |
| `HCCL_INTRA_ROCE_ENABLE` | 未设置 | HIXL RoCE 链路开关。未设置时 A2 默认使用 RoCE、A3 默认使用 HCCS；设置为 `1` 时使用 RoCE，是否启用 buffer-pool 由 `remote_h2d_hccs_buffer_pool`/`DS_RH2D_HCCS_BUFFER_POOL` 决定。Worker 和 Client 必须一致 |
| `DS_RH2D_HCCS_ENABLE_FABRIC_MEM` | 未设置 | Client HIXL FabricMem 模式开关。设置为 `1` 或 `true` 时启用 FabricMem，必须与 Worker 一致 |
| `DS_HIXL_REMOTE_GROUP_PARALLELISM` | `4` | 一个 MGetH2D 请求中包含多个远端来源 group 时的并发处理数，上限为 16；每个 group 对应一组共享同一远端 HIXL root identity 的对象 |
| `DS_HIXL_TRANSFER_SYNC_PARALLELISM` | `4` | FabricMem 模式下单个 group 内 HIXL `TransferSync()` descriptor batch 的并发切分数，上限为 16；非 FabricMem 模式不使用该切分 |
| `DS_RH2D_HCCS_ENABLE_AICPU_UNFOLD` | `0` | FabricMem AICPU 展开开关。默认关闭并使用 Host unfold，此时下发 `fabric_memory.enable_aicpu_unfold=false` 和 `fabric_memory.task_stream_num=8`；设置为 `1` 或 `true` 时下发 `fabric_memory.enable_aicpu_unfold=true` 和 `fabric_memory.task_stream_num=1` |

## 快速验证

通过跨节点拉取数据的样例可快速验证RH2D的能力。

在节点1执行以下Python脚本：

```python
import acl
from yr.datasystem import (HeteroClient, Blob, DeviceBlobList)

acl.init()
device_id = 0
acl.rt.set_device(device_id)
client = HeteroClient("192.168.0.1", 31501, enable_remote_h2d=True)
client.init()
expected_val = "value"
blob_size = len(expected_val)
dev_ptr, _ = acl.rt.malloc(blob_size, 0)
acl.rt.memcpy(dev_ptr, blob_size, acl.util.bytes_to_ptr(expected_val.encode()), blob_size, 1)
in_data_blob_list = [DeviceBlobList(device_id, [Blob(dev_ptr, blob_size)])]
client.mset_d2h(["key"], in_data_blob_list)
print("[OK] Set value")
```


在节点2执行以下Python脚本：

```python
import acl
from yr.datasystem import (HeteroClient, Blob, DeviceBlobList)

acl.init()
device_id = 2
acl.rt.set_device(device_id)
client = HeteroClient("192.168.0.2", 31501, enable_remote_h2d=True)
client.init()
key = "key"
expected_val = "value"
blob_size = len(expected_val)
dev_ptr, _ = acl.rt.malloc(blob_size, 0)
out_data_blob_list = [DeviceBlobList(device_id, [Blob(dev_ptr, blob_size)])]
client.mget_h2d([key], out_data_blob_list, 60000)
output_byte = bytes("0", "utf-8").zfill(blob_size)
output_ptr = acl.util.bytes_to_ptr(output_byte)
acl.rt.memcpy(output_ptr, blob_size, dev_ptr, blob_size, 2)
assert output_byte == expected_val.encode()
print("[OK] Get value")
```

> 注意：
> 
> 脚本中初始化HeteroClient的入参需要替换为节点1/节点2服务端组件的IP和端口号。

当脚本执行完均打印OK时说明验证成功。

## 推荐配置

为确保RH2D组件在生产环境中达到最佳性能与稳定性，请参考以下配置建议。


### 开启共享内存大页内存

开启大页内存可有效提升内存的分配与拷贝性能。在910B机器的RH2D场景，驱动版本25.5.0+支持将大于21G的共享内存注册到NPU，在此时需要启用大页内存。开启大页内存可参考附录文档：[大页内存配置指南](../appendix/hugepage_guide.md)。

运行环境开启大页内存之后，启动数据系统服务端组件时需要启用大页内存配置项：

::::{tab-set}

:::{tab-item}  进程部署

```bash
dscli start -w \
    --worker_address "${node_address}" \
    --etcd_address "${etcd_address}" \
    --arena_per_tenant 1 \
    --remote_h2d_device_ids "0" \
    --enable_huge_tlb true
```

:::

::::

### Worker启用多个NPU

在多client进程场景建议worker配置使用多个NPU，轮询使用这些NPU建立传输连接，这可以有效提高总传输带宽。

部署时启用多个NPU的样例命令如下：

::::{tab-set}

:::{tab-item}  进程部署

```bash
dscli start -w \
    --worker_address "${node_address}" \
    --etcd_address "${etcd_address}" \
    --arena_per_tenant 1 \
    --remote_h2d_device_ids "0,1,2,3,4,5,6,7" \
    --enable_huge_tlb true
```

表示worker在部署时使用0-7的全部NPU。

:::

::::


### 绑定NUMA节点

绑定NUMA节点可减少远程内存访问，提升缓存访问性能。并且在RH2D场景，NUMA和NPU的亲和性也会提高传输性能，作为参考在910B设备上经测试单client进程场景会有约2GB/s的差异。

> **注意**：当启用共享内存大页时，即使设置了 cpunodebind 参数，内存分配仍将根据大页分配到 NUMA 节点。

部署时绑定NUMA节点的样例命令如下：

::::{tab-set}

:::{tab-item}  进程部署

```bash
dscli start \
    --cpunodebind 0 \
    --localalloc \
    -w \
    --worker_address "${node_address}" \
    --etcd_address "${etcd_address}" \
    --arena_per_tenant 1 \
    --remote_h2d_device_ids "0"
```


表示绑定到 NUMA 节点 0 的 CPU，并在节点 NUMA 0 分配内存。
更多 dscli 绑定 NUMA 节点部署详细信息请参考：[dscli命令参数说明](../deployment/dscli.md#命令行参数说明)。

查看NPU和NUMA亲和性并通过CPU核心作对应的命令如下：
```bash
# 显示NPU和CPU核心的亲和性对应情况
npu-smi info -t topo
# 显示每个NUMA节点的CPU核心分配情况
lscpu | grep -i numa
```

:::

::::

### worker到worker的对象数据批量获取

在RH2D场景支持批量化获取，在这个基础上通过启用worker到worker的对象数据批量获取可以进一步减少请求耗时。

部署时开启worker到worker的对象数据批量获取的命令如下：

::::{tab-set}

:::{tab-item}  进程部署

```bash
dscli start -w \
    --worker_address "${node_address}" \
    --etcd_address "${etcd_address}" \
    --arena_per_tenant 1 \
    --remote_h2d_device_ids "0" \
    --enable_worker_worker_batch_get true
```

:::

::::

### HIXL HCCS FabricMem模式

FabricMem 是 RH2D over HIXL HCCS 的一种可选内存模式。开启后，Worker 的共享内存 arena 会通过 HIXL `MallocMem(MEM_HOST)` 分配，并注册为 HIXL `MEM_HOST`；底层由 HIXL/ADXL 负责 physical-backed FabricMem 的申请、映射和释放。建立 HIXL 连接时，HIXL 会 retain allocation handle、交换 Fabric shareable handle，并在传输时完成原始 VA 到 imported VA 的地址转换。Client 发起 MGetH2D 时，可通过 HIXL `TransferSync(READ)` 直接从该 arena 将数据读入本地普通 device 目标内存。

使用 FabricMem 时建议遵循以下原则：

1. Worker 和 Client 必须同时启用 FabricMem：Worker 设置 `--remote_h2d_hccs_enable_fabric_mem true`，Client 设置 `DS_RH2D_HCCS_ENABLE_FABRIC_MEM=1`，并保持 `remote_h2d_link_type`/`DS_RH2D_LINK_TYPE` 均为 `HCCS`。
2. Worker 侧必须同时关闭传统共享内存队列：`sc_regular_socket_num=0`、`sc_stream_socket_num=0`、`oc_metadata_header=false`、`enable_shm_decrease_ref_queue=false`，并保持 `enable_pipeline_h2d=false`。如果缺少这些配置，FabricMem 初始化会直接失败，避免运行时落入依赖 mmap/futex 的路径。
3. FabricMem 与 HIXL buffer-pool relay、RoCE direct 互斥。启用 FabricMem 后，`remote_h2d_hccs_buffer_pool` 和 `DS_RH2D_HCCS_BUFFER_POOL` 不生效；不要同时设置 `HCCL_INTRA_ROCE_ENABLE=1`，也不要把 buffer-pool 调优结果直接套用到 FabricMem。
4. FabricMem 模式下 Worker 当前只创建一个 HIXL engine，使用 `remote_h2d_device_ids` 的第一个 device id 作为 engine device；如果配置多个 device id，会按该列表创建多个 HIXL `MallocMem(MEM_HOST)` arena，并在 arena 分配时轮询选择 device id。数据系统不会直接指定 NUMA id，而是在调用 HIXL `MallocMem(MEM_HOST)` 前切换当前 device，交由 HIXL/ADXL 根据 device 到 NUMA 的映射选择 Host FabricMem 申请位置。因此示例使用 `0,4,8,12`，目的是在 A3 上尽量覆盖不同 NUMA 位置，让多个 arena 分散到不同 Host 内存域；如果机器拓扑不同，应按实际 device/NUMA 映射选择对应的 device id。多 arena 场景下 `shared_memory_size_mb` 表示所有 FabricMem arena 的总容量，每个 arena 约为 `shared_memory_size_mb / remote_h2d_device_ids 数量`；部署前应保证总容量可被 arena 数量整除，且单个 arena 大小 1GB 对齐，并按 `shared_memory_size_mb` 预留足够 Host 物理内存、优先保证大页和 NUMA 配置正确。可通过 `resource.log` / `kv_resource.log` 中的 `fabricmem_physical_memory_usage` 观察 FabricMem arena 已预申请的物理内存。
5. FabricMem 模式下 Client 侧 device 目标内存不需要调用 `PreRegisterDeviceMemory` 做预注册。性能测试时无需开启 `--pre-register-device-memory`。
6. FabricMem 需要 CANN 9.0.0+ 和 HDK 26.0.0+。如果启动日志出现 `HIXL plugin does not provide FabricMem allocation APIs` 或 `FabricMem allocation APIs are unavailable`，说明当前构建环境或运行时加载的 HIXL plugin 不支持 FabricMem 申请接口。请检查 ADXL header、`libllm_datadist.so`、CANN/HIXL/HDK 版本是否一致，或改用 HCCS buffer-pool 模式。
7. FabricMem 模式默认关闭 AICPU 展开，并显式设置 `fabric_memory.enable_aicpu_unfold=false` 与 `fabric_memory.task_stream_num=8`，使用 Host 展开路径。若需要测试 AICPU 展开，可设置 `DS_RH2D_HCCS_ENABLE_AICPU_UNFOLD=1`，此时会下发 `fabric_memory.enable_aicpu_unfold=true` 与 `fabric_memory.task_stream_num=1`。
8. 如果日志中出现 `HIXL plugin malloc_memory(MEM_HOST) failed`，优先检查 Host 物理内存、大页能力、NUMA 配置、容器特权配置以及当前机器是否支持 HIXL FabricMem。FabricMem arena 的具体申请策略由 HIXL/ADXL 内部实现决定。

### FabricMem Put/Get 分阶段性能采集

Client 和所有参与请求的 Worker 都需使用 `-p on` 构建的版本，例如：

```bash
bash build.sh -b cmake -X npu -p on -t build -j 32
```

`enable_perf_trace_log=true` 不代替编译开关 `-p on`。以下 PerfPoint 是进程级累计统计，
不携带逐请求 Trace ID。耗时字段单位为纳秒，除以 `1e6` 得到毫秒。失败或提前返回也会通过
PerfPoint 析构记录已进入阶段的耗时，因此分析前应确认访问日志中的请求状态。

| 位置 | 打点 | 统计范围 |
| --- | --- | --- |
| Client 总入口 | `HETERO_CLIENT_MSET_D2H` / `HETERO_CLIENT_MGET_H2D` | 同步 Put/Get API 总耗时 |
| Client 配置 | `CLIENT_HETERO_TRANSPORT_CONFIG` | 初始化或检查进程级异构 transport 配置 |
| Client Put 准备 | `CLIENT_D2H_PREPARE_INPUT` / `CLIENT_D2H_FILTER_SOURCES` | payload 大小计算、已有对象过滤 |
| Client Put 分配 | `CLIENT_D2H_MULTI_CREATE` | MultiCreate 调用，包含请求准备、RPC 和响应处理 |
| Client Put RPC | `CLIENT_MULTI_CREATE_IPC` | 远端 ClientWorker API 的 MultiCreate RPC 窗口 |
| Client Put 传输 | `CLIENT_RH2D_WRITE_BUILD_TRANSFER_DESCS` / `CLIENT_RH2D_WRITE_COMM_INIT` / `CLIENT_RH2D_WRITE_IMPORT_HOST_SEG` / `CLIENT_RH2D_WRITE_TRANSFER_BATCH` | descriptor 构造、建链、segment 导入和传输 |
| Client Put 发布 | `CLIENT_MULTI_PUBLISH_OBJECT` / `RPC_CLIENT_MULTI_PUBLISH_OBJECT` | 发布阶段总耗时及发布 RPC |
| Client Get | `CLIENT_MGET_H2D_GET` / `CLIENT_MGET_H2D_COPY` | 获取对象信息和后续数据拷贝阶段 |
| Client Get 分组 | `CLIENT_H2D_GROUP_SOURCES` / `CLIENT_RH2D_GROUPS_TOTAL` | 按来源分组、所有远端 group 的执行总窗口 |
| Client Get 调度 | `CLIENT_RH2D_GROUPS_SUBMIT` / `CLIENT_RH2D_GROUPS_WAIT` / `CLIENT_RH2D_GROUP_QUEUE_WAIT` | 提交循环、提交完成后等待、每个任务从提交前至开始运行的时间 |
| Client 连接等待 | `RH2D_MANAGER_COMM_LOCK_WAIT_READ/WRITE` / `RH2D_MANAGER_COMM_WAIT_READY_READ/WRITE` | communicator mutex 等待、连接 ready 等待；两者分开统计 |
| HIXL 串行锁 | `HCCS_TRANSPORT_TRANSFER_LOCK_WAIT` | 非并行路径的 transfer mutex 等待 |
| FabricMem 子批准备 | `HCCS_FABRIC_PREPARE_READ/WRITE` | descriptor 转换、切分及提交前准备 |
| FabricMem 子批调度 | `HCCS_FABRIC_SUBMIT_READ/WRITE` / `HCCS_FABRIC_WAIT_READ/WRITE` | 子批提交循环、提交完成后的等待窗口 |
| FabricMem 子批排队 | `HCCS_FABRIC_QUEUE_WAIT_READ/WRITE` | 每个子任务从提交前到开始执行，含提交开销及线程调度 |
| FabricMem 设备绑定 | `HCCS_FABRIC_BIND_DEVICE_READ/WRITE` | 子任务调用设备绑定的耗时 |
| HIXL 数据传输 | `HCCS_HIXL_TRANSFER_SYNC_READ/WRITE` | 每次插件同步传输调用，包含插件 descriptor 转换及 vendor 调用 |
| Worker 创建 | `WORKER_MULTI_CREATE_TOTAL` / `WORKER_MULTI_CREATE_AGGREGATE_ALLOC` / `WORKER_MULTI_CREATE_GET_SHM_UNITS` / `WORKER_MULTI_CREATE_ADD_SHM_UNITS` | 创建总耗时、聚合分配阶段、对象分配及子响应、引用注册 |
| Worker host_info | `WORKER_RH2D_HOST_INFO_TOTAL` | 每次从 ShmUnit 构造 host_info 的总耗时，Create 和 Get 共用 |
| Worker host_info 子阶段 | `WORKER_RH2D_HOST_INFO_DEVICE` / `ROOT` / `COMM` / `DATA` / `SEGMENT` | 选卡和设置设备、root info、通信上下文、blob 元数据、segment 信息；均带 `WORKER_RH2D_HOST_INFO_` 前缀 |
| Worker Get | `WORKER_GET_OBJECT` / `WORKER_GET_IMPL_EXECUTE` / `WORKER_REMOTE_GET_PREPARE_RH2D_HOST_INFO` | Get 入口、执行、远端 Get 的 host_info 准备 |
| Worker 发布 | `WORKER_MULTI_PUBLISH_TOTAL` / `WORKER_MULTI_PUBLISH_NTX_BATCH_LOCK` / `WORKER_MULTI_PUBLISH_NTX_SEND_TO_MASTER` | Publish 总耗时、批量锁等待、元数据发布阶段 |

`HCCS_FABRIC_*` 子批打点位于并行度配置大于 1 的 FabricMem 分支；若最终仅有一个 chunk，
会记录 PREPARE 和 BIND_DEVICE，不会记录 SUBMIT、QUEUE_WAIT、WAIT。并行度为 1 时，
使用已有 `HCCS_HIXL_TRANSFER_SYNC_READ/WRITE` 和 transfer 锁等待打点。

#### 在 benchmark 进程内主动采集

```python
import json
from yr.datasystem import PerfClient

# 与 HeteroClient 在同一个进程内创建；host/port 使用实际 Worker 配置。
perf = PerfClient(host, port)
perf.init()

# 完成初始化和 warmup，并确认没有在途请求后再 reset。
perf.reset_perf_log("client")
perf.reset_perf_log("worker")

# 在这里执行待测的 Put 或 Get，并等待全部调用完成。

with open("fabricmem_perf.json", "w") as output:
    json.dump({
        "client": perf.get_perf_log("client"),
        "worker": perf.get_perf_log("worker"),
    }, output, indent=2)
```

不能在独立 Python 进程里查询原 benchmark 的 `"client"` 统计：该接口读取的是调用进程自身。
跨 Worker Get 要分别连接并采集所有参与 Worker；reset Worker 统计是进程级操作，不要在其他测试
仍运行时执行。多 Client 场景应由一个控制方协调 Worker reset，每个 Client 独立采集自己的统计。
短测试建议主动采集，不依赖约 60 秒周期的 `[Perf Log]` 日志。现有
`tests/benchmark/hetero_h2d_d2h_benchmark.py` 已提供 warmup 后 reset、cleanup 前采集的实现。

#### 通过信号输出或重置 Worker perf

运行 `DataWorker::InitAndRun` 主循环的 Worker 支持：

```bash
kill -USR1 <worker_pid>  # 输出当前累计 perf，不清零
kill -USR2 <worker_pid>  # 清零当前进程 perf
```

**发送前先确认目标进程的 INFO 日志出现 `[Worker Perf Signal] ready`。**
旧版本、尚未进入主循环的进程或只调用嵌入式 `Init` 的进程没有安装这些处理器，
对它们发送 SIGUSR1/SIGUSR2 可能触发操作系统默认的进程终止行为。
请使用实际 `datasystem_worker` PID，不要对 dscli、容器 PID 1 或整个进程组发送。

- 信号处理器只设置 lock-free 原子请求位，不打印、不分配内存、不获取互斥锁。
  主循环通常在下一次约 1 秒的轮询内执行操作；主循环阻塞时会延迟。
- SIGUSR1 输出 `[Worker Perf Signal] dump begin`、`[Perf Log]` 和 `dump complete`，
  内容进入普通 INFO 日志。日志异步落盘可能晚于处理时刻，空统计也会输出完成标记。
- SIGUSR2 执行 reset 后输出 `[Worker Perf Signal] reset complete`。
  停止请求并等待在途操作结束后发 reset，看到完成标记再开始正式测量。
  Reset 与并发打点不是跨全部计数器的原子快照，不能保证把在途请求严格划入同一个测量窗口。
- 标准信号和请求位会合并重复请求，不保证每次 `kill` 都产生独立记录。
  若同一次轮询同时收到 dump/reset，先 dump 再 reset；不要依赖信号发送顺序。
  若退出信号同时到达，优先正常关闭，不保证再处理 perf 请求。
- 未使用 `-p on` 编译时，处理器仍接收信号并输出 `request ignored` 提示，不产生有效 perf 数据。
- 只控制目标 Worker 的进程级统计，不重置 metrics/access log，不影响 Client perf。
  主循环结束时恢复此前的 SIGUSR1/SIGUSR2 处理器；服务退出后不要继续发送 perf 信号。

#### 耗时归因边界

- 先分别测 Put、Get，再比较各阶段 `count`、`avg_time`、`total_time`；host_info 是按对象记录，
  TransferSync 是按 chunk 记录，API 是按请求记录，不能直接比较不同分母的平均值。
- 父子阶段包含关系不可重复相加；例如 MultiCreate 已包含 Worker 执行时间，
  Worker 执行时间不是额外的端到端开销。
- 多个 group/chunk 并行执行，子任务累计耗时可以超过请求墙钟时间。QUEUE_WAIT 与 SUBMIT/WAIT
  存在重叠，不能叠加成总耗时；WAIT 仅覆盖提交循环完成后等待 futures 的剩余时间。
- `HCCS_HIXL_TRANSFER_BYTES_*` 和 `HCCS_HIXL_TRANSFER_DESC_COUNT_*` 是已有的字节数/数量观测，
  虽然复用 PerfInfo 字段，但不是纳秒。不要对它们做耗时单位换算。
- FabricMem READ/WRITE 由 Client 发起。Worker 的 Create/Get/Publish 打点反映控制面，
  不代表 Worker CPU 上存在一个与每次 HIXL DMA 对应的传输函数。
- 本组统计不提供 P95/P99 或跨进程逐请求时间线；尾延迟仍需结合 access log 和 Trace ID 分析。
  `-p on` 会增加计时和原子计数开销，性能结论应再用关闭打点的构建复测。

### HIXL HCCS设备内存预注册

RH2D over HIXL HCCS buffer-pool 和 RoCE direct 模式支持在 Client 侧预注册后续 MGetH2D 会写入的 device 目标内存，减少热路径中反复执行 HIXL `RegisterMem(MEM_DEVICE)` 的开销。Python 使用 `HeteroClient.pre_register_device_memory(dev_ptrs, sizes)`，C++ 使用 `HeteroClient::PreRegisterDeviceMemory(devPtrs, sizes)`。

使用该接口时需满足以下约束：

1. 仅支持 `enable_remote_h2d=True`、链路类型为 `HCCS` 且未启用 FabricMem 的 RH2D 场景，不适用于 P2P-Transfer RoCE、HCCS FabricMem、GPU 或未启用 RH2D 的场景。
2. 调用前需完成 ACL 初始化、设置当前 device，并完成 `HeteroClient.init()`；当前 device 应与后续 MGetH2D 的 `DeviceBlobList` 中的 device id 一致。
3. 预注册句柄由 Client 管理，并在 `HeteroClient` shutdown 或析构时释放；用户无需在每次 MGetH2D 后手动释放。
4. 推荐预注册一块或少量大连续 HBM 目标池，再把后续 MGetH2D 的目标 Blob 切分到这些范围内。避免把大量离散小块逐个预注册，以免占满 HIXL MEM_DEVICE 注册预算。
5. buffer-pool中转模式下，预注册范围之外的目标 Blob 仍会走临时注册 fallback；长期预注册和临时注册共享同一 HIXL 注册预算，因此混用时应预留足够空间。RoCE direct 模式要求目标 Blob 命中预注册范围，否则传输会失败。
6. FabricMem 模式下目标 device 内存不需要 HIXL `MEM_DEVICE` 注册，既不会预注册，也不会在传输热路径中临时注册。调用该接口会直接返回成功但不会产生优化效果。

示例：

```python
import acl
from yr.datasystem import HeteroClient, Blob, DeviceBlobList

acl.init()
device_id = 0
acl.rt.set_device(device_id)

client = HeteroClient("192.168.0.2", 31501, enable_remote_h2d=True)
client.init()

pool_size = 1024 * 1024 * 1024
pool_ptr, _ = acl.rt.malloc(pool_size, 0)
client.pre_register_device_memory([pool_ptr], [pool_size])

# 后续 MGetH2D 的 Blob 目标地址应落在 [pool_ptr, pool_ptr + pool_size) 范围内。
out_data_blob_list = [DeviceBlobList(device_id, [Blob(pool_ptr, 1024 * 1024)])]
client.mget_h2d(["key"], out_data_blob_list, 60000)
```

## 性能测试

在启动etcd并根据需求（共享内存大小，NPU IDs，NUMA亲和，共享内存大页，worker到worker对象数据批量获取，等）部署完worker之后，通过tests/benchmark/hetero_h2d_d2h_benchmark.py的性能测试脚本可以进行基础的性能验证。运行可以选择单client进程或多client进程场景，以及各种数据大小规格。

在节点1执行以下命令：

```bash
python hetero_h2d_d2h_benchmark.py -i 192.168.0.1 -p 31501 --set-only --no-warmup -n 1process_1thread_32key_1024KB
```

将生成的keys.json通过scp拷贝到节点2的benchmark路径，并在节点2执行以下命令：

```bash
python hetero_h2d_d2h_benchmark.py -i 192.168.0.2 -p 31501 --get-only --no-warmup -n 1process_1thread_32key_1024KB
```

> 注意事项：
> 
> 1. 参数包括
>
> | 参数名称 | 描述 | 默认值 |
> |--|--|--|
> | --set-only | 仅运行MSetD2H操作 | False |
> | --get-only | 仅运行MGetH2D操作 | False |
> | -i, --ip | Worker组件的IP地址 | 127.0.0.1 |
> | -p, --port | Worker组件的端口号 | 31699|
> | -k, --keys | 保存对象key的路径。set-only场景生成对象key，再由get-only场景读取生成的对象key | ./keys.json |
> | --key-seed | 确定性生成 key 的种子。跨节点 set/get 使用相同 seed 时，可以不通过 `keys.json` 传递 key | "" |
> | --no-warmup | 跳过预热。在RH2D场景预热是无效的 | False|
> | -n, --name | 测试名称。如果未设置则运行所有测试，而这在仅运行set或get的场景里是无法跑通的 | None |
> | -d, --device-ids | 使用的 NPU device id 列表，多个 id 以逗号分隔。多进程场景按列表轮询分配 | 0,1,2,3,4,5,6,7 |
> | --get-multiplier | 倍数重复相同的 get 请求。多 client 进程测试 MGetH2D 时建议设置该参数，增加每个进程的 get 请求数量，减少进程启动和同步开销对带宽结果的影响 | 1 |
> | --show-all-request-times | 展示每个请求的时延信息 | False |
> | --perf-path | datasystem perf 日志记录输出路径 | `datasystem_perf_<timestamp>.csv` |
> | --pre-register-device-memory | 在 get/all 模式下为每种 blob 配置预分配一块连续 device 目标内存并预注册，再将 MGetH2D 的目标 Blob 切分到该范围内。仅适用于 RH2D over HIXL HCCS buffer-pool 和 RoCE direct 模式，FabricMem 模式无需开启 | False |
>
> 入参IP和端口号需要替换为节点1/节点2 Worker组件的IP和端口号。
>
> 多进程 MGetH2D 性能测试时，建议结合 `--get-multiplier` 增大 get 请求量，例如 `--get-multiplier 50` 或更高，避免每个进程请求过少导致总带宽被初始化和连接等开销稀释。
>
> 2. 根据需求设置-n参数，来选择测试场景
>
> | 测试名称 | 描述 |
> |--|--|
> | 1process_1thread_1key_72KB         | 1 client 进程 1 key * 61 blobs * 72KB                          |
> | 1process_1thread_1key_144KB        | 1 client 进程 1 key * 61 blobs * 144KB                         |
> | 1process_1thread_1key_1024KB       | 1 client 进程 1 key * 28 blobs * 1024KB                        |
> | 1process_1thread_32key_72KB        | 1 client 进程 32 keys * 61 blobs * 72KB                        |
> | 1process_1thread_32key_144KB       | 1 client 进程 32 keys * 61 blobs * 144KB                       |
> | 1process_1thread_32key_1024KB      | 1 client 进程 32 keys * 28 blobs * 1024KB                      |
> | 1process_8thread_32key_72KB        | 1 client 进程 32 keys * 61 blobs * 72KB (8 threads)            |
> | 1process_8thread_32key_144KB       | 1 client 进程 32 keys * 61 blobs * 144KB (8 threads)           |
> | 1process_8thread_32key_1024KB      | 1 client 进程 32 keys * 28 blobs * 1024KB (8 threads)          |
> | 8process_1thread_1key_72KB         | 8 client 进程 1 key * 61 blobs * 72KB                          |
> | 8process_1thread_1key_144KB        | 8 client 进程 1 key * 61 blobs * 144KB                         |
> | 8process_1thread_1key_1024KB       | 8 client 进程 1 key * 28 blobs * 1024KB                        |
> | 8process_1thread_32key_72KB        | 8 client 进程 32 keys * 61 blobs * 72KB                        |
> | 8process_1thread_32key_144KB       | 8 client 进程 32 keys * 61 blobs * 144KB                       |
> | 8process_1thread_32key_1024KB      | 8 client 进程 32 keys * 28 blobs * 1024KB                      |
> | 1process_1key_16blob_8MB           | 1 client 进程 1 key * 16 blobs * 8MB                           |
> | 1process_1key_32blob_4MB           | 1 client 进程 1 key * 32 blobs * 4MB                           |
> | 1process_1key_64blob_2MB           | 1 client 进程 1 key * 64 blobs * 2MB                           |
> | 1process_1key_128blob_1MB          | 1 client 进程 1 key * 128 blobs * 1MB                          |
> | 1process_1key_256blob_512KB        | 1 client 进程 1 key * 256 blobs * 512KB                        |
> | 1process_1key_512blob_256KB        | 1 client 进程 1 key * 512 blobs * 256KB                        |
>
> 3. 运行完成get的部分之后，数据会被清除，再运行需要重新完成set操作
> 4. 针对较大blob num * blob size的场景，可能会需要配置DS_DEVICE_ACL_SIZE环境变量，其默认值为100MB。样例命令如下：
> ```bash
> export DS_DEVICE_ACL_SIZE=419430400
> ```

## vllm-ascend端到端运行

vllm-ascend环境配置和部署请参考[vllm-ascend + yuanrong connector 调测指南](https://gitcode.com/openeuler/yuanrong-datasystem/blob/master/tests/kvconnector/README.md)以及[[PATCH] Implement yuanrong backend](https://gitcode.com/openeuler/yuanrong-datasystem/blob/master/tests/kvconnector/patch/v0.13.0rc1/0001-Implement-yuanrong-backend.patch)。worker组件的启动和启用RH2D的参数参考本文档中的部署指南。

vllm-ascend样例命令（调整自`[PATCH] Implement yuanrong backend`内容）：
```bash
export PYTHONHASHSEED=0
export DS_WORKER_ADDR="${WORKER_IP}:31501"
# 启用client侧RH2D功能，默认为0表示不启用
export DS_ENABLE_REMOTE_H2D=1

python3 -m vllm.entrypoints.openai.api_server \
    --model /xxxxx/Qwen2.5-7B-Instruct \
    --port 8100 \
    --trust-remote-code \
    --enforce-eager \
    --no_enable_prefix_caching \
    --tensor-parallel-size 1 \
    --data-parallel-size 1 \
    --max-model-len 10000 \
    --block-size 128 \
    --max-num-batched-tokens 4096 \
    --kv-transfer-config \
    '{
    "kv_connector": "AscendStoreConnector",
    "kv_role": "kv_both",
    "kv_connector_extra_config": {
        "lookup_rpc_port": "1",
        "backend": "yuanrong"
    }
}'
```

## 常见问题FAQ

::::{tab-set}

:::{tab-item}  错误码500000

acl错误码500000对应未知内部错误，但在容器场景有可能是驱动未在容器中挂载所致，请参考部署指南中的容器内进程部署注意事项。

:::

:::{tab-item}  错误码328004

libRA错误码328004对应网口down，在机器上确定有NPU设备时有可能是光模块故障或链路不通等环境问题，所以RH2D over RoCE无法启用。

确认本机RoCE端口是否正常工作，以及RoCE网络是否互通的命令如下：

```bash
# 查看NPU拓扑结构，正常表现为NPU对之间显示为HCCS或HCCS_SW，无NA、SYS、PHB等降级连接
npu-smi info -t topo
# 检查NPU的RoCE光模块状态，present: present代表光模块已成功检测到，反之则未被检测到
hccn_tool -i <本地NPU卡号> -optical -g
# 检查NPU的RoCE链路全局状态，link status: UP代表NPU的RoCE物理链路已正常启用且连接有效
for i in {0..7}; do hccn_tool -i $i -link -g; done
# 获取IP地址和子网掩码
hccn_tool -i <本地NPU卡号> -ip -g
# 获取指定设备到目的地的地址的ping结果
hccn_tool -i <本地NPU卡号> -ping -g address <目标NPU的IP>
```

:::

:::{tab-item}  错误码107001

acl错误码107001对应无效的Device ID，请检查Device ID是否合法。可能的情况是提供的NPU卡号超出可用设备范围，例如
1. 在只有0-7的8张卡可用的情况下配置使用了卡号=8超过了范围
2. 受到环境变量ASCEND_RT_VISIBLE_DEVICES的影响导致设备映射关系变化，在ASCEND_RT_VISIBLE_DEVICES = 4, 5, 6, 7的情况下4-7卡映射到了ID 0-3，这时使用了卡号=4就超过了范围

:::

:::{tab-item} RegisterHostMemory Failed

"RegisterHostMemory Failed"错误日志表示将共享内存注册到NPU的操作失败了，可能的情况是
1. 910B环境25.5.0以下驱动版本不支持设备侧2M大页映射的功能，所以有21G共享内存的限制
2. 910B环境25.5.0及以上的驱动版本需要开启大页支持大于21G的共享内存

:::

::::

其他acl错误码可以参考[aclError定义](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/850/API/appdevgapi/aclcppdevg_03_1345.html)
