// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * stdlib_parametre.c — Valeurs persistantes (ParametreInitialise / ParametreEnregistre /
 * ParametreCharge / ParametreSupprime).
 *
 * Modele : une table PLATE nom -> valeur texte, adossee a un "support" de
 * stockage choisi par ParametreInitialise. Quatre supports (enum paramSupport) :
 *
 *   paramJSON   : fichier JSON  { "nom": "valeur", ... }   (defaut)
 *   paramXML    : fichier XML   <Parametres><Parametre nom= valeur=/></...>
 *   paramIni    : fichier .ini  [Global] / nom=valeur
 *   paramChaine : table en memoire, initialisee depuis une chaine JSON
 *
 * La table interne vit en memoire C (malloc), PAS en objets GC : elle survit
 * aux collectes et n'a pas a etre rootee. Les seuls objets OWL manipules sont
 * les valeurs d'entree/sortie des natives.
 *
 * Ecriture immediate (write-through) : chaque ParametreEnregistre / ParametreSupprime
 * reserialise le support complet. C'est le contrat de persistance attendu (une
 * execution ulterieure relit ce qui a ete pose) et c'est ce qui rend deux
 * sources reellement isolees : basculer d'une source a l'autre relit du disque.
 *
 * Emplacement par defaut (aucun ParametreInitialise appele) :
 *   Windows        %APPDATA%\<Projet>\<Projet>.json
 *   Linux / macOS  $HOME/.local/share/<Projet>/<Projet>.json
 * <Projet> = identifiant du bloc `est un Projet` de projet.owl, a defaut le nom
 * du fichier lance sans extension. La RESOLUTION de ce chemin ne cree ni
 * dossier ni fichier : l'arborescence n'apparait qu'a la premiere ecriture
 * effective. Un programme qui ne sauve rien ne laisse aucune trace.
 *
 * Noms de parametres : chemins hierarchiques dont les trois separateurs
 * `.` `/` `\` sont equivalents. Le stockage canonise vers `.`. La suppression
 * par element coupe sur une FRONTIERE de separateur, jamais au milieu d'un
 * segment (ParametreSupprime("FEN_Client") n'efface pas "FEN_ClientBis.Nom").
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_errors.h"
#include "sentinels.h"
#include "table.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#ifdef _WIN32
  #include <direct.h>
  #define param_mkdir_one(p)  _mkdir(p)
  #define PARAM_SEP_CHAR      '\\'
#else
  #include <sys/types.h>
  #include <unistd.h>
  #define param_mkdir_one(p)  mkdir((p), 0755)
  #define PARAM_SEP_CHAR      '/'
#endif

/* Parser JSON du module JSON (stdlib_json.c) : la lecture d'un support JSON
 * passe par lui, pas par un parseur reecrit ici. */
extern ObjJson* json_parse_cstr(const char* src, int len);
/* Identifiant du bloc `est un Projet` capture au compile (compiler.c). On le
 * declare a la main plutot que d'inclure compiler.h : ce dernier tire scanner.h
 * dont le TokenType collide avec celui de windows.h. */
extern const char* compiler_projet_ident(void);
/*  — fenetres declarees du projet capturees au compile (compiler.c),
 * pour la reflexion ElementsDe(enumFenetre). Declaration a la main (meme raison
 * que ci-dessus : ne pas tirer compiler.h). */
extern int         compiler_projet_window_count(void);
extern const char* compiler_projet_window_name(int i);

/* ================================================================
 * ETAT DU SUPPORT COURANT
 * ================================================================ */

typedef struct {
    char* name;   /* nom canonise (separateurs ramenes a '.') */
    char* value;  /* valeur texte */
} ParamEntry;

static int         g_mode    = OWL_PARAM_JSON;
static char*       g_source  = NULL;   /* chemin fichier ; NULL en paramChaine */
static ParamEntry* g_entries = NULL;
static int         g_count   = 0;
static int         g_cap     = 0;
/* Faux tant que le support par defaut n'a pas ete resolu ET charge. La
 * resolution est PARESSEUSE : c'est ce qui permet d'observer le chemin par
 * defaut (via le retour de ParametreInitialise) sans rien ecrire sur disque. */
static bool        g_ready   = false;

static void store_clear(void) {
    for (int i = 0; i < g_count; i++) {
        free(g_entries[i].name);
        free(g_entries[i].value);
    }
    g_count = 0;
}

static void store_free(void) {
    store_clear();
    free(g_entries);
    g_entries = NULL;
    g_cap = 0;
}

static char* dup_str(const char* s, int len) {
    char* out = (char*)malloc((size_t)len + 1);
    if (!out) return NULL;
    if (len > 0) memcpy(out, s, (size_t)len);
    out[len] = '\0';
    return out;
}

static int store_find(const char* name) {
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].name, name) == 0) return i;
    }
    return -1;
}

static void store_set(const char* name, const char* value) {
    int idx = store_find(name);
    if (idx >= 0) {
        char* copy = dup_str(value, (int)strlen(value));
        if (copy == NULL) return;          /* l'ancienne valeur reste en place */
        free(g_entries[idx].value);
        g_entries[idx].value = copy;
        return;
    }
    if (g_count == g_cap) {
        int newCap = g_cap < 8 ? 8 : g_cap * 2;
        ParamEntry* grown = (ParamEntry*)realloc(g_entries,
                                                 sizeof(ParamEntry) * (size_t)newCap);
        if (!grown) return;
        g_entries = grown;
        g_cap = newCap;
    }
    /* Les deux copies avant publication : une entree a moitie allouee ferait
     * dereferencer un nom NULL au prochain store_find. */
    char* n = dup_str(name, (int)strlen(name));
    char* v = dup_str(value, (int)strlen(value));
    if (n == NULL || v == NULL) { free(n); free(v); return; }
    g_entries[g_count].name  = n;
    g_entries[g_count].value = v;
    g_count++;
}

static void store_remove_at(int idx) {
    free(g_entries[idx].name);
    free(g_entries[idx].value);
    for (int i = idx; i < g_count - 1; i++) g_entries[i] = g_entries[i + 1];
    g_count--;
}

/* ================================================================
 * NOMS DE PARAMETRES
 * ================================================================ */

/* Canonise un nom : les trois separateurs de chemin sont ramenes a '.'.
 * Retourne le nombre d'octets ecrits, ou -1 si le nom deborde du buffer. */
static int normalize_name(const char* src, int len, char* out, int outSize) {
    if (len >= outSize) return -1;
    for (int i = 0; i < len; i++) {
        char c = src[i];
        out[i] = (c == '/' || c == '\\') ? '.' : c;
    }
    out[len] = '\0';
    return len;
}

/* Le nom `key` est-il sous l'element `prefix` (frontiere de separateur) ?
 * "FEN_Client" couvre "FEN_Client.Nom" mais PAS "FEN_ClientBis.Nom". Le nom
 * exactement egal au prefixe compte aussi : ParametreSupprime(<nom>) doit
 * effacer la cle nommee comme l'element homonyme. */
static bool name_under_element(const char* key, const char* prefix, int prefixLen) {
    if (strncmp(key, prefix, (size_t)prefixLen) != 0) return false;
    char next = key[prefixLen];
    return next == '\0' || next == '.';
}

/* ================================================================
 * BACKEND JSON — ecriture maison (echappement), lecture via stdlib_json.c
 * ================================================================ */

typedef struct { char* buf; int len; int cap; } TxtBuf;

static void tb_init(TxtBuf* b) {
    b->cap = 512;
    b->buf = (char*)malloc((size_t)b->cap);
    b->len = 0;
    if (b->buf) b->buf[0] = '\0';
}

/* La capacite n'est publiee qu'apres un realloc reussi : sur echec, le buffer
 * garde sa taille reelle et tb_append refuse d'ecrire, au lieu de deborder. */
static void tb_reserve(TxtBuf* b, int need) {
    if (!b->buf) return;
    int newCap = b->cap;
    while (b->len + need + 1 > newCap) newCap *= 2;
    if (newCap == b->cap) return;
    char* grown = (char*)realloc(b->buf, (size_t)newCap);
    if (!grown) return;
    b->buf = grown;
    b->cap = newCap;
}

static void tb_append(TxtBuf* b, const char* s, int len) {
    tb_reserve(b, len);
    if (!b->buf || b->len + len + 1 > b->cap) return;
    memcpy(b->buf + b->len, s, (size_t)len);
    b->len += len;
    b->buf[b->len] = '\0';
}

static void tb_append_cstr(TxtBuf* b, const char* s) {
    tb_append(b, s, (int)strlen(s));
}

/* Echappement JSON minimal RFC 8259 : guillemet, antislash, controles < 0x20.
 * Les octets >= 0x80 passent tels quels (le fichier est en UTF-8). */
static void tb_append_json_string(TxtBuf* b, const char* s) {
    tb_append_cstr(b, "\"");
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"':  tb_append_cstr(b, "\\\""); break;
            case '\\': tb_append_cstr(b, "\\\\"); break;
            case '\n': tb_append_cstr(b, "\\n");  break;
            case '\r': tb_append_cstr(b, "\\r");  break;
            case '\t': tb_append_cstr(b, "\\t");  break;
            case '\b': tb_append_cstr(b, "\\b");  break;
            case '\f': tb_append_cstr(b, "\\f");  break;
            default:
                if (*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*p);
                    tb_append_cstr(b, esc);
                } else {
                    tb_append(b, (const char*)p, 1);
                }
        }
    }
    tb_append_cstr(b, "\"");
}

/* Serialise la table courante en JSON plat. Le caller free() le resultat. */
static char* store_to_json(void) {
    TxtBuf b;
    tb_init(&b);
    tb_append_cstr(&b, "{");
    for (int i = 0; i < g_count; i++) {
        if (i > 0) tb_append_cstr(&b, ",");
        tb_append_json_string(&b, g_entries[i].name);
        tb_append_cstr(&b, ":");
        tb_append_json_string(&b, g_entries[i].value);
    }
    tb_append_cstr(&b, "}");
    return b.buf;
}

/* Rend un noeud JSON NON-CHAINE sous forme texte dans `out`. Retourne false
 * pour tout le reste (objet, tableau, monetaire, numerique arbitraire) :
 * l'entree est alors ignoree au chargement. Aucun de ces types n'est produit
 * par l'ecriture — la table est plate et textuelle — donc l'aller-retour d'un
 * fichier ecrit par OWL est integral ; seul un fichier edite a la main peut
 * porter une valeur ecartee.
 * Les chaines sont traitees a part par l'appelant, sans recopie ni plafond :
 * elles peuvent etre arbitrairement longues et ce que ParametreEnregistre a ecrit
 * doit revenir entier. */
static bool json_scalar_to_text(ObjJson* node, char* out, int outSize) {
    switch (node->jtype) {
        case JSON_T_INT:
            snprintf(out, (size_t)outSize, "%lld", (long long)node->as.intVal);
            return true;
        case JSON_T_REAL:
            snprintf(out, (size_t)outSize, "%g", node->as.realVal);
            return true;
        case JSON_T_BOOL:
            snprintf(out, (size_t)outSize, "%s",
                     node->as.boolVal ? "Vrai" : "Faux");
            return true;
        case JSON_T_NULL:
            out[0] = '\0';
            return true;
        default:
            return false;
    }
}

/* Charge une table depuis un texte JSON. Table vide si le texte est illisible
 * (support absent / corrompu) : la lecture d'un parametre retombe alors sur sa
 * valeur par defaut, ce qui est le comportement attendu cote appelant. */
static void store_load_json(const char* text, int len) {
    store_clear();
    if (text == NULL || len <= 0) return;
    ObjJson* root = json_parse_cstr(text, len);
    if (root == NULL) return;
    /* Root rootee le temps du parcours : rien n'alloue en dessous (on copie
     * vers de la memoire C), mais la discipline  veut le push. */
    vm_push(OBJ_VAL(root));
    if (root->jtype == JSON_T_OBJECT) {
        for (int i = 0; i < root->as.objectVal.count; i++) {
            ObjString* key = root->as.objectVal.keys[i];
            ObjJson* val   = root->as.objectVal.values[i];
            if (key == NULL || val == NULL) continue;
            char name[1024];
            if (normalize_name(key->chars, key->length, name, sizeof name) < 0) continue;
            if (val->jtype == JSON_T_STRING) {
                ObjString* s = val->as.strVal;
                store_set(name, s != NULL ? s->chars : "");
                continue;
            }
            char text_val[64];
            if (!json_scalar_to_text(val, text_val, (int)sizeof text_val)) continue;
            store_set(name, text_val);
        }
    }
    vm_pop();
}

/* ================================================================
 * BACKEND XML — libxml2 (aucun parseur maison)
 *
 * Forme : <Parametres><Parametre nom="..." valeur="..."/>...</Parametres>.
 * Les noms de parametres sont des chemins (`FEN_Client.Nom`) qui ne sont pas
 * des noms d'element XML valides : ils vivent donc en ATTRIBUT, pas en balise.
 * ================================================================ */

static char* store_to_xml(void) {
    xmlDocPtr doc = xmlNewDoc(BAD_CAST "1.0");
    if (doc == NULL) return NULL;
    xmlNodePtr root = xmlNewNode(NULL, BAD_CAST "Parametres");
    xmlDocSetRootElement(doc, root);
    for (int i = 0; i < g_count; i++) {
        xmlNodePtr node = xmlNewChild(root, NULL, BAD_CAST "Parametre", NULL);
        xmlNewProp(node, BAD_CAST "nom",    BAD_CAST g_entries[i].name);
        xmlNewProp(node, BAD_CAST "valeur", BAD_CAST g_entries[i].value);
    }
    xmlChar* out = NULL;
    int outLen = 0;
    xmlDocDumpFormatMemoryEnc(doc, &out, &outLen, "UTF-8", 1);
    xmlFreeDoc(doc);
    if (out == NULL) return NULL;
    char* copy = dup_str((const char*)out, outLen);
    xmlFree(out);
    return copy;
}

static void store_load_xml(const char* text, int len) {
    store_clear();
    if (text == NULL || len <= 0) return;
    xmlDocPtr doc = xmlReadMemory(text, len, "parametres.xml", NULL,
                                  XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (doc == NULL) return;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr n = root ? root->children : NULL; n != NULL; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;
        xmlChar* nom = xmlGetProp(n, BAD_CAST "nom");
        if (nom == NULL) continue;
        xmlChar* val = xmlGetProp(n, BAD_CAST "valeur");
        char name[1024];
        if (normalize_name((const char*)nom, (int)strlen((const char*)nom),
                           name, sizeof name) >= 0) {
            store_set(name, val ? (const char*)val : "");
        }
        if (val) xmlFree(val);
        xmlFree(nom);
    }
    xmlFreeDoc(doc);
}

/* ================================================================
 * BACKEND INI — le seul format sans moteur prealable dans le runtime.
 *
 * Volontairement minimal : une seule section implicite `[Global]`, une ligne
 * `nom=valeur` par parametre. Limites assumees, non couvertes par ce backend :
 *   - aucune sequence d'echappement : une valeur contenant un retour a la ligne
 *     ne survit pas a l'aller-retour (la ligne suivante est relue comme une
 *     entree distincte, ou perdue) ;
 *   - un nom contenant '=' est illisible en relecture (coupure au 1er '=') ;
 *   - les commentaires (`;` `#`) et les sections supplementaires d'un fichier
 *     externe sont lus mais PAS conserves a la reecriture ;
 *   - pas de typage : tout est texte, comme pour les autres supports.
 * Pour un stockage sans ces limites, prendre paramJSON ou paramXML.
 * ================================================================ */

