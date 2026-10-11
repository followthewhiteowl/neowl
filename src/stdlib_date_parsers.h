// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_date_parsers.h - parsers temporels partages compile/runtime.
 *
 *  : expose les parsers de litteraux temporels utilises par les
 * fonctions de coerce runtime (fn_date_coerce, fn_heure_coerce,
 * fn_dateheure_coerce) au compiler. Single-parser invariant strict :
 * le compile-time literal filter appelle EXACTEMENT le meme parser
 * que la coerce runtime. Pas de duplication d'heuristique = pas de
 * drift compile/runtime possible.
 *
 *  (lot 1) : ajoute le dispatcher multi-format Date Q5
 * (`owl_parse_date_multi`) qui accepte AAAAMMJJ + JJ/MM/AAAA +
 * JJ-MM-AAAA + AAAA-MM-JJ + AAAA/MM/JJ. La coerce runtime et le
 * compile filter passent par ce dispatcher pour symetrie totale
 * coerce <-> ChaineVersDate (cf.  §migration, decision Q5).
 *
 * Symetrique de la verification deja en place pour Duree
 * (check_duree_string_literal dans compiler.c). */

#ifndef OWL_STDLIB_DATE_PARSERS_H
#define OWL_STDLIB_DATE_PARSERS_H

#include <stdbool.h>

/* Parse "AAAAMMJJ" strict 8 chiffres. */
bool owl_parse_date_string(const char* s, int len, int* y, int* m, int* d);

/* Parse "HHMMSS[CC]" ou "HH:MM[:SS[.CC|:CC]]". */
bool owl_parse_time_string_cs(const char* s, int len,
                              int* h, int* m, int* sec, int* cs);

/* Parse "AAAA[-/]MM[-/]JJ[ T]HH:MM[:SS]". */
bool owl_parse_dateheure_iso(const char* s, int len,
                             int* y, int* m, int* d,
                             int* hh, int* mm, int* ss);

/*  Q5 — Dispatcher multi-format Date. Accepte :
 *   - AAAAMMJJ            (8 chiffres strict)
 *   - JJ/MM/AAAA          (10 chars, separateur slash)
 *   - JJ-MM-AAAA          (10 chars, separateur tiret)
 *   - AAAA-MM-JJ          (10 chars, ISO 8601)
 *   - AAAA/MM/JJ          (10 chars, ISO slash)
 *
 * Distingue format invalide (parsing echoue / longueur inadaptee) de
 * borne calendaire invalide (mois>12, jour>days_in_month) via outErr.
 * outErr peut etre NULL si le caller ne distingue pas.
 *
 * Valeurs outErr :
 *   0 = succes (return true)
 *   1 = format invalide (longueur / digits / separateurs)
 *   2 = borne calendaire invalide (mois ou jour hors-borne) */
#define OWL_DATE_PARSE_OK              0
#define OWL_DATE_PARSE_ERR_FORMAT      1
#define OWL_DATE_PARSE_ERR_BOUNDS      2

bool owl_parse_date_multi(const char* s, int len,
                          int* y, int* m, int* d, int* outErr);

/*  lot 1 — variantes BOUNDS-aware pour Heure et DateHeure.
 * Meme contrat outErr que owl_parse_date_multi. Utilisees par le
 * compile filter pour differencier E_SEM_*_LITERAL_FORMAT vs
 * E_SEM_*_LITERAL_BOUNDS. */
bool owl_parse_time_string_cs_ex(const char* s, int len,
                                 int* h, int* m, int* sec, int* cs,
                                 int* outErr);

bool owl_parse_dateheure_iso_ex(const char* s, int len,
                                int* y, int* m, int* d,
                                int* hh, int* mm, int* ss,
                                int* outErr);

