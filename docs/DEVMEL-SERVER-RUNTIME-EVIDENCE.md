# DEVMEL AirSendWebService — évidence RUNTIME (=serveur live)

**Généré :** 2026-08-06 après-midi
**Source :** `AirSendWebService` (=binaire ELF x86-64 statiquement lié) lancé localement en 3 secondes sans aucun setup, écoutant sur `127.0.0.1:33863`.

## Résumé exécutif

Le service DEVMEL **tourne en local sans dongle physique** et accepte `POST /channels/build` avec `id=25455` (=PFX Profalux), retournant un channel virtuel utilisable. Cette évidence runtime **réconcilie** les résultats contradictoires du matin :

- **`isCloneable(PFX 25455) = 0`** dans le SDK Android → CORRECT, mais concerne "cloner depuis trame SNIFFÉE" (=pas de decoder disponible)
- **`POST /channels/build` avec PFX = OK** → le service **PEUT CONSTRUIRE** l'identité logique d'un émetteur PFX virtuel

**Reformulation exacte** : DEVMEL ne clone pas une télécommande Profalux **existante par sniff**, mais sait **construire une nouvelle identité virtuelle random** destinée au chemin PFX. Combiné à une procédure learn du récepteur, ce workflow est compatible avec une méthode **Chamberlain Self-Learn** et suggère que DEVMEL l'utilise. Le runtime du service seul ne prouve toutefois ni la trame RF finale, ni l'établissement de la clé dans le moteur.

## Setup reproductible

```bash
# Le binaire est statiquement lié = tourne directement sur Linux x86-64 host
/home/olivier/projects/OpenProfalux-Research/binaries/devmel/AirSendWebService &
# → écoute 127.0.0.1:33863 en 3 secondes
```

Aucun hardware, aucun dongle, aucun QEMU, aucune config nécessaire.

## Endpoints découverts en runtime

| Endpoint | Méthode | Statut | Effet observé |
|:-|:-:|:-:|:-|
| `/service/status` | GET | ✅ 200 | `{"version":0.22}` |
| `/channels` | GET | ✅ 200 | **Retourne la liste complète des 182 channels** avec `id`, `name`, `band`, `counter`, `getDecoder` |
| `/channels/build` | POST | ✅ 200 | Génère un channel virtuel avec `source` random |
| `/airsend/openapi.json` | GET | 🔒 401 | Doc OpenAPI protégée par auth Bearer sp:// |
| `/airsend/bind` | POST | 🔒 401 | Écoute RF via device — nécessite auth |
| `/airsend/transfer` | POST | 🔒 401 | Émission RF — nécessite auth |
| `/airsend/recv` | POST | 🔒 401 | Réception RF — nécessite auth |
| `/airsend/close` | POST | 🔒 401 | Fermeture connexion — nécessite auth |

**Auth format** : `Authorization: Bearer sp://<password>@[<ipv6_link_local>]?gw=<gateway_index>` — exige un vrai device AirSend Duo joignable via IPv6 link-local.

## POST /channels/build — construction d'une identité PFX

**Requête** :
```bash
curl -s -X POST -H 'Content-Type: application/json' \
     -d '{"id":25455,"source":0,"counter":0}' \
     http://127.0.0.1:33863/channels/build
```

**Réponse** :
```json
{"id":25455,"source":254055,"counter":2}
```

Observations critiques :
- Le champ `source` **envoyé par le client est IGNORÉ** — le serveur en génère un aléatoire
- **Un nouvel appel** avec la même requête retourne un `source` différent (=vérifié sur 5 appels : 16487, 168039, 20583, 159847, 143463)
- Le `counter` initial est stable à **2** (=pour PFX)
- Le mécanisme = **création d'un nouvel émetteur virtuel avec serial 28-bit aléatoire**

**Schéma étendu accepté** :
```bash
curl -X POST -d '{"id":25455,"source":123456,"counter":0,"memory":"NONE","mac":0,"token":0,"seed":0}' ...
# → {"id":25455,"source":159847,"counter":2,"mac":0,"token":0,"seed":0,"memory":1}
```
Le champ `memory` retour `1` = **`USE`** selon YAML enum `[NONE=0, AUTO, USE, PUT, REMOVE]`.

## GET /channels — table complète 182 channels

Sauvegardée dans `~/projects/OpenProfalux-Research/analysis/devmel_server_live/channels-full-2026-08-06.json` (=12 KB).

