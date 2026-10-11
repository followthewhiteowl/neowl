// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_similarity.c — Famille « SimilaritéTexte » : distance et
 * similarité entre chaînes (extension OWL, hors panel canon).
 *
 * SOCLE de la famille : un helper UTF-8 codepoint-level partagé +
 * TexteDistance (distance de Levenshtein). Les algos suivants
 * (Jaro-Winkler, Damerau, n-grammes…) réutiliseront le même décodeur
 * codepoint et la même normalisation par défaut.
 *
 * Choix structurants :
 *  - Tout le calcul opère sur des CODEPOINTS (uint32), jamais sur des
 *    octets. "café" fait 4 codepoints, pas 5 octets — sinon la distance
 *    surcompte les caractères multi-octets UTF-8.
 *  - Normalisation par défaut : insensible casse + accents. Le repli
 *    accent est une SUPPRESSION simple du diacritique (ü→u, é→e, à→a) ;
 *    pas d'expansion (ü→ue). Aligné sur le repli identifiants du scanner.
 *  - Pas d'argument d'options à ce stade (réglage fin reporté).
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_errors.h"
#include "vm.h"
#include "memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ================================================================
 * Décodage UTF-8 → codepoints + folding par défaut (casse + accents).
 *
 * Helper partagé de la famille. Décode une chaîne UTF-8 en tableau de
 * codepoints uint32, en appliquant la normalisation par défaut sur
 * chaque codepoint :
 *   - ASCII A-Z → minuscule
 *   - Latin-1 accentué (À..ÿ) → lettre de base ASCII (suppression du
 *     diacritique, mise en minuscule)
 *   - tout autre codepoint (CJK, emoji, symboles, ASCII non-lettre) →
 *     conservé tel quel, en tant que codepoint distinct
 *
 * La distinction « conservé tel quel » est ce qui rend le comptage
 * réellement codepoint-level : deux idéogrammes CJK différents (non
 * repliables vers l'ASCII) restent deux unités distinctes, pas leurs
 * octets respectifs.
 * ================================================================ */

/* Décode une séquence UTF-8 en un codepoint brut (pas de folding).
 * Écrit le codepoint dans *out, retourne le nombre d'octets consommés.
 * Sur séquence tronquée/invalide : codepoint = octet brut, consume 1. */
static int u8_decode_cp(const char* s, int len, uint32_t* out) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *out = c; return 1; }
    if ((c & 0xE0) == 0xC0 && len >= 2 && ((unsigned char)s[1] & 0xC0) == 0x80) {
        *out = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && len >= 3 &&
        ((unsigned char)s[1] & 0xC0) == 0x80 && ((unsigned char)s[2] & 0xC0) == 0x80) {
        *out = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)((unsigned char)s[1] & 0x3F) << 6) |
               ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && len >= 4 &&
        ((unsigned char)s[1] & 0xC0) == 0x80 && ((unsigned char)s[2] & 0xC0) == 0x80 &&
        ((unsigned char)s[3] & 0xC0) == 0x80) {
        *out = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((unsigned char)s[1] & 0x3F) << 12) |
               ((uint32_t)((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
        return 4;
    }
    /* Invalide : octet brut comme codepoint, on avance d'un cran. */
    *out = c;
    return 1;
}

/* Replie un codepoint vers sa forme normalisée par défaut (minuscule +
 * sans accent). Codepoints non repliables retournés inchangés.
 * Le mapping Latin-1 reprend la table d'accent-stripping des identifiants
 * (cf scanner.c::normalize_utf8_char) pour cohérence FR/DE/IT. */
static uint32_t cp_fold_default(uint32_t cp) {
    if (cp < 0x80) {
        /* ASCII : minuscule, le reste inchangé. */
        if (cp >= 'A' && cp <= 'Z') return cp + 32;
        return cp;
    }
    switch (cp) {
        /* A variants */
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
            return 'a';
        /* C cedilla */
        case 0xC7: case 0xE7:
            return 'c';
        /* E variants */
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
            return 'e';
        /* I variants */
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
            return 'i';
        /* N tilde */
        case 0xD1: case 0xF1:
            return 'n';
        /* O variants */
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
            return 'o';
        /* U variants */
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC:
            return 'u';
        /* Y variants */
        case 0xDD: case 0xFD: case 0xFF:
            return 'y';
        default:
            /* Non repliable (CJK, emoji, Latin-1 non-lettre, etc.) :
             * conservé tel quel — reste un codepoint distinct. */
            return cp;
    }
}

/* Décode `s` (len octets UTF-8) en un tableau de codepoints alloué via
 * malloc. *outCount reçoit le nombre de codepoints. Si fold==true, chaque
 * codepoint est replié (casse + accents). Le caller doit free() le retour.
 * Retourne NULL seulement si l'allocation échoue (len==0 → tableau vide
 * non-NULL de 1 élément pour rester déréférençable). */
static uint32_t* similarity_decode(const char* s, int len, bool fold, int* outCount) {
    /* Borne haute : au plus `len` codepoints (1 octet = 1 cp au minimum). */
    int cap = len > 0 ? len : 1;
    uint32_t* cps = (uint32_t*)malloc((size_t)cap * sizeof(uint32_t));
    if (!cps) { *outCount = 0; return NULL; }
    int n = 0, i = 0;
    while (i < len) {
        uint32_t cp;
        int consumed = u8_decode_cp(s + i, len - i, &cp);
        cps[n++] = fold ? cp_fold_default(cp) : cp;
        i += consumed;
    }
    *outCount = n;
    return cps;
}

/* ================================================================
 * Levenshtein sur codepoints.
 *
 * Programmation dynamique en mémoire O(min(m,n)) (deux lignes glissantes).
 * Opère sur deux tableaux de codepoints déjà décodés/foldés par l'appelant.
 * ================================================================ */
static int levenshtein_cp(const uint32_t* a, int la, const uint32_t* b, int lb) {
    if (la == 0) return lb;
    if (lb == 0) return la;
    /* On itère sur `a` en lignes, `b` en colonnes. On garde la colonne la
     * plus courte en mémoire vive. */
    const uint32_t* row = b;  int rowLen = lb;
    const uint32_t* col = a;  int colLen = la;
    if (rowLen < colLen) {
        /* Garde la dimension mémoire = min(la,lb). */
        const uint32_t* t = row; row = col; col = t;
        int tl = rowLen; rowLen = colLen; colLen = tl;
    }
    /* prev/cur de taille colLen+1. */
    int* prev = (int*)malloc((size_t)(colLen + 1) * sizeof(int));
    int* cur  = (int*)malloc((size_t)(colLen + 1) * sizeof(int));
    if (!prev || !cur) { free(prev); free(cur); return (la > lb) ? la : lb; }

    for (int j = 0; j <= colLen; j++) prev[j] = j;
    for (int i = 1; i <= rowLen; i++) {
        cur[0] = i;
        uint32_t ri = row[i - 1];
        for (int j = 1; j <= colLen; j++) {
            int cost = (ri == col[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;        /* suppression */
            int ins = cur[j - 1] + 1;     /* insertion */
            int sub = prev[j - 1] + cost; /* substitution */
            int m = del < ins ? del : ins;
            cur[j] = m < sub ? m : sub;
        }
        int* swap = prev; prev = cur; cur = swap;
    }
    int result = prev[colLen];
    free(prev);
    free(cur);
    return result;
}

/* ================================================================
 * Damerau-Levenshtein OSA (Optimal String Alignment) sur codepoints.
 *
 * Variante OSA : ajoute la transposition de DEUX caractères adjacents au
 * coût 1, en plus des trois opérations Levenshtein (insertion, suppression,
 * substitution). Contrainte OSA : aucune sous-chaîne n'est éditée plus
 * d'une fois — on ne ré-édite pas un segment déjà transposé. C'est le
 * comportement par défaut de la plupart des implémentations dites
 * « Damerau » et il ne nécessite pas de table d'alphabet (contrairement à
 * la vraie distance de Damerau-Levenshtein illimitée).
 *
 * Conséquence concrète : "ca" -> "ac" vaut 1 (une transposition) là où
 * Levenshtein pur compte 2 substitutions.
 *
 * Implémentation : DP plein O(m*n) en mémoire (3 lignes glissantes — il
 * faut la ligne i-2 pour la transposition, contrairement à Levenshtein
 * qui se contente de i-1).
 * ================================================================ */
static int damerau_osa_cp(const uint32_t* a, int la, const uint32_t* b, int lb) {
    if (la == 0) return lb;
    if (lb == 0) return la;

    /* Trois lignes de taille lb+1 : i-2, i-1, i. */
    int* prev2 = (int*)malloc((size_t)(lb + 1) * sizeof(int));
    int* prev1 = (int*)malloc((size_t)(lb + 1) * sizeof(int));
    int* cur   = (int*)malloc((size_t)(lb + 1) * sizeof(int));
    if (!prev2 || !prev1 || !cur) {
        free(prev2); free(prev1); free(cur);
        return (la > lb) ? la : lb;
    }

    for (int j = 0; j <= lb; j++) prev1[j] = j;

    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        uint32_t ai = a[i - 1];
        for (int j = 1; j <= lb; j++) {
            uint32_t bj = b[j - 1];
            int cost = (ai == bj) ? 0 : 1;
            int del = prev1[j] + 1;        /* suppression */
            int ins = cur[j - 1] + 1;      /* insertion */
            int sub = prev1[j - 1] + cost; /* substitution */
            int m = del < ins ? del : ins;
            m = m < sub ? m : sub;
            /* Transposition OSA : a[i-1]==b[j-2] && a[i-2]==b[j-1]. */
            if (i > 1 && j > 1 && ai == b[j - 2] && a[i - 2] == bj) {
                int trans = prev2[j - 2] + 1;
                if (trans < m) m = trans;
            }
            cur[j] = m;
        }
        /* Rotation des lignes : prev2 <- prev1 <- cur, cur réutilise prev2. */
        int* tmp = prev2;
        prev2 = prev1;
        prev1 = cur;
        cur = tmp;
    }

    int result = prev1[lb];
    free(prev2);
    free(prev1);
    free(cur);
    return result;
}

/* ================================================================
 * LCS (plus longue sous-séquence commune) sur codepoints.
 *
 * Programmation dynamique pleine O(la*lb) en temps, O(min(la,lb))
 * en mémoire (une ligne glissante). On garde la dimension mémoire
 * sur la plus courte des deux chaînes.
 * ================================================================ */
static int lcs_cp(const uint32_t* a, int la, const uint32_t* b, int lb) {
    if (la == 0 || lb == 0) return 0;
    /* Itère sur la dimension longue en lignes, la courte en colonnes. */
    const uint32_t* rows = a; int rlen = la;
    const uint32_t* cols = b; int clen = lb;
    if (clen > rlen) {
        const uint32_t* t = rows; rows = cols; cols = t;
        int tl = rlen; rlen = clen; clen = tl;
    }
    int* prev = (int*)calloc((size_t)(clen + 1), sizeof(int));
    int* cur  = (int*)calloc((size_t)(clen + 1), sizeof(int));
    if (!prev || !cur) { free(prev); free(cur); return 0; }
    for (int i = 1; i <= rlen; i++) {
        uint32_t ri = rows[i - 1];
        for (int j = 1; j <= clen; j++) {
            if (ri == cols[j - 1]) {
                cur[j] = prev[j - 1] + 1;
            } else {
                int up = prev[j];
                int left = cur[j - 1];
                cur[j] = up > left ? up : left;
            }
        }
        int* swap = prev; prev = cur; cur = swap;
    }
    int result = prev[clen];
    free(prev);
    free(cur);
    return result;
}

/* ================================================================
 * Ratio de similarité Indel normalisé sur codepoints, dans [0,1].
 *
 * indel(a,b) = la + lb - 2*LCS(a,b)  (insertions/suppressions seules ;
 * une substitution = 1 suppr + 1 insert = coût 2).
 * ratio = 1 - indel / (la + lb).
 * Convention : deux vides -> 1.0, une seule vide -> 0.0.
 * ================================================================ */
static double similarity_ratio_cp(const uint32_t* a, int la, const uint32_t* b, int lb) {
    int total = la + lb;
    if (total == 0) return 1.0;          /* deux vides */
    int lcs = lcs_cp(a, la, b, lb);
    int indel = total - 2 * lcs;
    return 1.0 - (double)indel / (double)total;
}

/* ================================================================
 * Jaro + Jaro-Winkler sur codepoints.
 *
 * Jaro(s1,s2) :
 *  - deux vides -> 1.0 ; une seule vide -> 0.0.
 *  - fenêtre de correspondance w = floor(max(|s1|,|s2|)/2) - 1 (min 0).
 *  - m = nombre de caractères correspondants : même codepoint, positions
 *    distantes d'au plus w, chaque caractère de s2 apparié au plus une fois,
 *    balayage gauche->droite.
 *  - m == 0 -> 0.0.
 *  - t = (nombre de caractères appariés dont l'ordre diffère) / 2.
 *  - Jaro = (m/|s1| + m/|s2| + (m - t)/m) / 3.
 *
 * Jaro-Winkler : jw = Jaro + L * p * (1 - Jaro), p = 0.1, L = longueur du
 * préfixe commun (codepoints) plafonnée à 4. Boost inconditionnel (pas de
 * seuil sur Jaro).
 * ================================================================ */
static double jaro_cp(const uint32_t* a, int la, const uint32_t* b, int lb) {
    if (la == 0 && lb == 0) return 1.0;       /* deux vides */
    if (la == 0 || lb == 0) return 0.0;       /* une seule vide */

    int maxLen = la > lb ? la : lb;
    int w = maxLen / 2 - 1;
    if (w < 0) w = 0;

    bool* aMatch = (bool*)calloc((size_t)la, sizeof(bool));
    bool* bMatch = (bool*)calloc((size_t)lb, sizeof(bool));
    if (!aMatch || !bMatch) { free(aMatch); free(bMatch); return 0.0; }

    /* Comptage des correspondances (appariement unique gauche->droite). */
    int m = 0;
    for (int i = 0; i < la; i++) {
        int lo = i - w; if (lo < 0) lo = 0;
        int hi = i + w; if (hi > lb - 1) hi = lb - 1;
        for (int j = lo; j <= hi; j++) {
            if (bMatch[j] || a[i] != b[j]) continue;
            aMatch[i] = true;
            bMatch[j] = true;
            m++;
            break;
        }
    }

    if (m == 0) { free(aMatch); free(bMatch); return 0.0; }

    /* Transpositions : on parcourt les appariés de a et de b dans l'ordre de
     * leurs positions respectives ; chaque paire de codepoints qui diffère
     * compte une demi-transposition. */
    int t2 = 0; /* 2*t : nombre de positions appariées discordantes */
    int k = 0;
    for (int i = 0; i < la; i++) {
        if (!aMatch[i]) continue;
        while (!bMatch[k]) k++;
        if (a[i] != b[k]) t2++;
        k++;
    }

    free(aMatch);
    free(bMatch);

    double dm = (double)m;
    double t = (double)t2 / 2.0;
    return (dm / (double)la + dm / (double)lb + (dm - t) / dm) / 3.0;
}

static double jaro_winkler_cp(const uint32_t* a, int la, const uint32_t* b, int lb) {
    double jaro = jaro_cp(a, la, b, lb);
    /* Préfixe commun (codepoints), plafonné à 4. */
    int L = 0;
    int maxPref = la < lb ? la : lb;
    if (maxPref > 4) maxPref = 4;
    while (L < maxPref && a[L] == b[L]) L++;
    return jaro + (double)L * 0.1 * (1.0 - jaro);
}

/* ================================================================
 * Native TexteDistance / TextDistance.
 * ================================================================ */
static Value fn_texte_distance(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    int dist = levenshtein_cp(ca, la, cb, lb);

    free(ca);
    free(cb);
    return INTEGER_VAL(dist);
}

/* ================================================================
 * Native TexteDistanceDamerau / TextDistanceDamerau.
 * ================================================================ */
static Value fn_texte_distance_damerau(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    int dist = damerau_osa_cp(ca, la, cb, lb);

    free(ca);
    free(cb);
    return INTEGER_VAL(dist);
}

/* ================================================================
 * Native TexteDistanceHamming / TextDistanceHamming.
 *
 * Nombre de positions (codepoints) qui diffèrent entre deux chaînes de
 * MÊME longueur (après décodage codepoint + repli par défaut). La distance
 * de Hamming n'est définie que pour des longueurs égales : sur longueurs
 * différentes on pose un soft-error récupérable (ErreurDétectée) plutôt
 * qu'un retour silencieux trompeur.
 * ================================================================ */
static Value fn_texte_distance_hamming(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    if (la != lb) {
        char buf1[16], buf2[16];
        snprintf(buf1, sizeof(buf1), "%d", la);
        snprintf(buf2, sizeof(buf2), "%d", lb);
        owl_emit_native_error(E_NATIVE_HAMMING_LENGTH_MISMATCH, "TexteDistanceHamming",
                              "native", "TexteDistanceHamming",
                              "len1", buf1,
                              "len2", buf2,
                              NULL);
        free(ca);
        free(cb);
        return NIL_VAL;
    }

    int dist = 0;
    for (int i = 0; i < la; i++) {
        if (ca[i] != cb[i]) dist++;
    }

    free(ca);
    free(cb);
    owl_clear_error();
    return INTEGER_VAL(dist);
}

/* ================================================================
 * Native TexteSimilarité / TextSimilarity.
 *
 * Ratio de similarité Indel normalisé : réel dans [0,1] où 1 = chaînes
 * identiques (après repli casse + accents par défaut). Voir
 * similarity_ratio_cp pour la formule.
 * ================================================================ */
static Value fn_texte_similarite(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    double ratio = similarity_ratio_cp(ca, la, cb, lb);

    free(ca);
    free(cb);
    return REAL_VAL(ratio);
}

/* ================================================================
 * Native TexteSimilaritéPartielle / TextSimilarityPartial.
 *
 * Soit `c` la plus courte des deux chaînes (longueur s en codepoints),
 * `l` la plus longue. Le score est le maximum, sur toutes les
 * sous-chaînes contiguës de longueur s de `l`, de similarity_ratio_cp(c,
 * sous-chaîne). Longueurs égales -> identique à TexteSimilarité.
 * Convention : une vide -> 0.0, deux vides -> 1.0.
 * ================================================================ */
static Value fn_texte_similarite_partielle(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    /* Désigne court/long par longueur codepoint. */
    const uint32_t* shortS; int sLen;
    const uint32_t* longS;  int lLen;
    if (la <= lb) { shortS = ca; sLen = la; longS = cb; lLen = lb; }
    else          { shortS = cb; sLen = lb; longS = ca; lLen = la; }

    double best;
    if (sLen == 0) {
        /* Deux vides -> 1.0 ; une seule vide -> 0.0. */
        best = (lLen == 0) ? 1.0 : 0.0;
    } else if (sLen == lLen) {
        best = similarity_ratio_cp(shortS, sLen, longS, lLen);
    } else {
        best = 0.0;
        /* Fenêtre glissante de longueur sLen sur longS. */
        for (int start = 0; start + sLen <= lLen; start++) {
            double r = similarity_ratio_cp(shortS, sLen, longS + start, sLen);
            if (r > best) best = r;
        }
    }

    free(ca);
    free(cb);
    return REAL_VAL(best);
}

/* ================================================================
 * Tokenisation « mot à mot » sur codepoints foldés.
 *
 * Découpe une chaîne déjà décodée/foldée (tableau de codepoints) en jetons
 * sur les blancs ASCII (espace, tab, \r, \n, \f, \v). Pas de strip de
 * ponctuation : la ponctuation collée à un mot reste dans le jeton.
 *
 * Conséquence assumée des espaces multiples : deux séparateurs consécutifs
 * produisent un jeton vide (longueur 0) entre eux, et un séparateur en
 * tête/queue produit un jeton vide de bord. Ce comportement est
 * volontairement non normalisé (cf. scénario espaces multiples) — les
 * formules sont définies dessus.
 *
 * Un jeton est une tranche (offset, len) dans le tableau de codepoints
 * source ; aucune copie tant qu'on ne reconstruit pas les chaînes jointes.
 * ================================================================ */
typedef struct {
    int off;   /* index de début dans le tableau de codepoints source */
    int len;   /* longueur en codepoints */
} SimToken;

static bool cp_is_space(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' ||
           cp == '\f' || cp == '\v';
}

/* Découpe `cps` (n codepoints) en jetons. Alloue le tableau de SimToken via
 * malloc (caller free). *outCount reçoit le nombre de jetons. Sur chaîne
 * vide (n==0) : 0 jeton. */
static SimToken* sim_tokenize(const uint32_t* cps, int n, int* outCount) {
    /* Borne haute : au plus (n/2)+1 jetons, mais n+1 couvre tous les cas
     * (jetons vides inclus). */
    int cap = n + 1;
    SimToken* toks = (SimToken*)malloc((size_t)cap * sizeof(SimToken));
    if (!toks) { *outCount = 0; return NULL; }
    int count = 0;
    int i = 0;
    if (n == 0) { *outCount = 0; return toks; }
    while (i < n) {
        int start = i;
        while (i < n && !cp_is_space(cps[i])) i++;
        toks[count].off = start;
        toks[count].len = i - start;
        count++;
        if (i < n) {
            /* On est sur un séparateur : chaque blanc isolé sépare deux
             * jetons. Un blanc consomme un séparateur ; deux blancs
             * consécutifs créent un jeton vide entre eux. */
            i++;
            if (i == n) {
                /* Séparateur en fin : jeton vide de bord. */
                toks[count].off = i;
                toks[count].len = 0;
                count++;
            }
        }
    }
    *outCount = count;
    return toks;
}

/* Compare deux jetons par valeur de codepoint (ordre lexicographique
 * déterministe, indépendant de la locale). */
static int sim_token_cmp(const uint32_t* cps, const SimToken* x, const SimToken* y) {
    int m = x->len < y->len ? x->len : y->len;
    for (int k = 0; k < m; k++) {
        uint32_t cx = cps[x->off + k];
        uint32_t cy = cps[y->off + k];
        if (cx != cy) return cx < cy ? -1 : 1;
    }
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return 0;
}

/* Contexte de tri (qsort_r non portable : on passe le tableau source via
 * une variable de fichier protégée par le mono-threading du runtime). */
static const uint32_t* g_sim_sort_cps = NULL;
static int sim_token_qsort_cmp(const void* a, const void* b) {
    return sim_token_cmp(g_sim_sort_cps, (const SimToken*)a, (const SimToken*)b);
}

/* Trie un tableau de jetons en place par valeur de codepoint. */
static void sim_sort_tokens(const uint32_t* cps, SimToken* toks, int count) {
    g_sim_sort_cps = cps;
    qsort(toks, (size_t)count, sizeof(SimToken), sim_token_qsort_cmp);
    g_sim_sort_cps = NULL;
}

/* Joint une liste ordonnée de jetons (tranches dans `cps`) en un tableau de
 * codepoints, séparés par un espace. Alloue via malloc (caller free).
 * *outLen reçoit la longueur. count==0 → tableau vide. */
static uint32_t* sim_join_tokens(const uint32_t* cps, const SimToken* toks,
                                 int count, int* outLen) {
    int total = 0;
    for (int i = 0; i < count; i++) total += toks[i].len;
    if (count > 1) total += count - 1; /* séparateurs */
    uint32_t* out = (uint32_t*)malloc((size_t)(total > 0 ? total : 1) * sizeof(uint32_t));
    if (!out) { *outLen = 0; return NULL; }
    int w = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0) out[w++] = ' ';
        for (int k = 0; k < toks[i].len; k++) out[w++] = cps[toks[i].off + k];
    }
    *outLen = w;
    return out;
}

