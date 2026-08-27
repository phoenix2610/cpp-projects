// A log-structured key-value store: memtable, WAL, sorted segment files, compaction.
//
//   g++ -std=c++23 -O2 kvstore.cpp -o kvstore && ./kvstore --demo
//   ./kvstore ./data put user:1 ana && ./kvstore ./data get user:1
//
// The LSM bargain: writes only ever append (to a log, then to a sorted file), which
// turns random writes into sequential ones. Reads pay for it — newest segment first,
// then older ones, until a key is found — so each segment carries a Bloom filter and
// a sparse index to make a miss cheap. Deletes are tombstones, not removals, because
// an older segment may still hold the key; compaction is what finally drops them.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::uint64_t hash64(std::string_view key, std::uint64_t seed = 0) {
    std::uint64_t h = 1469598103934665603ULL ^ seed;
    for (char c : key) { h ^= std::uint8_t(c); h *= 1099511628211ULL; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 29;
    return h;
}

struct BloomFilter {
    std::vector<std::uint8_t> bits;
    int hashes = 4;

    void reset(std::size_t expected) {
        std::size_t bit_count = std::max<std::size_t>(64, expected * 10);
        bits.assign((bit_count + 7) / 8, 0);
    }
    void add(std::string_view key) {
        std::uint64_t h1 = hash64(key), h2 = hash64(key, 0x9e3779b9) | 1;
        for (int i = 0; i < hashes; ++i) {
            std::size_t position = (h1 + std::uint64_t(i) * h2) % (bits.size() * 8);
            bits[position >> 3] |= std::uint8_t(1 << (position & 7));
        }
    }
    bool maybe_contains(std::string_view key) const {
        if (bits.empty()) return true;
        std::uint64_t h1 = hash64(key), h2 = hash64(key, 0x9e3779b9) | 1;
        for (int i = 0; i < hashes; ++i) {
            std::size_t position = (h1 + std::uint64_t(i) * h2) % (bits.size() * 8);
            if (!(bits[position >> 3] >> (position & 7) & 1)) return false;   // definitely absent
        }
        return true;
    }
};

struct Segment {
    std::string path;
    BloomFilter filter;
    std::vector<std::pair<std::string, std::uint64_t>> sparse_index;   // every Nth key -> file offset
    std::size_t entries = 0;
    int level = 0;
};

class KVStore {
public:
    struct Stats {
        std::uint64_t writes = 0, reads = 0, bloom_rejections = 0,
                      segments_scanned = 0, compactions = 0, tombstones_dropped = 0;
    };

    explicit KVStore(std::string directory, std::size_t memtable_limit = 1000)
        : dir_(std::move(directory)), limit_(memtable_limit) {
        fs::create_directories(dir_);
        replay_wal();
        load_segments();
    }

    void put(const std::string& key, const std::string& value) {
        append_wal(key, value, false);
        memtable_[key] = {value, false};
        ++stats_.writes;
        if (memtable_.size() >= limit_) flush();
    }

    void remove(const std::string& key) {
        append_wal(key, "", true);
        memtable_[key] = {"", true};                    // tombstone
        if (memtable_.size() >= limit_) flush();
    }

    std::optional<std::string> get(const std::string& key) {
        ++stats_.reads;
        auto found = memtable_.find(key);
        if (found != memtable_.end())
            return found->second.tombstone ? std::nullopt : std::optional{found->second.value};

        for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {   // newest first
            if (!it->filter.maybe_contains(key)) { ++stats_.bloom_rejections; continue; }
            ++stats_.segments_scanned;
            if (auto value = search_segment(*it, key)) {
                if (value->second) return std::nullopt;                       // tombstone wins
                return value->first;
            }
        }
        return std::nullopt;
    }

    void flush() {
        if (memtable_.empty()) return;
        std::string path = dir_ + "/seg-" + std::to_string(next_segment_++) + ".dat";
        write_segment(path, memtable_, 0);
        memtable_.clear();
        std::ofstream(wal_path(), std::ios::trunc);     // the log's job is done once data is sorted on disk
    }

    // Merge every segment into one, keeping only the newest version of each key.
    void compact() {
        flush();
        if (segments_.size() < 2) return;
        std::map<std::string, Value> merged;
        for (Segment& segment : segments_)              // oldest to newest, so newer overwrites
            for (auto& [key, value] : read_segment(segment.path)) merged[key] = value;

        std::size_t dropped = 0;
        for (auto it = merged.begin(); it != merged.end();) {
            if (it->second.tombstone) { it = merged.erase(it); ++dropped; }   // tombstones die here
            else ++it;
        }
        for (Segment& segment : segments_) fs::remove(segment.path);
        segments_.clear();
        std::string path = dir_ + "/seg-" + std::to_string(next_segment_++) + ".dat";
        write_segment(path, merged, 1);
        stats_.compactions++;
        stats_.tombstones_dropped += dropped;
    }

