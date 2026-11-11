/**
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "datasystem/common/urma_mock/transport/tcp_endpoint_service.h"

#include <arpa/inet.h>
#include <endian.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "datasystem/common/log/log.h"
#include "datasystem/common/urma_mock/mock_registry.h"
#include "datasystem/common/urma_mock/objects/mock_seg.h"

namespace datasystem {
namespace urma_mock {
namespace {
constexpr uint64_t K_MAGIC = 0x445346414b455542;
constexpr size_t K_MAX_TRANSFER = 16 * 1024 * 1024;
constexpr int K_TIMEOUT_MS = 1000;
constexpr std::array<uint8_t, 4> K_EID_MAGIC{ 'F', 'U', 'B', 1 };

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) { ::close(fd_); } }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    int Get() const { return fd_; }
private:
    int fd_;
};

bool Io(int fd, void *buffer, size_t size, bool send)
{
    auto *bytes = static_cast<uint8_t *>(buffer);
    while (size != 0) {
        ssize_t n = send ? ::send(fd, bytes, size, MSG_NOSIGNAL) : ::recv(fd, bytes, size, 0);
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { return false; }
        bytes += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void SetTimeouts(int fd)
{
    timeval timeout{ K_TIMEOUT_MS / 1000, (K_TIMEOUT_MS % 1000) * 1000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool ParseEndpoint(const char *value, sockaddr_in &address)
{
    if (value == nullptr) { return false; }
    std::string text(value);
    auto split = text.rfind(':');
    if (split == std::string::npos) { return false; }
    unsigned int port = 0;
    auto parsed = std::from_chars(text.data() + split + 1, text.data() + text.size(), port);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || port == 0 || port > 65535) {
        return false;
    }
    address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    return ::inet_pton(AF_INET, text.substr(0, split).c_str(), &address.sin_addr) == 1
           && address.sin_addr.s_addr != INADDR_ANY;
}

bool Connect(int fd, const sockaddr_in &address)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) { return false; }
    int result = ::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    if (result != 0) {
        if (errno != EINPROGRESS) { return false; }
        pollfd event{ fd, POLLOUT, 0 };
        if (::poll(&event, 1, K_TIMEOUT_MS) <= 0) { return false; }
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) { return false; }
    }
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

std::shared_ptr<MockSeg> FindSegment(uint64_t token, uint64_t address, uint64_t size, uint64_t &offset)
{
    auto &tables = Tables();
    std::lock_guard<std::mutex> lock(tables.mu);
    for (const auto &[raw, seg] : tables.tseg) {
        if (seg == nullptr || seg->IsRemote() || raw->seg.token_id != token) { continue; }
        const uint64_t base = raw->seg.ubva.va;
        if (address < base || address - base > seg->GetSize()) { continue; }
        offset = address - base;
        if (size <= seg->GetSize() - offset) { return seg; }
    }
    return nullptr;
}

bool FileIo(const MockSeg &seg, uint64_t offset, std::vector<uint8_t> &data, bool read)
{
    int fd = seg.GetMemfdFd() >= 0 ? seg.GetMemfdFd() : seg.GetShmFd();
    const uint64_t maxOffset = static_cast<uint64_t>(std::numeric_limits<off_t>::max());
    if (fd < 0 || offset > maxOffset || seg.GetMemfdOffset() > maxOffset - offset) { return false; }
    offset += seg.GetMemfdOffset();
    if (data.size() > maxOffset - offset) { return false; }
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = read ? ::pread(fd, data.data() + done, data.size() - done, static_cast<off_t>(offset + done))
                         : ::pwrite(fd, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { return false; }
        done += static_cast<size_t>(n);
    }
    return true;
}

void Serve(int fd)
{
    SetTimeouts(fd);
    std::array<uint64_t, 5> request{};
    if (!Io(fd, request.data(), sizeof(request), false)) { return; }
    for (auto &word : request) { word = be64toh(word); }
    uint64_t status = htobe64(1);
    uint64_t offset = 0;
    bool valid = request[0] == K_MAGIC && (request[1] == 1 || request[1] == 2)
                 && request[4] > 0 && request[4] <= K_MAX_TRANSFER;
    auto segment = valid ? FindSegment(request[2], request[3], request[4], offset) : nullptr;
    if (segment == nullptr) { Io(fd, &status, sizeof(status), true); return; }
    std::vector<uint8_t> data(static_cast<size_t>(request[4]));
    const bool read = request[1] == 1;
    if (!read && !Io(fd, data.data(), data.size(), false)) { return; }
    if (!FileIo(*segment, offset, data, read)) { Io(fd, &status, sizeof(status), true); return; }
    status = 0;
    if (Io(fd, &status, sizeof(status), true) && read) { Io(fd, data.data(), data.size(), true); }
}

class Listener {
public:
    bool Start(const sockaddr_in &address)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fd_.load() >= 0) {
            return owner_ == ::getpid() && address.sin_addr.s_addr == address_.sin_addr.s_addr
                   && address.sin_port == address_.sin_port;
        }
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) { return false; }
        int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || ::listen(fd, 32) != 0) {
            ::close(fd);
            return false;
        }
        fd_.store(fd);
        owner_ = ::getpid();
        address_ = address;
        try {
            thread_ = std::thread([this, fd]() {
                while (fd_.load() == fd) {
                    Socket peer(::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC));
                    if (peer.Get() < 0) {
                        if (errno == EINTR) { continue; }
                        break;
                    }
                    try { Serve(peer.Get()); }
                    catch (const std::exception &error) { LOG(WARNING) << "[MockUrma-TCP] " << error.what(); }
                }
            });
        } catch (...) {
            fd_.store(-1);
            ::close(fd);
            return false;
        }
        return true;
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (owner_ != ::getpid()) { return; }
        int fd = fd_.exchange(-1);
        if (fd < 0) { return; }
        ::shutdown(fd, SHUT_RDWR);
        if (thread_.joinable()) { thread_.join(); }
        ::close(fd);
    }
private:
    std::mutex mutex_;
    std::atomic<int> fd_{ -1 };
    pid_t owner_ = 0;
    sockaddr_in address_{};
    std::thread thread_;
};

Listener &Service()
{
    static auto *listener = new Listener;
    return *listener;
}
}  // namespace

bool TcpEndpointService::ConfigureSegment(urma_seg_t &seg)
{
    const char *endpoint = ::getenv("URMA_MOCK_TCP_ENDPOINT");
    if (endpoint == nullptr || *endpoint == '\0') { return true; }
    sockaddr_in address{};
    if (!ParseEndpoint(endpoint, address) || !Service().Start(address)) {
        LOG(ERROR) << "[MockUrma-TCP] cannot start " << endpoint;
        return false;
    }
    std::memset(seg.ubva.eid.raw, 0, sizeof(seg.ubva.eid.raw));
    std::memcpy(seg.ubva.eid.raw, K_EID_MAGIC.data(), K_EID_MAGIC.size());
    std::memcpy(seg.ubva.eid.raw + 4, &address.sin_addr, sizeof(address.sin_addr));
    std::memcpy(seg.ubva.eid.raw + 8, &address.sin_port, sizeof(address.sin_port));
    return true;
}

bool TcpEndpointService::DecodeEndpoint(const urma_seg_t &seg, ImportEndpoint &endpoint)
{
    if (std::memcmp(seg.ubva.eid.raw, K_EID_MAGIC.data(), K_EID_MAGIC.size()) != 0) { return false; }
    in_addr address{};
    uint16_t port = 0;
    std::memcpy(&address, seg.ubva.eid.raw + 4, sizeof(address));
    std::memcpy(&port, seg.ubva.eid.raw + 8, sizeof(port));
    char host[INET_ADDRSTRLEN]{};
    if (::inet_ntop(AF_INET, &address, host, sizeof(host)) == nullptr || port == 0) { return false; }
    endpoint = {};
    endpoint.host = host;
    endpoint.port = ntohs(port);
    endpoint.va = seg.ubva.va;
    endpoint.len = seg.len;
    endpoint.tcp = true;
    return true;
}

bool TcpEndpointService::Transfer(const MockSeg &seg, uint64_t offset, void *data, size_t size, bool read)
{
    if (data == nullptr || size == 0 || size > K_MAX_TRANSFER || offset > seg.GetSize()
        || size > seg.GetSize() - offset || seg.GetRemoteVa() > UINT64_MAX - offset) { return false; }
    sockaddr_in address{};
    auto endpoint = seg.GetRemoteHost() + ":" + std::to_string(seg.GetRemotePort());
    if (!ParseEndpoint(endpoint.c_str(), address)) { return false; }
    uint64_t token = 0;
    const auto &key = seg.GetKey();
    auto parsed = std::from_chars(key.data(), key.data() + key.size(), token);
    if (parsed.ec != std::errc() || parsed.ptr != key.data() + key.size()) { return false; }
    Socket socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (socket.Get() < 0 || !Connect(socket.Get(), address)) { return false; }
    SetTimeouts(socket.Get());
    std::array<uint64_t, 5> request{ K_MAGIC, read ? 1U : 2U, token, seg.GetRemoteVa() + offset, size };
    for (auto &word : request) { word = htobe64(word); }
    if (!Io(socket.Get(), request.data(), sizeof(request), true)
        || (!read && !Io(socket.Get(), data, size, true))) { return false; }
    uint64_t status = 1;
    return Io(socket.Get(), &status, sizeof(status), false) && be64toh(status) == 0
           && (!read || Io(socket.Get(), data, size, false));
}

void TcpEndpointService::Shutdown()
{
    Service().Stop();
}
}  // namespace urma_mock
}  // namespace datasystem
