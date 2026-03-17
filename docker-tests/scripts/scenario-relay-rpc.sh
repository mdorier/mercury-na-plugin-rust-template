#!/bin/bash
# Scenario 1: RPC via relay under full NAT.
# Proves HG RPC works end-to-end through relay when peers are
# on different private networks.
set -e
cd "$(dirname "$0")/.."

echo "=== Scenario: Relay RPC ==="

docker compose -f docker-compose.yml down -v 2>/dev/null || true
docker compose -f docker-compose.yml up -d

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

echo "Server address published, starting client..."

# Run RPC client on peer_b (foreground — exit code is the test result)
docker exec \
    -e MERCURY_RELAY_ADDR="$RELAY_ADDR" \
    peer_b /opt/test/test_nat_hg_rpc_client -p "tcp,relay" -f /shared/port.cfg

RC=$?
docker compose -f docker-compose.yml down -v
exit $RC
