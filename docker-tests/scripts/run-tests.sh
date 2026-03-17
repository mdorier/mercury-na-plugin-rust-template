#!/bin/bash
# Top-level orchestrator: builds Docker images and runs all NAT test scenarios.
set -e
cd "$(dirname "$0")/.."

# Build context is the grandparent (mercury-custom-na/) so COPY can reach
# mercury/, rust-libp2p/, and mercury-na-plugin-libp2p/
BUILD_CONTEXT="$(cd ../.. && pwd)"

echo "=== Building Docker images ==="
echo "Build context: $BUILD_CONTEXT"

docker build --target runtime -t mercury-nat-test:runtime \
    -f Dockerfile "$BUILD_CONTEXT"

PASS=0; FAIL=0

for scenario in scenario-relay-rpc scenario-dcutr-success scenario-relay-bulk; do
    echo ""
    echo "========================================"
    echo "=== Running $scenario ==="
    echo "========================================"
    if ./scripts/$scenario.sh; then
        echo "$scenario: PASS"
        PASS=$((PASS+1))
    else
        echo "$scenario: FAIL"
        FAIL=$((FAIL+1))
    fi
done

echo ""
echo "========================================"
echo "=== Results: $PASS passed, $FAIL failed ==="
echo "========================================"
[ "$FAIL" -eq 0 ]
