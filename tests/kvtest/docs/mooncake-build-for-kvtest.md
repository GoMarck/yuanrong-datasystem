# 为 KVTest 准备 Mooncake C++ 产物

KVTest 的 Mooncake 适配只使用已经编译好的 C++ 头文件、库和运行程序；拉取及编译 Mooncake 由环境使用者手动执行。本流程固定使用官方发布版 `v0.3.13.post1`，固定提交 `719735896c86b56fabec6cf3e825fb2ea640597a`（2026-10-31 核对发布页）。`v0.3.14-pre-3` 是预发布版，本流程不选用。旧版 `8952021` 通过 legacy profile 保留。可选 Mooncake 组合构建使用 CMake；本分支原生 DS 的 Bazel/CMake 构建仍保留。

三种传输的构建、运行边界及完整切换步骤见本文末尾“UB / TCP / RDMA 切换手册”。以下先给出当前 DataSystem UB / Mooncake RDMA 对比所用的官方版流程。

构建后执行 `deploy_compare_ub_rdma.py` 的全部步骤命令、实际部署形态、容器内服务启动参数及日志采集顺序见 [rc44 分支运行指南](rc44-mooncake-minimal.md)。

## 在 master 拉取源码并编译 TCP/RDMA

镜像构建排错见 [构建排错](mooncake-build-troubleshooting.md)。事件完成模式与轮询模式的单变量对照见 [mooncake-event-completion.md](mooncake-event-completion.md)。`MOONCAKE_USE_EVENT_DRIVEN_COMPLETION=ON` 贯通镜像构建与 CMake，默认 OFF；更新 SDK 后必须重新链接 KVTest，客户端头文件宏按导出 SDK 的缓存自动同步。

从仓库根目录执行：

```bash
MOONCAKE_SOURCE_PROFILE=official-v0.3.13.post1 \
bash tests/kvtest/tools/mooncake/prepare_source.sh /opt/Mooncake-v0.3.13.post1

MOONCAKE_SOURCE_PROFILE=official-v0.3.13.post1 \
MOONCAKE_USE_UB=OFF MOONCAKE_USE_TENT=OFF \
bash tests/kvtest/tools/mooncake/build_kvtest_store_image.sh \
  docker.io/library/kvcache_base:4d7a51beffc9 \
  mooncake-store-kvtest:official-v0.3.13.post1-rdma 8
```

`prepare_source.sh` 通过所选 profile 拉取官方固定提交及子模块；镜像构建会独立准备镜像内源码，宿主机这一步便于手工查看源码。prepare 和 configure 使用同一个 `source_profile.sh` 校验提交，不能只改镜像标签。使用新路径避免复用旧源码的 CMake 缓存。镜像构建脚本在隔离环境中安装依赖，并通过 Mooncake 的 CMake 构建 Store 与 C++ SDK；它还创建 Python wheel 供现有 Store 启动脚本使用。`8` 是并行编译任务数，可按编译机内存调整。构建日志路径会由脚本打印。若基础镜像标签在本机不同，应传入实际已有的镜像；可用 `nerdctl -n k8s.io images` 查看。

若在 `CMakeLists.txt:2 (project)` 报 `project requested file is a directory`，核对并更新 `tools/mooncake/configure_ub.sh`，不能传 `-DCMAKE_PROJECT_mooncake_INCLUDE:FILEPATH=`，CMake 会将已定义的空钩子路径作为目录处理。当前两个固定 Store profile 都将该参数设为实际存在的 `store_build_compat.cmake` 文件，覆盖旧缓存值。

官方 `v0.3.13.post1` 和 legacy `8952021` 的 `real_client.cpp` 都使用 gflags 的 `DEFINE_bool/DEFINE_int32`，在本构建的 glog `WITH_GFLAGS=OFF` 下需要显式包含 `gflags/gflags.h`。上述兼容钩子只对该源文件添加编译参数 `-include gflags/gflags.h`，不修改上游源码，也不把该头文件强制注入其他源文件。若日志出现这些宏的语法错误或 `FLAGS_enable_http_server/FLAGS_http_port` 未声明，更新脚本后重试同一镜像构建命令，保留现有 SDK 产物和构建缓存。