static char* store_to_ini(void) {
    TxtBuf b;
    tb_init(&b);
    tb_append_cstr(&b, "[Global]\n");
    for (int i = 0; i < g_count; i++) {
        tb_append_cstr(&b, g_entries[i].name);
        tb_append_cstr(&b, "=");
        tb_append_cstr(&b, g_entries[i].value);
        tb_append_cstr(&b, "\n");
    }
    return b.buf;
}

static void store_load_ini(const char* text, int len) {
    store_clear();
    if (text == NULL || len <= 0) return;
    int i = 0;
    while (i < len) {
        int start = i;
        while (i < len && text[i] != '\n') i++;
        int end = i;
        if (i < len) i++;                       /* saute le '\n' */
        if (end > start && text[end - 1] == '\r') end--;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        if (start >= end) continue;
        if (text[start] == ';' || text[start] == '#' || text[start] == '[') continue;
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
        char name[1024];
        if (normalize_name(text + start, nameEnd - start, name, sizeof name) < 0) continue;
        int valStart = eq + 1;
        char* value = dup_str(text + valStart, end - valStart);
        if (value == NULL) continue;
        store_set(name, value);
        free(value);
    }
}

/* ================================================================
 * SERIALISATION / IO FICHIER
 * ================================================================ */

static const char* mode_token(int mode) {
    switch (mode) {
        case OWL_PARAM_XML:    return "xml";
        case OWL_PARAM_INI:    return "ini";
        case OWL_PARAM_CHAINE: return "string";
        default:               return "json";
    }
}