/* Déduplique un tableau de jetons DÉJÀ TRIÉ en place : retire les doublons
 * consécutifs. Retourne le nouveau nombre de jetons. */
static int sim_dedup_sorted(const uint32_t* cps, SimToken* toks, int count) {
    if (count <= 1) return count;
    int w = 1;
    for (int i = 1; i < count; i++) {
        if (sim_token_cmp(cps, &toks[w - 1], &toks[i]) != 0) {
            toks[w++] = toks[i];
        }
    }
    return w;
}

/* Recherche binaire d'un jeton dans un set trié. Retourne true si présent. */
static bool sim_set_contains(const uint32_t* cps, const SimToken* set, int n,
                             const uint32_t* qcps, const SimToken* q) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        /* Compare set[mid] (dans cps) avec q (dans qcps). */
        int m = set[mid].len < q->len ? set[mid].len : q->len;
        int rel = 0;
        for (int k = 0; k < m; k++) {
            uint32_t cx = cps[set[mid].off + k];
            uint32_t cy = qcps[q->off + k];
            if (cx != cy) { rel = cx < cy ? -1 : 1; break; }
        }
        if (rel == 0 && set[mid].len != q->len) rel = set[mid].len < q->len ? -1 : 1;
        if (rel == 0) return true;
        if (rel < 0) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