这份构建同时具备普通 TCP 与 RDMA 代码。运行时由部署配置选择 `tcp` 或 `rdma`；`USE_UB=OFF` 避免当前固定源码在纯 RDMA Pod 中出现“网卡可用，但拓扑发现为 0 HCAs”的问题。RDMA 运行还要求节点已配置 RDMA 网卡、设备分配与 verbs 运行库。若要使用 TENT HP TCP，需要另外以 `MOONCAKE_USE_TENT=ON` 编译。

核对编译选项、可执行文件和共享库：

```bash
nerdctl -n k8s.io run --rm --net=none --entrypoint sh \
  mooncake-store-kvtest:official-v0.3.13.post1-rdma -ec '
    cat /opt/Mooncake/build-ub/source_revision.txt
    grep -E "^USE_(UB|TENT):BOOL=" /opt/Mooncake/build-ub/CMakeCache.txt
    test -x /opt/Mooncake/build-ub/mooncake-store/src/mooncake_master
    find /opt/Mooncake/build-ub -maxdepth 5 -type f -name "*.so*" | head -40
    echo build-ok
  '
```

应看到 `USE_UB:BOOL=OFF`、`USE_TENT:BOOL=OFF` 和 `build-ok`。`build-ok` 只表示产物存在，不代表 RDMA 设备或网络已经验证。

## 将编译产物放入 KVTest

KVTest 适配约定将 Mooncake C++ 产物放在 `tests/kvtest/mooncake-output/`，将 DataSystemTest 的 `output/` 内容放在 `tests/kvtest/datasystem-output/`；后者应包含 `sdk/cpp/include/datasystem/kv_client.h` 和 `sdk/cpp/lib/libdatasystem.so`。两份产物都由使用者手动准备。**不要提交编译产物到 Git。** Mooncake 镜像内的原始路径为 `/opt/Mooncake`，构建目录为 `/opt/Mooncake/build-ub`；主要头文件包含 `mooncake-store/include/real_client.h`，主要库位于构建目录的 `mooncake-store/src/` 和 `mooncake-transfer-engine/src/`。

先将原 `tests/kvtest/mooncake-output` 手动备份为独立目录（如 `mooncake-output-8952021`），保留现有 `datasystem-output`。导出器会拒绝向旧版本目录混入新版本产物；新版应导入一个新的空 `mooncake-output`。在仓库根目录执行导出脚本。它只从已编译镜像复制 KVTest 所需的 C++ 头文件（包括 header-only MessagePack）、静态库、动态库、master 程序及构建配置；不下载或重新编译 Mooncake。`libmooncake_store.a` 约 1.3 GiB，首次复制可能较慢。脚本逐项显示进度；若中断，使用同一镜像和目录重新执行即可续跑，已完成的文件会核对大小与 SHA-256 后复用，损坏或不完整的文件会从头复制。导出完成时才生成 `source_image.txt` 和 `libraries.sha256`：

```bash
bash tests/kvtest/tools/mooncake/export_cpp_output.sh mooncake-store-kvtest:official-v0.3.13.post1-rdma
find tests/kvtest/mooncake-output/Mooncake/build-ub -name '*.a' | sort
test -s tests/kvtest/mooncake-output/source_image.txt
test -s tests/kvtest/mooncake-output/libraries.sha256
test -f tests/kvtest/mooncake-output/mooncake-bench/.deps/msgpack-install/include/msgpack.hpp
```

两份目录准备好后，沿用 KVTest 的 CMake 构建入口。默认只编译 DataSystem 后端；显式传 `--with-mooncake` 才将 Mooncake C++ 后端链接进同一个 `output/kvtest`（CMake 开关为 `KVTEST_ENABLE_MOONCAKE=ON`）。默认从 `datasystem-output/sdk/cpp` 读取 DataSystem SDK，从 `mooncake-output/` 读取 Mooncake 产物；也可用 `--mooncake-output DIR` 指定后者：

