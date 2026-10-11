// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_i18n.c — Type `langue` + enum `langueXxx` (multilangue OWL, lot A).
 *
 * Type `langue` :
 *   Alias scalaire chaine. La valeur sous-jacente est un tag BCP 47
 *   ("fr-FR", "en-US", "zh-CN", ...). Le type sert au typage explicite
 *   d'une reference de langue et a l'indexation des `chaineMultilingue`
 *   (lot B). Pas de storage opaque : `lang est une langue = langueFrancais`
 *   produit la chaine "fr-FR" — comparable avec un litteral string et
 *   avec une autre variable `langue`.
 *
 * Constantes `langueXxx` :
 *   34 entrees couvrant 30 langues principales + 4 variantes regionales
 *   courantes. Convention "defaut sans suffixe = variante principale" :
 *     langueFrancais         -> "fr-FR"
 *     langueFrancaisCanada   -> "fr-CA"
 *     langueAnglais          -> "en-US"
 *     langueAnglaisRoyaumeUni-> "en-GB"
 *     langueChinoisSimplifie -> "zh-CN"
 *     langueChinoisTraditionnel-> "zh-TW"
 *   ...
 *
 * Coerce `chaine -> langue` :
 *   Enregistre via owl_register_instance_coerce. Pass-through string
 *   (toute chaine est acceptee comme tag BCP 47 libre — pas de validation
 *   stricte v1 : conforme au cadrage "string libre accepte"). Sur entree
 *   non-chaine, retourne NIL + ErreurDetectee.
 *
 * Hors-scope lot A : type chaineMultilingue, litteral inline, indexation,
 * fallback. Voir  lot B + C.
 */

