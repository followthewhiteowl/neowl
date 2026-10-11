// SPDX-License-Identifier: AGPL-3.0-only
/* Garde bilingue des catalogues generes.
 *
 * CE QUE LE GARDE PROTEGE
 * -----------------------
 * Le registre respecte l'invariant bilingue n°3 (tout symbole a une forme FR
 * canon ET une forme EN canon) — `--self-test-registry` le verifie. Ce sont
 * les EXPORTS qui perdent la forme EN en chemin : trois fois en une journee,
 * un catalogue genere depuis le registre vers une surface consommatrice a
 * emis le FR sans l'EN.
 *
 *    : --vscode-data, champs composes  -> nameEN absent
 *    : catalogue LSP, OwlComposedTypeField -> pas de champ nameEN
 *    : --vscode-data, types du registre -> nameEN absent
 *
 * Symptome utilisateur invariable : « ca marche en francais, pas en anglais »,
 * decouvert par un anglophone, jamais par la CI. Trois corrections ponctuelles
 * n'ont rien empeche parce qu'aucune ne posait de controle : ce fichier pose
 * le controle.
 *
 * LA REGLE (conditionnelle — c'est ce qui evite les faux positifs)
 * ----------------------------------------------------------------
 * Si un catalogue exporte un symbole, ET que ce symbole possede au registre
 * une forme EN DISTINCTE de son canon FR, alors la forme EN doit figurer dans
 * l'export, a sa place, et etre exactement celle du registre.
 *
 * Ce qui n'est PAS exige :
 *   - qu'un symbole du registre soit exporte (chaque catalogue filtre
 *     legitimement : natives internes, types sans template runtime…). Le
 *     controle part donc de l'EXPORT et remonte au registre, jamais l'inverse.
 *   - qu'un symbole dont l'EN vaut le FR emette quoi que ce soit. « Distinct »
 *     s'entend au sens du runtime : insensible a la casse ET aux accents
 *     (`Libellé`/`Libelle` designent le meme symbole, pas une traduction).
 *     Sans cette normalisation, le garde exigerait l'emission d'alias qui
 *     n'apportent rien et accuserait du code correct.
 *
 * METHODE
 * -------
 * Les deux exports sont des fonctions in-process ecrivant sur stdout. On
 * capture leur sortie (dup/dup2, patron cli_ops_test.c), on la PARSE, et pour
 * chaque entree on relit le registre vivant. Parser plutot que chercher une
 * sous-chaine est deliberé : `strstr(buf, "multilingualString")` passerait au
 * vert des qu'une forme EN apparait N'IMPORTE OU dans le fichier — y compris
 * dans une autre section — ce qui rendrait le garde aveugle a la panne meme
 * qu'il doit attraper ( : l'EN etait present cote natives, absent cote
 * types). Chaque champ est donc lu a sa position dans son enregistrement.
 *
 * COUVERTURE (les deux exports, les quatre categories deja touchees)
 * -----------------------------------------------------------------
 *   --vscode-data      : builtin_types (nom + champs), natives, constantes,
 *                        dsl_properties
 *   catalogue LSP      : natives, constantes, types, champs de types, enums,
 *                        types composes (OwlComposedTypeField), props DSL
 *
 * VALIDATION PAR MUTATION (mesuree, pas deduite — cf. )
 * -------------------------------------------------------------
 * Quatre emissions de nameEN neutralisees une a une dans src/main.c, chacune
 * fait rougir SA section et elle seule :
 *   nameEN des types dans dump_lsp_catalog        ->  68 rouges (LSP/types)
 *   nameEN des champs dans print_vscode_data      -> 289 rouges (champs composes)
 *   nameEN des champs composes du dump LSP        -> 278 rouges (types composes)
 *   repli enum_values_en sur le FR                -> 2328 rouges (valeurs d'enum)
 * Restauration + rebuild -> 6753 confrontations, zero violation.
 *
 * COUVERTURE EXIGIBLE (EXPECTED[], en fin de fichier)
 * ---------------------------------------------------
 * Ce garde peut rouiller comme le reste. Sa premiere execution l'a prouve :
 * 1074 confrontations concentrees sur UNE section, inerte partout ailleurs (les
 * exports terminent par vm_free(), le registre relu ensuite etait vide) — et un
 * vert triomphal. D'ou la liste EXPECTED[] des sections que le controle DOIT
 * confronter : une section attendue restee a zero, ou jamais atteinte donc
 * absente du releve, est un echec au meme titre qu'une forme EN perdue, avec un
 * message distinct (l'un dit « une forme EN s'est perdue », l'autre « ce
 * controle a cesse de regarder »). Reduire la couverture exigee reste possible,
 * mais en editant EXPECTED[] — decision visible en revue, pas derive silencieuse.
 *
 * Mutation de validation du durcissement : emission de dsl_properties neutralisee
 * dans print_vscode_data -> section a zero, rouge, message « controle mort ».
 */