**Nouveau champ `getDecoder`** (=inconnu avant cet audit) :
- `getDecoder = 0` → **pas de decoder** (=ne peut pas décoder trame reçue)
- `getDecoder = 1` → **decoder générique** partagé (=KEELOQ standard type)
- `getDecoder = <id>` → **decoder dédié propriétaire** au channel

**Répartition** :
- 106 channels utilisent le decoder générique `1`
- 27 channels ont `getDecoder = 0`
- ~48 channels ont un decoder dédié propriétaire

### Famille KEELOQ 868 (=id 25400-25720) — vue complète

| ID | Name | Band | Counter | getDecoder | Interprétation |
|---:|:---|:-:|---:|---:|:-|
| 25454 | HPD | 2 | 16 | 25454 | Decoder dédié |
| **25455** | **PFX** | **2** | **32** | **0** | ⭐ **Profalux : PAS de decoder, mais émission via build** |
| 25456 | KLQ | 1 | 3 | 1 | Standard KEELOQ 433 |
| 25457 | KLQ868 | 2 | 3 | 25457 | Standard KEELOQ 868 avec decoder dédié |
| 25464 | BFT | 1 | 16 | 0 | Pas de decoder |
| 25466 | SMO (Somfy) | 1 | 32 | 1 | Decoder générique |
| 25476 | NSL (Nice) | 1 | 32 | 1 | Decoder générique |
| 25485 | CRD868 | 2 | 32 | 0 | Pas de decoder |
| 25494 | TLC868 | 2 | 32 | 0 | Pas de decoder |
| 25497 | DKT868 | 2 | 32 | 0 | Pas de decoder |
| 25500 | FDN868 (Franciaflex) | 2 | 32 | 25500 | Decoder dédié |

### Sémantique définitive `getDecoder = 0`

Un channel avec `getDecoder = 0` :
- **NE peut PAS décoder** une trame reçue (=impossible d'extraire crypt_key/serial d'un sniff)
- **PEUT être BUILD** = construire un objet d'émetteur virtuel avec serial random
- Ce qui explique la contradiction apparente avec `isCloneable(25455) = 0` dans le SDK

Le SDK `isCloneable` teste **"est-ce qu'il y a un decoder ?"** — pour PFX c'est non, donc `isCloneable = 0` = **honnête** mais mal nommé.

Le chemin observé **construction d'un émetteur virtuel + apprentissage par le récepteur** ressemble au workflow **Chamberlain Self-Learn**. Il est distinct du champ API `clone mode 3` et doit encore être confirmé au niveau RF ou matériel.

## Réconciliation avec la journée

### Ce qui était vrai depuis le début

1. Le fichier `AirSendWebService.yaml` documente bien `clone: 0=none ; 1=unavailable ; 2=temporarly ; 3=full`
2. PFX (25455) est un channel supporté par DEVMEL (=confirmé par la table serveur)
3. Le SDK expose `Channel::isCloneable(25455) = 0` (=confirmé par exécution runtime user)
4. Le plugin BOX Athemium `atm_io_profalux.so` construit uniquement 9 commandes AT statiques

### Ce qui était mal interprété

1. **"clone=3 FULL" du YAML** ≠ **`isCloneable = 1` du SDK**. Ce sont **deux sémantiques différentes**.
2. **`isCloneable`** teste la présence d'un **decoder** (=capacité à décoder une trame reçue = clone from sniff)
3. **`/channels/build`** teste la présence d'un **encoder** (=capacité à créer une trame émettable)
4. **PFX** = pas d'encoder=1, mais **encoder OK** via un mécanisme différent (=probablement dédié dans le hardware AirSend)

### Ce qui reste incertain

1. Le champ `clone` du YAML (=0..3) : **on ne sait pas** exactement à quelle fonction native il correspond. `getCloneMode(id)` n'existe pas dans le SDK. La valeur "3=full" pour PFX est **peut-être une projection marketing** ou concerne le comportement du serveur, pas du SDK.
2. **La commande AT exacte** qu'envoie le hardware pour PFX = inconnu, exige un dongle physique
3. **Le mode learn du moteur Profalux** accepte-t-il vraiment un émetteur random en Chamberlain-style ? = à valider empiriquement vendredi

## Impact pour OpenProfalux

**La piste Chamberlain Self-Learn redevient une hypothèse de travail sérieuse**, soutenue par un comportement runtime compatible :

1. DEVMEL **accepte** de créer un émetteur PFX virtuel via `/channels/build`
2. Le mécanisme = **génération d'un serial 28-bit random + counter initialisé à 2**
3. Si Chamberlain hypothesis est vraie, un moteur Profalux en mode learn devrait accepter cet émetteur virtuel
4. **Vendredi** (=CC1101+ESP32) peut reproduire exactement ce workflow **à coût 0€**

