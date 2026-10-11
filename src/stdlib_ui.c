// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ui.c - Natives Tier 1 GUI.
 *
 * Compile seulement dans owl.exe (pas owl-cli). Voir CMakeLists.txt.
 *
 * Etat Phase E (V1) :
 *   OK Info / Erreur / Avertissement / Confirmer / OuiNon : implem complete
 *      via owl_ui_message_*. Aligne doc 3021011/3021013/3021009/
 *      3021007/3021005.
 *   OK Ouvre(FEN_X) : lookup OwlWindowDecl, instancie window + widgets
 *      (Libelle/Bouton/Saisie) avec props Tier 1A, run loop bloquante.
 *      Aligne doc 3038035.
 *   OK Ferme([fen,] [val]) : ferme + ValeurRenvoyee. Aligne doc 3038018.
 *   OK Ouvre retourne la ValeurRenvoyee posee par Ferme(val).
 *   OK Cablage PROCEDURE Initialisation/Clic via owl_ui_*_on_click.
 *   OK Bulle / ToucheRaccourci / TypeBouton(TBValider) / MaxLongueur cables.
 *   OK LireSaisie(nomCtrl) : lecture runtime du Texte d'une saisie.
 *   stub OuvreFille / FinProgramme : implem minimale (FinProgramme
 *      affiche message + exit ; OuvreFille no-op).
 *   V1.5+ : syntaxe canon 'nomCtrl..Texte' via type ObjUiControl ;
 *      multi-fenetres ; PROCEDURE Modification/Validation/Entree/Sortie.
 */

#include "stdlib_ui.h"
#include "stdlib_ui_defs.h"  /* DEFS Qt-free + stdlib_ui_defs_set_impl (wire) */
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_errors.h"
#include "owl_ui.h"
#include "ui_decl.h"
#include "ui_catalog.h"
#include "icone_render_qt.h"  /*  Cas K : polymorphisme BTN..Image = ICN */
#include "icons_catalog.h"    /*  : owl_icones_catalog_find pour "icones.<nom>" */
#include "etat_decl.h"   /*  : owl_etat_prepare_props sur OwlEtatDecl */
#include "owl_image.h"  /*  : dCopieImage* construit des OwlImage* */
#include "scanner.h"
#include "sentinels.h"
#include "event_catalog_widgets.h"  /*  : vue dedup events widget (evt*) */
#include "testing.h"
#include "vm.h"
#include "gc.h"  /*  : gc_mark_object/gc_mark_value pour stdlib_ui_mark_roots */
#include "object.h"
#include "memory.h"
#include "table.h"
#include "stdlib_date_parsers.h"  /*  : owl_parse_date_multi pour typSaisieDate Qt-first */
#include "mock.h"  /*  : mock_try_intercept_fn (routage TableVersPressePapier -> VersPressePapier) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ====================================================================
 *  Etat global (V1 single-window, V1.5 stack pour multi-fenetres)
 * ==================================================================== */

static OwlWindow* g_current_window = NULL;
static bool       g_ui_initialized = false;
static Value      g_return_value   = NIL_VAL;  /* ValeurRenvoyee de la fenetre courante (E.4) */

/*  — defini plus bas (pres de ui_move_focus_to). Enregistre comme
 * callback de synchro focused_index a l'init UI. */
static void ui_focus_sync_from_qt(OwlControl* now);

static void ensure_ui_init(void) {
    if (!g_ui_initialized) {
        owl_ui_init();
        owl_ui_set_focus_sync_callback(ui_focus_sync_from_qt);
        g_ui_initialized = true;
    }
}

/* ====================================================================
 *  Tables des controles vivants (Phase E.X)
 *
 *  A 'build_control', on cree un ObjUiControl et on le pose dans
 *  vm.globals[ctrlName]. Le compileur le voit comme un global ; le
 *  runtime peut faire 'ctrlName..PropName' qui dispatche via
 *  owl_ui_control_get_prop().
 * ==================================================================== */

/* ====================================================================
 *  Bindings d'evenements (Phase E.3)
 *
 *  Chaque PROCEDURE Clic / Initialisation / etc. d'un controle ou d'une
 *  fenetre est un OwlEventBlock contenant un ObjFunction*. On relie
 *  ces fn aux callbacks Qt via une table indexed-by-int (passee en
 *  userdata). Reset a chaque Ouvre — les bindings sont per-window.
 * ==================================================================== */

typedef struct {
    ObjFunction* fn;
    bool         is_window_event;  /* true = arity 1 (MaFenetre uniquement)
                                    * false = arity 2 (MaFenetre + MonChamp) */
    /*  — nom du widget/fenetre emetteur (strippe, ASCII case-insensible).
     * Sert a resoudre vm.globals[<name>] -> ObjUiControlRef/ObjWindowRef qu'on
     * push sur vm.event_sender_stack avant l'appel pour que MoiMême resolve.
     * NULL = pas de widget connu (call_event_block legacy via call sender=NIL). */
    char         sender_name[128];
    /*  — menu de la loupe d'un Champ Table : quand non vide, MoiMeme
     * doit etre la COLONNE cliquee, pas le champ. sender_name porte alors le nom
     * du Champ Table, sender_col le nom canon de la colonne : dispatch_event
     * construit le proxy-colonne (TABLE.COL, ) et le pousse comme sender.
     * Vide = comportement historique (sender = widget nomme par sender_name). */
    char         sender_col[128];
} EventBinding;

/*  — table de bindings DYNAMIQUE (grow, plus de drop silencieux).
 * BINDINGS_INIT_CAP est la capacite initiale ; au-dela, register_binding
 * double la capacite par realloc au lieu de dropper le binding (une UI a
 * beaucoup d'evenements perdait sinon tout handler au-dela de 256). Meme
 * discipline que g_inst_widget_watches / g_style_root_watches.
 * L'acces est TOUJOURS par indice (dispatch_event lit g_bindings[idx]),
 * jamais un pointeur cache : le realloc (nouveau buffer, memes indices) est
 * donc safe. Le buffer persiste entre cycles VM ; seul le count est remis a
 * 0 par reset_bindings (les ObjFunction* sont maintenus vivants ailleurs). */
#define BINDINGS_INIT_CAP 256
static EventBinding* g_bindings = NULL;
static int           g_binding_count = 0;
static int           g_binding_cap = 0;

static void reset_bindings(void) { g_binding_count = 0; }

/*  — profondeur de handler OWL "en vol".
 *
 * Incrémenté autour de chaque dispatch d'événement UI (clic, saisie, etc.)
 * ET du tic de Timer. Sert au timer à NE PAS s'intercaler dans un handler
 * déjà en cours : un handler qui monte une boucle Qt imbriquée (Temporise,
 * Ouvre modal, dialogue) laisse le QTimer tirer ; si on ré-entrait alors la
 * procédure du timer, elle s'exécuterait par-dessus la pile d'un handler
 * suspendu (event_sender erroné, ordre d'exécution surprenant pour le dev).
 * Un timer n'a pas besoin de rattrapage : on saute le tic et on reprend au
 * suivant (modèle "skip if busy", pas de file d'attente). Mono-thread (thread
 * Qt principal = thread VM) : un simple compteur suffit, aucune atomique. */
static int g_owl_handler_depth = 0;
static void owl_handler_enter(void) { g_owl_handler_depth++; }
static void owl_handler_leave(void) { if (g_owl_handler_depth > 0) g_owl_handler_depth--; }
static bool owl_handler_busy(void)  { return g_owl_handler_depth > 0; }

/*  — Ouvre imbrique : la table g_bindings est un tableau GLOBAL plat
 * indexe par les userdata Qt (dispatch_event lit g_bindings[idx]). Un Ouvre
 * imbrique (depuis un handler d'une autre fenetre) ne doit PAS faire
 * reset_bindings() : ca ecraserait les bindings de la fenetre parente, dont
 * les signaux Qt portent encore leurs anciens index. Au retour, ces index
 * pointeraient sur les bindings de la fenetre fille (ou hors borne) =>
 * mauvais handler appele / dispatch silencieusement ignore. La fenetre
 * parente "ne repond plus".
 *
 * Contrat save/restore : on memorise le compte courant avant d'ouvrir la
 * fille (mark), les bindings de la fille s'AJOUTENT a partir de la (append),
 * et on restaure le compte a la sortie de la boucle modale fille. Les index
 * de la parente redeviennent alors valides.
 *
 * Cible design (Option B, ) : bindings reellement per-window
 * rattaches a l'ObjWindowInstance, dispatch via la fenetre emettrice. Ce
 * save/restore est le correctif chirurgical en attendant l'infra de test GUI. */
static int  bindings_mark(void)        { return g_binding_count; }
static void bindings_restore(int mark) {
    /* Restaure le compte a la marque prise avant l'Ouvre fille. Garde
     * hors-borne : un mark negatif ou superieur au compte courant est
     * ignore (jamais d'extension du compte, jamais d'index negatif). */
    if (mark >= 0 && mark <= g_binding_count) g_binding_count = mark;
}

static int register_binding(ObjFunction* fn, bool is_window) {
    if (g_binding_count >= g_binding_cap) {
        int newCap = g_binding_cap < BINDINGS_INIT_CAP
                     ? BINDINGS_INIT_CAP : g_binding_cap * 2;
        EventBinding* grown = (EventBinding*)realloc(
            g_bindings, (size_t)newCap * sizeof(EventBinding));
        if (!grown) return -1;  /* OOM — degrade sans crash */
        g_bindings = grown;
        g_binding_cap = newCap;
    }
    g_bindings[g_binding_count].fn = fn;
    g_bindings[g_binding_count].is_window_event = is_window;
    g_bindings[g_binding_count].sender_name[0] = '\0';
    g_bindings[g_binding_count].sender_col[0] = '\0';
    return g_binding_count++;
}

/*  — variante qui attache le nom du widget/fenetre emetteur, pour
 * que dispatch_event puisse pousser le bon Value sur event_sender_stack
 * avant le callback. Le nom est attendu deja strippe (le caller utilise
 * scanner_strip_accents). Si name == NULL ou trop long : reset (NIL). */
static int register_binding_named(ObjFunction* fn, bool is_window,
                                   const char* sender_name) {
    int idx = register_binding(fn, is_window);
    if (idx < 0) return idx;
    if (sender_name) {
        size_t n = strlen(sender_name);
        if (n < sizeof(g_bindings[idx].sender_name)) {
            memcpy(g_bindings[idx].sender_name, sender_name, n + 1);
        }
    }
    return idx;
}

/*  — variante menu-loupe : sender = COLONNE cliquee. sender_name porte
 * le nom du Champ Table, sender_col le nom canon de la colonne. dispatch_event
 * reconstruit le proxy-colonne et le pousse comme MoiMeme. */
static int register_binding_named_col(ObjFunction* fn, bool is_window,
                                      const char* table_name,
                                      const char* col_name) {
    int idx = register_binding_named(fn, is_window, table_name);
    if (idx < 0) return idx;
    if (col_name) {
        size_t n = strlen(col_name);
        if (n < sizeof(g_bindings[idx].sender_col)) {
            memcpy(g_bindings[idx].sender_col, col_name, n + 1);
        }
    }
    return idx;
}

/*  — self-test du contrat bindings sous Ouvre imbrique. C pur, pas
 * de Qt ni de vm_init : on exerce directement la table g_bindings et le
 * mark/restore via des ObjFunction sentinelles (pointeurs distincts non
 * desreferencees). Definie ICI (pas dans internal_tests/) pour atteindre les
 * statics g_bindings / g_binding_count / bindings_mark / bindings_restore
 * sans les exporter. Cable via --self-test-ui-bindings (cf main.c). */
int ui_bindings_self_test_run(void) {
    int pass = 0, fail = 0;
    #define UIB_CHECK(expr, label) do { \
        if (expr) { pass++; } \
        else { fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
    } while (0)

    /* Sentinelles : 4 ObjFunction* factices, jamais desreferencees (le
     * self-test ne dispatch pas, il verifie l'arithmetique de la table). */
    ObjFunction* fnA0 = (ObjFunction*)(intptr_t)0x1001;
    ObjFunction* fnA1 = (ObjFunction*)(intptr_t)0x1002;
    ObjFunction* fnB0 = (ObjFunction*)(intptr_t)0x2001;
    ObjFunction* fnB1 = (ObjFunction*)(intptr_t)0x2002;

    /* --- Cycle 1 : fenetre parente A enregistre 2 bindings. --- */
    reset_bindings();
    int a0 = register_binding(fnA0, false);
    int a1 = register_binding(fnA1, true);
    UIB_CHECK(a0 == 0 && a1 == 1, "A : index 0,1");
    UIB_CHECK(g_binding_count == 2, "A : compte == 2");

    /* --- Ouvre imbrique B depuis un handler de A. --- */
    int mark = bindings_mark();
    UIB_CHECK(mark == 2, "mark == compte parente (2)");
    /* B ajoute (append, PAS reset) : index 2,3. */
    int b0 = register_binding(fnB0, false);
    int b1 = register_binding(fnB1, false);
    UIB_CHECK(b0 == 2 && b1 == 3, "B : append index 2,3 (pas d'ecrasement)");
    UIB_CHECK(g_binding_count == 4, "pendant B : compte == 4");

    /* B vit, ses handlers dispatchent correctement sur 2,3. */
    UIB_CHECK(g_bindings[b0].fn == fnB0 && g_bindings[b1].fn == fnB1,
              "B : table pointe les fn de B");

    /* --- B se ferme : restore le compte a la valeur d'avant l'Ouvre B. --- */
    bindings_restore(mark);
    UIB_CHECK(g_binding_count == 2, "apres Ferme(B) : compte restaure a 2");

    /* CONTRAT CRITIQUE : les bindings de la parente A sont INTACTS. Les
     * signaux Qt de A portent encore les index 0,1 -> doivent re-pointer
     * sur fnA0/fnA1, pas sur des residus de B. C'est l'assertion qui
     * rougit avec le STUB NO-OP (bindings_restore ne restaure rien et le
     * code buggy faisait reset_bindings() -> A ecrasee). */
    UIB_CHECK(g_bindings[a0].fn == fnA0, "A[0] intact apres cycle B (fnA0)");
    UIB_CHECK(g_bindings[a1].fn == fnA1, "A[1] intact apres cycle B (fnA1)");
    UIB_CHECK(g_bindings[a0].is_window_event == false &&
              g_bindings[a1].is_window_event == true,
              "A : flags is_window_event preserves");

    /* --- 2e cycle imbrique : re-ouvrir B doit rejouer proprement. --- */
    int mark2 = bindings_mark();
    UIB_CHECK(mark2 == 2, "2e cycle : mark == 2 (parente toujours a 2)");
    register_binding(fnB0, false);
    register_binding(fnB1, false);
    UIB_CHECK(g_binding_count == 4, "2e cycle B : compte == 4");
    bindings_restore(mark2);
    UIB_CHECK(g_binding_count == 2, "2e cycle : compte restaure a 2");
    UIB_CHECK(g_bindings[a0].fn == fnA0 && g_bindings[a1].fn == fnA1,
              "A intacte apres 2 cycles imbriques");

    /* --- Garde : restore avec un mark hors-borne ne corrompt pas. --- */
    bindings_restore(-1);
    UIB_CHECK(g_binding_count == 2, "restore(-1) ignore (garde)");
    bindings_restore(999);
    UIB_CHECK(g_binding_count == 2, "restore(>count) ignore (garde)");

    /*  — la table doit GRANDIR au-dela de la capacite initiale, pas
     * dropper silencieusement. On enregistre bien plus que la capacite
     * initiale (256) et on verifie qu'aucun binding n'est droppe : index
     * contigus 0..N-1, compte exact, et la table pointe les bonnes fn au-dela
     * de 256 (survit au realloc car l'acces est par indice). Avec l'ancienne
     * table statique[256], le 257e register_binding retournait -1 (drop). Les
     * checks d'index >256 sont gardes par g_binding_count pour ne PAS lire
     * hors-borne dans l'etat RED (short-circuit). */
    reset_bindings();
    {
        const int N = 300;  /* > 256 pour forcer au moins un grow */
        int firstDrop = -1;
        for (int i = 0; i < N; i++) {
            ObjFunction* fake = (ObjFunction*)(intptr_t)(0x4000 + i);
            int idx = register_binding(fake, (i % 2) == 0);
            if (idx != i) { firstDrop = i; break; }
        }
        /* Index runtime (pas des constantes) : evite que -Warray-bounds prouve
         * un acces hors-borne a la compilation dans l'etat RED ou la table est
         * encore statique[256]. Le guard g_binding_count > iN protege l'acces
         * a l'execution (short-circuit tant que la table n'a pas grandi). */
        int i256 = 256, i299 = 299;
        UIB_CHECK(firstDrop == -1, "grow: 300 bindings sans drop (index contigus 0..299)");
        UIB_CHECK(g_binding_count == N, "grow: compte == 300 (aucun drop au-dela de 256)");
        UIB_CHECK(g_binding_count > i256 &&
                  g_bindings[i256].fn == (ObjFunction*)(intptr_t)(0x4000 + 256),
                  "grow: g_bindings[256] intact apres realloc (257e non droppe)");
        UIB_CHECK(g_binding_count > i299 &&
                  g_bindings[i299].fn == (ObjFunction*)(intptr_t)(0x4000 + 299),
                  "grow: g_bindings[299] intact (dernier binding)");
        UIB_CHECK(g_binding_count > i256 &&
                  g_bindings[i256].is_window_event == ((256 % 2) == 0),
                  "grow: flag is_window_event du 257e preserve");
    }

    reset_bindings();
    #undef UIB_CHECK

    /* Resume self-test sur stdout (gate centralisation  :
     * stderr runtime reserve a owl_emit_*_error). */
    /* Garde ensemble vide : "ui-bindings: 0 PASS" est un vert vide. */
    if (pass + fail == 0) {
        fprintf(stdout, "ui-bindings: CORPUS VIDE — 0 cas execute. Un controle "
                        "qui n'examine rien ne prouve rien.\n");
        return 1;
    }
    if (fail > 0) fprintf(stdout, "ui-bindings: %d FAIL, %d PASS\n", fail, pass);
    else          fprintf(stdout, "ui-bindings: %d PASS\n", pass);
    return fail;
}

/*  — resout la Value widget associee a un nom (case+accent insensible)
 * via vm.globals[<strippe(name)>]. Retourne typiquement un ObjUiControlRef
 * (pour un champ) ou un ObjWindowRef (pour une fenetre), poses par le compilo
 * lors de la declaration. NIL_VAL si pas trouve — l'appelant push NIL et la
 * stack event_sender resout comme "pas de widget" -> soft error.
 *
 *  — la version publique est definie plus bas comme alias non-static
 * (signature exportee dans stdlib_ui.h pour reuse depuis stdlib_pdf.c). On
 * conserve l'inline `moimeme_widget_value_for_name` pour les call-sites du
 * meme TU. */
static Value moimeme_widget_value_for_name(const char* name) {
    if (!name) return NIL_VAL;
    char stripBuf[128];
    int  stripLen = scanner_strip_accents(name, (int)strlen(name),
                                          stripBuf, sizeof(stripBuf));
    if (stripLen <= 0) return NIL_VAL;
    ObjString* key = obj_copy_string(stripBuf, stripLen);
    Value v;
    /* Pas besoin de root explicite — table_get ne realloc pas. */
    if (!table_get(&vm.globals, key, &v)) return NIL_VAL;
    return v;
}

/*  — export public (signature dans stdlib_ui.h). Delegue au helper
 * statique. */
Value owl_moimeme_widget_value_for_name(const char* name) {
    return moimeme_widget_value_for_name(name);
}

/*  — construit un proxy-colonne (TABLE.COL, ) : un ObjInstance
 * leger portant la ref du Champ Table + le nom canon de la colonne, EXACTEMENT
 * la forme que produit `TBL.COL` (cf. vm.c OP_STRUCT_GET) et qu'accepte
 * table_colproxy_resolve. Pousse comme sender d'une option de menu loupe ->
 * MoiMeme = la colonne, branchable direct sur les natifs Table.
 * `tableRefVal` doit etre un ObjUiControlRef de Table ; `colCanon` le nom de
 * colonne deja strippe. GC-safe (helpers canon inst_field_set_*). */
static Value table_build_colproxy_value(Value tableRefVal, const char* colCanon) {
    if (!IS_UI_CONTROL_REF(tableRefVal) || !colCanon || !*colCanon)
        return NIL_VAL;
    ObjInstance* proxy = obj_new_instance(NULL);
    vm_push(OBJ_VAL(proxy));   /* root pendant les set (grow -> GC) */
    inst_field_set_value(proxy, OWL_TABLE_COLPROXY_REF_KEY, tableRefVal);
    inst_field_set_str(proxy, OWL_TABLE_COLPROXY_COL_KEY, colCanon);
    vm_pop();
    return OBJ_VAL(proxy);
}

/* Appelle une ObjFunction OWL avec les slots auto Phase C remplis.
 * V1 : MaFenetre/MonChamp = NIL_VAL (lecture runtime des props V1.5+).
 *
 *  — push/pop event_sender autour de l'appel pour que MoiMême
 * resolve au widget courant. `senderValue` peut etre NIL_VAL (cas
 * defensif : l'appelant n'a pas su resoudre le widget). */
static Value call_event_block_ret(ObjFunction* fn, bool is_window,
                                  Value senderValue) {
    if (!fn) return NIL_VAL;
    owl_handler_enter();
    vm_event_sender_push(senderValue);
    vm_push(OBJ_VAL(fn));
    vm_push(NIL_VAL);              /* MaFenetre */
    int argCount = 1;
    if (!is_window) {
        vm_push(NIL_VAL);          /* MonChamp */
        argCount = 2;
    }
    vm_call_reentrant(OBJ_VAL(fn), argCount);
    /* call_reentrant laisse la valeur de retour sur la stack. */
    Value ret = vm_pop();
    vm_event_sender_pop();
    owl_handler_leave();
    return ret;
}

static void call_event_block_with_sender(ObjFunction* fn, bool is_window,
                                         Value senderValue) {
    (void)call_event_block_ret(fn, is_window, senderValue);
}

/* Wrapper retro-compatible : appelle avec sender = NIL (cas ou l'appelant
 * n'a pas la decl sous la main). MoiMême tombera en stack vide -> soft error
 * si jamais le user l'utilise dans ce contexte. */
static void call_event_block(ObjFunction* fn, bool is_window) {
    call_event_block_with_sender(fn, is_window, NIL_VAL);
}

/* Callback generique attache a Qt. userdata = index dans g_bindings. */
static void dispatch_event(OwlControl* sender, void* userdata) {
    (void)sender;
    int idx = (int)(intptr_t)userdata;
    if (idx < 0 || idx >= g_binding_count) return;
    EventBinding* b = &g_bindings[idx];
    /*  — resout le widget Value via sender_name si connu, sinon NIL.
     *  — menu loupe : sender_col non vide => MoiMeme = la COLONNE
     * (proxy TABLE.COL reconstruit depuis la ref du Champ Table + le nom de
     * colonne), pas le champ. Meme mecanisme de push que TestSimuleMenu 3-args. */
    Value widget;
    if (b->sender_col[0] != '\0') {
        Value tableRef = moimeme_widget_value_for_name(b->sender_name);
        widget = table_build_colproxy_value(tableRef, b->sender_col);
    } else {
        widget = (b->sender_name[0] != '\0')
                 ? moimeme_widget_value_for_name(b->sender_name)
                 : NIL_VAL;
    }
    call_event_block_with_sender(b->fn, b->is_window_event, widget);
}

/* Variante booleenne de dispatch_event, pour l'autorisation de fermeture : la
 * valeur renvoyee par le handler OWL decide si la fermeture se poursuit. Un
 * handler qui ne renvoie rien (NIL) ne s'oppose pas. */
static int dispatch_close_query(OwlControl* sender, void* userdata) {
    (void)sender;
    int idx = (int)(intptr_t)userdata;
    if (idx < 0 || idx >= g_binding_count) return 1;
    EventBinding* b = &g_bindings[idx];
    if (!b->fn) return 1;
    Value widget = (b->sender_name[0] != '\0')
                   ? moimeme_widget_value_for_name(b->sender_name)
                   : NIL_VAL;
    Value ret = call_event_block_ret(b->fn, b->is_window_event, widget);
    return (IS_BOOL(ret) && !AS_BOOL(ret)) ? 0 : 1;
}

/* ============================================================
 * Dispatchers typés pour les events Champ Image V2.
 *
 * La PROC OWL a une arity fixée par sa déclaration. Le compiler valide
 * que les params déclarés correspondent au catalogue (cf compiler.c
 * ligne ~7080). Au dispatch, on push exactement autant de slots que
 * fn->arity, en remplissant : MaFenetre, MonChamp, puis params canon
 * de l'event dans l'ordre du catalogue (jusqu'à ce que arity soit
 * atteinte). Les params canon non utilisés sont silencieusement skippes.
 * ============================================================ */
static void dispatch_image_event_n(OwlControl* sender, void* userdata,
                                    int payloadCount, const Value* payload) {
    (void)sender;
    int idx = (int)(intptr_t)userdata;
    if (idx < 0 || idx >= g_binding_count) return;
    EventBinding* b = &g_bindings[idx];
    if (!b->fn) return;
    owl_handler_enter();
    /*  — push event_sender autour de l'appel (heritage chain MoiMême). */
    Value widget = (b->sender_name[0] != '\0')
                   ? moimeme_widget_value_for_name(b->sender_name)
                   : NIL_VAL;
    vm_event_sender_push(widget);
    int arity = b->fn->arity;
    /* Slots synthetiques implicites selon le contexte :
     *   - event de CHAMP   : MaFenetre + MonChamp (2 slots), puis payload.
     *   - event de FENETRE : MaFenetre seul (1 slot), puis payload.
     * ( — les events clavier fenetre n'ont pas de MonChamp ; sans
     * cette distinction le 1er payload tomberait dans le slot MonChamp.) */
    int synthSlots = b->is_window_event ? 1 : 2;
    vm_push(OBJ_VAL(b->fn));
    if (arity >= 1) vm_push(NIL_VAL);                 /* MaFenetre */
    if (!b->is_window_event && arity >= 2) vm_push(NIL_VAL);  /* MonChamp */
    int dataSlots = arity - synthSlots;
    if (dataSlots < 0) dataSlots = 0;
    if (dataSlots > payloadCount) dataSlots = payloadCount;
    for (int i = 0; i < dataSlots; i++) vm_push(payload[i]);
    vm_call_reentrant(OBJ_VAL(b->fn), arity);
    vm_pop();
    vm_event_sender_pop();
    owl_handler_leave();
}

/* Wrappers concrets — signatures fixées par owl_ui.h. */
static void dispatch_image_event_0(OwlControl* sender, void* userdata) {
    dispatch_image_event_n(sender, userdata, 0, NULL);
}
static void dispatch_image_event_1i(OwlControl* sender, void* userdata,
                                     int a) {
    Value payload[1] = { INTEGER_VAL(a) };
    dispatch_image_event_n(sender, userdata, 1, payload);
}
static void dispatch_image_event_2i(OwlControl* sender, void* userdata,
                                     int a, int b) {
    Value payload[2] = { INTEGER_VAL(a), INTEGER_VAL(b) };
    dispatch_image_event_n(sender, userdata, 2, payload);
}
static void dispatch_image_event_3i(OwlControl* sender, void* userdata,
                                     int a, int b, int c) {
    Value payload[3] = { INTEGER_VAL(a), INTEGER_VAL(b), INTEGER_VAL(c) };
    dispatch_image_event_n(sender, userdata, 3, payload);
}

/* Cherche un OwlEventBlock par nom (case+accent insensible).
 * Accepte n'importe lequel des 3 noms passes (typiquement surXxx, legacy, EN).
 * Les NULL sont ignores. */
/* Cherche un event par nom canon FR ou alias EN (case-insensible).
 * Le set canon est gere via owl_event_catalog_find — ici on fait juste
 * un strcasecmp sur les events attaches au widget par le compilo. */
/*  — compare deux noms d'event de maniere case ET accent insensible
 * (invariant 2 : `surToucheAppuyee` ecrit sans accent doit matcher la forme
 * canon accentuee `surToucheAppuyée` du catalogue). Strip d'abord les accents
 * des deux cotes puis strcasecmp. */
static bool event_name_eq(const char* stored, const char* canon) {
    if (!stored || !canon) return false;
    if (strcasecmp(stored, canon) == 0) return true;   /* fast path identique */
    char a[128], b[128];
    int la = scanner_strip_accents(stored, (int)strlen(stored), a, sizeof(a));
    int lb = scanner_strip_accents(canon,  (int)strlen(canon),  b, sizeof(b));
    if (la <= 0 || lb <= 0) return false;
    return strcasecmp(a, b) == 0;
}

static const OwlEventBlock* find_event(const OwlEventBlock* events, int n,
                                        const char* name_fr, const char* name_en) {
    for (int i = 0; i < n; i++) {
        if (name_fr && event_name_eq(events[i].name, name_fr)) return &events[i];
        if (name_en && event_name_eq(events[i].name, name_en)) return &events[i];
    }
    return NULL;
}

/* ====================================================================
 *  Helpers d'extraction de proprietes
 * ==================================================================== */

/* Recherche case+accent insensible d'une OwlPropertyInit par nom (FR ou
 * EN ou ASCII). Retourne NULL si absente. */
static const OwlPropertyInit* find_prop(const OwlPropertyInit* props, int n,
                                         const char* name) {
    for (int i = 0; i < n; i++) {
        if (strcasecmp(props[i].name, name) == 0) return &props[i];
    }
    return NULL;
}

/*  — invalide le cache de resolution des props DYNAMIQUES (eval !=
 * NULL) sur un decl PARTAGE, a appeler UNE fois en debut d'ouverture (par
 * fenetre, par controle, par etat).
 *
 * L'OwlPropertyInit vit dans le decl partage entre toutes les ouvertures
 * d'une meme fenetre. resolve_dynamic_props memoise is_constant=true apres
 * le 1er eval : sans invalidation, une prop dynamique (`Libelle = gTexte`)
 * resterait figee a la valeur de la 1re ouverture, alors que le top-level
 * re-tourne a chaque Ouvre et que la globale lue peut changer.
 *
 * On ne touche QUE les props a evaluateur : un literal pur a eval == NULL et
 * is_constant=true des le compile-time -> on le laisse (pas de re-eval inutile,
 * pas de cout). Le cache reste valide INTRA-ouverture (resolve_dynamic_props
 * peut etre appele plusieurs fois par ouverture, ex GUI seed + build_window) :
 * une seule eval reelle par ouverture, pas de double effet de bord. */
static void invalidate_dynamic_props_cache(OwlPropertyInit* props, int count) {
    for (int i = 0; i < count; i++) {
        if (props[i].eval) props[i].is_constant = false;
    }
}

/* V1.5-A : resout les props avec un eval ObjFunction (compile-time non
 * literal) en appelant la fonction 0-arg et en stockant le resultat dans
 * const_value. Apres ce passage, toutes les props sont en is_constant=true
 * et les helpers prop_int/prop_str/prop_value les voient normalement.
 * Idempotent intra-ouverture grace au cache is_constant ; le cache est
 * invalide entre ouvertures par invalidate_dynamic_props_cache. */
static void resolve_dynamic_props(OwlPropertyInit* props, int count) {
    for (int i = 0; i < count; i++) {
        OwlPropertyInit* p = &props[i];
        if (p->is_constant) continue;
        if (!p->eval) continue;
        vm_push(OBJ_VAL(p->eval));
        InterpretResult r = vm_call_reentrant(OBJ_VAL(p->eval), 0);
        if (r != INTERPRET_OK) {
            /* erreur runtime — leave NIL et stop. La VM a deja imprime. */
            p->const_value = NIL_VAL;
            p->is_constant = true;
            continue;
        }
        Value v = vm_pop();
        p->const_value = v;
        p->is_constant = true;
    }
}

/* Prepare le pipeline rendu d'un Etat avant lookup props.
 *
 * Resout les expressions dynamiques (eval ObjFunction 0-arg) sur les 3
 * niveaux d'un Etat :
 *   - props de l'Etat lui-meme (Format/Orientation/Marges et autres)
 *   - props de chaque bloc (Hauteur, Visible, CouleurFond...)
 *   - props de chaque sous-controle de chaque bloc (Libelle, Cadre, Police...)
 *
 * Idempotent : appele 1x par iImprimeEtat avant tout ctrl_find_prop.
 *
 * Les cascades homonymes (Cadre..Epaisseur + Cadre..Couleur en 2 lignes)
 * sont desormais regroupees en UN SEUL eval au compile-time,
 * donc plus besoin de fusion runtime (ex-merge_homonym_object_props). */
void owl_etat_prepare_props(OwlEtatDecl* etat) {
    if (!etat) return;
    /*  — invalide le cache de resolution des props DYNAMIQUES sur le
     * decl PARTAGE avant de re-resoudre : un Etat imprime plusieurs fois doit
     * re-evaluer ses props dynamiques (parite avec la reouverture de fenetre).
     * Litteraux purs non touches.
     *  — ne resout ICI que les props de l'Etat + des BLOCS. Les props
     * des CONTROLES (sous-champs) sont resolues plus tard via
     * owl_etat_resolve_control_props, APRES le top-level de l'Etat : une prop
     * dynamique de sous-champ (`Image = imgLogo`) peut referencer une globale
     * top-level Etat (ETAT_X$imgLogo) qui n'existe qu'une fois le top-level
     * execute. Parite stricte avec le chemin Fenetre. */
    invalidate_dynamic_props_cache(etat->properties, etat->property_count);
    for (int i = 0; i < etat->block_count; i++) {
        OwlEtatBlockDecl* b = &etat->blocks[i];
        invalidate_dynamic_props_cache(b->properties, b->property_count);
    }
    resolve_dynamic_props(etat->properties, etat->property_count);
    for (int i = 0; i < etat->block_count; i++) {
        OwlEtatBlockDecl* b = &etat->blocks[i];
        resolve_dynamic_props(b->properties, b->property_count);
    }
}

/*  — resout les props dynamiques des sous-champs d'un Etat APRES
 * l'execution de son top-level (les globales top-level `ETAT_X$var` sont
 * alors posees). Appele par fn_i_imprime_etat juste apres le top-level.
 * Invalide le cache avant (re-impression = re-eval, parite ). */
void owl_etat_resolve_control_props(OwlEtatDecl* etat) {
    if (!etat) return;
    for (int i = 0; i < etat->block_count; i++) {
        OwlEtatBlockDecl* b = &etat->blocks[i];
        for (int j = 0; j < b->control_count; j++) {
            OwlControlDecl* c = b->controls[j];
            if (!c) continue;
            invalidate_dynamic_props_cache(c->properties, c->property_count);
            resolve_dynamic_props(c->properties, c->property_count);
        }
    }
}

/*  — re-evalue UNE prop de controle a evaluateur (expression PROPRIETES)
 * avec la valeur COURANTE des variables referencees. Degel : une expression
 * `Libelle = sVal` reflete la valeur du moment a chaque rendu de bloc, au lieu
 * d'etre gelee a la 1re resolution (owl_etat_resolve_control_props ne tourne
 * qu'1x par iImprimeEtat). No-op sur un literal pur (eval == NULL) -> rendu
 * statique inchange. Le caller (render par bloc) lit const_value AVANT cet appel
 * comme temoin pour distinguer un override imperatif d'un simple seed. */
void owl_etat_prop_reeval(OwlPropertyInit* p) {
    if (!p || !p->eval) return;
    invalidate_dynamic_props_cache(p, 1);
    resolve_dynamic_props(p, 1);
}

static int prop_int(const OwlPropertyInit* props, int n,
                     const char* name, const char* nameEN, int dflt) {
    const OwlPropertyInit* p = find_prop(props, n, name);
    if (!p && nameEN) p = find_prop(props, n, nameEN);
    if (!p || !p->is_constant) return dflt;
    if (IS_INTEGER(p->const_value)) return (int)AS_INTEGER(p->const_value);
    return dflt;
}

static const char* prop_str(const OwlPropertyInit* props, int n,
                             const char* name, const char* nameEN, const char* dflt) {
    const OwlPropertyInit* p = find_prop(props, n, name);
    if (!p && nameEN) p = find_prop(props, n, nameEN);
    if (!p || !p->is_constant) return dflt;
    if (IS_STRING(p->const_value)) return AS_CSTRING(p->const_value);
    /*  : slot chaineMultilingue resolu vers langue courante.
     * Le literal inline `Libelle = {langueXxx: "..."}` compile en eval
     * non-const, donc on n'arrive ici que pour le cas chaine litterale
     * post-coerce. Defense quand meme : si l'eval a produit un ObjMultilingue
     * (cas affectation post-decl m..Libelle = uneCM), on resout. */
    if (IS_MULTILINGUE(p->const_value)) {
        Value resolved = multilingue_resolve(AS_MULTILINGUE(p->const_value),
                                             owl_i18n_langue_courante(),
                                             owl_i18n_langue_source());
        if (IS_STRING(resolved)) return AS_CSTRING(resolved);
    }
    return dflt;
}

/*  — taille de rasterisation d'une icone de controle.
 * override > 0 : taille forcee (..ImageTaille). Sinon auto : derivee
 * de la dimension du controle (hauteur du bouton, dimension du champ).
 * Bornes : plancher 16px (icone lisible), plafond 512px (pas de
 * rasterisation geante sur un controle demesure). */
#define OWL_ICONE_SIZE_MIN 16
#define OWL_ICONE_SIZE_MAX 512

static int icone_target_size(int control_size, int override_size) {
    int s = (override_size > 0) ? override_size : control_size;
    if (s < OWL_ICONE_SIZE_MIN) s = OWL_ICONE_SIZE_MIN;
    if (s > OWL_ICONE_SIZE_MAX) s = OWL_ICONE_SIZE_MAX;
    return s;
}

/* Rasterise un ObjIcone vers RGBA (taille auto = hauteur utile du bouton) et
 * pousse au QPushButton. Bloc partage par les chemins BTN..Image = icones.X
 * (creation + runtime). */
static void button_rasterize_icone_to_widget(OwlControl* w, ObjIcone* icn,
                                             int override_size) {
    if (icn == NULL || !icn->valide || icn->svg_raw == NULL
        || icn->svg_raw->length == 0) {
        return;
    }
    unsigned char* rgba = NULL;
    int iw = 0, ih = 0;
    bool want_bg = (icn->couleur_fond != -1);
    int icoSize = icone_target_size(owl_ui_control_get_height(w) - 16,
                                    override_size);
    if (owl_icone_rasterize_rgba(
            icn->svg_raw->chars, icn->svg_raw->length,
            /*  : -1 unset -> 0 (noir) defaut au lieu de 0xFFFFFF blanc. */
            (icn->couleur == -1) ? 0u : (unsigned int)(icn->couleur & 0xFFFFFF),
            icn->couleur_modifiable,
            icoSize, icn->rotation,
            icn->forme,
            (unsigned int)((want_bg ? icn->couleur_fond : 0) & 0xFFFFFF),
            want_bg,
            &rgba, &iw, &ih)) {
        owl_ui_button_set_icon_rgba(w, rgba, iw, ih);
        free(rgba);
    }
}

/*  — Resolveur d'icone Bouton depuis une chaine. Ordre :
 *   1. prefixe "icones." (case-insensible) -> catalogue embarque, rasterise
 *      via le meme chemin que la prop plate BTN..Image = icones.X.
 *   2. sinon -> chemin fichier disque / SVG inline, delegue a Qt (QIcon).
 * Chaine vide = pas d'icone (gere par owl_ui_button_set_icon). */
static void button_set_icon_from_string(OwlControl* w, const char* str,
                                        int override_size) {
    if (str == NULL) { owl_ui_button_set_icon(w, ""); return; }
    const char* nom = NULL;
    if (strncasecmp(str, "icones.", 7) == 0)      nom = str + 7;
    else if (strncasecmp(str, "icons.", 6) == 0)  nom = str + 6;
    if (nom != NULL) {
        const OwlIconCatalogEntry* entry = owl_icones_catalog_find(nom);
        if (entry != NULL) {
            Value v = owl_icones_materialize_entry(entry);
            if (IS_ICONE(v)) {
                vm_push(v);
                button_rasterize_icone_to_widget(w, AS_ICONE(v), override_size);
                vm_pop();
                return;
            }
        }
        /* Nom catalogue inconnu : signale (jamais silencieux) puis pas
         * d'icone (evite QIcon("icones.foo") traite comme chemin fichier
         * introuvable). Miroir defensif du Menu (menu_item_set_icon_value). */
        const char* wn = owl_ui_control_get_object_name(w);
        fprintf(stderr,
                "[ui] Bouton '%s' : reference une icone de catalogue "
                "inconnue '%s' (aucune icone affichee).\n",
                (wn && *wn) ? wn : "?", nom);
        owl_ui_button_set_icon(w, "");
        return;
    }
    /* Chemin de fichier disque : signale l'absence (jamais silencieux) mais
     * delegue quand meme a Qt, arbitre final des formats/ressources. */
    struct stat st;
    if (stat(str, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG) {
        const char* wn = owl_ui_control_get_object_name(w);
        fprintf(stderr,
                "[ui] Bouton '%s' : fichier image introuvable '%s' "
                "(aucune icone affichee).\n",
                (wn && *wn) ? wn : "?", str);
    }
    owl_ui_button_set_icon(w, str);
}

/* Pose l'icone du bouton depuis une valeur polymorphe (Image|Icone|chaine),
 * en respectant l'override ..ImageTaille du widget. Chemin partage par la prop
 * plate BTN..Image et la feuille Style..Image..Image. */
static void button_set_icon_value(OwlControl* w, Value v) {
    if (stdlib_image_is_instance(v)) {
        int iw = 0, ih = 0;
        const unsigned char* rgba = stdlib_image_get_rgba(v, &iw, &ih);
        owl_ui_button_set_icon_rgba(w, rgba, iw, ih);
    } else if (IS_ICONE(v)) {
        button_rasterize_icone_to_widget(
            w, AS_ICONE(v), owl_ui_control_get_icon_size_override(w));
    } else {
        ObjString* s = value_to_string(v);
        button_set_icon_from_string(
            w, s ? s->chars : "", owl_ui_control_get_icon_size_override(w));
    }
}

/*  — rasterise un ObjIcone vers RGBA et le pousse dans un SEUL mode
 * QIcon du bouton (survol -> Active, grisé -> Disabled), par-dessus l'icone de
 * base deja posee. Miroir de button_rasterize_icone_to_widget cible mode. */
static void button_rasterize_icone_to_state(OwlControl* w, ObjIcone* icn,
                                            int override_size, int mode) {
    if (icn == NULL || !icn->valide || icn->svg_raw == NULL
        || icn->svg_raw->length == 0) {
        return;
    }
    unsigned char* rgba = NULL;
    int iw = 0, ih = 0;
    bool want_bg = (icn->couleur_fond != -1);
    int icoSize = icone_target_size(owl_ui_control_get_height(w) - 16,
                                    override_size);
    if (owl_icone_rasterize_rgba(
            icn->svg_raw->chars, icn->svg_raw->length,
            (icn->couleur == -1) ? 0u : (unsigned int)(icn->couleur & 0xFFFFFF),
            icn->couleur_modifiable,
            icoSize, icn->rotation,
            icn->forme,
            (unsigned int)((want_bg ? icn->couleur_fond : 0) & 0xFFFFFF),
            want_bg,
            &rgba, &iw, &ih)) {
        owl_ui_button_set_icon_rgba_state(w, rgba, iw, ih, mode);
        free(rgba);
    }
}

/*  — pose l'icone d'un etat (survol/grisé) sur le bouton depuis une
 * valeur polymorphe (Image|Icone|chaine catalogue icones.X). Ecrit dans le
 * mode QIcon cible sans clobber l'icone de base. Une chaine hors-catalogue
 * (chemin fichier) n'est pas substituee par etat (Qt gererait le mode lui-meme
 * differemment) — hors-scope Option A. */
static void button_set_icon_state_value(OwlControl* w, Value v, int mode) {
    if (stdlib_image_is_instance(v)) {
        int iw = 0, ih = 0;
        const unsigned char* rgba = stdlib_image_get_rgba(v, &iw, &ih);
        owl_ui_button_set_icon_rgba_state(w, rgba, iw, ih, mode);
    } else if (IS_ICONE(v)) {
        button_rasterize_icone_to_state(
            w, AS_ICONE(v), owl_ui_control_get_icon_size_override(w), mode);
    } else {
        ObjString* s = value_to_string(v);
        const char* str = s ? s->chars : "";
        const char* nom = NULL;
        if (strncasecmp(str, "icones.", 7) == 0)      nom = str + 7;
        else if (strncasecmp(str, "icons.", 6) == 0)  nom = str + 6;
        if (nom != NULL) {
            const OwlIconCatalogEntry* entry = owl_icones_catalog_find(nom);
            if (entry != NULL) {
                Value iv = owl_icones_materialize_entry(entry);
                if (IS_ICONE(iv)) {
                    vm_push(iv);
                    button_rasterize_icone_to_state(
                        w, AS_ICONE(iv),
                        owl_ui_control_get_icon_size_override(w), mode);
                    vm_pop();
                }
            }
        }
    }
}

static bool prop_bool(const OwlPropertyInit* props, int n,
                       const char* name, const char* nameEN, bool dflt) {
    const OwlPropertyInit* p = find_prop(props, n, name);
    if (!p && nameEN) p = find_prop(props, n, nameEN);
    if (!p || !p->is_constant) return dflt;
    if (IS_BOOL(p->const_value)) return AS_BOOL(p->const_value);
    return dflt;
}

/* Lecture brute d'une prop (Value type union). Retourne NIL_VAL si la
 * prop n'existe pas ou n'est pas constante. Utilise pour les props
 * VARIANT (ex: Cadre — accepte enum int OU instance composee). */
static Value prop_value(const OwlPropertyInit* props, int n,
                         const char* name, const char* nameEN) {
    const OwlPropertyInit* p = find_prop(props, n, name);
    if (!p && nameEN) p = find_prop(props, n, nameEN);
    if (!p || !p->is_constant) return NIL_VAL;
    return p->const_value;
}

/* Lit un sous-champ entier d'une ObjInstance (utilise pour le dispatch
 * des types composes : Cadre, Police, ...). field_name doit etre stable
 * et sans accent (les sous-champs OWL sont declares en ASCII canon). */
static int instance_get_int(ObjInstance* inst, const char* field_name, int dflt) {
    if (!inst) return dflt;
    ObjString* k = obj_copy_string(field_name, (int)strlen(field_name));
    Value v;
    if (!table_get(&inst->fields, k, &v)) return dflt;
    if (IS_INTEGER(v)) return (int)AS_INTEGER(v);
    if (IS_REAL(v))    return (int)AS_REAL(v);
    return dflt;
}

/*  — point d'ingestion couleur unique. Sur une Value couleur, extrait
 * (bgr, alpha) : entier nu -> (v, 255, opaque) ; instance Couleur -> (.Couleur
 * BGR 24 bits, .Opacite 0-255). Toute autre Value -> bgr=-1 (pas de couleur),
 * alpha=255. Non-cassant : l'entier nu conserve exactement son comportement
 * opaque, seule l'instance Couleur peut porter un alpha < 255. */
static void owl_value_to_color(Value v, int* bgr, int* alpha) {
    *bgr = -1;
    *alpha = 255;
    if (IS_INTEGER(v)) { *bgr = (int)AS_INTEGER(v); return; }
    if (IS_REAL(v))    { *bgr = (int)AS_REAL(v);    return; }
    if (IS_INSTANCE(v)) {
        ObjInstance* in = AS_INSTANCE(v);
        *bgr = instance_get_int(in, "Couleur", -1);  /* color-int-ok: extracteur BGR packé, l'alpha (.Opacité) est lu juste après */
        int a = instance_get_int(in, "Opacite", 255);
        if (a < 0) a = 0; else if (a > 255) a = 255;
        *alpha = a;
    }
}

/*  — lit un sous-champ couleur (entier nu OU instance Couleur) d'une
 * ObjInstance et en extrait (bgr, alpha). Pendant de instance_get_int pour les
 * props CouleurFond de la cascade de Style : instance_get_int rendait -1 sur une
 * instance Couleur (le maillon cassé du glassmorphisme). bgr=-1 = champ absent
 * ou non-couleur.
 *
 * LECTEUR CANON couleur : toute lecture d'un sous-champ couleur DOIT passer par
 * ici (ou owl_value_to_color). Lire un champ Couleur/CouleurFond via l'entier nu
 * (instance_get_int) est alpha-aveugle et casse le build (tools/lint_color_reads.sh,
 * lancé par make). Exceptions légitimes (extracteur bas niveau, check booléen) :
 * marquer la ligne avec le commentaire color-int-ok suivi de la raison. */
static void instance_get_color(ObjInstance* inst, const char* field_name,
                               int* bgr, int* alpha) {
    *bgr = -1;
    *alpha = 255;
    if (!inst) return;
    ObjString* k = obj_copy_string(field_name, (int)strlen(field_name));
    Value v;
    if (!table_get(&inst->fields, k, &v)) return;
    owl_value_to_color(v, bgr, alpha);
}

static bool instance_get_bool(ObjInstance* inst, const char* field_name, bool dflt) {
    if (!inst) return dflt;
    ObjString* k = obj_copy_string(field_name, (int)strlen(field_name));
    Value v;
    if (!table_get(&inst->fields, k, &v)) return dflt;
    if (IS_BOOL(v)) return AS_BOOL(v);
    return dflt;
}


/* Recupere une ObjInstance imbriquee (ex: Style.Police, Style.Cadre).
 * Tente d'abord field_name en ASCII canon ; si fallback_accent != NULL
 * essaie aussi cette forme. NULL si absent ou pas une ObjInstance. */
static ObjInstance* instance_get_instance(ObjInstance* inst, const char* field_name) {
    if (!inst) return NULL;
    ObjString* k = obj_copy_string(field_name, (int)strlen(field_name));
    Value v;
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (IS_INSTANCE(v)) return AS_INSTANCE(v);
    return NULL;
}

/* Lit la valeur brute d'un champ d'instance (VARIANT). Retourne true + *out si
 * present. Utilise pour la feuille Style..Image..Image (Image|chaine|Icone). */
static bool instance_get_value(ObjInstance* inst, const char* field_name, Value* out) {
    if (!inst) return false;
    ObjString* k = obj_copy_string(field_name, (int)strlen(field_name));
    return table_get(&inst->fields, k, out);
}

/*  — applique la surcharge d'icone d'un etat (slot _Survol/_Grise) si
 * une icone y est renseignee (chaine non-vide / Image / Icone). */
static void apply_icon_state(OwlControl* w, ObjInstance* etats,
                             const char* slot, int mode) {
    ObjInstance* st = instance_get_instance(etats, slot);
    if (!st) return;
    Value iconVal;
    if (instance_get_value(st, "Image", &iconVal)
        && !(IS_STRING(iconVal) && AS_STRING(iconVal)->length == 0)) {
        button_set_icon_state_value(w, iconVal, mode);
    }
}

/* Forward decl — apply_style_to_control est defini plus bas dans le fichier
 * mais utilise plus haut (wiring des Style explicites a l'ouverture). */
static void apply_style_to_control(OwlControl* w, ObjInstance* style,
                                     OwlControlKind kind);
/*  — enregistrement widget->styleSaisie racine (defini plus bas,
 * utilise des la construction ). */
static void style_root_record(OwlControl* widget, ObjInstance* root);

/* Dernier message compose par une boite de message, memorise pour la sonde
 * de test __DebugDernierMessage. Copie malloc : aucun objet GC retenu. */
static char* g_dernier_message_boite = NULL;

static void message_boite_memorise(const char* msg) {
    free(g_dernier_message_boite);
    g_dernier_message_boite = NULL;
    if (msg == NULL) return;
    size_t n = strlen(msg);
    g_dernier_message_boite = (char*)malloc(n + 1);
    if (g_dernier_message_boite != NULL) memcpy(g_dernier_message_boite, msg, n + 1);
}

/* Compose args[start..argCount-1] en un message, un argument par ligne.
 *
 * Chaque argument est rendu dans la representation d'AFFICHAGE (celle de
 * Trace, cf. value_write_display) : un entier montre sa valeur, Null se
 * montre "Null", un tableau montre son contenu. Un argument ne peut donc
 * plus disparaitre en silence quel que soit son type.
 *
 * Les arguments sont des slots de la pile VM, donc racines GC pendant tout
 * l'appel natif ; la composition ecrit dans un tampon malloc et ne retient
 * aucun pointeur GC entre deux allocations.
 *
 * Le message est memorise AVANT tout court-circuit d'affichage : il reste
 * observable en mode test. Retour malloc (caller free), NULL si OOM. */
static char* concat_messages(int argCount, Value* args, int start) {
    OwlDisplaySink sink;
    owl_display_sink_init_buffer(&sink);
    for (int i = start; i < argCount; i++) {
        if (i > start) owl_display_putc(&sink, '\n');
        value_write_display(&sink, args[i]);
    }
    if (sink.data == NULL) {
        sink.data = (char*)malloc(1);
        if (sink.data != NULL) sink.data[0] = '\0';
    }
    message_boite_memorise(sink.data);
    return sink.data;
}

static bool first_arg_is_default_button(int argCount, Value* args, int max_button) {
    if (argCount < 2) return false;
    if (!IS_INTEGER(args[0])) return false;
    int64_t v = AS_INTEGER(args[0]);
    return v >= 0 && v <= max_button;
}

/* ====================================================================
 *  Construction des widgets depuis OwlWindowDecl (E.1)
 * ==================================================================== */

/* Forward decls (Lot Cadre Phase 4) — utilises dans build_control /
 * real_ui_get_prop mais definis plus bas. */
static void apply_cadre_instance_to_widget(OwlControl* w, ObjInstance* cadre);
static void apply_police_instance_to_widget(OwlControl* w, ObjInstance* police);
/*  lot 2 — relie un composite materialise (Cadre/Police) du store
 * d'une instance montee a son OwlControl* pour le watcher direct. */
static void inst_widget_watch_tag_recursive(ObjInstance* inst, OwlControl* widget);
/*  — (re)formate/de-formate l'affichage d'un Champ Saisie num selon
 * son FormatNombre effectif (defini plus bas ; utilise ici pour ContenuInitial). */
static void input_num_reformat_display(OwlControl* w, const char* name, bool raw);

/*  lot 3 — retrouve l'ObjUiControlInstance de la fenetre courante
 * correspondant a un OwlControlDecl (ou a un nom). Le store de cette instance
 * est la source de verite unique des props (composees + scalaires), partagee
 * GUI/headless. Renvoie NULL si pas de fenetre courante ou pas trouve. */
static ObjUiControlInstance* current_window_uici_for_decl(const OwlControlDecl* c) {
    ObjWindowInstance* inst = vm_window_stack_top();
    if (!inst || !c) return NULL;
    for (int i = 0; i < inst->control_count; i++) {
        ObjUiControlInstance* uici = inst->controls[i];
        if (uici && uici->decl == c) return uici;
    }
    return NULL;
}

static ObjUiControlInstance* current_window_uici_by_name(const char* name) {
    ObjWindowInstance* inst = vm_window_stack_top();
    if (!inst || !name) return NULL;
    for (int i = 0; i < inst->control_count; i++) {
        ObjUiControlInstance* uici = inst->controls[i];
        if (uici && uici->decl && uici->decl->name &&
            strcmp(uici->decl->name, name) == 0)
            return uici;
    }
    return NULL;
}

/*  — nom du champ de la fenetre courante dont le widget est `w`.
 * Le QLineEdit reste le controle logique (uici->widget), c'est aussi la cle
 * du focus event : ce pont permet a l'eventFilter ui_qt de remonter au nom
 * (donc au FormatNombre effectif) depuis le seul widget focuse. NULL si
 * introuvable / hors fenetre. */
static const char* current_window_ctrl_name_by_widget(void* w) {
    ObjWindowInstance* inst = vm_window_stack_top();
    if (!inst || !w) return NULL;
    for (int i = 0; i < inst->control_count; i++) {
        ObjUiControlInstance* uici = inst->controls[i];
        if (uici && uici->widget == w && uici->decl)
            return uici->decl->name;
    }
    return NULL;
}

/*  — instance de controle de la fenetre courante dont le widget est
 * `w`. Utilise par les ponts "filtre a la souris" pour remonter du QTableView
 * (seul connu du header custom Qt) a son uici (porteur du store $tblfilters).
 * NULL si introuvable / hors fenetre. */
static ObjUiControlInstance* current_window_uici_by_widget(void* w) {
    ObjWindowInstance* inst = vm_window_stack_top();
    if (!inst || !w) return NULL;
    for (int i = 0; i < inst->control_count; i++) {
        ObjUiControlInstance* uici = inst->controls[i];
        if (uici && uici->widget == w) return uici;
    }
    return NULL;
}

/*  — helper consolide : resout un argument receveur d'une famille
 * de natives UI (Liste/Combo/Selecteur/Interrupteur/ZoneRepetee) vers son
 * OwlControl* concret.
 *
 * Accepte :
 *   - ObjUiControl direct (legacy)
 *   - ObjUiControlRef (proxy pose au compile, cas dominant FEN_X.CTRL_Y)
 *   - ObjUiControlInstance (instance materialisee, cas exotique)
 *
 * Verifie ensuite le kind contre la whitelist (k1/k2 ; k2=-1 = un seul kind
 * autorise). Emet un message stderr standardise si mismatch. Retourne le
 * widget concret (peut etre NULL en headless — caller doit garder ce
 * fast-path, c'est la convention table_arg_or_error).
 *
 * NOTE : le mode "chaine" (lookup par nom) reste localise aux helpers qui
 * en avaient besoin avant (table, zr). Pas inclus ici pour ne pas changer
 * la surface d'erreur de combo/list/selecteur/interrupteur (cf. tests
 * existants qui dependent du texte exact). */
static OwlControl* resolve_widget_kinded(Value v,
                                         OwlControlKind k1,
                                         int k2,  /* -1 = inutilise */
                                         const char* fnName,
                                         const char* expectedLabel) {
    OwlControl* widget = NULL;
    OwlControlKind actualKind = (OwlControlKind)-1;
    bool resolved = false;

    if (IS_UI_CONTROL(v)) {
        ObjUiControl* uic = AS_UI_CONTROL(v);
        widget = (OwlControl*)uic->widget;
        actualKind = (OwlControlKind)uic->kind;
        resolved = true;
    } else if (IS_UI_CONTROL_REF(v)) {
        ObjUiControlRef* ref = AS_UI_CONTROL_REF(v);
        ObjUiControlInstance* uici = resolve_ui_ref(ref);
        if (!uici) {
            /* resolve_ui_ref a deja emis l'erreur appropriee. */
            return NULL;
        }
        widget = (OwlControl*)uici->widget;
        actualKind = uici->decl ? (OwlControlKind)uici->decl->kind
                                : (OwlControlKind)ref->kind;
        resolved = true;
    } else if (IS_UI_CONTROL_INSTANCE(v)) {
        ObjUiControlInstance* uici = AS_UI_CONTROL_INSTANCE(v);
        widget = (OwlControl*)uici->widget;
        if (uici->decl) actualKind = (OwlControlKind)uici->decl->kind;
        resolved = uici->decl != NULL;
    }

    if (!resolved) {
        /*  — pose ErreurDetectee via owl_emit_native_error
         * (avant : fprintf stderr seul -> ErreurDetectee restait Faux,
         * pas de soft-error canon observable cote user). */
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                              "arg", "1",
                              "expected", expectedLabel,
                              "actual", "valeur incompatible",
                              NULL);
        return NULL;
    }
    if (actualKind != k1 && (k2 < 0 || actualKind != (OwlControlKind)k2)) {
        /*  — idem : kind mismatch doit poser ErreurDetectee. */
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                              "arg", "1",
                              "expected", expectedLabel,
                              "actual", "type de champ incompatible",
                              NULL);
        return NULL;
    }
    return widget;
}

/*  lot 3 — recupere (ou materialise lazy) l'instance composee
 * `Police`/`Cadre` dans le STORE de l'uici, la relie a son widget Qt monte,
 * et la retourne. Remplace les anciens stores globaux __police_/__cadre_ :
 * une seule source de verite par instance. */
static ObjInstance* uici_composite_instance(ObjUiControlInstance* uici,
                                            const char* canonKey,
                                            const char* templateName) {
    if (!uici) return NULL;
    Value existing;
    if (obj_ui_control_instance_headless_get(uici, canonKey, &existing)
        && IS_INSTANCE(existing)) {
        if (uici->widget != NULL)
            inst_widget_watch_tag_recursive(AS_INSTANCE(existing),
                                            (OwlControl*)uici->widget);
        return AS_INSTANCE(existing);
    }
    Value inst = owl_clone_global_template(templateName);
    if (!IS_INSTANCE(inst)) return NULL;
    vm_push(inst);  /* root pendant le set (peut grossir la table -> GC) */
    obj_ui_control_instance_headless_set(uici, canonKey, inst);
    if (uici->widget != NULL)
        inst_widget_watch_tag_recursive(AS_INSTANCE(inst),
                                        (OwlControl*)uici->widget);
    vm_pop();
    return AS_INSTANCE(inst);
}

/*  — convergence des props plates d'apparence des widgets mono-element
 * (Libellé, Bouton) vers l'instance Style du store (verite unique, calque du
 * pattern Saisie ). Contrairement au Saisie (une seule brique
 * ZoneSaisie), un widget mono route CHAQUE prop vers une sous-brique differente :
 *   CouleurFond (plat)   <-> Style..CadreExtérieur..CouleurFond (le fond)
 *   PoliceCouleur (plat) <-> Style..Libellé..Couleur           (le texte)
 * Renseigne `*outBrick` (nom du sous-element interne, ASCII) + `*outSub` (nom du
 * sous-champ scalaire) et retourne true si `prop` est un raccourci mono connu
 * pour ce kind. Le template Style depend du kind (styleLibellé / styleBouton),
 * cf. mono_style_template_name. Retourne false hors LABEL/BUTTON. */
static bool mono_style_shortcut_bricksub(OwlControlKind kind, const char* prop,
                                         const char** outBrick,
                                         const char** outSub) {
    if (kind != OWL_CONTROL_LABEL && kind != OWL_CONTROL_BUTTON) return false;
    if (!prop) return false;
    if (strcasecmp(prop, "CouleurFond") == 0 ||
        strcasecmp(prop, "BackgroundColor") == 0) {
        *outBrick = "CadreExterieur";
        *outSub   = "CouleurFond";
        return true;
    }
    if (strcasecmp(prop, "PoliceCouleur") == 0 ||
        strcasecmp(prop, "FontColor") == 0) {
        *outBrick = "Libelle";
        *outSub   = "Couleur";
        return true;
    }
    /*  — Bouton uniquement : BTN..Image (plat, ) unifie avec
     * Style..Image..Image (feuille styleImageBouton). Le Libelle n'a pas cette
     * feuille (son ..Image plat suit un autre chemin). */
    if (kind == OWL_CONTROL_BUTTON && strcasecmp(prop, "Image") == 0) {
        *outBrick = "Image";
        *outSub   = "Image";
        return true;
    }
    return false;
}

/*  — nom du template Style a cloner pour un widget mono-element. */
static const char* mono_style_template_name(OwlControlKind kind) {
    if (kind == OWL_CONTROL_LABEL)  return "styleLibell\xc3\xa9";  /* styleLibellé */
    if (kind == OWL_CONTROL_BUTTON) return "styleBouton";
    return NULL;
}

/* Mappe une valeur d'enum PoliceEtirement (sentinel OWL_ETIR_*) vers la
 * valeur de stretch attendue par le moteur de rendu (100 = nominal). Toute
 * valeur inattendue retombe sur le nominal. */
static int owl_etirement_to_stretch(int etirement) {
    switch (etirement) {
        case OWL_ETIR_CONDENSE: return 75;
        case OWL_ETIR_ETENDU:   return 125;
        case OWL_ETIR_LARGE:    return 150;
        case OWL_ETIR_NORMAL:
        default:                return 100;
    }
}

/* Reciproque : valeur de stretch du moteur de rendu -> enum PoliceEtirement.
 * Tout stretch non reconnu (0 = unstretched, autre custom) retombe sur
 * etirNormal pour garantir un round-trip stable. */
static int owl_stretch_to_etirement(int stretch) {
    switch (stretch) {
        case 75:  return OWL_ETIR_CONDENSE;
        case 125: return OWL_ETIR_ETENDU;
        case 150: return OWL_ETIR_LARGE;
        default:  return OWL_ETIR_NORMAL;
    }
}

/* Forward decls ZR — utilises dans open_window mais definis avec les
 * natives plus bas. */
static bool zr_decl_is_zr_name(const OwlWindowDecl* decl, const char* name);
static void zr_register_at_open(OwlControl* zr_widget,
                                 const OwlWindowDecl* decl,
                                 const char* zr_name);

/*  — lecteurs de style Table/Colonne (definis plus bas, apres les
 * helpers d'instance). Utilises dans le post-pass Style de build_control. */
static void read_table_style(ObjInstance* style, OwlTableStyleSpec* spec,
                             OwlCadreSpec* extBorder, const char* fams[4]);
static void read_column_style(ObjInstance* style, OwlColumnStyleSpec* spec,
                              const char* fams[4]);

/* Cree un widget enfant selon le kind du controle, applique les props
 * Tier 1A. Retourne le control Qt (cast OwlControl*). */
/*  — pipeline image du Champ CodeBarres, partage avec la native
 * CodeBarreVersImage (defini dans stdlib_codebarres.c). Rasterise un code-barres
 * en Image (ObjInstance GC-geree) ; silencieux (NIL_VAL si echec). */
extern Value owl_codebarre_image_value_from_params(int64_t cbType, int64_t ecc,
                                                   int64_t options,
                                                   const char* contenu,
                                                   int contenuLen,
                                                   int largeurMax, int hauteurMax);
extern Value owl_codebarre_image_value_from_instance(ObjInstance* cb);
extern void owl_codebarre_params_from_instance(ObjInstance* cb, int64_t* typeOut,
                                               int64_t* eccOut, int64_t* optionsOut,
                                               const char** contenuOut,
                                               int* contenuLenOut);
/*  — pose la config code-barres (type/ecc/options/contenu) sur le
 * widget OwlImageLabel pour la re-rasterisation nette au resize. Defini dans
 * ui_qt.cpp (meme backend que owl_ui_label_set_image_rgba). */
extern void owl_ui_codebarre_set_config(OwlControl* w, int64_t type, int64_t ecc,
                                        int64_t options, const char* contenu, int len);
/*  — stocke (ou efface) le logo (image centrale QR) sur le widget pour
 * la recomposition au resize. Defini dans ui_qt.cpp. */
extern void owl_ui_codebarre_set_logo(OwlControl* w, const unsigned char* rgba,
                                      int lw, int lh, double proportion);

/* Cablage des events 0-argument d'un champ : (nom canon FR, nom canon EN,
 * hook backend). Chaque `case` de build_control recopiait la meme boucle
 * find_event -> register_binding_named -> hook, et les oublis de cablage
 * (event declare au catalogue mais jamais tire) venaient exactement de cette
 * duplication. Une table par famille d'events + un seul marcheur.
 * . */
typedef struct {
    const char* sur;
    const char* en;
    void (*hook)(OwlControl*, OwlCallback, void*);
} OwlEventBind0;

static void bind_events_0(OwlControl* widget, const OwlControlDecl* ctrl,
                          const OwlEventBind0* evs, size_t count) {
    if (!widget) return;
    for (size_t i = 0; i < count; i++) {
        const OwlEventBlock* e = find_event(ctrl->events, ctrl->event_count,
                                             evs[i].sur, evs[i].en);
        if (!e) continue;
        int idx = register_binding_named(e->impl, false, ctrl->name);
        if (idx >= 0) evs[i].hook(widget, dispatch_event, (void*)(intptr_t)idx);
    }
}
#define BIND_EVENTS_0(widget, ctrl, tbl) \
    bind_events_0((widget), (ctrl), (tbl), sizeof(tbl) / sizeof((tbl)[0]))

/* Familles generiques du catalogue (event_catalog_widgets.c) : tout champ qui
 * declare FAM_MOUSE_BASIC / FAM_FOCUS et n'a pas de signal Qt dedie passe par
 * l'event filter commun -> le cablage suit automatiquement le catalogue. */
static const OwlEventBind0 EVS_MOUSE_BASIC[] = {
    {"surClic",       "onClick",       owl_ui_control_on_clic},
    {"surDoubleClic", "onDoubleClick", owl_ui_control_on_double_clic},
};
/* Champs dont surClic est deja porte par un signal Qt dedie (Bouton ->
 * `clicked`) : seul le double-clic passe par l'event filter commun. Cabler
 * aussi surClic ici le ferait tirer deux fois par clic. */
static const OwlEventBind0 EVS_MOUSE_DBLCLIC_ONLY[] = {
    {"surDoubleClic", "onDoubleClick", owl_ui_control_on_double_clic},
};
static const OwlEventBind0 EVS_FOCUS[] = {
    {"surEntr\xc3\xa9""e", "onEntry", owl_ui_control_on_entry},
    {"surSortie",          "onExit",  owl_ui_control_on_exit},
};

/* Cablage clavier d'un champ : surToucheAppuyee / surToucheRelachee, payload =
 * code touche. Commun a tous les champs qui declarent FAM_KEYBOARD_EVENTS
 * (event_catalog_widgets.c) — un seul point d'appel par `case`, au lieu du
 * bloc recopie qui a produit les oublis de cablage des lots precedents.
 *
 * Le filtre est installe MEME sans handler declare : c'est ce qui permet a la
 * touche de remonter au handler fenetre (bubble-up ) au lieu d'etre
 * avalee par le widget Qt. Sans handler fenetre non plus, le filtre est
 * neutre. Avec un handler, install_key_filter rend au besoin le champ
 * focusable (cas des champs NoFocus par defaut, Image / CodeBarres). */
static void bind_events_key(OwlControl* widget, const OwlControlDecl* ctrl) {
    if (!widget) return;
    const OwlEventBlock* kdown = find_event(ctrl->events, ctrl->event_count,
            "surToucheAppuy\xc3\xa9""e", "onKeyDown");
    const OwlEventBlock* kup = find_event(ctrl->events, ctrl->event_count,
            "surToucheRel\xc3\xa2""ch\xc3\xa9""e", "onKeyUp");
    int idown = kdown ? register_binding_named(kdown->impl, false, ctrl->name) : -1;
    int iup   = kup   ? register_binding_named(kup->impl,   false, ctrl->name) : -1;
    owl_ui_install_key_filter(widget,
        (idown >= 0) ? dispatch_image_event_1i : NULL, (void*)(intptr_t)idown,
        (iup   >= 0) ? dispatch_image_event_1i : NULL, (void*)(intptr_t)iup);
}

/* Cablage des events souris/molette/menu/clavier d'un champ backend
 * OwlImageLabel. Partage par le Champ Image et le Champ CodeBarres (meme
 * widget, meme set d'events, cf event_catalog_widgets.c). */
static void bind_image_field_events(OwlControl* widget, const OwlControlDecl* ctrl) {
    if (!widget) return;
    /*  — param macro renomme `evname` (pas `name`) pour ne
     * pas masquer le token `ctrl->name` passe a register_binding_named
     * (le preprocesseur substitue textuellement chaque `name`). */
    #define IMG_BIND_0(evname, on_fn)                            \
        do {                                                     \
            const OwlEventBlock* e = find_event(ctrl->events,    \
                ctrl->event_count, evname, NULL);                \
            if (e) {                                             \
                int bidx = register_binding_named(e->impl,       \
                    false, ctrl->name);                          \
                if (bidx >= 0) on_fn(widget,                    \
                    dispatch_image_event_0,                      \
                    (void*)(intptr_t)bidx);                      \
            }                                                    \
        } while (0)
    #define IMG_BIND_2(evname, on_fn)                            \
        do {                                                     \
            const OwlEventBlock* e = find_event(ctrl->events,    \
                ctrl->event_count, evname, NULL);                \
            if (e) {                                             \
                int bidx = register_binding_named(e->impl,       \
                    false, ctrl->name);                          \
                if (bidx >= 0) on_fn(widget,                    \
                    dispatch_image_event_2i,                     \
                    (void*)(intptr_t)bidx);                      \
            }                                                    \
        } while (0)
    #define IMG_BIND_3(evname, on_fn)                            \
        do {                                                     \
            const OwlEventBlock* e = find_event(ctrl->events,    \
                ctrl->event_count, evname, NULL);                \
            if (e) {                                             \
                int bidx = register_binding_named(e->impl,       \
                    false, ctrl->name);                          \
                if (bidx >= 0) on_fn(widget,                    \
                    dispatch_image_event_3i,                     \
                    (void*)(intptr_t)bidx);                      \
            }                                                    \
        } while (0)

    IMG_BIND_0("surClic",                  owl_ui_image_field_on_clic);
    IMG_BIND_0("surDoubleClic",            owl_ui_image_field_on_double_clic);
    IMG_BIND_0("surEntr\xc3\xa9""eSurvol", owl_ui_image_field_on_entree_survol);
    IMG_BIND_0("surSortieSurvol",          owl_ui_image_field_on_sortie_survol);
    IMG_BIND_2("surSurvol",                owl_ui_image_field_on_survol);
    IMG_BIND_2("surBoutonGauche",          owl_ui_image_field_on_bouton_gauche);
    IMG_BIND_2("surRelacheBoutonGauche",   owl_ui_image_field_on_relache_btn_gauche);
    IMG_BIND_2("surBoutonDroit",           owl_ui_image_field_on_bouton_droit);
    IMG_BIND_2("surRelacheBoutonDroit",    owl_ui_image_field_on_relache_btn_droit);
    IMG_BIND_2("surDoubleClicDroit",       owl_ui_image_field_on_double_clic_droit);
    IMG_BIND_3("surMolette",               owl_ui_image_field_on_molette);
    IMG_BIND_2("surMenuContextuel",        owl_ui_image_field_on_menu_contextuel);
    IMG_BIND_2("surD\xc3\xa9""placement",  owl_ui_image_field_on_deplacement);
    IMG_BIND_2("surChangementTaille",      owl_ui_image_field_on_changement_taille);

    #undef IMG_BIND_0
    #undef IMG_BIND_2
    #undef IMG_BIND_3

}

/*  — plafond de rasterisation supersample pour un Champ CodeBarres.
 * Un code 1D large (Code128 ~150 modules) dans un champ de 200px donne ~1.3
 * px/module : le texte HRT est ecrase des la generation. On genere a SS fois
 * la taille du champ (borne a 2048px/cote pour la memoire), puis le QLabel
 * sous-echantillonne en SmoothTransformation -> HRT net a toute taille. */
#define OWL_CB_FIELD_SUPERSAMPLE   4
#define OWL_CB_FIELD_SS_MAX_PX     2048
static int cb_field_supersample(int px) {
    if (px <= 0) return px;
    long long ss = (long long)px * OWL_CB_FIELD_SUPERSAMPLE;
    if (ss > OWL_CB_FIELD_SS_MAX_PX) ss = OWL_CB_FIELD_SS_MAX_PX;
    return (int)ss;
}

/*  — dérivation du masque de frappe guidée (défini plus bas) et défaut
 * de format date projet (extern, défini dans compiler.c) utilisés dès la
 * construction du champ Date/Heure. */
static bool input_frappe_mask_from_format(const char* fmt, char* out, size_t outsz);
static bool input_duree_frappe_mask_from_format(const char* fmt, char* out, size_t outsz);
extern const char* compiler_projet_format_date(void);
extern const char* compiler_projet_format_heure(void);  /*  */
extern const char* compiler_projet_format_duree(void);  /*  */

/*  — pose le validateur num config-aware si le champ a un FormatNombre
 * effectif actif (defini plus bas, apres FnbConfig). */
static void input_num_apply_format_validator(OwlControl* w, const char* name);

static OwlControl* build_control(OwlWindow* parent, const OwlControlDecl* ctrl,
                                  int auto_y) {
    /* X/Y explicites prennent precedence ; sinon fallback auto-layout
     * vertical (x=10, y empile). Permet au user de placer pixel-precis
     * avec Qt Designer tout en gardant une UX simple sans Designer. */
    int x = prop_int(ctrl->properties, ctrl->property_count, "X", "X", -1);
    int y = prop_int(ctrl->properties, ctrl->property_count, "Y", "Y", -1);
    if (x < 0) x = 10;
    if (y < 0) y = auto_y;
    int defaultW = (ctrl->kind == OWL_CONTROL_TABLE) ? 600
                 : (ctrl->kind == OWL_CONTROL_INPUT) ? 200
                 : 100;
    int defaultH = (ctrl->kind == OWL_CONTROL_TABLE)  ? 300
                 : (ctrl->kind == OWL_CONTROL_BUTTON) ? 30
                 : 25;
    int w = prop_int(ctrl->properties, ctrl->property_count, "Largeur", "Width", defaultW);
    int h = prop_int(ctrl->properties, ctrl->property_count, "Hauteur", "Height", defaultH);

    OwlControl* widget = NULL;
    switch (ctrl->kind) {
        case OWL_CONTROL_LABEL: {
            const char* text = prop_str(ctrl->properties, ctrl->property_count,
                                         "Libelle", "Caption", "");
            /* Note : alias ASCII "Libelle" suffit (scanner accent-strippe). */
            widget = owl_ui_label_create(parent, text, x, y, w, h);
            /* Lot E — FormatAffichage = formatTexteRiche : transpile Markdown
             * OWL etendu vers HTML et pose en mode Qt::RichText.
             * Active aussi le wordwrap auto : sans ca, le texte reste sur une
             * ligne unique, [j:justifie] ne peut pas justifier (rien a justifier
             * sur une ligne) et le texte deborde / est tronque. */
            int formatAff = prop_int(ctrl->properties, ctrl->property_count,
                                      "FormatAffichage", "DisplayFormat", OWL_FAFF_TEXTE);
            if (formatAff == OWL_FAFF_TEXTE_RICHE && widget && text && *text) {
                ObjString* html = owl_text_riche_to_html(text, (int)strlen(text));
                if (html) {
                    owl_ui_label_set_text_rich(widget, html->chars);
                    owl_ui_label_set_word_wrap(widget, 1);
                }
            }
            /* TypeLibelle = TLMultiLigne → wordwrap actif. */
            int typeLibelle = prop_int(ctrl->properties, ctrl->property_count,
                                        "TypeLibelle", "LabelType", OWL_TL_NORMAL);
            if (typeLibelle == OWL_TL_MULTILIGNE && widget) {
                owl_ui_label_set_word_wrap(widget, 1);
            }
            /* Couleur / CouleurFond : appliquees en post-pass commune a tous
             * les widgets (cf bloc apres le switch), comme le Cadre. */
            /* Cadrage — applique avant Image pour que set_image l'utilise.
             * Defaut = -1 (non specifie) ; valeurs sentinels OWL_CADIMG_*. */
            int cadrage = prop_int(ctrl->properties, ctrl->property_count,
                                    "Cadrage", "Framing", -1);
            if (cadrage != -1 && widget) owl_ui_label_set_cadrage(widget, cadrage);
            /* Image — accepte string (chemin) ou instance Image en memoire. */
            Value imgVal = prop_value(ctrl->properties, ctrl->property_count,
                                       "Image", "Image");
            if (widget && stdlib_image_is_instance(imgVal)) {
                int iw = 0, ih = 0;
                const unsigned char* rgba = stdlib_image_get_rgba(imgVal, &iw, &ih);
                int cad = (cadrage != -1) ? cadrage : OWL_CADIMG_CENTRE;
                owl_ui_label_set_image_rgba(widget, rgba, iw, ih, cad);
            } else if (widget && IS_STRING(imgVal)) {
                owl_ui_label_set_image(widget, AS_CSTRING(imgVal));
            }
            /* Lot A — Ellipse (specifique Libelle). Defaut -1 = non specifie. */
            int ellipse = prop_int(ctrl->properties, ctrl->property_count,
                                    "Ellipse", "Ellipsis", -1);
            if (ellipse != -1 && widget) owl_ui_control_set_ellipsis_mode(widget, ellipse);
            /* surClic / surDoubleClic : le QLabel n'a pas de signal `clicked`,
             * l'event filter commun les relaie. */
            BIND_EVENTS_0(widget, ctrl, EVS_MOUSE_BASIC);
            break;
        }
        case OWL_CONTROL_BUTTON: {
            const char* text = prop_str(ctrl->properties, ctrl->property_count,
                                         "Libelle", "Caption", "");
            widget = owl_ui_button_create(parent, text, x, y, w, h);
            /* Phase E.3 : cabler PROCEDURE surClic vers signal clicked. */
            const OwlEventBlock* clic = find_event(ctrl->events, ctrl->event_count,
                                                    "surClic", "onClick");
            if (clic && widget) {
                int idx = register_binding_named(clic->impl, false, ctrl->name);
                if (idx >= 0) {
                    owl_ui_button_on_click(widget, dispatch_event,
                                           (void*)(intptr_t)idx);
                }
            }
            BIND_EVENTS_0(widget, ctrl, EVS_MOUSE_DBLCLIC_ONLY);
            /* TypeBouton : encapsule la semantique canon (default/shortcut). */
            int typeBouton = prop_int(ctrl->properties, ctrl->property_count,
                                       "TypeBouton", "ButtonType", OWL_BTN_NORMAL);
            if (widget) owl_ui_button_set_type_bouton(widget, typeBouton);
            const char* shortcut = prop_str(ctrl->properties, ctrl->property_count,
                                             "ToucheRaccourci", "ShortcutKey", NULL);
            if (shortcut) owl_ui_button_set_shortcut(widget, shortcut);
            /* Image : QPushButton::setIcon — polymorphique
             *   ObjImage / chaine path / ObjIcone ( Cas K).
             * Pour ObjIcone : on rasterise via Qt6Svg vers une OwlImage,
             * puis on passe son buffer RGBA au backend Qt. Le OwlImage
             * intermediaire est libere immediatement (uniquement le
             * buffer Qt-side reste utilise apres setPixmap). */
            Value btnImgVal = prop_value(ctrl->properties, ctrl->property_count,
                                          "Image", "Image");
            if (widget && stdlib_image_is_instance(btnImgVal)) {
                int iw = 0, ih = 0;
                const unsigned char* rgba = stdlib_image_get_rgba(btnImgVal, &iw, &ih);
                owl_ui_button_set_icon_rgba(widget, rgba, iw, ih);
            } else if (widget && IS_ICONE(btnImgVal)) {
                /* Rasterise directement en RGBA via Qt6Svg : evite
                 * l'allocation d'un OwlImage intermediaire (qui ne serait
                 * pas trackee par le GC et leak jusqu'a la fin du run).
                 * Le buffer rgba est alloue par owl_icone_rasterize_rgba
                 * et libere immediatement apres le push Qt. */
                ObjIcone* icn = AS_ICONE(btnImgVal);
                if (icn->valide && icn->svg_raw != NULL
                    && icn->svg_raw->length > 0) {
                    unsigned char* rgba = NULL;
                    int iw = 0, ih = 0;
                    /*  : forme + couleur_fond. */
                    bool want_bg = (icn->couleur_fond != -1);
                    /*  — taille adaptative : auto = hauteur utile du
                     * bouton (moins padding vertical), override = ..ImageTaille. */
                    int icoOverride = prop_int(ctrl->properties, ctrl->property_count,
                                                "ImageTaille", "ImageSize", 0);
                    owl_ui_control_set_icon_size_override(widget, icoOverride);
                    int icoSize = icone_target_size(h - 16, icoOverride);
                    if (owl_icone_rasterize_rgba(
                            icn->svg_raw->chars, icn->svg_raw->length,
                            /*  : -1 unset -> 0 (noir) defaut au lieu de 0xFFFFFF blanc. */
                            (icn->couleur == -1) ? 0u : (unsigned int)(icn->couleur & 0xFFFFFF),
                            icn->couleur_modifiable,
                            icoSize, icn->rotation,
                            icn->forme,
                            (unsigned int)((want_bg ? icn->couleur_fond : 0) & 0xFFFFFF),
                            want_bg,
                            &rgba, &iw, &ih)) {
                        owl_ui_button_set_icon_rgba(widget, rgba, iw, ih);
                        free(rgba);
                    }
                }
            } else if (widget && IS_STRING(btnImgVal)) {
                /*  — resolveur partage : "icones.<nom>" -> catalogue,
                 * sinon chemin fichier / SVG inline (Qt). */
                int icoOverride = prop_int(ctrl->properties, ctrl->property_count,
                                            "ImageTaille", "ImageSize", 0);
                owl_ui_control_set_icon_size_override(widget, icoOverride);
                button_set_icon_from_string(widget, AS_CSTRING(btnImgVal),
                                            icoOverride);
            }
            break;
        }
        case OWL_CONTROL_INPUT: {
            const char* indication = prop_str(ctrl->properties, ctrl->property_count,
                                               "Indication", "Placeholder", "");
            int typeSaisie = prop_int(ctrl->properties, ctrl->property_count,
                                       "TypeSaisie", "InputType", OWL_TYPESAISIE_TEXTE);
            if (typeSaisie == OWL_TYPESAISIE_MOTDEPASSE) {
                widget = owl_ui_password_create(parent, indication, x, y, w, h);
            } else if (typeSaisie == OWL_TYPESAISIE_MULTILIGNE) {
                widget = owl_ui_multiline_create(parent, indication, x, y, w, h);
            } else {
                /* Texte, Numerique et Monetaire partagent le QLineEdit ; le type
                 * pilote ensuite le validateur (num/monetaire) et le GET typé. */
                widget = owl_ui_lineedit_create(parent, indication, x, y, w, h);
            }
            /* Memorise le type sur le widget : le GET runtime le relit tel quel
             * (round-trip fidele, pas de repli Texte) et pose le validateur
             * numerique/monetaire quand il y a lieu. */
            if (widget) owl_ui_control_set_input_type(widget, typeSaisie);
            /* BorneMin/BorneMax : bornes reelles du validateur d'un champ
             * numerique/monetaire (0 = borne non fixee en design). */
            if (widget && (typeSaisie == OWL_TYPESAISIE_NUM ||
                           typeSaisie == OWL_TYPESAISIE_MONETAIRE ||
                           typeSaisie == OWL_TYPESAISIE_NUM_PRECIS)) {
                int bmin = prop_int(ctrl->properties, ctrl->property_count,
                                    "BorneMin", "MinValue", 0);
                int bmax = prop_int(ctrl->properties, ctrl->property_count,
                                    "BorneMax", "MaxValue", 0);
                if (bmin != 0) owl_ui_lineedit_set_min_value(widget, bmin);
                if (bmax != 0) owl_ui_lineedit_set_max_value(widget, bmax);
            }
            /*  — champ num avec FormatNombre effectif (champ > projet) :
             * remplace le QDoubleValidator locale-C par un validateur config-aware
             * qui remappe toute touche decimale (. ou ,) vers le separateur decimal
             * configure et neutralise le separateur de milliers (qui ne se tape
             * pas). Le texte en edition ne contient alors que chiffres + decimal
             * configure -> fnb_deformat ne peut plus confondre decimal et milliers.
             * No-op sans FormatNombre actif : le champ plat garde son
             * QDoubleValidator (decimal '.'), non-regression stricte. */
            if (widget && (typeSaisie == OWL_TYPESAISIE_NUM ||
                           typeSaisie == OWL_TYPESAISIE_MONETAIRE))
                input_num_apply_format_validator(widget, ctrl->name);
            /* Valeur (runtime) prime ; sinon ContenuInitial (valeur de depart
             * definie en design) sert de texte initial. */
            const char* texte = prop_str(ctrl->properties, ctrl->property_count,
                                          "Valeur", "Value", NULL);
            if (!texte) {
                texte = prop_str(ctrl->properties, ctrl->property_count,
                                  "ContenuInitial", "InitialContent", NULL);
            }
            if (texte) {
                /* Lot E : si FormatAffichage = formatTexteRiche ET widget multiligne
                 * (QTextEdit), transpile le source en HTML et pose via setHtml. */
                int formatAff = prop_int(ctrl->properties, ctrl->property_count,
                                          "FormatAffichage", "DisplayFormat", OWL_FAFF_TEXTE);
                if (formatAff == OWL_FAFF_TEXTE_RICHE && typeSaisie == OWL_TYPESAISIE_MULTILIGNE) {
                    ObjString* html = owl_text_riche_to_html(texte, (int)strlen(texte));
                    if (html) {
                        owl_ui_lineedit_set_text_rich(widget, html->chars);
                    } else {
                        owl_ui_control_set_text(widget, texte);
                    }
                } else {
                    owl_ui_control_set_text(widget, texte);
                }
                /*  — ContenuInitial d'un champ num : formate a
                 * l'ouverture selon le FormatNombre effectif (champ > projet),
                 * comme le SET programmatique. Le store (litteral) et le projet
                 * sont deja resolus a ce stade. No-op si format plat. */
                if (widget && (typeSaisie == OWL_TYPESAISIE_NUM ||
                               typeSaisie == OWL_TYPESAISIE_MONETAIRE))
                    input_num_reformat_display(widget, ctrl->name, false);
            }
            /* Phase E.2 : props avancees Saisie. */
            int maxLong = prop_int(ctrl->properties, ctrl->property_count,
                                    "MaxLongueur", "MaxLength", 0);
            if (maxLong > 0) owl_ui_lineedit_set_max_length(widget, maxLong);
            /* MasqueSaisie (V1.5 : texte simple). */
            const char* masque = prop_str(ctrl->properties, ctrl->property_count,
                                           "MasqueSaisie", "InputMask", NULL);
            if (masque) {
                owl_ui_lineedit_set_mask(widget, masque);
            } else if (widget && typeSaisie == OWL_TYPESAISIE_DUREE) {
                /*  — champ Durée sans masque de saisie explicite : masque
                 * de FRAPPE GUIDÉE + validateur DURÉE dédiés (minutes = MI/mm, HH non
                 * plafonné à 23). Format effectif = MasqueAffichage posé > défaut
                 * HH:MI:SS. Format variable (J seul, +N) -> saisie libre. */
                const char* fmt = prop_str(ctrl->properties, ctrl->property_count,
                                           "MasqueAffichage", "DisplayMask", NULL);
                if (fmt == NULL || fmt[0] == '\0') {
                    /*  — repli FormatDurée projet (miroir Heure/Date). */
                    const char* pf = compiler_projet_format_duree();
                    fmt = (pf != NULL && pf[0] != '\0') ? pf : "HH:MI:SS";
                }
                char qtmaskd[128];
                if (input_duree_frappe_mask_from_format(fmt, qtmaskd, sizeof(qtmaskd)))
                    owl_ui_lineedit_set_mask(widget, qtmaskd);
                /* format variable -> pas de masque (saisie libre), rien à poser. */
                owl_ui_lineedit_set_duree_validator(widget, fmt);
            } else if (widget && (typeSaisie == OWL_TYPESAISIE_DATE ||
                                   typeSaisie == OWL_TYPESAISIE_HEURE)) {
                /*  — champ Date/Heure sans masque de saisie explicite :
                 * dérive un masque de FRAPPE GUIDÉE depuis le format effectif
                 * (MasqueAffichage posé > projet > défaut). Chiffres seuls,
                 * séparateurs auto-insérés, lettres rejetées. Le store d'instance
                 * n'existe pas encore à la construction : on lit MasqueAffichage
                 * directement depuis la décl (et le défaut projet Date/Heure). */
                const char* fmt = prop_str(ctrl->properties, ctrl->property_count,
                                           "MasqueAffichage", "DisplayMask", NULL);
                if (typeSaisie == OWL_TYPESAISIE_DATE) {
                    if (fmt == NULL || fmt[0] == '\0') {
                        const char* pf = compiler_projet_format_date();
                        fmt = (pf != NULL && pf[0] != '\0') ? pf : "JJ/MM/AAAA";
                    }
                } else if (fmt == NULL || fmt[0] == '\0') {
                    /*  — repli FormatHeure projet (miroir Date). */
                    const char* pf = compiler_projet_format_heure();
                    fmt = (pf != NULL && pf[0] != '\0') ? pf : "HH:MI";
                }
                char qtmask[128];
                if (input_frappe_mask_from_format(fmt, qtmask, sizeof(qtmask)))
                    owl_ui_lineedit_set_mask(widget, qtmask);
                /* jetons nommés -> pas de masque (saisie libre), rien à poser. */
                /*  — validateur de plages (borne les VALEURS à la
                 * frappe), en plus du masque, dérivé du même format. */
                owl_ui_lineedit_set_date_time_validator(widget, fmt);
            }
            /*  — libelle integre : composite [QLabel | Saisie] rendu si
             * texte non vide ET largeur > 0. prop_str resout le multilingue sur
             * la langue courante. Le QLineEdit reste le controle logique. */
            if (widget) {
                const char* lblTxt = prop_str(ctrl->properties, ctrl->property_count,
                                               "Libelle", "Caption", "");
                int lblW = prop_int(ctrl->properties, ctrl->property_count,
                                     "LargeurLibelle", "LabelWidth", 0);
                owl_ui_lineedit_set_label_text(widget, lblTxt ? lblTxt : "");
                owl_ui_lineedit_set_label_width(widget, lblW);
            }
            /* Lot D : cabler les blocs PROCEDURE surValidation/surAChaqueModification/
             * surEntrée/surSortie aux signaux Qt correspondants.
             * Canon OWL : prefixe sur* systematique. Alias EN canon (onX).
             * Les noms sans prefixe (Clic/Validation/Modification) sont
             * inacceptes — break-net assume vs canon strict. */
            if (widget) {
                static const OwlEventBind0 evs[] = {
                    {"surValidation",          "onValidation", owl_ui_lineedit_on_validation},
                    {"surAChaqueModification", "onEachChange", owl_ui_lineedit_on_change},
                };
                BIND_EVENTS_0(widget, ctrl, evs);
                BIND_EVENTS_0(widget, ctrl, EVS_FOCUS);
            }
            break;
        }
        case OWL_CONTROL_TABLE: {
            /* Phase 0/1/2/8 : cree le widget, ajoute les colonnes,
             * cable les events lifecycle. */
            widget = owl_ui_table_create(parent, x, y, w, h);
            if (widget) {
                for (int ci = 0; ci < ctrl->column_count; ci++) {
                    const OwlColumnDecl* col = &ctrl->columns[ci];
                    const char* title = NULL;
                    int colType = 0;       /* 0 fallback = colTypeTexte logique */
                    int colWidth = -1;
                    bool colVisible = true;  /* defaut Vrai comme un controle */
                    bool colEditable = true; /* Etat = Actif par defaut */
                    const char* colMask = NULL;  /*  — masque declare */
                    for (int pi = 0; pi < col->property_count; pi++) {
                        const OwlPropertyInit* p = &col->properties[pi];
                        if (!p->is_constant) continue;
                        if (strcasecmp(p->name, "Libelle") == 0
                         || strcasecmp(p->name, "Caption") == 0) {
                            if (IS_STRING(p->const_value))
                                title = AS_CSTRING(p->const_value);
                        } else if (strcasecmp(p->name, "MasqueSaisie") == 0
                                || strcasecmp(p->name, "InputMask") == 0) {
                            /*  — le masque de saisie declare sur la colonne
                             * est cable ici jusqu'au widget : createEditor
                             * du delegate colonne le posera sur l'editeur de cellule. */
                            if (IS_STRING(p->const_value))
                                colMask = AS_CSTRING(p->const_value);
                        } else if (strcasecmp(p->name, "Type") == 0) {
                            if (IS_INTEGER(p->const_value))
                                colType = (int)AS_INTEGER(p->const_value);
                        } else if (strcasecmp(p->name, "Largeur") == 0
                                || strcasecmp(p->name, "Width") == 0) {
                            if (IS_INTEGER(p->const_value))
                                colWidth = (int)AS_INTEGER(p->const_value);
                        } else if (strcasecmp(p->name, "Visible") == 0) {
                            if (IS_BOOL(p->const_value))
                                colVisible = AS_BOOL(p->const_value);
                        } else if (strcasecmp(p->name, "Etat") == 0
                                || strcasecmp(p->name, "State") == 0) {
                            /* AffichageSeulement/Inactif : colonne en lecture
                             * seule. Ses cases a cocher restent affichees mais
                             * ne basculent plus au clic. */
                            if (IS_INTEGER(p->const_value)
                             && (int)AS_INTEGER(p->const_value)
                                    == OWL_ETAT_AFFICHAGE_SEUL)
                                colEditable = false;
                        }
                    }
                    /* Caption/Libelle absente => en-tete VIDE, pas le nom
                     * d'identifiant : aligne le runtime sur le designer
                     * canvas. Caption="" donnait deja un titre
                     * vide (string presente) ; absente le donne aussi. */
                    if (!title) title = "";
                    owl_ui_table_add_column(widget, title, colType, colWidth);
                    /* Visible=Faux : masque la colonne (col_1based = ci+1) sans
                     * la retirer du modele — column_count reste inchange, les
                     * donnees ne se decalent pas. */
                    if (!colVisible)
                        owl_ui_table_set_column_hidden(widget, ci + 1, 1);
                    if (!colEditable)
                        owl_ui_table_set_column_editable(widget, ci + 1, 0);
                    /*  — masque declare -> view (col_mask_<col0>) +
                     * delegate masquant. Path A : createEditor l'applique.
                     *  — a defaut de masque brut, une colonne TYPEE
                     * derive son comportement d'edition du FORMAT (framework
                     * type-driven, cf. le dispatch de createEditor). Le masque
                     * brut declare reste un override bas-niveau (precedence). */
                    if (colMask && colMask[0]) {
                        owl_ui_table_set_column_mask(widget, ci + 1, colMask);
                    } else if (colType == OWL_COL_TYPE_DATE
                            || colType == OWL_COL_TYPE_HEURE
                            || colType == OWL_COL_TYPE_DUREE) {
                        /* Colonne TYPEE sans masque brut : format defaut/projet
                         * (miroir de la Saisie Date/Heure/Duree sans MasqueAffichage,
                         * L1710-1753) -> masque de frappe pre-derive + validateur
                         * de plages cote GUI. Le format et le moteur de masque
                         * different par type ; le case du framework (createEditor)
                         * choisit le validateur adequat via col_type_<col0>. */
                        const char* fmt;
                        char qtmask[128];
                        qtmask[0] = '\0';
                        if (colType == OWL_COL_TYPE_DUREE) {
                            /* Duree : masque DEDIE, HH non plafonne. */
                            const char* pf = compiler_projet_format_duree();
                            fmt = (pf != NULL && pf[0] != '\0') ? pf : "HH:MI:SS";
                            if (!input_duree_frappe_mask_from_format(fmt, qtmask,
                                                                     sizeof(qtmask)))
                                qtmask[0] = '\0'; /* format variable -> saisie libre */
                        } else {
                            if (colType == OWL_COL_TYPE_DATE) {
                                const char* pf = compiler_projet_format_date();
                                fmt = (pf != NULL && pf[0] != '\0')
                                          ? pf : "JJ/MM/AAAA";
                            } else { /* OWL_COL_TYPE_HEURE ( repli projet) */
                                const char* pf = compiler_projet_format_heure();
                                fmt = (pf != NULL && pf[0] != '\0') ? pf : "HH:MI";
                            }
                            if (!input_frappe_mask_from_format(fmt, qtmask,
                                                               sizeof(qtmask)))
                                qtmask[0] = '\0'; /* jetons nommes -> saisie libre */
                        }
                        owl_ui_table_set_column_format(widget, ci + 1,
                                                       fmt, qtmask);
                    } else if (colType == OWL_COL_TYPE_NUMERIQUE
                            || colType == OWL_COL_TYPE_MONETAIRE
                            || colType == OWL_COL_TYPE_MOTDEPASSE) {
                        /* Numerique/Monetaire/MotDePasse sans masque brut : PAS de
                         * masque a positions fixes (longueur variable). On installe
                         * quand meme le delegate colonne (fmt/mask vides) pour que
                         * (1) createEditor pose le comportement d'edition a la
                         * frappe (validateur numerique ; echo masque pour MotDePasse),
                         * (2) initStyleOption transforme l'affichage hors edition
                         * (format defaut/projet ou masquage en puces, resolu au rendu
                         * par owl_ui_format_cell_typed). Le col_type_<col0> pose par
                         * add_column reste le discriminateur du framework. */
                        owl_ui_table_set_column_format(widget, ci + 1, "", "");
                    }
                }

                /* Events lifecycle propres a la Table (signaux QTableView). */
                static const OwlEventBind0 evs[] = {
                    {"surClic",                     "onClick",
                        owl_ui_table_on_click},
                    {"surDoubleClic",               "onDoubleClick",
                        owl_ui_table_on_double_click},
                    {"surChangementSelection",      "onSelectionChange",
                        owl_ui_table_on_selection},
                    {"surEntr\xc3\xa9""eLigne",     "onRowEnter",
                        owl_ui_table_on_row_enter},
                    {"surSortieLigne",              "onRowExit",
                        owl_ui_table_on_row_exit},
                };
                BIND_EVENTS_0(widget, ctrl, evs);
                /* surEntree / surSortie : la Table declare FAM_FOCUS mais le
                 * QTableView n'expose aucun signal de focus -> event filter
                 * commun, seule facon d'avoir la parite avec le headless
                 *. */
                BIND_EVENTS_0(widget, ctrl, EVS_FOCUS);
            }
            break;
        }
        case OWL_CONTROL_CONTAINER:
            /* Lot F — runtime build du Conteneur viendra en F.3 (backend Qt
             * QWidget vide + QLayout). Stub silencieux pour -Wswitch. */
            break;
        case OWL_CONTROL_IMAGE: {
            /* Champ Image V2 : OwlImageLabel custom (subclass QLabel) qui
             * intercepte les events souris/roulette/lifecycle et dispatche
             * vers les PROC OWL via dispatch_image_event_*. */
            widget = owl_ui_image_field_create(parent, x, y, w, h);
            /* Cadrage AVANT Image pour que set_image_rgba l'utilise.
             * Defaut -1 (non specifie) ; valeurs sentinels OWL_CADIMG_*
             * negatives (base INT32_MIN). Tester `>= 0` rejetait donc
             * TOUTE valeur reelle -> cadrage declare ignore. Comparer a
             * -1 comme le bloc Libelle / CadrageH. */
            int cadrage = prop_int(ctrl->properties, ctrl->property_count,
                                    "Cadrage", "Framing", -1);
            if (cadrage != -1 && widget) owl_ui_label_set_cadrage(widget, cadrage);
            /* Zoom (V2) — applique avant Image. */
            int zoom = prop_int(ctrl->properties, ctrl->property_count,
                                 "Zoom", "Zoom", 100);
            if (widget) owl_ui_image_field_set_zoom(widget, zoom);
            /* Image — string (chemin), ObjImage en memoire, ou Icone (
             * polymorphisme symetrique au Bouton Cas K ). */
            Value imgVal = prop_value(ctrl->properties, ctrl->property_count,
                                       "Image", "Image");
            if (widget && stdlib_image_is_instance(imgVal)) {
                int iw = 0, ih = 0;
                const unsigned char* rgba = stdlib_image_get_rgba(imgVal, &iw, &ih);
                int cad = (cadrage != -1) ? cadrage : OWL_CADIMG_CENTRE;
                owl_ui_label_set_image_rgba(widget, rgba, iw, ih, cad);
            } else if (widget && IS_ICONE(imgVal)) {
                /*  — Champ Image accepte Icone. Rasterise via Qt6Svg
                 * puis push au QLabel. Pattern symetrique au Bouton Cas K.
                 *  : forme + couleur_fond. */
                ObjIcone* icn = AS_ICONE(imgVal);
                if (icn->valide && icn->svg_raw != NULL
                    && icn->svg_raw->length > 0) {
                    unsigned char* rgba = NULL;
                    int iw = 0, ih = 0;
                    bool want_bg = (icn->couleur_fond != -1);
                    /*  — auto = dimension du champ (rasterise net a
                     * la bonne resolution), override = ..ImageTaille. */
                    int icoOverride = prop_int(ctrl->properties, ctrl->property_count,
                                                "ImageTaille", "ImageSize", 0);
                    owl_ui_control_set_icon_size_override(widget, icoOverride);
                    int fieldDim = (w < h) ? w : h;
                    int icoSize = icone_target_size(fieldDim, icoOverride);
                    if (owl_icone_rasterize_rgba(
                            icn->svg_raw->chars, icn->svg_raw->length,
                            /*  : -1 unset -> 0 (noir) defaut au lieu de 0xFFFFFF blanc. */
                            (icn->couleur == -1) ? 0u : (unsigned int)(icn->couleur & 0xFFFFFF),
                            icn->couleur_modifiable,
                            icoSize, icn->rotation,
                            icn->forme,
                            (unsigned int)((want_bg ? icn->couleur_fond : 0) & 0xFFFFFF),
                            want_bg,
                            &rgba, &iw, &ih)) {
                        int cad = (cadrage != -1) ? cadrage : OWL_CADIMG_CENTRE;
                        owl_ui_label_set_image_rgba(widget, rgba, iw, ih, cad);
                        free(rgba);
                    }
                }
            } else if (widget && IS_STRING(imgVal)) {
                owl_ui_label_set_image(widget, AS_CSTRING(imgVal));
            }

            /* V2 events : cabling souris/molette/menu/clavier partage avec le
             * Champ CodeBarres (meme backend OwlImageLabel). */
            bind_image_field_events(widget, ctrl);
            break;
        }
        case OWL_CONTROL_CODEBARRE: {
            /* Champ CodeBarres : calque strict du Champ Image (backend
             * OwlImageLabel). L'image est rasterisee depuis les props
             * (type + contenu + correction d'erreur) via le pipeline partage
             * avec CodeBarreVersImage, puis poussee au QLabel. */
            widget = owl_ui_image_field_create(parent, x, y, w, h);
            if (widget) {
                int64_t type = prop_int(ctrl->properties, ctrl->property_count,
                                        "TypeCodeBarres", "TypeBarCode", OWL_CB_QRCODE);
                int64_t ecc  = prop_int(ctrl->properties, ctrl->property_count,
                                        "CorrectionErreur", "CorrectionError", OWL_QR_ECC_M);
                int64_t opts = prop_int(ctrl->properties, ctrl->property_count,
                                        "Option", "Option", 0);
                const char* contenu = prop_str(ctrl->properties, ctrl->property_count,
                                               "Contenu", "Content", NULL);
                if (contenu && *contenu) {
                    Value imgVal = owl_codebarre_image_value_from_params(
                        type, ecc, opts, contenu, (int)strlen(contenu),
                        cb_field_supersample(w), cb_field_supersample(h));
                    if (stdlib_image_is_instance(imgVal)) {
                        vm_push(imgVal);  /* root pendant le readback RGBA */
                        int iw = 0, ih = 0;
                        const unsigned char* rgba =
                            stdlib_image_get_rgba(imgVal, &iw, &ih);
                        owl_ui_label_set_image_rgba(widget, rgba, iw, ih,
                                                    OWL_CADIMG_HOMOTHETIQUE);
                        vm_pop();
                    }
                    /*  — stocke la config pour la re-rasterisation nette
                     * au resize (le premier resize de layout corrige aussi le
                     * flou initial). */
                    owl_ui_codebarre_set_config(widget, type, ecc, opts,
                                                contenu, (int)strlen(contenu));
                }
                bind_image_field_events(widget, ctrl);
            }
            break;
        }
        case OWL_CONTROL_LISTE: {
            widget = owl_ui_list_create(parent, x, y, w, h);
            if (!widget) break;
            /* Multiselection / Triee. */
            bool multi = prop_bool(ctrl->properties, ctrl->property_count,
                                    "Multis\xc3\xa9lection", "MultiSelection", false);
            owl_ui_list_set_multiselect(widget, multi ? 1 : 0);
            bool sorted = prop_bool(ctrl->properties, ctrl->property_count,
                                     "Tri\xc3\xa9""e", "Sorted", false);
            if (sorted) owl_ui_list_set_sorted(widget, 1);
            int sortOpt = prop_int(ctrl->properties, ctrl->property_count,
                                    "OptionTri", "SortOption", 0);
            if (sortOpt > 0) owl_ui_list_set_sort_option(widget, sortOpt);
            int zoom = prop_int(ctrl->properties, ctrl->property_count,
                                 "Zoom", "Zoom", 100);
            if (zoom != 100) owl_ui_list_set_zoom(widget, zoom);
            /* ContenuInitial / Contenu : bourrage initial multiligne. */
            const char* contenuInit = prop_str(ctrl->properties, ctrl->property_count,
                                                "ContenuInitial", "InitialContent", NULL);
            if (contenuInit && *contenuInit) {
                owl_ui_list_set_content(widget, contenuInit);
            } else {
                const char* contenu = prop_str(ctrl->properties, ctrl->property_count,
                                                "Contenu", "Content", NULL);
                if (contenu && *contenu) owl_ui_list_set_content(widget, contenu);
            }
            /* ValeurInitiale : positionne la selection initiale (chaine =
             * recherche par texte ; ignore si introuvable). */
            Value vi = prop_value(ctrl->properties, ctrl->property_count,
                                   "ValeurInitiale", "InitialValue");
            if (IS_STRING(vi)) {
                int idx = owl_ui_list_find(widget, AS_CSTRING(vi));
                if (idx > 0) owl_ui_list_set_selection(widget, idx);
            } else if (IS_INTEGER(vi)) {
                owl_ui_list_set_selection(widget, (int)AS_INTEGER(vi));
            }
            /* Cabling events 0-arg : surSelection / surEntree / surSortie. */
            static const OwlEventBind0 evs[] = {
                {"surS\xc3\xa9lection", "onSelection", owl_ui_list_on_selection},
                {"surEntr\xc3\xa9""e",  "onEntry",     owl_ui_list_on_entry},
                {"surSortie",            "onExit",      owl_ui_list_on_exit},
            };
            BIND_EVENTS_0(widget, ctrl, evs);
            break;
        }
        case OWL_CONTROL_COMBO: {
            widget = owl_ui_combo_create(parent, x, y, w, h);
            if (!widget) break;
            bool sorted = prop_bool(ctrl->properties, ctrl->property_count,
                                     "Tri\xc3\xa9""e", "Sorted", false);
            if (sorted) owl_ui_listcombo_set_sorted(widget, 1);
            bool editable = prop_bool(ctrl->properties, ctrl->property_count,
                                       "EnSaisie", "Editable", false);
            if (editable) owl_ui_combo_set_editable(widget, 1);
            int maxV = prop_int(ctrl->properties, ctrl->property_count,
                                 "MaxLignesAffichees", "MaxVisibleItems", 0);
            if (maxV > 0) owl_ui_combo_set_max_visible(widget, maxV);
            /*  — libelle integre : composite [QLabel | Combo] rendu si
             * texte non vide ET largeur > 0 (calque du Saisie). Le QComboBox
             * reste le controle logique. */
            {
                const char* lblTxt = prop_str(ctrl->properties, ctrl->property_count,
                                               "Libelle", "Caption", "");
                int lblW = prop_int(ctrl->properties, ctrl->property_count,
                                     "LargeurLibelle", "LabelWidth", 0);
                owl_ui_combo_set_label_text(widget, lblTxt ? lblTxt : "");
                owl_ui_combo_set_label_width(widget, lblW);
            }
            const char* contenuInit = prop_str(ctrl->properties, ctrl->property_count,
                                                "ContenuInitial", "InitialContent", NULL);
            if (contenuInit && *contenuInit) {
                owl_ui_listcombo_set_content(widget, contenuInit);
            } else {
                const char* contenu = prop_str(ctrl->properties, ctrl->property_count,
                                                "Contenu", "Content", NULL);
                if (contenu && *contenu) owl_ui_listcombo_set_content(widget, contenu);
            }
            Value vi = prop_value(ctrl->properties, ctrl->property_count,
                                   "ValeurInitiale", "InitialValue");
            if (IS_STRING(vi)) {
                int idx = owl_ui_listcombo_find(widget, AS_CSTRING(vi));
                if (idx > 0) owl_ui_listcombo_set_selection(widget, idx);
            } else if (IS_INTEGER(vi)) {
                owl_ui_listcombo_set_selection(widget, (int)AS_INTEGER(vi));
            }
            /* Cabling events 0-arg : surSelection / surEntree / surSortie /
             * surAChaqueModification + surOuverture / surFermeture. */
            static const OwlEventBind0 evs[] = {
                {"surS\xc3\xa9lection",       "onSelection",    owl_ui_combo_on_selection},
                {"surEntr\xc3\xa9""e",        "onEntry",        owl_ui_combo_on_entry},
                {"surSortie",                  "onExit",         owl_ui_combo_on_exit},
                {"surAChaqueModification",     "onEachChange",   owl_ui_combo_on_change},
                {"surOuverture",               "onOpen",         owl_ui_combo_on_open},
                {"surFermeture",               "onClose",        owl_ui_combo_on_close},
            };
            BIND_EVENTS_0(widget, ctrl, evs);
            break;
        }
        case OWL_CONTROL_SELECTEUR: {
            widget = owl_ui_selecteur_create(parent, x, y, w, h);
            if (!widget) break;
            int mode = prop_int(ctrl->properties, ctrl->property_count,
                                 "ModeAffichage", "DisplayMode", 0);
            owl_ui_selecteur_set_display_mode(widget, mode);
            int cols = prop_int(ctrl->properties, ctrl->property_count,
                                 "NombreColonne", "ColumnCount", 1);
            owl_ui_selecteur_set_column_count(widget, cols);
            int optH = prop_int(ctrl->properties, ctrl->property_count,
                                 "HauteurOption", "OptionHeight", 0);
            owl_ui_selecteur_set_option_height(widget, optH);
            /* Options polymorphique : array de chaines OU array d'instances. */
            Value optsV = prop_value(ctrl->properties, ctrl->property_count,
                                      "Options", "Options");
            if (IS_ARRAY(optsV)) {
                ObjArray* arr = AS_ARRAY(optsV);
                for (int i = 0; i < arr->elements.count; i++) {
                    Value v = arr->elements.values[i];
                    if (IS_STRING(v)) {
                        owl_ui_selecteur_add_option(widget, AS_CSTRING(v),
                                                     NULL, NULL, NULL);
                    } else if (IS_INSTANCE(v)) {
                        ObjInstance* inst = AS_INSTANCE(v);
                        ObjString* kLib = obj_copy_string("Libelle", 7);
                        ObjString* kSub = obj_copy_string("SousLibelle", 11);
                        ObjString* kVal = obj_copy_string("ValeurRenvoyee", 14);
                        ObjString* kImg = obj_copy_string("Image", 5);
                        Value vLib = NIL_VAL, vSub = NIL_VAL, vVal = NIL_VAL, vImg = NIL_VAL;
                        table_get(&inst->fields, kLib, &vLib);
                        table_get(&inst->fields, kSub, &vSub);
                        table_get(&inst->fields, kVal, &vVal);
                        table_get(&inst->fields, kImg, &vImg);
                        const char* lib = IS_STRING(vLib) ? AS_CSTRING(vLib) : "";
                        const char* sub = IS_STRING(vSub) ? AS_CSTRING(vSub) : NULL;
                        const char* val = IS_STRING(vVal) ? AS_CSTRING(vVal) : NULL;
                        const char* img = IS_STRING(vImg) ? AS_CSTRING(vImg) : NULL;
                        owl_ui_selecteur_add_option(widget, lib, sub, val, img);
                    }
                }
            }
            /* ValeurInitiale : entier (index) ou string (lookup ValeurRenvoyee). */
            Value vi = prop_value(ctrl->properties, ctrl->property_count,
                                   "ValeurInitiale", "InitialValue");
            if (IS_INTEGER(vi)) {
                owl_ui_selecteur_set_selection_index(widget, (int)AS_INTEGER(vi));
            } else if (IS_STRING(vi)) {
                /* Lookup par stored value. */
                int n = owl_ui_selecteur_count(widget);
                const char* needle = AS_CSTRING(vi);
                for (int i = 1; i <= n; i++) {
                    const char* s = owl_ui_selecteur_get_option_stored(widget, i);
                    if (s && strcmp(s, needle) == 0) {
                        owl_ui_selecteur_set_selection_index(widget, i);
                        break;
                    }
                }
            }
            /* Cabling events : surAChaqueModification, surEntree, surSortie. */
            static const OwlEventBind0 evs[] = {
                {"surAChaqueModification", "onEachChange",
                    owl_ui_selecteur_on_change},
                {"surEntr\xc3\xa9""e",     "onEntry",
                    owl_ui_selecteur_on_entry},
                {"surSortie",              "onExit",
                    owl_ui_selecteur_on_exit},
            };
            BIND_EVENTS_0(widget, ctrl, evs);
            /* surClic / surDoubleClic sur le conteneur : le QFrame n'a pas de
             * signal de clic, l'event filter commun les relaie. */
            BIND_EVENTS_0(widget, ctrl, EVS_MOUSE_BASIC);
            break;
        }
        case OWL_CONTROL_INTERRUPTEUR: {
            widget = owl_ui_interrupteur_create(parent, x, y, w, h);
            if (!widget) break;
            int mode = prop_int(ctrl->properties, ctrl->property_count,
                                 "ModeAffichage", "DisplayMode", 0);
            owl_ui_interrupteur_set_display_mode(widget, mode);
            int cols = prop_int(ctrl->properties, ctrl->property_count,
                                 "NombreColonne", "ColumnCount", 1);
            owl_ui_interrupteur_set_column_count(widget, cols);
            int optH = prop_int(ctrl->properties, ctrl->property_count,
                                 "HauteurOption", "OptionHeight", 0);
            owl_ui_interrupteur_set_option_height(widget, optH);
            bool tri = prop_bool(ctrl->properties, ctrl->property_count,
                                  "TroisEtats", "ThreeStates", false);
            owl_ui_interrupteur_set_three_states(widget, tri ? 1 : 0);
            /* Options : array de chaines OU array d'instances. */
            Value optsV = prop_value(ctrl->properties, ctrl->property_count,
                                      "Options", "Options");
            if (IS_ARRAY(optsV)) {
                ObjArray* arr = AS_ARRAY(optsV);
                for (int i = 0; i < arr->elements.count; i++) {
                    Value v = arr->elements.values[i];
                    if (IS_STRING(v)) {
                        owl_ui_interrupteur_add_option(widget, AS_CSTRING(v),
                                                       NULL, NULL);
                    } else if (IS_INSTANCE(v)) {
                        ObjInstance* inst = AS_INSTANCE(v);
                        ObjString* kLib = obj_copy_string("Libelle", 7);
                        ObjString* kSub = obj_copy_string("SousLibelle", 11);
                        ObjString* kImg = obj_copy_string("Image", 5);
                        Value vLib = NIL_VAL, vSub = NIL_VAL, vImg = NIL_VAL;
                        table_get(&inst->fields, kLib, &vLib);
                        table_get(&inst->fields, kSub, &vSub);
                        table_get(&inst->fields, kImg, &vImg);
                        const char* lib = IS_STRING(vLib) ? AS_CSTRING(vLib) : "";
                        const char* sub = IS_STRING(vSub) ? AS_CSTRING(vSub) : NULL;
                        const char* img = IS_STRING(vImg) ? AS_CSTRING(vImg) : NULL;
                        owl_ui_interrupteur_add_option(widget, lib, sub, img);
                    }
                }
            }
            /* ValeurInitiale = bitmask. */
            Value vi = prop_value(ctrl->properties, ctrl->property_count,
                                   "ValeurInitiale", "InitialValue");
            if (IS_INTEGER(vi)) {
                owl_ui_interrupteur_set_bitmask(widget, AS_INTEGER(vi));
            }
            /* Cabling events. */
            static const OwlEventBind0 evs[] = {
                {"surAChaqueModification", "onEachChange",
                    owl_ui_interrupteur_on_change},
                {"surEntr\xc3\xa9""e",     "onEntry",
                    owl_ui_interrupteur_on_entry},
                {"surSortie",              "onExit",
                    owl_ui_interrupteur_on_exit},
            };
            BIND_EVENTS_0(widget, ctrl, evs);
            BIND_EVENTS_0(widget, ctrl, EVS_MOUSE_BASIC);
            break;
        }
        case OWL_CONTROL_ZONE_REPETEE: {
            widget = owl_ui_zr_create(parent, x, y, w, h);
            if (!widget) break;
            /* Sens : zrVertical est l'unique valeur (repetition vers le bas),
             * cablee dans le layout du widget a la creation. Rien a appliquer. */
            bool multi = prop_bool(ctrl->properties, ctrl->property_count,
                                    "Multiselection", "MultiSelection", false);
            owl_ui_zr_set_multi_select(widget, multi ? 1 : 0);
            /* Cabling event surSelectionLigne — declenche au clic sur une
             * rep ou via setter LigneSelectionnee. Payload = (indice, 0). */
            const OwlEventBlock* selev = find_event(ctrl->events,
                ctrl->event_count, "surS\xc3\xa9lectionLigne",
                "onRowSelection");
            if (selev) {
                int bidx = register_binding_named(selev->impl, false, ctrl->name);
                if (bidx >= 0) {
                    owl_ui_zr_on_row_select(widget, dispatch_image_event_2i,
                                             (void*)(intptr_t)bidx);
                }
            }
            /* surClic / surDoubleClic / surEntree / surSortie : le conteneur
             * n'expose aucun signal Qt correspondant, l'event filter commun les
             * relaie. */
            BIND_EVENTS_0(widget, ctrl, EVS_MOUSE_BASIC);
            BIND_EVENTS_0(widget, ctrl, EVS_FOCUS);
            /* Le clonage du template + l'instanciation des sub-controls a
             * chaque AjouteLigne est fait par les natives ZoneRepetee*. */
            break;
        }
        case OWL_CONTROL_JAUGE: {
            widget = owl_ui_jauge_create(parent, x, y, w, h);
            if (!widget) break;
            int bmin = prop_int(ctrl->properties, ctrl->property_count,
                                 "BorneMin", "MinValue", 0);
            int bmax = prop_int(ctrl->properties, ctrl->property_count,
                                 "BorneMax", "MaxValue", 100);
            owl_ui_jauge_set_range(widget, bmin, bmax);
            bool vert = prop_bool(ctrl->properties, ctrl->property_count,
                                   "Vertical", "Vertical", false);
            owl_ui_jauge_set_vertical(widget, vert ? 1 : 0);
            bool sym = prop_bool(ctrl->properties, ctrl->property_count,
                                  "Symetrique", "Symmetric", false);
            owl_ui_jauge_set_symmetric(widget, sym ? 1 : 0);
            int couleurJauge = prop_int(ctrl->properties, ctrl->property_count,
                                         "CouleurJauge", "ProgressBarColor", -1);
            if (couleurJauge >= 0) owl_ui_jauge_set_progress_color(widget, couleurJauge);
            const char* txtJauge = prop_str(ctrl->properties, ctrl->property_count,
                                             "TexteJauge", "TextProgressBar", NULL);
            if (txtJauge) owl_ui_jauge_set_text(widget, txtJauge);
            /* CadrageHorizontal du texte % : sentinel -1 = non declare (defaut
             * centre porte par le widget). Alignement dedie (le paint peint le
             * texte lui-meme). */
            int cadHJauge = prop_int(ctrl->properties, ctrl->property_count,
                                      "CadrageHorizontal", "HorizontalAlignment", -1);
            if (cadHJauge != -1) owl_ui_jauge_set_h_align(widget, cadHJauge);
            /* Valeur en dernier : bornee a la plage deja posee (setRange avant). */
            Value valJauge = prop_value(ctrl->properties, ctrl->property_count,
                                         "Valeur", "Value");
            if (IS_INTEGER(valJauge)) owl_ui_jauge_set_value(widget, (int)AS_INTEGER(valJauge));
            break;
        }
        case OWL_CONTROL_ONGLET:
        case OWL_CONTROL_VOLET:
            /*  lot 1 — kinds declares au compile-time (DSL/IR/catalog).
             * Le rendu Qt (QTabWidget + pages) arrive au lot 2. Aucun widget
             * materialise ici pour l'instant : keep-alive -Wswitch. */
            break;
        case OWL_CONTROL_WINDOW:
            /* Virtuel — un controle de kind WINDOW ne devrait pas atterrir
             * dans build_control. Silencieux pour eviter warning -Wswitch. */
            break;
    }

    if (widget) {
        /*  lot 3 — rattache le widget Qt frais a son ObjUiControlInstance
         * AVANT d'appliquer les props composees : le store de l'uici est la
         * source de verite (seede depuis le declaratif), Qt l'applique. Le
         * wiring precede donc le bloc Police/Cadre. */
        ObjUiControlInstance* uici = current_window_uici_for_decl(ctrl);
        if (uici != NULL) uici->widget = widget;

        bool visible = prop_bool(ctrl->properties, ctrl->property_count,
                                  "Visible", "Visible", true);
        bool grise   = prop_bool(ctrl->properties, ctrl->property_count,
                                  "Grise", "Disabled", false);
        if (!visible) owl_ui_control_set_visible(widget, 0);
        if (grise)    owl_ui_control_set_enabled(widget, 0);
        const char* bulle = prop_str(ctrl->properties, ctrl->property_count,
                                      "Bulle", "Tooltip", NULL);
        if (bulle) owl_ui_control_set_tooltip(widget, bulle);

        /* Clavier — un seul point de cablage pour TOUS les champs, pilote par
         * le catalogue : tout kind qui declare surToucheAppuyee recoit le
         * filtre. Suivre le catalogue plutot qu'enumerer les `case` supprime
         * la classe d'oubli "event declarable au catalogue mais jamais tire au
         * runtime". */
        if (owl_event_catalog_find(ctrl->kind,
                                    "surToucheAppuy\xc3\xa9""e") != NULL) {
            bind_events_key(widget, ctrl);
        }

        /*  — ToucheRaccourci sur les champs interactifs non-bouton :
         * le raccourci DONNE LE FOCUS au champ. Le Bouton, lui, gere sa propre
         * ToucheRaccourci (action = clic) dans son case build ci-dessus via
         * owl_ui_button_set_shortcut — on l'exclut donc ici. */
        switch (ctrl->kind) {
            case OWL_CONTROL_INPUT:
            case OWL_CONTROL_COMBO:
            case OWL_CONTROL_LISTE:
            case OWL_CONTROL_SELECTEUR:
            case OWL_CONTROL_INTERRUPTEUR:
            case OWL_CONTROL_TABLE:
            case OWL_CONTROL_ZONE_REPETEE: {
                const char* fsc = prop_str(ctrl->properties, ctrl->property_count,
                                            "ToucheRaccourci", "ShortcutKey", NULL);
                if (fsc) owl_ui_set_focus_shortcut(widget, fsc);
                break;
            }
            default: break;
        }

        /* Police composee : la prop Police (posee comme INSTANCE en PROPRIETES)
         * a ete seedee dans le store de l'uici. Les raccourcis plats declaratifs
         * (PoliceNom/Taille/...) ecrivent dans cette MEME instance du store,
         * garantissant l'equivalence raccourci <-> cascade. Apply final au
         * QWidget depuis le store (plus de global __police_). */
        Value policeInstVal = prop_value(ctrl->properties, ctrl->property_count,
                                          "Police", "Font");
        /* polInst = instance Police du store (materialisee lazy si un raccourci
         * plat est pose mais aucune prop Police composee). */
        ObjInstance* polInst = (IS_INSTANCE(policeInstVal) && uici)
            ? uici_composite_instance(uici, "Police", "Police") : NULL;
        const char* policeNomFlat = prop_str(ctrl->properties, ctrl->property_count,
                                              "PoliceNom", "FontName", NULL);
        if (policeNomFlat) {
            if (!polInst && uici) polInst = uici_composite_instance(uici, "Police", "Police");
            if (polInst) {
                inst_field_set_str(polInst, "Nom", policeNomFlat);
            }
        }
        int policeTaille = prop_int(ctrl->properties, ctrl->property_count,
                                     "PoliceTaille", "FontSize", 0);
        if (policeTaille > 0) {
            if (!polInst && uici) polInst = uici_composite_instance(uici, "Police", "Police");
            if (polInst)
                inst_field_set_int(polInst, "Taille", policeTaille);
        }
        if (prop_bool(ctrl->properties, ctrl->property_count,
                       "PoliceGras", "FontBold", false)) {
            if (!polInst && uici) polInst = uici_composite_instance(uici, "Police", "Police");
            if (polInst)
                inst_field_set_bool(polInst, "Gras", true);
        }
        if (prop_bool(ctrl->properties, ctrl->property_count,
                       "PoliceItalique", "FontItalic", false)) {
            if (!polInst && uici) polInst = uici_composite_instance(uici, "Police", "Police");
            if (polInst)
                inst_field_set_bool(polInst, "Italique", true);
        }
        if (prop_bool(ctrl->properties, ctrl->property_count,
                       "PoliceSoulignee", "FontUnderlined", false)) {
            if (!polInst && uici) polInst = uici_composite_instance(uici, "Police", "Police");
            if (polInst)
                inst_field_set_bool(polInst, "Souligne", true);
        }
        if (prop_bool(ctrl->properties, ctrl->property_count,
                       "PoliceBarree", "FontStrikeOut", false)) {
            if (!polInst && uici) polInst = uici_composite_instance(uici, "Police", "Police");
            if (polInst)
                inst_field_set_bool(polInst, "Barre", true);
        }
        int policeCouleur = prop_int(ctrl->properties, ctrl->property_count,
                                      "PoliceCouleur", "FontColor", -1);
        if (policeCouleur >= 0) {
            if (!polInst && uici) polInst = uici_composite_instance(uici, "Police", "Police");
            if (polInst)
                inst_field_set_int(polInst, "Couleur", policeCouleur);
        }
        /* Apply final : si une instance Police existe dans le store (composee ou
         * via raccourci), pousse tous ses champs au QWidget en une fois. */
        if (polInst && widget)
            apply_police_instance_to_widget(widget, polInst);

        /* Cadrage horizontal/vertical (Libelle + Saisie ; Bouton no-op).
         * Defaut -1 (non specifie). Les valeurs sentinels OWL_CH/OWL_CV sont
         * negatives (base INT32_MIN). Tester `>= 0` rejetait donc TOUTE valeur
         * reelle -> no-op a la construction declaree. Comparer a -1 comme
         * Etat/CurseurSouris. */
        int cadH = prop_int(ctrl->properties, ctrl->property_count,
                             "CadrageHorizontal", "HorizontalAlignment", -1);
        if (cadH != -1) owl_ui_control_set_h_align(widget, cadH);
        int cadV = prop_int(ctrl->properties, ctrl->property_count,
                             "CadrageVertical", "VerticalAlignment", -1);
        if (cadV != -1) owl_ui_control_set_v_align(widget, cadV);

        /* Lot Cadre Phase 4 : Cadre compose, applique a TOUT widget.
         *  lot 3 — source de verite unique = store de l'uici (seede
         * depuis le declaratif, y compris la cascade `LIB_X..Cadre..Epaisseur`
         * fusionnee au compile-time via CascadeBuffer ). On lit le
         * composite du store, on le relie au widget (watcher direct pour les
         * chained sets ulterieurs) et on l'applique a Qt. */
        Value cadreVal = NIL_VAL;
        if (uici) {
            (void)obj_ui_control_instance_headless_get(uici, "Cadre", &cadreVal);
        }
        if (IS_INSTANCE(cadreVal) && widget && uici) {
            inst_widget_watch_tag_recursive(AS_INSTANCE(cadreVal), widget);
            apply_cadre_instance_to_widget(widget, AS_INSTANCE(cadreVal));
        }

        /* Couleur / CouleurFond declarees (post-pass commune a tous les widgets,
         * pas seulement le Libelle). BGR int packe (R + V*256 + B*65536, canon
         * canon), -1 = defaut. Le garde `>= 0` est legitime : Couleur est un entier
         * positif reel, defaut -1. Applique apres le Style declare pour que la
         * couleur explicite override le style. */
        int couleur = prop_int(ctrl->properties, ctrl->property_count,
                                "Couleur", "Color", -1);
        if (couleur >= 0) owl_ui_control_set_text_color(widget, couleur);
        int couleurFond = prop_int(ctrl->properties, ctrl->property_count,
                                    "CouleurFond", "BackgroundColor", -1);
        if (couleurFond >= 0) owl_ui_control_set_bg_color(widget, couleurFond);

        /*  — Style aggrege declaratif (Style..X en PROPRIETES) applique
         * a la CONSTRUCTION, en calquant le Cadre juste au-dessus : l'instance
         * Style du store est la source de verite (seedee depuis le declaratif,
         * cascade fusionnee au compile-time). Sans ce bloc le Style ne peignait
         * qu'a l'assignation runtime `..Style = X` (real_ui_set_prop), jamais a
         * l'ouverture de la fenetre. apply_style_to_control porte deja les
         * gardes sentinelles (couleurs >= 0, marges > 0) : seuls les champs SET
         * sont pousses au widget, un Saisie sans Style n'est pas touche (pas
         * d'instance Style dans son store). */
        Value styleVal = NIL_VAL;
        if (uici) {
            (void)obj_ui_control_instance_headless_get(uici, "Style", &styleVal);
        }
        if (IS_INSTANCE(styleVal) && widget && uici) {
            inst_widget_watch_tag_recursive(AS_INSTANCE(styleVal), widget);
            style_root_record(widget, AS_INSTANCE(styleVal));   /*  */
            apply_style_to_control(widget, AS_INSTANCE(styleVal),
                                    (OwlControlKind)ctrl->kind);
        }

        /*  — Champ Table : props plates EtatInitial + HauteurLigne, et
         * override de style par colonne (styleColonne). Applique APRES le
         * styleTable global (ci-dessus) pour composer le fallback. */
        if (ctrl->kind == OWL_CONTROL_TABLE && widget) {
            /* EtatInitial (enum etatsTableInitial). Mappe la sentinelle vers le
             * code runtime 0=EnSaisie 1=Grisé 2=Sélection 3=NiSaisieNiSélection.
             * Grisé reutilise OWL_ETAT_GRISE (base enum `etat`). */
            int etatInit = prop_int(ctrl->properties, ctrl->property_count,
                                     "EtatInitial", "InitialState", -1);
            if (etatInit != -1) {
                int code = 0;
                if      (etatInit == OWL_ETAT_TABLE_EN_SAISIE) code = 0;
                else if (etatInit == OWL_ETAT_GRISE)           code = 1;
                else if (etatInit == OWL_ETAT_TABLE_SELECTION) code = 2;
                else if (etatInit == OWL_ETAT_TABLE_NI_NI)     code = 3;
                owl_ui_table_set_etat_initial(widget, code);
            }

            /* HauteurLigne (px, > 0). */
            int rowH = prop_int(ctrl->properties, ctrl->property_count,
                                 "HauteurLigne", "RowHeight", -1);
            if (rowH > 0) owl_ui_table_set_row_height(widget, rowH);

            /* HauteurTitre : 0 masque l'en-tete, > 0 impose la hauteur, < 0
             * laisse la hauteur naturelle. Sentinelle d'absence distincte de
             * -1, qui est une valeur signifiante ici. */
            const int TITLE_H_ABSENT = -0x40000000;
            int titleH = prop_int(ctrl->properties, ctrl->property_count,
                                   "HauteurTitre", "TitleHeight",
                                   TITLE_H_ABSENT);
            if (titleH != TITLE_H_ABSENT)
                owl_ui_table_set_title_height(widget, titleH);

            /* ColonneExtensible : nom d'une colonne qui s'etire pour occuper
             * l'espace restant. On resout le nom vers l'index de bloc colonne
             * (1-based cote widget). Nom inconnu = no-op (defaut conserve). */
            const char* stretchName = prop_str(ctrl->properties,
                                                ctrl->property_count,
                                                "ColonneExtensible",
                                                "StretchColumn", NULL);
            if (stretchName && *stretchName) {
                for (int ci = 0; ci < ctrl->column_count; ci++) {
                    if (ctrl->columns[ci].name
                     && strcasecmp(ctrl->columns[ci].name, stretchName) == 0) {
                        owl_ui_table_set_stretch_column(widget, ci + 1);
                        break;
                    }
                }
            }

            /* Override par colonne : la prop Style de chaque colonne est un eval
             * (styleColonne) resolu ici. Le styleTable global sert de fallback
             * pour composer les feuilles non posees sur la colonne. */
            OwlTableStyleSpec tableSpec;
            bool hasTableSpec = false;
            OwlCadreSpec tableExtBorder = {0};
            const char* tblFams[4] = { NULL, NULL, NULL, NULL };
            if (IS_INSTANCE(styleVal)) {
                read_table_style(AS_INSTANCE(styleVal), &tableSpec,
                                 &tableExtBorder, tblFams);
                hasTableSpec = true;
            }
            for (int ci = 0; ci < ctrl->column_count; ci++) {
                OwlColumnDecl* col = &ctrl->columns[ci];
                invalidate_dynamic_props_cache(col->properties, col->property_count);
                resolve_dynamic_props(col->properties, col->property_count);
                const OwlPropertyInit* sp = find_prop(col->properties,
                                                       col->property_count, "Style");
                if (!sp || !sp->is_constant || !IS_INSTANCE(sp->const_value))
                    continue;
                OwlColumnStyleSpec colSpec;
                const char* colFams[4] = { NULL, NULL, NULL, NULL };
                read_column_style(AS_INSTANCE(sp->const_value), &colSpec, colFams);
                owl_ui_table_set_column_style(widget, ci, &colSpec,
                                              hasTableSpec ? &tableSpec : NULL);
            }
        }

        /* Etat unifie : ecrase si specifie, sinon laisse Visible/Grise.
         * Defaut -1 (non specifie) ; valeurs sentinels OWL_ETAT_*. */
        int etat = prop_int(ctrl->properties, ctrl->property_count,
                             "Etat", "State", -1);
        if (etat != -1) owl_ui_control_set_etat(widget, etat);

        /* Opacite (commun) : 0..100, defaut 100 (no-op). */
        int opacite = prop_int(ctrl->properties, ctrl->property_count,
                                "Opacite", "Opacity", -1);
        if (opacite >= 0) owl_ui_control_set_opacite(widget, opacite);

        /* ---------- Lot A — props canon Libelle (doc 1013002) ----------
         * Setters communs applicables a tout QWidget. Declares dans
         * PROPS_LABEL pour V1 ; utilisable sur Bouton/Saisie meme si non
         * declare la-bas (V2 propagera la declaration). */

        /* AffichageActif — defaut Vrai (rafraichissement on). */
        if (!prop_bool(ctrl->properties, ctrl->property_count,
                        "AffichageActif", "DisplayEnabled", true)) {
            owl_ui_control_set_display_active(widget, 0);
        }

        /* Altitude — z-order intra-fenetre, defaut 0. */
        int altitude = prop_int(ctrl->properties, ctrl->property_count,
                                 "Altitude", "Altitude", -1);
        if (altitude >= 0) owl_ui_control_set_altitude(widget, altitude);

        /* CurseurSouris — defaut -1 (non specifie) ; sentinels OWL_CUR_*. */
        int curseur = prop_int(ctrl->properties, ctrl->property_count,
                                "CurseurSouris", "MouseCursor", -1);
        if (curseur != -1) owl_ui_control_set_cursor_shape(widget, curseur);

        /* Bornes redim. -1 = non specifie (laisse Qt par defaut). */
        int hauteurMin = prop_int(ctrl->properties, ctrl->property_count,
                                    "HauteurMin", "MinHeight", -1);
        if (hauteurMin > 0) owl_ui_control_set_min_height(widget, hauteurMin);
        int hauteurMax = prop_int(ctrl->properties, ctrl->property_count,
                                    "HauteurMax", "MaxHeight", -1);
        if (hauteurMax > 0) owl_ui_control_set_max_height(widget, hauteurMax);
        int largeurMin = prop_int(ctrl->properties, ctrl->property_count,
                                    "LargeurMin", "MinWidth", -1);
        if (largeurMin > 0) owl_ui_control_set_min_width(widget, largeurMin);
        int largeurMax = prop_int(ctrl->properties, ctrl->property_count,
                                    "LargeurMax", "MaxWidth", -1);
        if (largeurMax > 0) owl_ui_control_set_max_width(widget, largeurMax);

        /* Message — texte d'aide barre de statut. */
        const char* message = prop_str(ctrl->properties, ctrl->property_count,
                                        "Message", "StatusTip", NULL);
        if (message) owl_ui_control_set_status_tip(widget, message);

        /* SensEcriture — defaut -1 (non specifie) ; sentinels OWL_SE_*. */
        int sensEcr = prop_int(ctrl->properties, ctrl->property_count,
                                "SensEcriture", "TextDirection", -1);
        if (sensEcr != -1) owl_ui_control_set_text_direction(widget, sensEcr);

        /* PoliceEtirement — etirement horizontal unique (defaut etirNormal).
         * Sentinel OWL_ETIR_* -> valeur de stretch du moteur de rendu. */
        int etirement = prop_int(ctrl->properties, ctrl->property_count,
                                  "PoliceEtirement", "FontStretch", -1);
        if (etirement != -1)
            owl_ui_control_set_font_stretch(widget,
                owl_etirement_to_stretch(etirement));

        /* PoliceTailleMin — applique apres les autres props font/text afin
         * que le fit calcule sur la taille effective. */
        int policeMin = prop_int(ctrl->properties, ctrl->property_count,
                                  "PoliceTailleMin", "FontMinSize", -1);
        if (policeMin > 0) owl_ui_control_set_font_min_size(widget, policeMin);

        /* ---------- Lot C — marges interieures (canon/...). */
        int retraitG = prop_int(ctrl->properties, ctrl->property_count,
                                 "RetraitGauche", "LeftMargin", -1);
        if (retraitG >= 0) owl_ui_control_set_margin_left(widget, retraitG);
        int retraitD = prop_int(ctrl->properties, ctrl->property_count,
                                 "RetraitDroit", "RightMargin", -1);
        if (retraitD >= 0) owl_ui_control_set_margin_right(widget, retraitD);
        int retraitH = prop_int(ctrl->properties, ctrl->property_count,
                                 "RetraitHaut", "TopMargin", -1);
        if (retraitH >= 0) owl_ui_control_set_margin_top(widget, retraitH);
        int retraitB = prop_int(ctrl->properties, ctrl->property_count,
                                 "RetraitBas", "BottomMargin", -1);
        if (retraitB >= 0) owl_ui_control_set_margin_bottom(widget, retraitB);

        /*  — Modele C : le widget Qt frais a deja ete
         * rattache a son ObjUiControlInstance en tete de ce bloc (
         * lot 3), pour que les props composees alimentent le store puis Qt.
         * vm.globals[ctrl->name] reste l'ObjUiControlRef pose au compile-time
         * (cf compiler.c::window_parse_subfield) — la resolution dynamique
         * via OP_STRUCT_GET trouvera l'instance via la stack. */
        /* ZR : tag objectName + kind sur le QWidget pour permettre le
         * lookup ZR[i].sub via owl_ui_control_find_child. */
        owl_ui_control_set_object_name(widget, ctrl->name);
        owl_ui_control_set_widget_kind(widget, (int)ctrl->kind);
    }
    return widget;
}

/* Helper : retourne true si prop matche un alias de la prop par defaut
 * (Valeur). Aligne le pattern canon ou 'ctrl' tout court = ctrl..Valeur. */
static bool prop_is_default(const char* prop) {
    return strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0;
}

/*  — GET Valeur typee d'un champ de saisie (Numerique / Monetaire).
 *
 * Contrat  (Qt-first read path) : RESPECTE. Le widget monte FAIT FOI :
 * on lit le TEXTE VIVANT du QLineEdit et on le parse, jamais un store parallele.
 * Une frappe utilisateur (QKeyEvent -> textChanged) est donc toujours refletee ;
 * pas de valeur figee au dernier SET programmatique. L'affichage MVP est plat
 * ("42.25", "1234.56"), donc le reparse est lossless et le round-trip
 * programmatique (set 0m1234.56 -> get 0m1234.56) reste exact.
 *
 * Ce helper n'est atteint que par real_ui_get_prop, qui garantit un widget monte
 * (guard c->widget != NULL ; et try_live_qt_prop_for_uici, vm.c, ne le route
 * qu'avec uici->widget != NULL) -> w est toujours non-NULL. En pur headless
 * (widget absent) le GET ..Valeur ne passe pas ici : fallback store/default
 * cote vm.c (headless_default_with_alias). Aucun store typee n'est necessaire.
 *
 * Numerique -> owlReel (strtod sur le texte live).
 * Monetaire -> owlMonetaire (VAL_CURRENCY) par parse decimal EXACT : le texte
 *   passe par numeric_from_string (mpdecimal) puis numeric_to_currency_i128
 *   (x10^6, arrondi demi-pair). PAS via double : strtod(t)*1e6 introduirait un
 *   arrondi binaire ("99.99" -> 99.98999...), la conversion mpdecimal donne
 *   exactement 99990000. */
/*  — masque d'affichage (MasqueAffichage/DisplayMask) posé sur un champ
 * de saisie, lu depuis le store d'instance (source de verite unique GUI/headless).
 * Retourne NULL si aucun masque non vide n'est posé. La chaine renvoyee reste
 * rootee par le store (uici dans window_stack) tant que le champ existe -> pas
 * d'alloc entre-temps a proteger. */
/*  — format de date par defaut du projet (champ FormatDate du bloc
 * `est un Projet`). Declare a la main plutot que d'inclure compiler.h : ce
 * dernier tire scanner.h dont le TokenType collide avec celui de windows.h
 * (meme raison que stdlib_parametre.c pour compiler_projet_ident). */
extern const char* compiler_projet_format_date(void);
/*  — format d'heure par defaut du projet (miroir FormatDate). */
extern const char* compiler_projet_format_heure(void);
/*  — format de duree par defaut du projet (miroir FormatHeure). */
extern const char* compiler_projet_format_duree(void);

static const char* input_date_mask_for(ObjUiControl* c) {
    if (c == NULL || c->name == NULL) return NULL;
    ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
    if (uici != NULL) {
        Value mv;
        if (obj_ui_control_instance_headless_get(uici, "MasqueAffichage", &mv)
            && IS_STRING(mv)) {
            ObjString* s = AS_STRING(mv);
            if (s->length > 0) return s->chars;
        }
    }
    /* Pas de masque sur le champ : repli sur le format de date par defaut du
     * projet. NULL si aucun projet compile / champ absent -> le
     * dernier repli JJ/MM/AAAA en dur (input_typed_get/set) prend le relais. */
    const char* projFmt = compiler_projet_format_date();
    return (projFmt != NULL && projFmt[0] != '\0') ? projFmt : NULL;
}

/*  — masque d'affichage HEURE (typSaisieHeure). Comme input_date_mask_for :
 * MasqueAffichage du champ (priorite 1), sinon repli sur le FormatHeure projet
 *, sinon NULL -> input_typed_get/set applique le defaut HH:MI. */
static const char* input_time_mask_for(ObjUiControl* c) {
    if (c == NULL || c->name == NULL) return NULL;
    ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
    if (uici != NULL) {
        Value mv;
        if (obj_ui_control_instance_headless_get(uici, "MasqueAffichage", &mv)
            && IS_STRING(mv)) {
            ObjString* s = AS_STRING(mv);
            if (s->length > 0) return s->chars;
        }
    }
    /* Pas de masque sur le champ : repli sur le format d'heure par defaut du
     * projet. NULL si aucun projet compile / champ absent -> le
     * dernier repli HH:MI en dur (input_typed_get/set) prend le relais. */
    const char* projFmt = compiler_projet_format_heure();
    return (projFmt != NULL && projFmt[0] != '\0') ? projFmt : NULL;
}

/*  — masque d'affichage DURÉE (typSaisieDurée). Comme input_time_mask_for :
 * MasqueAffichage du champ (priorite 1), sinon repli sur le FormatDurée projet
 *, sinon NULL -> input_typed_get/set applique le defaut HH:MI:SS. */
static const char* input_duree_mask_for(ObjUiControl* c) {
    if (c == NULL || c->name == NULL) return NULL;
    ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
    if (uici != NULL) {
        Value mv;
        if (obj_ui_control_instance_headless_get(uici, "MasqueAffichage", &mv)
            && IS_STRING(mv)) {
            ObjString* s = AS_STRING(mv);
            if (s->length > 0) return s->chars;
        }
    }
    /* Pas de masque sur le champ : repli sur le format de duree par defaut du
     * projet. NULL si aucun projet compile / champ absent -> le
     * dernier repli HH:MI:SS en dur (input_typed_get/set) prend le relais. */
    const char* projFmt = compiler_projet_format_duree();
    return (projFmt != NULL && projFmt[0] != '\0') ? projFmt : NULL;
}

/*  — dérive un masque de FRAPPE GUIDÉE depuis un format à jetons DURÉE.
 * Vocabulaire durée (minutes = MI/mm, jamais MM) : JJ/HH/MI/SS/CC -> "99" (2 chiffres), CCC ->
 * "999", séparateurs recopiés. Un format VARIABLE (J seul largeur variable, ou
 * notation +N) rend le guidage chiffre impossible -> renvoie false et out=""
 * (le caller ne pose PAS de masque : saisie libre, comme les jetons nommés date).
 * Renvoie true si un masque de guidage non vide a été produit. */
static bool input_duree_frappe_mask_from_format(const char* fmt, char* out, size_t outsz) {
    if (out == NULL || outsz == 0) return false;
    out[0] = '\0';
    if (fmt == NULL) return false;
    int flen = (int)strlen(fmt);
    size_t oi = 0;
    int i = 0;
    while (i < flen && oi + 1 < outsz) {
        int rem = flen - i;
        if (fmt[i] == '+') { out[0] = '\0'; return false; } /* +N -> libre */
        int emit;
        if (rem >= 3 && strncmp(fmt + i, "CCC", 3) == 0) {
            emit = 3; i += 3;
        } else if (rem >= 2 && (strncmp(fmt + i, "CC", 2) == 0 ||
                                strncmp(fmt + i, "JJ", 2) == 0 ||
                                strncmp(fmt + i, "HH", 2) == 0 ||
                                strncmp(fmt + i, "MI", 2) == 0 ||
                                strncmp(fmt + i, "mm", 2) == 0 ||
                                strncmp(fmt + i, "SS", 2) == 0)) {
            emit = 2; i += 2;
        } else if (fmt[i] == 'J') {
            out[0] = '\0'; return false; /* J seul (variable) -> libre */
        } else {
            out[oi++] = fmt[i++]; /* séparateur littéral */
            continue;
        }
        for (int k = 0; k < emit && oi + 1 < outsz; k++) out[oi++] = '9';
    }
    out[oi] = '\0';
    return oi > 0;
}

/*  — dérive un masque de SAISIE (frappe guidée) depuis un format à
 * jetons date/heure. Écrit dans `out` un masque à jetons '9' + séparateurs
 * littéraux, grammaire attendue par owl_ui_lineedit_set_mask (qui fera ensuite
 * l'échappement Qt et le setInputMask). Correspondance alignée sur le moteur de
 * format (format_date_time_tokens) : AAAA/YYYY→9999, AA/MM/JJ/DD/HH/mm/MI/SS/CC→99,
 * séparateurs recopiés tels quels. Un jeton NOMMÉ (mois/jour en lettres :
 * MMMM/MMM/JJJJ/JJJ) rend le guidage chiffre impossible -> renvoie false et
 * out="" : le caller ne pose PAS de masque (saisie libre). Renvoie true si un
 * masque de guidage non vide a été produit. */
static bool input_frappe_mask_from_format(const char* fmt, char* out, size_t outsz) {
    if (out == NULL || outsz == 0) return false;
    out[0] = '\0';
    if (fmt == NULL) return false;
    int flen = (int)strlen(fmt);
    size_t oi = 0;
    int i = 0;
    while (i < flen && oi + 1 < outsz) {
        int rem = flen - i;
        /* Jetons NOMMÉS (texte) -> aucun guidage chiffre possible. */
        if ((rem >= 4 && strncasecmp(fmt + i, "MMMM", 4) == 0) ||
            (rem >= 4 && strncasecmp(fmt + i, "JJJJ", 4) == 0) ||
            (rem >= 3 && strncasecmp(fmt + i, "MMM", 3) == 0) ||
            (rem >= 3 && strncasecmp(fmt + i, "JJJ", 3) == 0)) {
            out[0] = '\0';
            return false;
        }
        int emit;
        if (rem >= 4 && (strncmp(fmt + i, "AAAA", 4) == 0 ||
                         strncmp(fmt + i, "YYYY", 4) == 0)) {
            emit = 4;
        } else if (rem >= 2 && (strncmp(fmt + i, "AA", 2) == 0 ||
                                strncmp(fmt + i, "MM", 2) == 0 ||
                                strncmp(fmt + i, "JJ", 2) == 0 ||
                                strncmp(fmt + i, "DD", 2) == 0 ||
                                strncmp(fmt + i, "HH", 2) == 0 ||
                                strncmp(fmt + i, "mm", 2) == 0 ||
                                strncmp(fmt + i, "MI", 2) == 0 ||
                                strncmp(fmt + i, "SS", 2) == 0 ||
                                strncmp(fmt + i, "CC", 2) == 0)) {
            emit = 2;
        } else {
            out[oi++] = fmt[i++]; /* séparateur littéral */
            continue;
        }
        for (int k = 0; k < emit && oi + 1 < outsz; k++) out[oi++] = '9';
        i += emit;
    }
    out[oi] = '\0';
    return oi > 0;
}

/*  — (re)pose le masque de frappe guidée d'un champ Date/Heure selon
 * son FORMAT EFFECTIF (MasqueAffichage posé > projet (date) > défaut). Un format
 * à jetons nommés -> masque effacé (saisie libre). No-op sur les autres types. */
static void input_apply_frappe_mask(ObjUiControl* c, OwlControl* w, int inputType) {
    if (w == NULL) return;
    const char* fmt;
    if (inputType == OWL_TYPESAISIE_DUREE) {
        /*  — moteur de masque DURÉE séparé (minutes = MI/mm, HH non plafonné
         * à 23) : dérivation de masque + validateur dédiés. */
        fmt = input_duree_mask_for(c);
        if (fmt == NULL || fmt[0] == '\0') fmt = "HH:MI:SS";
        char qtmaskd[128];
        if (input_duree_frappe_mask_from_format(fmt, qtmaskd, sizeof(qtmaskd)))
            owl_ui_lineedit_set_mask(w, qtmaskd);
        else
            owl_ui_lineedit_set_mask(w, ""); /* format variable -> saisie libre */
        owl_ui_lineedit_set_duree_validator(w, fmt);
        return;
    }
    if (inputType == OWL_TYPESAISIE_DATE) {
        fmt = input_date_mask_for(c);
        if (fmt == NULL || fmt[0] == '\0') fmt = "JJ/MM/AAAA";
    } else if (inputType == OWL_TYPESAISIE_HEURE) {
        fmt = input_time_mask_for(c);
        if (fmt == NULL || fmt[0] == '\0') fmt = "HH:MI";
    } else {
        return;
    }
    char qtmask[128];
    if (input_frappe_mask_from_format(fmt, qtmask, sizeof(qtmask)))
        owl_ui_lineedit_set_mask(w, qtmask);
    else
        owl_ui_lineedit_set_mask(w, ""); /* jetons nommés -> saisie libre */
    /*  — (re)pose le validateur de plages au même point que le masque
     * (borne les VALEURS à la frappe ; jetons nommés -> retiré). */
    owl_ui_lineedit_set_date_time_validator(w, fmt);
}

/*  — accessor projet (compiler.h inclus tard dans ce fichier). */
extern bool compiler_projet_format_nombre(int* chiffres,
                                          char* sepDec, int sepDecCap,
                                          char* sepMil, int sepMilCap);

/*  — config effective de FormatNombre applique a un Champ Saisie
 * numerique. chiffres == OWL_FNB_AUTO => affichage plat (pas d'arrondi). */
typedef struct {
    int  chiffres;      /* ChiffresApresVirgule, OWL_FNB_AUTO = auto/court */
    char sepDec[8];     /* separateur decimal, "" = point plat */
    char sepMil[8];     /* separateur de milliers, "" = aucun */
} FnbConfig;

/*  — controle de coherence des separateurs FormatNombre. Retourne le
 * separateur fautif si la config est invalide, NULL sinon. Rejets : (1) un
 * separateur contient un chiffre ; (2) milliers == decimal, les deux non vides.
 * Meme logique que fnb_sep_conflict cote compiler.c (chemin projet compile-time)
 * — dupliquee car TU separes, la faille tient en dix lignes. */
static const char* fnb_sep_conflict_rt(const char* sepMil, const char* sepDec) {
    for (const char* s = sepMil; s != NULL && *s != '\0'; s++)
        if (*s >= '0' && *s <= '9') return sepMil;
    for (const char* s = sepDec; s != NULL && *s != '\0'; s++)
        if (*s >= '0' && *s <= '9') return sepDec;
    if (sepMil != NULL && sepDec != NULL && sepMil[0] != '\0' && sepDec[0] != '\0'
        && strcmp(sepMil, sepDec) == 0)
        return sepMil;
    return NULL;
}

/* Extrait la config depuis une instance FormatNombre (cles canon ASCII). */
static void fnb_config_from_instance(ObjInstance* fnb, FnbConfig* out) {
    out->chiffres = OWL_FNB_AUTO;
    out->sepDec[0] = '\0';
    out->sepMil[0] = '\0';
    if (fnb == NULL) return;
    Value v;
    ObjString* k = table_find_key_case_insensitive(&fnb->fields, "ChiffresApresVirgule");
    if (k != NULL && table_get(&fnb->fields, k, &v) && IS_INTEGER(v))
        out->chiffres = (int)AS_INTEGER(v);
    k = table_find_key_case_insensitive(&fnb->fields, "SeparateurDecimal");
    if (k != NULL && table_get(&fnb->fields, k, &v) && IS_STRING(v))
        snprintf(out->sepDec, sizeof(out->sepDec), "%s", AS_CSTRING(v));
    k = table_find_key_case_insensitive(&fnb->fields, "SeparateurMilliers");
    if (k != NULL && table_get(&fnb->fields, k, &v) && IS_STRING(v))
        snprintf(out->sepMil, sizeof(out->sepMil), "%s", AS_CSTRING(v));

    /*  — config contradictoire (milliers == decimal, ou separateur =
     * chiffre) : erreur claire (soft, pose ErreurDetectee) plutot que des zeros
     * parasites silencieux. Point de convergence unique : champ Saisie inline ET
     * affectation dynamique `f..SeparateurMilliers = "."`. */
    const char* badSep = fnb_sep_conflict_rt(out->sepMil, out->sepDec);
    if (badSep != NULL)
        owl_emit_native_error(E_SEM_FORMAT_NOMBRE_SEP_INVALIDE, "FormatNombre",
                              "sep", badSep, NULL);
}

/* Instance FormatNombre EFFECTIVE d'un Champ Saisie (par nom) : celle du champ
 * (store) si posee, NULL sinon (le caller retombe sur le projet / le defaut
 * plat). */
static ObjInstance* input_fnb_instance_by_name(const char* name) {
    if (name == NULL) return NULL;
    ObjUiControlInstance* uici = current_window_uici_by_name(name);
    if (uici == NULL) return NULL;
    Value fv;
    if (obj_ui_control_instance_headless_get(uici, "FormatNombre", &fv)
        && IS_INSTANCE(fv))
        return AS_INSTANCE(fv);
    return NULL;
}

static ObjInstance* input_fnb_instance_for(ObjUiControl* c) {
    return (c != NULL) ? input_fnb_instance_by_name(c->name) : NULL;
}

/* Config effective (par nom) : FormatNombre du champ > FormatNombre du projet >
 * plat. Renvoie true si un format a ete resolu (champ ou projet), false = plat. */
static bool input_num_format_by_name(const char* name, FnbConfig* out) {
    ObjInstance* fnb = input_fnb_instance_by_name(name);
    if (fnb != NULL) {
        fnb_config_from_instance(fnb, out);
        return true;
    }
    int chiffres = OWL_FNB_AUTO;
    char sepDec[8] = "", sepMil[8] = "";
    if (compiler_projet_format_nombre(&chiffres, sepDec, (int)sizeof(sepDec),
                                      sepMil, (int)sizeof(sepMil))) {
        out->chiffres = chiffres;
        snprintf(out->sepDec, sizeof(out->sepDec), "%s", sepDec);
        snprintf(out->sepMil, sizeof(out->sepMil), "%s", sepMil);
        return true;
    }
    return false;
}

static bool input_num_format_for(ObjUiControl* c, FnbConfig* out) {
    return input_num_format_by_name(c != NULL ? c->name : NULL, out);
}

/* Un format est "actif" (arrondi + separateurs) ssi un nombre fixe de decimales
 * est pose. fnbAuto => forme plate courte (non-regression stricte). */
static bool fnb_active(const FnbConfig* f) {
    return f != NULL && f->chiffres != OWL_FNB_AUTO;
}

/* Formate un reel selon un FormatNombre actif : arrondi a N decimales,
 * separateur de milliers groupe par 3, separateur decimal (defaut "."). */
static void fnb_format_double(double d, const FnbConfig* f, char* out, size_t outsz) {
    if (out == NULL || outsz == 0) return;
    int nd = f->chiffres;
    if (nd < 0) nd = 0;
    if (nd > 30) nd = 30;
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%.*f", nd, d);
    /* tmp = [-]int[.frac], separateur decimal C = '.'. */
    const char* p = tmp;
    char sign = '\0';
    if (*p == '-' || *p == '+') { sign = *p; p++; }
    const char* dot = strchr(p, '.');
    int intLen = dot ? (int)(dot - p) : (int)strlen(p);
    const char* frac = dot ? dot + 1 : NULL;

    char grouped[96];
    int gi = 0;
    const char* sepMil = (f->sepMil[0] != '\0') ? f->sepMil : NULL;
    for (int i = 0; i < intLen && gi < (int)sizeof(grouped) - 12; i++) {
        if (sepMil != NULL && i > 0 && ((intLen - i) % 3) == 0) {
            for (const char* s = sepMil; *s && gi < (int)sizeof(grouped) - 1; s++)
                grouped[gi++] = *s;
        }
        grouped[gi++] = p[i];
    }
    grouped[gi] = '\0';

    const char* decSep = (f->sepDec[0] != '\0') ? f->sepDec : ".";
    if (nd > 0 && frac != NULL)
        snprintf(out, outsz, "%s%s%s%s", sign ? "-" : "", grouped, decSep, frac);
    else
        snprintf(out, outsz, "%s%s", sign ? "-" : "", grouped);
}

/* De-formate un texte affiche selon un FormatNombre actif : retire le
 * separateur de milliers, normalise le separateur decimal vers '.', puis
 * strtod. Coeur du round-trip Qt-first. */
static double fnb_deformat(const char* text, const FnbConfig* f) {
    if (text == NULL) return 0.0;
    char buf[128];
    size_t bi = 0;
    size_t milLen = (f->sepMil[0] != '\0') ? strlen(f->sepMil) : 0;
    size_t decLen = (f->sepDec[0] != '\0') ? strlen(f->sepDec) : 0;
    for (size_t i = 0; text[i] != '\0' && bi < sizeof(buf) - 1; ) {
        if (milLen > 0 && strncmp(text + i, f->sepMil, milLen) == 0) {
            i += milLen;  /* retire le separateur de milliers */
            continue;
        }
        if (decLen > 0 && strncmp(text + i, f->sepDec, decLen) == 0) {
            buf[bi++] = '.';   /* normalise le separateur decimal */
            i += decLen;
            continue;
        }
        buf[bi++] = text[i++];
    }
    buf[bi] = '\0';
    char* end;
    double x = strtod(buf, &end);
    return (end != buf) ? x : 0.0;
}

/* ============================================================
 *  — FormatMonétaire : config effective + format/deformat monetaire.
 * Miroir de la machinerie FormatNombre, avec le SYMBOLE en plus et un
 * traitement EXACT (VAL_CURRENCY, ) : le formatage part de la chaine
 * decimale exacte (currency_to_str), jamais d'un double.
 * ============================================================ */
extern bool compiler_projet_format_monetaire(char* symbole, int symboleCap,
                                             int* position,
                                             char* sepDec, int sepDecCap,
                                             char* sepMil, int sepMilCap,
                                             int* chiffres,
                                             int* negatif);

typedef struct {
    char symbole[16];   /* symbole monetaire, "" = aucun */
    int  position;      /* OWL_POSSYM_AVANT / OWL_POSSYM_APRES */
    char sepDec[8];     /* separateur decimal, "" = point plat */
    char sepMil[8];     /* separateur de milliers, "" = aucun */
    int  chiffres;      /* ChiffresApresVirgule (defaut 2) */
    int  negatif;       /* OWL_FMN_MOINS / OWL_FMN_PARENTHESES */
} FmnConfig;

/*  — controle de coherence d'une config FormatMonetaire (runtime).
 * Etend fnb_sep_conflict_rt au symbole : separateur/symbole = chiffre, milliers
 * == decimal, symbole = separateur. Retourne l'element fautif, NULL sinon.
 * Meme logique que fmn_config_conflict cote compiler.c (dupliquee cross-binaire). */
static const char* fmn_config_conflict_rt(const char* symbole, const char* sepMil,
                                          const char* sepDec) {
    const char* badSep = fnb_sep_conflict_rt(sepMil, sepDec);
    if (badSep != NULL) return badSep;
    for (const char* s = symbole; s != NULL && *s != '\0'; s++)
        if (*s >= '0' && *s <= '9') return symbole;
    if (symbole != NULL && symbole[0] != '\0') {
        if (sepMil != NULL && sepMil[0] != '\0' && strcmp(symbole, sepMil) == 0)
            return symbole;
        if (sepDec != NULL && sepDec[0] != '\0' && strcmp(symbole, sepDec) == 0)
            return symbole;
    }
    return NULL;
}

/* Extrait la config depuis une instance FormatMonétaire (cles canon ASCII). */
static void fmn_config_from_instance(ObjInstance* fmn, FmnConfig* out) {
    out->symbole[0] = '\0';
    out->position   = OWL_POSSYM_APRES;
    out->sepDec[0]  = '\0';
    out->sepMil[0]  = '\0';
    out->chiffres   = 2;
    out->negatif    = OWL_FMN_MOINS;
    if (fmn == NULL) return;
    Value v;
    ObjString* k = table_find_key_case_insensitive(&fmn->fields, "Symbole");
    if (k != NULL && table_get(&fmn->fields, k, &v) && IS_STRING(v))
        snprintf(out->symbole, sizeof(out->symbole), "%s", AS_CSTRING(v));
    k = table_find_key_case_insensitive(&fmn->fields, "PositionSymbole");
    if (k != NULL && table_get(&fmn->fields, k, &v) && IS_INTEGER(v))
        out->position = (int)AS_INTEGER(v);
    k = table_find_key_case_insensitive(&fmn->fields, "SeparateurDecimal");
    if (k != NULL && table_get(&fmn->fields, k, &v) && IS_STRING(v))
        snprintf(out->sepDec, sizeof(out->sepDec), "%s", AS_CSTRING(v));
    k = table_find_key_case_insensitive(&fmn->fields, "SeparateurMilliers");
    if (k != NULL && table_get(&fmn->fields, k, &v) && IS_STRING(v))
        snprintf(out->sepMil, sizeof(out->sepMil), "%s", AS_CSTRING(v));
    k = table_find_key_case_insensitive(&fmn->fields, "ChiffresApresVirgule");
    if (k != NULL && table_get(&fmn->fields, k, &v) && IS_INTEGER(v))
        out->chiffres = (int)AS_INTEGER(v);
    k = table_find_key_case_insensitive(&fmn->fields, "FormatNegatif");
    if (k != NULL && table_get(&fmn->fields, k, &v) && IS_INTEGER(v))
        out->negatif = (int)AS_INTEGER(v);

    /*  — config absurde posee dynamiquement (`f..Symbole = "5"`,
     * `f..SeparateurMilliers = "."`, ...) : erreur claire (soft, pose
     * ErreurDetectee) plutot que des valeurs parasites silencieuses au
     * de-formatage. Point de convergence : champ inline ET affectation a chaud. */
    const char* badElem = fmn_config_conflict_rt(out->symbole, out->sepMil,
                                                out->sepDec);
    if (badElem != NULL)
        owl_emit_native_error(E_SEM_FORMAT_MONETAIRE_CONFIG_INVALIDE,
                              "FormatMon\xc3\xa9taire", "elem", badElem, NULL);
}

/* Instance FormatMonétaire EFFECTIVE d'un Champ Saisie (par nom) : celle du
 * champ (store) si posee, NULL sinon. */
static ObjInstance* input_fmn_instance_by_name(const char* name) {
    if (name == NULL) return NULL;
    ObjUiControlInstance* uici = current_window_uici_by_name(name);
    if (uici == NULL) return NULL;
    Value fv;
    if (obj_ui_control_instance_headless_get(uici, "FormatMon\xc3\xa9taire", &fv)
        && IS_INSTANCE(fv))
        return AS_INSTANCE(fv);
    return NULL;
}

static ObjInstance* input_fmn_instance_for(ObjUiControl* c) {
    return (c != NULL) ? input_fmn_instance_by_name(c->name) : NULL;
}

/* Config effective (par nom) : FormatMonétaire du champ > du projet > plat.
 * Renvoie true si un format a ete resolu (champ ou projet), false = plat. */
static bool input_mon_format_by_name(const char* name, FmnConfig* out) {
    ObjInstance* fmn = input_fmn_instance_by_name(name);
    if (fmn != NULL) {
        fmn_config_from_instance(fmn, out);
        return true;
    }
    char symbole[16] = "", sepDec[8] = "", sepMil[8] = "";
    int position = OWL_POSSYM_APRES, chiffres = 2, negatif = OWL_FMN_MOINS;
    if (compiler_projet_format_monetaire(symbole, (int)sizeof(symbole), &position,
                                         sepDec, (int)sizeof(sepDec),
                                         sepMil, (int)sizeof(sepMil),
                                         &chiffres, &negatif)) {
        snprintf(out->symbole, sizeof(out->symbole), "%s", symbole);
        out->position = position;
        snprintf(out->sepDec, sizeof(out->sepDec), "%s", sepDec);
        snprintf(out->sepMil, sizeof(out->sepMil), "%s", sepMil);
        out->chiffres = chiffres;
        out->negatif  = negatif;
        return true;
    }
    return false;
}

static bool input_mon_format_for(ObjUiControl* c, FmnConfig* out) {
    return input_mon_format_by_name(c != NULL ? c->name : NULL, out);
}

/* Formate un montant EXACT (VAL_CURRENCY) selon un FormatMonétaire : part de la
 * chaine decimale exacte (currency_to_str), groupe les milliers, ajuste les
 * decimales a N chiffres, insere le separateur decimal + le symbole a sa
 * position, applique le motif du negatif. AUCUN transit par double. */
static void fmn_format_currency(__int128 cents, const FmnConfig* f,
                                char* out, size_t outsz) {
    if (out == NULL || outsz == 0) return;
    char exact[64];
    currency_to_str(cents, exact, sizeof(exact));  /* "[-]int[.frac]" */
    const char* p = exact;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    const char* dot = strchr(p, '.');
    int intLen = dot ? (int)(dot - p) : (int)strlen(p);
    const char* frac = dot ? dot + 1 : "";
    int fracLen = (int)strlen(frac);

    /* Ajuste les decimales a N chiffres (padding '0' ; troncature si N < frac). */
    int nd = f->chiffres;
    if (nd < 0) nd = 0;
    if (nd > 30) nd = 30;
    char fracBuf[40];
    int fi = 0;
    for (int i = 0; i < nd && fi < (int)sizeof(fracBuf) - 1; i++)
        fracBuf[fi++] = (i < fracLen) ? frac[i] : '0';
    fracBuf[fi] = '\0';

    /* Groupe la partie entiere avec le separateur de milliers. */
    char grouped[64];
    int gi = 0;
    const char* sepMil = (f->sepMil[0] != '\0') ? f->sepMil : NULL;
    for (int i = 0; i < intLen && gi < (int)sizeof(grouped) - 12; i++) {
        if (sepMil != NULL && i > 0 && ((intLen - i) % 3) == 0) {
            for (const char* s = sepMil; *s && gi < (int)sizeof(grouped) - 1; s++)
                grouped[gi++] = *s;
        }
        grouped[gi++] = p[i];
    }
    grouped[gi] = '\0';

    const char* decSep = (f->sepDec[0] != '\0') ? f->sepDec : ".";
    char number[96];
    if (nd > 0)
        snprintf(number, sizeof(number), "%s%s%s", grouped, decSep, fracBuf);
    else
        snprintf(number, sizeof(number), "%s", grouped);

    /* Symbole a sa position (espace de separation si symbole non vide). */
    char body[128];
    if (f->symbole[0] != '\0') {
        if (f->position == OWL_POSSYM_AVANT)
            snprintf(body, sizeof(body), "%s%s", f->symbole, number);
        else
            snprintf(body, sizeof(body), "%s %s", number, f->symbole);
    } else {
        snprintf(body, sizeof(body), "%s", number);
    }

    /* Motif du negatif. */
    if (neg) {
        if (f->negatif == OWL_FMN_PARENTHESES)
            snprintf(out, outsz, "(%s)", body);
        else
            snprintf(out, outsz, "-%s", body);
    } else {
        snprintf(out, outsz, "%s", body);
    }
}

/*  — AFFICHAGE (hors edition) d'une cellule de colonne typee
 * Numerique/Monetaire : resout le format PROJET (pas d'override par colonne dans
 * ce lot ; catalogue Colonne = Type + MasqueSaisie seulement) et formate la
 * chaine brute stockee dans la cellule. Repli PLAT (out = raw inchange) si aucun
 * format n'est resolu au niveau projet. Reutilise strictement la machinerie de
 * la Saisie (fnb_format_double / fmn_format_currency), pas de 2e implementation.
 * Appele depuis le delegate colonne (rendu Qt) et la sonde de test
 * __DebugTableCelluleTexteAffiche. col_type = OWL_COL_TYPE_* ; le name NULL force
 * la resolution defaut/projet uniquement (aucun champ Saisie associe). */
void owl_ui_format_cell_typed(int col_type, const char* raw,
                              char* out, size_t outsz) {
    if (out == NULL || outsz == 0) return;
    if (raw == NULL) raw = "";
    if (col_type == OWL_COL_TYPE_NUMERIQUE) {
        FnbConfig cfg;
        if (input_num_format_by_name(NULL, &cfg) && fnb_active(&cfg)) {
            fnb_format_double(strtod(raw, NULL), &cfg, out, outsz);
            return;
        }
    } else if (col_type == OWL_COL_TYPE_MONETAIRE) {
        FmnConfig cfg;
        if (input_mon_format_by_name(NULL, &cfg)) {
            /* Chaine -> montant EXACT (mpdecimal), JAMAIS via double : le
             * ObjNumeric ephemere est consomme immediatement (pas d'alloc OWL
             * intercalaire, pas de root necessaire). */
            bool ov = false;
            __int128 cents =
                numeric_to_currency_i128(numeric_from_string(raw), &ov);
            fmn_format_currency(cents, &cfg, out, outsz);
            return;
        }
    } else if (col_type == OWL_COL_TYPE_DATE) {
        /*  — masque d'affichage resolu defaut/projet (pas d'override par
         * colonne dans ce lot ; miroir ). Parse tolerant de la chaine
         * brute : dirige par le masque projet d'abord, sinon multi-format
         * generique (ISO / AAAAMMJJ / JJ-MM-AAAA...) comme input_typed_get. On
         * travaille en (y,m,d) purs C -> aucune alloc OWL, pas de rooting
         *. Non parseable -> repli plat (raw inchange). */
        const char* proj = compiler_projet_format_date();
        const char* mask = (proj != NULL && proj[0] != '\0') ? proj : "JJ/MM/AAAA";
        int y = 0, m = 0, d = 0, err = 0;
        int rlen = (int)strlen(raw), mlen = (int)strlen(mask);
        bool ok = owl_date_parse_with_mask(raw, rlen, mask, mlen, &y, &m, &d);
        if (!ok) ok = owl_parse_date_multi(raw, rlen, &y, &m, &d, &err);
        if (ok) {
            owl_date_format_with_mask(out, (int)outsz, mask, mlen, y, m, d);
            return;
        }
    } else if (col_type == OWL_COL_TYPE_HEURE) {
        /*  — miroir Date : masque HEURE resolu defaut/projet, parse
         * dirige par masque sinon repli generique (owl_parse_time_string_cs :
         * HHMMSS[CC] / HH:MM[:SS...]) comme input_typed_get. (hh,mi,sec,cs) purs
         * C -> pas de rooting. Non parseable -> repli plat. */
        const char* proj = compiler_projet_format_heure();
        const char* mask = (proj != NULL && proj[0] != '\0') ? proj : "HH:MI";
        int hh = 0, mi = 0, sec = 0, cs = 0;
        int rlen = (int)strlen(raw), mlen = (int)strlen(mask);
        bool ok = owl_time_parse_with_mask(raw, rlen, mask, mlen, &hh, &mi, &sec, &cs);
        if (!ok) ok = owl_parse_time_string_cs(raw, rlen, &hh, &mi, &sec, &cs);
        if (ok) {
            owl_time_format_with_mask(out, (int)outsz, mask, mlen, hh, mi, sec, cs);
            return;
        }
    } else if (col_type == OWL_COL_TYPE_DUREE) {
        /*  — miroir Date : masque DUREE resolu defaut/projet, parse
         * dirige par masque sinon repli sur le litteral signe canonique
         * +JHHMMSSCC (owl_parse_duration_multi,  ; deja reutilise au
         * set-path Saisie). totalMs pur C -> pas de rooting. Non parseable ->
         * repli plat. */
        const char* proj = compiler_projet_format_duree();
        const char* mask = (proj != NULL && proj[0] != '\0') ? proj : "HH:MI:SS";
        int64_t ms = 0;
        int derr = 0, rlen = (int)strlen(raw), mlen = (int)strlen(mask);
        bool ok = owl_duree_parse_with_mask(raw, rlen, mask, mlen, &ms);
        if (!ok) ok = owl_parse_duration_multi(raw, rlen, &ms, &derr);
        if (ok) {
            owl_duree_format_with_mask(out, (int)outsz, mask, mlen, ms);
            return;
        }
    } else if (col_type == OWL_COL_TYPE_MOTDEPASSE) {
        /* Masquage a l'AFFICHAGE : une puce U+25CF (BLACK CIRCLE, octets
         * e2 97 8f) par CARACTERE (codepoint) de la valeur brute. La valeur
         * reelle reste stockee dans le modele (editable en echo masque). Glyphe
         * FIXE (rendu deterministe cross-platform, pilier OWL #2) : U+25CF est
         * le defaut password de Qt (qplatformtheme) et l'echo reel de l'editeur
         * sur Windows/Linux ; macOS met U+2022 cote editeur (qcocoatheme) mais
         * l'affichage cellule, peint par OWL, reste U+25CF partout. Valeur vide
         * -> chaine vide. Compte les codepoints UTF-8 (continuation 0x80..0xBF
         * ignores). */
        static const char BULLET[3] = { '\xe2', '\x97', '\x8f' };
        size_t oi = 0;
        for (size_t i = 0; raw[i] != '\0'; i++) {
            if (((unsigned char)raw[i] & 0xC0) == 0x80) continue; /* continuation */
            if (oi + 3 >= outsz) break; /* garde la place du '\0' final */
            out[oi++] = BULLET[0];
            out[oi++] = BULLET[1];
            out[oi++] = BULLET[2];
        }
        out[oi] = '\0';
        return;
    }
    /* Repli plat : chaine brute inchangee (aucun symbole/separateur impose). */
    snprintf(out, outsz, "%s", raw);
}

/* De-formate un texte monetaire affiche vers la chaine decimale EXACTE : retire
 * le symbole, les separateurs de milliers, les parentheses/signe, normalise le
 * separateur decimal vers '.'. Le resultat est reparse par numeric_from_string
 * (mpdecimal) -> currency EXACT, JAMAIS via double. */
static void fmn_deformat_to_decimal(const char* text, const FmnConfig* f,
                                    char* out, size_t outsz) {
    if (out == NULL || outsz == 0) return;
    out[0] = '\0';
    if (text == NULL) return;
    size_t symLen = (f->symbole[0] != '\0') ? strlen(f->symbole) : 0;
    size_t milLen = (f->sepMil[0] != '\0') ? strlen(f->sepMil) : 0;
    size_t decLen = (f->sepDec[0] != '\0') ? strlen(f->sepDec) : 0;
    bool paren = false;
    size_t oi = 0;
    for (size_t i = 0; text[i] != '\0' && oi < outsz - 1; ) {
        if (symLen > 0 && strncmp(text + i, f->symbole, symLen) == 0) {
            i += symLen; continue;   /* retire le symbole */
        }
        if (milLen > 0 && strncmp(text + i, f->sepMil, milLen) == 0) {
            i += milLen; continue;   /* retire le separateur de milliers */
        }
        if (decLen > 0 && strncmp(text + i, f->sepDec, decLen) == 0) {
            out[oi++] = '.'; i += decLen; continue;  /* normalise le decimal */
        }
        char c = text[i];
        if (c == '(') { paren = true; i++; continue; }
        if (c == ')') { i++; continue; }
        if (c == ' ') { i++; continue; }   /* espace de separation symbole */
        out[oi++] = c; i++;
    }
    out[oi] = '\0';
    /* Parentheses comptables = negatif : prefixe '-' si pas deja signe. */
    if (paren && out[0] != '-' && oi + 1 < outsz) {
        memmove(out + 1, out, oi + 1);
        out[0] = '-';
    }
}

static Value input_typed_get(OwlControl* w, int inputType, const char* dateMask,
                             const FnbConfig* fnb, const FmnConfig* fmn) {
    const char* t = (w != NULL) ? owl_ui_lineedit_get_text(w) : "";
    if (t == NULL) t = "";
    if (inputType == OWL_TYPESAISIE_DATE) {
        /* Qt-first : on parse le texte AFFICHE vers un owlDate. Si un masque
         * d'affichage est pose, la relecture est DIRIGEE par son ordre de jetons
         * (leve l'ambiguite JJ/MM vs MM/JJ, ) ; sinon best-effort
         * generique (owl_parse_date_multi accepte AAAAMMJJ / JJ-MM-AAAA / ISO...,
         * ). Texte vide ou non-parseable -> chaine vide, comme
         * ChaineVersDate(""), sans reimplementer le parsing date. obj_new_date
         * est renvoye immediatement (aucune alloc OWL entre-temps -> pas de
         * rooting requis, ). */
        if (t[0] == '\0') return OBJ_VAL(obj_copy_string("", 0));
        int y = 0, m = 0, d = 0, err = 0;
        if (dateMask != NULL && dateMask[0] != '\0' &&
            owl_date_parse_with_mask(t, (int)strlen(t),
                                     dateMask, (int)strlen(dateMask), &y, &m, &d))
            return OBJ_VAL(obj_new_date(y, m, d));
        if (!owl_parse_date_multi(t, (int)strlen(t), &y, &m, &d, &err))
            return OBJ_VAL(obj_copy_string("", 0));
        return OBJ_VAL(obj_new_date(y, m, d));
    }
    if (inputType == OWL_TYPESAISIE_HEURE) {
        /* Qt-first (miroir Date) : on parse le texte AFFICHE vers un owlHeure.
         * La relecture est DIRIGEE par le masque (jetons HH/MI/SS/CC) ; sans
         * masque le defaut HH:MI truncature aux minutes (decision langage :
         * defaut 2 champs, secondes a :00). Repli generique (ChaineVersHeure :
         * HH:MM[:SS[:CC]] ou solide HHMMSSCC) quand le texte ne correspond pas
         * au masque — cas d'une saisie utilisateur plus precise que le masque.
         * Texte vide ou non-parseable -> chaine vide. obj_new_time_cs est
         * renvoye immediatement (aucune alloc OWL entre-temps, ). */
        if (t[0] == '\0') return OBJ_VAL(obj_copy_string("", 0));
        const char* m = (dateMask != NULL && dateMask[0] != '\0') ? dateMask : "HH:MI";
        int hh = 0, mi = 0, sec = 0, cs = 0;
        if (owl_time_parse_with_mask(t, (int)strlen(t), m, (int)strlen(m),
                                     &hh, &mi, &sec, &cs))
            return OBJ_VAL(obj_new_time_cs(hh, mi, sec, cs));
        if (owl_parse_time_string_cs(t, (int)strlen(t), &hh, &mi, &sec, &cs))
            return OBJ_VAL(obj_new_time_cs(hh, mi, sec, cs));
        return OBJ_VAL(obj_copy_string("", 0));
    }
    if (inputType == OWL_TYPESAISIE_DUREE) {
        /*  — Qt-first : on parse le texte AFFICHÉ vers un owlDurée via
         * le moteur de masque DURÉE dédié (HH non plafonné à 23, minutes = MI/mm). La
         * relecture est DIRIGÉE par le masque effectif (défaut HH:MI:SS). Texte
         * non conforme au gabarit -> repli chaîne (aucune durée composable) ;
         * texte vide -> chaîne vide. obj_new_duration est renvoyé immédiatement
         * (aucune alloc OWL entre-temps -> pas de rooting requis, ). */
        if (t[0] == '\0') return OBJ_VAL(obj_copy_string("", 0));
        const char* m = (dateMask != NULL && dateMask[0] != '\0') ? dateMask : "HH:MI:SS";
        int64_t ms = 0;
        if (owl_duree_parse_with_mask(t, (int)strlen(t), m, (int)strlen(m), &ms))
            return OBJ_VAL(obj_new_duration(ms));
        return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
    }
    if (inputType == OWL_TYPESAISIE_MONETAIRE) {
        if (t[0] == '\0') return CURRENCY_VAL(0);
        /*  — si un FormatMonétaire actif pilote l'affichage, on DE-FORMATE
         * d'abord (retrait symbole + separateurs + parentheses) vers la chaine
         * decimale EXACTE avant le reparse mpdecimal (Qt-first, ). */
        const char* src = t;
        char deformatted[128];
        if (fmn != NULL) {
            fmn_deformat_to_decimal(t, fmn, deformatted, sizeof(deformatted));
            src = deformatted;
        }
        /* numeric_from_string alloue un ObjNumeric ephemere immediatement
         * consomme par numeric_to_currency_i128 (aucune alloc OWL entre les
         * deux -> pas de rooting requis, ). */
        bool ov = false;
        return CURRENCY_VAL(numeric_to_currency_i128(numeric_from_string(src), &ov));
    }
    if (inputType == OWL_TYPESAISIE_NUM_PRECIS) {
        /* Coeur du type "precis" : le texte affiche est reparse en decimal EXACT
         * via mpdecimal (numeric_from_string), JAMAIS via strtod/double — sinon
         * la valeur serait tronquee a ~17 chiffres significatifs, ce qui viderait
         * le sens du type. numeric_from_string alloue un ObjNumeric renvoye
         * immediatement (aucune alloc OWL entre-temps -> pas de rooting requis,
         * ). Texte vide -> numerique zero. */
        if (t[0] == '\0') return numeric_from_string("0");
        return numeric_from_string(t);
    }
    /* Num plat : reparse le texte affiche.  — si un FormatNombre actif
     * pilote l'affichage, on DE-FORMATE d'abord (retrait separateur milliers,
     * normalisation decimale) AVANT strtod : le GET restitue la valeur affichee
     * (arrondie), pas une valeur corrompue (Qt-first, ). */
    double d = 0.0;
    if (t[0] != '\0') {
        if (inputType == OWL_TYPESAISIE_NUM && fnb_active(fnb)) {
            d = fnb_deformat(t, fnb);
        } else {
            char* end; double x = strtod(t, &end); if (end != t) d = x;
        }
    }
    return REAL_VAL(d);
}

/* SET Valeur typee : formate la valeur (num -> owlReel, monetaire -> VAL_CURRENCY
 * exact quand deja monetaire) et AFFICHE sa forme plate dans le widget. Pas de
 * store : le GET reparse ce texte affiche (Qt-first). */
static void input_typed_set(OwlControl* w, int inputType, Value v,
                            const char* dateMask, const FnbConfig* fnb,
                            const FmnConfig* fmn) {
    if (inputType == OWL_TYPESAISIE_DATE) {
        /* Formate la date en texte affiche puis la pose dans le widget ; le GET
         * reparse ce texte (Qt-first, pas de store). Le format suit le masque
         * d'affichage pose sur le champ ; a defaut, repli JJ/MM/AAAA
         *. v peut etre un owlDate (cas nominal, ChaineVersDate) ou
         * une chaine AAAAMMJJ / JJ/MM/AAAA (repli robuste). Chaine non-date ->
         * pose brute. */
        int y = 0, m = 0, d = 0;
        bool have = false;
        if (IS_DATE(v)) {
            ObjDate* dt = AS_DATE(v);
            y = dt->year; m = dt->month; d = dt->day;
            have = true;
        } else if (IS_STRING(v)) {
            ObjString* s = AS_STRING(v);
            int err = 0;
            if (s->length > 0 &&
                owl_parse_date_multi(s->chars, s->length, &y, &m, &d, &err))
                have = true;
        }
        if (have) {
            char buf[128];
            if (dateMask != NULL && dateMask[0] != '\0')
                owl_date_format_with_mask(buf, sizeof(buf), dateMask,
                                          (int)strlen(dateMask), y, m, d);
            else
                snprintf(buf, sizeof(buf), "%02d/%02d/%04d", d, m, y);
            owl_ui_control_set_text(w, buf);
        } else {
            ObjString* s = value_to_string(v);
            owl_ui_control_set_text(w, s ? s->chars : "");
        }
        return;
    }
    if (inputType == OWL_TYPESAISIE_HEURE) {
        /* Formate l'heure en texte affiche (miroir Date) puis la pose ; le GET
         * reparse ce texte (Qt-first, pas de store). Le format suit le masque du
         * champ ; a defaut HH:MI (decision langage : secondes tronquees a :00).
         * v = owlHeure (cas nominal, ChaineVersHeure) ou chaine HHMMSS[CC] /
         * HH:MM[:SS[:CC]] (repli robuste). Chaine non-heure -> pose brute. */
        int hh = 0, mi = 0, sec = 0, cs = 0;
        bool have = false;
        if (IS_TIME(v)) {
            ObjTime* tm = AS_TIME(v);
            hh = tm->hour; mi = tm->minute; sec = tm->second; cs = tm->cs;
            have = true;
        } else if (IS_STRING(v)) {
            ObjString* s = AS_STRING(v);
            if (s->length > 0 &&
                owl_parse_time_string_cs(s->chars, s->length, &hh, &mi, &sec, &cs))
                have = true;
        }
        if (have) {
            const char* m = (dateMask != NULL && dateMask[0] != '\0') ? dateMask : "HH:MI";
            char buf[128];
            owl_time_format_with_mask(buf, sizeof(buf), m, (int)strlen(m),
                                      hh, mi, sec, cs);
            owl_ui_control_set_text(w, buf);
        } else {
            ObjString* s = value_to_string(v);
            owl_ui_control_set_text(w, s ? s->chars : "");
        }
        return;
    }
    if (inputType == OWL_TYPESAISIE_DUREE) {
        /*  — formate la durée en texte affiché via le moteur de masque
         * DURÉE dédié (défaut HH:MI:SS) puis la pose ; le GET reparse ce texte
         * (Qt-first, pas de store). v = owlDurée (cas nominal, ChaîneVersDurée)
         * ou chaîne (repli robuste : coercion durée existante HH:MM:SS /
         * J j HH:MM:SS). Chaîne non-durée -> pose brute. */
        int64_t ms = 0;
        bool have = false;
        if (IS_DURATION(v)) {
            ms = AS_DURATION(v)->totalMs;
            have = true;
        } else if (IS_STRING(v)) {
            ObjString* s = AS_STRING(v);
            int perr = 0;
            if (s->length > 0 &&
                owl_parse_duration_multi(s->chars, s->length, &ms, &perr))
                have = true;
        }
        if (have) {
            const char* m = (dateMask != NULL && dateMask[0] != '\0') ? dateMask : "HH:MI:SS";
            char buf[128];
            owl_duree_format_with_mask(buf, sizeof(buf), m, (int)strlen(m), ms);
            owl_ui_control_set_text(w, buf);
        } else {
            ObjString* s = value_to_string(v);
            owl_ui_control_set_text(w, s ? s->chars : "");
        }
        return;
    }
    Value typed;
    if (inputType == OWL_TYPESAISIE_MONETAIRE) {
        if (IS_CURRENCY(v)) {
            typed = v;
        } else {
            Value src = v;
            if (IS_STRING(v)) { double d = 0.0; value_to_real(v, &d); src = REAL_VAL(d); }
            bool ov = false;
            typed = CURRENCY_VAL(numeric_to_currency_i128(src, &ov));
        }
    } else if (inputType == OWL_TYPESAISIE_NUM_PRECIS) {
        /* owlNumerique exact : on garde la valeur telle quelle si elle est deja
         * numerique (cas nominal, aucune perte) ; sinon on reparse sa forme
         * canonique (value_to_string reste lossless pour entier/reel/monetaire),
         * jamais de transit par double au SET. */
        if (IS_NUMERIC(v)) {
            typed = v;
        } else {
            ObjString* s = value_to_string(v);
            typed = numeric_from_string(s ? s->chars : "0");
        }
    } else {
        double d = 0.0; value_to_real(v, &d); typed = REAL_VAL(d);
    }
    /*  — Champ Numerique avec FormatNombre actif : affiche la forme
     * FORMATEE (arrondi + separateurs) au lieu de la forme plate. Le GET
     * de-formate ce texte (Qt-first, pas de store). */
    if (inputType == OWL_TYPESAISIE_NUM && fnb_active(fnb)) {
        double d = 0.0; value_to_real(typed, &d);
        char buf[128];
        fnb_format_double(d, fnb, buf, sizeof(buf));
        owl_ui_control_set_text(w, buf);
        return;
    }
    /*  — Champ Monétaire avec FormatMonétaire actif : affiche la forme
     * FORMATEE (symbole + separateurs + decimales) depuis le montant EXACT. Le
     * GET de-formate ce texte (Qt-first, pas de store). typed est un
     * VAL_CURRENCY (i128 exact) : formatage sans transit par double. */
    if (inputType == OWL_TYPESAISIE_MONETAIRE && fmn != NULL) {
        char buf[128];
        fmn_format_currency(AS_CURRENCY(typed), fmn, buf, sizeof(buf));
        owl_ui_control_set_text(w, buf);
        return;
    }
    /* Texte affiche : forme reelle/monetaire/numerique plate (owl_real_to_str /
     * currency_to_str / numeric_format via value_to_string), reparsable lossless
     * par le GET. typed peut etre un ObjNumeric fraichement alloue (NUM_PRECIS) :
     * le rooter avant value_to_string qui alloue a son tour. */
    if (IS_OBJ(typed)) vm_push(typed);
    ObjString* disp = value_to_string(typed);
    if (IS_OBJ(typed)) vm_pop();
    owl_ui_control_set_text(w, disp ? disp->chars : "");
}

/*  — (re)formate ou de-formate l'affichage COURANT d'un Champ Saisie
 * num selon son FormatNombre effectif (champ store > projet). Coeur partage par
 * le focus (ui_qt) et le ContenuInitial (build_control). Lit le texte affiche,
 * le de-formate en reel, puis le reecrit :
 *   - raw == false (focus-out / ouverture) -> forme FORMATEE (arrondi + separateurs) ;
 *   - raw == true  (focus-in)               -> forme EDITABLE brute (memes decimales,
 *                                              SANS separateur de milliers, pour ne
 *                                              pas editer par-dessus les separateurs).
 * No-op si aucun format actif (defaut plat) : non-regression stricte du champ
 * sans FormatNombre. Qt-first : aucun store, on ne fait que reecrire
 * le texte ; la valeur reste ce texte reparse par le GET. */
static void input_num_reformat_display(OwlControl* w, const char* name, bool raw) {
    if (w == NULL) return;
    /*  — miroir monetaire : un champ typSaisieMonétaire a
     * FormatMonétaire effectif (champ > projet) reformate son affichage au
     * focus-out / ouverture et l'edite brut au focus-in, comme le num. Le
     * montant reste EXACT (VAL_CURRENCY, i128) : de-formate -> numeric ->
     * currency i128 -> re-formate, JAMAIS de transit par double.
     * numeric_from_string alloue un ObjNumeric ephemere immediatement consomme
     * par numeric_to_currency_i128 (aucune alloc OWL entre les deux -> pas de
     * rooting requis, ). No-op sans FormatMonétaire actif. */
    if (owl_ui_control_get_input_type(w) == OWL_TYPESAISIE_MONETAIRE) {
        FmnConfig fmn;
        if (!input_mon_format_by_name(name, &fmn)) return;
        const char* mt = owl_ui_lineedit_get_text(w);
        if (mt == NULL || mt[0] == '\0') return;
        char deformatted[128];
        fmn_deformat_to_decimal(mt, &fmn, deformatted, sizeof(deformatted));
        bool ov = false;
        __int128 cents = numeric_to_currency_i128(numeric_from_string(deformatted), &ov);
        char mbuf[128];
        if (raw) {
            FmnConfig r = fmn;
            r.symbole[0] = '\0';   /* edition : ni symbole ... */
            r.sepMil[0]  = '\0';   /* ... ni separateur de milliers */
            fmn_format_currency(cents, &r, mbuf, sizeof(mbuf));
        } else {
            fmn_format_currency(cents, &fmn, mbuf, sizeof(mbuf));
        }
        owl_ui_control_set_text(w, mbuf);
        return;
    }
    FnbConfig fnb;
    if (!input_num_format_by_name(name, &fnb) || !fnb_active(&fnb)) return;
    const char* t = owl_ui_lineedit_get_text(w);
    if (t == NULL || t[0] == '\0') return;
    double d = fnb_deformat(t, &fnb);
    char buf[128];
    if (raw) {
        FnbConfig r = fnb;
        r.sepMil[0] = '\0';   /* edition : pas de separateur de milliers */
        fnb_format_double(d, &r, buf, sizeof(buf));
    } else {
        fnb_format_double(d, &fnb, buf, sizeof(buf));
    }
    owl_ui_control_set_text(w, buf);
}

/*  — pose (a la construction du champ) le validateur num config-aware
 * quand un FormatNombre effectif est actif (champ store > projet). No-op sinon :
 * le champ plat garde son QDoubleValidator locale-C. */
static void input_num_apply_format_validator(OwlControl* w, const char* name) {
    if (w == NULL) return;
    /*  — miroir monetaire : le champ typSaisieMonétaire a
     * FormatMonétaire effectif route vers le validateur config-aware (remap
     * touche decimale + neutralisation milliers), ce qui court-circuite le
     * QDoubleValidator hardcode a 2 decimales (owl_has_num_fmt_validator = true).
     * Sans separateur decimal configure, le validateur retombe sur '.'. */
    if (owl_ui_control_get_input_type(w) == OWL_TYPESAISIE_MONETAIRE) {
        FmnConfig fmn;
        if (!input_mon_format_by_name(name, &fmn)) return;
        owl_ui_lineedit_set_num_format_validator(w, fmn.sepDec);
        return;
    }
    FnbConfig fnb;
    if (!input_num_format_by_name(name, &fnb) || !fnb_active(&fnb)) return;
    owl_ui_lineedit_set_num_format_validator(w, fnb.sepDec);
}

/*  — pont ui_qt -> reformat num au focus. Appele par l'eventFilter
 * FocusIn/FocusOut d'OwlEventDispatcher sur un QLineEdit. Non-static : lie
 * depuis ui_qt.cpp. focusIn != 0 -> forme brute editable ; sinon forme formatee.
 * Aucun effet hors Champ Saisie num a FormatNombre effectif actif. */
void owl_ui_input_num_on_focus(OwlControl* w, int focusIn) {
    if (w == NULL) return;
    /*  — num ET monetaire partagent la couche interactive de
     * (re)formatage au focus. Les autres types (Date/Heure/Durée/Texte) n'ont
     * pas de reformat au focus. input_num_reformat_display branche lui-meme
     * selon le type effectif du widget. */
    int typeSaisie = owl_ui_control_get_input_type(w);
    if (typeSaisie != OWL_TYPESAISIE_NUM && typeSaisie != OWL_TYPESAISIE_MONETAIRE)
        return;
    const char* name = current_window_ctrl_name_by_widget((void*)w);
    input_num_reformat_display(w, name, focusIn != 0);
}

/* Dispatcher 'ctrl..Prop' appele depuis OP_STRUCT_GET (vm.c) via le
 * function pointer registered par stdlib_ui_init(). Phase E.X complete :
 * Valeur, Visible, Grise, Largeur, Hauteur, X, Y, Libelle.
 * Pour Saisie, le contenu est Valeur. Pour Bouton/Libelle, Valeur = Libelle.
 *
 *  lot C — Les fenetres ne transitent plus par ce dispatcher :
 * le proxy ObjWindowRef pose au compile-time dispatche directement vers
 * le store d'instance (vm.c branche IS_WINDOW_REF) avec reflet Qt via
 * owl_ui_apply_window_scalar. Plus de kind==OWL_CONTROL_WINDOW ici. */
static Value real_ui_get_prop(ObjUiControl* c, const char* prop) {
    if (!c || !c->widget || !prop) return NIL_VAL;
    OwlControlKind kind = (OwlControlKind)c->kind;

    OwlControl* w = (OwlControl*)c->widget;

    /*  lot 3 — LIB_X..Cadre retourne l'instance live depuis le STORE
     * de l'ObjUiControlInstance (source de verite unique GUI/headless), reliee
     * a son widget. Permet la chaine LIB_X..Cadre..Epaisseur = 1 : le set
     * sur l'instance declenche le watcher qui re-applique au QWidget. */
    if (strcasecmp(prop, "Cadre") == 0 || strcasecmp(prop, "Border") == 0) {
        ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
        if (uici != NULL)
            return OBJ_VAL(uici_composite_instance(uici, "Cadre", "Cadre"));
        return NIL_VAL;
    }

    /*  — SAI..FormatNombre : instance du store si posee, sinon une
     * instance FormatNombre par defaut (jamais nil, la cascade ..X reste sure). */
    if (kind == OWL_CONTROL_INPUT
        && (strcasecmp(prop, "FormatNombre") == 0
            || strcasecmp(prop, "NumberFormat") == 0)) {
        ObjInstance* fnb = input_fnb_instance_for(c);
        if (fnb != NULL) return OBJ_VAL(fnb);
        ObjString* nm = obj_copy_string("FormatNombre", 12);
        vm_push(OBJ_VAL(nm));
        ObjInstance* def = obj_new_instance(nm);
        vm_pop();
        vm_push(OBJ_VAL(def));
        inst_field_set_str(def, "SeparateurDecimal",   "");
        inst_field_set_str(def, "SeparateurMilliers",  "");
        inst_field_set_int(def, "ChiffresApresVirgule", OWL_FNB_AUTO);
        vm_pop();
        return OBJ_VAL(def);
    }

    /*  — SAI..FormatMonétaire : instance du store si posee, sinon une
     * instance FormatMonétaire par defaut (jamais nil, la cascade ..X reste sure). */
    if (kind == OWL_CONTROL_INPUT
        && (strcasecmp(prop, "FormatMon\xc3\xa9taire") == 0
            || strcasecmp(prop, "FormatMonetaire") == 0
            || strcasecmp(prop, "CurrencyFormat") == 0)) {
        ObjInstance* fmn = input_fmn_instance_for(c);
        if (fmn != NULL) return OBJ_VAL(fmn);
        ObjString* nm = obj_copy_string("FormatMon\xc3\xa9taire", 16);
        vm_push(OBJ_VAL(nm));
        ObjInstance* def = obj_new_instance(nm);
        vm_pop();
        vm_push(OBJ_VAL(def));
        inst_field_set_str(def, "Symbole",              "");
        inst_field_set_int(def, "PositionSymbole",       OWL_POSSYM_APRES);
        inst_field_set_str(def, "SeparateurDecimal",    "");
        inst_field_set_str(def, "SeparateurMilliers",   "");
        inst_field_set_int(def, "ChiffresApresVirgule", 2);
        inst_field_set_int(def, "FormatNegatif",         OWL_FMN_MOINS);
        vm_pop();
        return OBJ_VAL(def);
    }

    /* --- Valeur / Libelle (selon kind) --- */
    bool wantsLib  = strcasecmp(prop, "Libelle") == 0 || strcasecmp(prop, "Caption") == 0;
    bool wantsDef  = prop_is_default(prop);

    /*  — symetrique du setter : pour
     * Selecteur/Liste/Combo/Interrupteur, la propriete par defaut 'Valeur'
     * designe une SELECTION (index / bitmask), pas un texte. Sans ce garde,
     * la lecture 'Valeur' tombait dans get_text et court-circuitait le getter
     * de selection specifique plus bas (branches mortes), renvoyant le texte
     * du widget au lieu de la selection. 'Libelle' garde son get_text. */
    bool defIsSelection = wantsDef &&
        (kind == OWL_CONTROL_SELECTEUR || kind == OWL_CONTROL_LISTE ||
         kind == OWL_CONTROL_COMBO    || kind == OWL_CONTROL_INTERRUPTEUR ||
         kind == OWL_CONTROL_JAUGE    || kind == OWL_CONTROL_ONGLET);

    if ((wantsLib || wantsDef) && !defIsSelection) {
        /*  — Saisie num/monetaire : ..Valeur renvoie la valeur typee
         * (owlReel / owlMonetaire), pas le texte affiche. Les autres types de
         * saisie (Texte/MotDePasse/Multiligne) gardent la chaine ci-dessous. */
        if (kind == OWL_CONTROL_INPUT && wantsDef) {
            int it = owl_ui_control_get_input_type(w);
            if (it == OWL_TYPESAISIE_NUM || it == OWL_TYPESAISIE_MONETAIRE ||
                it == OWL_TYPESAISIE_NUM_PRECIS ||
                it == OWL_TYPESAISIE_DATE || it == OWL_TYPESAISIE_HEURE ||
                it == OWL_TYPESAISIE_DUREE) {
                const char* mask = (it == OWL_TYPESAISIE_DATE)  ? input_date_mask_for(c)
                                 : (it == OWL_TYPESAISIE_HEURE) ? input_time_mask_for(c)
                                 : (it == OWL_TYPESAISIE_DUREE) ? input_duree_mask_for(c)
                                 : NULL;
                FnbConfig fnbBuf; const FnbConfig* fnb = NULL;
                if (it == OWL_TYPESAISIE_NUM && input_num_format_for(c, &fnbBuf))
                    fnb = &fnbBuf;
                FmnConfig fmnBuf; const FmnConfig* fmn = NULL;
                if (it == OWL_TYPESAISIE_MONETAIRE && input_mon_format_for(c, &fmnBuf))
                    fmn = &fmnBuf;
                return input_typed_get(w, it, mask, fnb, fmn);
            }
        }
        const char* t = NULL;
        if (kind == OWL_CONTROL_INPUT) {
            t = owl_ui_lineedit_get_text(w);
        } else {
            t = owl_ui_control_get_text(w);
        }
        if (!t) t = "";
        return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
    }

    /* --- Champ Liste / Combo : props communes --- */
    if (kind == OWL_CONTROL_LISTE || kind == OWL_CONTROL_COMBO) {
        int sel = owl_ui_listcombo_get_selection(w);  /* 1-based, 0 si rien */
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0) {
            return INTEGER_VAL(sel > 0 ? sel : -1);
        }
        if (strcasecmp(prop, "ValeurMemorisee") == 0 ||
            strcasecmp(prop, "StoredValue") == 0) {
            if (sel <= 0) return OBJ_VAL(obj_copy_string("", 0));
            const char* s = owl_ui_listcombo_get_stored(w, sel);
            if (s) return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
            /* ValeurMemorisee est une chaine (clef interne) : pas de
             * valeur stockee -> chaine vide, jamais l'index entier. */
            return OBJ_VAL(obj_copy_string("", 0));
        }
        if (strcasecmp(prop, "ValeurAffichee") == 0 ||
            strcasecmp(prop, "DisplayedValue") == 0) {
            if (sel <= 0) return OBJ_VAL(obj_copy_string("", 0));
            const char* t = owl_ui_listcombo_get_item(w, sel);
            if (!t) t = "";
            return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
        }
        if (strcasecmp(prop, "Occurrence") == 0)
            return INTEGER_VAL(owl_ui_listcombo_count(w));
        if (strcasecmp(prop, "Vide") == 0 || strcasecmp(prop, "Empty") == 0)
            return BOOL_VAL(owl_ui_listcombo_count(w) == 0);
        if (strcasecmp(prop, "Triee") == 0 || strcasecmp(prop, "Sorted") == 0)
            return BOOL_VAL(owl_ui_listcombo_get_sorted(w) != 0);
    }
    /* Liste-only (multisel + row height + sort option + zoom). */
    if (kind == OWL_CONTROL_LISTE) {
        if (strcasecmp(prop, "Multiselection") == 0 ||
            strcasecmp(prop, "MultiSelection") == 0)
            return BOOL_VAL(owl_ui_list_get_multiselect(w) != 0);
        if (strcasecmp(prop, "OptionTri") == 0 || strcasecmp(prop, "SortOption") == 0)
            return INTEGER_VAL(owl_ui_list_get_sort_option(w));
        if (strcasecmp(prop, "Zoom") == 0)
            return INTEGER_VAL(owl_ui_list_get_zoom(w));
    }
    /* Champ ZoneRepetee — Occurrence / LigneSelectionnee / Sens / Multiselection. */
    if (kind == OWL_CONTROL_ZONE_REPETEE) {
        if (strcasecmp(prop, "Occurrence") == 0)
            return INTEGER_VAL(owl_ui_zr_count(w));
        if (strcasecmp(prop, "Vide") == 0 || strcasecmp(prop, "Empty") == 0)
            return BOOL_VAL(owl_ui_zr_count(w) == 0);
        if (strcasecmp(prop, "LigneS\xc3\xa9lectionn\xc3\xa9""e") == 0 ||
            strcasecmp(prop, "LigneSelectionnee") == 0 ||
            strcasecmp(prop, "SelectedRow") == 0)
            return INTEGER_VAL(owl_ui_zr_get_selected(w));
        if (strcasecmp(prop, "Sens") == 0 || strcasecmp(prop, "Direction") == 0)
            return INTEGER_VAL(OWL_ZRSENS_VERTICAL);
        if (strcasecmp(prop, "Multis\xc3\xa9lection") == 0 ||
            strcasecmp(prop, "Multiselection") == 0 ||
            strcasecmp(prop, "MultiSelection") == 0)
            return BOOL_VAL(owl_ui_zr_get_multi_select(w) != 0);
        /* Sub-widget broadcast : ZR.SAI_X retourne un proxy "broadcast" qui,
         * a l'ecriture STRUCT_SET, applique sur toutes les reps. Le proxy
         * encode le sub-name dans son name avec prefixe magique. On verifie
         * que le sub existe au moins dans la rep 1 avant de fabriquer le
         * proxy ; sinon NIL (sub inconnu). */
        if (owl_ui_zr_count(w) > 0) {
            OwlControl* rep1 = owl_ui_zr_get_rep(w, 1);
            if (rep1 && owl_ui_control_find_child(rep1, prop)) {
                char bname[160];
                snprintf(bname, sizeof(bname), "__zr_bcast__%s", prop);
                ObjUiControl* proxy = obj_new_ui_control(
                    (int)OWL_CONTROL_ZONE_REPETEE, w, bname);
                return OBJ_VAL(proxy);
            }
        }
    }

    /* Champ Interrupteur — Valeur (bitmask) / Occurrence / Vide / NombreColonne. */
    if (kind == OWL_CONTROL_INTERRUPTEUR) {
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0) {
            return INTEGER_VAL(owl_ui_interrupteur_get_bitmask(w));
        }
        if (strcasecmp(prop, "Occurrence") == 0)
            return INTEGER_VAL(owl_ui_interrupteur_count(w));
        if (strcasecmp(prop, "Vide") == 0 || strcasecmp(prop, "Empty") == 0)
            return BOOL_VAL(owl_ui_interrupteur_count(w) == 0);
        if (strcasecmp(prop, "NombreColonne") == 0 ||
            strcasecmp(prop, "ColumnCount") == 0)
            return INTEGER_VAL(owl_ui_interrupteur_get_column_count(w));
        if (strcasecmp(prop, "HauteurOption") == 0 ||
            strcasecmp(prop, "OptionHeight") == 0)
            return INTEGER_VAL(owl_ui_interrupteur_get_option_height(w));
        if (strcasecmp(prop, "ModeAffichage") == 0 ||
            strcasecmp(prop, "DisplayMode") == 0)
            return INTEGER_VAL(owl_ui_interrupteur_get_display_mode(w));
        if (strcasecmp(prop, "TroisEtats") == 0 ||
            strcasecmp(prop, "ThreeStates") == 0)
            return BOOL_VAL(owl_ui_interrupteur_get_three_states(w) != 0);
    }

    /* Champ Jauge — Valeur (bornee) / BorneMin / BorneMax / Vertical /
     * Symetrique / CouleurJauge / TexteJauge. Valeur lit le QProgressBar
     * (deja clampe a [min, max] par le backend). */
    if (kind == OWL_CONTROL_JAUGE) {
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0)
            return INTEGER_VAL(owl_ui_jauge_get_value(w));
        if (strcasecmp(prop, "BorneMin") == 0 || strcasecmp(prop, "MinValue") == 0)
            return INTEGER_VAL(owl_ui_jauge_get_min(w));
        if (strcasecmp(prop, "BorneMax") == 0 || strcasecmp(prop, "MaxValue") == 0)
            return INTEGER_VAL(owl_ui_jauge_get_max(w));
        if (strcasecmp(prop, "Vertical") == 0)
            return BOOL_VAL(owl_ui_jauge_get_vertical(w) != 0);
        if (strcasecmp(prop, "Symetrique") == 0 || strcasecmp(prop, "Symmetric") == 0)
            return BOOL_VAL(owl_ui_jauge_get_symmetric(w) != 0);
        if (strcasecmp(prop, "CouleurJauge") == 0 ||
            strcasecmp(prop, "ProgressBarColor") == 0) {
            int c = owl_ui_jauge_get_progress_color(w);
            return (c >= 0) ? INTEGER_VAL(c) : NIL_VAL;
        }
        if (strcasecmp(prop, "TexteJauge") == 0 ||
            strcasecmp(prop, "TextProgressBar") == 0) {
            const char* t = owl_ui_jauge_get_text(w);
            if (!t) t = "";
            return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
        }
        /* Cadrage du texte % : alignement dedie du widget (le get_h_align
         * generique lit l'alignement Qt d'un QLabel/QLineEdit, no-op ici). */
        if (strcasecmp(prop, "CadrageHorizontal") == 0 ||
            strcasecmp(prop, "HorizontalAlignment") == 0)
            return INTEGER_VAL(owl_ui_jauge_get_h_align(w));
    }

    /* Onglet — Valeur = index du volet actif (1-based, cohérent avec le setter
     * et le store headless). En GUI le QTabWidget n'a pas de texte : sans cette
     * branche, la lecture tombait dans get_text et renvoyait "". */
    if (kind == OWL_CONTROL_ONGLET) {
        if (wantsDef || strcasecmp(prop, "Valeur") == 0 ||
            strcasecmp(prop, "Value") == 0)
            return INTEGER_VAL(owl_ui_onglet_get_current(w) + 1);
    }

    /* Combo-only (EnSaisie + MaxLignesAffichees). */
    if (kind == OWL_CONTROL_COMBO) {
        if (strcasecmp(prop, "EnSaisie") == 0 || strcasecmp(prop, "Editable") == 0)
            return BOOL_VAL(owl_ui_combo_get_editable(w) != 0);
        if (strcasecmp(prop, "MaxLignesAffichees") == 0 ||
            strcasecmp(prop, "MaxVisibleItems") == 0)
            return INTEGER_VAL(owl_ui_combo_get_max_visible(w));
    }
    /* Selecteur — Valeur / ValeurMemorisee / Occurrence / NombreColonne / ModeAffichage. */
    if (kind == OWL_CONTROL_SELECTEUR) {
        int sel = owl_ui_selecteur_get_selection_index(w);
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0) {
            return INTEGER_VAL(sel > 0 ? sel : -1);
        }
        if (strcasecmp(prop, "ValeurMemorisee") == 0 ||
            strcasecmp(prop, "StoredValue") == 0 ||
            strcasecmp(prop, "ValeurRenvoyee") == 0) {
            if (sel <= 0) return OBJ_VAL(obj_copy_string("", 0));
            const char* s = owl_ui_selecteur_get_option_stored(w, sel);
            if (s && *s) return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
            /* ValeurMemorisee est une chaine : pas de valeur stockee
             * -> chaine vide, jamais l'index entier. */
            return OBJ_VAL(obj_copy_string("", 0));
        }
        if (strcasecmp(prop, "ValeurAffichee") == 0 ||
            strcasecmp(prop, "DisplayedValue") == 0) {
            if (sel <= 0) return OBJ_VAL(obj_copy_string("", 0));
            const char* t = owl_ui_selecteur_get_option_label(w, sel);
            if (!t) t = "";
            return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
        }
        if (strcasecmp(prop, "Occurrence") == 0)
            return INTEGER_VAL(owl_ui_selecteur_count(w));
        if (strcasecmp(prop, "Vide") == 0 || strcasecmp(prop, "Empty") == 0)
            return BOOL_VAL(owl_ui_selecteur_count(w) == 0);
        if (strcasecmp(prop, "NombreColonne") == 0 ||
            strcasecmp(prop, "ColumnCount") == 0)
            return INTEGER_VAL(owl_ui_selecteur_get_column_count(w));
        if (strcasecmp(prop, "HauteurOption") == 0 ||
            strcasecmp(prop, "OptionHeight") == 0)
            return INTEGER_VAL(owl_ui_selecteur_get_option_height(w));
        if (strcasecmp(prop, "ModeAffichage") == 0 ||
            strcasecmp(prop, "DisplayMode") == 0)
            return INTEGER_VAL(owl_ui_selecteur_get_display_mode(w));
    }

    /* --- Geometrie ---
     *  — ces quatre proprietes rendent la geometrie que le champ
     * occupe VRAIMENT, y compris quand elle vient du layout et non du
     * fichier de description. La synchro prealable evite de rendre une
     * taille provisoire quand la lecture precede le premier tour de boucle
     * (cas FinInitialisation). */
    if (strcasecmp(prop, "Largeur") == 0 || strcasecmp(prop, "Width") == 0 ||
        strcasecmp(prop, "Hauteur") == 0 || strcasecmp(prop, "Height") == 0 ||
        strcasecmp(prop, "X") == 0 || strcasecmp(prop, "Y") == 0) {
        owl_ui_control_sync_layout(w);
        if (strcasecmp(prop, "Largeur") == 0 || strcasecmp(prop, "Width") == 0)
            return INTEGER_VAL(owl_ui_control_get_width(w));
        if (strcasecmp(prop, "Hauteur") == 0 || strcasecmp(prop, "Height") == 0)
            return INTEGER_VAL(owl_ui_control_get_height(w));
        if (strcasecmp(prop, "X") == 0)
            return INTEGER_VAL(owl_ui_control_get_x(w));
        return INTEGER_VAL(owl_ui_control_get_y(w));
    }

    /* --- Etats --- */
    if (strcasecmp(prop, "Visible") == 0)
        return BOOL_VAL(owl_ui_control_get_visible(w) != 0);
    if (strcasecmp(prop, "Grise") == 0 || strcasecmp(prop, "Disabled") == 0)
        return BOOL_VAL(owl_ui_control_get_enabled(w) == 0);

    /* --- Bulle (commun aux 3 controles) --- */
    if (strcasecmp(prop, "Bulle") == 0 || strcasecmp(prop, "Tooltip") == 0) {
        const char* t = owl_ui_control_get_tooltip(w);
        if (!t) t = "";
        return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
    }

    /* --- Police composee : retourne l'INSTANCE du store de l'uici (source de
     * verite unique GUI/headless). Permet la chaine LIB_X..Police..Nom =
     * "Arial". Materialise lazy depuis le template si aucune instance n'est
     * encore presente.  lot 3. */
    if (strcasecmp(prop, "Police") == 0 || strcasecmp(prop, "Font") == 0) {
        ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
        ObjInstance* pol = uici ? uici_composite_instance(uici, "Police", "Police")
                                : NULL;
        if (pol == NULL) return NIL_VAL;
        return OBJ_VAL(pol);
    }
    /* --- Raccourcis Police plats : lisent le sous-champ de l'instance (meme
     * source de verite que la cascade ..Police..XXX). Equivalence garantie. */
    {
        const char* polSub = NULL;
        if (strcasecmp(prop, "PoliceNom") == 0 || strcasecmp(prop, "FontName") == 0)
            polSub = "Nom";
        else if (strcasecmp(prop, "PoliceTaille") == 0 || strcasecmp(prop, "FontSize") == 0)
            polSub = "Taille";
        else if (strcasecmp(prop, "PoliceGras") == 0 || strcasecmp(prop, "FontBold") == 0)
            polSub = "Gras";
        else if (strcasecmp(prop, "PoliceItalique") == 0 || strcasecmp(prop, "FontItalic") == 0)
            polSub = "Italique";
        else if (strcasecmp(prop, "PoliceSoulignee") == 0 ||
                 strcasecmp(prop, "FontUnderlined") == 0)
            polSub = "Souligne";
        else if (strcasecmp(prop, "PoliceBarree") == 0 ||
                 strcasecmp(prop, "FontStrikeOut") == 0)
            polSub = "Barre";
        else if ((strcasecmp(prop, "PoliceCouleur") == 0 ||
                  strcasecmp(prop, "FontColor") == 0)
                 && kind != OWL_CONTROL_INPUT
                 && kind != OWL_CONTROL_LABEL
                 && kind != OWL_CONTROL_BUTTON)
            /*  — Saisie : PoliceCouleur lit Style..ZoneSaisie..Couleur.
             *  — Libellé/Bouton : PoliceCouleur lit Style..Libellé..Couleur
             * (bloc mono dedie plus bas). Les autres widgets stylés gardent la
             * couleur du texte portee par l'instance Police. */
            polSub = "Couleur";
        if (polSub != NULL) {
            ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
            ObjInstance* pol = uici ? uici_composite_instance(uici, "Police", "Police")
                                    : NULL;
            if (pol == NULL) return NIL_VAL;
            ObjString* sk = table_find_key_case_insensitive(&pol->fields, polSub);
            Value sv;
            if (sk != NULL && table_get(&pol->fields, sk, &sv)) return sv;
            return NIL_VAL;
        }
    }

    /*  — raccourcis d'apparence plats du Champ Saisie : lisent le
     * sous-champ de la ZoneSaisie de l'instance Style du store (verite unique).
     * CouleurFond->ZoneSaisie..CouleurFond, PoliceCouleur->ZoneSaisie..Couleur.
     * Gate INPUT ; equivalence stricte avec la cascade `..Style..CouleurFond`. */
    if (kind == OWL_CONTROL_INPUT) {
        const char* zsSub = NULL;
        if (strcasecmp(prop, "CouleurFond") == 0 ||
            strcasecmp(prop, "BackgroundColor") == 0)
            zsSub = "CouleurFond";
        else if (strcasecmp(prop, "PoliceCouleur") == 0 ||
                 strcasecmp(prop, "FontColor") == 0)
            zsSub = "Couleur";
        if (zsSub != NULL) {
            ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
            ObjInstance* style = uici
                ? uici_composite_instance(uici, "Style", "styleSaisie")
                : NULL;
            ObjInstance* zone = style
                ? instance_get_instance(style, "ZoneSaisie") : NULL;
            if (zone == NULL) return NIL_VAL;
            ObjString* sk = table_find_key_case_insensitive(&zone->fields, zsSub);
            Value sv;
            if (sk != NULL && table_get(&zone->fields, sk, &sv)) return sv;
            return NIL_VAL;
        }
    }

    /*  — raccourcis d'apparence plats des widgets mono (Libellé/Bouton) :
     * lisent le sous-champ de la sous-brique correspondante de l'instance Style
     * du store (verite unique). CouleurFond->CadreExtérieur..CouleurFond,
     * PoliceCouleur->Libellé..Couleur. Equivalence stricte avec la cascade
     * `..Style..SousElement..Prop`. */
    {
        const char* mBrick = NULL;
        const char* mSub   = NULL;
        if (mono_style_shortcut_bricksub(kind, prop, &mBrick, &mSub)) {
            ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
            ObjInstance* style = uici
                ? uici_composite_instance(uici, "Style",
                                          mono_style_template_name(kind))
                : NULL;
            ObjInstance* brick = style ? instance_get_instance(style, mBrick)
                                       : NULL;
            if (brick == NULL) return NIL_VAL;
            ObjString* sk = table_find_key_case_insensitive(&brick->fields, mSub);
            Value sv;
            if (sk != NULL && table_get(&brick->fields, sk, &sv)) return sv;
            return NIL_VAL;
        }
    }

    /* --- Cadrage (Libelle + Saisie) --- */
    if (strcasecmp(prop, "CadrageHorizontal") == 0 ||
        strcasecmp(prop, "HorizontalAlignment") == 0)
        return INTEGER_VAL(owl_ui_control_get_h_align(w));
    if (strcasecmp(prop, "CadrageVertical") == 0 ||
        strcasecmp(prop, "VerticalAlignment") == 0)
        return INTEGER_VAL(owl_ui_control_get_v_align(w));

    /* --- Etat unifie (commun aux 3 controles) --- */
    if (strcasecmp(prop, "Etat") == 0 || strcasecmp(prop, "State") == 0)
        return INTEGER_VAL(owl_ui_control_get_etat(w));

    /* --- Opacite (commun aux 3 controles) --- */
    if (strcasecmp(prop, "Opacite") == 0 || strcasecmp(prop, "Opacity") == 0)
        return INTEGER_VAL(owl_ui_control_get_opacite(w));

    /* --- Etirement de police (commun) : relit le stretch Qt -> enum. --- */
    if (strcasecmp(prop, "PoliceEtirement") == 0 ||
        strcasecmp(prop, "FontStretch") == 0)
        return INTEGER_VAL(owl_stretch_to_etirement(
            owl_ui_control_get_font_stretch(w)));

    /* --- Taille minimale de police (fit) (commun). --- */
    if (strcasecmp(prop, "PoliceTailleMin") == 0 ||
        strcasecmp(prop, "FontMinSize") == 0)
        return INTEGER_VAL(owl_ui_control_get_font_min_size(w));

    /* --- Specifiques Libelle --- */
    if (kind == OWL_CONTROL_LABEL) {
        if (strcasecmp(prop, "TypeLibelle") == 0 || strcasecmp(prop, "LabelType") == 0)
            return INTEGER_VAL(owl_ui_label_get_word_wrap(w));
        if (strcasecmp(prop, "Couleur") == 0 || strcasecmp(prop, "Color") == 0)
            return INTEGER_VAL(owl_ui_control_get_text_color(w));
        if (strcasecmp(prop, "CouleurFond") == 0 ||
            strcasecmp(prop, "BackgroundColor") == 0)
            return INTEGER_VAL(owl_ui_control_get_bg_color(w));
        /* Cadre — type compose (canon). Lecture renvoie
         * l'instance live stockee dans les props du widget decl ; voir
         * Phase 3 (audit DSL) pour le reading propagation. V1 simple :
         * le getter live retourne NIL — la lecture live se fait cote prop
         * du decl (LIB_X..Cadre = ... lit la prop init, pas le widget). */
        if (strcasecmp(prop, "Image") == 0) {
            const char* s = owl_ui_label_get_image(w);
            if (!s) s = "";
            return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
        }
        if (strcasecmp(prop, "Cadrage") == 0 ||
            strcasecmp(prop, "Framing") == 0) {
            return INTEGER_VAL(owl_ui_label_get_cadrage(w));
        }
    }

    /* Champ Table : HauteurTitre se lit sur le widget, pas dans le store. Un
     * echo de ce qui a ete declare ne dirait rien de la bande reellement
     * rendue — or c'est precisement ce qu'on veut pouvoir constater (0 =
     * en-tete masque, valeur naturelle sinon). */
    if (kind == OWL_CONTROL_TABLE &&
        (strcasecmp(prop, "HauteurTitre") == 0 ||
         strcasecmp(prop, "TitleHeight") == 0)) {
        return INTEGER_VAL(owl_ui_table_get_title_height(w));
    }

    /*  — ToucheRaccourci (focus) sur les champs interactifs non-bouton.
     * Readback depuis le QShortcut vivant parente au widget. Le Bouton a son
     * propre readback (action = clic) juste apres. */
    if ((kind == OWL_CONTROL_INPUT || kind == OWL_CONTROL_COMBO ||
         kind == OWL_CONTROL_LISTE || kind == OWL_CONTROL_SELECTEUR ||
         kind == OWL_CONTROL_INTERRUPTEUR || kind == OWL_CONTROL_TABLE ||
         kind == OWL_CONTROL_ZONE_REPETEE) &&
        (strcasecmp(prop, "ToucheRaccourci") == 0 ||
         strcasecmp(prop, "ShortcutKey") == 0)) {
        const char* s = owl_ui_get_focus_shortcut(w);
        if (!s) s = "";
        return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
    }

    /* --- Specifiques Bouton --- */
    if (kind == OWL_CONTROL_BUTTON) {
        if (strcasecmp(prop, "ToucheRaccourci") == 0 ||
            strcasecmp(prop, "ShortcutKey") == 0) {
            const char* s = owl_ui_button_get_shortcut(w);
            if (!s) s = "";
            return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
        }
        if (strcasecmp(prop, "TypeBouton") == 0 || strcasecmp(prop, "ButtonType") == 0)
            return INTEGER_VAL(owl_ui_button_get_type_bouton(w));
        if (strcasecmp(prop, "Image") == 0) {
            const char* s = owl_ui_button_get_icon(w);
            if (!s) s = "";
            return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
        }
    }

    /* --- Specifiques Saisie --- */
    if (kind == OWL_CONTROL_INPUT) {
        if (strcasecmp(prop, "Indication") == 0 ||
            strcasecmp(prop, "Placeholder") == 0) {
            const char* s = owl_ui_lineedit_get_placeholder(w);
            if (!s) s = "";
            return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
        }
        if (strcasecmp(prop, "MaxLongueur") == 0 || strcasecmp(prop, "MaxLength") == 0)
            return INTEGER_VAL(owl_ui_lineedit_get_max_length(w));
        if (strcasecmp(prop, "TypeSaisie") == 0 || strcasecmp(prop, "InputType") == 0)
            return INTEGER_VAL(owl_ui_control_get_input_type(w));
        if (strcasecmp(prop, "Curseur") == 0 || strcasecmp(prop, "Cursor") == 0)
            return INTEGER_VAL(owl_ui_lineedit_get_cursor(w));
        if (strcasecmp(prop, "FinCurseur") == 0 || strcasecmp(prop, "EndCursor") == 0)
            return INTEGER_VAL(owl_ui_lineedit_get_end_cursor(w));
        if (strcasecmp(prop, "TexteSelectionne") == 0 ||
            strcasecmp(prop, "SelectedText") == 0) {
            const char* s = owl_ui_lineedit_get_selected_text(w);
            if (!s) s = "";
            return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
        }
        if (strcasecmp(prop, "BorneMin") == 0 || strcasecmp(prop, "MinValue") == 0)
            return INTEGER_VAL(owl_ui_lineedit_get_min_value(w));
        if (strcasecmp(prop, "BorneMax") == 0 || strcasecmp(prop, "MaxValue") == 0)
            return INTEGER_VAL(owl_ui_lineedit_get_max_value(w));
        if (strcasecmp(prop, "Modifie") == 0 || strcasecmp(prop, "Modified") == 0)
            return BOOL_VAL(owl_ui_lineedit_get_modified(w) != 0);
        if (strcasecmp(prop, "MasqueSaisie") == 0 ||
            strcasecmp(prop, "InputMask") == 0) {
            const char* s = owl_ui_lineedit_get_mask(w);
            if (!s) s = "";
            return OBJ_VAL(obj_copy_string(s, (int)strlen(s)));
        }
    }

    /* --- Specifiques Champ CodeBarres : les metadonnees du code-barres
     *     (type/contenu/correction/options + le descripteur complet) ne sont
     *     PAS portees par le QLabel (qui ne detient que l'image rasterisee).
     *     On les relit depuis le store d'instance (source de verite, seede au
     *     declaratif et maj a chaque set runtime). --- */
    if (kind == OWL_CONTROL_CODEBARRE &&
        (strcasecmp(prop, "Contenu") == 0          || strcasecmp(prop, "Content") == 0 ||
         strcasecmp(prop, "TypeCodeBarres") == 0    || strcasecmp(prop, "TypeBarCode") == 0 ||
         strcasecmp(prop, "CorrectionErreur") == 0  || strcasecmp(prop, "CorrectionError") == 0 ||
         strcasecmp(prop, "CodeBarres") == 0        || strcasecmp(prop, "BarCodes") == 0 ||
         strcasecmp(prop, "Option") == 0)) {
        ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
        Value out;
        if (uici && obj_ui_control_instance_headless_get(uici, prop, &out))
            return out;
        return NIL_VAL;
    }

    /* CONTAINER fallback : si on n'a pas trouve la prop par nom canonique,
     * tenter de la resoudre comme nom de sous-widget enfant (utile pour
     * ZR[i].sub..prop ou un Conteneur peut exposer ses enfants par nom). */
    if (kind == OWL_CONTROL_CONTAINER) {
        OwlControl* child = owl_ui_control_find_child(w, prop);
        if (child) {
            int childKind = owl_ui_control_get_widget_kind(child);
            if (childKind < 0) childKind = OWL_CONTROL_CONTAINER;
            ObjUiControl* sub = obj_new_ui_control(childKind, child, prop);
            return OBJ_VAL(sub);
        }
    }

    fprintf(stderr,
        "[ui] Lecture runtime '%s..%s' non supportee.\n",
        c->name ? c->name : "?", prop);
    return NIL_VAL;
}

/*  — taille de rasterisation de l'icone d'une fenetre. Rendu haute
 * def : Qt sous-echantillonne ensuite pour la barre de titre (16) et la
 * barre des taches (32). */
#define OWL_ICONE_WINDOW_SIZE 256

/*  — pose l'icone d'une Fenetre depuis une valeur polymorphe
 * (Image | Icone catalogue | chaine chemin fichier). Miroir de
 * button_set_icon_value cible fenetre : le Champ Bouton/Image rasterisait
 * deja l'ObjIcone catalogue, la Fenetre l'ignorait silencieusement (seul
 * le chemin fichier disque marchait). Chemin partage declaration + runtime. */
static void window_set_icon_value(OwlWindow* w, Value v) {
    if (stdlib_image_is_instance(v)) {
        int iw = 0, ih = 0;
        const unsigned char* rgba = stdlib_image_get_rgba(v, &iw, &ih);
        owl_ui_window_set_icon_rgba(w, rgba, iw, ih);
    } else if (IS_ICONE(v)) {
        ObjIcone* icn = AS_ICONE(v);
        if (icn->valide && icn->svg_raw != NULL && icn->svg_raw->length > 0) {
            unsigned char* rgba = NULL;
            int iw = 0, ih = 0;
            bool want_bg = (icn->couleur_fond != -1);
            if (owl_icone_rasterize_rgba(
                    icn->svg_raw->chars, icn->svg_raw->length,
                    /*  : -1 unset -> 0 (noir) defaut au lieu de blanc. */
                    (icn->couleur == -1) ? 0u : (unsigned int)(icn->couleur & 0xFFFFFF),
                    icn->couleur_modifiable,
                    OWL_ICONE_WINDOW_SIZE, icn->rotation,
                    icn->forme,
                    (unsigned int)((want_bg ? icn->couleur_fond : 0) & 0xFFFFFF),
                    want_bg,
                    &rgba, &iw, &ih)) {
                owl_ui_window_set_icon_rgba(w, rgba, iw, ih);
                free(rgba);
            }
        }
    } else {
        ObjString* s = value_to_string(v);
        owl_ui_window_set_icon(w, s ? s->chars : NULL);
    }
}

/*  lot C — Reflet Qt d'une ecriture scalaire dans le casier
 * d'instance fenetre. Appele via le hook owl_ui_apply_window_scalar
 * depuis OP_STRUCT_SET (vm.c, branche IS_WINDOW_REF) quand l'instance
 * porte un widget Qt monte. Mirroir strict de apply_instance_scalar_impl
 * pour les sous-controles : la source de verite reste le store (
 * etendu fenetre lot B), le hook applique le scalaire au QMainWindow.
 *
 * Retourne true si la prop a un equivalent Qt, false sinon (cosmetique :
 * pas exploite par l'appelant hook). */
static void apply_style_to_window(OwlWindow* w, ObjInstance* style);

static bool window_set_prop_qt(OwlWindow* w, const char* prop, Value v) {
    if (!w || !prop) return false;
    if (strcasecmp(prop, "Titre") == 0 || strcasecmp(prop, "Title") == 0) {
        ObjString* s = value_to_string(v);
        owl_ui_window_set_title(w, s ? s->chars : "");
        return true;
    }
    if (strcasecmp(prop, "Largeur") == 0 || strcasecmp(prop, "Width") == 0) {
        if (IS_INTEGER(v)) {
            owl_ui_window_set_size(w, (int)AS_INTEGER(v),
                                    owl_ui_window_get_height(w));
        }
        return true;
    }
    if (strcasecmp(prop, "Hauteur") == 0 || strcasecmp(prop, "Height") == 0) {
        if (IS_INTEGER(v)) {
            owl_ui_window_set_size(w, owl_ui_window_get_width(w),
                                    (int)AS_INTEGER(v));
        }
        return true;
    }
    if (strcasecmp(prop, "X") == 0) {
        if (IS_INTEGER(v)) {
            owl_ui_window_set_position(w, (int)AS_INTEGER(v),
                                        owl_ui_window_get_y(w));
        }
        return true;
    }
    if (strcasecmp(prop, "Y") == 0) {
        if (IS_INTEGER(v)) {
            owl_ui_window_set_position(w, owl_ui_window_get_x(w),
                                        (int)AS_INTEGER(v));
        }
        return true;
    }
    if (strcasecmp(prop, "Visible") == 0) {
        bool b = IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v);
        owl_ui_window_set_visible(w, b ? 1 : 0);
        return true;
    }
    if (strcasecmp(prop, "Grise") == 0 || strcasecmp(prop, "Disabled") == 0) {
        bool b = IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v);
        owl_ui_window_set_disabled(w, b ? 1 : 0);
        return true;
    }
    if (strcasecmp(prop, "Icone") == 0 || strcasecmp(prop, "Icon") == 0) {
        window_set_icon_value(w, v);
        return true;
    }
    if (strcasecmp(prop, "Redimensionnable") == 0 ||
        strcasecmp(prop, "Resizable") == 0) {
        bool b = IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v);
        owl_ui_window_set_resizable(w, b ? 1 : 0);
        return true;
    }
    if (strcasecmp(prop, "CouleurFond") == 0 ||
        strcasecmp(prop, "BackgroundColor") == 0) {
        /*  — accepte entier nu OU instance Couleur (alpha porté). */
        int wbBgr = -1, wbAlpha = 255;
        owl_value_to_color(v, &wbBgr, &wbAlpha);
        if (wbBgr >= 0) owl_ui_window_set_bg_color_alpha(w, wbBgr, wbAlpha);
        return true;
    }
    if (strcasecmp(prop, "Style") == 0) {
        if (IS_INSTANCE(v)) apply_style_to_window(w, AS_INSTANCE(v));
        return true;
    }
    if (strcasecmp(prop, "ValeurRenvoyee") == 0 ||
        strcasecmp(prop, "ReturnValue") == 0) {
        g_return_value = v;
        return true;
    }
    return false;
}

/*  lot C — hook owl_ui_apply_window_scalar.
 * Symetrique strict d'apply_instance_scalar_impl : delegue au helper Qt
 * en castant wi->widget en OwlWindow*. Pas de feedback si la prop est
 * inconnue de Qt (le store reste la verite, comme pour les sous-controles). */
/*  — construction de la barre de menu (definie avec le reste du
 * pipeline fenetre, plus bas). */
static bool build_window_main_menu(OwlWindow* w, const OwlWindowDecl* decl,
                                    const char* menu_name);

static void apply_window_scalar_impl(ObjWindowInstance* wi,
                                     const char* prop, Value v) {
    if (!wi || !prop) return;
    /*  — bascule runtime du menu classique : `FEN_X..MenuPrincipal =
     * MNU_Autre` reconstruit la barre entiere par le meme chemin que
     * l'ouverture (surSelection re-cables, ancienne barre detruite par Qt).
     * Traite meme sans widget monte (headless) : la resolution du nom doit
     * rester bruyante si l'arbre vise n'existe pas. */
    if (strcasecmp(prop, "MenuPrincipal") == 0 || strcasecmp(prop, "MainMenu") == 0) {
        if (wi->widget == NULL) {
            /* Pas de fenetre Qt montee : on valide seulement la cible. */
            const OwlWindowDecl* decl = wi->decl;
            const char* want = IS_STRING(v) ? AS_CSTRING(v) : NULL;
            if (decl && want && *want && owl_window_decl_find_menu(decl, want) == NULL) {
                fprintf(stderr,
                        "[ui] Fenetre '%s' : menu principal '%s' introuvable "
                        "(aucun bloc MenuPrincipal de ce nom).\n",
                        decl->name ? decl->name : "?", want);
            }
            return;
        }
        build_window_main_menu((OwlWindow*)wi->widget, wi->decl,
                               IS_STRING(v) ? AS_CSTRING(v) : NULL);
        return;
    }
    if (!wi->widget) return;
    window_set_prop_qt((OwlWindow*)wi->widget, prop, v);
}

/*  — hook owl_ui_window_live_get, pendant fenetre du Qt-first des
 * champs ( pour la valeur,  pour la geometrie).
 *
 * La taille d'une fenetre montee n'est pas celle de son fichier de
 * description : la disposition la pousse a la taille minimale que ses champs
 * exigent, et une taille ecrite peut etre refusee pour la meme raison. Le
 * store ne l'apprend jamais, donc la relecture rendait une valeur que
 * personne ne pouvait observer a l'ecran.
 *
 * La synchro prealable joue le meme role que pour un champ : sans elle une
 * lecture faite depuis FinInitialisation rend une taille provisoire, et le
 * meme programme rend deux valeurs selon le moment de la lecture. */
static bool window_live_get_impl(ObjWindowInstance* wi, const char* prop,
                                 Value* out) {
    if (!wi || !wi->widget || !prop || !out) return false;
    bool isW = (strcasecmp(prop, "Largeur") == 0 ||
                strcasecmp(prop, "Width") == 0);
    bool isH = (strcasecmp(prop, "Hauteur") == 0 ||
                strcasecmp(prop, "Height") == 0);
    bool isX = (strcasecmp(prop, "X") == 0);
    bool isY = (strcasecmp(prop, "Y") == 0);
    if (!isW && !isH && !isX && !isY) return false;

    OwlWindow* w = (OwlWindow*)wi->widget;
    owl_ui_window_sync_layout(w);
    if (isW)      *out = INTEGER_VAL(owl_ui_window_get_width(w));
    else if (isH) *out = INTEGER_VAL(owl_ui_window_get_height(w));
    else if (isX) *out = INTEGER_VAL(owl_ui_window_get_x(w));
    else          *out = INTEGER_VAL(owl_ui_window_get_y(w));
    return true;
}

/* Lot Cadre Phase 4 : helper qui lit un sous-objet Coin* (Largeur/Hauteur)
 * d'une instance Cadre. Renvoie 0 si absent. */
static void read_coin(ObjInstance* cadre, const char* coinName, int* w_, int* h_) {
    *w_ = 0; *h_ = 0;
    Value v;
    if (!cadre) return;
    ObjString* k = obj_copy_string(coinName, (int)strlen(coinName));
    if (table_get(&cadre->fields, k, &v) && IS_INSTANCE(v)) {
        ObjInstance* coin = AS_INSTANCE(v);
        *w_ = instance_get_int(coin, "Largeur", 0);
        *h_ = instance_get_int(coin, "Hauteur", 0);
    }
}

/* Idem pour les sous-objets Trait* (Couleur/Epaisseur/Type).  —
 * Couleur peut être une instance Couleur (alpha porté) : instance_get_color
 * extrait bgr + alpha (l'entier nu reste opaque 255, byte-identique). L'entier
 * absent -> 0 (noir opaque). */
static void read_trait(ObjInstance* cadre, const char* traitName,
                        int* color, int* thick, int* type, int* alpha) {
    *color = 0; *thick = 0; *type = 0; *alpha = 255;
    Value v;
    if (!cadre) return;
    ObjString* k = obj_copy_string(traitName, (int)strlen(traitName));
    if (table_get(&cadre->fields, k, &v) && IS_INSTANCE(v)) {
        ObjInstance* tr = AS_INSTANCE(v);
        instance_get_color(tr, "Couleur", color, alpha);
        if (*color < 0) *color = 0;   /* champ absent -> noir opaque (défaut) */
        *thick = instance_get_int(tr, "Epaisseur", 0);
        *type  = instance_get_int(tr, "Type",      0);
    }
}

/* Applique une instance Cadre composee a TOUT widget (canon).
 * Lit les 5 sous-champs scalaires + 4 Coin* + 4 Trait* et appelle le
 * helper Qt6 unifie owl_ui_widget_set_cadre. */
/*  — remplit un OwlCadreSpec depuis une instance Cadre (sans
 * l'appliquer). Extrait d'apply_cadre_instance_to_widget pour reutilisation
 * par les fragments d'etat (Style[etat]..Cadre). */
static void cadre_instance_to_spec(ObjInstance* cadre, OwlCadreSpec* spec) {
    /*  — Couleur peut être une instance Couleur (alpha porté) :
     * instance_get_color extrait bgr + alpha, l'entier nu reste opaque (255,
     * byte-identique). instance_get_int rendait 0 (noir) sur une instance. */
    spec->color = 0; spec->color_alpha = 255;
    instance_get_color(cadre, "Couleur", &spec->color, &spec->color_alpha);
    if (spec->color < 0) spec->color = 0;  /* champ absent -> noir opaque (défaut canon) */
    spec->thickness     = instance_get_int(cadre, "Epaisseur", 0);
    spec->line_style    = instance_get_int(cadre, "Trait", OWL_TC_CONTINU);
    spec->corner_radius = instance_get_int(cadre, "Arrondi",   0);
    spec->edges         = instance_get_int(cadre, "Bords",    15);
    read_trait(cadre, "TraitHaut",
               &spec->trait_haut_color, &spec->trait_haut_thick,
               &spec->trait_haut_type, &spec->trait_haut_alpha);
    read_trait(cadre, "TraitBas",
               &spec->trait_bas_color, &spec->trait_bas_thick,
               &spec->trait_bas_type, &spec->trait_bas_alpha);
    read_trait(cadre, "TraitGauche",
               &spec->trait_gauche_color, &spec->trait_gauche_thick,
               &spec->trait_gauche_type, &spec->trait_gauche_alpha);
    read_trait(cadre, "TraitDroite",
               &spec->trait_droite_color, &spec->trait_droite_thick,
               &spec->trait_droite_type, &spec->trait_droite_alpha);
    read_coin(cadre, "CoinHautGauche", &spec->coin_hg_w, &spec->coin_hg_h);
    read_coin(cadre, "CoinHautDroite", &spec->coin_hd_w, &spec->coin_hd_h);
    read_coin(cadre, "CoinBasGauche",  &spec->coin_bg_w, &spec->coin_bg_h);
    read_coin(cadre, "CoinBasDroite",  &spec->coin_bd_w, &spec->coin_bd_h);
}

static void apply_cadre_instance_to_widget(OwlControl* w, ObjInstance* cadre) {
    if (!w || !cadre) return;
    OwlCadreSpec spec = {0};
    cadre_instance_to_spec(cadre, &spec);
    owl_ui_widget_set_cadre(w, &spec);
}

/*  lot 2/3 — watcher direct instance composee -> OwlControl*.
 *
 * Source de verite unique : le store par-instance de l'ObjUiControlInstance
 * (Cadre/Police materialises sous leur cle canon). Pour re-appliquer a Qt sur
 * un chained set (LIB_X..Cadre..Epaisseur = 3), on relie ici l'arbre composite
 * a son OwlControl*, alimente a la materialisation du composite (lorsque
 * uici->widget != NULL). owl_ui_on_instance_modified consulte cette map.
 *
 * Allocation dynamique (grow par realloc, plus de drop silencieux) : un ecran a
 * beaucoup de widgets styles peut tagger largement plus de 256 sous-instances
 * (un Saisie style tague ~80 instances recursives). Un drop silencieux
 * empechait le re-apply du style sur les instances au-dela du seuil. Lookup
 * lineaire (chaque widget a typiquement 1-3 sous-objets a tracker). Le buffer
 * est conserve entre cycles VM (seul le count est remis a 0 par stdlib_ui_init) :
 * les entrees sont des pointeurs bruts (instances maintenues vivantes par le
 * headless_store, pas par cette table), le realloc ne casse aucun pointeur
 * detenu ailleurs (tout acces ici est indexe). */
typedef struct {
    ObjInstance* inst;
    OwlControl*  widget;
} InstWidgetWatch;

static InstWidgetWatch* g_inst_widget_watches = NULL;
static int              g_inst_widget_watch_count = 0;
static int              g_inst_widget_watch_cap = 0;

static void inst_widget_watch_add(ObjInstance* inst, OwlControl* widget) {
    if (!inst || !widget) return;
    for (int i = 0; i < g_inst_widget_watch_count; i++) {
        if (g_inst_widget_watches[i].inst == inst) {
            g_inst_widget_watches[i].widget = widget;
            return;
        }
    }
    if (g_inst_widget_watch_count >= g_inst_widget_watch_cap) {
        int newCap = g_inst_widget_watch_cap < 256 ? 256 : g_inst_widget_watch_cap * 2;
        InstWidgetWatch* grown = (InstWidgetWatch*)realloc(
            g_inst_widget_watches, (size_t)newCap * sizeof(InstWidgetWatch));
        if (!grown) return;  /* OOM — degrade sans crash (comme l'ancien drop) */
        g_inst_widget_watches = grown;
        g_inst_widget_watch_cap = newCap;
    }
    g_inst_widget_watches[g_inst_widget_watch_count].inst   = inst;
    g_inst_widget_watches[g_inst_widget_watch_count].widget = widget;
    g_inst_widget_watch_count++;
}

/*  — map widget -> instance styleSaisie racine. Permet, sur mutation
 * d'un overlay d'etat (Style[survol]/Style[focus] post-ouverture), de rejouer
 * la feuille de style COMPLETE depuis la racine (les etats sont des fragments
 * de la meme feuille unique, cf. ). Sans ca, un set post-open sur un
 * slot d'etat ne repeindrait pas. */
typedef struct {
    OwlControl*  widget;
    ObjInstance* root;   /* instance styleSaisie racine */
} StyleRootWatch;

/* Meme discipline dynamique que g_inst_widget_watches (grow, pas de drop). */
static StyleRootWatch* g_style_root_watches = NULL;
static int             g_style_root_watch_count = 0;
static int             g_style_root_watch_cap = 0;

static void style_root_record(OwlControl* widget, ObjInstance* root) {
    if (!widget || !root) return;
    for (int i = 0; i < g_style_root_watch_count; i++) {
        if (g_style_root_watches[i].widget == widget) {
            g_style_root_watches[i].root = root;
            return;
        }
    }
    if (g_style_root_watch_count >= g_style_root_watch_cap) {
        int newCap = g_style_root_watch_cap < 256 ? 256 : g_style_root_watch_cap * 2;
        StyleRootWatch* grown = (StyleRootWatch*)realloc(
            g_style_root_watches, (size_t)newCap * sizeof(StyleRootWatch));
        if (!grown) return;  /* OOM — degrade sans crash */
        g_style_root_watches = grown;
        g_style_root_watch_cap = newCap;
    }
    g_style_root_watches[g_style_root_watch_count].widget = widget;
    g_style_root_watches[g_style_root_watch_count].root   = root;
    g_style_root_watch_count++;
}

static ObjInstance* style_root_lookup(OwlControl* widget) {
    for (int i = 0; i < g_style_root_watch_count; i++) {
        if (g_style_root_watches[i].widget == widget)
            return g_style_root_watches[i].root;
    }
    return NULL;
}

static OwlControl* inst_widget_watch_lookup(ObjInstance* inst) {
    for (int i = 0; i < g_inst_widget_watch_count; i++) {
        if (g_inst_widget_watches[i].inst == inst)
            return g_inst_widget_watches[i].widget;
    }
    return NULL;
}

/*  — purge toutes les entrees de watch (instance->widget ET
 * widget->racine style) ciblant `widget`. Appele a la fermeture d'une fenetre
 * (fn_ferme) AVANT que les QWidget soient detruits : sans cela, une entree
 * obsolete pointe un widget libere, et la moindre mutation d'instance dont
 * l'adresse est reutilisee ferait deref ce widget mort (use-after-free non
 * deterministe). Corrige aussi le risque latent pre-existant sur Cadre/Police. */
static void widget_watches_purge(OwlControl* widget) {
    if (!widget) return;
    int j = 0;
    for (int i = 0; i < g_inst_widget_watch_count; i++) {
        if (g_inst_widget_watches[i].widget != widget) {
            g_inst_widget_watches[j++] = g_inst_widget_watches[i];
        }
    }
    g_inst_widget_watch_count = j;
    j = 0;
    for (int i = 0; i < g_style_root_watch_count; i++) {
        if (g_style_root_watches[i].widget != widget) {
            g_style_root_watches[j++] = g_style_root_watches[i];
        }
    }
    g_style_root_watch_count = j;
}

/* Tagge recursivement une instance composee + ses sous-objets (Coin/Trait
 * pour Cadre) vers le meme widget. */
static void inst_widget_watch_tag_recursive(ObjInstance* inst, OwlControl* widget) {
    if (!inst) return;
    inst_widget_watch_add(inst, widget);
    for (int i = 0; i < inst->fields.capacity; i++) {
        Entry* e = &inst->fields.entries[i];
        if (e->key == NULL) continue;
        if (IS_INSTANCE(e->value)) {
            inst_widget_watch_tag_recursive(AS_INSTANCE(e->value), widget);
        }
    }
}

/*  — soft-error partagee GUI/headless : coche -1 sur une option qui ne
 * gere pas trois etats. Pose ErreurDetectee, le programme continue, la coche
 * reste inchangee. */
static void interrupteur_option_tristate_error(int idx) {
    char idxBuf[16];
    snprintf(idxBuf, sizeof idxBuf, "%d", idx);
    owl_emit_native_error(E_RT_UI_OPTION_TRISTATE_REQUIRED,
                          "Champ Interrupteur",
                          "control", "Champ Interrupteur",
                          "index", idxBuf, NULL);
}

/*  lot 3 — hook appele par OP_STRUCT_SET (vm.c). Si l'instance modifiee
 * est reliee a un widget (composite Cadre/Police materialise dans le store
 * d'une instance montee), re-apply au QWidget hote. */
static void on_instance_modified_impl(ObjInstance* inst) {
    if (!inst) return;
    /*  lot 2/3 — liaison directe instance composee -> widget (Cadre/
     * Police materialises dans le store d'un sous-controle monte). Distinction
     * Cadre vs Police par structName de l'instance racine ; les sous-objets
     * (Coin/Trait) sont aussi taggues vers le meme widget. Un set direct sur
     * un sous-objet (Cadre..TraitBas..Epaisseur, dont le SET final est sur
     * l'objet Trait) n'a pas de structName Cadre/Police : pas de re-apply ici
     * (cas rare, comportement inchange depuis lot 2). */
    {
        OwlControl* direct = inst_widget_watch_lookup(inst);
        if (direct != NULL && inst->structName != NULL) {
            /* Le dispatch se fait sur le structName de l'instance MODIFIEE
             * (jamais deref du widget avant), calque sur le pattern Cadre/
             * Police : sans ce garde, une entree de watch obsolete (fenetre
             * fermee, adresse d'instance reutilisee) ferait deref un widget
             * libere pour toute mutation d'instance du programme. */
            const char* sn = inst->structName->chars;
            if (strcasecmp(sn, "Police") == 0) {
                apply_police_instance_to_widget(direct, inst);
                return;
            }
            if (strcasecmp(sn, "Cadre") == 0) {
                apply_cadre_instance_to_widget(direct, inst);
                return;
            }
            /* +0837 — mutation d'un noeud du modele styleSaisie (racine
             * OU sous-element ZoneSaisie/Libellé/CadreExtérieur/Etats/surcharge
             * plate d'etat) : rejoue la feuille COMPLETE depuis la racine (etats
             * = fragments de la feuille unique, contrainte ). Tous les
             * noms de type du modele commencent par "style" ; Police/Cadre sont
             * deja traites au-dessus (return anticipe). */
            if (strncasecmp(sn, "style", 5) == 0) {
                ObjInstance* styleRoot = style_root_lookup(direct);
                if (styleRoot != NULL) {
                    int kind = owl_ui_control_get_widget_kind(direct);
                    if (kind < 0) kind = OWL_CONTROL_INPUT;
                    apply_style_to_control(direct, styleRoot, (OwlControlKind)kind);
                }
                return;
            }
        }
    }
    /* S.4 — Selecteur option proxy : detecte les fields __sel_widget_ptr +
     * __sel_index, dispatche les nouveaux Libelle/SousLibelle/ValeurRenvoyee
     * vers le widget. */
    if (inst->structName &&
        strcmp(inst->structName->chars, "SelecteurOption") == 0) {
        Value vIdx, vPtr;
        if (table_get(&inst->fields, obj_copy_string("__sel_index", 11), &vIdx) &&
            table_get(&inst->fields, obj_copy_string("__sel_widget_ptr", 16), &vPtr) &&
            IS_INTEGER(vIdx) && IS_INTEGER(vPtr)) {
            int idx = (int)AS_INTEGER(vIdx);
            OwlControl* w = (OwlControl*)(intptr_t)AS_INTEGER(vPtr);
            Value vLib, vSub, vVal;
            if (table_get(&inst->fields, obj_copy_string("Libelle", 7), &vLib) &&
                IS_STRING(vLib))
                owl_ui_selecteur_set_option_label(w, idx, AS_CSTRING(vLib));
            if (table_get(&inst->fields, obj_copy_string("SousLibelle", 11), &vSub) &&
                IS_STRING(vSub))
                owl_ui_selecteur_set_option_sub_label(w, idx, AS_CSTRING(vSub));
            if (table_get(&inst->fields, obj_copy_string("ValeurRenvoyee", 14), &vVal) &&
                IS_STRING(vVal))
                owl_ui_selecteur_set_option_stored(w, idx, AS_CSTRING(vVal));
        }
        return;
    }
    /* I.4 — Interrupteur option proxy : pareil avec back-ref __int_*.
     *  — deux regimes selon la presence de __int_widget_ptr :
     *   - GUI (proxy ephemere avec widget) : le widget Qt fait foi, dispatch des
     *     nouveaux Libelle/SousLibelle/Valeur/TroisEtats vers lui ;
     *   - Headless (instance vivante du store) : les casiers __int_coche /
     *     __int_tristate font foi, on les synchronise depuis les champs visibles.
     * Dans les deux cas, la coche indeterminee (-1) exige le mode TroisEtats de
     * CETTE option : sinon soft-error (ErreurDetectee) et coche inchangee. */
    if (inst->structName &&
        strcmp(inst->structName->chars, "InterrupteurOption") == 0) {
        Value vIdx, vPtr;
        bool hasWidget =
            table_get(&inst->fields, obj_copy_string("__int_index", 11), &vIdx) &&
            table_get(&inst->fields, obj_copy_string("__int_widget_ptr", 16), &vPtr) &&
            IS_INTEGER(vIdx) && IS_INTEGER(vPtr);
        if (hasWidget) {
            int idx = (int)AS_INTEGER(vIdx);
            OwlControl* w = (OwlControl*)(intptr_t)AS_INTEGER(vPtr);
            Value vLib, vSub, vVal, vTri;
            if (table_get(&inst->fields, obj_copy_string("Libelle", 7), &vLib) &&
                IS_STRING(vLib))
                owl_ui_interrupteur_set_option_label(w, idx, AS_CSTRING(vLib));
            if (table_get(&inst->fields, obj_copy_string("SousLibelle", 11), &vSub) &&
                IS_STRING(vSub))
                owl_ui_interrupteur_set_option_sub_label(w, idx, AS_CSTRING(vSub));
            /* TroisEtats avant Valeur : le mode conditionne l'acceptation de -1. */
            if (table_get(&inst->fields, obj_copy_string("TroisEtats", 10), &vTri) &&
                IS_BOOL(vTri))
                owl_ui_interrupteur_set_option_three_states(w, idx,
                                                            AS_BOOL(vTri) ? 1 : 0);
            if (table_get(&inst->fields, obj_copy_string("Valeur", 6), &vVal)) {
                int state;
                if (IS_BOOL(vVal))         state = AS_BOOL(vVal) ? 1 : 0;
                else if (IS_INTEGER(vVal)) state = (int)AS_INTEGER(vVal);
                else                       state = 0;
                if (state == -1 &&
                    owl_ui_interrupteur_get_option_three_states(w, idx) == 0) {
                    interrupteur_option_tristate_error(idx);
                } else {
                    owl_ui_interrupteur_set_option_state(w, idx, state);
                }
            }
            return;
        }
        /* Headless : casiers de verite __int_coche / __int_tristate. Le champ
         * visible qui vient d'etre ecrit differe de son casier ; les autres ont
         * ete rafraichis a l'identique par la lecture indexee qui precede toute
         * ecriture indexee (option_headless_refresh_coche). Synchroniser tous les
         * casiers depuis les champs propage donc le seul reel changement. */
        Value vTri, vVal;
        bool tristate = false;
        if (table_get(&inst->fields, obj_copy_string("TroisEtats", 10), &vTri) &&
            IS_BOOL(vTri))
            tristate = AS_BOOL(vTri);
        inst_field_set_bool(inst, "__int_tristate", tristate);
        if (table_get(&inst->fields, obj_copy_string("Valeur", 6), &vVal)) {
            int state;
            if (IS_BOOL(vVal))         state = AS_BOOL(vVal) ? 1 : 0;
            else if (IS_INTEGER(vVal)) state = (int)AS_INTEGER(vVal);
            else                       state = 0;
            if (state == -1 && !tristate) {
                int idx = 0;
                Value vI;
                if (table_get(&inst->fields, obj_copy_string("__int_index", 11), &vI)
                    && IS_INTEGER(vI))
                    idx = (int)AS_INTEGER(vI);
                interrupteur_option_tristate_error(idx);
            } else {
                inst_field_set_int(inst, "__int_coche", state);
            }
        }
        return;
    }

}

/* Applique une instance Police composee a tout widget. Lit Nom/Taille/
 * Gras/Italique/Souligne/Barre/Couleur. Sous-champs accentues (Souligné,
 * Barré) ont aussi une forme ASCII (Souligne, Barre) acceptee — on tente
 * les 2 via le scanner accent-strip. */
/*  — une Police est au defaut INTEGRAL quand l'utilisateur n'a pose
 * aucune de ses props (Nom vide/0, Taille <= 0, Gras/Italique/Souligne/Barre
 * faux, Couleur <= 0). Le chemin Style seed toujours une brique Libellé porteuse
 * d'une Police, meme quand seul un CadreExtérieur est defini : appliquer cette
 * Police-defaut re-set le QFont du widget, ce qui retaille sa geometrie via
 * le sizeHint base-texte du QStyleSheetStyle. On saute donc l'application quand
 * rien n'est pose : la fonte reste celle du widget/theme, geometrie identique au
 * chemin plat.
 *
 *  — ce test etait inoperant : la brique semait Taille=11, donc
 * `Taille > 0` etait TOUJOURS vrai et la garde ne se declenchait jamais. Mesure
 * (Libellé 15 -> 17, Saisie 23 -> 25, Sélecteur/Interrupteur 6 options
 * 138 -> 150) : poser un style qui ne parle que de couleur retaillait tous les
 * champs texte, et ecrasait au passage un Gras/Italique pose a plat (largeur du
 * libellé 114 -> 109). La brique seme desormais Taille=0
 * (build_Police_surcharge_template), sentinelle hors-domaine : la garde
 * fonctionne, et une taille posee par l'utilisateur reste toujours > 0. */
static bool police_instance_is_default(ObjInstance* police) {
    if (!police) return true;
    ObjString* nomK = obj_copy_string("Nom", 3);
    Value nomV;
    if (table_get(&police->fields, nomK, &nomV)) {
        if (IS_INTEGER(nomV) && AS_INTEGER(nomV) != 0) return false;
        if (IS_STRING(nomV) && AS_STRING(nomV)->length > 0) return false;
    }
    if (instance_get_int(police, "Taille", 0) > 0) return false;
    if (instance_get_bool(police, "Gras",     false)) return false;
    if (instance_get_bool(police, "Italique", false)) return false;
    if (instance_get_bool(police, "Souligne", false)) return false;
    if (instance_get_bool(police, "Barre",    false)) return false;
    if (instance_get_int(police, "Couleur", -1) > 0) return false;  /* color-int-ok: check booléen "la police a-t-elle une couleur ?", pas un rendu */
    return true;
}

static void apply_police_instance_to_widget(OwlControl* w, ObjInstance* police) {
    if (!w || !police) return;
    /*  — Police au defaut integral : ne rien appliquer (sinon re-set du
     * QFont -> retaille le QPushButton stylé vs le chemin plat). */
    if (police_instance_is_default(police)) return;
    /* Nom peut etre stocke soit comme chaine ("Arial", "Liberation Sans"),
     * soit comme sentinelle entiere iPolice* (cascade/declaratif : la constante
     * fold en entier). On transmet la valeur sous forme de chaine a
     * owl_ui_control_set_font_name qui resout les sentinelles vers la fonte
     * embarquee avant setFamily. */
    {
        ObjString* nomK = obj_copy_string("Nom", 3);
        Value nomV;
        if (table_get(&police->fields, nomK, &nomV)) {
            if (IS_INTEGER(nomV)) {
                char sbuf[32];
                snprintf(sbuf, sizeof(sbuf), "%lld", (long long)AS_INTEGER(nomV));
                owl_ui_control_set_font_name(w, sbuf);
            } else if (IS_STRING(nomV) && AS_STRING(nomV)->length > 0) {
                owl_ui_control_set_font_name(w, AS_CSTRING(nomV));
            }
        }
    }
    int taille = instance_get_int(police, "Taille", 0);
    if (taille > 0) owl_ui_control_set_font_size(w, taille);
    owl_ui_control_set_font_bold     (w, instance_get_bool(police, "Gras",     false) ? 1 : 0);
    owl_ui_control_set_font_italic   (w, instance_get_bool(police, "Italique", false) ? 1 : 0);
    /* Souligne / Souligné — table_set stocke la forme ASCII via le scanner
     * accent-strip a la lookup, donc l'un OU l'autre marche. */
    owl_ui_control_set_font_underline(w, instance_get_bool(police, "Souligne", false) ? 1 : 0);
    owl_ui_control_set_font_strikeout(w, instance_get_bool(police, "Barre",    false) ? 1 : 0);
    /* Couleur du texte. La couleur n'est pas un attribut QFont — elle passe
     * par le stylesheet (owl_fg / apply_color_stylesheet), seul chemin de
     * couleur de texte pour Bouton/Saisie qui n'exposent pas de ..Couleur
     * directe.
     *
     *  — garde `> 0` (et non `>= 0`) : dans une brique de Style, la
     * Police imbriquee seed sa Couleur a 0 (=noir) des qu'on touche un autre
     * sous-champ (Taille/Gras...) ; appliquee telle quelle elle ecrasait la
     * Couleur directe de la ZoneSaisie (`..Couleur`) et le clobber se rejouait
     * via le watcher de mutation (on_instance_modified). Une Couleur de police
     * a 0 est indistinguable du seed dans ce modele ; on la traite comme "non
     * posee" — le texte reste noir par le defaut du widget/theme de toute
     * facon. Une Police..Couleur explicite (> 0) prime toujours. */
    int couleur = -1, couleurAlpha = 255;
    instance_get_color(police, "Couleur", &couleur, &couleurAlpha);
    if (couleur > 0) owl_ui_control_set_text_color_alpha(w, couleur, couleurAlpha);
}

/*  — un Cadre est "peignant" s'il produit un trait visible (epaisseur
 * globale ou per-bord > 0) ou des coins arrondis. Le template Cadre par defaut
 * (tout a 0) ne peint RIEN, mais le pousser via owl_ui_widget_set_cadre emet
 * quand meme un setStyleSheet ("* { border: 0 none }") qui REMPLACE integralement
 * la feuille de style du widget — donc ECRASE la couleur de fond (owl_bg) posee
 * juste avant. On evite d'appliquer un Cadre invisible pour ne pas clobbrer le
 * fond d'un Style qui ne definit qu'une CouleurFond (pas de trait). */
static bool cadre_instance_is_visible(ObjInstance* cadre) {
    if (!cadre) return false;
    if (instance_get_int(cadre, "Epaisseur", 0) > 0) return true;
    if (instance_get_int(cadre, "Arrondi",   0) > 0) return true;
    int c, t, ty, a;
    read_trait(cadre, "TraitHaut",   &c, &t, &ty, &a); if (t > 0) return true;
    read_trait(cadre, "TraitBas",    &c, &t, &ty, &a); if (t > 0) return true;
    read_trait(cadre, "TraitGauche", &c, &t, &ty, &a); if (t > 0) return true;
    read_trait(cadre, "TraitDroite", &c, &t, &ty, &a); if (t > 0) return true;
    int cw, ch;
    read_coin(cadre, "CoinHautGauche", &cw, &ch); if (cw > 0 || ch > 0) return true;
    read_coin(cadre, "CoinHautDroite", &cw, &ch); if (cw > 0 || ch > 0) return true;
    read_coin(cadre, "CoinBasGauche",  &cw, &ch); if (cw > 0 || ch > 0) return true;
    read_coin(cadre, "CoinBasDroite",  &cw, &ch); if (cw > 0 || ch > 0) return true;
    return false;
}

/* +0837 — applique un overlay d'etat (survol/focus/grisé/lectureSeule)
 * lu depuis la collection Etats de la ZoneSaisie : lit TOUTES les props du slot
 * (Couleur, CouleurFond, Police Gras/Italique, Retraits, Cadre) et emet un
 * fragment QSS unique via owl_ui_control_set_state_style sur le QLineEdit.
 * stateIdx : 0 = survol, 1 = focus, 2 = grisé, 3 = lectureSeule. Un slot
 * absent/vide reset le fragment (setter appele avec tout unset). Le CurseurSouris
 * (survol) reste applique sur le widget (setCursor, hors feuille QSS). */
static void apply_style_state(OwlControl* w, ObjInstance* etats,
                              const char* slotName, int stateIdx) {
    ObjInstance* slot = instance_get_instance(etats, slotName);
    OwlStateTextStyle t = { -1, 255, -1, 255, -1, -1, 0, 0, 0, 0, 0 };
    OwlCadreSpec border = {0};
    const OwlCadreSpec* borderPtr = NULL;
    if (slot) {
        instance_get_color(slot, "Couleur", &t.fg, &t.fgAlpha);
        /*  — CouleurFond peut être une instance Couleur (alpha porté). */
        instance_get_color(slot, "CouleurFond", &t.bg, &t.bgAlpha);
        ObjInstance* pol = instance_get_instance(slot, "Police");
        if (pol) {
            if (instance_get_bool(pol, "Gras", false))     t.bold = 1;
            if (instance_get_bool(pol, "Italique", false)) t.italic = 1;
        }
        int rg = instance_get_int(slot, "RetraitGauche", 0);
        int rd = instance_get_int(slot, "RetraitDroit",  0);
        int rh = instance_get_int(slot, "RetraitHaut",   0);
        int rb = instance_get_int(slot, "RetraitBas",    0);
        if (rg > 0) t.pad_l = rg;
        if (rd > 0) t.pad_r = rd;
        if (rh > 0) t.pad_t = rh;
        if (rb > 0) t.pad_b = rb;
        ObjInstance* cadre = instance_get_instance(slot, "Cadre");
        if (cadre && cadre_instance_is_visible(cadre)) {
            cadre_instance_to_spec(cadre, &border);
            borderPtr = &border;
        }
        /* CurseurSouris de survol : hors QSS, applique directement. */
        if (stateIdx == 0) {
            int hCur = instance_get_int(slot, "CurseurSouris", -1);
            if (hCur != -1) owl_ui_control_set_cursor_shape(w, hCur);
        }
    }
    owl_ui_control_set_state_style(w, stateIdx, &t, borderPtr);
}

/*  — applique une brique de props de base (Couleur, CouleurFond,
 * Retraits, CurseurSouris, Police, Cadre) sur un widget donne. Brique COMMUNE
 * partagee par les sous-elements ZoneSaisie (QLineEdit) et Libellé (QLabel). Le
 * Cadre passe par owl_ui_widget_set_cadre (`*`-scope sur le widget cible, sans
 * enfant) : le widget vise est toujours une feuille (QLineEdit/QLabel). */
static void apply_style_brick_to_widget(OwlControl* w, ObjInstance* brick,
                                        OwlControlKind kind) {
    if (!w || !brick) return;

    /* Couleur du texte. Defaut -1 = non posee ; garde `> 0` (et non `>= 0`)
     * comme la Police (apply_police_instance_to_widget) et les fragments d'etat.
     * Une brique de Style seed sa Couleur a 0 (=noir) des qu'un autre sous-champ
     * est touche (CouleurFond, Cadre...) ; appliquer ce 0 posait owl_fg=0 ->
     * `color: rgb(0,0,0)` dur dans la feuille, ce qui ecrase la couleur de texte
     * du theme en QSS-mode natif (police noire sur fond sombre). Un 0 est
     * indistinguable du seed ; on le traite comme non-pose et la couleur suit le
     * theme (palette applicative, ). Une Couleur explicite (> 0) prime. */
    int couleur = -1, couleurAlpha = 255;
    instance_get_color(brick, "Couleur", &couleur, &couleurAlpha);
    if (couleur > 0) owl_ui_control_set_text_color_alpha(w, couleur, couleurAlpha);

    /* Couleur de fond ; -1 = pas de fond (defaut).  — CouleurFond peut
     * être une instance Couleur (alpha porté) : instance_get_color extrait
     * bgr + alpha, l'entier nu reste opaque (byte-identique). */
    int couleurFond = -1, couleurFondAlpha = 255;
    instance_get_color(brick, "CouleurFond", &couleurFond, &couleurFondAlpha);
    if (couleurFond >= 0)
        owl_ui_control_set_bg_color_alpha(w, couleurFond, couleurFondAlpha);

    /* Marges interieures. */
    int rg = instance_get_int(brick, "RetraitGauche", 0);
    int rd = instance_get_int(brick, "RetraitDroit",  0);
    int rh = instance_get_int(brick, "RetraitHaut",   0);
    int rb = instance_get_int(brick, "RetraitBas",    0);
    if (rg > 0) owl_ui_control_set_margin_left  (w, rg);
    if (rd > 0) owl_ui_control_set_margin_right (w, rd);
    if (rh > 0) owl_ui_control_set_margin_top   (w, rh);
    if (rb > 0) owl_ui_control_set_margin_bottom(w, rb);

    /* CurseurSouris. Sentinelle OWL_CUR_* (zone negative). -1 = non defini. */
    int cur = instance_get_int(brick, "CurseurSouris", -1);
    if (cur != -1) owl_ui_control_set_cursor_shape(w, cur);

    /* Police imbriquee. */
    ObjInstance* police = instance_get_instance(brick, "Police");
    if (police) apply_police_instance_to_widget(w, police);

    /* Cadre imbrique. */
    if (kind == OWL_CONTROL_LABEL || kind == OWL_CONTROL_INPUT
        || kind == OWL_CONTROL_BUTTON) {
        ObjInstance* cadre = instance_get_instance(brick, "Cadre");
        if (cadre && cadre_instance_is_visible(cadre))
            apply_cadre_instance_to_widget(w, cadre);
    }
}

/*  — lit une brique texte (Couleur + CouleurFond + Police) d'une
 * instance de sous-element de style dans un OwlTextBrick. Sentinelles -1/0 =
 * non pose (calque du pattern Liste/Combo). `famVar` recoit le pointeur de
 * famille de police (dure tant que l'instance vit). */
static void read_text_brick(ObjInstance* elem, OwlTextBrick* out,
                            const char** famVar) {
    out->fg = -1; out->fg_alpha = 255; out->bg = -1; out->bg_alpha = 255;
    out->font_family = NULL; out->font_size = 0;
    out->bold = -1; out->italic = -1;
    if (!elem) return;
    /*  — Couleur/CouleurFond peuvent être une instance Couleur (alpha
     * porté). instance_get_int rendait -1 sur une instance -> couleur ignorée
     * (même cause racine que ). instance_get_color extrait (bgr, alpha) ;
     * l'entier nu reste opaque (byte-identique). */
    instance_get_color(elem, "Couleur", &out->fg, &out->fg_alpha);
    instance_get_color(elem, "CouleurFond", &out->bg, &out->bg_alpha);
    ObjInstance* police = instance_get_instance(elem, "Police");
    if (police && !police_instance_is_default(police)) {
        Value nomV;
        ObjString* nomK = obj_copy_string("Nom", 3);
        if (table_get(&police->fields, nomK, &nomV)
            && IS_STRING(nomV) && AS_STRING(nomV)->length > 0)
            *famVar = AS_CSTRING(nomV);
        out->font_family = *famVar;
        out->font_size = instance_get_int(police, "Taille", 0);
        out->bold   = instance_get_bool(police, "Gras",     false) ? 1 : 0;
        out->italic = instance_get_bool(police, "Italique", false) ? 1 : 0;
        int pc = -1, pcA = 255;
        instance_get_color(police, "Couleur", &pc, &pcA);
        if (pc > 0) { out->fg = pc; out->fg_alpha = pcA; }
    }
}

/*  — remplit un OwlTableStyleSpec depuis une instance styleTable.
 * Les pointeurs de famille de police sont conserves dans le tableau `fams`
 * (5 entrees : titre, lignes, selection, survol — index 0..3) fourni par le
 * caller pour que leur duree de vie couvre l'appel owl_ui_table_set_style. */
static void read_table_style(ObjInstance* style, OwlTableStyleSpec* spec,
                             OwlCadreSpec* extBorder, const char* fams[4]) {
    memset(spec, 0, sizeof(*spec));
    spec->ext_bg = -1; spec->ext_bg_alpha = 255;
    spec->paires_bg = -1; spec->impaires_bg = -1;
    spec->paires_bg_alpha = 255; spec->impaires_bg_alpha = 255;
    spec->quadrillage = -1; spec->quadrillage_alpha = 255; spec->ext_border = NULL;

    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (cadreExt) {
        /*  — CouleurFond peut être une instance Couleur (alpha porté). */
        instance_get_color(cadreExt, "CouleurFond", &spec->ext_bg, &spec->ext_bg_alpha);
        ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
        if (cadre && cadre_instance_is_visible(cadre)) {
            cadre_instance_to_spec(cadre, extBorder);
            spec->ext_border = extBorder;
        }
    }
    read_text_brick(instance_get_instance(style, "TitreColonne"),
                    &spec->titre, &fams[0]);
    read_text_brick(instance_get_instance(style, "LibelleLignes"),
                    &spec->lignes, &fams[1]);
    read_text_brick(instance_get_instance(style, "LigneSelectionnee"),
                    &spec->selection, &fams[2]);
    read_text_brick(instance_get_instance(style, "LigneSurvolee"),
                    &spec->survol, &fams[3]);
    /* LignesPaires/LignesImpaires : seul CouleurFond compte (peut porter l'alpha). */
    ObjInstance* paires = instance_get_instance(style, "LignesPaires");
    if (paires) instance_get_color(paires, "CouleurFond", &spec->paires_bg, &spec->paires_bg_alpha);
    ObjInstance* impaires = instance_get_instance(style, "LignesImpaires");
    if (impaires) instance_get_color(impaires, "CouleurFond", &spec->impaires_bg, &spec->impaires_bg_alpha);
    ObjInstance* quad = instance_get_instance(style, "Quadrillage");
    /*  — Quadrillage..Couleur peut être une instance Couleur (alpha
     * porté) : instance_get_color extrait bgr + alpha, le liseré translucide se
     * mélange au fond des lignes (gridline-color rgba). */
    if (quad) instance_get_color(quad, "Couleur", &spec->quadrillage, &spec->quadrillage_alpha);
}

/*  — remplit un OwlColumnStyleSpec depuis une instance styleColonne
 * (sous-ensemble du styleTable : pas de CadreExtérieur ni Quadrillage). */
static void read_column_style(ObjInstance* style, OwlColumnStyleSpec* spec,
                              const char* fams[4]) {
    memset(spec, 0, sizeof(*spec));
    spec->paires_bg = -1; spec->impaires_bg = -1;
    spec->paires_bg_alpha = 255; spec->impaires_bg_alpha = 255;
    read_text_brick(instance_get_instance(style, "TitreColonne"),
                    &spec->titre, &fams[0]);
    read_text_brick(instance_get_instance(style, "LibelleLignes"),
                    &spec->lignes, &fams[1]);
    read_text_brick(instance_get_instance(style, "LigneSelectionnee"),
                    &spec->selection, &fams[2]);
    read_text_brick(instance_get_instance(style, "LigneSurvolee"),
                    &spec->survol, &fams[3]);
    ObjInstance* paires = instance_get_instance(style, "LignesPaires");
    if (paires) instance_get_color(paires, "CouleurFond", &spec->paires_bg, &spec->paires_bg_alpha);
    ObjInstance* impaires = instance_get_instance(style, "LignesImpaires");
    if (impaires) instance_get_color(impaires, "CouleurFond", &spec->impaires_bg, &spec->impaires_bg_alpha);
}

/*  — remplit un OwlZrStyleSpec depuis une instance styleZoneRépétée.
 * Plus simple que la Table : pas de police par role a garder vivante ici (seuls
 * les fonds + couleurs de texte des etats + cadres comptent). Les OwlCadreSpec
 * (ext/inner) sont fournis par le caller pour couvrir la duree de l'appel. */
static void read_zr_style(ObjInstance* style, OwlZrStyleSpec* spec,
                          OwlCadreSpec* extBorder, OwlCadreSpec* innerBorder) {
    memset(spec, 0, sizeof(*spec));
    spec->ext_bg = -1; spec->ext_bg_alpha = 255;
    spec->inner_bg = -1; spec->inner_bg_alpha = 255;
    spec->impaires_bg = -1; spec->paires_bg = -1;
    spec->impaires_bg_alpha = 255; spec->paires_bg_alpha = 255;
    spec->selection_bg = -1; spec->selection_fg = -1;
    spec->selection_bg_alpha = 255; spec->selection_fg_alpha = 255;
    spec->survol_bg = -1; spec->survol_fg = -1;
    spec->survol_bg_alpha = 255; spec->survol_fg_alpha = 255;
    spec->separateur = -1; spec->separateur_alpha = 255;
    spec->ext_border = NULL; spec->inner_border = NULL;

    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (cadreExt) {
        /*  — CouleurFond peut être une instance Couleur (alpha porté). */
        instance_get_color(cadreExt, "CouleurFond", &spec->ext_bg, &spec->ext_bg_alpha);
        ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
        if (cadre && cadre_instance_is_visible(cadre)) {
            cadre_instance_to_spec(cadre, extBorder);
            spec->ext_border = extBorder;
        }
    }
    ObjInstance* cadreInt = instance_get_instance(style, "CadreInterne");
    if (cadreInt) {
        instance_get_color(cadreInt, "CouleurFond", &spec->inner_bg, &spec->inner_bg_alpha);
        ObjInstance* cadre = instance_get_instance(cadreInt, "Cadre");
        if (cadre && cadre_instance_is_visible(cadre)) {
            cadre_instance_to_spec(cadre, innerBorder);
            spec->inner_border = innerBorder;
        }
    }
    /*  — CouleurFond/Couleur peuvent être des instances Couleur (alpha
     * porté) : instance_get_color extrait bgr + alpha (entier nu opaque,
     * byte-identique). instance_get_int rendait -1 sur une instance. */
    ObjInstance* impaires = instance_get_instance(style, "LignesImpaires");
    if (impaires) instance_get_color(impaires, "CouleurFond",
                                     &spec->impaires_bg, &spec->impaires_bg_alpha);
    ObjInstance* paires = instance_get_instance(style, "LignesPaires");
    if (paires) instance_get_color(paires, "CouleurFond",
                                   &spec->paires_bg, &spec->paires_bg_alpha);
    ObjInstance* sel = instance_get_instance(style, "LigneSelectionnee");
    if (sel) {
        instance_get_color(sel, "CouleurFond",
                           &spec->selection_bg, &spec->selection_bg_alpha);
        instance_get_color(sel, "Couleur",
                           &spec->selection_fg, &spec->selection_fg_alpha);
    }
    ObjInstance* surv = instance_get_instance(style, "LigneSurvolee");
    if (surv) {
        instance_get_color(surv, "CouleurFond", &spec->survol_bg, &spec->survol_bg_alpha);
        instance_get_color(surv, "Couleur",     &spec->survol_fg, &spec->survol_fg_alpha);
    }
    ObjInstance* sep = instance_get_instance(style, "Separateur");
    if (sep) instance_get_color(sep, "Couleur",
                                &spec->separateur, &spec->separateur_alpha);
}

/*  — remplit un OwlJaugeStyleSpec depuis une instance styleJauge.
 * Trois sous-elements : CadreExtérieur (piste + bordure), Remplissage (barre :
 * CouleurFond) et Libellé (texte % : Couleur). extBorder est fourni par le
 * caller pour couvrir la duree de l'appel. Sentinelle -1 = feuille non posee ;
 * la couleur de texte suit la convention brique (>0 = posee, 0/noir = defaut). */
static void read_jauge_style(ObjInstance* style, OwlJaugeStyleSpec* spec,
                             OwlCadreSpec* extBorder) {
    memset(spec, 0, sizeof(*spec));
    spec->ext_bg = -1; spec->ext_bg_alpha = 255;
    spec->fill = -1; spec->fill_alpha = 255; spec->label_fg = -1;
    spec->label_fg_alpha = 255;
    spec->ext_border = NULL;
    spec->label_font_family = NULL; spec->label_font_size = 0;
    spec->label_bold = -1; spec->label_italic = -1;
    spec->label_underline = -1; spec->label_strike = -1;

    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (cadreExt) {
        /*  — CouleurFond peut être une instance Couleur (alpha porté). */
        instance_get_color(cadreExt, "CouleurFond", &spec->ext_bg, &spec->ext_bg_alpha);
        ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
        if (cadre && cadre_instance_is_visible(cadre)) {
            cadre_instance_to_spec(cadre, extBorder);
            spec->ext_border = extBorder;
        }
    }
    ObjInstance* rempl = instance_get_instance(style, "Remplissage");
    /*  — Remplissage..CouleurFond peut être une instance Couleur (alpha
     * porté) : le remplissage translucide se mélange à la piste (::chunk rgba). */
    if (rempl) instance_get_color(rempl, "CouleurFond", &spec->fill, &spec->fill_alpha);
    ObjInstance* lib = instance_get_instance(style, "Libelle");
    if (lib) {
        /*  — Libellé..Couleur peut être une instance Couleur (alpha
         * porté) : instance_get_color extrait bgr + alpha (entier nu opaque,
         * byte-identique). instance_get_int rendait -1 sur une instance. */
        int c = -1, cAlpha = 255;
        instance_get_color(lib, "Couleur", &c, &cAlpha);
        spec->label_fg = (c > 0) ? c : -1;   /* 0 = noir defaut, non pose */
        if (c > 0) spec->label_fg_alpha = cAlpha;
        /*  lot4 — Police du texte % (calque read_text_brick).
         * La famille pointe dans l'ObjString de la Police, vivante tant que
         * `style` l'est ; owl_ui_jauge_set_style (appele juste apres, meme
         * scope) recopie la chaine dans une QFont -> pas de holder externe. */
        ObjInstance* police = instance_get_instance(lib, "Police");
        if (police && !police_instance_is_default(police)) {
            Value nomV;
            ObjString* nomK = obj_copy_string("Nom", 3);
            if (table_get(&police->fields, nomK, &nomV)
                && IS_STRING(nomV) && AS_STRING(nomV)->length > 0)
                spec->label_font_family = AS_CSTRING(nomV);
            spec->label_font_size = instance_get_int(police, "Taille", 0);
            spec->label_bold      = instance_get_bool(police, "Gras",     false) ? 1 : 0;
            spec->label_italic    = instance_get_bool(police, "Italique", false) ? 1 : 0;
            spec->label_underline = instance_get_bool(police, "Souligne", false) ? 1 : 0;
            spec->label_strike    = instance_get_bool(police, "Barre",    false) ? 1 : 0;
            int pc = -1, pcAlpha = 255;
            instance_get_color(police, "Couleur", &pc, &pcAlpha);
            if (pc > 0) { spec->label_fg = pc; spec->label_fg_alpha = pcAlpha; }
        }
    }
}

/*  — extrait le sous-element CadreExtérieur d'un styleConteneur en une
 * spec plate (bg + bordure + retraits). Un seul sous-element : le conteneur est
 * un groupeur passif (pas de texte, pas d'etat). extBorder est fourni par le
 * caller pour couvrir la duree de l'appel. Sentinelles : bg -1 = pas de fond ;
 * pad -1 = retrait non pose ; ext_border NULL = pas de bordure. */
static void read_container_style(ObjInstance* style, int* bg, int* bgAlpha,
                                 OwlCadreSpec* extBorder,
                                 const OwlCadreSpec** extBorderPtr,
                                 int* padL, int* padT, int* padR, int* padB) {
    *bg = -1;
    *bgAlpha = 255;
    *extBorderPtr = NULL;
    *padL = *padT = *padR = *padB = -1;
    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (!cadreExt) return;
    /*  — CouleurFond du CadreExtérieur peut être une instance Couleur
     * (alpha porté) : instance_get_color extrait bgr + alpha (glassmorphisme). */
    instance_get_color(cadreExt, "CouleurFond", bg, bgAlpha);
    *padL = instance_get_int(cadreExt, "RetraitGauche", -1);
    *padR = instance_get_int(cadreExt, "RetraitDroit",  -1);
    *padT = instance_get_int(cadreExt, "RetraitHaut",   -1);
    *padB = instance_get_int(cadreExt, "RetraitBas",    -1);
    ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
    if (cadre && cadre_instance_is_visible(cadre)) {
        cadre_instance_to_spec(cadre, extBorder);
        *extBorderPtr = extBorder;
    }
}

/*  — applique l'ombre portée du widget entier depuis
 * style..CadreExtérieur..Ombre. Portée Phase 2 : CadreExtérieur SEUL, l'ombre
 * habille le widget principal `w`. Ombre totalement vide (aucun flou, décalage
 * ni étendue) => pas d'effet ; une ombre dure (Flou=0 + décalage) est rendue.
 * Réutilisable par tous les kinds mono-widget. */
/*  — applique une instance styleOmbre sur un widget cible arbitraire.
 * Factorisé d'apply_shadow_from_style : la source (CadreExtérieur ou ZoneSaisie)
 * et la cible (wrapper ou QLineEdit) sont désormais découplées. Ombre absente =
 * no-op ; ombre vide (tout à 0) => owl_ui_control_set_shadow retire l'effet. */
static void apply_shadow_from_ombre(OwlControl* target, ObjInstance* ombre) {
    if (!target || !ombre) return;
    int flou = instance_get_int(ombre, "Flou", 0);
    /*  — la couleur d'ombre peut être une instance Couleur (alpha porté) :
     * instance_get_color extrait bgr + alpha ; l'entier nu reste opaque (255).
     * Défaut bgr=0 (noir) conservé si le champ est absent. */
    int couleur = 0, couleurAlpha = 255;
    instance_get_color(ombre, "Couleur", &couleur, &couleurAlpha);
    if (couleur < 0) couleur = 0;
    int dx      = instance_get_int(ombre, "DecalageX", 0);
    int dy      = instance_get_int(ombre, "DecalageY", 0);
    int etendue = instance_get_int(ombre, "Etendue",   0);
    owl_ui_control_set_shadow(target, couleur, dx, dy, flou, etendue, couleurAlpha);
    /*  — mémorise la base pour le retour d'état (survol/appuyé → base
     * quand la souris sort ou relâche). Présente dès qu'un flou, un décalage ou
     * une étendue existe : une ombre dure (Flou=0 + décalage, néobrutalisme) doit
     * être restaurée au Leave, pas jetée. */
    int base_present = (flou > 0 || dx != 0 || dy != 0 || etendue > 0) ? 1 : 0;
    owl_ui_control_set_shadow_state(target, 0, couleur, dx, dy, flou, etendue,
                                    couleurAlpha, base_present);
}

/*  — une Ombre est « présente » dès qu'un flou, un décalage ou une
 * étendue existe. Sert de garde pour ne PAS écraser (retirer) une ombre déjà
 * posée par une autre source quand la source courante est un défaut inerte. */
static int ombre_instance_is_present(ObjInstance* ombre) {
    if (!ombre) return 0;
    return (instance_get_int(ombre, "Flou",      0) > 0
         || instance_get_int(ombre, "DecalageX", 0) != 0
         || instance_get_int(ombre, "DecalageY", 0) != 0
         || instance_get_int(ombre, "Etendue",   0) > 0) ? 1 : 0;
}

static void apply_shadow_from_style(OwlControl* w, ObjInstance* style) {
    if (!w || !style) return;
    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (!cadreExt) return;
    apply_shadow_from_ombre(w, instance_get_instance(cadreExt, "Ombre"));
}

/*  — lit l'Ombre d'un slot d'état (Etats.<slot>..Ombre) et la mémorise
 * comme surcharge d'ombre sur le widget. L'event filter Qt (armé si au moins un
 * état interactif porte une ombre) swappe l'effet au survol/appui. Un slot sans
 * Ombre => pas de surcharge (present=0, fallback base). */
static void apply_shadow_state(OwlControl* w, ObjInstance* etats,
                               const char* slotName, int stateIdx) {
    ObjInstance* slot = etats ? instance_get_instance(etats, slotName) : NULL;
    ObjInstance* ombre = slot ? instance_get_instance(slot, "Ombre") : NULL;
    if (!ombre) {
        owl_ui_control_set_shadow_state(w, stateIdx, 0, 0, 0, 0, 0, 255, 0);
        return;
    }
    int flou    = instance_get_int(ombre, "Flou",      0);
    /*  — couleur d'ombre d'état alpha-aware (miroir de la base). */
    int couleur = 0, couleurAlpha = 255;
    instance_get_color(ombre, "Couleur", &couleur, &couleurAlpha);
    if (couleur < 0) couleur = 0;
    int dx      = instance_get_int(ombre, "DecalageX", 0);
    int dy      = instance_get_int(ombre, "DecalageY", 0);
    int etendue = instance_get_int(ombre, "Etendue",   0);
    owl_ui_control_set_shadow_state(w, stateIdx, couleur, dx, dy, flou, etendue,
                                    couleurAlpha, 1);
}

/*  — mémorise les surcharges d'ombre d'état (survol/appuyé) du
 * CadreExtérieur. Miroir d'apply_frame_states_from_style pour l'ombre portée
 * (objet Qt, pas QSS). Conteneur exclu (même logique que les autres passes). */
static void apply_shadow_states_from_style(OwlControl* w, ObjInstance* style,
                                           OwlControlKind kind) {
    if (!w || !style) return;
    if (kind == OWL_CONTROL_CONTAINER) return;
    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (!cadreExt) return;
    ObjInstance* etats = instance_get_instance(cadreExt, "Etats");
    if (!etats) return;
    apply_shadow_state(w, etats, "_Survol", 1);
    apply_shadow_state(w, etats, "_Appuye", 2);
}

/*  — applique le dégradé de fond du widget depuis
 * style..CadreExtérieur..Dégradé. Le gate d'activation est le Type (dégradéAucun
 * -> pas de dégradé, le fond uni reprend). Un dégradé actif prime sur
 * CouleurFond via le QSS (background:). Portée : widgets mono-widget dont le fond
 * passe par apply_color_stylesheet (Bouton/Libellé/Image/Saisie). Les conteneurs
 * (QSS scopé #objectName) et la Fenêtre sont hors de ce chemin. */
/*  — applique une instance styleDégradé sur un widget cible arbitraire.
 * Factorisé d'apply_gradient_from_style : la source (CadreExtérieur ou
 * ZoneSaisie) et la cible (QLineEdit) sont découplées. Dégradé absent = no-op ;
 * Type dégradéAucun => owl_ui_control_set_bg_gradient retire le dégradé. */
/*  — résout les deux arrêts d'un dégradé, sentinelle -1 = « non défini ».
 * Un arrêt non défini rend TRANSPARENT (alpha 0), avec la couleur de l'arrêt pair
 * pour un fondu propre (couleur -> même couleur transparente). Distingue « non
 * posé » (transparent) d'un noir explicite (= Noir = 0 -> opaque). Miroir de la
 * sentinelle CouleurFond. */
static void degrade_resolve_stops(ObjInstance* degrade,
                                  int* debut, int* debutAlpha,
                                  int* fin, int* finAlpha) {
    *debut = 0; *debutAlpha = 255; *fin = 0; *finAlpha = 255;
    instance_get_color(degrade, "CouleurDebut", debut, debutAlpha);
    instance_get_color(degrade, "CouleurFin",   fin,   finAlpha);
    int debutDef = (*debut >= 0), finDef = (*fin >= 0);
    if (!finDef)   { *fin   = debutDef ? *debut : 0; *finAlpha   = 0; }
    if (!debutDef) { *debut = finDef   ? *fin   : 0; *debutAlpha = 0; }
}

static void apply_gradient_from_degrade(OwlControl* target, ObjInstance* degrade) {
    if (!target || !degrade) return;
    int type   = instance_get_int(degrade, "Type",  OWL_GRAD_AUCUN);
    int angle  = instance_get_int(degrade, "Angle", 0);
    /*  — arrêts en instance Couleur (alpha porté) via instance_get_color. */
    int debut, debutAlpha, fin, finAlpha;
    degrade_resolve_stops(degrade, &debut, &debutAlpha, &fin, &finAlpha);
    owl_ui_control_set_bg_gradient(target, type, angle, debut, debutAlpha, fin, finAlpha);
}

/*  — un Dégradé est « actif » quand son Type est linéaire ou radial.
 * Garde pour ne PAS retirer un dégradé posé par une autre source quand la source
 * courante est un défaut inerte (dégradéAucun). */
static int degrade_instance_is_active(ObjInstance* degrade) {
    if (!degrade) return 0;
    int type = instance_get_int(degrade, "Type", OWL_GRAD_AUCUN);
    return (type == OWL_GRAD_LINEAIRE || type == OWL_GRAD_RADIAL) ? 1 : 0;
}

static void apply_gradient_from_style(OwlControl* w, ObjInstance* style,
                                      OwlControlKind kind) {
    if (!w || !style) return;
    /* Le conteneur peint son fond via un QSS scopé (owl_ui_container_set_style) :
     * une feuille '*' d'apply_color_stylesheet baverait sur ses enfants. */
    if (kind == OWL_CONTROL_CONTAINER) return;
    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (!cadreExt) return;
    apply_gradient_from_degrade(w, instance_get_instance(cadreExt, "Degrade"));
}

/*  — lit un slot d'état du CadreExtérieur (CouleurFond + Cadre + Dégradé)
 * et pose le fragment QSS d'état correspondant sur le widget. Miroir de
 * apply_style_state (états texte de ZoneSaisie/Libellé) côté cadre. stateIdx :
 * 0 = survol (:hover), 1 = focus (:focus), 2 = grisé (:disabled),
 * 4 = appuyé (:pressed). Un slot absent/vide reset le fragment. */
static void apply_frame_state(OwlControl* w, ObjInstance* etats,
                              const char* slotName, int stateIdx) {
    ObjInstance* slot = instance_get_instance(etats, slotName);
    int bg = -1, bgAlpha = 255;
    int borderColor = 0, borderColorAlpha = 255;
    int borderThick = 0, borderStyle = OWL_TC_CONTINU;
    int borderRadius = 0;
    int gType = OWL_GRAD_AUCUN, gAngle = 0;
    int gDebut = 0, gDebutAlpha = 255, gFin = 0, gFinAlpha = 255;
    if (slot) {
        /*  — CouleurFond/Couleur peuvent être des instances Couleur
         * (alpha porté) : instance_get_color extrait bgr + alpha. */
        instance_get_color(slot, "CouleurFond", &bg, &bgAlpha);
        ObjInstance* cadre = instance_get_instance(slot, "Cadre");
        if (cadre) {
            instance_get_color(cadre, "Couleur", &borderColor, &borderColorAlpha);
            if (borderColor < 0) borderColor = 0;
            borderThick  = instance_get_int(cadre, "Epaisseur", 0);
            borderStyle  = instance_get_int(cadre, "Trait", OWL_TC_CONTINU);
            borderRadius = instance_get_int(cadre, "Arrondi",   0);
        }
        ObjInstance* degrade = instance_get_instance(slot, "Degrade");
        if (degrade) {
            gType  = instance_get_int(degrade, "Type",  OWL_GRAD_AUCUN);
            gAngle = instance_get_int(degrade, "Angle", 0);
            /*  — arrêt d'état non défini (-1) = transparent, comme le
             * dégradé de base (degrade_resolve_stops). Sans ça, la sentinelle -1
             * remonterait en owl_bg_css(-1,255) = blanc parasite. */
            degrade_resolve_stops(degrade, &gDebut, &gDebutAlpha, &gFin, &gFinAlpha);
        }
    }
    owl_ui_control_set_frame_state(w, stateIdx, bg, bgAlpha,
                                   borderColor, borderColorAlpha, borderThick,
                                   borderStyle, borderRadius,
                                   gType, gAngle, gDebut, gDebutAlpha,
                                   gFin, gFinAlpha);
}

/*  — applique les surcharges d'état du CadreExtérieur d'un style
 * (survol/appuyé/focus/grisé) sur un widget mono-element. Le conteneur est exclu
 * (son fond passe par un QSS scopé #id ; un `*:hover` baverait sur les enfants,
 * même logique qu'apply_gradient_from_style). */
static void apply_frame_states_from_style(OwlControl* w, ObjInstance* style,
                                          OwlControlKind kind) {
    if (!w || !style) return;
    if (kind == OWL_CONTROL_CONTAINER) return;
    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (!cadreExt) return;
    ObjInstance* etats = instance_get_instance(cadreExt, "Etats");
    if (!etats) return;
    apply_frame_state(w, etats, "_Survol", 0);   /* :hover */
    apply_frame_state(w, etats, "_Focus",  1);   /* :focus */
    apply_frame_state(w, etats, "_Grise",  2);   /* :disabled */
    apply_frame_state(w, etats, "_Appuye", 4);   /* :pressed */
}

/*  — Applique une instance styleSaisie (modele refondu : struct pur de
 * sous-elements) sur un champ Saisie. Trois cibles Qt distinctes :
 *   - ZoneSaisie   -> le QLineEdit (`w`) : props de base + Etats (survol/focus/
 *     grisé/lectureSeule) via le mecanisme QSS phase 1/2 (feuille unique).
 *   - Libellé      -> le QLabel du libelle integre (statique).
 *   - CadreExtérieur -> le wrapper conteneur (always-wrap), QSS `#id`-scope
 *     (fond + bordure), statique.
 *
 * Override canon : les props posees explicitement sur le widget avant le
 * ..Style restent ; les setters du Style ecrasent (derniere ecrite gagne). */
static void apply_style_to_control(OwlControl* w, ObjInstance* style,
                                     OwlControlKind kind) {
    if (!w || !style) return;

    /* ---- ZoneSaisie -> QLineEdit (le controle logique `w`). ---- */
    ObjInstance* zone = instance_get_instance(style, "ZoneSaisie");
    if (zone) {
        apply_style_brick_to_widget(w, zone, kind);
        /* Etats (surcharges par etat) : lus depuis ZoneSaisie..Etats, emis en
         * fragments QSS concatenes dans la feuille unique du QLineEdit. Grisé et
         * lectureSeule sont declenches par Qt (Etat = Grisé / AffichageSeulement
         * passe le widget disabled/readonly) — aucun lien manuel Etat->style. */
        ObjInstance* etats = instance_get_instance(zone, "Etats");
        if (etats) {
            apply_style_state(w, etats, "_Survol",       0);
            apply_style_state(w, etats, "_Focus",        1);
            apply_style_state(w, etats, "_Grise",        2);
            apply_style_state(w, etats, "_LectureSeule", 3);
        }
    }

    /* Sous-elements Libellé / CadreExtérieur : seulement pour un champ Saisie
     * (le libelle integre et son conteneur n'existent que la). */
    if (kind == OWL_CONTROL_INPUT) {
        /* ---- Libellé -> QLabel integre (statique). ---- */
        ObjInstance* lib = instance_get_instance(style, "Libelle");
        if (lib) {
            OwlControl* label = owl_ui_saisie_get_label(w);
            if (label) apply_style_brick_to_widget(label, lib, OWL_CONTROL_LABEL);
        }

        /* ---- CadreExtérieur -> wrapper conteneur (always-wrap, statique). ----
         * QSS `#id`-scope (fond + bordure) : ne cascade pas sur QLabel/QLineEdit.
         * fg = -1 : un conteneur n'a pas de texte. */
        ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
        if (cadreExt) {
            OwlControl* wrapper = owl_ui_saisie_ensure_wrapper(w);
            if (wrapper) {
                /*  — CouleurFond peut être une instance Couleur (alpha
                 * porté) : instance_get_color extrait bgr + alpha, l'entier nu
                 * reste opaque (byte-identique). */
                int bg = -1, bgAlpha = 255;
                instance_get_color(cadreExt, "CouleurFond", &bg, &bgAlpha);
                OwlCadreSpec border = {0};
                const OwlCadreSpec* borderPtr = NULL;
                ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
                if (cadre && cadre_instance_is_visible(cadre)) {
                    cadre_instance_to_spec(cadre, &border);
                    borderPtr = &border;
                }
                owl_ui_saisie_wrapper_set_style(wrapper, -1, bg, bgAlpha, borderPtr);
            }
        }
    }

    /*  — Champ Libellé stylé (styleLibellé widget). Widget mono-element :
     * le CadreExtérieur est la bordure DU QLabel lui-meme, pas un wrapper separe.
     * Deux cibles, meme widget `w` :
     *   - Libellé        -> texte (Couleur/Police) via la brique commune (garde
     *     `> 0` , le texte suit le theme sans Couleur explicite).
     *   - CadreExtérieur -> la boite (CouleurFond + bordure) en QSS directe sur
     *     `w`. Les setters (bg, cadre) alimentent owl_bg / owl_cadre_body, fusionnes
     *     dans la feuille unique par apply_color_stylesheet : pas de clobber avec la
     *     brique Libellé. Applique APRES la brique -> CadreExtérieur possede la
     *     boite (fond/bordure), la brique n'apporte que le texte. */
    if (kind == OWL_CONTROL_LABEL) {
        ObjInstance* lib = instance_get_instance(style, "Libelle");
        if (lib) apply_style_brick_to_widget(w, lib, OWL_CONTROL_LABEL);

        ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
        if (cadreExt) {
            /*  — CouleurFond peut être une instance Couleur (alpha porté). */
            int bg = -1, bgAlpha = 255;
            instance_get_color(cadreExt, "CouleurFond", &bg, &bgAlpha);
            if (bg >= 0) owl_ui_control_set_bg_color_alpha(w, bg, bgAlpha);
            ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre))
                apply_cadre_instance_to_widget(w, cadre);
        }
    }

    /*  — Champ Image stylé (styleImage widget). Affichage statique
     * (QLabel + pixmap), widget mono-element : le CadreExtérieur est la boite DU
     * widget image lui-meme, pas un wrapper separe (patron  Libellé/
     * Bouton). Un seul sous-element (pas de texte -> pas de brique Libellé). La
     * source de l'image (prop top-level Image) et le mode d'echelle (Cadrage)
     * restent geres par le dispatcher de props plates, hors Style.
     *  — le Champ CodeBarres reutilise le meme modele (styleImage,
     * backend OwlImageLabel) : CadreExtérieur = la boite du champ. */
    if (kind == OWL_CONTROL_IMAGE || kind == OWL_CONTROL_CODEBARRE) {
        ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
        if (cadreExt) {
            /*  — CouleurFond peut être une instance Couleur (alpha porté) :
             * instance_get_color extrait bgr + alpha, l'entier nu reste opaque
             * (byte-identique). instance_get_int rendait -1 (fond jamais posé) sur
             * une instance -> le fond translucide était ignoré. */
            int bg = -1, bgAlpha = 255;
            instance_get_color(cadreExt, "CouleurFond", &bg, &bgAlpha);
            if (bg >= 0) owl_ui_control_set_bg_color_alpha(w, bg, bgAlpha);
            ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre))
                apply_cadre_instance_to_widget(w, cadre);
        }
    }

    /* , ,  — Champ Bouton stylé (styleBouton widget).
     * Widget mono-element : le CadreExtérieur est la bordure DU QPushButton
     * lui-meme, pas un wrapper separe. Deux cibles, meme widget `w` :
     *   - LibelléEtImage -> texte (Couleur/Police) via la brique commune (garde
     *     `> 0`, le texte suit le theme sans Couleur explicite) + les etats
     *     survol/appuyé/focus/grisé via QSS (:hover/:pressed/:focus/:disabled).
     *     appuyé (stateIdx 4) = etat neuf.
     *   - CadreExtérieur -> la boite (CouleurFond + bordure) en QSS directe sur
     *     `w`. Les setters (bg, cadre) alimentent owl_bg / owl_cadre_body, fusionnes
     *     avec le texte de la brique ET les fragments d'etat dans la feuille unique
     *     par apply_color_stylesheet : pas de clobber. Applique APRES la brique
     *     -> CadreExtérieur possede la boite (fond/bordure). */
    if (kind == OWL_CONTROL_BUTTON) {
        ObjInstance* bouton = instance_get_instance(style, "Libelle");
        if (bouton) {
            apply_style_brick_to_widget(w, bouton, OWL_CONTROL_BUTTON);
            ObjInstance* etats = instance_get_instance(bouton, "Etats");
            if (etats) {
                apply_style_state(w, etats, "_Survol", 0);   /* :hover */
                apply_style_state(w, etats, "_Appuye", 4);   /* :pressed (neuf) */
                apply_style_state(w, etats, "_Focus",  1);   /* :focus */
                apply_style_state(w, etats, "_Grise",  2);   /* :disabled */
            }
        }

        /*  lot B (recadre , ) — sous-element Image.
         * La feuille Style..Image..Image est la SOURCE de l'icone (VARIANT
         * Image|chaine|Icone), unifiee avec la prop plate BTN..Image (une seule
         * verite dans le store, pattern ). Position pilote le placement
         * icone/texte via le QProxyStyle custom (lit l'icone reelle du
         * QPushButton, peu importe sa provenance). */
        ObjInstance* image = instance_get_instance(style, "Image");
        if (image) {
            Value iconVal;
            /* Ne pose que si une icone est renseignee (chaine vide = defaut
             * "pas d'icone" -> ne clobber pas une icone posee autrement). */
            if (instance_get_value(image, "Image", &iconVal)
                && !(IS_STRING(iconVal) && AS_STRING(iconVal)->length == 0)) {
                button_set_icon_value(w, iconVal);
            }
            int pos = instance_get_int(image, "Position", OWL_POSBTNIMG_GAUCHE);
            owl_ui_button_set_position_image(w, pos);

            /*  — surcharge d'icone par etat (survol -> QIcon::Active,
             * grisé -> QIcon::Disabled). Applique APRES l'icone de base : le
             * mode cible est substitue, les autres modes gardent la base. */
            ObjInstance* imgEtats = instance_get_instance(image, "Etats");
            if (imgEtats) {
                /*  — grisé d'abord : il modifie l'icone de base
                 * (overlay QIcon::Disabled). Le survol snapshote ensuite cette
                 * icone comme base restaurable, en preservant le mode grisé. */
                apply_icon_state(w, imgEtats, "_Grise",  1);  /* Disabled */
                apply_icon_state(w, imgEtats, "_Survol", 0);  /* hover swap */
            }
        }

        ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
        if (cadreExt) {
            /*  — CouleurFond peut être une instance Couleur (alpha porté) :
             * instance_get_color extrait bgr + alpha, l'entier nu reste opaque
             * (byte-identique). instance_get_int rendait -1 (fond jamais posé) sur
             * une instance -> le bouton flat gardait son chrome natif. */
            int bg = -1, bgAlpha = 255;
            instance_get_color(cadreExt, "CouleurFond", &bg, &bgAlpha);
            if (bg >= 0) owl_ui_control_set_bg_color_alpha(w, bg, bgAlpha);
            ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre))
                apply_cadre_instance_to_widget(w, cadre);
        }
    }

    /*  — Champ Interrupteur stylé (styleInterrupteur widget). Widget
     * composite (QFrame + N QCheckBox) : une feuille QSS unique, decomposee par
     * sous-element (CadreExtérieur -> conteneur #id-scope ; TexteOptions ->
     * QCheckBox ; CadreCoche -> QCheckBox::indicator ; Coche -> marque cochée,
     * approche via ::indicator:checked). Le passage par un setter dedie evite le
     * clobber que produirait apply_color_stylesheet (selecteur '*' cascadant sur
     * les cases). */
    if (kind == OWL_CONTROL_INTERRUPTEUR) {
        OwlInterrupteurStyleSpec spec = {0};
        spec.ext_bg     = -1;
        spec.coche_bg   = -1;
        spec.txt_fg     = -1;
        spec.txt_bg     = -1;
        spec.txt_bold   = -1;
        spec.txt_italic = -1;
        spec.coche_mark = -1;
        spec.ext_bg_alpha   = 255;   /*  */
        spec.txt_bg_alpha   = 255;
        spec.txt_fg_alpha   = 255;
        spec.coche_bg_alpha = 255;
        spec.coche_mark_alpha = 255;
        spec.ext_border = NULL;
        spec.coche_border = NULL;

        OwlCadreSpec extBorder   = {0};
        OwlCadreSpec cocheBorder = {0};
        const char* fontFamily = NULL;

        /* CadreExtérieur -> conteneur (fond + bordure). */
        ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
        if (cadreExt) {
            /*  — CouleurFond peut être une instance Couleur (alpha porté). */
            instance_get_color(cadreExt, "CouleurFond", &spec.ext_bg, &spec.ext_bg_alpha);
            ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre)) {
                cadre_instance_to_spec(cadre, &extBorder);
                spec.ext_border = &extBorder;
            }
        }

        /* TexteOptions -> texte des cases (Couleur + Police). Garde `> 0` sur la
         * couleur (seed 0 = non pose, comme les autres briques). */
        ObjInstance* texte = instance_get_instance(style, "TexteOptions");
        if (texte) {
            /*  — TexteOptions..Couleur/CouleurFond peuvent être des
             * instances Couleur (alpha porté) : instance_get_color extrait bgr + alpha. */
            instance_get_color(texte, "Couleur", &spec.txt_fg, &spec.txt_fg_alpha);
            instance_get_color(texte, "CouleurFond", &spec.txt_bg, &spec.txt_bg_alpha);
            ObjInstance* police = instance_get_instance(texte, "Police");
            if (police && !police_instance_is_default(police)) {
                Value nomV;
                ObjString* nomK = obj_copy_string("Nom", 3);
                if (table_get(&police->fields, nomK, &nomV)
                    && IS_STRING(nomV) && AS_STRING(nomV)->length > 0)
                    fontFamily = AS_CSTRING(nomV);
                spec.txt_font_family = fontFamily;
                spec.txt_font_size = instance_get_int(police, "Taille", 0);
                spec.txt_bold   = instance_get_bool(police, "Gras",     false) ? 1 : 0;
                spec.txt_italic = instance_get_bool(police, "Italique", false) ? 1 : 0;
                /* Police..Couleur prime sur TexteOptions..Couleur si posee. */
                int pc = -1, pcA = 255;
                instance_get_color(police, "Couleur", &pc, &pcA);
                if (pc > 0) { spec.txt_fg = pc; spec.txt_fg_alpha = pcA; }
            }
        }

        /* CadreCoche -> boite de la coche (::indicator : fond + bordure). */
        ObjInstance* cadreCoche = instance_get_instance(style, "CadreCoche");
        if (cadreCoche) {
            instance_get_color(cadreCoche, "CouleurFond",
                               &spec.coche_bg, &spec.coche_bg_alpha);
            ObjInstance* cadre = instance_get_instance(cadreCoche, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre)) {
                cadre_instance_to_spec(cadre, &cocheBorder);
                spec.coche_border = &cocheBorder;
            }
        }

        /* Coche -> couleur de la marque cochée. */
        ObjInstance* coche = instance_get_instance(style, "Coche");
        if (coche)
            instance_get_color(coche, "Couleur", &spec.coche_mark, &spec.coche_mark_alpha);

        owl_ui_interrupteur_set_style(w, &spec);
    }

    /*  — Champ Sélecteur stylé (styleSélecteur widget). Pendant du
     * traitement Interrupteur, adapte radio : QFrame + N QRadioButton, une
     * feuille QSS unique decomposee par sous-element (CadreExtérieur ->
     * conteneur #id-scope ; TexteOptions -> QRadioButton ; CadrePuce ->
     * QRadioButton::indicator ; Puce -> rond selectionne, approche via
     * ::indicator:checked). */
    if (kind == OWL_CONTROL_SELECTEUR) {
        OwlSelecteurStyleSpec spec = {0};
        spec.ext_bg     = -1;
        spec.puce_bg    = -1;
        spec.txt_fg     = -1;
        spec.txt_bg     = -1;
        spec.txt_bold   = -1;
        spec.txt_italic = -1;
        spec.puce_mark  = -1;
        spec.ext_bg_alpha  = 255;   /*  */
        spec.txt_bg_alpha  = 255;
        spec.txt_fg_alpha  = 255;
        spec.puce_bg_alpha = 255;
        spec.puce_mark_alpha = 255;
        spec.ext_border = NULL;
        spec.puce_border = NULL;

        OwlCadreSpec extBorder  = {0};
        OwlCadreSpec puceBorder = {0};
        const char* fontFamily = NULL;

        /* CadreExtérieur -> conteneur (fond + bordure). */
        ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
        if (cadreExt) {
            /*  — CouleurFond peut être une instance Couleur (alpha porté). */
            instance_get_color(cadreExt, "CouleurFond", &spec.ext_bg, &spec.ext_bg_alpha);
            ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre)) {
                cadre_instance_to_spec(cadre, &extBorder);
                spec.ext_border = &extBorder;
            }
        }

        /* TexteOptions -> texte des options (Couleur + Police). */
        ObjInstance* texte = instance_get_instance(style, "TexteOptions");
        if (texte) {
            /*  — TexteOptions..Couleur/CouleurFond alpha-aware (instance Couleur). */
            instance_get_color(texte, "Couleur", &spec.txt_fg, &spec.txt_fg_alpha);
            instance_get_color(texte, "CouleurFond", &spec.txt_bg, &spec.txt_bg_alpha);
            ObjInstance* police = instance_get_instance(texte, "Police");
            if (police && !police_instance_is_default(police)) {
                Value nomV;
                ObjString* nomK = obj_copy_string("Nom", 3);
                if (table_get(&police->fields, nomK, &nomV)
                    && IS_STRING(nomV) && AS_STRING(nomV)->length > 0)
                    fontFamily = AS_CSTRING(nomV);
                spec.txt_font_family = fontFamily;
                spec.txt_font_size = instance_get_int(police, "Taille", 0);
                spec.txt_bold   = instance_get_bool(police, "Gras",     false) ? 1 : 0;
                spec.txt_italic = instance_get_bool(police, "Italique", false) ? 1 : 0;
                int pc = -1, pcA = 255;
                instance_get_color(police, "Couleur", &pc, &pcA);
                if (pc > 0) { spec.txt_fg = pc; spec.txt_fg_alpha = pcA; }
            }
        }

        /* CadrePuce -> boite du rond (::indicator : fond + bordure). */
        ObjInstance* cadrePuce = instance_get_instance(style, "CadrePuce");
        if (cadrePuce) {
            instance_get_color(cadrePuce, "CouleurFond",
                               &spec.puce_bg, &spec.puce_bg_alpha);
            ObjInstance* cadre = instance_get_instance(cadrePuce, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre)) {
                cadre_instance_to_spec(cadre, &puceBorder);
                spec.puce_border = &puceBorder;
            }
        }

        /* Puce -> couleur du rond selectionne. */
        ObjInstance* puce = instance_get_instance(style, "Puce");
        if (puce)
            instance_get_color(puce, "Couleur", &spec.puce_mark, &spec.puce_mark_alpha);

        owl_ui_selecteur_set_style(w, &spec);
    }

    /*  — Champ Liste stylé (styleListe widget). Feuille QSS unique sur
     * le QListWidget, decomposee par sous-element : CadreExtérieur -> fond +
     * bordure de la liste ; LibelléLignes -> texte des lignes (::item) ;
     * LigneSélectionnée -> texte de la ligne selectionnee (::item:selected). */
    if (kind == OWL_CONTROL_LISTE) {
        OwlListeStyleSpec spec = {0};
        spec.ext_bg   = -1; spec.ext_bg_alpha = 255;
        spec.row_fg   = -1; spec.row_bg = -1;
        spec.row_fg_alpha = 255; spec.row_bg_alpha = 255;
        spec.row_bold = -1; spec.row_italic = -1;
        spec.sel_fg   = -1; spec.sel_bg = -1;
        spec.sel_fg_alpha = 255; spec.sel_bg_alpha = 255;
        spec.sel_bold = -1; spec.sel_italic = -1;

        OwlCadreSpec extBorder = {0};
        const char* rowFamily = NULL;
        const char* selFamily = NULL;

        /* CadreExtérieur -> fond + bordure de la liste. */
        ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
        if (cadreExt) {
            /*  — CouleurFond peut être une instance Couleur (alpha porté). */
            instance_get_color(cadreExt, "CouleurFond", &spec.ext_bg, &spec.ext_bg_alpha);
            ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre)) {
                cadre_instance_to_spec(cadre, &extBorder);
                spec.ext_border = &extBorder;
            }
        }

        /* LibelléLignes -> texte des lignes (Couleur + Police). */
        ObjInstance* rows = instance_get_instance(style, "LibelleLignes");
        if (rows) {
            instance_get_color(rows, "Couleur", &spec.row_fg, &spec.row_fg_alpha);
            instance_get_color(rows, "CouleurFond", &spec.row_bg, &spec.row_bg_alpha);
            ObjInstance* police = instance_get_instance(rows, "Police");
            if (police && !police_instance_is_default(police)) {
                Value nomV;
                ObjString* nomK = obj_copy_string("Nom", 3);
                if (table_get(&police->fields, nomK, &nomV)
                    && IS_STRING(nomV) && AS_STRING(nomV)->length > 0)
                    rowFamily = AS_CSTRING(nomV);
                spec.row_font_family = rowFamily;
                spec.row_font_size = instance_get_int(police, "Taille", 0);
                spec.row_bold   = instance_get_bool(police, "Gras",     false) ? 1 : 0;
                spec.row_italic = instance_get_bool(police, "Italique", false) ? 1 : 0;
                int pc = -1, pcA = 255;
                instance_get_color(police, "Couleur", &pc, &pcA);
                if (pc > 0) { spec.row_fg = pc; spec.row_fg_alpha = pcA; }
            }
        }

        /* LigneSélectionnée -> texte de la ligne selectionnee. */
        ObjInstance* selRow = instance_get_instance(style, "LigneSelectionnee");
        if (selRow) {
            instance_get_color(selRow, "Couleur", &spec.sel_fg, &spec.sel_fg_alpha);
            instance_get_color(selRow, "CouleurFond", &spec.sel_bg, &spec.sel_bg_alpha);
            ObjInstance* police = instance_get_instance(selRow, "Police");
            if (police && !police_instance_is_default(police)) {
                Value nomV;
                ObjString* nomK = obj_copy_string("Nom", 3);
                if (table_get(&police->fields, nomK, &nomV)
                    && IS_STRING(nomV) && AS_STRING(nomV)->length > 0)
                    selFamily = AS_CSTRING(nomV);
                spec.sel_font_family = selFamily;
                spec.sel_font_size = instance_get_int(police, "Taille", 0);
                spec.sel_bold   = instance_get_bool(police, "Gras",     false) ? 1 : 0;
                spec.sel_italic = instance_get_bool(police, "Italique", false) ? 1 : 0;
                int pc = -1, pcA = 255;
                instance_get_color(police, "Couleur", &pc, &pcA);
                if (pc > 0) { spec.sel_fg = pc; spec.sel_fg_alpha = pcA; }
            }
        }

        owl_ui_liste_set_style(w, &spec);
    }

    /*  — Champ Combo stylé (styleCombo widget). Feuille QSS unique sur
     * le QComboBox, decomposee par sous-element : Libellé -> le QLabel du wrapper
     * integre ; CadreExtérieur -> fond + bordure englobante ; ZoneSaisie -> le
     * champ combo affiche ; LibelléLignes -> items du popup (::item) ;
     * LigneSélectionnée -> item surligné (::item:selected). */
    if (kind == OWL_CONTROL_COMBO) {
        OwlComboStyleSpec spec = {0};
        spec.ext_bg    = -1; spec.ext_bg_alpha = 255;
        spec.field_fg  = -1; spec.field_bg = -1;
        spec.field_fg_alpha = 255; spec.field_bg_alpha = 255;
        spec.field_bold = -1; spec.field_italic = -1;
        spec.row_fg    = -1; spec.row_bg = -1;
        spec.row_fg_alpha = 255; spec.row_bg_alpha = 255;
        spec.row_bold  = -1; spec.row_italic = -1;
        spec.sel_fg    = -1; spec.sel_bg = -1;
        spec.sel_fg_alpha = 255; spec.sel_bg_alpha = 255;
        spec.sel_bold  = -1; spec.sel_italic = -1;
        spec.label_fg  = -1; spec.label_bg = -1;
        spec.label_fg_alpha = 255; spec.label_bg_alpha = 255;
        spec.label_bold = -1; spec.label_italic = -1;

        OwlCadreSpec extBorder = {0};
        OwlCadreSpec fieldBorder = {0};
        const char* fieldFamily = NULL;
        const char* rowFamily = NULL;
        const char* selFamily = NULL;
        const char* labelFamily = NULL;

        /* Helper local (inline) : lit une brique texte (Couleur + Police) dans
         * les champs fg/bg/font de la spec. Repris du pattern Liste. */
        #define COMBO_READ_TEXT_BRICK(elemName, fgF, bgF, famF, famVar, sizeF, boldF, italF) \
            do { \
                ObjInstance* _b = instance_get_instance(style, elemName); \
                if (_b) { \
                    instance_get_color(_b, "Couleur", &spec.fgF, &spec.fgF##_alpha); \
                    instance_get_color(_b, "CouleurFond", &spec.bgF, &spec.bgF##_alpha); \
                    ObjInstance* _p = instance_get_instance(_b, "Police"); \
                    if (_p && !police_instance_is_default(_p)) { \
                        Value _nomV; ObjString* _nomK = obj_copy_string("Nom", 3); \
                        if (table_get(&_p->fields, _nomK, &_nomV) \
                            && IS_STRING(_nomV) && AS_STRING(_nomV)->length > 0) \
                            famVar = AS_CSTRING(_nomV); \
                        spec.famF = famVar; \
                        spec.sizeF = instance_get_int(_p, "Taille", 0); \
                        spec.boldF = instance_get_bool(_p, "Gras", false) ? 1 : 0; \
                        spec.italF = instance_get_bool(_p, "Italique", false) ? 1 : 0; \
                        int _pc = -1, _pcA = 255; \
                        instance_get_color(_p, "Couleur", &_pc, &_pcA); \
                        if (_pc > 0) { spec.fgF = _pc; spec.fgF##_alpha = _pcA; } \
                    } \
                } \
            } while (0)

        /* CadreExtérieur -> fond + bordure englobante. Materialise le wrapper
         * (always-wrap) pour que le CadreExtérieur + le Libellé aient une cible,
         * meme sans libelle texte (calque du Saisie). */
        ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
        ObjInstance* libStyle = instance_get_instance(style, "Libelle");
        if (cadreExt || libStyle) owl_ui_combo_ensure_wrapper(w);
        if (cadreExt) {
            /*  — CouleurFond peut être une instance Couleur (alpha porté). */
            instance_get_color(cadreExt, "CouleurFond", &spec.ext_bg, &spec.ext_bg_alpha);
            ObjInstance* cadre = instance_get_instance(cadreExt, "Cadre");
            if (cadre && cadre_instance_is_visible(cadre)) {
                cadre_instance_to_spec(cadre, &extBorder);
                spec.ext_border = &extBorder;
            }
        }

        COMBO_READ_TEXT_BRICK("ZoneSaisie", field_fg, field_bg, field_font_family,
                              fieldFamily, field_font_size, field_bold, field_italic);

        /*  — ZoneSaisie..Cadre : la brique texte ne portait que
         * Couleur/CouleurFond/Police, et apply_style_brick_to_widget gate le
         * Cadre sur LABEL/INPUT/BUTTON. La bordure et l'arrondi du champ combo
         * n'atteignaient donc jamais la feuille QSS (fond seul appliqué). */
        ObjInstance* zoneSaisie = instance_get_instance(style, "ZoneSaisie");
        if (zoneSaisie) {
            ObjInstance* zCadre = instance_get_instance(zoneSaisie, "Cadre");
            if (zCadre && cadre_instance_is_visible(zCadre)) {
                cadre_instance_to_spec(zCadre, &fieldBorder);
                spec.field_border = &fieldBorder;
            }
        }
        COMBO_READ_TEXT_BRICK("LibelleLignes", row_fg, row_bg, row_font_family,
                              rowFamily, row_font_size, row_bold, row_italic);
        COMBO_READ_TEXT_BRICK("LigneSelectionnee", sel_fg, sel_bg, sel_font_family,
                              selFamily, sel_font_size, sel_bold, sel_italic);
        COMBO_READ_TEXT_BRICK("Libelle", label_fg, label_bg, label_font_family,
                              labelFamily, label_font_size, label_bold, label_italic);
        #undef COMBO_READ_TEXT_BRICK

        owl_ui_combo_set_style(w, &spec);
    }

    /*  — Champ Table stylé (styleTable widget). Feuille QSS unique sur
     * le QTableView + son QHeaderView, decomposee par sous-element :
     * CadreExtérieur -> fond + bordure ; TitreColonne -> QHeaderView::section ;
     * LibelléLignes -> ::item ; LignesPaires/Impaires -> background alterne ;
     * LigneSélectionnée -> ::item:selected ; LigneSurvolée -> ::item:hover ;
     * Quadrillage -> gridline-color. L'override par colonne est applique au
     * build (case OWL_CONTROL_TABLE), pas ici. */
    if (kind == OWL_CONTROL_TABLE) {
        OwlTableStyleSpec spec;
        OwlCadreSpec extBorder = {0};
        const char* fams[4] = { NULL, NULL, NULL, NULL };
        read_table_style(style, &spec, &extBorder, fams);
        owl_ui_table_set_style(w, &spec);
    }

    /*  — Champ Zone répétée stylé (styleZoneRépétée widget). Le
     * CadreExtérieur habille le QFrame conteneur ; le fond de chaque rep
     * (parite/selection/survol), le CadreInterne et le Séparateur sont poses
     * par-rep cote runtime (owl_ui_zr_set_style memorise la spec et re-style
     * chaque rep, y compris celles ajoutees ensuite via AjouteLigne). */
    if (kind == OWL_CONTROL_ZONE_REPETEE) {
        OwlZrStyleSpec spec;
        OwlCadreSpec extBorder = {0};
        OwlCadreSpec innerBorder = {0};
        read_zr_style(style, &spec, &extBorder, &innerBorder);
        owl_ui_zr_set_style(w, &spec);
    }

    /*  — Champ Conteneur stylé (styleConteneur widget). Groupeur passif :
     * un seul sous-element CadreExtérieur (fond + bordure + retraits). QSS scope
     * `#objectName` cote Qt pour ne PAS baver sur les widgets enfants. */
    if (kind == OWL_CONTROL_CONTAINER) {
        int bg, bgAlpha, padL, padT, padR, padB;
        OwlCadreSpec extBorder = {0};
        const OwlCadreSpec* extBorderPtr = NULL;
        read_container_style(style, &bg, &bgAlpha, &extBorder, &extBorderPtr,
                             &padL, &padT, &padR, &padB);
        owl_ui_container_set_style(w, bg, bgAlpha, extBorderPtr,
                                   padL, padT, padR, padB);
    }

    /*  — Champ Jauge stylé (styleJauge widget). Rendu HYBRIDE : le
     * CadreExtérieur (piste + bordure) part en QSS `QProgressBar {}` + palette
     * Base ; le Remplissage en QSS `QProgressBar::chunk {}` + m_progressColor
     * (partagé avec le raccourci ..CouleurJauge) ; le Libellé (couleur texte) en
     * palette Text (le paintEvent dessine le texte au pen QPalette::Text). Ombre
     * / dégradé / états du CadreExtérieur passent par les helpers communs plus
     * bas (non gatés contre JAUGE). */
    if (kind == OWL_CONTROL_JAUGE) {
        OwlJaugeStyleSpec spec;
        OwlCadreSpec extBorder = {0};
        read_jauge_style(style, &spec, &extBorder);
        owl_ui_jauge_set_style(w, &spec);
    }

    /*  — ombre portée du widget entier (CadreExtérieur..Ombre),
     * commune à tous les kinds mono-widget. Posée en dernier : indépendante du
     * QSS (fond/bordure) déjà appliqué. */
    apply_shadow_from_style(w, style);

    /*  — dégradé de fond (CadreExtérieur..Dégradé), commun aux kinds
     * mono-widget. Posé après le fond uni : un dégradé actif le remplace. */
    apply_gradient_from_style(w, style, kind);

    /*  — surcharges d'état du CadreExtérieur (survol/focus/appuyé/grisé)
     * → règles QSS :hover/:focus/:pressed/:disabled. Après la base : les états
     * ne s'appliquent que dans leur pseudo-classe. */
    apply_frame_states_from_style(w, style, kind);

    /*  — surcharges d'ombre d'état (survol/appuyé). L'ombre est un
     * QGraphicsDropShadowEffect (pas pilotable en QSS) : mémorisée ici, swappée
     * par un event filter branché sur les événements souris natifs. */
    apply_shadow_states_from_style(w, style, kind);

    /*  — Ombre + Dégradé de la ZoneSaisie -> le QLineEdit (w). Le Champ
     * Saisie est un composite [QLabel|QLineEdit] : la ZoneSaisie décore le champ
     * interne. Posé APRÈS les passes CadreExtérieur (base + états) : quand la
     * ZoneSaisie déclare réellement une ombre/un dégradé, elle prime sur la
     * décoration CadreExtérieur du champ (le CadreExtérieur habille surtout le
     * conteneur). Gardé sur la présence pour ne PAS effacer une décoration
     * CadreExtérieur quand la ZoneSaisie est à son défaut inerte. */
    if (kind == OWL_CONTROL_INPUT) {
        ObjInstance* zone = instance_get_instance(style, "ZoneSaisie");
        if (zone) {
            ObjInstance* zOmbre = instance_get_instance(zone, "Ombre");
            if (ombre_instance_is_present(zOmbre))
                apply_shadow_from_ombre(w, zOmbre);
            ObjInstance* zDegrade = instance_get_instance(zone, "Degrade");
            if (degrade_instance_is_active(zDegrade))
                apply_gradient_from_degrade(w, zDegrade);
        }
    }
}

/*  — applique un style (base Style ou styleConteneur) au panneau
 * central de la Fenêtre : on relit le CadreExtérieur (CouleurFond + Cadre +
 * retraits) et on habille le panneau via owl_ui_window_set_panel_style. Le
 * Style ne prime sur la CouleurFond plate QUE s'il porte réellement un fond
 * ou une bordure (sinon on laisse le fond plat intact — un Style sans
 * CadreExtérieur, ex STY_Curseur, ne doit pas effacer le fond). */
static void apply_style_to_window(OwlWindow* w, ObjInstance* style) {
    if (!w || !style) return;
    int bg, bgAlpha, padL, padT, padR, padB;
    OwlCadreSpec extBorder = {0};
    const OwlCadreSpec* extBorderPtr = NULL;
    read_container_style(style, &bg, &bgAlpha, &extBorder, &extBorderPtr,
                         &padL, &padT, &padR, &padB);
    /*  — dégradé de fond de la Fenêtre (CadreExtérieur..Dégradé). Gate
     * d'activation = Type (dégradéAucun -> fond uni CouleurFond comme avant). Un
     * dégradé actif prime sur CouleurFond côté panneau (background:). Pas d'ombre
     * sur la Fenêtre (décision produit).  — arrêts en instance Couleur
     * (alpha porté) via instance_get_color. */
    int gradType = OWL_GRAD_AUCUN, gradAngle = 0;
    int gradDebut = 0, gradDebutAlpha = 255, gradFin = 0, gradFinAlpha = 255;
    ObjInstance* cadreExt = instance_get_instance(style, "CadreExterieur");
    if (cadreExt) {
        ObjInstance* degrade = instance_get_instance(cadreExt, "Degrade");
        if (degrade) {
            gradType  = instance_get_int(degrade, "Type",  OWL_GRAD_AUCUN);
            gradAngle = instance_get_int(degrade, "Angle", 0);
            /*  — arrêt non défini (-1) = transparent (cf. base). */
            degrade_resolve_stops(degrade, &gradDebut, &gradDebutAlpha,
                                  &gradFin, &gradFinAlpha);
        }
    }
    int gradActif = (gradType == OWL_GRAD_LINEAIRE || gradType == OWL_GRAD_RADIAL);
    if (bg < 0 && extBorderPtr == NULL && !gradActif) return;
    owl_ui_window_set_panel_style(w, bg, bgAlpha, extBorderPtr,
                                  padL, padT, padR, padB,
                                  gradType, gradAngle,
                                  gradDebut, gradDebutAlpha,
                                  gradFin, gradFinAlpha);
}

/* Dispatcher 'ctrl..Prop = val' appele depuis OP_STRUCT_SET.
 *
 *  lot C — Les fenetres ne transitent plus par ce dispatcher
 * (cf real_ui_get_prop). */
static bool real_ui_set_prop(ObjUiControl* c, const char* prop, Value v) {
    if (!c || !c->widget || !prop) return false;
    OwlControlKind kind = (OwlControlKind)c->kind;

    OwlControl* w = (OwlControl*)c->widget;

    /* Broadcast ZR proxy : c->name commence par "__zr_bcast__" → applique
     * sur toutes les reps de la ZR (c->widget = ZR widget). Le sub-name est
     * la suite du prefixe ; prop = la propriete a poser sur chaque sub. */
    if (kind == OWL_CONTROL_ZONE_REPETEE && c->name &&
        strncmp(c->name, "__zr_bcast__", 12) == 0) {
        const char* sub_name = c->name + 12;
        int n = owl_ui_zr_count(w);
        for (int i = 1; i <= n; i++) {
            OwlControl* rep = owl_ui_zr_get_rep(w, i);
            if (!rep) continue;
            OwlControl* sub = owl_ui_control_find_child(rep, sub_name);
            if (!sub) continue;
            int subKind = owl_ui_control_get_widget_kind(sub);
            if (subKind < 0) subKind = OWL_CONTROL_CONTAINER;
            ObjUiControl* sub_uic = obj_new_ui_control(subKind, sub, sub_name);
            owl_ui_control_set_prop(sub_uic, prop, v);
        }
        return true;
    }

    /* ZR — props directes (LigneSelectionnee/Sens/Multiselection). */
    if (kind == OWL_CONTROL_ZONE_REPETEE) {
        if (strcasecmp(prop, "LigneS\xc3\xa9lectionn\xc3\xa9""e") == 0 ||
            strcasecmp(prop, "LigneSelectionnee") == 0 ||
            strcasecmp(prop, "SelectedRow") == 0) {
            if (IS_INTEGER(v)) owl_ui_zr_set_selected(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "Sens") == 0 || strcasecmp(prop, "Direction") == 0) {
            /* zrVertical est l'unique valeur : affectation acceptee (declaratif
             * ou runtime), sans effet sur le layout deja vertical. */
            return true;
        }
        if (strcasecmp(prop, "Multis\xc3\xa9lection") == 0 ||
            strcasecmp(prop, "Multiselection") == 0 ||
            strcasecmp(prop, "MultiSelection") == 0) {
            bool b = IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v);
            owl_ui_zr_set_multi_select(w, b ? 1 : 0);
            return true;
        }
    }

    (void)kind;

    /*  — Champ Onglet : ..Valeur (1-based) pilote le volet actif.
     * setCurrentIndex avec signaux bloques (owl_ui_onglet_set_current) : le clic
     * user est la seule source de currentChanged ; un set programmatique ne
     * re-tire donc pas surChangementVolet (source unique = hook OP_STRUCT_SET).
     * Intercept avant le handler generique Valeur->set_text (no-op ruissellement). */
    if (kind == OWL_CONTROL_ONGLET) {
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0) {
            if (IS_INTEGER(v)) owl_ui_onglet_set_current(w, (int)AS_INTEGER(v) - 1);
            return true;
        }
        /*  — les 3 reglages de la barre d'onglets sont pilotables apres
         * l'ouverture, pas seulement a la declaration. Sans ces branches, toute
         * ecriture tombait dans le fallback "non supportee" en fin de fonction :
         * la propriete etait bien rangee dans le store (donc relue a l'identique)
         * mais le widget ne bougeait jamais. Memes valeurs d'enum et memes
         * defauts qu'a la construction. */
        if (strcasecmp(prop, "Position") == 0) {
            if (IS_INTEGER(v)) owl_ui_onglet_set_position(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "LargeurVolets") == 0 ||
            strcasecmp(prop, "TabWidth") == 0) {
            if (IS_INTEGER(v)) owl_ui_onglet_set_tab_width_mode(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "AlignementLibelle") == 0 ||
            strcasecmp(prop, "LabelAlignment") == 0) {
            if (IS_INTEGER(v)) owl_ui_onglet_set_label_alignment(w, (int)AS_INTEGER(v));
            return true;
        }
    }

    /* Champ Table : HauteurTitre pilote la bande de titre APRES l'ouverture,
     * pas seulement a la declaration. Sans cette branche l'ecriture tomberait
     * dans le fallback de fin de fonction : rangee dans le store, jamais
     * poussee au widget. */
    if (kind == OWL_CONTROL_TABLE) {
        if (strcasecmp(prop, "HauteurTitre") == 0 ||
            strcasecmp(prop, "TitleHeight") == 0) {
            if (IS_INTEGER(v)) owl_ui_table_set_title_height(w, (int)AS_INTEGER(v));
            return true;
        }
        /*  — ModeLoupe/MagnifierMode : prop enum "store-only" cote etat
         * (le store d'instance, source de verite lue par owl_ui_table_user_mode_
         * loupe, a deja ete ecrit en amont par OP_STRUCT_SET). Aucun etat a poser
         * sur le widget : la frappe inline relit le mode a chaque touche. On
         * repeint juste l'en-tete pour que l'icone (loupe/entonnoir) suive le
         * nouveau mode. Sans cette reconnaissance, l'ecriture tombait dans le
         * fallback "non supportee" et l'icone ne se rafraichissait pas. */
        if (strcasecmp(prop, "ModeLoupe") == 0 ||
            strcasecmp(prop, "MagnifierMode") == 0) {
            owl_ui_table_header_refresh(w);
            return true;
        }
    }

    /*  — Volet : Libelle retitre l'onglet, Visible masque/affiche la
     * languette (pas le contenu de la page). Interceptes avant les handlers
     * generiques Libelle->set_text et Visible->set_visible. */
    if (kind == OWL_CONTROL_VOLET) {
        if (strcasecmp(prop, "Libelle") == 0 || strcasecmp(prop, "Caption") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_volet_set_libelle(w, s ? s->chars : "");
            return true;
        }
        if (strcasecmp(prop, "Visible") == 0) {
            bool b = IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v);
            owl_ui_volet_set_visible(w, b ? 1 : 0);
            return true;
        }
    }

    /* D.3 — Application d'un Style aggregé sur le widget.
     * 'LIB_X..Style = STY_Y' propage les sous-champs (Couleur, CouleurFond,
     * marges, Police imbriquee, Cadre imbrique) aux helpers Qt6 du widget. */
    if (strcasecmp(prop, "Style") == 0) {
        if (IS_INSTANCE(v)) {
            /*  — relie l'instance Style au widget (watch + racine)
             * pour repeindre sur mutation ulterieure d'un overlay d'etat. */
            inst_widget_watch_tag_recursive(AS_INSTANCE(v), w);
            style_root_record(w, AS_INSTANCE(v));
            apply_style_to_control(w, AS_INSTANCE(v), kind);
        }
        return true;
    }

    /*  — Saisie : 'Libelle'/'Caption' designe le LIBELLE INTEGRE
     * (composite a gauche), PAS la zone de saisie. 'LargeurLibelle' pilote sa
     * largeur. Intercept AVANT le branchement generique wantsLib->set_text qui
     * ecrirait le libelle dans la zone de saisie (ruissellement). */
    if (kind == OWL_CONTROL_INPUT) {
        if (strcasecmp(prop, "Libelle") == 0 || strcasecmp(prop, "Caption") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_lineedit_set_label_text(w, s ? s->chars : "");
            return true;
        }
        if (strcasecmp(prop, "LargeurLibelle") == 0 || strcasecmp(prop, "LabelWidth") == 0) {
            owl_ui_lineedit_set_label_width(w, IS_INTEGER(v) ? (int)AS_INTEGER(v) : 0);
            return true;
        }
    }

    /*  — Combo : 'Libelle'/'Caption' designe le LIBELLE INTEGRE
     * (composite a gauche du QComboBox), PAS le texte du champ. 'LargeurLibelle'
     * pilote sa largeur. Intercept AVANT wantsLib->set_text (ruissellement). */
    if (kind == OWL_CONTROL_COMBO) {
        if (strcasecmp(prop, "Libelle") == 0 || strcasecmp(prop, "Caption") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_combo_set_label_text(w, s ? s->chars : "");
            return true;
        }
        if (strcasecmp(prop, "LargeurLibelle") == 0 || strcasecmp(prop, "LabelWidth") == 0) {
            owl_ui_combo_set_label_width(w, IS_INTEGER(v) ? (int)AS_INTEGER(v) : 0);
            return true;
        }
    }

    bool wantsLib  = strcasecmp(prop, "Libelle") == 0 || strcasecmp(prop, "Caption") == 0;
    bool wantsDef  = prop_is_default(prop);

    /*  — pour Selecteur/Liste/Combo/Interrupteur, la propriete par
     * defaut 'Valeur' designe une SELECTION (index / bitmask), pas un texte.
     * Sans ce garde, 'Valeur' tombait dans set_text (no-op visuel sur ces
     * widgets) puis return, court-circuitant le handler de selection plus bas
     * (la radio ne se cochait jamais). On laisse 'Valeur' atteindre son
     * handler specifique pour ces 4 kinds ; 'Libelle' garde son set_text. */
    bool defIsSelection = wantsDef &&
        (kind == OWL_CONTROL_SELECTEUR || kind == OWL_CONTROL_LISTE ||
         kind == OWL_CONTROL_COMBO    || kind == OWL_CONTROL_INTERRUPTEUR ||
         kind == OWL_CONTROL_JAUGE);

    if ((wantsLib || wantsDef) && !defIsSelection) {
        /*  — Saisie num/monetaire : ..Valeur = X stocke la valeur
         * typee (source de verite) et affiche sa forme numerique. Les autres
         * types de saisie tombent dans le set_text chaine generique. */
        if (kind == OWL_CONTROL_INPUT && wantsDef) {
            int it = owl_ui_control_get_input_type(w);
            if (it == OWL_TYPESAISIE_NUM || it == OWL_TYPESAISIE_MONETAIRE ||
                it == OWL_TYPESAISIE_NUM_PRECIS ||
                it == OWL_TYPESAISIE_DATE || it == OWL_TYPESAISIE_HEURE ||
                it == OWL_TYPESAISIE_DUREE) {
                const char* mask = (it == OWL_TYPESAISIE_DATE)  ? input_date_mask_for(c)
                                 : (it == OWL_TYPESAISIE_HEURE) ? input_time_mask_for(c)
                                 : (it == OWL_TYPESAISIE_DUREE) ? input_duree_mask_for(c)
                                 : NULL;
                FnbConfig fnbBuf; const FnbConfig* fnb = NULL;
                if (it == OWL_TYPESAISIE_NUM && input_num_format_for(c, &fnbBuf))
                    fnb = &fnbBuf;
                FmnConfig fmnBuf; const FmnConfig* fmn = NULL;
                if (it == OWL_TYPESAISIE_MONETAIRE && input_mon_format_for(c, &fmnBuf))
                    fmn = &fmnBuf;
                input_typed_set(w, it, v, mask, fnb, fmn);
                return true;
            }
        }
        ObjString* s = value_to_string(v);
        owl_ui_control_set_text(w, s ? s->chars : "");
        return true;
    }

    /* --- Champ Liste / Combo : setters communs --- */
    if (kind == OWL_CONTROL_LISTE || kind == OWL_CONTROL_COMBO) {
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0) {
            if (IS_INTEGER(v)) owl_ui_listcombo_set_selection(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "Triee") == 0 || strcasecmp(prop, "Sorted") == 0) {
            owl_ui_listcombo_set_sorted(w, (IS_BOOL(v) && AS_BOOL(v)) ? 1 : 0);
            return true;
        }
        if (strcasecmp(prop, "Contenu") == 0 || strcasecmp(prop, "Content") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_listcombo_set_content(w, s ? s->chars : "");
            return true;
        }
    }
    /* Liste-only setters. */
    if (kind == OWL_CONTROL_LISTE) {
        if (strcasecmp(prop, "Multiselection") == 0 ||
            strcasecmp(prop, "MultiSelection") == 0) {
            owl_ui_list_set_multiselect(w, (IS_BOOL(v) && AS_BOOL(v)) ? 1 : 0);
            return true;
        }
        if (strcasecmp(prop, "OptionTri") == 0 || strcasecmp(prop, "SortOption") == 0) {
            if (IS_INTEGER(v)) owl_ui_list_set_sort_option(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "Zoom") == 0) {
            if (IS_INTEGER(v)) owl_ui_list_set_zoom(w, (int)AS_INTEGER(v));
            return true;
        }
    }
    /* Interrupteur setters. */
    if (kind == OWL_CONTROL_INTERRUPTEUR) {
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0) {
            if (IS_INTEGER(v)) owl_ui_interrupteur_set_bitmask(w, AS_INTEGER(v));
            else if (IS_NIL(v)) owl_ui_interrupteur_set_bitmask(w, 0);
            return true;
        }
        if (strcasecmp(prop, "NombreColonne") == 0 ||
            strcasecmp(prop, "ColumnCount") == 0) {
            if (IS_INTEGER(v)) owl_ui_interrupteur_set_column_count(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "HauteurOption") == 0 ||
            strcasecmp(prop, "OptionHeight") == 0) {
            if (IS_INTEGER(v)) owl_ui_interrupteur_set_option_height(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "ModeAffichage") == 0 ||
            strcasecmp(prop, "DisplayMode") == 0) {
            if (IS_INTEGER(v)) owl_ui_interrupteur_set_display_mode(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "TroisEtats") == 0 ||
            strcasecmp(prop, "ThreeStates") == 0) {
            owl_ui_interrupteur_set_three_states(w, (IS_BOOL(v) && AS_BOOL(v)) ? 1 : 0);
            return true;
        }
        if (strcasecmp(prop, "Options") == 0) {
            /*  — un non-tableau (ex : chaine multi-ligne d'un `[` en fin
             * de ligne) est refuse en amont, mode-independant, par
             * control_logic_after_set_impl (soft-error). Ici (chemin Qt/GUI) on
             * ne repeuple le widget QUE pour un tableau ; sinon on preserve. */
            if (IS_ARRAY(v)) {
                owl_ui_interrupteur_clear(w);
                ObjArray* arr = AS_ARRAY(v);
                for (int i = 0; i < arr->elements.count; i++) {
                    Value el = arr->elements.values[i];
                    if (IS_STRING(el)) {
                        owl_ui_interrupteur_add_option(w, AS_CSTRING(el), NULL, NULL);
                    } else if (IS_INSTANCE(el)) {
                        ObjInstance* inst = AS_INSTANCE(el);
                        ObjString* kLib = obj_copy_string("Libelle", 7);
                        ObjString* kSub = obj_copy_string("SousLibelle", 11);
                        ObjString* kImg = obj_copy_string("Image", 5);
                        Value vLib = NIL_VAL, vSub = NIL_VAL, vImg = NIL_VAL;
                        table_get(&inst->fields, kLib, &vLib);
                        table_get(&inst->fields, kSub, &vSub);
                        table_get(&inst->fields, kImg, &vImg);
                        const char* lib = IS_STRING(vLib) ? AS_CSTRING(vLib) : "";
                        const char* sub = IS_STRING(vSub) ? AS_CSTRING(vSub) : NULL;
                        const char* img = IS_STRING(vImg) ? AS_CSTRING(vImg) : NULL;
                        owl_ui_interrupteur_add_option(w, lib, sub, img);
                    }
                }
            }
            return true;
        }
    }

    /* Champ Jauge setters. Valeur -> QProgressBar::setValue (clampe natif).
     *  — les props numeriques entieres coercent le reel vers entier
     * (troncature). Une division OWL produit toujours un reel
     * (JAU..Valeur = n * 100 / total) : sans cette coercion, le reel ne matchait
     * aucune branche, setValue n'etait jamais appele et la jauge restait figee
     * (drop silencieux). Le store est coerce en parallele (control_logic_after_set_impl)
     * pour que headless et widget concordent. */
    if (kind == OWL_CONTROL_JAUGE) {
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0) {
            if (IS_INTEGER(v)) owl_ui_jauge_set_value(w, (int)AS_INTEGER(v));
            else if (IS_REAL(v)) owl_ui_jauge_set_value(w, (int)AS_REAL(v));
            else if (IS_NIL(v)) owl_ui_jauge_set_value(w, owl_ui_jauge_get_min(w));
            return true;
        }
        if (strcasecmp(prop, "BorneMin") == 0 || strcasecmp(prop, "MinValue") == 0) {
            if (IS_INTEGER(v))
                owl_ui_jauge_set_range(w, (int)AS_INTEGER(v), owl_ui_jauge_get_max(w));
            else if (IS_REAL(v))
                owl_ui_jauge_set_range(w, (int)AS_REAL(v), owl_ui_jauge_get_max(w));
            return true;
        }
        if (strcasecmp(prop, "BorneMax") == 0 || strcasecmp(prop, "MaxValue") == 0) {
            if (IS_INTEGER(v))
                owl_ui_jauge_set_range(w, owl_ui_jauge_get_min(w), (int)AS_INTEGER(v));
            else if (IS_REAL(v))
                owl_ui_jauge_set_range(w, owl_ui_jauge_get_min(w), (int)AS_REAL(v));
            return true;
        }
        if (strcasecmp(prop, "Vertical") == 0) {
            owl_ui_jauge_set_vertical(w, (IS_BOOL(v) && AS_BOOL(v)) ? 1 : 0);
            return true;
        }
        if (strcasecmp(prop, "Symetrique") == 0 || strcasecmp(prop, "Symmetric") == 0) {
            owl_ui_jauge_set_symmetric(w, (IS_BOOL(v) && AS_BOOL(v)) ? 1 : 0);
            return true;
        }
        if (strcasecmp(prop, "CouleurJauge") == 0 ||
            strcasecmp(prop, "ProgressBarColor") == 0) {
            if (IS_INTEGER(v)) owl_ui_jauge_set_progress_color(w, (int)AS_INTEGER(v));
            else if (IS_REAL(v)) owl_ui_jauge_set_progress_color(w, (int)AS_REAL(v));
            return true;
        }
        if (strcasecmp(prop, "TexteJauge") == 0 ||
            strcasecmp(prop, "TextProgressBar") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_jauge_set_text(w, s ? s->chars : "");
            return true;
        }
        if (strcasecmp(prop, "CadrageHorizontal") == 0 ||
            strcasecmp(prop, "HorizontalAlignment") == 0) {
            if (IS_INTEGER(v)) owl_ui_jauge_set_h_align(w, (int)AS_INTEGER(v));
            else if (IS_REAL(v)) owl_ui_jauge_set_h_align(w, (int)AS_REAL(v));
            return true;
        }
    }

    /* Combo-only setters. */
    if (kind == OWL_CONTROL_COMBO) {
        if (strcasecmp(prop, "EnSaisie") == 0 || strcasecmp(prop, "Editable") == 0) {
            owl_ui_combo_set_editable(w, (IS_BOOL(v) && AS_BOOL(v)) ? 1 : 0);
            return true;
        }
        if (strcasecmp(prop, "MaxLignesAffichees") == 0 ||
            strcasecmp(prop, "MaxVisibleItems") == 0) {
            if (IS_INTEGER(v)) owl_ui_combo_set_max_visible(w, (int)AS_INTEGER(v));
            return true;
        }
    }
    /* Selecteur setters. */
    if (kind == OWL_CONTROL_SELECTEUR) {
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0) {
            if (IS_INTEGER(v)) {
                owl_ui_selecteur_set_selection_index(w, (int)AS_INTEGER(v));
            } else if (IS_STRING(v)) {
                /* Lookup par stored value. */
                int n = owl_ui_selecteur_count(w);
                const char* needle = AS_CSTRING(v);
                for (int i = 1; i <= n; i++) {
                    const char* s = owl_ui_selecteur_get_option_stored(w, i);
                    if (s && strcmp(s, needle) == 0) {
                        owl_ui_selecteur_set_selection_index(w, i);
                        return true;
                    }
                }
                /* Si chaine vide ou pas trouvee : deselectionne. */
                owl_ui_selecteur_set_selection_index(w, 0);
            } else if (IS_NIL(v)) {
                owl_ui_selecteur_set_selection_index(w, 0);
            }
            return true;
        }
        if (strcasecmp(prop, "NombreColonne") == 0 ||
            strcasecmp(prop, "ColumnCount") == 0) {
            if (IS_INTEGER(v)) owl_ui_selecteur_set_column_count(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "HauteurOption") == 0 ||
            strcasecmp(prop, "OptionHeight") == 0) {
            if (IS_INTEGER(v)) owl_ui_selecteur_set_option_height(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "ModeAffichage") == 0 ||
            strcasecmp(prop, "DisplayMode") == 0) {
            if (IS_INTEGER(v)) owl_ui_selecteur_set_display_mode(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "Options") == 0) {
            /* Reconstruit la liste a partir du nouveau tableau. */
            /*  — un non-tableau (ex : chaine multi-ligne d'un `[` en fin
             * de ligne) est refuse en amont, mode-independant, par
             * control_logic_after_set_impl (soft-error). Ici (chemin Qt/GUI) on
             * ne repeuple le widget QUE pour un tableau ; sinon on preserve. */
            if (IS_ARRAY(v)) {
                owl_ui_selecteur_clear(w);
                ObjArray* arr = AS_ARRAY(v);
                for (int i = 0; i < arr->elements.count; i++) {
                    Value el = arr->elements.values[i];
                    if (IS_STRING(el)) {
                        owl_ui_selecteur_add_option(w, AS_CSTRING(el),
                                                     NULL, NULL, NULL);
                    } else if (IS_INSTANCE(el)) {
                        ObjInstance* inst = AS_INSTANCE(el);
                        ObjString* kLib = obj_copy_string("Libelle", 7);
                        ObjString* kSub = obj_copy_string("SousLibelle", 11);
                        ObjString* kVal = obj_copy_string("ValeurRenvoyee", 14);
                        ObjString* kImg = obj_copy_string("Image", 5);
                        Value vLib = NIL_VAL, vSub = NIL_VAL, vVal = NIL_VAL, vImg = NIL_VAL;
                        table_get(&inst->fields, kLib, &vLib);
                        table_get(&inst->fields, kSub, &vSub);
                        table_get(&inst->fields, kVal, &vVal);
                        table_get(&inst->fields, kImg, &vImg);
                        const char* lib = IS_STRING(vLib) ? AS_CSTRING(vLib) : "";
                        const char* sub = IS_STRING(vSub) ? AS_CSTRING(vSub) : NULL;
                        const char* val = IS_STRING(vVal) ? AS_CSTRING(vVal) : NULL;
                        const char* img = IS_STRING(vImg) ? AS_CSTRING(vImg) : NULL;
                        owl_ui_selecteur_add_option(w, lib, sub, val, img);
                    }
                }
            }
            return true;
        }
    }

    if (strcasecmp(prop, "Visible") == 0) {
        bool b = IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v);
        owl_ui_control_set_visible(w, b ? 1 : 0);
        return true;
    }
    if (strcasecmp(prop, "Grise") == 0 || strcasecmp(prop, "Disabled") == 0) {
        bool b = IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v);
        owl_ui_control_set_enabled(w, b ? 0 : 1);
        return true;
    }
    if (strcasecmp(prop, "Largeur") == 0 || strcasecmp(prop, "Width") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_width(w, (int)AS_INTEGER(v));
        return true;
    }
    if (strcasecmp(prop, "Hauteur") == 0 || strcasecmp(prop, "Height") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_height(w, (int)AS_INTEGER(v));
        return true;
    }
    /*  — écriture de position sur un champ placé par une disposition
     * automatique : refus explicite (soft-error), le champ ne bouge pas. La
     * disposition calcule intégralement la position ; l'écrire n'aurait aucun
     * effet persistant. En disposition libre, le déplacement absolu fonctionne
     * comme avant. */
    if (strcasecmp(prop, "X") == 0) {
        if (owl_ui_control_is_layout_managed(w)) {
            owl_emit_native_error(E_RT_UI_LAYOUT_POSITION_FIXED, c->name,
                                  "name", c->name, "prop", prop, NULL);
            return true;
        }
        if (IS_INTEGER(v)) owl_ui_control_set_x(w, (int)AS_INTEGER(v));
        return true;
    }
    if (strcasecmp(prop, "Y") == 0) {
        if (owl_ui_control_is_layout_managed(w)) {
            owl_emit_native_error(E_RT_UI_LAYOUT_POSITION_FIXED, c->name,
                                  "name", c->name, "prop", prop, NULL);
            return true;
        }
        if (IS_INTEGER(v)) owl_ui_control_set_y(w, (int)AS_INTEGER(v));
        return true;
    }

    /* --- Bulle (commun aux 3 controles) --- */
    if (strcasecmp(prop, "Bulle") == 0 || strcasecmp(prop, "Tooltip") == 0) {
        ObjString* s = value_to_string(v);
        owl_ui_control_set_tooltip(w, s ? s->chars : "");
        return true;
    }

    /* --- Police composee : accepte une INSTANCE Police.  lot 3 — la
     * valeur a deja ete ecrite dans le store de l'uici par l'appelant (vm.c) ;
     * on la relie au widget (watcher direct) et on apply a Qt. Un scalaire est
     * deja rejete au compile-time (prop OBJECT). */
    if (strcasecmp(prop, "Police") == 0 || strcasecmp(prop, "Font") == 0) {
        if (IS_INSTANCE(v)) {
            inst_widget_watch_tag_recursive(AS_INSTANCE(v), w);
            apply_police_instance_to_widget(w, AS_INSTANCE(v));
        }
        return true;
    }
    /* --- Raccourcis Police plats : ecrivent le sous-champ de l'instance
     * (source de verite), puis re-apply au QWidget. Equivalence bidirection-
     * nelle avec la cascade ..Police..XXX garantie (meme instance). --- */
    {
        const char* polSub = NULL;
        Value subVal = v;
        if (strcasecmp(prop, "PoliceNom") == 0 || strcasecmp(prop, "FontName") == 0) {
            polSub = "Nom";
            ObjString* s = value_to_string(v);
            subVal = OBJ_VAL(s ? s : obj_copy_string("", 0));
        } else if (strcasecmp(prop, "PoliceTaille") == 0 || strcasecmp(prop, "FontSize") == 0) {
            polSub = "Taille";
        } else if (strcasecmp(prop, "PoliceGras") == 0 || strcasecmp(prop, "FontBold") == 0) {
            polSub = "Gras";
            subVal = BOOL_VAL(IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v));
        } else if (strcasecmp(prop, "PoliceItalique") == 0 || strcasecmp(prop, "FontItalic") == 0) {
            polSub = "Italique";
            subVal = BOOL_VAL(IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v));
        } else if (strcasecmp(prop, "PoliceSoulignee") == 0 ||
                   strcasecmp(prop, "FontUnderlined") == 0) {
            polSub = "Souligne";
            subVal = BOOL_VAL(IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v));
        } else if (strcasecmp(prop, "PoliceBarree") == 0 ||
                   strcasecmp(prop, "FontStrikeOut") == 0) {
            polSub = "Barre";
            subVal = BOOL_VAL(IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v));
        } else if ((strcasecmp(prop, "PoliceCouleur") == 0 ||
                    strcasecmp(prop, "FontColor") == 0)
                   && kind != OWL_CONTROL_INPUT
                   && kind != OWL_CONTROL_LABEL
                   && kind != OWL_CONTROL_BUTTON) {
            /*  — sur un Champ Saisie, PoliceCouleur est un alias vers
             * Style..ZoneSaisie..Couleur (bloc dedie plus bas), pas vers
             * l'instance Police.  — Libellé/Bouton : alias vers
             * Style..Libellé..Couleur (bloc mono dedie plus bas). Les autres
             * widgets stylés gardent la couleur du texte via l'instance Police. */
            polSub = "Couleur";
        }
        if (polSub != NULL) {
            vm_push(subVal);  /* root pendant clone eventuel + table_set */
            /*  lot 3 — ecrit le sous-champ dans l'instance Police du
             * store de l'uici (source de verite unique), puis re-apply Qt. */
            ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
            ObjInstance* pol = uici ? uici_composite_instance(uici, "Police", "Police")
                                    : NULL;
            if (pol != NULL) {
                /*  lot 6 : root pol + key fresh avant table_set qui
                 * peut realloc et declencher le GC. subVal deja root L2387. */
                vm_push(OBJ_VAL(pol));
                ObjString* sk = table_find_key_case_insensitive(&pol->fields, polSub);
                ObjString* key = sk != NULL
                    ? sk : obj_copy_string(polSub, (int)strlen(polSub));
                /*  : delegue au canon obj_inst_set_kv. */
                obj_inst_set_kv(pol, key, subVal);
                vm_pop(); /* pol */
                apply_police_instance_to_widget(w, pol);
            }
            vm_pop();
            return true;
        }
    }

    /*  — raccourcis d'apparence plats du Champ Saisie : alias fins vers
     * la ZoneSaisie de l'instance Style du store (verite unique, calque du
     * pattern Police). `CouleurFond`->ZoneSaisie..CouleurFond,
     * `PoliceCouleur`->ZoneSaisie..Couleur (texte). Le store a deja ete mis a
     * jour par vm.c (branche IS_UI_CONTROL_REF) ; ici on materialise l'instance
     * Style, on la relie au widget (watch + racine) et on rejoue la feuille
     * COMPLETE — un seul apply Qt, plus de double-ecriture directe. Gate INPUT :
     * Libelle/Bouton gardent le chemin plat direct plus bas. */
    if (kind == OWL_CONTROL_INPUT) {
        const char* zsSub = NULL;
        Value subVal = v;
        if (strcasecmp(prop, "CouleurFond") == 0 ||
            strcasecmp(prop, "BackgroundColor") == 0) {
            zsSub = "CouleurFond";
        } else if (strcasecmp(prop, "PoliceCouleur") == 0 ||
                   strcasecmp(prop, "FontColor") == 0) {
            zsSub = "Couleur";
        }
        if (zsSub != NULL) {
            ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
            ObjInstance* style = uici
                ? uici_composite_instance(uici, "Style", "styleSaisie")
                : NULL;
            if (style != NULL) {
                ObjInstance* zone = instance_get_instance(style, "ZoneSaisie");
                if (zone != NULL) {
                    vm_push(subVal);   /* root pendant table_set (peut grow -> GC) */
                    vm_push(OBJ_VAL(zone));
                    ObjString* sk =
                        table_find_key_case_insensitive(&zone->fields, zsSub);
                    ObjString* key = sk != NULL
                        ? sk : obj_copy_string(zsSub, (int)strlen(zsSub));
                    obj_inst_set_kv(zone, key, subVal);
                    vm_pop(); /* zone */
                    vm_pop(); /* subVal */
                }
                inst_widget_watch_tag_recursive(style, w);
                style_root_record(w, style);
                apply_style_to_control(w, style, kind);
            }
            return true;
        }
    }

    /*  — raccourcis d'apparence plats des widgets mono (Libellé/Bouton) :
     * alias fins vers une sous-brique de l'instance Style du store (verite unique,
     * calque du pattern Saisie ). CouleurFond->CadreExtérieur..CouleurFond
     * (le fond), PoliceCouleur->Libellé..Couleur (le texte). Le store a deja ete
     * mis a jour par vm.c (branche IS_UI_CONTROL_REF) ; ici on materialise
     * l'instance Style, on la relie au widget (watch + racine) et on rejoue la
     * feuille COMPLETE — un seul apply Qt, plus de double-ecriture directe. */
    {
        const char* mBrick = NULL;
        const char* mSub   = NULL;
        if (mono_style_shortcut_bricksub(kind, prop, &mBrick, &mSub)) {
            ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
            ObjInstance* style = uici
                ? uici_composite_instance(uici, "Style",
                                          mono_style_template_name(kind))
                : NULL;
            if (style != NULL) {
                ObjInstance* brick = instance_get_instance(style, mBrick);
                if (brick != NULL) {
                    vm_push(v);   /* root pendant table_set (peut grow -> GC) */
                    vm_push(OBJ_VAL(brick));
                    ObjString* sk =
                        table_find_key_case_insensitive(&brick->fields, mSub);
                    ObjString* key = sk != NULL
                        ? sk : obj_copy_string(mSub, (int)strlen(mSub));
                    obj_inst_set_kv(brick, key, v);
                    vm_pop(); /* brick */
                    vm_pop(); /* v */
                }
                inst_widget_watch_tag_recursive(style, w);
                style_root_record(w, style);
                apply_style_to_control(w, style, kind);
            }
            return true;
        }
    }

    /* --- Cadrage (Libelle + Saisie) --- */
    if (strcasecmp(prop, "CadrageHorizontal") == 0 ||
        strcasecmp(prop, "HorizontalAlignment") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_h_align(w, (int)AS_INTEGER(v));
        return true;
    }
    if (strcasecmp(prop, "CadrageVertical") == 0 ||
        strcasecmp(prop, "VerticalAlignment") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_v_align(w, (int)AS_INTEGER(v));
        return true;
    }

    /* --- Etat unifie (commun aux 3 controles) --- */
    if (strcasecmp(prop, "Etat") == 0 || strcasecmp(prop, "State") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_etat(w, (int)AS_INTEGER(v));
        return true;
    }

    /* --- CurseurSouris (commun : tout widget visuel) : sentinelle OWL_CUR_*
     * (zone negative). Pas de garde >= 0 — les valeurs valides sont negatives. */
    if (strcasecmp(prop, "CurseurSouris") == 0 ||
        strcasecmp(prop, "MouseCursor") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_cursor_shape(w, (int)AS_INTEGER(v));
        return true;
    }

    /* --- Opacite (commun aux 3 controles) --- */
    if (strcasecmp(prop, "Opacite") == 0 || strcasecmp(prop, "Opacity") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_opacite(w, (int)AS_INTEGER(v));
        return true;
    }

    /* --- Etirement de police (commun) : enum -> stretch du moteur. --- */
    if (strcasecmp(prop, "PoliceEtirement") == 0 ||
        strcasecmp(prop, "FontStretch") == 0) {
        if (IS_INTEGER(v))
            owl_ui_control_set_font_stretch(w,
                owl_etirement_to_stretch((int)AS_INTEGER(v)));
        return true;
    }

    /* --- Taille minimale de police (fit) (commun). --- */
    if (strcasecmp(prop, "PoliceTailleMin") == 0 ||
        strcasecmp(prop, "FontMinSize") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_font_min_size(w, (int)AS_INTEGER(v));
        return true;
    }

    /* --- Couleur / CouleurFond (generique : tout QWidget supporte) --- */
    if (strcasecmp(prop, "Couleur") == 0 || strcasecmp(prop, "Color") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_text_color(w, (int)AS_INTEGER(v));
        return true;
    }
    if (strcasecmp(prop, "CouleurFond") == 0 ||
        strcasecmp(prop, "BackgroundColor") == 0) {
        /*  — accepte entier nu OU instance Couleur (alpha porté). */
        int cfBgr = -1, cfAlpha = 255;
        owl_value_to_color(v, &cfBgr, &cfAlpha);
        if (cfBgr >= 0) owl_ui_control_set_bg_color_alpha(w, cfBgr, cfAlpha);
        return true;
    }

    /*  — override de taille d'icone (..ImageTaille). Stocke la
     * valeur ; la prochaine assignation ..Image rasterise a cette taille. */
    if (strcasecmp(prop, "ImageTaille") == 0 ||
        strcasecmp(prop, "ImageSize") == 0) {
        if (IS_INTEGER(v)) owl_ui_control_set_icon_size_override(w, (int)AS_INTEGER(v));
        return true;
    }

    /* --- Specifiques Libelle --- */
    if (kind == OWL_CONTROL_LABEL) {
        if (strcasecmp(prop, "TypeLibelle") == 0 || strcasecmp(prop, "LabelType") == 0) {
            int t = IS_INTEGER(v) ? (int)AS_INTEGER(v) : OWL_TL_NORMAL;
            owl_ui_label_set_word_wrap(w, t == OWL_TL_MULTILIGNE ? 1 : 0);
            return true;
        }
        if (strcasecmp(prop, "Couleur") == 0 || strcasecmp(prop, "Color") == 0) {
            if (IS_INTEGER(v)) owl_ui_control_set_text_color(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "CouleurFond") == 0 ||
            strcasecmp(prop, "BackgroundColor") == 0) {
            if (IS_INTEGER(v)) owl_ui_control_set_bg_color(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "Cadre") == 0 || strcasecmp(prop, "Border") == 0) {
            /*  lot 3 — la valeur a deja ete ecrite dans le store de
             * l'uici par l'appelant (vm.c) ; on la relie au widget (watcher
             * direct) et on apply a Qt. */
            if (IS_INSTANCE(v)) {
                inst_widget_watch_tag_recursive(AS_INSTANCE(v), w);
                apply_cadre_instance_to_widget(w, AS_INSTANCE(v));
            }
            return true;
        }
        if (strcasecmp(prop, "Image") == 0) {
            if (stdlib_image_is_instance(v)) {
                int iw = 0, ih = 0;
                const unsigned char* rgba = stdlib_image_get_rgba(v, &iw, &ih);
                int cad = owl_ui_label_get_cadrage(w);
                owl_ui_label_set_image_rgba(w, rgba, iw, ih, cad);
            } else {
                ObjString* s = value_to_string(v);
                owl_ui_label_set_image(w, s ? s->chars : "");
            }
            return true;
        }
        if (strcasecmp(prop, "Cadrage") == 0 ||
            strcasecmp(prop, "Framing") == 0) {
            if (IS_INTEGER(v)) owl_ui_label_set_cadrage(w, (int)AS_INTEGER(v));
            return true;
        }
    }

    /* --- Specifiques Champ Image V2 (OwlImageLabel = subclass QLabel,
     * donc les setters Image/Cadrage du LABEL fonctionnent aussi). --- */
    if (kind == OWL_CONTROL_IMAGE) {
        if (strcasecmp(prop, "Image") == 0) {
            if (stdlib_image_is_instance(v)) {
                int iw = 0, ih = 0;
                const unsigned char* rgba = stdlib_image_get_rgba(v, &iw, &ih);
                int cad = owl_ui_label_get_cadrage(w);
                owl_ui_label_set_image_rgba(w, rgba, iw, ih, cad);
            } else if (IS_ICONE(v)) {
                /*  — Reassignation runtime CHP..Image = icones.X.
                 * Symetrique au Bouton L2671+  Cas K. */
                ObjIcone* icn = AS_ICONE(v);
                if (icn->valide && icn->svg_raw != NULL
                    && icn->svg_raw->length > 0) {
                    unsigned char* rgba = NULL;
                    int iw = 0, ih = 0;
                    /*  : forme + couleur_fond. */
                    bool want_bg = (icn->couleur_fond != -1);
                    /*  — auto = dimension du champ, override stocke. */
                    int wImg = owl_ui_control_get_width(w);
                    int hImg = owl_ui_control_get_height(w);
                    int fieldDim = (wImg < hImg) ? wImg : hImg;
                    int icoSize = icone_target_size(
                        fieldDim, owl_ui_control_get_icon_size_override(w));
                    if (owl_icone_rasterize_rgba(
                            icn->svg_raw->chars, icn->svg_raw->length,
                            /*  : -1 unset -> 0 (noir) defaut au lieu de 0xFFFFFF blanc. */
                            (icn->couleur == -1) ? 0u : (unsigned int)(icn->couleur & 0xFFFFFF),
                            icn->couleur_modifiable,
                            icoSize, icn->rotation,
                            icn->forme,
                            (unsigned int)((want_bg ? icn->couleur_fond : 0) & 0xFFFFFF),
                            want_bg,
                            &rgba, &iw, &ih)) {
                        int cad = owl_ui_label_get_cadrage(w);
                        owl_ui_label_set_image_rgba(w, rgba, iw, ih, cad);
                        free(rgba);
                    }
                }
            } else {
                ObjString* s = value_to_string(v);
                owl_ui_label_set_image(w, s ? s->chars : "");
            }
            return true;
        }
        if (strcasecmp(prop, "Cadrage") == 0 ||
            strcasecmp(prop, "Framing") == 0) {
            if (IS_INTEGER(v)) owl_ui_label_set_cadrage(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "Zoom") == 0) {
            if (IS_INTEGER(v)) owl_ui_image_field_set_zoom(w, (int)AS_INTEGER(v));
            return true;
        }
    }

    /* --- Specifiques Champ CodeBarres (OwlImageLabel) : poser le descripteur
     * complet (prop par defaut "CodeBarres") OU une metadonnee plate
     * (Contenu/TypeCodeBarres/CorrectionErreur/Options) regenere l'image et
     * met a jour le store d'instance (verite des relectures runtime). --- */
    if (kind == OWL_CONTROL_CODEBARRE) {
        bool isDefault = (strcasecmp(prop, "CodeBarres") == 0 ||
                          strcasecmp(prop, "BarCodes") == 0);
        bool isMeta = (strcasecmp(prop, "Contenu") == 0         || strcasecmp(prop, "Content") == 0 ||
                       strcasecmp(prop, "TypeCodeBarres") == 0   || strcasecmp(prop, "TypeBarCode") == 0 ||
                       strcasecmp(prop, "CorrectionErreur") == 0 || strcasecmp(prop, "CorrectionError") == 0 ||
                       strcasecmp(prop, "Option") == 0);
        if (isDefault || isMeta) {
            ObjUiControlInstance* uici = current_window_uici_by_name(c->name);
            Value imgVal = NIL_VAL;
            if (isDefault) {
                /* Adoption d'un descripteur complet. Le store retient
                 * l'instance sous "CodeBarres" pour la relecture. */
                if (IS_INSTANCE(v)) {
                    imgVal = owl_codebarre_image_value_from_instance(AS_INSTANCE(v));
                    if (uici) {
                        vm_push(v);
                        obj_ui_control_instance_headless_set(uici, "CodeBarres", v);
                        vm_pop();
                    }
                    /*  — met a jour la config regen depuis le descripteur
                     * adopte (sinon un resize re-rasteriserait l'ancien contenu).
                     * v reste reference : contenu copie immediatement cote widget. */
                    int64_t cbT, cbE, cbO; const char* cbC = NULL; int cbL = 0;
                    owl_codebarre_params_from_instance(AS_INSTANCE(v),
                                                       &cbT, &cbE, &cbO, &cbC, &cbL);
                    owl_ui_codebarre_set_config(w, cbT, cbE, cbO, cbC, cbL);
                    /*  — stocke le logo (image centrale QR) sur le
                     * widget pour qu'un regen au resize le recompositie (le
                     * chemin C plat ne repasse pas par l'instance). Instance
                     * sans ImageCentrale -> efface le logo (rgba NULL). */
                    Value imgCentreV;
                    if (instance_get_value(AS_INSTANCE(v), "ImageCentrale", &imgCentreV) &&
                        stdlib_image_is_instance(imgCentreV)) {
                        vm_push(imgCentreV);  /* root pendant le readback RGBA */
                        int lw = 0, lh = 0;
                        const unsigned char* logo =
                            stdlib_image_get_rgba(imgCentreV, &lw, &lh);
                        double prop = 0.0;
                        Value propV;
                        if (instance_get_value(AS_INSTANCE(v), "ProportionImage", &propV)) {
                            if (IS_REAL(propV))         prop = AS_REAL(propV);
                            else if (IS_INTEGER(propV)) prop = (double)AS_INTEGER(propV);
                        }
                        owl_ui_codebarre_set_logo(w, logo, lw, lh, prop);
                        vm_pop();
                    } else {
                        owl_ui_codebarre_set_logo(w, NULL, 0, 0, 0.0);
                    }
                }
            } else {
                if (uici) {
                    /* Persiste la nouvelle valeur puis regenere depuis l'etat
                     * complet du store. */
                    if (IS_OBJ(v)) vm_push(v);
                    obj_ui_control_instance_headless_set(uici, prop, v);
                    if (IS_OBJ(v)) vm_pop();
                    Value tv, cv, ev, ov;
                    int64_t type = (obj_ui_control_instance_headless_get(uici, "TypeCodeBarres", &tv)
                                    && IS_INTEGER(tv)) ? AS_INTEGER(tv) : OWL_CB_QRCODE;
                    int64_t ecc  = (obj_ui_control_instance_headless_get(uici, "CorrectionErreur", &ev)
                                    && IS_INTEGER(ev)) ? AS_INTEGER(ev) : OWL_QR_ECC_M;
                    int64_t opts = (obj_ui_control_instance_headless_get(uici, "Option", &ov)
                                    && IS_INTEGER(ov)) ? AS_INTEGER(ov) : 0;
                    const char* contenu = (obj_ui_control_instance_headless_get(uici, "Contenu", &cv)
                                           && IS_STRING(cv)) ? AS_CSTRING(cv) : "";
                    if (contenu && *contenu)
                        imgVal = owl_codebarre_image_value_from_params(
                            type, ecc, opts, contenu, (int)strlen(contenu),
                            cb_field_supersample(owl_ui_control_get_width(w)),
                            cb_field_supersample(owl_ui_control_get_height(w)));
                    /*  — met a jour la config de regen au resize avec les
                     * memes params (type/ecc/options/contenu) que ceux rendus ici. */
                    owl_ui_codebarre_set_config(w, type, ecc, opts,
                                                contenu, contenu ? (int)strlen(contenu) : 0);
                }
            }
            if (stdlib_image_is_instance(imgVal)) {
                vm_push(imgVal);
                int iw = 0, ih = 0;
                const unsigned char* rgba = stdlib_image_get_rgba(imgVal, &iw, &ih);
                owl_ui_label_set_image_rgba(w, rgba, iw, ih, OWL_CADIMG_HOMOTHETIQUE);
                vm_pop();
            }
            return true;
        }
    }

    /*  — ToucheRaccourci (focus) en ecriture imperative runtime sur les
     * champs interactifs non-bouton. Parite avec le SET Bouton (action = clic)
     * juste apres. Le getter symetrique est a ~2679. */
    if ((kind == OWL_CONTROL_INPUT || kind == OWL_CONTROL_COMBO ||
         kind == OWL_CONTROL_LISTE || kind == OWL_CONTROL_SELECTEUR ||
         kind == OWL_CONTROL_INTERRUPTEUR || kind == OWL_CONTROL_TABLE ||
         kind == OWL_CONTROL_ZONE_REPETEE) &&
        (strcasecmp(prop, "ToucheRaccourci") == 0 ||
         strcasecmp(prop, "ShortcutKey") == 0)) {
        ObjString* s = value_to_string(v);
        owl_ui_set_focus_shortcut(w, s ? s->chars : "");
        return true;
    }

    /* --- Specifiques Bouton --- */
    if (kind == OWL_CONTROL_BUTTON) {
        if (strcasecmp(prop, "ToucheRaccourci") == 0 ||
            strcasecmp(prop, "ShortcutKey") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_button_set_shortcut(w, s ? s->chars : "");
            return true;
        }
        if (strcasecmp(prop, "TypeBouton") == 0 ||
            strcasecmp(prop, "ButtonType") == 0) {
            if (IS_INTEGER(v)) owl_ui_button_set_type_bouton(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "Image") == 0) {
            /*  Cas K /  /  — chemin partage
             * (Image|Icone|chaine), unifie avec Style..Image..Image. */
            button_set_icon_value(w, v);
            return true;
        }
    }

    /* --- Specifiques Saisie --- */
    if (kind == OWL_CONTROL_INPUT) {
        if (strcasecmp(prop, "Indication") == 0 ||
            strcasecmp(prop, "Placeholder") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_lineedit_set_placeholder(w, s ? s->chars : "");
            return true;
        }
        if (strcasecmp(prop, "MaxLongueur") == 0 || strcasecmp(prop, "MaxLength") == 0) {
            if (IS_INTEGER(v)) owl_ui_lineedit_set_max_length(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "TypeSaisie") == 0 || strcasecmp(prop, "InputType") == 0) {
            int t = IS_INTEGER(v) ? (int)AS_INTEGER(v) : OWL_TYPESAISIE_TEXTE;
            owl_ui_lineedit_set_password_mode(w, t == OWL_TYPESAISIE_MOTDEPASSE ? 1 : 0);
            /* Memorise le type (GET coherent) et (re)pose le validateur
             * numerique/monetaire. Le passage vers/depuis Multiligne ne
             * reconstruit pas le widget (QLineEdit<->QPlainTextEdit) — inchangé. */
            owl_ui_control_set_input_type(w, t);
            /* /2033 — bascule vers Date/Heure/Durée : (re)pose le masque
             * de frappe guidée selon le format effectif. */
            if (t == OWL_TYPESAISIE_DATE || t == OWL_TYPESAISIE_HEURE ||
                t == OWL_TYPESAISIE_DUREE)
                input_apply_frappe_mask(c, w, t);
            return true;
        }
        if (strcasecmp(prop, "Curseur") == 0 || strcasecmp(prop, "Cursor") == 0) {
            if (IS_INTEGER(v)) owl_ui_lineedit_set_cursor(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "FinCurseur") == 0 || strcasecmp(prop, "EndCursor") == 0) {
            if (IS_INTEGER(v)) owl_ui_lineedit_set_end_cursor(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "TexteSelectionne") == 0 ||
            strcasecmp(prop, "SelectedText") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_lineedit_set_selected_text(w, s ? s->chars : "");
            return true;
        }
        if (strcasecmp(prop, "BorneMin") == 0 || strcasecmp(prop, "MinValue") == 0) {
            if (IS_INTEGER(v)) owl_ui_lineedit_set_min_value(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "BorneMax") == 0 || strcasecmp(prop, "MaxValue") == 0) {
            if (IS_INTEGER(v)) owl_ui_lineedit_set_max_value(w, (int)AS_INTEGER(v));
            return true;
        }
        if (strcasecmp(prop, "Modifie") == 0 || strcasecmp(prop, "Modified") == 0) {
            bool b = IS_BOOL(v) ? AS_BOOL(v) : !IS_NIL(v);
            owl_ui_lineedit_set_modified(w, b ? 1 : 0);
            return true;
        }
        if (strcasecmp(prop, "MasqueSaisie") == 0 ||
            strcasecmp(prop, "InputMask") == 0) {
            ObjString* s = value_to_string(v);
            owl_ui_lineedit_set_mask(w, s ? s->chars : "");
            return true;
        }
    }

    /*  — `..Note` est une propriete texte "store-only" (emplacement
     * libre disponible sur la fenetre et tous les champs, feat 1.0.5) : elle n'a
     * volontairement AUCUN setter Qt (rien a rendre). Le store d'instance a deja
     * ete applique en amont ; sans cette reconnaissance, toute ecriture ..Note
     * tombait dans le fallback ci-dessous et imprimait a tort "non supportee"
     * (support #199). No-op silencieux, succes. */
    if (strcasecmp(prop, "Note") == 0) {
        return true;
    }

    /*  — `..MasqueAffichage` est une prop texte "store-only" : le store
     * d'instance (source de verite) a deja ete ecrit en amont (vm.c). Elle n'a
     * pas de rendu Qt direct — elle est lue au SET/GET de ..Valeur d'un champ
     * Date pour piloter format et relecture (input_typed_set/get). No-op ici :
     * sans cette reconnaissance, le reflet Qt tomberait dans le fallback "non
     * supportee". */
    if (strcasecmp(prop, "MasqueAffichage") == 0 ||
        strcasecmp(prop, "DisplayMask") == 0) {
        /*  — le nouveau format effectif pilote le masque de frappe
         * guidée d'un champ Date/Heure (le store a déjà le masque à jour ->
         * input_date_mask_for/input_time_mask_for le relisent). Jetons nommés
         * -> masque effacé (saisie libre). */
        if (kind == OWL_CONTROL_INPUT) {
            int it = owl_ui_control_get_input_type(w);
            if (it == OWL_TYPESAISIE_DATE || it == OWL_TYPESAISIE_HEURE ||
                it == OWL_TYPESAISIE_DUREE)
                input_apply_frappe_mask(c, w, it);
        }
        return true;
    }

    /* CONTAINER fallback : ZR[i].sub = v ou Conteneur.sub = v.
     * Le proxy ZR[i] expose un kind=CONTAINER avec widget=rep ; quand le
     * caller ecrit '.sub = v' (raccourci sans ..Valeur), on resout sub
     * comme widget enfant et on dispatche v comme valeur par defaut. */
    if (kind == OWL_CONTROL_CONTAINER) {
        OwlControl* child = owl_ui_control_find_child(w, prop);
        if (child) {
            int childKind = owl_ui_control_get_widget_kind(child);
            if (childKind < 0) childKind = OWL_CONTROL_CONTAINER;
            ObjUiControl* sub = obj_new_ui_control(childKind, child, prop);
            /* Recurse vers la prop par defaut (Valeur) — qui est canon
             * pour le raccourci '<champ> = v'. */
            return owl_ui_control_set_prop(sub, "Valeur", v);
        }
    }

    fprintf(stderr,
        "[ui] Ecriture runtime '%s..%s = ...' non supportee.\n",
        c->name ? c->name : "?", prop);
    return false;
}

/*  lot 2 — impl du hook apply_instance_scalar. Une ecriture scalaire
 * (..Libelle = "x", ..X, ..Largeur, ...) atterrit dans le headless_store de
 * l'ObjUiControlInstance. Quand celle-ci porte un widget Qt monte, on rejoue
 * la valeur sur le QWidget via real_ui_set_prop (meme dispatch que les setters
 * existants) — un proxy ObjUiControl ephemere fournit widget/kind/name.
 *
 * Pas de boucle : real_ui_set_prop n'ecrit que sur Qt (owl_ui_control_set_*),
 * jamais dans le store. */
static void apply_instance_scalar_impl(ObjUiControlInstance* uici,
                                       const char* prop, Value v) {
    if (!uici || !uici->widget || !prop || !uici->decl) return;
    /*  : push v (si Obj) pendant real_ui_set_prop qui peut allouer
     * (coerce string → multilingue, prop setter alloue str enfant, etc.).
     * v passe par valeur sur la C-stack callee, non rooted sans ce push. */
    bool vIsObj = IS_OBJ(v);
    if (vIsObj) vm_push(v);
    ObjUiControl proxy;
    proxy.obj.type       = OBJ_UI_CONTROL;
    proxy.kind           = (int)uici->decl->kind;
    proxy.name           = uici->decl->name ? uici->decl->name : "?";
    proxy.widget         = uici->widget;
    proxy.headless_store = NULL;
    real_ui_set_prop(&proxy, prop, v);
    if (vIsObj) vm_pop();
}

/* ====================================================================
 *   — logique metier Champ Onglet / Volet apres set
 * ==================================================================== */

/* Un volet est visible sauf si son store porte explicitement Visible = Faux
 * (bool false). Absent du store -> visible par defaut. */
static bool onglet_uici_is_visible(ObjUiControlInstance* u) {
    Value v;
    if (obj_ui_control_instance_headless_get(u, "Visible", &v) && IS_BOOL(v))
        return AS_BOOL(v);
    return true;
}

/* Renvoie le volet (kind VOLET) d'indice 1-based `idx1` parmi les volets du
 * Champ Onglet `ongName`, dans l'ordre de declaration (= ordre de win->controls).
 * NULL si l'indice depasse le nombre de volets. */
static ObjUiControlInstance* onglet_volet_at(ObjWindowInstance* win,
                                             const char* ongName, int idx1) {
    int n = 0;
    for (int i = 0; i < win->control_count; i++) {
        ObjUiControlInstance* c = win->controls[i];
        if (!c || !c->decl) continue;
        if ((OwlControlKind)c->decl->kind != OWL_CONTROL_VOLET) continue;
        if (!c->decl->parent_name
            || strcasecmp(c->decl->parent_name, ongName) != 0) continue;
        if (++n == idx1) return c;
    }
    return NULL;
}

/* Tire l'event surChangementVolet / onPaneChange du Champ Onglet `ong` (s'il
 * en declare un). Sender = la ref globale du champ pour que MoiMême resolve. */
static void onglet_fire_change_event(ObjUiControlInstance* ong) {
    if (!ong || !ong->decl) return;
    const OwlEventBlock* ev = find_event(ong->decl->events, ong->decl->event_count,
                                         "surChangementVolet", "onPaneChange");
    if (!ev || !ev->impl) return;
    Value sender = moimeme_widget_value_for_name(ong->decl->name);
    call_event_block_with_sender(ev->impl, false, sender);
}

/* Callback GUI du clic utilisateur sur un onglet (QTabWidget::currentChanged).
 * userdata = ObjUiControlInstance* du Champ Onglet (rooted tant que la fenetre
 * vit, GC non-moving). Met a jour le store ..Valeur (ecriture DIRECTE, ne
 * repasse pas par OP_STRUCT_SET -> pas de double-fire) puis tire l'event UNE
 * fois. Un set programmatique bloque les signaux : ce callback n'est alors pas
 * appele, l'event vient du hook OP_STRUCT_SET. */
static void dispatch_onglet_change(OwlControl* sender, void* userdata, int newIndex0) {
    (void)sender;
    ObjUiControlInstance* ong = (ObjUiControlInstance*)userdata;
    if (!ong || !ong->decl || newIndex0 < 0) return;
    long newVal = (long)newIndex0 + 1;
    long curVal = 1;
    Value cur;
    if (obj_ui_control_instance_headless_get(ong, "Valeur", &cur) && IS_INTEGER(cur))
        curVal = AS_INTEGER(cur);
    if (curVal == newVal) return;              /* pas de changement reel */
    obj_ui_control_instance_headless_set(ong, "Valeur", INTEGER_VAL(newVal));
    onglet_fire_change_event(ong);
}

/* Hook OP_STRUCT_SET (vm.c) — appele apres toute ecriture scalaire dans le
 * store d'une ObjUiControlInstance, headless ET GUI. Deux comportements :
 *
 *  1. ONGLET..Valeur change reellement (old != new) -> tire surChangementVolet.
 *     Reaffecter la meme valeur ne retire pas l'event (canon « Modification du
 *     volet affiche » = changement reel).
 *  2. VOLET..Visible = Faux sur le volet ACTIF -> reporte ..Valeur du parent
 *     Onglet sur le volet visible le plus proche (suivant dans l'ordre de
 *     declaration, sinon precedent visible) : aucun volet masque ne reste
 *     « affiche ». Le report tire lui aussi surChangementVolet (changement reel).
 *
 * Double-fire GUI : le setter Qt (owl_ui_apply_instance_scalar -> QTabWidget::
 * setCurrentIndex) tourne AVANT ce hook avec les signaux Qt bloques ; l'event
 * n'a donc pas ete tire par currentChanged. Ce hook est la SOURCE UNIQUE de
 * l'event pour un set programmatique. Un clic utilisateur passe par
 * currentChanged (signaux non bloques) qui met a jour le store + tire l'event
 * une seule fois, sans repasser par OP_STRUCT_SET (pas de double-fire). */
static void seed_options_headless(ObjUiControlInstance* uici, Value optsVal,
                                  bool interrupteur);
static ObjArray* widget_items_store_peek_uici(ObjUiControlInstance* uici);

static void control_logic_after_set_impl(ObjUiControlInstance* uici,
                                         const char* prop,
                                         Value old, Value neuf) {
    if (!uici || !uici->decl || !prop) return;
    OwlControlKind kind = (OwlControlKind)uici->decl->kind;

    /*  — Champ Jauge : props numeriques entieres. En OWL, `/` produit
     * toujours un reel (JAU..Valeur = num * 100 / total). Le store d'instance
     * recoit le reel brut (vm.c) alors que le widget attend un entier : sans ce
     * garde, headless relisait le reel et le widget Qt droppait la valeur
     * (setValue jamais appele) -> jauge figee, aucune erreur. On coerce le store
     * vers l'entier (troncature) pour qu'il concorde avec ce que le widget recoit
     * (real_ui_set_prop coerce le reel en parallele cote Qt). Inconditionnel
     * headless ET GUI. */
    if (kind == OWL_CONTROL_JAUGE && IS_REAL(neuf)) {
        if (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0 ||
            strcasecmp(prop, "BorneMin") == 0 || strcasecmp(prop, "MinValue") == 0 ||
            strcasecmp(prop, "BorneMax") == 0 || strcasecmp(prop, "MaxValue") == 0 ||
            strcasecmp(prop, "CouleurJauge") == 0 ||
            strcasecmp(prop, "ProgressBarColor") == 0 ||
            strcasecmp(prop, "CadrageHorizontal") == 0 ||
            strcasecmp(prop, "HorizontalAlignment") == 0) {
            obj_ui_control_instance_headless_set(
                uici, prop, INTEGER_VAL((long)AS_REAL(neuf)));
        }
        return;
    }

    /*  — Champ Interrupteur ..Valeur (bitmask de champ) en HEADLESS :
     * propage vers les casiers de verite par option (__int_coche), pour que la
     * lecture nue INT_X[i] et la forme ..Valeur refletent la selection posee au
     * niveau du champ. En GUI le widget est deja la source (set_bitmask via
     * apply_instance_scalar), et get_option_state fait foi : rien a propager. */
    if (kind == OWL_CONTROL_INTERRUPTEUR && uici->widget == NULL
        && (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0)
        && IS_INTEGER(neuf)) {
        int64_t mask = AS_INTEGER(neuf);
        ObjArray* items = widget_items_store_peek_uici(uici);
        if (items != NULL) {
            for (int i = 0; i < items->elements.count; i++) {
                if (!IS_INSTANCE(items->elements.values[i])) continue;
                ObjInstance* opt = AS_INSTANCE(items->elements.values[i]);
                int bit = ((mask >> i) & 1) ? 1 : 0;
                inst_field_set_int(opt, "__int_coche", bit);
            }
        }
        return;
    }

    /* 1. ONGLET..Valeur : changement reel du volet actif -> event. */
    if (kind == OWL_CONTROL_ONGLET
        && (strcasecmp(prop, "Valeur") == 0 || strcasecmp(prop, "Value") == 0)) {
        if (IS_INTEGER(old) && IS_INTEGER(neuf)
            && AS_INTEGER(old) != AS_INTEGER(neuf)) {
            onglet_fire_change_event(uici);
        }
        return;
    }

    /*  — `SEL_X..Options = [...]` / `INT_X..Options = [...]` au runtime :
     * le store d'items suit la nouvelle liste, comme il suit la liste declaree
     * en PROPRIETES. Sans ca, ..Occurrence et SEL_X[i] resteraient sur l'ancienne
     * liste en headless — l'indexation dependrait a nouveau de la provenance de
     * l'element. Inconditionnel headless ET GUI : cote GUI le widget a deja ete
     * reconstruit par le setter Qt, le store le rattrape. */
    if ((kind == OWL_CONTROL_SELECTEUR || kind == OWL_CONTROL_INTERRUPTEUR)
        && strcasecmp(prop, "Options") == 0) {
        /*  (volet B) — `SEL_X..Options = <valeur>` avec une valeur qui
         * n'est pas un tableau (typiquement une chaine multi-ligne issue d'un `[`
         * en fin de ligne) : le compilateur ne peut pas l'attraper (affectation
         * `..` dynamique). Sans ce garde, seed_options_headless / le setter Qt
         * l'avalaient en silence, options inchangees sans le moindre signal.
         * Funnel unique headless ET GUI (vm.c owl_ui_control_logic_after_set) :
         * soft-error (pose ErreurDetectee, pas de hard-stop), options existantes
         * preservees. Aligne sur le canon "types incompatibles". */
        if (!IS_ARRAY(neuf)) {
            owl_emit_native_error(E_RT_UI_OPTIONS_NOT_ARRAY,
                                  uici->decl->name,
                                  "control", uici->decl->name,
                                  "prop", prop, NULL);
            return;
        }
        seed_options_headless(uici, neuf, kind == OWL_CONTROL_INTERRUPTEUR);
        return;
    }

    /* 2. VOLET..Visible = Faux sur le volet actif -> report du volet actif. */
    if (kind == OWL_CONTROL_VOLET && strcasecmp(prop, "Visible") == 0) {
        bool nowVisible = IS_BOOL(neuf) ? AS_BOOL(neuf) : true;
        if (nowVisible) return;                 /* on ne reporte qu'au masquage */
        ObjWindowInstance* win = uici->owner;
        const char* ongName = uici->decl->parent_name;
        if (!win || !ongName) return;

        /* Indice 1-based de ce volet + resolution du Champ Onglet parent. */
        ObjUiControlInstance* ong = NULL;
        int voletCount = 0, thisIndex = 0;
        for (int i = 0; i < win->control_count; i++) {
            ObjUiControlInstance* c = win->controls[i];
            if (!c || !c->decl) continue;
            if ((OwlControlKind)c->decl->kind == OWL_CONTROL_ONGLET
                && c->decl->name
                && strcasecmp(c->decl->name, ongName) == 0) {
                ong = c;
            } else if ((OwlControlKind)c->decl->kind == OWL_CONTROL_VOLET
                       && c->decl->parent_name
                       && strcasecmp(c->decl->parent_name, ongName) == 0) {
                voletCount++;
                if (c == uici) thisIndex = voletCount;
            }
        }
        if (!ong || thisIndex == 0) return;

        long active = 1;
        Value av;
        if (obj_ui_control_instance_headless_get(ong, "Valeur", &av)
            && IS_INTEGER(av)) active = AS_INTEGER(av);
        if (active != thisIndex) return;        /* le volet masque n'etait pas actif */

        /* Volet visible le plus proche : suivant, sinon precedent. */
        int target = 0;
        for (int idx = thisIndex + 1; idx <= voletCount && target == 0; idx++) {
            ObjUiControlInstance* v = onglet_volet_at(win, ongName, idx);
            if (v && onglet_uici_is_visible(v)) target = idx;
        }
        for (int idx = thisIndex - 1; idx >= 1 && target == 0; idx--) {
            ObjUiControlInstance* v = onglet_volet_at(win, ongName, idx);
            if (v && onglet_uici_is_visible(v)) target = idx;
        }
        if (target == 0) return;                /* plus aucun volet visible */

        obj_ui_control_instance_headless_set(ong, "Valeur", INTEGER_VAL(target));
        if (ong->widget != NULL)
            owl_ui_apply_instance_scalar(ong, "Valeur", INTEGER_VAL(target));
        onglet_fire_change_event(ong);          /* report = changement reel */
        return;
    }
}

/*  lot 2 — impl du hook register_composite. Quand un Cadre/Police est
 * materialise dans le store d'une instance montee (widget != NULL), on relie
 * l'arbre composite a son OwlControl* pour que owl_ui_on_instance_modified
 * retrouve le widget au prochain chained set (..Cadre..Epaisseur = N). */
static void register_composite_impl(ObjUiControlInstance* uici,
                                    ObjInstance* composite) {
    if (!uici || !uici->widget || !composite) return;
    inst_widget_watch_tag_recursive(composite, (OwlControl*)uici->widget);
    /*  — si le composite materialise est le Style typé du champ
     * (styleSaisie), on l'enregistre comme racine de style du widget : une
     * mutation d'overlay d'etat post-ouverture rejouera la feuille complete. */
    if (composite->structName != NULL &&
        strcasecmp(composite->structName->chars, "styleSaisie") == 0) {
        style_root_record((OwlControl*)uici->widget, composite);
    }
}

/*  — hot-switch langue ( §4). Callback owl_core -> GUI.
 *
 * Au changement de langue courante (Langue(<lang>)), on re-resout et re-pousse
 * chaque propriete typee chaineMultilingue des controles/fenetres VIVANTS vers
 * le backend graphique. Le store d'instance (headless_store) est la source de
 * verite : il RETIENT l'ObjMultilingue, donc une entree IS_MULTILINGUE
 * = exactement une prop chaineMultilingue a rafraichir. Les slots chaine simple
 * sont deja aplatis a la langue active au stockage (jamais multilingues) : on ne
 * les touche pas.
 *
 * Reutilise apply_instance_scalar_impl / apply_window_scalar_impl : real_ui_set_prop
 * (resp. window_set_prop_qt) resout l'ObjMultilingue via value_to_string (langue
 * courante) et ecrit la chaine au QWidget. No-op si widget == NULL (CLI/headless).
 * Pas de mutation du store pendant l'iteration (proxy headless_store=NULL). */
static void i18n_rerender_uici_multilingue(ObjUiControlInstance* uici) {
    if (!uici || !uici->widget || !uici->headless_store) return;
    Table* st = uici->headless_store;
    for (int i = 0; i < st->capacity; i++) {
        Entry* e = &st->entries[i];
        if (e->key == NULL) continue;
        if (!IS_MULTILINGUE(e->value)) continue;
        apply_instance_scalar_impl(uici, e->key->chars, e->value);
    }
}

static void i18n_rerender_window_multilingue(ObjWindowInstance* wi) {
    if (!wi || !wi->widget || !wi->headless_store) return;
    Table* st = wi->headless_store;
    for (int i = 0; i < st->capacity; i++) {
        Entry* e = &st->entries[i];
        if (e->key == NULL) continue;
        if (!IS_MULTILINGUE(e->value)) continue;
        apply_window_scalar_impl(wi, e->key->chars, e->value);
    }
}

static void i18n_language_changed_impl(void) {
    for (int i = 0; i < vm.window_stack_count; i++) {
        ObjWindowInstance* wi = vm.window_stack[i];
        if (!wi) continue;
        i18n_rerender_window_multilingue(wi);
        for (int j = 0; j < wi->control_count; j++) {
            i18n_rerender_uici_multilingue(wi->controls[j]);
        }
    }
}

/*  — SeedPhase + seed_uici_headless_phase sont definis plus bas mais
 * build_window doit les appeler (seed store des props dynamiques des controles
 * apres le top-level fenetre). Remontee ici : enum complet + forward decl. */
typedef enum {
    SEED_PHASE_LITERAL,
    SEED_PHASE_DYNAMIC,
} SeedPhase;
static void seed_uici_headless_phase(ObjUiControlInstance* uici,
                                     OwlControlDecl* c, SeedPhase phase);

/* ====================================================================
 *   — Menu classique de fenetre : IR -> barre de menu Qt
 *
 *  Chemin UNIQUE partage par la construction a l'ouverture et par la
 *  bascule runtime (`FEN_X..MenuPrincipal = MNU_Autre`) : la barre est
 *  toujours reconstruite de zero depuis l'arbre declare, donc les
 *  surSelection sont re-cablees a chaque fois et aucune action de
 *  l'ancienne barre ne survit (Qt detruit l'arbre precedent).
 * ==================================================================== */

/* Taille de rasterisation de l'icone d'une option (Qt sous-echantillonne
 * ensuite vers la hauteur de ligne du menu). */
#define OWL_MENU_ICONE_SIZE 64

/* Nombre de raccourcis suivis pour la detection de collision dans une meme
 * barre. Au-dela, on cesse de comparer : le menu se construit normalement. */
#define OWL_MENU_SHORTCUT_TRACK_MAX 64

typedef struct {
    OwlWindow*  window;
    const char* window_name;   /* pour le message de collision */
    char        shortcuts[OWL_MENU_SHORTCUT_TRACK_MAX][40];
    int         shortcut_count;
    /*  — pour un menu CONTEXTUEL : nom du widget declencheur (le champ
     * sur lequel on a fait clic droit, ou la fenetre pour un menu attache a la
     * fenetre). Cable comme sender de la surSelection -> MoiMeme = ce widget,
     * pas l'option (une option de menu n'est pas un global control-ref, donc
     * MoiMeme = nom d'option resoudrait NIL -> "acces champ sur Null"). NULL
     * pour une barre de menu principale (comportement inchange : sender = nom
     * d'option, historique). */
    const char* ctx_sender_name;
    /*  — menu de la LOUPE d'un Champ Table : nom canon de la colonne
     * cliquee. Quand non NULL, ctx_sender_name porte le nom du Champ Table et le
     * sender pousse est le proxy-colonne (MoiMeme = la colonne). NULL = menu
     * contextuel/barre classique (sender = widget/option). */
    const char* ctx_sender_col;
} MenuBuildCtx;

/* Rasterise un ObjIcone (SVG du catalogue ou construit) vers l'icone d'une
 * option de menu. No-op si l'icone est invalide. */
static void menu_item_set_icone(OwlMenuItem* item, ObjIcone* icn) {
    if (!icn->valide || icn->svg_raw == NULL || icn->svg_raw->length <= 0) return;
    unsigned char* rgba = NULL;
    int iw = 0, ih = 0;
    bool want_bg = (icn->couleur_fond != -1);
    if (owl_icone_rasterize_rgba(
            icn->svg_raw->chars, icn->svg_raw->length,
            (icn->couleur == -1) ? 0u : (unsigned int)(icn->couleur & 0xFFFFFF),
            icn->couleur_modifiable,
            OWL_MENU_ICONE_SIZE, icn->rotation, icn->forme,
            (unsigned int)((want_bg ? icn->couleur_fond : 0) & 0xFFFFFF),
            want_bg, &rgba, &iw, &ih)) {
        owl_ui_menu_item_set_icon_rgba(item, rgba, iw, ih);
        free(rgba);
    }
}

/* Icone d'une option : meme valeur polymorphe que la prop Image d'un Bouton
 * (Image | Icone | chaine). Une chaine est soit une reference du catalogue
 * integre ("icones.<nom>"/"icons.<nom>"), soit un chemin de fichier disque.
 * Contrat defensif (jamais silencieux) : catalogue inconnu ou fichier absent
 * -> diagnostic sur stderr + pas d'icone (aucune erreur d'execution levee).
 * `option_name`/`window_name` cadrent le diagnostic. */
static void menu_item_set_icon_value(OwlMenuItem* item, Value v,
                                     const char* option_name,
                                     const char* window_name) {
    if (!item || IS_NIL(v)) return;
    if (stdlib_image_is_instance(v)) {
        int iw = 0, ih = 0;
        const unsigned char* rgba = stdlib_image_get_rgba(v, &iw, &ih);
        owl_ui_menu_item_set_icon_rgba(item, rgba, iw, ih);
        return;
    }
    if (IS_ICONE(v)) {
        menu_item_set_icone(item, AS_ICONE(v));
        return;
    }
    ObjString* s = value_to_string(v);
    if (!s || s->length <= 0) return;

    /* Reference du catalogue d'icones integre, comme sur un Bouton. */
    const char* nom = NULL;
    if (strncasecmp(s->chars, "icones.", 7) == 0)      nom = s->chars + 7;
    else if (strncasecmp(s->chars, "icons.", 6) == 0)  nom = s->chars + 6;
    if (nom != NULL) {
        const OwlIconCatalogEntry* entry = owl_icones_catalog_find(nom);
        if (entry == NULL) {
            fprintf(stderr,
                    "[ui] Menu de '%s' : l'option '%s' reference une icone de "
                    "catalogue inconnue '%s' (aucune icone affichee).\n",
                    window_name ? window_name : "?",
                    option_name ? option_name : "?", nom);
            return;
        }
        Value iv = owl_icones_materialize_entry(entry);
        if (IS_ICONE(iv)) {
            vm_push(iv);            /* protege l'Icone du GC pendant la raster. */
            menu_item_set_icone(item, AS_ICONE(iv));
            vm_pop();
        }
        return;
    }

    /* Chemin de fichier disque : signale l'absence (jamais silencieux) mais
     * delegue quand meme a Qt, arbitre final des formats/ressources. */
    struct stat st;
    if (stat(s->chars, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG) {
        fprintf(stderr,
                "[ui] Menu de '%s' : l'option '%s' reference un fichier image "
                "introuvable '%s' (aucune icone affichee).\n",
                window_name ? window_name : "?",
                option_name ? option_name : "?", s->chars);
    }
    owl_ui_menu_item_set_icon_path(item, s->chars);
}

/* Signale une collision d'accelerateur dans la meme barre : deux options qui
 * repondent au meme raccourci, Qt n'en declenche aucune de facon fiable. On
 * construit quand meme (l'utilisateur voit son menu) mais on ne se tait pas. */
static void menu_track_shortcut(MenuBuildCtx* ctx, const char* raccourci,
                                 const char* option_name) {
    if (!raccourci || !*raccourci) return;
    char norm[40];
    int n = scanner_strip_accents(raccourci, (int)strlen(raccourci),
                                   norm, sizeof(norm));
    if (n <= 0 || n >= (int)sizeof(norm)) return;
    norm[n] = '\0';
    for (int i = 0; i < ctx->shortcut_count; i++) {
        if (strcasecmp(ctx->shortcuts[i], norm) != 0) continue;
        fprintf(stderr,
                "[ui] Menu de '%s' : le raccourci '%s' de l'option '%s' est "
                "deja utilise par une autre option du meme menu.\n",
                ctx->window_name ? ctx->window_name : "?",
                raccourci, option_name ? option_name : "?");
        return;
    }
    if (ctx->shortcut_count < OWL_MENU_SHORTCUT_TRACK_MAX) {
        memcpy(ctx->shortcuts[ctx->shortcut_count], norm, (size_t)n + 1);
        ctx->shortcut_count++;
    }
}

static void menu_build_children(MenuBuildCtx* ctx, OwlMenu* parent,
                                 OwlMenuNode** children, int count);

static void menu_build_node(MenuBuildCtx* ctx, OwlMenu* parent, OwlMenuNode* n) {
    if (!n) return;
    /* Les props d'un noeud de menu suivent le meme cycle que celles d'un champ :
     * l'evaluateur d'une prop non litterale est re-joue a chaque construction
     * (ouverture ET bascule), d'ou l'invalidation du cache juste avant. */
    invalidate_dynamic_props_cache(n->properties, n->property_count);
    resolve_dynamic_props(n->properties, n->property_count);
    bool visible = prop_bool(n->properties, n->property_count,
                              "Visible", "Visible", true);

    if (n->kind == OWL_MENU_NODE_SEPARATOR) {
        owl_ui_menu_add_separator(ctx->window, parent, visible ? 1 : 0);
        return;
    }

    const char* libelle = prop_str(n->properties, n->property_count,
                                    "Libelle", "Caption", n->name);
    bool grise = prop_bool(n->properties, n->property_count,
                            "Grise", "Grayed", false);

    /* Etat unifie (enum partage avec les champs) : ecrase Grisé/Visible quand
     * specifie, comme sur un widget. Sur un noeud de menu (pas de lecture-seule
     * native), AffichageSeulement (« Inactif » cote inspecteur) rend l'item non
     * cliquable MAIS de couleur normale (distinct de Grisé) — meme intention que
     * owl_ui_control_set_etat pour un widget non editable. Invisible reste
     * pilotable mais le masquage passe usuellement par Visible. */
    int etat = prop_int(n->properties, n->property_count, "Etat", "State", -1);
    bool inactif = false;
    if (etat != -1) {
        switch (etat) {
            case OWL_ETAT_ACTIF:          grise = false; visible = true; break;
            case OWL_ETAT_AFFICHAGE_SEUL: inactif = true;                 break;
            case OWL_ETAT_GRISE:          grise = true;                   break;
            case OWL_ETAT_INVISIBLE:      visible = false;                break;
            default: break;
        }
    }

    if (n->kind == OWL_MENU_NODE_SUBMENU) {
        OwlMenu* sub = owl_ui_menu_add_submenu(ctx->window, parent, libelle,
                                                grise ? 0 : 1, visible ? 1 : 0,
                                                inactif ? 1 : 0);
        if (sub) menu_build_children(ctx, sub, n->children, n->child_count);
        return;
    }

    /* OptionMenu : item terminal. */
    const char* raccourci = prop_str(n->properties, n->property_count,
                                      "Raccourci", "Shortcut", NULL);
    menu_track_shortcut(ctx, raccourci, n->name);
    /* `Coché` absent = option sans case a cocher (item d'action simple). */
    bool checkable = owl_menu_node_has_property(n, "Coche")
                     || owl_menu_node_has_property(n, "Checked");
    bool coche = prop_bool(n->properties, n->property_count,
                            "Coche", "Checked", false);

    int bidx = -1;
    const OwlEventBlock* sel = find_event(n->events, n->event_count,
                                           "surS\xc3\xa9lection", "onSelection");
    /* surSelection est compilee sans MonChamp (arity 1 : MaFenetre seule, une
     * option de menu n'est pas un champ). Le dispatch doit donc pousser 1 seul
     * argument synthetique -> enregistrer comme event de fenetre (is_window),
     * sinon call_event_block_with_sender pousse aussi MonChamp (2 args) et la
     * VM leve "1 argument attendu, 2 recus". MoiMeme reste resolu via le
     * sender_name pousse sur event_sender_stack, independant du flag :
     *   - barre principale : sender = nom de l'option (ctx_sender_name NULL) ;
     *   - menu contextuel   : sender = widget declencheur (ctx_sender_name),
     *     partage par toutes les options du meme menu. */
    const char* sender_name = (ctx->ctx_sender_name && ctx->ctx_sender_name[0])
                              ? ctx->ctx_sender_name : n->name;
    if (sel && sel->impl) {
        /*  — menu loupe : sender = proxy-colonne (MoiMeme = colonne). */
        if (ctx->ctx_sender_col && ctx->ctx_sender_col[0])
            bidx = register_binding_named_col(sel->impl, true, sender_name,
                                              ctx->ctx_sender_col);
        else
            bidx = register_binding_named(sel->impl, true, sender_name);
    }

    OwlMenuItem* item = owl_ui_menu_add_option(
        ctx->window, parent, libelle, raccourci,
        grise ? 0 : 1, checkable ? 1 : 0, coche ? 1 : 0, visible ? 1 : 0,
        inactif ? 1 : 0,
        (bidx >= 0) ? dispatch_event : NULL, (void*)(intptr_t)bidx);

    Value img = prop_value(n->properties, n->property_count, "Image", "Image");
    /* Coexistence Coché + Image : le rendu Qt dessine icône XOR coche dans la même
     * colonne (une option avec icône ne rend jamais la coche). On laisse donc
     * tomber l'icône quand l'option est cochée pour que la coche nette occupe sa
     * place ; décochée, l'icône s'affiche normalement. Le menu étant reconstruit à
     * chaque bascule, l'échange suit l'état courant de `Coché`. */
    bool coche_actif = checkable && coche;
    if (item && !IS_NIL(img) && !coche_actif)
        menu_item_set_icon_value(item, img, n->name, ctx->window_name);
}

static void menu_build_children(MenuBuildCtx* ctx, OwlMenu* parent,
                                 OwlMenuNode** children, int count) {
    for (int i = 0; i < count; i++) menu_build_node(ctx, parent, children[i]);
}

/*  — copie une brique de style plate (OwlMenuStyleBrick, ui_decl.h)
 * vers la surface résolue transmise à Qt (OwlMenuStyleResolved, owl_ui.h).
 * Les deux structs sont des miroirs plats ; découplées volontairement. */
static void menu_copy_style_brick(const OwlMenuStyleBrick* src,
                                  OwlMenuStyleSurface* dst) {
    dst->couleur         = src->couleur;
    dst->couleur_fond    = src->couleur_fond;
    dst->police_nom      = src->police_nom;   /* borrow : vit dans le decl */
    dst->police_taille   = src->police_taille;
    dst->police_gras     = src->police_gras;
    dst->police_italique = src->police_italique;
    dst->police_souligne = src->police_souligne;
    dst->police_barre    = src->police_barre;
}

/*  — sentinelle « non posé » sur une surface de menu résolue. memset(0)
 * ne suffit pas : 0 vaut « noir / normal », pas « non posé ». */
static void menu_surface_init_unset(OwlMenuStyleSurface* s) {
    s->couleur         = -1;
    s->couleur_fond    = -1;
    s->police_nom      = NULL;
    s->police_taille   = -1;
    s->police_gras     = -1;
    s->police_italique = -1;
    s->police_souligne = -1;
    s->police_barre    = -1;
}

/*  — défini avec la famille de remplissage depuis instance, plus bas. */
static void menu_style_fill_from_instance(ObjInstance* style,
                                          OwlMenuStyleResolved* out);

/* Résout le style typé d'un menu (slots plats du decl) en struct pour Qt.
 *  : les sentinelles sont posées MEME sans style plat, pour que le
 * remplissage depuis l'instance (`Style = theme.<nom>`) parte d'une base « rien
 * posé » et non de zéros interprétés comme des valeurs. Les deux sources se
 * composent : l'instance est la BASE, les slots plats la SURCHARGE (canon
 * , l'ordre source est tranché au parse — une affectation de Style
 * entière reset les cascades antérieures). */
static void menu_resolve_style(const OwlMenuDecl* menu, OwlMenuStyleResolved* out) {
    memset(out, 0, sizeof(*out));
    menu_surface_init_unset(&out->barre_base);
    menu_surface_init_unset(&out->barre_survol);
    menu_surface_init_unset(&out->option_base);
    menu_surface_init_unset(&out->option_survol);
    out->sousmenu_couleur_fond       = -1;
    out->sousmenu_couleur_separateur = -1;
    out->sousmenu_arrondi            = -1;
    out->sousmenu_epaisseur          = -1;
    out->sousmenu_couleur_bordure    = -1;
    if (!menu) { out->has_style = 0; return; }
    if (menu->has_style) {
        out->has_style = 1;
        menu_copy_style_brick(&menu->barre_base,   &out->barre_base);
        menu_copy_style_brick(&menu->barre_survol, &out->barre_survol);
        menu_copy_style_brick(&menu->option_base,  &out->option_base);
        menu_copy_style_brick(&menu->option_survol,&out->option_survol);
        out->sousmenu_couleur_fond       = menu->sousmenu_couleur_fond;
        out->sousmenu_couleur_separateur = menu->sousmenu_couleur_separateur;
        out->sousmenu_arrondi            = menu->sousmenu_arrondi;
        out->sousmenu_epaisseur          = menu->sousmenu_epaisseur;
        out->sousmenu_couleur_bordure    = menu->sousmenu_couleur_bordure;
    }
    if (menu->has_style_ref && menu->style_ref.is_constant
        && IS_INSTANCE(menu->style_ref.const_value)) {
        menu_style_fill_from_instance(AS_INSTANCE(menu->style_ref.const_value), out);
    }
}

/*  — copie une brique de style plate de volet (OwlOngletStyleBrick,
 * ui_decl.h) vers la surface résolue transmise à Qt (OwlOngletStyleSurface,
 * owl_ui.h). Miroirs plats, découplés. */
static void onglet_copy_style_brick(const OwlOngletStyleBrick* src,
                                    OwlOngletStyleSurface* dst) {
    dst->couleur         = src->couleur;
    dst->couleur_fond    = src->couleur_fond;
    dst->police_nom      = src->police_nom;   /* borrow : vit dans le decl */
    dst->police_taille   = src->police_taille;
    dst->police_gras     = src->police_gras;
    dst->police_italique = src->police_italique;
    dst->police_souligne = src->police_souligne;
    dst->police_barre    = src->police_barre;
    dst->arrondi         = src->arrondi;   /*  : arrondi de la languette */
}

/*  — sentinelle « non posé » sur une surface résolue. memset(0) ne
 * suffit pas : 0 vaut « noir / normal / 0px », pas « non posé ». */
static void onglet_surface_init_unset(OwlOngletStyleSurface* s) {
    s->couleur         = -1;
    s->couleur_fond    = -1;
    s->police_nom      = NULL;
    s->police_taille   = -1;
    s->police_gras     = -1;
    s->police_italique = -1;
    s->police_souligne = -1;
    s->police_barre    = -1;
    s->arrondi         = -1;
}

/* Résout le style typé d'un Champ Onglet (slots plats du decl) en struct Qt.
 *  : les sentinelles sont posées MEME sans style plat, pour que le
 * remplissage depuis l'instance (onglet_style_fill_from_instance) parte d'une
 * base « rien posé » et non de zéros interprétés comme des valeurs. */
static void onglet_resolve_style(const OwlControlDecl* c, OwlOngletStyleResolved* out) {
    memset(out, 0, sizeof(*out));
    out->cadre_ext.couleur_fond    = -1;
    out->cadre_ext.cadre_couleur   = -1;
    out->cadre_ext.cadre_epaisseur = -1;
    out->cadre_ext.cadre_arrondi   = -1;
    onglet_surface_init_unset(&out->actif_base);
    onglet_surface_init_unset(&out->actif_survol);
    onglet_surface_init_unset(&out->inactif_base);
    onglet_surface_init_unset(&out->inactif_survol);
    if (!c || !c->ong_has_style) { out->has_style = 0; return; }
    out->has_style                = 1;
    out->cadre_ext.couleur_fond    = c->ong_cadre_ext.couleur_fond;
    out->cadre_ext.cadre_couleur   = c->ong_cadre_ext.cadre_couleur;
    out->cadre_ext.cadre_epaisseur = c->ong_cadre_ext.cadre_epaisseur;
    out->cadre_ext.cadre_arrondi   = c->ong_cadre_ext.cadre_arrondi;
    onglet_copy_style_brick(&c->ong_actif_base,     &out->actif_base);
    onglet_copy_style_brick(&c->ong_actif_survol,   &out->actif_survol);
    onglet_copy_style_brick(&c->ong_inactif_base,   &out->inactif_base);
    onglet_copy_style_brick(&c->ong_inactif_survol, &out->inactif_survol);
}

/*  — lit une couleur d'instance et la range dans un slot résolu SI ce
 * slot est encore vierge. L'encodage cible est celui du style menu (BGR + bit
 * alpha), identique à ce que pose le parseur des slots plats. Un slot déjà posé
 * vient d'une cascade `Style..X = v` : elle SURCHARGE la valeur du registre
 * (canon , dernière écriture gagne). Partagé Onglet + menus. */
static void style_inst_color_into(ObjInstance* src, const char* field,
                                   int64_t* slot) {
    if (!src || *slot >= 0) return;
    int bgr = -1, alpha = 255;
    instance_get_color(src, field, &bgr, &alpha);
    if (bgr < 0) return;
    *slot = OWL_MENU_COL_PACK((int64_t)bgr, alpha);
}

/*  — remplit une surface (languette) depuis une instance styleVolet /
 * styleVoletBase du registre. `with_police` : seule la surface de BASE porte une
 * Police (l'état survol ne pilote que les couleurs, cf styleVoletBase) ; idem
 * pour Arrondi. Les chaînes de police sont empruntées à l'ObjString de
 * l'instance : elle vit dans le store de l'ObjUiControlInstance, donc au-delà de
 * l'appel owl_ui_onglet_apply_style. */
static void onglet_surface_from_instance(ObjInstance* src,
                                         OwlOngletStyleSurface* dst,
                                         bool with_police) {
    if (!src) return;
    style_inst_color_into(src, "Couleur",     &dst->couleur);
    style_inst_color_into(src, "CouleurFond", &dst->couleur_fond);
    if (!with_police) return;
    if (dst->arrondi < 0) {
        int arr = instance_get_int(src, "Arrondi", -1);
        if (arr >= 0) dst->arrondi = arr;
    }
    ObjInstance* police = instance_get_instance(src, "Police");
    if (!police || police_instance_is_default(police)) return;
    if (dst->police_nom == NULL) {
        Value      nomV;
        ObjString* nomK = obj_copy_string("Nom", 3);
        if (table_get(&police->fields, nomK, &nomV)) {
            if (IS_STRING(nomV) && AS_STRING(nomV)->length > 0)
                dst->police_nom = AS_CSTRING(nomV);
            else if (IS_INTEGER(nomV) && AS_INTEGER(nomV) != 0)
                dst->police_nom = owl_ui_ipol_embedded_family(AS_INTEGER(nomV));
        }
    }
    if (dst->police_taille < 0) {
        int taille = instance_get_int(police, "Taille", 0);
        if (taille > 0) dst->police_taille = taille;
    }
    /* Les booleens de police n'ont pas de sentinelle « non posé » cote instance
     * (le template les seede a Faux) : des lors que la Police n'est PAS au
     * defaut integral, on transmet les quatre etats explicitement — meme
     * convention que read_text_brick pour les briques texte des autres champs. */
    if (dst->police_gras < 0)
        dst->police_gras = instance_get_bool(police, "Gras", false) ? 1 : 0;
    if (dst->police_italique < 0)
        dst->police_italique = instance_get_bool(police, "Italique", false) ? 1 : 0;
    if (dst->police_souligne < 0)
        dst->police_souligne = instance_get_bool(police, "Souligne", false) ? 1 : 0;
    if (dst->police_barre < 0)
        dst->police_barre = instance_get_bool(police, "Barre", false) ? 1 : 0;
    /* La Police porte aussi une Couleur (canon du type Police) : elle alimente
     * la couleur de texte de la languette si celle-ci n'est pas deja posee. */
    style_inst_color_into(police, "Couleur", &dst->couleur);
}

/*  — remplit le style résolu d'un Champ Onglet depuis une instance
 * styleOnglet du registre (`Style = theme.<nom>`). Ne touche QUE les slots
 * encore vierges : l'instance est la BASE, les slots plats déjà posés par les
 * cascades `Style..` sont la SURCHARGE. */
static void onglet_style_fill_from_instance(ObjInstance* style,
                                            OwlOngletStyleResolved* out) {
    if (!style) return;
    out->has_style = 1;
    ObjInstance* ce = instance_get_instance(style, "CadreExterieur");
    if (ce) {
        style_inst_color_into(ce, "CouleurFond", &out->cadre_ext.couleur_fond);
        ObjInstance* cadre = instance_get_instance(ce, "Cadre");
        if (cadre) {
            /* Epaisseur 0 = cadre invisible (defaut du type CadreOnglet) : on le
             * traite comme « non posé » plutot que d'emettre `border: 0px` qui
             * ecraserait la bordure du theme. Meme convention que
             * cadre_instance_is_visible pour les autres champs. */
            int ep = instance_get_int(cadre, "Epaisseur", 0);
            if (ep > 0) {
                if (out->cadre_ext.cadre_epaisseur < 0) out->cadre_ext.cadre_epaisseur = ep;
                style_inst_color_into(cadre, "Couleur", &out->cadre_ext.cadre_couleur);
            }
            int arr = instance_get_int(cadre, "Arrondi", 0);
            if (arr > 0 && out->cadre_ext.cadre_arrondi < 0)
                out->cadre_ext.cadre_arrondi = arr;
        }
    }
    ObjInstance* actif   = instance_get_instance(style, "VoletActif");
    ObjInstance* inactif = instance_get_instance(style, "VoletInactif");
    onglet_surface_from_instance(actif,   &out->actif_base,   true);
    onglet_surface_from_instance(inactif, &out->inactif_base, true);
    if (actif)
        onglet_surface_from_instance(
            instance_get_instance(instance_get_instance(actif, "Etats"), "_Survol"),
            &out->actif_survol, false);
    if (inactif)
        onglet_surface_from_instance(
            instance_get_instance(instance_get_instance(inactif, "Etats"), "_Survol"),
            &out->inactif_survol, false);
}

/*  — remplit une surface texte de menu (Barre / OptionMenu) depuis une
 * instance styleMenuPrincipal*Base du registre. Miroir strict de
 * onglet_surface_from_instance, moins l'Arrondi (une surface de menu n'en a
 * pas). `with_police` : seule la surface de BASE porte une Police (l'état survol
 * ne pilote que les couleurs, cf install_menu_text_brick). Les chaînes de police
 * sont empruntées à l'ObjString de l'instance : celle-ci vit dans le decl (le
 * style_ref résolu), donc au-delà de l'appel owl_ui_window_menubar_reset. */
static void menu_surface_from_instance(ObjInstance* src,
                                       OwlMenuStyleSurface* dst,
                                       bool with_police) {
    if (!src) return;
    style_inst_color_into(src, "Couleur",     &dst->couleur);
    style_inst_color_into(src, "CouleurFond", &dst->couleur_fond);
    if (!with_police) return;
    ObjInstance* police = instance_get_instance(src, "Police");
    if (!police || police_instance_is_default(police)) return;
    if (dst->police_nom == NULL) {
        Value      nomV;
        ObjString* nomK = obj_copy_string("Nom", 3);
        if (table_get(&police->fields, nomK, &nomV)) {
            if (IS_STRING(nomV) && AS_STRING(nomV)->length > 0)
                dst->police_nom = AS_CSTRING(nomV);
            else if (IS_INTEGER(nomV) && AS_INTEGER(nomV) != 0)
                dst->police_nom = owl_ui_ipol_embedded_family(AS_INTEGER(nomV));
        }
    }
    if (dst->police_taille < 0) {
        int taille = instance_get_int(police, "Taille", 0);
        if (taille > 0) dst->police_taille = taille;
    }
    /* Les booleens de police n'ont pas de sentinelle « non posé » cote instance
     * (le template les seede a Faux) : des lors que la Police n'est PAS au
     * defaut integral, on transmet les quatre etats explicitement. */
    if (dst->police_gras < 0)
        dst->police_gras = instance_get_bool(police, "Gras", false) ? 1 : 0;
    if (dst->police_italique < 0)
        dst->police_italique = instance_get_bool(police, "Italique", false) ? 1 : 0;
    if (dst->police_souligne < 0)
        dst->police_souligne = instance_get_bool(police, "Souligne", false) ? 1 : 0;
    if (dst->police_barre < 0)
        dst->police_barre = instance_get_bool(police, "Barre", false) ? 1 : 0;
    /* La Police porte aussi une Couleur (canon du type Police) : elle alimente
     * la couleur de texte de la surface si celle-ci n'est pas deja posee. */
    style_inst_color_into(police, "Couleur", &dst->couleur);
}

/*  — remplit le style résolu d'un menu depuis une instance
 * styleMenuPrincipal / styleMenuContextuel du registre (`Style = theme.<nom>`).
 * Ne touche QUE les slots encore vierges : l'instance est la BASE, les slots
 * plats déjà posés par les cascades `Style..` sont la SURCHARGE. Un
 * styleMenuContextuel n'a pas de surface Barre : le lookup rend NULL et les
 * slots barre restent au défaut du thème, sans cas particulier. */
static void menu_style_fill_from_instance(ObjInstance* style,
                                          OwlMenuStyleResolved* out) {
    if (!style) return;
    out->has_style = 1;
    ObjInstance* barre  = instance_get_instance(style, "Barre");
    ObjInstance* option = instance_get_instance(style, "OptionMenu");
    menu_surface_from_instance(barre,  &out->barre_base,  true);
    menu_surface_from_instance(option, &out->option_base, true);
    if (barre)
        menu_surface_from_instance(
            instance_get_instance(instance_get_instance(barre, "Etats"), "_Survol"),
            &out->barre_survol, false);
    if (option)
        menu_surface_from_instance(
            instance_get_instance(instance_get_instance(option, "Etats"), "_Survol"),
            &out->option_survol, false);
    ObjInstance* sm = instance_get_instance(style, "SousMenu");
    if (sm) {
        style_inst_color_into(sm, "CouleurFond",       &out->sousmenu_couleur_fond);
        style_inst_color_into(sm, "CouleurSeparateur", &out->sousmenu_couleur_separateur);
        style_inst_color_into(sm, "CouleurBordure",    &out->sousmenu_couleur_bordure);
        /* Arrondi/Epaisseur n'ont pas de sentinelle cote instance (le template
         * les seede a 0 et 1) — ce sont exactement les valeurs de repli du rendu
         * pour un slot non posé, donc la transmission est neutre. */
        if (out->sousmenu_arrondi < 0)
            out->sousmenu_arrondi = instance_get_int(sm, "Arrondi", 0);
        if (out->sousmenu_epaisseur < 0)
            out->sousmenu_epaisseur = instance_get_int(sm, "Epaisseur", 1);
    }
}

/*  — résout les `Style = <reference>` de tous les arbres de menu d'une
 * fenêtre. Fait UNE fois au montage (avant la barre et avant l'installation des
 * menus contextuels sur les champs), pas dans menu_resolve_style : celui-ci est
 * rappelé à chaque clic droit, depuis un callback Qt. Invalidation préalable =
 * une réouverture ré-évalue (parité avec les props dynamiques, ). */
static void resolve_menu_style_refs(const OwlWindowDecl* decl) {
    if (!decl) return;
    for (int i = 0; i < decl->menu_count; i++) {
        OwlMenuDecl* m = decl->menus[i];
        if (!m || !m->has_style_ref) continue;
        invalidate_dynamic_props_cache(&m->style_ref, 1);
        resolve_dynamic_props(&m->style_ref, 1);
    }
}

/* Point d'entree unique : (re)construit la barre de menu de `w` a partir de
 * l'arbre nomme `menu_name` declare dans `decl`.
 *   - menu_name NULL / vide : aucune barre (la precedente est retiree).
 *   - menu_name introuvable : barre retiree + diagnostic (jamais silencieux).
 * Retourne true si une barre a effectivement ete posee. */
static bool build_window_main_menu(OwlWindow* w, const OwlWindowDecl* decl,
                                    const char* menu_name) {
    if (!w || !decl) return false;
    if (menu_name == NULL || *menu_name == '\0') {
        owl_ui_window_menubar_clear(w);
        return false;
    }
    const OwlMenuDecl* menu = owl_window_decl_find_menu(decl, menu_name);
    if (menu == NULL) {
        fprintf(stderr,
                "[ui] Fenetre '%s' : menu principal '%s' introuvable "
                "(aucun bloc MenuPrincipal de ce nom).\n",
                decl->name ? decl->name : "?", menu_name);
        owl_ui_window_menubar_clear(w);
        return false;
    }
    MenuBuildCtx ctx;
    ctx.window          = w;
    ctx.window_name     = decl->name;
    ctx.shortcut_count  = 0;
    ctx.ctx_sender_name = NULL;   /* barre principale : sender = nom d'option */
    ctx.ctx_sender_col  = NULL;   /* pas un menu loupe */
    /*  — style typé du menu : slots plats du OwlMenuDecl -> struct
     * résolu transmis à Qt. Chaque prop non posée (-1 / NULL) retombe sur le
     * défaut du thème côté rendu. */
    OwlMenuStyleResolved style;
    menu_resolve_style(menu, &style);
    owl_ui_window_menubar_reset(w, &style);
    menu_build_children(&ctx, NULL, menu->children, menu->child_count);
    return true;
}

/* ====================================================================
 *  Menu contextuel — clic droit sur un champ ou sur le fond
 *  de la fenetre. Contrairement a la barre (construite une fois a
 *  l'ouverture), l'arbre est (re)construit a chaque clic droit et affiche
 *  a la position du curseur, puis detruit.
 * ==================================================================== */

/* Liaison capturee par le callback de clic droit : la fenetre Qt + son decl +
 * le nom de l'arbre contextuel a afficher. Allouee a l'ouverture, elle vit tant
 * que la fenetre existe (jamais liberee, meme regime que les handlers Qt). */
typedef struct {
    OwlWindow*           window;
    const OwlWindowDecl* decl;
    char                 menu_name[128];
    /*  — nom du widget declencheur : le champ auquel ce menu contextuel
     * est attache, ou le nom de la fenetre pour un menu attache au fond. Cable
     * comme sender des surSelection -> MoiMeme = ce widget. Vide = non resolu. */
    char                 sender_name[128];
} ContextMenuBinding;

static void dispatch_context_menu(OwlControl* sender, void* userdata) {
    (void)sender;
    ContextMenuBinding* b = (ContextMenuBinding*)userdata;
    if (!b || !b->decl) return;
    const OwlMenuDecl* menu = owl_window_decl_find_menu(b->decl, b->menu_name);
    if (!menu || !menu->is_context) return;
    OwlMenuStyleResolved style;
    menu_resolve_style(menu, &style);
    OwlMenu* root = owl_ui_context_menu_create(b->window, &style);
    if (!root) return;
    MenuBuildCtx ctx;
    ctx.window          = b->window;
    ctx.window_name     = b->decl->name;
    ctx.shortcut_count  = 0;
    ctx.ctx_sender_name = (b->sender_name[0] != '\0') ? b->sender_name : NULL;
    ctx.ctx_sender_col  = NULL;   /* menu contextuel du corps : sender = champ */
    /* parent = racine autonome : owl_ui_menu_add_* ne touche pas la barre. */
    menu_build_children(&ctx, root, menu->children, menu->child_count);
    owl_ui_context_menu_popup(root);
}

/*  — nom canon (strippe) de la colonne d'index col0 (defini plus bas). */
static bool table_column_name_at(ObjUiControlInstance* uici, int col0,
                                 char* out, size_t cap);

/*  — pont Qt du menu de la LOUPE (clic droit sur l'en-tete de colonne
 * d'un Champ Table). Si la Table porte un `MenuLoupe = MNU_X` attache, on rend
 * MNU_X a la place du menu loupe integre : meme moteur que dispatch_context_menu
 * (owl_ui_context_menu_create + menu_build_children + popup), mais le sender des
 * options est le PROXY-COLONNE de la colonne cliquee -> MoiMeme = la colonne.
 * Retourne 1 si le menu custom a ete rendu (l'appelant supprime l'integre), 0
 * sinon (pas d'attache / colonne inconnue -> l'integre est conserve). */
int owl_ui_table_show_magnifier_menu(OwlControl* tableWidget, int col0) {
    ObjWindowInstance* inst = vm_window_stack_top();
    if (!inst || !inst->decl) return 0;
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici || !uici->decl) return 0;
    const char* menuName = uici->decl->magnifier_menu_name;
    if (!menuName || !*menuName) return 0;
    const OwlWindowDecl* decl = inst->decl;
    const OwlMenuDecl* menu = owl_window_decl_find_menu(decl, menuName);
    if (!menu || !menu->is_context) return 0;
    char colCanon[256];
    if (!table_column_name_at(uici, col0, colCanon, sizeof colCanon)) return 0;

    /* Nom du Champ Table sous forme strippee (comme les sender_name poses par le
     * compilo dans vm.globals) : dispatch_event reconstruit le proxy depuis lui. */
    char tblStrip[128];
    int tl = scanner_strip_accents(uici->decl->name,
                                   (int)strlen(uici->decl->name),
                                   tblStrip, sizeof tblStrip);
    if (tl <= 0 || tl >= (int)sizeof tblStrip) return 0;
    tblStrip[tl] = '\0';

    OwlMenuStyleResolved style;
    menu_resolve_style(menu, &style);
    OwlMenu* root = owl_ui_context_menu_create(g_current_window, &style);
    if (!root) return 0;
    MenuBuildCtx ctx;
    ctx.window          = g_current_window;
    ctx.window_name     = decl->name;
    ctx.shortcut_count  = 0;
    ctx.ctx_sender_name = tblStrip;    /* ref du Champ Table */
    ctx.ctx_sender_col  = colCanon;    /* colonne cliquee -> MoiMeme = colonne */
    menu_build_children(&ctx, root, menu->children, menu->child_count);
    owl_ui_context_menu_popup(root);
    return 1;
}

/*  — enregistre les raccourcis persistants des OptionMenu d'un arbre de
 * menu contextuel sur le widget cible (ou la fenetre si widget==NULL), afin
 * qu'ils firent MENU FERME quand le widget a le focus. Recursif : couvre aussi
 * les options des sous-menus. Le raccourci route sur le meme dispatch que le
 * clic (dispatch_event via une liaison surSelection), donc MoiMeme resout au
 * meme declencheur. Un Raccourci calcule (prop non litterale) n'est pas resolu
 * ici (litteral attendu) : le popup, lui, l'affiche normalement au clic. */
static void install_context_shortcut_node(OwlWindow* w, OwlControl* widget,
                                          const OwlMenuNode* n,
                                          const char* sender_name) {
    if (!n) return;
    if (n->kind == OWL_MENU_NODE_SUBMENU) {
        for (int i = 0; i < n->child_count; i++)
            install_context_shortcut_node(w, widget, n->children[i], sender_name);
        return;
    }
    if (n->kind != OWL_MENU_NODE_OPTION) return;
    const char* raccourci = prop_str(n->properties, n->property_count,
                                     "Raccourci", "Shortcut", NULL);
    if (!raccourci || !*raccourci) return;
    const OwlEventBlock* sel = find_event(n->events, n->event_count,
                                          "surS\xc3\xa9lection", "onSelection");
    if (!sel || !sel->impl) return;
    /* surSelection cablee comme event de FENETRE (arity 1) : voir menu_build_node. */
    int bidx = register_binding_named(sel->impl, true,
                                      sender_name ? sender_name : n->name);
    if (bidx < 0) return;
    owl_ui_install_context_shortcut(widget, w, raccourci,
                                    dispatch_event, (void*)(intptr_t)bidx);
}

/* Installe le menu contextuel `menu_name` sur le champ `widget`, ou, si
 * widget==NULL, sur le fond de la fenetre. Defensif : no-op si `menu_name` ne
 * designe pas un menu contextuel (deja garanti a la compilation). */
static void install_context_menu_on(OwlWindow* w, OwlControl* widget,
                                    const OwlWindowDecl* decl,
                                    const char* menu_name,
                                    const char* trigger_name) {
    if (!w || !decl || !menu_name || !*menu_name) return;
    const OwlMenuDecl* menu = owl_window_decl_find_menu(decl, menu_name);
    if (!menu || !menu->is_context) return;
    ContextMenuBinding* b =
        (ContextMenuBinding*)calloc(1, sizeof(ContextMenuBinding));
    b->window = w;
    b->decl   = decl;
    snprintf(b->menu_name, sizeof(b->menu_name), "%s", menu_name);
    /*  — sender des surSelection = widget declencheur (champ cliqué
     * ou fenetre pour un menu de fond). Un menu partage entre plusieurs champs
     * recoit une liaison distincte par champ, chacune avec son propre sender. */
    if (trigger_name)
        snprintf(b->sender_name, sizeof(b->sender_name), "%s", trigger_name);
    if (widget) owl_ui_install_context_menu(widget, dispatch_context_menu, b);
    else        owl_ui_install_window_context_menu(w, dispatch_context_menu, b);

    /*  — raccourcis actifs menu ferme (widget/fenetre focus). Le popup
     * (dispatch_context_menu) recree l'arbre au clic droit ; ces liaisons-la
     * sont posees UNE fois a l'attache, independamment du popup. */
    for (int i = 0; i < menu->child_count; i++)
        install_context_shortcut_node(w, widget, menu->children[i], trigger_name);
}

/* Nom du menu principal effectif d'une instance : la valeur du casier
 * (posee par le seed declaratif puis par toute bascule runtime) si elle est
 * exploitable, sinon le declaratif brut. */
static const char* window_effective_main_menu_name(ObjWindowInstance* wi,
                                                    const OwlWindowDecl* decl) {
    Value v;
    if (wi && obj_window_instance_headless_get(wi, "MenuPrincipal", &v)
        && IS_STRING(v)) {
        return AS_CSTRING(v);
    }
    return decl ? decl->main_menu_name : NULL;
}

/* Construit la fenetre Qt complete depuis l'IR. */
/*  — jalons du chemin menu (gates OWL_MENU_TRACE, impl platform.c). */
void owl_menu_trace(const char* fmt, ...);

static OwlWindow* build_window(const OwlWindowDecl* decl) {
    owl_menu_trace("build_window ENTER decl=%s main_menu=%s ctx_menu=%s",
                   decl->name ? decl->name : "?",
                   decl->main_menu_name ? decl->main_menu_name : "(none)",
                   decl->context_menu_name ? decl->context_menu_name : "(none)");
    /* V1.5-A : resout les expressions dynamiques (eval ObjFunction) des props
     * FENETRE AVANT tout helper prop_* (Titre, Largeur, Disposition...).
     * Casts le const : le decl est sharing entre Ouvre et Ouvre re-call, on
     * transforme une fois pour toutes la struct en full-literal apres le
     * premier passage.
     *  — la resolution des props des CONTROLES est differee APRES le
     * top-level fenetre (cf. plus bas) : une prop dynamique de widget
     * (`Valeur = gGlob`) peut referencer une globale top-level fenetre, qui
     * n'existe qu'une fois le top-level execute. Symetrie avec le chemin
     * headless (open_window_instance, seed LITERAL avant / DYNAMIC apres). */
    resolve_dynamic_props((OwlPropertyInit*)decl->properties, decl->property_count);

    const char* title = prop_str(decl->properties, decl->property_count,
                                  "Titre", "Title", decl->name);
    OwlWindow* w = owl_ui_window_create(title);

    int largeur = prop_int(decl->properties, decl->property_count,
                            "Largeur", "Width", 640);
    int hauteur = prop_int(decl->properties, decl->property_count,
                            "Hauteur", "Height", 480);
    owl_ui_window_set_size(w, largeur, hauteur);

    /* Position : X/Y avec -1 = centre. Defaut -1/-1 (centree). */
    int x = prop_int(decl->properties, decl->property_count, "X", "X", -1);
    int y = prop_int(decl->properties, decl->property_count, "Y", "Y", -1);
    owl_ui_window_set_position(w, x, y);

    /* Icone : polymorphe (chemin fichier | Image | Icone catalogue).
     *  — prop_value au lieu de prop_str pour reconnaitre l'ObjIcone
     * catalogue, sinon il etait coerce en chaine inexploitable et ignore. */
    Value iconeVal = prop_value(decl->properties, decl->property_count,
                                 "Icone", "Icon");
    if (!IS_NIL(iconeVal)) window_set_icon_value(w, iconeVal);

    /* Couleur de fond (RVB entier BGR, canon). -1 = fond systeme.
     * Appliquee avant Grise : l'overlay grise (s'il est pose) prime
     * visuellement, ce qui est le comportement attendu (champ HS). */
    int win_bg = prop_int(decl->properties, decl->property_count,
                           "CouleurFond", "BackgroundColor", -1);
    if (win_bg >= 0) owl_ui_window_set_bg_color(w, win_bg);

    /* Grise initial (overlay sombre + setEnabled false). */
    bool grise_init = prop_bool(decl->properties, decl->property_count,
                                 "Grise", "Disabled", false);
    if (grise_init) owl_ui_window_set_disabled(w, 1);

    /* Redimensionnable (defaut Vrai). Si Faux : setFixedSize. */
    bool resize_ok = prop_bool(decl->properties, decl->property_count,
                                "Redimensionnable", "Resizable", true);
    if (!resize_ok) owl_ui_window_set_resizable(w, 0);

    /* Lot F — Disposition / Layout.
     * Si la fenetre a une Disposition != dispLibre, on installe un QLayout sur
     * le central widget. Defaut = dispVerticale (0). Les enfants seront ajoutes
     * via owl_ui_layout_add_child (et leur X/Y est ignore). En mode dispLibre,
     * on retombe sur l'ancien fallback "auto-stack" pour ne pas bloquer les
     * fenetres explicitement positionnees X/Y. */
    int win_disp = prop_int(decl->properties, decl->property_count,
                             "Disposition", "Layout", OWL_DISP_VERTICALE);
    bool win_has_layout = (win_disp != OWL_DISP_LIBRE);
    if (win_has_layout) {
        int pad  = prop_int(decl->properties, decl->property_count, "Padding", "Padding", 0);
        int padL = prop_int(decl->properties, decl->property_count, "PaddingGauche", "PaddingLeft", pad);
        int padT = prop_int(decl->properties, decl->property_count, "PaddingHaut",   "PaddingTop", pad);
        int padR = prop_int(decl->properties, decl->property_count, "PaddingDroite", "PaddingRight", pad);
        int padB = prop_int(decl->properties, decl->property_count, "PaddingBas",    "PaddingBottom", pad);
        int spacing = prop_int(decl->properties, decl->property_count, "Espacement", "Spacing", 6);
        int cols = prop_int(decl->properties, decl->property_count, "Colonnes", "Columns", 1);
        int rows = prop_int(decl->properties, decl->property_count, "Lignes",   "Rows",    0);
        /* Defaut transverse pour les enfants (override par leur prop Alignement). */
        int defH = prop_int(decl->properties, decl->property_count, "AlignH", "AlignH", 0);
        int defV = prop_int(decl->properties, decl->property_count, "AlignV", "AlignV", 0);
        owl_ui_window_set_layout(w, win_disp, padL, padT, padR, padB, spacing, cols, rows, defH, defV);
    }

    /* Map locale : nom de Conteneur -> (widget, has_layout). Utilise pour
     * resoudre le parent quand un enfant est tag parent_name. Capacite 64
     * suffit pour V1 (cf. note interne si besoin d'extension).
     *
     * NB : decl->controls est en ORDRE D'APPEND POSTORDRE (children avant leur
     * Conteneur parent — cf compiler.c::window_parse_subfield). On utilise
     * donc un algo iteratif "process when parent is ready" avec defer, qui
     * preserve l'ordre source pour les siblings (important pour QLayout). */
    /*  (B) — any_etire : au moins un enfant DIRECT de ce layout porte
     * Étire=Vrai (facteur de stretch qui consomme deja l'excedent de l'axe
     * principal). Dans ce cas on n'ajoute PAS de ressort terminal (il volerait
     * la moitie de l'excedent a l'enfant elastique). Sinon (aucun Étire), on
     * empile en haut/gauche via un addStretch terminal. */
    typedef struct { const char* name; OwlControl* widget; bool has_layout; bool any_etire; } ContEntry;
    ContEntry cont_map[64];
    int cont_count = 0;
    bool win_any_etire = false;
    bool processed[256];
    for (int i = 0; i < 256; i++) processed[i] = false;
    int unprocessed = decl->control_count;

    /*  — liaison des PROCEDURE custom (user_procs) AVANT toute
     * execution de code de la fenetre. Le canon exige qu'un helper FEN
     * soit appelable PARTOUT : top-level fenetre, top-level champ,
     * FinInitialisation, handlers. Si on lie apres les top-levels (ancien
     * "phase 6"), le top-level d'un bloc champ crashe sur "Seules les
     * fonctions et procedures peuvent etre appelees" parce que la globale
     * '<window>_<proc>' n'existe pas encore.
     *
     * Les user_procs viennent du decl compile (ObjFunction* prets a etre
     * appeles) — pas de dependance sur les widgets Qt, le binding est safe
     * tres tot. Parite stricte avec open_window_instance (headless). */
    for (int i = 0; i < decl->user_proc_count; i++) {
        char qname[256];
        snprintf(qname, sizeof(qname), "%s_%s", decl->name, decl->user_procs[i].name);
        obj_globals_set(qname, OBJ_VAL(decl->user_procs[i].impl));
    }
    for (int i = 0; i < decl->control_count; i++) {
        OwlControlDecl* c = &decl->controls[i];
        for (int j = 0; j < c->user_proc_count; j++) {
            char qname[256];
            snprintf(qname, sizeof(qname), "%s_%s", c->name, c->user_procs[j].name);
            obj_globals_set(qname, OBJ_VAL(c->user_procs[j].impl));
        }
    }

    /*  — top-level FENETRE : execute AVANT la creation des champs
     * (apres seed du declaratif, fait dans open_window_instance). Ses
     * declarations globales doivent etre disponibles quand les top-levels des
     * champs s'executent. Arity 0 (cf compiler.c __fenetre_<X>_TopLevel) -> on
     * appelle sans aucun argument (call_event_block pousserait MaFenetre et
     * provoquerait un mismatch d'arite).
     *
     *  — push event_sender = la fenetre pour MoiMême. */
    Value winSenderGui = moimeme_widget_value_for_name(decl->name);
    if (decl->top_level) {
        vm_event_sender_push(winSenderGui);
        vm_push(OBJ_VAL(decl->top_level));
        vm_call_reentrant(OBJ_VAL(decl->top_level), 0);
        vm_pop();  /* valeur de retour */
        vm_event_sender_pop();
    }

    /*  — resolution + seed store des props dynamiques des CONTROLES
     * APRES le top-level fenetre (les globales top-level sont posees) mais
     * AVANT la boucle build_control. build_control lit les props resolues
     * (prop_int/prop_str/prop_value directement sur le decl) ET le composite
     * Style depuis le store de l'uici (obj_ui_control_instance_headless_get) :
     * il faut donc que les DYNAMIQUES soient a la fois resolues et seedees
     * dans le store a ce point. On seede via le meme uici que build_control
     * consulte (current_window_uici_for_decl) -> coherence garantie. Sans ce
     * report, `Valeur = gGlob` (gGlob = globale top-level) echouait sur
     * "Variable inconnue 'FEN_X$gGlob'". Les litteraux ont deja ete seedes
     * avant build_window (open_window_instance, SEED_PHASE_LITERAL). */
    for (int i = 0; i < decl->control_count; i++) {
        OwlControlDecl* c = (OwlControlDecl*)&decl->controls[i];
        ObjUiControlInstance* uici = current_window_uici_for_decl(c);
        if (uici) {
            seed_uici_headless_phase(uici, c, SEED_PHASE_DYNAMIC);
        } else {
            /* Pas d'instance sur la pile (ex. re-rendu dCopieImage d'une
             * fenetre deja materialisee) : le store est deja seede, on se
             * contente de resoudre pour les lectures directes de props. */
            resolve_dynamic_props(c->properties, c->property_count);
        }
        for (int j = 0; j < c->column_count; j++) {
            resolve_dynamic_props(c->columns[j].properties, c->columns[j].property_count);
        }
    }

    /*  — `Style = theme.<nom>` des arbres de menu. Resolu ICI (apres le
     * top-level fenetre, comme les props dynamiques des champs, donc les globales
     * top-level sont posees) et AVANT build_control : un menu contextuel est
     * installe sur son champ pendant la boucle ci-dessous, la barre juste apres. */
    resolve_menu_style_refs(decl);

    int auto_y = 10;
    while (unprocessed > 0) {
        int progress = 0;
        for (int i = 0; i < decl->control_count; i++) {
            if (processed[i]) continue;
            OwlControlDecl* c = &decl->controls[i];

            /* ZR : si ce controle est un membre du template d'une ZoneRepetee
             * (= parent_name pointe sur un ctrl kind=ZONE_REPETEE), on NE le
             * construit PAS au moment de l'ouverture. Il sera clone +
             * instancie a chaque ZoneRepeteeAjouteLigne. */
            if (c->parent_name && zr_decl_is_zr_name(decl, c->parent_name)) {
                processed[i] = true;
                unprocessed--;
                progress++;
                continue;
            }

            /* Verifie que le parent est pret (NULL ou deja dans cont_map). */
            OwlControl* container_parent = NULL;
            bool parent_has_layout = win_has_layout;
            if (c->parent_name) {
                bool ready = false;
                for (int k = 0; k < cont_count; k++) {
                    if (strcmp(cont_map[k].name, c->parent_name) == 0) {
                        container_parent = cont_map[k].widget;
                        parent_has_layout = cont_map[k].has_layout;
                        ready = true;
                        break;
                    }
                }
                if (!ready) continue;  /* defer */
            }
            OwlWindow* parent_slot = container_parent
                ? owl_ui_container_as_parent(container_parent) : w;

        /* Build du widget. Conteneur = cas special (creation + son propre layout). */
        OwlControl* widget = NULL;
        int defH = (c->kind == OWL_CONTROL_TABLE)  ? 300
                 : (c->kind == OWL_CONTROL_BUTTON) ? 30
                 : 25;
        int h = prop_int(c->properties, c->property_count, "Hauteur", "Height", defH);

        if (c->kind == OWL_CONTROL_CONTAINER) {
            int x = prop_int(c->properties, c->property_count, "X", "X", -1);
            int y = prop_int(c->properties, c->property_count, "Y", "Y", -1);
            int wpx = prop_int(c->properties, c->property_count, "Largeur", "Width", 0);
            int hpx = prop_int(c->properties, c->property_count, "Hauteur", "Height", 0);
            widget = owl_ui_container_create(parent_slot, x, y, wpx, hpx);
            int cdisp = prop_int(c->properties, c->property_count,
                                  "Disposition", "Layout", OWL_DISP_VERTICALE);
            bool this_has_layout = (cdisp != OWL_DISP_LIBRE);
            if (this_has_layout) {
                int cpad  = prop_int(c->properties, c->property_count, "Padding", "Padding", 0);
                int cpadL = prop_int(c->properties, c->property_count, "PaddingGauche", "PaddingLeft", cpad);
                int cpadT = prop_int(c->properties, c->property_count, "PaddingHaut",   "PaddingTop", cpad);
                int cpadR = prop_int(c->properties, c->property_count, "PaddingDroite", "PaddingRight", cpad);
                int cpadB = prop_int(c->properties, c->property_count, "PaddingBas",    "PaddingBottom", cpad);
                int cspacing = prop_int(c->properties, c->property_count, "Espacement", "Spacing", 6);
                int ccols = prop_int(c->properties, c->property_count, "Colonnes", "Columns", 1);
                int crows = prop_int(c->properties, c->property_count, "Lignes",   "Rows", 0);
                int cdefH = prop_int(c->properties, c->property_count, "AlignH", "AlignH", 0);
                int cdefV = prop_int(c->properties, c->property_count, "AlignV", "AlignV", 0);
                owl_ui_container_set_layout(widget, cdisp,
                                             cpadL, cpadT, cpadR, cpadB,
                                             cspacing, ccols, crows,
                                             cdefH, cdefV);
            }
            if (cont_count < 64) {
                cont_map[cont_count].name = c->name;
                cont_map[cont_count].widget = widget;
                cont_map[cont_count].has_layout = this_has_layout;
                cont_map[cont_count].any_etire = false;
                cont_count++;
            }
            /*  — le Conteneur ne transite PAS par build_control : le style
             * aggrege declaratif (Style..CadreExtérieur..) seede dans le store de
             * l'uici doit donc etre applique ici, en calquant le bloc styleVal de
             * build_control. Sans ca la branche CONTAINER d'apply_style_to_control
             * n'etait jamais atteinte a l'ouverture (fond/cadre invisibles). */
            {
                ObjUiControlInstance* cuici = current_window_uici_for_decl(c);
                if (cuici) cuici->widget = widget;
                Value cstyleVal = NIL_VAL;
                if (cuici)
                    (void)obj_ui_control_instance_headless_get(cuici, "Style", &cstyleVal);
                if (IS_INSTANCE(cstyleVal) && widget && cuici) {
                    inst_widget_watch_tag_recursive(AS_INSTANCE(cstyleVal), widget);
                    style_root_record(widget, AS_INSTANCE(cstyleVal));
                    apply_style_to_control(widget, AS_INSTANCE(cstyleVal),
                                            OWL_CONTROL_CONTAINER);
                }
            }
        } else if (c->kind == OWL_CONTROL_ONGLET) {
            /*  — Champ Onglet = QTabWidget. Les Volets (enfants directs)
             * s'y ajoutent par addTab ; les vrais champs vont dans la page du
             * Volet. Position de la barre + geometrie comme un champ. */
            int x   = prop_int(c->properties, c->property_count, "X", "X", -1);
            int y   = prop_int(c->properties, c->property_count, "Y", "Y", -1);
            int wpx = prop_int(c->properties, c->property_count, "Largeur", "Width", 0);
            int hpx = prop_int(c->properties, c->property_count, "Hauteur", "Height", 0);
            widget = owl_ui_onglet_create(parent_slot, x, y, wpx, hpx);
            int pos = prop_int(c->properties, c->property_count,
                                "Position", "Position", OWL_POS_HAUT);
            if (widget) owl_ui_onglet_set_position(widget, pos);
            /*  — reglages Detail : largeur des languettes (a poser
             * avant l'ajout des volets) + alignement horizontal du libelle. */
            int larg = prop_int(c->properties, c->property_count,
                                 "LargeurVolets", "TabWidth",
                                 OWL_LARGVOL_PROPORTIONNELLE);
            if (widget) owl_ui_onglet_set_tab_width_mode(widget, larg);
            int lalign = prop_int(c->properties, c->property_count,
                                  "AlignementLibelle", "LabelAlignment",
                                  OWL_ALIGN_CENTRE);
            if (widget) owl_ui_onglet_set_label_alignment(widget, lalign);
            /*  — style typé du Champ Onglet -> feuille QSS (pane +
             * languettes actives/inactives + survol). No-op si aucun style posé
             * (rendu par défaut du thème inchangé).
             *
             *  — deux sources composées : l'INSTANCE styleOnglet du store
             * (posée par `Style = theme.<nom>`, deep-clonée par le seed donc
             * isolée du thème) sert de BASE ; les slots plats const-foldés par le
             * parseur (`Style = { ... }` littéral et `Style..X..Y = v`) sont la
             * SURCHARGE — canon , dernière écriture gagne. */
            if (widget) {
                OwlOngletStyleResolved ostyle;
                onglet_resolve_style(c, &ostyle);
                ObjUiControlInstance* suici = current_window_uici_for_decl(c);
                Value ongStyleVal = NIL_VAL;
                if (suici)
                    (void)obj_ui_control_instance_headless_get(suici, "Style",
                                                               &ongStyleVal);
                if (IS_INSTANCE(ongStyleVal))
                    onglet_style_fill_from_instance(AS_INSTANCE(ongStyleVal), &ostyle);
                if (ostyle.has_style) owl_ui_onglet_apply_style(widget, &ostyle);
            }
            /* Etat unifie : un QTabWidget est un QWidget, la mecanique
             * generique setEnabled/setVisible (+ filtre Inactif pour
             * AffichageSeulement, sans lecture-seule native) s'applique. Le
             * build loop de l'Onglet ne passe pas par build_control : on
             * applique donc l'Etat ici, comme le bloc generique le fait pour
             * les autres champs. -1 = non specifie (laisse Visible/Grise). */
            int ongEtat = prop_int(c->properties, c->property_count,
                                    "Etat", "State", -1);
            if (widget && ongEtat != -1)
                owl_ui_control_set_etat(widget, ongEtat);
            /* Enregistre l'Onglet comme "parent" pour que les Volets resolvent
             * leur QTabWidget. has_layout=false : un Volet n'est PAS ajoute au
             * layout du parent (il passe par addTab) -> parent_has_layout faux. */
            if (cont_count < 64 && widget) {
                cont_map[cont_count].name = c->name;
                cont_map[cont_count].widget = widget;
                cont_map[cont_count].has_layout = false;
                cont_map[cont_count].any_etire = false;
                cont_count++;
            }
            ObjUiControlInstance* ouici = current_window_uici_for_decl(c);
            if (ouici) ouici->widget = widget;
            /* Clic utilisateur -> maj store + surChangementVolet. Source unique
             * cote GUI ; un set programmatique bloque les signaux (pas de
             * double-fire, l'event vient alors du hook OP_STRUCT_SET). */
            if (ouici && widget)
                owl_ui_onglet_on_change(widget, dispatch_onglet_change, (void*)ouici);
        } else if (c->kind == OWL_CONTROL_VOLET) {
            /*  — un Volet = page (QWidget avec son propre layout, modele
             * Conteneur) ajoutee au QTabWidget parent par addTab. container_parent
             * est le QTabWidget (resolu via cont_map). */
            OwlControl* page = owl_ui_onglet_volet_create();
            int vdisp = prop_int(c->properties, c->property_count,
                                  "Disposition", "Layout", OWL_DISP_VERTICALE);
            bool this_has_layout = (vdisp != OWL_DISP_LIBRE);
            if (this_has_layout && page) {
                int vpad  = prop_int(c->properties, c->property_count, "Padding", "Padding", 0);
                int vpadL = prop_int(c->properties, c->property_count, "PaddingGauche", "PaddingLeft", vpad);
                int vpadT = prop_int(c->properties, c->property_count, "PaddingHaut",   "PaddingTop", vpad);
                int vpadR = prop_int(c->properties, c->property_count, "PaddingDroite", "PaddingRight", vpad);
                int vpadB = prop_int(c->properties, c->property_count, "PaddingBas",    "PaddingBottom", vpad);
                int vspacing = prop_int(c->properties, c->property_count, "Espacement", "Spacing", 6);
                int vcols = prop_int(c->properties, c->property_count, "Colonnes", "Columns", 1);
                int vrows = prop_int(c->properties, c->property_count, "Lignes",   "Rows", 0);
                int vdefH = prop_int(c->properties, c->property_count, "AlignH", "AlignH", 0);
                int vdefV = prop_int(c->properties, c->property_count, "AlignV", "AlignV", 0);
                owl_ui_container_set_layout(page, vdisp,
                                             vpadL, vpadT, vpadR, vpadB,
                                             vspacing, vcols, vrows,
                                             vdefH, vdefV);
            }
            const char* libelle = prop_str(c->properties, c->property_count,
                                            "Libelle", "Caption", "");
            if (container_parent && page)
                owl_ui_onglet_add_volet(container_parent, page, libelle);
            widget = page;
            /* Enregistre la page sous le nom du Volet : ses champs enfants
             * (parent_name == Volet) y sont ajoutes comme dans un Conteneur. */
            if (cont_count < 64 && page) {
                cont_map[cont_count].name = c->name;
                cont_map[cont_count].widget = page;
                cont_map[cont_count].has_layout = this_has_layout;
                cont_map[cont_count].any_etire = false;
                cont_count++;
            }
            ObjUiControlInstance* vuici = current_window_uici_for_decl(c);
            if (vuici) vuici->widget = page;
        } else {
            widget = build_control(parent_slot, c, auto_y);
            if (!parent_has_layout) auto_y += h + 8;
            /* ZR : enregistre le runtime tracking (template indices +
             * reps list) pour permettre AjouteLigne / indexation. */
            if (c->kind == OWL_CONTROL_ZONE_REPETEE && widget) {
                zr_register_at_open(widget, decl, c->name);
            }
        }

        /* Ajout au layout du parent (si parent en a un). */
        if (parent_has_layout && widget) {
            int etire   = prop_bool(c->properties, c->property_count, "\xc3\x89tire", "Stretch", false) ? 1 : 0;
            if (!etire) etire = prop_bool(c->properties, c->property_count, "Etire", "Stretch", false) ? 1 : 0;
            /*  (B) — memorise si CE layout parent a un enfant elastique
             * (Étire) : il consomme deja l'excedent principal, pas de ressort
             * terminal a poser plus bas. */
            if (etire) {
                if (container_parent) {
                    for (int k = 0; k < cont_count; k++) {
                        if (cont_map[k].widget == container_parent) {
                            cont_map[k].any_etire = true;
                            break;
                        }
                    }
                } else {
                    win_any_etire = true;
                }
            }
            int align   = prop_int (c->properties, c->property_count, "Alignement", "Alignment", 0);
            int margin  = prop_int (c->properties, c->property_count, "Marge", "Margin", 0);
            int mL      = prop_int (c->properties, c->property_count, "MargeGauche", "LeftMargin",   margin);
            int mT      = prop_int (c->properties, c->property_count, "MargeHaut",   "TopMargin",    margin);
            int mR      = prop_int (c->properties, c->property_count, "MargeDroite", "RightMargin",  margin);
            int mB      = prop_int (c->properties, c->property_count, "MargeBas",    "BottomMargin", margin);
            int colspan = prop_int (c->properties, c->property_count, "ColSpan", "ColumnSpan", 1);
            int rowspan = prop_int (c->properties, c->property_count, "RowSpan", "RowSpan", 1);
            /*  (A) — Largeur/Hauteur declarees explicitement (0 = non
             * figee) : figent l'axe principal du box layout parent. On ne prend
             * PAS le defaut de defH/hauteur naturelle ici, seule une valeur
             * ecrite par l'utilisateur doit contraindre le layout. */
            int declW = prop_int(c->properties, c->property_count, "Largeur", "Width", 0);
            int declH = prop_int(c->properties, c->property_count, "Hauteur", "Height", 0);
            owl_ui_layout_add_child(container_parent ? NULL : w,
                                     container_parent,
                                     widget,
                                     etire, align,
                                     mL, mT, mR, mB,
                                     -1, -1, /* col/row : auto-append en grille V1 */
                                     colspan, rowspan,
                                     declW, declH);
        }

        /*  — top-level CHAMP (apres creation widget, ordre source).
         * Arity 0. Remplace l'ancien event Initialisation (mort ).
         *  — push event_sender = le champ pour MoiMême. */
        if (c->top_level) {
            Value ctrlSenderGui = moimeme_widget_value_for_name(c->name);
            vm_event_sender_push(ctrlSenderGui);
            vm_push(OBJ_VAL(c->top_level));
            vm_call_reentrant(OBJ_VAL(c->top_level), 0);
            vm_pop();
            vm_event_sender_pop();
        }

        /*  — menu contextuel du champ (clic droit) : pose la policy +
         * le callback de demande. L'arbre est construit a la volee au clic. */
        if (widget && c->context_menu_name) {
            owl_menu_trace("build_window field_ctx_menu champ=%s menu=%s",
                           c->name ? c->name : "?", c->context_menu_name);
            install_context_menu_on(w, widget, decl, c->context_menu_name,
                                    c->name);
        }

            processed[i] = true;
            unprocessed--;
            progress++;
        }
        if (progress == 0) break;  /* orphan loop guard : parent_name introuvable */
    }

    /*  (B) — ressort terminal : empile les widgets en haut (pile
     * verticale) / a gauche (pile horizontale) en absorbant l'excedent de l'axe
     * principal en fin de layout. Pose uniquement sur les box layouts SANS
     * enfant Étire (un enfant elastique consomme deja l'excedent ; un ressort
     * terminal lui volerait la moitie de l'espace). Le grille et le dispLibre
     * gerent leur distribution autrement -> no-op cote bridge. */
    if (win_has_layout && !win_any_etire) {
        owl_ui_layout_add_terminal_stretch(w, NULL);
    }
    for (int k = 0; k < cont_count; k++) {
        if (cont_map[k].has_layout && !cont_map[k].any_etire) {
            owl_ui_layout_add_terminal_stretch(NULL, cont_map[k].widget);
        }
    }

    /*  — post-pass Onglet/Volet : le build loop cree l'Onglet AVANT ses
     * Volets, donc aucune page a positionner au moment du create. Ici, tous les
     * addTab sont faits : on positionne le volet actif (..Valeur, 1-based ->
     * setCurrentIndex signaux bloques, pas d'event au demarrage) et on applique
     * la visibilite declaree de chaque Volet (masquage de languette). Les valeurs
     * initiales viennent des props DECLARATIVES (comme build_control) : le store
     * headless n'est pas encore garanti seede a ce point du build. */
    for (int i = 0; i < decl->control_count; i++) {
        OwlControlDecl* c = &decl->controls[i];
        if ((OwlControlKind)c->kind == OWL_CONTROL_ONGLET) {
            ObjUiControlInstance* ouici = current_window_uici_for_decl(c);
            if (!ouici || !ouici->widget) continue;
            int val = prop_int(c->properties, c->property_count,
                                "Valeur", "Value", 1);
            owl_ui_onglet_set_current(ouici->widget, val - 1);
        } else if ((OwlControlKind)c->kind == OWL_CONTROL_VOLET) {
            ObjUiControlInstance* vuici = current_window_uici_for_decl(c);
            if (!vuici || !vuici->widget) continue;
            bool vis = prop_bool(c->properties, c->property_count,
                                  "Visible", "Visible", true);
            if (!vis) owl_ui_volet_set_visible(vuici->widget, 0);
        }
    }

    /*  lot C — unification GUI/headless. Le proxy ObjWindowRef
     * pose dans vm.globals[FEN_X] par window_declaration (compiler.c) reste
     * en place pendant toute la vie de la fenetre. Les acces FEN_X..Prop
     * passent par la branche IS_WINDOW_REF de OP_STRUCT_GET/SET (vm.c) qui
     * lit/ecrit le store d'instance, avec reflet Qt via le hook
     * owl_ui_apply_window_scalar. Plus de shadow ObjUiControl(WINDOW). On
     * rattache uniquement le widget Qt frais a l'ObjWindowInstance top
     * pour que le hook puisse le retrouver. */
    ObjWindowInstance* top_inst = vm_window_stack_top();
    if (top_inst != NULL && top_inst->widget == NULL) {
        top_inst->widget = w;
    }

    /*  — habille le panneau central si un Style (base Style ou
     * styleConteneur) a été posé (déclaratif ou top-level fenêtre). Appliqué
     * APRÈS la CouleurFond plate (build_window plus haut) : un Style portant un
     * CadreExtérieur prime sur le fond plat. Le top-level fenêtre a déjà tourné
     * (widget encore NULL à ce moment) : on relit donc depuis le store. */
    if (top_inst != NULL) {
        Value styleVal;
        if (obj_window_instance_headless_get(top_inst, "Style", &styleVal)
            && IS_INSTANCE(styleVal)) {
            apply_style_to_window(w, AS_INSTANCE(styleVal));
        }
    }

    /* (Phase 6 historique — liaison des user_procs — remontee AVANT le
     * top-level fenetre par  pour que les helpers FEN soient
     * appelables PARTOUT, y compris depuis le top-level d'un bloc champ.) */

    /* PROCEDURE FinInitialisation() de la fenetre (apres tous les controls
     * ET apres exposition de FEN_X comme ObjUiControl, pour que le code
     * utilisateur puisse y faire FEN_Login..Titre = "..."). Canon OWL :
     * FinInitialisation (FR) / EndInitialization (EN). L'event Initialisation
     * n'existe plus : le code d'init vit dans le top-level
     * fenetre + champ, declenche plus haut. */
    {
        const OwlEventBlock* init = find_event(decl->events, decl->event_count,
                                                "FinInitialisation",
                                                "EndInitialization");
        if (init) call_event_block(init->impl, true);
    }

    /* Lot D : cabler PROCEDURE Fermeture() au closeEvent Qt. */
    const OwlEventBlock* close = find_event(decl->events, decl->event_count,
                                             "Fermeture", "Closing");
    if (close) {
        int idx = register_binding_named(close->impl, true, decl->name);
        if (idx >= 0) {
            owl_ui_window_on_close(w, dispatch_event, (void*)(intptr_t)idx);
        }
    }

    /*  — PROCEDURE AutorisationFermeture() sur le MEME QEvent::Close :
     * le veto doit valoir aussi bien pour Ferme() que pour la case de fermeture
     * de la fenetre. Un retour Faux refuse l'event, donc Fermeture ne tire pas
     * et la fenetre reste ouverte. */
    const OwlEventBlock* closeAuth = find_event(decl->events, decl->event_count,
                                                 "AutorisationFermeture",
                                                 "CloseAuthorization");
    if (closeAuth) {
        int idx = register_binding_named(closeAuth->impl, true, decl->name);
        if (idx >= 0) {
            owl_ui_window_on_close_query(w, dispatch_close_query,
                                         (void*)(intptr_t)idx);
        }
    }

    /*  — clavier au niveau fenetre : surToucheAppuyee /
     * surToucheRelachee. Cible des fleches directionnelles (Snake) et des
     * touches d'un champ focuse sans handler propre (bubble-up). Le dispatch
     * passe le code touche en payload (dispatch_image_event_1i). */
    {
        const OwlEventBlock* wkdown = find_event(decl->events, decl->event_count,
                "surToucheAppuy\xc3\xa9""e", "onKeyDown");
        const OwlEventBlock* wkup = find_event(decl->events, decl->event_count,
                "surToucheRel\xc3\xa2""ch\xc3\xa9""e", "onKeyUp");
        if (wkdown || wkup) {
            int idown = wkdown ? register_binding_named(wkdown->impl, true, decl->name) : -1;
            int iup   = wkup   ? register_binding_named(wkup->impl,   true, decl->name) : -1;
            owl_ui_install_window_key_filter(w,
                (idown >= 0) ? dispatch_image_event_1i : NULL, (void*)(intptr_t)idown,
                (iup   >= 0) ? dispatch_image_event_1i : NULL, (void*)(intptr_t)iup);
        }
    }

    /*  — surChangementTaille / onResize : declenche au resize de la
     * fenetre (ecriture de ..Largeur/..Hauteur, FenTaille, Maximise...). Le
     * filtre est installe AVANT le show pour initialiser sa taille de
     * reference sur la geometrie deja posee (le resize d'exposition, de meme
     * dimension, ne tire donc pas). */
    {
        const OwlEventBlock* wresize = find_event(decl->events, decl->event_count,
                "surChangementTaille", "onResize");
        if (wresize) {
            int idx = register_binding_named(wresize->impl, true, decl->name);
            if (idx >= 0) {
                owl_ui_install_window_resize_filter(w, dispatch_image_event_2i,
                                                    (void*)(intptr_t)idx);
            }
        }
    }

    /*  — barre de menu classique. Posee APRES les champs et les
     * top-levels (une option peut piloter un champ) mais AVANT le show, pour
     * que la fenetre s'affiche deja coiffee de sa barre. Seul le MenuPrincipal
     * effectivement affecte est rendu ; les autres blocs declares restent
     * disponibles pour une bascule runtime. */
    owl_menu_trace("build_window BEFORE main_menu w=%p", (void*)w);
    build_window_main_menu(w, decl,
                           window_effective_main_menu_name(top_inst, decl));
    owl_menu_trace("build_window AFTER main_menu");

    /*  — menu contextuel du fond de fenetre (clic droit hors champ). */
    if (decl->context_menu_name) {
        owl_menu_trace("build_window BEFORE window_ctx_menu name=%s",
                       decl->context_menu_name);
        /* menu attache au fond : declencheur = la fenetre elle-meme. */
        install_context_menu_on(w, NULL, decl, decl->context_menu_name,
                                decl->name);
        owl_menu_trace("build_window AFTER window_ctx_menu");
    }

    bool visible = prop_bool(decl->properties, decl->property_count,
                              "Visible", "Visible", true);
    owl_menu_trace("build_window BEFORE show visible=%d", (int)visible);
    if (visible) owl_ui_window_show(w);
    /* Fenetre centree (X/Y non specifies) plus grande que l'ecran : le clamp
     * pre-show positionne la zone client, mais le cadre (barre de titre) peut
     * deborder en haut. On finalise apres show(), quand frameGeometry connait
     * le chrome. On ne touche pas aux fenetres positionnees explicitement. */
    if (visible && (x < 0 || y < 0)) owl_ui_window_clamp_titlebar_onscreen(w);
    owl_menu_trace("build_window AFTER show / RETURN");

    return w;
}

/* ====================================================================
 *  Boites de dialogue (5 natives - implem complete)
 * ==================================================================== */

/* Garde-fou mode headless : sous `owl test`, vm.headless_mode est
 * ON par defaut, ce qui court-circuite les popups Qt modales. Le mock-system
 * reste le mecanisme PRINCIPAL pour intercepter et compter les appels — ce
 * garde n'intervient que si aucun mock n'a ete pose. Sans lui, un test qui
 * oublie de mocker Info/Erreur ouvre une fenetre bloquante et fige le runner
 * (ou cumule un etat Qt pollue sur les tests suivants). Le dev peut
 * desactiver localement avec TestModeHeadless(Faux) pour exercer la vraie UI.
 * Cf testing.c (set headless_mode=true par le runner au debut de chaque
 * fichier de test). */

/* Le message est compose AVANT le court-circuit headless : sous `owl test`
 * rien ne s'affiche, mais le contenu reste mesurable (__DebugDernierMessage),
 * donc assertable ligne par ligne. */
static Value fn_info(int argCount, Value* args) {
    char* msg = concat_messages(argCount, args, 0);
    if (!vm.headless_mode) owl_ui_message_info(NULL, msg ? msg : "");
    free(msg);
    return NIL_VAL;
}

static Value fn_erreur(int argCount, Value* args) {
    char* msg = concat_messages(argCount, args, 0);
    if (!vm.headless_mode) owl_ui_message_error(NULL, msg ? msg : "");
    free(msg);
    return NIL_VAL;
}

static Value fn_avertissement(int argCount, Value* args) {
    char* msg = concat_messages(argCount, args, 0);
    if (!vm.headless_mode) owl_ui_message_warning(NULL, msg ? msg : "");
    free(msg);
    return NIL_VAL;
}

/* Sablier([<Activation>]) — bascule le curseur souris en curseur d'attente
 * (sablier) et inversement, pour signaler qu'un traitement long est en cours.
 * Sans argument ou avec Vrai : active le sablier. Avec Faux : restitue le
 * curseur precedent. Les activations s'empilent (semantique de pile Qt) :
 * N activations exigent N restitutions pour retrouver le curseur d'origine.
 * Ne retourne rien.
 *
 * Validation du type AVANT le court-circuit headless : un argument non-booleen
 * pose ErreurDetectee meme sous owl test. En headless (owl test par defaut),
 * no-op effectif : aucun curseur a l'ecran. */
static Value fn_sablier(int argCount, Value* args) {
    bool activate = true;  /* defaut sans argument = activation */
    if (argCount >= 1) {
        if (!IS_BOOL(args[0])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Sablier",
                                  "native", "Sablier",
                                  "arg", "activation",
                                  "expected", "booleen",
                                  "actual", "autre",
                                  NULL);
            return NIL_VAL;
        }
        activate = AS_BOOL(args[0]);
    }
    if (vm.headless_mode) return NIL_VAL;
    owl_ui_wait_cursor(activate ? 1 : 0);
    return NIL_VAL;
}

/* ToastAffiche(<Message> [, <Durée> [, <CadrageV> [, <CadrageH> [, <CouleurFond>]]]])
 *
 * Toast = notification ephemere, non bloquante, fade-out auto. Defaults
 * canon : duree=toastCourt (~2s), cv=cvBas, ch=chCentre, couleurFond=-1
 * (laisse le rendu choisir une couleur lisible).
 *
 * Court-circuit vm.headless_mode en PREMIERE instruction : sans ca, les
 * scenarios mockes essaieraient d'ouvrir un widget Qt en CI headless. Le
 * mock-system intercepte au dispatch via call_value AVANT que cette
 * fonction soit appelee — donc on n'aboutit ici qu'en runtime reel. */
static Value fn_toast_affiche(int argCount, Value* args) {
    if (vm.headless_mode) return NIL_VAL;
    if (argCount < 1) {
        fprintf(stderr, "[ToastAffiche] Argument manquant : message attendu.\n");
        return NIL_VAL;
    }
    if (!IS_STRING(args[0])) {
        fprintf(stderr, "[ToastAffiche] Argument 1 doit etre une chaine.\n");
        return NIL_VAL;
    }
    const char* msg = AS_CSTRING(args[0]);

    /* Duree : fallback sur OWL_TOAST_COURT_MS si valeur hors enum (le
     * compiler intercepte deja les violations allowedBase). */
    int duration_ms = OWL_TOAST_COURT_MS;
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        int v = (int)AS_INTEGER(args[1]);
        if (v == OWL_TOAST_LONG) duration_ms = OWL_TOAST_LONG_MS;
        else if (v == OWL_TOAST_COURT) duration_ms = OWL_TOAST_COURT_MS;
        /* Sinon : valeur ignoree, fallback. */
    }

    int cv  = (argCount >= 3 && IS_INTEGER(args[2])) ? (int)AS_INTEGER(args[2]) : OWL_CV_BAS;
    int ch  = (argCount >= 4 && IS_INTEGER(args[3])) ? (int)AS_INTEGER(args[3]) : OWL_CH_CENTRE;
    int rgb = (argCount >= 5 && IS_INTEGER(args[4])) ? (int)AS_INTEGER(args[4]) : -1;

    ensure_ui_init();
    owl_ui_toast_show(msg, duration_ms, cv, ch, rgb);
    return NIL_VAL;
}

static Value fn_ouinon(int argCount, Value* args) {
    int default_btn = 1;
    int msg_start   = 0;
    if (first_arg_is_default_button(argCount, args, 1)) {
        default_btn = (int)AS_INTEGER(args[0]);
        msg_start   = 1;
    }
    if (argCount <= msg_start) return INTEGER_VAL(0);
    char* msg = concat_messages(argCount, args, msg_start);
    int r = default_btn;
    if (!vm.headless_mode) r = owl_ui_message_yesno(NULL, msg ? msg : "", default_btn);
    free(msg);
    return INTEGER_VAL(r);
}

static Value fn_confirmer(int argCount, Value* args) {
    int default_btn = 1;
    int msg_start   = 0;
    if (first_arg_is_default_button(argCount, args, 2)) {
        default_btn = (int)AS_INTEGER(args[0]);
        msg_start   = 1;
    }
    if (argCount <= msg_start) return INTEGER_VAL(0);
    char* msg = concat_messages(argCount, args, msg_start);
    int r = default_btn;
    if (!vm.headless_mode) r = owl_ui_message_confirm(NULL, msg ? msg : "", default_btn);
    free(msg);
    return INTEGER_VAL(r);
}

/* ====================================================================
 *  fSelecteur / fRepSelecteur — selecteurs fichier / repertoire
 * ==================================================================== */

/* Traduit un filtre de types canon vers le format Qt.
 *
 * canon : "<desc>" + TAB + "<motif;motif;...>" [ + CRLF + "<desc2>" + TAB + ... ]
 * Qt : "<desc> (<motif motif ...>);;<desc2> (...)".
 *
 * Retourne une chaine malloc (caller free). NULL si le filtre est vide. Les
 * espaces de bordure des motifs sont conserves tels quels (le backend Qt les
 * tolere ; canon accepte "*. *"). */
static char* fsel_translate_filter(const char* wl_filter) {
    if (!wl_filter || !*wl_filter) return NULL;

    size_t cap = strlen(wl_filter) * 2 + 16;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;
    out[0] = '\0';

    const char* p = wl_filter;
    bool first_group = true;
    while (*p) {
        /* Une entree = <desc> TAB <motifs>, terminee par CR et/ou LF
         * ou fin de chaine. */
        const char* desc_start = p;
        const char* tab = NULL;
        const char* end = p;
        while (*end && *end != '\r' && *end != '\n') {
            if (*end == '\t' && !tab) tab = end;
            end++;
        }
        size_t desc_len = tab ? (size_t)(tab - desc_start) : (size_t)(end - desc_start);
        const char* pat_start = tab ? tab + 1 : end;  /* pas de TAB : motifs vides */
        size_t pat_len = tab ? (size_t)(end - pat_start) : 0;

        if (desc_len > 0 || pat_len > 0) {
            if (!first_group) {
                if (pos + 2 < cap) { out[pos++] = ';'; out[pos++] = ';'; out[pos] = '\0'; }
            }
            first_group = false;
            /* "<desc> (" */
            if (pos + desc_len + 2 < cap) {
                memcpy(out + pos, desc_start, desc_len); pos += desc_len;
                out[pos++] = ' '; out[pos++] = '('; out[pos] = '\0';
            }
            /* motifs : ';' canon -> ' ' Qt. */
            for (size_t i = 0; i < pat_len && pos + 1 < cap; i++) {
                char c = pat_start[i];
                out[pos++] = (c == ';') ? ' ' : c;
            }
            if (pos + 1 < cap) { out[pos++] = ')'; out[pos] = '\0'; }
        }

        /* Avance au groupe suivant (saute la sequence CR/LF). */
        p = end;
        while (*p == '\r' || *p == '\n') p++;
    }

    if (pos == 0) { free(out); return NULL; }
    return out;
}

/* fSelecteur(<RepInitial>, <FichierDefaut>, <Titre>, <TypesFichiers>, <ExtDefaut> [, <Mode>])
 *   ou
 * fSelecteur(<CheminCompletFichierDefaut>, <Titre>, <TypesFichiers>, <ExtDefaut> [, <Mode>])
 *
 * Distinction des 2 syntaxes par le nombre d'args : SYNTAXE1 a 5 ou 6 args,
 * SYNTAXE2 a 4 ou 5 args. Retour : chemin(s) selectionne(s), "" si annule.
 * Multi (fselMulti) : chemins joints par CRLF. */
static Value fn_fselecteur(int argCount, Value* args) {
    if (vm.headless_mode) return OBJ_VAL(obj_copy_string("", 0));

    const char* rep_initial   = "";
    const char* fichier_defaut = "";
    const char* titre         = "";
    const char* filtre_wl     = "";
    int mode = OWL_FSEL_OUVRE;

    /* Le dernier argument est le Mode s'il est entier ; il decale donc la
     * detection de syntaxe. On compte les args "non-mode". */
    int nargs = argCount;
    if (nargs > 0 && IS_INTEGER(args[nargs - 1])) {
        mode = (int)AS_INTEGER(args[nargs - 1]);
        nargs--;
    }

    if (nargs >= 5) {
        /* SYNTAXE1 : RepInitial, FichierDefaut, Titre, TypesFichiers, ExtDefaut */
        if (IS_STRING(args[0])) rep_initial    = AS_CSTRING(args[0]);
        if (IS_STRING(args[1])) fichier_defaut = AS_CSTRING(args[1]);
        if (IS_STRING(args[2])) titre          = AS_CSTRING(args[2]);
        if (IS_STRING(args[3])) filtre_wl      = AS_CSTRING(args[3]);
        /* args[4] = ExtDefaut : applique par Qt via le filtre, non passe ici. */
    } else if (nargs == 4) {
        /* SYNTAXE2 : CheminCompletFichierDefaut, Titre, TypesFichiers, ExtDefaut */
        if (IS_STRING(args[0])) fichier_defaut = AS_CSTRING(args[0]);
        if (IS_STRING(args[1])) titre          = AS_CSTRING(args[1]);
        if (IS_STRING(args[2])) filtre_wl      = AS_CSTRING(args[2]);
    } else {
        /* Tolerance : moins d'args que documente, on prend ce qu'on peut. */
        if (nargs >= 1 && IS_STRING(args[0])) rep_initial = AS_CSTRING(args[0]);
        if (nargs >= 2 && IS_STRING(args[1])) titre       = AS_CSTRING(args[1]);
    }

    char* filtre_qt = fsel_translate_filter(filtre_wl);
    char* result = owl_ui_file_dialog(rep_initial, fichier_defaut, titre,
                                      filtre_qt, mode);
    free(filtre_qt);
    if (!result) return OBJ_VAL(obj_copy_string("", 0));
    Value v = OBJ_VAL(obj_copy_string(result, (int)strlen(result)));
    free(result);
    return v;
}

/* fRepSelecteur(<RepDefaut>, <Titre> [, <Commentaires> [, <Racine>]])
 * Retour : chemin du repertoire, "" si annule. Le parametre Commentaires
 * n'a pas d'equivalent QFileDialog (ignore). */
static Value fn_frepselecteur(int argCount, Value* args) {
    if (vm.headless_mode) return OBJ_VAL(obj_copy_string("", 0));

    const char* rep_defaut = "";
    const char* titre      = "";
    const char* racine     = "";

    if (argCount >= 1 && IS_STRING(args[0])) rep_defaut = AS_CSTRING(args[0]);
    if (argCount >= 2 && IS_STRING(args[1])) titre      = AS_CSTRING(args[1]);
    /* args[2] = Commentaires : ignore (pas d'equivalent Qt). */
    if (argCount >= 4 && IS_STRING(args[3])) racine     = AS_CSTRING(args[3]);

    char* result = owl_ui_dir_dialog(rep_defaut, titre, racine);
    if (!result) return OBJ_VAL(obj_copy_string("", 0));
    Value v = OBJ_VAL(obj_copy_string(result, (int)strlen(result)));
    free(result);
    return v;
}

/*  : TestModeHeadless(actif) — getter (arite 0) / setter (arite 1).
 * Active = Vrai : les dialogs modales et Ouvre() retournent un fallback
 * silencieux (NIL ou default_btn) au lieu d'instancier du Qt. Active = Faux :
 * UI reelle, popups visibles. En owl test, le runner met le mode a Vrai au
 * demarrage de chaque fichier (le dev peut le toggler localement). En owl
 * standard (GUI), defaut Faux. */
static Value fn_test_mode_headless(int argCount, Value* args) {
    if (argCount == 0) {
        return BOOL_VAL(vm.headless_mode);
    }
    /* Setter : un seul arg, booleen attendu. */
    if (!IS_BOOL(args[0])) {
        fprintf(stderr, "[TestModeHeadless] Argument 1 doit etre un booleen (Vrai/Faux).\n");
        return NIL_VAL;
    }
    vm.headless_mode = AS_BOOL(args[0]);
    return NIL_VAL;
}

/* ====================================================================
 *  Simulation d'actions utilisateur ( — famille TestSimule*)
 *
 *  Divergence canon Pilote* assumee (cf docs/ADR/ADR_NNNN-test-simule-*).
 *  Strategie : on declenche DIRECTEMENT le callback OWL associe au champ,
 *  pas un event Qt poste hardware. Marche en headless ET en GUI.
 * ==================================================================== */

/* Cherche le OwlControlDecl correspondant a un nom de controle, en
 * parcourant toutes les fenetres enregistrees dans vm.windows. Retourne
 * NULL si le nom n'est pas declare. */
static const OwlControlDecl* find_control_decl(const char* ctrlName) {
    if (!ctrlName) return NULL;
    for (int i = 0; i < vm.windowCount; i++) {
        const OwlWindowDecl* w = vm.windows[i];
        if (!w) continue;
        const OwlControlDecl* c = owl_window_decl_find_control(w, ctrlName);
        if (c) return c;
    }
    return NULL;
}

/*  — variante qui resout aussi un ObjUiControlRef vers son nom
 * brut tel qu'il apparait dans la decl. Retourne NULL si v n'est ni un
 * ObjUiControl ni un ObjUiControlRef. Utilise par les natives Test* qui
 * ne se servent que du nom (find_control_decl scan toutes les decls). */
static const char* resolve_ui_control_name_arg(Value v) {
    if (IS_UI_CONTROL(v)) return AS_UI_CONTROL(v)->name;
    if (IS_UI_CONTROL_REF(v)) {
        ObjUiControlRef* r = AS_UI_CONTROL_REF(v);
        return (r->name) ? r->name->chars : NULL;
    }
    return NULL;
}

/* Source d'un callback resolu : event catalog (slots auto MaFenetre/
 * MonChamp) ou user_proc (arity declaree par l'utilisateur). Distingue
 * pour le dispatch (call_event_block vs appel direct). */
typedef enum { CB_SOURCE_NONE, CB_SOURCE_EVENT, CB_SOURCE_USER_PROC } CallbackSource;

/* Cherche une PROCEDURE par nom (case-insensitive) dans deux tableaux
 * d'OwlEventBlock : d'abord les events canon (catalog), puis les
 * user_procs custom. Retourne la fonction OWL + sa source, ou NULL. */
static ObjFunction* find_callback_fn(const OwlControlDecl* c,
                                      const char* nameFR, const char* nameEN,
                                      CallbackSource* outSource) {
    const OwlEventBlock* e = find_event(c->events, c->event_count, nameFR, nameEN);
    if (e) { if (outSource) *outSource = CB_SOURCE_EVENT; return e->impl; }
    e = find_event(c->user_procs, c->user_proc_count, nameFR, nameEN);
    if (e) { if (outSource) *outSource = CB_SOURCE_USER_PROC; return e->impl; }
    if (outSource) *outSource = CB_SOURCE_NONE;
    return NULL;
}

/* Appelle un callback resolu en respectant sa source : pour un event
 * catalog on push les slots auto MaFenetre/MonChamp via call_event_block ;
 * pour un user_proc on appelle directement (l'arity correspond exactement
 * aux params declares par l'utilisateur, typiquement 0).
 *
 *  — variante avec `senderValue` : push/pop event_sender autour de
 * l'appel pour que MoiMême resolve dans le callback (et dans les procedures
 * appelees en cascade via l'heritage chain). */
static void invoke_callback_with_sender(ObjFunction* fn, CallbackSource src,
                                        Value senderValue) {
    if (!fn) return;
    if (src == CB_SOURCE_EVENT) {
        call_event_block_with_sender(fn, false, senderValue);
        return;
    }
    /* user_proc : appel direct, sans MaFenetre/MonChamp synthetiques. */
    vm_event_sender_push(senderValue);
    vm_push(OBJ_VAL(fn));
    vm_call_reentrant(OBJ_VAL(fn), 0);
    vm_pop();
    vm_event_sender_pop();
}

static void invoke_callback(ObjFunction* fn, CallbackSource src) {
    invoke_callback_with_sender(fn, src, NIL_VAL);
}

/* Appelle un callback en transmettant un payload de N valeurs entieres,
 * remplit les slots auto MaFenetre/MonChamp pour un event catalog (calque
 * exact de dispatch_image_event_n). Pour un user_proc on appelle direct.
 * Si arity < 2 + payloadCount, les valeurs du payload en surplus sont
 * silencieusement ignorees ; si arity > 2 + payloadCount, les slots
 * supplementaires ne sont pas pousses (le compilo l'aura deja refuse
 * en amont, mais on reste defensif comme dispatch_image_event_n). */
static void invoke_callback_payload_with_sender(ObjFunction* fn, CallbackSource src,
                                                const Value* payload, int payloadCount,
                                                Value senderValue) {
    if (!fn) return;
    vm_event_sender_push(senderValue);  /*  — MoiMême chain */
    if (src == CB_SOURCE_EVENT) {
        int arity = fn->arity;
        vm_push(OBJ_VAL(fn));
        if (arity >= 1) vm_push(NIL_VAL);   /* MaFenetre */
        if (arity >= 2) vm_push(NIL_VAL);   /* MonChamp */
        int dataSlots = arity - 2;
        if (dataSlots < 0) dataSlots = 0;
        if (dataSlots > payloadCount) dataSlots = payloadCount;
        for (int i = 0; i < dataSlots; i++) vm_push(payload[i]);
        vm_call_reentrant(OBJ_VAL(fn), arity);
        vm_pop();
        vm_event_sender_pop();
        return;
    }
    /* user_proc : appel direct, params = exactement ce que le dev a declare.
     * On push min(arity, payloadCount) valeurs du payload. */
    int arity = fn->arity;
    int dataSlots = arity;
    if (dataSlots > payloadCount) dataSlots = payloadCount;
    vm_push(OBJ_VAL(fn));
    for (int i = 0; i < dataSlots; i++) vm_push(payload[i]);
    vm_call_reentrant(OBJ_VAL(fn), arity);
    vm_pop();
    vm_event_sender_pop();
}

/* TestSimuleSouris : 2 surcharges.
 *  - (champ, action)            : actions sans coords (Clic/DoubleClic G/D).
 *  - (champ, action, x, y)      : actions a coords (BoutonG/D, SurvolSouris)
 *                                 et molette (MoletteHaut/MoletteBas, ou
 *                                 mouvement = +1/-1 est synthetise).
 * Si la PROCEDURE n'est pas declaree sur le champ : no-op silencieux. */
static Value fn_test_simule_souris(int argCount, Value* args) {
    if (argCount != 2 && argCount != 4) {
        fprintf(stderr, "[TestSimuleSouris] Attendu 2 ou 4 arguments (champ, action [, x, y]).\n");
        return NIL_VAL;
    }
    const char* ctrlName = resolve_ui_control_name_arg(args[0]);
    if (!ctrlName) {
        fprintf(stderr, "[TestSimuleSouris] Argument 1 doit etre un champ UI.\n");
        return NIL_VAL;
    }
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[TestSimuleSouris] Argument 2 doit etre une valeur d'ActionSouris.\n");
        return NIL_VAL;
    }
    int64_t action = AS_INTEGER(args[1]);

    /* Coords pour la surcharge 4-args. */
    int64_t x = 0, y = 0;
    if (argCount == 4) {
        if (!IS_INTEGER(args[2]) || !IS_INTEGER(args[3])) {
            fprintf(stderr, "[TestSimuleSouris] Arguments 3 et 4 (x, y) doivent etre des entiers.\n");
            return NIL_VAL;
        }
        x = AS_INTEGER(args[2]);
        y = AS_INTEGER(args[3]);
    }

    /* Determine le nom canon du callback (event catalog : surClic /
     * surBoutonGauche / ...) et le payload selon l'action. */
    const char* nameFR1 = NULL; const char* nameEN1 = NULL;  /* canon catalog */
    Value payload[3];
    int payloadCount = 0;
    bool needCoords = false;  /* action attend la surcharge 4-args */

    if (action == OWL_ACTION_SOURIS_CLIC_G || action == OWL_ACTION_SOURIS_CLIC_D) {
        nameFR1 = "surClic";       nameEN1 = "onClick";
    } else if (action == OWL_ACTION_SOURIS_DCLIC_G || action == OWL_ACTION_SOURIS_DCLIC_D) {
        nameFR1 = "surDoubleClic"; nameEN1 = "onDoubleClick";
    } else if (action == OWL_ACTION_SOURIS_BTN_G) {
        nameFR1 = "surBoutonGauche"; nameEN1 = "onLeftButtonDown";
        needCoords = true;
        payload[0] = INTEGER_VAL(x); payload[1] = INTEGER_VAL(y); payloadCount = 2;
    } else if (action == OWL_ACTION_SOURIS_BTN_D) {
        nameFR1 = "surBoutonDroit";  nameEN1 = "onRightButtonDown";
        needCoords = true;
        payload[0] = INTEGER_VAL(x); payload[1] = INTEGER_VAL(y); payloadCount = 2;
    } else if (action == OWL_ACTION_SOURIS_SURVOL) {
        nameFR1 = "surSurvol";       nameEN1 = "onMouseMove";
        needCoords = true;
        payload[0] = INTEGER_VAL(x); payload[1] = INTEGER_VAL(y); payloadCount = 2;
    } else if (action == OWL_ACTION_SOURIS_MOLETTE_H
            || action == OWL_ACTION_SOURIS_MOLETTE_B) {
        nameFR1 = "surMolette";      nameEN1 = "onWheel";
        needCoords = true;
        int mvt = (action == OWL_ACTION_SOURIS_MOLETTE_H) ? +1 : -1;
        payload[0] = INTEGER_VAL(mvt);
        payload[1] = INTEGER_VAL(x);
        payload[2] = INTEGER_VAL(y);
        payloadCount = 3;
    } else {
        fprintf(stderr, "[TestSimuleSouris] Valeur ActionSouris inconnue (%lld).\n",
                (long long)action);
        return NIL_VAL;
    }

    if (needCoords && argCount != 4) {
        fprintf(stderr, "[TestSimuleSouris] Cette action exige 4 arguments (champ, action, x, y).\n");
        return NIL_VAL;
    }
    if (!needCoords && argCount != 2) {
        fprintf(stderr, "[TestSimuleSouris] Cette action exige 2 arguments (champ, action).\n");
        return NIL_VAL;
    }

    const OwlControlDecl* decl = find_control_decl(ctrlName);
    if (!decl) return NIL_VAL;  /* controle non declare dans une fenetre : no-op */

    /*  — mode reel (widget Qt monte) pour le clic simple : on poste
     * un VRAI cycle press+release souris sur le widget. Un vrai clic vole le
     * focus clavier (comme un humain), ce qui rend testable le scenario
     *  (DonneFocus dans surClic doit reposer le focus). Le handler
     * bound (surClic via le signal `clicked`) tire seul a la pompe : pas
     * d'invoke manuel. Reserve aux actions sans coords ni payload ; les
     * variantes a payload (surBoutonGauche x,y, molette) gardent l'invoke
     * direct (le payload n'est pas reproductible par un simple QMouseEvent).
     *  — un double-clic exige la sequence complete (dbl) : sans elle,
     * Qt ne synthetise aucun MouseButtonDblClick sur un event poste et le
     * double-clic se degradait en simple clic. */
    if (!needCoords && payloadCount == 0) {
        ObjUiControlInstance* uici = current_window_uici_by_name(ctrlName);
        if (uici != NULL && uici->widget != NULL) {
            bool isDouble = (action == OWL_ACTION_SOURIS_DCLIC_G ||
                             action == OWL_ACTION_SOURIS_DCLIC_D);
            int button = (action == OWL_ACTION_SOURIS_CLIC_D ||
                          action == OWL_ACTION_SOURIS_DCLIC_D) ? 1 : 0;
            owl_ui_post_mouse_click((OwlControl*)uici->widget, button, -1, -1,
                                    isDouble ? 1 : 0);
            owl_ui_pump_events(200);
            return NIL_VAL;
        }
    }

    CallbackSource src = CB_SOURCE_NONE;
    ObjFunction* fn = find_callback_fn(decl, nameFR1, nameEN1, &src);
    if (!fn) return NIL_VAL;     /* no-op silencieux si PROCEDURE absente */

    /*  — resout le widget Value pour MoiMême depuis vm.globals. */
    Value sender = moimeme_widget_value_for_name(decl->name);
    if (payloadCount == 0) {
        invoke_callback_with_sender(fn, src, sender);
    } else {
        invoke_callback_payload_with_sender(fn, src, payload, payloadCount, sender);
    }
    return NIL_VAL;
}

/* DeclencheEvenement(cible, evtX[, params...]) — .
 *
 * Declenche par CODE le handler d'un evenement d'un champ, sans passer par
 * une interaction utilisateur. Calque strict du chemin TestSimule* : on
 * appelle directement le callback OWL associe -> marche en headless comme
 * en GUI.
 *   - cible : le champ en objet (ObjUiControl/Ref) OU son nom en chaine
 *     (indirection resolue au runtime).
 *   - evtX  : constante evt* (valeur OWL_EVT_BASE + index dedup). Le compilo
 *     a deja valide l'arite/type des params et l'appartenance de l'event au
 *     widget (forme objet / chaine litterale). Ici on reste defensif.
 *   - params : transmis au handler dans l'ordre de la signature de l'event.
 */
static Value fn_declenche_evenement(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[DeclencheEvenement] Attendu au moins 2 arguments "
                        "(cible, evt).\n");
        return NIL_VAL;
    }

    /* Cible : objet UI (ObjUiControl/Ref) ou nom de champ en chaine. */
    const char* ctrlName = resolve_ui_control_name_arg(args[0]);
    if (ctrlName == NULL && IS_STRING(args[0])) {
        ctrlName = AS_CSTRING(args[0]);
    }
    if (ctrlName == NULL || ctrlName[0] == '\0') {
        fprintf(stderr, "[DeclencheEvenement] Argument 1 doit etre un champ "
                        "ou un nom de champ.\n");
        return NIL_VAL;
    }

    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[DeclencheEvenement] Argument 2 doit etre une "
                        "constante evt*.\n");
        return NIL_VAL;
    }
    int idx = (int)(AS_INTEGER(args[1]) - (int64_t)OWL_EVT_BASE);
    const OwlEventDef* ev = owl_widget_event_dedup_at(idx);
    if (ev == NULL) {
        fprintf(stderr, "[DeclencheEvenement] Argument 2 : constante evt* "
                        "inconnue.\n");
        return NIL_VAL;
    }

    const OwlControlDecl* decl = find_control_decl(ctrlName);
    if (!decl) return NIL_VAL;  /* champ non declare (indirection) : no-op */

    CallbackSource src = CB_SOURCE_NONE;
    ObjFunction* fn = find_callback_fn(decl, ev->canonical, ev->nameEN, &src);
    if (!fn) return NIL_VAL;    /* handler absent : no-op silencieux */

    Value sender = moimeme_widget_value_for_name(decl->name);

    int payloadCount = argCount - 2;
    if (payloadCount <= 0) {
        invoke_callback_with_sender(fn, src, sender);
    } else {
        Value payload[8];
        if (payloadCount > 8) payloadCount = 8;
        for (int i = 0; i < payloadCount; i++) payload[i] = args[2 + i];
        invoke_callback_payload_with_sender(fn, src, payload, payloadCount, sender);
    }
    return NIL_VAL;
}

/* TestSimuleSaisie(champ, texte) — ecrit `texte` dans `champ..Texte` et
 * declenche le callback `Modification` du champ. Si la PROCEDURE n'est
 * pas declaree : la valeur est ecrite quand meme mais aucun callback
 * appele (no-op silencieux cote event). */
static Value fn_test_simule_saisie(int argCount, Value* args) {
    if (argCount != 2) {
        fprintf(stderr, "[TestSimuleSaisie] Attendu 2 arguments (champ, texte).\n");
        return NIL_VAL;
    }
    const char* ctrlName = resolve_ui_control_name_arg(args[0]);
    if (!ctrlName) {
        fprintf(stderr, "[TestSimuleSaisie] Argument 1 doit etre un champ UI.\n");
        return NIL_VAL;
    }
    if (!IS_STRING(args[1])) {
        fprintf(stderr, "[TestSimuleSaisie] Argument 2 doit etre une chaine.\n");
        return NIL_VAL;
    }

    /* Ecrit la valeur dans ..Valeur (prop de contenu canonique). 
     * modele C : on passe par l'ObjUiControlInstance de la fenetre courante.
     * Si pas d'instance (TestSimuleSaisie hors Ouvre) : no-op store.
     *
     *  — deux chemins selon le mode :
     *   - widget Qt monte (TestModeHeadless(Faux)) : on poste de VRAIS
     *     QKeyEvents au QLineEdit, un par caractere, puis on pompe la boucle.
     *     Le pipeline reel s'exerce (textChanged -> surAChaqueModification a
     *     chaque frappe) et ..Valeur lit le widget. On NE double-
     *     ecrit PAS le store : Qt->textChanged synchronise deja, et le live-read
     *      fait foi tant que widget != NULL.
     *   - pas de widget (headless dominant) : chemin store + un seul invoke
     *     callback (granularite globale, suffisant pour la majorite des tests). */
    ObjWindowInstance* inst = vm_window_stack_top();
    ObjUiControlInstance* target = NULL;
    if (inst != NULL) {
        for (int i = 0; i < inst->control_count; i++) {
            ObjUiControlInstance* uici = inst->controls[i];
            if (!uici || !uici->decl || !uici->decl->name) continue;
            if (strcmp(uici->decl->name, ctrlName) == 0) {
                target = uici;
                break;
            }
        }
    }

    if (target != NULL && target->widget != NULL) {
        /* Mode reel : vrais QKeyEvents + pompe. Les callbacks Modification
         * sont cables au signal textChanged (build_window), donc chaque frappe
         * pompee tire surAChaqueModification. Pas d'invoke manuel ici.
         * On vide d'abord le champ : le primitif REMPLACE le contenu (symetrie
         * avec le mode headless + doc « ecrit un texte »), sans quoi les frappes
         * s'ajouteraient au curseur en fin de contenu (append). */
        owl_ui_control_set_text((OwlControl*)target->widget, "");
        owl_ui_post_text_input((OwlControl*)target->widget, AS_CSTRING(args[1]));
        owl_ui_pump_events(200);
        return NIL_VAL;
    }

    /* Mode headless : ecrit le store puis declenche Modification une fois. */
    if (target != NULL) {
        obj_ui_control_instance_headless_set(target, "Valeur", args[1]);
    }
    const OwlControlDecl* decl = find_control_decl(ctrlName);
    if (decl) {
        CallbackSource src = CB_SOURCE_NONE;
        ObjFunction* fn = find_callback_fn(decl, "surAChaqueModification",
                                           "onEachChange", &src);
        if (fn) invoke_callback(fn, src);
    }
    return NIL_VAL;
}

/*  — ToucheEnfoncee(code) / KeyPressed(code).
 * Polling de l'etat instantane d'une touche (conforme doc 3014007).
 *
 * En mode headless (owl test / TestModeHeadless(Vrai)) : aucune touche
 * physique enfoncee -> toujours Faux. C'est le comportement attendu d'un
 * test automatise. En mode reel, l'etat est tenu a jour par le filtre
 * clavier installe sur les widgets (cf owl_ui_install_key_filter). Un code
 * hors-domaine ou negatif n'est pas une erreur : retourne Faux. */
static Value fn_touche_enfoncee(int argCount, Value* args) {
    if (argCount != 1 || !IS_INTEGER(args[0])) {
        return BOOL_VAL(false);
    }
    if (vm.headless_mode) return BOOL_VAL(false);
    int code = (int)AS_INTEGER(args[0]);
    return BOOL_VAL(owl_ui_key_is_down(code) != 0);
}

/*  — invoque un event clavier de FENETRE (surToucheAppuyee/Relachee
 * declare au niveau fenetre) en headless, avec payload = code touche. Les
 * events fenetre n'ont qu'un slot synthetique (MaFenetre), pas de MonChamp. */
static void invoke_window_key_event(ObjWindowInstance* inst,
                                    const char* nameFR, const char* nameEN,
                                    int code) {
    if (!inst || !inst->decl) return;
    const OwlEventBlock* e = find_event(inst->decl->events, inst->decl->event_count,
                                        nameFR, nameEN);
    if (!e || !e->impl) return;
    Value sender = moimeme_widget_value_for_name(inst->decl->name);
    ObjFunction* fn = e->impl;
    Value payload[1] = { INTEGER_VAL(code) };
    vm_event_sender_push(sender);
    int arity = fn->arity;
    vm_push(OBJ_VAL(fn));
    if (arity >= 1) vm_push(NIL_VAL);   /* MaFenetre (pas de MonChamp cote fenetre) */
    if (arity >= 2) vm_push(payload[0]);
    vm_call_reentrant(OBJ_VAL(fn), arity);
    vm_pop();
    vm_event_sender_pop();
}

/*  — dispatch headless d'une touche : modele de propagation
 * « champ d'abord, fenetre si le champ n'a pas de handler ».
 *   - champ focuse AVEC handler surTouche* -> le champ tire (consomme).
 *   - champ focuse SANS handler (ou aucun focus) -> la fenetre tire. */
static void envoie_touche_dispatch_headless(ObjWindowInstance* inst,
                                            const char* nameFR, const char* nameEN,
                                            int code) {
    if (!inst) return;
    int fi = inst->focused_index;
    if (fi >= 0 && fi < inst->control_count) {
        ObjUiControlInstance* uici = inst->controls[fi];
        if (uici && uici->decl) {
            CallbackSource src = CB_SOURCE_NONE;
            ObjFunction* fn = find_callback_fn(uici->decl, nameFR, nameEN, &src);
            if (fn) {
                Value sender = moimeme_widget_value_for_name(uici->decl->name);
                Value payload[1] = { INTEGER_VAL(code) };
                invoke_callback_payload_with_sender(fn, src, payload, 1, sender);
                return;   /* champ avec handler : consomme, pas de remontee */
            }
        }
    }
    /* Pas de champ focuse avec handler : remonte a la fenetre. */
    invoke_window_key_event(inst, nameFR, nameEN, code);
}

/* EnvoieTouche(code) / SendKey(code) — injecte une frappe COMPLETE (appui +
 * relachement, comme un humain) vers la fenetre active.
 *
 * Mode reel (fenetre montee) : poste un cycle QKeyEvent press+release au
 * widget focuse + pompe la boucle, de sorte que le filtre clavier dispatche
 * surToucheAppuyee puis surToucheRelachee via le vrai pipeline Qt.
 * Mode headless : dispatch direct des handlers (appuyee puis relachee) selon
 * le modele de propagation champ -> fenetre.
 *
 * Sans fenetre ouverte : no-op silencieux (pas d'ErreurDetectee). */
static Value fn_envoie_touche(int argCount, Value* args) {
    if (argCount != 1 || !IS_INTEGER(args[0])) {
        return NIL_VAL;
    }
    int code = (int)AS_INTEGER(args[0]);
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL) return NIL_VAL;   /* aucune fenetre : no-op */

    if (inst->widget != NULL) {
        /* Mode reel : vrais QKeyEvents press+release + pompe. */
        owl_ui_post_key((OwlWindow*)inst->widget, code);
        owl_ui_pump_events(200);
        return NIL_VAL;
    }

    /* Mode headless : dispatch direct appuyee puis relachee. */
    envoie_touche_dispatch_headless(inst,
        "surToucheAppuy\xc3\xa9""e", "onKeyDown", code);
    envoie_touche_dispatch_headless(inst,
        "surToucheRel\xc3\xa2""ch\xc3\xa9""e", "onKeyUp", code);
    return NIL_VAL;
}

/* ====================================================================
 *  DonneFocus / SetFocus + focus initial automatique
 * ==================================================================== */

/*  — trace opt-in du chemin de focus (OWL_TRACE_UI posee). Permanent :
 * le contexte clic (dispatch g_bindings) diverge du contexte tick (appel direct
 * vm_call_reentrant) et seul le trace prouve quelle branche de ui_move_focus_to
 * est prise quand un DonneFocus depuis surClic ne repose pas le focus. Cache le
 * getenv une fois (pas de surcout par mouvement de focus). */
static bool ui_trace_focus_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = (getenv("OWL_TRACE_UI") != NULL) ? 1 : 0;
    return cached == 1;
}

/*  — un champ est focusable (saisissable) si son kind est INPUT
 * (Saisie). V1 : seul ce kind capte le focus initial automatique et est une
 * cible legitime de DonneFocus. */
static bool ui_control_is_focusable(const OwlControlDecl* c) {
    return c != NULL && c->kind == OWL_CONTROL_INPUT;
}

/* Declenche un event focus (surEntree / surSortie) sur le champ d'indice
 * `idx` de l'instance. Pousse le sender pour que MoiMeme resolve au champ.
 * No-op si idx hors-borne ou si l'event n'est pas declare. */
static void ui_fire_focus_event(ObjWindowInstance* inst, int idx,
                                const char* nameFR, const char* nameEN) {
    if (!inst || idx < 0 || idx >= inst->control_count) return;
    ObjUiControlInstance* uici = inst->controls[idx];
    if (!uici || !uici->decl || !uici->decl->name) return;
    CallbackSource src = CB_SOURCE_NONE;
    ObjFunction* fn = find_callback_fn(uici->decl, nameFR, nameEN, &src);
    if (!fn) return;
    Value sender = moimeme_widget_value_for_name(uici->decl->name);
    invoke_callback_with_sender(fn, src, sender);
}

/* Deplace le focus de l'instance vers le champ d'indice `idx`. Si idx est deja
 * le champ courant : AUCUN evenement (canon : « aucun code execute
 * si le champ est le champ en cours »).
 *
 * Emission des events surSortie/surEntree (, Option A) :
 *   - GUI : aucun event emis ici ; le setFocus Qt reel declenche FocusOut/
 *     FocusIn que l'eventFilter relaie (source unique, pas de double emission).
 *   - Headless : events emis synchrones ici (aucun eventFilter Qt). */
static void ui_move_focus_to(ObjWindowInstance* inst, int idx) {
    if (!inst || idx < 0 || idx >= inst->control_count) return;
    ObjUiControlInstance* uici = inst->controls[idx];

    if (ui_trace_focus_enabled()) {
        const char* nm = (uici && uici->decl && uici->decl->name)
                         ? uici->decl->name : "?";
        fprintf(stderr,
                "[ui-focus] move_to idx=%d name=%s widget=%p focused_index=%d "
                "branch=%s\n",
                idx, nm, (uici ? (void*)uici->widget : NULL),
                inst->focused_index,
                (inst->focused_index == idx) ? "reapply-same" : "switch");
        fflush(stderr);
    }

    if (inst->focused_index == idx) {
        /* Champ deja courant dans le modele logique OWL : AUCUN evenement
         * (canon « aucun code execute si le champ est le champ en
         * cours »). MAIS en GUI le focus clavier reel de Qt a pu deriver hors
         * de ce champ sans que le modele OWL le sache — typiquement un clic sur
         * un Bouton qui vole le focus Qt : OWL ne suit pas les focus Qt non
         * sollicites, donc focused_index pointe toujours le champ saisissable.
         * Un DonneFocus(ce_champ) depuis le surClic doit alors REPOSER le focus
         * Qt reel (sinon le clavier reste capte par le bouton). On reapplique
         * donc le focus physique, sans rejouer surSortie/surEntree. */
        if (uici && uici->widget != NULL) {
            owl_ui_control_set_focus((OwlControl*)uici->widget);
        }
        return;
    }

    int prev = inst->focused_index;
    inst->focused_index = idx;

    /*  — source UNIQUE des events focus (Option A) :
     *   - HEADLESS (widget == NULL, pas d'eventFilter Qt) : on emet surSortie/
     *     surEntree synchrones ici, sinon plus aucun event focus en headless.
     *   - GUI (widget != NULL) : on N'EMET PAS ici. Le focus Qt reel pose
     *     ci-dessous (setFocus differe) declenche FocusOut(prev)+FocusIn(idx),
     *     que l'eventFilter d'OwlEventDispatcher relaie en surSortie/surEntree.
     *     Emettre aussi ici donnerait une DOUBLE emission (sync + Qt FocusIn).
     *
     * Garantie « 1 focus = 1 event » en GUI : chaque transition de focus passe
     * par un setFocus sur une fenetre ACTIVE (owl_ui_window_show active a
     * l'ouverture ; owl_ui_post_mouse_click active au clic), donc Qt produit
     * toujours le FocusIn correspondant -> surEntree tire exactement une fois. */
    if (uici && uici->widget != NULL) {
        owl_ui_control_set_focus((OwlControl*)uici->widget);
    } else {
        if (prev >= 0) ui_fire_focus_event(inst, prev, "surSortie", "onExit");
        ui_fire_focus_event(inst, idx, "surEntr\xc3\xa9""e", "onEntry");
    }
}

/*  — focus initial automatique : a l'ouverture, le premier champ
 * saisissable (ordre lexical) recoit le focus. Aucun effet si la fenetre n'a
 * aucun champ saisissable. Appele apres FinInitialisation (GUI et headless). */
static void ui_apply_initial_focus(ObjWindowInstance* inst) {
    if (!inst) return;
    /* Un focus a deja ete pose explicitement (typiquement DonneFocus depuis
     * FinInitialisation) : l'auto-focus ne doit pas l'ecraser. Couvre les
     * deux branches (GUI et headless) car focused_index est mute dans les
     * deux cas par ui_move_focus_to. */
    if (inst->focused_index >= 0) return;
    for (int i = 0; i < inst->control_count; i++) {
        ObjUiControlInstance* uici = inst->controls[i];
        if (uici && ui_control_is_focusable(uici->decl)) {
            ui_move_focus_to(inst, i);
            return;
        }
    }
}

/*  — synchro de focused_index avec le focus clavier Qt REEL.
 *
 * Appele par le backend (QApplication::focusChanged) a chaque changement de
 * focus, avec le widget `now` qui vient de prendre le focus. Met focused_index
 * de la fenetre courante a jour par SET DIRECT — JAMAIS via ui_move_focus_to
 * (qui rejouerait surSortie/surEntree, alors que ces events sont deja geres par
 * l'eventFilter FocusIn cote backend : double emission garantie sinon).
 *
 * Sens large (decision dev) : focused_index suit TOUS les widgets focusables
 * (Bouton, Libelle, Saisie...), pas seulement les Saisies. On rattache `now`
 * au controle proprietaire (lui-meme ou un sous-widget via owl_ui_control_owns_widget).
 *
 * Gardes (jamais corrompre focused_index) :
 *   - now == NULL (plus aucun widget focuse, ex. fenetre desactivee) : inchange.
 *   - aucune fenetre OWL courante : inchange.
 *   - `now` hors de la fenetre courante / non rattachable a un controle : inchange.
 *
 * Reentrance : le set direct est idempotent. focusChanged peut etre emis pendant
 * un setFocus programmatique (DonneFocus) ; on re-pose alors le meme index, sans
 * rejouer d'event ni declencher de nouveau mouvement -> pas de boucle. */
static void ui_focus_sync_from_qt(OwlControl* now) {
    if (now == NULL) return;
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL) return;
    for (int i = 0; i < inst->control_count; i++) {
        ObjUiControlInstance* uici = inst->controls[i];
        if (!uici || uici->widget == NULL) continue;
        if (owl_ui_control_owns_widget((OwlControl*)uici->widget, now)) {
            inst->focused_index = i;   /* SET DIRECT : etat logique uniquement */
            return;
        }
    }
    /* `now` n'appartient a aucun controle de la fenetre courante : on laisse
     * focused_index inchange (ne pas le passer a -1 : un focus pose sur un
     * widget hors-modele — barre de titre, popup systeme — ne doit pas effacer
     * le champ courant OWL). */
}

/* Cherche l'indice d'un champ par nom (case-insensitive) dans les controles
 * de l'instance. Retourne -1 si absent. */
static int ui_find_control_index(ObjWindowInstance* inst, const char* name) {
    if (!inst || !name) return -1;
    for (int i = 0; i < inst->control_count; i++) {
        ObjUiControlInstance* uici = inst->controls[i];
        if (uici && uici->decl && uici->decl->name
            && strcasecmp(uici->decl->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* DonneFocus([champ]) / SetFocus([champ]) — donne le focus clavier a un champ.
 *
 * Formes :
 *   - DonneFocus(SAI_X)            : reference de champ locale.
 *   - DonneFocus(FEN_X.SAI_Y)      : forme qualifiee (resolution ).
 *   - DonneFocus("SAI_X")          : nom de champ en chaine.
 *   - DonneFocus()                 : focus au champ en cours (no-op si deja).
 *
 * Declenche surSortie sur le champ quitte puis surEntree sur le champ vise.
 * Champ inexistant -> soft error E_RT_UI_FOCUS_FIELD_UNKNOWN (ErreurDetectee).
 * Aucune fenetre ouverte -> no-op silencieux. */
static Value fn_donne_focus(int argCount, Value* args) {
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL) return NIL_VAL;

    /* Forme sans argument : focus au champ courant (no-op si deja courant). */
    if (argCount == 0) {
        if (inst->focused_index >= 0) {
            ui_move_focus_to(inst, inst->focused_index);
        }
        return NIL_VAL;
    }

    /* Resolution du nom cible : reference de champ (locale ou qualifiee) ou
     * chaine. Meme resolution que les natives Test*. */
    const char* ctrlName = resolve_ui_control_name_arg(args[0]);
    if (!ctrlName && IS_STRING(args[0])) ctrlName = AS_CSTRING(args[0]);
    if (!ctrlName) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "DonneFocus", NULL);
        return NIL_VAL;
    }

    int idx = ui_find_control_index(inst, ctrlName);
    if (ui_trace_focus_enabled()) {
        fprintf(stderr, "[ui-focus] DonneFocus resolve name=%s -> idx=%d\n",
                ctrlName, idx);
        fflush(stderr);
    }
    if (idx < 0) {
        owl_emit_native_error(E_RT_UI_FOCUS_FIELD_UNKNOWN, "DonneFocus",
                              "name", ctrlName, NULL);
        return NIL_VAL;
    }
    ui_move_focus_to(inst, idx);
    /*  — meme raison que la pompe d'ouverture : en GUI, le
     * setFocus est DIFFERE (QTimer 0) et surSortie/surEntree ne naissent que du
     * FocusOut/FocusIn Qt. Sans pompe, la ligne suivante du programme observe un
     * focus qui n'a pas encore bouge — alors qu'en headless les deux events sont
     * deja tires. On pompe donc pour que DonneFocus rende la main focus pose,
     * events emis, dans les deux modes. */
    if (inst->widget != NULL) owl_ui_pump_events(50);
    return NIL_VAL;
}

/*  — __DebugDernierMessage() : dernier message compose par une boite
 * de message, quel que soit le mode. Test-only (masque LSP via
 * OWL_NAT_INTERNAL) : la boite est court-circuitee sous `owl test`, donc son
 * contenu ne serait autrement observable par aucune assertion — c'est ce trou
 * qui a laisse passer la perte silencieuse des arguments non-chaine. Renvoie
 * "" si aucune boite n'a encore ete composee. */
static Value fn_debug_dernier_message(int argCount, Value* args) {
    (void)argCount; (void)args;
    const char* msg = g_dernier_message_boite ? g_dernier_message_boite : "";
    return OBJ_VAL(obj_copy_string(msg, (int)strlen(msg)));
}

/*  — __DebugFeuilleStyle(champ) : renvoie la feuille de style QSS
 * courante du widget (contenu passe a setStyleSheet). Test-only (masque LSP
 * via OWL_NAT_INTERNAL) : verifie l'emission des fragments d'etat :hover/:focus
 * sans capture-pixel (aucun moyen headless de forcer le hover reel). Renvoie
 * "" si le champ est introuvable ou non monte. */
static Value fn_debug_feuille_style(int argCount, Value* args) {
    if (argCount < 1) return OBJ_VAL(obj_copy_string("", 0));
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL) return OBJ_VAL(obj_copy_string("", 0));
    const char* ctrlName = resolve_ui_control_name_arg(args[0]);
    if (!ctrlName && IS_STRING(args[0])) ctrlName = AS_CSTRING(args[0]);
    if (!ctrlName) return OBJ_VAL(obj_copy_string("", 0));
    int idx = ui_find_control_index(inst, ctrlName);
    if (idx < 0 || idx >= inst->control_count) return OBJ_VAL(obj_copy_string("", 0));
    ObjUiControlInstance* uici = inst->controls[idx];
    if (!uici || !uici->widget) return OBJ_VAL(obj_copy_string("", 0));
    const char* qss = owl_ui_control_get_stylesheet((OwlControl*)uici->widget);
    if (!qss) qss = "";
    return OBJ_VAL(obj_copy_string(qss, (int)strlen(qss)));
}

/*  — __DebugFeuilleStyleMenu() : renvoie la feuille de style QSS de la
 * barre de menu de la fenetre courante (contenu passe a setStyleSheet).
 * Test-only (masque LSP via OWL_NAT_INTERNAL) : verifie l'emission des couleurs
 * du bloc Style menu (alpha inclus, rgba) sans capture-pixel. Renvoie "" si
 * aucune fenetre courante ou barre non montee. */
static Value fn_debug_feuille_style_menu(int argCount, Value* args) {
    (void)argCount; (void)args;
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL || inst->widget == NULL) return OBJ_VAL(obj_copy_string("", 0));
    const char* qss = owl_ui_window_menubar_get_stylesheet((OwlWindow*)inst->widget);
    if (!qss) qss = "";
    return OBJ_VAL(obj_copy_string(qss, (int)strlen(qss)));
}

/*  — __DebugFeuilleStyleMenuContextuel(nom) : feuille QSS que le panneau
 * du menu contextuel `nom` de la fenetre courante recevrait au clic droit.
 * Test-only. Le popup reel est modal (exec) et l'arbre est detruit en sortie :
 * aucun test ne peut l'observer. On rejoue donc EXACTEMENT le calcul de
 * dispatch_context_menu (menu_resolve_style + composition de la feuille) sans
 * creer de widget. "" si aucune fenetre courante ou nom inconnu. */
static Value fn_debug_feuille_style_menu_ctx(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0]))
        return OBJ_VAL(obj_copy_string("", 0));
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL || inst->decl == NULL) return OBJ_VAL(obj_copy_string("", 0));
    const OwlMenuDecl* menu =
        owl_window_decl_find_menu(inst->decl, AS_CSTRING(args[0]));
    if (menu == NULL) return OBJ_VAL(obj_copy_string("", 0));
    OwlMenuStyleResolved style;
    menu_resolve_style(menu, &style);
    const char* qss = owl_ui_menu_stylesheet_preview(&style);
    if (!qss) qss = "";
    return OBJ_VAL(obj_copy_string(qss, (int)strlen(qss)));
}

/*  — __DebugPoliceBarreMenu() : descripteur de la police effective de la
 * barre de menu de la fenetre courante (test-only). Verifie que setFont a bien
 * pose la police du bloc Style (Qt n'honore pas font-family en QSS sur les items
 * de menu). "" si aucune fenetre courante ou barre non montee. */
static Value fn_debug_police_barre_menu(int argCount, Value* args) {
    (void)argCount; (void)args;
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL || inst->widget == NULL) return OBJ_VAL(obj_copy_string("", 0));
    const char* d = owl_ui_window_menubar_get_font_desc((OwlWindow*)inst->widget);
    if (!d) d = "";
    return OBJ_VAL(obj_copy_string(d, (int)strlen(d)));
}

/*  — __DebugPoliceSousMenu(sousMenu) : descripteur de la police effective
 * du deroulant designe par le libelle de son sous-menu (test-only). "" si
 * fenetre/deroulant introuvable. */
static Value fn_debug_police_sousmenu(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL || inst->widget == NULL) return OBJ_VAL(obj_copy_string("", 0));
    const char* d = owl_ui_window_submenu_get_font_desc((OwlWindow*)inst->widget,
                                                        AS_CSTRING(args[0]));
    if (!d) d = "";
    return OBJ_VAL(obj_copy_string(d, (int)strlen(d)));
}

/*  — __DebugEncreBarreMenu() : hauteur d'encre (glyphes reellement
 * peints) du texte de la barre de menu, en pixels (test-only). Grab off-screen
 * -> mesure du rendu reel, la ou __DebugPoliceBarreMenu ne lit que le QFont pose.
 * Attrape la divergence descripteur/peinture (ex. un fragment QSS qui
 * neutraliserait le setFont). -1 si aucune fenetre/barre. */
static Value fn_debug_encre_barre_menu(int argCount, Value* args) {
    (void)argCount; (void)args;
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL || inst->widget == NULL) return INTEGER_VAL(-1);
    return INTEGER_VAL(owl_ui_window_menubar_ink_height((OwlWindow*)inst->widget));
}

/*  lot B — __DebugPositionImageBouton(champ) : renvoie l'entier
 * E_PositionImageBouton effectivement applique au QPushButton (property lue
 * par le QProxyStyle). Test-only (masque LSP via OWL_NAT_INTERNAL) : verifie le
 * placement icone/texte sans capture-pixel (non robuste en headless). Renvoie
 * posBtnImageGauche si le champ est introuvable ou non monte. */
static Value fn_debug_position_image_bouton(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(OWL_POSBTNIMG_GAUCHE);
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL) return INTEGER_VAL(OWL_POSBTNIMG_GAUCHE);
    const char* ctrlName = resolve_ui_control_name_arg(args[0]);
    if (!ctrlName && IS_STRING(args[0])) ctrlName = AS_CSTRING(args[0]);
    if (!ctrlName) return INTEGER_VAL(OWL_POSBTNIMG_GAUCHE);
    int idx = ui_find_control_index(inst, ctrlName);
    if (idx < 0 || idx >= inst->control_count) return INTEGER_VAL(OWL_POSBTNIMG_GAUCHE);
    ObjUiControlInstance* uici = inst->controls[idx];
    if (!uici || !uici->widget) return INTEGER_VAL(OWL_POSBTNIMG_GAUCHE);
    return INTEGER_VAL(owl_ui_button_get_position_image((OwlControl*)uici->widget));
}

/*  — __DebugBoutonIconeSurvol(champ) : Vrai si une icone de survol est
 * armee sur le bouton (event filter installe + icone survol stockee). Test-only
 * (masque LSP via OWL_NAT_INTERNAL) : le hover reel n'est pas postable en
 * offscreen, on verifie donc structurellement le cablage. Faux si le champ est
 * introuvable ou non monte. */
static Value fn_debug_bouton_icone_survol(int argCount, Value* args) {
    if (argCount < 1) return BOOL_VAL(false);
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL) return BOOL_VAL(false);
    const char* ctrlName = resolve_ui_control_name_arg(args[0]);
    if (!ctrlName && IS_STRING(args[0])) ctrlName = AS_CSTRING(args[0]);
    if (!ctrlName) return BOOL_VAL(false);
    int idx = ui_find_control_index(inst, ctrlName);
    if (idx < 0 || idx >= inst->control_count) return BOOL_VAL(false);
    ObjUiControlInstance* uici = inst->controls[idx];
    if (!uici || !uici->widget) return BOOL_VAL(false);
    return BOOL_VAL(owl_ui_button_has_hover_icon((OwlControl*)uici->widget) != 0);
}

/*  — __DebugPoliceChamp(champ) : descripteur de la police EFFECTIVE du
 * widget d'un champ (test-only, masque LSP via OWL_NAT_INTERNAL). Format
 * "famille|taille|gras|italique", miroir de __DebugPoliceBarreMenu. Verifie que
 * la police des lignes d'une Liste stylee est bien posee (setFont), la ou Qt
 * n'honore pas font-family/size en QSS sur QListWidget::item. "" si le champ est
 * introuvable ou non monte. */
static Value fn_debug_police_champ(int argCount, Value* args) {
    if (argCount < 1) return OBJ_VAL(obj_copy_string("", 0));
    ObjWindowInstance* inst = vm_window_stack_top();
    if (inst == NULL) return OBJ_VAL(obj_copy_string("", 0));
    const char* ctrlName = resolve_ui_control_name_arg(args[0]);
    if (!ctrlName && IS_STRING(args[0])) ctrlName = AS_CSTRING(args[0]);
    if (!ctrlName) return OBJ_VAL(obj_copy_string("", 0));
    int idx = ui_find_control_index(inst, ctrlName);
    if (idx < 0 || idx >= inst->control_count) return OBJ_VAL(obj_copy_string("", 0));
    ObjUiControlInstance* uici = inst->controls[idx];
    if (!uici || !uici->widget) return OBJ_VAL(obj_copy_string("", 0));
    OwlControl* w = (OwlControl*)uici->widget;
    /* get_font_name renvoie un pointeur vers un buffer statique cote Qt : on le
     * recopie avant d'appeler les autres getters. */
    char family[128];
    const char* fam = owl_ui_control_get_font_name(w);
    snprintf(family, sizeof(family), "%s", fam ? fam : "");
    int size   = owl_ui_control_get_font_size(w);
    int bold   = owl_ui_control_get_font_bold(w);
    int italic = owl_ui_control_get_font_italic(w);
    char desc[192];
    snprintf(desc, sizeof(desc), "%s|%d|%d|%d", family, size, bold, italic);
    return OBJ_VAL(obj_copy_string(desc, (int)strlen(desc)));
}

/* ====================================================================
 *  Ouverture / Fermeture de fenetres (Phase E.1)
 * ==================================================================== */

/* Resoud le nom de fenetre depuis args[0].
 *
 * Formes acceptees :
 *   - ObjString : `Ouvre("FEN_X")` litteral (compat utilisateur).
 *   - ObjWindowRef : `Ouvre(FEN_X)` ou FEN_X est resolu comme proxy global
 *     pose au compile-time par window_declaration ( lot B). Le
 *     name->chars est le nom strippe servant directement de cle de lookup
 *     dans vm.windows.
 *
 * Retourne NULL si l'argument ne designe pas une fenetre. */
static const char* window_name_from_arg(int argCount, Value* args) {
    if (argCount < 1) return NULL;
    if (IS_STRING(args[0])) return AS_CSTRING(args[0]);
    if (IS_WINDOW_REF(args[0])) {
        ObjWindowRef* wref = AS_WINDOW_REF(args[0]);
        if (wref->name) return wref->name->chars;
    }
    return NULL;
}

/*  lot 1 — seed du declaratif (bloc PROPRIETES) dans le headless_store
 * d'une ObjUiControlInstance, au montage en mode headless.
 *
 * Trou bouche : sans ce seed, les props posees en PROPRIETES (composees
 * Cadre/Police = ObjInstance, ET scalaires Libelle/X/...) ne sont jamais
 * recopiees dans le store par-instance -> lues a 0/vide en headless alors
 * qu'elles rendent en GUI (build_control lit decl->properties et applique a
 * Qt). On les seede ici pour converger : meme valeur lue en headless qu'en
 * GUI.
 *
 * Resolution prealable identique a build_window (resolve_dynamic_props) :
 * evalue les eval ObjFunction. Idempotent : si build_window l'a deja fait
 * (re-Ouvre), aucun effet de bord.
 *
 * Cle de store : on passe par obj_ui_control_instance_headless_set qui
 * normalise via headless_key_normalize_canon — exactement la cle que les
 * lectures GET utilisent (OP_STRUCT_GET sur ref).
 *
 * Homonymes : depuis , les cascades homonymes sont fusionnees au
 * compile-time en UNE SEULE prop ; le filtre "premiere occurrence" reste
 * defensif au cas ou plusieurs lignes du meme nom apparaitraient (overwrite
 * litteral simple : Titre = "A" ; Titre = "B"). */
/*  — le seed headless est scinde en deux phases pour respecter le
 * cycle de vie quand le top-level fenetre est intercale (cf.
 * open_window_instance) :
 *   - SEED_PHASE_LITERAL : seede les props LITTERALES (eval == NULL : Libelle
 *     = "depart", X = 10...). Posee AVANT le top-level fenetre pour que les
 *     ecritures du top-level (un helper qui pose LIB..Libelle) gagnent ensuite.
 *   - SEED_PHASE_DYNAMIC : resout + seede uniquement les props a evaluateur
 *     (Image = imgSrc) qui peuvent dependre d'une globale top-level fenetre.
 *     Posee APRES le top-level (les globales existent). Ne re-pose PAS les
 *     litteraux (pour ne pas ecraser une ecriture du top-level).
 *
 *  — le chemin GUI (open_window_instance) suit desormais le meme
 * decoupage LITERAL-avant / DYNAMIC-apres le top-level fenetre que le chemin
 * headless : un seul enum a deux phases, plus de variante "tout en un temps".
 * (SeedPhase + forward decl remontes avant build_window, cf. plus haut.) */

static ObjArray* widget_items_store_get_or_create_uici(ObjUiControlInstance* uici);

/*  — forme CANON d'une option de Sélecteur / Interrupteur : une
 * instance typee, jamais une chaine nue. C'est ce que rend `SEL_X[i]` dans les
 * deux modes — proxy construit au-dessus du widget Qt en GUI, element du store
 * d'items en headless — et c'est la seule maniere qu'une option declaree en
 * PROPRIETES soit indistinguable d'une option ajoutee par SélecteurAjoute. Un
 * acces qui marche ou non selon la provenance de l'element serait pire que pas
 * d'acces du tout. */
static ObjInstance* option_new_instance(bool interrupteur,
                                        const char* lib, const char* sub,
                                        const char* val, int state) {
    const char* tname = interrupteur ? "InterrupteurOption" : "SelecteurOption";
    ObjString* tn = obj_copy_string(tname, (int)strlen(tname));
    vm_push(OBJ_VAL(tn));                 /* obj_new_instance peut declencher le GC */
    ObjInstance* opt = obj_new_instance(tn);
    vm_pop();
    if (!opt) return NULL;
    vm_push(OBJ_VAL(opt));                /* les inst_field_set_* allouent */
    inst_field_set_str(opt, "Libelle",     lib ? lib : "");
    inst_field_set_str(opt, "SousLibelle", sub ? sub : "");
    if (interrupteur) {
        /*  — Valeur = coche 0/1/-1, toujours un ENTIER (jamais un
         * booleen) : `n est un entier = INT_X[i]` doit recevoir un scalaire
         * typable en entier, et la forme nue equivaut a ..Valeur. TroisEtats =
         * mode PAR OPTION. Les casiers __int_coche / __int_tristate sont la
         * source de verite HEADLESS (le champ visible Valeur/TroisEtats est
         * rafraichi depuis eux a chaque lecture indexee) ; en GUI c'est le
         * widget qui fait foi (dispatch via __int_widget_ptr). */
        inst_field_set_int(opt, "Valeur", state);
        inst_field_set_bool(opt, "TroisEtats", false);
        inst_field_set_int(opt, "__int_coche", state);
        inst_field_set_bool(opt, "__int_tristate", false);
    } else {
        inst_field_set_str(opt, "ValeurRenvoyee", val ? val : "");
    }
    vm_pop();
    return opt;
}

/*  — rafraichit les champs visibles Valeur / TroisEtats d'une option
 * d'Interrupteur HEADLESS depuis les casiers de verite __int_coche /
 * __int_tristate. Appele avant chaque lecture indexee : garantit que la coche
 * relue reflete toujours l'etat courant, jamais une valeur -1 refusee qui aurait
 * transitoirement salit le champ Valeur. */
static void option_headless_refresh_coche(ObjInstance* opt) {
    if (!opt) return;
    Value v;
    int coche = 0;
    if (instance_get_value(opt, "__int_coche", &v) && IS_INTEGER(v))
        coche = (int)AS_INTEGER(v);
    bool tristate = false;
    if (instance_get_value(opt, "__int_tristate", &v) && IS_BOOL(v))
        tristate = AS_BOOL(v);
    vm_push(OBJ_VAL(opt));
    inst_field_set_int(opt, "Valeur", coche);
    inst_field_set_bool(opt, "TroisEtats", tristate);
    vm_pop();
}

/*  — lit un element du tableau `Options` : chaine nue OU structure
 * {Libelle, SousLibelle, ValeurRenvoyee}. Meme tolerance polymorphe (et meme
 * exigence de casse) que le peuplement du widget Qt dans build_control : les
 * deux chemins doivent lire la meme declaration de la meme facon. */
static void option_read_decl_element(Value v, const char** lib,
                                     const char** sub, const char** val) {
    *lib = ""; *sub = ""; *val = "";
    if (IS_STRING(v)) { *lib = AS_CSTRING(v); return; }
    if (!IS_INSTANCE(v)) return;
    ObjInstance* inst = AS_INSTANCE(v);
    Value f;
    if (instance_get_value(inst, "Libelle", &f) && IS_STRING(f))
        *lib = AS_CSTRING(f);
    if (instance_get_value(inst, "SousLibelle", &f) && IS_STRING(f))
        *sub = AS_CSTRING(f);
    if (instance_get_value(inst, "ValeurRenvoyee", &f) && IS_STRING(f))
        *val = AS_CSTRING(f);
}

/*  — (re)peuple le store d'items d'un Sélecteur / Interrupteur depuis
 * un tableau `Options`. Remplacement integral, pas ajout : `..Options = [...]`
 * substitue la liste, il ne la prolonge pas — comme cote widget Qt, qui vide
 * avant de remplir. Sans ce seed, ..Occurrence rend 0 en headless et SEL_X[i]
 * n'a rien a designer. */
static void seed_options_headless(ObjUiControlInstance* uici, Value optsVal,
                                  bool interrupteur) {
    if (!uici || !IS_ARRAY(optsVal)) return;
    ObjArray* items = widget_items_store_get_or_create_uici(uici);
    if (!items) return;
    items->elements.count = 0;
    vm_push(optsVal);                     /* source lue pendant des allocations */
    ObjArray* opts = AS_ARRAY(optsVal);
    for (int i = 0; i < opts->elements.count; i++) {
        const char *lib, *sub, *val;
        option_read_decl_element(opts->elements.values[i], &lib, &sub, &val);
        ObjInstance* opt = option_new_instance(interrupteur, lib, sub, val, 0);
        if (opt) obj_array_push(items, OBJ_VAL(opt));
        else     value_array_write(&items->elements, NIL_VAL);
    }
    vm_pop();
}

/*  — proxy d'option construit au-dessus du widget Qt (mode GUI, ou le
 * widget est la source de verite). Meme forme d'instance que l'element de store
 * headless, plus la back-ref que le dispatch d'ecriture d'instance relit pour
 * repousser vers le widget. NIL_VAL si l'indice est hors plage : l'appelant
 * decide alors quoi rendre.
 *
 * Les trois getters d'option de ui_qt.cpp partagent UN SEUL QByteArray statique
 * par kind : chaque appel invalide le retour du precedent. Retenir les trois
 * pointeurs avant de les consommer rendait un sous-libelle en octets morts
 * (mesure : « Sous 24h » -> trois octets illisibles). On consomme donc chaque
 * chaine — inst_field_set_str la recopie dans un ObjString — avant l'appel
 * suivant. */
static Value option_proxy_from_widget(bool interrupteur, OwlControl* w, int idx) {
    if (!w) return NIL_VAL;
    int n = interrupteur ? owl_ui_interrupteur_count(w)
                         : owl_ui_selecteur_count(w);
    if (idx < 1 || idx > n) return NIL_VAL;
    int state = interrupteur ? owl_ui_interrupteur_get_option_state(w, idx) : 0;
    ObjInstance* opt = option_new_instance(interrupteur, NULL, NULL, NULL, state);
    if (!opt) return NIL_VAL;
    vm_push(OBJ_VAL(opt));
    inst_field_set_str(opt, "Libelle",
                       interrupteur ? owl_ui_interrupteur_get_option_label(w, idx)
                                    : owl_ui_selecteur_get_option_label(w, idx));
    inst_field_set_str(opt, "SousLibelle",
                       interrupteur ? owl_ui_interrupteur_get_option_sub_label(w, idx)
                                    : owl_ui_selecteur_get_option_sub_label(w, idx));
    if (!interrupteur)
        inst_field_set_str(opt, "ValeurRenvoyee",
                           owl_ui_selecteur_get_option_stored(w, idx));
    /*  — mode TroisEtats PAR OPTION lu depuis le widget (source de
     * verite GUI), symetrique du champ Valeur. */
    if (interrupteur)
        inst_field_set_bool(opt, "TroisEtats",
                            owl_ui_interrupteur_get_option_three_states(w, idx) != 0);
    inst_field_set_int(opt, interrupteur ? "__int_index" : "__sel_index", idx);
    inst_field_set_int(opt,
                       interrupteur ? "__int_widget_ptr" : "__sel_widget_ptr",
                       (int64_t)(intptr_t)w);
    vm_pop();
    return OBJ_VAL(opt);
}

/*  — seed headless du ContenuInitial multi-ligne d'un Champ Liste :
 * chaque ligne (separateur '\n', CRLF tolere) devient un item du store headless,
 * de sorte que ..Occurrence lise le bon compte sans widget Qt. Symetrique de
 * owl_ui_list_set_content cote GUI. Appele depuis les branches LISTE et COMBO
 * ; ne touche pas Sélecteur/Interrupteur. */
static void seed_liste_contenu_initial_headless(ObjUiControlInstance* uici,
                                                 const char* content) {
    if (!uici || !content || !*content) return;
    ObjArray* items = widget_items_store_get_or_create_uici(uici);
    if (!items) return;
    const char* p = content;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r') len--;  /* CRLF tolere */
        Value sv = OBJ_VAL(obj_copy_string(p, (int)len));
        vm_push(sv);  /* value_array_write peut grow → GC */
        value_array_write(&items->elements, sv);
        vm_pop();
        if (!nl) break;
        p = nl + 1;
    }
}

/*  — matérialise une couleur de slot plat (int64 packé BGR + bit alpha,
 * cf OWL_MENU_COL_PACK) dans un champ d'instance. Le slot est le CANON du rendu ;
 * l'instance n'en est que le miroir lisible. Un slot semi-transparent est
 * matérialisé sur sa composante BGR seule : la forme instance Couleur (qui seule
 * porte l'Opacité côté lecture) se construit dans le module couleur, hors de
 * portée ici — le rendu, lui, garde l'alpha (il lit le slot, pas l'instance). */
static void style_flat_color_into_instance(ObjInstance* dst, const char* field,
                                           int64_t packed) {
    if (!dst || packed < 0) return;
    inst_field_set_int(dst, field, packed & 0xFFFFFF);
}

/*  — recopie une brique de style plate d'un volet dans l'instance
 * styleVolet / styleVoletBase correspondante du store. `with_police` : seule la
 * surface de BASE porte une Police et un Arrondi (miroir de
 * onglet_surface_from_instance, qui lit dans l'autre sens). */
static void onglet_brick_into_instance(const OwlOngletStyleBrick* b,
                                       ObjInstance* dst, bool with_police) {
    if (!b || !dst) return;
    style_flat_color_into_instance(dst, "Couleur",     b->couleur);
    style_flat_color_into_instance(dst, "CouleurFond", b->couleur_fond);
    if (!with_police) return;
    if (b->arrondi >= 0) inst_field_set_int(dst, "Arrondi", b->arrondi);
    if (b->police_nom == NULL && b->police_taille < 0 && b->police_gras < 0
        && b->police_italique < 0 && b->police_souligne < 0
        && b->police_barre < 0)
        return;
    ObjInstance* pol = instance_get_instance(dst, "Police");
    if (!pol) return;
    if (b->police_nom != NULL)   inst_field_set_str(pol, "Nom", b->police_nom);
    if (b->police_taille >= 0)   inst_field_set_int(pol, "Taille", b->police_taille);
    if (b->police_gras >= 0)     inst_field_set_bool(pol, "Gras",      b->police_gras != 0);
    if (b->police_italique >= 0) inst_field_set_bool(pol, "Italique",  b->police_italique != 0);
    if (b->police_souligne >= 0) inst_field_set_bool(pol, "Souligne",  b->police_souligne != 0);
    if (b->police_barre >= 0)    inst_field_set_bool(pol, "Barre",     b->police_barre != 0);
}

/*  — la surcharge `Style..Volet..Prop = v` d'un Champ Onglet vit dans des
 * slots plats du decl C, jamais dans l'instance du store. La relecture
 * `ONG..Style..Volet..Prop` interroge l'instance : elle rendait donc la valeur du
 * THÈME (la base) et jamais la surcharge. On matérialise ici les slots posés dans
 * l'instance, APRÈS le seed de la référence (`Style = theme.<nom>`) donc la
 * surcharge écrase bien la base — même ordre de composition que le rendu.
 * Le rendu est inchangé : il continue de partir des slots plats.
 *
 * Conditionné à la PRÉSENCE d'une instance dans le store, c'est-à-dire à une
 * référence effectivement posée : la matérialiser d'office pour un style
 * purement plat DÉPLACERAIT le rendu (mesuré). Une Police plate partielle
 * (`Police..Gras = Vrai` seul) rend l'instance non-défaut, et le chemin de
 * lecture inverse en tire alors les feuilles restantes du gabarit — la feuille
 * QSS gagnait `font-size: 11pt`, `font-style: normal` et `text-decoration:
 * none`, qui écrasent le défaut du thème. Sans référence, rien à composer :
 * les slots plats sont déjà la vérité entière. */
static void seed_onglet_style_flat_into_store(ObjUiControlInstance* uici,
                                              const OwlControlDecl* c) {
    if (!uici || !c) return;
    if (c->kind != OWL_CONTROL_ONGLET || !c->ong_has_style) return;
    Value styleVal = NIL_VAL;
    if (!obj_ui_control_instance_headless_get(uici, "Style", &styleVal)
        || !IS_INSTANCE(styleVal))
        return;
    ObjInstance* style = AS_INSTANCE(styleVal);
    ObjInstance* ce = instance_get_instance(style, "CadreExterieur");
    if (ce) {
        style_flat_color_into_instance(ce, "CouleurFond",
                                       c->ong_cadre_ext.couleur_fond);
        ObjInstance* cadre = instance_get_instance(ce, "Cadre");
        if (cadre) {
            style_flat_color_into_instance(cadre, "Couleur",
                                           c->ong_cadre_ext.cadre_couleur);
            if (c->ong_cadre_ext.cadre_epaisseur >= 0)
                inst_field_set_int(cadre, "Epaisseur", c->ong_cadre_ext.cadre_epaisseur);
            if (c->ong_cadre_ext.cadre_arrondi >= 0)
                inst_field_set_int(cadre, "Arrondi", c->ong_cadre_ext.cadre_arrondi);
        }
    }
    ObjInstance* actif   = instance_get_instance(style, "VoletActif");
    ObjInstance* inactif = instance_get_instance(style, "VoletInactif");
    onglet_brick_into_instance(&c->ong_actif_base,   actif,   true);
    onglet_brick_into_instance(&c->ong_inactif_base, inactif, true);
    if (actif)
        onglet_brick_into_instance(&c->ong_actif_survol,
            instance_get_instance(instance_get_instance(actif, "Etats"), "_Survol"),
            false);
    if (inactif)
        onglet_brick_into_instance(&c->ong_inactif_survol,
            instance_get_instance(instance_get_instance(inactif, "Etats"), "_Survol"),
            false);
}

static void seed_uici_headless_phase(ObjUiControlInstance* uici,
                                     OwlControlDecl* c, SeedPhase phase) {
    if (!uici || !c) return;
    if (phase != SEED_PHASE_LITERAL)
        resolve_dynamic_props(c->properties, c->property_count);
    for (int i = 0; i < c->property_count; i++) {
        OwlPropertyInit* p = &c->properties[i];
        if (!p->is_constant) continue;  /* eval non resolu -> skip defensif */
        /* Une prop dynamique resolue garde eval != NULL (resolve_dynamic_props
         * pose is_constant=true sans effacer eval) -> on distingue literal vs
         * dynamique meme apres resolution. */
        bool isDynamic = (p->eval != NULL);
        if (phase == SEED_PHASE_LITERAL && isDynamic)  continue;
        if (phase == SEED_PHASE_DYNAMIC && !isDynamic)  continue;
        /* premiere occurrence seulement (cf. note homonymes ci-dessus) */
        bool seen = false;
        for (int k = 0; k < i; k++) {
            if (strcasecmp(c->properties[k].name, p->name) == 0) { seen = true; break; }
        }
        if (seen) continue;
        /*  — Saisie : les props d'apparence plates declaratives sont des
         * alias vers la ZoneSaisie de l'instance Style (verite unique, cf. le
         * pattern runtime plus haut). On les seede DANS la brique Style, pas
         * sous une cle plate deconnectee, pour que la lecture back
         * `SAI..CouleurFond` (qui lit desormais la brique) reflete le
         * declaratif. Gate INPUT. */
        if (uici->decl && (OwlControlKind)uici->decl->kind == OWL_CONTROL_INPUT) {
            const char* zsSub = NULL;
            if (strcasecmp(p->name, "CouleurFond") == 0 ||
                strcasecmp(p->name, "BackgroundColor") == 0) {
                zsSub = "CouleurFond";
            } else if (strcasecmp(p->name, "PoliceCouleur") == 0 ||
                       strcasecmp(p->name, "FontColor") == 0) {
                zsSub = "Couleur";
            }
            if (zsSub != NULL) {
                ObjInstance* style =
                    uici_composite_instance(uici, "Style", "styleSaisie");
                ObjInstance* zone = style
                    ? instance_get_instance(style, "ZoneSaisie") : NULL;
                if (zone != NULL) {
                    vm_push(p->const_value);
                    vm_push(OBJ_VAL(zone));
                    ObjString* sk =
                        table_find_key_case_insensitive(&zone->fields, zsSub);
                    ObjString* key = sk != NULL
                        ? sk : obj_copy_string(zsSub, (int)strlen(zsSub));
                    obj_inst_set_kv(zone, key, p->const_value);
                    vm_pop(); /* zone */
                    vm_pop(); /* const_value */
                }
                continue;
            }
        }
        /*  — Libellé/Bouton : idem que Saisie mais chaque prop plate
         * declarative est routee vers SA sous-brique de l'instance Style (verite
         * unique). CouleurFond->CadreExtérieur..CouleurFond,
         * PoliceCouleur->Libellé..Couleur. Seede DANS la brique pour que la
         * lecture back `LIB..CouleurFond` (qui lit desormais la brique) reflete
         * le declaratif. */
        if (uici->decl) {
            OwlControlKind ukind = (OwlControlKind)uici->decl->kind;
            const char* mBrick = NULL;
            const char* mSub   = NULL;
            if (mono_style_shortcut_bricksub(ukind, p->name, &mBrick, &mSub)) {
                ObjInstance* style = uici_composite_instance(
                    uici, "Style", mono_style_template_name(ukind));
                ObjInstance* brick = style
                    ? instance_get_instance(style, mBrick) : NULL;
                if (brick != NULL) {
                    vm_push(p->const_value);
                    vm_push(OBJ_VAL(brick));
                    ObjString* sk =
                        table_find_key_case_insensitive(&brick->fields, mSub);
                    ObjString* key = sk != NULL
                        ? sk : obj_copy_string(mSub, (int)strlen(mSub));
                    obj_inst_set_kv(brick, key, p->const_value);
                    vm_pop(); /* brick */
                    vm_pop(); /* const_value */
                }
                continue;
            }
        }
        /*  — sémantique COPIE : affecter un style (instance) à la prop
         * Style d'un widget lui donne SA PROPRE copie profonde. Sans ça, deux
         * widgets stylés depuis la même source (theme.X / STY_X) partagent
         * l'instance ; une surcharge par widget (..Style..sous..feuille = ...)
         * fuite sur les autres et poisonne la source. Deep-clone => l'instance
         * ET ses sous-éléments imbriqués sont isolés. */
        Value storeVal = p->const_value;
        if ((strcasecmp(p->name, "Style") == 0) && IS_INSTANCE(p->const_value)) {
            vm_push(p->const_value);            /* root src pendant le clone */
            storeVal = owl_value_deep_clone(p->const_value);
            vm_pop();
        }
        /* GC root pendant le set : storeVal (ObjInstance composee) ne vit
         * que dans le decl C, le set peut grossir la table et declencher un
         * cycle. Une fois dans le store (uici rooted) il est atteignable. */
        vm_push(storeVal);
        obj_ui_control_instance_headless_set(uici, p->name, storeVal);
        vm_pop();

        /*  (Liste) +  (Combo) — le ContenuInitial multi-ligne
         * peuple aussi le store d'items (chaque ligne = un item), pour que
         * ..Occurrence / ..Vide soient observables en headless. Le raccourci
         * plat reste seede ci-dessus (lecture ContenuInitial).
         *
         *  — le gate LISTE/COMBO reste, mais plus pour la raison
         * d'origine : ce qui distingue ces kinds, c'est le TYPE d'element de
         * leur store. Liste et Combo y rangent des chaines ; Sélecteur et
         * Interrupteur y rangent des instances d'option (seed_options_headless
         * ci-dessous). Un store melangeant les deux formes rendrait
         * l'indexation dependante de la provenance de l'element. */
        if (uici->decl &&
            ((OwlControlKind)uici->decl->kind == OWL_CONTROL_LISTE ||
             (OwlControlKind)uici->decl->kind == OWL_CONTROL_COMBO) &&
            (strcasecmp(p->name, "ContenuInitial") == 0 ||
             strcasecmp(p->name, "InitialContent") == 0) &&
            IS_STRING(p->const_value)) {
            seed_liste_contenu_initial_headless(uici, AS_CSTRING(p->const_value));
        }

        /*  — Sélecteur / Interrupteur : `Options = [...]` peuple le
         * store d'items, pendant strict du ContenuInitial de Liste/Combo. Sans
         * lui, ..Occurrence rend 0 en headless quand le champ affiche trois
         * options, et SEL_X[i] n'a rien a designer. */
        if (uici->decl &&
            ((OwlControlKind)uici->decl->kind == OWL_CONTROL_SELECTEUR ||
             (OwlControlKind)uici->decl->kind == OWL_CONTROL_INTERRUPTEUR) &&
            strcasecmp(p->name, "Options") == 0) {
            seed_options_headless(uici, p->const_value,
                                  (OwlControlKind)uici->decl->kind
                                      == OWL_CONTROL_INTERRUPTEUR);
        }
    }
    /*  — après la boucle : la référence `Style = theme.<nom>` (prop
     * DYNAMIQUE) vient d'être déposée dans le store, la surcharge plate se pose
     * donc par-dessus. Phase DYNAMIQUE seulement : une seule passe suffit et
     * c'est la seule où la base est garantie présente. */
    if (phase == SEED_PHASE_DYNAMIC)
        seed_onglet_style_flat_into_store(uici, c);
}

/*  lot B — seed des props fenetre (Titre, Largeur, Hauteur,
 * Disposition...) dans le casier de l'instance a l'ouverture. Modele exact
 * du seed des sous-controles, adapte au catalogue fenetre. La
 * source de verite des props fenetre devient le headless_store : les acces
 * MaFenetre..Prop (lecture/ecriture) y atterrissent. */
static void seed_window_headless_from_decl(ObjWindowInstance* wi,
                                            OwlWindowDecl* decl) {
    if (!wi || !decl) return;
    /*  — `MenuPrincipal = MNU_X` du bloc PROPRIETES n'est pas une
     * OwlPropertyInit (le compilateur le range dans decl->main_menu_name) : on
     * le seede explicitement pour que `FEN_X..MenuPrincipal` soit lisible et
     * que la bascule runtime parte de la bonne valeur. */
    if (decl->main_menu_name != NULL) {
        ObjString* mn = obj_copy_string(decl->main_menu_name,
                                        (int)strlen(decl->main_menu_name));
        vm_push(OBJ_VAL(mn));
        obj_window_instance_headless_set(wi, "MenuPrincipal", OBJ_VAL(mn));
        vm_pop();
    }
    if (decl->property_count <= 0) return;
    /*  — resout ET retient les props EVAL de la fenetre (non
     * constantes au compile-time), au meme titre que le seed DYNAMIC des
     * sous-controles (seed_uici_headless_phase). Cas cible : un Titre
     * multilingue `Titre = {langueXxx: ...}` compile en eval ObjFunction
     * produisant un ObjMultilingue au runtime -> sans cette resolution, il
     * n'atterrit jamais dans le store et le hot-switch langue (Phase 2,
     * i18n_rerender_window_multilingue) ne peut pas le retrouver.
     * resolve_dynamic_props pose is_constant=true sans effacer eval : la
     * boucle ci-dessous seede donc le resultat (l'ObjMultilingue lui-meme,
     * PAS sa chaine aplatie -> le re-render pourra re-resoudre). Idempotent
     * (cache is_constant, invalide par ouverture via
     * invalidate_dynamic_props_cache). Le drain fallback_store, applique
     * APRES ce seed par l'appelant, ecrase toujours -> l'ecriture utilisateur
     * gagne sur le declaratif. */
    resolve_dynamic_props(decl->properties, decl->property_count);
    for (int i = 0; i < decl->property_count; i++) {
        OwlPropertyInit* p = &decl->properties[i];
        if (!p->is_constant) continue;
        /* premiere occurrence seulement */
        bool seen = false;
        for (int k = 0; k < i; k++) {
            if (strcasecmp(decl->properties[k].name, p->name) == 0) {
                seen = true; break;
            }
        }
        if (seen) continue;
        /*  — sémantique COPIE (parite widget, ) : affecter un
         * style (instance) à la prop Style de la fenetre lui donne SA PROPRE
         * copie profonde, pour qu'une surcharge par fenetre (`FEN..Style..sous..
         * feuille = ...`) ne fuite pas sur la source theme.X / STY_X. */
        Value storeVal = p->const_value;
        if ((strcasecmp(p->name, "Style") == 0) && IS_INSTANCE(p->const_value)) {
            vm_push(p->const_value);            /* root src pendant le clone */
            storeVal = owl_value_deep_clone(p->const_value);
            vm_pop();
        }
        vm_push(storeVal);
        obj_window_instance_headless_set(wi, p->name, storeVal);
        vm_pop();
    }
}

/*  lot B (drain) — overlay du fallback_store du proxy ObjWindowRef
 * dans le casier de l'instance fenetre fraichement ouverte. Le fallback_store
 * recueille les ecritures `FEN_X..Prop = val` faites AVANT Ouvre (cf. branche
 * IS_WINDOW_REF OP_STRUCT_SET dans vm.c). Sans ce drain, le seed declaratif
 * (seed_window_headless_from_decl) ecraserait ces ecritures explicites.
 *
 * Ordre attendu chez l'appelant : (1) seed depuis le DECL, (2) drain depuis
 * fallback_store. Le drain ecrase, ce qui materialise la regle "l'ecriture
 * utilisateur gagne sur le declaratif".
 *
 * Lookup : la cle dans vm.globals est l'ObjString interne du nom strippe de
 * la fenetre (cf. window_declaration dans compiler.c ~L14568). On reconstruit
 * cette cle via scanner_strip_accents + obj_copy_string (interning).
 *
 * Vidage post-drain : une fois transvase, le fallback_store n'a plus de
 * raison de vivre — l'instance est sur la window_stack, les ecritures
 * suivantes passent directement par OP_STRUCT_SET branche IS_WINDOW_REF
 * (qui delegue a vm_window_stack_top quand non NULL). On libere la Table
 * (memoire + symetrie avec la fin de l'instance). */
static void drain_window_ref_fallback_into_instance(ObjWindowInstance* wi,
                                                    const char* windowName) {
    if (!wi || !windowName) return;
    /* Strip + lookup dans vm.globals. */
    char stripBuf[128];
    int  stripLen = scanner_strip_accents(windowName, (int)strlen(windowName),
                                          stripBuf, sizeof(stripBuf));
    if (stripLen <= 0) return;
    ObjString* key = obj_copy_string(stripBuf, stripLen);
    /* GC root le temps du lookup + drain (table_set peut realloc + GC). */
    vm_push(OBJ_VAL(key));
    Value refVal;
    if (!table_get(&vm.globals, key, &refVal) || !IS_WINDOW_REF(refVal)) {
        vm_pop();
        return;
    }
    ObjWindowRef* wref = AS_WINDOW_REF(refVal);
    if (wref->fallback_store == NULL) {
        vm_pop();
        return;
    }
    /* Itere les entries vivantes et overlay dans le store d'instance. */
    Table* fb = wref->fallback_store;
    for (int i = 0; i < fb->capacity; i++) {
        Entry* e = &fb->entries[i];
        if (e->key == NULL) continue;
        /* La cle du fallback est deja lowercase ASCII (cf. branche IS_WINDOW_REF
         * dans vm.c OP_STRUCT_SET). obj_window_instance_headless_set
         * re-normalise via headless_key_normalize_window (idempotent). */
        vm_push(e->value);  /* GC root pendant le set */
        obj_window_instance_headless_set(wi, e->key->chars, e->value);
        vm_pop();
    }
    /* Libere le fallback_store : plus de raison de coexister avec l'instance,
     * et evite que des ecritures futures atterrissent par erreur dedans (la
     * branche IS_WINDOW_REF dans vm.c verifie d'abord window_stack_top, donc
     * en theorie pas atteignable tant qu'une instance est sur la pile — mais
     * mieux vaut nettoyer net : alpha = break net). */
    table_free(fb);
    free(fb);
    wref->fallback_store = NULL;
    vm_pop();  /* key */
}

/*  — coeur partage par Ouvre / OuvreFille / OuvreSoeur. Monte une
 * instance de la fenetre `decl`, materialise ses sous-controles et les push
 * sur vm.window_stack, puis declenche les PROCEDURE Initialisation.
 *
 * `modal` : Vrai = comportement Ouvre (event loop bloquante en GUI, l'appel
 *   ne rend la main qu'apres Ferme/clic [X]). Faux = OuvreFille/OuvreSoeur
 *   non-modal (rend la main immediatement, l'instance reste ouverte en
 *   parallele jusqu'a un Ferme explicite).
 *
 * En mode headless (owl test) les deux variantes sont identiques : pas
 * d'event loop, l'instance est laissee sur la stack et c'est Ferme() qui la
 * pop (pile LIFO). La distinction modal/non-modal n'a d'effet qu'en GUI.
 *
 * `fnName` sert uniquement aux messages d'erreur stderr. */
/* Injecte les valeurs des params utilisateur dans les globales declarees au
 * parse (`Fenetre(p1 est un type, ...)`). Symetrie stricte avec iImprimeEtat.
 * Itere sur TOUS les params declares : arg fourni -> valeur fournie ; sinon
 * si le param a une valeur par defaut -> le defaut ; sinon
 * -> NIL_VAL (reset explicite pour ne pas heriter d'une valeur d'un cycle
 * d'ouverture precedent laissee dans la globale). A appeler AVANT l'execution
 * du top-level fenetre ( : le top-level tourne a l'Ouvre, fenetre
 * endormie) pour que top-level/FinInitialisation lisent les bonnes valeurs. */
static void inject_window_params(const OwlWindowDecl* decl,
                                 int paramCount, Value* paramArgs) {
    for (int i = 0; i < decl->param_count; i++) {
        Value v;
        if (i < paramCount)                v = paramArgs[i];
        else if (decl->params[i].has_default) v = decl->params[i].default_value;
        else                               v = NIL_VAL;
        obj_globals_set(decl->params[i].name, v);
    }
}

static Value open_window_instance(const char* name, bool modal, const char* fnName,
                                  int paramCount, Value* paramArgs) {
    const OwlWindowDecl* decl = vm_window_lookup(name);
    if (!decl) {
        fprintf(stderr, "[%s] Fenetre '%s' non declaree.\n", fnName, name);
        return NIL_VAL;
    }

    /*  — invalide le cache de resolution des props DYNAMIQUES sur le
     * decl PARTAGE, au tout debut de CHAQUE ouverture (GUI et headless). Sans
     * ca, une prop dynamique (`Libelle = gTexte`) resterait figee a la valeur
     * de la 1re ouverture. Couvre la fenetre, ses controles et leurs colonnes.
     * Les litteraux purs (eval == NULL) ne sont pas touches. */
    invalidate_dynamic_props_cache((OwlPropertyInit*)decl->properties,
                                   decl->property_count);
    for (int i = 0; i < decl->control_count; i++) {
        OwlControlDecl* c = (OwlControlDecl*)&decl->controls[i];
        invalidate_dynamic_props_cache(c->properties, c->property_count);
        for (int j = 0; j < c->column_count; j++) {
            invalidate_dynamic_props_cache(c->columns[j].properties,
                                           c->columns[j].property_count);
        }
    }

    /* Mode test unitaire : on skip l'instanciation Qt + l'event loop bloquante.
     * On execute neanmoins le bloc Initialisation des sous-controles (s'il y
     * en a) pour que les tests verifient la peuplation des props virtuelles.
     * Pas de widget Qt -> les set/get de props transitent par headless_store
     * de l'ObjUiControlInstance ( modele C ). MaFenetre est
     * resolue via une ObjString du nom : ca permet a `Ferme(MaFenetre, val)`
     * de capturer val correctement (fn_ferme detecte IS_STRING +
     * vm_window_lookup). */
    if (vm.headless_mode) {
        Value prev_return = g_return_value;
        g_return_value = NIL_VAL;
        ObjString* nameStr = obj_copy_string(name, (int)strlen(name));
        /*  — Creation de l'ObjWindowInstance + materialisation de
         * chaque sous-controle en ObjUiControlInstance attache a l'instance.
         * Push sur vm.window_stack pour que les acces aux champs (via
         * ObjUiControlRef::resolve_ui_ref) trouvent l'instance courante. */
        ObjWindowInstance* inst = obj_new_window_instance((OwlWindowDecl*)decl);
        vm_push(OBJ_VAL(inst));  /* GC root pendant la construction */
        /*  lot B — seed des props fenetre dans le casier d'instance.
         * Symetrique du seed des sous-controles, source de verite
         * unique pour MaFenetre..Prop. */
        seed_window_headless_from_decl(inst, (OwlWindowDecl*)decl);
        /*  lot B (drain) — overlay des ecritures pre-Ouvre stockees
         * dans le fallback_store du proxy ObjWindowRef. APRES le seed declaratif :
         * l'ecriture utilisateur explicite gagne sur le declaratif. */
        drain_window_ref_fallback_into_instance(inst, name);
        /*  — on CREE d'abord toutes les instances de controle, puis on
         * seede uniquement leurs props LITTERALES (literaux : Libelle =
         * "depart", X = 10...). Le seed des props DYNAMIQUES (Image = imgSrc,
         * a evaluateur) est differe APRES le top-level fenetre (cf. plus bas) :
         * une prop dynamique peut referencer une globale top-level fenetre, qui
         * n'existe qu'une fois le top-level execute. L'eager au chargement
         * (, supprime) garantissait jadis ces globales avant Ouvre ;
         * desormais le top-level ne tourne qu'a l'ouverture. On seede les
         * litteraux AVANT le top-level pour que ses ecritures (ex. un helper
         * qui pose LIB..Libelle) gagnent sur le declaratif fixe. */
        for (int i = 0; i < decl->control_count; i++) {
            OwlControlDecl* c = (OwlControlDecl*)&decl->controls[i];
            ObjUiControlInstance* uici = obj_new_ui_control_instance(c, inst);
            obj_window_instance_add_control(inst, uici);
            seed_uici_headless_phase(uici, c, SEED_PHASE_LITERAL);
        }
        vm_window_stack_push(inst);

        /*  — liaison des PROCEDURE custom (user_procs) AVANT toute
         * execution de code de la fenetre. Symetrique du chemin GUI
         * (build_window). Sans ce binding precoce, le top-level d'un bloc
         * champ qui appelle un helper FEN crashe sur "Seules les fonctions
         * et procedures peuvent etre appelees" parce que la globale
         * '<window>_<proc>' n'existe pas encore. Le compileur a deja
         * reecrit les call-sites internes vers ces noms qualifies. */
        for (int i = 0; i < decl->user_proc_count; i++) {
            char qname[256];
            snprintf(qname, sizeof(qname), "%s_%s",
                     decl->name, decl->user_procs[i].name);
            obj_globals_set(qname, OBJ_VAL(decl->user_procs[i].impl));
        }
        for (int i = 0; i < decl->control_count; i++) {
            const OwlControlDecl* c = &decl->controls[i];
            for (int j = 0; j < c->user_proc_count; j++) {
                char qname[256];
                snprintf(qname, sizeof(qname), "%s_%s",
                         c->name, c->user_procs[j].name);
                obj_globals_set(qname, OBJ_VAL(c->user_procs[j].impl));
            }
        }

        /*  — ORDRE DU CYCLE DE VIE (identique GUI/headless, hors
         * dessin) : top-level FENETRE -> seed declaratif des champs ->
         * top-levels CHAMPS (ordre d'ecriture) -> [dessin, saute en headless]
         * -> FinInitialisation. L'event Initialisation est mort : le top-level
         * fait foi (fenetre + champ).
         *
         * 1. top-level FENETRE (arity 0 : declarations globales + statements
         *    de niveau 0 du body fenetre, executes en premier). Pose les
         *    globales top-level (cf. ) AVANT le seed des champs qui
         *    peut les referencer.
         *     — push event_sender = la fenetre (via son ObjWindowRef
         *    global) pour que MoiMême resolve a la fenetre dans le top-level. */
        Value winSender = moimeme_widget_value_for_name(name);
        /* Injecte les params Ouvre(FEN_X, val1, ...) AVANT le top-level pour
         * que ses declarations / FinInitialisation lisent les valeurs. */
        inject_window_params(decl, paramCount, paramArgs);
        if (decl->top_level) {
            vm_event_sender_push(winSender);
            vm_push(OBJ_VAL(decl->top_level));
            vm_call_reentrant(OBJ_VAL(decl->top_level), 0);
            vm_pop();  /* valeur de retour */
            vm_event_sender_pop();
        }

        /*  — seed des props DYNAMIQUES des champs APRES le top-level
         * fenetre : les evaluateurs (`Image = imgSrc`) voient les globales
         * top-level. Les litteraux ont deja ete poses avant le top-level (et
         * eventuellement ecrases par une ecriture du top-level, qui gagne).
         * uici deja root via inst (sur la vm stack). */
        for (int i = 0; i < inst->control_count; i++) {
            ObjUiControlInstance* uici = inst->controls[i];
            seed_uici_headless_phase(uici, (OwlControlDecl*)uici->decl,
                                     SEED_PHASE_DYNAMIC);
        }

        /* 2. top-levels des CHAMPS (ordre d'ecriture).
         *     — push event_sender = le champ (ObjUiControlRef) pour
         *    que MoiMême resolve au champ dans son top-level. */
        for (int i = 0; i < decl->control_count; i++) {
            const OwlControlDecl* c = &decl->controls[i];
            if (c->top_level) {
                Value ctrlSender = moimeme_widget_value_for_name(c->name);
                vm_event_sender_push(ctrlSender);
                vm_push(OBJ_VAL(c->top_level));
                vm_call_reentrant(OBJ_VAL(c->top_level), 0);
                vm_pop();
                vm_event_sender_pop();
            }
        }

        /* 3. [dessin : saute en headless] */

        /* (Step 4 historique  — liaison des user_procs — remonte
         *  AVANT le top-level fenetre par , pour parite avec le
         *  chemin GUI et pour rendre les helpers appelables depuis tous
         *  les sites, y compris top-level de bloc champ.) */

        /* 5. FinInitialisation() FENETRE (parite GUI : APRES toutes les
         *    initialisations).  — meme push fenetre que top-level. */
        {
            const OwlEventBlock* fin = find_event(decl->events, decl->event_count,
                                                  "FinInitialisation",
                                                  "EndInitialization");
            if (fin && fin->impl) {
                vm_event_sender_push(winSender);
                vm_push(OBJ_VAL(fin->impl));
                vm_push(OBJ_VAL(nameStr));   /* MaFenetre */
                vm_call_reentrant(OBJ_VAL(fin->impl), 1);
                vm_pop();
                vm_event_sender_pop();
            }
        }
        /*  — focus initial automatique : le premier champ saisissable
         * (ordre lexical) recoit le focus apres FinInitialisation, declenchant
         * son surEntree. Aucun effet si la fenetre n'a aucun champ saisissable. */
        ui_apply_initial_focus(inst);
        Value result = g_return_value;
        g_return_value = prev_return;
        /*  — en mode headless, Ouvre est non-bloquant : on rend la
         * main APRES Initialisation mais on LAISSE l'instance sur la stack.
         * Elle sera pop par Ferme(). C'est le canon : la fenetre reste
         * ouverte tant que Ferme n'a pas ete appele. Si user oublie Ferme,
         * la stack se vide a la fin du programme (vm_window_stack_clear). */
        vm_pop();  /* depush l'ObjWindowInstance root */
        return result;
    }

    ensure_ui_init();
    /*  — Ouvre imbrique : ne PAS faire reset_bindings() quand une
     * boucle event Qt tourne deja (Ouvre depuis un handler d'une autre
     * fenetre). Ce reset ecraserait les bindings de la fenetre parente, dont
     * les signaux Qt portent encore leurs anciens index (cf. bindings_mark).
     * Au top-level (aucune boucle), reset = base propre (mark vaudra 0). En
     * imbrique, on garde la table parente et les bindings de la fille
     * s'ajoutent par-dessus ; bindings_restore en sortie de boucle modale
     * rend la table parente intacte. */
    if (!owl_ui_event_loop_running()) {
        reset_bindings();
    }
    int bindings_save = bindings_mark();
    OwlWindow* prev_window = g_current_window;
    Value      prev_return = g_return_value;
    g_return_value = NIL_VAL;
    /*  — Modele C : creer l'ObjWindowInstance + materialiser les
     * ObjUiControlInstance pour chaque sous-controle AVANT build_window.
     * Push sur vm.window_stack pour que build_control puisse rattacher
     * chaque widget Qt frais a son uici (cf build_control ligne ~1254). */
    ObjWindowInstance* inst = obj_new_window_instance((OwlWindowDecl*)decl);
    vm_push(OBJ_VAL(inst));  /* GC root */
    /*  lot B — pareil cote GUI : seed des props fenetre AVANT
     * build_window. Le store est la source de verite, build_window pourra
     * lire les valeurs declarees pour configurer le QMainWindow. */
    seed_window_headless_from_decl(inst, (OwlWindowDecl*)decl);
    /*  lot B (drain) — overlay du fallback_store APRES seed declaratif :
     * `FEN_X..Prop = val` ecrit AVANT Ouvre prend le pas sur le declaratif. */
    drain_window_ref_fallback_into_instance(inst, name);
    for (int i = 0; i < decl->control_count; i++) {
        OwlControlDecl* c = (OwlControlDecl*)&decl->controls[i];
        ObjUiControlInstance* uici = obj_new_ui_control_instance(c, inst);
        obj_window_instance_add_control(inst, uici);
        /*  lot 3 — seede le declaratif dans le store d'instance AUSSI
         * en GUI : le store est la source de verite unique (GUI et headless),
         * Qt l'applique (build_control lit le composite du store). uici est
         * root via inst (sur la vm stack).
         *  — seulement les LITTERAUX ici ; les DYNAMIQUES sont seedees
         * APRES build_window (donc apres le top-level fenetre execute dans
         * build_window), symetrie avec le chemin headless. Sans ca, une prop
         * dynamique referencant une globale top-level fenetre etait evaluee
         * trop tot -> "Variable inconnue 'FEN_X$gGlob'". */
        seed_uici_headless_phase(uici, c, SEED_PHASE_LITERAL);
    }
    vm_window_stack_push(inst);
    /* Injecte les params Ouvre(FEN_X, val1, ...) AVANT build_window : c'est
     * lui qui execute le top-level fenetre (GUI), qui doit voir les valeurs. */
    inject_window_params(decl, paramCount, paramArgs);
    OwlWindow* w    = build_window(decl);
    g_current_window = w;

    /*  — focus initial automatique (GUI) : le widget est materialise
     * (uici->widget non NULL), ui_apply_initial_focus applique le focus Qt reel
     * sur le premier champ saisissable + declenche son surEntree. Idem headless. */
    ui_apply_initial_focus(inst);

    /*  — le focus initial GUI passe par un setFocus DIFFERE (QTimer 0,
     * ) et, depuis l'Option A, surEntree n'est plus emis synchrone
     * mais via le FocusIn Qt. Sur le chemin NON-MODAL (OuvreSoeur/OuvreFille),
     * Ouvre rend la main sans entrer dans une boucle event : sans pompe, le
     * FocusIn d'auto-focus ne tire qu'au prochain pump (TestSimule*, frappe...),
     * et le surEntree initial serait observe trop tard (regression vs headless
     * ou il est synchrone). On pompe donc une fois pour faire echeoir le timer
     * setFocus -> FocusIn -> surEntree avant le retour. No-op si Qt non init.
     * Le chemin modal n'en a pas besoin : owl_ui_run_modal_for pompe la boucle. */
    if (!modal) {
        owl_ui_pump_events(50);
    }

    /*  — non-modal (OuvreFille/OuvreSoeur) : afficher la fenetre
     * SANS entrer dans l'event loop bloquante. On rend la main immediatement
     * et l'instance reste sur la stack (fermee par un Ferme explicite). */
    if (!modal) {
        Value result = g_return_value;
        g_current_window = prev_window;
        g_return_value   = prev_return;
        vm_pop();  /* depush ObjWindowInstance root (reste sur window_stack) */
        return result;
    }

    /* Run loop bloquante jusqu'a Ferme() ou clic [X].
     *  — owl_ui_run_modal_for arbitre : QApplication::exec() au
     * top-level, QEventLoop locale en imbrique (Ouvre depuis un handler
     * d'une autre fenetre). Sans cet arbitrage, un second exec() est
     * refuse par Qt et le code post-exec depilait l'instance alors que la
     * fenetre Qt restait affichee (window stack desync, MISS silencieux
     * sur tous les handlers ulterieurs).
     *
     *  — fermeture demandee pendant l'init : build_window execute le
     * top-level fenetre/champ ET FinInitialisation AVANT ce point. Si l'un
     * d'eux a appele Ferme(), fn_ferme a deja pop `inst` de la window_stack
     * (et poste la fermeture Qt), alors qu'aucune boucle modale n'etait
     * encore active — le owl_ui_quit() de Ferme a donc ete avale. Entrer
     * maintenant dans owl_ui_run_modal_for sur une fenetre deja fermee
     * bloquerait l'app indefiniment (plus rien ne peut quitter la boucle).
     * On detecte le cas par l'instance disparue du sommet de pile (rien
     * d'autre ne pop l'instance courante pendant build_window : un Ouvre
     * imbrique dans l'init se resout et repop son parent) et on rend la main
     * sans entrer dans la modale. Le teardown ci-dessous reste coherent :
     * bindings_restore joue, et le pop final est deja garde par
     * `vm_window_stack_top() == inst`. */
    if (vm_window_stack_top() == inst) {
        owl_ui_run_modal_for(w);
    }

    /*  — la fenetre fille est fermee : on restaure le compte de
     * bindings a sa valeur d'avant l'ouverture. Les bindings de la fille
     * (index >= bindings_save) sont liberes ; ceux de la parente (index <
     * bindings_save) redeviennent les seuls valides, donc les signaux Qt de
     * la parente re-pointent sur SES handlers. Au top-level, bindings_save
     * vaut 0 -> equivaut a un reset (la fenetre top-level vient de se
     * fermer, plus rien a dispatcher). */
    bindings_restore(bindings_save);

    /* Capture la ValeurRenvoyee posee par Ferme(val) puis restore. */
    Value result = g_return_value;
    g_current_window = prev_window;
    g_return_value   = prev_return;
    /*  — pop l'instance (idempotent : Ferme() peut deja l'avoir
     * fait, cf fn_ferme). */
    if (vm_window_stack_top() == inst) vm_window_stack_pop();
    vm_pop();  /* depush ObjWindowInstance root */
    return result;
}

/* Ouvre(FEN_X [, val1 [, ...]]) - modale. Les args apres le nom de fenetre
 * sont injectes dans les params declares sur la 1ere ligne
 * (`Fenetre(p1 est un type, ...)`) avant l'execution du top-level. Retourne
 * la ValeurRenvoyee posee par Ferme(). */
static Value fn_ouvre(int argCount, Value* args) {
    const char* name = window_name_from_arg(argCount, args);
    if (!name) {
        fprintf(stderr, "[Ouvre] Argument 1 doit etre un nom de fenetre (FEN_X).\n");
        return NIL_VAL;
    }
    return open_window_instance(name, true, "Ouvre",
                                argCount - 1, args + 1);
}

/* OuvreFille(FEN_X [, p1 [, ...]]) - non-modale. Push une
 * instance, execute Initialisation, rend la main immediatement. La fenetre
 * reste ouverte en parallele jusqu'a un Ferme explicite. */
static Value fn_ouvre_fille(int argCount, Value* args) {
    const char* name = window_name_from_arg(argCount, args);
    if (!name) {
        fprintf(stderr, "[OuvreFille] Argument 1 doit etre un nom de fenetre (FEN_X).\n");
        return NIL_VAL;
    }
    return open_window_instance(name, false, "OuvreFille",
                                argCount - 1, args + 1);
}

/* OuvreSoeur(FEN_X [, p1 [, ...]]) - non-modale. Meme mecanique
 * runtime qu'OuvreFille ; la difference canon est l'absence de relation
 * hierarchique appelant->ouverte. Ouvrir 2x la meme fenetre cree 2 instances
 * independantes (chaque instance a son propre etat de champs). */
static Value fn_ouvre_soeur(int argCount, Value* args) {
    const char* name = window_name_from_arg(argCount, args);
    if (!name) {
        fprintf(stderr, "[OuvreSoeur] Argument 1 doit etre un nom de fenetre (FEN_X).\n");
        return NIL_VAL;
    }
    return open_window_instance(name, false, "OuvreSoeur",
                                argCount - 1, args + 1);
}

/* Declenche un event lifecycle sans parametre de la fenetre `inst` et rend la
 * valeur renvoyee par le handler (NIL si la fenetre n'en declare pas). Le seul
 * slot pousse est MaFenetre, comme sur le reste du chemin lifecycle runtime. */
static Value fire_window_lifecycle_event(ObjWindowInstance* inst,
                                          const char* nameFR, const char* nameEN) {
    if (inst == NULL || inst->decl == NULL) return NIL_VAL;
    const OwlEventBlock* ev = find_event(inst->decl->events,
                                          inst->decl->event_count,
                                          nameFR, nameEN);
    if (ev == NULL || ev->impl == NULL) return NIL_VAL;
    ObjString* wn = obj_copy_string(inst->decl->name,
                                    (int)strlen(inst->decl->name));
    vm_push(OBJ_VAL(ev->impl));
    vm_push(OBJ_VAL(wn));   /* MaFenetre */
    vm_call_reentrant(OBJ_VAL(ev->impl), 1);
    return vm_pop();
}

/* Ferme([fen [, val [, ... valN]]]) - ferme la fenetre.
 *
 * Le 1er argument chaine designe TOUJOURS un nom de fenetre (canon) :
 *   - chaine vide ""        -> fenetre courante (idiome Ferme("", val))
 *   - nom de fenetre connue -> fenetre courante (single window), arg suivant = valeur
 *   - nom inconnu non vide  -> erreur runtime soft, pas de fermeture
 * Si arg[0] n'est pas une chaine (entier, etc.), c'est la valeur renvoyee
 * (ValeurRenvoyee captee par Ouvre).
 *
 * Multiples valeurs : pour l'instant on prend juste la 1ere apres le nom de
 * fenetre.
 *
 * Aligne doc 3038018. */
static Value fn_ferme(int argCount, Value* args) {
    /* val_index : indice du 1er argument "valeur renvoyee". 0 = arg[0] est
     * deja une valeur (cas non-string). 1 = arg[0] est un nom/ref de fenetre,
     * la valeur eventuelle suit. */
    int val_index = 0;
    if (argCount >= 1 && IS_STRING(args[0])) {
        const char* s = AS_CSTRING(args[0]);
        /*  — la chaine vide vise explicitement la fenetre courante,
         * AVANT la garde "fenetre inconnue" (sinon "" leverait l'erreur car
         * vm_window_lookup("") == NULL). Preserve l'idiome Ferme("", val). */
        if (s[0] == '\0') {
            val_index = 1;
        } else if (vm_window_lookup(s) != NULL) {
            val_index = 1;
        } else {
            /*  — nom non vide qui ne correspond a aucune fenetre :
             * plus d'heuristique val-de-retour, on s'aligne sur le canon
             * (le 1er arg chaine est un nom de fenetre). Soft error, on ne
             * ferme rien (la fenetre visee n'existe pas) et on laisse
             * g_return_value intact. */
            owl_emit_native_error(E_RT_UI_WINDOW_NOT_FOUND, "Ferme",
                                  "name", s, NULL);
            return NIL_VAL;
        }
    } else if (argCount >= 1 && IS_WINDOW_REF(args[0])) {
        /*  lot B — 'Ferme(MaFenetre, val)' ou 'Ferme(FEN_X, val)' :
         * la fenetre est resolue comme ObjWindowRef (proxy global pose au
         * compile-time par window_declaration). C'est la fenetre courante
         * a fermer ; les args restants sont les valeurs renvoyees. */
        val_index = 1;
    }
    /*  — on hoiste la valeur de retour (et sa coercion) SANS la poser
     * ici. Une fermeture refusee par AutorisationFermeture ne doit pas laisser
     * la valeur posee : la pose est deplacee APRES le veto d'autorisation, mais
     * AVANT Fermeture (les deux chemins headless/GUI ci-dessous), pour que le
     * handler Fermeture voie toujours la valeur sur une fermeture autorisee. */
    bool hasValue = (argCount > val_index);
    Value v = NIL_VAL;
    if (hasValue) {
        v = args[val_index];
        /* Phase E.X : si la valeur passee est un ObjUiControl (ex.
         * 'Ferme(saiUser)'), on doit coercer immediatement vers la valeur
         * par defaut AVANT que le widget Qt soit detruit (sinon dangling
         * pointer sur lecture ulterieure). */
        if (IS_UI_CONTROL(v)) {
            ObjString* s = value_to_string(v);
            v = s ? OBJ_VAL(s) : NIL_VAL;
        }
    }
    /*  — parite headless : AutorisationFermeture() puis Fermeture()
     * de la fenetre, declenchees AVANT le pop de l'instance pour que les
     * handlers puissent encore lire l'etat des champs, et pour qu'un refus
     * laisse la fenetre intacte.
     *
     *  — en GUI les deux events sont portes par le QEvent::Close (cf.
     * build_window), afin que la case de fermeture de la fenetre emprunte
     * exactement la meme porte que Ferme(). Il faut donc fermer le widget de
     * l'INSTANCE fermee : sur le chemin non-modal (OuvreSoeur/OuvreFille),
     * g_current_window a deja ete restaure a la fenetre appelante par
     * open_window_instance et ne designe plus la fenetre a fermer — c'est ce
     * qui rendait Fermeture muet en mode graphique. */
    ObjWindowInstance* top = vm_window_stack_top();
    if (vm.headless_mode) {
        Value auth = fire_window_lifecycle_event(top, "AutorisationFermeture",
                                                 "CloseAuthorization");
        if (IS_BOOL(auth) && !AS_BOOL(auth)) return NIL_VAL;
        /*  — autorisation acquise : on pose la valeur AVANT Fermeture
         * pour que le handler puisse la lire (le refus ci-dessus n'a rien pose). */
        if (hasValue) g_return_value = v;
        fire_window_lifecycle_event(top, "Fermeture", "Closing");
    } else {
        OwlWindow* target = (top != NULL && top->widget != NULL)
                            ? (OwlWindow*)top->widget
                            : g_current_window;
        /*  — en GUI, owl_ui_window_close porte AutorisationFermeture ET
         * Fermeture sur le QEvent::Close ; Fermeture doit voir la
         * valeur, donc on la pose AVANT l'appel. Si la fermeture est refusee, on
         * restaure la valeur precedente : un refus ne doit rien poser. */
        Value prev_ret = g_return_value;
        if (hasValue) g_return_value = v;
        if (target != NULL && !owl_ui_window_close(target)) {
            g_return_value = prev_ret;   /* refus : la valeur n'est pas posee */
            return NIL_VAL;
        }
    }

    /*  — Modele C : pop l'instance courante de vm.window_stack
     * AVANT toute teardown Qt. Les acces post-Ferme aux champs deviennent
     * alors des soft errors via resolve_ui_ref(). */
    ObjWindowInstance* closed = NULL;
    if (vm_window_stack_top() != NULL) {
        closed = vm_window_stack_pop();
    }
    /*  — annule les setFocus differes en vol de l'instance fermee AVANT
     * le teardown. Cible = le QMainWindow de l'instance fermee (closed->widget),
     * PAS g_current_window : sur le chemin non-modal (OuvreSoeur/OuvreFille),
     * open_window_instance restaure g_current_window = prev_window, donc il ne
     * pointe deja plus vers la fenetre qu'on ferme. Sans cette annulation, un
     * DonneFocus(X) suivi d'un Ferme() immediat laisse le QTimer 0ms tirer
     * pendant un pump ulterieur -> FocusOut/FocusIn -> surSortie/surEntree sur
     * un binding OWL deja demonte (use-after-free / contamination cross-instance). */
    if (closed != NULL && closed->widget != NULL) {
        owl_ui_cancel_focus_timers((OwlWindow*)closed->widget);
        /*  — detache aussi la fenetre du systeme de focus (purge
         * dispatcher + clearFocus) : un champ qui garde le focus Qt apres
         * fermeture dispatche un FocusOut residuel quand la fenetre suivante
         * vole le focus, sur un binding reutilise -> contamination. */
        owl_ui_purge_window_focus((OwlWindow*)closed->widget);
    }
    /*  — invalide les pointeurs widgets de l'instance fermee. Le
     * widget Qt cesse d'etre une cible de lecture valide des l'instant ou la
     * fenetre est fermee (en GUI owl_ui_window_close ne fait qu'un hide, mais
     * le widget peut etre detruit ulterieurement par teardown parent). Le
     * live-read  (try_live_qt_prop_for_uici) garde sur widget!=NULL :
     * remettre ces pointeurs a NULL garantit qu'une reference residuelle a
     * l'instance fermee retombe sur le store (soft, jamais un widget mort).
     * Sans cela, ouvrir une fenetre, la fermer, puis re-lire un de ses champs
     * via une ref retenue lisait un QWidget potentiellement detache. */
    if (closed != NULL) {
        /*  — purge les watches instance->widget / widget->racine
         * style AVANT d'invalider les pointeurs : les QWidget de cette fenetre
         * cessent d'etre des cibles de re-apply valides. Evite tout deref
         * ulterieur d'un widget mort via une entree de watch obsolete. */
        for (int i = 0; i < closed->control_count; i++) {
            ObjUiControlInstance* uici = closed->controls[i];
            if (uici != NULL && uici->widget != NULL)
                widget_watches_purge((OwlControl*)uici->widget);
        }
        closed->widget = NULL;
        for (int i = 0; i < closed->control_count; i++) {
            ObjUiControlInstance* uici = closed->controls[i];
            if (uici != NULL) uici->widget = NULL;
        }
    }
    owl_ui_quit();
    return NIL_VAL;
}

/* DemandeMiseAJourIHM() — declencheur explicite de l'event DemandeMiseAJour de
 * la fenetre courante. Sans fenetre ouverte, ou si la fenetre ne declare pas cet
 * event : no-op silencieux. L'execution est synchrone (OWL n'a pas de thread UI
 * distinct) ; les parametres optionnels du canon ne sont pas exposes, cf.
 * W_SEM_PREDEFINED_BLOCK_PARAMS_IGNORED. */
static Value fn_demande_mise_a_jour_ihm(int argCount, Value* args) {
    (void)argCount; (void)args;
    fire_window_lifecycle_event(vm_window_stack_top(),
                                "DemandeMiseAJour", "RequestRefresh");
    return NIL_VAL;
}

/* FinProgramme - implem partielle (exit + message d'erreur optionnel). */
static Value fn_fin_programme(int argCount, Value* args) {
    int last_msg = argCount;
    bool exec_close = false;
    if (argCount >= 1 && IS_BOOL(args[argCount - 1])) {
        exec_close = AS_BOOL(args[argCount - 1]);
        last_msg = argCount - 1;
    }
    if (last_msg > 0) {
        char* msg = concat_messages(last_msg, args, 0);
        if (msg && *msg) owl_ui_message_error("Fin du programme", msg);
        free(msg);
    }
    (void)exec_close;
    exit(0);
    return NIL_VAL;
}

/* ====================================================================
 *  Champ Table (Phase 2) — TableAjouteLigne / TableSupprime / etc.
 * ==================================================================== */

/* Resout arg[0] vers le widget Qt d'un Champ Table. Accepte 3 formes :
 *   (1) ObjUiControl direct : forme historique pre- (encore
 *       posee par certains chemins). Vérifie le kind.
 *   (2) ObjUiControlRef : proxy compile-time pose par  — c'est
 *       la forme dominante apres  (les identifiants TABLE_X
 *       resolvent vers un proxy dans vm.globals). Resolu via
 *       resolve_ui_ref (vm.h) qui consulte window_stack puis etat_stack.
 *   (3) ObjString (nom) : la doc owl explain promet "reference ou nom".
 *       Lookup case-sensitive dans la fenetre top-of-stack.
 * Retourne NULL apres log stderr si aucune forme ne resout ou si le
 * controle resolu n'est pas un Champ Table. Sous owl test (vm.headless_mode),
 * uici->widget est NULL car build_window n'est pas execute : retourne NULL
 * SANS erreur — les natives owl_ui_table_* guardent toutes contre NULL et
 * deviennent no-op silencieuses, comportement coherent headless. */
static OwlControl* table_arg_or_error(Value v, const char* fnName) {
    /* Cas 1 : ObjUiControl direct. */
    if (IS_UI_CONTROL(v)) {
        ObjUiControl* uic = AS_UI_CONTROL(v);
        if ((OwlControlKind)uic->kind != OWL_CONTROL_TABLE) {
            /*  — owl_emit_native_error pour poser ErreurDetectee. */
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                                  "arg", "1",
                                  "expected", "Champ Table",
                                  "actual", "type de champ incompatible",
                                  NULL);
            return NULL;
        }
        return (OwlControl*)uic->widget;
    }
    /* Cas 2 : ObjUiControlRef (proxy). */
    if (IS_UI_CONTROL_REF(v)) {
        ObjUiControlInstance* uici = resolve_ui_ref(AS_UI_CONTROL_REF(v));
        if (!uici) {
            /* resolve_ui_ref a deja emis E_RT_UI_FIELD_* approprie. */
            return NULL;
        }
        if (uici->decl && uici->decl->kind != OWL_CONTROL_TABLE) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                                  "arg", "1",
                                  "expected", "Champ Table",
                                  "actual", "controle reference n'est pas un Champ Table",
                                  NULL);
            return NULL;
        }
        return (OwlControl*)uici->widget;  /* peut etre NULL en headless */
    }
    /*  — Cas 2bis : ObjUiControlInstance directe (cas exotique mais
     * passe au dispatch methode dans certains paths headless). */
    if (IS_UI_CONTROL_INSTANCE(v)) {
        ObjUiControlInstance* uici = AS_UI_CONTROL_INSTANCE(v);
        if (uici->decl && uici->decl->kind != OWL_CONTROL_TABLE) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                                  "arg", "1",
                                  "expected", "Champ Table",
                                  "actual", "controle reference n'est pas un Champ Table",
                                  NULL);
            return NULL;
        }
        return (OwlControl*)uici->widget;
    }
    /* Cas 3 : nom (chaine). */
    if (IS_STRING(v)) {
        const char* name = AS_CSTRING(v);
        ObjUiControlInstance* uici = current_window_uici_by_name(name);
        if (!uici) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                                  "arg", "1",
                                  "expected", "Champ Table existant",
                                  "actual", name,
                                  NULL);
            return NULL;
        }
        if (uici->decl && uici->decl->kind != OWL_CONTROL_TABLE) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                                  "arg", "1",
                                  "expected", "Champ Table",
                                  "actual", name,
                                  NULL);
            return NULL;
        }
        return (OwlControl*)uici->widget;
    }
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                          "arg", "1",
                          "expected", "Champ Table (reference ou nom)",
                          "actual", "valeur incompatible",
                          NULL);
    return NULL;
}

/*  — store de lignes headless Table (, calque ZR ).
 * Resout l'uici Table depuis l'argument receveur (Ref / Instance / nom), puis
 * maintient l'ObjArray d'items (OWL_UI_LIST_ITEMS_KEY) : une rep ObjInstance
 * par ligne, champ indexe par NOM de colonne, Value typee (la Value d'origine
 * passee a la native, pas value_to_string). Le widget Qt reste source de
 * verite pour l'affichage ; ce store sert la lecture cellule par programme
 * (TBL[ligne].COL via widget_instance_get_subitem). Definis apres les helpers
 * de store ci-dessous. */
static ObjUiControlInstance* table_uici_from_arg(Value v);
static void table_store_append_row(ObjUiControlInstance* uici,
                                    Value* values, int n);
static void table_store_set_row(ObjUiControlInstance* uici, int row,
                                Value* values, int n);
static void table_store_remove_row(ObjUiControlInstance* uici, int row);
static void table_store_clear(ObjUiControlInstance* uici);
static ObjArray* widget_items_store_peek_uici(ObjUiControlInstance* uici);
static void table_widget_set_row_from_rep(OwlControl* table,
                                          ObjUiControlInstance* uici, int row);

/*  — declenche surAffichageLigne(indice) sur un champ Table, calque
 * du pattern ZoneRepetee (zr_fire_row_display). L'indice est 1-based. No-op
 * si l'event n'est pas declare. */
static void ui_fire_table_row_display(ObjUiControlInstance* uici, int idx_1based) {
    if (!uici || !uici->decl || idx_1based < 1) return;
    CallbackSource src = CB_SOURCE_NONE;
    ObjFunction* fn = find_callback_fn(uici->decl, "surAffichageLigne",
                                       "onRowDisplay", &src);
    if (!fn) return;
    Value sender = moimeme_widget_value_for_name(uici->decl->name);
    Value payload[1] = { INTEGER_VAL(idx_1based) };
    invoke_callback_payload_with_sender(fn, src, payload, 1, sender);
}

static Value fn_table_ajoute_ligne(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[TableAjouteLigne] argument(s) attendu(s).\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableAjouteLigne");
    /* Note : table peut etre NULL en headless (pas de widget Qt) sans que ce
     * soit une erreur — table_arg_or_error retourne uici->widget. On distingue
     * l'erreur de type via le store : si l'uici est resolvable, on continue. */
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return NIL_VAL;
    int n = argCount - 1;
    if (n <= 0) {
        if (table) owl_ui_table_append_row(table, NULL, 0);
        table_store_append_row(uici, NULL, 0);
        ObjArray* items0 = widget_items_store_peek_uici(uici);
        if (items0) ui_fire_table_row_display(uici, items0->elements.count);
        return NIL_VAL;
    }
    /* Store headless : rep ObjInstance typee (Value d'origine). */
    table_store_append_row(uici, &args[1], n);
    if (!table) {
        /*  — surAffichageLigne(indice) au rendu de la ligne (calque
         * ZR). Indice 1-based = nouveau compte du store. */
        ObjArray* items = widget_items_store_peek_uici(uici);
        if (items) ui_fire_table_row_display(uici, items->elements.count);
        return NIL_VAL;
    }
    const char** cells = (const char**)malloc((size_t)n * sizeof(char*));
    if (!cells) {
        fprintf(stderr, "[TableAjouteLigne] allocation echouee.\n");
        return NIL_VAL;
    }
    /* keep[] retient les ObjString temporaires (value_to_string) le temps
     * de l'appel a owl_ui_table_append_row qui copie dans QString. */
    ObjString** keep = (ObjString**)calloc((size_t)n, sizeof(ObjString*));
    for (int i = 0; i < n; i++) {
        Value v = args[i + 1];
        if (IS_STRING(v)) {
            cells[i] = AS_CSTRING(v);
        } else {
            ObjString* s = value_to_string(v);
            if (keep) keep[i] = s;
            cells[i] = s ? s->chars : "";
        }
    }
    owl_ui_table_append_row(table, cells, n);
    free(cells);
    free(keep);
    /*  — surAffichageLigne(indice) apres rendu de la ligne Qt. */
    {
        ObjArray* items = widget_items_store_peek_uici(uici);
        if (items) ui_fire_table_row_display(uici, items->elements.count);
    }
    return NIL_VAL;
}

static Value fn_table_supprime(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[TableSupprime] (table, ligne) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableSupprime");
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[TableSupprime] ligne doit etre un entier.\n");
        return NIL_VAL;
    }
    int row = (int)AS_INTEGER(args[1]);
    table_store_remove_row(uici, row);
    if (table) owl_ui_table_remove_row(table, row);
    return NIL_VAL;
}

static Value fn_table_supprime_tout(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[TableSupprimeTout] (table) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableSupprimeTout");
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return NIL_VAL;
    table_store_clear(uici);
    if (table) owl_ui_table_clear(table);
    return NIL_VAL;
}

/* Une rep de ligne (ObjInstance du store) est VIDE ssi toutes ses cellules
 * de colonnes declarees sont des chaines vides. >= 1 cellule a contenu
 * (chaine non vide, OU valeur non-chaine : entier, booleen...) => non vide.
 * Le champ back-ref OWL_TABLE_REP_SRC_KEY est ignore (ce n'est pas une
 * cellule). Cle de cellule = nom de colonne accents strippes. */
static bool table_rep_is_empty(ObjUiControlInstance* uici, ObjInstance* rep) {
    if (!rep) return true;
    const OwlControlDecl* tdecl = (uici && uici->decl) ? uici->decl : NULL;
    int ncol = tdecl ? tdecl->column_count : 0;
    for (int i = 0; i < ncol; i++) {
        const char* colName = tdecl->columns[i].name;
        if (!colName) continue;
        char colKey[256];
        int klen = scanner_strip_accents(colName, (int)strlen(colName),
                                         colKey, (int)sizeof(colKey));
        if (klen <= 0 || klen >= (int)sizeof(colKey)) continue;
        colKey[klen] = '\0';
        Value cell;
        if (!instance_get_value(rep, colKey, &cell)) continue;
        if (IS_STRING(cell)) {
            if (AS_STRING(cell)->length > 0) return false;  /* chaine a contenu */
        } else if (!IS_NIL(cell)) {
            return false;  /* valeur non-chaine, non-nulle => contenu */
        }
    }
    return true;
}

/* TableOccurrence(table [, info]) — canon 3074021.
 *   info = toTotal (default) : nombre total de lignes en memoire
 *        = toAffichee : nombre de lignes NON VIDES
 *        = toVisible  : assume egal a toTotal en V1 (cf. ci-dessous)
 *        = toColonne  : nombre de colonnes declarees
 *
 * Chemin widget (GUI reel) : lit le modele Qt via owl_ui_table_row_count /
 * column_count. Chemin headless (widget absent, cas owl test) : lit le store
 * de lignes (ObjArray de reps), sinon la native serait un no-op non observable.
 *
 * Divergence assumee (V1) : toAffichee laisse tomber le filtre geometrique
 * "partie visible" du canon — impossible a calculer sans fenetre rendue et
 * dimensionnee (meme mur que toVisible). On compte donc les lignes non vides
 * tout court, pas seulement celles dans le viewport. Strictement mieux que
 * l'ancien comportement qui comptait aussi les lignes vides. toVisible reste
 * aligne sur toTotal pour la meme raison (capacite d'affichage non calculable
 * headless). */
static Value fn_table_occurrence(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[TableOccurrence] (table [, info]) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableOccurrence");
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return NIL_VAL;
    int info = OWL_TO_TOTAL;
    if (argCount >= 2 && IS_INTEGER(args[1])) info = (int)AS_INTEGER(args[1]);

    if (info == OWL_TO_COLONNE) {
        if (table) return INTEGER_VAL(owl_ui_table_column_count(table));
        return INTEGER_VAL(uici->decl ? uici->decl->column_count : 0);
    }

    if (info == OWL_TO_AFFICHEE) {
        /* Lignes non vides, comptees depuis le store (source de verite des
         * donnees, coherente widget et headless). */
        ObjArray* items = widget_items_store_peek_uici(uici);
        int count = 0;
        if (items) {
            for (int i = 0; i < items->elements.count; i++) {
                Value it = items->elements.values[i];
                ObjInstance* rep = IS_INSTANCE(it) ? AS_INSTANCE(it) : NULL;
                if (!table_rep_is_empty(uici, rep)) count++;
            }
        }
        return INTEGER_VAL(count);
    }

    /* toTotal / toVisible : nombre total de lignes en memoire. */
    if (table) return INTEGER_VAL(owl_ui_table_row_count(table));
    ObjArray* items = widget_items_store_peek_uici(uici);
    return INTEGER_VAL(items ? items->elements.count : 0);
}

/* Phase 3 — TableModifieLigne(table, ligne, val1, val2, ...) */
static Value fn_table_modifie_ligne(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[TableModifieLigne] (table, ligne, valeurs...) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableModifieLigne");
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[TableModifieLigne] ligne doit etre un entier (1-based).\n");
        return NIL_VAL;
    }
    int row = (int)AS_INTEGER(args[1]);
    int n = argCount - 2;
    if (n <= 0) return NIL_VAL;
    /* Store headless : remplace la rep de la ligne (Value typee d'origine). */
    table_store_set_row(uici, row, &args[2], n);
    if (!table) return NIL_VAL;
    const char** cells = (const char**)malloc((size_t)n * sizeof(char*));
    ObjString** keep = (ObjString**)calloc((size_t)n, sizeof(ObjString*));
    if (!cells) {
        fprintf(stderr, "[TableModifieLigne] allocation echouee.\n");
        return NIL_VAL;
    }
    for (int i = 0; i < n; i++) {
        Value v = args[i + 2];
        if (IS_STRING(v)) {
            cells[i] = AS_CSTRING(v);
        } else {
            ObjString* s = value_to_string(v);
            if (keep) keep[i] = s;
            cells[i] = s ? s->chars : "";
        }
    }
    owl_ui_table_set_row(table, row, cells, n);
    free(cells);
    free(keep);
    return NIL_VAL;
}

/* TableEchangeLigne(table, indiceSource, indiceDestination) — canon
 * 1000020627 (EN TableSwapLine). Echange (permutation) deux lignes identifiees
 * par leur numero 1-based. Le nom des parametres ("a deplacer" / "destination")
 * suggere un deplacement, mais la description ("Echange deux lignes") et
 * l'exemple ("On echange les lignes 5 et 20") tranchent : PERMUTATION stricte
 * des deux lignes, les autres ne bougent pas. Le move avec decalage est porte
 * par une fonction distincte au canon (TableDeplaceLigne).
 * Indice hors plage : le canon leve une erreur fatale ; OWL degrade en
 * soft-error E_RT_UI_ROW_OOB (pose ErreurDetectee, echange avorte, la VM
 * continue), alignee sur le contrat  partage par les autres acces
 * ligne (lecture cellule hors plage, TABLE_X[oob]). */
static Value fn_table_echange_ligne(int argCount, Value* args) {
    if (argCount < 3) {
        fprintf(stderr, "[TableEchangeLigne] (table, indiceSource, indiceDestination) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableEchangeLigne");
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return NIL_VAL;
    if (!IS_INTEGER(args[1]) || !IS_INTEGER(args[2])) {
        fprintf(stderr, "[TableEchangeLigne] indices doivent etre des entiers (1-based).\n");
        return NIL_VAL;
    }
    int a = (int)AS_INTEGER(args[1]);
    int b = (int)AS_INTEGER(args[2]);
    /* Le store est maintenu dans les deux modes (headless ET widget) : c'est
     * la source de verite du nombre de lignes. */
    ObjArray* items = widget_items_store_peek_uici(uici);
    int count = items ? items->elements.count
                      : (table ? owl_ui_table_row_count(table) : 0);
    char idxBuf[16], maxBuf[16];
    snprintf(maxBuf, sizeof maxBuf, "%d", count);
    if (a < 1 || a > count) {
        snprintf(idxBuf, sizeof idxBuf, "%d", a);
        owl_emit_native_error(E_RT_UI_ROW_OOB, "Table",
                              "control", "Table", "index", idxBuf,
                              "max", maxBuf, NULL);
        return NIL_VAL;
    }
    if (b < 1 || b > count) {
        snprintf(idxBuf, sizeof idxBuf, "%d", b);
        owl_emit_native_error(E_RT_UI_ROW_OOB, "Table",
                              "control", "Table", "index", idxBuf,
                              "max", maxBuf, NULL);
        return NIL_VAL;
    }
    if (a == b) return NIL_VAL;  /* permuter une ligne avec elle-meme = no-op */
    if (items) {
        Value tmp = items->elements.values[a - 1];
        items->elements.values[a - 1] = items->elements.values[b - 1];
        items->elements.values[b - 1] = tmp;
    }
    /* Widget present (GUI reel) : refleter le swap en reecrivant les deux
     * lignes Qt depuis les reps du store deja permutees. */
    if (table && items) {
        table_widget_set_row_from_rep(table, uici, a);
        table_widget_set_row_from_rep(table, uici, b);
    }
    return NIL_VAL;
}

/* TableDeplaceLigne(table, source, destination [, operation]) — canon
 * 1000017247 (EN TableMoveLine). Deplace une ligne vers une destination AVEC
 * decalage des lignes intermediaires (move), par opposition a la permutation
 * stricte de TableEchangeLigne. La destination prend deux formes tranchees sur
 * le canon :
 *   - un indice entier 1-based             -> destination absolue (SYNTAXE1) ;
 *   - une cible relative tdXxx (sentinelle) -> deplacement relatif (SYNTAXE2).
 * La distinction se fait par la VALEUR du 3e parametre : les cibles relatives
 * sont des sentinelles OWL_TD_* hors de la plage des indices reels — meme
 * mecanique que ListeDeplaceLigne. <Operation> est un bitmask combinable :
 * tdDeplace=0 (defaut, move+decalage), tdEchange=1 (permutation), tdAffiche=2
 * (defilement, sans effet en headless). Retour : nouvel indice 1-based de la
 * ligne deplacee (0 si refus). Indice hors plage : soft-error E_RT_UI_ROW_OOB
 * (comme TableEchangeLigne, contrat ). */
static Value fn_table_deplace_ligne(int argCount, Value* args) {
    if (argCount < 3) {
        fprintf(stderr, "[TableDeplaceLigne] (table, source, destination [, op]) attendu.\n");
        return INTEGER_VAL(0);
    }
    OwlControl* table = table_arg_or_error(args[0], "TableDeplaceLigne");
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return INTEGER_VAL(0);
    if (!IS_INTEGER(args[1]) || !IS_INTEGER(args[2])) {
        fprintf(stderr, "[TableDeplaceLigne] source et destination doivent etre entier.\n");
        return INTEGER_VAL(0);
    }
    ObjArray* items = widget_items_store_peek_uici(uici);
    int count = items ? items->elements.count
                      : (table ? owl_ui_table_row_count(table) : 0);

    /* Resolution de la source : indice 1-based ou ligneSelectionnee. */
    int src = (int)AS_INTEGER(args[1]);
    if (src == OWL_LIGNE_SELECTIONNEE)
        src = table ? owl_ui_table_get_selected_row_n(table, 1) : 0;

    char idxBuf[16], maxBuf[16];
    snprintf(maxBuf, sizeof maxBuf, "%d", count);
    if (src < 1 || src > count) {
        snprintf(idxBuf, sizeof idxBuf, "%d", src);
        owl_emit_native_error(E_RT_UI_ROW_OOB, "Table",
                              "control", "Table", "index", idxBuf,
                              "max", maxBuf, NULL);
        return INTEGER_VAL(0);
    }

    /* Resolution de la destination : indice absolu ou cible relative tdXxx.
     * Les cibles relatives sont relatives a la position de la source. */
    int dst = (int)AS_INTEGER(args[2]);
    if (dst == OWL_LIGNE_SELECTIONNEE)
        dst = table ? owl_ui_table_get_selected_row_n(table, 1) : 0;
    else if (dst == OWL_TD_PREMIER)          dst = 1;
    else if (dst == OWL_TD_DERNIER)          dst = count;
    else if (dst == OWL_TD_PRECEDENT)        dst = (src > 1) ? src - 1 : 1;
    else if (dst == OWL_TD_SUIVANT)          dst = (src < count) ? src + 1 : count;
    else if (dst == OWL_TD_PAGE_PRECEDENTE)  dst = (src > 10) ? src - 10 : 1;
    else if (dst == OWL_TD_PAGE_SUIVANTE)    dst = (src + 10 <= count) ? src + 10 : count;

    if (dst < 1 || dst > count) {
        snprintf(idxBuf, sizeof idxBuf, "%d", dst);
        owl_emit_native_error(E_RT_UI_ROW_OOB, "Table",
                              "control", "Table", "index", idxBuf,
                              "max", maxBuf, NULL);
        return INTEGER_VAL(0);
    }
    if (src == dst) return INTEGER_VAL(src);  /* no-op observable */

    int op_bits = (argCount >= 4 && IS_INTEGER(args[3])) ? (int)AS_INTEGER(args[3]) : 0;
    bool echange = (op_bits & OWL_TD_ECHANGE) != 0;

    if (items) {
        Value* vals = items->elements.values;
        if (echange) {
            Value tmp = vals[src - 1];
            vals[src - 1] = vals[dst - 1];
            vals[dst - 1] = tmp;
        } else {
            /* Move+decalage : on retire la rep source et on la reinsere a dst ;
             * l'intervalle glisse d'un cran. */
            Value moved = vals[src - 1];
            if (src < dst)
                memmove(&vals[src - 1], &vals[src], (size_t)(dst - src) * sizeof(Value));
            else
                memmove(&vals[dst], &vals[dst - 1], (size_t)(src - dst) * sizeof(Value));
            vals[dst - 1] = moved;
        }
    }
    /* Widget present (GUI reel) : reecrire les lignes Qt touchees depuis les
     * reps du store deja reordonnees. Move = tout l'intervalle ; swap = 2 lignes. */
    if (table && items) {
        if (echange) {
            table_widget_set_row_from_rep(table, uici, src);
            table_widget_set_row_from_rep(table, uici, dst);
        } else {
            int lo = (src < dst) ? src : dst;
            int hi = (src < dst) ? dst : src;
            for (int r = lo; r <= hi; r++)
                table_widget_set_row_from_rep(table, uici, r);
        }
    }
    return INTEGER_VAL(dst);
}

/* TableSelectPlus(table, ligne) — canon 3074031.
 * Selectionne la ligne (1-based). En multi-select, ajoute a la selection. */
static Value fn_table_select_plus(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[TableSelectPlus] (table, ligne) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableSelectPlus");
    if (!table) return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[TableSelectPlus] ligne doit etre un entier (1-based).\n");
        return NIL_VAL;
    }
    owl_ui_table_select_row(table, (int)AS_INTEGER(args[1]));
    return NIL_VAL;
}

/* TableSelectMoins(table, ligne) — canon 3074029.
 * Deselectionne la ligne (1-based). */
static Value fn_table_select_moins(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[TableSelectMoins] (table, ligne) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableSelectMoins");
    if (!table) return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[TableSelectMoins] ligne doit etre un entier (1-based).\n");
        return NIL_VAL;
    }
    owl_ui_table_deselect_row(table, (int)AS_INTEGER(args[1]));
    return NIL_VAL;
}

/* TableSelect(table [, rang [, info]]) — canon 3074020.
 *   rang : index multi-select (defaut 1 = 1ere ligne selectionnee)
 *   info : tsLigne (default), tsColonne, tsLigneCellule, tsColonneCellule */
static Value fn_table_select(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[TableSelect] (table [, rang [, info]]) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableSelect");
    if (!table) return NIL_VAL;
    int rang = 1;
    int info = OWL_TS_LIGNE;
    if (argCount >= 2 && IS_INTEGER(args[1])) rang = (int)AS_INTEGER(args[1]);
    if (argCount >= 3 && IS_INTEGER(args[2])) info = (int)AS_INTEGER(args[2]);
    if (info == OWL_TS_COLONNE || info == OWL_TS_COLONNE_CELLULE) {
        return INTEGER_VAL(owl_ui_table_get_selected_col(table, rang));
    }
    return INTEGER_VAL(owl_ui_table_get_selected_row_n(table, rang));
}

/* TableSelectOccurrence(table) — canon 3074030.
 * Nombre de lignes selectionnees. */
static Value fn_table_select_occurrence(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[TableSelectOccurrence] (table) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableSelectOccurrence");
    if (!table) return NIL_VAL;
    return INTEGER_VAL(owl_ui_table_selection_count(table));
}

/* Resout un proxy-colonne (TABLE.COL) — defini plus bas (partage avec
 * TableCherche). */
static bool table_colproxy_resolve(Value v, ObjUiControlInstance** uiciOut,
                                   char* colOut, size_t colCap);
/*  — helpers du store de filtres headless (definis apres
 * table_colproxy_resolve). */
static void table_filter_set_headless(ObjUiControlInstance* uici,
                                      const char* col, int type,
                                      const char* valeur);
/*  — pousse le jeu complet des filtres actifs vers le rendu Qt
 * (recompute ET). Defini apres les helpers du store. */
static void table_filters_apply_to_qt(ObjUiControlInstance* uici);
/*  — index 0-based (ordre declare) d'une colonne par nom. */
static int table_column_index_of(ObjUiControlInstance* uici, const char* col);

/* TableActiveFiltre(TAB.COL, type, valeur) — canon 1000017149.
 * Forme proxy-colonne : arg[0] est TABLE.COL. L'etat du filtre vit
 * dans le store headless (interrogeable via TableColonnesFiltrees), en plus de
 * l'application Qt (masquage visuel, rendu). valeur vide ("") = retire le
 * filtre. */
static Value fn_table_active_filtre(int argCount, Value* args) {
    if (argCount < 3) {
        fprintf(stderr,
            "[TableActiveFiltre] (colonne, type, valeur) attendu "
            "(forme TABLE.COL_X).\n");
        return NIL_VAL;
    }
    ObjUiControlInstance* uici = NULL;
    char col[256];
    if (!table_colproxy_resolve(args[0], &uici, col, sizeof col)) {
        fprintf(stderr,
            "[TableActiveFiltre] 1er argument : colonne de Table attendue "
            "(forme TABLE.COL_X).\n");
        return NIL_VAL;
    }
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[TableActiveFiltre] type doit etre une constante filtre*.\n");
        return NIL_VAL;
    }
    int filtreType = (int)AS_INTEGER(args[1]);
    const char* valeur = "";
    if (IS_STRING(args[2])) {
        valeur = AS_CSTRING(args[2]);
    } else if (!IS_NIL(args[2])) {
        ObjString* s = value_to_string(args[2]);
        valeur = s ? s->chars : "";
    }
    /* Copie locale : les set headless allouent (grow -> GC) et invalideraient
     * une ObjString fraiche non rootee. */
    char valbuf[512];
    snprintf(valbuf, sizeof valbuf, "%s", valeur);

    /* Etat headless interrogeable (TableColonnesFiltrees / TableDesactiveFiltre). */
    table_filter_set_headless(uici, col, filtreType, valbuf);

    /* Application Qt (masquage visuel) : recompute la visibilite en ET du jeu
     * complet de filtres. No-op si pas de widget (headless). */
    table_filters_apply_to_qt(uici);
    return NIL_VAL;
}

/* TableSaisieLoupe(TAB.COL [, type [, valeur]]) — canon 3074024.
 * Declenche par programme la saisie loupe d'une colonne, comme un clic
 * utilisateur sur l'icone loupe : passe le titre de colonne en saisie inline.
 *   - type = constante filtre* + valeur non vide : applique le filtre (meme
 *     store $tblfilters que TableActiveFiltre -> reflete par
 *     TableColonnesFiltrees, masquage Qt) et preremplit le champ ;
 *   - type = constante recherche* (ou defaut rechercheDefaut, ou absent) : passe
 *     en mode recherche (tri + positionnement, volet visuel non persiste).
 * Annulee par TableDesactiveFiltre (canon). Colonne en forme proxy. */
static Value fn_table_saisie_loupe(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr,
            "[TableSaisieLoupe] (colonne [, type [, valeur]]) attendu "
            "(forme TABLE.COL_X).\n");
        return NIL_VAL;
    }
    ObjUiControlInstance* uici = NULL;
    char col[256];
    if (!table_colproxy_resolve(args[0], &uici, col, sizeof col)) {
        fprintf(stderr,
            "[TableSaisieLoupe] 1er argument : colonne de Table attendue "
            "(forme TABLE.COL_X).\n");
        return NIL_VAL;
    }

    int type = OWL_RCHT_DEFAUT;
    if (argCount >= 2 && IS_INTEGER(args[1])) type = (int)AS_INTEGER(args[1]);

    /* Copie locale : les set headless allouent (grow -> GC) et invalideraient
     * une ObjString fraiche non rootee (meme garde que TableActiveFiltre). */
    char valbuf[512];
    valbuf[0] = '\0';
    if (argCount >= 3) {
        if (IS_STRING(args[2])) {
            snprintf(valbuf, sizeof valbuf, "%s", AS_CSTRING(args[2]));
        } else if (!IS_NIL(args[2])) {
            ObjString* s = value_to_string(args[2]);
            snprintf(valbuf, sizeof valbuf, "%s", s ? s->chars : "");
        }
    }

    /* filtre* = les 12 operateurs contigus (sentinels monotones). recherche* et
     * le defaut ne masquent rien : positionnement visuel uniquement. */
    bool isFilter = (type >= OWL_FILTRE_CONTIENT
                     && type <= OWL_FILTRE_NE_TERMINE_PAS);

    /* Volet interrogeable HEADLESS : un filtre* + valeur pose (ou retire, si
     * valeur vide) le filtre dans le store partage puis recompute Qt. */
    if (isFilter) {
        table_filter_set_headless(uici, col, type, valbuf);
        table_filters_apply_to_qt(uici);
    }

    /* Volet visuel : ouvre l'editeur inline du titre de colonne (comme un clic
     * loupe). forcedOp = l'operateur filtre* si fourni (la frappe le suit),
     * sinon -1 (la frappe suit ModeLoupe). No-op headless (pas de widget). */
    if (uici->widget) {
        int col0 = table_column_index_of(uici, col);
        if (col0 >= 0) {
            int forcedOp = isFilter ? type : -1;
            owl_ui_table_open_search((OwlControl*)uici->widget, col0,
                                     forcedOp, valbuf);
        }
    }
    return NIL_VAL;
}

/* TablePosition(table, idx [, option]) — set : scroll a la ligne idx
 * TablePosition(table) — get : retourne 1ere ligne visible (canon 3074022). */
static Value fn_table_position(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[TablePosition] (table [, indice [, option]]) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TablePosition");
    if (!table) return NIL_VAL;
    if (argCount == 1) {
        return INTEGER_VAL(owl_ui_table_get_first_visible_row(table));
    }
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[TablePosition] indice doit etre un entier.\n");
        return NIL_VAL;
    }
    int idx = (int)AS_INTEGER(args[1]);
    int option = OWL_TA_PREMIER;
    if (argCount >= 3 && IS_INTEGER(args[2])) option = (int)AS_INTEGER(args[2]);
    owl_ui_table_set_position(table, idx, option == OWL_TA_VISIBLE);
    return NIL_VAL;
}

/* TableInfoXY(table, type, X, Y) — canon 3074007.
 * Retourne info a la coordonnee pixel (X, Y) relative au champ. */
static Value fn_table_info_xy(int argCount, Value* args) {
    if (argCount < 4) {
        fprintf(stderr, "[TableInfoXY] (table, type, X, Y) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableInfoXY");
    if (!table) return NIL_VAL;
    if (!IS_INTEGER(args[1]) || !IS_INTEGER(args[2]) || !IS_INTEGER(args[3])) {
        fprintf(stderr, "[TableInfoXY] type, X, Y doivent etre entiers.\n");
        return NIL_VAL;
    }
    int type = (int)AS_INTEGER(args[1]);
    int x = (int)AS_INTEGER(args[2]);
    int y = (int)AS_INTEGER(args[3]);
    int row1, col1;
    owl_ui_table_hit_test(table, x, y, &row1, &col1);
    if (type == OWL_TIXY_NUM_LIGNE) {
        return INTEGER_VAL(row1 > 0 ? row1 : -1);
    }
    if (type == OWL_TIXY_NUM_COLONNE) {
        return INTEGER_VAL(col1 > 0 ? col1 : -1);
    }
    if (type == OWL_TIXY_NOM_COLONNE) {
        const char* name = owl_ui_table_get_column_name(table, col1);
        return OBJ_VAL(obj_copy_string(name ? name : "", name ? (int)strlen(name) : 0));
    }
    fprintf(stderr, "[TableInfoXY] type non supporte.\n");
    return INTEGER_VAL(-1);
}

/* TableTrie(table, "+/-NomColonne", ...) — canon 3074035.
 * Le prefix '+' (ou aucun) = tri croissant ; '-' = decroissant.
 * V1 ne gere que la 1ere colonne du tri (multi-colonnes en V1.5+). */
static Value fn_table_trie(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[TableTrie] (table, \"+ou-NomColonne\", ...) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* table = table_arg_or_error(args[0], "TableTrie");
    if (!table) return NIL_VAL;
    if (!IS_STRING(args[1])) {
        fprintf(stderr,
            "[TableTrie] colonne doit etre une chaine '+/-NomColonne'.\n");
        return NIL_VAL;
    }
    const char* spec = AS_CSTRING(args[1]);
    int ascending = 1;
    if (*spec == '-')      { ascending = 0; spec++; }
    else if (*spec == '+') { spec++; }
    owl_ui_table_sort_by_column(table, 0, spec, ascending);
    return NIL_VAL;
}

/*  — resout un proxy-colonne (ObjInstance porteur des marqueurs
 * COLPROXY poses par OP_STRUCT_GET sur TAB.COL) en uici Table + nom de colonne
 * canon (deja accents strippes cote proxy, cf. vm.c ). Retourne false
 * si l'arg n'est pas un proxy-colonne valide sur une Table. */
static bool table_colproxy_resolve(Value v, ObjUiControlInstance** uiciOut,
                                   char* colOut, size_t colCap) {
    if (!IS_INSTANCE(v)) return false;
    ObjInstance* px = AS_INSTANCE(v);
    ObjString* refKey = table_find_key_case_insensitive(
        &px->fields, OWL_TABLE_COLPROXY_REF_KEY);
    Value refV, colV;
    if (refKey == NULL || !table_get(&px->fields, refKey, &refV)
        || !IS_UI_CONTROL_REF(refV)) return false;
    ObjString* colKey = table_find_key_case_insensitive(
        &px->fields, OWL_TABLE_COLPROXY_COL_KEY);
    if (colKey == NULL || !table_get(&px->fields, colKey, &colV)
        || !IS_STRING(colV)) return false;
    ObjUiControlInstance* uici = resolve_ui_ref(AS_UI_CONTROL_REF(refV));
    if (!uici || !uici->decl || uici->decl->kind != OWL_CONTROL_TABLE)
        return false;
    snprintf(colOut, colCap, "%s", AS_CSTRING(colV));
    *uiciOut = uici;
    return true;
}

/*  — store de filtres headless d'un Champ Table.
 * ObjArray de reps ObjInstance nues (une par colonne filtree) dans le
 * headless_store sous OWL_TABLE_FILTERS_KEY. Une colonne a au plus un filtre :
 * re-filtrer la meme colonne remplace en place. */
static ObjArray* table_filters_peek(ObjUiControlInstance* uici) {
    if (!uici) return NULL;
    Value existing;
    if (obj_ui_control_instance_headless_get(uici, OWL_TABLE_FILTERS_KEY, &existing)
        && IS_ARRAY(existing))
        return AS_ARRAY(existing);
    return NULL;
}

static ObjArray* table_filters_get_or_create(ObjUiControlInstance* uici) {
    if (!uici) return NULL;
    ObjArray* arr = table_filters_peek(uici);
    if (arr) return arr;
    arr = obj_new_array();
    vm_push(OBJ_VAL(arr));  /* root pendant le set (table_set grow -> GC). */
    obj_ui_control_instance_headless_set(uici, OWL_TABLE_FILTERS_KEY, OBJ_VAL(arr));
    vm_pop();
    return arr;
}

/* Index de l'entree filtre pour <col> (casse-insensible), -1 si absente. */
static int table_filters_index_of(ObjArray* filters, const char* col) {
    if (!filters || !col) return -1;
    for (int i = 0; i < filters->elements.count; i++) {
        Value e = filters->elements.values[i];
        if (!IS_INSTANCE(e)) continue;
        Value cv;
        if (instance_get_value(AS_INSTANCE(e), "col", &cv) && IS_STRING(cv)
            && strcasecmp(AS_CSTRING(cv), col) == 0)
            return i;
    }
    return -1;
}

static void table_filters_remove_at(ObjArray* filters, int idx) {
    if (!filters || idx < 0 || idx >= filters->elements.count) return;
    for (int i = idx; i < filters->elements.count - 1; i++)
        filters->elements.values[i] = filters->elements.values[i + 1];
    filters->elements.count--;
}

/* Pose (ou remplace) le filtre <col>. Valeur vide = retire le filtre, SAUF
 * pour OWL_FILTRE_IN : un IN a ensemble vide reste un filtre actif qui ne
 * matche rien (table vide, symetrique de « Tout »). Les filtres texte gardent
 * la regle « vide = retrait ». */
static void table_filter_set_headless(ObjUiControlInstance* uici,
                                      const char* col, int type,
                                      const char* valeur) {
    if (!uici || !col) return;
    if ((valeur == NULL || valeur[0] == '\0') && type != OWL_FILTRE_IN) {
        ObjArray* filters = table_filters_peek(uici);
        int idx = table_filters_index_of(filters, col);
        if (idx >= 0) table_filters_remove_at(filters, idx);
        return;
    }
    ObjArray* filters = table_filters_get_or_create(uici);
    if (!filters) return;
    int idx = table_filters_index_of(filters, col);
    ObjInstance* rep = obj_new_instance(NULL);
    if (!rep) return;
    vm_push(OBJ_VAL(rep));  /* root pendant les inst_field_set_* (grow -> GC). */
    inst_field_set_str(rep, "col", col);
    inst_field_set_int(rep, "type", type);
    inst_field_set_str(rep, "val", valeur ? valeur : "");
    if (idx >= 0) filters->elements.values[idx] = OBJ_VAL(rep);
    else          obj_array_push(filters, OBJ_VAL(rep));  /* re-root canon. */
    vm_pop();
}

/*  — resout un identifiant de colonne (COL_Nom) en index 0-based via
 * l'ordre declare de la Table. L'index Qt suit l'ordre d'ajout des colonnes,
 * lui-meme l'ordre declare. Comparaison accent-strippee des deux cotes (le col
 * du store est deja strippe cote colproxy ; le nom declare peut etre accentue).
 * Casse-insensible. -1 si absente. */
static int table_column_index_of(ObjUiControlInstance* uici, const char* col) {
    if (!uici || !uici->decl || !col) return -1;
    for (int i = 0; i < uici->decl->column_count; i++) {
        const char* nm = uici->decl->columns[i].name;
        if (!nm) continue;
        char stripped[256];
        int len = scanner_strip_accents(nm, (int)strlen(nm),
                                        stripped, (int)sizeof stripped);
        if (len <= 0 || len >= (int)sizeof stripped) continue;
        stripped[len] = '\0';
        if (strcasecmp(stripped, col) == 0) return i;
    }
    return -1;
}

/*  — symetrique de table_column_index_of : nom accent-strippe de la
 * colonne d'index 0-based <col0> (ordre declare). Le header custom Qt ne
 * connait que l'index logique de section ; ce nom sert de cle dans le store
 * $tblfilters (coherent avec TableColonnesFiltrees / TableCherche). false si
 * hors bornes. */
static bool table_column_name_at(ObjUiControlInstance* uici, int col0,
                                 char* out, size_t cap) {
    if (!uici || !uici->decl || col0 < 0 || col0 >= uici->decl->column_count)
        return false;
    const char* nm = uici->decl->columns[col0].name;
    if (!nm) return false;
    char stripped[256];
    int len = scanner_strip_accents(nm, (int)strlen(nm),
                                    stripped, (int)sizeof stripped);
    if (len <= 0 || len >= (int)sizeof stripped) return false;
    stripped[len] = '\0';
    snprintf(out, cap, "%s", stripped);
    return true;
}

/*  — construit le jeu complet des filtres actifs (store $tblfilters)
 * et le pousse au rendu Qt qui recalcule la visibilite en ET. No-op si pas de
 * widget (headless : le store reste la seule source de verite). Aucune
 * allocation OWL entre la lecture des ObjString et l'appel Qt (pur Qt, pas de
 * GC), donc les pointeurs vals[] restent valides. */
static void table_filters_apply_to_qt(ObjUiControlInstance* uici) {
    if (!uici || !uici->widget) return;
    ObjArray* filters = table_filters_peek(uici);
    int n = filters ? filters->elements.count : 0;
    enum { MAX_FILTERS = 64 };
    int         colIdx[MAX_FILTERS];
    int         types[MAX_FILTERS];
    const char* vals[MAX_FILTERS];
    int k = 0;
    for (int i = 0; i < n && k < MAX_FILTERS; i++) {
        Value e = filters->elements.values[i];
        if (!IS_INSTANCE(e)) continue;
        ObjInstance* rep = AS_INSTANCE(e);
        Value cv;
        if (!instance_get_value(rep, "col", &cv) || !IS_STRING(cv)) continue;
        int idx = table_column_index_of(uici, AS_CSTRING(cv));
        if (idx < 0) continue;  /* colonne du filtre plus declaree : ignore */
        Value tv, vv;
        int type = 0;
        if (instance_get_value(rep, "type", &tv) && IS_INTEGER(tv))
            type = (int)AS_INTEGER(tv);
        const char* val = "";
        if (instance_get_value(rep, "val", &vv) && IS_STRING(vv))
            val = AS_CSTRING(vv);
        colIdx[k] = idx;
        types[k]  = type;
        vals[k]   = val;
        k++;
    }
    owl_ui_table_apply_filters((OwlControl*)uici->widget, colIdx, types, vals, k);
}

/*  — ponts "filtre a la souris" appeles par le header custom Qt
 * (loupe sur l'en-tete). Le header ne connait que le QTableView + l'index
 * logique de colonne ; ces fonctions mappent widget->uici (fenetre courante) et
 * col0->nom de colonne, puis reutilisent EXACTEMENT le chemin de
 * TableActiveFiltre : store $tblfilters partage + recompute ET via
 * table_filters_apply_to_qt. Aucun 2e chemin d'etat cote Qt : le filtre souris
 * et TableActiveFiltre programmatique partagent la meme source de verite
 * (TableColonnesFiltrees reflete les deux). No-op hors fenetre / colonne
 * inconnue. */
void owl_ui_table_user_apply_filter(OwlControl* tableWidget, int col0,
                                    int type, const char* val) {
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici) return;
    char col[256];
    if (!table_column_name_at(uici, col0, col, sizeof col)) return;
    /* Copie locale : les set headless allouent (grow -> GC) et invalideraient
     * une ObjString fraiche non rootee (meme garde que TableActiveFiltre). */
    char valbuf[512];
    snprintf(valbuf, sizeof valbuf, "%s", val ? val : "");
    table_filter_set_headless(uici, col, type, valbuf);
    table_filters_apply_to_qt(uici);
}

void owl_ui_table_user_clear_filter(OwlControl* tableWidget, int col0) {
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici) return;
    char col[256];
    if (!table_column_name_at(uici, col0, col, sizeof col)) return;
    /* Valeur vide => retire le seul filtre de cette colonne ; les autres
     * colonnes filtrees tiennent (recompute ET sur le jeu restant). */
    table_filter_set_headless(uici, col, 0, "");
    table_filters_apply_to_qt(uici);
}

int owl_ui_table_col_is_filtered(OwlControl* tableWidget, int col0) {
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici) return 0;
    char col[256];
    if (!table_column_name_at(uici, col0, col, sizeof col)) return 0;
    ObjArray* filters = table_filters_peek(uici);
    return table_filters_index_of(filters, col) >= 0 ? 1 : 0;
}

/*  — seuil de cardinalite au-dela duquel une colonne n'a pas de
 * checklist (calcul des distinctes borne, popup non proposee). Le test envoie
 * 150 valeurs distinctes ; tout seuil <= 149 verrouille le comportement. */
#define OWL_TABLE_CHECKLIST_MAX_DISTINCT 30

/*  — pose (ou remplace) le filtre IN {ensemble} de <col> via le MEME
 * store $tblfilters + le MEME recompute ET que TableActiveFiltre. C'est LA
 * fonction commune au hook de test (__DebugTableFiltreIN) et a la popup
 * checklist Qt : un seul systeme. valeursRC = valeurs a retenir, jointes par RC.
 * valeursRC vide => aucun filtre (via table_filter_set_headless). Le retrait
 * canonique reste TableDesactiveFiltre. */
static void table_apply_filter_in_headless(ObjUiControlInstance* uici,
                                           const char* col,
                                           const char* valeursRC) {
    if (!uici || !col) return;
    table_filter_set_headless(uici, col, OWL_FILTRE_IN, valeursRC ? valeursRC : "");
    table_filters_apply_to_qt(uici);
}

/*  — pont popup checklist Qt : le header custom (OwlFilterHeaderView)
 * appelle cette fonction a la validation de la checklist, exactement comme
 * __DebugTableFiltreIN cote test. col0 = index 0-based (ordre declare). No-op
 * hors fenetre courante / colonne inconnue. */
void owl_ui_table_user_apply_filter_in(OwlControl* tableWidget, int col0,
                                       const char* valeursRC) {
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici) return;
    char col[256];
    if (!table_column_name_at(uici, col0, col, sizeof col)) return;
    /* Copie locale : les set headless allouent (grow -> GC) et invalideraient
     * une chaine source non rootee (meme garde que TableActiveFiltre). Ensemble
     * potentiellement large (jusqu'au seuil) : buffer dynamique. */
    size_t n = valeursRC ? strlen(valeursRC) : 0;
    char* buf = (char*)malloc(n + 1);
    if (!buf) return;
    memcpy(buf, valeursRC ? valeursRC : "", n);
    buf[n] = '\0';
    table_apply_filter_in_headless(uici, col, buf);
    free(buf);
}

/*  — valeurs distinctes de la colonne <col> du store, dans l'ordre de
 * premiere apparition, jointes par RC. Rend un buffer malloc (a liberer) ou
 * NULL si la colonne est vide OU depasse le seuil de cardinalite (dans les deux
 * cas : pas de checklist). Backend pur : ne touche pas le widget. */
static char* table_distinct_values_of(ObjUiControlInstance* uici,
                                       const char* col) {
    if (!uici || !col) return NULL;
    ObjArray* items = widget_items_store_peek_uici(uici);
    if (!items || items->elements.count == 0) return NULL;
    char* distinct[OWL_TABLE_CHECKLIST_MAX_DISTINCT + 1];
    int nd = 0;
    bool overflow = false;
    for (int i = 0; i < items->elements.count && !overflow; i++) {
        Value it = items->elements.values[i];
        if (!IS_INSTANCE(it)) continue;
        Value cellV;
        if (!instance_get_value(AS_INSTANCE(it), col, &cellV)) continue;
        ObjString* cellS = value_to_string(cellV);  /* copie avant tout autre alloc */
        const char* s = cellS ? cellS->chars : "";
        bool found = false;
        for (int j = 0; j < nd; j++)
            if (strcmp(distinct[j], s) == 0) { found = true; break; }
        if (found) continue;
        if (nd >= OWL_TABLE_CHECKLIST_MAX_DISTINCT) { overflow = true; break; }
        size_t l = strlen(s);
        char* dup = (char*)malloc(l + 1);
        if (!dup) { overflow = true; break; }
        memcpy(dup, s, l + 1);
        distinct[nd++] = dup;
    }
    char* result = NULL;
    if (!overflow && nd > 0) {
        size_t total = 1;
        for (int j = 0; j < nd; j++) total += strlen(distinct[j]) + 2;
        result = (char*)malloc(total);
        if (result) {
            size_t len = 0;
            for (int j = 0; j < nd; j++) {
                if (j) { memcpy(result + len, "\r\n", 2); len += 2; }
                size_t l = strlen(distinct[j]);
                memcpy(result + len, distinct[j], l);
                len += l;
            }
            result[len] = '\0';
        }
    }
    for (int j = 0; j < nd; j++) free(distinct[j]);
    return result;
}

/*  — pont popup checklist Qt : valeurs distinctes de la colonne col0
 * (0-based) pour peupler les cases a cocher. out reçoit les distinctes jointes
 * par RC (chaine vide si vide ou au-dela du seuil : pas de checklist). No-op
 * hors fenetre courante / colonne inconnue. */
void owl_ui_table_distinct_values(OwlControl* tableWidget, int col0,
                                  char* out, int cap) {
    if (out && cap > 0) out[0] = '\0';
    if (!out || cap <= 0) return;
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici) return;
    char col[256];
    if (!table_column_name_at(uici, col0, col, sizeof col)) return;
    char* d = table_distinct_values_of(uici, col);
    if (d) {
        snprintf(out, (size_t)cap, "%s", d);
        free(d);
    }
}

/*  — valeur du filtre actif sur col0, pour prereplir le champ inline
 * a la re-ouverture de la loupe (pont differe, desormais en scope). Pure
 * lecture du store $tblfilters : aucune allocation OWL, pointeurs valides le
 * temps de la copie dans out. */
int owl_ui_table_user_get_filter_value(OwlControl* tableWidget, int col0,
                                        char* out, int cap) {
    if (out && cap > 0) out[0] = '\0';
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici) return 0;
    char col[256];
    if (!table_column_name_at(uici, col0, col, sizeof col)) return 0;
    ObjArray* filters = table_filters_peek(uici);
    int idx = table_filters_index_of(filters, col);
    if (idx < 0) return 0;
    Value e = filters->elements.values[idx];
    if (!IS_INSTANCE(e)) return 0;
    Value vv;
    if (!instance_get_value(AS_INSTANCE(e), "val", &vv) || !IS_STRING(vv))
        return 0;
    if (out && cap > 0) snprintf(out, (size_t)cap, "%s", AS_CSTRING(vv));
    return 1;
}

/*  — type (constante filtre*) du filtre actif sur la colonne col0.
 * Symetrique de owl_ui_table_user_get_filter_value : le header custom Qt lit ce
 * type AVANT de prereplir l'editeur inline. Pour un OWL_FILTRE_IN, la valeur
 * stockee est l'ensemble RC-joint des valeurs cochees (checklist) -- pas un
 * motif texte editable : l'editeur s'ouvre alors VIDE plutot que pre-rempli.
 * Lit le meme store $tblfilters (pure lecture). Rend 0 si aucun filtre actif. */
int owl_ui_table_user_get_filter_type(OwlControl* tableWidget, int col0) {
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici) return 0;
    char col[256];
    if (!table_column_name_at(uici, col0, col, sizeof col)) return 0;
    ObjArray* filters = table_filters_peek(uici);
    int idx = table_filters_index_of(filters, col);
    if (idx < 0) return 0;
    Value e = filters->elements.values[idx];
    if (!IS_INSTANCE(e)) return 0;
    Value tv;
    if (!instance_get_value(AS_INSTANCE(e), "type", &tv) || !IS_INTEGER(tv))
        return 0;
    return (int)AS_INTEGER(tv);
}

/*  — mode de gestion de la loupe (propriete ModeLoupe) resolu pour la
 * Table portant tableWidget. Lit le store d'instance (cle canon partagee FR/EN,
 * cf. headless_key_normalize_canon) et retombe sur le defaut rechercheCommencePar
 * hors fenetre courante ou si la propriete n'a jamais ete ecrite. */
int owl_ui_table_user_mode_loupe(OwlControl* tableWidget) {
    ObjUiControlInstance* uici = current_window_uici_by_widget((void*)tableWidget);
    if (!uici) return OWL_RCHT_COMMENCE_PAR;
    Value v;
    if (obj_ui_control_instance_headless_get(uici, "ModeLoupe", &v)
        && IS_INTEGER(v)) {
        return (int)AS_INTEGER(v);
    }
    return OWL_RCHT_COMMENCE_PAR;
}

/* TableDesactiveFiltre(TAB) — retire TOUS les filtres du champ.
 * TableDesactiveFiltre(TAB.COL) — retire le seul filtre de cette colonne.
 * canon 1000017150. Met a jour l'etat headless (reflete par
 * TableColonnesFiltrees). */
static Value fn_table_desactive_filtre(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr,
            "[TableDesactiveFiltre] (table) ou (table.colonne) attendu.\n");
        return NIL_VAL;
    }
    /* Cas colonne : TAB.COL. */
    ObjUiControlInstance* uici = NULL;
    char col[256];
    if (table_colproxy_resolve(args[0], &uici, col, sizeof col)) {
        ObjArray* filters = table_filters_peek(uici);
        int idx = table_filters_index_of(filters, col);
        if (idx >= 0) table_filters_remove_at(filters, idx);
        /* Qt : recompute la visibilite depuis les filtres restants (les autres
         * colonnes filtrees tiennent). No-op si pas de widget (headless). */
        table_filters_apply_to_qt(uici);
        return NIL_VAL;
    }
    /* Cas table entiere : retire tous les filtres. */
    OwlControl* table = table_arg_or_error(args[0], "TableDesactiveFiltre");
    ObjUiControlInstance* tuici = table_uici_from_arg(args[0]);
    if (!table && !tuici) return NIL_VAL;
    ObjArray* filters = table_filters_peek(tuici);
    if (filters) filters->elements.count = 0;
    /* Aucun filtre => toutes les lignes redeviennent visibles. */
    owl_ui_table_apply_filters(table, NULL, NULL, NULL, 0);
    return NIL_VAL;
}

/* TableColonnesFiltrees(TAB) — canon 1000020517. Rend une chaine : "" si
 * aucun filtre actif, sinon une entree "<colonne>;<type>;<valeur>" par filtre,
 * separees par RC (CRLF). <colonne> = identifiant OWL de colonne (COL_Nom),
 * coherent avec TableCherche. Pure lecture du store headless. */
static Value fn_table_colonnes_filtrees(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[TableColonnesFiltrees] (table) attendu.\n");
        return OBJ_VAL(obj_copy_string("", 0));
    }
    OwlControl* table = table_arg_or_error(args[0], "TableColonnesFiltrees");
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return OBJ_VAL(obj_copy_string("", 0));
    ObjArray* filters = table_filters_peek(uici);
    if (!filters || filters->elements.count == 0)
        return OBJ_VAL(obj_copy_string("", 0));

    size_t cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return OBJ_VAL(obj_copy_string("", 0));
    buf[0] = '\0';
    for (int i = 0; i < filters->elements.count; i++) {
        Value e = filters->elements.values[i];
        if (!IS_INSTANCE(e)) continue;
        Value cv, tv, vv;
        const char* c = (instance_get_value(AS_INSTANCE(e), "col", &cv)
                         && IS_STRING(cv)) ? AS_CSTRING(cv) : "";
        long long t = (instance_get_value(AS_INSTANCE(e), "type", &tv)
                       && IS_INTEGER(tv)) ? (long long)AS_INTEGER(tv) : 0;
        const char* v = (instance_get_value(AS_INSTANCE(e), "val", &vv)
                         && IS_STRING(vv)) ? AS_CSTRING(vv) : "";
        const char* sep = (len > 0) ? "\r\n" : "";
        int need = snprintf(NULL, 0, "%s%s;%lld;%s", sep, c, t, v);
        if (need < 0) continue;
        if (len + (size_t)need + 1 > cap) {
            while (len + (size_t)need + 1 > cap) cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return OBJ_VAL(obj_copy_string("", 0)); }
            buf = nb;
        }
        snprintf(buf + len, cap - len, "%s%s;%lld;%s", sep, c, t, v);
        len += (size_t)need;
    }
    ObjString* res = obj_copy_string(buf, (int)len);
    free(buf);
    return OBJ_VAL(res);
}

/* Saute les espaces de tete (semantique canon : la recherche ignore le blanc de
 * debut). La casse est traitee par les comparaisons ci-dessous. */
static const char* table_search_skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool table_search_ci_contains(const char* hay, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    for (const char* p = hay; *p != '\0'; p++)
        if (strncasecmp(p, needle, nlen) == 0) return true;
    return false;
}

static bool table_search_matches(const char* hay, const char* needle, int mode) {
    hay = table_search_skip_ws(hay);
    needle = table_search_skip_ws(needle);
    if (mode == OWL_RCHT_COMMENCE_PAR)
        return strncasecmp(hay, needle, strlen(needle)) == 0;
    if (mode == OWL_RCHT_CONTIENT)
        return table_search_ci_contains(hay, needle);
    /* rechercheIdentique / rechercheDéfaut : comparaison exacte. */
    return strcasecmp(hay, needle) == 0;
}

/*  — TableCherche(TAB.COL, valeur [, type [, ligneDepart]]).
 * arg[0] est le proxy-colonne. Rend l'indice 1-based de la premiere ligne dont
 * la cellule <col> correspond selon le type de recherche (identique par
 * defaut), -1 si aucune. Pure lecture du store headless, aucun Qt. */
static Value fn_table_cherche(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr,
            "[TableCherche] (colonne, valeur [, type [, ligne]]) attendu.\n");
        return INTEGER_VAL(-1);
    }
    ObjUiControlInstance* uici = NULL;
    char col[256];
    if (!table_colproxy_resolve(args[0], &uici, col, sizeof col)) {
        fprintf(stderr,
            "[TableCherche] 1er argument : colonne de Table attendue "
            "(forme TABLE.COL_X).\n");
        return INTEGER_VAL(-1);
    }

    /* Copie locale du terme cherche : value_to_string rappelee dans la boucle
     * peut allouer (grow -> GC) et invalider une ObjString fraiche non rootee. */
    ObjString* needleS = value_to_string(args[1]);
    char needle[512];
    snprintf(needle, sizeof needle, "%s", needleS ? needleS->chars : "");

    int mode = OWL_RCHT_IDENTIQUE;
    if (argCount >= 3 && IS_INTEGER(args[2])) mode = (int)AS_INTEGER(args[2]);

    int start = 1;
    if (argCount >= 4 && IS_INTEGER(args[3])) start = (int)AS_INTEGER(args[3]);
    if (start < 1) start = 1;

    ObjArray* items = widget_items_store_peek_uici(uici);
    if (!items) return INTEGER_VAL(-1);
    for (int i = start - 1; i < items->elements.count; i++) {
        Value it = items->elements.values[i];
        if (!IS_INSTANCE(it)) continue;
        Value cellV;
        if (!instance_get_value(AS_INSTANCE(it), col, &cellV)) continue;
        ObjString* cellS = value_to_string(cellV);   /* utilise avant toute alloc */
        if (cellS && table_search_matches(cellS->chars, needle, mode))
            return INTEGER_VAL(i + 1);
    }
    return INTEGER_VAL(-1);
}

/*  — TableCherchePartout(TAB, valeur [, type [, ligneDepart]]).
 * arg[0] est la Table entiere (pas une colonne). Rend l'indice 1-based de la
 * premiere ligne dont AU MOINS UNE colonne correspond selon le type de
 * recherche, -1 si aucune. Pure lecture du store headless.
 * Divergence assumee avec TableCherche : le type par DEFAUT est
 * rechercheContient (et non rechercheIdentique), y compris quand
 * rechercheDefaut est passe explicitement. Fouiller toutes les colonnes n'a
 * de sens que pour une correspondance partielle. */
static Value fn_table_cherche_partout(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr,
            "[TableCherchePartout] (table, valeur [, type [, ligne]]) attendu.\n");
        return INTEGER_VAL(-1);
    }
    OwlControl* table = table_arg_or_error(args[0], "TableCherchePartout");
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!table && !uici) return INTEGER_VAL(-1);
    if (!uici) return INTEGER_VAL(-1);

    /* Copie locale du terme cherche : value_to_string rappelee dans la boucle
     * peut allouer (grow -> GC) et invalider une ObjString fraiche non rootee. */
    ObjString* needleS = value_to_string(args[1]);
    char needle[512];
    snprintf(needle, sizeof needle, "%s", needleS ? needleS->chars : "");

    int mode = OWL_RCHT_CONTIENT;   /* defaut : contient (divergence TableCherche) */
    if (argCount >= 3 && IS_INTEGER(args[2])) mode = (int)AS_INTEGER(args[2]);
    if (mode == OWL_RCHT_DEFAUT) mode = OWL_RCHT_CONTIENT;

    int start = 1;
    if (argCount >= 4 && IS_INTEGER(args[3])) start = (int)AS_INTEGER(args[3]);
    if (start < 1) start = 1;

    const OwlControlDecl* tdecl = uici->decl;
    int ncol = tdecl ? tdecl->column_count : 0;
    ObjArray* items = widget_items_store_peek_uici(uici);
    if (!items) return INTEGER_VAL(-1);
    for (int i = start - 1; i < items->elements.count; i++) {
        Value it = items->elements.values[i];
        if (!IS_INSTANCE(it)) continue;
        ObjInstance* rep = AS_INSTANCE(it);
        for (int c = 0; c < ncol; c++) {
            const char* colName = tdecl->columns[c].name;
            if (!colName) continue;
            char colKey[256];
            int klen = scanner_strip_accents(colName, (int)strlen(colName),
                                             colKey, (int)sizeof(colKey));
            if (klen <= 0 || klen >= (int)sizeof(colKey)) continue;
            colKey[klen] = '\0';
            Value cellV;
            if (!instance_get_value(rep, colKey, &cellV)) continue;
            ObjString* cellS = value_to_string(cellV);  /* utilise avant toute alloc */
            if (cellS && table_search_matches(cellS->chars, needle, mode))
                return INTEGER_VAL(i + 1);
        }
    }
    return INTEGER_VAL(-1);
}

/* ====================================================================
 *  — export data-only d'un Champ Table (entetes + cellules) vers
 * Excel (.xlsx) et le presse-papier. Extraction pure-donnees depuis le store
 * headless : les entetes sont le libelle declare de chaque colonne, les
 * cellules les valeurs du store (une rep ObjInstance par ligne, cle = nom de
 * colonne strippe). Toutes les chaines sont dupliquees (strdup) : elles restent
 * valides pendant l'export owl_ui_table_export_xlsx, qui alloue des ObjString
 * (grow -> GC) et collecterait sinon des ObjString fraiches non rootees.
 * ==================================================================== */
typedef struct {
    char** headers;   /* [ncols], owned */
    char** cells;     /* [nrows*ncols], owned (row-major) */
    int    ncols;
    int    nrows;
} OwlTableDump;

static char* owl_table_dump_strdup(const char* s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

static void owl_table_dump_free(OwlTableDump* d) {
    if (d->headers) {
        for (int i = 0; i < d->ncols; i++) free(d->headers[i]);
        free(d->headers);
    }
    if (d->cells) {
        for (int i = 0; i < d->ncols * d->nrows; i++) free(d->cells[i]);
        free(d->cells);
    }
    d->headers = NULL; d->cells = NULL; d->ncols = 0; d->nrows = 0;
}

/* Remplit `out` depuis la Table. Retourne false si aucune colonne declaree. */
static bool owl_table_dump_build(ObjUiControlInstance* uici, OwlTableDump* out) {
    memset(out, 0, sizeof(*out));
    if (!uici || !uici->decl) return false;
    const OwlControlDecl* tdecl = uici->decl;
    int ncol = tdecl->column_count;
    if (ncol <= 0) return false;
    out->ncols = ncol;

    out->headers = (char**)calloc((size_t)ncol, sizeof(char*));
    if (!out->headers) { out->ncols = 0; return false; }
    for (int c = 0; c < ncol; c++) {
        const OwlColumnDecl* col = &tdecl->columns[c];
        const char* lib = prop_str(col->properties, col->property_count,
                                   "Libelle", "Caption", "");
        out->headers[c] = owl_table_dump_strdup(lib);
    }

    ObjArray* items = widget_items_store_peek_uici(uici);
    int nrows = items ? items->elements.count : 0;
    out->nrows = nrows;
    if (nrows <= 0) return true;
    out->cells = (char**)calloc((size_t)nrows * (size_t)ncol, sizeof(char*));
    if (!out->cells) { out->nrows = 0; return true; }  /* entetes seuls */
    for (int r = 0; r < nrows; r++) {
        Value rv = items->elements.values[r];
        ObjInstance* rep = IS_INSTANCE(rv) ? AS_INSTANCE(rv) : NULL;
        for (int c = 0; c < ncol; c++) {
            const char* val = "";
            ObjString* keep = NULL;   /* consomme avant le strdup, pas de GC entre */
            if (rep) {
                const char* colName = tdecl->columns[c].name;
                if (colName) {
                    char colKey[256];
                    int klen = scanner_strip_accents(colName, (int)strlen(colName),
                                                     colKey, (int)sizeof(colKey));
                    if (klen > 0 && klen < (int)sizeof(colKey)) {
                        colKey[klen] = '\0';
                        Value cellV;
                        if (instance_get_value(rep, colKey, &cellV)) {
                            keep = value_to_string(cellV);
                            if (keep) val = keep->chars;
                        }
                    }
                }
            }
            out->cells[r * ncol + c] = owl_table_dump_strdup(val);
        }
    }
    return true;
}

/* Serialise le dump en texte tabule : colonnes jointes par TAB, lignes par LF,
 * entetes en premiere ligne. Pas de LF final. Caller free(). */
static char* owl_table_dump_to_text(const OwlTableDump* d) {
    size_t total = 1;  /* NUL final */
    for (int c = 0; c < d->ncols; c++)
        total += strlen(d->headers[c] ? d->headers[c] : "") + 1;
    for (int i = 0; i < d->ncols * d->nrows; i++)
        total += strlen(d->cells[i] ? d->cells[i] : "") + 1;
    char* buf = (char*)malloc(total);
    if (!buf) return NULL;
    size_t pos = 0;
    for (int c = 0; c < d->ncols; c++) {
        if (c > 0) buf[pos++] = '\t';
        const char* s = d->headers[c] ? d->headers[c] : "";
        size_t n = strlen(s); memcpy(buf + pos, s, n); pos += n;
    }
    for (int r = 0; r < d->nrows; r++) {
        buf[pos++] = '\n';
        for (int c = 0; c < d->ncols; c++) {
            if (c > 0) buf[pos++] = '\t';
            const char* s = d->cells[r * d->ncols + c];
            if (!s) s = "";
            size_t n = strlen(s); memcpy(buf + pos, s, n); pos += n;
        }
    }
    buf[pos] = '\0';
    return buf;
}

/* TableVersExcel(<Table>, <chemin>) -> booleen. Ecrit un .xlsx avec les
 * entetes en ligne 1 et les donnees en lignes 2+. Chemin invalide : Faux +
 * ErreurDetectee (erreur douce, pas de crash). */
static Value fn_table_vers_excel(int argCount, Value* args) {
    if (argCount < 2) return BOOL_VAL(false);
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!uici) return BOOL_VAL(false);
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TableVersExcel",
                              "arg", "2",
                              "expected", "chaine (chemin du fichier)",
                              "actual", "valeur incompatible",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* path = AS_CSTRING(args[1]);
    OwlTableDump d;
    if (!owl_table_dump_build(uici, &d)) return BOOL_VAL(false);
    int ok = owl_ui_table_export_xlsx(path,
                (const char* const*)d.headers, d.ncols,
                (const char* const*)d.cells, d.nrows);
    owl_table_dump_free(&d);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "TableVersExcel",
                              "native", "TableVersExcel",
                              "path", path,
                              "detail", "ecriture du classeur impossible",
                              NULL);
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

/* TableVersPressePapier(<Table>) -> booleen. Copie le contenu (entetes +
 * donnees) au format texte tabule via VersPressePapier — meme point
 * d'interception mock que l'OP_INVOKE du VM, pour que
 * MockCree(VersPressePapier)/VerifieAppele fonctionnent. */
static Value fn_table_vers_pressepapier(int argCount, Value* args) {
    if (argCount < 1) return BOOL_VAL(false);
    ObjUiControlInstance* uici = table_uici_from_arg(args[0]);
    if (!uici) return BOOL_VAL(false);
    OwlTableDump d;
    if (!owl_table_dump_build(uici, &d)) return BOOL_VAL(false);
    char* text = owl_table_dump_to_text(&d);
    owl_table_dump_free(&d);
    if (!text) return BOOL_VAL(false);
    Value texteVal = OBJ_VAL(obj_copy_string(text, (int)strlen(text)));
    free(text);

    const OwlNativeDef* vpp = owl_registry_find("VersPressePapier");
    if (!vpp || !vpp->impl) return BOOL_VAL(false);
    vm_push(texteVal);  /* root pendant intercept/appel (peut allouer -> GC) */
    Value res;
    bool intercepted = mock_try_intercept_fn(vpp->impl, 1, &texteVal, &res);
    if (!intercepted) res = vpp->impl(1, &texteVal);
    vm_pop();
    return IS_BOOL(res) ? res : BOOL_VAL(true);
}

/* ====================================================================
 *  Registry
 * ==================================================================== */

/* "chaine" et pas "tout" : le type publie decrit le contrat NOMINAL, aligne
 * sur le canon (le message d'une boite est du texte) et sur , qui
 * s'appuie sur ce type pour coercer un litteral multilingue vers la langue
 * courante. La native accepte davantage — tout argument est rendu par sa
 * representation d'affichage, une ligne par argument — et le dit
 * dans la description du parametre. Meme convention que Taille, declaree
 * "chaine" alors qu'elle mesure aussi tableaux et collections. */
static OwlControl* list_arg_or_error(Value v, const char* fnName) {
    /*  — accepte aussi ObjUiControlRef / ObjUiControlInstance via
     * helper consolide. */
    return resolve_widget_kinded(v, OWL_CONTROL_LISTE, (int)OWL_CONTROL_COMBO,
                                 fnName, "Champ Liste ou Combo");
}
static ObjArray* widget_items_store_get_or_create_uici(ObjUiControlInstance* uici) {
    if (!uici) return NULL;
    Value existing;
    if (obj_ui_control_instance_headless_get(uici, OWL_UI_LIST_ITEMS_KEY, &existing)
        && IS_ARRAY(existing)) {
        return AS_ARRAY(existing);
    }
    ObjArray* arr = obj_new_array();
    /* push pendant le set : table_set peut grow → GC, et l'array vient
     * d'etre alloue sans ancre. Le pattern existe deja dans
     * obj_ui_control_instance_headless_set (qui re-push en interne), mais
     * symetrie defensive ici. */
    vm_push(OBJ_VAL(arr));
    obj_ui_control_instance_headless_set(uici, OWL_UI_LIST_ITEMS_KEY, OBJ_VAL(arr));
    vm_pop();
    return arr;
}
static ObjArray* widget_items_store_get_or_create_uic(ObjUiControl* uic) {
    if (!uic) return NULL;
    Value existing;
    if (obj_ui_control_headless_get(uic, OWL_UI_LIST_ITEMS_KEY, &existing)
        && IS_ARRAY(existing)) {
        return AS_ARRAY(existing);
    }
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));
    obj_ui_control_headless_set(uic, OWL_UI_LIST_ITEMS_KEY, OBJ_VAL(arr));
    vm_pop();
    return arr;
}
static ObjArray* widget_items_store_peek_uici(ObjUiControlInstance* uici) {
    if (!uici) return NULL;
    Value existing;
    if (obj_ui_control_instance_headless_get(uici, OWL_UI_LIST_ITEMS_KEY, &existing)
        && IS_ARRAY(existing))
        return AS_ARRAY(existing);
    return NULL;
}
static ObjArray* widget_items_store_peek_uic(ObjUiControl* uic) {
    if (!uic) return NULL;
    Value existing;
    if (obj_ui_control_headless_get(uic, OWL_UI_LIST_ITEMS_KEY, &existing)
        && IS_ARRAY(existing))
        return AS_ARRAY(existing);
    return NULL;
}
static ObjUiControlInstance* table_uici_from_arg(Value v) {
    ObjUiControlInstance* uici = NULL;
    if (IS_UI_CONTROL_REF(v))            uici = resolve_ui_ref(AS_UI_CONTROL_REF(v));
    else if (IS_UI_CONTROL_INSTANCE(v))  uici = AS_UI_CONTROL_INSTANCE(v);
    else if (IS_STRING(v))               uici = current_window_uici_by_name(AS_CSTRING(v));
    if (!uici || !uici->decl || uici->decl->kind != OWL_CONTROL_TABLE)
        return NULL;
    return uici;
}
static ObjInstance* table_build_headless_rep(ObjUiControlInstance* uici,
                                             Value* values, int n) {
    ObjInstance* rep = obj_new_instance(NULL);
    if (!rep) return NULL;
    vm_push(OBJ_VAL(rep));  /* root pendant les inst_field_set_* (table grow) */
    const OwlControlDecl* tdecl = (uici && uici->decl) ? uici->decl : NULL;
    int ncol = tdecl ? tdecl->column_count : 0;
    for (int i = 0; i < ncol; i++) {
        const char* colName = tdecl->columns[i].name;
        if (!colName) continue;
        /*  — la cle de cellule doit etre la forme canon (accents
         * strippes), pas la graphie de declaration. La lecture
         * `TBL[ligne].COL_X` interroge la rep avec le nom deja strippe par le
         * compilateur (invariant 2) : une colonne declaree accentuee posait
         * une cle accentuee que cette requete ne pouvait jamais retrouver, et
         * la cellule etait illisible dans TOUTES les graphies — y compris la
         * sienne. La forme colonne d'abord passait, d'ou la rupture de la
         * symetrie TBL.COL[L] == TBL[L].COL sur les seuls noms accentues. */
        char colKey[256];
        int klen = scanner_strip_accents(colName, (int)strlen(colName),
                                         colKey, (int)sizeof(colKey));
        if (klen <= 0 || klen >= (int)sizeof(colKey)) continue;
        colKey[klen] = '\0';
        if (i < n) inst_field_set_value(rep, colKey, values[i]);
        else       inst_field_set_str(rep, colKey, "");
    }
    /*  — back-ref vers la Table, miroir strict de `$zrsrc`.
     * Sans ce tag une rep de ligne est indistinguable d'un struct user : ni le
     * descripteur de cellule (`TABLE_X[ligne][colonne]`, OP_ARRAY_GET) ni la
     * mise en forme de ligne (`TABLE_X[ligne]..Couleur`, OP_STRUCT_GET/SET) ne
     * pourraient se declencher sans faux positif. La valeur est l'uici, deja
     * vivante dans owner->controls : pas de nouveau cycle GC. */
    if (uici != NULL) {
        inst_field_set_value(rep, OWL_TABLE_REP_SRC_KEY, OBJ_VAL(uici));
    }
    vm_pop();
    return rep;
}
static void table_store_append_row(ObjUiControlInstance* uici,
                                    Value* values, int n) {
    if (!uici) return;
    ObjArray* items = widget_items_store_get_or_create_uici(uici);
    if (!items) return;
    ObjInstance* rep = table_build_headless_rep(uici, values, n);
    if (rep) {
        /* canon obj_array_push : root arr+val avant le grow. */
        obj_array_push(items, OBJ_VAL(rep));
    } else {
        value_array_write(&items->elements, NIL_VAL);
    }
}
static void table_store_set_row(ObjUiControlInstance* uici, int row,
                                Value* values, int n) {
    if (!uici || row < 1) return;
    ObjArray* items = widget_items_store_get_or_create_uici(uici);
    if (!items || row > items->elements.count) return;
    ObjInstance* rep = table_build_headless_rep(uici, values, n);
    if (rep) items->elements.values[row - 1] = OBJ_VAL(rep);
}
static void table_store_remove_row(ObjUiControlInstance* uici, int row) {
    if (!uici || row < 1) return;
    ObjArray* items = widget_items_store_peek_uici(uici);
    if (!items || row > items->elements.count) return;
    for (int i = row - 1; i < items->elements.count - 1; i++)
        items->elements.values[i] = items->elements.values[i + 1];
    items->elements.count--;
}
static void table_store_clear(ObjUiControlInstance* uici) {
    if (!uici) return;
    ObjArray* items = widget_items_store_peek_uici(uici);
    if (items) items->elements.count = 0;
}
static void table_widget_set_row_from_rep(OwlControl* table,
                                          ObjUiControlInstance* uici, int row) {
    ObjArray* items = widget_items_store_peek_uici(uici);
    if (!table || !items || row < 1 || row > items->elements.count) return;
    Value rv = items->elements.values[row - 1];
    ObjInstance* rep = IS_INSTANCE(rv) ? AS_INSTANCE(rv) : NULL;
    const OwlControlDecl* tdecl = (uici && uici->decl) ? uici->decl : NULL;
    int ncol = tdecl ? tdecl->column_count : 0;
    if (ncol <= 0) return;
    const char** cells = (const char**)malloc((size_t)ncol * sizeof(char*));
    if (!cells) return;
    ObjString** keep = (ObjString**)calloc((size_t)ncol, sizeof(ObjString*));
    for (int i = 0; i < ncol; i++) {
        cells[i] = "";
        const char* colName = tdecl->columns[i].name;
        if (!colName || !rep) continue;
        char colKey[256];
        int klen = scanner_strip_accents(colName, (int)strlen(colName),
                                         colKey, (int)sizeof(colKey));
        if (klen <= 0 || klen >= (int)sizeof(colKey)) continue;
        colKey[klen] = '\0';
        Value cell;
        if (!instance_get_value(rep, colKey, &cell)) continue;
        if (IS_STRING(cell)) {
            cells[i] = AS_CSTRING(cell);
        } else {
            ObjString* s = value_to_string(cell);
            if (keep) keep[i] = s;
            cells[i] = s ? s->chars : "";
        }
    }
    owl_ui_table_set_row(table, row, cells, ncol);
    free(cells);
    free(keep);
}
static int table_col_index_stripped(ObjUiControlInstance* uici, const char* col) {
    const OwlControlDecl* tdecl = uici ? uici->decl : NULL;
    int ncol = tdecl ? tdecl->column_count : 0;
    for (int i = 0; i < ncol; i++) {
        const char* nm = tdecl->columns[i].name;
        if (!nm) continue;
        char key[256];
        int klen = scanner_strip_accents(nm, (int)strlen(nm), key,
                                         (int)sizeof(key));
        if (klen <= 0 || klen >= (int)sizeof(key)) continue;
        key[klen] = '\0';
        if (strcasecmp(key, col) == 0) return i + 1;
    }
    return 0;
}
static void table_fmt_applied_impl(ObjInstance* rep, const char* col,
                                   const char* propCanon, Value v) {
    if (!rep || !col || !propCanon) return;
    Value srcV;
    if (!instance_get_value(rep, OWL_TABLE_REP_SRC_KEY, &srcV)
        || !IS_UI_CONTROL_INSTANCE(srcV)) return;
    ObjUiControlInstance* uici = AS_UI_CONTROL_INSTANCE(srcV);
    OwlControl* widget = (OwlControl*)uici->widget;
    if (!widget) return;

    int bgr = -1, alpha = 255;
    owl_value_to_color(v, &bgr, &alpha);
    if (bgr < 0) return;
    int is_bg = (strcasecmp(propCanon, "CouleurFond") == 0);

    /*  Lot C — receveur COLONNE : le holder persistant $colfmt de la
     * Table (pas une rep du store). Il porte un nom de colonne non vide et se
     * reconnait par identite au holder enregistre sur l'uici. On peint toute la
     * colonne, lignes futures comprises. */
    Value holderV;
    if (col[0] != '\0'
        && obj_ui_control_instance_headless_get(uici, OWL_TABLE_COLFMT_HOLDER_KEY,
                                                &holderV)
        && IS_INSTANCE(holderV) && AS_INSTANCE(holderV) == rep) {
        int colIdx = table_col_index_stripped(uici, col);
        if (colIdx > 0)
            owl_ui_table_set_column_color(widget, colIdx, is_bg, bgr, alpha);
        return;
    }

    ObjArray* items = widget_items_store_peek_uici(uici);
    if (!items) return;
    int row = 0;
    for (int i = 0; i < items->elements.count; i++) {
        Value it = items->elements.values[i];
        if (IS_INSTANCE(it) && AS_INSTANCE(it) == rep) { row = i + 1; break; }
    }
    if (row == 0) return;

    /* colIdx 0 = niveau ligne (toutes les colonnes). */
    int colIdx = 0;
    if (col[0] != '\0') {
        colIdx = table_col_index_stripped(uici, col);
        if (colIdx == 0) return;
    }

    owl_ui_table_set_cell_color(widget, row, colIdx, is_bg, bgr, alpha);
}
static void table_colmask_applied_impl(void* widget, int col_1based,
                                       const char* wl_mask) {
    owl_ui_table_set_column_mask((OwlControl*)widget, col_1based, wl_mask);
}
static bool resolve_widget_items_target(Value v, const char* fnName,
                                        OwlControlKind k1, int k2,
                                        const char* humanKind,
                                        bool create,
                                        OwlControl** widget_out,
                                        ObjArray** items_out) {
    *widget_out = NULL;
    *items_out  = NULL;

    /* Kind check d'abord — emet l'erreur stderr standardisee si le receiver
     * n'est pas du bon kind. Retourne le widget Qt (peut etre NULL en
     * headless). */
    OwlControl* widget = resolve_widget_kinded(v, k1, k2, fnName, humanKind);

    /* resolve_widget_kinded a deja signale si le receiver est invalide.
     * Heuristique : si Value n'est ni UI_CONTROL ni UI_CONTROL_REF ni
     * UI_CONTROL_INSTANCE, c'est l'erreur "1er argument doit etre un <humanKind>"
     * et on sort. */
    if (!IS_UI_CONTROL(v) && !IS_UI_CONTROL_REF(v) && !IS_UI_CONTROL_INSTANCE(v))
        return false;

    if (widget != NULL) {
        *widget_out = widget;
        return true;
    }

    /* Widget Qt absent : on tente le store headless. Recheck du kind pour
     * discriminer "headless legitime" vs "erreur kind". */
    int actualKind = -1;
    if (IS_UI_CONTROL(v)) {
        ObjUiControl* uic = AS_UI_CONTROL(v);
        actualKind = uic->kind;
    } else if (IS_UI_CONTROL_REF(v)) {
        ObjUiControlRef* ref = AS_UI_CONTROL_REF(v);
        ObjUiControlInstance* uici = resolve_ui_ref(ref);
        if (uici && uici->decl) actualKind = (int)uici->decl->kind;
        else actualKind = ref->kind;
    } else if (IS_UI_CONTROL_INSTANCE(v)) {
        ObjUiControlInstance* uici = AS_UI_CONTROL_INSTANCE(v);
        if (uici->decl) actualKind = (int)uici->decl->kind;
    }
    if (actualKind != (int)k1 && (k2 < 0 || actualKind != k2)) {
        /* L'erreur a deja ete emise par resolve_widget_kinded. */
        return false;
    }

    /* Cible le bon host pour le store : ObjUiControlInstance pour les
     * receveurs Instance/Ref (cas dominant DSL Fenetre), ObjUiControl pour
     * les ObjUiControl directs (cas legacy). */
    if (IS_UI_CONTROL_INSTANCE(v)) {
        ObjUiControlInstance* uici = AS_UI_CONTROL_INSTANCE(v);
        *items_out = create ? widget_items_store_get_or_create_uici(uici)
                            : widget_items_store_peek_uici(uici);
    } else if (IS_UI_CONTROL_REF(v)) {
        ObjUiControlRef* ref = AS_UI_CONTROL_REF(v);
        ObjUiControlInstance* uici = resolve_ui_ref(ref);
        if (!uici) return false;
        *items_out = create ? widget_items_store_get_or_create_uici(uici)
                            : widget_items_store_peek_uici(uici);
    } else { /* IS_UI_CONTROL */
        ObjUiControl* uic = AS_UI_CONTROL(v);
        *items_out = create ? widget_items_store_get_or_create_uic(uic)
                            : widget_items_store_peek_uic(uic);
    }
    /* En mode peek (create=false), items_out peut etre NULL si le store
     * n'a jamais ete materialise → caller traite comme count=0. */
    return true;
}
static bool resolve_list_target(Value v, const char* fnName, bool create,
                                OwlControl** widget_out, ObjArray** items_out) {
    return resolve_widget_items_target(v, fnName,
                                       OWL_CONTROL_LISTE, (int)OWL_CONTROL_COMBO,
                                       "Champ Liste ou Combo",
                                       create, widget_out, items_out);
}
static const char* list_value_to_text(Value v, ObjString** keep) {
    if (IS_STRING(v)) {
        *keep = NULL;
        return AS_CSTRING(v);
    }
    ObjString* s = value_to_string(v);
    *keep = s;
    return s ? s->chars : "";
}
static Value fn_liste_ajoute(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[ListeAjoute] (liste [, element [, valeurMemorisee [, image]]]) attendu.\n");
        return NIL_VAL;
    }
    /* Signature canon etendue OWL :
     *   (champ [, element [, valeurMemorisee [, cheminImage]]])
     * Sans <element> : ajoute une ligne vide. */
    OwlControl* widget = NULL;
    ObjArray* items   = NULL;
    if (!resolve_list_target(args[0], "ListeAjoute", true, &widget, &items))
        return NIL_VAL;

    ObjString *keepTxt = NULL, *keepStored = NULL, *keepImg = NULL;
    const char* txt    = (argCount >= 2) ? list_value_to_text(args[1], &keepTxt) : "";
    const char* stored = (argCount >= 3) ? list_value_to_text(args[2], &keepStored) : NULL;
    const char* img    = (argCount >= 4) ? list_value_to_text(args[3], &keepImg) : NULL;
    if (widget != NULL) {
        owl_ui_listcombo_add_item(widget, txt, stored, img);
    } else if (items != NULL) {
        /* Headless : push le texte (ObjString) dans le store. Valeur
         * memorisee / image hors-scope V1 (cf. ticket ). */
        Value sv = OBJ_VAL(obj_copy_string(txt ? txt : "", txt ? (int)strlen(txt) : 0));
        vm_push(sv);  /* value_array_write peut grow → GC */
        value_array_write(&items->elements, sv);
        vm_pop();
    }
    (void)keepTxt; (void)keepStored; (void)keepImg;
    return NIL_VAL;
}
static Value fn_liste_insere(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[ListeInsere] (liste, indice [, element [, valeurMemorisee [, image]]]) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* widget = NULL;
    ObjArray* items   = NULL;
    if (!resolve_list_target(args[0], "ListeInsere", true, &widget, &items))
        return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[ListeInsere] indice doit etre un entier.\n");
        return NIL_VAL;
    }
    int idx_1b = (int)AS_INTEGER(args[1]);
    ObjString *keepTxt = NULL, *keepStored = NULL, *keepImg = NULL;
    const char* txt    = (argCount >= 3) ? list_value_to_text(args[2], &keepTxt) : "";
    const char* stored = (argCount >= 4) ? list_value_to_text(args[3], &keepStored) : NULL;
    const char* img    = (argCount >= 5) ? list_value_to_text(args[4], &keepImg) : NULL;
    if (widget != NULL) {
        owl_ui_listcombo_insert_item(widget, idx_1b, txt, stored, img);
    } else if (items != NULL) {
        /* Headless : insert at idx_1b (1-based). Out-of-range → clamp aux
         * extremites comme owl_ui_listcombo_insert_item (Qt). */
        int idx = idx_1b - 1;
        if (idx < 0) idx = 0;
        if (idx > items->elements.count) idx = items->elements.count;
        Value sv = OBJ_VAL(obj_copy_string(txt ? txt : "", txt ? (int)strlen(txt) : 0));
        vm_push(sv);
        value_array_write(&items->elements, sv);  /* append, on shifte ensuite */
        vm_pop();
        if (idx < items->elements.count - 1) {
            memmove(&items->elements.values[idx + 1],
                    &items->elements.values[idx],
                    (size_t)(items->elements.count - 1 - idx) * sizeof(Value));
            items->elements.values[idx] = sv;
        }
    }
    (void)keepTxt; (void)keepStored; (void)keepImg;
    return NIL_VAL;
}
static Value fn_liste_supprime(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[ListeSupprime] (liste, indice) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* widget = NULL;
    ObjArray* items   = NULL;
    /* create=false : Supprime sur liste vide est legitime no-op, pas besoin
     * de materialiser le store pour rien. */
    if (!resolve_list_target(args[0], "ListeSupprime", false, &widget, &items))
        return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[ListeSupprime] indice doit etre un entier.\n");
        return NIL_VAL;
    }
    int idx_1b = (int)AS_INTEGER(args[1]);
    if (widget != NULL) {
        owl_ui_listcombo_remove_item(widget, idx_1b);
    } else if (items != NULL) {
        int idx = idx_1b - 1;
        if (idx < 0 || idx >= items->elements.count) return NIL_VAL;
        /* Shift left depuis idx+1. */
        if (idx < items->elements.count - 1) {
            memmove(&items->elements.values[idx],
                    &items->elements.values[idx + 1],
                    (size_t)(items->elements.count - 1 - idx) * sizeof(Value));
        }
        items->elements.count--;
    }
    return NIL_VAL;
}
static Value fn_liste_supprime_tout(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[ListeSupprimeTout] (liste) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* widget = NULL;
    ObjArray* items   = NULL;
    if (!resolve_list_target(args[0], "ListeSupprimeTout", false, &widget, &items))
        return NIL_VAL;
    if (widget != NULL) {
        owl_ui_listcombo_clear(widget);
    } else if (items != NULL) {
        items->elements.count = 0;  /* GC : valeurs orphelines, sweep les ramassera */
    }
    return NIL_VAL;
}
static Value fn_liste_modifie(int argCount, Value* args) {
    if (argCount < 3) {
        fprintf(stderr, "[ListeModifie] (liste, indice, element [, valeurMemorisee [, image]]) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* widget = NULL;
    ObjArray* items   = NULL;
    if (!resolve_list_target(args[0], "ListeModifie", false, &widget, &items))
        return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[ListeModifie] indice doit etre un entier.\n");
        return NIL_VAL;
    }
    int idx_1b = (int)AS_INTEGER(args[1]);
    ObjString *keepTxt = NULL, *keepStored = NULL, *keepImg = NULL;
    const char* txt    = list_value_to_text(args[2], &keepTxt);
    const char* stored = (argCount >= 4) ? list_value_to_text(args[3], &keepStored) : NULL;
    const char* img    = (argCount >= 5) ? list_value_to_text(args[4], &keepImg) : NULL;
    if (widget != NULL) {
        owl_ui_listcombo_modify_item(widget, idx_1b, txt, stored, img);
    } else if (items != NULL) {
        int idx = idx_1b - 1;
        if (idx < 0 || idx >= items->elements.count) {
            (void)keepTxt; (void)keepStored; (void)keepImg;
            return NIL_VAL;
        }
        Value sv = OBJ_VAL(obj_copy_string(txt ? txt : "", txt ? (int)strlen(txt) : 0));
        items->elements.values[idx] = sv;
    }
    (void)keepTxt; (void)keepStored; (void)keepImg;
    return NIL_VAL;
}
static Value fn_liste_affiche(int argCount, Value* args) {
    if (argCount < 1) return NIL_VAL;
    OwlControl* lst = list_arg_or_error(args[0], "ListeAffiche");
    if (lst) owl_ui_listcombo_refresh(lst);
    return NIL_VAL;
}
static Value fn_liste_cherche(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[ListeCherche] (liste, valeur) attendu.\n");
        return INTEGER_VAL(-1);
    }
    OwlControl* widget = NULL;
    ObjArray* items   = NULL;
    if (!resolve_list_target(args[0], "ListeCherche", false, &widget, &items))
        return INTEGER_VAL(-1);
    ObjString* keep = NULL;
    const char* needle = list_value_to_text(args[1], &keep);
    if (widget != NULL) {
        int idx = owl_ui_listcombo_find(widget, needle);
        (void)keep;
        return INTEGER_VAL(idx);
    }
    if (items != NULL) {
        /* Lookup texte exact, case-sensitive, 1-based. -1 si non trouve.
         * Aligne le contrat owl_ui_listcombo_find headless. */
        for (int i = 0; i < items->elements.count; i++) {
            Value v = items->elements.values[i];
            if (IS_STRING(v) && strcmp(AS_CSTRING(v), needle ? needle : "") == 0) {
                (void)keep;
                return INTEGER_VAL(i + 1);
            }
        }
    }
    (void)keep;
    return INTEGER_VAL(-1);
}
static Value fn_liste_occurrence(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    OwlControl* widget = NULL;
    ObjArray* items   = NULL;
    if (!resolve_list_target(args[0], "ListeOccurrence", false, &widget, &items))
        return INTEGER_VAL(0);
    if (widget != NULL) return INTEGER_VAL(owl_ui_listcombo_count(widget));
    if (items  != NULL) return INTEGER_VAL(items->elements.count);
    return INTEGER_VAL(0);
}
static Value fn_liste_trie(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[ListeTrie] (liste [, ordre]) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* lst = list_arg_or_error(args[0], "ListeTrie");
    if (!lst) return NIL_VAL;
    /* ordre : si 2eme arg est entier <=0 -> descendant, sinon ascendant.
     * Defaut = ascendant. Canon accepte aussi "+nom" / "-nom" (en V2 si
     * besoin pour Liste multicolonne ; en V1 mono-colonne, juste un sens). */
    int ascending = 1;
    if (argCount >= 2) {
        if (IS_INTEGER(args[1])) ascending = (AS_INTEGER(args[1]) >= 0) ? 1 : 0;
        else if (IS_STRING(args[1])) {
            const char* s = AS_CSTRING(args[1]);
            if (s && (s[0] == '-')) ascending = 0;
        }
    }
    owl_ui_listcombo_sort(lst, ascending);
    return NIL_VAL;
}
static int resolve_liste_idx(OwlControl* lst, int idx) {
    if (idx == OWL_LIGNE_SELECTIONNEE) {
        int sel = owl_ui_listcombo_get_selection(lst);
        return sel; /* peut etre 0 si rien selectionne */
    }
    return idx;
}
static Value fn_liste_select_plus(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[ListeSelectPlus] (liste, indice) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* lst = list_arg_or_error(args[0], "ListeSelectPlus");
    if (!lst) return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[ListeSelectPlus] indice doit etre un entier.\n");
        return NIL_VAL;
    }
    int idx = resolve_liste_idx(lst, (int)AS_INTEGER(args[1]));
    if (idx > 0) owl_ui_list_select_plus(lst, idx);
    return NIL_VAL;
}
static Value fn_liste_select_moins(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[ListeSelectMoins] (liste, indice) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* lst = list_arg_or_error(args[0], "ListeSelectMoins");
    if (!lst) return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[ListeSelectMoins] indice doit etre un entier.\n");
        return NIL_VAL;
    }
    int idx = resolve_liste_idx(lst, (int)AS_INTEGER(args[1]));
    if (idx > 0) owl_ui_list_select_moins(lst, idx);
    return NIL_VAL;
}
static Value fn_liste_select_occurrence(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    OwlControl* lst = list_arg_or_error(args[0], "ListeSelectOccurrence");
    if (!lst) return INTEGER_VAL(0);
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        int rang = (int)AS_INTEGER(args[1]);
        return INTEGER_VAL(owl_ui_list_select_at(lst, rang));
    }
    return INTEGER_VAL(owl_ui_list_select_count(lst));
}
static Value fn_liste_deplace_ligne(int argCount, Value* args) {
    if (argCount < 3) {
        fprintf(stderr, "[ListeDeplaceLigne] (liste, source, destination [, op]) attendu.\n");
        return INTEGER_VAL(0);
    }
    OwlControl* lst = list_arg_or_error(args[0], "ListeDeplaceLigne");
    if (!lst) return INTEGER_VAL(0);
    if (!IS_INTEGER(args[1]) || !IS_INTEGER(args[2])) {
        fprintf(stderr, "[ListeDeplaceLigne] source et destination doivent etre entier.\n");
        return INTEGER_VAL(0);
    }
    int src  = resolve_liste_idx(lst, (int)AS_INTEGER(args[1]));
    int dest = (int)AS_INTEGER(args[2]);
    int n    = owl_ui_listcombo_count(lst);
    if (n <= 0 || src <= 0 || src > n) return INTEGER_VAL(0);

    /* Resout cibles relatives. */
    int cur = (dest == OWL_LIGNE_SELECTIONNEE) ? owl_ui_listcombo_get_selection(lst) : src;
    if (dest == OWL_TD_PREMIER)              dest = 1;
    else if (dest == OWL_TD_DERNIER)         dest = n;
    else if (dest == OWL_TD_PRECEDENT)       dest = (cur > 1) ? cur - 1 : 1;
    else if (dest == OWL_TD_SUIVANT)         dest = (cur < n) ? cur + 1 : n;
    else if (dest == OWL_TD_PAGE_PRECEDENTE) dest = (cur > 10) ? cur - 10 : 1;
    else if (dest == OWL_TD_PAGE_SUIVANTE)   dest = (cur + 10 <= n) ? cur + 10 : n;
    else if (dest == OWL_LIGNE_SELECTIONNEE) dest = owl_ui_listcombo_get_selection(lst);

    if (dest <= 0 || dest > n) return INTEGER_VAL(0);

    /* Operation : bitmask combinable directement. tdDeplace=0 (defaut),
     * tdEchange=1, tdAffiche=2 — l'utilisateur peut faire 'tdEchange +
     * tdAfficheDestination' qui donne 3, traite comme bitmask. */
    int op_bits = (argCount >= 4 && IS_INTEGER(args[3])) ? (int)AS_INTEGER(args[3])
                                                          : 0;
    int newIdx = owl_ui_list_move_row(lst, src, dest, op_bits);
    return INTEGER_VAL(newIdx);
}
static Value fn_liste_info_xy(int argCount, Value* args) {
    if (argCount < 4) {
        fprintf(stderr, "[ListeInfoXY] (liste, type, X, Y) attendu.\n");
        return INTEGER_VAL(-1);
    }
    OwlControl* lst = list_arg_or_error(args[0], "ListeInfoXY");
    if (!lst) return INTEGER_VAL(-1);
    if (!IS_INTEGER(args[1]) || !IS_INTEGER(args[2]) || !IS_INTEGER(args[3])) {
        fprintf(stderr, "[ListeInfoXY] type, X, Y doivent etre entier.\n");
        return INTEGER_VAL(-1);
    }
    int type = (int)AS_INTEGER(args[1]);
    int x    = (int)AS_INTEGER(args[2]);
    int y    = (int)AS_INTEGER(args[3]);
    /* En V1 on supporte tiNumLigne (combinable avec tiOrigineEcran ignore) ;
     * tout autre type retourne -1. */
    int wantLigne = (type == OWL_TIXY_NUM_LIGNE
                  || type == (OWL_TIXY_NUM_LIGNE | OWL_TIXY_ORIGINE_ECRAN)
                  || (type & OWL_TIXY_NUM_LIGNE) != 0);
    if (!wantLigne) return INTEGER_VAL(-1);
    return INTEGER_VAL(owl_ui_list_hit_test(lst, x, y));
}
static Value fn_liste_select(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[ListeSelect] (liste [, indice]) attendu.\n");
        return INTEGER_VAL(0);
    }
    OwlControl* lst = list_arg_or_error(args[0], "ListeSelect");
    if (!lst) return INTEGER_VAL(0);
    if (argCount >= 2) {
        if (!IS_INTEGER(args[1])) {
            fprintf(stderr, "[ListeSelect] indice doit etre un entier.\n");
            return INTEGER_VAL(0);
        }
        owl_ui_listcombo_set_selection(lst, (int)AS_INTEGER(args[1]));
        return INTEGER_VAL((int)AS_INTEGER(args[1]));
    }
    return INTEGER_VAL(owl_ui_listcombo_get_selection(lst));
}
static OwlControl* combo_arg_or_error(Value v, const char* fnName) {
    /*  — cf. list_arg_or_error. */
    return resolve_widget_kinded(v, OWL_CONTROL_COMBO, -1,
                                 fnName, "Champ Combo");
}
static Value fn_combo_ouvre(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[ComboOuvre] (combo) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* c = combo_arg_or_error(args[0], "ComboOuvre");
    if (c) owl_ui_combo_open(c);
    return NIL_VAL;
}
static Value fn_combo_ferme(int argCount, Value* args) {
    if (argCount < 1) {
        fprintf(stderr, "[ComboFerme] (combo) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* c = combo_arg_or_error(args[0], "ComboFerme");
    if (c) owl_ui_combo_close(c);
    return NIL_VAL;
}
static Value fn_selecteur_ajoute(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[SelecteurAjoute] (sel, libelle [, sousLib [, image]]) attendu.\n");
        return INTEGER_VAL(-1);
    }
    /*  — items store headless symetrique  Liste/Combo. */
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "SelecteurAjoute",
                                     OWL_CONTROL_SELECTEUR, -1,
                                     "Champ Selecteur",
                                     true, &widget, &items))
        return INTEGER_VAL(-1);
    ObjString *kT = NULL, *kS = NULL, *kI = NULL;
    const char* lbl = (argCount >= 2) ? list_value_to_text(args[1], &kT) : "";
    const char* sub = (argCount >= 3) ? list_value_to_text(args[2], &kS) : NULL;
    const char* img = (argCount >= 4) ? list_value_to_text(args[3], &kI) : NULL;
    if (widget != NULL) {
        owl_ui_selecteur_add_option(widget, lbl, sub, NULL, img);
        (void)kT; (void)kS; (void)kI;
        return INTEGER_VAL(owl_ui_selecteur_count(widget));
    }
    if (items != NULL) {
        /*  — instance d'option, pas chaine nue : le store est la source
         * de verite de SEL_X[i] en headless, il doit ranger la meme forme que
         * celle semee par `Options = [...]`. */
        ObjInstance* opt = option_new_instance(false, lbl, sub, NULL, 0);
        if (opt) obj_array_push(items, OBJ_VAL(opt));
        (void)kT; (void)kS; (void)kI;
        return INTEGER_VAL(items->elements.count);
    }
    (void)kT; (void)kS; (void)kI;
    return INTEGER_VAL(-1);
}
static Value fn_selecteur_insere(int argCount, Value* args) {
    if (argCount < 3) {
        fprintf(stderr, "[SelecteurInsere] (sel, indice, libelle [, sousLib [, image]]) attendu.\n");
        return INTEGER_VAL(-1);
    }
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "SelecteurInsere",
                                     OWL_CONTROL_SELECTEUR, -1,
                                     "Champ Selecteur",
                                     true, &widget, &items))
        return INTEGER_VAL(-1);
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[SelecteurInsere] indice doit etre un entier.\n");
        return INTEGER_VAL(-1);
    }
    int idx = (int)AS_INTEGER(args[1]);
    ObjString *kT = NULL, *kS = NULL, *kI = NULL;
    const char* lbl = list_value_to_text(args[2], &kT);
    const char* sub = (argCount >= 4) ? list_value_to_text(args[3], &kS) : NULL;
    const char* img = (argCount >= 5) ? list_value_to_text(args[4], &kI) : NULL;
    if (widget != NULL) {
        owl_ui_selecteur_insert_option(widget, idx, lbl, sub, NULL, img);
        (void)kT; (void)kS; (void)kI;
        return INTEGER_VAL(idx);
    }
    if (items != NULL) {
        int i = idx - 1;
        if (i < 0) i = 0;
        if (i > items->elements.count) i = items->elements.count;
        /*  — meme forme d'element que l'ajout et que le seed. */
        ObjInstance* opt = option_new_instance(false, lbl, sub, NULL, 0);
        if (opt == NULL) { (void)kT; (void)kS; (void)kI; return INTEGER_VAL(idx); }
        Value sv = OBJ_VAL(opt);
        obj_array_push(items, sv);
        if (i < items->elements.count - 1) {
            memmove(&items->elements.values[i + 1],
                    &items->elements.values[i],
                    (size_t)(items->elements.count - 1 - i) * sizeof(Value));
            items->elements.values[i] = sv;
        }
    }
    (void)kT; (void)kS; (void)kI;
    return INTEGER_VAL(idx);
}
static Value fn_selecteur_supprime(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[SelecteurSupprime] (sel, indice) attendu.\n");
        return NIL_VAL;
    }
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "SelecteurSupprime",
                                     OWL_CONTROL_SELECTEUR, -1,
                                     "Champ Selecteur",
                                     false, &widget, &items))
        return NIL_VAL;
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[SelecteurSupprime] indice doit etre un entier.\n");
        return NIL_VAL;
    }
    int idx_1b = (int)AS_INTEGER(args[1]);
    if (widget != NULL) {
        owl_ui_selecteur_remove_option(widget, idx_1b);
    } else if (items != NULL) {
        int i = idx_1b - 1;
        if (i < 0 || i >= items->elements.count) return NIL_VAL;
        if (i < items->elements.count - 1) {
            memmove(&items->elements.values[i],
                    &items->elements.values[i + 1],
                    (size_t)(items->elements.count - 1 - i) * sizeof(Value));
        }
        items->elements.count--;
    }
    return NIL_VAL;
}
static Value fn_selecteur_occurrence(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "SelecteurOccurrence",
                                     OWL_CONTROL_SELECTEUR, -1,
                                     "Champ Selecteur",
                                     false, &widget, &items))
        return INTEGER_VAL(0);
    if (widget != NULL) return INTEGER_VAL(owl_ui_selecteur_count(widget));
    if (items  != NULL) return INTEGER_VAL(items->elements.count);
    return INTEGER_VAL(0);
}
static Value fn_interrupteur_ajoute(int argCount, Value* args) {
    if (argCount < 2) {
        fprintf(stderr, "[InterrupteurAjoute] (int, libelle [, sousLib [, image]]) attendu.\n");
        return INTEGER_VAL(-1);
    }
    /*  — items store headless symetrique  Liste/Combo. */
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "InterrupteurAjoute",
                                     OWL_CONTROL_INTERRUPTEUR, -1,
                                     "Champ Interrupteur",
                                     true, &widget, &items))
        return INTEGER_VAL(-1);
    ObjString *kT = NULL, *kS = NULL, *kI = NULL;
    const char* lbl = (argCount >= 2) ? list_value_to_text(args[1], &kT) : "";
    const char* sub = (argCount >= 3) ? list_value_to_text(args[2], &kS) : NULL;
    const char* img = (argCount >= 4) ? list_value_to_text(args[3], &kI) : NULL;
    if (widget != NULL) {
        owl_ui_interrupteur_add_option(widget, lbl, sub, img);
        (void)kT; (void)kS; (void)kI;
        return INTEGER_VAL(owl_ui_interrupteur_count(widget));
    }
    if (items != NULL) {
        /*  — instance d'option, pas chaine nue (cf fn_selecteur_ajoute). */
        ObjInstance* opt = option_new_instance(true, lbl, sub, NULL, 0);
        if (opt) obj_array_push(items, OBJ_VAL(opt));
        (void)kT; (void)kS; (void)kI;
        return INTEGER_VAL(items->elements.count);
    }
    (void)kT; (void)kS; (void)kI;
    return INTEGER_VAL(-1);
}
static Value fn_interrupteur_insere(int argCount, Value* args) {
    if (argCount < 3) {
        fprintf(stderr, "[InterrupteurInsere] (int, idx, libelle [, sousLib [, image]]) attendu.\n");
        return INTEGER_VAL(-1);
    }
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "InterrupteurInsere",
                                     OWL_CONTROL_INTERRUPTEUR, -1,
                                     "Champ Interrupteur",
                                     true, &widget, &items))
        return INTEGER_VAL(-1);
    if (!IS_INTEGER(args[1])) {
        fprintf(stderr, "[InterrupteurInsere] indice doit etre un entier.\n");
        return INTEGER_VAL(-1);
    }
    int idx = (int)AS_INTEGER(args[1]);
    ObjString *kT = NULL, *kS = NULL, *kI = NULL;
    const char* lbl = list_value_to_text(args[2], &kT);
    const char* sub = (argCount >= 4) ? list_value_to_text(args[3], &kS) : NULL;
    const char* img = (argCount >= 5) ? list_value_to_text(args[4], &kI) : NULL;
    if (widget != NULL) {
        owl_ui_interrupteur_insert_option(widget, idx, lbl, sub, img);
        (void)kT; (void)kS; (void)kI;
        return INTEGER_VAL(idx);
    }
    if (items != NULL) {
        int i = idx - 1;
        if (i < 0) i = 0;
        if (i > items->elements.count) i = items->elements.count;
        /*  — meme forme d'element que l'ajout et que le seed. */
        ObjInstance* opt = option_new_instance(true, lbl, sub, NULL, 0);
        if (opt == NULL) { (void)kT; (void)kS; (void)kI; return INTEGER_VAL(idx); }
        Value sv = OBJ_VAL(opt);
        obj_array_push(items, sv);
        if (i < items->elements.count - 1) {
            memmove(&items->elements.values[i + 1],
                    &items->elements.values[i],
                    (size_t)(items->elements.count - 1 - i) * sizeof(Value));
            items->elements.values[i] = sv;
        }
    }
    (void)kT; (void)kS; (void)kI;
    return INTEGER_VAL(idx);
}
static Value fn_interrupteur_supprime(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "InterrupteurSupprime",
                                     OWL_CONTROL_INTERRUPTEUR, -1,
                                     "Champ Interrupteur",
                                     false, &widget, &items))
        return NIL_VAL;
    if (!IS_INTEGER(args[1])) return NIL_VAL;
    int idx_1b = (int)AS_INTEGER(args[1]);
    if (widget != NULL) {
        owl_ui_interrupteur_remove_option(widget, idx_1b);
    } else if (items != NULL) {
        int i = idx_1b - 1;
        if (i < 0 || i >= items->elements.count) return NIL_VAL;
        if (i < items->elements.count - 1) {
            memmove(&items->elements.values[i],
                    &items->elements.values[i + 1],
                    (size_t)(items->elements.count - 1 - i) * sizeof(Value));
        }
        items->elements.count--;
    }
    return NIL_VAL;
}
static Value fn_interrupteur_occurrence(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "InterrupteurOccurrence",
                                     OWL_CONTROL_INTERRUPTEUR, -1,
                                     "Champ Interrupteur",
                                     false, &widget, &items))
        return INTEGER_VAL(0);
    if (widget != NULL) return INTEGER_VAL(owl_ui_interrupteur_count(widget));
    if (items  != NULL) return INTEGER_VAL(items->elements.count);
    return INTEGER_VAL(0);
}
/* ============================================================
 * Champ ZoneRepetee — runtime tracking + natives
 *
 * Pour chaque ZR ouverte, on tient un ZrRuntime qui memorise :
 *  - le OwlWindowDecl owner (= la fenetre source)
 *  - les indices dans owner_decl->controls des sous-decls qui forment le
 *    template (= ceux dont parent_name == zr->name)
 *  - la liste des reps actuellement materialisees (chacune = un QWidget
 *    container avec ses sous-widgets cree par build_control)
 *
 * La registration est faite par zr_register_at_open() appelee depuis le
 * loop de open_window juste apres la creation du widget ZR.
 *
 * Le clonage du template a chaque AjouteLigne reutilise build_control.
 * ============================================================ */

typedef struct {
    OwlControl* rep_widget;   /* QWidget container hebergeant les sub-widgets de la rep */
} ZrRep;

typedef struct {
    OwlControl*    zr_widget;     /* le ZR Qt widget (clef principale) */
    OwlWindowDecl* owner_decl;
    int*           template_indices;
    int            template_count;
    ZrRep*         reps;
    int            rep_count;
    int            rep_capacity;
    /* Override hauteur d'une repetition en pixels (prop HauteurRepetition).
     * 0 = pas d'override (calcul auto bounding box des sub-widgets). */
    int            height_override;
} ZrRuntime;

#define ZR_MAX_RUNTIME 128
static ZrRuntime* g_zr_runtimes[ZR_MAX_RUNTIME];
static int        g_zr_count = 0;

static ZrRuntime* zr_runtime_for(OwlControl* zr_widget) {
    if (!zr_widget) return NULL;
    for (int i = 0; i < g_zr_count; i++) {
        if (g_zr_runtimes[i] && g_zr_runtimes[i]->zr_widget == zr_widget)
            return g_zr_runtimes[i];
    }
    return NULL;
}
static void zr_register_at_open(OwlControl* zr_widget,
                                 const OwlWindowDecl* decl,
                                 const char* zr_name) {
    if (!zr_widget || !decl || !zr_name) return;
    if (g_zr_count >= ZR_MAX_RUNTIME) return;
    /* Compte d'abord. */
    int n = 0;
    for (int i = 0; i < decl->control_count; i++) {
        if (decl->controls[i].parent_name &&
            strcasecmp(decl->controls[i].parent_name, zr_name) == 0) n++;
    }
    ZrRuntime* rt = (ZrRuntime*)calloc(1, sizeof(ZrRuntime));
    rt->zr_widget = zr_widget;
    rt->owner_decl = (OwlWindowDecl*)decl;  /* read-only via ZrRuntime */
    rt->template_count = n;
    rt->template_indices = n > 0 ? (int*)calloc(n, sizeof(int)) : NULL;
    int k = 0;
    for (int i = 0; i < decl->control_count; i++) {
        if (decl->controls[i].parent_name &&
            strcasecmp(decl->controls[i].parent_name, zr_name) == 0) {
            rt->template_indices[k++] = i;
        }
    }
    /* Lit la prop HauteurRepetition sur le decl de la ZR elle-meme (pas sur
     * les sous-widgets). 0 = override desactive. */
    for (int i = 0; i < decl->control_count; i++) {
        OwlControlDecl* c = &decl->controls[i];
        if (c->kind == OWL_CONTROL_ZONE_REPETEE &&
            c->name && strcasecmp(c->name, zr_name) == 0) {
            rt->height_override = prop_int(c->properties, c->property_count,
                                            "HauteurRepetition", "RepetitionHeight", 0);
            break;
        }
    }
    g_zr_runtimes[g_zr_count++] = rt;
}
static bool zr_decl_is_zr_name(const OwlWindowDecl* decl, const char* name) {
    if (!decl || !name) return false;
    for (int i = 0; i < decl->control_count; i++) {
        OwlControlDecl* c = &decl->controls[i];
        if (c->kind == OWL_CONTROL_ZONE_REPETEE &&
            c->name && strcasecmp(c->name, name) == 0) return true;
    }
    return false;
}
static OwlControl* zr_arg_or_error(Value v, const char* fnName) {
    if (IS_UI_CONTROL(v)) {
        ObjUiControl* uic = AS_UI_CONTROL(v);
        if (uic->kind == OWL_CONTROL_ZONE_REPETEE)
            return (OwlControl*)uic->widget;
    }
    /*  — proxy global FEN_X.ZR_Y. resolve_ui_ref tape le control
     * de la fenetre top-of-stack. */
    if (IS_UI_CONTROL_REF(v)) {
        ObjUiControlInstance* uici = resolve_ui_ref(AS_UI_CONTROL_REF(v));
        if (uici && uici->decl && uici->decl->kind == OWL_CONTROL_ZONE_REPETEE)
            return (OwlControl*)uici->widget;
    }
    /*  — instance materialisee directe (cas exotique). */
    if (IS_UI_CONTROL_INSTANCE(v)) {
        ObjUiControlInstance* uici = AS_UI_CONTROL_INSTANCE(v);
        if (uici->decl && uici->decl->kind == OWL_CONTROL_ZONE_REPETEE)
            return (OwlControl*)uici->widget;
    }
    if (IS_STRING(v)) {
        const char* nm = AS_CSTRING(v);
        Value g;
        ObjString* k = obj_copy_string(nm, (int)strlen(nm));
        if (table_get(&vm.globals, k, &g) && IS_UI_CONTROL(g)) {
            ObjUiControl* uic = AS_UI_CONTROL(g);
            if (uic->kind == OWL_CONTROL_ZONE_REPETEE)
                return (OwlControl*)uic->widget;
        }
    }
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                          "arg", "1",
                          "expected", "Champ ZoneRepetee",
                          "actual", "valeur incompatible",
                          NULL);
    return NULL;
}
static OwlControl* zr_build_rep_widget(ZrRuntime* rt) {
    if (!rt) return NULL;
    int rep_w = 0, rep_h = 0;
    for (int t = 0; t < rt->template_count; t++) {
        OwlControlDecl* sub = &rt->owner_decl->controls[rt->template_indices[t]];
        int sx = prop_int(sub->properties, sub->property_count, "X", "X", 0);
        int sy = prop_int(sub->properties, sub->property_count, "Y", "Y", 0);
        int defH = (sub->kind == OWL_CONTROL_TABLE)  ? 300
                 : (sub->kind == OWL_CONTROL_BUTTON) ? 30
                 : 25;
        int sw = prop_int(sub->properties, sub->property_count, "Largeur", "Width", 100);
        int sh = prop_int(sub->properties, sub->property_count, "Hauteur", "Height", defH);
        if (sx + sw > rep_w) rep_w = sx + sw;
        if (sy + sh > rep_h) rep_h = sy + sh;
    }
    if (rep_w <= 0) rep_w = 200;
    if (rep_h <= 0) rep_h = 32;
    rep_w += 8; rep_h += 8;
    /* Override hauteur si HauteurRepetition pose sur la ZR (designer slider). */
    if (rt->height_override > 0) rep_h = rt->height_override;
    OwlControl* rep = owl_ui_container_create(
        owl_ui_zr_rep_as_parent(rt->zr_widget), 0, 0, rep_w, rep_h);
    if (!rep) return NULL;
    owl_ui_control_set_min_height(rep, rep_h);
    owl_ui_control_set_min_width (rep, rep_w);
    OwlWindow* rep_slot = owl_ui_container_as_parent(rep);
    for (int t = 0; t < rt->template_count; t++) {
        OwlControlDecl* sub = &rt->owner_decl->controls[rt->template_indices[t]];
        (void)build_control(rep_slot, sub, 0);
    }
    return rep;
}
static void zr_track_rep(ZrRuntime* rt, int idx_1based, OwlControl* rep) {
    if (rt->rep_count >= rt->rep_capacity) {
        int nc = rt->rep_capacity < 4 ? 4 : rt->rep_capacity * 2;
        rt->reps = (ZrRep*)realloc(rt->reps, nc * sizeof(ZrRep));
        rt->rep_capacity = nc;
    }
    int pos = (idx_1based >= 1 && idx_1based <= rt->rep_count)
            ? (idx_1based - 1) : rt->rep_count;
    /* Decale les reps suivantes a droite. */
    for (int i = rt->rep_count; i > pos; i--) {
        rt->reps[i] = rt->reps[i - 1];
    }
    rt->reps[pos].rep_widget = rep;
    rt->rep_count++;
}
static int zr_build_one_rep(ZrRuntime* rt) {
    if (!rt) return 0;
    /* Conteneur de la rep — calcul bounding box des sub-widgets avant
     * creation pour donner au rep une taille initiale qui le rend visible
     * dans le layout du QScrollArea. */
    int rep_w = 0, rep_h = 0;
    for (int t = 0; t < rt->template_count; t++) {
        OwlControlDecl* sub = &rt->owner_decl->controls[rt->template_indices[t]];
        int sx = prop_int(sub->properties, sub->property_count, "X", "X", 0);
        int sy = prop_int(sub->properties, sub->property_count, "Y", "Y", 0);
        int defH = (sub->kind == OWL_CONTROL_TABLE)  ? 300
                 : (sub->kind == OWL_CONTROL_BUTTON) ? 30
                 : 25;
        int sw = prop_int(sub->properties, sub->property_count, "Largeur", "Width", 100);
        int sh = prop_int(sub->properties, sub->property_count, "Hauteur", "Height", defH);
        if (sx + sw > rep_w) rep_w = sx + sw;
        if (sy + sh > rep_h) rep_h = sy + sh;
    }
    if (rep_w <= 0) rep_w = 200;
    if (rep_h <= 0) rep_h = 32;
    rep_w += 8; rep_h += 8;  /* marge interieure */
    /* Override hauteur si HauteurRepetition pose sur la ZR (designer slider). */
    if (rt->height_override > 0) rep_h = rt->height_override;

    OwlControl* rep = owl_ui_container_create(
        owl_ui_zr_rep_as_parent(rt->zr_widget), 0, 0, rep_w, rep_h);
    if (!rep) return 0;
    /* Force une hauteur minimum pour que le QVBoxLayout du QScrollArea
     * donne au rep son espace propre (sinon collapse a 0). */
    owl_ui_control_set_min_height(rep, rep_h);
    owl_ui_control_set_min_width (rep, rep_w);

    /* Le rep cree ses sub-widgets via build_control. */
    OwlWindow* rep_slot = owl_ui_container_as_parent(rep);
    for (int t = 0; t < rt->template_count; t++) {
        OwlControlDecl* sub = &rt->owner_decl->controls[rt->template_indices[t]];
        OwlControl* sub_widget = build_control(rep_slot, sub, 0);
        (void)sub_widget;
    }
    int idx = owl_ui_zr_add_rep(rt->zr_widget, rep);
    /* Track. */
    if (rt->rep_count >= rt->rep_capacity) {
        int nc = rt->rep_capacity < 4 ? 4 : rt->rep_capacity * 2;
        rt->reps = (ZrRep*)realloc(rt->reps, nc * sizeof(ZrRep));
        rt->rep_capacity = nc;
    }
    rt->reps[rt->rep_count].rep_widget = rep;
    rt->rep_count++;
    return idx;
}
static void zr_fire_row_display(ZrRuntime* rt, int idx_1based) {
    if (!rt || idx_1based < 1) return;
    const char* zr_name = owl_ui_control_get_object_name(rt->zr_widget);
    if (!zr_name) return;
    for (int i = 0; i < rt->owner_decl->control_count; i++) {
        OwlControlDecl* c = &rt->owner_decl->controls[i];
        if (c->kind != OWL_CONTROL_ZONE_REPETEE) continue;
        if (strcmp(c->name, zr_name) != 0) continue;
        const OwlEventBlock* ev = find_event(c->events, c->event_count,
            "surAffichageLigne", "onRowDisplay");
        if (!ev || !ev->impl) return;
        /* Arity = 3 : MaFenetre + MonChamp + indice (cf
         * compile_event_block_body_with_params, with_monchamp=true). */
        vm_push(OBJ_VAL(ev->impl));
        vm_push(NIL_VAL);              /* MaFenetre */
        vm_push(NIL_VAL);              /* MonChamp */
        vm_push(INTEGER_VAL(idx_1based));
        vm_call_reentrant(OBJ_VAL(ev->impl), 3);
        vm_pop();  /* drop return value */
        return;
    }
}

/*  — cle reservee du champ cache portant la back-ref vers l'uici
 * source d'une rep ZR headless. Le `$` initial ne peut pas apparaitre dans
 * un nom de sous-controle (identifiant strippe), donc zero collision avec
 * une cellule. Constante propre au runtime Qt (aucune donnee de registre ne
 * l'utilise), reste donc cote stdlib_ui.c. */
#define OWL_ZR_REP_SRC_KEY "$zrsrc"

static ObjInstance* zr_build_headless_rep(ObjUiControlInstance* zr_uici) {
    ObjInstance* rep = obj_new_instance(NULL);
    if (!rep) return NULL;
    vm_push(OBJ_VAL(rep));  /* root pendant les inst_field_set_str (table grow) */
    const char* zr_name = (zr_uici->decl) ? zr_uici->decl->name : NULL;
    const OwlWindowDecl* wdecl =
        (zr_uici->owner) ? zr_uici->owner->decl : NULL;
    if (zr_name && wdecl) {
        for (int i = 0; i < wdecl->control_count; i++) {
            const OwlControlDecl* c = &wdecl->controls[i];
            if (c->parent_name && c->name &&
                strcasecmp(c->parent_name, zr_name) == 0) {
                inst_field_set_str(rep, c->name, "");
            }
        }
    }
    /*  — tag la rep ZR avec une back-ref vers l'uici source. Permet
     * a OP_STRUCT_SET (vm.c, branche ObjInstance generique) de coercer une
     * cellule selon le contrat de valeur de son sous-controle (Saisie ->
     * chaine, comme SAI..Valeur ) sans confondre avec une rep Table
     * (typee par colonne, jamais coercee). Cle reservee `$zrsrc` : ne peut
     * pas collisionner avec un nom de sous-controle (identifiant strippe).
     * La valeur est l'ObjUiControlInstance de la ZR (deja vivante dans
     * owner->controls), donc pas de cycle GC nouveau. */
    inst_field_set_value(rep, OWL_ZR_REP_SRC_KEY, OBJ_VAL(zr_uici));
    vm_pop();
    return rep;
}
static Value zr_rep_coerce_cell_impl(ObjInstance* rep, const char* field,
                                     Value v) {
    if (!rep || !field) return v;
    /* root v avant tout obj_copy_string / coerce : la 1re internalisation de
     * "$zrsrc" peut declencher un GC alors que v (si IS_OBJ) n'est pas encore
     * rootee. Pattern canon obj_ui_control_headless_set / . Un seul
     * push en tete, un pop symetrique avant chaque return situe apres. */
    if (IS_OBJ(v)) vm_push(v);
    Value src;
    if (!table_get(&rep->fields,
                   obj_copy_string(OWL_ZR_REP_SRC_KEY,
                                   (int)strlen(OWL_ZR_REP_SRC_KEY)),
                   &src)) {
        if (IS_OBJ(v)) vm_pop();
        return v;  /* pas une rep ZR (rep Table / struct user) */
    }
    if (!IS_UI_CONTROL_INSTANCE(src)) {
        if (IS_OBJ(v)) vm_pop();
        return v;
    }
    ObjUiControlInstance* zr_uici = AS_UI_CONTROL_INSTANCE(src);
    const char* zr_name = (zr_uici->decl) ? zr_uici->decl->name : NULL;
    const OwlWindowDecl* wdecl =
        (zr_uici->owner) ? zr_uici->owner->decl : NULL;
    if (!zr_name || !wdecl) {
        if (IS_OBJ(v)) vm_pop();
        return v;
    }
    for (int i = 0; i < wdecl->control_count; i++) {
        const OwlControlDecl* c = &wdecl->controls[i];
        if (c->parent_name && c->name &&
            strcasecmp(c->parent_name, zr_name) == 0 &&
            strcasecmp(c->name, field) == 0) {
            /* Contrat Saisie : la cellule est figee chaine (..Valeur).
             * v reste rootee pendant la coercion (qui peut allouer). */
            Value out = ui_input_coerce_string_prop((int)c->kind, "Valeur", v);
            if (IS_OBJ(v)) vm_pop();
            return out;
        }
    }
    if (IS_OBJ(v)) vm_pop();
    return v;
}
static const char* zr_kind_default_property(int kind) {
    const OwlControlKindInfo* tbl = owl_control_kinds_table();
    for (int i = 0; tbl[i].nameFR != NULL; i++) {
        if (!tbl[i].is_widget) continue;
        OwlControlKind k;
        if (owl_control_kind_of_info(&tbl[i], &k) && (int)k == kind) {
            return tbl[i].defaultProperty;
        }
    }
    return NULL;
}
static Value zr_rep_cell_subcontrol_impl(ObjInstance* rep, const char* field) {
    if (!rep || !field) return NIL_VAL;
    /*  : ce hook est appele pour TOUTE instance recevant `.champ`
     * (Table, struct user compris) AVANT toute garde. Le caller OP_STRUCT_GET
     * a deja POP la rep -> elle est un C local non-roote. On lookup le tag
     * `$zrsrc` SANS allouer (table_find_key_case_insensitive scanne, pas
     * d'obj_copy_string qui pourrait grow→GC→collecter rep). Critique pour
     * isoler Table : sa rep n'est pas taggee, on sort avant tout root. */
    Value src;
    ObjString* srcKey =
        table_find_key_case_insensitive(&rep->fields, OWL_ZR_REP_SRC_KEY);
    if (srcKey == NULL || !table_get(&rep->fields, srcKey, &src)) {
        return NIL_VAL;  /* pas une rep ZR (rep Table / struct user) */
    }
    if (!IS_UI_CONTROL_INSTANCE(src)) return NIL_VAL;
    ObjUiControlInstance* zr_uici = AS_UI_CONTROL_INSTANCE(src);
    /* Garde stricte : isole Table (jamais kind ZR). */
    if (!zr_uici->decl
        || zr_uici->decl->kind != OWL_CONTROL_ZONE_REPETEE) {
        return NIL_VAL;
    }
    const char* zr_name = zr_uici->decl->name;
    const OwlWindowDecl* wdecl =
        (zr_uici->owner) ? zr_uici->owner->decl : NULL;
    if (!zr_name || !wdecl) return NIL_VAL;

    /* Resout le sous-controle nomme `field` (et son kind) dans le decl. */
    int subKind = -1;
    for (int i = 0; i < wdecl->control_count; i++) {
        const OwlControlDecl* c = &wdecl->controls[i];
        if (c->parent_name && c->name &&
            strcasecmp(c->parent_name, zr_name) == 0 &&
            strcasecmp(c->name, field) == 0) {
            subKind = (int)c->kind;
            break;
        }
    }
    if (subKind < 0) return NIL_VAL;  /* `field` n'est pas un sous-controle */

    /* Lit le champ courant de la rep (scalaire seed ou ObjUiControl deja
     * materialise). Cle case-insensible : la rep stocke sous le nom de
     * declaration du sous-controle. */
    ObjString* slot = table_find_key_case_insensitive(&rep->fields, field);
    Value cur = NIL_VAL;
    if (slot != NULL) table_get(&rep->fields, slot, &cur);

    /* Idempotent : deja un ObjUiControl -> renvoyer tel quel. */
    if (IS_UI_CONTROL(cur)) return cur;

    /*  : le caller (OP_STRUCT_GET) a deja POP la rep de la stack —
     * elle est un C local non-roote, comme `cur` qui vit dans rep->fields.
     * Toute alloc ci-dessous (Table headless, obj_copy_string, table grow)
     * peut declencher un GC qui collecte rep (et donc cur). On root rep en
     * tete : cur reste joignable via rep->fields tant que rep vit. */
    vm_push(OBJ_VAL(rep));
    /* Materialise : ObjUiControl(kind, NULL, name). Root pendant les sets. */
    ObjUiControl* cell = obj_new_ui_control(subKind, NULL, field);
    vm_push(OBJ_VAL(cell));
    /* Seed la prop par defaut avec le scalaire courant (preserve  :
     * obj_ui_control_headless_set applique ui_input_coerce_string_prop). */
    const char* defProp = zr_kind_default_property(subKind);
    if (defProp != NULL && !IS_NIL(cur)) {
        obj_ui_control_headless_set(cell, defProp, cur);
    }
    /* Remplace le scalaire seed par l'ObjUiControl dans la rep (source de
     * verite unique pour les acces ..Prop suivants). cell reste rootee. */
    inst_field_set_value(rep, field, OBJ_VAL(cell));
    vm_pop(); /* cell */
    vm_pop(); /* rep */
    return OBJ_VAL(cell);
}
static Value fn_zr_ajoute_ligne(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    /*  — items store headless symetrique  Liste/Combo. Le ZR
     * en mode Qt instancie un sous-arbre de widgets via zr_build_one_rep ;
     * en headless on materialise une rep-proxy ObjInstance (,
     * ) au lieu d'un NIL_VAL placeholder, pour que ZR[i].champ
     * fonctionne hors fenetre Qt. */
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "ZoneRepeteeAjouteLigne",
                                     OWL_CONTROL_ZONE_REPETEE, -1,
                                     "Champ ZoneRepetee",
                                     true, &widget, &items))
        return INTEGER_VAL(0);
    if (widget != NULL) {
        ZrRuntime* rt = zr_runtime_for(widget);
        if (!rt) return INTEGER_VAL(0);
        int idx = zr_build_one_rep(rt);
        if (idx > 0) zr_fire_row_display(rt, idx);
        return INTEGER_VAL(idx);
    }
    if (items != NULL) {
        /* Resout l'uici de la ZR pour connaitre le template (noms des
         * sous-controles). args[0] est un Ref ou une Instance (deja
         * verifie par resolve_widget_items_target). */
        ObjUiControlInstance* zr_uici = NULL;
        if (IS_UI_CONTROL_REF(args[0]))
            zr_uici = resolve_ui_ref(AS_UI_CONTROL_REF(args[0]));
        else if (IS_UI_CONTROL_INSTANCE(args[0]))
            zr_uici = AS_UI_CONTROL_INSTANCE(args[0]);
        ObjInstance* rep = zr_uici ? zr_build_headless_rep(zr_uici) : NULL;
        if (rep) {
            /* canon obj_array_push : root arr+val avant le grow (cf ). */
            obj_array_push(items, OBJ_VAL(rep));
        } else {
            value_array_write(&items->elements, NIL_VAL);
        }
        return INTEGER_VAL(items->elements.count);
    }
    return INTEGER_VAL(0);
}
static Value widget_instance_get_subitem(ObjUiControlInstance* uici, int idx) {
    if (!uici || !uici->decl) return NIL_VAL;
    /*  — Sélecteur / Interrupteur : GUI = proxy sur le widget (source de
     * verite de l'affichage) ; headless = l'element du store, deja une instance
     * d'option. Hors plage : option vide typee, pour que SEL_X[oob]..Libellé
     * reste lisible apres la soft-error posee par la VM (contrat Table/ZR). */
    if (uici->decl->kind == OWL_CONTROL_SELECTEUR ||
        uici->decl->kind == OWL_CONTROL_INTERRUPTEUR) {
        bool inter = (uici->decl->kind == OWL_CONTROL_INTERRUPTEUR);
        if (uici->widget != NULL) {
            Value v = option_proxy_from_widget(inter, (OwlControl*)uici->widget,
                                               idx);
            if (!IS_NIL(v)) return v;
        } else {
            ObjArray* items = widget_items_store_peek_uici(uici);
            if (idx >= 1 && items && idx <= items->elements.count) {
                Value ov = items->elements.values[idx - 1];
                /*  — headless : la source de verite par option est le
                 * casier cache __int_coche / __int_tristate. On rafraichit les
                 * champs visibles Valeur / TroisEtats juste avant de rendre
                 * l'instance, pour que la lecture nue (INT_X[i]) et la forme
                 * ..Valeur relisent toujours l'etat courant (et que le -1
                 * refuse d'une ecriture invalide n'ait jamais persiste). */
                if (inter && IS_INSTANCE(ov))
                    option_headless_refresh_coche(AS_INSTANCE(ov));
                return ov;
            }
        }
        ObjInstance* empty = option_new_instance(inter, "", "", "", 0);
        return empty ? OBJ_VAL(empty) : NIL_VAL;
    }
    if (uici->decl->kind == OWL_CONTROL_ZONE_REPETEE && uici->widget != NULL) {
        OwlControl* rep = owl_ui_zr_get_rep((OwlControl*)uici->widget, idx);
        if (!rep) {
            /*  — ZR GUI hors plage : rep vide typee (chaine vide par
             * sous-controle) pour que ZR[oob].champ reste lisible apres la
             * soft-error posee par la VM (harmonise sur Table ). */
            ObjInstance* empty = zr_build_headless_rep(uici);
            return empty ? OBJ_VAL(empty) : NIL_VAL;
        }
        ObjUiControl* proxy = obj_new_ui_control((int)OWL_CONTROL_CONTAINER,
                                                  rep,
                                                  uici->decl ? uici->decl->name
                                                             : NULL);
        return OBJ_VAL(proxy);
    }
    if (uici->decl->kind != OWL_CONTROL_ZONE_REPETEE &&
        uici->decl->kind != OWL_CONTROL_TABLE) return NIL_VAL;
    /* ZR headless / Table : lit la rep-proxy ObjInstance du store. */
    ObjArray* items = widget_items_store_peek_uici(uici);
    if (idx >= 1 && items && idx <= items->elements.count)
        return items->elements.values[idx - 1];
    /*  (Table) +  (ZR) — lecture cellule hors plage : rep
     * vide typee (chaine vide par colonne / sous-controle) pour que
     * TBL[oob].COL / ZR[oob].champ reste lisible apres la soft-error posee
     * par la VM. */
    if (uici->decl->kind == OWL_CONTROL_TABLE) {
        ObjInstance* empty = table_build_headless_rep(uici, NULL, 0);
        return empty ? OBJ_VAL(empty) : NIL_VAL;
    }
    ObjInstance* empty = zr_build_headless_rep(uici);
    return empty ? OBJ_VAL(empty) : NIL_VAL;
}
static Value fn_zr_insere(int argCount, Value* args) {
    if (argCount < 2) return INTEGER_VAL(0);
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "ZoneRepeteeInsere",
                                     OWL_CONTROL_ZONE_REPETEE, -1,
                                     "Champ ZoneRepetee",
                                     true, &widget, &items))
        return INTEGER_VAL(0);
    if (!IS_INTEGER(args[1])) return INTEGER_VAL(0);
    int idx = (int)AS_INTEGER(args[1]);
    if (widget != NULL) {
        ZrRuntime* rt = zr_runtime_for(widget);
        if (!rt) return INTEGER_VAL(0);
        int count = owl_ui_zr_count(widget);
        if (idx < 1) idx = 1;
        if (idx > count + 1) idx = count + 1;
        /* Construit le rep widget puis l'insere a la position demandee via
         * le backend Qt (owl_ui_zr_insert_rep gere le insertWidget dans le
         * QBoxLayout + decale les rep suivantes). */
        OwlControl* rep = zr_build_rep_widget(rt);
        if (!rep) return INTEGER_VAL(0);
        int new_idx = owl_ui_zr_insert_rep(widget, idx, rep);
        if (new_idx <= 0) return INTEGER_VAL(0);
        zr_track_rep(rt, new_idx, rep);
        zr_fire_row_display(rt, new_idx);
        return INTEGER_VAL(new_idx);
    }
    if (items != NULL) {
        int i = idx - 1;
        if (i < 0) i = 0;
        if (i > items->elements.count) i = items->elements.count;
        value_array_write(&items->elements, NIL_VAL);
        if (i < items->elements.count - 1) {
            memmove(&items->elements.values[i + 1],
                    &items->elements.values[i],
                    (size_t)(items->elements.count - 1 - i) * sizeof(Value));
            items->elements.values[i] = NIL_VAL;
        }
        return INTEGER_VAL(i + 1);
    }
    return INTEGER_VAL(0);
}
static Value fn_zr_supprime(int argCount, Value* args) {
    if (argCount < 2) return BOOL_VAL(false);
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "ZoneRepeteeSupprime",
                                     OWL_CONTROL_ZONE_REPETEE, -1,
                                     "Champ ZoneRepetee",
                                     false, &widget, &items))
        return BOOL_VAL(false);
    if (!IS_INTEGER(args[1])) return BOOL_VAL(false);
    int idx = (int)AS_INTEGER(args[1]);
    if (widget != NULL) {
        ZrRuntime* rt = zr_runtime_for(widget);
        if (!rt) return BOOL_VAL(false);
        int count = owl_ui_zr_count(widget);
        if (idx < 1 || idx > count) return BOOL_VAL(false);
        owl_ui_zr_remove_rep(widget, idx);
        if (idx <= rt->rep_count) {
            /* Decale le suivi des reps. */
            for (int i = idx - 1; i < rt->rep_count - 1; i++) {
                rt->reps[i] = rt->reps[i + 1];
            }
            rt->rep_count--;
        }
        return BOOL_VAL(true);
    }
    if (items != NULL) {
        int i = idx - 1;
        if (i < 0 || i >= items->elements.count) return BOOL_VAL(false);
        if (i < items->elements.count - 1) {
            memmove(&items->elements.values[i],
                    &items->elements.values[i + 1],
                    (size_t)(items->elements.count - 1 - i) * sizeof(Value));
        }
        items->elements.count--;
        return BOOL_VAL(true);
    }
    return BOOL_VAL(false);
}
static Value fn_zr_supprime_tout(int argCount, Value* args) {
    if (argCount < 1) return NIL_VAL;
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "ZoneRepeteeSupprimeTout",
                                     OWL_CONTROL_ZONE_REPETEE, -1,
                                     "Champ ZoneRepetee",
                                     false, &widget, &items))
        return NIL_VAL;
    if (widget != NULL) {
        ZrRuntime* rt = zr_runtime_for(widget);
        if (!rt) return NIL_VAL;
        owl_ui_zr_clear(widget);
        rt->rep_count = 0;
    } else if (items != NULL) {
        items->elements.count = 0;
    }
    return NIL_VAL;
}
static Value fn_zr_occurrence(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    OwlControl* widget = NULL;
    ObjArray* items = NULL;
    if (!resolve_widget_items_target(args[0], "ZoneRepeteeOccurrence",
                                     OWL_CONTROL_ZONE_REPETEE, -1,
                                     "Champ ZoneRepetee",
                                     false, &widget, &items))
        return INTEGER_VAL(0);
    if (widget != NULL) return INTEGER_VAL(owl_ui_zr_count(widget));
    if (items  != NULL) return INTEGER_VAL(items->elements.count);
    return INTEGER_VAL(0);
}
static Value fn_zr_affiche(int argCount, Value* args) {
    /* No-op : Qt repaint est deja gere par l'event loop. */
    if (argCount < 1) return NIL_VAL;
    (void)zr_arg_or_error(args[0], "ZoneRepeteeAffiche");
    return NIL_VAL;
}
extern ObjInstance* image_make_instance(OwlImage* img);  /* defini dans stdlib_image.c */

/* Cherche "Largeur"/"Width" ou "Hauteur"/"Height" dans les properties
 * const d'un decl. Retourne `fallback` si introuvable. Repris du Lot 1. */
static int copie_decl_const_int_prop(const OwlPropertyInit* props, int count,
                                      const char* nameFR, const char* nameEN,
                                      int fallback) {
    for (int i = 0; i < count; i++) {
        const OwlPropertyInit* p = &props[i];
        if (!p->is_constant || !p->name) continue;
        if (strcasecmp(p->name, nameFR) == 0
            || strcasecmp(p->name, nameEN) == 0) {
            if (IS_INTEGER(p->const_value))
                return (int)AS_INTEGER(p->const_value);
        }
    }
    return fallback;
}
static void copie_dims_from_window_decl(const OwlWindowDecl* wd,
                                         int* w, int* h) {
    int dw = 640, dh = 480;
    if (wd) {
        dw = copie_decl_const_int_prop(wd->properties, wd->property_count,
                                        "Largeur", "Width", 640);
        dh = copie_decl_const_int_prop(wd->properties, wd->property_count,
                                        "Hauteur", "Height", 480);
    }
    if (dw <= 0) dw = 640;
    if (dh <= 0) dh = 480;
    *w = dw; *h = dh;
}
static void copie_dims_from_control_decl(const OwlControlDecl* cd,
                                          int* w, int* h) {
    int dw = 100, dh = 30;
    if (cd) {
        dw = copie_decl_const_int_prop(cd->properties, cd->property_count,
                                        "Largeur", "Width", 100);
        dh = copie_decl_const_int_prop(cd->properties, cd->property_count,
                                        "Hauteur", "Height", 30);
    }
    if (dw <= 0) dw = 100;
    if (dh <= 0) dh = 30;
    *w = dw; *h = dh;
}
static ObjWindowInstance* copie_resolve_window(Value v) {
    const char* name = NULL;
    if (IS_WINDOW_REF(v)) {
        ObjWindowRef* wref = AS_WINDOW_REF(v);
        if (wref->name) name = wref->name->chars;
    } else if (IS_STRING(v)) {
        name = AS_CSTRING(v);
    } else if (IS_WINDOW_INSTANCE(v)) {
        return AS_WINDOW_INSTANCE(v);
    } else {
        return NULL;
    }
    if (!name || !*name) return NULL;
    for (int i = vm.window_stack_count - 1; i >= 0; i--) {
        ObjWindowInstance* wi = vm.window_stack[i];
        if (!wi || !wi->decl || !wi->decl->name) continue;
        if (strcasecmp(wi->decl->name, name) == 0) return wi;
    }
    return NULL;
}
static ObjUiControlInstance* copie_resolve_control(Value v) {
    if (IS_UI_CONTROL_INSTANCE(v)) return AS_UI_CONTROL_INSTANCE(v);
    if (IS_UI_CONTROL_REF(v)) {
        ObjUiControlRef* r = AS_UI_CONTROL_REF(v);
        if (!r->name) return NULL;
        for (int s = vm.window_stack_count - 1; s >= 0; s--) {
            ObjWindowInstance* wi = vm.window_stack[s];
            if (!wi) continue;
            for (int i = 0; i < wi->control_count; i++) {
                ObjUiControlInstance* uici = wi->controls[i];
                if (!uici || !uici->decl || !uici->decl->name) continue;
                if (strcmp(uici->decl->name, r->name->chars) == 0) return uici;
            }
        }
    }
    if (IS_UI_CONTROL(v)) {
        ObjUiControl* uc = AS_UI_CONTROL(v);
        if (!uc->name) return NULL;
        for (int s = vm.window_stack_count - 1; s >= 0; s--) {
            ObjWindowInstance* wi = vm.window_stack[s];
            if (!wi) continue;
            for (int i = 0; i < wi->control_count; i++) {
                ObjUiControlInstance* uici = wi->controls[i];
                if (!uici || !uici->decl || !uici->decl->name) continue;
                if (strcasecmp(uici->decl->name, uc->name) == 0) return uici;
            }
        }
    }
    return NULL;
}
static Value fn_champs_de(int argCount, Value* args) {
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));                 /* root pendant obj_copy_string (peut GC) */

    if (argCount == 1) {
        ObjWindowInstance* win = copie_resolve_window(args[0]);
        const char* containerName = NULL;  /* NULL = liste la fenetre elle-meme */

        if (!win) {
            /* Pas une fenetre : conteneur ? Resout l'instance de champ et son
             * owner ; on listera les champs de la fenetre owner filtres sur ce
             * conteneur. */
            ObjUiControlInstance* cont = copie_resolve_control(args[0]);
            if (cont && cont->owner && cont->decl && cont->decl->name) {
                win = cont->owner;
                containerName = cont->decl->name;
            }
        }

        if (win) {
            for (int i = 0; i < win->control_count; i++) {
                ObjUiControlInstance* c = win->controls[i];
                if (!c || !c->decl || !c->decl->name) continue;
                const char* pn = c->decl->parent_name;
                bool match = containerName
                    ? (pn && strcasecmp(pn, containerName) == 0)
                    : (pn == NULL);
                if (!match) continue;
                ObjString* nm = obj_copy_string(c->decl->name,
                                                (int)strlen(c->decl->name));
                obj_array_push(arr, OBJ_VAL(nm));   /* canon : root arr+val */
            }
        }
    }

    vm_pop();
    return OBJ_VAL(arr);
}
static const OwlMenuNode* menu_find_option_node(const OwlMenuNode* n,
                                                const char* name) {
    if (!n) return NULL;
    if (n->kind == OWL_MENU_NODE_OPTION && n->name
        && strcasecmp(n->name, name) == 0) {
        return n;
    }
    for (int i = 0; i < n->child_count; i++) {
        const OwlMenuNode* found = menu_find_option_node(n->children[i], name);
        if (found) return found;
    }
    return NULL;
}
static Value fn_test_simule_menu(int argCount, Value* args) {
    if (argCount != 2 && argCount != 3) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "TestSimuleMenu", NULL);
        return NIL_VAL;
    }
    ObjWindowInstance* wi = copie_resolve_window(args[0]);
    if (!wi || !wi->decl) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "TestSimuleMenu",
                              "arg", "fenetre", NULL);
        return NIL_VAL;
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "TestSimuleMenu",
                              "arg", "cheminOption",
                              "expected", "chaîne",
                              "actual", "autre type", NULL);
        return NIL_VAL;
    }
    const char* optName = AS_CSTRING(args[1]);
    const OwlWindowDecl* decl = wi->decl;

    /* 1. Menu principal effectif de la fenetre. */
    const char* menuName = window_effective_main_menu_name(wi, decl);
    const OwlMenuDecl* menu = (menuName && *menuName)
                              ? owl_window_decl_find_menu(decl, menuName)
                              : NULL;

    const OwlMenuNode* opt = NULL;
    if (menu) {
        for (int i = 0; i < menu->child_count && opt == NULL; i++) {
            opt = menu_find_option_node(menu->children[i], optName);
        }
    }

    /* 2.  — sinon, l'option peut appartenir a un menu contextuel de la
     * fenetre (attache a un champ ou au fond). En headless il n'y a pas de clic
     * droit reel : TestSimuleMenu resout l'option dans TOUS les arbres de menu
     * contextuel declares, sur le meme chemin de dispatch. */
    const OwlMenuDecl* found_context = NULL;
    for (int m = 0; m < decl->menu_count && opt == NULL; m++) {
        const OwlMenuDecl* cm = decl->menus[m];
        if (!cm || !cm->is_context) continue;
        for (int i = 0; i < cm->child_count && opt == NULL; i++) {
            opt = menu_find_option_node(cm->children[i], optName);
        }
        if (opt) found_context = cm;
    }

    if (!opt) {
        /* Distinguer "la fenetre n'a aucun menu" de "option introuvable". */
        bool has_any_menu = (menu != NULL);
        for (int m = 0; m < decl->menu_count && !has_any_menu; m++) {
            if (decl->menus[m] && decl->menus[m]->is_context) has_any_menu = true;
        }
        owl_emit_native_error(
            has_any_menu ? E_RT_UI_MENU_OPTION_UNKNOWN : E_RT_UI_WINDOW_NO_MENU,
            "TestSimuleMenu",
            has_any_menu ? "name" : "window",
            has_any_menu ? optName : (decl->name ? decl->name : "?"),
            NULL);
        return NIL_VAL;
    }

    const OwlEventBlock* sel = find_event(opt->events, opt->event_count,
                                          "surS\xc3\xa9lection", "onSelection");
    if (!sel || !sel->impl) return NIL_VAL;  /* option sans handler : no-op */

    /*  +  — sender pour MoiMeme (moimeme strippe le nom en
     * interne). Pour un menu CONTEXTUEL, MoiMeme doit etre le widget declencheur
     * (le champ auquel le menu est attache, ou la fenetre pour un menu de fond),
     * pas l'option — comme au vrai clic droit. En headless il n'y a pas de clic :
     * on retrouve le declencheur en cherchant le champ dont MenuContextuel
     * reference l'arbre trouve, sinon la fenetre elle-meme. */
    /*  — forme a 3 arguments : le 3e (proxy-colonne TABLE.COL) fixe la
     * colonne contexte de la loupe SANS clic droit reel. Il devient directement
     * l'emetteur du surSelection -> MoiMeme = cette colonne, branchable sur les
     * natifs Table. Meme mecanisme de push que le clic droit reel (dispatch_event
     * reconstruit le meme proxy depuis (table, colonne)). La forme a 2 arguments
     * garde son comportement d'origine (sender = le champ/fenetre attache). */
    if (argCount == 3) {
        call_event_block_with_sender(sel->impl, true, args[2]);
        return NIL_VAL;
    }

    const char* sender_name = opt->name;
    if (found_context != NULL) {
        sender_name = decl->name;   /* defaut defensif : la fenetre */
        for (int i = 0; i < decl->control_count; i++) {
            const OwlControlDecl* c = &decl->controls[i];
            if (c->context_menu_name
                && owl_window_decl_find_menu(decl, c->context_menu_name)
                       == found_context) {
                sender_name = c->name;
                break;
            }
        }
    }
    Value sender = moimeme_widget_value_for_name(sender_name);
    call_event_block_with_sender(sel->impl, true, sender);
    return NIL_VAL;
}
static OwlWindow* copie_headless_build_window(ObjWindowInstance* wi) {
    if (!wi || !wi->decl) return NULL;
    /* Sous owl test, QApplication n'est pas instanciee par defaut : sans
     * cette init le new QMainWindow() crashe. owl_ui_init est idempotent
     * (no-op si g_app deja en place). */
    owl_ui_init();
    /* Sauve l'etat avant build : on doit pouvoir restaurer apres. */
    void* saved_widget = wi->widget;
    /* Memorise aussi les widgets de controls existants pour les
     * restaurer apres detruire le widget Qt ad-hoc. */
    void** saved_ctrl_widgets = NULL;
    if (wi->control_count > 0) {
        saved_ctrl_widgets = (void**)calloc((size_t)wi->control_count, sizeof(void*));
        if (!saved_ctrl_widgets) return NULL;
        for (int i = 0; i < wi->control_count; i++) {
            saved_ctrl_widgets[i] = wi->controls[i] ? wi->controls[i]->widget : NULL;
        }
    }

    OwlWindow* w = build_window(wi->decl);

    /* Detache : remet l'instance dans l'etat "headless propre" pour ne
     * pas leak un pointeur dangling apres destroy(). */
    wi->widget = saved_widget;
    if (saved_ctrl_widgets) {
        for (int i = 0; i < wi->control_count; i++) {
            if (wi->controls[i]) wi->controls[i]->widget = saved_ctrl_widgets[i];
        }
        free(saved_ctrl_widgets);
    }
    return w;
}
static OwlImage* copie_image_from_grab(unsigned char* rgba, int w, int h,
                                        int fallback_w, int fallback_h) {
    if (rgba && w > 0 && h > 0) {
        OwlImage* img = owl_image_new_from_rgba(w, h, rgba);
        free(rgba);
        if (img && owl_image_valid(img)) return img;
        /* img alloue mais invalide : on jette et on tombe sur le fallback. */
        return owl_image_new(fallback_w, fallback_h, 0xFFF0F0F0u);
    }
    if (rgba) free(rgba);
    return owl_image_new(fallback_w, fallback_h, 0xFFF0F0F0u);
}
static Value fn_dCopieImageFenetre(int argCount, Value* args) {
    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dCopieImageFenêtre",
                              "native", "dCopieImageFenêtre",
                              "arg", "1",
                              "expected", "Fenêtre",
                              "actual", "rien",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    ObjWindowInstance* wi = copie_resolve_window(args[0]);
    if (!wi) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dCopieImageFenêtre",
                              "native", "dCopieImageFenêtre",
                              "arg", "1",
                              "expected", "Fenêtre ouverte",
                              "actual", "fenêtre non trouvée / non ouverte",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    /* 2e param ThèmeSnapshot : type-check au compile par
     * allowedBase=&E_ThemeSnapshot. Lot 3 dispatche sur l'enum :
     * themeDefaut/themeClair/themeSombre forcent la palette Qt
     * pendant le grab via push/pop. Sans 2e param = themeDefaut. */
    int theme = OWL_THEME_DEFAUT;
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        theme = AS_INTEGER(args[1]);
    }

    int fw, fh;
    copie_dims_from_window_decl(wi->decl, &fw, &fh);

    int grab_w = 0, grab_h = 0;
    unsigned char* rgba = NULL;
    OwlWindow* adhoc_window = NULL;

    if (wi->widget != NULL) {
        /* Mode normal display : widget Qt deja monte. */
        rgba = owl_ui_window_grab_rgba((OwlWindow*)wi->widget, theme, &grab_w, &grab_h);
    } else {
        /* Mode headless : on materialise un widget ad-hoc. */
        adhoc_window = copie_headless_build_window(wi);
        if (adhoc_window) {
            rgba = owl_ui_window_grab_rgba(adhoc_window, theme, &grab_w, &grab_h);
        }
    }

    OwlImage* img = copie_image_from_grab(rgba, grab_w, grab_h, fw, fh);

    if (adhoc_window) owl_ui_window_destroy(adhoc_window);

    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_CAPTURE_FAILURE, "dCopieImageFenêtre",
                              "native", "dCopieImageFenêtre",
                              "detail", "capture échouée",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    owl_clear_error();
    return OBJ_VAL(image_make_instance(img));
}
static Value fn_capture_fenetre(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) return BOOL_VAL(false);
    ObjWindowInstance* wi = copie_resolve_window(args[0]);
    if (!wi) return BOOL_VAL(false);
    const char* path = AS_CSTRING(args[1]);
    if (!path || !*path) return BOOL_VAL(false);

    int ok = 0;
    OwlWindow* adhoc_window = NULL;
    if (wi->widget != NULL) {
        ok = owl_ui_window_capture((OwlWindow*)wi->widget, path);
    } else {
        adhoc_window = copie_headless_build_window(wi);
        if (adhoc_window) ok = owl_ui_window_capture(adhoc_window, path);
    }
    if (adhoc_window) owl_ui_window_destroy(adhoc_window);
    return BOOL_VAL(ok != 0);
}
extern int owl_ui_menu_capture_submenu(OwlWindow* w, const char* submenu_name,
                                       const char* path);

/*  — __CaptureSousMenu(fen, nomSousMenu, chemin) : capture offscreen du
 * panneau deroule d'un SousMenu nomme vers un PNG, alpha preserve. Calque
 * __CaptureFenetre : materialise la fenetre en headless (widget non monte),
 * retrouve le QMenu dans l'arbre du menu, grabbe dans un pixmap ARGB, cleanup.
 * Les coins transparents (panneau arrondi + WA_TranslucentBackground) ressortent
 * transparents dans le PNG — sert a auto-verifier le fix du debord de fond. */
static Value fn_capture_sousmenu(int argCount, Value* args) {
    if (argCount < 3 || !IS_STRING(args[1]) || !IS_STRING(args[2]))
        return BOOL_VAL(false);
    ObjWindowInstance* wi = copie_resolve_window(args[0]);
    if (!wi) return BOOL_VAL(false);
    const char* nom  = AS_CSTRING(args[1]);
    const char* path = AS_CSTRING(args[2]);
    if (!nom || !*nom || !path || !*path) return BOOL_VAL(false);

    int ok = 0;
    OwlWindow* adhoc_window = NULL;
    if (wi->widget != NULL) {
        ok = owl_ui_menu_capture_submenu((OwlWindow*)wi->widget, nom, path);
    } else {
        adhoc_window = copie_headless_build_window(wi);
        if (adhoc_window) ok = owl_ui_menu_capture_submenu(adhoc_window, nom, path);
    }
    if (adhoc_window) owl_ui_window_destroy(adhoc_window);
    return BOOL_VAL(ok != 0);
}
extern int owl_ui_menu_option_has_icon(OwlWindow* w, const char* submenu_name,
                                       const char* option_label);

/*  — __DebugMenuIcone(fen, sousMenu, libelleOption) : booleen indiquant
 * si l'OptionMenu (designee par le libelle de son sous-menu et son propre
 * libelle) porte une icone effectivement resolue. Test-only (OWL_NAT_INTERNAL) :
 * verifie la resolution de OptionMenu..Image (chemin fichier OU reference
 * catalogue "icones.X") sans oracle pixel. Materialise la fenetre en headless
 * comme __CaptureSousMenu. Retourne Faux si fenetre/option introuvable. */
static Value fn_debug_menu_icone(int argCount, Value* args) {
    if (argCount < 3 || !IS_STRING(args[1]) || !IS_STRING(args[2]))
        return BOOL_VAL(false);
    ObjWindowInstance* wi = copie_resolve_window(args[0]);
    if (!wi) return BOOL_VAL(false);
    const char* sousmenu = AS_CSTRING(args[1]);
    const char* option   = AS_CSTRING(args[2]);
    if (!sousmenu || !*sousmenu || !option || !*option) return BOOL_VAL(false);

    int has = 0;
    OwlWindow* adhoc_window = NULL;
    if (wi->widget != NULL) {
        has = owl_ui_menu_option_has_icon((OwlWindow*)wi->widget, sousmenu, option);
    } else {
        adhoc_window = copie_headless_build_window(wi);
        if (adhoc_window)
            has = owl_ui_menu_option_has_icon(adhoc_window, sousmenu, option);
    }
    if (adhoc_window) owl_ui_window_destroy(adhoc_window);
    return BOOL_VAL(has != 0);
}
extern int owl_ui_table_cell_editable(OwlControl* table, int row_1based,
                                      int col_1based);

/*  — __DebugTableCelluleEditable(champ, ligne, colonne) : booleen
 * indiquant si un double-clic ouvrirait effectivement un editeur sur la cellule
 * (ligne/colonne 1-based). Test-only (OWL_NAT_INTERNAL) : lit l'etat Qt REEL
 * (Qt::ItemIsEditable de l'item ET edit-triggers de la vue), non observable en
 * headless. Retourne Faux si le champ est introuvable/non monte, non-Table, ou
 * indices hors bornes. */
static Value fn_debug_table_cellule_editable(int argCount, Value* args) {
    if (argCount < 3 || !IS_INTEGER(args[1]) || !IS_INTEGER(args[2]))
        return BOOL_VAL(false);
    ObjUiControlInstance* uici = copie_resolve_control(args[0]);
    if (!uici || !uici->widget) return BOOL_VAL(false);
    int row = (int)AS_INTEGER(args[1]);
    int col = (int)AS_INTEGER(args[2]);
    return BOOL_VAL(owl_ui_table_cell_editable((OwlControl*)uici->widget,
                                               row, col) != 0);
}
static Value fn_debug_table_cellule_masque(int argCount, Value* args) {
    if (argCount < 3 || !IS_INTEGER(args[1]) || !IS_INTEGER(args[2]))
        return OBJ_VAL(obj_copy_string("", 0));
    ObjUiControlInstance* uici = copie_resolve_control(args[0]);
    if (!uici || !uici->widget) return OBJ_VAL(obj_copy_string("", 0));
    int row = (int)AS_INTEGER(args[1]);
    int col = (int)AS_INTEGER(args[2]);
    const char* m = owl_ui_table_cell_editor_mask((OwlControl*)uici->widget,
                                                  row, col);
    if (!m) m = "";
    return OBJ_VAL(obj_copy_string(m, (int)strlen(m)));
}
static Value fn_debug_table_cellule_valide_saisie(int argCount, Value* args) {
    if (argCount < 4 || !IS_INTEGER(args[1]) || !IS_INTEGER(args[2]))
        return BOOL_VAL(true);
    ObjUiControlInstance* uici = copie_resolve_control(args[0]);
    if (!uici || !uici->widget) return BOOL_VAL(true);
    int row = (int)AS_INTEGER(args[1]);
    int col = (int)AS_INTEGER(args[2]);
    const char* txt = IS_STRING(args[3]) ? AS_CSTRING(args[3]) : "";
    return BOOL_VAL(owl_ui_table_cell_editor_valide_saisie(
                        (OwlControl*)uici->widget, row, col, txt) != 0);
}
static Value fn_debug_table_cellule_texte_affiche(int argCount, Value* args) {
    if (argCount < 3 || !IS_INTEGER(args[1]) || !IS_INTEGER(args[2]))
        return OBJ_VAL(obj_copy_string("", 0));
    ObjUiControlInstance* uici = copie_resolve_control(args[0]);
    if (!uici || !uici->widget) return OBJ_VAL(obj_copy_string("", 0));
    int row = (int)AS_INTEGER(args[1]);
    int col = (int)AS_INTEGER(args[2]);
    const char* t = owl_ui_table_cell_texte_affiche((OwlControl*)uici->widget,
                                                    row, col);
    if (!t) t = "";
    return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
}
extern void owl_ui_table_visible_column_values(OwlControl* table, int col0,
                                               char* out, int cap);

/*  — __DebugTableFiltreIN(TAB.COL, valeursRC) : pose un filtre IN{set}
 * via le VRAI chemin (store $tblfilters + recompute Qt), identique a la popup
 * checklist. Test-only (OWL_NAT_INTERNAL). Retrait via TableDesactiveFiltre. */
static Value fn_debug_table_filtre_in(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    ObjUiControlInstance* uici = NULL;
    char col[256];
    if (!table_colproxy_resolve(args[0], &uici, col, sizeof col)) return NIL_VAL;
    const char* valeurs = "";
    if (IS_STRING(args[1])) {
        valeurs = AS_CSTRING(args[1]);
    } else if (!IS_NIL(args[1])) {
        ObjString* s = value_to_string(args[1]);
        valeurs = s ? s->chars : "";
    }
    /* Copie locale dynamique : les set headless allouent (grow -> GC) et
     * invalideraient une ObjString fraiche non rootee. */
    size_t n = strlen(valeurs);
    char* buf = (char*)malloc(n + 1);
    if (!buf) return NIL_VAL;
    memcpy(buf, valeurs, n + 1);
    table_apply_filter_in_headless(uici, col, buf);
    free(buf);
    return NIL_VAL;
}
static Value fn_debug_table_colonne_visible(int argCount, Value* args) {
    if (argCount < 1) return OBJ_VAL(obj_copy_string("", 0));
    ObjUiControlInstance* uici = NULL;
    char col[256];
    if (!table_colproxy_resolve(args[0], &uici, col, sizeof col)
        || !uici->widget)
        return OBJ_VAL(obj_copy_string("", 0));
    int col0 = table_column_index_of(uici, col);
    if (col0 < 0) return OBJ_VAL(obj_copy_string("", 0));
    char buf[8192];
    buf[0] = '\0';
    owl_ui_table_visible_column_values((OwlControl*)uici->widget, col0,
                                       buf, (int)sizeof buf);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}
static Value fn_debug_table_valeurs_distinctes(int argCount, Value* args) {
    if (argCount < 1) return OBJ_VAL(obj_copy_string("", 0));
    ObjUiControlInstance* uici = NULL;
    char col[256];
    if (!table_colproxy_resolve(args[0], &uici, col, sizeof col))
        return OBJ_VAL(obj_copy_string("", 0));
    char* d = table_distinct_values_of(uici, col);
    if (!d) return OBJ_VAL(obj_copy_string("", 0));
    Value out = OBJ_VAL(obj_copy_string(d, (int)strlen(d)));
    free(d);
    return out;
}
static Value fn_debug_table_filtre_type(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    ObjUiControlInstance* uici = NULL;
    char col[256];
    if (!table_colproxy_resolve(args[0], &uici, col, sizeof col) || !uici->widget)
        return INTEGER_VAL(0);
    int col0 = table_column_index_of(uici, col);
    if (col0 < 0) return INTEGER_VAL(0);
    return INTEGER_VAL(
        owl_ui_table_user_get_filter_type((OwlControl*)uici->widget, col0));
}
static Value fn_debug_ombre_flou(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(-1);
    ObjUiControlInstance* uici = copie_resolve_control(args[0]);
    if (!uici || !uici->widget) return INTEGER_VAL(-1);
    return INTEGER_VAL(owl_ui_control_shadow_blur((OwlControl*)uici->widget));
}
static Value fn_simule_interaction(int argCount, Value* args) {
    if (argCount < 2 || !IS_INTEGER(args[1])) return NIL_VAL;
    ObjUiControlInstance* uici = copie_resolve_control(args[0]);
    if (!uici || !uici->widget) return NIL_VAL;
    owl_ui_control_push_interaction((OwlControl*)uici->widget,
                                    (int)AS_INTEGER(args[1]));
    return NIL_VAL;
}
static Value fn_dCopieImageChamp(int argCount, Value* args) {
    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dCopieImageChamp",
                              "native", "dCopieImageChamp",
                              "arg", "1",
                              "expected", "Champ",
                              "actual", "rien",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    ObjUiControlInstance* uici = copie_resolve_control(args[0]);
    if (!uici) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dCopieImageChamp",
                              "native", "dCopieImageChamp",
                              "arg", "1",
                              "expected", "Champ d'une fenêtre ouverte",
                              "actual", "champ non trouvé / non monté",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    /* 2e param ThèmeSnapshot (Lot 3) — type-check enum au compile. */
    int theme = OWL_THEME_DEFAUT;
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        theme = AS_INTEGER(args[1]);
    }

    int fw, fh;
    copie_dims_from_control_decl(uici->decl, &fw, &fh);

    int grab_w = 0, grab_h = 0;
    unsigned char* rgba = NULL;
    OwlWindow* adhoc_window = NULL;

    if (uici->widget != NULL) {
        rgba = owl_ui_control_grab_rgba((OwlControl*)uici->widget,
                                         theme, &grab_w, &grab_h);
    } else if (uici->owner != NULL) {
        /* Mode headless : build la fenetre parent ad-hoc, retrouve le
         * control freshly materialise (meme nom), grab, cleanup. */
        adhoc_window = copie_headless_build_window(uici->owner);
        if (adhoc_window && uici->decl && uici->decl->name) {
            /* Le QMainWindow est aussi un QWidget : findChild recursif depuis
             * la fenetre retrouve le sub-widget tagge par objectName. */
            OwlControl* freshly = owl_ui_control_find_child(
                (OwlControl*)adhoc_window, uici->decl->name);
            if (freshly) {
                rgba = owl_ui_control_grab_rgba(freshly, theme, &grab_w, &grab_h);
            }
        }
    }

    OwlImage* img = copie_image_from_grab(rgba, grab_w, grab_h, fw, fh);

    if (adhoc_window) owl_ui_window_destroy(adhoc_window);

    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_CAPTURE_FAILURE, "dCopieImageChamp",
                              "native", "dCopieImageChamp",
                              "detail", "capture échouée",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    owl_clear_error();
    return OBJ_VAL(image_make_instance(img));
}
static Value fn_dCopieImageEcran(int argCount, Value* args) {
    if (vm.headless_mode) {
        owl_emit_native_error(E_NATIVE_IMAGE_CAPTURE_FAILURE, "dCopieImageEcran",
                              "native", "dCopieImageEcran",
                              "detail", "Capture d'écran indisponible en mode headless (aucun display physique).",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    /* 1er param ThèmeSnapshot (Lot 3) — pas d'effet visible sur le grab
     * desktop (l'override palette agit sur les widgets Qt, pas le shell
     * OS), mais on lit la valeur pour interface coherente avec les
     * deux autres natives. */
    int theme = OWL_THEME_DEFAUT;
    if (argCount >= 1 && IS_INTEGER(args[0])) {
        theme = AS_INTEGER(args[0]);
    }
    int w = 0, h = 0;
    unsigned char* rgba = owl_ui_screen_grab_rgba(theme, &w, &h);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) free(rgba);
        owl_emit_native_error(E_NATIVE_IMAGE_CAPTURE_FAILURE, "dCopieImageEcran",
                              "native", "dCopieImageEcran",
                              "detail", "écran indisponible (QScreen=NULL)",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    OwlImage* img = owl_image_new_from_rgba(w, h, rgba);
    free(rgba);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_CAPTURE_FAILURE, "dCopieImageEcran",
                              "native", "dCopieImageEcran",
                              "detail", "capture écran échouée",
                              NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    owl_clear_error();
    return OBJ_VAL(image_make_instance(img));
}

/* Modes d'affichage fenetre + cle de store du mode courant. Partages avec les
 * donnees de registre (FENTAILLE_CONSTS, stdlib_ui_defs.c) : memes valeurs des
 * deux cotes du split Qt / Qt-free. */
#define OWL_FENTAILLE_NORMALE 0
#define OWL_FENTAILLE_MINI    1
#define OWL_FENTAILLE_MAXI    2
#define FENTAILLE_MODE_KEY "__owl_fentaille_mode"

static ObjWindowInstance* fentaille_resolve(Value v) {
    if (IS_STRING(v) && AS_STRING(v)->length == 0) return vm_window_stack_top();
    ObjWindowInstance* wi = copie_resolve_window(v);
    return wi ? wi : vm_window_stack_top();
}
static void fentaille_store_set_int(ObjWindowInstance* wi, const char* prop,
                                    int val) {
    Value v = INTEGER_VAL(val);
    obj_window_instance_headless_set(wi, prop, v);
    if (wi->widget) window_set_prop_qt((OwlWindow*)wi->widget, prop, v);
}
static void fentaille_set_mode(ObjWindowInstance* wi, int mode) {
    obj_window_instance_headless_set(wi, FENTAILLE_MODE_KEY, INTEGER_VAL(mode));
    if (wi->widget) owl_ui_window_set_display_mode((OwlWindow*)wi->widget, mode);
}
static int fentaille_get_mode(ObjWindowInstance* wi) {
    Value v;
    if (obj_window_instance_headless_get(wi, FENTAILLE_MODE_KEY, &v)
        && IS_INTEGER(v)) {
        return (int)AS_INTEGER(v);
    }
    return OWL_FENTAILLE_NORMALE;
}
static Value fn_fentaille(int argCount, Value* args) {
    ObjWindowInstance* wi = (argCount >= 1) ? fentaille_resolve(args[0])
                                            : vm_window_stack_top();
    if (!wi) return INTEGER_VAL(OWL_FENTAILLE_NORMALE);

    switch (argCount) {
        case 0:
        case 1:
            /* Lecture du mode d'affichage courant. */
            return INTEGER_VAL(fentaille_get_mode(wi));
        case 2:
            /* (fen, mode) : applique le mode d'affichage. */
            fentaille_set_mode(wi, IS_INTEGER(args[1]) ? (int)AS_INTEGER(args[1])
                                                       : OWL_FENTAILLE_NORMALE);
            return NIL_VAL;
        case 3:
            /* (fen, largeur, hauteur) : redimensionne. */
            if (IS_INTEGER(args[1]))
                fentaille_store_set_int(wi, "Largeur", (int)AS_INTEGER(args[1]));
            if (IS_INTEGER(args[2]))
                fentaille_store_set_int(wi, "Hauteur", (int)AS_INTEGER(args[2]));
            return NIL_VAL;
        case 5:
            /* (fen, x, y, largeur, hauteur) : deplace + redimensionne. */
            if (IS_INTEGER(args[1]))
                fentaille_store_set_int(wi, "X", (int)AS_INTEGER(args[1]));
            if (IS_INTEGER(args[2]))
                fentaille_store_set_int(wi, "Y", (int)AS_INTEGER(args[2]));
            if (IS_INTEGER(args[3]))
                fentaille_store_set_int(wi, "Largeur", (int)AS_INTEGER(args[3]));
            if (IS_INTEGER(args[4]))
                fentaille_store_set_int(wi, "Hauteur", (int)AS_INTEGER(args[4]));
            return NIL_VAL;
        default:
            return NIL_VAL;
    }
}
static Value fn_test_fenetre_icone_largeur(int argCount, Value* args) {
    ObjWindowInstance* wi = (argCount >= 1) ? copie_resolve_window(args[0])
                                            : vm_window_stack_top();
    if (!wi || !wi->widget) return INTEGER_VAL(-1);
    return INTEGER_VAL(owl_ui_window_get_icon_width((OwlWindow*)wi->widget));
}
static Value fentaille_shortcut(int argCount, Value* args, int mode) {
    ObjWindowInstance* wi = (argCount >= 1) ? fentaille_resolve(args[0])
                                            : vm_window_stack_top();
    if (wi) fentaille_set_mode(wi, mode);
    return NIL_VAL;
}
static Value fn_maximise(int argCount, Value* args) {
    return fentaille_shortcut(argCount, args, OWL_FENTAILLE_MAXI);
}
static Value fn_iconise(int argCount, Value* args) {
    return fentaille_shortcut(argCount, args, OWL_FENTAILLE_MINI);
}
static Value fn_restaure(int argCount, Value* args) {
    return fentaille_shortcut(argCount, args, OWL_FENTAILLE_NORMALE);
}

/* S.4 — indexation widget[i]. Pour SELECTEUR / INTERRUPTEUR : proxy d'option
 * avec fields snapshot + back-ref widget (option_proxy_from_widget).
 * L'OP_STRUCT_SET sur ce proxy detecte les fields __sel_* / __int_* et
 * dispatche vers le widget via on_instance_modified_impl. */
static Value selecteur_get_subitem(ObjUiControl* uic, int idx) {
    if (!uic) return NIL_VAL;
    OwlControlKind k = (OwlControlKind)uic->kind;
    OwlControl* w = (OwlControl*)uic->widget;

    if (k == OWL_CONTROL_SELECTEUR)
        return option_proxy_from_widget(false, w, idx);
    if (k == OWL_CONTROL_INTERRUPTEUR)
        return option_proxy_from_widget(true, w, idx);

    if (k == OWL_CONTROL_ZONE_REPETEE) {
        /* ZR[i] retourne un ObjUiControl(kind=CONTAINER) ciblant le QWidget
         * rep. STRUCT_GET sur ce proxy avec un nom (ex SAI_X) tombe dans
         * real_ui_get_prop -> fallback CONTAINER -> owl_ui_control_find_child
         * qui resout le sub-widget par objectName. */
        OwlControl* rep = owl_ui_zr_get_rep(w, idx);
        if (!rep) return NIL_VAL;
        ObjUiControl* proxy = obj_new_ui_control((int)OWL_CONTROL_CONTAINER,
                                                  rep, uic->name);
        return OBJ_VAL(proxy);
    }

    return NIL_VAL;
}

/*  — wrappers pour les function pointers de ui_decl.c. Register
 * par stdlib_ui_init. Le commentaire/rationale est dans ui_decl.h.
 *  — generalises kind-agnostic. */
static int widget_items_count_qt(int kind, void* widget) {
    OwlControl* w = (OwlControl*)widget;
    switch (kind) {
        case OWL_CONTROL_LISTE:
        case OWL_CONTROL_COMBO:        return owl_ui_listcombo_count(w);
        case OWL_CONTROL_SELECTEUR:    return owl_ui_selecteur_count(w);
        case OWL_CONTROL_INTERRUPTEUR: return owl_ui_interrupteur_count(w);
        case OWL_CONTROL_ZONE_REPETEE: return owl_ui_zr_count(w);
        default: return 0;
    }
}

static int headless_widget_items_count_uic_impl(ObjUiControl* uic) {
    ObjArray* arr = widget_items_store_peek_uic(uic);
    return arr ? arr->elements.count : 0;
}

static int headless_widget_items_count_uici_impl(ObjUiControlInstance* uici) {
    ObjArray* arr = widget_items_store_peek_uici(uici);
    return arr ? arr->elements.count : 0;
}

/* ============================================================
 * Backend Timer — QTimer parenté à la fenêtre courante.
 *
 * timer_start_impl / timer_stop_impl sont enregistrés via
 * owl_timer_register_backend ; ils overrident les no-op CLI de
 * stdlib_timer.c. Le QTimer tire pendant la boucle d'événements ; à chaque
 * tic, timer_tick_cb ré-entre dans la VM pour appeler la procédure stockée.
 * ============================================================ */

static bool timer_read_field(ObjInstance* inst, const char* name, Value* out) {
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_pop();
    return table_get(&inst->fields, k, out);
}

/* Appelé par le QTimer (ui_qt.cpp) à chaque déclenchement. Lit la procédure
 * live (réassignation à chaud prise en compte), l'appelle sans argument, et
 * marque _fired. Pour un one-shot (..Répète = Faux), Qt a déjà stoppé le
 * timer : on repasse ..Actif (via _active) à Faux. */
static void timer_tick_cb(void* owlInstance) {
    ObjInstance* inst = (ObjInstance*)owlInstance;
    if (inst == NULL) return;

    /* "Skip if busy" : un handler OWL (clic, saisie, autre tic) tourne déjà
     * et a rendu la main au dispatch Qt via une boucle imbriquée (Temporise,
     * Ouvre modal, dialogue). Exécuter la procédure du timer maintenant
     * l'empilerait sur un handler suspendu (event_sender erroné, ordre
     * surprenant). Un timer n'a pas besoin de rattrapage : on saute ce tic.
     * Note : _fired n'est PAS posé (le tic n'a pas réellement eu lieu). */
    if (owl_handler_busy()) return;

    owl_handler_enter();
    inst_field_set_bool(inst, "_fired", true);

    Value proc;
    if (timer_read_field(inst, "Procedure", &proc) && IS_CALLABLE(proc)) {
        vm_push(proc);
        vm_call_reentrant(proc, 0);
        vm_pop();  /* résultat du callback */
    }

    Value repeat;
    bool repeats = timer_read_field(inst, "Repete", &repeat)
                   && IS_BOOL(repeat) && AS_BOOL(repeat);
    if (!repeats) {
        inst_field_set_bool(inst, "_active", false);
    }
    owl_handler_leave();
}

static void timer_start_impl(ObjInstance* inst) {
    if (inst == NULL) return;
    Value interval, repeat;
    int ms = 0;
    if (timer_read_field(inst, "Intervalle", &interval)) {
        /*  — ..Intervalle typé durée : accepte ObjDuration (totalMs)
         * ET entier brut. Sinon le QTimer tirerait à 0ms (régression). */
        if (IS_DURATION(interval))      ms = (int)AS_DURATION(interval)->totalMs;
        else if (IS_INTEGER(interval))  ms = (int)AS_INTEGER(interval);
    }
    int rep = (timer_read_field(inst, "Repete", &repeat)
               && IS_BOOL(repeat)) ? (AS_BOOL(repeat) ? 1 : 0) : 1;
    /* g_current_window est l'OwlWindow* de la fenêtre couramment montée
     * (NULL hors d'un bloc fenêtre). Le QTimer y est parenté : il meurt
     * avec elle. Si on est bien dans une fenêtre, une boucle d'événements
     * pompera le timer : on le marque _gui_engaged pour que le warning de
     * fin de process (cible = script CLI sans pompe) ne le compte pas si
     * l'utilisateur ferme la fenêtre avant l'échéance d'un one-shot. */
    if (g_current_window != NULL) {
        owl_timer_mark_gui_engaged(inst);
    }
    owl_ui_qt_timer_start(g_current_window, (void*)inst, ms, rep, timer_tick_cb);
}

static void timer_stop_impl(ObjInstance* inst) {
    if (inst == NULL) return;
    owl_ui_qt_timer_stop((void*)inst);
}

/* ..Intervalle / ..Répète modifié à chaud sur un timer ACTIF : relit les
 * champs live et propage au QTimer vivant (setInterval/setSingleShot, pris en
 * compte au cycle Qt suivant). Pas de re-création de QTimer : le rythme change
 * sans Arrête/Démarre. */
static void timer_update_impl(ObjInstance* inst) {
    if (inst == NULL) return;
    Value interval, repeat;
    int ms = 0;
    if (timer_read_field(inst, "Intervalle", &interval)) {
        if (IS_DURATION(interval))      ms = (int)AS_DURATION(interval)->totalMs;
        else if (IS_INTEGER(interval))  ms = (int)AS_INTEGER(interval);
    }
    int rep = (timer_read_field(inst, "Repete", &repeat)
               && IS_BOOL(repeat)) ? (AS_BOOL(repeat) ? 1 : 0) : 1;
    owl_ui_qt_timer_update((void*)inst, ms, rep);
}

/* Injecte les implementations Qt (fn_*) dans l'array DEFS Qt-free de
 * stdlib_ui_defs.c. A appeler AVANT stdlib_ui_defs_register() (owl_stdlib.c,
 * ordre wire-before-register -> register_module prend le chemin normal et le
 * registre GUI reste byte-identique a l'historique). Build GUI uniquement. */
void stdlib_ui_impls_wire(void) {
    stdlib_ui_defs_set_impl("Info", fn_info);
    stdlib_ui_defs_set_impl("Erreur", fn_erreur);
    stdlib_ui_defs_set_impl("Avertissement", fn_avertissement);
    stdlib_ui_defs_set_impl("Sablier", fn_sablier);
    stdlib_ui_defs_set_impl("ToastAffiche", fn_toast_affiche);
    stdlib_ui_defs_set_impl("OuiNon", fn_ouinon);
    stdlib_ui_defs_set_impl("Confirmer", fn_confirmer);
    stdlib_ui_defs_set_impl("fSelecteur", fn_fselecteur);
    stdlib_ui_defs_set_impl("fRepSelecteur", fn_frepselecteur);
    stdlib_ui_defs_set_impl("TestModeHeadless", fn_test_mode_headless);
    stdlib_ui_defs_set_impl("TestSimuleSouris", fn_test_simule_souris);
    stdlib_ui_defs_set_impl("DeclencheEvenement", fn_declenche_evenement);
    stdlib_ui_defs_set_impl("TestSimuleSaisie", fn_test_simule_saisie);
    stdlib_ui_defs_set_impl("TestSimuleMenu", fn_test_simule_menu);
    stdlib_ui_defs_set_impl("TestFenetreIconeLargeur", fn_test_fenetre_icone_largeur);
    stdlib_ui_defs_set_impl("ChampsDe", fn_champs_de);
    stdlib_ui_defs_set_impl("DonneFocus", fn_donne_focus);
    stdlib_ui_defs_set_impl("__DebugFeuilleStyle", fn_debug_feuille_style);
    stdlib_ui_defs_set_impl("__DebugFeuilleStyleMenu", fn_debug_feuille_style_menu);
    stdlib_ui_defs_set_impl("__DebugFeuilleStyleMenuContextuel", fn_debug_feuille_style_menu_ctx);
    stdlib_ui_defs_set_impl("__DebugPoliceBarreMenu", fn_debug_police_barre_menu);
    stdlib_ui_defs_set_impl("__DebugPoliceSousMenu", fn_debug_police_sousmenu);
    stdlib_ui_defs_set_impl("__DebugPoliceChamp", fn_debug_police_champ);
    stdlib_ui_defs_set_impl("__DebugEncreBarreMenu", fn_debug_encre_barre_menu);
    stdlib_ui_defs_set_impl("__DebugDernierMessage", fn_debug_dernier_message);
    stdlib_ui_defs_set_impl("__CaptureFenetre", fn_capture_fenetre);
    stdlib_ui_defs_set_impl("__CaptureSousMenu", fn_capture_sousmenu);
    stdlib_ui_defs_set_impl("__DebugMenuIcone", fn_debug_menu_icone);
    stdlib_ui_defs_set_impl("__DebugTableCelluleEditable", fn_debug_table_cellule_editable);
    stdlib_ui_defs_set_impl("__DebugTableCelluleMasque", fn_debug_table_cellule_masque);
    stdlib_ui_defs_set_impl("__DebugTableCelluleValideSaisie", fn_debug_table_cellule_valide_saisie);
    stdlib_ui_defs_set_impl("__DebugTableCelluleTexteAffiche", fn_debug_table_cellule_texte_affiche);
    stdlib_ui_defs_set_impl("__DebugTableFiltreIN", fn_debug_table_filtre_in);
    stdlib_ui_defs_set_impl("__DebugTableColonneVisible", fn_debug_table_colonne_visible);
    stdlib_ui_defs_set_impl("__DebugTableValeursDistinctes", fn_debug_table_valeurs_distinctes);
    stdlib_ui_defs_set_impl("__DebugTableFiltreType", fn_debug_table_filtre_type);
    stdlib_ui_defs_set_impl("__DebugPositionImageBouton", fn_debug_position_image_bouton);
    stdlib_ui_defs_set_impl("__DebugBoutonIconeSurvol", fn_debug_bouton_icone_survol);
    stdlib_ui_defs_set_impl("__DebugOmbreFlou", fn_debug_ombre_flou);
    stdlib_ui_defs_set_impl("__SimuleInteraction", fn_simule_interaction);
    stdlib_ui_defs_set_impl("ToucheEnfoncee", fn_touche_enfoncee);
    stdlib_ui_defs_set_impl("EnvoieTouche", fn_envoie_touche);
    stdlib_ui_defs_set_impl("Ouvre", fn_ouvre);
    stdlib_ui_defs_set_impl("OuvreFille", fn_ouvre_fille);
    stdlib_ui_defs_set_impl("OuvreSoeur", fn_ouvre_soeur);
    stdlib_ui_defs_set_impl("Ferme", fn_ferme);
    stdlib_ui_defs_set_impl("DemandeMiseAJourIHM", fn_demande_mise_a_jour_ihm);
    stdlib_ui_defs_set_impl("FinProgramme", fn_fin_programme);
    stdlib_ui_defs_set_impl("FenTaille", fn_fentaille);
    stdlib_ui_defs_set_impl("Maximise", fn_maximise);
    stdlib_ui_defs_set_impl("Iconise", fn_iconise);
    stdlib_ui_defs_set_impl("Restaure", fn_restaure);
    stdlib_ui_defs_set_impl("TableAjouteLigne", fn_table_ajoute_ligne);
    stdlib_ui_defs_set_impl("TableSupprime", fn_table_supprime);
    stdlib_ui_defs_set_impl("TableSupprimeTout", fn_table_supprime_tout);
    stdlib_ui_defs_set_impl("TableOccurrence", fn_table_occurrence);
    stdlib_ui_defs_set_impl("TableCherche", fn_table_cherche);
    stdlib_ui_defs_set_impl("TableCherchePartout", fn_table_cherche_partout);
    stdlib_ui_defs_set_impl("TableModifieLigne", fn_table_modifie_ligne);
    stdlib_ui_defs_set_impl("TableEchangeLigne", fn_table_echange_ligne);
    stdlib_ui_defs_set_impl("TableDeplaceLigne", fn_table_deplace_ligne);
    stdlib_ui_defs_set_impl("TableSelectPlus", fn_table_select_plus);
    stdlib_ui_defs_set_impl("TableSelectMoins", fn_table_select_moins);
    stdlib_ui_defs_set_impl("TableSelect", fn_table_select);
    stdlib_ui_defs_set_impl("TableSelectOccurrence", fn_table_select_occurrence);
    stdlib_ui_defs_set_impl("TableTrie", fn_table_trie);
    stdlib_ui_defs_set_impl("TableActiveFiltre", fn_table_active_filtre);
    stdlib_ui_defs_set_impl("TableSaisieLoupe", fn_table_saisie_loupe);
    stdlib_ui_defs_set_impl("TableDesactiveFiltre", fn_table_desactive_filtre);
    stdlib_ui_defs_set_impl("TableColonnesFiltrees", fn_table_colonnes_filtrees);
    stdlib_ui_defs_set_impl("TablePosition", fn_table_position);
    stdlib_ui_defs_set_impl("TableInfoXY", fn_table_info_xy);
    stdlib_ui_defs_set_impl("TableVersExcel", fn_table_vers_excel);
    stdlib_ui_defs_set_impl("TableVersPressePapier", fn_table_vers_pressepapier);
    stdlib_ui_defs_set_impl("ListeAjoute", fn_liste_ajoute);
    stdlib_ui_defs_set_impl("ListeInsere", fn_liste_insere);
    stdlib_ui_defs_set_impl("ListeSupprime", fn_liste_supprime);
    stdlib_ui_defs_set_impl("ListeSupprimeTout", fn_liste_supprime_tout);
    stdlib_ui_defs_set_impl("ListeModifie", fn_liste_modifie);
    stdlib_ui_defs_set_impl("ListeAffiche", fn_liste_affiche);
    stdlib_ui_defs_set_impl("ListeCherche", fn_liste_cherche);
    stdlib_ui_defs_set_impl("ListeOccurrence", fn_liste_occurrence);
    stdlib_ui_defs_set_impl("ListeTrie", fn_liste_trie);
    stdlib_ui_defs_set_impl("ListeSelect", fn_liste_select);
    stdlib_ui_defs_set_impl("ListeSelectPlus", fn_liste_select_plus);
    stdlib_ui_defs_set_impl("ListeSelectMoins", fn_liste_select_moins);
    stdlib_ui_defs_set_impl("ListeSelectOccurrence", fn_liste_select_occurrence);
    stdlib_ui_defs_set_impl("ListeDeplaceLigne", fn_liste_deplace_ligne);
    stdlib_ui_defs_set_impl("ListeInfoXY", fn_liste_info_xy);
    stdlib_ui_defs_set_impl("ComboOuvre", fn_combo_ouvre);
    stdlib_ui_defs_set_impl("ComboFerme", fn_combo_ferme);
    stdlib_ui_defs_set_impl("SelecteurAjoute", fn_selecteur_ajoute);
    stdlib_ui_defs_set_impl("SelecteurInsere", fn_selecteur_insere);
    stdlib_ui_defs_set_impl("SelecteurSupprime", fn_selecteur_supprime);
    stdlib_ui_defs_set_impl("SelecteurOccurrence", fn_selecteur_occurrence);
    stdlib_ui_defs_set_impl("InterrupteurAjoute", fn_interrupteur_ajoute);
    stdlib_ui_defs_set_impl("InterrupteurInsere", fn_interrupteur_insere);
    stdlib_ui_defs_set_impl("InterrupteurSupprime", fn_interrupteur_supprime);
    stdlib_ui_defs_set_impl("InterrupteurOccurrence", fn_interrupteur_occurrence);
    stdlib_ui_defs_set_impl("ZoneRepeteeAjouteLigne", fn_zr_ajoute_ligne);
    stdlib_ui_defs_set_impl("ZoneRepeteeInsere", fn_zr_insere);
    stdlib_ui_defs_set_impl("ZoneRepeteeSupprime", fn_zr_supprime);
    stdlib_ui_defs_set_impl("ZoneRepeteeSupprimeTout", fn_zr_supprime_tout);
    stdlib_ui_defs_set_impl("ZoneRepeteeOccurrence", fn_zr_occurrence);
    stdlib_ui_defs_set_impl("ZoneRepeteeAffiche", fn_zr_affiche);
    stdlib_ui_defs_set_impl("dCopieImageFenetre", fn_dCopieImageFenetre);
    stdlib_ui_defs_set_impl("dCopieImageChamp", fn_dCopieImageChamp);
    stdlib_ui_defs_set_impl("dCopieImageEcran", fn_dCopieImageEcran);
}

void stdlib_ui_init(void) {
    /*  — reset des tables de watch (instance->widget / widget->racine
     * style) a chaque cycle de VM. vm_free() libere tous les ObjInstance : sans
     * ce reset, leurs pointeurs survivent dans les tables et une instance du
     * fichier suivant reutilisant une adresse liberee ferait deref un widget
     * mort (crash non deterministe en suite multi-fichiers). Corrige aussi le
     * risque latent pre-existant sur g_inst_widget_watches (Cadre/Police). */
    g_inst_widget_watch_count = 0;
    g_style_root_watch_count = 0;
    /* DEFS + enums/consts UI : enregistres par stdlib_ui_defs_register()
     * (stdlib_ui_defs.c, Qt-free, inconditionnel). stdlib_ui_impls_wire() a
     * deja injecte les .impl Qt dans DEFS AVANT cet enregistrement (owl_stdlib.c,
     * ordre wire-before-register -> registre GUI byte-identique). */
    /* Phase E.X : enregistre les dispatchers 'ctrl..Prop' (override les
     * defaults fournis par ui_decl.c en mode CLI). */
    owl_ui_register_get_prop(real_ui_get_prop);
    owl_ui_register_set_prop(real_ui_set_prop);
    /* V1.5-D : watcher Cadre - re-apply Qt6 sur chained set. */
    owl_ui_register_on_instance_modified(on_instance_modified_impl);
    /*  lot 2 : Qt observe le store d'instance (imperatif -> GUI). */
    owl_ui_register_apply_instance_scalar(apply_instance_scalar_impl);
    owl_ui_register_register_composite(register_composite_impl);
    /*  : logique metier Onglet/Volet apres set (event surChangementVolet
     * sur changement reel de ..Valeur ; report du volet actif au masquage). */
    owl_ui_register_control_logic_after_set(control_logic_after_set_impl);
    /*  lot C : Qt observe aussi le store d'instance fenetre. */
    owl_ui_register_apply_window_scalar(apply_window_scalar_impl);
    /*  : et la lecture de sa geometrie vient du rendu, pas du store. */
    owl_ui_register_window_live_get(window_live_get_impl);
    /*  : hot-switch langue — re-render des props chaineMultilingue des
     * controles/fenetres vivants au changement de Langue(). */
    owl_i18n_register_on_language_changed(i18n_language_changed_impl);
    /*  : backend QTimer (override les no-op CLI de stdlib_timer.c).
     * update = propagation ..Intervalle/..Répète à chaud sur timer actif. */
    owl_timer_register_backend(timer_start_impl, timer_stop_impl, timer_update_impl);
    /* S.4 : indexation widget[i] (Selecteur). */
    owl_ui_register_get_subitem(selecteur_get_subitem);
    /*  : indexation ZR[i] sur instance (ObjUiControlRef resolu) —
     * proxy de rep GUI (CONTAINER) ou headless (ObjInstance store). */
    owl_ui_register_instance_get_subitem(widget_instance_get_subitem);
    /*  : coercion de cellule ZR au SET (Saisie -> chaine). */
    owl_ui_register_zr_rep_coerce(zr_rep_coerce_cell_impl);
    /*  : mise en forme de cellule / ligne de Table poussee au widget. */
    owl_ui_register_table_fmt_applied(table_fmt_applied_impl);
    /*  : masque de saisie d'une colonne poussé au widget (createEditor). */
    owl_ui_register_table_colmask_applied(table_colmask_applied_impl);
    /*  : materialisation lazy d'une cellule ZR en ObjUiControl
     * (store de props par cellule) pour ZR[i].champ..Prop headless. */
    owl_ui_register_zr_rep_cell_subcontrol(zr_rep_cell_subcontrol_impl);
    /*  /  : count items widgets-a-items (widget Qt + headless
     * store). Couvre Liste/Combo/Selecteur/Interrupteur/ZoneRepetee. */
    owl_ui_register_widget_items_count(widget_items_count_qt);
    owl_ui_register_headless_widget_items_count_uic(headless_widget_items_count_uic_impl);
    owl_ui_register_headless_widget_items_count_uici(headless_widget_items_count_uici_impl);

    /*  : enregistre g_return_value (Value) au registre unique des
     * racines persistantes. */
    owl_register_persistent_value_root(&g_return_value);

    /*  Lot 2e-2 — enregistre le rasterizer Qt6Svg cote
     * stdlib_icones (vit dans owl_core, sans Qt link). IconeVersImage et
     * polymorphisme BTN..Image = ICN consomment le hook. */
    owl_icone_register_rasterizer(owl_icone_rasterize_rgba);
}

/* ====================================================================
 *   — bridges du menu contextuel de table (corps)
 * ==================================================================== */

/* NB : owl_ui_table_export_xlsx (logique data->.xlsx) vit dans stdlib_xls.c —
 * elle est pure-données (pas de Qt) et doit etre linkable par owl-cli (owl_core),
 * ou tourne le self-test round-trip. Ici on ne garde que le pont icone, propre
 * au rendu Qt (appele uniquement par ui_qt.cpp). */

/* Rasterise une icone du catalogue Iconify embarqué vers RGBA (caller free).
 * Chemin silencieux : owl_icones_catalog_find n'émet pas d'erreur native sur un
 * miss (contrairement au namespace `icones.<nom>`), ce qui convient au rendu
 * d'un menu — un nom inconnu ne doit pas polluer l'état d'erreur de la VM. */
int owl_ui_icone_catalog_rgba(const char* name, int size, unsigned int color_bgr,
                              unsigned char** out_rgba, int* out_w, int* out_h) {
    if (name == NULL || out_rgba == NULL) return 0;
    const OwlIconCatalogEntry* e = owl_icones_catalog_find(name);
    if (e == NULL) return 0;
    Value icn = owl_icones_materialize_entry((const void*)e);
    /* Teinte demandée : appliquée UNIQUEMENT si l'icone est monochrome
     * (couleur_modifiable -> want_color côté rasterizer). Les icones aux
     * couleurs bakées gardent leurs couleurs natives (couleur_modifiable=faux
     * -> color ignoré). Le détecteur monochrome est celui figé au catalogue,
     * pas une heuristique locale. */
    if (IS_ICONE(icn)) {
        AS_ICONE(icn)->couleur = (int64_t)(color_bgr & 0xFFFFFF);
    }
    return owl_icone_rasterize_value(icn, size, out_rgba, out_w, out_h) ? 1 : 0;
}

/* ====================================================================
 *  Lot V — Live preview
 * ==================================================================== */

#include "compiler.h"
#include "project.h"

/* Etat global du preview en cours. */
static OwlWindow*       g_preview_window = NULL;
static char             g_preview_filepath[1024] = "";
static int              g_preview_x = -1;
static int              g_preview_y = -1;
static bool             g_preview_geometry_saved = false;

/* Compile le source et build la 1ere fenetre. Retourne le widget Qt
 * cree, ou NULL en cas d'echec. */
static OwlWindow* preview_compile_and_build(const char* source) {
    /* Reset etat compile + windows precedentes. */
    vm_windows_clear();
    reset_bindings();

    /* Silence des erreurs compile pour ne pas polluer stdout. */
    CompileDiagnostic dummyErr[64];
    int dummyCount = 0;
    compile_set_error_capture(dummyErr, 64, &dummyCount);

    (void)compile(source);

    compile_set_error_capture(NULL, 0, NULL);

    if (vm.windowCount == 0) {
        fprintf(stderr, "[preview] Aucune fenetre declaree dans le source.\n");
        return NULL;
    }
    OwlWindowDecl* decl = vm.windows[0];

    OwlWindow* w = build_window(decl);
    if (!w) {
        fprintf(stderr, "[preview] build_window a echoue pour '%s'.\n",
                decl->name);
        return NULL;
    }

    /* Restore geometry de la window precedente (preserve pos / size
     * meme en cas de reload). */
    if (g_preview_geometry_saved) {
        owl_ui_window_set_position(w, g_preview_x, g_preview_y);
    }
    return w;
}

/* Callback du file watcher : reload la fenetre. */
static void on_preview_file_changed(const char* path, void* userdata) {
    (void)userdata;
    fprintf(stderr, "[preview] %s changed, reloading...\n", path);

    /* Save current geometry. */
    if (g_preview_window) {
        g_preview_x = owl_ui_window_get_x(g_preview_window);
        g_preview_y = owl_ui_window_get_y(g_preview_window);
        g_preview_geometry_saved = true;
        owl_ui_window_destroy(g_preview_window);
        g_preview_window = NULL;
    }

    char* source = read_file(path);
    if (!source) {
        fprintf(stderr, "[preview] cannot read %s\n", path);
        return;
    }

    g_preview_window = preview_compile_and_build(source);
    free(source);
    if (g_preview_window) {
        owl_ui_window_show(g_preview_window);
    }
}

int stdlib_ui_preview_run(const char* filepath) {
    if (!filepath) return 1;

    snprintf(g_preview_filepath, sizeof(g_preview_filepath), "%s", filepath);
    g_preview_geometry_saved = false;

    char* source = read_file(filepath);
    if (!source) {
        fprintf(stderr, "[preview] cannot read %s\n", filepath);
        return 65;
    }

    ensure_ui_init();
    g_preview_window = preview_compile_and_build(source);
    free(source);
    if (!g_preview_window) return 1;

    owl_ui_window_show(g_preview_window);
    owl_ui_install_file_watcher(filepath, on_preview_file_changed, NULL);

    fprintf(stderr, "[preview] watching %s — Ctrl+C ou X pour quitter.\n",
            filepath);

    return owl_ui_run();
}

/* ============================================================
 *  Planchers irreductibles par famille de champ
 *
 *  Une fenetre declaree a 560 s'ouvre a 582 : un champ impose une taille
 *  minimale et le layout agrandit la fenetre pour la respecter. L'editeur doit
 *  pouvoir dessiner cette taille-la. Il sait composer (somme, max, espacement)
 *  a partir de la police et du nombre d'elements ; ce qu'il ne peut pas
 *  deviner, ce sont les planchers CONSTANTS du moteur. Ils sortent donc d'ici,
 *  a chaque build, mesures.
 *
 *  On cree un exemplaire neuf par la MEME fonction que le montage reel, dans
 *  une fenetre jetable hors ecran, et on lit son plancher. Le contenu ajoute
 *  est le strict minimum que le langage exige (une colonne pour la Table, un
 *  volet pour l'Onglet) : en dessous, la declaration serait refusee a la
 *  compilation et le plancher publie ne correspondrait a rien d'atteignable.
 *
 *  Releves du jour (police par defaut, champ vide), a titre indicatif :
 *    Libelle 8x15   Bouton 50x23   Saisie 23x23   Combo 79x23  Jauge 91x23
 *    Table / Liste / ZoneRepetee 70x70 (constant : ni le contenu, ni les
 *    colonnes, ni la police ne le bougent)
 *    Selecteur / Interrupteur 8x8 (le pourtour ; chaque ligne d'option
 *    s'ajoute par-dessus, separee de l'espacement rendu ci-dessous)
 *    Onglet 127x31 (largeur constante ; la hauteur du chrome suit la police
 *    des languettes, et le plancher reel y ajoute le plus exigeant des volets,
 *    masques compris — c'est la composition, elle reste a l'editeur)
 *    Conteneur / Image / CodeBarres : aucun plancher propre.
 * ============================================================ */
/* Police de reference du plancher, cote MODELE : la taille que `..Police..Taille`
 * rend a l'editeur pour un champ par defaut, donc celle a laquelle il comparera
 * la police reelle d'un champ pour deriver de combien monter le plancher. Lue
 * sur le descripteur Police canon (source unique), PAS sur le widget Qt : ce
 * dernier porte la police applicative rendue (un point de moins), et publier
 * deux references differentes desynchroniserait l'extrapolation de l'editeur.
 *
 * A appeler TOT (juste apres vm_init) : le template Police vit dans vm.types,
 * qui n'est pas racine du GC — une passe de collecte le libere, et le clone rend
 * alors NIL. Rend -1 si indisponible. */
int owl_ui_default_police_taille(void) {
    Value polV = owl_clone_global_template("Police");
    if (!IS_INSTANCE(polV)) return -1;
    return instance_get_int(AS_INSTANCE(polV), "Taille", -1);
}

int owl_ui_min_floor_for_kind(int kind, int* out_w, int* out_h,
                              int* out_option_spacing,
                              int* out_content_agnostic) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_option_spacing) *out_option_spacing = -1;
    if (out_content_agnostic) *out_content_agnostic = 0;
    if (owl_ui_init_offscreen() != 0) return 0;

    OwlWindow* host = owl_ui_window_create("");
    if (!host) return 0;

    OwlControl* c = NULL;
    const OwlControlKind ckind = (OwlControlKind)kind;
    switch (ckind) {
        case OWL_CONTROL_LABEL:  c = owl_ui_label_create(host, "", 0, 0, 0, 0);    break;
        case OWL_CONTROL_BUTTON: c = owl_ui_button_create(host, "", 0, 0, 0, 0);   break;
        case OWL_CONTROL_INPUT:  c = owl_ui_lineedit_create(host, "", 0, 0, 0, 0); break;
        case OWL_CONTROL_TABLE:
            c = owl_ui_table_create(host, 0, 0, 0, 0);
            if (c) owl_ui_table_add_column(c, "", 0, -1);
            break;
        case OWL_CONTROL_CONTAINER:    c = owl_ui_container_create(host, 0, 0, 0, 0);    break;
        /* Le Champ CodeBarres est un calque strict du Champ Image : meme
         * backend, donc meme plancher. Cf. build_control. */
        case OWL_CONTROL_IMAGE:
        case OWL_CONTROL_CODEBARRE:    c = owl_ui_image_field_create(host, 0, 0, 0, 0);  break;
        case OWL_CONTROL_LISTE:        c = owl_ui_list_create(host, 0, 0, 0, 0);         break;
        case OWL_CONTROL_COMBO:        c = owl_ui_combo_create(host, 0, 0, 0, 0);        break;
        case OWL_CONTROL_SELECTEUR:    c = owl_ui_selecteur_create(host, 0, 0, 0, 0);    break;
        case OWL_CONTROL_INTERRUPTEUR: c = owl_ui_interrupteur_create(host, 0, 0, 0, 0); break;
        case OWL_CONTROL_ZONE_REPETEE: c = owl_ui_zr_create(host, 0, 0, 0, 0);           break;
        case OWL_CONTROL_JAUGE:        c = owl_ui_jauge_create(host, 0, 0, 0, 0);        break;
        case OWL_CONTROL_ONGLET:
            c = owl_ui_onglet_create(host, 0, 0, 0, 0);
            if (c) owl_ui_onglet_add_volet(c, owl_ui_onglet_volet_create(), "");
            break;
        default: break;
    }

    int ok = 0;
    if (c) {
        int mw = 0, mh = 0;
        owl_ui_control_min_floor(c, &mw, &mh);
        if (out_w) *out_w = mw;
        if (out_h) *out_h = mh;

        int spacing = -1;
        /* Restreint aux deux familles qui EMPILENT des lignes d'option. Les
         * autres ont bien un layout interne, mais son espacement ne dit rien
         * de leur taille minimale : le publier inviterait l'editeur a composer
         * une hauteur qui n'existe pas. */
        if (ckind == OWL_CONTROL_SELECTEUR || ckind == OWL_CONTROL_INTERRUPTEUR) {
            spacing = owl_ui_control_layout_spacing(c);
        }
        if (out_option_spacing) *out_option_spacing = spacing;

        /* CONTENT-AGNOSTIC, mesure et non liste : une famille dont le plancher
         * ne suit NI sa police NI son espacement d'options le tient constant
         * quoi qu'on y mette (Table, Liste, ZoneRepetee : 70x70). L'editeur
         * dessine alors ce plancher tel quel au lieu de compter des lignes.
         *
         *   - un plancher nul (Conteneur, Image, CodeBarres) n'a rien a tenir ;
         *   - un espacement d'options publie signale une famille qui empile des
         *     lignes : sa hauteur suit son contenu par construction ;
         *   - sinon on pose un texte temoin et on GONFLE la police : si la
         *     hauteur bouge, le plancher suit sa police (Libelle, Bouton,
         *     Onglet, ...) ; si elle tient, il y est insensible.
         *
         * Le texte temoin est indispensable : un champ Libelle VIDE a un
         * plancher insensible a la police (rien a dessiner), et serait classe
         * insensible a tort. On mesure donc a police par defaut PUIS gonflee, le
         * meme texte pose dans les deux cas — sur les familles sans notion de
         * texte (Table, Liste, ZoneRepetee) le poser est sans effet. Le
         * gonflement passe la barre des 18 points ou le chrome d'un Onglet
         * commence a grimper. */
        int agnostic = 0;
        if (mh > 0 && spacing < 0) {
            const int base_pt = owl_ui_control_font_point_size(c);
            if (base_pt > 0) {
                owl_ui_control_set_text(c, "Ag");
                int h0 = 0, h1 = 0;
                owl_ui_control_min_floor_at_font(c, base_pt,      NULL, &h0);
                owl_ui_control_min_floor_at_font(c, base_pt + 24, NULL, &h1);
                agnostic = (h0 == h1) ? 1 : 0;
            }
        }
        if (out_content_agnostic) *out_content_agnostic = agnostic;
        ok = 1;
    }
    owl_ui_window_destroy(host);
    return ok;
}

/*  : stdlib_ui_mark_roots() supprime — g_return_value est
 * enregistre au registre unique dans stdlib_ui_init(). */

void stdlib_ui_reset_globals(void) {
    g_return_value = NIL_VAL;
}
