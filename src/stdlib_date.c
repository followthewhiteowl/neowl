// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"

/* skip section compile-aware d'owl_errors.h pour eviter la collision
 * TokenType (scanner.h vs winnt.h tire par windows.h plus bas). */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "owl_registry.h"
#include "memory.h"
#include "vm.h"  /* vm_push/vm_pop pour GC roots transitoires ( lot 6) */
#include "stdlib_date_parsers.h"  /*  Q5 — dispatcher multi-format */
#include <time.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * DATE ARITHMETIC HELPERS
 * Julian Day Number conversion for robust date math.
 * ================================================================ */

/* Convert y/m/d to Julian Day Number (valid for all Gregorian dates) */
static int64_t date_to_jdn(int y, int m, int d) {
    int a = (14 - m) / 12;
    int yy = y + 4800 - a;
    int mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

/* Convert Julian Day Number back to y/m/d */
static void jdn_to_date(int64_t jdn, int* y, int* m, int* d) {
    int64_t a = jdn + 32044;
    int64_t b = (4 * a + 3) / 146097;
    int64_t c = a - (146097 * b) / 4;
    int64_t dd = (4 * c + 3) / 1461;
    int64_t e = c - (1461 * dd) / 4;
    int64_t mm = (5 * e + 2) / 153;
    *d = (int)(e - (153 * mm + 2) / 5 + 1);
    *m = (int)(mm + 3 - 12 * (mm / 10));
    *y = (int)(100 * b + dd - 4800 + mm / 10);
}

/* Days in month (handles leap years) */
static int days_in_month(int y, int m) {
    static const int dim[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
        return 29;
    return dim[m];
}

/* Day of week: 0=Monday ... 6=Sunday (ISO) */
static int day_of_week(int y, int m, int d) {
    int64_t jdn = date_to_jdn(y, m, d);
    return (int)((jdn + 0) % 7); /* 0=Mon */
}

/* Parse "YYYYMMDD" string to y/m/d. Returns false on failure. */
static bool parse_date_string(const char* s, int len, int* y, int* m, int* d) {
    if (len != 8) return false;
    for (int i = 0; i < 8; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    *m = (s[4]-'0')*10 + (s[5]-'0');
    *d = (s[6]-'0')*10 + (s[7]-'0');
    return (*m >= 1 && *m <= 12 && *d >= 1 && *d <= days_in_month(*y, *m));
}

/* Parse "HHMMSS" or "HHMM" to h/m/s */
/* Parse HHMMSSCC (8), HHMMSS (6), HHMM (4). Returns centièmes in *cs. */
static bool parse_time_string(const char* s, int len, int* h, int* m, int* sec) {
    *h = *m = *sec = 0;
    /* canon : HH (2), HHMM (4), HHMMSS (6), HHMMSSCC (8). Toute longueur
     * paire entre 2 et 8 acceptee, les composants manquants restent a 0. */
    if (len != 2 && len != 4 && len != 6 && len != 8) return false;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    *h = (s[0]-'0')*10 + (s[1]-'0');
    if (len >= 4) *m   = (s[2]-'0')*10 + (s[3]-'0');
    if (len >= 6) *sec = (s[4]-'0')*10 + (s[5]-'0');
    return (*h >= 0 && *h <= 23 && *m >= 0 && *m <= 59 && *sec >= 0 && *sec <= 59);
}

/* Accept: HHMMSSCC / HHMMSS / HHMM (solid digits)
 *         HH:MM:SS:CC / HH:MM:SS / HH:MM (colon separated)
 *         HH:MM:SS.CC (dot for centièmes) */
static bool parse_time_string_cs(const char* s, int len, int* h, int* m, int* sec, int* cs) {
    *h = *m = *sec = *cs = 0;
    if (len < 2) return false;

    /* Separator-based form: HH:MM[:SS[:CC|.CC]] */
    if (len >= 5 && s[2] == ':') {
        if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') return false;
        if (s[3] < '0' || s[3] > '9' || s[4] < '0' || s[4] > '9') return false;
        *h = (s[0]-'0')*10 + (s[1]-'0');
        *m = (s[3]-'0')*10 + (s[4]-'0');
        if (len == 5) {
            /* HH:MM */
        } else if (len >= 8 && (s[5] == ':' || s[5] == '.')) {
            if (s[6] < '0' || s[6] > '9' || s[7] < '0' || s[7] > '9') return false;
            *sec = (s[6]-'0')*10 + (s[7]-'0');
            if (len == 8) {
                /* HH:MM:SS */
            } else if (len >= 11 && (s[8] == ':' || s[8] == '.')) {
                if (s[9] < '0' || s[9] > '9' || s[10] < '0' || s[10] > '9') return false;
                *cs = (s[9]-'0')*10 + (s[10]-'0');
                if (len != 11) return false;
            } else {
                return false;
            }
        } else {
            return false;
        }
        return (*h <= 23 && *m <= 59 && *sec <= 59 && *cs <= 99);
    }

    /* Solid digit form: HHMM / HHMMSS / HHMMSSCC */
    if (!parse_time_string(s, len, h, m, sec)) return false;
    if (len >= 8) *cs = (s[6]-'0')*10 + (s[7]-'0');
    return (*cs >= 0 && *cs <= 99);
}

/* ================================================================
 * NATIVE FUNCTIONS
 * ================================================================ */

/* DateSys() / Today() - current date as ObjDate */
/* DateSys([<chaîne AAAAMMJJ>]) — getter par défaut.
 * La forme avec argument canon est censée muter la date système (privilège admin).
 * En OWL on accepte l'argument pour conformité d'API mais on no-op : on ne
 * change pas la date système (sécurité, comportement non testable en CI).
 * Divergence documentée — le getter reste authoritative. */
static Value fn_date_sys(int argCount, Value* args) {
    (void)argCount; (void)args;
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    return OBJ_VAL(obj_new_date(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday));
}

/* HeureSys([<chaîne HHMMSS>]) / Now() — idem DateSys (setter no-op). */
static Value fn_heure_sys(int argCount, Value* args) {
    (void)argCount; (void)args;
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    return OBJ_VAL(obj_new_time(tm->tm_hour, tm->tm_min, tm->tm_sec, 0));
}

/* DateHeureSys([<chaîne AAAAMMJJHHMMSS>]) / DateTimeNow() — idem (setter no-op). */
static Value fn_dateheure_sys(int argCount, Value* args) {
    (void)argCount; (void)args;
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    return OBJ_VAL(obj_new_datetime(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                                     tm->tm_hour, tm->tm_min, tm->tm_sec, 0));
}

/* Constants for maskDate* (doc 3027013) : sentinelles centralisees
 * dans sentinels.h. */
#include "sentinels.h"

/* Month name tables (EN 3-letter for RFC-5322, FR 3-letter & full for canon). */
static const char* const MONTHS_EN3[12] = {
    "jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"
};
static const char* const MONTHS_FR3[12] = {
    "jan","fév","mar","avr","mai","jui","jui","aoû","sep","oct","nov","déc"
};
static const char* const MONTHS_FR_FULL[12] = {
    "janvier","février","mars","avril","mai","juin",
    "juillet","août","septembre","octobre","novembre","décembre"
};
static const char* const DAYS_FR3[7] = {
    "lun","mar","mer","jeu","ven","sam","dim"
};
static const char* const DAYS_FR_FULL[7] = {
    "lundi","mardi","mercredi","jeudi","vendredi","samedi","dimanche"
};

/* Parse 3-letter month (English RFC-5322 or French canon). */
static int month_name_to_num(const char* s) {
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(s, MONTHS_EN3[i], 3) == 0) return i + 1;
    }
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(s, MONTHS_FR3[i], 3) == 0) return i + 1;
    }
    return 0;
}

/* Parse a month full name (FR, longer-first). */
static int month_parse_full(const char* s, int slen, int* consumed) {
    for (int i = 0; i < 12; i++) {
        int fl = (int)strlen(MONTHS_FR_FULL[i]);
        if (slen >= fl && strncasecmp(s, MONTHS_FR_FULL[i], fl) == 0) {
            *consumed = fl;
            return i + 1;
        }
    }
    return 0;
}

/* Parse a month 3-letter (English RFC-5322 or French canon, handles UTF-8 like "déc"). */
static int month_parse_3letter(const char* s, int slen, int* consumed) {
    /* Try FR 3-letter (may be > 3 bytes because of UTF-8 accents) */
    for (int i = 0; i < 12; i++) {
        int fl = (int)strlen(MONTHS_FR3[i]);
        if (slen >= fl && strncasecmp(s, MONTHS_FR3[i], fl) == 0) {
            *consumed = fl;
            return i + 1;
        }
    }
    /* Try EN 3-letter (always 3 ASCII bytes) */
    if (slen >= 3) {
        for (int i = 0; i < 12; i++) {
            if (strncasecmp(s, MONTHS_EN3[i], 3) == 0) {
                *consumed = 3;
                return i + 1;
            }
        }
    }
    return 0;
}

/* Parse YYYYMMDD or auto-detect common formats (JJ/MM/AAAA, JJ-MM-AAAA, AAAA-MM-JJ, AAAA/MM/JJ).
 * Default format when no mask provided = JJ/MM/AAAA (doc).
 * La forme ISO AAAA-MM-JJ (et son pendant slash) est une extension OWL — cf. . */
static bool parse_date_auto(const char* s, int len, int* y, int* m, int* d) {
    /* YYYYMMDD (8 digits) */
    if (len == 8) {
        bool alldig = true;
        for (int i = 0; i < 8; i++) if (s[i] < '0' || s[i] > '9') { alldig = false; break; }
        if (alldig) {
            *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
            *m = (s[4]-'0')*10 + (s[5]-'0');
            *d = (s[6]-'0')*10 + (s[7]-'0');
            return (*m >= 1 && *m <= 12 && *d >= 1 && *d <= days_in_month(*y, *m));
        }
    }
    /* JJ/MM/AAAA or JJ-MM-AAAA */
    if (len == 10 && (s[2] == '/' || s[2] == '-') && s[2] == s[5]) {
        bool ok = true;
        for (int i = 0; i < 10; i++) {
            if (i == 2 || i == 5) continue;
            if (s[i] < '0' || s[i] > '9') { ok = false; break; }
        }
        if (ok) {
            *d = (s[0]-'0')*10 + (s[1]-'0');
            *m = (s[3]-'0')*10 + (s[4]-'0');
            *y = (s[6]-'0')*1000 + (s[7]-'0')*100 + (s[8]-'0')*10 + (s[9]-'0');
            return (*m >= 1 && *m <= 12 && *d >= 1 && *d <= days_in_month(*y, *m));
        }
    }
    /* AAAA-MM-JJ or AAAA/MM/JJ (ISO 8601 date-only, extension OWL — ).
     * Distinction sans ambiguite avec JJ/MM/AAAA : separateurs en positions 4 et 7
     * (vs 2 et 5), donc les deux branches sont mutuellement exclusives. */
    if (len == 10 && (s[4] == '/' || s[4] == '-') && s[4] == s[7]) {
        bool ok = true;
        for (int i = 0; i < 10; i++) {
            if (i == 4 || i == 7) continue;
            if (s[i] < '0' || s[i] > '9') { ok = false; break; }
        }
        if (ok) {
            *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
            *m = (s[5]-'0')*10 + (s[6]-'0');
            *d = (s[8]-'0')*10 + (s[9]-'0');
            return (*m >= 1 && *m <= 12 && *d >= 1 && *d <= days_in_month(*y, *m));
        }
    }
    return false;
}

/* Parse une chaine ISO datetime vers composants (y, m, d, hh, mm, ss).
 * Extension OWL — extension naturelle d' (qui couvre la partie date).
 * Formats reconnus :
 *   - "AAAA-MM-JJ HH:MM:SS"  (19 chars, separateur espace)
 *   - "AAAA-MM-JJTHH:MM:SS"  (19 chars, T-separator ISO8601 strict)
 *   - "AAAA/MM/JJ HH:MM:SS"  (19 chars, slash variant)
 *   - "AAAA-MM-JJ HH:MM"     (16 chars, sans secondes -> ss=0)
 *   - "AAAA/MM/JJ HH:MM"     (16 chars, slash sans secondes)
 *   - "AAAA-MM-JJ"           (10 chars, date seule -> hh=mm=ss=0)
 *   - "AAAA/MM/JJ"           (10 chars, slash date seule)
 *
 * Valide la FORME (positions de separateurs, digits aux bonnes positions)
 * mais PAS les plages calendaires. Cf. fn_dateheure_coerce qui doit accepter
 * "2023-02-29 12:00:00" (parse OK) et laisser DateHeureValide / dh_components_valid
 * detecter la non-validite (29 fevrier non-bissextile).
 *
 * Hors-scope strict : pas de precision sub-seconde (.FFF), pas de fuseaux
 * horaires (Z, +HH:MM) — c'est maskDateInternet qui couvre ces cas. */
static bool parse_dateheure_iso(const char* s, int len,
                                 int* y, int* m, int* d,
                                 int* hh, int* mm, int* ss) {
    *y = *m = *d = 0; *hh = *mm = *ss = 0;
    if (len != 10 && len != 16 && len != 19) return false;
    /* Separateur date : '-' ou '/' aux positions 4 et 7, symetrie obligatoire. */
    char dsep = s[4];
    if (dsep != '-' && dsep != '/') return false;
    if (s[7] != dsep) return false;
    /* Digits AAAA / MM / JJ. */
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    *m = (s[5]-'0')*10 + (s[6]-'0');
    *d = (s[8]-'0')*10 + (s[9]-'0');

    if (len == 10) return true;

    /* len 16 ou 19 : separateur date/heure (espace ou T), HH:MM[:SS]. */
    char tsep = s[10];
    if (tsep != ' ' && tsep != 'T') return false;
    /* HH a 11-12, ':' a 13, MM a 14-15. */
    if (s[13] != ':') return false;
    for (int i = 11; i < 16; i++) {
        if (i == 13) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    *hh = (s[11]-'0')*10 + (s[12]-'0');
    *mm = (s[14]-'0')*10 + (s[15]-'0');

    if (len == 16) return true;

    /* len == 19 : ':' a 16, SS a 17-18. */
    if (s[16] != ':') return false;
    if (s[17] < '0' || s[17] > '9') return false;
    if (s[18] < '0' || s[18] > '9') return false;
    *ss = (s[17]-'0')*10 + (s[18]-'0');
    return true;
}

/* Try to read N digits starting at s[si] into *out. Returns bytes read or 0 on fail. */
static int read_digits(const char* s, int slen, int si, int want_min, int want_max, int* out) {
    int val = 0;
    int read = 0;
    while (read < want_max && si + read < slen && s[si + read] >= '0' && s[si + read] <= '9') {
        val = val * 10 + (s[si + read] - '0');
        read++;
    }
    if (read < want_min) return 0;
    *out = val;
    return read;
}

/* Check if fmt[fi..] starts with one of the given tokens (NULL-terminated list).
 * Returns token index matched (0-based) or -1. */
static int match_fmt_token(const char* fmt, int flen, int fi, const char* const tokens[]) {
    for (int i = 0; tokens[i] != NULL; i++) {
        int tl = (int)strlen(tokens[i]);
        if (fi + tl <= flen && strncmp(fmt + fi, tokens[i], tl) == 0) return i;
    }
    return -1;
}

/* Parse using canon format string.
 * Supports: AAAA, AA, M, MM, MMM, Mmm, mmm, MMMM, Mmmm, mmmm,
 *           J, JJ, JJJ, Jjj, jjj, JJJJ, Jjjj, jjjj, L,
 *           H, HH, MI (minutes), mm (alias minutes), SS.
 * Literals must match exactly (incl. spaces and punctuation).
 *
 * Day-of-week tokens (JJJ/Jjj/jjj/JJJJ/Jjjj/jjjj/L) are consumed from input
 * but ignored for semantic — the date is determined by JJ/MM/AAAA only,
 * conformément au comportement canon (le jour de la semaine est redondant). */
static bool parse_date_with_format(const char* s, int slen, const char* fmt, int flen,
                                    int* y, int* m, int* d, int* hh, int* mm, int* ss) {
    *y = *m = *d = 0; *hh = *mm = *ss = 0;
    int si = 0, fi = 0;

    /* Longest-first order so "AAAA" beats "AA", "MM" beats "M", etc. */
    static const char* const TOK_YEAR4[]  = { "AAAA", NULL };
    static const char* const TOK_YEAR2[]  = { "AA",   NULL };
    /* Month full names: MMMM (casse locale), Mmmm (1re maj), mmmm (tout min) */
    static const char* const TOK_MONTHF[] = { "MMMM", "Mmmm", "mmmm", NULL };
    /* Month 3 letters */
    static const char* const TOK_MONTH3[] = { "MMM",  "Mmm",  "mmm",  NULL };
    static const char* const TOK_MONTH2[] = { "MM",   NULL };
    static const char* const TOK_MONTH1[] = { "M",    NULL };
    static const char* const TOK_DAYF[]   = { "JJJJ", "Jjjj", "jjjj", NULL };
    static const char* const TOK_DAY3[]   = { "JJJ",  "Jjj",  "jjj",  NULL };
    static const char* const TOK_DAY2[]   = { "JJ",   NULL };
    static const char* const TOK_DAY1[]   = { "J",    NULL };
    static const char* const TOK_DAYL[]   = { "L",    NULL };
    static const char* const TOK_HH[]     = { "HH",   NULL };
    /* `mm` minuscule = minutes (alias de `MI`, symétrie avec le format-side qui
     * disambigue par la casse : MM = mois, mm = minutes). On exclut mmm/mmmm
     * en checkant le caractère suivant : ces tokens nomment des mois. */
    static const char* const TOK_MI[]     = { "MI",   "mm",   NULL };
    static const char* const TOK_SS[]     = { "SS",   NULL };

    while (fi < flen && si < slen) {
        int r = 0;
        int tmp;

        /* Time tokens first — HH/MI/SS take precedence over M (else "MI" would
         * be consumed as M + I literal). */
        if (match_fmt_token(fmt, flen, fi, TOK_HH) >= 0) {
            r = read_digits(s, slen, si, 2, 2, &tmp);
            if (r == 0) return false;
            *hh = tmp; si += r; fi += 2;
            continue;
        }
        if (match_fmt_token(fmt, flen, fi, TOK_MI) >= 0) {
            /* Garde-fou : `mm` suivi d'un `m` est en réalité `mmm`/`mmmm` (mois
             * en lettres). On laisse alors le matcher Month full/3-letters
             * intercepter plus bas. `MI` n'a pas ce conflit (case-sensitive). */
            if (!(fmt[fi] == 'm' && fi + 2 < flen && fmt[fi + 2] == 'm')) {
                r = read_digits(s, slen, si, 2, 2, &tmp);
                if (r == 0) return false;
                *mm = tmp; si += r; fi += 2;
                continue;
            }
        }
        if (match_fmt_token(fmt, flen, fi, TOK_SS) >= 0) {
            r = read_digits(s, slen, si, 2, 2, &tmp);
            if (r == 0) return false;
            *ss = tmp; si += r; fi += 2;
            continue;
        }

        /* Year 4 digits */
        if (match_fmt_token(fmt, flen, fi, TOK_YEAR4) >= 0) {
            r = read_digits(s, slen, si, 4, 4, &tmp);
            if (r == 0) return false;
            *y = tmp; si += r; fi += 4;
            continue;
        }
        /* Year 2 digits (pivot 51: 51-99 → 19xx, 00-50 → 20xx) */
        if (match_fmt_token(fmt, flen, fi, TOK_YEAR2) >= 0) {
            r = read_digits(s, slen, si, 2, 2, &tmp);
            if (r == 0) return false;
            *y = (tmp >= 51) ? 1900 + tmp : 2000 + tmp;
            si += r; fi += 2;
            continue;
        }
        /* Month full name */
        if (match_fmt_token(fmt, flen, fi, TOK_MONTHF) >= 0) {
            int consumed = 0;
            int mo = month_parse_full(s + si, slen - si, &consumed);
            if (mo == 0) return false;
            *m = mo; si += consumed; fi += 4;
            continue;
        }
        /* Month 3 letters (UTF-8 aware for FR accented months) */
        if (match_fmt_token(fmt, flen, fi, TOK_MONTH3) >= 0) {
            int consumed = 0;
            int mo = month_parse_3letter(s + si, slen - si, &consumed);
            if (mo == 0) return false;
            *m = mo; si += consumed; fi += 3;
            continue;
        }
        /* Month 2 digits */
        if (match_fmt_token(fmt, flen, fi, TOK_MONTH2) >= 0) {
            r = read_digits(s, slen, si, 2, 2, &tmp);
            if (r == 0) return false;
            *m = tmp; si += r; fi += 2;
            continue;
        }
        /* Month 1-2 digits (M: zero not displayed) */
        if (match_fmt_token(fmt, flen, fi, TOK_MONTH1) >= 0) {
            r = read_digits(s, slen, si, 1, 2, &tmp);
            if (r == 0) return false;
            *m = tmp; si += r; fi += 1;
            continue;
        }
        /* Day full name (ignored for semantics) */
        if (match_fmt_token(fmt, flen, fi, TOK_DAYF) >= 0) {
            bool found = false;
            for (int i = 0; i < 7; i++) {
                int dl = (int)strlen(DAYS_FR_FULL[i]);
                if (si + dl <= slen && strncasecmp(s + si, DAYS_FR_FULL[i], dl) == 0) {
                    si += dl; found = true; break;
                }
            }
            if (!found) return false;
            fi += 4;
            continue;
        }
        /* Day 3 letters (ignored) */
        if (match_fmt_token(fmt, flen, fi, TOK_DAY3) >= 0) {
            if (si + 2 >= slen) return false;
            bool found = false;
            for (int i = 0; i < 7; i++) {
                if (strncasecmp(s + si, DAYS_FR3[i], 3) == 0) { found = true; break; }
            }
            if (!found) return false;
            si += 3; fi += 3;
            continue;
        }
        /* Day 2 digits */
        if (match_fmt_token(fmt, flen, fi, TOK_DAY2) >= 0) {
            r = read_digits(s, slen, si, 2, 2, &tmp);
            if (r == 0) return false;
            *d = tmp; si += r; fi += 2;
            continue;
        }
        /* Day 1-2 digits */
        if (match_fmt_token(fmt, flen, fi, TOK_DAY1) >= 0) {
            r = read_digits(s, slen, si, 1, 2, &tmp);
            if (r == 0) return false;
            *d = tmp; si += r; fi += 1;
            continue;
        }
        /* Day single letter L (L,M,M,J,V,S,D) — ignored semantically */
        if (match_fmt_token(fmt, flen, fi, TOK_DAYL) >= 0) {
            char c = s[si];
            if (c != 'L' && c != 'l' && c != 'M' && c != 'm' &&
                c != 'J' && c != 'j' && c != 'V' && c != 'v' &&
                c != 'S' && c != 's' && c != 'D' && c != 'd') return false;
            si += 1; fi += 1;
            continue;
        }
        /* Literal separator: must match */
        if (s[si] != fmt[fi]) return false;
        si++; fi++;
    }
    if (fi < flen) return false; /* format not fully consumed */
    if (si < slen) return false; /* input has trailing garbage */
    if (*m < 1 || *m > 12) return false;
    if (*d < 1 || *d > days_in_month(*y, *m)) return false;
    return true;
}

/* Parse RFC-5322: "Sun, 25 Nov 2012 12:11:55 +0100" → y/m/d/h/m/s */
static bool parse_rfc5322(const char* s, int len,
                          int* y, int* m, int* d, int* hh, int* mm, int* ss) {
    /* Skip optional day name "XXX, " */
    int i = 0;
    while (i < len && s[i] != ',' && (s[i] < '0' || s[i] > '9')) i++;
    if (i < len && s[i] == ',') i++;
    while (i < len && s[i] == ' ') i++;

    /* Day (1-2 digits) */
    int day = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { day = day*10 + (s[i]-'0'); i++; }
    if (day < 1 || day > 31) return false;
    while (i < len && s[i] == ' ') i++;

    /* Month (3 letters) */
    if (i + 2 >= len) return false;
    int month = month_name_to_num(s + i);
    if (month == 0) return false;
    i += 3;
    while (i < len && s[i] == ' ') i++;

    /* Year (4 digits) */
    int year = 0;
    int dc = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9' && dc < 4) { year = year*10 + (s[i]-'0'); i++; dc++; }
    if (dc != 4) return false;
    while (i < len && s[i] == ' ') i++;

    /* HH:MM:SS */
    int hour = 0, mn = 0, sec = 0;
    if (i + 7 < len && s[i+2] == ':' && s[i+5] == ':') {
        hour = (s[i]-'0')*10 + (s[i+1]-'0');
        mn   = (s[i+3]-'0')*10 + (s[i+4]-'0');
        sec  = (s[i+6]-'0')*10 + (s[i+7]-'0');
    }

    *y = year; *m = month; *d = day; *hh = hour; *mm = mn; *ss = sec;
    return true;
}

/* Parse RFC-3339 (ISO 8601): "2019-08-25T00:00:00.000+02:00" */
static bool parse_rfc3339(const char* s, int len,
                          int* y, int* m, int* d, int* hh, int* mm, int* ss) {
    *hh = *mm = *ss = 0;
    if (len < 10) return false;
    if (s[4] != '-' || s[7] != '-') return false;
    for (int k = 0; k < 4; k++) if (s[k] < '0' || s[k] > '9') return false;
    *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    *m = (s[5]-'0')*10 + (s[6]-'0');
    *d = (s[8]-'0')*10 + (s[9]-'0');
    if (len >= 19 && (s[10] == 'T' || s[10] == ' ') && s[13] == ':' && s[16] == ':') {
        *hh = (s[11]-'0')*10 + (s[12]-'0');
        *mm = (s[14]-'0')*10 + (s[15]-'0');
        *ss = (s[17]-'0')*10 + (s[18]-'0');
    }
    return (*m >= 1 && *m <= 12 && *d >= 1 && *d <= days_in_month(*y, *m));
}

/* Compute local timezone offset in minutes (e.g. +120 for CEST).
 * Computes the diff between localtime and gmtime decompositions of the
 * same epoch — robust against DST. */
static int local_tz_offset_minutes(int year, int month, int day,
                                   int hour, int minute, int second) {
    struct tm t;
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    if (epoch == (time_t)-1) {
        /* La CRT (Windows notamment) ne représente pas les time_t négatifs :
         * mktime échoue pour toute date pré-1970 (et hors plage post-2038).
         * Renvoyer 0 serait un offset muet, silencieusement faux. On recalcule
         * l'offset saisonnier sur une année de référence sûre post-1970 de
         * MÊME mois/jour/heure : capture l'état DST de la saison, résultat
         * déterministe et poste-agnostique. */
        struct tm ref = t;
        ref.tm_year = 2019 - 1900;  /* année fixe, hors bascule DST ambiguë */
        ref.tm_isdst = -1;
        epoch = mktime(&ref);
        if (epoch == (time_t)-1) return 0;  /* dernier recours */
    }
    struct tm localTm, utcTm;
#ifdef _WIN32
    localtime_s(&localTm, &epoch);
    gmtime_s(&utcTm, &epoch);
#else
    localtime_r(&epoch, &localTm);
    gmtime_r(&epoch, &utcTm);
#endif
    /* Diff between local and UTC decomposition gives the offset. */
    int diffMin = (localTm.tm_hour - utcTm.tm_hour) * 60
                + (localTm.tm_min  - utcTm.tm_min);
    /* Account for day rollover */
    int dayDiff = localTm.tm_mday - utcTm.tm_mday;
    if (dayDiff > 1)  dayDiff = -1;  /* end-of-month wrap */
    if (dayDiff < -1) dayDiff =  1;
    diffMin += dayDiff * 24 * 60;
    return diffMin;
}

/* Format RFC-3339 (maskDateInternet/UTC) — "2019-08-25T17:15:00.000+02:00" */
static int format_rfc3339(char* out, int cap, int y, int m, int d,
                           int hh, int mi, int sec, int ms, bool utc) {
    if (utc) {
        /* Convert local → UTC by subtracting the offset. On n'utilise PAS
         * mktime/gmtime ici : ils échouent pour les dates pré-1970 (qui
         * restaient alors non converties). Arithmétique en secondes via JDN :
         * offset saisonnier correct, pas de branche muette. */
        int off = local_tz_offset_minutes(y, m, d, hh, mi, sec);
        int64_t epoch_jdn = date_to_jdn(1970, 1, 1);
        int64_t total = (date_to_jdn(y, m, d) - epoch_jdn) * 86400LL
                      + hh * 3600 + mi * 60 + sec
                      - (int64_t)off * 60;
        int64_t days = total / 86400, rem = total % 86400;
        if (rem < 0) { days--; rem += 86400; }
        jdn_to_date(epoch_jdn + days, &y, &m, &d);
        hh = (int)(rem / 3600); mi = (int)((rem % 3600) / 60); sec = (int)(rem % 60);
        return snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                        y, m, d, hh, mi, sec, ms);
    }
    int off = local_tz_offset_minutes(y, m, d, hh, mi, sec);
    char sign = off >= 0 ? '+' : '-';
    int absOff = off >= 0 ? off : -off;
    return snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d",
                    y, m, d, hh, mi, sec, ms, sign, absOff/60, absOff%60);
}

