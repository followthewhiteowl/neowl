// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_ldap_ber_h
#define owl_ldap_ber_h

/* ================================================================
 * LDAP BER — couche d'encodage/decodage specialisee LDAP
 *
 * Le wire LDAP (RFC 4511) EST du BER : tag / longueur / contenu, avec
 * les memes primitives UNIVERSAL que n'importe quel ASN.1 (INTEGER,
 * BOOLEAN, ENUMERATED, OCTET STRING, SEQUENCE). LDAP n'ajoute par-dessus
 * que des tags de classe APPLICATION (le protocolOp : BindRequest,
 * SearchRequest...) et CONTEXT (les champs optionnels ou typeless).
 *
 * Cette couche s'appuie sur les primitives ASN.1 de mbedtls
 * (mbedtls_asn1_write_* / mbedtls_asn1_get_*) et n'ajoute que :
 *   - un buffer d'encodage adapte au sens d'ecriture de mbedtls
 *     (les write_* ecrivent A REBOURS, de la fin vers le debut) ;
 *   - l'emission de tags arbitraires (classes APPLICATION / CONTEXT,
 *     bit constructed) ;
 *   - l'enveloppe LDAPMessage (SEQUENCE { messageID, protocolOp }) et
 *     sa lecture (exposer le tag du protocolOp + un curseur sur son
 *     contenu, pour les couches PDU au-dessus).
 *
 * C'est une couche PURE encoder/decoder : pas de socket, pas de state
 * machine, pas de VM. Entree reseau non fiable => bornage strict via
 * mbedtls_asn1_get_len/get_tag (longueur definie seulement, l'indefinie
 * est rejetee), rejet propre des PDU malformes, jamais d'abort().
 *
 * Reference : RFC 4511 section 4.1 (LDAPMessage, types).
 * ================================================================ */

#include "common.h"

#include <mbedtls/asn1.h>

/* ================================================================
 * Classes et formes de tag BER (ITU X.690 section 8.1.2.2)
 *
 * Un octet de tag = [class:2 bits][P/C:1 bit][tag number:5 bits].
 * LDAP n'emet par-dessus les primitives UNIVERSAL que de l'APPLICATION
 * et du CONTEXT ; le bit constructed distingue un conteneur d'un
 * contenu primitif.
 * ================================================================ */

#define OWL_BER_CLASS_UNIVERSAL    0x00
#define OWL_BER_CLASS_APPLICATION  0x40
#define OWL_BER_CLASS_CONTEXT      0x80
#define OWL_BER_CLASS_PRIVATE      0xC0
#define OWL_BER_CONSTRUCTED        0x20

/* Compose un octet de tag. number doit tenir sur 5 bits (0..30) ; le
 * tag number long form (>= 31) n'est pas utilise par LDAP. */
static inline unsigned char owl_ber_tag(unsigned char cls,
                                        bool constructed,
                                        unsigned char number) {
    return (unsigned char)(cls | (constructed ? OWL_BER_CONSTRUCTED : 0)
                               | (number & 0x1F));
}

/* ================================================================
 * Buffer d'encodage (ecriture a rebours, style mbedtls)
 *
 * mbedtls_asn1_write_* ecrit de la fin du buffer vers le debut et met a
 * jour un pointeur courant. On encapsule ca dans un buffer croissant :
 * le contenu deja ecrit occupe [p, buf+cap), l'espace libre est a
 * l'avant [buf, p). Ecrire = prepender. Quand l'avant est plein, on
 * realloue (x2) et on recopie le contenu ecrit a la QUEUE du nouveau
 * buffer (le sens d'ecriture reste "vers le debut").
 * ================================================================ */

typedef struct {
    unsigned char* buf;   /* bloc heap possede */
    size_t         cap;   /* capacite totale du bloc */
    unsigned char* p;     /* position courante d'ecriture (descend) */
    bool           oom;   /* une allocation a echoue : writer condamne */
} OwlBerWriter;

void   owl_ber_writer_init(OwlBerWriter* w);
void   owl_ber_writer_free(OwlBerWriter* w);

