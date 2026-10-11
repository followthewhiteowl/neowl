/*  — `owl openapi <service.owl>` : export CLI du document OpenAPI 3
 * dérivé de l'AST ServiceREST/OpérationHTTP.
 *
 * Principe (calque tests/cli/doc_lang/leak_scan.c, ) : on invoque le
 * VRAI binaire owl.exe (chemin passé en argv[1], ou ../../build/owl.exe par
 * défaut) sur des fixtures `.owl` de service REST, on capture stdout, et on
 * scanne le JSON produit contre les critères d'acceptation du ticket — sans
 * validateur de schéma complet (pas de dépendance JSON dans le toolchain
 * C), juste des vérifications structurantes robustes à pretty vs compact
 * (cf. json_key_has_value / window_contains_pair ci-dessous, qui tolèrent
 * un espace optionnel autour de ':' et ne supposent aucun ordre de clés).
 *
 * Ce n'est PAS un self-test compilé dans owl.exe (on ne touche pas à src/) :
 * harness boîte noire indépendant, compilé à la volée par
 * tests/cli/openapi/run.sh.
 *
 * ROUGE ATTENDU AUJOURD'HUI : la sous-commande `openapi` n'existe pas
 * (dispatch argv absent de main.c) — la sortie sera vide ou une aide CLI
 * générique, aucune des assertions structurelles ne doit matcher.
 *
 * Couvre (voir ticket ) :
 *   1. Document structurellement valide (openapi 3.x, info, paths, components)
 *   2. Chemins UrlBase+Chemin, méthode correcte, paramètre {id}
 *   3. Paramètres path (required) / query (required dérivé du défaut)
 *   4. requestBody : type d'entrée + multipart/form-data (FichierTéléversé)
 *   5. responses : map code -> réponse, Problem Details, 500 implicite
 *   6. securityScheme dérivé de Authentification (Bearer + opt-out aucune)
 *   7. Dérivation : deux services qui diffèrent sur un point -> deux specs
 *      qui diffèrent sur ce point, sans seconde déclaration.
 *
 * Exit code : 0 si toutes les assertions passent, 1 sinon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

/* ---- capture d'une commande (calque leak_scan.c::run_capture) ---- */

static char* run_capture(const char* cmdline) {
    FILE* fp = POPEN(cmdline, "r");
    if (!fp) return NULL;
    size_t cap = 65536, len = 0;
    char* buf = (char*)malloc(cap);
    buf[0] = '\0';
    char chunk[4096];
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

/* `owl openapi <fixture>` — memes precautions Windows/_popen que
 * leak_scan.c::main (double guillemet enveloppant, cf. commentaire 
 * la-bas) : sous Windows, cmd.exe (via _popen) ne retire QUE la paire de
 * guillemets la plus externe. */
static char* run_owl_openapi(const char* owlBin, const char* fixture) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" openapi %s 2>&1", owlBin, fixture);
    char full[1200];
    snprintf(full, sizeof full, "\"%s\"", cmd);
    return run_capture(full);
}

/* ---- assertions JSON tolerantes pretty/compact -------------------- */

/* Cherche cle "key" a partir de `from` (ou du debut si NULL). */
static const char* json_find_key(const char* text, const char* from, const char* key) {
    char pat[256];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    return strstr(from ? from : text, pat);
}

/* Vrai si "key":<value...> existe QUELQUE PART dans text, `value` etant
 * matche en PREFIXE (permet de passer une chaine "\"3." pour capturer
 * "3.0.3", "3.1.0", ...). Espace optionnel autour du ':'. */
static int json_key_has_value(const char* text, const char* key, const char* value) {
    const char* p = text;
    size_t keylen = strlen(key);
    while ((p = json_find_key(text, p, key)) != NULL) {
        const char* q = p + keylen + 2; /* saute "key" */
        while (isspace((unsigned char)*q)) q++;
        if (*q != ':') { p += 1; continue; }
        q++;
        while (isspace((unsigned char)*q)) q++;
        if (strncmp(q, value, strlen(value)) == 0) return 1;
        p += 1;
    }
    return 0;
}

/* Vrai si "key" existe QUELQUE PART (presence seule, valeur non verifiee). */
static int json_has_key(const char* text, const char* key) {
    return json_find_key(text, NULL, key) != NULL;
}

