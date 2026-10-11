// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du mode GO local :
 *
 * `go_local_detect_fenetre(source, &out_name)` analyse un source .owl par
 * tokenisation et renvoie true si le fichier est un "fichier-fenetre pur" —
 * exactement une declaration top-level `FEN_X est une Fenetre ... FIN` et
 * RIEN d'autre (a part les commentaires). Dans ce cas main.c wrappe
 * implicitement Ouvre(FEN_X) apres project_run.
 *
 * Exerce 8 scenarios qui cadrent la decision :
 *   1. Cas positif : FEN pur (FR)
 *   2. Cas positif : FEN pur (EN, "is a Window")
 *   3. Cas positif : commentaires avant + apres
 *   4. Cas negatif : 2 fenetres -> pas d'auto-Ouvre (ambigu)
 *   5. Cas negatif : variable globale apres la fenetre
 *   6. Cas negatif : PROCEDURE Main() top-level
 *   7. Cas negatif : classe top-level
 *   8. Cas negatif : fichier vide
 *
 * Pas de vm_init ici — la detection est pure tokenisation (Scanner stateless).
 * Pattern inspire de cli_doc_test.c. */

#include "../scanner.h"

#include <stdio.h>
#include <string.h>

/* Symbole expose par src/main.c (sous-section ). */
extern bool go_local_detect_fenetre(const char* source, char* outName,
                                    int outCapacity);

/* Filet anti-silence expose par src/main.c : doit reconnaitre
 * la meme tete `IDENT EST [article] FENETRE` que le detecteur principal,
 * sinon un vrai auto-open rate sort en exit 0 muet. */
extern bool go_local_looks_like_fenetre_test(const char* source);

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

static void expect_positive(const char* src, const char* expectedName,
                            const char* label) {
    char name[128] = {0};
    bool got = go_local_detect_fenetre(src, name, (int)sizeof(name));
    CHECK(got, label);
    CHECK(strcmp(name, expectedName) == 0, label);
}

static void expect_negative(const char* src, const char* label) {
    char name[128] = {0};
    bool got = go_local_detect_fenetre(src, name, (int)sizeof(name));
    CHECK(!got, label);
}

/*  — le filet anti-silence go_local_looks_like_fenetre doit
 * reconnaitre la meme tete que le detecteur principal. */
static void expect_looks(const char* src, bool expected, const char* label) {
    bool got = go_local_looks_like_fenetre_test(src);
    CHECK(got == expected, label);
}

