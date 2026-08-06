# OpenProfalux - Synthèse recherche technique

**Session marathon 2026-08-05, ~4h20, ~80 messages, corrigée à plusieurs reprises par le user (=chef service dev IT senior 20+ ans)**

## Contexte projet

User a 5 volets Profalux installés avant génération Zigbee, avec :
- 5× télécommandes individuelles **MAI-EMPX-B1-NC** datées 20/04/2009
- 1× télécommande multi-canal **MAI-EMNOE** (=8 canaux)

**Objectif** : piloter les 5 volets depuis Home Assistant sans dépendance cloud, sans sacrifier de télécommande, à coût minimal.

## Découvertes techniques cristallisées

### 1. Preuve directe DEVMEL PFX clone=3 FULL

DEVMEL AirSend Duo est officiellement compatible Profalux 868 MHz (=confirmé Domadoo, Domotique-Store, Smarthome-Europe). Son OpenAPI officielle YAML documente :

```yaml
clone:
  type: integer
  description: Clone mode => 0=none ; 1=unavailable ; 2=temporarly ; 3=full
```

Symbol C++ dans `libDevmelSDKjni.x86_64.so` :
```
DevmelSDK::content::Channel::isCloneable(unsigned short) @ 0xfee0
```

Table hardcoded de 183 channels supportés à vaddr `0x0044b8f0` + switch de 50 cases retournant le mode clone.

**PFX (=Profalux, channel_id 25455) est marqué clone=3 (=FULL)**. Contraste avec KLQ868 générique = clone=2 (=temporary only).

### 2. Implication technique cruciale

Si Profalux utilisait un décodeur HCS500 Microchip standard :
- Les 3 modes learn (=Simple, Normal, Secure) **exigent MFG key stockée dans décodeur**
- Un nouvel émetteur ne peut être ajouté que si le décodeur peut valider ses trames avec la MFG key partagée
- DEVMEL ne pourrait faire que clone=2 (=grabber-replay one-shot désynchronisant)

**Le fait que DEVMEL fasse clone=3 FULL permanent PROUVE que Profalux N'utilise PAS HCS500 standard**. Le décodeur moteur implémente une variante propriétaire — probablement Chamberlain Self-Learn (=brevet US 5686904).

### 3. Architecture Chamberlain Self-Learn hypothèse

Le décodeur moteur Profalux est probablement un **MCU générique** (=PIC16Fxxx, ATmega, MSP430) avec firmware software KEELOQ qui :

1. **Pendant fenêtre 60s learn** (=déclenchée par télécommande d'origine + trombone/PROG) :
   - Accepte n'importe quelle trame KEELOQ format valide
   - Extrait (serial, encrypted_hop) de la trame
   - Stocke directement (serial, crypt_key) sans dérivation MFG key
2. **En utilisation normale** :
   - Valide trames avec crypt_key stockée
   - Anti-replay via rolling counter comparison
   - Refuse si counter <= last_counter_seen

### 4. Preuves négatives accumulées

Voies logicielles épuisées :
- **12 GB firmwares Profalux locaux** grep exhaustif : 0 NLF KEELOQ, 0 MFG key hardcoded
- **mtd7 pfxbox2** (=user data partition) : **VIDE** = firmware factory pas dump utilisateur
- **DEVMEL SDK/binary disasm** : 0 constante KEELOQ, algo XXTEA pour transport IPv6 uniquement
- **Blob 221KB DEVMEL** : précomputations XXTEA + enum ThingEvent, PAS catalogue MFG keys
- **Flipper Zero keystore chiffré** : master key dans crypto enclave STM32WB55, non extractible offline
- **Community keystores** : Profalux absent des 40+ fabricants Flipper (=fabricant français niche)
- **Publications MFG keys en clair** : aucune trouvée (=li0ard/Kaiju/shilapi = env vars ou test keys)

### 5. Chip candidat télécommande

**HCS301** (=~85% confiance) selon convergence :
- Ma mémoire propage HCS301 depuis 2026-06-27
- HCS301 = seul avec **Secure Learn** support (=cohérent avec variantes propriétaires)
- Frame 66-bit (=2 status + 4 button + 28 serial + 32 encrypted)
- Fréquence 868.350 MHz OOK confirmée via captures Flipper beedec65

**HCS310 N'EXISTE PAS** (=confusion Google avec HCS300/301/320).

### 6. Chip candidat décodeur moteur

**INCONNU** — à identifier via ouverture physique d'un moteur récup. Hypothèses :
- **A) HCS500/512/515 dédié Microchip** → exige MFG key → incompatible clone=3 DEVMEL
- **B) MCU générique + firmware software Chamberlain-like** → **hypothèse la plus probable** au vu de DEVMEL clone=3
- **C) Chip totalement custom Profalux** → analyse spécifique

