// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_prescan_h
#define owl_prescan_h

#include "common.h"
#include "scanner.h"   /* Token / Scanner : signature de prescan_depth_delta */

/* ================================================================
 * SYMBOL KINDS
 * ================================================================ */

typedef enum {
    SYM_PROCEDURE,
    SYM_FUNCTION,
    SYM_CLASS,
    SYM_STRUCT,
    SYM_INTERFACE,
    SYM_ENUM,
    SYM_CONSTANT,
    SYM_VARIABLE,
    /*  : Fenetre DSL top-level (FEN_X). Distinct de SYM_VARIABLE pour
     * permettre la pre-population de g_prescan_widgets cross-fichier (le slot
     * multilingue d'un FEN voisin doit ancrer le type cible des litteraux
     * inline `FEN_X..Titre = {...}`). Comportement runtime identique a
     * SYM_VARIABLE (global string expose par vm_window_register). */
    SYM_WINDOW
} SymbolKind;

/* ================================================================
 * PROJECT SYMBOL
 * ================================================================ */

typedef struct {
    char name[128];
    SymbolKind kind;
    int fileIndex;      /* index into Project.files */
    bool isPrivate;     /* preceded by PRIVE */
    /*  : line + column de la declaration au top-level. Permet le
     * pointage outer dans le message d'erreur shadow cross-file. 0 si
     * indisponible (chemin de prescan qui n'a pas le Token a portee). */
    int line;
    int column;
} ProjectSymbol;

/*  : declaration d'un ELEMENT top-level (Fenetre, Etat, Classe,
 * CollectionDeProcedures, Theme, Structure, Enumeration, Combinaison,
 * Interface). Registre dedie, distinct des symboles : sert au check d'unicite
 * de nom cross-fichier tous types confondus. Deux elements de meme nom canon
 * (casse + accent insensibles) dans un meme projet = doublon, quel que soit
 * leur type. On garde nom + fichier + position pour pointer la 2e declaration
 * (celle de fileIndex le plus eleve). */
typedef struct {
    char name[128];
    int fileIndex;
    int line;
    int column;
} ProjectElement;

/*  : membre d'un type user (champ/constante de classe, champ de
 * structure, membre d'enumeration). Capture nom + casse d'origine + type
 * declare. Le type est requis pour resoudre les chaines imbriquees a.b.c
 * (le type du membre b permet de chercher c dans ce type). */
typedef struct {
    char ownerType[128];   /* classe/struct/enum proprietaire */
    char name[128];        /* nom du membre, casse canonique (declaration) */
    char declType[128];    /* type declare du membre ("" si const/enum) */
    int fileIndex;
    /*  : line + column de la declaration du membre, pour le check
     * shadow cross-file (champ classe vs global main.owl). 0 si indisponible
     * (prescan d'un membre derive ou cas residuel). */
    int line;
    int column;
    /*  : visibilite du champ de CLASSE (0=PUBLIC, 1=PRIVE, 2=PROTEGE,
     * meme encodage que OwlVisibility). -1 = membre non concerne (champ de
     * structure, membre d'enum, constante) : n'entre pas dans l'hydratation
     * cross-fichier de la visibilite POO. Permet a `owl --check <file>
     * --project` d'hydrater g_class_field_vis depuis le prescan au lieu de
     * full-compiler chaque voisin (passe de contexte ). */
    int visibility;
} ProjectMember;

/* ================================================================
 * SYMBOL TABLE
 * ================================================================ */

#define MAX_PROJECT_FILES 256

/*  : limites des signatures pour le check de contrat interface.
 * KEEP IN SYNC avec MAX_METHOD_PARAMS dans owl_limits.def (cf. ). */
#define PRESCAN_MAX_METHOD_PARAMS 64

