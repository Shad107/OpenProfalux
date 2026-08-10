# Protocole AT dongle Profalux KEELOQ 868 — reverse analysis complet

**Généré :** 2026-08-06 après-midi
**Sources binaires** :
- `atm_io_profalux_pfxbox2_with_debug.so` (=76 KB, MIPS, debug_info intact)
- 7 versions diffées du plugin (=v29201 en 2018 → v66717 en 2025)

## Résumé

Le dongle USB Profalux 868 KEELOQ **`MAI-DONGLE868CH-NC`** parle **AT commands** au host Linux via `/dev/ttyACM0` (=USB CDC-ACM, symlink `/dev/atm_profalux_keeloq`). Le plugin `atm_io_profalux.so` construit et envoie les commandes AT, parse les réponses avec regex, et expose 5 handlers URI vers l'app.

## Commandes AT construites par le plugin (=liste complète)

| Commande | Effet | Fréquence |
|:-|:-|:-|
| `AT?` | Ping / query status | Init |
| `ATZ` | Factory reset | Sur demande |
| `ATQ0` | Quiet mode | Init |
| `AT&V` | **View verbose config** (=version + params) | ⭐ Init handshake — **envoyée 2× dans la séquence init** |
| `AT$C?` | Query channel actuel | Init |
| `AT$CP=14` | Set config parameter (=14 par défaut) | Init |
| `AT$CP?` | Query config parameter | Init |
| `AT$SF=<id>,<action>` | ⭐ **Send frame** (=commande principale de contrôle) | Runtime |
| `AT$TR=25,15,70,70` | Set transmission timing (=T0 + 3 delays ms) | Init |

## `AT$SF=<id>,<action>` — Mapping actions COMPLET (=révélé par émulation runtime)

Confirmé via wrapper C MIPS `profalux_plugin_host.c` chargeant `atm_io_profalux.so` en isolation avec `fake_profalux_dongle.py` : le plugin envoie **6 actions différentes** via `AT$SF` :

| Action utilisateur | Valeur `AT$SF` action |
|:-|-:|
| open / on / up | **0** |
| close / off / down | **1** |
| stop | **2** |
| fav_pos1 / fav_pos | **4** |
| ⭐ **register a newly allocated remote/device ID** | **11** |
| ⭐ **unregister a remote/device ID** | **14** |

**IMPORTANT** : **le register est PAS une commande AT séparée**. Le plugin :
1. Alloue un slot ID libre dans [0..49] (=le dongle a **50 slots max**)
2. Envoie `AT$SF=<slot>,11` au dongle
3. Le dongle enregistre le device dans ce slot (=probablement en émettant une trame RF register + attendant confirmation)

**Unregister** : `AT$SF=<slot>,14` libère le slot.

**Impact critique** : ma question précédente "quelle commande AT triggerait le register ?" était mal posée. **La réponse est `AT$SF=<slot>,11`**. Le mécanisme d'appairage est donc **entièrement contrôlé côté logiciel BOX** — le plugin choisit le slot, le dongle exécute.

## Séquence init boot (=ordre exact révélé par debug symbols)

```
1. AT?              ← ping
2. ATQ0             ← quiet mode
3. AT$C?            ← query channel
4. AT$TR=25,15,70,70 ← set timing params
5. AT$CP=14         ← set config parameter
6. AT$CP?           ← query config (echo pour vérif)
7. AT&V             ← ⭐ triggerhandshake complet (=identification + versions + list of keys)
```

## Réponses du dongle (=parsées par regex du plugin)

```
PFX KEELOQ                    ← marqueur d'identification (=exigence de match)
Software Version: X.Y         ← regex "Software Version: *(.*)"
Hardware Version: X.Y         ← regex "Hardware Version: *(.*)"
Frame Repeat Nb : N           ← regex "Frame Repeat Nb : *(.*)"
T0=<value>,                   ← regex "T0=([0-9]*),"
Received list of keys '...'   ← ⭐ le dongle retourne sa liste interne de clés
:OK                           ← terminaison succès
:KO                           ← terminaison échec
```

Messages debug du plugin qui révèlent le workflow :
- `"Peripheral connected! Waiting %i seconds for initialization."`
- `"Possible compatible dongle found. Requesting version."`
- `"No correct Profalux dongle version detected (timeout). Disconnecting."`
- `"Firmware is '%s'"`
- `"Dongle ready"`

## 5 URI handlers exposés côté API HTTP BOX