static const char* mode_extension(int mode) {
    switch (mode) {
        case OWL_PARAM_XML: return ".xml";
        case OWL_PARAM_INI: return ".ini";
        default:            return ".json";   /* paramChaine serialise en JSON */
    }
}

/* Serialise la table courante selon le support actif. Caller free(). */
static char* store_serialize(void) {
    switch (g_mode) {
        case OWL_PARAM_XML: return store_to_xml();
        case OWL_PARAM_INI: return store_to_ini();
        default:            return store_to_json();
    }
}

static void store_deserialize(const char* text, int len) {
    switch (g_mode) {
        case OWL_PARAM_XML: store_load_xml(text, len); break;
        case OWL_PARAM_INI: store_load_ini(text, len); break;
        default:            store_load_json(text, len); break;
    }
}

/* Cree l'arborescence menant a `path` (le dernier segment est le fichier).
 * Appele UNIQUEMENT depuis l'ecriture : la resolution du chemin par defaut ne
 * doit rien materialiser. */
static void ensure_parent_dirs(const char* path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n >= sizeof buf) return;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/' && buf[i] != '\\') continue;
        char saved = buf[i];
        buf[i] = '\0';
        struct stat st;
        if (stat(buf, &st) != 0) param_mkdir_one(buf);
        buf[i] = saved;
    }
}

