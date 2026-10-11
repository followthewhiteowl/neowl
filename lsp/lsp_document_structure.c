// SPDX-License-Identifier: AGPL-3.0-only
/* lsp/lsp_document_structure.c
 *  -- documentSymbol + foldingRange pour fichiers Fenetre OWL.
 *
 * Expose la structure d'un fichier fenetre au client LSP :
 *   handle_document_symbol(params) -> array de DocumentSymbol (hierarchie)
 *   handle_folding_range(params)   -> array de FoldingRange (plages repliables)
 *
 * Le DSL Fenetre est a structure simple : indentation + mots-cles
 *   <NOM> est une Fenetre  ... FIN        (racine)
 *   <NOM> est un Champ <Type> ... FIN     (champ)
 *   PROCEDURE <nom>() ... FIN             (procedure d'evenement)
 *   <nom> est un/une <type>               (declaration corps de fenetre)
 *
 * Le parsing se fait ligne a ligne. Les blocs sont apparies par indentation :
 * un bloc ouvert a l'indentation I est ferme par le premier FIN a la meme
 * indentation I (les blocs imbriques sont plus indentes, leurs FIN aussi).
 * La forme canonique tabulee garantie par le formatter rend cette regle sure.
 *
 * Casse/accent-insensibilite : les mots-cles sont normalises (strip accents +
 * lowercase) avant comparaison, donc "est une Fenetre" == "est une Fenêtre".
 * Les NOMS de symboles emis conservent l'orthographe source exacte.
 */

#include "lsp_server.h"
#include "../src/scanner.h"   /* scanner_strip_accents */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* SymbolKind LSP (spec 3.17). */
#define KIND_MODULE     2
#define KIND_NAMESPACE  3
#define KIND_CLASS      5
#define KIND_METHOD     6
#define KIND_PROPERTY   7
#define KIND_FIELD      8
#define KIND_ENUM      10
#define KIND_FUNCTION  12
#define KIND_VARIABLE  13
#define KIND_CONSTANT  14
#define KIND_ENUMMEMBER 22
#define KIND_STRUCT    23
#define KIND_EVENT     24

#define MAX_NAME   128
#define MAX_DECLS  256    /* capacite initiale ; les tableaux concernes croissent */
#define MAX_DEPTH    32   /* borne de recursion sur l'imbrication des conteneurs */

/* ================================================================
 * Modele de structure (parse intermediaire)
 *
 * Le corps d'un widget (Fenetre/Etat) est un arbre : un conteneur
 * (champ OU section d'etat type Corps/EnTete/...) peut imbriquer
 * d'autres conteneurs et des procedures d'evenement. On represente
 * cet arbre par un pool plat de SymNode chaines par firstChild /
 * nextSibling (evite l'alloc par noeud et borne la memoire).
 * ================================================================ */

typedef struct {
    char name[MAX_NAME];
    char detail[MAX_NAME];   /* type declare (ex "entier"), vide si absent */
    int  line;
} DeclNode;

typedef struct {
    char name[MAX_NAME];
    char detail[MAX_NAME];   /* type de widget pour un champ (ex "Bouton"), vide sinon */
    int  kind;          /* KIND_FIELD (conteneur) ou KIND_METHOD (procedure) */
    int  startLine;
    int  endLine;
    int  firstChild;    /* index dans nodes[], -1 si feuille */
    int  lastChild;     /* index dans nodes[], -1 si feuille (append O(1)) */
    int  nextSibling;   /* index dans nodes[], -1 si dernier de la fratrie */
} SymNode;

typedef struct {
    bool hasWindow;
    char windowName[MAX_NAME];
    int  windowStart;
    int  windowEnd;

    DeclNode* decls;                 /* declarations mono-ligne niveau widget */
    int       declCount;
    int       declCap;

    SymNode*  nodes;                 /* pool de noeuds, croissance dynamique */
    int       nodeCount;
    int       nodeCap;
    int       rootFirst;             /* 1er enfant top-level (-1 si aucun) */
    int       rootLast;
} FenStructure;

/* ----------------------------------------------------------------
 * Modele "module/script" : fichier sans racine widget (Fenetre/Etat).
 * On expose un outline generique des declarations top-level :
 *   PROCEDURE / FONCTION top-level -> symbole Function (callable libre)
 *   <NOM> est une Class / is a class -> symbole Class avec ses membres
 *     (methodes PROCEDURE -> Method ; champs <nom> est un <type> -> Field)
 * ---------------------------------------------------------------- */

typedef struct ClassMember {
    char name[MAX_NAME];
    char detail[MAX_NAME];   /* type declare pour un champ, vide sinon */
    int  startLine;
    int  endLine;
    int  kind;        /* KIND_METHOD / KIND_FIELD / KIND_ENUMMEMBER / KIND_CONSTANT
                       * ou un conteneur imbrique (KIND_STRUCT/ENUM/CLASS). */
    /* Un membre peut lui-meme etre un conteneur imbrique (structure declaree
     * dans une collection/classe) : ses champs internes sont ses enfants,
     * pas des membres a plat du conteneur parent. */
    struct ClassMember* children;
    int  childCount;
    int  childCap;
} ClassMember;

typedef struct {
    bool isContainer; /* true = bloc a corps (classe/enum/structure/constantes) */
    bool isCollection;/* true ssi bloc CollectionDeProcedures (modele init) */
    char name[MAX_NAME];
    char detail[MAX_NAME];   /* type declare pour une variable, vide sinon */
    int  kind;        /* kind de l'item lui-meme (Class/Enum/Struct/Variable/...) */
    int  startLine;
    int  endLine;
    ClassMember* members;    /* croissance dynamique, NULL si aucun membre */
    int  memberCount;
    int  memberCap;
} ModItem;

typedef struct {
    ModItem* items;          /* croissance dynamique */
    int     itemCount;
    int     itemCap;
} ModStructure;

/* ================================================================
 * Decoupage en lignes
 * ================================================================ */

typedef struct {
    int  start;     /* offset du 1er caractere de la ligne */
    int  len;       /* longueur hors retour chariot */
    int  indent;    /* nb de caracteres blancs (tab/espace) en tete */
    bool isFin;     /* la ligne est-elle un FIN/END seul ? */
} LineInfo;

static int leading_indent(const char* s, int len) {
    int i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

static bool is_word_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* Normalise un mot : strip accents puis lowercase ASCII. */
static void normalize_word(const char* s, int len, char* out, int outSize) {
    char tmp[256];
    int n = scanner_strip_accents(s, len, tmp, (int)sizeof(tmp));
    if (n >= outSize) n = outSize - 1;
    for (int i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)tmp[i]);
    out[n] = '\0';
}

typedef struct {
    const char* p;    /* pointeur sur le texte source (orthographe exacte) */
    int   len;
    char  norm[64];   /* forme normalisee pour matcher les mots-cles */
} Word;

/* Extrait jusqu'a `max` mots d'une ligne. Retourne le nombre extrait. */
static int line_words(const char* p, int len, Word* w, int max) {
    int pos = 0, count = 0;
    while (count < max && pos < len) {
        while (pos < len && (p[pos] == ' ' || p[pos] == '\t')) pos++;
        if (pos >= len) break;
        if (!is_word_char((unsigned char)p[pos])) {
            /* caractere isole ('=' '(' ...) : token d'un caractere */
            w[count].p = &p[pos];
            w[count].len = 1;
            normalize_word(&p[pos], 1, w[count].norm, sizeof(w[count].norm));
            count++;
            pos++;
            continue;
        }
        int start = pos;
        while (pos < len && is_word_char((unsigned char)p[pos])) pos++;
        w[count].p = &p[start];
        w[count].len = pos - start;
        normalize_word(&p[start], pos - start, w[count].norm, sizeof(w[count].norm));
        count++;
    }
    return count;
}

static bool eq(const Word* w, const char* kw) {
    return strcmp(w->norm, kw) == 0;
}

static bool is_un_une(const Word* w) {
    return eq(w, "un") || eq(w, "une");
}

