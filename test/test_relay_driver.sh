#!/bin/bash
#
# Three-process test driver for relay tests.
# Usage: test_relay_driver.sh <relay_cmd...> -- <server_cmd...> -- <client_cmd...>
#
set -e

# Parse arguments at "--" separators into 3 groups
RELAY_ARGS=()
SERVER_ARGS=()
CLIENT_ARGS=()
group=0
for arg in "$@"; do
    if [ "$arg" = "--" ]; then
        group=$((group + 1))
        continue
    fi
    case $group in
        0) RELAY_ARGS+=("$arg") ;;
        1) SERVER_ARGS+=("$arg") ;;
        2) CLIENT_ARGS+=("$arg") ;;
    esac
done

if [ ${#RELAY_ARGS[@]} -eq 0 ] || [ ${#SERVER_ARGS[@]} -eq 0 ] || [ ${#CLIENT_ARGS[@]} -eq 0 ]; then
    echo "Usage: $0 <relay_cmd...> -- <server_cmd...> -- <client_cmd...>"
    exit 1
fi

# Clean up on exit
RELAY_PID=""
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [ -n "$RELAY_PID" ] && kill -0 "$RELAY_PID" 2>/dev/null; then
        kill "$RELAY_PID" 2>/dev/null || true
        wait "$RELAY_PID" 2>/dev/null || true
    fi
    rm -f relay_addr.txt na_test_addr.txt
}
trap cleanup EXIT

# Clean up stale files
rm -f relay_addr.txt na_test_addr.txt

# 1. Start relay server
echo "=== Starting relay server ==="
"${RELAY_ARGS[@]}" &
RELAY_PID=$!

# Wait for relay address file (up to 15s)
for i in $(seq 1 150); do
    if [ -f relay_addr.txt ] && [ -s relay_addr.txt ]; then
        break
    fi
    sleep 0.1
done

if [ ! -f relay_addr.txt ] || [ ! -s relay_addr.txt ]; then
    echo "Timed out waiting for relay address file"
    exit 1
fi

RELAY_ADDR=$(cat relay_addr.txt)
echo "=== Relay address: $RELAY_ADDR ==="

# 2. Start NA server with relay address in environment
export MERCURY_RELAY_ADDR="$RELAY_ADDR"
echo "=== Starting NA server ==="
"${SERVER_ARGS[@]}" &
SERVER_PID=$!

# Wait for server to initialize and make relay reservation (longer than direct TCP)
sleep 3

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "NA server exited early"
    wait "$SERVER_PID" 2>/dev/null
    exit 1
fi

# 3. Run NA client (foreground)
echo "=== Starting NA client ==="
CLIENT_RC=0
"${CLIENT_ARGS[@]}" || CLIENT_RC=$?

# 4. Wait for NA server
SERVER_RC=0
wait "$SERVER_PID" || SERVER_RC=$?
SERVER_PID=""

# 5. Kill relay server
if kill -0 "$RELAY_PID" 2>/dev/null; then
    kill "$RELAY_PID" 2>/dev/null || true
    wait "$RELAY_PID" 2>/dev/null || true
fi
RELAY_PID=""

if [ $SERVER_RC -ne 0 ] || [ $CLIENT_RC -ne 0 ]; then
    echo "FAILED (server=$SERVER_RC client=$CLIENT_RC)"
    exit 1
fi

echo "=== RELAY TEST PASSED ==="