```bash
bash tests/kvtest/build.sh -b cmake --with-mooncake -j 8
test -x tests/kvtest/output/kvtest
```

启用 Mooncake 的 `kvtest` 仍需 DataSystem SDK 的编译头文件及共享库，因为同一个程序也保留 DataSystem 后端。Mooncake C++ 的 `libmooncake_store.a` 等静态库仅在构建时使用；Release 构建保留带符号的 `build/kvtest` 用于排障，并对部署用 `output/kvtest` 执行 `strip --strip-unneeded`，避免把静态库中的调试信息传到每个 Pod。Debug 构建不剥离符号。Mooncake 产物要和运行容器同架构、同构建版本；运行 Pod 使用上述 Mooncake 镜像，镜像内保留原始共享库路径和 RDMA 运行库。预编译 Store 静态库引用 io_uring，即使测试纯内存 Put/Get 也必须链接 `liburing`，CMake 缺库时会直接提示。若 CMake 报某个系统开发库缺失，应在编译 KVTest 的环境安装对应开发包，而不是重新编译 Mooncake。实际运行前用 `ldd tests/kvtest/output/kvtest` 检查依赖。

若最终链接出现 `undefined reference to io_uring_queue_exit`、`io_uring_queue_init_params`、`__io_uring_get_cqe` 等，在编译 KVTest 的 openEuler/RHEL 环境执行 `yum install -y liburing-devel`（Debian/Ubuntu 为 `apt-get install -y liburing-dev`），然后重跑 KVTest 构建。只有运行库 `liburing.so.2` 而没有开发包提供的 `liburing.so` 时，`find_library` 仍可能找不到链接库。也可按下文使用已经安装开发依赖的 Mooncake 镜像编译 KVTest。

切换 `--mooncake-output` 时，KVTest CMake 会重新查找该目录下的 gflags/glog，避免保留旧产物的绝对路径。若旧脚本报 `No rule to make target .../mooncake-output/.../libgflags.so`，而日志显示已选择新版目录，可在 `tests/kvtest` 执行 `cmake -S . -B build -U MC_GFLAGS -U MC_GLOG` 清除这两个旧缓存项，再用原参数编译。无需清空第三方依赖缓存，也不要将旧库复制到新目录凑路径。

若 master 缺少 Mooncake 的系统开发库，可在已构建的 Mooncake 镜像内编译 KVTest；挂载包含 `datasystem-output` 和 `mooncake-output` 的仓库目录即可，仍只执行 KVTest 的 CMake 构建：

```bash
nerdctl -n k8s.io run --rm --net=none \
  -v "$PWD:/workspace" -w /workspace --entrypoint bash \
  mooncake-store-kvtest:official-v0.3.13.post1-rdma \
  tests/kvtest/build.sh -b cmake --with-mooncake -j 8
```

## 升级后部署到 89 / 91

也可以保留原 `mooncake-output`，将新版本导出到独立目录并显式选择：

```bash
# 在 tests/kvtest 目录执行
bash tools/mooncake/export_cpp_output.sh \
  mooncake-store-kvtest:official-v0.3.13.post1-rdma mooncake-output-v0.3.13.post1
bash build.sh -b cmake --with-mooncake \
  --mooncake-output "$PWD/mooncake-output-v0.3.13.post1" -j 64
```

采用独立目录时，部署命令必须显式传下面的新 `--mc-image`，不能依赖原 `mooncake-output/source_image.txt` 的旧标签。

将新镜像 `mooncake-store-kvtest:official-v0.3.13.post1-rdma` 导入 master、worker02（89）、worker03（91）的 containerd `k8s.io` 命名空间；脚本使用 `imagePullPolicy: Never`。更新产物后必须重新编译 KVTest，不能只更新 store 镜像而继续运行链接旧静态库的 client。使用新镜像启动 master、store、client，整组一起切换。

