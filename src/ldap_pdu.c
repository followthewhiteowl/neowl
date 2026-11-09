// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "ldap_pdu.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/asn1.h>

/* ================================================================
 * Ecriture d'octets bruts dans le writer BER
 *
 * ldap_ber (lot BER) n'expose pas de primitive "prepend des octets
 * bruts" : seulement des TLV complets. On en a besoin pour inserer le
 * filtre (bloc BER opaque deja encode) a sa place dans SearchRequest.
 *
 * Astuce sans toucher a ldap_ber : on ecrit les octets comme un OCTET
 * STRING (owl_ber_write_octet_string fait croitre le buffer tout seul),
 * ce qui prepend "04 <longueur> <octets>", puis on avance le curseur
 * d'ecriture pour rejeter le prefixe tag+longueur. Il ne reste que les
 * octets bruts, prependes a l'identique. OOM-safe : si le write echoue,
 * on ne touche pas au curseur.
 * ================================================================ */
static bool pdu_write_raw(OwlBerWriter* w,
                          const unsigned char* data, size_t len) {
    size_t before = owl_ber_writer_len(w);
    if (!owl_ber_write_octet_string(w, data, len)) return false;
    size_t header = owl_ber_writer_len(w) - before - len;  /* tag + longueur */
    w->p += header;  /* rejette "04 <len>" : les octets bruts restent en tete */
    return true;
}

/* ================================================================
 * Encodage des requetes
 * ================================================================ */

bool owl_ldap_encode_bind_request_sasl(OwlBerWriter* w, int message_id,
                                       int version, const char* dn,
                                       const char* mechanism,
                                       const unsigned char* credentials,
                                       size_t credentials_len) {
    const char* name = dn ? dn : "";

    /* Ecriture a rebours : dernier champ d'abord (authentication), puis
     * name, puis version. */
    size_t mark = owl_ber_writer_len(w);

    /* authentication [3] sasl SaslCredentials ::= SEQUENCE {
     *   mechanism LDAPString, credentials OCTET STRING OPTIONAL }.
     * Le tag [3] IMPLICIT remplace le tag SEQUENCE universel (0xA3).
     * A rebours : credentials (dernier champ) d'abord, puis mechanism. */
    size_t amark = owl_ber_writer_len(w);
    if (credentials != NULL) {
        if (!owl_ber_write_octet_string(w, credentials, credentials_len))
            return false;
    }
    if (!owl_ber_write_octet_string(w, (const unsigned char*)mechanism,
                                    strlen(mechanism))) return false;
    if (!owl_ber_wrap(w, owl_ber_tag(OWL_BER_CLASS_CONTEXT, true, 3),
                      owl_ber_writer_len(w) - amark)) return false;

    /* name LDAPDN */
    if (!owl_ber_write_octet_string(w, (const unsigned char*)name,
                                    strlen(name))) return false;

    /* version INTEGER */
    if (!owl_ber_write_integer(w, version)) return false;

    /* enveloppe [APPLICATION 0] SEQUENCE */
    if (!owl_ber_wrap(w, owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 0),
                      owl_ber_writer_len(w) - mark)) return false;

    return owl_ldap_message_finish(w, message_id);
}

bool owl_ldap_encode_bind_request(OwlBerWriter* w, int message_id, int version,
                                  const char* dn,
                                  const unsigned char* password,
                                  size_t password_len,
                                  const char* sasl_mechanism) {
    if (sasl_mechanism != NULL) {
        /* SASL single-step (EXTERNAL) : credentials OMIS. */
        return owl_ldap_encode_bind_request_sasl(w, message_id, version, dn,
                                                 sasl_mechanism, NULL, 0);
    }

    const unsigned char* pw = password ? password : (const unsigned char*)"";
    const char* name = dn ? dn : "";

    /* Ecriture a rebours : dernier champ d'abord (authentication), puis
     * name, puis version. */
    size_t mark = owl_ber_writer_len(w);

    /* authentication [0] simple : meme structure binaire qu'un OCTET STRING
     * primitif (tag, longueur, octets), seul le tag change. On ecrit
     * l'OCTET STRING (qui gere la croissance du buffer) puis on reecrit son
     * octet de tag en CONTEXT-0 primitif (0x80, IMPLICIT). */
    if (!owl_ber_write_octet_string(w, pw, password_len)) return false;
    *w->p = owl_ber_tag(OWL_BER_CLASS_CONTEXT, false, 0);  /* 0x04 -> 0x80 */

    /* name LDAPDN */
    if (!owl_ber_write_octet_string(w, (const unsigned char*)name,
                                    strlen(name))) return false;

    /* version INTEGER */
    if (!owl_ber_write_integer(w, version)) return false;

    /* enveloppe [APPLICATION 0] SEQUENCE */
    if (!owl_ber_wrap(w, owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 0),
                      owl_ber_writer_len(w) - mark)) return false;

    return owl_ldap_message_finish(w, message_id);
}

