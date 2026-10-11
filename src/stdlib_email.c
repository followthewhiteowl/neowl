// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Fondations courriel (, Lot 0) — voir stdlib_email.h pour le contrat.
 *
 * ETAT : types de donnees uniquement (Email, emailSessionSMTP, emailAttache) +
 * constantes de priorite / confidentialite / option de connexion. AUCUN
 * comportement : pas d'encodage MIME, pas de construction/import de Source, pas
 * de transport SMTP. Ces lots viennent ensuite. Ce module declare des
 * structures posables et relisibles avec leurs valeurs par defaut.
 *
 * Surface canon :
 *   type Email            (Communication/Email/1000018713, EN=Email)
 *   type emailSessionSMTP (1000018765, EN=emailSMTPSession)
 *   type emailAttache     (1000018752, EN=emailAttach)
 *
 * Divergences assumees vs canon (tranchees au cadrage du ticket) :
 *   - emailSessionSMTP omet Asynchrone (envoi toujours synchrone), AuthToken
 *     (OAuth2 hors scope) et TimeoutConnexion (delai expose par une fonction au
 *     lot transport, pas par un champ) ;
 *   - emailSessionSMTP ajoute CertificatCA (CA privee optionnelle pour TLS),
 *     extension OWL alignee sur mqttSession.
 *
 * Les formes EN des champs ne figurent pas dans la doc FR : derivees par
 * convention alignee sur les types voisins (mqttSession), a confronter au canon
 * anglais si divergence.
 *
 * La constante optionSSL est enregistree par le socle MQ (categorie MQTT) et
 * reutilisee ici comme valeur d'Option (chiffrement TLS implicite) — elle n'est
 * PAS redefinie (collision registre). Valeurs d'Option distinctes :
 * emailOptionDefaut=0, optionSSL=1, emailOptionSecuriseTLS=2. */

#include "stdlib_email.h"

#include "owl_registry.h"
#include "owl_stdlib.h"
/* net_tcp.h (via net_tcp_tls.h) tire winsock2.h sous Windows -> collision
 * TokenType (winnt.h vs scanner.h). Meme garde que stdlib_ftp.c / stdlib_sql.c :
 * on n'utilise ici que les codes E_NATIVE_* + owl_emit_native_error, jamais
 * l'API compile-aware de owl_errors.h. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "mime_codec.h"
#include "vm.h"
#include "object.h"
#include "value.h"
#include "net_tcp.h"
#include "net_tcp_tls.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include <mbedtls/base64.h>
#include <mbedtls/md.h>

/* --- Valeurs des constantes -------------------------------------------------
 * Les entiers ne sont pas publies par la doc ; on fixe des valeurs stables
 * et distinctes. Priorite/Confidentialite : familles independantes, chacune
 * partant de la valeur par defaut du canon (Normale / Normal). */
#define EMAIL_PRIORITE_BASSE        1
#define EMAIL_PRIORITE_NORMALE      2   /* defaut canon */
#define EMAIL_PRIORITE_HAUTE        3

#define EMAIL_CONF_NORMAL           0   /* defaut canon */
#define EMAIL_CONF_CONFIDENTIEL     1
#define EMAIL_CONF_PERSONNEL        2
#define EMAIL_CONF_PRIVE            3

#define EMAIL_OPTION_DEFAUT         0   /* session non chiffree, port 25 */
#define EMAIL_OPTION_SSL            1   /* TLS implicite, port 465 (constante optionSSL du socle MQ) */
#define EMAIL_OPTION_SECURISE_TLS   2   /* STARTTLS, port 587 */

#define EMAIL_PORT_SMTP_DEFAUT      25

/* Delai par defaut (ms) d'attente du serveur SMTP a l'ouverture. Ecrasable par
 * EmailChangeTimeOut. Le canon par defaut vaut 10 min ; OWL retient une
 * valeur plus courte pour ne pas bloquer un programme sur un serveur muet. */
#define EMAIL_TIMEOUT_DEFAUT_MS     30000

/* Options de EmailConstruitSource : masque OU-binaire (bits distincts,
 * combinables). Famille SEPAREE des options de session SMTP ci-dessus (qui
 * pilotent le transport) — ici on pilote la fabrique du message brut. */
#define EMAIL_OPT_ENCODE_ENTETE     (1u << 0)   /* en-tetes non-ASCII -> RFC 2047 */
#define EMAIL_OPT_ENTETE_CCI        (1u << 1)   /* expose la copie cachee en Bcc: */

/* ============================================================
 * Champs des types reifies.
 * ============================================================ */

static const OwlField FIELDS_Email[] = {
    {.name="Expediteur",         .nameAccented="Expéditeur",         .type="chaîne",  .defaultValue="\"\"",
     .descFR="adresse de l'expéditeur du message", .descEN="the message sender's address", .nameEN="Sender"},
    {.name="AdresseExpediteur",  .nameAccented="AdresseExpéditeur",  .type="chaîne",  .defaultValue="\"\"",
     .descFR="adresse à laquelle les réponses doivent être envoyées (Reply-To)", .descEN="address replies should be sent to (Reply-To)", .nameEN="SenderAddress"},
    {.name="AdresseRetourErreur", .nameAccented=NULL,                .type="chaîne",  .defaultValue="\"\"",
     .descFR="adresse de retour en cas d'erreur de remise (Return-Path)", .descEN="return address on delivery failure (Return-Path)", .nameEN="ErrorReturnAddress"},
    {.name="Destinataire",       .nameAccented=NULL,                 .type="tableau de chaînes", .defaultValue="[]",
     .descFR="adresses des destinataires principaux", .descEN="primary recipient addresses", .nameEN="Recipient"},
    {.name="Cc",                 .nameAccented=NULL,                 .type="tableau de chaînes", .defaultValue="[]",
     .descFR="adresses des destinataires en copie", .descEN="carbon-copy recipient addresses", .nameEN="Cc"},
    {.name="Cci",                .nameAccented=NULL,                 .type="tableau de chaînes", .defaultValue="[]",
     .descFR="adresses des destinataires en copie cachée", .descEN="blind carbon-copy recipient addresses", .nameEN="Bcc"},
    {.name="Sujet",              .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="sujet du message", .descEN="message subject", .nameEN="Subject"},
    {.name="Message",            .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="corps du message en texte brut", .descEN="plain-text message body", .nameEN="Message"},
    {.name="HTML",               .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="corps du message au format HTML", .descEN="HTML-formatted message body", .nameEN="HTML"},
    {.name="Attache",            .nameAccented=NULL,                 .type="tableau de emailAttache", .defaultValue="[]",
     .descFR="pièces jointes du message", .descEN="the message's attachments", .nameEN="Attachment"},
    {.name="Categorie",          .nameAccented="Catégorie",          .type="chaîne",  .defaultValue="\"\"",
     .descFR="mots-clés associés au message, séparés par des points-virgules", .descEN="keywords associated with the message, separated by semicolons", .nameEN="Category"},
    {.name="Priorite",           .nameAccented="Priorité",           .type="entier",  .defaultValue="emailPrioritéNormale",
     .descFR="priorité du message (emailPrioritéBasse / Normale / Haute)", .descEN="message priority (emailPriorityLow / Normal / High)", .nameEN="Priority"},
    {.name="Confidentialite",    .nameAccented="Confidentialité",    .type="entier",  .defaultValue="emailNormal",
     .descFR="niveau de confidentialité (emailNormal / Confidentiel / Personnel / Privé)", .descEN="confidentiality level (emailNormal / Confidential / Personal / Private)", .nameEN="Confidentiality"},
    {.name="AccuseReception",    .nameAccented="AccuséRéception",    .type="booléen", .defaultValue="Faux",
     .descFR="Active la demande d'accusé de réception auprès du destinataire (Vrai)", .descEN="When True, asks the recipient for a delivery receipt", .nameEN="AcknowledgeReceipt"},
    {.name="ConfirmationLecture", .nameAccented=NULL,                .type="booléen", .defaultValue="Faux",
     .descFR="Active la demande de confirmation de lecture auprès du destinataire (Vrai)", .descEN="When True, asks the recipient for a read receipt", .nameEN="ReadReceipt"},
    {.name="MessageID",          .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="identifiant unique du message, utilisé pour suivre une conversation", .descEN="unique message identifier, used to track a conversation", .nameEN="MessageID"},
    {.name="Reference",          .nameAccented="Référence",          .type="chaîne",  .defaultValue="\"\"",
     .descFR="identifiants des messages de référence liés à la conversation", .descEN="identifiers of the reference messages tied to the conversation", .nameEN="Reference"},
    {.name="Source",             .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="contenu source brut du message (support de l'aller-retour construction / import)", .descEN="raw source content of the message (backing the build / import round-trip)", .nameEN="Source"},
    {.name="Entete",             .nameAccented="Entête",             .type="tableau de emailEntête", .defaultValue="[]",
     .descFR="en-têtes additionnels du message (par exemple des en-têtes X-*), au-delà des en-têtes standard", .descEN="the message's additional headers (for example X-* headers), beyond the standard ones", .nameEN="Header"},
    {0}
};

static const OwlField FIELDS_emailSessionSMTP[] = {
    {.name="AdresseServeur",     .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="nom DNS ou adresse IP du serveur d'envoi (SMTP)", .descEN="DNS name or IP address of the outgoing (SMTP) server", .nameEN="ServerAddress"},
    {.name="Nom",                .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="nom d'utilisateur pour une session authentifiée", .descEN="user name for an authenticated session", .nameEN="Name"},
    {.name="MotDePasse",         .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="mot de passe pour une session authentifiée", .descEN="password for an authenticated session", .nameEN="Password"},
    {.name="Port",               .nameAccented=NULL,                 .type="entier",  .defaultValue="25",
     .descFR="port du serveur d'envoi (25 par défaut)", .descEN="outgoing server port (25 by default)", .nameEN="Port"},
    {.name="Option",             .nameAccented=NULL,                 .type="entier",  .defaultValue="emailOptionDéfaut",
     .descFR="options de connexion (emailOptionDéfaut, emailOptionSécuriséTLS, optionSSL)", .descEN="connection options (emailOptionDefault, emailOptionSecureTLS, optionSSL)", .nameEN="Option"},
    {.name="CertificatCA",       .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="chemin d'un fichier PEM de certificat d'autorité pour valider le serveur en TLS ; vide = autorités publiques par défaut", .descEN="path to a PEM certificate-authority file used to validate the server over TLS; empty = default public authorities", .nameEN="CACertificate"},
    {0}
};

static const OwlField FIELDS_emailAttache[] = {
    {.name="Contenu",            .nameAccented=NULL,                 .type="Buffer",  .defaultValue="",
     .descFR="contenu binaire de la pièce jointe", .descEN="binary content of the attachment", .nameEN="Content"},
    {.name="ContentType",        .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="type MIME de la pièce jointe (par exemple application/pdf)", .descEN="MIME type of the attachment (for example application/pdf)", .nameEN="ContentType"},
    {.name="Nom",                .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="nom de fichier de la pièce jointe", .descEN="file name of the attachment", .nameEN="Name"},
    {.name="ContentDescription", .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="texte descriptif du contenu de la pièce jointe", .descEN="descriptive text for the attachment content", .nameEN="ContentDescription"},
    {.name="Identifiant",        .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="identifiant de contenu (Content-ID) d'une pièce jointe référencée depuis le corps HTML", .descEN="content identifier (Content-ID) of an attachment referenced from the HTML body", .nameEN="Identifier"},
    {0}
};

