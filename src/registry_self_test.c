// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* registry_self_test.c - Implementation des self-tests du registry.
 *
 * Voir registry_self_test.h + docs/internal_documentation/cahier_des_charges_langage.md.
 *
 * Les 11 regles verifiees sur le registry apres vm_init() :
 *
 *   1. canonical_ascii_missing
 *      Tout OwlNativeDef.canonical contenant un caractere non-ASCII
 *      doit avoir canonicalAscii non-NULL et different.
 *
 *   2. name_en_missing
 *      Tout OwlNativeDef public a nameEN non-NULL.
 *      Whitelist : flag OWL_NAT_INTERNAL = exempte (helpers internes).
 *
 *   3. desc_missing
 *      Tout OwlNativeDef / OwlConstDef / OwlTypeDef public a
 *      descFR ET descEN non-vides.
 *
 *   4. const_ascii_alias_missing
 *      Toute OwlConstDef.name avec accents doit avoir son alias ASCII
 *      enregistre via owl_registry_register_ascii_alias (alimente
 *      vm.globals seulement, jamais le registry consts -> pas de
 *      doublon LSP).
 *
 *   5. enum_value_orphan
 *      Chaque OwlEnum.values[i] doit correspondre a une OwlConstDef
 *      du registry. Sinon allowedBase rejette compile-time un usage
 *      legitime.
 *
 *   6. type_function_collision_unwhitelisted
 *      Si un nom (apres normalisation ASCII+lower) existe a la fois
 *      dans OwlNativeDef et OwlTypeDef, il doit etre dans
 *      KNOWN_TYPE_FUNCTION_DOUBLES (whitelist explicite).
 *
 *   7. ui_props_desc_missing (V2 — props DSL Libelle/Bouton/...)
 *      Toute OwlControlPropDef a descFR + descEN non-vides.
 *      (Active une fois le retard rattrape sur les props DSL.)
 *
 *   8-9. dsl_prop_enum_ref / dsl_prop_name_en
 *
 *   10. event_catalog : qualite minimale du catalogue d'events DSL.
 *
 *   11. field_name_en_missing
 *      Tout OwlField des types reifies (httpRequete, sqlConnexion, etc.)
 *      a un nameEN non-vide pour preserver l'invariant n°3 (bilingue
 *      FR<->EN). Strict absolu — aucune allow-list.
 *
 *   12. template_field_undeclared
 *      Toute key presente dans inst->fields d'un template runtime doit
 *      etre declaree dans FIELDS_X[] (sauf prefixes underscore comme
 *      _handle). Attrape les fields exposes en runtime mais oublies de
 *      la declaration (incoherence Definition.Parametres, Cadre.Coin*).
 *
 *   13. field_storage_not_ascii
 *      Le `name` d'un OwlField (1er arg = cle storage) doit etre ASCII
 *      pur. Si accentue, le scanner OWL strip au compile time mais
 *      table_get est strict byte-a-byte -> Null silencieux. Bugs
 *      Police.Souligné/Barré et pdf.Qualité corriges en B.12/B.15.
 *
 *   14. field_default_value_missing
 *      Tout OwlField publique (nom non prefixe par '_') a defaultValue
 *      non-NULL. La doc generee + vscode-data + hover LSP exposent
 *      cette valeur — un NULL est un oubli, "" est explicite (zero-typed).
 *
 *   16. method_binding_*
 *      Tout OwlMethodBinding well-formed : receiverType reconnu par
 *      receiver_type_from_name, methodName non vide, arity valide,
 *      pas de doublon (receiver, method) dans le meme array. Attrape
 *      les typos style "DateHeure" vs "dateheure" qui font register
 *      method bindings skip silencieusement -> methode prefix manquante
 *      au runtime sans warning.
 *
 *   17. dsl_prop_ascii_missing
 *      Pendant DSL de la regle 1 : toute OwlControlPropDef a nameFR
 *      accentue doit avoir nameAscii non-NULL. Sans ca, le lookup
 *      catalog (apres scanner accent-strip) rate silencieusement.
 *
 *   19. method_norm_redundant
 *      formatter::method_norms[] est une table d'accent-restoration. Toute
 *      entree dont le canonical est deja present tel quel dans le method
 *      catalog est inatteignable (le catalog hit en step 1) — code mort
 *      qui peut masquer un vrai canon si quelqu'un l'edite par la suite.
 *
 *   20. enum_value_not_in_sentinel_zone
 *      Tout OwlEnum combinable=false doit avoir ses valeurs en zone
 *      INT32_MIN+offset (sentinels). Empeche les typos silencieuses du
 *      genre 'Prop = 1' qui matchent par hasard une valeur d'enum.
 *      Opt-out via force_small_int=true uniquement si la valeur entiere
 *      porte une semantique runtime (ex Couleur RGB).
 *
 *   21. public_string_forbidden_mention
 *      Mentions interdites dans les chaines user-facing du registry
 *      (descFR/descEN des natives, consts, types, enums, fields, params,
 *      DSL props) : marques tierces, references au canon tiers,
 *      ID doc externes (doc + 7 chiffres), marqueurs chantier
 *      (V1/V2..., Lot X, Phase Y). Voir CLAUDE.md regle 7.
 *
 *   22. public_file_forbidden_mention
 *      Memes mentions, mais scan filesystem de examples/, tests/scenarios/
 *      et docs/public_documentation/ (.owl et .md). Les _brut_*.md sous
 *      candidats/ sont skip (research brouillon en attente de normalisation).
 *
 *   23. global_instance_missing_typedef
 *      Tout ObjInstance template enregistre dans vm.globals (typiquement
 *      via register_global_instance) doit avoir une OwlTypeDef dans
 *      TYPES[]. Empeche les "types fantomes" : exposes comme sous-objet
 *      d'un autre type via FIELDS_X, mais non declarables standalone.
 *      Cas historique : Coin/Trait/Option avant regularisation.
 *
 *   24. method_receiver_info_missing
 *      METHOD_RECEIVER_INFO[] (owl_methods.c) doit etre dense — chaque
 *      MethodReceiverType < METHOD_TYPE_COUNT a une entree avec canonical
 *      non-NULL. Empeche les trous quand un dev ajoute un enum sans la
 *      designated initializer correspondante.
 *
 *   (15 reserve : Surface Contract Native — voir TODO registry.)
 *
 *   39. test_scenario_filename_convention
 *      Tout fichier dans tests/scenarios/ (recursif) doit etre un .owl
 *      dont le basename commence par "test_". Garantit une discovery
 *      predictible par convention de nom et un repertoire sans bruit.
 *      Exception : les fixtures binaires (non-.owl, sans writer pour les
 *      generer a la volee — ex: .xls) vivent dans un sous-dossier
 *      'fixtures/', exclu du scan. Convention generale, pas un cas special.
 *
 *   41. public_file_utf8_bom / public_file_not_utf8
 *      Tout .owl et .md dans examples/, tests/, docs/public_documentation/
 *      doit etre encode en UTF-8 strict (RFC 3629) sans BOM (EF BB BF).
 *      Verrouille la convention encoding du projet.
 *
 *   43. widget_kind_headless_coverage
 *      Pour chaque OwlControlKind avec is_widget=true dans CONTROL_KINDS,
 *      verifie que (a) la prop universelle Visible est exposee dans le
 *      catalog via owl_control_catalog_find, et (b) obj_new_ui_control(
 *      kind, NULL, "TEST_<KIND>") retourne un ObjUiControl valide bien
 *      typee, sans crash. Garantit le mode headless  : tout
 *      widget materialise au compile-time est lisible/ecrivable via
 *      headless_store via OP_STRUCT_GET/SET sans surprise.
 *
 *   44. enum_lookup_consistant
 *      Pour chaque OwlEnum du registry, la resolution
 *      owl_registry_enum_find(name) (et nameAscii si different) retourne
 *      le bon pointeur — lookup consomme par la contrainte de param
 *      (allowedBase / prop DSL enum). Les enums ne sont PAS des types
 *      declarables (`x est un <enum>` echoue en Type inconnu) ; ce rejet
 *      est couvert par les fixtures compile-error, pas par cette regle.
 *
 *   49. scenario_pub_metadata_required
 *      Tout tests/scenarios/natives/test_*.owl doit avoir ligne 1
 *      `// @sujet : <NomCanon>` (resolvable via owl_registry_find) et
 *      ligne 2 `// @résumé : <texte ≥ 10 chars>`. Bloc obligatoire,
 *      mecanisme interne du sous-dossier natives/ (parsing
 *      embedded_docs alimentant `owl examples`). Pas une convention
 *      OWL user-facing — ne JAMAIS l'evoquer dans
 *      docs/public_documentation/, primer, fondations.
 *
 *   50. no_duplicate_native_canonical
 *      Aucun canonical native enregistre 2+ fois dans le registre plat
 *      (owl_registry_count/at). Un doublon = override implicite cross-module :
 *      runtime garde le dernier, mais doc-browser/LSP/explain renvoient le
 *      premier (souvent la mauvaise categorie). Ne concerne QUE le registre
 *      natives (les doubles type-vs-native legitimes vivent dans un registry
 *      separe, couverts par 6/45).
 *
 *   45. dispatcher_case_insensible_doubles
 *      Pour chaque entree de KNOWN_TYPE_FUNCTION_DOUBLES (vrais doubles
 *      type/native du registry, ex: httpRequête type + HTTPRequête native),
 *      verifie qu'owl_registry_find et owl_registry_type_find resolvent
 *      le bon symbole en case-insensible (UPPERCASE, lowercase). Sans ca,
 *      le dispatch contextuel call-site/annotation-type du compileur
 *      regresse silencieusement — bug masque par le formatter qui restitue
 *      la casse canon au save.
 *
 *   53. scenarios_subdir_whitelist
 *      Les sous-dossiers directs de tests/scenarios/ appartiennent a la
 *      liste blanche fermee {natives, types, language, ui, poo}. Tout autre
 *      sous-dossier = violation. Ferme le trou par lequel des categories
 *      hors-convention (anglais/, enums/) etaient creees et faisaient
 *      deriver l'organisation du corpus.
 *
 *   54. scenario_no_smoke_only + scenario_assertion_outside_scenario_proc (, )
 *      Chaque entree test_*.owl de tests/scenarios/ (recursif, compagnons
 *      FEN_/ETAT_/cXxx/iXxx exclus car non-test_) doit :
 *      a) porter au moins une assertion NON tautologique (anti smoke-only).
 *         Assertion = appel dbgVérifie* / dbgCheck*. Tautologique =
 *         dbgVérifieVrai(Vrai,...) / dbgCheckTrue(True,...) (casse + accent
 *         insensibles). Sans assertion reelle : sa place est runtime/.
 *      b) porter chaque assertion À L'INTÉRIEUR d'une PROCEDURE (jamais au
 *         top-level du fichier). Une assertion baladée au top-level est un
 *         floating : le runner ré-exécute le top-level ET les scénarios,
 *         donc elle compte deux fois (et double `Ouvre` pour l'UI). Le suivi
 *         lexical piste la profondeur PROCEDURE...FIN (openers FR/EN,
 *         exception elif SINON SI, membres `.Xxx()` ignorés) pour savoir si
 *         le curseur est dans une procédure au moment de l'assertion. La
 *         nomenclature fine (PROCEDURE scenario_ vs helper) est cadrée par la
 *         doctrine tests/scenarios/README.md, pas durcie par ce self-test.
 *
 *   58. polymorphic_form_count_desc
 *      Toute OwlNativeDef declarant formCount >= 2 (opt-in) doit mentionner ce
 *      nombre dans descFR ET descEN sous la forme "<mot> formes"/"<mot> cas"
 *      (FR) et "<mot> forms"/"<mot> cases" (EN), avec <mot> le cardinal du
 *      compteur (deux/trois/... , two/three/...). Garde-fou anti-derive : le
 *      dispatch runtime d'une native polymorphe gagne une forme d'operandes
 *      sans que la description user-facing suive -> `owl explain`/hover mentent
 *      silencieusement. Le compteur est le point de couplage tenu a la main
 *      (bumper formCount = bumper le code = check hurle si la desc reste en
 *      arriere). Opt-in strict : formCount == 0 (defaut) = non verifie, donc
 *      zero faux positif sur les natives mono-forme. Historique : classe de
 *      defaut ayant mordu 2x sur TenseurProduitMatriciel.
 */

#include "registry_self_test.h"
#include "owl_registry.h"
#include "stdlib_ui_defs.h"  /* rule 65 : membership natives UI (impl cable en GUI) */
#include "doc_phantom_lint.h"  /* rule 63 : lint anti-natives-fantomes (detecteur partage) */
#include "owl_name_normalize.h" /* rule 63 : match FR/EN accent-insensible (methodes/props DSL) */
#include "aws_service_models.h" /* rule 61 : mapping service->protocole (AWS_SERVICES) */
#include "owl_categories.h" /* rule 56 : paire FR/EN de chaque categorie */
#include "owl_limits.h"
#include "owl_event_catalog.h"
#include "owl_keyword_catalog.h"
#include "owl_operator_catalog.h"
#include "owl_globalvar_catalog.h"
#include "owl_new.h"
#include "compiler.h"
#include "embedded_docs.h"
#include "cli_doc.h"        /* owl_template_emitted_body (rule 38 canonicité) */
#include "owl_methods.h"
#include "scanner.h"
#include "prescan.h"   /* prescan_depth_delta_text — porte texte de la source unique */
#include "ui_catalog.h"
#include "ui_decl.h"
#include "object.h"
#include "icons_catalog.h"   /* rule 48 icones_catalog_coverage */
#include "owl_stdlib.h"      /* owl_icones_namespace_lookup pour rule 48 */
#include "value.h"
#include "vm.h"
#include "table.h"
#include "formatter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================
 * Helpers
 * ============================================================ */

static bool has_non_ascii(const char* s) {
    if (!s) return false;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p >= 0x80) return true;
    }
    return false;
}

static bool str_empty(const char* s) {
    if (!s) return true;
    while (*s) { if (!isspace((unsigned char)*s)) return false; s++; }
    return true;
}

/* ASCII-fold + lower : retourne longueur ecrite. dst doit avoir
 * dstSize >= 1. Suffisant pour comparer deux noms case+accent insensible. */
static int normalize_ascii_lower(const char* src, char* dst, int dstSize) {
    int o = 0;
    if (!src || dstSize <= 0) { if (dstSize > 0) dst[0] = 0; return 0; }
    for (const unsigned char* p = (const unsigned char*)src; *p && o < dstSize - 1; p++) {
        unsigned char c = *p;
        if (c < 0x80) {
            dst[o++] = (char)tolower(c);
        } else if ((c & 0xE0) == 0xC0 && p[1]) {
            /* UTF-8 2-byte : decode le codepoint pour les diacritiques courants. */
            unsigned cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
            char fold = 0;
            switch (cp) {
                case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
                case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
                    fold = 'a'; break;
                case 0xE7: case 0xC7:
                    fold = 'c'; break;
                case 0xE8: case 0xE9: case 0xEA: case 0xEB:
                case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                    fold = 'e'; break;
                case 0xEC: case 0xED: case 0xEE: case 0xEF:
                case 0xCC: case 0xCD: case 0xCE: case 0xCF:
                    fold = 'i'; break;
                case 0xF1: case 0xD1:
                    fold = 'n'; break;
                case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
                case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
                    fold = 'o'; break;
                case 0xF9: case 0xFA: case 0xFB: case 0xFC:
                case 0xD9: case 0xDA: case 0xDB: case 0xDC:
                    fold = 'u'; break;
                case 0xFD: case 0xFF: case 0xDD:
                    fold = 'y'; break;
                default:
                    fold = '?'; break;
            }
            dst[o++] = fold;
            p++;
        } else {
            /* 3+ bytes UTF-8 (rare en identifiers) — saute proprement. */
            if ((c & 0xF0) == 0xE0 && p[1] && p[2]) p += 2;
            else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) p += 3;
            dst[o++] = '?';
        }
    }
    dst[o] = '\0';
    return o;
}

/* ============================================================
 * Whitelist : doubles type/fonction legitimes (cf §5 cahier des charges)
 * ============================================================ */

/*  : whitelist nettoye. Ne liste que les doubles ou TYPE ET NATIVE
 * coexistent reellement dans le registry (verifie par rule 45). Sources de
 * verite pour le dispatch case-insensible contextuel cote compileur.
 *
 * Historique : `httpreponse` et `chrono` etaient listes mais n'ont qu'un
 * type, pas de native homonyme (HTTPRéponse n'existe pas en native — c'est
 * un type retourne par HTTPRequête ; Chrono est un type, pas de Chrono()
 * createur — on utilise ChronoDébut/ChronoFin). Les retirer evite que
 * rule 45 hurle a vide sur des "doubles" inexistants. */
static const char* const KNOWN_TYPE_FUNCTION_DOUBLES[] = {
    "httprequete",      /* HTTPRequête (fn) + httpRequête (type) */
    "xmldocument",      /* XMLDocument (fn créatrice) + xmlDocument (type DOM) */
    "langue",           /* Langue (fn getter/setter) + langue (type BCP 47) */
    "tenseur",          /* Tenseur (fn constructrice) + Tenseur (type autograd) */
    NULL
};

static bool is_whitelisted_double(const char* asciiLower) {
    for (int i = 0; KNOWN_TYPE_FUNCTION_DOUBLES[i]; i++) {
        if (strcmp(asciiLower, KNOWN_TYPE_FUNCTION_DOUBLES[i]) == 0) return true;
    }
    return false;
}

/* ============================================================
 * Reporter de violations
 * ============================================================ */

typedef struct {
    FILE*  out;
    int    count;
    int    capacity;
    bool   first;
} Reporter;

static void rep_init(Reporter* r, FILE* out) {
    r->out = out;
    r->count = 0;
    r->capacity = INT32_MAX;
    r->first = true;
}

static void json_escape_to(FILE* f, const char* s) {
    if (!s) { fputs("null", f); return; }
    fputc('"', f);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '"')       fputs("\\\"", f);
        else if (*p == '\\') fputs("\\\\", f);
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\r') fputs("\\r", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p < 0x20)  fprintf(f, "\\u%04x", *p);
        else                 fputc(*p, f);
    }
    fputc('"', f);
}

static void rep_violation(Reporter* r, const char* rule, const char* symbol,
                            const char* detail) {
    if (!r->first) fputs(",\n", r->out);
    fputs("    {", r->out);
    fputs("\"rule\":",   r->out); json_escape_to(r->out, rule);
    fputs(",\"symbol\":", r->out); json_escape_to(r->out, symbol);
    fputs(",\"detail\":", r->out); json_escape_to(r->out, detail);
    fputc('}', r->out);
    r->first = false;
    r->count++;
}

/* ============================================================
 * Regle 1 : canonicalAscii present si canonical contient des accents
 * ============================================================ */

static void check_rule1_canonical_ascii(Reporter* r) {
    int n = owl_registry_count();
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical) continue;
        if (!has_non_ascii(d->canonical)) continue;  /* ASCII pur : OK sans alias */
        if (!d->canonicalAscii || !*d->canonicalAscii) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                "canonical='%s' contient des accents mais canonicalAscii est NULL/vide. "
                "Sans cela le runtime ne trouve pas la fonction quand l'utilisateur "
                "tape la forme sans accent.", d->canonical);
            rep_violation(r, "canonical_ascii_missing", d->canonical, detail);
        }
    }
}

/* ============================================================
 * Regle 2 : nameEN present pour toute native publique
 * ============================================================ */

static void check_rule2_name_en(Reporter* r) {
    int n = owl_registry_count();
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical) continue;
        if (d->flags & OWL_NAT_INTERNAL) continue;  /* helper interne, exempt */
        if (str_empty(d->nameEN)) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                "Native publique '%s' sans nameEN. Le mode EN du LSP/grammar ne "
                "proposera rien pour cette fonction. Trouver la traduction canon "
                "dans le fichier de reference interne.", d->canonical);
            rep_violation(r, "name_en_missing", d->canonical, detail);
        }
    }
}

/* ============================================================
 * Regle 3 : descFR + descEN non vides (natives + consts + types)
 * ============================================================ */

static void check_rule3_desc(Reporter* r) {
    int n = owl_registry_count();
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical) continue;
        if (d->flags & OWL_NAT_INTERNAL) continue;
        if (str_empty(d->descFR)) {
            rep_violation(r, "desc_missing", d->canonical,
                "descFR vide pour une native publique. Reprendre la phrase canon "
                "du fichier de reference interne.");
        }
        if (str_empty(d->descEN)) {
            rep_violation(r, "desc_missing", d->canonical, "descEN vide.");
        }
    }
    int nc = owl_registry_consts_count();
    for (int i = 0; i < nc; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (!c || !c->name) continue;
        if (str_empty(c->descFR)) {
            rep_violation(r, "desc_missing", c->name, "Constante sans descFR.");
        }
        if (str_empty(c->descEN)) {
            rep_violation(r, "desc_missing", c->name, "Constante sans descEN.");
        }
    }
    int nt = owl_registry_types_count();
    for (int i = 0; i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->canonical) continue;
        if (str_empty(t->descFR)) {
            rep_violation(r, "desc_missing", t->canonical, "Type sans descFR.");
        }
        if (str_empty(t->descEN)) {
            rep_violation(r, "desc_missing", t->canonical, "Type sans descEN.");
        }
    }
}

/* ============================================================
 * Regle 4 : alias ASCII des constantes accentuees presents dans vm.globals
 * ============================================================ */

/* Cherche dans vm.globals si une key existe dont la forme asciifiee+lower
 * est egale a `target`. table_get est case-sensible alors que les alias
 * sont enregistres avec leur casse d'origine (ex: 'frRecursif' pas
 * 'frrecursif') — il faut iterer manuellement. */
static bool global_exists_ascii(const char* asciiName) {
    char target[128];
    normalize_ascii_lower(asciiName, target, sizeof(target));
    if (!*target) return false;
    for (int i = 0; i < vm.globals.capacity; i++) {
        ObjString* k = vm.globals.entries[i].key;
        if (!k || !k->chars) continue;
        char kAscii[128];
        normalize_ascii_lower(k->chars, kAscii, sizeof(kAscii));
        if (strcmp(kAscii, target) == 0) return true;
    }
    return false;
}

static void check_rule4_const_ascii_alias(Reporter* r) {
    int nc = owl_registry_consts_count();
    char ascii[128];
    for (int i = 0; i < nc; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (!c || !c->name) continue;
        if (!has_non_ascii(c->name)) continue;
        normalize_ascii_lower(c->name, ascii, sizeof(ascii));
        if (!*ascii) continue;
        if (!global_exists_ascii(ascii)) {
            char detail[300];
            snprintf(detail, sizeof(detail),
                "Constante '%s' a un nom accentue mais aucun alias ASCII '%s' "
                "n'est enregistre dans vm.globals. Utiliser "
                "owl_registry_register_ascii_alias() pour garantir l'invariant.",
                c->name, ascii);
            rep_violation(r, "const_ascii_alias_missing", c->name, detail);
        }
    }
}

/* ============================================================
 * Regle 5 : valeurs d'enum referencent des constantes existantes
 * ============================================================ */

static void check_rule5_enum_values(Reporter* r) {
    int ne = owl_registry_enums_count();
    for (int i = 0; i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (!e || !e->values) continue;
        for (const char* const* v = e->values; *v; v++) {
            /* Acceptable si presente dans le registry consts OU dans
             * vm.globals (cf CLAUDE.md : 'wlXxx' / 'errXxx' sont
             * enregistrees directement dans vm.globals via
             * define_global_int et listees comme values d'enum). */
            const OwlConstDef* c = owl_registry_const_find(*v);
            if (c) continue;
            if (global_exists_ascii(*v)) continue;
            char detail[300];
            snprintf(detail, sizeof(detail),
                "Enum '%s' liste la valeur '%s' qui n'existe ni dans le "
                "registre des constantes, ni dans vm.globals. allowedBase "
                "rejettera tout usage. Soit declarer la constante, soit "
                "retirer la valeur de l'enum.",
                e->name ? e->name : "?", *v);
            rep_violation(r, "enum_value_orphan", e->name ? e->name : *v, detail);
        }
    }
}

/* ============================================================
 * Regle 20 : enums combinable=false doivent etre en zone sentinel
 *
 * Tout OwlEnum combinable=false dont au moins une valeur est >= 0 est
 * une violation, sauf si force_small_int=true (opt-out explicite quand
 * la valeur entiere porte une semantique runtime — ex Couleur RGB).
 *
 * Empeche les typos silencieuses du genre 'cnx..ModeTLS = 1' qui tombe
 * sur tlsAccepteTout par hasard. Force a passer par les constantes
 * nommees pour les enums non-bitmask.
 * ============================================================ */
static void check_rule20_enum_must_be_sentinel(Reporter* r) {
    int ne = owl_registry_enums_count();
    for (int i = 0; i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (!e || !e->values) continue;
        if (e->combinable) continue;        /* bitmasks: 1/2/4/8 legitime */
        if (e->force_small_int) continue;   /* opt-out explicite */
        for (const char* const* v = e->values; *v; v++) {
            const OwlConstDef* c = owl_registry_const_find(*v);
            if (!c || !IS_INTEGER(c->value)) continue;
            int64_t val = AS_INTEGER(c->value);
            if (val >= 0) {
                char detail[400];
                snprintf(detail, sizeof(detail),
                    "Enum '%s' (combinable=false) a sa valeur '%s' = %lld (>= 0). "
                    "Risque de typo silencieuse : user qui tape '= %lld' au pif "
                    "matche cette constante. Migrer vers une base sentinel "
                    "OWL_<X>_BASE dans sentinels.h. Si la valeur entiere doit "
                    "rester telle quelle (semantique runtime : RGB, etc.), "
                    "marquer l'enum avec force_small_int=true et commenter.",
                    e->name ? e->name : "?", *v, (long long)val, (long long)val);
                rep_violation(r, "enum_value_not_in_sentinel_zone",
                              e->name ? e->name : *v, detail);
            }
        }
    }
}

/* ============================================================
 * Regle 5c : widget_kinds table couvre l'enum OwlControlKind (Lot F)
 *
 * Garantit qu'a chaque OwlControlKind reel (LABEL/BUTTON/INPUT/TABLE/CONTAINER)
 * correspond une entree dans CONTROL_KINDS (ui_decl.c) avec is_widget=true.
 * Si un futur dev ajoute un kind dans l'enum mais oublie la table -> le
 * dump --vscode-data ne le mentionnerait pas, la regex TextMate serait
 * incomplete, et la coloration manquerait. Self-test bloque le build.
 *
 * On verifie aussi via owl_control_kind_name() (ASCII) qui matche
 * nameAscii (ou nameFR) d'une entree.
 * ============================================================ */
static void check_rule5c_widget_kinds_table(Reporter* r) {
    const OwlControlKindInfo* tbl = owl_control_kinds_table();
    const OwlControlKind to_check[] = {
        OWL_CONTROL_LABEL, OWL_CONTROL_BUTTON, OWL_CONTROL_INPUT,
        OWL_CONTROL_TABLE, OWL_CONTROL_CONTAINER, OWL_CONTROL_WINDOW
    };
    for (size_t i = 0; i < sizeof(to_check)/sizeof(to_check[0]); i++) {
        const char* enumName = owl_control_kind_name(to_check[i]);
        if (!enumName || strcmp(enumName, "?") == 0) continue;
        bool found = false;
        for (const OwlControlKindInfo* k = tbl; k->nameFR; k++) {
            const char* asciiOrFR = k->nameAscii ? k->nameAscii : k->nameFR;
            if (strcasecmp(asciiOrFR, enumName) == 0) {
                if (!k->is_widget) {
                    /* Le kind enum est un widget mais la table le marque
                     * sous-bloc. Incoherence, mais pas bloquante : warning
                     * via violation explicite. */
                    char detail[200];
                    snprintf(detail, sizeof(detail),
                        "OwlControlKind '%s' present dans CONTROL_KINDS mais "
                        "marque is_widget=false. Mettre is_widget=true.", enumName);
                    rep_violation(r, "widget_kind_table_inconsistent", enumName, detail);
                }
                found = true; break;
            }
        }
        if (!found) {
            char detail[300];
            snprintf(detail, sizeof(detail),
                "OwlControlKind '%s' n'a PAS d'entree dans la table "
                "CONTROL_KINDS (src/ui_decl.c). Sans ca le dump --vscode-data "
                "n'expose pas ce kind, la regex TextMate du plugin VSCode est "
                "incomplete et la coloration manque. Ajouter une ligne dans "
                "CONTROL_KINDS avec nameFR/nameAscii/nameEN/is_widget=true.",
                enumName);
            rep_violation(r, "widget_kind_missing_in_table", enumName, detail);
        }
    }

    /* Sub-rule : pour chaque widget de CONTROL_KINDS, owl_control_kind_from_token
     * doit reconnaitre nameFR / nameAscii / nameEN et retourner le bon kind.
     * Garantit que tout site consommateur (formatter, compiler, LSP) qui
     * delegue a cette API reste en sync avec la table. */
    for (const OwlControlKindInfo* k = tbl; k->nameFR; k++) {
        if (!k->is_widget) continue;
        OwlControlKind expected;
        if (!owl_control_kind_of_info(k, &expected)) {
            char detail[300];
            snprintf(detail, sizeof(detail),
                "Widget '%s' n'est pas resolu par owl_control_kind_of_info. "
                "Le mapping kind_from_canon (src/ui_decl.c) ne couvre pas "
                "ce nom canon. Ajouter le case dans le switch.", k->nameFR);
            rep_violation(r, "control_kind_unresolved", k->nameFR, detail);
            continue;
        }
        const char* names[3] = { k->nameFR, k->nameAscii, k->nameEN };
        for (int n = 0; n < 3; n++) {
            if (!names[n]) continue;
            OwlControlKind got;
            if (!owl_control_kind_from_token(names[n], (int)strlen(names[n]),
                                              &got, NULL) || got != expected) {
                char detail[300];
                snprintf(detail, sizeof(detail),
                    "Widget '%s' (form '%s') n'est pas reconnu par "
                    "owl_control_kind_from_token. Symptome : formatter "
                    "n'incrementera pas l'indent et le LSP ne posera pas le "
                    "semantic token. Verifier ci_ai_eq + boucle CONTROL_KINDS.",
                    k->nameFR, names[n]);
                rep_violation(r, "control_kind_token_mismatch", names[n], detail);
            }
        }
    }
}

/* ============================================================
 * Regle 6 : doubles type/fonction whitelistes
 * ============================================================ */

static void check_rule6_type_fn_collision(Reporter* r) {
    int nfn = owl_registry_count();
    int nty = owl_registry_types_count();
    char fnLower[128], tyLower[128];
    for (int i = 0; i < nfn; i++) {
        const OwlNativeDef* fn = owl_registry_at(i);
        if (!fn || !fn->canonical) continue;
        if (fn->flags & OWL_NAT_INTERNAL) continue;
        normalize_ascii_lower(fn->canonical, fnLower, sizeof(fnLower));
        for (int j = 0; j < nty; j++) {
            const OwlTypeDef* ty = owl_registry_type_at(j);
            if (!ty || !ty->canonical) continue;
            normalize_ascii_lower(ty->canonical, tyLower, sizeof(tyLower));
            if (strcmp(fnLower, tyLower) != 0) continue;
            /* Collision detectee. */
            if (!is_whitelisted_double(fnLower)) {
                char detail[300];
                snprintf(detail, sizeof(detail),
                    "Une native '%s' et un type '%s' partagent le meme nom "
                    "normalise '%s' mais cette collision n'est pas whitelistee. "
                    "Si c'est intentionnel (cf cahier des charges §5), ajouter "
                    "'%s' a KNOWN_TYPE_FUNCTION_DOUBLES dans registry_self_test.c.",
                    fn->canonical, ty->canonical, fnLower, fnLower);
                rep_violation(r, "type_function_collision_unwhitelisted", fnLower, detail);
            }
        }
    }
}

/* ============================================================
 * Regle 7 : DSL props ont descFR + descEN non vides
 *
 * Toute prop declaree dans PROPS_WINDOW/LABEL/BUTTON/SAISIE/TABLE/ETAT/
 * ETAT_BLOCK doit fournir une description bilingue. C'est ce que la doc
 * --doc et le LSP hover affichent. Les designers VSCode aussi.
 * ============================================================ */
static void check_rule7_dsl_prop_desc(Reporter* r) {
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->def->nameFR) continue;
        char sym[160];
        snprintf(sym, sizeof(sym), "%s.%s", e->kind, e->def->nameFR);
        if (str_empty(e->def->descFR)) {
            rep_violation(r, "dsl_prop_desc_missing", sym,
                "Propriete DSL sans descFR. Recopier la phrase canon de la doc UI/Etat.");
        }
        if (str_empty(e->def->descEN)) {
            rep_violation(r, "dsl_prop_desc_missing", sym,
                "Propriete DSL sans descEN. Traduire fidelement le descFR.");
        }
    }
}

/* ============================================================
 * Regle 8 : DSL props type=ENUM ont enum_name pointant un OwlEnum existant
 *
 * Detecte les enum_name orphelins (typo, enum supprimee, jamais enregistree).
 * Si la prop est ENUM mais enum_name est NULL, la valeur n'est pas validee.
 * Si enum_name pointe une enum inconnue, le LSP autocomplete proposera
 * vide → bug silencieux.
 * ============================================================ */
static void check_rule8_dsl_prop_enum_ref(Reporter* r) {
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->def->nameFR) continue;
        /* On ne controle que les props effectivement marquees ENUM. */
        if (e->def->type != OWL_PROP_TYPE_ENUM) continue;
        char sym[160];
        snprintf(sym, sizeof(sym), "%s.%s", e->kind, e->def->nameFR);
        if (!e->def->enum_name || !*e->def->enum_name) {
            rep_violation(r, "dsl_prop_enum_missing_name", sym,
                "Propriete type=ENUM sans enum_name renseigne. Mettre le nom canon de l'OwlEnum (ex: \"TypeBouton\", \"etatFormat\").");
            continue;
        }
        const OwlEnum* en = owl_registry_enum_find(e->def->enum_name);
        if (!en) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Propriete '%s' type=ENUM pointe enum '%s' qui n'est pas enregistre dans owl_registry. Verifier le nom (case+accent insensible) ou enregistrer l'enum via owl_registry_register_enums().",
                e->def->nameFR, e->def->enum_name);
            rep_violation(r, "dsl_prop_enum_orphan", sym, detail);
        }
    }
}

