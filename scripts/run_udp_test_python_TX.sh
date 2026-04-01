#!/bin/bash
set -euo pipefail

############################################
# Resolve directories
############################################

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

############################################
# Load configuration
############################################

ENV_FILE="$PROJECT_ROOT/scripts/.env"

if [[ ! -f "$ENV_FILE" ]]; then
    echo "FATAL: .env not found at $ENV_FILE"
    exit 1
fi

set -a
source "$ENV_FILE"
set +a

: "${UDP_PORT:?Missing UDP_PORT}"
: "${UDP_DURATION_SECONDS:?Missing UDP_DURATION_SECONDS}"
: "${UDP_BUFFER_SIZE:?Missing UDP_BUFFER_SIZE}"
: "${UDP_PAYLOAD_SIZE:?Missing UDP_PAYLOAD_SIZE}"
: "${UDP_PPS:?Missing UDP_PPS}"

PORT="$UDP_PORT"
RECV_DURATION="$UDP_DURATION_SECONDS"
SEND_DURATION="$UDP_DURATION_SECONDS"
BUFFER_SIZE="$UDP_BUFFER_SIZE"
PAYLOAD_SIZE="$UDP_PAYLOAD_SIZE"
PPS="$UDP_PPS"

############################################
# Resolve receiver binary
############################################

MAC_BIN="$PROJECT_ROOT/cmake-build-release/kernel-bypass"
LINUX_BIN="$PROJECT_ROOT/kernel-bypass"

if [[ -x "$MAC_BIN" ]]; then
    RECEIVER="$MAC_BIN"
    BUILD_TYPE="CMake Release"
elif [[ -x "$LINUX_BIN" ]]; then
    RECEIVER="$LINUX_BIN"
    BUILD_TYPE="Linux Makefile"
else
    echo "FATAL: No receiver binary found."
    echo "Checked:"
    echo "  $MAC_BIN"
    echo "  $LINUX_BIN"
    exit 1
fi

############################################
# Resolve sender
############################################

SENDER_SCRIPT="$SCRIPT_DIR/send_udp.py"

if [[ ! -f "$SENDER_SCRIPT" ]]; then
    echo "FATAL: Sender script not found at $SENDER_SCRIPT"
    exit 1
fi

############################################
# Cleanup
############################################

cleanup() {
    if [[ -n "${RECV_PID:-}" ]]; then
        kill "$RECV_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

############################################
# Execution
############################################

echo "======================================"
echo "Platform      : $(uname)"
echo "Build type    : $BUILD_TYPE"
echo "Receiver      : $RECEIVER"
echo "Port          : $PORT"
echo "Duration      : $RECV_DURATION"
echo "Buffer size   : $BUFFER_SIZE"
echo "Payload size  : $PAYLOAD_SIZE"
echo "PPS           : $PPS"
echo "======================================"
echo ""

echo "Starting receiver..."

"$RECEIVER" \
  --port "$PORT" \
  --duration "$RECV_DURATION" \
  --buffer "$BUFFER_SIZE" \
  --payload "$PAYLOAD_SIZE" &
RECV_PID=$!

sleep 0.2

if ! kill -0 "$RECV_PID" 2>/dev/null; then
  wait "$RECV_PID" || true
  echo "======================================"
  echo "FATAL: Receiver exited during startup."
  echo "======================================"
  exit 1
fi

echo "Receiver started."
echo ""

############################################
# Start sender
############################################

echo "Starting sender..."

python3 "$SENDER_SCRIPT" \
    --host 127.0.0.1 \
    --port "$PORT" \
    --pps "$PPS" \
    --duration "$SEND_DURATION" \
    --size "$PAYLOAD_SIZE"

echo ""
echo "Waiting for receiver to finish..."

if ! wait "$RECV_PID"; then
    echo "======================================"
    echo "FATAL: Receiver exited with error."
    echo "======================================"
    exit 1
fi

echo ""
echo "Test complete."