/* Format RFC-5322 (maskDateEmail/UTC) — "Mon, 25 Nov 2019 17:15:00 +0100" */
static int format_rfc5322(char* out, int cap, int y, int m, int d,
                           int hh, int mi, int sec, bool utc) {
    static const char* const DAYS_EN3[7] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    static const char* const MONTHS_EN3_CAP[12] = {
        "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
    };
    if (utc) {
        struct tm t;
        t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
        t.tm_hour = hh; t.tm_min = mi; t.tm_sec = sec; t.tm_isdst = -1;
        time_t e = mktime(&t);
        if (e != (time_t)-1) {
            struct tm utcTm;
#ifdef _WIN32
            gmtime_s(&utcTm, &e);
#else
            gmtime_r(&e, &utcTm);
#endif
            y = utcTm.tm_year + 1900; m = utcTm.tm_mon + 1; d = utcTm.tm_mday;
            hh = utcTm.tm_hour; mi = utcTm.tm_min; sec = utcTm.tm_sec;
        }
    }
    int dow = day_of_week(y, m, d); /* 0=Lun..6=Dim */
    /* RFC-5322: 0=Sun..6=Sat — remap from ISO */
    int rfcDow = (dow + 1) % 7;  /* 0=Mon→1=Mon... actually need 0=Sun */
    /* day_of_week returns 0=Lun..6=Dim. RFC needs Mon=0..Sun=6 in our table. */
    int idxDay = dow;  /* table is Mon-first, matches our dow */
    if (idxDay < 0 || idxDay > 6) idxDay = 0;
    int idxMon = (m >= 1 && m <= 12) ? m - 1 : 0;
    char tz[16];
    if (utc) {
        snprintf(tz, sizeof(tz), "+0000");
    } else {
        int off = local_tz_offset_minutes(y, m, d, hh, mi, sec);
        char sign = off >= 0 ? '+' : '-';
        int absOff = off >= 0 ? off : -off;
        snprintf(tz, sizeof(tz), "%c%02d%02d", sign, absOff/60, absOff%60);
    }
    (void)rfcDow;
    return snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d %s",
                    DAYS_EN3[idxDay], d, MONTHS_EN3_CAP[idxMon], y,
                    hh, mi, sec, tz);
}

/* Format relatif "il y a / dans" — maskDateDuréeRelative.
 * Approximation simple en FR. La doc ne précise pas les seuils exacts.
 * Buckets pratiques : seconde, minute, heure, jour, semaine, mois, année.
 * Calcule le diff via JDN (date_to_jdn) au lieu de mktime pour gérer les
 * dates pré-1970 (mktime échoue sur Windows). */
static int format_duree_relative(char* out, int cap, int y, int m, int d,
                                  int hh, int mi, int sec) {
    time_t now = time(NULL);
    struct tm nowTm;
#ifdef _WIN32
    localtime_s(&nowTm, &now);
#else
    localtime_r(&now, &nowTm);
#endif
    int ny = nowTm.tm_year + 1900;
    int nmon = nowTm.tm_mon + 1;
    int nd = nowTm.tm_mday;
    int nhh = nowTm.tm_hour;
    int nmm = nowTm.tm_min;
    int nss = nowTm.tm_sec;

    /* Diff en secondes via JDN — robust pre-1970 */
    int64_t targetSec = date_to_jdn(y, m, d) * 86400LL
                      + (int64_t)hh * 3600 + (int64_t)mi * 60 + sec;
    int64_t nowSec    = date_to_jdn(ny, nmon, nd) * 86400LL
                      + (int64_t)nhh * 3600 + (int64_t)nmm * 60 + nss;
    int64_t diffSec = targetSec - nowSec;
    bool past = diffSec < 0;
    int64_t abs_s = past ? -diffSec : diffSec;

    const char* prefixPast = "il y a ";
    const char* prefixFuture = "dans ";
    const char* prefix = past ? prefixPast : prefixFuture;

    if (abs_s < 60) {
        return snprintf(out, cap, "%s%lld secondes", prefix, (long long)abs_s);
    }
    int64_t mins = abs_s / 60;
    if (mins < 60) {
        return snprintf(out, cap, "%s%lld minute%s", prefix,
                        (long long)mins, mins > 1 ? "s" : "");
    }
    int64_t hours = mins / 60;
    if (hours < 24) {
        return snprintf(out, cap, "%s%lld heure%s", prefix,
                        (long long)hours, hours > 1 ? "s" : "");
    }
    int64_t days = hours / 24;
    if (days < 7) {
        return snprintf(out, cap, "%s%lld jour%s", prefix,
                        (long long)days, days > 1 ? "s" : "");
    }
    if (days < 30) {
        int64_t weeks = days / 7;
        return snprintf(out, cap, "%s%lld semaine%s", prefix,
                        (long long)weeks, weeks > 1 ? "s" : "");
    }
    if (days < 365) {
        int64_t months = days / 30;
        return snprintf(out, cap, "%s%lld mois", prefix, (long long)months);
    }
    int64_t years = days / 365;
    if (years == 1) {
        return snprintf(out, cap, "%s",
                        past ? "l'année dernière" : "l'année prochaine");
    }
    return snprintf(out, cap, "%s%lld ans", prefix, (long long)years);
}

/* Helper: ChaîneVersDate en erreur = retour "" (doc 3027013) + set
 * ErreurDetectée=Vrai (improvement OWL, ne casse pas la conformité canon). */
static Value chaine_vers_date_erreur(const char* input) {
    owl_emit_native_error(E_NATIVE_PARSE_ERROR, "ChaineVersDate",
                          "native", "ChaineVersDate",
                          "value", input,
                          "expected", "date",
                          NULL);
    return OBJ_VAL(obj_copy_string("", 0));
}

/* ChaîneVersDate(s [, format]) / StringToDate — canon
 * Retour: ObjDate (cast implicite vers chaîne AAAAMMJJ via to_string)
 *         OU ObjDateTime si le format contient une heure
 *         "" (chaîne vide) sur erreur de parse
 * Improvement OWL: set ErreurDetectée=Vrai en plus du retour "".
 */
static Value fn_chaine_vers_date(int argCount, Value* args) {
    if (argCount < 1) return chaine_vers_date_erreur("<aucun argument>");
    if (!IS_STRING(args[0])) return chaine_vers_date_erreur("<non-string>");
    ObjString* s = AS_STRING(args[0]);

    /* Empty string → empty string (doc: "ChaîneVersDate('')" renvoie "").
     * Cas explicitement documenté, pas une erreur — on clear. */
    if (s->length == 0) {
        owl_clear_error();
        return OBJ_VAL(obj_copy_string("", 0));
    }

    int y, m, d, hh = 0, mm = 0, ss = 0;
    bool hasTime = false;

    /* Form 2: with format (string or mask constant) */
    if (argCount >= 2) {
        if (IS_STRING(args[1])) {
            ObjString* fmt = AS_STRING(args[1]);
            if (!parse_date_with_format(s->chars, s->length, fmt->chars, fmt->length,
                                         &y, &m, &d, &hh, &mm, &ss)) {
                return chaine_vers_date_erreur(s->chars);
            }
            /* Detect if format has a time token (HH, MI, SS) */
            for (int i = 0; i + 1 < fmt->length; i++) {
                if (strncmp(fmt->chars + i, "HH", 2) == 0 ||
                    strncmp(fmt->chars + i, "MI", 2) == 0 ||
                    strncmp(fmt->chars + i, "SS", 2) == 0) {
                    hasTime = true; break;
                }
            }
        } else if (IS_INTEGER(args[1])) {
            int mask = (int)AS_INTEGER(args[1]);
            bool ok = false;
            if (mask == OWL_MASK_DATE_EMAIL || mask == OWL_MASK_DATE_EMAIL_UTC) {
                ok = parse_rfc5322(s->chars, s->length, &y, &m, &d, &hh, &mm, &ss);
                hasTime = true; /* RFC-5322 always has time */
            } else if (mask == OWL_MASK_DATE_INTERNET || mask == OWL_MASK_DATE_INTERNETUTC) {
                ok = parse_rfc3339(s->chars, s->length, &y, &m, &d, &hh, &mm, &ss);
                hasTime = true; /* RFC-3339 always has time */
            } else if (mask == OWL_MASK_DATE_SYSTEME) {
                ok = parse_date_auto(s->chars, s->length, &y, &m, &d);
            }
            if (!ok) return chaine_vers_date_erreur(s->chars);
        } else {
            return chaine_vers_date_erreur(s->chars);
        }
    } else {
        /* Form 1: auto-detect JJ/MM/AAAA, JJ-MM-AAAA, YYYYMMDD */
        if (!parse_date_auto(s->chars, s->length, &y, &m, &d)) {
            return chaine_vers_date_erreur(s->chars);
        }
    }

    owl_clear_error();
    if (hasTime) {
        return OBJ_VAL(obj_new_datetime(y, m, d, hh, mm, ss, 0));
    }
    return OBJ_VAL(obj_new_date(y, m, d));
}

/* Tables FR jour/mois, forme courte (abrégée avec point) et longue,
 * capitalisées en FR ("Lun.", "Lundi", "Jan.", "Janvier"). */
static const char* const FR_DAYS_SHORT[7] = {
    "Lun.", "Mar.", "Mer.", "Jeu.", "Ven.", "Sam.", "Dim."
};
static const char* const FR_DAYS_LONG[7] = {
    "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi", "Dimanche"
};
static const char* const FR_MONTHS_SHORT[13] = {
    "", "Jan.", "F\xc3\xa9v.", "Mars", "Avr.", "Mai", "Juin",
    "Juil.", "Ao\xc3\xbb""t", "Sep.", "Oct.", "Nov.", "D\xc3\xa9" "c."
};
static const char* const FR_MONTHS_LONG[13] = {
    "", "Janvier", "F\xc3\xa9" "vrier", "Mars", "Avril", "Mai", "Juin",
    "Juillet", "Ao\xc3\xbb""t", "Septembre", "Octobre", "Novembre", "D\xc3\xa9" "cembre"
};

/*  — cœur de formatage date/heure dirigé par un masque de jetons,
 * factorisé depuis fn_date_vers_chaine (source unique). Réutilisé par
 * owl_date_format_with_mask (champ de saisie typSaisieDate, MasqueAffichage).
 * Écrit au plus outsz-1 octets dans out (non terminé ici), retourne la longueur
 * écrite. Les jetons horaires (HH/mm/MI/SS/CC) ne sont interprétés que si
 * hasTime ; sinon ils retombent en littéraux. */
static int format_date_time_tokens(char* out, int outsz,
                                   const char* fmt, int flen,
                                   int y, int m, int d,
                                   bool hasTime, int hh, int mi, int sec, int ms) {
    int dow = day_of_week(y, m, d); /* 0=Lun..6=Dim */
    int mo = (m >= 1 && m <= 12) ? m : 0;
    int olen = 0, i = 0;
    int cap = outsz - 16;
    while (i < flen && olen < cap) {
        int rem = flen - i;
        if (rem >= 4 && strncmp(fmt+i, "AAAA", 4) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%04d", y); i += 4;
        } else if (rem >= 4 && strncmp(fmt+i, "YYYY", 4) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%04d", y); i += 4;
        } else if (rem >= 4 && strncasecmp(fmt+i, "MMMM", 4) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%s", FR_MONTHS_LONG[mo]); i += 4;
        } else if (rem >= 4 && strncasecmp(fmt+i, "JJJJ", 4) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%s", FR_DAYS_LONG[dow]); i += 4;
        } else if (rem >= 3 && strncasecmp(fmt+i, "MMM", 3) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%s", FR_MONTHS_SHORT[mo]); i += 3;
        } else if (rem >= 3 && strncasecmp(fmt+i, "JJJ", 3) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%s", FR_DAYS_SHORT[dow]); i += 3;
        } else if (rem >= 2 && strncmp(fmt+i, "AA", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", y % 100); i += 2;
        } else if (rem >= 2 && strncmp(fmt+i, "MM", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", m); i += 2;
        } else if (rem >= 2 && strncmp(fmt+i, "JJ", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", d); i += 2;
        } else if (rem >= 2 && strncmp(fmt+i, "DD", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", d); i += 2;
        } else if (hasTime && rem >= 2 && strncmp(fmt+i, "HH", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", hh); i += 2;
        } else if (hasTime && rem >= 2 && strncmp(fmt+i, "mm", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", mi); i += 2;
        } else if (hasTime && rem >= 2 && strncmp(fmt+i, "MI", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", mi); i += 2;
        } else if (hasTime && rem >= 2 && strncmp(fmt+i, "SS", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", sec); i += 2;
        } else if (hasTime && rem >= 2 && strncmp(fmt+i, "CC", 2) == 0) {
            olen += snprintf(out+olen, outsz-olen, "%02d", ms / 10); i += 2;
        } else {
            out[olen++] = fmt[i++];
        }
    }
    return olen;
}

/*  — API publique du moteur de masque date, pour le champ de saisie
 * (stdlib_ui.c, MasqueAffichage sur typSaisieDate). Réutilise le moteur interne
 * de stdlib_date : formatage symétrique (format_date_time_tokens) et parse
 * dirigé par masque (parse_date_with_format). Aucune réimplémentation côté UI. */
int owl_date_format_with_mask(char* out, int outsz,
                              const char* fmt, int flen,
                              int y, int m, int d) {
    if (out == NULL || outsz <= 0) return 0;
    if (fmt == NULL) flen = 0;
    int n = format_date_time_tokens(out, outsz, fmt, flen, y, m, d,
                                    false, 0, 0, 0, 0);
    out[n < outsz ? n : outsz - 1] = '\0';
    return n;
}

bool owl_date_parse_with_mask(const char* s, int slen,
                              const char* fmt, int flen,
                              int* y, int* m, int* d) {
    if (s == NULL || fmt == NULL) return false;
    int hh = 0, mm = 0, ss = 0;
    return parse_date_with_format(s, slen, fmt, flen, y, m, d, &hh, &mm, &ss);
}

/*  — parse dirigé par masque pour le champ de saisie HEURE
 * (typSaisieHeure). Miroir de la partie horaire de parse_date_with_format mais
 * SANS validation calendaire : seuls les jetons HH / MI(|mm) / SS / CC sont
 * interprétés, le reste (séparateurs) est littéral et doit correspondre. On ne
 * peut pas réutiliser parse_date_with_format tel quel ici : sa queue exige un
 * mois/jour valides (1..12, 1..31), qu'un masque purement horaire ne fournit
 * jamais (m=d=0 -> rejet). Réutilise les helpers de tokenisation partagés
 * (read_digits / match_fmt_token). */
static bool parse_time_with_format(const char* s, int slen, const char* fmt, int flen,
                                   int* hh, int* mi, int* ss, int* cs) {
    *hh = *mi = *ss = *cs = 0;
    int si = 0, fi = 0;
    static const char* const TOK_HH[] = { "HH", NULL };
    /* `mm` minuscule = minutes (alias de `MI`, symétrie avec le format-side qui
     * disambigue par la casse : MM = mois, mm = minutes). */
    static const char* const TOK_MI[] = { "MI", "mm", NULL };
    static const char* const TOK_SS[] = { "SS", NULL };
    static const char* const TOK_CC[] = { "CC", NULL };
    while (fi < flen && si < slen) {
        int r = 0, tmp = 0;
        if (match_fmt_token(fmt, flen, fi, TOK_HH) >= 0) {
            r = read_digits(s, slen, si, 2, 2, &tmp);
            if (r == 0) return false;
            *hh = tmp; si += r; fi += 2; continue;
        }
        if (match_fmt_token(fmt, flen, fi, TOK_MI) >= 0) {
            /* Garde-fou hérité du parse date : `mm` suivi d'un `m` désigne un
             * mois en lettres (mmm/mmmm) — sans objet en contexte heure, gardé
             * par parité pour ne pas mal interpréter un tel gabarit. */
            if (!(fmt[fi] == 'm' && fi + 2 < flen && fmt[fi + 2] == 'm')) {
                r = read_digits(s, slen, si, 2, 2, &tmp);
                if (r == 0) return false;
                *mi = tmp; si += r; fi += 2; continue;
            }
        }
        if (match_fmt_token(fmt, flen, fi, TOK_SS) >= 0) {
            r = read_digits(s, slen, si, 2, 2, &tmp);
            if (r == 0) return false;
            *ss = tmp; si += r; fi += 2; continue;
        }
        if (match_fmt_token(fmt, flen, fi, TOK_CC) >= 0) {
            r = read_digits(s, slen, si, 2, 2, &tmp);
            if (r == 0) return false;
            *cs = tmp; si += r; fi += 2; continue;
        }
        if (s[si] != fmt[fi]) return false;
        si++; fi++;
    }
    if (fi < flen) return false; /* format non entièrement consommé */
    if (si < slen) return false; /* reliquat non parsé */
    return (*hh <= 23 && *mi <= 59 && *ss <= 59 && *cs <= 99);
}

int owl_time_format_with_mask(char* out, int outsz,
                              const char* fmt, int flen,
                              int hh, int mi, int sec, int cs) {
    if (out == NULL || outsz <= 0) return 0;
    if (fmt == NULL) flen = 0;
    /* CC = centièmes ; le moteur partagé raisonne en millisecondes (CC = ms/10)
     * -> ms = cs*10. y/m/d ignorés : hasTime=true active les jetons horaires. */
    int n = format_date_time_tokens(out, outsz, fmt, flen, 0, 0, 0,
                                    true, hh, mi, sec, cs * 10);
    out[n < outsz ? n : outsz - 1] = '\0';
    return n;
}

bool owl_time_parse_with_mask(const char* s, int slen,
                              const char* fmt, int flen,
                              int* hh, int* mi, int* sec, int* cs) {
    if (s == NULL || fmt == NULL) return false;
    return parse_time_with_format(s, slen, fmt, flen, hh, mi, sec, cs);
}

/* DateVersChaine(d [, format]) / DateToString(d [, format])
 * Default format (canon): "JJ/MM/AAAA".
 * <d> accepté : ObjDate, ObjDateTime, ou chaîne AAAAMMJJ / AAAAMMJJHH...
 * Formats supportés:
 *   AAAA / YYYY : année 4 chiffres
 *   AA          : année 2 chiffres
 *   MM          : mois 2 chiffres
 *   MMM / MMMM  : mois en lettres (abrégé / long), FR locale
 *   JJ / DD     : jour 2 chiffres
 *   JJJ / JJJJ  : jour en lettres (abrégé / long), FR locale
 *   HH mm SS CC : heure/min/sec/centièmes (si source DateHeure)
 * La casse Jjj/Jjjj/Mmm/Mmmm donne le même résultat (initiale majuscule). */
static Value fn_date_vers_chaine(int argCount, Value* args) {
    if (argCount < 1) return OBJ_VAL(obj_copy_string("", 0));
    ObjDate tmp;
    ObjDate* d = NULL;
    int hh = 0, mi = 0, sec = 0, ms = 0;
    bool hasTime = false;
    if (IS_DATE(args[0])) {
        d = AS_DATE(args[0]);
    } else if (IS_DATETIME(args[0])) {
        ObjDateTime* dt = AS_DATETIME(args[0]);
        tmp.year = dt->year; tmp.month = dt->month; tmp.day = dt->day;
        hh = dt->hour; mi = dt->minute; sec = dt->second; ms = dt->ms;
        hasTime = true;
        d = &tmp;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length == 0) {
            /* Doc: DateHeureVersChaîne("") retourne "" */
            return OBJ_VAL(obj_copy_string("", 0));
        }
        int y, m, dd;
        if (s->length < 8 || !parse_date_string(s->chars, 8, &y, &m, &dd)) return NIL_VAL;
        tmp.year = y; tmp.month = m; tmp.day = dd;
        /* Parse partie heure si la chaîne en contient (HHmm[SS[FFF]]) */
        if (s->length >= 12) {
            hh = (s->chars[8]-'0')*10 + (s->chars[9]-'0');
            mi = (s->chars[10]-'0')*10 + (s->chars[11]-'0');
            hasTime = true;
        }
        if (s->length >= 14) {
            sec = (s->chars[12]-'0')*10 + (s->chars[13]-'0');
        }
        if (s->length >= 17) {
            ms = ((s->chars[14]-'0')*100) + ((s->chars[15]-'0')*10) + (s->chars[16]-'0');
        } else if (s->length == 16) {
            /* CC sur 2 chiffres → ms = CC * 10 */
            ms = (((s->chars[14]-'0')*10) + (s->chars[15]-'0')) * 10;
        }
        d = &tmp;
    } else {
        return NIL_VAL;
    }
    char buf[128];
    /* Forme avec mask entier (maskDateInternet/Email/Système…) */
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        int mask = (int)AS_INTEGER(args[1]);
        int outLen = 0;
        switch (mask) {
            case OWL_MASK_DATE_INTERNET:
                outLen = format_rfc3339(buf, sizeof(buf),
                    d->year, d->month, d->day, hh, mi, sec, ms, false);
                break;
            case OWL_MASK_DATE_INTERNETUTC:
                outLen = format_rfc3339(buf, sizeof(buf),
                    d->year, d->month, d->day, hh, mi, sec, ms, true);
                break;
            case OWL_MASK_DATE_EMAIL:
                outLen = format_rfc5322(buf, sizeof(buf),
                    d->year, d->month, d->day, hh, mi, sec, false);
                break;
            case OWL_MASK_DATE_EMAIL_UTC:
                outLen = format_rfc5322(buf, sizeof(buf),
                    d->year, d->month, d->day, hh, mi, sec, true);
                break;
            case OWL_MASK_DATE_DUREE_RELATIVE:
                outLen = format_duree_relative(buf, sizeof(buf),
                    d->year, d->month, d->day, hh, mi, sec);
                break;
            case OWL_MASK_DATE_SYSTEME:
            default:
                /* Format projet par défaut → JJ/MM/AAAA [HH:mm:SS:CC] */
                if (hasTime) {
                    outLen = snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d:%02d",
                                      d->day, d->month, d->year, hh, mi, sec, ms / 10);
                } else {
                    outLen = snprintf(buf, sizeof(buf), "%02d/%02d/%04d",
                                      d->day, d->month, d->year);
                }
                break;
        }
        return OBJ_VAL(obj_copy_string(buf, outLen));
    }
    if (argCount >= 2 && IS_STRING(args[1])) {
        ObjString* fmt = AS_STRING(args[1]);
        char out[256];
        /* Name patterns case-insensitive (MMM/Mmm both give "Déc."), numeric
         * patterns case-sensitive (MM = mois, mm = minutes), longest-match
         * first — toute la logique vit dans format_date_time_tokens (,
         * source unique partagée avec le masque du champ de saisie). */
        int olen = format_date_time_tokens(out, sizeof(out),
                                           fmt->chars, fmt->length,
                                           d->year, d->month, d->day,
                                           hasTime, hh, mi, sec, ms);
        return OBJ_VAL(obj_copy_string(out, olen));
    }
    /* Default format canon:
     *   Date     → "JJ/MM/AAAA"
     *   DateHeure → "JJ/MM/AAAA HH:mm:SS:CC" */
    if (hasTime) {
        snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d:%02d",
                 d->day, d->month, d->year, hh, mi, sec, ms / 10);
    } else {
        snprintf(buf, sizeof(buf), "%02d/%02d/%04d", d->day, d->month, d->year);
    }
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* ChaineVersHeure(s) / StringToTime(s) - format HHMMSSCC or HHMMSS */
static Value fn_chaine_vers_heure(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;
    ObjString* s = AS_STRING(args[0]);
    int h, m, sec, cs;
    if (!parse_time_string_cs(s->chars, s->length, &h, &m, &sec, &cs)) {
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "ChaineVersHeure",
                              "native", "ChaineVersHeure",
                              "value", s->chars,
                              "expected", "heure HHMMSSCC ou HHMMSS",
                              NULL);
        return NIL_VAL;
    }
    owl_clear_error();
    return OBJ_VAL(obj_new_time_cs(h, m, sec, cs));
}

/* HeureVersChaine(h [, format]) / TimeToString(h)
 * canon default format = "HH:MM:SS:CC" (includes centièmes).
 * Accepts ObjTime, ObjDateTime, or a string HHMMSSCC/HHMMSS. */
static Value fn_heure_vers_chaine(int argCount, Value* args) {
    if (argCount < 1) return OBJ_VAL(obj_copy_string("", 0));
    int hour = 0, minute = 0, second = 0, cs = 0;

    if (IS_TIME(args[0])) {
        ObjTime* t = AS_TIME(args[0]);
        hour = t->hour; minute = t->minute; second = t->second; cs = t->cs;
    } else if (IS_DATETIME(args[0])) {
        ObjDateTime* dt = AS_DATETIME(args[0]);
        hour = dt->hour; minute = dt->minute; second = dt->second; cs = dt->ms / 10;
    } else if (IS_STRING(args[0])) {
        int h, m, sec, c;
        ObjString* s = AS_STRING(args[0]);
        if (!parse_time_string_cs(s->chars, s->length, &h, &m, &sec, &c)) {
            owl_emit_native_error(E_NATIVE_PARSE_ERROR, "HeureVersChaine",
                                  "native", "HeureVersChaine",
                                  "value", s->chars,
                                  "expected", "heure HHMMSSCC ou HHMMSS",
                                  NULL);
            return OBJ_VAL(obj_copy_string("", 0));
        }
        hour = h; minute = m; second = sec; cs = c;
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "HeureVersChaine",
                              "native", "HeureVersChaine",
                              "arg", "heure",
                              "actual", "autre type",
                              "expected", "Heure, DateHeure ou Chaine",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    /* canon default = "HH:MM:SS:CC" */
    const char* fmt = (argCount >= 2 && IS_STRING(args[1]))
                      ? AS_CSTRING(args[1]) : "HH:MM:SS:CC";

    char out[128]; int oi = 0;
    for (const char* p = fmt; *p && oi < 120; p++) {
        if (p[0]=='H' && p[1]=='H') {
            oi += snprintf(out+oi, sizeof(out)-oi, "%02d", hour); p++;
        } else if (p[0]=='M' && p[1]=='M') {
            oi += snprintf(out+oi, sizeof(out)-oi, "%02d", minute); p++;
        } else if (p[0]=='S' && p[1]=='S') {
            oi += snprintf(out+oi, sizeof(out)-oi, "%02d", second); p++;
        } else if (p[0]=='C' && p[1]=='C') {
            oi += snprintf(out+oi, sizeof(out)-oi, "%02d", cs); p++;
        } else {
            out[oi++] = *p;
        }
    }
    out[oi] = '\0';
    return OBJ_VAL(obj_copy_string(out, oi));
}

/* DateVersEntier(d [, Vrai]) / DateToInteger(d)
 * Retourne le nombre de jours écoulés (1-based canon):
 *   - par défaut: depuis le 01/01/1800 (01/01/1800 → 1, 02/01/1800 → 2, …)
 *   - avec Vrai : depuis le 1er janvier de l'année de <d>
 * Accepte Date, DateHeure, ou string AAAAMMJJ (la partie heure est ignorée). */
static Value fn_date_vers_entier(int argCount, Value* args) {
    int y = 0, m = 0, d = 0;
    if (IS_DATE(args[0])) {
        ObjDate* dd = AS_DATE(args[0]);
        y = dd->year; m = dd->month; d = dd->day;
    } else if (IS_DATETIME(args[0])) {
        ObjDateTime* dt = AS_DATETIME(args[0]);
        y = dt->year; m = dt->month; d = dt->day;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length < 8 || !parse_date_string(s->chars, 8, &y, &m, &d))
            return INTEGER_VAL(0);
    } else {
        return INTEGER_VAL(0);
    }
    bool fromStartOfYear = (argCount >= 2 && IS_BOOL(args[1]) && AS_BOOL(args[1]));
    int64_t jdn = date_to_jdn(y, m, d);
    int64_t epoch = fromStartOfYear ? date_to_jdn(y, 1, 1) : date_to_jdn(1800, 1, 1);
    return INTEGER_VAL(jdn - epoch + 1);
}

/* EntierVersDate(n) / IntegerToDate(n) - inverse of DateVersEntier
 * n = nombre de jours 1-based depuis le 01/01/1800 */
/* EntierVersDate(n [, partie])
 * partie absent  → ObjDate (jour 1 = 01/01/1800)
 * partie "A"     → chaîne 4 chars année
 * partie "M"     → chaîne 2 chars mois
 * partie "J"     → chaîne 2 chars jour
 */
static Value fn_entier_vers_date(int argCount, Value* args) {
    int64_t n = 0;
    if (IS_INTEGER(args[0])) n = AS_INTEGER(args[0]);
    else if (IS_REAL(args[0])) n = (int64_t)AS_REAL(args[0]);
    int64_t epoch = date_to_jdn(1800, 1, 1);
    int y, m, d;
    jdn_to_date(epoch + n - 1, &y, &m, &d);

    if (argCount >= 2 && IS_STRING(args[1])) {
        ObjString* p = AS_STRING(args[1]);
        /* Format strict : exactement 1 char. "" et "AB" sont des entrees
         * explicitement fournies par l'appelant — refuser plutot que de
         * deduire un comportement implicite (fallback date complete sur ""
         * ou troncature silencieuse sur "AB"). */
        if (p->length != 1) {
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "EntierVersDate",
                                  "native", "EntierVersDate",
                                  "arg", "partie",
                                  "value", p->chars,
                                  "expected", "format A, M, J ou D (1 caractere)",
                                  NULL);
            return OBJ_VAL(obj_copy_string("", 0));
        }
        char buf[8]; int len = 0;
        char c = p->chars[0];
        if (c == 'A' || c == 'a') { len = snprintf(buf, sizeof(buf), "%04d", y); }
        else if (c == 'M' || c == 'm') { len = snprintf(buf, sizeof(buf), "%02d", m); }
        else if (c == 'J' || c == 'j' || c == 'D' || c == 'd') { len = snprintf(buf, sizeof(buf), "%02d", d); }
        else {
            char repr[2] = { c, '\0' };
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "EntierVersDate",
                                  "native", "EntierVersDate",
                                  "arg", "partie",
                                  "value", repr,
                                  "expected", "format A, M, J ou D",
                                  NULL);
            return OBJ_VAL(obj_copy_string("", 0));
        }
        return OBJ_VAL(obj_copy_string(buf, len));
    }
    return OBJ_VAL(obj_new_date(y, m, d));
}

/* Jour(d) / Day(d) - extract day from date */
static Value fn_jour(int argCount, Value* args) {
    (void)argCount;
    if (IS_DATE(args[0])) return INTEGER_VAL(AS_DATE(args[0])->day);
    if (IS_DATETIME(args[0])) return INTEGER_VAL(AS_DATETIME(args[0])->day);
    return INTEGER_VAL(0);
}

/* Mois(d) / Month(d) */
static Value fn_mois(int argCount, Value* args) {
    (void)argCount;
    if (IS_DATE(args[0])) return INTEGER_VAL(AS_DATE(args[0])->month);
    if (IS_DATETIME(args[0])) return INTEGER_VAL(AS_DATETIME(args[0])->month);
    return INTEGER_VAL(0);
}

/* Annee(d) / Year(d) */
static Value fn_annee(int argCount, Value* args) {
    (void)argCount;
    if (IS_DATE(args[0])) return INTEGER_VAL(AS_DATE(args[0])->year);
    if (IS_DATETIME(args[0])) return INTEGER_VAL(AS_DATETIME(args[0])->year);
    return INTEGER_VAL(0);
}

/* DateVersJourDeLaSemaine(d) / DateToDayOfWeek(d)
 * canon convention (ISO européen): 1=Lundi, 2=Mardi ... 6=Samedi, 7=Dimanche
 * day_of_week() returns 0=Mon..6=Sun → add 1. */
static Value fn_jour_semaine(int argCount, Value* args) {
    (void)argCount;
    int y = 0, m = 0, d = 0;
    if (IS_DATE(args[0])) { y = AS_DATE(args[0])->year; m = AS_DATE(args[0])->month; d = AS_DATE(args[0])->day; }
    else if (IS_DATETIME(args[0])) { y = AS_DATETIME(args[0])->year; m = AS_DATETIME(args[0])->month; d = AS_DATETIME(args[0])->day; }
    else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length < 8 || !parse_date_string(s->chars, 8, &y, &m, &d)) return INTEGER_VAL(0);
    }
    else return INTEGER_VAL(0);
    int dow = day_of_week(y, m, d); /* 0=Mon..6=Sun */
    return INTEGER_VAL(dow + 1); /* 1=Lundi ... 7=Dimanche */
}