/* ============================================================
 * Regle 46 : DSL props type=OBJECT ont object_type non-NULL pointant un
 *            type compose existant dans le registry.
 *
 * Symetrique de la regle 8 (ENUM -> enum_name). Une prop composee
 * (OWL_PROP_TYPE_OBJECT, ex Cadre) doit nommer son type-cible via
 * object_type, et ce type doit etre enregistre comme OwlTypeDef. Sinon :
 *   - object_type NULL  -> la cascade compile-check ne sait pas quoi valider
 *     ni quel template materialiser au runtime (crash headless).
 *   - object_type orphelin -> aucun template, aucune validation de champ.
 * enum_name doit rester NULL sur une prop OBJECT (separation stricte
 * enum/type : un type compose ne passe jamais par enum_name).
 * ============================================================ */
static void check_rule46_object_prop_target_type_valid(Reporter* r) {
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->def->nameFR) continue;
        if (e->def->type != OWL_PROP_TYPE_OBJECT) continue;
        char sym[160];
        snprintf(sym, sizeof(sym), "%s.%s", e->kind, e->def->nameFR);
        if (!e->def->object_type || !*e->def->object_type) {
            rep_violation(r, "object_prop_target_type_valid", sym,
                "Propriete type=OBJECT sans object_type renseigne. Nommer le "
                "type compose cible (ex: \"Cadre\").");
            continue;
        }
        const OwlTypeDef* td = owl_registry_type_find(e->def->object_type);
        if (!td) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Propriete '%s' type=OBJECT pointe object_type '%s' qui n'est "
                "pas un type compose enregistre dans owl_registry. Verifier le "
                "nom (case+accent insensible) ou enregistrer le type.",
                e->def->nameFR, e->def->object_type);
            rep_violation(r, "object_prop_target_type_valid", sym, detail);
        }
        if (e->def->enum_name && *e->def->enum_name) {
            rep_violation(r, "object_prop_target_type_valid", sym,
                "Propriete type=OBJECT avec enum_name non-NULL : separation "
                "stricte violee. Le type compose passe par object_type, "
                "enum_name est reserve aux enumerations.");
        }
    }
}

/* ============================================================
 * Regle 9 : DSL props ont nameEN non vide (bilingue obligatoire)
 *
 * OWL est bilingue FR/EN par contrat (cf invariant 3 du cahier des
 * charges langage). Une prop sans nameEN casse l'autocomplete LSP en
 * mode EN et rend la grammar TextMate incoherente.
 * ============================================================ */
static void check_rule9_dsl_prop_name_en(Reporter* r) {
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->def->nameFR) continue;
        if (str_empty(e->def->nameEN)) {
            char sym[160];
            snprintf(sym, sizeof(sym), "%s.%s", e->kind, e->def->nameFR);
            rep_violation(r, "dsl_prop_name_en_missing", sym,
                "Propriete DSL sans nameEN. OWL etant bilingue par contrat, fournir l'alias EN canonique.");
        }
    }
}

/* ============================================================
 * Regle 52 : DSL props ont un defaut declare + coherent ( Veine Y)
 *
 * Invariant : chaque propriete UI declare son defaut canon dans
 * OwlControlPropDef.dft. Le champ etant zero-initialise a
 * OWL_PROP_DEFAULT_UNSET (sentinelle generique), "declare" est toujours
 * vrai des l'ajout du champ -> cette partie ne peut pas echouer (c'est
 * voulu : resout la tension du verrou, le self-test reste vert pendant la
 * population incrementale, sentinelle OU valeur comptant comme declare).
 *
 * Les DENTS de la regle sont les gardes de COHERENCE, non-circulaires
 * (ils croisent le type de la prop + le registry d'enums, que l'auteur du
 * defaut ne controle pas via le champ dft) :
 *
 *   (a) un defaut ENUM doit nommer une valeur reellement enregistree dans
 *       l'OwlEnum cible (enum_name). Attrape les valeurs inventees / typos.
 *   (b) le kind du defaut doit etre coherent avec le type declare de la
 *       prop (pas de defaut BOOL sur une prop INT, etc.). Attrape les
 *       erreurs de population (mauvais DFT_* colle a la mauvaise prop).
 *
 * Note : un garde "runtime" (instancier un widget sans la prop, lire la
 * valeur effective appliquee, comparer au defaut declare) serait l'ideal
 * anti-divergence, mais les defauts runtime vivent aujourd'hui comme des
 * litteraux disperses aux sites de lecture (prop_bool(...,DEFAUT) dans
 * stdlib_ui.c), sans API centrale "defaut effectif" lisible pour un
 * controle frais. La consolidation runtime-lit-le-registry est notee comme
 * 2e temps dans le ticket. Les gardes (a)/(b) restent non-circulaires.
 * ============================================================ */
static bool enum_has_value_ci(const OwlEnum* en, const char* val) {
    if (!en || !en->values || !val) return false;
    for (const char* const* v = en->values; *v; v++) {
        if (strcasecmp(*v, val) == 0) return true;
    }
    return false;
}

static void check_rule52_dsl_prop_default(Reporter* r) {
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->def->nameFR) continue;
        const OwlControlPropDef* d = e->def;
        OwlPropDefaultKind k = d->dft.kind;
        char sym[160];
        snprintf(sym, sizeof(sym), "%s.%s", e->kind, d->nameFR);

        /* (a) defaut ENUM -> valeur doit exister dans l'OwlEnum cible. */
        if (k == OWL_PROP_DEFAULT_ENUM) {
            if (!d->dft.s || !*d->dft.s) {
                rep_violation(r, "dsl_prop_default", sym,
                    "Defaut kind=ENUM sans nom de valeur (dft.s NULL/vide).");
            } else {
                const OwlEnum* en = d->enum_name
                    ? owl_registry_enum_find(d->enum_name) : NULL;
                if (!en) {
                    char detail[400];
                    snprintf(detail, sizeof(detail),
                        "Defaut ENUM '%s' sur prop dont enum_name '%s' est "
                        "introuvable dans le registry. Un defaut enum exige "
                        "un enum_name valide.",
                        d->dft.s, d->enum_name ? d->enum_name : "(NULL)");
                    rep_violation(r, "dsl_prop_default", sym, detail);
                } else if (!enum_has_value_ci(en, d->dft.s)) {
                    char detail[400];
                    snprintf(detail, sizeof(detail),
                        "Defaut ENUM '%s' n'est PAS une valeur de l'enum '%s'. "
                        "Verifier l'orthographe canon de la valeur (ex: une "
                        "valeur du 1er rang de l'enumeration).",
                        d->dft.s, d->enum_name);
                    rep_violation(r, "dsl_prop_default", sym, detail);
                }
            }
        }

        /* (b) coherence kind <-> type declare de la prop. UNSET passe
         * toujours (sentinelle generique). VARIANT/POSITION lenients. */
        bool ok = true;
        switch (d->type) {
            case OWL_PROP_TYPE_BOOL:
                ok = (k == OWL_PROP_DEFAULT_UNSET || k == OWL_PROP_DEFAULT_BOOL);
                break;
            case OWL_PROP_TYPE_INT:
            case OWL_PROP_TYPE_COLOR:
                ok = (k == OWL_PROP_DEFAULT_UNSET || k == OWL_PROP_DEFAULT_AUTO ||
                      k == OWL_PROP_DEFAULT_NULL  || k == OWL_PROP_DEFAULT_INT);
                break;
            case OWL_PROP_TYPE_STRING:
            case OWL_PROP_TYPE_MULTILINGUE:
                ok = (k == OWL_PROP_DEFAULT_UNSET || k == OWL_PROP_DEFAULT_NULL ||
                      k == OWL_PROP_DEFAULT_STRING);
                break;
            case OWL_PROP_TYPE_ENUM:
                ok = (k == OWL_PROP_DEFAULT_UNSET || k == OWL_PROP_DEFAULT_NULL ||
                      k == OWL_PROP_DEFAULT_ENUM);
                break;
            case OWL_PROP_TYPE_OBJECT:
                ok = (k == OWL_PROP_DEFAULT_UNSET || k == OWL_PROP_DEFAULT_NULL);
                break;
            case OWL_PROP_TYPE_VARIANT:
            case OWL_PROP_TYPE_POSITION:
            default:
                ok = true;  /* polymorphe : tout defaut accepte */
                break;
        }
        if (!ok) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Defaut kind=%s incoherent avec le type de prop '%s'. "
                "Verifier le DFT_* applique (mauvais raccourci colle a la "
                "mauvaise propriete ?).",
                owl_prop_default_kind_name(k), owl_prop_type_name(d->type));
            rep_violation(r, "dsl_prop_default", sym, detail);
        }
    }
}

/* ============================================================
 * Regle 10 : catalogue d'events DSL widget — qualité minimum
 *   - canonical non vide
 *   - descFR + descEN non vides (visible LSP hover, doc auto)
 *   - chaque param a name + typeName + descFR + descEN non vides
 *   - typeName référence un type connu (entier/chaine/...)
 *   - pas de doublon canonical par kind
 * Source : src/event_catalog_widgets.c
 * Filet anti-oubli : tout nouvel event ajouté au catalogue est validé
 * automatiquement par make (cf Makefile cible 'all' qui exit 1 si
 * registry_self_test trouve une violation).
 * ============================================================ */
static bool event_typename_known(const char* t) {
    /* Liste minimale des types acceptés en signature d'event V1.
     * Étendable plus tard si on introduit des params chaîne/booleen/etc. */
    if (str_empty(t)) return false;
    return strcmp(t, "entier") == 0
        || strcmp(t, "Entier") == 0
        || strcmp(t, "chaine") == 0
        || strcmp(t, "cha\xc3\xae" "ne") == 0   /* "chaîne" */
        || strcmp(t, "Chaine") == 0
        || strcmp(t, "Cha\xc3\xae" "ne") == 0   /* "Chaîne" */
        || strcmp(t, "booleen") == 0
        || strcmp(t, "bool\xc3\xa9" "en") == 0  /* "booléen" */
        || strcmp(t, "reel") == 0
        || strcmp(t, "r\xc3\xa9" "el") == 0;    /* "réel" */
}

static void check_rule10_event_catalog(Reporter* r) {
    /* OwlControlKind valeurs <10 ; on couvre l'enum entier. */
    for (int kind = 0; kind < 16; kind++) {
        int count = owl_event_catalog_count((OwlControlKind)kind);
        for (int i = 0; i < count; i++) {
            const OwlEventDef* d = owl_event_catalog_at((OwlControlKind)kind, i);
            if (!d) continue;
            char sym[160];
            const char* canon = d->canonical ? d->canonical : "<NULL>";

            /* Champ canonical */
            if (str_empty(d->canonical)) {
                rep_violation(r, "event_canonical_missing", "<unnamed>",
                    "Event au catalogue sans nom canonical.");
                continue;
            }
            /* descFR/descEN obligatoires (apparaissent en LSP hover). */
            if (str_empty(d->descFR)) {
                snprintf(sym, sizeof(sym), "%s", canon);
                rep_violation(r, "event_descfr_missing", sym,
                    "Event sans descFR. Apparait en hover LSP — obligatoire.");
            }
            if (str_empty(d->descEN)) {
                snprintf(sym, sizeof(sym), "%s", canon);
                rep_violation(r, "event_descen_missing", sym,
                    "Event sans descEN. OWL bilingue par contrat — obligatoire.");
            }
            /* Doublon canonical au sein du meme kind. */
            for (int j = 0; j < i; j++) {
                const OwlEventDef* prev = owl_event_catalog_at((OwlControlKind)kind, j);
                if (prev && prev->canonical &&
                    strcmp(prev->canonical, d->canonical) == 0) {
                    snprintf(sym, sizeof(sym), "%s", canon);
                    rep_violation(r, "event_duplicate", sym,
                        "Event canonical dupplique dans le meme widget kind.");
                }
            }
            /* Validation des params. */
            for (int p = 0; p < d->paramCount; p++) {
                const OwlEventParam* pa = &d->params[p];
                if (str_empty(pa->name)) {
                    snprintf(sym, sizeof(sym), "%s param[%d]", canon, p);
                    rep_violation(r, "event_param_name_missing", sym,
                        "Parametre d'event sans nom.");
                    continue;
                }
                if (str_empty(pa->typeName)) {
                    snprintf(sym, sizeof(sym), "%s.%s", canon, pa->name);
                    rep_violation(r, "event_param_type_missing", sym,
                        "Parametre d'event sans typeName.");
                } else if (!event_typename_known(pa->typeName)) {
                    snprintf(sym, sizeof(sym), "%s.%s", canon, pa->name);
                    char detail[256];
                    snprintf(detail, sizeof(detail),
                        "Type '%s' non reconnu. Types valides : entier, "
                        "chaine, booleen, reel.", pa->typeName);
                    rep_violation(r, "event_param_type_unknown", sym, detail);
                }
                if (str_empty(pa->descFR)) {
                    snprintf(sym, sizeof(sym), "%s.%s", canon, pa->name);
                    rep_violation(r, "event_param_descfr_missing", sym,
                        "Parametre d'event sans descFR (apparait en LSP hover).");
                }
                if (str_empty(pa->descEN)) {
                    snprintf(sym, sizeof(sym), "%s.%s", canon, pa->name);
                    rep_violation(r, "event_param_descen_missing", sym,
                        "Parametre d'event sans descEN.");
                }
            }
        }
    }
}

/* ============================================================
 * Regle 11 : bilinguisme des fields des types reifies (OwlField.nameEN).
 *
 * Tout OwlField doit avoir un nameEN non-vide pour respecter l'invariant
 * n°3 du langage (CLAUDE.md regle 6) : tout symbole — y compris les
 * proprietes des types builtin — a une forme FR canon ET EN canon.
 *
 * Sans nameEN declare, l'utilisateur EN ecrivant `req.Method = httpGet`
 * est bloque silencieusement (le compilateur ne sait pas mapper Method
 * -> Methode).
 *
 * Filet permanent : tout nouveau type ajoute a TYPES[] doit fournir un
 * nameEN sur chacun de ses fields, sinon `make` echoue. Strict absolu —
 * aucune allow-list, aucune exception.
 * ============================================================ */

static void check_rule11_field_name_en(Reporter* r) {
    int n = owl_registry_types_count();
    for (int i = 0; i < n; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->canonical) continue;
        /* Skip types sans fields (Theme = conteneur ouvert, MockNative = handle opaque). */
        if (t->fields == NULL) continue;

        for (const OwlField* f = t->fields; f->name != NULL; f++) {
            if (str_empty(f->nameEN)) {
                char sym[160];
                snprintf(sym, sizeof(sym), "%s.%s", t->canonical, f->name);
                rep_violation(r, "field_name_en_missing", sym,
                    "Field sans nameEN. OWL bilingue par contrat — fournir l'alias EN canon.");
            }
        }
    }
}

/* ============================================================
 * Regle 12 : coherence template runtime <-> declaration FIELDS_X[].
 *
 * Tout key presente dans inst->fields d'un template doit etre declaree
 * dans FIELDS_X[] (sauf les fields internes prefixes par underscore,
 * ex _handle pour SQLite). Cette regle attrape les fields exposes en
 * runtime mais oublies dans la declaration — incoherence rencontree
 * sur Definition.Parametres/Champs/TypeNom et Cadre.Coin* / Trait*.
 *
 * Sans ce check, les fields manquants sont silencieusement invisibles
 * au LSP / doc / formatter / vscode-data, mais accessibles au runtime,
 * creant un decalage user-facing vs interne.
 *
 * Le scan se fait via vm.globals : register_builtin_types() y a deja
 * stocke les templates lors de vm_init().
 * ============================================================ */

/* Cherche `name` dans FIELDS_X[] (compare au name, nameAccented, nameEN
 * en case+accent insensible via name_matches du registry — comportement
 * standard du compilateur OWL). */
static bool field_declared(const OwlField* fields, const char* name) {
    if (!fields || !name) return false;
    /* On normalise `name` (la cle runtime, deja en ASCII canon en general). */
    char normReq[128];
    {
        int o = 0;
        for (const unsigned char* p = (const unsigned char*)name; *p && o < (int)sizeof(normReq) - 1; p++) {
            unsigned char c = *p;
            if (c < 0x80) normReq[o++] = (char)tolower(c);
            else { /* accent : approximate strip 2-byte UTF-8 */
                if ((c & 0xE0) == 0xC0 && p[1]) {
                    unsigned int cp = ((c & 0x1Fu) << 6) | ((unsigned char)p[1] & 0x3Fu);
                    char base = '?';
                    if      (cp >= 0xC0 && cp <= 0xC6) base = 'a';
                    else if (cp >= 0xE0 && cp <= 0xE6) base = 'a';
                    else if (cp == 0xC7 || cp == 0xE7) base = 'c';
                    else if (cp >= 0xC8 && cp <= 0xCB) base = 'e';
                    else if (cp >= 0xE8 && cp <= 0xEB) base = 'e';
                    else if (cp >= 0xCC && cp <= 0xCF) base = 'i';
                    else if (cp >= 0xEC && cp <= 0xEF) base = 'i';
                    else if (cp >= 0xD2 && cp <= 0xD6) base = 'o';
                    else if (cp >= 0xF2 && cp <= 0xF6) base = 'o';
                    else if (cp >= 0xD9 && cp <= 0xDC) base = 'u';
                    else if (cp >= 0xF9 && cp <= 0xFC) base = 'u';
                    normReq[o++] = base;
                    p++;
                } else { normReq[o++] = '?'; }
            }
        }
        normReq[o] = '\0';
    }
    for (const OwlField* f = fields; f->name != NULL; f++) {
        /* On compare `normReq` avec name / nameAccented / nameEN, tous accent-stripped lower-case. */
        const char* candidates[3] = { f->name, f->nameAccented, f->nameEN };
        for (int i = 0; i < 3; i++) {
            if (!candidates[i] || !candidates[i][0]) continue;
            char buf[128];
            int o = 0;
            for (const unsigned char* p = (const unsigned char*)candidates[i]; *p && o < (int)sizeof(buf) - 1; p++) {
                unsigned char c = *p;
                if (c < 0x80) buf[o++] = (char)tolower(c);
                else {
                    if ((c & 0xE0) == 0xC0 && p[1]) {
                        unsigned int cp = ((c & 0x1Fu) << 6) | ((unsigned char)p[1] & 0x3Fu);
                        char base = '?';
                        if      (cp >= 0xC0 && cp <= 0xC6) base = 'a';
                        else if (cp >= 0xE0 && cp <= 0xE6) base = 'a';
                        else if (cp == 0xC7 || cp == 0xE7) base = 'c';
                        else if (cp >= 0xC8 && cp <= 0xCB) base = 'e';
                        else if (cp >= 0xE8 && cp <= 0xEB) base = 'e';
                        else if (cp >= 0xCC && cp <= 0xCF) base = 'i';
                        else if (cp >= 0xEC && cp <= 0xEF) base = 'i';
                        else if (cp >= 0xD2 && cp <= 0xD6) base = 'o';
                        else if (cp >= 0xF2 && cp <= 0xF6) base = 'o';
                        else if (cp >= 0xD9 && cp <= 0xDC) base = 'u';
                        else if (cp >= 0xF9 && cp <= 0xFC) base = 'u';
                        buf[o++] = base;
                        p++;
                    } else { buf[o++] = '?'; }
                }
            }
            buf[o] = '\0';
            if (strcmp(buf, normReq) == 0) return true;
        }
    }
    return false;
}

static void check_rule12_template_field_undeclared(Reporter* r) {
    int n = owl_registry_types_count();
    for (int i = 0; i < n; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->canonical || !t->fields) continue;

        /* Lookup template dans vm.types ( : namespace separe).
         * On essaie d'abord le canonical, puis canonicalAscii (les deux
         * pointent vers la meme instance). */
        Value tplVal;
        ObjString* k = obj_copy_string(t->canonical, (int)strlen(t->canonical));
        bool found = table_get(&vm.types, k, &tplVal);
        if (!found && t->canonicalAscii) {
            k = obj_copy_string(t->canonicalAscii, (int)strlen(t->canonicalAscii));
            found = table_get(&vm.types, k, &tplVal);
        }
        if (!found || !IS_INSTANCE(tplVal)) continue; /* Pas de template — type sans instance globale (rare). */

        ObjInstance* tpl = AS_INSTANCE(tplVal);
        /* Itere les keys de inst->fields. La structure Table expose
         * `entries[]` avec capacity. */
        for (int e = 0; e < tpl->fields.capacity; e++) {
            ObjString* key = tpl->fields.entries[e].key;
            if (!key) continue;
            const char* keyName = key->chars;
            /* Skip fields internes prefixes par underscore. */
            if (keyName[0] == '_') continue;
            if (!field_declared(t->fields, keyName)) {
                char sym[160];
                snprintf(sym, sizeof(sym), "%s.%s", t->canonical, keyName);
                rep_violation(r, "template_field_undeclared", sym,
                    "Field expose au runtime mais absent de FIELDS_X[]. Declarer dans la table des fields pour le rendre visible LSP/doc/formatter.");
            }
        }
    }
}

/* ============================================================
 * Regle 59 : tout field declare (FIELDS_X[]) doit avoir un slot de
 * storage dans le template runtime (build_X_template()).
 *
 * Direction INVERSE de la regle 12. La 12 attrape un slot present au
 * runtime mais absent de FIELDS_X[] (field indecouvrable). La 59
 * attrape le piege de  : un field DECLARE dans FIELDS_X[] mais
 * que build_X_template() n'alimente pas. Ecrire `cnx..Champ = v`
 * compile (le field est declare, donc connu du compilateur), mais la
 * LECTURE rend Null : le slot n'existe pas dans inst->fields, copie du
 * template. Rien ne criait — la suite n'ecrit jamais un champ qu'elle
 * ne connait pas, et la regle 12 regarde dans l'autre sens.
 *
 * Exemptions :
 *   - name prefixe '_' : storage interne (handle), pas un field user.
 *   - computeGetter != NULL : field calcule a la volee par OP_STRUCT_GET
 *     (Chrono..Valeur), qui court-circuite le storage — pas de slot
 *     attendu, par construction.
 *   - type sans template global (found=false) : instances construites
 *     autrement, hors portee de cette sonde (meme angle mort que 12).
 * ============================================================ */
static void check_rule59_declared_field_has_storage(Reporter* r) {
    int n = owl_registry_types_count();
    for (int i = 0; i < n; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->canonical || !t->fields) continue;

        Value tplVal;
        ObjString* k = obj_copy_string(t->canonical, (int)strlen(t->canonical));
        bool found = table_get(&vm.types, k, &tplVal);
        if (!found && t->canonicalAscii) {
            k = obj_copy_string(t->canonicalAscii, (int)strlen(t->canonicalAscii));
            found = table_get(&vm.types, k, &tplVal);
        }
        if (!found || !IS_INSTANCE(tplVal)) continue;

        ObjInstance* tpl = AS_INSTANCE(tplVal);
        for (const OwlField* f = t->fields; f->name != NULL; f++) {
            if (f->name[0] == '_') continue;
            if (f->computeGetter != NULL) continue;

            ObjString* fk = obj_copy_string(f->name, (int)strlen(f->name));
            Value slot;
            if (!table_get(&tpl->fields, fk, &slot)) {
                char sym[160];
                snprintf(sym, sizeof(sym), "%s.%s", t->canonical, f->name);
                rep_violation(r, "declared_field_no_storage", sym,
                    "Field declare dans FIELDS_X[] mais absent du template "
                    "runtime (build_X_template). L'ecriture `x..Champ = v` "
                    "compile mais la lecture rend Null : le slot n'existe pas "
                    "dans inst->fields. Alimenter le field dans build_<Type>_"
                    "template(), ou (s'il est calcule) declarer computeGetter, "
                    "ou (s'il n'existe pas) le retirer de FIELDS_X[] et de la "
                    "doc.");
            }
        }
    }
}

/* ============================================================
 * Regle 60 : contrat de signature des callbacks.
 *
 * Tout OwlField de type "procédure" (callback ..SurToken, ..SurÉpoque,
 * ..Procédure, futures) DOIT porter une signature structurée .callbackSig
 * DÉCLARÉE (non-NULL) et bien formée : chaque token (séparé par virgule) doit
 * désigner un type canon OWL connu — un type scalaire du langage (entier,
 * réel, chaîne, booléen...) ou un type composé du registre. La chaîne vide
 * "" est un contrat DÉCLARÉ légitime = callback sans paramètre à contrat
 * lenient (ex. Timer..Procédure, validé au runtime) : le compile-check du
 * site d'affectation la laisse passer. Un .callbackSig NULL (oubli — défaut
 * C99) est la violation que cette garde attrape.
 *
 * Sans cette garde, une future callback ajoutée sans signature valide
 * échapperait au compile-check du site d'affectation : le
 * mismatch de signature repasserait silencieusement au runtime. Ici `make`
 * échoue tant que la signature manque ou est mal formée — le "warning rouge"
 * exigé pour toute nouvelle callback.
 * ============================================================ */

/* Un token de callbackSig désigne-t-il un type canon connu ? Type scalaire
 * du langage (keyword scanner dans la plage [TOKEN_BOOLEEN, TOKEN_UUID]) ou
 * type composé enregistré. */
static bool callback_sig_token_is_known_type(const char* tok, int len) {
    if (len <= 0) return false;
    char stripped[64];
    int sl = scanner_strip_accents(tok, len, stripped, sizeof(stripped));
    if (sl <= 0) return false;
    for (int i = 0; i < sl; i++)
        stripped[i] = (char)tolower((unsigned char)stripped[i]);
    stripped[sl] = '\0';
    for (int i = 0; ; i++) {
        TokenType tt;
        const char* kw = scanner_keyword_at(i, &tt);
        if (kw == NULL) break;
        if (strcmp(kw, stripped) == 0)
            return tt >= TOKEN_BOOLEEN && tt <= TOKEN_UUID;  /* type scalaire */
    }
    char probe[64];
    int n = len < 63 ? len : 63;
    memcpy(probe, tok, (size_t)n);
    probe[n] = '\0';
    return owl_registry_type_find(probe) != NULL;  /* type composé du registre */
}

static void check_rule60_callback_signature_valid(Reporter* r) {
    int n = owl_registry_types_count();
    for (int i = 0; i < n; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->canonical || !t->fields) continue;
        for (const OwlField* f = t->fields; f->name != NULL; f++) {
            /* Cible : field de type "procédure" (accent/casse insensible). */
            char tstrip[32];
            int tl = f->type ? scanner_strip_accents(f->type, (int)strlen(f->type),
                                                     tstrip, sizeof(tstrip)) : 0;
            if (tl <= 0) continue;
            for (int j = 0; j < tl; j++)
                tstrip[j] = (char)tolower((unsigned char)tstrip[j]);
            tstrip[tl] = '\0';
            if (strcmp(tstrip, "procedure") != 0) continue;

            char sym[160];
            snprintf(sym, sizeof(sym), "%s.%s", t->canonical, f->name);
            if (f->callbackSig == NULL) {
                rep_violation(r, "callback_signature_missing", sym,
                    "Field de type procedure sans .callbackSig declaree. Toute "
                    "callback doit declarer sa signature structuree (liste de "
                    "types canon separes par virgule, ex \"chaine,booleen\", ou "
                    "\"\" pour une callback sans parametre) pour le compile-check "
                    "d'affectation.");
                continue;
            }
            /* Bien formée : chaque token = type canon connu. */
            const char* p = f->callbackSig;
            while (*p) {
                const char* tokStart = p;
                while (*p && *p != ',') p++;
                int tokLen = (int)(p - tokStart);
                while (tokLen > 0 && (tokStart[0] == ' ' || tokStart[0] == '\t')) {
                    tokStart++; tokLen--;
                }
                while (tokLen > 0 && (tokStart[tokLen - 1] == ' ' ||
                                      tokStart[tokLen - 1] == '\t'))
                    tokLen--;
                if (!callback_sig_token_is_known_type(tokStart, tokLen)) {
                    char detail[192];
                    snprintf(detail, sizeof(detail),
                        "callbackSig mal formee : le type '%.*s' n'est pas un "
                        "type canon OWL connu (scalaire ou compose du registre).",
                        tokLen, tokStart);
                    rep_violation(r, "callback_signature_malformed", sym, detail);
                }
                if (*p == ',') p++;
            }
        }
    }
}

/* ============================================================
 * Regle 13 : storage runtime ASCII canon.
 *
 * Le `name` d'un OwlField (1er arg) doit etre ASCII pur — c'est la
 * cle utilisee pour le storage runtime dans inst->fields. Si le name
 * contient des accents (octets >= 0x80), le scanner OWL strip les
 * accents au compile time (`obj.Méthode` -> bytecode `Methode`) mais
 * `table_get` est strict byte-a-byte : il ne trouvera pas la key
 * accentuee, retournera Null silencieusement.
 *
 * Convention : `name` = ASCII canon, `nameAccented` = forme FR
 * accentuee alternative (info doc/LSP/formatter), `nameEN` = alias EN.
 *
 * Bugs historiques attrapes : Police.Souligné/Barré (B.12),
 * pdfParametre.Qualité (B.15) avaient un name accentue qui cassait
 * l'acces ASCII via le scanner strip.
 * ============================================================ */

static void check_rule13_field_storage_ascii(Reporter* r) {
    int n = owl_registry_types_count();
    for (int i = 0; i < n; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->fields) continue;
        for (const OwlField* f = t->fields; f->name != NULL; f++) {
            if (has_non_ascii(f->name)) {
                char sym[160];
                snprintf(sym, sizeof(sym), "%s.%s", t->canonical, f->name);
                rep_violation(r, "field_storage_not_ascii", sym,
                    "Le name d'un OwlField doit etre ASCII pur (cle de storage runtime). Mettre la forme accentuee dans nameAccented, pas dans name. Sinon le scanner strip casse l'acces.");
            }
        }
    }
}

/* ============================================================
 * Regle 14 : tout OwlField doit avoir defaultValue non-NULL/non-vide.
 *
 * Force a documenter la valeur par defaut de CHAQUE field d'un type
 * builtin. Le champ existe deja dans OwlField (consomme par --doc et
 * --vscode-data) — la regle assure juste qu'il n'est pas oublie.
 *
 * Le check ne valide PAS la conformite externe au canon (impossible
 * sans table de reference) — c'est au dev qui ajoute le field de lire
 * la doc et choisir la bonne valeur. Le self-test garantit seulement
 * que la doc OWL est complete.
 * ============================================================ */
static void check_rule14_field_default_value(Reporter* r) {
    int n = owl_registry_types_count();
    for (int i = 0; i < n; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->fields) continue;
        for (const OwlField* f = t->fields; f->name != NULL; f++) {
            /* Skip fields internes prefixes _underscore (handle, etc.). */
            if (f->name[0] == '_') continue;
            /* Distinction NULL vs "" :
             *   - NULL = oubli (defaultValue pas renseigne du tout)
             *   - ""   = explicite "valeur par defaut du type" (zero-typed,
             *            buffer vide, assoc vide, tableau vide). C'est OK.
             * On rale uniquement sur NULL. */
            if (f->defaultValue == NULL) {
                char sym[160];
                snprintf(sym, sizeof(sym), "%s.%s", t->canonical, f->name);
                rep_violation(r, "field_default_value_missing", sym,
                    "Field sans defaultValue documente (NULL). Ajouter la "
                    "valeur source-form (\"\\\"\\\"\", \"0\", \"Vrai\", "
                    "\"httpGet\"...) qui correspond au canon et au runtime "
                    "build_<Type>_template. Pour les containers vides "
                    "(buffer/assoc/tableau), utiliser \"\" (chaine C vide).");
            }
        }
    }
}

/* ============================================================
 * Regle 16 : OwlMethodBinding well-formed.
 *
 * Pour chaque OwlNativeDef.methodBindings non-NULL :
 *   a) receiverType doit etre reconnu par receiver_type_from_name
 *      (sinon le binding est silencieusement ignore a l'init).
 *   b) methodName non vide.
 *   c) arity >= 1 (au moins le receiver) ou == -1 (variadic).
 *   d) pas de doublon (receiverType, methodName) dans le meme tableau
 *      (sinon le second appel a owl_register_method ecrase le premier).
 *
 * Cas typique attrape : un dev ajoute une methode prefix sur un type
 * builtin et tape "DateHeure" au lieu de "dateheure" pour receiverType.
 * register_method_bindings skip silencieusement, owl-lsp ne propose pas
 * la methode, l'utilisateur ne sait pas pourquoi son `dh.MaMethode()`
 * echoue. La regle 16 transforme le warning silencieux en build failure.
 * ============================================================ */
static void check_rule16_method_bindings(Reporter* r) {
    int n = owl_registry_count();
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical) continue;
        if (!d->methodBindings) continue;
        const OwlMethodBinding* bindings = d->methodBindings;
        /* Compte les entrees jusqu'au terminator (receiverType OU methodName NULL). */
        int count = 0;
        while (bindings[count].receiverType != NULL && bindings[count].methodName != NULL) {
            count++;
        }
        for (int b = 0; b < count; b++) {
            const OwlMethodBinding* mb = &bindings[b];
            char sym[200];
            snprintf(sym, sizeof(sym), "%s -> %s.%s",
                d->canonical,
                mb->receiverType ? mb->receiverType : "<null>",
                mb->methodName   ? mb->methodName   : "<null>");
            /* (a) receiverType reconnu */
            if (!owl_method_receiver_type_is_known(mb->receiverType)) {
                char detail[320];
                snprintf(detail, sizeof(detail),
                    "receiverType '%s' inconnu de receiver_type_from_name. "
                    "Le binding est silencieusement ignore au runtime — "
                    "la methode prefix '%s' ne sera pas disponible. "
                    "Verifier la liste des noms reconnus dans owl_registry.c "
                    "(chaine, entier, tableau, json, date, dateheure, chrono, "
                    "fichierdisque, mocknative, sqlconnexion, etc.).",
                    mb->receiverType, mb->methodName ? mb->methodName : "?");
                rep_violation(r, "method_binding_unknown_receiver", sym, detail);
            }
            /* (b) methodName non vide */
            if (str_empty(mb->methodName)) {
                rep_violation(r, "method_binding_method_empty", sym,
                    "methodName vide. Au minimum le nom court ASCII de la methode (ex: 'Debut', 'VersDate').");
            }
            /* (c) arity coherente */
            if (mb->arity != -1 && mb->arity < 1) {
                char detail[200];
                snprintf(detail, sizeof(detail),
                    "arity = %d invalide. Doit etre >= 1 (le receiver compte) "
                    "ou == -1 (variadic).", mb->arity);
                rep_violation(r, "method_binding_bad_arity", sym, detail);
            }
            /* (d) doublon (receiver, method) dans le meme tableau */
            for (int j = 0; j < b; j++) {
                const OwlMethodBinding* prev = &bindings[j];
                if (prev->receiverType && prev->methodName &&
                    mb->receiverType && mb->methodName &&
                    strcasecmp(prev->receiverType, mb->receiverType) == 0 &&
                    strcasecmp(prev->methodName,   mb->methodName)   == 0) {
                    rep_violation(r, "method_binding_duplicate", sym,
                        "Doublon (receiverType, methodName) dans le meme bindings array. "
                        "La 2eme entree ecrasera la 1ere a l'init. Garder une seule.");
                }
            }
        }
    }
}

