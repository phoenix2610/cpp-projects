// One path namespace over many backends: real directories, zip archives, memory, overlays.
//
//   g++ -std=c++23 -O2 vfs.cpp -o vfs && ./vfs --demo
//
// Game engines and build tools all end up here: code should open "shaders/blur.frag"
// without caring whether that lives in a folder during development, inside a packed
// archive in production, or in an overlay a mod dropped on top. The VFS is a mount
// table plus a resolution order, and the interesting parts are path normalisation
// (".." must never escape a mount root) and mount precedence.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct FileInfo {
    std::string path;
    std::size_t size = 0;
    std::string source;         // which mount answered
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::optional<std::string> read(const std::string& path) = 0;
    virtual bool exists(const std::string& path) = 0;
    virtual std::vector<std::string> list(const std::string& prefix) = 0;
    virtual bool write(const std::string&, const std::string&) { return false; }   // read-only by default
    virtual const char* name() const = 0;
};

class MemoryBackend : public Backend {
public:
    explicit MemoryBackend(std::string label = "memory") : label_(std::move(label)) {}

    std::optional<std::string> read(const std::string& path) override {
        auto found = files_.find(path);
        return found == files_.end() ? std::nullopt : std::optional{found->second};
    }
    bool exists(const std::string& path) override { return files_.count(path) > 0; }
    bool write(const std::string& path, const std::string& content) override {
        files_[path] = content;
        return true;
    }
    std::vector<std::string> list(const std::string& prefix) override {
        std::vector<std::string> out;
        for (const auto& [path, _] : files_) if (path.starts_with(prefix)) out.push_back(path);
        return out;
    }
    const char* name() const override { return label_.c_str(); }

private:
    std::string label_;
    std::map<std::string, std::string> files_;
};

class DirectoryBackend : public Backend {
public:
    explicit DirectoryBackend(std::string root) : root_(fs::absolute(root).lexically_normal().string()) {}

    std::optional<std::string> read(const std::string& path) override {
        auto resolved = resolve(path);
        if (!resolved) return std::nullopt;
        std::ifstream file(*resolved, std::ios::binary);
        if (!file) return std::nullopt;
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    bool exists(const std::string& path) override {
        auto resolved = resolve(path);
        return resolved && fs::exists(*resolved) && fs::is_regular_file(*resolved);
    }
    bool write(const std::string& path, const std::string& content) override {
        auto resolved = resolve(path);
        if (!resolved) return false;
        fs::create_directories(fs::path(*resolved).parent_path());
        std::ofstream file(*resolved, std::ios::binary);
        file << content;
        return bool(file);
    }
    std::vector<std::string> list(const std::string& prefix) override {
        std::vector<std::string> out;
        if (!fs::exists(root_)) return out;
        for (const auto& entry : fs::recursive_directory_iterator(root_)) {
            if (!entry.is_regular_file()) continue;
            std::string relative = fs::relative(entry.path(), root_).generic_string();
            if (relative.starts_with(prefix)) out.push_back(relative);
        }
        return out;
    }
    const char* name() const override { return "directory"; }

    // The security-relevant half: a path must never resolve outside the mount root.
    std::optional<std::string> resolve(const std::string& path) const {
        fs::path candidate = fs::path(root_) / path;
        std::string normalized = candidate.lexically_normal().string();
        if (!normalized.starts_with(root_)) return std::nullopt;
        return normalized;
    }

private:
    std::string root_;
};

// Stands in for a zip/pak: one blob, an index of (offset, length) — one open, many reads.
class ArchiveBackend : public Backend {
public:
    void pack(const std::string& path, const std::string& content) {
        index_[path] = {blob_.size(), content.size()};
        blob_ += content;
    }
    std::optional<std::string> read(const std::string& path) override {
        auto found = index_.find(path);
        if (found == index_.end()) return std::nullopt;
        return blob_.substr(found->second.first, found->second.second);
    }
    bool exists(const std::string& path) override { return index_.count(path) > 0; }
    std::vector<std::string> list(const std::string& prefix) override {
        std::vector<std::string> out;
        for (const auto& [path, _] : index_) if (path.starts_with(prefix)) out.push_back(path);
        return out;
    }
    const char* name() const override { return "archive"; }
    std::size_t bytes() const { return blob_.size(); }
    std::size_t count() const { return index_.size(); }

private:
    std::string blob_;
    std::map<std::string, std::pair<std::size_t, std::size_t>> index_;
};

class VirtualFileSystem {
public:
    struct Mount {
        std::string point;
        std::shared_ptr<Backend> backend;
        int priority;
        bool writable;
    };

