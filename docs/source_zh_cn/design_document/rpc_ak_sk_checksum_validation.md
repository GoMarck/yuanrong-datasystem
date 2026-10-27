# BRPC 原始请求字节认证最小可行性验证报告

日期：2026-10-19。按 `ds-test` 在远端验证，结果为**功能机制可行**，概要设计已更新至 v0.3。该日期的隔离探针尚未接入产品；后续实现状态见当前概要和详细设计。

后续概要设计已修订至 v0.4，补充关闭 AK/SK 时的理论成本与发送隔离要求。本报告保留 P0 最小功能验证的范围和结果；实际性能测试已按用户要求停止，受干扰的初步采样不作为性能对比或验收结论。

> 历史报告：2026-10-28 已从当前 PR 删除独立手工探针，等价 wire 关键断言收敛到 brpc_auth_test。本文仅记录所列历史 SHA；当前实现与验证状态以概要／详细设计为准。

## 结论

可以利用仓库当前依赖的 BRPC 1.15.0 原生 `ChecksumHandler`，在业务请求**只序列化一次**的情况下取得实际发送的正文 IOBuf，并在接收端解压、业务 PB 解析前读取相同字节。认证信息可通过原生 RpcMeta 的 user_fields 传递，无需新增正文外层 PB，也无需为此修改 BRPC 源码。

但它不是完整的认证设施：BRPC 对未知 checksum 类型会直接放行。新方案必须保留伴生认证服务，并在业务前强制检查预期 checksum 类型、本次 Controller 的本地摘要和 HMAC。不能将回调返回 true 或网络中的 verified 字段当作认证成功。

本次证明的是接口和执行顺序可用、可消除额外正文序列化；**尚未证明产品性能无退化，也未验证真实现网滚动升级**。增加的开销仍包含正文摘要、小输入 HMAC、原生小元数据编码与本地状态管理。

## 验证对象与环境

| 项目 | 结果 |
|---|---|
| 本地分支 / 远端验证状态 | `codex/brpc-raw-request-auth` / 同分支 fetch 后 detached HEAD |
| 最终本地与远端提交 | `ad2a5fd1b61dcc3ad1258f221e6d6e8cacb9a8f8`，Git 相等断言通过 |
| 探针 | `tests/manual/rpc_checksum_auth_probe/`，独立 CMake/CTest，不加入默认 UT/ST |
| 平台与编译器 | Linux aarch64，GCC 10.3.1，Release |
| 依赖 | BRPC 1.15.0，protoc 25.5，C++ Protobuf 宏版本 4025005，OpenSSL 1.1 |
| 目标选择 | fallback-5；首选 tracked tree 有其他任务改动且磁盘紧张；选中节点 tracked tree 干净、32 CPU、约 45GB 可用内存、91GB 可用磁盘 |
| SSH 路由 | direct；所有实际命令通过受控验证账户及 HOME guard，在验证节点执行 |
| Git 同步 | 本地 origin SSH push；目标通过 HTTPS fetch 同一 fork；未通过文件复制同步源码 |
| 依赖缓存 | 从既有成功构建日志发现，验证为可读、可写、非空；复用既有缓存，没有重建第三方依赖 |
| 编译器预检 | ccache 为正常二进制，非已知 35 字节脚本；使用显式 `/usr/bin/g++`，不改系统文件 |
| 构建方式 | 原主 build 仅用于读取依赖路径；新增独立子构建目录 `build/rpc-checksum-auth-probe`，后续增量编译探针 |
| 运行库 | ldd 确认 BRPC/Protobuf/Protoc/absl/gflags/OpenSSL 均取自验证缓存，避免误用系统中另一个 SDK 的库 |

免密探测矩阵如下；端点实际地址、完整 guard 输出、缓存路径及绝对日志路径保存在本地忽略目录的私有验证记录，不写入共享仓库文档。

| 端点 | 本机直连 | ProxyJump 备用 | 结果 / 选择 |
|---|---|---|---|
| preferred | 成功 | 不需要 | tracked tree 有改动，磁盘紧张，保留原状 |
| fallback-1 | 成功 | 不需要 | tracked tree 有改动、磁盘无可用空间 |
| fallback-3 | 成功 | 不需要 | tracked tree 有改动 |
| fallback-4 | Connection refused | Connection refused | unreachable |
| fallback-5 | 成功 | 不需要 | direct，唯一合适的干净节点，选中 |
| fallback-6 | 成功 | 不需要 | tracked tree 有改动 |
| legacy | 连接超时 | banner 超时 | unreachable |