/* Une signature de methode (interface ou classe externe). */
typedef struct {
    char name[128];
    int  paramCount;
    char paramTypes[PRESCAN_MAX_METHOD_PARAMS][32]; /* "" si non type */
    char returnType[32];                            /* "" si pas de retour */
} PrescanMethodSig;

typedef struct {
    ProjectSymbol* symbols;
    int count;
    int capacity;
    /* File name registry for qualified name resolution */
    char fileNames[MAX_PROJECT_FILES][128];
    int fileNameCount;
    /* Interface declarations (avec signatures pour cross-check de contrat) */
    struct {
        char name[128];
        PrescanMethodSig methods[32];
        int methodCount;
    } interfaces[64];
    int interfaceCount;

    /* Class methods (ClassName.MethodName) — avec signatures. */
    struct {
        char className[128];
        PrescanMethodSig sig;
    } classMethods[512];
    int classMethodCount;

    /*  : visibilite des methodes de classe, pour hydrater
     * g_method_visibility en `owl --check <file> --project` sans full-compiler
     * les voisins. Distinct de classMethods[] (qui ne capture pas les methodes
     * a modificateur et n'a pas la visibilite) pour ne pas elargir le contrat
     * de ce dernier (consomme par la resolution d'appel nu + le check
     * d'interface). visibility : 0=PUBLIC, 1=PRIVE, 2=PROTEGE. */
    struct {
        char className[128];
        char methodName[128];
        int  visibility;
        int  fileIndex;
        int  line;
    } classMethodVis[512];
    int classMethodVisCount;

    /* Implementations (ClassName implements InterfaceName) */
    struct {
        char className[128];
        char interfaceName[128];
    } implementations[64];
    int implementationCount;

    /*  : heritage des classes (Herite de cParent) pour le check
     * de contrat transitif. */
    struct {
        char className[128];
        char parentName[128];
    } inheritances[128];
    int inheritanceCount;

    /*  : membres des types user (champs/constantes de classe,
     * champs de structure, membres d'enum). Alloue dynamiquement. */
    ProjectMember* members;
    int memberCount;
    int memberCapacity;

    /*  : elements top-level du projet (tous types). Alloue
     * dynamiquement, alimente par prescan_file. */
    ProjectElement* elements;
    int elementCount;
    int elementCapacity;

    /*  : pointeurs non-owning vers les sources des fichiers du
     * projet. Permet a compile_in_project de re-walker les voisins via
     * prescan_source_unified pour ancrer les sous-widgets DSL cross-fichier
     * (BTN_X dans FEN_Y voisin). Liste compacte (paires fileIndex + source*)
     * et non pas indexee directement par fileIndex : en suite globale, les
     * fileIndex depassent MAX_PROJECT_FILES (~256) — le project objet est
     * partage entre tous les tests d'un meme run. Lifetime des `source` gere
     * par le caller (project.c) — la SymbolTable ne libere rien. */
    struct {
        int fileIndex;
        const char* source;
    } sources[MAX_PROJECT_FILES];
    int sourceCount;
} SymbolTable;

/* Reordonne companionIndices[] in-place selon la profondeur d'heritage des
 * classes declarees par chaque fichier (parent avant enfant). companionIndices
 * doit deja etre trie de maniere stable (alpha / ordre de decouverte) : cet
 * ordre sert de tiebreaker entre fichiers de meme profondeur ou sans heritage.
 * Requiert que t->inheritances[] soit rempli (prescan des fichiers concernes).
 * Partage entre owl test (testing.c) et owl <fichier> (project_run.c). */
void topo_sort_companions_by_inheritance(const SymbolTable* t,
                                         int* companionIndices,
                                         int companionCount);

/* Verify that all interface contracts are satisfied. Returns false if errors found. */
bool symtable_verify_interfaces(SymbolTable* t);

void symtable_init(SymbolTable* t);
void symtable_free(SymbolTable* t);

