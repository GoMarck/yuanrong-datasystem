# Client 源码集成契约

当前 xllm 交付目标已调整为独立私有静态 PIC SDK，请使用 [私有静态 SDK 契约](static-sdk.md)。
本文保留为可选的共同依赖源码集成接口说明，不是当前 xllm 的接入方式。

## 1. 源码版本

- 仓库地址：
- 分支：
- 完整 commit SHA：

按接入方要求留空。未提交修改不能用基线 SHA 代替最终交付版本。

## 2. CMake 接口与父工程

| 接口 | 默认值 | 行为 |
| --- | --- | --- |
| `DS_BUILD_CLIENT_ONLY` | `OFF` | `ON` 时仅配置 C++ Client、必要公共实现、发现/路由/协调器客户端和协议生成；不配置 Worker/Master/Coordinator 服务、dsbench、测试、语言绑定、发布包。 |
| `DS_USE_EXTERNAL_DEPS` | `OFF` | `ON` 时复用宿主 targets，缺失时配置报错，不构建公共依赖；当前只支持与 Client-only 同时启用。 |
| `Datasystem::client` | Client-only 时提供 | 实际 target `datasystem_client`，固定 `STATIC`，无额外静态开关；有 PIC，可链接进宿主可执行文件或共享库。 |
| `DS_GLIBCXX_USE_CXX11_ABI` | 空 | 继承宿主；可显式指定 `0` 或 `1`，应用到 DS 编译并向消费者传递。 |
| `DS_OPENSOURCE_DIR` | 沿用现有规则 | DS 专用第三方构建缓存；正式构建须按仓库验证规范显式设置。 |

需要 CMake 3.20+。Client-only 当前提供 Host C++ Client，目录内局部关闭异构、RDMA/URMA、transfer_engine、pipeline、perf、语言绑定及测试选项；父工程相同变量保持原值。NPU/GPU 扩展不在本接口本次支持范围内。

```cmake
cmake_minimum_required(VERSION 3.20)
project(Host LANGUAGES C CXX)

# 宿主拥有依赖版本、构建选项及 ABI。此文件创建/导入下表 targets，
# 设置真实版本及经过核验的 brpc 属性；不引用 DS 私有安装目录。
include(cmake/common-dependencies.cmake)
set(DS_BUILD_CLIENT_ONLY ON CACHE BOOL "" FORCE)
set(DS_USE_EXTERNAL_DEPS ON CACHE BOOL "" FORCE)
set(DS_BRPC_TARGET host_brpc CACHE STRING "" FORCE)
add_subdirectory(third_party/datasystem)
add_executable(kv_example main.cpp)
target_link_libraries(kv_example PRIVATE Datasystem::client)
```

完整可配置的父工程在 `example/cpp/source_client/CMakeLists.txt`；
`host-dependencies.cmake.example` 给出完整 find_package、版本和映射设置，按宿主真实包名/target 名调整。
`main.cpp` 只包含公开 `datasystem/kv_client.h`，没有 DS 内部 include 或手工链接顺序。
不使用全局 whole-archive，不安装或导出源码集成 target；现有独立构建的共享 SDK、安装导出和发布路径继续由原构建入口管理。

### 公共依赖与 target

已存在的 target 优先；不存在时从宿主 `CMAKE_PREFIX_PATH` / `<Package>_DIR` 解析已安装包。
除 OpenSSL、ZLIB、CURL 可使用标准 Find 模块外，使用 CONFIG 包，不 FetchContent 或回退 DS 构建。
目标映射必须指向已经存在的 target；同一规范 target 和映射 target 不能指向不同实现。

