import argparse
import signal
import socket
import sys
import time
from pathlib import Path

stop_requested = False


def handle_signal(signum, frame):
    global stop_requested
    stop_requested = True


def load_env_file(env_path):
    """
    Minimal .env parser:
    - ignores empty lines
    - ignores lines starting with '#'
    - supports KEY=VALUE
    - strips surrounding quotes
    """
    data = {}

    path = Path(env_path)
    if not path.exists():
        return data

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()

        if not line or line.startswith("#"):
            continue

        if "=" not in line:
            continue

        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")

        data[key] = value

    return data


def pick_env_file(explicit_path=None):
    """
    Search order:
    1. explicit --env-file
    2. udp_test.env
    3. .env
    """
    if explicit_path:
        return explicit_path

    for candidate in ("udp_test.env", ".env"):
        if Path(candidate).exists():
            return candidate

    return None


def env_get_str(env, key, default=None):
    return env.get(key, default)


def env_get_int(env, key, default=None):
    value = env.get(key)
    if value is None or value == "":
        return default
    return int(value)


def env_get_float(env, key, default=None):
    value = env.get(key)
    if value is None or value == "":
        return default
    return float(value)


def parse_args():
    pre_parser = argparse.ArgumentParser(add_help=False)
    pre_parser.add_argument(
        "--env-file",
        default=None,
        help="Path to .env file (default: udp_test.env, then .env if present)",
    )
    pre_args, _ = pre_parser.parse_known_args()

    env_file = pick_env_file(pre_args.env_file)
    env = load_env_file(env_file) if env_file else {}

    parser = argparse.ArgumentParser(
        description="Simple and robust UDP traffic generator"
    )

    parser.add_argument(
        "--env-file",
        default=env_file,
        help="Path to .env file",
    )

    parser.add_argument(
        "--host",
        default=env_get_str(env, "REMOTE_HOST_IP"),
        help="Destination IP or hostname (default: REMOTE_HOST_IP from .env)",
    )

    parser.add_argument(
        "--port",
        type=int,
        default=env_get_int(env, "UDP_PORT", 9000),
        help="Destination UDP port (default: UDP_PORT from .env or 9000)",
    )

    parser.add_argument(
        "--size",
        type=int,
        default=env_get_int(env, "UDP_PAYLOAD_SIZE", 512),
        help="Payload size in bytes (default: UDP_PAYLOAD_SIZE from .env or 512)",
    )

    parser.add_argument(
        "--duration",
        type=float,
        default=env_get_float(env, "UDP_DURATION_SECONDS", 10.0),
        help="Send duration in seconds (default: UDP_DURATION_SECONDS from .env or 10)",
    )

    parser.add_argument(
        "--count",
        type=int,
        default=0,
        help="Number of packets to send (overrides duration if > 0)",
    )

    parser.add_argument(
        "--pps",
        type=float,
        default=env_get_float(env, "UDP_PPS", 0.0),
        help="Packets per second limit (default: UDP_PPS from .env or unlimited)",
    )

    parser.add_argument(
        "--connect",
        action="store_true",
        help="Use connected UDP socket for slightly cleaner error handling",
    )

    parser.add_argument(
        "--report-interval",
        type=float,
        default=1.0,
        help="Stats reporting interval in seconds (default: 1.0)",
    )

    args = parser.parse_args()

    if not args.host:
        parser.error(
            "host is required (provide --host or set REMOTE_HOST_IP in the .env file)"
        )

    if not (1 <= args.port <= 65535):
        parser.error("port must be between 1 and 65535")

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

    payload = b"x" * args.size

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    except OSError as e:
        print(f"socket creation failed: {e}", file=sys.stderr)
        return 1

    try:
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

        if args.count > 0:
            target_packets = args.count
            end_time = None
        else:
            target_packets = None
            end_time = start + args.duration

        print(
            f"Sending UDP to {args.host}:{args.port} "
            f"size={args.size} "
            f"{'count=' + str(args.count) if args.count > 0 else 'duration=' + str(args.duration)} "
            f"pps={'unlimited' if args.pps == 0 else args.pps} "
            f"env_file={args.env_file if args.env_file else 'none'}"
        )

        while not stop_requested:
            now = time.monotonic()

            if target_packets is not None:
                if sent_packets >= target_packets:
                    break
            else:
                if now >= end_time:
                    break

            if args.pps > 0 and now < next_send_time:
                time.sleep(min(next_send_time - now, 0.01))
                continue

            try:
                if args.connect:
                    n = sock.send(payload)
                else:
                    n = sock.sendto(payload, (args.host, args.port))
            except OSError as e:
                errors += 1
                time.sleep(0.01)
                print(f"send error: {e}", file=sys.stderr)
                continue

            sent_packets += 1
            sent_bytes += n

            if args.pps > 0:
                next_send_time += 1.0 / args.pps
                if next_send_time < now - 1.0:
                    next_send_time = now

            if now >= next_report:
                elapsed = now - start
                pps = sent_packets / elapsed if elapsed > 0 else 0.0
                mbps = (sent_bytes * 8.0) / elapsed / 1e6 if elapsed > 0 else 0.0

                print(
                    f"[running] elapsed={elapsed:.3f}s "
                    f"packets={sent_packets} "
                    f"bytes={sent_bytes} "
                    f"pps={pps:.0f} "
                    f"Mb/s={mbps:.3f} "
                    f"errors={errors}"
                )
                next_report += args.report_interval

        finish = time.monotonic()
        elapsed = finish - start
        pps = sent_packets / elapsed if elapsed > 0 else 0.0
        mbps = (sent_bytes * 8.0) / elapsed / 1e6 if elapsed > 0 else 0.0

        print("\n=== FINAL STATS ===")
        print(f"elapsed_seconds : {elapsed:.6f}")
        print(f"packets         : {sent_packets}")
        print(f"bytes           : {sent_bytes}")
        print(f"packets/sec     : {pps:.0f}")
        print(f"Mb/s            : {mbps:.3f}")
        print(f"errors          : {errors}")

        return 0

    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())

