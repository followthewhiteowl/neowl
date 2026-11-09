/*  — l'aide HTML du champ Table (objet DATA[] genere par
 * `owl --dump-doc-html <template>`, l'affichage que l'utilisateur consulte)
 * ne disait rien d'exploitable sur la gestion des COLONNES (support #308,
 * re-signale 4x). Trois trous mesures sur le binaire reel :
 *
 *   1. xref = 0 : l'objet Table ne portait NI see-also vers la fiche Colonne,
 *      NI exemple de declaration de colonne. Les `examples` presents etaient
 *      des corps de tests (OuvreSoeur + assertions), pas une declaration.
 *   2. La guidance colonnes riche n'existait que cote CLI (`owl explain
 *      Table`), jamais repercutee cote HTML : desynchronisation des deux
 *      affichages.
 *   3. La guidance CLI citait une propriete INEXISTANTE : `TAB..CouleurTexte`
 *      (la vraie est `PoliceCouleur` / `FontColor`).
 *
 * Ce scanner verrouille le resultat pour qu'il ne re-casse pas (6e ratage) :
 *   - HTML : l'objet widget Table porte `widgetSeeAlso` vers `Colonne`.
 *   - HTML : l'objet widget Table porte `declExample` = une declaration de
 *     colonne reelle (`est une Colonne` + bloc PROPRIETES Libelle/Type/Largeur).
 *   - HTML : plus aucune mention de la propriete bidon `CouleurTexte`.
 *   - CLI  : `owl explain Table` cite `PoliceCouleur` et plus `CouleurTexte`
 *     (sync CLI<->HTML sur du contenu correct).
 *   - COMPILE : le code FR et EN du declExample, extrait tel qu'emis dans le
 *     HTML, est ecrit dans un .owl isole et passe un VRAI `owl --check` (0
 *     erreur). C'est le verrou critique : un `Champ Table` n'est valide que
 *     nesté dans une Fenetre, donc l'exemple doit livrer une Fenetre complete
 *     copier-collable. Sans ce check reel (strstr seul), l'exemple pouvait
 *     etre publie "compilable" tout en provoquant 65 erreurs de parse chez
 *     l'utilisateur (support #308 "infos fausses").
 *
 * Boite noire, calque crossref_viewer_parity_scan.c. Compile a la volee par
 * tests/cli/doc_lang/run.sh. Exit code 0 si tout tient, 1 sinon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define POPEN _popen
#define PCLOSE _pclose
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#define GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define POPEN popen
#define PCLOSE pclose
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#define GETPID() getpid()
#endif

static char* run_capture(const char* cmdline) {
    FILE* fp = POPEN(cmdline, "r");
    if (!fp) return NULL;
    size_t cap = 1 << 20, len = 0; /* DATA[] est gros (tout le registry) */
    char* buf = (char*)malloc(cap);
    if (!buf) { PCLOSE(fp); return NULL; }
    buf[0] = '\0';
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = (cap + n + 1) * 2;
            buf = (char*)realloc(buf, cap);
        }
        memcpy(buf + len, chunk, n);
        len += n;
        buf[len] = '\0';
    }
    PCLOSE(fp);
    return buf;
}

static void derive_repo_root(const char* owlBin, char* out, size_t outsz) {
    const char* marker = "/build/";
    const char* p = strstr(owlBin, marker);
    if (p) {
        const char* q;
        while ((q = strstr(p + 1, marker)) != NULL) p = q;
    }
    if (!p) { snprintf(out, outsz, "."); return; }
    size_t len = (size_t)(p - owlBin);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, owlBin, len);
    out[len] = '\0';
}

static char* run_dump_doc_html(const char* owlBin) {
    char root[1024];
    derive_repo_root(owlBin, root, sizeof(root));
    char cmdline[2048];
    snprintf(cmdline, sizeof(cmdline),
             "\"\"%s\" --dump-doc-html \"%s/docs/reference/_owl-doc-template.html\" 2>NUL\"",
             owlBin, root);
    return run_capture(cmdline);
}

