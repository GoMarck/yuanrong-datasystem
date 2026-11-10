"""Local Ascend toolkit detection for Bazel builds."""

def _quote(value):
    return "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\""

def _list(values):
    return "[" + ", ".join([_quote(value) for value in values]) + "]"

def _ascend_root(repository_ctx):
    home = repository_ctx.os.environ.get("ASCEND_HOME_PATH")
    if home:
        return home

    custom = repository_ctx.os.environ.get("ASCEND_CUSTOM_PATH")
    if custom:
        return custom + "/latest"

    return "/usr/local/Ascend/ascend-toolkit/latest"

def _parse_hixl_version(value):
    parts = value.strip().split(".")
    if len(parts) < 3:
        return None

    parsed = []
    for part in parts[:3]:
        digits = ""
        for i in range(len(part)):
            char = part[i]
            if char < "0" or char > "9":
                break
            digits += char
        if not digits:
            return None
        parsed.append(int(digits))
    return parsed

def _hixl_version_at_least(version, minimum):
    if version == None:
        return False
    for i in range(3):
        if version[i] > minimum[i]:
            return True
        if version[i] < minimum[i]:
            return False
    return True

def _read_hixl_version(repository_ctx, root):
    version_header = repository_ctx.path(root + "/include/version/hixl_version.h")
    if version_header.exists:
        for line in repository_ctx.read(version_header).splitlines():
            marker = "HIXL_VERSION_STR"
            if marker not in line:
                continue
            fields = line.split("\"")
            if len(fields) >= 2:
                return _parse_hixl_version(fields[1])

    version_info = repository_ctx.path(root + "/share/info/hixl/version.info")
    if version_info.exists:
        for line in repository_ctx.read(version_info).splitlines():
            marker = "Version="
            if line.startswith(marker):
                return _parse_hixl_version(line[len(marker):])

    return None

