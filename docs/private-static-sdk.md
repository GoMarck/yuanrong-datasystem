# DS 私有静态 PIC SDK

## 对接契约

DS 独立构建自身 Client 和所需第三方依赖，保留仓库固定版本及补丁。xllm 从安装目录消费
`DatasystemPrivate` CMake package，仅链接 `DatasystemPrivate::client`，再自行构建最终 bridge。
生产入口是默认关闭的 `DS_BUILD_STATIC_SDK`，不依赖任何测试构建入口。

此模式目前限定原生 Linux、CMake 3.20+、单配置生成器。它自动选择 Host Client 必需实现，
关闭 Worker/Master/Coordinator 服务、Python/Java/Go、异构后端、URMA/RDMA、transfer_engine 和发布包。
普通 Worker、共享 SDK 和 Python SDK 的默认构建入口不变。

本 SDK 的 CMake 私有命名只解决消费端 target 选择问题，不自动解决 ELF 符号抢占。
bridge 的 C 接口、符号导出白名单、绑定隔离、dlopen 和 wheel 打包由 xllm 负责。

## 源码与构建信息

实现基线是 `8991b50cb6b8519bae7b47bf75b3a5c190d073de`；远端验证使用临时分支
`codex/private-static-sdk-validation`，不更新 `perf_opt_1020`，不创建 PR。
不能把基线 SHA 当作包含本次改动的交付 commit。
安装包 `share/datasystem-private/build-info.txt` 的 `BASE_COMMIT` 记录实际构建源码的 HEAD，
另记录分支、工作树状态、版本和编译器信息；detached HEAD 的分支字段为空。
本次真实验证源码为 `7fa20a784473a8cffa758759ac81984d0967ba30`，
已推送 `sync` 对应的 GitCode 仓库；详情见[验证记录](private-static-sdk-validation.md)。

## 独立构建与安装

使用新的构建目录和专用第三方缓存，避免复用共享构建的 CMake 搜索缓存：

```bash
export DS_OPENSOURCE_DIR=<private-sdk-dependency-cache>
cmake -S <ds-source> -B <private-sdk-build> \
  -DDS_BUILD_STATIC_SDK=ON \
  -DDS_OPENSOURCE_DIR="$DS_OPENSOURCE_DIR" \
  -DBUILD_THREAD_NUM=<jobs> \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=<private-sdk-install>
cmake --build <private-sdk-build> -j <jobs>
cmake --install <private-sdk-build>
```

此入口必须单独配置，不能 add_subdirectory 到 xllm，也不能与 `DS_USE_EXTERNAL_DEPS=ON` 混用。
不要从 xllm 的第三方前缀寻找包。依赖构建使用 DS 自身前缀，禁用 package registry，并优先解析静态库。
导出阶段拒绝私有前缀之外的第三方 archive/头文件、共享 target、共享库路径、无法解析的裸库名和薄 archive。
必需补丁应用失败会停止配置，不继续生成可能不兼容的包。
依赖缓存未命中时使用该依赖独立的全新工作目录，避免失败后重新配置时对旧源码重复应用补丁。
TBB 递归 make 通过环境接收编译/ABI 参数，静态归档不接收最终共享库的链接器参数。
spdlog 源包的 README 行尾先归一化为 LF，使现有 DS 命名空间补丁可以完整应用。

`DS_GLIBCXX_USE_CXX11_ABI` 可显式设置为 0 或 1；默认探测 libstdc++ 的 ABI，并写入客户端 target。
它同时应用到 DS 代码和 DS 第三方构建。bridge 中调用 C++ SDK 的编译单元必须使用相同 ABI、
兼容编译器和同一类标准库。xllm 与 bridge 之间若只使用 C ABI，则不要求公开 C++ 对象跨越该边界。
不要跨两侧传递需要对方 free/delete 的 C++ 对象；最终 C 接口的所有权规则由 xllm 定义。

## 安装后的 CMake 消费

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyBridge LANGUAGES CXX)
find_package(DatasystemPrivate CONFIG REQUIRED
    PATHS "<private-sdk-install>/lib/cmake/DatasystemPrivate" NO_DEFAULT_PATH)
