#include "pfx_keys.h"
#include "keeloq.h"

/*
 * Cles fabricant Profalux stockees OBFUSQUEES (aucune valeur ici n'egale la cle
 * reelle, y compris l'index 0).
 *
 *   masque(i)  = KeeLoq(seed_hi ^ i*prime, OBFK1)<<32 | KeeLoq(seed_lo ^ i*prime, OBFK2)
 *   stocke[i]  = cle_reelle[i] ^ masque(i)
 *   cle[i]     = stocke[i] ^ masque(i)                    (deobfuscation runtime)
 *
 * Le masque est NON-LINEAIRE (deux KeeLoq 528 tours par index) : impossible a
 * retrouver par simple XOR de constantes greppables. On ne deobfusque qu'une
 * seule cle a la fois (l'index demande), jamais toute la table en RAM.
 *
 * Reste reversible pour qui execute/emule le firmware : c'est une obfuscation
 * au repos, pas un secret cryptographique. On ne livre JAMAIS de cle en clair.
 */
#define PFX_OBFK1  0x1B7A4C2E93F5A681ULL
#define PFX_OBFK2  0xC4E9D30F2A75B18CULL
#define PFX_OBF_PRIME  0x01000193u
#define PFX_OBF_SHI    0xA5A50000u
#define PFX_OBF_SLO    0x5A5A5A5Au

static const uint64_t _PFX_OBF[PFX_KEY_COUNT] = {
    0xA4D87A18212ABBD3ULL,     0x66FC3402B786F87AULL,     0xE68DC12D255BC510ULL,
    0xE37208B6ED66811DULL,     0x86245395FFC36BABULL,     0xCE4206992A2E788AULL,
    0x8F3172C24954A14FULL,     0xF8D43DE0FF9A09ACULL,     0xEDF6D4E357EC9F89ULL,
    0x90DDB2C3FA640217ULL,     0xDA57A47D5E61F6C3ULL,     0xCA2010C56F2A0C83ULL,
    0xFF7AA4DF94617C30ULL,     0x0009DCBCAC0CEC81ULL,     0x0038E127D1DB1BF7ULL,
    0x5359C4A1DBF8A2A9ULL,     0xC8BC2C2D08631C46ULL,     0xD31FA18A4DFEDF3AULL,
    0xA83128C024D043B5ULL,     0xB64098EEFCFC5C7CULL,     0x66B4E8C103A63E5DULL,
    0x49EFD391BF78BA0BULL,     0xC02A7CD958C1245CULL,     0x73C52416A55DA983ULL,
    0xE35AF6CDC03B68EDULL,     0x16B7AD3204A6603EULL,     0x4F7108922F9A28B9ULL,
    0x4B13AB79E9B4C271ULL,     0x0A92369587447165ULL,     0x79E797E127508A5AULL,
    0x913895A16A916E69ULL,     0xB3C8EED93B035CDBULL,     0x7A871DA573459E60ULL,
    0x86C31AFCACD61968ULL,     0xCC0A8748FBF74CC9ULL,     0x9F86A0E7C3D9272FULL,
    0x0A846BFAF2E70AEAULL,     0x79AF70DB7CE32C71ULL,     0xA189A995DD8066A4ULL,
    0x77DF3D7AA98164D6ULL,     0x7EC18EB2C41D6464ULL,     0x31E7071BEA7A8AF5ULL,
    0x8DEE7F954CB98537ULL,     0xC0A8AAD5C8C3D69CULL,     0x8BC8C76C89E74243ULL,
    0xF9549F513B81E1B7ULL,     0x43ECCDA69D808B50ULL,     0xD4C5F8C942253991ULL,
    0x4CC467DD69550D40ULL,     0x152FB68923A7CE9FULL,     0xFEBCB715155F095DULL,
    0x98B684682FB89F0EULL,     0x5625854712957BC4ULL,     0x862B709443849C5AULL,
    0x12D6B2773DA73151ULL,     0x4638DD11BF354FDBULL,     0xEC4B1FDF31A5E0FEULL,
    0xC8A38B1969D0F0D7ULL,     0x2558AD7C97D6BEC3ULL,     0xD5C7A22836E0A1C0ULL,
    0xF82158A02D17D790ULL,     0x99E73F22946FB64FULL,     0x7A7DE82B37602F7DULL,
};

static inline uint64_t _pfx_mask(uint32_t i) {
    uint32_t hi = keeloq_encrypt(PFX_OBF_SHI ^ (i * PFX_OBF_PRIME), PFX_OBFK1);
    uint32_t lo = keeloq_encrypt(PFX_OBF_SLO ^ (i * PFX_OBF_PRIME), PFX_OBFK2);
    return ((uint64_t)hi << 32) | lo;
}

bool pfx_key_for_serial(uint32_t serial, uint64_t *out_key, uint8_t *out_idx) {
    if ((serial & 0x3FFu) != PFX_FAMILY_MARKER) return false;
    uint32_t idx = (serial >> 12) - 1u;          /* index = (serial>>12)-1 */
    if (idx >= PFX_KEY_COUNT) return false;       /* wrap si serial>>10 == 0 => rejete */
    if (out_key) *out_key = _PFX_OBF[idx] ^ _pfx_mask(idx);
    if (out_idx) *out_idx = (uint8_t)idx;
    return true;
}