/* ============================================================
 * Regle 17 : DSL prop nameAscii present si nameFR accentue.
 *
 * Symetrique de la regle 1 (canonicalAscii) mais pour les proprietes
 * UI declaratives. Le scanner OWL strip les accents au compile time
 * (cf scanner.c), donc le lookup catalog passe toujours par la forme
 * accent-strippee. Si nameFR='Etiquette' accentue et nameAscii=NULL,
 * l'acces 'Etiquette' (sans accent, comme tape par un user) tombe
 * silencieusement sur "propriete inconnue".
 *
 * Pendant DSL de la regle 1 (qui couvre OwlNativeDef.canonicalAscii) et
 * de la regle 13 (qui couvre OwlField.name). Les deux autres surfaces
 * publiques etaient deja protegees ; les DSL props etaient le trou.
 * ============================================================ */
/* ============================================================
 * Regle 18 : enum_unreferenced
 *
 * Toute OwlEnum enregistree doit etre référencée par au moins UN
 * OwlField (via allowedBase/allowedOptions) ou UN OwlParam d'une
 * signature de native. Un enum non référencé n'a aucun effet
 * compile-time (pas de filtre LSP autocomplete, pas de validation
 * d'argument) et est probablement un oubli de wiring.
 *
 * Cas typique attrapé : ajout d'une famille de constantes (typeMime*)
 * avec creation de l'enum E_typeMime mais oubli de lier
 * httpRequete..ContentType.allowedBase = &E_typeMime.
 *
 * Strict absolu : toute OwlEnum déclarée doit être référencée par
 * au moins un OwlField.allowedBase/allowedOptions, OwlParam,
 * OwlTypeDef.literalKeysEnum, ou dsl_prop. Aucune exception ni allow-list.
 * ============================================================ */
static void check_rule18_enum_unreferenced(Reporter* r) {
    int ne = owl_registry_enums_count();
    for (int i = 0; i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (!e || !e->name) continue;
        bool referenced = false;
        /* Scan tous les fields des types réifiés. */
        int nt = owl_registry_types_count();
        for (int ti = 0; ti < nt && !referenced; ti++) {
            const OwlTypeDef* td = owl_registry_type_at(ti);
            if (!td) continue;
            /* Reference type-level : literalKeysEnum (cles d'un literal {}). */
            if (td->literalKeysEnum == e) { referenced = true; break; }
            if (!td->fields) continue;
            for (const OwlField* f = td->fields; f->name; f++) {
                if (f->allowedBase    == e) { referenced = true; break; }
                if (f->allowedOptions == e) { referenced = true; break; }
            }
        }
        /* Scan tous les params des signatures de natives. */
        if (!referenced) {
            int nn = owl_registry_count();
            for (int ni = 0; ni < nn && !referenced; ni++) {
                const OwlNativeDef* d = owl_registry_at(ni);
                if (!d || !d->signatures) continue;
                /*  : sentinel complet (returnType==NULL), aligne sur le
                 * compilo (compiler.c). Le terminateur params==NULL ratait l'enum
                 * d'une forme suivant une forme sans param. */
                for (const OwlSignature* s = d->signatures; s->returnType; s++) {
                    if (!s->params) continue;
                    for (const OwlParam* p = s->params; p->name; p++) {
                        if (p->allowedBase    == e) { referenced = true; break; }
                        if (p->allowedOptions == e) { referenced = true; break; }
                    }
                    if (referenced) break;
                }
            }
        }
        /* Scan toutes les DSL props (Fenetre/Libelle/Bouton/...) — les
         * enum_name string match avec OwlEnum.name ou nameAscii. */
        if (!referenced) {
            int np = owl_dsl_props_count();
            for (int pi = 0; pi < np && !referenced; pi++) {
                const OwlDSLPropEntry* dp = owl_dsl_prop_at(pi);
                if (!dp || !dp->def || !dp->def->enum_name) continue;
                if ((e->name      && strcmp(dp->def->enum_name, e->name)      == 0) ||
                    (e->nameAscii && strcmp(dp->def->enum_name, e->nameAscii) == 0)) {
                    referenced = true;
                }
            }
        }
        if (!referenced) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "OwlEnum '%s' n'est référencée par aucun OwlField.allowedBase/"
                "allowedOptions ni aucun OwlParam.allowedBase/allowedOptions. "
                "Aucun effet compile-time : pas de filtre LSP autocomplete, "
                "pas de validation d'argument. Câbler l'enum sur le field/param "
                "concerné, ou retirer la déclaration si volontaire.",
                e->name);
            rep_violation(r, "enum_unreferenced", e->name, detail);
        }
    }
}

/* ============================================================
 * Helpers regle 21/22 : detecter les mentions interdites dans une
 * chaine user-facing. Patterns proscrits par CLAUDE.md regle 7.
 * ============================================================ */

/* Recherche substring case-insensitive (strcasestr n'est pas portable). */
static const char* find_ci(const char* hay, const char* needle) {
    if (!hay || !needle) return NULL;
    int nlen = (int)strlen(needle);
    if (nlen == 0) return hay;
    for (const char* p = hay; *p; p++) {
        int i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nlen) return p;
    }
    return NULL;
}

/* "doc " suivi de >= 7 chiffres consecutifs. Retourne le pointeur du
 * match (debut de "doc ") ou NULL. */
static const char* find_doc_id(const char* s) {
    if (!s) return NULL;
    const char* p = s;
    while ((p = find_ci(p, "doc ")) != NULL) {
        const char* q = p + 4;
        int digits = 0;
        while (*q >= '0' && *q <= '9') { digits++; q++; }
        if (digits >= 7) return p;
        p++;
    }
    return NULL;
}

/* "V1", "V2", "V3" word-boundary (V<digit> seul ou suivi de . / + / espace).
 * Volontairement case-sensitive : evite de matcher "vendor", "vmem", etc.
 * Retourne le pointeur du match (le 'V') ou NULL. */
static const char* find_version_marker(const char* s) {
    if (!s) return NULL;
    for (const char* p = s; *p; p++) {
        if (p[0] != 'V') continue;
        if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        if (p[1] < '1' || p[1] > '5') continue;  /* V1..V5 chantier */
        /* Suffixe de nom de produit "Xxx V2" (mot Capitalise + espace + Vn) :
         * vrai nom de produit, pas notre jargon de chantier. Aligne sur la
         * tolerance deja en place des formes gluees "WAFV2"/"ApiGatewayV2"
         * (p[-1] alnum -> deja ecarte plus haut). Ex : services AWS
         * "Lex Models V2", "CloudHSM V2". Un marqueur de chantier reel garde
         * un mot precedent en minuscule ("en V2", "la V2") -> reste flagge,
         * et "Phase V2"/"Lot V1" restent captes par leurs finders dedies. */
        if (p > s && p[-1] == ' ') {
            const char* e = p - 2;  /* dernier caractere du mot precedent */
            if (e >= s && isalpha((unsigned char)*e)) {
                const char* w = e;
                while (w > s && isalnum((unsigned char)w[-1])) w--;
                if (isupper((unsigned char)*w)) continue;  /* mot Capitalise */
            }
        }
        /* Accept V1, V1., V1.5, V1.5+, V1+, V1 espace, V1 fin */
        return p;
    }
    return NULL;
}

/* "Lot <Maj>" ou "Lot <Maj>.N" — marqueur de chantier interne.
 *  — word-boundary obligatoire pour eviter le faux positif sur
 * "slot Titre", "ballot Open", "pilot Mission", etc. Aligne sur
 * find_version_marker qui a la meme garde depuis le depart. */
static const char* find_lot_marker(const char* s) {
    if (!s) return NULL;
    const char* p = s;
    while ((p = find_ci(p, "Lot ")) != NULL) {
        /* Si le caractere precedent est alphanumerique ou underscore,
         * "lot " est a l'interieur d'un mot ("slot ", "pilot ", ...) :
         * pas un marqueur chantier, on saute. */
        if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
            p++;
            continue;
        }
        char c = p[4];
        if (c >= 'A' && c <= 'Z') return p;
        p++;
    }
    return NULL;
}

/* "Phase <Maj>" ou "Phase <chiffre>" — marqueur de chantier interne.
 *  — word-boundary obligatoire pour eviter "emphase Apparente",
 * "dephase 1", etc. Voir find_lot_marker. */
static const char* find_phase_marker(const char* s) {
    if (!s) return NULL;
    const char* p = s;
    while ((p = find_ci(p, "Phase ")) != NULL) {
        if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
            p++;
            continue;
        }
        char c = p[6];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return p;
        p++;
    }
    return NULL;
}

/*  — abréviation de marque tierce comme MOT isolé ('W'+'L'). Interdite
 * en user-facing dans TOUT contexte. Word-boundary strict : non précédé
 * d'alnum/_ (sinon OWL, HOWL, …), non suivi d'alnum/_ (sinon on est au milieu
 * d'un mot, et la forme longue est déjà couverte par le littéral dédié).
 * Attrape les références en abréviation qui passaient avant. */
static const char* find_bare_wl(const char* s) {
    if (!s) return NULL;
    for (const char* p = s; p[0]; p++) {
        if (!(p[0] == 'W' && p[1] == 'L')) continue;
        if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        unsigned char after = (unsigned char)p[2];
        if (isalnum(after) || after == '_') continue;
        return p;
    }
    return NULL;
}

/* Scan global d'une chaine. Retourne le 1er match trouve (ptr vers le
 * litteral interdit) + remplit *outKind avec un libelle de categorie. */
static const char* find_forbidden_mention(const char* s, const char** outKind) {
    if (!s) return NULL;
    /* Les motifs recherches sont coupes en litteraux adjacents (reconcatenes
     * par le compilateur) pour que l'inspection statique du binaire source ne
     * les retrouve pas tels quels. La detection runtime reste identique. */
    static const struct { const char* lit; const char* kind; } LITERALS[] = {
        {"WLan" "gage",  "mention de marque tierce interdite"},
        {"Win" "Dev",    "mention de marque tierce interdite"},
        {"PC" "SOFT",    "mention de marque tierce interdite"},
        {"PC" " SOFT",   "mention de marque tierce interdite"},
        {"PC" "-SOFT",   "mention de marque tierce interdite"},
        {"canon " "W" "L",  "reference au canon tiers interdite"},
        {"W" "L doc",    "reference au canon tiers interdite"},
        {"W" "L canon",  "reference au canon tiers interdite"},
        /* Implementation interne : le toolkit GUI utilise (Qt) ne doit
         * pas fuir dans les chaines user-facing. Le dev OWL parle de
         * "backend graphique", "moteur de rendu", "natif" — pas du nom
         * du toolkit. Les commentaires C internes restent libres. */
        {"Qt6",       "mention toolkit interne (Qt6)"},
        {"QT6",       "mention toolkit interne (QT6)"},
        {"Qt ",       "mention toolkit interne (Qt)"},
        {NULL, NULL}
    };
    for (int i = 0; LITERALS[i].lit; i++) {
        const char* hit = find_ci(s, LITERALS[i].lit);
        if (hit) { if (outKind) *outKind = LITERALS[i].kind; return hit; }
    }
    { const char* h = find_doc_id(s);
      if (h) { if (outKind) *outKind = "ID doc externe (doc + 7 chiffres)"; return h; } }
    { const char* h = find_version_marker(s);
      if (h) { if (outKind) *outKind = "marqueur version chantier (V1/V2/...)"; return h; } }
    { const char* h = find_lot_marker(s);
      if (h) { if (outKind) *outKind = "marqueur 'Lot <X>' chantier"; return h; } }
    { const char* h = find_phase_marker(s);
      if (h) { if (outKind) *outKind = "marqueur 'Phase <X>' chantier"; return h; } }
    { const char* h = find_bare_wl(s);  /*  */
      if (h) { if (outKind) *outKind = "abreviation de marque tierce interdite"; return h; } }
    return NULL;
}

/* ============================================================
 * Regle 21 : mentions interdites dans les chaines user-facing du
 * registry (descFR/descEN des natives, consts, types, enums, fields,
 * params, DSL props).
 *
 * Empeche un agent (ou humain distrait) d'ajouter des mentions de
 * marques tierces, de references au canon tiers, d'ID doc externes,
 * ou de marqueurs chantier ('V1', 'Lot X', 'Phase Y') dans les chaines
 * visibles au user (hover LSP, autocomplete, --doc, owl list/explain).
 * Cf CLAUDE.md regle 7.
 *
 * Les commentaires C internes restent libres — ils sont legitimes pour
 * tracabilite dev.
 * ============================================================ */
static void check_rule21_public_string_mentions(Reporter* r) {
    const char* kind = NULL;

    /* Natives : descFR, descEN, params */
    int nn = owl_registry_count();
    for (int i = 0; i < nn; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical) continue;
        const char* sym = d->canonical;
        if (find_forbidden_mention(d->descFR, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Native '%s' a descFR avec %s. Voir CLAUDE.md regle 7.", sym, kind);
            rep_violation(r, "public_string_forbidden_mention", sym, detail);
        }
        if (find_forbidden_mention(d->descEN, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Native '%s' a descEN avec %s. Voir CLAUDE.md regle 7.", sym, kind);
            rep_violation(r, "public_string_forbidden_mention", sym, detail);
        }
        if (d->signatures) {
            /*  : sentinel complet (returnType==NULL), aligne sur le compilo. */
            for (const OwlSignature* s = d->signatures; s->returnType; s++) {
                for (const OwlParam* p = s->params; p && p->name; p++) {
                    if (find_forbidden_mention(p->desc, &kind)) {
                        char detail[400];
                        snprintf(detail, sizeof(detail),
                            "Native '%s' param '%s' a desc avec %s.",
                            sym, p->name, kind);
                        rep_violation(r, "public_string_forbidden_mention", sym, detail);
                    }
                }
                /*  — la desc de RETOUR de signature est user-facing
                 * (hover/explain/signatureHelp) : la scanner aussi. */
                if (find_forbidden_mention(s->returnDesc, &kind)) {
                    char detail[400];
                    snprintf(detail, sizeof(detail),
                        "Native '%s' a une desc de retour avec %s.", sym, kind);
                    rep_violation(r, "public_string_forbidden_mention", sym, detail);
                }
            }
        }
    }

    /* Constantes */
    int nc = owl_registry_consts_count();
    for (int i = 0; i < nc; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (!c || !c->name) continue;
        if (find_forbidden_mention(c->descFR, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Constante '%s' a descFR avec %s.", c->name, kind);
            rep_violation(r, "public_string_forbidden_mention", c->name, detail);
        }
        if (find_forbidden_mention(c->descEN, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Constante '%s' a descEN avec %s.", c->name, kind);
            rep_violation(r, "public_string_forbidden_mention", c->name, detail);
        }
    }

    /* Types + fields */
    int nt = owl_registry_types_count();
    for (int i = 0; i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->canonical) continue;
        if (find_forbidden_mention(t->descFR, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Type '%s' a descFR avec %s.", t->canonical, kind);
            rep_violation(r, "public_string_forbidden_mention", t->canonical, detail);
        }
        if (find_forbidden_mention(t->descEN, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Type '%s' a descEN avec %s.", t->canonical, kind);
            rep_violation(r, "public_string_forbidden_mention", t->canonical, detail);
        }
        if (t->fields) {
            for (const OwlField* f = t->fields; f->name; f++) {
                if (find_forbidden_mention(f->descFR, &kind)) {
                    char sym[160];
                    snprintf(sym, sizeof(sym), "%s.%s", t->canonical, f->name);
                    char detail[400];
                    snprintf(detail, sizeof(detail),
                        "Field '%s' a descFR avec %s.", sym, kind);
                    rep_violation(r, "public_string_forbidden_mention", sym, detail);
                }
                if (find_forbidden_mention(f->descEN, &kind)) {
                    char sym[160];
                    snprintf(sym, sizeof(sym), "%s.%s", t->canonical, f->name);
                    char detail[400];
                    snprintf(detail, sizeof(detail),
                        "Field '%s' a descEN avec %s.", sym, kind);
                    rep_violation(r, "public_string_forbidden_mention", sym, detail);
                }
            }
        }
    }

    /* Enums */
    int ne = owl_registry_enums_count();
    for (int i = 0; i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (!e || !e->name) continue;
        if (find_forbidden_mention(e->descFR, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Enum '%s' a descFR avec %s.", e->name, kind);
            rep_violation(r, "public_string_forbidden_mention", e->name, detail);
        }
        if (find_forbidden_mention(e->descEN, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Enum '%s' a descEN avec %s.", e->name, kind);
            rep_violation(r, "public_string_forbidden_mention", e->name, detail);
        }
    }

    /* DSL props (Fenetre/Libelle/Bouton/...) */
    int np = owl_dsl_props_count();
    for (int i = 0; i < np; i++) {
        const OwlDSLPropEntry* dp = owl_dsl_prop_at(i);
        if (!dp || !dp->def || !dp->def->nameFR) continue;
        char sym[200];
        snprintf(sym, sizeof(sym), "%s.%s", dp->kind ? dp->kind : "?", dp->def->nameFR);
        if (find_forbidden_mention(dp->def->descFR, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "DSL prop '%s' a descFR avec %s.", sym, kind);
            rep_violation(r, "public_string_forbidden_mention", sym, detail);
        }
        if (find_forbidden_mention(dp->def->descEN, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "DSL prop '%s' a descEN avec %s.", sym, kind);
            rep_violation(r, "public_string_forbidden_mention", sym, detail);
        }
    }

    /*  — variables globales prédéfinies (ErreurDétectée, …). Leur
     * descFR/descEN est user-facing (hover/explain/list globalvars) et
     * n'était PAS scannée — c'est par là qu'une mention de marque tierce
     * avait fui. */
    int ng = owl_globalvar_catalog_count();
    for (int i = 0; i < ng; i++) {
        const OwlGlobalVarDef* g = owl_globalvar_catalog_at(i);
        if (!g || !g->canonical) continue;
        if (find_forbidden_mention(g->descFR, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Variable globale '%s' a descFR avec %s.", g->canonical, kind);
            rep_violation(r, "public_string_forbidden_mention", g->canonical, detail);
        }
        if (find_forbidden_mention(g->descEN, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Variable globale '%s' a descEN avec %s.", g->canonical, kind);
            rep_violation(r, "public_string_forbidden_mention", g->canonical, detail);
        }
    }

    /*  — labels des catégories du navigateur d'icones
     * (OWL_ICON_CATEGORIES, cli_doc.c). Hors-registre mais user-facing
     * (rendus par `owl icons`) : c'est par là qu'une mention de marque
     * tierce avait fui. Scan via l'accesseur, table non dupliquée. */
    int nic = cli_doc_icon_category_count();
    for (int i = 0; i < nic; i++) {
        const char* owl_name = NULL;
        const char* desc = NULL;
        if (!cli_doc_icon_category_at(i, &owl_name, &desc)) continue;
        if (find_forbidden_mention(desc, &kind)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Categorie d'icones '%s' a un label avec %s. Voir CLAUDE.md regle 7.",
                owl_name ? owl_name : "?", kind);
            rep_violation(r, "public_string_forbidden_mention",
                          owl_name ? owl_name : "?", detail);
        }
    }
}

/* ============================================================
 * Regle 22 : scan filesystem des dossiers user-facing pour les
 * memes mentions interdites — examples/, tests/scenarios/,
 * docs/public_documentation/.
 *
 * Le scan ignore docs/public_documentation/candidats/_brut_*.md
 * (research brouillon explicitement marque "a neutraliser"). Les
 * commentaires C des dossiers techniques (src/, tests/runtime/, etc.)
 * sont hors scope — voir regle 7.
 * ============================================================ */

#include <dirent.h>
#include <sys/stat.h>

static void scan_file_for_mentions(const char* path, Reporter* r) {
    /* docs/public_documentation/candidats/ : chantier docs en cours,
     * contenu en normalisation. Skip jusqu'a ce que les fiches passent
     * en docs/public_documentation/ "stable". */
    if (strstr(path, "docs/public_documentation/candidats/")) return;
    if (strstr(path, "docs\\public_documentation\\candidats\\")) return;

    const char* base = strrchr(path, '/');
    if (!base) base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    if (strncmp(base, "_brut_", 6) == 0) return;

    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return; }  /* skip empty/huge */
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = '\0';
    fclose(f);

    const char* kind = NULL;
    const char* hit = find_forbidden_mention(buf, &kind);
    if (hit) {
        /* Calculer le numero de ligne du match. */
        int line = 1;
        for (const char* p = buf; p < hit && *p; p++) {
            if (*p == '\n') line++;
        }
        char detail[400];
        snprintf(detail, sizeof(detail),
            "Fichier user-facing contient %s ligne %d. Voir CLAUDE.md regle 7.",
            kind, line);
        rep_violation(r, "public_file_forbidden_mention", path, detail);
    }
    free(buf);
}

static void scan_dir_recursive(const char* dirPath, Reporter* r) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive(full, r);
        } else if (S_ISREG(st.st_mode)) {
            /* Cibler .owl et .md uniquement. */
            const char* ext = strrchr(e->d_name, '.');
            if (!ext) continue;
            if (strcmp(ext, ".owl") == 0 || strcmp(ext, ".md") == 0) {
                scan_file_for_mentions(full, r);
            }
        }
    }
    closedir(d);
}

static void check_rule22_public_files_mentions(Reporter* r) {
    /* Roots a scanner : user-facing strict. */
    static const char* ROOTS[] = {
        "examples",
        "tests/scenarios",
        "docs/public_documentation",
        NULL
    };
    for (int i = 0; ROOTS[i]; i++) {
        scan_dir_recursive(ROOTS[i], r);
    }
}

/* ============================================================
 * Regle 24 : METHOD_RECEIVER_INFO[] doit etre dense (pas de trou)
 *
 * Pour chaque MethodReceiverType < METHOD_TYPE_COUNT, METHOD_RECEIVER_INFO
 * doit avoir une entree avec canonical non-NULL. Sinon : trou zero-init
 * provoque par un ajout d'enum sans entree correspondante → receveur
 * "fantome" qui passe l'enum mais n'a pas de nom canonique → method
 * catalog dump LSP plante, registry binding silencieux, etc.
 *
 * Cas typique : un dev ajoute METHOD_TYPE_HTTPREQUETE entre deux valeurs
 * et oublie METHOD_RECEIVER_INFO[METHOD_TYPE_HTTPREQUETE].
 * ============================================================ */
static void check_rule24_method_receiver_info_dense(Reporter* r) {
    for (int t = 0; t < METHOD_TYPE_COUNT; t++) {
        const MethodReceiverInfo* info = method_receiver_info((MethodReceiverType)t);
        if (!info || !info->canonical || info->canonical[0] == '\0') {
            char sym[64];
            snprintf(sym, sizeof(sym), "METHOD_TYPE_#%d", t);
            char detail[400];
            snprintf(detail, sizeof(detail),
                "MethodReceiverType slot %d (sur %d) a METHOD_RECEIVER_INFO "
                "canonical NULL/vide. Trou dans la table — un ajout d'enum "
                "MethodReceiverType n'a pas ete accompagne d'une entree dans "
                "METHOD_RECEIVER_INFO[] (owl_methods.c). Ajouter l'entree avec "
                "designated initializer.",
                t, METHOD_TYPE_COUNT);
            rep_violation(r, "method_receiver_info_missing", sym, detail);
        }
    }
}

/* ============================================================
 * Regle 23 : tout ObjInstance template dans vm.globals doit etre
 * declarable cote user.
 *
 * Pattern OWL pour un type compose builtin = 3 sites synchros :
 *   1. build_X_template() + register_global_instance("X", tpl)
 *   2. Entree dans TYPES[] (owl list/explain/--doc + LSP)
 *   3. Entree dans BUILTIN_TYPE_NAMES[] cote compiler.c (strict check)
 *
 * Si seul (1) est fait, le type est un "fantome" : ses sous-objets sont
 * exposes en fields (FIELDS_X) mais le user ne peut pas declarer un
 * standalone `x est un X` (erreur compile "Type inconnu 'X'"). Cas
 * historique : Coin, Trait, Option avant la regularisation.
 *
 * Cette rule iterise vm.globals, attrape chaque template d'ObjInstance,
 * et verifie que son structName a une OwlTypeDef. Si non : violation +
 * suggestion de migration.
 *
 * Note : la verification BUILTIN_TYPE_NAMES[] cote compiler.c sera
 * eliminee quand on aura unifie la source de verite (phase D du
 * chantier). En attendant, la regle attrape (1) sans (2) — l'invariant
 * (3) sera ajoute le cas echeant.
 * ============================================================ */
static void check_rule23_global_instance_has_typedef(Reporter* r) {
    /*  : les templates de types sont dans vm.types (namespace
     * separe), plus dans vm.globals. */
    Table* t = &vm.types;
    if (!t || !t->entries) return;

    int nt = owl_registry_types_count();
    for (int i = 0; i < t->capacity; i++) {
        Entry* e = &t->entries[i];
        if (!e->key) continue;
        if (!IS_OBJ(e->value)) continue;
        Obj* obj = AS_OBJ(e->value);
        if (obj->type != OBJ_INSTANCE) continue;
        ObjInstance* inst = (ObjInstance*)obj;
        if (!inst->structName || !inst->structName->chars) continue;
        const char* sn = inst->structName->chars;
        int snLen = inst->structName->length;

        /* Cherche une OwlTypeDef qui matche par canonical, canonicalAscii
         * ou aliases (insensible casse+accents au sens registry). */
        bool found = false;
        for (int j = 0; j < nt && !found; j++) {
            const OwlTypeDef* td = owl_registry_type_at(j);
            if (!td) continue;
            if (td->canonical && strlen(td->canonical) == (size_t)snLen
                && strncmp(td->canonical, sn, (size_t)snLen) == 0) {
                found = true;
                break;
            }
            if (td->canonicalAscii && strlen(td->canonicalAscii) == (size_t)snLen
                && strncmp(td->canonicalAscii, sn, (size_t)snLen) == 0) {
                found = true;
                break;
            }
            if (td->aliases) {
                for (const char* const* a = td->aliases; *a; a++) {
                    if (strlen(*a) == (size_t)snLen
                        && strncmp(*a, sn, (size_t)snLen) == 0) {
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "ObjInstance template '%s' enregistre dans vm.types (probablement "
                "via register_global_instance) mais aucune OwlTypeDef dans TYPES[] "
                "ne porte ce structName. Type fantome : `x est un %s` echouera au "
                "compile. Ajouter FIELDS_%s + entree dans TYPES[] + dans "
                "BUILTIN_TYPE_NAMES[] compiler.c.",
                sn, sn, sn);
            rep_violation(r, "global_instance_missing_typedef", sn, detail);
        }
    }
}

/* ============================================================
 * Regle 19 : formatter::method_norms[] non-redondant avec le method
 * catalog dynamique.
 *
 * method_norms[] est une table d'accent-restoration : elle existe pour
 * remapper un nom de methode ASCII (catalog) vers sa forme canonique
 * accentuee (ex: "debut" -> "Début" parce que le catalog enregistre "Debut"
 * ASCII). Si une entree pointe vers une canonique deja presente telle quelle
 * dans le catalog, elle est inutile (le catalog hit en premier dans
 * find_method_norm) et constitue du code mort qui risque de masquer un
 * vrai canon plus tard.
 * ============================================================ */
static void check_rule19_method_norm_redundant(Reporter* r) {
    int nm = owl_formatter_method_norms_count();
    int nc = method_catalog_count();
    for (int i = 0; i < nm; i++) {
        const OwlFormatterMethodNorm* mn = owl_formatter_method_norms_at(i);
        if (!mn || !mn->normalized || !mn->canonical) continue;
        for (int j = 0; j < nc; j++) {
            const char* typeName = NULL;
            const MethodEntry* me = method_catalog_at(j, &typeName);
            if (!me || !me->name) continue;
            /* Le canonical du catalog est-il identique a la forme canon
             * que method_norms restaure ? Si oui : entree redondante. */
            if (strcmp(me->name, mn->canonical) == 0) {
                char detail[400];
                snprintf(detail, sizeof(detail),
                    "Entree method_norms[] {\"%s\" -> \"%s\"} est redondante : "
                    "le method catalog enregistre deja \"%s\" sur le type %s. "
                    "find_method_norm consulte d'abord le catalog (step 1), "
                    "donc cette entree statique est inatteignable. La virer.",
                    mn->normalized, mn->canonical, me->name,
                    typeName ? typeName : "?");
                rep_violation(r, "method_norm_redundant", mn->canonical, detail);
                break;
            }
        }
    }
}

/* ============================================================
 * Regle 26 : coherence type coercible <-> parser enregistre
 * ============================================================
 *
 * Pour chaque OwlTypeDef avec coercible=true, owl_has_instance_coerce
 * sur son nom canon DOIT retourner true (parser enregistre dans
 * stdlib_<module>_init() via owl_register_instance_coerce).
 *
 * Inversement, pour chaque entree de owl_instance_coerce_count, le
 * type ciblé DOIT exister dans le registry ET avoir coercible=true.
 *
 * Garantit l'invariant :
 *   - Type declare coercible mais sans parser -> crash runtime
 *     (compilo emet __InstanceCoerce("MonType", v) mais dispatch
 *     fail au runtime).
 *   - Parser enregistre pour un type inconnu -> dette de doc :
 *     impossible pour `owl explain MonType` de signaler la coerce.
 *   - Parser enregistre pour un type coercible=false -> incoherence :
 *     le parser tournera quand meme mais la doc ne le mentionnera
 *     pas, et `owl list coercibles` (futur) le ratera.
 *
 * Force la decision explicite a la declaration du type. ============================================================ */
static void check_rule26_coercible_has_parser(Reporter* r) {
    int n = owl_registry_types_count();
    for (int i = 0; i < n; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->canonical) continue;
        if (!t->coercible) continue;
        const char* name = t->canonicalAscii ? t->canonicalAscii : t->canonical;
        if (!owl_has_instance_coerce(name)) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Type '%s' declare coercible=true mais aucun parser enregistre. "
                "Symptome : `var est un %s = <expr>` emet __InstanceCoerce(v, "
                "\"%s\") qui crash au runtime. Ajouter owl_register_instance_"
                "coerce(\"%s\", parse_to_xxx) dans stdlib_<module>_init().",
                t->canonical, name, name, name);
            rep_violation(r, "type_coercible_missing_parser",
                          t->canonical, detail);
        }
    }
    /* Inverse : tout parser enregistre doit pointer sur un type coercible. */
    int nc = owl_instance_coerce_count();
    for (int i = 0; i < nc; i++) {
        const char* tname = owl_instance_coerce_name_at(i);
        if (!tname) continue;
        const OwlTypeDef* t = owl_registry_type_find(tname);
        if (!t) {
            char detail[300];
            snprintf(detail, sizeof(detail),
                "Parser de coerce enregistre pour '%s' mais ce type n'existe "
                "pas dans le registry. Declarer le OwlTypeDef correspondant "
                "(stdlib_builtin_types.c::TYPES) avec .coercible=true.", tname);
            rep_violation(r, "coerce_parser_unknown_type", tname, detail);
            continue;
        }
        if (!t->coercible) {
            char detail[300];
            snprintf(detail, sizeof(detail),
                "Parser de coerce enregistre pour '%s' mais le type a "
                "coercible=false. Mettre a true dans son OwlTypeDef pour "
                "que `owl explain %s` documente la coerce.", tname, tname);
            rep_violation(r, "coerce_parser_type_not_coercible",
                          tname, detail);
        }
    }
}

/* ============================================================
 * Regle 25 : tout widget user-declarable a une defaultProperty decidee
 * ============================================================
 * Pour chaque entree CONTROL_KINDS avec is_widget=true :
 *   - soit defaultProperty == NULL (refus explicite assumé : Table /
 *     Interrupteur / ZoneRepetee / Fenetre, widgets composes ou non
 *     scalaires) — le compilo emet une erreur claire au moment du
 *     `WIDGET = X`
 *   - soit defaultProperty == "Valeur" : conformite canon universelle
 *     (alias runtime generique, accepté par tous les widgets scalaires
 *     via stdlib_ui::prop_is_default)
 *   - soit defaultProperty == autre chose : la prop DOIT etre presente
 *     dans le catalog DSL du widget (owl_dsl_prop_find != NULL), sinon
 *     le translate `WIDGET = X` -> `WIDGET..<prop> = X` produira un
 *     bytecode qui crashe au runtime "propriete inconnue"
 *
 * Garantit le pacte universel sur l'evolution future : un dev qui
 * ajoute un widget ne peut pas oublier la decision sur l'affectation
 * directe. Le self-test refuse de compiler la decision implicite.
 * ============================================================ */

static void check_rule25_widget_default_property(Reporter* r) {
    const OwlControlKindInfo* tbl = owl_control_kinds_table();
    for (const OwlControlKindInfo* k = tbl; k->nameFR; k++) {
        if (!k->is_widget) continue;
        /* NULL = refus explicite ok. */
        if (k->defaultProperty == NULL) continue;
        /* "Valeur" = alias runtime universel ok pour la plupart des widgets
         * scalaires (Libelle/Bouton/Saisie : handlers explicites dans
         * stdlib_ui::owl_ui_control_set_prop ; Liste/Combo/Selecteur/
         * Interrupteur : "Valeur" listee dans leur catalog). EXCEPTION :
         * widget Image — voir regle 25b ci-dessous. */
        if (strcasecmp(k->defaultProperty, "Valeur") == 0) continue;
        /* Autre nom : doit etre present dans le catalog DSL du kind.
         * On teste avec nameAscii prioritaire (la convention catalog), puis
         * nameFR en fallback. */
        const char* kindName = k->nameAscii ? k->nameAscii : k->nameFR;
        const OwlControlPropDef* def = owl_dsl_prop_find(kindName, k->defaultProperty);
        if (!def && k->nameAscii && k->nameFR) {
            def = owl_dsl_prop_find(k->nameFR, k->defaultProperty);
        }
        if (!def) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                "Widget '%s' a defaultProperty='%s' mais cette prop n'est pas "
                "exposee dans son PROPS_<KIND>_FIELD catalog (owl_dsl_prop_find "
                "retourne NULL). Symptome : le translate compile-time emet un "
                "OP_STRUCT_SET vers une prop inconnue, qui crashe au runtime. "
                "Soit ajouter '%s' dans PROPS_%s_FIELD (src/ui_catalog.c), "
                "soit utiliser 'Valeur' (alias universel runtime), soit poser "
                "NULL pour refuser l'affectation directe au compile-time.",
                k->nameFR, k->defaultProperty, k->defaultProperty, kindName);
            rep_violation(r, "widget_default_property_unknown",
                          k->nameFR, detail);
        }
    }
}