在 `tests/kvtest` 执行：

```bash
python3 deploy_compare_ub_rdma.py \
  --scenario comparison/scenario.two-rdma-nodes-8clients.json \
  --systems mooncake --duration 60 \
  --mc-image mooncake-store-kvtest:official-v0.3.13.post1-rdma
```

这里没有自动下载或编译 Mooncake；以上构建、导出和 KVTest 编译均由使用者手动执行。此升级已核对官方头文件的 create/setup_real/put/get_buffer/tearDownAll 接口；真实 ARM64 依赖安装、编译链接及 RDMA 性能仍需在目标机器验证，不代表新版一定更快。回退时恢复原镜像、原 `mooncake-output-8952021` 并重新编译 KVTest，整组服务一起回退。

## UB / TCP / RDMA 切换手册

源码版本、编译能力和运行协议是三个独立选择。镜像标签只是名字；实际能力以 `source_revision.txt`、`CMakeCache.txt`、头文件和库为准。以下命令均在 `tests/kvtest` 目录手动执行。

### 构建与运行能力矩阵

| 目标 | 源码 profile | MOONCAKE_USE_UB | MOONCAKE_USE_TENT | SDK 运行协议 | 当前 C++ KVTest 状态 |
|---|---|---|---|---|---|
| 官方普通 TCP | official-v0.3.13.post1 | OFF | OFF | tcp | 接口已适配，官方包需目标环境编译验证 |
| 官方 RDMA | official-v0.3.13.post1 | OFF | OFF | rdma | 接口已适配，官方包需目标环境编译验证 |
| legacy 普通 TCP / RDMA | legacy | OFF | OFF | tcp / rdma | 当前已使用的 8952021 路径 |
| legacy UB | legacy | ON | OFF | ub | 镜像及旧 UB Store 路径已有；C++ KVTest UB 入口尚未适配 |
| 官方 UB 候选构建 | official-v0.3.13.post1 | ON | OFF | ub（需核对 SDK 与 UMDK） | 可按下文构建；未验证目标 UMDK ABI，C++ KVTest UB 入口尚未适配 |

普通 TCP 与 RDMA 使用相同的非 UB 构建。无需为了 TCP 重新编译同一提交和同一依赖的 RDMA 镜像，只要运行时将 SDK 和 store 一起切到 `tcp`。当前镜像仍包含并依赖 verbs 库，因此 TCP 模式也应保留该镜像内的运行库。`MOONCAKE_USE_TENT=ON` 是另一种引擎构建，不等价于普通 TCP；不能仅通过给镜像起名 `hptcp` 来切换。

`MOONCAKE_SOURCE_PROFILE` 必须显式选择：构建工具默认 `legacy`，不会自动跟随官方 main 或新 release。`prepare_source.sh` 和构建脚本必须使用同一个 profile。新源码提交应增加独立 profile 并核对接口/依赖，不要只改某一处哈希或复用旧 build 缓存。

### 三种传输的镜像编译命令

普通 TCP：

```bash
MOONCAKE_SOURCE_PROFILE=official-v0.3.13.post1 \
MOONCAKE_USE_UB=OFF MOONCAKE_USE_TENT=OFF \
bash tools/mooncake/build_kvtest_store_image.sh \
  docker.io/library/kvcache_base:4d7a51beffc9 \
  mooncake-store-kvtest:official-v0.3.13.post1-tcp 8
```

RDMA（与上面的普通 TCP 构建能力相同，单独标签用于保存实验记录）：

```bash
MOONCAKE_SOURCE_PROFILE=official-v0.3.13.post1 \
MOONCAKE_USE_UB=OFF MOONCAKE_USE_TENT=OFF \
bash tools/mooncake/build_kvtest_store_image.sh \
  docker.io/library/kvcache_base:4d7a51beffc9 \
  mooncake-store-kvtest:official-v0.3.13.post1-rdma 8
```

现有 legacy UB 路径：