/* ================================================================
 * Cœur partagé JetonsTriés : trie les jetons de a et de b, joint par
 * espace, retourne ratio(sa, sb). Opère sur les codepoints déjà foldés.
 * ================================================================ */
static double sim_sorted_tokens_ratio(const uint32_t* ca, int la,
                                       const uint32_t* cb, int lb) {
    int na = 0, nb = 0;
    SimToken* ta = sim_tokenize(ca, la, &na);
    SimToken* tb = sim_tokenize(cb, lb, &nb);
    sim_sort_tokens(ca, ta, na);
    sim_sort_tokens(cb, tb, nb);
    int saLen = 0, sbLen = 0;
    uint32_t* sa = sim_join_tokens(ca, ta, na, &saLen);
    uint32_t* sb = sim_join_tokens(cb, tb, nb, &sbLen);
    double r = similarity_ratio_cp(sa, saLen, sb, sbLen);
    free(ta); free(tb); free(sa); free(sb);
    return r;
}

/* ================================================================
 * Cœur partagé JetonsEnsemble (token_set façon fuzzywuzzy) :
 *   inter = (setA ∩ setB) trié rejoint ; restA = (setA \ setB) trié ;
 *   restB = (setB \ setA) trié ; combA = trim(inter+" "+restA) ;
 *   combB = trim(inter+" "+restB) ;
 *   = max( ratio(inter,combA), ratio(inter,combB), ratio(combA,combB) ).
 * inter/restX étant déjà des jetons triés joints par espace, la
 * concaténation "inter + ' ' + restA" est déjà triée par bloc et le trim
 * est superflu hors des cas vides — on gère les vides explicitement pour
 * éviter un espace de tête/queue.
 * ================================================================ */
static double sim_set_tokens_ratio(const uint32_t* ca, int la,
                                   const uint32_t* cb, int lb) {
    /* Conventions vides héritées de ratio, appliquées AVANT la formule du
     * max : sans ce garde, ratio(inter="", combB="") vaudrait 1.0 et ferait
     * remonter le max à tort sur le cas « une seule chaîne vide ». */
    if (la == 0 || lb == 0) return (la == 0 && lb == 0) ? 1.0 : 0.0;
    int na = 0, nb = 0;
    SimToken* ta = sim_tokenize(ca, la, &na);
    SimToken* tb = sim_tokenize(cb, lb, &nb);
    /* setA / setB = jetons uniques triés. */
    sim_sort_tokens(ca, ta, na);
    sim_sort_tokens(cb, tb, nb);
    int sa = sim_dedup_sorted(ca, ta, na);
    int sb = sim_dedup_sorted(cb, tb, nb);

    /* Partitionne setA en inter (∈ setB) et restA (∉ setB). On collecte les
     * indices ; setA/setB étant triés, inter et restA le restent. */
    SimToken* interT = (SimToken*)malloc((size_t)(sa > 0 ? sa : 1) * sizeof(SimToken));
    SimToken* restAT = (SimToken*)malloc((size_t)(sa > 0 ? sa : 1) * sizeof(SimToken));
    SimToken* restBT = (SimToken*)malloc((size_t)(sb > 0 ? sb : 1) * sizeof(SimToken));
    int ni = 0, nra = 0, nrb = 0;
    for (int i = 0; i < sa; i++) {
        if (sim_set_contains(cb, tb, sb, ca, &ta[i])) interT[ni++] = ta[i];
        else restAT[nra++] = ta[i];
    }
    for (int j = 0; j < sb; j++) {
        if (!sim_set_contains(ca, ta, sa, cb, &tb[j])) restBT[nrb++] = tb[j];
    }

    /* Joint inter (depuis ca), restA (depuis ca), restB (depuis cb). */
    int interLen = 0, restALen = 0, restBLen = 0;
    uint32_t* inter = sim_join_tokens(ca, interT, ni, &interLen);
    uint32_t* restA = sim_join_tokens(ca, restAT, nra, &restALen);
    uint32_t* restB = sim_join_tokens(cb, restBT, nrb, &restBLen);

    /* combA = inter + " " + restA (trim : pas de séparateur si l'un est vide). */
    int combALen = interLen + restALen + ((interLen > 0 && restALen > 0) ? 1 : 0);
    int combBLen = interLen + restBLen + ((interLen > 0 && restBLen > 0) ? 1 : 0);
    uint32_t* combA = (uint32_t*)malloc((size_t)(combALen > 0 ? combALen : 1) * sizeof(uint32_t));
    uint32_t* combB = (uint32_t*)malloc((size_t)(combBLen > 0 ? combBLen : 1) * sizeof(uint32_t));
    int w = 0;
    for (int i = 0; i < interLen; i++) combA[w++] = inter[i];
    if (interLen > 0 && restALen > 0) combA[w++] = ' ';
    for (int i = 0; i < restALen; i++) combA[w++] = restA[i];
    w = 0;
    for (int i = 0; i < interLen; i++) combB[w++] = inter[i];
    if (interLen > 0 && restBLen > 0) combB[w++] = ' ';
    for (int i = 0; i < restBLen; i++) combB[w++] = restB[i];

    double r1 = similarity_ratio_cp(inter, interLen, combA, combALen);
    double r2 = similarity_ratio_cp(inter, interLen, combB, combBLen);
    double r3 = similarity_ratio_cp(combA, combALen, combB, combBLen);
    double best = r1 > r2 ? r1 : r2;
    if (r3 > best) best = r3;

    free(ta); free(tb);
    free(interT); free(restAT); free(restBT);
    free(inter); free(restA); free(restB);
    free(combA); free(combB);
    return best;
}

