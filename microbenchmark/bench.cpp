// bench.cpp  –  OS paging (mmap) vs application LRU (pread) microbenchmark
//
// Hypothesis: Is OS-managed page replacement competitive with an explicit
//             application-level LRU buffer pool?
//
// mmap path:     MAP_SHARED file + MADV_RANDOM.  The OS page cache is the only
//                buffer; paging decisions are made entirely by the kernel.
//                Linux: add --huge-pages to enable Transparent Huge Pages (THP)
//                via madvise(MADV_HUGEPAGE), reducing TLB pressure (4K→2M pages).
//
// pread+LRU path: pread(2) with F_NOCACHE (macOS) / O_DIRECT (Linux) to bypass
//                the OS page cache.  All buffering is an explicit LRU in the app.
//
// Both paths use the same file and the same Zipf-distributed access sequence.
//
// Build: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
// Run:   ./build/bench [options] > result.json
//
// Linux-specific options:
//   --huge-pages     Enable THP on mmap region (madvise MADV_HUGEPAGE).
//                    Reduces TLB misses by promoting 4K pages to 2M pages.
//                    Requires kernel THP support (cat /sys/kernel/mm/transparent_hugepage/enabled).
//                    Combine with --prefault for best steady-state comparison.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ─── Config ──────────────────────────────────────────────────────────────────

struct Config
{
    size_t size_gb = 4;
    size_t num_ops = 1'000'000;
    size_t warmup = 50'000;
    size_t lru_pages = 10'000; // application buffer pool capacity (pages)
    double zipf_a = 1.0;
    size_t value_sz = 4096; // bytes per record — one OS page
    const char *file = "/tmp/bench_data.bin";
    bool prefault = false;     // touch all seq pages before measuring mmap steady-state
    size_t zipf_n = 0;         // unique pages for Zipf distribution; 0 = all pages in dataset
    size_t prefetch_ahead = 0; // issue MADV_WILLNEED N steps ahead (0 = disabled)
    size_t threads = 1;        // number of concurrent worker threads
    size_t scan_width = 0;     // scan mode: prefetch+read this many pages per "op" (0 = point mode)
    bool huge_pages = false;   // Linux: madvise(MADV_HUGEPAGE) to enable THP on mmap region
};

// ─── Zipf distribution (CDF + binary search, O(log n) per sample) ────────────

class Zipf
{
    std::vector<double> cdf_;
    std::uniform_real_distribution<double> u01_{0.0, 1.0};

public:
    Zipf(size_t n, double a) : cdf_(n + 1, 0.0)
    {
        double z = 0.0;
        for (size_t i = 1; i <= n; ++i)
            z += 1.0 / std::pow(static_cast<double>(i), a);
        for (size_t i = 1; i <= n; ++i)
            cdf_[i] = cdf_[i - 1] + 1.0 / (std::pow(static_cast<double>(i), a) * z);
    }
    // Returns 0-indexed rank in [0, n).
    size_t sample(std::mt19937_64 &rng)
    {
        double u = u01_(rng);
        return static_cast<size_t>(
                   std::lower_bound(cdf_.begin() + 1, cdf_.end(), u) - cdf_.begin()) -
               1;
    }
};

// ─── Application LRU buffer pool ─────────────────────────────────────────────
//
// Standard doubly-linked list + hash map; O(1) get/evict.

class LRU
{
    struct Node
    {
        size_t id;
        char *buf;
        Node *prev, *next;
    };

    Node sentinel_{}; // circular list head
    std::unordered_map<size_t, Node *> idx_;
    const size_t cap_, pgsz_;
    const int fd_;
    size_t hits_{}, misses_{};

    void unlink(Node *n)
    {
        n->prev->next = n->next;
        n->next->prev = n->prev;
    }
    void to_front(Node *n)
    {
        n->next = sentinel_.next;
        n->prev = &sentinel_;
        sentinel_.next->prev = n;
        sentinel_.next = n;
    }

public:
    LRU(int fd, size_t cap, size_t pgsz) : cap_(cap), pgsz_(pgsz), fd_(fd)
    {
        sentinel_.prev = sentinel_.next = &sentinel_;
        idx_.reserve(cap * 2);
    }
    ~LRU()
    {
        for (auto &[_, n] : idx_)
        {
            free(n->buf);
            delete n;
        }
    }