    void mount(const std::string& point, std::shared_ptr<Backend> backend, int priority = 0, bool writable = false) {
        mounts_.push_back({normalize(point), std::move(backend), priority, writable});
        std::stable_sort(mounts_.begin(), mounts_.end(),
                         [](const Mount& a, const Mount& b) { return a.priority > b.priority; });
    }

    static std::string normalize(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= path.size()) {
            std::size_t slash = path.find('/', start);
            std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (part == "..") { if (!parts.empty()) parts.pop_back(); }     // never escapes: pop or ignore
            else if (!part.empty() && part != ".") parts.push_back(part);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        std::string out;
        for (const std::string& part : parts) out += (out.empty() ? "" : "/") + part;
        return out;
    }

    std::optional<std::string> read(const std::string& raw_path) {
        std::string path = normalize(raw_path);
        ++reads_;
        last_source_ = "(none)";
        for (Mount& mount : mounts_) {
            auto relative = strip(mount, path);
            if (!relative) continue;
            if (auto content = mount.backend->read(*relative)) {
                last_source_ = mount.backend->name();
                return content;
            }
            ++fallthroughs_;
        }
        return std::nullopt;
    }

    bool write(const std::string& raw_path, const std::string& content) {
        std::string path = normalize(raw_path);
        for (Mount& mount : mounts_) {
            if (!mount.writable) continue;
            auto relative = strip(mount, path);
            if (!relative) continue;
            if (mount.backend->write(*relative, content)) { last_source_ = mount.backend->name(); return true; }
        }
        return false;
    }

    std::vector<FileInfo> list(const std::string& raw_prefix = "") {
        std::string prefix = normalize(raw_prefix);
        std::map<std::string, FileInfo> seen;         // higher-priority mounts win, first writer keeps the slot
        for (Mount& mount : mounts_) {
            std::string inner = prefix.starts_with(mount.point) && !mount.point.empty()
                                    ? prefix.substr(std::min(prefix.size(), mount.point.size() + 1)) : "";
            for (const std::string& path : mount.backend->list(inner)) {
                std::string full = mount.point.empty() ? path : mount.point + "/" + path;
                if (!full.starts_with(prefix)) continue;
                if (!seen.count(full))
                    seen[full] = {full, mount.backend->read(path) ? mount.backend->read(path)->size() : 0,
                                  mount.backend->name()};
            }
        }
        std::vector<FileInfo> out;
        for (auto& [_, info] : seen) out.push_back(info);
        return out;
    }

    const char* last_source() const { return last_source_; }
    std::size_t mount_count() const { return mounts_.size(); }
    std::uint64_t reads() const { return reads_; }
    std::uint64_t fallthroughs() const { return fallthroughs_; }

private:
    std::optional<std::string> strip(const Mount& mount, const std::string& path) const {
        if (mount.point.empty()) return path;
        if (!path.starts_with(mount.point)) return std::nullopt;
        if (path.size() == mount.point.size()) return std::string();
        if (path[mount.point.size()] != '/') return std::nullopt;
        return path.substr(mount.point.size() + 1);
    }