/* support #211 — index (dans w[]) du type declare dans une ligne
 * "<NOM> <connecteur> <type> ...". Le connecteur est "est"/"is" suivi d'un
 * article de CONFORT optionnel ("un"/"une"/"a"). L'article est facultatif
 * PARTOUT, comme dans le compilateur et l'extension :
 *   "x est un entier" / "x is a int" -> type en w[3]  (article present)
 *   "x est entier"    / "x is int"   -> type en w[2]  (article omis)
 * Retourne 0 si la ligne n'est pas une declaration (pas de connecteur en w[1],
 * ou aucun mot de type apres le connecteur). */
static int decl_type_index(const Word* w, int n) {
    if (n < 3) return 0;
    bool est = eq(&w[1], "est");
    bool is  = eq(&w[1], "is");
    if (!est && !is) return 0;
    if (n >= 4 && ((est && is_un_une(&w[2])) || (is && eq(&w[2], "a"))))
        return 3;   /* article de confort present : le type suit l'article */
    return 2;       /* connecteur nu : le type suit directement le "est"/"is" */
}

/* Racine widget : Fenetre/Window OU Etat/Report (meme DSL, meme kind). */
static bool is_type_window(const Word* w) {
    return eq(w, "fenetre") || eq(w, "window") ||
           eq(w, "etat")    || eq(w, "report");
}

/* Champ : FR "Champ", EN "Control" (canon) ; "Field" tolere. */
static bool is_type_champ(const Word* w) {
    return eq(w, "champ") || eq(w, "control") || eq(w, "field");
}

static bool is_type_class(const Word* w) {
    return eq(w, "class") || eq(w, "classe");
}

/* Racine projet : FR "Projet", EN "Project". */
static bool is_type_project(const Word* w) {
    return eq(w, "projet") || eq(w, "project");
}

/* Racine theme : FR "Thème" (normalise "theme"), EN "Theme". */
static bool is_type_theme(const Word* w) {
    return eq(w, "theme");
}

/* Type de style d'un theme : tout mot commencant par "style" (styleBouton,
 * styleConteneur, styleLabel, ...). Le suffixe designe la famille de widget. */
static bool is_type_style(const Word* w) {
    return strncmp(w->norm, "style", 5) == 0 && w->norm[5] != '\0';
}

/* Bloc collection de procedures : FR "CollectionDeProcédures" (normalise
 * "collectiondeprocedures"), EN "ProcedureCollection". Seul ce conteneur
 * generique porte du code d'initialisation + des variables globales ; une
 * classe/enum/structure ne suit PAS le modele "Initialisation en tete". */
static bool is_type_collection(const Word* w) {
    return eq(w, "collectiondeprocedures") || eq(w, "procedurecollection");
}

/* Bloc de proprietes/metadonnees : FR "PROPRIETES", EN "PROPERTIES". */
static bool is_kw_proprietes(const Word* w) {
    return eq(w, "proprietes") || eq(w, "properties");
}

/* Bloc de constantes : FR "CONSTANTE"/"CONSTANT", EN "CONSTANT". */
static bool is_kw_constant(const Word* w) {
    return eq(w, "constante") || eq(w, "constant");
}

/* Kind d'un bloc conteneur top-level d'apres son mot-cle de type. */
static int block_opener_kind(const Word* typeW) {
    if (is_type_class(typeW))          return KIND_CLASS;
    if (eq(typeW, "enumeration"))      return KIND_ENUM;   /* FR "Énumération" normalisee */
    if (eq(typeW, "structure"))        return KIND_STRUCT;
    return KIND_CLASS;                                     /* conteneur generique */
}

/* Mot-cle introduisant un callable (procedure ou fonction). */
static bool is_callable_kw(const Word* w) {
    return eq(w, "procedure") || eq(w, "fonction") || eq(w, "function");
}

/* Modificateur de visibilite intercale entre le mot-cle et le nom :
 * "PROCEDURE PUBLIQUE nom()". A ignorer pour resoudre le nom. */
static bool is_proc_modifier(const Word* w) {
    return eq(w, "publique") || eq(w, "public") ||
           eq(w, "prive")    || eq(w, "private");
}

/* Resout le nom d'un callable a partir des mots de sa ligne : saute le
 * mot-cle (w[0]) et d'eventuels modificateurs, retourne le 1er identifiant.
 * Echoue si aucun identifiant ne suit (ex: "PROCEDURE ()"). */
static bool resolve_callable_name(const Word* w, int n, Word* out) {
    int idx = 1;
    while (idx < n && is_proc_modifier(&w[idx])) idx++;
    if (idx >= n) return false;
    if (!is_word_char((unsigned char)w[idx].p[0])) return false;
    *out = w[idx];
    return true;
}

static void copy_word_name(char* dst, const Word* w) {
    int n = w->len < MAX_NAME ? w->len : MAX_NAME - 1;
    memcpy(dst, w->p, n);
    dst[n] = '\0';
}

/* Classification d'une ligne (sur ses premiers mots). */
typedef enum { L_OTHER, L_WINDOW, L_FIELD, L_PROC, L_DECL } LineClass;

/* Classifie une ligne. `outName` = nom du symbole. `outType` (nullable) recoit,
 * pour un L_FIELD, le mot de type de widget (`Champ Bouton` -> "Bouton") ; pour
 * un L_DECL, le mot de type declare (`x est un entier` -> "entier"). Vide si
 * absent (len 0). */
static LineClass classify_line(const char* p, int len, Word* outName, Word* outType) {
    if (outType) { outType->len = 0; outType->p = NULL; }
    Word w[5];
    int n = line_words(p, len, w, 5);
    if (n == 0) return L_OTHER;

    /* PROCEDURE/FONCTION [PUBLIQUE|PRIVE] <nom>() */
    if (is_callable_kw(&w[0])) {
        Word nm;
        if (resolve_callable_name(w, n, &nm)) { *outName = nm; return L_PROC; }
        return L_OTHER;
    }

    /* <NOM> [est un/une | is a | est | is] <type> — article facultatif (#211). */
    int ti = decl_type_index(w, n);
    if (ti > 0) {
        if (is_type_window(&w[ti])) { *outName = w[0]; return L_WINDOW; }
        if (is_type_champ(&w[ti]))  {
            *outName = w[0];
            /* Type de widget : mot qui suit "Champ"/"Control" (w[ti+1]). */
            if (outType && n > ti + 1) *outType = w[ti + 1];
            return L_FIELD;
        }
        *outName = w[0];
        /* Type declare : mot qui suit le connecteur (w[ti]). */
        if (outType) *outType = w[ti];
        return L_DECL;
    }
    return L_OTHER;
}

/* Trouve la fin d'un bloc ouvert a la ligne `openLine` (indentation `indent`) :
 * premier FIN a la meme indentation. Fallback : derniere ligne. */
static int find_block_end(const LineInfo* lines, int lineCount,
                          int openLine, int indent) {
    for (int i = openLine + 1; i < lineCount; i++) {
        if (lines[i].isFin && lines[i].indent == indent) return i;
    }
    return lineCount - 1;
}

/* Determine si la ligne `openLine` (indentation `indent`) OUVRE un bloc :
 * un corps reel plus indente, ferme par un FIN apparie au meme indent, le
 * tout AVANT la borne `limit` (fin du conteneur parent / de la fenetre).
 * Retourne la ligne du FIN apparie si c'est un conteneur, -1 sinon (le cas
 * d'une declaration mono-ligne type "nGlobal est un entier = 0" : la ligne
 * suivante n'est pas plus indentee, donc pas de corps). */
static int container_block_end(const LineInfo* lines, int openLine,
                               int indent, int limit) {
    /* 1re ligne non-vide suivante : doit etre plus indentee (corps reel). */
    int next = openLine + 1;
    while (next < limit && lines[next].indent >= lines[next].len) next++;
    if (next >= limit) return -1;
    if (lines[next].indent <= indent) return -1;
    /* FIN appariee au meme indent, avant la borne du parent. */
    for (int i = openLine + 1; i < limit; i++) {
        if (lines[i].isFin && lines[i].indent == indent) return i;
    }
    return -1;
}

/* ----- Pool de noeuds (arbre conteneur/proc) ----- */