| key | 必需/默认候选 target | 本次接入检查下限 |
| --- | --- | --- |
| GFLAGS | `gflags::gflags` / `gflags` | 2.2.2 |
| BRPC | `brpc::brpc` / `brpc-static` / `brpc-shared` / `brpc` | 1.12.1 |
| PROTOBUF | `protobuf::libprotobuf`；另需 `protobuf::libprotoc` | 3.21.12 |
| GRPC | `gRPC::grpc++` | 1.51.1 |
| ABSL | `absl::symbolize`；另需 `absl::failure_signal_handler` | 20230802.1 |
| SSL / CRYPTO | `OpenSSL::SSL` / `OpenSSL::Crypto` | 1.1.1 |
| LEVELDB | `leveldb::leveldb` / `leveldb` | 1.23 |
| ZLIB | `ZLIB::ZLIB` | 1.3.1 |
| RE2 | `re2::re2` | 2023-07-01 |
| CURL | `CURL::libcurl` | 8.6.0 |
| ZMQ | `libzmq` / `libzmq-static` / `ZeroMQ::ZeroMQ` | 4.3.5 |
| CARES | `c-ares::cares` / `c-ares::cares_static` | 1.19.1 |
| JSON | `nlohmann_json::nlohmann_json` | 3.11.3 |
| TBB | `TBB::tbb` | 2020.3 |
| SECUREC | `securec::securec` / `SecureC::SecureC` | 1.1.16 |

这些下限是保守接入范围，不是对所有较新版本的兼容性保证，也不是从完整编译推导出的理论最低版本。
TBB、SecureC 也有公共符号，不能当作任意可重复构建的 DS 私有实现。
TBB target 必须声明 `DS_TBB_BTHREAD_AWARE_YIELD=TRUE`：本仓库 2020.3 使用
`third_party/patches/tbb/2020.3/bthread-aware-yield.patch`，并由 `rpc_server.cpp` 的
`__tbb_yield_hook` 调用 bthread_yield；普通 sched_yield 不能让 M:N 调度器运行被抢占的锁持有者。
其他 TBB 版本必须提供经核验的等价能力，不能仅填写属性绕过缺失补丁。
版本按 `DS_<KEY>_VERSION`、target 的 `DS_DEPENDENCY_VERSION`、包版本变量的顺序读取；未知版本明确失败。
非标准名称用 `DS_<KEY>_TARGET` 指定。源码使用规范命名的依赖会验证/创建规范 alias，拒绝不同实现。
`protobuf::libprotoc` 只属于原生 DS 协议生成器的工具闭包，不要求运行时消费者链接 libprotoc。
宿主 brpc/gRPC/curl 等自身的传递闭包也必须使用同一组依赖；CMake 无法从一个不透明 DSO 推断其隐藏的内置实现。

### DS 专用依赖

只有以下隔离实现允许继续沿用 DS 获取和构建逻辑：

- 带 `datasystem_` 符号前缀的 jemalloc PIC 静态库；Client-only 不再构建无前缀的共享 jemalloc。
- 带 `ds_spdlog` 命名空间和 `libds-spdlog` 库名的日志实现，包含仓库现有补丁。

宿主已准备它们时，可以提供 `DS_PRIVATE_JEMALLOC_TARGET` 和 `ds_spdlog::spdlog`。
前者必须真实使用 `datasystem_` 前缀，不能映射到普通 jemalloc；后者不能映射到普通 spdlog。
未提供时，只构建这两项，不构建公共依赖。不构建 RocksDB、braft、OBS/XML/PCRE 等服务端第三方。
DS 内部通用工具使用 `ds_client_` 前缀；其余 `common_*`、`*_protos_client` 等内部名称仍是保留名称，
宿主若有同名 target，CMake 会明确报错，不能用外部 target 冒充内部实现。

## 3. xllm 当前组合的兼容性状态

下表是本次的源码/配置评估状态；没有任何一项经过真实 xllm 依赖组合的编译、ABI 或运行验证。
配置 fixture 的版本字符串不是兼容性证据。