static const OwlField FIELDS_emailEntete[] = {
    {.name="Nom",                .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="nom de l'en-tête (par exemple X-Traqueur-Interne)", .descEN="name of the header (for example X-Internal-Tracker)", .nameEN="Name"},
    {.name="Valeur",             .nameAccented=NULL,                 .type="chaîne",  .defaultValue="\"\"",
     .descFR="valeur de l'en-tête", .descEN="value of the header", .nameEN="Value"},
    {0}
};

/* ============================================================
 * Types reifies.
 * ============================================================ */

static const OwlTypeDef TYPES_EMAIL[] = {
    { .canonical      = "Email",
      .canonicalAscii = "Email",
      .category       = "Email",
      .descFR         = "Message électronique à envoyer : expéditeur, destinataires, contenu texte/HTML, pièces jointes, options d'accusé et de priorité.",
      .descEN         = "An electronic message to send: sender, recipients, text/HTML content, attachments, receipt and priority options.",
      .fields         = FIELDS_Email,
      .nameEN         = "Email",
      .genre          = OWL_GENRE_M },   /* un message */
    { .canonical      = "emailSessionSMTP",
      .canonicalAscii = "emailSessionSMTP",
      .category       = "Email",
      .descFR         = "Paramètres d'une connexion à un serveur d'envoi SMTP : adresse, port, identifiants, options de chiffrement.",
      .descEN         = "Parameters for a connection to an outgoing SMTP server: address, port, credentials, encryption options.",
      .fields         = FIELDS_emailSessionSMTP,
      .nameEN         = "emailSMTPSession",
      .genre          = OWL_GENRE_F },   /* une session */
    { .canonical      = "emailAttache",
      .canonicalAscii = "emailAttache",
      .category       = "Email",
      .descFR         = "Pièce jointe d'un Email : contenu binaire, type MIME, nom de fichier, description et identifiant de contenu.",
      .descEN         = "An attachment of an Email: binary content, MIME type, file name, description and content identifier.",
      .fields         = FIELDS_emailAttache,
      .nameEN         = "emailAttach",
      .genre          = OWL_GENRE_F },   /* une pièce jointe */
    { .canonical      = "emailEntête",
      .canonicalAscii = "emailEntete",
      .category       = "Email",
      .descFR         = "En-tête additionnel d'un Email : nom et valeur libres (par exemple un en-tête X-*), rangé dans le tableau Entête d'un Email.",
      .descEN         = "An additional header of an Email: free-form name and value (for example an X-* header), stored in an Email's Header array.",
      .fields         = FIELDS_emailEntete,
      .nameEN         = "emailHeader",
      .genre          = OWL_GENRE_M },   /* un en-tête */
};

/* ============================================================
 * Constantes.
 * ============================================================ */

static const OwlConstDef CONSTS_EMAIL[] = {
    { "emailPrioritéBasse", "Email", INTEGER_VAL(EMAIL_PRIORITE_BASSE),
      "Priorité basse : à affecter à la propriété Priorité d'un Email pour un message peu urgent.",
      "Low priority: assign to an Email's Priority property for a low-urgency message.",
      "emailPriorityLow" },
    { "emailPrioritéNormale", "Email", INTEGER_VAL(EMAIL_PRIORITE_NORMALE),
      "Priorité normale (valeur par défaut de la propriété Priorité d'un Email).",
      "Normal priority (default value of an Email's Priority property).",
      "emailPriorityNormal" },
    { "emailPrioritéHaute", "Email", INTEGER_VAL(EMAIL_PRIORITE_HAUTE),
      "Priorité haute : à affecter à la propriété Priorité d'un Email pour un message urgent.",
      "High priority: assign to an Email's Priority property for an urgent message.",
      "emailPriorityHigh" },

    { "emailNormal", "Email", INTEGER_VAL(EMAIL_CONF_NORMAL),
      "Confidentialité normale (valeur par défaut de la propriété Confidentialité d'un Email) : aucune restriction.",
      "Normal confidentiality (default value of an Email's Confidentiality property): no restriction.",
      "emailNormal" },
    { "emailConfidentiel", "Email", INTEGER_VAL(EMAIL_CONF_CONFIDENTIEL),
      "Confidentialité confidentielle : à affecter à la propriété Confidentialité d'un Email.",
      "Confidential confidentiality: assign to an Email's Confidentiality property.",
      "emailConfidential" },
    { "emailPersonnel", "Email", INTEGER_VAL(EMAIL_CONF_PERSONNEL),
      "Confidentialité personnelle : à affecter à la propriété Confidentialité d'un Email.",
      "Personal confidentiality: assign to an Email's Confidentiality property.",
      "emailPersonal" },
    { "emailPrivé", "Email", INTEGER_VAL(EMAIL_CONF_PRIVE),
      "Confidentialité privée : à affecter à la propriété Confidentialité d'un Email.",
      "Private confidentiality: assign to an Email's Confidentiality property.",
      "emailPrivate" },

    { "emailOptionDéfaut", "Email", INTEGER_VAL(EMAIL_OPTION_DEFAUT),
      "Session d'envoi non chiffrée (port standard 25). Valeur par défaut de la propriété Option d'une session SMTP.",
      "Unencrypted outgoing session (standard port 25). Default value of an SMTP session's Option property.",
      "emailOptionDefault" },
    { "emailOptionSécuriséTLS", "Email", INTEGER_VAL(EMAIL_OPTION_SECURISE_TLS),
      "Session d'envoi sécurisée : la connexion démarre en clair puis bascule en TLS (STARTTLS, port standard 587). À affecter à la propriété Option d'une session SMTP.",
      "Secured outgoing session: the connection starts in clear text then upgrades to TLS (STARTTLS, standard port 587). Assign to an SMTP session's Option property.",
      "emailOptionSecureTLS" },
    /* optionSSL (TLS implicite, port 465) est enregistree par le socle MQ et
     * reutilisee telle quelle comme valeur d'Option — NON redefinie ici. */

    /* Options de EmailConstruitSource — masque OU-binaire (combinable). Famille
     * distincte des options de session ci-dessus (transport). */
    { "emailOptionEncodeEntête", "Email", INTEGER_VAL(EMAIL_OPT_ENCODE_ENTETE),
      "Encode les en-têtes contenant des caractères non ASCII (mots-encodés). À combiner aux autres options de EmailConstruitSource.",
      "Encodes headers that contain non-ASCII characters (encoded words). Combine with the other EmailBuildSource options.",
      "emailOptionEncodeHeader" },
    { "emailOptionEntêteCCi", "Email", INTEGER_VAL(EMAIL_OPT_ENTETE_CCI),
      "Fait apparaître les destinataires en copie cachée dans un en-tête dédié du message construit (masqués par défaut). À combiner aux autres options de EmailConstruitSource.",
      "Exposes the blind-carbon-copy recipients in a dedicated header of the built message (hidden by default). Combine with the other EmailBuildSource options.",
      "emailOptionHeaderBcc" },
};

/* ============================================================
 * Templates runtime — valeurs par defaut clonees a chaque declaration.
 * Les cles de storage sont les formes ASCII des champs (invariant accent :
 * le scanner strip les accents, l'acces `..Expéditeur` resout vers
 * "Expediteur"). Les tableaux et le buffer sont poses ici puis deep-clones
 * par __StructClone a chaque `x est un T`.
 * ============================================================ */

static void set_field_array_empty(ObjInstance* inst, const char* name) {
    vm_push(OBJ_VAL(inst));                /* racine avant l'alloc du tableau */
    ObjArray* a = obj_new_array();
    vm_push(OBJ_VAL(a));
    inst_field_set_value(inst, name, OBJ_VAL(a));
    vm_pop();
    vm_pop();
}

static void set_field_buffer_empty(ObjInstance* inst, const char* name) {
    vm_push(OBJ_VAL(inst));                /* racine avant l'alloc du buffer */
    ObjBuffer* b = obj_buffer_empty(0);
    vm_push(OBJ_VAL(b));
    inst_field_set_value(inst, name, OBJ_VAL(b));
    vm_pop();
    vm_pop();
}

static ObjInstance* build_Email_template(void) {
    ObjString* name = obj_copy_string("Email", 5);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    inst_field_set_str (tpl, "Expediteur",          "");
    inst_field_set_str (tpl, "AdresseExpediteur",   "");
    inst_field_set_str (tpl, "AdresseRetourErreur", "");
    set_field_array_empty(tpl, "Destinataire");
    set_field_array_empty(tpl, "Cc");
    set_field_array_empty(tpl, "Cci");
    inst_field_set_str (tpl, "Sujet",               "");
    inst_field_set_str (tpl, "Message",             "");
    inst_field_set_str (tpl, "HTML",                "");
    set_field_array_empty(tpl, "Attache");
    inst_field_set_str (tpl, "Categorie",           "");
    inst_field_set_int (tpl, "Priorite",            EMAIL_PRIORITE_NORMALE);
    inst_field_set_int (tpl, "Confidentialite",     EMAIL_CONF_NORMAL);
    inst_field_set_bool(tpl, "AccuseReception",     false);
    inst_field_set_bool(tpl, "ConfirmationLecture", false);
    inst_field_set_str (tpl, "MessageID",           "");
    inst_field_set_str (tpl, "Reference",           "");
    inst_field_set_str (tpl, "Source",              "");
    set_field_array_empty(tpl, "Entete");
    return tpl;
}

static ObjInstance* build_emailSessionSMTP_template(void) {
    ObjString* name = obj_copy_string("emailSessionSMTP", 16);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    inst_field_set_str(tpl, "AdresseServeur", "");
    inst_field_set_str(tpl, "Nom",            "");
    inst_field_set_str(tpl, "MotDePasse",     "");
    inst_field_set_int(tpl, "Port",           EMAIL_PORT_SMTP_DEFAUT);
    inst_field_set_int(tpl, "Option",         EMAIL_OPTION_DEFAUT);
    inst_field_set_str(tpl, "CertificatCA",   "");
    /* Champs internes (non documentes cote user) : _handle porte le pointeur
     * opaque vers la session de transport ouverte, _timeout le delai courant. */
    inst_field_set_int(tpl, "_handle",        0);
    inst_field_set_int(tpl, "_timeout",       EMAIL_TIMEOUT_DEFAUT_MS);
    return tpl;
}

static ObjInstance* build_emailAttache_template(void) {
    ObjString* name = obj_copy_string("emailAttache", 12);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    set_field_buffer_empty(tpl, "Contenu");
    inst_field_set_str(tpl, "ContentType",        "");
    inst_field_set_str(tpl, "Nom",                "");
    inst_field_set_str(tpl, "ContentDescription", "");
    inst_field_set_str(tpl, "Identifiant",        "");
    return tpl;
}

static ObjInstance* build_emailEntete_template(void) {
    ObjString* name = obj_copy_string("emailEntête", (int)strlen("emailEntête"));
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    inst_field_set_str(tpl, "Nom",    "");
    inst_field_set_str(tpl, "Valeur", "");
    return tpl;
}

/* ============================================================
 * EmailConstruitSource — fabrique du message brut RFC 5322 / MIME.
 *
 * Assemblage sur un builder heap (pas d'Obj GC pendant la construction) ;
 * l'unique interaction GC est l'ecriture finale de la chaine dans
 * `email..Source`, ou l'instance est deja rootee (args[0] sur la pile VM).
 * Fins de ligne CRLF partout. Pas de dot-stuffing (concern du DATA SMTP).
 * ============================================================ */

