# Contre-analyse « 10e homme » — mécanisme Profalux / KeeLoq / AirSend

> Objectif : chercher activement les explications qui pourraient invalider ou affaiblir l’hypothèse principale du projet OpenProfalux, à savoir qu’un moteur Profalux accepterait l’enrôlement durable d’un nouvel émetteur KeeLoq sans que l’émetteur connaisse la manufacturer key Profalux.
>
> Cette note ne remplace pas la recherche existante. Elle sert de contradicteur méthodique afin d’éviter qu’une suite d’indices convergents soit prise trop tôt pour une preuve du mécanisme interne.

## 1. Ce que les recherches établissent solidement

Les éléments suivants sont fortement étayés par les binaires, API et analyses déjà présents dans ce dépôt :

1. DEVMEL possède une prise en charge spécifique de Profalux PFX, distincte du KeeLoq générique.
2. PFX est classé `clone=3` (= full), tandis que KLQ868 générique est classé `clone=2` (= temporary).
3. L’AirSend aboutit donc à un contrôle persistant de l’équipement PFX, et non à un simple replay ponctuel.
4. Aucun catalogue trivial de manufacturer keys Profalux n’a été identifié dans les couches applicatives étudiées.
5. Le blob DEVMEL de 221 Ko ne ressemble pas à une base de clés KeeLoq ; il correspond plutôt à des données et tables liées à XXTEA et aux événements internes.
6. Le plugin Athemium Profalux est essentiellement une couche de commandes AT vers un matériel radio distinct ; il n’exécute pas localement une cryptographie KeeLoq visible.
7. Les recherches négatives couvrent plusieurs couches indépendantes : SDK Android, service Linux, API, blob, plugin Athemium, firmwares et keystores communautaires.

Ces éléments permettent de conclure raisonnablement :

> DEVMEL ne fait pas un simple grab-and-replay KeeLoq générique pour PFX et ne semble pas stocker naïvement une master key Profalux dans les binaires applicatifs analysés.

Ils ne permettent pas encore de conclure comment la clé cryptographique utilisée pour les futures trames est obtenue.

---

## 2. Faiblesse centrale de l’hypothèse actuelle

Une trame normale HCS301 ne transmet pas la clé secrète de 64 bits.

Le HCS301 contient notamment :

- une crypt key de 64 bits ;
- un numéro de série ;
- un compteur de synchronisation ;
- des bits de discrimination ;
- éventuellement une seed de Secure Learn ;
- des bits de configuration.

La trame radio normale contient principalement :

- un hopping code chiffré de 32 bits ;
- un numéro de série de 28 bits ;
- les boutons et bits d’état.

Ainsi, si un nouvel émetteur choisit :

```text
serial = S
key = K
counter = C
```

et émet :

```text
ciphertext = KeeLoqEncrypt(K, plaintext(C, discrimination, boutons))
```

le moteur reçoit `S` et le ciphertext, mais pas `K`.

Il ne peut pas déduire directement une clé unique de 64 bits à partir d’un seul bloc chiffré de 32 bits. Même avec une partie du plaintext connue, de nombreuses clés restent compatibles.

Par conséquent, le modèle suivant est techniquement insuffisant :

```text
le moteur reçoit (serial, encrypted_hop)
puis stocke directement (serial, crypt_key)
```

Le moteur peut stocker le serial et le hopping code reçu, mais il lui manque toujours une méthode pour connaître ou établir la vraie clé utilisée par l’émetteur.

---

## 3. La seed HCS301 ne suffit pas seule

Dans le Secure Learn KeeLoq standard, la seed n’est pas directement la device key.

Le modèle attendu est plutôt :

```text
device_key = Derive(manufacturer_key, seed et/ou serial)
```

L’émetteur est programmé en usine avec la device key. Pendant l’apprentissage, il transmet sa seed et son serial. Le récepteur, qui contient la manufacturer key, recalcule la même device key et vérifie ensuite plusieurs transmissions.

Conséquence importante :

> Une procédure appelée « Secure Self-Learn » ne prouve pas l’absence de manufacturer key. Dans les documents Microchip/Chamberlain classiques, cette manufacturer key reste présente côté récepteur.

Il faut donc éviter de présenter le brevet Chamberlain Self-Learn comme preuve directe d’un apprentissage de clé arbitraire sans secret fabricant. Il décrit plutôt un moyen pour le récepteur de reconstruire la clé à partir d’un secret partagé et d’informations transmises durant le learn.

---

