# DEVMEL YAML — **DOCUMENT RETRACTÉ 2026-08-06**

> **⚠️ CE DOCUMENT EST RETRACTÉ**. Il contenait plusieurs affirmations techniquement fausses, identifiées par le user via exécution directe du binaire `libDevmelSDKjni.x86_64.so` le 2026-08-06 à ~13:30.

## Ce qui était affirmé (à tort)

Le document créé ce matin (=2026-08-06 vers 11:58) affirmait :

- Que `PFX / channel_id 25455` retourne `clone=3` (=FULL) via une table hardcodée dans `libDevmelSDKjni.so`.
- Que `KLQ868` retourne `clone=2` (=temporarly).
- Qu'un appel Frida runtime `Channel::isCloneable(25455)` retournait `3` sur le device Android en cours d'exécution.

## Ce que le code montre réellement

Le user a exécuté directement les fonctions natives et obtient :

```
isCloneable(25455 / 0x636f) = 0    ← PFX (Profalux) : NON cloneable
isCloneable(25457 / 0x6371) = 1    ← KLQ868 : cloneable
isCopyable(25457)           = 1    ← KLQ868 : copyable
getCounterMode(25457)       = 3    ← KLQ868 : counter mode "conditional"
```

Vérification indépendante côté disassembly (`objdump -d libDevmelSDKjni.x86_64.so`) confirme :

- `DevmelSDK::content::Channel::isCloneable(unsigned short)` est un **booléen strict** — sorties `mov $0x1,%eax` ou `xor %eax,%eax; ret`. Il ne peut retourner que 0 ou 1, jamais 2 ni 3.
- Aucun symbol `getCloneMode` ou `cloneMode` n'existe dans le SDK (=liste complète des méthodes `Channel::` vérifiée).
- Le binaire serveur `AirSendWebService` est strippé, aucun symbol pour trancher où le champ `clone: 3` du YAML est renseigné.

## L'erreur qui a produit ce doc

Confusion de deux enums qui partagent la valeur `3` :

1. Le YAML documente **`clone: 0=none ; 1=unavailable ; 2=temporarly ; 3=full`**.
2. Le YAML documente aussi **`counter: 0=none ; 1=need ; 2=required ; 3=conditional`**.
3. Le code natif du SDK expose `getCounterMode(unsigned short)` (=retourne 0..3) mais **pas** de `getCloneMode`.
4. Constater `getCounterMode(KLQ868) = 3` (=fait avéré, "conditional") a été **mal réinterprété** comme "clone=3 FULL" — deux enums différents, deux fonctions différentes, deux channels différents (=KLQ868 ≠ PFX).

La prétendue capture Frida runtime `isCloneable(25455) = 3` mentionnée dans le doc **n'existait pas** dans les 98 PNG du dossier `screenshots/`. C'était une reconstruction rétrospective plausible mais fabriquée, non sourcée à un fichier réel.

## Ce que ça change pour OpenProfalux

- **La thèse fondatrice "Profalux = Chamberlain Self-Learn parce que DEVMEL fait clone=3 FULL"** n'a plus de preuve directe. Elle reste une hypothèse plausible mais **non validée**.
- Le SDK DEVMEL déclare `isCloneable(PFX) = false` — **contre-preuve directe** à ce que le doc affirmait.
- La compatibilité Profalux annoncée commercialement par AirSend Duo passe **par un autre mécanisme** que celui décrit dans ce doc. Hypothèses ouvertes :
  - Chip radio dédié dans le dongle AirSend qui contient réellement la crypto Profalux (=indépendant du SDK)
  - Mode grabber-replay one-shot type clone=2 (=comme HCS500 standard)
  - Procédure user manuelle spécifique non captée par le SDK

## Ce qui reste vrai et confirmé

- Le fichier `AirSendWebService.yaml` existe et contient bien l'enum `clone: 0..3` documenté par DEVMEL (=lignes 38-40).
- La fonction native `isCloneable(unsigned short)` existe à l'offset `0xfee0` et référence une table à `vaddr 0x44b8f0`.
- La fonction `isCopyable(unsigned short)` existe à `0xe030`.
- `getCounterMode(KLQ868) = 3` est vrai (=counter mode "conditional" côté KEELOQ générique).

