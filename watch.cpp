// Watch a directory tree with inotify: recursive, coalescing, with a debounced callback.
//
//   g++ -std=c++23 -O2 watch.cpp -o watch && ./watch ./src --debounce 200
//   ./watch --demo          # creates a temp tree and drives it itself
//
// inotify gives you a raw event per syscall the kernel saw, which is not what a
// build tool wants: one editor save can produce CREATE, MODIFY, MOVED_FROM,
// MOVED_TO and DELETE across two files. This coalesces per path inside a debounce
// window, and adds watches for directories created while running — the gap that
// makes naive watchers miss everything under a newly cloned folder.

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <algorithm>

class DirectoryWatcher {
public:
    enum class Kind { Created, Modified, Deleted, Renamed };

    struct Event {
        std::string path;
        Kind kind;
        int raw_events = 0;          // how many inotify events collapsed into this one
    };

    explicit DirectoryWatcher(int debounce_ms = 150) : debounce_ms_(debounce_ms) {
        fd_ = ::inotify_init1(IN_NONBLOCK);
        if (fd_ < 0) throw std::runtime_error("inotify_init1 failed");
    }
    ~DirectoryWatcher() { if (fd_ >= 0) ::close(fd_); }

    void watch_tree(const std::string& root) {
        add_watch(root);
        DIR* dir = ::opendir(root.c_str());
        if (!dir) return;
        while (dirent* entry = ::readdir(dir)) {
            std::string name = entry->d_name;
            if (name == "." || name == ".." || name[0] == '.') continue;
            std::string child = root + "/" + name;
            struct stat info{};
            if (::stat(child.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) watch_tree(child);
        }
        ::closedir(dir);
    }

    std::size_t watch_count() const { return by_descriptor_.size(); }

    // Pump events for `budget_ms`, calling `on_batch` with each debounced group.
    void pump(int budget_ms, const std::function<void(const std::vector<Event>&)>& on_batch) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
        alignas(inotify_event) char buffer[8192];
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd pfd{fd_, POLLIN, 0};
            int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            int ready = ::poll(&pfd, 1, std::min(remaining, debounce_ms_ / 2 + 1));
            if (ready > 0) {
                ssize_t got = ::read(fd_, buffer, sizeof(buffer));
                for (char* cursor = buffer; cursor < buffer + got;) {
                    auto* event = reinterpret_cast<inotify_event*>(cursor);
                    ingest(*event);
                    cursor += sizeof(inotify_event) + event->len;
                }
            }
            flush_ready(on_batch);
        }
        flush_all(on_batch);
    }

private:
    struct Pending { Kind kind; int count; std::chrono::steady_clock::time_point last; };

    void add_watch(const std::string& path) {
        int wd = ::inotify_add_watch(fd_, path.c_str(),
                                     IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO |
                                     IN_CLOSE_WRITE | IN_DELETE_SELF);
        if (wd >= 0) by_descriptor_[wd] = path;
    }

    void ingest(const inotify_event& event) {
        auto it = by_descriptor_.find(event.wd);
        if (it == by_descriptor_.end()) return;
        std::string path = it->second + (event.len ? "/" + std::string(event.name) : "");

        if ((event.mask & IN_CREATE) && (event.mask & IN_ISDIR))
            watch_tree(path);                    // a directory created now must be watched now

        Kind kind = Kind::Modified;
        if (event.mask & (IN_CREATE | IN_MOVED_TO)) kind = Kind::Created;
        else if (event.mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF)) kind = Kind::Deleted;
        if (event.mask & IN_MOVED_FROM) kind = Kind::Renamed;

        auto now = std::chrono::steady_clock::now();
        auto& slot = pending_[path];
        // a delete beats a modify: reporting "changed" for a file that is gone is a lie
        if (slot.count == 0 || kind == Kind::Deleted) slot.kind = kind;
        slot.count++;
        slot.last = now;
    }

    void flush_ready(const std::function<void(const std::vector<Event>&)>& on_batch) {
        auto now = std::chrono::steady_clock::now();
        std::vector<Event> batch;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now - it->second.last >= std::chrono::milliseconds(debounce_ms_)) {
                batch.push_back({it->first, it->second.kind, it->second.count});
                it = pending_.erase(it);
            } else ++it;
        }
        if (!batch.empty()) on_batch(batch);
    }

    void flush_all(const std::function<void(const std::vector<Event>&)>& on_batch) {
        std::vector<Event> batch;
        for (auto& [path, slot] : pending_) batch.push_back({path, slot.kind, slot.count});
        pending_.clear();
        if (!batch.empty()) on_batch(batch);
    }

    int fd_ = -1, debounce_ms_;
    std::unordered_map<int, std::string> by_descriptor_;
    std::map<std::string, Pending> pending_;
};

static const char* name_of(DirectoryWatcher::Kind kind) {
    switch (kind) {
        case DirectoryWatcher::Kind::Created: return "created";
        case DirectoryWatcher::Kind::Modified: return "modified";
        case DirectoryWatcher::Kind::Deleted: return "deleted";
        case DirectoryWatcher::Kind::Renamed: return "renamed";
    }
    return "?";
}

int main(int argc, char** argv) {
    std::string root;
    int debounce = 150, seconds = 0;
    bool demo = (argc < 2) || std::string(argv[1]) == "--demo";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--debounce" && i + 1 < argc) debounce = std::atoi(argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (arg != "--demo") root = arg;
    }

    if (demo) {
        root = "/tmp/watch-demo";
        ::system("rm -rf /tmp/watch-demo && mkdir -p /tmp/watch-demo/src /tmp/watch-demo/docs");
        debounce = 120;
    }

    DirectoryWatcher watcher(debounce);
    watcher.watch_tree(root);
    std::printf("watching %s (%zu directories, debounce %dms)\n\n", root.c_str(), watcher.watch_count(), debounce);

    if (!demo) {
        int budget = seconds ? seconds * 1000 : 3600'000;
        watcher.pump(budget, [](const std::vector<DirectoryWatcher::Event>& batch) {
            for (const auto& e : batch)
                std::printf("  %-9s %s  (%d raw events)\n", name_of(e.kind), e.path.c_str(), e.raw_events);
        });
        return 0;
    }

    // drive the tree from a child process so the demo is self-contained
    if (::fork() == 0) {
        ::usleep(150'000);
        ::system("for i in 1 2 3 4 5; do echo line >> /tmp/watch-demo/src/app.ts; done");   // one file, many writes
        ::usleep(250'000);
        ::system("mkdir -p /tmp/watch-demo/src/nested && echo hi > /tmp/watch-demo/src/nested/new.ts");
        ::usleep(250'000);
        ::system("mv /tmp/watch-demo/docs /tmp/watch-demo/manual && rm -f /tmp/watch-demo/src/app.ts");
        ::_exit(0);
    }

    int batches = 0, events = 0, raw = 0;
    watcher.pump(1400, [&](const std::vector<DirectoryWatcher::Event>& batch) {
        ++batches;
        std::printf("  batch %d:\n", batches);
        for (const auto& e : batch) {
            std::printf("    %-9s %-40s %d raw events collapsed\n", name_of(e.kind), e.path.c_str(), e.raw_events);
            ++events;
            raw += e.raw_events;
        }
    });
    std::printf("\n  %d raw inotify events -> %d reported changes in %d batches\n", raw, events, batches);
    std::printf("  watches now: %zu (the directory created mid-run was picked up)\n", watcher.watch_count());
    ::system("rm -rf /tmp/watch-demo");
    return 0;
}