bool owl_ldap_encode_search_request(OwlBerWriter* w, int message_id,
                                    const char* base_dn,
                                    int scope,
                                    const unsigned char* filter_ber,
                                    size_t filter_len,
                                    const char* const* attributes,
                                    size_t attr_count) {
    if (filter_ber == NULL || filter_len == 0) return false;  /* filtre requis */
    const char* base = base_dn ? base_dn : "";

    size_t mark = owl_ber_writer_len(w);

    /* attributes SEQUENCE OF LDAPString. Ecrire a rebours pour preserver
     * l'ordre demande. */
    size_t amark = owl_ber_writer_len(w);
    for (size_t i = attr_count; i-- > 0; ) {
        const char* a = attributes[i] ? attributes[i] : "";
        if (!owl_ber_write_octet_string(w, (const unsigned char*)a,
                                        strlen(a))) return false;
    }
    if (!owl_ber_wrap_sequence(w, owl_ber_writer_len(w) - amark)) return false;

    /* filter : bloc BER opaque, insere tel quel */
    if (!pdu_write_raw(w, filter_ber, filter_len)) return false;

    /* typesOnly BOOLEAN FALSE */
    if (!owl_ber_write_boolean(w, false)) return false;
    /* timeLimit INTEGER 0 (pas de limite) */
    if (!owl_ber_write_integer(w, 0)) return false;
    /* sizeLimit INTEGER 0 (pas de limite) */
    if (!owl_ber_write_integer(w, 0)) return false;
    /* derefAliases ENUMERATED neverDerefAliases (0) */
    if (!owl_ber_write_enumerated(w, 0)) return false;
    /* scope ENUMERATED */
    if (!owl_ber_write_enumerated(w, scope)) return false;
    /* baseObject LDAPDN */
    if (!owl_ber_write_octet_string(w, (const unsigned char*)base,
                                    strlen(base))) return false;

    /* enveloppe [APPLICATION 3] SEQUENCE */
    if (!owl_ber_wrap(w, owl_ber_tag(OWL_BER_CLASS_APPLICATION, true, 3),
                      owl_ber_writer_len(w) - mark)) return false;

    return owl_ldap_message_finish(w, message_id);
}

bool owl_ldap_encode_unbind_request(OwlBerWriter* w, int message_id) {
    /* [APPLICATION 2] NULL : primitif, contenu vide. */
    if (!owl_ber_wrap(w, owl_ber_tag(OWL_BER_CLASS_APPLICATION, false, 2),
                      0)) return false;
    return owl_ldap_message_finish(w, message_id);
}

/* ================================================================
 * Parsing : LDAPResult (resultCode)
 * ================================================================ */

/* Lit un TLV a la position *p (borne sur end) : tag, pointeur/longueur du
 * contenu, et avance *p au-dela. Forme longue de longueur geree (1..4
 * octets), indefinie rejetee. Renvoie false si bornage depasse. */
static bool pdu_read_tlv(const unsigned char** p, const unsigned char* end,
                         unsigned char* tag,
                         const unsigned char** val, size_t* vlen) {
    if ((size_t)(end - *p) < 2) return false;
    *tag = (*p)[0];
    unsigned char l0 = (*p)[1];
    const unsigned char* q = *p + 2;
    size_t l;
    if (l0 < 0x80) {
        l = l0;
    } else {
        unsigned int nb = l0 & 0x7F;
        if (nb == 0 || nb > 4 || (size_t)(end - q) < nb) return false;
        l = 0;
        for (unsigned int i = 0; i < nb; i++) l = (l << 8) | q[i];
        q += nb;
    }
    if ((size_t)(end - q) < l) return false;
    *tag = (*p)[0];
    *val = q;
    *vlen = l;
    *p = q + l;
    return true;
}

