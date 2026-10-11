// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test temporal parity (Lot 2B-1, axe 4 du chantier ).
 *
 * Vérifie l'invariant single-parser sur les 4 familles temporelles
 * (Date, Heure, DateHeure, Durée) : la coerce runtime et le compile-time
 * literal filter passent EXACTEMENT par le même parser C (cf header
 * stdlib_date_parsers.h). Cet invariant est prouvé par construction
 * dans le code (un seul appel `owl_parse_*` chacun côté). Ce self-test
 * fait la preuve mécanique inverse : pour un set canon d'inputs
 * (valides + invalides format + invalides bornes), le parser répond
 * exactement comme l'oracle l'attend.
 *
 * Si l'oracle et le parser divergent, c'est qu'une régression a touché
 * la fonction primitive — donc les DEUX chemins (compile/runtime)
 * sont impactés identiquement. Bloque le build.
 *
 * Architecture :
 *   - 4 oracles statiques (un par famille temporelle)
 *   - Chaque entry : input, kind attendu (OK / FORMAT_ERR / BOUNDS_ERR),
 *     description courte pour le rapport
 *   - Iterate, compare, accumuler violations
 *
 * Pas de vm_init nécessaire — les parsers sont C purs (pas de touch
 * VM/globals/registry).
 *
 * Branche sur `owl --self-test-temporal-parity` (cf src/main.c), gate
 * make all.
 *
 * Retour : 0 si toutes les paires concordent, sinon le nombre de
 * violations. */

#include "stdlib_date_parsers.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Kind attendu pour chaque entrée d'oracle. */
typedef enum {
    KIND_OK = 0,           /* parser doit retourner true */
    KIND_ERR_FORMAT = 1,   /* parser doit retourner false, outErr == FORMAT */
    KIND_ERR_BOUNDS = 2    /* parser doit retourner false, outErr == BOUNDS */
} ExpectedKind;

typedef struct {
    const char* input;
    int         len;       /* -1 -> strlen(input) */
    ExpectedKind kind;
    const char* description;
} TemporalCase;

/* Helper longueur : -1 -> strlen. */
static int case_len(const TemporalCase* c) {
    return c->len < 0 ? (int)strlen(c->input) : c->len;
}

/* ================================================================
 * Oracle Date — owl_parse_date_multi
 * Formats valides : AAAAMMJJ, JJ/MM/AAAA, JJ-MM-AAAA, AAAA-MM-JJ,
 * AAAA/MM/JJ. Cf docstring stdlib_date_parsers.h.
 * ================================================================ */
