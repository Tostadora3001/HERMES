#!/usr/bin/env python3
"""The test network's servers (hermes_net), all on 127.0.0.1. Test only.
Usage: echo_server.py PORT [SECONDS] (it exits after SECONDS, default 120,
so a test can never leave it behind). It prints "ready" on stdout once
everything listens.

- PORT, TCP and UDP: echo -- what it receives, it sends back.
- 47067 UDP: a DHCP server (the virtual W5500 moves port 67 there, see
  modules/w5500/w5500_virtual.c): offers 10.0.0.50/24, router 10.0.0.1,
  DNS 127.0.0.1, a lease of 3600 s; answers to the sender's address.
- 47053 UDP: a DNS server (port 53 moved): "localhost" -> 127.0.0.1, any
  other name: "no such name".
- a client of the program's own server on PORT + 3: it tries to connect
  every 50 ms; once in, it sends "hola", reads what comes back until the
  program closes, and stops."""
import socket, struct, sys, threading, time

port = int(sys.argv[1])
life = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0

def tcp_client(c):
    with c:
        while True:
            data = c.recv(2048)
            if not data:
                return
            c.sendall(data)

def tcp():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(8)
    while True:
        c, _ = s.accept()
        threading.Thread(target=tcp_client, args=(c,), daemon=True).start()

def udp(u):
    while True:
        data, addr = u.recvfrom(2048)
        u.sendto(data, addr)

def dhcp():
    d = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    d.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    d.bind(("127.0.0.1", 47067))
    while True:
        msg, addr = d.recvfrom(2048)
        if len(msg) < 240 or msg[0] != 1 or msg[236:240] != b"\x63\x82\x53\x63":
            continue
        opts, kind, i = msg[240:], 0, 0
        while i < len(opts) and opts[i] != 255:
            if opts[i] == 0:
                i += 1
                continue
            if opts[i] == 53:
                kind = opts[i + 2]
            i += 2 + opts[i + 1]
        answer = {1: 2, 3: 5}.get(kind)  # DISCOVER -> OFFER, REQUEST -> ACK
        if not answer:
            continue
        reply = bytearray(240)
        reply[0:4] = b"\x02\x01\x06\x00"
        reply[4:8] = msg[4:8]                     # xid
        reply[10:12] = msg[10:12]                 # flags
        reply[16:20] = bytes([10, 0, 0, 50])      # yiaddr
        reply[28:44] = msg[28:44]                 # chaddr
        reply[236:240] = b"\x63\x82\x53\x63"
        reply += bytes([53, 1, answer, 54, 4, 10, 0, 0, 1, 51, 4]) + struct.pack(">I", 3600)
        reply += bytes([1, 4, 255, 255, 255, 0, 3, 4, 10, 0, 0, 1, 6, 4, 127, 0, 0, 1, 255])
        d.sendto(bytes(reply), addr)

def dns():
    d = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    d.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    d.bind(("127.0.0.1", 47053))
    while True:
        msg, addr = d.recvfrom(2048)
        if len(msg) < 17:
            continue
        labels, i = [], 12
        while i < len(msg) and msg[i]:
            labels.append(msg[i + 1:i + 1 + msg[i]].decode("ascii", "replace"))
            i += 1 + msg[i]
        question = msg[12:i + 5]
        known = ".".join(labels).lower() == "localhost"
        head = msg[0:2] + bytes([0x81, 0x80 if known else 0x83]) + struct.pack(">HHHH", 1, 1 if known else 0, 0, 0)
        answer = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + bytes([127, 0, 0, 1]) if known else b""
        d.sendto(head + question + answer, addr)

def client():
    while True:
        try:
            c = socket.create_connection(("127.0.0.1", port + 3), timeout=5)
        except OSError:
            time.sleep(0.05)
            continue
        with c:
            c.sendall(b"hola")
            try:
                while c.recv(2048):
                    pass
            except OSError:
                pass
        return

for f in (dhcp, dns, client):
    threading.Thread(target=f, daemon=True).start()

u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
u.bind(("127.0.0.1", port))
threading.Thread(target=tcp, daemon=True).start()
threading.Thread(target=udp, args=(u,), daemon=True).start()
time.sleep(0.2)
print("ready", flush=True)
time.sleep(life)