#include "common.h"
#include "owl_registry.h"
#include "scanner.h"
#include "ui_catalog.h"
#include "vm.h"
#include "fixture_unique.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <io.h>
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_unlink unlink
  #define owl_open   open
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* Les deux exports (src/main.c). Non-static pour ce garde. */
extern void print_vscode_data(void);
extern void dump_lsp_catalog(void);

static FILE* g_out       = NULL;
static int   g_violations = 0;   /* formes EN perdues par un export */
static int   g_dead       = 0;   /* controles morts (section qui ne confronte rien) */
static int   g_checked    = 0;   /* entrees confrontees au registre */

/* Compte par (catalogue, section). Un controle qui tombe a zero a cesse de
 * mordre — la section a ete renommee, deplacee ou vidée. Ce n'est pas un
 * defaut du meme genre qu'un nameEN manquant, mais c'est un echec : il est
 * compte a part (g_dead) et fait rougir au meme titre. */
#define MAX_SECTIONS 24
static struct { const char* catalog; const char* section; int count; }
       g_sections[MAX_SECTIONS];
static int g_section_count = 0;

static void tally(const char* catalog, const char* section) {
    for (int i = 0; i < g_section_count; i++) {
        if (strcmp(g_sections[i].catalog, catalog) == 0 &&
            strcmp(g_sections[i].section, section) == 0) {
            g_sections[i].count++;
            return;
        }
    }
    if (g_section_count < MAX_SECTIONS) {
        g_sections[g_section_count].catalog = catalog;
        g_sections[g_section_count].section = section;
        g_sections[g_section_count].count   = 1;
        g_section_count++;
    }
}

/* Diagnostic contextuel emis au fil de l'eau quand une section est introuvable
 * a la lecture. Volontairement NON comptant : l'autorite unique de comptage est
 * le bilan de couverture en fin de run (une section introuvable y apparaitra a
 * zero). Compter ici aussi dedoublerait le meme defaut. */
static void note_section_absent(const char* catalog, const char* section) {
    fprintf(g_out, "  [%s / %s] section introuvable dans l'export\n",
            catalog, section);
}

static void violation(const char* catalog, const char* section,
                      const char* symbol, const char* expectedEN,
                      const char* gotEN) {
    g_violations++;
    fprintf(g_out,
            "  [%s / %s] %s : forme EN attendue \"%s\", export porte %s%s%s\n",
            catalog, section, symbol, expectedEN,
            gotEN ? "\"" : "", gotEN ? gotEN : "AUCUNE", gotEN ? "\"" : "");
}

/* ================================================================
 * Comparaison « meme symbole ? » — casse + accents insensibles.
 * ================================================================ */

static void fold(const char* s, char* out, int outsz) {
    if (!s) { out[0] = '\0'; return; }
    scanner_strip_accents(s, (int)strlen(s), out, outsz);
}

static bool same_symbol(const char* a, const char* b) {
    if (!a || !b) return a == b;
    char fa[256], fb[256];
    fold(a, fa, (int)sizeof fa);
    fold(b, fb, (int)sizeof fb);
    return strcasecmp(fa, fb) == 0;
}

/* La forme EN de `en` est-elle une vraie traduction de `fr` (donc exigible
 * dans les exports), ou juste une variante d'ecriture du meme mot ? */
static bool en_is_distinct(const char* fr, const char* en) {
    if (!en || !*en || !fr) return false;
    return !same_symbol(fr, en);
}

/* Verifie une entree deja localisee : `gotEN` est ce que l'export porte a la
 * place prevue (NULL si le champ est vide / littéral 0).
 *
 * `fr` est la forme FR qui sert de reference a la comparaison (nom de champ nu,
 * nom de symbole) ; `label` est ce qu'on affiche en cas de violation et peut
 * etre qualifie (`httpRequête..Methode`). Les confondre rendrait tout champ
 * exigible : `httpRequête..Methode` n'est jamais egal a `Method`. */
static void expect_en_as(const char* catalog, const char* section,
                         const char* label, const char* fr,
                         const char* regEN, const char* gotEN) {
    if (!en_is_distinct(fr, regEN)) return;   /* hors-champ par construction */
    g_checked++;
    tally(catalog, section);
    if (gotEN && strcmp(gotEN, regEN) == 0) return;
    violation(catalog, section, label, regEN, gotEN);
}