## 4. Ce que `clone=3` prouve — et ne prouve pas

### Ce que `clone=3` prouve probablement

- le résultat fonctionnel est persistant ;
- DEVMEL dispose d’une procédure dédiée PFX ;
- cette procédure va au-delà d’un replay temporaire ;
- un état durable est créé côté AirSend et/ou côté moteur.

### Ce que `clone=3` ne prouve pas

- que le serial original est cloné ;
- que la clé originale est récupérée ;
- qu’un nouvel émetteur est créé ;
- que le moteur accepte n’importe quelle clé ;
- que DEVMEL ne connaît pas la manufacturer key ;
- que le mécanisme est celui du brevet Chamberlain ;
- que la cryptographie a lieu dans les binaires déjà analysés.

`clone` est une catégorie fonctionnelle interne à DEVMEL, pas une terminologie cryptographique normalisée.

---

## 5. Ce que `getDecoder=0` prouve — et ne prouve pas

L’inférence actuelle :

```text
getDecoder = 0
=> DEVMEL ne sait pas déchiffrer PFX
=> DEVMEL ne possède pas la manufacturer key
```

est plausible mais non démontrée.

`getDecoder=0` peut également signifier :

- aucun décodage exposé à l’API ;
- aucun mode sniffer utilisateur ;
- pas de conversion RX en événements applicatifs ;
- protocole volontairement TX-only ;
- décodage sans utilité commerciale ;
- absence de gestion générique d’émetteurs inconnus ;
- limitation du front-end plutôt qu’incapacité cryptographique.

Même avec une manufacturer key, un décodeur universel doit encore connaître ou gérer :

- le mode de dérivation ;
- la seed éventuelle ;
- le serial ;
- la discrimination ;
- l’état du compteur ;
- la sélection de la bonne identité.

Conclusion :

> `getDecoder=0` est compatible avec l’absence de manufacturer key, mais ne la démontre pas.

---

## 6. Hypothèses adverses encore compatibles avec les preuves

### Hypothèse A — Capacité cryptographique dans le firmware radio AirSend

Les couches analysées peuvent ne pas être celles qui produisent réellement la trame RF.

Le matériel AirSend peut comporter :

- un microcontrôleur radio distinct ;
- une zone flash non incluse dans les paquets Linux/Android ;
- un secure element ;
- une routine KeeLoq dans un firmware non extrait ;
- un coprocesseur ou module préprogrammé.

Le service Linux pourrait seulement transmettre :

```text
channel=PFX
source=...
seed=...
counter=...
action=UP
```

puis laisser le firmware radio calculer ou récupérer la vraie trame.

**Force de l’hypothèse : élevée tant que le firmware radio complet n’est pas extrait.**

---

### Hypothèse B — Identités Profalux préprovisionnées

DEVMEL n’a pas besoin d’intégrer la manufacturer key dans chaque AirSend.

Il peut provisionner en usine des couples :

```text
serial_i
device_key_i
seed_i
```

Lors de l’installation :

1. la télécommande d’origine ouvre le mode learn ;
2. l’AirSend choisit une identité PFX disponible ;
3. il envoie sa seed/son serial ;
4. le moteur dérive ou vérifie la même device key ;
5. l’AirSend utilise ensuite la clé déjà stockée.

Cette hypothèse explique :

- l’absence de master key visible ;
- le contrôle persistant ;
- la nécessité de la télécommande d’origine ;
- l’absence de dérivation observable dans les couches applicatives.

**Indices à rechercher :**

- plage de serials structurée ;
- nombre maximal d’identités PFX ;
- serial stable après reset usine ;
- réutilisation des mêmes identités ;
- différences entre deux AirSend neufs.

---

### Hypothèse C — Provisionnement ou calcul par le cloud

Le service distant peut fournir :

- une identité déjà calculée ;
- un blob opaque ;
- une device key enveloppée ;
- un token destiné au firmware radio ;
- un stock d’identités téléchargé lors de l’activation.

L’absence de clé dans les binaires locaux resterait alors normale.

**Test nécessaire :**

- reset usine ;
- aucun accès Internet depuis le premier démarrage ;
- DNS et trafic réseau contrôlés ;
- création d’un canal PFX ;
- redémarrage ;
- vérification du fonctionnement permanent.

Attention : un appareil déjà activé peut avoir reçu auparavant un stock d’identités.

---

### Hypothèse D — Dérivation Profalux faible ou publique

Profalux peut programmer les HCS301 avec une règle non standard :

