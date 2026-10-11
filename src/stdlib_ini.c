// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * stdlib_ini.c — Manipulation section-consciente de fichiers .ini quelconques :
 * INILit (EN INIRead) et INIEcrit (EN INIWrite).
 *
 * Distinct du support `paramIni` de stdlib_parametre.c, qui n'expose qu'une
 * table PLATE dans une section unique [Global] adossee a la famille Parametre.
 * Ici on lit et on ecrit des sections ARBITRAIRES d'un fichier .ini nomme
 * explicitement : valeur d'une cle, liste des sections, liste des cles d'une
 * section, creation/suppression de cles et de sections.
 *
 * ── Divergences assumees vis-a-vis du canon externe (doc 3013043 / 3013039) ──
 * Le canon d'origine fait defaut sur WIN.INI et sur des chemins systeme
 * virtualises UAC quand le fichier est omis. Inadapte aux piliers OWL
 * "cross-platform" et "deterministe" : ici le parametre <Fichier> est
 * OBLIGATOIRE (l'omettre est une erreur d'arite refusee a la compilation).
 * Aucun fichier par defaut, jamais WIN.INI, aucune virtualisation UAC, aucun
 * comportement plateforme-specifique. Encodage UTF-8 sans BOM.
 *
 * Chaque appel est une operation fichier atomique : INILit relit le fichier a
 * neuf ; INIEcrit relit, mute en memoire, reecrit. Aucun etat global persistant.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * MODELE EN MEMOIRE
 * ================================================================ */

typedef struct { char* key; char* value; } IniPair;
typedef struct {
    char*    name;
    IniPair* pairs;
    int      count;
    int      cap;
} IniSection;
typedef struct {
    IniSection* sections;
    int         count;
    int         cap;
} IniDoc;

static char* ini_dup(const char* s, int len) {
    char* out = (char*)malloc((size_t)len + 1);
    if (!out) return NULL;
    if (len > 0) memcpy(out, s, (size_t)len);
    out[len] = '\0';
    return out;
}

/* Comparaison de noms de section/cle : insensible a la casse ASCII (les octets
 * >= 0x80, dont les accents UTF-8, sont compares a l'identique). */
static bool ini_name_eq(const char* a, const char* b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static void ini_doc_free(IniDoc* doc) {
    for (int i = 0; i < doc->count; i++) {
        IniSection* s = &doc->sections[i];
        for (int j = 0; j < s->count; j++) {
            free(s->pairs[j].key);
            free(s->pairs[j].value);
        }
        free(s->pairs);
        free(s->name);
    }
    free(doc->sections);
    doc->sections = NULL;
    doc->count = doc->cap = 0;
}

static IniSection* ini_section_find(IniDoc* doc, const char* name) {
    for (int i = 0; i < doc->count; i++) {
        if (ini_name_eq(doc->sections[i].name, name)) return &doc->sections[i];
    }
    return NULL;
}

static IniSection* ini_section_add(IniDoc* doc, const char* name, int len) {
    if (doc->count == doc->cap) {
        int newCap = doc->cap < 4 ? 4 : doc->cap * 2;
        IniSection* grown = (IniSection*)realloc(doc->sections,
                                                 sizeof(IniSection) * (size_t)newCap);
        if (!grown) return NULL;
        doc->sections = grown;
        doc->cap = newCap;
    }
    IniSection* s = &doc->sections[doc->count];
    s->name = ini_dup(name, len);
    s->pairs = NULL;
    s->count = s->cap = 0;
    if (s->name == NULL) return NULL;
    doc->count++;
    return s;
}

static IniPair* ini_pair_find(IniSection* s, const char* key) {
    for (int i = 0; i < s->count; i++) {
        if (ini_name_eq(s->pairs[i].key, key)) return &s->pairs[i];
    }
    return NULL;
}

/* Pose ou met a jour une cle. Conserve le nom stocke a l'origine sur update. */
static void ini_pair_set(IniSection* s, const char* key, int keyLen,
                         const char* value, int valLen) {
    IniPair* p = ini_pair_find(s, key);
    if (p != NULL) {
        char* v = ini_dup(value, valLen);
        if (v == NULL) return;
        free(p->value);
        p->value = v;
        return;
    }
    if (s->count == s->cap) {
        int newCap = s->cap < 4 ? 4 : s->cap * 2;
        IniPair* grown = (IniPair*)realloc(s->pairs, sizeof(IniPair) * (size_t)newCap);
        if (!grown) return;
        s->pairs = grown;
        s->cap = newCap;
    }
    char* k = ini_dup(key, keyLen);
    char* v = ini_dup(value, valLen);
    if (k == NULL || v == NULL) { free(k); free(v); return; }
    s->pairs[s->count].key = k;
    s->pairs[s->count].value = v;
    s->count++;
}

static void ini_pair_remove(IniSection* s, const char* key) {
    for (int i = 0; i < s->count; i++) {
        if (!ini_name_eq(s->pairs[i].key, key)) continue;
        free(s->pairs[i].key);
        free(s->pairs[i].value);
        for (int j = i; j < s->count - 1; j++) s->pairs[j] = s->pairs[j + 1];
        s->count--;
        return;
    }
}

static void ini_section_remove(IniDoc* doc, const char* name) {
    for (int i = 0; i < doc->count; i++) {
        if (!ini_name_eq(doc->sections[i].name, name)) continue;
        IniSection* s = &doc->sections[i];
        for (int j = 0; j < s->count; j++) {
            free(s->pairs[j].key);
            free(s->pairs[j].value);
        }
        free(s->pairs);
        free(s->name);
        for (int j = i; j < doc->count - 1; j++) doc->sections[j] = doc->sections[j + 1];
        doc->count--;
        return;
    }
}

/* ================================================================
 * IO FICHIER + PARSE / SERIALISE
 * ================================================================ */

static char* ini_read_file(const char* path, int* outLen) {
    *outLen = 0;
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)size + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    *outLen = (int)got;
    return buf;
}

