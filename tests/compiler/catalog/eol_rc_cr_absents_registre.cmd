#  (lot 1) : RC et CR sortent du registre des constantes — ils
# cessent d'etre des symboles du langage (completion, hover, doc). Seuls
# LF et CRLF restent des constantes canon de fin de ligne.
#
# constants_runtime emet chaque constante deux fois (nameFR, nameEN) sauf
# quand nameEN vaut NULL (une seule occurrence). Le tri + filtre ci-dessous
# capture donc a la fois l'absence de RC/CR ET la bonne cardinalite de
# LF/CRLF (2 chacun, FR = EN).
grep -oE '"constants_runtime": \[[^]]*\]' \
| grep -oE '"[^"]+"' \
| tr -d '"' \
| grep -xE 'RC|CR|LF|CRLF' \
| sort