static int node_alloc(FenStructure* fs, const Word* name, int kind,
                      int startLine, int endLine, const Word* detail) {
    if (fs->nodeCount >= fs->nodeCap) {
        int nc = fs->nodeCap ? fs->nodeCap * 2 : 64;
        SymNode* nb = realloc(fs->nodes, sizeof(SymNode) * (size_t)nc);
        if (!nb) return -1;
        fs->nodes = nb;
        fs->nodeCap = nc;
    }
    SymNode* n = &fs->nodes[fs->nodeCount];
    int len = name->len < MAX_NAME ? name->len : MAX_NAME - 1;
    memcpy(n->name, name->p, len);
    n->name[len] = '\0';
    n->detail[0] = '\0';
    if (detail && detail->p && detail->len > 0) {
        int dl = detail->len < MAX_NAME ? detail->len : MAX_NAME - 1;
        memcpy(n->detail, detail->p, dl);
        n->detail[dl] = '\0';
    }
    n->kind = kind;
    n->startLine = startLine;
    n->endLine = endLine;
    n->firstChild = -1;
    n->lastChild = -1;
    n->nextSibling = -1;
    return fs->nodeCount++;
}

/* Rattache `child` a `parent` (-1 = niveau top widget). No-op si child < 0. */
static void node_append_child(FenStructure* fs, int parent, int child) {
    if (child < 0) return;
    if (parent < 0) {
        if (fs->rootFirst < 0) fs->rootFirst = child;
        else fs->nodes[fs->rootLast].nextSibling = child;
        fs->rootLast = child;
        return;
    }
    SymNode* p = &fs->nodes[parent];
    if (p->firstChild < 0) p->firstChild = child;
    else fs->nodes[p->lastChild].nextSibling = child;
    p->lastChild = child;
}

/* Nouvelle declaration mono-ligne au niveau widget. Retourne NULL si l'alloc
 * echoue (le symbole est alors simplement omis, jamais de crash). */
static DeclNode* fen_new_decl(FenStructure* fs) {
    if (fs->declCount >= fs->declCap) {
        int nc = fs->declCap ? fs->declCap * 2 : 32;
        DeclNode* nb = realloc(fs->decls, sizeof(DeclNode) * (size_t)nc);
        if (!nb) return NULL;
        fs->decls = nb;
        fs->declCap = nc;
    }
    return &fs->decls[fs->declCount++];
}

/* Libere les buffers dynamiques internes puis la structure elle-meme.
 * Sur pour fs == NULL et pour des buffers jamais alloues (NULL). */
static void fen_structure_free(FenStructure* fs) {
    if (!fs) return;
    free(fs->decls);
    free(fs->nodes);
    free(fs);
}

/* ================================================================
 * Parse
 * ================================================================ */

/* Neutralise les commentaires blocs avant le decoupage ligne-a-ligne.
 *
 * Le parseur de structure travaille ligne par ligne et n'a aucune notion des
 * commentaires blocs. Une fausse fin de bloc DANS un commentaire (typiquement
 * "end" colle a la fermeture, ou le token 'end' devient le 1er mot de la ligne)
 * est prise pour un vrai FIN et tronque tout le code aval. On blanchit donc le
 * CONTENU de chaque commentaire bloc (delimiteurs inclus) en remplacant chaque
 * octet par un espace, en PRESERVANT les '\n' et '\r' : le buffer resultat a la
 * meme longueur et les memes sauts de ligne, donc offsets et numeros de ligne
 * restent identiques a l'original. Le code non commente est copie tel quel.
 *
 * Gere aussi :
 *   - bloc non ferme jusqu'a EOF (blanchi jusqu'a la fin, pas de crash) ;
 *   - strings "..." et `...` : un delimiteur de commentaire a l'interieur
 *     d'une chaine n'ouvre pas de commentaire (pas d'echappement backslash :
 *     OWL ferme la chaine au guillemet, cf. syntaxe backtick pour les quotes) ;
 *   - commentaire ligne // : le reste de la ligne est du texte, un debut de
 *     bloc apres // n'ouvre pas de commentaire (evite de blanchir la suite).
 *
 * Retourne un buffer malloc'e (a liberer par l'appelant), NULL si malloc echoue. */
static char* strip_block_comments(const char* src, int srcLen) {
    char* out = malloc((size_t)srcLen + 1);
    if (!out) return NULL;
    memcpy(out, src, (size_t)srcLen);
    out[srcLen] = '\0';

    int i = 0;
    while (i < srcLen) {
        char c = src[i];
        /* Chaine "..." ou `...` : copier tel quel jusqu'a la fermeture. */
        if (c == '"' || c == '`') {
            char quote = c;
            i++;
            while (i < srcLen && src[i] != quote) i++;
            if (i < srcLen) i++;   /* consomme le guillemet fermant */
            continue;
        }
        /* Commentaire ligne // : le reste de la ligne n'ouvre pas de bloc. */
        if (c == '/' && i + 1 < srcLen && src[i + 1] == '/') {
            while (i < srcLen && src[i] != '\n') i++;
            continue;
        }
        /* Commentaire bloc : blanchir le contenu, garder \n et \r. */
        if (c == '/' && i + 1 < srcLen && src[i + 1] == '*') {
            out[i] = ' ';
            out[i + 1] = ' ';
            i += 2;
            while (i < srcLen) {
                if (src[i] == '*' && i + 1 < srcLen && src[i + 1] == '/') {
                    out[i] = ' ';
                    out[i + 1] = ' ';
                    i += 2;
                    break;   /* fermeture : la suite de la ligne reste analysee */
                }
                if (src[i] != '\n' && src[i] != '\r') out[i] = ' ';
                i++;
            }
            continue;
        }
        i++;
    }
    return out;
}

/* Decoupe le source en lignes (offset/longueur/indentation/isFin).
 * Tableau malloc'e a liberer par l'appelant. */
static LineInfo* split_lines(const char* src, int srcLen, int* outCount) {
    int cap = 64, count = 0;
    LineInfo* lines = malloc(sizeof(LineInfo) * cap);
    int i = 0;
    while (i <= srcLen) {
        int start = i;
        while (i < srcLen && src[i] != '\n') i++;
        int end = i;                 /* exclut '\n' */
        if (end > start && src[end - 1] == '\r') end--;  /* CRLF */
        if (count >= cap) {
            cap *= 2;
            lines = realloc(lines, sizeof(LineInfo) * cap);
        }
        LineInfo* L = &lines[count++];
        L->start = start;
        L->len = end - start;
        L->indent = leading_indent(&src[start], L->len);
        /* isFin : premier mot == fin/end. */
        L->isFin = false;
        {
            Word w[1];
            if (line_words(&src[start], L->len, w, 1) == 1 &&
                (strcmp(w[0].norm, "fin") == 0 || strcmp(w[0].norm, "end") == 0)) {
                L->isFin = true;
            }
        }
        if (i < srcLen && src[i] == '\n') i++;
        else break;
    }
    *outCount = count;
    return lines;
}

/* Parcourt le corps [bodyStart, bodyEnd) d'un conteneur (parent >= 0) ou du
 * widget (parent == -1) et rattache les noeuds enfants :
 *   - PROCEDURE/FONCTION              -> Method (feuille)
 *   - <nom> est un <Type> ouvrant un bloc FIN -> Field conteneur + recursion
 *   - <nom> est un <Type> mono-ligne  -> declaration (Declarations globales,
 *                                         uniquement au niveau widget)
 * `depth` borne la recursion sur l'imbrication des conteneurs. */