/* Builder d'octets extensible, propre a ce module (le Buf de mime_codec.c est
 * interne a ce fichier-la). NUL-termine a la finalisation. */
typedef struct { char* data; size_t len; size_t cap; int oom; } EBuf;

static void eb_init(EBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; b->oom = 0; }

static int eb_reserve(EBuf* b, size_t extra) {
    if (b->oom) return 0;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 1;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < need) ncap *= 2;
    char* nd = realloc(b->data, ncap);
    if (!nd) { b->oom = 1; free(b->data); b->data = NULL; return 0; }
    b->data = nd; b->cap = ncap; return 1;
}

static void eb_put(EBuf* b, const char* s, size_t n) {
    if (!eb_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void eb_puts(EBuf* b, const char* s) { eb_put(b, s, strlen(s)); }

static char* eb_finish(EBuf* b) {
    if (b->oom) { free(b->data); return NULL; }
    if (!eb_reserve(b, 0)) { free(b->data); return NULL; }
    b->data[b->len] = '\0';
    return b->data ? b->data : strdup("");
}

/* --- Lecture des champs d'instance (valides tant que l'instance est rootee). */

static const char* email_str(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_STRING(v)) return "";
    return AS_CSTRING(v);
}

static int email_int(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return 0;
    return (int)AS_INTEGER(v);
}

static ObjArray* email_array(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_ARRAY(v)) return NULL;
    return AS_ARRAY(v);
}

/* --- Emission d'en-tetes ---------------------------------------------------- */

/* Valeur d'en-tete : encodee RFC 2047 si `encode` ET non-ASCII, sinon brute. */
static void emit_header_value(EBuf* b, const char* value, int encode) {
    if (encode) {
        size_t olen = 0;
        char* enc = rfc2047_encode(value, strlen(value), &olen);
        if (enc) { eb_put(b, enc, olen); free(enc); return; }
    }
    eb_puts(b, value);
}

static void emit_header(EBuf* b, const char* name, const char* value, int encode) {
    if (value == NULL || value[0] == '\0') return;
    eb_puts(b, name);
    eb_puts(b, ": ");
    emit_header_value(b, value, encode);
    eb_puts(b, "\r\n");
}

/* En-tete d'adresses (To/Cc/Bcc) : les elements du tableau joints par ", ".
 * Les adresses restent brutes (RFC 2047 ne s'applique qu'aux noms d'affichage,
 * absents de notre modele). Rien n'est emis si le tableau est vide. */
static void emit_address_header(EBuf* b, const char* name, ObjArray* arr) {
    if (arr == NULL || arr->elements.count == 0) return;
    int emitted = 0;
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (!IS_STRING(v) || AS_CSTRING(v)[0] == '\0') continue;
        if (emitted == 0) { eb_puts(b, name); eb_puts(b, ": "); }
        else               { eb_puts(b, ", "); }
        eb_puts(b, AS_CSTRING(v));
        emitted++;
    }
    if (emitted > 0) eb_puts(b, "\r\n");
}