/* Compte les occurrences de la sous-chaine litterale `needle`. */
static int count_occurrences(const char* text, const char* needle) {
    int n = 0;
    const char* p = text;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { n++; p += nl; }
    return n;
}

/* Vrai si, dans une fenetre de `window` octets autour d'une occurrence de
 * "anchor_key":anchor_value_prefix, la paire "pair_key":pair_value_prefix
 * apparait aussi. Sert a verifier qu'un champ appartient au MEME objet
 * qu'un autre sans dependre de l'ordre des cles ni parser le JSON. */
static int window_contains_pair(const char* text,
                                 const char* anchor_key, const char* anchor_value,
                                 const char* pair_key, const char* pair_value,
                                 int window) {
    const char* p = text;
    size_t akeylen = strlen(anchor_key);
    size_t textlen = strlen(text);
    while ((p = json_find_key(text, p, anchor_key)) != NULL) {
        const char* q = p + akeylen + 2;
        while (isspace((unsigned char)*q)) q++;
        if (*q == ':') {
            const char* v = q + 1;
            while (isspace((unsigned char)*v)) v++;
            if (strncmp(v, anchor_value, strlen(anchor_value)) == 0) {
                const char* start = (p - text > window) ? p - window : text;
                const char* endp  = (p + window < text + textlen) ? p + window : text + textlen;
                size_t blen = (size_t)(endp - start);
                char* buf = (char*)malloc(blen + 1);
                memcpy(buf, start, blen);
                buf[blen] = '\0';
                int hit = json_key_has_value(buf, pair_key, pair_value);
                free(buf);
                if (hit) return 1;
            }
        }
        p += 1;
    }
    return 0;
}

/* ---- rapport ------------------------------------------------------- */

