"""Fake netplay peer for testing the app's NDS local-wireless transport (app/src/main/cpp/netplay.cpp).

    python tools/netplay_peer.py discover HOST[:PORT]
    python tools/netplay_peer.py join HOST[:PORT] PROTOCOL      # act as a client of the app
    python tools/netplay_peer.py host PORT PROTOCOL             # act as a host the app joins

It speaks the wire format only; it doesn't emulate a DS. Data packets it sends are well-formed
melonDS DS packets (8-byte timestamp, aid, type) with a dummy payload.
"""
import socket
import struct
import sys
import time

MAGIC = b"EMNP"
DISCOVER, ANNOUNCE, JOIN, WELCOME, REJECT, DATA, PING, BYE = range(1, 9)
NAMES = {1: "DISCOVER", 2: "ANNOUNCE", 3: "JOIN", 4: "WELCOME", 5: "REJECT", 6: "DATA", 7: "PING", 8: "BYE"}
BROADCAST = 0xFFFF


def packet(kind, src=0, dst=0, payload=b""):
    return MAGIC + struct.pack("<BBHH", 1, kind, src, dst) + payload


def parse(data):
    if len(data) < 10 or data[:4] != MAGIC:
        return None
    _, kind, src, dst = struct.unpack("<BBHH", data[4:10])
    return kind, src, dst, data[10:]


def addr(text, default_port=47000):
    host, _, port = text.partition(":")
    return host, int(port) if port else default_port


def melon_packet(n):
    return struct.pack(">Q", n) + bytes([0, 0]) + b"hello-%d" % n


def recv_for(sock, seconds):
    end = time.time() + seconds
    out = []
    while time.time() < end:
        sock.settimeout(max(0.01, end - time.time()))
        try:
            data, src = sock.recvfrom(65536)
        except socket.timeout:
            break
        p = parse(data)
        if p:
            out.append((p, src))
    return out


def discover(target):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(packet(DISCOVER), addr(target))
    for (kind, _, _, payload), src in recv_for(s, 2):
        if kind == ANNOUNCE:
            print(f"ANNOUNCE from {src}: players={payload[0]}/{payload[1]} title={payload[2:].decode()!r}")
            return True
    print("no ANNOUNCE")
    return False


def join(target, protocol):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    host = addr(target)
    s.sendto(packet(JOIN, payload=protocol.encode()), host)
    my_id = None
    for (kind, src, dst, payload), _ in recv_for(s, 2):
        print("<-", NAMES.get(kind, kind), src, dst, payload[:40])
        if kind == WELCOME:
            my_id = dst
            break
        if kind == REJECT:
            print("rejected:", payload.decode())
            return False
    if my_id is None:
        print("no WELCOME")
        return False
    print("joined as", my_id)
    for n in range(5):
        s.sendto(packet(DATA, my_id, BROADCAST, melon_packet(n)), host)
        s.sendto(packet(PING, my_id, 0), host)
        time.sleep(0.2)
    kinds = [NAMES.get(k, k) for (k, *_), _ in recv_for(s, 1.5)]
    print("received from host:", {k: kinds.count(k) for k in set(kinds)})
    s.sendto(packet(BYE, my_id, 0), host)
    print("sent BYE")
    return True


def host(port, protocol, seconds=20):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    print("hosting on", port)
    client = None
    end = time.time() + seconds
    sent = 0
    stats = {}
    while time.time() < end:
        for (kind, src, dst, payload), frm in recv_for(s, 0.2):
            stats[NAMES.get(kind, kind)] = stats.get(NAMES.get(kind, kind), 0) + 1
            if kind == JOIN:
                if payload.decode() != protocol:
                    print("JOIN with protocol", payload.decode(), "-> REJECT")
                    s.sendto(packet(REJECT, payload="버전 불일치(테스트)".encode()), frm)
                    continue
                client = frm
                s.sendto(packet(WELCOME, 0, 1), frm)
                print("client joined from", frm)
        if client:
            s.sendto(packet(PING, 0, 1), client)
            if sent < 10:
                s.sendto(packet(DATA, 0, BROADCAST, melon_packet(sent)), client)
                sent += 1
    print("received:", stats)
    if client:
        s.sendto(packet(BYE, 0, 1), client)
        print("sent BYE")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "discover":
        discover(sys.argv[2])
    elif cmd == "join":
        join(sys.argv[2], sys.argv[3])
    elif cmd == "host":
        host(int(sys.argv[2]), sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 20)
