# Débogueur

## 1. Le débogueur OWL, en bref

OWL fournit un débogueur dans sa toolchain, sous la forme d'un binaire
autonome — `owl-dap` — livré à côté d'`owl.exe`. Ce binaire implémente
le **DAP** (Debug Adapter Protocol), un protocole standard d'échange
entre un débogueur et un éditeur.

Conséquence directe : le protocole DAP rend le débogueur **universel** —
`owl-dap` se branche sur n'importe quel client compatible DAP, qui pilote
la session (poser un point d'arrêt, avancer, inspecter) sans que le
débogueur lui-même connaisse l'éditeur. Le débogueur, c'est `owl-dap` ;
l'éditeur n'est qu'une façade.

À ce jour, l'éditeur activement pris en charge est **Visual Studio Code**,
dont le plugin correspondant fournit l'intégration prête à l'emploi (voir
l'encadré du §4 et la documentation de l'extension OWL). Le protocole
n'exclut pour autant aucun autre client compatible DAP.

---

## 2. Ce que le débogueur permet

Le débogueur suspend l'exécution et donne la main pour observer et piloter
le programme. Concrètement, `owl-dap` propose :

- **Points d'arrêt** : suspendre l'exécution à une ligne donnée.
- **Pas-à-pas complet**, une fois la session suspendue :
  - **reprendre** l'exécution jusqu'au prochain point d'arrêt ;
  - passer à la ligne suivante **sans entrer** dans les appels (par-dessus) ;
  - **entrer** dans un appel de procédure, ou en **sortir** vers l'appelant ;
  - **suspendre** une exécution en cours.
- **Pile d'appels** : remonter la chaîne des appels ayant mené au point
  d'arrêt.
- **Inspection des variables** : lire les variables **locales** de la
  procédure courante **et** les variables **globales**.
- **Évaluation d'expressions** : évaluer une expression dans le contexte
  suspendu — depuis le panneau de surveillance (Watch), au survol d'une
  variable dans le code (hover), ou dans la console de débogage.

Ces capacités sont indépendantes du client : elles font partie du
débogueur, pas de l'éditeur qui l'affiche.

---

## 3. Un point d'arrêt ne s'arrête que sur une ligne exécutée

Un point d'arrêt ne suspend l'exécution que s'il est posé sur une **ligne
réellement exécutée** ; sur une ligne vide, un commentaire ou un en-tête
de procédure, il ne provoque aucune pause. La règle pratique : le poser
sur une **ligne d'instruction** du flux parcouru (ci-dessous, la deuxième).

```owl
sNom est une chaîne = "le monde"
sMessage est une chaîne = "Bonjour, " + sNom   // point d'arrêt utile ici
Trace(sMessage)
```

---

## 4. Brancher un client DAP

Le débogueur étant piloté par le client, la marche à suivre dépend de
l'éditeur. Le principe reste constant : ouvrir un fichier `.owl`, poser
un point d'arrêt sur une ligne exécutée, lancer le débogage. Le client
dialogue alors avec `owl-dap`.

> **Exemple avec un client DAP — Visual Studio Code**
>
> Avec l'extension OWL installée, aucune configuration à écrire : le
> plugin fournit une configuration de débogage par défaut (pas de
> fichier de configuration à rédiger à la main). Poser un point d'arrêt
> dans la marge d'un fichier `.owl`, puis lancer le débogage du fichier
> actif ; ses voisins du même projet sont chargés avec lui.
>
> Les détails d'installation et d'usage — raccourcis clavier, canal de
> diagnostic pour tracer les échanges avec le débogueur — relèvent de la
> documentation du plugin, à laquelle se reporter. Ce chapitre décrit le
> débogueur du framework, pas un éditeur particulier.

---

## Pour aller plus loin

| Sujet | Référence |
|---|---|
| Pièges courants du langage | [`22_pieges_courants.md`](22_pieges_courants.md) |
| Contrôle de flux et procédures | [`03_controle_de_flux.md`](03_controle_de_flux.md) |
| Tests intégrés | [`11_charte_de_test.md`](11_charte_de_test.md) |

Pour les détails propres à un éditeur, se reporter à la documentation du
client DAP utilisé.