/* Nombre d'octets deja ecrits (= longueur du message en cours). */
size_t owl_ber_writer_len(const OwlBerWriter* w);

/* Pointeur sur le premier octet ecrit (debut du message). Valable tant
 * que le writer n'est pas re-ecrit ni libere. NULL si writer vide. */
const unsigned char* owl_ber_writer_data(const OwlBerWriter* w);

/* ---- Writers de primitives UNIVERSAL ----
 * Chacun prepend un TLV complet. Renvoie false sur OOM/overflow (le
 * writer passe alors en etat oom et tout appel suivant echoue). */

/* INTEGER (0x02). val non negatif (messageID, versions, limites LDAP). */
bool owl_ber_write_integer(OwlBerWriter* w, int val);

/* BOOLEAN (0x01). */
bool owl_ber_write_boolean(OwlBerWriter* w, bool val);

/* ENUMERATED (0x0A). */
bool owl_ber_write_enumerated(OwlBerWriter* w, int val);

/* OCTET STRING (0x04). Les octets sont ecrits tels quels : pour une
 * LDAPString / LDAPDN, passer l'UTF-8 brut (aucun re-encodage). */
bool owl_ber_write_octet_string(OwlBerWriter* w,
                                const unsigned char* data, size_t len);

/* ---- Enrobage : longueur + tag arbitraire ----
 *
 * Idiome backward : le caller ecrit d'abord le CONTENU (il atterrit a
 * l'avant), puis prepend longueur+tag. content_len = nombre d'octets de
 * contenu deja ecrits depuis le mark pris avant.
 *
 *   size_t mark = owl_ber_writer_len(w);
 *   ... ecrire le contenu ...
 *   owl_ber_wrap(w, tag, owl_ber_writer_len(w) - mark);
 *
 * owl_ber_wrap pose un tag arbitraire (UNIVERSAL SEQUENCE, APPLICATION,
 * CONTEXT...). owl_ber_wrap_sequence est le raccourci SEQUENCE. */
bool owl_ber_wrap(OwlBerWriter* w, unsigned char tag, size_t content_len);
bool owl_ber_wrap_sequence(OwlBerWriter* w, size_t content_len);

/* ================================================================
 * Enveloppe LDAPMessage (RFC 4511 section 4.1.1)
 *
 *   LDAPMessage ::= SEQUENCE {
 *       messageID   INTEGER (0 .. maxInt),
 *       protocolOp  CHOICE { bindRequest, searchRequest, ... },
 *       controls    [0] Controls OPTIONAL }
 * ================================================================ */

/* Emballe le protocolOp DEJA encode dans le writer (son TLV complet,
 * tag APPLICATION en tete) en un LDAPMessage complet : prepend
 * l'INTEGER messageID puis enrobe le tout dans une SEQUENCE UNIVERSAL.
 * Precondition : le writer contient exactement le protocolOp.
 * Renvoie false sur OOM. message_id non negatif. */
bool owl_ldap_message_finish(OwlBerWriter* w, int message_id);

/* En-tete LDAPMessage lu cote reception. opContent/opLen pointent dans
 * le buffer source (zero copie) : valides tant que la source vit. */
typedef struct {
    int                  messageID;
    unsigned char        protocolOpTag;  /* ex. 0x61 = BindResponse */
    const unsigned char* opContent;      /* contenu du protocolOp */
    size_t               opLen;          /* longueur du contenu */
} OwlLdapMessageHeader;

/* Parse l'en-tete d'un LDAPMessage : SEQUENCE { messageID INTEGER,
 * protocolOp <tag> ... }. Expose le messageID, le tag du protocolOp et
 * un curseur sur son contenu (les controls optionnels sont ignores).
 * Bornage strict ; rejet propre (false) des PDU malformes : tag
 * inattendu, longueur qui deborde, longueur indefinie. Jamais d'abort.
 * Renvoie true sur succes, out rempli. */
bool owl_ldap_message_parse(const unsigned char* data, size_t len,
                            OwlLdapMessageHeader* out);

#endif /* owl_ldap_ber_h */
