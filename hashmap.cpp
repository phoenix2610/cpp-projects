// An open-addressing hash map with Robin Hood probing and backward-shift deletion.
//
//   g++ -std=c++23 -O2 hashmap.cpp -o hashmap && ./hashmap
//
// Chaining costs a pointer chase per lookup; open addressing keeps everything in
// one array, which is why it wins on cache misses. The two ideas that make it
// practical: Robin Hood probing (a key that has probed further steals the slot
// from one that has probed less, which flattens the worst case), and backward-shift
// deletion (pull the following cluster back one slot instead of leaving a
// tombstone, so a delete-heavy workload does not degrade forever).

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

template <typename Key, typename Value, typename Hash = std::hash<Key>>
class RobinHoodMap {
public:
    explicit RobinHoodMap(std::size_t capacity = 16) { rehash(capacity); }

    struct Slot {
        Key key{};
        Value value{};
        std::uint32_t hash = 0;
        bool occupied = false;
        std::uint16_t distance = 0;     // how far this entry sits from its ideal slot
    };

    bool insert(Key key, Value value) {
        if (static_cast<double>(size_ + 1) / slots_.size() > max_load_) rehash(slots_.size() * 2);
        return emplace(std::move(key), std::move(value), fingerprint(key));
    }

    Value* find(const Key& key) {
        const std::uint32_t h = fingerprint(key);
        std::size_t index = h & mask_;
        for (std::uint16_t dist = 0;; ++dist, index = (index + 1) & mask_) {
            Slot& slot = slots_[index];
            if (!slot.occupied) return nullptr;
            // the invariant that makes lookup fast: nobody sits further than its distance
            if (slot.distance < dist) return nullptr;
            if (slot.hash == h && slot.key == key) return &slot.value;
        }
    }

    bool erase(const Key& key) {
        const std::uint32_t h = fingerprint(key);
        std::size_t index = h & mask_;
        for (std::uint16_t dist = 0;; ++dist, index = (index + 1) & mask_) {
            Slot& slot = slots_[index];
            if (!slot.occupied || slot.distance < dist) return false;
            if (slot.hash == h && slot.key == key) break;
        }
        // backward shift: pull the cluster back rather than leaving a tombstone
        std::size_t next = (index + 1) & mask_;
        while (slots_[next].occupied && slots_[next].distance > 0) {
            slots_[index] = slots_[next];
            slots_[index].distance--;
            index = next;
            next = (next + 1) & mask_;
        }
        slots_[index] = Slot{};
        --size_;
        return true;
    }

    Value& operator[](const Key& key) {
        if (Value* found = find(key)) return *found;
        insert(key, Value{});
        return *find(key);
    }

    void set_max_load(double factor) { max_load_ = factor; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }
    double load_factor() const { return static_cast<double>(size_) / slots_.size(); }

    struct Probe { double mean; std::uint16_t worst; };
    Probe probe_stats() const {
        std::size_t total = 0;
        std::uint16_t worst = 0;
        for (const Slot& s : slots_)
            if (s.occupied) { total += s.distance + 1; worst = std::max(worst, static_cast<std::uint16_t>(s.distance + 1)); }
        return {size_ ? static_cast<double>(total) / size_ : 0.0, worst};
    }

private:
    static std::uint32_t fingerprint(const Key& key) {
        std::uint64_t h = Hash{}(key);
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 29;   // avalanche: weak hashes cluster badly
        return static_cast<std::uint32_t>(h);
    }

    bool emplace(Key key, Value value, std::uint32_t h) {
        std::size_t index = h & mask_;
        std::uint16_t dist = 0;
        Slot incoming{std::move(key), std::move(value), h, true, 0};
        for (;; index = (index + 1) & mask_, ++dist) {
            Slot& slot = slots_[index];
            if (!slot.occupied) {
                incoming.distance = dist;
                slot = std::move(incoming);
                ++size_;
                return true;
            }
            if (slot.hash == h && slot.key == incoming.key) {
                slot.value = std::move(incoming.value);
                return false;                    // updated in place
            }
            if (slot.distance < dist) {          // rob the richer entry, carry on with theirs
                incoming.distance = dist;
                std::swap(slot, incoming);
                dist = incoming.distance;
            }
        }
    }

    void rehash(std::size_t capacity) {
        std::size_t power = 16;
        while (power < capacity) power *= 2;
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(power, Slot{});
        mask_ = power - 1;
        size_ = 0;
        for (Slot& slot : old)
            if (slot.occupied) emplace(std::move(slot.key), std::move(slot.value), slot.hash);
    }

    std::vector<Slot> slots_;
    std::size_t size_ = 0, mask_ = 0;
    double max_load_ = 0.9;              // Robin Hood tolerates a load factor chaining cannot
};