/* ============================================================
 * Regle 25b : widget Image — defaultProperty doit cibler une vraie prop
 * ============================================================
 *  — Bug historique : le widget Image avait defaultProperty="Valeur",
 * exempte par la regle 25 au titre d'alias runtime universel. Mais
 * contrairement aux autres widgets scalaires (Libelle/Bouton/Saisie qui
 * ont un handler "Valeur" explicite dans stdlib_ui::owl_ui_control_set_prop,
 * et Liste/Combo/Selecteur/Interrupteur qui exposent "Valeur" dans leur
 * catalog), le widget Image n'a NI handler runtime "Valeur" NI prop "Valeur"
 * dans PROPS_IMAGE_FIELD. Resultat : `IMG_X = "/chemin"` etait traduit en
 * `IMG_X..Valeur = "/chemin"` qui populait un champ struct fantome sans
 * jamais toucher au QLabel — piege silencieux. Le seul contournement
 * passait par `Libelle..Image` + overlay.
 *
 * Regle ciblee : pour le widget Image specifiquement, defaultProperty DOIT
 * etre presente dans PROPS_IMAGE_FIELD. Pas de free pass "Valeur" car aucun
 * sink runtime ne l'accepterait. La voie canonique est defaultProperty=
 * "Image" (qui existe dans le catalog + a un handler runtime complet
 * stdlib_ui.c:2641+ chemin/image-memoire/icone).
 * ============================================================ */

static void check_rule25b_image_widget_default_property(Reporter* r) {
    const OwlControlKindInfo* tbl = owl_control_kinds_table();
    for (const OwlControlKindInfo* k = tbl; k->nameFR; k++) {
        if (!k->is_widget) continue;
        /* Cible : widget Image (nameFR "Image"). */
        if (strcmp(k->nameFR, "Image") != 0) continue;
        if (k->defaultProperty == NULL) return; /* NULL = refus, ok. */
        const char* kindName = k->nameAscii ? k->nameAscii : k->nameFR;
        const OwlControlPropDef* def = owl_dsl_prop_find(kindName, k->defaultProperty);
        if (!def) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                "Widget Image a defaultProperty='%s' mais cette prop n'est "
                "pas dans PROPS_IMAGE_FIELD (ui_catalog.c). Aucun handler "
                "runtime ne traite '%s' pour OWL_CONTROL_IMAGE dans "
                "stdlib_ui::owl_ui_control_set_prop : l'affectation directe "
                "`IMG_X = X` populerait un champ struct fantome sans toucher "
                "au QLabel. Voie canonique : defaultProperty=\"Image\" qui "
                "existe deja dans le catalog + a un handler runtime complet "
                "(chemin, image memoire, icone).",
                k->defaultProperty, k->defaultProperty);
            rep_violation(r, "image_widget_default_property_no_sink",
                          k->nameFR, detail);
        }
        return;
    }
}

/* ============================================================
 * Regle 27 : OwlParam.desc mentionnant un enum doit etre cable
 *
 * Heuristique anti-trou-d'autocomplete : si la description d'un param
 * mentionne >= 2 valeurs d'un meme OwlEnum combinable, alors le param
 * DOIT cabler cet enum via .allowedBase ou .allowedOptions. Sinon le
 * LSP propose le catalogue general au lieu des constantes strictes.
 *
 * Le seuil de 2 evite les faux positifs (mention isolee d'une seule
 * constante en exemple). Le matching est strstr() sur la desc UTF-8.
 *
 * Pour le cas legitime "enum OU autre chose" (entier libre + constante),
 * le cablage doit utiliser .acceptsLiterals=true qui suggere l'enum
 * sans court-circuiter le catalogue general.
 * ============================================================ */
static void check_rule27_param_desc_mentions_enum(Reporter* r) {
    int nn = owl_registry_count();
    int ne = owl_registry_enums_count();

    for (int i = 0; i < nn; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->signatures) continue;
        if (d->flags & OWL_NAT_INTERNAL) continue;

        for (const OwlSignature* s = d->signatures; s->returnType; s++) {
            if (!s->params) continue;
            for (const OwlParam* p = s->params; p->name; p++) {
                if (!p->desc) continue;
                /* Param deja cable (sur n'importe quel enum) : on fait
                 * confiance au cablage. La rule sert a detecter les
                 * trous, pas a auditer la justesse du choix d'enum. */
                if (p->allowedBase || p->allowedOptions) continue;

                for (int j = 0; j < ne; j++) {
                    const OwlEnum* e = owl_registry_enum_at(j);
                    if (!e || !e->combinable || !e->values) continue;

                    int count = 0;
                    const char* firstHit = NULL;
                    for (const char* const* v = e->values; *v; v++) {
                        if (strstr(p->desc, *v) != NULL) {
                            if (count == 0) firstHit = *v;
                            count++;
                        }
                    }
                    if (count < 2) continue;

                    char detail[600];
                    snprintf(detail, sizeof(detail),
                        "Param '%s' de '%s' mentionne %d valeurs de l'enum "
                        "combinable '%s' (dont '%s') dans sa description, "
                        "sans cabler .allowedOptions=&%s. Resultat : "
                        "autocomplete LSP pollue par le catalogue general. "
                        "Ajouter .allowedOptions=&%s a la declaration OwlParam "
                        "(ou .acceptsLiterals=true si le param accepte aussi "
                        "des entiers libres).",
                        p->name, d->canonical, count,
                        e->name ? e->name : "?",
                        firstHit, e->name ? e->name : "?",
                        e->name ? e->name : "?");
                    rep_violation(r, "param_desc_mentions_enum_unwired",
                                  d->canonical, detail);
                    /* Un seul enum signale par param suffit, evite le bruit
                     * si la desc mentionne plusieurs enums. */
                    goto next_param;
                }
                next_param: ;
            }
        }
    }
}

static void check_rule17_dsl_prop_ascii(Reporter* r) {
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->def->nameFR) continue;
        if (!has_non_ascii(e->def->nameFR)) continue;  /* ASCII pur : OK */
        if (str_empty(e->def->nameAscii)) {
            char sym[160];
            snprintf(sym, sizeof(sym), "%s.%s", e->kind, e->def->nameFR);
            char detail[320];
            snprintf(detail, sizeof(detail),
                "Propriete DSL '%s' a nameFR accentue mais nameAscii NULL/vide. "
                "Le scanner strip les accents au compile time : sans nameAscii, "
                "le lookup catalog rate quand l'utilisateur tape la forme sans "
                "accent. Ajouter la version ASCII en 2eme position de l'entree.",
                e->def->nameFR);
            rep_violation(r, "dsl_prop_ascii_missing", sym, detail);
        }
    }
}

/* ============================================================
 * Rules 28-31 — Catalogue keyword (couche pédagogique sur les
 * mots-clés du langage). Source de vérité unique : table KEYWORDS[]
 * du scanner. Le catalogue ajoute des fiches par TokenType ; ces
 * rules garantissent qu'on ne peut pas désynchroniser.
 * ============================================================ */

/* True si ce TokenType apparaît dans la table KEYWORDS[] du scanner
 * (au moins un alias enregistré). Utilisé pour valider qu'un
 * OwlKeywordDef.token n'est pas une chimère. */
static bool token_is_keyword_in_scanner(TokenType t) {
    for (int i = 0; ; i++) {
        TokenType type;
        if (!scanner_keyword_at(i, &type)) return false;
        if (type == t) return true;
    }
}

/* Rule 28 — couverture montante : tout TokenType user-facing doit avoir
 * une OwlKeywordDef. Strict absolu : aucune allow-list. */
static void check_rule28_keyword_coverage(Reporter* r) {
    /* Iterate over user-facing TokenTypes via the scanner table — on
     * recolte les types uniques, puis on vérifie. */
    TokenType seen[256];
    int n_seen = 0;
    for (int i = 0; ; i++) {
        TokenType type;
        if (!scanner_keyword_at(i, &type)) break;
        int dup = 0;
        for (int k = 0; k < n_seen; k++) if (seen[k] == type) { dup = 1; break; }
        if (!dup && n_seen < 256) seen[n_seen++] = type;
    }
    for (int i = 0; i < n_seen; i++) {
        TokenType t = seen[i];
        if (!is_keyword_user_facing(t)) continue;
        if (owl_keyword_def_for(t) == NULL) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "TokenType %s user-facing sans OwlKeywordDef. Rédige la "
                "fiche dans owl_keyword_catalog.c (ou retire-le du predicate "
                "is_keyword_user_facing).", token_type_name(t));
            rep_violation(r, "keyword_doc_missing", token_type_name(t), detail);
        }
    }
}

/* Rule 29 — couverture descendante : tout OwlKeywordDef.token doit
 * exister dans le scanner ET être déclaré user-facing. */
static void check_rule29_keyword_def_valid(Reporter* r) {
    int n = owl_keyword_count();
    for (int i = 0; i < n; i++) {
        const OwlKeywordDef* d = owl_keyword_at(i);
        if (!d) continue;
        if (!token_is_keyword_in_scanner(d->token)) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "OwlKeywordDef référence TokenType %s mais le scanner "
                "ne le connaît pas. Vérifie scanner.c::KEYWORDS[].",
                token_type_name(d->token));
            rep_violation(r, "keyword_def_unknown_token",
                          token_type_name(d->token), detail);
        }
        if (!is_keyword_user_facing(d->token)) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "OwlKeywordDef pour TokenType %s qui n'est PAS dans "
                "is_keyword_user_facing. Ajoute-le au predicate ou retire "
                "la fiche.", token_type_name(d->token));
            rep_violation(r, "keyword_def_not_user_facing",
                          token_type_name(d->token), detail);
        }
    }
}

/* Rule 30 — sens non vides : pour chaque OwlKeywordSense, tous les
 * champs (label_fr/en, syntax, descFR/EN, example) doivent être
 * renseignés non-vides. */
static void check_rule30_keyword_sense_complete(Reporter* r) {
    int n = owl_keyword_count();
    for (int i = 0; i < n; i++) {
        const OwlKeywordDef* d = owl_keyword_at(i);
        if (!d || !d->senses) continue;
        for (int k = 0; k < d->senses_count; k++) {
            const OwlKeywordSense* s = &d->senses[k];
            const char* field = NULL;
            if (!s->label_fr || !*s->label_fr) field = "label_fr";
            else if (!s->label_en || !*s->label_en) field = "label_en";
            else if (!s->syntax   || !*s->syntax)   field = "syntax";
            else if (!s->descFR   || !*s->descFR)   field = "descFR";
            else if (!s->descEN   || !*s->descEN)   field = "descEN";
            else if (!s->example  || !*s->example)  field = "example";
            if (field) {
                char detail[256];
                snprintf(detail, sizeof detail,
                    "KeywordSense %d/%d pour TokenType %s a le champ '%s' "
                    "vide.", k + 1, d->senses_count,
                    token_type_name(d->token), field);
                rep_violation(r, "keyword_sense_incomplete",
                              token_type_name(d->token), detail);
            }
        }
    }
}

/* Rule 31 — composites valides : t1 et t2 doivent exister dans le
 * scanner ; le sense doit être non-NULL et complet. */
static void check_rule31_keyword_composite_valid(Reporter* r) {
    int n = owl_keyword_composite_count();
    for (int i = 0; i < n; i++) {
        const OwlKeywordComposite* c = owl_keyword_composite_at(i);
        if (!c) continue;
        const char* sym_name = token_type_name(c->t1);
        if (!token_is_keyword_in_scanner(c->t1)) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "Composite référence t1=%s mais le scanner ne le connaît pas.",
                token_type_name(c->t1));
            rep_violation(r, "keyword_composite_unknown_t1", sym_name, detail);
        }
        if (!token_is_keyword_in_scanner(c->t2)) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "Composite référence t2=%s mais le scanner ne le connaît pas.",
                token_type_name(c->t2));
            rep_violation(r, "keyword_composite_unknown_t2", sym_name, detail);
        }
        if (!c->sense) {
            rep_violation(r, "keyword_composite_no_sense", sym_name,
                          "Composite sans sense — fiche absente.");
            continue;
        }
        const OwlKeywordSense* s = c->sense;
        const char* field = NULL;
        if (!s->label_fr || !*s->label_fr) field = "label_fr";
        else if (!s->label_en || !*s->label_en) field = "label_en";
        else if (!s->syntax   || !*s->syntax)   field = "syntax";
        else if (!s->descFR   || !*s->descFR)   field = "descFR";
        else if (!s->descEN   || !*s->descEN)   field = "descEN";
        else if (!s->example  || !*s->example)  field = "example";
        if (field) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "Composite (%s, %s) — sense incomplet, champ '%s' vide.",
                token_type_name(c->t1), token_type_name(c->t2), field);
            rep_violation(r, "keyword_composite_incomplete", sym_name, detail);
        }
    }
}

/* ============================================================
 * Rules 32-35 — Catalogue opérateur (couche pédagogique sur les
 * opérateurs symboliques du langage : +, [~~, ??*, etc.). Source
 * d'identité : TokenType + display_form. Rules parallèles à 28-31.
 * ============================================================ */

/* Rule 32 — couverture montante : tout TokenType d'opérateur user-facing
 * doit avoir une OwlOperatorDef. Strict absolu : aucune allow-list. */
static void check_rule32_operator_coverage(Reporter* r) {
    /* On itère tous les TokenType possibles en partant d'un range
     * raisonnable (TOKEN_EOF=0 jusqu'à un upper bound). Les valeurs
     * d'enum TokenType sont contiguës. On utilise un cap large pour
     * couvrir l'ensemble. */
    for (int t = 0; t < 512; t++) {
        TokenType tt = (TokenType)t;
        if (!is_operator_user_facing(tt)) continue;
        if (owl_operator_def_for(tt) == NULL) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "TokenType %s opérateur user-facing sans OwlOperatorDef. "
                "Rédige la fiche dans owl_operator_catalog.c (ou retire-le "
                "du predicate is_operator_user_facing).", token_type_name(tt));
            rep_violation(r, "operator_doc_missing", token_type_name(tt), detail);
        }
    }
}

/* Rule 33 — couverture descendante : toute OwlOperatorDef.token doit
 * être user-facing, et display_form non-NULL/non-vide. */
static void check_rule33_operator_def_valid(Reporter* r) {
    int n = owl_operator_count();
    for (int i = 0; i < n; i++) {
        const OwlOperatorDef* d = owl_operator_at(i);
        if (!d) continue;
        const char* sym = token_type_name(d->token);
        if (!is_operator_user_facing(d->token)) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "OwlOperatorDef pour TokenType %s qui n'est PAS dans "
                "is_operator_user_facing. Ajoute-le au predicate ou retire "
                "la fiche.", sym);
            rep_violation(r, "operator_def_not_user_facing", sym, detail);
        }
        if (!d->display_form || !*d->display_form) {
            rep_violation(r, "operator_display_form_missing", sym,
                "OwlOperatorDef sans display_form (chaîne brute attendue : "
                "\"+\", \"[~~\", etc.).");
        }
        if (!d->category || !*d->category) {
            rep_violation(r, "operator_category_missing", sym,
                "OwlOperatorDef sans category.");
        }
    }
}

/* Rule 34 — sens non vides : chaque OwlKeywordSense de l'opérateur a
 * tous ses champs renseignés. */
static void check_rule34_operator_sense_complete(Reporter* r) {
    int n = owl_operator_count();
    for (int i = 0; i < n; i++) {
        const OwlOperatorDef* d = owl_operator_at(i);
        if (!d || !d->senses) continue;
        const char* sym = d->display_form ? d->display_form
                                           : token_type_name(d->token);
        for (int k = 0; k < d->senses_count; k++) {
            const OwlKeywordSense* s = &d->senses[k];
            const char* field = NULL;
            if (!s->label_fr || !*s->label_fr) field = "label_fr";
            else if (!s->label_en || !*s->label_en) field = "label_en";
            else if (!s->syntax   || !*s->syntax)   field = "syntax";
            else if (!s->descFR   || !*s->descFR)   field = "descFR";
            else if (!s->descEN   || !*s->descEN)   field = "descEN";
            else if (!s->example  || !*s->example)  field = "example";
            if (field) {
                char detail[256];
                snprintf(detail, sizeof detail,
                    "KeywordSense %d/%d pour opérateur %s a le champ '%s' "
                    "vide.", k + 1, d->senses_count, sym, field);
                rep_violation(r, "operator_sense_incomplete", sym, detail);
            }
        }
    }
}

/* Rule 35 — pas de doublon display_form. */
/* Rule 37 — squelettes `owl new` parseables.
 *
 * Pour chaque variant (kind, sub) supporté par owl_new_generate, génère
 * le squelette dans un buffer, wrap si c'est un champ (les widgets
 * doivent vivre dans une Fenêtre), et invoque le compiler. Échoue si
 * le compile retourne des erreurs — protège contre la dérive du DSL :
 * si le canon évolue (PROPRIETES devient obligatoire, btnNormal renommé,
 * etc.), tous les squelettes désynchronisés font hurler le build. */
/* `owl_new_generate_ex` est exposé non-static par main.c ; on le forward-
 * déclare ici (owl_new.h conserve la signature historique 4-args pour le
 * tool MCP). disp : 0..3 ; en : 0=FR 1=EN. */
extern int owl_new_generate_ex(const char* kind, const char* sub,
                               const char* name, int disp, int fmt, int orient,
                               int en, FILE* out);

/* Génère une variante dans une chaîne NUL-terminée : surface exacte émise
 * par `owl new` (même point d'entrée). Retourne le code de
 * owl_new_generate_ex (0 = variante connue), -2 sur échec d'allocation.
 * *out alloué au succès (caller free). */
static int rule37_generate_string(const char* kind, const char* sub,
                                  const char* name, int disp, int fmt,
                                  int orient, int en, char** out) {
    *out = NULL;
    FILE* f = tmpfile();
    if (!f) return -2;
    int rc = owl_new_generate_ex(kind, sub, name, disp, fmt, orient, en, f);
    if (rc != 0) { fclose(f); return rc; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -2; }
    rewind(f);
    char* buf = (char*)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -2; }
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    *out = buf;
    return 0;
}

/* Numéro (1-based) de la première ligne où `a` et `b` divergent, et copie
 * tronquée des deux lignes fautives (whitespace rendu visible : tab -> "\t",
 * espace de tête -> '.'). 0 si les chaînes sont identiques. */
static int owl_gen_first_diff_line(const char* a, const char* b,
                                   char* la, size_t la_sz,
                                   char* lb, size_t lb_sz) {
    int line = 1;
    const char* pa = a; const char* pb = b;
    while (*pa && *pb && *pa == *pb) {
        if (*pa == '\n') line++;
        pa++; pb++;
    }
    if (*pa == *pb) return 0;  /* identiques */
    /* Recule au début de la ligne courante dans chaque chaîne. */
    const char* sa = pa; while (sa > a && sa[-1] != '\n') sa--;
    const char* sb = pb; while (sb > b && sb[-1] != '\n') sb--;
    const char* ea = strchr(sa, '\n'); if (!ea) ea = sa + strlen(sa);
    const char* eb = strchr(sb, '\n'); if (!eb) eb = sb + strlen(sb);
    struct { const char* s; const char* e; char* dst; size_t sz; } sides[2] = {
        { sa, ea, la, la_sz }, { sb, eb, lb, lb_sz }
    };
    for (int k = 0; k < 2; k++) {
        size_t o = 0; int leading = 1;
        for (const char* p = sides[k].s; p < sides[k].e && o + 3 < sides[k].sz; p++) {
            if (*p == '\t')      { sides[k].dst[o++] = '\\'; sides[k].dst[o++] = 't'; }
            else if (*p == ' ' && leading) { sides[k].dst[o++] = '.'; }
            else { if (*p != ' ') leading = 0; sides[k].dst[o++] = *p; }
        }
        sides[k].dst[o] = '\0';
    }
    return line;
}

/* Contenu généré vs son propre formatage : `owl fmt` doit être un no-op.
 * Sinon l'utilisateur voit son fichier modifié au premier save.
 * `family` préfixe le code de violation (owl_new / template). */
static void check_generated_is_canonical(Reporter* r, const char* family,
                                         const char* tag, const char* src) {
    char* canon = owl_format(src);
    if (!canon) {
        char code[64], detail[300];
        snprintf(code, sizeof code, "%s_fmt_failed", family);
        snprintf(detail, sizeof detail,
            "Le formatter refuse le contenu généré pour `%s` : owl_format a "
            "renvoyé NULL.", tag);
        rep_violation(r, code, tag, detail);
        return;
    }
    if (strcmp(canon, src) != 0) {
        char la[120], lb[120], code[64], detail[600];
        int line = owl_gen_first_diff_line(src, canon, la, sizeof la,
                                           lb, sizeof lb);
        snprintf(code, sizeof code, "%s_not_canonical", family);
        snprintf(detail, sizeof detail,
            "Contenu généré pour `%s` non canonique : `owl fmt` le modifie "
            "(ligne %d)\n            généré  : \"%s\"\n"
            "            formaté : \"%s\"\n"
            "        Le contenu généré doit passer par owl_format avant "
            "émission.", tag, line, la, lb);
        rep_violation(r, code, tag, detail);
    }
    free(canon);
}

/* Génère une variante (kind, sub, disp, en), vérifie qu'elle est canonique
 * (idempotence formatter), la compile, et signale une violation si rc != 0,
 * si le compile échoue, ou si `expect` (forme EN attendue, optionnel) est
 * absente du squelette. */
static void rule37_check_one_ex(Reporter* r, int idx,
                                const char* kind, const char* sub,
                                int disp, int fmt, int orient, int en,
                                const char* expect) {
    char fen_name[64], widget_name[64], menu_name[64];
    snprintf(fen_name,    sizeof fen_name,    "FEN_R37_%d", idx);
    snprintf(widget_name, sizeof widget_name, "W_R37_%d",  idx);
    snprintf(menu_name,   sizeof menu_name,   "MNU_R37_%d", idx);

    char tag[80];
    if (sub) snprintf(tag, sizeof tag, "%s/%s%s", kind, sub, en ? " (en)" : "");
    else     snprintf(tag, sizeof tag, "%s%s",    kind,      en ? " (en)" : "");

    char* gen = NULL;
    int rc = rule37_generate_string(kind, sub, widget_name, disp, fmt, orient,
                                    en, &gen);
    if (rc == -2) return;  /* échec d'allocation : rien à conclure */
    if (rc != 0) {
        char detail[300];
        snprintf(detail, sizeof detail,
            "owl_new_generate_ex a renvoyé -1 pour `%s` : variante non gérée.",
            tag);
        rep_violation(r, "owl_new_skeleton_unknown", tag, detail);
        return;
    }
    if (!gen || !*gen) { free(gen); return; }

    /* Pour la langue EN, vérifie qu'un marqueur EN canon est présent : sans
     * ça un squelette FR (qui compile aussi) masquerait un --lang en non
     * implémenté. */
    if (expect && !strstr(gen, expect)) {
        char detail[300];
        snprintf(detail, sizeof detail,
            "Squelette `%s` ne contient pas le marqueur attendu \"%s\".",
            tag, expect);
        rep_violation(r, "owl_new_skeleton_lang", tag, detail);
        free(gen);
        return;
    }

    /* Canonicité : le squelette est ce que l'utilisateur reçoit tel quel
     * (`owl new fenetre X > X.owl`), il doit survivre à un save éditeur. */
    check_generated_is_canonical(r, "owl_new_skeleton", tag, gen);

    /* Contexte de compilation requis par le kind :
     *   1 (Fenêtre)  : champ + menu racine (MenuPrincipal / MenuContextuel).
     *   2 (Menu)     : élément de menu (OptionMenu / SousMenu / MenuSéparateur),
     *                  qui doit vivre dans un MenuPrincipal, lui-même dans une
     *                  Fenêtre.
     *   0 (aucun)    : bloc top-level autonome (fenetre, classe, ...). */
    int wrap = 0;
    if (strcmp(kind, "champ") == 0 ||
        strcmp(kind, "menuprincipal")  == 0 ||
        strcmp(kind, "menucontextuel") == 0 ||
        strcmp(kind, "menuloupe")      == 0 ||
        strcmp(kind, "menucorps")      == 0) wrap = 1;
    else if (strcmp(kind, "optionmenu")     == 0 ||
             strcmp(kind, "sousmenu")       == 0 ||
             strcmp(kind, "menuseparateur") == 0) wrap = 2;

    char prefix[256]; prefix[0] = '\0';
    char suffix[64];  suffix[0] = '\0';
    if (wrap >= 1) {
        /* Fenêtre englobante ; le wrapper suit la langue testée pour rester
         * un fichier homogène. */
        if (en)
            snprintf(prefix, sizeof prefix,
                "%s is a Window\n    PROPERTIES\n        Title = \"T\"\n"
                "        Width = 200\n        Height = 100\n    END\n\n",
                fen_name);
        else
            snprintf(prefix, sizeof prefix,
                "%s est une Fen\xc3\xaatre\n    PROPRIETES\n        Titre = \"T\"\n"
                "        Largeur = 200\n        Hauteur = 100\n    FIN\n\n",
                fen_name);
    }
    if (wrap == 2) {
        /* Menu porteur pour un élément de menu isolé. */
        size_t used = strlen(prefix);
        snprintf(prefix + used, sizeof prefix - used,
                 en ? "    %s is a MainMenu\n" : "    %s est un MenuPrincipal\n",
                 menu_name);
        snprintf(suffix, sizeof suffix, en ? "    END\nEND\n" : "    FIN\nFIN\n");
    } else if (wrap == 1) {
        snprintf(suffix, sizeof suffix, en ? "END\n" : "FIN\n");
    }

    size_t need = strlen(prefix) + strlen(gen) + strlen(suffix) + 1;
    char* src = (char*)malloc(need);
    if (!src) { free(gen); return; }
    snprintf(src, need, "%s%s%s", prefix, gen, suffix);
    free(gen);

    CompileDiagnostic errs[16];
    int n_errs = 0;
    compile_set_error_capture(errs, 16, &n_errs);
    CompileDiagnostic warns[16];
    int n_warns = 0;
    compile_set_warning_capture(warns, 16, &n_warns);

    ObjFunction* fn = compile(src);
    (void)fn;

    compile_set_error_capture(NULL, 0, NULL);
    compile_set_warning_capture(NULL, 0, NULL);

    if (n_errs > 0) {
        char detail[400];
        snprintf(detail, sizeof detail,
            "Squelette `owl new %s` ne compile pas : %s (ligne %d). "
            "Mets à jour owl_new_generate_ex dans main.c pour respecter "
            "le DSL courant.",
            tag, errs[0].message[0] ? errs[0].message : "?", errs[0].line);
        rep_violation(r, "owl_new_skeleton_invalid", tag, detail);
    }

    free(src);
}

/* Wrapper historique : format/orientation par defaut (A4/portrait), compile
 * toujours. Les call sites existants (fenetre/classe/champ) passent par ici. */
static void rule37_check_one(Reporter* r, int idx,
                             const char* kind, const char* sub,
                             int disp, int en, const char* expect) {
    rule37_check_one_ex(r, idx, kind, sub, disp, 0, 0, en, expect);
}

static void check_rule37_new_skeleton_compiles(Reporter* r) {
    int idx = 0;

    /* Toutes les variantes (kind, sub) en FR puis en EN. La forme EN
     * attendue est le marqueur de type canon du registry.
     *
     * `etat` n'a pas de variante EN générique dans cette boucle : son axe
     * EN (orientation/format) est couvert plus bas par des appels dédiés
     * qui compile-checkent la sortie EN complète (sous-blocs bilingues). */
    for (const OwlNewVariant* v = OWL_NEW_VARIANTS; v->kind; v++) {
        bool etat = (strcmp(v->kind, "etat") == 0);
        const char* expect_en = NULL;
        if      (strcmp(v->kind, "fenetre") == 0) expect_en = "is a Window";
        else if (strcmp(v->kind, "classe")  == 0) expect_en = "is a Class";
        else if (strcmp(v->kind, "collection") == 0) expect_en = "is a boolean";
        else if (strcmp(v->kind, "theme") == 0)   expect_en = "is a Theme";
        else if (strcmp(v->kind, "service") == 0)  expect_en = "is a ServiceREST";
        else if (strcmp(v->kind, "menuprincipal")  == 0) expect_en = "is a MainMenu";
        else if (strcmp(v->kind, "menucontextuel") == 0) expect_en = "is a ContextMenu";
        else if (strcmp(v->kind, "menuloupe")      == 0) expect_en = "is a ContextMenu";
        else if (strcmp(v->kind, "menucorps")      == 0) expect_en = "is a ContextMenu";
        else if (strcmp(v->kind, "optionmenu")     == 0) expect_en = "is a MenuOption";
        else if (strcmp(v->kind, "sousmenu")       == 0) expect_en = "is a Submenu";
        else if (strcmp(v->kind, "menuseparateur") == 0) expect_en = "is a MenuSeparator";
        else if (!etat) /* champ */               expect_en = "is a Control";

        rule37_check_one(r, idx++, v->kind, v->sub, 0, /*en*/0, NULL);
        if (!etat)
            rule37_check_one(r, idx++, v->kind, v->sub, 0, /*en*/1, expect_en);
    }

    /* Le squelette `menuprincipal` reflète une vraie barre de menu : une
     * entrée de 1er niveau est un SousMenu déroulant (contenant les
     * OptionMenu), pas un OptionMenu direct. On vérifie la présence du
     * marqueur SousMenu/Submenu en plus du round-trip compile déjà couvert
     * par la boucle. `menucontextuel` reste une liste plate (OptionMenu
     * direct) : pas de SousMenu attendu. */
    rule37_check_one(r, idx++, "menuprincipal", NULL, 0, /*en*/0, "est un SousMenu");
    rule37_check_one(r, idx++, "menuprincipal", NULL, 0, /*en*/1, "is a Submenu");

    /* Fenêtre × chaque disposition (FR), le squelette doit porter la
     * constante émise correspondante. */
    static const struct { int disp; const char* konst; } DISPS[] = {
        {0, "dispVerticale"}, {1, "dispHorizontale"},
        {2, "dispGrille"},    {3, "dispLibre"},
    };
    for (size_t d = 0; d < sizeof DISPS / sizeof DISPS[0]; d++) {
        rule37_check_one(r, idx++, "fenetre", NULL, DISPS[d].disp, 0, NULL);
        /* EN : la constante doit aussi basculer (dispVertical, dispGrid, ...). */
        rule37_check_one(r, idx++, "fenetre", NULL, DISPS[d].disp, 1, "is a Window");
    }

    /* État × format de page (FR) : le squelette doit porter la constante
     * `etatFormat` correspondante et rester compilable. fmt : 0..4. */
    static const struct { int fmt; const char* konst; } FMTS[] = {
        {0, "formatA4"}, {1, "formatA3"}, {2, "formatA5"},
        {3, "formatLetter"}, {4, "formatLegal"},
    };
    for (size_t k = 0; k < sizeof FMTS / sizeof FMTS[0]; k++) {
        rule37_check_one_ex(r, idx++, "etat", NULL, 0, FMTS[k].fmt, 0,
                            /*en*/0, FMTS[k].konst);
    }

    /* État × orientation (FR) : `etatOrientation` émis + compilable. */
    static const struct { int orient; const char* konst; } ORIENTS[] = {
        {0, "orientPortrait"}, {1, "orientPaysage"},
    };
    for (size_t o = 0; o < sizeof ORIENTS / sizeof ORIENTS[0]; o++) {
        rule37_check_one_ex(r, idx++, "etat", NULL, 0, 0, ORIENTS[o].orient,
                            /*en*/0, ORIENTS[o].konst);
    }

    /* État --lang en : la forme EN canon de l'orientation doit basculer
     * (orientPaysage -> orientLandscape). Format inchangé en EN (formatA*
     * identiques). Compile-check actif : les sous-blocs etat EN
     * (Body/PageHeader/...) ont un parseur bilingue, donc la sortie EN
     * doit compiler comme les variantes FR. */
    rule37_check_one_ex(r, idx++, "etat", NULL, 0, 0, 1,
                        /*en*/1, "orientLandscape");
    rule37_check_one_ex(r, idx++, "etat", NULL, 0, 0, 0,
                        /*en*/1, "orientPortrait");
    rule37_check_one_ex(r, idx++, "etat", NULL, 0, 1, 0,
                        /*en*/1, "formatA3");
}

/* Rule 38 — templates embarqués (`owl template <X>`) parseables.
 *
 * Itère EMBEDDED_TEMPLATES (les .owl de docs/public_templates/ embarqués
 * au build) et compile chacun. Même contrat que rule 37 mais pour les
 * exemples métier publiés à l'utilisateur — un template qui ne parse
 * plus est un crash pour quiconque suit la doc. */
static void check_rule38_template_compiles(Reporter* r) {
    for (int i = 0; i < EMBEDDED_TEMPLATES_COUNT; i++) {
        const OwlEmbeddedDoc* t = &EMBEDDED_TEMPLATES[i];
        if (!t || !t->data || t->len <= 0) continue;

        /* data n'est pas zero-terminé — duplique en buffer NUL-terminé. */
        char* src = (char*)malloc((size_t)t->len + 1);
        if (!src) continue;
        memcpy(src, t->data, (size_t)t->len);
        src[t->len] = '\0';

        CompileDiagnostic errs[16];
        int n_errs = 0;
        compile_set_error_capture(errs, 16, &n_errs);
        CompileDiagnostic warns[16];
        int n_warns = 0;
        compile_set_warning_capture(warns, 16, &n_warns);

        /*  — passer par compile_in_project avec un faux path
         * `docs/public_templates/<name>.owl` pour ancrer la resolution
         * de projet (frontiere = dossier du synthetic). Le verrou DSL
         * strict s'applique aussi aux templates (canon pur, plus
         * d'exemption path-based depuis ). */
        char synthetic_path[256];
        snprintf(synthetic_path, sizeof synthetic_path,
                 "docs/public_templates/%s.owl", t->name);
        ObjFunction* fn = compile_in_project(src, synthetic_path, NULL, 0);
        (void)fn;

        compile_set_error_capture(NULL, 0, NULL);
        compile_set_warning_capture(NULL, 0, NULL);

        if (n_errs > 0) {
            char detail[400];
            snprintf(detail, sizeof detail,
                "Template `owl template %s` ne compile pas : %s (ligne %d). "
                "Mets à jour docs/public_templates/%s.owl ou le DSL.",
                t->name,
                errs[0].message[0] ? errs[0].message : "?",
                errs[0].line, t->name);
            rep_violation(r, "template_invalid", t->name, detail);
        }
        free(src);

        /* Canonicité du corps ÉMIS (header meta strippé) : `owl template X -o
         * f.owl` doit produire un fichier que le formatter laisse intact.
         * Même contrat que les squelettes `owl new`. */
        char* body = owl_template_emitted_body(t);
        if (body) {
            check_generated_is_canonical(r, "template", t->name, body);
            free(body);
        }
    }
}