static const TemporalCase ORACLE_DATE[] = {
    /* --- Valides --- */
    {"20250101",   -1, KIND_OK, "AAAAMMJJ canon"},
    {"20251231",   -1, KIND_OK, "AAAAMMJJ fin annee"},
    {"01/01/2025", -1, KIND_OK, "JJ/MM/AAAA canon"},
    {"31/12/2025", -1, KIND_OK, "JJ/MM/AAAA fin annee"},
    {"01-01-2025", -1, KIND_OK, "JJ-MM-AAAA tirets"},
    {"2025-01-01", -1, KIND_OK, "AAAA-MM-JJ ISO 8601"},
    {"2025/01/01", -1, KIND_OK, "AAAA/MM/JJ ISO slash"},
    /* Annees bissextiles canon */
    {"20240229",   -1, KIND_OK, "AAAAMMJJ 29 fev annee bissextile"},
    {"29/02/2024", -1, KIND_OK, "JJ/MM/AAAA 29 fev annee bissextile"},
    /* --- Format invalide --- */
    {"",           -1, KIND_ERR_FORMAT, "chaine vide"},
    {"abcd",       -1, KIND_ERR_FORMAT, "non-digits courts"},
    {"2025010",    -1, KIND_ERR_FORMAT, "AAAAMMJJ tronque (7 chars)"},
    {"202501012",  -1, KIND_ERR_FORMAT, "AAAAMMJJ trop long (9 chars)"},
    {"01-Jan-2025",-1, KIND_ERR_FORMAT, "texte mois litteral"},
    {"01/01-2025", -1, KIND_ERR_FORMAT, "separateurs mixtes"},
    {"2025.01.01", -1, KIND_ERR_FORMAT, "separateur point non supporte"},
    {"2025 01 01", -1, KIND_ERR_FORMAT, "separateur espace non supporte"},
    /* --- Bornes invalides --- */
    {"20251301",   -1, KIND_ERR_BOUNDS, "AAAAMMJJ mois 13"},
    {"20250132",   -1, KIND_ERR_BOUNDS, "AAAAMMJJ jour 32"},
    {"20250230",   -1, KIND_ERR_BOUNDS, "AAAAMMJJ 30 fev non-bissextile"},
    {"20250431",   -1, KIND_ERR_BOUNDS, "AAAAMMJJ 31 avril (mois 30j)"},
    {"32/01/2025", -1, KIND_ERR_BOUNDS, "JJ/MM/AAAA jour 32"},
    {"01/13/2025", -1, KIND_ERR_BOUNDS, "JJ/MM/AAAA mois 13"},
    {"2025-13-01", -1, KIND_ERR_BOUNDS, "AAAA-MM-JJ mois 13"},
    {"2025-02-30", -1, KIND_ERR_BOUNDS, "AAAA-MM-JJ 30 fev"},
    {"2023-02-29", -1, KIND_ERR_BOUNDS, "AAAA-MM-JJ 29 fev annee non-bissextile"},
    {"00000000",   -1, KIND_ERR_BOUNDS, "AAAAMMJJ tous zeros"},
};
#define ORACLE_DATE_COUNT (sizeof(ORACLE_DATE) / sizeof(ORACLE_DATE[0]))

/* ================================================================
 * Oracle Heure — owl_parse_time_string_cs_ex
 * Formats : HHMMSS[CC] ou HH:MM[:SS[.CC|:CC]].
 * ================================================================ */
static const TemporalCase ORACLE_HEURE[] = {
    /* --- Valides --- */
    {"123045",     -1, KIND_OK, "HHMMSS compact"},
    {"12304500",   -1, KIND_OK, "HHMMSSCC compact + centiemes"},
    {"12:30",      -1, KIND_OK, "HH:MM seul"},
    {"12:30:45",   -1, KIND_OK, "HH:MM:SS"},
    {"12:30:45.50",-1, KIND_OK, "HH:MM:SS.CC centiemes (point)"},
    {"12:30:45:50",-1, KIND_OK, "HH:MM:SS:CC centiemes (deux-points)"},
    {"000000",     -1, KIND_OK, "minuit canon"},
    {"235959",     -1, KIND_OK, "fin de journee"},
    /* --- Format invalide --- */
    {"",           -1, KIND_ERR_FORMAT, "chaine vide"},
    {"abc",        -1, KIND_ERR_FORMAT, "non-digits"},
    {"12305",      -1, KIND_ERR_FORMAT, "5 chars (ni 6 ni 8)"},
    {"12:30:",     -1, KIND_ERR_FORMAT, "HH:MM: tronque"},
    {"12-30-45",   -1, KIND_ERR_FORMAT, "tirets non supportes"},
    /* --- Bornes invalides --- */
    {"253045",     -1, KIND_ERR_BOUNDS, "HHMMSS heure 25"},
    {"126045",     -1, KIND_ERR_BOUNDS, "HHMMSS minute 60"},
    {"123060",     -1, KIND_ERR_BOUNDS, "HHMMSS seconde 60"},
    {"25:30",      -1, KIND_ERR_BOUNDS, "HH:MM heure 25"},
    {"12:60",      -1, KIND_ERR_BOUNDS, "HH:MM minute 60"},
    {"12:30:60",   -1, KIND_ERR_BOUNDS, "HH:MM:SS seconde 60"},
};
#define ORACLE_HEURE_COUNT (sizeof(ORACLE_HEURE) / sizeof(ORACLE_HEURE[0]))