/* ================================================================
 * Native TexteSimilaritéJetonsTriés / TextSimilaritySortedTokens.
 *
 * Insensible à l'ordre des mots : les jetons sont triés alphabétiquement
 * (par codepoint) puis rejoints par un espace avant le calcul de ratio.
 * Tokenisation sur les blancs, repli casse + accents par défaut.
 * ================================================================ */
static Value fn_texte_similarite_jetons_tries(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    double r = sim_sorted_tokens_ratio(ca, la, cb, lb);

    free(ca);
    free(cb);
    return REAL_VAL(r);
}

/* ================================================================
 * Native TexteSimilaritéJetonsEnsemble / TextSimilaritySetTokens.
 *
 * Robuste aux mots en trop : compare les ensembles de jetons uniques.
 * Si tous les jetons de a sont présents dans b, le score vaut 1.
 * ================================================================ */
static Value fn_texte_similarite_jetons_ensemble(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    double r = sim_set_tokens_ratio(ca, la, cb, lb);

    free(ca);
    free(cb);
    return REAL_VAL(r);
}

/* ================================================================
 * Native TexteSimilaritéJaroWinkler / TextSimilarityJaroWinkler.
 *
 * Similarité de Jaro-Winkler : réel dans [0,1] favorisant les chaînes
 * partageant un préfixe commun (boost p=0.1, préfixe plafonné à 4).
 * Repli casse + accents par défaut. Voir jaro_winkler_cp pour la formule.
 * ================================================================ */
static Value fn_texte_similarite_jaro_winkler(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    double r = jaro_winkler_cp(ca, la, cb, lb);

    free(ca);
    free(cb);
    return REAL_VAL(r);
}

/* ================================================================
 * Similarité n-gram (trigrammes) sur codepoints — Dice / Jaccard.
 *
 * Modèle « trigramme » (n=3) à la pg_trgm : on encadre la séquence de
 * codepoints (déjà foldée) de n-1 = 2 sentinelles avant ET après, puis on
 * collecte toutes les fenêtres contiguës de 3 codepoints. La sentinelle est
 * le codepoint 0 (U+0000), valeur que le fold ne produit jamais sur une
 * chaîne réelle — elle ne peut donc pas entrer en collision avec un vrai
 * caractère. Une chaîne de k codepoints produit k+2 trigrammes (avant
 * déduplication).
 *
 * Conséquence des bornes (aucun cas spécial — elles émergent du padding) :
 *  - "" → padding [⊥⊥⊥⊥] → unique trigramme {⊥⊥⊥}.
 *  - deux vides → A = B = {⊥⊥⊥} → Dice = Jaccard = 1.0.
 *  - une vide vs non vide → {⊥⊥⊥} absent de l'autre → 0.0.
 *
 * Un trigramme est un triplet ordonné de codepoints. On stocke les triplets
 * dans un tableau, on trie (ordre lexicographique sur les 3 composantes),
 * puis on déduplique : l'ensemble est représenté par un tableau trié de
 * triplets uniques, ce qui rend intersection/union linéaires par fusion.
 * ================================================================ */
typedef struct {
    uint32_t a, b, c;
} SimTrigram;

static int trigram_cmp(const SimTrigram* x, const SimTrigram* y) {
    if (x->a != y->a) return x->a < y->a ? -1 : 1;
    if (x->b != y->b) return x->b < y->b ? -1 : 1;
    if (x->c != y->c) return x->c < y->c ? -1 : 1;
    return 0;
}

static int trigram_qsort_cmp(const void* a, const void* b) {
    return trigram_cmp((const SimTrigram*)a, (const SimTrigram*)b);
}

/* Construit l'ensemble (trié, dédupliqué) des trigrammes paddés de `cps`
 * (n codepoints). Alloue via malloc (caller free). *outCount reçoit la
 * taille de l'ensemble. Toujours non vide (≥ 1, le cas "" donne {⊥⊥⊥}). */
static SimTrigram* ngram_set_build(const uint32_t* cps, int n, int* outCount) {
    /* Séquence paddée : 2 sentinelles + n codepoints + 2 sentinelles. */
    int padded = n + 4;
    int count = padded - 3 + 1; /* = n + 2 fenêtres */
    SimTrigram* set = (SimTrigram*)malloc((size_t)count * sizeof(SimTrigram));
    if (!set) { *outCount = 0; return NULL; }

    /* Accès indexé à la séquence paddée sans la matérialiser : positions
     * [0,1] et [padded-2, padded-1] sont des sentinelles (codepoint 0). */
    #define PADDED_AT(idx) \
        (((idx) < 2 || (idx) >= n + 2) ? (uint32_t)0 : cps[(idx) - 2])

    for (int i = 0; i < count; i++) {
        set[i].a = PADDED_AT(i);
        set[i].b = PADDED_AT(i + 1);
        set[i].c = PADDED_AT(i + 2);
    }
    #undef PADDED_AT

    qsort(set, (size_t)count, sizeof(SimTrigram), trigram_qsort_cmp);
    /* Déduplication des triplets consécutifs (tableau désormais trié). */
    int w = count > 0 ? 1 : 0;
    for (int i = 1; i < count; i++) {
        if (trigram_cmp(&set[w - 1], &set[i]) != 0) set[w++] = set[i];
    }
    *outCount = w;
    return set;
}

/* Taille de l'intersection de deux ensembles de trigrammes triés (fusion). */
static int ngram_intersection(const SimTrigram* A, int na,
                              const SimTrigram* B, int nb) {
    int i = 0, j = 0, inter = 0;
    while (i < na && j < nb) {
        int c = trigram_cmp(&A[i], &B[j]);
        if (c == 0) { inter++; i++; j++; }
        else if (c < 0) i++;
        else j++;
    }
    return inter;
}

/* Coefficient de Sørensen-Dice sur les ensembles de trigrammes :
 * 2·|A∩B| / (|A|+|B|). |A|+|B| ≥ 2 (ensembles toujours non vides). */
static double ngram_dice_cp(const uint32_t* ca, int la, const uint32_t* cb, int lb) {
    int na = 0, nb = 0;
    SimTrigram* A = ngram_set_build(ca, la, &na);
    SimTrigram* B = ngram_set_build(cb, lb, &nb);
    int inter = ngram_intersection(A, na, B, nb);
    free(A);
    free(B);
    return 2.0 * (double)inter / (double)(na + nb);
}

/* Indice de Jaccard sur les ensembles de trigrammes :
 * |A∩B| / |A∪B|, avec |A∪B| = |A| + |B| - |A∩B|. */
static double ngram_jaccard_cp(const uint32_t* ca, int la, const uint32_t* cb, int lb) {
    int na = 0, nb = 0;
    SimTrigram* A = ngram_set_build(ca, la, &na);
    SimTrigram* B = ngram_set_build(cb, lb, &nb);
    int inter = ngram_intersection(A, na, B, nb);
    int uni = na + nb - inter;
    free(A);
    free(B);
    return (double)inter / (double)uni;
}

/* ================================================================
 * Native TexteSimilaritéDice / TextSimilarityDice.
 * ================================================================ */
static Value fn_texte_similarite_dice(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    double r = ngram_dice_cp(ca, la, cb, lb);

    free(ca);
    free(cb);
    return REAL_VAL(r);
}

/* ================================================================
 * Native TexteSimilaritéJaccard / TextSimilarityJaccard.
 * ================================================================ */
static Value fn_texte_similarite_jaccard(int argCount, Value* args) {
    (void)argCount;
    ObjString* sa = value_to_string(args[0]);
    ObjString* sb = value_to_string(args[1]);

    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(sa->chars, sa->length, true, &la);
    uint32_t* cb = similarity_decode(sb->chars, sb->length, true, &lb);

    double r = ngram_jaccard_cp(ca, la, cb, lb);

    free(ca);
    free(cb);
    return REAL_VAL(r);
}

/* ================================================================
 * Registry declarations.
 * ================================================================ */