#include "owl_stdlib.h"
#include "owl_name_normalize.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "sentinels.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ================================================================
 * Table interne : nom de constante -> tag BCP 47.
 *
 * Stockee comme tableau aplati pour rester adressable depuis le lot B
 * (resolution d'une cle d'enum vers son tag BCP 47 au moment de
 * l'indexation). Expose en lecture seule via owl_i18n_lookup_tag().
 *
 * Note canonicalAscii : on enregistre la forme ACCENTUEE comme
 * canonical (cf. invariant n°2). L'alias ASCII est genere automatiquement
 * par owl_registry_register_consts (`langueFrancais` resout vers
 * `langueFrançais`).
 * ================================================================ */

typedef struct {
    const char* name;       /* canonical accentue, ex: "langueFrançais" */
    const char* nameAscii;  /* ASCII fallback, ex: "langueFrancais" */
    const char* bcp47;      /* tag BCP 47 (valeur runtime), ex: "fr-FR" */
    const char* descFR;
    const char* descEN;
    const char* nameEN;     /* alias EN canon (NULL si pas de forme EN connue) */
} LangueEntry;

/* Encodage UTF-8 inline via concat de string literals pour les accents :
 *   "Fran\xc3\xa7" "ais"   = "Français"
 *   "Hong\xc3\xa9" "..."   = ne s'applique pas ici (pas de double-accents).
 *
 * Toutes les valeurs BCP 47 sont ASCII pur (norme RFC 5646). */
static const LangueEntry LANGUE_ENTRIES[] = {
    /* Langues europeennes principales (defaut sans suffixe). */
    {"langueFran\xc3\xa7" "ais",          "langueFrancais",          "fr-FR",
     "Francais (France).", "French (France).", "languageFrench"},
    {"langueAnglais",                     NULL,                       "en-US",
     "Anglais (Etats-Unis).", "English (United States).", "languageEnglish"},
    {"langueAllemand",                    NULL,                       "de-DE",
     "Allemand (Allemagne).", "German (Germany).", "languageGerman"},
    {"langueEspagnol",                    NULL,                       "es-ES",
     "Espagnol (Espagne).", "Spanish (Spain).", "languageSpanish"},
    {"langueItalien",                     NULL,                       "it-IT",
     "Italien (Italie).", "Italian (Italy).", "languageItalian"},
    {"languePortugais",                   NULL,                       "pt-PT",
     "Portugais (Portugal).", "Portuguese (Portugal).", "languagePortuguese"},
    {"langueN\xc3\xa9" "erlandais",       "langueNeerlandais",        "nl-NL",
     "Neerlandais (Pays-Bas).", "Dutch (Netherlands).", "languageDutch"},
    {"languePolonais",                    NULL,                       "pl-PL",
     "Polonais (Pologne).", "Polish (Poland).", "languagePolish"},
    {"langueRoumain",                     NULL,                       "ro-RO",
     "Roumain (Roumanie).", "Romanian (Romania).", "languageRomanian"},
    {"langueSu\xc3\xa9" "dois",           "langueSuedois",            "sv-SE",
     "Suedois (Suede).", "Swedish (Sweden).", "languageSwedish"},
    {"langueDanois",                      NULL,                       "da-DK",
     "Danois (Danemark).", "Danish (Denmark).", "languageDanish"},
    {"langueNorv\xc3\xa9" "gien",         "langueNorvegien",          "nb-NO",
     "Norvegien bokmal (Norvege).", "Norwegian bokmal (Norway).", "languageNorwegian"},
    {"langueFinnois",                     NULL,                       "fi-FI",
     "Finnois (Finlande).", "Finnish (Finland).", "languageFinnish"},
    {"langueGrec",                        NULL,                       "el-GR",
     "Grec (Grece).", "Greek (Greece).", "languageGreek"},
    {"langueTurc",                        NULL,                       "tr-TR",
     "Turc (Turquie).", "Turkish (Turkey).", "languageTurkish"},

    /* Langues slaves. */
    {"langueRusse",                       NULL,                       "ru-RU",
     "Russe (Russie).", "Russian (Russia).", "languageRussian"},
    {"langueUkrainien",                   NULL,                       "uk-UA",
     "Ukrainien (Ukraine).", "Ukrainian (Ukraine).", "languageUkrainian"},
    {"langueTch\xc3\xa8" "que",           "langueTcheque",            "cs-CZ",
     "Tcheque (Republique tcheque).", "Czech (Czech Republic).", "languageCzech"},
    {"langueSlovaque",                    NULL,                       "sk-SK",
     "Slovaque (Slovaquie).", "Slovak (Slovakia).", "languageSlovak"},
    {"langueBulgare",                     NULL,                       "bg-BG",
     "Bulgare (Bulgarie).", "Bulgarian (Bulgaria).", "languageBulgarian"},

    /* Langues asiatiques principales. */
    {"langueChinoisSimplifi\xc3\xa9",     "langueChinoisSimplifie",   "zh-CN",
     "Chinois simplifie (Chine continentale).", "Simplified Chinese (Mainland China).", "languageChineseSimplified"},
    {"langueChinoisTraditionnel",         NULL,                       "zh-TW",
     "Chinois traditionnel (Taiwan).", "Traditional Chinese (Taiwan).", "languageChineseTraditional"},
    {"langueJaponais",                    NULL,                       "ja-JP",
     "Japonais (Japon).", "Japanese (Japan).", "languageJapanese"},
    {"langueCor\xc3\xa9" "en",            "langueCoreen",             "ko-KR",
     "Coreen (Coree du Sud).", "Korean (South Korea).", "languageKorean"},
    {"langueArabe",                       NULL,                       "ar-SA",
     "Arabe (Arabie saoudite).", "Arabic (Saudi Arabia).", "languageArabic"},
    {"langueH\xc3\xa9" "breu",            "langueHebreu",             "he-IL",
     "Hebreu (Israel).", "Hebrew (Israel).", "languageHebrew"},
    {"langueHindi",                       NULL,                       "hi-IN",
     "Hindi (Inde).", "Hindi (India).", "languageHindi"},
    {"langueTha\xc3\xaf",                 "langueThai",               "th-TH",
     "Thai (Thailande).", "Thai (Thailand).", "languageThai"},
    {"langueVietnamien",                  NULL,                       "vi-VN",
     "Vietnamien (Vietnam).", "Vietnamese (Vietnam).", "languageVietnamese"},
    {"langueIndon\xc3\xa9" "sien",        "langueIndonesien",         "id-ID",
     "Indonesien (Indonesie).", "Indonesian (Indonesia).", "languageIndonesian"},

    /* Variantes regionales courantes. */
    {"langueFran\xc3\xa7" "aisCanada",    "langueFrancaisCanada",     "fr-CA",
     "Francais (Canada).", "French (Canada).", "languageFrenchCanada"},
    {"langueAnglaisRoyaumeUni",           NULL,                       "en-GB",
     "Anglais (Royaume-Uni).", "English (United Kingdom).", "languageEnglishUK"},
    {"languePortugaisBr\xc3\xa9" "sil",   "languePortugaisBresil",    "pt-BR",
     "Portugais (Bresil).", "Portuguese (Brazil).", "languagePortugueseBrazil"},
    {"langueEspagnolMexique",             NULL,                       "es-MX",
     "Espagnol (Mexique).", "Spanish (Mexico).", "languageSpanishMexico"},
};

#define LANGUE_ENTRIES_COUNT  (int)(sizeof(LANGUE_ENTRIES) / sizeof(LANGUE_ENTRIES[0]))

/* ================================================================
 * API interne — exposee aux lots B/C via header dedie (a venir).
 * Pour l'instant, statique : pas de consumer hors module.
 * ================================================================ */

/* Resolution constante -> tag BCP 47. Comparaison stricte (utiliser le
 * canon ASCII ou accentue). NULL si non trouve.
 *
 * Sera utilisee au lot B pour traduire `m[langueFrancais]` -> lookup
 * sur la cle "fr-FR" dans le storage chaineMultilingue. */
const char* owl_i18n_lookup_tag_by_constant_name(const char* constName) {
    if (!constName) return NULL;
    for (int i = 0; i < LANGUE_ENTRIES_COUNT; i++) {
        if (strcmp(LANGUE_ENTRIES[i].name, constName) == 0) {
            return LANGUE_ENTRIES[i].bcp47;
        }
        if (LANGUE_ENTRIES[i].nameAscii &&
            strcmp(LANGUE_ENTRIES[i].nameAscii, constName) == 0) {
            return LANGUE_ENTRIES[i].bcp47;
        }
    }
    return NULL;
}

/* ================================================================
 * Factory native __MultilingueNew ( lot B).
 *
 * Invoque par le compilo sur `var est une chaineMultilingue` (sans init)
 * et sur le path litteral inline. Retourne une ObjMultilingue fraiche
 * et vide — coherent avec __AssocNew / __BufferNew (factories internes
 * a prefix "__"). Le mecanisme hasDefault/defaultValue du registry ne
 * convient pas : il pose une Value statique unique, partagee entre
 * toutes les declarations. Pour un type mutable, il faut une allocation
 * a chaque appel.
 * ================================================================ */

static Value fn_multilingue_new(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    return OBJ_VAL(obj_new_multilingue());
}

/* ================================================================
 * Globales i18n ( lot C + ).
 *
 * langueSource : hardcodee a "fr-FR" v1 (futur ticket settings projet
 *   permettra de la configurer). Read-only — pas de setter expose.
 *
 * langueCourante : buffer statique mutable (Option A break-net alpha).
 *    ouvre la mutation via la native publique Langue(<lang>).
 *   Stocke un tag BCP 47 ASCII (RFC 5646), 32 octets sont largement
 *   suffisants pour les tags realistes ("fr-FR", "zh-Hans-CN", ...).
 *
 * Statiques de fichier, non-thread-safe — coherent avec le reste du
 * runtime OWL mono-thread. Si un jour le runtime devient multi-thread,
 * ces globales devront migrer vers vm.state thread-local.
 * ================================================================ */
/* Langue source du projet. Defaut "fr-FR" (aucun projet.owl ou pas de champ
 * Langues) ; posee au boot projet par project_run via owl_i18n_set_project_source
 * depuis la 1ere langue declaree dans le champ Langues (, capturee au
 * compile-time ). Buffer mutable. */
static char        g_owl_langue_source[32]   = "fr-FR";
static char        g_owl_langue_courante[32] = "fr-FR";

/*  — liste complete des langues declarees du projet (tags BCP 47),
 * posee au boot projet par project_run via owl_i18n_set_project_languages depuis
 * la capture compile-time du champ Langues. Vide en mode script (hors projet) :
 * ListeLangues() retombe alors sur ["fr-FR"]. Meme borne que le compilo
 * (PROJET_META_MAX_LANGUES). */
#define OWL_LANGUES_MAX 255
static char        g_owl_langues[OWL_LANGUES_MAX][32];
static int         g_owl_langues_count = 0;

const char* owl_i18n_langue_source(void)   { return g_owl_langue_source; }
const char* owl_i18n_langue_courante(void) { return g_owl_langue_courante; }

/*  — hook de re-render UI au changement de langue. Vit en owl_core
 * comme un simple pointeur de fonction (pas de dependance Qt) ; la couche GUI
 * l'installe au boot. NULL tant qu'aucun backend graphique n'est enregistre. */
static OwlI18nLanguageChangedFn g_i18n_on_lang_changed = NULL;

void owl_i18n_register_on_language_changed(OwlI18nLanguageChangedFn fn) {
    g_i18n_on_lang_changed = fn;
}

void owl_i18n_on_language_changed(void) {
    if (g_i18n_on_lang_changed) g_i18n_on_lang_changed();
}

/* Pose la langue source (et la langue courante initiale) du projet au boot.
 * Appele au boot projet par project_run (langue capturee au compile-time). La
 * langue courante suit la source au demarrage : un projet declarant l'anglais
 * en 1ere langue resout ses chaines multilangues en anglais par defaut. Une
 * mutation ulterieure via Langue(<lang>) reste possible. */
void owl_i18n_set_project_source(const char* tag) {
    if (!tag || !*tag) return;
    /*  — la source est un tag BCP 47 case-insensible : on la stocke en
     * forme canonique pour que LangueSource() et le fallback multilingue
     * s'accordent avec les cles canonicalisees au stockage. */
    char canon[32];
    owl_bcp47_canon(tag, canon, sizeof(canon));
    const char* src = (canon[0] != '\0') ? canon : tag;
    strncpy(g_owl_langue_source, src, sizeof(g_owl_langue_source) - 1);
    g_owl_langue_source[sizeof(g_owl_langue_source) - 1] = '\0';
    strncpy(g_owl_langue_courante, src, sizeof(g_owl_langue_courante) - 1);
    g_owl_langue_courante[sizeof(g_owl_langue_courante) - 1] = '\0';
}

/*  — pose la liste complete des langues declarees du projet au boot,
 * en symetrie de owl_i18n_set_project_source (qui ne fige que la 1ere). Chaque
 * tag est normalise en forme canonique BCP 47, identique a celle de la source,
 * pour garantir ListeLangues()[1] == LangueSource(). Ecrase entierement la liste
 * precedente (tags==NULL ou count<=0 => liste vidée, retour au fallback
 * ["fr-FR"]). */
void owl_i18n_set_project_languages(const char* const* tags, int count) {
    g_owl_langues_count = 0;
    if (!tags || count <= 0) return;
    for (int i = 0; i < count && g_owl_langues_count < OWL_LANGUES_MAX; i++) {
        const char* t = tags[i];
        if (!t || !*t) continue;
        char canon[32];
        owl_bcp47_canon(t, canon, sizeof(canon));
        const char* src = (canon[0] != '\0') ? canon : t;
        strncpy(g_owl_langues[g_owl_langues_count], src,
                sizeof(g_owl_langues[0]) - 1);
        g_owl_langues[g_owl_langues_count][sizeof(g_owl_langues[0]) - 1] = '\0';
        g_owl_langues_count++;
    }
}

/* Exposition lecture du catalogue langueXxx (consommé par --vscode-data).
 * Le buffer statique OwlLangueInfo est rempli paresseusement depuis
 * LANGUE_ENTRIES — pas de duplication de la table, juste un view stable
 * pour les externes qui n'ont pas accès au type interne LangueEntry. */
int owl_i18n_langue_count(void) { return LANGUE_ENTRIES_COUNT; }

const OwlLangueInfo* owl_i18n_langue_at(int index) {
    if (index < 0 || index >= LANGUE_ENTRIES_COUNT) return NULL;
    static OwlLangueInfo view[LANGUE_ENTRIES_COUNT];
    const LangueEntry* e = &LANGUE_ENTRIES[index];
    view[index].name      = e->name;
    view[index].nameAscii = e->nameAscii;
    view[index].bcp47     = e->bcp47;
    view[index].nameEN    = e->nameEN;
    view[index].descFR    = e->descFR;
    view[index].descEN    = e->descEN;
    return &view[index];
}

/* ================================================================
 * Coerce auto chaine -> chaineMultilingue ( lot C).
 *
 * Invoque par __InstanceCoerce sur :
 *   m est une chaineMultilingue = "texte"   -- declaration init explicite
 *
 * IS_STRING(v)      -> nouvelle ObjMultilingue avec langueSource posee
 * IS_MULTILINGUE(v) -> pass-through (cas `m est une cM = autreM`, deja
 *                      construit en amont, on relie tel quel)
 * IS_NIL(v)         -> ObjMultilingue vide
 * sinon             -> erreur runtime + ObjMultilingue vide
 *
 * NOTE — reaffectation `m = "texte"` apres declaration : passe PAR
 * __MultilingueAssignSource (ci-dessous), pas par ce parser. La distinction
 * est requise par scenario_coerce_reaffectation_ecrase_source (les autres
 * langues doivent etre conservees ; ici on en cree une nouvelle).
 * ================================================================ */
static Value parse_to_multilingue(Value v) {
    if (IS_MULTILINGUE(v)) {
        owl_clear_error();
        return v;
    }
    if (IS_STRING(v)) {
        owl_clear_error();
        /*  +  : push v EN PREMIER (arg passe par valeur,
         * non rooted sur C-stack). obj_new_multilingue + obj_copy_string
         * peuvent grow → GC → v collected (ObjString fresh non rooted).
         * Bug exhibe par test_champ_libelle_assertions : v collected,
         * slot reutilise par "Annuler" cree juste apres → champ Valider
         * recoit Annuler. */
        vm_push(v);
        ObjMultilingue* m = obj_new_multilingue();
        vm_push(OBJ_VAL(m));
        const char* lang = owl_i18n_langue_source();
        ObjString* key = obj_copy_string(lang, (int)strlen(lang));
        obj_assoc_set(m->storage, key, v);
        vm_pop(); /* m */
        vm_pop(); /* v */
        return OBJ_VAL(m);
    }
    if (IS_NIL(v)) {
        owl_clear_error();
        return OBJ_VAL(obj_new_multilingue());
    }

    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "chaineMultilingue",
                          "native", "cha\xc3\xae" "neMultilingue",
                          "arg", "source",
                          "expected", "chaine ou chaineMultilingue",
                          "actual", "type non pris en charge",
                          NULL);
    return OBJ_VAL(obj_new_multilingue());
}

