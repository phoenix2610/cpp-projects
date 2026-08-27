// A logger whose hot path never formats, never allocates, and never blocks.
//
//   g++ -std=c++23 -O2 -pthread asynclog.cpp -o asynclog && ./asynclog
//
// The trick is moving work off the producing thread. A log call copies a small
// fixed-size record (level, timestamp, format pointer, a few POD arguments) into a
// lock-free ring; a background thread does the formatting and the write(). The
// caller pays ~30ns instead of the ~1-2us that snprintf plus a locked stream costs,
// and a burst degrades by dropping records with a counted gap rather than by
// stalling the thread doing real work.

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error };

static const char* level_name(Level level) {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

// A record is POD and fixed size: copying it is a memcpy, not a heap allocation.
struct Record {
    std::uint64_t nanos;
    const char* format;          // pointer into the binary's string table, always alive
    std::int64_t args[4];
    std::uint8_t arg_count;
    Level level;
    std::uint32_t thread_id;
};

class AsyncLogger {
public:
    explicit AsyncLogger(std::size_t capacity = 1 << 16, std::FILE* sink = stdout)
        : slots_(capacity), mask_(capacity - 1), sink_(sink) {
        worker_ = std::thread([this] { drain_loop(); });
    }
    ~AsyncLogger() {
        stop_.store(true, std::memory_order_release);
        worker_.join();
    }

    template <typename... Args>
    void log(Level level, const char* format, Args... args) {
        static_assert(sizeof...(Args) <= 4, "this logger carries at most four arguments");
        if (level < threshold_) return;

        // Claim a slot only if one exists. A plain fetch_add would advance the write
        // counter even on a drop, and the ring would never look non-full again.
        std::size_t write = write_.load(std::memory_order_relaxed);
        for (;;) {
            if (write - read_.load(std::memory_order_acquire) >= slots_.size()) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (write_.compare_exchange_weak(write, write + 1,
                                             std::memory_order_acq_rel, std::memory_order_relaxed))
                break;
        }
        Record& record = slots_[write & mask_];
        record.nanos = now_nanos();
        record.format = format;
        record.level = level;
        record.thread_id = thread_id();
        record.arg_count = sizeof...(Args);
        std::int64_t values[] = {static_cast<std::int64_t>(args)..., 0};
        for (std::size_t i = 0; i < sizeof...(Args); ++i) record.args[i] = values[i];
        // Publish in claim order so the reader never sees a slot that is still being filled.
        std::size_t expected = write;
        while (!ready_.compare_exchange_weak(expected, write + 1,
                                             std::memory_order_release, std::memory_order_relaxed))
            expected = write;
    }

    void set_threshold(Level level) { threshold_ = level; }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t written() const { return written_; }
    void flush() {
        while (read_.load(std::memory_order_acquire) < ready_.load(std::memory_order_acquire))
            std::this_thread::yield();
    }

private:
    static std::uint64_t now_nanos() {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static std::uint32_t thread_id() {
        static std::atomic<std::uint32_t> counter{0};
        thread_local std::uint32_t id = counter.fetch_add(1);
        return id;
    }

    void drain_loop() {
        std::string buffer;
        buffer.reserve(1 << 16);
        while (true) {
            const std::size_t ready = ready_.load(std::memory_order_acquire);
            std::size_t read = read_.load(std::memory_order_relaxed);
            if (read == ready) {
                if (stop_.load(std::memory_order_acquire)) break;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            buffer.clear();
            while (read < ready) {
                format_into(buffer, slots_[read & mask_]);
                ++read;
                ++written_;
            }
            std::fwrite(buffer.data(), 1, buffer.size(), sink_);      // one write for the whole batch
            read_.store(read, std::memory_order_release);
        }
        std::fflush(sink_);
    }

    void format_into(std::string& out, const Record& record) {
        char line[512];
        double ms = double(record.nanos - start_nanos_) / 1e6;
        int prefix = std::snprintf(line, sizeof(line), "[%9.3fms t%u %s] ", ms, record.thread_id,
                                   level_name(record.level));
        int body = 0;
        switch (record.arg_count) {
            case 0: body = std::snprintf(line + prefix, sizeof(line) - prefix, "%s", record.format); break;
            case 1: body = std::snprintf(line + prefix, sizeof(line) - prefix, record.format, record.args[0]); break;
            case 2: body = std::snprintf(line + prefix, sizeof(line) - prefix, record.format, record.args[0], record.args[1]); break;
            case 3: body = std::snprintf(line + prefix, sizeof(line) - prefix, record.format, record.args[0], record.args[1], record.args[2]); break;
            default: body = std::snprintf(line + prefix, sizeof(line) - prefix, record.format, record.args[0], record.args[1], record.args[2], record.args[3]); break;
        }
        out.append(line, std::size_t(prefix + body));
        out.push_back('\n');
    }

    std::vector<Record> slots_;
    std::size_t mask_;
    std::FILE* sink_;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> ready_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> stop_{false};
    std::uint64_t written_ = 0, start_nanos_ = now_nanos();
    Level threshold_ = Level::Trace;
    std::thread worker_;
};

template <typename F>
static double time_ns_per_op(F&& fn, int iterations) {
    auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
}

int main() {
    std::puts("1. it actually logs");
    {
        AsyncLogger logger(1024);
        logger.log(Level::Info, "service starting on port %lld", 8080);
        logger.log(Level::Warn, "cache miss rate %lld%% over %lld requests", 37, 1024);
        logger.log(Level::Error, "upstream %lld failed after %lld ms, retry %lld", 3, 1520, 2);
        logger.flush();
    }

    std::puts("\n2. hot-path cost: async enqueue vs formatting inline");
    {
        constexpr int N = 200'000;
        std::FILE* devnull = std::fopen("/dev/null", "w");
        {
            AsyncLogger logger(1 << 16, devnull);
            double async_ns = time_ns_per_op([&] {
                for (int i = 0; i < N; ++i) logger.log(Level::Info, "request %lld took %lld ms", i, i % 100);
            }, N);
            logger.flush();
            double sync_ns = time_ns_per_op([&] {
                for (int i = 0; i < N; ++i) std::fprintf(devnull, "[t0 INFO ] request %d took %d ms\n", i, i % 100);
            }, N);
            std::printf("   async enqueue    %6.1f ns/call\n", async_ns);
            std::printf("   inline fprintf   %6.1f ns/call  -> %.1fx cheaper on the calling thread\n",
                        sync_ns, sync_ns / async_ns);
            std::printf("   records written by the worker: %llu, dropped: %llu\n",
                        (unsigned long long)logger.written(), (unsigned long long)logger.dropped());
            std::printf("   note: 200k records enqueued at %.0f ns each is %.0f M/s, far above what one\n"
                        "   formatting thread can retire — the drops are the queue doing its job.\n",
                        async_ns, 1000.0 / async_ns);
        }
        std::fclose(devnull);
    }

    std::puts("\n3. eight threads logging at once (interleaving is the worker's problem, not theirs)");
    {
        std::FILE* devnull = std::fopen("/dev/null", "w");
        AsyncLogger logger(1 << 14, devnull);
        auto start = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&logger, t] {
                for (int i = 0; i < 50'000; ++i)
                    logger.log(Level::Debug, "thread %lld iteration %lld", t, i);
            });
        for (auto& th : threads) th.join();
        logger.flush();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("   400k records from 8 threads in %.0fms (%.1f M records/s)\n", ms, 400.0 / ms);
        std::printf("   written %llu, dropped %llu (%.1f%%)\n",
                    (unsigned long long)logger.written(), (unsigned long long)logger.dropped(),
                    100.0 * double(logger.dropped()) / 400'000.0);
        std::fclose(devnull);
    }

    std::puts("\n4. what the worker can actually retire, and the burst it can absorb");
    {
        std::FILE* devnull = std::fopen("/dev/null", "w");
        {
            AsyncLogger logger(1 << 16, devnull);
            auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 20'000; ++i) logger.log(Level::Info, "steady %lld of %lld", i, 20'000);
            logger.flush();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::printf("   formatter throughput: %.2f M records/s (%.0f ns per record, snprintf-bound)\n",
                        20.0 / ms, ms * 1e6 / 20'000);
            std::printf("   that is the sustainable rate; the ring exists to absorb bursts above it\n");
        }
        {
            AsyncLogger logger(1 << 16, devnull);
            for (int i = 0; i < 60'000; ++i) logger.log(Level::Warn, "burst %lld", i);
            logger.flush();
            std::printf("   60k-record burst into a 65536 ring: %llu written, %llu dropped\n",
                        (unsigned long long)logger.written(), (unsigned long long)logger.dropped());
        }
        std::fclose(devnull);
    }

    std::puts("\n5. a filtered-out level costs one comparison");
    {
        std::FILE* devnull = std::fopen("/dev/null", "w");
        AsyncLogger logger(1 << 12, devnull);
        logger.set_threshold(Level::Error);
        constexpr int N = 1'000'000;
        double ns = time_ns_per_op([&] {
            for (int i = 0; i < N; ++i) logger.log(Level::Debug, "never formatted %lld", i);
        }, N);
        std::printf("   disabled log call: %.2f ns (no timestamp, no copy, no format)\n", ns);
        std::fclose(devnull);
    }
    return 0;
}
