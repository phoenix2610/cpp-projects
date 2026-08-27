// A crash handler that survives the crash: async-signal-safe, allocation-free, on its own stack.
//
//   g++ -std=c++23 -O2 -g -rdynamic crashdump.cpp -o crashdump && ./crashdump --demo
//
// Almost every crash handler people write is unsafe. printf takes a lock the
// crashing thread may already hold; backtrace_symbols calls malloc, which may be
// the very thing that corrupted; and a stack-overflow crash has no stack left to
// run a handler on. The three fixes: write() only, an alternate signal stack
// installed with sigaltstack, and pre-rendered strings — nothing formatted at crash
// time. Then re-raise with the default handler so the exit status and core dump are
// still correct.

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr int kMaxFrames = 64;
char g_dump_path[256] = "./crash.log";
void* g_frames[kMaxFrames];                  // pre-allocated: no malloc during the handler
// glibc 2.34+ makes SIGSTKSZ a sysconf() call rather than a constant, so size the
// alternate stack ourselves — 64KB is plenty for write()-only handler frames.
constexpr std::size_t kAltStackSize = 64 * 1024;
char g_alt_stack[kAltStackSize];

// write() is on the async-signal-safe list; snprintf is not.
void write_all(int fd, const char* text, std::size_t length) {
    while (length > 0) {
        ssize_t written = ::write(fd, text, length);
        if (written <= 0) return;
        text += written;
        length -= std::size_t(written);
    }
}
void write_str(int fd, const char* text) { write_all(fd, text, ::strlen(text)); }

void write_hex(int fd, std::uint64_t value) {
    char buffer[19] = "0x";
    int pos = 2;
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        int nibble = int((value >> shift) & 0xF);
        if (nibble || started || shift == 0) {
            buffer[pos++] = "0123456789abcdef"[nibble];
            started = true;
        }
    }
    write_all(fd, buffer, std::size_t(pos));
}

void write_dec(int fd, long value) {
    char buffer[24];
    int pos = 24;
    bool negative = value < 0;
    unsigned long magnitude = negative ? (unsigned long)(-value) : (unsigned long)value;
    if (magnitude == 0) buffer[--pos] = '0';
    while (magnitude) { buffer[--pos] = char('0' + magnitude % 10); magnitude /= 10; }
    if (negative) buffer[--pos] = '-';
    write_all(fd, buffer + pos, std::size_t(24 - pos));
}

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory reference)";
        case SIGABRT: return "SIGABRT (abort called)";
        case SIGFPE:  return "SIGFPE (arithmetic error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGBUS:  return "SIGBUS (bad memory access)";
        default: return "unknown signal";
    }
}

const char* fault_reason(int sig, int code) {
    if (sig == SIGSEGV) return code == SEGV_MAPERR ? "address not mapped" : "no permission for that access";
    if (sig == SIGFPE)  return code == FPE_INTDIV ? "integer divide by zero" : "floating point error";
    if (sig == SIGBUS)  return code == BUS_ADRALN ? "misaligned address" : "physical address does not exist";
    return "";
}

void handler(int sig, siginfo_t* info, void* context) {
    int fd = ::open(g_dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) fd = STDERR_FILENO;

    write_str(fd, "\n=== crash ===\nsignal:   ");
    write_str(fd, signal_name(sig));
    write_str(fd, "\ncode:     ");
    write_str(fd, fault_reason(sig, info->si_code));
    write_str(fd, "\nfaulting address: ");
    write_hex(fd, reinterpret_cast<std::uint64_t>(info->si_addr));
    write_str(fd, "\npid:      ");
    write_dec(fd, ::getpid());

#if defined(__x86_64__)
    auto* uc = static_cast<ucontext_t*>(context);
    write_str(fd, "\nrip:      ");
    write_hex(fd, std::uint64_t(uc->uc_mcontext.gregs[REG_RIP]));
    write_str(fd, "\nrsp:      ");
    write_hex(fd, std::uint64_t(uc->uc_mcontext.gregs[REG_RSP]));
#else
    (void)context;
#endif

    // backtrace() itself is safe; backtrace_symbols() is not (it mallocs), so use the _fd form
    int depth = ::backtrace(g_frames, kMaxFrames);
    write_str(fd, "\n\nbacktrace (");
    write_dec(fd, depth);
    write_str(fd, " frames):\n");
    ::backtrace_symbols_fd(g_frames, depth, fd);

    write_str(fd, "\nre-raising with the default handler so the exit status is honest\n");
    if (fd != STDERR_FILENO) {
        ::close(fd);
        write_str(STDERR_FILENO, "crash dump written to ");
        write_str(STDERR_FILENO, g_dump_path);
        write_str(STDERR_FILENO, "\n");
    }

    struct sigaction restore{};
    restore.sa_handler = SIG_DFL;
    ::sigaction(sig, &restore, nullptr);
    ::raise(sig);
}

}  // namespace

