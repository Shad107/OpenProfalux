#include "keeloq.h"

uint32_t keeloq_encrypt(uint32_t data, uint64_t key) {
    uint32_t x = data;
    for (uint32_t r = 0; r < KEELOQ_ROUNDS; r++) {
        uint32_t nlf_bit = kl_bit(KEELOQ_NLF, kl_g5(x, 1, 9, 20, 26, 31));
        uint32_t b = kl_bit(x, 0) ^ kl_bit(x, 16) ^ kl_bit(key, r & 63) ^ nlf_bit;
        x = (x >> 1) | (b << 31);
    }
    return x;
}

uint32_t keeloq_decrypt(uint32_t data, uint64_t key) {
    uint32_t x = data;
    for (uint32_t r = 0; r < KEELOQ_ROUNDS; r++) {
        uint32_t nlf_bit = kl_bit(KEELOQ_NLF, kl_g5(x, 0, 8, 19, 25, 30));
        uint32_t b = kl_bit(x, 31) ^ kl_bit(x, 15) ^ kl_bit(key, (15 - r) & 63) ^ nlf_bit;
        x = (x << 1) | b;
    }
    return x;
}

uint64_t keeloq_normal_learning(uint32_t serial, uint64_t mfk) {
    uint32_t d = serial & 0x0FFFFFFF;
    uint32_t k1 = keeloq_decrypt(d | 0x20000000, mfk);
    uint32_t k2 = keeloq_decrypt(d | 0x60000000, mfk);
    return ((uint64_t)k2 << 32) | k1;
}

uint64_t keeloq_secure_learning(uint32_t serial, uint32_t seed, uint64_t mfk) {
    uint32_t k1 = keeloq_decrypt(serial & 0x0FFFFFFF, mfk);
    uint32_t k2 = keeloq_decrypt(seed, mfk);
    return ((uint64_t)k1 << 32) | k2;
}
