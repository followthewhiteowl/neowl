# OWL — Licence, en clair

*[🇬🇧 English](LICENSING.en.md)*

## En une phrase

OWL est **open source sous AGPL-3.0-only**, assortie de l'**OWL Runtime
Exception**. Vous pouvez l'utiliser, l'étudier, le modifier et le forker
librement, y compris dans un cadre professionnel ou commercial. **Les
applications que vous écrivez avec OWL restent sous la licence de votre choix**,
y compris propriétaire.

## Le modèle, en cinq lignes

- Le **runtime OWL** est sous AGPL.
- Les **modifications du runtime OWL** restent sous AGPL (si vous les distribuez
  ou les exposez en réseau, vous publiez vos modifications du moteur).
- Un **programme indépendant** (votre code `.owl`, votre application) peut être
  combiné au runtime OWL et distribué **sous la licence de votre choix**.
- Le seul fait de combiner votre programme avec le runtime — en le buildant, en
  l'interprétant, ou en utilisant la bibliothèque standard et les fonctions
  natives d'OWL — **ne place pas** votre programme sous AGPL.
- Cette permission **ne permet pas** de relicencier OWL lui-même ni ses
  modifications.

## Corriger OWL ne vous pénalise pas

Vous pouvez modifier le runtime OWL (corriger un bug, l'améliorer), publier ces
modifications sous AGPL comme la licence l'exige, **puis continuer à builder et
distribuer votre application indépendante propriétaire** avec ce runtime
modifié. L'obligation AGPL porte sur **vos modifications du moteur**, pas sur
votre application.

## Vos obligations quand vous distribuez

### Le moteur OWL (AGPL)
Un exécutable que vous distribuez contient le runtime OWL (AGPL + Runtime
Exception). L'obligation AGPL porte sur la source correspondante des parties
OWL couvertes — la source publique d'OWL plus vos éventuelles modifications du
runtime — **pas** sur votre programme indépendant. Si vous exposez une version
modifiée du moteur via un réseau, l'AGPL §13 s'applique à ces modifications du
moteur.

### Les composants tiers
OWL embarque des bibliothèques tierces, chacune sous sa propre licence
(inventaire : `THIRD-PARTY-NOTICES`). En particulier, une application GUI qui
lie **Qt statiquement** est soumise aux obligations propres de la **LGPL-3.0**
(permettre à ses destinataires de relier l'application avec une version modifiée
de Qt). Cette obligation découle de la licence de Qt — pas d'OWL — et la Runtime
Exception ne la lève pas.

## FAQ

**Puis-je écrire une application propriétaire / commerciale avec OWL ?**
Oui. Votre application garde la licence de votre choix. (Pour une app GUI
distribuée, voir l'obligation Qt ci-dessus.)

**Dois-je publier mon code `.owl` ?**
Non. L'exécuter ou le builder avec OWL ne le place pas sous AGPL.

**Puis-je utiliser OWL en entreprise, en interne ?**
Oui. Dans l'usage interne d'OWL non modifié, vous n'avez pas à publier votre
code applicatif.

**Puis-je forker OWL ?**
Oui, tant que votre fork respecte l'AGPL (il reste ouvert). Vous ne pouvez pas en
faire une version fermée/propriétaire.

**J'ai corrigé un bug dans OWL. Que dois-je faire ?**
Si vous distribuez le moteur corrigé (ou l'exposez en réseau modifié), publiez
votre correction sous AGPL. Votre application garde sa licence.
