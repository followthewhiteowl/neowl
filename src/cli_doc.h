// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* cli_doc.h — sous-commandes documentaires CLI d'owl.
 *
 * Toutes les commandes prennent (argc, argv) à partir de argv[0] = nom
 * de la sous-commande (style canon/argp). Elles retournent un code de sortie
 * standard : 0 succès, 1 erreur d'usage, 2 ressource non trouvée.
 *
 * Flags communs reconnus par toutes les commandes (consommés en interne) :
 *   --plain / --no-color : force la sortie sans ANSI
 *   --json               : format JSON structuré
 *   --lang en / --en     : sortie en anglais (fallback FR si traduction absente)
 *   --lang fr / --fr     : sortie en français (défaut)
 */

#ifndef OWL_CLI_DOC_H
#define OWL_CLI_DOC_H

#include "embedded_docs.h"   /* OwlEmbeddedDoc (owl_template_emitted_body) */

#ifdef __cplusplus
extern "C" {
#endif

/* owl help [<commande>] — synopsis général ou détail d'une sous-commande. */
int cli_doc_help(int argc, char** argv);

/* owl primer — affiche la fiche 00_apercu_du_langage. */
int cli_doc_primer(int argc, char** argv);

/* owl learn [<fiche>] — liste les fiches ou affiche une fiche.
 * Alias acceptés : apercu, types, null, operateurs, conventions, test,
 * prefixes (et les noms complets avec préfixe numérique). */
int cli_doc_learn(int argc, char** argv);

/* owl list <quoi> — listes filtrables du registry.
 *   owl list                  : aide (commandes disponibles)
 *   owl list categories       : familles avec comptage
 *   owl list natives [--cat C]: fonctions natives
 *   owl list types            : types composés
 *   owl list constants        : constantes du registry
 * Pas de `list enums` : les énumérations ne sont plus une famille
 * navigable, seules leurs fiches restent servies par explain. */
int cli_doc_list(int argc, char** argv);

/* Validité d'un nom de famille pour `owl list <bucket> --cat <cat>`.
 *
 * `bucket` est le nom canonique de la sous-commande (`natives`, `types`,
 * `constants`, `keywords`, `limits`, `events`, `props`), pas un de ses alias.
 * Le domaine dépend du bucket : catégorie de registre pour les cinq premiers,
 * champ DSL pour `events` et `props`. La résolution bilingue de la clé est
 * faite ici, comme au parse de `--cat`.
 *
 * Rend vrai quand `cat` est vide ou quand `bucket` ne filtre pas par famille :
 * il n'y a alors aucune famille à invalider. Ce n'est pas un blanc-seing pour
 * `--cat` — sur un bucket sans familles, `owl list` refuse l'appel en amont
 * (cf. le prédicat suivant) et ce prédicat n'est jamais ce qui décide. Rend
 * vrai aussi pour une famille valable mais vide — la question a été comprise,
 * la réponse est vide.
 *
 * Source unique du verdict : c'est ce prédicat qui décide du diagnostic
 * imprimé par `owl list`, et c'est lui que les canaux machine interrogent.
 * Aucun consommateur ne relit le texte d'affichage. */
int cli_doc_list_category_is_known(const char* bucket, const char* cat);

/* Le bucket est-il découpé en familles, c'est-à-dire `--cat` a-t-il un sens
 * dessus ? Complément du prédicat ci-dessus, qui répond sur une valeur : les
 * deux ensemble partagent les sous-commandes de `owl list` en deux, sans
 * troisième cas possible — elle filtre et sait nommer une famille inconnue,
 * ou elle ne filtre pas et refuse `--cat`. */
int cli_doc_list_bucket_filters_by_category(const char* bucket);

/* Vrai si `word` (longueur `len`) commence par un préfixe de nommage canon
 * de champ/conteneur UI (`FEN_`, `TABLE_`, `COMBO_`, `SAI_`, …) suivi d'au
 * moins un caractère. Sert au surligneur de code CLI (render_code_line_owl)
 * pour colorer un identifiant comme instance déclarée. La liste des préfixes
 * de champ est dérivée du registre `owl_control_kinds_table()` (.canonicalPrefix)
 * — source unique, pas de liste en dur qui re-diverge. Exposé pour le
 * self-test. */
int cli_doc_looks_like_ui_instance(const char* word, long len);

/* owl explain <symbole> — détail complet d'un symbole.
 * Cherche dans l'ordre : native, type composé, énumération, constante.
 * Joint les scénarios publics dont @sujet matche. */
int cli_doc_explain(int argc, char** argv);

/* owl examples <symbole> — liste les scénarios publics démontrant un symbole. */
int cli_doc_examples(int argc, char** argv);

/* owl search <mot> — recherche transversale (fondations + registry + scénarios). */
int cli_doc_search(int argc, char** argv);

/* owl where <symbole> — reverse lookup. Selon ce qu'est `<symbole>` :
 *   - Propriété DSL (Grisé, Visible, Titre…) → liste les widgets qui
 *     l'acceptent.
 *   - Widget kind (Bouton, Libellé, Corps, …) → indique le contexte
 *     parent où il peut être déclaré.
 *   - Autre (type, native, enum, constante) → message d'orientation
 *     vers `owl explain` qui est plus pertinent. */
int cli_doc_where(int argc, char** argv);

/* owl template [<nom>] — squelettes prêts à l'emploi.
 *   Sans arg              → liste des templates disponibles avec leur
 *                           description et niveau (basique/intermédiaire).
 *   Avec un nom           → imprime le code prêt à coller sur stdout.
 *                           Les lignes meta (// @nom:, // @description:,
 *                           // @niveau:) du header sont strippées.
 *   --json                → métadonnées + code en objet structuré. */
int cli_doc_template(int argc, char** argv);

/* owl gotchas — fiche des pièges du langage qui ne se devinent pas.
 * Répond au retour beta-test : un agent IA qui découvre OWL trébuche
 * sur des conventions (préfixer les variables, .. vs ., 0n pour exact,
 * owl test charge tout le projet, etc.). Format identique à primer. */
int cli_doc_gotchas(int argc, char** argv);

/* owl cheatsheet — aide-mémoire de syntaxe condensé.
 * One-liners pour les constructs courants (variable, condition, boucle,
 * procédure, classe, tableau, JSON, exception). Complémentaire au primer.
 * Format identique à primer. */
int cli_doc_cheatsheet(int argc, char** argv);

/* Itération publique des types primitifs (table hardcoded dans cli_doc.c).
 * Permet aux self-tests de vérifier que chaque primitif est routable via
 * le MCP sans dupliquer la liste. Retourne {name, nameAscii, nameEN} via
 * pointeurs out — chaque NULL = pas demandé. Renvoie 0 si i hors borne. */
int cli_doc_primitive_count(void);
int cli_doc_primitive_at(int i, const char** out_name,
                                const char** out_nameAscii,
                                const char** out_nameEN);

/* Variante riche pour le dump doc HTML : expose aussi descFR/descEN afin
 * d'émettre les primitifs comme fiches browsables au même format que les
 * types composés. Chaque out NULL = pas demandé. Renvoie 0 si i hors borne. */
int cli_doc_primitive_doc_at(int i, const char** out_name,
                                    const char** out_nameAscii,
                                    const char** out_nameEN,
                                    const char** out_descFR,
                                    const char** out_descEN);

/* Itération publique de la table des catégories d'icones (OWL_ICON_CATEGORIES
 * dans cli_doc.c). Permet aux self-tests de scanner les labels `desc`
 * user-facing (regle 7) sans dupliquer la table. Chaque out NULL = pas
 * demandé. Renvoie 0 si i hors borne. */
int cli_doc_icon_category_count(void);
int cli_doc_icon_category_at(int i, const char** out_owl_name,
                                    const char** out_desc);

/* owl icons <verb> [opts] — exploration du catalogue d'icones Iconify embarque.
 * Sous-verbes :
 *   owl icons                              aide listant les 3 verbes + flags
 *   owl icons list                         refuse, propose les 12 sets disponibles
 *   owl icons list --set <nom> [--limit N] liste les icones d'un set (defaut 50)
 *   owl icons search <mot> [<mot2>...]     substring AND case+accent insensible
 *   owl icons search <mot> --preview       en plus, thumbnails 24px en regard
 *   owl icons preview <nom> [--size N]     rendu Unicode+ANSI24 (16-128, defaut 48)
 *   owl icons preview <nom> --ascii        fallback rampe luminance pour pipes
 *
 * Codes de sortie : 0 succes, 1 erreur d'usage, 2 ressource non trouvee. */
int cli_doc_icons(int argc, char** argv);

/* owl __complete <position> [<prefix>] — backend pour tab completion shell.
 * Dump une complétion par ligne sur stdout, sans couleur ni formatage.
 *   position == ""        ou absent → liste les sous-commandes top-level
 *   position == "list"               → sous-commandes de `owl list` (LIST_CATS)
 *   position == "learn" ou "primer"  → aliases + noms de fiches
 *   position == "explain" ou "examples" → noms du registry (natives+types+enums+consts)
 *   position == "help"               → sous-commandes pour lesquelles `help` a du contenu
 *   autre                            → rien (mot libre, le shell remplit)
 * <prefix> filtre les complétions à celles qui commencent par <prefix>
 * (case-insensitive). */
int cli_doc_complete(int argc, char** argv);

/*  — émetteurs JSON réutilisés par le générateur de doc HTML
 * (main.c dump_doc_html). Fragments à insérer dans un objet symbole déjà
 * ouvert (rien émis si vide) :
 *   examples  : ,"examples":[{title,sub,code}, …]
 *   neighbors : ,"neighbors":[…]  (voisins registry de même catégorie) */
struct OwlNativeDef;
struct OwlTypeDef;
void cli_doc_emit_examples_json(const char* name, const char* name_ascii);
void cli_doc_emit_neighbors_json(const struct OwlNativeDef* d);
/*  — liens fiche->guide (declaratifs + heuristique) :
 *   ,"public_documentation":[<stem>, …]  (rien émis si aucun lien) */
void cli_doc_emit_public_docs_json(const char* canon, const char* canon_ascii);
/*  — liens fiche-propriete DSL->guide, emis dans DSLPROPS :
 *   ,"public_documentation":[<stem>, …]  (rien émis si aucun lien) */
void cli_doc_emit_dsl_prop_public_docs_json(const char* propNameFR,
                                            const char* propNameEN);

/*  — table de cross-references declaratives fiche->guide.
 * Iteration pour le self-test registry (rule 62) et le test negatif. */
int cli_doc_crossref_count(void);
int cli_doc_crossref_at(int i, const char** out_symbol, const char** out_stem);
/* Predicat de validite d'une entree, partage entre le self-test et le test
 * negatif : 1 si `symbol` resout dans un registre ET `stem` est un guide
 * embarque. out_symbol_ok / out_stem_ok (NULL accepte) detaillent quel bout
 * casse. */
int cli_doc_crossref_entry_valid(const char* symbol, const char* stem,
                                 int* out_symbol_ok, int* out_stem_ok);
/*  — "utilisé par" d'un type composé, pour la fiche HTML :
 *   ,"used_by":[{"name","kind"}, …],"used_by_total":N,"used_by_family":"Cat"|null
 * Rien émis si le type n'a aucun consommateur. */
void cli_doc_emit_usedby_json(const struct OwlTypeDef* t);

/*  — corps émis d'un template embarqué (`owl template <nom>`) :
 * header meta strippé, EOL en '\n' nu, contenu canonique au sens du
 * formatter. Chaîne allouée, caller free(). NULL si template vide.
 * Source unique des trois sorties (texte, -o, --json) et du self-test. */
char* owl_template_emitted_body(const OwlEmbeddedDoc* t);

#ifdef __cplusplus
}
#endif

#endif /* OWL_CLI_DOC_H */