    const char *get(size_t id)
    {
        if (auto it = idx_.find(id); it != idx_.end())
        {
            ++hits_;
            unlink(it->second);
            to_front(it->second);
            return it->second->buf;
        }
        ++misses_;
        Node *n;
        if (idx_.size() >= cap_)
        { // evict least-recently-used
            n = sentinel_.prev;
            unlink(n);
            idx_.erase(n->id);
        }
        else
        {
            n = new Node{};
            n->buf = static_cast<char *>(aligned_alloc(4096, pgsz_));
        }
        n->id = id;
        pread(fd_, n->buf, pgsz_, static_cast<off_t>(id * pgsz_));
        to_front(n);
        idx_[id] = n;
        return n->buf;
    }

    double hit_rate() const
    {
        return (hits_ + misses_) ? static_cast<double>(hits_) / (hits_ + misses_) : 0.0;
    }
    size_t hits() const { return hits_; }
    size_t misses() const { return misses_; }
};

// ─── Data file setup ─────────────────────────────────────────────────────────

static void ensure_data_file(const Config &cfg)
{
    size_t total = cfg.size_gb * (1ULL << 30);
    struct stat st{};
    if (!stat(cfg.file, &st) && static_cast<size_t>(st.st_size) == total)
    {
        fprintf(stderr, "[setup] data file OK (%zu GB)\n", cfg.size_gb);
        return;
    }
    fprintf(stderr, "[setup] creating %zu GB data file at %s …\n", cfg.size_gb, cfg.file);
    int fd = open(cfg.file, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
    {
        perror("open");
        exit(1);
    }
    std::mt19937_64 rng(0xdeadbeefcafeULL);
    const size_t CHUNK = 1 << 20; // 1 MB
    std::vector<uint64_t> buf(CHUNK / sizeof(uint64_t));
    for (size_t done = 0; done < total;)
    {
        for (auto &v : buf)
            v = rng();
        size_t n = std::min(CHUNK, total - done);
        if (write(fd, buf.data(), n) < 0)
        {
            perror("write");
            exit(1);
        }
        done += n;
    }
    close(fd);
    fprintf(stderr, "[setup] done\n");
}

// ─── Result & statistics ─────────────────────────────────────────────────────

struct Result
{
    double tput;                // ops / sec
    double p50, p95, p99, p999; // µs latency percentiles
    double elapsed;             // total wall-clock seconds for num_ops
    double hit_rate = -1.0;     // < 0 → N/A (mmap path)
    size_t hits = 0, misses = 0;
};

using Clock = std::chrono::steady_clock;
using Micros = std::chrono::duration<double, std::micro>;

static Result summarise(std::vector<double> &lats, double elapsed,
                        double hr = -1.0, size_t h = 0, size_t m = 0)
{
    std::sort(lats.begin(), lats.end());
    size_t n = lats.size();
    return {static_cast<double>(n) / elapsed,
            lats[n * 50 / 100],
            lats[n * 95 / 100],
            lats[n * 99 / 100],
            lats[n * 999 / 1000],
            elapsed, hr, h, m};
}

// ─── Thread-safe latency collector ───────────────────────────────────────────

struct LatCollector
{
    std::mutex mu;
    std::vector<double> lats;
    void push(double v)
    {
        std::lock_guard<std::mutex> lg(mu);
        lats.push_back(v);
    }
    void push_batch(std::vector<double> &v)
    {
        std::lock_guard<std::mutex> lg(mu);
        lats.insert(lats.end(), v.begin(), v.end());
    }
};

// ─── Benchmark: mmap path ────────────────────────────────────────────────────
//
// Workload modes:
//  point mode  (scan_width=0): one random page access per "op"
//  scan  mode  (scan_width=N): one op = MADV_WILLNEED N random pages,
//                               then read all N (simulates FaultKV Scan prefetch)

static Result bench_mmap(const Config &cfg, const std::vector<size_t> &seq)
{
    size_t total = cfg.size_gb * (1ULL << 30);
    size_t npages = total / cfg.value_sz;

    int fd = open(cfg.file, O_RDONLY);
    if (fd < 0)
    {
        perror("open");
        exit(1);
    }
    char *m = static_cast<char *>(mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0));
    if (m == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }
    madvise(m, total, MADV_RANDOM);
#ifdef __linux__
    if (cfg.huge_pages)
        madvise(m, total, MADV_HUGEPAGE); // request THP promotion (2 MB pages)
#endif

    volatile uint64_t sink = 0;
    if (cfg.prefault)
    {
        fprintf(stderr, "[bench/mmap] pre-faulting %zu unique pages …\n", seq.size());
        for (size_t pg : seq)
            sink ^= *reinterpret_cast<const uint64_t *>(m + (pg % npages) * cfg.value_sz);
        fprintf(stderr, "[bench/mmap] pre-fault done\n");
    }
    // warmup
    for (size_t i = 0; i < cfg.warmup; ++i)
        sink ^= *reinterpret_cast<const uint64_t *>(m + (seq[i] % npages) * cfg.value_sz);
    (void)sink;

    // Determine per-thread slice of the measured ops
    size_t total_ops = cfg.num_ops; // total "ops" across all threads
    size_t ops_per_thread = (total_ops + cfg.threads - 1) / cfg.threads;

    LatCollector collector;
    collector.lats.reserve(total_ops);
    std::atomic<size_t> seq_cursor{cfg.warmup}; // shared index into seq[]

    auto worker = [&]()
    {
        std::vector<double> local_lats;
        local_lats.reserve(ops_per_thread);
        volatile uint64_t s = 0;

        if (cfg.scan_width == 0)
        {
            // ── point mode ──────────────────────────────────────────────────
            while (true)
            {
                size_t idx = seq_cursor.fetch_add(1, std::memory_order_relaxed);
                if (idx >= seq.size())
                    break;

                if (cfg.prefetch_ahead > 0)
                {
                    size_t fidx = idx + cfg.prefetch_ahead;
                    if (fidx < seq.size())
                        madvise(m + (seq[fidx] % npages) * cfg.value_sz,
                                cfg.value_sz, MADV_WILLNEED);
                }
                auto a = Clock::now();
                s ^= *reinterpret_cast<const uint64_t *>(
                    m + (seq[idx] % npages) * cfg.value_sz);
                local_lats.push_back(Micros(Clock::now() - a).count());
            }
        }
        else
        {
            // ── scan mode ───────────────────────────────────────────────────
            // One "op" = pick scan_width random pages, MADV_WILLNEED them all,
            // then read them all.  Latency is measured over the full batch.
            const size_t W = cfg.scan_width;
            while (true)
            {
                size_t base = seq_cursor.fetch_add(W, std::memory_order_relaxed);
                if (base >= seq.size())
                    break;
                size_t end = std::min(base + W, seq.size());

                // phase 1: issue all prefetches (non-blocking)
                for (size_t k = base; k < end; ++k)
                    madvise(m + (seq[k] % npages) * cfg.value_sz,
                            cfg.value_sz, MADV_WILLNEED);

                // phase 2: read all pages (faults hidden by phase 1)
                auto a = Clock::now();
                for (size_t k = base; k < end; ++k)
                    s ^= *reinterpret_cast<const uint64_t *>(
                        m + (seq[k] % npages) * cfg.value_sz);
                local_lats.push_back(Micros(Clock::now() - a).count());
            }
        }
        (void)s;
        collector.push_batch(local_lats);
    };

    auto t0 = Clock::now();
    std::vector<std::thread> workers;
    workers.reserve(cfg.threads);
    for (size_t t = 0; t < cfg.threads; ++t)
        workers.emplace_back(worker);
    for (auto &t : workers)
        t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    // In scan mode, each latency sample covers scan_width pages.
    // Report throughput as individual page accesses for fair comparison.
    size_t effective_ops = cfg.scan_width > 0
                               ? collector.lats.size() * cfg.scan_width
                               : collector.lats.size();

    munmap(m, total);
    close(fd);
    return summarise(collector.lats, elapsed / effective_ops * collector.lats.size());
}

