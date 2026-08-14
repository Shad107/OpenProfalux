#include "pfx_keys.h"

/*
 * Cles fabricant Profalux stockees OBFUSQUEES (aucune valeur ici n'egale la cle
 * reelle, y compris l'index 0 - contrairement au simple ^index de DEVMEL).
 *
 *   masque(i)  = PFX_OBF_SALT ^ (i * PFX_OBF_GOLDEN)      (mod 2^64)
 *   stocke[i]  = cle_reelle[i] ^ masque(i)
 *   cle[i]     = stocke[i] ^ masque(i)                    (deobfuscation runtime)
 *
 * Obfuscation reversible (comme DEVMEL) : ne protege pas contre l'extraction
 * par un attaquant qui execute/emule le firmware, mais evite de livrer des cles
 * en clair / greppables. Les cles Profalux sont de toute facon deja extractibles
 * du binaire public DEVMEL AirSendWebService.
 */
#define PFX_OBF_SALT   0xA5C3F00D5EED1234ULL
#define PFX_OBF_GOLDEN 0x9E3779B97F4A7C15ULL

static const uint64_t _PFX_OBF[PFX_KEY_COUNT] = {
    0x2a557662e1357485ULL, 0x0c3674acd9c81873ULL, 0x55fef0ed13163b21ULL,
    0x7d35a1eb51b9ee86ULL, 0xd72a536e64017a60ULL, 0x0b1ede7d52602c47ULL,
    0x0afadffedebf2ec6ULL, 0xd3fbff9809af45e7ULL, 0xf2ef2740ff5dba76ULL,
    0x5bf88b12c6dacd1dULL, 0x66846b8b744cdfa9ULL, 0x963beb830df655baULL,
    0x27b9fcd61bc4e8c8ULL, 0xc7f65bf81a778267ULL, 0x3ece6ed8ba24b874ULL,
    0x5872284bd338f837ULL, 0xd59bde3adb0a4d40ULL, 0x0b0f888daaa2c2ebULL,
    0x9d0623f838d1d58eULL, 0x56736d325294eb9bULL, 0x6ead7f892cf2f44cULL,
    0x3bdda6008ff6f89eULL, 0x72b9483ee139830bULL, 0x3584e382ff26ebc4ULL,
    0x8c3061b6ae2078e1ULL, 0xc2d7dc7ad5ebbf87ULL, 0x37e8ad12de70a691ULL,
    0x159f773afffd2e89ULL, 0xe637776fa2ee8685ULL, 0x983056f5a841f72fULL,
    0xd361d02b3bcf76b2ULL, 0x0bc620eed37ea343ULL, 0x83f7506cf843f3f4ULL,
    0xf57e145cfcae7704ULL, 0x7b3be7aa02cc0094ULL, 0x179a71b97bea3823ULL,
    0x3738523c6ed782dcULL, 0x4ee5519b439f2cbdULL, 0x78b941879283382dULL,
    0x1a11d4628bf25b9cULL, 0x3de77b75d58a606bULL, 0x73923e9bf7554434ULL,
    0xf31cf4fd0a8292e6ULL, 0xd3567fff84fc2765ULL, 0x5ae425094242c48bULL,
    0xd0a16c2d22e52ffdULL, 0xcd4de01156f03da3ULL, 0x13fd6361505cd337ULL,
    0xd0920865ef78882fULL, 0xe7ecb1ed69e4dbccULL, 0xc9df9212cb1ff42fULL,
    0xf1e936df7cc30c40ULL, 0xe237997ffa95407cULL, 0x2515a7f46a5047caULL,
    0x2a85c93b5d547ce5ULL, 0x0eef1af1a66f218bULL, 0x6e6cd28e1f1b3a29ULL,
    0xd3c1b09797a5ff28ULL, 0xdaab331144444517ULL, 0x6477ecf751b1865cULL,
    0xbe4ea470d5267029ULL, 0x9075424a1a4f8108ULL, 0x99d740a9bd3fa22cULL,
};

static inline uint64_t _pfx_mask(uint32_t i) {
    return PFX_OBF_SALT ^ ((uint64_t)i * PFX_OBF_GOLDEN);
}

bool pfx_key_for_serial(uint32_t serial, uint64_t *out_key, uint8_t *out_idx) {
    if ((serial & 0x3FFu) != PFX_FAMILY_MARKER) return false;
    uint32_t idx = (serial >> 12) - 1u;          /* index = (serial>>12)-1 */
    if (idx >= PFX_KEY_COUNT) return false;       /* wrap si serial>>10 == 0 => rejete */
    if (out_key) *out_key = _PFX_OBF[idx] ^ _pfx_mask(idx);
    if (out_idx) *out_idx = (uint8_t)idx;
    return true;
}