| 依赖 | xllm 版本 | 当前状态与依据 |
| --- | --- | --- |
| gflags | 2.2.2 | 仅源码评估：`brpc_factory.cpp` 使用 `SetCommandLineOption`，与独立模式同版本。 |
| brpc | 1.12.1 xLLM fork | 尚未检查该 fork；Apache 1.12.1 的 Channel/Controller、pool flag、deadline 传递已有源码评估，不能外推到 fork 的补丁和 ABI。 |
| protobuf | 3.21.12 | 仅源码评估：使用传统 descriptor/CodeGenerator/Printer 和 message API；必须重生成全部 DS/etcd 协议。 |
| gRPC | 1.51.1 | 仅源码评估：etcd 使用 ClientContext、Channel 和生成 stub；须匹配 plugin 并真实编译。 |
| Abseil | 20230802.1 | 仅源码评估：日志的 symbolizer/failure-handler 接口；外部模式不固定 DS 自带 Abseil。 |
| OpenSSL | 3.2.1 | 仅源码评估：AK/SK、TLS/PEM 代码仍需针对 3.x 真实编译；未宣布完全兼容。 |
| LevelDB | 1.23 | 仅源码评估：主要为统一 brpc 的传递依赖，与 DS 原版本一致。 |
| zlib | 1.3.1 | 仅源码评估：相同基线，统一 target 传递，未做运行验证。 |
| RE2 | 2023-07-01 | 仅源码评估：使用 StringPiece/RE2 的匹配接口；未编译旧版。 |
| curl | 8.6.0 | 仅源码评估：HTTP 实现使用 easy/multi 传统接口；其 OpenSSL 等传递依赖也须统一。 |
| ZeroMQ | 4.3.5 | 仅源码评估：与原基线一致；保留 target，不重新构建 libsodium/ZeroMQ。 |
| c-ares | 1.26.0 | 仅源码评估：主要由统一 gRPC 闭包使用；不调用 DS 私有 c-ares 构建。 |
| nlohmann-json | 3.11.3 | 仅源码评估：与原基线一致。 |

目前没有由实际构建错误证实的必须升级项，因此不要求 xllm 先整体升级到 DS 独立构建版本。
已确认的局部源码适配是 `rpc_server.cpp` 的 butil 日志初始化：`WITH_GLOG=ON` 时不调用不存在的
`logging::LoggingSettings/InitLogging`，宿主继续拥有 glog 进程配置；DS 自身继续使用 ds_spdlog。
如后续真实构建发现 API 差异，必须记录调用点和错误，再做局部适配或确定新的最低版本。

## 4. 统一 brpc 的要求

外部模式不下载、不编译、不重新导入第二份 brpc，也不向宿主源码应用补丁。
在传入的真实 brpc target 上声明以下属性；这些是宿主构建契约，不是验证已完成的标志：

```cmake
set_target_properties(host_brpc PROPERTIES
    DS_DEPENDENCY_VERSION 1.12.1
    DS_BRPC_LOGGING GLOG  # 或 RENAMED_BUTIL，不能填未处理冲突的 BUTIL
    DS_BRPC_NO_PTHREAD_MUTEX_HOOK TRUE
    DS_BRPC_USES_HOST_DEPS TRUE)
```

| 补丁/选项 | 是否保留 |
| --- | --- |
| `fix-boringssl-compat.patch` | 只针对相应 BoringSSL API 路径；OpenSSL 3.2.1 不因这项补丁要求改版本或套用。 |
| `avoid-glog-flag-conflicts.patch` | `WITH_GLOG=OFF` 的 butil 与宿主 glog 共存时，需要等价重命名 `v/vmodule/minloglevel`；`WITH_GLOG=ON` 不套此补丁。 |
| `link-pthread-runtime.patch` | Linux 共享 brpc 必须保留正确的线程运行时依赖。在 pthread 独立 DSO 的系统上检查 `DT_NEEDED libpthread.so.0`；libc 已合并 pthread 的平台以实际工具链证据判断。静态 brpc 使用最终 target 的 `Threads::Threads`。 |
| `link-configured-zlib.patch` | 要求链接选定的统一 zlib；宿主已提供等价 target 传递时不必原样迁移补丁。不能悄悄链接另一份 `-lz`。 |
| `WITH_GLOG` | 接受 `GLOG` 或 `RENAMED_BUTIL` 两条明确路径，不强制所有宿主改为 OFF。DS 日志不改用 glog。 |
| `NO_PTHREAD_MUTEX_HOOK` | 两种日志路径均要求保留；不能让 brpc mutex hook 介入宿主 pthread 绑定。 |

