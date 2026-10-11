# Iconify icon-sets — snapshot vendore

Source : https://github.com/iconify/icon-sets
Script : `scripts/refresh_iconify_sets.sh`
Refresh recommande : tous les 6-12 mois (manuel).

## Etat du snapshot

- **Date refresh** : 2026-06-06T19:49:53+0200
- **Commit amont** : `5d7fcf84a040c1fb52dcad8790e837dcc0f0c556`
- **Cocktail T4** : 10 sets cherry-pickes (cf. `feat_0337-catalogue-icones-embarque.md`)

## Sets vendorises

| Prefixe | Fichier | Taille | SHA256 (16c) | Licence | Role |
|---|---|---:|---|---|---|
| `material-symbols` | material-symbols.json | 8.0 MB | `99063e7fdf45c5c3` | Apache-2.0 | UI base, look Material (3 styles inclus) |
| `mdi` | mdi.json | 3.3 MB | `cf9b89dda466f1c3` | Apache-2.0 | Diversite massive de concepts + file types |
| `fluent` | fluent.json | 11.7 MB | `4ef1cd9502c82c7c` | MIT | Look Windows 11 (filtre size 24 au build) |
| `game-icons` | game-icons.json | 6.1 MB | `9195a7d569916b41` | CC-BY-3.0 | Echecs, RPG, fantasy, originalite |
| `fluent-emoji-flat` | fluent-emoji-flat.json | 8.7 MB | `8d15d8881d780e39` | MIT | Emoji designe Windows 11 plat couleur |
| `simple-icons` | simple-icons.json | 4.5 MB | `ea694e18f756f73b` | CC0 | Logos marques mono |
| `line-md` | line-md.json | 1.4 MB | `3cba8ffa77541729` | MIT | Animations SVG natives (svg-static au build) |
| `carbon` | carbon.json | 1.1 MB | `624b7a15aeb386a1` | Apache-2.0 | Infra/cloud enterprise IBM |
| `devicon` | devicon.json | 4.8 MB | `2405f98c68fc23a4` | MIT | Langages/frameworks/clouds multi-color |
| `vscode-icons` | vscode-icons.json | 3.5 MB | `7948d326bfa2353f` | MIT | File types pre-colores natifs |
| `flat-color-icons` | flat-color-icons.json | 162.9 KB | `80cfb386d2164eb5` | MIT | Classiques flat colores style WinDev (Icons8, 329 icones) |
| `logos` | logos.json | 7.1 MB | `2bb0de8eb193740b` | CC0/MIT | Logos services tech multi-color (AWS/K8s/Docker/GCP/Azure/marques) |

## Notes

- **`line-md`** : seule la variante `svg-static` (animations desactivees) est embarquee dans le catalogue final, conformement au verdict de la veille SVG 1.2 vs 2.0 (cf. `feat_0336-poc-compat-svg-qt6.md`). Cherry-pick au build via le gen tool.
- **`fluent`** : multi-sizes (16/20/24/28/32/48). Le gen tool filtre size 24 seulement (~3.3 MB sur les 11.7 MB du JSON complet).
- **`fluent-emoji-flat`** : utilise des radialGradient. ~2-5% potentiellement degrades en Qt6 `QSvgRenderer` (bug `r` attribute manquant). A surveiller au POC E4.

## Licences

Toutes compatibles OWL alpha (link statique + redistribution). Attribution
**CC-BY-3.0** pour `game-icons` a mentionner dans `owl --credits`.

## Refresh

```bash
./scripts/refresh_iconify_sets.sh
```

Idempotent : skip les sets dont le SHA256 n'a pas change. Affiche un
resume `changed / skipped / failed` en fin de run.
