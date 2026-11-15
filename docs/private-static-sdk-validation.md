# 私有静态 SDK 验证记录

## 验证范围

本记录截至 2026-11-15。实现已获准提交并推送至 `origin` 临时分支
`codex/private-static-sdk-validation`；不推送 `perf_opt_1020`，不创建 PR。
源码基线为 `8991b50cb6b8519bae7b47bf75b3a5c190d073de`，最终验证源码应以实际同步 commit 为准。

## 已执行的本地检查

本地环境为 macOS arm64；以下结果不能替代真实 DS Linux 构建。

| 命令 | 结果 | 证据范围 |
| --- | --- | --- |
| `python3 tests/cmake/test_private_sdk_package.py -v` | 4 tests PASS | 小型真实 PIC archive、安装迁移、移除原始目录、宿主同名 target 干扰、共享消费链接、拒绝共享库及未管理库搜索项 |
| `python3 tests/cmake/test_client_integration.py -v` | 13 tests PASS | Client-only CMake 配置及依赖契约 fixture；没有编译真实 DS |
| `c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only -I include example/cpp/private_static_sdk/kv_smoke.cpp example/cpp/private_static_sdk/main.cpp` | PASS | 公开 KVClient 接口的语法和类型检查；没有链接 |
| `bash -n scripts/validate_private_sdk.sh` | PASS | 验证脚本语法 |
| `python3 scripts/ai_context/validate_module_metadata.py` | 18 files PASS | 模块元数据 |
| `git diff --check` | PASS | 已跟踪 diff 的空白检查 |

首轮本地命令输出保存在 `/tmp/ds-private-sdk-local-20261115/`；其中 package.log 是新增库搜索拒绝用例前的
3-test 输出，4-test 结果来自随后实际执行的命令输出。新增 SecureC 静态补丁已接续现有 CMake 补丁应用成功；
新增 brpc protobuf-target 补丁已对 1.15.0 上游 CMake 文本应用成功。这不是全量依赖构建证据。

## 远端预检查

验证池各端点均按 ds-test 先尝试直连；直连均超时，再使用 jump host 仅作 SSH 转发。

| 端点 | SSH 路由结果 | 资源与环境 |
| --- | --- | --- |
| `1.95.120.231:22302` | proxyjump PASS | aarch64，32 CPU，约 27 GiB 可用内存，4.2 GB 可用磁盘；marck 无 Docker socket 权限 |
| `1.95.120.231:22301` | proxyjump PASS | 1.1 GB 可用磁盘；marck 无 Docker socket 权限 |
| `1.95.120.231:22303` | proxyjump PASS | openEuler 22.03，aarch64，32 CPU，约 56 GiB 可用内存，23 GB 可用磁盘；未发现容器运行时 |
| `1.95.120.231:22304` | unreachable | fallback 连接关闭 |
| `1.95.120.231:22305` | proxyjump PASS | 1.2 GB 可用磁盘；未发现容器运行时 |
| `1.95.120.231:22306` | proxyjump PASS | 8.5 GB 可用磁盘；marck 无 Docker socket 权限 |
| `1.95.199.126:22224` | unreachable | fallback 超时 |

已验证可达节点的用户为 marck，HOME 为 `/home/marck`。22302 和 22303 的原有仓库均存在其他任务的
未提交改动，未切换、覆盖或清理它们；本次应另用 Git clone。未运行跨节点测试。

候选镜像来自 [vLLM Ascend 官方安装文档](https://docs.vllm.ai/projects/ascend/en/v0.21.0rc/installation.html)，
该版本要求 CANN 9.0.0，并列出 `quay.io/ascend/vllm-ascend:v0.21.0rc1`。
本次仅查询 registry manifest，尚未下载镜像：

```text
index digest: sha256:2a826863f1f09a09a134f45527d86eb113624f247941bd802d509a17b5544343
linux/arm64 digest: sha256:821ab2c3dcc5bcaa3f86d946b27c84f572fbb68a44da61751e7a632bbd2e163e
arm64 compressed layers: 16, total 6.25 GiB
```

压缩层大小不能代表解压后的磁盘需求。仍必须在容器内检查实际 CANN 版本、编译器、标准库及
protobuf/gRPC 工具来源，并保留镜像 inspect 和环境日志。

用户已授权在 22303 使用 root 准备 Docker 和下载镜像，但仓库自动工具钩子仍拒绝权限查询命令，
原因为 `sudo is blocked by repository hook policy`。未修改或绕过该钩子，未安装/启动 Docker。

## 尚未执行

- 实际 DS/第三方静态 PIC 编译和安装、安装后的完整 archive 清单。
- 使用真实 SDK 构建 `.so`，审计 DT_NEEDED、TEXTREL、link map、符号版本和 ABI。
- CANN 9.0.0 vLLM Ascend 容器内验证，以及真实 Worker 的 Init/MSet/Get/ShutDown。
- 最终 xllm bridge 符号绑定、dlopen/卸载、并发与 wheel 验证；此部分由 xllm 负责。
- 普通 Worker、动态/Python SDK 全量回归及 Bazel 发布包回归。

真实验证统一使用 `scripts/validate_private_sdk.sh`；它保存每步完整日志及退出码，未提供 Worker endpoint
会明确输出 `runtime=NOT_RUN`。不得用上面的 fixture 结果把这些未执行项标记为已验证。