/* ================================================================
 * Oracle DateHeure — owl_parse_dateheure_iso_ex
 * Format ISO : "AAAA[-/]MM[-/]JJ[ T]HH:MM[:SS]"
 *
 * Note : la branche compact "AAAAMMJJ[HHMM[SS[CC|FFF]]]" est gérée
 * séparément dans le compile filter (cf compiler.c L8178+) et dans
 * fn_dateheure_coerce. Ici on teste seulement le parser ISO direct ;
 * la branche compact reutilise owl_parse_date_multi + bornes HH/MM/SS
 * deja couvertes par Date et Heure.
 * ================================================================ */
static const TemporalCase ORACLE_DATEHEURE[] = {
    /* --- Valides --- */
    {"2025-01-01 12:30:45",-1, KIND_OK, "ISO tirets + espace + SS"},
    {"2025-01-01T12:30:45",-1, KIND_OK, "ISO tirets + T + SS"},
    {"2025-01-01 12:30",   -1, KIND_OK, "ISO tirets + espace + sans SS"},
    {"2025/01/01 12:30:45",-1, KIND_OK, "ISO slash + espace"},
    {"2025/01/01T12:30",   -1, KIND_OK, "ISO slash + T + sans SS"},
    /* --- Format invalide --- */
    {"",                   -1, KIND_ERR_FORMAT, "chaine vide"},
    {"2025-01-01",         -1, KIND_ERR_FORMAT, "date seule (pas de partie horaire)"},
    {"12:30:45",           -1, KIND_ERR_FORMAT, "heure seule"},
    {"abcdef",             -1, KIND_ERR_FORMAT, "non-digits"},
    {"2025-01-01_12:30",   -1, KIND_ERR_FORMAT, "separateur date/heure invalide"},
    /* --- Bornes invalides --- */
    {"2025-13-01 12:30:45",-1, KIND_ERR_BOUNDS, "mois 13"},
    {"2025-01-32 12:30:45",-1, KIND_ERR_BOUNDS, "jour 32"},
    {"2025-01-01 25:30:45",-1, KIND_ERR_BOUNDS, "heure 25"},
    {"2025-01-01 12:60:45",-1, KIND_ERR_BOUNDS, "minute 60"},
    {"2025-01-01 12:30:60",-1, KIND_ERR_BOUNDS, "seconde 60"},
    {"2023-02-29 12:30:00",-1, KIND_ERR_BOUNDS, "29 fev annee non-bissextile"},
};
#define ORACLE_DATEHEURE_COUNT (sizeof(ORACLE_DATEHEURE) / sizeof(ORACLE_DATEHEURE[0]))

/* ================================================================
 * Oracle Duree — owl_parse_duration_multi
 * Format canon Q5-bis : [+/-]<J>HHMMSSCC ou [+/-]<J>HHMMSSCCC.
 * Signe obligatoire (rupture  vs  strict 10 digits).
 * ================================================================ */