/* ================================================================
 * Native __MultilingueAssignSource(m, val) ( lot C).
 *
 * Mute m EN PLACE pour respecter le scenario "reaffectation conserve
 * les autres langues" :
 *   m = {langueFrancais: "X", langueAnglais: "Y"}
 *   m = "Z"   -- m[fr-FR] = "Z", m[en-US] reste "Y"
 *
 * Le compilateur emet cette native a la place de OP_SET_LOCAL/GLOBAL
 * sur affectation `m = expr` quand m est typee chaineMultilingue.
 *
 * Signature : (m, val) -> NIL. Pas de valeur de retour utile, on POP.
 * Le param `langue` (allowedBase=&E_langueXxx) ne sert pas a la valeur
 * actuelle de l'impl (la langue source est lue via globale) — il existe
 * pour le ticket 2 ou la signature acceptera optionnellement la langue
 * cible. Conserve a ce titre. (E_langueXxx est par ailleurs reference
 * proprement via chaineMultilingue.literalKeysEnum et les params des
 * natives publiques Langue/LangueSensEcriture — la regle 18 n'a plus
 * besoin de ce param pour etre satisfaite.)
 * ================================================================ */
static Value fn_multilingue_assign_source(int argCount, Value* args) {
    (void)argCount;
    if (!IS_MULTILINGUE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "__MultilingueAssignSource",
                              "native", "__MultilingueAssignSource",
                              "arg", "m",
                              "expected", "chaineMultilingue",
                              "actual", "type non pris en charge",
                              NULL);
        return NIL_VAL;
    }
    ObjMultilingue* m = AS_MULTILINGUE(args[0]);

    /* Resolution de la valeur a poser sur la langue source. IS_NIL est
     * traite comme chaine vide (cohérence canon : Null en context chaine). */
    ObjString* val;
    if (IS_STRING(args[1])) {
        val = AS_STRING(args[1]);
    } else if (IS_NIL(args[1])) {
        val = obj_copy_string("", 0);
    } else if (IS_MULTILINGUE(args[1])) {
        /* `m = autreM` : on resout autreM via la langue courante et on
         * pose le resultat sur la langue source de m. Comportement
         * defensif minimal (scenario non couvert, mais pas de crash). */
        Value resolved = multilingue_resolve(AS_MULTILINGUE(args[1]),
                                              owl_i18n_langue_courante(),
                                              owl_i18n_langue_source());
        val = IS_STRING(resolved) ? AS_STRING(resolved) : obj_copy_string("", 0);
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "__MultilingueAssignSource",
                              "native", "__MultilingueAssignSource",
                              "arg", "val",
                              "expected", "chaine",
                              "actual", "type non pris en charge",
                              NULL);
        return NIL_VAL;
    }

    /*  : push val avant obj_copy_string(lang) qui peut grow → GC.
     * val peut etre une ObjString fresh (cas L303 obj_copy_string("") ou
     * L311 multilingue_resolve), non rooted apres retour. */
    vm_push(OBJ_VAL(val));
    const char* lang = owl_i18n_langue_source();
    ObjString* key = obj_copy_string(lang, (int)strlen(lang));
    obj_assoc_set(m->storage, key, OBJ_VAL(val));
    vm_pop();
    return NIL_VAL;
}