/* Charge un fichier .ini dans le modele. Fichier absent -> document vide (pas
 * une erreur). Lignes avant tout `[...]` -> section implicite de nom "". */
static void ini_doc_load(IniDoc* doc, const char* path) {
    doc->sections = NULL;
    doc->count = doc->cap = 0;

    int len = 0;
    char* text = ini_read_file(path, &len);
    if (text == NULL) return;

    IniSection* cur = NULL;
    int i = 0;
    while (i < len) {
        int start = i;
        while (i < len && text[i] != '\n') i++;
        int end = i;
        if (i < len) i++;                        /* saute le '\n' */
        if (end > start && text[end - 1] == '\r') end--;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
        if (start >= end) continue;
        if (text[start] == ';' || text[start] == '#') continue;

        if (text[start] == '[') {
            int close = -1;
            for (int k = start + 1; k < end; k++) {
                if (text[k] == ']') { close = k; break; }
            }
            if (close < 0) continue;             /* en-tete malformee : ignoree */
            cur = ini_section_add(doc, text + start + 1, close - (start + 1));
            continue;
        }

        int eq = -1;
        for (int k = start; k < end; k++) {
            if (text[k] == '=') { eq = k; break; }
        }
        if (eq < 0) continue;
        int nameEnd = eq;
        while (nameEnd > start && (text[nameEnd - 1] == ' ' || text[nameEnd - 1] == '\t')) {
            nameEnd--;
        }
        if (nameEnd == start) continue;
        int valStart = eq + 1;
        while (valStart < end && (text[valStart] == ' ' || text[valStart] == '\t')) {
            valStart++;
        }
        if (cur == NULL) cur = ini_section_add(doc, "", 0);
        if (cur == NULL) break;
        ini_pair_set(cur, text + start, nameEnd - start,
                     text + valStart, end - valStart);
    }
    free(text);
}

/* Reecrit le document. Renvoie false si le fichier ne peut pas etre ouvert en
 * ecriture (repertoire absent : pas de creation d'arborescence). */
static bool ini_doc_write(IniDoc* doc, const char* path) {
    FILE* f = fopen(path, "wb");
    if (f == NULL) return false;
    for (int i = 0; i < doc->count; i++) {
        IniSection* s = &doc->sections[i];
        if (s->name[0] != '\0') fprintf(f, "[%s]\n", s->name);
        for (int j = 0; j < s->count; j++) {
            fprintf(f, "%s=%s\n", s->pairs[j].key, s->pairs[j].value);
        }
        if (i < doc->count - 1) fputc('\n', f);
    }
    fclose(f);
    return true;
}

/* ================================================================
 * ACCES ARGUMENTS
 * ================================================================ */

static const char* arg_cstr(Value v) {
    return IS_STRING(v) ? AS_CSTRING(v) : "";
}
static int arg_len(Value v) {
    return IS_STRING(v) ? AS_STRING(v)->length : 0;
}

/* ================================================================
 * NATIVES
 * ================================================================ */