static char* read_whole_file(const char* path, int* outLen) {
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

/* Ecrit la table sur le support actif. No-op en paramChaine (rien sur disque :
 * le contenu accumule se recupere par le retour du prochain ParametreInitialise). */
static void store_flush(void) {
    if (g_mode == OWL_PARAM_CHAINE || g_source == NULL) return;
    char* text = store_serialize();
    if (text == NULL) return;
    ensure_parent_dirs(g_source);
    FILE* f = fopen(g_source, "wb");
    if (f != NULL) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
    free(text);
}

/* Charge la table depuis le support actif. Fichier absent = table vide, sans
 * creation : ne rien avoir enregistre n'est pas une erreur. */
static void store_reload(void) {
    store_clear();
    if (g_mode == OWL_PARAM_CHAINE || g_source == NULL) return;
    int len = 0;
    char* text = read_whole_file(g_source, &len);
    if (text == NULL) return;
    store_deserialize(text, len);
    free(text);
}

/* ================================================================
 * EMPLACEMENT PAR DEFAUT
 * ================================================================ */

/* Le bloc Projet capture au compile gouverne-t-il l'execution EN COURS ?
 *
 * `compiler_projet_ident()` est une capture compile-time qui survit au-dela du
 * fichier qui l'a posee : dans une passe qui compile plusieurs frontieres
 * (owl test sur un arbre, LSP), l'identifiant du dernier projet.owl rencontre
 * reste en place et contaminerait le chemin par defaut d'un programme qui n'a
 * rien a voir avec lui. Le signal fiable est la MATERIALISATION du global de
 * metadonnees, que seul un lancement projet effectue et que vm_init remet a
 * neuf : on exige que l'identifiant designe une instance Projet vivante dans
 * vm.globals. Lookup sans allocation (le caller peut tenir une ObjString
 * fraiche non rootee, cf ). */
static bool projet_ident_is_live(const char* ident) {
    ObjString* key = table_find_key_case_insensitive(&vm.globals, ident);
    if (key == NULL) return false;
    Value v;
    if (!table_get(&vm.globals, key, &v)) return false;
    if (!IS_INSTANCE(v)) return false;
    ObjString* sname = AS_INSTANCE(v)->structName;
    return sname != NULL && strcmp(sname->chars, "Projet") == 0;
}

/* Nom du programme : identifiant du bloc Projet quand un projet gouverne le
 * lancement, sinon nom du fichier lance sans extension (frame la plus ancienne
 * de la pile d'appels), sinon "owl". Partage avec stdlib_file.c (segment <app>
 * de fRepDonnees*). */
void owl_program_name(char* out, int outSize) {
    const char* ident = compiler_projet_ident();
    if (ident != NULL && ident[0] != '\0' && projet_ident_is_live(ident)) {
        snprintf(out, (size_t)outSize, "%s", ident);
        return;
    }
    const char* path = NULL;
    if (vm.frameCount > 0) {
        ObjFunction* fn = vm.frames[0].function;
        if (fn != NULL && fn->filename != NULL) path = fn->filename->chars;
    }
    if (path == NULL || path[0] == '\0') {
        snprintf(out, (size_t)outSize, "owl");
        return;
    }
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    int len = (int)strlen(base);
    const char* dot = strrchr(base, '.');
    if (dot != NULL && dot != base) len = (int)(dot - base);
    if (len <= 0 || len >= outSize) {
        snprintf(out, (size_t)outSize, "owl");
        return;
    }
    memcpy(out, base, (size_t)len);
    out[len] = '\0';
}

/* ProjetInfo([typeInfo]) -> chaine. Aujourd'hui une seule information : le nom
 * du projet (identifiant du bloc `est un Projet` de la frontiere courante).
 * Le nom n'est expose que si un projet gouverne REELLEMENT le lancement en
 * cours (instance Projet vivante dans vm.globals) : en mode script, ou quand la
 * capture compile-time est un residu d'une autre frontiere, on renvoie "". */
static Value fn_projet_info(int argCount, Value* args) {
    int kind = OWL_PROJETINFO_NOM;
    if (argCount >= 1 && IS_INTEGER(args[0])) kind = (int)AS_INTEGER(args[0]);

    switch (kind) {
        case OWL_PROJETINFO_NOM: {
            const char* ident = compiler_projet_ident();
            if (ident != NULL && ident[0] != '\0' && projet_ident_is_live(ident)) {
                return OBJ_VAL(obj_copy_string(ident, (int)strlen(ident)));
            }
            return OBJ_VAL(obj_copy_string("", 0));
        }
        default:
            return OBJ_VAL(obj_copy_string("", 0));
    }
}

/* ElementsDe(cible) -> tableau de chaines. Reflexion sur les elements declares
 * du projet. Aujourd'hui une seule cible : enumFenetre, qui renvoie les noms de
 * TOUTES les fenetres declarees du projet (ouvertes ou non). Hors projet (mode
 * script), la liste captee au compile est vide -> tableau vide, sans erreur. */
static Value fn_elements_de(int argCount, Value* args) {
    int cible = OWL_ELEMENTPROJET_FENETRE;
    if (argCount >= 1 && IS_INTEGER(args[0])) cible = (int)AS_INTEGER(args[0]);

    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));  /* GC root pendant le remplissage */
    if (cible == OWL_ELEMENTPROJET_FENETRE) {
        int n = compiler_projet_window_count();
        for (int i = 0; i < n; i++) {
            const char* name = compiler_projet_window_name(i);
            if (name == NULL) continue;
            obj_array_push(arr, OBJ_VAL(obj_copy_string(name, (int)strlen(name))));
        }
    }
    vm_pop();
    return OBJ_VAL(arr);
}