static const OwlParam p_deux_textes[] = {
    {"texte1", "chaîne", "première chaîne à comparer"},
    {"texte2", "chaîne", "seconde chaîne à comparer"},
    {0}
};
static const OwlSignature sig_distance[] = {
    {2, p_deux_textes, "entier",
     "nombre minimal d'opérations unitaires (insertion, suppression, "
     "substitution) séparant les deux chaînes"},
    {0}
};
static const OwlSignature sig_distance_damerau[] = {
    {2, p_deux_textes, "entier",
     "nombre minimal d'opérations unitaires (insertion, suppression, "
     "substitution, transposition de deux caractères adjacents) séparant "
     "les deux chaînes"},
    {0}
};
static const OwlSignature sig_distance_hamming[] = {
    {2, p_deux_textes, "entier",
     "nombre de positions où les deux chaînes diffèrent (longueurs égales "
     "requises)"},
    {0}
};
static const OwlSignature sig_similarite[] = {
    {2, p_deux_textes, "réel",
     "score de similarité entre 0 et 1 (1 = chaînes identiques, 0 = "
     "totalement différentes)"},
    {0}
};
static const OwlSignature sig_similarite_partielle[] = {
    {2, p_deux_textes, "réel",
     "meilleur score de similarité entre 0 et 1 d'un alignement de la "
     "chaîne la plus courte sur une portion de la plus longue"},
    {0}
};
static const OwlSignature sig_similarite_jetons_tries[] = {
    {2, p_deux_textes, "réel",
     "score de similarité entre 0 et 1 calculé après tri des mots, "
     "insensible à l'ordre des mots"},
    {0}
};
static const OwlSignature sig_similarite_jetons_ensemble[] = {
    {2, p_deux_textes, "réel",
     "score de similarité entre 0 et 1 calculé sur les ensembles de mots, "
     "robuste aux mots supplémentaires"},
    {0}
};
static const OwlSignature sig_similarite_jaro_winkler[] = {
    {2, p_deux_textes, "réel",
     "score de similarité entre 0 et 1 (1 = chaînes identiques), majoré "
     "lorsque les chaînes partagent un préfixe commun"},
    {0}
};
static const OwlSignature sig_similarite_dice[] = {
    {2, p_deux_textes, "réel",
     "score de similarité entre 0 et 1 fondé sur les groupes de trois "
     "caractères communs aux deux chaînes (1 = identiques)"},
    {0}
};
static const OwlSignature sig_similarite_jaccard[] = {
    {2, p_deux_textes, "réel",
     "score de similarité entre 0 et 1 fondé sur le recouvrement des "
     "groupes de trois caractères des deux chaînes (1 = identiques)"},
    {0}
};

static const OwlNativeDef DEFS[] = {
    {
        .canonical  = "TexteDistance",
        .nameEN     = "TextDistance",
        .category   = "SimilaritéTexte",
        .descFR     = "Distance d'édition entre deux chaînes : nombre minimal "
                      "d'insertions, suppressions ou substitutions de caractères "
                      "pour transformer l'une en l'autre. Comparaison par "
                      "caractère (et non par octet), insensible à la casse et aux "
                      "accents par défaut. Retourne un entier supérieur ou égal à 0.",
        .descEN     = "Edit distance between two strings: the minimum number of "
                      "character insertions, deletions or substitutions needed to "
                      "turn one into the other. Compared per character (not per "
                      "byte), case- and accent-insensitive by default. Returns an "
                      "integer greater than or equal to 0.",
        .signatures = sig_distance,
        .impl       = fn_texte_distance,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical  = "TexteDistanceDamerau",
        .nameEN     = "TextDistanceDamerau",
        .category   = "SimilaritéTexte",
        .descFR     = "Distance d'édition entre deux chaînes tenant compte des "
                      "inversions : comme la distance d'édition classique "
                      "(insertion, suppression, substitution), mais l'échange de "
                      "deux caractères voisins compte pour une seule opération. "
                      "Mieux adaptée aux fautes de frappe (lettres inversées). "
                      "Comparaison par caractère, insensible à la casse et aux "
                      "accents par défaut. Retourne un entier supérieur ou égal à 0.",
        .descEN     = "Edit distance between two strings that accounts for "
                      "swaps: like the classic edit distance (insertion, "
                      "deletion, substitution), but swapping two adjacent "
                      "characters counts as a single operation. Better suited to "
                      "typos (transposed letters). Compared per character, case- "
                      "and accent-insensitive by default. Returns an integer "
                      "greater than or equal to 0.",
        .signatures = sig_distance_damerau,
        .impl       = fn_texte_distance_damerau,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical  = "TexteDistanceHamming",
        .nameEN     = "TextDistanceHamming",
        .category   = "SimilaritéTexte",
        .descFR     = "Nombre de positions où deux chaînes de même longueur "
                      "diffèrent. Comparaison par caractère, insensible à la "
                      "casse et aux accents par défaut. Les deux chaînes doivent "
                      "avoir la même longueur (en caractères, après normalisation) "
                      "sinon une erreur récupérable est déclenchée. Retourne un "
                      "entier supérieur ou égal à 0.",
        .descEN     = "Number of positions at which two equal-length strings "
                      "differ. Compared per character, case- and accent-"
                      "insensitive by default. Both strings must have the same "
                      "length (in characters, after normalization), otherwise a "
                      "recoverable error is raised. Returns an integer greater "
                      "than or equal to 0.",
        .signatures = sig_distance_hamming,
        .impl       = fn_texte_distance_hamming,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical      = "TexteSimilarité",
        .canonicalAscii = "TexteSimilarite",
        .nameEN         = "TextSimilarity",
        .category       = "SimilaritéTexte",
        .descFR     = "Mesure de ressemblance entre deux chaînes, exprimée par "
                      "un réel entre 0 et 1 : 1 lorsque les chaînes sont "
                      "identiques, 0 lorsqu'elles n'ont aucun caractère commun. "
                      "Le score décroît avec le nombre d'insertions et de "
                      "suppressions nécessaires pour passer de l'une à l'autre. "
                      "Comparaison par caractère, insensible à la casse et aux "
                      "accents par défaut. Deux chaînes vides valent 1 ; une "
                      "seule vide vaut 0.",
        .descEN     = "Measure of how alike two strings are, given as a real "
                      "number between 0 and 1: 1 when the strings are identical, "
                      "0 when they share no characters. The score decreases with "
                      "the number of insertions and deletions needed to turn one "
                      "into the other. Compared per character, case- and accent-"
                      "insensitive by default. Two empty strings score 1; a "
                      "single empty string scores 0.",
        .signatures = sig_similarite,
        .impl       = fn_texte_similarite,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical      = "TexteSimilaritéPartielle",
        .canonicalAscii = "TexteSimilaritePartielle",
        .nameEN         = "TextSimilarityPartial",
        .category       = "SimilaritéTexte",
        .descFR     = "Recherche la meilleure ressemblance d'une chaîne courte "
                      "à l'intérieur d'une chaîne plus longue. La plus courte "
                      "des deux est comparée à chaque portion de même longueur "
                      "de la plus longue, et le meilleur score obtenu est "
                      "retourné (réel entre 0 et 1, 1 = portion identique "
                      "trouvée). Utile pour repérer un mot ou un nom dans une "
                      "phrase. Comparaison par caractère, insensible à la casse "
                      "et aux accents par défaut. Deux chaînes vides valent 1 ; "
                      "une seule vide vaut 0.",
        .descEN     = "Finds the best resemblance of a short string inside a "
                      "longer one. The shorter of the two is compared against "
                      "every same-length slice of the longer string, and the "
                      "best score is returned (real number between 0 and 1, "
                      "1 = an identical slice was found). Useful to spot a word "
                      "or a name within a sentence. Compared per character, "
                      "case- and accent-insensitive by default. Two empty "
                      "strings score 1; a single empty string scores 0.",
        .signatures = sig_similarite_partielle,
        .impl       = fn_texte_similarite_partielle,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical      = "TexteSimilaritéJetonsTriés",
        .canonicalAscii = "TexteSimilariteJetonsTries",
        .nameEN         = "TextSimilaritySortedTokens",
        .category       = "SimilaritéTexte",
        .descFR     = "Ressemblance entre deux chaînes en faisant abstraction "
                      "de l'ordre des mots. Les mots de chaque chaîne sont "
                      "triés par ordre alphabétique puis comparés : deux "
                      "phrases composées des mêmes mots dans un ordre différent "
                      "obtiennent un score de 1. Découpe sur les espaces, "
                      "comparaison par caractère, insensible à la casse et aux "
                      "accents par défaut. Retourne un réel entre 0 et 1. Deux "
                      "chaînes vides valent 1 ; une seule vide vaut 0.",
        .descEN     = "Resemblance between two strings that ignores word order. "
                      "The words of each string are sorted alphabetically and "
                      "then compared: two sentences made of the same words in a "
                      "different order score 1. Split on spaces, compared per "
                      "character, case- and accent-insensitive by default. "
                      "Returns a real number between 0 and 1. Two empty strings "
                      "score 1; a single empty string scores 0.",
        .signatures = sig_similarite_jetons_tries,
        .impl       = fn_texte_similarite_jetons_tries,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical      = "TexteSimilaritéJetonsEnsemble",
        .canonicalAscii = "TexteSimilariteJetonsEnsemble",
        .nameEN         = "TextSimilaritySetTokens",
        .category       = "SimilaritéTexte",
        .descFR     = "Ressemblance entre deux chaînes robuste aux mots en "
                      "trop. Les mots communs aux deux chaînes sont isolés des "
                      "mots propres à chacune : lorsque tous les mots de l'une "
                      "se retrouvent dans l'autre, le score vaut 1, même si la "
                      "seconde contient des mots supplémentaires. Utile pour "
                      "rapprocher des noms ou adresses partiellement renseignés. "
                      "Découpe sur les espaces, comparaison par caractère, "
                      "insensible à la casse et aux accents par défaut. Retourne "
                      "un réel entre 0 et 1. Deux chaînes vides valent 1 ; une "
                      "seule vide vaut 0.",
        .descEN     = "Resemblance between two strings that is robust to extra "
                      "words. The words shared by both strings are separated "
                      "from the words specific to each: when every word of one "
                      "string is found in the other, the score is 1, even if the "
                      "second contains additional words. Useful to match "
                      "partially filled names or addresses. Split on spaces, "
                      "compared per character, case- and accent-insensitive by "
                      "default. Returns a real number between 0 and 1. Two empty "
                      "strings score 1; a single empty string scores 0.",
        .signatures = sig_similarite_jetons_ensemble,
        .impl       = fn_texte_similarite_jetons_ensemble,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical      = "TexteSimilaritéJaroWinkler",
        .canonicalAscii = "TexteSimilariteJaroWinkler",
        .nameEN         = "TextSimilarityJaroWinkler",
        .category       = "SimilaritéTexte",
        .descFR     = "Ressemblance entre deux chaînes adaptée au rapprochement "
                      "de noms propres. Le score, réel entre 0 et 1, augmente "
                      "lorsque les chaînes partagent un même début : les chaînes "
                      "ayant les mêmes premiers caractères obtiennent un score "
                      "plus élevé (préfixe pris en compte jusqu'à quatre "
                      "caractères). 1 signifie chaînes identiques, 0 aucune "
                      "correspondance. Comparaison par caractère, insensible à la "
                      "casse et aux accents par défaut. Deux chaînes vides valent "
                      "1 ; une seule vide vaut 0.",
        .descEN     = "Resemblance between two strings tailored to matching "
                      "proper names. The score, a real number between 0 and 1, "
                      "rises when the strings share a common beginning: strings "
                      "with the same leading characters get a higher score "
                      "(prefix counted up to four characters). 1 means identical "
                      "strings, 0 means no match. Compared per character, case- "
                      "and accent-insensitive by default. Two empty strings score "
                      "1; a single empty string scores 0.",
        .signatures = sig_similarite_jaro_winkler,
        .impl       = fn_texte_similarite_jaro_winkler,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical      = "TexteSimilaritéDice",
        .canonicalAscii = "TexteSimilariteDice",
        .nameEN         = "TextSimilarityDice",
        .category       = "SimilaritéTexte",
        .descFR     = "Ressemblance entre deux chaînes fondée sur les groupes "
                      "de trois caractères qu'elles ont en commun. Chaque chaîne "
                      "est découpée en groupes de trois caractères glissants, et "
                      "le score privilégie les groupes partagés : réel entre 0 "
                      "et 1, 1 lorsque les chaînes sont identiques, 0 lorsqu'elles "
                      "ne partagent aucun groupe. Tolérant aux fautes à "
                      "l'intérieur d'un mot, adapté au rapprochement d'adresses ou "
                      "de libellés. Comparaison par caractère, insensible à la "
                      "casse et aux accents par défaut. Deux chaînes vides valent "
                      "1 ; une seule vide vaut 0.",
        .descEN     = "Resemblance between two strings based on the groups of "
                      "three characters they share. Each string is split into "
                      "sliding three-character groups, and the score favours the "
                      "shared groups: a real number between 0 and 1, 1 when the "
                      "strings are identical, 0 when they share no group. Tolerant "
                      "of typos inside a word, suited to matching addresses or "
                      "labels. Compared per character, case- and accent-"
                      "insensitive by default. Two empty strings score 1; a "
                      "single empty string scores 0.",
        .signatures = sig_similarite_dice,
        .impl       = fn_texte_similarite_dice,
        .flags      = OWL_NAT_PURE,
    },
    {
        .canonical      = "TexteSimilaritéJaccard",
        .canonicalAscii = "TexteSimilariteJaccard",
        .nameEN         = "TextSimilarityJaccard",
        .category       = "SimilaritéTexte",
        .descFR     = "Ressemblance entre deux chaînes fondée sur le recouvrement "
                      "des groupes de trois caractères qu'elles produisent. Le "
                      "score rapporte le nombre de groupes communs au nombre total "
                      "de groupes distincts des deux chaînes : réel entre 0 et 1, "
                      "1 lorsque les chaînes sont identiques, 0 lorsqu'elles ne "
                      "partagent aucun groupe. Tolérant aux fautes à l'intérieur "
                      "d'un mot, adapté au rapprochement d'adresses ou de "
                      "libellés. Comparaison par caractère, insensible à la casse "
                      "et aux accents par défaut. Deux chaînes vides valent 1 ; "
                      "une seule vide vaut 0.",
        .descEN     = "Resemblance between two strings based on the overlap of "
                      "the groups of three characters they produce. The score is "
                      "the number of shared groups over the total number of "
                      "distinct groups of both strings: a real number between 0 "
                      "and 1, 1 when the strings are identical, 0 when they share "
                      "no group. Tolerant of typos inside a word, suited to "
                      "matching addresses or labels. Compared per character, case- "
                      "and accent-insensitive by default. Two empty strings score "
                      "1; a single empty string scores 0.",
        .signatures = sig_similarite_jaccard,
        .impl       = fn_texte_similarite_jaccard,
        .flags      = OWL_NAT_PURE,
    },
};

