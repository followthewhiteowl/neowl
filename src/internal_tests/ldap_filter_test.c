// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test de la couche filtres LDAP (ldap_filter.{c,h}).
 *
 * Execute via `owl --self-test-net` (famille reseau pure, cf. main.c).
 * C pur, aucune VM, aucune socket.
 *
 * Deux familles de verifs :
 *   - ENCODAGE : chaines RFC 4515 comparees a des VECTEURS D'OCTETS
 *     calcules a la main, chaque octet commente par son decodage TLV
 *     (seul garde-fou contre un codec auto-confirmant).
 *   - REJET : entrees malformees (parentheses desequilibrees, filtre
 *     vide, combinateur sans operande, operateur manquant, echappement
 *     tronque, imbrication abusive) qui doivent toutes retourner false
 *     sans jamais abort / deborder.
 *
 * Plus un round-trip d'integration leger : un filtre produit est injecte
 * dans owl_ldap_encode_search_request et le SearchRequest resultant est
 * re-parse par owl_ldap_message_parse.
 *
 * Rappels TLV (RFC 4511 = BER) utilises ici :
 *   04 OCTET STRING, 30 SEQUENCE.
 *   Filter CHOICE (IMPLICIT, le tag CONTEXT remplace le tag universel) :
 *     A0 and, A1 or, A2 not, A3 equalityMatch, A4 substrings,
 *     A5 greaterOrEqual, A6 lessOrEqual, A8 approxMatch,
 *     87 present ([7] primitif), 80/81/82 initial/any/final de substring.
 */

#include "ldap_filter.h"
#include "ldap_pdu.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* Encode `filter`, compare le bloc BER produit au vecteur attendu. */
static bool filter_equals(const char* filter,
                          const unsigned char* expected, size_t n) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);
    bool ok = owl_ldap_filter_encode(&w, filter);
    bool match = ok
        && owl_ber_writer_len(&w) == n
        && (n == 0 || (owl_ber_writer_data(&w) != NULL
                       && memcmp(owl_ber_writer_data(&w), expected, n) == 0));
    owl_ber_writer_free(&w);
    return match;
}

/* `filter` doit etre rejete (false), writer laisse liberable. */
static bool filter_rejected(const char* filter) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);
    bool ok = owl_ldap_filter_encode(&w, filter);
    owl_ber_writer_free(&w);
    return !ok;
}

/* ================================================================
 * Encodage : vecteurs d'octets
 * ================================================================ */

static void test_equality(void) {
    /* (cn=bob) : equalityMatch "cn"="bob".
     *   A3 09                 [3] equalityMatch len 9
     *     04 02 63 6E         OCTET STRING "cn"    (attributeDesc)
     *     04 03 62 6F 62      OCTET STRING "bob"   (assertionValue) */
    const unsigned char expected[] = {
        0xA3, 0x09,
          0x04, 0x02, 0x63, 0x6E,
          0x04, 0x03, 0x62, 0x6F, 0x62
    };
    CHECK(filter_equals("(cn=bob)", expected, sizeof expected),
          "equality: (cn=bob) = A3 09 04 02 63 6E 04 03 62 6F 62");
}

static void test_present(void) {
    /* (cn=*) : present "cn".
     *   87 02 63 6E           [7] present (primitif) "cn" (octets bruts) */
    const unsigned char expected[] = { 0x87, 0x02, 0x63, 0x6E };
    CHECK(filter_equals("(cn=*)", expected, sizeof expected),
          "present: (cn=*) = 87 02 63 6E");

    /* (mail=*) : present "mail".
     *   87 04 6D 61 69 6C     [7] present "mail" */
    const unsigned char exp2[] = { 0x87, 0x04, 0x6D, 0x61, 0x69, 0x6C };
    CHECK(filter_equals("(mail=*)", exp2, sizeof exp2),
          "present: (mail=*) = 87 04 6D 61 69 6C");
}

static void test_and(void) {
    /* (&(a=1)(b=2)) : and SET de 2 equalityMatch, ordre source preserve.
     *   A0 10                     [0] and len 16
     *     A3 06                   equalityMatch "a"="1"
     *       04 01 61 04 01 31
     *     A3 06                   equalityMatch "b"="2"
     *       04 01 62 04 01 32 */
    const unsigned char expected[] = {
        0xA0, 0x10,
          0xA3, 0x06, 0x04, 0x01, 0x61, 0x04, 0x01, 0x31,
          0xA3, 0x06, 0x04, 0x01, 0x62, 0x04, 0x01, 0x32
    };
    CHECK(filter_equals("(&(a=1)(b=2))", expected, sizeof expected),
          "and: (&(a=1)(b=2)) = A0 10 ...");
}

