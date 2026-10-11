// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_date_parsers.c - dispatcher temporel multi-format + variantes
 * BOUNDS-aware. Compagnon de stdlib_date_parsers.h (cree ,
 * etendu  lot 1).
 *
 *  lot 1 — la coerce runtime et le compile filter passent
 * par owl_parse_date_multi (Q5 : accepte AAAAMMJJ + JJ/MM/AAAA +
 * JJ-MM-AAAA + AAAA-MM-JJ + AAAA/MM/JJ). Les variantes _ex
 * distinguent format invalide vs borne calendaire invalide pour
 * remonter E_SEM_*_LITERAL_FORMAT vs E_SEM_*_LITERAL_BOUNDS.
 *
 * Les wrappers historiques (owl_parse_date_string, etc.) restent
 * dans stdlib_date.c — pas de big-bang. Ce fichier ne contient que
 * les nouvelles APIs lot 1. */

#include "stdlib_date_parsers.h"

#include <stdbool.h>

/* days_in_month duplique du module stdlib_date.c. Sans extern dispo,
 * on reimplemente localement. Source unique au sens semantique :
 * meme regle gregorienne, valide en self-test parity. */
static int dp_days_in_month(int y, int m) {
    static const int dim[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
        return 29;
    return dim[m];
}

/* Helper : tous les chars sont digits ? */
static bool dp_all_digits(const char* s, int from, int to) {
    for (int i = from; i < to; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

/* ================================================================
 * Q5 — Dispatcher multi-format Date.
 *
 * Format valide acceptes :
 *   - AAAAMMJJ            (8 chiffres strict, len=8)
 *   - JJ/MM/AAAA          (len=10, sep slash positions 2,5)
 *   - JJ-MM-AAAA          (len=10, sep tiret positions 2,5)
 *   - AAAA-MM-JJ          (len=10, ISO sep tiret positions 4,7)
 *   - AAAA/MM/JJ          (len=10, ISO sep slash positions 4,7)
 *
 * Distinction critique outErr :
 *   - format passe ET bornes OK    => OK
 *   - format passe ET bornes KO    => ERR_BOUNDS (ex: 32/01/2025, 01/13/2025)
 *   - format ne passe pas          => ERR_FORMAT (ex: "midi", "01-Jan-2025")
 *
 * Note : "12/31/2025" (US) est ERR_BOUNDS — la longueur 10 + separateurs
 * positions 2,5 matchent JJ/MM/AAAA, mais m=31 hors-borne. C'est cette
 * regle qui pose ErreurDetectee sur format US (cf. Bloc A oracle).
 * ================================================================ */

bool owl_parse_date_multi(const char* s, int len,
                          int* y, int* m, int* d, int* outErr) {
    if (outErr) *outErr = OWL_DATE_PARSE_ERR_FORMAT;
    *y = *m = *d = 0;

    /* Branche 1 : AAAAMMJJ (8 digits strict). */
    if (len == 8) {
        if (!dp_all_digits(s, 0, 8)) return false;
        *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
        *m = (s[4]-'0')*10 + (s[5]-'0');
        *d = (s[6]-'0')*10 + (s[7]-'0');
        if (*m < 1 || *m > 12 || *d < 1 || *d > dp_days_in_month(*y, *m)) {
            if (outErr) *outErr = OWL_DATE_PARSE_ERR_BOUNDS;
            return false;
        }
        if (outErr) *outErr = OWL_DATE_PARSE_OK;
        return true;
    }

    /* Branche 2 : 10 chars avec separateurs. */
    if (len == 10) {
        /* JJ[/-]MM[/-]AAAA : separateurs en positions 2 et 5, symetriques. */
        if ((s[2] == '/' || s[2] == '-') && s[2] == s[5]) {
            if (!dp_all_digits(s, 0, 2) || !dp_all_digits(s, 3, 5) ||
                !dp_all_digits(s, 6, 10)) return false;
            *d = (s[0]-'0')*10 + (s[1]-'0');
            *m = (s[3]-'0')*10 + (s[4]-'0');
            *y = (s[6]-'0')*1000 + (s[7]-'0')*100 + (s[8]-'0')*10 + (s[9]-'0');
            if (*m < 1 || *m > 12 || *d < 1 || *d > dp_days_in_month(*y, *m)) {
                if (outErr) *outErr = OWL_DATE_PARSE_ERR_BOUNDS;
                return false;
            }
            if (outErr) *outErr = OWL_DATE_PARSE_OK;
            return true;
        }
        /* AAAA[/-]MM[/-]JJ : separateurs en positions 4 et 7, ISO style.
         * Mutuellement exclusif avec JJ[/-]MM[/-]AAAA (positions distinctes). */
        if ((s[4] == '/' || s[4] == '-') && s[4] == s[7]) {
            if (!dp_all_digits(s, 0, 4) || !dp_all_digits(s, 5, 7) ||
                !dp_all_digits(s, 8, 10)) return false;
            *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
            *m = (s[5]-'0')*10 + (s[6]-'0');
            *d = (s[8]-'0')*10 + (s[9]-'0');
            if (*m < 1 || *m > 12 || *d < 1 || *d > dp_days_in_month(*y, *m)) {
                if (outErr) *outErr = OWL_DATE_PARSE_ERR_BOUNDS;
                return false;
            }
            if (outErr) *outErr = OWL_DATE_PARSE_OK;
            return true;
        }
    }

    /* Toute autre forme : format invalide. */
    return false;
}

/* ================================================================
 * Variante Heure BOUNDS-aware.
 *
 * Formats acceptes (memes que owl_parse_time_string_cs) :
 *   - HH        (2 digits)
 *   - HHMM      (4 digits)
 *   - HHMMSS    (6 digits)
 *   - HHMMSSCC  (8 digits)
 *   - HH:MM           (5 chars, sep ':')
 *   - HH:MM:SS        (8 chars, sep ':')
 *   - HH:MM:SS:CC     (11 chars, sep ':')
 *   - HH:MM:SS.CC     (11 chars, sep '.')
 *
 * Format ok mais HH/MM/SS/CC hors borne -> ERR_BOUNDS (ex: "24:00:00").
 * ================================================================ */

bool owl_parse_time_string_cs_ex(const char* s, int len,
                                 int* h, int* m, int* sec, int* cs,
                                 int* outErr) {
    if (outErr) *outErr = OWL_DATE_PARSE_ERR_FORMAT;
    *h = *m = *sec = *cs = 0;
    if (len < 2) return false;

    /* Forme avec separateur ':' (positions 2). */
    if (len >= 5 && s[2] == ':') {
        if (!dp_all_digits(s, 0, 2) || !dp_all_digits(s, 3, 5)) return false;
        *h = (s[0]-'0')*10 + (s[1]-'0');
        *m = (s[3]-'0')*10 + (s[4]-'0');
        if (len == 5) {
            /* HH:MM ok. */
        } else if (len >= 8 && (s[5] == ':' || s[5] == '.')) {
            if (!dp_all_digits(s, 6, 8)) return false;
            *sec = (s[6]-'0')*10 + (s[7]-'0');
            if (len == 8) {
                /* HH:MM:SS ok. */
            } else if (len == 11 && (s[8] == ':' || s[8] == '.')) {
                if (!dp_all_digits(s, 9, 11)) return false;
                *cs = (s[9]-'0')*10 + (s[10]-'0');
            } else {
                return false;
            }
        } else {
            return false;
        }
        if (*h > 23 || *m > 59 || *sec > 59 || *cs > 99) {
            if (outErr) *outErr = OWL_DATE_PARSE_ERR_BOUNDS;
            return false;
        }
        if (outErr) *outErr = OWL_DATE_PARSE_OK;
        return true;
    }

    /* Forme compacte : HH, HHMM, HHMMSS, HHMMSSCC. */
    if (len != 2 && len != 4 && len != 6 && len != 8) return false;
    if (!dp_all_digits(s, 0, len)) return false;
    *h = (s[0]-'0')*10 + (s[1]-'0');
    if (len >= 4) *m   = (s[2]-'0')*10 + (s[3]-'0');
    if (len >= 6) *sec = (s[4]-'0')*10 + (s[5]-'0');
    if (len >= 8) *cs  = (s[6]-'0')*10 + (s[7]-'0');
    if (*h > 23 || *m > 59 || *sec > 59 || *cs > 99) {
        if (outErr) *outErr = OWL_DATE_PARSE_ERR_BOUNDS;
        return false;
    }
    if (outErr) *outErr = OWL_DATE_PARSE_OK;
    return true;
}

/* ================================================================
 * Variante DateHeure ISO BOUNDS-aware.
 *
 * Formats acceptes :
 *   - "AAAA[-/]MM[-/]JJ"            (10 chars, date seule)
 *   - "AAAA[-/]MM[-/]JJ[ T]HH:MM"   (16 chars)
 *   - "AAAA[-/]MM[-/]JJ[ T]HH:MM:SS"(19 chars)
 *
 * Note : len 10 (date seule) est explicitement INVALIDE pour
 * DateHeure (cf. Bloc A test "date seule invalide" — coerce
 * dateheure doit refuser). On accepte ici (parser permissif), c'est
 * le caller (owl_parse_dateheure_iso_ex via compile filter) qui peut
 * decider de rejeter. Mais le runtime coerce dateheure refuse deja.
 *
 * On garde meme regle : len 10 OK pour parser, rejete par caller si
 * besoin (compile filter rejette len < 16 ici).
 * ================================================================ */

bool owl_parse_dateheure_iso_ex(const char* s, int len,
                                int* y, int* m, int* d,
                                int* hh, int* mm, int* ss,
                                int* outErr) {
    if (outErr) *outErr = OWL_DATE_PARSE_ERR_FORMAT;
    *y = *m = *d = 0; *hh = *mm = *ss = 0;

    /* Pour DateHeure (vs Date pure), on exige une partie horaire.
     * len 10 (date seule) est refuse format-wise pour ce parser. */
    if (len != 16 && len != 19) return false;

    /* Separateur date : positions 4 et 7, symetriques. */
    char dsep = s[4];
    if ((dsep != '-' && dsep != '/') || s[7] != dsep) return false;
    if (!dp_all_digits(s, 0, 4) || !dp_all_digits(s, 5, 7) ||
        !dp_all_digits(s, 8, 10)) return false;
    *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    *m = (s[5]-'0')*10 + (s[6]-'0');
    *d = (s[8]-'0')*10 + (s[9]-'0');

    /* Separateur date/heure : position 10, ' ' ou 'T'. */
    if (s[10] != ' ' && s[10] != 'T') return false;

    /* Heure : positions 11-12, separateur ':' position 13, minutes 14-15. */
    if (!dp_all_digits(s, 11, 13) || s[13] != ':' ||
        !dp_all_digits(s, 14, 16)) return false;
    *hh = (s[11]-'0')*10 + (s[12]-'0');
    *mm = (s[14]-'0')*10 + (s[15]-'0');

    /* Secondes optionnelles : len 19 => ':' + 2 digits. */
    if (len == 19) {
        if (s[16] != ':' || !dp_all_digits(s, 17, 19)) return false;
        *ss = (s[17]-'0')*10 + (s[18]-'0');
    }

    /* Bornes calendaires + horaires. */
    if (*m < 1 || *m > 12 || *d < 1 || *d > dp_days_in_month(*y, *m) ||
        *hh > 23 || *mm > 59 || *ss > 59) {
        if (outErr) *outErr = OWL_DATE_PARSE_ERR_BOUNDS;
        return false;
    }
    if (outErr) *outErr = OWL_DATE_PARSE_OK;
    return true;
}

/* ================================================================
 * Q5-bis — Dispatcher multi-format Duree.
 *
 * Layout : [+/-]<J digits greedy>HHMMSSCC (mode centieme, 8 chars
 * fixes apres J). Signe obligatoire (, alpha break-net
 * vs  historique JHHMMSSCCC). Mode millieme accessible via
 * native ChaineVersDuree(s, dureeMillieme) qui passe par le format
 * template explicite, pas par ce dispatcher.
 *
 * outErr :
 *   - signe manquant / non-digit / longueur < 10 => FORMAT
 *   - HH>=24 || MM>=60 || SS>=60                 => BOUNDS
 * ================================================================ */

bool owl_parse_duration_multi(const char* s, int len,
                              int64_t* out_ms, int* outErr) {
    if (outErr) *outErr = OWL_DATE_PARSE_ERR_FORMAT;
    if (out_ms) *out_ms = 0;

    if (len < 10) return false;  /* signe + 9 chars min (J=1 + HHMMSSCC) */
    int sign;
    if (s[0] == '+') sign = 1;
    else if (s[0] == '-') sign = -1;
    else return false;

    int rest = len - 1;
    int j_chars = rest - 8;
    if (j_chars < 1) return false;

    /* Tous les chars apres signe doivent etre digits. */
    for (int i = 1; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }

    int64_t J = 0;
    for (int i = 1; i < 1 + j_chars; i++) {
        J = J * 10 + (s[i] - '0');
    }
    int base = 1 + j_chars;
    int HH = (s[base]-'0')*10 + (s[base+1]-'0');
    int MM = (s[base+2]-'0')*10 + (s[base+3]-'0');
    int SS = (s[base+4]-'0')*10 + (s[base+5]-'0');
    int CC = (s[base+6]-'0')*10 + (s[base+7]-'0');

    if (HH > 23 || MM > 59 || SS > 59) {
        if (outErr) *outErr = OWL_DATE_PARSE_ERR_BOUNDS;
        return false;
    }

    int64_t total = J * 86400000LL
                  + (int64_t)HH * 3600000LL
                  + (int64_t)MM * 60000LL
                  + (int64_t)SS * 1000LL
                  + (int64_t)CC * 10LL;
    if (out_ms) *out_ms = sign * total;
    if (outErr) *outErr = OWL_DATE_PARSE_OK;
    return true;
}