void stdlib_similarity_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
}

/* ================================================================
 * Self-test C — `owl --self-test-similarity`.
 *
 * Couvre les vecteurs de référence Levenshtein, la normalisation par
 * défaut (casse + accents), et — point crucial — PROUVE le comptage
 * codepoint-level sur un cas que le folding ne masque PAS.
 *
 * Subtilité : pour les chaînes latines, le repli accent ramène tout en
 * ASCII 1 octet, donc "café" vs "cafe" passe par un chemin où octet et
 * codepoint coïncident après normalisation — la preuve codepoint-level
 * y est indirecte. On l'exerce donc sur des idéogrammes CJK (3 octets
 * UTF-8 chacun, NON repliables) : "猫A" vs "狗A" diffèrent d'un seul
 * codepoint (猫→狗). En codepoint-level la distance vaut 1 ; en
 * byte-level elle vaudrait 3 (les deux idéogrammes ne partagent aucun
 * octet). On teste aussi la distance brute pré-fold directement.
 * Retourne 0 si tous les tests passent, sinon le nombre d'échecs. */

static int g_sim_pass = 0;
static int g_sim_fail = 0;

#define SIM_CHECK(expr, label) do { \
    if (expr) { g_sim_pass++; } \
    else { g_sim_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* Distance Levenshtein directe sur deux chaînes UTF-8, avec ou sans fold,
 * sans passer par le registry/VM (pour exercer le décodeur brut). */
static int sim_distance_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    int d = levenshtein_cp(ca, la, cb, lb);
    free(ca);
    free(cb);
    return d;
}

/* Distance Damerau OSA directe (idem, hors VM). */
static int sim_damerau_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    int d = damerau_osa_cp(ca, la, cb, lb);
    free(ca);
    free(cb);
    return d;
}

/* Distance Hamming directe (idem, hors VM). Retourne -1 si les longueurs
 * (en codepoints, après fold éventuel) diffèrent — l'équivalent hors-VM du
 * soft-error que pose la native. */
static int sim_hamming_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    int d = -1;
    if (la == lb) {
        d = 0;
        for (int i = 0; i < la; i++) if (ca[i] != cb[i]) d++;
    }
    free(ca);
    free(cb);
    return d;
}

/* Ratio de similarité Indel direct sur deux chaînes UTF-8 (hors VM). */
static double sim_ratio_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    double r = similarity_ratio_cp(ca, la, cb, lb);
    free(ca);
    free(cb);
    return r;
}

/* Similarité partielle directe (sliding window, hors VM). */
static double sim_partial_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);

    const uint32_t* shortS; int sLen;
    const uint32_t* longS;  int lLen;
    if (la <= lb) { shortS = ca; sLen = la; longS = cb; lLen = lb; }
    else          { shortS = cb; sLen = lb; longS = ca; lLen = la; }

    double best;
    if (sLen == 0) {
        best = (lLen == 0) ? 1.0 : 0.0;
    } else if (sLen == lLen) {
        best = similarity_ratio_cp(shortS, sLen, longS, lLen);
    } else {
        best = 0.0;
        for (int start = 0; start + sLen <= lLen; start++) {
            double r = similarity_ratio_cp(shortS, sLen, longS + start, sLen);
            if (r > best) best = r;
        }
    }
    free(ca);
    free(cb);
    return best;
}

/* Similarité jetons triés directe (hors VM). */
static double sim_sorted_tokens_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    double r = sim_sorted_tokens_ratio(ca, la, cb, lb);
    free(ca);
    free(cb);
    return r;
}

/* Similarité jetons ensemble directe (hors VM). */
static double sim_set_tokens_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    double r = sim_set_tokens_ratio(ca, la, cb, lb);
    free(ca);
    free(cb);
    return r;
}

/* Similarité Jaro-Winkler directe (hors VM). */
static double sim_jaro_winkler_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    double r = jaro_winkler_cp(ca, la, cb, lb);
    free(ca);
    free(cb);
    return r;
}

/* Jaro nu direct (hors VM) — pour vérifier le boost de préfixe isolément. */
static double sim_jaro_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    double r = jaro_cp(ca, la, cb, lb);
    free(ca);
    free(cb);
    return r;
}

/* Similarité Dice (trigrammes) directe (hors VM). */
static double sim_dice_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    double r = ngram_dice_cp(ca, la, cb, lb);
    free(ca);
    free(cb);
    return r;
}

/* Similarité Jaccard (trigrammes) directe (hors VM). */
static double sim_jaccard_raw(const char* a, const char* b, bool fold) {
    int la = 0, lb = 0;
    uint32_t* ca = similarity_decode(a, (int)strlen(a), fold, &la);
    uint32_t* cb = similarity_decode(b, (int)strlen(b), fold, &lb);
    double r = ngram_jaccard_cp(ca, la, cb, lb);
    free(ca);
    free(cb);
    return r;
}

/* Comparaison réel avec tolérance pour les vecteurs ratio/partielle. */
static bool sim_close(double a, double b) {
    double d = a - b;
    if (d < 0) d = -d;
    return d <= 0.0001;
}