static Value fn_date_coerce(int argCount, Value* args); /* forward */

/* DateDifference(d1, d2) - difference in days (d2 - d1).
 * Accepte Date ou chaîne AAAAMMJJ (cast implicite via fn_date_coerce). */
static Value fn_date_difference(int argCount, Value* args) {
    (void)argCount;
    /*   : v1 (ObjDate fresh) unrooted pendant la 2e
     * fn_date_coerce qui alloue → GC → v1 collecte → IS_DATE(v1) false
     * → return 0. Symptome : DateDifference(date1, date2) retourne 0. */
    Value v1 = fn_date_coerce(1, &args[0]);
    if (IS_OBJ(v1)) vm_push(v1);
    Value v2 = fn_date_coerce(1, &args[1]);
    if (IS_OBJ(v1)) vm_pop();
    if (!IS_DATE(v1) || !IS_DATE(v2)) return INTEGER_VAL(0);
    ObjDate* d1 = AS_DATE(v1);
    ObjDate* d2 = AS_DATE(v2);
    int64_t jdn1 = date_to_jdn(d1->year, d1->month, d1->day);
    int64_t jdn2 = date_to_jdn(d2->year, d2->month, d2->day);
    return INTEGER_VAL(jdn2 - jdn1);
}

/* DateValide(d) / DateIsValid(d) - check if date is valid */
static Value fn_date_valide(int argCount, Value* args) {
    (void)argCount;
    if (IS_DATE(args[0])) {
        ObjDate* d = AS_DATE(args[0]);
        if (d->month < 1 || d->month > 12 || d->day < 1) return BOOL_VAL(false);
        return BOOL_VAL(d->day <= days_in_month(d->year, d->month));
    }
    if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        int y, m, d;
        return BOOL_VAL(parse_date_string(s->chars, s->length, &y, &m, &d));
    }
    return BOOL_VAL(false);
}

/* HeureValide(h) / TimeIsValid(h) - check if time is valid */
static Value fn_heure_valide(int argCount, Value* args) {
    (void)argCount;
    if (IS_TIME(args[0])) {
        ObjTime* t = AS_TIME(args[0]);
        return BOOL_VAL(t->hour >= 0 && t->hour <= 23 &&
                         t->minute >= 0 && t->minute <= 59 &&
                         t->second >= 0 && t->second <= 59);
    }
    if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        int h, m, sec;
        return BOOL_VAL(parse_time_string(s->chars, s->length, &h, &m, &sec));
    }
    return BOOL_VAL(false);
}

/* AnneeBissextile([<Date|Année>]) / LeapYear([<Date|Year>])
 *   No arg        -> current year
 *   Integer       -> treated as year (e.g. 2024)
 *   Date/DateTime -> year of the value
 *   String        -> year if 4 chars, date (AAAAMMJJ) if 8 chars */
static Value fn_annee_bissextile(int argCount, Value* args) {
    int y = 0;
    bool parse_failed = false;
    if (argCount == 0) {
        time_t t = time(NULL);
        struct tm* now = localtime(&t);
        y = now->tm_year + 1900;
    } else if (IS_INTEGER(args[0])) {
        y = (int)AS_INTEGER(args[0]);
    } else if (IS_DATE(args[0])) {
        y = AS_DATE(args[0])->year;
    } else if (IS_DATETIME(args[0])) {
        y = AS_DATETIME(args[0])->year;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length == 4) {
            for (int i = 0; i < 4; i++) {
                if (s->chars[i] < '0' || s->chars[i] > '9') { parse_failed = true; break; }
                y = y * 10 + (s->chars[i] - '0');
            }
        } else if (s->length >= 8) {
            int m = 0, d = 0;
            if (!parse_date_string(s->chars, 8, &y, &m, &d)) parse_failed = true;
        } else {
            parse_failed = true;
        }
    } else {
        parse_failed = true;
    }
    /*  Q2 — cassure : annee hors borne calendrier gregorien
     * (1583-9999) ou parse echec -> ErreurDetectee + Faux. Cohere avec
     * la borne basse appliquee par Paques et consorts. */
    if (parse_failed || y < 1583 || y > 9999) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "AnneeBissextile",
                              "native", "AnneeBissextile",
                              "arg", "année_ou_date",
                              "value", "annee invalide ou hors borne",
                              "expected", "annee 1583-9999 (calendrier gregorien)",
                              NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

static bool dh_components_valid(int y, int m, int d, int hh, int mm, int ss); /* forward */

/* DateHeureVersEpoch(dh) / DateTimeToEpoch(dh) - Unix timestamp */
static Value fn_dateheure_vers_epoch(int argCount, Value* args) {
    (void)argCount;
    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    if (IS_DATETIME(args[0])) {
        ObjDateTime* dt = AS_DATETIME(args[0]);
        y = dt->year; m = dt->month; d = dt->day;
        hh = dt->hour; mm = dt->minute; ss = dt->second;
    } else if (IS_DATE(args[0])) {
        ObjDate* dt = AS_DATE(args[0]);
        y = dt->year; m = dt->month; d = dt->day;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        /* Sentinel d'erreur canon : -1 si parse / validation echoue.
         * Format compact accepte : 8 (date seule), 12 (HHMM), 14 (HHMMSS).
         * Divergence assumee : "19700101000000" reste valide -> epoch 0
         * legitime preserve (le sentinel -1 ne se confond pas avec une
         * valeur valide). */
        if (s->length != 8 && s->length != 12 && s->length != 14)
            return INTEGER_VAL(-1);
        for (int i = 0; i < s->length; i++) {
            if (s->chars[i] < '0' || s->chars[i] > '9') return INTEGER_VAL(-1);
        }
        if (!parse_date_string(s->chars, 8, &y, &m, &d)) return INTEGER_VAL(-1);
        if (s->length >= 12) {
            hh = (s->chars[8]-'0')*10 + (s->chars[9]-'0');
            mm = (s->chars[10]-'0')*10 + (s->chars[11]-'0');
        }
        if (s->length >= 14) {
            ss = (s->chars[12]-'0')*10 + (s->chars[13]-'0');
        }
        if (!dh_components_valid(y, m, d, hh, mm, ss)) return INTEGER_VAL(-1);
    } else return INTEGER_VAL(-1);
    /* Garde finale : un DateHeure object peut avoir ete construit avec des
     * composants hors borne (cf. fn_dateheure_vers_reel). */
    if (!dh_components_valid(y, m, d, hh, mm, ss)) return INTEGER_VAL(-1);
    /* Conformité canon : <DateHeure> est en heure LOCALE → on convertit en
     * epoch UTC en soustrayant l'offset TZ (DST-aware). */
    int64_t jdn = date_to_jdn(y, m, d);
    int64_t epoch_jdn = date_to_jdn(1970, 1, 1);
    int64_t days = jdn - epoch_jdn;
    int64_t epochLocal = days * 86400 + hh * 3600 + mm * 60 + ss;
    int offMin = local_tz_offset_minutes(y, m, d, hh, mm, ss);
    return INTEGER_VAL(epochLocal - (int64_t)offMin * 60);
}

/* EpochVersDateHeure(n) / EpochToDateTime(n) — épochUTC → DateHeure locale */
static Value fn_epoch_vers_dateheure(int argCount, Value* args) {
    (void)argCount;
    int64_t epoch = 0;
    if (IS_INTEGER(args[0])) epoch = AS_INTEGER(args[0]);
    else if (IS_REAL(args[0])) epoch = (int64_t)AS_REAL(args[0]);
    /* Décompose l'epoch UTC pour récupérer la date d'application de l'offset
     * (le DST dépend de la date). On applique ensuite l'offset local. */
    int64_t epoch_jdn = date_to_jdn(1970, 1, 1);
    int64_t days0 = epoch / 86400;
    int64_t rem0 = epoch % 86400;
    if (rem0 < 0) { days0--; rem0 += 86400; }
    int y0, m0, d0;
    jdn_to_date(epoch_jdn + days0, &y0, &m0, &d0);
    int hh0 = (int)(rem0 / 3600);
    int mm0 = (int)((rem0 % 3600) / 60);
    int ss0 = (int)(rem0 % 60);
    int offMin = local_tz_offset_minutes(y0, m0, d0, hh0, mm0, ss0);
    int64_t epochLocal = epoch + (int64_t)offMin * 60;
    int64_t days = epochLocal / 86400;
    int64_t rem = epochLocal % 86400;
    if (rem < 0) { days--; rem += 86400; }
    int y, m, d;
    jdn_to_date(epoch_jdn + days, &y, &m, &d);
    int hh = (int)(rem / 3600);
    int mm = (int)((rem % 3600) / 60);
    int ss = (int)(rem % 60);
    return OBJ_VAL(obj_new_datetime(y, m, d, hh, mm, ss, 0));
}

/* Extrait les composants d'une DateHeure/Date/chaîne compacte (8/12/14).
 * Retourne false si le type n'est pas supporté ou si les composants sont
 * hors bornes. Mutualise l'entrée des conversions de fuseau. */
static bool dh_extract_components(Value v, int* y, int* m, int* d,
                                  int* hh, int* mm, int* ss) {
    *y = 0; *m = 0; *d = 0; *hh = 0; *mm = 0; *ss = 0;
    if (IS_DATETIME(v)) {
        ObjDateTime* dt = AS_DATETIME(v);
        *y = dt->year; *m = dt->month; *d = dt->day;
        *hh = dt->hour; *mm = dt->minute; *ss = dt->second;
    } else if (IS_DATE(v)) {
        ObjDate* dt = AS_DATE(v);
        *y = dt->year; *m = dt->month; *d = dt->day;
    } else if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        if (s->length != 8 && s->length != 12 && s->length != 14) return false;
        for (int i = 0; i < s->length; i++)
            if (s->chars[i] < '0' || s->chars[i] > '9') return false;
        if (!parse_date_string(s->chars, 8, y, m, d)) return false;
        if (s->length >= 12) {
            *hh = (s->chars[8]-'0')*10 + (s->chars[9]-'0');
            *mm = (s->chars[10]-'0')*10 + (s->chars[11]-'0');
        }
        if (s->length >= 14)
            *ss = (s->chars[12]-'0')*10 + (s->chars[13]-'0');
    } else return false;
    return dh_components_valid(*y, *m, *d, *hh, *mm, *ss);
}

/* DateHeureLocaleVersUTC(dh) / DateTimeLocalToUTC(dh)
 * Composants locaux → mur-horloge UTC. L'offset est pris à la date locale
 * fournie (DST-aware : offset d'hiver ≠ offset d'été). */
static Value fn_dateheure_locale_vers_utc(int argCount, Value* args) {
    (void)argCount;
    int y, m, d, hh, mm, ss;
    if (!dh_extract_components(args[0], &y, &m, &d, &hh, &mm, &ss)) return NIL_VAL;
    int64_t epoch_jdn = date_to_jdn(1970, 1, 1);
    int64_t epochLocal = (date_to_jdn(y, m, d) - epoch_jdn) * 86400
                       + hh * 3600 + mm * 60 + ss;
    int offMin = local_tz_offset_minutes(y, m, d, hh, mm, ss);
    int64_t epochUTC = epochLocal - (int64_t)offMin * 60;
    /* Décompose l'epoch UTC en mur-horloge UTC (aucun offset réappliqué). */
    int64_t days = epochUTC / 86400, rem = epochUTC % 86400;
    if (rem < 0) { days--; rem += 86400; }
    int uy, um, ud;
    jdn_to_date(epoch_jdn + days, &uy, &um, &ud);
    return OBJ_VAL(obj_new_datetime(uy, um, ud,
        (int)(rem / 3600), (int)((rem % 3600) / 60), (int)(rem % 60), 0));
}

/* DateHeureUTCVersLocale(dh) / DateTimeUTCToLocal(dh)
 * Composants UTC → mur-horloge local. L'offset est pris à la date UTC fournie
 * (même approximation aux bornes DST qu'EpochVersDateHeure). */
static Value fn_dateheure_utc_vers_locale(int argCount, Value* args) {
    (void)argCount;
    int y, m, d, hh, mm, ss;
    if (!dh_extract_components(args[0], &y, &m, &d, &hh, &mm, &ss)) return NIL_VAL;
    int64_t epoch_jdn = date_to_jdn(1970, 1, 1);
    int64_t epochUTC = (date_to_jdn(y, m, d) - epoch_jdn) * 86400
                     + hh * 3600 + mm * 60 + ss;
    int offMin = local_tz_offset_minutes(y, m, d, hh, mm, ss);
    int64_t epochLocal = epochUTC + (int64_t)offMin * 60;
    int64_t days = epochLocal / 86400, rem = epochLocal % 86400;
    if (rem < 0) { days--; rem += 86400; }
    int ly, lm, ld;
    jdn_to_date(epoch_jdn + days, &ly, &lm, &ld);
    return OBJ_VAL(obj_new_datetime(ly, lm, ld,
        (int)(rem / 3600), (int)((rem % 3600) / 60), (int)(rem % 60), 0));
}

static bool extract_ymd(Value v, int* y, int* m, int* d); /* forward */

/* Helper : extrait year/month depuis args.
 * Formes acceptées :
 *   0 args  → mois courant (DateSys)
 *   1 arg   → Date / DateTime / chaîne AAAAMMJJ
 *   2 args  → (year, month) directement (entiers)
 * Retourne false si introuvable. */
static bool extract_year_month(int argCount, Value* args, int* y, int* m) {
    if (argCount == 0) {
        time_t t = time(NULL);
        struct tm* tm = localtime(&t);
        *y = tm->tm_year + 1900; *m = tm->tm_mon + 1;
        return true;
    }
    if (argCount >= 2 && IS_INTEGER(args[0]) && IS_INTEGER(args[1])) {
        *y = (int)AS_INTEGER(args[0]); *m = (int)AS_INTEGER(args[1]);
        return *m >= 1 && *m <= 12;
    }
    int d = 0;
    if (extract_ymd(args[0], y, m, &d)) return true;
    return false;
}

/* PremierJourDuMois([d ou (y,m)]) / FirstDayOfMonth */
static Value fn_premier_jour_du_mois(int argCount, Value* args) {
    int y, m;
    if (!extract_year_month(argCount, args, &y, &m)) return NIL_VAL;
    return OBJ_VAL(obj_new_date(y, m, 1));
}

/* DernierJourDuMois([d ou (y,m)]) / LastDayOfMonth */
static Value fn_dernier_jour_du_mois(int argCount, Value* args) {
    int y, m;
    if (!extract_year_month(argCount, args, &y, &m)) return NIL_VAL;
    return OBJ_VAL(obj_new_date(y, m, days_in_month(y, m)));
}

/* DateVersJourEnLettre(d) / DateToDayName(d)
 * Accepte Date, DateHeure, chaîne AAAAMMJJ (8) ou AAAAMMJJHHmmSS[CC] (14/16).
 * Refus explicite si chaîne non parsable ou type non supporté — sans garde,
 * le runtime renvoyait NIL silencieusement, piège indétectable côté appelant. */
static Value fn_date_vers_jour_en_lettre(int argCount, Value* args) {
    (void)argCount;
    static const char* joursFR[] = {"Lundi","Mardi","Mercredi","Jeudi","Vendredi","Samedi","Dimanche"};
    int y = 0, m = 0, d = 0;
    if (IS_DATE(args[0])) {
        y = AS_DATE(args[0])->year; m = AS_DATE(args[0])->month; d = AS_DATE(args[0])->day;
    } else if (IS_DATETIME(args[0])) {
        y = AS_DATETIME(args[0])->year; m = AS_DATETIME(args[0])->month; d = AS_DATETIME(args[0])->day;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        /* Formats acceptés : AAAAMMJJ (8), AAAAMMJJHHmmSS (14), AAAAMMJJHHmmSSCC (16). */
        bool good_len = (s->length == 8 || s->length == 14 || s->length == 16);
        if (!good_len || !parse_date_string(s->chars, 8, &y, &m, &d)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "DateVersJourEnLettre",
                                  "native", "DateVersJourEnLettre",
                                  "arg", "date",
                                  "expected", "chaine AAAAMMJJ, AAAAMMJJHHmmSS, AAAAMMJJHHmmSSCC, date ou dateheure",
                                  "actual", "autre",
                                  NULL);
            return OBJ_VAL(obj_copy_string("", 0));
        }
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "DateVersJourEnLettre",
                              "native", "DateVersJourEnLettre",
                              "arg", "date",
                              "expected", "chaine AAAAMMJJ, AAAAMMJJHHmmSS, AAAAMMJJHHmmSSCC, date ou dateheure",
                              "actual", "autre",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    /*  Q4 — Date sentinel invalide (y<1 ou m<1 ou d<1) :
     * pose ErreurDetectee + retour "" (cassure , Q2/Q4). */
    if (y < 1 || m < 1 || d < 1) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "DateVersJourEnLettre",
                              "native", "DateVersJourEnLettre",
                              "arg", "date",
                              "value", "date sentinel invalide",
                              "expected", "date valide (y>=1, m>=1, d>=1)",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    owl_clear_error();
    int dow = day_of_week(y, m, d); /* 0=Mon */
    const char* name = joursFR[dow];
    return OBJ_VAL(obj_copy_string(name, (int)strlen(name)));
}

/* DateVersMoisEnLettre(d) / DateToMonthName(d) */
static Value fn_date_vers_mois_en_lettre(int argCount, Value* args) {
    (void)argCount;
    static const char* moisFR[] = {"","Janvier","F\xc3\xa9vrier","Mars","Avril","Mai","Juin",
                                   "Juillet","Ao\xc3\xbb""t","Septembre","Octobre","Novembre","D\xc3\xa9""cembre"};
    int m = 0;
    /*  Q2/Q4 — sentinel Date/DateHeure (y<1, m<1 ou d<1) ou
     * mois hors borne ou entier hors-borne -> ErreurDetectee + "".
     * Cassure assumee : avant, retour NIL_VAL silencieux. */
    if (IS_DATE(args[0])) {
        int y = AS_DATE(args[0])->year, d = AS_DATE(args[0])->day;
        m = AS_DATE(args[0])->month;
        if (y < 1 || m < 1 || d < 1) m = 0; /* force erreur ci-dessous */
    } else if (IS_DATETIME(args[0])) {
        int y = AS_DATETIME(args[0])->year, d = AS_DATETIME(args[0])->day;
        m = AS_DATETIME(args[0])->month;
        if (y < 1 || m < 1 || d < 1) m = 0;
    } else if (IS_INTEGER(args[0])) m = (int)AS_INTEGER(args[0]);
    else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        int y, dd;
        if (s->length >= 8 && parse_date_string(s->chars, 8, &y, &m, &dd)) {
            if (y < 1 || m < 1 || dd < 1) m = 0;
        } else m = 0;
    }
    if (m < 1 || m > 12) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "DateVersMoisEnLettre",
                              "native", "DateVersMoisEnLettre",
                              "arg", "date",
                              "value", "mois invalide ou date sentinel",
                              "expected", "Date valide ou mois 1-12",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    owl_clear_error();
    const char* name = moisFR[m];
    return OBJ_VAL(obj_copy_string(name, (int)strlen(name)));
}

/* ================================================================
 * DATE ARITHMETIC AND NORMALIZATION
 * Exported for use in vm.c
 * ================================================================ */

void owl_normalize_date(ObjDate* d) {
    int64_t jdn = date_to_jdn(d->year, d->month, d->day);
    jdn_to_date(jdn, &d->year, &d->month, &d->day);
}

void owl_normalize_time(ObjTime* t) {
    /* Wrap seconds/minutes/hours */
    int64_t totalMs = (int64_t)t->hour * 3600000 + (int64_t)t->minute * 60000 +
                      (int64_t)t->second * 1000 + t->ms;
    if (totalMs < 0) totalMs = 0;
    totalMs = totalMs % 86400000LL; /* wrap to 24h */
    t->hour = (int)(totalMs / 3600000);
    t->minute = (int)((totalMs % 3600000) / 60000);
    t->second = (int)((totalMs % 60000) / 1000);
    t->ms = (int)(totalMs % 1000);
}

void owl_normalize_datetime(ObjDateTime* dt) {
    /* Normalize time part first, then date */
    int64_t totalMs = (int64_t)dt->hour * 3600000 + (int64_t)dt->minute * 60000 +
                      (int64_t)dt->second * 1000 + dt->ms;
    int64_t extraDays = 0;
    if (totalMs < 0) {
        extraDays = -((-totalMs + 86399999) / 86400000LL);
        totalMs -= extraDays * 86400000LL;
    } else if (totalMs >= 86400000LL) {
        extraDays = totalMs / 86400000LL;
        totalMs -= extraDays * 86400000LL;
    }
    dt->hour = (int)(totalMs / 3600000);
    dt->minute = (int)((totalMs % 3600000) / 60000);
    dt->second = (int)((totalMs % 60000) / 1000);
    dt->ms = (int)(totalMs % 1000);
    /* Normalize date with extra days from time overflow */
    int64_t jdn = date_to_jdn(dt->year, dt->month, dt->day) + extraDays;
    jdn_to_date(jdn, &dt->year, &dt->month, &dt->day);
}

Value owl_date_add_days(Value dateVal, int64_t days) {
    if (IS_DATE(dateVal)) {
        ObjDate* d = AS_DATE(dateVal);
        int64_t jdn = date_to_jdn(d->year, d->month, d->day) + days;
        int y, m, dd;
        jdn_to_date(jdn, &y, &m, &dd);
        return OBJ_VAL(obj_new_date(y, m, dd));
    }
    if (IS_DATETIME(dateVal)) {
        ObjDateTime* dt = AS_DATETIME(dateVal);
        int64_t jdn = date_to_jdn(dt->year, dt->month, dt->day) + days;
        int y, m, dd;
        jdn_to_date(jdn, &y, &m, &dd);
        return OBJ_VAL(obj_new_datetime(y, m, dd, dt->hour, dt->minute, dt->second, dt->ms));
    }
    return NIL_VAL;
}

Value owl_date_subtract(Value a, Value b) {
    /* Date/DateHeure - Date/DateHeure = Durée (ms). Mix OK grâce au helper. */
    if ((IS_DATE(a) || IS_DATETIME(a)) && (IS_DATE(b) || IS_DATETIME(b))) {
        int64_t ms_a = 0, ms_b = 0;
        owl_value_to_absolute_ms(a, &ms_a);
        owl_value_to_absolute_ms(b, &ms_b);
        return OBJ_VAL(obj_new_duration(ms_a - ms_b));
    }
    /* Date - int = Date - N jours */
    if (IS_DATE(a) && IS_INTEGER(b)) {
        return owl_date_add_days(a, -AS_INTEGER(b));
    }
    return NIL_VAL;
}

/* ================================================================
 * REGISTRATION
 * ================================================================ */

/* ================================================================
 * COERCION FUNCTIONS (called by compiler for typed declarations)
 * __DateCoerce: any value -> Date
 * __HeureCoerce: any value -> Heure
 * __DateHeureCoerce: any value -> DateHeure
 * ================================================================ */

/*  — Coerce sentinel temporal (Date/Heure/DateHeure aligne sur
 * fn_duree_coerce / ).
 *
 * Echec parse chaine -> sentinel "vide" (00/00/0000, 00:00:00,
 * 00/00/0000 00:00:00) + ErreurDetectee = Vrai. L'utilisateur teste
 * `Si ErreurDetectee Alors ...`. Preserve TypeVar et sous-props
 * lisibles a 0 (pas de drift vers chaine).
 *
 * Nullable -> __DateCoerceNullable / __HeureCoerceNullable / etc. :
 * meme parsing, mais echec retourne NIL au lieu du sentinel
 * (l'utilisateur teste `Si EstNul(d) Alors ...`). */
static Value fn_date_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    /* Already a Date */
    if (IS_DATE(v)) return v;
    /* DateTime -> extract date */
    if (IS_DATETIME(v)) {
        ObjDateTime* dt = AS_DATETIME(v);
        return OBJ_VAL(obj_new_date(dt->year, dt->month, dt->day));
    }
    /*  Q5 — coerce multi-format. Symetrie totale coerce <->
     * ChaineVersDate : accepte AAAAMMJJ + JJ/MM/AAAA + JJ-MM-AAAA +
     * AAAA-MM-JJ + AAAA/MM/JJ via le dispatcher unique. */
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int y, m, d, perr = 0;
        if (owl_parse_date_multi(s->chars, s->length, &y, &m, &d, &perr))
            return OBJ_VAL(obj_new_date(y, m, d));
        char snippet[64];
        int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
        memcpy(snippet, s->chars, n);
        snippet[n] = '\0';
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "Date",
                              "value", snippet,
                              "expected", "AAAAMMJJ, JJ/MM/AAAA, JJ-MM-AAAA, "
                                          "AAAA-MM-JJ ou AAAA/MM/JJ valide",
                              NULL);
        return OBJ_VAL(obj_new_date(0, 0, 0));
    }
    /* Integer -> days since 01/01/1800 */
    if (IS_INTEGER(v)) {
        int64_t n = AS_INTEGER(v);
        int64_t epoch = date_to_jdn(1800, 1, 1);
        int y, m, d;
        jdn_to_date(epoch + n, &y, &m, &d);
        return OBJ_VAL(obj_new_date(y, m, d));
    }
    return v;
}

/* Variante nullable : echec parse -> NIL + ErreurDetectee. Cf. fn_date_coerce. */
static Value fn_date_coerce_nullable(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_NIL(v)) return NIL_VAL;
    if (IS_DATE(v) || IS_DATETIME(v)) return fn_date_coerce(argCount, args);
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int y, m, d, perr = 0;
        if (owl_parse_date_multi(s->chars, s->length, &y, &m, &d, &perr))
            return OBJ_VAL(obj_new_date(y, m, d));
        char snippet[64];
        int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
        memcpy(snippet, s->chars, n);
        snippet[n] = '\0';
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "Date",
                              "value", snippet,
                              "expected", "AAAAMMJJ, JJ/MM/AAAA, JJ-MM-AAAA, "
                                          "AAAA-MM-JJ ou AAAA/MM/JJ valide",
                              NULL);
        return NIL_VAL;
    }
    if (IS_INTEGER(v)) return fn_date_coerce(argCount, args);
    return v;
}