不能仅根据“1.12.1”承诺直接复用现有 xllm brpc。先核对 fork 的上述条件及共同依赖闭包；满足即可复用，
不要求仅为版本数字一致升级到 1.15.0。若不满足，重建同一份宿主 brpc 并迁移适用补丁，而不是 DS 再构建一份。
构建时的依赖检查程序会检查日志头文件配置、重命名 flags、DS 必需 RPC flags、pool flag 符号及部分运行库关系。
它不会证明 mutex hook 编译选项或隐藏依赖的全部情况，仍需检查宿主构建记录和 ELF。

## 5. 代码生成、ABI 与部署

- 每个 DS/etcd `.proto` 使用选定 `protobuf::protoc` 重新生成；gRPC 使用选定 `gRPC::grpc_cpp_plugin`。
  不读取 DS 私有 `Protobuf_ROOT` 或 `gRPC_LIB_PATH` 来运行外部工具；外部模式不改工具的 LD_LIBRARY_PATH。
- 原生构建工具 target 缺失即失败。导入 protoc 在配置时执行 `--version` 检查，普通源码 target 在构建时检查。
  plugin 必须提供独立的版本声明且与 gRPC 版本一致，不能仅从运行库版本推断；设置 `DS_HOST_GRPC_PLUGIN_VERSION` 或 target 的版本属性。
- 跨编译显式提供三个宿主可执行 imported targets：`DS_HOST_PROTOC_TARGET`、`DS_HOST_GRPC_PLUGIN_TARGET`、
  `DS_HOST_ZMQ_PLUGIN_TARGET`，不能拿目标架构工具代替。DS 插件必须由同一 DS 源码与同版本宿主 protobuf 构建。
  跨编译还必须预先提供目标侧的前缀 jemalloc 和 ds_spdlog targets，避免用宿主默认编译器构建目标侧专用库。
  跨编译只编译依赖检查程序，不尝试运行目标架构二进制；最终目标机验证另行执行。
- 代码生成器包含路径从统一 protobuf target 取得，必要时补充 `DS_PROTO_IMPORT_DIRS`，不修改宿主搜索路径。
- DS 编译使用 C++17，公共 target 传递最低语言要求；其他库可以采用兼容的不同语言标准。
  DS、宿主和所有跨边界 C++ 依赖须使用同一标准库、兼容编译器 ABI、相同 `_GLIBCXX_USE_CXX11_ABI`，
  不能混用 libc++/libstdc++。原生外部模式构建 DS 专用库时传递宿主编译器和全局 C/C++ flags，
  显式 DS ABI 选项也传入 ds_spdlog 构建；宿主提供预建专用 target 时由宿主保证其 ABI。宿主 target 的私有 ABI 宏不会自动应用到 DS，须通过共同编译配置或 DS ABI 选项传入。
- `Datasystem::client` 的 SDK include 是 PUBLIC，内部第三方头文件没有进入公开 KVClient API，
  内部依赖按 PRIVATE 链接，由 CMake 为 STATIC 消费者传递链接闭包。
- DS 自有 `common_flags`、`rpc_option_protos`、`etcdapi_proto` 在此模式下为静态库；不部署独立 DS 核心 DSO。
  默认仍需部署隔离的 `libds-spdlog.so`，若宿主提供兼容静态 `ds_spdlog::spdlog` 则不需要这个 DSO。
  `datasystem_` jemalloc 进入静态闭包；宿主选用共享公共依赖时按其正常运行时规则部署。
