# Tableaux associatifs

Un tableau associatif range des valeurs sous une **clé de type chaîne**, au
lieu d'une position numérique comme un `tableau de <T>` classique. Chaque clé
désigne au plus une valeur.

## Déclaration

Le type des valeurs se précise après `de` :

```owl
scores est un tableau associatif de entiers
```

Ce type peut être un type fondamental (`chaîne`, `entier`, `réel`, `booléen`,
`monétaire`, `numérique`) ou un type composé (structure). Sans type précisé,
les valeurs sont hétérogènes :

```owl
ligne est un tableau associatif
```

## Utilisation

La clé se donne entre crochets, en écriture comme en lecture. Une clé absente
rend la valeur par défaut du type. `..Occurrence` donne le nombre d'entrées, et
`POUR TOUT` parcourt les valeurs :

```owl
scores est un tableau associatif de entiers

scores["alice"] = 10          // écrire sous une clé
scores["bob"]   = 7

Trace(scores["alice"])        // lire une clé      -> 10
Trace(scores["inconnu"])      // clé absente       -> 0 (défaut du type)
Trace(scores..Occurrence)     // nombre d'entrées  -> 2

POUR TOUT valeur DE scores    // parcours des valeurs
    Trace(valeur)             // -> 10 puis 7
FIN
```
