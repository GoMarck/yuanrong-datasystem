# 私有静态 SDK 验证记录

## 源码与环境

记录日期：2026-11-16。真实构建和功能验证均使用源码
`7fa20a784473a8cffa758759ac81984d0967ba30`，两份远端 checkout 的已跟踪文件均干净。
后续仅更新验证文档的 commit 不代表 SDK 二进制重新构建。

- GitCode：[yaohaolin/yuanrong-datasystem](https://gitcode.com/yaohaolin/yuanrong-datasystem)。
- 分支：`codex/private-static-sdk-validation`；本地推送 remote 为 `sync`，远端使用同一仓库的 HTTPS URL。
- 保留 `perf_opt_1020`；未创建 PR/MR。
- 按 ds-test 在验证池 22302 执行，SSH 路由为 proxyjump；jump 仅转发，构建与测试使用普通验证用户。
- 宿主 openEuler 22.03 / aarch64；构建容器 Ubuntu 22.04 / aarch64。
- 镜像：`quay.io/ascend/vllm-ascend:v0.20.2rc1`，实际下载并使用固定 digest：
  `sha256:a926b560b81785a9f820f975748a39807f0b1753675ab40ec30cb4ae3dabb71e`。
- 容器内 `/usr/local/Ascend/cann` 指向 `cann-9.0.0`，compiler、opp 的 `version.info` 均为 `Version=9.0.0`。
- `/usr/bin/gcc`、`/usr/bin/g++` 11.4.0，`/usr/bin/cmake` 3.22.1，SDK 为 Release / PIC / C++17 / libstdc++ ABI=1。
  显式 PATH 和编译器避开 ccache；未修改系统编译器或 ccache。
- 没有挂载 NPU 设备或驱动，验证范围是 Host KV。CANN 的存在不代表异构能力已验证。

池中各端点均先直连一次，直连均超时；proxyjump 下 22301/22302/22303/22305/22306 成功，
22304 连接关闭，legacy 端点超时。22303 缺少免密 sudo，最终使用 22302。
按授权清理了确认未被进程使用的旧 Bazel 编译缓存，释放约 28 GiB；没有删除源码或活动构建。
没有运行跨节点测试。完整环境、镜像、磁盘和 Git 日志保留在受控验证目录，不在仓库记录私有连接信息。

SDK checkout 最初从获准的 GitHub 临时分支克隆，随后转为 GitCode fetch。
独立 Worker 的第二次 HTTPS clone 曾因 HTTP/2 失败；改为目标服务器上对已核对 checkout 执行
`git clone --no-hardlinks --no-checkout`，随后两份 checkout 都从 GitCode fetch 并切到上述 SHA。
没有通过文件复制、归档或粘贴补丁同步 DS 验证源码。

## SDK 构建与安装

下文 `<logs>`、`<sdk-source>`、`<worker-source>`、`<sdk-run>`、`<worker-run>` 是验证环境中的独立目录。
完整日志由执行环境保留；以下为实际命令的参数化写法。

SDK 使用新建的专用 `DS_OPENSOURCE_DIR=<sdk-run>/opensource-cache`，从源码编译 DS 版本的第三方依赖。
后续修复重试复用已成功且 cache key 匹配的依赖和 DS 对象文件：

```bash
export PATH=/usr/bin:/bin:/usr/local/python3.11.15/bin
export CC=/usr/bin/gcc CXX=/usr/bin/g++
export DS_OPENSOURCE_DIR=<sdk-run>/opensource-cache
export DS_VALIDATION_JOBS=4
bash <sdk-source>/scripts/validate_private_sdk.sh <sdk-run>/validation
```

最终日志在 `<logs>/sdk-run-9/`，每步退出码均为 0：

| 步骤 | 命令 / 结果 | 日志 |
| --- | --- | --- |
| 配置 | `cmake -S <sdk-source> -B <sdk-build> -DDS_BUILD_STATIC_SDK=ON -DDS_OPENSOURCE_DIR="$DS_OPENSOURCE_DIR" -DBUILD_THREAD_NUM=4 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<sdk-prefix>` | `configure.log` |
| 编译 | `cmake --build <sdk-build> -j 4`；真实 DS 静态 Client | `build.log` |
| 安装 | `cmake --install <sdk-build>` | `install.log` |
| 迁移后配置 | 安装目录移至 `relocated-sdk` 后配置示例 | `consumer-configure.log` |
| 消费者编译 | `cmake --build <consumer-build> -j 4`；真实 `.so`，`--no-undefined` | `consumer-build.log` |
| 链接审计 | `verify_private_sdk.py --prefix <relocated-sdk> --library <consumer.so> --link-map <consumer.map>` | `linkage.log` |

脚本本轮未传 Worker endpoint，因此该 driver 正确记录 `runtime=NOT_RUN`。
真实运行验证在下面的独立容器中单独完成。

## 安装包独立消费与链接证据

将安装产物在目标服务器本地打包、迁移至另一目录，再挂载至新的同镜像容器 `/opt/ds-private-sdk:ro`。
该容器不挂载 DS 源码、构建目录或原依赖缓存；仅额外挂载 Git checkout 中的消费示例目录、审计脚本和日志目录。
断言原 DS checkout/build 路径在容器中不存在后，执行：

```bash
export DS_OPENSOURCE_DIR=/opt/ds-private-sdk
cmake -S /consumer-src -B <consumer-build> -DCMAKE_PREFIX_PATH=/opt/ds-private-sdk
cmake --build <consumer-build> -j 2
python3 /audit.py --prefix /opt/ds-private-sdk \
  --library <consumer-build>/libds_private_sdk_consumer.so \
  --link-map <consumer-build>/ds_private_sdk_consumer.map
```

全部退出 0。示例共享库仅链接 `DatasystemPrivate::client`，未列举任何第三方 target 或库。
证据在 `<logs>/independent-consumer/{configure.log,build.log,linkage.json,readelf-dynamic.log,ldd.log}`：

- package：`DatasystemPrivate`；正式 target：`DatasystemPrivate::client`，IMPORTED STATIC。
- 安装包 160 个完整静态 archive；link map 从包中实际选入 105 个，无第三方 archive 来自原缓存或系统。
- 安装 target 声明均以 `DatasystemPrivate::` 开头；不含原绝对路径、通用 protobuf/gRPC target 引用。
- 无 TEXTREL，无打包 `.so`，无 brpc/gflags/protobuf/gRPC/OpenSSL 等第三方 DT_NEEDED。
- 实际 DT_NEEDED：`libstdc++.so.6`、`libm.so.6`、`libgcc_s.so.1`、`libc.so.6`、`ld-linux-aarch64.so.1`。
- 实际最高版本需求：GLIBC 2.34、GLIBCXX 3.4.30、CXXABI 1.3.13。没有证明兼容更老的系统运行时。
- `nm --defined-only` 确认消费者没有定义全局 malloc/free/calloc/realloc 等替代入口，
  存在 `datasystem_malloc`、`datasystem_calloc`、`datasystem_free`。日志为 `global-allocator.log` 和 `defined-symbols.log`。

完整版本、源包 SHA256、补丁路径及 SHA256 已生成于包内
`share/datasystem-private/private-sdk-dependencies.txt`，archive 名单在 `archives.txt`，
每个 archive 的 SHA256 在 `linkage.json`。本次实际版本为 brpc 1.15.0、gflags 2.2.2、protobuf 3.25.5、
gRPC 1.65.4、Abseil 20240722、RE2 2024-07-02、OpenSSL 1.1.1wa、zlib 1.3.1、LevelDB 1.23、
c-ares 1.19.1、curl 8.8.0、ZeroMQ 4.3.5、libsodium 1.0.18、TBB 2020.3-5.oe2203sp1、
SecureC v1.1.16、jemalloc 5.3.0、ds_spdlog 1.12.0、nlohmann-json 3.11.3。
没有将 xllm 的不同版本组合标记为兼容；此次方案保留两侧各自版本。

受控目录中的交付文件：`artifacts/datasystem-private-sdk-linux-aarch64-7fa20a7.tar.gz`，106371996 bytes。
SHA256：`4b353815fd29fc7ef65ede590f964a8133df4947ad6b8df40c226a366146ccea`。
归档只含可安装 SDK，不含 DS 源码、构建目录或工具缓存。

## 真实 Worker 功能验证

Worker 使用另一份干净 checkout、独立第三方缓存和独立容器。关闭 tests、Python/Java/Go、异构后端、
URMA/RDMA 等可选能力，保持普通 Worker 构建入口，编译真实 `datasystem_worker_bin`。
首次全量编译在 `0680a1a6c3b4064047a97a5f9fe83ef8f385a007` 通过；之后切到最终验证 SHA，
重新配置并增量重建同一 target，通过，退出 0。日志在 `worker-run-2/`、`worker-run-3/`。

构建命令为 `DS_OPENSOURCE_DIR=<worker-run>/opensource-cache cmake --build <worker-run>/build --target datasystem_worker_bin -j 4`。
准确配置参数保留在对应 `environment.log`、`configure.log` 及 CMakeCache 中。

Worker 使用其独立缓存中的动态库运行路径，启动新的单节点 Host Worker 和内嵌 metastore，
使用空闲回环端口、独立 cluster_name、日志/数据库/UDS/健康检查目录，64 MiB 共享内存。
启动参数包含 `--master_address=<worker-address>`、`--metastore_address=<metastore-address>`、
`--start_metastore_service=true`。没有连接或停止已有集群。
首次启动缺少普通 Worker 的动态依赖搜索路径，第二次缺少 metastore 配置，均保留失败日志后修正启动命令；
最终 Worker ready。SDK 消费者没有设置 Worker 的 `LD_LIBRARY_PATH`。

独立消费者容器共享本次 Worker 的 IPC namespace 和 UDS 日志挂载，执行：

```bash
export DS_OPENSOURCE_DIR=/opt/ds-private-sdk
export DATASYSTEM_CLIENT_LOG_DIR=<client-log-dir>
timeout 45 <consumer-build>/ds_private_sdk_smoke 127.0.0.1 <worker-port>
```

退出 0，`runtime/status.log` 记录 `runtime-status=0`。实际 C++ 示例完成 Init、MSet 两个值、批量 Get
逐项校验内容、Del 清理、释放 Buffer、ShutDown；任一步错误或返回值不匹配会非零退出。
完整客户端、Worker 日志在 `runtime/`、`client-runtime/`；测试后对核对过可执行文件的本次 Worker PID 发送 SIGTERM。

## 构建问题与回归证据

前八次 SDK 尝试保留在 `<logs>/sdk-run-1/` 至 `sdk-run-8/`，依次暴露并修复：
TBB 归档误收链接参数、spdlog README CRLF 补丁应用、gflags 用户注册表写入、brpc 静态工具缺失
utf8 target、curl 常量 BOOL 表达式中的构建探测路径、CMake 跨目录内部标记、导出图丢失静态依赖顺序。
这些尝试不是成功验证；最终通过结果以 sdk-run-9 和 independent-consumer 为准。

本地 macOS arm64 执行以下检查；不能代替上述 Linux 真构建：

| 命令 | 结果 |
| --- | --- |
| `python3 tests/cmake/test_private_sdk_package.py -v` | 5 tests PASS；真实 PIC archive、静态环、安装迁移、删除原目录、同名宿主 target、加载 `.so` 返回 42、共享/未管理库拒绝、CRLF 回归 |
| `python3 tests/cmake/test_client_integration.py -v` | 13 tests PASS；Client-only CMake 配置 fixture |
| `python3 tests/cmake/test_client_linkage_audit.py -v` | 4 tests PASS |
| `c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only -I include example/cpp/private_static_sdk/kv_smoke.cpp example/cpp/private_static_sdk/main.cpp` | PASS；公开接口类型检查 |
| `bash -n scripts/validate_private_sdk.sh` | PASS |
| `python3 scripts/ai_context/validate_module_metadata.py` | 18 files PASS |
| `git diff --check`、暂存区检查 | PASS |

## 尚未验证

- 最终 xllm bridge 的 C ABI、导出白名单、ELF 绑定隔离、双版本依赖同时加载、dlopen/卸载和 wheel：由 xllm 负责。
- ABI=0、其他编译器/标准库/架构/系统，以及 protobuf 28.3 选择分支。
- 并发压力、性能、sanitizer、容错恢复、多节点、TLS/认证及 NPU 功能；本次只做单次 Host KV 功能 smoke。
- 普通共享/Python SDK 全量回归、完整 Worker 功能测试集及 Bazel 发布包回归。
  选定容器未提供 Bazel；已完成的普通 Worker 目标编译不代表这些发布产物验证。

此次证明 DS 保留自身依赖的静态 PIC package 可以独立生成共享库并调用真实 KVClient，
没有证明两套公共依赖在最终 xllm 进程中的符号及生命周期隔离。
