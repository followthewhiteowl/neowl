# Fixtures de smoke test .xls (BIFF8 / OLE2)

Deux classeurs Excel 97-2003 reels, repris du corpus de tests amont libxls
(memes conditions de licence BSD-2 que la bibliotheque) :

- `mtcars.xls` : en-tetes texte + colonnes numeriques (jeu de donnees R mtcars).
  Exerce SST (LABELSST), NUMBER et RK.
- `vietnamese-utf8.xls` : chaines UTF-16 avec diacritiques vietnamiennes
  (codepage 1200). Prouve la conversion UTF-16 -> UTF-8 SANS iconv
  (chemin locale-forcee .65001 / C.UTF-8). Non-ASCII round-trip.

Consommes par `src/internal_tests/libxls_smoketest.c`.