static Value fn_heure_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_TIME(v)) return v;
    if (IS_DATETIME(v)) {
        ObjDateTime* dt = AS_DATETIME(v);
        return OBJ_VAL(obj_new_time_cs(dt->hour, dt->minute, dt->second, dt->ms / 10));
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int h, m, sec, cs;
        if (parse_time_string_cs(s->chars, s->length, &h, &m, &sec, &cs))
            return OBJ_VAL(obj_new_time_cs(h, m, sec, cs));
        char snippet[64];
        int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
        memcpy(snippet, s->chars, n);
        snippet[n] = '\0';
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "Heure",
                              "value", snippet,
                              "expected", "HHMMSS[CC] ou HH:MM[:SS[.CC]] valide",
                              NULL);
        return OBJ_VAL(obj_new_time_cs(0, 0, 0, 0));
    }
    return v;
}

/* Variante nullable Heure : echec parse -> NIL + ErreurDetectee. */
static Value fn_heure_coerce_nullable(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_NIL(v)) return NIL_VAL;
    if (IS_TIME(v) || IS_DATETIME(v)) return fn_heure_coerce(argCount, args);
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int h, m, sec, cs;
        if (parse_time_string_cs(s->chars, s->length, &h, &m, &sec, &cs))
            return OBJ_VAL(obj_new_time_cs(h, m, sec, cs));
        char snippet[64];
        int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
        memcpy(snippet, s->chars, n);
        snippet[n] = '\0';
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "Heure",
                              "value", snippet,
                              "expected", "HHMMSS[CC] ou HH:MM[:SS[.CC]] valide",
                              NULL);
        return NIL_VAL;
    }
    return v;
}

static Value fn_dateheure_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_DATETIME(v)) return v;
    if (IS_DATE(v)) {
        ObjDate* d = AS_DATE(v);
        return OBJ_VAL(obj_new_datetime(d->year, d->month, d->day, 0, 0, 0, 0));
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
        /* Format ISO datetime (extension naturelle , symetrie avec
         * parse_date_auto). Reconnu : AAAA-MM-JJ[ T]HH:MM[:SS] + variantes
         * slash. Cf. Bloc A test 'date seule invalide' : len 10 (date seule
         * ISO) doit etre rejete pour la coerce DateHeure (une DateHeure
         * exige une partie horaire). On force le rejet ici plutot que dans
         * le parser permissif pour conserver la branche compact (8 chars). */
        if (s->length != 10 &&
            parse_dateheure_iso(s->chars, s->length, &y, &m, &d, &hh, &mm, &ss)) {
            if (!dh_components_valid(y, m, d, hh, mm, ss)) {
                owl_emit_native_error(E_NATIVE_PARSE_ERROR, "DateHeure",
                                      "value", s->chars,
                                      "expected", "composants temporels valides (h 0-23, m 0-59, s 0-59)",
                                      NULL);
                return OBJ_VAL(obj_new_datetime(0, 0, 0, 0, 0, 0, 0));
            }
            return OBJ_VAL(obj_new_datetime(y, m, d, hh, mm, ss, 0));
        }
        if (s->length >= 8 && s->length != 10) {
            if (!parse_date_string(s->chars, 8, &y, &m, &d)) {
                char snippet[64];
                int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
                memcpy(snippet, s->chars, n);
                snippet[n] = '\0';
                owl_emit_native_error(E_NATIVE_PARSE_ERROR, "DateHeure",
                                      "value", snippet,
                                      "expected", "AAAAMMJJ[HHMM[SS[CCC]]] ou ISO valide",
                                      NULL);
                return OBJ_VAL(obj_new_datetime(0, 0, 0, 0, 0, 0, 0));
            }
            if (s->length >= 12) {
                hh = (s->chars[8]-'0')*10 + (s->chars[9]-'0');
                mm = (s->chars[10]-'0')*10 + (s->chars[11]-'0');
            }
            if (s->length >= 14) {
                ss = (s->chars[12]-'0')*10 + (s->chars[13]-'0');
            }
            /* Sous-secondes : 16 chars = CC centiemes (x10 -> ms),
             * 17 chars = FFF millisecondes directes. Symetrique avec
             * fn_date_vers_chaine et coerce_to_ms_total. */
            int ms = 0;
            if (s->length == 16) {
                ms = ((s->chars[14]-'0')*10 + (s->chars[15]-'0')) * 10;
            } else if (s->length >= 17) {
                ms = (s->chars[14]-'0')*100 + (s->chars[15]-'0')*10 + (s->chars[16]-'0');
            }
            /* Validation stricte des composants temporels (h/m/s hors borne).
             * parse_date_string a deja valide mois/jour/leap-year ; on couvre
             * ici le gap HH/MM/SS laisse par les lectures positionnelles
             * ci-dessus. Coherent avec dh_components_valid defensif dans
             * DateHeureVersReel/Epoch et avec ChaineVersHeure stricte. */
            if (!dh_components_valid(y, m, d, hh, mm, ss)) {
                owl_emit_native_error(E_NATIVE_PARSE_ERROR, "DateHeure",
                                      "value", s->chars,
                                      "expected", "composants temporels valides (h 0-23, m 0-59, s 0-59)",
                                      NULL);
                return OBJ_VAL(obj_new_datetime(0, 0, 0, 0, 0, 0, 0));
            }
            return OBJ_VAL(obj_new_datetime(y, m, d, hh, mm, ss, ms));
        }
        /* IS_STRING mais length < 8 (chaine trop courte pour toutes les
         * variantes acceptees) : sentinel + ErreurDetectee, pas de drift type. */
        {
            char snippet[64];
            int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
            memcpy(snippet, s->chars, n);
            snippet[n] = '\0';
            owl_emit_native_error(E_NATIVE_PARSE_ERROR, "DateHeure",
                                  "value", snippet,
                                  "expected", "AAAAMMJJ[HHMM[SS[CCC]]] ou ISO valide",
                                  NULL);
            return OBJ_VAL(obj_new_datetime(0, 0, 0, 0, 0, 0, 0));
        }
    }
    return v;
}

/* Variante nullable DateHeure : echec parse -> NIL + ErreurDetectee. */
static Value fn_dateheure_coerce_nullable(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_NIL(v)) return NIL_VAL;
    /* Delegue au coerce standard puis substitue NIL si echec parse posa
     * ErreurDetectee (chaine devenue sentinel 00/00/0000 00:00:00). */
    if (!IS_STRING(v)) return fn_dateheure_coerce(argCount, args);
    ObjString* s = AS_STRING(v);
    int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
    /* Coerce nullable DateHeure : len 10 (date seule ISO) rejete pour
     * symetrie avec fn_dateheure_coerce. Cf. Bloc A 'date seule invalide'. */
    if (s->length != 10 &&
        parse_dateheure_iso(s->chars, s->length, &y, &m, &d, &hh, &mm, &ss)) {
        if (!dh_components_valid(y, m, d, hh, mm, ss)) {
            owl_emit_native_error(E_NATIVE_PARSE_ERROR, "DateHeure",
                                  "value", s->chars,
                                  "expected", "composants temporels valides (h 0-23, m 0-59, s 0-59)",
                                  NULL);
            return NIL_VAL;
        }
        return OBJ_VAL(obj_new_datetime(y, m, d, hh, mm, ss, 0));
    }
    if (s->length >= 8 && s->length != 10) {
        if (!parse_date_string(s->chars, 8, &y, &m, &d)) {
            char snippet[64];
            int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
            memcpy(snippet, s->chars, n);
            snippet[n] = '\0';
            owl_emit_native_error(E_NATIVE_PARSE_ERROR, "DateHeure",
                                  "value", snippet,
                                  "expected", "AAAAMMJJ[HHMM[SS[CCC]]] ou ISO valide",
                                  NULL);
            return NIL_VAL;
        }
        if (s->length >= 12) {
            hh = (s->chars[8]-'0')*10 + (s->chars[9]-'0');
            mm = (s->chars[10]-'0')*10 + (s->chars[11]-'0');
        }
        if (s->length >= 14) {
            ss = (s->chars[12]-'0')*10 + (s->chars[13]-'0');
        }
        int ms = 0;
        if (s->length == 16) {
            ms = ((s->chars[14]-'0')*10 + (s->chars[15]-'0')) * 10;
        } else if (s->length >= 17) {
            ms = (s->chars[14]-'0')*100 + (s->chars[15]-'0')*10 + (s->chars[16]-'0');
        }
        if (!dh_components_valid(y, m, d, hh, mm, ss)) {
            owl_emit_native_error(E_NATIVE_PARSE_ERROR, "DateHeure",
                                  "value", s->chars,
                                  "expected", "composants temporels valides (h 0-23, m 0-59, s 0-59)",
                                  NULL);
            return NIL_VAL;
        }
        return OBJ_VAL(obj_new_datetime(y, m, d, hh, mm, ss, ms));
    }
    /* Chaine trop courte. */
    {
        char snippet[64];
        int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
        memcpy(snippet, s->chars, n);
        snippet[n] = '\0';
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "DateHeure",
                              "value", snippet,
                              "expected", "AAAAMMJJ[HHMM[SS[CCC]]] ou ISO valide",
                              NULL);
        return NIL_VAL;
    }
}

/* fn_duree_coerce : `var est une durée = <expr>` parse l'expression vers
 * ObjDuration. Trois entrees supportees :
 *   - Duree (passthrough)
 *   - Entier : millisecondes (canon — un entier 1000 =
 *     1 seconde). Signe respecte (durée negative supportee).
 *     Cf  (refacto break-net storage ms canon) +  (signe).
 *   - Chaine :
 *       a. Format court OWL "1s", "200ms", "1mn" (cf owl_duree_parse_short_form)
 *       b. Format canon Q5-bis "[+/-]<J>HHMMSSCC" mode centieme via
 *          owl_parse_duration_multi (single-parser invariant, ).
 *
 * Echec parsing chaine inconnue -> Duree(0) + ErreurDetectee = Vrai
 * (cf. ). L'utilisateur teste `Si ErreurDetectee Alors ...` pour
 * intercepter un parse fail dynamique (cas littéral statique : rejete
 * a la compilation). */
static Value fn_duree_coerce(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_DURATION(v)) return v;
    if (IS_INTEGER(v)) {
        return OBJ_VAL(obj_new_duration(AS_INTEGER(v)));
    }
    if (IS_REAL(v)) {
        /* Réel = millisecondes, tronqué vers l'entier (grain ms entier).
         *  : composante réelle `{ Seconde = 2.5 }` émet `2.5*1000`
         * (réel 2500.0) -> 2500 ms. Sous-ms `0.5` -> 0 (troncature, pas
         * d'arrondi), cohérent avec durée->entier qui tronque aussi. */
        return OBJ_VAL(obj_new_duration((int64_t)AS_REAL(v)));
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        /* Try short form OWL "1s" / "200ms" / "1mn". */
        int64_t ms = owl_duree_parse_short_form(s->chars, s->length);
        if (ms >= 0) return OBJ_VAL(obj_new_duration(ms));
        /* Dispatcher Q5-bis canon : [+/-]<J>HHMMSSCC. */
        int perr = 0;
        if (owl_parse_duration_multi(s->chars, s->length, &ms, &perr)) {
            return OBJ_VAL(obj_new_duration(ms));
        }
        /* Format inconnu / bornes invalides : pose ErreurDetectee = Vrai
         * puis retourne Duree(0). On force ObjDuration(0) plutot que
         * keep-as-is pour que ..Millisecondes et le typage statique de la
         * variable restent fonctionnels. */
        char snippet[64];
        int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
        memcpy(snippet, s->chars, n);
        snippet[n] = '\0';
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "Dur\xc3\xa9" "e",
                              "value", snippet,
                              "expected", "[+/-]<J>HHMMSSCC (signe + jours + heures/minutes/secondes/centiemes)",
                              NULL);
        return OBJ_VAL(obj_new_duration(0));
    }
    /* Null ou autre type -> Duree(0) silencieuse. */
    return OBJ_VAL(obj_new_duration(0));
}

/* Variante nullable Duree : echec parse chaine -> NIL + ErreurDetectee.
 * Cf. fn_duree_coerce. */
static Value fn_duree_coerce_nullable(int argCount, Value* args) {
    (void)argCount;
    Value v = args[0];
    if (IS_NIL(v)) return NIL_VAL;
    if (IS_DURATION(v)) return v;
    if (IS_INTEGER(v)) return OBJ_VAL(obj_new_duration(AS_INTEGER(v)));
    if (IS_REAL(v)) return OBJ_VAL(obj_new_duration((int64_t)AS_REAL(v)));  /*  : ms tronqué */
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int64_t ms = owl_duree_parse_short_form(s->chars, s->length);
        if (ms >= 0) return OBJ_VAL(obj_new_duration(ms));
        int perr = 0;
        if (owl_parse_duration_multi(s->chars, s->length, &ms, &perr)) {
            return OBJ_VAL(obj_new_duration(ms));
        }
        char snippet[64];
        int n = s->length < (int)sizeof(snippet) - 1 ? s->length : (int)sizeof(snippet) - 1;
        memcpy(snippet, s->chars, n);
        snippet[n] = '\0';
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "Dur\xc3\xa9" "e",
                              "value", snippet,
                              "expected", "[+/-]<J>HHMMSSCC (signe + jours + heures/minutes/secondes/centiemes)",
                              NULL);
        return NIL_VAL;
    }
    return NIL_VAL;
}

/* ================================================================
 * GROUPE 1 — Extraction jour/semaine dans l'annee
 * ================================================================ */

/* DateVersJourDansLAnnee(d) / DateToDayOfYear(d) - day number 1-366 */
static Value fn_jour_dans_annee(int argCount, Value* args) {
    (void)argCount;
    int y = 0, m = 0, d = 0;
    if (IS_DATE(args[0])) { y = AS_DATE(args[0])->year; m = AS_DATE(args[0])->month; d = AS_DATE(args[0])->day; }
    else if (IS_DATETIME(args[0])) { y = AS_DATETIME(args[0])->year; m = AS_DATETIME(args[0])->month; d = AS_DATETIME(args[0])->day; }
    else return INTEGER_VAL(0);
    int64_t jdn = date_to_jdn(y, m, d);
    int64_t jan1 = date_to_jdn(y, 1, 1);
    return INTEGER_VAL((int)(jdn - jan1 + 1));
}

/* Calcul du numero de semaine pour une date valide (y>=1, m>=1, d>=1).
 *   useISO != 0 : norme ISO 8601 stricte (semaine 1 contient le 1er jeudi
 *                 de l'annee, debordement possible vers annee voisine).
 *   useISO == 0 : convention canon (semaine 0 pour debut janvier hors
 *                 sem 1, semaine 53 pour fin decembre quand jan suivant
 *                 tombe sur sem 1). La date reste dans son annee calendaire.
 * Partage entre NumeroDeSemaine / DateVersNumeroDeSemaine (fn_numero_de_semaine)
 * et EntierVersNumeroDeSemaine (fn_entier_vers_semaine). Tous trois exposent
 * un format optionnel iso8601 ; defaut = canon. */
static int compute_week_number(int y, int m, int d, int useISO) {
    int dow = day_of_week(y, m, d); /* 0 = lundi */
    int64_t jdn = date_to_jdn(y, m, d);

    if (useISO) {
        /* semaine 1 = celle qui contient le jeudi le plus proche. */
        int64_t thu = jdn + (3 - dow);
        int thy, thm, thd;
        jdn_to_date(thu, &thy, &thm, &thd);
        int64_t jan1 = date_to_jdn(thy, 1, 1);
        return (int)((thu - jan1) / 7) + 1;
    }

    int64_t jan1 = date_to_jdn(y, 1, 1);
    int dowJan1 = day_of_week(y, 1, 1);
    int64_t mondayW1;
    if (dowJan1 <= 3) {
        mondayW1 = jan1 - dowJan1;
    } else {
        mondayW1 = jan1 + (7 - dowJan1);
    }
    if (jdn < mondayW1) return 0;
    int week = (int)((jdn - mondayW1) / 7) + 1;
    int64_t thu = jdn + (3 - dow);
    int thy, thm, thd;
    jdn_to_date(thu, &thy, &thm, &thd);
    if (thy > y) return 53;
    return week;
}

/* NumeroDeSemaine(d [, format]) / WeekNumber(d [, format]) — doc 3027027. */
static Value fn_numero_de_semaine(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    int y = 0, m = 0, d = 0;
    if (IS_DATE(args[0])) { y = AS_DATE(args[0])->year; m = AS_DATE(args[0])->month; d = AS_DATE(args[0])->day; }
    else if (IS_DATETIME(args[0])) { y = AS_DATETIME(args[0])->year; m = AS_DATETIME(args[0])->month; d = AS_DATETIME(args[0])->day; }
    else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length < 8 || !parse_date_string(s->chars, 8, &y, &m, &d)) {
            /*  Q2 — cassure : pose ErreurDetectee au lieu de 0 silencieux. */
            owl_emit_native_error(E_NATIVE_PARSE_ERROR, "NumeroDeSemaine",
                                  "native", "NumeroDeSemaine",
                                  "value", "chaine invalide",
                                  "expected", "AAAAMMJJ valide",
                                  NULL);
            return INTEGER_VAL(0);
        }
    }
    else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NumeroDeSemaine",
                              "native", "NumeroDeSemaine",
                              "arg", "date",
                              "expected", "date, dateheure ou chaine AAAAMMJJ",
                              "actual", "autre",
                              NULL);
        return INTEGER_VAL(0);
    }
    /*  Q2/Q4 — sentinel date invalide (y<1, m<1 ou d<1) :
     * pose ErreurDetectee + retour 0 (cassure ). */
    if (y < 1 || m < 1 || d < 1) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "NumeroDeSemaine",
                              "native", "NumeroDeSemaine",
                              "arg", "date",
                              "value", "date sentinel invalide",
                              "expected", "date valide (y>=1, m>=1, d>=1)",
                              NULL);
        return INTEGER_VAL(0);
    }
    owl_clear_error();

    /* Lecture format optionnel : seule la constante iso8601 (sentinel) active
     * le mode ISO. Un entier ordinaire (0 ou 1) tombe sur le defaut canon. */
    int useISO = 0;
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        useISO = (AS_INTEGER(args[1]) == OWL_ISO8601);
    }

    return INTEGER_VAL(compute_week_number(y, m, d, useISO));
}

/* SemaineVersDate(semaine, annee) / WeekToDate(week, year) - Monday of week N */
static Value fn_semaine_vers_date(int argCount, Value* args) {
    (void)argCount;
    int week = 0, year = 0;
    if (IS_INTEGER(args[0])) week = (int)AS_INTEGER(args[0]);
    if (argCount >= 2 && IS_INTEGER(args[1])) year = (int)AS_INTEGER(args[1]);
    /* ISO: week 1 contains Jan 4. Find Monday of week 1, then add (week-1)*7 */
    int dow_jan4 = day_of_week(year, 1, 4); /* 0=Mon */
    int64_t jdn_jan4 = date_to_jdn(year, 1, 4);
    int64_t monday_w1 = jdn_jan4 - dow_jan4; /* Monday of week 1 */
    int64_t target = monday_w1 + (int64_t)(week - 1) * 7;
    int ry, rm, rd;
    jdn_to_date(target, &ry, &rm, &rd);
    return OBJ_VAL(obj_new_date(ry, rm, rd));
}

/* AnneeEnCours() / CurrentYear() */
static Value fn_annee_en_cours(int argCount, Value* args) {
    (void)argCount; (void)args;
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    return INTEGER_VAL(tm->tm_year + 1900);
}

/* MoisEnCours() / CurrentMonth() */
static Value fn_mois_en_cours(int argCount, Value* args) {
    (void)argCount; (void)args;
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    return INTEGER_VAL(tm->tm_mon + 1);
}

/* ================================================================
 * GROUPE 2 — Bornes semaine et annee
 * ================================================================ */

/* Extrait y/m/d depuis args[0] (Date/DateTime/chaîne AAAAMMJJ).
 * Retourne false si l'argument n'est pas convertible en date. */
static bool extract_ymd(Value v, int* y, int* m, int* d) {
    if (IS_DATE(v))     { *y = AS_DATE(v)->year;     *m = AS_DATE(v)->month;     *d = AS_DATE(v)->day;     return true; }
    if (IS_DATETIME(v)) { *y = AS_DATETIME(v)->year; *m = AS_DATETIME(v)->month; *d = AS_DATETIME(v)->day; return true; }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        if (s->length >= 8 && parse_date_string(s->chars, 8, y, m, d)) return true;
    }
    return false;
}

/* PremierJourDeLaSemaine(d) / FirstDayOfWeek(d) - Monday of the week */
static Value fn_premier_jour_semaine(int argCount, Value* args) {
    (void)argCount;
    int y, m, d;
    if (!extract_ymd(args[0], &y, &m, &d)) return NIL_VAL;
    int dow = day_of_week(y, m, d); /* 0=Mon */
    int64_t jdn = date_to_jdn(y, m, d) - dow;
    jdn_to_date(jdn, &y, &m, &d);
    return OBJ_VAL(obj_new_date(y, m, d));
}

/* DernierJourDeLaSemaine([d]) / LastDayOfWeek([d]) - Sunday of the week.
 * Sans argument : dimanche de la semaine courante (basé sur date système). */
static Value fn_dernier_jour_semaine(int argCount, Value* args) {
    int y, m, d;
    if (argCount == 0) {
        time_t t = time(NULL);
        struct tm* now = localtime(&t);
        y = now->tm_year + 1900;
        m = now->tm_mon + 1;
        d = now->tm_mday;
    } else if (!extract_ymd(args[0], &y, &m, &d)) return NIL_VAL;
    int dow = day_of_week(y, m, d); /* 0=Mon */
    int64_t jdn = date_to_jdn(y, m, d) + (6 - dow);
    jdn_to_date(jdn, &y, &m, &d);
    return OBJ_VAL(obj_new_date(y, m, d));
}

/* PremierJourDeLAnnee([d|annee]) / FirstDayOfYear
 * Sans argument : annee courante (canon). */
static Value fn_premier_jour_annee(int argCount, Value* args) {
    int y, m, d;
    if (argCount == 0) {
        time_t t = time(NULL);
        struct tm* now = localtime(&t);
        y = now->tm_year + 1900;
    } else if (IS_INTEGER(args[0])) y = (int)AS_INTEGER(args[0]);
    else if (extract_ymd(args[0], &y, &m, &d)) {} /* y set by helper */
    else return NIL_VAL;
    return OBJ_VAL(obj_new_date(y, 1, 1));
}

/* DernierJourDeLAnnee([d|annee]) / LastDayOfYear */
static Value fn_dernier_jour_annee(int argCount, Value* args) {
    int y, m, d;
    if (argCount == 0) {
        time_t t = time(NULL);
        struct tm* now = localtime(&t);
        y = now->tm_year + 1900;
    } else if (IS_INTEGER(args[0])) y = (int)AS_INTEGER(args[0]);
    else if (extract_ymd(args[0], &y, &m, &d)) {} /* y set by helper */
    else return NIL_VAL;
    return OBJ_VAL(obj_new_date(y, 12, 31));
}

/* ================================================================
 * GROUPE 3 — Conversions Heure, validation DateHeure
 * ================================================================ */

/* HeureVersEntier(h) / TimeToInteger(h) - centiseconds since midnight */
static Value fn_heure_vers_entier(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TIME(args[0])) return INTEGER_VAL(0);
    ObjTime* t = AS_TIME(args[0]);
    int64_t cs = (int64_t)t->hour * 360000 + (int64_t)t->minute * 6000 +
                 (int64_t)t->second * 100 + t->ms / 10;
    return INTEGER_VAL(cs);
}

/* EntierVersHeure(n) / IntegerToTime(n) - centiseconds since midnight -> Time */
static Value fn_entier_vers_heure(int argCount, Value* args) {
    (void)argCount;
    int64_t cs = 0;
    if (IS_INTEGER(args[0])) cs = AS_INTEGER(args[0]);
    else if (IS_REAL(args[0])) cs = (int64_t)AS_REAL(args[0]);
    int h = (int)(cs / 360000); cs %= 360000;
    int m = (int)(cs / 6000); cs %= 6000;
    int s = (int)(cs / 100);
    int ms = (int)(cs % 100) * 10;
    return OBJ_VAL(obj_new_time(h, m, s, ms));
}

/* DateHeureValide(dh) / DateTimeIsValid(dh) — accepte DateHeure ou chaîne
 * (formats AAAAMMJJ[HHmm[SS[FFF]]]). */
static bool dh_components_valid(int y, int m, int d, int hh, int mm, int ss) {
    if (m < 1 || m > 12 || d < 1) return false;
    if (d > days_in_month(y, m)) return false;
    if (hh < 0 || hh > 23) return false;
    if (mm < 0 || mm > 59) return false;
    if (ss < 0 || ss > 59) return false;
    return true;
}

static Value fn_dateheure_valide(int argCount, Value* args) {
    (void)argCount;
    if (IS_DATETIME(args[0])) {
        ObjDateTime* dt = AS_DATETIME(args[0]);
        return BOOL_VAL(dh_components_valid(dt->year, dt->month, dt->day,
                                             dt->hour, dt->minute, dt->second));
    }
    if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
        /* Format ISO datetime (extension naturelle , symetrie coerce). */
        if (parse_dateheure_iso(s->chars, s->length, &y, &m, &d, &hh, &mm, &ss)) {
            return BOOL_VAL(dh_components_valid(y, m, d, hh, mm, ss));
        }
        /* Longueurs canon compactes : 8, 12, 14, 16 (CC), 17 (FFF) */
        if (s->length != 8 && s->length != 12 && s->length != 14 &&
            s->length != 16 && s->length != 17) return BOOL_VAL(false);
        for (int i = 0; i < s->length; i++) {
            if (s->chars[i] < '0' || s->chars[i] > '9') return BOOL_VAL(false);
        }
        if (!parse_date_string(s->chars, 8, &y, &m, &d)) return BOOL_VAL(false);
        if (s->length >= 12) {
            hh = (s->chars[8]-'0')*10 + (s->chars[9]-'0');
            mm = (s->chars[10]-'0')*10 + (s->chars[11]-'0');
        }
        if (s->length >= 14) {
            ss = (s->chars[12]-'0')*10 + (s->chars[13]-'0');
        }
        return BOOL_VAL(dh_components_valid(y, m, d, hh, mm, ss));
    }
    return BOOL_VAL(false);
}

/* HeureDifference(h1, h2) / TimeDifference(h1, h2) - diff in centiseconds */
static Value fn_heure_difference(int argCount, Value* args) {
    (void)argCount;
    if (!IS_TIME(args[0]) || !IS_TIME(args[1])) return INTEGER_VAL(0);
    ObjTime* t1 = AS_TIME(args[0]);
    ObjTime* t2 = AS_TIME(args[1]);
    int64_t cs1 = (int64_t)t1->hour * 360000 + (int64_t)t1->minute * 6000 +
                  (int64_t)t1->second * 100 + t1->ms / 10;
    int64_t cs2 = (int64_t)t2->hour * 360000 + (int64_t)t2->minute * 6000 +
                  (int64_t)t2->second * 100 + t2->ms / 10;
    return INTEGER_VAL(cs2 - cs1);
}

/* ================================================================
 * GROUPE 4 — Noms depuis numeros
 * ================================================================ */

/* NumeroDeJourVersLettre(n) / DayNumberToName(n) - 1=Lundi ... 7=Dimanche */
static Value fn_numero_jour_vers_lettre(int argCount, Value* args) {
    (void)argCount;
    static const char* jours[] = {"","Lundi","Mardi","Mercredi","Jeudi","Vendredi","Samedi","Dimanche"};
    int n = 0;
    if (IS_INTEGER(args[0])) n = (int)AS_INTEGER(args[0]);
    if (n < 1 || n > 7) return NIL_VAL;
    return OBJ_VAL(obj_copy_string(jours[n], (int)strlen(jours[n])));
}

/* NumeroDeMoisVersLettre(n) / MonthNumberToName(n) - 1=Janvier ... 12=Decembre
 * Accepte entier 1..12 ou chaîne numérique (coerce interne). Refus explicite
 * si chaîne non parsable ou borne hors 1..12 — sans garde, mois[0] renvoie ""
 * (ou indice hors tableau), piège silencieux à neutraliser. */
static Value fn_numero_mois_vers_lettre(int argCount, Value* args) {
    (void)argCount;
    static const char* mois[] = {"","Janvier","F\xc3\xa9vrier","Mars","Avril","Mai","Juin",
                                  "Juillet","Ao\xc3\xbb""t","Septembre","Octobre","Novembre","D\xc3\xa9""cembre"};
    int64_t n = 0;
    bool ok = false;
    if (IS_INTEGER(args[0])) {
        n = AS_INTEGER(args[0]);
        ok = true;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length > 0 && s->length <= 18) {
            int64_t parsed = 0;
            int i = 0;
            int sign = 1;
            if (s->chars[0] == '-') { sign = -1; i = 1; }
            int digits = 0;
            for (; i < s->length; i++) {
                char ch = s->chars[i];
                if (ch < '0' || ch > '9') break;
                parsed = parsed * 10 + (ch - '0');
                digits++;
            }
            if (i == s->length && digits > 0) { n = sign * parsed; ok = true; }
        }
    }
    if (!ok) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NumeroDeMoisVersLettre",
                              "native", "NumeroDeMoisVersLettre",
                              "arg", "numero",
                              "expected", "entier ou chaine numerique 1..12",
                              "actual", "autre",
                              NULL);
        return NIL_VAL;
    }
    if (n < 1 || n > 12) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "NumeroDeMoisVersLettre",
                              "native", "NumeroDeMoisVersLettre",
                              "arg", "numero",
                              NULL);
        return NIL_VAL;
    }
    owl_clear_error();
    return OBJ_VAL(obj_copy_string(mois[n], (int)strlen(mois[n])));
}

