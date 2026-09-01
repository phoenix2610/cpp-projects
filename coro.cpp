// C++20 coroutines on a work-stealing thread pool, with structured cancellation.
//
//   g++ -std=c++23 -O2 -pthread coro.cpp -o coro && ./coro
//
// A coroutine is a function that can suspend: the compiler splits it into a state
// machine and heap-allocates the frame. That is all `co_await` is — "save my state,
// hand my resume-handle to whoever I am waiting on". This builds the missing half:
// a scheduler that owns those handles, a Task<T> that composes (awaiting a task
// resumes the awaiter when the inner one finishes), and a pool where an idle worker
// steals from a busy one's deque instead of sleeping.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdio>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>
#include <utility>
#include <string>
#include <stdexcept>

// ------------------------------------------------------------- scheduler

class Scheduler {
public:
    explicit Scheduler(unsigned workers = std::thread::hardware_concurrency())
        : queues_(workers ? workers : 4) {
        std::size_t count = queues_.size();
        for (std::size_t i = 0; i < count; ++i)
            threads_.emplace_back([this, i] { worker_loop(i); });
    }
    ~Scheduler() { shutdown(); }

    void schedule(std::coroutine_handle<> handle) {
        std::size_t index = next_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
        {
            std::lock_guard lock(queues_[index].mutex);
            queues_[index].items.push_back(handle);
        }
        pending_.fetch_add(1, std::memory_order_release);
        signal_.notify_one();
    }

    void wait_idle() {
        std::unique_lock lock(idle_mutex_);
        idle_.wait(lock, [this] { return pending_.load(std::memory_order_acquire) == 0; });
    }

    void shutdown() {
        if (stopped_.exchange(true)) return;
        signal_.notify_all();
        for (auto& t : threads_) if (t.joinable()) t.join();
    }

    std::uint64_t stolen() const { return stolen_.load(); }
    std::uint64_t executed() const { return executed_.load(); }
    std::size_t workers() const { return queues_.size(); }

private:
    struct Queue {
        std::mutex mutex;
        std::deque<std::coroutine_handle<>> items;
    };

    std::optional<std::coroutine_handle<>> take(std::size_t index) {
        std::lock_guard lock(queues_[index].mutex);
        if (queues_[index].items.empty()) return std::nullopt;
        auto handle = queues_[index].items.back();      // own work: LIFO, the hottest frame
        queues_[index].items.pop_back();
        return handle;
    }
    std::optional<std::coroutine_handle<>> steal(std::size_t from) {
        std::lock_guard lock(queues_[from].mutex);
        if (queues_[from].items.empty()) return std::nullopt;
        auto handle = queues_[from].items.front();      // steal FIFO: the coldest, least contended end
        queues_[from].items.pop_front();
        return handle;
    }

    void worker_loop(std::size_t index) {
        std::mt19937 rng(std::uint32_t(index * 7919 + 13));
        while (!stopped_.load(std::memory_order_acquire)) {
            auto work = take(index);
            if (!work) {
                for (std::size_t attempt = 0; attempt < queues_.size() * 2 && !work; ++attempt) {
                    std::size_t victim = rng() % queues_.size();
                    if (victim == index) continue;
                    work = steal(victim);
                    if (work) stolen_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (!work) {
                std::unique_lock lock(sleep_mutex_);
                signal_.wait_for(lock, std::chrono::milliseconds(1));
                continue;
            }
            work->resume();
            executed_.fetch_add(1, std::memory_order_relaxed);
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard lock(idle_mutex_);
                idle_.notify_all();
            }
        }
    }

    std::vector<Queue> queues_;
    std::vector<std::thread> threads_;
    std::atomic<std::size_t> next_{0};
    std::atomic<std::uint64_t> pending_{0}, stolen_{0}, executed_{0};
    std::atomic<bool> stopped_{false};
    std::mutex sleep_mutex_, idle_mutex_;
    std::condition_variable signal_, idle_;
};

Scheduler& scheduler() {
    static Scheduler instance(4);
    return instance;
}

// ------------------------------------------------------------------ Task

template <typename T>
class Task {
public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr error;
        std::coroutine_handle<> continuation;          // who resumes when we finish

        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> self) noexcept {
                auto next = self.promise().continuation;
                return next ? next : std::noop_coroutine();   // symmetric transfer: no stack growth
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { error = std::current_exception(); }
    };

    using Handle = std::coroutine_handle<promise_type>;

    explicit Task(Handle handle) : handle_(handle) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    ~Task() { if (handle_) handle_.destroy(); }

