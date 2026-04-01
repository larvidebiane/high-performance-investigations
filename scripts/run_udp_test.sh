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

# Keep RX alive slightly longer than TX.
# Why:
# - TX uses a connected UDP socket (--connect).
# - If RX closes first, TX can see ECONNREFUSED on the last packets.
# - Letting RX outlive TX avoids artificial end-of-test send errors.
RECV_DURATION=$((UDP_DURATION_SECONDS + 1))
SEND_DURATION="$UDP_DURATION_SECONDS"

BUFFER_SIZE="$UDP_BUFFER_SIZE"
PAYLOAD_SIZE="$UDP_PAYLOAD_SIZE"
PPS="$UDP_PPS"

############################################
# Resolve receiver binary
############################################

MAC_RECV_BIN="$PROJECT_ROOT/cmake-build-release/kernel-bypass"
LINUX_RECV_BIN="$PROJECT_ROOT/kernel-bypass"

if [[ -x "$MAC_RECV_BIN" ]]; then
    RECEIVER="$MAC_RECV_BIN"
    RECEIVER_BUILD_TYPE="CMake Release"
elif [[ -x "$LINUX_RECV_BIN" ]]; then
    RECEIVER="$LINUX_RECV_BIN"
    RECEIVER_BUILD_TYPE="Linux Makefile"
else
    echo "FATAL: No receiver binary found."
    echo "Checked:"
    echo "  $MAC_RECV_BIN"
    echo "  $LINUX_RECV_BIN"
    exit 1
fi

############################################
# Resolve sender binary
############################################

MAC_SEND_BIN="$PROJECT_ROOT/cmake-build-release/send_udp"
LINUX_SEND_BIN="$PROJECT_ROOT/send_udp"

if [[ -x "$MAC_SEND_BIN" ]]; then
    SENDER="$MAC_SEND_BIN"
    SENDER_BUILD_TYPE="CMake Release"
elif [[ -x "$LINUX_SEND_BIN" ]]; then
    SENDER="$LINUX_SEND_BIN"
    SENDER_BUILD_TYPE="Linux Makefile"
else
    echo "FATAL: No sender binary found."
    echo "Checked:"
    echo "  $MAC_SEND_BIN"
    echo "  $LINUX_SEND_BIN"
    exit 1
fi

############################################
# Cleanup
############################################

cleanup() {
    if [[ -n "${RECV_PID:-}" ]]; then
        kill "$RECV_PID" 2>/dev/null || true
    fi

    if [[ -n "${READY_LOG:-}" && -f "$READY_LOG" ]]; then
        rm -f "$READY_LOG"
    fi
}
trap cleanup EXIT

############################################
# Execution
############################################

echo "======================================"
echo "Platform        : $(uname)"
echo "Receiver build  : $RECEIVER_BUILD_TYPE"
echo "Receiver        : $RECEIVER"
echo "Sender build    : $SENDER_BUILD_TYPE"
echo "Sender          : $SENDER"
echo "Port            : $PORT"
echo "Duration        : $RECV_DURATION"
echo "Buffer size     : $BUFFER_SIZE"
echo "Payload size    : $PAYLOAD_SIZE"
echo "PPS             : $PPS"
echo "======================================"
echo ""

############################################
# Start receiver and wait for READY
############################################

echo "Starting receiver..."

READY_LOG="$(mktemp)"
READY_TIMEOUT_SECONDS=5

# Receiver stdout stays attached to the terminal:
# [RX-running] lines are shown live.
#
# Receiver stderr goes to READY_LOG:
# it is used only for READY / startup diagnostics.
"$RECEIVER" \
  --port "$PORT" \
  --duration "$RECV_DURATION" \
  --buffer "$BUFFER_SIZE" \
  --payload "$PAYLOAD_SIZE" \
  2>"$READY_LOG" &
RECV_PID=$!

START_TIME="$(date +%s)"

while true; do
    if grep -Fq "RX READY port=$PORT" "$READY_LOG"; then
        break
    fi

    if ! kill -0 "$RECV_PID" 2>/dev/null; then
        wait "$RECV_PID" || true
        echo "======================================"
        echo "FATAL: Receiver exited during startup."
        echo "READY/error output:"
        cat "$READY_LOG"
        echo "======================================"
        exit 1
    fi

    NOW="$(date +%s)"
    if (( NOW - START_TIME >= READY_TIMEOUT_SECONDS )); then
        echo "======================================"
        echo "FATAL: Timeout waiting for receiver READY."
        echo "READY/error output so far:"
        cat "$READY_LOG"
        echo "======================================"
        exit 1
    fi

    sleep 0.01
done

# Optional: show the READY line once it has been detected.
cat "$READY_LOG"

echo "Receiver started."
echo ""

############################################
# Start sender
############################################

echo "Starting sender..."

"$SENDER" \
    --host 127.0.0.1 \
    --port "$PORT" \
    --pps "$PPS" \
    --duration "$SEND_DURATION" \
    --size "$PAYLOAD_SIZE" \
    --connect

echo ""
echo "Waiting for receiver to finish..."

if ! wait "$RECV_PID"; then
    echo "======================================"
    echo "FATAL: Receiver exited with error."
    echo "READY/error output:"
    cat "$READY_LOG"
    echo "======================================"
    exit 1
fi

echo ""
echo "Test complete."