static void parse_body(const char* src, const LineInfo* lines, int lineCount,
                       int bodyStart, int bodyEnd, int parent, int depth,
                       FenStructure* fs) {
    if (depth > MAX_DEPTH) return;
    int j = bodyStart;
    while (j < bodyEnd) {
        Word name, type;
        LineClass cls = classify_line(&src[lines[j].start], lines[j].len,
                                      &name, &type);

        if (cls == L_PROC) {
            int pend = find_block_end(lines, lineCount, j, lines[j].indent);
            node_append_child(fs, parent,
                              node_alloc(fs, &name, KIND_METHOD, j, pend, NULL));
            j = pend + 1;
        } else if (cls == L_FIELD || cls == L_DECL) {
            /* Conteneur ssi la ligne ouvre reellement un bloc FIN apparie.
             * Sinon c'est une declaration mono-ligne. */
            int cend = container_block_end(lines, j, lines[j].indent, bodyEnd);
            if (cend >= 0) {
                /* Un champ porte son type de widget en "detail" ; une section
                 * d'etat (L_DECL conteneur, type non pertinent) n'en a pas. */
                const Word* det = (cls == L_FIELD) ? &type : NULL;
                int node = node_alloc(fs, &name, KIND_FIELD, j, cend, det);
                node_append_child(fs, parent, node);
                /* node < 0 (pool sature) : on saute le bloc sans recurser,
                 * pour ne pas reverser ses enfants au niveau du parent. */
                if (node >= 0)
                    parse_body(src, lines, lineCount, j + 1, cend,
                               node, depth + 1, fs);
                j = cend + 1;
            } else {
                /* Declaration mono-ligne : seules celles du niveau widget
                 * alimentent "Declarations globales". */
                if (parent < 0) {
                    DeclNode* dn = fen_new_decl(fs);
                    if (dn) {
                        int n = name.len < MAX_NAME ? name.len : MAX_NAME - 1;
                        memcpy(dn->name, name.p, n);
                        dn->name[n] = '\0';
                        dn->detail[0] = '\0';
                        if (type.p && type.len > 0) {
                            int dl = type.len < MAX_NAME ? type.len : MAX_NAME - 1;
                            memcpy(dn->detail, type.p, dl);
                            dn->detail[dl] = '\0';
                        }
                        dn->line = j;
                    }
                }
                j++;
            }
        } else {
            j++;
        }
    }
}

static void parse_structure(const char* src, int srcLen, FenStructure* fs) {
    memset(fs, 0, sizeof(*fs));
    fs->rootFirst = -1;
    fs->rootLast = -1;

    /* Neutralise les commentaires blocs : le parsing (ci-dessous et parse_body)
     * lit desormais le buffer nettoye. Les noms de symboles y sont identiques
     * a l'original (on ne touche qu'au contenu commente). */
    char* clean = strip_block_comments(src, srcLen);
    if (clean) src = clean;

    int count = 0;
    LineInfo* lines = split_lines(src, srcLen, &count);

    /* Repere la ligne Fenetre. */
    int winLine = -1;
    Word nm;
    for (int l = 0; l < count; l++) {
        if (classify_line(&src[lines[l].start], lines[l].len, &nm, NULL) == L_WINDOW) {
            winLine = l;
            break;
        }
    }
    if (winLine < 0) { free(lines); free(clean); return; }

    fs->hasWindow = true;
    int wl = nm.len < MAX_NAME ? nm.len : MAX_NAME - 1;
    memcpy(fs->windowName, nm.p, wl);
    fs->windowName[wl] = '\0';
    fs->windowStart = winLine;
    fs->windowEnd = find_block_end(lines, count, winLine, lines[winLine].indent);

    parse_body(src, lines, count, winLine + 1, fs->windowEnd, -1, 0, fs);

    free(lines);
    free(clean);
}

/* Copie dans `out` le texte a droite du premier '=' de la ligne, trimme
 * (espaces/tabs de tete et de queue retires). Vide si pas de '='. */
static void extract_value_after_eq(const char* line, int len, char* out, int outSize) {
    out[0] = '\0';
    int eq = -1;
    for (int i = 0; i < len; i++) { if (line[i] == '=') { eq = i; break; } }
    if (eq < 0) return;
    int s = eq + 1;
    while (s < len && (line[s] == ' ' || line[s] == '\t')) s++;
    int e = len;
    while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) e--;
    int n = e - s;
    if (n >= outSize) n = outSize - 1;
    if (n > 0) memcpy(out, &line[s], n);
    out[n < 0 ? 0 : n] = '\0';
}

/* Ajoute un slot a une liste de ClassMember (croissance dynamique) et le
 * remet a zero (children = NULL : realloc ne zero-init pas). NULL si l'alloc
 * echoue. Partage par les membres directs d'un conteneur (ModItem) et les
 * enfants d'un membre conteneur imbrique. */
static ClassMember* member_list_grow(ClassMember** arr, int* count, int* cap) {
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        ClassMember* nb = realloc(*arr, sizeof(ClassMember) * (size_t)nc);
        if (!nb) return NULL;
        *arr = nb;
        *cap = nc;
    }
    ClassMember* m = &(*arr)[(*count)++];
    memset(m, 0, sizeof(*m));
    return m;
}

/* Libere recursivement une liste de membres et les enfants de chacun. */
static void member_list_free(ClassMember* arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++)
        member_list_free(arr[i].children, arr[i].childCount);
    free(arr);
}

/* Nouvel item top-level (croissance dynamique). Le slot est remis a zero
 * (members = NULL, memberCap = 0) car realloc ne zero-init pas. NULL si
 * l'alloc echoue. */
static ModItem* mod_new_item(ModStructure* ms) {
    if (ms->itemCount >= ms->itemCap) {
        int nc = ms->itemCap ? ms->itemCap * 2 : 32;
        ModItem* nb = realloc(ms->items, sizeof(ModItem) * (size_t)nc);
        if (!nb) return NULL;
        ms->items = nb;
        ms->itemCap = nc;
    }
    ModItem* it = &ms->items[ms->itemCount++];
    memset(it, 0, sizeof(*it));
    return it;
}

/* Libere les buffers de membres de chaque item (recursivement, enfants
 * compris), le tableau d'items, puis ms. */
static void mod_structure_free(ModStructure* ms) {
    if (!ms) return;
    for (int i = 0; i < ms->itemCount; i++)
        member_list_free(ms->items[i].members, ms->items[i].memberCount);
    free(ms->items);
    free(ms);
}

/* Sous-scan des membres d'un bloc conteneur [bodyStart, bodyEnd) :
 *   - PROCEDURE/FONCTION       -> Method
 *   - <nom> est un/une <type>  -> Field (detail = type)
 *   - <NOM> = <valeur>         -> EnumMember (bloc enum) sinon Constant
 *                                 (detail = valeur)
 * `containerKind` sert a departager EnumMember vs Constant. */
static void scan_members_range(const char* src, const LineInfo* lines, int count,
                               int bodyStart, int bodyEnd, int containerKind,
                               ClassMember** arr, int* cnt, int* cap, int depth) {
    if (depth > MAX_DEPTH) return;
    int k = bodyStart;
    while (k < bodyEnd) {
        Word mw[5];
        int mn = line_words(&src[lines[k].start], lines[k].len, mw, 5);
        if (mn == 0) { k++; continue; }
        int mti = decl_type_index(mw, mn);

        if (is_callable_kw(&mw[0])) {
            int mEnd = find_block_end(lines, count, k, lines[k].indent);
            Word mname;
            if (resolve_callable_name(mw, mn, &mname)) {
                ClassMember* cm = member_list_grow(arr, cnt, cap);
                if (cm) {
                    copy_word_name(cm->name, &mname);
                    cm->detail[0] = '\0';
                    cm->startLine = k;
                    cm->endLine = mEnd;
                    cm->kind = KIND_METHOD;
                }
            }
            k = mEnd + 1;
        } else if (mti > 0) {
            /* Conteneur imbrique (structure/enum/classe) ssi la ligne ouvre un
             * vrai bloc FIN apparie ; sinon declaration de champ mono-ligne. */
            int cend = container_block_end(lines, k, lines[k].indent, bodyEnd);
            if (cend >= 0) {
                ClassMember* cm = member_list_grow(arr, cnt, cap);
                if (cm) {
                    int ck = block_opener_kind(&mw[mti]);
                    copy_word_name(cm->name, &mw[0]);
                    copy_word_name(cm->detail, &mw[mti]);   /* type ecrit */
                    cm->startLine = k;
                    cm->endLine = cend;
                    cm->kind = ck;
                    /* Les champs internes deviennent les enfants du membre,
                     * pas des membres a plat du conteneur parent. Le membre
                     * n'est plus reference apres cette recursion (arr n'est
                     * pas grow durant le scan des enfants). */
                    scan_members_range(src, lines, count, k + 1, cend, ck,
                                       &cm->children, &cm->childCount,
                                       &cm->childCap, depth + 1);
                }
                k = cend + 1;
            } else {
                ClassMember* cm = member_list_grow(arr, cnt, cap);
                if (cm) {
                    copy_word_name(cm->name, &mw[0]);
                    copy_word_name(cm->detail, &mw[mti]);   /* type declare */
                    cm->startLine = k;
                    cm->endLine = k;
                    cm->kind = KIND_FIELD;
                }
                k++;
            }
        } else if (mn >= 3 && eq(&mw[1], "=")) {
            /* Valeur nommee : membre d'enum ou constante. */
            ClassMember* cm = member_list_grow(arr, cnt, cap);
            if (cm) {
                copy_word_name(cm->name, &mw[0]);
                extract_value_after_eq(&src[lines[k].start], lines[k].len,
                                       cm->detail, MAX_NAME);
                cm->startLine = k;
                cm->endLine = k;
                cm->kind = (containerKind == KIND_ENUM) ? KIND_ENUMMEMBER
                                                        : KIND_CONSTANT;
            }
            k++;
        } else {
            /* Ligne non-declarative (code imperatif, commentaire) : ignoree,
             * seules les declarations peuplent l'outline. */
            k++;
        }
    }
}