static void emit_date_header(EBuf* b) {
    static const char* const DOW[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* const MON[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
    time_t now = time(NULL);
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    char line[80];
    int idxD = (g.tm_wday >= 0 && g.tm_wday <= 6) ? g.tm_wday : 0;
    int idxM = (g.tm_mon  >= 0 && g.tm_mon  <= 11) ? g.tm_mon  : 0;
    snprintf(line, sizeof(line), "Date: %s, %02d %s %04d %02d:%02d:%02d +0000\r\n",
             DOW[idxD], g.tm_mday, MON[idxM], g.tm_year + 1900,
             g.tm_hour, g.tm_min, g.tm_sec);
    eb_puts(b, line);
}

/* Message-ID : la valeur existante est reprise (encadree de <> si besoin) ;
 * sinon un identifiant unique <compteur.horodatage@domaine> est genere, le
 * domaine etant derive de l'adresse d'expedition (partie apres @). */
static void emit_message_id(EBuf* b, const char* existing, const char* sender) {
    if (existing && existing[0] != '\0') {
        eb_puts(b, "Message-ID: ");
        if (existing[0] != '<') eb_puts(b, "<");
        eb_puts(b, existing);
        if (existing[strlen(existing) - 1] != '>') eb_puts(b, ">");
        eb_puts(b, "\r\n");
        return;
    }
    static unsigned long counter = 0;
    counter++;
    const char* at = sender ? strchr(sender, '@') : NULL;
    const char* domain = (at && at[1] != '\0') ? at + 1 : "owl.local";
    char line[160];
    snprintf(line, sizeof(line), "Message-ID: <%lx.%lu@%s>\r\n",
             (unsigned long)time(NULL), counter, domain);
    eb_puts(b, line);
}

/* X-Priority RFC : 1 = la plus haute, 3 = normale, 5 = la plus basse. */
static void emit_priority_header(EBuf* b, int priorite) {
    int xp = 3;
    if (priorite == EMAIL_PRIORITE_HAUTE) xp = 1;
    else if (priorite == EMAIL_PRIORITE_BASSE) xp = 5;
    char line[24];
    snprintf(line, sizeof(line), "X-Priority: %d\r\n", xp);
    eb_puts(b, line);
}

/* En-tetes additionnels (Entête[]) : chaque emailEntête non vide devient une
 * ligne `Nom: Valeur` (valeur encodee RFC 2047 si `encode` et non-ASCII). */
static void emit_custom_headers(EBuf* b, ObjArray* headers, int encode) {
    if (headers == NULL) return;
    for (int i = 0; i < headers->elements.count; i++) {
        Value v = headers->elements.values[i];
        if (!IS_INSTANCE(v)) continue;
        ObjInstance* h = AS_INSTANCE(v);
        const char* nom = email_str(h, "Nom");
        const char* val = email_str(h, "Valeur");
        if (nom[0] == '\0') continue;
        eb_puts(b, nom);
        eb_puts(b, ": ");
        emit_header_value(b, val, encode);
        eb_puts(b, "\r\n");
    }
}

/* Frontiere multipart unique (jamais susceptible d'apparaitre dans le corps). */
static void gen_boundary(char* out, size_t cap) {
    static unsigned long bc = 0;
    bc++;
    snprintf(out, cap, "=_OWL_%lx_%lu", (unsigned long)time(NULL), bc);
}

/* --- Corps ------------------------------------------------------------------ */

/* Partie texte : en-tetes de partie + separateur + corps quoted-printable.
 * Se termine par un CRLF pour que la frontiere suivante debute sur sa ligne. */
static void emit_text_part(EBuf* b, const char* subtype, const char* text) {
    eb_puts(b, "Content-Type: ");
    eb_puts(b, subtype);
    eb_puts(b, "; charset=UTF-8\r\n");
    eb_puts(b, "Content-Transfer-Encoding: quoted-printable\r\n");
    eb_puts(b, "\r\n");
    size_t olen = 0;
    char* enc = qp_encode(text, strlen(text), &olen);
    if (enc) { eb_put(b, enc, olen); free(enc); }
    eb_puts(b, "\r\n");
}

/* Entite « corps » : text/plain, text/html, ou multipart/alternative si les
 * deux formes sont fournies (partie texte PUIS html). Emet ses propres en-tetes
 * de Content-Type + le separateur d'entete + le corps. */
static void emit_body_entity(EBuf* b, const char* msg, const char* html) {
    int hasText = msg  && msg[0]  != '\0';
    int hasHtml = html && html[0] != '\0';

    if (hasText && hasHtml) {
        char bnd[64];
        gen_boundary(bnd, sizeof(bnd));
        eb_puts(b, "Content-Type: multipart/alternative; boundary=\"");
        eb_puts(b, bnd);
        eb_puts(b, "\"\r\n\r\n");
        eb_puts(b, "--"); eb_puts(b, bnd); eb_puts(b, "\r\n");
        emit_text_part(b, "text/plain", msg);
        eb_puts(b, "--"); eb_puts(b, bnd); eb_puts(b, "\r\n");
        emit_text_part(b, "text/html", html);
        eb_puts(b, "--"); eb_puts(b, bnd); eb_puts(b, "--\r\n");
    } else if (hasHtml) {
        emit_text_part(b, "text/html", html);
    } else {
        emit_text_part(b, "text/plain", msg);   /* msg vide toleree */
    }
}

/* Base64 replie a 76 colonnes (57 octets source -> 76 caracteres). */
static void emit_base64_wrapped(EBuf* b, const unsigned char* data, size_t len) {
    size_t i = 0;
    unsigned char line[128];
    while (i < len) {
        size_t chunk = len - i;
        if (chunk > 57) chunk = 57;
        size_t olen = 0;
        if (mbedtls_base64_encode(line, sizeof(line), &olen, data + i, chunk) != 0) {
            b->oom = 1; return;
        }
        eb_put(b, (const char*)line, olen);
        eb_puts(b, "\r\n");
        i += chunk;
    }
}

static void emit_attachment(EBuf* b, ObjInstance* att) {
    const char* ct   = email_str(att, "ContentType");
    const char* nom  = email_str(att, "Nom");
    const char* id   = email_str(att, "Identifiant");
    if (ct[0] == '\0') ct = "application/octet-stream";

    eb_puts(b, "Content-Type: ");
    eb_puts(b, ct);
    eb_puts(b, "\r\n");
    eb_puts(b, "Content-Transfer-Encoding: base64\r\n");
    eb_puts(b, "Content-Disposition: attachment; filename=\"");
    eb_puts(b, nom);
    eb_puts(b, "\"\r\n");
    if (id[0] != '\0') {
        eb_puts(b, "Content-ID: ");
        if (id[0] != '<') eb_puts(b, "<");
        eb_puts(b, id);
        if (id[strlen(id) - 1] != '>') eb_puts(b, ">");
        eb_puts(b, "\r\n");
    }
    eb_puts(b, "\r\n");

    Value v;
    ObjString* k = obj_copy_string("Contenu", 7);
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&att->fields, k, &v);
    vm_pop();
    if (ok && IS_BUFFER(v)) {
        ObjBuffer* buf = AS_BUFFER(v);
        emit_base64_wrapped(b, buf->bytes, (size_t)buf->length);
    } else if (ok && IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        emit_base64_wrapped(b, (const unsigned char*)s->chars, (size_t)s->length);
    }
}

static bool is_email_instance(ObjInstance* inst) {
    return inst->structName != NULL && strcmp(inst->structName->chars, "Email") == 0;
}

/* Assemble le message brut (en-tetes RFC 5322 + corps MIME, fins de ligne
 * CRLF) d'un Email. Renvoie une chaine heap NUL-terminee (a liberer par
 * l'appelant) ou NULL sur echec memoire. Aucune interaction GC : lecture des
 * champs d'une instance que l'appelant garde rootee. PAS de dot-stuffing (c'est
 * l'affaire du DATA SMTP). Partage entre EmailConstruitSource (depot dans
 * ..Source) et l'envoi SMTP (charge utile de DATA). */
static char* build_mime_source(ObjInstance* e, int options) {
    int encHdr  = (options & EMAIL_OPT_ENCODE_ENTETE) != 0;
    int showBcc = (options & EMAIL_OPT_ENTETE_CCI) != 0;

    const char* sender = email_str(e, "Expediteur");
    const char* msg    = email_str(e, "Message");
    const char* html   = email_str(e, "HTML");

    EBuf b;
    eb_init(&b);

    /* En-tetes : nom RFC-standard sur le fil, quelle que soit la propriete. */
    emit_header(&b, "From", sender, 0);
    emit_address_header(&b, "To", email_array(e, "Destinataire"));
    emit_address_header(&b, "Cc", email_array(e, "Cc"));
    if (showBcc) emit_address_header(&b, "Bcc", email_array(e, "Cci"));
    emit_header(&b, "Reply-To", email_str(e, "AdresseExpediteur"), 0);
    emit_header(&b, "Return-Path", email_str(e, "AdresseRetourErreur"), 0);
    emit_header(&b, "Subject", email_str(e, "Sujet"), encHdr);
    emit_date_header(&b);
    emit_message_id(&b, email_str(e, "MessageID"), sender);
    emit_priority_header(&b, email_int(e, "Priorite"));
    emit_custom_headers(&b, email_array(e, "Entete"), encHdr);
    eb_puts(&b, "MIME-Version: 1.0\r\n");

    /* Structure du corps. */
    ObjArray* att = email_array(e, "Attache");
    int nAtt = att ? att->elements.count : 0;

    if (nAtt > 0) {
        char bnd[64];
        gen_boundary(bnd, sizeof(bnd));
        eb_puts(&b, "Content-Type: multipart/mixed; boundary=\"");
        eb_puts(&b, bnd);
        eb_puts(&b, "\"\r\n\r\n");
        eb_puts(&b, "--"); eb_puts(&b, bnd); eb_puts(&b, "\r\n");
        emit_body_entity(&b, msg, html);
        for (int i = 0; i < nAtt; i++) {
            Value av = att->elements.values[i];
            if (!IS_INSTANCE(av)) continue;
            eb_puts(&b, "--"); eb_puts(&b, bnd); eb_puts(&b, "\r\n");
            emit_attachment(&b, AS_INSTANCE(av));
        }
        eb_puts(&b, "--"); eb_puts(&b, bnd); eb_puts(&b, "--\r\n");
    } else {
        emit_body_entity(&b, msg, html);
    }

    return eb_finish(&b);
}

static Value fn_email_construit_source(int argCount, Value* args) {
    if (!IS_INSTANCE(args[0]) || !is_email_instance(AS_INSTANCE(args[0]))) {
        const char* actual = "<non-structuré>";
        if (IS_INSTANCE(args[0]) && AS_INSTANCE(args[0])->structName != NULL)
            actual = AS_INSTANCE(args[0])->structName->chars;
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EmailConstruitSource",
                              "native", "EmailConstruitSource", "arg", "1",
                              "expected", "Email", "actual", actual, NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* e = AS_INSTANCE(args[0]);

    int options = 0;
    if (argCount >= 2 && IS_INTEGER(args[1])) options = (int)AS_INTEGER(args[1]);

    char* src = build_mime_source(e, options);
    if (src == NULL) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "EmailConstruitSource",
                              "native", "EmailConstruitSource", NULL);
        return BOOL_VAL(false);
    }
    inst_field_set_str(e, "Source", src);   /* e rootee (args[0] sur la pile VM) */
    free(src);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ============================================================
 * EmailImporteSource — parseur inverse (message brut -> champs).
 *
 * Entree NON fiable (fichier .eml quelconque, source recopiee a la main) :
 * tout est best-effort, aucun chemin ne plante. Un en-tete malforme, un
 * mot-encode RFC 2047 incomplet, un base64 corrompu ou une frontiere declaree
 * mais absente laissent le champ vide plutot que d'interrompre l'analyse.
 *
 * On travaille sur une copie C de la chaine Source : les allocations OWL du
 * remplissage (instances de piece jointe, buffers, chaines) declenchent le GC,
 * et decoupler du storage evite tout aliasing avec un champ en cours d'ecriture.
 * ============================================================ */

/* Profondeur d'imbrication multipart maximale (garde anti-recursion sur une
 * entree hostile). Au-dela, les sous-parties sont ignorees. */
#define MIME_MAX_DEPTH 24

static int ci_char(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : (unsigned char)c; }

static int ci_streq(const char* a, const char* b) {
    while (*a && *b) { if (ci_char(*a) != ci_char(*b)) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}

static int ci_prefix(const char* s, const char* pfx) {
    while (*pfx) { if (ci_char(*s) != ci_char(*pfx)) return 0; s++; pfx++; }
    return 1;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Copie [s, s+n) sans blancs de tete/queue, NUL-terminee (heap). */
static char* dup_trim(const char* s, size_t n) {
    size_t a = 0, b = n;
    while (a < b && is_ws(s[a])) a++;
    while (b > a && is_ws(s[b - 1])) b--;
    char* out = malloc(b - a + 1);
    if (!out) return NULL;
    memcpy(out, s + a, b - a);
    out[b - a] = '\0';
    return out;
}

/* --- En-tetes (unfolding RFC 5322) ------------------------------------------ */

typedef struct { char* name; char* value; } MimeHeader;   /* name en casse d'origine */

/* Parse le bloc d'en-tetes de [seg, seg+seglen). Renvoie les en-tetes unfoldes
 * (continuation = ligne suivante debutant par espace/tab, CRLF retire, blancs
 * de repli conserves). *body_off recoit l'offset du corps (apres la ligne
 * vide), ou seglen si aucun corps. Retourne le nombre d'en-tetes ; *out est
 * alloue (a liberer via free_headers). */
static int parse_headers(const char* seg, size_t seglen,
                         size_t* body_off, MimeHeader** out) {
    MimeHeader* hs = NULL;
    int n = 0, cap = 0;
    *body_off = seglen;

    size_t i = 0;
    while (i < seglen) {
        size_t ls = i, le = ls;
        while (le < seglen && seg[le] != '\n') le++;
        size_t ce = le;
        if (ce > ls && seg[ce - 1] == '\r') ce--;
        size_t next = (le < seglen) ? le + 1 : seglen;

        if (ce == ls) { *body_off = next; break; }   /* ligne vide = fin */

        if ((seg[ls] == ' ' || seg[ls] == '\t') && n > 0) {
            /* Continuation : on ajoute le contenu brut (blancs de tete inclus). */
            char* old = hs[n - 1].value;
            size_t ol = old ? strlen(old) : 0;
            size_t add = ce - ls;
            char* nv = malloc(ol + add + 1);
            if (nv) {
                if (old) memcpy(nv, old, ol);
                memcpy(nv + ol, seg + ls, add);
                nv[ol + add] = '\0';
                free(old);
                hs[n - 1].value = nv;
            }
        } else {
            size_t colon = ls;
            while (colon < ce && seg[colon] != ':') colon++;
            if (colon < ce) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 8;
                    MimeHeader* nh = realloc(hs, (size_t)cap * sizeof(MimeHeader));
                    if (!nh) break;
                    hs = nh;
                }
                char* name = dup_trim(seg + ls, colon - ls);
                char* val  = dup_trim(seg + colon + 1, ce - (colon + 1));
                /* Casse d'origine conservee (hdr_get compare deja sans casse) :
                 * les en-tetes custom restitues doivent garder leur casse. */
                hs[n].name = name;
                hs[n].value = val;
                n++;
            }
        }
        i = next;
    }

    *out = hs;
    return n;
}

static void free_headers(MimeHeader* hs, int n) {
    for (int i = 0; i < n; i++) { free(hs[i].name); free(hs[i].value); }
    free(hs);
}

static const char* hdr_get(MimeHeader* hs, int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (hs[i].name && ci_streq(hs[i].name, name)) return hs[i].value;
    return NULL;
}

/* En-tete « standard » : soit mappe vers un champ d'Email, soit produit par
 * EmailConstruitSource (X-Priority) ou porteur de structure MIME (Content-*).
 * Ces en-tetes ne sont PAS redeposes dans Entête[] a l'import : seuls les
 * en-tetes libres (X- custom, List-*, References, etc.) y atterrissent. */
static int is_standard_header(const char* name) {
    static const char* const STD[] = {
        "from", "to", "cc", "bcc", "subject", "date", "message-id",
        "reply-to", "return-path", "mime-version", "x-priority", NULL
    };
    for (int i = 0; STD[i]; i++)
        if (ci_streq(name, STD[i])) return 1;
    if (ci_prefix(name, "content-")) return 1;   /* Content-Type / -Transfer-Encoding / -Disposition / -ID */
    return 0;
}

/* --- Content-Type / parametres ---------------------------------------------- */

/* Type de media seul (avant le premier ';'), trimme, minuscules. */
static char* mime_media_type(const char* value) {
    if (!value) return NULL;
    const char* semi = strchr(value, ';');
    size_t n = semi ? (size_t)(semi - value) : strlen(value);
    char* m = dup_trim(value, n);
    if (m) for (char* p = m; *p; p++) *p = (char)ci_char(*p);
    return m;
}

/* Valeur d'un parametre nomme (`key=val`, val eventuellement entre guillemets),
 * insensible a la casse pour la cle. Renvoie une copie heap ou NULL. */
static char* mime_param(const char* value, const char* key) {
    if (!value) return NULL;
    const char* p = strchr(value, ';');
    while (p) {
        p++;                                       /* apres ';' */
        while (*p == ' ' || *p == '\t') p++;
        const char* eq = p;
        while (*eq && *eq != '=' && *eq != ';') eq++;
        if (*eq == '=') {
            size_t klen = (size_t)(eq - p);
            if (strlen(key) == klen && ci_prefix(p, key)) {
                const char* v = eq + 1;
                while (*v == ' ' || *v == '\t') v++;
                if (*v == '"') {
                    v++;
                    const char* e = v;
                    while (*e && *e != '"') e++;
                    return dup_trim(v, (size_t)(e - v));
                }
                const char* e = v;
                while (*e && *e != ';') e++;
                return dup_trim(v, (size_t)(e - v));
            }
        }
        p = strchr(eq, ';');
    }
    return NULL;
}

/* --- Adresses --------------------------------------------------------------- */

/* Adresse nue depuis une forme `Nom <adresse>` (ou l'adresse seule). */
static char* address_only(const char* s, size_t n) {
    const char* lt = NULL;
    for (size_t i = 0; i < n; i++) if (s[i] == '<') { lt = s + i; break; }
    if (lt) {
        const char* gt = lt + 1;
        const char* end = s + n;
        while (gt < end && *gt != '>') gt++;
        return dup_trim(lt + 1, (size_t)(gt - (lt + 1)));
    }
    return dup_trim(s, n);
}

static void push_string(ObjArray* arr, const char* s) {
    if (!s || s[0] == '\0') return;
    ObjString* str = obj_copy_string(s, (int)strlen(s));
    obj_array_push(arr, OBJ_VAL(str));   /* canon : root interne avant grow */
}

/* Depose un en-tete libre dans Entête[] : clone d'emailEntête rempli {Nom,
 * Valeur} puis pousse dans le tableau de l'instance parente (deja rootee). */
static void push_custom_header(ObjInstance* e, const char* name, const char* value) {
    Value hV = owl_clone_global_template("emailEntête");
    if (!IS_INSTANCE(hV)) return;
    vm_push(hV);                                 /* root avant toute alloc */
    ObjInstance* h = AS_INSTANCE(hV);
    inst_field_set_str(h, "Nom",    name  ? name  : "");
    inst_field_set_str(h, "Valeur", value ? value : "");
    ObjArray* arr = email_array(e, "Entete");
    if (arr) obj_array_push(arr, hV);            /* hV rootee sur la pile */
    vm_pop();
}

/* Decoupe une liste d'adresses (separees par des virgules) et pousse chaque
 * adresse nue dans le tableau (deja rooted via l'instance parente). */
static void split_addresses(ObjArray* arr, const char* value) {
    if (!arr || !value) return;
    size_t i = 0, len = strlen(value);
    while (i <= len) {
        size_t start = i;
        while (i < len && value[i] != ',') i++;
        char* a = address_only(value + start, i - start);
        if (a) { push_string(arr, a); free(a); }
        if (i >= len) break;
        i++;                                       /* saute la virgule */
    }
}

/* --- Decodage transfer-encoding --------------------------------------------- */

/* Decode le corps d'une partie selon Content-Transfer-Encoding. Renvoie une
 * zone heap NUL-terminee (le \0 n'est pas compte dans *outlen), ou NULL si le
 * decodage echoue (base64 corrompu). Best-effort : jamais de plantage. */
static char* transfer_decode(const char* cte, const char* body, size_t blen,
                             size_t* outlen) {
    *outlen = 0;
    char* enc = cte ? dup_trim(cte, strlen(cte)) : NULL;
    int is_b64 = enc && ci_streq(enc, "base64");
    int is_qp  = enc && (ci_streq(enc, "quoted-printable") || ci_streq(enc, "quotedprintable"));
    free(enc);

    if (is_b64) {
        char* clean = malloc(blen + 1);
        if (!clean) return NULL;
        size_t cn = 0;
        for (size_t i = 0; i < blen; i++) if (!is_ws(body[i])) clean[cn++] = body[i];
        clean[cn] = '\0';
        unsigned char* raw = malloc(cn + 1);
        if (!raw) { free(clean); return NULL; }
        size_t olen = 0;
        int rc = mbedtls_base64_decode(raw, cn + 1, &olen,
                                       (const unsigned char*)clean, cn);
        free(clean);
        if (rc != 0) { free(raw); return NULL; }   /* corrompu -> best-effort */
        raw[olen] = '\0';
        *outlen = olen;
        return (char*)raw;
    }
    if (is_qp) return qp_decode(body, blen, outlen);

    /* 7bit / 8bit / binary / absent : corps tel quel. */
    char* d = malloc(blen + 1);
    if (!d) return NULL;
    memcpy(d, body, blen);
    d[blen] = '\0';
    *outlen = blen;
    return d;
}

/* --- Traitement d'une entite MIME ------------------------------------------- */

static void mime_entity(ObjInstance* e, const char* seg, size_t seglen,
                        int is_top, int depth);

/* Feuille : range le corps decode dans Message / HTML, ou fabrique une piece
 * jointe (contenu binaire = Buffer d'octets decodes). */
static void mime_leaf(ObjInstance* e, MimeHeader* hs, int nh,
                      const char* body, size_t blen) {
    const char* ct   = hdr_get(hs, nh, "content-type");
    const char* cte  = hdr_get(hs, nh, "content-transfer-encoding");
    const char* disp = hdr_get(hs, nh, "content-disposition");
    const char* cid  = hdr_get(hs, nh, "content-id");

    char* media = mime_media_type(ct);
    const char* mt = media ? media : "text/plain";

    char* fname = disp ? mime_param(disp, "filename") : NULL;
    char* nparam = ct ? mime_param(ct, "name") : NULL;

    /* Le CRLF precedant la frontiere appartient au delimiteur : on retire un
     * unique retour de ligne terminal du corps de la feuille. */
    size_t bl = blen;
    if (bl >= 2 && body[bl - 2] == '\r' && body[bl - 1] == '\n') bl -= 2;
    else if (bl >= 1 && body[bl - 1] == '\n') bl -= 1;

    int force_attach = (disp && ci_prefix(disp, "attachment")) || fname != NULL;
    int is_html  = ci_streq(mt, "text/html");
    int is_plain = ci_streq(mt, "text/plain");

    size_t dl = 0;
    char* dec = transfer_decode(cte, body, bl, &dl);

    if (!force_attach && is_html) {
        inst_field_set_str(e, "HTML", dec ? dec : "");
    } else if (!force_attach && is_plain) {
        inst_field_set_str(e, "Message", dec ? dec : "");
    } else {
        /* Piece jointe : instance emailAttache clonee du template. */
        Value attV = owl_clone_global_template("emailAttache");
        if (IS_INSTANCE(attV)) {
            vm_push(attV);                          /* root avant toute alloc */
            ObjInstance* att = AS_INSTANCE(attV);
            ObjBuffer* buf = (dec && dl > 0) ? obj_new_buffer(dec, (int)dl)
                                             : obj_buffer_empty(0);
            inst_field_set_value(att, "Contenu", OBJ_VAL(buf));  /* canon root */
            inst_field_set_str(att, "ContentType", mt);
            const char* nom = fname ? fname : (nparam ? nparam : "");
            inst_field_set_str(att, "Nom", nom);
            if (cid && cid[0]) {
                char* idc = dup_trim(cid, strlen(cid));
                /* Retrait des chevrons encadrants s'ils sont presents. */
                const char* idv = idc ? idc : "";
                size_t il = strlen(idv);
                if (il >= 2 && idv[0] == '<' && idv[il - 1] == '>') {
                    char* inner = malloc(il - 1);
                    if (inner) { memcpy(inner, idv + 1, il - 2); inner[il - 2] = '\0';
                                 inst_field_set_str(att, "Identifiant", inner); free(inner); }
                } else {
                    inst_field_set_str(att, "Identifiant", idv);
                }
                free(idc);
            }
            ObjArray* arr = email_array(e, "Attache");
            if (arr) obj_array_push(arr, attV);     /* attV rooted sur la pile */
            vm_pop();
        }
    }

    free(dec);
    free(media);
    free(fname);
    free(nparam);
}

/* Verifie si la ligne [ls, ce) est un delimiteur `--boundary`. *is_close = 1
 * si suivi de `--` (frontiere de cloture). */
static int is_boundary_line(const char* seg, size_t ls, size_t ce,
                            const char* boundary, int* is_close) {
    size_t blen = strlen(boundary);
    if (ce - ls < 2 + blen) return 0;
    if (seg[ls] != '-' || seg[ls + 1] != '-') return 0;
    if (memcmp(seg + ls + 2, boundary, blen) != 0) return 0;
    size_t rest = ls + 2 + blen;
    *is_close = (ce - rest >= 2 && seg[rest] == '-' && seg[rest + 1] == '-');
    return 1;
}

/* Parcourt les parties delimitees par `boundary` et recurse sur chacune. */
static void mime_multipart(ObjInstance* e, const char* seg, size_t start,
                           size_t seglen, const char* boundary, int depth) {
    size_t part_start = (size_t)-1;
    size_t i = start;
    while (i <= seglen) {
        size_t ls = i, le = ls;
        while (le < seglen && seg[le] != '\n') le++;
        size_t ce = le;
        if (ce > ls && seg[ce - 1] == '\r') ce--;
        size_t next = (le < seglen) ? le + 1 : seglen;

        int is_close = 0;
        if (is_boundary_line(seg, ls, ce, boundary, &is_close)) {
            if (part_start != (size_t)-1) {
                size_t pe = ls;                     /* CRLF avant delim = delim */
                if (pe >= 2 && seg[pe - 2] == '\r' && seg[pe - 1] == '\n') pe -= 2;
                else if (pe >= 1 && seg[pe - 1] == '\n') pe -= 1;
                mime_entity(e, seg + part_start, pe - part_start, 0, depth + 1);
            }
            if (is_close) return;
            part_start = next;
        }
        if (le >= seglen) break;
        i = next;
    }
}

static void mime_entity(ObjInstance* e, const char* seg, size_t seglen,
                        int is_top, int depth) {
    if (depth > MIME_MAX_DEPTH) return;

    size_t body_off = seglen;
    MimeHeader* hs = NULL;
    int nh = parse_headers(seg, seglen, &body_off, &hs);

    if (is_top) {
        const char* from = hdr_get(hs, nh, "from");
        const char* to   = hdr_get(hs, nh, "to");
        const char* cc   = hdr_get(hs, nh, "cc");
        const char* subj = hdr_get(hs, nh, "subject");
        const char* mid  = hdr_get(hs, nh, "message-id");

        if (from) {
            char* addr = address_only(from, strlen(from));
            if (addr) { inst_field_set_str(e, "Expediteur", addr); free(addr); }
        }
        if (to) split_addresses(email_array(e, "Destinataire"), to);
        if (cc) split_addresses(email_array(e, "Cc"), cc);
        if (subj) {
            char* dec = rfc2047_decode(subj, strlen(subj), NULL);
            inst_field_set_str(e, "Sujet", dec ? dec : subj);
            free(dec);
        }
        if (mid) {
            char* m = dup_trim(mid, strlen(mid));
            const char* mv = m ? m : "";
            size_t ml = strlen(mv);
            if (ml >= 2 && mv[0] == '<' && mv[ml - 1] == '>') {
                char* inner = malloc(ml - 1);
                if (inner) { memcpy(inner, mv + 1, ml - 2); inner[ml - 2] = '\0';
                             inst_field_set_str(e, "MessageID", inner); free(inner); }
            } else {
                inst_field_set_str(e, "MessageID", mv);
            }
            free(m);
        }

        /* En-tetes non standard rencontres -> Entête[] (X-*, List-*, References…).
         * Les en-tetes mappes ci-dessus ou porteurs de structure ne sont pas
         * redeposes (pas de doublon). */
        for (int i = 0; i < nh; i++) {
            if (!hs[i].name || !hs[i].value) continue;
            if (is_standard_header(hs[i].name)) continue;
            push_custom_header(e, hs[i].name, hs[i].value);
        }
    }

    const char* ct = hdr_get(hs, nh, "content-type");
    char* media = mime_media_type(ct);
    if (media && ci_prefix(media, "multipart/")) {
        char* boundary = mime_param(ct, "boundary");
        if (boundary && boundary[0]) {
            mime_multipart(e, seg, body_off, seglen, boundary, depth);
        }
        free(boundary);
        free(media);
        free_headers(hs, nh);
        return;                                     /* conteneur : rien a poser */
    }
    free(media);

    mime_leaf(e, hs, nh, seg + body_off, seglen - body_off);
    free_headers(hs, nh);
}

static Value fn_email_importe_source(int argCount, Value* args) {
    (void)argCount;
    if (!IS_INSTANCE(args[0]) || !is_email_instance(AS_INSTANCE(args[0]))) {
        const char* actual = "<non-structuré>";
        if (IS_INSTANCE(args[0]) && AS_INSTANCE(args[0])->structName != NULL)
            actual = AS_INSTANCE(args[0])->structName->chars;
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EmailImporteSource",
                              "native", "EmailImporteSource", "arg", "1",
                              "expected", "Email", "actual", actual, NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* e = AS_INSTANCE(args[0]);

    /* Copie C decouplee du storage : le remplissage alloue (GC) et l'aliasing
     * avec le champ Source en cours de vie serait fragile. */
    const char* src0 = email_str(e, "Source");
    size_t slen = strlen(src0);
    if (slen == 0) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "EmailImporteSource",
                              "native", "EmailImporteSource", NULL);
        return BOOL_VAL(false);
    }
    char* src = malloc(slen + 1);
    if (src == NULL) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "EmailImporteSource",
                              "native", "EmailImporteSource", NULL);
        return BOOL_VAL(false);
    }
    memcpy(src, src0, slen + 1);

    /* Faux si aucun en-tete n'a pu etre isole (source non structuree). */
    size_t body_off = 0;
    MimeHeader* probe = NULL;
    int nh = parse_headers(src, slen, &body_off, &probe);
    free_headers(probe, nh);
    if (nh == 0) { free(src); return BOOL_VAL(false); }

    mime_entity(e, src, slen, 1, 0);   /* e rooted (args[0] sur la pile VM) */
    free(src);
    return BOOL_VAL(true);
}

