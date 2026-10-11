// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_formatter_h
#define owl_formatter_h

#include <stdbool.h>

/* Format an OWL source string in-place (canonical style).
 * Returns a newly allocated formatted string. Caller must free().
 *
 * Normalisation appliquee : casse + accents canoniques, indent (#14 SI
 * inline inclus), et whitespace (collapse espaces multiples, 1 espace
 * autour des operateurs binaires, 1 espace apres virgule, pas d'espace
 * interne aux parentheses). Les brace-blocks et les lignes CAS/AUTRE
 * restent copies verbatim (alignement utilisateur + comparateurs
 * prefixes canon preserves). */
char* owl_format(const char* source);

/* ================================================================
 * Contexte de casse cross-fichier
 *
 * Le formatter ne traite qu'UN fichier a la fois, mais une declaration
 * (type / membre) vit souvent dans un fichier voisin. Un contexte
 * read-only seede depuis le prescan projet alimente la canonisation de
 * casse/accents des identifiants declares ailleurs. Le harvest local de
 * la source courante reste prioritaire ; le contexte complete.
 * ================================================================ */

typedef struct OwlFormatContext OwlFormatContext;

OwlFormatContext* owl_format_context_new(void);
void owl_format_context_free(OwlFormatContext* ctx);

/* Enregistre un type user (classe/struct/enum) declare ailleurs. */
void owl_format_context_add_type(OwlFormatContext* ctx, const char* name);

/* Enregistre un membre (champ/constante de classe ou struct) declare
 * ailleurs. `declType` est le type declare du membre (pour resoudre les
 * chaines a.b.c) ou NULL/"" si constante. */
void owl_format_context_add_member(OwlFormatContext* ctx, const char* owner,
                                   const char* name, const char* declType);

/* Enregistre un membre d'enumeration declare ailleurs. Reference bare
 * (sans `.`), sa casse de declaration est canonique. */
void owl_format_context_add_enum_member(OwlFormatContext* ctx,
                                        const char* owner, const char* name);

/*  — enregistre un identifiant TOP-LEVEL user declare ailleurs
 * (variable globale, constante globale, nom de procedure/fonction). Le
 * formatter reecrit les usages cross-fichier vers cette casse declarative
 * en suivant  (projet.owl = frontiere de scan recursif). */
void owl_format_context_add_global(OwlFormatContext* ctx, const char* name);

/*  — enregistre un nom de procedure/fonction TOP-LEVEL user declare
 * ailleurs (sous-ensemble callable des globals). Sert a restaurer la graphie
 * de declaration sur un appel NU cross-fichier (accent/casse insensible) sans
 * confondre avec une variable/constante globale homonyme. */
void owl_format_context_add_proc(OwlFormatContext* ctx, const char* name);

/* Variante context-aware de owl_format. `ctx` peut etre NULL (= owl_format
 * classique, canon intra-fichier seul). */
char* owl_format_ctx(const char* source,
                     const OwlFormatContext* ctx);

/* ================================================================
 * TRADUCTION DE SURFACE LANGAGE FR <-> EN
 *
 * owl_translate reutilise integralement le pipeline du formatter
 * (tokenize -> emit + canonisation registry/keyword) mais FORCE la
 * langue cible sur la surface langage : mots-cles, natives, types
 * primitifs, proprietes builtin, blocs/proprietes DSL. Identifiants
 * utilisateur, chaines, commentaires, et tout token de surface sans
 * canon bilingue (neutre) restent INCHANGES.
 *
 * Contrat round-trip : translate(translate(F, EN), FR) == owl_format(F)
 * (identite au canon). Les synonymes FR collapsent au canon (tout/toute/
 * tous/chaque -> canon de chaque forme), donc l'identite tient sur la
 * forme deja canonique, pas sur une forme non-canon.
 *
 * OWL_TRANSLATE_NONE = comportement formatter actuel (preservation de
 * langue) : owl fmt passe par owl_format_ctx avec target NONE -> zero
 * regression.
 * ================================================================ */

typedef enum {
    OWL_TRANSLATE_NONE = 0,  /* preservation (= owl fmt) */
    OWL_TRANSLATE_FR,        /* force le canon FR */
    OWL_TRANSLATE_EN         /* force le canon EN */
} OwlTranslateLang;

/* Reecrit `source` en forcant la langue cible sur la surface langage.
 * Retourne une chaine nouvellement allouee (caller free()). NULL si
 * `target` n'est ni FR ni EN. `ctx` (cross-fichier) peut etre NULL. */
char* owl_translate(const char* source, OwlTranslateLang target);
char* owl_translate_ctx(const char* source, OwlTranslateLang target,
                        const OwlFormatContext* ctx);

/* Format a file. Returns 0 on success, non-zero on error.
 * If dryRun is true, prints the diff but doesn't write. */
int owl_format_file(const char* path, bool dryRun);

/* Variante context-aware de owl_format_file. `ctx` peut etre NULL. */
int owl_format_file_ctx(const char* path, bool dryRun,
                        const OwlFormatContext* ctx);

/* ================================================================
 * Construction du contexte (formatter_context.c) — partage owl.exe / owl-lsp
 * ================================================================ */