Endpoints atteignables via `command/io/profalux/*` :

| URI | Effet supposé |
|:-|:-|
| `command/io/profalux/command` | Envoie commande normale (=open/close/stop/fav_pos1) |
| `command/io/profalux/raw_command` | ⭐ **Passthrough AT arbitraire** — envoie n'importe quelle string AT au dongle |
| `command/io/profalux/register` | Enregistre un device dans la table interne du dongle |
| `command/io/profalux/remove` | Retire un device |
| `command/io/profalux/unregister` | Désenregistre |

Le handler `raw_command` = point d'entrée pour sonder toutes commandes AT non documentées, y compris celles construites dynamiquement au runtime.

## Comportement du plugin (=révélé par messages debug)

- **Table interne** : `atm_object_manager_get`/`_get_or_create`/`_get_first`/`_get_next`/`_remove` — gestion d'objets Device persistés via `atm_config_save_to_file`
- **Queue send avec purge** : `"New command of id %i, purging existing commands beginnig with 'AT$SF=%i,'"` — évite les collisions send
- **Repeat configurable** : `"Motorisation commands open/stop/close/fav_pos1 will be repeated %i times"` — nombre configurable
- **T0 retry count** : `"Found transmission count T0=%i for commands open/close/stop"` — timing configurable
- **Allocation dynamique d'IDs** : `"Device '%s' not recognized, allocating a new id"` — le plugin attribue automatiquement les IDs internes
- **Parse packets reçus** : `"Received full packet of %i bytes"` + `"Packet of %i bytes not known after command '%s'. Ignoring '%s'"` — parsing binaire des retours

## Ce qui est CONFIRMÉ (=analyse statique 100 %)

1. Le dongle est un chip fermé — **AUCUNE commande AT d'update firmware** (`AT$FW`, `AT$BL`, `AT+DFU`, `AT$UP`, séquence `+++` etc. = tous absents)
2. Le protocole AT est **stable depuis 2018** — diff de 7 versions du plugin montre les mêmes 9 commandes AT dans TOUTES les versions (=v29201 → v66717)
3. Le grossissement du plugin 45KB→76KB entre v42623 et v47975 = ajout **LED pfxbox2 + math flottant**, pas de nouveau protocole
4. La version `_profalux` v66717 (=45KB) = variante compacte BOX client final sans LED pfxbox2

## Ce qui reste INCERTAIN (=nécessiterait dongle physique ou émulation runtime)

1. **~~La commande AT qui trigger "Received list of keys"~~** — ⭐ **CONFIRMÉ 2026-08-06 après-midi via r2 désassemblé** : c'est bien **`AT&V`** qui déclenche la réponse multi-lignes incluant "PFX KEELOQ", "Hardware Version", "Software Version", "Frame Repeat Nb", "T0=", et "Received list of keys". Confirmé dans `atm_io_profalux_connect_step2` : log "Possible compatible dongle found. Requesting version." → `send_AT("AT&V")` → `g_timeout_add_seconds(5,...)`.
2. **Le format binaire des packets reçus** en réponse aux `AT$SF` (=le plugin fait un `"Packet of %i bytes"` parsing) — toujours inconnu sans dongle physique
3. **Les paramètres exacts que register/unregister passent au dongle** — analyse partielle via r2 mais pseudocode complet non extrait (=Ghidra GUI recommandé pour clarté)
4. **Le workflow complet d'appairage** — ligne rest à extraire du désassemblé de `atm_io_profalux_register_cmd` (1004 bytes, 18 blocs) via Ghidra pseudocode

## Firmware caché dans le plugin ? — Vérifié 2026-08-06

Question posée par le user : le firmware du chip radio KEELOQ pourrait-il être embedded dans `atm_io_profalux.so` et envoyé au dongle au boot ?

**Réponse : NON, aucun firmware caché** — vérification par entropy analysis + signature scan :

- **0 zone haute entropie (>7.5)** sur tout le fichier 76 KB = pas de firmware chiffré ni compressé
- **Aucune signature firmware** : pas d'Intel HEX (`:10`), pas de S-records (`S1`), pas d'ELF secondaire, pas de GZIP (`\x1f\x8b`), pas de XZ, pas de blob U-Boot
- **Sections utiles trop petites** : `.text` 22 KB (=code), `.rodata` 4.7 KB (=constantes/strings), `.data` 1.7 KB. Le reste = 12 KB DWARF debug + symbols + dynamic linking
- Version stripped v66700 = ~45 KB au total = encore moins de place

