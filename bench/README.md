# Benchmarking

Build and start the Release server with the desired persistence policy, then run:

```bash
chmod +x bench/benchmark.sh
KEYFORGE_BENCH_OPERATIONS=100000 KEYFORGE_BENCH_CLIENTS=50 \
KEYFORGE_BENCH_PIPELINE=16 KEYFORGE_GRAPH_NODES=1000 ./bench/benchmark.sh
```

The first section delegates PING/SET/GET/INCR measurement to `redis-benchmark`; the
second creates a dependency chain and reports the measured nanoseconds for one
transitive invalidation. Graph construction is deliberately outside the measured
interval.

For a reproducible report, retain the raw output and record the date, KeyForge
revision, build type, CPU/WSL version, client count, pipeline depth, payload size,
operation count, and `--appendfsync` mode. Report the throughput and percentile
latencies printed by the installed `redis-benchmark`; do not compare with Redis
unless both servers are tested under identical conditions.
