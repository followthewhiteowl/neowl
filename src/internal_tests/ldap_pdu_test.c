// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test de la couche PDU LDAP (ldap_pdu.{c,h}).
 *
 * Execute via `owl --self-test-net` (famille reseau pure, cf. main.c).
 * C pur, aucune VM, aucune socket.
 *
 * Deux familles de verifs :
 *   - ENCODAGE : BindRequest / SearchRequest / UnbindRequest comparees a
 *     des VECTEURS D'OCTETS calcules a la main, chaque octet commente par
 *     son decodage TLV (seul garde-fou contre un codec auto-confirmant).
 *   - PARSING : reponses synthetiques (BindResponse, SearchResultEntry
 *     mono et multi-value, SearchResultDone) + cas d'erreur (mauvais tag,
 *     longueur debordante, SET vide, PDU tronque).
 *
 * Rappels TLV (RFC 4511 = BER) utilises ici :
 *   02 INTEGER, 01 BOOLEAN, 0A ENUMERATED, 04 OCTET STRING,
 *   30 SEQUENCE, 31 SET (0x11 | constructed 0x20).
 *   Tags LDAP : 60 BindRequest, 61 BindResponse, 42 UnbindRequest,
 *   63 SearchRequest, 64 SearchResultEntry, 65 SearchResultDone,
 *   80 [0] simple (CONTEXT 0 primitif), 87 [7] present (CONTEXT 7).
 */

#include "ldap_pdu.h"

#include <mbedtls/asn1.h>

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static bool writer_equals(const OwlBerWriter* w,
                          const unsigned char* expected, size_t n) {
    if (owl_ber_writer_len(w) != n) return false;
    const unsigned char* got = owl_ber_writer_data(w);
    if (got == NULL) return n == 0;
    return memcmp(got, expected, n) == 0;
}

/* ================================================================
 * Encodage : BindRequest (simple bind)
 * ================================================================ */

static void test_encode_bind_request(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* message_id=1, dn="cn=admin", password="secret".
     *
     * Attendu, octet par octet :
     *   30 1A                      SEQUENCE len 26         (LDAPMessage)
     *     02 01 01                 INTEGER 1               (messageID)
     *     60 15                    [APPLICATION 0] len 21  (BindRequest)
     *       02 01 03               INTEGER 3               (version LDAPv3)
     *       04 08 63 6E 3D 61 64 6D 69 6E   OCTET STRING "cn=admin" (name)
     *       80 06 73 65 63 72 65 74         [0] simple "secret" (auth)
     */
    CHECK(owl_ldap_encode_bind_request(&w, 1, 3, "cn=admin",
              (const unsigned char*)"secret", 6, NULL),
          "bind: encode ok");

    const unsigned char expected[] = {
        0x30, 0x1A,
          0x02, 0x01, 0x01,
          0x60, 0x15,
            0x02, 0x01, 0x03,
            0x04, 0x08, 0x63, 0x6E, 0x3D, 0x61, 0x64, 0x6D, 0x69, 0x6E,
            0x80, 0x06, 0x73, 0x65, 0x63, 0x72, 0x65, 0x74
    };
    CHECK(writer_equals(&w, expected, sizeof expected),
          "bind: vecteur BindRequest");

    /* Re-parse l'enveloppe : tag protocolOp = 0x60. */
    OwlLdapMessageHeader hdr;
    bool ok = owl_ldap_message_parse(owl_ber_writer_data(&w),
                                     owl_ber_writer_len(&w), &hdr);
    CHECK(ok && hdr.messageID == 1
             && hdr.protocolOpTag == OWL_LDAP_OP_BIND_REQUEST,
          "bind: enveloppe relue (id=1, tag=0x60)");

    owl_ber_writer_free(&w);
}

/* ================================================================
 * Encodage : BindRequest SASL EXTERNAL (mTLS)
 * ================================================================ */