static void test_or(void) {
    /* (|(a=1)(b=2)) : or, meme structure que and mais tag A1.
     *   A1 10
     *     A3 06 04 01 61 04 01 31
     *     A3 06 04 01 62 04 01 32 */
    const unsigned char expected[] = {
        0xA1, 0x10,
          0xA3, 0x06, 0x04, 0x01, 0x61, 0x04, 0x01, 0x31,
          0xA3, 0x06, 0x04, 0x01, 0x62, 0x04, 0x01, 0x32
    };
    CHECK(filter_equals("(|(a=1)(b=2))", expected, sizeof expected),
          "or: (|(a=1)(b=2)) = A1 10 ...");
}

static void test_not(void) {
    /* (!(cn=x)) : not { equalityMatch "cn"="x" }.
     *   A2 09                     [2] not len 9
     *     A3 07                   equalityMatch
     *       04 02 63 6E 04 01 78  "cn"="x" */
    const unsigned char expected[] = {
        0xA2, 0x09,
          0xA3, 0x07,
            0x04, 0x02, 0x63, 0x6E, 0x04, 0x01, 0x78
    };
    CHECK(filter_equals("(!(cn=x))", expected, sizeof expected),
          "not: (!(cn=x)) = A2 09 A3 07 ...");
}

static void test_substrings(void) {
    /* (cn=a*b) : substrings { type "cn", initial "a", final "b" }.
     *   A4 0C                     [4] substrings len 12
     *     04 02 63 6E             OCTET STRING "cn"     (type)
     *     30 06                   SEQUENCE OF
     *       80 01 61              [0] initial "a"
     *       82 01 62              [2] final "b" */
    const unsigned char exp_ab[] = {
        0xA4, 0x0C,
          0x04, 0x02, 0x63, 0x6E,
          0x30, 0x06,
            0x80, 0x01, 0x61,
            0x82, 0x01, 0x62
    };
    CHECK(filter_equals("(cn=a*b)", exp_ab, sizeof exp_ab),
          "substr: (cn=a*b) = A4 0C ... 80 01 61 82 01 62");

    /* (cn=a*b*c) : initial "a", any "b", final "c" (ordre source).
     *   A4 0F
     *     04 02 63 6E             "cn"
     *     30 09
     *       80 01 61              [0] initial "a"
     *       81 01 62              [1] any "b"
     *       82 01 63              [2] final "c" */
    const unsigned char exp_abc[] = {
        0xA4, 0x0F,
          0x04, 0x02, 0x63, 0x6E,
          0x30, 0x09,
            0x80, 0x01, 0x61,
            0x81, 0x01, 0x62,
            0x82, 0x01, 0x63
    };
    CHECK(filter_equals("(cn=a*b*c)", exp_abc, sizeof exp_abc),
          "substr: (cn=a*b*c) = A4 0F ... 80..61 81..62 82..63");

    /* (cn=a*) : initial seul (le segment final vide ne produit rien).
     *   A4 09
     *     04 02 63 6E
     *     30 03
     *       80 01 61              [0] initial "a" */
    const unsigned char exp_a[] = {
        0xA4, 0x09,
          0x04, 0x02, 0x63, 0x6E,
          0x30, 0x03,
            0x80, 0x01, 0x61
    };
    CHECK(filter_equals("(cn=a*)", exp_a, sizeof exp_a),
          "substr: (cn=a*) = A4 09 ... 80 01 61 (initial seul)");

    /* (cn=*bob*) : any seul (segments initial ET final vides).
     *   A4 0B
     *     04 02 63 6E
     *     30 05
     *       81 03 62 6F 62        [1] any "bob" */
    const unsigned char exp_bob[] = {
        0xA4, 0x0B,
          0x04, 0x02, 0x63, 0x6E,
          0x30, 0x05,
            0x81, 0x03, 0x62, 0x6F, 0x62
    };
    CHECK(filter_equals("(cn=*bob*)", exp_bob, sizeof exp_bob),
          "substr: (cn=*bob*) = A4 0B ... 81 03 62 6F 62 (any seul)");
}