/*  : la SymbolTable pese ~5,8 Mo (dominee par PRESCAN_MAX_METHOD_PARAMS
 * via paramTypes[64][32] replique dans chaque PrescanMethodSig). Sur la pile,
 * sa taille plafonne le nombre de parametres : la reserve de pile est 16 Mo et
 * project_run.c en declare DEUX dans le meme cadre (~11,6 Mo, non recouvertes
 * meme a -O2). D'ou l'allocation HEAP obligatoire : symtable_create (calloc +
 * symtable_init) / symtable_destroy (symtable_free + free du conteneur). Aucun
 * site ne doit remettre une SymbolTable sur la pile — cf. ADR (SymbolTable heap).
 * symtable_destroy est NULL-safe. */
SymbolTable* symtable_create(void);
void symtable_destroy(SymbolTable* t);

/* Pre-scan a file's source for top-level declarations.
 * Adds discovered symbols to the table. Returns false on error. */
bool prescan_file(SymbolTable* t, const char* source, int fileIndex);

/* ================================================================
 * PROFONDEUR DE BLOCS — LA TABLE (une seule, deux portes)
 * ================================================================
 *
 *  : la regle « ce mot-cle ouvre-t-il un bloc ferme par FIN ? » a DEUX
 * accesseurs — un par jeton (prescan_depth_delta) et un par texte
 * (prescan_depth_delta_text) — mais UNE table, celle-ci. Toute liste
 * d'ouvreurs recopiee ailleurs est un clone : vingt-six sites en avaient un,
 * chacun avec sa propre omission.
 *
 * Trois categories :
 *   OPENER(tok)         ouvre toujours un bloc ferme par FIN            -> +1
 *   OPENER_INLINE(tok)  ouvre un bloc OU a une forme inline sans FIN ; le
 *                       discriminant demande un peek (cf. prescan.c)    -> +1/0
 *   CLOSER(tok)         referme un bloc                                 -> -1
 *
 * Absents de la table, volontairement, parce que leur discriminant n'est PAS
 * le jeton suivant et n'est donc pas exprimable ici :
 *   - AVEC : le meme mot-cle sert de MODIFICATEUR sans FIN au sein d'une autre
 *     instruction (`POUR TOUT v DE t AVEC cond`). Discriminant = POSITION dans
 *     la ligne -> prescan_avec_opens_block, renseigne par le site.
 *   - CLASSE / STRUCTURE / ENUMERATION / COMBINAISON / INTERFACE / FENETRE et
 *     les genres de widget : n'arrivent qu'apres la copule (`X est une
 *     Structure`), ou leur verdict depend du lookbehind. Traites au site.
 *   - PROCEDURE / FONCTION post-copule (`p est une Procedure`) : annotation de
 *     type, sans FIN. La table les classe ouvrants (cas majoritaire, tete
 *     d'instruction) ; la neutralisation post-copule est au site. */
#define OWL_DEPTH_TOKENS(OPENER, OPENER_INLINE, CLOSER) \
    CLOSER(TOKEN_FIN)                                   \
    OPENER(TOKEN_POUR)                                  \
    OPENER(TOKEN_TANTQUE)                               \
    OPENER(TOKEN_BOUCLE)                                \
    OPENER(TOKEN_SELON)                                 \
    OPENER(TOKEN_QUAND)                                 \
    OPENER(TOKEN_PROPRIETES)                            \
    OPENER(TOKEN_PROCEDURE)                             \
    OPENER(TOKEN_FONCTION)                              \
    OPENER_INLINE(TOKEN_CONSTANTE)                      \
    OPENER_INLINE(TOKEN_SI)

/* Hook neutre : pour les passes qui traitent une categorie a la main. */
#define OWL_DEPTH_X_NOTHING(t)