```bash
MOONCAKE_SOURCE_PROFILE=legacy \
MOONCAKE_USE_UB=ON MOONCAKE_USE_TENT=OFF \
bash tools/mooncake/build_kvtest_store_image.sh \
  docker.io/library/kvcache_base:4d7a51beffc9 \
  mooncake-store-kvtest:legacy-8952021-ub 8
```

官方版 UB 的候选编译命令如下。该命令只选择编译能力，不表示已经完成 C++ KVTest UB 适配或目标节点验证：

```bash
MOONCAKE_SOURCE_PROFILE=official-v0.3.13.post1 \
MOONCAKE_USE_UB=ON MOONCAKE_USE_TENT=OFF \
URMA_INCLUDE_DIR=/usr/include/ub/umdk/urma \
URMA_LIBRARY=/usr/lib64/liburma.so \
bash tools/mooncake/build_kvtest_store_image.sh \
  docker.io/library/kvcache_base:4d7a51beffc9 \
  mooncake-store-kvtest:official-v0.3.13.post1-ub 8
```

UB 基础镜像需要匹配的 URMA 头文件、库和 provider；节点需要匹配的内核/UMDK、UB 设备及 device plugin。`URMA_INCLUDE_DIR`、`URMA_LIBRARY` 指镜像内路径，不是宿主机路径。运行 Pod 必须实际分配到 UB 设备，单纯开启 `USE_UB` 不会提供设备。当前 UB target 的自动选择要求 `/dev/uburma` 中恰好有一个分配到的 `bonding_dev_*` 字符设备，多设备时应明确指定设备。不要在纯 RDMA Pod 中使用已出现拓扑冲突的 legacy UB 镜像；该场景用 `USE_UB=OFF` 构建。

### 产物校验、镜像分发、导出和 KVTest 编译

下面以 RDMA 为例；切换时把 `IMAGE`、`SDK_OUTPUT` 一起改为所选版本和构建类型。每个版本/编译组合使用独立产物目录。

```bash
IMAGE=mooncake-store-kvtest:official-v0.3.13.post1-rdma
SDK_OUTPUT="$PWD/mooncake-output-official-v0.3.13.post1-rdma"

nerdctl -n k8s.io run --rm --net=none --entrypoint sh "$IMAGE" -ec '
  cat /opt/Mooncake/build-ub/source_revision.txt
  grep -E "^USE_(UB|TENT|TCP):BOOL=" /opt/Mooncake/build-ub/CMakeCache.txt
  test -x /opt/Mooncake/build-ub/mooncake-store/src/mooncake_master
  test -s /opt/Mooncake/build-ub/mooncake-store/src/libmooncake_store.a
  python3 -c "from mooncake.store import MooncakeDistributedStore"
'

nerdctl -n k8s.io save -o /tmp/mooncake-switch.tar "$IMAGE"
for node_ip in 141.62.33.89 141.62.33.91
do
  scp /tmp/mooncake-switch.tar root@"$node_ip":/tmp/mooncake-switch.tar
  ssh -o BatchMode=yes root@"$node_ip" 'ctr -n k8s.io images import /tmp/mooncake-switch.tar'
done

bash tools/mooncake/export_cpp_output.sh "$IMAGE" "$SDK_OUTPUT"
test -s "$SDK_OUTPUT/source_image.txt"
test -s "$SDK_OUTPUT/libraries.sha256"
cat "$SDK_OUTPUT/Mooncake/build-ub/source_revision.txt"
```

对普通 TCP / RDMA 产物，接着执行：

```bash
bash build.sh -b cmake --with-mooncake \
  --mooncake-output "$SDK_OUTPUT" -j 64
```

UB 镜像也可归档/导出 C++ 产物，但当前 KVTest CMake 显式按非 UB 的 RDMA/TCP 编译定义和依赖链接，尚未实现与 UB 产物对应的宏、URMA 链接与运行协议入口。不要把“导出成功”当成“当前 C++ KVTest 可运行 UB”。旧 Python Store UB 执行器已移除；当前 C++ KVTest 尚不提供 Mooncake UB 运行入口。