/* ================================================================
 * Coerce parser : chaine -> langue (alias scalaire chaine).
 *
 * Pass-through pour toute Value chaine (le tag BCP 47 est libre v1).
 * Sur entree non-chaine valide, signale une erreur runtime et retourne
 * NIL_VAL (symetrique du pattern parse_to_uuid sur input mal forme).
 * ================================================================ */

static Value parse_to_langue(Value v) {
    /* Pass-through chaine : toute valeur deja stringifiee est acceptee
     * comme tag BCP 47. Pas de validation stricte du format en v1 :
     * cadrage permet les tags libres type "fr-jura". */
    if (IS_STRING(v)) {
        owl_clear_error();
        return v;
    }

    /* Sur Null on retourne une chaine vide : symetrique du comportement
     * scalaire d'une variable typee non-initialisee. Pas d'erreur (Null
     * est un etat legitime). */
    if (IS_NIL(v)) {
        owl_clear_error();
        return OBJ_VAL(obj_copy_string("", 0));
    }

    /* Autre type (entier, booleen, instance...) : erreur. */
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "langue",
                          "native", "langue",
                          "arg", "source",
                          "expected", "chaine BCP 47 ou constante langueXxx",
                          "actual", "type non pris en charge",
                          NULL);
    return OBJ_VAL(obj_copy_string("", 0));
}

/* ================================================================
 * Registry declarations
 * ================================================================ */

/* Enum langueXxx — declare statiquement les noms (canonical accentue).
 *
 * Au lot A, l'enum n'est PAS enregistre dans le registry (la regle 18
 * du self-test exige qu'un enum soit cable a au moins un field ou param,
 * ce qui arrive au lot B avec chaineMultilingue). La struct reste
 * exposee comme symbole extern pour que le lot B puisse :
 *   1. Brancher chaineMultilingue.<field>.allowedBase = &E_langueXxx
 *   2. Appeler owl_registry_register_enums(&E_langueXxx, 1)
 * L'init populate LANGUE_ENUM_VALUES avant que le lot B ne register. */
static const char* LANGUE_ENUM_VALUES[LANGUE_ENTRIES_COUNT + 1];

const OwlEnum E_langueXxx = {
    .name      = "langueXxx",
    .nameAscii = "langueXxx",
    .nameEN    = "languageXxx",
    .category  = "Internationalisation",
    .descFR    = "Langues reconnues pour les chaines multilangues. "
                 "Chaque constante mappe un tag BCP 47 (\"fr-FR\", \"en-US\", "
                 "\"zh-CN\"...). Convention : defaut sans suffixe (langueFrancais "
                 "= variante principale), suffixe region pour les variantes "
                 "(langueFrancaisCanada).",
    .descEN    = "Recognized languages for multilingual strings. Each constant "
                 "maps to a BCP 47 tag (\"fr-FR\", \"en-US\", \"zh-CN\"...). "
                 "Convention: default with no suffix is the main variant "
                 "(langueFrancais = main), region suffix for variants "
                 "(langueFrancaisCanada).",
    .values    = LANGUE_ENUM_VALUES,
    .combinable      = false,
    /* Opt-out sentinel zone : les valeurs sont des CHAINES (pas des entiers),
     * la regle 20 du self-test ne s'applique pas (lecture conditionnee a
     * IS_INTEGER). force_small_int reste a false par defaut. */
};

