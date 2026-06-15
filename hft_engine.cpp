#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cassert>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

// Platform Compatibility
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <x86intrin.h>
#elif defined(__aarch64__)
#include <time.h>
#endif

// ── Include our new modules ──────────────────────────────────
#include "include/types.hpp"
#include "include/spsc_queue.hpp"
#include "include/indicators.hpp"
#include "include/strategy.hpp"
#include "include/feed.hpp"
#include "src/strategies/my_strategy.hpp"

// ─────────────────────────────────────────────────────────────
//  SECTION 1 — Platform Utilities
// ─────────────────────────────────────────────────────────────

static uint64_t rdtsc_hz = 0;

#if defined(__x86_64__) || defined(_M_X64)
static void calibrate_tsc()
{
    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    uint64_t c1 = __rdtsc();
    usleep(10000);
    uint64_t c2 = __rdtsc();
    clock_gettime(CLOCK_MONOTONIC_RAW, &t2);
    uint64_t ns = (t2.tv_sec - t1.tv_sec) * 1'000'000'000ULL + (t2.tv_nsec - t1.tv_nsec);
    rdtsc_hz = (c2 - c1) * 1'000'000'000ULL / ns;
}
inline uint64_t now_ns() { return __rdtsc() * 1'000'000'000ULL / rdtsc_hz; }
#else
static void calibrate_tsc() { rdtsc_hz = 1; }
inline uint64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}
#endif

static void pin_thread(int core)
{
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)core; // Ignored on MacOS
#endif
}

// ─────────────────────────────────────────────────────────────
//  SECTION 2 — Engine Utilities
// ─────────────────────────────────────────────────────────────

constexpr int BOOK_LEVELS = 64;
constexpr int64_t TICK_SIZE = 10000;
struct Level
{
    int64_t price;
    uint64_t qty;
};

class alignas(CACHE_LINE) OrderBook
{
public:
    void reset(int64_t mid) noexcept
    {
        mid_price_ = mid;
        for (int i = 0; i < BOOK_LEVELS; i++)
        {
            bids_[i] = {mid - (i + 1) * TICK_SIZE, 0};
            asks_[i] = {mid + (i + 1) * TICK_SIZE, 0};
        }
    }
    void update_bid(int64_t price, uint64_t qty) noexcept
    {
        int idx = (int)((mid_price_ - price) / TICK_SIZE) - 1;
        if (idx >= 0 && idx < BOOK_LEVELS)
        {
            bids_[idx].qty = qty;
            bids_[idx].price = price;
        }
    }
    void update_ask(int64_t price, uint64_t qty) noexcept
    {
        int idx = (int)((price - mid_price_) / TICK_SIZE) - 1;
        if (idx >= 0 && idx < BOOK_LEVELS)
        {
            asks_[idx].qty = qty;
            asks_[idx].price = price;
        }
    }

private:
    int64_t mid_price_{0};
    Level bids_[BOOK_LEVELS];
    Level asks_[BOOK_LEVELS];
};

constexpr int LAT_SAMPLES = 1024;
class LatencyTracker
{
public:
    void record(uint64_t t0, uint64_t t1) noexcept
    {
        uint64_t lat = t1 - t0;
        uint32_t idx = head_ & (LAT_SAMPLES - 1);
        sum_ = sum_ - samples_[idx] + lat;
        samples_[idx] = lat;
        head_++;
        if (lat < min_)
            min_ = lat;
        if (lat > max_)
            max_ = lat;
            
        if (lat < 250) bins_[0]++;
        else if (lat < 500) bins_[1]++;
        else if (lat < 1000) bins_[2]++;
        else if (lat < 2000) bins_[3]++;
        else if (lat < 5000) bins_[4]++;
        else bins_[5]++;
    }
    uint64_t avg_ns() const noexcept { return head_ == 0 ? 0 : sum_ / std::min(head_, (uint64_t)LAT_SAMPLES); }
    uint64_t min_ns() const noexcept { return min_; }
    uint64_t p99_ns() const noexcept
    {
        uint64_t buf[LAT_SAMPLES];
        int n = (int)std::min(head_, (uint64_t)LAT_SAMPLES);
        std::memcpy(buf, samples_, n * sizeof(uint64_t));
        std::nth_element(buf, buf + n * 99 / 100, buf + n);
        return buf[n * 99 / 100];
    }
    
