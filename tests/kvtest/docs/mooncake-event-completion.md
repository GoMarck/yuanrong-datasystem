# Mooncake RDMA 事件完成模式对照

固定官方 `v0.3.13.post1` 源码、8 MiB、每节点 1 store / 8 client、每 client 35 写 QPS、16 写线程 / 16 读线程、读扇出 10、CPU request 16 / limit 32。只将 SDK 的 `USE_EVENT_DRIVEN_COMPLETION` 从 OFF 改为 ON。这是编译期选项，不能通过运行时 ENV 切换。

目的：验证同步传输忙轮询对 CPU、接口延迟和实际完成读取数的影响。事件模式可以减少调用线程的主动轮询，但不能预先保证延迟或吞吐改善。当前控制服务仍使用 bthread，SDK 的条件变量等待也不是 bthread 原生等待；本轮不同时修改线程池、亲和性、网卡或负载。

以下命令在 Kubernetes master 的 `tests/kvtest` 目录执行，除明确注明的 worker 命令。每段成功后再继续下一段。Mooncake 及 KVTest 均由操作者手工编译。

本分支的原生 DS/MC 运行与统计边界以 [rc44 指南](rc44-mooncake-minimal.md) 为准。

## 1. 更新适配代码

```bash
cd /home/jkx/mooncaketest/yuanrong-datasystem_jkx/tests/kvtest
git pull --ff-only origin codex/kvtest-rc44-mooncake-minimal
```

## 2. 构建独立镜像

使用已编译的普通 RDMA 镜像作为基础，复用相同源码、工具和依赖。构建脚本仍检查固定提交并通过 CMake 构建和安装 wheel。旧镜像标签保持可用。

```bash
BASE_IMAGE=mooncake-store-kvtest:official-v0.3.13.post1-rdma
IMAGE=mooncake-store-kvtest:official-v0.3.13.post1-rdma-event

MOONCAKE_SOURCE_PROFILE=official-v0.3.13.post1 \
MOONCAKE_USE_UB=OFF \
MOONCAKE_USE_TENT=OFF \
MOONCAKE_USE_EVENT_DRIVEN_COMPLETION=ON \
bash tools/mooncake/build_kvtest_store_image.sh "$BASE_IMAGE" "$IMAGE" 8
```

核对镜像内真实 CMake 配置和本机提交：

```bash
nerdctl -n k8s.io run --rm --net=none --entrypoint sh "$IMAGE" -ec '
  cat /opt/Mooncake/build-ub/source_revision.txt
  grep -E "^USE_(EVENT_DRIVEN_COMPLETION|UB|TENT):BOOL=" /opt/Mooncake/build-ub/CMakeCache.txt
'
```

提交应为 `719735896c86b56fabec6cf3e825fb2ea640597a`，事件完成 ON、UB OFF、TENT OFF。构建入口不传事件完成变量时明确传入 OFF，避免已有 CMake 缓存保留之前的 ON。

## 3. 导出 SDK，并重新编译 KVTest

使用新目录，避免混入原 SDK。导出脚本拒绝向配置不同的旧目录覆盖。

```bash
SDK_OUTPUT="$PWD/mooncake-output-v0.3.13.post1-rdma-event"

bash tools/mooncake/export_cpp_output.sh "$IMAGE" "$SDK_OUTPUT"

bash build.sh -b cmake --with-mooncake \
  --mooncake-output "$SDK_OUTPUT" -j 64

grep '^USE_EVENT_DRIVEN_COMPLETION:BOOL=' \
  "$SDK_OUTPUT/Mooncake/build-ub/CMakeCache.txt"

grep '^CXX_DEFINES =' build/CMakeFiles/kvtest.dir/flags.make
```

KVTest 配置应打印 `Mooncake event-driven completion: ON (prebuilt SDK)`，编译定义应含 `-DUSE_EVENT_DRIVEN_COMPLETION`。它由导出 SDK 的缓存自动同步，操作者不用另外添加 CXXFLAGS。该宏影响 SDK 传输头文件中的结构体布局，不能只换静态库而忽略头文件编译配置。旧产物未记录此开关时，保留原先不定义宏的行为，并打印 `not recorded`。