/* Alias EN pour le type `langue` lui-meme. Invariant n°3 OWL : tout
 * symbole user-facing a une forme FR + EN. `language` (anglais) ↔
 * `langue` (FR canon). */
static const char* const ALIAS_langue[] = { "language", NULL };

/* Alias EN pour `chaineMultilingue`. Invariant n°3 OWL.
 *
 * Note canonicalAscii : on enregistre la forme accentuee comme canonical
 * et l'ASCII comme nameAscii — coherent avec invariant n°2 et lot A. */
static const char* const ALIAS_chaineMultilingue[] = { "multilingualString", NULL };

static const OwlTypeDef LANGUE_TYPES[] = {
    { .canonical      = "langue",
      .canonicalAscii = "langue",
      .aliases        = ALIAS_langue,
      .category       = "Internationalisation",
      .descFR         = "Reference typee vers une langue. Stockee comme tag BCP 47 "
                        "(\"fr-FR\", \"en-US\", ...). Initialisable par une constante "
                        "langueXxx ou directement par un tag string libre.",
      .descEN         = "Typed reference to a language. Stored as a BCP 47 tag "
                        "(\"fr-FR\", \"en-US\", ...). Initializable from a langueXxx "
                        "constant or directly from a free string tag.",
      .fields         = NULL,
      .coercible      = true,
      /*  : alias scalaire registry sans template. Declaration
       * sans init -> Null (handle non encore branche sur une langue
       * specifique). Coherent avec zipArchive / MockNative. */
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "language",
      /*  — genre lu par l'article EN->FR de `owl translate` : sans lui
       * `a language` revient en `est a langue` (forme mixte). Articles releves
       * dans le depot : 37 `est une`, 1 `est un`. */
      .genre          = OWL_GENRE_F },   /* une langue */

    /*  lot B : type first-class chaineMultilingue. Dictionnaire
     * tag BCP 47 -> texte traduit, indexable par constante langueXxx ou
     * tag string libre. Materialise par ObjMultilingue cote runtime ;
     * declaration sans init -> instance fresh via __MultilingueNew()
     * (cf branche dediee dans compiler.c). On laisse hasDefault=false :
     * le defaultValue ne peut pas etre un objet alloue (singleton partage
     * sinon, ce qui casse la mutabilite par declaration). */
    { .canonical      = "cha\xc3\xae" "neMultilingue",  /* chaîneMultilingue (UTF-8) */
      .canonicalAscii = "chaineMultilingue",
      .aliases        = ALIAS_chaineMultilingue,
      .category       = "Internationalisation",
      .descFR         = "Dictionnaire de chaines traduites, indexable par constante "
                        "langueXxx (\"fr-FR\", \"en-US\", ...) ou par tag BCP 47 libre. "
                        "Lecture sur cle absente retombe sur la langue source puis sur "
                        "une chaine vide si la source est elle-meme absente.",
      .descEN         = "Dictionary of translated strings, indexable by langueXxx "
                        "constant (\"fr-FR\", \"en-US\", ...) or by free BCP 47 tag. "
                        "Reading a missing key falls back to the source language, "
                        "then to an empty string if the source language itself is "
                        "absent.",
      .fields         = NULL,
      /*  lot C : coerce auto chaine ↔ chaineMultilingue
       * enregistre via owl_register_instance_coerce dans
       * stdlib_i18n_init. coercible=true marque le type comme
       * documente "se laisse coercer depuis chaine" pour --doc. */
      .coercible      = true,
      .hasDefault     = false,
      /*  : cle propre du literal `{}` = valeurs de E_langueXxx.
       * Le LSP propose langueFrancais/langueAnglais/... dans
       * `s est une chaineMultilingue = { <curseur> }`. Reference l'enum
       * proprement (rule 18 le compte). */
      .literalKeysEnum = &E_langueXxx,
      .nameEN          = "multilingualString",
      .genre           = OWL_GENRE_F },   /* une chaîne — 109 `est une` releves */
};

/* ================================================================
 *  — Natives publiques Langue / LangueSource / LangueSysteme /
 *             LangueSensEcriture / ListeLangues.
 *
 * Enum sensEcriture unifie : declare dans ui_catalog.c (consumer UI
 * historique : Champ..SensEcriture), reutilise ici comme retour de
 * LangueSensEcriture(). Valeurs entieres OWL_SE_* (sentinels.h) ;
 * la comparaison `sens = sensGaucheDroite` cote OWL fonctionne car
 * les constantes `sensGaucheDroite`/etc. partagent les memes
 * sentinelles. */

extern const OwlEnum E_sensEcriture;          /* ui_catalog.c */
extern void owl_platform_detect_locale(char* buf, int bufsize);  /* platform.c */

/* Resolution d'un argument langue : accepte une constante langueXxx
 * (string BCP 47 deja resolue par le scanner via vm.globals) ou un tag
 * BCP 47 libre. Pour un argument Value :
 *   IS_STRING  -> AS_CSTRING (pass-through, le tag est deja la valeur)
 *   autre      -> NULL (erreur loggee par le caller)
 *
 * Note : le scanner OWL resout les constantes globales `langueXxx` en
 * leur Value stockee (cf stdlib_i18n_init etape 1) — au moment ou la
 * native recoit l'arg, c'est deja la string BCP 47. */
static const char* resolve_lang_arg(Value v) {
    if (IS_STRING(v)) return AS_CSTRING(v);
    return NULL;
}

/* Langue() / Langue(<lang>) — getter ET setter.
 *
 * Arity 0 : retourne la langue courante (tag BCP 47).
 * Arity 1 : pose la langue courante, retourne l'ANCIENNE valeur
 *   (pattern canon Nation()). Accepte constante langueXxx ou tag libre.
 *   Chaine vide acceptee (defensif — Option A : pose litteralement "",
 *   le caller voit "" sur le prochain getter ; pas de crash).
 */