static void expect_en(const char* catalog, const char* section,
                      const char* fr, const char* regEN, const char* gotEN) {
    expect_en_as(catalog, section, fr, fr, regEN, gotEN);
}

/* ================================================================
 * Capture stdout d'un export in-process.
 * ================================================================ */

static char* capture(void (*fn)(void), const char* tmpname) {
    fflush(stdout);
    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return NULL;
    int tmp_fd = owl_open(tmpname, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (tmp_fd < 0) { owl_close(saved_fd); return NULL; }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        /* Le fichier est deja cree : sans cet unlink, un nom porteur du PID
         * n'etant plus ecrase par l'execution suivante, chaque echec
         * laisserait un residu de plus dans le repertoire courant. */
        owl_close(tmp_fd); owl_close(saved_fd); owl_unlink(tmpname); return NULL;
    }
    owl_close(tmp_fd);

    fn();

    fflush(stdout);
    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    char* buf = NULL;
    FILE* r = fopen(tmpname, "rb");
    if (r) {
        fseek(r, 0, SEEK_END);
        long sz = ftell(r);
        fseek(r, 0, SEEK_SET);
        if (sz >= 0) {
            buf = (char*)malloc((size_t)sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, (size_t)sz, r);
                buf[n] = '\0';
            }
        }
        fclose(r);
    }
    owl_unlink(tmpname);
    return buf;
}

/* ================================================================
 * Lecture du catalogue LSP (source C : suites de `{champ, champ, …},`)
 * ================================================================ */

/* Deséchappe une chaine litterale C (emit_c_string n'emet que \" \\ \n \r \t
 * et \xNN — ces derniers n'apparaissent pas dans un nom de symbole). */
static void unescape_c(const char* p, const char* end, char* out, int outsz) {
    int o = 0;
    while (p < end && o < outsz - 1) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 'r': out[o++] = '\r'; break;
                case 't': out[o++] = '\t'; break;
                default:  out[o++] = *p;   break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
}

#define MAXFIELDS 16
#define FIELDLEN  512

/* Un enregistrement lu : jusqu'a MAXFIELDS champs. `isnull[i]` marque le
 * littéral C `0` (pointeur NULL cote LSP) — distinct d'une chaine vide. */
typedef struct {
    int  count;
    char v[MAXFIELDS][FIELDLEN];
    bool isnull[MAXFIELDS];
} CRecord;

/* Lit l'entree `{...}` commencant en *p (qui pointe sur `{`). Avance *p apres
 * l'accolade fermante. Retourne false si la forme est inattendue. */
static bool c_read_record(const char** p, const char* end, CRecord* rec) {
    const char* q = *p;
    if (q >= end || *q != '{') return false;
    q++;
    rec->count = 0;
    while (q < end) {
        while (q < end && (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) q++;
        if (q >= end) return false;
        if (*q == '}') { q++; *p = q; return true; }
        if (*q == ',') { q++; continue; }
        int slot = rec->count < MAXFIELDS ? rec->count : -1;
        if (*q == '"') {
            const char* s = ++q;
            while (q < end && *q != '"') { if (*q == '\\') q++; q++; }
            if (q >= end) return false;
            if (slot >= 0) {
                unescape_c(s, q, rec->v[slot], FIELDLEN);
                rec->isnull[slot] = false;
            }
            q++;
            /* Concatenation de litteraux adjacents ("a" "b") : ignoree, les
             * noms de symboles n'en produisent pas. */
        } else {
            const char* s = q;
            while (q < end && *q != ',' && *q != '}') q++;
            if (slot >= 0) {
                int len = (int)(q - s);
                if (len > FIELDLEN - 1) len = FIELDLEN - 1;
                memcpy(rec->v[slot], s, (size_t)len);
                rec->v[slot][len] = '\0';
                rec->isnull[slot] = (len == 1 && s[0] == '0');
            }
        }
        rec->count++;
    }
    return false;
}

static const char* c_field(const CRecord* r, int idx) {
    if (idx >= r->count || r->isnull[idx]) return NULL;
    return r->v[idx];
}

/* Itere les entrees d'un tableau C nomme `header` (texte exact precedant le
 * `{` ouvrant), et appelle `visit` pour chacune. */
typedef void (*CVisit)(const CRecord*, void* ctx);

static int c_iter_array(const char* buf, const char* header,
                        CVisit visit, void* ctx) {
    const char* h = strstr(buf, header);
    if (!h) return -1;
    const char* p = strchr(h + strlen(header) - 1, '{');
    if (!p) return -1;
    p++;                                  /* entre dans le tableau */
    const char* end = buf + strlen(buf);
    int n = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
        if (p >= end || *p != '{') break; /* `};` ou toute autre fin */
        CRecord rec;
        if (!c_read_record(&p, end, &rec)) break;
        visit(&rec, ctx);
        n++;
    }
    return n;
}

