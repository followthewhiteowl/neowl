// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test de la couche BER LDAP (ldap_ber.{c,h}).
 *
 * Execute via `owl --self-test-net` (famille reseau pure, cf. main.c).
 * C pur, aucune VM, aucune socket.
 *
 * Deux familles de verifs :
 *   - round-trip encode -> decode des primitives et d'un LDAPMessage ;
 *   - VECTEURS D'OCTETS calcules a la main (chaque octet est commente par
 *     son decodage TLV) : seul garde-fou contre un codec auto-confirmant.
 *
 * Rappels TLV (RFC 4511 = BER, tag/longueur/valeur) utilises ici :
 *   02 = INTEGER, 01 = BOOLEAN, 0A = ENUMERATED, 04 = OCTET STRING,
 *   30 = SEQUENCE (0x10 | constructed 0x20).
 *   Tag de classe = bits 8-7 : UNIVERSAL 0x00, APPLICATION 0x40,
 *   CONTEXT 0x80 ; bit constructed = 0x20.
 */

#include "ldap_ber.h"

#include <mbedtls/asn1.h>

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* Compare le contenu ecrit du writer a un vecteur d'octets attendu. */
static bool writer_equals(const OwlBerWriter* w,
                          const unsigned char* expected, size_t n) {
    if (owl_ber_writer_len(w) != n) return false;
    const unsigned char* got = owl_ber_writer_data(w);
    if (got == NULL) return n == 0;
    return memcmp(got, expected, n) == 0;
}

/* ================================================================
 * Vecteurs d'octets : primitives UNIVERSAL
 * ================================================================ */

static void test_vectors_primitives(void) {
    OwlBerWriter w;

    /* INTEGER 1 -> 02 01 01
     *   02 = tag INTEGER, 01 = longueur 1 octet, 01 = valeur 1 */
    owl_ber_writer_init(&w);
    CHECK(owl_ber_write_integer(&w, 1), "write INTEGER 1 ok");
    CHECK(writer_equals(&w, (const unsigned char[]){0x02, 0x01, 0x01}, 3),
          "vecteur INTEGER 1 = 02 01 01");
    owl_ber_writer_free(&w);

    /* INTEGER 0 -> 02 01 00 */
    owl_ber_writer_init(&w);
    CHECK(owl_ber_write_integer(&w, 0), "write INTEGER 0 ok");
    CHECK(writer_equals(&w, (const unsigned char[]){0x02, 0x01, 0x00}, 3),
          "vecteur INTEGER 0 = 02 01 00");
    owl_ber_writer_free(&w);

    /* BOOLEAN vrai -> 01 01 FF
     *   01 = tag BOOLEAN, 01 = longueur, FF = vrai (BER : tout non-nul,
     *   mbedtls emet FF) */
    owl_ber_writer_init(&w);
    CHECK(owl_ber_write_boolean(&w, true), "write BOOLEAN true ok");
    CHECK(writer_equals(&w, (const unsigned char[]){0x01, 0x01, 0xFF}, 3),
          "vecteur BOOLEAN true = 01 01 FF");
    owl_ber_writer_free(&w);

    /* BOOLEAN faux -> 01 01 00 */
    owl_ber_writer_init(&w);
    CHECK(owl_ber_write_boolean(&w, false), "write BOOLEAN false ok");
    CHECK(writer_equals(&w, (const unsigned char[]){0x01, 0x01, 0x00}, 3),
          "vecteur BOOLEAN false = 01 01 00");
    owl_ber_writer_free(&w);

    /* ENUMERATED 2 -> 0A 01 02 (ex. resultCode LDAP) */
    owl_ber_writer_init(&w);
    CHECK(owl_ber_write_enumerated(&w, 2), "write ENUMERATED 2 ok");
    CHECK(writer_equals(&w, (const unsigned char[]){0x0A, 0x01, 0x02}, 3),
          "vecteur ENUMERATED 2 = 0A 01 02");
    owl_ber_writer_free(&w);

    /* OCTET STRING "ou" -> 04 02 6F 75
     *   04 = tag OCTET STRING, 02 = longueur, 6F 75 = 'o' 'u' (UTF-8) */
    owl_ber_writer_init(&w);
    CHECK(owl_ber_write_octet_string(&w, (const unsigned char*)"ou", 2),
          "write OCTET STRING ou ok");
    CHECK(writer_equals(&w, (const unsigned char[]){0x04, 0x02, 0x6F, 0x75}, 4),
          "vecteur OCTET STRING ou = 04 02 6F 75");
    owl_ber_writer_free(&w);
}