static Value fn_langue(int argCount, Value* args) {
    if (argCount == 0) {
        return OBJ_VAL(obj_copy_string(g_owl_langue_courante,
                                       (int)strlen(g_owl_langue_courante)));
    }
    /* argCount >= 1 : setter, capture ancienne valeur d'abord. */
    Value ancienne = OBJ_VAL(obj_copy_string(g_owl_langue_courante,
                                             (int)strlen(g_owl_langue_courante)));
    const char* nouv = resolve_lang_arg(args[0]);
    if (!nouv) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Langue",
                              "native", "Langue",
                              "arg", "langue",
                              "expected", "chaine ou constante langueXxx",
                              "actual", "type non pris en charge",
                              NULL);
        return ancienne;
    }
    /* Tronque silencieusement si > 31 chars (tag BCP 47 realiste tient
     * largement, "zh-Hans-CN" = 10 chars). strncpy + NUL final garanti. */
    strncpy(g_owl_langue_courante, nouv, sizeof(g_owl_langue_courante) - 1);
    g_owl_langue_courante[sizeof(g_owl_langue_courante) - 1] = '\0';
    /*  — hot-switch : si la langue a reellement change, re-render de
     * l'UI vivante (re-resolution + re-push des props chaineMultilingue). No-op
     * hors GUI (callback NULL). Garde-fou "reellement change" pour eviter une
     * passe couteuse sur un set redondant (Langue(memeLangue)). */
    if (strcmp(g_owl_langue_courante, AS_CSTRING(ancienne)) != 0) {
        owl_i18n_on_language_changed();
    }
    return ancienne;
}

/* LangueSource() — readonly. Retourne la langue source du projet (1ere langue
 * du champ Langues, posee au boot ; defaut "fr-FR" sans projet.owl/Langues). */
static Value fn_langue_source(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_copy_string(g_owl_langue_source,
                                   (int)strlen(g_owl_langue_source)));
}

/* LangueSysteme() — detection OS cross-platform via owl_platform_detect_locale
 * (platform.c). Fallback "en-US" gere cote platform. */
static Value fn_langue_systeme(int argCount, Value* args) {
    (void)argCount; (void)args;
    char buf[64];
    buf[0] = '\0';
    owl_platform_detect_locale(buf, (int)sizeof(buf));
    if (!buf[0]) {
        strncpy(buf, "en-US", sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    }
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* LangueSensEcriture(<lang>) — retourne une constante sensEcriture
 * (sensGaucheDroite/sensDroiteGauche). Detection RTL via prefixe BCP 47
 * sur les 4 langues RTL principales (ar-*, he-*, fa-*, ur-*). Toutes
 * les autres -> LTR. Pas de table exhaustive (acceptable v1, couvre
 * 99% des cas reels — yi/ku/ps non geres v1, futur si demande). */
static Value fn_langue_sens_ecriture(int argCount, Value* args) {
    (void)argCount;
    const char* tag = resolve_lang_arg(args[0]);
    if (!tag) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "LangueSensEcriture",
                              "native", "LangueSensEcriture",
                              "arg", "langue",
                              "expected", "chaine ou constante langueXxx",
                              "actual", "type non pris en charge",
                              NULL);
        return INTEGER_VAL(OWL_SE_DEFAUT);
    }
    /* Prefixe 2 chars + soit fin de chaine soit '-'. Match insensible casse
     * (BCP 47 est officiellement case-insensitive sur la sous-balise langue,
     * mais on garde lowercase strict — toutes nos constantes sont en
     * lowercase canon ISO-639). */
    bool rtl = false;
    if ((tag[0] == 'a' && tag[1] == 'r') ||  /* arabe */
        (tag[0] == 'h' && tag[1] == 'e') ||  /* hebreu */
        (tag[0] == 'f' && tag[1] == 'a') ||  /* persan / farsi */
        (tag[0] == 'u' && tag[1] == 'r')) {  /* ourdou */
        if (tag[2] == '\0' || tag[2] == '-' || tag[2] == '_') {
            rtl = true;
        }
    }
    return INTEGER_VAL(rtl ? OWL_SE_DROITE_GAUCHE : OWL_SE_GAUCHE_DROITE);
}

/* ListeLangues() — tableau des langues declarees du projet (tags BCP 47), dans
 * l'ordre de declaration ([1] = langue source). La liste est posee au boot
 * projet depuis le champ Langues (capture compile-time). En mode script / hors
 * projet (liste vide), retombe sur ["fr-FR"] (defaut historique). Le tableau
 * retourne est mutable cote caller — pas une vue figee. */
static Value fn_liste_langues(int argCount, Value* args) {
    (void)argCount; (void)args;
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));  /* GC root */
    if (g_owl_langues_count > 0) {
        for (int i = 0; i < g_owl_langues_count; i++) {
            obj_array_push(arr, OBJ_VAL(obj_copy_string(
                g_owl_langues[i], (int)strlen(g_owl_langues[i]))));
        }
    } else {
        obj_array_push(arr, OBJ_VAL(obj_copy_string("fr-FR", 5)));
    }
    vm_pop();
    return OBJ_VAL(arr);
}

/* ================================================================
 * INIT
 * ================================================================ */

