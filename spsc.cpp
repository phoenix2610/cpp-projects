// A single-producer single-consumer ring buffer with no locks, and a proof it is correct.
//
//   g++ -std=c++23 -O2 -pthread spsc.cpp -o spsc && ./spsc
//
// Two atomics, two threads, one rule each: the producer owns `write_`, the
// consumer owns `read_`, and each publishes its index with release so the other
// sees the data written before it. The two details that decide whether this is
// fast or a disaster: the indices must sit on separate cache lines (otherwise
// every push invalidates the consumer's line — false sharing), and each side
// caches the other's index so it only re-reads the shared atomic when it looks
// empty or full.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <new>
#include <thread>
#include <vector>

#ifdef __cpp_lib_hardware_interference_size
constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr std::size_t kCacheLine = 64;
#endif

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    bool push(const T& value) {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1) & kMask;
        if (next == cached_read_) {                              // looks full — refresh the cache
            cached_read_ = read_.load(std::memory_order_acquire);
            if (next == cached_read_) return false;              // genuinely full
        }
        slots_[write] = value;
        write_.store(next, std::memory_order_release);           // publishes the slot write above
        return true;
    }

    bool pop(T& out) {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        if (read == cached_write_) {
            cached_write_ = write_.load(std::memory_order_acquire);
            if (read == cached_write_) return false;             // genuinely empty
        }
        out = slots_[read];
        read_.store((read + 1) & kMask, std::memory_order_release);
        return true;
    }

    std::size_t size() const {
        return (write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire)) & kMask;
    }
    static constexpr std::size_t capacity() { return Capacity - 1; }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    std::vector<T> slots_ = std::vector<T>(Capacity);

    alignas(kCacheLine) std::atomic<std::size_t> write_{0};
    std::size_t cached_read_ = 0;                                // producer-private
    alignas(kCacheLine) std::atomic<std::size_t> read_{0};
    std::size_t cached_write_ = 0;                               // consumer-private
    alignas(kCacheLine) char padding_[kCacheLine]{};
};

// The same queue with everything on one cache line, to measure what false sharing costs.
template <typename T, std::size_t Capacity>
class SharedLineQueue {
public:
    bool push(const T& value) {
        const std::size_t write = write_.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1) & kMask;
        if (next == read_.load(std::memory_order_acquire)) return false;
        slots_[write] = value;
        write_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        const std::size_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        out = slots_[read];
        read_.store((read + 1) & kMask, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    std::vector<T> slots_ = std::vector<T>(Capacity);
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};        // deliberately adjacent
};

struct Message { std::uint64_t sequence; std::uint64_t payload; };

template <typename Queue>
double run_throughput(Queue& queue, std::uint64_t messages) {
    std::atomic<bool> go{false};
    std::uint64_t checksum = 0;

    std::thread consumer([&] {
        while (!go.load(std::memory_order_acquire)) {}
        Message msg{};
        for (std::uint64_t received = 0; received < messages;) {
            if (queue.pop(msg)) { checksum += msg.payload; ++received; }
        }
    });

    auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (std::uint64_t i = 0; i < messages; ++i) {
        Message msg{i, i * 3};
        while (!queue.push(msg)) {}
    }
    consumer.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::uint64_t expected = 0;
    for (std::uint64_t i = 0; i < messages; ++i) expected += i * 3;
    if (checksum != expected) std::printf("   CHECKSUM MISMATCH: %llu vs %llu\n",
                                          (unsigned long long)checksum, (unsigned long long)expected);
    return ms;
}

int main() {
    std::puts("1. single-threaded correctness");
    {
        SpscQueue<int, 8> q;
        int value = 0;
        std::printf("   capacity %zu (one slot is reserved to tell full from empty)\n", q.capacity());
        std::printf("   pop on empty: %s\n", q.pop(value) ? "returned true (wrong)" : "false");
        int pushed = 0;
        while (q.push(pushed)) ++pushed;
        std::printf("   pushed %d before full, size reports %zu\n", pushed, q.size());
        int popped = 0, seen = 0;
        bool ordered = true;
        while (q.pop(value)) { ordered &= (value == popped++); ++seen; }
        std::printf("   popped %d in FIFO order: %s\n", seen, ordered ? "yes" : "NO");
    }

    std::puts("\n2. two threads, 10 million messages, checksummed");
    {
        constexpr std::uint64_t N = 10'000'000;
        SpscQueue<Message, 1024> queue;
        double ms = run_throughput(queue, N);
        std::printf("   padded queue      %7.1fms  %6.1f M msg/s  %5.1f ns/msg\n",
                    ms, N / ms / 1000.0, ms * 1e6 / N);

        SharedLineQueue<Message, 1024> crowded;
        double bad_ms = run_throughput(crowded, N);
        std::printf("   same line (false sharing) %7.1fms  %6.1f M msg/s  %5.1f ns/msg\n",
                    bad_ms, N / bad_ms / 1000.0, bad_ms * 1e6 / N);
        std::printf("   padding + index caching is worth %.2fx here\n", bad_ms / ms);
    }

    std::puts("\n3. queue depth under a slow consumer (backpressure, not unbounded growth)");
    {
        SpscQueue<Message, 64> queue;
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> dropped{0}, sent{0};
        std::thread consumer([&] {
            Message msg{};
            while (!stop.load(std::memory_order_relaxed)) {
                if (queue.pop(msg)) std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        });
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline) {
            if (queue.push(Message{sent, sent})) ++sent; else ++dropped;
        }
        stop = true;
        consumer.join();
        std::printf("   in 200ms: %llu accepted, %llu rejected by a full queue "
                    "(the producer learns immediately instead of queueing forever)\n",
                    (unsigned long long)sent.load(), (unsigned long long)dropped.load());
    }
    return 0;
}