int go_local_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    /* 1. Cas canonique FR */
    expect_positive(
        "FEN_Demo est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"X\"\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Demo",
        "positif FR canonique");

    /* 2. Variante EN — "is a Window" */
    expect_positive(
        "FEN_Demo est un Window\n"
        "  PROPRIETES\n"
        "    Titre = \"X\"\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Demo",
        "positif EN (Window)");

    /* 3. Commentaires avant et apres bloc */
    expect_positive(
        "// header\n"
        "// suite\n"
        "FEN_Login est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Login\"\n"
        "  FIN\n"
        "FIN\n"
        "// pied de fichier\n",
        "FEN_Login",
        "positif avec commentaires");

    /* 4. Deux fenetres -> ambigu, pas d'auto-Ouvre */
    expect_negative(
        "FEN_A est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"A\"\n"
        "  FIN\n"
        "FIN\n"
        "FEN_B est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"B\"\n"
        "  FIN\n"
        "FIN\n",
        "negatif 2 fenetres");

    /* 5. Variable globale apres la fenetre -> code top-level autre */
    expect_negative(
        "FEN_Demo est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"X\"\n"
        "  FIN\n"
        "FIN\n"
        "gCompteur est un entier = 0\n",
        "negatif variable apres FEN");

    /* 6. PROCEDURE Main top-level -> on laisse Main piloter (cf. project_run) */
    expect_negative(
        "PROCEDURE Main()\n"
        "  Ouvre(FEN_Demo)\n"
        "FIN\n"
        "FEN_Demo est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"X\"\n"
        "  FIN\n"
        "FIN\n",
        "negatif PROCEDURE Main");

    /* 7. Classe top-level + fenetre -> pas un fichier-fenetre pur */
    expect_negative(
        "cConfig est une Classe\n"
        "  nom est une chaine\n"
        "FIN\n"
        "FEN_Demo est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"X\"\n"
        "  FIN\n"
        "FIN\n",
        "negatif classe top-level");

    /* 8. Fichier vide (que des commentaires) */
    expect_negative(
        "// vide\n"
        "// rien d'autre\n",
        "negatif fichier vide");

    /* 9. Sanity : fichier purement instructions (Trace) sans fenetre */
    expect_negative(
        "Trace(\"hello\")\n"
        "dbgBilanTest()\n",
        "negatif script classique");

    /* 10.  — repro fixture demarche snapshot reelle :
     * commentaires en tete multiples + ligne vide + bloc Fenetre avec
     * PROCEDURE FinInitialisation. Doit etre detecte. */
    expect_positive(
        "// Un commentaire en tete de fichier\n"
        "// comme dans la vraie demo snapshot.\n"
        "\n"
        "FEN_AvecComm est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tLargeur = 200\n"
        "\tFIN\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tTrace(\"go-local ok\")\n"
        "\t\tFerme()\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_AvecComm",
        "positif fixture demo (commentaires + FinInitialisation)");

    /* 11.  — commentaire multi-ligne (slash-star ... star-slash)
     * en tete. Le scanner doit le sauter comme un single-line. */
    expect_positive(
        "/* Bloc de doc\n"
        "   sur plusieurs\n"
        "   lignes */\n"
        "FEN_Multi est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"M\"\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Multi",
        "positif commentaire multi-ligne en tete");

    /* 12.  — commentaire APRES le FIN (pas seulement avant).
     * Le tail-scan doit tolerer les commentaires comme du whitespace. */
    expect_positive(
        "FEN_TailComm est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"T\"\n"
        "  FIN\n"
        "FIN\n"
        "/* footer block comment */\n"
        "// trailing line\n",
        "FEN_TailComm",
        "positif commentaires en queue (avant EOF)");

    /* ===  : cas ROUGES attendus (detecteur token-based cassé) ===
     *
     * Les 4 cas suivants doivent retourner TRUE (fichier-fenetre pur, ouvrable
     * en mode GO), mais le détecteur actuel retourne FALSE a cause de bugs
     * dans go_local_token_opens_block / l'heuristique sous-controle.
     *
     * Bug 1 (CONSTANTE) : TOKEN_CONSTANTE absent de go_local_token_opens_block.
     *   Le pattern `IDENT EST IDENT` à l'intérieur du bloc (ex. `NB est un entier`)
     *   fait depth++ parasite. Avec N declarations, depth est déséquilibré de N
     *   après le FIN du bloc CONSTANTE → le FIN final de Fenetre ne ramene pas
     *   depth à 0 → return false.
     *
     * Bug 2 (sous-widget type keyword à bloc) : les types UI built-in (Libelle, Bouton,
     *   Saisie) sont des KEYWORDS (TOKEN_LIBELLE, TOKEN_BOUTON, TOKEN_SAISIE), pas des
     *   TOKEN_IDENTIFIER → le pattern IDENT-EST-IDENT ne se déclenche PAS pour eux →
     *   depth n'est pas incrémenté → le FIN explicite du sous-widget (après PROPRIETES+FIN)
     *   fait depth-- sans depth++ préalable → sous-shoot → break prématuré ou
     *   rejet en Phase 3.
     *
     * Bug 3 (Timer top-level) : une variable de type Timer déclarée au sein de la
     *   Fenetre (`tmrAnim est un Timer`) trigger le pattern `IDENT EST IDENT` →
     *   depth++ parasite sans FIN correspondant (Timer n'est pas un sous-widget DSL,
     *   pas de FIN). Déséquilibre +1. */

    /* 13.  cas A — fenetre avec bloc CONSTANTE a 2 declarations.
     * Un seul FIN après les 2 constantes. Le détecteur comptera 2 depth++
     * parasites (1 par `NB est un entier`) et seulement 1 depth-- (le FIN
     * du bloc CONSTANTE), laissant depth==2 au lieu de 1. Le FIN final de
     * Fenetre ne ramene pas a 0 → return false (rouge attendu aujourd'hui). */
    expect_positive(
        "FEN_Constante est une Fenetre\n"
        "  CONSTANTE\n"
        "    TAILLE_CASE est un entier = 20\n"
        "    NB_COLONNES est un entier = 30\n"
        "  FIN\n"
        "  PROPRIETES\n"
        "    Titre = \"Test CONSTANTE\"\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Constante",
        "positif fenetre + bloc CONSTANTE (2 decls) [fix/0564]");

    /* 14.  cas B — sous-widget de type keyword (TOKEN_LIBELLE) avec
     * son propre bloc PROPRIETES+FIN+FIN.
     * Bug : `LIB_X est un Libelle` → `Libelle` est TOKEN_LIBELLE (keyword),
     * pas TOKEN_IDENTIFIER → le pattern IDENT-EST-IDENT ne se déclenche PAS
     * → depth n'est pas incrémenté pour le sous-widget. Ensuite PROPRIETES
     * fait depth++ (depth=2), FIN(PROPRIETES) fait depth-- (depth=1), et le
     * FIN final du widget fait depth-- (depth=0) → break PRÉMATURÉ avant le
     * vrai FIN de Fenetre → Phase 3 voit ce FIN → return false.
     * Idem pour TOKEN_BOUTON, TOKEN_SAISIE (tous les types UI built-in keywords). */
    expect_positive(
        "FEN_SubWidget est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Test sous-widget\"\n"
        "  FIN\n"
        "  LIB_X est un Libelle\n"
        "    PROPRIETES\n"
        "      Libelle = \"Hello\"\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_SubWidget",
        "positif fenetre + sous-widget token keyword (Libelle) a PROPRIETES bloc [fix/0564]");

    /* 15.  cas C — variable type Timer interne (pas un sous-widget).
     * `tmrAnim est un Timer` → IDENT EST IDENT → depth++ parasite.
     * Timer n'a pas de FIN → déséquilibre +1 permanent. Le FIN final de
     * Fenetre ne ramene pas depth a 0 → return false (rouge attendu). */
    expect_positive(
        "FEN_Timer est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Test Timer\"\n"
        "  FIN\n"
        "  tmrAnim est un Timer\n"
        "  PROCEDURE FinInitialisation()\n"
        "    Trace(\"init\")\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Timer",
        "positif fenetre + var Timer interne (sans FIN) [fix/0564]");

    /* 16.  cas D — combo Snake : CONSTANTE (2 decls) + sous-widget
     * type keyword (TOKEN_BOUTON) avec PROPRIETES + Timer. Cumule les 3 bugs :
     *   - CONSTANTE : depth++ parasites pour N décls, FIN seul pour N.
     *   - TOKEN_BOUTON (keyword) : pattern IDENT-EST-IDENT non déclenché →
     *     depth-- parasite au FIN widget (sous-shoot).
     *   - Timer : pattern IDENT-EST-IDENT déclenché mais pas de FIN → depth++ parasite.
     * Exit 0 silencieux dans le vrai Snake. Cas prioritaire pour l'implementer. */
    expect_positive(
        "FEN_Snake est une Fenetre\n"
        "  CONSTANTE\n"
        "    TAILLE_CASE est un entier = 20\n"
        "    NB_COLONNES est un entier = 30\n"
        "  FIN\n"
        "  PROPRIETES\n"
        "    Titre = \"Snake\"\n"
        "  FIN\n"
        "  BTN_Rejouer est un Bouton\n"
        "    PROPRIETES\n"
        "      Libelle = \"Rejouer\"\n"
        "    FIN\n"
        "  FIN\n"
        "  tmrJeu est un Timer\n"
        "  PROCEDURE FinInitialisation()\n"
        "    Trace(\"init\")\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Snake",
        "positif combo Snake (CONSTANTE + sous-widget + Timer) [fix/0564]");

    /* ===  TEMOINS POSITIFS (doivent rester TRUE — déjà corrects) ===
     *
     * 17. Fenetre triviale sans widget ni CONSTANTE. Deja couvert par cas 1-3
     * mais on pose un temoin explicite  pour le rapport de progression. */
    expect_positive(
        "FEN_Triviale est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Trivial\"\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Triviale",
        "temoin positif fenetre triviale [fix/0564]");

    /* ===  TEMOINS NEGATIFS (doivent rester FALSE) ===
     *
     * 18. Vrai script (pas de fenetre) — doit rester FALSE. */
    expect_negative(
        "x est un entier = 42\n"
        "Trace(x)\n",
        "temoin negatif script pur [fix/0564]");

    /* 19. Deux blocs top-level : une fenetre + une variable globale apres.
     * Doit rester FALSE (code top-level supplementaire apres la fenetre). */
    expect_negative(
        "FEN_A est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"A\"\n"
        "  FIN\n"
        "FIN\n"
        "gScore est un entier = 0\n",
        "temoin negatif fenetre + variable globale apres [fix/0564]");

    /* ===  : prefixe `Champ` obligatoire sur les widgets ===
     *
     * Depuis l'introduction du prefixe `Champ` dans les declarations widget
     * (`LIB_X est un Champ Libelle ... FIN`), les tokens deviennent
     * `IDENT(LIB_X) EST IDENT(champ) LIBELLE ...`. `champ` n'est PAS un
     * keyword scanner (conflit avec `champ est une chaine`) -> TOKEN_IDENTIFIER
     * -> tombe dans la sous-branche "type IDENT" qui peek un PROPRIETES. Or
     * apres `champ` vient `Libelle` (TOKEN_LIBELLE), pas PROPRIETES -> pas de
     * depth++, mais le FIN du widget decremente quand meme -> desequilibre
     * -> detection FALSE. Le fix detecte le prefixe `champ` par lexeme et fait
     * depth++ inconditionnel (un `<NOM> est un Champ <Type>` a TOUJOURS un
     * corps + FIN). */

    /* 20.  cas A — un seul widget `Champ Libelle` (corps + FIN). */
    expect_positive(
        "FEN_Champ est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Test Champ\"\n"
        "  FIN\n"
        "  LIB_1 est un Champ Libelle\n"
        "    PROPRIETES\n"
        "      Libelle = \"Hello\"\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Champ",
        "positif fenetre + 1 widget Champ Libelle [fix/0645]");

    /* 21.  cas B — deux widgets freres : `Champ Libelle` puis
     * `Champ de Saisie` (forme avec filler `de` -> TOKEN_DE). Chacun a son
     * corps + FIN. */
    expect_positive(
        "FEN_Deux est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Deux widgets\"\n"
        "  FIN\n"
        "  LIB_1 est un Champ Libelle\n"
        "    PROPRIETES\n"
        "      Libelle = \"Nom\"\n"
        "    FIN\n"
        "  FIN\n"
        "  SAI_1 est un Champ de Saisie\n"
        "    PROPRIETES\n"
        "      Indication = \"Saisir\"\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Deux",
        "positif fenetre + Champ Libelle + Champ de Saisie [fix/0645]");

    /* 22.  cas C — `Champ Conteneur` contenant un `Champ Libelle`
     * imbrique. Verifie le balancage depth sur widgets a corps imbrique. */
    expect_positive(
        "FEN_Cont est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Conteneur\"\n"
        "  FIN\n"
        "  CONT_1 est un Champ Conteneur\n"
        "    PROPRIETES\n"
        "      Disposition = dispVerticale\n"
        "    FIN\n"
        "    LIB_Inner est un Champ Libelle\n"
        "      PROPRIETES\n"
        "        Libelle = \"Imbrique\"\n"
        "      FIN\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Cont",
        "positif fenetre + Champ Conteneur imbriquant Champ Libelle [fix/0645]");

    /* 23.  cas D — case/accent insensible sur le prefixe :
     * `CHAMP` majuscule doit etre reconnu comme prefixe widget. */
    expect_positive(
        "FEN_Casse est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Casse\"\n"
        "  FIN\n"
        "  BTN_1 est un CHAMP Bouton\n"
        "    PROPRIETES\n"
        "      Libelle = \"OK\"\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Casse",
        "positif prefixe Champ en majuscules [fix/0645]");

    /* 24.  temoin negatif — variable scalaire dont le NOM contient
     * "champ" comme sous-chaine ne doit PAS etre confondue avec le prefixe.
     * `champPerso est un entier` : ici le token apres EST est `champPerso`
     * (un IDENT != "champ"), pas le prefixe -> pas de depth++ parasite
     * -> la fenetre reste detectee (un seul widget reel autour). */
    expect_positive(
        "FEN_Faux est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Faux champ\"\n"
        "  FIN\n"
        "  champPerso est un entier\n"
        "  LIB_1 est un Champ Libelle\n"
        "    PROPRIETES\n"
        "      Libelle = \"X\"\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Faux",
        "positif var nommee champPerso != prefixe Champ [fix/0645]");

    /* ===  : article EN `a`/`an` entre la copule `is` et le type
     * `Window`.  avait ajoute le skip de l'article DANS la boucle
     * de profondeur, mais PAS au check initial (`IDENT is a Window`) ni
     * dans le filet anti-silence -> `owl FEN.owl` sortait en
     * exit 0 muet sur une fenetre ecrite en anglais. ROUGES attendus
     * avant impl. */

    /* 25.  cas A — fenetre EN canonique `is a Window`. Le token
     * apres `is` (TOKEN_EST) est l'article `a` (TOKEN_A), pas Window. Doit
     * etre saute pour que le check du kind voie TOKEN_FENETRE. */
    expect_positive(
        "FEN_T is a Window\n"
        "  PROPERTIES\n"
        "    Title = \"t\"\n"
        "  END\n"
        "END\n",
        "FEN_T",
        "positif EN `is a Window` (article a) [fix/0691]");

    /* 26.  cas B — article `an` (IDENT 2 chars, case-insensible).
     * Devant Window c'est grammaticalement faux mais le skip doit le
     * tolerer en miroir exact de la boucle de profondeur. */
    expect_positive(
        "FEN_An is an Window\n"
        "  PROPERTIES\n"
        "    Title = \"t\"\n"
        "  END\n"
        "END\n",
        "FEN_An",
        "positif EN `is an Window` (article an) [fix/0691]");

    /* 27.  temoin FR — non-regression : `est une Fenetre` (filler
     * `une` mange par le scanner, pas d'article TOKEN_A) reste detecte. */
    expect_positive(
        "FEN_Fr est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"t\"\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Fr",
        "temoin positif FR `est une Fenetre` [fix/0691]");

    /* 28.  temoin negatif — decl scalaire `tmr est un Timer` seule
     * (pas une fenetre) ne doit PAS etre prise pour une fenetre. Garde
     *  : le skip article ne doit pas relaxer ce rejet. */
    expect_negative(
        "tmr est un Timer\n",
        "temoin negatif `tmr est un Timer` scalaire [fix/0691]");

    /* 29.  — meme verrou, version EN `tmr is a Timer` : le token
     * apres l'article est TOKEN_IDENTIFIER(Timer) != TOKEN_FENETRE -> rejet. */
    expect_negative(
        "tmr is a Timer\n",
        "temoin negatif EN `tmr is a Timer` scalaire [fix/0691]");

    /* 30-34.  — filet anti-silence go_local_looks_like_fenetre :
     * doit reconnaitre la meme tete que le detecteur, sinon exit 0 muet. */
    expect_looks(
        "FEN_T is a Window\n  PROPERTIES\n    Title = \"t\"\n  END\nEND\n",
        true, "looks_like EN `is a Window` [fix/0691]");
    expect_looks(
        "FEN_An is an Window\n  PROPERTIES\n    Title = \"t\"\n  END\nEND\n",
        true, "looks_like EN `is an Window` [fix/0691]");
    expect_looks(
        "FEN_Fr est une Fenetre\n  PROPRIETES\n    Titre = \"t\"\n  FIN\nFIN\n",
        true, "looks_like FR `est une Fenetre` [fix/0691]");
    expect_looks(
        "tmr est un Timer\n",
        false, "looks_like negatif `tmr est un Timer` [fix/0691]");
    expect_looks(
        "tmr is a Timer\n",
        false, "looks_like negatif EN `tmr is a Timer` [fix/0691]");

    /* ===  (suite) : prefixe widget EN `Control` (nameEN de `Champ`)
     * et synonyme `Field`. go_local_token_is_champ_prefix ne reconnaissait
     * que `champ` (5 chars) -> un widget EN `LIB_1 is a Control Label` n'etait
     * pas compte comme ouvreur de bloc (depth++), son PROPERTIES/END
     * desynchronisait la profondeur -> fenetre EN non detectee -> pas
     * d'auto-open (exit 65 anti-silence). Le compilo/plugin acceptent deja
     * les 3 prefixes (Champ|Control|Field). ROUGES attendus avant impl. */

    /* 35.  cas A — widget EN prefixe `Control` (LABEL keyword). */
    expect_positive(
        "FEN_T is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle = \"t\"\n"
        "\tEND\n"
        "\tLIB_1 is a Control Label\n"
        "\t\tPROPERTIES\n"
        "\t\t\tCaption = \"x\"\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n",
        "FEN_T",
        "positif fenetre EN + widget `Control Label` [fix/0691]");

    /* 36.  cas B — synonyme EN `Field` (5 chars, comme Champ). */
    expect_positive(
        "FEN_F is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle = \"t\"\n"
        "\tEND\n"
        "\tLIB_1 is a Field Label\n"
        "\t\tPROPERTIES\n"
        "\t\t\tCaption = \"x\"\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n",
        "FEN_F",
        "positif fenetre EN + widget `Field Label` [fix/0691]");

    /* 37.  cas C — mix `Control` + `Champ` (le vrai Snake EN melange
     * les deux prefixes). Les deux widgets doivent ouvrir un bloc. */
    expect_positive(
        "FEN_Mix is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle = \"t\"\n"
        "\tEND\n"
        "\tLIB_Score is a Control Label\n"
        "\t\tPROPERTIES\n"
        "\t\t\tCaption = \"0\"\n"
        "\t\tEND\n"
        "\tEND\n"
        "\tBTN_Rejouer est un Champ Bouton\n"
        "\t\tPROPRIETES\n"
        "\t\t\tLibelle = \"Rejouer\"\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "END\n",
        "FEN_Mix",
        "positif fenetre EN mix `Control` + `Champ` (Snake EN) [fix/0691]");

    /* 38.  cas D — case-insensible sur `Control` (CONTROL majuscule). */
    expect_positive(
        "FEN_Up is a Window\n"
        "\tPROPERTIES\n"
        "\t\tTitle = \"t\"\n"
        "\tEND\n"
        "\tBTN_1 is a CONTROL Button\n"
        "\t\tPROPERTIES\n"
        "\t\t\tCaption = \"OK\"\n"
        "\t\tEND\n"
        "\tEND\n"
        "END\n",
        "FEN_Up",
        "positif prefixe `Control` en majuscules [fix/0691]");

    /* ===  : propriete `Etat = <valeur>` (singuliere, hors chaine
     * membre `..`) collisionne avec TOKEN_ETAT dans go_local_token_opens_block.
     * `Etat` seul (4 chars, sans `..` devant) matche exactement le keyword
     * scanner "etat" -> TOKEN_ETAT est traite comme ouvreur de bloc
     * inconditionnel (liste go_local_token_opens_block) alors qu'ici c'est
     * une simple valeur de propriete scalaire (`Etat = Actif`), pas un bloc
     * DSL. depth++ parasite jamais rebalance par un FIN -> le FIN final de
     * la Fenetre ne ramene pas depth a 0 -> return false. Repro reel :
     * examples/style_saisie_demo/FEN_TestStyle.owl (LST_SansNom1, `Etat =
     * Actif`). ROUGES attendus avant impl Lot 2. */

    /* 39.  cas A — bisection minimale : un seul widget avec une
     * PROPRIETES contenant `Etat = Actif` (sans `..` devant). C'est LE
     * construct minimal qui fait basculer detect a false. */
    expect_positive(
        "FEN_EtatSeul est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Test Etat\"\n"
        "  FIN\n"
        "  LST_X est un Champ Liste\n"
        "    PROPRIETES\n"
        "      Etat = Actif\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_EtatSeul",
        "positif fenetre + widget avec propriete Etat= scalaire [fix/0937]");

    /* 40.  cas B — repro reel multi-widgets stylee (calque le
     * pattern FEN_TestStyle.owl) : plusieurs widgets avec chaines de
     * membres `Style..X..Etats[etat]..Y` (qui NE posent PAS probleme,
     * cf. cas 41 ci-dessous) COMBINEES a une propriete `Etat = Actif`
     * scalaire sur un widget distinct. Doit etre detectee (true). */
    expect_positive(
        "FEN_MultiStyle est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Style\"\n"
        "  FIN\n"
        "  SAI_Demo est un Champ Saisie\n"
        "    PROPRIETES\n"
        "      Libelle = \"Champ\"\n"
        "      Style..ZoneSaisie..Etats[survol]..Couleur = OrangeClair\n"
        "      Style..ZoneSaisie..Couleur = Rouge\n"
        "    FIN\n"
        "  FIN\n"
        "  LST_X est un Champ Liste\n"
        "    PROPRIETES\n"
        "      Largeur = 330\n"
        "      Etat = Actif\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_MultiStyle",
        "positif fenetre multi-widgets Style..Etats[..].. + Etat= scalaire [fix/0937]");

    /* 41.  temoin positif — la chaine de membres `Style..X..
     * Etats[etat]..Y` SEULE (sans propriete Etat= scalaire ailleurs) est
     * DEJA correctement geree par le skip de membre (prev==DOUBLE_DOT
     * intercepte `Etats` avant le check opens_block). Ce cas doit rester
     * TRUE avant ET apres le fix Lot 2 — sert de garde anti-regression
     * pour ne pas casser le skip de membre existant en corrigeant le
     * cas 39/40. */
    expect_positive(
        "FEN_ChaineMembre est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Chaine membre\"\n"
        "  FIN\n"
        "  SAI_X est un Champ Saisie\n"
        "    PROPRIETES\n"
        "      Style..ZoneSaisie..Etats[survol]..Couleur = Rouge\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_ChaineMembre",
        "temoin positif chaine ..Etats[etat].. seule (deja correcte) [fix/0937]");

    /* ===  : garde anti-silence — cas negatifs qui
     * DOIVENT rester false apres le fix Lot 2. Le futur correctif de
     * go_local_token_opens_block / TOKEN_ETAT ne doit PAS relaxer ces
     * rejets legitimes (code top-level supplementaire apres la fenetre,
     * deuxieme fenetre, variable globale, PROCEDURE top-level). */

    /* 42.  garde — deuxieme fenetre APRES une fenetre qui contient
     * elle-meme une propriete Etat= scalaire. Doit rester false (ambigu,
     * 2 fenetres). */
    expect_negative(
        "FEN_A est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"A\"\n"
        "  FIN\n"
        "  LST_X est un Champ Liste\n"
        "    PROPRIETES\n"
        "      Etat = Actif\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n"
        "FEN_B est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"B\"\n"
        "  FIN\n"
        "FIN\n",
        "garde negative 2 fenetres (dont 1 avec Etat=) [fix/0937]");

    /* 43.  garde — code top-level (variable globale) APRES une
     * fenetre avec Etat= scalaire. Doit rester false. */
    expect_negative(
        "FEN_C est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"C\"\n"
        "  FIN\n"
        "  LST_X est un Champ Liste\n"
        "    PROPRIETES\n"
        "      Etat = Actif\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n"
        "gCompteur est un entier = 0\n",
        "garde negative variable globale apres FEN avec Etat= [fix/0937]");

    /* 44.  garde — PROCEDURE top-level APRES une fenetre avec
     * Etat= scalaire. Doit rester false. */
    expect_negative(
        "FEN_D est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"D\"\n"
        "  FIN\n"
        "  LST_X est un Champ Liste\n"
        "    PROPRIETES\n"
        "      Etat = Actif\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n"
        "PROCEDURE Utilitaire()\n"
        "  Trace(\"x\")\n"
        "FIN\n",
        "garde negative PROCEDURE top-level apres FEN avec Etat= [fix/0937]");

    /* ===  : blocs de menu classique dans une Fenetre. Chaque noeud
     * (MenuPrincipal, SousMenu, OptionMenu, MenuSeparateur) ouvre un corps +
     * FIN mais n'est PAS un keyword scanner ( : is_widget=false) ->
     * arrive en TOKEN_IDENTIFIER. Le peek-PROPRIETES ne les couvre pas tous :
     * MenuPrincipal enchaine une declaration enfant (pas de PROPRIETES) et
     * MenuSeparateur n'a aucune PROPRIETES -> leur FIN sous-decremente depth,
     * la detection rate le vrai FIN de la Fenetre -> exit 65 anti-silence.
     * ROUGES attendus avant le fix go_local_token_is_menu_keyword. */

    /* 45.  cas complet — MenuPrincipal + SousMenu + OptionMenu (avec
     * surSelection) + MenuSeparateur, suivi d'un Champ. Doit etre detectee. */
    expect_positive(
        "FEN_Menu est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Menu\"\n"
        "    MenuPrincipal = MNU_M\n"
        "  FIN\n"
        "  MNU_M est un MenuPrincipal\n"
        "    SM_F est un SousMenu\n"
        "      PROPRIETES\n"
        "        Libelle = \"&Fichier\"\n"
        "      FIN\n"
        "      OPT_Q est une OptionMenu\n"
        "        PROPRIETES\n"
        "          Libelle = \"&Quitter\"\n"
        "        FIN\n"
        "        PROCEDURE surSelection()\n"
        "          Ferme(FEN_Menu)\n"
        "        FIN\n"
        "      FIN\n"
        "      SEP_1 est un MenuSeparateur\n"
        "      FIN\n"
        "    FIN\n"
        "  FIN\n"
        "  LBL_X est un Champ Libelle\n"
        "    PROPRIETES\n"
        "      Libelle = \"hello\"\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Menu",
        "positif fenetre + MenuPrincipal/SousMenu/OptionMenu/Separateur + Champ [feat/1168]");

    /* 46.  cas EN — MainMenu / Submenu / MenuOption / MenuSeparator
     * (formes anglaises canon). Doit etre detectee. */
    expect_positive(
        "FEN_En is a Window\n"
        "  PROPERTIES\n"
        "    Title = \"Menu\"\n"
        "  END\n"
        "  MNU_E is a MainMenu\n"
        "    SM_E is a Submenu\n"
        "      PROPERTIES\n"
        "        Caption = \"&File\"\n"
        "      END\n"
        "      OPT_E is a MenuOption\n"
        "        PROPERTIES\n"
        "          Caption = \"&Quit\"\n"
        "        END\n"
        "      END\n"
        "      SEP_E is a MenuSeparator\n"
        "      END\n"
        "    END\n"
        "  END\n"
        "END\n",
        "FEN_En",
        "positif fenetre EN + MainMenu/Submenu/MenuOption/MenuSeparator [feat/1168]");

    /* 47.  bisection minimale — un MenuPrincipal (non suivi de
     * PROPRIETES) + un MenuSeparateur (sans PROPRIETES) : les deux noeuds que
     * le peek-PROPRIETES ne compte PAS. C'est LE construct minimal qui faisait
     * basculer detect a false avant le fix. */
    expect_positive(
        "FEN_MiniMenu est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Mini\"\n"
        "  FIN\n"
        "  MNU_X est un MenuPrincipal\n"
        "    SEP_A est un MenuSeparateur\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_MiniMenu",
        "positif fenetre + MenuPrincipal nu + MenuSeparateur [feat/1168]");

    /* 48.  garde negative — une variable globale APRES une fenetre a
     * menu doit rester false (le fix menu ne relaxe pas l'anti-silence). */
    expect_negative(
        "FEN_G est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"G\"\n"
        "  FIN\n"
        "  MNU_G est un MenuPrincipal\n"
        "    SM_G est un SousMenu\n"
        "      PROPRIETES\n"
        "        Libelle = \"X\"\n"
        "      FIN\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n"
        "gCompteur est un entier = 0\n",
        "garde negative variable globale apres FEN a menu [feat/1168]");

    /* ===  : bloc `Style ... FIN` a l'interieur d'un MenuPrincipal
     *. `Style` seul sur sa ligne ouvre un corps + FIN mais n'est
     * PAS un keyword scanner -> arrive en TOKEN_IDENTIFIER, non compte comme
     * ouvreur -> son FIN sous-decremente depth -> desync -> exit 65
     * anti-silence. Le fix compte `Style` (forme bloc = suivi d'un NEWLINE)
     * comme depth++, en distinguant la propriete widget (`Style = STY_X`,
     * `Style..CouleurFond`) qui NE doit PAS compter. */

    /* 49.  cas complet — MenuPrincipal avec bloc Style stylant
     * Barre/SousMenu/OptionMenu, chaines de membres `..Etats[survol]..`,
     * suivi d'un SousMenu/OptionMenu reel puis d'un Champ. Doit etre detectee
     * (calque examples/menu_classique/FEN_MenuStyleDemoVisuel.owl). */
    expect_positive(
        "FEN_MenuStyle est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Style menu\"\n"
        "    MenuPrincipal = MNU_S\n"
        "  FIN\n"
        "  MNU_S est un MenuPrincipal\n"
        "    Style\n"
        "      Barre..CouleurFond = BleuFonce\n"
        "      Barre..Etats[survol]..CouleurFond = JauneClair\n"
        "      SousMenu..CouleurFond = GrisClair\n"
        "      OptionMenu..Etats[survol]..Police..Gras = Vrai\n"
        "    FIN\n"
        "    SM_F est un SousMenu\n"
        "      PROPRIETES\n"
        "        Libelle = \"&Fichier\"\n"
        "      FIN\n"
        "      OPT_Q est une OptionMenu\n"
        "        PROPRIETES\n"
        "          Libelle = \"&Quitter\"\n"
        "        FIN\n"
        "      FIN\n"
        "    FIN\n"
        "  FIN\n"
        "  LBL_X est un Champ Libelle\n"
        "    PROPRIETES\n"
        "      Libelle = \"hello\"\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_MenuStyle",
        "positif fenetre + MenuPrincipal avec bloc Style + Etats[survol] [fix/1181]");

    /* 50.  bisection minimale — un MenuPrincipal ne contenant qu'un
     * bloc Style nu. C'est LE construct minimal qui faisait basculer detect
     * a false (le FIN du bloc Style sans depth++ prealable). */
    expect_positive(
        "FEN_MiniStyle est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Mini style\"\n"
        "  FIN\n"
        "  MNU_X est un MenuPrincipal\n"
        "    Style\n"
        "      Barre..CouleurFond = Bleu\n"
        "    FIN\n"
        "    SEP_A est un MenuSeparateur\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_MiniStyle",
        "positif MenuPrincipal + bloc Style nu + Separateur [fix/1181]");

    /* 51.  garde — propriete widget `Style = STY_X` scalaire dans un
     * bloc PROPRIETES. La forme propriete (token suivant = `=`) NE doit PAS
     * declencher de depth++, sinon le compteur diverge et casse la detection.
     * Doit rester true. */
    expect_positive(
        "FEN_PropStyle est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Prop style\"\n"
        "  FIN\n"
        "  SAI_X est un Champ Saisie\n"
        "    PROPRIETES\n"
        "      Style = STY_Bleu\n"
        "      Style..CouleurFond = Rouge\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_PropStyle",
        "positif widget avec propriete Style= et Style..CouleurFond (pas de faux depth++) [fix/1181]");

    /* 52.  garde — le menu SANS bloc Style reste detecte (le fix ne
     * regresse pas ). */
    expect_positive(
        "FEN_MenuSansStyle est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Sans style\"\n"
        "  FIN\n"
        "  MNU_N est un MenuPrincipal\n"
        "    SM_N est un SousMenu\n"
        "      PROPRIETES\n"
        "        Libelle = \"&Sans\"\n"
        "      FIN\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_MenuSansStyle",
        "positif menu sans bloc Style (non regresse) [fix/1181]");

    /* ===  : bloc `<NOM> est un MenuContextuel` declare dans la
     * Fenetre (sibling des champs, feat menu contextuel clic droit). Comme
     * MenuPrincipal, ce n'est PAS un keyword scanner -> arrive en
     * TOKEN_IDENTIFIER, et n'etait pas liste dans go_local_token_is_menu_keyword
     * -> son FIN sous-decremente depth -> desync -> exit 65 anti-silence. La
     * demo examples/menu_contextuel/FEN_MenuContextuelDemo.owl a 2 blocs
     * MenuContextuel -> -2 -> fossile. Le fix ajoute MenuContextuel/ContextMenu
     * a la table kws[]. ROUGES attendus avant le fix. */

    /* 53.  cas complet — un MenuContextuel avec bloc Style +
     * OptionMenu + MenuSeparateur, suivi d'un Champ Saisie. Calque un fragment
     * de la demo. Doit etre detectee. */
    expect_positive(
        "FEN_Ctx est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Contextuel\"\n"
        "    MenuContextuel = MNU_C\n"
        "  FIN\n"
        "  MNU_C est un MenuContextuel\n"
        "    Style\n"
        "      SousMenu..CouleurFond = Blanc\n"
        "      OptionMenu..Etats[survol]..CouleurFond = BleuPastel\n"
        "    FIN\n"
        "    OPT_Copier est une OptionMenu\n"
        "      PROPRIETES\n"
        "        Libelle = \"&Copier\"\n"
        "      FIN\n"
        "    FIN\n"
        "    SEP_1 est un MenuSeparateur\n"
        "    FIN\n"
        "  FIN\n"
        "  ZS_A est un Champ Saisie\n"
        "    PROPRIETES\n"
        "      MenuContextuel = MNU_C\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Ctx",
        "positif fenetre + MenuContextuel avec Style + OptionMenu + Separateur [feat/1164]");

    /* 54.  repro demo — DEUX blocs MenuContextuel dans la meme
     * fenetre (comme FEN_MenuContextuelDemo.owl : MNU_Edition + MNU_Fenetre).
     * C'est le cas qui produisait -2 sur depth -> fossile. Doit etre detectee. */
    expect_positive(
        "FEN_Deux est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Deux contextuels\"\n"
        "    MenuContextuel = MNU_Fenetre\n"
        "  FIN\n"
        "  MNU_Edition est un MenuContextuel\n"
        "    Style\n"
        "      SousMenu..CouleurFond = Blanc\n"
        "    FIN\n"
        "    OPT_Copier est une OptionMenu\n"
        "      PROPRIETES\n"
        "        Libelle = \"&Copier\"\n"
        "      FIN\n"
        "    FIN\n"
        "  FIN\n"
        "  MNU_Fenetre est un MenuContextuel\n"
        "    Style\n"
        "      SousMenu..CouleurFond = Blanc\n"
        "    FIN\n"
        "    OPT_APropos est une OptionMenu\n"
        "      PROPRIETES\n"
        "        Libelle = \"A propos\"\n"
        "      FIN\n"
        "    FIN\n"
        "  FIN\n"
        "  ZS_A est un Champ Saisie\n"
        "    PROPRIETES\n"
        "      MenuContextuel = MNU_Edition\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Deux",
        "positif fenetre + 2 blocs MenuContextuel (repro demo) [feat/1164]");

    /* 55.  cas EN — ContextMenu (forme anglaise canon). Doit etre
     * detectee. */
    expect_positive(
        "FEN_En is a Window\n"
        "  PROPERTIES\n"
        "    Title = \"Context\"\n"
        "  END\n"
        "  MNU_E is a ContextMenu\n"
        "    OPT_E is a MenuOption\n"
        "      PROPERTIES\n"
        "        Caption = \"&Copy\"\n"
        "      END\n"
        "    END\n"
        "  END\n"
        "END\n",
        "FEN_En",
        "positif fenetre EN + ContextMenu [feat/1164]");

    /* 56.  bisection minimale — un MenuContextuel nu contenant un
     * MenuSeparateur. LE construct minimal qui faisait basculer detect a false
     * (le FIN du bloc MenuContextuel sans depth++ prealable). */
    expect_positive(
        "FEN_Mini est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"Mini contextuel\"\n"
        "  FIN\n"
        "  MNU_X est un MenuContextuel\n"
        "    SEP_A est un MenuSeparateur\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n",
        "FEN_Mini",
        "positif MenuContextuel nu + Separateur [feat/1164]");

    /* 57.  garde negative — variable globale APRES une fenetre a menu
     * contextuel doit rester false (le fix ne relaxe pas l'anti-silence). */
    expect_negative(
        "FEN_G est une Fenetre\n"
        "  PROPRIETES\n"
        "    Titre = \"G\"\n"
        "  FIN\n"
        "  MNU_G est un MenuContextuel\n"
        "    OPT_G est une OptionMenu\n"
        "      PROPRIETES\n"
        "        Libelle = \"X\"\n"
        "      FIN\n"
        "    FIN\n"
        "  FIN\n"
        "FIN\n"
        "gCompteur est un entier = 0\n",
        "garde negative variable globale apres FEN a menu contextuel [feat/1164]");

    /* ===  : `SI ... ALORS <instr>` mono-ligne (sans FIN, canon
     * valide) dans une PROCEDURE de Fenetre. Le tracker de profondeur
     * incremente sur TOKEN_SI en supposant systematiquement un FIN a venir ;
     * sans FIN, c'est le FIN de la Fenetre qui est consomme pour "fermer" le
     * SI -> le detecteur croit la fenetre encore ouverte -> false. Repro
     * exacte du support (Gilles, Telegram) : ces 6 cas DOIVENT devenir
     * positifs une fois le fix pose. === */

    /* 58. SI mono-ligne en DERNIERE instruction avant le FIN de la
     * PROCEDURE — repro exacte du signalement support. */
    expect_positive(
        "FEN_MonoDerniere est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tv est un entier = 0\n"
        "\t\tSI v <> 1 ALORS v = 2\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_MonoDerniere",
        "positif SI mono-ligne derniere instruction avant FIN [fix/1434]");

    /* 59. SI mono-ligne suivi d'une autre instruction (pas la derniere). */
    expect_positive(
        "FEN_MonoSuivi est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tv est un entier = 0\n"
        "\t\tSI v <> 1 ALORS v = 2\n"
        "\t\tTrace(\"apres\")\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_MonoSuivi",
        "positif SI mono-ligne suivi d'une instruction [fix/1434]");

    /* 60. SI mono-ligne avec SINON sur la meme ligne. */
    expect_positive(
        "FEN_MonoSinon est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tv est un entier = 0\n"
        "\t\tSI v <> 1 ALORS v = 2 SINON v = 3\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_MonoSinon",
        "positif SI mono-ligne avec SINON meme ligne [fix/1434]");

    /* 61. Deux SI mono-ligne consecutifs. */
    expect_positive(
        "FEN_MonoDouble est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tv est un entier = 0\n"
        "\t\tSI v <> 1 ALORS v = 2\n"
        "\t\tSI v <> 2 ALORS v = 3\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_MonoDouble",
        "positif deux SI mono-ligne consecutifs [fix/1434]");

    /* 62. SI mono-ligne imbrique dans un SI multi-ligne. */
    expect_positive(
        "FEN_MonoImbrique est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tv est un entier = 0\n"
        "\t\tSI v = 0 ALORS\n"
        "\t\t\tSI v <> 1 ALORS v = 2\n"
        "\t\tFIN\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_MonoImbrique",
        "positif SI mono-ligne imbrique dans SI multi-ligne [fix/1434]");

    /* 63. Non-regression : la forme multi-ligne `SI ... ALORS` + `FIN`
     * continue de fonctionner normalement (le fix ne doit pas la casser). */
    expect_positive(
        "FEN_MultiTemoin est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "\tPROCEDURE FinInitialisation()\n"
        "\t\tv est un entier = 0\n"
        "\t\tSI v <> 1 ALORS\n"
        "\t\t\tv = 2\n"
        "\t\tFIN\n"
        "\t\tTrace(\"apres\")\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_MultiTemoin",
        "temoin non-regression SI multi-ligne + FIN [fix/1434]");

    /* 64. Non-regression : une instruction nue (pas une declaration) au
     * meme niveau APRES la fenetre doit rester detectee comme contenu
     * supplementaire (le fix ne doit pas rendre le detecteur permissif au
     * point de ne plus rien attraper — 3e cause enumeree par le message
     * d'erreur, jusqu'ici seulement couverte par une variable globale). */
    expect_negative(
        "FEN_Instr est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"X\"\n"
        "\tFIN\n"
        "FIN\n"
        "Trace(\"residu\")\n",
        "garde negative instruction nue apres FEN [fix/1434]");

    /* ===  (suite) : meme forme de defaut que le SI mono-ligne, sur
     * d'autres tokens comptes inconditionnellement comme ouvreurs de bloc.
     *
     * `CONSTANTE` a deux formes : bloc (`CONSTANTE` seul en fin de ligne, puis
     * les declarations, puis FIN) et inline (`CONSTANTE K = 3`, sans FIN). Le
     * tracker comptait les deux -> avec la forme inline, le FIN de la Fenetre
     * est consomme pour "fermer" la constante -> detection false -> exit 65
     * anti-silence sur un fichier pourtant valide (`--check` CLEAN).
     *
     * `Procedure`/`Fonction` APRES la copule (`p est une Procedure = Compile(...)`)
     * sont des annotations de TYPE (variable porteuse de code), pas des handlers
     * `PROCEDURE Nom()` : aucun FIN ne suit. Meme desync +1. === */

    /* 65. CONSTANTE inline dans une PROCEDURE de Fenetre — repro exacte. */
    expect_positive(
        "FEN_CstInline est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"C\"\n"
        "\tFIN\n"
        "\tPROCEDURE Calc()\n"
        "\t\tCONSTANTE K = 3\n"
        "\t\tTrace(K)\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_CstInline",
        "positif CONSTANTE inline dans PROCEDURE de fenetre [fix/1434]");

    /* 66. CONSTANTE inline directement dans le corps de la Fenetre. */
    expect_positive(
        "FEN_CstCorps est une Fenetre\n"
        "\tCONSTANTE K = 3\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"C\"\n"
        "\tFIN\n"
        "\tPROCEDURE Calc()\n"
        "\t\tTrace(K)\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_CstCorps",
        "positif CONSTANTE inline dans le corps de la fenetre [fix/1434]");

    /* 67. Deux CONSTANTE inline consecutives (une par ligne). */
    expect_positive(
        "FEN_CstDouble est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"C\"\n"
        "\tFIN\n"
        "\tPROCEDURE Calc()\n"
        "\t\tCONSTANTE K = 3\n"
        "\t\tCONSTANTE L = 4\n"
        "\t\tTrace(K + L)\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_CstDouble",
        "positif deux CONSTANTE inline consecutives [fix/1434]");

    /* 68. Temoin non-regression : la forme BLOC `CONSTANTE` + FIN continue de
     * compter (le peek ne doit pas la reclasser en inline, sinon son FIN
     * sous-decremente depth et la detection casse dans l'autre sens). Cas 13
     * couvre la variante `NOM est un entier = v` ; ici la forme `NOM = v`. */
    expect_positive(
        "FEN_CstBloc est une Fenetre\n"
        "\tCONSTANTE\n"
        "\t\tK = 3\n"
        "\t\tL = 4\n"
        "\tFIN\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"C\"\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_CstBloc",
        "temoin non-regression bloc CONSTANTE + FIN [fix/1434]");

    /* 69. Temoin melange : bloc CONSTANTE au corps + inline dans un handler. */
    expect_positive(
        "FEN_CstMixte est une Fenetre\n"
        "\tCONSTANTE\n"
        "\t\tK = 3\n"
        "\tFIN\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"C\"\n"
        "\tFIN\n"
        "\tPROCEDURE Calc()\n"
        "\t\tCONSTANTE L = 4\n"
        "\t\tTrace(K + L)\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_CstMixte",
        "positif bloc CONSTANTE + inline dans un handler [fix/1434]");

    /* 70-72. Gardes negatives : le detecteur ne devient pas aveugle quand une
     * CONSTANTE inline est presente. Les 3 causes de contenu supplementaire
     * (2e fenetre / variable globale / instruction nue) restent refusees. */
    expect_negative(
        "FEN_CstNeg1 est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"A\"\n"
        "\tFIN\n"
        "\tPROCEDURE Calc()\n"
        "\t\tCONSTANTE K = 3\n"
        "\t\tTrace(K)\n"
        "\tFIN\n"
        "FIN\n"
        "FEN_CstNeg1b est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"B\"\n"
        "\tFIN\n"
        "FIN\n",
        "garde negative 2 fenetres avec CONSTANTE inline [fix/1434]");

    expect_negative(
        "FEN_CstNeg2 est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"A\"\n"
        "\tFIN\n"
        "\tPROCEDURE Calc()\n"
        "\t\tCONSTANTE K = 3\n"
        "\t\tTrace(K)\n"
        "\tFIN\n"
        "FIN\n"
        "gCompteur est un entier = 0\n",
        "garde negative variable globale apres FEN a CONSTANTE inline [fix/1434]");

    expect_negative(
        "FEN_CstNeg3 est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"A\"\n"
        "\tFIN\n"
        "\tPROCEDURE Calc()\n"
        "\t\tCONSTANTE K = 3\n"
        "\t\tTrace(K)\n"
        "\tFIN\n"
        "FIN\n"
        "Trace(\"residu\")\n",
        "garde negative instruction nue apres FEN a CONSTANTE inline [fix/1434]");

    /* 73. `p est une Procedure = Compile(...)` dans le corps de la Fenetre :
     * annotation de type, aucun FIN. */
    expect_positive(
        "FEN_ProcType est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"P\"\n"
        "\tFIN\n"
        "\tp est une Procedure = Compile(\"Trace(1)\")\n"
        "\tPROCEDURE Calc()\n"
        "\t\tTrace(\"ok\")\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_ProcType",
        "positif variable de type Procedure dans le corps [fix/1434]");

    /* 74. Meme annotation, sans initialisation, dans un handler (la ligne se
     * termine donc par un NEWLINE : la discrimination ne peut pas etre le
     * peek de fin de ligne, c'est la copule qui tranche). */
    expect_positive(
        "FEN_FoncType est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"F\"\n"
        "\tFIN\n"
        "\tPROCEDURE Calc()\n"
        "\t\tf est une Fonction\n"
        "\t\tTrace(\"ok\")\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_FoncType",
        "positif variable de type Fonction sans init dans un handler [fix/1434]");

    /* 75. Garde negative associee : variable globale apres une fenetre qui
     * contient une annotation `est une Procedure`. */
    expect_negative(
        "FEN_ProcNeg est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"P\"\n"
        "\tFIN\n"
        "\tp est une Procedure = Compile(\"Trace(1)\")\n"
        "FIN\n"
        "gCompteur est un entier = 0\n",
        "garde negative variable globale apres FEN a annotation Procedure [fix/1434]");

    /* 76. Temoin non-regression : les mots-cles structurels APRES la copule
     * (`E est une Enumeration`, `st est une Structure`) ouvrent, eux, un vrai
     * bloc ferme par FIN dans le corps d'une fenetre. Le fix Procedure/Fonction
     * ne doit pas les degrader. */
    expect_positive(
        "FEN_EnumBloc est une Fenetre\n"
        "\tPROPRIETES\n"
        "\t\tTitre = \"E\"\n"
        "\tFIN\n"
        "\tEStatut est une Enumeration\n"
        "\t\tActif\n"
        "\t\tInactif\n"
        "\tFIN\n"
        "\tstP est une Structure\n"
        "\t\tn est un entier\n"
        "\tFIN\n"
        "FIN\n",
        "FEN_EnumBloc",
        "temoin non-regression Enumeration/Structure imbriquees (blocs) [fix/1434]");

    /* Garde ensemble vide : "go_local: 0 PASS" est un vert vide. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "go_local: CORPUS VIDE — 0 cas execute. Un controle "
                        "qui n'examine rien ne prouve rien.\n");
        return 1;
    }
    if (g_fail > 0) {
        fprintf(stderr, "go_local: %d FAIL, %d PASS\n", g_fail, g_pass);
    } else {
        fprintf(stdout, "go_local: %d PASS\n", g_pass);
    }
    return g_fail;
}