/* Dossier de donnees utilisateur, sans separateur final. */
static void user_data_dir(char* out, int outSize) {
    out[0] = '\0';
#ifdef _WIN32
    const char* appdata = getenv("APPDATA");
    if (appdata != NULL && appdata[0] != '\0') {
        snprintf(out, (size_t)outSize, "%s", appdata);
        return;
    }
    const char* profile = getenv("USERPROFILE");
    if (profile != NULL && profile[0] != '\0') {
        snprintf(out, (size_t)outSize, "%s\\AppData\\Roaming", profile);
        return;
    }
#else
    const char* home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        snprintf(out, (size_t)outSize, "%s/.local/share", home);
        return;
    }
#endif
    /* Dernier recours : le dossier temporaire du systeme. Cas de figure d'un
     * environnement sans variable de profil (service, conteneur nu). */
    const char* tmp = getenv("TEMP");
    if (tmp == NULL) tmp = getenv("TMP");
    if (tmp == NULL) tmp = getenv("TMPDIR");
    if (tmp == NULL) tmp = "/tmp";
    snprintf(out, (size_t)outSize, "%s", tmp);
}

/* Chemin par defaut pour le support `mode`. AUCUN effet de bord disque. */
static void default_source_path(int mode, char* out, int outSize) {
    char dir[768];
    char name[256];
    user_data_dir(dir, (int)sizeof dir);
    owl_program_name(name, (int)sizeof name);
    /* Un nom de projet ne doit pas pouvoir sortir du dossier de donnees. */
    for (char* p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') *p = '_';
    }
    snprintf(out, (size_t)outSize, "%s%c%s%c%s%s",
             dir, PARAM_SEP_CHAR, name, PARAM_SEP_CHAR, name,
             mode_extension(mode));
}

/* Resolution paresseuse du support par defaut. Appelee par toutes les natives
 * de la famille AVANT de lire l'etat courant : le premier appel a
 * ParametreInitialise renvoie donc bien l'etat par defaut deja resolu. */
static void param_ensure_ready(void) {
    if (g_ready) return;
    g_ready = true;
    char path[1024];
    default_source_path(g_mode, path, (int)sizeof path);
    free(g_source);
    g_source = dup_str(path, (int)strlen(path));
    store_reload();
}

/* ================================================================
 * NATIVES
 * ================================================================ */

/* Etat courant sous la forme `<jeton de mode>` TAB `<source>`.
 *   - supports fichier : <source> est le chemin, tel qu'il a ete fourni ;
 *   - paramChaine      : <source> est le contenu serialise courant, seul canal
 *                        de recuperation d'un support qui ne touche pas au
 *                        disque.
 * Le jeton de mode est un ASCII neutre (json/xml/ini/string), pas un nom de
 * constante : un programme ecrit en anglais et son equivalent francais doivent
 * observer la meme valeur. */
static Value current_state_value(void) {
    const char* token = mode_token(g_mode);
    char* source = NULL;
    const char* src = "";
    if (g_mode == OWL_PARAM_CHAINE) {
        source = store_to_json();
        if (source != NULL) src = source;
    } else if (g_source != NULL) {
        src = g_source;
    }
    int tokenLen = (int)strlen(token);
    int srcLen   = (int)strlen(src);
    char* buf = (char*)malloc((size_t)tokenLen + 1 + (size_t)srcLen + 1);
    if (buf == NULL) {
        free(source);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    memcpy(buf, token, (size_t)tokenLen);
    buf[tokenLen] = '\t';
    memcpy(buf + tokenLen + 1, src, (size_t)srcLen);
    int total = tokenLen + 1 + srcLen;
    buf[total] = '\0';
    Value out = OBJ_VAL(obj_copy_string(buf, total));
    free(buf);
    free(source);
    return out;
}

/* ParametreInitialise(<Support> [, <Source>]) : bascule le support de stockage et
 * renvoie l'etat ANTERIEUR a l'appel. */
static Value fn_init_parametre(int argCount, Value* args) {
    param_ensure_ready();
    /*  : `previous` est une ObjString fraiche, non rootee sur la
     * C-stack. Le chargement du nouveau support (json_parse_cstr) alloue et
     * peut declencher une collecte — sans ce push, l'etat anterieur revient
     * empoisonne. Un seul point de sortie pour garantir le pop. */
    Value previous = current_state_value();
    vm_push(previous);

    int mode = (argCount >= 1 && IS_INTEGER(args[0])) ? (int)AS_INTEGER(args[0])
                                                      : OWL_PARAM_JSON - 1;
    if (mode != OWL_PARAM_JSON && mode != OWL_PARAM_XML &&
        mode != OWL_PARAM_INI  && mode != OWL_PARAM_CHAINE) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Param\xc3\xa8" "treInitialise",
                              "native", "Param\xc3\xa8" "treInitialise",
                              "arg", "support",
                              "expected", "paramJSON, paramXML, paramIni ou "
                                          "paramCha\xc3\xae" "ne",
                              "actual", "une valeur qui ne designe aucun support",
                              NULL);
        return vm_pop();
    }

    const char* source = NULL;
    int sourceLen = 0;
    if (argCount >= 2 && IS_STRING(args[1])) {
        source = AS_CSTRING(args[1]);
        sourceLen = AS_STRING(args[1])->length;
    }
    /* `source` pointe dans args[1], rootee par la stack d'appel : pas de copie
     * defensive necessaire tant qu'on ne l'utilise que sous cette frame. */

    if (mode == OWL_PARAM_CHAINE) {
        /* Le support memoire est initialise DEPUIS la chaine fournie : elle en
         * est le contenu de depart, pas une adresse. Absente, on part vide. */
        g_mode = OWL_PARAM_CHAINE;
        free(g_source);
        g_source = NULL;
        store_clear();
        if (source != NULL && sourceLen > 0) store_load_json(source, sourceLen);
    } else {
        g_mode = mode;
        free(g_source);
        if (source != NULL) {
            g_source = dup_str(source, sourceLen);
        } else {
            char path[1024];
            default_source_path(mode, path, (int)sizeof path);
            g_source = dup_str(path, (int)strlen(path));
        }
        store_reload();
    }
    g_ready = true;
    owl_clear_error();
    return vm_pop();
}