**Protocole exact à implémenter côté OpenProfalux ESP32** :
- Générer un `source` aléatoire 28-bit (=via TRNG ESP32)
- Initialiser un `counter` = 2
- Format frame KEELOQ 66-bit standard (=NLF 0x3A5C742E, 528 rounds)
- Émettre plusieurs trames pendant que l'user active mode learn moteur (=trombone R + STOP sur télécommande d'origine)
- Le moteur enregistre le nouvel émetteur → clones possible

**Si Chamberlain KO empiriquement vendredi**, backup plan = PICkit + sacrifice MAI-EMPX (=voie hardware économique).

## Localisation forensique

- **Binaire serveur** : `~/projects/OpenProfalux-Research/binaries/devmel/AirSendWebService` (=6.3 MB)
- **Table channels dumpée** : `~/projects/OpenProfalux-Research/analysis/devmel_server_live/channels-full-2026-08-06.json` (=12 KB, 182 channels)
- **YAML OpenAPI** : `~/projects/OpenProfalux-Research/binaries/devmel/AirSendWebService.yaml`

## Mise à jour runtime — transport BOX confirmé

Un `POST /airsend/transfer` PFX avec le locator
`sp://password@[fe80::1]?gw=0` franchit l'authentification et retourne un
événement `PENDING` (`type=0`, `reliability=192`). Le traçage des appels
réseau du binaire original montre ensuite :

- création d'une socket `AF_INET6/SOCK_DGRAM` ;
- destination exacte `fe80::1`, UDP **33863** ;
- une copie envoyée sur chaque interface non-loopback ;
- premier datagramme de 30 octets, chiffré/authentifié par le mot de passe du locator.

Exemple capturé :

```text
0ba9746a00006000ae3717cdf7395e62d2c2f669fb35399d42134b6c61c7
```

Le port `8820`, auparavant déduit d'une chaîne isolée du binaire, n'est pas
le transport observé ici. Le SDK Java original confirme également 33863 comme
port SimpleIP par défaut. La prochaine étape consiste à répondre au handshake
SimpleIP pour amener le service jusqu'au datagramme de commande PFX complet.

Traces reproductibles :

- `~/projects/profalux/reverse/emulation/airsend-network-live.strace`
- `~/projects/profalux/reverse/emulation/airsend-live-response.json`

## Mise à jour runtime — handshake et bus SPI émulés

Le SDK Java récupéré établit que la remorque d'authentification de 20 octets
est un HMAC-SHA1 standard calculé sur le paquet SimpleIP complet avec le mot de
passe du locator (`AnonymousClass13.m33`). Un émulateur IPv6 local utilisant ce
calcul a réussi le handshake du binaire Linux original.

AirSendWebService a alors envoyé la commande SimpleIP verrouillée `0x0125`
(293). Le SDK authentique associe 293 à `SpiMaster.transfer`. Son corps contient
un sélecteur CS puis des transferts SPI préfixés par une longueur `uint16` LE :

```text
36
3a
34
ff 00...00        # transfert de 65 octets
3d
```

Ces valeurs correspondent exactement aux strobes CC1101 `SIDLE`, `SFRX`,
`SRX`, lecture burst de la FIFO RX et `SNOP`. C'est une inférence protocolaire
forte, pas encore l'identification physique certaine de la puce : le firmware
intermédiaire du dongle peut interpréter lui-même ce format groupé.

La réponse SimpleIP corrigée (`channel|0x80`, verrou `0xC000`, session recopiée)
est acceptée au niveau transport. Un résultat SPI rempli de zéros ne suffit pas
à faire progresser le service vers une émission PFX. La prochaine cible est
donc un émulateur stateful des états et FIFO CC1101.

Nouveaux artefacts :

- `~/projects/profalux/reverse/emulation/capture_airsend_udp.py`
- `~/projects/profalux/reverse/emulation/airsend-simpleip-v2.log`
- `~/projects/profalux/reverse/emulation/airsend-simpleip-v2.jsonl`

## Note méthodologique

Cette découverte a validé le point du user : **arrêter de s'accrocher aux noms** (=`SimpleIPUpdater`, `SerialTerminal`, `isCloneable`) et **regarder le comportement** (=lancer le binaire, sonder les endpoints). Le service tournait avec toutes ses réponses **en 3 secondes** au lieu des heures d'analyse statique. À généraliser pour toute future analyse binaire.