/* ================================================================
 * Lecture de --vscode-data (JSON emis par json_escape)
 * ================================================================ */

/* Extrait la valeur chaine de la cle `key` (forme "\"nom\":") cherchee entre
 * [from,to). Retourne false si la cle est absente. */
static bool json_str_after(const char* from, const char* to,
                           const char* key, char* out, int outsz) {
    size_t klen = strlen(key);
    for (const char* p = from; p + klen < to; p++) {
        if (memcmp(p, key, klen) != 0) continue;
        const char* q = p + klen;
        while (q < to && (*q == ' ')) q++;
        if (q >= to || *q != '"') return false;
        const char* s = ++q;
        while (q < to && *q != '"') { if (*q == '\\') q++; q++; }
        if (q > to) return false;
        unescape_c(s, q, out, outsz);   /* meme jeu d'echappements que le C */
        return true;
    }
    return false;
}

/* Delimite la section JSON introduite par `header` jusqu'a `terminator`. */
static bool json_section(const char* buf, const char* header,
                         const char* terminator,
                         const char** start, const char** end) {
    const char* s = strstr(buf, header);
    if (!s) return false;
    s += strlen(header);
    const char* e = strstr(s, terminator);
    if (!e) return false;
    *start = s; *end = e;
    return true;
}

/* ================================================================
 * Controles — catalogue LSP
 * ================================================================ */

static void visit_lsp_native(const CRecord* r, void* ctx) {
    (void)ctx;
    const char* fr = c_field(r, 0);
    if (!fr) return;
    const OwlNativeDef* d = owl_registry_find(fr);
    if (!d) return;                 /* rien a confronter */
    expect_en("catalogue LSP", "natives", fr, d->nameEN, c_field(r, 1));
}

static void visit_lsp_const(const CRecord* r, void* ctx) {
    (void)ctx;
    const char* fr = c_field(r, 0);
    if (!fr) return;
    const OwlConstDef* c = owl_registry_const_find(fr);
    if (!c) return;
    expect_en("catalogue LSP", "constantes", fr, c->nameEN, c_field(r, 1));
}

static void visit_lsp_type(const CRecord* r, void* ctx) {
    (void)ctx;
    const char* fr = c_field(r, 0);
    if (!fr) return;
    const OwlTypeDef* t = owl_registry_type_find(fr);
    if (!t) return;
    expect_en("catalogue LSP", "types", fr, t->nameEN, c_field(r, 2));
}

static void visit_lsp_enum(const CRecord* r, void* ctx) {
    (void)ctx;
    const char* fr = c_field(r, 0);
    if (!fr) return;
    const OwlEnum* e = owl_registry_enum_find(fr);
    if (!e) return;
    expect_en("catalogue LSP", "enums", fr, e->nameEN, c_field(r, 2));
}

/* Retrouve le champ registre d'un type par son nom FR (storage ASCII ou forme
 * accentuee), comme le font les exports. */
static const OwlField* field_of(const OwlTypeDef* t, const char* frName) {
    if (!t || !t->fields || !frName) return NULL;
    for (const OwlField* f = t->fields; f->name; f++) {
        if (strcasecmp(f->name, frName) == 0) return f;
        if (f->nameAccented && strcasecmp(f->nameAccented, frName) == 0) return f;
    }
    return NULL;
}

/* OwlComposedTypeField : {type, champ, champEN|0, …} — le trou de . */
static void visit_lsp_composed(const CRecord* r, void* ctx) {
    (void)ctx;
    const char* typeName = c_field(r, 0);
    const char* frField  = c_field(r, 1);
    if (!typeName || !frField) return;
    const OwlTypeDef* t = owl_registry_type_find(typeName);
    const OwlField*   f = field_of(t, frField);
    if (!f) return;                 /* champ purement runtime : pas de verite */
    char label[512];
    snprintf(label, sizeof label, "%s..%s", typeName, frField);
    expect_en_as("catalogue LSP", "types composes", label, frField,
                 f->nameEN, c_field(r, 2));
}

static void visit_lsp_dslprop(const CRecord* r, void* ctx) {
    (void)ctx;
    const char* fr = c_field(r, 1);
    if (!fr) return;
    /* Verite = l'entree DSL correspondante du catalogue UI (kind + nomFR). */
    int n = owl_dsl_props_count();
    for (int i = 0; i < n; i++) {
        const OwlDSLPropEntry* e = owl_dsl_prop_at(i);
        if (!e || !e->def || !e->def->nameFR) continue;
        if (strcmp(e->def->nameFR, fr) != 0) continue;
        const char* kind = c_field(r, 0);
        if (kind && e->kind && strcmp(kind, e->kind) != 0) continue;
        expect_en("catalogue LSP", "props DSL", fr, e->def->nameEN, c_field(r, 3));
        return;
    }
}

