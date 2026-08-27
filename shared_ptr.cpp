// A shared pointer built from scratch: atomic refcounts, weak refs, aliasing, custom deleters.
//
//   g++ -std=c++23 -O2 -pthread shared_ptr.cpp -o shared_ptr && ./shared_ptr
//
// The control block is the whole design. Two counts, not one: `strong` owns the
// object, `weak` owns the control block. The object is destroyed when strong hits
// zero; the block is freed only when weak (which strong collectively holds one of)
// also hits zero — that is what lets a weak_ptr safely ask "is it still alive?"
// without ever touching freed memory.

#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace mine {

struct ControlBlock {
    std::atomic<long> strong{1};
    std::atomic<long> weak{1};              // strong owners collectively hold one weak count
    std::function<void()> destroy;          // type-erased deleter, captured at construction

    explicit ControlBlock(std::function<void()> d) : destroy(std::move(d)) {}

    void release_strong() {
        if (strong.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroy();                      // last owner: run the deleter now
            release_weak();                 // and drop the strong group's weak count
        }
    }
    void release_weak() {
        if (weak.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
    bool try_lock() {                       // weak -> strong upgrade, race-free
        long current = strong.load(std::memory_order_relaxed);
        while (current != 0) {
            if (strong.compare_exchange_weak(current, current + 1,
                                             std::memory_order_acq_rel, std::memory_order_relaxed))
                return true;
        }
        return false;
    }
};

template <typename T> class WeakPtr;

template <typename T>
class SharedPtr {
public:
    SharedPtr() = default;

    template <typename Deleter = std::default_delete<T>>
    explicit SharedPtr(T* raw, Deleter deleter = {})
        : ptr_(raw), block_(raw ? new ControlBlock([raw, deleter]() mutable { deleter(raw); }) : nullptr) {}

    SharedPtr(const SharedPtr& other) : ptr_(other.ptr_), block_(other.block_) {
        if (block_) block_->strong.fetch_add(1, std::memory_order_relaxed);
    }
    SharedPtr(SharedPtr&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr)), block_(std::exchange(other.block_, nullptr)) {}

    // aliasing constructor: share ownership of `owner`, but point at a subobject
    template <typename U>
    SharedPtr(const SharedPtr<U>& owner, T* member) : ptr_(member), block_(owner.control()) {
        if (block_) block_->strong.fetch_add(1, std::memory_order_relaxed);
    }

    SharedPtr& operator=(SharedPtr other) noexcept { swap(other); return *this; }
    ~SharedPtr() { if (block_) block_->release_strong(); }

    void swap(SharedPtr& other) noexcept { std::swap(ptr_, other.ptr_); std::swap(block_, other.block_); }
    void reset() { SharedPtr().swap(*this); }

    T* get() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    long use_count() const noexcept { return block_ ? block_->strong.load(std::memory_order_relaxed) : 0; }
    ControlBlock* control() const noexcept { return block_; }

private:
    template <typename U> friend class SharedPtr;
    template <typename U> friend class WeakPtr;
    friend class WeakPtr<T>;
    SharedPtr(T* ptr, ControlBlock* block) : ptr_(ptr), block_(block) {}

    T* ptr_ = nullptr;
    ControlBlock* block_ = nullptr;
};

template <typename T>
class WeakPtr {
public:
    WeakPtr() = default;
    WeakPtr(const SharedPtr<T>& shared) : ptr_(shared.get()), block_(shared.control()) {
        if (block_) block_->weak.fetch_add(1, std::memory_order_relaxed);
    }
    WeakPtr(const WeakPtr& other) : ptr_(other.ptr_), block_(other.block_) {
        if (block_) block_->weak.fetch_add(1, std::memory_order_relaxed);
    }
    WeakPtr& operator=(WeakPtr other) noexcept {
        std::swap(ptr_, other.ptr_); std::swap(block_, other.block_); return *this;
    }
    ~WeakPtr() { if (block_) block_->release_weak(); }