static const TemporalCase ORACLE_DUREE[] = {
    /* --- Valides --- */
    {"+0000000000",    -1, KIND_OK, "zero positif (10 chars, mode CC)"},
    {"+00000000000",   -1, KIND_OK, "zero positif (11 chars, mode CCC)"},
    {"-0000000000",    -1, KIND_OK, "zero negatif (10 chars)"},
    /* Composition signe + J (1+) + HHMMSSCC (8 derniers chars).
     *   "+101234560" = J=1, HH=01 MM=23 SS=45 CC=60 (CC pas borne)
     *   "+1101234560" = J=11, HH=01 MM=23 SS=45 CC=60
     *   "-101234560"  = idem positif mais signe negatif. */
    {"+101234560",     -1, KIND_OK, "J=1 01h23m45s CC=60 (9 chars apres signe)"},
    {"+1101234560",    -1, KIND_OK, "J=11 01h23m45s CC=60 (10 chars apres signe)"},
    {"-101234560",     -1, KIND_OK, "negatif J=1 01h23m45s CC=60"},
    /* --- Format invalide --- */
    {"",               -1, KIND_ERR_FORMAT, "chaine vide"},
    {"0000000000",     -1, KIND_ERR_FORMAT, "signe manquant (10 chars)"},
    {"abc",            -1, KIND_ERR_FORMAT, "non-digits"},
    {"+",              -1, KIND_ERR_FORMAT, "signe seul"},
    {"+abc",           -1, KIND_ERR_FORMAT, "non-digits apres signe"},
    {"+12345",         -1, KIND_ERR_FORMAT, "trop court apres signe (5 chars)"},
    {"+123456",        -1, KIND_ERR_FORMAT, "trop court apres signe (6 chars)"},
    {"+1234567",       -1, KIND_ERR_FORMAT, "trop court apres signe (7 chars, j_chars<1)"},
    /* Note : le parser actuel accepte des longueurs >= 10 (J prend
     * les chars residuels du debut). Une chaine "+123456789012"
     * (12 chars apres signe) est lue J='1234' + HHMMSSCC='56789012'
     * -> HH=56 hors borne -> BOUNDS, pas FORMAT. */
    /* --- Bornes invalides --- */
    /* Composition : signe + J + HHMMSSCC (8 chars fixes en fin).
     *   "+125000000" = J=1, HH=25, MM=00, SS=00, CC=00 -> HH>23 BOUNDS
     *   "+123600000" = J=1, HH=23, MM=60, SS=00, CC=00 -> MM>59 BOUNDS
     *   "+123596000" = J=1, HH=23, MM=59, SS=60, CC=00 -> SS>59 BOUNDS
     *   "+225000000" = J=2, HH=25, MM=00, SS=00, CC=00 -> HH>23 BOUNDS (J multi-digit ne suffit pas a sauver) */
    {"+125000000",     -1, KIND_ERR_BOUNDS, "J=1 HH=25 hors borne"},
    {"+123600000",     -1, KIND_ERR_BOUNDS, "J=1 HH=23 MM=60 hors borne"},
    {"+123596000",     -1, KIND_ERR_BOUNDS, "J=1 HH=23 MM=59 SS=60 hors borne"},
    {"+225000000",     -1, KIND_ERR_BOUNDS, "J=2 HH=25 hors borne (10 chars apres signe)"},
};
#define ORACLE_DUREE_COUNT (sizeof(ORACLE_DUREE) / sizeof(ORACLE_DUREE[0]))

/* ================================================================
 * Dispatch : invoke le parser approprie pour chaque famille, compare
 * a l'oracle (succes attendu + classe d'erreur attendue).
 *
 * Retourne 0 si OK, 1 si violation. Imprime un detail sur stderr en
 * cas de divergence.
 * ================================================================ */

static int check_date(const TemporalCase* c, FILE* out) {
    int y, m, d, perr = 0;
    int len = case_len(c);
    bool ok = owl_parse_date_multi(c->input, len, &y, &m, &d, &perr);
    bool match = false;
    switch (c->kind) {
        case KIND_OK:           match = ok && perr == OWL_DATE_PARSE_OK; break;
        case KIND_ERR_FORMAT:   match = !ok && perr == OWL_DATE_PARSE_ERR_FORMAT; break;
        case KIND_ERR_BOUNDS:   match = !ok && perr == OWL_DATE_PARSE_ERR_BOUNDS; break;
    }
    if (!match) {
        fprintf(out, "  Date: input=\"%s\" desc=\"%s\" attendu=%s got ok=%d perr=%d\n",
                c->input, c->description,
                c->kind == KIND_OK ? "OK" : c->kind == KIND_ERR_FORMAT ? "ERR_FORMAT" : "ERR_BOUNDS",
                ok ? 1 : 0, perr);
        return 1;
    }
    return 0;
}

static int check_heure(const TemporalCase* c, FILE* out) {
    int h, m, sec, cs, perr = 0;
    int len = case_len(c);
    bool ok = owl_parse_time_string_cs_ex(c->input, len, &h, &m, &sec, &cs, &perr);
    bool match = false;
    switch (c->kind) {
        case KIND_OK:           match = ok && perr == OWL_DATE_PARSE_OK; break;
        case KIND_ERR_FORMAT:   match = !ok && perr == OWL_DATE_PARSE_ERR_FORMAT; break;
        case KIND_ERR_BOUNDS:   match = !ok && perr == OWL_DATE_PARSE_ERR_BOUNDS; break;
    }
    if (!match) {
        fprintf(out, "  Heure: input=\"%s\" desc=\"%s\" attendu=%s got ok=%d perr=%d\n",
                c->input, c->description,
                c->kind == KIND_OK ? "OK" : c->kind == KIND_ERR_FORMAT ? "ERR_FORMAT" : "ERR_BOUNDS",
                ok ? 1 : 0, perr);
        return 1;
    }
    return 0;
}