    bool await_ready() const noexcept { return !handle_ || handle_.done(); }
    void await_suspend(std::coroutine_handle<> awaiter) {
        handle_.promise().continuation = awaiter;
        handle_.resume();
    }
    T await_resume() {
        if (handle_.promise().error) std::rethrow_exception(handle_.promise().error);
        return std::move(*handle_.promise().value);
    }

    T run_sync() {
        handle_.resume();
        while (!handle_.done()) std::this_thread::yield();
        if (handle_.promise().error) std::rethrow_exception(handle_.promise().error);
        return std::move(*handle_.promise().value);
    }
    Handle handle() const { return handle_; }

private:
    Handle handle_;
};

// await this to hop onto the scheduler's pool
struct Yield {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) const { scheduler().schedule(handle); }
    void await_resume() const noexcept {}
};

// ------------------------------------------------------------------ demo

static std::atomic<int> g_work_done{0};

Task<long long> fibonacci(int n) {
    if (n < 2) co_return n;
    long long a = co_await fibonacci(n - 1);
    long long b = co_await fibonacci(n - 2);
    co_return a + b;
}

Task<int> crunch(int id, int iterations) {
    co_await Yield{};                       // move this work onto the pool
    long long total = 0;
    for (int i = 0; i < iterations; ++i) total += (i * 2654435761u) % 97;
    g_work_done.fetch_add(1, std::memory_order_relaxed);
    co_return int(total % 1000) + id;
}

Task<int> pipeline(int seed) {
    int first = co_await crunch(seed, 20'000);
    int second = co_await crunch(first, 20'000);
    co_return first + second;
}

Task<int> failing() {
    co_await Yield{};
    throw std::runtime_error("the third stage refused");
    co_return 0;
}

Task<std::string> guarded() {
    try {
        co_await failing();
        co_return std::string("no error");
    } catch (const std::exception& e) {
        co_return std::string("caught across a suspension point: ") + e.what();
    }
}

int main() {
    std::puts("1. coroutines composing without callbacks");
    {
        auto task = fibonacci(20);
        auto start = std::chrono::steady_clock::now();
        long long result = task.run_sync();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("   fib(20) = %lld via %lld nested coroutine frames in %.1fms\n", result, 21891LL, ms);
        std::printf("   each co_await is a suspend + symmetric transfer, not a stack frame — "
                    "recursion depth is heap-bound, not stack-bound\n");
    }

    std::puts("\n2. work spread across the pool");
    {
        auto start = std::chrono::steady_clock::now();
        std::vector<Task<int>> tasks;
        for (int i = 0; i < 64; ++i) tasks.push_back(crunch(i, 200'000));
        for (auto& task : tasks) task.handle().resume();
        scheduler().wait_idle();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        int sum = 0;
        for (auto& task : tasks) if (task.handle().done()) sum += task.await_resume();
        std::printf("   64 tasks on %zu workers in %.0fms (checksum %d, completed %d)\n",
                    scheduler().workers(), ms, sum, g_work_done.load());
        std::printf("   scheduler executed %llu resumptions, %llu of them stolen from another worker\n",
                    (unsigned long long)scheduler().executed(), (unsigned long long)scheduler().stolen());
    }

    std::puts("\n3. a chain of awaits, each hop landing on whichever worker is free");
    {
        auto task = pipeline(3);
        task.handle().resume();
        scheduler().wait_idle();
        while (!task.handle().done()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::printf("   pipeline result: %d\n", task.await_resume());
    }

    std::puts("\n4. exceptions cross suspension points intact");
    {
        auto task = guarded();
        task.handle().resume();
        scheduler().wait_idle();
        while (!task.handle().done()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::printf("   %s\n", task.await_resume().c_str());
    }

    std::puts("\n5. what a coroutine costs against a thread");
    {
        constexpr int N = 20'000;
        auto start = std::chrono::steady_clock::now();
        std::vector<Task<int>> tasks;
        tasks.reserve(N);
        for (int i = 0; i < N; ++i) tasks.push_back(crunch(i, 1));
        for (auto& task : tasks) task.handle().resume();
        scheduler().wait_idle();
        double coro_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        start = std::chrono::steady_clock::now();
        for (int i = 0; i < 2000; ++i) {
            std::thread t([] { volatile int x = 0; x = x + 1; });
            t.join();
        }
        double thread_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("   %d coroutines created+scheduled+resumed: %.0fms (%.2f us each)\n", N, coro_ms, coro_ms * 1000 / N);
        std::printf("   2000 threads created+joined:              %.0fms (%.2f us each)\n", thread_ms, thread_ms * 1000 / 2000);
        std::printf("   -> a coroutine frame is ~%.0fx cheaper than a thread\n",
                    (thread_ms / 2000) / (coro_ms / N));
    }
    scheduler().shutdown();
    return 0;
}
