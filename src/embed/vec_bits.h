/*
 * vec_bits.h — shared bit-layout helpers for the vector tier embed path.
 *
 * Pure C (usable from C and C++), header-only, no dependencies.  These are
 * the SINGLE SOURCE OF TRUTH for dl-embed's emission side and MUST stay
 * byte-identical to src/vector.c's band slicing and to scripts/embed.py:
 *
 *   band j = (sig[j/2] >> ((1 - j%2) * 16)) & 0xFFFF     (MSB-first words)
 *   pack4_le(b0..b3) = b0 | b1<<8 | b2<<16 | b3<<24      (little-endian)
 *
 * Band 0 = HIGH 16 bits of sig[0]; band 1 = LOW 16 of sig[0]; etc.
 * (vector.c:band_slice — the C1 gate.)
 */
#ifndef VEC_BITS_H
#define VEC_BITS_H

#include <stdint.h>
#include <string.h>

/* Band j (0..VEC_M-1) of a c-bit signature stored MSB-first in VEC_SIG_WORDS
 * u32 words.  Identical expression to vector.c:band_slice. */
static inline uint32_t vec_band_slice(const uint32_t *sig, int j) {
    return (sig[j / 2] >> ((1u - (uint32_t)(j % 2)) * 16u)) & 0xFFFFu;
}

/* Inverse of vec_band_slice: set band j to val16 (round-trip helper). */
static inline void vec_band_set(uint32_t *sig, int j, uint32_t val16) {
    uint32_t shift = (1u - (uint32_t)(j % 2)) * 16u;
    uint32_t mask = 0xFFFFu << shift;
    sig[j / 2] = (sig[j / 2] & ~mask) | ((val16 & 0xFFFFu) << shift);
}

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