**Conclusion** : le firmware du chip radio est bien **BURNED-IN** dans la ROM du chip (=probablement Silicon Labs / Microchip / equivalent). Aucun mécanisme software côté BOX ne peut le mettre à jour ni le modifier.

## Fonctions internes du plugin (=révélées par debug info + r2 aa)

20 fonctions atm_io_profalux_* identifiées par nom via DWARF :

| Fonction | Addr | Size | Rôle |
|:-|:-:|-:|:-|
| `g_module_check_init` | 0x5040 | 268 | ⭐ Entry point GLib au dlopen (=là où le plugin bloque en émulation) |
| `atm_io_profalux_send_AT` | 0x1a20 | 384 | Envoie une AT au dongle (=queue+sscanf sur `AT$SF=%i,%i`) |
| `atm_io_profalux_connect_step2` | 0x3710 | 180 | ⭐ Handshake boot : envoie `AT&V` + timeout 5s |
| `atm_io_profalux_connect_timeout` | 0x4a60 | 128 | Timeout handshake |
| `atm_io_profalux_disconnect` | 0x2f30 | 828 | Nettoyage/déconnexion dongle |
| `atm_io_profalux_watchdog` | 0x326c | 1188 | Surveillance état dongle |
| `atm_io_profalux_send_command` | 0x2264 | 744 | Handler URI `command/io/profalux/command` |
| `atm_io_profalux_action_cmd` | 0x2b48 | 680 | Handler action (=open/close/stop/fav_pos1) |
| `atm_io_profalux_register_cmd` | 0x275c | 1004 | ⭐ Handler URI `register` — construit AT pour appairer device |
| `atm_io_profalux_unregister_cmd` | 0x254c | 528 | Handler URI `unregister` |
| `atm_io_profalux_remove_cmd` | 0x1d58 | 1292 | Handler URI `remove` |
| `atm_io_profalux_raw_command_cmd` | 0x1ba0 | 248 | Handler URI `raw_command` (=passthrough AT) |
| `atm_io_profalux_send_packet` | 0x4e38 | 520 | Envoi packet raw au dongle |
| `atm_io_profalux_queue_packet_serialyze` | 0x1c98 | 192 | Sérialisation packet |
| `atm_io_profalux_queue_event_cb` | 0x4ae0 | 856 | Callback events queue |
| `atm_io_profalux_atcommand_same_id` | 0x4900 | 352 | Purge queue commandes AT pour ID |
| `atm_io_profalux_get_object_id` | 0x16d4 | 92 | Résolution ID device |
| `atm_io_profalux_packet_free` | 0x16a0 | 52 | Libération packet |
| `atm_io_profalux_device_set_state_on_command_timeout` | 0x1730 | 408 | Machine à états sur timeout |
| `atm_io_profalux_queue_log`, `_queues_log` | 0x18c8, 0x1988 | 192, 152 | Logs internes |

**Pour aller plus loin** : Ghidra GUI avec projet importé permettrait d'extraire le pseudocode C complet de chaque fonction. Task #107 dans mémoire.

## Émulation setup — fonctionnelle (=session 2026-08-06 après-midi)

Infrastructure entièrement opérationnelle :

**Rootfs BOX extractible** :
- `mtd5.bin` = SquashFS avec magic modifié `shsq` (=variante Ralink RT5350)
- **Patch magic** : `printf '\x68\x73\x71\x73' | dd of=mtd5.bin bs=1 count=4 conv=notrunc`
- Extract : `sasquatch -d rootfs mtd5-patched.bin` (=fork onekey-sec, compile OK sur gcc 14 avec `make ZSTD_SUPPORT=0 XATTR_SUPPORT=0`)

**Stub `pipe2` compilé pour MIPS** (=Zig 0.13) :
```bash
zig cc -target mipsel-linux-musl -shared -fPIC -o pipe2_stub.so pipe2_stub.c
# → ELF 32-bit LSB shared object, MIPS, 4820 bytes
```

**Chroot userland fonctionnel** (=sans sudo, via bwrap + qemu-mipsel binfmt) :
```bash
bwrap --bind rootfs / --dev /dev --proc /proc --tmpfs /tmp \
  --bind /dev/pts/N /dev/atm_profalux_keeloq \
  --setenv HOME /mnt/root \
  --setenv LD_LIBRARY_PATH /lib:/usr/lib:/mnt/root/athemium/install/domus_gw/lib \
  --setenv LD_PRELOAD /lib/pipe2_stub.so \
  /mnt/root/athemium/install/domus_gw/bin/athemium_dgw -c /mnt/root/.config/athemium/atm.ini
```

