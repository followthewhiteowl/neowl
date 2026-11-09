// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_pays.c — types composites `Pays`/`Country` et `Continent`, enum
 * `continentXxx` et native `PaysListe`/`CountryList`.
 *
 * Modele de donnees :
 *   - Continent : type composite a deux proprietes (..Nom chaine, ..Code chaine
 *     de 2 lettres). Sept continents (modele geographique classique).
 *   - Pays      : type composite expose en acces propriete (double point) :
 *     ..Nom, ..CodeISO2, ..CodeISO3, ..CodeISONum (entier), ..Continent (un
 *     Continent) et ..PrefixeTelephone.
 *   - PaysListe([continent]) : rend un tableau de Pays, filtre optionnel sur
 *     une constante continentXxx (ou son code 2 lettres en chaine).
 *
 * Donnees embarquees : liste ISO 3166-1 (codes alpha-2/alpha-3/numeriques),
 * rattachement continental d'apres le decoupage M49 de la CLDR, indicatifs
 * telephoniques UIT E.164. Les codes et noms courts sont des faits publics ;
 * la table est batie ici en C statique (aucun generateur externe). Les
 * libelles i18n sont pragmatiques et ne valent pas position de reconnaissance
 * politique.
 *
 * Les libelles accentues sont en UTF-8 brut dans les litteraux (clang/zig-cc
 * lit la source en UTF-8 ; octets preserves a l'identique dans le binaire).
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "object.h"
#include "vm.h"
#include "sentinels.h"

#include <string.h>

/* ================================================================
 * Continents — index interne 0..6, sentinelles continentXxx
 * ================================================================ */

/* Offset 108 déjà pris par OWL_LOG_BASE (stdlib_log.c) ; 109/110 par OAuth.
 * 111 = premier libre (OWL_SENTINEL_OFFSET est fonction pure de n, collision
 * bit-à-bit sinon : continentAfrique == logNiveauDebug). */
#define OWL_CONTINENT_BASE   OWL_SENTINEL_OFFSET(111)
#define OWL_CONTINENT_AFRIQUE        (OWL_CONTINENT_BASE + 0)
#define OWL_CONTINENT_AMERIQUE_NORD  (OWL_CONTINENT_BASE + 1)
#define OWL_CONTINENT_AMERIQUE_SUD   (OWL_CONTINENT_BASE + 2)
#define OWL_CONTINENT_ANTARCTIQUE    (OWL_CONTINENT_BASE + 3)
#define OWL_CONTINENT_ASIE           (OWL_CONTINENT_BASE + 4)
#define OWL_CONTINENT_EUROPE         (OWL_CONTINENT_BASE + 5)
#define OWL_CONTINENT_OCEANIE        (OWL_CONTINENT_BASE + 6)

/* Index 0..6 alignes sur l'ordre des constantes ci-dessus. */
enum {
    CONT_AF = 0, CONT_NA, CONT_SA, CONT_AN, CONT_AS, CONT_EU, CONT_OC,
    CONT_COUNT
};

typedef struct {
    const char* code;    /* code 2 lettres, ex "EU" */
    const char* nomFR;
    const char* nomEN;
    int32_t     sentinel;
} ContinentEntry;

static const ContinentEntry CONTINENTS[CONT_COUNT] = {
    { "AF", "Afrique",            "Africa",        OWL_CONTINENT_AFRIQUE },
    { "NA", "Amérique du Nord",   "North America", OWL_CONTINENT_AMERIQUE_NORD },
    { "SA", "Amérique du Sud",    "South America", OWL_CONTINENT_AMERIQUE_SUD },
    { "AN", "Antarctique",        "Antarctica",    OWL_CONTINENT_ANTARCTIQUE },
    { "AS", "Asie",               "Asia",          OWL_CONTINENT_ASIE },
    { "EU", "Europe",             "Europe",        OWL_CONTINENT_EUROPE },
    { "OC", "Océanie",            "Oceania",       OWL_CONTINENT_OCEANIE },
};

/* ================================================================
 * Table des pays (ISO 3166-1)
 * ================================================================ */

typedef struct {
    const char* iso2;
    const char* iso3;
    int         num;        /* code numerique ISO 3166-1, sans zero de tete */
    int         continent;  /* index CONT_* */
    const char* phone;      /* indicatif E.164, ex "+33" */
    const char* nomFR;
    const char* nomEN;
} PaysEntry;

static const PaysEntry PAYS[] = {
    { "AD", "AND", 20,  CONT_EU, "+376",  "Andorre", "Andorra" },
    { "AE", "ARE", 784, CONT_AS, "+971",  "Émirats arabes unis", "United Arab Emirates" },
    { "AF", "AFG", 4,   CONT_AS, "+93",   "Afghanistan", "Afghanistan" },
    { "AG", "ATG", 28,  CONT_NA, "+1268", "Antigua-et-Barbuda", "Antigua and Barbuda" },
    { "AI", "AIA", 660, CONT_NA, "+1264", "Anguilla", "Anguilla" },
    { "AL", "ALB", 8,   CONT_EU, "+355",  "Albanie", "Albania" },
    { "AM", "ARM", 51,  CONT_AS, "+374",  "Arménie", "Armenia" },
    { "AO", "AGO", 24,  CONT_AF, "+244",  "Angola", "Angola" },
    { "AQ", "ATA", 10,  CONT_AN, "+672",  "Antarctique", "Antarctica" },
    { "AR", "ARG", 32,  CONT_SA, "+54",   "Argentine", "Argentina" },
    { "AS", "ASM", 16,  CONT_OC, "+1684", "Samoa américaines", "American Samoa" },
    { "AT", "AUT", 40,  CONT_EU, "+43",   "Autriche", "Austria" },
    { "AU", "AUS", 36,  CONT_OC, "+61",   "Australie", "Australia" },
    { "AW", "ABW", 533, CONT_NA, "+297",  "Aruba", "Aruba" },
    { "AX", "ALA", 248, CONT_EU, "+358",  "Åland", "Aland Islands" },
    { "AZ", "AZE", 31,  CONT_AS, "+994",  "Azerbaïdjan", "Azerbaijan" },
    { "BA", "BIH", 70,  CONT_EU, "+387",  "Bosnie-Herzégovine", "Bosnia and Herzegovina" },
    { "BB", "BRB", 52,  CONT_NA, "+1246", "Barbade", "Barbados" },
    { "BD", "BGD", 50,  CONT_AS, "+880",  "Bangladesh", "Bangladesh" },
    { "BE", "BEL", 56,  CONT_EU, "+32",   "Belgique", "Belgium" },
    { "BF", "BFA", 854, CONT_AF, "+226",  "Burkina Faso", "Burkina Faso" },
    { "BG", "BGR", 100, CONT_EU, "+359",  "Bulgarie", "Bulgaria" },
    { "BH", "BHR", 48,  CONT_AS, "+973",  "Bahreïn", "Bahrain" },
    { "BI", "BDI", 108, CONT_AF, "+257",  "Burundi", "Burundi" },
    { "BJ", "BEN", 204, CONT_AF, "+229",  "Bénin", "Benin" },
    { "BL", "BLM", 652, CONT_NA, "+590",  "Saint-Barthélemy", "Saint Barthelemy" },
    { "BM", "BMU", 60,  CONT_NA, "+1441", "Bermudes", "Bermuda" },
    { "BN", "BRN", 96,  CONT_AS, "+673",  "Brunei", "Brunei" },
    { "BO", "BOL", 68,  CONT_SA, "+591",  "Bolivie", "Bolivia" },
    { "BQ", "BES", 535, CONT_NA, "+599",  "Pays-Bas caribéens", "Caribbean Netherlands" },
    { "BR", "BRA", 76,  CONT_SA, "+55",   "Brésil", "Brazil" },
    { "BS", "BHS", 44,  CONT_NA, "+1242", "Bahamas", "Bahamas" },
    { "BT", "BTN", 64,  CONT_AS, "+975",  "Bhoutan", "Bhutan" },
    { "BV", "BVT", 74,  CONT_AN, "+47",   "Île Bouvet", "Bouvet Island" },
    { "BW", "BWA", 72,  CONT_AF, "+267",  "Botswana", "Botswana" },
    { "BY", "BLR", 112, CONT_EU, "+375",  "Biélorussie", "Belarus" },
    { "BZ", "BLZ", 84,  CONT_NA, "+501",  "Belize", "Belize" },
    { "CA", "CAN", 124, CONT_NA, "+1",    "Canada", "Canada" },
    { "CC", "CCK", 166, CONT_OC, "+61",   "Îles Cocos", "Cocos (Keeling) Islands" },
    { "CD", "COD", 180, CONT_AF, "+243",  "République démocratique du Congo", "DR Congo" },
    { "CF", "CAF", 140, CONT_AF, "+236",  "République centrafricaine", "Central African Republic" },
    { "CG", "COG", 178, CONT_AF, "+242",  "Congo", "Congo" },
    { "CH", "CHE", 756, CONT_EU, "+41",   "Suisse", "Switzerland" },
    { "CI", "CIV", 384, CONT_AF, "+225",  "Côte d'Ivoire", "Cote d'Ivoire" },
    { "CK", "COK", 184, CONT_OC, "+682",  "Îles Cook", "Cook Islands" },
    { "CL", "CHL", 152, CONT_SA, "+56",   "Chili", "Chile" },
    { "CM", "CMR", 120, CONT_AF, "+237",  "Cameroun", "Cameroon" },
    { "CN", "CHN", 156, CONT_AS, "+86",   "Chine", "China" },
    { "CO", "COL", 170, CONT_SA, "+57",   "Colombie", "Colombia" },
    { "CR", "CRI", 188, CONT_NA, "+506",  "Costa Rica", "Costa Rica" },
    { "CU", "CUB", 192, CONT_NA, "+53",   "Cuba", "Cuba" },
    { "CV", "CPV", 132, CONT_AF, "+238",  "Cap-Vert", "Cabo Verde" },
    { "CW", "CUW", 531, CONT_NA, "+599",  "Curaçao", "Curacao" },
    { "CX", "CXR", 162, CONT_OC, "+61",   "Île Christmas", "Christmas Island" },
    { "CY", "CYP", 196, CONT_AS, "+357",  "Chypre", "Cyprus" },
    { "CZ", "CZE", 203, CONT_EU, "+420",  "Tchéquie", "Czechia" },
    { "DE", "DEU", 276, CONT_EU, "+49",   "Allemagne", "Germany" },
    { "DJ", "DJI", 262, CONT_AF, "+253",  "Djibouti", "Djibouti" },
    { "DK", "DNK", 208, CONT_EU, "+45",   "Danemark", "Denmark" },
    { "DM", "DMA", 212, CONT_NA, "+1767", "Dominique", "Dominica" },
    { "DO", "DOM", 214, CONT_NA, "+1809", "République dominicaine", "Dominican Republic" },
    { "DZ", "DZA", 12,  CONT_AF, "+213",  "Algérie", "Algeria" },
    { "EC", "ECU", 218, CONT_SA, "+593",  "Équateur", "Ecuador" },
    { "EE", "EST", 233, CONT_EU, "+372",  "Estonie", "Estonia" },
    { "EG", "EGY", 818, CONT_AF, "+20",   "Égypte", "Egypt" },
    { "EH", "ESH", 732, CONT_AF, "+212",  "Sahara occidental", "Western Sahara" },
    { "ER", "ERI", 232, CONT_AF, "+291",  "Érythrée", "Eritrea" },
    { "ES", "ESP", 724, CONT_EU, "+34",   "Espagne", "Spain" },
    { "ET", "ETH", 231, CONT_AF, "+251",  "Éthiopie", "Ethiopia" },
    { "FI", "FIN", 246, CONT_EU, "+358",  "Finlande", "Finland" },
    { "FJ", "FJI", 242, CONT_OC, "+679",  "Fidji", "Fiji" },
    { "FK", "FLK", 238, CONT_SA, "+500",  "Îles Malouines", "Falkland Islands" },
    { "FM", "FSM", 583, CONT_OC, "+691",  "Micronésie", "Micronesia" },
    { "FO", "FRO", 234, CONT_EU, "+298",  "Îles Féroé", "Faroe Islands" },
    { "FR", "FRA", 250, CONT_EU, "+33",   "France", "France" },
    { "GA", "GAB", 266, CONT_AF, "+241",  "Gabon", "Gabon" },
    { "GB", "GBR", 826, CONT_EU, "+44",   "Royaume-Uni", "United Kingdom" },
    { "GD", "GRD", 308, CONT_NA, "+1473", "Grenade", "Grenada" },
    { "GE", "GEO", 268, CONT_AS, "+995",  "Géorgie", "Georgia" },
    { "GF", "GUF", 254, CONT_SA, "+594",  "Guyane", "French Guiana" },
    { "GG", "GGY", 831, CONT_EU, "+44",   "Guernesey", "Guernsey" },
    { "GH", "GHA", 288, CONT_AF, "+233",  "Ghana", "Ghana" },
    { "GI", "GIB", 292, CONT_EU, "+350",  "Gibraltar", "Gibraltar" },
    { "GL", "GRL", 304, CONT_NA, "+299",  "Groenland", "Greenland" },
    { "GM", "GMB", 270, CONT_AF, "+220",  "Gambie", "Gambia" },
    { "GN", "GIN", 324, CONT_AF, "+224",  "Guinée", "Guinea" },
    { "GP", "GLP", 312, CONT_NA, "+590",  "Guadeloupe", "Guadeloupe" },
    { "GQ", "GNQ", 226, CONT_AF, "+240",  "Guinée équatoriale", "Equatorial Guinea" },
    { "GR", "GRC", 300, CONT_EU, "+30",   "Grèce", "Greece" },
    { "GS", "SGS", 239, CONT_AN, "+500",  "Géorgie du Sud-et-les Îles Sandwich du Sud", "South Georgia" },
    { "GT", "GTM", 320, CONT_NA, "+502",  "Guatemala", "Guatemala" },
    { "GU", "GUM", 316, CONT_OC, "+1671", "Guam", "Guam" },
    { "GW", "GNB", 624, CONT_AF, "+245",  "Guinée-Bissau", "Guinea-Bissau" },
    { "GY", "GUY", 328, CONT_SA, "+592",  "Guyana", "Guyana" },
    { "HK", "HKG", 344, CONT_AS, "+852",  "Hong Kong", "Hong Kong" },
    { "HM", "HMD", 334, CONT_AN, "+672",  "Îles Heard-et-MacDonald", "Heard & McDonald Islands" },
    { "HN", "HND", 340, CONT_NA, "+504",  "Honduras", "Honduras" },
    { "HR", "HRV", 191, CONT_EU, "+385",  "Croatie", "Croatia" },
    { "HT", "HTI", 332, CONT_NA, "+509",  "Haïti", "Haiti" },
    { "HU", "HUN", 348, CONT_EU, "+36",   "Hongrie", "Hungary" },
    { "ID", "IDN", 360, CONT_AS, "+62",   "Indonésie", "Indonesia" },
    { "IE", "IRL", 372, CONT_EU, "+353",  "Irlande", "Ireland" },
    { "IL", "ISR", 376, CONT_AS, "+972",  "Israël", "Israel" },
    { "IM", "IMN", 833, CONT_EU, "+44",   "Île de Man", "Isle of Man" },
    { "IN", "IND", 356, CONT_AS, "+91",   "Inde", "India" },
    { "IO", "IOT", 86,  CONT_AS, "+246",  "Territoire britannique de l'océan Indien", "British Indian Ocean Territory" },
    { "IQ", "IRQ", 368, CONT_AS, "+964",  "Irak", "Iraq" },
    { "IR", "IRN", 364, CONT_AS, "+98",   "Iran", "Iran" },
    { "IS", "ISL", 352, CONT_EU, "+354",  "Islande", "Iceland" },
    { "IT", "ITA", 380, CONT_EU, "+39",   "Italie", "Italy" },
    { "JE", "JEY", 832, CONT_EU, "+44",   "Jersey", "Jersey" },
    { "JM", "JAM", 388, CONT_NA, "+1876", "Jamaïque", "Jamaica" },
    { "JO", "JOR", 400, CONT_AS, "+962",  "Jordanie", "Jordan" },
    { "JP", "JPN", 392, CONT_AS, "+81",   "Japon", "Japan" },
    { "KE", "KEN", 404, CONT_AF, "+254",  "Kenya", "Kenya" },
    { "KG", "KGZ", 417, CONT_AS, "+996",  "Kirghizistan", "Kyrgyzstan" },
    { "KH", "KHM", 116, CONT_AS, "+855",  "Cambodge", "Cambodia" },
    { "KI", "KIR", 296, CONT_OC, "+686",  "Kiribati", "Kiribati" },
    { "KM", "COM", 174, CONT_AF, "+269",  "Comores", "Comoros" },
    { "KN", "KNA", 659, CONT_NA, "+1869", "Saint-Christophe-et-Niévès", "Saint Kitts and Nevis" },
    { "KP", "PRK", 408, CONT_AS, "+850",  "Corée du Nord", "North Korea" },
    { "KR", "KOR", 410, CONT_AS, "+82",   "Corée du Sud", "South Korea" },
    { "KW", "KWT", 414, CONT_AS, "+965",  "Koweït", "Kuwait" },
    { "KY", "CYM", 136, CONT_NA, "+1345", "Îles Caïmans", "Cayman Islands" },
    { "KZ", "KAZ", 398, CONT_AS, "+7",    "Kazakhstan", "Kazakhstan" },
    { "LA", "LAO", 418, CONT_AS, "+856",  "Laos", "Laos" },
    { "LB", "LBN", 422, CONT_AS, "+961",  "Liban", "Lebanon" },
    { "LC", "LCA", 662, CONT_NA, "+1758", "Sainte-Lucie", "Saint Lucia" },
    { "LI", "LIE", 438, CONT_EU, "+423",  "Liechtenstein", "Liechtenstein" },
    { "LK", "LKA", 144, CONT_AS, "+94",   "Sri Lanka", "Sri Lanka" },
    { "LR", "LBR", 430, CONT_AF, "+231",  "Libéria", "Liberia" },
    { "LS", "LSO", 426, CONT_AF, "+266",  "Lesotho", "Lesotho" },
    { "LT", "LTU", 440, CONT_EU, "+370",  "Lituanie", "Lithuania" },
    { "LU", "LUX", 442, CONT_EU, "+352",  "Luxembourg", "Luxembourg" },
    { "LV", "LVA", 428, CONT_EU, "+371",  "Lettonie", "Latvia" },
    { "LY", "LBY", 434, CONT_AF, "+218",  "Libye", "Libya" },
    { "MA", "MAR", 504, CONT_AF, "+212",  "Maroc", "Morocco" },
    { "MC", "MCO", 492, CONT_EU, "+377",  "Monaco", "Monaco" },
    { "MD", "MDA", 498, CONT_EU, "+373",  "Moldavie", "Moldova" },
    { "ME", "MNE", 499, CONT_EU, "+382",  "Monténégro", "Montenegro" },
    { "MF", "MAF", 663, CONT_NA, "+590",  "Saint-Martin", "Saint Martin" },
    { "MG", "MDG", 450, CONT_AF, "+261",  "Madagascar", "Madagascar" },
    { "MH", "MHL", 584, CONT_OC, "+692",  "Îles Marshall", "Marshall Islands" },
    { "MK", "MKD", 807, CONT_EU, "+389",  "Macédoine du Nord", "North Macedonia" },
    { "ML", "MLI", 466, CONT_AF, "+223",  "Mali", "Mali" },
    { "MM", "MMR", 104, CONT_AS, "+95",   "Birmanie", "Myanmar" },
    { "MN", "MNG", 496, CONT_AS, "+976",  "Mongolie", "Mongolia" },
    { "MO", "MAC", 446, CONT_AS, "+853",  "Macao", "Macao" },
    { "MP", "MNP", 580, CONT_OC, "+1670", "Îles Mariannes du Nord", "Northern Mariana Islands" },
    { "MQ", "MTQ", 474, CONT_NA, "+596",  "Martinique", "Martinique" },
    { "MR", "MRT", 478, CONT_AF, "+222",  "Mauritanie", "Mauritania" },
    { "MS", "MSR", 500, CONT_NA, "+1664", "Montserrat", "Montserrat" },
    { "MT", "MLT", 470, CONT_EU, "+356",  "Malte", "Malta" },
    { "MU", "MUS", 480, CONT_AF, "+230",  "Maurice", "Mauritius" },
    { "MV", "MDV", 462, CONT_AS, "+960",  "Maldives", "Maldives" },
    { "MW", "MWI", 454, CONT_AF, "+265",  "Malawi", "Malawi" },
    { "MX", "MEX", 484, CONT_NA, "+52",   "Mexique", "Mexico" },
    { "MY", "MYS", 458, CONT_AS, "+60",   "Malaisie", "Malaysia" },
    { "MZ", "MOZ", 508, CONT_AF, "+258",  "Mozambique", "Mozambique" },
    { "NA", "NAM", 516, CONT_AF, "+264",  "Namibie", "Namibia" },
    { "NC", "NCL", 540, CONT_OC, "+687",  "Nouvelle-Calédonie", "New Caledonia" },
    { "NE", "NER", 562, CONT_AF, "+227",  "Niger", "Niger" },
    { "NF", "NFK", 574, CONT_OC, "+672",  "Île Norfolk", "Norfolk Island" },
    { "NG", "NGA", 566, CONT_AF, "+234",  "Nigéria", "Nigeria" },
    { "NI", "NIC", 558, CONT_NA, "+505",  "Nicaragua", "Nicaragua" },
    { "NL", "NLD", 528, CONT_EU, "+31",   "Pays-Bas", "Netherlands" },
    { "NO", "NOR", 578, CONT_EU, "+47",   "Norvège", "Norway" },
    { "NP", "NPL", 524, CONT_AS, "+977",  "Népal", "Nepal" },
    { "NR", "NRU", 520, CONT_OC, "+674",  "Nauru", "Nauru" },
    { "NU", "NIU", 570, CONT_OC, "+683",  "Niue", "Niue" },
    { "NZ", "NZL", 554, CONT_OC, "+64",   "Nouvelle-Zélande", "New Zealand" },
    { "OM", "OMN", 512, CONT_AS, "+968",  "Oman", "Oman" },
    { "PA", "PAN", 591, CONT_NA, "+507",  "Panama", "Panama" },
    { "PE", "PER", 604, CONT_SA, "+51",   "Pérou", "Peru" },
    { "PF", "PYF", 258, CONT_OC, "+689",  "Polynésie française", "French Polynesia" },
    { "PG", "PNG", 598, CONT_OC, "+675",  "Papouasie-Nouvelle-Guinée", "Papua New Guinea" },
    { "PH", "PHL", 608, CONT_AS, "+63",   "Philippines", "Philippines" },
    { "PK", "PAK", 586, CONT_AS, "+92",   "Pakistan", "Pakistan" },
    { "PL", "POL", 616, CONT_EU, "+48",   "Pologne", "Poland" },
    { "PM", "SPM", 666, CONT_NA, "+508",  "Saint-Pierre-et-Miquelon", "Saint Pierre and Miquelon" },
    { "PN", "PCN", 612, CONT_OC, "+64",   "Îles Pitcairn", "Pitcairn Islands" },
    { "PR", "PRI", 630, CONT_NA, "+1787", "Porto Rico", "Puerto Rico" },
    { "PS", "PSE", 275, CONT_AS, "+970",  "Palestine", "Palestine" },
    { "PT", "PRT", 620, CONT_EU, "+351",  "Portugal", "Portugal" },
    { "PW", "PLW", 585, CONT_OC, "+680",  "Palaos", "Palau" },
    { "PY", "PRY", 600, CONT_SA, "+595",  "Paraguay", "Paraguay" },
    { "QA", "QAT", 634, CONT_AS, "+974",  "Qatar", "Qatar" },
    { "RE", "REU", 638, CONT_AF, "+262",  "La Réunion", "Reunion" },
    { "RO", "ROU", 642, CONT_EU, "+40",   "Roumanie", "Romania" },
    { "RS", "SRB", 688, CONT_EU, "+381",  "Serbie", "Serbia" },
    { "RU", "RUS", 643, CONT_EU, "+7",    "Russie", "Russia" },
    { "RW", "RWA", 646, CONT_AF, "+250",  "Rwanda", "Rwanda" },
    { "SA", "SAU", 682, CONT_AS, "+966",  "Arabie saoudite", "Saudi Arabia" },
    { "SB", "SLB", 90,  CONT_OC, "+677",  "Îles Salomon", "Solomon Islands" },
    { "SC", "SYC", 690, CONT_AF, "+248",  "Seychelles", "Seychelles" },
    { "SD", "SDN", 729, CONT_AF, "+249",  "Soudan", "Sudan" },
    { "SE", "SWE", 752, CONT_EU, "+46",   "Suède", "Sweden" },
    { "SG", "SGP", 702, CONT_AS, "+65",   "Singapour", "Singapore" },
    { "SH", "SHN", 654, CONT_AF, "+290",  "Sainte-Hélène", "Saint Helena" },
    { "SI", "SVN", 705, CONT_EU, "+386",  "Slovénie", "Slovenia" },
    { "SJ", "SJM", 744, CONT_EU, "+47",   "Svalbard et Jan Mayen", "Svalbard and Jan Mayen" },
    { "SK", "SVK", 703, CONT_EU, "+421",  "Slovaquie", "Slovakia" },
    { "SL", "SLE", 694, CONT_AF, "+232",  "Sierra Leone", "Sierra Leone" },
    { "SM", "SMR", 674, CONT_EU, "+378",  "Saint-Marin", "San Marino" },
    { "SN", "SEN", 686, CONT_AF, "+221",  "Sénégal", "Senegal" },
    { "SO", "SOM", 706, CONT_AF, "+252",  "Somalie", "Somalia" },
    { "SR", "SUR", 740, CONT_SA, "+597",  "Suriname", "Suriname" },
    { "SS", "SSD", 728, CONT_AF, "+211",  "Soudan du Sud", "South Sudan" },
    { "ST", "STP", 678, CONT_AF, "+239",  "Sao Tomé-et-Principe", "Sao Tome and Principe" },
    { "SV", "SLV", 222, CONT_NA, "+503",  "Salvador", "El Salvador" },
    { "SX", "SXM", 534, CONT_NA, "+1721", "Saint-Martin (partie néerlandaise)", "Sint Maarten" },
    { "SY", "SYR", 760, CONT_AS, "+963",  "Syrie", "Syria" },
    { "SZ", "SWZ", 748, CONT_AF, "+268",  "Eswatini", "Eswatini" },
    { "TC", "TCA", 796, CONT_NA, "+1649", "Îles Turques-et-Caïques", "Turks and Caicos Islands" },
    { "TD", "TCD", 148, CONT_AF, "+235",  "Tchad", "Chad" },
    { "TF", "ATF", 260, CONT_AN, "+262",  "Terres australes françaises", "French Southern Territories" },
    { "TG", "TGO", 768, CONT_AF, "+228",  "Togo", "Togo" },
    { "TH", "THA", 764, CONT_AS, "+66",   "Thaïlande", "Thailand" },
    { "TJ", "TJK", 762, CONT_AS, "+992",  "Tadjikistan", "Tajikistan" },
    { "TK", "TKL", 772, CONT_OC, "+690",  "Tokelau", "Tokelau" },
    { "TL", "TLS", 626, CONT_AS, "+670",  "Timor oriental", "Timor-Leste" },
    { "TM", "TKM", 795, CONT_AS, "+993",  "Turkménistan", "Turkmenistan" },
    { "TN", "TUN", 788, CONT_AF, "+216",  "Tunisie", "Tunisia" },
    { "TO", "TON", 776, CONT_OC, "+676",  "Tonga", "Tonga" },
    { "TR", "TUR", 792, CONT_AS, "+90",   "Türkiye", "Turkiye" },
    { "TT", "TTO", 780, CONT_NA, "+1868", "Trinité-et-Tobago", "Trinidad and Tobago" },
    { "TV", "TUV", 798, CONT_OC, "+688",  "Tuvalu", "Tuvalu" },
    { "TW", "TWN", 158, CONT_AS, "+886",  "Taiwan", "Taiwan" },
    { "TZ", "TZA", 834, CONT_AF, "+255",  "Tanzanie", "Tanzania" },
    { "UA", "UKR", 804, CONT_EU, "+380",  "Ukraine", "Ukraine" },
    { "UG", "UGA", 800, CONT_AF, "+256",  "Ouganda", "Uganda" },
    { "UM", "UMI", 581, CONT_OC, "+1",    "Îles mineures éloignées des États-Unis", "U.S. Outlying Islands" },
    { "US", "USA", 840, CONT_NA, "+1",    "États-Unis", "United States" },
    { "UY", "URY", 858, CONT_SA, "+598",  "Uruguay", "Uruguay" },
    { "UZ", "UZB", 860, CONT_AS, "+998",  "Ouzbékistan", "Uzbekistan" },
    { "VA", "VAT", 336, CONT_EU, "+379",  "Vatican", "Vatican City" },
    { "VC", "VCT", 670, CONT_NA, "+1784", "Saint-Vincent-et-les-Grenadines", "Saint Vincent and the Grenadines" },
    { "VE", "VEN", 862, CONT_SA, "+58",   "Venezuela", "Venezuela" },
    { "VG", "VGB", 92,  CONT_NA, "+1284", "Îles Vierges britanniques", "British Virgin Islands" },
    { "VI", "VIR", 850, CONT_NA, "+1340", "Îles Vierges américaines", "U.S. Virgin Islands" },
    { "VN", "VNM", 704, CONT_AS, "+84",   "Vietnam", "Vietnam" },
    { "VU", "VUT", 548, CONT_OC, "+678",  "Vanuatu", "Vanuatu" },
    { "WF", "WLF", 876, CONT_OC, "+681",  "Wallis-et-Futuna", "Wallis and Futuna" },
    { "WS", "WSM", 882, CONT_OC, "+685",  "Samoa", "Samoa" },
    { "XK", "XKX", 0,   CONT_EU, "+383",  "Kosovo", "Kosovo" },
    { "YE", "YEM", 887, CONT_AS, "+967",  "Yémen", "Yemen" },
    { "YT", "MYT", 175, CONT_AF, "+262",  "Mayotte", "Mayotte" },
    { "ZA", "ZAF", 710, CONT_AF, "+27",   "Afrique du Sud", "South Africa" },
    { "ZM", "ZMB", 894, CONT_AF, "+260",  "Zambie", "Zambia" },
    { "ZW", "ZWE", 716, CONT_AF, "+263",  "Zimbabwe", "Zimbabwe" },
};

#define PAYS_COUNT ((int)(sizeof(PAYS) / sizeof(PAYS[0])))

/* ================================================================
 * Enum + constantes continentXxx
 * ================================================================ */

static const char* const CONTINENT_VALUES[] = {
    "continentAfrique", "continentAmériqueNord", "continentAmériqueSud",
    "continentAntarctique", "continentAsie", "continentEurope",
    "continentOcéanie", NULL
};

static const OwlEnum E_continent = {
    .name = "continentXxx", .nameAscii = "continentXxx", .category = "Pays",
    .descFR = "Désigne un des sept continents, pour filtrer PaysListe.",
    .descEN = "Designates one of the seven continents, to filter CountryList.",
    .values = CONTINENT_VALUES,
    .combinable = false,
    .nameEN = "continentXxx",
};

static const OwlEnum* const PAYS_ENUMS[] = { &E_continent };

static const OwlConstDef PAYS_CONSTS[] = {
    { .name = "continentAfrique", .category = "Pays", .nameEN = "continentAfrica",
      .value = INTEGER_VAL(OWL_CONTINENT_AFRIQUE),
      .descFR = "Continent africain.", .descEN = "African continent." },
    { .name = "continentAmériqueNord", .category = "Pays", .nameEN = "continentNorthAmerica",
      .value = INTEGER_VAL(OWL_CONTINENT_AMERIQUE_NORD),
      .descFR = "Continent nord-américain (Amérique centrale et Caraïbes incluses).",
      .descEN = "North American continent (including Central America and the Caribbean)." },
    { .name = "continentAmériqueSud", .category = "Pays", .nameEN = "continentSouthAmerica",
      .value = INTEGER_VAL(OWL_CONTINENT_AMERIQUE_SUD),
      .descFR = "Continent sud-américain.", .descEN = "South American continent." },
    { .name = "continentAntarctique", .category = "Pays", .nameEN = "continentAntarctica",
      .value = INTEGER_VAL(OWL_CONTINENT_ANTARCTIQUE),
      .descFR = "Continent antarctique.", .descEN = "Antarctic continent." },
    { .name = "continentAsie", .category = "Pays", .nameEN = "continentAsia",
      .value = INTEGER_VAL(OWL_CONTINENT_ASIE),
      .descFR = "Continent asiatique.", .descEN = "Asian continent." },
    { .name = "continentEurope", .category = "Pays", .nameEN = "continentEurope",
      .value = INTEGER_VAL(OWL_CONTINENT_EUROPE),
      .descFR = "Continent européen.", .descEN = "European continent." },
    { .name = "continentOcéanie", .category = "Pays", .nameEN = "continentOceania",
      .value = INTEGER_VAL(OWL_CONTINENT_OCEANIE),
      .descFR = "Continent océanien.", .descEN = "Oceanian continent." },
};

/* ================================================================
 * Types composites Continent et Pays
 * ================================================================ */

/* Pays et Continent sont des types composites de donnees : leurs proprietes
 * s'accedent en DOUBLE POINT (invariant 6 : `monPays..CodeISO2`,
 * `monPays..Continent..Nom`). Les `fields` declares ci-dessous activent la
 * validation compile-time des proprietes ET publient la paire bilingue FR/EN
 * de chaque propriete (explain LSP, `owl translate`). Le STOCKAGE reel vit dans
 * les instances peuplees par le runtime (pays_make_country / _continent) ; les
 * templates (vm.types) servent au clone `x est un Pays` et a resoudre le
 * sous-type Continent a la compilation. */

static const OwlField FIELDS_Continent[] = {
    { "Nom",  NULL, "chaîne", "\"\"",
      "nom du continent dans la langue courante",
      "continent name in the current language", .nameEN = "Name" },
    { "Code", NULL, "chaîne", "\"\"",
      "code à deux lettres du continent (AF, NA, SA, AN, AS, EU, OC)",
      "two-letter continent code (AF, NA, SA, AN, AS, EU, OC)", .nameEN = "Code" },
    { 0 },
};

static const OwlField FIELDS_Pays[] = {
    { "Nom",        NULL, "chaîne", "\"\"",
      "nom court du pays ou territoire dans la langue courante",
      "short name of the country or territory in the current language", .nameEN = "Name" },
    { "CodeISO2",   NULL, "chaîne", "\"\"",
      "code ISO 3166-1 alpha-2 (deux lettres, ex. \"FR\")",
      "ISO 3166-1 alpha-2 code (two letters, e.g. \"FR\")", .nameEN = "CodeISO2" },
    { "CodeISO3",   NULL, "chaîne", "\"\"",
      "code ISO 3166-1 alpha-3 (trois lettres, ex. \"FRA\")",
      "ISO 3166-1 alpha-3 code (three letters, e.g. \"FRA\")", .nameEN = "CodeISO3" },
    { "CodeISONum", NULL, "entier", "0",
      "code numérique ISO 3166-1 en entier, sans zéro de tête (ex. 250)",
      "ISO 3166-1 numeric code as an integer, no leading zero (e.g. 250)", .nameEN = "CodeISONum" },
    { "Continent",  NULL, "Continent", "Null",
      "continent de rattachement (un Continent)",
      "the continent it belongs to (a Continent)", .nameEN = "Continent" },
    { "PrefixeTelephone", "PréfixeTéléphone", "chaîne", "\"\"",
      "indicatif téléphonique international (ex. \"+33\")",
      "international dialing prefix (e.g. \"+33\")", .nameEN = "PhonePrefix" },
    { 0 },
};

static const char* const ALIAS_Continent[] = { "Continent", NULL };
static const char* const ALIAS_Pays[]      = { "Pays", "Country", NULL };

static const OwlTypeDef PAYS_TYPES[] = {
    { .canonical      = "Continent",
      .canonicalAscii = "Continent",
      .aliases        = ALIAS_Continent,
      .category       = "Pays",
      .descFR         = "Un continent, avec deux propriétés (double point) : ..Nom (chaîne) "
                        "et ..Code (chaîne de deux lettres : AF, NA, SA, AN, AS, EU, OC).",
      .descEN         = "A continent, with two properties (double dot): ..Name (string) and "
                        "..Code (two-letter string: AF, NA, SA, AN, AS, EU, OC).",
      .fields         = FIELDS_Continent,
      .nameEN         = "Continent",
      .genre          = OWL_GENRE_M },   /* le continent */
    { .canonical      = "Pays",
      .canonicalAscii = "Pays",
      .aliases        = ALIAS_Pays,
      .category       = "Pays",
      .descFR         = "Un pays ou territoire, avec des propriétés en double point : ..Nom "
                        "(nom court), ..CodeISO2 (\"FR\"), ..CodeISO3 (\"FRA\"), ..CodeISONum "
                        "(code numerique ISO en entier, sans zero de tete, ex. 250), "
                        "..Continent (un Continent) et ..PréfixeTéléphone (\"+33\"). "
                        "Obtenu via PaysListe.",
      .descEN         = "A country or territory, with double-dot properties: ..Name (short "
                        "name), ..CodeISO2 (\"FR\"), ..CodeISO3 (\"FRA\"), ..CodeISONum (ISO "
                        "numeric code as an integer, no leading zero, e.g. 250), "
                        "..Continent (a Continent) and ..PhonePrefix (\"+33\"). Obtained "
                        "via CountryList.",
      .fields         = FIELDS_Pays,
      .nameEN         = "Country",
      .genre          = OWL_GENRE_M },   /* le pays */
};

/* ================================================================
 * Templates runtime (vm.types)
 * ================================================================ */

static ObjInstance* build_continent_template(void) {
    ObjString* name = obj_copy_string("Continent", 9);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_str(tpl, "Nom",  "");
    inst_field_set_str(tpl, "Code", "");
    return tpl;
}

static ObjInstance* build_pays_template(void) {
    ObjString* name = obj_copy_string("Pays", 4);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_str(tpl, "Nom",        "");
    inst_field_set_str(tpl, "CodeISO2",   "");
    inst_field_set_str(tpl, "CodeISO3",   "");
    inst_field_set_int(tpl, "CodeISONum", 0);
    /* Sous-objet Continent : le sous-type de `monPays..Continent..Nom` se
     * resout a la compilation via le champ declare (type "Continent") ; le
     * sous-objet du template reste peuple pour le clone `x est un Pays`. */
    vm_push(OBJ_VAL(tpl));
    ObjInstance* cont = build_continent_template();
    vm_push(OBJ_VAL(cont));
    inst_field_set_value(tpl, "Continent", OBJ_VAL(cont));
    vm_pop();
    vm_pop();
    inst_field_set_str(tpl, "PrefixeTelephone", "");
    return tpl;
}

/* ================================================================
 * Construction d'une instance Pays peuplee
 * ================================================================ */

/* Construit une instance Continent pour l'index donne. */
static ObjInstance* pays_make_continent(int contIdx) {
    const ContinentEntry* ce = &CONTINENTS[contIdx];
    const char* nom = owl_error_render_lang_is_en() ? ce->nomEN : ce->nomFR;
    ObjString* name = obj_copy_string("Continent", 9);
    vm_push(OBJ_VAL(name));
    ObjInstance* inst = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(inst));
    inst_field_set_str(inst, "Nom",  nom);
    inst_field_set_str(inst, "Code", ce->code);
    vm_pop();
    return inst;
}