void install_crash_handler(const char* dump_path) {
    ::strncpy(g_dump_path, dump_path, sizeof(g_dump_path) - 1);

    // an alternate stack is what makes a stack-overflow crash reportable at all
    stack_t alt{};
    alt.ss_sp = g_alt_stack;
    alt.ss_size = kAltStackSize;
    alt.ss_flags = 0;
    ::sigaltstack(&alt, nullptr);

    struct sigaction action{};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    ::sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) ::sigaction(sig, &action, nullptr);
}

// ------------------------------------------------------------------ demo

__attribute__((noinline)) int level_three(int* pointer) { return *pointer; }
__attribute__((noinline)) int level_two(int* pointer) { return level_three(pointer) + 1; }
__attribute__((noinline)) int level_one(int* pointer) { return level_two(pointer) + 1; }

// Threading the previous frame's address through the call keeps the optimiser from
// reusing (or eliding) the frame — otherwise -O2 turns this into a loop that spins
// forever instead of overflowing the stack.
__attribute__((noinline)) int recurse(int depth, volatile char* previous) {
    volatile char padding[8192];
    padding[0] = char(depth);
    padding[1] = previous ? previous[0] : 0;
    if (depth > 500'000) return padding[0];        // guard: never hang, even if the frame shrinks
    return padding[0] + recurse(depth + 1, padding);
}

static int run_crash(const char* mode, const char* dump_path) {
    install_crash_handler(dump_path);
    if (::strcmp(mode, "segv") == 0) return level_one(nullptr);
    if (::strcmp(mode, "abort") == 0) { std::abort(); }
    if (::strcmp(mode, "overflow") == 0) return recurse(0, nullptr);
    if (::strcmp(mode, "fpe") == 0) {
        volatile int zero = 0;
        return 42 / zero;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--crash") return run_crash(argv[2], argv[3]);

    struct rlimit no_core{0, 0};
    ::setrlimit(RLIMIT_CORE, &no_core);       // keep the demo from littering core files

    const char* modes[] = {"segv", "abort", "fpe", "overflow"};
    for (const char* mode : modes) {
        std::string dump = std::string("/tmp/crash-") + mode + ".log";
        std::printf("--- crashing with %s ---\n", mode);
        std::fflush(stdout);
        pid_t pid = ::fork();
        if (pid == 0) {
            ::setrlimit(RLIMIT_CORE, &no_core);
            if (std::string(mode) == "overflow") {
                struct rlimit small_stack{1 << 20, 1 << 20};
                ::setrlimit(RLIMIT_STACK, &small_stack);
            }
            ::execl(argv[0], argv[0], "--crash", mode, dump.c_str(), nullptr);
            ::_exit(127);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        std::printf("child exited: %s by signal %d (%s)\n",
                    WIFSIGNALED(status) ? "killed" : "returned",
                    WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status),
                    WIFSIGNALED(status) ? ::strsignal(WTERMSIG(status)) : "no signal");

        std::FILE* dumped = std::fopen(dump.c_str(), "r");
        if (!dumped) { std::puts("  (no dump written)\n"); continue; }
        char line[512];
        int printed = 0;
        while (std::fgets(line, sizeof(line), dumped) && printed < 9) {
            if (line[0] != '\n') { std::printf("  %s", line); ++printed; }
        }
        std::fclose(dumped);
        std::puts("");
    }
    std::puts("every dump was written from inside the signal handler using write() only —");
    std::puts("no malloc, no printf, and on a stack the crash could not have destroyed.");
    return 0;
}