static int check_dateheure(const TemporalCase* c, FILE* out) {
    int y, m, d, hh, mm, ss, perr = 0;
    int len = case_len(c);
    bool ok = owl_parse_dateheure_iso_ex(c->input, len, &y, &m, &d, &hh, &mm, &ss, &perr);
    bool match = false;
    switch (c->kind) {
        case KIND_OK:           match = ok && perr == OWL_DATE_PARSE_OK; break;
        case KIND_ERR_FORMAT:   match = !ok && perr == OWL_DATE_PARSE_ERR_FORMAT; break;
        case KIND_ERR_BOUNDS:   match = !ok && perr == OWL_DATE_PARSE_ERR_BOUNDS; break;
    }
    if (!match) {
        fprintf(out, "  DateHeure: input=\"%s\" desc=\"%s\" attendu=%s got ok=%d perr=%d\n",
                c->input, c->description,
                c->kind == KIND_OK ? "OK" : c->kind == KIND_ERR_FORMAT ? "ERR_FORMAT" : "ERR_BOUNDS",
                ok ? 1 : 0, perr);
        return 1;
    }
    return 0;
}

static int check_duree(const TemporalCase* c, FILE* out) {
    int64_t ms = 0;
    int perr = 0;
    int len = case_len(c);
    bool ok = owl_parse_duration_multi(c->input, len, &ms, &perr);
    bool match = false;
    switch (c->kind) {
        case KIND_OK:           match = ok && perr == OWL_DATE_PARSE_OK; break;
        case KIND_ERR_FORMAT:   match = !ok && perr == OWL_DATE_PARSE_ERR_FORMAT; break;
        case KIND_ERR_BOUNDS:   match = !ok && perr == OWL_DATE_PARSE_ERR_BOUNDS; break;
    }
    if (!match) {
        fprintf(out, "  Duree: input=\"%s\" desc=\"%s\" attendu=%s got ok=%d perr=%d\n",
                c->input, c->description,
                c->kind == KIND_OK ? "OK" : c->kind == KIND_ERR_FORMAT ? "ERR_FORMAT" : "ERR_BOUNDS",
                ok ? 1 : 0, perr);
        return 1;
    }
    return 0;
}

/* ================================================================
 * Entry point. Itere les 4 oracles, accumule les violations, imprime
 * un resume final.
 * ================================================================ */
int temporal_parity_self_test_run(FILE* out) {
    int viol = 0;
    int checked = 0;

    fprintf(out, "{\"self_test\":\"temporal_parity\",\"violations\":[\n");
    int first = 1;
    (void)first;

    for (size_t i = 0; i < ORACLE_DATE_COUNT; i++) {
        if (check_date(&ORACLE_DATE[i], out)) viol++;
        checked++;
    }
    for (size_t i = 0; i < ORACLE_HEURE_COUNT; i++) {
        if (check_heure(&ORACLE_HEURE[i], out)) viol++;
        checked++;
    }
    for (size_t i = 0; i < ORACLE_DATEHEURE_COUNT; i++) {
        if (check_dateheure(&ORACLE_DATEHEURE[i], out)) viol++;
        checked++;
    }
    for (size_t i = 0; i < ORACLE_DUREE_COUNT; i++) {
        if (check_duree(&ORACLE_DUREE[i], out)) viol++;
        checked++;
    }

    fprintf(out, "],\"checked\":%d,\"violation_count\":%d}\n", checked, viol);

    /* Garde ensemble vide : `checked` etait affiche, jamais
     * verifie. Quatre tables d'oracles vides rendaient "violation_count: 0"
     * sans avoir compare une seule valeur. */
    if (checked == 0) {
        fprintf(stderr, "temporal-parity: CORPUS VIDE — 0 oracle compare. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return viol + 1;
    }
    return viol;
}
