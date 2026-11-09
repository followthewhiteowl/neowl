// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_ldap_filter_h
#define owl_ldap_filter_h

/* ================================================================
 * LDAP Filter — compilation d'une chaine de filtre RFC 4515 en son
 * encodage Filter BER (RFC 4511 section 4.5.1).
 *
 * Entree : une chaine de filtre LDAP (RFC 4515) telle qu'on l'ecrit a
 * la main : "(cn=bob)", "(&(objectClass=user)(!(cn=andy)))",
 * "(cn=sm*)", "(mail=*)", "(cn=a*b*c)", "(sn~=kurd)".
 *
 * Sortie : le bloc BER du Filter CHOICE (RFC 4511), ecrit dans un
 * OwlBerWriter. Le resultat est un bloc BER AUTONOME : (data, len) via
 * owl_ber_writer_data / owl_ber_writer_len se passe tel quel comme
 * filtre opaque a owl_ldap_encode_search_request (le lot PDU a deja
 * cable l'injection opaque — on garde ce design, la couche BER n'expose
 * aucune primitive supplementaire).
 *
 * Le Filter est un CHOICE a encodage IMPLICIT : le tag CONTEXT remplace
 * le tag universel. Correspondances (RFC 4511 section 4.5.1) :
 *   and           [0] 0xA0  SET OF Filter (>=1)
 *   or            [1] 0xA1  SET OF Filter (>=1)
 *   not           [2] 0xA2  un seul Filter
 *   equalityMatch [3] 0xA3  AttributeValueAssertion (deux OCTET STRING)
 *   substrings    [4] 0xA4  SubstringFilter
 *   greaterOrEqual[5] 0xA5  AttributeValueAssertion
 *   lessOrEqual   [6] 0xA6  AttributeValueAssertion
 *   present       [7] 0x87  AttributeDescription (octets bruts, primitif)
 *   approxMatch   [8] 0xA8  AttributeValueAssertion
 *
 * Entree potentiellement MALFORMEE (saisie utilisateur) : parentheses
 * desequilibrees, filtre vide, combinateur sans operande, operateur
 * manquant, echappement tronque, imbrication abusive -> rejet propre
 * (false), jamais d'abort, pas de debordement, recursion bornee.
 * ================================================================ */

#include "common.h"
#include "ldap_ber.h"

/* Profondeur d'imbrication maximale d'un filtre (and/or/not emboites).
 * Garde-fou contre une chaine pathologique qui ferait exploser la pile. */
#define OWL_LDAP_FILTER_MAX_DEPTH 32

/* Compile la chaine de filtre RFC 4515 filter_str (UTF-8 nul-terminee)
 * en son bloc Filter BER, ecrit dans w.
 *
 * Precondition : w fraichement initialise (vide). Sur succes, le writer
 * contient exactement le bloc Filter (son TLV complet, tag CONTEXT en
 * tete), pret a servir de filtre opaque a owl_ldap_encode_search_request.
 *
 * Renvoie false si filter_str est malforme (le writer peut alors
 * contenir un fragment : le caller le libere via owl_ber_writer_free et
 * ne l'utilise pas) ou sur OOM. Jamais d'abort. */
bool owl_ldap_filter_encode(OwlBerWriter* w, const char* filter_str);

#endif /* owl_ldap_filter_h */