def _ascend_configure_impl(repository_ctx):
    root = _ascend_root(repository_ctx)
    root_path = repository_ctx.path(root)
    include_dir = repository_ctx.path(root + "/include")
    lib_dir = root + "/lib64"
    lib_dir_path = repository_ctx.path(lib_dir)
    adxl_engine_header = repository_ctx.path(root + "/include/adxl/adxl_engine.h")
    hixl_header = repository_ctx.path(root + "/include/hixl/hixl.h")
    hixl_types_header = repository_ctx.path(root + "/include/hixl/hixl_types.h")
    cann_hixl_lib = repository_ctx.path(root + "/lib64/libcann_hixl.so")
    metadef_lib = repository_ctx.path(root + "/lib64/libmetadef.so")
    llm_datadist_lib = repository_ctx.path(root + "/lib64/libllm_datadist.so")
    hixl_version = _read_hixl_version(repository_ctx, root)
    hixl_hccs_supported = _hixl_version_at_least(hixl_version, [8, 5, 2])

    includes = []
    ascend_linkopts = []
    hccl_linkopts = []
    hixl_linkopts = []
    hixl_plugin_linkopts = []
    defines = []
    hixl_defines = []
    hixl_srcs = []
    hixl_hdrs = []
    hixl_deps = []
    hixl_plugin_targets = []

    if root_path.exists:
        repository_ctx.symlink(root_path, "ascend")

    if include_dir.exists:
        includes.append("ascend/include")
        defines.append("USE_ASCEND")

    if repository_ctx.path(root + "/include/experiment/msprof").exists:
        includes.append("ascend/include/experiment/msprof")

    if repository_ctx.path(root + "/include/experiment/runtime").exists:
        includes.append("ascend/include/experiment/runtime")

    if repository_ctx.path(root + "/pkg_inc/runtime").exists:
        includes.append("ascend/pkg_inc")
        includes.append("ascend/pkg_inc/runtime")
        includes.append("ascend/pkg_inc/runtime/runtime")

    if lib_dir_path.exists:
        ascend_linkopts.extend([
            "-L" + lib_dir,
            "-Wl,-rpath," + lib_dir,
            "-lascendcl",
        ])
        hccl_linkopts.extend([
            "-L" + lib_dir,
            "-Wl,-rpath," + lib_dir,
            "-lhccl",
        ])

    hixl_available = (hixl_header.exists and hixl_types_header.exists and cann_hixl_lib.exists
                      and metadef_lib.exists and hixl_hccs_supported)
    if adxl_engine_header.exists:
        adxl_engine_content = repository_ctx.read(adxl_engine_header)
    else:
        adxl_engine_content = ""
    fabric_mem_available = (hixl_available and llm_datadist_lib.exists and "MallocMem" in adxl_engine_content
                            and "FreeMem" in adxl_engine_content)
    if hixl_available:
        hixl_linkopts.extend([
            "-L" + lib_dir,
            "-Wl,-rpath," + lib_dir,
            "-lcann_hixl",
            "-lmetadef",
        ])
        hixl_plugin_linkopts.extend([
            "-L" + lib_dir,
            "-Wl,--no-as-needed",
            "-lascendcl",
            "-lcann_hixl",
            "-lmetadef",
            "-Wl,--as-needed",
        ])
        hixl_defines.append("ASCEND_HIXL_AVAILABLE")
        hixl_content = repository_ctx.read(hixl_header)
        if "TransferAsync" in hixl_content and "GetTransferStatus" in hixl_content:
            hixl_defines.append("ASCEND_HIXL_ASYNC_AVAILABLE")
        hixl_srcs.append("hixl_transport.cpp")
        hixl_srcs.append("hixl_plugin_loader.cpp")
        hixl_deps.append(":hixl_plugin_sha256_header")
        hixl_hdrs.extend([
            "hixl_plugin_api.h",
            "hixl_plugin_loader.h",
            "hixl_transport.h",
            "hixl_async_executor.h",
        ])
        hixl_plugin_targets.append("//src/datasystem/common/rdma/npu/plugin:libds_hixl_plugin.so")
        if fabric_mem_available:
            hixl_defines.append("ASCEND_HIXL_FABRIC_MEM_AVAILABLE")
            hixl_plugin_linkopts.append("-llm_datadist")

    if fabric_mem_available:
        defines.append("ASCEND_SUPPORT_FABRIC_MEM")

    repository_ctx.file(
        "BUILD.bazel",
        """
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "ascendcl",
    defines = {defines},
    includes = {includes},
    linkopts = {ascend_linkopts},
)

cc_library(
    name = "hccl",
    linkopts = {hccl_linkopts},
    deps = [":ascendcl"],
)

cc_library(
    name = "hixl",
    defines = {hixl_defines},
    linkopts = {hixl_linkopts},
    deps = [":ascendcl"],
)

cc_library(
    name = "hixl_plugin_sdk",
    defines = {hixl_defines},
    hdrs = glob([
        "ascend/include/adxl/**/*.h",
        "ascend/include/adxl/**/*.hpp",
        "ascend/include/hixl/**/*.h",
        "ascend/include/hixl/**/*.hpp",
        "ascend/include/external/ge_common/**/*.h",
        "ascend/include/external/ge_common/**/*.hpp",
        "ascend/include/graph/**/*.h",
        "ascend/include/graph/**/*.hpp",
        "ascend/include/exe_graph/**/*.h",
        "ascend/include/exe_graph/**/*.hpp",
        "ascend/include/utils/**/*.h",
        "ascend/include/utils/**/*.hpp",
    ]),
    includes = {includes},
    linkopts = {hixl_plugin_linkopts},
)
""".format(
            ascend_linkopts = _list(ascend_linkopts),
            defines = _list(defines),
            hccl_linkopts = _list(hccl_linkopts),
            hixl_defines = _list(hixl_defines),
            hixl_linkopts = _list(hixl_linkopts),
            hixl_plugin_linkopts = _list(hixl_plugin_linkopts),
            includes = _list(includes),
        ),
    )
    repository_ctx.file(
        "ascend_config.bzl",
        """
ASCEND_HIXL_SRCS = {hixl_srcs}
ASCEND_HIXL_HDRS = {hixl_hdrs}
ASCEND_HIXL_DEPS = {hixl_deps}
ASCEND_HIXL_DEFINES = {hixl_defines}
ASCEND_HIXL_PLUGIN_TARGETS = {hixl_plugin_targets}
""".format(
            hixl_defines = _list(hixl_defines),
            hixl_deps = _list(hixl_deps),
            hixl_hdrs = _list(hixl_hdrs),
            hixl_plugin_targets = _list(hixl_plugin_targets),
            hixl_srcs = _list(hixl_srcs),
        ),
    )

_ascend_configure = repository_rule(
    implementation = _ascend_configure_impl,
    local = True,
    environ = [
        "ASCEND_CUSTOM_PATH",
        "ASCEND_HOME_PATH",
    ],
)

def ascend_configure(name):
    _ascend_configure(name = name)