    void print_histogram() const noexcept
    {
        printf("\n=== Tick-to-Signal Latency Histogram ===\n");
        printf("  < 250 ns    : %llu\n", (unsigned long long)bins_[0]);
        printf("  250-500 ns  : %llu\n", (unsigned long long)bins_[1]);
        printf("  500-1000 ns : %llu\n", (unsigned long long)bins_[2]);
        printf("  1-2 us      : %llu\n", (unsigned long long)bins_[3]);
        printf("  2-5 us      : %llu\n", (unsigned long long)bins_[4]);
        printf("  > 5 us      : %llu\n", (unsigned long long)bins_[5]);
        printf("========================================\n");
    }

private:
    uint64_t samples_[LAT_SAMPLES]{};
    uint64_t head_{0}, sum_{0};
    uint64_t min_{UINT64_MAX}, max_{0};
    uint64_t bins_[6]{0};
};

// ─────────────────────────────────────────────────────────────
//  SECTION 3 — ZMQ Telemetry Layer
// ─────────────────────────────────────────────────────────────

struct TelemetryMsg
{
    enum Kind : uint8_t
    {
        TICK,
        BAR,
        SIGNAL,
        STATS
    } kind;
    union
    {
        struct
        {
            int64_t price, bid, ask;
            uint64_t ts_ns;
        } tick;
        struct
        {
            int64_t o, h, l, c;
            uint32_t vol;
            uint64_t ts;
        } bar;
        struct
        {
            SignalType type;
            uint8_t strength;
            int64_t entry, sl, tp;
            uint64_t lag_ns;
        } sig;
        struct
        {
            uint64_t tick_count, signal_count;
            uint64_t lat_avg, lat_min, lat_p99;
            int64_t pnl;
            uint64_t trades, wins;
        } stats;
    };
};

using TelQueue = SPSCQueue<TelemetryMsg, 2048>;

constexpr size_t SHM_SIZE = 1024 * 1024 * 16; // 16 MB

struct MmapPublisher
{
    int fd;
    uint8_t* base;
    std::atomic<uint64_t>* head_ptr;
    uint8_t* buffer;
    size_t buffer_size;

    bool init(const char *endpoint = "/tmp/hft_telemetry.mmap")
    {
        fd = open(endpoint, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return false;
        if (ftruncate(fd, SHM_SIZE) != 0) return false;
        base = (uint8_t*)mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) return false;

        head_ptr = new (base) std::atomic<uint64_t>();
        head_ptr->store(0, std::memory_order_relaxed);
        buffer = base + 8; // 8 bytes for head
        buffer_size = SHM_SIZE - 8;
        return true;
    }
    
    void publish(const void *data, size_t len) 
    {
        uint64_t h = head_ptr->load(std::memory_order_relaxed);
        uint32_t total_len = 2 + len;
        uint64_t offset = h % buffer_size;
        
        if (offset + total_len > buffer_size) {
            uint32_t pad = buffer_size - offset;
            if (pad >= 2) {
                uint16_t zero = 0;
                std::memcpy(buffer + offset, &zero, 2);
            }
            h += pad;
            offset = 0;
        }

        uint16_t frame_len = len;
        std::memcpy(buffer + offset, &frame_len, 2);
        std::memcpy(buffer + offset + 2, data, len);

        head_ptr->store(h + total_len, std::memory_order_release);
    }
};

#pragma pack(push, 1)
struct BinTick { uint8_t kind; int64_t price, bid, ask; uint64_t ts_ns; };
struct BinBar { uint8_t kind; int64_t o, h, l, c; uint32_t vol; uint64_t ts; };
struct BinSignal { uint8_t kind; uint8_t type, strength; int64_t entry, sl, tp; uint64_t lag_ns; };
struct BinStats { uint8_t kind; uint64_t t_count, s_count, l_avg, l_min, l_p99; int64_t pnl; uint64_t trades, wins; };
#pragma pack(pop)