/* Construit une instance Pays peuplee depuis l'entree de table. */
static ObjInstance* pays_make_country(const PaysEntry* e) {
    ObjString* name = obj_copy_string("Pays", 4);
    vm_push(OBJ_VAL(name));
    ObjInstance* inst = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(inst));

    const char* nom = owl_error_render_lang_is_en() ? e->nomEN : e->nomFR;
    inst_field_set_str(inst, "Nom",        nom);
    inst_field_set_str(inst, "CodeISO2",   e->iso2);
    inst_field_set_str(inst, "CodeISO3",   e->iso3);
    inst_field_set_int(inst, "CodeISONum", e->num);

    ObjInstance* cont = pays_make_continent(e->continent);
    vm_push(OBJ_VAL(cont));
    inst_field_set_value(inst, "Continent", OBJ_VAL(cont));
    vm_pop();

    inst_field_set_str(inst, "PrefixeTelephone", e->phone);

    vm_pop();   /* inst */
    return inst;
}

/* ================================================================
 * Native PaysListe / CountryList
 * ================================================================ */

/* Resout l'argument de filtre vers un index CONT_* (0..6), ou -1 si aucun
 * filtre applicable. Accepte une constante continentXxx (entier sentinelle)
 * ou un code 2 lettres en chaine ("EU"). */
