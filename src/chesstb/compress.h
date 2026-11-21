#pragma once

// ChessTB's decoders: LZ4 point reads (WDL) and LZMA block decoding (the
// distance tables).

#include "common.h"
#include "LZMA/LzmaDec.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Stockfish::ChessTB {

// Point decompression of a dictionary-primed LZ4 block: a block is held as a
// strided checkpoint index rather than decompressed bytes, and a read enters at
// the nearest checkpoint, then chases match references back to a literal or into
// the dictionary. Nothing is materialized.

inline constexpr size_t LZ4_MIN_MATCH         = 4;  // LZ4's MINMATCH, not exported
inline constexpr size_t LZ4_POINT_STRIDE_LOG2 = 8;
inline constexpr size_t LZ4_POINT_STRIDE      = size_t{1} << LZ4_POINT_STRIDE_LOG2;

struct LZ4_Point_Checkpoint {
    uint32_t in_off;   // the sequence's token, within the block
    uint32_t out_off;  // where its literals land in the output
};

inline size_t lz4_point_index_bytes(size_t out_size) {
    return ((out_size + LZ4_POINT_STRIDE - 1) >> LZ4_POINT_STRIDE_LOG2)
         * sizeof(LZ4_Point_Checkpoint);
}

// Parse-only pass: moves no data, drops a checkpoint every LZ4_POINT_STRIDE
// output bytes. `dst` must be lz4_point_index_bytes(out_size) long.
inline void lz4_build_point_index(Const_Span<uint8_t> src, Span<uint8_t> dst) {
    LZ4_Point_Checkpoint* const cp     = reinterpret_cast<LZ4_Point_Checkpoint*>(dst.data());
    const size_t                num_cp = dst.size() / sizeof(LZ4_Point_Checkpoint);

    const uint8_t* const ibegin  = src.data();
    const uint8_t* const iend    = ibegin + src.size();
    const uint8_t*       ip      = ibegin;
    uint32_t             op      = 0;
    size_t               next_cp = 0;

    while (ip < iend)
    {
        const LZ4_Point_Checkpoint here{static_cast<uint32_t>(ip - ibegin), op};

        const unsigned token   = *ip++;
        size_t         lit_len = token >> 4;
        if (lit_len == 15)
        {
            unsigned s;
            do
            {
                s = *ip++;
                lit_len += s;
            } while (s == 255);
        }
        ip += lit_len;
        op += static_cast<uint32_t>(lit_len);

        if (ip < iend)  // a final sequence is literals only
        {
            ip += 2;  // match offset
            size_t match_len = token & 15;
            if (match_len == 15)
            {
                unsigned s;
                do
                {
                    s = *ip++;
                    match_len += s;
                } while (s == 255);
            }
            op += static_cast<uint32_t>(match_len + LZ4_MIN_MATCH);
        }

        while (next_cp < num_cp && (next_cp << LZ4_POINT_STRIDE_LOG2) < op)
            cp[next_cp++] = here;
    }
}

// Reads output byte `pos` through the index built above.
inline uint8_t lz4_point_read(Const_Span<uint8_t> src,
                              Const_Span<uint8_t> dict,
                              const uint8_t*      index,
                              size_t              pos) {
    const LZ4_Point_Checkpoint* const cp     = reinterpret_cast<const LZ4_Point_Checkpoint*>(index);
    const uint8_t* const              ibegin = src.data();
    int64_t                           target = static_cast<int64_t>(pos);

    for (;;)
    {
        if (target < 0)  // chased back past the block, into the dict
            return dict[static_cast<size_t>(static_cast<int64_t>(dict.size()) + target)];

        const LZ4_Point_Checkpoint start = cp[target >> LZ4_POINT_STRIDE_LOG2];
        const uint8_t*             ip    = ibegin + start.in_off;
        int64_t                    op    = start.out_off;

        for (;;)
        {
            const unsigned token   = *ip++;
            int64_t        lit_len = token >> 4;
            if (lit_len == 15)
            {
                unsigned s;
                do
                {
                    s = *ip++;
                    lit_len += s;
                } while (s == 255);
            }
            const uint8_t* const literals = ip;
            ip += lit_len;

            if (target < op + lit_len)
                return literals[target - op];
            op += lit_len;

            const int64_t match_off = ip[0] | (ip[1] << 8);
            ip += 2;
            int64_t match_len = token & 15;
            if (match_len == 15)
            {
                unsigned s;
                do
                {
                    s = *ip++;
                    match_len += s;
                } while (s == 255);
            }
            match_len += static_cast<int64_t>(LZ4_MIN_MATCH);

            if (target < op + match_len)
            {
                // Fold every period the match repeats in one step, so a long
                // run at a short offset costs one hop, not one per period.
                // Offsets are two bytes and positions 32 bits, so is the divide.
                const uint32_t into   = static_cast<uint32_t>(target - op);
                const uint32_t period = static_cast<uint32_t>(match_off);
                target -= (into < period) ? match_off
                                          : (static_cast<int64_t>(into / period) + 1) * match_off;
                break;
            }
            op += match_len;
        }
    }
}

// LZMA's default allocator (g_Alloc), without the rest of the LZMA SDK's Alloc.
inline void* lzma_alloc_fn(ISzAllocPtr, size_t size) { return size ? std::malloc(size) : nullptr; }
inline void  lzma_free_fn(ISzAllocPtr, void* address) { std::free(address); }
inline const ISzAlloc lzma_alloc = {lzma_alloc_fn, lzma_free_fn};

inline void lzma_decompress_into(Span<uint8_t> dst, Const_Span<uint8_t> src) {
    assert(src.size() >= LZMA_PROPS_SIZE);

    size_t         out_sz = dst.size();
    size_t         in_sz  = src.size() - LZMA_PROPS_SIZE;
    const uint8_t* props  = src.data() + in_sz;

    ELzmaStatus                status;
    [[maybe_unused]] const int ret =
      LzmaDecode(dst.data(), &out_sz, src.data(), &in_sz, props, LZMA_PROPS_SIZE, LZMA_FINISH_ANY,
                 &status, &lzma_alloc);

    assert(ret == SZ_OK && out_sz == dst.size());
}

}  // namespace Stockfish::ChessTB