/* ================================================================
 * GROUPE 5 — Jours feries
 * ================================================================ */

#define MAX_BANK_HOLIDAYS 64
static struct { int month; int day; int year; /* 0 = recurring */ } bankHolidays[MAX_BANK_HOLIDAYS];
static int bankHolidayCount = 0;

/* Helper: insère un jour férié validé dans le tableau (capacité 64).
 * year=0 = récurrent annuel. Retourne false si capacité dépassée. Validation
 * m/d déjà faite par l'appelant. */
static bool bank_holiday_push(int y, int m, int d) {
    if (bankHolidayCount >= MAX_BANK_HOLIDAYS) return false;
    bankHolidays[bankHolidayCount].month = m;
    bankHolidays[bankHolidayCount].day = d;
    bankHolidays[bankHolidayCount].year = y;
    bankHolidayCount++;
    return true;
}

/* Helper: parse une chaîne "MMJJ" (récurrent) ou "AAAAMMJJ" (année spécifique)
 * et ajoute. Retourne false si format invalide, mois/jour hors plage, ou
 * capacité dépassée. Une chaîne contenant des séparateurs CRLF ou LF est éclatée
 * et chaque ligne traitée — Faux dès qu'une ligne échoue. */
static bool bank_holiday_add_from_chars(const char* chars, int len) {
    /* Forme multi-ligne (séparateurs CRLF ou LF) : éclate et délègue. */
    bool has_separator = false;
    for (int i = 0; i < len; i++) {
        if (chars[i] == '\r' || chars[i] == '\n') { has_separator = true; break; }
    }
    if (has_separator) {
        int start = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || chars[i] == '\r' || chars[i] == '\n') {
                if (i > start) {
                    if (!bank_holiday_add_from_chars(chars + start, i - start)) return false;
                }
                start = i + 1;
            }
        }
        return true;
    }
    /* Forme unique : 4 chiffres (MMJJ) ou 8 chiffres (AAAAMMJJ). */
    if (len != 4 && len != 8) return false;
    for (int i = 0; i < len; i++) {
        if (chars[i] < '0' || chars[i] > '9') return false;
    }
    int y, m, d;
    if (len == 4) {
        y = 0;
        m = (chars[0]-'0')*10 + (chars[1]-'0');
        d = (chars[2]-'0')*10 + (chars[3]-'0');
        /* Validation jour : sur année non-bissextile pour éviter 29/02 récurrent. */
        if (m < 1 || m > 12) return false;
        if (d < 1 || d > days_in_month(2001, m)) return false;
    } else {
        y = (chars[0]-'0')*1000 + (chars[1]-'0')*100 + (chars[2]-'0')*10 + (chars[3]-'0');
        m = (chars[4]-'0')*10 + (chars[5]-'0');
        d = (chars[6]-'0')*10 + (chars[7]-'0');
        if (y < 1 || m < 1 || m > 12) return false;
        if (d < 1 || d > days_in_month(y, m)) return false;
    }
    return bank_holiday_push(y, m, d);
}

/* Dispatch sur le type de l'argument. Réutilisé par la forme tableau. */
static bool bank_holiday_add_value(Value v);

static bool bank_holiday_add_value(Value v) {
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        if (s->length == 0) return false;
        return bank_holiday_add_from_chars(s->chars, s->length);
    }
    if (IS_DATE(v)) {
        ObjDate* d = AS_DATE(v);
        return bank_holiday_push(d->year, d->month, d->day);
    }
    if (IS_DATETIME(v)) {
        ObjDateTime* dh = AS_DATETIME(v);
        return bank_holiday_push(dh->year, dh->month, dh->day);
    }
    if (IS_ARRAY(v)) {
        ObjArray* arr = AS_ARRAY(v);
        for (int i = 0; i < arr->elements.count; i++) {
            if (!bank_holiday_add_value(arr->elements.values[i])) return false;
        }
        return true;
    }
    return false;
}

/* JourFérié Ajoute(s|d|dh|tab) / BankHolidayAdd — voir signatures registry pour les
 * formes acceptées (chaîne MMJJ/AAAAMMJJ, date, dateheure, tableau).
 * Retourne Vrai si tout ajouté, Faux si un format est invalide ou la capacité
 * dépassée. */
static Value fn_jour_ferie_ajoute(int argCount, Value* args) {
    (void)argCount;
    bool ok = bank_holiday_add_value(args[0]);
    return BOOL_VAL(ok);
}

/* JourFerie(d) / BankHoliday(d) - test if date is a bank holiday */
static Value fn_jour_ferie(int argCount, Value* args) {
    (void)argCount;
    int y = 0, m = 0, d = 0;
    if (IS_DATE(args[0])) { y = AS_DATE(args[0])->year; m = AS_DATE(args[0])->month; d = AS_DATE(args[0])->day; }
    else if (IS_DATETIME(args[0])) { y = AS_DATETIME(args[0])->year; m = AS_DATETIME(args[0])->month; d = AS_DATETIME(args[0])->day; }
    else if (IS_STRING(args[0])) {
        /* Doc : JourFerie("19700714") accepte la forme AAAAMMJJ. */
        ObjString* s = AS_STRING(args[0]);
        if (!parse_date_string(s->chars, s->length, &y, &m, &d)) return BOOL_VAL(false);
    }
    else return BOOL_VAL(false);
    for (int i = 0; i < bankHolidayCount; i++) {
        if (bankHolidays[i].month == m && bankHolidays[i].day == d) {
            if (bankHolidays[i].year == 0 || bankHolidays[i].year == y)
                return BOOL_VAL(true);
        }
    }
    return BOOL_VAL(false);
}

/* JourFerieListe() / BankHolidayList() - list all registered holidays */
static Value fn_jour_ferie_liste(int argCount, Value* args) {
    (void)argCount; (void)args;
    char buf[2048];
    int off = 0;
    for (int i = 0; i < bankHolidayCount && off < 2000; i++) {
        if (i > 0) buf[off++] = '\n';
        if (bankHolidays[i].year == 0)
            off += snprintf(buf + off, 2048 - off, "%02d%02d",
                            bankHolidays[i].month, bankHolidays[i].day);
        else
            off += snprintf(buf + off, 2048 - off, "%04d%02d%02d",
                            bankHolidays[i].year, bankHolidays[i].month, bankHolidays[i].day);
    }
    return OBJ_VAL(obj_copy_string(buf, off));
}

/* JourFerieSupprimeTout() / BankHolidayDeleteAll() */
static Value fn_jour_ferie_supprime_tout(int argCount, Value* args) {
    (void)argCount; (void)args;
    bankHolidayCount = 0;
    return NIL_VAL;
}

/* ================================================================
 * FONCTIONS COMPLEMENTAIRES
 * ================================================================ */

/* Age(dateNaissance) - returns age in years */
/* Extract year/month/day from a Value (String AAAAMMJJ[HHmmSSCC], Date, or DateTime).
 * Returns true on success. */
static bool age_extract_ymd(Value v, int* y, int* m, int* d) {
    if (IS_DATE(v)) {
        *y = AS_DATE(v)->year; *m = AS_DATE(v)->month; *d = AS_DATE(v)->day;
        /*  Q4 — sentinel date invalide (y<1, m<1 ou d<1) rejete.
         * Cohere avec critere canon applique aux autres natives cassures Q2. */
        if (*y < 1 || *m < 1 || *d < 1) return false;
        return true;
    }
    if (IS_DATETIME(v)) {
        *y = AS_DATETIME(v)->year; *m = AS_DATETIME(v)->month; *d = AS_DATETIME(v)->day;
        if (*y < 1 || *m < 1 || *d < 1) return false;
        return true;
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        /* Accept 8 (AAAAMMJJ) or >=8 and ignore trailing time bytes. */
        if (s->length < 8) return false;
        return parse_date_string(s->chars, 8, y, m, d);
    }
    return false;
}

/* Age(<date_naissance>[, <date_calcul>]) : chaîne AAAAMMJJ (ans/mois/jours).
 * Retourne "" si l'un des arguments est invalide, ou si naissance > calcul. */
static Value fn_age(int argCount, Value* args) {
    if (argCount < 1) return OBJ_VAL(obj_copy_string("", 0));
    int by = 0, bm = 0, bd = 0; /* birth */
    int ry = 0, rm = 0, rd = 0; /* reference */

    if (!age_extract_ymd(args[0], &by, &bm, &bd)) {
        const char* repr = IS_STRING(args[0]) ? AS_CSTRING(args[0])
                                              : "<type non supporte>";
        owl_emit_native_error(E_NATIVE_PARSE_ERROR, "Age",
                              "native", "Age",
                              "value", repr,
                              "expected", "date de naissance valide (Date, DateHeure ou chaine AAAAMMJJ)",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    if (argCount >= 2) {
        if (!age_extract_ymd(args[1], &ry, &rm, &rd)) {
            const char* repr = IS_STRING(args[1]) ? AS_CSTRING(args[1])
                                                  : "<type non supporte>";
            owl_emit_native_error(E_NATIVE_PARSE_ERROR, "Age",
                                  "native", "Age",
                                  "value", repr,
                                  "expected", "date de reference valide (Date, DateHeure ou chaine AAAAMMJJ)",
                                  NULL);
            return OBJ_VAL(obj_copy_string("", 0));
        }
    } else {
        time_t t = time(NULL);
        struct tm* now = localtime(&t);
        ry = now->tm_year + 1900;
        rm = now->tm_mon + 1;
        rd = now->tm_mday;
    }

    /* Birth after reference -> error case per canon spec */
    long bcmp = (long)by * 10000L + (long)bm * 100L + (long)bd;
    long rcmp = (long)ry * 10000L + (long)rm * 100L + (long)rd;
    if (bcmp > rcmp) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "Age",
                              "native", "Age",
                              "arg", "naissance",
                              "value", "naissance posterieure a la reference",
                              "expected", "date de naissance <= date de reference",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    int years  = ry - by;
    int months = rm - bm;
    int days   = rd - bd;

    if (days < 0) {
        /* borrow from previous month */
        int prev_m = rm - 1, prev_y = ry;
        if (prev_m == 0) { prev_m = 12; prev_y--; }
        days += days_in_month(prev_y, prev_m);
        months--;
    }
    if (months < 0) {
        months += 12;
        years--;
    }

    char buf[40];
    snprintf(buf, sizeof buf, "%04d%02d%02d", years, months, days);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* Demain(d?) / Tomorrow(d?) - tomorrow or day after d */
static Value fn_demain(int argCount, Value* args) {
    if (argCount >= 1 && IS_DATE(args[0])) {
        return owl_date_add_days(args[0], 1);
    }
    time_t t = time(NULL);
    struct tm* now = localtime(&t);
    ObjDate* today = obj_new_date(now->tm_year + 1900, now->tm_mon + 1, now->tm_mday);
    return owl_date_add_days(OBJ_VAL(today), 1);
}

/* Hier(d?) / Yesterday(d?) - yesterday or day before d */
static Value fn_hier(int argCount, Value* args) {
    if (argCount >= 1 && IS_DATE(args[0])) {
        return owl_date_add_days(args[0], -1);
    }
    time_t t = time(NULL);
    struct tm* now = localtime(&t);
    ObjDate* today = obj_new_date(now->tm_year + 1900, now->tm_mon + 1, now->tm_mday);
    return owl_date_add_days(OBJ_VAL(today), -1);
}

/* JourEnCours() / CurrentDay() - current day number in month */
static Value fn_jour_en_cours(int argCount, Value* args) {
    (void)argCount; (void)args;
    time_t t = time(NULL);
    struct tm* now = localtime(&t);
    return INTEGER_VAL(now->tm_mday);
}

/* JourDansLAnneeVersDate(jour, annee) / DayOfYearToDate(day, year) */
static Value fn_jour_annee_vers_date(int argCount, Value* args) {
    if (argCount < 1) return NIL_VAL;
    int dayNum = 0, year = 0;
    if (IS_INTEGER(args[0])) dayNum = (int)AS_INTEGER(args[0]);
    if (argCount >= 2 && IS_INTEGER(args[1])) year = (int)AS_INTEGER(args[1]);
    if (year == 0) { time_t t = time(NULL); struct tm* now = localtime(&t); year = now->tm_year + 1900; }
    int64_t jan1 = date_to_jdn(year, 1, 1);
    int y, m, d;
    jdn_to_date(jan1 + dayNum - 1, &y, &m, &d);
    return OBJ_VAL(obj_new_date(y, m, d));
}

/* EntierVersJour(n) / IntegerToDay(n) - weekday from integer date (days since 1800) */
static Value fn_entier_vers_jour(int argCount, Value* args) {
    (void)argCount;
    int64_t n = 0;
    if (IS_INTEGER(args[0])) n = AS_INTEGER(args[0]);
    int64_t epoch = date_to_jdn(1800, 1, 1);
    int y, m, d;
    /* Convention canon : jour 1 = 01/01/1800, donc -1 (cohérent EntierVersDate). */
    jdn_to_date(epoch + n - 1, &y, &m, &d);
    return INTEGER_VAL(day_of_week(y, m, d) + 1);
}

/* EntierVersJourEnLettre(jour) / IntegerToDayInAlpha(day)
 * Accepte entier (jours depuis 01/01/1800, base 1), chaîne numérique, ou date
 * (coerce interne via JDN). Refus explicite si chaîne non parsable ou
 * entier < 1 (plus strict que la doc canon qui ignore silencieusement). */
static Value fn_entier_vers_jour_lettre(int argCount, Value* args) {
    (void)argCount;
    static const char* jours[] = {"Lundi","Mardi","Mercredi","Jeudi","Vendredi","Samedi","Dimanche"};
    int64_t n = 0;
    bool ok = false;
    if (IS_INTEGER(args[0])) {
        n = AS_INTEGER(args[0]);
        ok = true;
    } else if (IS_DATE(args[0])) {
        ObjDate* dt = AS_DATE(args[0]);
        n = date_to_jdn(dt->year, dt->month, dt->day) - date_to_jdn(1800, 1, 1) + 1;
        ok = true;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length > 0 && s->length <= 18) {
            int64_t parsed = 0;
            int i = 0;
            int sign = 1;
            if (s->chars[0] == '-') { sign = -1; i = 1; }
            int digits = 0;
            for (; i < s->length; i++) {
                char ch = s->chars[i];
                if (ch < '0' || ch > '9') break;
                parsed = parsed * 10 + (ch - '0');
                digits++;
            }
            if (i == s->length && digits > 0) { n = sign * parsed; ok = true; }
        }
    }
    if (!ok) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EntierVersJourEnLettre",
                              "native", "EntierVersJourEnLettre",
                              "arg", "n",
                              "expected", "entier, chaine numerique ou date",
                              "actual", "autre",
                              NULL);
        return NIL_VAL;
    }
    if (n < 1) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "EntierVersJourEnLettre",
                              "native", "EntierVersJourEnLettre",
                              "arg", "n",
                              NULL);
        return NIL_VAL;
    }
    owl_clear_error();
    int64_t epoch = date_to_jdn(1800, 1, 1);
    int y, m, d;
    jdn_to_date(epoch + n - 1, &y, &m, &d);
    int dow = day_of_week(y, m, d);
    return OBJ_VAL(obj_copy_string(jours[dow], (int)strlen(jours[dow])));
}

/* EntierVersMoisEnLettre(n) / IntegerToMonthInAlpha(n)
 * Accepte entier (jours depuis 01/01/1800, base 1), chaîne numérique, ou date
 * (coerce interne via JDN). Refus explicite si chaîne non parsable ou
 * entier < 1 — sans garde, mois[0] renvoie "" (ou indice négatif lit hors
 * tableau), comportement piège silencieux à neutraliser. */
static Value fn_entier_vers_mois_lettre(int argCount, Value* args) {
    (void)argCount;
    static const char* mois[] = {"","Janvier","F\xc3\xa9vrier","Mars","Avril","Mai","Juin",
                                  "Juillet","Ao\xc3\xbb""t","Septembre","Octobre","Novembre","D\xc3\xa9""cembre"};
    int64_t n = 0;
    bool ok = false;
    if (IS_INTEGER(args[0])) {
        n = AS_INTEGER(args[0]);
        ok = true;
    } else if (IS_DATE(args[0])) {
        ObjDate* dt = AS_DATE(args[0]);
        n = date_to_jdn(dt->year, dt->month, dt->day) - date_to_jdn(1800, 1, 1) + 1;
        ok = true;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length > 0 && s->length <= 18) {
            int64_t parsed = 0;
            int i = 0;
            int sign = 1;
            if (s->chars[0] == '-') { sign = -1; i = 1; }
            int digits = 0;
            for (; i < s->length; i++) {
                char ch = s->chars[i];
                if (ch < '0' || ch > '9') break;
                parsed = parsed * 10 + (ch - '0');
                digits++;
            }
            if (i == s->length && digits > 0) { n = sign * parsed; ok = true; }
        }
    }
    if (!ok) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EntierVersMoisEnLettre",
                              "native", "EntierVersMoisEnLettre",
                              "arg", "n",
                              "expected", "entier, chaine numerique ou date",
                              "actual", "autre",
                              NULL);
        return NIL_VAL;
    }
    if (n < 1) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "EntierVersMoisEnLettre",
                              "native", "EntierVersMoisEnLettre",
                              "arg", "n",
                              NULL);
        return NIL_VAL;
    }
    owl_clear_error();
    int64_t epoch = date_to_jdn(1800, 1, 1);
    int y, m, d;
    jdn_to_date(epoch + n - 1, &y, &m, &d);
    return OBJ_VAL(obj_copy_string(mois[m], (int)strlen(mois[m])));
}

/* EntierVersNumeroDeSemaine(n [, format]) / IntegerToWeekNumber(n [, format])
 * n est un entier representant le nombre de jours ecoules depuis le
 * 01/01/1800 (base 1, doc 3027030). Aligne sur DateVersEntier.
 * format optionnel : seule la constante sentinel iso8601 active le mode
 * ISO 8601 strict. Defaut : convention canon (semaine 0 pour les
 * tout debuts de janvier hors S1, semaine 53 pour fin decembre si la
 * semaine bascule sur l'annee suivante). */
static Value fn_entier_vers_semaine(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    int64_t n = 0;
    if (IS_INTEGER(args[0])) n = AS_INTEGER(args[0]);
    else if (IS_REAL(args[0])) n = (int64_t)AS_REAL(args[0]);
    else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EntierVersNumeroDeSemaine",
                              "native", "EntierVersNumeroDeSemaine",
                              "arg", "n",
                              "expected", "entier (jours depuis 01/01/1800, base 1)",
                              "actual", "autre",
                              NULL);
        return INTEGER_VAL(0);
    }
    int64_t epoch = date_to_jdn(1800, 1, 1);
    int y, m, d;
    jdn_to_date(epoch + n - 1, &y, &m, &d);
    if (y < 1 || m < 1 || m > 12 || d < 1 || d > 31) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "EntierVersNumeroDeSemaine",
                              "native", "EntierVersNumeroDeSemaine",
                              "arg", "n",
                              "value", "entier hors plage de dates valides",
                              "expected", "n produit une date >= 01/01/0001",
                              NULL);
        return INTEGER_VAL(0);
    }
    owl_clear_error();

    /* Lecture format optionnel : seule la sentinel iso8601 active ISO. */
    int useISO = 0;
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        useISO = (AS_INTEGER(args[1]) == OWL_ISO8601);
    }
    return INTEGER_VAL(compute_week_number(y, m, d, useISO));
}

/* Paques(annee) / Easter(year) - Computus (algorithme grégorien anonyme).
 * Accepte entier, chaîne numérique ("AAAA"), ou date (extraction de l'année).
 * Hors bornes 1583-9999 ou chaîne non parsable → owl_set_error + NIL.
 * Borne basse 1583 : la réforme grégorienne a pris effet en octobre 1582 ;
 * l'algorithme n'a plus de sens en deçà. Borne haute 9999 : limite usuelle
 * du calendrier grégorien dans les systèmes informatiques. */
static Value fn_paques(int argCount, Value* args) {
    (void)argCount;
    int y = 0;
    bool ok = false;
    if (IS_INTEGER(args[0])) {
        y = (int)AS_INTEGER(args[0]);
        ok = true;
    } else if (IS_DATE(args[0])) {
        y = AS_DATE(args[0])->year;
        ok = true;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        if (s->length > 0 && s->length <= 9) {
            int parsed = 0;
            int i = 0;
            for (; i < s->length; i++) {
                char ch = s->chars[i];
                if (ch < '0' || ch > '9') break;
                parsed = parsed * 10 + (ch - '0');
            }
            if (i == s->length) { y = parsed; ok = true; }
        }
    }
    if (!ok) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Paques",
                              "native", "Paques",
                              "arg", "annee",
                              "expected", "entier, chaine numerique ou date",
                              "actual", "autre",
                              NULL);
        return NIL_VAL;
    }
    if (y < 1583 || y > 9999) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "Paques",
                              "native", "Paques",
                              "arg", "annee",
                              NULL);
        return NIL_VAL;
    }
    owl_clear_error();
    /* Anonymous Gregorian algorithm */
    int a = y % 19;
    int b = y / 100;
    int c = y % 100;
    int d = b / 4;
    int e = b % 4;
    int f = (b + 8) / 25;
    int g = (b - f + 1) / 3;
    int h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4;
    int k = c % 4;
    int l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;
    int month = (h + l - 7 * m + 114) / 31;
    int day = ((h + l - 7 * m + 114) % 31) + 1;
    return OBJ_VAL(obj_new_date(y, month, day));
}

/* Convert a Value (DateTime, Date, or string in canon format) to absolute ms.
 * canon formats accepted for strings (per doc):
 *   AAAAMMJJ           (8, date only)
 *   AAAAMMJJHH         (10)
 *   AAAAMMJJHHmm       (12)
 *   AAAAMMJJHHmmSS     (14)
 *   AAAAMMJJHHmmSSCC   (16, CC = centièmes → ms*10)
 *   AAAAMMJJHHmmSSFFF  (17, FFF = millièmes, extension OWL maskDateEmail)
 * Returns true on success, false if format unrecognized. */
bool owl_value_to_absolute_ms(Value v, int64_t* outMs) {
    if (IS_DATETIME(v)) {
        ObjDateTime* dt = AS_DATETIME(v);
        *outMs = date_to_jdn(dt->year, dt->month, dt->day) * 86400000LL +
                 (int64_t)dt->hour * 3600000 + (int64_t)dt->minute * 60000 +
                 (int64_t)dt->second * 1000 + dt->ms;
        return true;
    }
    if (IS_DATE(v)) {
        ObjDate* d = AS_DATE(v);
        *outMs = date_to_jdn(d->year, d->month, d->day) * 86400000LL;
        return true;
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0, ms = 0;
        if (s->length < 8) return false;
        if (!parse_date_string(s->chars, 8, &y, &mo, &d)) return false;
        for (int i = 8; i < s->length && i < 17; i++) {
            if (s->chars[i] < '0' || s->chars[i] > '9') return false;
        }
        if (s->length >= 10) hh = (s->chars[8]-'0')*10 + (s->chars[9]-'0');
        if (s->length >= 12) mm = (s->chars[10]-'0')*10 + (s->chars[11]-'0');
        if (s->length >= 14) ss = (s->chars[12]-'0')*10 + (s->chars[13]-'0');
        if (s->length == 16) { /* CC centièmes */
            ms = ((s->chars[14]-'0')*10 + (s->chars[15]-'0')) * 10;
        } else if (s->length >= 17) { /* FFF millièmes */
            ms = (s->chars[14]-'0')*100 + (s->chars[15]-'0')*10 + (s->chars[16]-'0');
        }
        *outMs = date_to_jdn(y, mo, d) * 86400000LL +
                 (int64_t)hh * 3600000 + (int64_t)mm * 60000 +
                 (int64_t)ss * 1000 + ms;
        return true;
    }
    return false;
}

/* DateHeureDifference(dh1, dh2) — retourne directement une Durée (ms2 - ms1).
 * Symétrique avec l'opérateur '-' sur dateheures. Storage Durée en ms canon.
 * Cf.  (divergence canon assumée pour cohérence fonction/opérateur). */
static Value fn_dateheure_difference(int argCount, Value* args) {
    (void)argCount;
    int64_t ms1 = 0, ms2 = 0;
    owl_value_to_absolute_ms(args[0], &ms1);
    owl_value_to_absolute_ms(args[1], &ms2);
    return OBJ_VAL(obj_new_duration(ms2 - ms1));
}

/* Parse une forme courte "<chiffres><unité>" vers ms.
 *
 * Unités acceptées (insensible à la casse) :
 *   ms = millisecondes
 *   cs = centièmes de seconde     (×10)
 *   s  = secondes                  (×1000)
 *   mn = minutes                   (×60000)
 *
 * Espaces blancs en tête/fin tolérés (« 200ms », «  10 cs  », « 1s »).
 * Le nombre doit être un entier positif (pas de signe, pas de décimale).
 *
 * Retourne le total en millisecondes (toujours >= 0), ou -1 si la chaîne
 * n'est pas reconnue. La valeur 0 est valide (« 0ms » → 0).
 */
int64_t owl_duree_parse_short_form(const char* s, int slen) {
    if (s == NULL || slen <= 0) return -1;
    int i = 0;
    /* Skip leading whitespace */
    while (i < slen && (s[i] == ' ' || s[i] == '\t')) i++;
    /* Read digits — au moins un */
    int64_t n = 0;
    int ndig = 0;
    while (i < slen && s[i] >= '0' && s[i] <= '9') {
        if (n > (INT64_MAX - 9) / 10) return -1; /* overflow guard */
        n = n * 10 + (s[i] - '0');
        i++;
        ndig++;
    }
    if (ndig == 0) return -1;
    /* Optional whitespace between number and unit */
    while (i < slen && (s[i] == ' ' || s[i] == '\t')) i++;
    /* Unit — au moins une lettre */
    if (i >= slen) return -1;
    char u0 = s[i] | 0x20; /* lowercase ASCII */
    char u1 = (i + 1 < slen) ? (char)(s[i + 1] | 0x20) : '\0';
    int consumed = 0;
    int64_t mult = 0;
    if (u0 == 'm' && u1 == 's')                { mult = 1LL;     consumed = 2; }
    else if (u0 == 'c' && u1 == 's')           { mult = 10LL;    consumed = 2; }
    else if (u0 == 'm' && u1 == 'n')           { mult = 60000LL; consumed = 2; }
    else if (u0 == 's' && (u1 == '\0' || u1 == ' ' || u1 == '\t')) {
        mult = 1000LL; consumed = 1;
    }
    else return -1;
    i += consumed;
    /* Trailing whitespace only */
    while (i < slen && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != slen) return -1; /* trailing garbage */
    /* Overflow guard sur la multiplication */
    if (mult != 0 && n > INT64_MAX / mult) return -1;
    return n * mult;
}

/* Constants for ChaîneVersDurée format (doc 3027002) */
#define OWL_DUREE_CENTIEME  1   /* format: +JHHMMSSCC  (10 chars) */
#define OWL_DUREE_MILLIEME  2   /* format: +JHHMMSSCCC (11 chars) */

/* Parse a duration string with canon format template tokens:
 *   J   = jours (variable width)
 *   HH  = heures (2 digits)
 *   MM  = minutes (2 digits)
 *   SS  = secondes (2 digits)
 *   CCC = millisecondes (3 digits)
 *   CC  = centièmes (2 digits)
 * Example: "3 jours, 13 heures" with format "J jours, HH heures"
 *
 * Returns total milliseconds, or INT64_MIN on parse failure. */
#define DUREE_PARSE_FAIL INT64_MIN