static void test_encode_bind_request_sasl_external(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* message_id=1, name vide, mecanisme "EXTERNAL", credentials omis.
     *
     * Attendu, octet par octet :
     *   30 16                      SEQUENCE len 22         (LDAPMessage)
     *     02 01 01                 INTEGER 1               (messageID)
     *     60 11                    [APPLICATION 0] len 17  (BindRequest)
     *       02 01 03               INTEGER 3               (version LDAPv3)
     *       04 00                  OCTET STRING ""         (name vide)
     *       A3 0A                  [3] sasl len 10         (SaslCredentials)
     *         04 08 45 58 54 45 52 4E 41 4C   OCTET STRING "EXTERNAL"
     *                                         (mechanism ; credentials omis)
     */
    CHECK(owl_ldap_encode_bind_request(&w, 1, 3, "", NULL, 0, "EXTERNAL"),
          "bind sasl: encode ok");

    const unsigned char expected[] = {
        0x30, 0x16,
          0x02, 0x01, 0x01,
          0x60, 0x11,
            0x02, 0x01, 0x03,
            0x04, 0x00,
            0xA3, 0x0A,
              0x04, 0x08, 0x45, 0x58, 0x54, 0x45, 0x52, 0x4E, 0x41, 0x4C
    };
    CHECK(writer_equals(&w, expected, sizeof expected),
          "bind sasl: vecteur BindRequest EXTERNAL");

    OwlLdapMessageHeader hdr;
    bool ok = owl_ldap_message_parse(owl_ber_writer_data(&w),
                                     owl_ber_writer_len(&w), &hdr);
    CHECK(ok && hdr.messageID == 1
             && hdr.protocolOpTag == OWL_LDAP_OP_BIND_REQUEST,
          "bind sasl: enveloppe relue (id=1, tag=0x60)");

    owl_ber_writer_free(&w);
}

/* ================================================================
 * Encodage : SearchRequest (filtre opaque injecte)
 * ================================================================ */

static void test_encode_search_request(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* Filtre pre-encode opaque (produit par la couche filtres au lot
     * suivant) : present "objectClass" = [7] IMPLICIT OCTET STRING.
     *   87 0B 6F 62 6A 65 63 74 43 6C 61 73 73   (objectClass=*) */
    const unsigned char filter[] = {
        0x87, 0x0B, 0x6F, 0x62, 0x6A, 0x65, 0x63, 0x74,
        0x43, 0x6C, 0x61, 0x73, 0x73
    };
    const char* attrs[] = { "cn" };

    /* message_id=2, base="dc=x", scope=wholeSubtree(2), attributs={"cn"}.
     *
     * Attendu, octet par octet :
     *   30 2D                      SEQUENCE len 45         (LDAPMessage)
     *     02 01 02                 INTEGER 2               (messageID)
     *     63 28                    [APPLICATION 3] len 40  (SearchRequest)
     *       04 04 64 63 3D 78      OCTET STRING "dc=x"     (baseObject)
     *       0A 01 02               ENUMERATED 2            (scope wholeSubtree)
     *       0A 01 00               ENUMERATED 0            (neverDerefAliases)
     *       02 01 00               INTEGER 0               (sizeLimit)
     *       02 01 00               INTEGER 0               (timeLimit)
     *       01 01 00               BOOLEAN FALSE           (typesOnly)
     *       87 0B ...objectClass   [7] present             (filter, opaque)
     *       30 04 04 02 63 6E      SEQUENCE { "cn" }       (attributes)
     */
    CHECK(owl_ldap_encode_search_request(&w, 2, "dc=x",
              OWL_LDAP_SCOPE_WHOLE_SUBTREE, filter, sizeof filter,
              attrs, 1),
          "search: encode ok");

    const unsigned char expected[] = {
        0x30, 0x2D,
          0x02, 0x01, 0x02,
          0x63, 0x28,
            0x04, 0x04, 0x64, 0x63, 0x3D, 0x78,
            0x0A, 0x01, 0x02,
            0x0A, 0x01, 0x00,
            0x02, 0x01, 0x00,
            0x02, 0x01, 0x00,
            0x01, 0x01, 0x00,
            0x87, 0x0B, 0x6F, 0x62, 0x6A, 0x65, 0x63, 0x74,
                        0x43, 0x6C, 0x61, 0x73, 0x73,
            0x30, 0x04, 0x04, 0x02, 0x63, 0x6E
    };
    CHECK(writer_equals(&w, expected, sizeof expected),
          "search: vecteur SearchRequest");

    OwlLdapMessageHeader hdr;
    bool ok = owl_ldap_message_parse(owl_ber_writer_data(&w),
                                     owl_ber_writer_len(&w), &hdr);
    CHECK(ok && hdr.messageID == 2
             && hdr.protocolOpTag == OWL_LDAP_OP_SEARCH_REQUEST,
          "search: enveloppe relue (id=2, tag=0x63)");

    owl_ber_writer_free(&w);

    /* Filtre absent => echec propre (le filtre est obligatoire). */
    owl_ber_writer_init(&w);
    CHECK(!owl_ldap_encode_search_request(&w, 3, "dc=x",
              OWL_LDAP_SCOPE_WHOLE_SUBTREE, NULL, 0, attrs, 1),
          "search: filtre NULL rejete");
    owl_ber_writer_free(&w);
}

