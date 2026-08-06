# OpenProfalux — Plan test empirique appairage

Ce document décrit la procédure de test empirique révisée après la [contre-analyse « 10e homme »](CONTRE-ANALYSE.md) qui a affaibli l'hypothèse initiale d'un simple "émets crypt_key random et le moteur enregistre".

## Rappel de la faiblesse identifiée

Une trame HCS301 ne transmet pas la clé secrète 64-bit. Un ciphertext 32-bit peut être produit par ~2^32 clés compatibles pour une paire plaintext/ciphertext donnée. **Le moteur ne peut pas déduire directement la crypt_key d'un seul bloc chiffré**.

Donc si l'appairage marche vraiment sans MFG key partagée, il existe **nécessairement** :
- Soit un canal séparé qui transmet la clé (=séquence propriétaire, trames hors format HCS301)
- Soit un stock d'identités préprovisionnées côté BOX AirSend
- Soit une dérivation faible côté Profalux (=clé calculable depuis serial seul)
- Soit une capacité crypto dans un firmware radio séparé non-analysé

## Plan test empirique révisé — 4 phases

### Phase 0 — Setup hardware (=vendredi 8 août ~1h)

1. Soudure E07-900M10S selon [WIRING.md](WIRING.md)
2. Câblage régulateur 3.3V + M5Stack ATOM Lite
3. Flash firmware OpenProfalux via docker
4. Vérifier `PARTNUM=0x00` au log série

### Phase 1 — Capture RX passive AVANT tout TX (=priorité absolue)

**But** : caractériser exactement ce que la BOX AirSend émet pendant sa procédure d'appairage, sans polluer avec notre propre TX.

**Procédure** :
1. Démarrer OpenProfalux en mode RX (=`openprofalux/listen/start` MQTT)
2. Rien d'autre — pas de burst TX
3. Simuler une procédure d'appairage classique :
   - Prendre télécommande Profalux d'origine EMPX-B1
   - Basculer switch P/N vers **P**
   - Presser **STOP** 5 sec
   - Moteur aller-retour de confirmation
   - Rebasculer switch vers **N**
4. Capturer TOUTES les trames pendant cette séquence

**Observables** :
- Nombre total de trames
- Durée séquence
- Longueur bit de chaque trame (=66 bits HCS301 standard, ou différentes ?)
- Modulation (=OOK ou changement en cours ?)
- Débit / bit-time
- Structure trame (=préambule, header, payload)
- Serial visible
- Rolling counter progression
- **Y a-t-il des trames HORS format HCS301 ?** — question critique

**Interprétation** :
- **Toutes trames = 66-bit HCS301 standard** → hypothèse E (=transfert clé propriétaire) affaiblie
- **Trames hors format présentes** → hypothèse E confirmée, il faut reverse ce format
- **Séquence longue avec pattern répétitif** → hypothèse préprovisionnement (=matching multiple serials essayés)

### Phase 2 — Test 1 : émission avec identité contrôlée

**But** : tester si le moteur accepte une paire (serial, crypt_key) arbitraire.

**Procédure** :
1. Générer runtime dans ESP32 :
   - `serial = 0x0A5F2C41` (=valeur contrôlée connue)
   - `crypt_key = 0x123456789ABCDEF0` (=valeur contrôlée, testable côté user)
   - `counter = 0`
2. Trigger appairage via HA (=`openprofalux/volet_test/pair`)
3. **Immédiatement** : sur télécommande d'origine, procédure PROG / P+STOP
4. Moteur en mode learn 60s
5. ESP32 émet burst 60 trames pendant fenêtre
6. Observer résultat

**Interprétation** :
- Moteur enregistre → hypothèse Chamberlain naïve (=D "dérivation faible" ou vraie acceptation arbitraire)
- Moteur ignore → hypothèse E ou A (=besoin séquence propriétaire ou firmware séparé)

### Phase 3 — Matrice discriminante (=si Phase 2 échoue)

Ta table de la contre-analyse section 8, Test 5 :

| # | Serial | Seed | Clé | Counter | Résultat |
|---|--------|------|-----|---------|----------|
| 1 | A     | X    | K1  | 1       | ?        |
| 2 | A     | X    | K2  | 1       | ?        |
| 3 | A     | Y    | K1  | 1       | ?        |
| 4 | B     | X    | K1  | 1       | ?        |
| 5 | B     | Y    | K2  | 1       | ?        |

**Interprétation** :
- Toutes passent → apprentissage direct ou validation très faible
- Une seule relation passe → dérivation attendue
- Seul un serial particulier passe → identité préprovisionnée ou whitelist
- Aucune ne passe → secret fabricant ou séquence propriétaire manquante

### Phase 4 — Reverse séquence propriétaire (=si nécessaire)

Si Phase 2-3 échouent ET Phase 1 a révélé des trames hors format HCS301 :

1. Analyser structure trames non-HCS301 :
   - Longueur bits
   - Pattern début (=magic bytes)
   - Champs (=serial ? key parts ? counter ?)
2. Comparer avec la doc KEELOQ Programming System (=DS51036D Microchip) qui décrit les commandes propriétaires
3. Reproduire la séquence dans le firmware OpenProfalux
4. Retester Phase 2 avec la séquence complète

## Priorité absolue = Phase 1

**Ne pas sauter la Phase 1 pour aller directement à la Phase 2 "burst brute force"**. Sans caractériser d'abord ce que la BOX AirSend émet, on tire à l'aveugle et un échec de Phase 2 ne discrimine rien.

## Après validation empirique

Selon les résultats, mettre à jour :
- `CONTRE-ANALYSE.md` avec les hypothèses éliminées et celles renforcées
- `docs/RESEARCH.md` avec les preuves empiriques ajoutées
- `firmware/main/profalux.c` selon la vraie procédure identifiée
