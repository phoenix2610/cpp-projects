// A sampling profiler: interrupt on a timer, walk the stack, fold samples into a flame graph.
//
//   g++ -std=c++23 -O2 -g -rdynamic -pthread sampler.cpp -o sampler && ./sampler
//
// Instrumenting every function call changes what you are measuring — the overhead
// lands hardest on the small hot functions you most want to see. Sampling asks a
// different question: "what is on the stack right now?", a few hundred times a
// second. Cost is fixed and tiny, and the answer converges on where time actually
// goes. The signal handler here does the one safe thing available to it: backtrace()
// into a preallocated buffer. Symbol resolution happens later, off the hot path.

#include <cxxabi.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int kMaxFrames = 48;
constexpr int kMaxSamples = 200'000;
constexpr int kHandlerFrames = 2;   // on_tick + the signal trampoline

struct Sample { int depth; void* frames[kMaxFrames]; };

Sample* g_samples = nullptr;
std::atomic<int> g_sample_count{0};
std::atomic<bool> g_running{false};
std::atomic<long> g_signals{0};

// Everything here must be async-signal-safe: no malloc, no locks, no formatting.
void on_tick(int, siginfo_t*, void*) {
    g_signals.fetch_add(1, std::memory_order_relaxed);
    if (!g_running.load(std::memory_order_relaxed)) return;
    int slot = g_sample_count.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kMaxSamples) return;
    g_samples[slot].depth = ::backtrace(g_samples[slot].frames, kMaxFrames);
}

std::string demangle_frame(const std::string& raw) {
    // "./sampler(_Z6workerii+0x1f) [0x55...]" -> "worker(int, int)"
    auto open = raw.find('(');
    auto plus = raw.find('+', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || plus == std::string::npos || plus <= open + 1) {
        // no symbol name: fall back to the library it came from, which is still informative
        // (time in libm means sin/sqrt, and you cannot see that from an address)
        std::string object = raw.substr(0, open == std::string::npos ? raw.find(' ') : open);
        auto slash = object.rfind('/');
        if (slash != std::string::npos) object = object.substr(slash + 1);
        return object.empty() ? raw : object + " (no symbols)";
    }
    std::string mangled = raw.substr(open + 1, plus - open - 1);
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string result(demangled);
        std::free(demangled);
        return result;
    }
    return mangled;
}

}  // namespace

class Profiler {
public:
    explicit Profiler(int hertz = 500) : hertz_(hertz) {
        g_samples = new Sample[kMaxSamples];
    }
    ~Profiler() { stop(); delete[] g_samples; }

    void start() {
        struct sigaction action{};
        action.sa_sigaction = on_tick;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        ::sigemptyset(&action.sa_mask);
        ::sigaction(SIGPROF, &action, nullptr);

        itimerval timer{};
        timer.it_interval.tv_usec = 1'000'000 / hertz_;
        timer.it_value = timer.it_interval;
        // ITIMER_PROF fires on CPU time, not wall clock — a sleeping thread is not sampled
        ::setitimer(ITIMER_PROF, &timer, nullptr);
        g_sample_count.store(0);
        g_running.store(true);
        started_ = std::chrono::steady_clock::now();
    }

    void stop() {
        if (!g_running.exchange(false)) return;
        itimerval off{};
        ::setitimer(ITIMER_PROF, &off, nullptr);
        elapsed_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    }

    struct Report {
        std::map<std::string, int> self;         // samples where this frame was on top
        std::map<std::string, int> total;        // samples where it appeared anywhere
        std::map<std::string, int> stacks;       // folded stacks, flame-graph format
        int samples = 0;
    };

    Report analyse(int max_depth = 12) const {
        Report report;
        int count = std::min(g_sample_count.load(), kMaxSamples);
        report.samples = count;
        for (int i = 0; i < count; ++i) {
            const Sample& sample = g_samples[i];
            if (sample.depth <= 0) continue;
            char** symbols = ::backtrace_symbols(sample.frames, sample.depth);
            if (!symbols) continue;

            // Drop the profiler's own frames. backtrace() was called inside the handler,
            // so frame 0 is on_tick and frame 1 is the kernel's signal trampoline — and
            // neither resolves to a name (static function, vdso), so this cannot be done
            // by matching symbols. The count is fixed, so skip by position.
            const int start = std::min(kHandlerFrames, sample.depth);

            std::vector<std::string> names;
            for (int f = start; f < sample.depth; ++f) {
                std::string name = demangle_frame(symbols[f]);
                if (name.find("Profiler") != std::string::npos) continue;
                names.push_back(name);
            }
            std::free(symbols);
            if (names.empty()) continue;

            report.self[names.front()]++;
            std::vector<std::string> seen;
            for (const std::string& name : names)
                if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
                    report.total[name]++;
                    seen.push_back(name);
                }

            std::string folded;
            int depth = 0;
            for (auto it = names.rbegin(); it != names.rend() && depth < max_depth; ++it, ++depth)
                folded += (folded.empty() ? "" : ";") + *it;
            report.stacks[folded]++;
        }
        return report;
    }

    double elapsed() const { return elapsed_; }
    long signals() const { return g_signals.load(); }

