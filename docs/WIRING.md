# OpenProfalux — Wiring convention couleurs fils

Convention couleurs standard pour souder les 7 fils entre **EBYTE E07-900M10S** et **ESP32 DevKit**.

## Table câblage — Target M5Stack ATOM Lite (=Dupont-friendly bottom header)

| Signal | Couleur fil | Pin E07 | Rangée E07 | Pin M5Stack ATOM Lite | Rôle |
|--------|-------------|---------|------------|-----------------------|------|
| **VCC** | 🔴 **Rouge** | 9 | **HAUT** (=milieu) | **3.3V** (bottom) | Alimentation |
| **GND** | ⚫ **Noir** | 20 (ou 22) | **BAS** (=milieu-droit) | **GND** (bottom) | Masse |
| **MOSI** | 🟡 **Jaune** | 17 | **BAS** (=milieu) | **G23** (bottom) | SPI data out |
| **MISO** | 🟢 **Vert** | 16 | **BAS** (=milieu) | **G33** (bottom) | SPI data in |
| **SCK** | 🔵 **Bleu** | 18 | **BAS** (=milieu) | **G19** (bottom) | SPI clock |
| **CSN** | ⚪ **Blanc** | 19 | **BAS** (=milieu) | **G22** (bottom) | SPI chip select |
| **GDO0** | 🟠 **Orange** | 15 | **BAS** (=milieu) | **G25** (bottom) | Data async OOK |
| GDO2 (optionnel) | 🟣 Violet | 14 | **BAS** (=milieu-gauche) | **G21** (bottom) | Debug (=non requis) |

## Alternative — Target External (=ESP32 DevKit)

| Signal | Pin E07 | ESP32 DevKit |
|--------|---------|--------------|
| VCC | 9 | 3.3V |
| GND | 20 | GND |
| MOSI | 17 | GPIO23 |
| MISO | 16 | GPIO19 |
| SCK | 18 | GPIO18 |
| CSN | 19 | GPIO5 |
| GDO0 | 15 | GPIO4 |
| GDO2 | 14 | GPIO2 |

**Compile flag** :
- `CONFIG_OPENPROFALUX_TARGET_M5STACK=y` → M5Stack ATOM Lite (=production)
- `CONFIG_OPENPROFALUX_TARGET_EXTERNAL=y` → ESP32 DevKit (=prototype)

**Convention** : rouge/noir suit le standard électronique universel. Signaux SPI ordre visuel type Ethernet (=jaune/vert/bleu/blanc pour paires).

## Schéma câblage avec numéros + couleurs

Position naturelle : **écriture face + IPX bas-droite**.

```
              ── rangée HAUT (=gauche à droite) ──
       ┌──────────────────────────────────────────────────────┐
       │  ●   ●   ●   ●   ●   ●   ●   ●   ●   ●   ●          │
       │ 11  10   9   8   7   6   5   4   3   2   1          │
       │      —  🔴  —   —   —   —   —   —   —   —          │
       │         V                                           │
       │         C                                           │
       │         C                                           │
       │                                                     │
       │            E07 900M10S                              │
       │            QR • FCC • CE                            │
       │                                                     │
       │                                        ⓘ IPX        │
       │                                                     │
       │  ●   ●   ●   ●   ●   ●   ●   ●   ●   ●   ●          │
       │ 12  13  14  15  16  17  18  19  20  21  22          │
       │      —  🟣  🟠  🟢  🟡  🔵  ⚪  ⚫   —   —          │
       │              G   G   M   M   S   C   G              │
       │              D   D   I   O   C   S   N              │
       │              O   O   S   S   K   N   D              │
       │              2   0   O                              │
       └──────────────────────────────────────────────────────┘
              ── rangée BAS (=gauche à droite) ──

           ↓ Correspondance fils vers ESP32

    Pin 9  → 🔴 Rouge  → VCC   → ESP32 3.3V
    Pin 20 → ⚫ Noir   → GND   → ESP32 GND
    Pin 19 → ⚪ Blanc  → CSN   → ESP32 GPIO5
    Pin 18 → 🔵 Bleu   → SCK   → ESP32 GPIO18
    Pin 17 → 🟡 Jaune  → MOSI  → ESP32 GPIO23
    Pin 16 → 🟢 Vert   → MISO  → ESP32 GPIO19
    Pin 15 → 🟠 Orange → GDO0  → ESP32 GPIO4
    Pin 14 → 🟣 Violet → GDO2  → ESP32 GPIO2 (=optionnel)
```

## Ordre soudure recommandé (=du plus critique au moins)

1. **🔴 Rouge (VCC pin 9)** — priorité alim
2. **⚫ Noir (GND pin 20)** — priorité alim
3. **⚪ Blanc (CSN pin 19)** — SPI critique
4. **🔵 Bleu (SCK pin 18)** — SPI critique
5. **🟡 Jaune (MOSI pin 17)** — SPI critique
6. **🟢 Vert (MISO pin 16)** — SPI critique
7. **🟠 Orange (GDO0 pin 15)** — data KEELOQ critique
8. 🟣 Violet (GDO2 pin 14) — optionnel debug

## Notes soudure

- **Loupe Anpro 3.5X + LED** conseillée pour SMD 1.27mm
- **Support 3ème main** pour fixer le module
- **Étamer chaque pad** avec une micro-goutte avant d'y poser le fil
- **Fils AWG 28-30** courts (=~10cm) pré-étamés
- **Colle chaude** ou epoxy après soudure pour robustesse mécanique (=éviter arrachage)
- **Ne pas connecter GDO2** au premier essai — inutile pour fonctionnement de base
- **Longueur fils** : garder ~10cm max, sinon parasites SPI possibles à haut débit

## Test continuité après soudure

Avec multimètre en mode continuité :
- Vérifier 🔴 VCC contre 3.3V ESP32 (=bip)
- Vérifier ⚫ GND contre GND ESP32
- Vérifier chaque signal SPI (=bip entre pin E07 et GPIO ESP32 correspondante)
- **Aucun court-circuit** entre 2 signaux (=silence multimètre entre GPIO adjacents)

## Vérification firmware

Après flash + boot, log série doit afficher :
```
[cc1101] PARTNUM=0x00 (=expected 0x00 for CC1101)
```

Si `PARTNUM != 0x00` ou fail → problème câblage SPI (=vérifier MISO/MOSI inversés, CSN mal soudé, GND commun manquant).
