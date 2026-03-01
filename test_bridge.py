#!/usr/bin/env python3
"""
TCP-UART Bridge latency test.

Usage:
    python3 test_bridge.py [host] [port]

Defaults: host=uart-bridge.local, port=3333
"""

import socket
import time
import sys
import statistics
import os

HOST = sys.argv[1] if len(sys.argv) > 1 else "uart-bridge.local"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3333

TIMEOUT = 2.0


def connect(host, port):
    print(f"Connecting to {host}:{port} ...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.settimeout(5.0)
    try:
        sock.connect((host, port))
    except Exception as e:
        print(f"Connection failed: {e}")
        sys.exit(1)
    print("Connected.\n")
    time.sleep(0.3)
    sock.settimeout(0.3)
    try:
        sock.recv(4096)
    except socket.timeout:
        pass
    return sock


def send_and_measure(sock, gcode):
    """Send G-code, measure time until 'ok' in response."""
    cmd = gcode + "\n"
    sock.sendall(cmd.encode())
    t0 = time.perf_counter()

    buf = b""
    while True:
        elapsed = time.perf_counter() - t0
        if elapsed > TIMEOUT:
            return None, buf.decode(errors="replace")
        sock.settimeout(TIMEOUT - elapsed)
        try:
            chunk = sock.recv(1024)
        except socket.timeout:
            return None, buf.decode(errors="replace")
        if not chunk:
            return None, buf.decode(errors="replace")
        buf += chunk
        if b"ok" in buf:
            rtt = (time.perf_counter() - t0) * 1000
            return rtt, buf.decode(errors="replace")


def test_commands(sock):
    """Round-trip test with various G-code commands."""
    commands = [
        ("M118 P0 ping",  "Echo ping (shortest)"),
        ("M31",           "Print time"),
        ("M105",          "Report temperatures"),
        ("M114",          "Report position"),
        ("M119",          "Endstop status"),
        ("M115",          "Firmware info (long)"),
    ]
    num_rounds = 5

    all_rtts = []

    for round_n in range(1, num_rounds + 1):
        print(f"--- Round {round_n}/{num_rounds} ---")
        for gcode, label in commands:
            rtt, resp = send_and_measure(sock, gcode)
            resp_short = resp.strip().replace("\n", " | ")[:80]
            if rtt is not None:
                all_rtts.append(rtt)
                print(f"  {gcode:20s} ({label:22s})  {rtt:7.1f} ms")
            else:
                print(f"  {gcode:20s} ({label:22s})  TIMEOUT")
            time.sleep(0.05)
        print()

    if all_rtts:
        print("=" * 60)
        print(f"Command test results ({len(all_rtts)} responses):")
        print(f"  Min:    {min(all_rtts):.1f} ms")
        print(f"  Max:    {max(all_rtts):.1f} ms")
        print(f"  Mean:   {statistics.mean(all_rtts):.1f} ms")
        print(f"  Median: {statistics.median(all_rtts):.1f} ms")
        if len(all_rtts) > 1:
            print(f"  Stdev:  {statistics.stdev(all_rtts):.1f} ms")
        print("=" * 60)


def test_sustained(sock, duration_s=60):
    """Sustained ping load test with live histogram."""
    print(f"\n{'=' * 60}")
    print(f"Sustained load test: {duration_s}s of continuous M118 pings")
    print(f"{'=' * 60}\n")

    gcode = "M118 P0 ping"
    rtts = []
    timeouts = 0
    t_start = time.time()

    buckets = [
        (  5, "<5ms  "),
        ( 10, "5-10  "),
        ( 20, "10-20 "),
        ( 50, "20-50 "),
        (100, "50-100"),
        (999, ">100ms"),
    ]
    counts = [0] * len(buckets)

    try:
        cols = os.get_terminal_size().columns
    except OSError:
        cols = 80
    bar_max = cols - 30

    last_print = 0
    while True:
        elapsed = time.time() - t_start
        if elapsed >= duration_s:
            break

        rtt, _ = send_and_measure(sock, gcode)
        if rtt is not None:
            rtts.append(rtt)
            for i, (threshold, _) in enumerate(buckets):
                if rtt < threshold:
                    counts[i] += 1
                    break
        else:
            timeouts += 1

        now = time.time()
        if now - last_print >= 1.0:
            last_print = now
            secs = int(now - t_start)
            total = len(rtts)
            if total == 0:
                continue

            cur_med = statistics.median(rtts[-50:]) if len(rtts) >= 2 else rtts[-1]
            cur_max = max(rtts[-50:])

            sys.stdout.write(f"\r\033[K  [{secs:3d}s] n={total}  "
                             f"med={cur_med:.0f}ms  max={cur_max:.0f}ms  "
                             f"to={timeouts}")
            sys.stdout.flush()

    print("\n")

    if not rtts:
        print("No responses received.")
        return

    print(f"Duration:  {duration_s}s")
    print(f"Requests:  {len(rtts)} ok, {timeouts} timeouts")
    print(f"Rate:      {len(rtts) / duration_s:.1f} req/s")
    print()
    print(f"  Min:     {min(rtts):.1f} ms")
    print(f"  Max:     {max(rtts):.1f} ms")
    print(f"  Mean:    {statistics.mean(rtts):.1f} ms")
    print(f"  Median:  {statistics.median(rtts):.1f} ms")
    print(f"  p95:     {sorted(rtts)[int(len(rtts) * 0.95)]:.1f} ms")
    print(f"  p99:     {sorted(rtts)[int(len(rtts) * 0.99)]:.1f} ms")
    if len(rtts) > 1:
        print(f"  Stdev:   {statistics.stdev(rtts):.1f} ms")
    print()

    peak = max(counts) if max(counts) > 0 else 1
    print("Latency distribution:")
    for i, (threshold, label) in enumerate(buckets):
        c = counts[i]
        pct = c / len(rtts) * 100
        bar_len = int(c / peak * bar_max)
        bar = "█" * bar_len
        print(f"  {label} │{bar} {c} ({pct:.0f}%)")

    print()
    window = 10
    print(f"Timeline (median per {window}s window):")
    chunk_size = max(1, len(rtts) // (duration_s // window))
    row_vals = []
    for i in range(0, len(rtts), chunk_size):
        chunk_rtts = rtts[i:i + chunk_size]
        if chunk_rtts:
            row_vals.append(statistics.median(chunk_rtts))

    if row_vals:
        v_max = max(row_vals)
        v_max = max(v_max, 10)
        graph_w = min(40, bar_max)
        for idx, v in enumerate(row_vals):
            t_label = f"{idx * window:3d}s"
            bar_len = int(v / v_max * graph_w)
            bar = "▓" * bar_len
            print(f"  {t_label} │{bar} {v:.0f}ms")


def main():
    sock = connect(HOST, PORT)

    print("=" * 60)
    print("  Phase 1: Command latency test")
    print("=" * 60 + "\n")
    test_commands(sock)

    print("\n" + "=" * 60)
    print("  Phase 2: Sustained load test (60s)")
    print("=" * 60)
    test_sustained(sock, duration_s=60)

    sock.close()
    print("\nDone.")


if __name__ == "__main__":
    main()
