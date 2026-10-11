// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test des encodeurs MIME internes.
 *
 * Execute via `owl --self-test-net` (C pur, aucune VM, aucun registre).
 * Couvre :
 *   - Quoted-Printable : vecteurs canoniques (=E2=82=AC = "€"), encodage
 *     de '=' / octets de controle, espaces en fin de ligne, soft break
 *     <=76 colonnes, decode tolerant.
 *   - RFC 2047 : vecteurs canoniques (ex. =?ISO-8859-1?Q?Keld_J=F8rn_...),
 *     Q vs B, mots adjacents + repli, pur-ASCII intact.
 *   - Round-trips encode∘decode = identite (accents FR, UTF-8 multi-octets,
 *     emoji, chaine vide, ligne longue, pur-ASCII).
 *
 * Retourne le nombre d'echecs ; imprime un resume. */

#include "../mime_codec.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* Egalite chaine C (les codecs terminent toujours par \0). */
#define CHECK_STR(got, want, label) do { \
    char* _g = (got); \
    int _ok = (_g != NULL) && (strcmp(_g, (want)) == 0); \
    if (!_ok) fprintf(stderr, "    got=[%s] want=[%s]\n", _g ? _g : "(null)", (want)); \
    CHECK(_ok, label); \
    free(_g); \
} while (0)

/* Round-trip binaire : decode(encode(x)) doit rendre x octet-pour-octet. */
static int roundtrip_qp(const char* in, size_t len) {
    size_t el = 0, dl = 0;
    char* enc = qp_encode(in, len, &el);
    if (!enc) return 0;
    char* dec = qp_decode(enc, el, &dl);
    free(enc);
    if (!dec) return 0;
    int ok = (dl == len) && (memcmp(dec, in, len) == 0);
    free(dec);
    return ok;
}

static int roundtrip_2047(const char* in, size_t len) {
    size_t el = 0, dl = 0;
    char* enc = rfc2047_encode(in, len, &el);
    if (!enc) return 0;
    char* dec = rfc2047_decode(enc, el, &dl);
    free(enc);
    if (!dec) return 0;
    int ok = (dl == len) && (memcmp(dec, in, len) == 0);
    free(dec);
    return ok;
}

/* ---------------------------------------------------------------- */

static void test_qp_encode_vectors(void) {
    /* '€' = U+20AC = E2 82 AC en UTF-8 -> =E2=82=AC (hexa MAJUSCULE). */
    CHECK_STR(qp_encode("\xE2\x82\xAC", 3, NULL), "=E2=82=AC", "QP euro -> =E2=82=AC");

    /* '=' (0x3D) doit devenir =3D. */
    CHECK_STR(qp_encode("a=b", 3, NULL), "a=3Db", "QP '=' -> =3D");

    /* ASCII imprimable pur : inchange. */
    CHECK_STR(qp_encode("Hello, World!", 13, NULL), "Hello, World!", "QP ascii intact");

    /* Octet de controle (TAB 0x09) EN FIN de ligne -> encode =09. */
    CHECK_STR(qp_encode("x\t", 2, NULL), "x=09", "QP tab final encode");

    /* Espace en fin de ligne -> =20 ; espace au milieu -> litteral. */
    CHECK_STR(qp_encode("a b ", 4, NULL), "a b=20", "QP espace final encode, milieu litteral");

    /* Chaine vide. */
    CHECK_STR(qp_encode("", 0, NULL), "", "QP vide");
}

static void test_qp_softbreak(void) {
    /* 80 'a' : doit inserer un soft break "=\r\n", aucune ligne > 76 col. */
    char in[80];
    memset(in, 'a', sizeof(in));
    size_t el = 0;
    char* enc = qp_encode(in, sizeof(in), &el);
    CHECK(enc != NULL, "QP long alloc");
    if (enc) {
        int ok_len = 1;
        size_t col = 0;
        for (size_t i = 0; i < el; i++) {
            if (enc[i] == '\n') { col = 0; }
            else if (enc[i] == '\r') { /* la colonne est bornee par le \n suivant */ }
            else { col++; if (col > 76) ok_len = 0; }
        }
        CHECK(ok_len, "QP aucune ligne > 76 colonnes");
        CHECK(strstr(enc, "=\r\n") != NULL, "QP soft break present");
        /* Round-trip : on retrouve les 80 'a'. */
        size_t dl = 0;
        char* dec = qp_decode(enc, el, &dl);
        CHECK(dec && dl == sizeof(in) && memcmp(dec, in, sizeof(in)) == 0,
              "QP soft break round-trip");
        free(dec);
        free(enc);
    }
}

static void test_qp_decode_vectors(void) {
    /* =E2=82=AC -> € */
    size_t dl = 0;
    char* d = qp_decode("=E2=82=AC", 9, &dl);
    CHECK(d && dl == 3 && memcmp(d, "\xE2\x82\xAC", 3) == 0, "QP decode euro");
    free(d);

    /* Soft break "=\r\n" au milieu doit disparaitre. */
    CHECK_STR(qp_decode("ab=\r\ncd", 7, NULL), "abcd", "QP decode soft break \\r\\n");

    /* Soft break tolerant "=\n" (sans CR). */
    CHECK_STR(qp_decode("ab=\ncd", 6, NULL), "abcd", "QP decode soft break \\n tolerant");

    /* Hexa minuscule tolere en entree. */
    CHECK_STR(qp_decode("=e2=82=ac", 9, NULL), "\xE2\x82\xAC", "QP decode hexa minuscule");
}

