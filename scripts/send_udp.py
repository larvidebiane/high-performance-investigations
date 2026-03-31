import argparse
import signal
import socket
import sys
import time


stop_requested = False


def handle_signal(signum, frame):
    global stop_requested
    stop_requested = True


def parse_args():
    parser = argparse.ArgumentParser(
        description="Deterministic UDP traffic generator"
    )

    parser.add_argument("--host", required=True, help="Destination host")
    parser.add_argument("--port", type=int, required=True, help="Destination port")
    parser.add_argument("--size", type=int, default=512, help="Payload size (bytes)")
    parser.add_argument("--duration", type=float, default=10.0, help="Send duration (seconds)")
    parser.add_argument("--count", type=int, default=0, help="Number of packets (overrides duration)")
    parser.add_argument("--pps", type=float, default=0.0, help="Packets per second (0 = unlimited)")
    parser.add_argument("--connect", action="store_true", help="Use connected UDP socket")
    parser.add_argument("--report-interval", type=float, default=1.0)

    args = parser.parse_args()

    if not (1 <= args.port <= 65535):
        parser.error("port must be 1–65535")

    if args.size <= 0:
        parser.error("size must be > 0")

    if args.duration <= 0 and args.count <= 0:
        parser.error("duration must be > 0 unless count > 0")

    if args.pps < 0:
        parser.error("pps must be >= 0")

    if args.report_interval <= 0:
        parser.error("report-interval must be > 0")

    return args


def main():
    global stop_requested

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    args = parse_args()

    print("=== Effective Configuration ===")
    print(f"host            : {args.host}")
    print(f"port            : {args.port}")
    print(f"payload_size    : {args.size}")
    print(f"duration        : {args.duration}")
    print(f"count           : {args.count}")
    print(f"pps             : {args.pps}")
    print("================================\n")

    payload = b"x" * args.size
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    except OSError:
        pass

    if args.connect:
        sock.connect((args.host, args.port))

    start = time.monotonic()
    next_report = start + args.report_interval
    next_send_time = start

    sent_packets = 0
    sent_bytes = 0
    errors = 0

    end_time = None if args.count > 0 else start + args.duration

    while not stop_requested:
        now = time.monotonic()

        if args.count > 0:
            if sent_packets >= args.count:
                break
        else:
            if now >= end_time:
                break

        if args.pps > 0 and now < next_send_time:
            time.sleep(min(next_send_time - now, 0.001))
            continue

        try:
            if args.connect:
                n = sock.send(payload)
            else:
                n = sock.sendto(payload, (args.host, args.port))
        except OSError:
            errors += 1
            continue

        sent_packets += 1
        sent_bytes += n

        if args.pps > 0:
            next_send_time += 1.0 / args.pps
            if next_send_time < now - 1.0:
                next_send_time = now

        if now >= next_report:
            elapsed = now - start
            print(
                f"[client-running] elapsed={elapsed:.3f}s "
                f"packets={sent_packets} "
                f"bytes={sent_bytes} "
                f"pps={sent_packets/elapsed:.0f} "
                f"Mb/s={(sent_bytes*8/elapsed)/1e6:.3f} "
                f"errors={errors}"
            )
            next_report += args.report_interval

    elapsed = time.monotonic() - start

    print("\n=== FINAL CLIENT STATS ===")
    print(f"elapsed_seconds : {elapsed:.6f}")
    print(f"packets         : {sent_packets}")
    print(f"bytes           : {sent_bytes}")
    print(f"packets/sec     : {sent_packets/elapsed:.0f}")
    print(f"Mb/s            : {(sent_bytes*8/elapsed)/1e6:.3f}")
    print(f"errors          : {errors}")

    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())