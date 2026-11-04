ARG BASE_IMAGE=mooncake-store-kvtest:local
FROM ${BASE_IMAGE}
COPY urma_runtime.py /opt/mooncake-bench/urma_runtime.py
RUN python3 /opt/mooncake-bench/urma_runtime.py install