/* ================================================================
 * Encodage : UnbindRequest
 * ================================================================ */

static void test_encode_unbind_request(void) {
    OwlBerWriter w;
    owl_ber_writer_init(&w);

    /* message_id=5. UnbindRequest = [APPLICATION 2] NULL (contenu vide).
     *   30 05                SEQUENCE len 5   (LDAPMessage)
     *     02 01 05           INTEGER 5        (messageID)
     *     42 00              [APPLICATION 2] len 0 (UnbindRequest, primitif)
     */
    CHECK(owl_ldap_encode_unbind_request(&w, 5), "unbind: encode ok");

    const unsigned char expected[] = {
        0x30, 0x05, 0x02, 0x01, 0x05, 0x42, 0x00
    };
    CHECK(writer_equals(&w, expected, sizeof expected),
          "unbind: vecteur UnbindRequest = 30 05 02 01 05 42 00");

    OwlLdapMessageHeader hdr;
    bool ok = owl_ldap_message_parse(owl_ber_writer_data(&w),
                                     owl_ber_writer_len(&w), &hdr);
    CHECK(ok && hdr.messageID == 5
             && hdr.protocolOpTag == OWL_LDAP_OP_UNBIND_REQUEST
             && hdr.opLen == 0,
          "unbind: enveloppe relue (id=5, tag=0x42, len 0)");

    owl_ber_writer_free(&w);
}

/* ================================================================
 * Parsing : BindResponse (resultCode)
 * ================================================================ */

static void test_parse_bind_response(void) {
    /* Reponse de succes construite via le writer (round-trip) :
     * BindResponse [APPLICATION 1] contenant ENUMERATED 0, messageID 1. */
    OwlBerWriter w;
    owl_ber_writer_init(&w);
    size_t mark = owl_ber_writer_len(&w);
    owl_ber_write_enumerated(&w, OWL_LDAP_SUCCESS);
    owl_ber_wrap(&w, owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 1),
                 owl_ber_writer_len(&w) - mark);
    owl_ldap_message_finish(&w, 1);

    OwlLdapMessageHeader hdr;
    bool ok = owl_ldap_message_parse(owl_ber_writer_data(&w),
                                     owl_ber_writer_len(&w), &hdr);
    CHECK(ok && hdr.protocolOpTag == OWL_LDAP_OP_BIND_RESPONSE,
          "bindresp: tag protocolOp = 0x61");

    OwlLdapResult res;
    CHECK(ok && owl_ldap_parse_result(hdr.opContent, hdr.opLen, &res)
             && res.resultCode == OWL_LDAP_SUCCESS,
          "bindresp: resultCode success (0)");
    owl_ber_writer_free(&w);

    /* Reponse d'echec synthetique : resultCode invalidCredentials (49)
     * suivi de matchedDN "" et diagnosticMessage "". parse_result lit le
     * resultCode puis les deux OCTET STRING ; un diagnosticMessage vide
     * laisse diagnosticMessage NULL (cas verrouille plus bas).
     *   0A 01 31   ENUMERATED 49
     *   04 00      matchedDN ""
     *   04 00      diagnosticMessage "" */
    const unsigned char content[] = {
        0x0A, 0x01, 0x31, 0x04, 0x00, 0x04, 0x00
    };
    OwlLdapResult res2;
    CHECK(owl_ldap_parse_result(content, sizeof content, &res2)
             && res2.resultCode == OWL_LDAP_INVALID_CREDENTIALS,
          "bindresp: resultCode invalidCredentials (49)");

    /* Mauvais tag : commence par OCTET STRING au lieu d'ENUMERATED. */
    const unsigned char bad[] = { 0x04, 0x01, 0x00 };
    CHECK(!owl_ldap_parse_result(bad, sizeof bad, &res2),
          "bindresp: resultCode non-ENUMERATED rejete");
}