## 4. 将镜像导入两台 worker

master 使用 nerdctl 导出，worker 仅需 ctr：

```bash
nerdctl -n k8s.io save -o /tmp/mooncake-rdma-event.tar "$IMAGE"

scp /tmp/mooncake-rdma-event.tar root@141.62.33.89:/tmp/
scp /tmp/mooncake-rdma-event.tar root@141.62.33.91:/tmp/

ssh root@141.62.33.89 'ctr -n k8s.io images import /tmp/mooncake-rdma-event.tar'
ssh root@141.62.33.91 'ctr -n k8s.io images import /tmp/mooncake-rdma-event.tar'

ssh root@141.62.33.89 'ctr -n k8s.io images list | grep official-v0.3.13.post1-rdma-event'
ssh root@141.62.33.91 'ctr -n k8s.io images list | grep official-v0.3.13.post1-rdma-event'
```

## 5. 释放上一轮 Pod

先对明确指定的上一轮结果目录执行 collect（如尚未采集）和 delete，释放 RDMA 配额及调度资源。结果文件不会被删除。下例的 `OLD_RUN` 必须替换为实际旧目录，不要按时间猜测应删除哪个命名空间。

```bash
OLD_RUN=/path/to/previous-case
python3 deploy_mooncake_case.py collect --output "$OLD_RUN"
python3 deploy_mooncake_case.py delete --output "$OLD_RUN"
```

## 6. 运行事件模式对照

```bash
RUN="$PWD/results/mooncake-event-cpu32-$(date +%Y%m%d-%H%M%S)"

python3 deploy_mooncake_case.py start \
  --output "$RUN" \
  --scenario comparison/scenario.two-rdma-nodes-8clients.json \
  --mc-image "$IMAGE" \
  --duration 60 \
  --client-cpu-limit 32

cp "$SDK_OUTPUT/Mooncake/build-ub/CMakeCache.txt" "$RUN/mooncake/sdk-CMakeCache.txt"
cp build/CMakeFiles/kvtest.dir/flags.make "$RUN/mooncake/kvtest-flags.make"

python3 deploy_mooncake_case.py run --output "$RUN" --monitor --profile-client-cpu
python3 deploy_mooncake_case.py collect --output "$RUN"
```

不附加 gdb，避免暂停 client 干扰测量。比较 `report.html` 的 `put/get_buffer` Avg/P99、实际完成读写数，以及 `mooncake/monitoring/summary.json` 的 CPU/节流和 `mooncake/thread-cpu.json`。CPU 平均包含启动/退出，稳态应结合线程采样或原始 monitoring JSONL。缺失的 RDMA 计数器仍为未采集，不能当成零。

若 CPU 下降、延迟未改善，说明主动轮询解释了 CPU 消耗，但未解释传输慢。若延迟与读取完成数也改善，仍需重复运行并检查相同配置和后台负载。事件模式不是预设的最优配置。

确认不再需要现场后：

```bash
python3 deploy_mooncake_case.py delete --output "$RUN"
```

## 7. 回退到轮询基线

保留原镜像和原 SDK 目录。重新链接原 SDK，CMake 会移除事件宏；新一轮选择旧镜像即可。下面旧 SDK 目录若实际名称不同，以操作者保留的基线为准。

```bash
bash build.sh -b cmake --with-mooncake \
  --mooncake-output "$PWD/mooncake-output-v0.3.13.post1" -j 64

IMAGE=mooncake-store-kvtest:official-v0.3.13.post1-rdma
```

默认开关仍为 OFF，未修改业务数据、存储格式、测试负载或运行中 Pod。

依据：[固定版本完成等待](https://github.com/kvcache-ai/Mooncake/blob/719735896c86b56fabec6cf3e825fb2ea640597a/mooncake-store/src/transfer_task.cpp)、[固定版本构建选项](https://github.com/kvcache-ai/Mooncake/blob/719735896c86b56fabec6cf3e825fb2ea640597a/mooncake-common/common.cmake)。ARM64 镜像构建和真实 RDMA 效果需目标环境验证。
