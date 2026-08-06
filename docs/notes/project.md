---
name: openprofalux-project
description: "Firmware ESP32+CC1101 pour piloter volets Profalux 868 MHz via Chamberlain Self-Learn. Repo Shad107/OpenProfalux privé, créé 2026-08-05 après session marathon reverse research 4h56."
metadata: 
  node_type: memory
  type: project
  originSessionId: a4851386-2bd1-4b16-95d2-d244bce36fa6
  modified: 2026-08-06T08:08:19.285Z
---

# OpenProfalux — firmware ESP32+CC1101

**Créé 2026-08-05 après session marathon reverse research** (=voir [[profalux-keeloq-reverse-research]] pour synthèse complète des découvertes qui ont établi l'hypothèse Chamberlain Self-Learn via DEVMEL PFX clone=3).

## Localisation

- **Dossier local** : `/home/olivier/projects/OpenProfalux/` (=**majuscule O et P**)
- **Repo GitHub** : `Shad107/OpenProfalux` (=**PRIVÉ**)
- **URL** : https://github.com/Shad107/OpenProfalux
- **License** : MIT
- **Identity commit** : `Shad107 <olshad@gmail.com>` (=alias GitHub perso, **pas** Olivier Delafosse ni isno.fr ni pro)
- **Co-author** : Claude Opus 4.7

## Objectif

Piloter les 5 volets Profalux MAI-EMPX-B1-NC 2009 depuis Home Assistant via un ESP32 + CC1101 sans dépendance cloud, sans sacrifier de télécommande, sans licence, sans MFG key partagée.

**Approche technique** : reproduire l'approche `clone=3 FULL` de DEVMEL AirSend Duo — appairage d'un nouvel émetteur random via la procédure d'apprentissage utilisateur du moteur (=variante Chamberlain Self-Learn brevet US 5686904).

## Architecture

```
OpenProfalux/
├── README.md, BUILD.md, LICENSE
├── Dockerfile.esp-idf, docker-compose.yml
├── docs/
│   ├── RESEARCH.md            # Synthèse session recherche 2026-08-05
│   └── API.md                 # Topics MQTT + workflow HA
├── firmware/                   # ESP-IDF v5.2.2 flashable
│   ├── CMakeLists.txt
│   ├── partitions.csv
│   ├── sdkconfig.defaults
│   └── main/
│       ├── CMakeLists.txt
│       ├── hardware_config.h  # Multi-target (SIM/External/M5Stack)
│       ├── keeloq.c/h         # Algo 528 rounds public
│       ├── profalux.c/h       # Frame build/parse + burst + NVS
│       ├── profalux_sim.c     # PC version pour debug local
│       ├── cc1101.c/h         # Driver SPI + TX OOK + RX skeleton
│       ├── wifi_bridge.c/h    # STA + SoftAP
│       ├── mqtt_bridge.c/h    # Client + HA discovery + logs
│       └── main.c             # Orchestration + handlers MQTT
└── sim/                        # PC simulation gcc
    ├── Makefile
    ├── sim_main.c
    ├── mock_cc1101.c, mock_nvs.c
    └── test_keeloq.c
```

## Topics MQTT

```
openprofalux/{device}/pair          [sub]  → trigger burst 60s
openprofalux/{device}/reset         [sub]  → regen state random
openprofalux/{device}/cmd           [sub]  → {"btn":"UP"|"STOP"|"DOWN"}
openprofalux/{device}/state         [pub]  → serial+counter+rssi
openprofalux/{device}/pair_result   [pub]  → success/timeout
openprofalux/listen/start           [sub]  → RX capture ON
openprofalux/listen/stop            [sub]  → RX capture OFF
openprofalux/listen/frame           [pub]  → chaque trame captured
openprofalux/log                    [pub]  → logs verbose
openprofalux/system/status          [pub, retained]
```

## Workflow appairage (=à tester empiriquement)

1. User publie `openprofalux/volet_test/pair` depuis HA
2. ESP32 démarre burst : émet 60 trames STOP avec (serial random, crypt_key random) sur 60s
3. **User AVANT expiration** : appuie bouton **PROG au dos de télécommande Profalux d'origine EMPX-B1** ~2-3 sec
4. Moteur aller-retour confirmation → mode learn actif
5. Moteur reçoit trames ESP32 → enregistre nouvelle télécommande
6. ESP32 publie résultat sur `openprofalux/volet_test/pair_result`

## Compilation + flash

```bash
cd ~/projects/OpenProfalux
docker compose build
docker compose run --rm esp-idf bash -c "cd /project/firmware && idf.py set-target esp32 && idf.py build"
docker compose run --rm esp-idf bash -c "cd /project/firmware && idf.py -p /dev/ttyUSB0 flash monitor"
```

## Simulation PC (=testable maintenant)

```bash
cd sim && make && ./sim_openprofalux reset
./test_keeloq  # vecteurs test KEELOQ
```

## Roadmap

- [x] **Phase 1** — Prototype ESP32 + CC1101 (=structure OpenXtraflame-like, fait)
- [ ] **Phase 2** — Test empirique appairage sur volet chambre invitée
- [ ] **Phase 3** — Full RX capture (=implémenter rx_task avec RMT peripheral ESP32)
- [ ] **Phase 4** — Web UI SoftAP config (=WiFi/MQTT au premier boot)
- [ ] **Phase 5** — Documentation publique + envisager passage public
- [ ] **Phase 6** — Extension X2D/X3D Delta Dore (=même famille clone=3 DEVMEL)

## Statut au 2026-08-05

- ✅ 31 fichiers, 2083 lignes commit initial pushé
- ✅ Simulation PC compile + run + KEELOQ vectors partiels (=3/5, 2 comparent à mauvaise fonction ref)
- ✅ Repo GitHub privé créé + push
- ⏸ Firmware ESP32 : main.c + wifi + mqtt + cc1101 skeleton, à compléter web_ui.c + finaliser cc1101 RX
- ⏸ Test empirique non réalisé (=CC1101 pas encore soudé chez user)

## Notes hardware

- Matos user existant : ESP32 (=débitmètre spare) + CC1101 + antenne 868 MHz (=confirmé mémoire ha-projet-volets-profalux)
- **Coût prototype : ~0€**
- Pinout dans `firmware/main/hardware_config.h` (=cible External ou M5Stack)

Voir [[profalux-keeloq-reverse-research]] pour la synthèse de recherche complète qui a abouti à ce projet.
Voir [[ha-projet-volets-profalux]] pour le contexte HA/domotique global des volets.

## Comment DEVMEL (et autres) font le clone=3 FULL

**Preuves accumulées session 2026-08-05** (=référence pour éviter re-recherche) :

### Mécanisme technique DEVMEL AirSend Duo → moteur Profalux

1. **Trigger user via HA** (=bouton "Pair" MQTT / web UI DEVMEL) → BOX AirSend démarre burst 60s
2. **BOX AirSend génère runtime** (=confirmé via `/dev/urandom` string + XXTEA transport) :
   - Un serial 28-bit random pour cette nouvelle "télécommande virtuelle"
   - Une crypt_key 64-bit random
   - Un rolling counter initial (=souvent 0)
3. **User met le moteur en mode learn** :
   - Trombone dans trou R sur BOX Profalux native (=AirSend adaptation)
   - OU bouton PROG au dos de télécommande d'origine 2-3 sec
   - OU switch P/N + STOP 5 sec (=procédure notice EMPX-B1)
   - Moteur fait aller-retour de confirmation = mode learn actif 60s
4. **BOX AirSend émet trames KEELOQ** avec (serial random, crypt_key random) sur 868.35 MHz OOK pendant fenêtre 60s
5. **Moteur enregistre** : stocke direct (serial, crypt_key) reçus dans son slot suivant (=12-16 slots max)
6. **Pas de dérivation via MFG key** — c'est la différence critique avec Normal Learn Microchip standard

### Pourquoi ça marche sans MFG key partagée

Profalux implémente une **variante propriétaire Chamberlain Self-Learn** (=brevet US 5686904 Chamberlain "Secure self learning system") :
- Décodeur moteur = probablement **MCU générique** (=PIC/AVR/MSP430) + firmware software custom
- Pendant fenêtre learn, moteur **accepte n'importe quelle** trame KEELOQ format valide
- Stocke direct sans essayer de dériver crypt_key via MFG key
- Anti-replay via rolling counter uniquement

### Preuves techniques accumulées

- **DEVMEL binary AirSendWebService** : 0 constante NLF KEELOQ (`0x3A5C742E`), 0 rotation ROR intensive, aucune fonction crypto KEELOQ
- **DEVMEL blob 221KB** : précomputations XXTEA + tables enum ThingEvent (=pas catalogue MFG keys, confirmé fork 3)
- **YAML OpenAPI DEVMEL** documente officiellement `clone: 0=none, 1=unavailable, 2=temporarly, 3=full`
- **Symbol C++ `DevmelSDK::content::Channel::isCloneable(uint16_t)`** confirme la fonction
- **PFX (Profalux, id 25455)** marqué `clone=3 FULL` vs **KLQ868 (générique)** marqué `clone=2 temporary`
- **Distinction PFX vs KLQ868** prouve que Profalux utilise **PAS** HCS500 Microchip standard (=sinon clone=2 seulement possible)

### Marques 868 MHz avec clone=3 FULL (=variantes Chamberlain-like)

Selon table interne DEVMEL :
- **PFX** (=Profalux) — cible OpenProfalux
- **X2D868** (=Delta Dore)
- **X3D** (=Delta Dore)

### Marques 868 MHz avec clone=2 temporary uniquement (=HCS5xx Microchip standard)

- Somfy SMO/RFY, BFT, Nice NSL, Bubendorff B12/B24, Gaposa GSA, DGL, DKT, V2, CRD, FDN, SLH, TLC, BID, **KLQ868 générique**

Pour ces marques : grabber-replay one-shot (=désynchronise TX d'origine, usage unique). Pas de full clone permanent possible sans MFG key.

## Hardware final validé 2026-08-06

**BOM commandée (=livraison vendredi 8 août)** :
- Lunette loupe Anpro 3.5X + LED (~20€ Amazon Prime)
- Régulateur DC step-down **Fasizi 4.75V-12V → 3.3V, 800mA** (=lot 10pcs, 5.99€) — **OBLIGATOIRE** car M5Stack ATOM Lite bottom header n'expose que 5V

**Matos existant** :
- M5Stack ATOM Lite ESP32-PICO-D4 (=1 des 3 du lot AliExpress 33.39€)
- EBYTE E07-900M10S + antenne TX915-FPC-8521 IPX
- Fils AWG 28-30, étain 0.5mm, fer, support 3ème main

**Total setup : ~26€** (=uniquement lunette + régulateur nouveaux).

## Pinout final M5Stack ATOM Lite → E07-900M10S

Câblage Dupont femelle-femelle sur bottom header ATOM Lite + soudure fils sur pads castellated E07 (=1.27mm pitch, loupe Anpro requise).

Voir [[../projects/OpenProfalux/docs/WIRING.md]] (=repo GitHub Shad107/OpenProfalux) pour :
- Table couleurs 8 fils avec pins E07 + pins M5Stack
- Schéma ASCII position naturelle (=écriture face + IPX bas-droite)
- Ordre soudure recommandé
- Test continuité multimètre
- Diagnostic PARTNUM=0x00 au boot

**⚠️ Régulateur 3.3V OBLIGATOIRE** : ATOM Lite n'expose que 5V bottom → E07/CC1101 supporte max 3.6V → sans régulateur, destruction immédiate.

## État session 2026-08-05→06

- ✅ Recherche 4h56 → hypothèse Chamberlain validée par convergence multi-source
- ✅ Firmware skeleton + simulation PC fonctionnelle
- ✅ Repo GitHub Shad107/OpenProfalux privé + push
- ✅ Doc WIRING.md complète M5Stack ATOM Lite
- ✅ BOM finalisée + hardware commandé
- ⏳ **Vendredi 8 août** : soudure + flash + test empirique appairage sur volet chambre invitée
- ⏳ Si Chamberlain validé empiriquement → généralisation aux 4 autres volets + Noé + docs publiques