    std::vector<Mount> mounts_;
    const char* last_source_ = "";
    std::uint64_t reads_ = 0, fallthroughs_ = 0;
};

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    std::string base = "/tmp/vfs-demo";
    fs::remove_all(base);
    fs::create_directories(base + "/assets/shaders");
    std::ofstream(base + "/assets/shaders/blur.frag") << "// the shipped blur shader\nvoid main() {}\n";
    std::ofstream(base + "/assets/config.json") << R"({"quality":"high"})";

    VirtualFileSystem vfs;

    auto archive = std::make_shared<ArchiveBackend>();
    archive->pack("shaders/blur.frag", "// archived blur\n");
    archive->pack("shaders/bloom.frag", "// archived bloom\n");
    archive->pack("textures/wall.png", std::string(2048, '\xff'));
    vfs.mount("assets", archive, /*priority=*/0);

    auto directory = std::make_shared<DirectoryBackend>(base + "/assets");
    vfs.mount("assets", directory, /*priority=*/10);           // a real folder shadows the archive

    auto overlay = std::make_shared<MemoryBackend>("mod-overlay");
    overlay->write("shaders/blur.frag", "// MODDED blur\n");
    vfs.mount("assets", overlay, /*priority=*/100);            // a mod shadows both

    auto scratch = std::make_shared<MemoryBackend>("scratch");
    vfs.mount("tmp", scratch, 0, /*writable=*/true);

    std::printf("%zu mounts over one namespace\n\n", vfs.mount_count());

    std::puts("1. resolution order (highest priority wins)");
    auto show = [&](const std::string& path) {
        auto content = vfs.read(path);
        std::string first_line = content ? content->substr(0, content->find('\n')) : "(not found)";
        bool binary = std::any_of(first_line.begin(), first_line.end(),
                                  [](char c) { return c != '\t' && (c < 32 || c > 126); });
        if (binary) first_line = "<" + std::to_string(content->size()) + " bytes of binary data>";
        std::printf("   %-28s -> %-18s %s\n", path.c_str(), vfs.last_source(), first_line.c_str());
    };
    show("assets/shaders/blur.frag");
    show("assets/shaders/bloom.frag");
    show("assets/config.json");
    show("assets/textures/wall.png");
    show("assets/nothing-here.txt");

    std::puts("\n2. path normalisation, including the traversal attempt");
    for (const char* path : {"assets//shaders/./blur.frag", "assets/shaders/../config.json",
                             "assets/../../../etc/passwd", "assets/shaders/../../assets/config.json"}) {
        std::string normalized = VirtualFileSystem::normalize(path);
        auto content = vfs.read(path);
        std::printf("   %-38s -> %-22s %s\n", path, normalized.c_str(),
                    content ? "readable" : "not found (contained, as it should be)");
    }

    std::puts("\n3. one flat listing across every backend");
    for (const FileInfo& info : vfs.list("assets"))
        std::printf("   %-32s %6zu bytes   from %s\n", info.path.c_str(), info.size, info.source.c_str());

    std::puts("\n4. writes land on the one writable mount");
    std::printf("   write to tmp/session.log:    %s\n", vfs.write("tmp/session.log", "started\n") ? "ok" : "refused");
    std::printf("   write to assets/blur.frag:   %s (archive and overlay are read-only)\n",
                vfs.write("assets/shaders/blur.frag", "nope") ? "ok (BUG)" : "refused");
    std::printf("   read back tmp/session.log:   %s", vfs.read("tmp/session.log").value_or("(missing)").c_str());

    std::puts("\n5. cost of the indirection");
    {
        constexpr int N = 200'000;
        auto start = std::chrono::steady_clock::now();
        std::size_t bytes = 0;
        for (int i = 0; i < N; ++i)
            if (auto content = vfs.read("assets/shaders/bloom.frag")) bytes += content->size();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("   %d resolutions in %.0fms (%.2f us each, %zu bytes read)\n", N, ms, ms * 1000 / N, bytes);
        std::printf("   %llu reads total with %llu mount fall-throughs before a hit\n",
                    (unsigned long long)vfs.reads(), (unsigned long long)vfs.fallthroughs());
    }
    fs::remove_all(base);
    return 0;
}