static int pays_filter_index(Value arg) {
    if (IS_INTEGER(arg)) {
        int64_t v = AS_INTEGER(arg);
        for (int i = 0; i < CONT_COUNT; i++)
            if (CONTINENTS[i].sentinel == v) return i;
        return -1;
    }
    if (IS_STRING(arg)) {
        ObjString* s = AS_STRING(arg);
        for (int i = 0; i < CONT_COUNT; i++)
            if (strcasecmp(s->chars, CONTINENTS[i].code) == 0) return i;
        return -1;
    }
    return -1;
}

static Value fn_pays_liste(int argCount, Value* args) {
    int filter = -1;   /* -1 = tous */
    if (argCount >= 1 && !IS_NIL(args[0])) {
        filter = pays_filter_index(args[0]);
        if (filter < 0) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "PaysListe",
                                  "arg", "continent",
                                  "expected", "une constante continentXxx ou un code continent sur deux lettres",
                                  "actual", "valeur non reconnue", NULL);
            return NIL_VAL;
        }
    }

    ObjArray* result = obj_new_array();
    vm_push(OBJ_VAL(result));
    for (int i = 0; i < PAYS_COUNT; i++) {
        if (filter >= 0 && PAYS[i].continent != filter) continue;
        ObjInstance* p = pays_make_country(&PAYS[i]);
        vm_push(OBJ_VAL(p));
        obj_array_push(result, OBJ_VAL(p));
        vm_pop();
    }
    vm_pop();   /* result */
    return OBJ_VAL(result);
}

