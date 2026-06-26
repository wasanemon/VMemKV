# microbenchmark: mmap vs pread+LRU

OS-managed page caching (`mmap`) vs application-managed LRU buffer pool (`pread`)
under random-access workload with Zipfian key distribution.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Run

```sh
# Evict file page cache first (macOS)
sudo purge

# Default: 4 GB dataset, 1M ops, Zipf α=1.0, LRU capacity = 10 000 pages × 4 KB = 40 MB
./build/bench > result.json

# Set dataset larger than physical RAM to induce swap/paging pressure
./build/bench \
  --size-gb    16       \
  --ops        2000000  \
  --warmup     100000   \
  --lru-pages  20000    \
  --zipf-alpha 0.8      \
  --value-size 4096     \
  --data-file  /tmp/bench.bin \
  > result.json
```

### Options

| flag             | default             | description                                 |
| ---------------- | ------------------- | ------------------------------------------- |
| `--size-gb N`    | 4                   | dataset size in GB                          |
| `--ops N`        | 1 000 000           | measured operations                         |
| `--warmup N`     | 50 000              | warmup ops (excluded from stats)            |
| `--lru-pages N`  | 10 000              | LRU buffer pool capacity (pages)            |
| `--zipf-alpha F` | 1.0                 | Zipf skew (higher = more skewed)            |
| `--value-size N` | 4096                | bytes per record (must be multiple of 4096) |
| `--data-file P`  | /tmp/bench_data.bin | path to data file                           |

## Output

```json
{
  "config": { "total_size_gb": 4, "value_size_bytes": 4096, ... },
  "results": {
    "mmap": {
      "throughput_ops_per_sec": 95000,
      "latency_p50_us": 1.2, "latency_p99_us": 80.0, ...
    },
    "pread_lru": {
      "throughput_ops_per_sec": 60000,
      "latency_p50_us": 1.8, "latency_p99_us": 120.0, ...,
      "cache_hit_rate": 0.87, "cache_hits": 870000, "cache_misses": 130000
    }
  }
}
```

## Interpretation

| Condition                               | Expected winner                                     |
| --------------------------------------- | --------------------------------------------------- |
| Working set fits in cache (high Zipf α) | Similar; LRU list-update overhead visible           |
| Working set > cache, Zipf hot tail      | `mmap` — no per-access syscall or list manipulation |
| Large dataset, near-uniform access      | Both I/O bound; `mmap` avoids a syscall per miss    |

## macOS notes

- `pread_lru` uses `F_NOCACHE` so the OS page cache is bypassed entirely —
  the app LRU is the **sole** buffer. This gives the cleanest comparison.
- Monitor swap activity with `vm_stat 1` or Activity Monitor → Memory pressure.
- Dataset must exceed physical RAM to observe meaningful paging behavior.
- Run `sudo purge` before each experiment to start with a cold page cache.