/* ================================================================
 * Parsing : diagnosticMessage + serverSaslCreds [7]
 *
 * Verrouille l'avancement du curseur apres matchedDN/diagnosticMessage.
 * Un off-by-one y casserait silencieusement l'extraction du [7], coeur
 * d'un bind SASL multi-etapes.
 * ================================================================ */

static void test_parse_result_diag_and_sasl(void) {
    /* diagnosticMessage non-vide : resultCode 49 + matchedDN "" +
     * diagnosticMessage "80090346" (sous-code diagnostique serveur).
     *   0A 01 31              ENUMERATED 49
     *   04 00                 matchedDN ""
     *   04 08 "80090346"      diagnosticMessage */
    {
        const unsigned char content[] = {
            0x0A, 0x01, 0x31,
            0x04, 0x00,
            0x04, 0x08, 0x38, 0x30, 0x30, 0x39, 0x30, 0x33, 0x34, 0x36
        };
        OwlLdapResult res;
        bool ok = owl_ldap_parse_result(content, sizeof content, &res);
        CHECK(ok && res.resultCode == OWL_LDAP_INVALID_CREDENTIALS,
              "diag: resultCode invalidCredentials (49)");
        CHECK(ok && res.diagnosticMessageLen == 8,
              "diag: diagnosticMessage longueur 8");
        CHECK(ok && res.diagnosticMessage != NULL
                 && memcmp(res.diagnosticMessage, "80090346", 8) == 0,
              "diag: diagnosticMessage contenu = 80090346");
        CHECK(ok && res.serverSaslCreds == NULL,
              "diag: pas de serverSaslCreds");
    }

    /* serverSaslCreds [7] APRES matchedDN + diagnosticMessage vides :
     * resultCode 14 (saslBindInProgress). ASSERTION ANTI-REGRESSION
     * critique : le curseur doit arriver pile sur le [7].
     *   0A 01 0E              ENUMERATED 14
     *   04 00                 matchedDN ""
     *   04 00                 diagnosticMessage ""
     *   87 04 A1 02 03 04     [7] serverSaslCreds (4 octets connus) */
    {
        const unsigned char content[] = {
            0x0A, 0x01, 0x0E,
            0x04, 0x00,
            0x04, 0x00,
            0x87, 0x04, 0xA1, 0x02, 0x03, 0x04
        };
        const unsigned char creds[] = { 0xA1, 0x02, 0x03, 0x04 };
        OwlLdapResult res;
        bool ok = owl_ldap_parse_result(content, sizeof content, &res);
        CHECK(ok && res.resultCode == OWL_LDAP_SASL_BIND_IN_PROGRESS,
              "sasl: resultCode saslBindInProgress (14)");
        CHECK(ok && res.serverSaslCreds != NULL,
              "sasl: serverSaslCreds present");
        CHECK(ok && res.serverSaslCredsLen == 4,
              "sasl: serverSaslCreds longueur 4");
        CHECK(ok && res.serverSaslCreds != NULL
                 && memcmp(res.serverSaslCreds, creds, 4) == 0,
              "sasl: serverSaslCreds octets A1 02 03 04");
    }

    /* diagnosticMessage non-vide ET serverSaslCreds dans le meme PDU :
     * les deux doivent sortir intacts (le [7] vient apres un diag non-vide).
     *   0A 01 0E              ENUMERATED 14
     *   04 00                 matchedDN ""
     *   04 05 "hello"         diagnosticMessage
     *   87 03 DE AD BE        [7] serverSaslCreds (3 octets connus) */
    {
        const unsigned char content[] = {
            0x0A, 0x01, 0x0E,
            0x04, 0x00,
            0x04, 0x05, 0x68, 0x65, 0x6C, 0x6C, 0x6F,
            0x87, 0x03, 0xDE, 0xAD, 0xBE
        };
        const unsigned char creds[] = { 0xDE, 0xAD, 0xBE };
        OwlLdapResult res;
        bool ok = owl_ldap_parse_result(content, sizeof content, &res);
        CHECK(ok && res.diagnosticMessageLen == 5
                 && res.diagnosticMessage != NULL
                 && memcmp(res.diagnosticMessage, "hello", 5) == 0,
              "diag+sasl: diagnosticMessage = hello");
        CHECK(ok && res.serverSaslCredsLen == 3
                 && res.serverSaslCreds != NULL
                 && memcmp(res.serverSaslCreds, creds, 3) == 0,
              "diag+sasl: serverSaslCreds octets DE AD BE");
    }
}