static char* run_owl(const char* owlBin, const char* args) {
    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "\"\"%s\" %s 2>NUL\"", owlBin, args);
    return run_capture(cmdline);
}

/* Isole l'objet JSON dont le prefixe exact est `needle` (ex.
 * `{"kind":"widget","name":"Table"`), borne a l'accolade fermante
 * correspondante (comptage de profondeur). NULL si introuvable. */
static char g_obj_buf[1 << 16];
static const char* find_object_by_prefix(const char* data, const char* needle) {
    const char* objStart = strstr(data, needle);
    if (!objStart) return NULL;
    int depth = 0;
    const char* p = objStart;
    for (; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
    }
    size_t len = (size_t)(p - objStart);
    if (len >= sizeof(g_obj_buf)) len = sizeof(g_obj_buf) - 1;
    memcpy(g_obj_buf, objStart, len);
    g_obj_buf[len] = '\0';
    return g_obj_buf;
}

static int must_contain(const char* hay, const char* needle, const char* ctx) {
    if (hay && strstr(hay, needle)) return 0;
    fprintf(stderr, "FAIL: %s : attendu present mais absent : %s\n", ctx, needle);
    return 1;
}

static int must_not_contain(const char* hay, const char* needle, const char* ctx) {
    if (!hay || !strstr(hay, needle)) return 0;
    fprintf(stderr, "FAIL: %s : present a tort : %s\n", ctx, needle);
    return 1;
}

/* Extrait la valeur string JSON de la clef `"<key>":"..."` dans `obj`, en
 * dé-échappant \n \t \r \" \\ (les octets UTF-8 accentues passent tels quels).
 * Ecrit dans out (zero-terminee). Retourne 1 si trouvee, 0 sinon. */
static int extract_json_string(const char* obj, const char* key,
                               char* out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = obj ? strstr(obj, pat) : NULL;
    if (!p) return 0;
    p += strlen(pat);
    size_t o = 0;
    while (*p && o + 1 < outsz) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                case 'r': out[o++] = '\r'; break;
                case '"': out[o++] = '"';  break;
                case '\\': out[o++] = '\\'; break;
                case '/': out[o++] = '/';  break;
                default:  out[o++] = *p;   break; /* \uXXXX non attendu ici */
            }
            if (*p) p++;
        } else if (*p == '"') {
            break; /* fin de la chaine JSON (quote non echappee) */
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return 1;
}

/* Ecrit `code` dans un .owl isole (dossier temp dedie), lance un VRAI
 * `owl --check` dessus, retourne 0 si 0 erreur de parse, 1 sinon. C'est ce
 * verrou qui rend la claim "exemple compilable" vraie : le declExample publie
 * dans l'aide doit se copier-coller et passer la verif sans wrapper externe. */