本次为单节点真实 loopback RPC，不需要节点间有序 SSH 边，不包含跨节点测试。未执行 Worker ST、未重建 Worker，不能据此声称 Worker ST 已通过。

## 测试怎么做

探针启动原生 BRPC Server 和普通 Channel，使用标准生成的普通服务和认证服务。两个服务都接收原业务 RequestV1；没有正文外层消息。发送端测试包装类只在 `_InternalSerialize` 中计数后委托原生成请求编码，不预序列化，也不是 BRPC 的 SerializedRequest 特殊路径。该包装类只是测量工具，产品直接提交原 ReqPb。

Compute 逐块读取 IOBuf，计算 SHA256，将 AK、时间戳和 HMAC 放入原生 user_fields。Verify 对收到的同一 IOBuf 求摘要，并保存到 Controller 的 session-local data。认证服务检查类型、本地捕获及签名后才增加业务计数。摘要保存在本地，不相信网络中的 verified 标记。

探针使用测试 key、测试 checksum ID 1001 和秒级 60s 有效期，仅验证机制；生产协议的命名、ID 分配和沿用微秒时间戳的精确定义仍需详细设计。注册使用公开 API，无认证补丁；仓库已有的独立 BRPC 构建兼容补丁仍在依赖中，不声称使用了完全无补丁的第三方构建。

接收端额外重编码一次只是重现旧问题的测试 oracle，不属于拟议认证路径，也不计入“客户端业务请求编码一次”的断言。

## 结果

远端直接执行输出 10 组 PASS，最终计数：

```text
RESULT PASS compute=92 verify=92 local_access=183 reset=92 business=85
```

CTest：`1/1 passed`，0.02s。该耗时只是测试完成时间，不是性能基准。

| 检查项 | 实际断言及结果 |
|---|---|
| 关闭认证 | 请求序列化计数 1；Compute、Verify、本地摘要访问均不增长；在签名流量前后分别检查，通过 |
| 普通签名 Unary | 请求编码 1 次，原生元数据到达，双方正文摘要及长度相等，通过 |
| PB 字段演进 | V2 新增字段 5，V1 服务端保留未知字段；双方原始摘要相等，V1 重编码摘要不同，验签成功 |
| 异步作用域 | 带异步 done 的 CallMethod 返回前，编码次数已为 1 且 Compute 已完成；离开凭证 scope 后 Join 成功 |
| 错误请求 | 错误正文摘要、签名、AK、修改时间戳、方法错配、过期、未保存本地捕获均拒绝，业务计数不增长 |
| 本地可信状态 | 观察到池化状态 Reset；即使报文携带 verified=true，缺失本次本地捕获仍拒绝 |
| 未知 checksum | 普通服务确实接受未知类型，Verify 未执行；认证服务对未知类型和 NONE 均拒绝 |
| 分块大正文 | 256KiB 正文编码 1 次；接收 IOBuf 多块；分块摘要一致，通过 |
| 压缩 | GZIP 请求编码 1 次；被摘要的正文小于原 PB，双方摘要一致，通过 |
| 并发身份隔离 | 4 个线程、80 次异步请求、两组凭证共用一个 Channel，全部成功且每次编码为 1 |

正文负例通过“签入与实际正文不同的摘要”构造，并没有修改网络报文字节；它证明正文与签名不一致会被拒绝，真实链路篡改应在产品测试补齐。池化 Reset 和拒绝缺失捕获通过，并不等于完整的对象池竞态或异常寿命验证。

具体字段例子：

```text
V2: payload=sample, added=7, known=3
发送字节:        0a 06 73 61 6d 70 6c 65 28 07 50 03
接收原始字节:    0a 06 73 61 6d 70 6c 65 28 07 50 03
V1 重新编码:     0a 06 73 61 6d 70 6c 65 50 03 28 07
```

`28 07` 是新增字段 5。V1 重编码把未知字段放到已知字段后面，所以原先重编码验签会使用不同字节；新方案在解析前记录摘要，完全不依赖这次重编码。

## 执行记录

以下日志均完整保留在目标仓库 `logs/` 下，表内使用相对路径。实际 SSH 目标、工作目录、同步 URL、每条命令及私有预检详情见本地 `.claude/plans/rpc-checksum-validation-private.md`。