static void test_comparison_ops(void) {
    /* (n>=3) : greaterOrEqual "n"="3".
     *   A5 06 04 01 6E 04 01 33 */
    const unsigned char ge[] = {
        0xA5, 0x06, 0x04, 0x01, 0x6E, 0x04, 0x01, 0x33
    };
    CHECK(filter_equals("(n>=3)", ge, sizeof ge),
          "ge: (n>=3) = A5 06 04 01 6E 04 01 33");

    /* (n<=3) : lessOrEqual "n"="3".
     *   A6 06 04 01 6E 04 01 33 */
    const unsigned char le[] = {
        0xA6, 0x06, 0x04, 0x01, 0x6E, 0x04, 0x01, 0x33
    };
    CHECK(filter_equals("(n<=3)", le, sizeof le),
          "le: (n<=3) = A6 06 04 01 6E 04 01 33");

    /* (sn~=kurd) : approxMatch "sn"="kurd".
     *   A8 0A
     *     04 02 73 6E            "sn"
     *     04 04 6B 75 72 64      "kurd" */
    const unsigned char approx[] = {
        0xA8, 0x0A,
          0x04, 0x02, 0x73, 0x6E,
          0x04, 0x04, 0x6B, 0x75, 0x72, 0x64
    };
    CHECK(filter_equals("(sn~=kurd)", approx, sizeof approx),
          "approx: (sn~=kurd) = A8 0A ...");
}

static void test_escaping(void) {
    /* (cn=x\2ay) : la valeur contient une ETOILE LITTERALE (echappee).
     * Donc egalite "cn"="x*y" (= 78 2A 79), PAS un substring, PAS present.
     *   A3 09
     *     04 02 63 6E           "cn"
     *     04 03 78 2A 79        "x*y" (l'octet 0x2A est la valeur, pas un joker) */
    const unsigned char expected[] = {
        0xA3, 0x09,
          0x04, 0x02, 0x63, 0x6E,
          0x04, 0x03, 0x78, 0x2A, 0x79
    };
    CHECK(filter_equals("(cn=x\\2ay)", expected, sizeof expected),
          "escape: (cn=x\\2ay) = egalite avec '*' litteral");

    /* Echappements des chars reserves : \28 '(' \29 ')' \5c '\'.
     * (o=a\28b\29c) -> egalite "o"="a(b)c" = 61 28 62 29 63.
     *   A3 0A
     *     04 01 6F              "o"
     *     04 05 61 28 62 29 63  "a(b)c" */
    const unsigned char parens[] = {
        0xA3, 0x0A,
          0x04, 0x01, 0x6F,
          0x04, 0x05, 0x61, 0x28, 0x62, 0x29, 0x63
    };
    CHECK(filter_equals("(o=a\\28b\\29c)", parens, sizeof parens),
          "escape: (o=a\\28b\\29c) = egalite \"a(b)c\"");

    /* Hex insensible a la casse : \5C == \5c == '\'.
     * (p=a\5Cb) -> "p"="a\\b" = 61 5C 62.
     *   A3 08 04 01 70 04 03 61 5C 62 */
    const unsigned char backslash[] = {
        0xA3, 0x08,
          0x04, 0x01, 0x70,
          0x04, 0x03, 0x61, 0x5C, 0x62
    };
    CHECK(filter_equals("(p=a\\5Cb)", backslash, sizeof backslash),
          "escape: (p=a\\5Cb) hex majuscule = '\\'");
}

static void test_nested(void) {
    /* (&(objectClass=user)(!(cn=andy))) : exemple realiste.
     *   A0 LL
     *     A3 13                              equalityMatch objectClass=user
     *       04 0B objectClass  (6F 62 6A 65 63 74 43 6C 61 73 73)
     *       04 04 user         (75 73 65 72)
     *     A2 0C                              not
     *       A3 0A                            equalityMatch cn=andy
     *         04 02 63 6E
     *         04 04 61 6E 64 79 */
    const unsigned char expected[] = {
        0xA0, 0x23,
          0xA3, 0x13,
            0x04, 0x0B, 0x6F, 0x62, 0x6A, 0x65, 0x63, 0x74,
                        0x43, 0x6C, 0x61, 0x73, 0x73,
            0x04, 0x04, 0x75, 0x73, 0x65, 0x72,
          0xA2, 0x0C,
            0xA3, 0x0A,
              0x04, 0x02, 0x63, 0x6E,
              0x04, 0x04, 0x61, 0x6E, 0x64, 0x79
    };
    CHECK(filter_equals("(&(objectClass=user)(!(cn=andy)))",
                        expected, sizeof expected),
          "nested: (&(objectClass=user)(!(cn=andy)))");
}

/* ================================================================
 * Rejet : entrees malformees
 * ================================================================ */