/*  Q5-bis — Dispatcher Duree canon (single source of truth).
 *
 * Format strict : [+/-]<J>HHMMSSCC ou [+/-]<J>HHMMSSCCC
 *   - Signe obligatoire ('+' ou '-')
 *   - J : nombre de jours, 1+ chiffres greedy
 *   - HH MM SS : 2 chiffres chacun, bornes 0-23 / 0-59 / 0-59
 *   - CC : centiemes (2 chiffres, x10 -> ms)
 *   - CCC : millisecondes (3 chiffres directes)
 *   - mode CC actif si reste apres J = 8 chars
 *   - mode CCC actif si reste apres J = 9 chars
 *   - autres longueurs => format invalide
 *
 * Distingue format invalide (signe manquant, non-digits, longueur) de
 * borne calendaire (HH/MM/SS hors-borne) via outErr.
 *
 * Total ms : J*86400000 + HH*3600000 + MM*60000 + SS*1000 + ms_sub.
 * Signe applique sur le total.
 *
 * Cf.  §migration : rupture avec  historique strict
 * 10 digits JHHMMSSCCC (J=1). Le nouveau dispatcher interprete les
 * memes 10 digits comme JJHHMMSSCC (mode centieme). */
#include <stdint.h>
bool owl_parse_duration_multi(const char* s, int len,
                              int64_t* out_ms, int* outErr);

/*  — moteur de masque date partagé (DÉFINI dans stdlib_date.c).
 * Le champ de saisie typSaisieDate (MasqueAffichage/DisplayMask) pilote via ces
 * deux points d'entrée le format d'affichage et la relecture selon un gabarit de
 * jetons (AAAA/MM/JJ, MMMM, JJJ...). Réutilise le moteur interne de stdlib_date
 * (parse_date_with_format + format_date_time_tokens) — pas de réimplémentation.
 *
 * owl_date_format_with_mask : formate (y,m,d) selon fmt dans out (null-terminé),
 *   retourne la longueur écrite.
 * owl_date_parse_with_mask  : parse s DIRIGÉ par fmt (ordre de jetons imposé),
 *   retourne false si le texte ne correspond pas exactement au masque. */
int owl_date_format_with_mask(char* out, int outsz,
                              const char* fmt, int flen,
                              int y, int m, int d);
bool owl_date_parse_with_mask(const char* s, int slen,
                              const char* fmt, int flen,
                              int* y, int* m, int* d);

/*  — moteur de masque HEURE partagé (DÉFINI dans stdlib_date.c).
 * Miroir des wrappers date mais côté heure (jetons horaires HH / MI|mm / SS /
 * CC, hasTime=true) : le champ de saisie typSaisieHeure (MasqueAffichage) pilote
 * via ces deux points d'entrée le format d'affichage et la relecture. Réutilise
 * le moteur interne de stdlib_date (format_date_time_tokens + parse dédié heure)
 * — pas de réimplémentation. CC = centièmes (0-99).
 *
 * owl_time_format_with_mask : formate (hh,mi,sec,cs) selon fmt dans out
 *   (null-terminé), retourne la longueur écrite.
 * owl_time_parse_with_mask  : parse s DIRIGÉ par fmt (ordre de jetons imposé),
 *   retourne false si le texte ne correspond pas exactement au masque. */
int owl_time_format_with_mask(char* out, int outsz,
                              const char* fmt, int flen,
                              int hh, int mi, int sec, int cs);
bool owl_time_parse_with_mask(const char* s, int slen,
                              const char* fmt, int flen,
                              int* hh, int* mi, int* sec, int* cs);

/*  — moteur de masque DURÉE dédié (DÉFINI dans stdlib_date.c). Distinct
 * du parseur durée canon : HH n'est PAS plafonné à 23 (une durée de 99 h
 * existe) et le vocabulaire est celui de la durée (MM = minutes, pas mois).
 * Jetons : J (jours variable width) / JJ / HH / MM / SS / CC (centièmes) / CCC
 * (millièmes). Alimente le champ de saisie typSaisieDurée (relecture Qt-first +
 * affichage). owl_duree_parse_with_mask retourne false si le texte ne
 * correspond pas exactement au masque ; owl_duree_format_with_mask retourne la
 * longueur écrite. */
bool owl_duree_parse_with_mask(const char* s, int slen,
                               const char* fmt, int flen, int64_t* outMs);
int  owl_duree_format_with_mask(char* out, int outsz,
                                const char* fmt, int flen, int64_t totalMs);

#endif /* OWL_STDLIB_DATE_PARSERS_H */
