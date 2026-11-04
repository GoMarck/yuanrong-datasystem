ARG BASE_IMAGE=docker.io/library/kvcache_base:4d7a51beffc9
FROM ${BASE_IMAGE}

ARG BUILD_JOBS=8
ARG MOONCAKE_USE_UB=ON
ARG MOONCAKE_USE_TENT=OFF
ARG URMA_INCLUDE_DIR=/usr/include/ub/umdk/urma
ARG URMA_LIBRARY=/usr/lib64/liburma.so
ARG MOONCAKE_PROXY_URL=http://141.61.25.219:3128

RUN dnf install -y git make pkgconf-pkg-config numactl-devel \
        openssl-devel zlib-devel yaml-cpp-devel jsoncpp-devel libcurl-devel \
        zstd-devel xxhash-devel boost-devel liburing-devel && \
    (dnf install -y rdma-core-devel || dnf install -y libibverbs-devel) && \
    dnf clean all

# Optional acceleration; unavailable packages must not block a normal build.
RUN (command -v ccache || dnf install -y ccache || \
    echo 'ccache package unavailable; compiler cache will be disabled') && dnf clean all

WORKDIR /opt/mooncake-bench
COPY build_msgpack.sh proxy_env.sh /opt/mooncake-bench/
RUN bash build_msgpack.sh /opt/mooncake-bench/.deps/msgpack-install
ENV CPLUS_INCLUDE_PATH=/opt/mooncake-bench/.deps/msgpack-install/include

# Check directly included Store headers before downloading and compiling Mooncake.
RUN printf '%s\n' \
        '#include <msgpack.hpp>' \
        '#include <boost/functional/hash.hpp>' \
        '#include <zstd.h>' \
        '#include <xxhash.h>' \
        'int main() { msgpack::sbuffer buffer; msgpack::pack(buffer, 1);' \
        'return msgpack::unpack(buffer.data(), buffer.size()).get().as<int>() == 1 ? 0 : 1; }' \
    | g++ -std=c++20 -x c++ -fsyntax-only -

ENV LD_LIBRARY_PATH=/opt/mooncake-bench/.deps/glog-install/lib64:/opt/mooncake-bench/.deps/glog-install/lib:/opt/mooncake-bench/.deps/gflags-install/lib64:/opt/mooncake-bench/.deps/gflags-install/lib:/opt/Mooncake/build-ub/mooncake-store/src:/opt/Mooncake/build-ub/mooncake-transfer-engine/src:/usr/lib64

RUN if [ "${MOONCAKE_USE_UB}" = ON ]; then \
        test -f "${URMA_INCLUDE_DIR}/urma_api.h" && test -f "${URMA_LIBRARY}"; \
    else test "${MOONCAKE_USE_UB}" = OFF; fi
COPY build_glog.sh /opt/mooncake-bench/
RUN bash build_glog.sh /opt/mooncake-bench/.deps/glog-install "${BUILD_JOBS}"
COPY build_gflags.sh /opt/mooncake-bench/
RUN bash build_gflags.sh /opt/mooncake-bench/.deps/gflags-install "${BUILD_JOBS}"
ARG MOONCAKE_SOURCE_PROFILE=legacy
COPY prepare_source.sh source_profile.sh /opt/mooncake-bench/
RUN bash prepare_source.sh /opt/Mooncake
COPY configure_ub.sh build_store_ub.sh build_log.sh pip_env.sh store_build_compat.cmake /opt/mooncake-bench/
ARG MOONCAKE_CCACHE=auto
ARG MOONCAKE_USE_EVENT_DRIVEN_COMPLETION=OFF
ARG MOONCAKE_PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple
ARG MOONCAKE_PIP_TIMEOUT=600
ARG MOONCAKE_PIP_RETRIES=5
ARG MOONCAKE_PIP_NO_CACHE=0
ARG MOONCAKE_PIP_VERBOSE=0
ARG MOONCAKE_PIP_OFFLINE=0
RUN --mount=type=cache,id=mooncake-store-pip,target=/var/cache/mooncake-pip,sharing=locked,mode=0777 \
    --mount=type=cache,id=mooncake-store-ccache,target=/var/cache/mooncake-ccache,sharing=locked,mode=0777 \
    --mount=type=bind,source=.,target=/opt/mooncake-build-context,ro \
    PIP_CACHE_DIR=/var/cache/mooncake-pip \
    MOONCAKE_PIP_WHEELHOUSE=/opt/mooncake-build-context/wheelhouse \
    CCACHE_DIR=/var/cache/mooncake-ccache CCACHE_MAXSIZE=5G \
    MOONCAKE_USE_UB="${MOONCAKE_USE_UB}" MOONCAKE_USE_TENT="${MOONCAKE_USE_TENT}" MOONCAKE_CCACHE="${MOONCAKE_CCACHE}" \
    MOONCAKE_USE_EVENT_DRIVEN_COMPLETION="${MOONCAKE_USE_EVENT_DRIVEN_COMPLETION}" \
    bash build_store_ub.sh /opt/Mooncake "${URMA_INCLUDE_DIR}" \
        "${URMA_LIBRARY}" "${BUILD_JOBS}" && \
    if command -v ccache >/dev/null 2>&1 && [ "${MOONCAKE_CCACHE}" != 0 ]; then \
        CCACHE_DIR=/var/cache/mooncake-ccache ccache --show-stats; fi
RUN test -x /opt/Mooncake/build-ub/mooncake-store/src/mooncake_master && \
    python3 -m pip show mooncake-transfer-engine && \
    python3 -c 'from mooncake.store import MooncakeDistributedStore'

# Runtime-only changes reuse the compiled Mooncake image layers.
COPY store_pod_target.py /opt/mooncake-bench/

CMD ["/bin/sh", "-c", "sleep infinity"]