static int check_compiles(const char* owlBin, const char* code, const char* ctx) {
    const char* base = getenv("TEMP");
    if (!base) base = getenv("TMP");
    if (!base) base = ".";
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/owl_decl_check_%d", base, (int)GETPID());
    MKDIR(dir); /* ignore EEXIST */
    char file[1200];
    snprintf(file, sizeof(file), "%s/FEN_Contacts.owl", dir);
    FILE* f = fopen(file, "wb");
    if (!f) {
        fprintf(stderr, "FAIL: %s : impossible d'ecrire le fichier temp %s\n", ctx, file);
        return 1;
    }
    fwrite(code, 1, strlen(code), f);
    fclose(f);

    char args[1400];
    snprintf(args, sizeof(args), "--check \"%s\"", file);
    char* out = run_owl(owlBin, args);

    int fail = 0;
    /* `owl --check` emet du JSON {"errors":[...]} ; 0 erreur => tableau vide,
     * donc aucune clef "message". Une seule erreur de parse suffit a faire
     * echouer. */
    if (!out) {
        fprintf(stderr, "FAIL: %s : sortie vide de 'owl --check'\n", ctx);
        fail = 1;
    } else if (strstr(out, "\"message\"")) {
        fprintf(stderr, "FAIL: %s : l'exemple declExample NE COMPILE PAS en standalone.\n"
                        "      owl --check a rapporte des erreurs :\n%s\n", ctx, out);
        fail = 1;
    }
    free(out);

    remove(file);
    RMDIR(dir);
    return fail;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* ---- Volet HTML (objet widget Table dans DATA[]) ---- */
    char* out = run_dump_doc_html(owlBin);
    if (!out || !*out) {
        fprintf(stderr, "FAIL: sortie vide pour 'owl --dump-doc-html <template>'\n");
        free(out);
        return 1;
    }
    if (!strstr(out, "const DATA = [")) {
        fprintf(stderr, "FAIL: marqueur \"const DATA = [\" absent (template introuvable ?)\n");
        free(out);
        return 1;
    }

    const char* obj = find_object_by_prefix(out, "{\"kind\":\"widget\",\"name\":\"Table\"");
    if (!obj) {
        fprintf(stderr, "FAIL: objet widget \"Table\" introuvable dans DATA[]\n");
        fails++;
    } else {
        const char* ctx = "DATA[] widget Table";
        /* (b) see-also structure vers la fiche Colonne. */
        fails += must_contain(obj, "\"widgetSeeAlso\"", ctx);
        fails += must_contain(obj, "Colonne", ctx);
        /* (c) exemple de DECLARATION de colonne (pas un corps de test). */
        fails += must_contain(obj, "\"declExample\"", ctx);
        fails += must_contain(obj, "est une Colonne", ctx);
        fails += must_contain(obj, "colTypeTexte", ctx);
        /* propriete bidon : ne doit plus apparaitre. */
        fails += must_not_contain(obj, "CouleurTexte", ctx);

        /* (d) VRAI verrou "compilable" : on extrait le code FR et EN du
         * declExample tel qu'emis dans le HTML, on l'ecrit dans un .owl
         * isole et on lance owl --check dessus. L'exemple se copie-colle et
         * doit passer sans wrapper externe (support #308 : "infos fausses"
         * si l'exemple de l'aide ne compile pas). */
        static char codeFR[1 << 15], codeEN[1 << 15];
        /* Scoper au sous-objet declExample : l'objet widget contient aussi un
         * tableau `examples` dont les entrees portent leur propre clef "code"
         * (corps de tests) ; sans ce scope on extrairait le mauvais "code". */
        const char* decl = strstr(obj, "\"declExample\"");
        if (!decl) {
            fprintf(stderr, "FAIL: %s : sous-objet declExample introuvable\n", ctx);
            fails++;
        } else {
            if (!extract_json_string(decl, "code", codeFR, sizeof(codeFR))) {
                fprintf(stderr, "FAIL: %s : clef declExample.code introuvable\n", ctx);
                fails++;
            } else {
                fails += check_compiles(owlBin, codeFR, "declExample.code (FR) owl --check");
            }
            if (!extract_json_string(decl, "codeEN", codeEN, sizeof(codeEN))) {
                fprintf(stderr, "FAIL: %s : clef declExample.codeEN introuvable\n", ctx);
                fails++;
            } else {
                fails += check_compiles(owlBin, codeEN, "declExample.codeEN (EN) owl --check");
            }
        }
    }
    free(out);

    /* ---- Volet CLI (sync : meme contenu correct cote owl explain) ---- */
    char* fr = run_owl(owlBin, "explain Table --lang fr");
    if (!fr || !*fr) {
        fprintf(stderr, "FAIL: sortie vide pour 'explain Table --lang fr'\n");
        fails++;
    } else {
        const char* ctx = "explain Table --lang fr";
        fails += must_contain(fr, "PoliceCouleur", ctx);
        fails += must_not_contain(fr, "CouleurTexte", ctx);
    }
    free(fr);

    printf("=== feat/2393 aide HTML Table : colonnes (see-also + exemple + fix CouleurTexte) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (widgetSeeAlso->Colonne, declExample present, CouleurTexte absent, CLI sync)\n");
    return 0;
}
