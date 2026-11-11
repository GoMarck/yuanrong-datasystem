#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Manual peer test: tcp_probe_test.py HOST:PORT '<probe JSON line>'."""

import json
import socket
import struct
import sys


def receive(peer, size):
    data = bytearray()
    while len(data) < size:
        part = peer.recv(size - len(data))
        assert part, "peer closed before complete response"
        data.extend(part)
    return bytes(data)


def main():
    host, port = sys.argv[1].rsplit(":", 1)
    segment = json.loads(sys.argv[2])
    base, length, token = (segment[k] for k in ("address", "length", "token"))

    def connect():
        return socket.create_connection((host, int(port)), timeout=3)

    def request(op, address, size, key=token, data=b"", expected_status=0):
        with connect() as peer:
            peer.sendall(struct.pack("!5Q", 0x445346414B455542, op, key, address, size))
            if data:
                peer.sendall(data)
            status, = struct.unpack("!Q", receive(peer, 8))
            assert status == expected_status, (status, expected_status)
            return receive(peer, size) if op == 1 and status == 0 else b""

    assert request(1, base, length) == b"Z" * length
    for size in (1, 31, length):
        data = bytes(i % 251 for i in range(size))
        request(2, base, size, data=data)
        assert request(1, base, size) == data
    for address, size, key in ((base, 1, token + 1), (base - 1, 1, token),
                               (base + length, 1, token), (base, 16 * 1024 * 1024 + 1, token)):
        request(1, address, size, key=key, expected_status=1)
    before = request(1, base, length)
    with connect() as peer:
        peer.sendall(b"short header")
    with connect() as peer:
        peer.sendall(struct.pack("!5Q", 0x445346414B455542, 2, token, base, length))
        peer.sendall(b"partial body")
    assert request(1, base, length) == before
    print(json.dumps({"read_write": "pass", "token_range": "pass", "disconnect": "pass"}))


if __name__ == "__main__":
    main()
