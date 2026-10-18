/*
 * vec_bits.h — shared bit-layout helpers for the vector tier embed path.
 *
 * Pure C (usable from C and C++), header-only, no dependencies.  These are
 * the SINGLE SOURCE OF TRUTH for the external encoder's emission side and
 * MUST stay byte-identical to src/vector.h / zig/src/vector.zig:
 *
 *   pack4_le(b0..b3) = b0 | b1<<8 | b2<<16 | b3<<24      (little-endian)
 *
 * Since U9 (2026-08-11) the encoder emits ONLY the int8 chunks: one
 * __vec_q__(entity_sym, chunk_idx, packed_4x_int8_u32) row per 4 int8 lanes,
 * 96 chunks per vector.  The 256-bit band signature the old MIH query path
 * sliced (vec_band_slice/vec_band_set) is no longer emitted or read — the
 * engine scans and ranks, so there is no band layout to agree on.
 */
#ifndef VEC_BITS_H
#define VEC_BITS_H

#include <stdint.h>
#include <string.h>

/* Pack 4 int8 values into one u32, little-endian byte order. */
static inline uint32_t vec_pack4_le(int8_t b0, int8_t b1, int8_t b2, int8_t b3) {
    return (uint32_t)((uint32_t)((uint8_t)b0))
         | ((uint32_t)((uint8_t)b1) << 8)
         | ((uint32_t)((uint8_t)b2) << 16)
         | ((uint32_t)((uint8_t)b3) << 24);
}

/* Unpack a packed u32 into 4 int8 values (little-endian). */
static inline void vec_unpack4_le(uint32_t packed, int8_t out[4]) {
    out[0] = (int8_t)(uint8_t)(packed >> 0);
    out[1] = (int8_t)(uint8_t)(packed >> 8);
    out[2] = (int8_t)(uint8_t)(packed >> 16);
    out[3] = (int8_t)(uint8_t)(packed >> 24);
}

/* float32 -> its IEEE-754 bit pattern as u32 (embed.py float32_to_bits). */
static inline uint32_t vec_float32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* u32 bit pattern -> float32 (oracle/embed.py bits_to_float32). */
static inline float vec_bits_float32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

#endif /* VEC_BITS_H */