/* Basename (sans dossier) d'un path, separateurs / ou \. */
const char* owl_format_path_basename(const char* path);

/* `test_*.owl` ? (cible test => fichiers de test admis dans le contexte). */
bool owl_format_basename_is_test(const char* basename);

/* Decouvre le projet raciné a `root` ( : recursif ssi projet.owl
 * ancetre), prescanne les fichiers eligibles (la cible `targetBasename` est
 * exclue) et construit un OwlFormatContext de casse cross-fichier. A liberer
 * via owl_format_context_free. NULL si la decouverte echoue. Source unique
 * partagee par owl fmt (owl.exe) et le handler de format du LSP. */
OwlFormatContext* owl_format_build_context(const char* root,
                                           const char* targetBasename,
                                           bool targetIsTest,
                                           bool recursive);

/* ================================================================
 * Introspection des tables internes (pour registry_self_test).
 * ================================================================ */

typedef struct {
    const char* normalized;   /* lowercase ascii — clé de matching */
    const char* canonical;    /* forme canon retournée (avec accent) */
} OwlFormatterMethodNorm;

/* Iterator sur formatter::method_norms[] (table d'accent-restoration pour
 * methodes a canonique accentue enregistrees ASCII dans le catalog).
 * Le self-test registry consomme ca pour verifier qu'aucune entree n'est
 * redondante avec une methode deja exposee canonique par le catalog. */
int owl_formatter_method_norms_count(void);
const OwlFormatterMethodNorm* owl_formatter_method_norms_at(int i);

/* Iterator sur formatter::fmt_keywords[] — la table qui decide de la forme
 * ecrite dans le fichier au save. Le self-test fmt-canon la confronte a la
 * forme canon que la fiche keyword affiche (owl explain / hover) : deux
 * sources de canon pour le meme mot, personne ne les confrontait.
 * `lang` : 0 = neutre, 1 = FR, 2 = EN. */
int owl_formatter_keyword_count(void);
bool owl_formatter_keyword_at(int i, const char** normalized,
                              const char** canonical, int* lang);

/* Canon de la LANGUE OPPOSEE pour un mot du langage (colonne `partner` de
 * fmt_keywords[]). `word` est accepte sous n'importe quelle forme : la
 * comparaison strippe les accents et ignore la casse (invariants 1 et 2).
 * NULL si le mot est absent de la table, ou s'il n'a pas de contrepartie
 * 1:1 dans l'autre langue (formes synonymes d'iteration, mots neutres).
 *
 * Existe parce que le catalogue de mots-cles ne porte QUE la forme canon FR
 * (owl_keyword_catalog.h interdit d'y dupliquer une valeur qu'une autre
 * source rend deja) : le rendu bilingue de `owl list keywords` n'a pas
 * d'autre chemin que de lire cette table-ci. Lecture seule. */
const char* owl_formatter_keyword_partner(const char* word);

/*  : second canon EN d'un mot dont le canon anglais depend du SENS.
 * `PAS` est le seul cas : `owl_formatter_keyword_partner("PAS")` rend le canon
 * context-free `NOT` (le sens negation, cf. ), et cet accesseur-ci rend
 * l'AUTRE canon `STEP` (le sens increment de boucle). NULL pour tout mot a canon
 * EN unique. Sert l'affichage standalone de `owl list keywords --lang en`, qui
 * doit nommer les DEUX canons sans jamais les composer (pas de « STEP IN »).
 * Lecture seule. */
const char* owl_formatter_keyword_partner_alt(const char* word);

/* Retourne la forme canonique d'une constante "magique" du langage si
 * `name` (case-insensible) en est une : TAB / CRLF / LF / EOT / ESC / ESP
 * / ValPI + les litteraux Vrai / Faux / Null. NULL sinon.
 *
 * Equivalent au set CAT_BOOL de la table de normalisation du formatter.
 * Utilise par le compilo pour le warning de shadow. Les formes heritees
 * CAT_EOL_LEGACY (rc / cr) ont leur propre accesseur ci-dessous.
 *
 * Le canonical est utile pour le message d'erreur ("'tab' shadow 'TAB'")
 * et pour distinguer la forme uppercase utilisable pour referencer la
 * constante apres le shadow.
 *
 * Pourquoi pas owl_registry_const_find tout court : le registry contient
 * aussi les constantes "metier" prefixees (httpGet, fLecture, psdMiseEnForme,
 * typeMimeXML…) qui collisionnent regulierement avec les noms Hungarian
 * (sTexte, fNum...). Trop bruyant — on warn seulement sur les noms
 * "universels" du langage qu'un dev doit reconnaitre. */
const char* owl_formatter_magic_const_canonical(const char* name);

/* Idem pour les formes heritees de fin de ligne (rc / cr) : retourne "CRLF"
 * si `name` en est une, NULL sinon. Elles ne sont pas des symboles du
 * registre, mais restent resolvables et sont donc masquables par une
 * variable homonyme — d'ou un warning de masquage a message distinct. */
const char* owl_formatter_eol_legacy_canonical(const char* name);

#endif