private:
    int hertz_;
    std::chrono::steady_clock::time_point started_;
    double elapsed_ = 0;
};

// ------------------------------------------------- a program worth profiling

__attribute__((noinline)) double hot_math(int iterations) {
    double total = 0;
    for (int i = 1; i <= iterations; ++i) total += std::sqrt(double(i)) * std::sin(double(i));
    return total;
}

__attribute__((noinline)) double slow_string_work(int iterations) {
    std::string buffer;
    for (int i = 0; i < iterations; ++i) buffer += std::to_string(i % 10);   // quadratic-ish churn
    return double(buffer.size());
}

__attribute__((noinline)) double middle_layer(int iterations) {
    return hot_math(iterations * 8) + slow_string_work(iterations);
}

__attribute__((noinline)) double rarely_called(int iterations) {
    return hot_math(iterations / 40);
}

__attribute__((noinline)) double application(int rounds) {
    double total = 0;
    for (int round = 0; round < rounds; ++round) {
        total += middle_layer(4000);
        if (round % 25 == 0) total += rarely_called(4000);
    }
    return total;
}

static void print_bar(int value, int peak, int width = 34) {
    int filled = peak ? std::max(1, value * width / peak) : 0;
    for (int i = 0; i < filled; ++i) std::putchar('#');
    for (int i = filled; i < width; ++i) std::putchar(' ');
}

int main(int argc, char** argv) {
    int hertz = 500, rounds = 3000;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--hz" && i + 1 < argc) hertz = std::atoi(argv[++i]);
        if (arg == "--rounds" && i + 1 < argc) rounds = std::atoi(argv[++i]);
    }

    Profiler profiler(hertz);
    std::printf("profiling at %d Hz...\n", hertz);
    profiler.start();
    double result = application(rounds);
    profiler.stop();

    auto report = profiler.analyse();
    std::printf("\n%d samples over %.2fs of CPU time (result %.1f, %ld timer signals)\n",
                report.samples, profiler.elapsed(), result, profiler.signals());
    std::printf("sampling overhead: about %.3f%% of runtime\n\n",
                100.0 * report.samples * 2e-6 / std::max(profiler.elapsed(), 1e-9));

    std::vector<std::pair<int, std::string>> self, total;
    for (const auto& [name, count] : report.self) self.emplace_back(count, name);
    for (const auto& [name, count] : report.total) total.emplace_back(count, name);
    std::sort(self.rbegin(), self.rend());
    std::sort(total.rbegin(), total.rend());

    std::puts("self time — where the CPU actually was:");
    int peak = self.empty() ? 1 : self.front().first;
    for (std::size_t i = 0; i < std::min<std::size_t>(8, self.size()); ++i) {
        std::printf("  %5.1f%%  ", 100.0 * self[i].first / std::max(report.samples, 1));
        print_bar(self[i].first, peak);
        std::printf("  %s\n", self[i].second.substr(0, 52).c_str());
    }

    std::puts("\ntotal time — including everything called from here:");
    for (std::size_t i = 0; i < std::min<std::size_t>(8, total.size()); ++i)
        std::printf("  %5.1f%%  %s\n", 100.0 * total[i].first / std::max(report.samples, 1),
                    total[i].second.substr(0, 60).c_str());

    std::puts("\nfolded stacks (feed these straight to flamegraph.pl):");
    std::vector<std::pair<int, std::string>> stacks;
    for (const auto& [stack, count] : report.stacks) stacks.emplace_back(count, stack);
    std::sort(stacks.rbegin(), stacks.rend());
    for (std::size_t i = 0; i < std::min<std::size_t>(4, stacks.size()); ++i) {
        std::string line = stacks[i].second;
        if (line.size() > 96) line = "..." + line.substr(line.size() - 93);
        std::printf("  %s %d\n", line.c_str(), stacks[i].first);
    }
    return 0;
}
