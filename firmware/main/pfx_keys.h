/*
 * Profalux PFX manufacturer keys - selection par serial (reverse DEVMEL 2026-08-07)
 *
 * Origine : table @VA 0x970D08 du binaire DEVMEL AirSendWebService (chemin PFX @0x7fa2c0).
 * La cle KEELOQ (crypt_key) n'est PAS aleatoire ni derivee par-serial : elle est
 * SELECTIONNEE dans une table de 63 cles par un slot encode dans le serial :
 *     (serial & 0x3FF) == 0x067      // marqueur famille PFX/Profalux
 *     index = (serial >> 12) - 1     // dans [0,62]
 *     crypt_key = deobfuscation(index)  // table salee, cf pfx_keys.c
 *
 * Les cles sont stockees OBFUSQUEES (salt + melange par index) : AUCUNE valeur en
 * clair dans la source, y compris l'index 0. On ne livre JAMAIS de cle en clair.
 */
#ifndef PFX_KEYS_H
#define PFX_KEYS_H

#include <stdint.h>
#include <stdbool.h>

#define PFX_FAMILY_MARKER 0x067u   /* serial & 0x3FF doit valoir ceci */
#define PFX_KEY_COUNT     63

/**
 * Deobfusque et retourne la crypt_key pour un serial Profalux.
 * @param serial   serial 28-bit lu en clair dans la trame
 * @param out_key  recoit la crypt_key deobfusquee (si succes)
 * @param out_idx  recoit l'index [0,62] (peut etre NULL)
 * @return true si serial PFX valide (marqueur + index en plage), false sinon.
 */
bool pfx_key_for_serial(uint32_t serial, uint64_t *out_key, uint8_t *out_idx);

#endif
