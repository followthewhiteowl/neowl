# Encodage et entrées/sorties

## 1. L'idée en deux minutes

Un texte n'a pas d'encodage. C'est le point de départ, et il évite le
vieux casse-tête « ANSI ou Unicode ? » ainsi que les accents cassés
(mojibake).

- Une `chaîne` OWL est **neutre** : c'est du texte, sans encodage.
  Comme c'est du texte, `Taille` compte des caractères —
  `Taille("café")` vaut 4.
- Un `buffer`, c'est une séquence d'**octets** bruts.
- L'encodage (UTF-8, Latin-1, Windows-1252…) est une propriété des
  **octets**, pas du texte. Il se règle **aux frontières**, au moment
  où l'on passe d'une `chaîne` à un `buffer` ou à un fichier — et jamais
  ailleurs. C'est ça, l'essentiel.

OWL ne devine jamais l'encodage de ce qui entre : il faut l'indiquer.
En contrepartie, le comportement est identique sur Windows, Linux et
macOS, sans dépendre d'un réglage régional de la machine.

---

## 2. Convertir aux frontières

Deux natives forment le pont entre texte et octets, chacune paramétrée
par un encodage :

- `ChaîneVersBuffer(s, encodage)` : encode une `chaîne` en `buffer`
  d'octets.
- `BufferVersChaîne(buf, encodage)` : décode un `buffer` d'octets en
  `chaîne`.

Encoder puis redécoder avec le **même** encodage restitue le texte de
départ. C'est là, et seulement là, que les octets apparaissent :

```owl
// le même texte occupe un nombre d'octets variable selon l'encodage
buf est un buffer = ChaîneVersBuffer("café", encWindows1252)
Trace(Taille(buf))                          // 4  — 'é' tient sur 1 octet ici
Trace(Taille(ChaîneVersBuffer("café", encUTF8)))   // 5  — 'é' occupe 2 octets en UTF-8

sRendu est une chaîne = BufferVersChaîne(buf, encWindows1252)
Trace(sRendu)                               // café
Trace(sRendu = "café")                      // 1 (Vrai)
```

Même principe côté fichier avec `fChargeTexte` / `fSauveTexte` (et
`fAjouteTexte`), qui acceptent le même argument d'encodage :

```owl
chemin est une chaîne = "_demo.txt"
fSauveTexte(chemin, "déjà vu", encLatin1)
Trace(fChargeTexte(chemin, encLatin1))      // déjà vu
```

L'encodage est **optionnel** et vaut `encUTF8` par défaut : sans
argument, les octets internes passent tels quels.

Quelques encodages courants : `encUTF8` (défaut), `encLatin1`,
`encWindows1252`, `encUTF16LE`. La liste complète et à jour est donnée
par la documentation de `owlEncodage`.

Un caractère absent de l'encodage cible est remplacé, à l'écriture, par
l'octet `63` (le caractère `?`) :

```owl
// '€' n'existe pas en Latin-1
buf est un buffer = ChaîneVersBuffer("euro €", encLatin1)
Trace(BufferVersChaîne(buf, encLatin1))     // euro ?
```

---

## 3. Format n'est pas charset

Base64, percent-encoding d'URL ou hexadécimal sont des encodages de
**format** : ils ré-encodent des octets déjà déterminés, sans rapport
avec le charset d'un texte. Ils vivent dans des natives distinctes —
`ChaîneVersBase64`, `URLEncode`, `ChaîneVersHexa` — et ne se mélangent
pas avec le choix d'un `owlEncodage`. Voir la documentation de
`URLEncode` et `ChaîneVersBase64`.

Le Base64 existe en deux alphabets, choisis par le second paramètre de
`ChaîneVersBase64` et de `Base64VersChaîne` :

- `encodeBASE64` (défaut) — alphabet standard, avec `+`, `/` et le
  caractère de remplissage `=` ;
- `encodeBASE64URL` — alphabet compatible URL, avec `-` et `_`, et sans
  remplissage. C'est la forme à employer pour un jeton transporté dans
  une URL ou un en-tête.

```owl
octets est un buffer = HasardSécuriséOctets(32)
jeton est une chaîne = ChaîneVersBase64(octets, encodeBASE64URL)
relu est un buffer = Base64VersChaîne(jeton, encodeBASE64URL)
Trace(relu = octets)                         // 1 (Vrai) — aller-retour sans perte
```

Les deux alphabets ne se mélangent pas : soumettre un `+` au décodage
`encodeBASE64URL`, ou un `-` au décodage standard, produit une erreur et
une chaîne vide plutôt qu'un résultat approximatif. Une longueur
invalide, un caractère hors alphabet ou un remplissage mal placé sont
refusés de la même façon ; l'erreur se lit avec `ErreurDétectée`.

---

## 4. Fins de ligne

Dans un fichier `.owl`, **une fin de ligne est une fin de ligne**. Le
fichier peut être enregistré à la manière Unix (LF) ou à la manière
Windows (CRLF), et même mélanger les deux : il est accepté sans
avertissement, et la valeur d'une chaîne multiligne est la même dans
tous les cas. Ce qui est lu à l'écran est ce que le programme voit.
La règle vaut pour les deux formes multilignes, backticks et crochets.

```owl
extrait est une chaîne = `ligne 1
ligne 2`
Trace(Taille(extrait))                       // 15 — quel que soit l'enregistrement du fichier
Trace(extrait = "ligne 1" + LF + "ligne 2")  // 1 (Vrai)
```

**Les données ne sont jamais retouchées.** Cette règle ne concerne que
le texte du programme, lu une seule fois à la compilation. Un texte
saisi, un fichier lu, une réponse réseau, une ligne de base de données
gardent leurs octets exacts, retour chariot compris :

```owl
chemin est une chaîne = "_demo_eol.txt"
contenu est une chaîne = "ligne 1" + CRLF + "ligne 2"
fSauveTexte(chemin, contenu)
relu est une chaîne = fChargeTexte(chemin)
Trace(Taille(relu))                          // 16 — le retour chariot est intact
Trace(relu = contenu)                        // 1 (Vrai)
```

Quand un format ou un protocole exige une fin de ligne précise, elle
s'écrit, sous la forme d'une constante :

| Constante | Valeur |
|---|---|
| `LF` | saut de ligne seul — la fin de ligne d'OWL |
| `CRLF` | retour chariot suivi d'un saut de ligne (CR+LF) |

Pour l'octet de retour chariot seul, sans saut de ligne, il n'existe pas
de constante : il s'écrit `Caract(13)`.

La conversion d'un texte déjà constitué, en une seule opération, passe
par `Remplace(texte, LF, CRLF)`.

---

## Pour aller plus loin

| Sujet | Référence |
|---|---|
| Système de types, `chaîne`, travailler au niveau de l'octet | [`01_types_et_coercions.md`](01_types_et_coercions.md) |
| Le buffer comme conteneur binaire (marshalling, I/O) | [`27_buffers.md`](27_buffers.md) |
| Multilangue et tags de langue | [`21_internationalisation.md`](21_internationalisation.md) |
| Conventions de code | [`10_conventions_de_code.md`](10_conventions_de_code.md) |

Pour la signature exacte d'une native ou les valeurs d'un énum, se
reporter à la documentation de référence du symbole.
