#!/bin/bash
# Scenario 2: DCUtR success + RPC.
# All containers on public_net (no NAT). DCUtR upgrades to a direct
# connection. After DCUtR, the relay is killed. RPC tests must still
# work over the direct connection.
set -e
cd "$(dirname "$0")/.."

echo "=== Scenario: DCUtR Success ==="

docker compose -f docker-compose.direct.yml down -v 2>/dev/null || true
docker compose -f docker-compose.direct.yml up -d

# Wait for relay to write its address
./scripts/wait-for-file.sh relay /shared/relay_addr.txt

RELAY_ADDR=$(docker exec relay cat /shared/relay_addr.txt)
echo "Relay address: $RELAY_ADDR"

# Start HG server on peer_a (background)
docker exec -d \
    -e MERCURY_RELAY_ADDR="$RELAY_ADDR" \
    peer_a /opt/test/test_nat_hg_server -p "tcp,relay" -f /shared/port.cfg

# Wait for server to publish address
./scripts/wait-for-file.sh peer_a /shared/port.cfg 60

echo "Server address published, starting DCUtR client..."

# Start DCUtR client on peer_b (background — it will block during lookup).
# Wrap in a shell that writes exit code to /shared/client_rc when done.
docker exec -d \
    -e MERCURY_RELAY_ADDR="$RELAY_ADDR" \
    peer_b bash -c '/opt/test/test_nat_hg_dcutr_client -p "tcp,relay" -f /shared/port.cfg; echo $? > /shared/client_rc'

# Wait for client to signal lookup done (DCUtR completed)
./scripts/wait-for-file.sh peer_b /shared/dcutr_lookup_done 60

echo "DCUtR lookup done, killing relay..."

# Kill the relay
docker stop relay

# Signal client that relay is dead
docker exec peer_b bash -c "echo done > /shared/relay_killed"

echo "Waiting for client to finish..."

# Wait for the client to write its exit code (poll /shared/client_rc)
./scripts/wait-for-file.sh peer_b /shared/client_rc 60
CLIENT_EXIT=$(docker exec peer_b cat /shared/client_rc | tr -d '[:space:]')
echo "Client exited with code: $CLIENT_EXIT"

echo "Waiting for server to finish..."

# The server should exit once the client sends finalize RPC.
# Wait for it by polling — give it up to 30s.
SERVER_EXIT=0
for i in $(seq 1 300); do
    # Check if server process is still running
    if ! docker exec peer_a pgrep -f test_nat_hg_server >/dev/null 2>&1; then
        break
    fi
    if [ "$i" -eq 300 ]; then
        echo "Server did not exit in time"
        SERVER_EXIT=1
    fi
    sleep 0.1
done

docker compose -f docker-compose.direct.yml down -v

if [ "$CLIENT_EXIT" -ne 0 ] || [ "$SERVER_EXIT" -ne 0 ]; then
    echo "FAILED (server=$SERVER_EXIT client=$CLIENT_EXIT)"
    exit 1
fi
echo "=== DCUTR SUCCESS TEST PASSED ==="
