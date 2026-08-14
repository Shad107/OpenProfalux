/*
 * KEELOQ 528 rounds NLFSR algorithm - Microchip HCS301 compatible
 *
 * Reference: Microchip DS21143C (HCS301 datasheet)
 * Patent US 5517187 (Nanoteq 1996) - EXPIRED 2016, algorithm is PUBLIC
 * NLF constant 0x3A5C742E is universally used and public
 */
#ifndef KEELOQ_H
#define KEELOQ_H

#include <stdint.h>

#define KEELOQ_NLF        0x3A5C742E
#define KEELOQ_ROUNDS            528

/* Learning types - Microchip standard + variants (=cf Flipper Zero keeloq_common.h) */
typedef enum {
    KEELOQ_LEARN_UNKNOWN            = 0,
    KEELOQ_LEARN_SIMPLE             = 1,
    KEELOQ_LEARN_NORMAL             = 2,
    KEELOQ_LEARN_SECURE             = 3,
    KEELOQ_LEARN_MAGIC_XOR_TYPE_1   = 4,
    KEELOQ_LEARN_FAAC               = 5,
    KEELOQ_LEARN_MAGIC_SERIAL_1     = 6,
    KEELOQ_LEARN_MAGIC_SERIAL_2     = 7,
    KEELOQ_LEARN_MAGIC_SERIAL_3     = 8,
    /* 9 reserved (Beninca) */
    KEELOQ_LEARN_CHAMBERLAIN_SELF   = 100, /* Custom: our Profalux hypothesis */
} keeloq_learn_type_t;

/**
 * KEELOQ encrypt : forward 528 rounds NLFSR
 */
uint32_t keeloq_encrypt(uint32_t data, uint64_t key);

/**
 * KEELOQ decrypt : inverse 528 rounds NLFSR
 */
uint32_t keeloq_decrypt(uint32_t data, uint64_t key);

/**
 * Simple Learning : crypt_key = MFG key directly
 */
static inline uint64_t keeloq_simple_learning(uint64_t mfk) { return mfk; }

/**
 * Normal Learning : crypt_key derived from serial via KL_decrypt with MFG key
 */
uint64_t keeloq_normal_learning(uint32_t serial, uint64_t mfk);

/**
 * Secure Learning : crypt_key derived from seed via KL_decrypt with MFG key
 */
uint64_t keeloq_secure_learning(uint32_t serial, uint32_t seed, uint64_t mfk);

/**
 * Chamberlain Self-Learn (custom Profalux hypothesis):
 * TX generates crypt_key random, receiver stores it directly
 * NO MFG key needed on receiver side
 *
 * Cette fonction retourne juste la crypt_key passée (=no derivation)
 */
static inline uint64_t keeloq_chamberlain_learning(uint64_t crypt_key_random) {
    return crypt_key_random;
}

/* Bit utils */
static inline uint32_t kl_bit(uint64_t x, uint32_t n) {
    return (uint32_t)((x >> n) & 1ULL);
}
static inline uint32_t kl_g5(uint32_t x, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    return (kl_bit(x, a) << 0) | (kl_bit(x, b) << 1) | (kl_bit(x, c) << 2) |
           (kl_bit(x, d) << 3) | (kl_bit(x, e) << 4);
}

#endif