```text
K = f(serial)
```

ou :

```text
K = f(serial, seed, constante fixe)
```

Dans ce scénario, aucune master key secrète n’est nécessaire à DEVMEL.

Cela reste compatible avec l’utilisation d’un HCS301 : le composant n’impose pas la manière dont la crypt key est choisie lors de sa programmation.

Les variantes possibles comprennent :

- clé fixe par gamme ;
- transformation simple du serial ;
- manufacturer key commune Stella Group ;
- mode Normal Learn mal configuré ;
- discrimination faible ;
- relation seed/clé reproductible publiquement.

Cette hypothèse est favorable au projet OpenProfalux, mais elle doit être distinguée d’un « apprentissage arbitraire ».

---

### Hypothèse E — Protocole propriétaire transmettant réellement la clé

Le moteur peut avoir un firmware spécifique qui, pendant une fenêtre administrative ouverte par la télécommande originale, accepte une séquence non standard :

```text
START_LEARN
SERIAL
KEY_PART_1
KEY_PART_2
COUNTER
COMMIT
```

Puis les communications normales utilisent KeeLoq.

Cela expliquerait parfaitement comment le moteur connaît la clé arbitraire choisie par l’AirSend.

Difficulté : un HCS301 standard n’est pas un microcontrôleur RF librement programmable et ne transmet pas normalement une clé de 64 bits.

Il faut donc qu’au moins l’un de ces points soit vrai :

- la télécommande Noé contient un MCU supplémentaire ;
- le mode a été prévu pour un outil installateur/usine ;
- l’AirSend exploite une commande propriétaire ou une vulnérabilité ;
- le moteur accepte un format hors HCS301 ;
- le composant télécommande n’est pas exactement celui supposé dans toutes les variantes.

**Cette hypothèse devient forte si la séquence AirSend contient des trames plus longues ou différentes du format 66 bits HCS301.**

---

### Hypothèse F — Cassage dynamique de la clé KeeLoq

KeeLoq est cassé au sens académique et matériel, mais cela ne signifie pas qu’une poignée de trames RF ordinaires suffit à retrouver immédiatement la clé.

Il faut distinguer :

1. algorithme public ;
2. cryptanalyse réduisant le coût théorique ;
3. attaques side-channel avec accès physique ;
4. clonage RF immédiat à partir de quelques appuis.

Les attaques pratiques connues reposent surtout sur :

- des traces de consommation ;
- un accès physique au HCS301 ou au récepteur ;
- du matériel spécialisé ;
- des volumes de données ou calculs incompatibles avec un assistant rapide.

Les recherches DEVMEL ne montrent pas de moteur cryptanalytique correspondant.

**Probabilité faible, sauf mécanisme privé non publié.**

---

## 7. Contradiction structurelle à résoudre

Si le moteur accepte réellement une clé arbitraire pendant le learn, une question demeure :

> Pourquoi utiliser des HCS301 préprogrammés dans une chaîne de fabrication complexe si le moteur peut accepter n’importe quelle clé locale ?

Une réponse cohérente est possible :

- l’ancienne télécommande authentifie l’ouverture du learn ;
- la nouvelle clé est ensuite transférée au moteur ;
- KeeLoq protège les usages normaux ;
- aucune manufacturer key globale n’est nécessaire ;
- chaque télécommande a une clé aléatoire individuelle.

Mais cette réponse exige toujours de trouver **le canal de transfert ou d’établissement de la clé**.

C’est le principal trou restant dans le modèle.

---

## 8. Programme expérimental discriminant

### Test 1 — AirSend hors ligne après reset usine

But : éliminer le cloud immédiat.

Procédure :

1. reset usine réel ;
2. réseau isolé sans Internet ;
3. capture DNS/TCP/UDP complète ;
4. création d’un canal PFX ;
5. redémarrage ;
6. test persistant du volet.

Interprétation :

- succès => cloud immédiat très improbable ;
- échec => provisionnement distant plausible.

---

### Test 2 — Comparer le serial original et le serial AirSend

Capturer :

- serial de la télécommande d’origine ;
- serial des transmissions AirSend ;
- counter apparent ;
- boutons ;
- seed éventuelle ;
- timings.

Interprétation :

- même serial => clonage/émulation de l’identité existante ;
- nouveau serial => nouvel enrôlement ;
- plage fixe ou structurée => préprovisionnement probable.

---

### Test 3 — Deux AirSend sur le même volet

But : détecter un stock d’identités.

Comparer :

