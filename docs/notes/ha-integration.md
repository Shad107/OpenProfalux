---
name: ha-projet-volets-profalux
description: "Projet DIY pilotage 5 volets Profalux 868 MHz RF (pré-Zigbee) depuis HA via ESP32 + CC1101 868 MHz, firmware l0ad/profalux2Esphome"
metadata: 
  node_type: memory
  type: project
  originSessionId: 11adade3-cd2b-486f-8ddd-8288ea2a992c
  modified: 2026-08-06T08:09:13.352Z
---

## ⚠️ MISE À JOUR 2026-08-06 — APPROCHE PRIMAIRE CHANGÉE

**Depuis 2026-08-05 après session marathon reverse (~5h)**, l'approche primaire n'est **plus** le "Plan A sacrifice Noé + PCB DREVET" décrit plus bas.

**Nouvelle approche = [[openprofalux-project]] — firmware ESP32 OpenProfalux** :
- **Repo GitHub Shad107/OpenProfalux** (=privé)
- **Principe** : reproduire l'approche DEVMEL AirSend `clone=3 FULL` (=documenté OpenAPI officielle) qui exploite la procédure d'apprentissage utilisateur Profalux sans nécessiter la MFG key
- **Hypothèse validée par convergence** : Profalux implémente une variante Chamberlain Self-Learn (=US 5686904) où le moteur accepte n'importe quel nouvel émetteur pendant la fenêtre 60s learn
- **Matos** : M5Stack ATOM Lite ESP32-PICO-D4 + EBYTE E07-900M10S + antenne TX915 IPX + régulateur AMS1117-3.3V (=~26€ nouveau + matos existant)
- **Aucun sacrifice** de la Noé nécessaire — elle reste utilisable
- **Test empirique prévu vendredi 8 août 2026** sur volet chambre invitée

Voir [[openprofalux-project]] pour :
- Preuves techniques accumulées (=DEVMEL disasm, blob 221KB analyse, HCS500 datasheet)
- Mécanisme exact "comment DEVMEL et autres font"
- Hardware BOM final + pinout M5Stack + WIRING.md doc
- Roadmap phase 1-6

Voir [[profalux-keeloq-reverse-research]] sections 11-18 pour la synthèse recherche 4h56 qui a établi Chamberlain Self-Learn.

**Le contenu ci-dessous (=juin 2026) reste comme référence historique** (=fallback si OpenProfalux Chamberlain échoue empiriquement → retour au Plan A sacrifice Noé + PCB DREVET).

---

Projet à monter en même temps que le débitmètre eau, achat fin juin 2026, install été 2026. Piloter les 5 volets Profalux depuis HA pour automatisations (fermeture coucher de soleil, ouverture lever, mode canicule, fermeture si pic PV pour ombrage, etc.).

**Why:** Olivier a 5 volets Profalux installés **avant la génération Zigbee** (= protocole RF 868 MHz Profalux propriétaire, pas Somfy RTS). Télécommandes individuelles ✅ (1 par volet, avec bouton PROG au dos) + une **Noé PX+NO multicanaux** (référence laboutiqueduvolet.com/4634-emetteur-multicanaux-noe-px+no.html). Le DIY à 15-20€ est ultra-rentable vs RFXtrx433XL à 130€ pour seulement 5 volets, et reste 100% local.

**⚠️ INFOS HARDWARE PRÉCISES découvertes 2026-06-28** :
- Télécommande individuelle = **MAI-EMPX-B1-NC** datée **20/04/2009**
- Noé multi-channel = **MAI-EMNOE** (= confirmé 8 canaux + 8 horloges via SAV Profalux, ref pieces-detachees-profalux.com)
- **Volets installés ~2010** (= probable, à confirmer via étiquettes lames format "C+chiffres" où 2 premiers = mois et 2 suivants = année)
- **Pré-cutoff janvier 2012** = officiellement NON-compatibles Calyps'HOME box + dongle 868 selon doc Profalux Pro 2024 (https://www.profalux-pro.com/wp-content/uploads/2024/02/Tableau-des-equivalences...)
  - Implication : AirSend Duo + Calyps'HOME = risque compat NON-officielle pour Olivier
  - Mais protocole RF KEELOQ HCS301 inchangé depuis 2007 (= les EMPX-B1 + Noé pilotent ses motors prouvent que ça marche entre télécommandes)
  - Le cutoff 2012 concerne probablement firmware moteur pour acceptation dongle, pas le protocole RF de base
  
