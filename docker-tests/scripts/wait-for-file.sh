#!/bin/bash
# Wait for a file to appear inside a Docker container.
# Usage: wait-for-file.sh <container> <path> [timeout_s]
set -e
CONTAINER=$1; FILE=$2; TIMEOUT=${3:-30}
for i in $(seq 1 $((TIMEOUT * 10))); do
    if docker exec "$CONTAINER" test -f "$FILE" 2>/dev/null; then exit 0; fi
    sleep 0.1
done
echo "Timeout waiting for $FILE in $CONTAINER"
exit 1