/* SOURCE DE VERITE UNIQUE — « ce token ouvre-t-il un bloc ferme par FIN ? »
 *. Retourne le delta de profondeur a appliquer : +1 (ouvre un bloc),
 * -1 (FIN), 0 (rien, ou forme inline sans FIN).
 *
 * Deux tokens ont AUSSI une forme inline legale, sans FIN : `SI c ALORS instr`
 * et `CONSTANTE K = 3`. Les distinguer demande un peek, d'ou le `peek` (une
 * COPIE du scanner du caller ; la fonction le restaure toujours, le scan reel
 * n'avance pas).
 *
 * Le 4e membre de la famille — `p est une Procedure|Fonction`, annotation de
 * type post-copule — a pour discriminant la COPULE, pas la fin de ligne : il
 * n'est pas visible d'ici (aucun lookbehind) et reste neutralise au site
 * d'appel, avant l'appel a cette fonction. Le corpus complet (4 formes sans
 * FIN + 13 tokens sains) est verrouille par `owl --self-test-block-openers`.
 *
 * Tout site qui suit une profondeur de blocs sur un Scanner brut DOIT passer
 * par ici : trois tickets en deux jours (, , ) sont nes
 * d'un clone de cette regle reste en arriere. */
int prescan_depth_delta(Token tok, Scanner* peek);

/* SECONDE PORTE — entree TEXTE de la meme table.
 *
 * Deux consommateurs marchent sur des CARACTERES bruts et non sur des jetons :
 * le formatter (`owl fmt`, walkers ligne a ligne) et le self-test de registre.
 * Y appeler prescan_depth_delta demanderait de reecrire leur parcours ; y
 * recopier le peek recreerait le clone. Cette porte scanne `text` avec le
 * Scanner reel et DELEGUE a prescan_depth_delta : meme table, meme peek, par
 * construction (l'une appelle l'autre).
 *
 * `text` pointe sur le PREMIER caractere du mot a juger et doit rester lisible
 * jusqu'a la fin de la ligne au moins (chaine NUL-terminee).
 *
 * Retourne true si le mot appartient a la table (et pose son delta dans
 * *delta), false si la table ne le connait pas — auquel cas le site garde la
 * main (widgets DSL, noms de type, mots-cles hors table). *delta vaut 0. */
bool prescan_depth_delta_text(const char* text, int* delta);

/* Le jeton fait-il partie de la table ? Derive de la MEME liste (cf.
 * OWL_DEPTH_TOKENS). Sert aux sites qui veulent savoir si la source unique a
 * un avis avant d'appliquer leur propre liste d'extras : un extra local qui
 * intersecte la table est un clone en train de renaitre. */
bool prescan_token_in_depth_table(TokenType type);

/* `AVEC` : hors table (son discriminant est la POSITION, pas le jeton suivant).
 * En tete d'instruction, `AVEC obj` ouvre un bloc ferme par FIN ; au milieu
 * d'une autre instruction (`POUR TOUT v DE t AVEC cond`, `Position(...) AVEC
 * opts`) c'est un modificateur sans FIN. Le site sait ou il est dans la ligne,
 * la regle vit ici. */
bool prescan_avec_opens_block(bool atStatementStart);

/*  — le top-level de ce fichier contient-il au moins un bloc DSL
 * (Projet / Fenetre / Etat / Classe / Interface / Structure / Enumeration /
 * Combinaison / Theme / CollectionDeProcedures) ? Un fichier sans aucun bloc
 * (procedures/globales/instructions au top-level) est un script PLAT. */
bool prescan_source_is_dsl_block_file(const char* source);

/*  — kind du PREMIER bloc DSL top-level d'un fichier. Sert au filtre
 * de lancement direct (`owl <fichier>`) : seuls PROJET, FENETRE et NONE (script
 * plat) sont lancables. Les autres kinds sont refuses (blocage anti-silence). */