/* Rule 36 — catalogue des variables globales prédéfinies (globalvar) :
 * cohérence stricte. Chaque OwlGlobalVarDef doit avoir canonical/nameAscii/
 * nameEN/type non vides et descFR/descEN obligatoires (>= 10 chars). */
static void check_rule36_globalvar_catalog(Reporter* r) {
    int n = owl_globalvar_catalog_count();
    for (int i = 0; i < n; i++) {
        const OwlGlobalVarDef* g = owl_globalvar_catalog_at(i);
        const char* tag = g && g->canonical ? g->canonical : "?";
        if (!g || !g->canonical || !*g->canonical) {
            rep_violation(r, "globalvar_canonical_missing", tag,
                "OwlGlobalVarDef sans canonical.");
            continue;
        }
        if (!g->nameAscii || !*g->nameAscii) {
            rep_violation(r, "globalvar_name_ascii_missing", tag,
                "OwlGlobalVarDef sans nameAscii (alias sans accent).");
        }
        if (!g->nameEN || !*g->nameEN) {
            rep_violation(r, "globalvar_name_en_missing", tag,
                "OwlGlobalVarDef sans nameEN (canon EN).");
        }
        if (!g->type || !*g->type) {
            rep_violation(r, "globalvar_type_missing", tag,
                "OwlGlobalVarDef sans type.");
        }
        if (!g->descFR || strlen(g->descFR) < 10) {
            rep_violation(r, "globalvar_desc_fr_missing", tag,
                "OwlGlobalVarDef sans descFR substantielle (>= 10 chars).");
        }
        if (!g->descEN || strlen(g->descEN) < 10) {
            rep_violation(r, "globalvar_desc_en_missing", tag,
                "OwlGlobalVarDef sans descEN substantielle (>= 10 chars).");
        }
    }
}

static void check_rule35_operator_display_unique(Reporter* r) {
    int n = owl_operator_count();
    for (int i = 0; i < n; i++) {
        const OwlOperatorDef* a = owl_operator_at(i);
        if (!a || !a->display_form) continue;
        for (int j = i + 1; j < n; j++) {
            const OwlOperatorDef* b = owl_operator_at(j);
            if (!b || !b->display_form) continue;
            if (strcmp(a->display_form, b->display_form) == 0) {
                char detail[256];
                snprintf(detail, sizeof detail,
                    "display_form '%s' présent deux fois (TokenType %s et %s). "
                    "Le lookup owl_operator_find_by_symbol deviendrait non "
                    "déterministe.",
                    a->display_form, token_type_name(a->token),
                    token_type_name(b->token));
                rep_violation(r, "operator_display_duplicate",
                              a->display_form, detail);
            }
        }
    }
}

/* ============================================================
 * Regle 39 : convention de nommage des fichiers tests/scenarios/
 *
 * Tout fichier regulier present dans tests/scenarios/ (recursif) doit
 * etre un .owl dont le basename commence par "test_". Sinon violation.
 *
 * Exception : les fichiers compagnons d'un scenario multifile (classe
 * cFoo.owl, interface iFoo.owl, enum eFoo.owl, struct sFoo.owl) sont
 * autorises a cote d'un test_*.owl dans le meme sous-dossier. La
 * convention OWL 1 classe = 1 fichier (1c1f) impose que le fichier
 * porte le nom de la classe — on ne peut pas le prefixer 'test_'.
 *
 * Garantit qu'on a un seul point d'entree net pour la suite : un
 * directory propre, des noms predictibles. Anti-pattern vise : fichiers
 * `selon_*.owl`, README.md, dump.json, brouillon.owl qui trainent.
 * ============================================================ */

/* Detecte si un test_*.owl existe dans dirPath (non recursif). */
static int dir_has_test_file(const char* dirPath) {
    DIR* d = opendir(dirPath);
    if (!d) return 0;
    struct dirent* e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 5 && strncmp(e->d_name, "test_", 5) == 0 &&
            n > 4 && strcmp(e->d_name + n - 4, ".owl") == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Verifie qu'un nom de fichier matche le pattern compagnon OWL :
 * prefixe c/i/e/s suivi d'une majuscule + ".owl" (cFoo.owl, iBar.owl…). */
static int is_owl_companion_filename(const char* name) {
    size_t n = strlen(name);
    if (n < 6) return 0; /* min : "cF.owl" = 6 chars */
    if (strcmp(name + n - 4, ".owl") != 0) return 0;
    char p = name[0];
    if (p != 'c' && p != 'i' && p != 'e' && p != 's') return 0;
    return (name[1] >= 'A' && name[1] <= 'Z');
}

/* Avance sur les blancs ASCII (espace, tab). */
static const char* skip_ws_ascii(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Detecte un fichier compagnon de type PascalCase : un .owl (non test_*)
 * dont le basename est NomDeType et qui declare ce type a l'interieur,
 * sous la forme "<Basename> est une <Enumeration|Structure|Interface|Classe>".
 *
 * Sert les scenarios multifile ou le nom de fichier DOIT etre le nom du
 * type (collision filename/type voulue par le test). Reconnait les formes
 * FR et EN canon. Un .owl non-test_* qui ne declare PAS un type homonyme
 * reste flagge par la regle 39 — on n'ouvre pas une exemption generale.
 *
 * name : basename brut (avec ".owl"). full : chemin complet pour lecture. */
static int is_owl_type_companion_file(const char* name, const char* full) {
    size_t n = strlen(name);
    if (n < 6) return 0;                            /* min : "Ab.owl" */
    if (strcmp(name + n - 4, ".owl") != 0) return 0;
    if (!(name[0] >= 'A' && name[0] <= 'Z')) return 0; /* PascalCase */

    size_t base_len = n - 4;                        /* sans ".owl" */
    char base[256];
    if (base_len >= sizeof(base)) return 0;
    memcpy(base, name, base_len);
    base[base_len] = '\0';

    FILE* f = fopen(full, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return 0; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = '\0';
    fclose(f);

    /* Mots-cles de type, formes canon FR + EN. Comparaison sensible a la
     * casse : on cible la forme canon produite par le formatter. */
    static const char* type_kw[] = {
        "\xc3\x89num" "\xc3\xa9" "ration",  /* Énumération (FR) */
        "Enumeration",                       /* Enumeration (EN) */
        "Structure", "Interface", "Classe", "Class", NULL
    };

    int matched = 0;
    for (const char* line = buf; *line && !matched; ) {
        const char* p = skip_ws_ascii(line);
        /* basename en debut de ligne ? */
        if (strncmp(p, base, base_len) == 0) {
            const char* q = p + base_len;
            const char* r2 = skip_ws_ascii(q);
            /* "est une " ou "est un " */
            if (strncmp(r2, "est une", 7) == 0 ||
                strncmp(r2, "est un", 6) == 0) {
                const char* s = r2 + (strncmp(r2, "est une", 7) == 0 ? 7 : 6);
                s = skip_ws_ascii(s);
                for (int i = 0; type_kw[i]; i++) {
                    size_t kl = strlen(type_kw[i]);
                    if (strncmp(s, type_kw[i], kl) == 0) { matched = 1; break; }
                }
            }
        }
        /* ligne suivante */
        const char* nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }

    free(buf);
    return matched;
}

static void scan_test_scenarios_dir(const char* dirPath, Reporter* r,
                                    int* checked) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    /* Determinons une fois si on est dans un sous-dossier multifile
     * (presence d'un test_*.owl voisin → on accepte les compagnons). */
    int allow_companions = dir_has_test_file(dirPath);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            /* Sous-dossier 'fixtures/' : ressources binaires (non-.owl) sans
             * writer pour les generer a la volee (ex: .xls). Exclu du scan —
             * la convention 'test_*.owl' ne s'y applique pas. */
            if (strcmp(e->d_name, "fixtures") == 0) continue;
            scan_test_scenarios_dir(full, r, checked);
        } else if (S_ISREG(st.st_mode)) {
            const char* name = e->d_name;
            /* README.md : doctrine au point d'usage, pas un
             * scenario. Toleré à la racine comme dans les sous-dossiers. */
            if (strcmp(name, "README.md") == 0) continue;
            (*checked)++;
            size_t name_len = strlen(name);
            int is_test_prefix = (name_len > 5 &&
                                  strncmp(name, "test_", 5) == 0);
            int is_owl_ext = (name_len > 4 &&
                              strcmp(name + name_len - 4, ".owl") == 0);
            if (is_test_prefix && is_owl_ext) continue;
            /* Exception compagnon multifile : cFoo.owl, iFoo.owl, etc. */
            if (allow_companions && is_owl_companion_filename(name))
                continue;
            /* Exception compagnon de type PascalCase : Statut.owl declarant
             * "Statut est une Énumération" (nom de fichier = nom de type). */
            if (allow_companions && is_owl_type_companion_file(name, full))
                continue;
            /* Exception ressources voisines ( canon split DSL strict) :
             * FEN_*.owl et ETAT_*.owl sont des declarations de Fenetre/Etat
             * voisines d'un test_*.owl pilote dans le meme sous-dossier.
             * Cohérent avec  "Migration : test_xxx.owl script +
             * voisin ressource pure". */
            if (allow_companions &&
                (strncmp(name, "FEN_", 4) == 0 ||
                 strncmp(name, "ETAT_", 5) == 0) &&
                is_owl_ext) continue;
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Fichier '%s' dans tests/scenarios/ ne respecte pas "
                "la convention 'test_*.owl'. Renommer ou retirer.",
                name);
            rep_violation(r, "test_scenario_filename_convention",
                          full, detail);
        }
    }
    closedir(d);
}

static void check_rule39_test_scenario_filename(Reporter* r) {
    int checked = 0;
    scan_test_scenarios_dir("tests/scenarios", r, &checked);
    /* Si tests/scenarios/ n'existe pas (cwd inattendu, smoke test sur
       owl.exe deploye), checked reste a 0 — pas de violation par defaut,
       on ne hurle pas dans un contexte ou le repo n'est pas accessible. */
}

/* Rule 40 — registry des limites compile-time (owl_limits.def) : champs
 * minimaux non vides + valeur strictement positive. Cf . */
static void check_rule40_limits_catalog(Reporter* r) {
    int n = owl_registry_limit_count();
    for (int i = 0; i < n; i++) {
        const OwlLimitDef* L = owl_registry_limit_at(i);
        if (!L) {
            rep_violation(r, "limit_null_entry", "(limit)",
                "owl_registry_limit_at retourne NULL pour un index valide.");
            continue;
        }
        if (!L->name || !*L->name) {
            rep_violation(r, "limit_name_missing", "(limit)",
                "Entree OWL_LIMITS[] sans nom. Le .def est corrompu.");
            continue;
        }
        if (L->value <= 0) {
            char detail[160];
            snprintf(detail, sizeof detail,
                "Limite '%s' a une valeur non strictement positive (%d). "
                "Une limite compile-time doit etre >= 1.",
                L->name, L->value);
            rep_violation(r, "limit_value_non_positive", L->name, detail);
        }
        if (!L->category || !*L->category) {
            char detail[160];
            snprintf(detail, sizeof detail,
                "Limite '%s' sans categorie. Ajoute une categorie dans owl_limits.def "
                "(ex: \"Compiler/Types\").", L->name);
            rep_violation(r, "limit_category_missing", L->name, detail);
        }
        bool fr_ok = L->descFR && *L->descFR;
        bool en_ok = L->descEN && *L->descEN;
        if (!fr_ok && !en_ok) {
            char detail[160];
            snprintf(detail, sizeof detail,
                "Limite '%s' sans description FR ni EN. Au moins une des deux "
                "est requise (idealement les deux).", L->name);
            rep_violation(r, "limit_desc_missing", L->name, detail);
        }
    }
}

/* ============================================================
 * Regle 41 : fichiers user-facing en UTF-8 strict, pas de BOM
 *
 * Tout .owl et .md dans examples/, tests/, docs/public_documentation/
 * doit etre encode en UTF-8 valide et SANS BOM (EF BB BF).
 *
 * Le BOM casse les shebangs, le JSON strict (RFC 8259), et certains
 * parsers. Il est generalement ajoute silencieusement par Notepad ou
 * un editeur Microsoft mal configure. Aucun outil moderne n'en a besoin.
 *
 * Reject : BOM, sequences UTF-8 invalides (latin-1, UTF-16, mix
 * d'encodings dans un meme fichier).
 * ============================================================ */
static int rule41_is_valid_utf8(const unsigned char* b, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = b[i];
        if (c < 0x80) {
            i++;
        } else if ((c >> 5) == 0x06) {
            if (i + 1 >= n || (b[i+1] >> 6) != 2) return 0;
            i += 2;
        } else if ((c >> 4) == 0x0E) {
            if (i + 2 >= n ||
                (b[i+1] >> 6) != 2 || (b[i+2] >> 6) != 2) return 0;
            i += 3;
        } else if ((c >> 3) == 0x1E) {
            if (i + 3 >= n ||
                (b[i+1] >> 6) != 2 || (b[i+2] >> 6) != 2 ||
                (b[i+3] >> 6) != 2) return 0;
            i += 4;
        } else {
            return 0;  /* continuation byte isole, ou 0xF8+ */
        }
    }
    return 1;
}

static void scan_file_for_utf8(const char* path, Reporter* r) {
    /*  : fixture qui valide la TOLERANCE du scanner au BOM UTF-8 en
     * tete de fichier — elle DOIT commencer par un BOM (EF BB BF). Seule
     * exemption intentionnelle de la regle 41. */
    if (strstr(path, "scanner_invisible_carveout")) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return; }
    unsigned char* buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (nread >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        rep_violation(r, "public_file_utf8_bom", path,
            "Fichier commence par BOM UTF-8 (EF BB BF). Le BOM casse "
            "les shebangs, le JSON strict, et certains parsers. Retirer "
            "le BOM (e.g. via 'sed -i 1s/^\\xEF\\xBB\\xBF//' <file>).");
    }

    if (!rule41_is_valid_utf8(buf, nread)) {
        rep_violation(r, "public_file_not_utf8", path,
            "Fichier contient des bytes non-UTF-8 (probablement latin-1/"
            "cp1252 ou UTF-16). Convertir en UTF-8 sans BOM via "
            "'iconv -f <src> -t UTF-8' ou via l'editeur (VSCode : Save "
            "with Encoding -> UTF-8).");
    }

    free(buf);
}

static void scan_dir_for_utf8(const char* dirPath, Reporter* r) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir_for_utf8(full, r);
        } else if (S_ISREG(st.st_mode)) {
            const char* ext = strrchr(e->d_name, '.');
            if (!ext) continue;
            if (strcmp(ext, ".owl") == 0 || strcmp(ext, ".md") == 0) {
                scan_file_for_utf8(full, r);
            }
        }
    }
    closedir(d);
}

static void check_rule41_public_files_utf8(Reporter* r) {
    static const char* ROOTS[] = {
        "examples",
        "tests",
        "docs/public_documentation",
        NULL
    };
    for (int i = 0; ROOTS[i]; i++) {
        scan_dir_for_utf8(ROOTS[i], r);
    }
}

/* ============================================================
 * Regle 42 : invariants registry erreurs
 *
 * Verifie la coherence interne d'OWL_ERRORS[] (owl_errors.def) :
 *  - Chaque entree a un name non-vide
 *  - Chaque entree a une category valide (lex/syntax/semantic/runtime/native/io)
 *  - Chaque entree a un templateFR ET templateEN non-vide
 *  - Aucun doublon de name (case-insensitive)
 *
 * Jauge de migration (informational, non-bloquante) : affiche le compte
 * de sites legacy scanner/compiler/vm/stdlib qui n'ont pas encore migre
 * vers owl_emit_error. STUB initial : retourne 0/0 tant que l'implementer
 * n'a pas branche la logique.
 * ============================================================ */

#include "owl_errors.h"

/* Categories autorisees pour les codes d'erreur. La categorie "warning"
 * couvre les diagnostics non-bloquants emis par le compiler (variables
 * non utilisees, shadowing, constructions non encore supportees...).
 * Cf.  jalon D pour la migration des warning_at vers le registry. */
static const char* ALLOWED_ERROR_CATEGORIES[] = {
    "lex", "syntax", "semantic", "runtime", "native", "io", "warning", NULL
};

static bool is_allowed_error_category(const char* cat) {
    if (!cat) return false;
    for (const char** c = ALLOWED_ERROR_CATEGORIES; *c; c++) {
        if (strcmp(cat, *c) == 0) return true;
    }
    return false;
}

static void check_rule42_error_registry_invariants(Reporter* r) {
    int n = owl_registry_error_count();
    /* STUB : retourne 0 tant que l'implementer n'a pas branche. Les
     * tests d'assertions en C dedies verifieront le rouge attendu. */
    if (n == 0) {
        /* Note : on ne bloque pas le build si count=0 — ca veut juste dire
         * que l'implementer n'a pas encore rempli les stubs. Le ticket
         *  couvre cette phase. */
        return;
    }

    /* Verifier chaque entree. */
    for (int i = 0; i < n; i++) {
        const OwlErrorDef* e = owl_registry_error_at(i);
        if (!e) {
            rep_violation(r, "error_null_entry", "(error)",
                "owl_registry_error_at retourne NULL pour un index valide.");
            continue;
        }
        if (!e->name || !*e->name) {
            rep_violation(r, "error_name_missing", "(error)",
                "Entree OWL_ERRORS[] sans nom. Le .def est corrompu.");
            continue;
        }
        if (!is_allowed_error_category(e->category)) {
            char detail[200];
            snprintf(detail, sizeof detail,
                "Code erreur '%s' a une categorie invalide '%s'. "
                "Categories autorisees : lex, syntax, semantic, runtime, native, io, warning.",
                e->name, e->category ? e->category : "(null)");
            rep_violation(r, "error_category_invalid", e->name, detail);
        }
        if (str_empty(e->templateFR)) {
            char detail[160];
            snprintf(detail, sizeof detail,
                "Code erreur '%s' sans templateFR. Tout code d'erreur doit avoir "
                "un message bilingue.", e->name);
            rep_violation(r, "error_template_fr_missing", e->name, detail);
        }
        if (str_empty(e->templateEN)) {
            char detail[160];
            snprintf(detail, sizeof detail,
                "Code erreur '%s' sans templateEN. Invariant bilingue.", e->name);
            rep_violation(r, "error_template_en_missing", e->name, detail);
        }
    }

    /* Verifier doublons (case-insensitive). */
    for (int i = 0; i < n; i++) {
        const OwlErrorDef* e1 = owl_registry_error_at(i);
        if (!e1 || !e1->name) continue;
        for (int j = i + 1; j < n; j++) {
            const OwlErrorDef* e2 = owl_registry_error_at(j);
            if (!e2 || !e2->name) continue;
            char norm1[128], norm2[128];
            normalize_ascii_lower(e1->name, norm1, sizeof norm1);
            normalize_ascii_lower(e2->name, norm2, sizeof norm2);
            if (strcmp(norm1, norm2) == 0) {
                char detail[200];
                snprintf(detail, sizeof detail,
                    "Doublon code erreur : '%s' et '%s' se normalisent "
                    "en '%s'. Renommer l'un des deux.", e1->name, e2->name, norm1);
                rep_violation(r, "error_name_duplicate", e1->name, detail);
            }
        }
    }
}

/* ============================================================
 * Regle 43 : couverture headless des widget kinds
 *
 * Pour chaque OwlControlKind avec is_widget=true dans CONTROL_KINDS :
 *   (a) la prop universelle "Visible" doit etre exposee dans le catalog
 *       PROPS_<KIND> via owl_control_catalog_find(kind, "Visible") (prop
 *       sentinelle du mode headless — sans elle, aucun test d'etat
 *       affichable n'est possible).
 *   (b) obj_new_ui_control(kind, NULL, "TEST_<KIND>") doit retourner un
 *       ObjUiControl non-NULL et bien forme (kind correct preserve,
 *       headless_store initialise).
 *
 * Symptome attendu sans cette regle : un nouveau widget kind ajoute dans
 * l'enum + CONTROL_KINDS mais oublie dans le switch de
 * owl_control_catalog_find -> les set/get de prop Visible en mode headless
 * retournent NIL_VAL silencieusement et le test 'IDENT est un <Kind>;
 * IDENT..Visible = Faux' tombe sans diagnostic clair.
 * ============================================================ */
static void check_rule43_widget_kind_headless_coverage(Reporter* r) {
    const OwlControlKindInfo* tbl = owl_control_kinds_table();
    for (const OwlControlKindInfo* k = tbl; k->nameFR; k++) {
        if (!k->is_widget) continue;
        OwlControlKind kind;
        if (!owl_control_kind_of_info(k, &kind)) continue;  /* check_rule5c
                                                              attrape deja. */
        /* (a) + (c) : la prop universelle Visible doit etre exposee pour
         * tout widget reel. C'est la prop sentinelle du mode headless : si
         * Visible n'est pas resolu, aucune scene d'assertion runtime ne peut
         * verifier l'etat affichable d'un widget. Nom n'est pas teste ici
         * car c'est l'identifiant intrinseque ObjUiControl (uic->name),
         * pas une prop de catalog — la voie de lecture/ecriture passe par
         * un autre chemin que owl_control_catalog_find. */
        const OwlControlPropDef* visDef = owl_control_catalog_find(kind, "Visible");
        if (!visDef) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Widget '%s' (kind=%d) n'expose pas la prop universelle "
                "'Visible' dans son PROPS_<KIND> catalog (owl_control_catalog_find "
                "retourne NULL). Le mode headless feat/0082 garantit que tout "
                "ObjUiControl accepte au minimum Visible via headless_store ; "
                "sans entree catalog, le compilo refuse 'IDENT..Visible = X' "
                "au compile-time. Ajouter 'Visible' dans PROPS_<KIND> "
                "(src/ui_catalog.c).",
                k->nameFR, (int)kind);
            rep_violation(r, "widget_kind_missing_visible_prop",
                          k->nameFR, detail);
        }
        /* (b) : allocation ObjUiControl. Test minimal qui couvre les
         * regressions d'init (table headless_store, name strdup, type tag
         * OBJ_UI_CONTROL). Si obj_new_ui_control crashe pour un kind donne,
         * c'est attrape ici plutot qu'au premier test runtime. */
        char test_name[64];
        snprintf(test_name, sizeof(test_name), "TEST_%s",
                 k->nameAscii ? k->nameAscii : k->nameFR);
        ObjUiControl* uic = obj_new_ui_control((int)kind, NULL, test_name);
        if (!uic) {
            char detail[300];
            snprintf(detail, sizeof(detail),
                "obj_new_ui_control((int)%s, NULL, \"%s\") a retourne NULL. "
                "Le constructeur ObjUiControl doit accepter tous les widget "
                "kinds declares dans CONTROL_KINDS sans dependre du widget Qt "
                "(arg2=NULL = mode headless). Verifier src/object.c.",
                k->nameFR, test_name);
            rep_violation(r, "widget_kind_alloc_failed", k->nameFR, detail);
            continue;
        }
        if ((OwlControlKind)uic->kind != kind) {
            char detail[300];
            snprintf(detail, sizeof(detail),
                "ObjUiControl alloue pour '%s' a kind=%d au lieu de %d. Le "
                "constructeur ne preserve pas le kind passe en argument.",
                k->nameFR, uic->kind, (int)kind);
            rep_violation(r, "widget_kind_alloc_corrupt", k->nameFR, detail);
        }
    }
}

/* ============================================================
 * Regle 44 — enum_lookup_consistant
 *
 * Pour chaque enum du registry, `owl_registry_enum_find(name)` retourne
 * le meme pointeur que `owl_registry_enum_at(i)`, en forme accentuee ET
 * en forme nameAscii (invariant n°2 du langage). Ce lookup est consomme
 * par la contrainte de param (allowedBase / prop DSL `param ∈ enum`) —
 * s'il casse silencieusement (refacto du normalize_name, hash table
 * corrompue...), la validation des valeurs enum-contraintes regresse
 * sans bruit. Cette regle hurle AVANT qu'un test runtime regresse.
 *
 * Note : les enums NE SONT PAS des types declarables (`x est un <enum>`
 * echoue en Type inconnu, sauf si le nom est aussi un type composé du
 * registry comme Couleur, ou c'est le type qui resout). Le rejet de
 * l'enum-as-type est couvert par les fixtures compile-error dediees,
 * pas ici — la resolution de type est statique a compiler.c.
 * ============================================================ */
/* ============================================================
 * Regle 45 : dispatcher case-insensible doubles type/native
 *
 * Pour chaque double KNOWN_TYPE_FUNCTION_DOUBLES, verifie que :
 *   - la NATIVE est resolvable par owl_registry_find sur differentes
 *     formes de casse (lowercase, UPPERCASE, mixed) ;
 *   - le TYPE est resolvable par owl_registry_type_find sur les memes
 *     formes ;
 *   - les deux pointeurs sont distincts (sinon ce n'est pas un vrai
 *     double, contradiction de la whitelist).
 *
 * Garantit que le dispatch contextuel call-site/annotation-type reste
 * fonctionnel apres toute refacto future du registry ou du compileur.
 * Sans cette regle, une regression silencieuse de l'invariant 1 OWL
 * (case-insensible) ne se manifesterait qu'au moment ou un utilisateur
 * tape une mauvaise casse — masque en pratique par le formatter.
 * ============================================================ */

static void check_rule45_dispatcher_case_insensible_doubles(Reporter* r) {
    for (int i = 0; KNOWN_TYPE_FUNCTION_DOUBLES[i] != NULL; i++) {
        const char* baseName = KNOWN_TYPE_FUNCTION_DOUBLES[i];

        /* La native doit etre resolvable en case-insensible. */
        const OwlNativeDef* nat = owl_registry_find(baseName);
        if (nat == NULL) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                "Le double declare '%s' n'a pas de native homonyme dans le "
                "registry. Soit la native a ete retiree et la whitelist doit "
                "etre mise a jour, soit la native est presente sous un nom "
                "qui n'est pas reconnu par owl_registry_find (lookup case+"
                "accent insensible).", baseName);
            rep_violation(r, "dispatcher_case_insensible_doubles",
                          baseName, detail);
            continue;
        }

        /* Le type doit etre resolvable en case-insensible. */
        const OwlTypeDef* td = owl_registry_type_find(baseName);
        if (td == NULL) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                "Le double declare '%s' n'a pas de type homonyme dans le "
                "registry. La whitelist KNOWN_TYPE_FUNCTION_DOUBLES doit ne "
                "lister que les vrais doubles (TYPE ET NATIVE), source de "
                "verite pour le dispatch case-insensible contextuel.",
                baseName);
            rep_violation(r, "dispatcher_case_insensible_doubles",
                          baseName, detail);
            continue;
        }

        /* Verifications case-insensible sur 3 formes : lowercase pur,
         * UPPERCASE pur, et casse-mixte typique. La meme native ET le
         * meme type doivent etre retrouves quelle que soit la casse. */
        char upper[128];
        int blen = (int)strlen(baseName);
        int n = blen < (int)sizeof(upper) - 1 ? blen : (int)sizeof(upper) - 1;
        for (int k = 0; k < n; k++) upper[k] = (char)toupper((unsigned char)baseName[k]);
        upper[n] = '\0';

        if (owl_registry_find(upper) != nat) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                "owl_registry_find('%s' UPPERCASE) ne retourne pas la meme "
                "native que owl_registry_find('%s'). Invariant 1 OWL (case-"
                "insensible) casse sur le lookup natives — le dispatch "
                "call-site case-incorrecte (`%s(args)`) plantera au runtime.",
                upper, baseName, upper);
            rep_violation(r, "dispatcher_case_insensible_doubles",
                          baseName, detail);
        }

        if (owl_registry_type_find(upper) != td) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                "owl_registry_type_find('%s' UPPERCASE) ne retourne pas le "
                "meme type que owl_registry_type_find('%s'). Invariant 1 OWL "
                "casse sur le lookup types — le dispatch annotation type "
                "case-incorrecte (`est un %s`) plantera au runtime.",
                upper, baseName, upper);
            rep_violation(r, "dispatcher_case_insensible_doubles",
                          baseName, detail);
        }
    }
}

static void check_rule44_enum_lookup_consistant(Reporter* r) {
    int ne = owl_registry_enums_count();
    for (int i = 0; i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (e == NULL || e->name == NULL) continue;
        const OwlEnum* foundByName = owl_registry_enum_find(e->name);
        if (foundByName != e) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                "owl_registry_enum_find(\"%s\") retourne un autre pointeur que "
                "owl_registry_enum_at(%d). Bug de lookup registry : la "
                "contrainte de param (allowedBase / prop DSL enum) ne resout "
                "plus ce nom — regression silencieuse.",
                e->name, i);
            rep_violation(r, "enum_lookup_consistant", e->name, detail);
            continue;
        }
        if (e->nameAscii != NULL && e->nameAscii != e->name) {
            const OwlEnum* foundByAscii = owl_registry_enum_find(e->nameAscii);
            if (foundByAscii != e) {
                char detail[256];
                snprintf(detail, sizeof(detail),
                    "owl_registry_enum_find(\"%s\" via nameAscii) retourne "
                    "un autre pointeur que owl_registry_enum_at(%d). Lookup "
                    "accent-strip casse pour cet enum (invariant n°2 du "
                    "langage).",
                    e->nameAscii, i);
                rep_violation(r, "enum_lookup_consistant",
                              e->nameAscii, detail);
            }
        }
    }
}

/* ============================================================
 * Jauge migration registry erreurs (mode strict depuis )
 *
 * Affiche le compteur de sites migres vers owl_emit_error /
 * owl_emit_native_error / owl_error_render_*. Les valeurs sont
 * hardcodees a la main (le binaire n'a pas acces au code source, un
 * grep statique n'a pas de sens). A chaque lot de migration, mettre
 * a jour les MIGRATED et committer en meme temps que le code.
 *
 * Mode strict  : violation_count++ via rep_violation des
 * que `migrated < total` sur une famille. Verrouille le chantier
 * registry erreurs a 100% et empeche toute regression future. La
 * jauge JSON reste produite pour traceabilite / CI.
 * ============================================================ */
typedef struct { const char* family; int migrated; int total; } MigrationCounter;
static const MigrationCounter MIGRATION_PROGRESS[] = {
    /* Scanner : 6 sites error_token, tous migres au lot 1. */
    { "scanner",  6,   6 },
    /* Compiler : compte des error_at/error_at_current/error()/error2()
     * dans src/compiler.c au moment de la fondation . */
    { "compiler", 233, 233 },
    /* VM : runtime_error() dans src/vm.c. */
    { "vm",       91,  91 },
    /* Stdlib : owl_set_error() dans src/stdlib_*.c (somme des 18 modules
     * stdlib presents au lot 1, 376 sites cumules).
     *  jalon B : PoC migration 6 sites (stdlib_math 2 + stdlib_regex 1
     * + stdlib_runtime 3) vers owl_emit_native_error.
     *  : +4 sites stdlib_eval (Compile / ExecuteCode /
     * ExecuteTraitement / EvalueExpression) + 8 sites stdlib_string
     * (ChaineVersEntier, ChaineVersReel, TableauVersCSV, CSVVersTableau,
     * Tronque, POUR TOUTE CHAINE x2, POUR TOUTE POSITION) + 12 sites
     * stdlib_date (ChaineVersDate, ChaineVersHeure, DateVersJourEnLettre
     * x2, NumeroDeMoisVersLettre x2, EntierVersJourEnLettre x2,
     * EntierVersMoisEnLettre x2, Paques x2).
     *  : +9 sites stdlib_uuid (DonneUUID x3 + UUID.VersChaine x4 +
     * UUID x2) + 34 sites stdlib_crypto (HashChaine x8 + HashFichier x9 +
     * HMACChaine x1 + HashVerifieChaine x5 + HashVerifieFichier x2 +
     * CrypteStandard x3 + DecrypteStandard x6) vers owl_emit_native_error.
     * Codes ajoutes : E_NATIVE_CRYPTO_BAD_KEY/BAD_DATA/INIT_FAILURE +
     * E_NATIVE_CRYPTO_UNSUPPORTED_ALGO + E_NATIVE_IO_OPEN_FAILURE.
     *  : +15 sites stdlib_json (ChaineVersJSON, JSONValide x6,
     * Serialise, Deserialise x2, JSONSerialise, JSONDeserialise x3) +
     * 41 sites stdlib_xml (XMLOuvre x2, XMLDocument, XMLTermine,
     * XMLConstruitChaine, XMLSauve x3, XMLRacine, XMLEcrit,
     * XMLAjouteFils x3, XMLSupprime, XMLAjouteAttribut x2,
     * XMLExecuteXPath x2, XMLExecuteXPathNS, XMLLitXPath x2,
     * XMLEcritXPath x3, XMLInsereFils x3, XMLAjouteCommentaire x2,
     * XMLAjouteCDATA x2, XMLLitAttribut x2, XMLAjouteEspaceNom x3,
     * XMLDocumentValide x5). Codes ajoutes : E_NATIVE_XML_PARSE_ERROR +
     * E_NATIVE_XML_SCHEMA_ERROR + E_NATIVE_XML_NODE_CREATE_FAILURE +
     * E_NATIVE_XML_SAVE_FAILURE + E_NATIVE_JSON_PARSE_ERROR.
     *  : +11 sites stdlib_http (HTTPRequete x3,
     * HTTPTransfereFichier x4, HTTPEnvoie x4) + 60 sites stdlib_sql
     * (resolve_sql_cnx x2, 5 backends Postgres x4 + Envoie x4 / MySQL
     * x6 / DuckDB x5 / MSSQL x10 / ODBC x3 / HFSQL x3 / ODBC Envoie
     * x5 + dispatcher SQLConnexionOuvre x3 + SQLite path x5 +
     * SQLConnexionFerme x1 + SQLEnvoie dispatcher x9). Codes
     * ajoutes : E_NATIVE_OUT_OF_MEMORY + E_NATIVE_SQL_NOT_OPEN +
     * E_NATIVE_SQL_ALREADY_OPEN + E_NATIVE_SQL_PROVIDER_UNKNOWN +
     * E_NATIVE_SQL_BACKEND_ERROR + E_NATIVE_HTTP_INVALID_URL +
     * E_NATIVE_HTTP_REQUEST_FAILURE + E_NATIVE_HTTP_STATUS_ERROR +
     * E_NATIVE_HTTP_BUILD_BODY_FAILURE.
     *  : +8 sites stdlib_file (file_error helper x1 ;
     * fOuvre/fCrée slot épuisé x2 ; fFerme/fPositionne/fChangeTaille
     * handle invalide x3 ; fAjouteBuffer/fSauveBuffer type mismatch
     * x2). Codes ajoutes : E_NATIVE_IO_TOO_MANY_OPEN +
     * E_NATIVE_IO_INVALID_HANDLE + E_NATIVE_IO_FAILURE.
     *  : +94 sites stdlib_image (85) + stdlib_pdf (9).
     * Codes ajoutes : E_NATIVE_IMAGE_LOAD_FAILURE +
     * E_NATIVE_IMAGE_SAVE_FAILURE + E_NATIVE_IMAGE_UNSUPPORTED_FORMAT +
     * E_NATIVE_IMAGE_INVALID_HANDLE + E_NATIVE_IMAGE_NO_CANVAS +
     * E_NATIVE_IMAGE_FONT_REQUIRED + E_NATIVE_IMAGE_FONT_LOAD_FAILURE +
     * E_NATIVE_PDF_ETAT_NOT_FOUND + E_NATIVE_PDF_BLOC_NOT_FOUND +
     * E_NATIVE_PDF_NOT_IN_ETAT.
     *  : +71 sites stdlib_compression (69 legacy int + 2
     * nouveaux cas d'erreur decouverts pendant migration). Native
     * zipMsgErreur retiree pure et simple (option β figee). Codes
     * ajoutes : 6 E_NATIVE_COMPRESSION_* (INVALID_TYPE, INVALID_LEVEL,
     * MEMORY_FAILURE, CORRUPT_DATA, INVALID_HEADER, TRUNCATED) + 11
     * E_NATIVE_ZIP_* (ARCHIVE_NOT_OPEN_READ/WRITE, FILE_NOT_FOUND,
     * ENTRY_NOT_FOUND, INDEX_OUT_OF_BOUNDS, UNSUPPORTED_FORMAT,
     * CORRUPT_ARCHIVE, WRITE_FAILURE, IO_FAILURE, INVALID_PASSWORD,
     * ENCRYPTION_REQUIRED, OPERATION_CANCELED). */
    /*  : cloture du chantier registry erreurs. Migration des 4
     * derniers sites callsites (owl_registry.c __InstanceCoerce x2,
     * stdlib_ui.c zr_arg_or_error x1, stdlib_erreur.c fn_erreur_declenche
     * x1). Le legacy transport owl_set_error(int, const char*) a ete
     * retire pure et simple. fn_erreur_declenche utilise desormais
     * owl_set_error_native() avec le code numerique user-explicit
     * stocke en chaine ("42" pour ErreurDeclenche(42, ...)), preservant
     * le contrat ErreurInfo(errCode). Total reajuste de 387 a 380 pour
     * refleter le decompte reel post-grep frais (les 7 sites "fantomes"
     * mentionnes par  n'existaient plus). */
    { "stdlib",   380, 380 },
};
#define MIGRATION_PROGRESS_LEN \
    ((int)(sizeof(MIGRATION_PROGRESS) / sizeof(MIGRATION_PROGRESS[0])))