镜像导入与头文件/库存在校验不代表网络验证完成。TCP 需要 Pod 间 TCP 连通；RDMA 还需要 HCA/GID/端口、verbs provider、有效内存锁权限及设备配额；UB 需要分配到的 UB 设备和匹配 URMA 运行环境。master/store/client 使用同一源码和构建组合；控制/通知 RPC 仍可走 TCP，数据协议由 SDK 配置决定。

### 运行参数与当前编排入口边界

| 配置位置 | 普通 TCP | RDMA | UB |
|---|---|---|---|
| KVTest JSON `backend` | mooncake | mooncake | C++ 入口尚未适配 |
| KVTest JSON `mooncake.transport` | tcp | rdma | 当前校验拒绝 ub |
| `mooncake.device_name` | 空 | 每个 Pod 实际分配的 HCA | 需新增 UB 设备配置与校验 |
| `deploy_client.py gen-config --mooncake-transport` | tcp | rdma | 当前 choices 不接受 ub |
| `deploy_mooncake.py start --protocol` | tcp | rdma | 当前 choices 不接受 ub |
| `store_pod_target.py --protocol` | tcp | rdma | ub（底层 target 支持，不能替代整个 C++ 编排适配） |

普通 TCP 的 KVTest 配置片段（合并到完整 client 配置，保留 pipeline/nodes 等字段；替换地址为实际 Pod IP）：

```json
{
  "backend": "mooncake",
  "mooncake": {
    "master_address": "192.168.0.10:50051",
    "pod_ip": "192.168.0.11",
    "transport": "tcp",
    "device_name": "",
    "local_buffer_size": "2GB"
  }
}
```

RDMA 使用同样结构，将 `transport` 改为 `rdma`，`device_name` 改为该 Pod 获配且已验证的 HCA。不要把 worker02 的设备名直接复制给 worker03。`deploy_mooncake.py` 支持通过 `--devices-json` 为各 store 指定不同 HCA，client 部署配置支持每实例 `mooncake_device`。

**`deploy_compare_ub_rdma.py` 当前固定 DataSystem UB / Mooncake RDMA。** `--systems mooncake` 只是选择测试对象，`--mc-image` 只是选择镜像；二者都不会改数据协议。该入口会强制生成 RDMA client/store 参数并请求 RDMA 配额，不能用换成 `*-tcp` 或 `*-ub` 镜像标签来切换为 TCP/UB。普通 TCP 可以复用 `deploy_pods.py`、`deploy_mooncake.py` 和 `deploy_client.py`，同时生成 TCP client/store 配置、移除 RDMA Pod 设备请求和设备探测；本文不将尚未提供的一键 TCP/UB 对比命令描述为可用功能。

### 哪些切换需要重新编译

| 变化 | Mooncake 镜像/库 | KVTest |
|---|---|---|
| 同一非 UB 构建，仅 tcp ↔ rdma | 无需重编，整组进程按新协议重启 | 产物相同时无需重编；重新生成配置并部署 |
| 源码版本、USE_UB、USE_TENT 或 ABI 依赖变化 | 重新构建并使用独立镜像标签 | 重新导出，适配对应编译定义/依赖后重新链接 |
| 容量、buffer、网卡选择、线程和负载参数变化 | 通常无需重编；重新配置/启动生效 | 通常无需重编，按参数作用范围重启 |
| 仅改控制机 Python 编排或 HTML 报告 | 无需重编 Mooncake | 无需重编 C++ KVTest |

每次记录源码提交、编译开关、镜像身份、导出库校验值和 KVTest 构建提交，并保留当前 `report.html`、`service-startup.json`、`pods.json`。新旧运行分别保存，不将不同协议或版本的 client 与 store 混用。回退时恢复原镜像、原 SDK 导出目录，必要时重新链接 KVTest，再重新部署整组进程；不要只回退 master 或一台 store。