bool owl_ldap_parse_result(const unsigned char* content, size_t len,
                           OwlLdapResult* out) {
    if (content == NULL || out == NULL) return false;

    out->resultCode = 0;
    out->diagnosticMessage = NULL;
    out->diagnosticMessageLen = 0;
    out->serverSaslCreds = NULL;
    out->serverSaslCredsLen = 0;

    unsigned char* p = (unsigned char*)content;
    const unsigned char* end = content + len;

    int code = 0;
    if (mbedtls_asn1_get_enum(&p, end, &code) != 0) return false;
    out->resultCode = code;

    /* Best-effort : apres resultCode viennent matchedDN (OCTET STRING) puis
     * diagnosticMessage (OCTET STRING), puis d'eventuels champs optionnels
     * (referral [3], serverSaslCreds [7]). On lit les deux OCTET STRING pour
     * exposer le diagnosticMessage (sous-code AD type "data 52e"), puis on
     * balaie les TLV restants pour extraire le serverSaslCreds [7]. Rien de
     * tout cela n'est requis : seul le resultCode fait echouer s'il manque. */
    {
        unsigned char* d = p;
        size_t slen = 0;
        if (mbedtls_asn1_get_tag(&d, end, &slen, MBEDTLS_ASN1_OCTET_STRING) == 0) {
            d += slen;  /* saute matchedDN */
            size_t dlen = 0;
            if (mbedtls_asn1_get_tag(&d, end, &dlen, MBEDTLS_ASN1_OCTET_STRING) == 0) {
                if (dlen > 0) {
                    out->diagnosticMessage = d;
                    out->diagnosticMessageLen = dlen;
                }
                d += dlen;
                p = d;  /* le scan [7] reprend apres diagnosticMessage */
            }
        }
    }

    const unsigned char* q = p;
    unsigned char tag;
    const unsigned char* val;
    size_t vlen;
    while (pdu_read_tlv(&q, end, &tag, &val, &vlen)) {
        if (tag == (OWL_BER_CLASS_CONTEXT | 7)) {   /* [7] serverSaslCreds */
            out->serverSaslCreds = val;
            out->serverSaslCredsLen = vlen;
            break;
        }
    }

    return true;
}

/* ================================================================
 * Parsing : SearchResultEntry
 * ================================================================ */

void owl_ldap_search_entry_free(OwlLdapSearchEntry* entry) {
    if (entry == NULL) return;
    for (size_t i = 0; i < entry->attribute_count; i++) {
        free(entry->attributes[i].values);
    }
    free(entry->attributes);
    entry->attributes = NULL;
    entry->attribute_count = 0;
    entry->dn = NULL;
    entry->dn_len = 0;
}