- serials attribués ;
- stabilité après reset ;
- limites du nombre de canaux ;
- réutilisation éventuelle des mêmes identités.

---

### Test 4 — Émetteur avec clé arbitraire connue

Créer :

```text
serial = valeur contrôlée
key = valeur aléatoire connue
seed = valeur contrôlée
counter = valeur contrôlée
```

Tester :

1. trame normale seule ;
2. seed learn standard ;
3. deux ou plusieurs trames consécutives ;
4. variantes de boutons ;
5. discrimination différente ;
6. répétitions conformes aux timings Profalux.

Un succès durable démontrerait que :

- soit le moteur accepte réellement une clé arbitraire ;
- soit la clé choisie satisfait accidentellement une règle de dérivation ;
- soit la validation KeeLoq est faible.

---

### Test 5 — Matrice de variation

| Essai | Serial | Seed | Clé | Compteur |
|---|---:|---:|---:|---:|
| 1 | A | X | K1 | 1 |
| 2 | A | X | K2 | 1 |
| 3 | A | Y | K1 | 1 |
| 4 | B | X | K1 | 1 |
| 5 | B | Y | K2 | 1 |

Interprétation :

- toutes les clés passent => apprentissage direct ou validation très faible ;
- une seule relation passe => dérivation attendue ;
- seul un serial particulier passe => identité préprovisionnée ou whitelist ;
- aucune ne passe => secret fabricant ou séquence propriétaire manquante.

---

### Test 6 — Capturer toute la séquence RF d’apprentissage AirSend

Observer :

- nombre de trames ;
- durée totale ;
- changements de modulation ou débit ;
- longueurs supérieures à 66 bits ;
- seed ;
- serial ;
- répétitions ;
- trames hors format HCS301 ;
- séquences avant et après le mouvement de confirmation du volet.

Une séquence contenant des trames hors format HCS301 soutiendrait fortement une procédure propriétaire de provisioning.

---

## 9. Classement provisoire des hypothèses

Ces valeurs ne sont pas des probabilités mesurées ; elles servent à hiérarchiser les pistes adverses tant que les tests discriminants ne sont pas exécutés.

| Hypothèse | Estimation provisoire |
|---|---:|
| Capacité Profalux dans un firmware radio ou composant non analysé | 30 % |
| Apprentissage propriétaire transmettant/établissant une clé | 25 % |
| Identités Profalux préprovisionnées | 20 % |
| Dérivation faible ou publique | 15 % |
| Provisionnement cloud | 7 % |
| Cassage RF dynamique de KeeLoq | 3 % |

Les deux scénarios directement favorables à une reproduction libre par ESP32 sont :

- apprentissage propriétaire sans secret fabricant ;
- dérivation faible/publique.

Ils représentent ensemble une hypothèse de travail significative, mais non prouvée.

---

## 10. Formulation scientifique recommandée

Formulation trop forte :

> Le fait que PFX soit `clone=3` prouve que Profalux n’utilise pas un décodeur KeeLoq standard et que le moteur apprend une clé arbitraire sans manufacturer key.

Formulation robuste :

> Les observations DEVMEL démontrent une procédure PFX persistante spécifique, distincte du clonage KeeLoq générique. L’absence de clé ou de dérivation visible dans les couches applicatives analysées est compatible avec un apprentissage propriétaire sans manufacturer key, mais ne permet pas encore de distinguer ce scénario d’une capacité cryptographique cachée, préprovisionnée ou distante située dans une couche radio non analysée.

---

## 11. Verdict du contradicteur

Les recherches actuelles ont probablement éliminé :

- le simple replay permanent ;
- la master key en clair dans les SDK étudiés ;
- une dérivation KeeLoq évidente dans les couches applicatives ;
- la crypto locale dans le plugin Athemium ;
- un cassage RF KeeLoq trivial à chaque appairage.

Elles n’ont pas encore éliminé :

- le firmware radio AirSend non extrait ;
- un secure element ou une zone flash protégée ;
- des identités PFX préprovisionnées ;
- une dérivation distante préalable ;
- une dérivation Profalux faible ;
- une séquence propriétaire de transfert de clé.

Le test décisif reste l’enrôlement d’un émetteur dont le serial, la seed, la clé et le compteur sont entièrement contrôlés, associé à une capture RF complète de la procédure AirSend.

> Tant que ce test n’est pas positif, l’hypothèse d’apprentissage arbitraire doit rester l’hypothèse principale du prototype, pas un fait établi.