static void print_migration_progress(Reporter* r, FILE* out) {
    int totMig = 0, totAll = 0;
    fputs("  \"errors_migration\": {", out);
    for (int i = 0; i < MIGRATION_PROGRESS_LEN; i++) {
        const MigrationCounter* m = &MIGRATION_PROGRESS[i];
        totMig += m->migrated;
        totAll += m->total;
        if (i > 0) fputc(',', out);
        fprintf(out, "\"%s\":{\"migrated\":%d,\"total\":%d}",
                m->family, m->migrated, m->total);
        /* Mode strict : viol si famille pas a 100%. */
        if (m->migrated < m->total) {
            char detail[200];
            snprintf(detail, sizeof(detail),
                "Migration registry erreurs incomplete sur famille '%s' : "
                "%d/%d sites migres vers owl_emit_*_error. Reste %d site(s) "
                "legacy a migrer.", m->family, m->migrated, m->total,
                m->total - m->migrated);
            rep_violation(r, "errors_migration_incomplete", m->family, detail);
        }
    }
    double pct = totAll > 0 ? (100.0 * totMig) / totAll : 0.0;
    fprintf(out, ",\"_total\":{\"migrated\":%d,\"total\":%d,\"pct\":%.1f}",
            totMig, totAll, pct);
    fputs("},\n", out);
    /* Echo humain sur stderr (n'invalide pas le JSON stdout). */
    fprintf(stderr,
            "Migration registry erreurs : scanner %d/%d, compiler %d/%d, "
            "vm %d/%d, stdlib %d/%d (total %d/%d, %.1f%%)\n",
            MIGRATION_PROGRESS[0].migrated, MIGRATION_PROGRESS[0].total,
            MIGRATION_PROGRESS[1].migrated, MIGRATION_PROGRESS[1].total,
            MIGRATION_PROGRESS[2].migrated, MIGRATION_PROGRESS[2].total,
            MIGRATION_PROGRESS[3].migrated, MIGRATION_PROGRESS[3].total,
            totMig, totAll, pct);
}

/* ============================================================
 * Regle 47 : @sujet d'un scenario PUB doit pointer une native canon
 *
 * Tout fichier tests/scenarios/natives/test_*.owl doit declarer en
 * tete une ligne `// @sujet : <Native>[, <Native2>, ...]` ou chaque
 * token (apres trim) resout vers une native du registry (case +
 * accent insensible via owl_registry_find).
 *
 * Sans ce gage, un scenario peut declarer en @sujet une description
 * libre ("forEach sur tableau" au lieu de "Applique"). Le parser
 * `sujet_matches` (cli_doc.c) ne le match plus contre la native
 * cible et `examples_count` reste a 0 — le hover LSP / owl explain
 * n'expose pas le scenario.
 *
 * Tolerances :
 *   - sujet vide ou ligne `// @sujet` absente : SKIP (un scenario
 *     peut etre transverse sans cible unique ; rule39 verifie deja
 *     que le fichier respecte la convention test_*.owl).
 *   - tokens separes par virgule pour les scenarios transverses
 *     (round-trip JSON, formatage transverse, etc.).
 *
 * Reject : token non vide qui ne resout vers AUCUN symbole publie
 * (native, mot-cle, event).
 * ============================================================ */
static void rule47_trim_token(char* tok) {
    /* Trim leading whitespace + ponctuation typique. */
    size_t n = strlen(tok);
    size_t start = 0;
    while (start < n && (tok[start] == ' ' || tok[start] == '\t' ||
                         tok[start] == '`' || tok[start] == '\'' ||
                         tok[start] == '"')) {
        start++;
    }
    if (start > 0) memmove(tok, tok + start, n - start + 1);
    n = strlen(tok);
    while (n > 0) {
        char c = tok[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '`' || c == '\'' || c == '"') {
            tok[--n] = '\0';
        } else break;
    }
}

static void rule47_check_file(const char* path, Reporter* r) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char line[1024];
    char sujet[512];
    sujet[0] = '\0';
    /* On scanne les ~30 premieres lignes pour trouver @sujet. */
    for (int i = 0; i < 30 && fgets(line, sizeof(line), f); i++) {
        /* Match prefixe "//" + whitespace + "@sujet" + ":" */
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] != '/' || p[1] != '/') continue;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "@sujet", 6) != 0) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        size_t cp = strlen(p);
        if (cp >= sizeof(sujet)) cp = sizeof(sujet) - 1;
        memcpy(sujet, p, cp);
        sujet[cp] = '\0';
        /* Trim CR/LF tail. */
        while (cp > 0 && (sujet[cp - 1] == '\r' || sujet[cp - 1] == '\n')) {
            sujet[--cp] = '\0';
        }
        break;
    }
    fclose(f);
    if (!sujet[0]) return;  /* Sujet vide ou absent : SKIP. */

    /* Split sur virgule, verifier chaque token. */
    char* p = sujet;
    while (*p) {
        char* comma = strchr(p, ',');
        char tok[256];
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        if (tlen >= sizeof(tok)) tlen = sizeof(tok) - 1;
        memcpy(tok, p, tlen);
        tok[tlen] = '\0';
        rule47_trim_token(tok);
        if (tok[0]) {
            /*  — accepte aussi un keyword catalog hit (ex. MoiMême)
             * pour permettre aux scenarios de documenter un mot-cle bilingue
             * sans devoir le declarer comme native. Tolere aussi un slash
             * `/` comme separateur supplementaire entre formes bilingues
             * (FR/EN) — on prend la premiere moitie comme nom canon. */
            char* slash = strchr(tok, '/');
            if (slash) {
                *slash = '\0';
                rule47_trim_token(tok);
            }
            const OwlNativeDef* nat = owl_registry_find(tok);
            const OwlKeywordDef* kw = nat ? NULL : owl_keyword_find_by_word(tok);
            /*  — un scenario peut documenter un EVENT (lifecycle
             * Fenetre, event widget, Etat, OptionMenu, collection). Ces
             * symboles vivent dans owl_event_catalog, indexes ni par
             * owl_registry_find ni par le catalogue de mots-cles, alors
             * que `owl explain` / `owl examples` les resolvent. */
            const OwlEventDef* ev =
                (nat || kw) ? NULL : owl_event_catalog_find_any(tok);
            if (!nat && !kw && !ev) {
                char detail[600];
                snprintf(detail, sizeof(detail),
                    "Token '@sujet' = \"%s\" dans %s ne resout vers aucune "
                    "native canon, keyword ni event du registry. "
                    "Forme attendue : "
                    "`// @sujet : <NomCanon>` (eventuellement liste separee "
                    "par virgule pour scenarios transverses). La description "
                    "libre va dans `@resume` sur ligne suivante.",
                    tok, path);
                rep_violation(r, "test_scenario_sujet_not_canonical",
                              path, detail);
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
}

static void rule47_scan_dir(const char* dirPath, Reporter* r) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            rule47_scan_dir(full, r);
        } else if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(e->d_name);
            if (nl <= 9) continue;  /* "test_X.owl" minimum */
            if (strncmp(e->d_name, "test_", 5) != 0) continue;
            if (strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
            rule47_check_file(full, r);
        }
    }
    closedir(d);
}

static void check_rule47_test_scenario_sujet_canonical(Reporter* r) {
    /* Scope : tests/scenarios/natives/ uniquement (autres sous-dossiers
     * comme widgets/, regression/ peuvent avoir conventions
     * differentes — extensible si besoin futur). */
    rule47_scan_dir("tests/scenarios/natives", r);
}

/* ============================================================
 * Regle 49 : scenario_pub_metadata_required
 *
 * Tout fichier tests/scenarios/natives/test_*.owl doit declarer en
 * TETE de fichier le bloc de metadonnees parse par embedded_docs :
 *
 *   Ligne 1 : `// @sujet : <NomCanon>[, <NomCanon2>, ...]`
 *   Ligne 2 : `// @résumé : <description ≥ 10 chars>` (variante ASCII
 *             `@resume` toleree)
 *
 * Le sujet (1er token) doit resoudre vers une native du registry
 * (case + accent insensible via owl_registry_find), sinon
 * `owl examples <NomNative>` ne trouve rien.
 *
 * Pourquoi en doublon partiel de rule47 ?
 * Rule47 SKIP si la ligne `@sujet` est absente ou vide (tolerance
 * historique). Rule49 est STRICTE : le bloc est OBLIGATOIRE et a sa
 * place fixe en ligne 1+2. C'est un mecanisme **interne** au sous-
 * dossier natives/ (parsing embedded_docs.h, alimente `owl examples`)
 * — pas une convention OWL user-facing. Cf. doctrine ticket .
 *
 * Scope : tests/scenarios/natives/ uniquement (les autres sous-
 * dossiers — language/, ui/, types/, poo/, widgets/ — n'ont pas de
 * cible native unique).
 * ============================================================ */

#define RULE49_MIN_RESUME_LEN 10

/* Verifie si line commence par `// @<tag>[ :]`. Retourne pointeur sur
 * le debut du contenu apres `:` (trim left whitespace) si match, NULL
 * sinon. tag_alt peut etre NULL ou variante ASCII (ex: "resume" pour
 * "résumé"). */
static const char* rule49_match_tag(const char* line, const char* tag,
                                    const char* tag_alt) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] != '/' || p[1] != '/') return NULL;
    p += 2;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '@') return NULL;
    p++;
    size_t lt = strlen(tag);
    if (strncmp(p, tag, lt) == 0) {
        p += lt;
    } else if (tag_alt) {
        size_t la = strlen(tag_alt);
        if (strncmp(p, tag_alt, la) == 0) {
            p += la;
        } else {
            return NULL;
        }
    } else {
        return NULL;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Trim CR/LF en fin de chaine in-place. */
static void rule49_rtrim_eol(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static void rule49_check_file(const char* path, Reporter* r) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char line1[1024], line2[1024];
    line1[0] = line2[0] = '\0';
    if (!fgets(line1, sizeof(line1), f)) { fclose(f); }
    else if (!fgets(line2, sizeof(line2), f)) { fclose(f); }
    else { fclose(f); }
    rule49_rtrim_eol(line1);
    rule49_rtrim_eol(line2);

    /* --- Check ligne 1 : @sujet --- */
    const char* sujet_body = rule49_match_tag(line1, "sujet", NULL);
    if (!sujet_body) {
        char detail[512];
        snprintf(detail, sizeof(detail),
            "Ligne 1 doit etre `// @sujet : <NomCanon>`. Trouve : \"%s\". "
            "Le bloc @sujet/@résumé est le mecanisme interne du sous-dossier "
            "tests/scenarios/natives/ (parsing embedded_docs) — il est "
            "obligatoire en ligne 1 et 2. Pas une convention OWL user-facing.",
            line1);
        rep_violation(r, "scenario_pub_metadata_required", path, detail);
    } else if (!*sujet_body) {
        char detail[512];
        snprintf(detail, sizeof(detail),
            "Ligne 1 `// @sujet :` a un sujet vide. Forme attendue : "
            "`// @sujet : <NomCanon>` (ex: `// @sujet : ChaineCommencePar`).");
        rep_violation(r, "scenario_pub_metadata_required", path, detail);
    } else {
        /* Premier token (avant virgule) doit resoudre vers une native. */
        char tok[256];
        const char* comma = strchr(sujet_body, ',');
        size_t tlen = comma ? (size_t)(comma - sujet_body) : strlen(sujet_body);
        if (tlen >= sizeof(tok)) tlen = sizeof(tok) - 1;
        memcpy(tok, sujet_body, tlen);
        tok[tlen] = '\0';
        rule47_trim_token(tok);
        if (!tok[0]) {
            char detail[512];
            snprintf(detail, sizeof(detail),
                "Ligne 1 `// @sujet :` n'a aucun token exploitable apres "
                "le `:`. Trouve : \"%s\".", line1);
            rep_violation(r, "scenario_pub_metadata_required", path, detail);
        } else {
            /*  — accepte aussi un keyword catalog hit (MoiMême etc.)
             * + tolere `/` comme separateur bilingue (`MoiMême / MySelf`). */
            char* slash2 = strchr(tok, '/');
            if (slash2) {
                *slash2 = '\0';
                rule47_trim_token(tok);
            }
            if (!owl_registry_find(tok) && !owl_keyword_find_by_word(tok)) {
                char detail[600];
                snprintf(detail, sizeof(detail),
                    "Ligne 1 `// @sujet : %s` : le nom \"%s\" ne resout vers "
                    "aucune native ni keyword du registry (case + accent insensible). "
                    "Verifier la casse / les accents canon. "
                    "owl examples <NomNative> ne trouvera rien sans ca.",
                    tok, tok);
                rep_violation(r, "scenario_pub_metadata_required", path, detail);
            }
        }
    }

    /* --- Check ligne 2 : @résumé --- */
    const char* resume_body = rule49_match_tag(line2, "résumé", "resume");
    if (!resume_body) {
        char detail[512];
        snprintf(detail, sizeof(detail),
            "Ligne 2 doit etre `// @résumé : <description ≥ %d chars>`. "
            "Trouve : \"%s\". Le bloc metadata (ligne1=@sujet, "
            "ligne2=@résumé) est obligatoire en tete de chaque scenario PUB "
            "natives/ — c'est le mecanisme parse par embedded_docs pour "
            "alimenter `owl examples`.",
            RULE49_MIN_RESUME_LEN, line2);
        rep_violation(r, "scenario_pub_metadata_required", path, detail);
    } else {
        size_t blen = strlen(resume_body);
        if (blen < RULE49_MIN_RESUME_LEN) {
            char detail[600];
            snprintf(detail, sizeof(detail),
                "Ligne 2 `// @résumé : %s` est trop courte (%d chars, "
                "minimum %d). Resumer en une phrase ce que le scenario "
                "demontre, pour l'expose `owl examples`.",
                resume_body, (int)blen, RULE49_MIN_RESUME_LEN);
            rep_violation(r, "scenario_pub_metadata_required", path, detail);
        }
    }
}

static void rule49_scan_dir(const char* dirPath, Reporter* r) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(e->d_name);
            if (nl <= 9) continue;
            if (strncmp(e->d_name, "test_", 5) != 0) continue;
            if (strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
            rule49_check_file(full, r);
        }
        /* Pas de recursion : scope strict natives/ (cf. commentaire entete). */
    }
    closedir(d);
}

static void check_rule49_scenario_pub_metadata_required(Reporter* r) {
    rule49_scan_dir("tests/scenarios/natives", r);
}

/*  — helper qsort pour tri pointeurs de chaines (utilise par
 * rule 48 (f) unicite globale des noms du catalog d'icones). */
static int strcmp_ptr_helper(const void* a, const void* b) {
    const char* sa = *(const char* const*)a;
    const char* sb = *(const char* const*)b;
    return strcmp(sa, sb);
}

/* ============================================================
 * Rule 48 — icones_catalog_coverage
 *
 * Critere 7 du ticket T4 : auto-coverage du catalogue
 * d'icones embarque. Detecte :
 *
 *   (a) Catalogue vide (tool gen pas tourne, ou JSON sources absents)
 *   (b) Set sans aucune entry (filtre fluent -24- / line-md -static
 *       trop agressif, ou JSON corrompu cote vendor/iconify-sets/)
 *   (c) Entry avec body length=0 (corruption table)
 *   (d) Offset+length depasse OWL_ICONS_BLOB_RAW_SIZE (corruption blob)
 *   (e) Lookup roundtrip casse (regression index secondaire en
 *       stdlib_icones.c Lot 2a-ii)
 *
 * Pourquoi pas de verification hash recalcule = hash stocke ?
 * Le tool gen wrap chaque body avec `<svg viewBox='0 0 W H'>...` ou
 * W/H sont les dimensions par set (info.width/info.height du JSON
 * Iconify). Le header public n'expose pas ces dimensions, et le
 * resolver wrap actuellement avec viewBox fixe `0 0 24 24` (cf.
 * stdlib_icones.c L429 + commentaire L430-431 "affinage par set
 * reste a faire"). Donc le hash recalcule sur le svg_raw runtime ne
 * matchera pas le hash stocke pour les sets non-24x24 (fluent-emoji-flat
 * 32x32, etc.). Le hash content-addressable est garanti integre par le
 * pipeline build (blob compresse en .rodata, immuable post-build). La
 * vraie surface de regression a detecter = corruption d'integrite blob
 * + breakage du lookup runtime, couverts par (a)-(e).
 *
 * Sampling : 5 entries par set (60 verifs au total), suffisant pour
 * detecter une corruption systematique sans plomber le startup
 * --self-test-registry. Si une corruption ne touche qu'une icone
 * isolee, ca passera entre les mailles -- acceptable (le content
 * addressing par hash signale au runtime au moment du load).
 * ============================================================ */
static void check_rule48_icones_catalog_coverage(Reporter* r) {
    /* (a) Catalogue non vide. */
    if (OWL_ICONS_CATALOG_COUNT == 0) {
        rep_violation(r, "icones_catalog_empty", "OWL_ICONS_CATALOG",
            "Le catalogue d'icones est vide (COUNT=0). Le tool "
            "icons_catalog_gen n'a pas tourne ou les 12 JSON vendores "
            "manquent. Verifier mingw32-make all + vendor/iconify-sets/.");
        return;
    }

    /* Compte le nombre d'entries par set pour detecter les sets vides. */
    size_t set_counts[16] = {0};  /* Marge confortable vs OWL_ICONS_SET_COUNT=12. */
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
        const OwlIconCatalogEntry* e = &OWL_ICONS_CATALOG[i];
        if (e->set_id < (sizeof set_counts / sizeof set_counts[0])) {
            set_counts[e->set_id]++;
        }
    }

    /* (b) Chaque set declare doit avoir au moins 1 entry. */
    for (size_t s = 0; s < OWL_ICONS_SET_COUNT; s++) {
        if (set_counts[s] == 0) {
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Set '%s' (set_id=%zu) n'a aucune entry dans "
                "OWL_ICONS_CATALOG. Filtre tool gen trop agressif "
                "(fluent -24- / line-md -static) ou JSON vendore "
                "manquant. Verifier "
                "vendor/iconify-sets/%s.json + tools/icons_catalog_gen/.",
                OWL_ICONS_SET_NAMES[s], s, OWL_ICONS_SET_NAMES[s]);
            rep_violation(r, "icones_set_empty",
                          OWL_ICONS_SET_NAMES[s], detail);
        }
    }

    /* (c) (d) (e) Echantillon 5 entries par set. */
    const int max_samples = 5;
    for (size_t s = 0; s < OWL_ICONS_SET_COUNT; s++) {
        if (set_counts[s] == 0) continue;
        int samples_done = 0;
        for (size_t i = 0;
             i < OWL_ICONS_CATALOG_COUNT && samples_done < max_samples;
             i++) {
            const OwlIconCatalogEntry* e = &OWL_ICONS_CATALOG[i];
            if (e->set_id != s) continue;

            /* (c) body non vide */
            if (e->length == 0) {
                char detail[300];
                snprintf(detail, sizeof(detail),
                    "Icone '%s' (set='%s') a length=0 (body SVG vide). "
                    "Corruption table OWL_ICONS_CATALOG ou filtre tool "
                    "gen erratique.",
                    e->name, OWL_ICONS_SET_NAMES[s]);
                rep_violation(r, "icones_body_empty", e->name, detail);
            }

            /* (d) offset + length dans le blob */
            if ((size_t)e->offset + (size_t)e->length
                > OWL_ICONS_BLOB_RAW_SIZE) {
                char detail[300];
                snprintf(detail, sizeof(detail),
                    "Icone '%s' (set='%s') offset=%u + length=%u "
                    "depasse OWL_ICONS_BLOB_RAW_SIZE=%zu. Corruption "
                    "blob raw ou regression gen tool.",
                    e->name, OWL_ICONS_SET_NAMES[s], e->offset,
                    e->length, OWL_ICONS_BLOB_RAW_SIZE);
                rep_violation(r, "icones_offset_oob", e->name, detail);
            }

            /* (e) lookup roundtrip via namespace public */
            Value v = owl_icones_namespace_lookup(e->name);
            if (!IS_ICONE(v)) {
                char detail[400];
                snprintf(detail, sizeof(detail),
                    "owl_icones_namespace_lookup('%s') ne retourne pas "
                    "un ObjIcone (set='%s'). Probable regression index "
                    "secondaire stdlib_icones.c (Lot 2a-ii) ou "
                    "normalisation casse-insensible cassee.",
                    e->name, OWL_ICONS_SET_NAMES[s]);
                rep_violation(r, "icones_lookup_failed", e->name, detail);
            } else {
                ObjIcone* ic = AS_ICONE(v);
                if (!ic->valide) {
                    char detail[400];
                    snprintf(detail, sizeof(detail),
                        "owl_icones_namespace_lookup('%s') retourne "
                        "ObjIcone avec valide=false (set='%s'). Probable "
                        "echec decompression blob zstd ou OOM init lazy.",
                        e->name, OWL_ICONS_SET_NAMES[s]);
                    rep_violation(r, "icones_lookup_invalid", e->name,
                                  detail);
                }
            }

            samples_done++;
        }
    }

    /* (f)  — Unicite globale des noms. Apres uniquification
     * cote tool gen (suffix `_N` pour les 2e+ occurrences), tous les
     * `name` du catalog doivent etre uniques. Detecte une regression
     * du tool gen (nouvelle ajout sans uniquification, ou bug dans
     * `uniquify_entry_names`). Implementation O(n log n) via tri d'un
     * tableau de pointeurs vers les noms, puis walk lineaire. */
    if (OWL_ICONS_CATALOG_COUNT > 1) {
        const char** names = (const char**)malloc(
            OWL_ICONS_CATALOG_COUNT * sizeof(const char*));
        if (names) {
            for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
                names[i] = OWL_ICONS_CATALOG[i].name;
            }
            /* Trie via qsort (O(n log n) sur ~50k entries, instantane). */
            qsort(names, OWL_ICONS_CATALOG_COUNT,
                  sizeof(const char*), strcmp_ptr_helper);
            /* Walk : detecte premier doublon, emit violation. */
            for (size_t i = 1; i < OWL_ICONS_CATALOG_COUNT; i++) {
                if (strcmp(names[i - 1], names[i]) == 0) {
                    char detail[400];
                    snprintf(detail, sizeof(detail),
                        "Catalog d'icones contient des doublons de "
                        "nom (premier detecte : '%s'). L'uniquification "
                        "cote tools/icons_catalog_gen/ (Phase 4.5 "
                        "fix/0386) doit suffixer `_N` les 2e+ "
                        "occurrences. Regression du tool gen ou bug "
                        "dans uniquify_entry_names.",
                        names[i]);
                    rep_violation(r, "icones_catalog_duplicate_name",
                                  names[i], detail);
                    break;
                }
            }
            free(names);
        }
    }
}

/* ============================================================
 * Regle 50 : pas de canonical native enregistre plusieurs fois.
 *
 * Le registre plat des natives (owl_registry_count / owl_registry_at) ne
 * doit pas contenir deux entrees partageant le meme canonical (normalise
 * accent+casse). Un doublon signifie qu'un module re-enregistre un nom
 * deja pose par un autre module : le runtime garde la derniere version
 * (override implicite), mais le registre plat conserve les DEUX entrees,
 * et la doc-browser / le LSP / `explain` iterent ce registre et renvoient
 * la PREMIERE — souvent la mauvaise (categorie/desc divergentes).
 *
 * Cette regle ne regarde QUE le registre des natives entre elles. Les
 * doubles type-vs-native legitimes (invariant 6 #5, ex HTTPRequête native
 * + httpRequête type) vivent dans des registries SEPARES (natives vs
 * types) et sont couverts par les regles 6/45 — ils ne peuvent pas
 * declencher cette regle.
 * ============================================================ */
static void check_rule50_no_duplicate_native_canonical(Reporter* r) {
    int n = owl_registry_count();
    char a[128], b[128];
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* di = owl_registry_at(i);
        if (!di || !di->canonical) continue;
        normalize_ascii_lower(di->canonical, a, sizeof(a));
        if (!*a) continue;
        /* On ne signale que la PREMIERE re-occurrence (j > i) pour eviter
         * N rapports sur un meme triplet. On verifie aussi qu'aucune entree
         * anterieure (k < i) n'a deja le meme canonical : ainsi seule la
         * 2e entree du groupe emet la violation, une fois. */
        bool seen_before = false;
        for (int k = 0; k < i; k++) {
            const OwlNativeDef* dk = owl_registry_at(k);
            if (!dk || !dk->canonical) continue;
            normalize_ascii_lower(dk->canonical, b, sizeof(b));
            if (strcmp(a, b) == 0) { seen_before = true; break; }
        }
        if (seen_before) continue;
        for (int j = i + 1; j < n; j++) {
            const OwlNativeDef* dj = owl_registry_at(j);
            if (!dj || !dj->canonical) continue;
            normalize_ascii_lower(dj->canonical, b, sizeof(b));
            if (strcmp(a, b) != 0) continue;
            char detail[400];
            snprintf(detail, sizeof(detail),
                "Native '%s' enregistree plusieurs fois dans le registre plat "
                "(autre entree : '%s'). Le runtime garde la derniere version "
                "mais doc-browser/LSP/explain iterent le registre et renvoient "
                "la premiere — souvent la mauvaise (categorie/desc divergentes). "
                "Une seule definition canonique par nom. Retirer les doublons.",
                di->canonical, dj->canonical);
            rep_violation(r, "no_duplicate_native_canonical", di->canonical, detail);
            break;  /* un rapport par canonical duplique suffit */
        }
    }
}

/* ============================================================
 * Regle 51 : nameEN present sur consts / enums / types (digue ).
 *
 * Sœur de rule 11 (fields) et rule 2 (natives) : etend l'invariant
 * bilingue n°3 aux 3 structs OwlConstDef / OwlEnum / OwlTypeDef. Itere
 * chaque catalogue et signale toute entree publique dont nameEN est
 * NULL/vide. Le `symbol` rapporte = nom FR canon, le `detail` indique
 * le kind (const/enum/type) pour piloter l'inventaire du mop-up.
 *
 * Volet anti-collision (NOTE 1 ) : un nameEN de constante
 * enregistre un alias runtime dans vm.globals (obj_globals_set ecrase
 * silencieusement). On signale donc tout nameEN de const qui, apres
 * normalisation casse+accent, est egal au nom d'une AUTRE const FR
 * (ou d'un symbole global) portant une VALEUR DIFFERENTE. Le cas
 * benin "nameEN == ascii-strip(propre FR), meme valeur" (ex
 * fCréation->fCreation) n'est PAS une violation : le re-set runtime
 * repose la meme cle sur la meme valeur.
 *
 * Regle toujours active (digue fermee, couverture nameEN 100%
 * atteinte ). L'ancien gate OWL_SELFTEST_NAMEEN a ete retire.
 *
 * Plus aucune exemption : la derniere (la constante 'RC', sans nameEN pour
 * ne pas ecraser l'homonyme 'CR') est tombee a  lot 1, RC et CR
 * ayant quitte le registre pour la couche de compatibilite
 * EOL_LEGACY_ALIASES. L'invariant 3 est satisfait, plus contourne.
 * ============================================================ */

/* Valeurs identiques pour l'anti-collision : on traite Null/strings/ints. */
static bool values_equal_for_collision(Value a, Value b) {
    return values_equal(a, b);
}

static void check_rule51_name_en_consts_enums_types(Reporter* r) {
    /* -- Volet presence : consts -- */
    int nc = owl_registry_consts_count();
    for (int i = 0; i < nc; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (!c || !c->name) continue;
        if (str_empty(c->nameEN)) {
            rep_violation(r, "name_en_missing_const", c->name,
                "Constante sans nameEN. OWL bilingue par contrat (invariant 3) — "
                "fournir l'alias EN canon (canon sourcé ou OWL-proper validé).");
        }
    }

    /* -- Volet presence : enums -- */
    int ne = owl_registry_enums_count();
    for (int i = 0; i < ne; i++) {
        const OwlEnum* e = owl_registry_enum_at(i);
        if (!e || !e->name) continue;
        if (str_empty(e->nameEN)) {
            rep_violation(r, "name_en_missing_enum", e->name,
                "Enum sans nameEN. OWL bilingue par contrat (invariant 3) — "
                "fournir l'alias EN canon du nom d'enum (LSP/doc).");
        }
    }

    /* -- Volet presence : types -- */
    int nt = owl_registry_types_count();
    for (int i = 0; i < nt; i++) {
        const OwlTypeDef* t = owl_registry_type_at(i);
        if (!t || !t->canonical) continue;
        if (str_empty(t->nameEN)) {
            rep_violation(r, "name_en_missing_type", t->canonical,
                "Type sans nameEN. OWL bilingue par contrat (invariant 3) — "
                "fournir l'alias EN canon du nom de type.");
        }
    }

    /* -- Volet anti-collision (NOTE 1 ) --
     * Pour chaque const ayant un nameEN, on verifie que la forme
     * normalisee (casse+accent) de ce nameEN n'entre pas en conflit
     * avec une AUTRE const FR de valeur differente. La collision
     * "nameEN == ascii-strip(propre FR), meme valeur" est benigne. */
    char enNorm[128], frNorm[128];
    for (int i = 0; i < nc; i++) {
        const OwlConstDef* ci = owl_registry_const_at(i);
        if (!ci || !ci->name || str_empty(ci->nameEN)) continue;
        normalize_ascii_lower(ci->nameEN, enNorm, sizeof(enNorm));
        if (!*enNorm) continue;
        for (int j = 0; j < nc; j++) {
            if (j == i) continue;
            const OwlConstDef* cj = owl_registry_const_at(j);
            if (!cj || !cj->name) continue;
            normalize_ascii_lower(cj->name, frNorm, sizeof(frNorm));
            if (!*frNorm) continue;
            if (strcmp(enNorm, frNorm) != 0) continue;
            /* Collision de nom. Benin si meme valeur (re-set no-op). */
            if (values_equal_for_collision(ci->value, cj->value)) continue;
            char detail[400];
            snprintf(detail, sizeof(detail),
                "nameEN '%s' de la constante '%s' collisionne (casse+accent) "
                "avec la constante FR '%s' de VALEUR DIFFERENTE. obj_globals_set "
                "ecraserait silencieusement l'une des deux dans vm.globals. "
                "Choisir un nameEN distinct ou re-arbitrer le sourcing.",
                ci->nameEN, ci->name, cj->name);
            rep_violation(r, "name_en_collision_const", ci->name, detail);
            break;  /* un rapport par const suffit */
        }
    }
}

/* ============================================================
 * Rule 53 — scenarios_subdir_whitelist
 *
 * Les sous-dossiers directs de tests/scenarios/ sont une liste blanche
 * fermee. Bloque la reapparition de categories hors-convention.
 * ============================================================ */

static int rule53_subdir_whitelisted(const char* name) {
    static const char* wl[] = {
        "natives", "types", "language", "ui", "poo", NULL
    };
    for (int i = 0; wl[i]; i++)
        if (strcmp(name, wl[i]) == 0) return 1;
    return 0;
}

static void check_rule53_scenarios_subdir_whitelist(Reporter* r) {
    const char* base = "tests/scenarios";
    DIR* d = opendir(base);
    if (!d) return; /* cwd inattendu (owl.exe deploye) : pas de hurlement */
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", base, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        if (rule53_subdir_whitelisted(e->d_name)) continue;
        char detail[400];
        snprintf(detail, sizeof(detail),
            "Sous-dossier '%s' hors liste blanche de tests/scenarios/. "
            "Seuls {natives, types, language, ui, poo} sont autorises. "
            "Ranger les scenarios dans une categorie existante.", e->d_name);
        rep_violation(r, "scenarios_subdir_whitelist", full, detail);
    }
    closedir(d);
}

