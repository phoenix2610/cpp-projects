// Two allocators the standard one cannot be: a bump arena and a fixed-size pool.
//
//   g++ -std=c++23 -O2 allocators.cpp -o allocators && ./allocators
//
// malloc is general, and generality costs: a size class lookup, a free list walk,
// locking, and metadata next to every block. When you know the lifetime (a frame,
// a request, a parse) an arena reduces allocation to a pointer add and free to
// nothing. When you know the size (a node, a particle, a connection) a pool
// reduces it to a linked-list pop that reuses the block's own memory for the link.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

// ------------------------------------------------------------------ arena

class Arena {
public:
    explicit Arena(std::size_t block_size = 64 * 1024) : block_size_(block_size) {}
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena() { for (Block& b : blocks_) std::free(b.memory); }

    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
        if (!blocks_.empty()) {
            Block& block = blocks_.back();
            std::size_t aligned = align_up(block.used, alignment);
            if (aligned + bytes <= block.capacity) {       // the fast path: three instructions
                void* result = block.memory + aligned;
                block.used = aligned + bytes;
                ++allocations_;
                return result;
            }
        }
        grow(std::max(bytes + alignment, block_size_));
        return allocate(bytes, alignment);
    }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    // A marker lets you roll back to a point — scoped allocation without per-object frees.
    struct Marker { std::size_t block, used; };
    Marker mark() const { return {blocks_.size() - 1, blocks_.empty() ? 0 : blocks_.back().used}; }
    void release(Marker marker) {
        while (blocks_.size() > marker.block + 1) {
            std::free(blocks_.back().memory);
            blocks_.pop_back();
        }
        if (!blocks_.empty()) blocks_.back().used = marker.used;
    }
    void reset() {                                  // free everything at once, no destructors run
        for (std::size_t i = 1; i < blocks_.size(); ++i) std::free(blocks_[i].memory);
        blocks_.resize(std::min<std::size_t>(blocks_.size(), 1));
        if (!blocks_.empty()) blocks_[0].used = 0;
        allocations_ = 0;
    }

    std::size_t bytes_reserved() const {
        std::size_t total = 0;
        for (const Block& b : blocks_) total += b.capacity;
        return total;
    }
    std::size_t bytes_used() const {
        std::size_t total = 0;
        for (const Block& b : blocks_) total += b.used;
        return total;
    }
    std::size_t blocks() const { return blocks_.size(); }
    std::size_t allocations() const { return allocations_; }

private:
    struct Block { std::byte* memory; std::size_t used, capacity; };

    static std::size_t align_up(std::size_t value, std::size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }
    void grow(std::size_t capacity) {
        auto* memory = static_cast<std::byte*>(std::malloc(capacity));
        if (!memory) throw std::bad_alloc();
        blocks_.push_back({memory, 0, capacity});
    }

    std::vector<Block> blocks_;
    std::size_t block_size_, allocations_ = 0;
};

// STL-compatible adaptor, so arena memory works with std::vector and friends
template <typename T>
struct ArenaAllocator {
    using value_type = T;
    Arena* arena;
    explicit ArenaAllocator(Arena& a) : arena(&a) {}
    template <typename U> ArenaAllocator(const ArenaAllocator<U>& other) : arena(other.arena) {}
    T* allocate(std::size_t n) { return static_cast<T*>(arena->allocate(n * sizeof(T), alignof(T))); }
    void deallocate(T*, std::size_t) noexcept {}      // arenas free in bulk; this is a no-op on purpose
    template <typename U> bool operator==(const ArenaAllocator<U>& o) const { return arena == o.arena; }
};

// ------------------------------------------------------------------- pool

template <typename T, std::size_t ChunkObjects = 1024>
class PoolAllocator {
public:
    PoolAllocator() = default;
    PoolAllocator(const PoolAllocator&) = delete;
    ~PoolAllocator() { for (std::byte* chunk : chunks_) std::free(chunk); }

    T* allocate() {
        if (!free_list_) grow();
        Node* node = free_list_;
        free_list_ = node->next;         // the free block stores its own link: zero metadata overhead
        ++live_;
        return reinterpret_cast<T*>(node);
    }
    void deallocate(T* object) noexcept {
        auto* node = reinterpret_cast<Node*>(object);
        node->next = free_list_;
        free_list_ = node;
        --live_;
    }
    template <typename... Args>
    T* construct(Args&&... args) { return new (allocate()) T(std::forward<Args>(args)...); }
    void destroy(T* object) noexcept { object->~T(); deallocate(object); }

    std::size_t live() const { return live_; }
    std::size_t capacity() const { return chunks_.size() * ChunkObjects; }

private:
    union Node { Node* next; alignas(T) std::byte storage[sizeof(T)]; };

    void grow() {
        auto* chunk = static_cast<std::byte*>(std::malloc(sizeof(Node) * ChunkObjects));
        if (!chunk) throw std::bad_alloc();
        chunks_.push_back(chunk);
        auto* nodes = reinterpret_cast<Node*>(chunk);
        for (std::size_t i = 0; i + 1 < ChunkObjects; ++i) nodes[i].next = &nodes[i + 1];
        nodes[ChunkObjects - 1].next = nullptr;
        free_list_ = nodes;
    }

