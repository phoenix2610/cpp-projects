// mmap a file and iterate it by line, measured against buffered reads.
//
//   g++ -std=c++23 -O2 mmapread.cpp -o mmapread && ./mmapread --demo
//   ./mmapread bigfile.log --grep ERROR
//
// mmap turns file access into page faults: no read() syscall per block, no copy
// into your buffer, and the page cache pages you touched are shared, not duplicated.
// The costs are equally real — you cannot mmap something bigger than your address
// space usefully without windowing, a truncated file turns into SIGBUS instead of a
// short read, and random access across a cold file trades syscalls for page faults.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

class MappedFile {
public:
    explicit MappedFile(const std::string& path, bool sequential = true) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("open: " + path);
        struct stat info{};
        if (::fstat(fd_, &info) < 0) { ::close(fd_); throw std::runtime_error("fstat failed"); }
        size_ = static_cast<std::size_t>(info.st_size);
        if (size_ == 0) return;
        data_ = static_cast<const char*>(::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data_ == MAP_FAILED) { ::close(fd_); throw std::runtime_error("mmap failed"); }
        // tell the kernel the access pattern so it can read ahead (or not)
        ::madvise(const_cast<char*>(data_), size_, sequential ? MADV_SEQUENTIAL : MADV_RANDOM);
    }
    ~MappedFile() {
        if (data_ && data_ != MAP_FAILED) ::munmap(const_cast<char*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }
    MappedFile(const MappedFile&) = delete;

    std::string_view view() const { return {data_, size_}; }
    std::size_t size() const { return size_; }

    // Lines without allocating: each one is a view into the mapping itself.
    template <typename Fn>
    std::size_t for_each_line(Fn&& fn) const {
        std::size_t count = 0;
        const char* cursor = data_;
        const char* end = data_ + size_;
        while (cursor < end) {
            const char* newline = static_cast<const char*>(std::memchr(cursor, '\n', std::size_t(end - cursor)));
            const char* stop = newline ? newline : end;
            fn(std::string_view(cursor, std::size_t(stop - cursor)), count++);
            cursor = stop + 1;
        }
        return count;
    }

private:
    int fd_ = -1;
    const char* data_ = nullptr;
    std::size_t size_ = 0;
};

template <typename F>
static double time_ms(F&& fn) {
    auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static std::size_t count_lines_buffered(const std::string& path, std::size_t buffer_size) {
    std::vector<char> buffer(buffer_size);
    int fd = ::open(path.c_str(), O_RDONLY);
    std::size_t lines = 0;
    ssize_t got = 0;
    while ((got = ::read(fd, buffer.data(), buffer.size())) > 0)
        for (ssize_t i = 0; i < got; ++i) lines += (buffer[i] == '\n');
    ::close(fd);
    return lines;
}

static std::size_t count_lines_getline(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    std::size_t lines = 0;
    while (std::getline(file, line)) ++lines;
    return lines;
}

int main(int argc, char** argv) {
    std::string path;
    std::string grep;
    bool demo = (argc < 2) || std::string(argv[1]) == "--demo";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--grep" && i + 1 < argc) grep = argv[++i];
        else if (arg != "--demo") path = arg;
    }

    if (demo) {
        path = "/tmp/mmapread-demo.log";
        std::ofstream out(path);
        for (int i = 0; i < 400'000; ++i)
            out << "2026-08-25T09:" << (i % 60) << ":00 " << (i % 97 == 0 ? "ERROR" : "INFO ")
                << " request id=" << i << " latency=" << (i % 250) << "ms\n";
        out.close();
    }

    struct stat info{};
    ::stat(path.c_str(), &info);
    std::printf("file: %s (%.1f MB)\n\n", path.c_str(), double(info.st_size) / 1048576.0);

    std::size_t mapped_lines = 0, buffered_lines = 0, getline_lines = 0;
    double mapped_ms = time_ms([&] {
        MappedFile file(path);
        mapped_lines = file.for_each_line([](std::string_view, std::size_t) {});
    });
    double small_ms = time_ms([&] { buffered_lines = count_lines_buffered(path, 4096); });
    double big_ms = time_ms([&] { count_lines_buffered(path, 1 << 20); });
    double getline_ms = time_ms([&] { getline_lines = count_lines_getline(path); });

    std::printf("counting %zu lines:\n", mapped_lines);
    std::printf("  mmap + memchr        %7.1fms   %6.0f MB/s\n", mapped_ms, double(info.st_size) / mapped_ms / 1048.576);
    std::printf("  read() 4KB buffer    %7.1fms   %6.0f MB/s\n", small_ms, double(info.st_size) / small_ms / 1048.576);
    std::printf("  read() 1MB buffer    %7.1fms   %6.0f MB/s\n", big_ms, double(info.st_size) / big_ms / 1048.576);
    std::printf("  std::getline         %7.1fms   %6.0f MB/s  (allocates a string per line)\n",
                getline_ms, double(info.st_size) / getline_ms / 1048.576);
    std::printf("  all three agree on the count: %s\n",
                (mapped_lines == buffered_lines && buffered_lines == getline_lines) ? "yes" : "NO");

    if (!grep.empty() || demo) {
        std::string needle = grep.empty() ? "ERROR" : grep;
        MappedFile file(path);
        std::size_t hits = 0;
        std::vector<std::string> samples;
        double search_ms = time_ms([&] {
            file.for_each_line([&](std::string_view line, std::size_t number) {
                if (line.find(needle) != std::string_view::npos) {
                    ++hits;
                    if (samples.size() < 3) samples.emplace_back(std::string(line));
                }
            });
        });
        std::printf("\ngrep %s: %zu hits in %.1fms (zero allocations for the scan itself)\n",
                    needle.c_str(), hits, search_ms);
        for (const auto& s : samples) std::printf("  %s\n", s.c_str());
    }

    std::puts("\nrandom access: 200k reads at random offsets");
    {
        MappedFile file(path, /*sequential=*/false);
        std::string_view whole = file.view();
        std::uint64_t state = 12345, sink = 0;
        double random_ms = time_ms([&] {
            for (int i = 0; i < 200'000; ++i) {
                state = state * 6364136223846793005ULL + 1;
                sink += static_cast<unsigned char>(whole[state % whole.size()]);
            }
        });
        int fd = ::open(path.c_str(), O_RDONLY);
        char byte = 0;
        state = 12345;
        double pread_ms = time_ms([&] {
            for (int i = 0; i < 200'000; ++i) {
                state = state * 6364136223846793005ULL + 1;
                ssize_t n = ::pread(fd, &byte, 1, static_cast<off_t>(state % whole.size()));
                (void)n;
            }
        });
        ::close(fd);
        std::printf("  mmap indexing        %7.1fms  (checksum %llu)\n", random_ms, (unsigned long long)sink);
        std::printf("  pread() per byte     %7.1fms  -> %.0fx more expensive\n", pread_ms, pread_ms / random_ms);
    }
    if (demo) ::unlink(path.c_str());
    return 0;
}