/* ============================================================
 * Transport SMTP (RFC 5321) — client texte-ligne au-dessus de net_tcp / TLS.
 *
 * Patron : stdlib_ftp.c. Le canal SMTP est une conversation ligne a ligne :
 * chaque reponse serveur est un code a 3 chiffres suivi d'un texte,
 * eventuellement multiligne ("250-...\r\n250 OK\r\n" : '-' = continuation,
 * ' ' = ligne terminale). Un seul socket, dispatch clair/chiffre par le flag
 * tls_active (STARTTLS promeut le canal en cours de route, comme FTPES).
 *
 * La session ouverte est un handle opaque (OwlSmtpSession*) cache dans le champ
 * _handle de l'instance emailSessionSMTP, alloue a l'ouverture, libere a la
 * fermeture / a l'echec. VERIFY_FULL conserve : CertificatCA n'ajoute
 * qu'une autorite privee, il ne relache pas la verification.
 * ============================================================ */

typedef struct {
    OwlTlsSocket sock;         /* .tcp = transport TCP (clair ou chiffre) */
    bool   tls_active;         /* handshake TLS effectue -> router via owl_tls_* */
    char   rbuf[4096];
    size_t rlen;               /* octets valides dans rbuf */
    size_t rpos;               /* prochain octet a consommer */
    bool   cap_starttls;       /* EHLO a annonce STARTTLS */
    bool   cap_auth_plain;     /* AUTH PLAIN annonce */
    bool   cap_auth_login;     /* AUTH LOGIN annonce */
    bool   cap_auth_cram_md5;  /* AUTH CRAM-MD5 annonce */
} OwlSmtpSession;

