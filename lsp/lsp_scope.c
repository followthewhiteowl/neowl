// SPDX-License-Identifier: AGPL-3.0-only
/* Moteur de portee/liaison du LSP ( Lot 0, ).
 *
 * Voir lsp_scope.h pour le contrat. Ce module promeut le proto-resolveur
 * de portee jusqu'ici confine a handle_semantic_tokens ( : pile
 * scopeStack2 + procLocals, coloration seule) en tables PERSISTANTES,
 * consommables par le go-to-definition scope-correct.
 *
 * Note de dette (dedup) : handle_semantic_tokens continue de reconstruire
 * SA propre pile de portee pour ce lot — le refactor pour lui faire
 * consommer ce module est reporte (Lot 0b, cf. brief), le couplage avec
 * la grosse suite semantic-tokens etant trop serre pour etre sur ici. */

#include "lsp_scope.h"
#include "lsp_server.h"
#include "../src/scanner.h"
#include "../src/prescan.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

/* ---- Plafonds d'arene (dimensionnes prudemment,  fichiers
 * 1000+ .owl : le moteur ne couvre QUE les docs ouverts — poignee de
 * buffers, les compagnons fermes passent par les fallbacks disque). ---- */
#define SCOPE_MAX_NODES     4096
#define SCOPE_MAX_BINDINGS  4096
#define SCOPE_MAX_OCC        256   /* occurrences par binding : declaration +
                                    * usages lies (Lot 1). 16 tronquait des
                                    * variables courantes ; overflow au-dela
                                    * cape silencieusement (pas d'UB). */
#define SCOPE_MAX_TOKENS    8192   /* par document */
#define SCOPE_STACK_MAX       64

/* Genres de portee (miroir de l'enum local de handle_semantic_tokens). */
enum { KIND_TOPLEVEL = 0, KIND_CLASS, KIND_PROC, KIND_OTHER_ISOLATE };

typedef struct {
    int id;
    int parent;      /* -1 pour la racine d'un document */
    int kind;
    int docIndex;
    int startLine, startChar;  /* 0-based */
    int endLine,   endChar;    /* 0-based ; grand tant que non ferme */
} ScopeNode;

typedef struct { int line, character; } Occurrence;

typedef struct {
    char canonKey[128];  /* identifiant canonicalise (casse/accent-insensible,
                          * bilingue) via scanner_canonicalize_identifier */
    int  axis;           /* ScopeAxis */
    int  declScope;      /* id du ScopeNode de declaration */
    int  docIndex;
    int  declLine, declChar; /* 0-based, position du nom (verbatim) */
    int  nameLen;        /* octets du nom declare */
    Occurrence occ[SCOPE_MAX_OCC];
    int  occCount;
    bool isUserDeclared;
} Binding;

static ScopeNode g_scopes[SCOPE_MAX_NODES];
static int       g_scopeCount = 0;
static Binding   g_bindings[SCOPE_MAX_BINDINGS];
static int       g_bindingCount = 0;

/* ---- helpers ---- */

static int scope_find_doc(const char* uri) {
    for (int i = 0; i < lspServer.documentCount; i++) {
        if (strcmp(lspServer.documents[i].uri, uri) == 0) return i;
    }
    return -1;
}

static int scope_new_node(int parent, int kind, int docIndex,
                          int startLine, int startChar) {
    if (g_scopeCount >= SCOPE_MAX_NODES) return -1;
    ScopeNode* s = &g_scopes[g_scopeCount];
    s->id = g_scopeCount;
    s->parent = parent;
    s->kind = kind;
    s->docIndex = docIndex;
    s->startLine = startLine;
    s->startChar = startChar;
    s->endLine = INT_MAX;
    s->endChar = INT_MAX;
    return g_scopeCount++;
}

/* Un mot-cle qui, place juste apres `est`, denote une DECLARATION DE TYPE
 * (`cBoite est une Classe`) et non une valeur : on n'en fait pas un binding
 * VALUE (le go-to-def d'un nom de type reste servi par le lookup classes/
 * structs/enums existant de handle_definition). */
static bool scope_is_type_decl_keyword(TokenType t) {
    switch (t) {
        case TOKEN_CLASSE:
        case TOKEN_STRUCTURE:
        case TOKEN_ENUMERATION:
        case TOKEN_COMBINAISON:
        case TOKEN_INTERFACE:
            return true;
        default:
            return false;
    }
}

/* Resolution partagee entre scope_resolve_at (go-to-def) et la 2e passe de
 * peuplement des usages : definie plus bas (section resolution). */
static int scope_resolve_binding(int docIndex, int line, int character,
                                 const char* canonKey);

/* ---- construction ---- */

static void scope_build_one_doc(int docIndex, const char* source) {
    if (!source) return;

    Scanner sc;
    scanner_init(&sc, source);

    Token toks[SCOPE_MAX_TOKENS];
    int nToks = 0;
    while (nToks < SCOPE_MAX_TOKENS) {
        Token t = scanner_scan_token(&sc);
        toks[nToks++] = t;
        if (t.type == TOKEN_EOF) break;
    }

    /* Portee racine du document (jamais fermee : les FIN parasites d'un
     * bloc DSL top-level ne doivent pas l'amputer). */
    int rootId = scope_new_node(-1, KIND_TOPLEVEL, docIndex, 0, 0);
    if (rootId < 0) return;

    typedef struct { int scopeId; int subDepth; } Frame;
    Frame stack[SCOPE_STACK_MAX];
    int top = 0;
    stack[0].scopeId = rootId;
    stack[0].subDepth = 0;

    for (int ti = 0; ti < nToks; ti++) {
        Token t = toks[ti];
        int line0 = t.line > 0 ? t.line - 1 : 0;
        int char0 = t.column > 0 ? t.column - 1 : 0;

        /* Ouvertures ISOLANTES : nouvelle portee lexicale. */
        int openKind = -1;
        if (t.type == TOKEN_CLASSE) openKind = KIND_CLASS;
        else if (t.type == TOKEN_PROCEDURE || t.type == TOKEN_FONCTION) openKind = KIND_PROC;
        else if (t.type == TOKEN_STRUCTURE || t.type == TOKEN_ENUMERATION ||
                 t.type == TOKEN_COMBINAISON || t.type == TOKEN_INTERFACE)
            openKind = KIND_OTHER_ISOLATE;

        if (openKind >= 0) {
            if (top + 1 < SCOPE_STACK_MAX) {
                int parentId = stack[top].scopeId;
                int nid = scope_new_node(parentId, openKind, docIndex, line0, char0);
                if (nid >= 0) {
                    top++;
                    stack[top].scopeId = nid;
                    stack[top].subDepth = 0;
                }
            }
            continue;
        }

        /* Fermeture : FIN depile le sous-bloc courant (SI/POUR/...) ou clot
         * la portee isolante. La racine ne se ferme jamais. */
        if (t.type == TOKEN_FIN) {
            if (stack[top].subDepth > 0) {
                stack[top].subDepth--;
            } else if (top > 0) {
                int sid = stack[top].scopeId;
                g_scopes[sid].endLine = line0;
                g_scopes[sid].endChar = char0;
                top--;
            }
            continue;
        }

        /* Sous-blocs TRANSPARENTS (fermes par FIN mais sans nouvelle portee) :
         * AVEC, PROPRIETES, et tout ouvreur connu de la table de profondeur
         * (SI/POUR/TANTQUE/SELON/BOUCLE + blocs DSL Fenetre/Etat/Projet/
         * Collection). Miroir de la balance FIN de scan_one_source. */
        if (t.type == TOKEN_AVEC || t.type == TOKEN_PROPRIETES) {
            stack[top].subDepth++;
            continue;
        }
        if (prescan_token_in_depth_table(t.type) && t.type != TOKEN_FIN &&
            t.type != TOKEN_PROPRIETES) {
            int delta = 0;
            prescan_depth_delta_text(t.start, &delta);
            if (delta > 0) stack[top].subDepth++;
            continue;
        }

        /* Declaration `IDENT est <...>` : le nom declare est l'IDENT, la
         * copule EST suit immediatement (meme critere que le proto semantic
         * tokens et le scan userSymbols). */
        if (t.type == TOKEN_IDENTIFIER && ti + 1 < nToks &&
            toks[ti + 1].type == TOKEN_EST) {
            TokenType after = (ti + 2 < nToks) ? toks[ti + 2].type : TOKEN_EOF;
            if (scope_is_type_decl_keyword(after)) continue; /* decl de type */

            int declScopeId = stack[top].scopeId;
            int declKind = g_scopes[declScopeId].kind;
            /* Champ de classe -> axe MEMBER (namespace disjoint des valeurs,
             *  §2). Sinon valeur (globale / locale / parametre). */
            int axis = (declKind == KIND_CLASS) ? SCOPE_AXIS_MEMBER : SCOPE_AXIS_VALUE;

            if (g_bindingCount < SCOPE_MAX_BINDINGS) {
                Binding* b = &g_bindings[g_bindingCount++];
                int n = t.length < (int)sizeof(b->canonKey) - 1
                            ? t.length : (int)sizeof(b->canonKey) - 1;
                scanner_canonicalize_identifier(t.start, n,
                                                b->canonKey, (int)sizeof(b->canonKey));
                b->axis = axis;
                b->declScope = declScopeId;
                b->docIndex = docIndex;
                b->declLine = line0;
                b->declChar = char0;
                b->nameLen = t.length;
                b->isUserDeclared = true;
                b->occCount = 0;
                /* occurrence[0] = la declaration (Lot 1 y ajoutera les usages
                 * lies via une passe de resolution ; structure prete). */
                b->occ[b->occCount].line = line0;
                b->occ[b->occCount].character = char0;
                b->occCount++;
            }
        }
    }

    /* Cloture de la racine sur le dernier token (EOF). */
    if (nToks > 0) {
        Token last = toks[nToks - 1];
        g_scopes[rootId].endLine = last.line > 0 ? last.line - 1 : INT_MAX;
        g_scopes[rootId].endChar = INT_MAX;
    }

    /* 2e passe (Lot 1) : peuple les usages. Les scopes/bindings de ce doc
     * sont maintenant complets (end positions posees) ; on rejoue les tokens
     * et, pour chaque IDENTIFIER qui est un USAGE VALUE, on resout SON binding
     * (meme algo que scope_resolve_at, via scope_resolve_binding) et on y
     * ajoute l'occurrence. occ[0] reste la declaration (posee en passe 1). */
    for (int ti = 0; ti < nToks; ti++) {
        Token t = toks[ti];
        if (t.type != TOKEN_IDENTIFIER) continue;

        /* La declaration `IDENT est ...` est deja occ[0] : on la saute (meme
         * critere que la passe 1, decl de type incluse). */
        if (ti + 1 < nToks && toks[ti + 1].type == TOKEN_EST) continue;

        /* Acces MEMBRE (`recv.champ`, `recv:m`, `recv..prop`, `recv::x`) :
         * namespace disjoint des valeurs ( §2) — pas un usage VALUE. */
        if (ti > 0) {
            TokenType prev = toks[ti - 1].type;
            if (prev == TOKEN_DOT || prev == TOKEN_COLON ||
                prev == TOKEN_DOUBLE_DOT || prev == TOKEN_DOUBLE_COLON)
                continue;
        }

        int line0 = t.line > 0 ? t.line - 1 : 0;
        int char0 = t.column > 0 ? t.column - 1 : 0;

        char key[128];
        int n = t.length < (int)sizeof(key) - 1 ? t.length : (int)sizeof(key) - 1;
        scanner_canonicalize_identifier(t.start, n, key, (int)sizeof(key));

        int b = scope_resolve_binding(docIndex, line0, char0, key);
        if (b < 0) continue;  /* pas une valeur liee (appel natif, type...) */

        Binding* bd = &g_bindings[b];
        if (bd->occCount < SCOPE_MAX_OCC) {
            bd->occ[bd->occCount].line = line0;
            bd->occ[bd->occCount].character = char0;
            bd->occCount++;
        }
    }
}

void scope_build(void) {
    g_scopeCount = 0;
    g_bindingCount = 0;
    for (int d = 0; d < lspServer.documentCount; d++) {
        scope_build_one_doc(d, lspServer.documents[d].content);
    }
}

/* ---- resolution ---- */

static bool scope_pos_in(const ScopeNode* s, int line, int character) {
    bool afterStart = (line > s->startLine) ||
                      (line == s->startLine && character >= s->startChar);
    bool beforeEnd = (line < s->endLine) ||
                     (line == s->endLine && character <= s->endChar);
    return afterStart && beforeEnd;
}

/* Portee la plus profonde du document contenant la position : parmi les
 * portees englobantes, celle dont le debut est le plus tardif (les portees
 * imbriquees commencent apres leur parente). */
static int scope_deepest_at(int docIndex, int line, int character) {
    int best = -1;
    for (int i = 0; i < g_scopeCount; i++) {
        ScopeNode* s = &g_scopes[i];
        if (s->docIndex != docIndex) continue;
        if (!scope_pos_in(s, line, character)) continue;
        /* Depart le plus tardif = portee la plus profonde. Egalite exacte
         * (proc/classe ouverte en colonne 0, meme point que la racine) : on
         * prefere l'enfant (id plus grand, cree en descendant), sinon la
         * racine capturerait les usages d'un parametre/local et casserait la
         * resolution scope-correcte. */
        if (best < 0 ||
            s->startLine > g_scopes[best].startLine ||
            (s->startLine == g_scopes[best].startLine &&
             s->startChar > g_scopes[best].startChar) ||
            (s->startLine == g_scopes[best].startLine &&
             s->startChar == g_scopes[best].startChar &&
             s->id > g_scopes[best].id)) {
            best = i;
        }
    }
    return best;
}

/* Remontee de la chaine de portees jusqu'au premier binding VALUE visible de
 * `canonKey`.  (masquage cross-scope = erreur compile) garantit que le
 * premier trouve en remontant est le bon. Renvoie l'index du binding, ou -1.
 * Partage entre le go-to-def et la 2e passe de peuplement des usages. */
static int scope_resolve_binding(int docIndex, int line, int character,
                                 const char* canonKey) {
    int sid = scope_deepest_at(docIndex, line, character);
    for (int cur = sid; cur >= 0; cur = g_scopes[cur].parent) {
        for (int b = 0; b < g_bindingCount; b++) {
            Binding* bd = &g_bindings[b];
            if (bd->docIndex != docIndex) continue;
            if (bd->axis != SCOPE_AXIS_VALUE) continue;
            if (bd->declScope != cur) continue;
            if (strcmp(bd->canonKey, canonKey) != 0) continue;
            return b;
        }
    }
    return -1;
}

bool scope_resolve_at(const char* uri, int line, int character,
                      const char* word, ScopeResolveResult* out) {
    if (out) out->found = false;
    if (!uri || !word || !word[0]) return false;

    int docIndex = scope_find_doc(uri);
    if (docIndex < 0) return false;  /* compagnon ferme -> fallback disque */

    char key[128];
    scanner_canonicalize_identifier(word, (int)strlen(word), key, (int)sizeof(key));

    int b = scope_resolve_binding(docIndex, line, character, key);
    if (b < 0) return false;

    if (out) {
        Binding* bd = &g_bindings[b];
        out->docIndex = docIndex;
        out->line = bd->declLine;
        out->character = bd->declChar;
        out->nameLen = bd->nameLen;
        out->found = true;
    }
    return true;
}

/* Index du binding VALUE dont une occurrence (declaration OU usage) couvre la
 * position : le curseur peut tomber n'importe ou dans le mot. Les occurrences
 * ayant deja ete assignees scope-correctement en 2e passe, ce choix isole
 * naturellement les homonymes de portees sœurs. Renvoie -1 si aucun. */
static int scope_binding_covering(int docIndex, int line, int character) {
    for (int b = 0; b < g_bindingCount; b++) {
        Binding* bd = &g_bindings[b];
        if (bd->docIndex != docIndex) continue;
        if (bd->axis != SCOPE_AXIS_VALUE) continue;
        for (int i = 0; i < bd->occCount; i++) {
            if (bd->occ[i].line == line &&
                character >= bd->occ[i].character &&
                character < bd->occ[i].character + bd->nameLen) {
                return b;
            }
        }
    }
    return -1;
}

int scope_references_of(const char* uri, int line, int character,
                        bool includeDeclaration,
                        ScopeReference* out, int maxOut) {
    if (!uri || !out || maxOut <= 0) return 0;

    int docIndex = scope_find_doc(uri);
    if (docIndex < 0) return 0;  /* compagnon ferme -> pas de moteur de portee */

    int found = scope_binding_covering(docIndex, line, character);
    if (found < 0) return 0;

    Binding* bd = &g_bindings[found];
    int n = 0;
    /* occ[0] = declaration ; includeDeclaration=false l'omet. */
    int startIdx = includeDeclaration ? 0 : 1;
    for (int i = startIdx; i < bd->occCount && n < maxOut; i++) {
        out[n].docIndex = docIndex;
        out[n].line = bd->occ[i].line;
        out[n].character = bd->occ[i].character;
        out[n].nameLen = bd->nameLen;
        n++;
    }
    return n;
}

int scope_rename_query(const char* uri, int line, int character,
                       bool* outRenamable, int* outNameLen,
                       bool* outIsGlobal, char* outCanonKey, int canonKeySize,
                       ScopeReference* out, int maxOut) {
    if (outRenamable) *outRenamable = false;
    if (outNameLen) *outNameLen = 0;
    if (outIsGlobal) *outIsGlobal = false;
    if (outCanonKey && canonKeySize > 0) outCanonKey[0] = '\0';
    if (!uri || !out || maxOut <= 0) return 0;

    int docIndex = scope_find_doc(uri);
    if (docIndex < 0) return 0;  /* compagnon ferme -> pas de moteur de portee */

    int found = scope_binding_covering(docIndex, line, character);
    if (found < 0) return 0;

    Binding* bd = &g_bindings[found];
    int declKind = (bd->declScope >= 0) ? g_scopes[bd->declScope].kind : -1;
    /* Renommable : binding VALUE user-declared dont la portee de declaration est
     * une PROCEDURE / FONCTION (locale/parametre, Lot 2a) OU le top-level
     * (globale, Lot 2b —  en garantit l'unicite projet-wide). */
    bool isGlobal = bd->isUserDeclared && declKind == KIND_TOPLEVEL;
    if (outRenamable) {
        *outRenamable = bd->isUserDeclared &&
                        (declKind == KIND_PROC || declKind == KIND_TOPLEVEL);
    }
    if (outIsGlobal) *outIsGlobal = isGlobal;
    if (outCanonKey && canonKeySize > 0) {
        snprintf(outCanonKey, (size_t)canonKeySize, "%s", bd->canonKey);
    }
    if (outNameLen) *outNameLen = bd->nameLen;

    int n = 0;
    /* includeDeclaration implicite = true : le rename edite decl + usages. */
    for (int i = 0; i < bd->occCount && n < maxOut; i++) {
        out[n].docIndex = docIndex;
        out[n].line = bd->occ[i].line;
        out[n].character = bd->occ[i].character;
        out[n].nameLen = bd->nameLen;
        n++;
    }
    return n;
}

int scope_collect_value_occurrences(const char* source, const char* canonKey,
                                    ScopeReference* out, int maxOut) {
    if (!source || !canonKey || !canonKey[0] || !out || maxOut <= 0) return 0;

    Scanner sc;
    scanner_init(&sc, source);

    Token toks[SCOPE_MAX_TOKENS];
    int nToks = 0;
    while (nToks < SCOPE_MAX_TOKENS) {
        Token t = scanner_scan_token(&sc);
        toks[nToks++] = t;
        if (t.type == TOKEN_EOF) break;
    }

    int n = 0;
    for (int ti = 0; ti < nToks && n < maxOut; ti++) {
        Token t = toks[ti];
        if (t.type != TOKEN_IDENTIFIER) continue;

        /* Acces MEMBRE (`recv.champ`, `recv:m`, `recv..prop`, `recv::x`) :
         * namespace disjoint des valeurs ( §2) — pas la globale. */
        if (ti > 0) {
            TokenType prev = toks[ti - 1].type;
            if (prev == TOKEN_DOT || prev == TOKEN_COLON ||
                prev == TOKEN_DOUBLE_DOT || prev == TOKEN_DOUBLE_COLON)
                continue;
        }

        char key[128];
        int kn = t.length < (int)sizeof(key) - 1 ? t.length : (int)sizeof(key) - 1;
        scanner_canonicalize_identifier(t.start, kn, key, (int)sizeof(key));
        if (strcmp(key, canonKey) != 0) continue;

        out[n].docIndex = -1;  /* URI/doc a la charge de l'appelant */
        out[n].line = t.line > 0 ? t.line - 1 : 0;
        out[n].character = t.column > 0 ? t.column - 1 : 0;
        out[n].nameLen = t.length;
        n++;
    }
    return n;
}