/* ============================================================
 * Rule 54 — scenario_no_smoke_only + scenario_assertion_outside_scenario_proc
 *           (, )
 *
 * Chaque test_*.owl de scenarios/ (recursif) doit porter au moins une
 * assertion NON tautologique ET toutes ses assertions DANS une PROCEDURE
 * scenario_ (scenario_/test_/should_ — les prefixes que le runner execute).
 * Une assertion au top-level ou dans une procedure helper non prefixee
 * scenario_ = violation (double comptage ou verification morte). Voir entete
 * du fichier pour la doctrine.
 * ============================================================ */

static int rule54_ident_start(unsigned char c) {
    return isalpha(c) || c == '_' || c >= 0x80;
}
static int rule54_ident_char(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 0x80;
}

/* p pointe sur '(' d'un appel. Ecrit le 1er argument (trim) dans arg. */
static void rule54_first_arg(const char* p, char* arg, int argSize) {
    p++; /* saute '(' */
    int depth = 1;
    const char* start = p;
    const char* end = p;
    while (*p && depth > 0) {
        char c = *p;
        if (c == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; continue; }
        if (c == '`') { p++; while (*p && *p != '`') p++; if (*p) p++; continue; }
        if (c == '(') { depth++; p++; continue; }
        if (c == ')') { depth--; if (depth == 0) { end = p; break; } p++; continue; }
        if (c == ',' && depth == 1) { end = p; break; }
        p++;
    }
    if (depth > 0) end = p; /* appel malforme : prend tout */
    while (start < end && (*start == ' ' || *start == '\t' ||
                           *start == '\n' || *start == '\r')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' ||
                           end[-1] == '\n' || end[-1] == '\r')) end--;
    int n = (int)(end - start);
    if (n >= argSize) n = argSize - 1;
    if (n < 0) n = 0;
    memcpy(arg, start, (size_t)n);
    arg[n] = '\0';
}

/* Ouvre un bloc terminé par FIN sans que la source unique le sache : mots-clés
 * structurels qui n'arrivent qu'APRÈS la copule (`c est une Classe`), donc
 * hors de OWL_DEPTH_TOKENS par construction (cf. prescan.h). Formes canoniques
 * (ascii, accents strippés) FR + EN.
 *
 *  — POUR / TANTQUE / BOUCLE / SELON / QUAND ont quitté cette liste :
 * ils sont dans la table, et c'est elle qui les rend maintenant. Ce qui reste
 * ici est disjoint de la table, volontairement. */
static int rule54_is_extra_block_opener(const char* canon) {
    static const char* const openers[] = {
        "classe", "class", "interface",
        "structure", "enumeration", "combinaison",
        NULL
    };
    for (int i = 0; openers[i]; i++)
        if (strcmp(canon, openers[i]) == 0) return 1;
    return 0;
}

static int rule54_is_proc_opener(const char* canon) {
    return strcmp(canon, "procedure") == 0
        || strcmp(canon, "fonction")  == 0
        || strcmp(canon, "function")  == 0;
}

/* Modificateurs qui s'intercalent entre PROCEDURE et le nom (visibilité,
 * portée, INTERNE...). Ils ne sont pas le nom de la procédure. */
static int rule54_is_proc_modifier(const char* canon) {
    static const char* const mods[] = {
        "public", "publique", "prive", "privee", "private",
        "protege", "protegee", "protected",
        "interne", "internal", "abstraite", "abstract",
        "globale", "static", "local", "locale",
        NULL
    };
    for (int i = 0; mods[i]; i++)
        if (strcmp(canon, mods[i]) == 0) return 1;
    return 0;
}

/* Un nom de procédure-scénario au sens du runner (testing.c::is_scenario_name).
 * Les trois préfixes sont exécutés comme scénarios. */
static int rule54_is_scenario_name(const char* canon) {
    return strncmp(canon, "scenario_", 9) == 0
        || strncmp(canon, "test_",     5) == 0
        || strncmp(canon, "should_",   7) == 0;
}

#define R54_MAX_DEPTH 256

/* Analyse d'un buffer deja charge. `path` ne sert qu'aux messages ; `r` peut
 * etre NULL (mode sonde, cf. registry_self_test_rule54_probe_depth). Rend la
 * profondeur de blocs atteinte en fin de source — c'est elle que le garde
 * --self-test-block-openers verrouille. */
static int rule54_analyze_buffer(char* buf, const char* path, Reporter* r) {
    int assertions = 0, real = 0, floating = 0;

    /* Pile de blocs FIN-terminés. scenStack[d] = 1 si le bloc à la
     * profondeur d est une PROCEDURE/FONCTION scenario_/test_/should_.
     * Une assertion est "couverte" si un bloc scénario l'englobe. */
    char scenStack[R54_MAX_DEPTH];
    int depth = 0;
    int pendingProc = 0;   /* PROCEDURE/FONCTION vu, on attend le nom */
    int prevWasSinon = 0;  /* dernier mot-clé = SINON/ELSE ? (elif) */
    int sawNL = 0;         /* un saut de ligne depuis le token précédent ? */

    const char* p = buf;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') { sawNL = 1; p++; continue; }
        if (c == '/' && p[1] == '/') { p += 2; while (*p && *p != '\n') p++; continue; }
        if (c == '"')  { p++; while (*p && *p != '"') { if (*p == '\n') sawNL = 1; p++; } if (*p) p++; continue; }
        if (c == '`')  { p++; while (*p && *p != '`') { if (*p == '\n') sawNL = 1; p++; } if (*p) p++; continue; }
        if (rule54_ident_start(c)) {
            const char* s = p;
            p++;
            while (*p && rule54_ident_char((unsigned char)*p)) p++;
            char canon[128];
            scanner_canonicalize_identifier(s, (int)(p - s), canon, sizeof(canon));
            int crossedNL = sawNL;
            sawNL = 0;
            const char* q = p;
            while (*q == ' ' || *q == '\t') q++;

            /* Membre (méthode/propriété) précédé de '.' ou '..' : ce n'est PAS
             * un mot-clé. Ex : `c.Fin()` — `Fin` canonicalise en "fin" mais
             * ferme une méthode, pas un bloc. `req..Valeur` idem. On ne
             * l'interprète ni comme opener/FIN ni comme assertion. */
            if (s > buf && s[-1] == '.') { prevWasSinon = 0; continue; }

            /* Nom attendu après PROCEDURE/FONCTION (sauter les modifieurs). */
            if (pendingProc) {
                if (rule54_is_proc_modifier(canon)) { prevWasSinon = 0; continue; }
                /* Frame procedure : 1 = scenario (scenario_/test_/should_),
                 * 2 = autre procedure. Non-nul => "dans une procedure". */
                int isScen = rule54_is_scenario_name(canon);
                if (depth < R54_MAX_DEPTH) scenStack[depth] = isScen ? 1 : 2;
                depth++;
                pendingProc = 0;
                prevWasSinon = 0;
                continue;
            }
            if (rule54_is_proc_opener(canon)) { pendingProc = 1; prevWasSinon = 0; continue; }
            /* `SINON SI` sur la même ligne = clause elif (un seul FIN pour
             * toute la structure) : ne pas ouvrir de bloc. Un SI séparé de
             * SINON par un saut de ligne est un bloc imbriqué (FIN propre).
             * Ce discriminant-là est un lookbehind : la table ne le voit pas. */
            if ((strcmp(canon, "si") == 0 || strcmp(canon, "if") == 0)
                && prevWasSinon && !crossedNL) {
                prevWasSinon = 0;
                continue;
            }
            /*  — le reste du verdict vient de la source unique, par sa
             * porte TEXTE : ce walker marche sur des caractères bruts, pas sur
             * des jetons. Il portait sa propre liste, qui comptait
             * `SI c ALORS instr` comme un bloc à FIN. Conséquence mesurée :
             * une procédure contenant cette forme n'était jamais dépilée, son
             * cadre restait ouvert, et une assertion réellement flottante
             * placée après elle passait pour couverte — la règle 54 devenait
             * aveugle sur tout le reste du fichier. `constante` manquait aussi
             * (sous-comptage symétrique). */
            {
                int delta = 0;
                if (prescan_depth_delta_text(s, &delta)) {
                    if (delta < 0) { if (depth > 0) depth--; }
                    else if (delta > 0) {
                        if (depth < R54_MAX_DEPTH) scenStack[depth] = 0;
                        depth++;
                    }
                    /* delta == 0 : forme sans FIN, rien à empiler. */
                    prevWasSinon = 0;
                    continue;
                }
            }
            if (rule54_is_extra_block_opener(canon)) {
                if (depth < R54_MAX_DEPTH) scenStack[depth] = 0;
                depth++;
                prevWasSinon = 0;
                continue;
            }

            int is_assert = (strncmp(canon, "dbgverifie", 10) == 0 ||
                             strncmp(canon, "dbgcheck", 8) == 0);
            if (*q == '(' && is_assert) {
                assertions++;
                int tautological = 0;
                if (strcmp(canon, "dbgverifievrai") == 0 ||
                    strcmp(canon, "dbgchecktrue") == 0) {
                    char arg[128], acanon[128];
                    rule54_first_arg(q, arg, sizeof(arg));
                    scanner_canonicalize_identifier(arg, (int)strlen(arg),
                                                    acanon, sizeof(acanon));
                    if (strcmp(acanon, "vrai") == 0 || strcmp(acanon, "true") == 0)
                        tautological = 1;
                }
                if (!tautological) real++;

                /* Une assertion doit vivre dans une PROCEDURE scenario_
                 * (scenario_/test_/should_ — les prefixes que le runner
                 * execute, cf. testing.c::is_scenario_name). Une assertion
                 * hors d'un tel frame est un floating :
                 *   - au top-level du fichier : le runner ré-exécute le
                 *     top-level ET les scenarios → double comptage ;
                 *   - dans une procedure helper NON prefixee scenario_
                 *     (ex : repro_, edge_case_, un helper metier) : le runner
                 *     ne la detecte pas → assertion morte, jamais executee.
                 * Dans les deux cas la verification est illusoire. La
                 * nomenclature scenario_ PUBLIQUE (vitrine) vs PRIVÉE
                 * (edge/interne) est cadree par tests/scenarios/README.md. */
                int inScenario = 0;
                for (int d = 0; d < depth && d < R54_MAX_DEPTH; d++)
                    if (scenStack[d] == 1) { inScenario = 1; break; }
                if (!inScenario) floating++;
            }
            prevWasSinon = (strcmp(canon, "sinon") == 0 || strcmp(canon, "else") == 0);
            continue;
        }
        p++;
    }

    if (r == NULL) return depth;   /* mode sonde : pas de verdict a rendre */

    if (assertions == 0) {
        rep_violation(r, "scenario_no_smoke_only", path,
            "Scenario sans aucune assertion (dbgVérifie* / dbgCheck*). Un "
            "scenario doit verifier au moins une valeur concrete. Pure demo "
            "sans verification : la deplacer hors scenarios/.");
    } else if (real == 0) {
        rep_violation(r, "scenario_no_smoke_only", path,
            "Scenario smoke-only : toutes les assertions sont tautologiques "
            "(dbgVérifieVrai(Vrai,...) / dbgCheckTrue(True,...)). Ajouter au "
            "moins une assertion sur valeur reelle (retour, propriete, etat "
            "observable), sinon la place du fichier est runtime/, pas "
            "scenarios/.");
    }

    if (floating > 0) {
        /* 640 et pas 480 : le libelle seul fait 527 caracteres, la troncature
         * etait latente depuis toujours (elle ne se voyait qu'a l'analyse
         * inter-procedurale, que la scission de cette fonction a debloquee). */
        char detail[640];
        snprintf(detail, sizeof(detail),
            "%d assertion(s) hors d'une PROCEDURE scenario_. Toute assertion "
            "(dbgVérifie* / dbgCheck*) doit vivre dans une PROCEDURE PUBLIQUE "
            "scenario_* (vitrine) ou PROCEDURE PRIVÉE scenario_* (edge/interne), "
            "cf. tests/scenarios/README.md. Une assertion au top-level est "
            "comptée deux fois (le runner ré-exécute le top-level ET les "
            "scénarios) ; une assertion dans une procédure NON préfixée "
            "scenario_ (helper, repro_, edge_case_...) n'est jamais exécutée "
            "par le runner → vérification morte. La déplacer / préfixer "
            "scenario_.",
            floating);
        rep_violation(r, "scenario_assertion_outside_scenario_proc", path, detail);
    }
    return depth;
}

/* Sonde du suivi de profondeur de la regle 54, pour le garde
 * --self-test-block-openers. Un comptage local reintroduit ici
 * ferait diverger cette profondeur du verdict de la source unique. */
int registry_self_test_rule54_probe_depth(const char* source) {
    if (source == NULL) return 0;
    size_t n = strlen(source);
    char* buf = malloc(n + 1);
    if (buf == NULL) return 0;
    memcpy(buf, source, n + 1);
    int depth = rule54_analyze_buffer(buf, "<sonde>", NULL);
    free(buf);
    return depth;
}

static void rule54_analyze_file(const char* path, Reporter* r) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = '\0';
    fclose(f);

    rule54_analyze_buffer(buf, path, r);
    free(buf);
}

static void rule54_scan_dir(const char* dirPath, Reporter* r) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { rule54_scan_dir(full, r); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        size_t nl = strlen(e->d_name);
        if (nl <= 9) continue;
        if (strncmp(e->d_name, "test_", 5) != 0) continue;     /* entrees seules */
        if (strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
        rule54_analyze_file(full, r);
    }
    closedir(d);
}

static void check_rule54_scenario_no_smoke_only(Reporter* r) {
    rule54_scan_dir("tests/scenarios", r);
}

/* ============================================================
 * Rule 55 — completude bilingue des mots-cles de declaration/DSL
 * (, garde mecanique support #219 : "manques recurrents cote
 * anglais").
 *
 * Cartographie (pourquoi boite noire) : les formes EN de ces mots-cles de
 * bloc/type vivent dans `fmt_keywords[]` (src/formatter.c) ; le genre
 * grammatical qui pilote l'article un/une <-> a/an au round-trip EN->FR vit
 * sur la MEME entree depuis  (colonne `genre`) pour les mots-cles, et
 * sur `CONTROL_KINDS[].genre` (src/ui_decl.c) pour les sortes — que la regle 57
 * verifie mecaniquement. Les DEUX tables sont `static`,
 * donc NON exportees — contrairement a `method_norms[]`
 * (owl_formatter_method_norms_at, formatter.h) ou a CONTROL_KINDS
 * (owl_control_kinds_table, ui_decl.h) qui ont un iterateur public. Ajouter
 * un tel iterateur toucherait src/formatter.c, hors perimetre de ce ticket
 * (audit/tests uniquement). Ce check sonde donc les 2 SEULES APIs publiques
 * du formatter (owl_translate/owl_format) pour chaque mot-cle de la
 * matrice ci-dessous — meme methode que src/internal_tests/translate_test.c,
 * mais range ici comme garde de COMPLETUDE (toute nouvelle entree DSL de
 * declaration doit etre ajoutee a cette matrice, sous peine de violation).
 *
 * Deux verifications par entree :
 *   a) forme EN canon appliquee : la forme EN attendue (en_expected) doit
 *      apparaitre dans la traduction EN ; et si elle est distincte du FR, la
 *      forme FR ne doit plus fuiter. Sinon -> trou de table fmt_keywords[]
 *      (cf. F1/F3 ). Les mots-cles a forme EN identique au FR (ex.
 *      Structure) ne testent que la presence.
 *   b) genre enregistre : le round-trip FR->EN->FR doit restaurer le canon
 *      FR exact (article compris). Si le mot n'a pas de genre sur son
 *      enregistrement source, l'article EN->FR degenere en 'a'/'an' fige au
 *      lieu de un/une (cf. F2 ).
 * ============================================================ */
typedef struct {
    const char* label;       /* mot-cle FR sonde (identifiant du cas) */
    const char* en_expected; /* forme EN canon attendue (== label si identique
                              * FR/EN, ex. Structure). Verifiees : ces formes
                              * EN compilent deja (coordinator ). */
    const char* fr_source;   /* declaration FR minimale, canon, parsable */
} DslDeclKeywordCase;

static const DslDeclKeywordCase DSL_DECL_KEYWORDS[] = {
    { "Classe",         "Class",          "x est une Classe\nFIN\n" },
    { "Structure",      "Structure",      "x est une Structure\nFIN\n" },
    { "Combinaison",    "Combination",
      "x est une Combinaison\n\tAlpha\n\tBeta\nFIN\n" },
    { "\xc3\x89num\xc3\xa9ration", "Enumeration",
      "x est une \xc3\x89num\xc3\xa9ration\n\tAlpha = 1\n\tBeta = 2\nFIN\n" },
    { "MenuPrincipal",  "MainMenu",       "MNU est un MenuPrincipal\nFIN\n" },
    { "Etat",           "Report",         "R est un Etat\n\tc est un Corps\n\tFIN\nFIN\n" },
    { "Corps",          "Body",
      "R est un Etat\n\tcorps est un Corps\n\tFIN\nFIN\n" },
    /*  : le NOM du bloc ne doit pas etre l'homonyme exact du mot-cle
     * sonde. Le formatter laisse desormais intact le nom choisi par
     * l'utilisateur (il n'appartient pas au canon), donc un nom homonyme
     * ferait faussement echouer le check (b) de non-fuite de la forme FR —
     * la « fuite » serait le nom, pas le type. Convention deja suivie par
     * l'entree Corps ci-dessus : initiale minuscule. Le check gagne en
     * precision, il ne porte plus que sur la position de TYPE. */
    { "EnTeteDocument", "DocumentHeader",
      "R est un Etat\n\tenTeteDocument est un EnTeteDocument\n\tFIN\nFIN\n" },
    { "HautDePage",     "PageHeader",
      "R est un Etat\n\thautDePage est un HautDePage\n\tFIN\nFIN\n" },
    { "BasDePage",      "PageFooter",
      "R est un Etat\n\tbasDePage est un BasDePage\n\tFIN\nFIN\n" },
    { "FinDeDocument",  "DocumentFooter",
      /* canon FR feminin (une fin de document ; cf. examples/etats/facture). */
      "R est un Etat\n\tfinDeDocument est une FinDeDocument\n\tFIN\nFIN\n" },
};
#define DSL_DECL_KEYWORDS_N \
    (int)(sizeof(DSL_DECL_KEYWORDS) / sizeof(DSL_DECL_KEYWORDS[0]))

static void check_rule55_dsl_declaration_keyword_bilingual_genre(Reporter* r) {
    for (int i = 0; i < DSL_DECL_KEYWORDS_N; i++) {
        const DslDeclKeywordCase* c = &DSL_DECL_KEYWORDS[i];

        char* canonFR = owl_format(c->fr_source);
        char* en = owl_translate(c->fr_source, OWL_TRANSLATE_EN);
        if (!canonFR || !en) {
            rep_violation(r, "dsl_decl_keyword_translate_null", c->label,
                "owl_format/owl_translate a renvoye NULL pour ce mot-cle de "
                "declaration DSL — verifier le parsing de la fixture ou "
                "fmt_keywords[] (src/formatter.c).");
            free(canonFR);
            free(en);
            continue;
        }

        /* (a) forme EN canon appliquee. La forme EN attendue doit apparaitre ;
         * et — si distincte du FR — la forme FR ne doit plus fuiter. Le cas
         * en_expected == label (ex. Structure) ne teste que la presence
         * (trivialement vraie), pas l'absence : leur forme EN EST le mot FR. */
        if (strstr(en, c->en_expected) == NULL) {
            char detail[512];
            snprintf(detail, sizeof detail,
                "translate FR->EN ne produit pas la forme EN canon '%s' pour "
                "le mot-cle '%s' (forme EN absente ou non appliquee). Ajouter "
                "l'entree dans fmt_keywords[] (src/formatter.c).",
                c->en_expected, c->label);
            rep_violation(r, "dsl_decl_keyword_missing_en_form", c->label, detail);
        } else if (strcmp(c->en_expected, c->label) != 0 &&
                   strstr(en, c->label) != NULL) {
            char detail[512];
            snprintf(detail, sizeof detail,
                "translate FR->EN produit '%s' mais laisse aussi fuiter la "
                "forme FR '%s' dans la sortie. Verifier fmt_keywords[] "
                "(src/formatter.c).", c->en_expected, c->label);
            rep_violation(r, "dsl_decl_keyword_fr_form_leaked", c->label, detail);
        }

        /* (b) genre enregistre (article restaure au round-trip). */
        char* back = owl_translate(en, OWL_TRANSLATE_FR);
        if (!back || strcmp(back, canonFR) != 0) {
            char detail[768];
            snprintf(detail, sizeof detail,
                "round-trip FR->EN->FR ne restaure pas le canon pour '%s' "
                "(genre manquant : colonne `genre` de fmt_keywords[], "
                "src/formatter.c, ou de CONTROL_KINDS[], src/ui_decl.c). "
                "canon='%s' round-trip='%s'",
                c->label, canonFR, back ? back : "(null)");
            rep_violation(r, "dsl_decl_keyword_genre_missing", c->label, detail);
        }
        free(back);

        free(canonFR);
        free(en);
    }
}

/* Regle 56 — toute categorie portee par une declaration du registre doit
 * avoir sa paire FR/EN dans owl_categories.def.
 *
 * La categorie est la CLE du filtre `owl list ... --cat <X>` et cette cle est
 * bilingue : sans paire, la forme anglaise ne resout pas et l'en-tete anglais
 * affiche un nom francais. Une categorie ajoutee sans sa paire ne casse rien
 * bruyamment — elle degrade silencieusement le rendu anglais. C'est exactement
 * la trappe que ce chantier a passe des tickets entiers a refermer ailleurs :
 * on la ferme ici par un verrou, pas par une consigne.
 *
 *  — la regle ne voyait que les natives ; les categories de mots-cles
 * (owl_keyword_catalog) et de limites (owl_limits.def) sortaient en francais en
 * --lang en sans qu'aucun controle ne bronche. Elle couvre desormais ces trois
 * familles. (Les operateurs restent hors couverture : leur commande
 * `list operators` porte d'autres libelles non traduits, chantier a part.)
 *
 * La comparaison est byte a byte : la forme FR de la table EST la chaine du
 * champ `category`, pas une variante tolerable. */
static int category_pair_exists(const char* cat) {
    for (int j = 0; j < OWL_CATEGORIES_COUNT; j++) {
        if (strcmp(OWL_CATEGORIES[j].fr, cat) == 0) return 1;
    }
    return 0;
}

static void report_category_pair_missing(Reporter* r, const char* owner,
                                         const char* cat) {
    char detail[512];
    snprintf(detail, sizeof detail,
        "categorie '%s' absente de src/owl_categories.def : la forme "
        "anglaise ne resoudra pas au filtre --cat et l'en-tete anglais "
        "affichera le nom francais. Ajouter la ligne "
        "OWL_CATEGORY(\"%s\", \"<forme EN>\").",
        cat, cat);
    rep_violation(r, "category_pair_missing", owner, detail);
}

static void check_rule56_native_category_pair(Reporter* r) {
    int total = owl_registry_count();
    for (int i = 0; i < total; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d->category || !*d->category) continue;
        if (!category_pair_exists(d->category))
            report_category_pair_missing(r, d->canonical, d->category);
    }

    int nkw = owl_keyword_count();
    for (int i = 0; i < nkw; i++) {
        const OwlKeywordDef* k = owl_keyword_at(i);
        if (!k->category || !*k->category) continue;
        if (!category_pair_exists(k->category))
            report_category_pair_missing(r, token_type_name(k->token),
                                         k->category);
    }
    int nkc = owl_keyword_composite_count();
    for (int i = 0; i < nkc; i++) {
        const OwlKeywordComposite* c = owl_keyword_composite_at(i);
        if (!c->category || !*c->category) continue;
        if (!category_pair_exists(c->category))
            report_category_pair_missing(r, "keyword-composite", c->category);
    }

    int nlim = owl_registry_limit_count();
    for (int i = 0; i < nlim; i++) {
        const OwlLimitDef* L = owl_registry_limit_at(i);
        if (!L || !L->category || !*L->category) continue;
        if (!category_pair_exists(L->category))
            report_category_pair_missing(r, L->name, L->category);
    }
}

/* ============================================================
 * Regle 57 — toute sorte de widget/bloc declare son genre grammatical
 *.
 *
 * Le genre pilote l'article de la declaration en traduction EN->FR (`a Pane`
 * -> `un Volet`). Une sorte sans genre ne casse rien bruyamment : elle rend
 * une phrase mixte `est a Volet`, que rien ne signale. Trois tickets
 * (, , ) ont decouvert l'oubli APRES coup, chacun
 * ajoutant a la main l'entree manquante dans une table du formateur.
 *
 * Depuis  le genre est une colonne de CONTROL_KINDS, donc l'oubli est
 * mecaniquement detectable : c'est ce que fait ce check. Il ne verifie pas la
 * JUSTESSE du genre (indecidable sans dictionnaire) mais sa PRESENCE, qui est
 * le seul mode d'echec observe.
 * ============================================================ */
static void check_rule57_widget_kind_genre(Reporter* r) {
    for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
        if (k->genre != OWL_KIND_GENRE_NONE) continue;
        char ascii[128];
        if (k->nameAscii) snprintf(ascii, sizeof ascii, "\"%s\"", k->nameAscii);
        else              snprintf(ascii, sizeof ascii, "NULL");
        char detail[640];
        snprintf(detail, sizeof detail,
            "sorte '%s' sans genre grammatical : la traduction EN->FR ne peut "
            "pas accorder l'article et rend la forme mixte "
            "\"est a %s\" au lieu de \"est un/une %s\". Ajouter la colonne "
            "genre (8e champ) a son entree CONTROL_KINDS dans src/ui_decl.c : "
            "{\"%s\", %s, \"%s\", %s, ..., descFR, descEN, OWL_KIND_GENRE_M} "
            "pour \"un %s\", OWL_KIND_GENRE_F pour \"une %s\".",
            k->nameFR, k->nameFR, k->nameFR,
            k->nameFR, ascii,
            k->nameEN ? k->nameEN : "", k->is_widget ? "true" : "false",
            k->nameFR, k->nameFR);
        rep_violation(r, "widget_kind_genre_missing", k->nameFR, detail);
    }
}

/* ============================================================
 * Regle 58 : coherence desc <-> formCount des natives polymorphes
 * ============================================================ */

/* Copie lowercase avec whitespace normalise (runs d'espaces/newlines -> 1
 * espace). Indispensable : les desc sont concatenees sur plusieurs lignes de
 * literal C, un "trois formes" peut se retrouver coupe par un saut de ligne
 * source. dst tronque proprement si trop court. */
static void desc_normalize_ws_lower(const char* src, char* dst, int dstSize) {
    int o = 0;
    bool prevSpace = false;
    if (dstSize <= 0) return;
    if (!src) { dst[0] = 0; return; }
    for (const unsigned char* p = (const unsigned char*)src; *p && o < dstSize - 1; p++) {
        unsigned char c = *p;
        if (isspace(c)) {
            if (!prevSpace && o > 0) { dst[o++] = ' '; prevSpace = true; }
        } else {
            dst[o++] = (char)tolower(c);
            prevSpace = false;
        }
    }
    /* strip trailing space eventuel */
    if (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

static const char* form_word_fr(int n) {
    switch (n) {
        case 2: return "deux";  case 3: return "trois"; case 4: return "quatre";
        case 5: return "cinq";  case 6: return "six";   case 7: return "sept";
        case 8: return "huit";  default: return NULL;
    }
}
static const char* form_word_en(int n) {
    switch (n) {
        case 2: return "two";   case 3: return "three"; case 4: return "four";
        case 5: return "five";  case 6: return "six";   case 7: return "seven";
        case 8: return "eight"; default: return NULL;
    }
}

/* "<word> formes" ou "<word> cas" present dans la desc normalisee ? */
static bool desc_mentions_count(const char* normDesc, const char* word,
                                const char* nounA, const char* nounB) {
    char needle[64];
    snprintf(needle, sizeof needle, "%s %s", word, nounA);
    if (strstr(normDesc, needle)) return true;
    snprintf(needle, sizeof needle, "%s %s", word, nounB);
    if (strstr(normDesc, needle)) return true;
    return false;
}

static void check_rule58_polymorphic_form_count(Reporter* r) {
    int n = owl_registry_count();
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical) continue;
        if (d->formCount < 2) continue;   /* opt-in strict : 0/1 = non verifie */

        const char* wfr = form_word_fr(d->formCount);
        const char* wen = form_word_en(d->formCount);
        if (!wfr || !wen) {
            char detail[192];
            snprintf(detail, sizeof detail,
                "formCount=%d hors plage supportee (2..8) pour la regle 58. "
                "Etendre form_word_fr/form_word_en si un tel polymorphisme est reel.",
                d->formCount);
            rep_violation(r, "polymorphic_form_count_desc", d->canonical, detail);
            continue;
        }

        char normFR[2048], normEN[2048];
        desc_normalize_ws_lower(d->descFR, normFR, sizeof normFR);
        desc_normalize_ws_lower(d->descEN, normEN, sizeof normEN);

        if (!desc_mentions_count(normFR, wfr, "formes", "cas")) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "formCount=%d mais descFR ne mentionne pas '%s formes' (ni '%s cas'). "
                "Le dispatch gere %d formes d'operandes -> la description user-facing "
                "doit les annoncer, sinon `owl explain` ment. Mettre a jour descFR ou "
                "corriger formCount.", d->formCount, wfr, wfr, d->formCount);
            rep_violation(r, "polymorphic_form_count_desc", d->canonical, detail);
        }
        if (!desc_mentions_count(normEN, wen, "forms", "cases")) {
            char detail[256];
            snprintf(detail, sizeof detail,
                "formCount=%d mais descEN ne mentionne pas '%s forms' (ni '%s cases'). "
                "Aligner descEN sur le nombre de formes reellement gerees.",
                d->formCount, wen, wen);
            rep_violation(r, "polymorphic_form_count_desc", d->canonical, detail);
        }
    }
}

/* ============================================================
 * Regle 61 : scenario_enum_value_coverage (, raffinee )
 *
 * Opt-in par native. Un OwlParam contraint par un enum (allowedBase) ET
 * marque requireEnumValueCoverage=true exige qu'AU MOINS un scenario public
 * (@sujet:<native>) exerce chaque DIMENSION DE BRANCHEMENT de l'enum. Attrape
 * le trou "un cas de branchement jamais exerce -> default silencieux, lot vert
 * trompeur" (classe du bug de cadrage EC2,  : awsEC2 tombait dans le
 * default du switch, aucun scenario ne l'exercait).
 *
 * Deux modes de couverture :
 *   - PAR VALEUR (defaut) : chaque valeur de l'enum doit etre exercee.
 *   - PAR PROTOCOLE (enum awsService, ) : la vraie dimension de
 *     branchement d'AWSRequete n'est pas le service (419 au catalogue complet)
 *     mais le switch (svc->protocol) de fn_aws_requete. On exige donc qu'au
 *     moins un service de CHAQUE protocole GERE present dans la table soit
 *     exerce. Un service par valeur n'echelle pas ; un service par protocole
 *     gere, si. rest-xml / unknown tombent dans le default du switch -> non
 *     geres -> exclus de la couverture exigee (pas de faux positif).
 *
 * Volontairement PAS applique a tous les enums : les enums descriptifs
 * (typeMime, algoHachage — 30+ valeurs sans branchement) produiraient un
 * deluge de faux positifs. L'opt-in declaratif cible les enums a branchement.
 *
 * Detection : scan textuel/token des test_*.owl (recursif) de tests/scenarios/
 * dont le @sujet resout vers la native. Une valeur est couverte si son
 * identifiant apparait comme token isole (bornes non-identifiantes) dans
 * l'un de ces fichiers. Pas de parse complet — suffisant et robuste.
 * ============================================================ */

/* Protocoles AWS reellement GERES par le generique AWSRequete : les cases
 * explicites du switch (svc->protocol) de fn_aws_requete
 * (src/stdlib_aws_native.c). Seul le default (AWS_PROTO_UNKNOWN) n'est PAS
 * gere -> exclu de la couverture exigee. A GARDER SYNCHRO avec ce switch :
 * ajouter un case la-bas => ajouter une entree ici. */
typedef struct { AwsProtocol proto; const char* label; } R61AwsManagedProto;
static const R61AwsManagedProto R61_AWS_MANAGED[] = {
    { AWS_PROTO_JSON,      "json" },
    { AWS_PROTO_QUERY,     "query" },
    { AWS_PROTO_EC2,       "ec2" },
    { AWS_PROTO_REST_JSON, "rest-json" },
    { AWS_PROTO_REST_XML,  "rest-xml" },
};
static const int R61_AWS_MANAGED_COUNT =
    (int)(sizeof(R61_AWS_MANAGED) / sizeof(R61_AWS_MANAGED[0]));

static bool r61_is_ident_char(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 0x80;
}

/* Le fichier a-t-il un @sujet (scanne sur ~30 lignes) qui resout vers
 * `target` (case + accent insensible via owl_registry_find) ? */
static bool r61_file_sujet_matches(const char* path, const OwlNativeDef* target) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char line[1024];
    char sujet[512];
    sujet[0] = '\0';
    for (int i = 0; i < 30 && fgets(line, sizeof(line), f); i++) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] != '/' || p[1] != '/') continue;
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "@sujet", 6) != 0) continue;
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        size_t cp = strlen(p);
        if (cp >= sizeof(sujet)) cp = sizeof(sujet) - 1;
        memcpy(sujet, p, cp);
        sujet[cp] = '\0';
        break;
    }
    fclose(f);
    if (!sujet[0]) return false;

    /* Split sur virgule ; chaque token (eventuellement `FR/EN`) resolu. */
    char* p = sujet;
    while (*p) {
        char* comma = strchr(p, ',');
        char tok[256];
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        if (tlen >= sizeof(tok)) tlen = sizeof(tok) - 1;
        memcpy(tok, p, tlen);
        tok[tlen] = '\0';
        rule47_trim_token(tok);
        char* slash = strchr(tok, '/');
        if (slash) { *slash = '\0'; rule47_trim_token(tok); }
        if (tok[0] && owl_registry_find(tok) == target) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

/* `tok` apparait-il comme identifiant isole dans `text` (bornes gauche/droite
 * non-identifiantes) ? */
static bool r61_text_contains_token(const char* text, const char* tok) {
    size_t tl = strlen(tok);
    if (tl == 0) return false;
    const char* hay = text;
    for (;;) {
        const char* m = strstr(hay, tok);
        if (!m) return false;
        bool left_ok  = (m == text) || !r61_is_ident_char((unsigned char)m[-1]);
        bool right_ok = !r61_is_ident_char((unsigned char)m[tl]);
        if (left_ok && right_ok) return true;
        hay = m + 1;
    }
}

/* Lit tout le fichier en buffer heap (NUL-termine). Caller free(). */
static char* r61_slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static void r61_scan_dir(const char* dirPath, const OwlNativeDef* nat,
                         const char* const* values, bool* covered, int nval) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirPath, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            r61_scan_dir(full, nat, values, covered, nval);
        } else if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(e->d_name);
            if (nl <= 9) continue;
            if (strncmp(e->d_name, "test_", 5) != 0) continue;
            if (strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
            if (!r61_file_sujet_matches(full, nat)) continue;
            char* text = r61_slurp(full);
            if (!text) continue;
            for (int i = 0; i < nval; i++) {
                if (!covered[i] && r61_text_contains_token(text, values[i]))
                    covered[i] = true;
            }
            free(text);
        }
    }
    closedir(d);
}