/* ================================================================
 * Parsing : SearchResultEntry (mono + multi-value)
 *
 * Contenu synthetique (= opContent apres le tag [APPLICATION 4]) :
 *   04 06 "cn=bob"                       objectName (DN)
 *   30 25                                attributes SEQUENCE OF (len 37)
 *     30 0B                              PartialAttribute (len 11)
 *       04 02 "cn"                       type
 *       31 05                            vals SET (len 5)
 *         04 03 "bob"                    value (mono)
 *     30 16                              PartialAttribute (len 22)
 *       04 04 "mail"                     type
 *       31 0E                            vals SET (len 14)
 *         04 05 "bob@x"                  value 1 (ordre preserve)
 *         04 05 "bob@y"                  value 2
 * ================================================================ */

static const unsigned char kEntryContent[] = {
    0x04, 0x06, 0x63, 0x6E, 0x3D, 0x62, 0x6F, 0x62,            /* "cn=bob" */
    0x30, 0x25,
      0x30, 0x0B,
        0x04, 0x02, 0x63, 0x6E,                               /* "cn" */
        0x31, 0x05,
          0x04, 0x03, 0x62, 0x6F, 0x62,                       /* "bob" */
      0x30, 0x16,
        0x04, 0x04, 0x6D, 0x61, 0x69, 0x6C,                   /* "mail" */
        0x31, 0x0E,
          0x04, 0x05, 0x62, 0x6F, 0x62, 0x40, 0x78,           /* "bob@x" */
          0x04, 0x05, 0x62, 0x6F, 0x62, 0x40, 0x79            /* "bob@y" */
};

static bool bytes_equal(const OwlLdapValue* v, const char* s) {
    size_t n = strlen(s);
    return v->len == n && memcmp(v->value, s, n) == 0;
}

static bool type_equal(const OwlLdapAttribute* a, const char* s) {
    size_t n = strlen(s);
    return a->type_len == n && memcmp(a->type, s, n) == 0;
}

static void test_parse_search_entry(void) {
    OwlLdapSearchEntry e;
    bool ok = owl_ldap_parse_search_entry(kEntryContent,
                                          sizeof kEntryContent, &e);
    CHECK(ok, "entry: parse ok");
    if (ok) {
        CHECK(e.dn_len == 6 && memcmp(e.dn, "cn=bob", 6) == 0,
              "entry: DN = cn=bob");
        CHECK(e.attribute_count == 2, "entry: 2 attributs");

        if (e.attribute_count == 2) {
            /* attribut mono-value "cn" */
            CHECK(type_equal(&e.attributes[0], "cn"), "entry: attr[0] type cn");
            CHECK(e.attributes[0].value_count == 1, "entry: cn 1 valeur");
            CHECK(e.attributes[0].value_count == 1
                     && bytes_equal(&e.attributes[0].values[0], "bob"),
                  "entry: cn[0] = bob");

            /* attribut multi-value "mail", ordre serveur preserve */
            CHECK(type_equal(&e.attributes[1], "mail"),
                  "entry: attr[1] type mail");
            CHECK(e.attributes[1].value_count == 2, "entry: mail 2 valeurs");
            CHECK(e.attributes[1].value_count == 2
                     && bytes_equal(&e.attributes[1].values[0], "bob@x"),
                  "entry: mail[0] = bob@x");
            CHECK(e.attributes[1].value_count == 2
                     && bytes_equal(&e.attributes[1].values[1], "bob@y"),
                  "entry: mail[1] = bob@y (ordre preserve)");
        }
        owl_ldap_search_entry_free(&e);
        /* Idempotence du free. */
        CHECK(e.attributes == NULL && e.attribute_count == 0,
              "entry: free reinitialise");
        owl_ldap_search_entry_free(&e);
    }
}