static void scan_container_members(const char* src, const LineInfo* lines, int count,
                                   int bodyStart, int bodyEnd, int containerKind,
                                   ModItem* it) {
    scan_members_range(src, lines, count, bodyStart, bodyEnd, containerKind,
                       &it->members, &it->memberCount, &it->memberCap, 0);
}

/* Parse "module/script" : pas de racine widget. Outline top-level des
 * callables libres (PROCEDURE/FONCTION), des blocs conteneurs (classe /
 * enum / structure / bloc CONSTANTE) avec leurs membres, et des declarations
 * mono-ligne (variables, constantes inline). */
/* Detecte si un fichier module/script est ecrit en anglais (mots-cles EN)
 * plutot qu'en francais. Heuristique par vote sur les mots-cles de surface :
 * connecteur de declaration (est/is), fin de bloc (fin/end), callable
 * (fonction/function), controle (si/if, pour/for, tantque/while, sinon/else,
 * renvoyer|retourner/return, selon/switch). Egalite ou aucun signal ->
 * francais (canon par defaut). Sert a localiser le libelle "Declarations
 * globales" / "Global declarations". */
static bool module_is_english(const char* src, int srcLen) {
    char* clean = strip_block_comments(src, srcLen);
    if (clean) src = clean;
    int count = 0;
    LineInfo* lines = split_lines(src, srcLen, &count);
    int fr = 0, en = 0;
    for (int l = 0; l < count; l++) {
        Word w[5];
        int n = line_words(&src[lines[l].start], lines[l].len, w, 5);
        for (int k = 0; k < n; k++) {
            const char* z = w[k].norm;
            if (strcmp(z, "est") == 0 || strcmp(z, "fin") == 0 ||
                strcmp(z, "fonction") == 0 || strcmp(z, "si") == 0 ||
                strcmp(z, "pour") == 0 || strcmp(z, "tantque") == 0 ||
                strcmp(z, "sinon") == 0 || strcmp(z, "renvoyer") == 0 ||
                strcmp(z, "retourner") == 0 || strcmp(z, "selon") == 0) {
                fr++;
            } else if (strcmp(z, "is") == 0 || strcmp(z, "end") == 0 ||
                       strcmp(z, "function") == 0 || strcmp(z, "if") == 0 ||
                       strcmp(z, "for") == 0 || strcmp(z, "while") == 0 ||
                       strcmp(z, "else") == 0 || strcmp(z, "return") == 0 ||
                       strcmp(z, "switch") == 0) {
                en++;
            }
        }
    }
    free(lines);
    free(clean);
    return en > fr;
}

/* Libelle localise du noeud regroupant les declarations de variables
 * globales top-level. Partage par les 3 chemins (fenetre/etat, fichier plat,
 * bloc CollectionDeProcedures) pour garantir un libelle unique. FR canon par
 * defaut ; EN si le fichier vote anglais. UTF-8 pour la forme FR. */
static const char* declarations_globales_label(const char* src, int srcLen) {
    return module_is_english(src, srcLen) ? "Global declarations"
                                          : "D\xc3\xa9" "clarations globales";
}

static void parse_module(const char* src, int srcLen, ModStructure* ms) {
    memset(ms, 0, sizeof(*ms));

    char* clean = strip_block_comments(src, srcLen);
    if (clean) src = clean;

    int count = 0;
    LineInfo* lines = split_lines(src, srcLen, &count);

    int i = 0;
    while (i < count) {
        Word w[5];
        int n = line_words(&src[lines[i].start], lines[i].len, w, 5);
        if (n == 0) { i++; continue; }
        int ti = decl_type_index(w, n);

        if (is_callable_kw(&w[0])) {
            /* Callable libre top-level. */
            int blockEnd = find_block_end(lines, count, i, lines[i].indent);
            Word nm;
            if (resolve_callable_name(w, n, &nm)) {
                ModItem* it = mod_new_item(ms);
                if (it) {
                    copy_word_name(it->name, &nm);
                    it->kind = KIND_FUNCTION;
                    it->startLine = i;
                    it->endLine = blockEnd;
                }
            }
            i = blockEnd + 1;
        } else if (is_kw_constant(&w[0])) {
            /* Constante(s) top-level : forme inline `CONSTANT NOM = valeur`
             * ou bloc `CONSTANTE ... FIN`. */
            if (n >= 4 && eq(&w[2], "=")) {
                ModItem* it = mod_new_item(ms);
                if (it) {
                    copy_word_name(it->name, &w[1]);
                    extract_value_after_eq(&src[lines[i].start], lines[i].len,
                                           it->detail, MAX_NAME);
                    it->kind = KIND_CONSTANT;
                    it->startLine = i;
                    it->endLine = i;
                }
                i++;
            } else {
                int blockEnd = find_block_end(lines, count, i, lines[i].indent);
                ModItem* it = mod_new_item(ms);
                if (it) {
                    it->isContainer = true;
                    copy_word_name(it->name, &w[0]);   /* "CONSTANTE"/"CONSTANT" */
                    it->kind = KIND_CONSTANT;
                    it->startLine = i;
                    it->endLine = blockEnd;
                    scan_container_members(src, lines, count, i + 1, blockEnd,
                                           KIND_CONSTANT, it);
                }
                i = blockEnd + 1;
            }
        } else if (ti > 0) {
            /* Bloc conteneur (classe/enum/structure) OU declaration mono-ligne. */
            int cend = container_block_end(lines, i, lines[i].indent, count);
            if (cend >= 0) {
                ModItem* it = mod_new_item(ms);
                if (it) {
                    it->isContainer = true;
                    it->isCollection = is_type_collection(&w[ti]);
                    copy_word_name(it->name, &w[0]);
                    it->kind = block_opener_kind(&w[ti]);
                    it->startLine = i;
                    it->endLine = cend;
                    scan_container_members(src, lines, count, i + 1, cend,
                                           it->kind, it);
                }
                i = cend + 1;
            } else {
                /* Declaration mono-ligne : variable top-level. */
                ModItem* it = mod_new_item(ms);
                if (it) {
                    copy_word_name(it->name, &w[0]);
                    copy_word_name(it->detail, &w[ti]);   /* type declare */
                    it->kind = KIND_VARIABLE;
                    it->startLine = i;
                    it->endLine = i;
                }
                i++;
            }
        } else {
            /* Ligne non-declarative top-level (code imperatif, commentaire) :
             * ignoree, seules les declarations peuplent l'outline. */
            i++;
        }
    }

    free(lines);
    free(clean);
}