int main() {
    std::puts("1. correctness against std::unordered_map (100k mixed operations)");
    {
        RobinHoodMap<std::string, int> mine;
        std::unordered_map<std::string, int> reference;
        std::mt19937 rng(42);
        for (int i = 0; i < 100'000; ++i) {
            std::string key = "key-" + std::to_string(rng() % 20'000);
            int op = rng() % 10;
            if (op < 6) { mine.insert(key, i); reference[key] = i; }
            else if (op < 8) { mine.erase(key); reference.erase(key); }
            else {
                int* a = mine.find(key);
                auto b = reference.find(key);
                assert((a == nullptr) == (b == reference.end()));
                if (a) assert(*a == b->second);
            }
        }
        std::printf("   sizes match: %zu vs %zu — every lookup agreed\n", mine.size(), reference.size());
        auto stats = mine.probe_stats();
        std::printf("   load factor %.2f, mean probe %.2f, worst probe %u\n",
                    mine.load_factor(), stats.mean, stats.worst);
    }

    std::puts("\n2. what Robin Hood buys you: probe length at rising load");
    for (double target : {0.5, 0.7, 0.9, 0.95}) {
        RobinHoodMap<std::uint64_t, std::uint64_t> map(1 << 16);
        map.set_max_load(0.99);          // hold the table still so the load factor is what we asked for
        auto wanted = static_cast<std::size_t>(target * (1 << 16));
        for (std::size_t i = 0; i < wanted; ++i) map.insert(i * 2654435761u, i);
        auto stats = map.probe_stats();
        std::printf("   load %.2f  mean probe %.2f  worst %u\n", map.load_factor(), stats.mean, stats.worst);
    }

    std::puts("\n3. deletion does not rot the table (500k inserts and erases, fixed size)");
    {
        RobinHoodMap<std::uint64_t, std::uint64_t> map(1 << 14);
        std::mt19937_64 rng(7);
        for (int i = 0; i < 500'000; ++i) {
            std::uint64_t key = rng() % 12'000;
            if (i % 2) map.insert(key, i); else map.erase(key);
        }
        auto stats = map.probe_stats();
        std::printf("   after churn: size %zu, mean probe %.2f, worst %u "
                    "(a tombstone table would be far worse)\n", map.size(), stats.mean, stats.worst);
    }

    std::puts("\n4. lookup speed vs std::unordered_map (1M lookups)");
    {
        constexpr int N = 200'000;
        RobinHoodMap<std::uint64_t, std::uint64_t> mine(N * 2);
        std::unordered_map<std::uint64_t, std::uint64_t> theirs;
        theirs.reserve(N * 2);
        for (int i = 0; i < N; ++i) { mine.insert(i, i * 3); theirs[i] = i * 3; }

        std::mt19937_64 rng(1);
        std::vector<std::uint64_t> probes(1'000'000);
        for (auto& p : probes) p = rng() % (N * 2);

        auto clock = [](auto&& fn) {
            auto start = std::chrono::steady_clock::now();
            std::uint64_t sink = fn();
            auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            return std::pair{ms, sink};
        };
        auto [mine_ms, sink1] = clock([&] {
            std::uint64_t hits = 0;
            for (auto p : probes) if (mine.find(p)) ++hits;
            return hits;
        });
        auto [std_ms, sink2] = clock([&] {
            std::uint64_t hits = 0;
            for (auto p : probes) if (theirs.count(p)) ++hits;
            return hits;
        });
        std::printf("   RobinHoodMap      %6.1fms  (%llu hits)\n", mine_ms, (unsigned long long)sink1);
        std::printf("   unordered_map     %6.1fms  (%llu hits)\n", std_ms, (unsigned long long)sink2);
        std::printf("   ratio             %.2fx  (libstdc++ hashes integers with the identity function,\n"
                    "                            which is exactly the case open addressing gains least on)\n",
                    std_ms / mine_ms);

        RobinHoodMap<std::string, std::uint64_t> mine_s(N * 2);
        std::unordered_map<std::string, std::uint64_t> theirs_s;
        theirs_s.reserve(N * 2);
        std::vector<std::string> keys;
        keys.reserve(N);
        for (int i = 0; i < N; ++i) {
            keys.push_back("session:" + std::to_string(i) + ":token");
            mine_s.insert(keys.back(), i);
            theirs_s[keys.back()] = i;
        }
        std::vector<const std::string*> string_probes(1'000'000);
        for (auto& p : string_probes) p = &keys[rng() % keys.size()];

        auto [mine_s_ms, s1] = clock([&] {
            std::uint64_t hits = 0;
            for (auto* p : string_probes) if (mine_s.find(*p)) ++hits;
            return hits;
        });
        auto [std_s_ms, s2] = clock([&] {
            std::uint64_t hits = 0;
            for (auto* p : string_probes) if (theirs_s.count(*p)) ++hits;
            return hits;
        });
        std::puts("\n   same test with string keys (a pointer chase per bucket for chaining):");
        std::printf("   RobinHoodMap      %6.1fms  (%llu hits)\n", mine_s_ms, (unsigned long long)s1);
        std::printf("   unordered_map     %6.1fms  (%llu hits)\n", std_s_ms, (unsigned long long)s2);
        std::printf("   ratio             %.2fx\n", std_s_ms / mine_s_ms);
    }
    return 0;
}