/* ================================================================
 * UTF-8 brut : aucun re-encodage d'une LDAPString/LDAPDN
 * ================================================================ */

static void test_octet_string_utf8_raw(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* "cafe" avec e accent aigu = 63 61 66 C3 A9 en UTF-8 (5 octets).
     * On attend le TLV 04 05 63 61 66 C3 A9 : les octets UTF-8 passent
     * tels quels, la longueur compte les OCTETS (5), pas les caracteres. */
    const unsigned char utf8[] = {0x63, 0x61, 0x66, 0xC3, 0xA9};
    CHECK(owl_ber_write_octet_string(&w, utf8, sizeof utf8),
          "write OCTET STRING utf8 ok");
    const unsigned char expected[] = {0x04, 0x05, 0x63, 0x61, 0x66, 0xC3, 0xA9};
    CHECK(writer_equals(&w, expected, sizeof expected),
          "vecteur OCTET STRING utf8 brut = 04 05 63 61 66 C3 A9");

    /* Relecture : les 5 octets de contenu sont rendus a l'identique. */
    unsigned char* p = (unsigned char*)owl_ber_writer_data(&w);
    const unsigned char* end = p + owl_ber_writer_len(&w);
    size_t len = 0;
    int r = mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OCTET_STRING);
    CHECK(r == 0 && len == 5 && memcmp(p, utf8, 5) == 0,
          "relecture OCTET STRING utf8 identique");

    owl_ber_writer_free(&w);
}

/* ================================================================
 * Enrobage : SEQUENCE et tags arbitraires APPLICATION/CONTEXT
 * ================================================================ */

static void test_vector_sequence_wrap(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* SEQUENCE { INTEGER 1 } -> 30 03 02 01 01
     *   on ecrit d'abord le contenu (INTEGER 1 = 02 01 01, 3 octets),
     *   puis on enrobe en SEQUENCE : 30 = tag, 03 = longueur du contenu. */
    size_t mark = owl_ber_writer_len(&w);
    CHECK(owl_ber_write_integer(&w, 1), "seq: write INTEGER 1");
    CHECK(owl_ber_wrap_sequence(&w, owl_ber_writer_len(&w) - mark),
          "seq: wrap SEQUENCE");
    CHECK(writer_equals(&w,
          (const unsigned char[]){0x30, 0x03, 0x02, 0x01, 0x01}, 5),
          "vecteur SEQUENCE{INTEGER 1} = 30 03 02 01 01");

    owl_ber_writer_free(&w);
}

static void test_arbitrary_tags(void) {
    /* Composition des octets de tag LDAP (RFC 4511 annexe B). */
    CHECK(owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 0) == 0x60,
          "tag APPLICATION 0 constructed = 0x60 (BindRequest)");
    CHECK(owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 1) == 0x61,
          "tag APPLICATION 1 constructed = 0x61 (BindResponse)");
    CHECK(owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 3) == 0x63,
          "tag APPLICATION 3 constructed = 0x63 (SearchRequest)");
    CHECK(owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 4) == 0x64,
          "tag APPLICATION 4 constructed = 0x64 (SearchResultEntry)");
    CHECK(owl_ber_tag(OWL_BER_CLASS_CONTEXT, false, 0) == 0x80,
          "tag CONTEXT 0 primitif = 0x80");
    CHECK(owl_ber_tag(OWL_BER_CLASS_CONTEXT, true, 0) == 0xA0,
          "tag CONTEXT 0 constructed = 0xA0 (controls)");
}

/* ================================================================
 * LDAPMessage : enveloppe complete, vecteur a la main
 * ================================================================ */