| 阶段 / 命令 | 状态 | 日志 |
|---|---|---|
| `git fetch`、detach、HEAD 相等断言；初始探针 `ed16d5160` | 通过 | `logs/rpc-checksum-auth-sync-ed16d5160.log` |
| `cmake -S tests/manual/rpc_checksum_auth_probe -B build/rpc-checksum-auth-probe -DDS_MAIN_BUILD=... -DCMAKE_CXX_COMPILER=/usr/bin/g++ -DCMAKE_BUILD_TYPE=Release` | 通过 | `logs/rpc-checksum-auth-configure-ed16d5160.log` |
| 初次 `cmake --build ... -j 4 --target rpc_checksum_auth_probe` | 编译失败，已修正测试代码 | `logs/rpc-checksum-auth-build-ed16d5160.log` |
| Git 同步最终提交 `ad2a5fd1b`，HEAD 相等断言 | 通过 | `logs/rpc-checksum-auth-sync-ad2a5fd1b.log` |
| 最终增量 `cmake --build ... -j 4 --target rpc_checksum_auth_probe` | 通过 | `logs/rpc-checksum-auth-build-ad2a5fd1b.log` |
| 子进程专用库路径下 `timeout 30s .../rpc_checksum_auth_probe` | 10 组 PASS | `logs/rpc-checksum-auth-test-ad2a5fd1b.log` |
| 编译器、protoc、BRPC 版本及 ldd 检查 | 通过 | `logs/rpc-checksum-auth-runtime-ad2a5fd1b.log` |
| `ctest -N -R '^rpc_checksum_auth_probe$'` | 发现 1 个测试 | `logs/rpc-checksum-auth-ctest-list-ad2a5fd1b.log` |
| `ctest -R '^rpc_checksum_auth_probe$' --output-on-failure` | 1/1 通过 | `logs/rpc-checksum-auth-ctest-ad2a5fd1b.log` |

初次编译错误仅为探针适配：BRPC 的 UserFieldsMap 是 FlatMap，需用 seek；当前 Protobuf GetCachedSize 不可按虚函数 override。修改后通过 Git 提交、推送、重新拉取，未在远端手改源码。

另一次 CTest 启动检查失败：将缓存 OpenSSL 的 LD_LIBRARY_PATH 导出给 CTest 本身，会使系统 LDAP 出现 EVP_md2 符号冲突。清除 runner 的该环境，仅通过 CTest 的测试 ENVIRONMENT 给探针设置缓存库路径后通过；无需系统修改。直接执行探针和最终 CTest 都成功。该失败发生在测试发现前，不能计为测试失败或用来掩盖产品错误。

所有 configure/build/test 命令均设置已验证的 DS_OPENSOURCE_DIR。没有清理或重配置整个产品 build，没有运行 Bazel、打包、Worker ST、真实部署或基准测试。

## 对方案的影响与剩余验证

概要设计 v0.3 已采用：原业务 PB 一次编码、原生 RpcMeta 携带认证字段、checksum 回调处理正文、本地摘要交给认证入口强制校验。取消 v0.2 的 RpcAuthRequestPb 和正文解包过程，保留伴生服务以明确拒绝旧端或缺失认证设施的端点。

后续实现必须验证 DS 生成器与 CMake/Bazel 产物、真实 IAM/系统/租户/token/skip 策略、静态 SDK 链接、braft 共存、受支持 Protobuf 版本，以及异步取消、嵌套、重试、轮换、UnarySocket 和原首条流请求。后续 StreamWrite 不经过 RPC checksum；根据最终确认的需求范围，不扩展为全流逐帧认证。

真实新旧二进制滚动升级和关闭认证的 A/B 性能仍是独立验收项。当前没有分配/复制剖析，也没有 p50/p99/QPS/CPU 基准，不据此承诺零性能损耗。

源码参考：[BRPC checksum 扩展接口](https://github.com/apache/brpc/blob/1.15.0/src/brpc/checksum.h)、[默认 checksum 处理](https://github.com/apache/brpc/blob/1.15.0/src/brpc/checksum.cpp)、[baidu_std 编码与接收顺序](https://github.com/apache/brpc/blob/1.15.0/src/brpc/policy/baidu_rpc_protocol.cpp)、[普通 Channel 调用顺序](https://github.com/apache/brpc/blob/1.15.0/src/brpc/channel.cpp)。