    bool expired() const noexcept {
        return !block_ || block_->strong.load(std::memory_order_acquire) == 0;
    }
    SharedPtr<T> lock() const noexcept {
        if (block_ && block_->try_lock()) return SharedPtr<T>(ptr_, block_);
        return {};
    }

private:
    T* ptr_ = nullptr;
    ControlBlock* block_ = nullptr;
};

template <typename T, typename... Args>
SharedPtr<T> make_shared(Args&&... args) {
    return SharedPtr<T>(new T(std::forward<Args>(args)...));
}

}  // namespace mine

// ---------------------------------------------------------------- demo

static int live_objects = 0;

struct Node {
    int value;
    mine::SharedPtr<Node> next;             // strong: makes a cycle if you are not careful
    mine::WeakPtr<Node> prev;               // weak: breaks it
    explicit Node(int v) : value(v) { ++live_objects; }
    ~Node() { --live_objects; }
};

struct Widget {
    int id;
    double weight;
};

int main() {
    std::puts("1. basic ownership");
    {
        auto a = mine::make_shared<Node>(1);
        std::printf("   after make_shared:      use_count=%ld live=%d\n", a.use_count(), live_objects);
        {
            auto b = a;
            auto c = b;
            std::printf("   three owners:           use_count=%ld live=%d\n", a.use_count(), live_objects);
        }
        std::printf("   copies went out of scope: use_count=%ld live=%d\n", a.use_count(), live_objects);
    }
    std::printf("   owner destroyed:        live=%d (expected 0)\n", live_objects);

    std::puts("\n2. weak_ptr sees the object die, and never dangles");
    mine::WeakPtr<Node> observer;
    {
        auto owner = mine::make_shared<Node>(7);
        observer = owner;
        auto locked = observer.lock();
        std::printf("   while alive:  expired=%d locked=%d value=%d use_count=%ld\n",
                    observer.expired(), static_cast<bool>(locked), locked->value, owner.use_count());
    }
    std::printf("   after death:  expired=%d locked=%d (lock() returned empty, no crash)\n",
                observer.expired(), static_cast<bool>(observer.lock()));

    std::puts("\n3. the cycle that leaks, and the weak_ptr that fixes it");
    {
        auto first = mine::make_shared<Node>(1);
        auto second = mine::make_shared<Node>(2);
        first->next = second;
        second->prev = first;               // weak — no cycle
        std::printf("   two nodes linked:       live=%d first.use_count=%ld\n", live_objects, first.use_count());
    }
    std::printf("   both released:          live=%d (a strong `prev` would leave 2 here)\n", live_objects);

    std::puts("\n4. custom deleter and the aliasing constructor");
    {
        bool closed = false;
        {
            auto handle = mine::SharedPtr<int>(new int(42), [&closed](int* p) {
                closed = true;
                delete p;
            });
            std::printf("   custom deleter pending: value=%d\n", *handle);
        }
        std::printf("   deleter ran:            closed=%d\n", closed);

        auto widget = mine::make_shared<Widget>(Widget{99, 1.5});
        mine::SharedPtr<double> weight(widget, &widget->weight);   // points at a member, owns the whole
        std::printf("   aliased member:         weight=%.1f, owner use_count=%ld\n", *weight, widget.use_count());
        widget.reset();
        std::printf("   original reset:         weight still readable = %.1f (kept alive by the alias)\n", *weight);
    }

    std::puts("\n5. refcount under 8 threads x 50k copies");
    {
        auto shared = mine::make_shared<Node>(0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&shared] {
                for (int i = 0; i < 50'000; ++i) {
                    auto copy = shared;
                    auto weak = mine::WeakPtr<Node>(copy);
                    if (auto locked = weak.lock()) locked->value += 0;
                }
            });
        for (auto& th : threads) th.join();
        std::printf("   final use_count=%ld (expected 1), live=%d (expected 1)\n",
                    shared.use_count(), live_objects);
        assert(shared.use_count() == 1);
    }
    std::printf("\nall objects destroyed: live=%d (expected 0)\n", live_objects);
    return 0;
}