    std::size_t segment_count() const { return segments_.size(); }
    std::size_t memtable_size() const { return memtable_.size(); }
    const Stats& stats() const { return stats_; }
    std::uint64_t disk_bytes() const {
        std::uint64_t total = 0;
        for (const Segment& s : segments_) total += fs::file_size(s.path);
        return total;
    }

private:
    struct Value { std::string value; bool tombstone; };

    std::string wal_path() const { return dir_ + "/wal.log"; }

    void append_wal(const std::string& key, const std::string& value, bool tombstone) {
        std::ofstream log(wal_path(), std::ios::app | std::ios::binary);
        std::uint32_t key_size = std::uint32_t(key.size()), value_size = std::uint32_t(value.size());
        std::uint8_t flag = tombstone;
        log.write(reinterpret_cast<const char*>(&key_size), 4);
        log.write(reinterpret_cast<const char*>(&value_size), 4);
        log.write(reinterpret_cast<const char*>(&flag), 1);
        log.write(key.data(), key_size);
        log.write(value.data(), value_size);
    }

    void replay_wal() {
        std::ifstream log(wal_path(), std::ios::binary);
        if (!log) return;
        std::uint32_t key_size = 0, value_size = 0;
        std::uint8_t flag = 0;
        while (log.read(reinterpret_cast<char*>(&key_size), 4) &&
               log.read(reinterpret_cast<char*>(&value_size), 4) &&
               log.read(reinterpret_cast<char*>(&flag), 1)) {
            std::string key(key_size, '\0'), value(value_size, '\0');
            if (!log.read(key.data(), key_size) || !log.read(value.data(), value_size)) break;
            memtable_[key] = {value, bool(flag)};
        }
        recovered_ = memtable_.size();
    }

    template <typename Map>
    void write_segment(const std::string& path, const Map& entries, int level) {
        std::ofstream file(path, std::ios::binary);
        Segment segment{path, {}, {}, entries.size(), level};
        segment.filter.reset(entries.size());
        std::size_t index = 0;
        for (const auto& [key, value] : entries) {
            std::uint64_t offset = std::uint64_t(file.tellp());
            if (index % 64 == 0) segment.sparse_index.emplace_back(key, offset);   // sparse: 1 in 64
            segment.filter.add(key);
            std::uint32_t key_size = std::uint32_t(key.size()), value_size = std::uint32_t(value.value.size());
            std::uint8_t flag = value.tombstone;
            file.write(reinterpret_cast<const char*>(&key_size), 4);
            file.write(reinterpret_cast<const char*>(&value_size), 4);
            file.write(reinterpret_cast<const char*>(&flag), 1);
            file.write(key.data(), key_size);
            file.write(value.value.data(), value_size);
            ++index;
        }
        segments_.push_back(std::move(segment));
    }

    std::map<std::string, Value> read_segment(const std::string& path) const {
        std::map<std::string, Value> entries;
        std::ifstream file(path, std::ios::binary);
        std::uint32_t key_size = 0, value_size = 0;
        std::uint8_t flag = 0;
        while (file.read(reinterpret_cast<char*>(&key_size), 4) &&
               file.read(reinterpret_cast<char*>(&value_size), 4) &&
               file.read(reinterpret_cast<char*>(&flag), 1)) {
            std::string key(key_size, '\0'), value(value_size, '\0');
            file.read(key.data(), key_size);
            file.read(value.data(), value_size);
            entries[key] = {value, bool(flag)};
        }
        return entries;
    }

    // Seek to the nearest sparse index entry, then scan forward — no full-file read.
    std::optional<std::pair<std::string, bool>> search_segment(const Segment& segment, const std::string& key) const {
        std::uint64_t start = 0;
        for (const auto& [indexed_key, offset] : segment.sparse_index) {
            if (indexed_key <= key) start = offset;
            else break;
        }
        std::ifstream file(segment.path, std::ios::binary);
        file.seekg(std::streamoff(start));
        std::uint32_t key_size = 0, value_size = 0;
        std::uint8_t flag = 0;
        while (file.read(reinterpret_cast<char*>(&key_size), 4) &&
               file.read(reinterpret_cast<char*>(&value_size), 4) &&
               file.read(reinterpret_cast<char*>(&flag), 1)) {
            std::string candidate(key_size, '\0'), value(value_size, '\0');
            file.read(candidate.data(), key_size);
            file.read(value.data(), value_size);
            if (candidate == key) return std::pair{value, bool(flag)};
            if (candidate > key) return std::nullopt;      // sorted: we have passed it
        }
        return std::nullopt;
    }