/* Refuse un nom vide et pose l'erreur soft correspondante. */
static bool reject_empty_name(const char* native, Value v,
                              char* out, int outSize) {
    const char* raw = IS_STRING(v) ? AS_CSTRING(v) : NULL;
    int len = IS_STRING(v) ? AS_STRING(v)->length : 0;
    if (raw == NULL || len == 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, native,
                              "native", native,
                              "arg", "nom",
                              "expected", "un nom de param\xc3\xa8" "tre non vide",
                              "actual", raw == NULL ? "une valeur qui n'est pas un nom"
                                                    : "une cha\xc3\xae" "ne vide",
                              NULL);
        return true;
    }
    if (normalize_name(raw, len, out, outSize) < 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, native,
                              "native", native,
                              "arg", "nom",
                              "expected", "un nom de param\xc3\xa8" "tre plus court",
                              "actual", "un nom trop long",
                              NULL);
        return true;
    }
    return false;
}

/* ParametreEnregistre(<Nom>, <Valeur>) : enregistre et persiste immediatement. */
static Value fn_sauve_parametre(int argCount, Value* args) {
    (void)argCount;
    param_ensure_ready();

    char name[1024];
    if (reject_empty_name("Param\xc3\xa8" "treEnregistre", args[0],
                          name, (int)sizeof name)) {
        return NIL_VAL;
    }

    char intBuf[32];
    const char* value;
    if (IS_STRING(args[1])) {
        value = AS_CSTRING(args[1]);
    } else if (IS_INTEGER(args[1])) {
        snprintf(intBuf, sizeof intBuf, "%lld", (long long)AS_INTEGER(args[1]));
        value = intBuf;
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Param\xc3\xa8" "treEnregistre",
                              "native", "Param\xc3\xa8" "treEnregistre",
                              "arg", "valeur",
                              "expected", "une cha\xc3\xae" "ne ou un entier",
                              "actual", "un type non pris en charge",
                              NULL);
        return NIL_VAL;
    }

    store_set(name, value);
    store_flush();
    owl_clear_error();
    return NIL_VAL;
}

/* ParametreCharge(<Nom> [, <Defaut>]) : toujours une chaine. La conversion
 * numerique reste a la charge de l'appelant (Val). */
static Value fn_charge_parametre(int argCount, Value* args) {
    param_ensure_ready();

    const char* fallback = "";
    if (argCount >= 2 && IS_STRING(args[1])) fallback = AS_CSTRING(args[1]);

    char name[1024];
    if (reject_empty_name("Param\xc3\xa8" "treCharge", args[0],
                          name, (int)sizeof name)) {
        return OBJ_VAL(obj_copy_string(fallback, (int)strlen(fallback)));
    }

    int idx = store_find(name);
    owl_clear_error();
    if (idx < 0) {
        return OBJ_VAL(obj_copy_string(fallback, (int)strlen(fallback)));
    }
    const char* v = g_entries[idx].value;
    return OBJ_VAL(obj_copy_string(v, (int)strlen(v)));
}

/* ParametreSupprime([<Nom>]) : sans argument, efface la totalite du support
 * actif ; avec un nom, efface la cle nommee ET tous les parametres de
 * l'element homonyme (coupure sur frontiere de separateur). */
static Value fn_supprime_parametre(int argCount, Value* args) {
    param_ensure_ready();

    if (argCount == 0) {
        store_clear();
        store_flush();
        owl_clear_error();
        return NIL_VAL;
    }

    char name[1024];
    if (reject_empty_name("Param\xc3\xa8" "treSupprime", args[0],
                          name, (int)sizeof name)) {
        return NIL_VAL;
    }

    int nameLen = (int)strlen(name);
    for (int i = g_count - 1; i >= 0; i--) {
        if (name_under_element(g_entries[i].name, name, nameLen)) {
            store_remove_at(i);
        }
    }
    store_flush();
    owl_clear_error();
    return NIL_VAL;
}

/* ================================================================
 * REGISTRY
 * ================================================================ */

static const char* const E_paramSupport_values[] = {
    "paramJSON", "paramXML", "paramIni",
    "paramCha\xc3\xae" "ne",
    NULL
};

static const OwlEnum E_paramSupport = {
    .name      = "paramSupport",
    .nameAscii = "paramSupport",
    .nameEN    = "paramStorage",
    .category  = "Syst\xc3\xa8me",
    .descFR    = "Supports de stockage des valeurs persistantes : fichier JSON "
                 "(defaut), fichier XML, fichier ini, ou table en memoire "
                 "initialisee depuis une chaine.",
    .descEN    = "Storage backends for persistent values: JSON file (default), "
                 "XML file, ini file, or in-memory table initialized from a "
                 "string.",
    .values    = E_paramSupport_values,
    .combinable = false,
};

static const OwlConstDef PARAM_CONSTS[] = {
    {"paramJSON", "Syst\xc3\xa8me", INTEGER_VAL(OWL_PARAM_JSON),
     "Stockage dans un fichier JSON (support par defaut).",
     "Storage in a JSON file (default backend).",
     "paramJSON"},
    {"paramXML", "Syst\xc3\xa8me", INTEGER_VAL(OWL_PARAM_XML),
     "Stockage dans un fichier XML.",
     "Storage in an XML file.",
     "paramXML"},
    {"paramIni", "Syst\xc3\xa8me", INTEGER_VAL(OWL_PARAM_INI),
     "Stockage dans un fichier ini (section unique [Global], lignes nom=valeur).",
     "Storage in an ini file (single [Global] section, name=value lines).",
     "paramIni"},
    {"paramCha\xc3\xae" "ne", "Syst\xc3\xa8me", INTEGER_VAL(OWL_PARAM_CHAINE),
     "Stockage en memoire, initialise depuis la chaine fournie a "
     "Param\xc3\xa8" "treInitialise. Rien n'est ecrit sur disque ; le contenu "
     "accumule se recupere par la valeur de retour de l'appel "
     "Param\xc3\xa8" "treInitialise suivant.",
     "In-memory storage, initialized from the string given to ParameterInitialize. "
     "Nothing is written to disk; the accumulated content is retrieved through "
     "the return value of the next ParameterInitialize call.",
     "paramString"},
};

/* pi* — type d'information demande a ProjetInfo. Enum extensible : une seule
 * valeur aujourd'hui (le nom du projet). */