- 普通 Host KVClient 不加载 Worker DSO，也不引入 CANN/HIXL。现有 `InitEmbedded` 是另一项显式能力，
  会尝试加载 `libdatasystem_worker.so`；本例不调用它，源码集成模式不构建该服务。

## 6. KVClient API、生命周期与进程影响

可执行示例 `example/cpp/source_client/main.cpp` 使用真实签名：
`Init()`、`MSet(keys, vector<StringView>, failedKeys)`、
`Get(keys, vector<Optional<ReadOnlyBuffer>>)`、`ShutDown()`，并清理本例写入的 keys。
已完成公开头文件语法编译；没有完成真实库链接或 Worker 执行。

```cpp
datasystem::ConnectOptions options;
options.host = workerHost;
options.port = workerPort;
options.connectTimeoutMs = 9000;
options.requestTimeoutMs = 5000;
datasystem::KVClient client(options);
auto status = client.Init();
// 成功后执行示例中的 MSet/Get；所有业务线程和返回 Buffer 结束后调用 ShutDown。
```

- MSet 部分成功可返回 OK，必须同时检查 `outFailedKeys`。StringView 的底层输入存储至少活到同步调用返回。
- 批量 Get 的结果按输入下标对应；部分命中时返回 OK，缺失项为空 Optional。全缺失或请求失败应先检查 Status，
  不保证错误路径输出有效。字符串重载用空字符串填缺失位置，不能可靠区分缺失和空值，建议使用 Optional 重载。
- 同一实例按并发 MSet/Get 使用；调用方为各调用提供独立输入/输出容器，保证输入存储有效，
  不与 Init/ShutDown/析构并发。该并发用法有源码锁/请求上下文机制支撑，本次未做并发压力或 sanitizer 验证。
- `connectTimeoutMs` 是连接初始化预算，默认 9000；`requestTimeoutMs` 是 API 请求预算，0 沿用 SDK 默认策略。
  Get 的 `subTimeoutMs` 约束等待对象可用的时间，默认 0 不等待，不代替整体请求预算。
- 推荐顺序：宿主启动串行 Init，启动业务并发，停止并 join 所有调用方，释放 ReadOnlyBuffer/Buffer，
  显式 ShutDown，最后析构。不得在 xllm 已析构共享 RPC/日志运行库之后再销毁 DS Client。
- 普通 Client Init 的已查调用链不安装 DS 崩溃信号处理器；`InstallFailureSignalHandler` 当前由 Worker 启动路径调用。
  这不等于 brpc/其他宿主库没有信号影响。
- Init 会启动 DS 进程级日志、flags monitor、metrics 等设施；首次 `Init(KVClientConfig)` 可配置日志目录、
  等级、stderr/异步日志和监控等。进程配置有首次冻结语义，多个 Client 不能各自重置成相互冲突的配置。
- brpc 是共享进程运行库：DS 工厂仍会设置超时传递、max_body_size、circuit-breaker 隔离和配置的连接池/事件线程参数。
  本次保持原语义，没有“零全局影响”的开关；宿主需要知晓并协调这些参数。
- DS 前缀 jemalloc 用于 DS 自有内存路径，不替换宿主 malloc/free，不使用 RTLD_DEEPBIND 或 SDK dlopen bridge。
  `ShutDown` 不承诺卸载/复位所有进程单例，也不应在其间销毁宿主共同依赖。

## 7. 验证与证据边界

### 本次实际执行

```bash
rtk proxy python3 tests/cmake/test_client_integration.py -v
rtk proxy python3 tests/cmake/test_client_linkage_audit.py -v
rtk proxy c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only -I include example/cpp/source_client/main.cpp
rtk proxy python3 scripts/ai_context/validate_module_metadata.py
rtk proxy git diff --check
```