/* ================================================================
 * Agregation : N entrees + SearchResultDone
 * ================================================================ */

static void test_search_result_aggregation(void) {
    OwlLdapSearchResult r;
    owl_ldap_search_result_init(&r);

    CHECK(r.resultCode == -1 && !r.done && r.entry_count == 0,
          "aggr: init vide");

    /* Deux entrees (meme contenu synthetique reutilise). */
    CHECK(owl_ldap_search_result_add_entry(&r, kEntryContent,
              sizeof kEntryContent), "aggr: add entry 1");
    CHECK(owl_ldap_search_result_add_entry(&r, kEntryContent,
              sizeof kEntryContent), "aggr: add entry 2");
    CHECK(r.entry_count == 2, "aggr: 2 entrees accumulees");
    CHECK(r.entry_count == 2 && r.entries[1].attribute_count == 2,
          "aggr: entree 2 parsee");

    /* SearchResultDone synthetique : resultCode success. */
    const unsigned char done[] = { 0x0A, 0x01, 0x00 };
    CHECK(owl_ldap_search_result_set_done(&r, done, sizeof done),
          "aggr: set done");
    CHECK(r.done && r.resultCode == OWL_LDAP_SUCCESS,
          "aggr: done + resultCode success");

    owl_ldap_search_result_free(&r);
    CHECK(r.entries == NULL && r.entry_count == 0, "aggr: free");
}

/* ================================================================
 * Parsing : cas d'erreur (entree non fiable)
 * ================================================================ */

static void test_parse_errors(void) {
    OwlLdapSearchEntry e;

    /* objectName manquant : contenu vide. */
    CHECK(!owl_ldap_parse_search_entry((const unsigned char*)"", 0, &e),
          "err: entry vide rejetee");

    /* DN present mais attributes pas une SEQUENCE (ici un INTEGER). */
    {
        const unsigned char buf[] = {
            0x04, 0x01, 0x61,   /* objectName "a" */
            0x02, 0x01, 0x00    /* INTEGER au lieu de SEQUENCE */
        };
        CHECK(!owl_ldap_parse_search_entry(buf, sizeof buf, &e),
              "err: attributes non-SEQUENCE rejete");
    }

    /* SET vide : un attribut sans valeur doit etre rejete.
     *   04 01 "a"  DN
     *   30 06      attributes SEQUENCE
     *     30 04    PartialAttribute
     *       04 02 "cn"  type
     *       31 00       SET vide */
    {
        const unsigned char buf[] = {
            0x04, 0x01, 0x61,
            0x30, 0x06,
              0x30, 0x04,
                0x04, 0x02, 0x63, 0x6E,
                0x31, 0x00
        };
        CHECK(!owl_ldap_parse_search_entry(buf, sizeof buf, &e),
              "err: SET vide rejete");
    }

    /* Longueur debordante : SEQUENCE attributes annonce 10 octets, 2 dispo. */
    {
        const unsigned char buf[] = {
            0x04, 0x01, 0x61,
            0x30, 0x0A, 0x30, 0x00   /* 0x0A > reste disponible */
        };
        CHECK(!owl_ldap_parse_search_entry(buf, sizeof buf, &e),
              "err: longueur attributes debordante rejetee");
    }

    /* PDU tronque : DN annonce 4 octets, 1 disponible. */
    {
        const unsigned char buf[] = { 0x04, 0x04, 0x61 };
        CHECK(!owl_ldap_parse_search_entry(buf, sizeof buf, &e),
              "err: DN tronque rejete");
    }

    /* parse_result sur buffer vide. */
    {
        OwlLdapResult res;
        CHECK(!owl_ldap_parse_result((const unsigned char*)"", 0, &res),
              "err: parse_result buffer vide rejete");
    }
}

int ldap_pdu_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_encode_bind_request();
    test_encode_bind_request_sasl_external();
    test_encode_search_request();
    test_encode_unbind_request();
    test_parse_bind_response();
    test_parse_result_diag_and_sasl();
    test_parse_search_entry();
    test_search_result_aggregation();
    test_parse_errors();

    printf("ldap_pdu self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
