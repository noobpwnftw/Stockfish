#pragma once

// ChessTB's table file layout: a serial reader over a mapped file's header, and
// the packed integer vectors (block offsets and sizes) a table file stores. Bit
// reads are unaligned and read up to two 8-byte words, so every packed region
// must be followed by >= 8 readable bytes; on disk, whatever follows each
// section (more sections, the compressed data, the end checksum) is.

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Stockfish::ChessTB {

struct Serial_Memory_Reader {
    Serial_Memory_Reader(Const_Span<uint8_t> span) :
        m_caret(span.begin()),
        m_begin(span.begin()),
        m_end(span.end()) {}

    template<typename T>
    T read() {
        T val;
        assert(m_caret + sizeof(val) <= m_end);
        std::memcpy(&val, m_caret, sizeof(val));
        m_caret += sizeof(val);
        return val;
    }

    void advance(size_t size) {
        assert(m_caret + size <= m_end);
        m_caret += size;
    }

    void align(size_t alignment) {
        const size_t misalignment = (m_caret - m_begin) % alignment;
        if (misalignment == 0)
            return;

        assert(m_caret + (alignment - misalignment) <= m_end);
        m_caret += alignment - misalignment;
    }

    const uint8_t* caret() const { return m_caret; }

   private:
    const uint8_t* m_caret;
    const uint8_t* m_begin;
    const uint8_t* m_end;
};

constexpr uint64_t mono_low_mask(unsigned width) {
    return width >= 64 ? ~uint64_t{0} : ((uint64_t{1} << width) - 1);
}

// Read `width` (<=64) bits starting at bit `bitpos`. Requires >= 8 readable
// bytes past the last touched byte (see header note).
inline uint64_t mono_read_bits(const uint8_t* base, size_t bitpos, unsigned width) {
    if (width == 0)
        return 0;
    const size_t   byte = bitpos >> 3;
    const unsigned bit  = static_cast<unsigned>(bitpos & 7);
    uint64_t       lo;
    std::memcpy(&lo, base + byte, 8);
    uint64_t v = lo >> bit;
    if (bit + width > 64)
    {
        uint64_t hi;
        std::memcpy(&hi, base + byte + 8, 8);
        v |= hi << (64 - bit);
    }
    return v & mono_low_mask(width);
}

// On-disk/in-memory blob layout: [sample_bits][delta_bits].
// The 3 width/param bytes live in the caller's section header, not the blob.
struct Mono_Uint_Vec {
    const uint8_t* m_base         = nullptr;  // start of sample region
    const uint8_t* m_delta        = nullptr;  // start of delta region
    uint8_t        m_log2_bu      = 0;
    uint8_t        m_sample_width = 0;
    uint8_t        m_offset_width = 0;

    Mono_Uint_Vec() = default;

    // num_values is the count of stored offsets (== block_cnt + 1).
    Mono_Uint_Vec(const uint8_t* blob,
                  size_t         num_values,
                  uint8_t        log2_bu,
                  uint8_t        sample_width,
                  uint8_t        offset_width) :
        m_base(blob),
        m_log2_bu(log2_bu),
        m_sample_width(sample_width),
        m_offset_width(offset_width) {
        const size_t num_samples = ceil_div(num_values, size_t{1} << log2_bu);
        m_delta                  = blob + ceil_div(num_samples * sample_width, size_t{8});
    }

    // {O[i], O[i+1]}; valid for i in [0, num_values-2] (i.e. any block id).
    std::array<uint64_t, 2> get2(size_t i) const {
        const size_t   sb0 = i >> m_log2_bu;
        const size_t   sb1 = (i + 1) >> m_log2_bu;
        const uint64_t d0  = mono_read_bits(m_delta, i * m_offset_width, m_offset_width);
        const uint64_t d1  = mono_read_bits(m_delta, (i + 1) * m_offset_width, m_offset_width);
        const uint64_t b0  = mono_read_bits(m_base, sb0 * m_sample_width, m_sample_width);
        const uint64_t b1 =
          (sb1 == sb0) ? b0 : mono_read_bits(m_base, sb1 * m_sample_width, m_sample_width);
        return {b0 + d0, b1 + d1};
    }

    static size_t
    on_disk_bytes(size_t num_values, uint8_t log2_bu, uint8_t sample_width, uint8_t offset_width) {
        const size_t num_samples = ceil_div(num_values, size_t{1} << log2_bu);
        return ceil_div(num_samples * sample_width, size_t{8})
             + ceil_div(num_values * offset_width, size_t{8});
    }
};

struct Min0_Uint_Vec {
    const uint8_t* m_data  = nullptr;
    uint8_t        m_width = 0;

    Min0_Uint_Vec() = default;
    Min0_Uint_Vec(const uint8_t* data, uint8_t width) :
        m_data(data),
        m_width(width) {}

    uint64_t get(size_t i) const { return mono_read_bits(m_data, i * m_width, m_width); }

    static size_t on_disk_bytes(size_t size, uint8_t width) {
        return ceil_div(size * width, size_t{8});
    }
};

// Per-color offset section layout used by all EGTB formats:
//   [u8 log2_bu][u8 sample_width][u8 offset_width][u8 usz_width]
//   mono blob  (Mono_Uint_Vec::on_disk_bytes bytes)
//   min0 blob  (Min0_Uint_Vec::on_disk_bytes bytes; DTC and DTM50 only)
// No padding: all reads are memcpy-based (alignment-tolerant); the trailing
// 8-byte read-slack required by mono_read_bits is provided by whatever follows
// the section in the file (further sections, the compressed-data region, or
// the end checksum).

}  // namespace Stockfish::ChessTB