// ─── Benchmark: pread + LRU path ─────────────────────────────────────────────
//
// LRU is inherently serial (shared data structure), so multiple threads each
// get their own independent LRU instance backed by the same fd.

static Result bench_pread_lru(const Config &cfg, const std::vector<size_t> &seq)
{
    size_t total = cfg.size_gb * (1ULL << 30);
    size_t npages = total / cfg.value_sz;

    // Open one fd per thread (F_NOCACHE is per-fd on macOS)
    auto open_fd = [&]() -> int
    {
        int fd = open(cfg.file, O_RDONLY);
        if (fd < 0)
        {
            perror("open");
            exit(1);
        }
#ifdef __APPLE__
        fcntl(fd, F_NOCACHE, 1);
#elif defined(O_DIRECT)
        close(fd);
        fd = open(cfg.file, O_RDONLY | O_DIRECT);
        if (fd < 0)
        {
            perror("open O_DIRECT");
            exit(1);
        }
#endif
        return fd;
    };

    // warmup (single thread, single LRU)
    {
        int fd = open_fd();
        LRU lru(fd, cfg.lru_pages, cfg.value_sz);
        volatile uint64_t s = 0;
        for (size_t i = 0; i < cfg.warmup; ++i)
            s ^= *reinterpret_cast<const uint64_t *>(lru.get(seq[i] % npages));
        (void)s;
        close(fd);
    }

    std::atomic<size_t> seq_cursor{cfg.warmup};
    LatCollector collector;
    collector.lats.reserve(cfg.num_ops);

    // aggregate stats across threads
    std::atomic<size_t> total_hits{0}, total_misses{0};

    auto worker = [&]()
    {
        int fd = open_fd();
        LRU lru(fd, cfg.lru_pages / cfg.threads + 1, cfg.value_sz);
        std::vector<double> local_lats;
        local_lats.reserve((cfg.num_ops + cfg.threads - 1) / cfg.threads);
        volatile uint64_t s = 0;

        if (cfg.scan_width == 0)
        {
            // point mode
            while (true)
            {
                size_t idx = seq_cursor.fetch_add(1, std::memory_order_relaxed);
                if (idx >= seq.size())
                    break;
                auto a = Clock::now();
                s ^= *reinterpret_cast<const uint64_t *>(lru.get(seq[idx] % npages));
                local_lats.push_back(Micros(Clock::now() - a).count());
            }
        }
        else
        {
            // scan mode: read scan_width pages per op (no prefetch hint for LRU)
            const size_t W = cfg.scan_width;
            while (true)
            {
                size_t base = seq_cursor.fetch_add(W, std::memory_order_relaxed);
                if (base >= seq.size())
                    break;
                size_t end = std::min(base + W, seq.size());
                auto a = Clock::now();
                for (size_t k = base; k < end; ++k)
                    s ^= *reinterpret_cast<const uint64_t *>(lru.get(seq[k] % npages));
                local_lats.push_back(Micros(Clock::now() - a).count());
            }
        }
        (void)s;
        total_hits.fetch_add(lru.hits(), std::memory_order_relaxed);
        total_misses.fetch_add(lru.misses(), std::memory_order_relaxed);
        collector.push_batch(local_lats);
        close(fd);
    };

    auto t0 = Clock::now();
    std::vector<std::thread> workers;
    workers.reserve(cfg.threads);
    for (size_t t = 0; t < cfg.threads; ++t)
        workers.emplace_back(worker);
    for (auto &t : workers)
        t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    size_t h = total_hits.load(), ms = total_misses.load();
    double hr = (h + ms) ? static_cast<double>(h) / (h + ms) : 0.0;

    size_t effective_ops = cfg.scan_width > 0
                               ? collector.lats.size() * cfg.scan_width
                               : collector.lats.size();
    return summarise(collector.lats,
                     elapsed / effective_ops * collector.lats.size(),
                     hr, h, ms);
}