**🔑 SWITCH P/N CONFIRMÉ SUR LES 2 TYPES DE TÉLÉCOMMANDES** :
- EMPX-B1 individuelle : switch P/N derrière (= cache amovible), trombone pour basculer
- Noé : MÊME switch P/N sous un cache (= confirmé 2026-06-28 par Olivier)
- Position **N = Normal** (= mode utilisation, défaut sortie usine)
- Position **P = PROG** (= mode appairage nouveau remote)
- Procédure documentée notice EMPX-B1 (https://static.1001telecommandes.com/documents/1001-notice-profalux-empxb1.pdf) :
  1. EXISTING remote : volet à 5cm du haut
  2. NEW remote : appuie STOP + switch P pendant 5 sec
  3. Switch back to N, relâche STOP
  4. Dans 1 minute, EXISTING remote séquence : Montée → Descente 3s → STOP → Montée
  5. Volet fait aller-retour = appairé ✅

= **Protocole d'appairage UNIFORME Profalux depuis 2007** (= aucune génération avec PROG différent)
= L'appairage utilise le RF + dance avec existing remote, PAS bouton PROG sur le moteur
= Cette dance suggère DEVMEL et l'approche DIY peuvent fonctionner SANS manufacturer key (= 50-60% probable)

**Implications protocole pour Plan D ESP32+CC1101** :
Le ESP32 doit émuler la séquence "switch to P" puis "STOP 5 sec en P" puis "switch back to N" en émettant les RF correspondantes. Le receiver (= motor) attend ensuite la dance d'autorisation depuis un EXISTING remote. Si motor accepte l'ESP32 comme nouveau remote, il stocke (serial, key) générés aléatoirement par l'ESP32 au boot. Plus besoin de manufacturer key Profalux. Sécurité Profalux reposerait alors sur l'accès physique à un EXISTING remote (= dance authorisation), pas sur secret crypto manufacturer.

**À tester empiriquement été 2026** : si l'ESP32 émule fidèlement le protocole P→STOP→N et la séquence d'announcement, et que la dance authorise, il devient un remote pairé. **Si succès = premier reverse Plan D Profalux public, économise sacrifice Noé**.

Voir aussi [[profalux-keeloq-reverse-research.md]] pour la recherche reverse complète.

**⚠️ CORRECTION MAJEURE 2026-06-27 — vérification README l0ad** :

L'approche initialement documentée (= ESP32 + CC1101 émettant le RF + pairing PROG button) est **FAUSSE** pour ce projet. L'approche réelle de l0ad/profalux2Esphome est très différente :

- **Replay des trames RF = IMPOSSIBLE** car HCS301 utilise KEELOQ (= rolling code avec clés constructeur secrètes Profalux non accessibles)
- **Sniff puis émission par CC1101 = NE MARCHE PAS** car même sans clés tu peux pas générer un counter valide
- **Pairing via PROG button + émission RF par ESP = NE MARCHE PAS** car tu n'as pas la clé KEELOQ unique d'une télécommande Profalux

**Vraie approche l0ad** : Acheter une télécommande Profalux MAI-EMPX-B1 **supplémentaire spare** (= ~35-70€ neuve, ~17-35€ refurb), souder des fils sur les contacts des boutons UP/DOWN/STOP de son PCB, ESP relie ces fils en GPIO open-drain (vers GND quand "press"). L'ESP émule physiquement les pressions de boutons. La télécommande EMPX-B1 elle-même émet le RF avec son rolling code HCS301 légitime. Le bouton reste pressable physiquement par un humain en parallèle (= mode "configuration 2" du README).

**Setup HYBRIDE optimal (= correction 2026-06-27 après recherche tiers/leak)** :
- ✅ **EBYTE E07-900M10S + antenne TX915-FPC-8521** = UTILE pour le côté RX (sniff/sync HA bidirectionnel). Mode listen 868.35 MHz (= fréquence exacte confirmée) pour décoder trames Noé + télécommandes individuelles via OOK/ASK + PWM, 66-bit code word. Quand utilisées physiquement, ESP détecte → HA met à jour cover.volet_X = sync parfaite.
- **Recherche reverse Profalux 2026-06-27** : aucune fuite KEELOQ manufacturer key, aucun decoder rtl_433/URH/Flipper, aucune attaque crypto spécifique Profalux publiée. Pas de plugin Jeedom 868 terminé. RFPlayer/RFXtrx868XL échouent sur rolling code. Profalux verrouillé par HCS301 + KEELOQ + Secure Learn = écosystème reverse complètement vide (contrairement à Somfy RTS qui a 10+ implementations car protocole différent). CC1101 ne peut donc PAS générer trames TX valides.
- **Pour le TX, 2 voies maintenant** :
  1. **l0ad/profalux2Esphome (= soudure pads boutons)** : approche classique, fil sur boutons UP/DOWN/STOP du PCB d'une télécommande, GPIO ESP open-drain
  2. **PCB Florian DREVET avec optoisolateurs** ([HA thread 658133](https://community.home-assistant.io/t/profalux-868mhz-mai-empx-b1-integration/658133)) : plus propre, sans soudure directe, **1 PCB pilote 4 volets via 1 SEULE télécommande**, réversible (= récupération télécommande possible)
- **Astuce ÉCONOMIE** : Olivier a 6 télécommandes pour 5 volets (= 5 EMPX-B1 + 1 Noé multi-channel). Sacrifier la **Noé multi-channel** pour le PCB DREVET = pilote 4-5 volets sans achat de spare. Économie ~35-70€. La Noé est appairée à tous les volets par défaut = idéal pour pilotage groupé via 1 PCB.
- Alternative non-DIY commerciale qui marche : **AirSend Duo** (DEVMEL) — listée mais non investiguée.
- ✅ **M5Stack ATOM Lite** = TOUJOURS UTILE pour ce projet (= il pilote les boutons de l'EMPX-B1 spare)
- ✅ **Pin headers + Dupont F-F + USB-C cable + boîtier Plexo** = TOUJOURS UTILES (= boîtier doit englober remote + ESP)
- ➕ **À AJOUTER** : 1 télécommande Profalux MAI-EMPX-B1 spare (~35-70€). Cherche sur leboncoin / 1001telecommandes refurb. Peut aussi être commandé chez installateur Profalux.

**Mise à jour BOM total** : 35-55€ au lieu de 15-20€ estimé. Reste très rentable vs RFXtrx433XL 130€ ou Profalux Connect 150-200€.

---

## PLAN FINAL VALIDÉ 2026-06-27 — setup hybride avec Noé 8 canaux sacrifiée

**Découverte cruciale** : Noé Profalux confirmée = **8 canaux + 8 horloges** (source officielle profalux-pieces-detachees.com, ref produit 32-telecommande-generale-avec-horloge-noe-profalux, prix neuf 208.56€ TTC). 5 canaux suffisent pour 5 volets + 3 spare pour expansion future.

### BOM total final (~10-15€ supplémentaire à commander)

Déjà commandé / chez Olivier (= utilisable, aucun achat) :
- ✅ M5Stack ATOM Lite (= 1 unité du lot de 3, mutualisé avec [[ha-projet-debitmetre-eau]])
- ✅ E07-900M10S CC1101 wideband 855-925 MHz (= utilisé en mode RX uniquement)
- ✅ Antenne TX915-FPC-8521 wideband 860-940 MHz IPEX (= couvre 868 MHz Profalux)
- ✅ Noé 8 canaux Profalux (= déjà chez Olivier, sera sacrifiée pour TX)
- ✅ Pin headers 2.54mm + Dupont F-F + USB-C cable + boîtier Plexo 105×105×55

À ajouter prochaine commande AliExpress (~10-15€) :
- ➕ **1× MCP23017 module breakout** (~2€) — I2C GPIO expander 16 lignes, nécessaire car ATOM Lite a ~6 GPIOs vs 15 inputs Noé à piloter
- ➕ **10× optocoupleurs PC817** (~2€) — isolation galvanique ESP ↔ Noé, approche DREVET, réversible
- ➕ **Lot résistances 1kΩ + 220Ω** (~1€ si pas en stock) — pour limitation courant LED des PC817
- ➕ **Fil souple AWG 28-30** (~3€) — pour soudure sur pads boutons Noé (= fil rigide casse les pads)
- ➕ **Étain 0.5mm** (~2€) si stock perso = 1mm trop gros pour pads SMD Noé
- ➕ **Loupe / panavise** (~5€) optionnel mais conseillé pour soudure SMD

### Architecture câblage complète

**Niveau 1 — ESP32 ATOM Lite ↔ CC1101 E07-900M10S (= RX 868 MHz)**

8 fils SPI + alim 3.3V STRICTEMENT (= 5V tue le module) :

| Pad E07-900M10S | Pin ATOM Lite | Fonction |
|---|---|---|
| VCC | **3V3** (PAS 5V !) | Alim 1.8-3.6V uniquement |
| GND | GND | Masse commune |
| MOSI | G19 | SPI Master Out Slave In |
| MISO | G22 | SPI Master In Slave Out |
| SCK | G21 | SPI Clock |
| CSn | G33 | SPI Chip Select |
| GDO0 | G25 | Interrupt RX (= trame reçue) |
| GDO2 | (non utilisé) | optionnel |

Antenne TX915-FPC-8521 → clipse direct sur connecteur IPEX du E07, pas de soudure coax.

**Niveau 2 — ESP32 ATOM Lite ↔ MCP23017 (= I2C extender)**

2 fils I2C + alim 3.3V + 2 résistances pull-up 4.7kΩ entre SDA/SCL et 3.3V :

| Pin MCP23017 | Pin ATOM Lite | Fonction |
|---|---|---|
| VDD | 3V3 | Alim 1.8-5.5V (3.3V OK) |
| VSS | GND | Masse commune |
| SDA | G26 (= Grove yellow) | I2C Data |
| SCL | G32 (= Grove white) | I2C Clock |
| A0, A1, A2 | GND | Adresse I2C = 0x20 (= par défaut) |
| RESET | 3V3 | Pull-up actif haut |

MCP23017 expose 16 GPIOs (= GPA0-7 + GPB0-7) → on en utilise 15 pour boutons Noé.

**Niveau 3 — MCP23017 ↔ Optocoupleurs PC817 (= 1 par bouton Noé)**

Pour CHAQUE bouton Noé à piloter (= 15 boutons = 5 canaux × 3 Up/Stop/Down) :

```
MCP23017 GPIO ─→ [résistance 1kΩ] ─→ Anode LED PC817 (pin 1)
                                       │
                                    Cathode LED (pin 2) ─→ GND ESP32
                                    
PC817 côté transistor :
  Collecteur (pin 4) ─→ pad bouton Noé "signal"
  Émetteur (pin 3) ─→ pad bouton Noé "GND" (= masse Noé)
```

Quand MCP23017 met sa sortie à HIGH (3.3V) :
- Courant traverse LED interne PC817 (= ~3 mA via résistance 1kΩ)
- LED illumine phototransistor interne
- Phototransistor conduit = équivalent "bouton pressé"
- Noé voit son bouton actif → émet trame KEELOQ légitime

Avantages PC817 :
- Isolation galvanique 5kV ESP ↔ Noé (= zéro risque feedback)
- Réversibilité : on peut retirer les fils des pads boutons Noé un jour
- Approche éprouvée DREVET / l0ad

**Niveau 4 — Soudure sur PCB Noé**

Démontage Noé (= clip ou vis, regarder fournisseur prix neuf 208€ donc précieux) :
1. Identifier les 8 canaux (= 8 paires de boutons Up/Down ou 8 groupes Up/Stop/Down selon modèle)
2. Pour chaque bouton à piloter, identifier au multimètre :
   - Le pad qui va au GND PCB (= masse pile)
   - Le pad qui va au chip principal Noé (= signal d'entrée détection bouton)
3. Souder un fil AWG 28-30 sur le pad SIGNAL de chaque bouton (= pas le pad GND)
4. Souder UN fil GND commun (= 1 seul fil GND total, partagé tous les optos PC817)
5. Gainer chaque soudure (= colle chaude ou silicone) pour éviter court-circuit avec voisins
6. Faire sortir les fils du boîtier Noé via un trou propre (= drill 3mm)
7. Remonter le boîtier Noé partiellement (= laisser accessible pile et boutons physiques)

⚠️ Précautions soudure SMD :
- Fer pointe fine OBLIGATOIRE (= 0.4mm idéal)
- Soudure rapide <2s par pad (= sinon décolle pad de piste = bouton mort)
- Loupe + lampe forte
- Étamer le bout du fil AVANT soudure pour gagner du temps sur le pad
- Si pad arraché lors de soudure = grattage piste 2-3mm + soudure sur piste

**Niveau 5 — Mapping ESPHome final**

15 inputs Noé pilotables → 5 volets contrôlables individuellement :

| MCP23017 GPIO | Bouton Noé | Volet HA | Action |
|---|---|---|---|
| GPA0 | Canal 1 Up | cover.volet_1 | Open |
| GPA1 | Canal 1 Stop | cover.volet_1 | Stop |
| GPA2 | Canal 1 Down | cover.volet_1 | Close |
| GPA3 | Canal 2 Up | cover.volet_2 | Open |
| GPA4 | Canal 2 Stop | cover.volet_2 | Stop |
| GPA5 | Canal 2 Down | cover.volet_2 | Close |
| GPA6 | Canal 3 Up | cover.volet_3 | Open |
| GPA7 | Canal 3 Stop | cover.volet_3 | Stop |
| GPB0 | Canal 3 Down | cover.volet_3 | Close |
| GPB1 | Canal 4 Up | cover.volet_4 | Open |
| GPB2 | Canal 4 Stop | cover.volet_4 | Stop |
| GPB3 | Canal 4 Down | cover.volet_4 | Close |
| GPB4 | Canal 5 Up | cover.volet_5 | Open |
| GPB5 | Canal 5 Stop | cover.volet_5 | Stop |
| GPB6 | Canal 5 Down | cover.volet_5 | Close |
| GPB7 | (spare) | - | (= canal 6/7/8 si expansion future) |

### Procédure d'appairage (~30 min total pour 5 volets)

Les 5 volets sont déjà appairés aux 5 canaux de la Noé chez Olivier (= installateur d'origine). Pas de PROG button nécessaire si Noé est déjà appairée. Si jamais besoin :
1. Maintenir bouton PROG au dos d'une télécommande individuelle EMPX-B1 du volet ~2-3 sec
2. Le moteur fait un bref aller-retour (= mode apprentissage)
3. Presser le bouton du canal Noé visé (= via HA pour automatiser)
4. Moteur refait un aller-retour = appairé
5. Noé fonctionne en parallèle avec les EMPX-B1 perso (= moteur stocke 12-16 remotes max)

### Workflow assembly final (~4-5h)

1. Démontage Noé (5 min)
2. Identification pads boutons au multimètre (15 min)
3. Soudure 15 fils signal + 1 fil GND sur Noé (~1h30)
4. Sortie fils par trou Plexo + gaine thermo (15 min)
5. Câblage MCP23017 ↔ ATOM Lite I2C (5 min)
6. Soudage PC817 sur perfboard (~30 min)
7. Câblage MCP23017 ↔ PC817 ↔ fils Noé (30 min)
8. Câblage CC1101 ↔ ATOM Lite SPI (~10 min, déjà testé via Dupont)
9. Flash ESPHome custom (= base l0ad + driver MCP23017 + RX CC1101 custom) (~30 min)
10. Test 5 volets via HA (~30 min)
11. Mise en boîtier Plexo final (~30 min)
12. Installation garage sous escalier avec antenne sortante verticale (~15 min)

### Côté RX CC1101 (= sync HA quand télécommandes perso utilisées)

ESPHome config (= à compléter au flash) :
```yaml
# Listen mode 868.35 MHz OOK
spi:
  clk_pin: G21
  mosi_pin: G19
  miso_pin: G22

cc1101_rx:
  cs_pin: G33
  gdo0_pin: G25
  frequency: 868350000
  modulation: OOK
  bit_rate: 1538  # ~650 µs/bit

# Identification 5 remotes Olivier (= à découvrir au flash via mode debug)
profalux_rx_remotes:
  - serial: 0x???????  # remote chambre1 - capturer au flash
    cover: volet_chambre1
  - serial: 0x???????  # remote chambre2
    cover: volet_chambre2
  - serial: 0x???????  # remote salon
    cover: volet_salon
  - serial: 0x???????  # remote cuisine
    cover: volet_cuisine
  - serial: 0x???????  # remote 5eme volet
    cover: volet_5
```

Tableau serial à remplir au moment du flash via mode debug ESPHome (= log toutes les trames RX). Push chaque télécommande 1 fois → noter le serial 28-bit affiché → mapper à son volet → mettre à jour le YAML → reflash.

### Sécurités projet

- Counter ESP propre maintenu par firmware (= si emulation TX, mais ici TX = Noé = pas concerné)
- ⚠️ Re-flash USB en effaçant tout = perd config RX (= devra recapturer serials)
- Préférer toujours OTA ESPHome pour updates
- Backup counter Noé dans HA pour info (= via attribut)
- 5 télécommandes perso EMPX-B1 RESTENT FONCTIONNELLES (= aucun impact)
- Famille peut continuer d'utiliser les EMPX-B1 = HA détecte via RX et sync l'état

**BOM finale révisée 2026-06-16 (~30-35€)** :
- **EBYTE E07-900M10S CC1101 868 MHz + antenne JKS whip 200mm IPX** kit AliExpress à **4.29€** ([listing 5.0/5 sur 1183 ventes](https://a.aliexpress.com/_EyVc86Q), livraison juillet 02-09) — remplace OCKULT (~10-15€), économie ~10€. Module a **IPEX connector ONBOARD** + stamp holes en alternative → antenne JKS se **clipse directement** sur l'IPEX, zéro soudure côté antenne. Spec : 1.8-3.6V uniquement, NE PAS brancher sur 5V ! **Confirmation specs 2026-06-27** : module E07-900M10S = wideband 855-925 MHz (= couvre 868 EU + 915 US). Antenne commandée variant `TX915-FPC-8521` = wideband 860-940 MHz (= malgré "915" dans nom, couvre 868 OK). Setup compatible Profalux 868 MHz. Performance ~85-90% à 868 MHz (= bord bas plage). ⚠️ NE PAS commander variant `TX915-JKS-IPX20` qui lui est narrow 915 MHz uniquement.
- **M5Stack ATOM Lite** — utilise 1 unité du **lot de 3 acheté à 33.39€ sur AliExpress** (mutualisé avec [[ha-projet-debitmetre-eau]], ~11€/unité au lieu de 14.90€ Amazon)
- Pack pin headers mâles 2.54 mm 40 broches (~3€) — à souder sur les pads SMD de l'ATOM Lite pour pouvoir brancher des Dupont en démontable
- **Pas d'achat Dupont supplémentaire** — utilise les Dupont F-F déjà en stock perso. Pour câbler le E07 : couper 4 Dupont F-F en deux → 8 demi-fils avec embout F crimpé + bout dénudé à souder direct sur les 8 pads SPI du E07 (méthode jonction soudée pure)
- **Pas d'achat fil 30 AWG supplémentaire** non plus — le fil 22-26 AWG des Dupont coupées suffit pour les pads stamp-hole E07 (qui font ~1mm)
- Câble USB-A → USB-C 1 m (~3€) — alim depuis PC nearby
- Boîtier Plexo 092041 105×105×55 mm (~10€) — plus petit que celui du débitmètre eau
- Téflon, Wagos, presse-étoupes : réutilisation du stock projet débitmètre eau (0€)
- Pas de 220V → USB module : alim via USB direct depuis PC du garage (PC toujours allumé, sinon problème EasySolar 2 GX en amont qui demande intervention de toute façon)

**Câblage E07-900M10S ↔ ATOM Lite** (8 fils SPI, ⚠️ VCC sur 3.3V pas 5V) :

| Pad E07 (stamp-hole) | Pin ATOM Lite | Note |
|---|---|---|
| VCC | **3V3** (PAS 5V !) | Module 1.8-3.6V, mort sur 5V |
| GND | GND | |
| MOSI | G19 | SPI data out |
| MISO | G22 | SPI data in |
| SCK | G21 | SPI clock |
| CSn | G33 | SPI chip select |
| GDO0 | G25 | Interrupt RX/TX |
| GDO2 | (non utilisé) | optionnel |

**Antenne** : la JKS 200mm se clipse direct sur le connecteur **IPEX** du module — pas de soudure coax nécessaire. Sort par presse-étoupe du Plexo, pointe verticale vers le haut.

**Workflow soudure (~25 min total)** :
1. Souder 1 pin header 2.54mm 1×5 ou 1×6 sur les pads SMD de l'ATOM Lite (2 min, pads larges)
2. Couper 4 Dupont F-F en deux → 8 demi-fils
3. Dénuder 3 mm à l'extrémité coupée + étamer
4. Souder le bout étamé sur les 8 pads stamp-hole du E07 (~15 min, pads ~1mm, accessibles)
5. Embouts F côté ATOM s'enfilent sur les pin headers fraîchement soudés
6. Clipser antenne JKS sur IPEX du E07
7. Tester en USB depuis PC garage AVANT de fixer le boîtier (commander 1 volet, vérifier le RSSI)

**Emplacement prévu chez Olivier** : garage sous l'escalier, en hauteur. Pas idéalement central, plusieurs murs à traverser pour atteindre tous les volets. **Murs = parpaing creux + BA13 à 100%** (pas de béton armé), donc atténuation très favorable (~3-5 dB/mur parpaing creux, ~1-2 dB/mur BA13). Sur 4 murs typiques = ~10-15 dB de perte totale, budget RF largement OK. Donc :
- ⚠️ **Antenne fil 1/4 onde 8.2 cm OBLIGATOIRE** soudée sur le pad ANT du CC1101 (remplace le ressort SMD fourni) — gain ~10-15 dB vs ressort, double facilement la portée à travers murs
- Antenne **doit sortir physiquement** du boîtier Plexo, pointe vers le haut ou horizontale vers le volet le plus distant. Jamais repliée à l'intérieur ou contre une masse métallique
- Loin du chauffe-eau, tableau élec, structure métal d'escalier
- À tester en USB portable AVANT fixation définitive : se balader dans le garage avec, commander le volet le plus distant, choisir le point réel où ça passe
- Plan B si pas assez : 2ème ESP "esclave" avec mêmes IDs (rolling code partagé). 15€ supplémentaires. Probablement inutile avec antenne 1/4 onde bien placée.

**Câblage CC1101 ↔ M5Stack ATOM Lite** (à valider avec doc l0ad au moment du flash) :
```
CC1101         ATOM Lite
─────────────────────────
VCC      →     3V3 pad
GND      →     GND
CSN/CS   →     GPIO 25
SCK      →     GPIO 23
MOSI     →     GPIO 19
MISO     →     GPIO 22
GDO0     →     GPIO 26 (= Grove yellow)
GDO2     →     optionnel, peut sauter
```

**Firmware** : [l0ad/profalux2Esphome](https://github.com/l0ad/profalux2Esphome) — actif (dernier commit 2026-06-08, 1 semaine avant cette mémoire). Composant custom ESPHome pour Profalux 868 MHz, gère le protocole proprement.

**Procédure d'appairage (~5 min/volet, ~30 min total)** :
1. Prendre la télécommande individuelle du volet
2. Maintenir le bouton **PROG au dos** ~2-3 sec → le moteur fait un bref aller-retour (mode apprentissage)
3. Depuis HA / ESPHome, déclencher la commande "register" pour ce channel
4. Le moteur refait un aller-retour = appairé. L'ESP32 est mémorisé comme nouvelle télécommande (les moteurs Profalux mémorisent généralement 12-16 télécommandes, donc largement la place)
5. Répéter pour les 5 volets. La Noé et les télécommandes individuelles continuent de marcher en parallèle, aucun conflit.

**Rolling code (= "code tournant") — points critiques à connaître** :
- Chaque télécommande (= individual + Noé multi-channel) a un **ID unique** (24-32 bits) + un **compteur incrémenté** à chaque émission (16-24 bits)
- Le moteur stocke `(ID, dernier compteur vu)` par remote appairée et refuse les compteurs inférieurs (anti-replay)
- Tolérance fenêtre : [dernier+1, dernier+128] environ. Au-delà : remote doit être ré-appairée.
- **L'ESP32 maintient son propre compteur dans la flash NVS** — survit aux reboots et OTA ESPHome
- ⚠️ **Re-flash USB en effaçant tout = perte compteur = re-appairage forcé des 5 volets**. Toujours utiliser OTA ESPHome pour les updates, jamais erase complet.
- Usure flash : ~100 commandes/jour × 10 ans = 365k cycles, dans les specs (flash supporte ~100k/cellule + wear leveling), pas un souci pratique.

**Backup + restore counter ESP32 (= protection panne)** :
- Le firmware l0ad expose le compteur en attribut HA (`sensor.volet_X_counter`)
- **À mettre en place** : logger ce compteur en InfluxDB ou en `input_number` HA pour récupération
- En cas de panne ESP catastrophique : restore via service `cover.profalux.set_counter` avec valeur backupée + marge sécurité (+10)
- Sinon : forcé re-pairing des 5 volets (= 30 min)

**Capture des codes de la Noé multi-channel (= sync HA avec usage physique)** :

Le firmware l0ad/profalux2Esphome supporte un **mode RX/sniffer** qui écoute les trames 868 MHz et les loggue. Utile pour :
1. **Sync HA avec actions Noé** : quand famille utilise Noé physique, HA détecte → état `cover.volet_X` mis à jour
2. **Backup couterunes des télécommandes existantes** : log compteurs Noé + 5 remotes individuelles
3. **Debug réception** : vérifier RSSI, détecter interférences

**Procédure capture** :
1. Configurer ESPHome avec mode `profalux_rx` activé (= cf doc l0ad section "listen mode")
2. Au démarrage, ESP32 écoute en permanence sur 868.3 MHz
3. Quand une trame est reçue (= utilisation Noé ou télécommande perso) :
   - Log dans HA : `sensor.profalux_rx_last_frame` avec ID + counter + command
   - Match avec table `(ID, channel)` connue pour identifier la remote
   - Trigger automation HA : mise à jour `cover.volet_X` selon command détectée
4. Pour la **Noé multichannel** (= remote séparée par bouton/canal) :
   - Chaque canal envoie avec un **ID dérivé du master ID Noé** (= scheme propriétaire Profalux)
   - Au premier capture par canal, logger le mapping `(channel button → ID dérivé, volet cible)`
   - Stocker dans `input_text.noe_remote_mapping` pour mémoriser

**Pour récupérer les IDs initiaux des télécommandes existantes (= avant pairing ESP32)** :
1. Activer mode RX ESPHome avec verbose logging
2. Appuyer une fois sur chaque télécommande (= individual 1 à 5 + tous les canaux Noé)
3. Chaque trame loggée donne : `ID=0x123456, counter=0x789A, channel=N, command=open/close/stop`
4. Noter dans un tableau de référence (= ci-dessous, à compléter au flash) :

| Remote | ID (hex) | Counter initial | Volet ciblé |
|---|---|---|---|
| Telecommande 1 (= chambre 1) | TODO capture | TODO | chambre1 |
| Telecommande 2 (= chambre 2) | TODO | TODO | chambre2 |
| Telecommande 3 (= salon) | TODO | TODO | salon |
| Telecommande 4 (= cuisine) | TODO | TODO | cuisine |
| Telecommande 5 (= cuisine 2 ?) | TODO | TODO | TODO |
| Noé canal 1 | TODO | TODO | volet1 |
| Noé canal 2 | TODO | TODO | volet2 |
| Noé canal 3 | TODO | TODO | volet3 |
| Noé canal 4 | TODO | TODO | volet4 |
| Noé canal 5 | TODO | TODO | volet5 |
| Noé canal ALL | TODO | TODO | tous |

**Note importante** : cette capture est OPTIONNELLE pour le projet (= l'approche bouton PROG suffit pour piloter). Mais elle ajoute la SYNC bidirectionnelle (= HA voit quand famille utilise Noé) et la BACKUP exhaustive de tous les compteurs.

**Quand l'ESP est en route** :
- 5 entités `cover.volet_X` dans HA (chambre1, chambre2, salon, cuisine, ...)
- Services natifs : open, close, stop, set_position
- Scènes type "mode nuit" (tous fermés), "mode jour" (tous ouverts), "mode canicule" (fermés sud)
- Automatisations : fermeture sunset + offset, ouverture sunrise + offset, fermeture si pic PV pour ombrage volontaire, etc.

**Coûts comparés** (5 volets) :
- DIY ESP32+CC1101 : **~15-20€** total (3-4€/volet) ⭐
- RFXtrx433XL/E : 130€ (26€/volet) — overkill pour 5 devices, intéressant à partir de 10+
- Profalux Connect officiel : 150-200€ (cloud-dépendant) — écarté

Voir aussi [[ha-projet-debitmetre-eau]] pour le projet ESPHome jumeau (commande groupée fin juin), [[ha-stack-architecture]] pour le stack HA général.

---

## R&D log = résumé exécutif de la recherche reverse (voir [[profalux-keeloq-reverse-research.md]])

La R&D exhaustive a été menée en session marathon 2026-06-27/28 (= 24 sections, ~1200 lignes de notes) puis complétée session 2026-07-03/04. Rien de ce qui suit ne change la décision projet : **Plan A (sacrifice Noé + PCB DREVET) reste optimal**. Ce log résume juste les conclusions clés pour éviter d'y revenir.

**Ce qui est confirmé** :
- Protocole RF Profalux = KEELOQ HCS301 868.350 MHz OOK, frame 66-bit (2 status + 4 button + 28 serial + 32 encrypted)
- Manufacturer key Profalux = jamais leakée en 16 ans, inaccessible software (= confirmé par 4 agents parallèles session marathon)
- Software-only reverse impossible : validé empiriquement par instrumentation Frida sur APK AirSend + analyse binaire AirSendWebService + analyse blob 221KB
- Les manufacturer keys DEVMEL (~40 marques KEELOQ dont PFX) vivent UNIQUEMENT dans le hardware BOX AirSend Duo
- HCS301 EEPROM contient seulement la crypt/device key dérivée (= PAS le manufacturer code lui-même)

**Alternatives évaluées et écartées** :
- ChipWhisperer-Nano/Lite DPA : donne SEULEMENT device key remote, pas la mfg key → inutile vs Plan A qui n'a besoin ni de l'une ni de l'autre
- Tear-down BOX AirSend Duo (~200€ + 50h skills RE) : voie hardware la plus prometteuse mais rentabilité nulle pour 5 volets
- Dongle Profalux USB MAI-DONGLE868CH-NC + AT commands (= Plan E) : techniquement plus simple mais ~100-150€ hardware
- LIBRIO CHRONO France Fermetures (= Plan A v2, ~20€ occasion) : partage manufacturer key avec Profalux confirmé empirique (topic Jeedom 85090) → alternative si Noé finalement pas sacrifiable

**Artefacts R&D encore utilisables** (= session actuelle 2026-07-04) :
- Android SDK + AVD `airsend_test` (= 3.5 GB, survit reboots) : setup Frida rejouable en 5 min
- Firmwares Avidsen re-téléchargeables via `calypshome.avidsen.one/repository/` (= directory listing nginx ouvert)
- Binary AirSendWebService re-téléchargeable via `devmel.com/dl/AirSendWebService.tgz`
- Specs OpenAPI DEVMEL disponibles publiquement sur `app.airsend.cloud/openapi/`
- Scripts fake box + Frida hooks : perdus après reboot MS01 mais reconstructibles en ~1h depuis les sections 12/16 de la doc R&D

**Curiosité RE future (= hors projet volets)** :
Si un jour l'envie de casser vraiment la manufacturer key Profalux : tear-down AirSend Duo + JTAG/SWD/UART firmware dump + Ghidra. Serait le premier reverse public complet de l'écosystème DEVMEL et débloquerait 40+ marques KEELOQ d'un coup. Budget : ~200€ hardware + ~50h skills RE = projet SCA dédié plusieurs mois.

Cross-links R&D : [[profalux-keeloq-reverse-research.md]] contient les 24 sections détaillées (Frida hooks, endpoints cloud, provisioning URLs provbox.profalux.com, AT commands dongle, analyse blob 221KB, 4 agents session marathon, etc.).
