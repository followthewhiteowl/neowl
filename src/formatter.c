// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "formatter.h"
#include "formatter_canon.h"
#include "owl_name_normalize.h"
#include "common.h"
#include "scanner.h"
#include "prescan.h"   /* prescan_depth_delta_text — porte texte de la source unique */
#include "ui_decl.h"
#include "owl_event_catalog.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Provider de formes canoniques (natives, types, consts, methodes, DSL).
 * Pose par le binaire hote : owl.exe -> backend registry (apres vm_init),
 * owl-lsp.exe -> backend catalogue. NULL avant init : la canonisation des
 * symboles builtin est neutralisee, le reste du formatter fonctionne. */
const OwlFmtCanon* owl_fmt_canon = NULL;

void owl_format_set_canon(const OwlFmtCanon* canon) {
    owl_fmt_canon = canon;
}

/* ================================================================
 * DYNAMIC STRING BUFFER
 * ================================================================ */

typedef struct {
    char* data;
    int count;
    int capacity;
} Buffer;

static void buf_init(Buffer* b) {
    b->data = NULL;
    b->count = 0;
    b->capacity = 0;
}

static void buf_ensure(Buffer* b, int extra) {
    if (b->count + extra <= b->capacity) return;
    int newCap = b->capacity < 256 ? 256 : b->capacity * 2;
    while (newCap < b->count + extra) newCap *= 2;
    b->data = realloc(b->data, newCap);
    b->capacity = newCap;
}

static void buf_write(Buffer* b, const char* str, int len) {
    buf_ensure(b, len);
    memcpy(b->data + b->count, str, len);
    b->count += len;
}

static void buf_puts(Buffer* b, const char* str) {
    buf_write(b, str, (int)strlen(str));
}

static void buf_putc(Buffer* b, char c) {
    buf_ensure(b, 1);
    b->data[b->count++] = c;
}

static char* buf_finish(Buffer* b) {
    buf_putc(b, '\0');
    return b->data;
}

/* ================================================================
 * CANONICAL KEYWORD LOOKUP
 *
 * We match words case-insensitively and replace with canonical form.
 * This does NOT depend on the scanner — it works on raw text.
 * ================================================================ */

/*  : langue d'une entree fmt_keywords[]. NEUTRE = mot de surface
 * sans canon bilingue oppose (type primitif partage date/buffer/octet,
 * fillers un/une/le/la, constantes magiques RC/TAB/Vrai/Faux/Null,
 * operateurs bit*). Pour ces entrees `partner` reste NULL -> translate
 * laisse la forme inchangee (pass-through). */
#define KW_NEUTRE  0
#define KW_FR      1
#define KW_EN      2

typedef struct {
    const char* normalized;  /* lowercase match */
    const char* canonical;   /* replacement text */
    int category;            /* 0=control, 1=decl, 2=type, 3=bool, 4=oop, 5=filler */
    int lang;                /*  : KW_FR / KW_EN / KW_NEUTRE */
    const char* partner;     /*  : canon de la langue opposee, NULL si neutre */
    /*  : genre grammatical du nom FR (GENRE_M / GENRE_F, 0 = non
     * renseigne). Consomme UNIQUEMENT par `owl translate` pour accorder
     * l'article EN->FR (`a string` -> `une chaîne`). Renseigne sur les mots
     * qui peuvent suivre l'article ET n'ont pas d'autre enregistrement source
     * (primitifs du langage, Classe/Structure/Combinaison/Énumération). Les
     * mots qui SONT une sorte de widget ou de bloc portent leur genre sur
     * CONTROL_KINDS (ui_decl.c) et laissent cette colonne a 0 : une seule
     * ligne de verite par mot. */
    int genre;
} FmtKeyword;

/* Genre grammatical (valeurs OwlGenre encodees en int, cf. owl_registry.h).
 * Declares ici car fmt_keywords[] les consomme des sa definition. */
#define GENRE_M 1
#define GENRE_F 2

/* Category definitions for indent tracking */
#define CAT_CONTROL  0
#define CAT_DECL     1
#define CAT_TYPE     2
#define CAT_BOOL     3
#define CAT_OOP      4
#define CAT_FILLER   5
#define CAT_LOGICAL  6
/* Formes de surface heritees, canonisees vers un symbole du langage sans etre
 * elles-memes des symboles (ni registre, ni keyword scanner). Traitees comme
 * CAT_BOOL par la canonisation et la garde shadow-variable, mais exclues de
 * owl_formatter_magic_const_canonical() : on n'avertit pas qu'une variable
 * shadow un nom qui n'existe plus dans le langage. */
#define CAT_EOL_LEGACY 7

/*  : colonnes `lang` (KW_FR/KW_EN/KW_NEUTRE) + `partner` (canon de
 * la langue opposee, NULL si neutre ou sans pairing 1:1). owl_translate
 * emet `partner` quand la langue cible differe de `lang` et que partner !=
 * NULL ; sinon il emet `canonical` (== comportement formatter). Un groupe de
 * synonymes n'a qu'un canon par langue : les formes secondaires pointent le
 * canon oppose via `partner`, le collapse se fait donc au changement de
 * langue. */
static const FmtKeyword fmt_keywords[] = {
    /* Control flow — UPPERCASE */
    /* Opérateurs binaires (canon). Forme accentuée canon.
     * NEUTRE : un seul canon (forme FR accentuee) pour les deux ecritures. */
    {"bitdecalegauche", "bitD\xc3\xa9""caleGauche", CAT_LOGICAL, KW_NEUTRE, NULL},  /* bitDécaleGauche */
    {"bitshiftleft",    "bitD\xc3\xa9""caleGauche", CAT_LOGICAL, KW_NEUTRE, NULL},
    {"bitdecaledroite", "bitD\xc3\xa9""caleDroite", CAT_LOGICAL, KW_NEUTRE, NULL},  /* bitDécaleDroite */
    {"bitshiftright",   "bitD\xc3\xa9""caleDroite", CAT_LOGICAL, KW_NEUTRE, NULL},

    /*  : formatter bilingue-preservant. Chaque entree emet le canon
     * de SA langue d'ecriture (EN reste EN, FR reste FR — comme les natives).
     * Canon EN = casse EXACTE du canon FR miroir (controle de flux + classes
     * de stockage en MAJUSCULES ; copule/article/preposition en minuscules ;
     * booleens en PascalCase). Zero traduction FR<->EN. */
    {"si",          "SI",          CAT_CONTROL, KW_FR, "IF"},
    {"if",          "IF",          CAT_CONTROL, KW_EN, "SI"},
    {"alors",       "ALORS",       CAT_CONTROL, KW_FR, "THEN"},
    {"then",        "THEN",        CAT_CONTROL, KW_EN, "ALORS"},
    {"sinon",       "SINON",       CAT_CONTROL, KW_FR, "ELSE"},
    {"else",        "ELSE",        CAT_CONTROL, KW_EN, "SINON"},
    {"fin",         "FIN",         CAT_CONTROL, KW_FR, "END"},
    {"end",         "END",         CAT_CONTROL, KW_EN, "FIN"},
    {"pour",        "POUR",        CAT_CONTROL, KW_FR, "FOR"},
    {"for",         "FOR",         CAT_CONTROL, KW_EN, "POUR"},
    {"pas",         "PAS",         CAT_CONTROL, KW_FR, "STEP"},
    {"step",        "STEP",        CAT_CONTROL, KW_EN, "PAS"},
    {"not",         "NOT",         CAT_CONTROL, KW_NEUTRE, NULL},
    /*  — borne inclusive de plage (POUR i = 1 À 5, CAS x À y). Le
     * chemin qui canonise reellement `a`/`to` est le cas special en tete de
     * boucle (bornes de plage, seenPourEquals/seenCas) ; ces entrees donnent
     * a `_a_`/`_to_` (borne evaluee une fois) leur canonisation ET fournissent
     * a la table la fiche keyword que le self-test fmt-canon confronte. Canon
     * FR accentue majuscule (À), miroir EN TO — cf. scanner TOKEN_A/TOKEN_A_SC. */
    {"a",           "\xc3\x80",    CAT_CONTROL, KW_FR, "TO"},     /* À */
    {"to",          "TO",          CAT_CONTROL, KW_EN, "\xc3\x80"},
    {"_a_",         "_\xc3\x80_",  CAT_CONTROL, KW_FR, "_TO_"},   /* _À_ */
    {"_to_",        "_TO_",        CAT_CONTROL, KW_EN, "_\xc3\x80_"},
    /*  — Iteration : les sept formes sont strictement equivalentes,
     * le groupe a donc UN canon par langue, TOUT cote FR et EACH cote EN.
     * Les formes secondaires (TOUTE/TOUS/CHAQUE, EVERY/ALL) restent des mots
     * du langage a l'entree, et le collapse ne se declenche qu'au CHANGEMENT
     * de langue (via partner) : une cible egale a la langue du fichier laisse
     * la forme ecrite intacte. Borne volontaire — normaliser aussi a
     * l'interieur d'une langue casserait l'accord grammatical francais
     * (`POUR TOUTE chaîne x` deviendrait `POUR TOUT chaîne x`).
     * Contrepartie assumee ( amende) : l'aller-retour d'une variante
     * traverse deux changements de langue et rend le canon du groupe, pas la
     * forme d'origine. */
    {"tout",        "TOUT",        CAT_CONTROL, KW_FR, "EACH"},
    {"toute",       "TOUTE",       CAT_CONTROL, KW_FR, "EACH"},
    {"tous",        "TOUS",        CAT_CONTROL, KW_FR, "EACH"},
    {"chaque",      "CHAQUE",      CAT_CONTROL, KW_FR, "EACH"},
    {"each",        "EACH",        CAT_CONTROL, KW_EN, "TOUT"},
    {"every",       "EVERY",       CAT_CONTROL, KW_EN, "TOUT"},
    {"all",         "ALL",         CAT_CONTROL, KW_EN, "TOUT"},
    /*  — mots optionnels d'iteration. ÉLÉMENT (`POUR TOUT ÉLÉMENT x
     * DE t`) partage le meme mot en FR/EN au strip d'accent : une seule entree
     * KW_FR (canon accentue majuscule) dont le partner porte la forme EN sans
     * accent, comme Énumération/Enumeration. SÉPARÉE (`... SÉPARÉE PAR <sep>`)
     * a une graphie EN distincte cote scanner (SEPARATED) : paire complete. */
    {"element",     "\xc3\x89L\xc3\x89MENT", CAT_CONTROL, KW_FR, "ELEMENT"},   /* ÉLÉMENT */
    {"separee",     "S\xc3\x89PAR\xc3\x89""E", CAT_CONTROL, KW_FR, "SEPARATED"},   /* SÉPARÉE */
    {"separated",   "SEPARATED",   CAT_CONTROL, KW_EN, "S\xc3\x89PAR\xc3\x89""E"},
    {"dans",        "DANS",        CAT_CONTROL, KW_FR, "IN"},
    {"in",          "IN",          CAT_CONTROL, KW_EN, "DANS"},
    {"tantque",     "TANTQUE",     CAT_CONTROL, KW_FR, "WHILE"},
    {"while",       "WHILE",       CAT_CONTROL, KW_EN, "TANTQUE"},
    {"boucle",      "BOUCLE",      CAT_CONTROL, KW_FR, "LOOP"},
    {"loop",        "LOOP",        CAT_CONTROL, KW_EN, "BOUCLE"},
    {"faire",       "FAIRE",       CAT_CONTROL, KW_FR, "DO"},
    {"do",          "DO",          CAT_CONTROL, KW_EN, "FAIRE"},
    {"selon",       "SELON",       CAT_CONTROL, KW_FR, "SWITCH"},
    {"switch",      "SWITCH",      CAT_CONTROL, KW_EN, "SELON"},
    {"cas",         "CAS",         CAT_CONTROL, KW_FR, "CASE"},
    {"case",        "CASE",        CAT_CONTROL, KW_EN, "CAS"},
    {"autre",       "AUTRE",       CAT_CONTROL, KW_FR, "OTHER"},
    {"other",       "OTHER",       CAT_CONTROL, KW_EN, "AUTRE"},
    {"default",     "OTHER",       CAT_CONTROL, KW_EN, "AUTRE"},
    {"sortir",      "SORTIR",      CAT_CONTROL, KW_FR, "BREAK"},
    {"break",       "BREAK",       CAT_CONTROL, KW_EN, "SORTIR"},
    {"exit",        "EXIT",        CAT_CONTROL, KW_NEUTRE, NULL},
    {"continuer",   "CONTINUER",   CAT_CONTROL, KW_FR, "CONTINUE"},
    {"continue",    "CONTINUE",    CAT_CONTROL, KW_EN, "CONTINUER"},
    {"renvoyer",    "RENVOYER",    CAT_CONTROL, KW_FR, "RESULT"},
    {"result",      "RESULT",      CAT_CONTROL, KW_EN, "RENVOYER"},
    {"retour",      "RETOUR",      CAT_CONTROL, KW_FR, "RETURN"},
    {"return",      "RETURN",      CAT_CONTROL, KW_EN, "RETOUR"},
    {"goto",        "GOTO",        CAT_CONTROL, KW_NEUTRE, NULL},
    {"avec",        "AVEC",        CAT_CONTROL, KW_FR, "WITH"},
    {"with",        "WITH",        CAT_CONTROL, KW_EN, "AVEC"},
    {"sur",         "SUR",         CAT_CONTROL, KW_FR, "ON"},
    {"on",          "ON",          CAT_CONTROL, KW_EN, "SUR"},
    {"quand",       "QUAND",       CAT_CONTROL, KW_FR, "WHEN"},
    {"when",        "WHEN",        CAT_CONTROL, KW_EN, "QUAND"},
    {"exception",   "EXCEPTION",   CAT_CONTROL, KW_NEUTRE, NULL},
    {"enfin",       "ENFIN",       CAT_CONTROL, KW_FR, "FINALLY"},
    {"finally",     "FINALLY",     CAT_CONTROL, KW_EN, "ENFIN"},

    /* Declaration */
    {"est",         "est",         CAT_DECL, KW_FR, "is"},
    {"is",          "is",          CAT_DECL, KW_EN, "est"},
    {"sont",        "sont",        CAT_DECL, KW_FR, "are"},
    {"are",         "are",         CAT_DECL, KW_EN, "sont"},
    {"de",          "de",          CAT_DECL, KW_FR, "of"},
    {"of",          "of",          CAT_DECL, KW_EN, "de"},
    /*  — preserve orthographe ecrite : `constante` reste CONSTANTE,
     * `constant` reste CONSTANT (deux formes synonymes). Le formatter
     * ne fait que passer en majuscules.
     *  : `constant` est la forme EN canon ; pairing EN<->FR vers
     * CONSTANTE (forme FR longue). */
    {"constante",   "CONSTANTE",   CAT_DECL, KW_FR, "CONSTANT"},
    {"constant",    "CONSTANT",    CAT_DECL, KW_EN, "CONSTANTE"},
    {"local",       "LOCAL",       CAT_DECL, KW_NEUTRE, NULL},
    {"global",      "GLOBAL",      CAT_DECL, KW_NEUTRE, NULL},
    /* Bloc PROPRIETES des widgets DSL : keyword structurel bilingue.
     * Canon FR = PROPRIETES (majuscules sans accent, cf. fixtures), EN =
     * PROPERTIES. L'alignement des '=' (align_proprietes_blocks) reconnait
     * deja les deux formes ; ici on assure la traduction du mot-cle lui-meme. */
    {"proprietes",  "PROPRIETES",  CAT_DECL, KW_FR, "PROPERTIES"},
    {"properties",  "PROPERTIES",  CAT_DECL, KW_EN, "PROPRIETES"},

    /* DSL Fenetre v1 (Phase G) : keywords structurels avec accents canon.
     * Fenêtre, Libellé, Bouton, Saisie. Le formatter restaure la forme
     * accentuee depuis l'ASCII strippe par le scanner. */
    /*  lot2 : widgets DSL bilingue-preservants. Chaque forme EN
     * emet son nameEN registry (ui_decl.c) au lieu de rabattre vers le FR. */
    {"fenetre",     "Fen\xc3\xaatre",   CAT_DECL, KW_FR, "Window"},   /* Fenêtre */
    {"window",      "Window",           CAT_DECL, KW_EN, "Fen\xc3\xaatre"},   /* nameEN canon */
    {"libelle",     "Libell\xc3\xa9",   CAT_DECL, KW_FR, "Label"},   /* Libellé */
    {"label",       "Label",            CAT_DECL, KW_EN, "Libell\xc3\xa9"},
    {"bouton",      "Bouton",           CAT_DECL, KW_FR, "Button"},
    {"button",      "Button",           CAT_DECL, KW_EN, "Bouton"},
    {"saisie",      "Saisie",           CAT_DECL, KW_FR, "Input"},
    {"input",       "Input",            CAT_DECL, KW_EN, "Saisie"},
    {"conteneur",   "Conteneur",        CAT_DECL, KW_FR, "Container"},   /* Lot F */
    {"container",   "Container",        CAT_DECL, KW_EN, "Conteneur"},
    /* Champ Table + sous-bloc Colonne. `table`/`colonne`/`column` sont des
     * keywords scanner (TOKEN_TABLE / TOKEN_COLONNE), emis verbatim hors de
     * ce chemin avant  ; ajoutes ici pour canoniser la casse comme
     * libelle/bouton/saisie. La detection du sous-bloc colonne (indentation)
     * reste assuree par word_opens_block via owl_control_kinds_table — chemin
     * independant, non impacte. table EN=FR (Table). column EN preserve en
     * Column (nameEN PascalCase, ui_decl.c). */
    {"table",       "Table",            CAT_DECL, KW_NEUTRE, NULL},   /* Table EN=FR */
    {"colonne",     "Colonne",          CAT_DECL, KW_FR, "Column"},
    {"column",      "Column",           CAT_DECL, KW_EN, "Colonne"},
    /*  — sous-bloc Volet d'un Champ Onglet. Comme Colonne, c'est une
     * sorte de role ecrite SANS prefixe `Champ` : le pairing keyword est donc
     * le seul chemin qui la fait basculer (le chemin CONTROL_KINDS ne couvre
     * que la position `Champ <sorte>`). Sans cette paire, la sorte restait FR
     * pendant que l'article basculait -> `est un Volet` / `is a Volet` /
     * `est a Volet`, round-trip rompu. */
    {"volet",       "Volet",            CAT_DECL, KW_FR, "Pane"},
    {"pane",        "Pane",             CAT_DECL, KW_EN, "Volet"},
    /* Champ Liste / Combo / Selecteur / Interrupteur — formes canon. */
    {"liste",       "Liste",            CAT_DECL, KW_FR, "List"},
    {"list",        "List",             CAT_DECL, KW_EN, "Liste"},
    {"combo",       "Combo",            CAT_DECL, KW_FR, "ComboBox"},   /* nameEN canon = ComboBox (registry) */
    {"combobox",    "ComboBox",         CAT_DECL, KW_EN, "Combo"},
    {"selecteur",   "S\xc3\xa9lecteur", CAT_DECL, KW_FR, "RadioGroup"},   /* Sélecteur */
    {"radiogroup",  "RadioGroup",       CAT_DECL, KW_EN, "S\xc3\xa9lecteur"},
    {"interrupteur","Interrupteur",     CAT_DECL, KW_FR, "CheckBox"},
    {"checkbox",    "CheckBox",         CAT_DECL, KW_EN, "Interrupteur"},
    /* Champ ZoneRepetee — canon FR avec accents (ZoneRépétée). */
    {"zonerepetee", "ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "e", CAT_DECL, KW_FR, "Looper"},   /* ZoneRépétée */
    {"looper",      "Looper",           CAT_DECL, KW_EN, "ZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9" "e"},
    /*  — menu classique de fenetre. Blocs structurels (cf.
     * CONTROL_KINDS is_widget=false) : le formatter restaure la forme canon
     * de chaque mot-cle, et `surSélection` (nom d'event canon) avec eux —
     * sans lui, `sursélection` resterait tel quel apres save. */
    {"menuprincipal", "MenuPrincipal",  CAT_DECL, KW_FR, "MainMenu"},
    {"mainmenu",      "MainMenu",       CAT_DECL, KW_EN, "MenuPrincipal"},
    {"menucontextuel","MenuContextuel", CAT_DECL, KW_FR, "ContextMenu"},   /*  */
    {"contextmenu",   "ContextMenu",    CAT_DECL, KW_EN, "MenuContextuel"},
    {"sousmenu",      "SousMenu",       CAT_DECL, KW_FR, "Submenu"},
    {"submenu",       "Submenu",        CAT_DECL, KW_EN, "SousMenu"},
    {"optionmenu",    "OptionMenu",     CAT_DECL, KW_FR, "MenuOption"},
    {"menuoption",    "MenuOption",     CAT_DECL, KW_EN, "OptionMenu"},
    {"menuseparateur","MenuS\xc3\xa9parateur", CAT_DECL, KW_FR, "MenuSeparator"},   /* MenuSéparateur */
    {"menuseparator", "MenuSeparator",  CAT_DECL, KW_EN, "MenuS\xc3\xa9parateur"},
    {"surselection",  "surS\xc3\xa9lection", CAT_DECL, KW_FR, "onSelection"},   /* surSélection */
    {"onselection",   "onSelection",    CAT_DECL, KW_EN, "surS\xc3\xa9lection"},

    /*  — blocs de l'Etat imprimable (declaration + sous-blocs). Blocs
     * structurels (CONTROL_KINDS is_widget=false) : miroir cote translate des
     * formes EN que le compilo accepte deja (Report/Body/DocumentHeader/...).
     * `Etat` collisionne avec la propriete unifiee `Etat`/`State` : la garde
     * d'homonymie (position `<prop> =` en tete de ligne) resout la propriete
     * via find_dsl_prop_norm AVANT ce pairing, comme pour Libelle/Bouton. */
    {"etat",           "\xc3\x89tat",    CAT_DECL, KW_FR, "Report"},   /* État */
    {"report",         "Report",         CAT_DECL, KW_EN, "\xc3\x89tat"},
    {"corps",          "Corps",          CAT_DECL, KW_FR, "Body"},
    {"body",           "Body",           CAT_DECL, KW_EN, "Corps"},
    {"entetedocument", "EnT\xc3\xaateDocument", CAT_DECL, KW_FR, "DocumentHeader"},   /* EnTêteDocument */
    {"documentheader", "DocumentHeader", CAT_DECL, KW_EN, "EnT\xc3\xaateDocument"},
    {"hautdepage",     "HautDePage",     CAT_DECL, KW_FR, "PageHeader"},
    {"pageheader",     "PageHeader",     CAT_DECL, KW_EN, "HautDePage"},
    {"basdepage",      "BasDePage",      CAT_DECL, KW_FR, "PageFooter"},
    {"pagefooter",     "PageFooter",     CAT_DECL, KW_EN, "BasDePage"},
    {"findedocument",  "FinDeDocument",  CAT_DECL, KW_FR, "DocumentFooter"},
    {"documentfooter", "DocumentFooter", CAT_DECL, KW_EN, "FinDeDocument"},
    /* Itération <-> Iteration : cle normalisee unique ("iteration" ASCII pour
     * les deux formes), meme montage qu'Énumération. Une seule entree KW_FR
     * dont le canon porte l'accent ; le partner sert la forme EN. */
    {"iteration",      "It\xc3\xa9ration", CAT_DECL, KW_FR, "Iteration"},   /* Itération */

    /* DSL de service HTTP : bloc ServiceREST + sous-bloc opération + handler.
     * Blocs structurels (pas des sortes de widget) : le formatter restaure la
     * forme canon bilingue. `ServiceREST` est language-neutral (une seule
     * forme). `OpérationHTTP`/`HTTPOperation` et le handler `surRequête`/
     * `onRequest` (calque surSélection/onSelection ci-dessus) portent la paire
     * FR⇄EN. La clé normalisée strip accent+casse : « OpérationHTTP » ->
     * « operationhttp », « HTTPOperation » -> « httpoperation » (clés
     * distinctes, aucune collision). */
    {"servicerest",    "ServiceREST",    CAT_DECL, KW_NEUTRE, NULL},
    {"operationhttp",  "Op\xc3\xa9rationHTTP", CAT_DECL, KW_FR, "HTTPOperation"},   /* OpérationHTTP */
    {"httpoperation",  "HTTPOperation",  CAT_DECL, KW_EN, "Op\xc3\xa9rationHTTP"},
    {"surrequete",     "surRequ\xc3\xaate", CAT_DECL, KW_FR, "onRequest"},   /* surRequête */
    {"onrequest",      "onRequest",      CAT_DECL, KW_EN, "surRequ\xc3\xaate"},

    /* Types — lowercase, canonical forms KEEP the accents.
     *  : 6e colonne = genre grammatical du nom FR, sur la forme
     * SINGULIERE FR et EN (seule position ou un article precede : `a string`
     * -> `une chaîne`). Les formes plurielles/accordees ne suivent jamais un
     * article et gardent 0. */
    {"booleen",     "bool\xc3\xa9""en",     CAT_TYPE, KW_FR, "boolean", GENRE_M},   /* un booléen */
    {"booleenne",   "bool\xc3\xa9""enne",   CAT_TYPE, KW_FR, "boolean"},
    {"booleens",    "bool\xc3\xa9""ens",    CAT_TYPE, KW_FR, "boolean"},
    {"booleennes",  "bool\xc3\xa9""ennes",  CAT_TYPE, KW_FR, "boolean"},
    {"boolean",     "boolean",             CAT_TYPE, KW_EN, "bool\xc3\xa9""en", GENRE_M},   /* nameEN canon */
    {"booleans",    "booleans",            CAT_TYPE, KW_EN, "bool\xc3\xa9""en"},
    {"entier",      "entier",              CAT_TYPE, KW_FR, "integer", GENRE_M},   /* un entier */
    {"entiers",     "entiers",             CAT_TYPE, KW_FR, "integer"},
    {"entiere",     "enti\xc3\xa8re",       CAT_TYPE, KW_FR, "integer"},   /* entière */
    {"entieres",    "enti\xc3\xa8res",      CAT_TYPE, KW_FR, "integer"},
    {"integer",     "integer",             CAT_TYPE, KW_EN, "entier", GENRE_M},
    {"integers",    "integers",            CAT_TYPE, KW_EN, "entier"},
    {"reel",        "r\xc3\xa9""el",        CAT_TYPE, KW_FR, "real", GENRE_M},   /* un réel */
    {"reels",       "r\xc3\xa9""els",       CAT_TYPE, KW_FR, "real"},
    {"real",        "real",                CAT_TYPE, KW_EN, "r\xc3\xa9""el", GENRE_M},
    {"reals",       "reals",               CAT_TYPE, KW_EN, "r\xc3\xa9""el"},
    {"monetaire",   "mon\xc3\xa9taire",     CAT_TYPE, KW_FR, "currency", GENRE_M},   /* un monétaire */
    {"monetaires",  "mon\xc3\xa9taires",    CAT_TYPE, KW_FR, "currency"},
    {"currency",    "currency",            CAT_TYPE, KW_EN, "mon\xc3\xa9taire", GENRE_M},
    {"numerique",   "num\xc3\xa9rique",     CAT_TYPE, KW_FR, "numeric", GENRE_M},   /* un numérique */
    {"numeriques",  "num\xc3\xa9riques",    CAT_TYPE, KW_FR, "numeric"},
    {"numeric",     "numeric",             CAT_TYPE, KW_EN, "num\xc3\xa9rique", GENRE_M},
    /* The canonical form keeps the circumflex on "chaîne" — this is the
     * official spelling, so owl fmt enforces it. */
    {"chaine",      "cha\xc3\xaene",   CAT_TYPE, KW_FR, "string", GENRE_F},    /* une chaîne */
    {"chaines",     "cha\xc3\xaenes",  CAT_TYPE, KW_FR, "string"},    /* chaînes */
    {"string",      "string",          CAT_TYPE, KW_EN, "cha\xc3\xaene", GENRE_F},
    {"strings",     "strings",         CAT_TYPE, KW_EN, "cha\xc3\xaene"},
    {"caractere",   "caract\xc3\xa8re",     CAT_TYPE, KW_FR, "character", GENRE_M},   /* un caractère */
    {"character",   "character",            CAT_TYPE, KW_EN, "caract\xc3\xa8re", GENRE_M},
    {"date",        "date",                CAT_TYPE, KW_NEUTRE, NULL, GENRE_F},   /* une date — EN=FR */
    {"heure",       "heure",               CAT_TYPE, KW_FR, "time", GENRE_F},   /* une heure */
    {"time",        "time",                CAT_TYPE, KW_EN, "heure", GENRE_F},
    {"dateheure",   "dateheure",           CAT_TYPE, KW_FR, "datetime", GENRE_F},   /* une date-heure */
    {"datetime",    "datetime",            CAT_TYPE, KW_EN, "dateheure", GENRE_F},
    {"duree",       "dur\xc3\xa9""e",       CAT_TYPE, KW_FR, "duration", GENRE_F},   /* une durée */
    {"duration",    "duration",            CAT_TYPE, KW_EN, "dur\xc3\xa9""e", GENRE_F},
    {"buffer",      "buffer",      CAT_TYPE, KW_NEUTRE, NULL, GENRE_M},   /* un buffer — EN=FR */
    {"uuid",        "UUID",        CAT_TYPE, KW_NEUTRE, NULL},            /* UUID — always uppercase (canonical form) ; genre porte par le type reifie du registre */
    {"octet",       "octet",       CAT_TYPE, KW_FR, "byte", GENRE_M},   /* un octet */
    {"octets",      "octets",      CAT_TYPE, KW_FR, "byte"},
    {"byte",        "byte",        CAT_TYPE, KW_EN, "octet", GENRE_M},
    {"bytes",       "bytes",       CAT_TYPE, KW_EN, "octet"},
    {"sans",        "sans",        CAT_TYPE, KW_FR, "unsigned"},
    {"unsigned",    "unsigned",    CAT_TYPE, KW_EN, "sans"},
    {"signe",       "signe",       CAT_TYPE, KW_NEUTRE, NULL},

    /* Composite types */
    {"tableau",     "tableau",     CAT_TYPE, KW_FR, "array", GENRE_M},   /* un tableau */
    {"tableaux",    "tableaux",    CAT_TYPE, KW_FR, "array"},
    {"array",       "array",       CAT_TYPE, KW_EN, "tableau", GENRE_M},
    {"structure",   "Structure",   CAT_TYPE, KW_NEUTRE, NULL, GENRE_F},   /* une structure — EN=FR */
    /*  — Énumération <-> Enumeration. Cle normalisee unique
     * ("enumeration" ASCII pour les deux formes) : une seule entree KW_FR dont
     * fmt_kw_emit_form emet le canon accentue en FR/NONE et la forme EN
     * (Enumeration, acceptee par le compilo) quand la cible est EN. */
    {"enumeration", "\xc3\x89num\xc3\xa9ration", CAT_TYPE, KW_FR, "Enumeration", GENRE_F},   /* Énumération — canonical form with accents ; une énumération */
    {"combinaison", "Combinaison", CAT_TYPE, KW_FR, "Combination", GENRE_F},   /* une combinaison */
    {"combination", "Combination", CAT_TYPE, KW_EN, "Combinaison", GENRE_F},
    /* Canonical form keeps these collection types lowercase: "une pile de entiers". */
    {"file",        "file",        CAT_TYPE, KW_NEUTRE, NULL},   /* file EN=FR */
    {"queue",       "queue",       CAT_TYPE, KW_NEUTRE, NULL},
    {"pile",        "pile",        CAT_TYPE, KW_FR, "stack"},
    {"stack",       "stack",       CAT_TYPE, KW_EN, "pile"},
    {"liste",       "liste",       CAT_TYPE, KW_FR, "list"},
    {"list",        "list",        CAT_TYPE, KW_EN, "liste"},
    {"associatif",  "associatif",  CAT_TYPE, KW_FR, "associative"},
    {"associative", "associative", CAT_TYPE, KW_EN, "associatif"},
    {"fixe",        "fixe",        CAT_TYPE, KW_FR, "fixed"},
    {"fixed",       "fixed",       CAT_TYPE, KW_EN, "fixe"},
    {"dynamique",   "dynamique",   CAT_TYPE, KW_FR, "dynamic"},
    {"dynamic",     "dynamic",     CAT_TYPE, KW_EN, "dynamique"},
    {"par",         "par",         CAT_TYPE, KW_FR, "by"},
    {"by",          "by",          CAT_TYPE, KW_EN, "par"},

    /* Procedures — UPPERCASE */
    {"procedure",   "PROCEDURE",   CAT_CONTROL, KW_NEUTRE, NULL},   /* PROCEDURE EN=FR */
    {"fonction",    "FONCTION",    CAT_CONTROL, KW_FR, "FUNCTION"},
    {"function",    "FUNCTION",    CAT_CONTROL, KW_EN, "FONCTION"},

    /* OOP —  lot2 : miroir STRICT du FR. Canon EN = casse exacte
     * du canon FR equivalent (zero traduction). On herite de l'incoherence
     * FR existante (Classe/Herite/Liberer PascalCase vs implemente/detruire
     * non canonises), on ne la corrige PAS ici. Les ACCENTS, eux, sont
     * alignes sur la fiche keyword (HÉRITE / LIBÉRER) : deux formes d'un
     * meme mot dont l'une est reecrite par le save, c'est l'invariant 4
     * casse. La casse reste celle du formatter. */
    {"classe",      "Classe",      CAT_OOP, KW_FR, "Class", GENRE_F},   /* une classe */
    {"class",       "Class",       CAT_OOP, KW_EN, "Classe", GENRE_F},
    {"objet",       "objet",       CAT_OOP, KW_FR, "object"},
    {"this",        "this",        CAT_OOP, KW_NEUTRE, NULL},
    {"object",      "object",      CAT_OOP, KW_EN, "objet"},
    {"herite",      "H\xc3\xa9rite",  CAT_OOP, KW_FR, "Inherits"},   /* Hérite */
    {"inherits",    "Inherits",    CAT_OOP, KW_EN, "H\xc3\xa9rite"},
    /* Emetteur / champ courant / fenetre courante : canon FR mixte accentue
     * (MoiMême / MaFenêtre), partner EN mixte (MySelf / MyControl / MyWindow).
     * Cle = forme accent-strip lowercase (normBuf). */
    {"moimeme",     "MoiM\xc3\xaa""me",   CAT_OOP, KW_FR, "MySelf"},
    {"myself",      "MySelf",             CAT_OOP, KW_EN, "MoiM\xc3\xaa""me"},
    {"monchamp",    "MonChamp",           CAT_OOP, KW_FR, "MyControl"},
    {"mycontrol",   "MyControl",          CAT_OOP, KW_EN, "MonChamp"},
    {"mafenetre",   "MaFen\xc3\xaa""tre", CAT_OOP, KW_FR, "MyWindow"},
    {"mywindow",    "MyWindow",           CAT_OOP, KW_EN, "MaFen\xc3\xaa""tre"},
    {"interface",   "Interface",   CAT_OOP, KW_NEUTRE, NULL},   /* Interface EN=FR */
    {"implemente",  "implemente",  CAT_OOP, KW_NEUTRE, NULL},
    /* implements : le FR `implemente` n'est PAS canonise (pass-through casse
     * preservee) -> l'EN `implements` non plus. Volontairement ABSENT de la
     * table (miroir du non-traitement FR). NE PAS ajouter. */
    {"public",      "PUBLIC",      CAT_OOP, KW_NEUTRE, NULL},   /* PUBLIC EN=FR */
    {"publique",    "PUBLIC",      CAT_OOP, KW_NEUTRE, NULL},
    /* Visibilite : forme canon FR = MASCULIN accentue (marqueur de bloc
     * « PRIVÉ » / « PROTÉGÉ » en colonne 0). Le modificateur inline de
     * methode (« PROCÉDURE PRIVÉE foo() ») prend l'accord feminin via
     * fmt_visibility_inline_canon() quand PROCEDURE/FONCTION precede sur la
     * ligne.
     *  : PAIRE bilingue, pas neutre. Les graphies EN sont acceptees
     * par le scanner depuis l'origine (scanner.c) mais etaient rabattues sur
     * le canon FR -> une classe traduite gardait un mot-cle FR accentue, et
     * fmt reecrivait PRIVÉ par-dessus PRIVATE. */
    {"prive",       "PRIV\xc3\x89",          CAT_OOP, KW_FR, "PRIVATE"},   /* PRIVÉ */
    {"privee",      "PRIV\xc3\x89",          CAT_OOP, KW_FR, "PRIVATE"},
    {"private",     "PRIVATE",               CAT_OOP, KW_EN, "PRIV\xc3\x89"},
    {"protege",     "PROT\xc3\x89G\xc3\x89", CAT_OOP, KW_FR, "PROTECTED"}, /* PROTÉGÉ */
    {"protegee",    "PROT\xc3\x89G\xc3\x89", CAT_OOP, KW_FR, "PROTECTED"},
    {"protected",   "PROTECTED",             CAT_OOP, KW_EN, "PROT\xc3\x89G\xc3\x89"},
    {"globale",     "GLOBALE",     CAT_OOP, KW_NEUTRE, NULL},
    {"static",      "GLOBALE",     CAT_OOP, KW_NEUTRE, NULL},
    {"liberer",     "Lib\xc3\xa9rer",  CAT_OOP, KW_FR, "Free"},   /* Libérer */
    {"free",        "Free",        CAT_OOP, KW_EN, "Lib\xc3\xa9rer"},
    {"delete",      "Delete",      CAT_OOP, KW_NEUTRE, NULL},
    {"constructeur","Constructeur", CAT_OOP, KW_NEUTRE, NULL},   /* Constructeur EN=FR */
    {"constructor", "Constructeur", CAT_OOP, KW_NEUTRE, NULL},
    {"destructeur", "Destructeur",  CAT_OOP, KW_NEUTRE, NULL},
    {"destructor",  "Destructeur",  CAT_OOP, KW_NEUTRE, NULL},

    /* Logical — UPPERCASE (canon EN = miroir FR : AND/OR/_AND_/_OR_/_IN_) */
    {"et",          "ET",          CAT_LOGICAL, KW_FR, "AND"},
    {"and",         "AND",         CAT_LOGICAL, KW_EN, "ET"},
    {"ou",          "OU",          CAT_LOGICAL, KW_FR, "OR"},
    {"or",          "OR",          CAT_LOGICAL, KW_EN, "OU"},
    {"_et_",        "_ET_",        CAT_LOGICAL, KW_FR, "_AND_"},
    {"_and_",       "_AND_",       CAT_LOGICAL, KW_EN, "_ET_"},
    {"_ou_",        "_OU_",        CAT_LOGICAL, KW_FR, "_OR_"},
    {"_or_",        "_OR_",        CAT_LOGICAL, KW_EN, "_OU_"},
    {"modulo",      "MODULO",      CAT_LOGICAL, KW_NEUTRE, NULL},   /* MODULO EN=FR */
    {"_dans_",      "_DANS_",      CAT_LOGICAL, KW_FR, "_IN_"},
    {"_in_",        "_IN_",        CAT_LOGICAL, KW_EN, "_DANS_"},

    /* Booleans — PascalCase (canon EN = miroir FR : True/False) */
    {"vrai",        "Vrai",        CAT_BOOL, KW_FR, "True"},
    {"true",        "True",        CAT_BOOL, KW_EN, "Vrai"},
    {"faux",        "Faux",        CAT_BOOL, KW_FR, "False"},
    {"false",       "False",       CAT_BOOL, KW_EN, "Faux"},
    {"null",        "Null",        CAT_BOOL, KW_NEUTRE, NULL},   /* Null EN=FR */

    /* Misc character constants — ALWAYS
     * written in uppercase in the canonical form. owl fmt normalizes
     * `rc`/`tab`/etc. to their canonical uppercase form. Neutres (memes
     * formes FR et EN). */
    {"crlf",        "CRLF",        CAT_BOOL, KW_NEUTRE, NULL},
    /* LF est une vraie constante du registre, au meme titre que
     * CRLF : CAT_BOOL, donc warning de masquage generique et garde
     * shadow-variable. Sa canonisation passait deja par le chemin generique
     * des constantes du registre ; l'entree ici sert le warning compilo, qui
     * lit exclusivement fmt_keywords[]. */
    {"lf",          "LF",          CAT_BOOL, KW_NEUTRE, NULL},
    /*  lot 1 — formes heritees de fin de ligne. Elles ne sont plus des
     * symboles du registre (cf. EOL_LEGACY_ALIASES dans stdlib_builtin_types.c)
     * mais valent exactement CRLF : les reecrire ici est neutre en semantique et
     * fait s'evaporer le legacy au fil des sauvegardes. Categorie dediee et non
     * CAT_BOOL : la garde shadow-variable s'applique telle quelle (`cr est un
     * entier` reste `cr`), mais le warning de masquage a son propre libelle —
     * ce que `cR` masque, c'est la forme `CR`, pas `CRLF` que le user n'a pas
     * ecrit (cf. owl_formatter_eol_legacy_canonical). */
    {"rc",          "CRLF",        CAT_EOL_LEGACY, KW_NEUTRE, NULL},
    {"cr",          "CRLF",        CAT_EOL_LEGACY, KW_NEUTRE, NULL},
    {"tab",         "TAB",         CAT_BOOL, KW_NEUTRE, NULL},
    {"eot",         "EOT",         CAT_BOOL, KW_NEUTRE, NULL},
    {"esc",         "ESC",         CAT_BOOL, KW_NEUTRE, NULL},
    {"esp",         "ESP",         CAT_BOOL, KW_NEUTRE, NULL},
    {"valpi",       "ValPI",       CAT_BOOL, KW_NEUTRE, NULL},     /* ValPI keeps mixed case per doc */

    /* Articles — lowercase. `un`/`une`/`des` (FR) <-> `a`/`an` n'ont pas de
     * forme reservee EN dans le langage (l'article EN n'est pas un keyword
     * OWL) -> neutres, pass-through.
     *  : `uns`/`unes`/`du`/`le`/`la`/`les` ont quitte cette table en
     * meme temps que la table de fillers du scanner. Ce sont des identifiants
     * ordinaires, et les rabattre vers la minuscule abimait la graphie d'un
     * nom declare (`Le`, `Du`...). */
    {"un",          "un",          CAT_FILLER, KW_NEUTRE, NULL},
    {"une",         "une",         CAT_FILLER, KW_NEUTRE, NULL},
    {"des",         "des",         CAT_FILLER, KW_NEUTRE, NULL},

    {NULL, NULL, 0, KW_NEUTRE, NULL}
};

/* ================================================================
 * GENRE GRAMMATICAL — RESIDU SANS ENREGISTREMENT SOURCE (, )
 *
 * Donnee PASSIVE consommee UNIQUEMENT par `owl translate` pour choisir
 * l'article francais (un/une) en traduction EN->FR d'une declaration. Le
 * compilateur reste gender-agnostic et le formatter (mode NONE) n'y touche pas.
 *
 *  — cette table portait 49 entrees (91 cles) et doublonnait ses
 * sources. Le genre vit desormais SUR l'enregistrement qui porte deja la paire
 * de noms FR/EN du mot :
 *   - sortes de widget et blocs DSL  -> CONTROL_KINDS[].genre  (ui_decl.c)
 *   - primitifs et mots-cles de type -> fmt_keywords[].genre   (ci-dessus)
 *   - types reifies du registre      -> OwlTypeDef.genre       (owl_fmt_canon)
 *
 * Ne subsistent ici que les mots qui n'ont AUCUN enregistrement source
 * atteignable depuis les deux binaires qui hebergent ce formatter (owl.exe sur
 * le backend registre, owl-lsp sur le backend catalogue) :
 *   - `json` : primitif de la VM. Sa fiche vit dans PRIMITIVE_TYPES (cli_doc.c),
 *     qui n'est pas linke dans owl-lsp ; une entree fmt_keywords[] changerait
 *     la casse emise (json -> JSON), donc le comportement.
 *   - `champ`/`control`/`field` : prefixe de widget reconnu par une liste en
 *     dur (fmt_prev_word_is_widget_prefix), pas par une table. Le type `Champ`
 *     du registre porte bien genre=M, mais le backend catalogue rend 0 pour
 *     tous les types et `field` n'est ni son canon, ni son ASCII, ni son
 *     nameEN.
 *   - `theme`, `collectiondeprocedures` : openers de bloc DSL traites en dur
 *     dans ce fichier, sans entree de registre ni de fmt_keywords[].
 *
 * Cle = forme normalisee (lowercase, accents strippes), FR ET EN : la
 * traduction EN->FR retrouve le genre depuis le mot EN qui suit l'article
 * (`a Theme` -> genre M -> `un Thème`). */
typedef struct { const char* norm; int genre; } FmtGenreNoSource;
static const FmtGenreNoSource fmt_genre_no_source[] = {
    {"json",        GENRE_M},                            /* un document JSON */
    {"champ",       GENRE_M}, {"control",    GENRE_M}, {"field", GENRE_M},   /* un champ */
    {"theme",       GENRE_M},                            /* un thème */
    {"collectiondeprocedures", GENRE_F}, {"procedurecollection", GENRE_F},   /* une collection */
    {NULL, 0}
};

/* Genre du type nomme `word` (longueur `len`, forme brute). ,
 * . Interroge dans l'ordre les enregistrements SOURCES du mot :
 *   1. le residu sans domicile (json, champ, theme, collection) ;
 *   2. fmt_keywords[] (primitifs, Classe/Structure/Combinaison/Énumération) ;
 *   3. CONTROL_KINDS (sortes de widget, blocs de menu et d'Etat) ;
 *   4. le registre des types reifies (Image, Timer, httpRequete...).
 * Aucun mot n'a de genre dans deux de ces sources : chacun appartient a une
 * seule table de definition. Retourne 0 (NONE) si inconnu — l'article ecrit
 * est alors laisse tel quel. */
static const FmtKeyword* find_keyword(const char* word, int len);   /* fwd */
static const OwlControlKindInfo* fmt_control_kind_lookup(const char* word, int len,
                                                         bool* wroteEN);  /* fwd */

static int fmt_genre_of_type(const char* word, int len) {
    if (len <= 0 || len >= 200) return 0;
    char buf[256];
    memcpy(buf, word, (size_t)len);
    buf[len] = '\0';
    char q[256];
    owl_normalize_name(buf, q, sizeof q);   /* lowercase + accent-strip */
    for (int i = 0; fmt_genre_no_source[i].norm != NULL; i++) {
        if (strcmp(q, fmt_genre_no_source[i].norm) == 0)
            return fmt_genre_no_source[i].genre;
    }
    const FmtKeyword* kw = find_keyword(q, (int)strlen(q));
    if (kw != NULL && kw->genre != 0) return kw->genre;
    const OwlControlKindInfo* kind = fmt_control_kind_lookup(q, (int)strlen(q), NULL);
    if (kind != NULL && kind->genre != 0) return kind->genre;
    /* Registre des types reifies (Image/Timer/httpRequete/Icone/...). */
    if (owl_fmt_canon && owl_fmt_canon->type_genre) {
        int nt = owl_fmt_canon->type_count();
        for (int i = 0; i < nt; i++) {
            const char* canon = owl_fmt_canon->type_canonical(i);
            const char* ascii = owl_fmt_canon->type_ascii(i);
            /*  : matcher aussi la forme EN. En EN->FR, le mot qui suit
             * l'article est le nom EN du type (`a styleContainer`) ; sans ce
             * test, le genre reste NONE et l'article `a` ne redevient pas `un`
             * (round-trip de la DSL composee casse). */
            const char* en = owl_fmt_canon->type_en ? owl_fmt_canon->type_en(i) : NULL;
            if ((canon && owl_name_matches(canon, q)) ||
                (ascii && owl_name_matches(ascii, q)) ||
                (en    && owl_name_matches(en, q))) {
                return owl_fmt_canon->type_genre(i);
            }
        }
    }
    return 0;
}

/* ================================================================
 * ETAT LANGUE CIBLE
 *
 * Pose par owl_translate_ctx() le temps d'une passe, remis a NONE en
 * sortie. NONE = comportement formatter standard (preservation de la
 * langue ecrite) -> owl fmt inchange, zero regression. FR/EN = force la
 * langue cible sur la surface langage.
 *
 * Process-level (pas per-call sur la pile) : la passe est entierement
 * synchrone (owl_format_ctx ne reentre pas en concurrence dans owl.exe).
 * Save/restore quand meme pour la robustesse si jamais imbrique. ================================================================ */
static OwlTranslateLang g_fmt_target_lang = OWL_TRANSLATE_NONE;

/* Defini plus bas (apres le provider DSL) ; utilise en amont a la resolution
 * des proprietes DSL en position d'affectation. */
static const char* find_dsl_prop_norm(const char* word, int len);
static const char* find_service_ctx_prop_norm(const char* word, int len);

/* ================================================================
 * ARTICLE TRANSLATE
 *
 * En traduction (`owl translate`), l'article de declaration suit la langue
 * cible. FR->EN : un/une -> a/an. EN->FR : a/an -> un/une selon le GENRE du
 * type qui suit (lu via fmt_genre_of_type). Hors translate (mode NONE), tout
 * passe inchange -> `owl fmt` ne consomme jamais le genre. Le compilateur
 * reste gender-agnostic (il ne lit ni n'enforce rien ici).
 * ================================================================ */

static bool is_word_char(char c);  /* fwd : defini plus bas */
static bool fmt_is_word_char(char c);  /* fwd : defini plus bas */

/* Un canon de membre after-dot doit rester un identifiant OWL atomique :
 * chaque octet est un word-char (lettre, chiffre, '_', ou continuation
 * UTF-8 d'un accent). Certaines formes d'affichage de champs builtin sont
 * des libelles FR lisibles ("Sous-libellé") et non des identifiants ; les
 * emettre en position de membre corromprait le code (le '-' se relit en
 * soustraction, l'espace scinde l'expression). On ne canonise donc un
 * membre que si la forme cible est un identifiant valide. */
static bool fmt_canon_is_atomic_ident(const char* s) {
    if (s == NULL || *s == '\0') return false;
    for (const char* p = s; *p; ++p) {
        if (!fmt_is_word_char(*p)) return false;
    }
    return true;
}

/* Peek du prochain mot apres `p` (whitespace saute). Ecrit le mot dans
 * outBuf et retourne sa longueur, 0 si rien. Ne consomme pas `p`. */
static int fmt_peek_next_word(const char* p, char* outBuf, int outCap) {
    while (*p == ' ' || *p == '\t') p++;
    int n = 0;
    while (is_word_char(*p) && n < outCap - 1) outBuf[n++] = *p++;
    outBuf[n] = '\0';
    return n;
}

/*  — mots du type "tableau composE" (nom + adjectif, deux tokens
 * separes). En traduction, la grammaire cible impose l'ordre : FR nom-adjectif
 * ("tableau associatif"), EN adjectif-nom ("associative array"). La traduction
 * mot-a-mot ne reordonnait pas -> "associatif tableau" / "array associative".
 * Scope : associatif/associative (le cas rapporte, support #202). fixe/dynamique
 * ont le meme ordre mais un couplage article-phonetique (a/an) distinct -> hors. */
static bool fmt_is_array_noun(const char* w, int len) {
    return (len == 7 && strcmp(w, "tableau")  == 0) ||
           (len == 8 && strcmp(w, "tableaux") == 0) ||
           (len == 5 && strcmp(w, "array")    == 0);
}
static bool fmt_is_array_adj(const char* w, int len) {
    return (len == 10 && strcmp(w, "associatif")  == 0) ||
           (len == 11 && strcmp(w, "associative") == 0);
}

/* Forme EN d'un type nomme `word` (longueur `len`), pour determiner l'article
 * phonetique EN (a/an). Cherche la forme EN dans fmt_keywords (partner d'une
 * entree CAT_TYPE/CAT_DECL FR, ou canonical d'une entree EN), puis le registre
 * (nameEN via native/type — ici on se rabat sur la forme FR si pas trouve, ce
 * qui suffit pour la 1ere lettre phonetique des noms identiques FR=EN comme
 * Image/Timer). Ecrit dans outBuf, retourne true si une forme a ete trouvee. */
static bool fmt_type_en_form(const char* word, int len, char* outBuf, int outCap) {
    if (len <= 0 || len >= 200) return false;
    char norm[256];
    memcpy(norm, word, (size_t)len);
    norm[len] = '\0';
    char q[256];
    owl_normalize_name(norm, q, sizeof q);
    const FmtKeyword* kw = find_keyword(q, (int)strlen(q));
    if (kw != NULL) {
        const char* en = NULL;
        if (kw->lang == KW_EN) en = kw->canonical;           /* deja EN */
        else if (kw->lang == KW_FR && kw->partner) en = kw->partner;
        if (en != NULL) {
            snprintf(outBuf, (size_t)outCap, "%s", en);
            return true;
        }
    }
    /* Fallback : forme telle qu'ecrite (couvre Image/Timer/Chrono... FR=EN). */
    snprintf(outBuf, (size_t)outCap, "%.*s", len, word);
    return true;
}

/* Article EN phonetique devant le type FR `word` : `an` si la forme EN
 * traduite commence par une voyelle, sinon `a`. (un entier -> an integer ;
 * une Fenetre -> a Window.) */
static const char* fmt_article_en_before_type(const char* word, int len) {
    char en[256];
    if (!fmt_type_en_form(word, len, en, sizeof en) || en[0] == '\0') return "a";
    char c = (char)tolower((unsigned char)en[0]);
    if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') return "an";
    return "a";
}

/*  : forme a emettre pour une entree fmt_keywords[] selon la
 * langue cible. NONE -> canon (formatter standard). Si la cible differe de la
 * langue de l'entree ET qu'un partner existe -> partner (forme de la langue
 * cible). Cible == langue de l'entree -> canon de la forme ecrite : on ne
 * normalise qu'en changeant de langue. */
static const char* fmt_kw_emit_form(const FmtKeyword* kw) {
    if (g_fmt_target_lang == OWL_TRANSLATE_NONE || kw == NULL) return kw->canonical;
    if (kw->partner != NULL) {
        if (g_fmt_target_lang == OWL_TRANSLATE_EN && kw->lang == KW_FR) return kw->partner;
        if (g_fmt_target_lang == OWL_TRANSLATE_FR && kw->lang == KW_EN) return kw->partner;
    }
    return kw->canonical;
}

/*  — canon : apres `tableau/pile/file/liste de` (ou `... of`), le
 * type d'element PRIMITIF s'ecrit au PLURIEL en FR (`tableau de entiers`,
 * `tableau de chaînes`). Map
 * pivot EN -> pluriel FR, pour les seuls primitifs SCALAIRES (les collections
 * tableau->tableaux sont irregulieres et exclues ; les types user passent en
 * identifiants). Sert au round-trip translate EN->FR ET a la canonicalisation
 * `owl fmt` d'une source FR. */
static const char* fmt_coll_elem_fr_plural(const FmtKeyword* kw) {
    if (!kw || kw->category != CAT_TYPE) return NULL;
    const char* enKey = (kw->lang == KW_EN) ? kw->canonical
                      : (kw->lang == KW_FR) ? kw->partner : NULL;
    if (!enKey) return NULL;
    static const struct { const char* en; const char* frPlural; } M[] = {
        {"integer",   "entiers"},
        {"real",      "r\xc3\xa9""els"},
        {"currency",  "mon\xc3\xa9""taires"},
        {"numeric",   "num\xc3\xa9""riques"},
        {"string",    "cha\xc3\xae""nes"},
        {"character", "caract\xc3\xa8""res"},
        {"boolean",   "bool\xc3\xa9""ens"},
        {"time",      "heures"},
        {"duration",  "dur\xc3\xa9""es"},
        {"byte",      "octets"},
        {NULL, NULL}
    };
    for (int i = 0; M[i].en; i++)
        if (strcmp(M[i].en, enKey) == 0) return M[i].frPlural;
    return NULL;
}

/*  — vrai si le mot source `w` (brut, longueur `len`) est le connecteur
 * de type `de`/`of` (`tableau de <type>`). Seul construct ou `de/of` precede
 * directement un mot-cle de type primitif ; l'iteration `POUR TOUT x DE <var>`
 * est suivie d'une variable, pas d'un keyword de type scalaire. */
static bool fmt_prev_word_is_de_of(const char* w, int len) {
    if (!w || len <= 0 || len >= 60) return false;
    char buf[64], norm[64];
    memcpy(buf, w, (size_t)len);
    buf[len] = '\0';
    owl_normalize_name(buf, norm, sizeof norm);
    return strcmp(norm, "de") == 0 || strcmp(norm, "of") == 0;
}

/*  — vrai si le mot precedent est un NOM de collection (`tableau`,
 * `pile`, `file`, `liste` + formes EN), la seule position ou l'elision `d'`
 * introduit le connecteur de type (`tableau d'entiers`). Distingue le `d'`
 * legitime d'une apostrophe fortuite ailleurs. */
static bool fmt_prev_word_is_coll_noun(const char* w, int len) {
    if (!w || len <= 0 || len >= 60) return false;
    char buf[64], norm[64];
    memcpy(buf, w, (size_t)len);
    buf[len] = '\0';
    owl_normalize_name(buf, norm, sizeof norm);
    return strcmp(norm, "tableau")  == 0 || strcmp(norm, "tableaux") == 0 ||
           strcmp(norm, "pile")     == 0 || strcmp(norm, "file")     == 0 ||
           strcmp(norm, "liste")    == 0 || strcmp(norm, "array")    == 0 ||
           strcmp(norm, "queue")    == 0 || strcmp(norm, "stack")    == 0 ||
           strcmp(norm, "list")     == 0;
}

/*  — vrai si la forme FR de sortie du type d'element de collection `w`
 * (mot brut, longueur `len`) commence par une voyelle. C'est la condition
 * d'elision du connecteur `de` -> `d'` en tete de type (`tableau d'entiers`,
 * `tableau d'Image` ; consonne inchangee : `tableau de réels`). On calcule la
 * forme FR reellement emise — pluriel primitif (`integer` -> `entiers`), forme
 * keyword sinon, identifiant verbatim pour un type user/registre (`Image`) —
 * puis on depouille les accents pour tester la 1re lettre (`réels` -> `r`,
 * consonne ; `entiers` -> `e`, voyelle). */
static bool fmt_coll_elem_fr_starts_vowel(const char* w, int len) {
    if (!w || len <= 0 || len >= 200) return false;
    char raw[256];
    memcpy(raw, w, (size_t)len);
    raw[len] = '\0';
    char norm[256];
    owl_normalize_name(raw, norm, sizeof norm);
    const FmtKeyword* kw = find_keyword(norm, (int)strlen(norm));
    const char* frForm = raw;                 /* identifiant/type user : inchange */
    if (kw != NULL) {
        const char* pl = fmt_coll_elem_fr_plural(kw);
        frForm = (pl != NULL) ? pl : fmt_kw_emit_form(kw);
    }
    char stripped[256];
    int sl = scanner_strip_accents(frForm, (int)strlen(frForm),
                                   stripped, sizeof stripped);
    if (sl <= 0) return false;
    char c = (char)tolower((unsigned char)stripped[0]);
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

/*  — DECLARATION MULTIPLE (`x, y sont des entiers` / `x, y are
 * integers`). Vrai si le mot precedent ouvre la position de type d'une
 * declaration plurielle : la copule elle-meme, ou l'article `des` qui la suit.
 * `sont`/`are` sont le MEME token cote scanner et n'ont aucun autre emploi
 * dans la grammaire — apres eux vient toujours un type. */
static bool fmt_prev_word_is_decl_plural(const char* w, int len) {
    if (!w || len <= 0 || len >= 60) return false;
    char buf[64], norm[64];
    memcpy(buf, w, (size_t)len);
    buf[len] = '\0';
    owl_normalize_name(buf, norm, sizeof norm);
    return strcmp(norm, "sont") == 0 || strcmp(norm, "are") == 0 ||
           strcmp(norm, "des")  == 0;
}

/*  — forme plurielle a emettre en POSITION DE DECLARATION MULTIPLE.
 * Table distincte de fmt_coll_elem_fr_plural : le repli « pluriel primitif »
 * du compilateur ne vaut qu'apres `de`/`of`, si bien que
 * `caractères`, `heures`, `durées` sont acceptes en element de collection mais
 * REFUSES ici (« Type inconnu »). Ne listent donc que les pluriels reellement
 * enregistres comme mots du langage cote scanner.
 *
 * `outFR` = langue de la sortie. NULL = pas de pluriel disponible dans cette
 * langue (`currency`, `numeric`, `array` n'en ont pas cote EN) : le singulier
 * canon est alors emis, et c'est la copule qui porte le pluriel au retour. */
static const char* fmt_decl_plural(const FmtKeyword* kw, bool outFR) {
    if (!kw || kw->category != CAT_TYPE) return NULL;
    const char* enKey = (kw->lang == KW_EN) ? kw->canonical
                      : (kw->lang == KW_FR) ? kw->partner : NULL;
    if (!enKey) return NULL;
    static const struct { const char* en; const char* fr; const char* enPlural; } M[] = {
        {"integer",  "entiers",              "integers"},
        {"real",     "r\xc3\xa9""els",       "reals"},
        {"string",   "cha\xc3\xae""nes",     "strings"},
        {"boolean",  "bool\xc3\xa9""ens",    "booleans"},
        {"currency", "mon\xc3\xa9""taires",  NULL},
        {"numeric",  "num\xc3\xa9""riques",  NULL},
        {"array",    "tableaux",             NULL},
        {NULL, NULL, NULL}
    };
    /* La cle pivot est le SINGULIER anglais. Une entree deja plurielle cote EN
     * (`integers`, `strings`...) porte son propre pluriel en canonical : on
     * retombe sur le singulier en retirant le `s` final. */
    char sing[64];
    size_t n = strlen(enKey);
    if (n > 1 && n < sizeof sing && enKey[n - 1] == 's') {
        memcpy(sing, enKey, n - 1);
        sing[n - 1] = '\0';
    } else {
        sing[0] = '\0';
    }
    for (int i = 0; M[i].en; i++)
        if (strcmp(M[i].en, enKey) == 0 ||
            (sing[0] != '\0' && strcmp(M[i].en, sing) == 0))
            return outFR ? M[i].fr : M[i].enPlural;
    return NULL;
}

/*  — la sortie s'ecrit-elle en francais ? En traduction c'est la
 * cible ; hors traduction (`owl fmt`), c'est la langue du mot ecrit, le
 * formateur ne traduisant jamais. */
static bool fmt_out_lang_is_fr(const FmtKeyword* kw) {
    if (g_fmt_target_lang == OWL_TRANSLATE_FR) return true;
    if (g_fmt_target_lang == OWL_TRANSLATE_EN) return false;
    return kw != NULL && kw->lang != KW_EN;
}

/*  : choisit entre une forme FR et une forme EN explicites selon
 * la langue cible. Utilise par les surcharges positionnelles (de/of
 * d'iteration, procédure/fonction en position de type, visibilite inline).
 * NONE -> rend la forme passee en `written` (preservation). */
static const char* fmt_pick_lang(const char* written, const char* fr,
                                 const char* en) {
    if (g_fmt_target_lang == OWL_TRANSLATE_FR) return fr;
    if (g_fmt_target_lang == OWL_TRANSLATE_EN) return en;
    return written;
}

/* ================================================================
 * NATIVE FUNCTION ACCENT NORMALIZATION
 * Maps accented forms to canonical (unaccented) names.
 * The normalized key is lowercase ASCII (after accent stripping).
 * ================================================================ */

typedef struct {
    const char* normalized;  /* lowercase ascii (accents stripped) */
    const char* canonical;   /* correct accented/PascalCase form */
} DynamicGlobalNorm;

/* Runtime-mutable globals : variables exposees par le runtime via table_set()
 * direct sur vm.globals (pas OwlRegistry), parce qu'elles sont MUTABLES.
 * Le registry ne sait pas representer un global qui change de valeur — il
 * decrit des constantes et des natives. ErreurDetectee bascule a chaque
 * execution d'une instruction, donc reste un global runtime.
 *
 * Ce n'est PAS une table de migration legacy. Liste exhaustive : 1 entree.
 * N'ajoute rien ici sans demander — la regle est : "tout dans le registry,
 * sauf si la valeur change a l'execution". */
static const DynamicGlobalNorm runtime_mutable_globals[] = {
    {"erreurdetectee", "ErreurD\xc3\xa9tect\xc3\xa9""e"}, /* ErreurDétectée — cf stdlib_erreur.c */
    {NULL, NULL}
};

/* Note : la liste des proprietes builtin (Annee, Mois, EnJours, Occurrence,
 * Vide, Type, etc.) est fournie par le provider (backend registry -> compiler.c
 * PROPS_* ; backend catalogue -> OWL_BUILTIN_PROP_CATALOG). Le matching vit ici
 * (find_property_norm), pas dans les backends -> pas de logique dupliquee. */

/* Method name normalization: after "." we keep identifiers as-is but apply
 * canonical PascalCase for well-known methods (Chrono, HTTP, etc.) and the
 * accented canonical forms (Début, Démarre...). This avoids the formatter
 * upper-casing things like `c.Fin()` into `c.FIN()` when 'fin' is ALSO a
 * control keyword. */
typedef struct {
    const char* normalized;
    const char* canonical;
} MethodNorm;

/* Accent-restoration pour methodes a canonique accentue NON enregistrees
 * dans le method catalog. Si une methode au canon accentue est aussi
 * enregistree explicitement via owl_register_method (cas standard, ex
 * Chrono.Début, Tableau.Agrège), elle est restituee par find_method_norm
 * step 1 directement et n'a RIEN a faire dans cette table — la regle 20
 * du self-test (method_norm_redundant) hurle si on duplique.
 *
 * Cette table reste pour les cas exotiques ou il faut un override de
 * normalisation que le catalog ne peut pas exprimer. Actuellement vide :
 * toutes les methodes a canon accentue sont enregistrees proprement. */
static const MethodNorm method_norms[] = {
    {NULL, NULL}
};

/* Iteration CAT_BOOL : table des constantes "magiques" du langage que
 * tout dev canon reconnait (chars specials + ValPI + litteraux Vrai/Faux/
 * Null). Source unique = fmt_keywords[]. Le compilo consomme ca pour
 * son warning de shadow.
 *
 * CAT_EOL_LEGACY a son propre accesseur (owl_formatter_eol_legacy_canonical) :
 * le masquage y est reel mais le message doit dire autre chose.
 *
 * Retourne le canonical (TAB / Vrai / ValPI / …) si match, NULL sinon. */
static const char* fmt_canonical_in_category(const char* name, int category) {
    if (name == NULL || *name == '\0') return NULL;
    /* Strip accents + lowercase pour matcher la cle normalisee. */
    char ascii[64];
    int aLen = scanner_strip_accents(name, (int)strlen(name),
                                      ascii, sizeof(ascii));
    if (aLen <= 0 || aLen >= (int)sizeof(ascii)) return NULL;
    for (int i = 0; i < aLen; i++) ascii[i] = (char)tolower((unsigned char)ascii[i]);
    ascii[aLen] = '\0';
    for (int i = 0; fmt_keywords[i].normalized != NULL; i++) {
        if (fmt_keywords[i].category != category) continue;
        if (strcmp(fmt_keywords[i].normalized, ascii) == 0) {
            return fmt_keywords[i].canonical;
        }
    }
    return NULL;
}

const char* owl_formatter_magic_const_canonical(const char* name) {
    return fmt_canonical_in_category(name, CAT_BOOL);
}

const char* owl_formatter_eol_legacy_canonical(const char* name) {
    return fmt_canonical_in_category(name, CAT_EOL_LEGACY);
}

/* Introspection pour registry_self_test : verifie qu'aucune entree
 * n'est redondante avec le method catalog. Voir formatter.h. */
int owl_formatter_method_norms_count(void) {
    int n = 0;
    for (int i = 0; method_norms[i].normalized != NULL; i++) n++;
    return n;
}
const OwlFormatterMethodNorm* owl_formatter_method_norms_at(int i) {
    if (i < 0 || i >= owl_formatter_method_norms_count()) return NULL;
    return (const OwlFormatterMethodNorm*)&method_norms[i];
}

int owl_formatter_keyword_count(void) {
    int n = 0;
    for (int i = 0; fmt_keywords[i].normalized != NULL; i++) n++;
    return n;
}
bool owl_formatter_keyword_at(int i, const char** normalized,
                              const char** canonical, int* lang) {
    if (i < 0 || i >= owl_formatter_keyword_count()) return false;
    if (normalized) *normalized = fmt_keywords[i].normalized;
    if (canonical)  *canonical  = fmt_keywords[i].canonical;
    if (lang)       *lang       = fmt_keywords[i].lang;
    return true;
}

/*  : `PAS` est le SEUL mot-cle dont le canon anglais depend du SENS —
 * `STEP` en en-tete de boucle, `NOT` en negation. La colonne `partner` de
 * fmt_keywords[] n'en porte qu'un (STEP), et le second canon vivait jusqu'ici
 * en dur dans le corps de owl_formatter_keyword_partner (`if (pas) return
 * "NOT"`, ). On le sort en DONNEE ici, sans alourdir la forme de
 * fmt_keywords[] pour les autres entrees (une seule ligne divergente).
 *   default_en = canon rendu a un appelant SANS contexte de boucle : NOT,
 *                « le cas commun ». C'est la seule reponse
 *                sense-independante qu'un appelant sans contexte peut recevoir
 *                sans etre trompe (mesure : `PAS DANS` -> `NOT IN`).
 *   alt_en     = second canon (STEP), expose separement pour l'affichage
 *                standalone de `owl list keywords`. JAMAIS compose (pas de
 *                « STEP IN »).
 * `owl translate` n'emprunte PAS cet accesseur : il lit fmt_keywords[].partner
 * avec son contexte de boucle (fmt_kw_emit_form) et tranche correctement les
 * deux sens. */
typedef struct {
    const char* fr_ascii;    /* forme FR strip-accent lowercase, ex "pas" */
    const char* fr_canon;    /* forme FR canon rendue en reverse, ex "PAS" */
    const char* default_en;  /* canon EN context-free */
    const char* alt_en;      /* second canon EN (autre sens) */
} FmtDivergentEnCanon;
static const FmtDivergentEnCanon fmt_divergent_en[] = {
    { "pas", "PAS", "NOT", "STEP" },
    { NULL, NULL, NULL, NULL },
};

/* Strip-accent + lowercase de `word` dans `out` (>= 64). Rend la longueur,
 * ou -1 si vide / trop long. Factorise les deux accesseurs partner. */
static int fmt_word_to_ascii_lower(const char* word, char* out, size_t cap) {
    if (word == NULL || *word == '\0') return -1;
    int aLen = scanner_strip_accents(word, (int)strlen(word), out, (int)cap);
    if (aLen <= 0 || aLen >= (int)cap) return -1;
    for (int i = 0; i < aLen; i++) out[i] = (char)tolower((unsigned char)out[i]);
    out[aLen] = '\0';
    return aLen;
}

const char* owl_formatter_keyword_partner(const char* word) {
    char ascii[64];
    if (fmt_word_to_ascii_lower(word, ascii, sizeof(ascii)) < 0) return NULL;
    /*  : canon EN dependant du sens (PAS) -> donnee, plus de literal.
     * Forward (fr canon -> default_en) ET reverse (l'un des canons EN -> fr). */
    for (int i = 0; fmt_divergent_en[i].fr_ascii != NULL; i++) {
        const FmtDivergentEnCanon* d = &fmt_divergent_en[i];
        if (strcmp(ascii, d->fr_ascii) == 0) return d->default_en;
        if (strcasecmp(ascii, d->default_en) == 0 ||
            strcasecmp(ascii, d->alt_en) == 0) return d->fr_canon;
    }
    for (int i = 0; fmt_keywords[i].normalized != NULL; i++) {
        if (strcmp(fmt_keywords[i].normalized, ascii) == 0)
            return fmt_keywords[i].partner;
    }
    return NULL;
}

const char* owl_formatter_keyword_partner_alt(const char* word) {
    char ascii[64];
    if (fmt_word_to_ascii_lower(word, ascii, sizeof(ascii)) < 0) return NULL;
    for (int i = 0; fmt_divergent_en[i].fr_ascii != NULL; i++) {
        if (strcmp(ascii, fmt_divergent_en[i].fr_ascii) == 0)
            return fmt_divergent_en[i].alt_en;
    }
    return NULL;
}

/* Detection rapide : un nom catalog contient-il un octet UTF-8 multibyte
 * (>= 0x80), donc un accent ? */
static bool name_has_accent(const char* s) {
    for (const char* p = s; *p; ++p) {
        if ((unsigned char)*p >= 0x80) return true;
    }
    return false;
}

static const char* find_method_norm(const char* word, int len) {
    if (len <= 0 || len >= 128) return NULL;

    /* Strategie : scanner le catalog des methodes enregistrees et choisir
     * la meilleure entree qui matche l'input apres strip-accent. La regle :
     *   - si une entree au CANON ACCENTUE matche, c'est elle qu'on rend
     *     (le formatter restaure l'accent canon meme quand l'user a tape
     *     l'alias ASCII fallback)
     *   - sinon, si une entree ASCII pure matche, on la rend telle quelle
     *
     * Pourquoi cette priorite : pour un meme owl_register_method enregistre
     * 3 fois (canon FR accent + alias ASCII + alias EN), l'user qui tape
     * l'ASCII fallback doit etre remis sur le canon FR accent au save.
     * Mais l'user qui tape l'alias EN (`Aggregate`, `Reset`, `Start`) garde
     * EN : aucun alias accente ne strip vers le meme ASCII, donc rien ne
     * gagne la priorite — on retombe sur le match strict. */

    char lbuf[128];
    for (int i = 0; i < len; i++) lbuf[i] = (char)tolower((unsigned char)word[i]);

    /* Input strip-accent + lowercase pour le compare accent-insensible. */
    char input_strip[128];
    int sLen = scanner_strip_accents(word, len, input_strip, sizeof(input_strip));
    if (sLen > 0) {
        for (int i = 0; i < sLen; i++)
            input_strip[i] = (char)tolower((unsigned char)input_strip[i]);
    }

    const char* match_strict = NULL;   /* fallback: meme strlen + strcase */
    const char* match_accent = NULL;   /* prefere: canon accentue qui strip vers input */

    int n = owl_fmt_canon ? owl_fmt_canon->method_count() : 0;
    for (int i = 0; i < n; i++) {
        const char* mname = owl_fmt_canon->method_name(i);
        if (!mname) continue;
        int mlen = (int)strlen(mname);

        /* Match 1 : strlen + strcase strict (input et entree memes bytes). */
        if (mlen == len) {
            bool ok = true;
            for (int j = 0; j < len; j++) {
                if (lbuf[j] != tolower((unsigned char)mname[j])) { ok = false; break; }
            }
            if (ok && match_strict == NULL) match_strict = mname;
        }

        /* Match 2 : entree au canon accentue qui strip vers le meme ASCII
         * que l'input. Priorise par rapport au match strict. */
        if (sLen > 0 && name_has_accent(mname)) {
            char ent_strip[128];
            int eLen = scanner_strip_accents(mname, mlen, ent_strip, sizeof(ent_strip));
            if (eLen == sLen) {
                bool ok = true;
                for (int j = 0; j < sLen; j++) {
                    if (input_strip[j] != tolower((unsigned char)ent_strip[j])) {
                        ok = false; break;
                    }
                }
                if (ok && match_accent == NULL) match_accent = mname;
            }
        }
    }
    if (match_accent) return match_accent;
    if (match_strict) return match_strict;

    /* 2. Fallback : static method_norms table (vide aujourd'hui, mais
     * laissee pour cas exotiques futurs — cf commentaire en tete). */
    for (int i = 0; method_norms[i].normalized != NULL; i++) {
        int nlen = (int)strlen(method_norms[i].normalized);
        if (nlen != len) continue;
        bool match = true;
        for (int j = 0; j < len; j++) {
            if (tolower((unsigned char)word[j]) !=
                (unsigned char)method_norms[i].normalized[j]) {
                match = false;
                break;
            }
        }
        if (match) return method_norms[i].canonical;
    }
    return NULL;
}

/*  — bascule de langue d'une methode fluent (apres `.`) en mode
 * translate. Une methode fluent qui EST une native bilingue (Ajoute<->Add,
 * Taille<->Length, Supprime<->Delete...) doit suivre la langue cible, comme
 * le fait deja le chemin des appels natifs nus (find_native_norm). On resout
 * `name` contre le SEUL catalogue natives (via owl_fmt_canon, partage
 * registry/catalogue -> parite dual-backend) et on renvoie la forme cible.
 * NULL si la methode n'a pas de partenaire natif (methode-only sans native
 * bilingue, ou nom absent du catalogue) : le caller garde alors la forme
 * canon rendue par find_method_norm -> pas de sur-traduction d'une methode
 * user. Hors translate (target NONE), renvoie NULL (aucune bascule). */
static const char* fmt_method_partner(const char* name, int len) {
    if (g_fmt_target_lang == OWL_TRANSLATE_NONE) return NULL;
    if (name == NULL || len <= 0 || len >= 200 || owl_fmt_canon == NULL)
        return NULL;
    char buf[256];
    memcpy(buf, name, (size_t)len);
    buf[len] = '\0';
    char q[256];
    owl_normalize_name(buf, q, sizeof q);
    int nn = owl_fmt_canon->native_count();
    for (int i = 0; i < nn; i++) {
        const char* canon = owl_fmt_canon->native_canonical(i);
        const char* ascii = owl_fmt_canon->native_ascii(i);
        const char* en    = owl_fmt_canon->native_en(i);
        bool hit = (canon && owl_name_matches(canon, q)) ||
                   (ascii && owl_name_matches(ascii, q)) ||
                   (en    && owl_name_matches(en, q));
        if (hit) {
            if (g_fmt_target_lang == OWL_TRANSLATE_EN)
                return (en != NULL) ? en : canon;
            return (canon != NULL) ? canon : en;
        }
    }
    /*  — methode fluente SANS native bilingue nue homonyme (les methodes
     * specifiques d'un type : Tenseur.Rétropropage/Backpropagate,
     * .EntropieCroisée/.CrossEntropy...). Le pairing bilingue est porte par le
     * catalogue des methodes lui-meme (deux graphies, meme impl). On bascule
     * vers la graphie soeur : pour une entree monolingue (translate sur un
     * fichier d'une seule langue), la soeur EST la forme de la langue cible. */
    if (owl_fmt_canon->method_partner != NULL) {
        const char* sib = owl_fmt_canon->method_partner(name, len);
        if (sib != NULL) return sib;
    }
    return NULL;
}

/*  — resout un nom d'event canon (FR ou EN) contre TOUS les
 * catalogues d'events (widgets par kind + lifecycle Fenetre/Etat/sous-controle
 * Etat + OptionMenu + collection). Miroir cote formatter de
 * lsp_find_event_def_any. Le match est accent/casse-insensible (name_eq_loose
 * dans owl_event_catalog.c), donc reconnait aussi bien surClic que onClick.
 * Les catalogues sont peuples des DEUX backends : owl.exe via
 * stdlib_ui_catalog_init, owl-lsp via event_catalog_widgets_init — mais la
 * bascule n'est armee qu'en mode translate (owl.exe uniquement). */
static const OwlEventDef* fmt_find_event_any(const char* name) {
    if (name == NULL || name[0] == '\0') return NULL;
    for (int kind = OWL_CONTROL_LABEL; kind <= OWL_CONTROL_WINDOW; kind++) {
        const OwlEventDef* d = owl_event_catalog_find((OwlControlKind)kind, name);
        if (d) return d;
    }
    const OwlEventDef* d;
    if ((d = owl_etat_event_catalog_find(name)))            return d;
    if ((d = owl_etat_subcontrol_event_catalog_find(name))) return d;
    if ((d = owl_window_lifecycle_catalog_find(name)))      return d;
    if ((d = owl_menu_option_event_catalog_find(name)))     return d;
    if ((d = owl_collection_event_catalog_find(name)))      return d;
    return NULL;
}

/*  — bascule de langue d'un nom d'event en mode translate, en position
 * de nom de PROCEDURE (`PROCEDURE surClic()`). Symetrique de
 * fmt_method_partner mais sur le catalogue d'events. Renvoie la
 * forme de la langue cible si `name` EST un event canon, NULL sinon (procedure
 * user -> pas de sur-traduction). Hors translate (target NONE), NULL : la
 * parite dual-backend est preservee (le formatter non-translate ne touche
 * jamais au catalogue events). */
static const char* fmt_event_partner(const char* name, int len) {
    if (g_fmt_target_lang == OWL_TRANSLATE_NONE) return NULL;
    if (name == NULL || len <= 0 || len >= 200) return NULL;
    char buf[256];
    memcpy(buf, name, (size_t)len);
    buf[len] = '\0';
    const OwlEventDef* d = fmt_find_event_any(buf);
    if (d == NULL) return NULL;
    if (g_fmt_target_lang == OWL_TRANSLATE_EN)
        return (d->nameEN != NULL) ? d->nameEN : d->canonical;
    return d->canonical;
}

static const char* find_property_norm(const char* word, int len,
                                      const char* raw, int rawLen) {
    /* Proprietes des types primitifs builtin (date, heure, dateheure, duree,
     * tableau, file/pile/liste, JSON) — source canon : compiler.c::PROPS_*.
     * Matching cote formatter sur les donnees du provider (fr et en strippes
     * via owl_name_matches). Retour : forme de la langue cible en mode
     * translate ; en mode gatekeeper (NONE) la langue ECRITE est preservee,
     * seuls casse/accents sont canonises (cf.  ci-dessous). */
    if (owl_fmt_canon && len > 0 && len < 200) {
        char wbuf[256];
        memcpy(wbuf, word, (size_t)len);
        wbuf[len] = '\0';
        char q[256];
        owl_normalize_name(wbuf, q, sizeof q);
        int nb = owl_fmt_canon->builtin_prop_count();
        for (int i = 0; i < nb; i++) {
            const char* fr = owl_fmt_canon->builtin_prop_fr(i);
            const char* en = owl_fmt_canon->builtin_prop_en(i);
            bool hitFR = fr && owl_name_matches(fr, q);
            bool hitEN = en && owl_name_matches(en, q);
            if (hitFR || hitEN) {
                /*  : translate -> forme de la langue cible (fallback
                 * sur l'autre forme si NULL). */
                if (g_fmt_target_lang == OWL_TRANSLATE_EN)
                    return (en != NULL) ? en : fr;
                if (g_fmt_target_lang == OWL_TRANSLATE_FR)
                    return (fr != NULL) ? fr : en;
                /*  — gatekeeper (NONE) : préserver la langue ÉCRITE.
                 * Une prop matchée par sa colonne EN (..Empty, ..Count) doit
                 * ressortir en EN, pas se faire écraser vers le FR. Le canon
                 * bilingue impose que chaque forme reste dans sa langue ; le
                 * formateur ne fait que canoniser casse/accents. Symétrique du
                 * garde  sur les champs de types (type_field). */
                if (hitFR) return fr;
                return en;
            }
        }
    }

    /* Registry fallback: scan fields of every registered builtin type
     * (httpRequête, Chrono, Definition…). `len`/`word` comes in accent-
     * stripped ASCII. We compare to both `name` (canonical ASCII) and
     * `nameAccented`. Returns the display form (accented if present). */
    if (len > 0 && len < 200) {
        char buf[256];
        memcpy(buf, word, (size_t)len);
        buf[len] = '\0';
        int nt = owl_fmt_canon ? owl_fmt_canon->type_count() : 0;
        /*  — passe 1 : si le mot ECRIT (accents/casse inclus) est deja
         * EXACTEMENT le display d'un champ, on le laisse tel quel. Deux champs
         * de types distincts peuvent partager la forme accent-strippee (ex la
         * surface menu `Barre` de styleMenuPrincipal vs l'attribut de police
         * `Barré`) ; sans cette passe, le scan accent-insensible ci-dessous
         * canoniserait `Barre` en `Barré` (premier match). Preferer une forme
         * deja canonique evite ce renommage errone. */
        if (raw != NULL && rawLen > 0 && rawLen < 200) {
            for (int ti = 0; ti < nt; ti++) {
                int nf = owl_fmt_canon->type_field_count(ti);
                for (int fi = 0; fi < nf; fi++) {
                    const char* key = owl_fmt_canon->type_field_key(ti, fi);
                    if (!key || strcasecmp(buf, key) != 0) continue;
                    /*  — homographe-modulo-accent (FR/EN ne different
                     * que par un accent, ex ..Temperature/..Température). En
                     * gatekeeper, si la forme ECRITE est exactement le nameEN
                     * canon, la preserver EN plutot que la laisser filer vers
                     * le scan accent-insensible qui la reecrirait en display
                     * FR. Le display FR (verifie juste apres) ne matche pas la
                     * graphie EN (accents differents), d'ou ce garde en amont.
                     * Reserve au gatekeeper : en translate la langue cible
                     * prime (cf.  plus bas). */
                    if (g_fmt_target_lang == OWL_TRANSLATE_NONE
                        && owl_fmt_canon->type_field_en) {
                        const char* en = owl_fmt_canon->type_field_en(ti, fi);
                        if (en && (int)strlen(en) == rawLen
                            && memcmp(en, raw, (size_t)rawLen) == 0
                            && fmt_canon_is_atomic_ident(en)) {
                            return en;
                        }
                    }
                    const char* disp = owl_fmt_canon->type_field_display(ti, fi);
                    if (disp && (int)strlen(disp) == rawLen
                        && memcmp(disp, raw, (size_t)rawLen) == 0
                        && fmt_canon_is_atomic_ident(disp)) {
                        /*  — en translate EN, un champ builtin dont la
                         * forme ecrite est deja le display FR doit basculer vers
                         * son nameEN (`..Valeur` -> `..Value`) : sans ce garde la
                         * cascade `champ..Prop` sortait en FR (asymetrie avec le
                         * sens EN->FR deja correct). */
                        if (g_fmt_target_lang == OWL_TRANSLATE_EN
                            && owl_fmt_canon->type_field_en) {
                            const char* en = owl_fmt_canon->type_field_en(ti, fi);
                            if (en && fmt_canon_is_atomic_ident(en)) return en;
                        }
                        return disp;
                    }
                }
            }
        }
        for (int ti = 0; ti < nt; ti++) {
            int nf = owl_fmt_canon->type_field_count(ti);
            for (int fi = 0; fi < nf; fi++) {
                const char* key = owl_fmt_canon->type_field_key(ti, fi);
                const char* en  = owl_fmt_canon->type_field_en
                                      ? owl_fmt_canon->type_field_en(ti, fi)
                                      : NULL;
                bool matchFR = key && strcasecmp(buf, key) == 0;
                /*  — matching sur la forme EN reserve a translate. En
                 * mode gatekeeper (fmt), une prop ecrite en EN reste EN : le
                 * formatter canonise casse/accents mais ne TRADUIT pas EN->FR.
                 * Sans ce garde, un `..Interval` (forme EN d'un champ Timer) se
                 * ferait reecrire `..Intervalle` a chaque save. En translate,
                 * au contraire, l'entree EN doit ramener la forme FR (symetrie
                 * du sens FR->EN deja couvert par matchFR). */
                bool matchEN = (g_fmt_target_lang != OWL_TRANSLATE_NONE)
                               && en && strcasecmp(buf, en) == 0;
                if (matchFR || matchEN) {
                    const char* disp = owl_fmt_canon->type_field_display(ti, fi);
                    /* Forme d'affichage non-identifiant (ex "Sous-libellé") :
                     * pas canonisable en membre, sinon corruption. On ne la
                     * rend pas — verbatim (le membre user reste intact). */
                    if (fmt_canon_is_atomic_ident(disp)) {
                        /*  — forme de la langue cible en translate EN. */
                        if (g_fmt_target_lang == OWL_TRANSLATE_EN
                            && en && fmt_canon_is_atomic_ident(en)) return en;
                        return disp;
                    }
                }
            }
        }
    }
    return NULL;
}

static const char* find_native_norm(const char* word, int len, char origFirstChar) {
    /* Types vs functions disambiguation: when the user typed a lowercase-
     * initial word (e.g. "httprequete"), they most likely mean the type
     * (`httpRequête`), not the same-name function (`HTTPRequête`). Try
     * the type registry first in that case.
     *
     * For uppercase-initial input ("HTTPRequete") we keep the original
     * order: native registry first, types later.
     *
     * `word` is the accent-stripped lowercase form used for matching.
     * `origFirstChar` is the unmodified first byte the user actually typed,
     * passed in so we can tell `HTTPRequete` (function) from `httprequete`
     * (type). Without it, every input would look lowercase-initial. */
    bool lowerInitial = (origFirstChar >= 'a' && origFirstChar <= 'z');

    if (lowerInitial && len > 0 && len < 200) {
        char buf[256];
        memcpy(buf, word, (size_t)len);
        buf[len] = '\0';
        int nt = owl_fmt_canon ? owl_fmt_canon->type_count() : 0;
        for (int i = 0; i < nt; i++) {
            const char* ascii = owl_fmt_canon->type_ascii(i);
            if (ascii && strcasecmp(buf, ascii) == 0)
                return owl_fmt_canon->type_canonical(i);
        }
    }

    /* Registry : owl_registry_native_canonical_for fait case + accent-
     * insensitive, ET preserve la langue dans laquelle l'user a tape :
     *   - input FR canon/ASCII -> renvoie canon FR accent (canonifie)
     *   - input EN (nameEN) -> renvoie EN tel quel (pas de cross-language)
     *   - input alias arbitraire -> renvoie l'alias matche
     * Quasi-totalite des natives passent par ici. Le fallback en dessous
     * couvre UNIQUEMENT le cas registry-vide (owl --help) ou les rares
     * globaux mutables runtime (runtime_mutable_globals[]). */
    if (len > 0 && len < 200) {
        char buf[256];
        memcpy(buf, word, (size_t)len);
        buf[len] = '\0';
        /* Matching native cote formatter (source unique, partagee registry/
         * catalogue). Pour chaque native : canonical -> ascii -> EN -> aliases,
         * exactement comme owl_registry_native_canonical_for. Preserve la langue
         * tapee (FR canon/ascii -> canon FR ; EN -> EN ; alias -> alias). */
        char q[256];
        owl_normalize_name(buf, q, sizeof q);
        int nn = owl_fmt_canon ? owl_fmt_canon->native_count() : 0;
        for (int i = 0; i < nn; i++) {
            const char* canon = owl_fmt_canon->native_canonical(i);
            const char* ascii = owl_fmt_canon->native_ascii(i);
            const char* en = owl_fmt_canon->native_en(i);
            bool hit = (canon && owl_name_matches(canon, q)) ||
                       (ascii && owl_name_matches(ascii, q)) ||
                       (en && owl_name_matches(en, q));
            if (hit) {
                /*  : en mode translate, retourne la forme de la
                 * langue cible (fallback sur l'autre forme si NULL). Hors
                 * translate, comportement formatter : FR canon pour input
                 * FR/ascii, EN pour input EN (preservation). */
                if (g_fmt_target_lang == OWL_TRANSLATE_EN)
                    return (en != NULL) ? en : canon;
                if (g_fmt_target_lang == OWL_TRANSLATE_FR)
                    return (canon != NULL) ? canon : en;
                if (canon && owl_name_matches(canon, q)) return canon;
                if (ascii && owl_name_matches(ascii, q)) return canon;
                return en;
            }
            int na = owl_fmt_canon->native_alias_count(i);
            for (int a = 0; a < na; a++) {
                const char* al = owl_fmt_canon->native_alias(i, a);
                if (al && owl_name_matches(al, q)) {
                    /* Alias matche : en translate, normalise vers la forme
                     * cible (canon FR / EN) plutot que de garder l'alias. */
                    if (g_fmt_target_lang == OWL_TRANSLATE_EN)
                        return (en != NULL) ? en : canon;
                    if (g_fmt_target_lang == OWL_TRANSLATE_FR)
                        return (canon != NULL) ? canon : en;
                    return al;
                }
            }
        }
    }

    /* Runtime-mutable globals — voir runtime_mutable_globals[] supra.
     * Pas du fallback "pas encore migre" : ce sont des globaux mutables que
     * le registry ne sait pas representer. */
    for (int i = 0; runtime_mutable_globals[i].normalized != NULL; i++) {
        int nlen = (int)strlen(runtime_mutable_globals[i].normalized);
        if (nlen != len) continue;
        bool match = true;
        for (int j = 0; j < len; j++) {
            if (tolower((unsigned char)word[j]) !=
                (unsigned char)runtime_mutable_globals[i].normalized[j]) {
                match = false;
                break;
            }
        }
        if (match) return runtime_mutable_globals[i].canonical;
    }

    /* Registry fallback for constants (httpGet, httpPost, ValPI, RC…) and
     * builtin types (httpRequête, Chrono, Definition…). Accent + case-
     * insensitive — returns the canonical display form. */
    if (len > 0 && len < 200) {
        char buf[256];
        memcpy(buf, word, (size_t)len);
        buf[len] = '\0';

        /* Lookup case+accent insensible (normalize_name ASCII-fold + lower).
         * Sans ca, 'iPDFGenerique' (ASCII tape par l'utilisateur) ne matche
         * pas 'iPDFGénérique' (canonique) en strcasecmp brut. */
        char q[256];
        owl_normalize_name(buf, q, sizeof q);
        int nc = owl_fmt_canon ? owl_fmt_canon->const_count() : 0;
        /*  — deux passes, FR d'abord. Une seule passe qui testerait
         * FR puis EN entree par entree laisserait le nom EN d'une constante
         * masquer le nom FR d'une constante declaree plus loin. Le catalogue
         * n'a aujourd'hui aucune collision de ce genre, mais l'ordre de la
         * table ne doit pas devenir un invariant tacite. */
        for (int i = 0; i < nc; i++) {
            const char* cn = owl_fmt_canon->const_name(i);
            if (cn == NULL || !owl_name_matches(cn, q)) continue;
            /* Le nom EN vit dans le registre (name_en) ; la traduction le
             * consomme ici pour que la constante suive la langue cible, au
             * lieu de rester en FR au milieu du code EN. */
            const char* ce = owl_fmt_canon->const_en
                             ? owl_fmt_canon->const_en(i) : NULL;
            if (g_fmt_target_lang == OWL_TRANSLATE_EN && ce != NULL) return ce;
            return cn;
        }
        for (int i = 0; i < nc; i++) {
            const char* ce = owl_fmt_canon->const_en
                             ? owl_fmt_canon->const_en(i) : NULL;
            if (ce == NULL || !owl_name_matches(ce, q)) continue;
            const char* cn = owl_fmt_canon->const_name(i);
            /* Hors translate, la langue ecrite est preservee (meme contrat
             * que les natives) : une constante ecrite en EN reste en EN. */
            if (g_fmt_target_lang == OWL_TRANSLATE_FR && cn != NULL) return cn;
            return ce;
        }

        int nt = owl_fmt_canon ? owl_fmt_canon->type_count() : 0;
        for (int i = 0; i < nt; i++) {
            const char* ascii = owl_fmt_canon->type_ascii(i);
            const char* canon = owl_fmt_canon->type_canonical(i);
            if (ascii && strcasecmp(buf, ascii) == 0) return canon;
            if (canon && strcasecmp(buf, canon) == 0) return canon;
        }
    }
    return NULL;
}

/* ================================================================
 * TRANSLATE TYPE-AWARE — DSL des types composes
 *
 * `owl translate` doit connaitre le TYPE COMPOSE courant pour resoudre le bon
 * name_en d'un champ : la meme cle FR `Libellé` vaut `Label` sous styleLibellé
 * mais `Caption` sous styleBouton. Ces helpers, actifs uniquement en mode
 * translate, resolvent un nom de type / de champ / de valeur d'enum contre le
 * registre (via owl_fmt_canon), et exposent le type du sous-bloc pour la
 * descente recursive (pile de contexte dans OwlFormatState). Hors translate
 * (owl fmt, g_fmt_target_lang == NONE) ils ne sont jamais appeles.
 * ================================================================ */

/* Index registre du type compose dont un nom (FR canon/ascii/EN) matche `word`
 * (accent/casse-insensible). -1 si aucun. */
static int fmt_composite_type_index(const char* word, int len) {
    if (!owl_fmt_canon || len <= 0 || len >= 200) return -1;
    char buf[256];
    memcpy(buf, word, (size_t)len);
    buf[len] = '\0';
    char q[256];
    owl_normalize_name(buf, q, sizeof q);
    int nt = owl_fmt_canon->type_count();
    for (int i = 0; i < nt; i++) {
        const char* canon = owl_fmt_canon->type_canonical(i);
        const char* ascii = owl_fmt_canon->type_ascii(i);
        const char* en    = owl_fmt_canon->type_en ? owl_fmt_canon->type_en(i) : NULL;
        if ((canon && owl_name_matches(canon, q)) ||
            (ascii && owl_name_matches(ascii, q)) ||
            (en    && owl_name_matches(en, q)))
            return i;
    }
    return -1;
}

/*  — deux formes canon FR/EN ne different-elles QUE par des accents ?
 * (accent-strip egaux, casse-insensible, mais octets bruts distincts). C'est le
 * cas Détection/Detection, ou owl_name_matches (accent-insensible) est aveugle
 * a la langue ecrite et matcherait toujours l'EN. */
static bool fmt_accent_only_diff(const char* fr, const char* en) {
    char sf[256], se[256];
    int nf = scanner_strip_accents(fr, (int)strlen(fr), sf, sizeof sf);
    int ne = scanner_strip_accents(en, (int)strlen(en), se, sizeof se);
    if (nf <= 0 || ne <= 0 || nf != ne) return false;
    for (int i = 0; i < nf; i++)
        if (tolower((unsigned char)sf[i]) != tolower((unsigned char)se[i]))
            return false;
    return true;
}

/*  — la forme ecrite `word`(len) est-elle la graphie `canon`, a la casse
 * ASCII pres mais accent-SENSIBLE ? Sert a trancher entre deux graphies qui ne
 * different que par l'accent : `Detection` (9 octets) ne peut pas matcher
 * `Détection` (10 octets), la longueur suffit deja a les separer ; le fold ASCII
 * couvre les casses (`detection`, `DETECTION`). Les octets non-ASCII (l'accent)
 * sont compares tels quels : c'est precisement ce que owl_name_matches efface. */
static bool fmt_word_matches_accent_sensitive(const char* word, int len,
                                              const char* canon) {
    if ((int)strlen(canon) != len) return false;
    for (int i = 0; i < len; i++) {
        unsigned char a = (unsigned char)word[i];
        unsigned char b = (unsigned char)canon[i];
        if (a < 0x80) a = (unsigned char)tolower(a);
        if (b < 0x80) b = (unsigned char)tolower(b);
        if (a != b) return false;
    }
    return true;
}

/*  — forme canon du nom d'un type compose en position de type : langue
 * CIBLE en traduction, sinon langue de la forme ECRITE (`owl fmt` preserve, meme
 * contrat que fmt_control_kind_lang_form pour les sortes de controle).
 *
 * Hors traduction ce chemin est necessaire, pas cosmetique : sans lui `owl fmt`
 * sur un fichier EN faisait passer le nom du TYPE par le catalogue des natives,
 * qui contient l'homonyme (`httpRequest` -> `HTTPRequest`, `language` ->
 * `Language`). Le fichier EN ressortait avec un nom de fonction en position de
 * type, donc non compilable. NULL si `word` n'est pas un type du registre. */
static const char* fmt_composite_type_lang_form(const char* word, int len) {
    int ti = fmt_composite_type_index(word, len);
    if (ti < 0) return NULL;
    const char* fr = owl_fmt_canon->type_canonical(ti);
    const char* en = owl_fmt_canon->type_en ? owl_fmt_canon->type_en(ti) : NULL;
    if (g_fmt_target_lang == OWL_TRANSLATE_EN) return (en != NULL) ? en : fr;
    if (g_fmt_target_lang == OWL_TRANSLATE_FR) return (fr != NULL) ? fr : en;
    /* Mode NONE : la langue ecrite gagne. Le nom EN n'est retenu que s'il est
     * distinct du FR et que c'est bien lui qui a ete tape. */
    if (en != NULL && fr != NULL && strcmp(en, fr) != 0 && len > 0 && len < 200) {
        /* Paires accent-only (Détection/Detection) : owl_name_matches est
         * accent-insensible et renverrait TOUJOURS l'EN, effacant l'accent d'une
         * forme FR ecrite. On tranche sur les octets tapes (accent-sensible).
         * . */
        if (fmt_accent_only_diff(fr, en)) {
            if (fmt_word_matches_accent_sensitive(word, len, en)) return en;
            if (fmt_word_matches_accent_sensitive(word, len, fr)) return fr;
            /* Forme toleree (accents partiels/absents non tranchables) : FR canon. */
            return fr;
        }
        char buf[256];
        memcpy(buf, word, (size_t)len);
        buf[len] = '\0';
        char q[256];
        owl_normalize_name(buf, q, sizeof q);
        if (owl_name_matches(en, q)) return en;
    }
    return (fr != NULL) ? fr : en;
}

/* Resout le champ `word` (len) dans le type compose ti. Retourne la forme
 * cible du champ, ou NULL si inconnu du type. Pose *outSubType a l'index du
 * type du sous-bloc `<champ> = { … }` (ou -1). Matche FR/ASCII/EN du champ. */
static const char* fmt_composite_field_resolve(int ti, const char* word, int len,
                                               int* outSubType) {
    if (outSubType) *outSubType = -1;
    if (ti < 0 || !owl_fmt_canon || len <= 0 || len >= 200) return NULL;
    char buf[256];
    memcpy(buf, word, (size_t)len);
    buf[len] = '\0';
    char q[256];
    owl_normalize_name(buf, q, sizeof q);
    int nf = owl_fmt_canon->type_field_count(ti);
    for (int fi = 0; fi < nf; fi++) {
        const char* key  = owl_fmt_canon->type_field_key(ti, fi);      /* FR ascii */
        const char* disp = owl_fmt_canon->type_field_display(ti, fi);  /* FR accentue */
        const char* en   = owl_fmt_canon->type_field_en
                               ? owl_fmt_canon->type_field_en(ti, fi) : NULL;
        bool hit = (key  && owl_name_matches(key, q)) ||
                   (disp && owl_name_matches(disp, q)) ||
                   (en   && owl_name_matches(en, q));
        if (hit) {
            if (outSubType) {
                const char* sub = owl_fmt_canon->type_field_type
                                      ? owl_fmt_canon->type_field_type(ti, fi) : NULL;
                if (sub) *outSubType = fmt_composite_type_index(sub, (int)strlen(sub));
            }
            if (g_fmt_target_lang == OWL_TRANSLATE_EN)
                return (en != NULL) ? en : (disp ? disp : key);
            return disp ? disp : key;  /* FR */
        }
    }
    return NULL;
}

/* Si `word` est une constante membre d'enum (a un alias EN), retourne sa forme
 * cible ; sinon NULL. Context-free (nom d'enum member globalement unique).
 * Utilise UNIQUEMENT en valeur RHS a l'interieur d'un bloc compose. */
static const char* fmt_enum_const_target(const char* word, int len) {
    if (!owl_fmt_canon || !owl_fmt_canon->const_en || len <= 0 || len >= 200)
        return NULL;
    char buf[256];
    memcpy(buf, word, (size_t)len);
    buf[len] = '\0';
    char q[256];
    owl_normalize_name(buf, q, sizeof q);
    int nc = owl_fmt_canon->const_count();
    for (int i = 0; i < nc; i++) {
        const char* en = owl_fmt_canon->const_en(i);
        if (!en) continue;  /* pas d'alias EN -> pas de pairing bilingue */
        const char* fr = owl_fmt_canon->const_name(i);
        if ((fr && owl_name_matches(fr, q)) || owl_name_matches(en, q)) {
            if (g_fmt_target_lang == OWL_TRANSLATE_EN) return en;
            return (fr != NULL) ? fr : en;  /* FR */
        }
    }
    return NULL;
}

/* fmt_current_composite_type : defini apres g_fmt_st (voir plus bas). */

/* ================================================================
 * USER-DECLARED SYMBOL TABLE (per owl_format call)
 *
 * Pre-scan of the source collects struct names and their fields so the
 * formatter can normalize case ("resultat" -> "Resultat") for symbols
 * the user defined themselves. Cleared at the start of each owl_format
 * call.
 * ================================================================ */

typedef struct { char name[64]; int len; } FmtUserName;

/* Per-variable declared type, harvested from "X est un Y" patterns.
 * Used in after-dot lookup to route field-name normalisation:
 *   - if Y is a user struct -> only user_fields normalisation applies
 *   - if Y is a builtin type -> only registry/property normalisation
 *   - if Y is unknown -> verbatim (no risky guesses)
 */
typedef struct {
    char var[64];
    int  varLen;
    char type[64];
    int  typeLen;
    int  typeKind; /* 0=unknown, 1=user, 2=builtin */
} FmtVarType;

/* ---- Scope-aware locals / params ( Lot 1) ----------------
 * A "scope" is a PROCEDURE / FONCTION body. Locals and params are
 * canonised to their declaration casing ONLY within their own scope so
 * a homonym in another procedure (declared with a different casing) is
 * never bled across. Each scope owns a [startOff, endOff[ byte range in
 * the source; emission maps the current cursor offset to a scope id. */
typedef struct { int startOff; int endOff; } FmtScope;

typedef struct { char name[64]; int len; int scopeId; } FmtScopedName;

/* ---- Class members (fields + constants), keyed by class (Lot 1) ----
 * Harvested intra-file. Keyed by class name so two classes declaring the
 * same member in different casings don't collide. */
typedef struct {
    char cls[64];  int clsLen;
    char name[64]; int len;
    char declType[64]; int declTypeLen;  /*  : type du membre ("" si const/enum) */
    /*  — l'entree vient d'un fichier VOISIN et son nom est l'homonyme
     * d'un symbole du registre. Calcule une fois a l'ensemencement (le lookup
     * est sur le chemin chaud, un scan du registre par identifiant emis serait
     * hors de question). Consomme par les helpers any-class, qui resolvent un
     * identifiant NU sans owner : la, rien ne rattache le nom a la classe qui
     * le declare, donc le canon du registre garde la main. La forme
     * `objet.membre`, elle, a son owner et n'est pas concernee. */
    bool ctxRegistryHomonym;
} FmtClassMember;

/* ================================================================
 * ETAT MUTABLE D'UNE SESSION DE FORMATAGE (per-call)
 * ================================================================
 * Toutes les tables de symboles user collectees pendant une passe de
 * formatage. Allouee sur la pile de owl_format_ctx et pointee par g_fmt_st
 * (avec save/restore -> sûr en ré-entrance/nesting). Reset implicite via
 * {0} a chaque appel. Ce n'est PAS un etat global partage : si le formatter
 * devait tourner multi-thread, g_fmt_st deviendrait thread-local. Les tables :
 *   - user_types/user_fields : structs/classes user + leurs champs.
 *   - var_types              : type declare par variable ("X est un Y").
 *   - user_scopes/scoped_names : locals/params bornes a leur scope.
 *   - class_members          : membres de classe keyes par classe (Lot 1).
 *   - enum_members           : membres d'enum user, verbatim (, ).
 *   - user_globals           : identifiants top-level user.
 * ================================================================ */
typedef struct OwlFormatState {
    FmtUserName    user_types[256];     int user_types_count;
    FmtUserName    user_fields[1024];   int user_fields_count;
    FmtVarType     var_types[512];      int var_types_count;
    FmtScope       user_scopes[256];    int user_scopes_count;
    FmtScopedName  scoped_names[1024];  int scoped_names_count;
    FmtClassMember class_members[1024]; int class_members_count;
    FmtUserName    enum_members[1024];  int enum_members_count;
    FmtUserName    user_globals[512];   int user_globals_count;
    /*  — noms de procedures/fonctions globales (callables) user. Sous-
     * ensemble de user_globals restreint aux symboles appelables, pour que la
     * restauration de graphie sur un appel NU (accent/casse insensible) ne
     * concerne QUE de vrais callables et ne degrade pas un appel natif dont un
     * champ user homonyme traine dans user_globals (`Val(...)` vs champ `val`). */
    FmtUserName    user_procs[512];     int user_procs_count;
    /*  — pile de contexte de type compose pour `owl translate`
     * type-aware sur la DSL themes/styles. type_stack[d] = index registre du
     * type compose ouvert au niveau de brace d (-1 si inconnu). pending_type =
     * index du type que le prochain `{` empilera (pose par la resolution du
     * type d'une declaration ou du sous-bloc d'un champ). Actif uniquement en
     * mode translate ; persiste a travers les appels par-ligne de
     * format_line_content (d'ou son sejour ici et non en local). pending_type
     * est remis a -1 par owl_format_ctx apres le memset (zero-init = 0 = index
     * de type valide, donc sentinelle explicite requise). */
    int            type_stack[64];      int type_sp;
    int            pending_type;
    /*  — index registre du type de style racine de la declaration de
     * controle courante (`ONG_X est un Champ Onglet` -> styleOnglet), -1 si la
     * sorte n'a pas de style typé. C'est le porteur de contexte INTER-LIGNES de
     * la cascade `Style..X..Y` du bloc PROPRIETES, la ou la forme bloc porte le
     * sien sur ses accolades. Pose par la passe d'indentation (qui voit deja la
     * declaration), consomme par format_line_content. */
    int            widget_style_root;
    /*  — index registre du type META du bloc top-level courant (`X est
     * un Projet`), -1 hors d'un tel bloc. Porteur inter-lignes qui permet a
     * `owl translate` de resoudre les cles du bloc PROPRIETES du Projet contre
     * les champs du type (Langues<->Languages, FenêtreDémarrage<->StartupWindow)
     * — ces cles sont des champs de type builtin, pas des proprietes DSL de
     * controle. Pose par la passe d'indentation sur la declaration `est un
     * Projet` (indent 0), remis a -1 a chaque nouvelle ligne top-level. */
    int            enclosing_meta_type;
} OwlFormatState;

/* Pointe l'etat de la session courante (pose par owl_format_ctx). */
static OwlFormatState* g_fmt_st = NULL;

/* Index du type compose au sommet de la pile de contexte translate,
 * -1 si hors bloc compose ou type inconnu. */
static int fmt_current_composite_type(void) {
    if (!g_fmt_st || g_fmt_st->type_sp <= 0) return -1;
    int d = g_fmt_st->type_sp - 1;
    if (d >= 64) return -1;
    return g_fmt_st->type_stack[d];
}

/* ---- Contexte cross-fichier ---------------------------
 * Symboles declares dans les fichiers voisins, seedes par le caller via
 * l'API owl_format_context_*. Read-only pendant le format. */
typedef struct {
    char name[64]; int len;
} FmtCtxType;
typedef struct {
    char owner[64]; int ownerLen;
    char name[64];  int len;
    char declType[64]; int declTypeLen;
    bool isEnum;    /* membre d'enumeration (reference bare) */
} FmtCtxMember;

struct OwlFormatContext {
    FmtCtxType* types;     int typeCount;   int typeCap;
    FmtCtxMember* members; int memberCount; int memberCap;
    /*  — globals user (variables, constantes, procedures top-level)
     * declares dans d'autres fichiers du projet. Forme bare, sans owner. */
    FmtCtxType* globals;   int globalCount; int globalCap;
    /*  — sous-ensemble callable (procedures/fonctions) des globals.
     * Sert a restaurer la graphie d'un appel NU accent/casse-insensible sans
     * confondre avec une variable/constante globale homonyme. */
    FmtCtxType* procGlobals; int procGlobalCount; int procGlobalCap;
};

OwlFormatContext* owl_format_context_new(void) {
    OwlFormatContext* c = calloc(1, sizeof(OwlFormatContext));
    return c;
}

void owl_format_context_free(OwlFormatContext* ctx) {
    if (!ctx) return;
    free(ctx->types);
    free(ctx->members);
    free(ctx->globals);
    free(ctx->procGlobals);
    free(ctx);
}

void owl_format_context_add_type(OwlFormatContext* ctx, const char* name) {
    if (!ctx || !name) return;
    int len = (int)strlen(name);
    if (len <= 0 || len >= 64) return;
    if (ctx->typeCount >= ctx->typeCap) {
        int nc = ctx->typeCap < 16 ? 16 : ctx->typeCap * 2;
        ctx->types = realloc(ctx->types, sizeof(FmtCtxType) * nc);
        ctx->typeCap = nc;
    }
    FmtCtxType* t = &ctx->types[ctx->typeCount++];
    memcpy(t->name, name, len); t->name[len] = '\0'; t->len = len;
}

void owl_format_context_add_member(OwlFormatContext* ctx, const char* owner,
                                   const char* name, const char* declType) {
    if (!ctx || !owner || !name) return;
    int ol = (int)strlen(owner);
    int nl = (int)strlen(name);
    if (ol <= 0 || ol >= 64 || nl <= 0 || nl >= 64) return;
    if (ctx->memberCount >= ctx->memberCap) {
        int nc = ctx->memberCap < 32 ? 32 : ctx->memberCap * 2;
        ctx->members = realloc(ctx->members, sizeof(FmtCtxMember) * nc);
        ctx->memberCap = nc;
    }
    FmtCtxMember* m = &ctx->members[ctx->memberCount++];
    memcpy(m->owner, owner, ol); m->owner[ol] = '\0'; m->ownerLen = ol;
    memcpy(m->name, name, nl); m->name[nl] = '\0'; m->len = nl;
    m->isEnum = false;
    if (declType && declType[0]) {
        int dl = (int)strlen(declType);
        if (dl >= 64) dl = 63;
        memcpy(m->declType, declType, dl); m->declType[dl] = '\0';
        m->declTypeLen = dl;
    } else {
        m->declType[0] = '\0'; m->declTypeLen = 0;
    }
}

void owl_format_context_add_enum_member(OwlFormatContext* ctx,
                                        const char* owner, const char* name) {
    if (!ctx || !owner || !name) return;
    int before = ctx->memberCount;
    owl_format_context_add_member(ctx, owner, name, NULL);
    if (ctx->memberCount > before) {
        ctx->members[ctx->memberCount - 1].isEnum = true;
    }
}

void owl_format_context_add_global(OwlFormatContext* ctx, const char* name) {
    if (!ctx || !name) return;
    int len = (int)strlen(name);
    if (len <= 0 || len >= 64) return;
    if (ctx->globalCount >= ctx->globalCap) {
        int nc = ctx->globalCap < 16 ? 16 : ctx->globalCap * 2;
        ctx->globals = realloc(ctx->globals, sizeof(FmtCtxType) * nc);
        ctx->globalCap = nc;
    }
    FmtCtxType* g = &ctx->globals[ctx->globalCount++];
    memcpy(g->name, name, len); g->name[len] = '\0'; g->len = len;
}

void owl_format_context_add_proc(OwlFormatContext* ctx, const char* name) {
    if (!ctx || !name) return;
    int len = (int)strlen(name);
    if (len <= 0 || len >= 64) return;
    if (ctx->procGlobalCount >= ctx->procGlobalCap) {
        int nc = ctx->procGlobalCap < 16 ? 16 : ctx->procGlobalCap * 2;
        ctx->procGlobals = realloc(ctx->procGlobals, sizeof(FmtCtxType) * nc);
        ctx->procGlobalCap = nc;
    }
    FmtCtxType* g = &ctx->procGlobals[ctx->procGlobalCount++];
    memcpy(g->name, name, len); g->name[len] = '\0'; g->len = len;
}

/* fwd : defini plus bas, utilise par seed_from_context. */
static void user_symbols_add(FmtUserName* table, int* count, int cap,
                              const char* name, int len);
static void var_type_add(const char* var, int varLen,
                         const char* type, int typeLen);

/*  — `name` est-il un symbole du REGISTRE (a la casse et aux accents
 * pres) ? Sert de filtre a l'ensemencement cross-fichier : une declaration
 * utilisateur faite dans un fichier VOISIN ne devient jamais la reference de
 * graphie d'un homonyme du langage lu dans un AUTRE fichier.
 *
 *  posait ce filtre pour les natives seules, et le motif s'est repete :
 * un voisin declarant `police est un entier` faisait ecrire `police..Taille`,
 * un voisin declarant l'enumeration `Couleur { bleu }` faisait ecrire `bleu`
 * la ou le registre dit `Bleu`, un voisin declarant `x est un entier` faisait
 * ecrire `{x = 10}` la ou le registre dit `..X`. La passe cessait d'etre
 * monotone : elle rendait le fichier MOINS canonique qu'avant.
 *
 * Le canon d'un symbole du langage se lit dans le registre, jamais chez le
 * voisin. La declaration faite dans le fichier COURANT garde, elle, sa
 * priorite (harvest local, cf.  et ) : elle est sous les yeux
 * du lecteur, et c'est ce qui la distingue.
 *
 * Le filtre couvre la surface qu'expose le provider (natives, constantes,
 * types builtin, proprietes builtin, proprietes de bloc DSL, methodes
 * d'extension) plus les constantes "magiques" et les formes heritees de fin de
 * ligne, qui vivent dans fmt_keywords[]. Volontairement PAS les mots-cles :
 * un mot-cle n'est pas ensemencable comme nom d'utilisateur, et fmt_keywords[]
 * a deja son propre chemin. */
static bool fmt_name_collides_registry(const char* name, int len) {
    if (len <= 0 || len >= 64) return false;
    char buf[64];
    memcpy(buf, name, (size_t)len);
    buf[len] = '\0';

    /* Constantes du langage portees par fmt_keywords[] (TAB, RC, Vrai…). */
    if (owl_formatter_magic_const_canonical(buf) != NULL) return true;
    if (owl_formatter_eol_legacy_canonical(buf) != NULL) return true;

    if (owl_fmt_canon == NULL) return false;

    char normalized[64];
    owl_normalize_name(buf, normalized, sizeof normalized);

    int n = owl_fmt_canon->native_count();
    for (int i = 0; i < n; i++) {
        const char* c = owl_fmt_canon->native_canonical(i);
        if (c && owl_name_matches(c, normalized)) return true;
        const char* en = owl_fmt_canon->native_en(i);
        if (en && owl_name_matches(en, normalized)) return true;
    }
    n = owl_fmt_canon->const_count();
    for (int i = 0; i < n; i++) {
        const char* c = owl_fmt_canon->const_name(i);
        if (c && owl_name_matches(c, normalized)) return true;
    }
    n = owl_fmt_canon->type_count();
    for (int i = 0; i < n; i++) {
        const char* c = owl_fmt_canon->type_canonical(i);
        if (c && owl_name_matches(c, normalized)) return true;
        const char* a = owl_fmt_canon->type_ascii(i);
        if (a && owl_name_matches(a, normalized)) return true;
    }
    n = owl_fmt_canon->builtin_prop_count();
    for (int i = 0; i < n; i++) {
        const char* c = owl_fmt_canon->builtin_prop_fr(i);
        if (c && owl_name_matches(c, normalized)) return true;
        const char* en = owl_fmt_canon->builtin_prop_en(i);
        if (en && owl_name_matches(en, normalized)) return true;
    }
    n = owl_fmt_canon->dsl_prop_count();
    for (int i = 0; i < n; i++) {
        const char* c = owl_fmt_canon->dsl_prop_fr(i);
        if (c && owl_name_matches(c, normalized)) return true;
        const char* a = owl_fmt_canon->dsl_prop_ascii(i);
        if (a && owl_name_matches(a, normalized)) return true;
        const char* en = owl_fmt_canon->dsl_prop_en(i);
        if (en && owl_name_matches(en, normalized)) return true;
    }
    n = owl_fmt_canon->method_count();
    for (int i = 0; i < n; i++) {
        const char* c = owl_fmt_canon->method_name(i);
        if (c && owl_name_matches(c, normalized)) return true;
    }
    return false;
}

/* Seed les tables internes du formatter avec le contexte cross-fichier.
 * Appele APRES le harvest local (user_symbols_collect) : le local a deja
 * rempli les tables, donc les declarations locales restent prioritaires
 * (dedup case-sensitive first-wins). Le contexte ne fait qu'ajouter ce qui
 * manque. enum members du contexte -> enum_members (canon bare). */
static void seed_from_context(const OwlFormatContext* ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->typeCount; i++) {
        user_symbols_add(g_fmt_st->user_types, &g_fmt_st->user_types_count, 256,
                         ctx->types[i].name, ctx->types[i].len);
    }
    for (int i = 0; i < ctx->memberCount; i++) {
        const FmtCtxMember* cm = &ctx->members[i];
        if (cm->ownerLen <= 0 || cm->ownerLen >= 64 ||
            cm->len <= 0 || cm->len >= 64) continue;
        /* class_members keyed by owner — dedup case-sensitive first-wins. */
        bool dup = false;
        for (int j = 0; j < g_fmt_st->class_members_count; j++) {
            if (g_fmt_st->class_members[j].clsLen == cm->ownerLen &&
                strncasecmp(g_fmt_st->class_members[j].cls, cm->owner, cm->ownerLen) == 0 &&
                g_fmt_st->class_members[j].len == cm->len &&
                strncasecmp(g_fmt_st->class_members[j].name, cm->name, cm->len) == 0) {
                dup = true; break;
            }
        }
        if (!dup && g_fmt_st->class_members_count < 1024) {
            FmtClassMember* m = &g_fmt_st->class_members[g_fmt_st->class_members_count++];
            memcpy(m->cls, cm->owner, cm->ownerLen); m->cls[cm->ownerLen] = '\0';
            m->clsLen = cm->ownerLen;
            memcpy(m->name, cm->name, cm->len); m->name[cm->len] = '\0';
            m->len = cm->len;
            if (cm->declTypeLen > 0) {
                memcpy(m->declType, cm->declType, cm->declTypeLen);
                m->declType[cm->declTypeLen] = '\0';
                m->declTypeLen = cm->declTypeLen;
            } else { m->declType[0] = '\0'; m->declTypeLen = 0; }
            m->ctxRegistryHomonym = fmt_name_collides_registry(cm->name, cm->len);
        }
        /* Membre d'enum : aussi enregistre bare (canon hors acces `.`).
         *  : sauf s'il collisionne avec un symbole du registre. Une
         * reference NUE est le seul cas ou la graphie d'un voisin peut atterrir
         * sur un symbole du langage sans que rien ne le rattache a l'enumeration
         * qui la declare — c'est par la que `Bleu` devenait `bleu`. La forme
         * QUALIFIEE (`Couleur.bleu`) passe par class_members, keyee par son
         * proprietaire, donc non concernee. */
        if (cm->isEnum && !fmt_name_collides_registry(cm->name, cm->len)) {
            user_symbols_add(g_fmt_st->enum_members, &g_fmt_st->enum_members_count, 1024,
                             cm->name, cm->len);
        }
    }
    /*  : seed les globals cross-fichier dans user_globals (locaux
     * prioritaires, le contexte ne fait que completer ce qui manque). On
     * seede aussi via var_type_add pour que is_var_name() reconnaisse ces
     * noms et active le branch user_globals au lookup.
     *
     *  : skip si une entree locale existe deja dans var_types pour ce
     * nom. var_type_add() dedupe sur le nom et ECRASE le slot (typeKind
     * inclus) — un global cross-fichier "oP" surcharge alors un local "oP est
     * une Personne" en typeKind=0, et la route after-dot tombe sur le
     * fallback property_norm builtin (oP.nom -> oP.Nom).
     *
     *  : skip si le nom collide avec une native du registry. Sans ce
     * filtre, une declaration user du type `trace est une chaine` dans un
     * fichier du projet contaminerait le formatter d'autres fichiers : tout
     * `Trace(...)` ailleurs serait reformatte en `trace(...)` (canon user
     * shadow canon native). Le user qui declare un homonyme accepte
     * implicitement le warning compile-time, mais le formatter ne doit pas
     * activer la canon user au prix du canon native.
     *
     *  : le filtre couvre desormais tout le registre, pas seulement
     * les natives. Le meme mecanisme degradait `Police..Taille` (propriete de
     * bloc) et `{X = 10}` (propriete builtin) depuis un voisin declarant
     * `police` / `x`. Cf. fmt_name_collides_registry. */
    for (int i = 0; i < ctx->globalCount; i++) {
        const FmtCtxType* g = &ctx->globals[i];

        if (fmt_name_collides_registry(g->name, g->len)) continue;

        user_symbols_add(g_fmt_st->user_globals, &g_fmt_st->user_globals_count, 512, g->name, g->len);
        bool local_var = false;
        for (int j = 0; j < g_fmt_st->var_types_count; j++) {
            if (g_fmt_st->var_types[j].varLen == g->len &&
                strncasecmp(g_fmt_st->var_types[j].var, g->name, g->len) == 0) {
                local_var = true;
                break;
            }
        }
        if (!local_var) var_type_add(g->name, g->len, "global", 6);
    }
    /*  : seed les procedures/fonctions globales cross-fichier dans
     * user_procs. La restauration de graphie d'un appel NU s'appuie sur cette
     * table (callables uniquement). Pas de filtre native ici : la regle
     * d'homonymie veut que la def user du projet gagne sur un homonyme natif. */
    for (int i = 0; i < ctx->procGlobalCount; i++) {
        const FmtCtxType* g = &ctx->procGlobals[i];
        user_symbols_add(g_fmt_st->user_procs, &g_fmt_st->user_procs_count, 512,
                         g->name, g->len);
    }
}

static int user_type_kind_lookup(const char* type, int typeLen) {
    /* User struct? */
    for (int i = 0; i < g_fmt_st->user_types_count; i++) {
        if (g_fmt_st->user_types[i].len == typeLen &&
            strncasecmp(g_fmt_st->user_types[i].name, type, typeLen) == 0) return 1;
    }
    /* Builtin type? */
    if (typeLen > 0 && typeLen < 200) {
        char buf[256]; memcpy(buf, type, typeLen); buf[typeLen] = '\0';
        int nt = owl_fmt_canon ? owl_fmt_canon->type_count() : 0;
        for (int i = 0; i < nt; i++) {
            const char* a = owl_fmt_canon->type_ascii(i);
            const char* canon = owl_fmt_canon->type_canonical(i);
            if ((a && strcasecmp(buf, a) == 0) ||
                (canon && strcasecmp(buf, canon) == 0)) return 2;
        }
    }
    return 0;
}

static int find_var_type_kind(const char* var, int varLen) {
    for (int i = 0; i < g_fmt_st->var_types_count; i++) {
        if (g_fmt_st->var_types[i].varLen == varLen &&
            strncasecmp(g_fmt_st->var_types[i].var, var, varLen) == 0) {
            return g_fmt_st->var_types[i].typeKind;
        }
    }
    return 0;
}

/* Returns true when `word` is a declared variable name (LHS of "X est un/une Y").
 * Used to prevent normalising a variable to the casing of its type:
 * "personne" must not become "Personne" just because "Personne" is a known struct. */
static bool is_var_name(const char* word, int len) {
    if (len <= 0 || len >= 64) return false;
    for (int i = 0; i < g_fmt_st->var_types_count; i++) {
        if (g_fmt_st->var_types[i].varLen == len &&
            strncasecmp(g_fmt_st->var_types[i].var, word, len) == 0) return true;
    }
    return false;
}

static void user_symbols_reset(void) {
    g_fmt_st->user_types_count = 0;
    g_fmt_st->user_fields_count = 0;
    g_fmt_st->var_types_count = 0;
    g_fmt_st->user_scopes_count = 0;
    g_fmt_st->scoped_names_count = 0;
    g_fmt_st->class_members_count = 0;
    g_fmt_st->enum_members_count = 0;
    g_fmt_st->user_globals_count = 0;
    g_fmt_st->user_procs_count = 0;
}

static void var_type_add(const char* var, int varLen, const char* type, int typeLen) {
    if (g_fmt_st->var_types_count >= 512) return;
    if (varLen <= 0 || varLen >= 64 || typeLen <= 0 || typeLen >= 64) return;
    /* Dedup on var name (last decl wins — harmless for casing). */
    int slot = -1;
    for (int i = 0; i < g_fmt_st->var_types_count; i++) {
        if (g_fmt_st->var_types[i].varLen == varLen &&
            strncasecmp(g_fmt_st->var_types[i].var, var, varLen) == 0) { slot = i; break; }
    }
    if (slot < 0) slot = g_fmt_st->var_types_count++;
    memcpy(g_fmt_st->var_types[slot].var, var, varLen);
    g_fmt_st->var_types[slot].var[varLen] = '\0';
    g_fmt_st->var_types[slot].varLen = varLen;
    memcpy(g_fmt_st->var_types[slot].type, type, typeLen);
    g_fmt_st->var_types[slot].type[typeLen] = '\0';
    g_fmt_st->var_types[slot].typeLen = typeLen;
    g_fmt_st->var_types[slot].typeKind = user_type_kind_lookup(type, typeLen);
}

static void user_symbols_add(FmtUserName* table, int* count, int cap,
                              const char* name, int len) {
    if (*count >= cap) return;
    if (len <= 0 || len >= 64) return;
    /* Dedup case-sensitive: keep first occurrence as canonical. */
    for (int i = 0; i < *count; i++) {
        if (table[i].len == len && memcmp(table[i].name, name, len) == 0) return;
    }
    /* If a different-case version exists, keep the first one — the canonical
     * form is the FIRST declaration encountered (the struct definition). */
    for (int i = 0; i < *count; i++) {
        if (table[i].len == len && strncasecmp(table[i].name, name, len) == 0) return;
    }
    memcpy(table[*count].name, name, len);
    table[*count].name[len] = '\0';
    table[*count].len = len;
    (*count)++;
}

static bool user_name_known(FmtUserName* table, int count,
                             const char* word, int len) {
    if (len <= 0 || len >= 64) return false;
    for (int i = 0; i < count; i++) {
        if (table[i].len == len &&
            strncasecmp(table[i].name, word, len) == 0) return true;
    }
    return false;
}

static const char* find_user_canonical(FmtUserName* table, int count,
                                        const char* word, int len) {
    if (len <= 0 || len >= 64) return NULL;
    for (int i = 0; i < count; i++) {
        if (table[i].len == len &&
            strncasecmp(table[i].name, word, len) == 0) {
            if (memcmp(table[i].name, word, len) != 0) return table[i].name;
            return NULL; /* already canonical */
        }
    }
    return NULL;
}

/*  : variantes accent-insensibles pour la resolution d'un appel nu de
 * procedure/fonction globale. `find_user_canonical`/`is_var_name` comparent en
 * bytes (strncasecmp) : un appel ASCII `ReinitialiseTout` ne matche jamais la
 * declaration accentuee `RéinitialiseTout` (longueurs differentes, é = 2 octets).
 * Pour restaurer la graphie de declaration sur un appel nu, on matche comme le
 * runtime resout les identifiants : casse ET accent insensibles. Declare ici en
 * prototype, defini apres ident_equal_ci_accent (plus bas dans le fichier). */
static const char* find_user_canonical_ci_accent(FmtUserName* table, int count,
                                                  const char* word, int len);

static bool fmt_is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80;
}

/* Avance *p depuis un '"' ouvrant jusqu'apres le '"' fermant, selon la regle
 * canonique des chaines OWL (cf. scanner.c::scan_string) : le backslash n'a
 * AUCUN role special ; un '"' ferme la chaine SAUF doublage "" (deux
 * guillemets consecutifs = un guillemet litteral, on reste dans la chaine).
 * Traiter `\"` comme un echappement C decalait les frontieres de chaines et
 * canonicalisait des mots-cles a l'interieur de la chaine suivante.
 *
 * Bornes : toujours '\0' ; 'end' si non-NULL (buffer borne par longueur, non
 * NUL-termine) ; '\n' si lineScoped (analyse par ligne physique — les chaines
 * "..." peuvent legalement enjamber un '\n', donc les passes qui raisonnent
 * ligne par ligne coupent au '\n', celles qui balayent le buffer complet ne
 * le font pas). A l'appel *p pointe sur le '"' ouvrant ; au retour *p pointe
 * juste apres le '"' fermant, ou sur la borne terminale si non ferme. */
static void fmt_skip_string_literal(const char** p, const char* end,
                                    bool lineScoped) {
    const char* s = *p + 1;  /* skip opening " */
    while (*s != '\0' && (end == NULL || s < end)) {
        if (lineScoped && *s == '\n') break;
        if (*s == '"') {
            if ((end == NULL || s + 1 < end) && s[1] == '"') {
                s += 2;  /* doublage "" : guillemet litteral, on reste dedans */
                continue;
            }
            s++;  /* vrai guillemet fermant, consomme */
            break;
        }
        s++;
    }
    *p = s;
}

/*  : les collecteurs de symboles user comparent du texte brut (lexeme),
 * pas des tokens. La copule s'ecrit `est` (FR) ou `is` (EN) ; ces deux formes
 * mappent au meme TOKEN_EST au scanner, mais ici on est sur du lexeme. Sans
 * gerer `is`, aucun symbole user n'est collecte dans un fichier EN -> casse et
 * accents jamais restaures au format. */
static bool fmt_is_copule(const char* w, int len) {
    return (len == 3 && strncasecmp(w, "est", 3) == 0) ||
           (len == 2 && strncasecmp(w, "is", 2) == 0);
}

/*  : le mot courant est-il en POSITION DE TYPE ? Un identifiant choisi
 * par l'utilisateur en reste un partout — a la declaration comme a l'usage —
 * et n'appartient donc pas au canon du langage. Seule la position de type
 * releve du canon. Positions relevees par sonde sur le chemin find_keyword :
 *   - apres la copule et son article : `est un Corps`, `is a Body`
 *   - apres un prefixe de widget     : `Champ Bouton`, `Control Button`
 *   - apres un connecteur de type    : `tableau de chaînes`, `Champ de Saisie`
 * `seenCopule` (portee ligne) evite de prendre `de` pour un connecteur de type
 * hors declaration — sans lui, `POUR TOUT e DE table` renommerait `table`. */
static bool fmt_in_type_position(const char* lastWord, int lastWordLen,
                                 bool seenCopule) {
    if (!seenCopule || lastWord == NULL || lastWordLen <= 0) return false;
    char lw[16];
    int ll = scanner_strip_accents(lastWord, lastWordLen, lw, sizeof(lw));
    if (ll <= 0 || ll >= (int)sizeof(lw)) return false;
    for (int i = 0; i < ll; i++) lw[i] = (char)tolower((unsigned char)lw[i]);
    lw[ll] = '\0';
    static const char* const CONNECTORS[] = {
        "est", "is", "sont", "are",       /* copule */
        "un", "une", "des", "a", "an",    /* article filler */
        "de", "of",                       /* type composee / prefixe widget */
        "champ", "field", "control",      /* prefixe widget */
        NULL
    };
    for (int i = 0; CONNECTORS[i] != NULL; i++)
        if (strcmp(lw, CONNECTORS[i]) == 0) return true;
    return false;
}

/*  : la copule ouvre la zone d'annotation de type sur la ligne. */
static bool fmt_word_is_copule_any(const char* norm, int len) {
    return (len == 3 && strcmp(norm, "est")  == 0) ||
           (len == 2 && strcmp(norm, "is")   == 0) ||
           (len == 4 && strcmp(norm, "sont") == 0) ||
           (len == 3 && strcmp(norm, "are")  == 0);
}

/*  : article filler optionnel apres la copule. FR : `un`/`une`.
 * EN : `a`/`an` (cf. consume_en_a_an_filler cote compiler). En position
 * post-copule, `a` est l'article et non le mot-cle de plage `A`/`to`. */
static bool fmt_is_article_filler(const char* w, int len) {
    return (len == 2 && strncasecmp(w, "un", 2) == 0) ||
           (len == 3 && strncasecmp(w, "une", 3) == 0) ||
           (len == 1 && (w[0] == 'a' || w[0] == 'A')) ||
           (len == 2 && strncasecmp(w, "an", 2) == 0);
}

/* Pre-scan the source: locate every "Name est une Structure ... FIN" and
 * record the type name + its field names. Robust to whitespace, comments,
 * accented "structure"/"struct" (we lowercase ASCII for matching). */
static void user_symbols_collect(const char* src) {
    user_symbols_reset();
    const char* p = src;
    while (*p) {
        /* Skip strings (so a quoted "Structure" doesn't trigger). */
        if (*p == '"') {
            fmt_skip_string_literal(&p, NULL, false);
            continue;
        }
        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (!fmt_is_word_char(*p)) { p++; continue; }
        const char* nameStart = p;
        while (fmt_is_word_char(*p)) p++;
        int nameLen = (int)(p - nameStart);
        const char* afterName = p;
        /* Look for whitespace then "est" (case-insens). */
        while (*p == ' ' || *p == '\t') p++;
        if (!fmt_is_word_char(*p)) continue;
        const char* w1 = p;
        while (fmt_is_word_char(*p)) p++;
        int w1len = (int)(p - w1);
        if (!fmt_is_copule(w1, w1len)) {
            p = afterName; continue;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (!fmt_is_word_char(*p)) continue;
        const char* w2 = p;
        while (fmt_is_word_char(*p)) p++;
        int w2len = (int)(p - w2);
        /* Skip "un"/"une"/"a"/"an" article filler. */
        if (fmt_is_article_filler(w2, w2len)) {
            while (*p == ' ' || *p == '\t') p++;
            if (!fmt_is_word_char(*p)) continue;
            w2 = p;
            while (fmt_is_word_char(*p)) p++;
            w2len = (int)(p - w2);
        }
        /* Accent-stripped lowercase form of the type keyword (handles
         * accented openers like « Énumération »). */
        char w2norm[24];
        int w2nl = scanner_strip_accents(w2, w2len, w2norm, sizeof(w2norm));
        if (w2nl > 0 && w2nl < (int)sizeof(w2norm)) {
            for (int i = 0; i < w2nl; i++) w2norm[i] = (char)tolower((unsigned char)w2norm[i]);
            w2norm[w2nl] = '\0';
        } else { w2norm[0] = '\0'; }
        if (w2len >= 9 && strncasecmp(w2, "structure", 9) == 0) {
            user_symbols_add(g_fmt_st->user_types, &g_fmt_st->user_types_count, 256, nameStart, nameLen);
            /* Now read fields until FIN. Pattern per field: <name> est ... */
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (*p == '/' && p[1] == '/') {
                    while (*p && *p != '\n') p++;
                    continue;
                }
                if (!fmt_is_word_char(*p)) { p++; continue; }
                const char* fStart = p;
                while (fmt_is_word_char(*p)) p++;
                int fLen = (int)(p - fStart);
                if (fLen == 3 && strncasecmp(fStart, "fin", 3) == 0) break;
                /* Look for " est ..." after the field name. */
                const char* save = p;
                while (*p == ' ' || *p == '\t') p++;
                if (fmt_is_word_char(*p)) {
                    const char* k = p;
                    while (fmt_is_word_char(*p)) p++;
                    int kl = (int)(p - k);
                    if (fmt_is_copule(k, kl)) {
                        user_symbols_add(g_fmt_st->user_fields, &g_fmt_st->user_fields_count, 1024,
                                         fStart, fLen);
                        /* Skip rest of the line. */
                        while (*p && *p != '\n') p++;
                        continue;
                    }
                }
                p = save;
                /* Not a field decl line: skip to next line. */
                while (*p && *p != '\n') p++;
            }
        } else if ((w2len == 6 && strncasecmp(w2, "classe", 6) == 0) ||
                   (w2len == 5 && strncasecmp(w2, "class", 5) == 0)) {
            /* Classes: register the type name AND harvest fields + constants
             * (Lot 1). We must NOT harvest method names: a PROCEDURE/FONCTION
             * inside the body opens its own block, so we skip its content up
             * to the matching FIN. Visibility markers (PUBLIC/PRIVÉE/...) and
             * CONSTANT(E) blocks are handled explicitly. */
            const char* clsName = nameStart;
            int clsNameLen = nameLen;
            user_symbols_add(g_fmt_st->user_types, &g_fmt_st->user_types_count, 256, clsName, clsNameLen);
            int depth = 1;       /* class body depth (this class = 1) */
            bool inConst = false;
            while (*p && depth > 0) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (*p == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
                if (*p == '"') { /* skip string */
                    fmt_skip_string_literal(&p, NULL, false); continue;
                }
                if (!fmt_is_word_char(*p)) { p++; continue; }
                const char* mStart = p;
                while (fmt_is_word_char(*p)) p++;
                int mLen = (int)(p - mStart);

                char kw[16];
                int kl = scanner_strip_accents(mStart, mLen, kw, sizeof(kw));
                if (kl > 0 && kl < (int)sizeof(kw)) {
                    for (int i = 0; i < kl; i++) kw[i] = (char)tolower((unsigned char)kw[i]);
                    kw[kl] = '\0';
                } else { kw[0] = '\0'; }

                if (strcmp(kw, "fin") == 0 || strcmp(kw, "end") == 0) {
                    if (inConst) { inConst = false; }
                    else { depth--; }
                    while (*p && *p != '\n') p++;
                    continue;
                }
                if (strcmp(kw, "constante") == 0 || strcmp(kw, "constant") == 0) {
                    inConst = true;
                    while (*p && *p != '\n') p++;
                    continue;
                }
                /* Visibility markers: no member, skip line. */
                if (strcmp(kw, "public") == 0 || strcmp(kw, "publique") == 0 ||
                    strcmp(kw, "prive") == 0 || strcmp(kw, "privee") == 0 ||
                    strcmp(kw, "private") == 0 || strcmp(kw, "protege") == 0 ||
                    strcmp(kw, "protegee") == 0 || strcmp(kw, "protected") == 0) {
                    while (*p && *p != '\n') p++;
                    continue;
                }
                /* Method: PROCEDURE/FONCTION opens a nested block — skip its
                 * whole body so the locals aren't harvested.  : on
                 * capture quand meme le NOM de la methode comme class_member,
                 * pour que `recepteur.Methode()` retrouve la graphie de
                 * declaration a l'appel (sinon le formatter strippe l'accent
                 * vers une forme native homonyme, ou laisse une casse
                 * divergente). Le corps reste skippe (depth++). */
                if (strcmp(kw, "procedure") == 0 || strcmp(kw, "fonction") == 0 ||
                    strcmp(kw, "function") == 0) {
                    depth++;
                    /* Peek le nom de methode : skip un eventuel modificateur de
                     * visibilite (PUBLIC/PRIVE[E]/PROTEGE[E]), puis le 1er mot
                     * est le nom. Forme `Classe.Methode` (decl externe inline)
                     * non traitee ici — ce path ne voit que les methodes
                     * declarees dans le corps de la classe. */
                    const char* mq = p;
                    while (*mq == ' ' || *mq == '\t') mq++;
                    if (fmt_is_word_char(*mq)) {
                        const char* nm = mq;
                        while (fmt_is_word_char(*mq)) mq++;
                        int nmLen = (int)(mq - nm);
                        char vkw[16];
                        int vkl = scanner_strip_accents(nm, nmLen, vkw, sizeof(vkw));
                        if (vkl > 0 && vkl < (int)sizeof(vkw)) {
                            for (int i = 0; i < vkl; i++)
                                vkw[i] = (char)tolower((unsigned char)vkw[i]);
                            vkw[vkl] = '\0';
                        } else { vkw[0] = '\0'; }
                        bool isVis = strcmp(vkw, "public") == 0 ||
                                     strcmp(vkw, "publique") == 0 ||
                                     strcmp(vkw, "prive") == 0 ||
                                     strcmp(vkw, "privee") == 0 ||
                                     strcmp(vkw, "private") == 0 ||
                                     strcmp(vkw, "protege") == 0 ||
                                     strcmp(vkw, "protegee") == 0 ||
                                     strcmp(vkw, "protected") == 0;
                        if (isVis) {
                            while (*mq == ' ' || *mq == '\t') mq++;
                            if (fmt_is_word_char(*mq)) {
                                nm = mq;
                                while (fmt_is_word_char(*mq)) mq++;
                                nmLen = (int)(mq - nm);
                            } else { nmLen = 0; }
                        }
                        if (nmLen > 0 && nmLen < 64 &&
                            clsNameLen > 0 && clsNameLen < 64 &&
                            g_fmt_st->class_members_count < 1024) {
                            bool dup = false;
                            for (int i = 0; i < g_fmt_st->class_members_count; i++) {
                                if (g_fmt_st->class_members[i].clsLen == clsNameLen &&
                                    strncasecmp(g_fmt_st->class_members[i].cls,
                                                clsName, clsNameLen) == 0 &&
                                    g_fmt_st->class_members[i].len == nmLen &&
                                    strncasecmp(g_fmt_st->class_members[i].name,
                                                nm, nmLen) == 0) {
                                    dup = true; break;
                                }
                            }
                            if (!dup) {
                                FmtClassMember* mm =
                                    &g_fmt_st->class_members[g_fmt_st->class_members_count++];
                                memcpy(mm->cls, clsName, clsNameLen);
                                mm->cls[clsNameLen] = '\0';
                                mm->clsLen = clsNameLen;
                                memcpy(mm->name, nm, nmLen);
                                mm->name[nmLen] = '\0';
                                mm->len = nmLen;
                                mm->declType[0] = '\0'; mm->declTypeLen = 0;
                                mm->ctxRegistryHomonym = false;  /* declaration locale */
                            }
                        }
                    }
                    while (*p && *p != '\n') p++;
                    continue;
                }
                /* Otherwise: a member declaration. In a CONSTANT block the
                 * form is `NAME = value`; elsewhere `NAME est un Type [= ...]`.
                 * Either way the first word is the member name. */
                if (g_fmt_st->class_members_count < 1024 && mLen > 0 && mLen < 64 &&
                    clsNameLen > 0 && clsNameLen < 64) {
                    /* Dedup case-sensitive (first decl is canonical). */
                    bool dup = false;
                    for (int i = 0; i < g_fmt_st->class_members_count; i++) {
                        if (g_fmt_st->class_members[i].clsLen == clsNameLen &&
                            strncasecmp(g_fmt_st->class_members[i].cls, clsName, clsNameLen) == 0 &&
                            g_fmt_st->class_members[i].len == mLen &&
                            strncasecmp(g_fmt_st->class_members[i].name, mStart, mLen) == 0) {
                            dup = true; break;
                        }
                    }
                    if (!dup) {
                        FmtClassMember* m = &g_fmt_st->class_members[g_fmt_st->class_members_count++];
                        memcpy(m->cls, clsName, clsNameLen); m->cls[clsNameLen] = '\0';
                        m->clsLen = clsNameLen;
                        memcpy(m->name, mStart, mLen); m->name[mLen] = '\0';
                        m->len = mLen;
                        m->declType[0] = '\0'; m->declTypeLen = 0;
                        m->ctxRegistryHomonym = false;  /* declaration locale */
                        /*  : capture le type declare du champ
                         * (`Config est un cConfig`) pour resoudre les chaines
                         * imbriquees a.b.c. `q` repart apres le nom du membre. */
                        const char* q = p;
                        while (*q == ' ' || *q == '\t') q++;
                        if (fmt_is_word_char(*q)) {
                            const char* kw2 = q;
                            while (fmt_is_word_char(*q)) q++;
                            int kw2l = (int)(q - kw2);
                            if (fmt_is_copule(kw2, kw2l)) {
                                while (*q == ' ' || *q == '\t') q++;
                                /* Skip `un`/`une`/`a`/`an` article filler. */
                                if (fmt_is_word_char(*q)) {
                                    const char* tw = q;
                                    while (fmt_is_word_char(*q)) q++;
                                    int twl = (int)(q - tw);
                                    if (fmt_is_article_filler(tw, twl)) {
                                        while (*q == ' ' || *q == '\t') q++;
                                        if (fmt_is_word_char(*q)) {
                                            tw = q;
                                            while (fmt_is_word_char(*q)) q++;
                                            twl = (int)(q - tw);
                                        } else { twl = 0; }
                                    }
                                    if (twl > 0 && twl < 64) {
                                        memcpy(m->declType, tw, twl);
                                        m->declType[twl] = '\0';
                                        m->declTypeLen = twl;
                                    }
                                }
                            }
                        }
                    }
                }
                while (*p && *p != '\n') p++;
            }
        } else if (strcmp(w2norm, "enumeration") == 0) {
            /* Énumération user : register the type name AND harvest
             * each member so the emitter keeps the member's declaration casing
             * instead of canonising it toward a homonym native (actif->Actif).
             * Members are bare identifiers, one per line, until FIN. A member
             * may carry `= value` (explicit ordinal) — first word is the name. */
            user_symbols_add(g_fmt_st->user_types, &g_fmt_st->user_types_count, 256, nameStart, nameLen);
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (*p == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
                if (!fmt_is_word_char(*p)) { p++; continue; }
                const char* mStart = p;
                while (fmt_is_word_char(*p)) p++;
                int mLen = (int)(p - mStart);
                if (mLen == 3 && strncasecmp(mStart, "fin", 3) == 0) break;
                if (mLen == 3 && strncasecmp(mStart, "end", 3) == 0) break;
                user_symbols_add(g_fmt_st->enum_members, &g_fmt_st->enum_members_count, 1024,
                                 mStart, mLen);
                while (*p && *p != '\n') p++;
            }
        } else {
            /* Variable declaration: "X est un Y" where Y is a type token.
             * For collection / variant types we still record the var with
             * typeKind=0 so any earlier entry of the same name (e.g. an
             * outer-scope "c est un Chrono") doesn't bleed into a later
             * "c est un variant" inner-scope access. The 0 forces the
             * conservative no-receiver-type path on after-dot lookup. */
            var_type_add(nameStart, nameLen, w2, w2len);
        }
    }
}

static bool word_opens_block(const char* word, int len);  /* forward */

/*  — verdict de profondeur pour un mot en TETE de ligne, dans les
 * walkers de pre-passe (constant_names_collect, user_globals_collect,
 * user_scopes_collect). Ces trois-la interrogeaient word_opens_block nu, dont
 * la liste large est faite pour l'INDENTATION : elle compte `si` et
 * `constante` sans regarder la suite de la ligne. Consequence : un
 * `SI c ALORS instr` mono-ligne ouvrait une profondeur qui ne se refermait
 * jamais, et tout ce qui suivait sortait du niveau top-level — les globales et
 * les constantes declarees plus bas n'etaient plus recoltees, donc plus
 * canonisees a l'ecriture.
 *
 * Quand la source unique connait le mot, c'est elle qui tranche (elle seule
 * fait le peek). Sinon on garde la liste large du formatter, qui couvre ce que
 * la table ne peut pas couvrir : genres de widget, Theme,
 * CollectionDeProcedures, mots-cles structurels post-copule.
 *
 * `word` pointe dans le buffer source, donc le peek voit la suite de la ligne.
 * Les trois walkers n'examinent que le PREMIER mot de chaque ligne (ils sautent
 * ensuite jusqu'au '\n'), donc `avec` y est toujours en tete d'instruction :
 * la liste large a raison de le compter. */
static bool word_opens_block_here(const char* word, int len) {
    int delta = 0;
    if (prescan_depth_delta_text(word, &delta)) return delta > 0;
    return word_opens_block(word, len);
}

/*  — pre-scan top-level CONSTANT[E] declarations (formes inline et
 * bloc, doc 1514012) et enregistre les noms de constantes comme
 * variables. Permet a is_var_name() de proteger les LHS de bindings
 * (`MAX = 500`) contre la reecriture vers une native homonyme (`Max`).
 * Les CONSTANT[E] de classe sont deja captures par class_members lors de
 * la passe class — pas besoin de les reharvester ici.
 *
 * Pattern reconnu, debut de ligne (apres skip leading ws) :
 *   CONSTANT[E] NAME = literal      (forme inline, une constante)
 *   CONSTANT[E]                     (forme bloc, ouverture)
 *     NAME = literal                (lignes du body, jusqu'a FIN)
 *     ...
 *   FIN
 * On reste conservateur : on scrute juste les debuts de ligne au niveau
 * top-level (pas dans une PROCEDURE / CLASSE — on saute leurs corps via
 * word_opens_block tracking de depth). */
static void constant_names_collect(const char* src) {
    const char* p = src;
    int depth = 0;          /* depth des blocs ouverts (non-CONSTANT) */
    bool inConstBlock = false;  /* on est dans un body CONSTANT bloc top-level */

    while (*p) {
        /* Skip leading whitespace de la ligne. */
        while (*p == ' ' || *p == '\t') p++;
        /* Ligne vide ou commentaire ligne — saute. */
        if (*p == '\n') { p++; continue; }
        if (*p == '\0') break;
        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        /* Skip multi-line strings `[...]` : pas critique ici, on tolere les
         * faux positifs (les noms reels suivront le pattern '= literal'). */

        if (!fmt_is_word_char(*p)) {
            while (*p && *p != '\n') p++;
            continue;
        }
        const char* w = p;
        while (fmt_is_word_char(*p)) p++;
        int wlen = (int)(p - w);

        /* Normalise pour test (lowercase ASCII, strip accents). */
        char kw[16];
        int kl = scanner_strip_accents(w, wlen, kw, sizeof(kw));
        if (kl > 0 && kl < (int)sizeof(kw)) {
            for (int i = 0; i < kl; i++) kw[i] = (char)tolower((unsigned char)kw[i]);
            kw[kl] = '\0';
        } else { kw[0] = '\0'; }

        /* FIN : ferme le bloc en cours. */
        bool isFin = (strcmp(kw, "fin") == 0 || strcmp(kw, "end") == 0);
        if (isFin) {
            if (inConstBlock) {
                inConstBlock = false;
            } else if (depth > 0) {
                depth--;
            }
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Si on est dans un body CONSTANT bloc top-level : la ligne est
         * `NAME = literal`. On enregistre w comme variable (typeKind=0). */
        if (inConstBlock && depth == 0) {
            /* Verifie qu'il y a bien un '=' apres (sinon ce n'est pas un
             * binding — defensive). */
            const char* save = p;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') {
                var_type_add(w, wlen, "constante", 9);
            }
            p = save;
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Au top-level (depth == 0, pas in const block) : detecte
         * CONSTANT[E] ouvrant inline ou bloc. */
        if (depth == 0 && (strcmp(kw, "constante") == 0 || strcmp(kw, "constant") == 0)) {
            /* Skip whitespace. */
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '\n' || *p == '\r' ||
                (p[0] == '/' && p[1] == '/')) {
                /* Forme bloc : pas de IDENT sur la meme ligne. */
                inConstBlock = true;
                while (*p && *p != '\n') p++;
                continue;
            }
            /* Forme inline : IDENT NAME = literal. */
            if (fmt_is_word_char(*p)) {
                const char* nw = p;
                while (fmt_is_word_char(*p)) p++;
                int nwlen = (int)(p - nw);
                /* Verifie le '=' suivant. */
                const char* save2 = p;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '=') {
                    var_type_add(nw, nwlen, "constante", 9);
                }
                p = save2;
            }
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Autres openers (PROCEDURE/FONCTION/CLASSE/STRUCTURE/...) :
         * incrementent depth. word_opens_block_here reconnait tous les
         * keywords structurels et delegue a la source unique le verdict des
         * formes qui ont AUSSI une variante sans FIN. */
        if (depth == 0 && word_opens_block_here(w, wlen)) {
            depth++;
        } else if (depth > 0) {
            /* A l'interieur d'un block non-CONSTANT, on suit toujours les
             * ouvertures imbriquees. */
            if (word_opens_block_here(w, wlen)) depth++;
        }
        while (*p && *p != '\n') p++;
    }
}

/*  — pre-scan des identifiants TOP-LEVEL declares par l'utilisateur :
 *   - variables globales (`gVar est un entier`)
 *   - constantes globales (formes inline et bloc CONSTANT[E])
 *   - noms de procedures/fonctions top-level (`PROCEDURE Foo()`)
 *
 * Stocke la graphie de declaration dans `user_globals[]`. Le branch emit
 * dans format_line_content interroge cette table pour reecrire les usages
 * vers la casse de declaration. Distinct de var_types (qui mixe globals et
 * locals) : on n'enregistre QUE le top-level via tracking de depth.
 *
 * NB : les noms de classe / structure / enum sont deja capturees comme
 * user_types par user_symbols_collect — pas besoin de les re-harvester ici. */
static void user_globals_collect(const char* src) {
    const char* p = src;
    int depth = 0;          /* depth des blocs ouverts (non-CONSTANT) */
    bool inConstBlock = false;  /* on est dans un body CONSTANT bloc top-level */

    while (*p) {
        /* Skip leading whitespace de la ligne. */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n') { p++; continue; }
        if (*p == '\0') break;
        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }

        if (!fmt_is_word_char(*p)) {
            while (*p && *p != '\n') p++;
            continue;
        }
        const char* w = p;
        while (fmt_is_word_char(*p)) p++;
        int wlen = (int)(p - w);

        /* Normalise pour test keyword (lowercase ASCII, strip accents). */
        char kw[16];
        int kl = scanner_strip_accents(w, wlen, kw, sizeof(kw));
        if (kl > 0 && kl < (int)sizeof(kw)) {
            for (int i = 0; i < kl; i++) kw[i] = (char)tolower((unsigned char)kw[i]);
            kw[kl] = '\0';
        } else { kw[0] = '\0'; }

        bool isFin = (strcmp(kw, "fin") == 0 || strcmp(kw, "end") == 0);
        if (isFin) {
            if (inConstBlock) inConstBlock = false;
            else if (depth > 0) depth--;
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Body d'un block CONSTANT[E] top-level : LHS = nom de constante. */
        if (inConstBlock && depth == 0) {
            const char* save = p;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '=') {
                user_symbols_add(g_fmt_st->user_globals, &g_fmt_st->user_globals_count, 512, w, wlen);
            }
            p = save;
            while (*p && *p != '\n') p++;
            continue;
        }

        /* PROCEDURE / FONCTION top-level : capture le nom apres le keyword.
         * `PROCEDURE Foo(...)` -> ajoute "Foo" comme global. Le body sera
         * traverse avec depth>0, donc ses locals n'iront PAS dans user_globals. */
        if (depth == 0 && (strcmp(kw, "procedure") == 0 ||
                           strcmp(kw, "fonction") == 0 ||
                           strcmp(kw, "function") == 0)) {
            while (*p == ' ' || *p == '\t') p++;
            /* Skip `ABSTRAITE`/`PRIVEE`/`PUBLIQUE`/... modifier pour atteindre
             * le nom — on consomme un seul mot s'il est suivi d'un autre mot. */
            if (fmt_is_word_char(*p)) {
                const char* nw1 = p;
                while (fmt_is_word_char(*p)) p++;
                int nw1len = (int)(p - nw1);
                /* Lookahead : suivi d'un autre word ? */
                const char* save = p;
                while (*p == ' ' || *p == '\t') p++;
                if (fmt_is_word_char(*p)) {
                    /* nw1 etait un modifier (PRIVEE/ABSTRAITE/...) — le nom
                     * de procedure est le prochain word. */
                    const char* nw2 = p;
                    while (fmt_is_word_char(*p)) p++;
                    int nw2len = (int)(p - nw2);
                    user_symbols_add(g_fmt_st->user_globals, &g_fmt_st->user_globals_count, 512,
                                     nw2, nw2len);
                    user_symbols_add(g_fmt_st->user_procs, &g_fmt_st->user_procs_count, 512,
                                     nw2, nw2len);
                } else {
                    /* nw1 etait directement le nom. */
                    p = save;
                    user_symbols_add(g_fmt_st->user_globals, &g_fmt_st->user_globals_count, 512,
                                     nw1, nw1len);
                    user_symbols_add(g_fmt_st->user_procs, &g_fmt_st->user_procs_count, 512,
                                     nw1, nw1len);
                }
            }
            depth++;
            while (*p && *p != '\n') p++;
            continue;
        }

        /* CONSTANT[E] ouvrant inline ou bloc, top-level. */
        if (depth == 0 && (strcmp(kw, "constante") == 0 || strcmp(kw, "constant") == 0)) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '\n' || *p == '\r' ||
                (p[0] == '/' && p[1] == '/')) {
                inConstBlock = true;
                while (*p && *p != '\n') p++;
                continue;
            }
            /* Forme inline : CONSTANT NAME = literal. */
            if (fmt_is_word_char(*p)) {
                const char* nw = p;
                while (fmt_is_word_char(*p)) p++;
                int nwlen = (int)(p - nw);
                const char* save = p;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '=') {
                    user_symbols_add(g_fmt_st->user_globals, &g_fmt_st->user_globals_count, 512,
                                     nw, nwlen);
                }
                p = save;
            }
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Autre opener structurel top-level (CLASSE/STRUCTURE/ENUMERATION/
         * INTERFACE/...): incremente depth sans rien harvester (le nom du
         * type est deja dans user_types). */
        if (depth == 0 && word_opens_block_here(w, wlen)) {
            depth++;
            while (*p && *p != '\n') p++;
            continue;
        }
        if (depth > 0) {
            if (word_opens_block_here(w, wlen)) depth++;
            while (*p && *p != '\n') p++;
            continue;
        }

        /* Top-level (depth == 0) : tentative de matcher `IDENT est ...` —
         * declaration de variable globale (avec ou sans = init).
         * Pattern : <w> est [un|une] <type> [= literal]. On enregistre w. */
        const char* save = p;
        while (*p == ' ' || *p == '\t') p++;
        if (fmt_is_word_char(*p)) {
            const char* nw = p;
            while (fmt_is_word_char(*p)) p++;
            int nwlen = (int)(p - nw);
            if (fmt_is_copule(nw, nwlen)) {
                user_symbols_add(g_fmt_st->user_globals, &g_fmt_st->user_globals_count, 512,
                                 w, wlen);
            }
        }
        p = save;
        while (*p && *p != '\n') p++;
    }
}

/* Case- AND accent-insensitive identifier equality. Used so a usage typed
 * without accents (`donnees`) matches its accented declaration (`données`),
 * mirroring the runtime's accent-insensitive identifier resolution. */
static bool ident_equal_ci_accent(const char* a, int aLen,
                                   const char* b, int bLen) {
    char sa[128], sb[128];
    int la = scanner_strip_accents(a, aLen, sa, sizeof(sa));
    int lb = scanner_strip_accents(b, bLen, sb, sizeof(sb));
    if (la <= 0 || lb <= 0 || la != lb) return false;
    for (int i = 0; i < la; i++) {
        if (tolower((unsigned char)sa[i]) != tolower((unsigned char)sb[i]))
            return false;
    }
    return true;
}

/*  : lookup accent-insensible dans une table user. Renvoie la graphie
 * de declaration si elle differe de la forme passee (en bytes), NULL si deja
 * identique ou inconnue. Le caller emet verbatim quand NULL. */
static const char* find_user_canonical_ci_accent(FmtUserName* table, int count,
                                                  const char* word, int len) {
    if (len <= 0 || len >= 64) return NULL;
    for (int i = 0; i < count; i++) {
        if (ident_equal_ci_accent(table[i].name, table[i].len, word, len)) {
            if (table[i].len != len || memcmp(table[i].name, word, len) != 0)
                return table[i].name;
            return NULL; /* deja canon byte-pour-byte */
        }
    }
    return NULL;
}

/* User enum member canon. Accent + case-insensitive match so a
 * usage typed without accents (`valide`) follows its accented declaration
 * (`validé`), mirroring the runtime. Returns the declaration casing, or NULL
 * if unknown / already canonical (caller emits verbatim either way). */
static bool enum_member_known(const char* word, int len) {
    if (len <= 0 || len >= 64) return false;
    for (int i = 0; i < g_fmt_st->enum_members_count; i++) {
        if (ident_equal_ci_accent(g_fmt_st->enum_members[i].name, g_fmt_st->enum_members[i].len,
                                  word, len)) return true;
    }
    return false;
}

static const char* find_enum_member_canonical(const char* word, int len) {
    if (len <= 0 || len >= 64) return NULL;
    for (int i = 0; i < g_fmt_st->enum_members_count; i++) {
        if (ident_equal_ci_accent(g_fmt_st->enum_members[i].name, g_fmt_st->enum_members[i].len,
                                  word, len)) {
            if (g_fmt_st->enum_members[i].len != len ||
                memcmp(g_fmt_st->enum_members[i].name, word, len) != 0)
                return g_fmt_st->enum_members[i].name;
            return NULL; /* already canonical */
        }
    }
    return NULL;
}

static void scoped_name_add(const char* name, int len, int scopeId) {
    if (g_fmt_st->scoped_names_count >= 1024 || len <= 0 || len >= 64) return;
    /* Dedup within the same scope (first decl is canonical). */
    for (int i = 0; i < g_fmt_st->scoped_names_count; i++) {
        if (g_fmt_st->scoped_names[i].scopeId == scopeId && g_fmt_st->scoped_names[i].len == len &&
            strncasecmp(g_fmt_st->scoped_names[i].name, name, len) == 0) return;
    }
    FmtScopedName* s = &g_fmt_st->scoped_names[g_fmt_st->scoped_names_count++];
    memcpy(s->name, name, len); s->name[len] = '\0';
    s->len = len; s->scopeId = scopeId;
}

/* Pre-scan: delimit each PROCEDURE/FONCTION body, harvest its parameters
 * (from the signature parentheses) and its local declarations (`X est ...`
 * lines inside the body). Skips strings, comments and class bodies (a class
 * may contain methods — handled by user_symbols_collect for members; here we
 * only care about top-level procedures so locals/params don't leak). */
static void user_scopes_collect(const char* src) {
    const char* p = src;
    while (*p) {
        if (*p == '"') {
            fmt_skip_string_literal(&p, NULL, false); continue;
        }
        if (*p == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (!fmt_is_word_char(*p)) { p++; continue; }
        const char* wStart = p;
        while (fmt_is_word_char(*p)) p++;
        int wLen = (int)(p - wStart);

        char kw[16];
        int kl = scanner_strip_accents(wStart, wLen, kw, sizeof(kw));
        if (kl > 0 && kl < (int)sizeof(kw)) {
            for (int i = 0; i < kl; i++) kw[i] = (char)tolower((unsigned char)kw[i]);
            kw[kl] = '\0';
        } else { kw[0] = '\0'; }

        bool isProc = (strcmp(kw, "procedure") == 0 || strcmp(kw, "fonction") == 0 ||
                       strcmp(kw, "function") == 0);
        if (!isProc) continue;

        if (g_fmt_st->user_scopes_count >= 256) break;
        int scopeId = g_fmt_st->user_scopes_count;
        FmtScope* sc = &g_fmt_st->user_scopes[g_fmt_st->user_scopes_count++];

        /* Parse parameters: scan to '(' then collect each `name est ...`
         * until the matching ')'. */
        const char* q = p;
        while (*q && *q != '(' && *q != '\n') q++;
        /*  bug (a) : le scope demarre a la signature (`(`), pas au
         * keyword PROCEDURE/FONCTION. Sinon le nom de procedure tombe dans
         * la canonisation scoped et collisionne case-insensitive avec ses
         * propres params (`PROCEDURE C(c est ...)` se corrompt en
         * `PROCEDURE c(C est ...)`). Le nom de procedure est un global
         * gere par user_globals, pas un local. */
        sc->startOff = (int)((*q == '(' ? q : p) - src);
        if (*q == '(') {
            q++;
            int paren = 1;
            while (*q && paren > 0) {
                if (*q == '(') { paren++; q++; continue; }
                if (*q == ')') { paren--; q++; continue; }
                if (*q == '"') { fmt_skip_string_literal(&q, NULL, false); continue; }
                if (!fmt_is_word_char(*q)) { q++; continue; }
                const char* pn = q;
                while (fmt_is_word_char(*q)) q++;
                int pnLen = (int)(q - pn);
                /* Followed by `est` => it's a parameter name. */
                const char* r = q;
                while (*r == ' ' || *r == '\t') r++;
                if (fmt_is_word_char(*r)) {
                    const char* kwp = r;
                    while (fmt_is_word_char(*r)) r++;
                    int kwpLen = (int)(r - kwp);
                    if (fmt_is_copule(kwp, kwpLen)) {
                        scoped_name_add(pn, pnLen, scopeId);
                    }
                }
                /* Skip to the next comma or ')' so the type token isn't
                 * mistaken for another parameter name. */
                while (*q && *q != ',' && *q != ')' && *q != '(') {
                    if (*q == '"') { fmt_skip_string_literal(&q, NULL, false); continue; }
                    q++;
                }
                if (*q == ',') q++;
            }
        }
        p = q;
        /* Walk the body collecting `name est ...` locals until matching FIN. */
        int depth = 1;
        while (*p && depth > 0) {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
            if (*p == '"') { fmt_skip_string_literal(&p, NULL, false); continue; }
            if (!fmt_is_word_char(*p)) { p++; continue; }
            const char* tStart = p;
            while (fmt_is_word_char(*p)) p++;
            int tLen = (int)(p - tStart);

            char tk[16];
            int tkl = scanner_strip_accents(tStart, tLen, tk, sizeof(tk));
            if (tkl > 0 && tkl < (int)sizeof(tk)) {
                for (int i = 0; i < tkl; i++) tk[i] = (char)tolower((unsigned char)tk[i]);
                tk[tkl] = '\0';
            } else { tk[0] = '\0'; }

            if (strcmp(tk, "fin") == 0 || strcmp(tk, "end") == 0) {
                depth--;
                if (depth == 0) { sc->endOff = (int)(p - src); }
                while (*p && *p != '\n') p++;
                continue;
            }
            /* Nested block opener — track depth so a FIN doesn't close the
             * procedure prematurely. */
            if (word_opens_block_here(tStart, tLen)) {
                depth++;
                while (*p && *p != '\n') p++;
                continue;
            }
            /* `name est ...` => local declaration. */
            const char* r = p;
            while (*r == ' ' || *r == '\t') r++;
            if (fmt_is_word_char(*r)) {
                const char* kw2 = r;
                while (fmt_is_word_char(*r)) r++;
                int kw2Len = (int)(r - kw2);
                if (fmt_is_copule(kw2, kw2Len)) {
                    scoped_name_add(tStart, tLen, scopeId);
                }
            }
            while (*p && *p != '\n') p++;
        }
        if (depth > 0) sc->endOff = (int)(p - src); /* unterminated: to EOF */
    }
}

/*  bug (a) : vrai ssi `word` est revendique comme local/param par
 * le scope qui contient `offset` (match case-insensitive). Utilise pour
 * empecher la branche user_globals de re-canoniser un local dont la canon
 * est deja correcte mais qui collisionne case-insensitive avec un global
 * homonyme (typiquement le nom de la procedure elle-meme). */
static bool scoped_name_known(const char* word, int len, int offset) {
    if (len <= 0 || len >= 64) return false;
    int scopeId = -1;
    for (int i = 0; i < g_fmt_st->user_scopes_count; i++) {
        if (offset >= g_fmt_st->user_scopes[i].startOff &&
            offset < g_fmt_st->user_scopes[i].endOff) {
            scopeId = i;
        }
    }
    if (scopeId < 0) return false;
    for (int i = 0; i < g_fmt_st->scoped_names_count; i++) {
        if (g_fmt_st->scoped_names[i].scopeId == scopeId &&
            ident_equal_ci_accent(g_fmt_st->scoped_names[i].name,
                                  g_fmt_st->scoped_names[i].len,
                                  word, len)) return true;
    }
    return false;
}

/* Canonical casing for a local/param at byte `offset`, or NULL if unknown /
 * already canonical. Scope-bounded: only declarations of the enclosing
 * procedure are considered. */
static const char* find_scoped_canonical(const char* word, int len,
                                          int offset) {
    if (len <= 0 || len >= 64) return NULL;
    int scopeId = -1;
    for (int i = 0; i < g_fmt_st->user_scopes_count; i++) {
        if (offset >= g_fmt_st->user_scopes[i].startOff && offset < g_fmt_st->user_scopes[i].endOff) {
            scopeId = i; /* keep innermost (last) match */
        }
    }
    if (scopeId < 0) return NULL;
    for (int i = 0; i < g_fmt_st->scoped_names_count; i++) {
        if (g_fmt_st->scoped_names[i].scopeId == scopeId &&
            ident_equal_ci_accent(g_fmt_st->scoped_names[i].name, g_fmt_st->scoped_names[i].len,
                                  word, len)) {
            if (g_fmt_st->scoped_names[i].len != len ||
                memcmp(g_fmt_st->scoped_names[i].name, word, len) != 0)
                return g_fmt_st->scoped_names[i].name;
            return NULL; /* already canonical */
        }
    }
    return NULL;
}

/* Canonical casing for `member` accessed on a receiver of class `cls`
 * (`cls` may be a variable's declared type, or the class name itself).
 * Returns NULL if unknown / already canonical. */
static bool class_member_exists(const char* cls, int clsLen,
                                const char* member, int len) {
    if (len <= 0 || len >= 64 || clsLen <= 0 || clsLen >= 64) return false;
    for (int i = 0; i < g_fmt_st->class_members_count; i++) {
        if (ident_equal_ci_accent(g_fmt_st->class_members[i].cls, g_fmt_st->class_members[i].clsLen,
                                  cls, clsLen) &&
            ident_equal_ci_accent(g_fmt_st->class_members[i].name, g_fmt_st->class_members[i].len,
                                  member, len)) return true;
    }
    return false;
}

static const char* find_class_member_canonical(const char* cls, int clsLen,
                                                const char* member, int len) {
    if (len <= 0 || len >= 64 || clsLen <= 0 || clsLen >= 64) return NULL;
    for (int i = 0; i < g_fmt_st->class_members_count; i++) {
        if (ident_equal_ci_accent(g_fmt_st->class_members[i].cls, g_fmt_st->class_members[i].clsLen,
                                  cls, clsLen) &&
            ident_equal_ci_accent(g_fmt_st->class_members[i].name, g_fmt_st->class_members[i].len,
                                  member, len)) {
            if (g_fmt_st->class_members[i].len != len ||
                memcmp(g_fmt_st->class_members[i].name, member, len) != 0)
                return g_fmt_st->class_members[i].name;
            return NULL; /* already canonical */
        }
    }
    return NULL;
}

/*  : helpers any-class, sans owner connu. Utilises pour proteger
 * un identifier standalone homonyme d'une native quand le nom est declare
 * comme membre d'une classe user quelconque (par symetrie avec
 * user_name_known sur user_fields). On ne se soucie pas du owner ici —
 * il s'agit juste de savoir si le nom est revendique par l'utilisateur
 * dans au moins une classe locale. */
static bool class_member_exists_any_class(const char* member, int len) {
    if (len <= 0 || len >= 64) return false;
    for (int i = 0; i < g_fmt_st->class_members_count; i++) {
        if (g_fmt_st->class_members[i].ctxRegistryHomonym) continue;  /*  */
        if (ident_equal_ci_accent(g_fmt_st->class_members[i].name,
                                  g_fmt_st->class_members[i].len,
                                  member, len)) return true;
    }
    return false;
}

static const char* find_class_member_canonical_any_class(const char* member, int len) {
    if (len <= 0 || len >= 64) return NULL;
    for (int i = 0; i < g_fmt_st->class_members_count; i++) {
        if (g_fmt_st->class_members[i].ctxRegistryHomonym) continue;  /*  */
        if (ident_equal_ci_accent(g_fmt_st->class_members[i].name,
                                  g_fmt_st->class_members[i].len,
                                  member, len)) {
            if (g_fmt_st->class_members[i].len != len ||
                memcmp(g_fmt_st->class_members[i].name, member, len) != 0)
                return g_fmt_st->class_members[i].name;
            return NULL; /* already canonical */
        }
    }
    return NULL;
}

/*  : type declare du membre `member` de la classe `cls`, pour
 * resoudre la chaine imbriquee a.b.c (le type du membre b devient la classe
 * receveuse pour c). Retourne NULL si inconnu ou sans type (const/enum). */
static const char* find_class_member_decl_type(const char* cls, int clsLen,
                                               const char* member, int len,
                                               int* outLen) {
    if (len <= 0 || len >= 64 || clsLen <= 0 || clsLen >= 64) return NULL;
    for (int i = 0; i < g_fmt_st->class_members_count; i++) {
        if (ident_equal_ci_accent(g_fmt_st->class_members[i].cls, g_fmt_st->class_members[i].clsLen,
                                  cls, clsLen) &&
            ident_equal_ci_accent(g_fmt_st->class_members[i].name, g_fmt_st->class_members[i].len,
                                  member, len)) {
            if (g_fmt_st->class_members[i].declTypeLen > 0) {
                *outLen = g_fmt_st->class_members[i].declTypeLen;
                return g_fmt_st->class_members[i].declType;
            }
            return NULL;
        }
    }
    return NULL;
}

/* Declared type name of variable `var`, or NULL. Used to resolve a class
 * receiver to its class so member access can be canonised. */
static const char* find_var_type_name(const char* var, int varLen, int* outLen) {
    for (int i = 0; i < g_fmt_st->var_types_count; i++) {
        if (g_fmt_st->var_types[i].varLen == varLen &&
            strncasecmp(g_fmt_st->var_types[i].var, var, varLen) == 0) {
            *outLen = g_fmt_st->var_types[i].typeLen;
            return g_fmt_st->var_types[i].type;
        }
    }
    return NULL;
}

/* Modificateur de visibilite inline (« PROCÉDURE PRIVÉE / PROTÉGÉE foo() ») :
 * accord feminin avec « procédure ». Distinct du marqueur de bloc standalone
 * (« PRIVÉ » / « PROTÉGÉ » colonne 0). Prend le normBuf accent-strip lowercase.
 * Retourne la forme feminine accentuee, ou NULL si le mot n'est pas un
 * marqueur de visibilite privee/protegee. */
static const char* fmt_visibility_inline_canon(const char* norm, int len) {
    /*  : l'accord feminin est une regle FR. La cible EN rend la forme
     * invariable ; le retour FR restaure le feminin. Hors translate, la forme
     * ecrite decide (une methode ecrite en EN reste en EN). */
    if ((len == 5 && strcmp(norm, "prive") == 0) ||
        (len == 6 && strcmp(norm, "privee") == 0)) {
        return fmt_pick_lang("PRIV\xc3\x89""E", "PRIV\xc3\x89""E", "PRIVATE");
    }
    if (len == 7 && strcmp(norm, "private") == 0) {
        return fmt_pick_lang("PRIVATE", "PRIV\xc3\x89""E", "PRIVATE");
    }
    if ((len == 7 && strcmp(norm, "protege") == 0) ||
        (len == 8 && strcmp(norm, "protegee") == 0)) {
        return fmt_pick_lang("PROT\xc3\x89G\xc3\x89""E",
                             "PROT\xc3\x89G\xc3\x89""E", "PROTECTED");
    }
    if (len == 9 && strcmp(norm, "protected") == 0) {
        return fmt_pick_lang("PROTECTED", "PROT\xc3\x89G\xc3\x89""E", "PROTECTED");
    }
    /*  : symetrie avec prive/protege — PROCEDURE PUBLIQUE conserve
     * la forme feminine. Sans ce branch, `publique` tombait sur le keyword
     * standalone `PUBLIC` (table fmt_keywords), ce qui produit la regression
     * `PROCEDURE PUBLIQUE` -> `PROCEDURE PUBLIC` revelee par l'oracle classe. */
    if ((len == 6 && strcmp(norm, "public") == 0) ||
        (len == 8 && strcmp(norm, "publique") == 0)) {
        /*  : « PUBLIC » n'a pas de forme feminine en EN. */
        return fmt_pick_lang("PUBLIQUE", "PUBLIQUE", "PUBLIC");
    }
    return NULL;
}

/* Case-insensitive match */
static const FmtKeyword* find_keyword(const char* word, int len) {
    for (int i = 0; fmt_keywords[i].normalized != NULL; i++) {
        int klen = (int)strlen(fmt_keywords[i].normalized);
        if (klen != len) continue;
        bool match = true;
        for (int j = 0; j < len; j++) {
            if (tolower((unsigned char)word[j]) !=
                (unsigned char)fmt_keywords[i].normalized[j]) {
                match = false;
                break;
            }
        }
        if (match) return &fmt_keywords[i];
    }
    return NULL;
}

/* ================================================================
 * INDENTATION RULES
 *
 * We detect the FIRST keyword on each line to decide indent changes.
 * ================================================================ */

static bool is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_' ||
           ((unsigned char)c >= 0x80); /* UTF-8 continuation/lead bytes */
}

/*  — attributs de declaration `<cle[=valeur]>` : nullable,
 * indetectable, utile/useful, Serialise, agrandissement. Le formatter est le
 * gatekeeper canonique (invariant 4) : il restaure la casse ET les accents de
 * la CLE (forme registre), tout en PRESERVANT la valeur litterale (`=Faux`,
 * `="rename"`). Pas de flip FR<->EN : chaque forme se canonise vers elle-meme
 * (utile/useful distincts ; les quatre autres n'ont qu'une forme). Logique
 * pure (scanner_canonicalize_identifier), donc identique dans owl.exe et
 * owl-lsp -> parite dual-backend par construction (aucune entree fmt_keywords[],
 * qui s'appliquerait hors position et reecrirait un homonyme du flux). */
static const char* fmt_decl_attr_canon(const char* w, int wlen) {
    char norm[64];
    int nl = scanner_canonicalize_identifier(w, wlen, norm, sizeof norm);
    if (nl == 8  && memcmp(norm, "nullable", 8) == 0)        return "nullable";
    if (nl == 12 && memcmp(norm, "indetectable", 12) == 0)   return "ind\xc3\xa9tectable";
    if (nl == 5  && memcmp(norm, "utile", 5) == 0)           return "utile";
    if (nl == 6  && memcmp(norm, "useful", 6) == 0)          return "useful";
    if (nl == 9  && memcmp(norm, "serialise", 9) == 0)       return "S\xc3\xa9rialise";
    if (nl == 14 && memcmp(norm, "agrandissement", 14) == 0) return "agrandissement";
    return NULL;
}

/* `end` NULL = borne sur '\0' (chaine C, pass format_line_content) ;
 * sinon borne stricte `s < end` (buffer non termine, pass emit_normalized_line). */
static bool fmt_attr_at_end(const char* s, const char* end) {
    return end ? (s >= end) : (*s == '\0');
}

/* Position-gate : `s` pointe juste apres un `<`. Confirme une ouverture
 * d'attribut de declaration ([ws] CLE-connue [ws] (`>` | `=` | `,`)).
 * Rejette tout comparateur `n < 10` / generique `tableau<entier>`. */
static bool fmt_peek_decl_attr(const char* s, const char* end) {
    while (!fmt_attr_at_end(s, end) && (*s == ' ' || *s == '\t')) s++;
    const char* ks = s;
    while (!fmt_attr_at_end(s, end) && is_word_char(*s)) s++;
    int klen = (int)(s - ks);
    if (klen == 0 || fmt_decl_attr_canon(ks, klen) == NULL) return false;
    while (!fmt_attr_at_end(s, end) && (*s == ' ' || *s == '\t')) s++;
    if (fmt_attr_at_end(s, end)) return false;
    return *s == '>' || *s == '=' || *s == ',';
}

/* Emet le corps d'attribut depuis `s` (juste apres `<`) jusqu'au `>` inclus.
 * CLE(s) canonisee(s), valeurs `"..."` preservees VERBATIM (jamais squeezees
 * ni canonisees — litteral utilisateur), whitespace hors chaine squeeze ->
 * forme compacte `<cle=valeur>`. Retourne le pointeur juste apres le `>`. */
static const char* fmt_emit_attr_body(Buffer* out, const char* s, const char* end) {
    bool expectKey = true;   /* faux apres un `=` ; vrai a nouveau apres `,` */
    while (!fmt_attr_at_end(s, end) && *s != '>' && *s != '\n') {
        char c = *s;
        if (c == ' ' || c == '\t' || c == '\r') { s++; continue; }
        if (c == '"') {
            buf_putc(out, '"'); s++;
            while (!fmt_attr_at_end(s, end) && *s != '"' && *s != '\n')
                buf_putc(out, *s++);
            if (!fmt_attr_at_end(s, end) && *s == '"') { buf_putc(out, '"'); s++; }
            expectKey = false;
            continue;
        }
        if (c == '=') { buf_putc(out, '='); s++; expectKey = false; continue; }
        if (c == ',') { buf_putc(out, ','); s++; expectKey = true;  continue; }
        if (is_word_char(c)) {
            const char* ws = s;
            while (!fmt_attr_at_end(s, end) && is_word_char(*s)) s++;
            int wl = (int)(s - ws);
            const char* canon = expectKey ? fmt_decl_attr_canon(ws, wl) : NULL;
            if (canon) buf_puts(out, canon);
            else       buf_write(out, ws, wl);
            expectKey = false;
            continue;
        }
        buf_putc(out, c); s++;
    }
    if (!fmt_attr_at_end(s, end) && *s == '>') { buf_putc(out, '>'); s++; }
    return s;
}

/*  — separateur de borne DANS un slice `s[[i À j]]`. Le slice partage
 * le token TOKEN_A de la borne de plage (compiler.c::string_slice) mais HORS
 * contexte POUR/CAS, donc la canonisation amont (seenPourEquals/seenCas) ne le
 * touche pas et le contenu du `[[ ]]` etait copie verbatim. A l'interieur d'un
 * slice, un `a`/`to`/`à`/`À` isole est TOUJOURS le separateur : `a`/`to` sont
 * reserves (scanner TOKEN_A) et ne peuvent pas nommer un index — aucune
 * ambiguite avec un identifiant. Retourne 1 pour la forme FR (a/à/À), 2 pour
 * la forme EN (to), 0 sinon (index, `SUR`, tout le reste = verbatim). */
static int fmt_slice_range_sep(const char* w, int len) {
    if (len == 1) {
        if (w[0] == 'a' || w[0] == 'A') return 1;
    } else if (len == 2) {
        /* à/À en UTF-8 : 0xC3 0xA0 (à) / 0xC3 0x80 (À). */
        if ((unsigned char)w[0] == 0xC3 &&
            ((unsigned char)w[1] == 0xA0 || (unsigned char)w[1] == 0x80))
            return 1;
        if ((w[0] == 't' || w[0] == 'T') && (w[1] == 'o' || w[1] == 'O'))
            return 2;
    }
    return 0;
}


/* Check if a word is a block-opening keyword. Strip les accents (et lower)
 * pour matcher les forms 'Fenêtre' / 'Libellé' / etc. saisies par le user. */
static bool word_opens_block(const char* word, int len) {
    /* Openers non-DSL : keywords scanner + sous-blocs DSL Etat statiques.
     * Les widgets DSL eux-memes sont resolus dynamiquement via
     * owl_control_kinds_table() pour eviter la duplication avec
     * src/ui_decl.c::CONTROL_KINDS. */
    const char* static_openers[] = {
        "si", "if", "pour", "for", "tantque", "while",
        "boucle", "loop", "selon", "switch", "avec", "with",
        "procedure", "fonction", "function",
        "classe", "class", "interface", "structure", "enumeration",
        "combinaison", "combination",   /*  — forme EN opener aussi */
        /*  — `constant` et `constante` sont synonymes canon
         *. Tous deux peuvent ouvrir un bloc multi-constantes ;
         * la forme inline `CONSTANT NAME = literal` est filtree par
         * is_constant_inline_line() en aval. */
        "constante",
        "constant",
        "quand", "when",
        /* DSL Fenetre v2 : bloc statique des proprietes. */
        "proprietes", "properties",
        /*  — bloc `<Nom> est un Thème` : catalogue de styles nommes.
         * Opener top-level uniquement (cf. word_is_toplevel_only_dsl) et non
         * suivi de `=`/`.` (cf. dsl_word_really_opens), pour ne pas confondre
         * avec une variable locale ou un acces `Theme.<style>`. */
        "theme",
        /*  — bloc `<Nom> est une CollectionDeProcédures` : corps
         * indente, FIN aligne sur l'ouverture. Opener top-level uniquement
         * (cf. word_is_toplevel_only_dsl). */
        "collectiondeprocedures",
        "procedurecollection",
        NULL
    };
    char ascii[64];
    int aLen = scanner_strip_accents(word, len, ascii, sizeof(ascii));
    if (aLen <= 0 || aLen >= (int)sizeof(ascii)) return false;
    char lower[64];
    for (int i = 0; i < aLen; i++) lower[i] = (char)tolower((unsigned char)ascii[i]);
    lower[aLen] = '\0';

    for (int i = 0; static_openers[i]; i++) {
        if (strcmp(lower, static_openers[i]) == 0) return true;
    }
    /* Widget kinds + sous-blocs DSL : iteration de CONTROL_KINDS. Tout
     * widget ajoute dans src/ui_decl.c::CONTROL_KINDS apparait ici sans
     * edit du formatter. Inclut les sous-blocs DSL (is_widget=false) car
     * ils ouvrent aussi un block (Etat, Corps, ...). */
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        const char* names[3] = { k->nameFR, k->nameAscii, k->nameEN };
        for (int n = 0; n < 3; n++) {
            if (!names[n]) continue;
            char nbuf[64];
            int nlen = scanner_strip_accents(names[n], (int)strlen(names[n]),
                                              nbuf, sizeof(nbuf));
            if (nlen <= 0 || nlen != aLen) continue;
            bool eq = true;
            for (int i = 0; i < nlen; i++) {
                if (lower[i] != (char)tolower((unsigned char)nbuf[i])) { eq = false; break; }
            }
            if (eq) return true;
        }
    }
    return false;
}

/*  — mots-cles de controle qui n'ouvrent un bloc QUE depuis le debut
 * de leur ligne. Contrairement aux genres de widget/type (`X est un Bouton`,
 * ouverture legitime en 2e position apres la copule), ces keywords en position
 * non-initiale sont des identifiants homonymes : `Trace(si)`, `Tableau(pour)`.
 * word_opens_block les reconnait sans distinguer la position ; un `si` passe
 * comme argument y ouvrait alors un niveau jamais referme, sur-indentant tout
 * le reste de la ligne / du fichier. `SINON SI` n'est pas concerne : le `SI`
 * en 2e position y est deja neutralise par la logique mid-block (une seule FIN
 * partagee). L'input est de toute facon rejete au compile (mot reserve), mais
 * le formateur tourne avant et ne doit pas diverger. */
static bool is_control_flow_opener(const char* word, int len) {
    char norm[16];
    int n = scanner_strip_accents(word, len, norm, sizeof norm);
    if (n <= 0 || n >= (int)sizeof norm) return false;
    for (int i = 0; i < n; i++) norm[i] = (char)tolower((unsigned char)norm[i]);
    norm[n] = '\0';
    return strcmp(norm, "si") == 0      || strcmp(norm, "if") == 0
        || strcmp(norm, "pour") == 0    || strcmp(norm, "for") == 0
        || strcmp(norm, "tantque") == 0 || strcmp(norm, "while") == 0
        || strcmp(norm, "boucle") == 0  || strcmp(norm, "loop") == 0
        || strcmp(norm, "selon") == 0   || strcmp(norm, "switch") == 0
        || strcmp(norm, "avec") == 0    || strcmp(norm, "with") == 0;
}

/* Peek le prochain mot significatif a partir de `afterWord` (skip
 * whitespace + newlines + line comments) et retourne true si c'est
 * `PROPRIETES`/`PROPERTIES`. Partage entre l'heuristique des types
 * ambigus (Image/Liste, dsl_word_really_opens) et la detection des
 * types de widget INCONNUS suivis d'un bloc PROPRIETES. */
static bool peek_word_is_proprietes(const char* afterWord) {
    const char* peek = afterWord;
    while (*peek) {
        if (*peek == ' ' || *peek == '\t'
            || *peek == '\n' || *peek == '\r') { peek++; continue; }
        /* Skip line comment '// ...' */
        if (peek[0] == '/' && peek[1] == '/') {
            while (*peek && *peek != '\n') peek++;
            continue;
        }
        break;
    }
    if (!*peek) return false;
    if (!is_word_char(*peek)) return false;
    const char* pw = peek;
    while (is_word_char(*peek)) peek++;
    int pwLen = (int)(peek - pw);
    char pAscii[16];
    int paLen = scanner_strip_accents(pw, pwLen, pAscii, sizeof(pAscii));
    if (paLen <= 0 || paLen >= (int)sizeof(pAscii)) return false;
    char pLow[16];
    for (int i = 0; i < paLen; i++)
        pLow[i] = (char)tolower((unsigned char)pAscii[i]);
    pLow[paLen] = '\0';
    return strcmp(pLow, "proprietes") == 0
        || strcmp(pLow, "properties") == 0;
}

/* Distingue 'Libelle lblX' (declaration → ouvre block) de 'Libelle = "..."'
 * (propriete → pas de block). Retourne true si le mot DSL ouvre vraiment
 * un block. Pour les non-DSL keywords, retourne true (comportement
 * inchange via word_opens_block). */
static bool dsl_word_really_opens(const char* word, int len, const char* afterWord) {
    /* Test si word est un keyword DSL ambigu. Strip accents pour gerer
     * 'Libellé' (avec accent) aussi bien que 'Libelle'. */
    char ascii[16];
    int aLen = scanner_strip_accents(word, len, ascii, sizeof(ascii));
    if (aLen <= 0 || aLen >= (int)sizeof(ascii)) return true;
    char lower[16];
    for (int i = 0; i < aLen; i++) lower[i] = (char)tolower((unsigned char)ascii[i]);
    lower[aLen] = '\0';
    /*  — deux familles d'ambiguite distinctes :
     *
     * (a) ambigu SEULEMENT pour le nom de propriete : `Libelle = "..."`,
     *     `Bouton = ...` dans un bloc PROPRIETES. Le mot n'a AUCUN sens
     *     de type/variable — il ne sert de nom de propriete que suivi de
     *     `=`. Hors `=`, c'est forcement une declaration de widget qui
     *     ouvre un bloc, comme un widget non-ambigu (Combo/Table). Pas de
     *     fallback peek-PROPRIETES : le compilo garde la regle PROPRIETES
     *     obligatoire, aligner le FIN sans PROPRIETES ne masque rien et
     *     corrige l'ergonomie d'edition. */
    bool isPropNameAmbiguous = strcmp(lower, "libelle") == 0 || strcmp(lower, "label") == 0
                            || strcmp(lower, "bouton")  == 0 || strcmp(lower, "button") == 0
                            || strcmp(lower, "saisie")  == 0 || strcmp(lower, "input") == 0;
    /* (b) ambigu comme TYPE/variable : `imgQ est une Image = dCreeImage(...)`,
     *     `img est une Image` variable simple vs `IMG_X est une Image` widget.
     *     Vrai double sens variable/type → besoin du peek-PROPRIETES pour
     *     trancher quand le mot est nu. */
    /*  — `CodeBarres`/`BarCodes` : meme double sens que `Image`.
     * Type compose first-class (`cb est un CodeBarres` = variable) ET widget
     * (`CBA est un Champ CodeBarres` = controle). Nu, le peek-PROPRIETES
     * tranche : suivi de PROPRIETES → widget (ouvre block), sinon variable. */
    bool isTypeAmbiguous = strcmp(lower, "image") == 0
                        || strcmp(lower, "codebarres") == 0
                        || strcmp(lower, "barcodes") == 0;
    /* Liste : ambigu car aussi un type collection (`mL est une Liste de
     * chaine`). On distingue : Champ Liste (widget) ouvre un block, et
     * 'Liste' suivi de 'de' (mot-cle DE de collection) ne ouvre pas. */
    bool isListeAmbiguous = strcmp(lower, "liste") == 0
                         || strcmp(lower, "list")  == 0;
    /*  — `etat`/`report` : mot-cle DSL de declaration de fichier Etat
     * (`ETAT_X est un Etat`) MAIS aussi propriete unifiee d'un controle
     * (`Etat = Actif` dans un bloc PROPRIETES). Seul le `=` distingue : suivi
     * de `=` c'est une propriete (pas de block), sinon c'est la declaration
     * (ouvre un block, y compris `est un Etat(params)` ou suivi d'un
     * sous-bloc Corps/EnTeteDocument, pas seulement PROPRIETES). */
    bool isEtatAmbiguous = strcmp(lower, "etat") == 0
                        || strcmp(lower, "report") == 0;
    /*  — `Theme` : opener du bloc de styles MAIS aussi receveur d'un
     * acces `Theme.<style>` (valeur d'une propriete Style). Suivi de `=`/`.`
     * → pas un opener. Nu (declaration `<Nom> est un Thème`) → opener. */
    bool isThemeAmbiguous = strcmp(lower, "theme") == 0;
    if (!isPropNameAmbiguous && !isTypeAmbiguous && !isListeAmbiguous
        && !isEtatAmbiguous && !isThemeAmbiguous) return true;

    /* Skip whitespace puis peek. Si '=' → propriete. */
    while (*afterWord == ' ' || *afterWord == '\t') afterWord++;
    if (*afterWord == '=') return false;
    /*  — `Theme.<style>` : acces membre, pas un opener de bloc.
     * `<Nom> est un Thème` nu : opener du bloc de styles (pas de fallback
     * peek-PROPRIETES, le corps est une liste de styles nommes). */
    if (isThemeAmbiguous) return *afterWord != '.';
    /* Bouton/Libelle/Saisie (+ EN) hors `=` : opener comme un widget
     * non-ambigu. Pas de peek-PROPRIETES : pas de sens
     * type/variable a trancher. */
    if (isPropNameAmbiguous) return true;
    /* Etat/report hors `=` : toujours un opener de declaration (pas de
     * fallback peek-PROPRIETES, contrairement a Image/Liste). */
    if (isEtatAmbiguous) return true;
    if (isListeAmbiguous) {
        /* 'Liste de <type>' : collection, pas de block. Specifique Liste
         * (Image n'a pas de syntaxe `est une Image de X`). */
        if ((afterWord[0] == 'd' || afterWord[0] == 'D')
            && (afterWord[1] == 'e' || afterWord[1] == 'E')
            && (afterWord[2] == ' ' || afterWord[2] == '\t' || afterWord[2] == '\0'
                || afterWord[2] == '\n' || afterWord[2] == '\r'))
            return false;
    }
    /*  +  — Image et Liste partagent la meme heuristique
     * peek-PROPRIETES quand le mot est nu (newline immediat) : ambigu
     * entre widget `IMG_X est une Image \n PROPRIETES ...` (resp.
     * `LISTE_X est une Liste \n PROPRIETES ...`) et variable simple
     * `img est une Image \n ...autre code...` (resp. collection
     * `mL est une Liste \n ...`). Peek le prochain mot significatif
     * (skip whitespace + newlines + line comments). Si c'est
     * `PROPRIETES`/`PROPERTIES` → widget (ouvre block). Sinon (autre
     * statement, EOF) → variable / collection.
     *
     *  — un commentaire inline (`Image // cmt`) doit aussi
     * declencher ce peek : sans ca, `afterWord` pointe sur `/`, on
     * tombait sur le `return true` final et le commentaire ouvrait un
     * faux block qui cascadait sur tout le corps de la fenetre. Le peek
     * saute deja les `//`, il suffit d'entrer dans la branche. */
    if (*afterWord == '\0' || *afterWord == '\n' || *afterWord == '\r'
        || (afterWord[0] == '/' && afterWord[1] == '/')) {
        return peek_word_is_proprietes(afterWord);
    }
    return true;
}

/*  — types DSL verrouilles au top-level : Fenetre/Window et
 * Etat/Report ne peuvent ouvrir un bloc DSL qu'a la racine du fichier. A
 * l'interieur d'une procedure/classe/bloc, `x est une Fenetre` est une simple
 * declaration de variable locale, PAS un ouvreur de bloc — sinon le FIN
 * englobant ressort sur-indente. Les widgets (Bouton, Saisie...) et sous-blocs
 * Etat (Corps, HautDePage...) restent des ouvreurs a indent > 0 : eux vivent
 * dans le bloc, ils ne sont PAS concernes. */
static bool word_is_toplevel_only_dsl(const char* word, int len) {
    char ascii[64];
    int aLen = scanner_strip_accents(word, len, ascii, sizeof(ascii));
    if (aLen <= 0 || aLen >= (int)sizeof(ascii)) return false;
    char lower[64];
    for (int i = 0; i < aLen; i++) lower[i] = (char)tolower((unsigned char)ascii[i]);
    lower[aLen] = '\0';
    return strcmp(lower, "fenetre") == 0 || strcmp(lower, "window") == 0
        || strcmp(lower, "etat")    == 0 || strcmp(lower, "report") == 0
        || strcmp(lower, "theme")   == 0
        || strcmp(lower, "collectiondeprocedures") == 0
        || strcmp(lower, "procedurecollection")    == 0;
}

static bool word_closes_block(const char* word, int len) {
    char lower[16];
    if (len >= 16) return false;
    for (int i = 0; i < len; i++) lower[i] = (char)tolower((unsigned char)word[i]);
    lower[len] = '\0';
    return strcmp(lower, "fin") == 0 || strcmp(lower, "end") == 0;
}

/* Detecte "cas" / "autre" (case+accent insensible). Ces mots ne sont
 * PAS dans word_is_mid_block car ils ont une semantique propre dans
 * SELON : leur body newline-form ouvre un niveau d'indent supplementaire,
 * different du pattern close+reopen au meme niveau (SINON, FAIRE...). */
static bool word_is_cas_or_autre(const char* word, int len) {
    char lower[16];
    if (len >= 16) return false;
    char ascii[16];
    int aLen = scanner_strip_accents(word, len, ascii, sizeof(ascii));
    if (aLen <= 0 || aLen >= 16) return false;
    for (int i = 0; i < aLen; i++) lower[i] = (char)tolower((unsigned char)ascii[i]);
    lower[aLen] = '\0';
    return strcmp(lower, "cas") == 0 || strcmp(lower, "autre") == 0
        || strcmp(lower, "case") == 0;
}

/* Detecte si la ligne CAS/AUTRE se termine par ':' seul (newline form),
 * vs `CAS X : instr` (inline form). On scanne jusqu'au '\n' ou au debut
 * d'un commentaire ligne, en ignorant les espaces/tab trailing.
 * Si le dernier caractere significatif est ':', c'est newline-form. */
static bool cas_line_ends_with_colon_only(const char* line) {
    const char* lineEnd = line;
    while (*lineEnd && *lineEnd != '\n') {
        if (lineEnd[0] == '/' && lineEnd[1] == '/') break;
        if (*lineEnd == '"') { fmt_skip_string_literal(&lineEnd, NULL, true); continue; }
        lineEnd++;
    }
    /* Strip trailing whitespace (CRLF inclus : le \r final ne doit pas
     * masquer le ':' qui marque la forme newline du CAS). */
    while (lineEnd > line && (lineEnd[-1] == ' ' || lineEnd[-1] == '\t'
                              || lineEnd[-1] == '\r'))
        lineEnd--;
    return lineEnd > line && lineEnd[-1] == ':';
}

static bool word_is_mid_block(const char* word, int len) {
    const char* mids[] = {
        /* Mid-block keywords that dedent+reopen (SI / SINON / FIN pattern,
         * class visibility sections). CAS / AUTRE are NOT here because they
         * stay indented under SELON — they're regular content inside the
         * block, not a dedent marker. */
        "sinon", "else",
        "faire", "do",
        "enfin", "finally",
        "public", "publique", "prive", "privee", "private",
        "protege", "protegee", "protected",
        NULL
    };
    char lower[32];
    if (len >= 32) return false;
    char ascii[32];
    int aLen = scanner_strip_accents(word, len, ascii, sizeof(ascii));
    if (aLen <= 0 || aLen >= 32) return false;
    for (int i = 0; i < aLen; i++) lower[i] = (char)tolower((unsigned char)ascii[i]);
    lower[aLen] = '\0';

    for (int i = 0; mids[i]; i++) {
        if (strcmp(lower, mids[i]) == 0) return true;
    }
    return false;
}

/* Le mot-cle qui ouvre cette ligne est-il dans sa forme INLINE, celle qui
 * n'attend aucun FIN ? Deux formes du langage sont concernees :
 *     SI <cond> ALORS <instr>     (mono-ligne canon)
 *     CONSTANT[E] NOM = literal   (forme inline, doc synonyme de la forme bloc)
 * Aucune des deux ne doit incrementer le compteur d'indentation.
 *
 *  — le discriminant n'est plus reimplemente ici. Deux clones vivaient
 * a cet endroit (is_si_inline_line / is_constant_inline_line), chacun avec sa
 * propre gestion des fins de ligne et des commentaires ; la porte TEXTE de la
 * source unique (prescan.h) rend le meme verdict via le Scanner reel, donc
 * avec la meme lecture des chaines, des commentaires et des continuations de
 * ligne que le compilateur. */
static bool line_opener_is_inline_form(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    int delta = 0;
    if (!prescan_depth_delta_text(p, &delta)) return false;
    return delta == 0;
}

/* Detecte si la ligne commence par CAS / AUTRE / CASE / OTHER. Active le
 * mode "preserveOps" : les comparateurs prefixes canon (`[=`, `[~`, `=]`,
 * `~]`, `~~]`, ...) ne suivent pas la convention "1 espace autour de
 * l'op binaire". */
static bool line_starts_with_cas_or_autre(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    if (!is_word_char(*p)) return false;
    const char* w = p;
    while (is_word_char(*p)) p++;
    int wlen = (int)(p - w);
    if (wlen == 3 && strncasecmp(w, "cas", 3) == 0) return true;
    if (wlen == 4 && strncasecmp(w, "case", 4) == 0) return true;
    if (wlen == 5 && strncasecmp(w, "autre", 5) == 0) return true;
    if (wlen == 5 && strncasecmp(w, "other", 5) == 0) return true;
    return false;
}

/* ================================================================
 * WHITESPACE NORMALIZATION (post-format pass per line)
 *
 * Regles canoniques d'espacement :
 *   - collapse espaces multiples vers 1 seul
 *   - pas d'espace apres `(`, avant `)`, avant `,`
 *   - 1 espace apres `,` et `;`
 *   - 1 espace autour des operateurs binaires `=`, `+`, `-`, `*`, `/`,
 *     `<`, `>`, `<=`, `>=`, `<>`, `==`, `!=`
 *   - operateurs unaires (`-`/`+` apres `=`, `,`, `(`, autre op, debut) :
 *     pas d'espace apres
 *   - autour de `.` et `..` : pas d'espace
 *   - strings / comments / multiline preserves
 *   - operateurs prefixes canon `[=`, `[~`, `=]`, `~]`, etc. : preserves
 *     verbatim (mode preserveOps sur ligne CAS)
 * ================================================================ */

static bool norm_is_op_char(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '<' || c == '>' || c == '=' || c == '%';
}

static bool norm_is_unary_context(char prevChar, const char* prevWord, int prevWordLen) {
    if (prevChar == '\0') return true;
    if (prevChar == '(' || prevChar == '[' || prevChar == '{') return true;
    if (prevChar == ',' || prevChar == ';' || prevChar == ':') return true;
    if (norm_is_op_char(prevChar)) return true;
    if (prevWord && prevWordLen > 0 && prevWordLen < 32) {
        char lw[32];
        for (int i = 0; i < prevWordLen; i++)
            lw[i] = (char)tolower((unsigned char)prevWord[i]);
        lw[prevWordLen] = '\0';
        const char* exprOpeners[] = {
            "si", "if", "alors", "then", "sinon", "else",
            "et", "and", "ou", "or", "pas", "not",
            /*  : `step` est la forme EN du pas de boucle (PAS). La
             * normalisation d'espacement voit la forme DEJA traduite, donc en
             * FR->EN le mot devant `-1` est `STEP`, pas `PAS`. Sans cette
             * entree, `POUR i = 5 À 1 PAS -1` sortait `STEP - 1` (le `-` traite
             * en binaire). `pas` couvre deja le sens EN->FR. */
            "step",
            "retour", "return", "renvoyer", "result",
            "tantque", "while", "selon", "switch", "cas", "case",
            "dans", "in", "par", "by", "pour", "for",
            "faire", "do",
            NULL
        };
        for (int i = 0; exprOpeners[i]; i++) {
            if (strcmp(lw, exprOpeners[i]) == 0) return true;
        }
    }
    return false;
}

static void emit_normalized_line(const char* line, int len, Buffer* out,
                                  bool preserveOps) {
    /* preserveOps reserve pour usage futur. Pour l'instant le mode "ligne
     * CAS" est gere par shortcut copie-verbatim dans owl_format (cf
     * isCasLine), donc preserveOps reste toujours false ici. */
    (void)preserveOps;
    int i = 0;
    char prevEmitted = '\0';
    const char* prevWord = NULL;
    int prevWordLen = 0;
    bool pendingSpace = false;
    /*  — tracker "dernier mot = tableau" : si on detecte un `<`
     * immediatement apres le mot-cle `tableau` (toutes formes : tableau,
     * tableaux, array), on est tres probablement sur la forme generique
     * style C++ `tableau<T>` que le compiler rejette. Le formatter ne
     * RE-ECRIT PAS vers la forme canon (decision coordinator : le compiler
     * fait le job avec un message guidant), mais il ne doit pas non plus
     * AGGRAVER en ajoutant des espaces autour des chevrons (transformant
     * `tableau<entier>` en `tableau < entier >`). Le flag reste actif
     * jusqu'au `>` de fermeture. */
    bool inTableauGeneric = false;

    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

    while (i < len) {
        char c = line[i];

        if (c == '/' && i + 1 < len && line[i+1] == '/') {
            if (prevEmitted != '\0') buf_putc(out, ' ');
            while (i < len) buf_putc(out, line[i++]);
            return;
        }

        if (c == '/' && i + 1 < len && line[i+1] == '*') {
            if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, line[i++]);
            buf_putc(out, line[i++]);
            while (i < len && !(line[i] == '*' && i + 1 < len && line[i+1] == '/')) {
                buf_putc(out, line[i++]);
            }
            if (i < len) { buf_putc(out, line[i++]); if (i < len) buf_putc(out, line[i++]); }
            prevEmitted = '/'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == '"') {
            if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, line[i++]);
            /* Chaines BRUTES (cf. scanner.c::scan_string) : le backslash n'a
             * AUCUN role special. Seul echappement = doublage "" (deux
             * guillemets consecutifs = un guillemet litteral, on reste dans
             * la chaine). Un " ferme donc la chaine SAUF s'il est suivi d'un
             * autre ". Traiter \" comme un echappement C decalait les
             * frontieres de chaines et canonicalisait des mots-cles a
             * l'interieur de la chaine suivante. */
            while (i < len) {
                if (line[i] == '"') {
                    if (i + 1 < len && line[i+1] == '"') {
                        buf_putc(out, line[i++]);
                        buf_putc(out, line[i++]);
                        continue;
                    }
                    break;  /* vrai guillemet fermant */
                }
                buf_putc(out, line[i++]);
            }
            if (i < len) buf_putc(out, line[i++]);
            prevEmitted = '"'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        /* Backtick string OWL (raw + interpolation [%%]). Doublage `` pour
         * backtick litteral. Preserve verbatim. */
        if (c == '`') {
            if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, line[i++]);
            while (i < len && line[i] != '`') {
                buf_putc(out, line[i++]);
            }
            /* Gere `` doublage : si on s'arrete sur un ` et le suivant aussi `,
             * c'est un litteral, on consomme les deux et on continue. */
            while (i < len && line[i] == '`' && i + 1 < len && line[i+1] == '`') {
                buf_putc(out, line[i++]);
                buf_putc(out, line[i++]);
                while (i < len && line[i] != '`') buf_putc(out, line[i++]);
            }
            if (i < len) buf_putc(out, line[i++]);
            prevEmitted = '`'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == '[' && i + 1 < len && line[i+1] == '[') {
            if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, line[i++]);
            buf_putc(out, line[i++]);
            while (i < len && !(line[i] == ']' && i + 1 < len && line[i+1] == ']')) {
                buf_putc(out, line[i++]);
            }
            if (i < len) { buf_putc(out, line[i++]); if (i < len) buf_putc(out, line[i++]); }
            prevEmitted = ']'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == ' ' || c == '\t') {
            if (prevEmitted != '\0') pendingSpace = true;
            i++;
            continue;
        }

        if (c == '.' && i + 1 < len && line[i+1] == '.') {
            pendingSpace = false;
            buf_putc(out, '.');
            buf_putc(out, '.');
            i += 2;
            prevEmitted = '.'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == '.') {
            pendingSpace = false;
            buf_putc(out, '.');
            i++;
            prevEmitted = '.'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == '(') {
            /* Glue `(` au mot precedent (style `foo(x)`) SAUF si le mot
             * precedent est un keyword de controle (BOUCLE, SI, POUR,
             * TANTQUE, AVEC, RETOUR, RENVOYER, ...) — `BOUCLE (3)` garde
             * l'espace intentionnel.
             *  — egalement preserver l'espace quand `(` suit un
             * operateur binaire (`2 * (3 + 4)`, `x = (a + b)`). Couvre les
             * op chars (+, -, *, /, =, <, >, %) + `^` (puissance) + les op
             * textuels `MODULO`/`MOD`, `_ET_`/`_OU_` (les FR/EN `ET`/`OU`
             * etaient deja dans keepKw). */
            bool keepSpaceBeforeParen = false;
            if (pendingSpace && prevWord && prevWordLen > 0 && prevWordLen < 32) {
                /*  patch — accent-strip pour matcher les keywords de
                 * range loop apres canonisation FR par format_line_content :
                 * `POUR i = 1 A (...)` devient `POUR i = 1 À (...)` au 1er
                 * passage. Au 2e passage (ici), le mot precedent est `À`
                 * (UTF-8 0xC3 0x80), pas `a`. Sans strip, keepKw ne matche
                 * jamais et l'espace avant `(` est mange. */
                char lw[32];
                int lwLen = scanner_strip_accents(prevWord, prevWordLen,
                                                  lw, sizeof(lw));
                for (int j = 0; j < lwLen; j++)
                    lw[j] = (char)tolower((unsigned char)lw[j]);
                lw[lwLen] = '\0';
                const char* keepKw[] = {
                    "boucle", "loop", "si", "if", "pour", "for",
                    "tantque", "while", "avec", "with",
                    "retour", "return", "renvoyer", "result",
                    "et", "and", "ou", "or",
                    "modulo", "mod",
                    "_et_", "_ou_",
                    /*  patch — range loop keywords (TOKEN_A, TOKEN_A_SC,
                     * TOKEN_PAS, TOKEN_PAS_SC). `a` est forme FR canon (post-
                     * strip de À) ET ASCII brut `A`/`TO` jamais canonise par
                     * format_line_content. `pas` collisionne avec le NOT
                     * unaire mais le contexte `(` apres est sans ambiguite :
                     * `POUR ... PAS (expr)` veut l'espace. */
                    "a", "to", "_a_", "_to_",
                    "pas", "not", "step", "_pas_", "_step_",
                    "sortir", "break", "exit", "continuer", "continue",
                    "alors", "then", "sinon", "else",
                    "dans", "in",
                    NULL
                };
                for (int k = 0; keepKw[k]; k++) {
                    if (strcmp(lw, keepKw[k]) == 0) { keepSpaceBeforeParen = true; break; }
                }
            }
            /*  — operateur binaire emis comme caractere(s) : `+`,
             * `-`, `*`, `/`, `=`, `<`, `>`, `%`, `^`. On lit `prevEmitted`
             * (dernier char emis) avec pendingSpace (espace input present).
             * Cas `tableau<entier>` : prevEmitted='>' mais prevWord==NULL
             * (reset par la branche tableau) — ce code n'est pas atteint
             * car la forme generique est suivie d'un identifier, pas d'un
             * `(`. Si jamais : on injecterait un espace, mais la forme est
             * deja rejected by compiler (cf ). */
            if (!keepSpaceBeforeParen && pendingSpace && prevEmitted != '\0') {
                if (norm_is_op_char(prevEmitted) || prevEmitted == '^') {
                    keepSpaceBeforeParen = true;
                }
            }
            if (keepSpaceBeforeParen && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, c);
            i++;
            prevEmitted = c; prevWord = NULL; prevWordLen = 0;
            continue;
        }
        if (c == '[') {
            /*  — '[=', '[=]', '[~', '[~]', '[~~', '[~~]' : operateurs
             * binaires de comparaison souple, pas une indexation. Une
             * indexation ne peut pas commencer par '=' ni par '~', la
             * distinction tient donc au seul caractere suivant. Sans elle la
             * regle de collage de l'indexation s'appliquait et l'operateur se
             * collait a son terme de gauche ('s [= "x"' -> 's[= "x"') tout en
             * gardant son espace a droite : une espace de chaque cote, comme
             * tout operateur binaire. */
            if (i + 1 < len && (line[i+1] == '=' || line[i+1] == '~')) {
                if (prevEmitted != '\0' && prevEmitted != '(' &&
                    prevEmitted != '[' && prevEmitted != ',') {
                    buf_putc(out, ' ');
                }
                pendingSpace = false;
                buf_putc(out, '[');
                i++;
                char last = '[';
                while (i < len && (line[i] == '=' || line[i] == '~' || line[i] == ']')) {
                    last = line[i];
                    buf_putc(out, line[i++]);
                }
                pendingSpace = true;
                prevEmitted = last; prevWord = NULL; prevWordLen = 0;
                continue;
            }
            bool glue = (prevEmitted != '\0') &&
                        (isalnum((unsigned char)prevEmitted) ||
                         prevEmitted == '_' || prevEmitted == ')' ||
                         prevEmitted == ']' ||
                         (unsigned char)prevEmitted >= 0x80);
            if (!glue && pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, '[');
            i++;
            while (i < len && (line[i] == '=' || line[i] == '~' || line[i] == ']')) {
                buf_putc(out, line[i++]);
            }
            prevEmitted = '['; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == ')' || c == ']') {
            pendingSpace = false;
            buf_putc(out, c);
            i++;
            prevEmitted = c; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == '{') {
            if (pendingSpace && prevEmitted != '\0' && prevEmitted != '(' &&
                prevEmitted != '[' && prevEmitted != ',') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, '{');
            i++;
            prevEmitted = '{'; prevWord = NULL; prevWordLen = 0;
            continue;
        }
        if (c == '}') {
            pendingSpace = false;
            buf_putc(out, '}');
            i++;
            prevEmitted = '}'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == ',') {
            pendingSpace = false;
            buf_putc(out, ',');
            i++;
            pendingSpace = true;
            prevEmitted = ','; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == ';') {
            pendingSpace = false;
            buf_putc(out, ';');
            i++;
            pendingSpace = true;
            prevEmitted = ';'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == ':') {
            if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, ':');
            i++;
            if (i < len && (line[i] == ' ' || line[i] == '\t')) pendingSpace = true;
            prevEmitted = ':'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (c == '~') {
            if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, '~');
            i++;
            while (i < len && (line[i] == '~' || line[i] == '=' || line[i] == ']')) {
                buf_putc(out, line[i++]);
            }
            prevEmitted = '~'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        /*  — prefixe de chaine interpolee `%"..."` (cf. scanner.c
         * scan_string_interp : au `%`, si le caractere suivant est `"`, c'est
         * une chaine, pas l'operateur modulo). Le `%` se colle au guillemet ;
         * l'espace canon a GAUCHE est celui de l'operande qui precede (via
         * pendingSpace), aucun a droite. Sans ce cas, la branche operateur
         * generique inserait ` % ` et le `%` redevenait modulo -> code valide
         * corrompu au save (invariant 4). `%=` (PERCENT_EQUAL) reste
         * l'operateur : le scanner teste `=` avant `"`, ici line[i+1]!='"'. */
        if (c == '%' && i + 1 < len && line[i+1] == '"') {
            if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
            pendingSpace = false;
            buf_putc(out, '%');
            i++;
            prevEmitted = '%'; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (norm_is_op_char(c) || c == '!') {
            char opbuf[4];
            int oplen = 0;
            opbuf[oplen++] = c;
            i++;
            if (i < len) {
                char n = line[i];
                if ((c == '<' && (n == '=' || n == '>')) ||
                    (c == '>' && n == '=') ||
                    (c == '=' && (n == '=' || n == ']' || n == '>')) ||   /* `==`, `=]`, ou `=>` (lambda) */
                    (c == '!' && n == '=') ||
                    (c == '+' && (n == '=' || n == '+')) ||   /* `+=` ou `++` */
                    (c == '-' && (n == '=' || n == '-')) ||   /* `-=` ou `--` */
                    (c == '*' && n == '=') ||
                    (c == '/' && n == '=')) {
                    opbuf[oplen++] = n;
                    i++;
                }
            }
            opbuf[oplen] = '\0';

            /*  — non-aggravation `tableau<T>`. Si le mot precedent
             * est `tableau` (ou `tableaux` / `array`) et qu'on emet un `<`
             * simple, on regarde le mot qui suit (apres whitespace eventuel).
             * S'il s'agit de `agrandissement` = attribut canon,
             * comportement legacy preserve (espaces autour). Sinon
             * = forme generique illegale `tableau<T>` : on emet l'op colle
             * (pas d'espace gauche/droite) et on arme un flag pour le `>`
             * correspondant. Le compiler rejettera avec un message guidant.
             * Objectif : ne pas transformer `tableau<entier>` en
             * `tableau < entier >` (aggravation lisibilite + diagnostic). */
            if (oplen == 1 && opbuf[0] == '<' &&
                prevWord && prevWordLen > 0 && prevWordLen < 16) {
                char lw[16];
                for (int j = 0; j < prevWordLen; j++)
                    lw[j] = (char)tolower((unsigned char)prevWord[j]);
                lw[prevWordLen] = '\0';
                if (strcmp(lw, "tableau") == 0 ||
                    strcmp(lw, "tableaux") == 0 ||
                    strcmp(lw, "array") == 0) {
                    /* Peek le prochain mot apres whitespace. */
                    int peek = i;
                    while (peek < len && (line[peek] == ' ' || line[peek] == '\t')) peek++;
                    int wstart = peek;
                    while (peek < len && is_word_char(line[peek])) peek++;
                    int wlen = peek - wstart;
                    bool isAgrandissement = (wlen == 14 &&
                        strncasecmp(line + wstart, "agrandissement", 14) == 0);
                    if (!isAgrandissement) {
                        pendingSpace = false;
                        buf_putc(out, '<');
                        prevEmitted = '<'; prevWord = NULL; prevWordLen = 0;
                        inTableauGeneric = true;
                        continue;
                    }
                    /*  — attribut canon `<agrandissement[=valeur]>`.
                     * Le formatter ne doit ni injecter d'espace autour de
                     * `<`/`=`/`>` (interieur de l'attribut), ni canoniser la
                     * valeur (litteral utilisateur — entier ou identifier).
                     * Strategie non-aggravante : on garde l'espace canon
                     * entre `tableau` et `<` (forme canon standard), puis on
                     * copie verbatim tout le contenu jusqu'au `>` en
                     * squeezant les runs de whitespace interieurs (sortie
                     * compacte idempotente). */
                    if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
                    pendingSpace = false;
                    buf_putc(out, '<');
                    /* Consomme le whitespace eventuel entre `<` et le mot. */
                    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
                    while (i < len && line[i] != '>') {
                        if (line[i] == ' ' || line[i] == '\t') {
                            /* squeeze : on saute tout run de whitespace */
                            while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
                            continue;
                        }
                        buf_putc(out, line[i++]);
                    }
                    if (i < len && line[i] == '>') {
                        buf_putc(out, '>');
                        i++;
                    }
                    prevEmitted = '>'; prevWord = NULL; prevWordLen = 0;
                    continue;
                }
            }
            /*  — attribut de declaration `<cle[=valeur]>` hors tableau
             * (deja canonise en casse par format_line_content ; ici on garantit
             * la forme COMPACTE en court-circuitant l'espacement operateur du
             * `<`/`=`/`>` que ce char-loop appliquerait sinon). Meme
             * position-gate strict que la pass 1 (fmt_peek_decl_attr) : un
             * comparateur `<`/`<=`/`<>` n'est jamais capte. */
            if (oplen == 1 && opbuf[0] == '<' &&
                fmt_peek_decl_attr(line + i, line + len)) {
                if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
                pendingSpace = false;
                buf_putc(out, '<');
                i = (int)(fmt_emit_attr_body(out, line + i, line + len) - line);
                prevEmitted = '>'; prevWord = NULL; prevWordLen = 0;
                continue;
            }
            if (oplen == 1 && opbuf[0] == '>' && inTableauGeneric) {
                pendingSpace = false;
                buf_putc(out, '>');
                prevEmitted = '>'; prevWord = NULL; prevWordLen = 0;
                inTableauGeneric = false;
                continue;
            }

            /* `++` / `--` : operateur post/pre-fixe unaire. Pas d'espace
             * entre l'identifier precedent et l'op (`x++`, pas `x ++`). */
            bool isIncrDecr = (oplen == 2 &&
                               ((opbuf[0] == '+' && opbuf[1] == '+') ||
                                (opbuf[0] == '-' && opbuf[1] == '-')));
            if (isIncrDecr) {
                pendingSpace = false;
                buf_write(out, opbuf, oplen);
                prevEmitted = opbuf[oplen - 1]; prevWord = NULL; prevWordLen = 0;
                continue;
            }

            if (preserveOps) {
                if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
                pendingSpace = false;
                buf_write(out, opbuf, oplen);
                if (i < len && (line[i] == ' ' || line[i] == '\t')) pendingSpace = true;
                prevEmitted = opbuf[oplen - 1]; prevWord = NULL; prevWordLen = 0;
                continue;
            }

            bool isUnaryCandidate = (oplen == 1 && (c == '-' || c == '+'));
            bool unary = isUnaryCandidate &&
                         norm_is_unary_context(prevEmitted, prevWord, prevWordLen);

            if (unary) {
                /* Espace de gauche SAUF si on suit immediatement un opener
                 * (`(`, `[`, `{`, `,`). Apres un operateur binaire (`= -1`)
                 * ou un mot-cle (`RETOUR -1`), on emet l'espace via pending. */
                if (pendingSpace && prevEmitted != '\0' && prevEmitted != '(' &&
                    prevEmitted != '[' && prevEmitted != ',' &&
                    prevEmitted != '{') {
                    buf_putc(out, ' ');
                }
                pendingSpace = false;
                buf_write(out, opbuf, oplen);
                pendingSpace = false;
                prevEmitted = opbuf[oplen - 1]; prevWord = NULL; prevWordLen = 0;
                continue;
            }

            if (prevEmitted != '\0') buf_putc(out, ' ');
            buf_write(out, opbuf, oplen);
            pendingSpace = true;
            prevEmitted = opbuf[oplen - 1]; prevWord = NULL; prevWordLen = 0;
            continue;
        }

        if (is_word_char(c)) {
            /*  — `tableau<T>` non-aggravation : si on est entre les
             * chevrons de la forme generique (rejetee par le compiler), on
             * colle l'identifier au `<` sans espace, comme `(`/`[`/`.`/`{`. */
            if (pendingSpace && prevEmitted != '\0' &&
                prevEmitted != '(' && prevEmitted != '[' &&
                prevEmitted != '.' && prevEmitted != '{' &&
                !(inTableauGeneric && prevEmitted == '<')) {
                buf_putc(out, ' ');
            }
            pendingSpace = false;
            const char* ws = line + i;
            bool startsDigit = (c >= '0' && c <= '9');
            if (startsDigit) {
                /*  — prefixe litteral atomique 0[xXnNmM]<chiffres>.
                 * Aligne sur scanner.c::scan_number (lignes 700-714). Sans ca
                 * le re-tokenizer sortait `0` puis identifiait `n154` comme
                 * mot separe et inserait un espace, cassant l'idempotence. */
                if (c == '0' && i + 1 < len) {
                    char pfx = line[i + 1];
                    if (pfx == 'x' || pfx == 'X') {
                        i += 2; /* consomme 0x */
                        while (i < len &&
                               ((line[i] >= '0' && line[i] <= '9') ||
                                (line[i] >= 'a' && line[i] <= 'f') ||
                                (line[i] >= 'A' && line[i] <= 'F') ||
                                line[i] == '_')) {
                            i++;
                        }
                    } else if (pfx == 'n' || pfx == 'N' ||
                               pfx == 'm' || pfx == 'M') {
                        i += 2; /* consomme 0n / 0m */
                        /* Signe `-` collé (suivi d'un chiffre) = signe du
                         * littéral, pas une soustraction : on le garde collé
                         * (forme canonique 0n-X.Y, cf. scanner.c). */
                        if (i + 1 < len && line[i] == '-' &&
                            line[i + 1] >= '0' && line[i + 1] <= '9') {
                            i++;
                        }
                        while (i < len &&
                               ((line[i] >= '0' && line[i] <= '9') ||
                                line[i] == '_')) {
                            i++;
                        }
                        if (i < len && line[i] == '.' && i + 1 < len &&
                            line[i + 1] >= '0' && line[i + 1] <= '9') {
                            i++; /* . */
                            while (i < len &&
                                   ((line[i] >= '0' && line[i] <= '9') ||
                                    line[i] == '_')) {
                                i++;
                            }
                        }
                    } else {
                        while (i < len &&
                               ((line[i] >= '0' && line[i] <= '9') ||
                                line[i] == '.' || line[i] == '_')) {
                            i++;
                        }
                        /* Exposant scientifique optionnel : e/E[+/-]digit+ */
                        if (i < len && (line[i] == 'e' || line[i] == 'E')) {
                            int look = i + 1;
                            if (look < len && (line[look] == '+' || line[look] == '-')) look++;
                            if (look < len && line[look] >= '0' && line[look] <= '9') {
                                i = look;
                                while (i < len && line[i] >= '0' && line[i] <= '9') i++;
                            }
                        }
                    }
                } else {
                    while (i < len &&
                           ((line[i] >= '0' && line[i] <= '9') ||
                            line[i] == '.' || line[i] == '_')) {
                        i++;
                    }
                    /* Exposant scientifique optionnel : e/E[+/-]digit+ */
                    if (i < len && (line[i] == 'e' || line[i] == 'E')) {
                        int look = i + 1;
                        if (look < len && (line[look] == '+' || line[look] == '-')) look++;
                        if (look < len && line[look] >= '0' && line[look] <= '9') {
                            i = look;
                            while (i < len && line[i] >= '0' && line[i] <= '9') i++;
                        }
                    }
                }
            } else {
                while (i < len && is_word_char(line[i])) i++;
            }
            int wlen = (int)((line + i) - ws);
            buf_write(out, ws, wlen);
            prevEmitted = ws[wlen - 1]; prevWord = ws; prevWordLen = wlen;
            if (startsDigit && i < len && is_word_char(line[i])) {
                pendingSpace = true;
            }
            continue;
        }

        if (pendingSpace && prevEmitted != '\0') buf_putc(out, ' ');
        pendingSpace = false;
        buf_putc(out, c);
        i++;
        prevEmitted = c; prevWord = NULL; prevWordLen = 0;
    }
}


/* ================================================================
 * LINE-BASED FORMATTER
 *
 * Process source line by line:
 *   1. Strip leading whitespace
 *   2. Determine indent level from first keyword
 *   3. Walk the line replacing keywords with canonical forms
 *   4. Skip string literals and comments verbatim
 * ================================================================ */

/*  round 2 — vrai ssi `p` (positionne juste apres un identifier)
 * pointe vers une ouverture d'appel `(`, eventuellement precedee de
 * whitespace. Sert a discriminer `Age("...")` (appel native standalone)
 * d'un usage de champ user homonyme `age` (lecture/declaration). */
static bool is_call_position(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return *p == '(';
}

/*  — vrai ssi `lastWord` (le mot emis juste avant) est un connecteur
 * d'annotation de type : `est` / `un` / `une` / `is`. Le token courant est
 * alors en position de TYPE (`p est une procédure`), pas en position
 * structurelle. Sert a discriminer le keyword homonyme `procédure`/`fonction`
 * (nom de type minuscule accentue) du declarateur de bloc `PROCEDURE`. */
static bool fmt_after_type_connector(const char* lastWord, int lastWordLen) {
    if (!lastWord || lastWordLen <= 0 || lastWordLen > 3) return false;
    char lw[8];
    int ll = scanner_strip_accents(lastWord, lastWordLen, lw, sizeof(lw));
    if (ll <= 0 || ll >= (int)sizeof(lw)) return false;
    for (int i = 0; i < ll; i++) lw[i] = (char)tolower((unsigned char)lw[i]);
    lw[ll] = '\0';
    return strcmp(lw, "est") == 0 || strcmp(lw, "un") == 0 ||
           strcmp(lw, "une") == 0 || strcmp(lw, "is") == 0;
}

/*  — true si le mot brut (accent/casse-insensible) est un KIND de
 * widget reconnu (is_widget de la table CONTROL_KINDS). Sert a confirmer le
 * contexte prefixe-widget : `Champ <kind>`. */
static bool fmt_word_is_widget_kind(const char* word, int len) {
    char asc[64];
    int al = scanner_strip_accents(word, len, asc, sizeof(asc));
    if (al <= 0 || al >= (int)sizeof(asc)) return false;
    char low[64];
    for (int i = 0; i < al; i++) low[i] = (char)tolower((unsigned char)asc[i]);
    low[al] = '\0';
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        if (!k->is_widget) continue;
        const char* nm[3] = { k->nameFR, k->nameAscii, k->nameEN };
        for (int n = 0; n < 3; n++) {
            if (!nm[n]) continue;
            char nb[64];
            int nl = scanner_strip_accents(nm[n], (int)strlen(nm[n]), nb, sizeof(nb));
            if (nl <= 0 || nl != al) continue;
            bool eq = true;
            for (int i = 0; i < nl; i++)
                if (low[i] != (char)tolower((unsigned char)nb[i])) { eq = false; break; }
            if (eq) return true;
        }
    }
    return false;
}

/*  — entree CONTROL_KINDS de la sorte de controle ecrite `word`, quelle
 * que soit sa langue ou sa graphie (`Fenêtre`, `Window`, `onglet`). `wroteEN`
 * (optionnel) rend true quand la forme ecrite est la forme EN canon et qu'elle
 * differe du FR : c'est ce qui permet a `owl fmt` de preserver la langue ecrite.
 * Couvre AUSSI les sortes structurelles (Volet, Colonne, blocs d'Etat), a la
 * difference de fmt_word_is_widget_kind. NULL si `word` n'est pas une sorte. */
static const OwlControlKindInfo* fmt_control_kind_lookup(const char* word, int len,
                                                         bool* wroteEN) {
    if (wroteEN) *wroteEN = false;
    char asc[64];
    int al = scanner_strip_accents(word, len, asc, sizeof(asc));
    if (al <= 0 || al >= (int)sizeof(asc)) return NULL;
    for (int i = 0; i < al; i++) asc[i] = (char)tolower((unsigned char)asc[i]);
    asc[al] = '\0';
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        const char* nm[3] = { k->nameFR, k->nameAscii, k->nameEN };
        for (int n = 0; n < 3; n++) {
            if (!nm[n]) continue;
            char nb[64];
            int nl = scanner_strip_accents(nm[n], (int)strlen(nm[n]), nb, sizeof(nb));
            if (nl <= 0 || nl != al) continue;
            bool eq = true;
            for (int i = 0; i < nl; i++)
                if (asc[i] != (char)tolower((unsigned char)nb[i])) { eq = false; break; }
            if (!eq) continue;
            if (wroteEN) *wroteEN = (n == 2 && strcmp(k->nameEN, k->nameFR) != 0);
            return k;
        }
    }
    return NULL;
}

/*  — forme ASCII canon (cle du catalogue DSL) de la sorte de controle
 * ecrite `word`. NULL si le mot n'est pas une sorte de controle. */
static const char* fmt_control_kind_ascii(const char* word, int len) {
    const OwlControlKindInfo* k = fmt_control_kind_lookup(word, len, NULL);
    if (k == NULL) return NULL;
    return k->nameAscii ? k->nameAscii : k->nameFR;
}

/*  — forme canon d'une sorte de controle en position de genre : langue
 * CIBLE en traduction, sinon langue de la forme ECRITE (`owl fmt` preserve).
 * Source unique = CONTROL_KINDS, donc toute sorte de la table bascule sans edit
 * du formateur. C'est ce cablage qui manquait : Onglet/Tab, Jauge/ProgressBar et
 * CodeBarres/BarCodes avaient leur paire dans la table sans jamais etre emise,
 * et `Tab` ne pouvait pas prendre une entree fmt_keywords[] sans masquer la
 * constante TAB partout ailleurs. */
static const char* fmt_control_kind_lang_form(const char* word, int len) {
    bool wroteEN = false;
    const OwlControlKindInfo* k = fmt_control_kind_lookup(word, len, &wroteEN);
    if (k == NULL) return NULL;
    if (g_fmt_target_lang == OWL_TRANSLATE_EN)
        return k->nameEN ? k->nameEN : k->nameFR;
    if (g_fmt_target_lang == OWL_TRANSLATE_FR) return k->nameFR;
    return wroteEN ? k->nameEN : k->nameFR;
}

/*  — le mot precedent est le prefixe widget `Champ`/`Field`/`Control`.
 * Seule position ou un mot designe une sorte de controle sans ambiguite : dans
 * `Champ Tab` c'est la sorte Onglet, alors que `TAB` seul reste la constante. */
static bool fmt_prev_word_is_widget_prefix(const char* lastWord, int lastWordLen) {
    if (lastWord == NULL || lastWordLen < 5 || lastWordLen > 7) return false;
    char lw[16];
    int ll = scanner_strip_accents(lastWord, lastWordLen, lw, sizeof(lw));
    if (ll <= 0 || ll >= (int)sizeof(lw)) return false;
    for (int i = 0; i < ll; i++) lw[i] = (char)tolower((unsigned char)lw[i]);
    lw[ll] = '\0';
    return strcmp(lw, "champ") == 0 || strcmp(lw, "field") == 0 ||
           strcmp(lw, "control") == 0;
}

/*  — index registre du type de style RACINE d'une sorte de controle
 * (`Onglet` -> styleOnglet), lu sur la prop `Style` de son entree de catalogue
 * DSL (object_type). C'est le point de depart de la cascade `Style..X..Y`
 * ecrite en ligne : sans lui aucun segment ne resout contre un type, exactement
 * comme la forme bloc sans son `{`. Source unique = le catalogue, aucune table
 * locale. -1 si la sorte n'a pas de prop `Style` typee. */
static int fmt_widget_style_root_index(const char* word, int len) {
    if (!owl_fmt_canon || !owl_fmt_canon->dsl_prop_kind
        || !owl_fmt_canon->dsl_prop_object_type) return -1;
    const char* kind = fmt_control_kind_ascii(word, len);
    if (kind == NULL) return -1;
    int n = owl_fmt_canon->dsl_prop_count();
    for (int i = 0; i < n; i++) {
        const char* fr = owl_fmt_canon->dsl_prop_fr(i);
        if (fr == NULL || strcasecmp(fr, "Style") != 0) continue;
        const char* k = owl_fmt_canon->dsl_prop_kind(i);
        if (k == NULL || strcasecmp(k, kind) != 0) continue;
        const char* ot = owl_fmt_canon->dsl_prop_object_type(i);
        if (ot == NULL) return -1;
        return fmt_composite_type_index(ot, (int)strlen(ot));
    }
    return -1;
}

/*  — confirme le contexte prefixe-widget : a partir de `p` (juste
 * apres le token `Champ`/`Control`/`Field`), le prochain mot significatif est
 * un kind de widget reconnu, en tolerant un connecteur `de`/`of` intercale
 * (`Champ de Saisie`). Whitespace seul saute (pas de newline/commentaire :
 * la declaration `<copule> <prefixe> <kind>` tient sur une ligne). Retourne
 * false sinon (type nu `c est un Champ`, variable, etc.). */
static bool fmt_widget_prefix_kind_follows(const char* p) {
    char w1[64];
    int n1 = fmt_peek_next_word(p, w1, sizeof w1);
    if (n1 <= 0) return false;
    if (fmt_word_is_widget_kind(w1, n1)) return true;
    /* Tolere `de`/`of` intercale : `Champ de Saisie`. */
    char a1[16];
    int al = scanner_strip_accents(w1, n1, a1, sizeof(a1));
    if (al != 2) return false;
    for (int i = 0; i < al; i++) a1[i] = (char)tolower((unsigned char)a1[i]);
    a1[al] = '\0';
    if (strcmp(a1, "de") != 0 && strcmp(a1, "of") != 0) return false;
    /* Avance p au-dela de `de`/`of` puis re-peek le kind. */
    const char* q = p;
    while (*q == ' ' || *q == '\t') q++;
    while (is_word_char(*q)) q++;          /* skip le `de`/`of` */
    char w2[64];
    int n2 = fmt_peek_next_word(q, w2, sizeof w2);
    return n2 > 0 && fmt_word_is_widget_kind(w2, n2);
}

/*  — palette d'etats de style pour le formatter. Miroir de la table
 * compile-time STATES_styleZoneSaisie (compiler.c) : les formes canon FR/EN
 * sont MINUSCULE. Vit ici (formatter.c) plutot que dans le provider OwlFmtCanon :
 * c'est une constante de langage, pas une donnee de registre. Comme formatter.c
 * est compile a l'identique dans owl.exe ET owl-lsp, les deux backends
 * produisent la meme sortie par construction (parite dual-backend garantie). */
typedef struct { const char* fr; const char* en; } FmtStyleState;
static const FmtStyleState FMT_STYLE_STATES[] = {
    {"survol",             "hover"},
    {"appuy\xc3\xa9",      "pressed"},     /* appuyé — palettes bouton / cadre */
    {"focus",              "focus"},
    {"gris\xc3\xa9",       "grayed"},      /* grisé */
    {"lectureSeule",       "readOnly"},
    {NULL, NULL}
};

/* Canonise un identifiant d'etat vers sa forme MINUSCULE de palette (bilingue :
 * la forme FR mappe vers FR canon, la forme EN vers EN canon). Retourne NULL si
 * l'identifiant n'est pas un etat de la palette (le caller emet verbatim). */
static const char* fmt_style_state_canon(const char* w, int len) {
    for (const FmtStyleState* s = FMT_STYLE_STATES; s->fr; s++) {
        if (ident_equal_ci_accent(w, len, s->fr, (int)strlen(s->fr))) return s->fr;
        if (ident_equal_ci_accent(w, len, s->en, (int)strlen(s->en))) return s->en;
    }
    return NULL;
}

/*  — meme palette, mais forme de la LANGUE CIBLE (translate) : le
 * subscript `[survol]` doit basculer en `[hover]` comme la cle de bloc
 * `Survol` bascule en `Hover`. Hors translate, c'est fmt_style_state_canon qui
 * reste appele (preservation de la langue ecrite, mode NONE inchange). */
static const char* fmt_style_state_target(const char* w, int len) {
    for (const FmtStyleState* s = FMT_STYLE_STATES; s->fr; s++) {
        if (ident_equal_ci_accent(w, len, s->fr, (int)strlen(s->fr)) ||
            ident_equal_ci_accent(w, len, s->en, (int)strlen(s->en)))
            return (g_fmt_target_lang == OWL_TRANSLATE_EN) ? s->en : s->fr;
    }
    return NULL;
}

/*  — cles d'etat en position de BLOC DSL (`Survol = { ... }`), avec
 * leur forme cible PascalCase. Distinct de FMT_STYLE_STATES (formes MINUSCULE
 * du subscript `[survol]`) : les cles de bloc suivent la convention PascalCase
 * des champs de la DSL. Inclut `Appuyé`/`Pressed` (palettes bouton/cadre).
 * Consomme UNIQUEMENT par le translate type-aware du bloc Etats ; le path fmt
 * (mode NONE) ne le voit jamais -> zero impact fmt-stability. */
typedef struct { const char* fr; const char* en; } FmtStyleStateBlock;
static const FmtStyleStateBlock FMT_STYLE_STATE_BLOCKS[] = {
    {"Survol",         "Hover"},
    {"Focus",          "Focus"},
    {"Appuy\xc3\xa9",  "Pressed"},   /* Appuyé */
    {"Gris\xc3\xa9",   "Grayed"},    /* Grisé */
    {"LectureSeule",   "ReadOnly"},
    {NULL, NULL}
};

/* Miroir formatter de STYLE_PALETTES (compiler.c) : type COLLECTION palette
 * -> baseType PLAT sous chaque etat. Vit ici (pas dans compiler.c) car
 * formatter.c est compile a l'identique dans owl.exe ET owl-lsp, mais owl-lsp
 * ne linke PAS compiler.c : appeler style_palette_baseType casserait le link
 * catalogue. Meme rationale que FMT_STYLE_STATES (constante de langage). */
typedef struct { const char* coll; const char* base; } FmtStylePaletteBase;
static const FmtStylePaletteBase FMT_STYLE_PALETTE_BASES[] = {
    {"styleZoneSaisieEtats",     "styleZoneSaisieBase"},
    {"styleZoneBoutonEtats",     "styleZoneBoutonBase"},
    {"styleImageBoutonEtats",    "styleImageBoutonBase"},
    {"styleCadreExterieurEtats", "styleCadreExterieurBase"},
    /*  — familles restantes du registre : les deux surfaces stateful
     * du menu principal et la languette d'un Champ Onglet. Leur absence
     * aveuglait la resolution des etats sur les DEUX formes (bloc `Etats =
     * { Survol = ... }` comme cascade `..Etats[survol]..`). */
    {"styleMenuPrincipalBarreEtats",  "styleMenuPrincipalBarreBase"},
    {"styleMenuPrincipalOptionEtats", "styleMenuPrincipalOptionBase"},
    {"styleVoletEtats",               "styleVoletBase"},
    {NULL, NULL}
};

/* Forme cible (langue translate) d'une cle d'etat de bloc DSL, ou NULL si `w`
 * n'en est pas une. Matching accent/casse insensible FR ET EN (round-trip). */
static const char* fmt_style_state_block_target(const char* w, int len) {
    for (const FmtStyleStateBlock* s = FMT_STYLE_STATE_BLOCKS; s->fr; s++) {
        if (ident_equal_ci_accent(w, len, s->fr, (int)strlen(s->fr)) ||
            ident_equal_ci_accent(w, len, s->en, (int)strlen(s->en)))
            return (g_fmt_target_lang == OWL_TRANSLATE_EN) ? s->en : s->fr;
    }
    return NULL;
}

/* baseType PLAT d'un type collection palette (ex styleZoneSaisieEtats ->
 * styleZoneSaisieBase), ou NULL si `coll` n'est pas une collection palette. */
static const char* fmt_style_palette_basetype(const char* coll, int len) {
    for (const FmtStylePaletteBase* p = FMT_STYLE_PALETTE_BASES; p->coll; p++) {
        if (ident_equal_ci_accent(coll, len, p->coll, (int)strlen(p->coll)))
            return p->base;
    }
    return NULL;
}

/* Vrai ssi le receveur `recv` (identifiant precedant un `[`) arme le chemin
 * ETAT du subscript. Dans le modele definitif, le crochet [etat] ne vit que
 * sur le membre `Etats` de la zone de saisie
 * (`..ZoneSaisie..Etats[survol]..X`) : le receveur du `[` est donc toujours
 * `Etats` (EN : `States`). Toute autre cible (tab[i], tab[UneConst],
 * indexation ordinaire) renvoie false : le contenu du `[...]` n'est alors pas
 * canonise en etat. Garde anti-faux-positif complementaire cote caller : le
 * `[` n'est detourne QUE si l'identifiant interne est un etat reel de la
 * palette — un `Etats[i]` coincidentel (variable homonyme indexee) reste un
 * index ordinaire. */
static bool fmt_bracket_is_style_state(const char* recv, int rlen) {
    if (rlen <= 0) return false;
    return ident_equal_ci_accent(recv, rlen, "Etats", 5) ||
           ident_equal_ci_accent(recv, rlen, "States", 6);
}

/* Format a single line's content (after indent), replacing keywords.
 * Writes to buffer. Stops at newline or NUL. Returns pointer past line end. */
/*  — le nom de type `name` designe-t-il chaineMultilingue ?
 * Compare accent/casse-insensiblement (î canon ou i ASCII). */
static bool fmt_type_is_multilingue(const char* name, int len) {
    char buf[24];
    int n = 0;
    for (int i = 0; i < len && n < 23; ) {
        unsigned char c = (unsigned char)name[i];
        if (c == 0xC3 && i + 1 < len) {
            unsigned char c2 = (unsigned char)name[i + 1];
            buf[n++] = (c2 == 0xAE || c2 == 0x8E) ? 'i' : '?';  /* î/Î -> i */
            i += 2;
        } else {
            buf[n++] = (char)tolower(c);
            i++;
        }
    }
    buf[n] = '\0';
    return strcmp(buf, "chainemultilingue") == 0;
}

static const char* format_line_content(const char* p, Buffer* out,
                                       const char* srcBase) {
    bool inString = false;
    bool afterDoubleDot = false;
    bool afterDot = false;          /* set after a single '.' — method/field access */
    /* Receiver context for after-dot lookups: the word that ended just before
     * the dot. Used to decide whether to apply user_fields or builtin
     * property normalisation when the field name exists in both worlds. */
    const char* lastWord = NULL;
    int lastWordLen = 0;
    /*  : type du dernier membre resolu apres un `.`, pour chainer
     * a.b.c (b est un membre de type T -> c se cherche dans T). Pointe dans
     * une table statique (class_members) donc reste valide. Reset des qu'un
     * mot non-membre est emis. */
    const char* receiverType = NULL;
    int receiverTypeLen = 0;
    bool seenPour = false;          /* POUR keyword seen on this line */
    bool seenPourTout = false;      /* POUR TOUT/TOUTE — promotes `de` to DE */
    bool seenPourEquals = false;    /* POUR ... = seen — inside the range expression */
    bool seenCas = false;           /* CAS keyword seen on this line — `CAS X A Y` */
    bool seenPourBorne = false;     /*  : borne de plage À/TO franchie dans un
                                     * en-tete POUR — arme le sens STEP de PAS (vs NOT) */
    bool seenProcOrFunc = false;    /* PROCEDURE/FONCTION seen — visibilite inline feminine */
    bool expectProcName = false;    /*  : prochain IDENTIFIER = nom de proc
                                     * (apres PROCEDURE/FONCTION + visibilite eventuelle) —
                                     * candidat a la bascule d'event en translate */
    /*  — contexte de canonisation des tags BCP 47 (case-insensible).
     * Line-scoped (les 3 constructs vivent sur une ligne dans les usages
     * courants). Les cles de litteraux multilingues et les index de variables
     * chaineMultilingue et les elements du champ Langues sont restaures a leur
     * casse canonique ("fr-fr" -> "fr-FR"). Les VALEURS d'un litteral et toute
     * autre chaine restent verbatim. */
    bool firstWord = true;          /* prochain mot = 1er de la ligne (champ) */
    bool langueFieldCtx = false;    /* ligne `Langues = [...]` : strings = tags */
    bool seenMlTypeOnLine = false;  /* type chaineMultilingue vu sur la ligne */
    bool mlLiteralCtx = false;      /* dans un litteral `{...}` multilingue */
    bool mlIndexPending = false;    /* prochaine string = index d'une var multi. */
    bool seenCopuleOnLine = false;  /*  : zone d'annotation de type ouverte */
    /*  : le mot en cours d'emission est-il le 1er de la ligne ? `firstWord`
     * est consomme (et remis a faux) bien avant les branches d'emission, d'ou ce
     * relais. Un article (`un`, `une`, `a`, `an`) n'ouvre jamais une ligne : en
     * tete de ligne, ces mots sont des NOMS. */
    bool wordIsFirstOnLine = false;
    /*  — curseur de type de la cascade `..` en cours (index registre du
     * type compose porte par le dernier segment resolu), -1 hors cascade ou des
     * qu'un maillon ne resout plus. Line-scoped : une cascade tient sur sa
     * ligne. Equivalent lineaire du type_stack indexe sur les accolades. */
    int cascadeType = -1;
    /*  — la propriete `receveur..Prop` d'un PREMIER saut se traduit
     * (langue cible) meme si le type du receveur est inconnu ; mais un SEGMENT
     * de CASCADE non resolu (`..Style..Libellé..`) reste verbatim (« pas de
     * devinette », cf. translate_test HORS_BLOC). ddPropEmitted retient si le
     * dernier token emis etait un `..Prop` ; ddIsContinuation, arme a la
     * detection du `..`, dit si le segment courant continue une cascade. */
    bool ddPropEmitted = false;
    bool ddIsContinuation = false;

    while (*p != '\0' && *p != '\n') {
        bool ddPrevWasProp = ddPropEmitted;
        ddPropEmitted = false;
        /* Handle // comments: copy rest of line verbatim */
        if (!inString && *p == '/' && *(p+1) == '/') {
            while (*p != '\0' && *p != '\n') {
                buf_putc(out, *p++);
            }
            break;
        }

        /* Handle block comments (slash-star) */
        if (!inString && *p == '/' && *(p+1) == '*') {
            buf_putc(out, *p++);
            buf_putc(out, *p++);
            while (*p != '\0' && !(*p == '*' && *(p+1) == '/')) {
                buf_putc(out, *p++);
            }
            if (*p == '*') { buf_putc(out, *p++); buf_putc(out, *p++); }
            continue;
        }

        /* Handle string literals: copy verbatim, sauf tag BCP 47 en contexte
         * langue dont la casse est restauree en forme canonique. */
        if (*p == '"' && !inString) {
            const char* contentStart = p + 1;
            const char* q = contentStart;
            /* Chaines BRUTES (cf. scanner.c::scan_string) : backslash 100 %
             * litteral, aucun echappement C. Seul echappement = doublage ""
             * (deux guillemets = un guillemet litteral, on reste dans la
             * chaine). Un " ferme donc SAUF s'il est suivi d'un autre ". */
            while (*q != '\0' && *q != '\n') {
                if (*q == '"') {
                    if (q[1] == '"') { q += 2; continue; }  /* doublage "" */
                    break;  /* vrai guillemet fermant */
                }
                q++;
            }
            bool closed = (*q == '"');
            int contentLen = (int)(q - contentStart);

            /*  — canonise la casse du tag ? Champ Langues ou index d'une
             * variable multilingue -> oui. Litteral multilingue -> uniquement la
             * cle (string suivie de ':'), jamais la valeur. */
            bool doCanon = false;
            if (closed && contentLen > 0) {
                if (langueFieldCtx || mlIndexPending) {
                    doCanon = true;
                } else if (mlLiteralCtx) {
                    const char* r = q + 1;
                    while (*r == ' ' || *r == '\t') r++;
                    if (*r == ':') doCanon = true;
                }
            }
            mlIndexPending = false;  /* la string d'index est consommee ici */

            buf_putc(out, '"');
            if (doCanon) {
                /* owl_bcp47_canon attend un tag NUL-termine : materialise le
                 * slice [contentStart, contentLen[ dans un buffer local. */
                char rawTag[96];
                int rawLen = contentLen < (int)sizeof(rawTag) - 1
                                 ? contentLen : (int)sizeof(rawTag) - 1;
                memcpy(rawTag, contentStart, (size_t)rawLen);
                rawTag[rawLen] = '\0';
                char canonBuf[96];
                owl_bcp47_canon(rawTag, canonBuf, sizeof(canonBuf));
                buf_puts(out, canonBuf);
            } else {
                buf_write(out, contentStart, contentLen);
            }
            if (closed) { buf_putc(out, '"'); p = q + 1; }
            else        { p = q; }   /* non fermee : la ligne se termine */
            continue;
        }

        /* Handle backtick string OWL : contenu OPAQUE (aucune canonisation
         * casse/accent/mot-cle a l'interieur), a parite avec les chaines
         * `"..."`. Le doublage `` est un backtick litteral : on reste dans
         * la chaine. Line-scoped comme le handler `"` : si le litteral n'est
         * pas ferme sur la ligne (backtick multi-ligne), on copie jusqu'a la
         * fin de ligne et la machinerie in_backtick du caller (owl_format)
         * prend le relais verbatim pour les lignes suivantes. */
        if (!inString && *p == '`') {
            buf_putc(out, *p++);  /* backtick ouvrant */
            while (*p != '\0' && *p != '\n') {
                if (*p == '`') {
                    if (*(p+1) == '`') {  /* doublage `` : backtick litteral */
                        buf_putc(out, *p++);
                        buf_putc(out, *p++);
                        continue;
                    }
                    buf_putc(out, *p++);  /* backtick fermant */
                    break;
                }
                buf_putc(out, *p++);
            }
            continue;
        }

        /* Handle [[ ... ]] : slice/buffer extraction `s[[i À j]]` OU litteral
         * matrice `[[1.0, 2.0], [3.0, 4.0]]`. Le contenu est du CODE (indices,
         * separateur de borne, nombres), pas une chaine : on le recopie verbatim
         * SAUF le separateur de plage `a`/`to`/`à`/`À`, canonise vers
         * `À`/`TO` comme la borne d'iteration. Un `a`/`to` isole dedans est
         * toujours le separateur (mots reserves, jamais un index ni un element de
         * matrice), donc aucune desambiguisation contextuelle a poser ; les
         * litteraux matrice (nombres only) ne contiennent jamais ce token. */
        if (!inString && *p == '[' && *(p+1) == '[') {
            buf_putc(out, *p++);
            buf_putc(out, *p++);
            while (*p != '\0' && !(*p == ']' && *(p+1) == ']')) {
                if (is_word_char(*p)) {
                    const char* ws = p;
                    while (is_word_char(*p)) p++;
                    int wl = (int)(p - ws);
                    int sep = fmt_slice_range_sep(ws, wl);
                    if (sep == 1) {
                        buf_puts(out, g_fmt_target_lang == OWL_TRANSLATE_EN
                                          ? "TO" : "\xC3\x80"); /* À (U+00C0) */
                    } else if (sep == 2) {
                        buf_puts(out, g_fmt_target_lang == OWL_TRANSLATE_FR
                                          ? "\xC3\x80" : "TO");
                    } else {
                        /*  — le reste du contenu du [[ ]] est du CODE :
                         * on canonise chaque mot nu comme partout ailleurs
                         * (find_native_norm), pour que `taille(ch)` dans un
                         * index devienne `Taille(ch)`. NULL (variable, nombre,
                         * element de litteral matrice) -> verbatim comme avant :
                         * `[[1.0, 2.0]]` reste byte-identique (aucun chiffre
                         * n'est une native). Le separateur de borne (sep 1/2)
                         * est traite AVANT, il ne passe jamais ici. */
                        char asc[128];
                        int al = scanner_strip_accents(ws, wl, asc, sizeof(asc));
                        const char* norm = NULL;
                        if (al > 0 && al < (int)sizeof(asc)) {
                            for (int i = 0; i < al; i++)
                                asc[i] = (char)tolower((unsigned char)asc[i]);
                            asc[al] = '\0';
                            norm = find_native_norm(asc, al, ws[0]);
                        }
                        if (norm != NULL) buf_puts(out, norm);
                        else buf_write(out, ws, wl);
                    }
                } else {
                    buf_putc(out, *p++);
                }
            }
            if (*p == ']') { buf_putc(out, *p++); buf_putc(out, *p++); }
            continue;
        }

        /*  — index d'une variable chaineMultilingue : `m["fr-fr"]`.
         * Le receveur (lastWord) est-il declare chaineMultilingue ? Si oui, la
         * prochaine string (l'index) est un tag a canoniser. On arme juste le
         * flag ; le `[` est emis par le traitement standard. */
        if (!inString && *p == '[' && *(p+1) != '[' &&
            lastWord != NULL && lastWordLen > 0) {
            int tlen = 0;
            const char* tname = find_var_type_name(lastWord, lastWordLen, &tlen);
            if (tname != NULL && fmt_type_is_multilingue(tname, tlen)) {
                mlIndexPending = true;
            }
        }

        /*  — sous-etat de style `..ZoneSaisie..Etats[etat]..prop`.
         * L'identifiant entre crochets est un etat de la palette
         * (survol/focus/grisé/lectureSeule), PAS un index ni une constante : on
         * le canonise vers sa forme MINUSCULE de palette. Sans ca `grisé`
         * deriverait vers la constante d'enum homonyme `Grisé` (canon MIXTE,
         * incoherent). Le receveur (`Etats`) est retrouve par scan arriere borne
         * a la ligne. Garde anti-faux-positif : on ne detourne le `[` QUE si
         * l'interieur est un etat REEL de la palette — un index ordinaire
         * (`tab[i]`, `tab[UneConst]`, `Etats[maVar]`) tombe dans le traitement
         * standard, sa canonisation propre est preservee. */
        if (!inString && *p == '[' && *(p+1) != '[') {
            const char* q = p;
            while (q > srcBase && (q[-1] == ' ' || q[-1] == '\t')) q--;
            const char* rend = q;
            while (q > srcBase && q[-1] != '\n' && is_word_char(q[-1])) q--;
            int rlen = (int)(rend - q);
            if (rlen > 0 && fmt_bracket_is_style_state(q, rlen)) {
                const char* ip = p + 1;
                while (*ip == ' ' || *ip == '\t') ip++;
                const char* wstart = ip;
                while (is_word_char(*ip)) ip++;
                int wlen = (int)(ip - wstart);
                /*  : en translate, l'etat suit la langue cible
                 * (`[survol]` <-> `[hover]`), comme la cle de bloc `Survol`
                 * <-> `Hover`. Hors translate, la langue ecrite est preservee. */
                const char* sc = (g_fmt_target_lang != OWL_TRANSLATE_NONE)
                    ? fmt_style_state_target(wstart, wlen)
                    : fmt_style_state_canon(wstart, wlen);
                if (sc != NULL) {
                    buf_putc(out, '[');
                    buf_puts(out, sc);
                    p = ip;
                    lastWord = NULL; lastWordLen = 0;
                    afterDot = false; afterDoubleDot = false;
                    receiverType = NULL; receiverTypeLen = 0;
                    /*  : le crochet descend de la COLLECTION d'etats
                     * (styleXxxEtats, sans champs au registre) vers son type
                     * PLAT — meme bascule que le sous-bloc `Survol = { … }` de
                     * la forme bloc, via la meme table de palettes. */
                    if (cascadeType >= 0 && owl_fmt_canon) {
                        const char* coll = owl_fmt_canon->type_ascii(cascadeType);
                        const char* base = coll
                            ? fmt_style_palette_basetype(coll, (int)strlen(coll))
                            : NULL;
                        cascadeType = base
                            ? fmt_composite_type_index(base, (int)strlen(base))
                            : -1;
                    }
                    continue;
                }
            }
        }

        /* Detect ".." for property normalization */
        if (!inString && *p == '.' && *(p+1) == '.') {
            buf_putc(out, *p++);
            buf_putc(out, *p++);
            afterDoubleDot = true;
            afterDot = false;
            /*  — ce `..` continue-t-il une cascade (le token juste avant
             * etait deja un `..Prop`) ? Sert a distinguer `receveur..Prop` (1er
             * saut, traduisible) d'un maillon de cascade non resolu (verbatim). */
            ddIsContinuation = ddPrevWasProp;
            continue;
        }

        /* Detect single "." for method/field access — suppresses keyword
         * canonicalization so `c.Fin()` stays `c.Fin()` and not `c.FIN()`. */
        if (!inString && *p == '.' && *(p+1) != '.') {
            buf_putc(out, *p++);
            afterDot = true;
            continue;
        }

        /* Detect "=" inside POUR line (marker for range expression start) */
        if (!inString && *p == '=' && seenPour && !seenPourEquals) {
            /* Skip compound/equality ops (==, >=, <=, <>) */
            if (*(p+1) != '=' && *(p-1) != '!' && *(p-1) != '<' &&
                *(p-1) != '>' && *(p-1) != '+' && *(p-1) != '-' &&
                *(p-1) != '*' && *(p-1) != '/') {
                seenPourEquals = true;
            }
        }

        /* Identifier or keyword: collect and check */
        if (is_word_char(*p)) {
            const char* wordStart = p;
            while (is_word_char(*p)) p++;
            int wordLen = (int)(p - wordStart);

            /* Normalize to lowercase ASCII for keyword matching
             * (handles UTF-8 accented chars like à → a) */
            char normBuf[128];
            int normLen = 0;
            bool hasUtf8 = false;
            for (int j = 0; j < wordLen && normLen < 126; ) {
                unsigned char ch = (unsigned char)wordStart[j];
                if (ch < 0x80) {
                    normBuf[normLen++] = (char)tolower(ch);
                    j++;
                } else if ((ch & 0xE0) == 0xC0 && j + 1 < wordLen) {
                    /* 2-byte UTF-8 */
                    uint16_t cp = ((ch & 0x1F) << 6) |
                                  ((unsigned char)wordStart[j+1] & 0x3F);
                    hasUtf8 = true;
                    if (cp >= 0xC0 && cp <= 0xC6) normBuf[normLen++] = 'a';      /* A with accent */
                    else if (cp >= 0xE0 && cp <= 0xE6) normBuf[normLen++] = 'a'; /* a with accent */
                    else if (cp == 0xC7) normBuf[normLen++] = 'c';               /* C cedilla */
                    else if (cp == 0xE7) normBuf[normLen++] = 'c';               /* c cedilla */
                    else if (cp >= 0xC8 && cp <= 0xCB) normBuf[normLen++] = 'e'; /* E with accent */
                    else if (cp >= 0xE8 && cp <= 0xEB) normBuf[normLen++] = 'e'; /* e with accent */
                    else if (cp >= 0xCC && cp <= 0xCF) normBuf[normLen++] = 'i'; /* I with accent */
                    else if (cp >= 0xEC && cp <= 0xEF) normBuf[normLen++] = 'i'; /* i with accent */
                    else if (cp >= 0xD2 && cp <= 0xD6) normBuf[normLen++] = 'o'; /* O with accent */
                    else if (cp >= 0xF2 && cp <= 0xF6) normBuf[normLen++] = 'o'; /* o with accent */
                    else if (cp >= 0xD9 && cp <= 0xDC) normBuf[normLen++] = 'u'; /* U with accent */
                    else if (cp >= 0xF9 && cp <= 0xFC) normBuf[normLen++] = 'u'; /* u with accent */
                    else normBuf[normLen++] = '?';
                    j += 2;
                } else {
                    normBuf[normLen++] = '?';
                    j++;
                }
            }
            normBuf[normLen] = '\0';

            /*  — detection de contexte tag BCP 47 (avant les branches
             * d'emission). Le 1er mot `Langues`/`Languages` marque le champ
             * projet (ses strings sont des tags) ; le type `chaineMultilingue`
             * sur la ligne arme le litteral `{...}` qui suit. */
            {
                bool wasFirstWord = firstWord;
                firstWord = false;
                wordIsFirstOnLine = wasFirstWord;  /*  */
                if (fmt_word_is_copule_any(normBuf, normLen))
                    seenCopuleOnLine = true;
                if (normLen == 17 &&
                    strcmp(normBuf, "chainemultilingue") == 0) {
                    seenMlTypeOnLine = true;
                }
                if (wasFirstWord &&
                    ((normLen == 7 && strcmp(normBuf, "langues") == 0) ||
                     (normLen == 9 && strcmp(normBuf, "languages") == 0))) {
                    langueFieldCtx = true;
                }
            }

            /*  — elision `d'` en position de connecteur de type de
             * collection (`tableau d'entiers`). Le scanner isole `d`, puis `'`,
             * puis le type ; sans traitement le `d'` traversait la traduction
             * (`array d'integer`). En cible EN on emet le connecteur `of ` ; le
             * type qui suit est traduit par le path standard. Hors translate et
             * en cible FR, la graphie elidee ecrite est preservee (aucune
             * interception). */
            if (g_fmt_target_lang == OWL_TRANSLATE_EN &&
                normLen == 1 && normBuf[0] == 'd' && *p == '\'' &&
                fmt_prev_word_is_coll_noun(lastWord, lastWordLen)) {
                buf_puts(out, "of ");
                p++;  /* consomme l'apostrophe ; le type suit sans espace source */
                lastWord = wordStart; lastWordLen = wordLen;
                continue;
            }

            /* Special: standalone accented "a" (à/À) → keyword À.
             *  : la borne de plage suit la langue cible en translate.
             * `à` est toujours le keyword de plage TOKEN_A (jamais un article,
             * cf. scanner) -> cible EN = TO. Hors translate / cible FR = À. */
            if (normLen == 1 && normBuf[0] == 'a' && hasUtf8) {
                if (seenPour) seenPourBorne = true;  /*  */
                buf_puts(out, g_fmt_target_lang == OWL_TRANSLATE_EN
                                  ? "TO" : "\xC3\x80"); /* À (U+00C0) */
                continue;
            }

            /* POUR / CAS context: bare "A"/"a" (single ASCII char) is the
             * range keyword TOKEN_A. Convert to accented À for
             * visual canon FR. `CAS X A Y` (pattern de plage SELON) bénéficie
             * du même traitement que `POUR i = 1 A N`.  : cible EN = TO. */
            if (normLen == 1 && normBuf[0] == 'a' && !hasUtf8 &&
                (seenPourEquals || seenCas)) {
                if (seenPour) seenPourBorne = true;  /*  */
                buf_puts(out, g_fmt_target_lang == OWL_TRANSLATE_EN
                                  ? "TO" : "\xC3\x80"); /* À */
                continue;
            }
            /*  : meme borne de plage, ecrite en EN (`to`). Le scanner
             * mappe `to` -> TOKEN_A (comme `a`), mais bilingue-preservant : la
             * forme EN reste EN, canon `TO` (miroir FR `À`). Desambiguisation
             * par la forme ecrite, pas par la langue du contexte.  :
             * cible FR = À (bascule de la borne EN->FR). */
            if (normLen == 2 && normBuf[0] == 't' && normBuf[1] == 'o' &&
                (seenPourEquals || seenCas)) {
                if (seenPour) seenPourBorne = true;  /*  */
                buf_puts(out, g_fmt_target_lang == OWL_TRANSLATE_FR
                                  ? "\xC3\x80" : "TO");
                continue;
            }

            /*  — DSL des types composes (themes/styles) : resolution
             * TYPE-AWARE en mode translate. Gate translate + hors acces membre
             * (afterDot/afterDoubleDot restent au chemin membre habituel).
             *   (a) nom de type compose en position de declaration suivi de
             *       `= {` -> forme cible + arme pending_type pour le `{`.
             *   (b) nom de champ (LHS d'un `=`) dans un bloc du type courant ->
             *       forme cible via les name_en de CE type ; si `= {`, arme
             *       pending_type = type du sous-bloc (descente recursive).
             *   (c) valeur RHS membre d'enum dans un bloc compose -> forme
             *       cible (dégradéRadial -> gradientRadial).
             * Le bloc `Thème` (corps indente, pas `{ }`) est traite juste apres. */
            /* Connecteur de type : est/un/une/is (fmt_after_type_connector)
             * + article EN a/an (source EN, indispensable au round-trip)
             * + connecteur d'element de collection de/of (`tableau de <Type>` /
             *   `array of <Type>`, ). Sans de/of, le type composite d'une
             *   collection ne passe jamais par la resolution lang-aware (case a)
             *   et tombe sur la canonicalisation accentuante par defaut : un
             *   `array of Detection` (EN) ressortait `array of Détection` (FR).
             *  : calcule hors du gate translate, la branche (a) en a
             * besoin dans les DEUX modes. */
            bool typeConn = fmt_after_type_connector(lastWord, lastWordLen);
            if (!typeConn && lastWord && lastWordLen >= 1 && lastWordLen <= 2) {
                char lw[4];
                int ll = scanner_strip_accents(lastWord, lastWordLen, lw, sizeof lw);
                if (ll >= 1 && ll < (int)sizeof lw) {
                    for (int i = 0; i < ll; i++)
                        lw[i] = (char)tolower((unsigned char)lw[i]);
                    lw[ll] = '\0';
                    if (strcmp(lw, "a") == 0 || strcmp(lw, "an") == 0 ||
                        strcmp(lw, "de") == 0 || strcmp(lw, "of") == 0)
                        typeConn = true;
                }
            }
            /* Peek du prochain caractere significatif apres le mot. */
            bool followedByAssign = false, followedByBrace = false;
            {
                const char* pk = p;
                while (*pk == ' ' || *pk == '\t') pk++;
                followedByAssign = (*pk == '=' && *(pk + 1) != '=');
                if (followedByAssign) {
                    const char* pk2 = pk + 1;
                    while (*pk2 == ' ' || *pk2 == '\t') pk2++;
                    followedByBrace = (*pk2 == '{');
                }
            }

            if (!afterDot && !afterDoubleDot) {
                /* (a) type compose en POSITION DE TYPE, avec ou sans `= {`.
                 *
                 *  — la garde etait `followedByBrace`, donc seule la
                 * forme DSL `X est un styleBouton = {` basculait : la
                 * declaration nue `r est un httpRequête` n'a jamais de `{` et
                 * restait donc integralement en FR. Mesure : 68 des 81 types du
                 * registre ont une paire FR/EN distincte, aucune n'etait emise.
                 *
                 * La resolution passe par le REGISTRE (fmt_composite_type_index
                 * / _name), pas par des entrees fmt_keywords[] : tout type
                 * ajoute au registre bascule sans edit du formateur, et aucun
                 * nom n'est pose en table — donc aucun risque de masquer un
                 * homonyme (`Champ`, `Style`, `Cadre`, `Police`, `Couleur`…
                 * sont homonymes d'une native, d'un champ de style ou d'un
                 * mot-cle contextuel ; une entree `couleur/Color` en table
                 * traduirait l'appel homonyme partout ailleurs, exactement le
                 * piege paye par  avec la constante TAB).
                 *
                 * Discrimination : position de type (connecteur est/un/une/is/
                 * a/an) ET pas une position d'appel — `HTTPRequête("u")` reste
                 * la NATIVE (double type-vs-fonction, invariant 5). Ce chemin
                 * corrige aussi le sens EN->FR, ou `is a httpRequest` etait
                 * capte par la native homonyme et rendait `est une HTTPRequête`.
                 *
                 * `pending_type` reste arme au seul cas `= {` : c'est le
                 * contexte de descente dans le corps du litteral, qui n'a de
                 * sens qu'en traduction. */
                if (typeConn && !is_call_position(p)) {
                    const char* tf = fmt_composite_type_lang_form(wordStart, wordLen);
                    if (tf != NULL) {
                        buf_puts(out, tf);
                        if (g_fmt_st && followedByBrace &&
                            g_fmt_target_lang != OWL_TRANSLATE_NONE)
                            g_fmt_st->pending_type =
                                fmt_composite_type_index(wordStart, wordLen);
                        lastWord = wordStart; lastWordLen = wordLen;
                        continue;
                    }
                }
            }

            if (g_fmt_target_lang != OWL_TRANSLATE_NONE && !afterDot &&
                !afterDoubleDot) {
                /* (b) champ LHS dans un bloc compose. Un identifiant en position
                 *     de champ (suivi de `=`) a l'interieur d'un bloc `{ }` de
                 *     type compose n'est JAMAIS un appel natif ni un keyword de
                 *     controle : on le resout contre le type courant (forme
                 *     cible), sinon on l'emet VERBATIM. Ce verbatim est
                 *     essentiel : sans lui, un champ homonyme d'une native
                 *     (Arrondi/Round, Taille/Length) serait traduit en cette
                 *     native et le fichier traduit ne compilerait plus. Il
                 *     couvre aussi les sous-blocs dont le type exact est inconnu
                 *     (ex. etats d'un style : palette sans FIELDS registry) :
                 *     leurs champs restent en forme source (compile bilingue). */
                if (followedByAssign && g_fmt_st && g_fmt_st->type_sp > 0) {
                    int curType = fmt_current_composite_type();
                    int subType = -1;
                    const char* fld = (curType >= 0)
                        ? fmt_composite_field_resolve(curType, wordStart, wordLen,
                                                      &subType)
                        : NULL;
                    if (fld != NULL) {
                        buf_puts(out, fld);
                    } else {
                        /*  — le type courant peut etre une COLLECTION
                         * palette (styleXxxEtats), sans FIELDS registry : ses
                         * cles sont des ETATS (Survol/Focus/Appuyé/Grisé/
                         * LectureSeule). On les traduit via la palette et on
                         * arme le baseType PLAT (styleXxxBase) comme type du
                         * sous-bloc, pour que ses champs internes (Couleur,
                         * CouleurFond, Cadre, Ombre...) se resolvent comme tout
                         * autre compose. Hors palette / cle inconnue : verbatim
                         * (champ homonyme d'une native reste intact -> compile
                         * bilingue). */
                        const char* coll = (curType >= 0 && owl_fmt_canon)
                            ? owl_fmt_canon->type_ascii(curType) : NULL;
                        const char* base = coll
                            ? fmt_style_palette_basetype(coll, (int)strlen(coll))
                            : NULL;
                        const char* st = base
                            ? fmt_style_state_block_target(wordStart, wordLen)
                            : NULL;
                        if (st != NULL) {
                            buf_puts(out, st);
                            subType = fmt_composite_type_index(
                                          base, (int)strlen(base));
                        } else {
                            buf_write(out, wordStart, wordLen);
                        }
                    }
                    if (followedByBrace && g_fmt_st)
                        g_fmt_st->pending_type = subType;
                    lastWord = wordStart; lastWordLen = wordLen;
                    continue;
                }

                /* (c) valeur RHS membre d'enum dans un bloc compose (context-free :
                 *     nom d'enum member globalement unique). Actif des qu'on est
                 *     dans un bloc `{ }` (type_sp>0), meme si le type exact du
                 *     niveau est inconnu (etats). dégradéRadial -> gradientRadial. */
                if (!followedByAssign && g_fmt_st && g_fmt_st->type_sp > 0) {
                    const char* ev = fmt_enum_const_target(wordStart, wordLen);
                    if (ev != NULL) {
                        buf_puts(out, ev);
                        lastWord = wordStart; lastWordLen = wordLen;
                        continue;
                    }
                }

                /* Bloc `Thème` : opener DSL (corps indente), pas une entree
                 * registry. `<Nom> est un Thème` -> Theme/Thème selon la cible.
                 * Restreint a la position de type (pas le receveur `theme`,
                 * cf. ). */
                if (normLen == 5 && strcmp(normBuf, "theme") == 0 && typeConn) {
                    buf_puts(out, g_fmt_target_lang == OWL_TRANSLATE_EN
                                      ? "Theme" : "Th\xc3\xa8me");
                    lastWord = wordStart; lastWordLen = wordLen;
                    continue;
                }

                /*  — bloc collection de procedures : opener DSL
                 * bilingue, meme traitement que `Thème`. */
                if (typeConn
                    && ((normLen == 22
                         && strcmp(normBuf, "collectiondeprocedures") == 0)
                     || (normLen == 19
                         && strcmp(normBuf, "procedurecollection") == 0))) {
                    buf_puts(out, g_fmt_target_lang == OWL_TRANSLATE_EN
                                      ? "ProcedureCollection"
                                      : "CollectionDeProc\xc3\xa9" "dures");
                    lastWord = wordStart; lastWordLen = wordLen;
                    continue;
                }
            }

            /* After ".." -> normalize as property */
            if (afterDoubleDot) {
                afterDoubleDot = false;
                ddPropEmitted = true;  /*  : ce segment est un `..Prop` */
                /*  — marcheur de la cascade `Style..X..Y` ecrite en
                 * ligne. Meme resolution que la forme bloc : les
                 * memes fmt_composite_field_resolve / palette d'etats, sur le
                 * meme registre. Seul le PORTEUR de contexte change — un
                 * curseur de type qui avance de segment en segment au lieu de
                 * la pile indexee sur les accolades, que la forme en ligne n'a
                 * pas. La racine vient de la sorte du controle declare ; un
                 * segment non resolu coupe la chaine (verbatim ensuite), comme
                 * un sous-bloc de type inconnu en forme bloc. */
                if (g_fmt_target_lang != OWL_TRANSLATE_NONE && g_fmt_st) {
                    int curType = cascadeType;
                    if (lastWord && lastWordLen > 0 &&
                        ident_equal_ci_accent(lastWord, lastWordLen, "Style", 5))
                        curType = g_fmt_st->widget_style_root;
                    cascadeType = -1;
                    if (curType >= 0) {
                        int subType = -1;
                        const char* fld = fmt_composite_field_resolve(
                            curType, wordStart, wordLen, &subType);
                        if (fld != NULL) {
                            buf_puts(out, fld);
                            cascadeType = subType;
                            lastWord = wordStart; lastWordLen = wordLen;
                            continue;
                        }
                    }
                }
                /*  : court-circuit user-receiver, meme logique que
                 * la branche afterDot ci-dessous. Si le receveur (lastWord)
                 * est :
                 *   - une variable typee user (Structure/Classe, rkind==1)
                 *   - OU le nom d'une classe user declaree (user_types)
                 * alors `<receiver>..membre` est un acces ou appel user, pas
                 * une propriete d'un type builtin. NE PAS appliquer le canon
                 * registry des proprietes (`Demarre` -> `Démarré` via la prop
                 * Chrono.Démarré, par exemple). Si la classe user a declare
                 * un champ homonyme on emet son canon, sinon verbatim. */
                bool user_receiver_dd = false;
                if (lastWord && lastWordLen > 0) {
                    int rk = find_var_type_kind(lastWord, lastWordLen);
                    if (rk == 1) {
                        user_receiver_dd = true;
                    } else if (user_name_known(g_fmt_st->user_types,
                                               g_fmt_st->user_types_count,
                                               lastWord, lastWordLen)) {
                        /* Receveur est un nom de classe/structure user
                         * (cas `MaClasse..membre`, rare mais legitime). */
                        user_receiver_dd = true;
                    }
                }
                if (user_receiver_dd) {
                    const char* cm = NULL;
                    if (g_fmt_st->class_members_count > 0) {
                        int tlen = 0;
                        const char* tname = find_var_type_name(
                            lastWord, lastWordLen, &tlen);
                        if (tname && tlen > 0)
                            cm = find_class_member_canonical(
                                tname, tlen, wordStart, wordLen);
                        if (!cm)
                            cm = find_class_member_canonical(
                                lastWord, lastWordLen, wordStart, wordLen);
                    }
                    if (cm) buf_puts(out, cm);
                    else    buf_write(out, wordStart, wordLen);
                    lastWord = wordStart; lastWordLen = wordLen;
                    continue;
                }
                /*  — segment de cascade non resolu par le chemin
                 * type-aware ci-dessus : en translate on NE devine PAS contre le
                 * catalogue a plat (`..Style..Libellé..` sur un receveur inconnu
                 * reste verbatim). Seul le PREMIER saut `receveur..Prop` (non
                 * continuation) est traduit. Hors translate, la canonisation
                 * gatekeeper reste inchangee. */
                if (g_fmt_target_lang != OWL_TRANSLATE_NONE && ddIsContinuation) {
                    buf_write(out, wordStart, wordLen);
                    continue;
                }
                /*  : le formatter est gatekeeper canonique (Regle 6 #4).
                 * Hors bloc PROPRIETES, un acces `champ..Prop` sur un receveur
                 * NON-user (le garde user_receiver_dd ci-dessus a deja court-
                 * circuite les structures/classes user) doit aussi canoniser
                 * les proprietes du catalogue widget (..Note, ..Bulle, ..Style,
                 * Tooltip...). find_property_norm ne couvre que les types
                 * primitifs/registry ; on complete par le catalogue DSL, dans
                 * les MEMES conditions (jamais plus agressif). On passe la forme
                 * source brute pour preserver la langue ecrite (FR/EN).
                 *
                 *  — en translate, la propriete de widget prime sur le
                 * champ de type HOMONYME : un `..Libellé` bare designe la caption
                 * (`..Caption`), pas la SORTE Libellé (champ de type -> `Label`).
                 * On interroge donc le catalogue DSL d'abord ; find_property_norm
                 * reste le fallback pour les proprietes de types builtin sans
                 * homonyme DSL (`..Valeur` d'un Chrono...). Hors translate, l'ordre
                 * gatekeeper historique (type puis DSL) est preserve. */
                const char* pnorm = NULL;
                if (g_fmt_target_lang != OWL_TRANSLATE_NONE) {
                    pnorm = find_dsl_prop_norm(wordStart, wordLen);
                    /*  — conteneurs/sous-champs du contexte d'echange
                     * de service (Requête/Réponse/Corps/TypeContenu/Identité),
                     * hors catalogue DSL : bascule vers la langue cible. */
                    if (pnorm == NULL)
                        pnorm = find_service_ctx_prop_norm(wordStart, wordLen);
                    if (pnorm == NULL)
                        pnorm = find_property_norm(normBuf, normLen,
                                                   wordStart, wordLen);
                } else {
                    /* Idempotence : une propriété DSL service déjà écrite dans
                     * sa forme canon accentuée (MoiMême..CodeÉtat) ne doit pas
                     * être rabattue sur un homographe de champ builtin sans
                     * accent (httpRéponse.CodeEtat). find_property_norm matche
                     * accent-insensiblement et écraserait l'accent ; quand ce
                     * qui est écrit coïncide déjà exactement avec un canon DSL,
                     * on le préserve. */
                    const char* dprop = find_dsl_prop_norm(wordStart, wordLen);
                    if (dprop != NULL && (int)strlen(dprop) == wordLen
                        && memcmp(dprop, wordStart, (size_t)wordLen) == 0) {
                        pnorm = dprop;
                    } else {
                        /*  — conteneurs/sous-champs du contexte
                         * d'echange de service. Avant find_property_norm (qui
                         * matche accent-insensiblement) pour preserver l'accent
                         * de la forme ecrite (Réponse reste Réponse). */
                        const char* sctx =
                            find_service_ctx_prop_norm(wordStart, wordLen);
                        if (sctx != NULL) {
                            pnorm = sctx;
                        } else {
                            pnorm = find_property_norm(normBuf, normLen,
                                                       wordStart, wordLen);
                            if (pnorm == NULL) pnorm = dprop;
                        }
                    }
                }
                if (pnorm != NULL) {
                    buf_puts(out, pnorm);
                } else {
                    buf_write(out, wordStart, wordLen);
                }
                continue;
            }

            /* After single "." -> method/field access. Never apply keyword
             * canonicalization (so `c.Fin()` stays `c.Fin()` not `c.FIN()`).
             * Only PascalCase well-known method names. */
            if (afterDot) {
                afterDot = false;
                /*  + revision : receveur `icones` → force lowercase
                 * sur le membre, alignement canon catalog Iconify (les ids
                 * sont stockes lowercase : `save`, `delete`, `home`,
                 * `youtube-2`...).  avait initialement "preserve la
                 * casse user" pour empecher le formatter de transformer
                 * `delete` en `Delete` (canon native EN). Mais le vrai canon
                 * = catalog lowercase. La revision force lowercase ASCII
                 * directement, ce qui aligne `icones.Save` → `icones.save`,
                 * `icones.SAVE` → `icones.save`, etc. Les chiffres,
                 * underscore et autre ASCII non-alpha restent inchanges
                 * (`icones.youtube_2` reste `icones.youtube_2`). */
                {
                    char rstrip[32];
                    int rlen = scanner_strip_accents(lastWord, lastWordLen,
                                                     rstrip, sizeof(rstrip));
                    if (rlen > 0 && rlen < (int)sizeof(rstrip)) {
                        rstrip[rlen] = '\0';
                        /* lowercase comparison */
                        bool match = true;
                        const char* ref = "icones";
                        if (rlen != 6) match = false;
                        else {
                            for (int i = 0; i < 6 && match; ++i) {
                                char c = rstrip[i];
                                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                                if (c != ref[i]) match = false;
                            }
                        }
                        if (match) {
                            /* Emit le membre en lowercase ASCII (alignement
                             * canon catalog). */
                            for (int i = 0; i < wordLen; ++i) {
                                unsigned char ch = (unsigned char)wordStart[i];
                                if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch + 32);
                                buf_putc(out, (char)ch);
                            }
                            receiverType = NULL; receiverTypeLen = 0;
                            lastWord = wordStart; lastWordLen = wordLen;
                            continue;
                        }
                    }
                }
                /*  : court-circuit user-receiver. Si le receveur est une
                 * variable typee user (Structure/Classe) ou une classe declaree
                 * avec un membre du nom de wordStart, on resout en priorite via
                 * les tables user (user_fields/class_members) — JAMAIS via le
                 * catalogue global de methods/props. La canon builtin (`Nom`,
                 * `Type`, `Valeur` exposes par xmlNoeud/xmlAttribut/Chrono) ne
                 * doit PAS ecraser un champ user homonyme. */
                bool user_receiver = false;
                if (lastWord && lastWordLen > 0) {
                    int rk = find_var_type_kind(lastWord, lastWordLen);
                    if (rk == 1) {
                        user_receiver = true;
                    } else if (user_name_known(g_fmt_st->user_types,
                                               g_fmt_st->user_types_count,
                                               lastWord, lastWordLen)) {
                        /*  : receveur = nom d'une classe/structure user
                         * declaree (cas `MaClasse.membre`) — miroir du garde
                         * afterDoubleDot. Resoudre via tables user, pas via le
                         * catalogue builtin de methods/props homonymes. */
                        user_receiver = true;
                    } else if (rk == 0) {
                        /* Receveur de type inconnu (var typeKind=0) : si le mot
                         * matche un user_field declare, on prefere le canon user
                         * (regle "user-declared local gagne sur builtin homonyme",
                         * cf. doctrine canon_wl_red_flags section 4). */
                        if (user_name_known(g_fmt_st->user_fields,
                                            g_fmt_st->user_fields_count,
                                            wordStart, wordLen))
                            user_receiver = true;
                    }
                    /* Receveur classe (cVoiture.x ou v.x avec v est un cVoiture) :
                     * si class_members connait wordStart pour cette classe, on
                     * est sur un member access user — pas une method. */
                    if (!user_receiver && g_fmt_st->class_members_count > 0) {
                        int tlen = 0;
                        const char* tname = find_var_type_name(lastWord, lastWordLen, &tlen);
                        if (tname && tlen > 0 &&
                            class_member_exists(tname, tlen, wordStart, wordLen)) {
                            user_receiver = true;
                        } else if (class_member_exists(lastWord, lastWordLen,
                                                       wordStart, wordLen)) {
                            user_receiver = true;
                        } else if (receiverType && receiverTypeLen > 0 &&
                                   class_member_exists(receiverType, receiverTypeLen,
                                                       wordStart, wordLen)) {
                            user_receiver = true;
                        }
                    }
                }
                const char* mnorm = user_receiver ? NULL
                                                  : find_method_norm(normBuf, normLen);
                /*  : ne JAMAIS desaccentuer via une forme native
                 * homonyme. Si la source porte un accent que le canon native
                 * matche n'a pas (cas d'une methode native enregistree en
                 * ASCII, ex. binding `Reinitialise`, homonyme d'une methode
                 * user `Réinitialise` sur un receveur non resolu), appliquer
                 * `mnorm` stripperait l'accent — interdit. On garde verbatim.
                 * Le re-accentuage reste autorise (source ASCII -> canon
                 * accentue) : on ne coupe que le sens "perte d'accent". */
                if (mnorm != NULL) {
                    bool srcAccent = false;
                    for (int i = 0; i < wordLen; i++) {
                        if ((unsigned char)wordStart[i] >= 0x80) { srcAccent = true; break; }
                    }
                    if (srcAccent && !name_has_accent(mnorm)) {
                        mnorm = NULL;  /* refuse le strip — chute vers verbatim */
                    }
                }
                if (mnorm != NULL) {
                    /*  : en translate, bascule vers la forme
                     * partenaire si la methode est une native bilingue. */
                    const char* mpart = fmt_method_partner(mnorm, (int)strlen(mnorm));
                    buf_puts(out, mpart != NULL ? mpart : mnorm);
                    /* Update receiver context before continuing. */
                    receiverType = NULL; receiverTypeLen = 0;
                    lastWord = wordStart; lastWordLen = wordLen;
                    continue;
                }
                /* Class member access. The receiver class is resolved, in
                 * priority order:
                 *   1. receiverType — type du membre precedent dans une chaine
                 *      a.b.c, permet de descendre la composition.
                 *   2. type declare de la variable receveuse (`c est un cX`).
                 *   3. le mot lui-meme s'il EST une classe (acces constante
                 *      `cConfig.NOM`).
                 * Une fois le membre resolu, receiverType est positionne au
                 * type declare de CE membre pour le maillon suivant. */
                if (lastWord && lastWordLen > 0 && g_fmt_st->class_members_count > 0) {
                    const char* cls = NULL;
                    int clsLen = 0;
                    if (receiverType && receiverTypeLen > 0 &&
                        class_member_exists(receiverType, receiverTypeLen,
                                            wordStart, wordLen)) {
                        cls = receiverType; clsLen = receiverTypeLen;
                    } else {
                        int tlen = 0;
                        const char* tname = find_var_type_name(lastWord, lastWordLen, &tlen);
                        if (tname && tlen > 0 &&
                            class_member_exists(tname, tlen, wordStart, wordLen)) {
                            cls = tname; clsLen = tlen;
                        } else if (class_member_exists(lastWord, lastWordLen,
                                                       wordStart, wordLen)) {
                            /* Receiver is the class name directly. */
                            cls = lastWord; clsLen = lastWordLen;
                        }
                    }
                    if (cls) {
                        const char* cm = find_class_member_canonical(cls, clsLen,
                                                                     wordStart, wordLen);
                        if (cm) buf_puts(out, cm);
                        else    buf_write(out, wordStart, wordLen);
                        /* Avance la chaine : type de ce membre = prochaine classe. */
                        int mtLen = 0;
                        const char* mt = find_class_member_decl_type(
                            cls, clsLen, wordStart, wordLen, &mtLen);
                        receiverType = mt; receiverTypeLen = mt ? mtLen : 0;
                        lastWord = wordStart; lastWordLen = wordLen;
                        continue;
                    }
                    /* Maillon non resolu dans la chaine : on coupe la chaine
                     * (verbatim pour ce maillon et les suivants). */
                    receiverType = NULL; receiverTypeLen = 0;
                }
                /* Route by receiver type so we don't false-positive across
                 * user structs and builtin types that share field names. */
                int rkind = 0;
                if (lastWord && lastWordLen > 0)
                    rkind = find_var_type_kind(lastWord, lastWordLen);
                /*  — aucune canonisation de PROPRIETE sur ce chemin.
                 * Une propriete s'ecrit au double point ; apres un point
                 * simple, le nom n'en designe pas une. Restaurer sa casse et
                 * ses accents reviendrait a benir une ecriture que le
                 * compilateur refuse — le gatekeeper canonique n'a pas a
                 * cautionner un operateur faux. Les membres user, les
                 * methodes et les namespaces gardent leur canon, traites plus
                 * haut.
                 *
                 * Receveur user (rkind 1) ou de type inconnu (rkind 0) : seule
                 * la declaration user compte, et elle compte meme quand elle
                 * est deja canonique (`c.nom` reste `c.nom` alors que `nom`
                 * est aussi un champ de Resultat). */
                const char* chosen = NULL;
                if (rkind == 0 || rkind == 1) {
                    chosen = find_user_canonical(g_fmt_st->user_fields,
                                                 g_fmt_st->user_fields_count,
                                                 wordStart, wordLen);
                }
                if (chosen != NULL) buf_puts(out, chosen);
                else                buf_write(out, wordStart, wordLen);
                receiverType = NULL; receiverTypeLen = 0;
                lastWord = wordStart; lastWordLen = wordLen;
                continue;
            }

            /*  — receveur de namespace `theme` en usage `theme.<style>`
             * (valeur d'une prop `Style = theme.boutonDanger`) : canon =
             * minuscule ASCII `theme`, comme le receveur `icones`. Quelle que
             * soit la source (theme / Theme / thème / Thème). On distingue
             * l'USAGE (suivi d'un `.` simple) de la DECLARATION de bloc
             * (`<Nom> est un Thème`, non suivie de `.`) qui garde sa forme
             * keyword accentuee `Thème` via le chemin block-opener plus bas :
             * ici on n'intercepte QUE le receveur suivi d'un `.` (pas `..`). */
            if (normLen == 5 && strcmp(normBuf, "theme") == 0) {
                const char* pk = p;
                while (*pk == ' ' || *pk == '\t') pk++;
                if (*pk == '.' && *(pk + 1) != '.') {
                    buf_puts(out, "theme");
                    afterDot = false; afterDoubleDot = false;
                    receiverType = NULL; receiverTypeLen = 0;
                    lastWord = wordStart; lastWordLen = wordLen;
                    continue;
                }
            }

            /*  — cle du bloc PROPRIETES d'un `est un Projet`. Les cles
             * (Langues, FenêtreDémarrage, Auteur...) sont des CHAMPS du type
             * builtin Projet, pas des proprietes DSL de controle : elles ne
             * passent ni par find_dsl_prop_norm ni par le keyword pairing. On
             * les resout contre le type meta porte par la passe d'indentation,
             * en position de cle (premier mot, suivi de `=`). Le verbatim en cas
             * de non-match preserve un identifiant user homonyme. */
            if (g_fmt_target_lang != OWL_TRANSLATE_NONE && lastWord == NULL &&
                g_fmt_st && g_fmt_st->enclosing_meta_type >= 0) {
                const char* pk = p;
                while (*pk == ' ' || *pk == '\t') pk++;
                if (*pk == '=' && *(pk + 1) != '=') {
                    int subType = -1;
                    const char* mf = fmt_composite_field_resolve(
                        g_fmt_st->enclosing_meta_type, wordStart, wordLen,
                        &subType);
                    if (mf != NULL) {
                        buf_puts(out, mf);
                        lastWord = wordStart; lastWordLen = wordLen;
                        continue;
                    }
                }
            }

            /* Homonymie KIND widget / PROPRIETE en position d'affectation.
             * Un mot comme `Libellé` est a la fois un KIND de widget (nameEN
             * Label) ET une propriete caption (nameEN Caption). Quand il est
             * le premier mot de la ligne (lastWord == NULL ; la boucle est
             * per-ligne) et suivi d'un `=` d'affectation (pas `==`/`>=`/...),
             * c'est la PROPRIETE qui est visee, pas le kind. On la resout via
             * la table DSL AVANT find_keyword, sinon le pairing keyword emet
             * le nameEN du kind (Label) au lieu de celui de la propriete
             * (Caption), produisant un EN qui ne compile pas.
             *
             * Restreint au cas homonyme (le mot matche un DSL prop ET serait
             * sinon traite comme keyword) pour ne pas toucher les
             * affectations de variables user ordinaires (`x = 0` reste `x`). */
            if (lastWord == NULL && find_keyword(normBuf, normLen) != NULL) {
                const char* pk = p;
                while (*pk == ' ' || *pk == '\t') pk++;
                bool propAssign = (*pk == '=' && *(pk + 1) != '=');
                if (propAssign) {
                    const char* dprop = find_dsl_prop_norm(wordStart, wordLen);
                    if (dprop != NULL) {
                        buf_puts(out, dprop);
                        lastWord = wordStart; lastWordLen = wordLen;
                        continue;
                    }
                }
            }

            /*  — prefixe widget `Champ`/`Field`/`Control` en
             * traduction. STRICTEMENT contextuel : seulement en position
             * `<copule> <article>? Champ <kind-reconnu>` (le meme contexte ou
             * translate traduit deja le kind + le `de`/`of`). On reecrit le
             * prefixe vers la langue cible (EN -> Control, FR -> Champ). Hors
             * de ce contexte (variable `champ`, type nu `c est un Champ`), on
             * NE touche a rien — pas de pairing keyword global (casserait
             * l'identifiant/type nu). */
            bool prefixConnector = fmt_after_type_connector(lastWord, lastWordLen);
            if (!prefixConnector && lastWord && lastWordLen >= 1 && lastWordLen <= 2) {
                /* Article EN a/an comme connecteur (`is a Control Button`).
                 * fmt_after_type_connector ne couvre que est/un/une/is. */
                char lw[4];
                int ll = scanner_strip_accents(lastWord, lastWordLen, lw, sizeof(lw));
                if (ll >= 1 && ll < (int)sizeof(lw)) {
                    for (int i = 0; i < ll; i++) lw[i] = (char)tolower((unsigned char)lw[i]);
                    lw[ll] = '\0';
                    if (strcmp(lw, "a") == 0 || strcmp(lw, "an") == 0) prefixConnector = true;
                }
            }
            if (g_fmt_target_lang != OWL_TRANSLATE_NONE &&
                ((normLen == 5 && strcmp(normBuf, "champ")   == 0) ||
                 (normLen == 5 && strcmp(normBuf, "field")   == 0) ||
                 (normLen == 7 && strcmp(normBuf, "control") == 0)) &&
                prefixConnector &&
                fmt_widget_prefix_kind_follows(p)) {
                buf_puts(out, g_fmt_target_lang == OWL_TRANSLATE_EN
                                  ? "Control" : "Champ");
                lastWord = wordStart; lastWordLen = wordLen;
                continue;
            }

            /*  — SORTE de controle juste apres le prefixe widget. La
             * resolution passe par CONTROL_KINDS (source unique) et non par
             * fmt_keywords[] : la table connaissait deja Tab/Pane/ProgressBar/
             * BarCodes sans que la traduction ne les emette, et `Tab` ne peut
             * pas prendre d'entree fmt_keywords[] sans masquer la constante TAB.
             * Vaut aussi hors traduction (canonisation de casse `Champ onglet`
             * -> `Champ Onglet`, langue ecrite preservee). */
            if (fmt_prev_word_is_widget_prefix(lastWord, lastWordLen)) {
                const char* kindForm = fmt_control_kind_lang_form(wordStart, wordLen);
                if (kindForm != NULL) {
                    buf_puts(out, kindForm);
                    lastWord = wordStart; lastWordLen = wordLen;
                    continue;
                }
            }

            const FmtKeyword* kw = find_keyword(normBuf, normLen);
            /*  : `a`/`to` partagent le token de la borne de plage À
             * (scanner TOKEN_A) mais sont AUSSI l'article EN (`x is a Class`)
             * et des identifiants valides ( : `a est un entier`,
             * `PROCEDURE A(...)`, `Trace(a)`). La VRAIE borne de plage est
             * canonisee en amont par le cas special POUR/CAS (continue) et
             * n'atteint jamais ce point : tout `a`/`to` qui arrive ici est un
             * article ou un nom, jamais la borne. On neutralise donc l'entree
             * keyword — elle n'existe dans fmt_keywords[] que pour porter la
             * fiche canon `À`/`TO` que le self-test fmt-canon confronte. Les
             * variantes `_a_`/`_to_` (borne evaluee une fois) n'ont, elles, pas
             * de cas special amont ni d'emploi courant comme identifiant : leur
             * entree reste active et fait le vrai travail (`_a_` -> `_À_`). */
            if (kw != NULL && kw->category == CAT_CONTROL &&
                ((normLen == 1 && normBuf[0] == 'a') ||
                 (normLen == 2 && strcmp(normBuf, "to") == 0))) {
                kw = NULL;
            }
            /* Garde shadow constante built-in : si le mot match une constante
             * de la table (CAT_BOOL — Vrai/Faux/Null/TAB/CRLF/… — ou une forme
             * heritee CAT_EOL_LEGACY) ET qu'il est declare comme variable user,
             * on emet verbatim. canon autorise `tab est un tableau` (avec
             * warning compile-time "surcharge"). Le formatter ne doit pas
             * ecraser ce choix user en le canonisant vers la constante.
             *
             *  : meme garde pour CAT_DECL, ou vivent les genres du DSL
             * (Corps, Bouton, Colonne, Table…). Le compilo les accepte comme
             * nom de bloc ou de variable, et ce nom appartient a l'utilisateur,
             * pas au canon — partout : a la declaration (`corps est un Corps`)
             * comme a l'usage (`Ajoute(table, "a")`). Le critere est donc
             * l'absence de POSITION DE TYPE, pas la presence d'une copule
             * derriere : sans lui la meme variable sortait sous deux
             * orthographes dans un meme fichier.
             * La condition de position reste indispensable dans l'autre sens :
             * is_var_name matche AUSSI le mot en position de type (comparaison
             * casse-insensible avec le nom du bloc), donc sans elle
             * `Champ bouton` cesserait d'etre canonise.
             * Les types declares par l'utilisateur obtiennent ce comportement
             * gratuitement : ils n'ont pas d'entree dans fmt_keywords[]. */
            bool declNameProtected = is_var_name(wordStart, wordLen);
            /*  — les champs de structure/classe vivent dans des tables
             * dediees (user_fields / class members), pas dans var_types que
             * sonde is_var_name. Un membre nomme comme un genre DSL
             * (`corps est une chaine`) tombait sinon sur la canonisation
             * keyword et ressortait `Corps`. En position hors-type (le mot
             * n'est pas le type de la declaration) c'est un identifiant
             * utilisateur : verbatim, comme le fait deja  / 
             * pour les homonymes non-keyword. Limite a CAT_DECL : CAT_BOOL /
             * CAT_EOL_LEGACY gardent leur garde is_var_name inchangee. */
            if (!declNameProtected && kw != NULL && kw->category == CAT_DECL &&
                !fmt_in_type_position(lastWord, lastWordLen, seenCopuleOnLine) &&
                (user_name_known(g_fmt_st->user_fields,
                                 g_fmt_st->user_fields_count,
                                 wordStart, wordLen) ||
                 class_member_exists_any_class(wordStart, wordLen))) {
                declNameProtected = true;
            }
            if (kw != NULL &&
                (kw->category == CAT_BOOL || kw->category == CAT_EOL_LEGACY ||
                 (kw->category == CAT_DECL &&
                  !fmt_in_type_position(lastWord, lastWordLen, seenCopuleOnLine))) &&
                declNameProtected) {
                buf_write(out, wordStart, wordLen);
                lastWord = wordStart; lastWordLen = wordLen;
                continue;
            }
            /*  — meme principe que , applique aux ARTICLES.
             * `un`/`une`/`a`/`an` sont des noms d'identifiant valides (
             * les a rendus utilisables partout ou un nom est attendu) et leur
             * role d'article est strictement POSITIONNEL : il suit une copule.
             * En tete de ligne il n'y a rien devant, donc pas d'article — c'est
             * un nom declare (`Un = 1` d'une enumeration, `Un est un entier`
             * d'une structure) ou une cible d'affectation.
             *
             * Le rabattre vers la forme minuscule de l'article ne faisait pas
             * qu'abimer la graphie : la 1re passe reecrivait la DECLARATION, la
             * 2e la relisait comme reference et propageait la minuscule a tous
             * les usages. C'est la rupture d'idempotence du formateur, et elle
             * a fausse le depistage de  (une passe ne la voit pas). */
            if (kw != NULL && kw->category == CAT_FILLER && wordIsFirstOnLine) {
                buf_write(out, wordStart, wordLen);
                lastWord = wordStart; lastWordLen = wordLen;
                continue;
            }
            if (kw != NULL) {
                /*  : keyword homonyme dont le canon depend de la
                 * position syntaxique. Calcule une fois ici pour les deux
                 * surcharges (type-position / call-position) ci-dessous. */
                bool typeConnector = fmt_after_type_connector(lastWord, lastWordLen);
                /* (a) PROCEDURE/FONCTION en position de TYPE (apres est/un/une/
                 * is) = nom de type minuscule accentue, comme chaîne/entier/
                 * durée. Le declarateur de bloc (`PROCEDURE Nom()`, sans
                 * connecteur `est` devant) garde la forme MAJUSCULE. */
                const char* typePosCanon = NULL;
                if (typeConnector) {
                    if (normLen == 9 && strcmp(normBuf, "procedure") == 0)
                        typePosCanon = "proc\xc3\xa9""dure";   /* procédure (EN=FR) */
                    else if ((normLen == 8 && strcmp(normBuf, "fonction") == 0) ||
                             (normLen == 8 && strcmp(normBuf, "function") == 0))
                        /*  : type-position `fonction`/`function` ->
                         * langue cible (fonction / function). */
                        typePosCanon = fmt_pick_lang("fonction", "fonction", "function");
                }
                /* (b) Keyword homonyme d'une NATIVE en position d'appel :
                 * mot ACCOLE a `(` (sans espace) = appel de fonction, pas
                 * l'operateur/keyword. On resout alors via la native registry
                 * (bilingue), ce qui permet la traduction.
                 *   - `modulo(` -> native Modulo (Maths), pas l'operateur infixe.
                 *   - `Delete(` / `Supprime(` -> native Supprime (nameEN Delete),
                 *     pas l'operateur OOP `delete`. Sans ce chemin, `Delete`
                 *     matche le keyword CAT_OOP NEUTRE et ne retraduit jamais
                 *     vers `Supprime` (round-trip EN->FR casse, bug ).
                 * La `(` doit suivre immediatement (`7 modulo (2 + 1)` reste
                 * l'operateur infixe, paren detachee). find_native_norm ne
                 * renvoie non-NULL que pour une native reelle : un keyword de
                 * controle non-native suivi de `(` n'est pas capte ici. */
                const char* callPosCanon = NULL;
                if (typePosCanon == NULL && *p == '(') {
                    char origFirst = wordStart[0];
                    callPosCanon = find_native_norm(normBuf, normLen, origFirst);
                }
                /* `de`/`of` d'ITERATION ("POUR CHAQUE x DE tab", "FOR EACH x
                 * OF tab") passe en MAJUSCULE, mais bilingue-preservant
                 * : `de` -> DE, `of` -> OF (l'EN reste EN, jamais
                 * rabattu vers DE). Ailleurs le connecteur reste minuscule —
                 * notamment le `de`/`of` de CONSTRUCTION DE TYPE
                 * ("tableau de entier", "array of integer"), distingue ici par
                 * l'absence de seenPourTout (pas de POUR/FOR + mot d'iteration
                 * en amont sur la ligne). */
                const char* inlineVis = seenProcOrFunc
                    ? fmt_visibility_inline_canon(normBuf, normLen) : NULL;
                if (typePosCanon != NULL) {
                    buf_puts(out, typePosCanon);
                } else if (callPosCanon != NULL) {
                    buf_puts(out, callPosCanon);
                } else if (seenPourTout && normLen == 2 &&
                           strcmp(normBuf, "de") == 0) {
                    /*  : `DE`/`OF` d'iteration suit la langue cible. */
                    buf_puts(out, fmt_pick_lang("DE", "DE", "OF"));
                } else if (seenPourTout && normLen == 2 &&
                           strcmp(normBuf, "of") == 0) {
                    buf_puts(out, fmt_pick_lang("OF", "DE", "OF"));
                } else if (seenPourTout && normLen == 3 &&
                           strcmp(normBuf, "par") == 0) {
                    /*  : `PAR`/`BY` d'iteration (POUR TOUT ... SÉPARÉE
                     * PAR <sep>) passe en MAJUSCULE, symetrique du DE/OF
                     *. Ailleurs (`tableau de 3 par 4`, tri) l'absence
                     * de seenPourTout laisse le canon minuscule de la ligne 393. */
                    buf_puts(out, fmt_pick_lang("PAR", "PAR", "BY"));
                } else if (seenPourTout && normLen == 2 &&
                           strcmp(normBuf, "by") == 0) {
                    buf_puts(out, fmt_pick_lang("BY", "PAR", "BY"));
                } else if (inlineVis != NULL) {
                    /* « PROCÉDURE PRIVÉE / PROTÉGÉE foo() » : accord feminin.
                     * Visibilite neutre (PRIVÉ/PROTÉGÉ EN=FR) -> pas de
                     * pairing langue, on garde la forme inline. */
                    buf_puts(out, inlineVis);
                } else if (g_fmt_target_lang == OWL_TRANSLATE_EN &&
                           kw->category == CAT_FILLER &&
                           (strcmp(normBuf, "un") == 0 ||
                            strcmp(normBuf, "une") == 0)) {
                    /*  : article FR un/une -> a/an (phonetique du nom
                     * EN qui suit). Round-trip assure par le genre cote EN->FR. */
                    char nextW[256];
                    int nwl = fmt_peek_next_word(p, nextW, sizeof nextW);
                    buf_puts(out, nwl > 0 ? fmt_article_en_before_type(nextW, nwl)
                                          : fmt_kw_emit_form(kw));
                } else if (g_fmt_target_lang == OWL_TRANSLATE_FR &&
                           kw->category == CAT_DECL &&
                           strcmp(normBuf, "of") == 0 &&
                           fmt_prev_word_is_coll_noun(lastWord, lastWordLen)) {
                    /*  — elision EN->FR du connecteur de type d'une
                     * collection : `array of X` -> `tableau de X`, mais `de`
                     * s'elide en `d'` quand le type FR qui suit commence par une
                     * voyelle (`tableau d'entiers`, `tableau d'Image`). Gate sur
                     * le connecteur SOURCE `of` (reconstruction depuis l'anglais)
                     * uniquement : un `de` deja ecrit (source FR, `--to fr`
                     * identite ou `owl fmt` cible NONE) reste `de entiers`, ce qui
                     * preserve la byte-identite avec le formatter (qui tolere
                     * `de entiers`, follow-up separe). Le type reste traduit par
                     * le path standard (pluriel primitif ou identifiant) a
                     * l'iteration suivante. */
                    char nextRaw[256];
                    int nrl = fmt_peek_next_word(p, nextRaw, sizeof nextRaw);
                    if (nrl > 0 &&
                        fmt_coll_elem_fr_starts_vowel(nextRaw, nrl)) {
                        buf_puts(out, "d'");
                        /* colle le type au `d'` : avale l'espace source. */
                        while (*p == ' ' || *p == '\t') p++;
                    } else {
                        buf_puts(out, "de");
                    }
                } else if ((normLen == 4 && strcmp(normBuf, "sont") == 0) ||
                           (normLen == 3 && strcmp(normBuf, "are") == 0)) {
                    /*  : copule de DECLARATION MULTIPLE. L'article `des`
                     * n'a pas d'equivalent anglais — il tombe vers EN et se
                     * reconstruit vers FR. Ce qui porte le pluriel d'un bout a
                     * l'autre, c'est la copule (toujours presente, toujours
                     * traduite), pas le `s` du type : `monétaire`, `numérique`,
                     * `tableau` et les types utilisateur n'ont pas de pluriel
                     * anglais. Canon FR = `sont des <type au pluriel>` (forme
                     * unique du corpus) ; canon EN = `are <type au pluriel si le
                     * langage en connait un>` (cf. tests/regression/
                     * test_declaration_multi.owl). */
                    buf_puts(out, fmt_kw_emit_form(kw));
                    char nextW[256], nextNorm[256];
                    int nwl = fmt_peek_next_word(p, nextW, sizeof nextW);
                    owl_normalize_name(nextW, nextNorm, sizeof nextNorm);
                    bool nextIsDes = (nwl > 0 && strcmp(nextNorm, "des") == 0);
                    if (fmt_out_lang_is_fr(kw)) {
                        if (nwl > 0 && !nextIsDes) buf_puts(out, " des");
                    } else if (nextIsDes) {
                        while (*p == ' ' || *p == '\t') p++;
                        p += nwl;            /* article avale : l'EN n'en a pas */
                    }
                } else if (kw->category == CAT_TYPE &&
                           fmt_prev_word_is_decl_plural(lastWord, lastWordLen) &&
                           fmt_decl_plural(kw, fmt_out_lang_is_fr(kw)) != NULL) {
                    /*  : type d'une declaration multiple -> pluriel de la
                     * langue de sortie. Passe AVANT le couple tableau associatif
                     * : `sont des tableaux associatifs` n'est de toute
                     * facon pas une declaration que le compilateur accepte. */
                    buf_puts(out, fmt_decl_plural(kw, fmt_out_lang_is_fr(kw)));
                } else if (g_fmt_target_lang != OWL_TRANSLATE_NONE &&
                           ((normLen == 3 && (strcmp(normBuf, "pas") == 0 ||
                                              strcmp(normBuf, "not") == 0)) ||
                            (normLen == 4 && strcmp(normBuf, "step") == 0))) {
                    /*  : PAS/STEP/NOT collapse au meme token (TOKEN_PAS,
                     * cf. scanner). Le canon EN depend du SENS, pas de la forme
                     * ecrite : STEP dans l'en-tete POUR une fois la borne de
                     * plage franchie (seenPour && seenPourBorne), NOT (negation)
                     * partout ailleurs — c'est le cas commun. Cible FR : les deux
                     * sens ramenent a PAS. Hors translate, on tombe dans le else
                     * standard (forme ecrite preservee via fmt_kw_emit_form). */
                    if (g_fmt_target_lang == OWL_TRANSLATE_EN)
                        buf_puts(out, (seenPour && seenPourBorne) ? "STEP" : "NOT");
                    else
                        buf_puts(out, "PAS");
                } else if (kw->category == CAT_TYPE &&
                           g_fmt_target_lang != OWL_TRANSLATE_NONE &&
                           (fmt_is_array_noun(normBuf, normLen) ||
                            fmt_is_array_adj(normBuf, normLen))) {
                    /*  — couple type "tableau associatif" <-> "associative
                     * array". Le mot courant est le 1er du couple (nom en FR,
                     * adjectif en EN) ; on peek le 2nd, traduit les deux et emet
                     * dans l'ordre de la LANGUE CIBLE (FR nom-adjectif, EN
                     * adjectif-nom), puis consomme le 2nd mot. Robuste quel que
                     * soit l'ordre source. */
                    char nextRaw[256];
                    int nrl = fmt_peek_next_word(p, nextRaw, sizeof nextRaw);
                    char nextNorm[256];
                    owl_normalize_name(nextRaw, nextNorm, sizeof nextNorm);
                    int nnl = (int)strlen(nextNorm);
                    bool curNoun = fmt_is_array_noun(normBuf, normLen);
                    bool paired = nrl > 0 &&
                        (curNoun ? fmt_is_array_adj(nextNorm, nnl)
                                 : fmt_is_array_noun(nextNorm, nnl));
                    if (paired) {
                        const FmtKeyword* kwNext = find_keyword(nextNorm, nnl);
                        const char* curForm  = fmt_kw_emit_form(kw);
                        const char* nextForm = kwNext ? fmt_kw_emit_form(kwNext)
                                                      : nextRaw;
                        const char* noun = curNoun ? curForm : nextForm;
                        const char* adj  = curNoun ? nextForm : curForm;
                        if (g_fmt_target_lang == OWL_TRANSLATE_FR) {
                            buf_puts(out, noun); buf_putc(out, ' '); buf_puts(out, adj);
                        } else {
                            buf_puts(out, adj); buf_putc(out, ' '); buf_puts(out, noun);
                        }
                        while (*p == ' ' || *p == '\t') p++;
                        p += nrl;            /* consomme le 2nd mot du couple */
                        lastWord = NULL; lastWordLen = 0;
                        continue;
                    }
                    buf_puts(out, fmt_kw_emit_form(kw));   /* mot seul : normal */
                } else if (kw->category == CAT_TYPE &&
                           fmt_prev_word_is_de_of(lastWord, lastWordLen) &&
                           (g_fmt_target_lang == OWL_TRANSLATE_FR ||
                            (g_fmt_target_lang == OWL_TRANSLATE_NONE &&
                             kw->lang == KW_FR)) &&
                           fmt_coll_elem_fr_plural(kw) != NULL) {
                    /*  — type d'element de collection au PLURIEL en
                     * sortie FR : translate EN->FR (`array of integer` ->
                     * `tableau de entiers`) ET canonicalisation `owl fmt` d'une
                     * source FR (`de entier` -> `de entiers`). En sortie EN, le
                     * singulier (canon EN `array of integer`) reste emis par la
                     * branche standard ci-dessous. */
                    buf_puts(out, fmt_coll_elem_fr_plural(kw));
                } else {
                    buf_puts(out, fmt_kw_emit_form(kw));
                }
                /* Track PROCEDURE/FONCTION so the visibility modifier on the
                 * same line takes the feminine inline form (vs the masculine
                 * standalone block marker).  : pas en position de TYPE
                 * (`x est une procédure`) — la ce n'est pas un declarateur, et
                 * aucun modificateur de visibilite ne suit. */
                if (typePosCanon == NULL &&
                    ((normLen == 9 && strcmp(normBuf, "procedure") == 0) ||
                    (normLen == 8 && strcmp(normBuf, "fonction") == 0) ||
                    (normLen == 8 && strcmp(normBuf, "function") == 0))) {
                    seenProcOrFunc = true;
                    /*  : le prochain identifier est le nom de proc. Les
                     * modificateurs de visibilite (PUBLIQUE/PRIVÉE...) passent
                     * par le path keyword, pas par le path identifier — le flag
                     * survit donc jusqu'au vrai nom. */
                    expectProcName = true;
                }
                /* Track POUR so we can upgrade bare 'A' in the range */
                if (normLen == 4 && strcmp(normBuf, "pour") == 0) {
                    seenPour = true;
                } else if (normLen == 3 && strcmp(normBuf, "for") == 0) {
                    seenPour = true;
                } else if (seenPour &&
                           (strcmp(normBuf, "tout")   == 0 ||
                            strcmp(normBuf, "toute")  == 0 ||
                            strcmp(normBuf, "tous")   == 0 ||
                            strcmp(normBuf, "chaque") == 0 ||
                            strcmp(normBuf, "each")   == 0 ||
                            strcmp(normBuf, "every")  == 0 ||
                            strcmp(normBuf, "all")    == 0)) {
                    /*  : tous les mots-cles d'iteration (FR + EN)
                     * arment la promotion du `de`/`of` qui suit la variable
                     * de boucle. Couvre `all` (len 3) que l'ancien test
                     * normLen==4 ratait. */
                    seenPourTout = true;
                } else if ((normLen == 3 && strcmp(normBuf, "cas") == 0) ||
                           (normLen == 4 && strcmp(normBuf, "case") == 0)) {
                    seenCas = true;
                }
            } else if (g_fmt_target_lang == OWL_TRANSLATE_FR &&
                       fmt_after_type_connector(lastWord, lastWordLen) &&
                       ((normLen == 1 && strcmp(normBuf, "a") == 0) ||
                        (normLen == 2 && strcmp(normBuf, "an") == 0))) {
                /*  : article EN a/an -> un/une selon le GENRE du type
                 * qui suit (lookup registre/primitifs/widgets). C'est l'unique
                 * point ou le genre est lu.
                 *
                 *  : les 81 types composes du registre ont un genre,
                 * comme les primitifs et les sortes de widget. Un type sans
                 * genre connu (NONE) en position de declaration est donc un nom
                 * HORS registre : une classe/structure UTILISATEUR. OWL n'a pas
                 * de notion de genre de classe.
                 *
                 *  : defaut masculin `un` pour ce cas (choix OWL assume,
                 * pas une devinette de canon). Avant, la branche NONE gardait
                 * l'article EN ecrit -> `is a cJoueur` sortait `est a cJoueur`
                 * (~86 fuites sur un projet reel). Une classe au nom "feminin"
                 * (cJoueuse) rend donc aussi `un` : l'info de genre n'existe
                 * pas cote OWL, on ne l'invente pas. Round-trip stable pour les
                 * classes masculines (un->a->un) ; une classe feminine perd son
                 * genre au passage EN (une->a->un), contrepartie assumee.
                 * Le repli verbatim ne reste que si aucun nom ne suit l'article
                 * (article pendant en fin de ligne). */
                char nextW[256];
                int nwl = fmt_peek_next_word(p, nextW, sizeof nextW);
                /*  : type "tableau composE" ecrit adjectif-nom en EN
                 * ("an associative array") -> le genre vient du NOM, pas de
                 * l'adjectif. On saute un adjectif de tableau en tete. */
                {
                    char an[256];
                    owl_normalize_name(nextW, an, sizeof an);
                    if (nwl > 0 && fmt_is_array_adj(an, (int)strlen(an))) {
                        const char* q = p;
                        while (*q == ' ' || *q == '\t') q++;
                        q += nwl;
                        char nounW[256];
                        int nn2 = fmt_peek_next_word(q, nounW, sizeof nounW);
                        if (nn2 > 0) { memcpy(nextW, nounW, (size_t)nn2 + 1); nwl = nn2; }
                    }
                }
                int genre = nwl > 0 ? fmt_genre_of_type(nextW, nwl) : 0;
                if (genre == GENRE_F)      buf_puts(out, "une");
                else if (genre == GENRE_M) buf_puts(out, "un");
                else if (nwl > 0)          buf_puts(out, "un");   /*  : classe user sans genre -> masculin par defaut */
                else                       buf_write(out, wordStart, wordLen);
                lastWord = wordStart; lastWordLen = wordLen;
                continue;
            } else {
                /*  : nom de proc en position event. Le 1er identifier
                 * apres PROCEDURE/FONCTION (visibilite eventuelle deja passee)
                 * est le nom declare. En translate, s'il matche un event canon
                 * (surClic, FinInitialisation, surBoutonGauche...), on emet la
                 * forme de la langue cible ; sinon (proc user) on laisse le
                 * traitement normal recanoniser. Consomme le flag une seule fois
                 * pour ne pas toucher les params qui suivent dans les (). */
                if (expectProcName) {
                    expectProcName = false;
                    if (!afterDot) {
                        const char* epart = fmt_event_partner(wordStart, wordLen);
                        if (epart != NULL) {
                            buf_puts(out, epart);
                            receiverType = NULL; receiverTypeLen = 0;
                            lastWord = wordStart; lastWordLen = wordLen;
                            continue;
                        }
                    }
                }
                /* Scope-aware local/param canon: if the word is a
                 * local or parameter of the enclosing procedure, re-case it to
                 * its declaration. Skipped in TYPE position (right after
                 * `est`/`un`/`une`/`is`) so the type token on a declaration's
                 * RHS (`point est un Point`) keeps its own casing instead of
                 * being dragged to the variable's — a variable and a type may
                 * legitimately share a name up to casing (invariant 5). */
                bool typePos = fmt_after_type_connector(lastWord, lastWordLen);
                int wordOff = (int)(wordStart - srcBase);
                const char* scoped =
                    typePos ? NULL
                            : find_scoped_canonical(wordStart, wordLen, wordOff);
                if (scoped != NULL) {
                    buf_puts(out, scoped);
                }
                /* If the word is a declared variable name (LHS of "X est un/une Y"),
                 * emit it toward its declaration casing when known as a TOP-LEVEL
                 * global, else verbatim. The user_globals table holds
                 * the declared spelling of globals (variables, constants, procedure
                 * names). Local variables / parameters are handled above by
                 * find_scoped_canonical and never reach this branch with their
                 * own canon: if they did, they wouldn't be in user_globals (a
                 * local of one procedure must not normalise an homonym in another). */
                /*  bug (a) : si le mot est revendique par le scope
                 * courant (local/param, deja canon donc scoped a renvoye
                 * NULL plus haut), NE PAS tomber sur la branche user_globals
                 * — sinon un global homonyme case-insensitive (typiquement
                 * le nom de la procedure englobante) ecrase la casse du
                 * local. Cas reproduit : `PROCEDURE C(c est une chaine)` ou
                 * `c` (param) tombait sur user_globals.find("C") et
                 * devenait `C`.
                 *
                 *  : garde `!typePos` (meme raison que pour
                 * find_scoped_canonical plus haut). En position de type
                 * (`projet est un Projet`, `point est un Point`), un nom
                 * de variable homonyme du type — a la casse pres — ne doit
                 * PAS tirer le token de type vers la graphie de la variable.
                 * Le type garde son canon (builtin composе ou user) resolu
                 * en aval. Aligne l'invariant 5 (variable et type peuvent
                 * partager un nom a la casse pres) deja applique a
                 * find_scoped_canonical mais qui manquait ici. */
                else if (!typePos && is_var_name(wordStart, wordLen) &&
                         !scoped_name_known(wordStart, wordLen, wordOff)) {
                    const char* gnorm = find_user_canonical(
                        g_fmt_st->user_globals, g_fmt_st->user_globals_count, wordStart, wordLen);
                    if (gnorm != NULL) buf_puts(out, gnorm);
                    else               buf_write(out, wordStart, wordLen);
                }
                else if (!typePos && is_var_name(wordStart, wordLen)) {
                    /* Local/param deja canon : emettre verbatim, sans
                     * canon globale. */
                    buf_write(out, wordStart, wordLen);
                }
                /* User enum member: a member declared in
                 * `X est une Énumération` keeps its declaration casing — it
                 * does NOT borrow the canon of a homonym native (actif vs
                 * Actif). Not applied after a dot: `obj.actif` is field
                 * access, handled by the class-member path above. */
                else if (!afterDot && enum_member_known(wordStart, wordLen)) {
                    const char* enorm = find_enum_member_canonical(wordStart, wordLen);
                    if (enorm != NULL) buf_puts(out, enorm);
                    else buf_write(out, wordStart, wordLen);
                }
                /*  : un champ declare dans une Structure/Classe user
                 * conserve sa casse de declaration meme s'il collisionne avec
                 * une native homonyme (age vs Age, nom vs Nom prop builtin).
                 * Path standalone (pas afterDot — l'acces .member est traite
                 * plus haut au branch class member). La canon user gagne sur
                 * la canon builtin pour les noms declares localement par
                 * l'utilisateur.
                 *
                 *  round 2 : ne PAS appliquer en position d'appel. Si
                 * l'identifier est immediatement suivi de `(` (eventuellement
                 * apres whitespace), c'est un appel — le canon natif gagne
                 * (`Age("...")` reste `Age`). Sans ce garde, un champ user
                 * homonyme degrade aveuglement TOUT appel native homonyme
                 * standalone (regression detectee fixture round 2). La
                 * declaration `age est un entier` reste protegee : `age`
                 * n'est pas suivi de `(` mais d'un espace + `est`. */
                else if (!afterDot && user_name_known(g_fmt_st->user_fields,
                                                      g_fmt_st->user_fields_count,
                                                      wordStart, wordLen) &&
                         !is_call_position(p)) {
                    const char* fnorm = find_user_canonical(g_fmt_st->user_fields,
                                                            g_fmt_st->user_fields_count,
                                                            wordStart, wordLen);
                    if (fnorm != NULL) buf_puts(out, fnorm);
                    else buf_write(out, wordStart, wordLen);
                }
                /*  : meme protection que  mais sur les
                 * class_members. Un champ declare dans une Classe user
                 * (`val est un entier`) ou son acces via `:` (`:val = 42`)
                 * ne doit pas etre force vers la forme canon d'une native
                 * homonyme (Val() / Taille() / ...). On ne touche pas a la
                 * position d'appel (`Val("123")` reste canonise) ni au
                 * path afterDot (deja gere par la branche class-member
                 * plus haut). */
                else if (!afterDot &&
                         class_member_exists_any_class(wordStart, wordLen) &&
                         !is_call_position(p)) {
                    const char* fnorm = find_class_member_canonical_any_class(
                                            wordStart, wordLen);
                    if (fnorm != NULL) buf_puts(out, fnorm);
                    else buf_write(out, wordStart, wordLen);
                } else {
                    /*  : appel/usage NU d'une procedure/fonction/global
                     * user dont la graphie de declaration differe par l'accent
                     * (`ReinitialiseTout` -> `RéinitialiseTout`). Le branch
                     * user_globals plus haut est garde par is_var_name() qui
                     * compare en bytes (strncasecmp) : il rate le cas ASCII vs
                     * declaration accentuee (longueurs differentes). Ici on
                     * matche comme le runtime resout les identifiants — casse +
                     * accent insensibles — et on ramene vers la graphie de la
                     * declaration. Homonymie user/native : la def user du projet
                     * gagne (coherent avec la resolution du compilateur), donc
                     * ce check passe AVANT find_native_norm. Hors afterDot (acces
                     * membre traite plus haut). Le re-accentuage des natives nues
                     * reste assure par find_native_norm en aval quand aucun global
                     * user ne matche. */
                    if (!afterDot) {
                        const char* gnorm = find_user_canonical_ci_accent(
                            g_fmt_st->user_procs, g_fmt_st->user_procs_count,
                            wordStart, wordLen);
                        bool gKnown = false;
                        for (int gi = 0; gi < g_fmt_st->user_procs_count; gi++) {
                            if (ident_equal_ci_accent(
                                    g_fmt_st->user_procs[gi].name,
                                    g_fmt_st->user_procs[gi].len,
                                    wordStart, wordLen)) { gKnown = true; break; }
                        }
                        if (gKnown) {
                            if (gnorm != NULL) buf_puts(out, gnorm);
                            else               buf_write(out, wordStart, wordLen);
                            receiverType = NULL; receiverTypeLen = 0;
                            lastWord = wordStart; lastWordLen = wordLen;
                            continue;
                        }
                    }
                    /* Method call detection: if we're right after a '.', look up
                     * method catalog first to re-case to canonical form
                     * (e.g. QUAND → Quand, AVEC → Avec). */
                    const char* mnorm = afterDot ? find_method_norm(normBuf, normLen) : NULL;
                    if (mnorm != NULL) {
                        /*  : bascule de langue de la methode fluent. */
                        const char* mpart = fmt_method_partner(mnorm, (int)strlen(mnorm));
                        buf_puts(out, mpart != NULL ? mpart : mnorm);
                        continue;
                    }
                    /* Check native function / builtin type accent normalisation.
                     * Pass the original first char so HTTPRequete (function) and
                     * httprequete (type) are disambiguated by casing. */
                    char origFirst = wordLen > 0 ? wordStart[0] : '\0';
                    const char* norm = find_native_norm(normBuf, normLen, origFirst);
                    if (norm != NULL) {
                        buf_puts(out, norm);
                    } else {
                        /* User-declared struct/class name — case-correct if known. */
                        const char* unorm = find_user_canonical(g_fmt_st->user_types, g_fmt_st->user_types_count,
                                                                wordStart, wordLen);
                        if (unorm != NULL) {
                            buf_puts(out, unorm);
                        } else {
                            buf_write(out, wordStart, wordLen);
                        }
                    }
                }
            }
            receiverType = NULL; receiverTypeLen = 0;
            lastWord = wordStart; lastWordLen = wordLen;
            continue;
        }

        /*  +  — attribut de declaration `<cle[=valeur]>`.
         * Deux familles : `<agrandissement=N>` apres tableau/array, et les
         * attributs de type `<nullable>`/`<indetectable>`/`<utile|useful>`/
         * `<Serialise=...>` apres un type. On intercepte le `<` ici (avant la
         * canonisation identifier de la branche is_word_char) et on canonise la
         * CLE (casse+accents, forme registre) tout en PRESERVANT la valeur : la
         * canonisation d'identifiants cross-fichier ne doit pas
         * reecrire une valeur homonyme (`<agrandissement=N>` reste `=N` meme si
         * un `n` existe ailleurs ; `<Serialise="rename">` garde son litteral).
         * Position-gate strict (fmt_peek_decl_attr) : jamais un comparateur
         * `<`/`<=`/`<>` ni un generique `tableau<entier>` (mot non reconnu). */
        if (!inString && *p == '<' && fmt_peek_decl_attr(p + 1, NULL)) {
            buf_putc(out, '<');
            p = fmt_emit_attr_body(out, p + 1, NULL);
            lastWord = NULL; lastWordLen = 0;
            afterDot = false; afterDoubleDot = false;
            continue;
        }

        /* Handle _ET_, _OU_, _DANS_ — starts with underscore */
        if (*p == '_' && is_word_char(*(p+1))) {
            const char* start = p;
            p++; /* skip _ */
            while (is_word_char(*p)) p++;
            if (*p == '_') p++; /* closing _ */
            int len = (int)(p - start);

            const FmtKeyword* kw = find_keyword(start, len);
            if (kw != NULL) {
                buf_puts(out, fmt_kw_emit_form(kw));
            } else {
                buf_write(out, start, len);
            }
            continue;
        }

        /*  — bornes du contexte tag BCP 47. Le `{` arme le litteral
         * multilingue si le type a ete vu sur la ligne ; `}` le referme ; `]`
         * clot un index/liste (desarme un index en attente). Emission standard. */
        if (!inString && *p == '{') {
            if (seenMlTypeOnLine) mlLiteralCtx = true;
            /*  — empile le type compose du bloc qui s'ouvre (pose par
             * la resolution du type de declaration ou du champ-sous-bloc). */
            if (g_fmt_target_lang != OWL_TRANSLATE_NONE && g_fmt_st) {
                if (g_fmt_st->type_sp >= 0 && g_fmt_st->type_sp < 64)
                    g_fmt_st->type_stack[g_fmt_st->type_sp] = g_fmt_st->pending_type;
                g_fmt_st->type_sp++;
                g_fmt_st->pending_type = -1;
            }
            buf_putc(out, *p++);
            continue;
        }
        if (!inString && *p == '}') {
            mlLiteralCtx = false;
            if (g_fmt_target_lang != OWL_TRANSLATE_NONE && g_fmt_st &&
                g_fmt_st->type_sp > 0) {
                g_fmt_st->type_sp--;
            }
            buf_putc(out, *p++);
            continue;
        }
        if (!inString && *p == ']') {
            mlIndexPending = false;
            buf_putc(out, *p++);
            continue;
        }

        /* Everything else: copy as-is */
        buf_putc(out, *p++);
    }

    return p;
}

/* Check whether a line (up to its newline) ends with '[' as the last
 * non-whitespace non-comment char — that's the multi-line string
 * opener. Passed the start of the line content (already past leading ws). */
static bool line_ends_with_open_bracket(const char* p) {
    const char* last = NULL;
    while (*p != '\0' && *p != '\n') {
        /* Skip comments */
        if (*p == '/' && *(p+1) == '/') break;
        /* Skip strings */
        if (*p == '"') { fmt_skip_string_literal(&p, NULL, true); continue; }
        if (*p != ' ' && *p != '\t' && *p != '\r') last = p;
        p++;
    }
    return last != NULL && *last == '[';
}

/* Scanne une ligne (hors comment/double-quote) et renvoie true si elle laisse
 * une chaine backtick (`...`) OUVERTE a la fin de la ligne — c.-a-d. le debut
 * d'une chaine multi-ligne backtick (canon 2024). Le doublage `` est un backtick
 * litteral DANS la chaine et ne compte pas comme delimiteur (cf. scanner
 * scan_backtick_string). Les chaines "..." classiques sont sautees pour ne pas
 * confondre un backtick contenu dans une chaine double-quote.
 *
 * `p` pointe sur le debut du contenu de la ligne (apres l'indent). */
static bool line_opens_backtick_string(const char* p) {
    bool inBacktick = false;
    while (*p != '\0' && *p != '\n') {
        if (inBacktick) {
            if (*p == '`') {
                /* Doublage `` => backtick litteral, on reste dans la chaine. */
                if (*(p+1) == '`') { p += 2; continue; }
                inBacktick = false;
                p++;
                continue;
            }
            p++;
            continue;
        }
        /* Hors chaine backtick. */
        if (*p == '/' && *(p+1) == '/') break;     /* commentaire ligne */
        if (*p == '"') {                            /* chaine double-quote */
            fmt_skip_string_literal(&p, NULL, true);
            continue;
        }
        if (*p == '`') {
            if (*(p+1) == '`') { p += 2; continue; }  /* `` litteral hors chaine */
            inBacktick = true;
        }
        p++;
    }
    return inBacktick;
}

/* Une ligne INTERNE de chaine backtick multi-ligne ferme-t-elle la chaine ?
 * Le contenu de la chaine est opaque : on cherche le premier backtick non
 * double (``) qui termine le litteral. Renvoie true si trouve. */
static bool line_closes_backtick_string(const char* p) {
    while (*p != '\0' && *p != '\n') {
        if (*p == '`') {
            if (*(p+1) == '`') { p += 2; continue; }  /* `` litteral */
            return true;
        }
        p++;
    }
    return false;
}

/*  — helper mutualise : `*p` pointe sur un backtick ouvrant.
 * Avance `*p` jusqu'au caractere SUIVANT le backtick fermant (en sautant
 * les doublages `` consideres comme litteraux). Le contenu est opaque :
 * `{`, `"`, mots-cles, sauts de ligne — tout est ignore. Si la chaine
 * n'est pas fermee (multi-ligne ou tronquee), avance jusqu'a la fin de
 * la ligne / du buffer. Utilise par les pre-scans d'indentation pour
 * eviter que le contenu d'un backtick fasse monter `brace_depth` ou
 * declenche faussement un opener (cf. `line_opens_backtick_string`). */
static inline void skip_backtick_string(const char** p) {
    /* Caller garantit **p == '`' a l'entree. */
    (*p)++;  /* consomme le backtick ouvrant */
    while (**p != '\0' && **p != '\n') {
        if (**p == '`') {
            if (*(*p + 1) == '`') { (*p) += 2; continue; }  /* doublage `` litteral */
            (*p)++;  /* consomme le backtick fermant */
            return;
        }
        (*p)++;
    }
    /* Pas ferme sur la ligne : `*p` est sur '\n' ou '\0', caller continue. */
}

/* Delta `{` vs `}` d'une ligne, hors chaines ("..." et `...`) et hors
 * commentaire `//`.
 *
 * `skipLeadingClose` : ignorer un `}` en tete de ligne, deja consomme par
 * `startsWithBraceClose` pour desindenter la ligne elle-meme. Faux quand on
 * mesure un fragment de ligne (la ponctuation qui suit le `]` fermant d'une
 * chaine-bloc) : la, un `}` initial est bien un fermant a decompter.
 *
 *  — sauter le contenu d'un backtick est indispensable : une `{`
 * non balancee dans un litteral ferait monter brace_depth et casserait
 * l'indentation de tout ce qui suit. */
static int fmt_line_brace_delta(const char* p, bool skipLeadingClose) {
    int delta = 0;
    bool first = skipLeadingClose;
    while (*p != '\0' && *p != '\n') {
        if (*p == '"') { fmt_skip_string_literal(&p, NULL, true); continue; }
        if (*p == '`') {
            if (*(p + 1) == '`') { p += 2; continue; }  /* `` hors chaine */
            skip_backtick_string(&p);
            first = false;  /* un backtick a au moins un char non-blanc */
            continue;
        }
        if (*p == '/' && *(p + 1) == '/') break;
        if (*p == '{') delta++;
        else if (*p == '}' && !first) delta--;
        if (*p != ' ' && *p != '\t') first = false;
        p++;
    }
    return delta;
}

/* DSL v2 : aligne les '=' dans les blocs PROPRIETES ... FIN.
 * Post-pass sur le buffer deja formate. Pour chaque bloc PROPRIETES :
 *   1. scanne les lignes jusqu'au FIN matching
 *   2. calcule la largeur max du nom de prop (avant '=')
 *   3. reecrit chaque ligne 'name = value' avec padding uniforme
 * Les lignes sans '=' (commentaires, vides) sont preservees telles quelles. */
static bool line_starts_with_keyword(const char* line, const char* lineEnd, const char* kw) {
    const char* p = line;
    while (p < lineEnd && (*p == ' ' || *p == '\t')) p++;
    int klen = (int)strlen(kw);
    if (lineEnd - p < klen) return false;
    for (int i = 0; i < klen; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)kw[i])) return false;
    }
    /* Word boundary apres */
    const char* after = p + klen;
    if (after < lineEnd && is_word_char(*after)) return false;
    return true;
}

/* Trouve l'offset du '=' simple (non '==', '<=', '>=', '!=', '<>', '+=', ...)
 * dans la ligne. Retourne -1 si absent. nameStart/nameEnd remplis avec les
 * bornes du LHS (tout ce qui precede le '='), qui peut etre un identifiant
 * simple OU une cascade 'obj..prop[sub]..prop'. Un subscript '[...]' est
 * absorbe dans le LHS UNIQUEMENT s'il vit dans une cascade (au moins un '..',
 * ex. etat de style '..Etats[appuye]..') : la ligne reste alignable et rejoint
 * le run ( — sinon le '[' rompait le scan et scindait le run). Une
 * indexation nue 'tab[i] = ...' (aucun '..') n'est PAS alignable ( L33
 * -> retour -1). */
static int find_prop_equals(const char* line, int lineLen,
                              int* nameStart, int* nameEnd) {
    int i = 0;
    while (i < lineLen && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= lineLen) return -1;
    if (!is_word_char(line[i])) return -1;
    int ns = i;
    while (i < lineLen && is_word_char(line[i])) i++;
    /* Cascade : segment mot, subscript '[...]' optionnel, puis '..' -> mot.
     * Le subscript n'est absorbe que s'il vit DANS une cascade de proprietes
     * (au moins un '..'). Une indexation nue 'tab[i] = ...' (aucun '..') n'est
     * PAS un LHS alignable ( L33) -> retour -1. */
    bool sawSubscript = false;
    bool sawCascade = false;
    for (;;) {
        if (i < lineLen && line[i] == '[') {
            int depth = 1;
            i++;
            while (i < lineLen && depth > 0) {
                if (line[i] == '[') depth++;
                else if (line[i] == ']') depth--;
                i++;
            }
            if (depth != 0) return -1;  /* crochet non balance */
            sawSubscript = true;
        }
        if (i + 1 < lineLen && line[i] == '.' && line[i+1] == '.') {
            int j = i + 2;
            if (j >= lineLen || !is_word_char(line[j])) return -1;
            i = j;
            while (i < lineLen && is_word_char(line[i])) i++;
            sawCascade = true;
            continue;
        }
        break;
    }
    /* Indexation nue (subscript sans cascade) : non alignable ( L33). */
    if (sawSubscript && !sawCascade) return -1;
    int ne = i;
    /* skip ws */
    while (i < lineLen && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= lineLen) return -1;
    if (line[i] != '=') return -1;
    /* Pas '==' (comparaison). Les '<=', '>=', '<>', '+=' etc. n'atteignent
     * jamais ce point : le caractere avant le '=' rompt le scan du LHS.
     *
     *  — deux operateurs commencent par '=' et se poursuivent APRES :
     * '=]' (se termine par) et '=>' (lambda). Les prendre pour un '=' seul
     * faisait aligner leur premier caractere et pousser le second de l'autre
     * cote de l'espace : 'CAS =] ".pdf"' devenait 'CAS = ] ".pdf"', qui ne
     * compile plus. Le formateur tourne a l'enregistrement — il detruisait le
     * fichier pendant que l'utilisateur travaillait. */
    if (i + 1 < lineLen &&
        (line[i+1] == '=' || line[i+1] == ']' || line[i+1] == '>')) return -1;
    *nameStart = ns;
    *nameEnd = ne;
    return i;
}

/* DSL props canonisation : pour 'largeur'/'width'/'LIBELLE' dans un bloc
 * PROPRIETES, retourne la forme canon — FR si l'entree matche le canon FR
 * (Largeur, Libellé, ...) ou son ASCII (Libelle), EN si l'entree matche
 * un nameEN (Width, Caption, ...). Preserve donc le choix linguistique
 * de l'utilisateur, conformement au contrat bilingue (CLAUDE.md §6).
 *
 * Source : OWL_DSL_PROP_CATALOG[] (ui_catalog.c via dump_lsp_catalog).
 * Returns NULL si word ne matche aucune prop DSL connue. */
static const char* find_dsl_prop_norm(const char* word, int len) {
    if (len <= 0 || len >= 64) return NULL;
    char buf[64];
    memcpy(buf, word, (size_t)len);
    buf[len] = '\0';
    int prop_count = owl_fmt_canon ? owl_fmt_canon->dsl_prop_count() : 0;
    for (int i = 0; i < prop_count; i++) {
        const char* fr = owl_fmt_canon->dsl_prop_fr(i);
        const char* ascii = owl_fmt_canon->dsl_prop_ascii(i);
        const char* en = owl_fmt_canon->dsl_prop_en(i);
        bool hit = (en && strcasecmp(buf, en) == 0) ||
                   (fr && strcasecmp(buf, fr) == 0) ||
                   (ascii && strcasecmp(buf, ascii) == 0);
        if (hit) {
            /*  : translate -> forme de la langue cible (fallback sur
             * l'autre forme si NULL). */
            if (g_fmt_target_lang == OWL_TRANSLATE_EN)
                return (en != NULL) ? en : fr;
            if (g_fmt_target_lang == OWL_TRANSLATE_FR)
                return (fr != NULL) ? fr : en;
            /* Hors translate : preservation de la langue ecrite. */
            if (en && strcasecmp(buf, en) == 0) return en;
            return fr;
        }
    }
    return NULL;
}

/*  — contexte d'echange d'un handler de service (MoiMême..Requête /
 * MoiMême..Réponse et leurs sous-champs). Ces conteneurs et champs ne sont pas
 * des proprietes du bloc PROPRIETES (absents du catalogue DSL, validation
 * compile-time cote compilateur), mais le formatter reste gatekeeper canonique
 * (invariant 4) : il restaure la casse/accent SANS traduire la langue ecrite ;
 * en mode translate il bascule vers la langue cible. CodeÉtat/Méthode/Chemin
 * sont deja canonises via le catalogue OperationHTTP. NULL si non reconnu. */
static const char* find_service_ctx_prop_norm(const char* word, int len) {
    if (len <= 0 || len >= 64) return NULL;
    static const struct { const char* fr; const char* ascii; const char* en; }
    tbl[] = {
        { "R\xc3\xa9ponse",  "Reponse",     "Response" },
        { "Requ\xc3\xaate",  "Requete",     "Request" },
        { "Corps",           "Corps",       "Body" },
        { "TypeContenu",     "TypeContenu", "ContentType" },
        { "Identit\xc3\xa9", "Identite",    "Identity" },
    };
    char buf[64];
    memcpy(buf, word, (size_t)len);
    buf[len] = '\0';
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        bool hit = strcasecmp(buf, tbl[i].fr) == 0
                || strcasecmp(buf, tbl[i].ascii) == 0
                || strcasecmp(buf, tbl[i].en) == 0;
        if (!hit) continue;
        if (g_fmt_target_lang == OWL_TRANSLATE_EN) return tbl[i].en;
        if (g_fmt_target_lang == OWL_TRANSLATE_FR) return tbl[i].fr;
        /* Hors translate : preservation de la langue ecrite. */
        if (strcasecmp(buf, tbl[i].en) == 0) return tbl[i].en;
        return tbl[i].fr;
    }
    return NULL;
}

/* Largeur visible (en bytes UTF-8 = en colonnes pour ASCII; pour les accents
 * on compte les codepoints). */
static int utf8_codepoint_count(const char* s, int len) {
    int n = 0;
    for (int i = 0; i < len; ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        n++;
    }
    return n;
}

/* Etat "a l'interieur d'un commentaire bloc slash-etoile" a la FIN d'une
 * ligne, en partant de l'etat 'inBlock' a son debut. Respecte les chaines
 * "..." (avec echappement) et `...` mono-ligne, et le commentaire ligne //
 * (qui ne peut pas ouvrir de bloc). Miroir de la gestion opaque du pass
 * principal : un '=' ou un mot-cle a l'interieur d'un commentaire
 * ne doit pas etre vu. Gere ouverture/fermeture en milieu de ligne. */
static bool line_ends_in_block_comment(const char* s, int len, bool inBlock) {
    int i = 0;
    while (i < len) {
        char c = s[i];
        if (inBlock) {
            if (c == '*' && i + 1 < len && s[i+1] == '/') { inBlock = false; i += 2; continue; }
            i++;
            continue;
        }
        if (c == '"') {
            const char* q = s + i;
            fmt_skip_string_literal(&q, s + len, false);
            i = (int)(q - s);
            continue;
        }
        if (c == '`') {
            i++;
            while (i < len && s[i] != '`') i++;
            if (i < len) i++;
            continue;
        }
        if (c == '/' && i + 1 < len && s[i+1] == '/') break;  /* commentaire ligne */
        if (c == '/' && i + 1 < len && s[i+1] == '*') { inBlock = true; i += 2; continue; }
        i++;
    }
    return inBlock;
}

/* Aligne les '=' de toute suite consecutive de lignes 'LHS = valeur' au meme
 * indent, partout (dedans comme dehors des blocs PROPRIETES). Un run est casse
 * par une ligne vide, un commentaire, un changement d'indent ou toute ligne
 * non-'LHS = valeur'. Le LHS = tout ce qui precede le '=' (identifiant simple
 * ou cascade 'obj..prop'). Padding sur la largeur LHS max du run (codepoints).
 *
 * Commentaires bloc slash-etoile opaques : une ligne qui commence dans un
 * commentaire OU qui en ouvre un (non ferme avant la fin de ligne) est emise
 * VERBATIM — elle ne participe a aucun run, ne flippe pas inProp, et n'est
 * jamais realignee. Sans ca, un bloc PROPERTIES...END entierement commente se
 * faisait aligner partiellement (motifs X = .. et cascades Font..Nom = ..).
 *
 * Contexte PROPRIETES : suivi via un flag pour canoniser les noms de props
 * simples connues (Libelle -> Libellé, Width -> Largeur selon la langue). Un
 * bloc PROPRIETES est prop-pur (pas de bloc imbrique), donc le premier FIN/END
 * le referme. La canonisation DSL ne s'applique qu'en contexte prop. */
static char* align_proprietes_blocks(char* source) {
    Buffer out;
    buf_init(&out);

    bool inProp = false;
    bool inBlock = false;  /*  — a l'interieur d'un commentaire bloc */
    const char* p = source;
    while (*p) {
        const char* lineStart = p;
        while (*p && *p != '\n') p++;
        const char* lineEnd = p;
        int lineLen = (int)(lineEnd - lineStart);

        /*  — commentaire bloc opaque : une ligne qui commence dans un
         * commentaire OU qui en ouvre un (non ferme sur la ligne) est emise
         * verbatim, ne participe a aucun run et ne flippe pas inProp. */
        bool startedInBlock = inBlock;
        bool endsInBlock = line_ends_in_block_comment(lineStart, lineLen, inBlock);
        if (startedInBlock || endsInBlock) {
            inBlock = endsInBlock;
            buf_write(&out, lineStart, lineLen);
            if (*p == '\n') { buf_putc(&out, '\n'); p++; }
            continue;
        }

        if (line_starts_with_keyword(lineStart, lineEnd, "PROPRIETES")
            || line_starts_with_keyword(lineStart, lineEnd, "PROPRI\xc3\x89T\xc3\x89S")
            || line_starts_with_keyword(lineStart, lineEnd, "PROPERTIES")) {
            inProp = true;
            buf_write(&out, lineStart, lineLen);
            if (*p == '\n') { buf_putc(&out, '\n'); p++; }
            continue;
        }
        if (inProp && (line_starts_with_keyword(lineStart, lineEnd, "FIN")
                       || line_starts_with_keyword(lineStart, lineEnd, "END"))) {
            inProp = false;
            buf_write(&out, lineStart, lineLen);
            if (*p == '\n') { buf_putc(&out, '\n'); p++; }
            continue;
        }

        int ns, ne, eq;
        eq = find_prop_equals(lineStart, lineLen, &ns, &ne);
        if (eq < 0) {
            /* Ligne non alignable : emise verbatim, casse tout run. */
            buf_write(&out, lineStart, lineLen);
            if (*p == '\n') { buf_putc(&out, '\n'); p++; }
            continue;
        }

        /* Debut d'un run. L'indent du run = les 'ns' premiers bytes. */
        const char* runIndent = lineStart;
        int runIndentLen = ns;

        /* Passe 1 : largeur max du LHS (canon si prop connue en contexte prop). */
        int maxNameWidth = 0;
        const char* scan = lineStart;
        const char* runEnd = lineStart;
        while (*scan) {
            const char* lstart = scan;
            const char* lscan = scan;
            while (*lscan && *lscan != '\n') lscan++;
            int llen = (int)(lscan - lstart);

            /*  — une ligne qui ouvre un commentaire bloc casse le run
             * (le run demarre hors commentaire, donc inBlock=false ici). La
             * ligne ouvrante sera traitee verbatim par la boucle externe. */
            if (line_ends_in_block_comment(lstart, llen, false)) break;

            int s2, e2, eq2;
            eq2 = find_prop_equals(lstart, llen, &s2, &e2);
            if (eq2 < 0) break;
            if (s2 != runIndentLen
                || strncmp(lstart, runIndent, (size_t)runIndentLen) != 0)
                break;

            const char* canon = inProp ? find_dsl_prop_norm(lstart + s2, e2 - s2)
                                       : NULL;
            int w = canon ? utf8_codepoint_count(canon, (int)strlen(canon))
                          : utf8_codepoint_count(lstart + s2, e2 - s2);
            if (w > maxNameWidth) maxNameWidth = w;

            scan = lscan;
            if (*scan == '\n') scan++;
            runEnd = scan;
        }

        /* Passe 2 : re-emit chaque ligne du run avec padding uniforme. */
        const char* q = lineStart;
        while (q < runEnd) {
            const char* lstart = q;
            while (q < runEnd && *q != '\n') q++;
            int llen = (int)(q - lstart);

            int s2, e2;
            int eq2 = find_prop_equals(lstart, llen, &s2, &e2);

            buf_write(&out, lstart, s2);  /* indent */
            const char* canon = inProp ? find_dsl_prop_norm(lstart + s2, e2 - s2)
                                       : NULL;
            int nameW;
            if (canon) {
                int clen = (int)strlen(canon);
                buf_write(&out, canon, clen);
                nameW = utf8_codepoint_count(canon, clen);
            } else {
                buf_write(&out, lstart + s2, e2 - s2);
                nameW = utf8_codepoint_count(lstart + s2, e2 - s2);
            }
            int pad = maxNameWidth - nameW + 1;
            for (int i = 0; i < pad; i++) buf_putc(&out, ' ');
            buf_putc(&out, '=');
            int after = eq2 + 1;
            if (after < llen && lstart[after] != ' ') buf_putc(&out, ' ');
            buf_write(&out, lstart + after, llen - after);
            if (q < runEnd && *q == '\n') { buf_putc(&out, '\n'); q++; }
        }

        p = runEnd;
    }

    free(source);
    return buf_finish(&out);
}

/* Longueur du code d'une ligne, commentaire `//` de fin exclu (chaines "..." et
 * backticks `...` respectes). Miroir de la lecture opaque du reste du pass. */
static int fmt_line_code_len(const char* s, int len) {
    int i = 0;
    while (i < len) {
        char c = s[i];
        if (c == '"') {
            const char* q = s + i;
            fmt_skip_string_literal(&q, s + len, false);
            i = (int)(q - s);
            continue;
        }
        if (c == '`') { i++; while (i < len && s[i] != '`') i++; if (i < len) i++; continue; }
        if (c == '/' && i + 1 < len && s[i + 1] == '/') return i;
        i++;
    }
    return len;
}

/* La ligne (code hors commentaire de fin) se termine-t-elle par un marqueur de
 * continuation ? `...` exactement (pas `....`), ou un `_` isole precede d'un
 * blanc. Semantique alignee sur scanner.c (check_triple_dot_continuation /
 * check_line_continuation). */
static bool fmt_line_is_continuation_head(const char* s, int len) {
    int n = fmt_line_code_len(s, len);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) n--;
    if (n >= 3 && s[n-1] == '.' && s[n-2] == '.' && s[n-3] == '.')
        return (n == 3 || s[n-4] != '.');
    if (n >= 2 && s[n-1] == '_' && (s[n-2] == ' ' || s[n-2] == '\t'))
        return true;
    return false;
}

/*  — Alignement des lignes de continuation (`...` et `_`).
 *
 * Passe texte-a-texte idempotente (modele : align_proprietes_blocks, )
 * lancee APRES la normalisation des `=`. Une ligne "de tete" ouvre une
 * continuation quand son code (hors commentaire de fin) se termine par un
 * marqueur ; les lignes physiques suivantes sont des continuations jusqu'a une
 * ligne sans marqueur.
 *
 * Ancre (Option A) : les continuations s'alignent sous le 1er operande du RHS
 * de la tete. Pour une tete `LHS = RHS`, find_prop_equals borne le LHS et
 * detecte le `=` simple (exclut deja `==`/`+=`/indexation nue) : colonne =
 * len(LHS)+3 codepoints apres l'indentation (le "= " normalise vaut +3). Sans
 * `=` simple (appel pur, `+=`...) : repli a +1 tab. L'ancre est relative au
 * post-indent : [indent de la tete] + [ancre espaces | +1 tab] + contenu
 * trimme. Une continuation qui finit elle aussi par un marqueur prolonge la
 * chaine et garde l'ancre de la tete (invariant : colonne partagee). */
static char* align_continuation_lines(char* source) {
    Buffer out;
    buf_init(&out);

    bool inBlock = false;      /* commentaire bloc slash-etoile */
    bool inMultiline = false;  /* litteral multi-ligne [ ... ] */
    bool inBacktick = false;   /* chaine backtick multi-ligne */
    bool contActive = false;   /* la ligne courante est une continuation */
    char contPrefix[256];
    int contPrefixLen = 0;

    const char* p = source;
    while (*p) {
        const char* lineStart = p;
        while (*p && *p != '\n') p++;
        int lineLen = (int)(p - lineStart);

        /* Regions opaques : contenu verbatim, aucune continuation. */
        if (inMultiline) {
            bool closes = owl_multiline_block_line_closes(lineStart);
            buf_write(&out, lineStart, lineLen);
            if (*p == '\n') { buf_putc(&out, '\n'); p++; }
            if (closes) inMultiline = false;
            contActive = false;
            continue;
        }
        if (inBacktick) {
            bool closes = line_closes_backtick_string(lineStart);
            buf_write(&out, lineStart, lineLen);
            if (*p == '\n') { buf_putc(&out, '\n'); p++; }
            if (closes) inBacktick = false;
            contActive = false;
            continue;
        }
        bool startedInBlock = inBlock;
        bool endsInBlock = line_ends_in_block_comment(lineStart, lineLen, inBlock);
        if (startedInBlock) {
            inBlock = endsInBlock;
            buf_write(&out, lineStart, lineLen);
            if (*p == '\n') { buf_putc(&out, '\n'); p++; }
            contActive = false;
            continue;
        }

        if (contActive) {
            /* Continuation : remplace l'indent de tete par le prefixe calcule
             * sur la tete, conserve le contenu trimme. */
            int i = 0;
            while (i < lineLen && (lineStart[i] == ' ' || lineStart[i] == '\t')) i++;
            buf_write(&out, contPrefix, contPrefixLen);
            buf_write(&out, lineStart + i, lineLen - i);
            if (*p == '\n') { buf_putc(&out, '\n'); p++; }
            contActive = fmt_line_is_continuation_head(lineStart, lineLen);
            inBlock = endsInBlock;
            if (line_ends_with_open_bracket(lineStart)) inMultiline = true;
            if (line_opens_backtick_string(lineStart)) inBacktick = true;
            continue;
        }

        bool isHead = fmt_line_is_continuation_head(lineStart, lineLen);
        buf_write(&out, lineStart, lineLen);
        if (*p == '\n') { buf_putc(&out, '\n'); p++; }

        if (isHead) {
            int indentLen = 0;
            while (indentLen < lineLen &&
                   (lineStart[indentLen] == ' ' || lineStart[indentLen] == '\t')) indentLen++;
            int ns, ne;
            int eq = find_prop_equals(lineStart, lineLen, &ns, &ne);
            contPrefixLen = 0;
            for (int k = 0; k < indentLen && contPrefixLen < (int)sizeof(contPrefix); k++)
                contPrefix[contPrefixLen++] = lineStart[k];
            if (eq >= 0) {
                int anchor = utf8_codepoint_count(lineStart + ns, ne - ns) + 3;
                for (int k = 0; k < anchor && contPrefixLen < (int)sizeof(contPrefix); k++)
                    contPrefix[contPrefixLen++] = ' ';
            } else if (contPrefixLen < (int)sizeof(contPrefix)) {
                contPrefix[contPrefixLen++] = '\t';  /* repli : +1 niveau */
            }
            contActive = true;
        }

        inBlock = endsInBlock;
        if (line_ends_with_open_bracket(lineStart)) inMultiline = true;
        if (line_opens_backtick_string(lineStart)) inBacktick = true;
    }

    free(source);
    return buf_finish(&out);
}

char* owl_format(const char* source) {
    return owl_format_ctx(source, NULL);
}

char* owl_translate_ctx(const char* source, OwlTranslateLang target,
                        const OwlFormatContext* ctx) {
    if (target != OWL_TRANSLATE_FR && target != OWL_TRANSLATE_EN) return NULL;
    OwlTranslateLang prev = g_fmt_target_lang;
    g_fmt_target_lang = target;
    char* out = owl_format_ctx(source, ctx);
    g_fmt_target_lang = prev;  /* remis NONE en sortie -> owl fmt intact */
    return out;
}

char* owl_translate(const char* source, OwlTranslateLang target) {
    return owl_translate_ctx(source, target, NULL);
}

char* owl_format_ctx(const char* source,
                     const OwlFormatContext* ctx) {
    Buffer out;
    buf_init(&out);

    /* Etat de session per-call sur la pile : g_fmt_st pointe dessus pendant
     * toute la passe. Save/restore -> sûr si owl_format_ctx etait imbrique. */
    OwlFormatState fmtState;
    memset(&fmtState, 0, sizeof fmtState);
    OwlFormatState* prevSt = g_fmt_st;
    g_fmt_st = &fmtState;
    fmtState.pending_type = -1;  /*  : sentinelle (0 = index valide) */
    fmtState.widget_style_root = -1;  /*  : idem */
    fmtState.enclosing_meta_type = -1;  /*  : idem */

    /* Collect user-declared struct/class names, their fields/members, and
     * per-procedure locals/params so the formatter can normalize their case
     * in the rest of the file. */
    user_symbols_collect(source);
    constant_names_collect(source);  /*  : noms LHS CONSTANT[E] */
    user_scopes_collect(source);
    user_globals_collect(source);    /*  : globals + procedures top-level */
    /*  : complete les tables avec le contexte cross-fichier APRES
     * le harvest local (local prioritaire, contexte complementaire). */
    seed_from_context(ctx);

    const char* p = source;
    int indent = 0;
    int brace_depth = 0;  /* Inline literal `{ ... }` indent tracking. */
    int cas_body_open = 0; /* Flag : ligne courante est-elle dans le body
                            * d'un CAS X : newline-form ? Reset par chaque
                            * CAS/AUTRE/FIN. Limitation : pas de support
                            * nested SELON-dans-SELON (rare, on etend si
                            * besoin avec stack). */
    bool in_multiline = false;
    bool in_backtick = false;  /*  — dans une chaine backtick multi-ligne :
                                * contenu OPAQUE, lignes internes + fermeture
                                * emises verbatim (aucune indentation injectee). */
    /*  — pile parallele indexee par profondeur d'indent.
     * block_is_interface[d] == true ssi le block ouvert au niveau d est
     * une Interface. Dans un body d'Interface, les `PROCEDURE Foo()` et
     * `FONCTION Foo()` sont des signatures sans corps : elles NE doivent
     * PAS ouvrir un bloc. 64 niveaux d'imbrication = largement suffisant. */
    bool block_is_interface[64] = {0};
    /*  — pile parallele : block_is_procfunc[d] == true ssi le block
     * ouvert au niveau d est une PROCEDURE/FONCTION. En code VALIDE une
     * procedure ne s'imbrique jamais dans le corps d'une autre (chaque proc a
     * son FIN). Si un membre de collection (ou une methode) perd son FIN, le
     * corps precedent reste ouvert et les procedures suivantes se comptent
     * comme imbriquees -> sur-indentation. Cette pile permet de refermer
     * implicitement le proc precedent quand un nouvel ouvreur proc/func
     * apparait alors que l'englobant est deja un proc/func (degradation
     * gracieuse sur input malforme, no-op sur code valide). */
    bool block_is_procfunc[64] = {0};

    while (*p != '\0') {
        /* Multi-line string — emit every line verbatim (preserve
         * content exactly as the user wrote it) until a line starts with ']'.
         * Don't apply any keyword/indent rewriting, don't touch indent level. */
        if (in_multiline) {
            /*  — MEME regle de fermeture que le scanner
             * (owl_multiline_block_line_closes, cf. scanner.h). Le formateur
             * appliquait la sienne, plus permissive : toute ligne dont le
             * premier caractere non blanc etait `]`. Une ligne de CONTENU
             * `] texte` sortait donc du bloc, et le formateur canonisait la
             * suite du texte comme du code — `fin` ressortait en `FIN`, la
             * valeur de la chaine changeait au save. */
            if (!owl_multiline_block_line_closes(p)) {
                /* Ligne de contenu : donnee opaque, emise telle quelle. */
                while (*p != '\0' && *p != '\n') buf_putc(&out, *p++);
                buf_putc(&out, '\n');
                if (*p == '\n') p++;
                continue;
            }

            /*  — ligne de fermeture. La frontiere donnee/code est
             * EXACTEMENT au `]` : l'indentation de tete et le `]` lui-meme
             * appartiennent encore a la forme du litteral et sortent
             * verbatim ; ce qui suit est du code ordinaire (arguments,
             * appels, constantes, depuis que  autorise la ponctuation
             * ici) et doit etre canonise comme partout ailleurs, sinon cette
             * portion de fichier echappe au gatekeeper (invariant 4). */
            while (*p == ' ' || *p == '\t') buf_putc(&out, *p++);
            buf_putc(&out, *p++);  /* le `]` fermant */

            /* La ponctuation qui suit le `]` appartient a la structure
             * englobante et compte dans la profondeur : sans ca le `}` de
             * `]}` n'est jamais decompte et tout le reste du fichier gagne un
             * niveau d'indentation, a chaque passage. Le contenu du bloc, lui,
             * ne compte pas. */
            brace_depth += fmt_line_brace_delta(p, false);
            if (brace_depth < 0) brace_depth = 0;

            /* Un second litteral multi-ligne peut s'ouvrir sur ce fragment
             * (`], ChaineConstruit([`) : son contenu doit redevenir opaque
             * immediatement, sinon il repart dans le traitement de code. */
            bool fragReopensMultiline = line_ends_with_open_bracket(p);
            bool fragOpensBacktick = line_opens_backtick_string(p);

            Buffer fragBuf;
            buf_init(&fragBuf);
            p = format_line_content(p, &fragBuf, source);
            /* Le `\r` d'un CRLF appartient au terminateur, pas au contenu
             * (cf.  sur la voie normale). */
            bool fragEndsWithCr = (fragBuf.count > 0 &&
                                   fragBuf.data[fragBuf.count - 1] == '\r');
            if (fragEndsWithCr) fragBuf.count--;
            /* Copie verbatim du fragment deja canonise : on restaure la casse
             * et les accents sans toucher a l'espacement voulu par l'auteur. */
            if (fragBuf.count > 0) buf_write(&out, fragBuf.data, fragBuf.count);
            free(fragBuf.data);

            if (fragEndsWithCr) buf_putc(&out, '\r');
            buf_putc(&out, '\n');
            if (*p == '\n') p++;

            in_multiline = fragReopensMultiline;
            if (fragOpensBacktick) in_backtick = true;
            continue;
        }

        /*  — chaine backtick multi-ligne : le contenu est OPAQUE.
         * Lignes internes + ligne de fermeture emises VERBATIM (pas de strip
         * de l'indent de tete, pas d'injection de tab). Altérer ces lignes
         * changerait la valeur runtime du litteral. */
        if (in_backtick) {
            bool closes = line_closes_backtick_string(p);
            while (*p != '\0' && *p != '\n') buf_putc(&out, *p++);
            buf_putc(&out, '\n');
            if (*p == '\n') p++;
            if (closes) in_backtick = false;
            continue;
        }

        /* Skip leading whitespace on the line */
        while (*p == ' ' || *p == '\t') p++;

        /* Empty line — emit it truly empty (no trailing indentation tabs).
         * Indenter une ligne vide ne produit que du trailing whitespace.
         *
         *  — le `\r` d'un terminateur CRLF appartient a la fin de
         * ligne, pas au contenu : apres le skip d'indentation ci-dessus, une
         * ligne vide vaut `\r\n` et non `\n`. Sans le reconnaitre, elle
         * repartait dans le scan de mots qui reemettait l'indentation courante
         * avant le CR (`\t\r\n`) — le canon « ligne vide nue » s'inversait
         * selon l'EOL du fichier. On isole le CR et on le restitue avec le
         * newline — meme traitement que la fin d'une ligne ordinaire, ou le CR
         * est isole de lineBuf puis re-emis. L'EOL du fichier reste
         * donc preserve. Un `\r` solitaire (non suivi de `\n`) reste un
         * caractere ordinaire. */
        if (*p == '\n' || (*p == '\r' && *(p + 1) == '\n')) {
            if (*p == '\r') { buf_putc(&out, '\r'); p++; }
            buf_putc(&out, '\n');
            p++;
            continue;
        }
        if (*p == '\0') break;

        /* Scan all words on the line to detect structure.
         * First word determines close/mid-block behavior.
         * Any word on the line can be a block opener. */
        const char* firstW = NULL;
        int firstLen = 0;
        bool lineOpens = false;
        /*  — flags pour neutraliser l'ouverture de PROCEDURE/FONCTION
         * sans corps. lineHasAbstraite : ligne courante contient
         * 'abstraite'/'abstract' (signature pure). lineOpensInterface : la
         * ligne ouvre une Interface (sa profondeur sera marquee dans la pile). */
        bool lineHasAbstraite = false;
        bool lineOpensInterface = false;
        bool lineOpenerIsProcOrFunc = false;
        /* Suppression du compteur d'opens quand on est deja dans une Interface :
         * dans le body d'une Interface, les PROCEDURE/FONCTION sont des
         * signatures sans corps. */
        bool insideInterfaceBody = (indent > 0 && indent <= 64
                                    && block_is_interface[indent - 1]);

        /*  — une nouvelle ligne top-level (indent 0) referme la portee
         * meta du bloc `est un Projet` : on remet le porteur a -1 avant de
         * (re)scanner la ligne, qui le repositionnera si elle declare un Projet. */
        if (g_fmt_st && indent == 0) g_fmt_st->enclosing_meta_type = -1;

        {
            const char* scan = p;
            bool first = true;
            /* Tracks if we passed the canon declaration keyword `est`/`is` earlier
             * on this line. Combined with `lineHasEquals` below, this disables
             * block-opener detection for the RHS of a runtime init like
             * 'x est une Procédure = Compile(...)' — otherwise type names
             * (Procédure, Classe, Structure...) cascade indent each line.
             * Pure DSL declarations like 'IMG_X est un Champ Image' (sans '=')
             * gardent leur opener intact. */
            bool seenEst = false;

            /* Pre-scan : est-ce que la ligne contient un '=' (hors strings) ?
             * Permet de differencier init typée (x est une T = expr) du
             * declaration DSL pure (x est un Champ Image). */
            bool lineHasEquals = false;
            {
                const char* sc2 = p;
                int parenDepth = 0;
                while (*sc2 != '\0' && *sc2 != '\n') {
                    if (*sc2 == '"') {
                        fmt_skip_string_literal(&sc2, NULL, true);
                        continue;
                    }
                    /*  — saute un commentaire bloc slash-etoile : un
                     * '=' a l'interieur ne doit pas etre pris pour un binding
                     * (fausserait lineHasEquals -> skipOpener errone). */
                    if (*sc2 == '/' && *(sc2+1) == '*') {
                        sc2 += 2;
                        while (*sc2 != '\0' && *sc2 != '\n'
                               && !(*sc2 == '*' && *(sc2+1) == '/')) sc2++;
                        if (*sc2 == '*' && *(sc2+1) == '/') { sc2 += 2; continue; }
                        break;  /* commentaire non ferme sur la ligne */
                    }
                    if (*sc2 == '/' && *(sc2+1) == '/') break;
                    /*  — un '=' DANS les parentheses d'un header de
                     * declaration (`FEN_X est une Fenetre(p est une chaine =
                     * "x")`) est un defaut de parametre, pas l'init runtime
                     * `x est une T = expr` (dont le '=' est toujours a depth 0).
                     * Seul un '=' de profondeur 0 doit neutraliser l'ouverture
                     * du bloc DSL, sinon le corps de la Fenetre/Etat se
                     * de-indente. */
                    if (*sc2 == '(') { parenDepth++; sc2++; continue; }
                    if (*sc2 == ')') { if (parenDepth > 0) parenDepth--; sc2++; continue; }
                    if (*sc2 == '=' && parenDepth == 0) { lineHasEquals = true; break; }
                    sc2++;
                }
            }

            while (*scan != '\0' && *scan != '\n') {
                /* Skip strings */
                if (*scan == '"') {
                    fmt_skip_string_literal(&scan, NULL, true);
                    continue;
                }
                /*  — saute le contenu d'une chaine backtick `...`
                 * (mono-ligne ou multi-ligne). Sans ca, un opener (`SI`,
                 * `POUR`...) ou un mot-cle a l'interieur du litteral
                 * serait pris pour un vrai opener et casserait l'indent. */
                if (*scan == '`') {
                    if (*(scan+1) == '`') { scan += 2; continue; }  /* `` hors chaine */
                    skip_backtick_string((const char**)&scan);
                    continue;
                }
                /*  — commentaire bloc opaque au comptage de
                 * profondeur : un mot-cle ouvrant/fermant a l'interieur d'un
                 * commentaire slash-etoile ne doit PAS affecter lineOpens ni
                 * firstW. Cas mono-ligne : saute jusqu'au delimiteur fermant.
                 * Cas multi-ligne : l'ouvreur non ferme sur la ligne termine
                 * le scan (le reste est du commentaire) ; format_line_content
                 * consomme ensuite le corps du commentaire (avec ses newlines)
                 * en un seul passage, donc le word-scan ne redemarre jamais a
                 * l'interieur d'un bloc. */
                if (*scan == '/' && *(scan+1) == '*') {
                    scan += 2;
                    while (*scan != '\0' && *scan != '\n'
                           && !(*scan == '*' && *(scan+1) == '/')) scan++;
                    if (*scan == '*' && *(scan+1) == '/') { scan += 2; continue; }
                    break;  /* commentaire non ferme sur la ligne */
                }
                /* Skip line comments */
                if (*scan == '/' && *(scan+1) == '/') break;

                if (is_word_char(*scan)) {
                    const char* ws = scan;
                    while (is_word_char(*scan)) scan++;
                    int wlen = (int)(scan - ws);
                    if (first) { firstW = ws; firstLen = wlen; first = false; }
                    /* Skip block-opener detection if the word is a method call
                     * (preceded by '.') or an identifier following an identifier.
                     * Prevents `.Quand()` / `obj.Si(...)` from triggering block
                     * indent as if they were the canon `QUAND` / `SI` keywords. */
                    bool isMethodCall = (ws > p && *(ws-1) == '.');
                    /*  — depuis  les noms de widgets registry
                     * sont legitimement utilisables comme noms de variable.
                     * Un widget kind n'ouvre legitimement un body que en RHS
                     * d'une declaration `NAME est un|une <WidgetKind>` (donc
                     * seenEst=true au moment du widget kind). Si le widget
                     * kind apparait sans `est un|une` precedent (premier mot
                     * comme `Bouton est un entier = 1`, ou au milieu d'une
                     * expression `dbgVerifie(Bouton + Saisie, ...)`), il ne
                     * doit PAS ouvrir un bloc. */
                    bool isWidgetKindWord = false;
                    /*  — meme scan sur les kinds structurels
                     * (is_widget=false : MenuPrincipal, blocs d'Etat...). Ils
                     * ouvrent un bloc sans `est` (`Corps`), donc la garde
                     * out-of-place ne peut pas etre exactement la meme : la
                     * forme nue en tete de ligne reste un ouvreur legitime
                     * (cf. blockKindOutOfPlace plus bas). */
                    bool isBlockKindWord = false;
                    if (word_opens_block(ws, wlen)) {
                        char wAsc[64];
                        int waL = scanner_strip_accents(ws, wlen, wAsc, sizeof(wAsc));
                        if (waL > 0 && waL < (int)sizeof(wAsc)) {
                            char wLow[64];
                            for (int i = 0; i < waL; i++) wLow[i] = (char)tolower((unsigned char)wAsc[i]);
                            wLow[waL] = '\0';
                            for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
                                bool hit = false;
                                const char* nm[3] = { k->nameFR, k->nameAscii, k->nameEN };
                                for (int n = 0; n < 3; n++) {
                                    if (!nm[n]) continue;
                                    char nb[64];
                                    int nl = scanner_strip_accents(nm[n], (int)strlen(nm[n]), nb, sizeof(nb));
                                    if (nl <= 0 || nl != waL) continue;
                                    bool eq = true;
                                    for (int i = 0; i < nl; i++) {
                                        if (wLow[i] != (char)tolower((unsigned char)nb[i])) { eq = false; break; }
                                    }
                                    if (eq) { hit = true; break; }
                                }
                                if (!hit) continue;
                                if (k->is_widget) isWidgetKindWord = true;
                                else              isBlockKindWord  = true;
                                break;
                            }
                        }
                    }
                    /*  — declaration de controle : memorise le type de
                     * style racine de la sorte declaree. Le bloc PROPRIETES qui
                     * suit (et lui seul) porte les cascades `Style..X..Y`, donc
                     * la derniere declaration vue est la bonne racine — le canon
                     * DSL impose PROPRIETES en tete de corps, avant tout
                     * sous-controle. Une sorte sans style typé (Volet, Colonne)
                     * remet la racine a -1 : jamais d'heritage du parent. */
                    if (g_fmt_target_lang != OWL_TRANSLATE_NONE && g_fmt_st
                        && seenEst && (isWidgetKindWord || isBlockKindWord)) {
                        g_fmt_st->widget_style_root =
                            fmt_widget_style_root_index(ws, wlen);
                    }
                    /*  — declaration top-level `X est un Projet` : arme
                     * le porteur meta pour que les cles de son bloc PROPRIETES
                     * (champs du type Projet) se traduisent. Restreint au type
                     * Projet (bilingue Projet/Project) : les blocs de controle
                     * passent deja par les proprietes DSL. */
                    if (g_fmt_target_lang != OWL_TRANSLATE_NONE && g_fmt_st
                        && seenEst && indent == 0) {
                        char pab[16];
                        int pl = scanner_strip_accents(ws, wlen, pab, sizeof(pab));
                        if (pl > 0 && pl < (int)sizeof(pab)) {
                            for (int i = 0; i < pl; i++)
                                pab[i] = (char)tolower((unsigned char)pab[i]);
                            pab[pl] = '\0';
                            if (strcmp(pab, "projet") == 0
                                || strcmp(pab, "project") == 0)
                                g_fmt_st->enclosing_meta_type =
                                    fmt_composite_type_index(ws, wlen);
                        }
                    }
                    bool widgetKindOutOfPlace = isWidgetKindWord && !seenEst;
                    /*  — symetrie avec widgetKindOutOfPlace. Un kind
                     * structurel n'a que DEUX positions d'ouverture legitimes :
                     * apres la copule (`intro est un EnTeteDocument`) ou nu en
                     * tete de ligne (`Corps`). Partout ailleurs c'est un
                     * identifiant utilisateur homonyme, et il ne doit ouvrir
                     * aucun niveau — sinon `Trace(corps)` decale tout le reste
                     * du fichier d'une tabulation qui ne se referme jamais.
                     * La garde ne couvrait que l'affectation (`MenuPrincipal =
                     * MNU_X` dans un bloc PROPRIETES), donc pas l'usage en
                     * expression, qui n'a ni copule ni '='.
                     *
                     *  — troisieme position hors-ouverture : le genre en
                     * TETE de ligne suivi d'une copule (`corps est une chaine`).
                     * C'est le NOM d'une declaration de membre/variable, pas
                     * l'ouvreur nu `Corps` (qui n'a jamais de copule derriere).
                     * Le compilo accepte `corps` comme identifiant ; sans ce cas
                     * le formateur ouvrait un faux niveau et sur-indentait le FIN
                     * de la structure/classe. On sonde le mot suivant sur la
                     * meme ligne (la copule d'une declaration y est toujours). */
                    bool blockKindNamedDecl = false;
                    if (isBlockKindWord && !seenEst && ws == firstW) {
                        const char* pk = scan;
                        while (*pk == ' ' || *pk == '\t') pk++;
                        if (is_word_char(*pk)) {
                            const char* kw2 = pk;
                            while (is_word_char(*pk)) pk++;
                            blockKindNamedDecl = fmt_is_copule(kw2, (int)(pk - kw2));
                        }
                    }
                    bool blockKindOutOfPlace = isBlockKindWord && !seenEst
                                            && (lineHasEquals || ws != firstW
                                                || blockKindNamedDecl);
                    /*  — mot-cle ouvreur (SI, POUR, CLASSE, STRUCTURE,
                     * TANTQUE...) utilise comme NOM de variable : `si est un
                     * entier`. Un ouvreur en tete de ligne suivi immediatement
                     * d'une copule (`est`/`is`) est le nom d'une declaration,
                     * jamais une ouverture de bloc. Sans cette garde le
                     * formateur ouvre un niveau jamais referme et sur-indente
                     * tout le reste de la procedure. L'entree est rejetee au
                     * compile (mot reserve), mais le formateur tourne avant /
                     * independamment et ne doit pas diverger. Miroir de
                     * blockKindNamedDecl pour les keywords scanner ; un vrai
                     * `SI cond ALORS` n'a jamais de copule en 2e position. */
                    bool keywordOpenerNamedDecl = false;
                    if (ws == firstW && !seenEst) {
                        const char* pk = scan;
                        while (*pk == ' ' || *pk == '\t') pk++;
                        if (is_word_char(*pk)) {
                            const char* kw2 = pk;
                            while (is_word_char(*pk)) pk++;
                            keywordOpenerNamedDecl =
                                fmt_is_copule(kw2, (int)(pk - kw2));
                        }
                    }
                    /*  — keyword de controle (SI, POUR, TANTQUE...)
                     * hors position initiale = identifiant homonyme passe en
                     * argument (`Trace(si)`), pas une ouverture. */
                    bool controlOpenerOutOfPlace =
                        ws != firstW && is_control_flow_opener(ws, wlen);
                    bool skipOpener = isMethodCall
                                   || (seenEst && lineHasEquals)
                                   || widgetKindOutOfPlace
                                   || blockKindOutOfPlace
                                   || keywordOpenerNamedDecl
                                   || controlOpenerOutOfPlace;
                    /*  — type de widget INCONNU (typo, ex. Bidonxyz)
                     * suivi d'un bloc PROPRIETES. word_opens_block ne reconnait
                     * que les types KNOWN (keywords + CONTROL_KINDS) ; un type
                     * inconnu n'est donc pas vu comme ouvreur et le PROPRIETES
                     * qui suit se de-indente. Miroir de l'heuristique
                     * peek-PROPRIETES des types ambigus Image/Liste : si le mot
                     * est le type RHS d'une declaration pure
                     * (`IDENT est un <X>` sans '='), n'est pas deja un opener
                     * reconnu, et est suivi (apres newlines/commentaires) d'un
                     * PROPRIETES → widget DSL, ouvre un bloc. Aligne sur le
                     * compilo qui traite ces types inconnus + bloc
                     * comme des widgets. */
                    if (!lineOpens && seenEst && !lineHasEquals && !isMethodCall
                        && !word_opens_block(ws, wlen)
                        && peek_word_is_proprietes(scan)) {
                        lineOpens = true;
                    }
                    /*  — Fenetre/Etat hors top-level = declaration de
                     * variable locale, pas un ouvreur de bloc DSL.
                     * indent > 0 ⇒ on est dans une procedure/classe/bloc. */
                    bool toplevelDslOutOfPlace =
                        indent > 0 && word_is_toplevel_only_dsl(ws, wlen);
                    if (!skipOpener && !toplevelDslOutOfPlace
                        && word_opens_block(ws, wlen)
                        && dsl_word_really_opens(ws, wlen, scan)) {
                        lineOpens = true;
                        /*  — note si l'opener est interface ou
                         * procedure/fonction (accent+case insensitive). */
                        {
                            char ab[16];
                            int al = scanner_strip_accents(ws, wlen, ab, sizeof(ab));
                            if (al > 0 && al < (int)sizeof(ab)) {
                                for (int i = 0; i < al; i++)
                                    ab[i] = (char)tolower((unsigned char)ab[i]);
                                ab[al] = '\0';
                                if (strcmp(ab, "interface") == 0) {
                                    lineOpensInterface = true;
                                } else if (strcmp(ab, "procedure") == 0
                                        || strcmp(ab, "fonction") == 0
                                        || strcmp(ab, "function") == 0) {
                                    lineOpenerIsProcOrFunc = true;
                                }
                            }
                        }
                    }
                    /*  — detecte 'abstraite'/'abstract' nimporte ou
                     * sur la ligne (typiquement 'PROCEDURE ABSTRAITE Foo()').
                     * Une signature abstraite n'ouvre PAS un body. */
                    if (!lineHasAbstraite && wlen >= 8 && wlen <= 10) {
                        char ab[16];
                        int al = scanner_strip_accents(ws, wlen, ab, sizeof(ab));
                        if (al > 0 && al < (int)sizeof(ab)) {
                            for (int i = 0; i < al; i++)
                                ab[i] = (char)tolower((unsigned char)ab[i]);
                            ab[al] = '\0';
                            if (strcmp(ab, "abstraite") == 0
                             || strcmp(ab, "abstract")  == 0) {
                                lineHasAbstraite = true;
                            }
                        }
                    }
                    /* Detect 'est' (FR) / 'is' (EN) — accent-insensitive lower
                     * compare. Past this token, the rest of the line is the
                     * RHS of a typed declaration. */
                    if (!seenEst && wlen <= 4) {
                        char lw[8];
                        int al = scanner_strip_accents(ws, wlen, lw, sizeof(lw));
                        if (al > 0 && al < (int)sizeof(lw)) {
                            for (int i = 0; i < al; i++) lw[i] = (char)tolower((unsigned char)lw[i]);
                            lw[al] = '\0';
                            if (strcmp(lw, "est") == 0 || strcmp(lw, "is") == 0) {
                                seenEst = true;
                            }
                        }
                    }
                } else {
                    /* ~ for destructors */
                    if (*scan == '~' && first) { firstW = scan; scan++; while(is_word_char(*scan)) scan++; firstLen = (int)(scan - firstW); first = false; continue; }
                    scan++;
                }
            }
        }

        /* Forme inline de l'ouvreur (`SI c ALORS instr`, `CONSTANTE K = 3`) :
         * aucun FIN ne suit, donc pas de cran d'indentation. Verdict rendu par
         * la source unique. */
        if (lineOpens && line_opener_is_inline_form(p)) lineOpens = false;

        /*  — signature sans corps : ne pas ouvrir un bloc.
         *  (a) PROCEDURE/FONCTION dans le body d'une Interface
         *  (b) PROCEDURE ABSTRAITE / FONCTION ABSTRAITE (toute position)
         * On garde lineOpensInterface a part : le mot Interface lui-meme
         * ouvre un bloc normal. */
        if (lineOpens && lineOpenerIsProcOrFunc
            && (insideInterfaceBody || lineHasAbstraite)
            && !lineOpensInterface) {
            lineOpens = false;
        }

        /* Adjust indent BEFORE writing this line */
        bool closes = (firstW && word_closes_block(firstW, firstLen));
        bool mid = (firstW && word_is_mid_block(firstW, firstLen));
        bool startsWithCas = (firstW && word_is_cas_or_autre(firstW, firstLen));
        /* CAS X : newline form (body ouvre un cran de plus). On detecte
         * que la ligne finit par ':' seul (post strip whitespace). */
        bool casOpensBody = startsWithCas && cas_line_ends_with_colon_only(p);

        if (closes) {
            /* Si FIN ferme un SELON avec un CAS body encore ouvert, fermer
             * d'abord ce body (un dedent supplementaire). */
            if (cas_body_open) {
                if (indent > 0) indent--;
                cas_body_open = 0;
            }
            if (indent > 0) {
                indent--;
                /*  — clear le slot de pile a ce niveau. */
                if (indent < 64) block_is_interface[indent] = false;
                if (indent < 64) block_is_procfunc[indent] = false;
            }
        }
        if (startsWithCas && cas_body_open) {
            /* Le CAS courant ferme le body du CAS precedent. */
            if (indent > 0) indent--;
            cas_body_open = 0;
        }
        if (mid) {
            /* Mid-block (SINON, FAIRE, PUBLIC...) acts as close+reopen.
             * Dedent for this line, content after will be at original level. */
            if (indent > 0) indent--;
        }

        /*  — PROCEDURE/FONCTION membre d'une CollectionDeProcedures (ou
         * methode) dont le FIN propre manque : le corps precedent reste ouvert,
         * les membres suivants se comptent comme imbriques et sur-indentent sans
         * borne utile. En code valide un ouvreur proc/func ne vit jamais
         * directement dans le corps d'un autre proc/func : si l'englobant est
         * lui-meme un proc/func, on ferme implicitement le precedent (un dedent)
         * pour degrader gracieusement. No-op sur code valide. */
        if (lineOpens && lineOpenerIsProcOrFunc && !mid
            && indent > 0 && indent <= 64 && block_is_procfunc[indent - 1]) {
            indent--;
            block_is_procfunc[indent] = false;
            block_is_interface[indent] = false;
        }

        /* Brace literal : si la ligne commence par `}`, dedent avant emit.
         * Comptage simple — on regarde le premier non-blank. */
        bool startsWithBraceClose = (*p == '}');
        if (startsWithBraceClose && brace_depth > 0) {
            brace_depth--;
        }

        int emitIndent = indent + brace_depth;

        /* Emit indentation */
        for (int j = 0; j < emitIndent; j++) buf_putc(&out, '\t');

        /* Detect multi-line-string opener BEFORE formatting (so we can toggle
         * state for subsequent lines). */
        bool opensMultiline = line_ends_with_open_bracket(p);
        /*  — la ligne d'ouverture (`s est ... = \`...`) suit
         * l'indentation normale ; ce flag bascule l'etat pour que les LIGNES
         * SUIVANTES soient traitees verbatim. */
        bool opensBacktick = line_opens_backtick_string(p);

        /* Compte le delta `{` vs `}` non-strings/comments sur la ligne pour
         * ajuster brace_depth apres emit. */
        int braceDelta = fmt_line_brace_delta(p, true);

        /* Format dans un buffer intermediaire puis normalisation whitespace.
         * Skip a l interieur d un brace literal multi-ligne (preserve
         * alignement utilisateur). */
        Buffer lineBuf;
        buf_init(&lineBuf);
        p = format_line_content(p, &lineBuf, source);

        /*  — un `\r` final appartient au terminateur CRLF, pas au
         * contenu de la ligne. Le laisser dans le contenu en fait un caractere
         * ordinaire pour emit_normalized_line, qui flushe alors l'espace
         * differee posee apres un operateur binaire (le `>` d'une ligne
         * d'attribut `< Serialise = ... >`) : l'espace atterrit avant le CR.
         * On l'isole ici et on le re-emet avec le newline. */
        bool lineEndsWithCr = (lineBuf.count > 0 &&
                               lineBuf.data[lineBuf.count - 1] == '\r');
        if (lineEndsWithCr) lineBuf.count--;

        bool insideBraceBlock = (brace_depth > 0) && !startsWithBraceClose;
        /* Lignes CAS/AUTRE : copie verbatim. Les comparateurs prefixes canon
         * (`[=`, `[~`, `=]`, `~]`, `~~]`, `<`, `>`, ...) et l'alignement
         * utilisateur (`CAS 10 A 12  : ...`) doivent etre preserves.
         * La canonisation des keywords est deja faite par format_line_content. */
        bool isCasLine = (firstW && line_starts_with_cas_or_autre(firstW));
        if (insideBraceBlock || isCasLine) {
            /* Copie verbatim le contenu deja canonise par format_line_content
             * (casse + accents) : brace-blocks (alignement utilisateur des
             * literaux multi-ligne) et lignes CAS/AUTRE (comparateurs prefixes
             * canon `[=`, `~]`, alignement `CAS 10 A 12  : ...`). Hors ces deux
             * cas, la normalisation whitespace (#13/#15/#16) s'applique
             * toujours. #14 (SI inline indent) reste actif via
             * is_si_inline_line applique avant cette branche. */
            if (lineBuf.count > 0) buf_write(&out, lineBuf.data, lineBuf.count);
        } else {
            emit_normalized_line(lineBuf.data ? lineBuf.data : "",
                                  lineBuf.count, &out, false);
        }
        free(lineBuf.data);

        /* Emit newline (CR restitue : le formateur preserve l'EOL du fichier) */
        if (lineEndsWithCr) buf_putc(&out, '\r');
        buf_putc(&out, '\n');
        if (*p == '\n') p++;

        /* Adjust indent AFTER writing.
         * Mid-block (SINON, FAIRE, PUBLIC...) already re-opens — if the line
         * also contains an opener (e.g. "SINON SI"), suppress the extra open
         * because the elseif chain shares ONE FIN with the outer SI. */
        if (lineOpens && !mid) {
            /*  — push le type de bloc dans la pile AVANT increment.
             * indent est l'index du nouveau slot. */
            if (indent < 64) block_is_interface[indent] = lineOpensInterface;
            if (indent < 64) block_is_procfunc[indent] = lineOpenerIsProcOrFunc;
            indent++;
        }
        if (mid) indent++;  /* re-open after mid-block */
        if (casOpensBody) {
            /* CAS X : newline-form ouvre un niveau d'indent pour son body. */
            indent++;
            cas_body_open = 1;
        }

        brace_depth += braceDelta;
        if (brace_depth < 0) brace_depth = 0;

        if (opensMultiline) in_multiline = true;
        if (opensBacktick) in_backtick = true;
    }

    /* Ensure trailing newline */
    if (out.count > 0 && out.data[out.count - 1] != '\n') {
        buf_putc(&out, '\n');
    }

    char* result = buf_finish(&out);
    /* Post-pass : aligne les '=' dans les blocs PROPRIETES. */
    char* aligned = align_proprietes_blocks(result);
    /* Post-pass : aligne les lignes de continuation (`...` / `_`) sous le RHS
     * de leur tete. Apres la normalisation des `=`. */
    char* continued = align_continuation_lines(aligned);
    g_fmt_st = prevSt;  /* restore l'etat de session precedent */
    return continued;
}

/* ================================================================
 * FORMAT FILE
 * ================================================================ */

int owl_format_file(const char* path, bool dryRun) {
    return owl_format_file_ctx(path, dryRun, NULL);
}

int owl_format_file_ctx(const char* path, bool dryRun,
                        const OwlFormatContext* ctx) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "owl fmt: cannot open '%s'\n", path);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char* source = malloc(sz + 1);
    fread(source, 1, sz, f);
    source[sz] = '\0';
    fclose(f);

    char* formatted = owl_format_ctx(source, ctx);

    if (strcmp(source, formatted) == 0) {
        free(source);
        free(formatted);
        return 0;
    }

    if (dryRun) {
        printf("--- %s (original)\n", path);
        printf("+++ %s (formatted)\n", path);
        printf("%s", formatted);
        free(source);
        free(formatted);
        return 2;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "owl fmt: cannot write '%s'\n", path);
        free(source);
        free(formatted);
        return 1;
    }

    size_t fmtLen = strlen(formatted);
    fwrite(formatted, 1, fmtLen, f);
    fclose(f);

    /* Progression sur stderr (contrat Unix : stderr=progression,
     * stdout=données). Sinon, en pipe owl_format via MCP, ce message
     * pollue les réponses JSON-RPC envoyées sur stdout. */
    fprintf(stderr, "  formatted %s\n", path);
    free(source);
    free(formatted);
    return 0;
}
