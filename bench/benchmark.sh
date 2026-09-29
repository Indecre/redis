#!/usr/bin/env bash
set -euo pipefail

host="${KEYFORGE_BENCH_HOST:-127.0.0.1}"
port="${KEYFORGE_BENCH_PORT:-6380}"
operations="${KEYFORGE_BENCH_OPERATIONS:-100000}"
clients="${KEYFORGE_BENCH_CLIENTS:-50}"
pipeline="${KEYFORGE_BENCH_PIPELINE:-16}"
nodes="${KEYFORGE_GRAPH_NODES:-1000}"

for tool in redis-cli redis-benchmark; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing required tool: $tool (install the redis-tools package)" >&2
        exit 1
    fi
done

echo "KeyForge RESP benchmark"
echo "host=$host port=$port operations=$operations clients=$clients pipeline=$pipeline"
redis-benchmark -h "$host" -p "$port" -t ping,set,get,incr \
    -n "$operations" -c "$clients" -P "$pipeline"

echo
echo "Building dependency chain with $nodes nodes"
redis-cli -h "$host" -p "$port" FLUSHDB >/dev/null
redis-cli -h "$host" -p "$port" SET bench:node:0 value >/dev/null
for ((index = 1; index < nodes; ++index)); do
    redis-cli -h "$host" -p "$port" SET "bench:node:$index" value >/dev/null
    redis-cli -h "$host" -p "$port" KFG.DEPEND "bench:node:$index" \
        "bench:node:$((index - 1))" >/dev/null
done

start_ns="$(date +%s%N)"
removed="$(redis-cli --raw -h "$host" -p "$port" KFG.INVALIDATE bench:node:0)"
end_ns="$(date +%s%N)"
elapsed_ns="$((end_ns - start_ns))"
echo "cascade_nodes=$nodes removed=$removed elapsed_ns=$elapsed_ns"