add_library(my_bridge SHARED bridge.cpp)
target_link_libraries(my_bridge PRIVATE DatasystemPrivate::client)
```

安装布局：

```text
include/datasystem/                       # 公开 C++ 头文件
lib/datasystem-private/*.a                # 已选择的静态依赖，使用唯一文件名
lib/cmake/DatasystemPrivate/              # 可迁移的 config/version/targets
share/datasystem-private/archives.txt      # 实际交付 archive 清单
share/datasystem-private/private-sdk-dependencies.txt
share/datasystem-private/build-info.txt
```

所有私有依赖 target 均重写为 `DatasystemPrivate::...`；客户端 target 本身是 IMPORTED STATIC。
包配置不执行 protobuf/gRPC/Abseil 等共同依赖的 find_package，不引用宿主的通用第三方 target。
同一个实际 archive 被多条路径引用时只复制一份。静态库之间的依赖图交给 CMake 管理顺序和静态环。
第三方导入名使用该静态 archive target 的别名，传递依赖保留在静态 target 上。
公开 include、C++17、PIC 和所需 ABI 定义在 client target 上；DS 私有 include 和全局构建 flags 不传播。
`Threads::Threads` 是唯一需要 CMake 解析的系统 target，不是 DS 私有第三方实现。

导出器遍历实际 Client 链接闭包，保留必要的协调器客户端、发现、路由和轻量 transport API；
不把整个服务端装进 SDK。协议实现只保留一份 Client 版本，不靠空实现绕过依赖。

## 第三方版本、补丁与 archive 范围

版本来自 `cmake/external_libs/*.cmake`；新增的是静态/PIC 构建规则，不是整体依赖升级。
下表是构建候选及典型原始文件名，最终交付以生成的 `archives.txt` 为准，安装时文件会加私有标识。
未进入实际运行时闭包的工具/候选库不装入 SDK。

| 依赖 | DS 版本 | 典型静态 archive / 处理 |
| --- | --- | --- |
| DS Client 与内部公共模块 | 当前源码 | `libdatasystem_client.a`、必要的 common/proto/client helper archives |
| brpc | 1.15.0 | `libbrpc.a`，含 bthread/butil/bvar |
| gflags | 2.2.2 | `libgflags.a` |
| protobuf | 默认 3.25.5；仓库另支持 28.3 | `libprotobuf.a` 与选定版本所需 utf8 等传递 archive |
| gRPC | 1.65.4 | `libgrpc++.a`、`libgrpc.a`、`libgpr.a`、upb/address-sorting 等实际传递 archive |
| Abseil | 20240722 | 实际需要的 `libabsl_*.a` |
| RE2 | 2024-07-02 | `libre2.a` |
| OpenSSL | 1.1.1wa | `libssl.a`、`libcrypto.a`；关闭共享库和可加载 DSO engine |
| zlib | 1.3.1 | `libz.a` |
| LevelDB | 1.23 | `libleveldb.a` |
| c-ares | 1.19.1 | `libcares.a` |
| TBB | 2020.3-5.oe2203sp1 | `libtbb.a`，使用上游 `big_iron.inc` 原生静态构建规则，继续使用 DS 补丁 |
| SecureC | v1.1.16 | `libsecurec.a` |
| ds_spdlog | 1.12.0 | `libds-spdlog.a`，保留 ds_spdlog 命名空间及日志补丁 |
| jemalloc | 5.3.0 | `libjemalloc_pic.a`，保留 `datasystem_` 符号前缀；不构建无前缀共享分配器 |
| curl | 8.8.0 | 若进入闭包则为 `libcurl.a`；关闭未管理的可选外部后端 |
| ZeroMQ / libsodium | 4.3.5 / 1.0.18 | 若进入闭包则为 `libzmq.a`、`libsodium.a` |
| nlohmann-json | 3.11.3 | 仅头文件构建依赖，无运行时 archive |

安装 manifest 记录每个第三方版本、源包 SHA256、实际补丁文件及补丁 SHA256。
继续保留：brpc 的 BoringSSL 兼容、日志 flag 重命名、pthread 链接和指定 zlib 补丁，
`WITH_GLOG=OFF` 与 `NO_PTHREAD_MUTEX_HOOK`；TBB 的软链接、编译器适配和 bthread 协作让出补丁；
Abseil 信号处理、gRPC 编译器适配、ds_spdlog 的现有日志修复，以及 curl 当前安全补丁。

本模式额外有两项窄构建适配：

- `brpc/static-sdk-protobuf-targets.patch`：brpc 构建工具使用选定 protobuf 的 config targets，
  让静态 protoc/protobuf 的传递依赖正常进入工具链接，不依赖不完整的手工 archive 列表。
- `securec/static-sdk.patch`：让 SecureC 在该模式下生成静态库，默认共享构建规则保持不变。

protoc、grpc_cpp_plugin 和 DS RPC 生成器只属于构建工具闭包，不要求 bridge 运行时携带它们。
DS/etcd 协议在本次构建中使用同一套选定工具重新生成，生成代码与对应运行库配套。

TBB 上游对静态运行库有单实例约束；xllm 若也使用另一套 TBB，最终 bridge 必须落实符号和运行时隔离，
不能仅凭 archive 静态化认定两个调度运行库可以安全混用。该最终隔离属于 xllm bridge 的验证范围。
参考：[TBB 2020.3 静态构建规则](https://github.com/oneapi-src/oneTBB/blob/v2020.3/build/big_iron.inc)。

## 保留的动态依赖

不静态封装系统 C/C++ 运行时。预期动态依赖仅为平台实际需要的 libc、libstdc++、libgcc_s、libm，
以及 pthread/dl/rt/resolv/atomic 等系统运行库；较新 glibc 会合并其中部分功能，实际 DT_NEEDED 可能更少。
本次 aarch64 实测 `.so` 的 DT_NEEDED 为 `libstdc++.so.6`、`libm.so.6`、`libgcc_s.so.1`、
`libc.so.6`、`ld-linux-aarch64.so.1`。符号版本需求最高为 GLIBC 2.34、GLIBCXX 3.4.30、
CXXABI 1.3.13；这是本次产物的部署要求，不能由编译器版本推断更低的运行时兼容性。
其他平台必须重新构建并审计。

Host KV 模式不链接 CANN/HIXL、Ascend/CUDA 驱动库，不安装 DS 私有 `.so`。
已有 CPU NUMA 探测路径可能按需 dlopen 系统 `libnuma.so.1`，缺失时沿用现有降级；
显式调用嵌入式 Worker 是另一能力，不属于此交付。

DS 日志、flags/metrics、brpc 参数仍存在进程级状态，静态构建不改变 KVClient 生命周期或现有全局状态语义。
前缀 jemalloc 仅用于 DS 自有分配路径，不注入另一套宿主全局 malloc/free。

## 实际 KVClient 共享库示例

`example/cpp/private_static_sdk` 只从安装后的 package 构建 `libds_private_sdk_consumer.so`，
并链接一个 runner。共享库中的 C++ smoke 函数调用真实 Init、MSet、批量 Get、Del、ShutDown，
检查失败 keys 和读取结果，并在关闭前释放返回 Buffer。这不是最终 bridge C ABI 或符号隔离实现。

```bash
cmake -S <ds-source>/example/cpp/private_static_sdk -B <consumer-build> \
  -DCMAKE_PREFIX_PATH=<private-sdk-install>
cmake --build <consumer-build> -j <jobs>
python3 <ds-source>/scripts/verify_private_sdk.py \
  --prefix <private-sdk-install> \
  --library <consumer-build>/libds_private_sdk_consumer.so \
  --link-map <consumer-build>/ds_private_sdk_consumer.map
<consumer-build>/ds_private_sdk_smoke <worker-host> <worker-port>
```

共享库使用 `--no-undefined`，配合 link map、DT_NEEDED、动态符号表、TEXTREL 和安装 archive SHA256 审计。
不把同一 archive 重复出现在静态环链接命令中认定为第二份实现。
审计成功仍不能替代 Worker 功能、并发生命周期和最终 bridge 符号绑定验证。

## 验证入口、日志与当前状态

远端按仓库 `ds-test` 准备和同步确切源码，在实际 CANN 9.0.0 的 vLLM Ascend 容器中以验证用户执行：

```bash
export DS_OPENSOURCE_DIR=<container-visible-private-cache>
export DS_VALIDATION_JOBS=4
bash scripts/validate_private_sdk.sh <fresh-validation-directory> [worker-host worker-port]
```

脚本分别保存 configure/build/install、迁移后 consumer configure/build、链接审计和可选 runtime 日志，
每步输出退出码与日志路径；缺少 Worker endpoint 时明确记录 runtime=NOT_RUN。
镜像必须记录 tag/digest、架构和容器内实际 CANN 版本，不能仅按标签推断。root 只用于明确授权的依赖安装/服务管理，
源码构建和测试按验证用户执行；不能绕过 Git-only 源码同步规则。

本次已在 CANN 9.0.0 的 vLLM Ascend aarch64 容器完成真实第三方及 Client 静态编译、安装迁移、
共享消费者链接和 ELF 审计。另一个不挂载 DS 源码、构建目录及依赖缓存的容器，仅从安装包重新构建
消费者，并对同一源码版本的独立 Host Worker 完成 Init/MSet/Get/Del/ShutDown，退出码为 0。
安装包包含 160 个完整静态 archive，示例 link map 实际选入其中 105 个；没有 DS 私有共享库或公共第三方 DSO。

本地 fixture 另外覆盖父工程同名 protobuf/gRPC target 干扰、静态环和同一 archive 去重，
并实际加载共享消费者验证返回值。最终 xllm 符号绑定/卸载、ABI=0、其他平台、并发压力、性能、
Python/共享 SDK 及 Bazel 发布包回归未验证。完整命令、失败修复过程、产物校验值和日志索引见
[验证记录](private-static-sdk-validation.md)。