/* ---- Dispatch canal : TLS si le handshake a eu lieu, sinon le TCP clair. ---- */
static int smtp_recv(OwlSmtpSession* s, void* buf, size_t len) {
    return s->tls_active ? owl_tls_recv(&s->sock, buf, len)
                         : owl_tcp_recv(&s->sock.tcp, buf, len);
}
static int smtp_send(OwlSmtpSession* s, const void* buf, size_t len) {
    return s->tls_active ? owl_tls_send(&s->sock, buf, len)
                         : owl_tcp_send(&s->sock.tcp, buf, len);
}

/* Lit une ligne complete (jusqu'au \n) du canal ; le \r\n final est retire.
 * Retourne false sur fermeture ou erreur. */
static bool smtp_read_line(OwlSmtpSession* s, char* out, size_t out_size) {
    size_t n = 0;
    for (;;) {
        if (s->rpos >= s->rlen) {
            int got = smtp_recv(s, s->rbuf, sizeof(s->rbuf));
            if (got <= 0) return false;
            s->rlen = (size_t)got;
            s->rpos = 0;
        }
        char ch = s->rbuf[s->rpos++];
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return true;
        }
        if (n + 1 < out_size) out[n++] = ch;
    }
}

/* Lit une reponse complete (mono ou multiligne) et renvoie son code a 3
 * chiffres. Les lignes de continuation ("NNN-...") sont accumulees dans text
 * (indispensable a EHLO : les capacites vivent sur ces lignes). -1 sur echec. */
static int smtp_read_reply(OwlSmtpSession* s, char* text, size_t text_size) {
    char line[1024];
    if (!smtp_read_line(s, line, sizeof(line))) return -1;
    if (strlen(line) < 3) return -1;

    char code[4] = { line[0], line[1], line[2], '\0' };
    int codeN = atoi(code);
    size_t tlen = 0;
    if (text && text_size) {
        int w = snprintf(text, text_size, "%s", strlen(line) > 4 ? line + 4 : line + 3);
        tlen = (w > 0) ? (size_t)w : 0;
        if (tlen >= text_size) tlen = text_size - 1;
    }

    if (line[3] == '-') {
        for (;;) {
            if (!smtp_read_line(s, line, sizeof(line))) return -1;
            bool terminal = (strlen(line) >= 4 &&
                             strncmp(line, code, 3) == 0 && line[3] == ' ');
            if (text && text_size && tlen + 1 < text_size) {
                int w = snprintf(text + tlen, text_size - tlen, "\n%s", line);
                if (w > 0) tlen += (size_t)w;
                if (tlen >= text_size) tlen = text_size - 1;
            }
            if (terminal) break;
        }
    }
    return codeN;
}

/* Envoie une commande + CRLF sur le canal. */
static bool smtp_send_cmd(OwlSmtpSession* s, const char* fmt, ...) {
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n > sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\r';
    buf[n++] = '\n';
    return smtp_send(s, buf, (size_t)n) == n;
}

/* Envoie une commande puis lit sa reponse. Retourne le code (ou -1). */
static int smtp_command(OwlSmtpSession* s, char* text, size_t text_size,
                        const char* fmt, ...) {
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!smtp_send_cmd(s, "%s", buf)) return -1;
    return smtp_read_reply(s, text, text_size);
}

/* Analyse le texte d'une reponse EHLO et arme les capacites reconnues. Le
 * texte est une ligne par mot-cle d'extension (SIZE, STARTTLS, AUTH ...). */
static void smtp_parse_ehlo(OwlSmtpSession* s, const char* feat) {
    s->cap_starttls = s->cap_auth_plain = s->cap_auth_login = s->cap_auth_cram_md5 = false;
    if (feat == NULL) return;
    if (strstr(feat, "STARTTLS") || strstr(feat, "starttls")) s->cap_starttls = true;
    /* Mecanismes AUTH : la ligne "AUTH PLAIN LOGIN CRAM-MD5" liste les
     * mecanismes supportes, separes par des espaces. */
    if (strstr(feat, "PLAIN"))    s->cap_auth_plain    = true;
    if (strstr(feat, "LOGIN"))    s->cap_auth_login    = true;
    if (strstr(feat, "CRAM-MD5")) s->cap_auth_cram_md5 = true;
}

static int smtp_ehlo(OwlSmtpSession* s, const char* domain) {
    char feat[2048];
    int code = smtp_command(s, feat, sizeof(feat), "EHLO %s",
                            (domain && domain[0]) ? domain : "owl.local");
    if (code == 250) smtp_parse_ehlo(s, feat);
    return code;
}

/* Ferme et libere la session (sans QUIT — usage interne sur echec). */
static void smtp_free(OwlSmtpSession* s) {
    if (s == NULL) return;
    if (s->tls_active) owl_tls_close(&s->sock);
    else               owl_tcp_close(&s->sock.tcp);
    free(s);
}

/* ---- Encodage base64 heap (NUL-termine). NULL sur echec. ---- */
static char* b64_encode(const unsigned char* data, size_t len) {
    size_t need = 0;
    mbedtls_base64_encode(NULL, 0, &need, data, len);   /* dimensionne */
    char* out = malloc(need + 1);
    if (!out) return NULL;
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char*)out, need + 1, &olen, data, len) != 0) {
        free(out);
        return NULL;
    }
    out[olen] = '\0';
    return out;
}

/* ---- Authentification : mecanisme choisi selon les capacites annoncees.
 * CRAM-MD5 prefere (aucun secret en clair) puis LOGIN puis PLAIN. Retourne
 * true si l'authentification a reussi (235). ---- */
static bool smtp_authenticate(OwlSmtpSession* s, const char* user, const char* pass) {
    if (s->cap_auth_cram_md5) {
        char chal[1024];
        if (smtp_command(s, chal, sizeof(chal), "AUTH CRAM-MD5") != 334) return false;

        /* Le defi est en base64 : le decoder pour calculer le HMAC dessus. */
        unsigned char dec[512];
        size_t dlen = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec), &dlen,
                                  (const unsigned char*)chal, strlen(chal)) != 0)
            return false;

        unsigned char mac[16];
        const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
        if (!info) return false;
        if (mbedtls_md_hmac(info, (const unsigned char*)pass, strlen(pass),
                            dec, dlen, mac) != 0) return false;

        /* Reponse = "utilisateur <hex(hmac)>", encodee base64. */
        char resp[512];
        int rn = snprintf(resp, sizeof(resp), "%s ", user);
        for (int i = 0; i < 16 && rn + 2 < (int)sizeof(resp); i++)
            rn += snprintf(resp + rn, sizeof(resp) - rn, "%02x", mac[i]);

        char* b64 = b64_encode((const unsigned char*)resp, (size_t)rn);
        if (!b64) return false;
        int code = smtp_command(s, NULL, 0, "%s", b64);
        free(b64);
        return code == 235;
    }

    if (s->cap_auth_login) {
        if (smtp_command(s, NULL, 0, "AUTH LOGIN") != 334) return false;
        char* bu = b64_encode((const unsigned char*)user, strlen(user));
        if (!bu) return false;
        int c1 = smtp_command(s, NULL, 0, "%s", bu);
        free(bu);
        if (c1 != 334) return false;
        char* bp = b64_encode((const unsigned char*)pass, strlen(pass));
        if (!bp) return false;
        int c2 = smtp_command(s, NULL, 0, "%s", bp);
        free(bp);
        return c2 == 235;
    }

    if (s->cap_auth_plain) {
        /* AUTH PLAIN : "\0user\0pass" en base64 (RFC 4616). */
        size_t ul = strlen(user), pl = strlen(pass);
        size_t total = 1 + ul + 1 + pl;
        unsigned char* raw = malloc(total);
        if (!raw) return false;
        raw[0] = 0;
        memcpy(raw + 1, user, ul);
        raw[1 + ul] = 0;
        memcpy(raw + 2 + ul, pass, pl);
        char* b64 = b64_encode(raw, total);
        free(raw);
        if (!b64) return false;
        int code = smtp_command(s, NULL, 0, "AUTH PLAIN %s", b64);
        free(b64);
        return code == 235;
    }

    /* Aucun mecanisme utilisable annonce : rien a faire (best-effort). */
    return true;
}

/* Envoie la charge utile DATA avec dot-stuffing (RFC 5321 §4.5.2 : une ligne
 * qui commence par '.' voit ce '.' double) puis le terminateur <CRLF>.<CRLF>.
 * Le message est suppose en CRLF. */
static bool smtp_send_data(OwlSmtpSession* s, const char* msg, size_t len) {
    EBuf b;
    eb_init(&b);
    size_t i = 0;
    bool line_start = true;
    while (i < len) {
        if (line_start && msg[i] == '.') eb_put(&b, ".", 1);   /* dot-stuffing */
        char c = msg[i];
        eb_put(&b, &c, 1);
        line_start = (c == '\n');
        i++;
    }
    /* Garantit une fin de ligne avant le terminateur. */
    if (b.len == 0 || b.data[b.len - 1] != '\n') eb_puts(&b, "\r\n");
    eb_puts(&b, ".\r\n");

    char* payload = eb_finish(&b);
    if (payload == NULL) return false;
    size_t plen = strlen(payload);
    bool ok = ((size_t)smtp_send(s, payload, plen) == plen);
    free(payload);
    if (!ok) return false;
    return smtp_read_reply(s, NULL, 0) == 250;
}

/* ============================================================
 * Natives de session SMTP.
 * ============================================================ */