bool owl_ldap_parse_search_entry(const unsigned char* content, size_t len,
                                 OwlLdapSearchEntry* out) {
    if (content == NULL || out == NULL) return false;

    out->dn = NULL;
    out->dn_len = 0;
    out->attributes = NULL;
    out->attribute_count = 0;

    unsigned char* p = (unsigned char*)content;
    const unsigned char* end = content + len;
    size_t l = 0;

    /* objectName LDAPDN */
    if (mbedtls_asn1_get_tag(&p, end, &l, MBEDTLS_ASN1_OCTET_STRING) != 0)
        goto fail;
    out->dn = p;
    out->dn_len = l;
    p += l;

    /* attributes PartialAttributeList = SEQUENCE OF */
    size_t seqlen = 0;
    if (mbedtls_asn1_get_tag(&p, end, &seqlen,
            MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0)
        goto fail;
    const unsigned char* aend = p + seqlen;

    size_t acap = 0;
    while (p < aend) {
        /* PartialAttribute = SEQUENCE { type, vals SET OF } */
        size_t palen = 0;
        if (mbedtls_asn1_get_tag(&p, aend, &palen,
                MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0)
            goto fail;
        const unsigned char* pend = p + palen;

        /* type AttributeDescription (LDAPString) */
        size_t tlen = 0;
        if (mbedtls_asn1_get_tag(&p, pend, &tlen,
                MBEDTLS_ASN1_OCTET_STRING) != 0)
            goto fail;
        const unsigned char* tptr = p;
        p += tlen;

        /* vals SET OF value */
        size_t setlen = 0;
        if (mbedtls_asn1_get_tag(&p, pend, &setlen,
                MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0)
            goto fail;
        const unsigned char* send = p + setlen;

        OwlLdapValue* vals = NULL;
        size_t vcount = 0, vcap = 0;
        while (p < send) {
            size_t vlen = 0;
            if (mbedtls_asn1_get_tag(&p, send, &vlen,
                    MBEDTLS_ASN1_OCTET_STRING) != 0) {
                free(vals);
                goto fail;
            }
            if (vcount == vcap) {
                size_t nc = vcap ? vcap * 2 : 4;
                OwlLdapValue* nv = (OwlLdapValue*)realloc(vals,
                    nc * sizeof(OwlLdapValue));
                if (nv == NULL) { free(vals); goto fail; }
                vals = nv;
                vcap = nc;
            }
            vals[vcount].value = p;
            vals[vcount].len = vlen;
            vcount++;
            p += vlen;
        }
        if (vcount == 0) { free(vals); goto fail; }  /* SET vide rejete */

        /* push l'attribut dans out->attributes */
        if (out->attribute_count == acap) {
            size_t nc = acap ? acap * 2 : 4;
            OwlLdapAttribute* na = (OwlLdapAttribute*)realloc(out->attributes,
                nc * sizeof(OwlLdapAttribute));
            if (na == NULL) { free(vals); goto fail; }
            out->attributes = na;
            acap = nc;
        }
        OwlLdapAttribute* a = &out->attributes[out->attribute_count++];
        a->type = tptr;
        a->type_len = tlen;
        a->values = vals;
        a->value_count = vcount;

        p = (unsigned char*)pend;  /* resync sur la frontiere PartialAttribute */
    }

    return true;

fail:
    owl_ldap_search_entry_free(out);
    return false;
}

/* ================================================================
 * Agregation d'une reponse de recherche
 * ================================================================ */

void owl_ldap_search_result_init(OwlLdapSearchResult* r) {
    r->entries = NULL;
    r->entry_count = 0;
    r->entry_cap = 0;
    r->resultCode = -1;
    r->done = false;
}

bool owl_ldap_search_result_add_entry(OwlLdapSearchResult* r,
                                      const unsigned char* content,
                                      size_t len) {
    if (r->entry_count == r->entry_cap) {
        size_t nc = r->entry_cap ? r->entry_cap * 2 : 4;
        OwlLdapSearchEntry* ne = (OwlLdapSearchEntry*)realloc(r->entries,
            nc * sizeof(OwlLdapSearchEntry));
        if (ne == NULL) return false;
        r->entries = ne;
        r->entry_cap = nc;
    }
    /* Le slot n'est comptabilise que si le parse reussit : en cas d'echec
     * il reste non initialise mais hors de entry_count, donc jamais libere. */
    if (!owl_ldap_parse_search_entry(content, len,
                                     &r->entries[r->entry_count]))
        return false;
    r->entry_count++;
    return true;
}

bool owl_ldap_search_result_set_done(OwlLdapSearchResult* r,
                                     const unsigned char* content,
                                     size_t len) {
    OwlLdapResult res;
    if (!owl_ldap_parse_result(content, len, &res)) return false;
    r->resultCode = res.resultCode;
    r->done = true;
    return true;
}

void owl_ldap_search_result_free(OwlLdapSearchResult* r) {
    if (r == NULL) return;
    for (size_t i = 0; i < r->entry_count; i++) {
        owl_ldap_search_entry_free(&r->entries[i]);
    }
    free(r->entries);
    r->entries = NULL;
    r->entry_count = 0;
    r->entry_cap = 0;
}