int similarity_self_test_run(void) {
    g_sim_pass = 0;
    g_sim_fail = 0;

    /* --- Vecteurs de référence Levenshtein (folding sans effet ici) --- */
    SIM_CHECK(sim_distance_raw("kitten", "sitting", true) == 3,
              "kitten -> sitting = 3");
    SIM_CHECK(sim_distance_raw("Saturday", "Sunday", true) == 3,
              "Saturday -> Sunday = 3");
    SIM_CHECK(sim_distance_raw("", "", true) == 0, "vide vs vide = 0");
    SIM_CHECK(sim_distance_raw("abc", "", true) == 3, "abc vs vide = 3");
    SIM_CHECK(sim_distance_raw("", "xyz", true) == 3, "vide vs xyz = 3");
    SIM_CHECK(sim_distance_raw("bonjour", "bonjour", true) == 0, "identiques = 0");

    /* --- Normalisation par défaut : casse + accents --- */
    SIM_CHECK(sim_distance_raw("Marie", "marie", true) == 0, "casse ignorée");
    SIM_CHECK(sim_distance_raw("café", "cafe", true) == 0, "accent ignoré");
    SIM_CHECK(sim_distance_raw("Café", "CAFE", true) == 0, "casse + accent ignorés");
    SIM_CHECK(sim_distance_raw("éléphant", "elephant", true) == 0,
              "accents FR multiples foldés");
    /* Umlaut DE replié vers la lettre de base (suppression diacritique). */
    SIM_CHECK(sim_distance_raw("Müller", "Muller", true) == 0,
              "umlaut DE replié vers u");

    /* --- PREUVE codepoint-level (cas NON masqué par le folding) --- */
    /* "café" vs "cafü" : sans fold, é et ü sont 2 codepoints distincts
     * (2 octets chacun). La substitution é->ü compte 1 (codepoint), pas 2
     * (octets). */
    SIM_CHECK(sim_distance_raw("café", "cafü", false) == 1,
              "café/cafü sans fold = 1 (codepoint, pas 2 octets)");

    /* CJK : deux idéogrammes 3 octets, non repliables vers ASCII. "猫A" et
     * "狗A" diffèrent d'un seul codepoint. Codepoint-level => 1. Byte-level
     * donnerait 3 (les deux glyphes ne partagent aucun octet). On teste AVEC
     * fold (true) pour prouver que même sur le chemin public — qui passe
     * toujours par le fold — les caractères non repliables restent comptés
     * comme une unité codepoint. */
    SIM_CHECK(sim_distance_raw("\xE7\x8C\xAB" "A", "\xE7\x8B\x97" "A", true) == 1,
              "CJK 猫A/狗A = 1 codepoint diff (preuve non-byte, public path)");

    /* Sanity : le décodeur compte bien 2 codepoints pour "猫A" (et non 4
     * octets). */
    {
        int n = 0;
        uint32_t* cps = similarity_decode("\xE7\x8C\xAB" "A", 4, false, &n);
        SIM_CHECK(n == 2, "décodage \"猫A\" = 2 codepoints (4 octets)");
        free(cps);
    }

    /* --- Symétrie + caractères ordinaires --- */
    SIM_CHECK(sim_distance_raw("abc", "def", true) == sim_distance_raw("def", "abc", true),
              "distance symétrique");
    SIM_CHECK(sim_distance_raw("a b", "ab", true) == 1, "espace = caractère");
    SIM_CHECK(sim_distance_raw("test123", "test456", true) == 3,
              "chiffres = caractères ordinaires");

    /* --- Damerau OSA : transposition de 2 caractères adjacents = coût 1 --- */
    SIM_CHECK(sim_damerau_raw("ca", "ac", true) == 1,
              "Damerau ca/ac = 1 (transposition, Levenshtein donnerait 2)");
    SIM_CHECK(sim_distance_raw("ca", "ac", true) == 2,
              "contrôle : Levenshtein ca/ac = 2 substitutions");
    SIM_CHECK(sim_damerau_raw("Café", "cafe", true) == 0,
              "Damerau casse + accents foldés = 0");
    SIM_CHECK(sim_damerau_raw("bonjour", "bonjour", true) == 0,
              "Damerau identiques = 0");
    SIM_CHECK(sim_damerau_raw("abc", "", true) == 3, "Damerau abc vs vide = 3");
    SIM_CHECK(sim_damerau_raw("", "xyz", true) == 3, "Damerau vide vs xyz = 3");
    SIM_CHECK(sim_damerau_raw("Mraie", "Marie", true) == 1,
              "Damerau faute de frappe Mraie/Marie = 1 transposition");
    SIM_CHECK(sim_damerau_raw("abc", "bac", true) == sim_damerau_raw("bac", "abc", true),
              "Damerau symétrique");

    /* --- Hamming : nb de positions différentes, longueurs égales requises --- */
    SIM_CHECK(sim_hamming_raw("karolin", "kathrin", true) == 3,
              "Hamming karolin/kathrin = 3 (vecteur canon)");
    SIM_CHECK(sim_hamming_raw("1011101", "1001001", true) == 2,
              "Hamming binaire = 2 positions");
    SIM_CHECK(sim_hamming_raw("test", "test", true) == 0, "Hamming identiques = 0");
    SIM_CHECK(sim_hamming_raw("Café", "cafe", true) == 0,
              "Hamming casse + accents foldés, longueurs égales = 0");
    SIM_CHECK(sim_hamming_raw("", "", true) == 0, "Hamming vide/vide = 0");
    SIM_CHECK(sim_hamming_raw("abc", "abcd", true) == -1,
              "Hamming longueurs différentes = non défini (erreur)");

    /* --- Similarité ratio Indel : réel 0..1 --- */
    SIM_CHECK(sim_close(sim_ratio_raw("kitten", "sitting", true), 8.0 / 13.0),
              "ratio kitten/sitting = 8/13 (LCS=4, indel=5, total=13)");
    SIM_CHECK(sim_close(sim_ratio_raw("test", "test", true), 1.0),
              "ratio identiques = 1.0");
    SIM_CHECK(sim_close(sim_ratio_raw("abc", "xyz", true), 0.0),
              "ratio disjoints = 0.0 (LCS=0)");
    SIM_CHECK(sim_close(sim_ratio_raw("Café", "cafe", true), 1.0),
              "ratio casse + accents foldés = 1.0");
    SIM_CHECK(sim_close(sim_ratio_raw("abc", "", true), 0.0),
              "ratio une vide = 0.0");
    SIM_CHECK(sim_close(sim_ratio_raw("", "", true), 1.0),
              "ratio deux vides = 1.0 (convention)");
    SIM_CHECK(sim_close(sim_ratio_raw("Saturday", "Sunday", true), 5.0 / 7.0),
              "ratio Saturday/Sunday = 5/7 (LCS=5, indel=4, total=14)");
    SIM_CHECK(sim_close(sim_ratio_raw("test", "tests", true), 8.0 / 9.0),
              "ratio ajout 1 char test/tests = 8/9");
    SIM_CHECK(sim_close(sim_ratio_raw("abc", "def", true),
                        sim_ratio_raw("def", "abc", true)),
              "ratio symétrique");

    /* --- Similarité partielle : sliding window --- */
    SIM_CHECK(sim_close(sim_partial_raw("Dupont", "Jean Dupont", true), 1.0),
              "partielle sous-chaîne exacte Dupont/Jean Dupont = 1.0");
    SIM_CHECK(sim_close(sim_partial_raw("chat", "le chat noir", true), 1.0),
              "partielle match exact sliding chat/le chat noir = 1.0");
    SIM_CHECK(sim_close(sim_partial_raw("test", "test", true), 1.0),
              "partielle identiques = 1.0 (longueurs égales)");
    SIM_CHECK(sim_close(sim_partial_raw("abc", "", true), 0.0),
              "partielle une vide = 0.0");
    SIM_CHECK(sim_close(sim_partial_raw("", "", true), 1.0),
              "partielle deux vides = 1.0");
    SIM_CHECK(sim_close(sim_partial_raw("CAFÉ", "café au lait", true), 1.0),
              "partielle casse + accents foldés, match exact = 1.0");
    /* Ordre indifférent : la plus courte glisse toujours sur la plus longue. */
    SIM_CHECK(sim_close(sim_partial_raw("Dupont", "Jean Dupont Martin", true),
                        sim_partial_raw("Jean Dupont Martin", "Dupont", true)),
              "partielle indépendante de l'ordre des arguments");
    /* Score partiel strictement entre 0 et 1 (pas de match exact). */
    {
        double p = sim_partial_raw("toto", "totalement", true);
        SIM_CHECK(p > 0.5 && p < 1.0,
                  "partielle toto/totalement : 0.5 < score < 1.0");
    }

    /* --- Similarité jetons triés : ordre des mots ignoré --- */
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("Jean Dupont", "Dupont Jean", true), 1.0),
              "tri Jean Dupont / Dupont Jean = 1.0 (ordre ignoré)");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("Jean Dupont", "Jean Dupont", true), 1.0),
              "tri identiques = 1.0");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("JEAN dupont", "Dupont Jean", true), 1.0),
              "tri casse ignorée = 1.0");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("Jean Dup\xC3\xB4nt", "Dupont Jean", true), 1.0),
              "tri accents ignorés = 1.0");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("chat", "chat", true),
                        sim_ratio_raw("chat", "chat", true)),
              "tri un seul jeton = ratio brut");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("chat chat", "chat", true), 8.0 / 13.0),
              "tri duplication non dédupliquée chat chat / chat = 8/13");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("chat!", "chat", true), 8.0 / 9.0),
              "tri ponctuation non strippée chat!/chat = 8/9");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("", "", true), 1.0),
              "tri deux vides = 1.0");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("abc", "", true), 0.0),
              "tri une vide = 0.0");
    SIM_CHECK(sim_close(sim_sorted_tokens_raw("abc def", "def abc", true),
                        sim_sorted_tokens_raw("def abc", "abc def", true)),
              "tri symétrique");

    /* --- Similarité jetons ensemble : robuste aux mots en trop --- */
    SIM_CHECK(sim_close(sim_set_tokens_raw("Jean Dupont", "Jean Dupont Martin", true), 1.0),
              "ensemble sous-ensemble Jean Dupont ⊆ Jean Dupont Martin = 1.0");
    SIM_CHECK(sim_close(sim_set_tokens_raw("Paris France", "France Paris Londres", true), 1.0),
              "ensemble Paris France ⊆ France Paris Londres = 1.0");
    SIM_CHECK(sim_close(sim_set_tokens_raw("CAF\xC3\x89 Paris", "paris caf\xC3\xA9", true), 1.0),
              "ensemble casse + accents ignorés = 1.0");
    SIM_CHECK(sim_close(sim_set_tokens_raw("chat chat chien", "chat chien chat", true), 1.0),
              "ensemble duplication absorbée = 1.0");
    SIM_CHECK(sim_close(sim_set_tokens_raw("chat!", "chat", true), 8.0 / 9.0),
              "ensemble ponctuation non strippée max ratio = 8/9");
    SIM_CHECK(sim_close(sim_set_tokens_raw("", "", true), 1.0),
              "ensemble deux vides = 1.0");
    SIM_CHECK(sim_close(sim_set_tokens_raw("abc", "", true), 0.0),
              "ensemble une vide = 0.0");
    {
        double s = sim_set_tokens_raw("Paris France", "France Paris Londres", true);
        double t = sim_sorted_tokens_raw("Paris France", "France Paris Londres", true);
        SIM_CHECK(s > t, "ensemble > triés sur mots en trop");
    }
    SIM_CHECK(sim_close(sim_set_tokens_raw("Paris France", "France Londres", true),
                        sim_set_tokens_raw("France Londres", "Paris France", true)),
              "ensemble symétrique");
    /* Bornes : tout score reste dans [0,1]. */
    {
        double v = sim_set_tokens_raw("a  b", "b a", true);
        SIM_CHECK(v >= 0.0 && v <= 1.0, "ensemble espaces multiples : bornes [0,1]");
    }

    /* --- Jaro-Winkler : préfixe commun majoré (p=0.1, L max 4) --- */
    /* Vecteur canon académique : Jaro=0.9444, préfixe "mar"=3,
     * jw = 0.9444 + 3*0.1*(1-0.9444) ≈ 0.9611. */
    SIM_CHECK(sim_close(sim_jaro_raw("MARTHA", "MARHTA", true), 0.94444),
              "Jaro MARTHA/MARHTA = 0.9444 (m=6, t=1)");
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("MARTHA", "MARHTA", true), 0.96111),
              "Jaro-Winkler MARTHA/MARHTA ≈ 0.961 (préfixe mar=3)");
    /* Identiques -> 1.0 (Jaro=1, boost ne change rien). */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("dupont", "dupont", true), 1.0),
              "Jaro-Winkler identiques = 1.0");
    /* Disjoints (aucune correspondance, fenêtre insuffisante) -> 0.0. */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("abc", "xyz", true), 0.0),
              "Jaro-Winkler disjoints = 0.0");
    /* Bornes vides : deux vides -> 1.0 ; une vide -> 0.0. */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("", "", true), 1.0),
              "Jaro-Winkler deux vides = 1.0");
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("abc", "", true), 0.0),
              "Jaro-Winkler une vide = 0.0");
    /* Casse + accents foldés par défaut : minuscules == majuscules. */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("martha", "marhta", true),
                        sim_jaro_winkler_raw("MARTHA", "MARHTA", true)),
              "Jaro-Winkler casse ignorée par défaut");
    /* Boost de préfixe : jw >= Jaro nu, strictement > quand préfixe non vide
     * et Jaro < 1. */
    {
        double j = sim_jaro_raw("DIXON", "DICKSONX", true);
        double jw = sim_jaro_winkler_raw("DIXON", "DICKSONX", true);
        SIM_CHECK(jw > j, "Jaro-Winkler boost préfixe DI : jw > Jaro nu");
        SIM_CHECK(sim_close(j, 0.76667), "Jaro DIXON/DICKSONX = 0.7667 (m=4)");
        SIM_CHECK(sim_close(jw, 0.81333), "Jaro-Winkler DIXON/DICKSONX ≈ 0.813");
    }
    /* Sans préfixe commun : pas de boost (L=0), jw == Jaro. */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("MARTHA", "XARHTA", true),
                        sim_jaro_raw("MARTHA", "XARHTA", true)),
              "Jaro-Winkler sans préfixe : aucun boost (jw == Jaro)");
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("MARTHA", "XARHTA", true), 0.82222),
              "Jaro-Winkler MARTHA/XARHTA ≈ 0.822 (m=5, t=1, L=0)");
    /* Préfixe plafonné à 4 : "préfixe"/"préfixer" (foldés prefixe/prefixer). */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("préfixe", "préfixer", true), 0.975),
              "Jaro-Winkler préfixe plafonné à 4 ≈ 0.975");
    /* Transposition hors fenêtre courte : "AL"/"LA", w=0 -> m=0 -> 0.0. */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("AL", "LA", true), 0.0),
              "Jaro-Winkler AL/LA = 0.0 (fenêtre courte, m=0)");
    /* Symétrie. */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("MARTHA", "MARHTA", true),
                        sim_jaro_winkler_raw("MARHTA", "MARTHA", true)),
              "Jaro-Winkler symétrique");
    /* Un seul char médian différent : "ABCDEF"/"ABXDEF", préfixe "AB"=2. */
    SIM_CHECK(sim_close(sim_jaro_winkler_raw("ABCDEF", "ABXDEF", true), 0.91111),
              "Jaro-Winkler ABCDEF/ABXDEF ≈ 0.911 (m=5, L=2)");

    /* --- Similarité n-gram Dice (trigrammes, padding sentinelle) --- */
    /* Identiques -> 1.0 (A = B). */
    SIM_CHECK(sim_close(sim_dice_raw("abc", "abc", true), 1.0),
              "Dice identiques abc/abc = 1.0");
    /* Disjoints -> 0.0 (aucun trigramme commun). */
    SIM_CHECK(sim_close(sim_dice_raw("abc", "xyz", true), 0.0),
              "Dice disjoints abc/xyz = 0.0");
    /* Recouvrement partiel : |A|=5, |B|=6, ∩=3 -> 2·3/11 = 6/11. */
    SIM_CHECK(sim_close(sim_dice_raw("abc", "abcd", true), 6.0 / 11.0),
              "Dice abc/abcd = 6/11 (∩=3, |A|=5, |B|=6)");
    /* nuit/nuits : |A|=6, |B|=7, ∩=4 -> 2·4/13 = 8/13. */
    SIM_CHECK(sim_close(sim_dice_raw("nuit", "nuits", true), 8.0 / 13.0),
              "Dice nuit/nuits = 8/13 (∩=4, |A|=6, |B|=7)");
    /* Déduplication : "aaaa" génère 'aaa' deux fois, fusionné -> A = B. */
    SIM_CHECK(sim_close(sim_dice_raw("aaa", "aaaa", true), 1.0),
              "Dice aaa/aaaa = 1.0 (trigrammes dédupliqués)");
    /* Casse + accents foldés : CAFÉ == cafe. */
    SIM_CHECK(sim_close(sim_dice_raw("CAFÉ", "cafe", true), 1.0),
              "Dice CAFÉ/cafe = 1.0 (folding)");
    /* Bornes : deux vides -> 1.0 ({⊥⊥⊥}=={⊥⊥⊥}) ; une vide -> 0.0. */
    SIM_CHECK(sim_close(sim_dice_raw("", "", true), 1.0),
              "Dice deux vides = 1.0");
    SIM_CHECK(sim_close(sim_dice_raw("abc", "", true), 0.0),
              "Dice une vide = 0.0");
    /* Symétrie. */
    SIM_CHECK(sim_close(sim_dice_raw("abc", "abcd", true),
                        sim_dice_raw("abcd", "abc", true)),
              "Dice symétrique");

    /* --- Similarité n-gram Jaccard (trigrammes, padding sentinelle) --- */
    SIM_CHECK(sim_close(sim_jaccard_raw("abc", "abc", true), 1.0),
              "Jaccard identiques abc/abc = 1.0");
    SIM_CHECK(sim_close(sim_jaccard_raw("abc", "xyz", true), 0.0),
              "Jaccard disjoints abc/xyz = 0.0");
    /* Recouvrement partiel : ∩=3, ∪=8 -> 3/8 = 0.375. */
    SIM_CHECK(sim_close(sim_jaccard_raw("abc", "abcd", true), 3.0 / 8.0),
              "Jaccard abc/abcd = 3/8 (∩=3, ∪=8)");
    /* nuit/nuits : ∩=4, ∪=9 -> 4/9. */
    SIM_CHECK(sim_close(sim_jaccard_raw("nuit", "nuits", true), 4.0 / 9.0),
              "Jaccard nuit/nuits = 4/9 (∩=4, ∪=9)");
    SIM_CHECK(sim_close(sim_jaccard_raw("aaa", "aaaa", true), 1.0),
              "Jaccard aaa/aaaa = 1.0 (trigrammes dédupliqués)");
    SIM_CHECK(sim_close(sim_jaccard_raw("CAFÉ", "cafe", true), 1.0),
              "Jaccard CAFÉ/cafe = 1.0 (folding)");
    SIM_CHECK(sim_close(sim_jaccard_raw("", "", true), 1.0),
              "Jaccard deux vides = 1.0");
    SIM_CHECK(sim_close(sim_jaccard_raw("abc", "", true), 0.0),
              "Jaccard une vide = 0.0");
    SIM_CHECK(sim_close(sim_jaccard_raw("abc", "abcd", true),
                        sim_jaccard_raw("abcd", "abc", true)),
              "Jaccard symétrique");
    /* Relation Dice ≥ Jaccard sur recouvrement partiel. */
    SIM_CHECK(sim_dice_raw("abc", "abcd", true) > sim_jaccard_raw("abc", "abcd", true),
              "Dice > Jaccard sur recouvrement partiel");

    fprintf(stdout, "self-test-similarity: %d cas, %d passed, %d failed\n",
            g_sim_pass + g_sim_fail, g_sim_pass, g_sim_fail);
    /* Garde ensemble vide. */
    if (g_sim_pass + g_sim_fail == 0) {
        fprintf(stdout, "self-test-similarity: CORPUS VIDE — 0 cas execute. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_sim_fail + 1;
    }
    return g_sim_fail;
}