typedef enum {
    PRESCAN_ENTRY_NONE = 0,   /* aucun bloc DSL top-level : script plat */
    PRESCAN_ENTRY_PROJET,
    PRESCAN_ENTRY_FENETRE,
    PRESCAN_ENTRY_ETAT,
    PRESCAN_ENTRY_CLASSE,
    PRESCAN_ENTRY_COLLECTION,
    PRESCAN_ENTRY_THEME,
    PRESCAN_ENTRY_STRUCTURE,
    PRESCAN_ENTRY_ENUM,
    PRESCAN_ENTRY_COMBINAISON,
    PRESCAN_ENTRY_INTERFACE
} PrescanEntryKind;

/*  — retourne le kind du premier bloc DSL top-level et copie le nom
 * declare (identifiant a gauche de `est`) dans nameOut (tronque a nameCap-1).
 * PRESCAN_ENTRY_NONE si aucun bloc DSL au top-level (script plat). */
PrescanEntryKind prescan_source_entry_block_kind(const char* source,
                                                 char* nameOut, int nameCap);

/*  — active/desactive le filtre de perimetre projet pour les appels
 * ULTERIEURS a prescan_file. En mode projet (restrictToDsl==true), les
 * compagnons PLATS (hors entryIndex) sont ignores par prescan_file. Mode script
 * : restrictToDsl==false (defaut) -> aucun filtre. entryIndex = -1 si aucune
 * entree forcee (projet.owl participe de toute facon via son bloc DSL). */
void prescan_set_project_scope(bool restrictToDsl, int entryIndex);

/* Look up a symbol by name.
 * If ambiguous (multiple non-private from different files), sets *ambiguous = true.
 * Returns the first match, or NULL if not found. */
ProjectSymbol* symtable_find(SymbolTable* t, const char* name, int len,
                              bool* ambiguous);

/* Look up a symbol by name, restricted to a specific file index.
 * Returns the match or NULL. */
ProjectSymbol* symtable_find_in_file(SymbolTable* t, const char* name, int len,
                                      int fileIndex);

/*  : un element top-level de nom `name` (canon casse+accent) est-il
 * deja declare dans un fichier de fileIndex STRICTEMENT INFERIEUR ? Sert a
 * n'emettre l'erreur de doublon que sur la 2e declaration (la 1re — fileIndex
 * le plus bas — ne porte pas d'erreur). */
bool symtable_element_has_earlier_dup(SymbolTable* t, const char* name, int len,
                                      int fileIndex);

/*  : recherche d'un membre par type proprietaire + nom (les deux
 * case/accent-insensibles). Retourne le membre (donc sa casse canonique +
 * son type declare) ou NULL. */
ProjectMember* symtable_find_member(SymbolTable* t,
                                    const char* ownerType, int ownerLen,
                                    const char* member, int memberLen);

/*  : vrai si `methodName` est une methode de `className` OU d'une
 * de ses classes ancetres (chaine via t->inheritances). Case-insensible.
 * Sert a la resolution d'un appel nu `Calcule(...)` depuis le body d'une
 * methode (acces a une methode soeur, eventuellement heritee). */
bool symtable_has_class_method_inherited(SymbolTable* t,
                                         const char* className, int classLen,
                                         const char* methodName, int methodLen);

/*  : un symbole top-level est "ambigu" pour la canonisation de casse
 * quand il est declare dans >=2 fichiers avec des casses DIVERGENTES (ex.
 * `cConfig` dans un fichier, `cconfig` dans un autre). Dans ce cas on ne peut
 * pas trancher la forme canonique -> le formatter laisse verbatim. Comparaison
 * sur le nom complet, owner inclus pour les membres. */
bool symtable_type_is_ambiguous(SymbolTable* t, const char* name);
/*  — variante pour les globals (variables/constantes globales,
 * procedures/fonctions top-level). Le formatter s'abstient si ambigu. */
bool symtable_global_is_ambiguous(SymbolTable* t, const char* name);
bool symtable_member_is_ambiguous(SymbolTable* t, const char* ownerType,
                                  const char* member);

#endif