static const char* const E_projetInfo_values[] = {
    "piNomProjet",
    NULL
};

static const OwlEnum E_projetInfo = {
    .name      = "projetInfo",
    .nameAscii = "projetInfo",
    .nameEN    = "projectInfo",
    .category  = "Projet",
    .descFR    = "Informations disponibles sur le projet en cours d'execution.",
    .descEN    = "Information available about the running project.",
    .values    = E_projetInfo_values,
    .combinable = false,
};

static const OwlConstDef PROJET_CONSTS[] = {
    {"piNomProjet", "Projet", INTEGER_VAL(OWL_PROJETINFO_NOM),
     "Nom du projet en cours d'execution (valeur par defaut).",
     "Name of the running project (default value).",
     "piProjectName"},
};

/* enum* — cible de la reflexion ElementsDe. Enum extensible : une seule valeur
 * aujourd'hui (les fenetres declarees du projet). */
static const char* const E_elementProjet_values[] = {
    "enumFen\xc3\xaatre",
    NULL
};

static const OwlEnum E_elementProjet = {
    .name      = "elementProjet",
    .nameAscii = "elementProjet",
    .nameEN    = "projectElement",
    .category  = "Projet",
    .descFR    = "Cat\xc3\xa9gorie d'\xc3\xa9l\xc3\xa9ments du projet a recenser avec "
                 "\xc3\x89l\xc3\xa9mentsDe.",
    .descEN    = "Category of project elements to list with ElementsOf.",
    .values    = E_elementProjet_values,
    .combinable = false,
};

static const OwlConstDef ELEMENT_PROJET_CONSTS[] = {
    {"enumFen\xc3\xaatre", "Projet", INTEGER_VAL(OWL_ELEMENTPROJET_FENETRE),
     "Fen\xc3\xaatres d\xc3\xa9""clar\xc3\xa9""es du projet.",
     "Windows declared in the project.",
     "enumWindow"},
};

static const OwlEnum* const PARAM_ENUMS[] = {
    &E_paramSupport, &E_projetInfo, &E_elementProjet
};

static const OwlParam p_init_1[] = {
    {"support", "entier",
     "Support de stockage a activer.", &E_paramSupport, NULL},
    {0}
};
static const OwlParam p_init_2[] = {
    {"support", "entier",
     "Support de stockage a activer.", &E_paramSupport, NULL},
    {"source", "cha\xc3\xae" "ne",
     "Chemin du fichier de stockage ; contenu serialise de depart pour le "
     "support en memoire.", NULL, NULL},
    {0}
};
static const OwlSignature sig_init[] = {
    {1, p_init_1, "cha\xc3\xae" "ne",
     "Etat anterieur a l'appel : jeton de support, tabulation, source."},
    {2, p_init_2, "cha\xc3\xae" "ne",
     "Etat anterieur a l'appel : jeton de support, tabulation, source."},
    {0}
};

static const OwlParam p_sauve[] = {
    {"nom", "cha\xc3\xae" "ne",
     "Nom du parametre. Les separateurs '.', '/' et '\\' delimitent un chemin.",
     NULL, NULL},
    {"valeur", "cha\xc3\xae" "ne|entier",
     "Valeur a enregistrer ; stockee sous sa forme texte.", NULL, NULL},
    {0}
};
static const OwlSignature sig_sauve[] = {
    {2, p_sauve, "rien", "Enregistre et persiste immediatement le parametre."},
    {0}
};

static const OwlParam p_charge_1[] = {
    {"nom", "cha\xc3\xae" "ne", "Nom du parametre a relire.", NULL, NULL},
    {0}
};
static const OwlParam p_charge_2[] = {
    {"nom", "cha\xc3\xae" "ne", "Nom du parametre a relire.", NULL, NULL},
    {"d\xc3\xa9" "faut", "cha\xc3\xae" "ne",
     "Valeur renvoyee si le parametre n'a jamais ete enregistre.", NULL, NULL},
    {0}
};
static const OwlSignature sig_charge[] = {
    {1, p_charge_1, "cha\xc3\xae" "ne",
     "Valeur enregistree, ou chaine vide si le parametre est absent."},
    {2, p_charge_2, "cha\xc3\xae" "ne",
     "Valeur enregistree, ou valeur par defaut si le parametre est absent."},
    {0}
};

static const OwlParam p_supprime[] = {
    {"nom", "cha\xc3\xae" "ne",
     "Nom du parametre a effacer, ou nom d'element dont tous les parametres "
     "sont effaces.", NULL, NULL},
    {0}
};
static const OwlSignature sig_supprime[] = {
    {0, NULL, "rien", "Efface tous les parametres du support actif."},
    {1, p_supprime, "rien",
     "Efface le parametre nomme ainsi que tous ceux de l'element homonyme."},
    {0}
};

static const OwlParam p_projet_info[] = {
    {"typeInfo", "entier",
     "Type d'information demande.", &E_projetInfo, NULL},
    {0}
};
static const OwlSignature sig_projet_info[] = {
    {0, NULL, "cha\xc3\xae" "ne",
     "Nom du projet en cours (defaut piNomProjet), chaine vide hors projet."},
    {1, p_projet_info, "cha\xc3\xae" "ne",
     "Information demandee sur le projet en cours ; chaine vide hors projet."},
    {0}
};

static const OwlParam p_elements_de[] = {
    {"cible", "entier",
     "Cat\xc3\xa9gorie d'\xc3\xa9l\xc3\xa9ments a recenser.", &E_elementProjet, NULL},
    {0}
};
static const OwlSignature sig_elements_de[] = {
    {1, p_elements_de, "tableau de cha\xc3\xae" "nes",
     "Noms des elements de la categorie demandee ; tableau vide hors projet."},
    {0}
};