/* ----------------------------------------------------------------
 * Modele "projet" : fichier projet.owl (bloc `est un Projet`).
 * On expose la racine du projet, son bloc PROPRIETES (metadonnees) et
 * les declarations mono-ligne du corps du projet.
 * ---------------------------------------------------------------- */

typedef struct {
    char name[MAX_NAME];    /* cle de propriete OU nom de variable */
    char detail[MAX_NAME];  /* valeur (propriete) OU type (variable) */
    int  line;
    int  kind;              /* KIND_PROPERTY (metadonnee) ou KIND_VARIABLE */
} ProjChild;

typedef struct {
    bool hasProject;
    char name[MAX_NAME];
    char detail[MAX_NAME];  /* type ecrit : "Projet"/"Project" */
    int  startLine;
    int  endLine;

    bool hasProps;          /* bloc PROPRIETES present */
    char propsName[MAX_NAME]; /* mot-cle tel qu'ecrit (PROPRIETES/PROPERTIES) */
    int  propsStart;
    int  propsEnd;
    ProjChild props[MAX_DECLS];
    int  propCount;

    ProjChild vars[MAX_DECLS];  /* declarations mono-ligne du corps */
    int  varCount;
} ProjStructure;

static void parse_project(const char* src, int srcLen, ProjStructure* ps) {
    memset(ps, 0, sizeof(*ps));

    char* clean = strip_block_comments(src, srcLen);
    if (clean) src = clean;

    int count = 0;
    LineInfo* lines = split_lines(src, srcLen, &count);

    /* Repere la ligne racine `<NOM> est un/une Projet`. */
    int projLine = -1;
    int projTi = 0;
    Word w[5];
    for (int l = 0; l < count; l++) {
        int n = line_words(&src[lines[l].start], lines[l].len, w, 5);
        int ti = decl_type_index(w, n);
        if (ti > 0 && is_type_project(&w[ti])) {
            projLine = l;
            projTi = ti;
            break;
        }
    }
    if (projLine < 0) { free(lines); free(clean); return; }

    ps->hasProject = true;
    copy_word_name(ps->name, &w[0]);
    copy_word_name(ps->detail, &w[projTi]);   /* "Projet"/"Project" */
    ps->startLine = projLine;
    ps->endLine = find_block_end(lines, count, projLine, lines[projLine].indent);

    /* Corps du projet : reperer le bloc PROPRIETES et les decls mono-ligne. */
    int j = projLine + 1;
    while (j < ps->endLine) {
        Word cw[5];
        int cn = line_words(&src[lines[j].start], lines[j].len, cw, 5);
        if (cn == 0) { j++; continue; }
        int cti = decl_type_index(cw, cn);

        if (is_kw_proprietes(&cw[0])) {
            int pend = find_block_end(lines, count, j, lines[j].indent);
            ps->hasProps = true;
            copy_word_name(ps->propsName, &cw[0]);
            ps->propsStart = j;
            ps->propsEnd = pend;
            for (int p = j + 1; p < pend; p++) {
                Word pw[3];
                int pn = line_words(&src[lines[p].start], lines[p].len, pw, 3);
                if (pn >= 2 && eq(&pw[1], "=") && ps->propCount < MAX_DECLS) {
                    ProjChild* pc = &ps->props[ps->propCount++];
                    copy_word_name(pc->name, &pw[0]);
                    extract_value_after_eq(&src[lines[p].start], lines[p].len,
                                           pc->detail, MAX_NAME);
                    pc->line = p;
                    pc->kind = KIND_PROPERTY;
                }
            }
            j = pend + 1;
        } else if (cti > 0 && !is_type_champ(&cw[cti])) {
            /* Declaration mono-ligne du corps (variable du programme projet). */
            if (ps->varCount < MAX_DECLS) {
                ProjChild* vc = &ps->vars[ps->varCount++];
                copy_word_name(vc->name, &cw[0]);
                copy_word_name(vc->detail, &cw[cti]);
                vc->line = j;
                vc->kind = KIND_VARIABLE;
            }
            j++;
        } else {
            j++;
        }
    }

    free(lines);
    free(clean);
}

/* ----------------------------------------------------------------
 * Modele "theme" : fichier `<Nom> est un Thème` (EN `is a Theme`).
 * Un theme groupe des styles nommes : `<nom> est un style<Type> = { ... }`.
 * On expose deux niveaux : racine Theme -> styles nommes. Le litteral `{ ... }`
 * (Libellé/CadreExtérieur/Etats/Couleur...) est la CONFIG du style, PAS un
 * symbole de navigation : on ne descend jamais dedans (sinon "le vrac").
 * ---------------------------------------------------------------- */

typedef struct {
    char name[MAX_NAME];    /* nom du style (ex "boutonPrimaire") */
    char detail[MAX_NAME];  /* type ecrit "style<Type>" (ex "styleBouton") */
    int  startLine;
    int  endLine;
} ThemeStyle;

typedef struct {
    bool hasTheme;
    char name[MAX_NAME];
    char detail[MAX_NAME];  /* type ecrit : "Thème"/"Theme" */
    int  startLine;
    int  endLine;
    ThemeStyle styles[MAX_DECLS];
    int  styleCount;
} ThemeStructure;

/* Fin d'un bloc `... = { ... }` ouvert a la ligne openLine : suit l'equilibre
 * des accolades jusqu'au retour a zero. Fallback : limit-1 si desequilibre. */
static int brace_block_end(const LineInfo* lines, const char* src,
                           int openLine, int limit) {
    int depth = 0;
    bool seen = false;
    for (int i = openLine; i < limit; i++) {
        const char* p = &src[lines[i].start];
        int len = lines[i].len;
        for (int k = 0; k < len; k++) {
            if (p[k] == '{') { depth++; seen = true; }
            else if (p[k] == '}') {
                depth--;
                if (seen && depth == 0) return i;
            }
        }
    }
    return limit - 1;
}

static void parse_theme(const char* src, int srcLen, ThemeStructure* ts) {
    memset(ts, 0, sizeof(*ts));

    char* clean = strip_block_comments(src, srcLen);
    if (clean) src = clean;

    int count = 0;
    LineInfo* lines = split_lines(src, srcLen, &count);

    /* Repere la ligne racine `<NOM> est un/une Thème`. */
    int themeLine = -1;
    int themeTi = 0;
    Word w[5];
    for (int l = 0; l < count; l++) {
        int n = line_words(&src[lines[l].start], lines[l].len, w, 5);
        int ti = decl_type_index(w, n);
        if (ti > 0 && is_type_theme(&w[ti])) {
            themeLine = l;
            themeTi = ti;
            break;
        }
    }
    if (themeLine < 0) { free(lines); free(clean); return; }

    ts->hasTheme = true;
    copy_word_name(ts->name, &w[0]);
    copy_word_name(ts->detail, &w[themeTi]);   /* "Thème"/"Theme" orthographe source */
    ts->startLine = themeLine;
    ts->endLine = find_block_end(lines, count, themeLine, lines[themeLine].indent);

    int themeIndent = lines[themeLine].indent;

    /* Corps : declarations de style au niveau DIRECT du theme uniquement
     * (indentation = themeIndent+1, forme canonique tabulee). On saute le
     * litteral `{ ... }` de chaque style sans descendre dedans. */
    int j = themeLine + 1;
    while (j < ts->endLine) {
        Word cw[5];
        int cn = line_words(&src[lines[j].start], lines[j].len, cw, 5);
        if (cn == 0) { j++; continue; }
        int cti = decl_type_index(cw, cn);

        if (cti > 0 && lines[j].indent == themeIndent + 1 &&
            is_type_style(&cw[cti])) {
            int sEnd = brace_block_end(lines, src, j, ts->endLine);
            if (ts->styleCount < MAX_DECLS) {
                ThemeStyle* st = &ts->styles[ts->styleCount++];
                copy_word_name(st->name, &cw[0]);
                copy_word_name(st->detail, &cw[cti]);   /* "style<Type>" source */
                st->startLine = j;
                st->endLine = sEnd;
            }
            j = sEnd + 1;   /* saute le litteral : pas de descente */
        } else {
            j++;
        }
    }

    free(lines);
    free(clean);
}