/* Resout un argument attendu emailSessionSMTP. NULL + erreur si mauvais type. */
static ObjInstance* resolve_smtp_session(Value v, const char* fn) {
    if (!IS_INSTANCE(v) || AS_INSTANCE(v)->structName == NULL ||
        strcmp(AS_INSTANCE(v)->structName->chars, "emailSessionSMTP") != 0) {
        const char* actual = "<non-structuré>";
        if (IS_INSTANCE(v) && AS_INSTANCE(v)->structName != NULL)
            actual = AS_INSTANCE(v)->structName->chars;
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "emailSessionSMTP", "actual", actual, NULL);
        return NULL;
    }
    return AS_INSTANCE(v);
}

static OwlSmtpSession* smtp_get_handle(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return NULL;
    return (OwlSmtpSession*)(intptr_t)AS_INTEGER(v);
}

static void smtp_set_handle(ObjInstance* inst, OwlSmtpSession* s) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)s);
}

static Value fn_email_ouvre_session(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* sess = resolve_smtp_session(args[0], "EmailOuvreSession");
    if (sess == NULL) return BOOL_VAL(false);

    /* Une session deja ouverte est fermee avant de rouvrir (pas de fuite). */
    OwlSmtpSession* prev = smtp_get_handle(sess);
    if (prev) { smtp_free(prev); smtp_set_handle(sess, NULL); }

    const char* host = email_str(sess, "AdresseServeur");
    int port    = email_int(sess, "Port");
    int option  = email_int(sess, "Option");
    const char* user = email_str(sess, "Nom");
    const char* pass = email_str(sess, "MotDePasse");
    const char* ca   = email_str(sess, "CertificatCA");
    int timeout = email_int(sess, "_timeout");
    if (timeout <= 0) timeout = EMAIL_TIMEOUT_DEFAUT_MS;
    if (port <= 0)    port = EMAIL_PORT_SMTP_DEFAUT;
    const char* extra_ca = (ca && ca[0]) ? ca : NULL;

    OwlSmtpSession* s = (OwlSmtpSession*)calloc(1, sizeof(OwlSmtpSession));
    if (s == NULL) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "EmailOuvreSession",
                              "native", "EmailOuvreSession", NULL);
        return BOOL_VAL(false);
    }

    owl_tcp_global_init();
    owl_tls_init(&s->sock);

    char cible[300];
    snprintf(cible, sizeof(cible), "%s:%d", host ? host : "", port);

    if (option == EMAIL_OPTION_SSL) {
        /* TLS implicite d'emblee (port 465 usuel). */
        if (!owl_tls_connect(&s->sock, host, port, OWL_TLS_VERIFY_FULL, timeout, extra_ca)) {
            owl_emit_native_error(E_NATIVE_EMAIL_CONNECT_FAILURE, "EmailOuvreSession",
                                  "native", "EmailOuvreSession", "cible", cible,
                                  "detail", owl_tls_last_error(&s->sock), NULL);
            smtp_free(s);
            return BOOL_VAL(false);
        }
        s->tls_active = true;
    } else {
        if (!owl_tcp_connect(&s->sock.tcp, host, port, timeout)) {
            owl_emit_native_error(E_NATIVE_EMAIL_CONNECT_FAILURE, "EmailOuvreSession",
                                  "native", "EmailOuvreSession", "cible", cible,
                                  "detail", owl_tcp_last_error(&s->sock.tcp), NULL);
            smtp_free(s);
            return BOOL_VAL(false);
        }
    }

    /* Banniere d'accueil : 220. */
    if (smtp_read_reply(s, NULL, 0) != 220) {
        owl_emit_native_error(E_NATIVE_EMAIL_CONNECT_FAILURE, "EmailOuvreSession",
                              "native", "EmailOuvreSession", "cible", cible,
                              "detail", "bannière d'accueil inattendue", NULL);
        smtp_free(s);
        return BOOL_VAL(false);
    }

    if (smtp_ehlo(s, host) != 250) {
        owl_emit_native_error(E_NATIVE_EMAIL_CONNECT_FAILURE, "EmailOuvreSession",
                              "native", "EmailOuvreSession", "cible", cible,
                              "detail", "EHLO refusé", NULL);
        smtp_free(s);
        return BOOL_VAL(false);
    }

    /* STARTTLS : bascule le canal clair en TLS puis re-EHLO (les capacites
     * annoncees en clair et en TLS peuvent differer). */
    if (option == EMAIL_OPTION_SECURISE_TLS) {
        if (smtp_command(s, NULL, 0, "STARTTLS") != 220 ||
            !owl_tls_start_tls(&s->sock, host, OWL_TLS_VERIFY_FULL, extra_ca)) {
            owl_emit_native_error(E_NATIVE_EMAIL_CONNECT_FAILURE, "EmailOuvreSession",
                                  "native", "EmailOuvreSession", "cible", cible,
                                  "detail", "négociation STARTTLS échouée", NULL);
            smtp_free(s);
            return BOOL_VAL(false);
        }
        s->tls_active = true;
        if (smtp_ehlo(s, host) != 250) {
            owl_emit_native_error(E_NATIVE_EMAIL_CONNECT_FAILURE, "EmailOuvreSession",
                                  "native", "EmailOuvreSession", "cible", cible,
                                  "detail", "EHLO (post-STARTTLS) refusé", NULL);
            smtp_free(s);
            return BOOL_VAL(false);
        }
    }

    /* Authentification si des identifiants sont renseignes. */
    if (user && user[0]) {
        if (!smtp_authenticate(s, user, pass ? pass : "")) {
            owl_emit_native_error(E_NATIVE_EMAIL_CONNECT_FAILURE, "EmailOuvreSession",
                                  "native", "EmailOuvreSession", "cible", cible,
                                  "detail", "authentification refusée", NULL);
            smtp_free(s);
            return BOOL_VAL(false);
        }
    }

    smtp_set_handle(sess, s);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Emet MAIL FROM / RCPT TO / DATA pour chaque destinataire To+Cc+Cci. */
static bool smtp_rcpt_array(OwlSmtpSession* s, ObjArray* arr) {
    if (arr == NULL) return true;
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (!IS_STRING(v) || AS_CSTRING(v)[0] == '\0') continue;
        int code = smtp_command(s, NULL, 0, "RCPT TO:<%s>", AS_CSTRING(v));
        if (code != 250 && code != 251) return false;
    }
    return true;
}

static Value fn_email_envoie_message(int argCount, Value* args) {
    ObjInstance* sess = resolve_smtp_session(args[0], "EmailEnvoieMessage");
    if (sess == NULL) return BOOL_VAL(false);

    OwlSmtpSession* s = smtp_get_handle(sess);
    if (s == NULL) {
        owl_emit_native_error(E_NATIVE_EMAIL_NOT_OPEN, "EmailEnvoieMessage",
                              "native", "EmailEnvoieMessage", NULL);
        return BOOL_VAL(false);
    }

    if (argCount < 2 || !IS_INSTANCE(args[1]) || !is_email_instance(AS_INSTANCE(args[1]))) {
        const char* actual = "<absent>";
        if (argCount >= 2 && IS_INSTANCE(args[1]) && AS_INSTANCE(args[1])->structName)
            actual = AS_INSTANCE(args[1])->structName->chars;
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EmailEnvoieMessage",
                              "native", "EmailEnvoieMessage", "arg", "2",
                              "expected", "Email", "actual", actual, NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* e = AS_INSTANCE(args[1]);
    int options = 0;
    if (argCount >= 3 && IS_INTEGER(args[2])) options = (int)AS_INTEGER(args[2]);

    const char* sender = email_str(e, "Expediteur");

    /* Enveloppe. */
    if (smtp_command(s, NULL, 0, "MAIL FROM:<%s>", sender ? sender : "") != 250) {
        owl_emit_native_error(E_NATIVE_EMAIL_SEND_FAILURE, "EmailEnvoieMessage",
                              "native", "EmailEnvoieMessage",
                              "detail", "MAIL FROM refusé", NULL);
        return BOOL_VAL(false);
    }
    if (!smtp_rcpt_array(s, email_array(e, "Destinataire")) ||
        !smtp_rcpt_array(s, email_array(e, "Cc")) ||
        !smtp_rcpt_array(s, email_array(e, "Cci"))) {
        owl_emit_native_error(E_NATIVE_EMAIL_SEND_FAILURE, "EmailEnvoieMessage",
                              "native", "EmailEnvoieMessage",
                              "detail", "destinataire refusé", NULL);
        return BOOL_VAL(false);
    }
    if (smtp_command(s, NULL, 0, "DATA") != 354) {
        owl_emit_native_error(E_NATIVE_EMAIL_SEND_FAILURE, "EmailEnvoieMessage",
                              "native", "EmailEnvoieMessage",
                              "detail", "DATA refusé", NULL);
        return BOOL_VAL(false);
    }

    char* src = build_mime_source(e, options);   /* e rootee (args[1] pile VM) */
    if (src == NULL) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "EmailEnvoieMessage",
                              "native", "EmailEnvoieMessage", NULL);
        return BOOL_VAL(false);
    }
    /* Depose aussi le brut dans ..Source (miroir de EmailConstruitSource). */
    inst_field_set_str(e, "Source", src);

    bool ok = smtp_send_data(s, src, strlen(src));
    free(src);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_EMAIL_SEND_FAILURE, "EmailEnvoieMessage",
                              "native", "EmailEnvoieMessage",
                              "detail", "message rejeté par le serveur", NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_email_ferme_session(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* sess = resolve_smtp_session(args[0], "EmailFermeSession");
    if (sess == NULL) return NIL_VAL;

    OwlSmtpSession* s = smtp_get_handle(sess);
    if (s != NULL) {
        smtp_command(s, NULL, 0, "QUIT");   /* best-effort, on ferme quoi qu'il arrive */
        smtp_free(s);
        smtp_set_handle(sess, NULL);
    }
    owl_clear_error();
    return NIL_VAL;
}

static Value fn_email_change_timeout(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* sess = resolve_smtp_session(args[0], "EmailChangeTimeOut");
    if (sess == NULL) return NIL_VAL;
    int64_t delai = IS_INTEGER(args[1]) ? AS_INTEGER(args[1]) : EMAIL_TIMEOUT_DEFAUT_MS;
    if (delai <= 0) delai = EMAIL_TIMEOUT_DEFAUT_MS;
    inst_field_set_int(sess, "_timeout", delai);
    owl_clear_error();
    return NIL_VAL;
}

/* --- EmailChargeFichierAttaché : lit un fichier local en piece jointe. ------
 * Type MIME devine sur l'extension (liste minimale usuelle), sinon
 * application/octet-stream. Nom de la PJ = nom du fichier (sans le chemin). */

static const char* email_content_type_from_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (dot == NULL) return "application/octet-stream";
    const char* e = dot + 1;
    struct { const char* ext; const char* ct; } map[] = {
        {"txt", "text/plain"},   {"htm", "text/html"},   {"html", "text/html"},
        {"csv", "text/csv"},     {"pdf", "application/pdf"},
        {"json", "application/json"}, {"xml", "application/xml"},
        {"zip", "application/zip"},
        {"png", "image/png"},    {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},    {"svg", "image/svg+xml"},
        {"doc", "application/msword"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xls", "application/vnd.ms-excel"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {NULL, NULL}
    };
    for (int i = 0; map[i].ext; i++) {
        /* comparaison insensible a la casse sur l'extension */
        const char* a = e; const char* b = map[i].ext;
        while (*a && *b && ci_char(*a) == ci_char(*b)) { a++; b++; }
        if (*a == '\0' && *b == '\0') return map[i].ct;
    }
    return "application/octet-stream";
}