/* ================================================================
 * Enregistrement
 * ================================================================ */

void stdlib_pays_init(void) {
    static const OwlParam p_liste[] = {
        { "continent", "entier",
          "Continent a conserver (constante continentXxx). Absent : tous les pays.",
          &E_continent, NULL, true },
        { 0 }
    };
    static const OwlSignature sig_liste[] = {
        { 0, NULL,     "tableau", "Tableau de Pays : tous les pays et territoires connus." },
        { 1, p_liste,  "tableau", "Tableau de Pays appartenant au continent demande." },
        { 0 }
    };

    static const OwlNativeDef DEFS[] = {
        { .canonical      = "PaysListe",
          .canonicalAscii = "PaysListe",
          .nameEN         = "CountryList",
          .category       = "Pays",
          .descFR         = "Rend la liste des pays et territoires (type Pays) sous "
                            "forme de tableau. Sans argument : tous les pays. Avec une "
                            "constante continentXxx : seulement les pays de ce continent. "
                            "Chaque Pays expose son nom, ses codes ISO 3166-1 "
                            "(..CodeISO2, ..CodeISO3, ..CodeISONum), son continent et son "
                            "indicatif telephonique (donnees ISO 3166-1 / CLDR).",
          .descEN         = "Returns the list of countries and territories (Country type) "
                            "as an array. With no argument: all countries. With a "
                            "continentXxx constant: only the countries of that continent. "
                            "Each Country exposes its name, its ISO 3166-1 codes "
                            "(..CodeISO2, ..CodeISO3, ..CodeISONum), its continent and its "
                            "dialing prefix (ISO 3166-1 / CLDR data).",
          .signatures     = sig_liste,
          .impl           = fn_pays_liste,
          .flags          = OWL_NAT_PURE },
    };

    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
    owl_registry_register_enums(PAYS_ENUMS, (int)(sizeof(PAYS_ENUMS) / sizeof(PAYS_ENUMS[0])));
    owl_registry_register_consts(PAYS_CONSTS, (int)(sizeof(PAYS_CONSTS) / sizeof(PAYS_CONSTS[0])));
    owl_registry_register_types(PAYS_TYPES, (int)(sizeof(PAYS_TYPES) / sizeof(PAYS_TYPES[0])));

    /* Templates runtime (vm.types) sous les noms canon ASCII + EN. */
    ObjInstance* contTpl = build_continent_template();
    vm_push(OBJ_VAL(contTpl));
    vm_type_register("Continent", OBJ_VAL(contTpl));
    vm_pop();

    ObjInstance* paysTpl = build_pays_template();
    vm_push(OBJ_VAL(paysTpl));
    vm_type_register("Pays",    OBJ_VAL(paysTpl));
    vm_type_register("Country", OBJ_VAL(paysTpl));
    vm_pop();
}