static void test_2047_decode_vectors(void) {
    /* Vecteur canonique RFC 2047 §8 : ISO-8859-1, Q-encoding.
     * J=F8 -> ø (U+00F8 -> C3 B8 en UTF-8), '_' -> espace. */
    {
        const char* v = "=?ISO-8859-1?Q?Keld_J=F8rn_Simonsen?=";
        CHECK_STR(rfc2047_decode(v, strlen(v), NULL),
                  "Keld J\xC3\xB8rn Simonsen", "2047 decode ISO-8859-1 Q");
    }

    /* UTF-8 B-encoding : base64("€") = base64(E2 82 AC) = "4oKs". */
    CHECK_STR(rfc2047_decode("=?UTF-8?B?4oKs?=", 16, NULL),
              "\xE2\x82\xAC", "2047 decode UTF-8 B");

    /* UTF-8 Q-encoding de "café" : caf + =C3=A9. */
    CHECK_STR(rfc2047_decode("=?UTF-8?Q?caf=C3=A9?=", 21, NULL),
              "caf\xC3\xA9", "2047 decode UTF-8 Q accent");

    /* Deux mots-encodes adjacents separes par un repli : l'espace disparait. */
    CHECK_STR(rfc2047_decode("=?UTF-8?Q?a?= =?UTF-8?Q?b?=", 27, NULL),
              "ab", "2047 decode mots adjacents, repli retire");

    /* Texte non encode autour : l'espace reste. */
    CHECK_STR(rfc2047_decode("Re: =?UTF-8?Q?caf=C3=A9?=", 25, NULL),
              "Re: caf\xC3\xA9", "2047 decode mixte texte + mot");
}

static void test_2047_encode(void) {
    /* Pur-ASCII : renvoye tel quel. */
    CHECK_STR(rfc2047_encode("Hello World", 11, NULL), "Hello World", "2047 encode ascii intact");

    /* Non-ASCII : doit produire un mot-encode UTF-8, redecodable en l'original. */
    size_t el = 0;
    char* enc = rfc2047_encode("caf\xC3\xA9", 5, &el);
    CHECK(enc != NULL, "2047 encode accent alloc");
    if (enc) {
        CHECK(strncmp(enc, "=?UTF-8?", 8) == 0, "2047 encode prefixe =?UTF-8?");
        CHECK(strstr(enc, "?=") != NULL, "2047 encode suffixe ?=");
        size_t dl = 0;
        char* dec = rfc2047_decode(enc, el, &dl);
        CHECK(dec && dl == 5 && memcmp(dec, "caf\xC3\xA9", 5) == 0, "2047 encode->decode accent");
        free(dec);
        free(enc);
    }

    /* Folding : chaque mot-encode <= 75 caracteres, meme sur entree longue. */
    char longin[300];
    for (size_t i = 0; i < sizeof(longin); i += 2) { longin[i] = (char)0xC3; longin[i+1] = (char)0xA9; }
    enc = rfc2047_encode(longin, sizeof(longin), &el);
    CHECK(enc != NULL, "2047 encode long alloc");
    if (enc) {
        int ok = 1;
        size_t wlen = 0;
        for (size_t i = 0; i < el; i++) {
            if (enc[i] == '\r' || enc[i] == '\n') { wlen = 0; }
            else { wlen++; if (wlen > 75) ok = 0; }
        }
        CHECK(ok, "2047 encode mots <= 75 car");
        free(enc);
    }
}

static void test_roundtrips(void) {
    /* QP round-trips. */
    CHECK(roundtrip_qp("", 0), "QP RT vide");
    CHECK(roundtrip_qp("Hello, World!", 13), "QP RT ascii");
    CHECK(roundtrip_qp("\xC3\xA9\xC3\xA8\xC3\xA0\xC3\xA7", 8), "QP RT accents FR");
    CHECK(roundtrip_qp("\xE2\x82\xAC", 3), "QP RT euro");
    CHECK(roundtrip_qp("\xF0\x9F\x98\x80", 4), "QP RT emoji");
    {
        char big[500];
        for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)('!' + (i % 90));
        CHECK(roundtrip_qp(big, sizeof(big)), "QP RT ligne longue");
    }

    /* RFC 2047 round-trips. */
    CHECK(roundtrip_2047("", 0), "2047 RT vide");
    CHECK(roundtrip_2047("Hello World", 11), "2047 RT ascii");
    CHECK(roundtrip_2047("caf\xC3\xA9 \xC3\xA8\xC3\xA0\xC3\xA7", 11), "2047 RT accents FR");
    CHECK(roundtrip_2047("prix: \xE2\x82\xAC 10", 12), "2047 RT euro");
    CHECK(roundtrip_2047("\xF0\x9F\x98\x80 sourire", 12), "2047 RT emoji");
    {
        /* Entree longue non-ASCII : force le folding + round-trip. */
        char big[200];
        for (size_t i = 0; i < sizeof(big); i += 2) { big[i] = (char)0xC3; big[i+1] = (char)0xA9; }
        CHECK(roundtrip_2047(big, sizeof(big)), "2047 RT longue avec folding");
    }
}

int mime_codec_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_qp_encode_vectors();
    test_qp_softbreak();
    test_qp_decode_vectors();
    test_2047_decode_vectors();
    test_2047_encode();
    test_roundtrips();

    printf("mime_codec self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