/* ================================================================
 * Controles — --vscode-data
 * ================================================================ */

#define MAX_ENUM_VALUES 512
static char g_vals[MAX_ENUM_VALUES][96];
static char g_valsEN[MAX_ENUM_VALUES][96];

/* Lit le tableau de chaines JSON qui suit `key` entre [from,to). Retourne le
 * nombre d'elements lus, -1 si la cle est absente, -2 s'il deborde. */
static int json_str_array(const char* from, const char* to, const char* key,
                          char dst[][96], int maxn) {
    size_t klen = strlen(key);
    const char* p = NULL;
    for (const char* q = from; q + klen < to; q++) {
        if (memcmp(q, key, klen) == 0) { p = q + klen; break; }
    }
    if (!p) return -1;
    while (p < to && *p == ' ') p++;
    if (p >= to || *p != '[') return -1;
    p++;
    int n = 0;
    while (p < to) {
        while (p < to && (*p == ' ' || *p == ',')) p++;
        if (p >= to || *p == ']') return n;
        if (*p != '"') return n;
        const char* s = ++p;
        while (p < to && *p != '"') { if (*p == '\\') p++; p++; }
        if (p >= to) return n;
        if (n >= maxn) return -2;
        unescape_c(s, p, dst[n], 96);
        n++;
        p++;
    }
    return n;
}

/* Valeurs d'enum d'un champ : "enum_values" (FR) et "enum_values_en" doivent
 * rester paralleles element par element. Meme classe de panne que les autres —
 * une liste EN qui se replie silencieusement sur le FR et le designer affiche
 * du francais a un utilisateur anglophone. */
static void check_vscode_enum_values(const char* from, const char* to,
                                     const char* label) {
    int nv  = json_str_array(from, to, "\"enum_values\":",    g_vals,   MAX_ENUM_VALUES);
    if (nv <= 0) return;                       /* pas d'enum sur ce champ */
    int nve = json_str_array(from, to, "\"enum_values_en\":", g_valsEN, MAX_ENUM_VALUES);
    if (nve != nv) {
        g_violations++;
        fprintf(g_out, "  [--vscode-data / valeurs d'enum] %s : %d valeurs FR "
                       "mais %d EN — les deux listes doivent rester paralleles\n",
                label, nv, nve);
        return;
    }
    for (int i = 0; i < nv; i++) {
        const OwlConstDef* c = owl_registry_const_find(g_vals[i]);
        if (!c) continue;
        expect_en_as("--vscode-data", "valeurs d'enum", g_vals[i], g_vals[i],
                     c->nameEN, g_valsEN[i]);
    }
}

/* builtin_types : un enregistrement par type, champs inlines dans "fields".
 * Couvre les deux trous  (nameEN du type) et  (nameEN des
 * champs composes). */
static void check_vscode_types(const char* buf) {
    const char *s, *e;
    if (!json_section(buf, "\"builtin_types\": [\n", "\n  ],", &s, &e)) {
        note_section_absent("--vscode-data", "builtin_types");
        return;
    }
    const char* p = s;
    while (p < e) {
        const char* rec = strstr(p, "    {\"name\":");
        if (!rec || rec >= e) break;
        const char* next = strstr(rec + 1, "\n    {\"name\":");
        const char* recEnd = (next && next < e) ? next : e;

        char typeFR[256];
        if (!json_str_after(rec, recEnd, "\"name\":", typeFR, sizeof typeFR)) break;
        const OwlTypeDef* t = owl_registry_type_find(typeFR);
        if (t) {
            char en[256];
            bool has = json_str_after(rec, recEnd, "\"nameEN\":", en, sizeof en);
            expect_en("--vscode-data", "builtin_types", typeFR, t->nameEN,
                      has ? en : NULL);

            /* Champs : chaque `{"name":` a l'interieur de "fields":[…]. */
            const char* fs = strstr(rec, "\"fields\":[");
            if (fs && fs < recEnd) {
                const char* fp = fs;
                while (fp < recEnd) {
                    const char* fr = strstr(fp, "{\"name\":");
                    if (!fr || fr >= recEnd) break;
                    const char* fnext = strstr(fr + 1, "{\"name\":");
                    const char* fEnd = (fnext && fnext < recEnd) ? fnext : recEnd;
                    char fieldFR[256];
                    if (json_str_after(fr, fEnd, "\"name\":", fieldFR, sizeof fieldFR)) {
                        const OwlField* f = field_of(t, fieldFR);
                        if (f) {
                            char fen[256];
                            bool fhas = json_str_after(fr, fEnd, "\"nameEN\":",
                                                       fen, sizeof fen);
                            char label[512];
                            snprintf(label, sizeof label, "%s..%s", typeFR, fieldFR);
                            expect_en_as("--vscode-data", "champs composes",
                                         label, fieldFR, f->nameEN,
                                         fhas ? fen : NULL);
                            check_vscode_enum_values(fr, fEnd, label);
                        }
                    }
                    fp = fEnd;
                }
            }
        }
        p = recEnd;
    }
}

