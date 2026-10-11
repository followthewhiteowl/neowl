// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_limits.h"
#include "ui_catalog.h"
#include "stdlib_pdf_defs.h"   /* donnees de registre PDF (Qt-free, console + GUI) */
#include "stdlib_ui_defs.h"    /* donnees de registre UI  (Qt-free, console + GUI) */

#ifdef OWL_HAS_GUI
#include "stdlib_ui.h"
#include "stdlib_pdf.h"
#endif

void stdlib_init(void) {
    /* Garde idempotente. Si le registry contient deja des
     * types, on a deja ete init dans ce cycle -> no-op, pour eviter
     * de re-pousser g_natives/g_types/g_consts/g_enums (pas de dedup
     * cote owl_registry_register_module, sinon doublerait les comptes
     * exposes par owl_search/owl_list MCP).
     *
     * Sentinel = types_count (pas natives_count) parce que
     * vm_init() appelle register_vm_internals() AVANT stdlib_init() ;
     * register_vm_internals() pousse une dizaine de natives internes
     * (__StructNew, ExceptionDeclenche, ...) mais aucun type. Le 1er
     * type est enregistre par register_builtin_types() en fin de
     * stdlib_init() — donc types_count > 0 implique stdlib_init()
     * deja execute dans le cycle courant.
     *
     * Compatible avec vm_init() reentrant utilise par les tests stress
     * GC : vm_init() commence par owl_registry_free() qui reset les
     * counts a 0, puis appelle stdlib_init() qui repeuple normalement
     * (cf. vm.c:603-644). Pas de state global supplementaire a
     * synchroniser. */
    if (owl_registry_types_count() > 0) return;

    /* Limites compile-time : registry static const, init no-op aujourd'hui,
     * mais branché ici pour homogénéité avec natives/types/consts/enums. */
    owl_limits_init();
    methods_init();
    stdlib_io_init();
    stdlib_string_init();
    stdlib_math_init();
    stdlib_array_init();
    stdlib_vecteur_init();      /*  : type Vecteur/Vector + conversions */
    stdlib_matrice_init();      /*  : type Matrice/Matrix + conversions */
    stdlib_tenseur_init();      /*  : type Tenseur/Tensor + moteur autograd */
    stdlib_iamodele_init();     /*  : type IAModèle/AIModel + couches + Infère */
    stdlib_mlarbre_init();      /*  : type MLArbreDécision/MLDecisionTree (CART) */
    stdlib_mllineaire_init();   /*  : types MLRégressionLinéaire + MLRégressionLogistique */
    stdlib_mlcluster_init();    /*  : type MLKMeans (clustering non-supervisé) */
    stdlib_llm_init();          /*  : type LLM (GGUF) + OptionsGénération */
    stdlib_file_init();
    stdlib_json_init();
    stdlib_date_init();
    stdlib_crypto_init();
    stdlib_regex_init();
    stdlib_http_init();
    stdlib_http_service_init(); /*  : surface OWL du DSL de service (FluxÉcrit) */
    stdlib_ftp_init();          /*  : type ftpClient + client FTP en clair */
    stdlib_email_init();        /*  : types Email/emailSessionSMTP/emailAttache + constantes (Lot 0) */
    stdlib_email_msg_init();    /*  : native EmailImporteMSG (import .msg Outlook, Lot 5) */
    stdlib_mq_init();           /* socle Message Queue E1 : init central neutre, avant tout client MQ */
    stdlib_mqtt_init();         /*  : type mqttSession + client MQTT 3.1.1 */
    stdlib_amqp_init();         /*  : type amqpConnection + client AMQP 0-9-1 (RabbitMQ) */
    stdlib_stomp_init();        /*  : types stompSession/stompMessage + client STOMP 1.2 */
    stdlib_nats_init();         /*  : types natsConnexion/natsMessage + client NATS */
    stdlib_aws_init();          /*  : type AWSSession + enum awsService + AWSRequête */
    stdlib_bitwise_init();
    stdlib_similarity_init();   /* SimilaritéTexte : TexteDistance (socle famille) */
    stdlib_reflect_init();
    stdlib_console_init();
    stdlib_eval_init();
    stdlib_erreur_init();
    stdlib_timer_init();        /*  : type Timer (TimerDémarre/Arrête) */
    stdlib_compression_init();
    stdlib_color_init();
    stdlib_markdown_init();
    stdlib_text_riche_init();
    stdlib_mock_init();
    stdlib_xml_init();
    stdlib_image_init();        /* Lot I.1 : type Image natif (load/save raster) */
    stdlib_dataset_init();      /*  : ImageVersTenseur + types ImageÉtiquetée/Dataset + chargeurs */
    stdlib_detection_init();    /*  : IoU + SuppressionNonMax/NonMaxSuppression */
    stdlib_xls_init();          /*  : type xlsDocument + ecriture/lecture .xlsx (OOXML) */
    stdlib_icones_init();       /* Type Icone + enum Forme (catalogue SVG embarque) */
    stdlib_codebarres_init();   /*  : type CodeBarres + generation (zint) */
    stdlib_theme_init();        /*  : namespace theme.<nom> (styles nommes) */
#ifdef OWL_HAS_SQL
    stdlib_sql_init();          /* SQL.2.a : moteur SQL embarque (SQLite V1) */
#endif
    stdlib_uuid_init();         /* DonneUUID + méthode UUID.VersChaîne */
    stdlib_i18n_init();         /*  lot A : type langue + enum langueXxx */
    stdlib_parametre_init();    /*  : valeurs persistantes (ParamètreInitialise & co) */
    stdlib_ini_init();          /*  : fichiers .ini section-conscients (INILit / INIEcrit) */
    stdlib_runtime_init();      /* OwlInfoLibrairies — manifest des libs embarquees */
    stdlib_ui_catalog_init();   /* Phase D : enums + constantes UI (TBValider, ...) */
#ifdef OWL_HAS_GUI
    stdlib_ui_impls_wire();     /* remplit DEFS[].impl UI AVANT register -> chemin GUI normal */
    stdlib_ui_init();           /* Phase E : hooks Qt (get/set prop, timer, rasterizer, ...) */
    stdlib_pdf_impls_wire();    /* remplit DEFS[].impl PDF AVANT register -> chemin GUI normal */
#endif
    /* INCONDITIONNEL (console comme GUI) : le registre annonce les signatures
     * des natives UI + Etats/PDF (et les enums/consts UI) pour que le compilateur
     * les resolve a l'identique. Hors GUI, .impl reste NULL (jamais dispatche :
     * contextes compile-only, owl-cli --check + portail de compile d'owl-build). */
    stdlib_ui_defs_register();  /* Info, Erreur, ChampsDe, fSelecteur, ... + enums UI */
    stdlib_pdf_defs_register(); /* Etats V1 : iImprimeEtat, iLigne, iCadre, iImprime, ... */
    register_builtin_types();
    /* Native dispatcher __InstanceCoerce — apres que tous les modules
     * stdlib_xxx_init() ont eu l'occasion d'enregistrer leurs parsers
     * de coerce. Coherent avec le pattern owl_register_method qui
     * accumule les bindings module par module. */
    owl_register_instance_coerce_native();
}