static void telemetry_thread_fn(TelQueue *q, MmapPublisher *pub)
{
    TelemetryMsg msg;
    while (true)
    {
        if (!q->pop(msg))
        {
            usleep(100);
            continue;
        }
        if (msg.kind == TelemetryMsg::TICK)
        {
            BinTick b{0, msg.tick.price, msg.tick.bid, msg.tick.ask, msg.tick.ts_ns};
            pub->publish(&b, sizeof(b));
        }
        else if (msg.kind == TelemetryMsg::BAR)
        {
            BinBar b{1, msg.bar.o, msg.bar.h, msg.bar.l, msg.bar.c, msg.bar.vol, msg.bar.ts};
            pub->publish(&b, sizeof(b));
        }
        else if (msg.kind == TelemetryMsg::SIGNAL)
        {
            BinSignal b{2, (uint8_t)msg.sig.type, msg.sig.strength, msg.sig.entry, msg.sig.sl, msg.sig.tp, msg.sig.lag_ns};
            pub->publish(&b, sizeof(b));
        }
        else if (msg.kind == TelemetryMsg::STATS)
        {
            BinStats b{3, msg.stats.tick_count, msg.stats.signal_count, msg.stats.lat_avg, msg.stats.lat_min, msg.stats.lat_p99, msg.stats.pnl, msg.stats.trades, msg.stats.wins};
            pub->publish(&b, sizeof(b));
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  Execution Simulator
// ─────────────────────────────────────────────────────────────
class ExecutionSimulator {
    int64_t position_qty{0};
    int64_t entry_price{0};
    int64_t stop_loss{0};
    int64_t take_profit{0};
    int64_t realized_pnl{0};
    uint64_t total_trades{0};
    uint64_t winning_trades{0};

public:
    void on_tick(const Tick& tick) {
        if (position_qty > 0) {
            if (tick.bid <= stop_loss || tick.bid >= take_profit) {
                close_position(tick.bid);
            }
        } else if (position_qty < 0) {
            if (tick.ask >= stop_loss || tick.ask <= take_profit) {
                close_position(tick.ask);
            }
        }
    }

    void on_signal(const Signal& sig, const Tick& tick) {
        if (position_qty != 0) return; 
        
        bool is_long = (sig.type == SignalType::LONG_EMA_CROSS || sig.type == SignalType::LONG_MOMENTUM || sig.type == SignalType::LONG_RSI_OVERSOLD || sig.type == SignalType::LONG_VWAP_RECLAIM);
        
        position_qty = is_long ? 1 : -1;
        entry_price = is_long ? tick.ask : tick.bid;
        stop_loss = sig.stop_loss;
        take_profit = sig.take_profit;
    }

private:
    void close_position(int64_t exit_price) {
        int64_t pnl = position_qty > 0 ? (exit_price - entry_price) : (entry_price - exit_price);
        realized_pnl += pnl;
        total_trades++;
        if (pnl > 0) winning_trades++;
        
        position_qty = 0;
        entry_price = 0;
        stop_loss = 0;
        take_profit = 0;
    }
    
public:
    int64_t get_pnl() const { return realized_pnl; }
    uint64_t get_trades() const { return total_trades; }
    uint64_t get_wins() const { return winning_trades; }
};

// ─────────────────────────────────────────────────────────────
//  SECTION 4 — Threads & Main
// ─────────────────────────────────────────────────────────────

using TickQueue = SPSCQueue<Tick, 4096>;
using SignalQueue = SPSCQueue<Signal, 256>;

struct Engine
{
    TickQueue tick_q;
    SignalQueue signal_q;
    TelQueue tel_q;

    BarBuilder bar_builder;
    IndicatorEngine indicators;
    OrderBook order_book;
    LatencyTracker latency;
    ExecutionSimulator exec_sim;
    MmapPublisher mmap_pub;

    std::atomic<bool> running{true};
    std::atomic<uint64_t> tick_count{0};
    std::atomic<uint64_t> signal_count{0};
};

static void signal_thread(Engine *eng)
{
    pin_thread(3);
    Tick tick;
    Bar bar;
    Signal sig;
    uint64_t stats_counter = 0;

    // Load dynamic risk config and init your custom strategy
    RiskConfig config;
    config.min_signal_strength = 75; // Lower to get more trades!
    SwingMomentumStrategy my_strategy(config);

    while (eng->running.load(std::memory_order_relaxed))
    {
        if (!eng->tick_q.pop(tick))
        {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#else
            asm volatile("yield" ::: "memory");
#endif
            continue;
        }

        uint64_t t0 = now_ns();
        if (tick.ts_ns == 0) tick.ts_ns = now_ns();
        eng->tick_count.fetch_add(1, std::memory_order_relaxed);

        eng->exec_sim.on_tick(tick);

        eng->order_book.update_bid(tick.bid, (uint64_t)tick.last_size);
        eng->order_book.update_ask(tick.ask, (uint64_t)tick.last_size);

        TelemetryMsg tm;
        tm.kind = TelemetryMsg::TICK;
        tm.tick = {tick.price, tick.bid, tick.ask, tick.ts_ns};
        eng->tel_q.push(tm);

        if (eng->bar_builder.on_tick(tick, bar))
        {
            eng->indicators.update(bar);

            TelemetryMsg bm;
            bm.kind = TelemetryMsg::BAR;
            bm.bar = {bar.open, bar.high, bar.low, bar.close, bar.volume, bar.ts_close_ns};
            eng->tel_q.push(bm);

            // ---> Evaluate Custom Strategy from src/strategies/my_strategy.hpp <---
            if (my_strategy.evaluate(bar, eng->indicators, sig))
            {
                sig.tick_ts_ns = bar.ts_close_ns;
                sig.ts_ns = now_ns();

                eng->exec_sim.on_signal(sig, tick);

                while (!eng->signal_q.push(sig)) { /* spin */ }
                eng->signal_count.fetch_add(1, std::memory_order_relaxed);

                TelemetryMsg sm;
                sm.kind = TelemetryMsg::SIGNAL;
                sm.sig = {sig.type, sig.strength, sig.entry_price, sig.stop_loss, sig.take_profit, sig.ts_ns - sig.tick_ts_ns};
                eng->tel_q.push(sm);
            }
        }

        eng->latency.record(t0, now_ns());
        if (++stats_counter % 50000 == 0)
        {
            eng->latency.print_histogram();
            
            TelemetryMsg stm;
            stm.kind = TelemetryMsg::STATS;
            stm.stats = {eng->tick_count.load(), eng->signal_count.load(), eng->latency.avg_ns(), eng->latency.min_ns(), eng->latency.p99_ns(), eng->exec_sim.get_pnl(), eng->exec_sim.get_trades(), eng->exec_sim.get_wins()};
            eng->tel_q.push(stm);
        }
    }
}

static void feed_thread(Engine *eng, FeedHandler* feed)
{
    pin_thread(2);
    feed->start(eng->tick_q, eng->running);
}

int main(int argc, char* argv[])
{
    calibrate_tsc();
    printf("[HFT] TSC frequency: %.3f GHz\n", rdtsc_hz / 1e9);

    Engine eng;
    if (!eng.mmap_pub.init())
    {
        fprintf(stderr, "[HFT] Mmap bind failed — check permissions for /tmp\n");
        return 1;
    }
    printf("[HFT] Mmap telemetry → /tmp/hft_telemetry.mmap\n");

    FeedHandler* feed = nullptr;
    if (argc > 1) {
        feed = new CsvReplayFeed(argv[1], 10);
        printf("[HFT] Replaying CSV Feed: %s\n", argv[1]);
    } else {
        feed = new RandomFeed();
        printf("[HFT] Running Random Feed\n");
    }

    pthread_t feed_tid, signal_tid, tel_tid;
    pthread_create(&feed_tid, nullptr, [](void *a) -> void *
                   { 
                       auto p = (std::pair<Engine*, FeedHandler*>*)a;
                       feed_thread(p->first, p->second); 
                       return nullptr; 
                   }, new std::pair<Engine*, FeedHandler*>(&eng, feed));
    pthread_create(&signal_tid, nullptr, [](void *a) -> void *
                   { signal_thread((Engine*)a); return nullptr; }, &eng);
    pthread_create(&tel_tid, nullptr, [](void *a) -> void *
                   {
        auto* p = (std::pair<TelQueue*, MmapPublisher*>*)a;
        telemetry_thread_fn(p->first, p->second);
        return nullptr; }, new std::pair<TelQueue *, MmapPublisher *>(&eng.tel_q, &eng.mmap_pub));

    printf("[HFT] Engine running! Launch Python Dashboard now.\n");

    for (;;)
    {
        sleep(1);
    } // Main thread waits forever
    return 0;
}