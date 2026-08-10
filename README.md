# OpenProfalux

Firmware ESP32 open-source pour piloter les volets 868 MHz KEELOQ variants Chamberlain-like (=Profalux, Delta Dore X2D/X3D). Bridge MQTT vers Home Assistant local.

**Objectif** : reproduire l'approche "clone=3 FULL" de DEVMEL AirSend en open-source, sans dépendance cloud, sans licence commerciale, sans MFG key partagée.

Voir aussi `docs/RESEARCH.md` pour la synthèse technique de la session recherche 2026-08-05 (=4h20) qui a établi les preuves et l'architecture.

## Marques ciblées (=multi-target)

| Marque | Protocole | Statut |
|--------|-----------|--------|
| **Profalux** (=EMPX-B1, Noé, EMPEPX4, EMPXMUR) | KEELOQ **868.425 MHz** OOK (mesuré 2026-08-10, cf docs/MESURES-RF.md) | **Target primaire** |
| Delta Dore X2D | Variante 868 MHz | Extension future (=clone=3 confirmé DEVMEL) |
| Delta Dore X3D | Variante 868.95 MHz FSK | Extension future |
| France Fermetures LIBRIO | Même stack Stella que Profalux | Compat naturelle (=même MFG key groupe) |

## Hypothèse de travail (NON validée empiriquement)

> **Statut au 2026-08-10** : ce qui suit est une hypothèse déduite du binaire
> DEVMEL, pas un fait établi. Deux points la fragilisent :
>
> 1. Le point « aucune MFG key nécessaire » **contredit le code de ce dépôt** :
>    `firmware/main/pfx_keys.c` est justement une table de 15 clés fabricant
>    Profalux sélectionnées par index. Les deux ne peuvent pas être vrais
>    ensemble. Les modes d'apprentissage KeeLoq standard (simple, normal,
>    secure) exigent tous la MFG key, côté encodeur ET décodeur.
> 2. La vérification empirique de ces clés contre des trames réelles captées le
>    2026-08-10 a **échoué** : elles ne déchiffrent pas. Voir
>    `OpenProfalux-Research/captures/2026-08-10-profalux-868/VERIFICATION.md`.

DEVMEL AirSend Duo pilote officiellement Profalux via `clone=3 FULL` (=documenté OpenAPI DEVMEL + symbol C++ `DevmelSDK::content::Channel::isCloneable`). L'interprétation initiale était que Profalux implémente une variante Chamberlain Self-Learn (=US 5686904) sans MFG key partagée. Mais la présence même des clés PFX dans le binaire DEVMEL suggère l'inverse : **DEVMEL détient la MFG key et clone grâce à elle**. La question de savoir si le moteur accepte un émetteur à clé arbitraire reste ouverte, à trancher par un essai d'appairage réel.

## Architecture dual-target (=inspiration OpenXtraflame)

**Target External** (=public, safe) :
- ESP32 spare (=user en a des débitmètres/M5Stack)
- CC1101 externe câblé en SPI
- Bouton PAIR + boutons UP/STOP/DOWN pour test manuel
- MQTT vers Home Assistant + web UI de config
- **À publier** GitHub public + www.isno.fr

**Target M5Stack** (=production perso) :
- M5Stack ATOM Lite ou similaire
- Compact, alimenté USB-C
- Boutons virtuels via app HA
- **Reste privé** ou publique selon dev

## Composants matériels

| Composant | Ref | Coût | Statut user |
|-----------|-----|------|-------------|
| ESP32 (=any devkit) | ESP32-WROOM, ESP32-S3, C3 | 0€ | Existant |
| CC1101 breakout | E07-868T20D, ELECHOUSE | 0€ | Existant |
| Antenne 868 MHz | 8dBi hélicoïdale | 0€ | Existant |

**Total : 0€** (=matos déjà chez user, confirmé mémoire ha-projet-volets-profalux)

## Structure projet

```
openprofalux/
├── README.md                         # Ce fichier
├── LICENSE                           # MIT
├── .gitignore
├── Dockerfile.esp-idf                # Container ESP-IDF v5.2.2
├── docker-compose.yml                # Build/flash via Docker
├── docs/
│   ├── RESEARCH.md                   # Synthèse session 2026-08-05
│   ├── PAIRING.md                    # Procédure d'appairage détaillée
│   └── PROTOCOL.md                   # Frame KEELOQ 66-bit format
├── firmware/
│   ├── CMakeLists.txt                # Racine ESP-IDF
│   ├── partitions.csv
│   ├── sdkconfig.defaults            # Default config ESP-IDF
│   └── main/
│       ├── CMakeLists.txt
│       ├── hardware_config.h         # Pinout Target External/M5Stack
│       ├── main.c                    # Orchestration tasks
│       ├── config_nvs.c/h            # Persist serial + crypt_key + counter
│       ├── wifi_bridge.c/h           # STA + SoftAP + scan JSON
│       ├── mqtt_bridge.c/h           # Publish + HA discovery
│       ├── web_ui.c/h                # HTTP config server
│       ├── keeloq.c/h                # Algo 528 rounds standard
│       ├── profalux.c/h              # Protocole spécifique Profalux
│       └── cc1101.c/h                # Driver SPI CC1101
└── tests/
    ├── test_keeloq.c                 # Vecteurs test algo
    └── test_frame.c                  # Frame build/parse
```

## Roadmap

- [ ] **Phase 1** — Prototype ESP32 + CC1101 (=structure ci-dessus)
- [ ] **Phase 2** — Test empirique appairage sur volet chambre invitée
- [ ] **Phase 3** — Intégration MQTT + HA discovery
- [ ] **Phase 4** — Web UI de config (=SoftAP au premier boot)
- [ ] **Phase 5** — Documentation publique + publication
- [ ] **Phase 6** — Extension X2D Delta Dore (=même famille clone=3)

## Documentation technique

- **RESEARCH.md** : origines, preuves, hypothèses cristallisées
- **PAIRING.md** : procédure exacte pour un utilisateur (=trombone, switch P/N, etc.)
- **PROTOCOL.md** : frame KEELOQ 66-bit, timings, boutons, encryption

## License

MIT — algorithme KEELOQ = brevet US 5517187 **expiré en 2016**, algo public/libre.

## Contexte

Session marathon 2026-08-05 (~4h20) a exhaustivement épuisé toutes les voies logicielles pour extraire la MFG key Profalux. Conclusion : elle n'est pas nécessaire pour piloter les volets — la procédure d'apprentissage utilisateur permet à un nouvel émetteur random de s'enregistrer directement dans le moteur. Voir `docs/RESEARCH.md` pour détails.

Mémoire d'origine : `~/.claude/projects/-home-olivier/memory/profalux-keeloq-reverse-research.md`
