// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "ldap_ber.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/asn1write.h>

#define OWL_BER_WRITER_INIT_CAP  256

/* ================================================================
 * Buffer d'ecriture a rebours
 * ================================================================ */

void owl_ber_writer_init(OwlBerWriter* w) {
    w->buf = NULL;
    w->cap = 0;
    w->p = NULL;
    w->oom = false;
}

void owl_ber_writer_free(OwlBerWriter* w) {
    free(w->buf);
    w->buf = NULL;
    w->cap = 0;
    w->p = NULL;
    w->oom = false;
}

size_t owl_ber_writer_len(const OwlBerWriter* w) {
    if (w->buf == NULL) return 0;
    return (size_t)((w->buf + w->cap) - w->p);
}

const unsigned char* owl_ber_writer_data(const OwlBerWriter* w) {
    if (w->buf == NULL || owl_ber_writer_len(w) == 0) return NULL;
    return w->p;
}

/* Garantit au moins `extra` octets libres a l'avant du buffer (entre
 * buf et p), de sorte que les write_* de mbedtls, qui bornent contre
 * `start = buf`, ne puissent pas echouer. Relocalise le contenu deja
 * ecrit a la queue du nouveau bloc. Renvoie false (et condamne le
 * writer) sur echec d'allocation. */
static bool ber_ensure(OwlBerWriter* w, size_t extra) {
    if (w->oom) return false;

    if (w->buf == NULL) {
        size_t cap = OWL_BER_WRITER_INIT_CAP;
        while (cap < extra) cap *= 2;
        w->buf = (unsigned char*)malloc(cap);
        if (w->buf == NULL) { w->oom = true; return false; }
        w->cap = cap;
        w->p = w->buf + cap;  /* vide : tout l'espace est libre a l'avant */
        return true;
    }

    size_t front = (size_t)(w->p - w->buf);  /* espace libre courant */
    if (front >= extra) return true;

    size_t used = owl_ber_writer_len(w);
    size_t newcap = w->cap * 2;
    while (newcap - used < extra) newcap *= 2;

    unsigned char* nb = (unsigned char*)malloc(newcap);
    if (nb == NULL) { w->oom = true; return false; }
    /* Recopie le contenu ecrit a la QUEUE : le sens d'ecriture (vers le
     * debut) est preserve, l'espace neuf s'ouvre a l'avant. */
    memcpy(nb + newcap - used, w->p, used);
    free(w->buf);
    w->buf = nb;
    w->cap = newcap;
    w->p = nb + newcap - used;
    return true;
}

/* Marge de securite pour un tag (1 octet) + une longueur (jusqu'a 5
 * octets en forme longue sur 4 octets de taille) + petit contenu. */
#define BER_TLV_OVERHEAD  8

bool owl_ber_write_integer(OwlBerWriter* w, int val) {
    if (!ber_ensure(w, BER_TLV_OVERHEAD)) return false;
    int r = mbedtls_asn1_write_int(&w->p, w->buf, val);
    if (r < 0) { w->oom = true; return false; }
    return true;
}

bool owl_ber_write_boolean(OwlBerWriter* w, bool val) {
    if (!ber_ensure(w, BER_TLV_OVERHEAD)) return false;
    int r = mbedtls_asn1_write_bool(&w->p, w->buf, val ? 1 : 0);
    if (r < 0) { w->oom = true; return false; }
    return true;
}

bool owl_ber_write_enumerated(OwlBerWriter* w, int val) {
    if (!ber_ensure(w, BER_TLV_OVERHEAD)) return false;
    int r = mbedtls_asn1_write_enum(&w->p, w->buf, val);
    if (r < 0) { w->oom = true; return false; }
    return true;
}

bool owl_ber_write_octet_string(OwlBerWriter* w,
                                const unsigned char* data, size_t len) {
    if (!ber_ensure(w, len + BER_TLV_OVERHEAD)) return false;
    int r = mbedtls_asn1_write_octet_string(&w->p, w->buf, data, len);
    if (r < 0) { w->oom = true; return false; }
    return true;
}

bool owl_ber_wrap(OwlBerWriter* w, unsigned char tag, size_t content_len) {
    if (!ber_ensure(w, BER_TLV_OVERHEAD)) return false;
    /* Ordre backward : la longueur d'abord (elle se retrouve derriere le
     * tag), puis le tag. mbedtls borne chaque ecriture contre w->buf. */
    int r = mbedtls_asn1_write_len(&w->p, w->buf, content_len);
    if (r < 0) { w->oom = true; return false; }
    r = mbedtls_asn1_write_tag(&w->p, w->buf, tag);
    if (r < 0) { w->oom = true; return false; }
    return true;
}

bool owl_ber_wrap_sequence(OwlBerWriter* w, size_t content_len) {
    return owl_ber_wrap(w,
        owl_ber_tag(OWL_BER_CLASS_UNIVERSAL, true, MBEDTLS_ASN1_SEQUENCE),
        content_len);
}

/* ================================================================
 * Enveloppe LDAPMessage
 * ================================================================ */

bool owl_ldap_message_finish(OwlBerWriter* w, int message_id) {
    /* Le protocolOp occupe deja tout le writer. On prepend l'INTEGER
     * messageID devant lui : le contenu de la SEQUENCE est alors tout ce
     * qui est ecrit (messageID ++ protocolOp). */
    if (!owl_ber_write_integer(w, message_id)) return false;
    return owl_ber_wrap_sequence(w, owl_ber_writer_len(w));
}

/* ================================================================
 * Parsing de l'en-tete LDAPMessage
 * ================================================================ */

bool owl_ldap_message_parse(const unsigned char* data, size_t len,
                            OwlLdapMessageHeader* out) {
    if (data == NULL || out == NULL) return false;

    /* mbedtls travaille sur des pointeurs non-const ; on ne modifie pas
     * le contenu, seulement le curseur. */
    unsigned char* p = (unsigned char*)data;
    const unsigned char* end = data + len;

    /* SEQUENCE englobante. get_tag borne la longueur contre `end` et
     * rejette l'indefinie (get_len : n==0 => INVALID_LENGTH). */
    size_t seq_len = 0;
    int r = mbedtls_asn1_get_tag(&p, end, &seq_len,
        MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    if (r != 0) return false;

    /* Le contenu du LDAPMessage est strictement borne par la SEQUENCE :
     * on ne lit jamais au-dela, meme si le buffer source est plus long. */
    const unsigned char* seq_end = p + seq_len;

    /* messageID INTEGER. */
    int message_id = 0;
    r = mbedtls_asn1_get_int(&p, seq_end, &message_id);
    if (r != 0) return false;

    /* protocolOp : tag arbitraire (APPLICATION/CONTEXT). On lit le tag
     * et la longueur a la main, car le tag n'est pas connu d'avance. */
    if (p >= seq_end) return false;
    unsigned char op_tag = *p;
    p++;
    size_t op_len = 0;
    r = mbedtls_asn1_get_len(&p, seq_end, &op_len);
    if (r != 0) return false;  /* longueur indefinie ou debordante rejetee */

    out->messageID = message_id;
    out->protocolOpTag = op_tag;
    out->opContent = p;
    out->opLen = op_len;
    /* Les controls optionnels (tag [0]) qui suivraient le protocolOp
     * sont intentionnellement ignores en lecture (cf. RFC 4511 4.1.1). */
    return true;
}