static void check_vscode_natives(const char* buf) {
    const char *s, *e;
    if (!json_section(buf, "\"natives\": [\n", "\n  ],", &s, &e)) {
        note_section_absent("--vscode-data", "natives");
        return;
    }
    const char* p = s;
    while (p < e) {
        const char* rec = strstr(p, "{\"category\":");
        if (!rec || rec >= e) break;
        const char* next = strstr(rec + 1, "{\"category\":");
        const char* recEnd = (next && next < e) ? next : e;
        char fr[256], en[256];
        if (json_str_after(rec, recEnd, "\"nameFR\":", fr, sizeof fr)) {
            const OwlNativeDef* d = owl_registry_find(fr);
            if (d) {
                bool has = json_str_after(rec, recEnd, "\"nameEN\":", en, sizeof en);
                expect_en("--vscode-data", "natives", fr, d->nameEN,
                          has ? en : NULL);
            }
        }
        p = recEnd;
    }
}

/* constants_runtime est un tableau plat de chaines : la forme EN suit
 * immediatement la forme FR. Le controle porte donc sur cette adjacence
 * ("FR","EN"), pas sur une presence globale — un EN present ailleurs dans le
 * tableau (valeur d'enum homonyme) ne doit pas absoudre l'entree. */
static void check_vscode_consts(const char* buf) {
    const char *s, *e;
    if (!json_section(buf, "\"constants_runtime\": [", "],\n", &s, &e)) {
        note_section_absent("--vscode-data", "constants_runtime");
        return;
    }
    size_t seclen = (size_t)(e - s);
    char* sec = (char*)malloc(seclen + 1);
    if (!sec) return;
    memcpy(sec, s, seclen);
    sec[seclen] = '\0';

    int nc = owl_registry_consts_count();
    for (int i = 0; i < nc; i++) {
        const OwlConstDef* c = owl_registry_const_at(i);
        if (!c || !c->name) continue;
        if (!en_is_distinct(c->name, c->nameEN)) continue;
        char frTok[300], pairTok[600];
        snprintf(frTok, sizeof frTok, "\"%s\"", c->name);
        if (!strstr(sec, frTok)) continue;      /* non exporte : hors-champ */
        g_checked++;
        tally("--vscode-data", "constants_runtime");
        snprintf(pairTok, sizeof pairTok, "\"%s\",\"%s\"", c->name, c->nameEN);
        if (!strstr(sec, pairTok))
            violation("--vscode-data", "constants_runtime", c->name,
                      c->nameEN, NULL);
    }
    free(sec);
}

static void check_vscode_dslprops(const char* buf) {
    const char *s, *e;
    if (!json_section(buf, "\"dsl_properties\": [\n", "\n  ],", &s, &e)) {
        note_section_absent("--vscode-data", "dsl_properties");
        return;
    }
    const char* p = s;
    while (p < e) {
        const char* rec = strstr(p, "    {\"kind\":");
        if (!rec || rec >= e) break;
        const char* next = strstr(rec + 1, "\n    {\"kind\":");
        const char* recEnd = (next && next < e) ? next : e;
        char kind[128], fr[256], en[256];
        if (json_str_after(rec, recEnd, "\"kind\":", kind, sizeof kind) &&
            json_str_after(rec, recEnd, "\"name\":", fr, sizeof fr)) {
            int n = owl_dsl_props_count();
            for (int i = 0; i < n; i++) {
                const OwlDSLPropEntry* d = owl_dsl_prop_at(i);
                if (!d || !d->def || !d->def->nameFR) continue;
                if (strcmp(d->def->nameFR, fr) != 0) continue;
                if (d->kind && strcmp(d->kind, kind) != 0) continue;
                bool has = json_str_after(rec, recEnd, "\"nameEN\":", en, sizeof en);
                expect_en("--vscode-data", "dsl_properties", fr, d->def->nameEN,
                          (has && en[0]) ? en : NULL);
                break;
            }
        }
        p = recEnd;
    }
}

/* ================================================================ */