static int64_t parse_duree_with_format(const char* s, int slen,
                                       const char* fmt, int flen) {
    int64_t jours = 0, hh = 0, mm = 0, ss = 0, ccc = 0;
    int si = 0, fi = 0;

    while (fi < flen && si < slen) {
        int remaining = slen - si;

        /* CCC (3 digits millisecondes) */
        if (fi + 2 < flen && strncmp(fmt + fi, "CCC", 3) == 0) {
            if (remaining < 3) return DUREE_PARSE_FAIL;
            for (int k = 0; k < 3; k++)
                if (s[si+k] < '0' || s[si+k] > '9') return DUREE_PARSE_FAIL;
            ccc = (s[si]-'0')*100 + (s[si+1]-'0')*10 + (s[si+2]-'0');
            si += 3; fi += 3;
            continue;
        }
        /* CC (2 digits centièmes -> convert to ms via *10) */
        if (fi + 1 < flen && strncmp(fmt + fi, "CC", 2) == 0) {
            if (remaining < 2) return DUREE_PARSE_FAIL;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return DUREE_PARSE_FAIL;
            int cs = (s[si]-'0')*10 + (s[si+1]-'0');
            ccc = cs * 10; /* centièmes → ms */
            si += 2; fi += 2;
            continue;
        }
        /* HH */
        if (fi + 1 < flen && strncmp(fmt + fi, "HH", 2) == 0) {
            if (remaining < 2) return DUREE_PARSE_FAIL;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return DUREE_PARSE_FAIL;
            hh = (s[si]-'0')*10 + (s[si+1]-'0');
            si += 2; fi += 2;
            continue;
        }
        /* MM (minutes) */
        if (fi + 1 < flen && strncmp(fmt + fi, "MM", 2) == 0) {
            if (remaining < 2) return DUREE_PARSE_FAIL;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return DUREE_PARSE_FAIL;
            mm = (s[si]-'0')*10 + (s[si+1]-'0');
            si += 2; fi += 2;
            continue;
        }
        /* SS */
        if (fi + 1 < flen && strncmp(fmt + fi, "SS", 2) == 0) {
            if (remaining < 2) return DUREE_PARSE_FAIL;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return DUREE_PARSE_FAIL;
            ss = (s[si]-'0')*10 + (s[si+1]-'0');
            si += 2; fi += 2;
            continue;
        }
        /* J (jours, variable width: greedy mais laisse assez de chars pour
         * les tokens restants du format). */
        if (fmt[fi] == 'J') {
            /* Count chars required by the rest of the format (tokens + literals) */
            int needed = 0;
            for (int k = fi + 1; k < flen; k++) {
                if (k + 2 < flen && strncmp(fmt + k, "CCC", 3) == 0) { needed += 3; k += 2; }
                else if (k + 1 < flen && strncmp(fmt + k, "CC", 2) == 0) { needed += 2; k += 1; }
                else if (k + 1 < flen && strncmp(fmt + k, "HH", 2) == 0) { needed += 2; k += 1; }
                else if (k + 1 < flen && strncmp(fmt + k, "MM", 2) == 0) { needed += 2; k += 1; }
                else if (k + 1 < flen && strncmp(fmt + k, "SS", 2) == 0) { needed += 2; k += 1; }
                else if (fmt[k] == 'J') { needed += 1; /* J counts as min 1 */ }
                else { needed += 1; /* literal */ }
            }
            int avail = slen - si;
            int jlen = avail - needed;
            if (jlen < 1) jlen = 1; /* at least one digit for J */
            /* Read exactly jlen digits (or fewer if the input runs out of digits,
             * which means we need to let literal separators kick in). Actually:
             * read digits greedily, up to jlen or until non-digit encountered. */
            int read = 0;
            while (read < jlen && si + read < slen &&
                   s[si+read] >= '0' && s[si+read] <= '9') {
                jours = jours * 10 + (s[si+read] - '0');
                read++;
            }
            if (read == 0) return DUREE_PARSE_FAIL;
            si += read; fi += 1;
            continue;
        }
        /* Literal separator: must match */
        if (s[si] != fmt[fi]) return DUREE_PARSE_FAIL;
        si++; fi++;
    }
    if (fi < flen || si < slen) return DUREE_PARSE_FAIL;

    /*  Q5-bis — bornes runtime sur composants HHMMSS.
     * Symetrique avec owl_parse_duration_multi. */
    if (hh > 23 || mm > 59 || ss > 59) return DUREE_PARSE_FAIL;

    return jours * 86400000LL
         + hh * 3600000LL
         + mm * 60000LL
         + ss * 1000LL
         + ccc;
}

/*  — moteur de masque DURÉE dédié au champ de saisie typSaisieDurée.
 * Distinct du parseur durée canon (parse_duree_with_format ci-dessus) sur UN
 * point crucial : une DURÉE n'est pas une heure, donc HH n'est PAS plafonné à
 * 23 quand aucun jeton jour n'est présent (une durée de 99 h existe). Le
 * vocabulaire de jetons est aussi celui de la durée : les minutes s'écrivent
 * MI/mm (jamais MM, qui désigne les mois partout ailleurs dans OWL, ).
 * Jetons : J (jours variable width),
 * JJ (jours 2 chiffres), HH (heures), MI/mm (minutes), SS (secondes), CC
 * (centièmes → *10 ms), CCC (millièmes). Aucun cap runtime : la frappe est
 * bornée en amont par le validateur Qt (owl_ui_lineedit_set_duree_validator),
 * ce parseur ne fait que décomposer un texte guidé/libre en totalMs.
 * Retourne true et *outMs si le texte correspond EXACTEMENT au masque. */
bool owl_duree_parse_with_mask(const char* s, int slen,
                               const char* fmt, int flen, int64_t* outMs) {
    int64_t jours = 0, hh = 0, mm = 0, ss = 0, ms = 0;
    int si = 0, fi = 0;
    while (fi < flen && si < slen) {
        int remaining = slen - si;
        if (fi + 2 < flen && strncmp(fmt + fi, "CCC", 3) == 0) {
            if (remaining < 3) return false;
            for (int k = 0; k < 3; k++)
                if (s[si+k] < '0' || s[si+k] > '9') return false;
            ms = (s[si]-'0')*100 + (s[si+1]-'0')*10 + (s[si+2]-'0');
            si += 3; fi += 3; continue;
        }
        if (fi + 1 < flen && strncmp(fmt + fi, "CC", 2) == 0) {
            if (remaining < 2) return false;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return false;
            ms = ((s[si]-'0')*10 + (s[si+1]-'0')) * 10; /* centièmes → ms */
            si += 2; fi += 2; continue;
        }
        if (fi + 1 < flen && strncmp(fmt + fi, "JJ", 2) == 0) {
            if (remaining < 2) return false;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return false;
            jours = (s[si]-'0')*10 + (s[si+1]-'0');
            si += 2; fi += 2; continue;
        }
        if (fi + 1 < flen && strncmp(fmt + fi, "HH", 2) == 0) {
            if (remaining < 2) return false;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return false;
            hh = (s[si]-'0')*10 + (s[si+1]-'0'); /* PAS de cap 23 : durée */
            si += 2; fi += 2; continue;
        }
        if (fi + 1 < flen && (strncmp(fmt + fi, "MI", 2) == 0 ||
                              strncmp(fmt + fi, "mm", 2) == 0)) {
            if (remaining < 2) return false;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return false;
            mm = (s[si]-'0')*10 + (s[si+1]-'0'); /* MINUTES (MI/mm ; MM=mois ailleurs) */
            si += 2; fi += 2; continue;
        }
        if (fi + 1 < flen && strncmp(fmt + fi, "SS", 2) == 0) {
            if (remaining < 2) return false;
            if (s[si] < '0' || s[si] > '9' || s[si+1] < '0' || s[si+1] > '9')
                return false;
            ss = (s[si]-'0')*10 + (s[si+1]-'0');
            si += 2; fi += 2; continue;
        }
        /* J seul : jours largeur variable (greedy, laisse assez de chars aux
         * jetons/littéraux restants). */
        if (fmt[fi] == 'J') {
            int needed = 0;
            for (int k = fi + 1; k < flen; k++) {
                if (k + 2 < flen && strncmp(fmt + k, "CCC", 3) == 0) { needed += 3; k += 2; }
                else if (k + 1 < flen && strncmp(fmt + k, "CC", 2) == 0) { needed += 2; k += 1; }
                else if (k + 1 < flen && strncmp(fmt + k, "JJ", 2) == 0) { needed += 2; k += 1; }
                else if (k + 1 < flen && strncmp(fmt + k, "HH", 2) == 0) { needed += 2; k += 1; }
                else if (k + 1 < flen && (strncmp(fmt + k, "MI", 2) == 0 ||
                                          strncmp(fmt + k, "mm", 2) == 0)) { needed += 2; k += 1; }
                else if (k + 1 < flen && strncmp(fmt + k, "SS", 2) == 0) { needed += 2; k += 1; }
                else if (fmt[k] == 'J') { needed += 1; }
                else { needed += 1; }
            }
            int avail = slen - si;
            int jlen = avail - needed;
            if (jlen < 1) jlen = 1;
            int read = 0;
            while (read < jlen && si + read < slen &&
                   s[si+read] >= '0' && s[si+read] <= '9') {
                jours = jours * 10 + (s[si+read] - '0');
                read++;
            }
            if (read == 0) return false;
            si += read; fi += 1; continue;
        }
        /* littéral : doit correspondre */
        if (s[si] != fmt[fi]) return false;
        si++; fi++;
    }
    if (fi < flen || si < slen) return false;
    if (outMs != NULL)
        *outMs = jours * 86400000LL + hh * 3600000LL
               + mm * 60000LL + ss * 1000LL + ms;
    return true;
}

/*  — formate une durée (totalMs) selon un masque durée dans out
 * (null-terminé). La présence d'un jeton jour (J/JJ) détermine la décomposition :
 * avec jour, les heures portent le reste (0-23) ; sans jour, HH absorbe toutes
 * les heures (peut dépasser 23, tronqué à 2 chiffres par le format — MVP). */
int owl_duree_format_with_mask(char* out, int outsz,
                               const char* fmt, int flen, int64_t totalMs) {
    if (out == NULL || outsz == 0) return 0;
    out[0] = '\0';
    int64_t ms = totalMs < 0 ? -totalMs : totalMs;
    bool neg = totalMs < 0;
    bool hasDay = false;
    for (int k = 0; k < flen; k++) if (fmt[k] == 'J') { hasDay = true; break; }
    int64_t days = hasDay ? (ms / 86400000LL) : 0;
    int64_t rem = ms - days * 86400000LL;
    int64_t hh = rem / 3600000LL;
    int mm = (int)((rem % 3600000LL) / 60000LL);
    int ss = (int)((rem % 60000LL) / 1000LL);
    int msr = (int)(rem % 1000LL);
    int oi = 0;
    if (neg && oi + 1 < outsz) out[oi++] = '-';
    int fi = 0;
    while (fi < flen && oi + 1 < outsz) {
        char tmp[16];
        int n = 0;
        if (fi + 2 < flen && strncmp(fmt + fi, "CCC", 3) == 0) {
            n = snprintf(tmp, sizeof(tmp), "%03d", msr); fi += 3;
        } else if (fi + 1 < flen && strncmp(fmt + fi, "CC", 2) == 0) {
            n = snprintf(tmp, sizeof(tmp), "%02d", msr / 10); fi += 2;
        } else if (fi + 1 < flen && strncmp(fmt + fi, "JJ", 2) == 0) {
            n = snprintf(tmp, sizeof(tmp), "%02lld", (long long)days); fi += 2;
        } else if (fi + 1 < flen && strncmp(fmt + fi, "HH", 2) == 0) {
            n = snprintf(tmp, sizeof(tmp), "%02lld", (long long)hh); fi += 2;
        } else if (fi + 1 < flen && (strncmp(fmt + fi, "MI", 2) == 0 ||
                                     strncmp(fmt + fi, "mm", 2) == 0)) {
            n = snprintf(tmp, sizeof(tmp), "%02d", mm); fi += 2;
        } else if (fi + 1 < flen && strncmp(fmt + fi, "SS", 2) == 0) {
            n = snprintf(tmp, sizeof(tmp), "%02d", ss); fi += 2;
        } else if (fmt[fi] == 'J') {
            n = snprintf(tmp, sizeof(tmp), "%lld", (long long)days); fi += 1;
        } else {
            tmp[0] = fmt[fi++]; n = 1; tmp[1] = '\0';
        }
        for (int k = 0; k < n && oi + 1 < outsz; k++) out[oi++] = tmp[k];
    }
    out[oi] = '\0';
    return oi;
}

/* ChaineVersDuree(s, format) / StringToDuration — canon
 * Signature 2-arg canon stricte (cf. ). La forme 1-arg
 * non-canon a été retirée (rejet compile-time).
 * Forme A (constante mask): duréeCentième ("+JHHMMSSCC") ou duréeMillième ("+JHHMMSSCCC")
 * Forme B (template chaîne): tokens J/HH/MM/SS/CCC dans une chaîne libre.
 *
 * Signe: préfixe '+' ou '-' accepté dans la chaîne d'entrée.
 */
static Value fn_chaine_vers_duree(int argCount, Value* args) {
    /*  Q2 — cassure : tout chemin d'echec parse pose
     * ErreurDetectee au lieu d'un Duree(0) silencieux. */
    if (!IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "ChaineVersDuree",
                              "native", "ChaineVersDuree",
                              "arg", "s",
                              "expected", "chaine",
                              "actual", "autre",
                              NULL);
        return OBJ_VAL(obj_new_duration(0));
    }
    ObjString* s = AS_STRING(args[0]);

    /* Garde défensif: la signature registry exige 2 args (rejet
     * compile-time). En cas d'invocation runtime hors compiler, on
     * renvoie une durée nulle plutôt que de planter. */
    if (argCount >= 2) {
        if (IS_INTEGER(args[1])) {
            int mask = (int)AS_INTEGER(args[1]);
            const char* fmt = NULL;
            int flen = 0;
            if (mask == OWL_DUREE_CENTIEME)  { fmt = "+JHHMMSSCC";  flen = 10; }
            else if (mask == OWL_DUREE_MILLIEME) { fmt = "+JHHMMSSCCC"; flen = 11; }
            if (fmt != NULL) {
                /* Signe obligatoire ( / Q5-bis) : fmt commence par
                 * '+' literal, le '+' input matche. Pour '-' on retry sans
                 * '+' literal puis on flip le signe. Pas de fallback
                 * "sans signe" — cassure assumee (cf. test parser parity). */
                int64_t ms = parse_duree_with_format(s->chars, s->length, fmt, flen);
                if (ms == DUREE_PARSE_FAIL && s->length > 0 && s->chars[0] == '-') {
                    ms = parse_duree_with_format(s->chars + 1, s->length - 1,
                                                 fmt + 1, flen - 1);
                    if (ms != DUREE_PARSE_FAIL) ms = -ms;
                }
                if (ms == DUREE_PARSE_FAIL) {
                    owl_emit_native_error(E_NATIVE_PARSE_ERROR, "ChaineVersDuree",
                                          "native", "ChaineVersDuree",
                                          "value", "chaine invalide",
                                          "expected", "[+/-]JHHMMSSCC ou [+/-]JHHMMSSCCC",
                                          NULL);
                    return OBJ_VAL(obj_new_duration(0));
                }
                owl_clear_error();
                return OBJ_VAL(obj_new_duration(ms));
            }
            /* Mask inconnu : pose ErreurDetectee (cassure Q2). */
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "ChaineVersDuree",
                                  "native", "ChaineVersDuree",
                                  "arg", "format",
                                  "value", "masque format inconnu",
                                  "expected", "dureeCentieme ou dureeMillieme",
                                  NULL);
            return OBJ_VAL(obj_new_duration(0));
        } else if (IS_STRING(args[1])) {
            ObjString* fmt = AS_STRING(args[1]);
            /* Handle optional leading sign in input */
            int sign = 1;
            const char* sp = s->chars;
            int slen = s->length;
            if (slen > 0 && (sp[0] == '+' || sp[0] == '-')) {
                if (sp[0] == '-') sign = -1;
                /* If format starts with '+' or '-', consume it as literal;
                 * else skip the sign silently in input. */
                if (fmt->length > 0 && (fmt->chars[0] == '+' || fmt->chars[0] == '-')) {
                    /* keep sp as-is, let parser match literal sign */
                } else {
                    sp++; slen--;
                }
            }
            int64_t ms = parse_duree_with_format(sp, slen, fmt->chars, fmt->length);
            if (ms == DUREE_PARSE_FAIL) {
                owl_emit_native_error(E_NATIVE_PARSE_ERROR, "ChaineVersDuree",
                                      "native", "ChaineVersDuree",
                                      "value", "chaine invalide pour format donne",
                                      "expected", "chaine conforme au masque format",
                                      NULL);
                return OBJ_VAL(obj_new_duration(0));
            }
            owl_clear_error();
            return OBJ_VAL(obj_new_duration(sign * ms));
        }
    }

    /* argCount < 2 ne devrait pas arriver (compiler bloque), garde
     * défensif: pose ErreurDetectee + durée nulle. */
    owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "ChaineVersDuree",
                          "native", "ChaineVersDuree",
                          "arg", "format",
                          "value", "arguments manquants",
                          "expected", "(chaine, masque)",
                          NULL);
    return OBJ_VAL(obj_new_duration(0));
}

/* DateHeureVersChaine / DateTimeToString
 * Délègue à fn_date_vers_chaine qui gère DateTime + formats lettres FR. */
static Value fn_dateheure_vers_chaine(int argCount, Value* args) {
    return fn_date_vers_chaine(argCount, args);
}

/* ================================================================
 * RéelVersDateHeure / DateHeureVersRéel — doc 1000021364 / 1000021362
 *
 * Excel sérialisation : entier = numéro du jour, fraction = fraction du jour.
 *   Mode défaut (1900) : jour 1 = 01/01/1900
 *   Mode 1904          : jour 0 = 01/01/1904
 *
 * Excel quirk : 1900 est traité comme année bissextile (bug historique
 * conservé pour compat). En conséquence le jour 60 = 29/02/1900 (fictif),
 * et 01/03/1900 = jour 61. Le canon suit ce comportement.
 * Notre implémentation suit Excel : on calcule en JDN avec le jour 60 saute.
 * ================================================================ */

/* Convertit "Excel days since 1900" en (year, month, day).
 * mode1904 = false : jour 1 = 01/01/1900
 * mode1904 = true  : jour 0 = 01/01/1904
 * Bug Excel : 1900 traité comme bissextile, donc jour 60 = 29/02/1900
 * (date fictive renvoyée littéralement). */
static void excel_days_to_date(double days, bool mode1904, int* y, int* m, int* d) {
    int64_t whole = (int64_t)days;
    if (mode1904) {
        /* 1904 mode: jour 0 = 01/01/1904, pas de bug bissextile */
        int64_t jdn = date_to_jdn(1904, 1, 1) + whole;
        jdn_to_date(jdn, y, m, d);
        return;
    }
    /* Cas spécial : jour 60 = 29/02/1900 fictif. */
    if (whole == 60) {
        *y = 1900; *m = 2; *d = 29;
        return;
    }
    /* Mode 1900 (Excel) :
     * Jours 1..59  → 01/01/1900 .. 28/02/1900 (réel)
     * Jour 60      → 29/02/1900 (fictif, géré ci-dessus)
     * Jours 61+    → décale d'un jour pour aligner sur le calendrier réel */
    if (whole > 60) whole -= 1;  /* skip day 60 (fictif) */
    int64_t jdn = date_to_jdn(1900, 1, 1) + (whole - 1);
    jdn_to_date(jdn, y, m, d);
}

/* Convertit (y, m, d) en "Excel days". Inverse de excel_days_to_date. */
static int64_t date_to_excel_days(int y, int m, int d, bool mode1904) {
    if (mode1904) {
        return date_to_jdn(y, m, d) - date_to_jdn(1904, 1, 1);
    }
    int64_t days = date_to_jdn(y, m, d) - date_to_jdn(1900, 1, 1) + 1;
    /* Ajout du jour fictif 60 si la date est >= 01/03/1900 */
    if (y > 1900 || (y == 1900 && m >= 3)) days += 1;
    return days;
}

/* RéelVersDateHeure(<réel> [, <depuis1904>]) → DateHeure */
static Value fn_reel_vers_dateheure(int argCount, Value* args) {
    if (argCount < 1) return OBJ_VAL(obj_copy_string("", 0));
    double r = 0.0;
    if (IS_REAL(args[0])) r = AS_REAL(args[0]);
    else if (IS_INTEGER(args[0])) r = (double)AS_INTEGER(args[0]);
    else if (IS_NUMERIC(args[0])) {
        r = numeric_to_double(args[0]);
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "ReelVersDateHeure",
                              "native", "ReelVersDateHeure",
                              "arg", "valeur",
                              "actual", "autre type",
                              "expected", "Reel, Entier ou Numerique",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    bool mode1904 = false;
    if (argCount >= 2) value_to_bool(args[1], &mode1904);

    int y, m, d;
    excel_days_to_date(r, mode1904, &y, &m, &d);

    /* Fraction de jour → heures/min/sec/ms */
    double frac = r - (double)(int64_t)r;
    if (frac < 0) frac += 1.0;  /* sécu réels négatifs */
    int64_t totalMs = (int64_t)(frac * 86400000.0 + 0.5);
    int hh = (int)(totalMs / 3600000); totalMs %= 3600000;
    int mi = (int)(totalMs / 60000);   totalMs %= 60000;
    int ss = (int)(totalMs / 1000);
    int ms = (int)(totalMs % 1000);

    return OBJ_VAL(obj_new_datetime(y, m, d, hh, mi, ss, ms));
}

/* DateHeureVersRéel(<dh> [, <depuis1904>]) → réel */
static Value fn_dateheure_vers_reel(int argCount, Value* args) {
    if (argCount < 1) return REAL_VAL(-1);
    int y, m, d, hh = 0, mi = 0, ss = 0, ms = 0;
    if (IS_DATETIME(args[0])) {
        ObjDateTime* dt = AS_DATETIME(args[0]);
        y = dt->year; m = dt->month; d = dt->day;
        hh = dt->hour; mi = dt->minute; ss = dt->second; ms = dt->ms;
    } else if (IS_DATE(args[0])) {
        ObjDate* dd = AS_DATE(args[0]);
        y = dd->year; m = dd->month; d = dd->day;
    } else if (IS_STRING(args[0])) {
        ObjString* s = AS_STRING(args[0]);
        /* Sentinel d'erreur canon : -1.0 si parse / validation echoue.
         * Format compact accepte : 8, 12, 14, 17 (FFF millisecondes). */
        if (s->length != 8 && s->length != 12 && s->length != 14
            && s->length != 17) return REAL_VAL(-1);
        for (int i = 0; i < s->length; i++) {
            if (s->chars[i] < '0' || s->chars[i] > '9') return REAL_VAL(-1);
        }
        if (!parse_date_string(s->chars, 8, &y, &m, &d)) return REAL_VAL(-1);
        if (s->length >= 12) {
            hh = (s->chars[8]-'0')*10 + (s->chars[9]-'0');
            mi = (s->chars[10]-'0')*10 + (s->chars[11]-'0');
        }
        if (s->length >= 14) {
            ss = (s->chars[12]-'0')*10 + (s->chars[13]-'0');
        }
        if (s->length >= 17) {
            ms = ((s->chars[14]-'0')*100) + ((s->chars[15]-'0')*10) + (s->chars[16]-'0');
        }
        if (!dh_components_valid(y, m, d, hh, mi, ss)) return REAL_VAL(-1);
    } else {
        return REAL_VAL(-1);
    }
    /* Garde finale : un DateHeure object peut avoir ete construit avec des
     * composants hors borne (litteral "20260605256100" coerce sans validation
     * stricte). Retourner -1 plutot qu'une valeur reelle aberrante. */
    if (!dh_components_valid(y, m, d, hh, mi, ss)) return REAL_VAL(-1);
    bool mode1904 = false;
    if (argCount >= 2) value_to_bool(args[1], &mode1904);
    int64_t days = date_to_excel_days(y, m, d, mode1904);
    double frac = ((double)hh * 3600000.0 + (double)mi * 60000.0
                + (double)ss * 1000.0 + (double)ms) / 86400000.0;
    return REAL_VAL((double)days + frac);
}

/* DuréeVersChaîne(dur [, format]) / DurationToString — doc 3027007.
 * format peut être:
 *   absent         → HH:MM:SS (ou "J j HH:MM:SS" si days>0), style lisible OWL
 *   duréeCentième  → "JHHMMSSCC"  (ou "-JHHMMSSCC" si négatif), 9-10 chars
 *   duréeMillième  → "JHHMMSSCCC" (ou "-JHHMMSSCCC" si négatif), 10-11 chars
 *   chaîne custom  → substitutions J, HH, MM, SS, CCC, + (signe)
 *                    (tokens matchés longest-first; J seul = 1 chiffre,
 *                    plusieurs J consécutifs = zero-padding sur N chiffres) */
static Value fn_duree_vers_chaine(int argCount, Value* args) {
    if (!IS_DURATION(args[0])) return NIL_VAL;
    ObjDuration* dur = AS_DURATION(args[0]);
    int64_t ms = dur->totalMs;
    int neg = ms < 0;
    int64_t ams = neg ? -ms : ms;
    int64_t secs = ams / 1000;
    int msec = (int)(ams % 1000);
    int64_t days = secs / 86400;
    int hh = (int)((secs % 86400) / 3600);
    int mm = (int)((secs % 3600) / 60);
    int ss = (int)(secs % 60);
    int cc = msec / 10;
    char buf[128];

    if (argCount >= 2 && IS_INTEGER(args[1])) {
        int mask = (int)AS_INTEGER(args[1]);
        int len;
        if (mask == OWL_DUREE_CENTIEME) {
            len = snprintf(buf, sizeof(buf), "%s%lld%02d%02d%02d%02d",
                           neg?"-":"", (long long)days, hh, mm, ss, cc);
            return OBJ_VAL(obj_copy_string(buf, len));
        }
        if (mask == OWL_DUREE_MILLIEME) {
            len = snprintf(buf, sizeof(buf), "%s%lld%02d%02d%02d%03d",
                           neg?"-":"", (long long)days, hh, mm, ss, msec);
            return OBJ_VAL(obj_copy_string(buf, len));
        }
        /* Unknown mask: fall through to default */
    } else if (argCount >= 2 && IS_STRING(args[1])) {
        ObjString* fmt = AS_STRING(args[1]);
        int olen = 0;
        char out[256];
        int i = 0;
        while (i < fmt->length && olen < 240) {
            char c = fmt->chars[i];
            /* Count consecutive identical tokens for zero-pad width */
            int run = 1;
            while (i + run < fmt->length && fmt->chars[i + run] == c) run++;
            if (c == 'J') {
                olen += snprintf(out+olen, 256-olen, "%0*lld", run, (long long)days);
                i += run;
            } else if (c == 'H' && run >= 2) {
                olen += snprintf(out+olen, 256-olen, "%02d", hh); i += 2;
            } else if (c == 'M' && run >= 2) {
                olen += snprintf(out+olen, 256-olen, "%02d", mm); i += 2;
            } else if (c == 'S' && run >= 2) {
                olen += snprintf(out+olen, 256-olen, "%02d", ss); i += 2;
            } else if (c == 'C' && run >= 3) {
                olen += snprintf(out+olen, 256-olen, "%03d", msec); i += 3;
            } else if (c == 'C' && run >= 2) {
                olen += snprintf(out+olen, 256-olen, "%02d", cc); i += 2;
            } else if (c == '+') {
                if (neg) out[olen++] = '-';
                i += 1;
            } else {
                out[olen++] = c; i += 1;
            }
        }
        return OBJ_VAL(obj_copy_string(out, olen));
    }

    /* Default: HH:MM:SS (ou "N j HH:MM:SS") — lisible, style OWL */
    int len;
    if (days > 0)
        len = snprintf(buf, sizeof(buf), "%s%lld j %02d:%02d:%02d",
                       neg?"-":"", (long long)days, hh, mm, ss);
    else
        len = snprintf(buf, sizeof(buf), "%s%02d:%02d:%02d", neg?"-":"", hh, mm, ss);
    return OBJ_VAL(obj_copy_string(buf, len));
}

/* ================================================================
 * CHRONO / STOPWATCH
 * ================================================================ */

#ifdef _WIN32
#include <windows.h>
static int64_t chrono_freq = 0;

static int64_t chrono_now_ms(void) {
    LARGE_INTEGER freq, counter;
    if (chrono_freq == 0) { QueryPerformanceFrequency(&freq); chrono_freq = freq.QuadPart; }
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000) / chrono_freq;
}
#else
static int64_t chrono_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* Numbered chronometers (doc 3027031 SYNTAXE1).
 * ChronoDébut() sans arg == ChronoDébut(1). Max 32 chronos. */
#define CHRONO_MAX_SLOTS 32
typedef struct {
    int64_t start_ms;
    int64_t accumulated_ms;
    bool running;
    bool paused;
} ChronoSlot;
static ChronoSlot chrono_slots[CHRONO_MAX_SLOTS];

static ChronoSlot* chrono_slot_get(int id) {
    if (id < 1 || id > CHRONO_MAX_SLOTS) return NULL;
    return &chrono_slots[id - 1];
}

/* Get/set helpers for Chrono instance fields.
 *  lot 6 : root inst pendant obj_copy_string (alloc -> GC sous
 * stress collecterait inst purement C-stack-local). Setters delegues aux
 * helpers canon inst_field_set_* (object.h) qui rootent deja inst+key. */
static int64_t chrono_field_int(ObjInstance* inst, const char* name) {
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_pop();
    Value v;
    if (!table_get(&inst->fields, k, &v)) return 0;
    if (IS_INTEGER(v)) return AS_INTEGER(v);
    return 0;
}
static bool chrono_field_bool(ObjInstance* inst, const char* name) {
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_pop();
    Value v;
    if (!table_get(&inst->fields, k, &v)) return false;
    if (IS_BOOL(v)) return AS_BOOL(v);
    return false;
}
static void chrono_set_int(ObjInstance* inst, const char* name, int64_t val) {
    inst_field_set_int(inst, name, val);
}
static void chrono_set_bool(ObjInstance* inst, const char* name, bool val) {
    inst_field_set_bool(inst, name, val);
}

