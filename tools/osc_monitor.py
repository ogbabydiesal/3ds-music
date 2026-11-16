#!/usr/bin/env python3
"""Tiny OSC-over-UDP monitor (no dependencies). Prints every message osc3ds sends.

    python3 tools/osc_monitor.py            # listens on 0.0.0.0:9000
    python3 tools/osc_monitor.py 8000
"""
import socket
import struct
import sys


def read_str(data, i):
    end = data.index(b"\0", i)
    s = data[i:end].decode("utf-8", "replace")
    return s, (end + 4) & ~3


def decode(data):
    addr, i = read_str(data, 0)
    tags, i = read_str(data, i)
    args = []
    for t in tags[1:]:
        if t == "f":
            args.append(round(struct.unpack(">f", data[i:i + 4])[0], 4)); i += 4
        elif t == "i":
            args.append(struct.unpack(">i", data[i:i + 4])[0]); i += 4
        elif t == "s":
            s, i = read_str(data, i); args.append(s)
    return addr, args


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"Listening for OSC on UDP {port} ... (Ctrl+C to stop)")
    while True:
        data, (host, _) = sock.recvfrom(2048)
        try:
            addr, args = decode(data)
            print(f"{host:>15}  {addr:<24} {' '.join(map(str, args))}", flush=True)
        except Exception as e:  # malformed packet
            print(f"{host:>15}  <bad packet: {e}>")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
