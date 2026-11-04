#!/usr/bin/env bash
# Source from download/build scripts so Git and CMake use the configured proxy.
proxy_url=${MOONCAKE_PROXY_URL-http://141.61.25.219:3128}
if [[ -z "$proxy_url" ]]; then
    unset ALL_PROXY all_proxy
fi
export http_proxy="$proxy_url" https_proxy="$proxy_url"
export HTTP_PROXY="$proxy_url" HTTPS_PROXY="$proxy_url"

bypass=${no_proxy:-${NO_PROXY:-}}
for host in localhost 127.0.0.1 ::1 local .local \
    141.62.33.73 141.62.33.75 141.62.33.89 141.62.33.91 \
    141.62.33.0/24 141.61.0.0/16 10.0.0.0/8 172.16.0.0/12 192.168.0.0/16; do
    if [[ ",$bypass," != *,"$host",* ]]; then
        bypass="${bypass:+$bypass,}$host"
    fi
done
export no_proxy="$bypass" NO_PROXY="$bypass"
