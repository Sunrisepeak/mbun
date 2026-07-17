// runtime/icu_decompress.inc — per-item zstd decompression hook for ICU data.
//
// Blueprint: bun-ref/src/jsc/bindings/bun_icu_decompress.cpp. oven-sh/WebKit's
// ICU build patches udata.cpp to insert a WEAK call to bun_icu_maybe_decompress
// between TOC lookup and checkDataItem. Display-name items (curr/lang/region/
// unit/zone, non-en) are stored as raw zstd frames; everything else keeps its
// 0xda27 header and passes through after one u32 compare. Without a strong
// definition of this hook, compressed items reach ICU raw and Intl.*Format
// throws. This file is #included from prelude.hpp's GLOBAL MODULE FRAGMENT so
// the extern "C" symbol has plain external linkage and binds the weak-undef in
// libicuuc.a (mirrors the napi_* ABI placement).
//
// std-based per the task: pthread mutex + an intrusive malloc'd cache list
// (a handful of entries, process lifetime). Decompressed buffers are cached by
// .rodata pointer and never freed — ICU reads them for the process lifetime,
// exactly like bun's WTF::HashMap cache.

#if defined(__linux__)

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

static_assert(ZSTD_MAGICNUMBER == 0xFD2FB528u);
// Raw ICU items have bytes[2..3] == {0xda, 0x27} (ucmndata.h MAGIC1/MAGIC2),
// so their first u32 is 0x27da'hhhh — cannot collide with zstd's magic.

extern "C" __attribute__((weak)) const unsigned char bun_icu_zstd_dict[];
extern "C" __attribute__((weak)) const unsigned int bun_icu_zstd_dict_size;

namespace {
namespace icu_decompress_detail {

struct CacheNode {
    const void* key;
    void* buf;
    CacheNode* next;
};

struct Decompressor {
    pthread_mutex_t lock;
    CacheNode* cache;
    ZSTD_DCtx* dctx;
    ZSTD_DDict* ddict;
};

inline size_t frame_bound(int32_t toc_length)
{
    return toc_length > 0 ? static_cast<size_t>(toc_length) : (size_t{1} << 20);
}

inline Decompressor& get()
{
    static Decompressor d = [] {
        Decompressor x;
        pthread_mutex_init(&x.lock, nullptr);
        x.cache = nullptr;
        x.dctx = ZSTD_createDCtx();
        x.ddict = (&bun_icu_zstd_dict_size && bun_icu_zstd_dict_size)
            ? ZSTD_createDDict_byReference(bun_icu_zstd_dict, bun_icu_zstd_dict_size)
            : nullptr;
        return x;
    }();
    return d;
}

const void* decompress(const void* p, int32_t* length)
{
    Decompressor& d = get();
    pthread_mutex_lock(&d.lock);

    for (CacheNode* n = d.cache; n; n = n->next) {
        if (n->key == p) {
            void* cached = n->buf;
            *length = static_cast<int32_t>(ZSTD_getFrameContentSize(p, frame_bound(*length)));
            pthread_mutex_unlock(&d.lock);
            return cached;
        }
    }

    size_t clen = ZSTD_findFrameCompressedSize(p, frame_bound(*length));
    if (ZSTD_isError(clen)) {
        pthread_mutex_unlock(&d.lock);
        return p;
    }
    unsigned long long dlen = ZSTD_getFrameContentSize(p, clen);
    if (dlen == ZSTD_CONTENTSIZE_UNKNOWN || dlen == ZSTD_CONTENTSIZE_ERROR) {
        pthread_mutex_unlock(&d.lock);
        return p;
    }

    // aligned_alloc requires size to be a multiple of alignment; ICU item sizes
    // are only 4-aligned, so round up to 16 (matches bun's roundUpToMultipleOf).
    size_t alloc = (static_cast<size_t>(dlen) + size_t{15}) & ~size_t{15};
    void* buf = std::aligned_alloc(16, alloc);
    if (!buf) {
        pthread_mutex_unlock(&d.lock);
        return p;
    }

    size_t r = d.ddict
        ? ZSTD_decompress_usingDDict(d.dctx, buf, static_cast<size_t>(dlen), p, clen, d.ddict)
        : ZSTD_decompressDCtx(d.dctx, buf, static_cast<size_t>(dlen), p, clen);
    if (ZSTD_isError(r)) {
        std::free(buf);
        pthread_mutex_unlock(&d.lock);
        return p;
    }

    auto* node = static_cast<CacheNode*>(std::malloc(sizeof(CacheNode)));
    if (node) {
        node->key = p;
        node->buf = buf;
        node->next = d.cache;
        d.cache = node;
    }
    *length = static_cast<int32_t>(dlen);
    pthread_mutex_unlock(&d.lock);
    return buf;
}

} // namespace icu_decompress_detail
} // namespace

extern "C" const void* bun_icu_maybe_decompress(const void* p, int32_t* length)
{
    if (!p)
        return p;
    uint32_t magic;
    std::memcpy(&magic, p, sizeof(magic));
    if (magic != ZSTD_MAGICNUMBER) [[likely]]
        return p;
    return icu_decompress_detail::decompress(p, length);
}

#endif // __linux__
