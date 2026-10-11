// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "prescan.h"
#include "scanner.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/*  — perimetre de decouverte projet. Quand restrict==true (mode
 * projet), un fichier compagnon PLAT (top-level a contenu lache : proc/fonction
 * nue, globale, instruction imperative — AUCUN bloc DSL) est un script prive :
 * hors perimetre projet, il n'est ni indexe (symboles invisibles) ni memorise
 * dans sources[] (invisible a l'hydratation cross-fichier du compilateur). Le
 * fichier d'entree (entryIndex) participe TOUJOURS, meme plat. Mode script
 * (restrict==false) : inchange, tout le monde participe. */
static bool g_prescan_restrict_to_dsl = false;
static int  g_prescan_entry_index     = -1;

void prescan_set_project_scope(bool restrictToDsl, int entryIndex) {
    g_prescan_restrict_to_dsl = restrictToDsl;
    g_prescan_entry_index     = entryIndex;
}

/* ================================================================
 * SYMBOL TABLE
 * ================================================================ */

void symtable_init(SymbolTable* t) {
    t->symbols = NULL;
    t->count = 0;
    t->capacity = 0;
    t->fileNameCount = 0;
    t->interfaceCount = 0;
    t->classMethodCount = 0;
    t->classMethodVisCount = 0;  /*  */
    t->implementationCount = 0;
    t->inheritanceCount = 0;
    t->members = NULL;
    t->memberCount = 0;
    t->memberCapacity = 0;
    t->elements = NULL;          /*  */
    t->elementCount = 0;
    t->elementCapacity = 0;
    /*  : init liste compacte non-owning des sources. */
    t->sourceCount = 0;
    for (int i = 0; i < MAX_PROJECT_FILES; i++) {
        t->sources[i].fileIndex = -1;
        t->sources[i].source = NULL;
    }
}

/*  : extrait le 1er token de la type spec a partir d'un Scanner.
 * Avance le scanner jusqu'a apres la type spec (consomme TYPE [modifiers]).
 * Renseigne `out` avec le nom du type (case originale, tronque a 31).
 * `out` peut etre NULL si on veut juste skipper. */
static void prescan_capture_type_spec(Scanner* s, char* out) {
    if (out) out[0] = '\0';
    Token tok;
    for (;;) {
        tok = scanner_scan_token(s);
        if (tok.type != TOKEN_NEWLINE) break;
    }
    /* Avale l'article EN `a`/`an` : le scanner strippe les articles
     * FR (un/une/des) apres la copule, mais laisse les EN au parseur
     * (consume_en_a_an_filler). En walk prescan brut il faut les sauter ici,
     * sinon le type capture pour un champ EN (`dico is an associative array`,
     * `fond is a Couleur`) est l'article ("an"/"a") et non le vrai type — ce
     * qui casse la parite bilingue de declType ( chain resolution +
     * detection conteneur cross-fichier). Miroir de consume_en_a_an_filler. */
    bool isEnArticle =
        tok.type == TOKEN_A ||
        (tok.type == TOKEN_IDENTIFIER && tok.length == 1 &&
         (tok.start[0] == 'a' || tok.start[0] == 'A')) ||
        (tok.type == TOKEN_IDENTIFIER && tok.length == 2 &&
         (tok.start[0] == 'a' || tok.start[0] == 'A') &&
         (tok.start[1] == 'n' || tok.start[1] == 'N'));
    if (isEnArticle) {
        for (;;) {
            tok = scanner_scan_token(s);
            if (tok.type != TOKEN_NEWLINE) break;
        }
    }
    /* Capture le nom du type. */
    if (tok.type != TOKEN_EOF && tok.type != TOKEN_COMMA &&
        tok.type != TOKEN_RIGHT_PAREN) {
        if (out) {
            int n = tok.length < 31 ? tok.length : 31;
            memcpy(out, tok.start, n);
            out[n] = '\0';
        }
    }
    /* On laisse le scanner positionne sur le token suivant — le caller
     * gere les `,` ou `)`. Volontairement minimaliste : pour les types
     * composites (tableau de X), seul "tableau" est capture, ce qui est
     * la meme convention que cote compiler.c. */
}

void symtable_free(SymbolTable* t) {
    free(t->symbols);
    free(t->members);
    free(t->elements);           /*  */
    symtable_init(t);
}

/*  : allocation heap de la SymbolTable (cf. prescan.h). calloc pour un
 * zero-init coherent avec ce que la pile + symtable_init fournissaient, puis
 * symtable_init pose les sentinelles (sources[].fileIndex = -1). */
SymbolTable* symtable_create(void) {
    SymbolTable* t = calloc(1, sizeof(SymbolTable));
    symtable_init(t);
    return t;
}

void symtable_destroy(SymbolTable* t) {
    if (t == NULL) return;
    symtable_free(t);   /* libere symbols/members/elements + re-init */
    free(t);            /* libere le conteneur lui-meme */
}

/*  : enregistre un element top-level (nom + fichier + position). */
static void symtable_add_element(SymbolTable* t, const char* name, int nameLen,
                                 int fileIndex, int line, int column) {
    if (nameLen <= 0) return;
    if (t->elementCount >= t->elementCapacity) {
        int newCap = t->elementCapacity < 16 ? 16 : t->elementCapacity * 2;
        t->elements = realloc(t->elements, sizeof(ProjectElement) * newCap);
        t->elementCapacity = newCap;
    }
    ProjectElement* e = &t->elements[t->elementCount];
    int cl = nameLen < (int)sizeof(e->name) - 1 ? nameLen : (int)sizeof(e->name) - 1;
    memcpy(e->name, name, cl);
    e->name[cl] = '\0';
    e->fileIndex = fileIndex;
    e->line = line;
    e->column = column;
    t->elementCount++;
}

bool symtable_element_has_earlier_dup(SymbolTable* t, const char* name, int len,
                                      int fileIndex) {
    if (t == NULL || name == NULL || len <= 0) return false;
    char canonCur[128];
    scanner_canonicalize_identifier(name, len, canonCur, sizeof(canonCur));
    for (int i = 0; i < t->elementCount; i++) {
        ProjectElement* e = &t->elements[i];
        if (e->fileIndex >= fileIndex) continue;
        char canonOther[128];
        scanner_canonicalize_identifier(e->name, (int)strlen(e->name),
                                        canonOther, sizeof(canonOther));
        if (strcmp(canonCur, canonOther) == 0) return true;
    }
    return false;
}

/*  : ajoute un membre (champ/const/enum) au catalogue. `name` et
 * `declType` portent leur casse d'origine. `declType` peut etre vide. */
static void symtable_add_member(SymbolTable* t,
                                const char* owner, int ownerLen,
                                const char* name, int nameLen,
                                const char* declType, int declTypeLen,
                                int fileIndex,
                                int line, int column,
                                int visibility) {
    if (ownerLen <= 0 || nameLen <= 0) return;
    if (t->memberCount >= t->memberCapacity) {
        int newCap = t->memberCapacity < 32 ? 32 : t->memberCapacity * 2;
        t->members = realloc(t->members, sizeof(ProjectMember) * newCap);
        t->memberCapacity = newCap;
    }
    ProjectMember* m = &t->members[t->memberCount];
    int ol = ownerLen < (int)sizeof(m->ownerType) - 1
             ? ownerLen : (int)sizeof(m->ownerType) - 1;
    memcpy(m->ownerType, owner, ol); m->ownerType[ol] = '\0';
    int nl = nameLen < (int)sizeof(m->name) - 1
             ? nameLen : (int)sizeof(m->name) - 1;
    memcpy(m->name, name, nl); m->name[nl] = '\0';
    if (declType && declTypeLen > 0) {
        int dl = declTypeLen < (int)sizeof(m->declType) - 1
                 ? declTypeLen : (int)sizeof(m->declType) - 1;
        memcpy(m->declType, declType, dl); m->declType[dl] = '\0';
    } else {
        m->declType[0] = '\0';
    }
    m->fileIndex = fileIndex;
    m->line = line;
    m->column = column;
    m->visibility = visibility;
    t->memberCount++;
}

ProjectMember* symtable_find_member(SymbolTable* t,
                                    const char* ownerType, int ownerLen,
                                    const char* member, int memberLen) {
    for (int i = 0; i < t->memberCount; i++) {
        ProjectMember* m = &t->members[i];
        if ((int)strlen(m->ownerType) == ownerLen &&
            strncasecmp(m->ownerType, ownerType, ownerLen) == 0 &&
            (int)strlen(m->name) == memberLen &&
            strncasecmp(m->name, member, memberLen) == 0) {
            return m;
        }
    }
    return NULL;
}

/*  : casse divergente du meme nom (top-level) dans >=2 fichiers ?
 * On compare case-insensible pour matcher les homonymes, puis case-sensible
 * pour detecter la divergence de casse. Deux declarations a la meme casse
 * (meme nom canon, peu importe le fichier) ne sont PAS ambigues. */
bool symtable_type_is_ambiguous(SymbolTable* t, const char* name) {
    const char* canon = NULL;  /* casse de la 1ere occurrence */
    for (int i = 0; i < t->count; i++) {
        ProjectSymbol* s = &t->symbols[i];
        if (s->kind != SYM_CLASS && s->kind != SYM_STRUCT &&
            s->kind != SYM_ENUM && s->kind != SYM_INTERFACE) continue;
        if (strcasecmp(s->name, name) != 0) continue;
        if (canon == NULL) { canon = s->name; continue; }
        if (strcmp(canon, s->name) != 0) return true;  /* casse divergente */
    }
    return false;
}

/*  — variante "globals" (variables/constantes/procedures/fonctions
 * top-level). Meme regle que symtable_type_is_ambiguous : si un meme nom est
 * declare avec des casses divergentes dans le projet, on s'abstient cote
 * formatter (verbatim). Fonction distincte pour ne pas elargir le contrat
 * de la version "types" qui filtre explicitement CLASS/STRUCT/ENUM/INTERFACE. */
bool symtable_global_is_ambiguous(SymbolTable* t, const char* name) {
    const char* canon = NULL;
    for (int i = 0; i < t->count; i++) {
        ProjectSymbol* s = &t->symbols[i];
        if (s->kind != SYM_VARIABLE && s->kind != SYM_CONSTANT &&
            s->kind != SYM_PROCEDURE && s->kind != SYM_FUNCTION &&
            s->kind != SYM_WINDOW) continue;  /*  */
        if (strcasecmp(s->name, name) != 0) continue;
        if (canon == NULL) { canon = s->name; continue; }
        if (strcmp(canon, s->name) != 0) return true;
    }
    return false;
}

bool symtable_member_is_ambiguous(SymbolTable* t, const char* ownerType,
                                  const char* member) {
    const char* canon = NULL;
    for (int i = 0; i < t->memberCount; i++) {
        ProjectMember* m = &t->members[i];
        if (strcasecmp(m->ownerType, ownerType) != 0) continue;
        if (strcasecmp(m->name, member) != 0) continue;
        if (canon == NULL) { canon = m->name; continue; }
        if (strcmp(canon, m->name) != 0) return true;  /* casse divergente */
    }
    return false;
}

static void symtable_add(SymbolTable* t, const char* name, int nameLen,
                          SymbolKind kind, int fileIndex, bool isPrivate,
                          int line, int column) {
    if (t->count >= t->capacity) {
        int newCap = t->capacity < 16 ? 16 : t->capacity * 2;
        t->symbols = realloc(t->symbols, sizeof(ProjectSymbol) * newCap);
        t->capacity = newCap;
    }

    ProjectSymbol* sym = &t->symbols[t->count];
    int copyLen = nameLen;
    if (copyLen >= (int)sizeof(sym->name)) copyLen = (int)sizeof(sym->name) - 1;
    memcpy(sym->name, name, copyLen);
    sym->name[copyLen] = '\0';
    sym->kind = kind;
    sym->fileIndex = fileIndex;
    sym->isPrivate = isPrivate;
    sym->line = line;
    sym->column = column;
    t->count++;
}

ProjectSymbol* symtable_find(SymbolTable* t, const char* name, int len,
                              bool* ambiguous) {
    ProjectSymbol* first = NULL;
    if (ambiguous) *ambiguous = false;

    for (int i = 0; i < t->count; i++) {
        ProjectSymbol* s = &t->symbols[i];
        if ((int)strlen(s->name) == len && memcmp(s->name, name, len) == 0) {
            if (first == NULL) {
                first = s;
            } else if (!first->isPrivate && !s->isPrivate &&
                       first->fileIndex != s->fileIndex) {
                /* Two non-private symbols from different files = ambiguous */
                if (ambiguous) *ambiguous = true;
                return first;
            }
        }
    }
    return first;
}

ProjectSymbol* symtable_find_in_file(SymbolTable* t, const char* name, int len,
                                      int fileIndex) {
    for (int i = 0; i < t->count; i++) {
        ProjectSymbol* s = &t->symbols[i];
        if (s->fileIndex == fileIndex &&
            (int)strlen(s->name) == len &&
            memcmp(s->name, name, len) == 0) {
            return s;
        }
    }
    return NULL;
}

/* ================================================================
 * PRE-SCANNER
 * ================================================================
 *
 * Lightweight lexical scan that extracts top-level declarations
 * without doing full parsing. Uses the existing Scanner from scanner.c.
 *
 * Patterns detected at depth 0:
 *   PROCEDURE identifier       -> SYM_PROCEDURE (skip if Class.Method)
 *   FONCTION identifier        -> SYM_FUNCTION  (skip if Class.Method)
 *   identifier EST CLASSE      -> SYM_CLASS
 *   identifier EST STRUCTURE   -> SYM_STRUCT
 *   identifier EST INTERFACE   -> SYM_INTERFACE
 *   identifier EST ENUMERATION -> SYM_ENUM
 *
 * Visibility: PROCEDURE PRIVE NomProc() (modifier AFTER keyword)
 *
 * Depth tracking: +1 on PROCEDURE/FONCTION/CLASSE/STRUCTURE/INTERFACE/ENUMERATION
 *                 -1 on FIN
 */

/* Check if a token is a visibility modifier */
static bool is_visibility_token(TokenType type) {
    return type == TOKEN_PRIVE || type == TOKEN_PROTEGE ||
           type == TOKEN_PUBLIC;
}

/*  : helper centralise pour le tracking de profondeur des
 * blocs lexicaux (SI/POUR/TANTQUE/BOUCLE/SELON/QUAND) + CONSTANTE et
 * PROPRIETES. Retourne le delta a appliquer a depth (+1, 0, -1).
 *
 *  : exporte (cf. prescan.h) — c'est desormais LA regle, consommee
 * aussi par le detecteur GO (main.c) et la pre-passe d'element (compiler.c).
 * Le contrat complet est documente sur la declaration.
 *
 * Pour les ouvrants qui ont une forme inline LEGALE (verifie
 * empiriquement coordinator/--check) : seul TOKEN_SI est concerne
 * (POUR/TANTQUE/SELON/BOUCLE/QUAND rejettent leur forme inline au
 * parsing). Pour ces tokens, on peek sur `peek` pour distinguer
 * forme bloc (depth +1) de forme inline (depth +0).
 *
 * `peek` est toujours restaure avant retour : le caller peut passer une copie
 * de son scanner sans avoir a la resynchroniser.
 *
 * NB : PROCEDURE/FONCTION en tete d'instruction ouvrent toujours un bloc ; la
 * forme annotation de type (`p est une Procedure`) est neutralisee au site
 * d'appel — la copule n'est pas visible d'ici.
 *
 *  : la liste des jetons vit desormais dans OWL_DEPTH_TOKENS
 * (prescan.h), partagee avec la porte texte prescan_depth_delta_text et avec
 * prescan_token_in_depth_table. Une seule table, trois accesseurs. */

/* CONSTANTE : bloc (NEWLINE apres) vs inline (IDENT apres). */
static int depth_delta_constante(Scanner* peek) {
    Scanner saved = *peek;
    Token next = scanner_scan_token(peek);
    *peek = saved;
    if (next.type == TOKEN_NEWLINE || next.type == TOKEN_EOF) return +1;
    return 0;
}

int prescan_depth_delta(Token tok, Scanner* peek) {
    switch (tok.type) {
#define OWL_DEPTH_X_CLOSER(t)  case t: return -1;
#define OWL_DEPTH_X_OPENER(t)  case t: return +1;
        OWL_DEPTH_TOKENS(OWL_DEPTH_X_OPENER, OWL_DEPTH_X_NOTHING,
                         OWL_DEPTH_X_CLOSER)
#undef OWL_DEPTH_X_CLOSER
#undef OWL_DEPTH_X_OPENER

        /* CONSTANTE : bloc (NEWLINE apres) vs inline (IDENT apres). */
        case TOKEN_CONSTANTE:
            return depth_delta_constante(peek);

        /* SI : seul ouvrant lexical avec forme inline legale. La
         * forme bloc est `SI cond ALORS\n stmt\n FIN` (NEWLINE apres
         * ALORS) ; la forme inline est `SI cond ALORS stmt` (pas de
         * NEWLINE apres ALORS). On peek jusqu'a ALORS, puis on
         * inspecte le token suivant. Si on n'atteint pas ALORS avant
         * la fin de ligne (NEWLINE / FIN / EOF — source en cours
         * d'edition, ou condition malformee), on traite comme bloc
         * par defaut (conservateur : eviter de droper un FIN legitime
         * du contexte englobant). Le NEWLINE fait partie du critere
         * d'arret : sans lui, le peek fuyait sur les lignes suivantes
         * et pouvait juger ce SI sur le ALORS d'un SI plus bas. */
        case TOKEN_SI: {
            Scanner saved = *peek;
            for (;;) {
                Token t = scanner_scan_token(peek);
                if (t.type == TOKEN_ALORS) break;
                if (t.type == TOKEN_NEWLINE || t.type == TOKEN_FIN ||
                    t.type == TOKEN_EOF) {
                    *peek = saved;
                    return +1;
                }
            }
            /* Apres ALORS : NEWLINE/EOF = bloc, sinon = inline. */
            Token afterAlors = scanner_scan_token(peek);
            *peek = saved;
            if (afterAlors.type == TOKEN_NEWLINE ||
                afterAlors.type == TOKEN_EOF) {
                return +1;
            }
            return 0;
        }

        default:
            return 0;
    }
}

bool prescan_token_in_depth_table(TokenType type) {
    switch (type) {
#define OWL_DEPTH_X_ANY(t) case t:
        OWL_DEPTH_TOKENS(OWL_DEPTH_X_ANY, OWL_DEPTH_X_ANY, OWL_DEPTH_X_ANY)
#undef OWL_DEPTH_X_ANY
            return true;
        default:
            return false;
    }
}

bool prescan_depth_delta_text(const char* text, int* delta) {
    if (delta != NULL) *delta = 0;
    if (text == NULL || *text == '\0') return false;
    Scanner sc;
    scanner_init(&sc, text);
    Token tok = scanner_scan_token(&sc);
    if (!prescan_token_in_depth_table(tok.type)) return false;
    /* Le scanner est deja positionne juste apres `tok` : c'est exactement le
     * peek attendu par la porte jeton. */
    int d = prescan_depth_delta(tok, &sc);
    if (delta != NULL) *delta = d;
    return true;
}

bool prescan_avec_opens_block(bool atStatementStart) {
    return atStatementStart;
}

/*  — un identifiant (post-`est [un/une/a/an]`) nomme-t-il un bloc DSL
 * de type compose (pas de token dedie) : Projet, Theme, CollectionDeProcedures.
 * Casse-insensible ; accents strippes pour Theme (Thème) et Collection. */
static bool prescan_ident_is_projet_kw(const char* start, int length) {
    return (length == 6 && strncasecmp(start, "Projet", 6) == 0)
        || (length == 7 && strncasecmp(start, "Project", 7) == 0);
}

static bool prescan_ident_is_theme_kw(const char* start, int length) {
    char probe[16];
    int n = scanner_strip_accents(start, length, probe, sizeof(probe));
    return n == 5 && strncasecmp(probe, "Theme", 5) == 0;
}

static bool prescan_ident_is_collection_kw(const char* start, int length) {
    char probe[40];
    int n = scanner_strip_accents(start, length, probe, sizeof(probe));
    if (n <= 0 || n >= (int)sizeof(probe)) return false;
    return (n == 22 && strncasecmp(probe, "CollectionDeProcedures", 22) == 0)
        || (n == 19 && strncasecmp(probe, "ProcedureCollection", 19) == 0);
}

/*  — miroir exact du matcher compilateur `service_ident_is("ServiceREST")`
 * (compiler.c) : accents strippes + strncasecmp sur "ServiceREST" (longueur 11).
 * Pas de forme EN — ServiceREST n'en a pas dans le compilateur (invariant 3
 * respecte : on ne synthetise pas d'alias que le langage ne connait pas). */
static bool prescan_ident_is_servicerest_kw(const char* start, int length) {
    char probe[16];
    int n = scanner_strip_accents(start, length, probe, sizeof(probe));
    return n == 11 && strncasecmp(probe, "ServiceREST", 11) == 0;
}

/*  — le token qui suit `est [article]` ouvre-t-il un bloc DSL
 * CONTENEUR qui confere la PARTICIPATION au projet ? Liste canon (bilingue,
 * accent-insensible) : Projet, Fenetre, Etat/Report, Classe, Theme,
 * CollectionDeProcedures, ServiceREST. Une declaration de TYPE (Structure, Enumeration,
 * Combinaison, Interface) est PASSIVE — au meme titre qu'une variable ou une
 * procedure nue, elle ne rend PAS le fichier participant : exclue ici. Les
 * widgets (Libelle/Bouton/Saisie/Table/...) ne sont pas des blocs top-level
 * (valides seulement dans une Fenetre) : exclus aussi. */
static bool prescan_after_est_is_dsl_block(Token after) {
    switch (after.type) {
        case TOKEN_CLASSE:
        case TOKEN_FENETRE:
        case TOKEN_ETAT:
            return true;
        default: break;
    }
    if (after.type == TOKEN_IDENTIFIER) {
        return prescan_ident_is_projet_kw(after.start, after.length)
            || prescan_ident_is_theme_kw(after.start, after.length)
            || prescan_ident_is_collection_kw(after.start, after.length)
            || prescan_ident_is_servicerest_kw(after.start, after.length);
    }
    return false;
}

/*  — le top-level de ce fichier contient-il AU MOINS un bloc DSL ?
 * Determine si un fichier "participe au projet" (visible, decouvert) ou est un
 * script PLAT prive (aucun bloc DSL au top-level). Un fichier MIXTE (bloc +
 * code lache) contient un bloc, donc participe ici — le verrou top-level du
 * compilateur produira l'erreur « code hors bloc ». Scan lexical au depth 0
 * (les blocs DSL ne sont legaux qu'au top-level). Casse/accent-insensible via
 * le scanner. */
bool prescan_source_is_dsl_block_file(const char* source) {
    if (source == NULL) return false;
    Scanner s;
    scanner_init(&s, source);
    int depth = 0;
    for (;;) {
        Token tok = scanner_scan_token(&s);
        if (tok.type == TOKEN_EOF) break;
        if (tok.type == TOKEN_NEWLINE || tok.type == TOKEN_ERROR) continue;

        if (tok.type == TOKEN_FIN) {
            if (depth > 0) depth--;
            continue;
        }

        /* Au top-level : `IDENT est [article] <blockkw>` ? */
        if (depth == 0 && tok.type == TOKEN_IDENTIFIER) {
            Scanner saved = s;
            Token next;
            do { next = scanner_scan_token(&s); } while (next.type == TOKEN_NEWLINE);
            /* Multi-decl `a, b sont des X` : sauter les `, IDENT`. */
            while (next.type == TOKEN_COMMA) {
                do { next = scanner_scan_token(&s); } while (next.type == TOKEN_NEWLINE);
                if (next.type != TOKEN_IDENTIFIER) break;
                do { next = scanner_scan_token(&s); } while (next.type == TOKEN_NEWLINE);
            }
            if (next.type == TOKEN_EST || next.type == TOKEN_SONT) {
                Token after;
                do { after = scanner_scan_token(&s); } while (after.type == TOKEN_NEWLINE);
                /* Sauter l'article un/une/a/an. */
                if (after.type == TOKEN_A ||
                    (after.type == TOKEN_IDENTIFIER &&
                     ((after.length == 1 &&
                       (after.start[0] == 'a' || after.start[0] == 'A')) ||
                      (after.length == 2 &&
                       (strncasecmp(after.start, "un", 2) == 0 ||
                        strncasecmp(after.start, "an", 2) == 0)) ||
                      (after.length == 3 && strncasecmp(after.start, "une", 3) == 0)))) {
                    do { after = scanner_scan_token(&s); } while (after.type == TOKEN_NEWLINE);
                }
                if (prescan_after_est_is_dsl_block(after)) return true;
                /*  — bloc passif (Structure/Enumeration/Combinaison/
                 * Interface) : ne confere PAS la participation, mais possede un
                 * corps termine par FIN. On saute ce corps jusqu'a la FIN
                 * appariee pour ne pas dereger le depth-tracking de la suite du
                 * fichier (sinon une Interface, dont le corps est fait de
                 * PROCEDURE nues sans FIN propre, laisserait ces PROCEDURE
                 * incrementer la profondeur et masquerait un bloc conteneur
                 * suivant). On ne compte donc PAS PROCEDURE/FONCTION comme
                 * ouvrant : seuls FIN (ferme) et PROPRIETES / sous-bloc passif
                 * imbrique (ouvrent une FIN propre) modifient bodyDepth. */
                if (after.type == TOKEN_STRUCTURE ||
                    after.type == TOKEN_ENUMERATION ||
                    after.type == TOKEN_COMBINAISON ||
                    after.type == TOKEN_INTERFACE) {
                    int bodyDepth = 1;
                    for (;;) {
                        Token bt = scanner_scan_token(&s);
                        if (bt.type == TOKEN_EOF) break;
                        if (bt.type == TOKEN_NEWLINE ||
                            bt.type == TOKEN_ERROR) continue;
                        if (bt.type == TOKEN_FIN) {
                            bodyDepth--;
                            if (bodyDepth <= 0) break;
                            continue;
                        }
                        if (bt.type == TOKEN_PROPRIETES) { bodyDepth++; continue; }
                    }
                    continue;
                }
                /* Declaration ordinaire (variable/const-typee) : pas de bloc,
                 * pas de FIN a matcher. On continue apres les tokens consommes. */
                continue;
            }
            /* Pas de EST : restaure pour laisser le depth-tracker voir ce token. */
            s = saved;
        }

        /* Suivi de profondeur (procedures + blocs lexicaux) pour ne considerer
         * que le vrai top-level. Reutilise le helper du prescan principal. */
        int delta = prescan_depth_delta(tok, &s);
        if (delta > 0) depth += delta;
    }
    return false;
}

/*  — mappe le token (ou l'identifiant compose) qui suit `est [article]`
 * vers un PrescanEntryKind. PRESCAN_ENTRY_NONE si ce n'est pas un bloc DSL
 * (declaration de variable / constante typee). */
static PrescanEntryKind prescan_after_est_entry_kind(Token after) {
    switch (after.type) {
        case TOKEN_CLASSE:      return PRESCAN_ENTRY_CLASSE;
        case TOKEN_FENETRE:     return PRESCAN_ENTRY_FENETRE;
        case TOKEN_ETAT:        return PRESCAN_ENTRY_ETAT;
        case TOKEN_STRUCTURE:   return PRESCAN_ENTRY_STRUCTURE;
        case TOKEN_ENUMERATION: return PRESCAN_ENTRY_ENUM;
        case TOKEN_COMBINAISON: return PRESCAN_ENTRY_COMBINAISON;
        case TOKEN_INTERFACE:   return PRESCAN_ENTRY_INTERFACE;
        default: break;
    }
    if (after.type == TOKEN_IDENTIFIER) {
        if (prescan_ident_is_projet_kw(after.start, after.length))
            return PRESCAN_ENTRY_PROJET;
        if (prescan_ident_is_theme_kw(after.start, after.length))
            return PRESCAN_ENTRY_THEME;
        if (prescan_ident_is_collection_kw(after.start, after.length))
            return PRESCAN_ENTRY_COLLECTION;
    }
    return PRESCAN_ENTRY_NONE;
}

PrescanEntryKind prescan_source_entry_block_kind(const char* source,
                                                 char* nameOut, int nameCap) {
    if (nameOut && nameCap > 0) nameOut[0] = '\0';
    if (source == NULL) return PRESCAN_ENTRY_NONE;
    Scanner s;
    scanner_init(&s, source);
    int depth = 0;
    for (;;) {
        Token tok = scanner_scan_token(&s);
        if (tok.type == TOKEN_EOF) break;
        if (tok.type == TOKEN_NEWLINE || tok.type == TOKEN_ERROR) continue;
        if (tok.type == TOKEN_FIN) { if (depth > 0) depth--; continue; }

        if (depth == 0 && tok.type == TOKEN_IDENTIFIER) {
            Token identTok = tok;   /* nom declare = premier identifiant */
            Scanner saved = s;
            Token next;
            do { next = scanner_scan_token(&s); } while (next.type == TOKEN_NEWLINE);
            /* Multi-decl `a, b sont des X` : le nom retenu reste identTok. */
            while (next.type == TOKEN_COMMA) {
                do { next = scanner_scan_token(&s); } while (next.type == TOKEN_NEWLINE);
                if (next.type != TOKEN_IDENTIFIER) break;
                do { next = scanner_scan_token(&s); } while (next.type == TOKEN_NEWLINE);
            }
            if (next.type == TOKEN_EST || next.type == TOKEN_SONT) {
                Token after;
                do { after = scanner_scan_token(&s); } while (after.type == TOKEN_NEWLINE);
                /* Sauter l'article un/une/a/an. */
                if (after.type == TOKEN_A ||
                    (after.type == TOKEN_IDENTIFIER &&
                     ((after.length == 1 &&
                       (after.start[0] == 'a' || after.start[0] == 'A')) ||
                      (after.length == 2 &&
                       (strncasecmp(after.start, "un", 2) == 0 ||
                        strncasecmp(after.start, "an", 2) == 0)) ||
                      (after.length == 3 && strncasecmp(after.start, "une", 3) == 0)))) {
                    do { after = scanner_scan_token(&s); } while (after.type == TOKEN_NEWLINE);
                }
                PrescanEntryKind k = prescan_after_est_entry_kind(after);
                if (k != PRESCAN_ENTRY_NONE) {
                    if (nameOut && nameCap > 0) {
                        int n = identTok.length < nameCap - 1
                                ? identTok.length : nameCap - 1;
                        memcpy(nameOut, identTok.start, (size_t)n);
                        nameOut[n] = '\0';
                    }
                    return k;
                }
                /* Declaration ordinaire (variable/const-typee) : pas de bloc,
                 * on poursuit le scan (top-level d'un script). */
                continue;
            }
            /* Pas de EST : restaure pour laisser le depth-tracker voir ce token. */
            s = saved;
        }

        int delta = prescan_depth_delta(tok, &s);
        if (delta > 0) depth += delta;
    }
    return PRESCAN_ENTRY_NONE;
}

bool prescan_file(SymbolTable* t, const char* source, int fileIndex) {
    /*  — filtre de perimetre projet : un compagnon PLAT (aucun bloc
     * DSL au top-level, hors entree) sort de la decouverte. Ni indexe ni
     * memorise dans sources[]. Voir prescan_set_project_scope. */
    if (g_prescan_restrict_to_dsl && fileIndex != g_prescan_entry_index
        && !prescan_source_is_dsl_block_file(source)) {
        return true;
    }

    /*  : memoriser la source (non-owning) pour permettre a
     * compile_in_project de re-walker les voisins via prescan_source_unified
     * et ancrer les sous-widgets DSL cross-fichier. Liste compacte (paires
     * fileIndex + source*) — en suite globale, fileIndex peut depasser
     * MAX_PROJECT_FILES alors que le nombre de compagnons d'un test reste
     * petit. */
    if (t->sourceCount < MAX_PROJECT_FILES) {
        t->sources[t->sourceCount].fileIndex = fileIndex;
        t->sources[t->sourceCount].source = source;
        t->sourceCount++;
    }

    Scanner scanner;
    scanner_init(&scanner, source);

    int depth = 0;
    /*  — mode de portee top-level courant (bascule GLOBAL/LOCAL seule
     * sur une ligne, cf. compiler.c::declaration). En mode LOCAL, une variable
     * top-level est confinee : elle ne doit PAS entrer dans la table de symboles
     * projet (sinon elle resterait resoluble en nom nu / cross-fichier, cassant
     * le confinement). N'a d'effet qu'au depth 0. */
    bool confine = false;

    for (;;) {
        Token tok = scanner_scan_token(&scanner);

        /* Skip newlines and errors */
        if (tok.type == TOKEN_NEWLINE) continue;
        if (tok.type == TOKEN_ERROR) continue;
        if (tok.type == TOKEN_EOF) break;

        /* FIN decrements depth */
        if (tok.type == TOKEN_FIN) {
            if (depth > 0) depth--;
            continue;
        }

        /*  — bascule de portee au depth 0 (hors classe : dans une
         * classe GLOBAL/GLOBALE = membres/methodes statiques, mais on est au
         * depth 0 ici, donc jamais dans un corps de classe). */
        if (depth == 0 && (tok.type == TOKEN_GLOBAL || tok.type == TOKEN_LOCAL)) {
            confine = (tok.type == TOKEN_LOCAL);
            continue;
        }

        /* PROPRIETES ouvre un bloc termine par FIN. Important au prescan
         * pour que depth reste coherent dans les fenetres / tables /
         * sous-widgets (chaque PROPRIETES + ses FIN doit s'apparier sinon
         * depth retombe a 0 au milieu d'une Fenetre, et les noms de
         * colonnes/sous-widgets seraient catalogues comme globals). */
        if (tok.type == TOKEN_PROPRIETES) {
            depth++;
            continue;
        }

        /* At depth 0: detect declarations */
        if (depth == 0) {

            /* PROCEDURE/FONCTION [PRIVE|PROTEGE|...] identifier */
            if (tok.type == TOKEN_PROCEDURE || tok.type == TOKEN_FONCTION) {
                SymbolKind kind = (tok.type == TOKEN_PROCEDURE)
                                  ? SYM_PROCEDURE : SYM_FUNCTION;

                /* Peek next non-newline token: may be visibility or name */
                Token nameToken;
                bool privFlag = false;
                for (;;) {
                    nameToken = scanner_scan_token(&scanner);
                    if (nameToken.type != TOKEN_NEWLINE) break;
                }

                /* If it's a visibility modifier, consume it and get the real name */
                if (is_visibility_token(nameToken.type)) {
                    privFlag = (nameToken.type == TOKEN_PRIVE);
                    for (;;) {
                        nameToken = scanner_scan_token(&scanner);
                        if (nameToken.type != TOKEN_NEWLINE) break;
                    }
                }

                if (nameToken.type == TOKEN_IDENTIFIER) {
                    /* Check if it's a method: name DOT name -> skip */
                    Token afterName;
                    for (;;) {
                        afterName = scanner_scan_token(&scanner);
                        if (afterName.type != TOKEN_NEWLINE) break;
                    }

                    if (afterName.type == TOKEN_DOT) {
                        /* It's a method like ClassName.Method -> track and increase depth.
                         *  : capture aussi la signature (params + retour). */
                        Token methodTok;
                        for (;;) {
                            methodTok = scanner_scan_token(&scanner);
                            if (methodTok.type != TOKEN_NEWLINE) break;
                        }
                        if (methodTok.type == TOKEN_IDENTIFIER &&
                            t->classMethodCount < 512) {
                            int ci = t->classMethodCount;
                            int cl = nameToken.length < 127 ? nameToken.length : 127;
                            memcpy(t->classMethods[ci].className, nameToken.start, cl);
                            t->classMethods[ci].className[cl] = '\0';
                            PrescanMethodSig* sig = &t->classMethods[ci].sig;
                            int ml = methodTok.length < 127 ? methodTok.length : 127;
                            memcpy(sig->name, methodTok.start, ml);
                            sig->name[ml] = '\0';
                            sig->paramCount = 0;
                            memset(sig->paramTypes, 0, sizeof(sig->paramTypes));
                            sig->returnType[0] = '\0';

                            /* Parse params + retour optionnel (meme protocole
                             * que les methodes interfaces ci-dessous). */
                            Token tok3;
                            for (;;) {
                                tok3 = scanner_scan_token(&scanner);
                                if (tok3.type != TOKEN_NEWLINE) break;
                            }
                            if (tok3.type == TOKEN_LEFT_PAREN) {
                                for (;;) {
                                    Token pname;
                                    for (;;) {
                                        pname = scanner_scan_token(&scanner);
                                        if (pname.type != TOKEN_NEWLINE) break;
                                    }
                                    if (pname.type == TOKEN_RIGHT_PAREN ||
                                        pname.type == TOKEN_EOF) break;
                                    if (pname.type == TOKEN_IDENTIFIER) {
                                        Scanner savedAfterName = scanner;
                                        Token est;
                                        for (;;) {
                                            est = scanner_scan_token(&scanner);
                                            if (est.type != TOKEN_NEWLINE) break;
                                        }
                                        char* slot = (sig->paramCount <
                                                      PRESCAN_MAX_METHOD_PARAMS)
                                            ? sig->paramTypes[sig->paramCount]
                                            : NULL;
                                        if (est.type == TOKEN_EST) {
                                            prescan_capture_type_spec(&scanner, slot);
                                        } else {
                                            scanner = savedAfterName;
                                        }
                                        sig->paramCount++;
                                    }
                                    for (;;) {
                                        Token next3;
                                        for (;;) {
                                            next3 = scanner_scan_token(&scanner);
                                            if (next3.type != TOKEN_NEWLINE) break;
                                        }
                                        if (next3.type == TOKEN_COMMA) break;
                                        if (next3.type == TOKEN_RIGHT_PAREN ||
                                            next3.type == TOKEN_EOF) {
                                            goto class_method_params_done;
                                        }
                                    }
                                }
                            }
                            class_method_params_done:
                            {
                                Scanner savedColon = scanner;
                                Token colon;
                                for (;;) {
                                    colon = scanner_scan_token(&scanner);
                                    if (colon.type != TOKEN_NEWLINE) break;
                                }
                                if (colon.type == TOKEN_COLON) {
                                    prescan_capture_type_spec(
                                        &scanner, sig->returnType);
                                } else {
                                    scanner = savedColon;
                                }
                            }
                            t->classMethodCount++;
                        }
                        depth++;
                    } else {
                        /* It's a global procedure/function */
                        symtable_add(t, nameToken.start, nameToken.length,
                                     kind, fileIndex, privFlag,
                                     nameToken.line, nameToken.column);
                        depth++;
                    }
                } else if (nameToken.type == TOKEN_EOF) {
                    break;
                } else {
                    /* Destructor (~ClassName) or other -> just increase depth */
                    depth++;
                }

                continue;
            }

            /* identifier EST ... */
            if (tok.type == TOKEN_IDENTIFIER) {
                Token identTok = tok;

                /* Peek next — sauve le scanner pour pouvoir restaurer si
                 * 'next' n'est pas TOKEN_EST. Sans ca, un pattern
                 * 'STY_X..prop = val \n STY_Y est un Style' perd STY_Y
                 * (consomme par le peek qui voit que ce n'est pas EST). */
                Scanner savedScanForPeek = scanner;
                Token next;
                for (;;) {
                    next = scanner_scan_token(&scanner);
                    if (next.type != TOKEN_NEWLINE) break;
                }

                if (next.type != TOKEN_EST) {
                    /* Pas une declaration : restaure le scanner pour ne
                     * pas avoir consomme le token 'next'. Il sera lu au
                     * prochain tour de la boucle. */
                    scanner = savedScanForPeek;
                    continue;
                }

                /* TOKEN_EST consume — on continue avec la logique existante. */
                if (next.type == TOKEN_EST) {
                    /* Peek what comes after EST */
                    Token after;
                    for (;;) {
                        after = scanner_scan_token(&scanner);
                        if (after.type != TOKEN_NEWLINE) break;
                    }

                    /* Copule EN : `is a/an <BlockType>`. Le scanner mappe `a`
                     * sur TOKEN_A (borne de plage), sans ambiguite en position
                     * post-EST ; `an` reste IDENT 2 chars. On saute l'article
                     * pour atteindre le token de bloc (sinon le membre
                     * inherite cross-fichier d'une classe declaree `is a class`
                     * n'est jamais capture). */
                    if (after.type == TOKEN_A ||
                        (after.type == TOKEN_IDENTIFIER &&
                         ((after.length == 1 &&
                           (after.start[0] == 'a' || after.start[0] == 'A')) ||
                          (after.length == 2 &&
                           (after.start[0] == 'a' || after.start[0] == 'A') &&
                           (after.start[1] == 'n' || after.start[1] == 'N'))))) {
                        for (;;) {
                            after = scanner_scan_token(&scanner);
                            if (after.type != TOKEN_NEWLINE) break;
                        }
                    }

                    if (after.type == TOKEN_CLASSE) {
                        symtable_add(t, identTok.start, identTok.length,
                                     SYM_CLASS, fileIndex, false,
                                     identTok.line, identTok.column);
                        symtable_add_element(t, identTok.start, identTok.length,
                                             fileIndex, identTok.line,
                                             identTok.column);  /*  */
                        /* Track IMPLEMENTE inside the class body (scan ahead without consuming FIN) */
                        {
                            Scanner savedScan = scanner;
                            Token ctok;
                            int cdepth = 1;
                            /*  : capture des membres (champs + constantes
                             * de classe). On suit la profondeur reelle pour ne pas
                             * aspirer les locals d'une methode (PROCEDURE/FONCTION
                             * ouvre un bloc). Un bloc CONSTANTE est traite a part :
                             * ses lignes `NAME = value` sont des constantes membres,
                             * pas des declarations de variable. */
                            bool inConstBlock = false;
                            /*  : visibilite du bloc courant (0=PUBLIC,
                             * 1=PRIVE, 2=PROTEGE, encodage OwlVisibility). Un
                             * mot-cle PUBLIC/PRIVE/PROTEGE a cdepth==1 pose la
                             * visibilite de tous les champs declares en dessous
                             * (miroir de currentVis dans class_declaration).
                             * Default PUBLIC (canon). */
                            int curVis = 0;
                            /*  : bloc GLOBAL en cours. Les champs GLOBAL
                             * (statiques de classe) ne sont PAS enregistres en
                             * visibilite par le compilateur (class_declaration ne
                             * fait class_field_vis_register que sur les champs
                             * d'instance) : on les exclut de l'hydratation en
                             * posant visibility=-1, sinon divergence cross-fichier.
                             * GLOBAL pose le flag, LOCAL/PUBLIC/PRIVE/PROTEGE le
                             * levent (miroir class_declaration). */
                            bool inGlobalBlock = false;
                            for (;;) {
                                ctok = scanner_scan_token(&savedScan);
                                if (ctok.type == TOKEN_EOF) break;
                                if (ctok.type == TOKEN_NEWLINE) continue;
                                if (ctok.type == TOKEN_FIN) {
                                    if (inConstBlock) { inConstBlock = false; continue; }
                                    cdepth--;
                                    if (cdepth <= 0) break;
                                    continue;
                                }
                                if (ctok.type == TOKEN_PROCEDURE ||
                                    ctok.type == TOKEN_FONCTION) {
                                    /*  : si `PROCEDURE Constructeur(...)`
                                     * inline dans une classe, on capture l'arite
                                     * minimale (params SANS valeur par defaut)
                                     * pour permettre l'hydration cross-file de
                                     * `g_class_constructor_arity[]` (mode --check
                                     * compile un fichier sans avoir compile les
                                     * autres). Stocke dans classMethods[] avec
                                     * methodName="Constructeur" ; paramCount du
                                     * sig porte ici la valeur minArity (pas le
                                     * total) — usage interne dedie a ,
                                     * le check d'interface n'attend pas de
                                     * methode Constructeur.
                                     *
                                     *  : meme traitement pour
                                     * `PROCEDURE <Nom>(...)` regulier (methode
                                     * d'instance inline). Necessaire pour la
                                     * resolution d'appel nu d'une methode soeur
                                     * cross-fichier (cFille.M() qui appelle
                                     * Calcule() heritee de cBase.owl voisin).
                                     * paramCount = arity stricte (sans defaults),
                                     * comme pour les methodes externes au-dessus. */
                                    if (cdepth == 1) {
                                        /*  : capture visibilite + nom pour
                                         * hydrater g_method_visibility cross-fichier
                                         * (colon-ref :M() d'une methode heritee d'une
                                         * base voisine). Peek dedie qui saute les
                                         * modificateurs [PRIVE|PROTEGE|PUBLIC]
                                         * [GLOBALE|GLOBAL] [ABSTRAITE] pour atteindre
                                         * le nom. Visibilite = modif explicite sinon
                                         * curVis (miroir class_declaration : procVis =
                                         * currentVis puis override). N'affecte PAS la
                                         * capture classMethods[] ci-dessous. */
                                        {
                                            Scanner pv = savedScan;
                                            int mvis = curVis;
                                            Token mt;
                                            for (;;) { mt = scanner_scan_token(&pv); if (mt.type != TOKEN_NEWLINE) break; }
                                            if (mt.type == TOKEN_PRIVE || mt.type == TOKEN_PROTEGE || mt.type == TOKEN_PUBLIC) {
                                                mvis = (mt.type == TOKEN_PRIVE) ? 1 : (mt.type == TOKEN_PROTEGE) ? 2 : 0;
                                                for (;;) { mt = scanner_scan_token(&pv); if (mt.type != TOKEN_NEWLINE) break; }
                                            }
                                            if (mt.type == TOKEN_GLOBALE || mt.type == TOKEN_GLOBAL) {
                                                for (;;) { mt = scanner_scan_token(&pv); if (mt.type != TOKEN_NEWLINE) break; }
                                            }
                                            if (mt.type == TOKEN_ABSTRAITE) {
                                                for (;;) { mt = scanner_scan_token(&pv); if (mt.type != TOKEN_NEWLINE) break; }
                                            }
                                            /* Seules les methodes nommees (IDENTIFIER) sont
                                             * cibles de :M()/.M() ; Constructeur/Destructeur
                                             * exclus (pas d'appel colon-ref). */
                                            if (mt.type == TOKEN_IDENTIFIER && t->classMethodVisCount < 512) {
                                                int mi = t->classMethodVisCount;
                                                int cl5 = identTok.length < 127 ? identTok.length : 127;
                                                memcpy(t->classMethodVis[mi].className, identTok.start, cl5);
                                                t->classMethodVis[mi].className[cl5] = '\0';
                                                int mnl = mt.length < 127 ? mt.length : 127;
                                                memcpy(t->classMethodVis[mi].methodName, mt.start, mnl);
                                                t->classMethodVis[mi].methodName[mnl] = '\0';
                                                t->classMethodVis[mi].visibility = mvis;
                                                t->classMethodVis[mi].fileIndex = fileIndex;
                                                t->classMethodVis[mi].line = mt.line;
                                                t->classMethodVisCount++;
                                            }
                                        }
                                        Scanner peekProc = savedScan;
                                        Token after;
                                        for (;;) {
                                            after = scanner_scan_token(&peekProc);
                                            if (after.type != TOKEN_NEWLINE) break;
                                        }
                                        bool isCstr = (after.type == TOKEN_CONSTRUCTEUR);
                                        bool isRegularMethod = (after.type == TOKEN_IDENTIFIER);
                                        if ((isCstr || isRegularMethod) &&
                                            t->classMethodCount < 512) {
                                            int ci = t->classMethodCount;
                                            int cl4 = identTok.length < 127
                                                      ? identTok.length : 127;
                                            memcpy(t->classMethods[ci].className,
                                                   identTok.start, cl4);
                                            t->classMethods[ci].className[cl4] = '\0';
                                            PrescanMethodSig* csig =
                                                &t->classMethods[ci].sig;
                                            if (isCstr) {
                                                memcpy(csig->name, "Constructeur", 13);
                                            } else {
                                                int ml = after.length < 127
                                                         ? after.length : 127;
                                                memcpy(csig->name, after.start, (size_t)ml);
                                                csig->name[ml] = '\0';
                                            }
                                            csig->paramCount = 0;
                                            memset(csig->paramTypes, 0,
                                                   sizeof(csig->paramTypes));
                                            csig->returnType[0] = '\0';
                                            int minArity = 0;
                                            /* Avale LEFT_PAREN si present puis
                                             * parcourt les params, comptant ceux
                                             * sans `= <default>`. */
                                            Token tokp;
                                            for (;;) {
                                                tokp = scanner_scan_token(&peekProc);
                                                if (tokp.type != TOKEN_NEWLINE) break;
                                            }
                                            if (tokp.type == TOKEN_LEFT_PAREN) {
                                                for (;;) {
                                                    Token pname;
                                                    for (;;) {
                                                        pname = scanner_scan_token(&peekProc);
                                                        if (pname.type != TOKEN_NEWLINE) break;
                                                    }
                                                    if (pname.type == TOKEN_RIGHT_PAREN ||
                                                        pname.type == TOKEN_EOF) break;
                                                    if (pname.type == TOKEN_IDENTIFIER) {
                                                        /* Skip `est un <type>` */
                                                        Scanner saveP = peekProc;
                                                        Token est;
                                                        for (;;) {
                                                            est = scanner_scan_token(&peekProc);
                                                            if (est.type != TOKEN_NEWLINE) break;
                                                        }
                                                        if (est.type == TOKEN_EST) {
                                                            prescan_capture_type_spec(
                                                                &peekProc, NULL);
                                                        } else {
                                                            peekProc = saveP;
                                                        }
                                                        /* Lookahead pour EQUAL
                                                         * (valeur par defaut). */
                                                        Scanner saveEq = peekProc;
                                                        Token eq;
                                                        for (;;) {
                                                            eq = scanner_scan_token(&peekProc);
                                                            if (eq.type != TOKEN_NEWLINE) break;
                                                        }
                                                        bool hasDefault =
                                                            (eq.type == TOKEN_EQUAL);
                                                        if (!hasDefault) {
                                                            peekProc = saveEq;
                                                            minArity++;
                                                        } else {
                                                            /* Avale la valeur de
                                                             * default jusqu'a
                                                             * COMMA ou RPAREN. */
                                                            int parenDepth = 0;
                                                            for (;;) {
                                                                Token dv = scanner_scan_token(&peekProc);
                                                                if (dv.type == TOKEN_LEFT_PAREN) {
                                                                    parenDepth++;
                                                                    continue;
                                                                }
                                                                if (dv.type == TOKEN_RIGHT_PAREN) {
                                                                    if (parenDepth == 0) {
                                                                        /* Fin params */
                                                                        goto cstr_params_done;
                                                                    }
                                                                    parenDepth--;
                                                                    continue;
                                                                }
                                                                if (parenDepth == 0 &&
                                                                    dv.type == TOKEN_COMMA) {
                                                                    break;
                                                                }
                                                                if (dv.type == TOKEN_EOF) {
                                                                    goto cstr_params_done;
                                                                }
                                                            }
                                                            continue;
                                                        }
                                                    }
                                                    /* Cherche COMMA ou RPAREN. */
                                                    for (;;) {
                                                        Token next4;
                                                        for (;;) {
                                                            next4 = scanner_scan_token(&peekProc);
                                                            if (next4.type != TOKEN_NEWLINE) break;
                                                        }
                                                        if (next4.type == TOKEN_COMMA) break;
                                                        if (next4.type == TOKEN_RIGHT_PAREN ||
                                                            next4.type == TOKEN_EOF) {
                                                            goto cstr_params_done;
                                                        }
                                                    }
                                                }
                                            }
                                            cstr_params_done:
                                            csig->paramCount = minArity;
                                            t->classMethodCount++;
                                        }
                                    }
                                    cdepth++;
                                    continue;
                                }
                                if (ctok.type == TOKEN_PROPRIETES) { cdepth++; continue; }
                                /*  : ouvrants lexicaux dans le body
                                 * des methodes de classe (cdepth > 1). Sans
                                 * ce tracking, un POUR/SI dans une methode
                                 * faisait dribbler cdepth a 0 et la fin du
                                 * sub-scan se faisait prematurement, faisant
                                 * manquer Herite/Implemente declares apres
                                 * la 1ere methode dribblee. Helper partage
                                 * avec la boucle principale (peek inline SI
                                 * inclus). CONSTANTE et PROCEDURE/FONCTION
                                 * sont traites ci-dessus en cas specialise
                                 * (capture des membres/methodes), donc on
                                 * filtre ici sur les 6 ouvrants lexicaux
                                 * purs (SI/POUR/TANTQUE/BOUCLE/SELON/QUAND). */
                                if (ctok.type == TOKEN_SI       ||
                                    ctok.type == TOKEN_POUR     ||
                                    ctok.type == TOKEN_TANTQUE  ||
                                    ctok.type == TOKEN_BOUCLE   ||
                                    ctok.type == TOKEN_SELON    ||
                                    ctok.type == TOKEN_QUAND) {
                                    int d = prescan_depth_delta(ctok, &savedScan);
                                    if (d > 0) cdepth += d;
                                    continue;
                                }
                                /*  : mot-cle de visibilite au niveau
                                 * direct du corps de classe. Pose curVis pour
                                 * les champs declares en dessous. GLOBAL/LOCAL
                                 * n'affectent pas la visibilite (axe orthogonal,
                                 * cf. class_declaration). */
                                if (cdepth == 1 && !inConstBlock) {
                                    if (ctok.type == TOKEN_PUBLIC)  { curVis = 0; inGlobalBlock = false; continue; }
                                    if (ctok.type == TOKEN_PRIVE)   { curVis = 1; inGlobalBlock = false; continue; }
                                    if (ctok.type == TOKEN_PROTEGE) { curVis = 2; inGlobalBlock = false; continue; }
                                    if (ctok.type == TOKEN_GLOBAL)  { inGlobalBlock = true;  continue; }
                                    if (ctok.type == TOKEN_LOCAL)   { inGlobalBlock = false; continue; }
                                }
                                /* Membres au niveau direct du corps de classe. */
                                if (cdepth == 1 && !inConstBlock &&
                                    ctok.type == TOKEN_CONSTANTE) {
                                    /* CONSTANTE NAME = value  (inline) ou
                                     * CONSTANTE\n NAME = value ... FIN (bloc). */
                                    Scanner peekc = savedScan;
                                    Token cn;
                                    for (;;) {
                                        cn = scanner_scan_token(&peekc);
                                        if (cn.type != TOKEN_NEWLINE) break;
                                    }
                                    if (cn.type == TOKEN_IDENTIFIER) {
                                        symtable_add_member(t, identTok.start,
                                            identTok.length, cn.start, cn.length,
                                            NULL, 0, fileIndex,
                                            cn.line, cn.column, 0 /*PUBLIC*/);
                                        savedScan = peekc;
                                    } else {
                                        inConstBlock = true;
                                    }
                                    continue;
                                }
                                if (cdepth == 1 && inConstBlock &&
                                    ctok.type == TOKEN_IDENTIFIER) {
                                    /* Ligne de bloc CONSTANTE : NAME = value. */
                                    symtable_add_member(t, identTok.start,
                                        identTok.length, ctok.start, ctok.length,
                                        NULL, 0, fileIndex,
                                        ctok.line, ctok.column, 0 /*PUBLIC*/);
                                    continue;
                                }
                                /* Champ : IDENT EST <type>. */
                                if (cdepth == 1 && !inConstBlock &&
                                    ctok.type == TOKEN_IDENTIFIER) {
                                    Token fieldName = ctok;
                                    Scanner peekf = savedScan;
                                    Token est;
                                    for (;;) {
                                        est = scanner_scan_token(&peekf);
                                        if (est.type != TOKEN_NEWLINE) break;
                                    }
                                    if (est.type == TOKEN_EST) {
                                        char typeBuf[32];
                                        prescan_capture_type_spec(&peekf, typeBuf);
                                        symtable_add_member(t, identTok.start,
                                            identTok.length, fieldName.start,
                                            fieldName.length, typeBuf,
                                            (int)strlen(typeBuf), fileIndex,
                                            fieldName.line, fieldName.column,
                                            inGlobalBlock ? -1 : curVis);
                                        savedScan = peekf;
                                    }
                                    continue;
                                }
                                if (ctok.type == TOKEN_IMPLEMENTE && cdepth == 1) {
                                    Token ifaceTok;
                                    for (;;) {
                                        ifaceTok = scanner_scan_token(&savedScan);
                                        if (ifaceTok.type != TOKEN_NEWLINE) break;
                                    }
                                    if (ifaceTok.type == TOKEN_IDENTIFIER &&
                                        t->implementationCount < 64) {
                                        int ii = t->implementationCount;
                                        int cl2 = identTok.length < 127 ? identTok.length : 127;
                                        memcpy(t->implementations[ii].className, identTok.start, cl2);
                                        t->implementations[ii].className[cl2] = '\0';
                                        int il = ifaceTok.length < 127 ? ifaceTok.length : 127;
                                        memcpy(t->implementations[ii].interfaceName, ifaceTok.start, il);
                                        t->implementations[ii].interfaceName[il] = '\0';
                                        t->implementationCount++;
                                    }
                                }
                                /*  : track Herite de pour le check
                                 * de contrat transitif (cChild herite de cBase
                                 * herite implicitement de implemente iY). */
                                if (ctok.type == TOKEN_HERITE && cdepth == 1) {
                                    Token deTok;
                                    for (;;) {
                                        deTok = scanner_scan_token(&savedScan);
                                        if (deTok.type != TOKEN_NEWLINE) break;
                                    }
                                    /* 'de' optionnel canon (Herite cBase ou Herite de cBase). */
                                    Token parentTok = deTok;
                                    if (deTok.type == TOKEN_DE) {
                                        for (;;) {
                                            parentTok = scanner_scan_token(&savedScan);
                                            if (parentTok.type != TOKEN_NEWLINE) break;
                                        }
                                    }
                                    if (parentTok.type == TOKEN_IDENTIFIER &&
                                        t->inheritanceCount < 128) {
                                        int ii = t->inheritanceCount;
                                        int cl3 = identTok.length < 127 ? identTok.length : 127;
                                        memcpy(t->inheritances[ii].className, identTok.start, cl3);
                                        t->inheritances[ii].className[cl3] = '\0';
                                        int pl = parentTok.length < 127 ? parentTok.length : 127;
                                        memcpy(t->inheritances[ii].parentName, parentTok.start, pl);
                                        t->inheritances[ii].parentName[pl] = '\0';
                                        t->inheritanceCount++;
                                    }
                                }
                            }
                        }
                        depth++;
                    } else if (after.type == TOKEN_STRUCTURE) {
                        symtable_add(t, identTok.start, identTok.length,
                                     SYM_STRUCT, fileIndex, false,
                                     identTok.line, identTok.column);
                        /*  : capture des champs de structure
                         * (`NAME est un Type`), un par ligne jusqu'a FIN.
                         * Scan lateral sans consommer le scanner principal. */
                        {
                            Scanner savedScan = scanner;
                            int sdepth = 1;
                            for (;;) {
                                Token stok = scanner_scan_token(&savedScan);
                                if (stok.type == TOKEN_EOF) break;
                                if (stok.type == TOKEN_NEWLINE) continue;
                                if (stok.type == TOKEN_FIN) {
                                    sdepth--;
                                    if (sdepth <= 0) break;
                                    continue;
                                }
                                if (stok.type == TOKEN_PROPRIETES) { sdepth++; continue; }
                                if (sdepth == 1 && stok.type == TOKEN_IDENTIFIER) {
                                    Token fieldName = stok;
                                    Scanner peekf = savedScan;
                                    Token est;
                                    for (;;) {
                                        est = scanner_scan_token(&peekf);
                                        if (est.type != TOKEN_NEWLINE) break;
                                    }
                                    if (est.type == TOKEN_EST) {
                                        char typeBuf[32];
                                        prescan_capture_type_spec(&peekf, typeBuf);
                                        symtable_add_member(t, identTok.start,
                                            identTok.length, fieldName.start,
                                            fieldName.length, typeBuf,
                                            (int)strlen(typeBuf), fileIndex,
                                            fieldName.line, fieldName.column,
                                            -1 /*champ struct : hors POO vis*/);
                                        savedScan = peekf;
                                    }
                                }
                            }
                        }
                        depth++;
                    } else if (after.type == TOKEN_INTERFACE) {
                        symtable_add(t, identTok.start, identTok.length,
                                     SYM_INTERFACE, fileIndex, false,
                                     identTok.line, identTok.column);

                        /* Parse interface body to extract required methods.
                         *  : capture aussi paramCount + paramTypes +
                         * returnType pour le check de signature. */
                        if (t->interfaceCount < 64) {
                            int idx = t->interfaceCount;
                            int copyLen = identTok.length < 127 ? identTok.length : 127;
                            memcpy(t->interfaces[idx].name, identTok.start, copyLen);
                            t->interfaces[idx].name[copyLen] = '\0';
                            t->interfaces[idx].methodCount = 0;

                            /* Scan until FIN for PROCEDURE declarations */
                            while (1) {
                                Token itok;
                                for (;;) {
                                    itok = scanner_scan_token(&scanner);
                                    if (itok.type != TOKEN_NEWLINE) break;
                                }
                                if (itok.type == TOKEN_FIN || itok.type == TOKEN_EOF) break;

                                if (itok.type == TOKEN_PROCEDURE || itok.type == TOKEN_FONCTION) {
                                    /* Next token is the method name */
                                    Token mname;
                                    for (;;) {
                                        mname = scanner_scan_token(&scanner);
                                        if (mname.type != TOKEN_NEWLINE) break;
                                    }
                                    if (mname.type == TOKEN_IDENTIFIER &&
                                        t->interfaces[idx].methodCount < 32) {
                                        int mc = t->interfaces[idx].methodCount;
                                        PrescanMethodSig* sig =
                                            &t->interfaces[idx].methods[mc];
                                        int ml = mname.length < 127 ? mname.length : 127;
                                        memcpy(sig->name, mname.start, ml);
                                        sig->name[ml] = '\0';
                                        sig->paramCount = 0;
                                        memset(sig->paramTypes, 0,
                                               sizeof(sig->paramTypes));
                                        sig->returnType[0] = '\0';

                                        /* Parse signature parametres + retour. */
                                        Token tok2;
                                        for (;;) {
                                            tok2 = scanner_scan_token(&scanner);
                                            if (tok2.type != TOKEN_NEWLINE) break;
                                        }
                                        if (tok2.type == TOKEN_LEFT_PAREN) {
                                            /* Parse params jusqu'a ')' */
                                            for (;;) {
                                                Token pname;
                                                for (;;) {
                                                    pname = scanner_scan_token(&scanner);
                                                    if (pname.type != TOKEN_NEWLINE) break;
                                                }
                                                if (pname.type == TOKEN_RIGHT_PAREN ||
                                                    pname.type == TOKEN_EOF) break;
                                                if (pname.type == TOKEN_IDENTIFIER) {
                                                    /* Optional `est <type>` */
                                                    Scanner savedAfterName = scanner;
                                                    Token est;
                                                    for (;;) {
                                                        est = scanner_scan_token(&scanner);
                                                        if (est.type != TOKEN_NEWLINE) break;
                                                    }
                                                    char* slot = (sig->paramCount <
                                                                  PRESCAN_MAX_METHOD_PARAMS)
                                                        ? sig->paramTypes[sig->paramCount]
                                                        : NULL;
                                                    if (est.type == TOKEN_EST) {
                                                        prescan_capture_type_spec(&scanner, slot);
                                                    } else {
                                                        scanner = savedAfterName;
                                                    }
                                                    sig->paramCount++;
                                                }
                                                /* Avance jusqu'a ',' ou ')' */
                                                for (;;) {
                                                    Token next2;
                                                    for (;;) {
                                                        next2 = scanner_scan_token(&scanner);
                                                        if (next2.type != TOKEN_NEWLINE) break;
                                                    }
                                                    if (next2.type == TOKEN_COMMA) break;
                                                    if (next2.type == TOKEN_RIGHT_PAREN ||
                                                        next2.type == TOKEN_EOF) {
                                                        goto interface_method_done;
                                                    }
                                                }
                                            }
                                        }
                                        interface_method_done:
                                        /* Type retour optionnel `: type` (peek). */
                                        {
                                            Scanner savedColon = scanner;
                                            Token colon;
                                            for (;;) {
                                                colon = scanner_scan_token(&scanner);
                                                if (colon.type != TOKEN_NEWLINE) break;
                                            }
                                            if (colon.type == TOKEN_COLON) {
                                                prescan_capture_type_spec(
                                                    &scanner, sig->returnType);
                                            } else {
                                                scanner = savedColon;
                                            }
                                        }

                                        t->interfaces[idx].methodCount++;
                                    }
                                }
                            }
                            t->interfaceCount++;
                        } else {
                            /* Too many interfaces, just skip body */
                            depth++;
                        }
                        /* Don't increase depth — we already consumed until FIN */
                    } else if (after.type == TOKEN_ENUMERATION ||
                               after.type == TOKEN_COMBINAISON) {
                        symtable_add(t, identTok.start, identTok.length,
                                     SYM_ENUM, fileIndex, false,
                                     identTok.line, identTok.column);
                        /*  : capture des membres d'enum. Chaque membre
                         * est un identifiant en debut de ligne (avec `= valeur`
                         * optionnel), jusqu'a FIN. Owner = nom de l'enum. */
                        {
                            Scanner savedScan = scanner;
                            for (;;) {
                                Token etok = scanner_scan_token(&savedScan);
                                if (etok.type == TOKEN_EOF) break;
                                if (etok.type == TOKEN_NEWLINE) continue;
                                if (etok.type == TOKEN_FIN) break;
                                if (etok.type == TOKEN_IDENTIFIER) {
                                    symtable_add_member(t, identTok.start,
                                        identTok.length, etok.start, etok.length,
                                        NULL, 0, fileIndex,
                                        etok.line, etok.column,
                                        -1 /*membre enum : hors POO vis*/);
                                    /* Saute le reste de la ligne (= valeur). */
                                    for (;;) {
                                        Token sk = scanner_scan_token(&savedScan);
                                        if (sk.type == TOKEN_NEWLINE ||
                                            sk.type == TOKEN_EOF) break;
                                        if (sk.type == TOKEN_FIN) goto enum_members_done;
                                    }
                                }
                            }
                            enum_members_done: ;
                        }
                        depth++;
                    } else if (after.type == TOKEN_FENETRE ||
                               after.type == TOKEN_ETAT) {
                        /* DSL Fenetre / Etat top-level. Le nom (FEN_X / ETAT_X)
                         * est une variable globale (le runtime expose le nom
                         * comme global string via vm_window/etat_register).
                         * Cross-file, ces noms doivent etre referencables
                         * meme si le fichier n'a pas encore ete compile.
                         *  : Fenetre -> SYM_WINDOW (kind distinct pour
                         * la pre-population cross-fichier de g_prescan_widgets,
                         * qui ancre le type cible des litteraux multilingues
                         * `FEN_X..Titre = {...}`). Etat reste SYM_VARIABLE en
                         * attendant un eventuel support kind dedie. */
                        SymbolKind k = (after.type == TOKEN_FENETRE)
                                        ? SYM_WINDOW : SYM_VARIABLE;
                        symtable_add(t, identTok.start, identTok.length,
                                     k, fileIndex, false,
                                     identTok.line, identTok.column);
                        symtable_add_element(t, identTok.start, identTok.length,
                                             fileIndex, identTok.line,
                                             identTok.column);  /*  */
                        depth++;
                    } else if (after.type == TOKEN_IDENTIFIER &&
                               (prescan_ident_is_collection_kw(after.start,
                                                               after.length) ||
                                prescan_ident_is_theme_kw(after.start,
                                                          after.length))) {
                        /*  — CollectionDeProcedures / Theme : element
                         * top-level (pas de token dedie). Enregistre le nom en
                         * SYM_VARIABLE (comportement historique — acces qualifie
                         * COL.membre / Theme.style) ET dans le registre
                         * d'elements pour le check d'unicite. Pas de depth++ :
                         * on preserve strictement le scan de corps existant (les
                         * types membres de collection restent des SYM top-level,
                         * cf. resolution /1177). */
                        if (!confine) {
                            symtable_add(t, identTok.start, identTok.length,
                                         SYM_VARIABLE, fileIndex, false,
                                         identTok.line, identTok.column);
                        }
                        symtable_add_element(t, identTok.start, identTok.length,
                                             fileIndex, identTok.line,
                                             identTok.column);
                    } else if (after.type != TOKEN_EOF &&
                               after.type != TOKEN_NEWLINE) {
                        /* Regular variable declaration: `gVar est <type>` (ou
                         * `gVar est une chaine`, etc. -- les articles "un/une"
                         * sont skip par le scanner). On enregistre le nom comme
                         * SYM_VARIABLE pour permettre la forward reference :
                         * une PROCEDURE déclarée plus haut dans le fichier
                         * peut référencer ce global même s'il est défini en
                         * dessous d'elle (cf VISION §5bis "Forward refs").
                         *  — sauf en mode LOCAL : la variable est
                         * confinee au flux top-level, jamais indexee au projet. */
                        if (!confine) {
                            symtable_add(t, identTok.start, identTok.length,
                                         SYM_VARIABLE, fileIndex, false,
                                         identTok.line, identTok.column);
                        }
                        /* Pas d'increment de depth — la déclaration tient sur
                         * une ligne (avec ou sans = init), pas de FIN à matcher. */
                    }
                }

                continue;
            }
        } else {
            /* Inside a block (depth > 0): track nested blocks.
             *
             *  : helper centralise `prescan_depth_delta` qui
             * couvre PROCEDURE/FONCTION/PROPRIETES + ouvrants
             * lexicaux SI/POUR/TANTQUE/BOUCLE/SELON/QUAND + CONSTANTE
             * (avec peek inline). Remplace les patches au coup par
             * coup historiques ( Lot C, ). FIN n'est
             * pas traite ici : decremente plus haut, avant le test
             * depth == 0.
             *
             * NB : la branche IDENT EST <widget> reste manuelle —
             * elle ne se reduit pas a un simple delta (capture du
             * type de sous-widget + restauration du scanner si la
             * forme ne match pas une decl). */
            int delta = prescan_depth_delta(tok, &scanner);
            if (delta > 0) {
                depth += delta;
                continue;
            }

            /* DSL Fenetre/Etat: les sous-widgets `IDENT est un/une <Type>`
             * (Table, Bouton, Saisie, Libelle, Conteneur, colonne) ouvrent
             * un bloc termine par FIN. On doit incrementer depth sinon
             * leur FIN fait redescendre depth a 0 au milieu de la fenetre
             * et les noms de colonnes finissent catalogues comme globals.
             *
             *  Lot C : meme traitement pour les types user-defined
             * autorises dans le body Fenetre (, hisses au scope
             * enclosing par le compilateur) : STRUCTURE / ENUMERATION /
             * COMBINAISON. CLASSE et INTERFACE sont rejetees en amont par
             * E_SEM_DSL_TYPE_REJECTED_IN_WINDOW — pas besoin de tracker. */
            if (tok.type == TOKEN_IDENTIFIER) {
                Scanner saved = scanner;
                Token next;
                for (;;) {
                    next = scanner_scan_token(&scanner);
                    if (next.type != TOKEN_NEWLINE) break;
                }
                if (next.type == TOKEN_EST) {
                    Token after;
                    for (;;) {
                        after = scanner_scan_token(&scanner);
                        if (after.type != TOKEN_NEWLINE) break;
                    }
                    if (after.type == TOKEN_TABLE   || after.type == TOKEN_BOUTON  ||
                        after.type == TOKEN_SAISIE  || after.type == TOKEN_LIBELLE ||
                        after.type == TOKEN_CONTENEUR || after.type == TOKEN_COLONNE ||
                        after.type == TOKEN_STRUCTURE   ||
                        after.type == TOKEN_ENUMERATION ||
                        after.type == TOKEN_COMBINAISON) {
                        depth++;
                    }
                    /* Autres types (chaine, entier, etc.) = decl variable
                     * locale sur une ligne, pas de FIN a matcher. */
                } else {
                    /* Pas une declaration : restaure le scanner. */
                    scanner = saved;
                }
            }
        }

    }

    return true;
}

/* ================================================================
 * INTERFACE VERIFICATION
 * ================================================================ */

/* Cherche une methode declaree pour `className` (case-insensible).
 * Retourne la signature ou NULL. */
static const PrescanMethodSig* find_class_method_sig(SymbolTable* t,
                                                     const char* className,
                                                     const char* methodName) {
    for (int i = 0; i < t->classMethodCount; i++) {
        if (strcasecmp(t->classMethods[i].className, className) == 0 &&
            strcasecmp(t->classMethods[i].sig.name, methodName) == 0) {
            return &t->classMethods[i].sig;
        }
    }
    return NULL;
}

/* Cherche une methode declaree pour `className` OU une de ses classes
 * ancetres (chaine via t->inheritances). Limite anti-cycle a 64 hops. */
static const PrescanMethodSig* find_class_method_sig_inherited(
        SymbolTable* t, const char* className, const char* methodName) {
    const char* cur = className;
    char buf[128];
    for (int hops = 0; hops < 64; hops++) {
        const PrescanMethodSig* s = find_class_method_sig(t, cur, methodName);
        if (s) return s;
        /* Cherche parent. */
        const char* nextParent = NULL;
        for (int i = 0; i < t->inheritanceCount; i++) {
            if (strcasecmp(t->inheritances[i].className, cur) == 0) {
                nextParent = t->inheritances[i].parentName;
                break;
            }
        }
        if (!nextParent) return NULL;
        /* Copie pour pouvoir re-iterer. */
        size_t l = strlen(nextParent);
        if (l >= sizeof(buf)) l = sizeof(buf) - 1;
        memcpy(buf, nextParent, l);
        buf[l] = '\0';
        cur = buf;
    }
    return NULL;
}

/*  : variante publique consommee par compiler.c (resolution d'un
 * appel nu `Calcule(...)` dans une methode soeur/heritee). Recopie locale
 * de className+methodName en buffers null-terminus pour reutiliser la
 * version case-insensible existante (strcasecmp sur asciiz). */
bool symtable_has_class_method_inherited(SymbolTable* t,
                                         const char* className, int classLen,
                                         const char* methodName, int methodLen) {
    if (t == NULL || classLen <= 0 || methodLen <= 0) return false;
    char cBuf[128];
    char mBuf[128];
    int cl = classLen < (int)sizeof(cBuf) - 1 ? classLen : (int)sizeof(cBuf) - 1;
    int ml = methodLen < (int)sizeof(mBuf) - 1 ? methodLen : (int)sizeof(mBuf) - 1;
    memcpy(cBuf, className, cl); cBuf[cl] = '\0';
    memcpy(mBuf, methodName, ml); mBuf[ml] = '\0';
    return find_class_method_sig_inherited(t, cBuf, mBuf) != NULL;
}

/*  : verification etendue. Au-dela de l'existence (legacy),
 * compare la signature complete : paramCount + paramTypes + returnType.
 * "Type" est compare case-insensitive ; vide cote interface OU cote impl
 * = permissif (pas de divergence reportee). */
bool symtable_verify_interfaces(SymbolTable* t) {
    bool ok = true;
    for (int i = 0; i < t->implementationCount; i++) {
        const char* className = t->implementations[i].className;
        const char* ifaceName = t->implementations[i].interfaceName;

        /* Find the interface declaration */
        int ifaceIdx = -1;
        for (int j = 0; j < t->interfaceCount; j++) {
            if (strcasecmp(t->interfaces[j].name, ifaceName) == 0) {
                ifaceIdx = j;
                break;
            }
        }

        if (ifaceIdx == -1) {
            fprintf(stderr,
                "Error: la classe '%s' implemente l'interface inconnue '%s'.\n",
                className, ifaceName);
            ok = false;
            continue;
        }

        /* Check each required method */
        for (int m = 0; m < t->interfaces[ifaceIdx].methodCount; m++) {
            const PrescanMethodSig* req = &t->interfaces[ifaceIdx].methods[m];
            const PrescanMethodSig* impl =
                find_class_method_sig_inherited(t, className, req->name);

            if (impl == NULL) {
                fprintf(stderr,
                    "Error: la classe '%s' implemente l'interface '%s' mais "
                    "ne fournit pas la methode '%s' signee dans l'interface.\n",
                    className, ifaceName, req->name);
                ok = false;
                continue;
            }

            /* Compare paramCount. */
            if (impl->paramCount != req->paramCount) {
                fprintf(stderr,
                    "Error: la methode '%s' de la classe '%s' ne respecte pas la "
                    "signature de l'interface '%s' (attendu : %d parametre(s), "
                    "trouve : %d).\n",
                    req->name, className, ifaceName,
                    req->paramCount, impl->paramCount);
                ok = false;
                continue;
            }

            /* Compare returnType (permissif si l'un des deux est vide). */
            if (req->returnType[0] != '\0' && impl->returnType[0] != '\0' &&
                strcasecmp(req->returnType, impl->returnType) != 0) {
                fprintf(stderr,
                    "Error: la methode '%s' de la classe '%s' ne respecte pas la "
                    "signature de l'interface '%s' (type de retour attendu : "
                    "'%s', trouve : '%s').\n",
                    req->name, className, ifaceName,
                    req->returnType, impl->returnType);
                ok = false;
                continue;
            }

            /* Compare paramTypes position par position. */
            for (int p = 0; p < req->paramCount &&
                            p < PRESCAN_MAX_METHOD_PARAMS; p++) {
                if (req->paramTypes[p][0] != '\0' &&
                    impl->paramTypes[p][0] != '\0' &&
                    strcasecmp(req->paramTypes[p],
                               impl->paramTypes[p]) != 0) {
                    fprintf(stderr,
                        "Error: la methode '%s' de la classe '%s' ne respecte "
                        "pas la signature de l'interface '%s' (parametre %d : "
                        "type attendu '%s', trouve '%s').\n",
                        req->name, className, ifaceName,
                        p + 1,
                        req->paramTypes[p], impl->paramTypes[p]);
                    ok = false;
                    break;
                }
            }
        }
    }
    return ok;
}

/* ================================================================
 * TRI TOPOLOGIQUE DES COMPAGNONS PAR CHAINE D'HERITAGE (, )
 *
 * Probleme historique : un ordre de decouverte alphabetique/lexicographique
 * plante des qu'un parent est nomme APRES un enfant. Ex : cAnimal Herite de
 * cMammifere -> ordre alpha = cAnimal, cMammifere -> cAnimal materialise en
 * premier, 'cMammifere' inconnu -> CRASH.
 *
 * Solution : apres prescan (qui a rempli t->inheritances[]), trier les
 * compagnons par profondeur d'heritage croissante. Parent avant enfant,
 * garanti. Fallback stable (ordre alpha/decouverte) entre freres.
 *
 * Partage entre owl test (testing.c, materialisation des compagnons de test)
 * et owl <fichier>/owl <projet> (project_run.c, phase 1.b). L'appelant
 * fournit companionIndices deja trie de maniere stable ; cet ordre sert de
 * seed pour les classes de meme profondeur (et les fichiers sans classe).
 * ================================================================ */

/* Profondeur d'heritage d'une classe (0 = racine ou inconnue).
 * Lookup case-insensitive dans t->inheritances[]. Garde anti-cycle via
 * maxDepth — si on depasse, on coupe (retourne -1). Comparaison
 * case-insensitive : OWL est case-insensible (invariant 1). */
static int class_inheritance_depth(const SymbolTable* t, const char* className,
                                   int maxDepth) {
    if (className == NULL || className[0] == '\0') return 0;
    const char* current = className;
    int depth = 0;
    while (depth <= maxDepth) {
        const char* parent = NULL;
        for (int i = 0; i < t->inheritanceCount; i++) {
            if (strcasecmp(t->inheritances[i].className, current) == 0) {
                parent = t->inheritances[i].parentName;
                break;
            }
        }
        if (parent == NULL || parent[0] == '\0') return depth;
        depth++;
        current = parent;
    }
    /* Cycle ou chaine pathologique : fallback prudent. */
    return -1;
}

/* Profondeur max parmi les classes declarees par fileIndex. 0 si le
 * fichier ne declare aucune classe (procs/structs/consts uniquement) —
 * il peut etre charge en premier sans risque. */
static int file_max_class_depth(const SymbolTable* t, int fileIndex) {
    int maxD = 0;
    bool sawCycle = false;
    for (int i = 0; i < t->count; i++) {
        const ProjectSymbol* s = &t->symbols[i];
        if (s->kind != SYM_CLASS) continue;
        if (s->fileIndex != fileIndex) continue;
        int d = class_inheritance_depth(t, s->name, t->inheritanceCount + 1);
        if (d < 0) { sawCycle = true; continue; }
        if (d > maxD) maxD = d;
    }
    /* En cas de cycle detecte, on pousse le fichier en fin de liste —
     * le compile produira l'erreur appropriee, mais au moins les autres
     * fichiers passent. */
    if (sawCycle) return 1000000;
    return maxD;
}

/* Cle de tri : (depth, indexOriginal) pour stabilite. */
typedef struct {
    int index;
    int depth;
    int seq; /* position dans l'ordre stable initial — tiebreaker */
} CompanionSortKey;

static int compare_companion_topo(const void* a, const void* b) {
    const CompanionSortKey* ka = (const CompanionSortKey*)a;
    const CompanionSortKey* kb = (const CompanionSortKey*)b;
    if (ka->depth != kb->depth) return ka->depth - kb->depth;
    return ka->seq - kb->seq;
}

void topo_sort_companions_by_inheritance(const SymbolTable* t,
                                         int* companionIndices,
                                         int companionCount) {
    if (companionCount <= 1) return;
    CompanionSortKey keys[MAX_PROJECT_FILES];
    if (companionCount > MAX_PROJECT_FILES) companionCount = MAX_PROJECT_FILES;
    for (int c = 0; c < companionCount; c++) {
        keys[c].index = companionIndices[c];
        keys[c].seq   = c; /* position stable = tiebreaker */
        keys[c].depth = file_max_class_depth(t, companionIndices[c]);
    }
    qsort(keys, companionCount, sizeof(CompanionSortKey), compare_companion_topo);
    for (int c = 0; c < companionCount; c++) {
        companionIndices[c] = keys[c].index;
    }
}