    std::vector<std::byte*> chunks_;
    Node* free_list_ = nullptr;
    std::size_t live_ = 0;
};

// -------------------------------------------------------------------- demo

struct Particle { float x, y, vx, vy; int life; };

template <typename F>
double time_ms(F&& fn) {
    auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int main() {
    constexpr int N = 500'000;

    std::puts("1. arena vs new/delete for a batch with one shared lifetime");
    {
        double heap_ms = time_ms([&] {
            std::vector<Particle*> particles;
            particles.reserve(N);
            for (int i = 0; i < N; ++i) particles.push_back(new Particle{float(i), 0, 1, 1, i});
            for (Particle* p : particles) delete p;
        });
        Arena arena(1 << 20);
        std::size_t reserved = 0, blocks = 0, allocations = 0;
        double arena_ms = time_ms([&] {
            for (int i = 0; i < N; ++i) arena.create<Particle>(Particle{float(i), 0, 1, 1, i});
            reserved = arena.bytes_reserved(); blocks = arena.blocks(); allocations = arena.allocations();
            arena.reset();                       // "freeing" 500k objects: one pointer assignment
        });
        std::printf("   new/delete   %7.2fms\n   arena        %7.2fms  -> %.1fx faster\n",
                    heap_ms, arena_ms, heap_ms / arena_ms);
        std::printf("   arena served %zu allocations from %zuKB across %zu blocks\n",
                    allocations, reserved / 1024, blocks);
    }

    std::puts("\n2. pool vs new/delete for churn (allocate, free, reallocate)");
    {
        // a fixed-size ring of live objects: replace the oldest each step, so the
        // measurement is allocator cost, not std::vector bookkeeping
        constexpr int kLive = 1000;
        double heap_ms = time_ms([&] {
            std::vector<Particle*> live(kLive, nullptr);
            for (int i = 0; i < N; ++i) {
                int slot = i % kLive;
                delete live[slot];
                live[slot] = new Particle{};
            }
            for (Particle* p : live) delete p;
        });
        PoolAllocator<Particle> pool;
        double pool_ms = time_ms([&] {
            std::vector<Particle*> live(kLive, nullptr);
            for (int i = 0; i < N; ++i) {
                int slot = i % kLive;
                if (live[slot]) pool.destroy(live[slot]);
                live[slot] = pool.construct();
            }
            for (Particle* p : live) if (p) pool.destroy(p);
        });
        std::printf("   new/delete   %7.2fms\n   pool         %7.2fms  -> %.1fx faster\n",
                    heap_ms, pool_ms, heap_ms / pool_ms);
        std::printf("   pool holds %zu slots, %zu live at the end\n", pool.capacity(), pool.live());
    }

    std::puts("\n3. markers: scoped allocation inside a long-lived arena");
    {
        Arena arena;
        for (int i = 0; i < 3; ++i) arena.create<Particle>();
        std::size_t before = arena.bytes_used();
        auto marker = arena.mark();
        for (int i = 0; i < 10'000; ++i) arena.create<Particle>();
        std::size_t peak = arena.bytes_used();
        arena.release(marker);
        std::printf("   before %zuB, peak %zuB, after release %zuB (back to the mark exactly)\n",
                    before, peak, arena.bytes_used());
    }

    std::puts("\n4. std::vector allocating out of the arena");
    {
        Arena arena;
        std::vector<int, ArenaAllocator<int>> numbers{ArenaAllocator<int>(arena)};
        for (int i = 0; i < 10'000; ++i) numbers.push_back(i);
        long long sum = 0;
        for (int n : numbers) sum += n;
        std::printf("   vector of %zu ints, sum %lld, arena reserved %zuKB "
                    "(growth reallocations are never returned — the arena's trade)\n",
                    numbers.size(), sum, arena.bytes_reserved() / 1024);
    }

    std::puts("\n5. memory locality: arena objects land next to each other");
    {
        Arena arena;
        auto* a = arena.create<Particle>();
        auto* b = arena.create<Particle>();
        auto* c = arena.create<Particle>();
        auto* h1 = new Particle{}; auto* h2 = new Particle{}; auto* h3 = new Particle{};
        std::printf("   arena gaps: %td, %td bytes (sizeof(Particle)=%zu)\n",
                    reinterpret_cast<std::byte*>(b) - reinterpret_cast<std::byte*>(a),
                    reinterpret_cast<std::byte*>(c) - reinterpret_cast<std::byte*>(b), sizeof(Particle));
        std::printf("   heap gaps:  %td, %td bytes\n",
                    reinterpret_cast<std::byte*>(h2) - reinterpret_cast<std::byte*>(h1),
                    reinterpret_cast<std::byte*>(h3) - reinterpret_cast<std::byte*>(h2));
        delete h1; delete h2; delete h3;
    }
    return 0;
}