/*  +  : les fields publics `Demarre` / `Valeur` sont
 * resolus via OwlField.computeGetter (cf FIELDS_Chrono), qui delegue a
 * fn_chrono_demarre / fn_chrono_valeur a chaque lecture. Le storage
 * d'instance n'est plus consulte ni par OP_STRUCT_GET, ni par
 * value_to_json, ni par fill_var. La fonction chrono_sync_public
 * d'avant  est donc retiree (helper write-only). */

/* Returns the Chrono ObjInstance argument if present and valid, NULL
 * otherwise (global chrono). */
static ObjInstance* chrono_target(int argCount, Value* args) {
    if (argCount >= 1 && IS_INSTANCE(args[0])) {
        ObjInstance* inst = AS_INSTANCE(args[0]);
        if (inst->structName != NULL
            && strcmp(inst->structName->chars, "Chrono") == 0) {
            return inst;
        }
    }
    return NULL;
}

/* Résout le slot numéroté à utiliser. Défaut 1 si pas d'arg entier. */
static ChronoSlot* chrono_numbered_slot(int argCount, Value* args) {
    int id = 1; /* default per doc */
    if (argCount >= 1 && IS_INTEGER(args[0])) id = (int)AS_INTEGER(args[0]);
    return chrono_slot_get(id);
}

static Value fn_chrono_debut(int argCount, Value* args) {
    /* Forme 2: variable Chrono */
    ObjInstance* inst = chrono_target(argCount, args);
    if (inst != NULL) {
        chrono_set_int (inst, "_start_ms",       chrono_now_ms());
        chrono_set_int (inst, "_accumulated_ms", 0);
        chrono_set_bool(inst, "_running",        true);
        chrono_set_bool(inst, "_paused",         false);
        return NIL_VAL;
    }
    /* Forme 1: numéro de chrono (défaut 1) */
    ChronoSlot* slot = chrono_numbered_slot(argCount, args);
    if (slot == NULL) return NIL_VAL;
    slot->start_ms = chrono_now_ms();
    slot->accumulated_ms = 0;
    slot->running = true;
    slot->paused = false;
    return NIL_VAL;
}

static Value fn_chrono_fin(int argCount, Value* args) {
    /* Forme 2: variable Chrono */
    ObjInstance* inst = chrono_target(argCount, args);
    if (inst != NULL) {
        if (!chrono_field_bool(inst, "_running")) return INTEGER_VAL(0);
        int64_t acc   = chrono_field_int(inst, "_accumulated_ms");
        int64_t start = chrono_field_int(inst, "_start_ms");
        bool paused   = chrono_field_bool(inst, "_paused");
        int64_t elapsed = acc + (paused ? 0 : chrono_now_ms() - start);
        chrono_set_bool(inst, "_running", false);
        chrono_set_int (inst, "_accumulated_ms", elapsed);
        return INTEGER_VAL(elapsed);
    }
    /* Forme 1: numéro de chrono */
    ChronoSlot* slot = chrono_numbered_slot(argCount, args);
    if (slot == NULL || !slot->running) return INTEGER_VAL(0);
    int64_t elapsed = slot->accumulated_ms;
    if (!slot->paused) elapsed += chrono_now_ms() - slot->start_ms;
    slot->running = false;
    return INTEGER_VAL(elapsed);
}

static Value fn_chrono_pause(int argCount, Value* args) {
    /* Doc 1000018829: retourne la Durée écoulée depuis ChronoDébut. */
    ObjInstance* inst = chrono_target(argCount, args);
    if (inst != NULL) {
        int64_t elapsed = 0;
        if (chrono_field_bool(inst, "_running")
            && !chrono_field_bool(inst, "_paused")) {
            int64_t acc   = chrono_field_int(inst, "_accumulated_ms");
            int64_t start = chrono_field_int(inst, "_start_ms");
            elapsed = acc + chrono_now_ms() - start;
            chrono_set_int (inst, "_accumulated_ms", elapsed);
            chrono_set_bool(inst, "_paused", true);
        } else {
            elapsed = chrono_field_int(inst, "_accumulated_ms");
        }
        return INTEGER_VAL(elapsed);
    }
    ChronoSlot* slot = chrono_numbered_slot(argCount, args);
    if (slot == NULL) return INTEGER_VAL(0);
    if (slot->running && !slot->paused) {
        slot->accumulated_ms += chrono_now_ms() - slot->start_ms;
        slot->paused = true;
    }
    return INTEGER_VAL(slot->accumulated_ms);
}

static Value fn_chrono_reprend(int argCount, Value* args) {
    ObjInstance* inst = chrono_target(argCount, args);
    if (inst != NULL) {
        if (chrono_field_bool(inst, "_running")
            && chrono_field_bool(inst, "_paused")) {
            chrono_set_int (inst, "_start_ms", chrono_now_ms());
            chrono_set_bool(inst, "_paused",   false);
        }
        return NIL_VAL;
    }
    ChronoSlot* slot = chrono_numbered_slot(argCount, args);
    if (slot != NULL && slot->running && slot->paused) {
        slot->start_ms = chrono_now_ms();
        slot->paused = false;
    }
    return NIL_VAL;
}

static Value fn_chrono_raz(int argCount, Value* args) {
    ObjInstance* inst = chrono_target(argCount, args);
    if (inst != NULL) {
        chrono_set_bool(inst, "_running",        false);
        chrono_set_bool(inst, "_paused",         false);
        chrono_set_int (inst, "_accumulated_ms", 0);
        return NIL_VAL;
    }
    ChronoSlot* slot = chrono_numbered_slot(argCount, args);
    if (slot != NULL) {
        slot->running = false;
        slot->paused = false;
        slot->accumulated_ms = 0;
    }
    return NIL_VAL;
}

Value fn_chrono_valeur(int argCount, Value* args) {
    ObjInstance* inst = chrono_target(argCount, args);
    if (inst != NULL) {
        if (!chrono_field_bool(inst, "_running")) return INTEGER_VAL(0);
        int64_t acc   = chrono_field_int(inst, "_accumulated_ms");
        int64_t start = chrono_field_int(inst, "_start_ms");
        bool paused   = chrono_field_bool(inst, "_paused");
        int64_t elapsed = acc + (paused ? 0 : chrono_now_ms() - start);
        return INTEGER_VAL(elapsed);
    }
    ChronoSlot* slot = chrono_numbered_slot(argCount, args);
    if (slot == NULL || !slot->running) return INTEGER_VAL(0);
    int64_t elapsed = slot->accumulated_ms;
    if (!slot->paused) elapsed += chrono_now_ms() - slot->start_ms;
    return INTEGER_VAL(elapsed);
}

/* Getter calcule pour le champ Chrono..Demarre. Refleter l'etat live du
 * runtime (_running). Branche via OwlField.computeGetter dans
 * FIELDS_Chrono. Cf  (introduction du getter live) + 
 * (retrait du snapshot storage redondant). */
Value fn_chrono_demarre(int argCount, Value* args) {
    ObjInstance* inst = chrono_target(argCount, args);
    if (inst != NULL) {
        return BOOL_VAL(chrono_field_bool(inst, "_running"));
    }
    return BOOL_VAL(false);
}

/* ================================================================
 * EXPOSED PARSERS (compile-time literal filter, single-parser invariant)
 * Cf. stdlib_date_parsers.h, .
 * ================================================================ */

bool owl_parse_date_string(const char* s, int len, int* y, int* m, int* d) {
    return parse_date_string(s, len, y, m, d);
}

bool owl_parse_time_string_cs(const char* s, int len,
                              int* h, int* m, int* sec, int* cs) {
    return parse_time_string_cs(s, len, h, m, sec, cs);
}

bool owl_parse_dateheure_iso(const char* s, int len,
                             int* y, int* m, int* d,
                             int* hh, int* mm, int* ss) {
    return parse_dateheure_iso(s, len, y, m, d, hh, mm, ss);
}

/* ================================================================
 * REGISTRATION
 * ================================================================ */