void stdlib_i18n_init(void) {
    /* 1) Constantes langueXxx — chaque entree est une string BCP 47.
     *    Buffer statique car Value(string) requiert vm_init() avant que
     *    obj_copy_string ne fonctionne (static initializer impossible).
     *    Meme pattern que MIME_TYPES dans stdlib_http.c. */
    static OwlConstDef langueConsts[LANGUE_ENTRIES_COUNT];

    for (int i = 0; i < LANGUE_ENTRIES_COUNT; i++) {
        const LangueEntry* e = &LANGUE_ENTRIES[i];

        /* Enregistre dans vm.globals : permet `lang = langueFrancais`
         * a la VM. owl_register_global_string copie la string. */
        owl_register_global_string(e->name, e->bcp47, (int)strlen(e->bcp47));

        /* Recupere la Value pour la transmettre au registre (utilise par
         * le LSP autocomplete + cli `owl explain`). */
        ObjString* k = obj_copy_string(e->name, (int)strlen(e->name));
        Value stored = NIL_VAL;
        Value tmp;
        if (table_get(&vm.globals, k, &tmp)) stored = tmp;

        langueConsts[i].name     = e->name;
        langueConsts[i].category = "Internationalisation";
        langueConsts[i].value    = stored;
        langueConsts[i].descFR   = e->descFR;
        langueConsts[i].descEN   = e->descEN;
        langueConsts[i].nameEN   = e->nameEN;
    }
    owl_registry_register_consts(langueConsts, LANGUE_ENTRIES_COUNT);

    /* 2) Enum langueXxx — populate LANGUE_ENUM_VALUES depuis la table.
     *    NULL-terminated. La declaration runtime de l'enum est differee au
     *    lot B : la regle 18 du self-test (enum_unreferenced)
     *    exige qu'un enum soit cable a un field ou param. Au lot A, aucun
     *    consumer n'existe encore — l'enum sera enregistre quand
     *    chaineMultilingue arrivera avec ses fields/methodes. Les constantes
     *    individuelles restent visibles via le registre consts. */
    for (int i = 0; i < LANGUE_ENTRIES_COUNT; i++) {
        LANGUE_ENUM_VALUES[i] = LANGUE_ENTRIES[i].name;
    }
    LANGUE_ENUM_VALUES[LANGUE_ENTRIES_COUNT] = NULL;

    /* 3) Type `langue` — declaration registry. Alias scalaire chaine, la
     *    coerce produit directement la string BCP 47. Pas de template
     *    ObjInstance : le mecanisme hasDefault/defaultValue du
     *    compilateur emet `defaultValue = NIL_VAL` sur declaration sans
     *    init explicite. */
    owl_registry_register_types(LANGUE_TYPES,
                                (int)(sizeof(LANGUE_TYPES) / sizeof(LANGUE_TYPES[0])));

    /* 4) Parser de coerce chaine -> langue. Le compilo emet
     *    __InstanceCoerce(v, "langue") sur `var est une langue = expr`. */
    owl_register_instance_coerce("langue", parse_to_langue);

    /* 5) Factory __MultilingueNew ( lot B). Le compilo emet
     *    GET_GLOBAL __MultilingueNew + CALL 0 sur :
     *      - `var est une chaineMultilingue`        (declaration sans init)
     *      - `var est une chaineMultilingue = {...}` (litteral inline,
     *        suivi du remplissage par OP_ARRAY_SET sur chaque entree). */
    owl_define_native("__MultilingueNew", fn_multilingue_new, 0);

    /* 6) Parser de coerce chaine -> chaineMultilingue ( lot C).
     *    Le compilo emet __InstanceCoerce(v, "chaineMultilingue") sur
     *    `m est une chaineMultilingue = expr` (path init explicite, hors
     *    litteral inline `{}` deja gere lot B). Enregistre les DEUX
     *    formes (accentue + ASCII) car compiler.c::composedTypeName copie
     *    le source utilisateur tel quel — `chaîneMultilingue` accentue OU
     *    `chaineMultilingue` ASCII selon ce que l'utilisateur tape. */
    owl_register_instance_coerce("cha\xc3\xae" "neMultilingue", parse_to_multilingue);
    owl_register_instance_coerce("chaineMultilingue", parse_to_multilingue);

    /* 7-8) Native __MultilingueAssignSource ( lot C) + enum
     *    E_langueXxx. Enregistree DIRECTEMENT via le registry (qui appelle
     *    owl_define_native_wl en interne) plutot que via owl_define_native
     *    standalone : ainsi la signature consomme E_langueXxx via
     *    OwlParam.allowedBase et satisfait la regle 18 du self-test
     *    (enum_unreferenced). Le compilo emettra GET_GLOBAL +
     *    __MultilingueAssignSource + CALL 2 + POP sur `m = "texte"` quand
     *    m est typee chaineMultilingue (cf compiler.c::named_variable
     *    branche d'affectation). */
    static const OwlParam p_multilingue_assign_source[] = {
        {"m",   "cha\xc3\xae" "neMultilingue",
         "chaineMultilingue cible, mutee en place sur la langue source",
         NULL, NULL},
        {"val", "chaine",
         "texte affecte a la langue source ou chaineMultilingue source",
         NULL, NULL},
        {"langue", "langue",
         "langue cible (actuellement ignore : la langue source globale "
         "est utilisee)",
         &E_langueXxx, NULL},
        {0}
    };
    /* Liste param dediee a la forme 2 args (m, val) : sans le 3e param
     * `langue` reserve futur. L'impl ne lit que args[0] et args[1]. */
    static const OwlParam p_multilingue_assign_2[] = {
        {"m",   "cha\xc3\xae" "neMultilingue",
         "chaineMultilingue cible, mutee en place sur la langue source",
         NULL, NULL},
        {"val", "chaine",
         "texte affecte a la langue source ou chaineMultilingue source",
         NULL, NULL},
        {0}
    };
    /* Formes discretes 2 ou 3 args : v1 le compilo appelle toujours avec
     * 2 args (m, val). La forme 3 args anticipe le futur ticket 2 ou le
     * compilo emettra un 3e arg (langue cible). La forme 3 args (porteuse
     * de E_langueXxx) est placee EN PREMIER : le loop self-test s'arrete
     * a la 1re forme et y trouve l'enum reference. Note : E_langueXxx est
     * par ailleurs reference via chaineMultilingue.literalKeysEnum. */
    static const OwlSignature sig_multilingue_assign_source[] = {
        {3, p_multilingue_assign_source, "rien",
         "mutation en place — pas de valeur retournee."},
        {2, p_multilingue_assign_2, "rien",
         "mutation en place — pas de valeur retournee."},
        {0}
    };
    /*  — natives publiques i18n. */

    /* Langue() / Langue(<lang>) — getter + setter. */
    static const OwlParam p_langue[] = {
        {"langue", "langue",
         "Nouvelle langue courante (constante langueXxx ou tag BCP 47).",
         &E_langueXxx, NULL},
        {0}
    };
    static const OwlSignature sig_langue[] = {
        { 0, NULL,     "chaine",
          "Retourne la langue courante (tag BCP 47)." },
        { 1, p_langue, "chaine",
          "Pose la langue courante, retourne l'ancienne valeur." },
        {0}
    };

    /* LangueSource() — getter readonly. */
    static const OwlSignature sig_langue_source[] = {
        { 0, NULL, "chaine",
          "Langue source du projet (tag BCP 47), definie par la 1ere langue "
          "du champ Langues du projet ; vaut \"fr-FR\" par defaut en mode "
          "script (hors projet)." },
        {0}
    };

    /* LangueSysteme() — detection OS. */
    static const OwlSignature sig_langue_systeme[] = {
        { 0, NULL, "chaine",
          "Langue OS detectee (tag BCP 47, fallback \"en-US\")." },
        {0}
    };

    /* LangueSensEcriture(<lang>) — retourne un sensEcriture. */
    static const OwlParam p_langue_sens[] = {
        {"langue", "langue",
         "Langue dont on cherche le sens d'ecriture.",
         &E_langueXxx, NULL},
        {0}
    };
    static const OwlSignature sig_langue_sens[] = {
        { 1, p_langue_sens, "sens\xc3\x89""criture",
          "sensGaucheDroite (LTR) ou sensDroiteGauche (RTL)." },
        {0}
    };

    /* ListeLangues() — tableau de langue. */
    static const OwlSignature sig_liste_langues[] = {
        { 0, NULL, "tableau de langue",
          "Tags BCP 47 des langues declarees du projet, dans l'ordre de "
          "declaration (le 1er est la langue source)." },
        {0}
    };

    static const OwlNativeDef DEFS_I18N[] = {
        { .canonical   = "__MultilingueAssignSource",
          .category    = "Internationalisation",
          .descFR      = "Pose la valeur sur la langue source d'une "
                         "chaineMultilingue, sans toucher aux autres "
                         "traductions. Interne, emis par le compilateur sur "
                         "`m = \"...\"` quand m est de type chaineMultilingue.",
          .descEN      = "Set the value on the source language of a "
                         "multilingualString, leaving other translations "
                         "untouched. Internal, emitted by the compiler on "
                         "`m = \"...\"` when m is of type multilingualString.",
          .signatures  = sig_multilingue_assign_source,
          .impl        = fn_multilingue_assign_source,
          .flags       = OWL_NAT_INTERNAL,
        },
        { .canonical   = "Langue",
          .nameEN      = "Language",
          .category    = "Internationalisation",
          .descFR      = "Lit ou modifie la langue courante du runtime. "
                         "Sans argument, retourne le tag BCP 47 courant. "
                         "Avec un argument (constante langueXxx ou tag libre), "
                         "pose la langue et retourne l'ancienne valeur.",
          .descEN      = "Reads or sets the runtime current language. "
                         "Without argument, returns the current BCP 47 tag. "
                         "With an argument (langueXxx constant or free tag), "
                         "sets the language and returns the previous value.",
          .signatures  = sig_langue,
          .impl        = fn_langue,
        },
        { .canonical   = "LangueSource",
          .nameEN      = "SourceLanguage",
          .category    = "Internationalisation",
          .descFR      = "Retourne la langue source du projet (langue dans "
                         "laquelle les textes ont ete saisis a l'origine), "
                         "sous forme de tag BCP 47. Definie par la 1ere langue "
                         "du champ Langues du projet ; vaut \"fr-FR\" par "
                         "defaut en mode script (hors projet).",
          .descEN      = "Returns the project source language (the language "
                         "in which texts were originally written) as a BCP 47 "
                         "tag. Set from the first language of the project's "
                         "Langues field; defaults to \"fr-FR\" in script mode "
                         "(outside a project).",
          .signatures  = sig_langue_source,
          .impl        = fn_langue_source,
        },
        { .canonical   = "LangueSyst\xc3\xa8""me",
          .canonicalAscii = "LangueSysteme",
          .nameEN      = "SystemLanguage",
          .category    = "Internationalisation",
          .descFR      = "Detecte la langue du systeme d'exploitation et "
                         "renvoie un tag BCP 47 (\"fr-FR\", \"en-US\", ...). "
                         "Fallback \"en-US\" si la detection echoue (variable "
                         "d'environnement absente, locale C).",
          .descEN      = "Detects the operating system language and returns "
                         "a BCP 47 tag (\"fr-FR\", \"en-US\", ...). Falls "
                         "back to \"en-US\" if detection fails (environment "
                         "variable missing, C locale).",
          .signatures  = sig_langue_systeme,
          .impl        = fn_langue_systeme,
        },
        { .canonical   = "LangueSens\xc3\x89""criture",
          .canonicalAscii = "LangueSensEcriture",
          .nameEN      = "LanguageWritingDirection",
          .category    = "Internationalisation",
          .descFR      = "Retourne le sens d'ecriture d'une langue : "
                         "sensDroiteGauche (RTL) pour ar-*, he-*, fa-*, "
                         "ur-* ; sensGaucheDroite (LTR) pour toutes les "
                         "autres. Accepte une constante langueXxx ou un "
                         "tag BCP 47 libre.",
          .descEN      = "Returns the writing direction of a language: "
                         "sensDroiteGauche (RTL) for ar-*, he-*, fa-*, "
                         "ur-*; sensGaucheDroite (LTR) for all others. "
                         "Accepts a langueXxx constant or a free BCP 47 tag.",
          .signatures  = sig_langue_sens,
          .impl        = fn_langue_sens_ecriture,
        },
        { .canonical   = "ListeLangues",
          .nameEN      = "LanguageList",
          .category    = "Internationalisation",
          .descFR      = "Retourne le tableau des langues declarees du projet "
                         "(tags BCP 47 du champ Langues), dans l'ordre de "
                         "declaration : le 1er element est la langue source. En "
                         "mode script (hors projet), renvoie [\"fr-FR\"].",
          .descEN      = "Returns the array of the project's declared languages "
                         "(BCP 47 tags from the Langues field), in declaration "
                         "order: the first element is the source language. In "
                         "script mode (outside a project), returns [\"fr-FR\"].",
          .signatures  = sig_liste_langues,
          .impl        = fn_liste_langues,
        },
    };
    owl_registry_register_module(DEFS_I18N,
        (int)(sizeof(DEFS_I18N) / sizeof(DEFS_I18N[0])));

    /* Enum E_langueXxx : consumer historique sur __MultilingueAssignSource
     * + nouveaux consumers  sur Langue() et LangueSensEcriture(). */
    static const OwlEnum* const ENUMS_I18N[] = { &E_langueXxx };
    owl_registry_register_enums(ENUMS_I18N,
        (int)(sizeof(ENUMS_I18N) / sizeof(ENUMS_I18N[0])));
}
