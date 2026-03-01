#!/usr/bin/env python3
"""TCP bridge monitor — hex dump of all traffic between host and printer."""

import argparse
import select
import socket
import sys
import time

COLORS = {
    "tx": "\033[36m",   # cyan  — host → printer
    "rx": "\033[33m",   # yellow — printer → host
    "dim": "\033[90m",
    "reset": "\033[0m",
}

SHOW = {"hex": True, "ascii": True, "len": True}

def hex_dump(data, prefix, color):
    t = time.time()
    ts = time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"
    parts = [f"{COLORS['dim']}{ts}{COLORS['reset']}",
             f"{color}{prefix}{COLORS['reset']}"]
    if SHOW["len"]:
        parts.append(f"[{len(data):>4d}]")
    if SHOW["hex"]:
        hx = " ".join(f"{b:02x}" for b in data)
        parts.append(f" {color}{hx}{COLORS['reset']}")
    if SHOW["ascii"]:
        ascii_repr = "".join(chr(b) if 0x20 <= b < 0x7f else "." for b in data)
        parts.append(f" {COLORS['dim']}{ascii_repr}{COLORS['reset']}")
    print("  ".join(parts[:3]) + "".join(parts[3:]))


def connect_bridge(bridge_host, bridge_port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.connect((bridge_host, bridge_port))
    print(f"Connected to bridge {bridge_host}:{bridge_port}")
    return sock


def monitor(bridge_host, bridge_port, listen_port, force=False):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", listen_port))
    srv.listen(1)
    srv.setblocking(False)

    print(f"Bridge monitor listening on :{listen_port}")
    print(f"  bridge target: {bridge_host}:{bridge_port}")
    print(f"  Point your application to: 127.0.0.1:{listen_port}")
    print()
    print(f"  {COLORS['tx']}>>>  host → printer{COLORS['reset']}")
    print(f"  {COLORS['rx']}<<<  printer → host{COLORS['reset']}")
    print()

    bridge = None
    if force:
        bridge = connect_bridge(bridge_host, bridge_port)
        print("Sniffing printer traffic (-f mode)...")
        print("-" * 72)

    client = None
    tx_bytes = 0
    rx_bytes = 0
    tx_pkts = 0
    rx_pkts = 0

    while True:
        socks = []
        if bridge:
            socks.append(bridge)
        if client:
            socks.append(client)
        socks.append(srv)

        readable, _, _ = select.select(socks, [], [], 1.0)

        for sock in readable:
            if sock is srv:
                conn, addr = sock.accept()
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                print(f"\nClient connected: {addr[0]}:{addr[1]}")

                if not bridge:
                    try:
                        bridge = connect_bridge(bridge_host, bridge_port)
                    except (ConnectionRefusedError, OSError) as e:
                        print(f"Cannot connect to bridge: {e}")
                        conn.close()
                        continue

                if client:
                    client.close()
                client = conn
                print("-" * 72)
                continue

            data = None
            try:
                data = sock.recv(4096)
            except OSError:
                pass

            if not data:
                if sock is bridge:
                    print("\nBridge disconnected.")
                    bridge.close()
                    bridge = None
                    if client:
                        client.close()
                        client = None
                elif sock is client:
                    print(f"\nClient disconnected. "
                          f"TX: {tx_pkts} pkt / {tx_bytes} bytes, "
                          f"RX: {rx_pkts} pkt / {rx_bytes} bytes")
                    client.close()
                    client = None
                    tx_bytes = rx_bytes = tx_pkts = rx_pkts = 0
                    if not force and bridge:
                        bridge.close()
                        bridge = None
                continue

            if sock is client:
                if bridge:
                    bridge.sendall(data)
                hex_dump(data, ">>>", COLORS["tx"])
                tx_bytes += len(data)
                tx_pkts += 1
            elif sock is bridge:
                if client:
                    client.sendall(data)
                hex_dump(data, "<<<", COLORS["rx"])
                rx_bytes += len(data)
                rx_pkts += 1


if __name__ == "__main__":
    p = argparse.ArgumentParser(description="TCP-UART bridge hex monitor")
    p.add_argument("-b", "--bridge", default="192.168.3.68",
                   help="Bridge IP (default: 192.168.3.68)")
    p.add_argument("-p", "--port", type=int, default=3333,
                   help="Bridge TCP port (default: 3333)")
    p.add_argument("-l", "--listen", type=int, default=3334,
                   help="Local listen port (default: 3334)")
    p.add_argument("-f", "--force", action="store_true",
                   help="Connect to bridge immediately and sniff traffic")
    p.add_argument("-x", "--hex", action="store_true",
                   help="Show hex only (no ASCII)")
    p.add_argument("-a", "--ascii", action="store_true",
                   help="Show ASCII only (no hex)")
    p.add_argument("-r", "--raw", action="store_true",
                   help="Show raw bytes only (no hex, no ASCII, no length)")
    p.add_argument("--no-len", action="store_true",
                   help="Hide packet length")
    p.add_argument("--no-color", action="store_true",
                   help="Disable colored output")
    args = p.parse_args()

    if args.no_color:
        COLORS = {k: "" for k in COLORS}
    if args.hex and not args.ascii:
        SHOW["ascii"] = False
    if args.ascii and not args.hex:
        SHOW["hex"] = False
    if args.raw:
        SHOW["hex"] = False
        SHOW["ascii"] = False
        SHOW["len"] = False
    if args.no_len:
        SHOW["len"] = False

    try:
        monitor(args.bridge, args.port, args.listen, args.force)
    except KeyboardInterrupt:
        print("\nStopped.")