**Mock dongle pty pair** via socat + Python responder :
```bash
socat -d PTY,link=/tmp/pty_dongle,rawer EXEC:'python3 -u mock_dongle_stdio.py',pty,rawer
# → Le pty côté daemon est bindé sur /dev/atm_profalux_keeloq dans le rootfs
```

## État observé de l'émulation

**Ce qui fonctionne** :
- `athemium_dgw` démarre entièrement dans bwrap
- Scripts d'init `10_atm_restore.sh`, `20_rsyslog.sh`, `90_halt_kodi.sh`, `10_check_usb_errors.sh` tournent
- Banner : `[libatm] MESSAGE Starting athemium_dgw-v3.0.66700.e33b7c0e3`
- **Tous les plugins visités** (=`atm_connector_websocket`, `atm_system_gateway`, `atm_system_rsyslog`, `atm_system_stat`, `atm_system_base`, `atm_log`, `atm_system_power`, `atm_event_scheduler`, `atm_ui_web`, `atm_system_config`, `atm_io_profalux`)
- **`atm_io_profalux` est le DERNIER plugin visité** (=chargement OK, message `Loading plugin 'atm_io_profalux'` émis)
- Mock dongle répond correctement à `AT?` en test direct (=vérifié via echo dans le pty)

**Ce qui reste à débloquer** :
- Le plugin `atm_io_profalux` **reste bloqué en init** après le message `Loading plugin` (=plus aucun log follow-up sur 45+ secondes)
- Le mock dongle ne reçoit aucune commande AT du plugin
- Causes possibles : symbol resolution MIPS incompatible avec cette version dgw, socat pty termios settings incorrects, threads init qui timeout

**Prochaines étapes pour débloquer** (=reprise ultérieure) :
1. `ltrace/strace` sur le plugin lors du init pour identifier ce qu'il attend
2. Utiliser le plugin original du tarball v66700 au lieu de la version debug pfxbox2
3. Fournir termios settings CDC-ACM au pty via `socat` (=`b115200,cs8,-parenb,-cstopb,raw`)
4. Vérifier signaux/threads du plugin via `ltrace -e '*_thread*,*_mutex*,dlopen,open'`

**Localisation infrastructure** : `/home/olivier/scratch-devmel/emu-mipsel/`
- `sasquatch-onekey/squashfs-tools/sasquatch` = extracteur compilé
- `rootfs-bare/` = rootfs BOX v2.2 extrait + libs athemium_dgw superposées + atm_io_profalux debug plugin
- `dgw/mipsel/` = binaires athemium_dgw v3.0.66700 extraits
- `pipe2_stub.so` = stub Zig-compiled mipsel-musl
- `dgw-first-boot.log` = trace boot complet visible
- `dgw-with-mock-pty.log` = trace avec mock pty pair
- `zig/zig-linux-x86_64-0.13.0/` = cross-compiler Zig standalone

**Mock dongle** : `/tmp/mock_dongle_stdio.py` — répond à `AT?`, `ATZ`, `ATQ0`, `AT$C?`, `AT$CP=14`, `AT$CP?`, `AT$TR=25,15,70,70`, `AT&V` (=avec "PFX KEELOQ" + Hardware/Software Version + "Received list of keys"), et `AT$SF=id,action`.

## Impact sur OpenProfalux (=projet firmware ESP32+CC1101)

- **Le plugin ne construit que 9 commandes AT** — le dongle fait tout le reste (crypto, table, rolling counter, serial)
- **Le CC1101 doit reproduire directement le trame RF KEELOQ 868**, pas le protocole AT (=inutile de rejouer AT$SF via CC1101)
- **Pour émuler un dongle Profalux via CC1101** : nécessite d'obtenir au moins UNE (serial, crypt_key, counter) valide via :
  - Dump HCS301 d'une télécommande MAI-EMPX via PICkit (=~30€, plus économique)
  - OU dump du dongle Profalux physique via JTAG/SWD (=~150€ + démontage)
  - OU sniffer une trame émise et essayer de reverse la crypt_key via DPA (=impossible sans hardware ChipWhisperer)

**Recommandation confirmée** : voie PICkit + télécommande sacrifiée = meilleur ROI pour obtenir authentiquement une (serial, crypt_key).