static void test_vector_ldap_message(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* Squelette BindRequest : protocolOp = [APPLICATION 0] SEQUENCE
     * contenant juste INTEGER 3 (le champ version). On construit le corps
     * protocolOp, puis owl_ldap_message_finish ajoute messageID + SEQUENCE.
     *
     * Attendu, octet par octet (la longueur d'une SEQUENCE = taille du
     * CONTENU, soit 8 octets ici ; le total transfere fait 10 octets) :
     *   30 08                SEQUENCE, longueur 8   (LDAPMessage)
     *     02 01 01           INTEGER 1              (messageID)
     *     60 03              [APPLICATION 0] constructed, longueur 3
     *       02 01 03         INTEGER 3              (version)
     */
    size_t mark = owl_ber_writer_len(&w);
    CHECK(owl_ber_write_integer(&w, 3), "msg: write version INTEGER 3");
    CHECK(owl_ber_wrap(&w, owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 0),
                       owl_ber_writer_len(&w) - mark),
          "msg: wrap protocolOp APPLICATION 0");
    CHECK(owl_ldap_message_finish(&w, 1), "msg: finish LDAPMessage id=1");

    const unsigned char expected[] = {
        0x30, 0x08,
          0x02, 0x01, 0x01,
          0x60, 0x03,
            0x02, 0x01, 0x03
    };
    CHECK(writer_equals(&w, expected, sizeof expected),
          "vecteur LDAPMessage BindRequest = 30 08 02 01 01 60 03 02 01 03");

    owl_ber_writer_free(&w);
}

/* ================================================================
 * Round-trip encode -> parse d'un LDAPMessage
 * ================================================================ */

static void test_round_trip_message(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* Squelette BindResponse : protocolOp = [APPLICATION 1] contenant
     * ENUMERATED 0 (resultCode = success). messageID = 2.
     * Vecteur attendu : 30 08 02 01 02 61 03 0A 01 00. */
    size_t mark = owl_ber_writer_len(&w);
    CHECK(owl_ber_write_enumerated(&w, 0), "rt: write resultCode ENUM 0");
    CHECK(owl_ber_wrap(&w, owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 1),
                       owl_ber_writer_len(&w) - mark),
          "rt: wrap protocolOp APPLICATION 1");
    CHECK(owl_ldap_message_finish(&w, 2), "rt: finish LDAPMessage id=2");
    CHECK(writer_equals(&w, (const unsigned char[]){
              0x30, 0x08, 0x02, 0x01, 0x02, 0x61, 0x03, 0x0A, 0x01, 0x00}, 10),
          "vecteur LDAPMessage BindResponse = 30 08 02 01 02 61 03 0A 01 00");

    /* Parse : messageID = 2, tag protocolOp = 0x61, contenu = 0A 01 00. */
    OwlLdapMessageHeader hdr;
    bool ok = owl_ldap_message_parse(owl_ber_writer_data(&w),
                                     owl_ber_writer_len(&w), &hdr);
    CHECK(ok, "rt: parse LDAPMessage ok");
    CHECK(ok && hdr.messageID == 2, "rt: messageID = 2");
    CHECK(ok && hdr.protocolOpTag == 0x61, "rt: protocolOpTag = 0x61");
    CHECK(ok && hdr.opLen == 3, "rt: opLen = 3");

    /* Le curseur protocolOp pointe sur l'ENUMERATED : on le relit. */
    if (ok) {
        unsigned char* p = (unsigned char*)hdr.opContent;
        const unsigned char* end = hdr.opContent + hdr.opLen;
        int val = 0;
        int r = mbedtls_asn1_get_enum(&p, end, &val);
        CHECK(r == 0 && val == 0, "rt: resultCode relu = 0");
    }

    owl_ber_writer_free(&w);
}

/* ================================================================
 * Parsing : cas d'erreur (entree non fiable)
 * ================================================================ */

