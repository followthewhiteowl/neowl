// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_email_h
#define owl_stdlib_email_h

/* Fondations courriel (, Lot 0) — types de donnees declaratifs, sans
 * comportement reseau ni MIME. Trois types reifies :
 *
 *   Email            (Communication/Email/1000018713) : message a envoyer
 *   emailSessionSMTP (1000018765, EN=emailSMTPSession) : connexion SMTP sortante
 *   emailAttache     (1000018752, EN=emailAttach)      : piece jointe
 *
 * + constantes de priorite, de confidentialite et d'option de connexion. Les
 * encodeurs, la construction/import Source (.eml) et le transport SMTP arrivent
 * dans les lots ulterieurs du chantier. Ce module ne fait qu'enregistrer les
 * structures declarables + lisibles et leurs valeurs par defaut.
 *
 * La surface OWL (noms de champs, valeurs par defaut) est alignee sur le canon
 * canon (doc Communication/Email). Les formes EN des champs ne figurent pas dans
 * la doc FR : elles sont derivees par convention alignee sur les types voisins
 * (cf. mqttSession), a confronter au doc anglais si divergence. */

void stdlib_email_init(void);

#endif /* owl_stdlib_email_h */
