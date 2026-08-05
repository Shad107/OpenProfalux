# Build & flash — OpenProfalux

## Setup (une fois)

```bash
cd /home/olivier/projects/openprofalux
docker compose build
```

## Build firmware

```bash
docker compose run --rm esp-idf bash -c "\
    cd /project/firmware && \
    idf.py set-target esp32 && \
    idf.py build \
"
```

## Flash + monitor série

```bash
docker compose run --rm esp-idf bash -c "\
    cd /project/firmware && \
    idf.py -p /dev/ttyUSB0 flash monitor \
"
```

## Setup MQTT côté HA (pré-requis)

```yaml
# configuration.yaml (=ou via Mosquitto broker addon HA)
mqtt:
  broker: 192.168.1.x
  discovery: true
  discovery_prefix: homeassistant
```

## Config initiale ESP32 (=au premier boot)

1. ESP32 démarre → boot log verbose via UART
2. Aucun WiFi configuré → démarre SoftAP `OpenProfalux-Setup` (mdp `openprofalux`)
3. Se connecter au SoftAP + ouvrir http://192.168.4.1
4. Configurer :
   - Device name (=ex `volet_chambre_invitee`)
   - WiFi SSID + password
   - MQTT broker URI (=`mqtt://192.168.1.x:1883`)
5. ESP32 reboot → connect WiFi → connect MQTT → HA discovery auto

## Debug depuis mon côté (=via MQTT)

```bash
# Subscribe à tous les logs et frames émises
mosquitto_sub -h 192.168.1.x -v -t 'openprofalux/#'

# Trigger un pair
mosquitto_pub -h 192.168.1.x -t 'openprofalux/volet_test/pair' -m '{}'

# Voir le state
mosquitto_sub -h 192.168.1.x -t 'openprofalux/volet_test/state'

# Envoyer commande
mosquitto_pub -h 192.168.1.x -t 'openprofalux/volet_test/cmd' -m '{"btn":"UP"}'

# Capture RX pour analyse trames télécommande d'origine
mosquitto_pub -h 192.168.1.x -t 'openprofalux/listen/start' -m ''
mosquitto_sub -h 192.168.1.x -t 'openprofalux/listen/frame' -v
# ... presser bouton sur télécommande d'origine, voir les trames captured ici ...
mosquitto_pub -h 192.168.1.x -t 'openprofalux/listen/stop' -m ''
```

## Logs disponibles

Le firmware publie sur MQTT :
- `openprofalux/log` — logs de toutes activités (=`{"lvl":"info","msg":"..."}`)
- `openprofalux/{device}/state` — state actuel (=serial, counter, rssi, heap)
- `openprofalux/{device}/pair_result` — résultat pairing (=success/timeout)
- `openprofalux/listen/frame` — chaque trame RF captured en mode listen
- `openprofalux/system/status` — heap free, uptime, target

Depuis mon côté (=Claude), je peux :
1. Te demander de lancer `mosquitto_sub -h {ip} -v -t 'openprofalux/#' > /tmp/log.txt`
2. Puis toi tu me colles le contenu via chat
3. Je peux analyser les frames, counters, résultats

Ou si le broker MQTT est exposé sur le LAN (=via WiFi routable depuis outside), je peux idéalement lancer moi-même mosquitto_sub via Bash.

## Simulation PC (=déjà testable maintenant)

```bash
cd sim
make
./sim_openprofalux       # Full simulation avec verbose logs
./test_keeloq          # Vérification algo KEELOQ
```
