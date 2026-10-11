# Buffers : le conteneur binaire

## 1. Le buffer, en bref

Un `buffer` est une **séquence d'octets bruts** : le conteneur de tout
ce qui est binaire (formats de fichiers, protocoles réseau, échanges avec
du code externe). Là où une `chaîne` porte du texte et un `entier` porte
un nombre, le `buffer` porte des octets sans interprétation.

Point de modèle mental utile pour l'interop bas niveau : **quand une
taille précise en octets est nécessaire, c'est le buffer qui la porte**,
jamais le type texte ni le type numérique. On pré-dimensionne un buffer
pour recevoir exactement _n_ octets (voir la
[section 3](#3-pré-dimensionner-un-buffer)), sans réintroduire d'entier
ou de chaîne à taille fixe dans le langage : le type reste simple à la
lecture, la contrainte de taille vit dans le conteneur d'octets.

Deux facettes du buffer sont documentées ailleurs et ne sont pas
reprises ici :

- l'**accès octet par octet** (opérateur `buffer[[i]]`, lecture et
  écriture d'un octet, modes de débordement, opérations bit à bit) est
  détaillé dans [`01_types_et_coercions.md`](01_types_et_coercions.md),
  section « Travailler au niveau de l'octet » ;
- la **conversion texte ⇄ octets** et le choix d'un encodage
  (`ChaîneVersBuffer` / `BufferVersChaîne`) sont détaillés dans
  [`26_encodage_et_io.md`](26_encodage_et_io.md).

Ce chapitre traite le niveau au-dessus : le buffer comme outil de
marshalling binaire et d'entrée/sortie.

---

## 2. Empaqueter des scalaires (marshalling)

Pour écrire un nombre sous forme d'octets à taille fixe, OWL fournit des
natives dédiées :

| Native | Rôle |
|---|---|
| `EntierVersBuffer(valeur, nbOctets, boutisme?)` | Entier → octets (1, 2, 4 ou 8), complément à deux |
| `BufferVersEntier(buffer, position, nbOctets, boutisme?, signé?)` | Octets → entier |
| `RéelVersBuffer(valeur, nbOctets, boutisme?)` | Réel → octets IEEE 754 (4 ou 8) |
| `BufferVersRéel(buffer, position, nbOctets, boutisme?)` | Octets → réel |

Le paramètre `boutisme` (énumération) fixe l'**ordre des octets** :
`boutismePetit` place le poids faible en premier (le défaut),
`boutismeGros` place le poids fort en premier. Cet ordre compte dès
qu'on échange des données avec l'extérieur : deux systèmes qui ne
s'accordent pas sur le boutisme lisent la même séquence d'octets comme
deux valeurs différentes. Il faut donc l'expliciter selon le format
attendu par l'autre bout.

Round-trip d'un entier à travers un buffer, dans les deux ordres :

```owl
buf est un buffer = EntierVersBuffer(1000, 4)   // petit-boutiste par défaut
Trace(Taille(buf))                               // 4
Trace(buf[[1]])                                  // 232  — 0xE8, poids faible en tête
n est un entier = BufferVersEntier(buf, 1, 4)
Trace(n)                                          // 1000

bufGros est un buffer = EntierVersBuffer(1000, 4, boutismeGros)
Trace(bufGros[[1]])                              // 0    — poids fort en tête
Trace(BufferVersEntier(bufGros, 1, 4, boutismeGros))   // 1000
```

Le même principe s'applique aux réels, au format IEEE 754 (4 octets en
simple précision, 8 en double) :

```owl
bufR est un buffer = RéelVersBuffer(1.5, 8)
Trace(Taille(bufR))                              // 8
Trace(BufferVersRéel(bufR, 1, 8))                // 1.5
```

Une taille non prise en charge (autre que 1/2/4/8 pour un entier, ou
4/8 pour un réel), une valeur qui ne tient pas dans la taille demandée,
ou une position hors des limites du buffer lèvent une exception.

---

## 3. Pré-dimensionner un buffer

`BufferVide(nbOctets)` réserve un buffer de _n_ octets, tous
initialisés à zéro. C'est le tampon de réception que l'on prépare
**avant** l'opération qui va le remplir — typiquement la réponse d'un
appel externe dont on connaît la taille maximale.

```owl
buf est un buffer = BufferVide(16)
Trace(Taille(buf))                               // 16
Trace(buf[[1]])                                  // 0
Trace(buf[[16]])                                 // 0
```

`BufferVide(0)` retourne un buffer vide valide ; un nombre d'octets
négatif lève une exception. C'est ici que se matérialise le compromis
évoqué en [section 1](#1-le-buffer-en-bref) : la taille fixe vit sur le
buffer, pas sur les scalaires.

---

## 4. Fichiers binaires

Lire ou écrire un buffer sur disque se fait avec trois natives
symétriques des versions texte :

| Native | Rôle |
|---|---|
| `fSauveBuffer(chemin, buffer)` | Écrit le buffer dans le fichier (écrase) |
| `fChargeBuffer(chemin, offset?, longueur?)` | Charge tout ou partie du fichier en buffer |
| `fAjouteBuffer(chemin, buffer)` | Ajoute le buffer à la fin du fichier |

Aucun encodage n'intervient : ce sont des octets bruts, écrits et relus
tels quels. Le chemin suit la résolution de projet habituelle ; un
chemin relatif est résolu par rapport au projet courant.

```owl
chemin est une chaîne = "_paquet.bin"
donnees est un buffer = EntierVersBuffer(305419896, 4, boutismeGros)
fSauveBuffer(chemin, donnees)

relu est un buffer = fChargeBuffer(chemin)
Trace(Taille(relu))                              // 4
Trace(BufferVersEntier(relu, 1, 4, boutismeGros))   // 305419896
fSupprime(chemin)
```

---

## 5. Le buffer comme résultat

Le buffer n'est pas qu'un tampon que l'on remplit à la main : plusieurs
familles de natives **produisent** un buffer en sortie. Le buffer est le
format d'échange commun de ces opérations binaires (leur détail vit dans
leur propre documentation) :

- **sérialisation** : `Sérialise` transforme une structure en buffer ;
- **chiffrement** : `CrypteStandard` produit un buffer chiffré ;
- **compression** : `Compresse` renvoie un buffer compressé ;
- **archives zip** : elles échangent avec des buffers — extraction en
  mémoire (`zipExtraitFichier`) et ajout d'un buffer (`zipAjouteFichier`),
  en sélectionnant le mode « en mémoire » via la constante `zipEnMémoire` ;
  voir la documentation zip.

Une fois obtenu, ce buffer se manipule comme n'importe quel autre : on
le sauve sur disque, on le transmet, on lit ses octets. Se reporter à la
documentation de chaque native pour les paramètres et le format exact.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Accès octet par octet, `buffer[[i]]`, débordement, bit à bit | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Conversion texte ⇄ octets, choix d'un encodage | [`26_encodage_et_io.md`](26_encodage_et_io.md) |
| Conventions de code | [`10_conventions_de_code.md`](10_conventions_de_code.md) |