/* INILit(<Section>, <Cle>, <NonTrouve>, <Fichier>) -> chaine.
 *   section=="" && cle=="" -> liste des sections (separees par CRLF)
 *   section!="" && cle=="" -> liste des cles de la section (separees par CRLF)
 *   section+cle            -> valeur de la cle
 * <NonTrouve> est renvoye si : valeur vide, cle absente, ou fichier absent. */
static Value fn_ini_lit(int argCount, Value* args) {
    (void)argCount;
    const char* section  = arg_cstr(args[0]);
    const char* key      = arg_cstr(args[1]);
    const char* notFound = arg_cstr(args[2]);
    int         notLen   = arg_len(args[2]);
    const char* path     = arg_cstr(args[3]);

    IniDoc doc;
    ini_doc_load(&doc, path);

    /* Liste des sections. */
    if (section[0] == '\0' && key[0] == '\0') {
        char* buf = NULL;
        int cap = 0, len = 0;
        for (int i = 0; i < doc.count; i++) {
            const char* nm = doc.sections[i].name;
            if (nm[0] == '\0') continue;         /* section implicite non listee */
            int nl = (int)strlen(nm);
            int need = len + (len > 0 ? 2 : 0) + nl + 1;
            if (need > cap) {
                int nc = cap < 64 ? 64 : cap;
                while (nc < need) nc *= 2;
                char* g = (char*)realloc(buf, (size_t)nc);
                if (!g) { free(buf); ini_doc_free(&doc); return OBJ_VAL(obj_copy_string("", 0)); }
                buf = g; cap = nc;
            }
            if (len > 0) { buf[len++] = '\r'; buf[len++] = '\n'; }
            memcpy(buf + len, nm, (size_t)nl); len += nl;
        }
        Value out = OBJ_VAL(obj_copy_string(buf ? buf : "", len));
        free(buf);
        ini_doc_free(&doc);
        return out;
    }

    IniSection* s = ini_section_find(&doc, section);

    /* Liste des cles d'une section. */
    if (key[0] == '\0') {
        char* buf = NULL;
        int cap = 0, len = 0;
        if (s != NULL) {
            for (int j = 0; j < s->count; j++) {
                const char* nm = s->pairs[j].key;
                int nl = (int)strlen(nm);
                int need = len + (len > 0 ? 2 : 0) + nl + 1;
                if (need > cap) {
                    int nc = cap < 64 ? 64 : cap;
                    while (nc < need) nc *= 2;
                    char* g = (char*)realloc(buf, (size_t)nc);
                    if (!g) { free(buf); ini_doc_free(&doc); return OBJ_VAL(obj_copy_string("", 0)); }
                    buf = g; cap = nc;
                }
                if (len > 0) { buf[len++] = '\r'; buf[len++] = '\n'; }
                memcpy(buf + len, nm, (size_t)nl); len += nl;
            }
        }
        Value out = OBJ_VAL(obj_copy_string(buf ? buf : "", len));
        free(buf);
        ini_doc_free(&doc);
        return out;
    }

    /* Valeur d'une cle : repli si section/cle absente ou valeur vide. */
    IniPair* p = (s != NULL) ? ini_pair_find(s, key) : NULL;
    if (p == NULL || p->value[0] == '\0') {
        Value out = OBJ_VAL(obj_copy_string(notFound, notLen));
        ini_doc_free(&doc);
        return out;
    }
    Value out = OBJ_VAL(obj_copy_string(p->value, (int)strlen(p->value)));
    ini_doc_free(&doc);
    return out;
}

/* INIEcrit(<Section>, <Cle>, <Valeur>, <Fichier>) -> booleen (succes).
 *   cle=="" (ou NULL)    -> supprime la section entiere
 *   valeur=="" (ou NULL) -> supprime la cle
 *   sinon                -> cree/met a jour section + cle
 * Cree le fichier s'il est absent (repertoire existant). */
static Value fn_ini_ecrit(int argCount, Value* args) {
    (void)argCount;
    const char* section = arg_cstr(args[0]);
    int         secLen  = arg_len(args[0]);
    const char* key     = arg_cstr(args[1]);
    int         keyLen  = arg_len(args[1]);
    const char* value   = arg_cstr(args[2]);
    int         valLen  = arg_len(args[2]);
    const char* path    = arg_cstr(args[3]);

    if (section[0] == '\0') return BOOL_VAL(false);   /* section obligatoire */

    IniDoc doc;
    ini_doc_load(&doc, path);

    if (key[0] == '\0') {
        ini_section_remove(&doc, section);            /* suppression de section */
    } else {
        IniSection* s = ini_section_find(&doc, section);
        if (value[0] == '\0') {
            if (s != NULL) ini_pair_remove(s, key);   /* suppression de cle */
        } else {
            if (s == NULL) s = ini_section_add(&doc, section, secLen);
            if (s != NULL) ini_pair_set(s, key, keyLen, value, valLen);
        }
    }

    bool ok = ini_doc_write(&doc, path);
    ini_doc_free(&doc);
    return BOOL_VAL(ok);
}

