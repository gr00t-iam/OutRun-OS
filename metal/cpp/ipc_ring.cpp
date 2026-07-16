// ============================================================================
// OUTRUN OS — cpp/ipc_ring.cpp   (compiled by g++, linked into the kernel)
// ============================================================================
// A real C++ translation unit in the bootable image: freestanding (no libstdc++,
// no exceptions, no RTTI, no iostream). It implements the manifesto's zero-copy
// single-producer / single-consumer ring buffer as an actual C++ class with
// methods and member state, and exposes a plain extern "C" surface so the C
// kernel and the assembly fast-path can drive it.
//
// Atomics are the GCC __atomic builtins (fully inline on x86-64, no library),
// so the acquire/release ordering is genuine, not faked.
// ============================================================================

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

namespace outrun {

// Fixed-capacity byte ring. Descriptors would carry pointers in the real
// zero-copy path; here the class owns a small inline arena so the whole thing
// is self-contained and needs no allocator.
class SpscRing {
public:
    static constexpr u32 CAP = 4096;

    void init() {
        __atomic_store_n(&head_, 0ull, __ATOMIC_RELAXED);
        __atomic_store_n(&tail_, 0ull, __ATOMIC_RELAXED);
    }

    // Producer: copy `len` bytes in once, publish with a release store.
    u32 push(const u8* src, u32 len) {
        u64 tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        u64 head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        u32 free = CAP - static_cast<u32>(tail - head);
        if (len > free) len = free;
        for (u32 i = 0; i < len; ++i) data_[(tail + i) % CAP] = src[i];
        __atomic_store_n(&tail_, tail + len, __ATOMIC_RELEASE);
        return len;
    }

    // Consumer: hand back a pointer INTO the arena (zero-copy) + advance head.
    u32 pop(const u8** out, u32 want) {
        u64 head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        u64 tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        u32 avail = static_cast<u32>(tail - head);
        if (want > avail) want = avail;
        *out = &data_[head % CAP];
        __atomic_store_n(&head_, head + want, __ATOMIC_RELEASE);
        return want;
    }

    u64 depth() const {
        return __atomic_load_n(&tail_, __ATOMIC_ACQUIRE)
             - __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
    }

    // Exposed so the assembly fast-path can peek head/tail/first-byte directly.
    u8*  arena()     { return data_; }
    u64* head_ptr()  { return &head_; }
    u64* tail_ptr()  { return &tail_; }

private:
    u64 head_;
    u64 tail_;
    u8  data_[CAP];
};

} // namespace outrun

// A single kernel-owned ring lives in .bss (trivial ctor, no __cxa_atexit).
static outrun::SpscRing g_ring;

extern "C" {
    void*    cpp_ring(void)                          { return &g_ring; }
    void     cpp_ring_init(void)                     { g_ring.init(); }
    u32      cpp_ring_push(const u8* src, u32 len)    { return g_ring.push(src, len); }
    u32      cpp_ring_pop(const u8** out, u32 want)   { return g_ring.pop(out, want); }
    u64      cpp_ring_depth(void)                     { return g_ring.depth(); }
    // Byte offsets the asm handshake needs into the ring object.
    u64      cpp_ring_arena_ptr(void)                 { return (u64)g_ring.arena(); }
    u64      cpp_ring_head_ptr(void)                  { return (u64)g_ring.head_ptr(); }
    u64      cpp_ring_tail_ptr(void)                  { return (u64)g_ring.tail_ptr(); }
}
