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
    0x37a613d4cdba79f3ULL, 0x34d7798b765b34edULL, 0x68845fe524786cf7ULL,
    0xab45d23cde1bf8e1ULL, 0xd71aac2264846f78ULL, 0x40bafa9e812d9e0eULL,
    0x82ca3cdf72673f23ULL, 0x72f52ecca1e213ceULL, 0x674d20b5c0d94fa5ULL,
    0x0d24df5a2f16f728ULL, 0x129cdc88eb3c1fd5ULL, 0xf64530bb6aac8b8aULL,
    0xadc608159c6557adULL, 0x0e1b7847106c437eULL, 0x777c32fdf8a2d066ULL,
};

static inline uint64_t _pfx_mask(uint32_t i) {
    return PFX_OBF_SALT ^ ((uint64_t)i * PFX_OBF_GOLDEN);
}

bool pfx_key_for_serial(uint32_t serial, uint64_t *out_key, uint8_t *out_idx) {
    if ((serial & 0x3FFu) != PFX_FAMILY_MARKER) return false;
    uint32_t idx = (serial >> 10) - 1u;          /* index = (serial>>10)-1 */
    if (idx >= PFX_KEY_COUNT) return false;       /* wrap si serial>>10 == 0 => rejete */
    if (out_key) *out_key = _PFX_OBF[idx] ^ _pfx_mask(idx);
    if (out_idx) *out_idx = (uint8_t)idx;
    return true;
}