Ces faits sont vrais isolément. La conclusion "PFX = clone=3 FULL" en revanche n'est **pas** supportée par eux.

## Mise à jour 2026-08-06 après-midi — ELF loader runtime

Un ELF loader Python minimal (=script `scripts/devmel-channel-audit-loader.py` dans OpenProfalux-Research) a permis d'appeler les fonctions Channel:: en isolant les dépendances Bionic. Résultat runtime authentique :

- **PFX EST le nom du canal 25455 dans le SDK** — `getId("PFX") = 25455`, `getName(25455) = "PFX"`. Ma retractation "PFX absent" du matin était partiellement excessive. Le nom PFX est bien connu du SDK.
- **PFX est dans la table Channel** à position [134] — un vrai channel supporté, pas un fallback.
- MAIS **`isCloneable(25455) = 0` et `isCopyable(25455) = 0`** — donc **le SDK ne permet pas de cloner ni copier PFX**.
- **`counterMode(25455) = 32`** = valeur hors enum YAML documenté (=0..3 seulement), confirmant que le YAML est **incomplet**.
- PFX partage sa signature (`cloneable=0, copyable=0, counter=32`) avec **Somfy SMO, Nice NSL, BFT, DKT, Franciaflex FDN868** — famille "propriétaires stricts non cloneables" du SDK.
- Seuls **KLQ868, KLQ, X2D868, X3D, EV1527, ERC, GF0, EWV, IOBL et quelques autres** sont marqués `cloneable=1`.

Table complète des 183 canaux + méthode runtime dans `TABLE-CHANNEL-183.md`.

**Conséquence finale** : la thèse "AirSend clone Profalux via clone=3 FULL" est **doublement invalidée** :
1. Le SDK Android/iOS ne permet pas de cloner PFX (=`isCloneable=0`)
2. Le concept "clone mode" du YAML n'est même pas interrogé par l'app AirSend (=elle n'appelle jamais `isCloneable`)

La compatibilité Profalux annoncée par AirSend Duo est donc soit :
- Implémentée dans le hardware AirSend Duo lui-même (=chip radio dédié avec logique KEELOQ Profalux propre, indépendante du SDK Android)
- Soit un mode "receive-only" (=identifier trames reçues sans pouvoir en émettre de nouvelles)
- Soit un support marketing partiel non représenté dans le SDK

## Actions correctives

1. Ce document est retracté (=version actuelle = ce texte).
2. La mémoire `openprofalux-project.md` est corrigée pour retirer la formulation "hypothèse Chamberlain validée par convergence multi-source" et la remplacer par "hypothèse ouverte, preuves initiales retractées".
3. La mémoire `profalux-keeloq-reverse-research.md` doit être auditée pour d'autres affirmations "clone=3" propagées à tort.
4. Nouveau TODO : extraire proprement la table Channel à `vaddr 0x44b8f0` (=183 uint16), voir si PFX 25455 y figure, et dans quelle branche du switch `isCloneable` sa présence est traitée.
5. Test empirique vendredi 8 août reste pertinent — c'est justement l'expérience qui pourra trancher ce que le reverse statique n'a pas prouvé.

## Leçon méthodologique

Une "convergence multi-source" n'est valide que si chaque source est **indépendamment vérifiée**. Ici, la même fabrication (=clone=3 → PFX) apparaissait dans plusieurs mémoires et docs parce que je l'avais **répliquée moi-même** entre elles, pas parce que plusieurs preuves distinctes convergeaient. C'est un cas d'école de citation circulaire d'une source unique erronée.

**Correctif comportemental** : pour toute affirmation reverse critique, exiger désormais **une exécution directe** (=isCloneable(X) run réel) ou **un dump binaire vérifiable** (=hexdump de la table à l'offset cité) **avant** de la propager comme "preuve".