static int g_fail = 0;
static void check(int ok, const char* label) {
    if (!ok) { g_fail++; fprintf(stderr, "  FAIL [%s]\n", label); }
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    const char* fixtureA = "tests/internal_fixtures/cli_openapi/service_clients.owl";
    const char* fixtureB = "tests/internal_fixtures/cli_openapi/service_clients_variante.owl";
    const char* fixtureC = "tests/internal_fixtures/cli_openapi/service_fichiers_catchall.owl";
    const char* fixtureD = "tests/internal_fixtures/cli_openapi/service_exclusion.owl";
    const char* fixtureE = "tests/internal_fixtures/cli_openapi/service_exclusion_en.owl";
    const char* fixtureF = "tests/internal_fixtures/cli_openapi/service_flux.owl";

    char* outA = run_owl_openapi(owlBin, fixtureA);
    char* outB = run_owl_openapi(owlBin, fixtureB);
    char* outC = run_owl_openapi(owlBin, fixtureC);
    char* outD = run_owl_openapi(owlBin, fixtureD);
    char* outE = run_owl_openapi(owlBin, fixtureE);
    char* outF = run_owl_openapi(owlBin, fixtureF);

    if (!outA || !*outA) {
        fprintf(stderr, "ERREUR: sortie vide pour %s (commande non executee)\n", fixtureA);
        free(outA); free(outB);
        return 1;
    }

    /* --- Critere 1 : document structurellement valide --- */
    check(json_key_has_value(outA, "openapi", "\"3."), "c1_version_openapi_3x");
    check(json_has_key(outA, "info"), "c1_bloc_info_present");
    check(json_has_key(outA, "paths"), "c1_bloc_paths_present");
    check(json_has_key(outA, "components"), "c1_bloc_components_present");
    {
        /* Sortie CLI = JSON pur (pas de banniere avant/apres) : le premier
         * caractere non-blanc doit ouvrir un objet. */
        const char* p = outA;
        while (isspace((unsigned char)*p)) p++;
        check(*p == '{', "c1_stdout_commence_par_accolade_ouvrante");
    }

    /* --- Critere 2 : chemins/methodes, {id} bien place --- */
    check(strstr(outA, "\"/api/clients/{id}\"") != NULL,
          "c2_chemin_urlbase_plus_segment_parametre");
    check(window_contains_pair(outA, "/api/clients/{id}", "{", "get", "{", 400),
          "c2_methode_get_sur_route_parametree");
    check(strstr(outA, "\"/api/clients\"") != NULL,
          "c2_chemin_urlbase_plus_segment_litteral");
    /* fenetre 1200 (vs 400 ailleurs) : post est la 2e methode du path object,
     * apres un bloc get complet (~721 o) ; le path suivant est a ~1430 o. */
    check(window_contains_pair(outA, "/api/clients", "{", "post", "{", 1200),
          "c2_methode_post_sur_creation");
    check(strstr(outA, "\"/api/ping\"") != NULL, "c2_chemin_segment_seul");

    /* --- Critere 3 : required path (Vrai) vs query avec defaut (Faux) --- */
    check(window_contains_pair(outA, "name", "\"id\"", "required", "true", 300),
          "c3_param_chemin_required_vrai");
    check(window_contains_pair(outA, "name", "\"page\"", "required", "false", 300),
          "c3_param_requete_avec_defaut_required_faux");

    /* --- Critere 4 : requestBody type d'entree + multipart --- */
    check(strstr(outA, "application/json") != NULL,
          "c4_requestbody_json_pour_corps_structure");
    check(strstr(outA, "multipart/form-data") != NULL,
          "c4_requestbody_multipart_pour_fichierteleverse");
    check(window_contains_pair(outA, "name", "\"avatar\"", "format", "\"binary\"", 400) ||
          strstr(outA, "\"format\":\"binary\"") != NULL,
          "c4_fichierteleverse_format_binary");

    /* --- Critere 5 : responses (code->reponse), 500 implicite --- */
    check(strstr(outA, "\"404\"") != NULL, "c5_code_erreur_declare_present");
    check(count_occurrences(outA, "\"500\"") >= 5,
          "c5_500_implicite_sur_chaque_operation");

    /* --- Critere 6 : securityScheme derive de Authentification --- */
    check(json_has_key(outA, "securitySchemes"), "c6_bloc_securityschemes_present");
    check(json_key_has_value(outA, "type", "\"http\""), "c6_scheme_type_http");
    check(json_key_has_value(outA, "scheme", "\"bearer\""), "c6_scheme_bearer");
    check(window_contains_pair(outA, "/api/ping", "{", "security", "[]", 500) ||
          window_contains_pair(outA, "/api/ping", "{", "security", "[ ]", 500),
          "c6_authaucune_optout_operation_ping");

    /* --- Critere 7 : derivation, deux services qui diffèrent d'un point --- */
    check(outB && *outB, "c7_variante_produit_aussi_un_document");
    if (outB && *outB) {
        check(window_contains_pair(outA, "/api/ping", "{", "get", "{", 400),
              "c7_service_a_ping_en_get");
        check(window_contains_pair(outB, "/api/ping", "{", "post", "{", 400),
              "c7_service_b_ping_en_post_apres_seul_changement_dsl");
        check(strcmp(outA, outB) != 0, "c7_deux_services_distincts_deux_specs_distinctes");
    }

    /* ---  : catch-all glouton (`*reste`, ) enrichi --- */
    /* Le segment catch-all capture 1+ segments finaux ; le document doit le
     * signaler (contrat OpenAPI honnete) via une description dediee + une
     * extension vendor machine-lisible. Un parametre de chemin ORDINAIRE
     * (`id`) reste STRICTEMENT nu (Option A, decision dev). */
    check(outC && *outC, "fix1972_fixture_catchall_produit_un_document");
    if (outC && *outC) {
        /* Le chemin reste `{reste}` (OpenAPI 3 n'a pas de catch-all natif). */
        check(strstr(outC, "\"/api/v1/fichiers/{reste}\"") != NULL,
              "fix1972_chemin_catchall_reste_accolade");
        /* Extension vendor presente EXACTEMENT une fois (sur le catch-all,
         * pas sur le param ordinaire `id` du meme document). */
        check(count_occurrences(outC, "x-owl-catch-all") == 1,
              "fix1972_extension_catchall_unique");
        /* ...et rattachee au bon parametre (`reste`), valeur true. */
        check(window_contains_pair(outC, "name", "\"reste\"", "x-owl-catch-all", "true", 400),
              "fix1972_extension_sur_param_reste");
        /* Description non vide mentionnant la nature multi-segment. Phrase
         * unique (absente des descriptions de reponse "200 OK", etc.) : garde
         * le RED honnete avant impl. */
        check(strstr(outC, "one or more trailing segments") != NULL,
              "fix1972_description_multisegment");
        /* Description co-localisee avec l'extension (meme objet parametre),
         * pas une description de reponse capturee par hasard. */
        check(window_contains_pair(outC, "x-owl-catch-all", "true", "description", "\"", 200),
              "fix1972_description_dans_objet_param_catchall");
        /* NON-REGRESSION : le param ordinaire `id` du meme document n'a NI
         * extension NI description propre. */
        check(!window_contains_pair(outC, "name", "\"id\"", "x-owl-catch-all", "true", 400),
              "fix1972_param_ordinaire_id_sans_extension");
    }
    /* NON-REGRESSION : un service 100% params ordinaires (fixtureA) n'emet
     * aucune extension catch-all. */
    check(count_occurrences(outA, "x-owl-catch-all") == 0,
          "fix1972_service_params_ordinaires_aucune_extension");

    /* ---  : ExclureDeOpenAPI — op servie mais absente du document --- */
    check(outD && *outD, "f2034_fixture_exclusion_produit_un_document");
    if (outD && *outD) {
        /* L'op normale (sans le flag) est documentee. */
        check(strstr(outD, "\"/api/v1/facture\"") != NULL,
              "f2034_op_normale_presente");
        /* L'op avec ExclureDeOpenAPI = Faux (defaut explicite) reste documentee. */
        check(strstr(outD, "\"/api/v1/bundle\"") != NULL,
              "f2034_op_flag_faux_presente");
        /* L'op avec ExclureDeOpenAPI = Vrai est OMISE : ni son chemin ni son
         * operationId n'apparaissent. */
        check(strstr(outD, "\"/api/v1/scalar\"") == NULL,
              "f2034_op_exclue_chemin_absent");
        check(strstr(outD, "\"DocScalar\"") == NULL,
              "f2034_op_exclue_operationid_absent");
        /* Exactement deux chemins documentes (facture + bundle), pas trois. */
        check(count_occurrences(outD, "\"/api/v1/") == 2,
              "f2034_deux_chemins_documentes_pas_trois");
    }

    /* ---  : invariant 3 — forme EN ExcludeFromOpenAPI --- */
    check(outE && *outE, "f2034_fixture_exclusion_en_produit_un_document");
    if (outE && *outE) {
        check(strstr(outE, "\"/api/v1/metrics\"") != NULL,
              "f2034_en_op_normale_presente");
        check(strstr(outE, "\"/api/v1/healthz\"") == NULL,
              "f2034_en_op_exclue_chemin_absent");
        check(strstr(outE, "\"HealthInternal\"") == NULL,
              "f2034_en_op_exclue_operationid_absent");
    }

    /* ---  (BUG 4) : le mode Flux se reflète dans le content-type --- */
    check(outF && *outF, "f2152_fixture_flux_produit_un_document");
    if (outF && *outF) {
        /* Route Flux=fluxSSE : la réponse succès 200 est servie en
         * text/event-stream, PAS en application/json. */
        check(strstr(outF, "text/event-stream") != NULL,
              "f2152_sse_content_type_event_stream");
        check(window_contains_pair(outF, "get", "{", "text/event-stream", "{", 500),
              "f2152_sse_content_type_dans_operation_get");
        /* Route Flux=fluxOctets : réponse succès servie en
         * application/octet-stream avec schéma binaire. */
        check(strstr(outF, "application/octet-stream") != NULL,
              "f2152_octets_content_type_octet_stream");
        check(window_contains_pair(outF, "application/octet-stream", "{",
                                   "format", "\"binary\"", 300),
              "f2152_octets_schema_binary");
        /* NON-RÉGRESSION : la route sans Flux garde application/json sur son
         * 200 (le content-type de succès n'est pas altéré ailleurs). Compté :
         * la route /statut a un 200 json + le corps de requête éventuel. Ici
         * aucun body, donc au moins une occurrence application/json = le 200
         * de /statut. */
        check(strstr(outF, "application/json") != NULL,
              "f2152_route_sans_flux_reste_json");
        /* Les erreurs (500 implicite) restent en problem+json quel que soit
         * le mode Flux — le flux ne concerne que la réponse succès. */
        check(strstr(outF, "application/problem+json") != NULL,
              "f2152_erreurs_restent_problem_json");
    }

    free(outA);
    free(outB);
    free(outC);
    free(outD);
    free(outE);
    free(outF);

    if (g_fail == 0) {
        printf("openapi_cli_scan: RESULT: GREEN (0 echec)\n");
        return 0;
    }
    printf("openapi_cli_scan: RESULT: RED (%d echec(s))\n", g_fail);
    return 1;
}