/* ================================================================
 * REGISTRY
 * ================================================================ */

static const OwlParam p_ini_lit[] = {
    {"section", "cha\xc3\xae" "ne",
     "Nom de la section. Vide avec une cle vide, la liste des sections du "
     "fichier est renvoyee.", NULL, NULL},
    {"cl\xc3\xa9", "cha\xc3\xae" "ne",
     "Nom de la cle a lire. Vide, la liste des cles de la section est "
     "renvoyee.", NULL, NULL},
    {"nonTrouv\xc3\xa9", "cha\xc3\xae" "ne",
     "Valeur renvoyee quand la cle est absente, sa valeur vide, ou le fichier "
     "absent.", NULL, NULL},
    {"fichier", "cha\xc3\xae" "ne",
     "Chemin du fichier .ini a lire. Obligatoire.", NULL, NULL},
    {0}
};
static const OwlSignature sig_ini_lit[] = {
    {4, p_ini_lit, "cha\xc3\xae" "ne",
     "Valeur de la cle, liste des cles d'une section, ou liste des sections "
     "(elements separes par un retour a la ligne)."},
    {0}
};

static const OwlParam p_ini_ecrit[] = {
    {"section", "cha\xc3\xae" "ne",
     "Nom de la section, creee au besoin.", NULL, NULL},
    {"cl\xc3\xa9", "cha\xc3\xae" "ne",
     "Nom de la cle a ecrire. Vide, la section entiere est supprimee.",
     NULL, NULL},
    {"valeur", "cha\xc3\xae" "ne",
     "Valeur a ecrire. Vide, la cle est supprimee.", NULL, NULL},
    {"fichier", "cha\xc3\xae" "ne",
     "Chemin du fichier .ini a ecrire. Obligatoire ; cree s'il est absent et "
     "que son repertoire existe.", NULL, NULL},
    {0}
};
static const OwlSignature sig_ini_ecrit[] = {
    {4, p_ini_ecrit, "bool\xc3\xa9" "en",
     "Vrai si l'ecriture a reussi, Faux sinon (repertoire absent, ecriture "
     "impossible)."},
    {0}
};

static const OwlNativeDef DEFS_INI[] = {
    {
        .canonical  = "INILit",
        .nameEN     = "INIRead",
        .category   = "Fichiers",
        .descFR     = "Lit un fichier .ini section par section. Avec une section "
                      "et une cle, renvoie la valeur de la cle. Avec une section "
                      "seule (cle vide), renvoie la liste de ses cles. Sans "
                      "section ni cle, renvoie la liste des sections. La valeur "
                      "de repli est renvoyee lorsque la cle est absente, sa "
                      "valeur vide, ou le fichier introuvable. Le fichier cible "
                      "est toujours explicite.",
        .descEN     = "Reads an .ini file section by section. With a section and "
                      "a key, returns the key value. With a section only (empty "
                      "key), returns its list of keys. With neither section nor "
                      "key, returns the list of sections. The fallback value is "
                      "returned when the key is missing, its value empty, or the "
                      "file not found. The target file is always explicit.",
        .signatures = sig_ini_lit,
        .impl       = fn_ini_lit,
    },
    {
        .canonical  = "INIEcrit",
        .nameEN     = "INIWrite",
        .category   = "Fichiers",
        .descFR     = "Ecrit dans un fichier .ini section par section. Cree la "
                      "section et la cle au besoin. Une cle vide supprime la "
                      "section entiere ; une valeur vide supprime la cle. Le "
                      "fichier est cree s'il est absent et que son repertoire "
                      "existe. Renvoie Vrai en cas de succes. Le fichier cible "
                      "est toujours explicite.",
        .descEN     = "Writes into an .ini file section by section. Creates the "
                      "section and key as needed. An empty key deletes the whole "
                      "section; an empty value deletes the key. The file is "
                      "created if absent and its directory exists. Returns True "
                      "on success. The target file is always explicit.",
        .signatures = sig_ini_ecrit,
        .impl       = fn_ini_ecrit,
    },
};

void stdlib_ini_init(void) {
    owl_registry_register_module(DEFS_INI,
        (int)(sizeof(DEFS_INI) / sizeof(DEFS_INI[0])));
}