/* ================================================================
 * Construction JSON
 * ================================================================ */

static JsonValue* make_position(int line, int character) {
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(line));
    json_object_set(pos, "character", json_new_int(character));
    return pos;
}

static JsonValue* make_range(int sLine, int sChar, int eLine, int eChar) {
    JsonValue* r = json_new_object();
    json_object_set(r, "start", make_position(sLine, sChar));
    json_object_set(r, "end", make_position(eLine, eChar));
    return r;
}

/* Longueur (hors CR/LF) de la ligne d'index `line` dans src. Sert a placer
 * range.end.character a la fin de la ligne de fermeture FIN (FIN inclus),
 * pour que l'extraction/reinjection cote editeur couvre tout le bloc. */
static int line_len_at(const char* src, int srcLen, int line) {
    int start = 0, curLine = 0, i = 0;
    while (i < srcLen && curLine < line) {
        if (src[i] == '\n') { curLine++; start = i + 1; }
        i++;
    }
    if (curLine < line) return 0;  /* ligne hors borne */
    int end = start;
    while (end < srcLen && src[end] != '\n') end++;
    if (end > start && src[end - 1] == '\r') end--;
    return end - start;
}

/* DocumentSymbol avec children vide (renseigne par l'appelant). selectionRange
 * = ligne d'ouverture (toujours incluse dans range). range.end pointe a la fin
 * de la ligne de fermeture (eChar = longueur de cette ligne). */
static JsonValue* make_symbol(const char* name, int kind,
                              int sLine, int eLine, int eChar, int selLineLen) {
    JsonValue* sym = json_new_object();
    json_object_set(sym, "name", json_new_string(name));
    json_object_set(sym, "kind", json_new_int(kind));
    json_object_set(sym, "range", make_range(sLine, 0, eLine, eChar));
    json_object_set(sym, "selectionRange", make_range(sLine, 0, sLine, selLineLen));
    json_object_set(sym, "children", json_new_array());
    return sym;
}

static JsonValue* sym_children_arr(JsonValue* sym) {
    return json_get(sym, "children");
}

/* Renseigne le champ optionnel "detail" d'un DocumentSymbol (no-op si vide). */
static void sym_set_detail(JsonValue* sym, const char* detail) {
    if (sym && detail && detail[0])
        json_object_set(sym, "detail", json_new_string(detail));
}

/* Construit recursivement le DocumentSymbol d'un noeud (conteneur ou proc)
 * et de toute sa descendance. */
static JsonValue* build_node_symbol(const FenStructure* fs, int nodeIdx,
                                    const char* src, int srcLen) {
    const SymNode* n = &fs->nodes[nodeIdx];
    JsonValue* sym = make_symbol(n->name, n->kind, n->startLine, n->endLine,
                                 line_len_at(src, srcLen, n->endLine), 0);
    sym_set_detail(sym, n->detail);
    JsonValue* children = sym_children_arr(sym);
    for (int c = n->firstChild; c >= 0; c = fs->nodes[c].nextSibling) {
        json_array_push(children, build_node_symbol(fs, c, src, srcLen));
    }
    return sym;
}

/* Construit le DocumentSymbol d'un membre de conteneur (module/collection) et,
 * recursivement, de ses enfants (cas d'une structure imbriquee portant ses
 * champs). */
static JsonValue* build_member_symbol(const ClassMember* cm,
                                      const char* src, int srcLen) {
    JsonValue* sym = make_symbol(cm->name, cm->kind, cm->startLine, cm->endLine,
                                 line_len_at(src, srcLen, cm->endLine), 0);
    sym_set_detail(sym, cm->detail);
    if (cm->childCount > 0) {
        JsonValue* children = sym_children_arr(sym);
        for (int i = 0; i < cm->childCount; i++)
            json_array_push(children, build_member_symbol(&cm->children[i], src, srcLen));
    }
    return sym;
}

/* Resout le document courant a partir des params {textDocument:{uri}}. */
static const char* doc_content(JsonValue* params, int* outLen) {
    JsonValue* td = params ? json_get(params, "textDocument") : NULL;
    const char* uri = td ? json_get_string(td, "uri") : NULL;
    if (!uri) return NULL;
    for (int i = 0; i < lspServer.documentCount; i++) {
        if (strcmp(lspServer.documents[i].uri, uri) == 0) {
            if (outLen) *outLen = lspServer.documents[i].contentLen;
            return lspServer.documents[i].content;
        }
    }
    return NULL;
}