void stdlib_date_init(void) {
    /* Override the old string-returning DateSys/HeureSys/DateHeureSys */
    /* Internal coercion functions (called by compiler for typed declarations) */
    owl_define_native("__DateCoerce", fn_date_coerce, 1);
    owl_define_native("__HeureCoerce", fn_heure_coerce, 1);
    owl_define_native("__DateHeureCoerce", fn_dateheure_coerce, 1);
    owl_define_native("__DureeCoerce", fn_duree_coerce, 1);
    /*  — variantes nullable : echec parse chaine -> NIL au lieu
     * du sentinel. Cf. fn_date_coerce_nullable etc. */
    owl_define_native("__DateCoerceNullable", fn_date_coerce_nullable, 1);
    owl_define_native("__HeureCoerceNullable", fn_heure_coerce_nullable, 1);
    owl_define_native("__DateHeureCoerceNullable", fn_dateheure_coerce_nullable, 1);
    owl_define_native("__DureeCoerceNullable", fn_duree_coerce_nullable, 1);

    /* ================================================================
     * Registry declarations — Date/Time/Duration/Chrono family.
     * Canonical forms follow the official accents (Année, Pâques,
     * JourFérié, ChronoDébut, NuméroDeSemaine, Durée, Différence…).
     * canonicalAscii gives an ASCII alias for runtime compat.
     * ================================================================ */
    {
        static const OwlParam p_d[]   = { {"date",    "date",      "date source"}, {0} };
        static const OwlParam p_h[]   = { {"heure",   "heure",     "heure source"}, {0} };
        static const OwlParam p_dh[]  = { {"dh",      "dateheure", "dateheure source"}, {0} };
        static const OwlParam p_s[]   = { {"s",       "chaîne",    "chaîne source"}, {0} };
        static const OwlParam p_i[]   = { {"n",       "entier",    "valeur entière"}, {0} };
        static const OwlParam p_dd[]  = { {"d1",      "date",      "première date"}, {"d2", "date", "deuxième date"}, {0} };
        static const OwlParam p_hh[]  = { {"h1",      "heure",     "première heure"}, {"h2", "heure", "deuxième heure"}, {0} };
        static const OwlParam p_dhdh[]= { {"dh1",     "dateheure", "première date+heure"}, {"dh2", "dateheure", "deuxième date+heure"}, {0} };
        static const OwlParam p_week[]= { {"année",   "entier", "année"}, {"semaine", "entier", "numéro de semaine"}, {0} };
        static const OwlParam p_chrono[] = { {"chrono",  "Chrono", "chronomètre (optionnel, global par défaut)"}, {0} };

        static const OwlSignature sig_no_arg[]   = { {0,  NULL, "entier", "valeur entière système courante"}, {0} };
        static const OwlParam p_sys_set[]        = { {"valeur", "chaîne?", "AAAAMMJJ[HHMMSS] (setter — no-op en OWL)"}, {0} };
        /*  — returnType précis par famille (plus l'union ambiguë
         * date|heure|dateheure) : la sémantique runtime renvoie un objet typé,
         * l'aide doit l'annoncer. Getter 0-arg + setter 1-arg no-op. */
        static const OwlSignature sig_date_sys[] = { {0, NULL, "date", "date système courante"},
                                                      {1, p_sys_set, "date", "setter (no-op en OWL, conformité API)"}, {0} };
        static const OwlSignature sig_heure_sys[]= { {0, NULL, "heure", "heure système courante"},
                                                      {1, p_sys_set, "heure", "setter (no-op en OWL, conformité API)"}, {0} };
        static const OwlSignature sig_dh_sys[]   = { {0, NULL, "dateheure", "date+heure système courante"},
                                                      {1, p_sys_set, "dateheure", "setter (no-op en OWL, conformité API)"}, {0} };
        static const OwlSignature sig_d[]        = { {1,  p_d,  "entier",  "valeur extraite"}, {0} };
        static const OwlParam p_d_week1[]        = { {"date", "date", "date source"}, {0} };
        static const OwlParam p_d_week[]         = { {"date", "date", "date source"}, {"format", "entier", "iso8601 (1) ou défaut 0"}, {0} };
        static const OwlSignature sig_d_week[]   = { {1, p_d_week1, "entier", "numéro de semaine (0..53)"},
                                                     {2, p_d_week,  "entier", "numéro de semaine (0..53)"}, {0} };
        static const OwlParam p_i_week1[]        = { {"n", "entier", "jours depuis 01/01/1800 (base 1)"}, {0} };
        static const OwlParam p_i_week[]         = { {"n", "entier", "jours depuis 01/01/1800 (base 1)"}, {"format", "entier", "iso8601 (1) ou défaut 0"}, {0} };
        static const OwlSignature sig_i_week[]   = { {1, p_i_week1, "entier", "numéro de semaine (0..53)"},
                                                     {2, p_i_week,  "entier", "numéro de semaine (0..53)"}, {0} };
        static const OwlSignature sig_d_bool[]   = { {1,  p_d,  "booléen", "Vrai si valide"}, {0} };
        static const OwlParam p_bissex[]         = { {"année_ou_date", "entier|chaîne|date|dateheure", "année à tester (défaut: année courante)"}, {0} };
        static const OwlSignature sig_bissex[]   = { {0, NULL,    "booléen", "Vrai si l'année courante est bissextile"},
                                                     {1, p_bissex, "booléen", "Vrai si bissextile"}, {0} };
        static const OwlParam p_d_s2[]           = { {"date", "date|dateheure|chaîne", "valeur source"}, {"format", "entier|chaîne", "masque ou template de formatage"}, {0} };
        static const OwlSignature sig_d_s[]      = { {1, p_d,    "chaîne", "représentation"},
                                                     {2, p_d_s2, "chaîne", "représentation selon format"}, {0} };
        static const OwlParam p_s_d2[]           = { {"s", "chaîne", "chaîne source"}, {"format", "entier|chaîne", "masque ou template de parsing"}, {0} };
        static const OwlSignature sig_s_d[]      = { {1, p_s,    "date", "date parsée"},
                                                     {2, p_s_d2, "date", "date parsée selon format"}, {0} };
        static const OwlSignature sig_s_h[]      = { {1,  p_s,  "heure",   "heure parsée"}, {0} };
        static const OwlParam p_h_s2[]           = { {"heure", "heure|dateheure|chaîne", "valeur source"}, {"format", "chaîne", "template de formatage (défaut HH:MM:SS:CC)"}, {0} };
        static const OwlSignature sig_h_s[]      = { {1, p_h,    "chaîne", "représentation"},
                                                     {2, p_h_s2, "chaîne", "représentation selon format"}, {0} };
        static const OwlSignature sig_h_bool[]   = { {1,  p_h,  "booléen", "Vrai si valide"}, {0} };
        static const OwlSignature sig_h_i[]      = { {1,  p_h,  "entier",  "valeur numérique"}, {0} };
        static const OwlSignature sig_i_h[]      = { {1,  p_i,  "heure",   "heure construite"}, {0} };
        static const OwlParam p_i_part[]         = { {"n", "entier", "jours depuis 01/01/1800"},
                                                      {"partie", "chaîne", "\"A\"=année, \"M\"=mois, \"J\"=jour"}, {0} };
        static const OwlSignature sig_i_d_part[] = { {1, p_i, "date", "date construite"},
                                                      {2, p_i_part, "chaîne", "partie extraite (4 ou 2 chars)"}, {0} };
        static const OwlParam p_d_fromyear[] = { {"date", "date|chaîne|dateheure", "date à convertir"},
                                                  {"depuisDebutAnnee", "booléen?", "Vrai = depuis 01/01 de l'année"}, {0} };
        static const OwlSignature sig_d_i[]      = { {1,  p_d,  "entier",  "valeur numérique"},
                                                     {2,  p_d_fromyear, "entier", "jours depuis 01/01 année ou 01/01/1800"}, {0} };
        static const OwlSignature sig_dh_i[]     = { {1,  p_dh, "entier",  "timestamp"}, {0} };
        static const OwlSignature sig_dh_dh[]    = { {1,  p_dh, "dateheure", "dateheure convertie"}, {0} };
        static const OwlSignature sig_i_dh[]     = { {1,  p_i,  "dateheure", "dateheure construite"}, {0} };
        static const OwlSignature sig_dh_bool[]  = { {1,  p_dh, "booléen", "Vrai si valide"}, {0} };
        static const OwlParam p_dh_s2[]          = { {"dh", "dateheure|date|chaîne", "valeur source"}, {"format", "entier|chaîne", "masque ou template de formatage"}, {0} };
        static const OwlSignature sig_dh_s[]     = { {1, p_dh,    "chaîne", "représentation"},
                                                     {2, p_dh_s2, "chaîne", "représentation selon format"}, {0} };
        static const OwlParam p_reel_dh1[]       = { {"valeur", "réel", "Excel days (entier+fraction)"}, {0} };
        static const OwlParam p_reel_dh[]        = { {"valeur", "réel", "Excel days (entier+fraction)"},
                                                      {"depuis1904", "booléen?", "Vrai = base 1904 (défaut: 1900)"}, {0} };
        static const OwlSignature sig_reel_dh[]  = { {1, p_reel_dh1, "dateheure", "DateHeure construite"},
                                                     {2, p_reel_dh,  "dateheure", "DateHeure construite"}, {0} };
        static const OwlParam p_dh_reel1[]       = { {"dateheure", "dateheure|chaîne", "DateHeure à convertir"}, {0} };
        static const OwlParam p_dh_reel[]        = { {"dateheure", "dateheure|chaîne", "DateHeure à convertir"},
                                                      {"depuis1904", "booléen?", "Vrai = base 1904 (défaut: 1900)"}, {0} };
        static const OwlSignature sig_dh_reel[]  = { {1, p_dh_reel1, "réel", "Excel days"},
                                                     {2, p_dh_reel,  "réel", "Excel days"}, {0} };
        static const OwlSignature sig_dd_dur[]   = { {2,  p_dd, "entier",  "nombre de jours (d2-d1)"}, {0} };
        static const OwlSignature sig_hh_dur[]   = { {2,  p_hh, "durée",   "écart entre heures"}, {0} };
        static const OwlSignature sig_dhdh_dur[] = { {2,  p_dhdh, "durée", "écart entre date+heures"}, {0} };
        static const OwlSignature sig_i_s[]      = { {1,  p_i,  "chaîne",  "nom"}, {0} };
        static const OwlSignature sig_d_day[]    = {
            {0,  NULL, "date", "année courante"},
            {1,  p_d,  "date", "année de la date donnée"},
            {0}
        };
        static const OwlParam p_ym[]             = { {"année", "entier", "année (4 chiffres)"},
                                                      {"mois", "entier", "mois 1-12"}, {0} };
        static const OwlSignature sig_jourdumois[] = { {0, NULL, "date", "mois courant"},
                                                        {1, p_d, "date", "depuis date"},
                                                        {2, p_ym, "date", "année + mois"}, {0} };
        static const OwlSignature sig_week_d[]   = { {2,  p_week, "date",  "lundi de la semaine"}, {0} };
        static const OwlSignature sig_d_str[]    = { {1,  p_d,  "chaîne",  "nom en lettres"}, {0} };
        static const OwlSignature sig_demain[]   = { {0, NULL, "date", "date décalée par rapport à aujourd'hui"},
                                                     {1, p_d,  "date", "date décalée par rapport à la date donnée"}, {0} };
        static const OwlParam p_day_year1[]      = { {"jour", "entier", "numéro de jour dans l'année (1..366)"}, {0} };
        static const OwlParam p_day_year2[]      = { {"jour", "entier", "numéro de jour dans l'année (1..366)"},
                                                     {"année", "entier", "année cible (défaut: année courante)"}, {0} };
        static const OwlSignature sig_day_year[] = { {1, p_day_year1, "date", "date calculée (année courante)"},
                                                     {2, p_day_year2, "date", "date calculée"}, {0} };
        static const OwlParam p_paques[]         = { {"année", "entier|chaîne|date", "année (1583-9999)"}, {0} };
        static const OwlSignature sig_paques[]   = { {1, p_paques, "date", "dimanche de Pâques (Computus grégorien)"}, {0} };

        /* JourFériéAjoute — multi-signature (4 formes). */
        static const OwlParam p_jfa_s[]    = { {"jour",  "chaîne",    "\"MMJJ\" (récurrent annuel), \"AAAAMMJJ\" (année spécifique), ou combinaison LF-jointe pour plusieurs entrées"}, {0} };
        static const OwlParam p_jfa_d[]    = { {"jour",  "date",      "date typée — composantes année/mois/jour utilisées"}, {0} };
        static const OwlParam p_jfa_dh[]   = { {"jour",  "dateheure", "dateheure typée — composante date utilisée, heure ignorée"}, {0} };
        static const OwlParam p_jfa_tab[]  = { {"jours", "tableau",   "tableau de chaînes, dates ou dateheures (mêmes formats que les signatures unitaires)"}, {0} };
        static const OwlSignature sig_jfa[] = {
            {1, p_jfa_s,   "booléen", "Vrai si ajouté, Faux si format invalide ou capacité dépassée"},
            {1, p_jfa_d,   "booléen", "Vrai si ajouté, Faux si capacité dépassée"},
            {1, p_jfa_dh,  "booléen", "Vrai si ajouté, Faux si capacité dépassée"},
            {1, p_jfa_tab, "booléen", "Vrai si tous ajoutés, Faux dès qu'un élément échoue"},
            {0}
        };
        static const OwlSignature sig_jf_liste[]     = { {0, NULL, "chaîne", "liste LF-jointe des entrées (\"MMJJ\" ou \"AAAAMMJJ\"), vide si aucun jour férié"}, {0} };
        static const OwlSignature sig_jf_suppr_tt[]  = { {0, NULL, "rien",   "vide le calendrier des jours fériés"}, {0} };

        static const OwlParam p_jour_lettre[]    = { {"jour", "entier|chaîne|date", "jours depuis 01/01/1800 (base 1) ou date"}, {0} };
        static const OwlSignature sig_jour_lettre[] = { {1, p_jour_lettre, "chaîne", "Lundi, Mardi, Mercredi, Jeudi, Vendredi, Samedi, Dimanche"}, {0} };
        static const OwlParam p_mois_lettre[]    = { {"mois", "entier|chaîne|date", "jours depuis 01/01/1800 (base 1) ou date"}, {0} };
        static const OwlSignature sig_mois_lettre[] = { {1, p_mois_lettre, "chaîne", "Janvier, Février, Mars, Avril, Mai, Juin, Juillet, Août, Septembre, Octobre, Novembre, Décembre"}, {0} };
        static const OwlParam p_num_mois[]       = { {"mois", "entier|chaîne", "numéro du mois (1..12)"}, {0} };
        static const OwlSignature sig_num_mois[] = { {1, p_num_mois, "chaîne", "Janvier, Février, Mars, Avril, Mai, Juin, Juillet, Août, Septembre, Octobre, Novembre, Décembre"}, {0} };
        static const OwlParam p_jour_lettre_date[]    = { {"date", "chaîne|date|dateheure", "chaîne AAAAMMJJ ou AAAAMMJJHHmmSS[CC], variable date ou dateheure"}, {0} };
        static const OwlSignature sig_jour_lettre_date[] = { {1, p_jour_lettre_date, "chaîne", "Lundi, Mardi, Mercredi, Jeudi, Vendredi, Samedi, Dimanche"}, {0} };
        static const OwlParam p_age[]            = { {"naissance", "date|chaîne|dateheure", "date de naissance"},
                                                     {"calcul",    "date|chaîne|dateheure", "date de calcul (défaut: aujourd'hui)"},
                                                     {0} };
        static const OwlParam p_age1[]           = { {"naissance", "date|chaîne|dateheure", "date de naissance"}, {0} };
        static const OwlSignature sig_age[]      = { {1, p_age1, "chaîne", "âge à aujourd'hui — chaîne AAAAMMJJ encodant la durée. Chaîne vide si la date n'est pas valide."},
                                                     {2, p_age,  "chaîne", "chaîne AAAAMMJJ encodant la durée (ex: \"00450203\" = 45 ans, 2 mois, 3 jours). Chaîne vide si la date n'est pas valide."}, {0} };
        static const OwlSignature sig_chrono_v[] = { {0, NULL,     "rien", "opération sur le chronomètre global"},
                                                     {1, p_chrono, "rien", "opération sur le chronomètre désigné"}, {0} };
        static const OwlSignature sig_chrono_ms[]= { {0, NULL,     "entier", "millisecondes écoulées (chronomètre global)"},
                                                     {1, p_chrono, "entier", "millisecondes écoulées"}, {0} };
        static const OwlParam p_s_fmt_dur[] = { {"s",      "chaîne",        "chaîne source"},
                                                 {"format", "chaîne|entier", "format (duréeCentième/Millième ou template)"}, {0} };
        static const OwlSignature sig_s_fmt_dur[]= { {2, p_s_fmt_dur, "durée", "durée parsée selon format"}, {0} };
        static const OwlParam p_dur_fmt[] = { {"durée", "durée", "durée à formater"},
                                               {"format", "chaîne|entier", "format (duréeCentième/Millième ou template)"}, {0} };
        static const OwlSignature sig_dur_s[]    = { {1, p_d, "chaîne", "durée formatée"},
                                                     {2, p_dur_fmt, "chaîne", "durée formatée selon format"}, {0} };

#define D_DEF(fr, fr_ascii, en, cat, df, de, sig, fn) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), .category = (cat), \
      .descFR = (df), .descEN = (de), .signatures = (sig), .impl = (fn) }

        static const OwlNativeDef DEFS[] = {
            D_DEF("DateSys", NULL, "DateSys", "Dates",
                "Date système courante (setter no-op en OWL).",
                "Current system date (setter is a no-op in OWL).",
                sig_date_sys, fn_date_sys),
            D_DEF("DateDuJour", NULL, "Today", "Dates",
                "Alias de DateSys.", "Alias of DateSys.", sig_date_sys, fn_date_sys),
            D_DEF("HeureSys", NULL, "TimeSys", "Dates",
                "Heure système courante (setter no-op en OWL).",
                "Current system time (setter is a no-op in OWL).",
                sig_heure_sys, fn_heure_sys),
            D_DEF("Maintenant", NULL, "Now", "Dates",
                "Alias de HeureSys.", "Alias of HeureSys.", sig_heure_sys, fn_heure_sys),
            D_DEF("DateHeureSys", NULL, "SysDateTime", "Dates",
                "Date+heure système (setter no-op en OWL).",
                "Current system date+time (setter is a no-op in OWL).",
                sig_dh_sys, fn_dateheure_sys),

            D_DEF("Cha\xc3\xae" "neVersDate", "ChaineVersDate", "StringToDate", "Dates",
                "Parse une chaîne en date.", "Parse a string to date.", sig_s_d, fn_chaine_vers_date),
            D_DEF("DateVersCha\xc3\xae" "ne", "DateVersChaine", "DateToString", "Dates",
                "Formate une date en chaîne.", "Format a date to string.", sig_d_s, fn_date_vers_chaine),
            D_DEF("Cha\xc3\xae" "neVersHeure", "ChaineVersHeure", "StringToTime", "Dates",
                "Parse une chaîne en heure.", "Parse a string to time.", sig_s_h, fn_chaine_vers_heure),
            D_DEF("HeureVersCha\xc3\xae" "ne", "HeureVersChaine", "TimeToString", "Dates",
                "Formate une heure en chaîne.", "Format a time to string.", sig_h_s, fn_heure_vers_chaine),
            D_DEF("DateVersEntier", NULL, "DateToInteger", "Dates",
                "Date en entier : nombre de jours depuis le 01/01/1800.",
                "Date as integer: days since 01/01/1800.", sig_d_i, fn_date_vers_entier),
            D_DEF("EntierVersDate", NULL, "IntegerToDate", "Dates",
                "Entier (jours depuis 01/01/1800) en date, ou partie extraite (\"A\"/\"M\"/\"J\").",
                "Integer (days since 01/01/1800) to date, or extracted part (\"A\"/\"M\"/\"J\").",
                sig_i_d_part, fn_entier_vers_date),

            D_DEF("Jour", NULL, "Day", "Dates",
                "Jour d'une date (1..31).", "Day of a date (1..31).", sig_d, fn_jour),
            D_DEF("Mois", NULL, "Month", "Dates",
                "Mois d'une date (1..12).", "Month of a date (1..12).", sig_d, fn_mois),
            D_DEF("Ann\xc3\xa9" "e", "Annee", "Year", "Dates",
                "Année d'une date.", "Year of a date.", sig_d, fn_annee),
            D_DEF("DateVersJourDeLaSemaine", NULL, "DateToDayOfWeek", "Dates",
                "Jour de la semaine (1=lundi, 7=dimanche). Identique à DateVersJour.", "Day of week (1=Monday, 7=Sunday). Identical to DateToDay.",
                sig_d, fn_jour_semaine),
            D_DEF("DateVersJour", NULL, "DateToDay", "Dates",
                "Jour de la semaine (1=lundi, 7=dimanche). Alias de DateVersJourDeLaSemaine.", "Day of week (1=Monday, 7=Sunday). Alias of DateToDayOfWeek.", sig_d, fn_jour_semaine),
            D_DEF("JourDeLaSemaine", NULL, "DayOfWeek", "Dates",
                "Extension OWL : alias court de DateVersJourDeLaSemaine (1=lundi, 7=dimanche).", "OWL extension: short alias of DateToDayOfWeek (1=Monday, 7=Sunday).",
                sig_d, fn_jour_semaine),

            D_DEF("DateDiff\xc3\xa9" "rence", "DateDifference", "DateDifference", "Dates",
                "Différence entre deux dates (durée).", "Difference between two dates (duration).",
                sig_dd_dur, fn_date_difference),

            D_DEF("DateValide", NULL, "DateValid", "Dates",
                "Vrai si la date est valide.", "True if the date is valid.",
                sig_d_bool, fn_date_valide),
            D_DEF("HeureValide", NULL, "TimeValid", "Dates",
                "Vrai si l'heure est valide.", "True if the time is valid.",
                sig_h_bool, fn_heure_valide),
            D_DEF("Ann\xc3\xa9" "eBissextile", "AnneeBissextile", "LeapYear", "Dates",
                "Vrai si l'année est bissextile (sans arg: année courante).",
                "True if the year is a leap year (no arg: current year).",
                sig_bissex, fn_annee_bissextile),

            D_DEF("DateHeureVersEpoch", NULL, "DateTimeToEpoch", "Dates",
                "Convertit en timestamp Unix.", "Convert to Unix timestamp.",
                sig_dh_i, fn_dateheure_vers_epoch),
            D_DEF("EpochVersDateHeure", NULL, "EpochToDateTime", "Dates",
                "Convertit un timestamp Unix en date+heure.", "Convert a Unix timestamp to datetime.",
                sig_i_dh, fn_epoch_vers_dateheure),

            D_DEF("DateHeureLocaleVersUTC", NULL, "DateTimeLocalToUTC", "Dates",
                "Convertit une date+heure locale en temps universel (UTC), selon le fuseau et l'horaire d'ete/hiver du poste a la date fournie.",
                "Convert a local datetime to universal time (UTC), using the machine time zone and daylight-saving rules at the given date.",
                sig_dh_dh, fn_dateheure_locale_vers_utc),
            D_DEF("DateHeureUTCVersLocale", NULL, "DateTimeUTCToLocal", "Dates",
                "Convertit une date+heure exprimee en temps universel (UTC) en heure locale, selon le fuseau et l'horaire d'ete/hiver du poste a la date fournie.",
                "Convert a datetime expressed in universal time (UTC) to local time, using the machine time zone and daylight-saving rules at the given date.",
                sig_dh_dh, fn_dateheure_utc_vers_locale),

            D_DEF("PremierJourDuMois", NULL, "FirstDayOfMonth", "Dates",
                "Premier jour du mois (date, ou (année, mois), ou aucun arg = mois courant).",
                "First day of the month (date, or (year, month), or no arg = current).",
                sig_jourdumois, fn_premier_jour_du_mois),
            D_DEF("DernierJourDuMois", NULL, "LastDayOfMonth", "Dates",
                "Dernier jour du mois (date, ou (année, mois), ou aucun arg = mois courant).",
                "Last day of the month (date, or (year, month), or no arg = current).",
                sig_jourdumois, fn_dernier_jour_du_mois),
            D_DEF("PremierJourDeLaSemaine", NULL, "FirstDayOfWeek", "Dates",
                "Premier jour de la semaine (lundi).", "First day of the week (Monday).",
                sig_d_day, fn_premier_jour_semaine),
            D_DEF("DernierJourDeLaSemaine", NULL, "LastDayOfWeek", "Dates",
                "Dernier jour de la semaine (dimanche).", "Last day of the week (Sunday).",
                sig_d_day, fn_dernier_jour_semaine),
            D_DEF("PremierJourDeLAnn\xc3\xa9" "e", "PremierJourDeLAnnee", "FirstDayOfYear", "Dates",
                "Premier jour de l'année.", "First day of the year.",
                sig_d_day, fn_premier_jour_annee),
            D_DEF("DernierJourDeLAnn\xc3\xa9" "e", "DernierJourDeLAnnee", "LastDayOfYear", "Dates",
                "Dernier jour de l'année.", "Last day of the year.",
                sig_d_day, fn_dernier_jour_annee),

            D_DEF("DateVersJourEnLettre", NULL, "DateToDayInAlpha", "Dates",
                "Nom du jour de la semaine (Lundi à Dimanche, capitalisé) pour une chaîne AAAAMMJJ ou AAAAMMJJHHmmSS[CC], une variable date ou dateheure. Langue : français uniquement.",
                "Day-of-week name (Lundi to Dimanche, capitalised) for a AAAAMMJJ or AAAAMMJJHHmmSS[CC] string, a date or datetime variable. Language: French only.",
                sig_jour_lettre_date, fn_date_vers_jour_en_lettre),
            D_DEF("DateVersMoisEnLettre", NULL, "DateToMonthInAlpha", "Dates",
                "Nom du mois.", "Name of the month.",
                sig_d_str, fn_date_vers_mois_en_lettre),

            D_DEF("DateVersJourDansLAnn\xc3\xa9" "e", "DateVersJourDansLAnnee", "DateToDayOfYear", "Dates",
                "Jour dans l'année (1..366).", "Day of year (1..366).",
                sig_d, fn_jour_dans_annee),
            D_DEF("DateVersNum\xc3\xa9" "roDeSemaine", "DateVersNumeroDeSemaine", "DateToWeekNumber", "Dates",
                "Numéro de semaine (0..53). Passer iso8601 en 2e arg pour ISO 8601 strict.",
                "Week number (0..53). Pass iso8601 as 2nd arg for ISO 8601 strict.",
                sig_d_week, fn_numero_de_semaine),
            D_DEF("Num\xc3\xa9" "roDeSemaine", "NumeroDeSemaine", "WeekNumber", "Dates",
                "Numéro de semaine (0..53). Passer iso8601 en 2e arg pour ISO 8601 strict.",
                "Week number (0..53). Pass iso8601 as 2nd arg for ISO 8601 strict.",
                sig_d_week, fn_numero_de_semaine),
            D_DEF("SemaineVersDate", NULL, "WeekToDate", "Dates",
                "Date du lundi d'une semaine donnée.", "Monday date of a given week.",
                sig_week_d, fn_semaine_vers_date),
            D_DEF("Ann\xc3\xa9" "eEnCours", "AnneeEnCours", "CurrentYear", "Dates",
                "Année courante.", "Current year.", sig_no_arg, fn_annee_en_cours),
            D_DEF("MoisEnCours", NULL, "CurrentMonth", "Dates",
                "Mois courant.", "Current month.", sig_no_arg, fn_mois_en_cours),

            D_DEF("HeureVersEntier", NULL, "TimeToInteger", "Dates",
                "Heure en entier : nombre de centièmes de seconde écoulés depuis 00:00:00:00.",
                "Time as integer: number of hundredths of a second since 00:00:00:00.",
                sig_h_i, fn_heure_vers_entier),
            D_DEF("EntierVersHeure", NULL, "IntegerToTime", "Dates",
                "Centièmes de seconde depuis 00:00:00:00 en heure.",
                "Hundredths of a second since 00:00:00:00 to time.",
                sig_i_h, fn_entier_vers_heure),
            D_DEF("DateHeureValide", NULL, "DateTimeValid", "Dates",
                "Vrai si la date+heure est valide.", "True if the datetime is valid.",
                sig_dh_bool, fn_dateheure_valide),
            D_DEF("HeureDiff\xc3\xa9" "rence", "HeureDifference", "TimeDifference", "Dates",
                "Différence entre deux heures.", "Difference between two times.",
                sig_hh_dur, fn_heure_difference),

            D_DEF("Num\xc3\xa9" "roDeJourVersLettre", "NumeroDeJourVersLettre", "DayNumberInAlpha", "Dates",
                "Nom du jour de la semaine (1..7).", "Name of the day of the week (1..7).",
                sig_i_s, fn_numero_jour_vers_lettre),
            D_DEF("Num\xc3\xa9" "roDeMoisVersLettre", "NumeroDeMoisVersLettre", "MonthNumberInAlpha", "Dates",
                "Nom du mois (Janvier à Décembre, capitalisé) pour un numéro de mois (1..12) ou une chaîne numérique équivalente. Langue : français uniquement.",
                "Month name (Janvier to Décembre, capitalised) for a month number (1..12) or its numeric-string equivalent. Language: French only.",
                sig_num_mois, fn_numero_mois_vers_lettre),

            D_DEF("JourF\xc3\xa9" "ri\xc3\xa9", "JourFerie", "BankHoliday", "Dates",
                "Vrai si la date est un jour férié enregistré. Le calendrier est vide par défaut, à peupler via JourFériéAjoute.",
                "True if the date is a registered bank holiday. The calendar is empty by default, populate it via BankHolidayAdd.",
                sig_d_bool, fn_jour_ferie),
            D_DEF("JourF\xc3\xa9" "ri\xc3\xa9" "Ajoute", "JourFerieAjoute", "BankHolidayAdd", "Dates",
                "Ajoute un jour férié au calendrier (vide par défaut). Accepte les formats compacts \"MMJJ\" (récurrent annuel) ou \"AAAAMMJJ\" (année spécifique), une chaîne LF-jointe pour plusieurs entrées, une date ou dateheure typée, ou un tableau de plusieurs valeurs. Retourne Vrai si tous ajoutés.",
                "Adds a bank holiday to the calendar (empty by default). Accepts compact \"MMJJ\" (yearly recurring) or \"AAAAMMJJ\" (specific year) string formats, an LF-joined multi-line string, a typed date or datetime, or an array of multiple values. Returns True if all were added.",
                sig_jfa, fn_jour_ferie_ajoute),
            D_DEF("JourF\xc3\xa9" "ri\xc3\xa9" "Liste", "JourFerieListe", "BankHolidayList", "Dates",
                "Liste des jours fériés enregistrés, LF-jointe (\"MMJJ\" ou \"AAAAMMJJ\" par entrée).",
                "List of registered bank holidays, LF-joined (\"MMJJ\" or \"AAAAMMJJ\" per entry).",
                sig_jf_liste, fn_jour_ferie_liste),
            D_DEF("JourF\xc3\xa9" "ri\xc3\xa9" "SupprimeTout", "JourFerieSupprimeTout", "BankHolidayDeleteAll", "Dates",
                "Vide le calendrier des jours fériés enregistrés.",
                "Clears the calendar of registered bank holidays.",
                sig_jf_suppr_tt, fn_jour_ferie_supprime_tout),

            D_DEF("Age", NULL, "Age", "Dates",
                "Âge à partir d'une date de naissance.", "Age from a birth date.",
                sig_age, fn_age),
            D_DEF("Demain", NULL, "Tomorrow", "Dates",
                "Date du lendemain (ou date + n).", "Tomorrow (or date + n days).",
                sig_demain, fn_demain),
            D_DEF("Hier", NULL, "Yesterday", "Dates",
                "Date de la veille (ou date - n).", "Yesterday (or date - n days).",
                sig_demain, fn_hier),
            D_DEF("JourEnCours", NULL, "CurrentDay", "Dates",
                "Jour du mois courant.", "Current day of the month.",
                sig_no_arg, fn_jour_en_cours),
            D_DEF("JourDansLAnn\xc3\xa9" "eVersDate", "JourDansLAnneeVersDate", "DayOfYearToDate", "Dates",
                "Date depuis un numéro de jour dans l'année.", "Date from day-of-year number.",
                sig_day_year, fn_jour_annee_vers_date),
            D_DEF("P\xc3\xa2" "ques", "Paques", "Easter", "Dates",
                "Dimanche de Pâques pour l'année donnée (calendrier grégorien occidental, non valable pour le calendrier orthodoxe).",
                "Easter Sunday for the given year (Western Gregorian calendar, not valid for the Orthodox calendar).",
                sig_paques, fn_paques),

            D_DEF("EntierVersJour", NULL, "IntegerToDay", "Dates",
                "Entier en numéro de jour de semaine.", "Integer to day-of-week number.",
                sig_i_s, fn_entier_vers_jour),
            D_DEF("EntierVersJourEnLettre", NULL, "IntegerToDayInAlpha", "Dates",
                "Nom du jour de la semaine (Lundi à Dimanche, capitalisé) pour un entier (jours depuis 01/01/1800, base 1), une chaîne numérique ou une date. Langue : français uniquement.",
                "Day-of-week name (Lundi to Dimanche, capitalised) for an integer (days since 1800-01-01, base 1), a numeric string or a date. Language: French only.",
                sig_jour_lettre, fn_entier_vers_jour_lettre),
            D_DEF("EntierVersMoisEnLettre", NULL, "IntegerToMonthInAlpha", "Dates",
                "Nom du mois (Janvier à Décembre, capitalisé) pour un entier (jours depuis 01/01/1800, base 1), une chaîne numérique ou une date. Langue : français uniquement.",
                "Month name (Janvier to Décembre, capitalised) for an integer (days since 1800-01-01, base 1), a numeric string or a date. Language: French only.",
                sig_mois_lettre, fn_entier_vers_mois_lettre),
            D_DEF("EntierVersNum\xc3\xa9" "roDeSemaine", "EntierVersNumeroDeSemaine", "IntegerToWeekNumber", "Dates",
                "Numéro de semaine pour un entier représentant le nombre de jours écoulés depuis le 01/01/1800 (base 1). Par défaut, applique la convention canon (semaine 0 si début janvier hors semaine 1, semaine 53 si fin décembre bascule sur la semaine 1 suivante). Avec iso8601, applique ISO 8601 strict (semaine 1 = celle contenant le 1er jeudi de l'année).",
                "Week number for an integer representing the number of days since 1800-01-01 (base 1). Defaults to the canonical convention (week 0 for early January outside week 1, week 53 for late December when the week rolls over to the following week 1). With iso8601, applies strict ISO 8601 (week 1 = the one containing the year's first Thursday).",
                sig_i_week, fn_entier_vers_semaine),

            D_DEF("DateHeureDiff\xc3\xa9" "rence", "DateHeureDifference", "DateTimeDifference", "Dates",
                "Différence entre deux date+heures, sous forme de durée signée en millisecondes (cohérent avec l'opérateur '-').",
                "Difference between two datetimes, as a signed duration in milliseconds (consistent with the '-' operator).",
                sig_dhdh_dur, fn_dateheure_difference),
            D_DEF("DateHeureVersCha\xc3\xae" "ne", "DateHeureVersChaine", "DateTimeToString", "Dates",
                "Formate une date+heure en chaîne.", "Format a datetime to string.",
                sig_dh_s, fn_dateheure_vers_chaine),
            D_DEF("R\xc3\xa9" "elVersDateHeure", "ReelVersDateHeure", "RealToDateTime", "Dates",
                "Convertit un réel (Excel days) en DateHeure.",
                "Convert a real (Excel days) to a DateTime.",
                sig_reel_dh, fn_reel_vers_dateheure),
            D_DEF("DateHeureVersR\xc3\xa9" "el", "DateHeureVersReel", "DateTimeToReal", "Dates",
                "Convertit une DateHeure en réel (Excel days).",
                "Convert a DateTime to a real (Excel days).",
                sig_dh_reel, fn_dateheure_vers_reel),

            D_DEF("Cha\xc3\xae" "neVersDur\xc3\xa9" "e", "ChaineVersDuree", "StringToDuration", "Dates",
                "Parse une chaîne en durée selon un format (constante duréeCentième/duréeMillième ou template chaîne).",
                "Parse a string to duration according to a format (constant durationCenti/durationMilli or template string).",
                sig_s_fmt_dur, fn_chaine_vers_duree),
            D_DEF("Dur\xc3\xa9" "eVersCha\xc3\xae" "ne", "DureeVersChaine", "DurationToString", "Dates",
                "Formate une durée en chaîne.", "Format a duration to string.",
                sig_dur_s, fn_duree_vers_chaine),

            /* Chrono */
            D_DEF("ChronoD\xc3\xa9" "but", "ChronoDebut", "ChronoStart", "Chrono",
                "Démarre un chronomètre.", "Start a chronometer.",
                sig_chrono_v, fn_chrono_debut),
            D_DEF("ChronoFin", NULL, "ChronoEnd", "Chrono",
                "Arrête un chronomètre et retourne les ms.", "Stop a chronometer and return ms.",
                sig_chrono_ms, fn_chrono_fin),
            D_DEF("ChronoPause", NULL, "ChronoPause", "Chrono",
                "Met en pause un chronomètre.", "Pause a chronometer.",
                sig_chrono_v, fn_chrono_pause),
            D_DEF("ChronoReprend", NULL, "ChronoResume", "Chrono",
                "Reprend un chronomètre en pause.", "Resume a paused chronometer.",
                sig_chrono_v, fn_chrono_reprend),
            D_DEF("ChronoRAZ", NULL, "ChronoReset", "Chrono",
                "Remet à zéro.", "Reset a chronometer.",
                sig_chrono_v, fn_chrono_raz),
            D_DEF("ChronoValeur", NULL, "ChronoValue", "Chrono",
                "Valeur courante (ms).", "Current value (ms).",
                sig_chrono_ms, fn_chrono_valeur),
        };
#undef D_DEF

        owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    }

    /* Constantes maskDate* */
    {
        /* nameEN sourcé de la doc bilingue :
         * mask* identiques sauf maskDateSystème->maskDateSystem et
         * maskDateDuréeRelative->maskDateRelativeDuration. */
        #define C_MASK(nm, val, dfr, den, en) {nm, "Dates", INTEGER_VAL(val), dfr, den, en}
        static const OwlConstDef DATE_MASK_CONSTS[] = {
            C_MASK("maskDateEmail",       OWL_MASK_DATE_EMAIL,
                   "Format RFC-5322 (email/RSS), fuseau local.",
                   "RFC-5322 format (email/RSS), local timezone.",
                   "maskDateEmail"),
            C_MASK("maskDateEmailUTC",    OWL_MASK_DATE_EMAIL_UTC,
                   "Format RFC-5322 (email/RSS), UTC.",
                   "RFC-5322 format (email/RSS), UTC.",
                   "maskDateEmailUTC"),
            C_MASK("maskDateInternet",    OWL_MASK_DATE_INTERNET,
                   "Format RFC-3339 (ISO 8601), fuseau local.",
                   "RFC-3339 format (ISO 8601), local timezone.",
                   "maskDateInternet"),
            C_MASK("maskDateInternetUTC", OWL_MASK_DATE_INTERNETUTC,
                   "Format RFC-3339 (ISO 8601), UTC.",
                   "RFC-3339 format (ISO 8601), UTC.",
                   "maskDateInternetUTC"),
            C_MASK("maskDateSyst\xc3\xa8" "me", OWL_MASK_DATE_SYSTEME,
                   "Format système (projet).",
                   "System format (project).",
                   "maskDateSystem"),
            C_MASK("maskDateDur\xc3\xa9" "eRelative", OWL_MASK_DATE_DUREE_RELATIVE,
                   "Durée relative depuis maintenant (\"il y a 8 mois\", \"dans 6 heures\").",
                   "Relative duration from now (\"8 months ago\", \"in 6 hours\").",
                   "maskDateRelativeDuration"),
        };
        #undef C_MASK
        /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
         * (maskDateSysteme, maskDateDureeRelative...). */
        owl_registry_register_consts(DATE_MASK_CONSTS,
            (int)(sizeof(DATE_MASK_CONSTS)/sizeof(DATE_MASK_CONSTS[0])));
    }

    /* Constante iso8601 pour NumeroDeSemaine 2e param (doc 3027027) */
    {
        static const OwlConstDef WEEK_FORMAT_CONSTS[] = {
            {"iso8601", "Dates", INTEGER_VAL(OWL_ISO8601),
             "Format ISO 8601 pour NuméroDeSemaine (semaine 1 = celle contenant le 1er jeudi).",
             "ISO 8601 format for WeekNumber (week 1 contains the first Thursday).",
             "iso8601"},
        };
        /* register_consts auto-enregistre dans vm.globals. */
        owl_registry_register_consts(WEEK_FORMAT_CONSTS,
            (int)(sizeof(WEEK_FORMAT_CONSTS)/sizeof(WEEK_FORMAT_CONSTS[0])));
    }

    /* Constantes duréeCentième / duréeMillième (doc 3027002) */
    {
        #define C_DUREE(nm, val, dfr, den, en) {nm, "Dates", INTEGER_VAL(val), dfr, den, en}
        static const OwlConstDef DUREE_CONSTS[] = {
            C_DUREE("dur\xc3\xa9" "eCenti\xc3\xa8" "me", OWL_DUREE_CENTIEME,
                    "Format \"+JHHMMSSCC\" pour ChaîneVersDurée (précision centième).",
                    "Format \"+JHHMMSSCC\" for ChaineVersDuree (hundredth precision).", "durationCenti"),
            C_DUREE("dur\xc3\xa9" "eMilli\xc3\xa8" "me", OWL_DUREE_MILLIEME,
                    "Format \"+JHHMMSSCCC\" (avec millisecondes).",
                    "Format \"+JHHMMSSCCC\" (with milliseconds).", "durationMilli"),
        };
        #undef C_DUREE
        /* register_consts auto-enregistre + auto-alias ASCII
         * (duréeCentième → dureeCentieme, duréeMillième → dureeMillieme). */
        owl_registry_register_consts(DUREE_CONSTS,
            (int)(sizeof(DUREE_CONSTS)/sizeof(DUREE_CONSTS[0])));
    }

    /* Prefix methods on Chrono instances: c.Début(), c.Fin(), c.Pause(), ...
     * Pattern aligne sur stdlib_array.c : canon accent FR + alias ASCII +
     * alias EN, chacun enregistre explicitement (la map de methods est un
     * lookup direct par nom, pas accent-insensible comme les fields). */
    owl_register_method(METHOD_TYPE_CHRONO, "D\xc3\xa9" "but", fn_chrono_debut,   -1);  /* Début (canon FR) */
    owl_register_method(METHOD_TYPE_CHRONO, "Debut",           fn_chrono_debut,   -1);  /* alias ASCII */
    owl_register_method(METHOD_TYPE_CHRONO, "Start",           fn_chrono_debut,   -1);  /* alias EN */
    owl_register_method(METHOD_TYPE_CHRONO, "Fin",             fn_chrono_fin,     -1);
    owl_register_method(METHOD_TYPE_CHRONO, "End",             fn_chrono_fin,     -1);
    owl_register_method(METHOD_TYPE_CHRONO, "Pause",           fn_chrono_pause,   -1);
    owl_register_method(METHOD_TYPE_CHRONO, "Reprend",         fn_chrono_reprend, -1);
    owl_register_method(METHOD_TYPE_CHRONO, "Resume",          fn_chrono_reprend, -1);
    owl_register_method(METHOD_TYPE_CHRONO, "RAZ",             fn_chrono_raz,     -1);
    owl_register_method(METHOD_TYPE_CHRONO, "Reset",           fn_chrono_raz,     -1);
    owl_register_method(METHOD_TYPE_CHRONO, "Valeur",          fn_chrono_valeur,  -1);
    owl_register_method(METHOD_TYPE_CHRONO, "Value",           fn_chrono_valeur,  -1);


    /* Extension methods on String -> Date/Time */
    owl_register_method(METHOD_TYPE_STRING, "VersDate",    fn_chaine_vers_date, -1);
    owl_register_method(METHOD_TYPE_STRING, "ToDate",      fn_chaine_vers_date, -1);
    owl_register_method(METHOD_TYPE_STRING, "VersHeure",   fn_chaine_vers_heure, 1);
    owl_register_method(METHOD_TYPE_STRING, "ToTime",      fn_chaine_vers_heure, 1);
    owl_register_method(METHOD_TYPE_STRING, "VersDur\xc3\xa9" "e", fn_chaine_vers_duree, -1);
    owl_register_method(METHOD_TYPE_STRING, "VersDuree",   fn_chaine_vers_duree, -1);
    owl_register_method(METHOD_TYPE_STRING, "ToDuration",  fn_chaine_vers_duree, -1);

    /* Extension methods on Date */
    owl_register_method(METHOD_TYPE_DATE, "VersChaine",       fn_date_vers_chaine, -1);
    owl_register_method(METHOD_TYPE_DATE, "ToString",         fn_date_vers_chaine, -1);
    owl_register_method(METHOD_TYPE_DATE, "VersEntier",       fn_date_vers_entier, -1);
    owl_register_method(METHOD_TYPE_DATE, "ToInteger",        fn_date_vers_entier, -1);
    owl_register_method(METHOD_TYPE_DATE, "Annee",            fn_annee, 1);
    owl_register_method(METHOD_TYPE_DATE, "Year",             fn_annee, 1);
    owl_register_method(METHOD_TYPE_DATE, "Mois",             fn_mois, 1);
    owl_register_method(METHOD_TYPE_DATE, "Month",            fn_mois, 1);
    owl_register_method(METHOD_TYPE_DATE, "Jour",             fn_jour, 1);
    owl_register_method(METHOD_TYPE_DATE, "Day",              fn_jour, 1);
    owl_register_method(METHOD_TYPE_DATE, "JourDeLaSemaine",         fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "VersJourDeLaSemaine",     fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "DateVersJourDeLaSemaine", fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "VersJour",                fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "DateVersJour",            fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "DayOfWeek",               fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "EstValide",        fn_date_valide, 1);
    owl_register_method(METHOD_TYPE_DATE, "IsValid",          fn_date_valide, 1);
    owl_register_method(METHOD_TYPE_DATE, "JourEnLettre",     fn_date_vers_jour_en_lettre, 1);
    owl_register_method(METHOD_TYPE_DATE, "DayName",          fn_date_vers_jour_en_lettre, 1);
    owl_register_method(METHOD_TYPE_DATE, "MoisEnLettre",         fn_date_vers_mois_en_lettre, 1);
    owl_register_method(METHOD_TYPE_DATE, "MonthName",            fn_date_vers_mois_en_lettre, 1);
    owl_register_method(METHOD_TYPE_DATE, "JourDansLAnnee",      fn_jour_dans_annee, 1);
    owl_register_method(METHOD_TYPE_DATE, "DayOfYear",           fn_jour_dans_annee, 1);
    owl_register_method(METHOD_TYPE_DATE, "NumeroDeSemaine",     fn_numero_de_semaine, -1);  /* formes 1/2 args (compile-time valide) */
    owl_register_method(METHOD_TYPE_DATE, "WeekNumber",          fn_numero_de_semaine, -1);
    owl_register_method(METHOD_TYPE_DATE, "PremierJourDuMois",   fn_premier_jour_du_mois, 1);
    owl_register_method(METHOD_TYPE_DATE, "FirstDayOfMonth",     fn_premier_jour_du_mois, 1);
    owl_register_method(METHOD_TYPE_DATE, "DernierJourDuMois",   fn_dernier_jour_du_mois, 1);
    owl_register_method(METHOD_TYPE_DATE, "LastDayOfMonth",      fn_dernier_jour_du_mois, 1);
    owl_register_method(METHOD_TYPE_DATE, "PremierJourDeLaSemaine", fn_premier_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "FirstDayOfWeek",      fn_premier_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "DernierJourDeLaSemaine", fn_dernier_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "LastDayOfWeek",       fn_dernier_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATE, "EstFerie",            fn_jour_ferie, 1);
    owl_register_method(METHOD_TYPE_DATE, "IsBankHoliday",       fn_jour_ferie, 1);

    /* Extension methods on Time */
    owl_register_method(METHOD_TYPE_TIME, "VersChaine",  fn_heure_vers_chaine, -1);  /* formes 1/2 args (compile-time valide) */
    owl_register_method(METHOD_TYPE_TIME, "ToString",    fn_heure_vers_chaine, -1);
    owl_register_method(METHOD_TYPE_TIME, "VersEntier",  fn_heure_vers_entier, 1);
    owl_register_method(METHOD_TYPE_TIME, "ToInteger",   fn_heure_vers_entier, 1);

    /* Extension methods on DateTime */
    owl_register_method(METHOD_TYPE_DATETIME, "Annee",            fn_annee, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "Year",             fn_annee, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "Mois",             fn_mois, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "Month",            fn_mois, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "Jour",             fn_jour, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "Day",              fn_jour, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "JourDeLaSemaine",         fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "VersJourDeLaSemaine",     fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "DateVersJourDeLaSemaine", fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "VersJour",                fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "DateVersJour",            fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "DayOfWeek",               fn_jour_semaine, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "VersChaine",       fn_dateheure_vers_chaine, -1);
    owl_register_method(METHOD_TYPE_DATETIME, "ToString",         fn_dateheure_vers_chaine, -1);
    owl_register_method(METHOD_TYPE_DATETIME, "VersEpoch",        fn_dateheure_vers_epoch, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "ToEpoch",          fn_dateheure_vers_epoch, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "EstValide",        fn_dateheure_valide, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "IsValid",          fn_dateheure_valide, 1);
    owl_register_method(METHOD_TYPE_DATETIME, "VersR\xc3\xa9" "el",     fn_dateheure_vers_reel, -1);
    owl_register_method(METHOD_TYPE_DATETIME, "VersReel",                fn_dateheure_vers_reel, -1);
    owl_register_method(METHOD_TYPE_DATETIME, "ToReal",                  fn_dateheure_vers_reel, -1);
    owl_register_method(METHOD_TYPE_DATETIME, "DateHeureVersR\xc3\xa9" "el", fn_dateheure_vers_reel, -1);
    owl_register_method(METHOD_TYPE_DATETIME, "DateHeureVersReel",       fn_dateheure_vers_reel, -1);

    /* Extension methods on Duration */
    owl_register_method(METHOD_TYPE_DURATION, "VersChaine",  fn_duree_vers_chaine, -1);
    owl_register_method(METHOD_TYPE_DURATION, "ToString",    fn_duree_vers_chaine, -1);
}
