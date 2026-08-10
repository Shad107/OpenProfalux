# Protocole SimpleIP — synthèse observationnelle PARTIELLE (=runtime seulement)

Date : 2026-08-06 (soirée)

## ⚠️ AVERTISSEMENT CRITIQUE : couverture partielle

**Ce document décrit UNIQUEMENT le protocole runtime observé APRÈS que la session soit déjà établie.**

**Phase INIT NON observée** :
- Discovery initial dongle (=possiblement port 3950 en multicast/broadcast)
- Auth handshake (=SDK expose `SimpleIPLocator::setPassword(bytes, len)` + `SimpleIPUpdater::writeFactoryPassword()` — challenge/response probable)
- Capabilities exchange / négociation version
- Établissement session_id (=comment `78df746a` est décidé)
- Configuration CC1101 initiale (=doc audit confirme "aucune écriture registres CC1101 pendant transfert" → écritures font partie de l'init non capturée)
- Provisioning IPv4/IPv6 (=`SimpleIPSettings::writeIPv4`, `writeHostname` dans SDK)

**Le faux CC1101 émulateur triche** — il accepte les TRANSFER sans exiger d'auth ni de setup initial. Le vrai dongle Profalux à froid a très probablement une phase de handshake auth complète que ce doc ne couvre pas.

**Pour capturer l'init réel** = nécessite un vrai dongle Profalux physique + capture Wireshark/tcpdump sur son trafic UDP entre allumage et premier transfer utilisable.

Sources :
- Logs UDP live émulateur `airsend-simpleip-live.log`, `simpleip-recv-emulator.log`
- Disassembly `libDevmelSDK.dll` (=Windows) et `AirSendWebService` (=Linux)
- Traces GDB `simpleip-recv-gdb.log`, `simpleip-return-gdb.log`, `simpleip-error-gdb.log`
- Symboles démanglés `windows-devmelsdk-symbols-demangled.txt`

## Statut

Synthèse basée sur observation UDP live + symboles ELF/PE, PAS sur documentation officielle Devmel. Structure globale confirmée par 3 sessions indépendantes (=peer ports 47430, 34531, 39569). Sémantique fine du payload SPI TRANSFER partiellement décodée. **Init handshake absent**.

## Transport

- UDP IPv6 link-local (=`fe80::be24:11ff:fee4:a9d3` observé)
- Port service principal : **33863** (=`0x8447`)
- Ports secondaires : **8820**, **3950** (=rôle inconnu, probablement discovery/multicast ou management)
- Le "faux CC1101" apparaît comme un pair UDP local pour AirSendWebService

## Format wire général

```
+--------+--------+--------+--------+
| session_id  [4 bytes]              |
+--------+--------+--------+--------+
| ctrl [2 bytes]  | opcode [2 bytes] |
+--------+--------+--------+--------+
| payload variable ...                |
+--------+--------+
| trailer / peer_nonce [2 bytes]     |
+--------+--------+
```

- **session_id** : 4 bytes qui varient à chaque paquet (=probablement compteur+session), mais **partagent souvent leur suffixe** `746a` sur une même session
- **ctrl** : 2 bytes de contrôle
  - `0x0000` en request typique
  - `0x8000` sur certaines responses (=comportement inconsistant selon l'opcode, voir table)
- **opcode** : 2 bytes big-endian, identifiant la commande
  - Bit `0x8000` sur response pour certains opcodes
- **payload** : variable selon opcode
- **trailer** : 2 bytes, constant sur une session (=peer_nonce ou node identifier attendu par les 2 parties)

## Opcodes observés

| Opcode req | Opcode resp | Sens | Taille req | Taille resp |
|:-:|:-:|:-|:-:|:-:|
| `0x6000` | `0x6000` | PING / HANDSHAKE | 30B | 46B |
| `0x5FFF` | `0xDFFF` | CLOSE / ACK session | 12B | 10B ou 12B |
| `0x4125` | `0xC125` | SPI TRANSFER | 90B | variable (=~120B) |

Notation : quand l'opcode réponse a le bit `0x8000` set (=passe de `0x4125` à `0xC125`, ou `0x5FFF` à `0xDFFF`), c'est un ACK/response. PING (`0x6000`) ne set pas ce bit — comportement asymétrique.

## PING (`0x6000`)

Request 30 bytes :
```
78df746a 0000 6000 9f58 <24 bytes payload aléatoire>
```

Response 46 bytes :
```
78df746a 0000 6000 01000001 00000000 5adf746a 0400 9f58 <20 bytes payload>
```

Interprétation : la response embarque `01000001` (=capabilities ?) puis une nouvelle session_id (`5adf746a`) qui sera utilisée pour la suite, puis flags `0400`, puis le trailer/nonce `9f58`.

## CLOSE (`0x5FFF` / `0xDFFF`)

Request 12 bytes :
```
78df746a 0000 5fff 0120 9f58
```

Response 12 bytes (=comportement A) :
```
78df746a 8000 dfff 0120 9f58
```

OU response 10 bytes (=comportement B, observé une fois) :
```
78df746a 00005fff 9f58   (=6 bytes payload)
```

`0120` semble être un status code (=peut-être "gracefully closed"). Le bit `0x8000` sur ctrl **et** opcode confirme la nature response.

## SPI TRANSFER (`0x4125` / `0xC125`)

C'est **l'opcode central** — il commande au faux CC1101 d'exécuter une séquence de strobes + burst FIFO.

### Request (90 bytes)

```
7adf746a 0000 4125 
0101 0036             <-- strobe SIDLE (0x36)
0100 3a01             <-- strobe SFRX (0x3a)  
0034 4100             <-- strobe SRX (0x34) + prefix burst read
ff 0000...0000        <-- 64 bytes FIFO buffer (0xff + 63×0x00)
0001 003d             <-- strobe SNOP (0x3d)
9f58                  <-- trailer/peer_nonce
```

L'émulateur log confirme dans son champ `spi_batch` :
```json
"spi_batch": ["36", "3a", "34", "ff00...", "3d"]
```

Séquence : **SIDLE → SFRX → SRX → burst FIFO 64B → SNOP**

### Response

```
7adf746a 8000 c125 
01000001 000001 000041 001f 
420c64cc39a926a700  (× 7)
420100 10 9f58
```

- `0x8000c125` = response marker
- `1f` = 31 = longueur payload utile ?
- 7 copies de `420c64cc39a926a700` (=9 bytes = trame PFX 65-bit)
- Trailer `420100 10 9f58` (=footer + peer_nonce)

**Point important** : la trame `420c64cc39a926a700` sortie n'est **pas** dérivée du FIFO input (=rempli de `ff+zeros`). Elle est **fabriquée par le service** — probablement une trame de référence PFX construite depuis l'ID de channel + counter interne.

## Ports secondaires 8820 et 3950

Non observés dans les logs live. Hypothèses (=à valider) :
- **8820** : discovery ou management config (=cohérent avec `SimpleIPSettings::readIPv4/writeIPv4`)
- **3950** : notifications asynchrones ou keep-alive

À confirmer par instrumentation ciblée sur ces ports.

## Classes SDK (=depuis symboles démanglés)

### `DevmelSDK::content::SimpleIPLocator`

Configuration d'un endpoint SimpleIP :
- `setIp(char const*)` / `setIp(unsigned char const*, unsigned char)`
- `setProtocol(unsigned short)`
- `setRemoteHost(char const*)` / `setRemotePort(unsigned short)`
- `setTimeout(unsigned int)`
- `setUrl(char const*)` — URL parse
- `setPassword(unsigned char const*, unsigned char)`
- `setLock(bool)` / `setGateway(bool)`

### `DevmelSDK::devices::SimpleIP`

Protocole primitif :
- Constructor `SimpleIP(SimpleIPLocator const&, bool)` OU `SimpleIP(unsigned char const*, unsigned char)`
- `managerRun()` / `managerRunSrv(void*)` / `managerAutorun()`
- `setLock(bool)` / `setTimeout(unsigned int)` / `setExceptions(bool)`
- `close()`
- **Pas de méthode publique `send` / `recv` directe** — tout passe par `managerRun` interne

### `DevmelSDK::devices::SimpleIPSettings`

Provisionnement device :
- `readHostname` / `readIPv4Address` / `readIPv4Mask` / `readIPv4GatewayAddress` / `readIPv6PublicAddress`
- `writeConfig(unsigned char)` / `writeAutoStaticIPv4` / `writeHostname` / `writeIPv4` / `writePassword`
- `buildConfig(bool, bool, bool)`

### `DevmelSDK::devices::SimpleIPUpdater`

Updater minimal :
- Constructor `SimpleIPUpdater(unsigned char const*)`
- `writeFactoryPassword()` — seule opération spécifique
- **Pas** un flasheur firmware — juste provisioning admin

### `DevmelSDK::devices::SimpleIPException`

Codes d'erreur :
- Constructor `SimpleIPException(unsigned char)` — code enum
- `what(unsigned char)` — message

## Ce qui reste à documenter

- **Sémantique exacte des groupes 4-byte** dans le payload TRANSFER (=`0101 0036`, `0100 3a01`, etc.) — pattern probable `[len][op][arg1][arg2]` mais pas 100% certain
- **Algorithme génération session_id** (=séquentiel ? aléatoire ? timestamp ?)
- **Fonction du `trailer/peer_nonce`** (=identifiant device attendu ? Sécurité ?)
- **Protocol des ports 8820 et 3950** (=jamais observés en live)
- **SimpleIPUpdater `writeFactoryPassword` payload exact** (=probablement opcode dédié)
- **Handshake d'authentification** si il existe (=`SimpleIPLocator::setPassword` suggère qu'il y en a un)

## Phase INIT — analyse statique (=preuve binaire)

**Sources** : `libDevmelSDK.dll` disassembly + symbol table + trace UDP live.

### `SimpleIP::SimpleIP(unsigned char const*, unsigned char)` @ `0x69f6e120`

Le vrai constructor primitif. Arguments :
- `rdx` = pointeur bytes (=identité/nonce/target)
- `r8d` = longueur (=**16 bytes** quand appelé depuis `SimpleIPUpdater`)

Séquence init observée (=disassembly `0x69f6e120..0x69f6e2a9`) :

1. **Allocation** `_Znwy(0x230)` = 560-byte structure interne
2. **3 × `CreateMutexA`** — mutex de synchro session (=offsets 0x18, 0xC8, 0xE8 dans structure)
3. **`CreateEventA`** — event Windows pour signal RX
4. **Timeout default `0xBB8 = 3000ms`** stocké à `struct+0x110`
5. **Counter global `.LC1406`** incrémenté = ID d'instance thread
6. Config zéro : counters `0x128`, `0x12c`, `0x13a`, `0x172`, `0x148` (=états internes)

**Aucun échange réseau dans le constructor** — le socket UDP est ouvert plus tard par `managerAutorun` appelé par `.LC1017` (=send function).

### `.LC1017` = fonction SEND privée @ `0x69f729d0`

Arguments (=convention MSVC x64) :
- `rcx` = `SimpleIP*` (=this)
- `edx` = opcode (=1 word, ex 0x23 pour writeFactoryPassword)
- `r8` = payload pointer
- `r9d` = flag (=souvent 0 ou 1)
- `[rsp+0x540]` = timeout ou length secondaire

Séquence observée :
1. `call managerAutorun` — démarre le thread manager si pas déjà lancé
2. `GetSystemTimeAsFileTime` — timestamp émission
3. `WaitForSingleObject(mutex, INFINITE)` — lock exclusif socket
4. Construit paquet, envoie via socket, attend réponse
5. Save 7 registres XMM = calcul CRC/hash sur paquet (=probable)
6. Stack frame = **0x4C8 bytes** = grosse buffer wire

**`.LC1017` est la primitive de send request/response utilisée par toutes les couches supérieures.**

### `SimpleIPUpdater::writeFactoryPassword` @ `0x69f88890`

Séquence auth handshake :
1. `GetSystemTimeAsFileTime` — timestamp initial
2. Calculs magic (`0xcccccccccccccccd` = /10, `0x20c49ba5e353f7cf` = /1000) = conversion µs→ms
3. Compare magic device `0x424c0001` (=`\x01\x00\x4C\x42` = "BL\x01" probablement Boot Loader magic) à `[rax+0x128]`
4. Si NON matching → send opcode **0x23** avec payload state initial
5. Timing 0x1F4 = **500ms max** avec retry via `Sleep(500-elapsed)`
6. Si magic match → send opcode **0x23** avec payload différent

**Auth flow probable** :
- Client envoie challenge (=`GetSystemTimeAsFileTime` timestamp comme nonce)
- Serveur répond avec status byte à `[rsi+0x1a]` — `0xff` = pas fini, `0x00` = OK, `0x03` = state 3
- Retry avec sleep(remaining)
- Opcode `0x23` = probablement **AUTH_FACTORY_PASSWORD** ou **PROVISIONING**

### Phase INIT observée dans logs UDP

**Le PING (`0x6000`) est le PREMIER paquet observé** dans tous les logs — pas de handshake antérieur au niveau UDP. Deux hypothèses :

1. **Le PING EST le handshake** : les 24 bytes "aléatoires" en payload sont un challenge crypto
2. **Un pré-handshake existe hors capture** (=ex: sur un autre port, ou lors du bind socket)

Analyse structure response PING :
```
78df746a  4B session_id (=echo request)
0000      2B ctrl
6000      2B opcode (=PING)
0100 0001 4B version/capabilities marker
0000 0000 4B zeros
5adf746a  4B second session_id (=byte 0 = req_byte0 - 0x1e)  ← CONSISTENT DIFF -30
0400      2B new ctrl (=flags "server accepted")
9f58      2B peer_nonce (=echo trailer/peer_id)
[20B payload]  = probable session key material
```

**Différence constante `0x1E`** entre byte 0 request et byte 0 response session_id (=vérifié sur 3 sessions indépendantes) :
- Session 1 : `4a` → `2c` (diff 30)
- Session 2 : `78` → `5a` (diff 30)
- Session 3 : `7a` → `5c` (diff 30)

Signification : **byte 0 = compteur ou type field**, réponse = req - 30. Pas un vrai session_id aléatoire.

## Table opcodes complète

| Opcode | Symbole probable | Sens | Preuve |
|:-:|:-|:-|:-|
| `0x0023` | AUTH_FACTORY / PROVISION | Écriture password factory | `writeFactoryPassword` passe `edx=0x23` |
| `0x0125` | SPI_TRANSFER | Alternative Linux (=vu `simpleip-0125-call.txt`) | Trace GDB fonction 40da |
| `0x4125` | SPI_TRANSFER (Windows) | Transfer strobes CC1101 | Observé UDP live |
| `0x5FFF` | CLOSE | Fin session gracieuse | Observé UDP live |
| `0x6000` | PING / HANDSHAKE | Établissement session | Observé UDP live |
| `0xC125` | =SPI_TRANSFER response | bit 0x8000 set | Observé UDP live |
| `0xDFFF` | =CLOSE response | bit 0x8000 set | Observé UDP live |

**Opcodes non capturés en UDP mais présents dans SDK** (=à confirmer par capture ciblée) :
- Opcode **`0x0020`** = `mov edx, 0x20` observé dans `writeFactoryPassword` — probablement WRITE_PASSWORD standard
- Opcodes de `SimpleIPSettings::read*/write*` = correspond aux méthodes `readHostname`, `readIPv4Address`, `readIPv4Mask`, `readIPv4GatewayAddress`, `readIPv6PublicAddress`, `writeConfig`, `writeAutoStaticIPv4`, `writeHostname`, `writeIPv4`, `writePassword` (=10+ opcodes à identifier)

## `AirSend::bind::list` — table des channels bindables

Extraite du binaire à vaddr `0x6a1e1980` (=Windows DLL), 177 entrées WORD LE terminées par `0x0000`.

**Statistiques** :
- 14 IDs famille **KEELOQ 868** (=25000-26000)
- 41 IDs range 26000+
- 20+ IDs range 1000-25000 (=protocoles bas)
- Quelques IDs sub-1000 (=peut-être RTC/config internes)

**Premiers IDs** (=ordre du binaire) :
```
[0]  0x055b = 1371   (=1er check, valeur "défaut")
[1]  0x6372 = 25458  ← KEELOQ 868
[2]  0x0370 = 880
[3]  0x0371 = 881
[4]  0x6373 = 25459  ← KEELOQ 868 (=près de KLQ868=25457)
[5]  0x1332 = 4914
...
[19] 0xf102 = 61698  (=range haut, protocoles récents)
```

**Note importante** : `PFX = 0x636f = 25455` **N'EST PAS** dans les premiers 40. Il faut vérifier s'il est plus loin dans la table (=peut-être encore présent) OU s'il est routé par un chemin différent (=probable, car PFX est un canal spécial 65-bit).

Table complète parsée dans script Python inline (=à sauver dans `reverse/emulation/bind-list-parsed.txt` si nécessaire pour référence).

## Auth handshake — modèle probable

Basé sur `writeFactoryPassword` + structure UDP :

1. **Discovery** (=non capturé, probable) : broadcast/multicast pour trouver dongle
2. **Ouverture session** : constructor `SimpleIP(target_bytes, 16)` — les 16 bytes sont soit l'IPv6 target soit un session token
3. **PING/HANDSHAKE** (=opcode 0x6000) : 24 bytes aléatoires en payload = challenge crypto
4. **Réponse PING** : contient `01000001` (=version), key material 20B, session_id modifié (=byte0 - 0x1E)
5. **Optionnel : AUTH** (=opcode 0x0023) via `writeFactoryPassword` — nécessaire si magic `0x424C0001` non détecté
6. **Bind canal** (=`AirSend::bind` local, pas d'opcode réseau) — vérifie channel dans `bind::list`
7. **Runtime** : PING keep-alive + SPI TRANSFER on demand
8. **CLOSE** (=opcode 0x5FFF) : fin gracieuse

**Statut** : les étapes 2, 5 sont **inférées** du disassembly. Les autres sont **observées**.

## Utilité pour OpenProfalux

Pour émettre une trame PFX depuis un firmware ESP32 **sans** DEVMEL :

**Non nécessaire** :
- On n'a **pas** besoin de reimplémenter SimpleIP côté ESP32
- Le protocole SimpleIP relie DEVMEL au dongle CC1101 propriétaire — un ESP32 avec CC1101 propre n'a pas besoin de cette couche

**Nécessaire** :
- Récupérer la trame PFX 65-bit construite (=confirmée par audit : `420c64cc39a926a700` ou séquence counter++)
- Récupérer les paramètres modulation OOK 868.35 MHz + timing 2500 (=confirmés à `0x6859c1` dans binaire)
- Reconstruire la représentation FIFO physique (=préambule + sync + OOK sur-échantillonné) — task #117

Le protocole SimpleIP sert donc de **preuve documentaire** que le canal PFX existe et fonctionne. L'implémentation ESP32 court-circuite cette couche transport.