## Attaques KEELOQ documentées (=contexte)

Papers académiques (=Kasper/Eisenbarth 2008 CRYPTO, Bogdanov 2007/2008, Courtois FSE 2008) :
- **DPA sur récepteur avec MFG key** : 10-30 power traces suffisent avec ChipWhisperer ~55€
- **Slide attack** : 2^32 known plaintexts requis
- **COPACOBANA** : 1-2 RF frames mais FPGA $10K
- **Kaiju PandwaRF** : "a couple RF frames" commercial non-public

**Aucune attaque pratique en pur RF < 100 trames sans hardware spécialisé**. La "8-10 trames" cru par le user vient probablement de confusion avec "10 power traces DPA" (=hardware attack).

## Voies alternatives

1. **Test empirique Chamberlain avec ESP32+CC1101** = **0€, matos existant** — **LA voie du prototype**
2. **DPA hardware** sur moteur récup + ChipWhisperer = ~75€ si Chamberlain infirmé
3. **Ouvrir moteur physique** pour identifier chip décodeur = 30 min + tournevis

## Références documentaires clés

- Datasheets Microchip : HCS301 (DS21143C), HCS500 (DS40000153E), HCS410 (DS40158E)
- Brevet Chamberlain "Secure self learning system" : US 5686904
- KeeLoq cryptanalysis Bogdanov 2007 : eprint IACR 2007/055
- Complete Break Kasper/Eisenbarth CRYPTO 2008 : eprint IACR 2008/058
- DEVMEL OpenAPI YAML : `/home/olivier/projects/profalux/reverse/devmel-emulator/doc/AirSendWebService.yaml`
- Flipper Zero keeloq_common.c : DarkFlippers/unleashed-firmware
- fake-airsend-box-v3.py : `/home/olivier/projects/profalux/reverse/fake-airsend-box-v3.py`
- Mémoire complète : `~/.claude/projects/-home-olivier/memory/profalux-keeloq-reverse-research.md`

## Leçons méthodologiques

Le user a corrigé mes conclusions à plusieurs reprises. À retenir :
1. "0 hits string = 0 fonctionnalité" est FAUX (=chercher patterns/structures binaires)
2. Numérologie ≠ preuve (=216 × 1024 = coïncidence pas structure)
3. Endpoints HTTP = accessoires, la LOGIQUE compte
4. La vraie référence technique = datasheet du chip, pas reverse endpoint
5. La MFG key n'est **pas nécessaire pour piloter** si le protocole permet enregistrement d'un nouvel émetteur
6. **Écouter tout de suite** quand le user dit "attention"

## Verdict final consolidé

**Ton ESP32+CC1101 peut probablement piloter tes 5 volets Profalux à ~0€ hardware** via reproduction du protocole Chamberlain-like. Le test empirique sur un volet (=chambre invitée) tranche définitivement en une session de 30 min.

---

## Mise à jour 2026-08-06 — formulation robuste post-contre-analyse

Après [contre-analyse « 10e homme »](CONTRE-ANALYSE.md), reformulation scientifique de la conclusion :

**Formulation antérieure (=trop forte)** :
> Le fait que PFX soit `clone=3` prouve que Profalux n'utilise pas un décodeur KeeLoq standard et que le moteur apprend une clé arbitraire sans manufacturer key.

**Formulation robuste** :
> Les observations DEVMEL démontrent une procédure PFX persistante spécifique, distincte du clonage KeeLoq générique. L'absence de clé ou de dérivation visible dans les couches applicatives analysées est compatible avec un apprentissage propriétaire sans manufacturer key, mais ne permet pas encore de distinguer ce scénario d'une capacité cryptographique cachée, préprovisionnée ou distante située dans une couche radio non analysée.

**6 hypothèses adverses restent compatibles** avec toutes les preuves accumulées :
- A (30%) — Capacité crypto dans firmware radio AirSend non extrait
- B (20%) — Identités PFX préprovisionnées côté BOX
- C (7%) — Provisionnement cloud
- D (15%) — Dérivation Profalux faible ou publique
- E (25%) — Séquence propriétaire de transfert de clé
- F (3%) — Cassage RF dynamique KEELOQ

Voir [PAIRING.md](PAIRING.md) pour le plan test empirique discriminant qui vise à trancher entre ces hypothèses.
