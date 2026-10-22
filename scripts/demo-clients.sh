#!/usr/bin/env bash
# Only orchestrates processes; every demo TCP connection uses the C client.
set -euo pipefail
cd "$(dirname "$0")/.."
host=${1:-127.0.0.1}
port=${2:-5555}
pids=()
for i in 1 2 3; do
    printf -v request '{"type":"PING","request_id":"demo-%d","payload":{"text":"Xin chào từ client %d"}}' "$i" "$i"
    ./build/client "$host" "$port" "$request" &
    pids+=("$!")
done
result=0
for pid in "${pids[@]}"; do
    if ! wait "$pid"; then result=1; fi
done
exit "$result"