int catalog_bilingual_self_test_run(FILE* out) {
    g_out = out ? out : stdout;
    g_violations = 0;
    g_checked = 0;

    /* Noms propres au processus : a nom constant, deux executions
     * lancees du meme dossier se tronquaient mutuellement ces captures. La
     * fenetre est etroite (les deux captures sont au tout debut du process),
     * d'ou ~10 % de paires rouges seulement — mais l'observable ment a plein :
     * le dump relu tronque fait tomber 6 sections LSP en CONTROLE MORT, ce qui
     * accuse l'export d'avoir cesse d'emettre une categorie. */
    char lspTmp[128], vscTmp[128];
    fixture_unique(lspTmp, sizeof lspTmp, "_owl_catbi_lsp_", ".tmp");
    fixture_unique(vscTmp, sizeof vscTmp, "_owl_catbi_vsc_", ".tmp");

    char* lsp = capture(dump_lsp_catalog, lspTmp);
    char* vsc = capture(print_vscode_data, vscTmp);

    /* MESURE, pas deduction : les deux exports terminent par vm_free(), qui
     * vide le registre. Sans ce vm_init() de reprise, toutes les recherches
     * ci-dessous renvoient NULL et le garde ne confronte plus RIEN tout en
     * affichant un vert triomphal. C'est le compteur par section (plus bas)
     * qui a revele la panne — d'ou son maintien en dur. */
    vm_init();

    if (!lsp || !vsc) {
        fprintf(g_out, "  capture des exports impossible\n");
        free(lsp); free(vsc);
        return 1;
    }

    struct { const char* header; CVisit visit; const char* label; } LSP_SECTIONS[] = {
        {"const OwlNativeInfo OWL_NATIVE_CATALOG[] = {",        visit_lsp_native,   "natives"},
        {"const OwlConstInfo OWL_CONST_CATALOG[] = {",          visit_lsp_const,    "constantes"},
        {"const OwlTypeInfo OWL_TYPE_CATALOG[] = {",            visit_lsp_type,     "types"},
        {"const OwlEnumInfo OWL_ENUM_CATALOG[] = {",            visit_lsp_enum,     "enums"},
        {"const OwlComposedTypeField OWL_COMPOSED_TYPE_CATALOG[] = {",
                                                                visit_lsp_composed, "types composes"},
        {"const OwlDSLPropInfo OWL_DSL_PROP_CATALOG[] = {",     visit_lsp_dslprop,  "props DSL"},
    };
    for (size_t i = 0; i < sizeof LSP_SECTIONS / sizeof LSP_SECTIONS[0]; i++) {
        int n = c_iter_array(lsp, LSP_SECTIONS[i].header, LSP_SECTIONS[i].visit, NULL);
        if (n <= 0) note_section_absent("catalogue LSP", LSP_SECTIONS[i].label);
    }

    /* Champs des types du catalogue LSP : un tableau OWL_TYPE_FIELDS_<i> par
     * type, indexe sur la position registre — c'est ce qui permet de rattacher
     * chaque champ a son type sans heuristique de nom. */
    {
        const char* p = lsp;
        const char* marker = "static const OwlFieldInfo OWL_TYPE_FIELDS_";
        while ((p = strstr(p, marker)) != NULL) {
            int idx = atoi(p + strlen(marker));
            const OwlTypeDef* t = owl_registry_type_at(idx);
            const char* arr = strchr(p, '{');
            const char* end = strstr(p, "\n};");
            p += strlen(marker);
            if (!t || !arr || !end) continue;
            const char* q = arr + 1;
            while (q < end) {
                while (q < end && (*q == ' ' || *q == '\n' || *q == '\r' ||
                                   *q == '\t' || *q == ',')) q++;
                if (q >= end || *q != '{') break;
                CRecord rec;
                if (!c_read_record(&q, end, &rec)) break;
                const char* frField = c_field(&rec, 0);
                const OwlField* f = field_of(t, frField);
                if (f) {
                    char label[512];
                    snprintf(label, sizeof label, "%s..%s", t->canonical, frField);
                    expect_en_as("catalogue LSP", "champs de types", label,
                                 frField, f->nameEN, c_field(&rec, 6));
                }
            }
        }
    }

    check_vscode_types(vsc);
    check_vscode_natives(vsc);
    check_vscode_consts(vsc);
    check_vscode_dslprops(vsc);

    free(lsp);
    free(vsc);

    /* Couverture exigee. Cette liste est la SPECIFICATION de ce que le garde
     * doit regarder — pas un resume de ce qu'il a regarde. Toute section citee
     * ici doit avoir confronte au moins une entree ; a zero (ou jamais atteinte,
     * donc absente de g_sections), c'est un echec.
     *
     * Sans cette liste en dur, le bilan serait construit a partir des sections
     * effectivement visitees : une section qui cesse d'etre emise ou reconnue
     * disparaitrait simplement de l'affichage, et le garde resterait vert en
     * ayant cesse de regarder. C'est exactement la panne vecue au premier run
     * (11 sections sur 12 muettes, « 0 violation » affiche). */
    static const struct { const char* catalog; const char* section; }
    EXPECTED[] = {
        {"catalogue LSP", "natives"},
        {"catalogue LSP", "constantes"},
        {"catalogue LSP", "types"},
        {"catalogue LSP", "enums"},
        {"catalogue LSP", "types composes"},
        {"catalogue LSP", "props DSL"},
        {"catalogue LSP", "champs de types"},
        {"--vscode-data", "builtin_types"},
        {"--vscode-data", "champs composes"},
        {"--vscode-data", "natives"},
        {"--vscode-data", "constants_runtime"},
        {"--vscode-data", "dsl_properties"},
        {"--vscode-data", "valeurs d'enum"},
    };
    const size_t EXPECTED_N = sizeof EXPECTED / sizeof EXPECTED[0];
    fprintf(g_out, "  couverture par section (attendues : %d)\n", (int)EXPECTED_N);
    for (size_t i = 0; i < EXPECTED_N; i++) {
        int found = 0;
        for (int k = 0; k < g_section_count; k++) {
            if (strcmp(g_sections[k].catalog, EXPECTED[i].catalog) == 0 &&
                strcmp(g_sections[k].section, EXPECTED[i].section) == 0) {
                found = g_sections[k].count;
                break;
            }
        }
        fprintf(g_out, "  %-14s %-18s %5d%s\n",
                EXPECTED[i].catalog, EXPECTED[i].section, found,
                found == 0 ? "   <-- CONTROLE MORT" : "");
        if (found == 0) g_dead++;
    }

    /* Une section visitee hors liste n'est pas un echec (le garde regarde plus
     * que le minimum exige), mais elle signale que EXPECTED[] a pris du retard
     * sur le code : on le dit, sans faire rougir. */
    for (int k = 0; k < g_section_count; k++) {
        bool listed = false;
        for (size_t i = 0; i < EXPECTED_N && !listed; i++) {
            listed = strcmp(g_sections[k].catalog, EXPECTED[i].catalog) == 0 &&
                     strcmp(g_sections[k].section, EXPECTED[i].section) == 0;
        }
        if (!listed)
            fprintf(g_out, "  %-14s %-18s %5d   (hors liste attendue — a ajouter "
                           "a EXPECTED[])\n",
                    g_sections[k].catalog, g_sections[k].section,
                    g_sections[k].count);
    }

    fprintf(g_out, "catalog-bilingual: %d entrees confrontees au registre, "
                   "%d/%d sections vivantes, violation_count: %d\n",
            g_checked, (int)EXPECTED_N - g_dead, (int)EXPECTED_N,
            g_violations + g_dead);

    if (g_violations > 0) {
        fprintf(g_out,
            "\n%d forme(s) EN perdue(s) : un catalogue genere n'emet pas le "
            "nameEN d'un symbole\nqui en a un au registre. L'export doit emettre "
            "la forme EN a cote de la forme FR\n(invariant bilingue n°3).\n",
            g_violations);
    }
    if (g_dead > 0) {
        /* Defaut d'une autre nature que ci-dessus : rien n'est faux dans les
         * exports, c'est le CONTROLE qui a cesse de regarder. Le message doit
         * l'expliciter, sinon on cherche une forme EN manquante qui n'existe pas. */
        fprintf(g_out,
            "\n%d controle(s) mort(s) : une section attendue n'a confronte AUCUNE "
            "entree.\nCe n'est pas une forme EN perdue — c'est ce garde qui a "
            "cesse de regarder,\nsilencieusement, exactement le mode de "
            "defaillance qu'il doit empecher.\n"
            "Causes possibles : la section a ete renommee ou deplacee dans "
            "l'export, son\nformat d'emission a change (le parseur de ce fichier "
            "ne la reconnait plus),\nle catalogue ne l'emet plus, ou la categorie "
            "a disparu du registre.\n"
            "Si le retrait est LEGITIME (categorie retiree du langage), retirer "
            "l'entree\ncorrespondante de EXPECTED[] dans "
            "src/internal_tests/catalog_bilingual_test.c,\ndans le commit meme "
            "qui retire la categorie : reduire la couverture exigee est\nun acte "
            "conscient, pas un effet de bord.\n", g_dead);
    }
    return g_violations + g_dead;
}