JsonValue* handle_document_symbol(JsonValue* params) {
    int len = 0;
    const char* src = doc_content(params, &len);
    JsonValue* result = json_new_array();
    if (!src) return result;

    /* FenStructure : heap-allouee, ses buffers internes (decls, nodes)
     * croissent a la demande sans borne fixe (cf. fen_structure_free). */
    FenStructure* fs = malloc(sizeof(FenStructure));
    if (!fs) return result;
    parse_structure(src, len, fs);
    if (!fs->hasWindow) {
        fen_structure_free(fs);

        /* projet.owl : racine `est un Projet` -> structure dediee. */
        ProjStructure* ps = malloc(sizeof(ProjStructure));
        if (!ps) return result;
        parse_project(src, len, ps);
        if (ps->hasProject) {
            JsonValue* root = make_symbol(ps->name, KIND_MODULE,
                                          ps->startLine, ps->endLine,
                                          line_len_at(src, len, ps->endLine), 0);
            sym_set_detail(root, ps->detail);
            JsonValue* rootCh = sym_children_arr(root);

            if (ps->hasProps) {
                /* Nom canon tel qu'ecrit (FR PROPRIETES / EN PROPERTIES). */
                JsonValue* props = make_symbol(ps->propsName, KIND_NAMESPACE,
                                               ps->propsStart, ps->propsEnd,
                                               line_len_at(src, len, ps->propsEnd), 0);
                JsonValue* propsCh = sym_children_arr(props);
                for (int p = 0; p < ps->propCount; p++) {
                    ProjChild* pc = &ps->props[p];
                    JsonValue* keySym = make_symbol(pc->name, pc->kind,
                                                    pc->line, pc->line,
                                                    line_len_at(src, len, pc->line), 0);
                    sym_set_detail(keySym, pc->detail);
                    json_array_push(propsCh, keySym);
                }
                json_array_push(rootCh, props);
            }
            for (int v = 0; v < ps->varCount; v++) {
                ProjChild* vc = &ps->vars[v];
                JsonValue* vSym = make_symbol(vc->name, vc->kind,
                                              vc->line, vc->line,
                                              line_len_at(src, len, vc->line), 0);
                sym_set_detail(vSym, vc->detail);
                json_array_push(rootCh, vSym);
            }
            json_array_push(result, root);
            free(ps);
            return result;
        }
        free(ps);

        /* theme.owl : racine `est un Thème` -> racine + styles nommes. */
        ThemeStructure* ts = malloc(sizeof(ThemeStructure));
        if (!ts) return result;
        parse_theme(src, len, ts);
        if (ts->hasTheme) {
            JsonValue* root = make_symbol(ts->name, KIND_MODULE,
                                          ts->startLine, ts->endLine,
                                          line_len_at(src, len, ts->endLine), 0);
            sym_set_detail(root, ts->detail);
            JsonValue* rootCh = sym_children_arr(root);
            for (int s = 0; s < ts->styleCount; s++) {
                ThemeStyle* st = &ts->styles[s];
                JsonValue* sSym = make_symbol(st->name, KIND_VARIABLE,
                                              st->startLine, st->endLine,
                                              line_len_at(src, len, st->endLine), 0);
                sym_set_detail(sSym, st->detail);
                json_array_push(rootCh, sSym);
            }
            json_array_push(result, root);
            free(ts);
            return result;
        }
        free(ts);

        /* Sinon mode module/script -> outline generique. */
        ModStructure* ms = malloc(sizeof(ModStructure));
        if (!ms) return result;
        parse_module(src, len, ms);
        const char* dgLabel = declarations_globales_label(src, len);

        /* Modele "Declarations globales en tete" (identique a la fenetre) : les
         * declarations de variables globales top-level sont regroupees sous UN
         * noeud "Declarations globales" (Namespace) pose EN PREMIER, dont elles
         * sont les enfants. Declarations SEULES : le code imperatif n'est pas
         * capture. Range = 1re -> derniere declaration. Emis des qu'il y a >=1
         * declaration de variable globale. */
        {
            int declFirst = -1, declLast = -1;
            for (int it = 0; it < ms->itemCount; it++) {
                ModItem* m = &ms->items[it];
                if (m->isContainer || m->kind != KIND_VARIABLE) continue;
                if (declFirst < 0 || m->startLine < declFirst) declFirst = m->startLine;
                if (m->endLine > declLast) declLast = m->endLine;
            }
            if (declFirst >= 0) {
                JsonValue* dgSym = make_symbol(
                    dgLabel, KIND_NAMESPACE, declFirst, declLast,
                    line_len_at(src, len, declLast), 0);
                JsonValue* dgCh = sym_children_arr(dgSym);
                for (int it = 0; it < ms->itemCount; it++) {
                    ModItem* m = &ms->items[it];
                    if (m->isContainer || m->kind != KIND_VARIABLE) continue;
                    JsonValue* vSym = make_symbol(m->name, m->kind,
                                                  m->startLine, m->endLine,
                                                  line_len_at(src, len, m->endLine), 0);
                    sym_set_detail(vSym, m->detail);
                    json_array_push(dgCh, vSym);
                }
                json_array_push(result, dgSym);
            }
        }

        /* Freres, dans l'ordre source : tout SAUF les variables top-level (deja
         * regroupees sous "Declarations globales"). Callables, enums, structures,
         * classes, blocs CONSTANTE, collections. */
        for (int it = 0; it < ms->itemCount; it++) {
            ModItem* m = &ms->items[it];
            if (!m->isContainer && m->kind == KIND_VARIABLE) continue;
            JsonValue* itSym = make_symbol(m->name, m->kind,
                                           m->startLine, m->endLine,
                                           line_len_at(src, len, m->endLine), 0);
            sym_set_detail(itSym, m->detail);
            if (m->isContainer) {
                JsonValue* itChildren = sym_children_arr(itSym);
                if (m->isCollection) {
                    /* Meme modele que le top-level : "Declarations globales" en
                     * tete regroupe les declarations de variables globales
                     * (Field) du bloc, SEULES ; les autres membres (procedures,
                     * valeurs nommees) restent freres, apres. Range = 1re ->
                     * derniere declaration. */
                    int cFirst = -1, cLast = -1;
                    for (int mm = 0; mm < m->memberCount; mm++) {
                        ClassMember* cm = &m->members[mm];
                        if (cm->kind != KIND_FIELD) continue;
                        if (cFirst < 0 || cm->startLine < cFirst) cFirst = cm->startLine;
                        if (cm->endLine > cLast) cLast = cm->endLine;
                    }
                    if (cFirst >= 0) {
                        JsonValue* dgSym = make_symbol(
                            dgLabel, KIND_NAMESPACE, cFirst, cLast,
                            line_len_at(src, len, cLast), 0);
                        JsonValue* dgCh = sym_children_arr(dgSym);
                        for (int mm = 0; mm < m->memberCount; mm++) {
                            ClassMember* cm = &m->members[mm];
                            if (cm->kind != KIND_FIELD) continue;
                            JsonValue* memSym = make_symbol(cm->name, cm->kind,
                                                            cm->startLine, cm->endLine,
                                                            line_len_at(src, len, cm->endLine), 0);
                            sym_set_detail(memSym, cm->detail);
                            json_array_push(dgCh, memSym);
                        }
                        json_array_push(itChildren, dgSym);
                    }
                    for (int mm = 0; mm < m->memberCount; mm++) {
                        ClassMember* cm = &m->members[mm];
                        if (cm->kind == KIND_FIELD) continue;
                        json_array_push(itChildren, build_member_symbol(cm, src, len));
                    }
                } else {
                    /* Classe/enum/structure/CONSTANTE : membres (avec enfants
                     * recursifs pour un conteneur imbrique). */
                    for (int mm = 0; mm < m->memberCount; mm++) {
                        json_array_push(itChildren,
                                        build_member_symbol(&m->members[mm], src, len));
                    }
                }
            }
            json_array_push(result, itSym);
        }
        mod_structure_free(ms);
        return result;
    }

    JsonValue* root = make_symbol(fs->windowName, KIND_CLASS,
                                  fs->windowStart, fs->windowEnd,
                                  line_len_at(src, len, fs->windowEnd), 0);
    JsonValue* rootChildren = sym_children_arr(root);

    /* Noeud "Declarations globales" si au moins une declaration corps. */
    if (fs->declCount > 0) {
        int first = fs->decls[0].line;
        int last = fs->decls[fs->declCount - 1].line;
        JsonValue* dg = make_symbol(declarations_globales_label(src, len),
                                    KIND_NAMESPACE, first, last,
                                    line_len_at(src, len, last), 0);
        JsonValue* dgChildren = sym_children_arr(dg);
        for (int d = 0; d < fs->declCount; d++) {
            JsonValue* v = make_symbol(fs->decls[d].name, KIND_VARIABLE,
                                       fs->decls[d].line, fs->decls[d].line,
                                       line_len_at(src, len, fs->decls[d].line), 0);
            sym_set_detail(v, fs->decls[d].detail);
            json_array_push(dgChildren, v);
        }
        json_array_push(rootChildren, dg);
    }

    /* Conteneurs (champs/sections) et procedures du widget, dans l'ordre
     * d'apparition. Recursion : une section expose ses champs/sous-sections
     * (Field) et ses evenements (Method). */
    for (int c = fs->rootFirst; c >= 0; c = fs->nodes[c].nextSibling) {
        json_array_push(rootChildren, build_node_symbol(fs, c, src, len));
    }

    json_array_push(result, root);
    fen_structure_free(fs);
    return result;
}

JsonValue* handle_folding_range(JsonValue* params) {
    int len = 0;
    const char* src = doc_content(params, &len);
    JsonValue* result = json_new_array();
    if (!src) return result;

    /* FenStructure : buffers internes dynamiques (cf. handle_document_symbol). */
    FenStructure* fs = malloc(sizeof(FenStructure));
    if (!fs) return result;
    parse_structure(src, len, fs);
    if (!fs->hasWindow) {
        /* Mode module/script : une plage par callable/classe top-level
         * et par methode de classe. */
        fen_structure_free(fs);
        ModStructure* ms = malloc(sizeof(ModStructure));
        if (!ms) return result;
        parse_module(src, len, ms);
        for (int it = 0; it < ms->itemCount; it++) {
            ModItem* m = &ms->items[it];
            /* Plage seulement pour les items multi-lignes (callable/conteneur) :
             * une decl mono-ligne (variable/constante inline) n'est pas repliable. */
            if (m->endLine > m->startLine) {
                JsonValue* fr = json_new_object();
                json_object_set(fr, "startLine", json_new_int(m->startLine));
                json_object_set(fr, "endLine", json_new_int(m->endLine));
                json_array_push(result, fr);
            }
            if (m->isContainer) {
                for (int mm = 0; mm < m->memberCount; mm++) {
                    ClassMember* cm = &m->members[mm];
                    if (cm->kind != KIND_METHOD) continue;
                    JsonValue* mr = json_new_object();
                    json_object_set(mr, "startLine", json_new_int(cm->startLine));
                    json_object_set(mr, "endLine", json_new_int(cm->endLine));
                    json_array_push(result, mr);
                }
            }
        }
        mod_structure_free(ms);
        return result;
    }

    /* Une plage repliable par noeud conteneur/proc, a toute profondeur
     * (champs, sections d'etat, sous-champs, evenements). */
    for (int n = 0; n < fs->nodeCount; n++) {
        JsonValue* fr = json_new_object();
        json_object_set(fr, "startLine", json_new_int(fs->nodes[n].startLine));
        json_object_set(fr, "endLine", json_new_int(fs->nodes[n].endLine));
        json_array_push(result, fr);
    }

    fen_structure_free(fs);
    return result;
}