static void test_parse_errors(void) {
    OwlLdapMessageHeader hdr;

    /* Tag inattendu : commence par INTEGER (02) au lieu de SEQUENCE (30). */
    {
        const unsigned char buf[] = {0x02, 0x01, 0x01};
        CHECK(!owl_ldap_message_parse(buf, sizeof buf, &hdr),
              "err: tag inattendu (INTEGER au lieu de SEQUENCE) rejete");
    }

    /* Longueur qui deborde : SEQUENCE annonce 5 octets, 3 disponibles. */
    {
        const unsigned char buf[] = {0x30, 0x05, 0x02, 0x01, 0x01};
        CHECK(!owl_ldap_message_parse(buf, sizeof buf, &hdr),
              "err: longueur SEQUENCE debordante rejetee");
    }

    /* Longueur indefinie (0x80) : non supportee en BER defini -> rejet. */
    {
        const unsigned char buf[] = {0x30, 0x80, 0x02, 0x01, 0x01, 0x00, 0x00};
        CHECK(!owl_ldap_message_parse(buf, sizeof buf, &hdr),
              "err: longueur indefinie rejetee");
    }

    /* protocolOp dont la longueur deborde la SEQUENCE englobante :
     *   30 05            SEQUENCE longueur 5
     *     02 01 01       messageID INTEGER 1
     *     60 05          [APPLICATION 0] annonce 5 octets, 0 disponible
     */
    {
        const unsigned char buf[] = {0x30, 0x05, 0x02, 0x01, 0x01, 0x60, 0x05};
        CHECK(!owl_ldap_message_parse(buf, sizeof buf, &hdr),
              "err: longueur protocolOp debordante rejetee");
    }

    /* Buffer vide et tronque. */
    {
        const unsigned char buf[] = {0x30};
        CHECK(!owl_ldap_message_parse(buf, sizeof buf, &hdr),
              "err: SEQUENCE sans longueur rejetee");
        CHECK(!owl_ldap_message_parse(buf, 0, &hdr),
              "err: buffer vide rejete");
    }

    /* SEQUENCE sans messageID : contenu vide. */
    {
        const unsigned char buf[] = {0x30, 0x00};
        CHECK(!owl_ldap_message_parse(buf, sizeof buf, &hdr),
              "err: LDAPMessage sans messageID rejete");
    }

    /* messageID present mais protocolOp absent. */
    {
        const unsigned char buf[] = {0x30, 0x03, 0x02, 0x01, 0x01};
        CHECK(!owl_ldap_message_parse(buf, sizeof buf, &hdr),
              "err: LDAPMessage sans protocolOp rejete");
    }
}

/* ================================================================
 * Croissance du buffer : force realloc + relocalisation du contenu
 * ================================================================ */

static void test_writer_growth(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* OCTET STRING de 2000 octets : depasse la capacite initiale (256) et
     * force plusieurs grow avec recopie du contenu vers la queue. */
    enum { N = 2000 };
    unsigned char payload[N];
    for (int i = 0; i < N; i++) payload[i] = (unsigned char)(i & 0xFF);

    CHECK(owl_ber_write_octet_string(&w, payload, N), "grow: write 2000 octets");

    /* TLV attendu : 04 (tag) + longueur forme longue 82 07 D0 (0x07D0=2000)
     * + 2000 octets de contenu = 2004 octets au total. */
    CHECK(owl_ber_writer_len(&w) == 2004, "grow: longueur TLV = 2004");

    unsigned char* p = (unsigned char*)owl_ber_writer_data(&w);
    const unsigned char* end = p + owl_ber_writer_len(&w);
    size_t len = 0;
    int r = mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OCTET_STRING);
    CHECK(r == 0 && len == N, "grow: relecture longueur contenu = 2000");
    CHECK(r == 0 && memcmp(p, payload, N) == 0,
          "grow: contenu preserve apres relocalisation");

    owl_ber_writer_free(&w);
}

int ldap_ber_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_vectors_primitives();
    test_octet_string_utf8_raw();
    test_vector_sequence_wrap();
    test_arbitrary_tags();
    test_vector_ldap_message();
    test_round_trip_message();
    test_parse_errors();
    test_writer_growth();

    printf("ldap_ber self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