static const OwlNativeDef DEFS_PARAM[] = {
    {
        .canonical  = "ProjetInfo",
        .nameEN     = "ProjectInfo",
        .category   = "Projet",
        .descFR     = "Renvoie une information sur le projet en cours "
                      "d'execution. Sans argument, renvoie le nom du projet "
                      "(identifiant du bloc `est un Projet`). Hors contexte "
                      "projet, renvoie une chaine vide.",
        .descEN     = "Returns information about the running project. Without an "
                      "argument, returns the project name (identifier of the "
                      "`is a Project` block). Outside a project context, returns "
                      "an empty string.",
        .signatures = sig_projet_info,
        .impl       = fn_projet_info,
    },
    {
        .canonical      = "\xc3\x89l\xc3\xa9mentsDe",   /* ÉlémentsDe */
        .canonicalAscii = "ElementsDe",
        .nameEN         = "ElementsOf",
        .category       = "Projet",
        .descFR         = "Renvoie la liste des elements declares du projet pour "
                          "une categorie donnee, sous forme de tableau de "
                          "chaines. Avec enumFen\xc3\xaatre, renvoie les noms de "
                          "toutes les fenetres declarees du projet, qu'elles "
                          "soient ouvertes ou non. Hors contexte projet, renvoie "
                          "un tableau vide.",
        .descEN         = "Returns the list of declared project elements for a "
                          "given category, as an array of strings. With "
                          "enumWindow, returns the names of every window declared "
                          "in the project, whether open or not. Outside a project "
                          "context, returns an empty array.",
        .signatures     = sig_elements_de,
        .impl           = fn_elements_de,
    },
    {
        .canonical      = "Param\xc3\xa8" "treInitialise",
        .canonicalAscii = "ParametreInitialise",
        .nameEN         = "ParameterInitialize",
        .category       = "Syst\xc3\xa8me",
        .descFR         = "Choisit le support de stockage des valeurs "
                          "persistantes et sa source. Renvoie l'etat qui etait "
                          "actif juste avant l'appel, sous la forme d'un jeton "
                          "de support (json, xml, ini, string), d'une "
                          "tabulation, puis de la source. Sans appel prealable, "
                          "les valeurs sont stockees au format JSON dans le "
                          "dossier de donnees utilisateur ; ce chemin par "
                          "defaut n'est materialise sur disque qu'au premier "
                          "enregistrement.",
        .descEN         = "Selects the storage backend for persistent values "
                          "and its source. Returns the state that was active "
                          "right before the call, as a backend token (json, "
                          "xml, ini, string), a tab, then the source. Without "
                          "a prior call, values are stored in JSON format in "
                          "the user data folder; that default path is only "
                          "materialized on disk at the first save.",
        .signatures     = sig_init,
        .impl           = fn_init_parametre,
    },
    {
        .canonical      = "Param\xc3\xa8" "treEnregistre",
        .canonicalAscii = "ParametreEnregistre",
        .nameEN         = "ParameterSave",
        .category       = "Syst\xc3\xa8me",
        .descFR         = "Enregistre une valeur persistante sous un nom. La "
                          "valeur est stockee sous sa forme texte et ecrite "
                          "immediatement sur le support actif. Les separateurs "
                          "'.', '/' et '\\' sont equivalents et decoupent le "
                          "nom en chemin. Un nom vide est refuse.",
        .descEN         = "Saves a persistent value under a name. The value is "
                          "stored in text form and written immediately to the "
                          "active backend. The '.', '/' and '\\' separators are "
                          "equivalent and split the name into a path. An empty "
                          "name is rejected.",
        .signatures     = sig_sauve,
        .impl           = fn_sauve_parametre,
    },
    {
        .canonical      = "Param\xc3\xa8" "treCharge",
        .canonicalAscii = "ParametreCharge",
        .nameEN         = "ParameterLoad",
        .category       = "Syst\xc3\xa8me",
        .descFR         = "Relit une valeur persistante. Le resultat est "
                          "toujours une chaine : convertir avec Val pour "
                          "obtenir un nombre. Sur un nom jamais enregistre, "
                          "renvoie la valeur par defaut fournie, ou la chaine "
                          "vide en son absence. Un nom vide est refuse.",
        .descEN         = "Reads back a persistent value. The result is always "
                          "a string: convert with Val to get a number. For a "
                          "name that was never saved, returns the given default "
                          "value, or an empty string if none. An empty name is "
                          "rejected.",
        .signatures     = sig_charge,
        .impl           = fn_charge_parametre,
    },
    {
        .canonical      = "Param\xc3\xa8" "treSupprime",
        .canonicalAscii = "ParametreSupprime",
        .nameEN         = "ParameterDelete",
        .category       = "Syst\xc3\xa8me",
        .descFR         = "Efface des valeurs persistantes du support actif. "
                          "Sans argument, efface la totalite. Avec un nom, "
                          "efface le parametre nomme et tous ceux de l'element "
                          "homonyme : la coupure se fait sur une frontiere de "
                          "separateur, jamais au milieu d'un segment de nom.",
        .descEN         = "Deletes persistent values from the active backend. "
                          "Without an argument, deletes everything. With a "
                          "name, deletes the named parameter and every "
                          "parameter of the matching element: the cut happens "
                          "on a separator boundary, never in the middle of a "
                          "name segment.",
        .signatures     = sig_supprime,
        .impl           = fn_supprime_parametre,
    },
};

void stdlib_parametre_init(void) {
    /* Etat remis a neuf a chaque vm_init : un fichier de test ne doit pas
     * heriter du support choisi par le precedent. */
    store_free();
    free(g_source);
    g_source = NULL;
    g_mode   = OWL_PARAM_JSON;
    g_ready  = false;

    owl_registry_register_module(DEFS_PARAM,
        (int)(sizeof(DEFS_PARAM) / sizeof(DEFS_PARAM[0])));
    owl_registry_register_enums(PARAM_ENUMS,
        (int)(sizeof(PARAM_ENUMS) / sizeof(PARAM_ENUMS[0])));
    owl_registry_register_consts(PARAM_CONSTS,
        (int)(sizeof(PARAM_CONSTS) / sizeof(PARAM_CONSTS[0])));
    owl_registry_register_consts(PROJET_CONSTS,
        (int)(sizeof(PROJET_CONSTS) / sizeof(PROJET_CONSTS[0])));
    owl_registry_register_consts(ELEMENT_PROJET_CONSTS,
        (int)(sizeof(ELEMENT_PROJET_CONSTS) / sizeof(ELEMENT_PROJET_CONSTS[0])));
}