// ─── JSON output ─────────────────────────────────────────────────────────────

static void json_result(const char *name, const Result &r, bool last)
{
    printf("    \"%s\": {\n", name);
    printf("      \"throughput_ops_per_sec\": %.1f,\n", r.tput);
    printf("      \"latency_p50_us\":          %.3f,\n", r.p50);
    printf("      \"latency_p95_us\":          %.3f,\n", r.p95);
    printf("      \"latency_p99_us\":          %.3f,\n", r.p99);
    printf("      \"latency_p999_us\":         %.3f,\n", r.p999);
    printf("      \"total_time_sec\":           %.3f", r.elapsed);
    if (r.hit_rate >= 0.0)
    {
        printf(",\n      \"cache_hit_rate\":          %.4f", r.hit_rate);
        printf(",\n      \"cache_hits\":              %zu", r.hits);
        printf(",\n      \"cache_misses\":            %zu", r.misses);
    }
    printf("\n    }%s\n", last ? "" : ",");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    Config cfg;
    for (int i = 1; i < argc; ++i)
    {
        std::string k = argv[i];
        if (k == "--size-gb")
            cfg.size_gb = std::stoul(argv[++i]);
        else if (k == "--ops")
            cfg.num_ops = std::stoul(argv[++i]);
        else if (k == "--warmup")
            cfg.warmup = std::stoul(argv[++i]);
        else if (k == "--lru-pages")
            cfg.lru_pages = std::stoul(argv[++i]);
        else if (k == "--zipf-alpha")
            cfg.zipf_a = std::stod(argv[++i]);
        else if (k == "--value-size")
            cfg.value_sz = std::stoul(argv[++i]);
        else if (k == "--data-file")
            cfg.file = argv[++i];
        else if (k == "--prefault")
            cfg.prefault = true;
        else if (k == "--zipf-n")
            cfg.zipf_n = std::stoul(argv[++i]);
        else if (k == "--prefetch-ahead")
            cfg.prefetch_ahead = std::stoul(argv[++i]);
        else if (k == "--threads")
            cfg.threads = std::stoul(argv[++i]);
        else if (k == "--scan-width")
            cfg.scan_width = std::stoul(argv[++i]);
        else if (k == "--huge-pages")
            cfg.huge_pages = true;
    }
    if (cfg.value_sz % 4096 != 0)
    {
        fprintf(stderr, "error: --value-size must be a multiple of 4096\n");
        return 1;
    }

    ensure_data_file(cfg);

    size_t total = cfg.size_gb * (1ULL << 30);
    size_t npages = total / cfg.value_sz;
    // zipf_n=0 means use all pages; cap at npages so we never exceed the dataset
    size_t zn = (cfg.zipf_n > 0) ? std::min(cfg.zipf_n, npages) : npages;

    fprintf(stderr, "[bench] generating %zu accesses  Zipf α=%.2f  n=%zu pages (dataset=%zu pages)\n",
            cfg.num_ops + cfg.warmup, cfg.zipf_a, zn, npages);
    fprintf(stderr, "[bench] building Zipf table for %zu pages …\n", zn);
    Zipf zipf(zn, cfg.zipf_a);
    std::mt19937_64 rng(0xcafebabe12345678ULL);
    std::vector<size_t> seq(cfg.num_ops + cfg.warmup);
    for (auto &v : seq)
        v = zipf.sample(rng);
    fprintf(stderr, "[bench] done\n");

    const char *mmap_mode = cfg.prefault             ? " (prefault)"
                            : cfg.scan_width > 0     ? " (scan+MADV_WILLNEED)"
                            : cfg.prefetch_ahead > 0 ? " (MADV_WILLNEED)"
                                                     : " (cold)";
    fprintf(stderr, "[bench] running mmap%s  threads=%zu", mmap_mode, cfg.threads);
    if (cfg.scan_width > 0)
        fprintf(stderr, "  scan_width=%zu", cfg.scan_width);
    if (cfg.prefetch_ahead > 0)
        fprintf(stderr, "  prefetch_ahead=%zu", cfg.prefetch_ahead);
    fprintf(stderr, "\n");
    Result mr = bench_mmap(cfg, seq);
    fprintf(stderr, "        %.0f ops/sec\n", mr.tput);

    fprintf(stderr, "[bench] running pread+LRU  threads=%zu", cfg.threads);
    if (cfg.scan_width > 0)
        fprintf(stderr, "  scan_width=%zu", cfg.scan_width);
    fprintf(stderr, "\n");
    Result lr = bench_pread_lru(cfg, seq);
    fprintf(stderr, "        %.0f ops/sec  hit=%.1f%%\n", lr.tput, lr.hit_rate * 100.0);

    printf("{\n");
    printf("  \"config\": {\n");
    printf("    \"total_size_gb\":    %zu,\n", cfg.size_gb);
    printf("    \"value_size_bytes\": %zu,\n", cfg.value_sz);
    printf("    \"num_operations\":   %zu,\n", cfg.num_ops);
    printf("    \"warmup_ops\":       %zu,\n", cfg.warmup);
    printf("    \"lru_cache_pages\":  %zu,\n", cfg.lru_pages);
    printf("    \"zipf_alpha\":       %.2f,\n", cfg.zipf_a);
    printf("    \"zipf_n_pages\":     %zu,\n", zn);
    printf("    \"prefault_mmap\":        %s,\n", cfg.prefault ? "true" : "false");
    printf("    \"prefetch_ahead_pages\": %zu,\n", cfg.prefetch_ahead);
    printf("    \"threads\":              %zu,\n", cfg.threads);
    printf("    \"scan_width\":           %zu,\n", cfg.scan_width);
    printf("    \"huge_pages\":           %s\n", cfg.huge_pages ? "true" : "false");
    printf("  },\n");
    printf("  \"results\": {\n");
    json_result("mmap", mr, false);
    json_result("pread_lru", lr, true);
    printf("  }\n");
    printf("}\n");
    return 0;
}