/* Nom de fichier seul depuis un chemin (dernier segment apres / ou \). */
static const char* email_basename(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static Value fn_email_charge_fichier_attache(int argCount, Value* args) {
    (void)argCount;
    if (!IS_INSTANCE(args[0]) || !is_email_instance(AS_INSTANCE(args[0]))) {
        const char* actual = "<non-structuré>";
        if (IS_INSTANCE(args[0]) && AS_INSTANCE(args[0])->structName != NULL)
            actual = AS_INSTANCE(args[0])->structName->chars;
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EmailChargeFichierAttaché",
                              "native", "EmailChargeFichierAttaché", "arg", "1",
                              "expected", "Email", "actual", actual, NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "EmailChargeFichierAttaché",
                              "native", "EmailChargeFichierAttaché", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* e = AS_INSTANCE(args[0]);
    const char* chemin = AS_CSTRING(args[1]);

    FILE* f = fopen(chemin, "rb");
    if (f == NULL) {
        owl_emit_native_error(E_NATIVE_IO_OPEN_FAILURE, "EmailChargeFichierAttaché",
                              "native", "EmailChargeFichierAttaché",
                              "path", chemin, "detail", "fichier introuvable", NULL);
        return BOOL_VAL(false);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) sz = 0;
    fseek(f, 0, SEEK_SET);
    char* data = (sz > 0) ? malloc((size_t)sz) : NULL;
    size_t n = (data && sz > 0) ? fread(data, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (sz > 0 && data == NULL) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "EmailChargeFichierAttaché",
                              "native", "EmailChargeFichierAttaché", NULL);
        return BOOL_VAL(false);
    }

    /* Cree l'emailAttache (clone du template), remplit, pousse dans ..Attache. */
    Value attV = owl_clone_global_template("emailAttache");
    if (!IS_INSTANCE(attV)) {
        free(data);
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "EmailChargeFichierAttaché",
                              "native", "EmailChargeFichierAttaché", NULL);
        return BOOL_VAL(false);
    }
    vm_push(attV);                              /* root avant toute alloc */
    ObjInstance* att = AS_INSTANCE(attV);
    ObjBuffer* buf = (data && n > 0) ? obj_new_buffer(data, (int)n) : obj_buffer_empty(0);
    inst_field_set_value(att, "Contenu", OBJ_VAL(buf));   /* canon : root interne */
    inst_field_set_str(att, "ContentType", email_content_type_from_ext(chemin));
    inst_field_set_str(att, "Nom", email_basename(chemin));
    ObjArray* arr = email_array(e, "Attache");
    if (arr) obj_array_push(arr, attV);         /* attV rooted sur la pile */
    vm_pop();

    free(data);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ============================================================
 * Definition native.
 * ============================================================ */

static const OwlParam P_CONSTRUIT_SOURCE[] = {
    {"email",   "Email",  "message dont la propriété Source doit être remplie"},
    {"options", "entier", "options de construction combinables (emailOptionEncodeEntête, emailOptionEntêteCCi)"},
    {0}
};

#define ECS_RET "vrai si le message brut a été généré et déposé dans la propriété Source, faux sinon"
static const OwlSignature SIG_CONSTRUIT_SOURCE[] = {
    {1, P_CONSTRUIT_SOURCE, "booléen", ECS_RET},
    {2, P_CONSTRUIT_SOURCE, "booléen", ECS_RET},
    {0}
};

static const OwlParam P_IMPORTE_SOURCE[] = {
    {"email", "Email", "message dont la propriété Source contient le message brut à analyser"},
    {0}
};

#define EIS_RET "vrai si le message brut a été analysé et les champs de l'Email remplis, faux sinon"
static const OwlSignature SIG_IMPORTE_SOURCE[] = {
    {1, P_IMPORTE_SOURCE, "booléen", EIS_RET},
    {0}
};

static const OwlParam P_OUVRE_SESSION[] = {
    {"session", "emailSessionSMTP", "session d'envoi renseignée (adresse serveur, port, option de chiffrement, identifiants éventuels)"},
    {0}
};
static const OwlSignature SIG_OUVRE_SESSION[] = {
    {1, P_OUVRE_SESSION, "booléen", "vrai si la session a été ouverte (connexion, EHLO, chiffrement et authentification éventuels), faux sinon"},
    {0}
};

static const OwlParam P_ENVOIE_MESSAGE[] = {
    {"session", "emailSessionSMTP", "session d'envoi préalablement ouverte"},
    {"email",   "Email",            "message à envoyer"},
    {"options", "entier",           "options de construction du message combinables (emailOptionEncodeEntête, emailOptionEntêteCCi)"},
    {0}
};
#define EEM_RET "vrai si le message a été accepté par le serveur d'envoi, faux sinon"
static const OwlSignature SIG_ENVOIE_MESSAGE[] = {
    {2, P_ENVOIE_MESSAGE, "booléen", EEM_RET},
    {3, P_ENVOIE_MESSAGE, "booléen", EEM_RET},
    {0}
};

static const OwlParam P_FERME_SESSION[] = {
    {"session", "emailSessionSMTP", "session d'envoi à fermer"},
    {0}
};
static const OwlSignature SIG_FERME_SESSION[] = {
    {1, P_FERME_SESSION, "rien", NULL},
    {0}
};

static const OwlParam P_CHANGE_TIMEOUT[] = {
    {"session", "emailSessionSMTP", "session d'envoi concernée"},
    {"délai",   "entier",           "délai maximal d'attente du serveur, en millisecondes"},
    {0}
};
static const OwlSignature SIG_CHANGE_TIMEOUT[] = {
    {2, P_CHANGE_TIMEOUT, "rien", NULL},
    {0}
};

static const OwlParam P_CHARGE_ATTACHE[] = {
    {"email",  "Email",  "message auquel ajouter la pièce jointe"},
    {"chemin", "chaîne", "nom et chemin du fichier local à joindre"},
    {0}
};
static const OwlSignature SIG_CHARGE_ATTACHE[] = {
    {2, P_CHARGE_ATTACHE, "booléen", "vrai si le fichier a été lu et ajouté aux pièces jointes du message, faux sinon"},
    {0}
};

static const OwlNativeDef DEFS_EMAIL[] = {
    { .canonical      = "EmailConstruitSource",
      .canonicalAscii = NULL,
      .nameEN         = "EmailBuildSource",
      .category       = "Email",
      .descFR         = "Construit le message brut (en-têtes et corps MIME, fins de ligne CRLF) d'un Email et le dépose dans sa propriété Source, base d'un fichier .eml.",
      .descEN         = "Builds the raw message (MIME headers and body, CRLF line endings) of an Email and stores it in its Source property, the basis of an .eml file.",
      .signatures     = SIG_CONSTRUIT_SOURCE,
      .impl           = fn_email_construit_source },
    { .canonical      = "EmailImporteSource",
      .canonicalAscii = NULL,
      .nameEN         = "EmailImportSource",
      .category       = "Email",
      .descFR         = "Analyse le message brut contenu dans la propriété Source d'un Email (en-têtes, corps MIME, pièces jointes) et en remplit les champs expéditeur, destinataires, sujet, corps texte/HTML et pièces jointes ; base de la lecture d'un fichier .eml.",
      .descEN         = "Parses the raw message held in an Email's Source property (headers, MIME body, attachments) and fills its sender, recipients, subject, text/HTML body and attachment fields; the basis for reading an .eml file.",
      .signatures     = SIG_IMPORTE_SOURCE,
      .impl           = fn_email_importe_source },
    { .canonical      = "EmailOuvreSession",
      .canonicalAscii = NULL,
      .nameEN         = "EmailStartSession",
      .category       = "Email",
      .descFR         = "Ouvre une session d'envoi vers un serveur SMTP : connexion, présentation, chiffrement (TLS direct ou STARTTLS selon l'option) et authentification si des identifiants sont renseignés.",
      .descEN         = "Opens an outgoing session to an SMTP server: connection, greeting, encryption (direct TLS or STARTTLS depending on the option) and authentication when credentials are set.",
      .signatures     = SIG_OUVRE_SESSION,
      .impl           = fn_email_ouvre_session },
    { .canonical      = "EmailEnvoieMessage",
      .canonicalAscii = NULL,
      .nameEN         = "EmailSendMessage",
      .category       = "Email",
      .descFR         = "Envoie un Email au travers d'une session d'envoi déjà ouverte : dépose l'expéditeur, chaque destinataire (principaux, copies et copies cachées) puis le message construit à la volée.",
      .descEN         = "Sends an Email through an already-open outgoing session: submits the sender, each recipient (primary, copies and blind copies) then the message built on the fly.",
      .signatures     = SIG_ENVOIE_MESSAGE,
      .impl           = fn_email_envoie_message },
    { .canonical      = "EmailFermeSession",
      .canonicalAscii = NULL,
      .nameEN         = "EmailCloseSession",
      .category       = "Email",
      .descFR         = "Ferme une session d'envoi ouverte et libère la connexion au serveur.",
      .descEN         = "Closes an open outgoing session and releases the server connection.",
      .signatures     = SIG_FERME_SESSION,
      .impl           = fn_email_ferme_session },
    { .canonical      = "EmailChangeTimeOut",
      .canonicalAscii = NULL,
      .nameEN         = "EmailSetTimeOut",
      .category       = "Email",
      .descFR         = "Change le délai maximal d'attente du serveur d'envoi pour une session, en millisecondes. Passé ce délai sans réponse, l'ouverture de la session échoue.",
      .descEN         = "Changes the maximum wait time for the outgoing server on a session, in milliseconds. Past this delay without a reply, opening the session fails.",
      .signatures     = SIG_CHANGE_TIMEOUT,
      .impl           = fn_email_change_timeout },
    { .canonical      = "EmailChargeFichierAttaché",
      .canonicalAscii = "EmailChargeFichierAttache",
      .nameEN         = "EmailLoadAttachment",
      .category       = "Email",
      .descFR         = "Charge un fichier local en pièce jointe d'un Email : lit son contenu, déduit le type MIME de l'extension et ajoute une pièce jointe portant le nom du fichier.",
      .descEN         = "Loads a local file as an Email attachment: reads its content, infers the MIME type from the extension and adds an attachment named after the file.",
      .signatures     = SIG_CHARGE_ATTACHE,
      .impl           = fn_email_charge_fichier_attache },
};

/* ============================================================
 * Init.
 * ============================================================ */

void stdlib_email_init(void) {
    owl_registry_register_module(DEFS_EMAIL,
        (int)(sizeof(DEFS_EMAIL) / sizeof(DEFS_EMAIL[0])));
    owl_registry_register_types(TYPES_EMAIL,
        (int)(sizeof(TYPES_EMAIL) / sizeof(TYPES_EMAIL[0])));
    owl_registry_register_consts(CONSTS_EMAIL,
        (int)(sizeof(CONSTS_EMAIL) / sizeof(CONSTS_EMAIL[0])));

    ObjInstance* tpl;

    tpl = build_Email_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("Email", OBJ_VAL(tpl));
    vm_pop();

    tpl = build_emailSessionSMTP_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("emailSessionSMTP", OBJ_VAL(tpl));
    vm_pop();

    tpl = build_emailAttache_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("emailAttache", OBJ_VAL(tpl));
    vm_pop();

    tpl = build_emailEntete_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("emailEntête", OBJ_VAL(tpl));
    vm_pop();
}
