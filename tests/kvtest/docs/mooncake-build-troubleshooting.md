# Store 镜像构建排错

以下构建命令均在 `tests/kvtest/tools/mooncake` 目录执行。在该目录执行 `bash build_kvtest_store_image.sh`。脚本将完整构建输出保存到
`kvtest-store-image-build-日期-时间-PID.log`，并在失败后重新打印最早的编译错误上下文。
可以使用 `MOONCAKE_BUILD_LOG=/path/to/build.log` 指定日志位置；同名文件会被覆盖。
日志保存在宿主机，镜像构建失败也可查看。

## Installing build dependencies 下载超时

若日志停在 NumPy 等 wheel 下载，报 `files.pythonhosted.org ... ReadTimeoutError`，
失败发生在 pip 安装构建依赖阶段，尚未执行 Mooncake C++ 编译。

镜像构建默认使用清华 PyPI 镜像 `https://pypi.tuna.tsinghua.edu.cn/simple`，
socket 超时为 600 秒（10 分钟），连接重试次数为 5。可按构建网络覆盖：

```bash
MOONCAKE_PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple \
MOONCAKE_PIP_TIMEOUT=600 MOONCAKE_PIP_RETRIES=8 \
bash build_kvtest_store_image.sh
```

使用内部镜像时替换 `MOONCAKE_PIP_INDEX_URL`；恢复官方源可设置为
`https://pypi.org/simple`。这三个参数由脚本传入 Docker build，再由 `pip_env.sh`
导出为 pip 环境变量，覆盖隔离环境依赖安装、wheel 构建和运行依赖安装。
直接运行 `build_store_ub.sh` 时也有效；未指定 `MOONCAKE_PIP_*` 时，尊重已有的
`PIP_INDEX_URL`、`PIP_TIMEOUT`/`PIP_DEFAULT_TIMEOUT` 和 `PIP_RETRIES`。
连接重试不保证旧版 pip 能续传已经中断的大文件；下载超时仍需检查包源和代理连通性。
保留 pip 隔离构建和 TLS 校验，不使用 `--no-build-isolation` 或 `--trusted-host` 绕过。
重跑时保留 BuildKit/pip/ccache 缓存。下载参数在 wheel 层声明，前面的依赖层可以复用。
包源配置参考[清华镜像说明](https://mirrors.tuna.tsinghua.edu.cn/help/pypi/)，
超时和重试含义参考[pip 官方文档](https://pip.pypa.io/en/stable/cli/pip/)。

### 索引检查成功，但隔离安装报 from versions: none

这条错误本身不能区分索引访问失败和没有兼容包。若同一基础镜像通过代理执行
`pip index versions numpy --no-cache-dir` 能列出版本，而镜像构建仍失败，可执行一次：

```bash
MOONCAKE_PIP_NO_CACHE=1 MOONCAKE_PIP_VERBOSE=2 \
MOONCAKE_PIP_TIMEOUT=600 MOONCAKE_PIP_RETRIES=8 \
MOONCAKE_BUILD_LOG=./pip-diagnostic-build.log \
bash build_kvtest_store_image.sh
```

保留原来的代理配置；若直连 DNS 失败，不要清空 `MOONCAKE_PROXY_URL`。
`MOONCAKE_PIP_NO_CACHE=1` 仅绕过 pip 缓存，不删除缓存文件，也不禁用 ccache。
`MOONCAKE_PIP_VERBOSE=2` 为隔离安装子进程启用详细日志，包含索引请求和候选过滤原因。
脚本先打印实际导出的超时、重试、缓存和详细日志参数。默认两个诊断开关均为 `0`；
直接运行 `build_store_ub.sh` 且未设置开关时，保留已有的 `PIP_NO_CACHE_DIR` 和 `PIP_VERBOSE`。
默认禁用 pip 自身的版本更新检查，避免额外请求干扰诊断。

另开终端实时查看 `tail -F pip-diagnostic-build.log`，失败后提取关键位置：

```bash
grep -nE 'Pip environment:|Could not fetch URL|ReadTimeout|Read timed out|Retrying|Name or service not known|No matching distribution|ERROR:' pip-diagnostic-build.log
```

禁用缓存是对照诊断，不代表已经确认缓存损坏；需根据详细日志中的请求错误或过滤原因继续定位。

### 先下载依赖，再离线安装 Python 包

若 wheel 已匹配成功，却在传输中途 `Read timed out`，可先把依赖准备到当前目录的
`wheelhouse/`，之后构建时仅从本地安装 Python 包：

```bash
bash download_store_wheels.sh
MOONCAKE_PIP_OFFLINE=1 bash build_kvtest_store_image.sh
```

下载脚本使用与构建相同的默认基础镜像，在临时容器内运行 `pip download`，默认使用
现有代理、清华源和 600 秒超时。它只下载 wheel 及传递依赖，不安装到宿主机或修改基础镜像。
`store-wheel-requirements.txt` 对齐固定 Mooncake 源码的构建及运行依赖；更新源码时需同步。
完整下载日志保存在 `store-wheel-download-*.log`，可用 `MOONCAKE_DOWNLOAD_LOG` 指定路径。

已完整下载的包保留在目录中，可重跑脚本补齐；旧版 pip 不保证续传半个 wheel。
若现场网络仍无法完成下载，可在网络正常、具有同一基础镜像的 **ARM64** 机器上运行
下载脚本，再将整个 `wheelhouse/` 拷到构建机脚本目录。也可手动将匹配的 wheel 放进去；
本例 NumPy 需要 `cp311`、`aarch64`，不能使用 Windows 或 x86_64 的 wheel。
可用 `MOONCAKE_PIP_INDEX_URL` 切换下载源；自定义基础镜像和下载目录：

```bash
bash download_store_wheels.sh <基础镜像> /path/to/wheelhouse
# 将该目录下的 wheel 拷贝到构建脚本旁的 wheelhouse/ 后再构建。
MOONCAKE_PIP_OFFLINE=1 bash build_kvtest_store_image.sh <相同基础镜像> <输出镜像标签>
```

构建在 wheel 层只读挂载构建上下文根目录，离线模式从其中的 `wheelhouse/` 读取依赖；
在线模式不要求构建上下文存在 `wheelhouse/`，下载包也不会额外复制进镜像。
`PIP_NO_INDEX=1` 和 `PIP_FIND_LINKS` 会传给隔离构建子进程及运行依赖安装。
缺包时明确失败，不回退到联网下载。单独预装 NumPy 到宿主机或普通容器环境，
不能替代隔离环境需要的依赖。完整目录应包含构建依赖、运行依赖及各自的传递依赖。
恢复在线 pip 安装时去掉 `MOONCAKE_PIP_OFFLINE=1` 即可，下载文件仍保留。

此开关仅控制 pip：基础镜像解析、dnf、Git 和 CMake 自身下载依赖仍可能访问网络。
它不等于整个镜像支持完全离线构建。用法依据 [pip download 官方文档](https://pip.pypa.io/en/stable/cli/pip_download/)。

## target 已退出，URMA 枚举不到设备

### provider 目录为空：只修复运行库层

当设备文件和 sysfs 可见、`urma_admin show` 为 ACTIVE，但独立原生 URMA 程序返回
`count=0`，且容器 `/usr/lib64/urma` 为空、镜像自身 `/usr/lib64` 中有 UDMA/UB 聚合
provider 时，可修复现有镜像的扫描目录，无需重新编译 Mooncake：

```bash
git pull --ff-only
MOONCAKE_URMA_REPAIR_ONLY=1 bash build_kvtest_store_image.sh \
  mooncake-store-kvtest:peer-2026102602 mooncake-store-kvtest:peer-2026102602-urma
python3 diagnose_ub.py --image mooncake-store-kvtest:peer-2026102602-urma
```

第一个镜像参数替换为现场已编译的镜像，第二个使用新标签。只使用输入镜像中的
URMA 核心库和 provider，不读取或打包宿主机 `.so`，也不再使用 `MOONCAKE_URMA_LIB_DIR`。
镜像内 `/usr/lib64/urma` 已有完整 UDMA/UB 聚合 provider 时保留该目录；如果扫描
目录不完整而 `/usr/lib64` 中有完整 provider，则用镜像自身文件补齐扫描目录。
输出 `URMA_PROVIDER_SOURCE=image`，不覆盖 `liburma.so`、`liburma_common.so`。
缺少核心库或完整 provider 时直接报错，需改用具有完整 URMA 的基础镜像。
安装后以立即解析符号的方式加载 provider，缺失依赖或未解析符号会使构建失败。
日志保存在构建日志路径后追加 `.urma.log` 的文件中。

`URMA_PROVIDERS_LOAD_OK` 仅证明动态库可加载。必须再检查诊断日志中的
`NATIVE_URMA count` 大于 0 及实际设备名称，然后重新运行 Store 验证。
将新镜像按 [构建指南](mooncake-build-for-kvtest.md) 分发至全部运行节点，运行时使用 `--mc-image` 指定新标签。原镜像保留用于回退。

正常 `MOONCAKE_USE_UB=ON` 构建也自动应用同一运行库层，默认基础镜像为
`docker.io/library/kvcache_base:4d7a51beffc9`。构建主机无需安装 URMA。
`MOONCAKE_USE_UB=OFF` 的 TCP 构建跳过该运行库层。

### 采集设备和运行库证据

`UB_DEVICE selected=bonding_dev_*` 只说明容器存在分配的字符设备。
`No UB devices found` / `urma_get_device_list failed` 表示原生 URMA 枚举失败；
`mock_urma_device` 是固定 Mooncake 版本在拓扑为空时添加的占位名称，不能据此判断
链接了 mock 库。URMA 初始化成功也不等于 provider 和设备均可用。

失败 Pod 不能 `kubectl exec`；master 未申请 UB 资源，也不能代替 target 检查。
在测试脚本目录执行以下命令，默认读取 `results/` 下最新保存的 `*-target.json`：

```bash
python3 diagnose_ub.py
# 也可以指定某次运行保存的 target 配置：
python3 diagnose_ub.py --manifest results/<run>/<round>/<name>-target.json
# 失败 Pod 尚未被清理时：
python3 diagnose_ub.py --pod <name>-target --namespace mooncake-perf
```

命令创建唯一命名的临时诊断 Pod，复用 target 的镜像、节点、资源、环境和权限，
由设备插件重新分配设备（编号可能变化）。不会启动 Store 或访问 master，不增加特权。
检查设备目录、加载的 URMA 库、provider 的动态链接依赖，并编译运行最小 URMA 枚举
程序；镜像缺少编译器或头文件时明确记录，不把未执行当成通过。
日志保存为当前目录的 `mc-ub-check-*.log`，结束时删除该诊断 Pod。采集完成不代表 UB 已修复。

## 源码下载出现 curl 18 / early EOF

`prepare_source.sh` 中的 `RPC failed; curl 18 Transferred a partial file`、
`early EOF` 表示 Git 下载连接在传输完成前断开，尚未进入 C++ 编译；仅凭此日志不能
区分代理、上游或中间网络故障。

脚本只浅拉取固定提交，不下载整个 main 历史；本次 Git 命令使用 HTTP/1.1，源码与
子模块下载各最多尝试 3 次，失败后分别等待 3 秒、6 秒。仍然失败会返回非零状态，
不会跳过子模块或更换源码版本。已有跟踪文件的修改会阻止切换版本。

更新本分支后，重新执行原来的 `bash build_kvtest_store_image.sh` 即可；保持相同
builder 时，前面成功的依赖层可复用，不需要清缓存或重新安装 gflags。直接运行
`bash prepare_source.sh ./Mooncake` 时，中断后再次执行可以复用同一 Git 仓库中已完成
的对象和子模块；失败的 Docker RUN 层不保证跨构建保留下载进度。

## 构建缓存与耗时

镜像按依赖、源码下载、C++ 编译、运行脚本分层。README、本地配置和结果不复制进镜像；
修改 `store_pod_target.py` 只更新运行层。源码版本、构建脚本、
基础镜像及构建参数变化仍可能让相关层重建，第一次使用新版分层也需要重新建立缓存。
镜像标签变化本身不会要求重编译。Store 构建由官方 scikit-build-core 配置，关闭 examples/benchmarks，
构建 wheel 所需的 `mooncake_master mooncake_client store engine _fast_copy`，再由 pip 安装；
构建日志包含 wheel 的完整路径。

`MOONCAKE_CCACHE=auto` 为默认值：构建镜像尝试安装 ccache，仓库未提供此包时提示后继续
普通编译。`MOONCAKE_CCACHE=1` 要求编译时存在 ccache，否则失败；`MOONCAKE_CCACHE=0`
禁用它。共享配置 `configure_ub.sh` 对 C/C++ 都设置 compiler launcher，禁用时清除旧配置，
以编译器内容校验缓存，不放宽校验或降低 Release 优化。修改脚本导致重建时，ccache 可以
复用未变化的编译结果；它不会消除链接、安装或下载耗时，也不保证每个翻译单元都可缓存。

镜像构建使用 BuildKit cache mount 保存 ccache（默认容量上限 5GiB）和 pip 下载缓存。缓存不进入最终镜像，
失败构建已生成的有效缓存也可以复用。编译完成后输出累计 ccache 统计；整层命中缓存时，
日志显示 `CACHED`，不会重新执行编译或刷新统计。

脚本默认保留 BuildKit 状态到 `~/.cache/mooncake-buildkit-k8s`，重启临时 daemon 不会主动
删除这个目录。保持同一用户、同一 `MOONCAKE_BUILDKIT_ROOT` 和 worker 配置才能复用该缓存；
使用外部 `MOONCAKE_BUILDKIT_HOST` 时由该 builder 管理缓存。GC 或人工清理可能移除缓存。

```bash
# 在相同源码和配置下连续构建，比较完整宿主机日志中的 CACHED 与耗时。
time bash build_kvtest_store_image.sh
time bash build_kvtest_store_image.sh

# 有足够空闲 CPU/内存时，第三个参数可调整并发；默认仍是 8。
time bash build_kvtest_store_image.sh docker.io/library/kvcache_base:4d7a51beffc9 mooncake-store-kvtest:local 16

# 排查编译缓存相关问题时禁用缓存；这不删除已有缓存。
MOONCAKE_CCACHE=0 bash build_kvtest_store_image.sh
```

构建缓存效果以现场完整构建日志为准，不承诺固定加速比例。

直接执行 `build_store_ub.sh` 时，官方打包过程（含隔离环境依赖、CMake 配置和编译）保存在
`build-ub/store-wheel.log`，安装和运行依赖解析分别保存在 `store-install.log` 和 `store-dependencies.log`。
构建依赖以固定源码 `pyproject.toml` 的 `[build-system]` 为准，pip 需要可访问包索引；离线构建需提前准备完整 wheelhouse，并设置
`PIP_NO_INDEX=1 PIP_FIND_LINKS=/path/to/wheelhouse`，下载缓存不能代替完整离线依赖目录。

## real_client.cpp 的 DEFINE_bool / DEFINE_int32 报错

固定源码版本 `89520212377af7114bbba4177021ba092d78ed11` 中，
`mooncake-store/src/real_client.cpp` 使用 gflags 宏但没有显式包含
`<gflags/gflags.h>`。当前 glog 使用 `WITH_GFLAGS=OFF` 构建，不能依赖它间接包含
gflags 头文件。此时即使 gflags 已安装并链接，也会出现以下编译错误：

```text
DEFINE_bool: expected constructor, destructor, or type conversion before '(' token
FLAGS_enable_http_server was not declared in this scope
FLAGS_http_port was not declared in this scope
```

`build_store_ub.sh` 通过 `store_build_compat.cmake` 只为 `real_client.cpp` 增加
`-include gflags/gflags.h`。该修复要求 CMake 3.19 或更新版本，保持原始源码及
固定提交校验不变。它不修改其他源文件的编译选项，也不关闭编译警告。
更新构建脚本后重新运行构建即可，不需要手工编辑 Mooncake 源码。
