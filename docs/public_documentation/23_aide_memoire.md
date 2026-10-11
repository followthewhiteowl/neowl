# OWL — Aide-mémoire

Les constructions courantes dans leur forme canonique. Chaque exemple
compile tel quel et suit l'indentation du formatter (une tabulation par
niveau). Les noms de variables sont préfixés selon leur type (`sNom`,
`nX`, `tabNoms`, `oObjet`) pour éviter toute collision avec un type ou un
mot-clé.

---

## Variable

```owl
sNom est une chaîne = "Alice"
```

## Condition

Forme bloc, pour plusieurs instructions :

```owl
nX est un entier = 5
SI nX > 0 ALORS
	Trace("positif")
SINON
	Trace("negatif")
FIN
```

Forme en ligne, pour une seule instruction (pas de `FIN`) :

```owl
nX est un entier = 5
SI nX > 0 ALORS Trace("positif")
```

## Compteur

```owl
POUR i = 1 À 10
	Trace(i)
FIN
```

## Parcours de collection

```owl
tabNoms est un tableau de chaînes
POUR TOUT sElem DE tabNoms
	Trace(sElem)
FIN
```

## Procédure

```owl
PROCEDURE Calc(nVal est un entier) : entier
	nResultat est un entier = nVal * 2
	RENVOYER nResultat
FIN
```

## Classe

```owl
cType est une Classe
PUBLIC
	sChamp est une chaîne

	PROCEDURE Constructeur(nom est une chaîne)
		:sChamp = nom
	FIN
FIN
```

## Tableau

```owl
tabNoms est un tableau de chaînes
tabNoms.Ajoute("x")
nOcc est un entier = tabNoms..Occurrence
```

## JSON

```owl
jDoc est un JSON = ChaîneVersJSON("{}")
sValeur est une chaîne = "test"
jDoc.cle = sValeur
```

## Exception

```owl
QUAND EXCEPTION DANS
	Trace("essai")
FAIRE
	Trace("rattrape")
FIN
```

## Octet et buffer

```owl
bDonnees est un buffer = "ABC"
nOctet est un entier = bDonnees[[1]]            // lit l'octet 1 → 65
bDonnees[[1]] = 200                              // écrit l'octet 1 (stricte 0–255)
nClamp est un entier = EntierVersOctet(300, débordementBorne)   // 255
nXor est un entier = 0xFF || 0x0F                // OU exclusif → 240
```

---

Déboguer un programme (points d'arrêt, pas-à-pas, inspection) :
[`28_debogueur.md`](28_debogueur.md).