static void check_rule61_scenario_enum_value_coverage(Reporter* r) {
    int nn = owl_registry_count();
    for (int i = 0; i < nn; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical || !d->signatures) continue;
        /* Collecte des enums opt-in de cette native (dedoublonne : un meme
         * enum peut apparaitre sur plusieurs signatures). */
        const OwlEnum* seen[16];
        int nseen = 0;
        for (const OwlSignature* s = d->signatures; s->returnType; s++) {
            if (!s->params) continue;
            for (const OwlParam* p = s->params; p->name; p++) {
                if (!p->requireEnumValueCoverage || !p->allowedBase) continue;
                const OwlEnum* e = p->allowedBase;
                bool dup = false;
                for (int k = 0; k < nseen; k++)
                    if (seen[k] == e) { dup = true; break; }
                if (dup) continue;
                if (nseen < (int)(sizeof(seen)/sizeof(seen[0]))) seen[nseen++] = e;

                if (!e->values) continue;
                int nval = 0;
                while (e->values[nval]) nval++;
                if (nval == 0) continue;
                bool* covered = (bool*)calloc((size_t)nval, sizeof(bool));
                if (!covered) continue;
                r61_scan_dir("tests/scenarios", d, e->values, covered, nval);

                /* Mode PAR PROTOCOLE pour awsService : la valeur d'enum indexe
                 * AWS_SERVICES (meme ordre, cf. aws_service_models.c). On exige
                 * une couverture par protocole gere present, pas par service. */
                bool byProtocol = (strcmp(e->name, "awsService") == 0) &&
                                  (nval == AWS_SERVICE_COUNT);
                if (byProtocol) {
                    for (int m = 0; m < R61_AWS_MANAGED_COUNT; m++) {
                        AwsProtocol proto = R61_AWS_MANAGED[m].proto;
                        bool present = false, satisfied = false;
                        for (int v = 0; v < nval; v++) {
                            if (AWS_SERVICES[v].protocol != proto) continue;
                            present = true;
                            if (covered[v]) { satisfied = true; break; }
                        }
                        if (!present || satisfied) continue;
                        char detail[600];
                        snprintf(detail, sizeof(detail),
                            "native '%s' : aucun service de protocole '%s' "
                            "(gere par le switch de fn_aws_requete) n'est exerce "
                            "par un scenario @sujet:%s de tests/scenarios/. Un "
                            "protocole de branchement jamais teste passe au vert "
                            "par defaut (classe du trou EC2). Ajouter un scenario "
                            "qui invoque %s avec un service de protocole '%s'.",
                            d->canonical, R61_AWS_MANAGED[m].label, d->canonical,
                            d->canonical, R61_AWS_MANAGED[m].label);
                        rep_violation(r, "scenario_enum_value_coverage",
                                      d->canonical, detail);
                    }
                    free(covered);
                    continue;
                }

                for (int v = 0; v < nval; v++) {
                    if (covered[v]) continue;
                    char detail[600];
                    snprintf(detail, sizeof(detail),
                        "native '%s' : la valeur d'enum '%s' (de %s) n'est "
                        "exercee par AUCUN scenario @sujet:%s de "
                        "tests/scenarios/. Un cas de branchement jamais teste "
                        "passe au vert par defaut (classe du trou EC2). Ajouter "
                        "un scenario qui invoque %s avec %s.",
                        d->canonical, e->values[v], e->name, d->canonical,
                        d->canonical, e->values[v]);
                    rep_violation(r, "scenario_enum_value_coverage",
                                  d->canonical, detail);
                }
                free(covered);
            }
        }
    }
}

/* ============================================================
 * Regle 62 : table de cross-references declaratives fiche->guide
 *
 *  : la table OWL_CROSSREF_TABLE (cli_doc.c) epingle des liens
 * symbole->guide. Chaque entree doit pointer vers (a) un symbole existant dans
 * un registre ET (b) un stem de guide effectivement embarque. Un lien mort
 * (symbole renomme/supprime, guide renomme) = violation -> build rouge.
 * ============================================================ */
static void check_rule62_crossref_table_valid(Reporter* r) {
    int n = cli_doc_crossref_count();
    for (int i = 0; i < n; i++) {
        const char* symbol = NULL;
        const char* stem   = NULL;
        if (!cli_doc_crossref_at(i, &symbol, &stem)) continue;
        int sym_ok = 0, stem_ok = 0;
        if (cli_doc_crossref_entry_valid(symbol, stem, &sym_ok, &stem_ok)) continue;
        char detail[256];
        if (!sym_ok && !stem_ok) {
            snprintf(detail, sizeof(detail),
                     "cross-ref : symbole '%s' introuvable au registre ET stem "
                     "de guide '%s' non embarque.", symbol ? symbol : "(null)",
                     stem ? stem : "(null)");
        } else if (!sym_ok) {
            snprintf(detail, sizeof(detail),
                     "cross-ref : symbole '%s' introuvable au registre.",
                     symbol ? symbol : "(null)");
        } else {
            snprintf(detail, sizeof(detail),
                     "cross-ref : stem de guide '%s' non present dans "
                     "EMBEDDED_PUBLIC_DOCS.", stem ? stem : "(null)");
        }
        rep_violation(r, "crossref_table_dangling", symbol ? symbol : stem,
                      detail);
    }
}

/* ============================================================
 * Entry point
 * ============================================================ */

/* ============================================================
 * Regle 63 : natives fantomes citees dans la doc embarquee
 *
 * La prose des fiches .md de docs/public_documentation est la source de la doc
 * embarquee servie par `owl explain` / `owl search` / le MCP. Elle ne
 * doit pas citer, COMME native/fonction, un symbole absent du registre :
 * un nom invente est recopie dans le hint MCP et fait boucler un agent
 * (surtout un modele local) qui le prend pour argent comptant.
 *
 * Detecteur partage owl_doc_phantom_detect (doc_phantom_lint.h), calibre
 * sur tests/doc-lint-fixtures/ via --self-test-doc-phantom-lint. Les 3
 * heuristiques sont documentees dans tests/doc-lint-fixtures/CALIBRATION.md.
 * ============================================================ */

static bool dpl_is_ident_byte(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 0x80;
}

static bool dpl_is_titlecase(const char* name) {
    unsigned char c = (unsigned char)name[0];
    return (c >= 'A' && c <= 'Z') || c >= 0x80;
}

/* Une methode publique (Majuscule, Entraine, ArgMax...) porte-t-elle ce nom ? */
static bool dpl_method_known(const char* name) {
    char q[256];
    owl_normalize_name(name, q, sizeof q);
    int n = method_catalog_count();
    for (int i = 0; i < n; i++) {
        const char* tn = NULL;
        const MethodEntry* m = method_catalog_at(i, &tn);
        if (m && m->name && owl_name_matches(m->name, q)) return true;
    }
    return false;
}

/* Une propriete DSL (Nom, Source, Largeur...) porte-t-elle ce nom (FR/EN) ? */
static bool dpl_dsl_prop_known(const char* name) {
    char q[256];
    owl_normalize_name(name, q, sizeof q);
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* dp = owl_dsl_prop_at(i);
        if (!dp || !dp->def) continue;
        if (dp->def->nameFR && owl_name_matches(dp->def->nameFR, q)) return true;
        if (dp->def->nameEN && owl_name_matches(dp->def->nameEN, q)) return true;
    }
    return false;
}

/* Vocabulaire doc-legitime que les catalogues runtime n'exposent PAS sous la
 * forme citee en prose. Trois familles (cf. , section lookup) :
 *   - types primitifs builtin (chaine, entier, JSON...) : ils vivent dans la
 *     VM, pas dans owl_registry_type_find ; on interroge le catalogue primitif
 *     du CLI (source unique, pas de duplication de la liste) ;
 *   - vocabulaire structurel DSL non tokenise par le scanner (PROPRIETES,
 *     RESTREINT, blocs ServiceREST/OperationHTTP) et types SQL cites dans les
 *     fiches connecteurs — table interne curatee ;
 *   - evenements en forme courte (Clic, Modification, Ouverture...) et
 *     quelques proprietes de type nommees en backtick sans prefixe `..`.
 * Aucun de ces tokens n'est une native fantome : les reconnaitre evite un
 * faux positif du lint. */
static bool dpl_doc_vocab_known(const char* name) {
    char q[256];
    owl_normalize_name(name, q, sizeof q);

    int np = cli_doc_primitive_count();
    for (int i = 0; i < np; i++) {
        const char *fr = NULL, *asc = NULL, *en = NULL;
        cli_doc_primitive_at(i, &fr, &asc, &en);
        if ((fr  && owl_name_matches(fr,  q)) ||
            (asc && owl_name_matches(asc, q)) ||
            (en  && owl_name_matches(en,  q))) return true;
    }

    static const char* EXTRA[] = {
        /* mots-cles structurels DSL absents du catalogue keyword runtime */
        "PROPRIETES", "PROPRIÉTÉS", "PROPERTIES", "RESTREINT",
        "COLONNES", "COLUMNS", "ServiceREST", "OpérationHTTP", "OperationHTTP",
        /* types SQL cites dans les fiches connecteurs (13/14_sql) */
        "VARBINARY", "NVARCHAR", "VARCHAR", "NCHAR", "CHAR", "TEXT", "NTEXT",
        "INT", "BIGINT", "SMALLINT", "TINYINT", "BIT", "DECIMAL", "NUMERIC",
        "FLOAT", "REAL", "MONEY", "DATETIME", "DATETIME2", "SMALLDATETIME",
        "TIMESTAMP", "BINARY", "IMAGE", "UNIQUEIDENTIFIER", "XML",
        /* evenements cites en forme courte dans les fiches lifecycle/test */
        "Clic", "DoubleClic", "Modification", "Ouverture", "Fermeture",
        "Initialisation", "Survol", "Molette", "Saisie", "Selection", "Sélection",
        /* proprietes de type nommees en backtick sans prefixe `..` */
        "Source", "RépondreÀ", "RepondreA",
        NULL
    };
    for (int i = 0; EXTRA[i]; i++)
        if (owl_name_matches(EXTRA[i], q)) return true;
    return false;
}

/* Allowlist des identifiants d'EXEMPLE que les 3 regles ne peuvent pas
 * distinguer structurellement d'une native fantome (cf. , section
 * allowlist). Echappatoire standard d'un lint : inventions pedagogiques,
 * concepts retires, technos externes. Toute entree est justifiee ; garder
 * cette liste COURTE — un fantome reel ne doit jamais s'y glisser. */
static bool dpl_allowlisted(const char* name) {
    static const char* ALLOW[] = {
        /* identifiants d'exemple de tutoriels (getters/setters, placeholders) */
        "CalculerTVA",      /* 12_prefixes : nom de procedure d'exemple */
        "Solde", "DefinirSolde", "LargeurChange", /* 06_poo : getter/setter d'exemple */
        "Foo",              /* 15_lifecycle_champs : placeholder d'appel */
        "f",                /* 10_conventions / 17_init : fonction placeholder */
        "maProc",           /* 17_init : procedure utilisateur d'exemple */
        /* concept retire du langage */
        "Main",             /*  : plus de Main() auto-appele */
        /* technos / marques externes citees en prose */
        "View",             /* 18_layouts : composant React (analogie) */
        "OWL",              /* nom du langage / namespace `OWL.`, pas une native */
        NULL
    };
    char q[256];
    owl_normalize_name(name, q, sizeof q);
    for (int i = 0; ALLOW[i]; i++)
        if (owl_name_matches(ALLOW[i], q)) return true;
    return false;
}

/* Symbole connu d'OWL, toutes familles + formes FR/EN. Le lint ne flag QUE
 * ce qui n'est nulle part : natif, type, const/valeur d'enum, enum, mot-cle,
 * variable globale predefinie, event publie, methode, propriete DSL, plus le
 * vocabulaire doc-legitime hors catalogues runtime (dpl_doc_vocab_known). */
static bool dpl_symbol_known(const char* name) {
    if (!name || !*name) return false;
    if (owl_registry_find(name)) return true;
    if (owl_registry_type_find(name)) return true;
    if (owl_registry_const_find_bilingual(name)) return true;
    if (owl_registry_enum_find(name)) return true;
    if (owl_keyword_find_by_word(name)) return true;
    if (owl_globalvar_catalog_find(name)) return true;
    if (owl_event_catalog_find_any(name)) return true;
    if (dpl_method_known(name)) return true;
    if (dpl_dsl_prop_known(name)) return true;
    if (dpl_doc_vocab_known(name)) return true;
    return false;
}

/* Extrait le nom (run d'identifiant en tete) du contenu d'un token
 * backtick. Retourne la longueur ecrite dans out (0 si vide). */
static size_t dpl_extract_name(const char* content, char* out, size_t outSz) {
    size_t n = 0;
    const char* p = content;
    while (*p && dpl_is_ident_byte((unsigned char)*p) && n + 1 < outSz) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n;
}

/* Acces propriete ".." (invariant 6) dans le contenu d'un token ? On ne
 * reconnait que la forme "<ident>..y" ou le ".." en tete (`..Source`) : le
 * ".." doit toucher un identifiant. Ainsi l'ellipse d'appel `Nom(...)` (ou le
 * ".." est colle a une parenthese, pas a un identifiant) n'est PAS confondue
 * avec un acces propriete. */
static bool dpl_is_property_ref(const char* content) {
    if (content[0] == '.' && content[1] == '.') return true;   /* ..Source */
    for (const char* p = content; p[0] && p[1] && p[2]; p++) {
        if (dpl_is_ident_byte((unsigned char)p[0]) &&
            p[1] == '.' && p[2] == '.') return true;           /* req..URL */
    }
    return false;
}

static int dpl_line_of(const char* buf, size_t off) {
    int line = 1;
    for (size_t i = 0; i < off; i++) if (buf[i] == '\n') line++;
    return line;
}

/* La phrase autour de tokenOff contient-elle "native"/"fonction" ?
 * Bornes de phrase : '.'/'!'/'?' suivi d'un blanc, saut de paragraphe
 * (\n\n), ou bornes du buffer. Casse-insensible (mots sans accent). */
static bool dpl_sentence_has_native_word(const char* buf, size_t tokenOff) {
    size_t len = strlen(buf);
    size_t start = 0;
    if (tokenOff > 0) {
        for (size_t i = tokenOff; i-- > 0; ) {
            char c = buf[i];
            if (c == '.' || c == '!' || c == '?') {
                char nxt = buf[i + 1];
                if (nxt == ' ' || nxt == '\t' || nxt == '\n' || nxt == '\r') {
                    start = i + 1; break;
                }
            }
            if (c == '\n' && i > 0 && buf[i - 1] == '\n') { start = i + 1; break; }
        }
    }
    size_t end = len;
    for (size_t i = tokenOff; i < len; i++) {
        char c = buf[i];
        if (c == '.' || c == '!' || c == '?') {
            char nxt = buf[i + 1];
            if (nxt == '\0' || nxt == ' ' || nxt == '\t' || nxt == '\n' || nxt == '\r') {
                end = i; break;
            }
        }
        if (c == '\n' && buf[i + 1] == '\n') { end = i; break; }
    }
    static const char* WORDS[] = { "native", "fonction", NULL };
    for (size_t i = start; i < end; i++) {
        for (int w = 0; WORDS[w]; w++) {
            size_t k = 0;
            while (WORDS[w][k] && i + k < end &&
                   tolower((unsigned char)buf[i + k]) == WORDS[w][k]) k++;
            if (WORDS[w][k] == '\0') return true;
        }
    }
    return false;
}

static void dpl_add_hit(OwlDocPhantomHit* hits, int* count, int maxHits,
                        const char* name, int line) {
    for (int i = 0; i < *count; i++) {
        if (strcmp(hits[i].name, name) == 0) return;   /* dedup par nom */
    }
    if (*count >= maxHits) return;
    snprintf(hits[*count].name, OWL_DOC_PHANTOM_NAME_MAX, "%s", name);
    hits[*count].line = line;
    (*count)++;
}

/* Regle 3 : tente de parser une enumeration parenthesee `(`A`, `B`)` a
 * partir de l'offset de la parenthese ouvrante. Flag les membres inconnus
 * si au moins un membre est un native reel (distingue "liste de callables"
 * d'une "liste de types/champs"). Read-only : n'avance pas l'index appelant. */
static void dpl_parse_enum(const char* work, size_t len, size_t openParen,
                           OwlDocPhantomHit* hits, int* count, int maxHits) {
    struct { size_t off; bool dot; char name[OWL_DOC_PHANTOM_NAME_MAX]; } mem[32];
    int mn = 0;
    size_t p = openParen + 1;
    for (;;) {
        while (p < len && (work[p]==' '||work[p]=='\t'||work[p]=='\n'||work[p]=='\r')) p++;
        if (p >= len) return;
        if (work[p] == ')') break;
        if (work[p] != '`') return;   /* contenu non-token -> pas une enum propre */
        size_t ts = p;
        size_t q = p + 1;
        while (q < len && work[q] != '`' && work[q] != '\n') q++;
        if (q >= len || work[q] != '`') return;
        if (mn < 32) {
            size_t clen = q - (ts + 1);
            char content[256];
            size_t cc = clen < sizeof(content) - 1 ? clen : sizeof(content) - 1;
            memcpy(content, work + ts + 1, cc);
            content[cc] = '\0';
            mem[mn].off = ts;
            mem[mn].dot = dpl_is_property_ref(content);
            dpl_extract_name(content, mem[mn].name, sizeof mem[mn].name);
            mn++;
        }
        p = q + 1;
        while (p < len && (work[p]==' '||work[p]=='\t'||work[p]=='\n'||work[p]=='\r')) p++;
        if (p < len && work[p] == ',') { p++; continue; }
        if (p < len && work[p] == ')') break;
        return;   /* separateur inattendu -> pas une enum propre */
    }
    if (mn < 2) return;
    bool anyNative = false;
    for (int k = 0; k < mn; k++) {
        if (mem[k].name[0] && owl_registry_find(mem[k].name)) { anyNative = true; break; }
    }
    if (!anyNative) return;   /* pas de native -> liste de types/champs, on ignore */
    for (int k = 0; k < mn; k++) {
        if (!mem[k].name[0] || mem[k].dot) continue;
        if (!dpl_symbol_known(mem[k].name) && !dpl_allowlisted(mem[k].name))
            dpl_add_hit(hits, count, maxHits, mem[k].name, dpl_line_of(work, mem[k].off));
    }
}

int owl_doc_phantom_detect(const char* buf, OwlDocPhantomHit* hits, int maxHits) {
    int count = 0;
    if (!buf) return 0;
    size_t len = strlen(buf);
    char* work = malloc(len + 1);
    if (!work) return 0;
    memcpy(work, buf, len + 1);

    /* Neutraliser les blocs de code fences ```...``` : contenu (fences
     * comprises) remplace par des espaces, \n preserves pour garder la
     * numerotation de ligne. */
    {
        bool inFence = false;
        size_t i = 0;
        while (i < len) {
            size_t ls = i;
            size_t le = ls;
            while (le < len && work[le] != '\n') le++;
            size_t pp = ls;
            while (pp < le && (work[pp] == ' ' || work[pp] == '\t')) pp++;
            bool isFence = (le - pp >= 3) &&
                           work[pp] == '`' && work[pp + 1] == '`' && work[pp + 2] == '`';
            if (isFence) {
                for (size_t k = ls; k < le; k++) work[k] = ' ';
                inFence = !inFence;
            } else if (inFence) {
                for (size_t k = ls; k < le; k++) work[k] = ' ';
            }
            i = (le < len) ? le + 1 : le;
        }
    }

    /* Regles 1 & 2 : parcours des tokens backtick. */
    for (size_t i = 0; i < len; i++) {
        if (work[i] != '`') continue;
        size_t tokStart = i;
        size_t j = i + 1;
        while (j < len && work[j] != '`' && work[j] != '\n') j++;
        if (j >= len || work[j] != '`') { i = j; continue; }
        size_t clen = j - (i + 1);
        char content[256];
        size_t cc = clen < sizeof(content) - 1 ? clen : sizeof(content) - 1;
        memcpy(content, work + i + 1, cc);
        content[cc] = '\0';
        i = j;   /* le ++ de boucle repart apres le backtick fermant */

        if (dpl_is_property_ref(content)) continue;
        char name[OWL_DOC_PHANTOM_NAME_MAX];
        if (dpl_extract_name(content, name, sizeof name) == 0) continue;
        bool hasParens = content[strlen(name)] == '(';
        if (dpl_symbol_known(name) || dpl_allowlisted(name)) continue;
        int line = dpl_line_of(work, tokStart);
        if (hasParens) {                                   /* regle 1 */
            dpl_add_hit(hits, &count, maxHits, name, line);
            continue;
        }
        if (dpl_is_titlecase(name) &&                      /* regle 2 */
            dpl_sentence_has_native_word(work, tokStart)) {
            dpl_add_hit(hits, &count, maxHits, name, line);
        }
    }

    /* Regle 3 : enumerations parenthesees hors token backtick. */
    {
        bool inTok = false;
        for (size_t i = 0; i < len; i++) {
            char c = work[i];
            if (c == '`') { inTok = !inTok; continue; }
            if (inTok) continue;
            if (c == '(') dpl_parse_enum(work, len, i, hits, &count, maxHits);
        }
    }

    free(work);
    return count;
}

/* --- Surface production : scan du corpus reel --- */

static void dpl_scan_file(const char* path, Reporter* r) {
    if (strstr(path, "docs/public_documentation/candidats/")) return;
    if (strstr(path, "docs\\public_documentation\\candidats\\")) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    buf[nr] = '\0';
    fclose(f);

    OwlDocPhantomHit hits[OWL_DOC_PHANTOM_MAX_HITS];
    int n = owl_doc_phantom_detect(buf, hits, OWL_DOC_PHANTOM_MAX_HITS);
    for (int i = 0; i < n; i++) {
        char detail[400];
        snprintf(detail, sizeof detail,
            "Symbole '%s' cite comme native ligne %d mais absent du registre "
            "(natives/types/consts/enums). Corriger la prose : un nom inexistant "
            "empoisonne owl search / le hint MCP.",
            hits[i].name, hits[i].line);
        rep_violation(r, "public_doc_phantom_native", path, detail);
    }
    free(buf);
}

static void dpl_scan_dir(const char* dirPath, Reporter* r) {
    DIR* d = opendir(dirPath);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dirPath, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            dpl_scan_dir(full, r);
        } else if (S_ISREG(st.st_mode)) {
            const char* ext = strrchr(e->d_name, '.');
            if (ext && strcmp(ext, ".md") == 0) dpl_scan_file(full, r);
        }
    }
    closedir(d);
}

static void check_rule63_doc_phantom_natives(Reporter* r) {
    dpl_scan_dir("docs/public_documentation", r);
}

/* ============================================================
 * Regle 64 : natives Etats/PDF cablees en build GUI
 *
 * Le module PDF est scinde : les signatures (array DEFS) vivent dans
 * stdlib_pdf_defs.c (Qt-free, enregistre en console ET en GUI, avec impl=NULL
 * cote console) ; les implementations fn_i_* (stdlib_pdf.c, GUI seulement) sont
 * injectees par stdlib_pdf_impls_wire() AVANT l'enregistrement. Un fn_i_*
 * oublie au wiring = une native Etats enregistree avec impl=NULL dans un build
 * GUI -> resolue au compile mais non dispatchable au runtime (undefined
 * silencieux).
 *
 * Detection sans la macro OWL_HAS_GUI (ce fichier est core, compile sans) : si
 * AU MOINS une native de categorie "Etats" porte un impl, on est dans un build
 * ou les impls sont cablees -> TOUTES doivent l'etre. En console (toutes
 * impl=NULL), la regle est un no-op legitime (impl=NULL tolere). */
static void check_rule64_pdf_native_impl_wired(Reporter* r) {
    int n = owl_registry_count();
    bool anyWired = false;
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->category) continue;
        if (strcmp(d->category, "Etats") != 0) continue;
        if (d->impl != NULL) { anyWired = true; break; }
    }
    if (!anyWired) return;  /* build console : impl=NULL tolere */
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->category || !d->canonical) continue;
        if (strcmp(d->category, "Etats") != 0) continue;
        if (d->impl == NULL) {
            rep_violation(r, "pdf_native_impl_unwired", d->canonical,
                "native Etats/PDF sans impl dans un build GUI : ajouter son "
                "cablage dans stdlib_pdf_impls_wire() (stdlib_pdf.c). Sans cela "
                "la native se resout au compile mais n'est pas dispatchable au "
                "runtime (undefined silencieux).");
        }
    }
}

/* ============================================================
 * Regle 65 : natives UI cablees en build GUI
 *
 * Meme split que le PDF (regle 64), applique aux 108 natives UI : les signatures
 * (array DEFS) vivent dans stdlib_ui_defs.c (Qt-free, enregistre en console ET
 * en GUI, impl=NULL cote console) ; les impls fn_* (stdlib_ui.c, GUI seulement)
 * sont injectees par stdlib_ui_impls_wire() AVANT l'enregistrement. Un fn_*
 * oublie au wiring = une native UI enregistree avec impl=NULL dans un build GUI
 * -> resolue au compile mais non dispatchable au runtime (undefined silencieux).
 *
 * Les natives UI melent plusieurs categories (UI, Fichiers, Test, Image,
 * Clavier) dont certaines sont aussi portees par des natives core toujours
 * cablees : impossible de keyer sur la categorie comme la regle 64 (faux positif
 * garanti en console sur fOuvre & co). On interroge donc l'appartenance exacte
 * via stdlib_ui_defs_contains(). Detection du build GUI : si AU MOINS une native
 * UI porte un impl, toutes doivent l'etre. En console (toutes impl=NULL), no-op
 * legitime (impl=NULL tolere). */
static void check_rule65_ui_native_impl_wired(Reporter* r) {
    int n = owl_registry_count();
    bool anyWired = false;
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical) continue;
        if (!stdlib_ui_defs_contains(d->canonical)) continue;
        if (d->impl != NULL) { anyWired = true; break; }
    }
    if (!anyWired) return;  /* build console : impl=NULL tolere */
    for (int i = 0; i < n; i++) {
        const OwlNativeDef* d = owl_registry_at(i);
        if (!d || !d->canonical) continue;
        if (!stdlib_ui_defs_contains(d->canonical)) continue;
        if (d->impl == NULL) {
            rep_violation(r, "ui_native_impl_unwired", d->canonical,
                "native UI sans impl dans un build GUI : ajouter son cablage "
                "dans stdlib_ui_impls_wire() (stdlib_ui.c). Sans cela la native "
                "se resout au compile mais n'est pas dispatchable au runtime "
                "(undefined silencieux).");
        }
    }
}

int registry_self_test_run(FILE* out) {
    Reporter r;
    rep_init(&r, out);

    fputs("{\n  \"violations\": [\n", out);

    check_rule1_canonical_ascii(&r);
    check_rule2_name_en(&r);
    check_rule3_desc(&r);
    check_rule4_const_ascii_alias(&r);
    check_rule5_enum_values(&r);
    check_rule20_enum_must_be_sentinel(&r);
    check_rule5c_widget_kinds_table(&r);
    /* check_rule5d_composed_types : deplacee dans tests/lsp/lsp_test_main.c
     * car OWL_COMPOSED_TYPE_CATALOG vit dans lsp_catalog_gen.c (LSP-only). */
    check_rule6_type_fn_collision(&r);
    check_rule7_dsl_prop_desc(&r);
    check_rule8_dsl_prop_enum_ref(&r);
    check_rule9_dsl_prop_name_en(&r);
    check_rule10_event_catalog(&r);
    check_rule11_field_name_en(&r);
    check_rule12_template_field_undeclared(&r);
    check_rule13_field_storage_ascii(&r);
    check_rule14_field_default_value(&r);
    check_rule16_method_bindings(&r);
    check_rule18_enum_unreferenced(&r);
    check_rule17_dsl_prop_ascii(&r);
    check_rule19_method_norm_redundant(&r);
    check_rule21_public_string_mentions(&r);
    check_rule22_public_files_mentions(&r);
    check_rule23_global_instance_has_typedef(&r);
    check_rule24_method_receiver_info_dense(&r);
    check_rule25_widget_default_property(&r);
    check_rule25b_image_widget_default_property(&r);
    check_rule26_coercible_has_parser(&r);
    check_rule27_param_desc_mentions_enum(&r);
    check_rule28_keyword_coverage(&r);
    check_rule29_keyword_def_valid(&r);
    check_rule30_keyword_sense_complete(&r);
    check_rule31_keyword_composite_valid(&r);
    check_rule32_operator_coverage(&r);
    check_rule33_operator_def_valid(&r);
    check_rule34_operator_sense_complete(&r);
    check_rule35_operator_display_unique(&r);
    check_rule36_globalvar_catalog(&r);
    check_rule37_new_skeleton_compiles(&r);
    check_rule38_template_compiles(&r);
    check_rule39_test_scenario_filename(&r);
    check_rule40_limits_catalog(&r);
    check_rule41_public_files_utf8(&r);
    check_rule42_error_registry_invariants(&r);
    check_rule43_widget_kind_headless_coverage(&r);
    check_rule44_enum_lookup_consistant(&r);
    check_rule45_dispatcher_case_insensible_doubles(&r);
    check_rule46_object_prop_target_type_valid(&r);
    check_rule47_test_scenario_sujet_canonical(&r);
    check_rule49_scenario_pub_metadata_required(&r);
    check_rule48_icones_catalog_coverage(&r);
    check_rule50_no_duplicate_native_canonical(&r);
    check_rule51_name_en_consts_enums_types(&r);  /* digue nameEN active, sans exemption */
    check_rule52_dsl_prop_default(&r);            /* defaut declare + coherent ( Veine Y) */
    check_rule53_scenarios_subdir_whitelist(&r);  /* liste blanche sous-dossiers scenarios */
    check_rule54_scenario_no_smoke_only(&r);      /* anti smoke-only + assertions dans une PROCEDURE scenario_ (, ) */
    check_rule55_dsl_declaration_keyword_bilingual_genre(&r); /* completude bilingue DSL declaration (, support #219) */
    check_rule56_native_category_pair(&r);        /* paire FR/EN de toute categorie du registre */
    check_rule57_widget_kind_genre(&r);           /* genre grammatical de toute sorte de widget/bloc */
    check_rule58_polymorphic_form_count(&r);      /* coherence desc <-> formCount des natives polymorphes */
    check_rule59_declared_field_has_storage(&r);  /* field declare FIELDS_X[] sans slot storage build_X_template */
    check_rule60_callback_signature_valid(&r);    /* contrat de signature des callbacks */
    check_rule61_scenario_enum_value_coverage(&r); /* couverture scenario par valeur d'enum opt-in */
    check_rule62_crossref_table_valid(&r);         /* table cross-ref fiche->guide valide des 2 bouts */
    check_rule63_doc_phantom_natives(&r);          /*  volet B : lint anti-natives-fantomes doc embarquee */
    check_rule64_pdf_native_impl_wired(&r);         /*  : natives Etats/PDF toutes cablees en build GUI */
    check_rule65_ui_native_impl_wired(&r);          /*  : natives UI toutes cablees en build GUI */

    fputs("\n  ],\n", out);
    fprintf(out, "  \"violation_count\": %d,\n", r.count);
    fprintf(out, "  \"ok\": %s,\n", r.count == 0 ? "true" : "false");
    /* Compteurs informationnels pour situer la couverture du test. */
    fprintf(out, "  \"natives_checked\": %d,\n", owl_registry_count());
    fprintf(out, "  \"types_checked\": %d,\n", owl_registry_types_count());
    fprintf(out, "  \"consts_checked\": %d,\n", owl_registry_consts_count());
    fprintf(out, "  \"enums_checked\": %d,\n", owl_registry_enums_count());
    fprintf(out, "  \"dsl_props_checked\": %d,\n", owl_dsl_props_count());
    fprintf(out, "  \"limits_checked\": %d,\n", owl_registry_limit_count());
    fprintf(out, "  \"errors_checked\": %d,\n", owl_registry_error_count());
    print_migration_progress(&r, out);
    /* La derniere ligne du JSON ne doit pas avoir de virgule. */
    fprintf(out, "  \"errors_total_sites\": %d\n",
            MIGRATION_PROGRESS[0].total + MIGRATION_PROGRESS[1].total +
            MIGRATION_PROGRESS[2].total + MIGRATION_PROGRESS[3].total);
    fputs("}\n", out);

    /* Garde ensemble vide. Les 55 regles iterent le registre : si
     * une famille est vide, toutes les regles qui la parcourent bouclent zero
     * fois et rendent "violation_count: 0" sans avoir rien verifie. Un vert
     * obtenu sur du vide est plus couteux qu'un rouge : il ne gene personne.
     * Diagnostic sur stderr pour ne pas casser le JSON de stdout. */
    struct { const char* name; int n; } famille[] = {
        { "natives",   owl_registry_count()        },
        { "types",     owl_registry_types_count()  },
        { "consts",    owl_registry_consts_count() },
        { "enums",     owl_registry_enums_count()  },
        { "dsl_props", owl_dsl_props_count()       },
        { "errors",    owl_registry_error_count()  },
    };
    int vides = 0;
    for (size_t i = 0; i < sizeof famille / sizeof famille[0]; i++) {
        if (famille[i].n == 0) {
            vides++;
            fprintf(stderr, "registry self-test: CORPUS VIDE — famille '%s' a "
                            "0 entree, les regles qui la parcourent n'ont rien "
                            "examine.\n", famille[i].name);
        }
    }

    return r.count + vides;
}