配置用例验证父工程已有依赖不会被重复创建，宿主编译/输出/搜索路径和环境变量保持不变，
Client 和关键 DS 辅助库为静态，无服务端/语言/打包 target，消费者只链接正式 target；
缺失依赖、错误映射、未处理日志冲突、缺失 TBB 协作让出、过低版本、protoc/plugin 不匹配均明确失败。
使用 INTERFACE targets 和 fixture 工具，只证明 CMake 图/错误行为，不证明真实版本兼容。
公开 API 检查使用仓库真实头文件，但不链接实现。
最终检查结果：13 项 CMake 配置契约通过；4 项链接审计解析回归通过（mock 工具输出，不是实际 ELF 审计）；
公开 API 严格语法编译通过；18 个模块 metadata 校验通过；diff whitespace 检查通过。
审计解析回归特意覆盖同一 archive 重复/未提取 LOAD 不误报、不同选中实现、重复强符号和传递 DS 核心 DSO。

### 有真实 Linux 依赖环境后的可执行步骤（本次未执行）

按仓库 ds-test 选择验证主机、同步确切源码并设置 `DS_OPENSOURCE_DIR`，随后运行：

```bash
cmake -S example/cpp/source_client -B build/source-client \
  -DDS_HOST_DEPENDENCIES=<host-dependencies.cmake> \
  -DCMAKE_PREFIX_PATH=<unified-prefix> \
  -DDS_OPENSOURCE_DIR=<verified-cache>
cmake --build build/source-client --target ds_kv_example -j <jobs>
# 构建原生 Client 时自动编译并运行 ds_client_dependency_check。
python3 scripts/verify_client_linkage.py \
  --binary build/source-client/ds_kv_example \
  --link-map build/source-client/ds_kv_example.map \
  --targets-report build/source-client/datasystem/client-targets.txt
build/source-client/ds_kv_example <worker-host> <worker-port>
```

审计结合实际选中的 archive members、target 依赖、ELF DT_NEEDED、运行库解析和共同强符号。
同一 `.a` 在循环静态依赖的链接命令中重复出现不作为重复实现；同一路径/同一真实文件去重。
不同文件的公共库被实际选择时报告冲突；隐藏符号、LTO/strip 的影响还需要宿主构建记录辅助判断。
此审计不能单独证明 ABI 和运行行为。

独立构建/Worker 发布回归仍用原命令：

```bash
env DS_OPENSOURCE_DIR=<verified-cache> bash build.sh -X off -P off
```

本次没有执行：原有完整独立构建及发布、真实公共依赖 Client 构建、最小消费者真实链接、
实际 ELF/link map 审计、真实 Worker Init/MSet/Get/ShutDown、并发压力/资源生命周期与 sanitizer、
xllm 锁定依赖组合、跨编译和目标机运行。
本机为 macOS；仓库 ds-test 的远程源码同步要求 Git 提交/推送，而任务明确禁止两者。
没有使用文件拷贝等方式绕过该规则，也没有将本机配置结果当作 Linux 构建证据。

xllm 后续最小修改：准备一组统一依赖 targets，核对并声明 brpc 契约，
设置两个选项后 add_subdirectory，链接 `Datasystem::client`，将 bridge 调用改为公开 KVClient API，
按上述生命周期管理客户端；最终接入前补齐真实构建、ABI、链接和运行证据。

### 源码评估引用

- [Apache brpc 1.12.1 socket/pool flag](https://github.com/apache/brpc/blob/1.12.1/src/brpc/socket.cpp)、
  [deadline 传递 flag](https://github.com/apache/brpc/blob/1.12.1/src/brpc/policy/baidu_rpc_protocol.cpp)、
  [两种日志编译路径](https://github.com/apache/brpc/blob/1.12.1/src/butil/logging.h)。这些引用不代表 xLLM fork 已核验。
- [gRPC 1.51.1 C++ Version 实现](https://github.com/grpc/grpc/blob/v1.51.1/src/cpp/common/version_cc.cc)。
  检查程序使用 C++ 版本接口，不能把单独编号的 C core 版本与 C++ package 版本直接比较。
- [Abseil 20230802.1 failure-handler options](https://github.com/abseil/abseil-cpp/blob/20230802.1/absl/debugging/failure_signal_handler.h)。