static void test_rejects(void) {
    CHECK(filter_rejected(""),            "rej: chaine vide");
    CHECK(filter_rejected("(cn=bob"),     "rej: parenthese fermante manquante");
    CHECK(filter_rejected("cn=bob)"),     "rej: parenthese ouvrante manquante");
    CHECK(filter_rejected("(cn=bob))"),   "rej: parenthese surnumeraire");
    CHECK(filter_rejected("(cn=x)zz"),    "rej: reliquat apres le filtre");
    CHECK(filter_rejected("()"),          "rej: filtre vide");
    CHECK(filter_rejected("(&)"),         "rej: and sans operande");
    CHECK(filter_rejected("(|)"),         "rej: or sans operande");
    CHECK(filter_rejected("(!)"),         "rej: not sans operande");
    CHECK(filter_rejected("(!(a=1)(b=2))"), "rej: not a deux operandes");
    CHECK(filter_rejected("(cn)"),        "rej: operateur manquant");
    CHECK(filter_rejected("(=bob)"),      "rej: attribut manquant");
    CHECK(filter_rejected("(cn~bob)"),    "rej: ~ sans = (operateur incomplet)");
    CHECK(filter_rejected("(cn=a\\2)"),   "rej: echappement tronque (1 hex)");
    CHECK(filter_rejected("(cn=a\\zz)"),  "rej: echappement non hexa");
    CHECK(filter_rejected("(cn=a\\)"),    "rej: backslash en fin de valeur");
    CHECK(filter_rejected("(cn=a(b)"),    "rej: parenthese non echappee dans la valeur");
    CHECK(filter_rejected("(cn>=a*b)"),   "rej: joker non echappe sur >=");
    CHECK(filter_rejected("(cn=**)"),     "rej: substring tout-etoiles (0 composant)");

    /* Imbrication au-dela de la profondeur max : 40 not emboites.
     * Construit "((((...(cn=x)...))))" avec 40 '(!' + '(cn=x)' + 40 ')'. */
    {
        char buf[256];
        size_t i = 0;
        for (int k = 0; k < 40; k++) { buf[i++] = '('; buf[i++] = '!'; }
        const char* leaf = "(cn=x)";
        memcpy(buf + i, leaf, strlen(leaf)); i += strlen(leaf);
        for (int k = 0; k < 40; k++) buf[i++] = ')';
        buf[i] = '\0';
        CHECK(filter_rejected(buf), "rej: imbrication > profondeur max");
    }
}

/* ================================================================
 * Round-trip d'integration : filtre -> SearchRequest -> re-parse
 * ================================================================ */

static void test_integration_search_request(void) {
    /* Produit le filtre (cn=bob), l'injecte comme bloc opaque dans un
     * SearchRequest, puis re-parse l'enveloppe LDAPMessage. */
    OwlBerWriter f;
    owl_ber_writer_init(&f);
    bool fok = owl_ldap_filter_encode(&f, "(cn=bob)");
    CHECK(fok, "integ: filtre (cn=bob) encode");

    OwlBerWriter req;
    owl_ber_writer_init(&req);
    const char* attrs[] = { "cn", "mail" };
    bool rok = fok && owl_ldap_encode_search_request(
        &req, 7, "dc=example,dc=com", OWL_LDAP_SCOPE_WHOLE_SUBTREE,
        owl_ber_writer_data(&f), owl_ber_writer_len(&f), attrs, 2);
    CHECK(rok, "integ: SearchRequest accepte le filtre opaque");

    OwlLdapMessageHeader hdr;
    bool pok = rok && owl_ldap_message_parse(owl_ber_writer_data(&req),
                                             owl_ber_writer_len(&req), &hdr);
    CHECK(pok && hdr.messageID == 7
             && hdr.protocolOpTag == OWL_LDAP_OP_SEARCH_REQUEST,
          "integ: enveloppe relue (id=7, tag=0x63 SearchRequest)");

    owl_ber_writer_free(&req);
    owl_ber_writer_free(&f);

    /* Un filtre malforme ne doit RIEN produire d'exploitable : l'encode
     * echoue, donc aucun SearchRequest n'est tente avec. */
    OwlBerWriter bad;
    owl_ber_writer_init(&bad);
    CHECK(!owl_ldap_filter_encode(&bad, "(&)"),
          "integ: filtre malforme rejete avant injection");
    owl_ber_writer_free(&bad);
}

int ldap_filter_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_equality();
    test_present();
    test_and();
    test_or();
    test_not();
    test_substrings();
    test_comparison_ops();
    test_escaping();
    test_nested();
    test_rejects();
    test_integration_search_request();

    printf("ldap_filter self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
