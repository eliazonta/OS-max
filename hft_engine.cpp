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
#include <zmq.h>

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
constexpr double TICK_SIZE = 0.01;
struct Level
{
    double price;
    uint64_t qty;
};

class alignas(CACHE_LINE) OrderBook
{
public:
    void reset(double mid) noexcept
    {
        mid_price_ = mid;
        for (int i = 0; i < BOOK_LEVELS; i++)
        {
            bids_[i] = {mid - (i + 1) * TICK_SIZE, 0};
            asks_[i] = {mid + (i + 1) * TICK_SIZE, 0};
        }
    }
    void update_bid(double price, uint64_t qty) noexcept
    {
        int idx = (int)((mid_price_ - price) / TICK_SIZE) - 1;
        if (idx >= 0 && idx < BOOK_LEVELS)
        {
            bids_[idx].qty = qty;
            bids_[idx].price = price;
        }
    }
    void update_ask(double price, uint64_t qty) noexcept
    {
        int idx = (int)((price - mid_price_) / TICK_SIZE) - 1;
        if (idx >= 0 && idx < BOOK_LEVELS)
        {
            asks_[idx].qty = qty;
            asks_[idx].price = price;
        }
    }

private:
    double mid_price_{0.0};
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

private:
    uint64_t samples_[LAT_SAMPLES]{};
    uint64_t head_{0}, sum_{0};
    uint64_t min_{UINT64_MAX}, max_{0};
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
            double price, bid, ask;
            uint64_t ts_ns;
        } tick;
        struct
        {
            float o, h, l, c;
            uint32_t vol;
            uint64_t ts;
        } bar;
        struct
        {
            SignalType type;
            uint8_t strength;
            float entry, sl, tp;
            uint64_t lag_ns;
        } sig;
        struct
        {
            uint64_t tick_count, signal_count;
            uint64_t lat_avg, lat_min, lat_p99;
        } stats;
    };
};

using TelQueue = SPSCQueue<TelemetryMsg, 2048>;

struct ZmqPublisher
{
    void *ctx{nullptr};
    void *sock{nullptr};
    bool init(const char *endpoint = "tcp://*:5555")
    {
        ctx = zmq_ctx_new();
        sock = zmq_socket(ctx, ZMQ_PUB);
        int sndhwm = 1000;
        zmq_setsockopt(sock, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
        return zmq_bind(sock, endpoint) == 0;
    }
    void publish(const char *json, int len) { zmq_send(sock, json, len, ZMQ_NOBLOCK); }
};

static void telemetry_thread_fn(TelQueue *q, ZmqPublisher *pub)
{
    char buf[512];
    TelemetryMsg msg;
    while (true)
    {
        if (!q->pop(msg))
        {
            usleep(100);
            continue;
        }
        int n = 0;
        if (msg.kind == TelemetryMsg::TICK)
        {
            n = snprintf(buf, sizeof(buf), "{\"type\":\"tick\",\"price\":%.4f,\"bid\":%.4f,\"ask\":%.4f,\"ts_ns\":%llu}",
                         msg.tick.price, msg.tick.bid, msg.tick.ask, (unsigned long long)msg.tick.ts_ns);
        }
        else if (msg.kind == TelemetryMsg::BAR)
        {
            n = snprintf(buf, sizeof(buf), "{\"type\":\"bar\",\"open\":%.4f,\"high\":%.4f,\"low\":%.4f,\"close\":%.4f,\"volume\":%u,\"ts\":%llu}",
                         msg.bar.o, msg.bar.h, msg.bar.l, msg.bar.c, msg.bar.vol, (unsigned long long)msg.bar.ts);
        }
        else if (msg.kind == TelemetryMsg::SIGNAL)
        {
            n = snprintf(buf, sizeof(buf), "{\"type\":\"signal\",\"signal_type\":\"%s\",\"strength\":%u,\"entry\":%.4f,\"sl\":%.4f,\"tp\":%.4f,\"lag_ns\":%llu}",
                         signal_name(msg.sig.type), msg.sig.strength, msg.sig.entry, msg.sig.sl, msg.sig.tp, (unsigned long long)msg.sig.lag_ns);
        }
        else if (msg.kind == TelemetryMsg::STATS)
        {
            n = snprintf(buf, sizeof(buf), "{\"type\":\"stats\",\"tick_count\":%llu,\"signal_count\":%llu,\"lat_avg_ns\":%llu,\"lat_min_ns\":%llu,\"lat_p99_ns\":%llu}",
                         (unsigned long long)msg.stats.tick_count, (unsigned long long)msg.stats.signal_count,
                         (unsigned long long)msg.stats.lat_avg, (unsigned long long)msg.stats.lat_min, (unsigned long long)msg.stats.lat_p99);
        }
        if (n > 0)
            pub->publish(buf, n);
    }
}

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
    ZmqPublisher zmq_pub;

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
        eng->tick_count.fetch_add(1, std::memory_order_relaxed);

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

                while (!eng->signal_q.push(sig)) { /* spin */ }
                eng->signal_count.fetch_add(1, std::memory_order_relaxed);

                TelemetryMsg sm;
                sm.kind = TelemetryMsg::SIGNAL;
                sm.sig = {sig.type, sig.strength, sig.entry_price, sig.stop_loss, sig.take_profit, sig.ts_ns - sig.tick_ts_ns};
                eng->tel_q.push(sm);
            }
        }

        eng->latency.record(t0, now_ns());
        if (++stats_counter % 500 == 0)
        {
            TelemetryMsg stm;
            stm.kind = TelemetryMsg::STATS;
            stm.stats = {eng->tick_count.load(), eng->signal_count.load(), eng->latency.avg_ns(), eng->latency.min_ns(), eng->latency.p99_ns()};
            eng->tel_q.push(stm);
        }
    }
}

static void feed_thread(Engine *eng)
{
    pin_thread(2);
    double price = 445.00;
    eng->order_book.reset(price);

    while (eng->running.load(std::memory_order_relaxed))
    {
        price += (double)(rand() - RAND_MAX / 2) / (RAND_MAX * 10.0);
        Tick t{};
        t.price = price;
        t.bid = price - 0.005;
        t.ask = price + 0.005;
        t.last_size = 100.0 + (rand() % 900);
        t.ts_ns = now_ns();
        while (!eng->tick_q.push(t)) { /* spin */ }
        usleep(10);
    }
}

int main()
{
    calibrate_tsc();
    printf("[HFT] TSC frequency: %.3f GHz\n", rdtsc_hz / 1e9);

    Engine eng;
    if (!eng.zmq_pub.init())
    {
        fprintf(stderr, "[HFT] ZMQ bind failed — run 'killall hft_engine'\n");
        return 1;
    }
    printf("[HFT] ZMQ telemetry → tcp://*:5555\n");

    pthread_t feed_tid, signal_tid, tel_tid;
    pthread_create(&feed_tid, nullptr, [](void *a) -> void *
                   { feed_thread((Engine*)a); return nullptr; }, &eng);
    pthread_create(&signal_tid, nullptr, [](void *a) -> void *
                   { signal_thread((Engine*)a); return nullptr; }, &eng);
    pthread_create(&tel_tid, nullptr, [](void *a) -> void *
                   {
        auto* p = (std::pair<TelQueue*, ZmqPublisher*>*)a;
        telemetry_thread_fn(p->first, p->second);
        return nullptr; }, new std::pair<TelQueue *, ZmqPublisher *>(&eng.tel_q, &eng.zmq_pub));

    printf("[HFT] Engine running! Launch Python Dashboard now.\n");

    for (;;)
    {
        sleep(1);
    } // Main thread waits forever
    return 0;
}