    void load_segments() {
        std::vector<std::string> paths;
        for (const auto& entry : fs::directory_iterator(dir_))
            if (entry.path().filename().string().starts_with("seg-")) paths.push_back(entry.path().string());
        std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
            auto number = [](const std::string& p) {
                auto start = p.find("seg-") + 4;
                return std::stoi(p.substr(start, p.find('.', start) - start));
            };
            return number(a) < number(b);
        });
        for (const std::string& path : paths) {
            auto entries = read_segment(path);
            Segment segment{path, {}, {}, entries.size(), 0};
            segment.filter.reset(entries.size());
            std::size_t index = 0;
            std::uint64_t offset = 0;
            for (const auto& [key, value] : entries) {
                if (index % 64 == 0) segment.sparse_index.emplace_back(key, offset);
                segment.filter.add(key);
                offset += 9 + key.size() + value.value.size();
                ++index;
            }
            segments_.push_back(std::move(segment));
            auto start = path.find("seg-") + 4;
            next_segment_ = std::max(next_segment_, std::stoi(path.substr(start, path.find('.', start) - start)) + 1);
        }
    }

    std::string dir_;
    std::size_t limit_;
    std::map<std::string, Value> memtable_;
    std::vector<Segment> segments_;
    int next_segment_ = 0;
    std::size_t recovered_ = 0;
    Stats stats_;
};

static int demo() {
    std::string dir = "/tmp/kvstore-demo";
    fs::remove_all(dir);

    std::puts("1. writing 5000 keys through a 500-key memtable");
    {
        KVStore store(dir, 500);
        for (int i = 0; i < 5000; ++i) store.put("user:" + std::to_string(i), "value-" + std::to_string(i * 7));
        std::printf("   %zu sealed segments on disk, %zu keys still in the memtable, %.1f KB written\n",
                    store.segment_count(), store.memtable_size(), store.disk_bytes() / 1024.0);

        std::printf("   get user:0    -> %s\n", store.get("user:0").value_or("(missing)").c_str());
        std::printf("   get user:4999 -> %s\n", store.get("user:4999").value_or("(missing)").c_str());
        std::printf("   get user:9999 -> %s\n", store.get("user:9999").value_or("(missing)").c_str());

        store.put("user:0", "updated");
        std::printf("   after overwriting user:0 -> %s (newest segment wins)\n",
                    store.get("user:0").value_or("(missing)").c_str());
        store.remove("user:1");
        std::printf("   after deleting user:1    -> %s (tombstone shadows the old value)\n",
                    store.get("user:1").value_or("(missing)").c_str());
    }

    std::puts("\n2. reopening: the WAL replays whatever never reached a segment");
    {
        KVStore store(dir, 500);
        std::printf("   %zu segments loaded, %zu keys recovered from the log\n",
                    store.segment_count(), store.memtable_size());
        std::printf("   user:4999 still readable: %s\n", store.get("user:4999").value_or("(missing)").c_str());
        std::printf("   user:1 still deleted:     %s\n", store.get("user:1").has_value() ? "NO" : "yes");
    }

    std::puts("\n3. what the Bloom filters save on a miss");
    {
        KVStore store(dir, 500);
        for (int i = 0; i < 2000; ++i) store.get("absent:" + std::to_string(i));
        const auto& s = store.stats();
        std::printf("   2000 lookups for keys that do not exist\n");
        std::printf("   segments skipped by their Bloom filter: %llu\n", (unsigned long long)s.bloom_rejections);
        std::printf("   segments actually opened and scanned:   %llu\n", (unsigned long long)s.segments_scanned);
        std::printf("   without filters this would be %zu file scans; it was %llu\n",
                    2000 * store.segment_count(), (unsigned long long)s.segments_scanned);
    }

    std::puts("\n4. compaction");
    {
        KVStore store(dir, 500);
        std::uint64_t before = store.disk_bytes();
        std::size_t segments_before = store.segment_count();
        store.compact();
        std::printf("   %zu segments (%.1f KB) -> %zu segment (%.1f KB), %llu tombstones dropped\n",
                    segments_before, before / 1024.0, store.segment_count(), store.disk_bytes() / 1024.0,
                    (unsigned long long)store.stats().tombstones_dropped);
        std::printf("   spot check after compaction: user:0=%s user:2500=%s user:1=%s\n",
                    store.get("user:0").value_or("(missing)").c_str(),
                    store.get("user:2500").value_or("(missing)").c_str(),
                    store.get("user:1").has_value() ? "PRESENT (bug)" : "(deleted)");
    }

    std::puts("\n5. correctness sweep: all 5000 keys after everything above");
    {
        KVStore store(dir, 500);
        int correct = 0, wrong = 0;
        for (int i = 2; i < 5000; ++i) {
            auto value = store.get("user:" + std::to_string(i));
            if (value && *value == "value-" + std::to_string(i * 7)) ++correct;
            else ++wrong;
        }
        std::printf("   %d correct, %d wrong (user:0 was overwritten and user:1 deleted, so both are excluded)\n",
                    correct, wrong);
    }
    fs::remove_all(dir);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return demo();
    KVStore store(argv[1]);
    std::string command = argv[2];
    if (command == "put" && argc >= 5) { store.put(argv[3], argv[4]); store.flush(); std::printf("  ok\n"); }
    else if (command == "get" && argc >= 4) {
        auto value = store.get(argv[3]);
        std::printf("  %s\n", value.value_or("(not found)").c_str());
        return value ? 0 : 1;
    } else if (command == "del" && argc >= 4) { store.remove(argv[3]); store.flush(); std::printf("  ok\n"); }
    else if (command == "compact") { store.compact(); std::printf("  %zu segments remain\n", store.segment_count()); }
    return 0;
}
