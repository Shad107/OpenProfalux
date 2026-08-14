# Portage GPU de l'attaque slide-MITM KeeLoq (RTX 1000 Ada)

Portage CUDA (via cupy `RawKernel`) de l'attaque **slide-meet-in-the-middle**
d'Indesteege et al., à partir du code C d'Enderlein
(`references/enderlein-keeloq-attack/preneel.cpp`). **Attaquant fonctionnel et
validé** : il récupère de vraies clés KeeLoq sur le GPU.

## Résultat

- **Cœur crypto validé bit-à-bit** contre le C d'Enderlein (`enc16/dec16/extract16/enc528/is_equal`).
- **Récupère une clé connue** (`E25AD14098DA3906`) générée par `genkey`/`genkp -x`,
  triple recoupement genkey = CPU = GPU.
- **Attaque complète (65 536 trames) estimée à ~4 jours** sur RTX 1000 Ada (6 Go,
  ~200 Go/s, 20 SM) - soit **~10× plus rapide que 28 cœurs CPU** (~40 j).

## Journal d'optimisation (empirique, GPU libre)

| Version | Idée | Temps attaque complète |
|---|---|---|
| v1 | scratch **par thread** (mémoire globale, non coalescé) | **262 j** ❌ |
| v2 | **par bloc** : table partagée, lectures pool coalescées, atomicCAS | **~4 j** ✅ (×66) |
| v3 | gen-counter (u64) pour éviter l'effacement de table | **~21 j** ❌ régression |

**Leçon (confirmée par la littérature)** : ce kernel est **borné par le trafic
mémoire**. v3 réduisait les opérations mais **doublait le trafic** (slots u64) →
plus lent. Sur un kernel memory-bound, **coalescing et trafic** priment sur les ops
(cf. best practices NVIDIA). Le bitslicing du seul papier « KeeLoq CUDA »
(Agosta-Barenghi, PARMA 2012) ne s'applique **qu'au brute-force**, pas au MITM.

Meilleure config trouvée : `GRID=768, BLK=512, HTS=next_pow2(N+1)`. Chasser
l'occupation (BLK=1024) échoue (`CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES`) et, d'après
la littérature, n'aiderait pas (register spills). VRAM utilisée ~1 Go : en mettre
plus n'accélère PAS (le goulot est le nombre de SM, pas la VRAM - vérifié :
GRID=2048 à 2,7 Go n'est pas plus rapide que GRID=768 à 1 Go).

## Fichiers

- `gpu_keeloq.py` - Étape A : primitives KeeLoq sur GPU, validées vs le C.
- `gpu_attack.py` - Étape B : attaque O(N²) (petit N), retrouve une clé connue.
- `gpu_attack_v2.py` - **Étape C, la version retenue** : hash-table O(N),
  bloc-coopératif, ~4 j. C'est la référence.
- `gpu_25frames.py` - scan COMPLET (65 536 k1 × 65 536 alpha) sur nos 25 trames
  réelles → **39,3 s, aucune clé** (confirme qu'il faut ~65 536 trames, pas 25).

## Usage

```bash
# venv cupy (driver NVIDIA Windows via passthrough WSL)
kl_venv/bin/python gpu_attack_v2.py     # valide + benchmark sur jeu synthetique
kl_venv/bin/python gpu_25frames.py      # scan complet des 25 trames reelles
```

## Limites honnêtes

- **Il faut ~65 536 vraies trames** de la télécommande cible (loi du paradoxe des
  anniversaires : `P(paire glissée) ≈ 1 - e^{-n²/2³²}`) - on en a 25.
- « Heures » n'est atteignable que sur un GPU data-center (A100/H100, ~×10 de bande
  passante). Sur cette carte, ~4 j est le bon ordre de grandeur.
- Le **cœur d'attaque** (crypto KeeLoq de référence) est le code C d'Enderlein,
  transmis en privé et **non redistribué ici** : ce dossier ne contient que le
  **portage GPU** (notre travail). Le C original reste dans le dépôt de recherche privé.
