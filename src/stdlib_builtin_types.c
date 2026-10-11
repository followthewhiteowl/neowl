// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL built-in structure types.
 *
 * Pre-registers templates for structure variables (e.g. httpRequete,
 * httpReponse) and their companion integer constants (httpGet=1, httpPost=2, ...).
 *
 * Templates are ObjInstance objects stored in vm.globals. When the user writes
 *   req est un httpRequete
 * the compiler emits __StructClone(<template>) which produces a fresh instance
 * with a deep copy of mutable fields (notably the "Entete" associative array).
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "sentinels.h"
#include "vm.h"
#include <string.h>

/* Set a field on an instance's field table. Copies the key string.
 *
 * GC safety : `inst` (et `child`/`val` quand applicables) est pushé sur la
 * VM stack pendant les allocations internes (obj_copy_string, table grow).
 * Sans ça, un GC déclenché ici collecterait l'instance en construction —
 * elle n'est pas encore enregistrée comme root. */
/*  : les helpers triviaux set_field_string/int/bool sont supprimes
 * (call-sites migres vers inst_field_set_str/int/bool, canon dans object.c).
 *
 * Les helpers specialises ci-dessous (assoc_chaines, buffer, buffer_zeros,
 * instance, array_empty) sont conserves pour leur lisibilite au call-site
 * (le builder construit un sub-objet typed) — ils sont reecrits pour
 * deleguer au canon inst_field_set_value, donc la discipline GC roots
 * reste centralisee dans object.c.
 *
 * register_global_instance idem : delegue a obj_globals_set (canon). */

static void set_field_assoc_chaines(ObjInstance* inst, const char* name) {
    /*  : push inst AVANT obj_new_assoc (alloc peut grow → GC). */
    vm_push(OBJ_VAL(inst));
    ObjAssocArray* a = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_CHAINE);
    vm_push(OBJ_VAL(a));
    inst_field_set_value(inst, name, OBJ_VAL(a));
    vm_pop();
    vm_pop();
}

static void set_field_buffer(ObjInstance* inst, const char* name) {
    vm_push(OBJ_VAL(inst));
    ObjBuffer* b = obj_buffer_empty(0);
    vm_push(OBJ_VAL(b));
    inst_field_set_value(inst, name, OBJ_VAL(b));
    vm_pop();
    vm_pop();
}

/* Variante de set_field_buffer qui alloue un buffer de nbytes octets remplis
 * de zéros. Utile pour les templates dont le storage interne doit avoir
 * une taille précise dès la construction (UUID 16 octets) — sinon la
 * coercion vers chaîne canonique ne sait pas distinguer un UUID nul
 * d'un buffer vide. */
static void set_field_buffer_zeros(ObjInstance* inst, const char* name, int nbytes) {
    vm_push(OBJ_VAL(inst));
    unsigned char zeros[64] = {0};  /* defensif : large headroom pour ré-emplois futurs */
    ObjBuffer* b = obj_new_buffer(zeros, nbytes);
    vm_push(OBJ_VAL(b));
    inst_field_set_value(inst, name, OBJ_VAL(b));
    vm_pop();
    vm_pop();
}

/* Imbrique une ObjInstance comme sous-champ. Permet aux types composes
 * d'agreger d'autres types (ex: Style.Police → ObjInstance Police, etc.). */
static void set_field_instance(ObjInstance* inst, const char* name, ObjInstance* child) {
    /*  : push inst aussi (defense en profondeur, meme si caller
     * peut deja l'avoir push — coute negligeable, securite > paranoia). */
    vm_push(OBJ_VAL(inst));
    vm_push(OBJ_VAL(child));
    inst_field_set_value(inst, name, OBJ_VAL(child));
    vm_pop();
    vm_pop();
}

/*  — les templates de types vivent maintenant dans vm.types, plus
 * dans vm.globals. Le local helper conserve son nom pour minimiser le diff
 * aux ~30 call-sites (Chrono, httpRequête, Cadre, Police, etc.) ; seule la
 * cible change. */
static void register_global_instance(const char* name, ObjInstance* inst) {
    vm_type_register(name, OBJ_VAL(inst));
}

/* owl_register_global_string vit dans owl_core (owl_registry.c) car il est
 * utilise par stdlib_http.c / stdlib_i18n.c — ce fichier-ci est compile
 * par target (OWL_HAS_SQL conditionnel) et ne peut donc pas porter un
 * symbole dont le core depend. Declaration : owl_stdlib.h. */

/* Local alias so existing call-sites in this file keep their short name. */
#define register_global_string owl_register_global_string

static ObjInstance* build_pdfParametre_template(void) {
    ObjString* name = obj_copy_string("pdfParametre", 12);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    /* Defaults canon. */
    inst_field_set_str(tpl, "MotDePasseModification", "");
    inst_field_set_str(tpl, "MotDePasseOuverture",    "");
    inst_field_set_int   (tpl, "Protection",             0);  /* iProtectionAucune */
    inst_field_set_bool  (tpl, "Signet",                 false);
    inst_field_set_bool  (tpl, "Unicode",                true);
    return tpl;
}

/* Police — descripteur de police canon.
 * V1 : 7 sous-proprietes essentielles (Nom, Taille, Gras, Italique,
 * Souligne, Barre, Couleur). V2 ajoutera Angle, Alphabet, Interligne,
 * Interlettre, UnitéTaille (rare en pratique). */
static ObjInstance* build_Police_template_taille(int tailleDefaut) {
    ObjString* name = obj_copy_string("Police", 6);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    /* Defaults canon. */
    inst_field_set_str(tpl, "Nom",       "");      /* "Arial" ou iPolice* */
    inst_field_set_int   (tpl, "Taille",    tailleDefaut);  /* points typographiques */
    inst_field_set_bool  (tpl, "Gras",      false);
    inst_field_set_bool  (tpl, "Italique",  false);
    /* Storage en ASCII canon (Souligne, Barre) — le compilateur scanner
     * strip les accents au compile time, donc l'utilisateur peut ecrire
     * `p.Souligné` ou `p.Souligne` indifferemment, le bytecode pointera
     * vers la key ASCII. Le commentaire dans FIELDS_Police[] signale la
     * forme accentuee canon. (Avant B.12 le storage etait accentue,
     * ce qui empechait l'acces ASCII de fonctionner — bug fixe.) */
    inst_field_set_bool  (tpl, "Souligne", false);
    inst_field_set_bool  (tpl, "Barre",    false);
    inst_field_set_int   (tpl, "Couleur",   0);       /* RVB packe BGR (canon) */
    return tpl;
}

/* Police autonome (`p est une Police`) : un DESCRIPTEUR. Il decrit une police
 * complete, donc il porte une taille reelle. */
static ObjInstance* build_Police_template(void) {
    return build_Police_template_taille(11);
}

/*  — Police imbriquee dans une brique de style : une SURCHARGE, pas un
 * descripteur. Elle ne decrit que ce que l'utilisateur a explicitement pose ;
 * tout le reste doit rester "non renseigne" pour que le widget garde la police
 * de son theme. Meme distinction que la couleur de fond d'une brique (-1) ou
 * son nom de police (chaine vide) : tous les voisins de Taille portent deja une
 * sentinelle hors-domaine, Taille etait le seul slot seme avec une valeur
 * LEGALE (11) — donc indistinguable d'un choix utilisateur. D'ou 0, hors du
 * domaine des tailles valides.
 *
 * Ne PAS "reparer" l'asymetrie avec le descripteur autonome : elle est le
 * fond du sujet. Semer 11 ici faisait emettre une taille de police par TOUTE
 * instance de style, meme celles qui ne parlent que de couleur, ce qui
 * retaillait la geometrie de tous les champs texte stylés. */
static ObjInstance* build_Police_surcharge_template(void) {
    return build_Police_template_taille(0);
}

/* Style — descripteur d'apparence agrege (D.2 : enabler du Lot D).
 * Inspire du concept canon "feuille de styles" mais en type compose first-class.
 *
 * V1 : 6 props plates + 2 sous-objets composes (Police + Cadre).
 *   Couleur        — couleur du texte
 *   CouleurFond    — couleur de fond (-1 = pas de fond)
 *   RetraitGauche/Droit/Haut/Bas — marges interieures en pixels
 *   Police         — ObjInstance Police imbriquee (Nom/Taille/Gras/...)
 *   Cadre          — ObjInstance Cadre imbriquee (Couleur/Epaisseur/...)
 *
 * V2+ ajoutera les overrides d'etat : Survol, Presse, Desactive,
 * (chacun ObjInstance partielle qui surcharge ponctuellement).
 *
 * Application : LIB_X..Style = STY_Y au runtime resout les sous-champs
 * et applique chaque setter correspondant sur le widget. Cf D.2.b. */
static ObjInstance* build_Style_template(void);  /* forward */

/* Coin* sous-objet : {Largeur, Hauteur} canon pour les coins arrondis
 * asymetriques (CoinHautGauche/Droite/BasGauche/Droite). */
static ObjInstance* build_Coin_template(void) {
    ObjString* name = obj_copy_string("Coin", 4);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_int(tpl, "Largeur", 0);
    inst_field_set_int(tpl, "Hauteur", 0);
    return tpl;
}

/* Trait* sous-objet : {Couleur, Epaisseur, Type} canon pour les
 * traits par bord (TraitHaut/Bas/Gauche/Droite). */
static ObjInstance* build_Trait_template(void) {
    ObjString* name = obj_copy_string("Trait", 5);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_int(tpl, "Couleur",   0);   /* noir */
    inst_field_set_int(tpl, "Epaisseur", 0);   /* 0 = bord invisible */
    inst_field_set_int(tpl, "Type",      OWL_TC_CONTINU);
    return tpl;
}

/* Cadre — descripteur de cadre canon.
 * Sous-proprietes :
 *   - Couleur, Epaisseur, Trait, Arrondi, Bords  (5 champs scalaires)
 *   - 4 Coin* (CoinHautGauche/HautDroite/BasGauche/BasDroite)
 *   - 4 Trait* (TraitHaut/Bas/Gauche/Droite)
 * Defaults canon : invisible (Epaisseur=0), noir, continu, tous bords,
 * coins droits. Les Trait* heritent des valeurs scalaires si non set. */
static ObjInstance* build_Cadre_template(void) {
    ObjString* name = obj_copy_string("Cadre", 5);
    vm_push(OBJ_VAL(name));                /*  : root name pendant alloc tpl */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));                 /*  : root tpl pendant TOUS les sub-builds */
    /* 5 props scalaires. */
    inst_field_set_int(tpl, "Couleur",    0);
    inst_field_set_int(tpl, "Epaisseur",  0);
    inst_field_set_int(tpl, "Trait",      OWL_TC_CONTINU);
    inst_field_set_int(tpl, "Arrondi",    0);
    inst_field_set_int(tpl, "Bords",     15);
    /* 4 Coin* — chacun avec son propre {Largeur, Hauteur}.
     * Chaque sub-template est rooté entre build et set_field_instance. */
    #define INSTALL_SUB(fname, builder) do { \
        ObjInstance* _sub = (builder)();    \
        vm_push(OBJ_VAL(_sub));             \
        set_field_instance(tpl, (fname), _sub); \
        vm_pop(); \
    } while (0)
    INSTALL_SUB("CoinHautGauche", build_Coin_template);
    INSTALL_SUB("CoinHautDroite", build_Coin_template);
    INSTALL_SUB("CoinBasGauche",  build_Coin_template);
    INSTALL_SUB("CoinBasDroite",  build_Coin_template);
    /* 4 Trait* — chacun avec son propre {Couleur, Epaisseur, Type}. */
    INSTALL_SUB("TraitHaut",   build_Trait_template);
    INSTALL_SUB("TraitBas",    build_Trait_template);
    INSTALL_SUB("TraitGauche", build_Trait_template);
    INSTALL_SUB("TraitDroite", build_Trait_template);
    #undef INSTALL_SUB
    vm_pop();  /* tpl */
    return tpl;
}

/* CadreOnglet — cadre restreint du CadreExtérieur d'un Champ Onglet.
 * Le pane d'un onglet ne pilote que la bordure simple : Couleur/Epaisseur/Arrondi.
 * PAS de Trait/Bords/Coins (le parser onglet les rejette deja) — d'ou un type
 * dedie, pour que explain/vscode-data/LSP n'exposent QUE le declarable. */
static ObjInstance* build_CadreOnglet_template(void) {
    ObjString* name = obj_copy_string("CadreOnglet", 11);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int(tpl, "Couleur",   0);
    inst_field_set_int(tpl, "Epaisseur", 0);
    inst_field_set_int(tpl, "Arrondi",   0);
    vm_pop();
    return tpl;
}

static ObjInstance* build_FormatNombre_template(void);  /*  */
static ObjInstance* build_FormatMonetaire_template(void);  /*  */

/* Projet — catalogue figé de métadonnées. Trois champs chaîne défaut "".
 * L'instance globale MonApp est un clone de ce template ; ses champs sont
 * ensuite écrits par le bloc PROPRIETES de projet.owl. */
static ObjInstance* build_Projet_template(void) {
    ObjString* name = obj_copy_string("Projet", 6);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Description",      "");
    inst_field_set_str(tpl, "Auteur",           "");
    inst_field_set_str(tpl, "Licence",          "");
    inst_field_set_str(tpl, "Theme",            "");
    inst_field_set_str(tpl, "FenetreDemarrage", "");
    inst_field_set_str(tpl, "FormatDate",       "");
    inst_field_set_str(tpl, "FormatHeure",      "");
    inst_field_set_str(tpl, "FormatDuree",      "");
    inst_field_set_bool(tpl, "ActiveAvertissementConventionDeNommage", false);
    /*  — FormatNombre : objet par defaut (jamais nil). */
    {
        ObjInstance* fnbDef = build_FormatNombre_template();
        inst_field_set_value(tpl, "FormatNombre", OBJ_VAL(fnbDef));
    }
    /*  — FormatMonétaire : objet par defaut (jamais nil). */
    {
        ObjInstance* fmnDef = build_FormatMonetaire_template();
        inst_field_set_value(tpl, "FormatMonetaire", OBJ_VAL(fmnDef));
    }
    /* Langues : tableau vide par defaut (projet.owl sans champ Langues =>
     * langue source implicite fr-FR, cf. owl_i18n_set_project_source). */
    ObjArray* langues = obj_new_array();
    inst_field_set_value(tpl, "Langues", OBJ_VAL(langues));
    vm_pop();
    return tpl;
}

static ObjInstance* build_Style_template(void) {
    ObjString* name = obj_copy_string("Style", 5);
    vm_push(OBJ_VAL(name));   /*  : root name pendant obj_new_instance */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));    /*  : root tpl pendant TOUTES les sous-allocs */
    /* Defaults canon : noir sur fond transparent, marges 0, Police +
     * Cadre par defaut (eux-memes a leurs defauts). */
    inst_field_set_int(tpl, "Couleur",       0);    /* texte noir */
    inst_field_set_int(tpl, "CouleurFond",  -1);    /* -1 = pas de fond */
    inst_field_set_int(tpl, "RetraitGauche", 0);
    inst_field_set_int(tpl, "RetraitDroit",  0);
    inst_field_set_int(tpl, "RetraitHaut",   0);
    inst_field_set_int(tpl, "RetraitBas",    0);
    inst_field_set_int(tpl, "CurseurSouris", -1);   /* -1 = non defini */
    /* Sous-objets composes : chacun a son propre template defaut. */
    {
        ObjInstance* poli = build_Police_surcharge_template();
        vm_push(OBJ_VAL(poli));    /* root poli pendant set_field_instance */
        set_field_instance(tpl, "Police", poli);
        vm_pop();
    }
    {
        ObjInstance* cadre = build_Cadre_template();
        vm_push(OBJ_VAL(cadre));
        set_field_instance(tpl, "Cadre", cadre);
        vm_pop();
    }
    /* Survol — sous-état appliqué au passage souris (D.5 hover support).
     * Conteneur ouvert : l'user n'override que les props qu'il veut.
     * Sous-champs typiques : Couleur, CouleurFond, CurseurSouris. */
    {
        ObjString* hoverName = obj_copy_string("Style", 5);
        vm_push(OBJ_VAL(hoverName));
        ObjInstance* hover = obj_new_instance(hoverName);
        vm_pop();    /* hoverName plus utilisé ; hover détient sa ref */
        vm_push(OBJ_VAL(hover));
        set_field_instance(tpl, "Survol", hover);
        vm_pop();
    }
    vm_pop();    /* tpl */
    return tpl;
}

/*  — déclarations anticipées : styleZoneSaisie porte désormais Ombre et
 * Dégradé (calqués sur le CadreExtérieur), mais leurs builders sont définis plus
 * bas. */
static ObjInstance* build_styleOmbre_template(void);
static ObjInstance* build_styleDegrade_template(void);

/* Brique de style commune : les props partagees par tous les
 * sous-elements de style (ZoneSaisie/Libellé/CadreExtérieur et leur type de
 * surcharge plat). Installe Couleur/CouleurFond/Retraits + les composites
 * Police et Cadre. Le `tpl` DOIT etre push sur la VM stack par l'appelant
 * (allocations internes -> GC). */
static void install_style_brick(ObjInstance* tpl) {
    inst_field_set_int(tpl, "Couleur",       0);    /* texte noir */
    inst_field_set_int(tpl, "CouleurFond",  -1);    /* -1 = pas de fond */
    inst_field_set_int(tpl, "RetraitGauche", 0);
    inst_field_set_int(tpl, "RetraitDroit",  0);
    inst_field_set_int(tpl, "RetraitHaut",   0);
    inst_field_set_int(tpl, "RetraitBas",    0);
    {
        ObjInstance* poli = build_Police_surcharge_template();
        vm_push(OBJ_VAL(poli));
        set_field_instance(tpl, "Police", poli);
        vm_pop();
    }
    {
        ObjInstance* cadre = build_Cadre_template();
        vm_push(OBJ_VAL(cadre));
        set_field_instance(tpl, "Cadre", cadre);
        vm_pop();
    }
}

/* styleZoneSaisieBase — type PLAT de la surcharge par etat. C'est
 * ce que designe `..ZoneSaisie..Etats[etat]`. Props seules (la brique) : PAS de
 * membre Etats, PAS de sous-elements. La recursion `..Etats[x]..Etats[y]` est
 * donc inecrivable par construction du type (pas une garde runtime). */
static ObjInstance* build_styleZoneSaisieBase_template(void) {
    ObjString* name = obj_copy_string("styleZoneSaisieBase", 19);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_style_brick(tpl);
    vm_pop();
    return tpl;
}

/* styleZoneSaisieEtats — collection d'etats de la zone de saisie.
 * Slots internes _Survol/_Focus/_Grise/_LectureSeule, chacun un
 * styleZoneSaisieBase (surcharge plate). Indexee par l'enum d'etats via
 * `..Etats[etat]` (le crochet vit ici, jamais sur un sous-element ni la racine). */
static ObjInstance* build_styleZoneSaisieEtats_template(void) {
    ObjString* name = obj_copy_string("styleZoneSaisieEtats", 20);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    #define INSTALL_STATE(slot) do {                              \
        ObjInstance* _s = build_styleZoneSaisieBase_template();   \
        vm_push(OBJ_VAL(_s));                                     \
        set_field_instance(tpl, (slot), _s);                     \
        vm_pop();                                                 \
    } while (0)
    INSTALL_STATE("_Survol");
    INSTALL_STATE("_Focus");
    INSTALL_STATE("_Grise");
    INSTALL_STATE("_LectureSeule");
    #undef INSTALL_STATE
    vm_pop();
    return tpl;
}

/* styleZoneSaisie — sous-element zone de saisie : la brique de props
 * de base + le membre public Etats (surcharges par etat). */
static ObjInstance* build_styleZoneSaisie_template(void) {
    ObjString* name = obj_copy_string("styleZoneSaisie", 15);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_style_brick(tpl);
    /*  — Ombre + Dégradé de la zone de saisie (le QLineEdit), calqués
     * sur le CadreExtérieur. Le champ interne accepte sa propre ombre portée et
     * son dégradé de fond, distincts du conteneur. */
    {
        ObjInstance* ombre = build_styleOmbre_template();
        vm_push(OBJ_VAL(ombre));
        set_field_instance(tpl, "Ombre", ombre);
        vm_pop();
    }
    {
        ObjInstance* degrade = build_styleDegrade_template();
        vm_push(OBJ_VAL(degrade));
        set_field_instance(tpl, "Degrade", degrade);
        vm_pop();
    }
    {
        ObjInstance* et = build_styleZoneSaisieEtats_template();
        vm_push(OBJ_VAL(et));
        set_field_instance(tpl, "Etats", et);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleLibelléSaisie (, renomme ) — brique du sous-element
 * libellé INTEGRE du Saisie. Statique en V1 : la brique de props, PAS de membre
 * Etats (le libellé ne porte pas d'etats). Anciennement styleLibellé ; renomme
 * pour liberer ce nom au profit du type widget styleLibellé (Champ Libellé). */
static ObjInstance* build_styleLibelleSaisie_template(void) {
    ObjString* name = obj_copy_string("styleLibell\xc3\xa9Saisie", 19);  /* styleLibelléSaisie */
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_style_brick(tpl);
    vm_pop();
    return tpl;
}

/* styleOmbre — sous-objet ombre portée / élévation / glow d'un
 * styleCadreExtérieur (l'ombre du widget entier). Feuilles : Couleur (couleur de
 * l'ombre ou du glow), DécalageX/DécalageY (offset en px), Flou (rayon de flou ;
 * 0 = ombre désactivée), Étendue (épaississement, approximé via le flou au rendu
 * — QGraphicsDropShadowEffect n'a pas de spread natif). Défauts : ombre absente
 * (Flou=0, offsets 0, noir). */
static ObjInstance* build_styleOmbre_template(void) {
    ObjString* name = obj_copy_string("styleOmbre", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int(tpl, "Couleur",   0);    /* noir */
    inst_field_set_int(tpl, "DecalageX", 0);
    inst_field_set_int(tpl, "DecalageY", 0);
    inst_field_set_int(tpl, "Flou",      0);    /* 0 = pas d'ombre */
    inst_field_set_int(tpl, "Etendue",   0);
    vm_pop();
    return tpl;
}

/* styleDégradé — sous-objet dégradé de fond d'un styleCadreExtérieur.
 * Feuilles : Type (enum typeDégradé : aucun/linéaire/radial ; défaut dégradéAucun
 * = pas de dégradé), Angle (orientation du dégradé linéaire ; 0 = horizontal
 * gauche->droite, 90 = vertical haut->bas ; ignoré en radial), CouleurDébut et
 * CouleurFin (arrêts). Gate d'activation par Type : le dégradé ne peint que si
 * Type ∈ {linéaire, radial} ; sinon le fond uni (CouleurFond) reste. */
static ObjInstance* build_styleDegrade_template(void) {
    ObjString* name = obj_copy_string("styleD\xc3\xa9grad\xc3\xa9", 14);  /* styleDégradé */
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int(tpl, "Type",         OWL_GRAD_AUCUN);  /* défaut : pas de dégradé */
    inst_field_set_int(tpl, "Angle",        0);
    /* -1 = arrêt non défini (sentinelle, comme CouleurFond) : distingue « non posé »
     * (rendu transparent) d'un noir explicite (= Noir = 0, rendu opaque). . */
    inst_field_set_int(tpl, "CouleurDebut", -1);
    inst_field_set_int(tpl, "CouleurFin",   -1);
    vm_pop();
    return tpl;
}

/* Brique commune du CadreExtérieur : les props plates partagees par
 * styleCadreExtérieur et son type de surcharge par etat (styleCadreExterieurBase).
 * CouleurFond + Retraits + Cadre + Ombre + Dégradé, PAS de texte (ni Couleur ni
 * Police — un cadre n'a pas de texte). Miroir de FIELDS_styleCadreExterieur (hors
 * Etats). Le `tpl` DOIT etre push par l'appelant (allocations internes -> GC). */
static void install_cadre_exterieur_brick(ObjInstance* tpl) {
    inst_field_set_int(tpl, "CouleurFond",  -1);    /* -1 = pas de fond */
    inst_field_set_int(tpl, "RetraitGauche", 0);
    inst_field_set_int(tpl, "RetraitDroit",  0);
    inst_field_set_int(tpl, "RetraitHaut",   0);
    inst_field_set_int(tpl, "RetraitBas",    0);
    {
        ObjInstance* cadre = build_Cadre_template();
        vm_push(OBJ_VAL(cadre));
        set_field_instance(tpl, "Cadre", cadre);
        vm_pop();
    }
    {
        ObjInstance* ombre = build_styleOmbre_template();
        vm_push(OBJ_VAL(ombre));
        set_field_instance(tpl, "Ombre", ombre);
        vm_pop();
    }
    {
        ObjInstance* degrade = build_styleDegrade_template();
        vm_push(OBJ_VAL(degrade));
        set_field_instance(tpl, "Degrade", degrade);
        vm_pop();
    }
}

/* styleCadreExterieurBase — type PLAT de la surcharge par etat du
 * cadre exterieur. C'est ce que designe `..CadreExtérieur..Etats[etat]`. La
 * brique seule : PAS de membre Etats (fin de chaine, recursion inecrivable par
 * construction du type). Miroir de styleZoneBoutonBase mais avec la palette de
 * props du cadre (Ombre/Dégradé, pas de texte). */
static ObjInstance* build_styleCadreExterieurBase_template(void) {
    ObjString* name = obj_copy_string("styleCadreExterieurBase", 23);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_cadre_exterieur_brick(tpl);
    vm_pop();
    return tpl;
}

/* styleCadreExterieurEtats — collection d'etats du cadre exterieur.
 * Palette PROPRE au frame/bouton : survol/appuyé/focus/grisé (miroir de
 * styleZoneBoutonEtats ; PAS de lectureSeule, sans sens sur un cadre). Slots
 * internes _Survol/_Appuye/_Focus/_Grise, chacun un styleCadreExterieurBase.
 * Indexee via `..Etats[etat]` (le crochet vit ici, jamais sur la racine). */
static ObjInstance* build_styleCadreExterieurEtats_template(void) {
    ObjString* name = obj_copy_string("styleCadreExterieurEtats", 24);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    #define INSTALL_STATE(slot) do {                                  \
        ObjInstance* _s = build_styleCadreExterieurBase_template();   \
        vm_push(OBJ_VAL(_s));                                         \
        set_field_instance(tpl, (slot), _s);                         \
        vm_pop();                                                     \
    } while (0)
    INSTALL_STATE("_Survol");
    INSTALL_STATE("_Appuye");
    INSTALL_STATE("_Focus");
    INSTALL_STATE("_Grise");
    #undef INSTALL_STATE
    vm_pop();
    return tpl;
}

/* styleCadreExtérieur (, allege , ombre , dégradé
 * , etats ) — sous-element conteneur qui enveloppe le libellé
 * et la zone de saisie. Un cadre n'a pas de texte : on n'installe PAS Couleur
 * (texte) ni Police, uniquement CouleurFond + Retraits + Cadre + Ombre + Dégradé
 * (miroir de FIELDS_styleCadreExterieur) + le membre public Etats (surcharges
 * par etat, ). */
static ObjInstance* build_styleCadreExterieur_template(void) {
    ObjString* name = obj_copy_string("styleCadreExt\xc3\xa9rieur", 20);  /* styleCadreExtérieur */
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_cadre_exterieur_brick(tpl);
    {
        ObjInstance* et = build_styleCadreExterieurEtats_template();
        vm_push(OBJ_VAL(et));
        set_field_instance(tpl, "Etats", et);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleSaisie — modele de style typé du champ Saisie (, refonte).
 * Struct PUR de sous-elements structurels : ZoneSaisie (avec Etats), Libellé
 * (statique) et CadreExtérieur (le conteneur, avec Etats). Plus de props plates a
 * la racine ; le crochet [etat] vit sur ZoneSaisie..Etats et CadreExtérieur..Etats. Alias EN :
 * styleInput. */
static ObjInstance* build_styleSaisie_template(void) {
    ObjString* name = obj_copy_string("styleSaisie", 11);
    vm_push(OBJ_VAL(name));   /* root name pendant obj_new_instance */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));    /* root tpl pendant TOUTES les sous-allocs */
    {
        ObjInstance* z = build_styleZoneSaisie_template();
        vm_push(OBJ_VAL(z));
        set_field_instance(tpl, "ZoneSaisie", z);
        vm_pop();
    }
    {
        ObjInstance* l = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(l));
        set_field_instance(tpl, "Libelle", l);
        vm_pop();
    }
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    vm_pop();    /* tpl */
    return tpl;
}

/* styleZoneBoutonBase — type PLAT de la surcharge par etat du
 * bouton. Miroir de styleZoneSaisieBase : la brique seule, PAS de membre Etats
 * (fin de chaine, recursion impossible). */
static ObjInstance* build_styleZoneBoutonBase_template(void) {
    ObjString* name = obj_copy_string("styleZoneBoutonBase", 19);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_style_brick(tpl);
    vm_pop();
    return tpl;
}

/* styleZoneBoutonEtats — collection d'etats du bouton. Palette
 * PROPRE au bouton : survol/appuyé/focus/grisé (PAS lectureSeule — sans sens
 * sur un bouton ; + appuyé = etat neuf, mappe :pressed). Slots internes
 * _Survol/_Appuye/_Focus/_Grise, chacun un styleZoneBoutonBase. */
static ObjInstance* build_styleZoneBoutonEtats_template(void) {
    ObjString* name = obj_copy_string("styleZoneBoutonEtats", 20);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    #define INSTALL_STATE(slot) do {                              \
        ObjInstance* _s = build_styleZoneBoutonBase_template();   \
        vm_push(OBJ_VAL(_s));                                     \
        set_field_instance(tpl, (slot), _s);                     \
        vm_pop();                                                 \
    } while (0)
    INSTALL_STATE("_Survol");
    INSTALL_STATE("_Appuye");
    INSTALL_STATE("_Focus");
    INSTALL_STATE("_Grise");
    #undef INSTALL_STATE
    vm_pop();
    return tpl;
}

/* styleZoneBouton — sous-element bouton : la brique de props de
 * base + le membre public Etats (surcharges par etat). Miroir stateful de
 * styleZoneSaisie, mais avec la palette du bouton. */
static ObjInstance* build_styleZoneBouton_template(void) {
    ObjString* name = obj_copy_string("styleZoneBouton", 15);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_style_brick(tpl);
    {
        ObjInstance* et = build_styleZoneBoutonEtats_template();
        vm_push(OBJ_VAL(et));
        set_field_instance(tpl, "Etats", et);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleImageBouton ( lot B, recadre , ) — sous-element
 * du styleBouton decrivant l'icone du bouton. Deux feuilles :
 *   - Image : la SOURCE de l'icone (VARIANT, coercion Image|chaine|Icone). Unifiee
 *     avec la prop plate BTN..Image : une seule verite dans le store,
 *     dernier-ecrit-gagne (pattern ). Defaut chaine vide = pas d'icone.
 *   - Position : entier contraint a E_PositionImageBouton (placement icone/texte).
 * Etats : surcharges d'icône par etat (survol/grisé uniquement),
 * cablees au rendu via QIcon::Active/Disabled. */

/* styleImageBoutonBase — feuille plate de la surcharge d'icone
 * par etat : une seule feuille Image (VARIANT), pas de membre Etats. */
static ObjInstance* build_styleImageBoutonBase_template(void) {
    ObjString* name = obj_copy_string("styleImageBoutonBase", 20);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Image", "");
    vm_pop();
    return tpl;
}

/* styleImageBoutonEtats — collection des surcharges d'icone par
 * etat. Palette RESTREINTE au regard du bouton : survol/grisé seulement
 * (echange d'icone gratuit via QIcon::Active/Disabled). Slots _Survol/_Grise,
 * chacun un styleImageBoutonBase. */
static ObjInstance* build_styleImageBoutonEtats_template(void) {
    ObjString* name = obj_copy_string("styleImageBoutonEtats", 21);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    #define INSTALL_IMG_STATE(slot) do {                          \
        ObjInstance* _s = build_styleImageBoutonBase_template();  \
        vm_push(OBJ_VAL(_s));                                     \
        set_field_instance(tpl, (slot), _s);                     \
        vm_pop();                                                 \
    } while (0)
    INSTALL_IMG_STATE("_Survol");
    INSTALL_IMG_STATE("_Grise");
    #undef INSTALL_IMG_STATE
    vm_pop();
    return tpl;
}

static ObjInstance* build_styleImageBouton_template(void) {
    ObjString* name = obj_copy_string("styleImageBouton", 16);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Image", "");
    inst_field_set_int(tpl, "Position", OWL_POSBTNIMG_GAUCHE);
    {
        ObjInstance* et = build_styleImageBoutonEtats_template();
        vm_push(OBJ_VAL(et));
        set_field_instance(tpl, "Etats", et);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleBouton (, ) — modele de style typé du Champ Bouton
 * (3e widget stylé). Struct de sous-elements : Libellé (le texte, brique
 * interactive stateful, type styleZoneBouton), Image (placement de l'icone :
 * Position, type styleImageBouton) et CadreExtérieur (le conteneur, type
 * styleCadreExtérieur). Alias EN : styleButton. */
static ObjInstance* build_styleBouton_template(void) {
    ObjString* name = obj_copy_string("styleBouton", 11);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* b = build_styleZoneBouton_template();
        vm_push(OBJ_VAL(b));
        set_field_instance(tpl, "Libelle", b);
        vm_pop();
    }
    {
        ObjInstance* im = build_styleImageBouton_template();
        vm_push(OBJ_VAL(im));
        set_field_instance(tpl, "Image", im);
        vm_pop();
    }
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleLibellé — modele de style typé du Champ Libellé (2e widget
 * stylé apres le Saisie). Struct de sous-elements : Libellé (la brique texte,
 * type styleLibelléSaisie) et CadreExtérieur (le conteneur, type
 * styleCadreExtérieur). Statique en V1 (aucun etat). Alias EN : styleLabel. */
static ObjInstance* build_styleLibelle_template(void) {
    ObjString* name = obj_copy_string("styleLibell\xc3\xa9", 13);  /* styleLibellé */
    vm_push(OBJ_VAL(name));   /* root name pendant obj_new_instance */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));    /* root tpl pendant les sous-allocs */
    {
        ObjInstance* l = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(l));
        set_field_instance(tpl, "Libelle", l);
        vm_pop();
    }
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    vm_pop();    /* tpl */
    return tpl;
}

/* styleImage — modele de style typé du Champ Image. Struct d'un
 * seul sous-element : CadreExtérieur (la boite, type styleCadreExtérieur). La
 * source de l'image (prop top-level Image) et le mode d'echelle (Cadrage) ne
 * font PAS partie du Style. Statique (aucun etat). */
static ObjInstance* build_styleImage_template(void) {
    ObjString* name = obj_copy_string("styleImage", 10);
    vm_push(OBJ_VAL(name));   /* root name pendant obj_new_instance */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));    /* root tpl pendant les sous-allocs */
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    vm_pop();    /* tpl */
    return tpl;
}

/* styleCodeBarre — modele de style typé du Champ CodeBarres. Meme
 * structure que styleImage (un CadreExtérieur), mais type DEDIE : le style
 * Image et celui du code-barres divergeront (le premier gagnera un libellé). */
static ObjInstance* build_styleCodeBarre_template(void) {
    ObjString* name = obj_copy_string("styleCodeBarre", 14);
    vm_push(OBJ_VAL(name));   /* root name pendant obj_new_instance */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));    /* root tpl pendant les sous-allocs */
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    vm_pop();    /* tpl */
    return tpl;
}

/* styleConteneur — modele de style typé du Champ Conteneur. Un
 * seul sous-element : CadreExtérieur (la boite : CouleurFond + Cadre +
 * Retraits, PAS de texte). Le conteneur est un groupeur passif : aucune police,
 * aucun etat. Statique. Alias EN : styleContainer. */
static ObjInstance* build_styleConteneur_template(void) {
    ObjString* name = obj_copy_string("styleConteneur", 14);
    vm_push(OBJ_VAL(name));   /* root name pendant obj_new_instance */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));    /* root tpl pendant les sous-allocs */
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    vm_pop();    /* tpl */
    return tpl;
}

/* styleCoche — sous-element marque de coche du Champ Interrupteur.
 * Feuille unique Couleur : la couleur de la marque cochee. Qt ne stylise pas
 * directement le glyphe de coche (image thémée) ; le rendu approche colore le
 * fond de l'indicator coché (::indicator:checked background). Statique. */
static ObjInstance* build_styleCoche_template(void) {
    ObjString* name = obj_copy_string("styleCoche", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int(tpl, "Couleur", -1);   /* -1 = non posee */
    vm_pop();
    return tpl;
}

/* styleInterrupteur — modele de style typé du Champ Interrupteur
 * (cases a cocher). Struct de sous-elements : CadreExtérieur (le conteneur des
 * N cases, styleCadreExtérieur reutilise), TexteOptions (le texte de chaque
 * case, brique texte styleLibelléSaisie : Couleur/Police/Retraits/Cadre), Coche
 * (la marque cochee, styleCoche) et CadreCoche (la boite de la coche,
 * styleCadreExtérieur : CouleurFond + Cadre en QSS ::indicator). Statique (aucun
 * etat). Alias EN : styleSwitch. */
static ObjInstance* build_styleInterrupteur_template(void) {
    ObjString* name = obj_copy_string("styleInterrupteur", 17);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    {
        ObjInstance* t = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(t));
        set_field_instance(tpl, "TexteOptions", t);
        vm_pop();
    }
    {
        ObjInstance* k = build_styleCoche_template();
        vm_push(OBJ_VAL(k));
        set_field_instance(tpl, "Coche", k);
        vm_pop();
    }
    {
        ObjInstance* cc = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(cc));
        set_field_instance(tpl, "CadreCoche", cc);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* stylePuce — sous-element rond du bouton radio du Champ Sélecteur.
 * Feuille unique Couleur : la couleur du rond selectionne. Qt ne stylise pas
 * directement le glyphe radio (image thémée) ; le rendu approche colore le fond
 * de l'indicator selectionne (::indicator:checked background). Statique. */
static ObjInstance* build_stylePuce_template(void) {
    ObjString* name = obj_copy_string("stylePuce", 9);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int(tpl, "Couleur", -1);   /* -1 = non posee */
    vm_pop();
    return tpl;
}

/* styleSélecteur — modele de style typé du Champ Sélecteur (boutons
 * radio exclusifs). Pendant du styleInterrupteur adapte radio : CadreExtérieur
 * (le conteneur des N options, styleCadreExtérieur reutilise), TexteOptions (le
 * texte de chaque option, brique texte styleLibelléSaisie), Puce (le rond,
 * stylePuce) et CadrePuce (la boite du rond, styleCadreExtérieur). Statique
 * (aucun etat). Alias EN : styleSelector. */
static ObjInstance* build_styleSelecteur_template(void) {
    ObjString* name = obj_copy_string("styleSelecteur", 14);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    {
        ObjInstance* t = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(t));
        set_field_instance(tpl, "TexteOptions", t);
        vm_pop();
    }
    {
        ObjInstance* p = build_stylePuce_template();
        vm_push(OBJ_VAL(p));
        set_field_instance(tpl, "Puce", p);
        vm_pop();
    }
    {
        ObjInstance* cp = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(cp));
        set_field_instance(tpl, "CadrePuce", cp);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleListe — modele de style typé du Champ Liste (list box).
 * Struct de trois sous-elements : CadreExtérieur (le cadre + fond de la liste,
 * styleCadreExtérieur reutilise), LibelléLignes (le texte des items, brique
 * texte styleLibelléSaisie : Couleur/Police) et LigneSélectionnée (le texte de
 * l'item selectionne, brique texte styleLibelléSaisie). Statique (aucun etat).
 * Alias EN : styleList. */
static ObjInstance* build_styleListe_template(void) {
    ObjString* name = obj_copy_string("styleListe", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    {
        ObjInstance* l = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(l));
        set_field_instance(tpl, "LibelleLignes", l);
        vm_pop();
    }
    {
        ObjInstance* s = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(s));
        /*  C3 — le rendu par defaut de la selection est le bleu highlight
         * (fallback runtime `palette Highlight` dans owl_ui_liste_set_style).
         * On aligne le DEFAUT VISIBLE du Style dessus : sans ca, le Style montre
         * CouleurFond=-1 (pas de fond) alors que le rendu peint un fond bleu ->
         * incoherence designer/runtime. Bleu de selection #0078D7, packe BGR
         * (canon) : R=0x00 | G<<8=0x7800 | B<<16=0xD70000 = 0xD77800. */
        inst_field_set_int(s, "CouleurFond", 0xD77800);
        set_field_instance(tpl, "LigneSelectionnee", s);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleCombo — modele de style typé du Champ Combo (liste
 * deroulante). Composite qui reunit la structure du Saisie (Libellé integre +
 * zone + cadre) et le dropdown de la Liste (texte des items + item selectionne).
 * Cinq sous-elements :
 *   - Libellé            : le libellé integre a gauche (brique texte, comme styleSaisie..Libellé)
 *   - CadreExtérieur     : le conteneur englobant (comme styleSaisie..CadreExtérieur)
 *   - ZoneSaisie         : le champ combo lui-meme (brique stateful, comme styleSaisie..ZoneSaisie)
 *   - LibelléLignes      : le texte des items du popup (brique texte, comme styleListe..LibelléLignes)
 *   - LigneSélectionnée  : l'item surligné (brique texte, defaut CouleurFond #0078D7 comme styleListe)
 * Alias EN : styleCombobox. */
static ObjInstance* build_styleCombo_template(void) {
    ObjString* name = obj_copy_string("styleCombo", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* l = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(l));
        set_field_instance(tpl, "Libelle", l);
        vm_pop();
    }
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    {
        ObjInstance* z = build_styleZoneSaisie_template();
        vm_push(OBJ_VAL(z));
        set_field_instance(tpl, "ZoneSaisie", z);
        vm_pop();
    }
    {
        ObjInstance* l = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(l));
        set_field_instance(tpl, "LibelleLignes", l);
        vm_pop();
    }
    {
        ObjInstance* s = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(s));
        /*  C3 (repris de styleListe) — le rendu par defaut de la
         * selection est le bleu highlight #0078D7, packe BGR (canon) :
         * R=0x00 | G<<8=0x7800 | B<<16=0xD70000 = 0xD77800. On aligne le
         * defaut visible du Style dessus pour coherence designer/runtime. */
        inst_field_set_int(s, "CouleurFond", 0xD77800);
        set_field_instance(tpl, "LigneSelectionnee", s);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleQuadrillage — sous-element des separateurs (grille) du
 * Champ Table. Feuille unique Couleur : la couleur des lignes de separation
 * horizontales/verticales (gridline-color en QSS). -1 = non posee (couleur de
 * grille par defaut du theme). Statique. */
static ObjInstance* build_styleQuadrillage_template(void) {
    ObjString* name = obj_copy_string("styleQuadrillage", 16);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int(tpl, "Couleur", -1);   /* -1 = non posee */
    vm_pop();
    return tpl;
}

/* styleTable — modele de style typé du Champ Table (tableau).
 * Struct de huit sous-elements :
 *   - CadreExtérieur     : le cadre + fond du tableau (styleCadreExtérieur)
 *   - TitreColonne       : le texte des en-tetes de colonne (brique texte)
 *   - LibelléLignes      : le texte des cellules de ligne (brique texte)
 *   - LignesPaires       : le fond des lignes de rang pair (brique fond)
 *   - LignesImpaires     : le fond des lignes de rang impair (brique fond)
 *   - LigneSélectionnée  : le texte + fond de la ligne selectionnee (brique texte)
 *   - LigneSurvolée      : le texte + fond de la ligne survolee (brique texte)
 *   - Quadrillage        : la couleur des separateurs (styleQuadrillage)
 * Alias EN : styleTable. */
static ObjInstance* build_styleTable_template(void) {
    ObjString* name = obj_copy_string("styleTable", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    {
        ObjInstance* t = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(t));
        set_field_instance(tpl, "TitreColonne", t);
        vm_pop();
    }
    {
        ObjInstance* l = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(l));
        set_field_instance(tpl, "LibelleLignes", l);
        vm_pop();
    }
    {
        ObjInstance* p = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(p));
        set_field_instance(tpl, "LignesPaires", p);
        vm_pop();
    }
    {
        ObjInstance* im = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(im));
        set_field_instance(tpl, "LignesImpaires", im);
        vm_pop();
    }
    {
        ObjInstance* s = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(s));
        /* Aligne le defaut visible de la selection sur le rendu par defaut
         * (bleu highlight #0078D7, packe BGR canon) comme styleListe. */
        inst_field_set_int(s, "CouleurFond", 0xD77800);
        set_field_instance(tpl, "LigneSelectionnee", s);
        vm_pop();
    }
    {
        ObjInstance* h = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(h));
        set_field_instance(tpl, "LigneSurvolee", h);
        vm_pop();
    }
    {
        ObjInstance* q = build_styleQuadrillage_template();
        vm_push(OBJ_VAL(q));
        set_field_instance(tpl, "Quadrillage", q);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleJauge — modele de style typé du Champ Jauge (barre de
 * progression). Struct de trois sous-elements :
 *   - CadreExtérieur : la piste + bordure + arrondi + fond de la jauge, avec
 *                      surcharges par état (styleCadreExtérieur reutilise).
 *   - Remplissage    : la barre de progression (brique : seul CouleurFond
 *                      compte pour la portion remplie).
 *   - Libellé        : le texte du pourcentage (brique texte : Couleur/Police).
 * Alias EN : styleGauge. */
static ObjInstance* build_styleJauge_template(void) {
    ObjString* name = obj_copy_string("styleJauge", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    {
        ObjInstance* r = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(r));
        set_field_instance(tpl, "Remplissage", r);
        vm_pop();
    }
    {
        ObjInstance* l = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(l));
        set_field_instance(tpl, "Libelle", l);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleColonne — modele de style typé d'une Colonne de Table :
 * surcharge locale du styleTable, colonne par colonne. Sous-ensemble de
 * styleTable SANS CadreExtérieur ni Quadrillage (propres au tableau entier) :
 * TitreColonne, LibelléLignes, LignesPaires, LignesImpaires, LigneSélectionnée,
 * LigneSurvolée. Une feuille non renseignee sur la colonne retombe sur le
 * styleTable. Alias EN : styleColumn. */
static ObjInstance* build_styleColonne_template(void) {
    ObjString* name = obj_copy_string("styleColonne", 12);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* t = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(t));
        set_field_instance(tpl, "TitreColonne", t);
        vm_pop();
    }
    {
        ObjInstance* l = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(l));
        set_field_instance(tpl, "LibelleLignes", l);
        vm_pop();
    }
    {
        ObjInstance* p = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(p));
        set_field_instance(tpl, "LignesPaires", p);
        vm_pop();
    }
    {
        ObjInstance* im = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(im));
        set_field_instance(tpl, "LignesImpaires", im);
        vm_pop();
    }
    {
        ObjInstance* s = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(s));
        inst_field_set_int(s, "CouleurFond", 0xD77800);
        set_field_instance(tpl, "LigneSelectionnee", s);
        vm_pop();
    }
    {
        ObjInstance* h = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(h));
        set_field_instance(tpl, "LigneSurvolee", h);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleZoneRépétée — modele de style typé du Champ Zone répétée.
 * Conteneur a repetition : plus simple que la Table (pas de per-colonne, pas de
 * delegate ; chaque rep est un vrai QWidget stylé directement). Sept
 * sous-elements :
 *   - CadreExtérieur     : cadre + fond du widget ENTIER (styleCadreExtérieur)
 *   - CadreInterne       : cadre de CHAQUE repetition (styleCadreExtérieur)
 *   - LignesImpaires     : fond de base d'une rep (brique fond)
 *   - LignesPaires       : fond alterne d'une rep (brique fond)
 *   - LigneSélectionnée  : fond (+ texte) de la rep selectionnee (brique)
 *   - LigneSurvolée      : fond au survol (brique)
 *   - Séparateur         : trait entre reps (styleQuadrillage, feuille Couleur)
 * DEFAUTS SOBRES : tous les fonds transparents (-1), tous les cadres invisibles
 * (Épaisseur 0), aucun bleu de selection (contrairement a styleTable/styleListe/
 * styleCombo). La zone est discrete par defaut. Alias EN : styleLooper. */
static ObjInstance* build_styleZoneRepetee_template(void) {
    ObjString* name = obj_copy_string("styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", 19);  /* styleZoneRépétée */
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* c = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    {
        ObjInstance* ci = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(ci));
        set_field_instance(tpl, "CadreInterne", ci);
        vm_pop();
    }
    {
        ObjInstance* im = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(im));
        set_field_instance(tpl, "LignesImpaires", im);
        vm_pop();
    }
    {
        ObjInstance* p = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(p));
        set_field_instance(tpl, "LignesPaires", p);
        vm_pop();
    }
    {
        /* DEFAUT SOBRE : PAS de 0xD77800 (bleu highlight). La rep selectionnee
         * reste transparente tant que l'utilisateur ne pose rien. */
        ObjInstance* s = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(s));
        set_field_instance(tpl, "LigneSelectionnee", s);
        vm_pop();
    }
    {
        ObjInstance* h = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(h));
        set_field_instance(tpl, "LigneSurvolee", h);
        vm_pop();
    }
    {
        ObjInstance* q = build_styleQuadrillage_template();
        vm_push(OBJ_VAL(q));
        set_field_instance(tpl, "Separateur", q);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/*  (ticket C) — styleMenuPrincipal : brique texte des surfaces
 * stateful du menu (Barre + OptionMenu). Palette MINIMALE, pertinente pour un
 * menu : Couleur (texte), CouleurFond, Police. Pas de Retraits ni de Cadre
 * (contrairement a STYLE_BRICK_FIELDS) : un menu classique ne les stylise pas.
 * Sentinelles -1 = « non pose » -> repli sur le defaut du theme au rendu. */
static void install_menu_text_brick(ObjInstance* tpl, bool with_police) {
    inst_field_set_int(tpl, "Couleur",     -1);   /* -1 = non pose */
    inst_field_set_int(tpl, "CouleurFond", -1);   /* -1 = non pose */
    /*  — la police par état (survol) n'est pas rendue par Qt : on ne
     * l'expose donc que sur l'état de base (with_police), pas sur les surcharges
     * d'état (styleMenuPrincipal*Base). Les couleurs varient bien par état. */
    if (with_police) {
        ObjInstance* poli = build_Police_surcharge_template();
        vm_push(OBJ_VAL(poli));
        set_field_instance(tpl, "Police", poli);
        vm_pop();
    }
}

/* styleMenuPrincipalBarreBase — surcharge PLATE par etat de la
 * barre (ce que designe Barre..Etats[survol]). La brique texte seule, PAS de
 * membre Etats (fin de chaine, recursion inecrivable par construction). */
static ObjInstance* build_styleMenuPrincipalBarreBase_template(void) {
    ObjString* name = obj_copy_string("styleMenuPrincipalBarreBase", 27);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_menu_text_brick(tpl, false);  /* état survol : pas de Police */
    vm_pop();
    return tpl;
}

/* styleMenuPrincipalBarreEtats — collection d'etats de la barre.
 * Un seul etat pertinent : survol (entree survolee). Slot interne _Survol, un
 * styleMenuPrincipalBarreBase. Indexee via ..Etats[survol]. */
static ObjInstance* build_styleMenuPrincipalBarreEtats_template(void) {
    ObjString* name = obj_copy_string("styleMenuPrincipalBarreEtats", 28);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* s = build_styleMenuPrincipalBarreBase_template();
        vm_push(OBJ_VAL(s));
        set_field_instance(tpl, "_Survol", s);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleMenuPrincipalBarre — surface barre de menu (QMenuBar).
 * Stateful : la brique texte de base + le membre Etats (surcharge survol). */
static ObjInstance* build_styleMenuPrincipalBarre_template(void) {
    ObjString* name = obj_copy_string("styleMenuPrincipalBarre", 23);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_menu_text_brick(tpl, true);   /* état de base : Police exposée */
    {
        ObjInstance* et = build_styleMenuPrincipalBarreEtats_template();
        vm_push(OBJ_VAL(et));
        set_field_instance(tpl, "Etats", et);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleMenuPrincipalOptionBase — surcharge PLATE par etat d'une
 * rangee de menu (ce que designe OptionMenu..Etats[survol]). Brique texte
 * seule, pas de membre Etats. */
static ObjInstance* build_styleMenuPrincipalOptionBase_template(void) {
    ObjString* name = obj_copy_string("styleMenuPrincipalOptionBase", 28);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_menu_text_brick(tpl, false);  /* état survol : pas de Police */
    vm_pop();
    return tpl;
}

/* styleMenuPrincipalOptionEtats — collection d'etats d'une rangee.
 * Un seul etat : survol (item survole / item courant / parent ouvert = tous
 * :selected cote Qt). Slot interne _Survol. */
static ObjInstance* build_styleMenuPrincipalOptionEtats_template(void) {
    ObjString* name = obj_copy_string("styleMenuPrincipalOptionEtats", 29);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* s = build_styleMenuPrincipalOptionBase_template();
        vm_push(OBJ_VAL(s));
        set_field_instance(tpl, "_Survol", s);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleMenuPrincipalOption — surface d'une rangee de menu
 * (QMenu::item : options terminales ET rangees « ▶ » ouvrant un sous-menu).
 * Stateful : brique texte de base + membre Etats (survol). */
static ObjInstance* build_styleMenuPrincipalOption_template(void) {
    ObjString* name = obj_copy_string("styleMenuPrincipalOption", 24);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_menu_text_brick(tpl, true);   /* état de base : Police exposée */
    {
        ObjInstance* et = build_styleMenuPrincipalOptionEtats_template();
        vm_push(OBJ_VAL(et));
        set_field_instance(tpl, "Etats", et);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleMenuPrincipalSousMenu — surface du panneau deroulant
 * (QMenu). Statique : conteneur sans texte, deux feuilles seulement :
 * CouleurFond (fond du panneau) et CouleurSeparateur (trait de separation). */
static ObjInstance* build_styleMenuPrincipalSousMenu_template(void) {
    ObjString* name = obj_copy_string("styleMenuPrincipalSousMenu", 26);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int(tpl, "CouleurFond",       -1);   /* -1 = non pose */
    inst_field_set_int(tpl, "CouleurSeparateur", -1);   /* -1 = non pose */
    /*  lot 3-ter — bordure + arrondi du panneau (props plates). */
    inst_field_set_int(tpl, "Arrondi",        0);   /* rayon coins px */
    inst_field_set_int(tpl, "Epaisseur",      1);   /* liseré px */
    inst_field_set_int(tpl, "CouleurBordure", -1);  /* -1 = defaut theme */
    vm_pop();
    return tpl;
}

/* styleMenuPrincipal — modele de style typé du menu classique de
 * fenetre. Composite de 3 surfaces : Barre (QMenuBar, stateful), SousMenu
 * (panneau deroulant, statique) et OptionMenu (rangee, stateful). Toute prop
 * non posee retombe sur le defaut du theme au rendu. */
static ObjInstance* build_styleMenuPrincipal_template(void) {
    ObjString* name = obj_copy_string("styleMenuPrincipal", 18);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* b = build_styleMenuPrincipalBarre_template();
        vm_push(OBJ_VAL(b));
        set_field_instance(tpl, "Barre", b);
        vm_pop();
    }
    {
        ObjInstance* sm = build_styleMenuPrincipalSousMenu_template();
        vm_push(OBJ_VAL(sm));
        set_field_instance(tpl, "SousMenu", sm);
        vm_pop();
    }
    {
        ObjInstance* o = build_styleMenuPrincipalOption_template();
        vm_push(OBJ_VAL(o));
        set_field_instance(tpl, "OptionMenu", o);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleMenuContextuel — modele de style typé du menu contextuel
 * (clic droit). Composite de 2 surfaces SEULEMENT : SousMenu (le panneau
 * deroulant) et OptionMenu (une rangee, stateful). PAS de Barre : un menu
 * contextuel n'a pas de bande horizontale. Reutilise les MEMES surfaces que
 * styleMenuPrincipal (styleMenuPrincipalSousMenu / styleMenuPrincipalOption),
 * la composition seule differe. Toute prop non posee retombe sur le defaut. */
static ObjInstance* build_styleMenuContextuel_template(void) {
    ObjString* name = obj_copy_string("styleMenuContextuel", 19);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* sm = build_styleMenuPrincipalSousMenu_template();
        vm_push(OBJ_VAL(sm));
        set_field_instance(tpl, "SousMenu", sm);
        vm_pop();
    }
    {
        ObjInstance* o = build_styleMenuPrincipalOption_template();
        vm_push(OBJ_VAL(o));
        set_field_instance(tpl, "OptionMenu", o);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/*  (lot 5d) — styleOnglet : brique texte d'un volet (VoletActif /
 * VoletInactif). Palette MINIMALE : Couleur (texte de la languette), CouleurFond
 * (fond de la languette), Police. Sentinelles -1 = « non pose » -> repli sur le
 * defaut du theme au rendu. La surcharge par etat (survol) ne porte que les
 * couleurs (with_police=false) : la police par etat n'est pas rendue. */
static void install_volet_text_brick(ObjInstance* tpl, bool with_police) {
    inst_field_set_int(tpl, "Couleur",     -1);   /* -1 = non pose */
    inst_field_set_int(tpl, "CouleurFond", -1);   /* -1 = non pose */
    if (with_police) {
        ObjInstance* poli = build_Police_surcharge_template();
        vm_push(OBJ_VAL(poli));
        set_field_instance(tpl, "Police", poli);
        vm_pop();
    }
}

/* styleVoletBase — surcharge PLATE par etat d'un volet (ce que
 * designe VoletActif..Etats[survol]). Couleurs seules, PAS de Police par etat,
 * PAS de membre Etats (fin de chaine). */
static ObjInstance* build_styleVoletBase_template(void) {
    ObjString* name = obj_copy_string("styleVoletBase", 14);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_volet_text_brick(tpl, false);  /* etat survol : pas de Police */
    vm_pop();
    return tpl;
}

/* styleVoletEtats — collection des surcharges par etat d'un volet.
 * Un seul etat pertinent : survol (languette survolee). Slot interne _Survol,
 * un styleVoletBase. Indexee via ..Etats[survol]. */
static ObjInstance* build_styleVoletEtats_template(void) {
    ObjString* name = obj_copy_string("styleVoletEtats", 15);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* s = build_styleVoletBase_template();
        vm_push(OBJ_VAL(s));
        set_field_instance(tpl, "_Survol", s);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleVolet — surface d'un volet (la languette + son en-tete).
 * Type PARTAGE par les deux slots VoletActif / VoletInactif de styleOnglet :
 * ils sont structurellement identiques. Stateful : brique texte de base
 * (Couleur, CouleurFond, Police) + membre Etats (surcharge survol). */
static ObjInstance* build_styleVolet_template(void) {
    ObjString* name = obj_copy_string("styleVolet", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    install_volet_text_brick(tpl, true);   /* etat de base : Police exposee */
    inst_field_set_int(tpl, "Arrondi", -1); /*  : -1 = non pose (coins du theme) */
    {
        ObjInstance* et = build_styleVoletEtats_template();
        vm_push(OBJ_VAL(et));
        set_field_instance(tpl, "Etats", et);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleCadreExtérieurOnglet — CadreExtérieur RESTREINT du Champ
 * Onglet. Contrairement au styleCadreExtérieur complet (partage par Saisie /
 * Bouton / Table...), le pane d'un onglet n'expose que CouleurFond + un Cadre
 * simple (Couleur/Epaisseur/Arrondi) : ni Etats, ni Dégradé, ni Ombre, ni
 * Retraits, ni Trait/Bords. Miroir exact de ce que le parser onglet accepte. */
static ObjInstance* build_styleCadreExterieurOnglet_template(void) {
    ObjString* name = obj_copy_string("styleCadreExt\xc3\xa9rieurOnglet", 26);  /* styleCadreExtérieurOnglet */
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int(tpl, "CouleurFond", -1);   /* -1 = pas de fond */
    {
        ObjInstance* cadre = build_CadreOnglet_template();
        vm_push(OBJ_VAL(cadre));
        set_field_instance(tpl, "Cadre", cadre);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

/* styleOnglet (, refonte ) — modele de style typé du Champ
 * Onglet. Trois sous-elements composites : CadreExtérieur (fond + bordure du
 * pane, type restreint styleCadreExtérieurOnglet) + deux surfaces VoletActif /
 * VoletInactif (meme type styleVolet, stateful survol). Plus aucune prop
 * scalaire a la racine. Toute prop non posee retombe sur le defaut du theme au
 * rendu. Applique via un bloc 'Style' dans le Champ Onglet. */
static ObjInstance* build_styleOnglet_template(void) {
    ObjString* name = obj_copy_string("styleOnglet", 11);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    {
        ObjInstance* c = build_styleCadreExterieurOnglet_template();
        vm_push(OBJ_VAL(c));
        set_field_instance(tpl, "CadreExterieur", c);
        vm_pop();
    }
    {
        ObjInstance* a = build_styleVolet_template();
        vm_push(OBJ_VAL(a));
        set_field_instance(tpl, "VoletActif", a);
        vm_pop();
    }
    {
        ObjInstance* i = build_styleVolet_template();
        vm_push(OBJ_VAL(i));
        set_field_instance(tpl, "VoletInactif", i);
        vm_pop();
    }
    vm_pop();
    return tpl;
}

static ObjInstance* build_httpRequete_template(void) {
    ObjString* name = obj_copy_string("httpRequete", 11);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    /* 12 fields aligned with the canon. Global-level settings (proxy, user
     * agent, compression, redirection, TLS verify, keep-alive) are configured
     * via HTTPParametre, not via struct fields. */
    inst_field_set_str(tpl, "URL",          "");
    inst_field_set_str   (tpl, "Methode",      "GET");  /* httpGet = "GET" (string-valued) */
    set_field_buffer(tpl, "Contenu");           /* buffer by default */
    inst_field_set_str(tpl, "ContentType",  "application/x-www-form-urlencoded");
    set_field_assoc_chaines(tpl, "Entete");
    inst_field_set_int   (tpl, "DureeNonReponse",  20000);  /* canon : 20s par défaut */
    inst_field_set_int   (tpl, "TimeoutConnexion", 20000);  /* canon : 20s par défaut */

    /* Basic auth (canon-standard) */
    inst_field_set_str(tpl, "Utilisateur",    "");
    inst_field_set_str(tpl, "MotDePasse",     "");

    /* mTLS (client certificates) */
    inst_field_set_str(tpl, "CertificatClient",           "");
    inst_field_set_str(tpl, "MotDePasseCertificatClient", "");  /* canon */

    /* Palier 1 — new canonical properties. */
    inst_field_set_str(tpl, "AgentUtilisateur",       "");
    inst_field_set_str(tpl, "Destination",            "");
    inst_field_set_str(tpl, "CipherChoisi",           "");  /* read-only, set after handshake */
    inst_field_set_int   (tpl, "VersionSSLChoisie",      0);   /* read-only, set after handshake */

    /* Palier 2 — enum-constrained combinable fields. */
    inst_field_set_int   (tpl, "VersionSSL",             0);
    inst_field_set_int   (tpl, "IgnoreErreur",           0);

    return tpl;
}

static ObjInstance* build_httpReponse_template(void) {
    ObjString* name = obj_copy_string("httpReponse", 11);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    inst_field_set_int   (tpl, "CodeEtat",            0);
    inst_field_set_str(tpl, "DescriptionCodeEtat", "");
    inst_field_set_str(tpl, "Contenu",             "");
    inst_field_set_str(tpl, "ContentType",         "");
    set_field_assoc_chaines(tpl, "Entete");
    inst_field_set_str(tpl, "EnteteBrut",          "");
    inst_field_set_str(tpl, "URL",                 "");

    return tpl;
}

/* AWSSession — identifiants AWS statiques passés en 1er argument d'AWSRequête.
 * Tous les champs sont des chaînes, vides par défaut : la validation (région /
 * clés obligatoires) se fait à l'appel, pas à la construction. */
static ObjInstance* build_AWSSession_template(void) {
    ObjString* name = obj_copy_string("AWSSession", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    inst_field_set_str(tpl, "Region",     "");
    inst_field_set_str(tpl, "CleAcces",   "");
    inst_field_set_str(tpl, "CleSecrete", "");
    inst_field_set_str(tpl, "Jeton",      "");
    inst_field_set_str(tpl, "Endpoint",   "");
    return tpl;
}

static void set_field_array_empty(ObjInstance* inst, const char* name) {
    /*  : push inst AVANT obj_new_array. */
    vm_push(OBJ_VAL(inst));
    ObjArray* a = obj_new_array();
    vm_push(OBJ_VAL(a));
    inst_field_set_value(inst, name, OBJ_VAL(a));
    vm_pop();
    vm_pop();
}

static ObjInstance* build_FichierDisque_template(void) {
    ObjString* name = obj_copy_string("FichierDisque", 13);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    /* _handle: slot 1-based, 0 = non ouvert. Rempli par __FichierDisqueCoerce
     * à l'affectation = fOuvre(...). */
    inst_field_set_int(tpl, "_handle", 0);
    /* Propriétés publiques (doc 1410088883) */
    inst_field_set_int   (tpl, "Handle",     -1);    /* -1 si pas de fichier associé */
    inst_field_set_str(tpl, "NomFichier", "");
    return tpl;
}

/* FichierTéléversé — fichier reçu via un formulaire multipart/form-data en
 * paramètre direct de surRequête (DSL de service HTTP, ). Les
 * métadonnées (NomFichier/Taille/TypeMime) sont dissociées du contenu, lisibles
 * avant toute lecture. Le contenu est streamé vers un fichier temporaire dont
 * le chemin C interne vit dans _cheminTemp (jamais dérivé du nom client, qui
 * peut être hostile). Le binding serve-time (parse + remplissage + nettoyage)
 * est porté par le runtime multipart. Field names stored ASCII (le compilateur
 * accent-strippe les identifiants au compile-time). */
static ObjInstance* build_FichierTeleverse_template(void) {
    ObjString* name = obj_copy_string("FichierTeleverse", 16);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    /* Métadonnées publiques (lecture seule côté handler). */
    inst_field_set_str(tpl, "NomFichier", "");   /* nom client, jamais un chemin */
    inst_field_set_int(tpl, "Taille",     0);    /* octets */
    inst_field_set_str(tpl, "TypeMime",   "");   /* content-type de la partie */
    /* Chemin du fichier temporaire C (interne, rempli au binding serve-time). */
    inst_field_set_str(tpl, "_cheminTemp", "");
    return tpl;
}

static ObjInstance* build_Chrono_template(void) {
    ObjString* name = obj_copy_string("Chrono", 6);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    /* Internal state (mutated by native functions). Fields prefixed with '_'
     * by convention — not meant to be read directly by user code. */
    inst_field_set_int (tpl, "_start_ms",       0);
    inst_field_set_int (tpl, "_accumulated_ms", 0);
    inst_field_set_bool(tpl, "_running",        false);
    inst_field_set_bool(tpl, "_paused",         false);
    /* Public read-only properties. Storage ASCII (canon name dans
     * FIELDS_Chrono). Le runtime resout `Démarré` -> `Demarre` via
     * l'invariant 2 (accent-insensible), donc un seul enregistrement. */
    inst_field_set_bool(tpl, "Demarre", false);
    inst_field_set_int (tpl, "Valeur",  0);
    return tpl;
}

static ObjInstance* build_Timer_template(void) {
    ObjString* name = obj_copy_string("Timer", 5);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    /* Public read/write. Procedure laissée à Null (non assignée). */
    inst_field_set_int  (tpl, "Intervalle", 0);
    inst_field_set_value(tpl, "Procedure",  NIL_VAL);
    inst_field_set_bool (tpl, "Repete",     true);
    /* État interne (muté par fn_timer_demarre / fn_timer_arrete). */
    inst_field_set_bool (tpl, "_active", false);
    inst_field_set_bool (tpl, "_fired",  false);
    return tpl;
}

/* UUID — 128 bits (canon). Storage interne : un seul
 * field privé _bytes (buffer 16 octets, tout zéro à la construction). Le
 * format d'affichage canonique (36 chars hex à tirets sans accolades) est
 * généré à la coercion vers chaîne via owl_uuid_format_canonical (cf.
 * stdlib_uuid.c, hook depuis vm.c::value_to_string). */
static ObjInstance* build_UUID_template(void) {
    ObjString* name = obj_copy_string("UUID", 4);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    set_field_buffer_zeros(tpl, "_bytes", 16);
    return tpl;
}

static ObjInstance* build_Definition_template(void) {
    ObjString* name = obj_copy_string("Definition", 10);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_str(tpl, "Nom",               "");
    inst_field_set_int   (tpl, "Type",               0);
    inst_field_set_int   (tpl, "FamilleDefinition",  0);
    inst_field_set_bool  (tpl, "Nullable",           false);
    set_field_array_empty(tpl, "ChampsNoms");
    /* Parametres (Etats) : tableau de mini-Definition typees pour chaque
     * parametre formel (Nom + Type + TypeNom). Vide pour les autres types. */
    set_field_array_empty(tpl, "Parametres");
    /* Champs (Fenetres) : tableau de mini-Definition typees pour chaque
     * sous-controle (Nom + Type owlChamp*). Vide pour les autres types.
     * Note : pour les structures, ChampsNoms reste l'array de strings canon. */
    set_field_array_empty(tpl, "Champs");
    /* TypeNom : nom textuel du type tel qu'ecrit par l'utilisateur, utile
     * dans les mini-Definition de parametres ("Personne", "tableau de
     * Article"). Vide pour les types simples. */
    inst_field_set_str(tpl, "TypeNom", "");
    return tpl;
}

/* FormatNumérique — type structuré pour
 * configurer NumériqueVersChaîne sans masque chaîne. 11 propriétés.
 *
 * Field names stored ASCII : le compilateur accent-strippe les
 * identifiants au compile-time (cf. compiler.c::identifier_constant),
 * donc le runtime cherche par chaîne ASCII. La forme accentuée pour
 * affichage LSP/VSCode est portée par `nameAccented` dans FIELDS_*. */
static ObjInstance* build_FormatNumerique_template(void) {
    ObjString* name = obj_copy_string("FormatNumerique", 15);
    vm_push(OBJ_VAL(name));   /*  */
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_int   (tpl, "Alignement",           OWL_FNC_DROITE);
    inst_field_set_str(tpl, "CaractereRemplissage", " ");
    inst_field_set_int   (tpl, "ChiffresApresVirgule", 6);
    inst_field_set_int   (tpl, "LongueurMinimale",     0);
    inst_field_set_bool  (tpl, "Majuscules",           false);
    inst_field_set_int   (tpl, "Notation",             OWL_FNN_DECIMAL);
    inst_field_set_str(tpl, "Prefixe",              "");
    inst_field_set_str(tpl, "SeparateurDecimal",    "");      /* "" = locale */
    inst_field_set_str(tpl, "SeparateurMilliers",   "");      /* "" = none */
    inst_field_set_int   (tpl, "Signe",                OWL_FNS_MOINS_SI_NEGATIF);
    inst_field_set_int   (tpl, "SigneExponentielle",   OWL_FNS_TOUJOURS);
    return tpl;
}

/* FormatNombre — type léger orienté champ pour l'héritage de format projet →
 * champ. Trois propriétés ciblées (séparateurs + décimales). Le défaut de
 * ChiffresAprèsVirgule est la sentinelle "auto/court" (pas 6) : une instance
 * par défaut reproduit l'affichage plat courant, pas un arrondi. */
static ObjInstance* build_FormatNombre_template(void) {
    ObjString* name = obj_copy_string("FormatNombre", 12);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_str(tpl, "SeparateurDecimal",   "");   /* "" = point plat */
    inst_field_set_str(tpl, "SeparateurMilliers",  "");   /* "" = aucun */
    inst_field_set_int(tpl, "ChiffresApresVirgule", OWL_FNB_AUTO);  /* auto/court */
    return tpl;
}

/* FormatMonétaire — type orienté champ pour l'héritage de format monétaire
 * projet → champ. Miroir de FormatNombre, avec le symbole et sa position en
 * plus. Le défaut reproduit l'affichage monétaire plat courant : Symbole vide,
 * séparateurs plats, 2 décimales (précision monétaire canon), signe '-'. */
static ObjInstance* build_FormatMonetaire_template(void) {
    ObjString* name = obj_copy_string("FormatMon\xc3\xa9taire", 16);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Symbole",              "");   /* "" = pas de symbole */
    inst_field_set_int(tpl, "PositionSymbole",       OWL_POSSYM_APRES);
    inst_field_set_str(tpl, "SeparateurDecimal",    "");   /* "" = point plat */
    inst_field_set_str(tpl, "SeparateurMilliers",   "");   /* "" = aucun */
    inst_field_set_int(tpl, "ChiffresApresVirgule", 2);    /* précision monétaire */
    inst_field_set_int(tpl, "FormatNegatif",         OWL_FMN_MOINS);
    vm_pop();
    return tpl;
}

/* ================================================================
 * Registry declarations — builtin types & constants.
 * Each entry publishes metadata (name, category, fields) to the registry
 * so --vscode-data and --doc can enumerate them. The actual runtime
 * template instantiation + vm.globals registration still happens in
 * register_builtin_types() below.
 * ================================================================ */

/* Enums declared in stdlib_http.c. */
extern const OwlEnum E_httpMethodes;
extern const OwlEnum E_protocoleVersions;
extern const OwlEnum E_httpIgnoreErreur;
extern const OwlEnum E_typeMime;

static const OwlField FIELDS_httpRequete[] = {
    {"URL",              NULL,       "chaîne",                      "\"\"",   "URL cible", "Target URL", .nameEN="URL"},
    {"Methode",          "Méthode",  "chaîne",                      "httpGet", "méthode HTTP", "HTTP method",
     .allowedBase=&E_httpMethodes, .nameEN="Method"},
    {"Contenu",          NULL,       "buffer",                      "",       "corps de la requête", "request body", .nameEN="Content"},
    {"ContentType",      NULL,       "chaîne",                      "\"application/x-www-form-urlencoded\"", "Content-Type", "Content-Type header",
     .allowedBase=&E_typeMime, .nameEN="ContentType"},
    {"Entete",           "Entête",   "tableau associatif de chaînes", "",      "entêtes HTTP", "HTTP headers", .nameEN="Headers"},
    {"DureeNonReponse",  "DuréeNonRéponse", "entier",               "20000",  "timeout en ms (défaut 20s)", "timeout ms (default 20s)", .nameEN="Timeout"},
    {"TimeoutConnexion", NULL,       "entier",                      "20000",  "timeout connexion en ms (défaut 20s)", "connect timeout ms (default 20s)", .nameEN="ConnectionTimeout"},
    {"Utilisateur",      NULL,       "chaîne",                      "\"\"",   "Basic auth utilisateur", "Basic auth user", .nameEN="User"},
    {"MotDePasse",       NULL,       "chaîne",                      "\"\"",   "Basic auth mot de passe", "Basic auth password", .nameEN="Password"},
    {"CertificatClient", NULL,       "chaîne",                      "\"\"",   "chemin du fichier .p12 contenant le certificat à joindre à la requête", "path to .p12 file with client cert", .nameEN="ClientCertificate"},
    {"MotDePasseCertificatClient", NULL, "chaîne",                  "\"\"",   "mot de passe associé au certificat client", "client certificate password", .nameEN="ClientCertificatePassword"},
    {"AgentUtilisateur", NULL,       "chaîne",                      "\"\"",   "valeur de l'entête User-Agent", "User-Agent header value", .nameEN="UserAgent"},
    {"Destination",      NULL,       "chaîne",                      "\"\"",   "chemin fichier local pour sauver le contenu reçu", "local file path to save response content", .nameEN="Destination"},
    {"CipherChoisi",     NULL,       "chaîne",                      "\"\"",   "suite cipher négociée (lecture seule, post-handshake)", "negotiated cipher suite (read-only, post-handshake)", .nameEN="ChosenCipher"},
    {"VersionSSL",       NULL,       "entier",                      "ProtocoleDefaut", "bitmask des versions TLS acceptables", "bitmask of acceptable TLS versions",
     .allowedBase=&E_protocoleVersions, .nameEN="SSLVersion"},
    {"VersionSSLChoisie",NULL,       "entier",                      "ProtocoleDefaut", "version TLS négociée (lecture seule)", "negotiated TLS version (read-only)",
     .allowedBase=&E_protocoleVersions, .nameEN="ChosenSSLVersion"},
    {"IgnoreErreur",     NULL,       "entier",                      "0",      "bitmask d'erreurs HTTP/TLS à ignorer", "bitmask of HTTP/TLS errors to ignore",
     .allowedBase=&E_httpIgnoreErreur, .nameEN="IgnoreError"},
    {0},
};

static const OwlField FIELDS_httpReponse[] = {
    {"CodeEtat",             NULL,      "entier",                      "0",    "code HTTP (200, 404…)", "HTTP status code", .nameEN="StatusCode"},
    {"DescriptionCodeEtat",  NULL,      "chaîne",                      "\"\"", "OK / Not Found / …", "status text", .nameEN="StatusText"},
    {"Contenu",              NULL,      "chaîne",                      "\"\"", "corps de la réponse", "response body", .nameEN="Content"},
    {"ContentType",          NULL,      "chaîne",                      "\"\"", "Content-Type", "Content-Type", .nameEN="ContentType"},
    {"Entete",               "Entête",  "tableau associatif de chaînes", "",  "entêtes de réponse", "response headers", .nameEN="Headers"},
    {"EnteteBrut",           "EntêteBrut","chaîne",                      "\"\"", "entêtes bruts", "raw headers", .nameEN="RawHeaders"},
    {"URL",                  NULL,      "chaîne",                      "\"\"", "URL finale après redirects", "final URL after redirects", .nameEN="URL"},
    {0},
};

static const OwlField FIELDS_AWSSession[] = {
    {"Region",     "Région",     "chaîne", "\"\"", "région AWS (ex. us-east-1)", "AWS region (e.g. us-east-1)", .nameEN="Region"},
    {"CleAcces",   "CléAccès",   "chaîne", "\"\"", "identifiant de clé d'accès", "access key id", .nameEN="AccessKey"},
    {"CleSecrete", "CléSecrète", "chaîne", "\"\"", "clé d'accès secrète", "secret access key", .nameEN="SecretKey"},
    {"Jeton",      NULL,         "chaîne", "\"\"", "jeton de session optionnel (credentials temporaires)", "optional session token (temporary credentials)", .nameEN="Token"},
    {"Endpoint",   NULL,         "chaîne", "\"\"", "endpoint personnalisé (vide = dérivé du service et de la région)", "custom endpoint (empty = derived from service and region)", .nameEN="Endpoint"},
    {0},
};

static const OwlField FIELDS_Chrono[] = {
    /* Champs computed : la valeur stockee est ignoree partout au runtime,
     * le getter delegue a fn_chrono_demarre / fn_chrono_valeur pour
     * refleter l'etat live. Branche dans OP_STRUCT_GET (cf ) +
     * value_to_json + debugger fill_var (cf ). */
    {"Demarre", "Démarré", "booléen", "Faux", "Vrai si démarré (lecture seule)", "True if started (read-only)", .nameEN="Started", .computeGetter=fn_chrono_demarre},
    {"Valeur",  NULL,      "entier",  "0",    "durée courante en ms", "current duration in ms", .nameEN="Value", .computeGetter=fn_chrono_valeur},
    {0},
};

/*  — Timer. Champs publics read/write (Intervalle/Repete/Procedure)
 * + Actif computé read-only (reflète l'état interne _active via
 * fn_timer_actif). L'écriture de ..Actif et l'affectation d'une procédure
 * exigeant des paramètres sont rejetées par owl_timer_on_field_set
 * (onFieldSet du type). */
static const OwlField FIELDS_Timer[] = {
    {"Intervalle", NULL, "durée",     "0",    "période entre deux déclenchements (durée ; un entier est coercé en millisecondes)", "period between two triggers (duration; an integer is coerced to milliseconds)", .nameEN="Interval"},
    {"Procedure",  "Procédure", "procédure", "Null", "procédure sans paramètre appelée à chaque déclenchement (référence directe ou lambda)", "parameterless procedure called on each trigger (direct reference or lambda)", .nameEN="Procedure", .callbackSig=""},
    {"Repete",     "Répète",    "booléen", "Vrai", "Vrai = déclenchements répétés ; Faux = un seul déclenchement puis arrêt", "True = repeated triggers; False = a single trigger then stop", .nameEN="Repeat"},
    {"Actif",      NULL, "booléen",   "Faux", "Vrai si le timer est armé (lecture seule — passer par TimerDémarre/TimerArrête)", "True if the timer is armed (read-only — use TimerStart/TimerStop)", .nameEN="Active", .computeGetter=fn_timer_actif},
    {0},
};

static const OwlField FIELDS_Image[] = {
    {"Largeur",            NULL, "entier",  "0",     "largeur en pixels",                    "width in pixels", .nameEN="Width"},
    {"Hauteur",            NULL, "entier",  "0",     "hauteur en pixels",                    "height in pixels", .nameEN="Height"},
    {"Valide",             NULL, "booléen", "Faux",  "Vrai si l'image a été chargée ou créée correctement (lecture seule)", "true if image was loaded or created correctly (read-only)", .nameEN="Valid"},
    {"AvecAlpha",          NULL, "booléen", "Faux",  "Vrai si l'image contient un canal alpha (lecture seule)", "true if image has alpha channel (read-only)", .nameEN="WithAlpha"},
    {"BitParPixel",        NULL, "entier",  "0",     "nombre de bits par pixel (lecture seule)", "bits per pixel (read-only)", .nameEN="BitsPerPixel"},
    {"CouleurRemplissage", NULL, "couleur", "Blanc", "couleur de remplissage utilisée lors d'un redimensionnement ou d'une rotation", "fill color used during resize or rotation", .nameEN="FillColor"},
    {"EchelleDessin",      NULL, "réel",    "1.0",   "facteur multiplicateur appliqué aux coordonnées des fonctions dXxx", "scale factor applied to coords of dXxx drawing functions", .nameEN="DrawingScale"},
    {0},
};

static const OwlField FIELDS_FichierDisque[] = {
    {"Handle",     NULL, "entier", "0",  "handle système (lecture seule, -1 si non ouvert)", "system handle (read-only, -1 if closed)", .nameEN="Handle"},
    {"NomFichier", NULL, "chaîne", "\"\"", "chemin du fichier (lecture seule)", "file path (read-only)", .nameEN="FileName"},
    {0},
};

static const OwlField FIELDS_FichierTeleverse[] = {
    {"NomFichier", NULL, "chaîne", "\"\"", "nom d'origine du fichier tel qu'envoyé par le client (lecture seule)", "original file name as sent by the client (read-only)", .nameEN="FileName"},
    {"Taille",     NULL, "entier", "0",    "taille du fichier en octets (lecture seule)", "file size in bytes (read-only)", .nameEN="Size"},
    {"TypeMime",   NULL, "chaîne", "\"\"", "type MIME déclaré de la partie (lecture seule)", "declared MIME type of the part (read-only)", .nameEN="MimeType"},
    {0},
};

static const OwlField FIELDS_Definition[] = {
    {"Nom",                NULL, "chaîne",  "\"\"",   "nom du type", "type name",
     .allowedBase=NULL, .nameEN="Name"},
    {"Type",               NULL, "entier",  "0",      "code owl* du type", "owl* type code",
     .allowedBase=&E_owlTypes, .nameEN="Type"},
    {"FamilleDefinition", "FamilleDéfinition", "entier", "0", "famille (structure, classe…)", "family (structure, class…)",
     .allowedBase=&E_familleDefinition, .nameEN="DefinitionFamily"},
    {"Nullable",           NULL, "booléen", "Faux",   "accepte Null", "accepts Null",
     .allowedBase=NULL, .nameEN="Nullable"},
    {"ChampsNoms",         NULL, "tableau de chaînes", "", "noms des champs", "field names",
     .allowedBase=NULL, .nameEN="FieldNames"},
    /* Champs ajoutes en B.10 : exposes par le template runtime mais qui
     * manquaient a la declaration FIELDS_Definition (incoherence remontee
     * lors de la migration bilingue — voir post-mortem self-test). */
    {"Parametres", "Paramètres", "tableau", "[]",
     "parametres formels (Etats) : mini-Definition par parametre (Nom + Type + TypeNom). Vide sinon.",
     "formal parameters (Etats): mini-Definition per param (Name + Type + TypeName). Empty otherwise.",
     .allowedBase=NULL, .nameEN="Parameters"},
    {"Champs",     NULL, "tableau", "[]",
     "sous-controles (Fenetres) : mini-Definition par champ (Nom + Type owlChamp*). Vide sinon.",
     "sub-controls (Windows): mini-Definition per field (Name + Type owlChamp*). Empty otherwise.",
     .allowedBase=NULL, .nameEN="Fields"},
    {"TypeNom",    NULL, "chaîne", "\"\"",
     "nom textuel du type tel qu'ecrit par l'utilisateur (\"Personne\", \"tableau de Article\"). Vide pour les types simples.",
     "user-written type name (\"Personne\", \"array of Article\"). Empty for simple types.",
     .allowedBase=NULL, .nameEN="TypeName"},
    {0},
};

/* FormatNumérique — 11 propriétés. */
static const OwlField FIELDS_FormatNumerique[] = {
    {"Alignement",            NULL,                    "entier",   "fncDroite",
     "Alignement / position du caractère de remplissage.", "Text alignment / fill character position.",
     .allowedBase=&E_fnAlignement, .nameEN="Alignment"},
    {"CaractereRemplissage",  "CaractèreRemplissage",  "chaîne",   "\" \"",
     "Caractère utilisé pour combler la sortie (1 char).", "Fill character (1 char).",
     .allowedBase=NULL, .nameEN="FillCharacter"},
    {"ChiffresApresVirgule",  "ChiffresAprèsVirgule",  "entier",   "6",
     "Nombre de décimales (défaut 6, < 100).", "Decimal places (default 6, < 100).",
     .allowedBase=NULL, .nameEN="DecimalPlaces"},
    {"LongueurMinimale",      NULL,                    "entier",   "0",
     "Longueur minimale de la sortie (< 100).", "Minimum output length (< 100).",
     .allowedBase=NULL, .nameEN="MinimumLength"},
    {"Majuscules",            NULL,                    "booléen",  "Faux",
     "Lettres en majuscules (hexadécimal etc).", "Use uppercase for letter formats (hex...).",
     .allowedBase=NULL, .nameEN="Uppercase"},
    {"Notation",              NULL,                    "entier",   "fnnDécimal",
     "Notation de sortie (binaire, décimal, exp, hex, octal).", "Output notation.",
     .allowedBase=&E_fnNotation, .nameEN="Notation"},
    {"Prefixe",               "Préfixe",               "chaîne",   "\"\"",
     "Préfixe : chaîne libre, ou fnpAuto pour préfixe auto selon notation.", "Prefix: free string or fnpAuto.",
     .allowedBase=&E_fnPrefixe, .nameEN="Prefix"},
    {"SeparateurDecimal",     "SéparateurDécimal",     "chaîne",   "\"\"",
     "Séparateur décimal (défaut: locale système).", "Decimal separator (default: system locale).",
     .allowedBase=NULL, .nameEN="DecimalSeparator"},
    {"SeparateurMilliers",    "SéparateurMilliers",    "chaîne",   "\"\"",
     "Séparateur de milliers (caractère). \"\" = aucun.", "Thousands separator. \"\" = none.",
     .allowedBase=NULL, .nameEN="ThousandsSeparator"},
    {"Signe",                 NULL,                    "entier",   "fnsMoinsSiNégatif",
     "Affichage du signe.", "Sign display mode.",
     .allowedBase=&E_fnSigne, .nameEN="Sign"},
    {"SigneExponentielle",    NULL,                    "entier",   "fnsToujours",
     "Affichage du signe de l'exposant en notation exponentielle.", "Exponent sign display.",
     .allowedBase=&E_fnSigne, .nameEN="ExponentSign"},
    {0},
};

/* FormatNombre — type léger d'héritage de format projet → champ (3 propriétés).
 * ChiffresAprèsVirgule défaut = fnbAuto (auto/court), pas 6 : une instance par
 * défaut reproduit l'affichage plat courant. */
static const OwlField FIELDS_FormatNombre[] = {
    {"SeparateurDecimal",     "SéparateurDécimal",     "chaîne",   "\"\"",
     "Séparateur décimal utilisé à l'affichage. Vide = point plat.",
     "Decimal separator used for display. Empty = plain dot.",
     .allowedBase=NULL, .nameEN="DecimalSeparator"},
    {"SeparateurMilliers",    "SéparateurMilliers",    "chaîne",   "\"\"",
     "Séparateur de milliers inséré à l'affichage. Vide = aucun.",
     "Thousands separator inserted for display. Empty = none.",
     .allowedBase=NULL, .nameEN="ThousandsSeparator"},
    {"ChiffresApresVirgule",  "ChiffresAprèsVirgule",  "entier",   "fnbAuto",
     "Nombre de décimales affichées. fnbAuto (défaut) = forme plate courte, sans arrondi fixe.",
     "Number of displayed decimals. fnbAuto (default) = short flat form, no fixed rounding.",
     .allowedBase=NULL, .nameEN="DecimalPlaces"},
    {0},
};

/*  — enums du type FormatMonétaire. PositionSymbole (avant/après le
 * montant) et FormatNégatif (signe '-' vs parenthèses comptables). Valeurs en
 * zone sentinelle (cf. sentinels.h) ; formes EN portées par les constantes. */
static const char* const E_positionSymbole_values[] = { "posAvant", "posApr\xc3\xa8s", NULL };
const OwlEnum E_positionSymbole = {
    .name="positionSymbole", .nameAscii="positionSymbole", .nameEN="symbolPosition",
    .category="FormatMon\xc3\xa9taire",
    .descFR="Position du symbole monétaire par rapport au montant (avant ou après).",
    .descEN="Currency symbol position relative to the amount (before or after).",
    .values=E_positionSymbole_values, .combinable=false
};
static const char* const E_formatMonetaireNegatif_values[] = {
    "fmnMoins", "fmnParenth\xc3\xa8ses", NULL
};
const OwlEnum E_formatMonetaireNegatif = {
    .name="formatMon\xc3\xa9taireN\xc3\xa9gatif", .nameAscii="formatMonetaireNegatif",
    .nameEN="currencyNegativeFormat", .category="FormatMon\xc3\xa9taire",
    .descFR="Motif d'affichage des montants négatifs : signe '-' ou parenthèses comptables.",
    .descEN="Display pattern for negative amounts: '-' sign or accounting parentheses.",
    .values=E_formatMonetaireNegatif_values, .combinable=false
};

/* FormatMonétaire — type d'héritage de format monétaire projet → champ (6
 * propriétés). Miroir de FIELDS_FormatNombre + Symbole/PositionSymbole/
 * FormatNégatif. ChiffresAprèsVirgule défaut = 2 (précision monétaire), Symbole
 * défaut vide : une instance par défaut reproduit l'affichage monétaire plat. */
static const OwlField FIELDS_FormatMonetaire[] = {
    {"Symbole",               NULL,                    "chaîne",   "\"\"",
     "Symbole monétaire affiché (ex. \"€\", \"$\"). Vide = aucun symbole.",
     "Displayed currency symbol (e.g. \"€\", \"$\"). Empty = no symbol.",
     .allowedBase=NULL, .nameEN="Symbol"},
    {"PositionSymbole",       NULL,                    "entier",   "posApr\xc3\xa8s",
     "Position du symbole (posAvant / posAprès). Défaut posAprès.",
     "Symbol position (posBefore / posAfter). Default posAfter.",
     .allowedBase=&E_positionSymbole, .nameEN="SymbolPosition"},
    {"SeparateurDecimal",     "SéparateurDécimal",     "chaîne",   "\"\"",
     "Séparateur décimal utilisé à l'affichage. Vide = point plat.",
     "Decimal separator used for display. Empty = plain dot.",
     .allowedBase=NULL, .nameEN="DecimalSeparator"},
    {"SeparateurMilliers",    "SéparateurMilliers",    "chaîne",   "\"\"",
     "Séparateur de milliers inséré à l'affichage. Vide = aucun.",
     "Thousands separator inserted for display. Empty = none.",
     .allowedBase=NULL, .nameEN="ThousandsSeparator"},
    {"ChiffresApresVirgule",  "ChiffresAprèsVirgule",  "entier",   "2",
     "Nombre de décimales affichées. Défaut 2 (précision monétaire).",
     "Number of displayed decimals. Default 2 (currency precision).",
     .allowedBase=NULL, .nameEN="DecimalPlaces"},
    {"FormatNegatif",         "FormatNégatif",         "entier",   "fmnMoins",
     "Motif du négatif : fmnMoins ('-') ou fmnParenthèses (style comptable). Défaut fmnMoins.",
     "Negative pattern: fmnMinus ('-') or fmnParentheses (accounting). Default fmnMinus.",
     .allowedBase=&E_formatMonetaireNegatif, .nameEN="NegativeFormat"},
    {0},
};

static const char* const ALIAS_httpRequete[] = { "httpRequete", "httpRequest", NULL };
static const char* const ALIAS_httpReponse[] = { "httpReponse", "httpResponse", NULL };
static const char* const ALIAS_pdfParametre[] = { "pdfParametre", "pdfParameter", NULL };
static const char* const ALIAS_FichierDisque[] = { "DiskFile", NULL };
static const char* const ALIAS_FichierTeleverse[] = { "UploadedFile", NULL };
static const char* const ALIAS_Police[]      = { "Police", "Font", NULL };
static const char* const ALIAS_Cadre[]        = { "Cadre", "Frame", NULL };
static const char* const ALIAS_CadreOnglet[]  = { "TabFrame", NULL };
static const char* const ALIAS_Style[]        = { "Style", NULL };
static const char* const ALIAS_styleSaisie[]  = { "styleInput", NULL };
static const char* const ALIAS_styleZoneSaisie[]      = { "styleInputZone", NULL };
static const char* const ALIAS_styleZoneSaisieBase[]  = { "styleInputZoneBase", NULL };
static const char* const ALIAS_styleZoneSaisieEtats[] = { "styleInputZoneStates", NULL };
static const char* const ALIAS_styleLibelleSaisie[]   = { "styleLibelleSaisie", "styleInputLabel", NULL };
static const char* const ALIAS_styleLibelle[]         = { "styleLibelle", "styleLabel", NULL };
static const char* const ALIAS_styleCadreExterieur[]  = { "styleCadreExterieur", "styleOuterFrame", NULL };
static const char* const ALIAS_styleCadreExterieurBase[]  = { "styleOuterFrameBase", NULL };
static const char* const ALIAS_styleCadreExterieurEtats[] = { "styleOuterFrameStates", NULL };
static const char* const ALIAS_styleOmbre[]           = { "styleShadow", NULL };
static const char* const ALIAS_styleDegrade[]         = { "styleGradient", NULL };
static const char* const ALIAS_styleBouton[]          = { "styleButton", NULL };
static const char* const ALIAS_styleImage[]           = { "styleImage", NULL };
static const char* const ALIAS_styleCodeBarre[]       = { "styleBarCode", NULL };
static const char* const ALIAS_styleConteneur[]       = { "styleContainer", NULL };
static const char* const ALIAS_styleInterrupteur[]    = { "styleSwitch", NULL };
static const char* const ALIAS_styleCoche[]           = { "styleCheckMark", NULL };
static const char* const ALIAS_styleSelecteur[]       = { "styleSelector", NULL };
static const char* const ALIAS_stylePuce[]            = { "styleRadioMark", NULL };
static const char* const ALIAS_styleListe[]           = { "styleList", NULL };
static const char* const ALIAS_styleCombo[]           = { "styleCombobox", NULL };
static const char* const ALIAS_styleTable[]           = { NULL };  /* "Table" identique FR/EN */
static const char* const ALIAS_styleJauge[]           = { "styleGauge", NULL };
static const char* const ALIAS_styleColonne[]         = { "styleColumn", NULL };
static const char* const ALIAS_styleQuadrillage[]     = { "styleGridlines", NULL };
static const char* const ALIAS_styleZoneRepetee[]     = { "styleLooper", NULL };
static const char* const ALIAS_styleImageBouton[]     = { "styleButtonImage", NULL };
static const char* const ALIAS_styleImageBoutonBase[]  = { "styleButtonImageBase", NULL };
static const char* const ALIAS_styleImageBoutonEtats[] = { "styleButtonImageStates", NULL };
static const char* const ALIAS_styleZoneBouton[]      = { "styleButtonZone", NULL };
static const char* const ALIAS_styleZoneBoutonBase[]  = { "styleButtonZoneBase", NULL };
static const char* const ALIAS_styleZoneBoutonEtats[] = { "styleButtonZoneStates", NULL };
/*  (ticket C) — styleMenuPrincipal : formes EN parent-prefixees,
 * coherentes avec le DSL menu (MainMenu/Bar/Submenu/MenuOption). */
static const char* const ALIAS_styleMenuPrincipal[]           = { "styleMainMenu", NULL };
static const char* const ALIAS_styleMenuPrincipalBarre[]      = { "styleMainMenuBar", NULL };
static const char* const ALIAS_styleMenuPrincipalBarreBase[]  = { "styleMainMenuBarBase", NULL };
static const char* const ALIAS_styleMenuPrincipalBarreEtats[] = { "styleMainMenuBarStates", NULL };
static const char* const ALIAS_styleMenuPrincipalSousMenu[]   = { "styleMainMenuSubmenu", NULL };
static const char* const ALIAS_styleMenuPrincipalOption[]      = { "styleMainMenuOption", NULL };
static const char* const ALIAS_styleMenuPrincipalOptionBase[]  = { "styleMainMenuOptionBase", NULL };
static const char* const ALIAS_styleMenuPrincipalOptionEtats[] = { "styleMainMenuOptionStates", NULL };
/*  — styleMenuContextuel : forme EN parent-prefixee coherente avec
 * styleMainMenu (styleMenuPrincipal -> styleMainMenu). */
static const char* const ALIAS_styleMenuContextuel[]           = { "styleContextMenu", NULL };
/*  (lot 5d) — styleOnglet : formes EN parent-prefixees (styleTab),
 * volet partage stylePane. */
static const char* const ALIAS_styleOnglet[]      = { "styleTab", NULL };
/*  — CadreExtérieur restreint du pane d'onglet (forme ascii + EN). */
static const char* const ALIAS_styleCadreExterieurOnglet[] = { "styleCadreExterieurOnglet", "styleTabOuterFrame", NULL };
static const char* const ALIAS_styleVolet[]       = { "stylePane", NULL };
static const char* const ALIAS_styleVoletBase[]   = { "stylePaneBase", NULL };
static const char* const ALIAS_styleVoletEtats[]  = { "stylePaneStates", NULL };
static const char* const ALIAS_Projet[]       = { "Project", NULL };
static const char* const ALIAS_Coin[]         = { "Corner", NULL };
static const char* const ALIAS_Trait[]        = { "Stroke", NULL };
static const char* const ALIAS_FormatNumerique[] = { "NumberFormat", NULL };
/*  — alias EN canon des types Champ / Groupe. */
static const char* const ALIAS_Champ[]        = { "Control", NULL };
static const char* const ALIAS_Groupe[]       = { "Group", NULL };

/* SQL.2.a : types reifies du moteur SQL embarque.
 * Convention : 1er arg = canon ASCII (storage runtime), 2e arg = forme
 * accentuee FR (info LSP/doc, le compilateur OWL fait l'accent-strip
 * pour rediriger vers le canon ASCII). Cf pattern httpRequete. */
extern const OwlEnum E_Couleur;
extern const OwlEnum E_Curseurs;   /* defini dans ui_catalog.c (enum curseurs) */
extern const OwlEnum E_ftpMode;    /* defini dans stdlib_ftp.c */
#ifdef OWL_HAS_SQL
extern const OwlEnum E_tlsModes;
extern const OwlEnum E_sqlBackend;
static const OwlField FIELDS_sqlConnexion[] = {
    {"Provider",      NULL,             "entier", "sqlSqlite", "backend SQL (sqlSqlite/sqlPostgres/sqlMySQL)", "SQL backend",
     .allowedBase=&E_sqlBackend, .nameEN="Provider"},
    {"Source",        NULL,             "chaîne", "\"\"",      "fichier SQLite, ou DSN ODBC", "SQLite file path, or ODBC DSN", .nameEN="Source"},
    {"Serveur",       NULL,             "chaîne", "\"\"",      "host PG/MySQL (format \"host\" ou \"host:port\")", "PG/MySQL host (format \"host\" or \"host:port\")", .nameEN="Server"},
    {"Port",          NULL,             "entier", "0",         "port TCP explicite (0 = parse depuis Serveur, défaut 5432 pour PG)", "explicit TCP port (0 = parse from Server, default 5432 for PG)", .nameEN="Port"},
    {"BaseDeDonnees", "BaseDeDonnées",  "chaîne", "\"\"",      "nom de la base (PG/MySQL)", "database name (PG/MySQL)", .nameEN="Database"},
    {"Utilisateur",   NULL,             "chaîne", "\"\"",      "nom d'utilisateur", "user name", .nameEN="User"},
    {"MotDePasse",    NULL,             "chaîne", "\"\"",      "mot de passe", "password", .nameEN="Password"},
    {"ModeTLS",       NULL,             "entier",  "tlsVerifieComplet", "niveau TLS pour la connexion (aligne Postgres sslmode)", "TLS level for the connection (aligned with Postgres sslmode)",
     .allowedBase=&E_tlsModes, .nameEN="TLSMode"},
    {"CertificatCA",  NULL,             "chaîne",  "\"\"",
     "Chemin d'un fichier PEM contenant la CA à utiliser comme racine de confiance pour la vérification du certificat serveur. Sert quand le serveur présente un certificat signé par une CA interne (CA d'entreprise, AD CS, certificat auto-signé) qui n'est pas dans le bundle Mozilla embarqué dans OWL. Combiné avec ModeTLS = tlsVerifieCA ou tlsVerifieComplet. Chaîne vide = bundle Mozilla par défaut. Si fourni, remplace la chaîne Mozilla (le champ exprime un choix de confiance explicite).",
     "Path to a PEM file holding the CA to use as trusted root for the server certificate verification. Useful when the server presents a certificate signed by an internal CA (enterprise CA, AD CS, self-signed) that is not in the Mozilla bundle embedded in OWL. Combined with ModeTLS = tlsVerifieCA or tlsVerifieComplet. Empty string = default Mozilla bundle. If provided, replaces the Mozilla chain (the field expresses an explicit trust choice).",
     .nameEN="CACertificate"},
    /*  — defaut 0 et non 5 : c'est le 0 qui distingue "jamais
     * renseigne" de "pose par l'utilisateur", et sans cette distinction la
     * garde SQL_PROP_SCOPES alerterait sur toute connexion (meme piege que
     * ModeTLS, muet parce que son defaut est non vide). Les sites de lecture
     * normalisent 0 -> 5 s. */
    {"TimeoutCnx",    NULL,             "entier",  "0",
     "Délai d'attente à l'ouverture de la connexion, exprimé en secondes. 0 = délai par défaut (5 secondes). Pris en compte par les connecteurs sqlODBC, sqlHFSQL, sqlHFSQLClassic et sqlMSSQL ; les autres ouvrent avec le délai propre à leur bibliothèque cliente.",
     "Connection opening timeout, expressed in seconds. 0 = default delay (5 seconds). Honoured by the sqlODBC, sqlHFSQL, sqlHFSQLClassic and sqlMSSQL connectors; the others open with the delay of their own client library.",
     .nameEN="ConnectTimeout"},
    {"Pilote",        NULL,             "chaîne",  "\"\"",
     "Nom du driver ODBC tel qu'enregistré sur le poste (ex : \"HFSQL\", \"Oracle in OraClient21Home1\", \"Microsoft Access Driver (*.mdb, *.accdb)\"). Utilisé uniquement quand Provider = sqlODBC. Si Source est renseigné, il prend la priorité (connection string complète style ADO.NET) ; sinon OWL assemble la chaîne depuis Pilote, Serveur, BaseDeDonnees, Utilisateur, MotDePasse.",
     "ODBC driver name as registered on the host (ex: \"HFSQL\", \"Oracle in OraClient21Home1\", \"Microsoft Access Driver (*.mdb, *.accdb)\"). Used only when Provider = sqlODBC. If Source is provided, it takes priority (full connection string, ADO.NET style) ; otherwise OWL assembles the string from Pilote, Serveur, BaseDeDonnees, Utilisateur, MotDePasse.",
     .nameEN="Driver"},
    {"Analyse",       NULL,             "chaîne",  "\"\"",
     "Chemin du fichier d'analyse (schéma) d'une base HFSQL locale. Obligatoire quand Provider = sqlHFSQLClassic : sans lui, impossible de requêter les fichiers de données.",
     "Path to the analysis file (schema) of a local HFSQL database. Required when Provider = sqlHFSQLClassic: without it, the data files cannot be queried.",
     .nameEN="Analysis"},
    {"Repertoire",    "Répertoire",     "chaîne",  "\"\"",
     "Dossier contenant les fichiers de données d'une base HFSQL locale. Obligatoire quand Provider = sqlHFSQLClassic.",
     "Folder holding the data files of a local HFSQL database. Required when Provider = sqlHFSQLClassic.",
     .nameEN="Directory"},
    {0},
};
static const OwlField FIELDS_sqlRequete[] = {
    {"Code",        NULL,          "chaîne", "\"\"", "texte SQL avec placeholders {nom}", "SQL text with {name} placeholders", .nameEN="Code"},
    {"Parametres",  "Paramètres",  "tableau associatif", "[]", "valeurs des placeholders nommés", "values of named placeholders", .nameEN="Parameters"},
    {0},
};
static const OwlField FIELDS_sqlReponse[] = {
    {"Succes",          "Succès",            "booléen", "Faux", "vrai si la requête a réussi", "true if request succeeded", .nameEN="Success"},
    {"Lignes",          NULL,                "tableau", "[]",   "lignes retournées (tableau de tableau associatif)", "returned rows (array of associative array)", .nameEN="Rows"},
    {"Occurrence",      NULL,                "entier",  "0",    "nombre de lignes retournées", "number of rows returned", .nameEN="Count"},
    {"LignesAffectees", "LignesAffectées",   "entier",  "0",    "nombre de lignes affectées (INSERT/UPDATE/DELETE)", "number of rows affected (INSERT/UPDATE/DELETE)", .nameEN="AffectedRows"},
    {0},
};
static const char* const ALIAS_sqlConnexion[] = { "sqlConnection", NULL };
static const char* const ALIAS_sqlRequete[]   = { "sqlRequête", "sqlQuery", NULL };
static const char* const ALIAS_sqlReponse[]   = { "sqlRéponse", "sqlResponse", NULL };
#endif /* OWL_HAS_SQL */

/* ftpClient — client FTP en clair. Ungated (couche net_tcp dans le core,
 * comme HTTP) : présent dans owl-cli aussi. */
static const OwlField FIELDS_ftpClient[] = {
    {"Serveur",     NULL,          "chaîne",  "\"\"",           "nom d'hôte ou IP du serveur FTP", "FTP server host name or IP", .nameEN="Server"},
    {"Utilisateur", NULL,          "chaîne",  "\"\"",           "nom d'utilisateur (vide = anonyme)", "user name (empty = anonymous)", .nameEN="User"},
    {"MotDePasse",  NULL,          "chaîne",  "\"\"",           "mot de passe", "password", .nameEN="Password"},
    {"Port",        NULL,          "entier",  "21",             "port du canal de contrôle", "control channel port", .nameEN="Port"},
    {"Mode",        NULL,          "entier",  "ftpModePassif",  "mode du canal de données (constante ftpMode*)", "data channel mode (ftpMode* constant)",
     .allowedBase=&E_ftpMode, .nameEN="Mode"},
    {"Connectee",   "Connectée",   "booléen", "Faux",           "vrai quand la connexion est ouverte", "true while the connection is open", .nameEN="Connected"},
    {"Securisee",   "Sécurisée",   "booléen", "Faux",           "vrai quand le canal de contrôle est chiffré (FTPS/FTPES)", "true when the control channel is encrypted (FTPS/FTPES)", .nameEN="Secured"},
    {"Chiffrement", NULL,          "chaîne",  "\"\"",           "chiffrement négocié : \"FTPS\", \"FTPES\", \"SFTP\" ou \"\" (clair)", "negotiated encryption: \"FTPS\", \"FTPES\", \"SFTP\" or \"\" (clear)", .nameEN="Encryption"},
    {"VerifieHote", "VérifieHôte", "booléen", "Faux",           "vrai quand la clé d'hôte SSH d'une connexion SFTP a été vérifiée contre known_hosts", "true when the SSH host key of an SFTP connection was verified against known_hosts", .nameEN="VerifyHost"},
    {0},
};

/* Thème — conteneur ouvert de variables sémantiques (D.4 enabler Lot D).
 * Diffère des autres types composés : pas de sous-champs prédéfinis. L'user
 * declare ses propres variables (Primaire, Surface, Espacement, ...) via
 * un bloc PROPRIETES qui justifie ce mecanisme dans ce cas precis (le
 * Thème agrège du contenu mixte : couleurs, typographies, espacements).
 *
 * V1 : juste les variables semantiques. Les Styles qui les referencent
 * vivent au top-level a cote :
 *
 *   THEME_Corp est un Thème
 *       PROPRIETES
 *           Primaire    = RVB(33, 100, 200)
 *           Surface     = Blanc
 *           Espacement  = 8
 *       FIN
 *   FIN
 *
 *   STY_BtnPrim est un Style
 *   STY_BtnPrim..CouleurFond = THEME_Corp..Primaire
 *
 * V2+ ajoutera l'imbrication directe des Styles dans le Theme (section
 * STYLES) et la resolution implicite des references (Surface au lieu de
 * THEME_Corp..Surface). */

/* Style — descripteur d'apparence aggregé (D.2). 8 sous-proprietes V1
 * dont 2 composees (Police + Cadre). */
static const OwlField FIELDS_Style[] = {
    {"Couleur",       NULL, "entier", "0",
     "Couleur du texte (RVB packé BGR ; 0 = noir).",
     "Text color (RGB packed BGR ; 0 = black).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {"CouleurFond",   NULL, "entier", "-1",
     "Couleur de fond (RVB packé BGR ; -1 = pas de fond).",
     "Background color (RGB packed BGR ; -1 = no fill).",
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"},
    {"RetraitGauche", NULL, "entier", "0",
     "Marge interieure gauche en pixels.",
     "Left inner padding in pixels.",
     .nameEN="LeftPadding"},
    {"RetraitDroit",  NULL, "entier", "0",
     "Marge interieure droite en pixels.",
     "Right inner padding in pixels.",
     .nameEN="RightPadding"},
    {"RetraitHaut",   NULL, "entier", "0",
     "Marge interieure haute en pixels.",
     "Top inner padding in pixels.",
     .nameEN="TopPadding"},
    {"RetraitBas",    NULL, "entier", "0",
     "Marge interieure basse en pixels.",
     "Bottom inner padding in pixels.",
     .nameEN="BottomPadding"},
    {"CurseurSouris", NULL, "entier", "-1",
     "Forme du curseur de survol (curFleche, curMain, curSablier, ...). -1 = non defini.",
     "Hover cursor shape (curFleche, curMain, curSablier, ...). -1 = unset.",
     .allowedBase=&E_Curseurs, .nameEN="MouseCursor"},
    {"Police",        NULL, "Police", "Police par defaut",
     "Descripteur de police imbrique (Nom/Taille/Gras/Italique/...).",
     "Embedded font descriptor (Name/Size/Bold/Italic/...).",
     .nameEN="Font"},
    {"Cadre",         NULL, "Cadre",  "Cadre par defaut",
     "Descripteur de cadre imbrique (Couleur/Epaisseur/Trait/Arrondi/Bords).",
     "Embedded border descriptor (Color/Thickness/LineStyle/Radius/Edges).",
     .nameEN="Border"},
    {"Survol",        NULL, "Style",  "vide",
     "Sous-etat applique au passage souris. Conteneur ouvert : l'user override ce qu'il veut (typiquement Couleur, CouleurFond, CurseurSouris).",
     "Hover sub-state. Open container : user overrides what they want (typically Couleur, CouleurFond, CurseurSouris).",
     .nameEN="Hover"},
    {0},
};

/* Brique de style commune : les props de base partagees par tous
 * les sous-elements de style. Facteur commun de FIELDS_styleZoneSaisieBase /
 * ZoneSaisie / Libellé / CadreExtérieur — garantit un contrat identique. */
#define STYLE_BRICK_FIELDS                                                   \
    {"Couleur",       NULL, "entier", "0",                                   \
     "Couleur du texte (RVB packé BGR ; 0 = noir).",                        \
     "Text color (RGB packed BGR ; 0 = black).",                            \
     .allowedBase=&E_Couleur, .nameEN="Color"},                             \
    {"CouleurFond",   NULL, "entier", "-1",                                  \
     "Couleur de fond (RVB packé BGR ; -1 = pas de fond).",                 \
     "Background color (RGB packed BGR ; -1 = no fill).",                    \
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"},                   \
    {"RetraitGauche", NULL, "entier", "0",                                   \
     "Marge interieure gauche en pixels.",                                   \
     "Left inner padding in pixels.",                                        \
     .nameEN="LeftPadding"},                                                 \
    {"RetraitDroit",  NULL, "entier", "0",                                   \
     "Marge interieure droite en pixels.",                                   \
     "Right inner padding in pixels.",                                       \
     .nameEN="RightPadding"},                                               \
    {"RetraitHaut",   NULL, "entier", "0",                                   \
     "Marge interieure haute en pixels.",                                    \
     "Top inner padding in pixels.",                                         \
     .nameEN="TopPadding"},                                                 \
    {"RetraitBas",    NULL, "entier", "0",                                   \
     "Marge interieure basse en pixels.",                                    \
     "Bottom inner padding in pixels.",                                      \
     .nameEN="BottomPadding"},                                              \
    {"Police",        NULL, "Police", "Police par defaut",                   \
     "Descripteur de police imbrique (Nom/Taille/Gras/Italique/...).",       \
     "Embedded font descriptor (Name/Size/Bold/Italic/...).",               \
     .nameEN="Font"},                                                        \
    {"Cadre",         NULL, "Cadre",  "Cadre par defaut",                    \
     "Descripteur de cadre imbrique (Couleur/Epaisseur/Trait/Arrondi/Bords).", \
     "Embedded border descriptor (Color/Thickness/LineStyle/Radius/Edges).", \
     .nameEN="Border"}

/* styleZoneSaisieBase — type PLAT de la surcharge par etat.
 * La brique seule : PAS de membre Etats (fin de chaine, recursion impossible). */
static const OwlField FIELDS_styleZoneSaisieBase[] = {
    STYLE_BRICK_FIELDS,
    {0},
};

/* styleZoneSaisie — sous-element zone de saisie : la brique + le
 * membre Etats (collection de surcharges par etat, indexee via [etat]). */
static const OwlField FIELDS_styleZoneSaisie[] = {
    STYLE_BRICK_FIELDS,
    {"Ombre",         NULL, "styleOmbre", "{}",
     "Ombre portée de la zone de saisie (Couleur/DécalageX/DécalageY/Flou/Étendue).",
     "Drop shadow of the input zone (Color/OffsetX/OffsetY/Blur/Spread).",
     .nameEN="Shadow"},
    {"Degrade",       "D\xc3\xa9grad\xc3\xa9", "styleDegrade", "{}",
     "Dégradé de fond de la zone de saisie (Type/Angle/CouleurDébut/CouleurFin ; prime sur CouleurFond).",
     "Background gradient of the input zone (Type/Angle/StartColor/EndColor; overrides BackgroundColor).",
     .nameEN="Gradient"},
    {"Etats", NULL, "styleZoneSaisieEtats", "{}",
     "Surcharges par etat de la zone (survol/focus/grisé/lectureSeule). "
     "Chaque etat est un style plat : ..Etats[survol]..CouleurFond = ...",
     "Per-state overrides of the zone (hover/focus/disabled/read-only). "
     "Each state is a flat style: ..Etats[hover]..CouleurFond = ...",
     .nameEN="States"},
    {0},
};

/* styleLibelléSaisie — brique du sous-element libellé integre du Saisie
 * (, renomme ). Statique en V1. */
static const OwlField FIELDS_styleLibelleSaisie[] = {
    STYLE_BRICK_FIELDS,
    {0},
};

/* styleOmbre — sous-objet ombre portée d'un styleCadreExtérieur.
 * Couleur de l'ombre (ou du glow), décalages, rayon de flou (0 = désactivée),
 * étendue (approximée via le flou au rendu ; conservée telle quelle au modèle).
 * Glow = ombre colorée, décalages 0, gros flou. */
static const OwlField FIELDS_styleOmbre[] = {
    {"Couleur",   NULL, "entier", "0",
     "Couleur de l'ombre (RVB packé BGR ; 0 = noir).",
     "Shadow color (RGB packed BGR ; 0 = black).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {"DecalageX", "D\xc3\xa9" "calageX", "entier", "0",
     "Décalage horizontal de l'ombre en pixels.",
     "Horizontal shadow offset in pixels.",
     .nameEN="OffsetX"},
    {"DecalageY", "D\xc3\xa9" "calageY", "entier", "0",
     "Décalage vertical de l'ombre en pixels.",
     "Vertical shadow offset in pixels.",
     .nameEN="OffsetY"},
    {"Flou",      NULL, "entier", "0",
     "Rayon de flou de l'ombre en pixels (0 = pas d'ombre).",
     "Shadow blur radius in pixels (0 = no shadow).",
     .nameEN="Blur"},
    {"Etendue",   "\xc3\x89tendue", "entier", "0",
     "Épaississement de l'ombre en pixels (approximé via le flou au rendu).",
     "Shadow spread in pixels (approximated via blur at render time).",
     .nameEN="Spread"},
    {0},
};

/* styleDégradé — sous-objet dégradé de fond d'un styleCadreExtérieur.
 * Type (enum typeDégradé : aucun/linéaire/radial ; défaut dégradéAucun = pas de
 * dégradé), Angle (0 = horizontal G->D, 90 = vertical H->B ; ignoré en radial),
 * CouleurDébut/CouleurFin (arrêts). Le dégradé actif (linéaire/radial) prime sur
 * CouleurFond. */
static const OwlField FIELDS_styleDegrade[] = {
    {"Type",         NULL, "entier", "d\xc3\xa9grad\xc3\xa9""Aucun",
     "Type de dégradé (dégradéAucun/dégradéLinéaire/dégradéRadial).",
     "Gradient type (gradientNone/gradientLinear/gradientRadial).",
     .allowedBase=&E_TypeDegrade, .nameEN="Type"},
    {"Angle",        NULL, "entier", "0",
     "Orientation du dégradé linéaire (0 = horizontal, 90 = vertical ; ignoré en radial).",
     "Linear gradient orientation (0 = horizontal, 90 = vertical; ignored when radial).",
     .nameEN="Angle"},
    {"CouleurDebut", "CouleurD\xc3\xa9""but", "entier", "-1",
     "Couleur du premier arrêt du dégradé (RVB packé BGR ; -1 = non défini, transparent).",
     "First gradient stop color (RGB packed BGR ; -1 = undefined, transparent).",
     .allowedBase=&E_Couleur, .nameEN="StartColor"},
    {"CouleurFin",   NULL, "entier", "-1",
     "Couleur du dernier arrêt du dégradé (RVB packé BGR ; -1 = non défini, transparent).",
     "Last gradient stop color (RGB packed BGR ; -1 = undefined, transparent).",
     .allowedBase=&E_Couleur, .nameEN="EndColor"},
    {0},
};

/* Brique commune du CadreExtérieur : les feuilles plates partagees
 * par FIELDS_styleCadreExterieur et FIELDS_styleCadreExterieurBase. Un cadre
 * n'a pas de texte : PAS de Couleur (texte) ni Police. Diverge donc de la
 * brique commune (STYLE_BRICK_FIELDS) volontairement. */
#define CADRE_BRICK_FIELDS                                                   \
    {"CouleurFond",   NULL, "entier", "-1",                                  \
     "Couleur de fond (RVB packé BGR ; -1 = pas de fond).",                 \
     "Background color (RGB packed BGR ; -1 = no fill).",                    \
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"},                   \
    {"RetraitGauche", NULL, "entier", "0",                                   \
     "Marge interieure gauche en pixels.",                                   \
     "Left inner padding in pixels.",                                        \
     .nameEN="LeftPadding"},                                                 \
    {"RetraitDroit",  NULL, "entier", "0",                                   \
     "Marge interieure droite en pixels.",                                   \
     "Right inner padding in pixels.",                                       \
     .nameEN="RightPadding"},                                               \
    {"RetraitHaut",   NULL, "entier", "0",                                   \
     "Marge interieure haute en pixels.",                                    \
     "Top inner padding in pixels.",                                         \
     .nameEN="TopPadding"},                                                 \
    {"RetraitBas",    NULL, "entier", "0",                                   \
     "Marge interieure basse en pixels.",                                    \
     "Bottom inner padding in pixels.",                                      \
     .nameEN="BottomPadding"},                                              \
    {"Cadre",         NULL, "Cadre",  "Cadre par defaut",                    \
     "Descripteur de cadre imbrique (Couleur/Epaisseur/Trait/Arrondi/Bords).", \
     "Embedded border descriptor (Color/Thickness/LineStyle/Radius/Edges).", \
     .nameEN="Border"},                                                      \
    {"Ombre",         NULL, "styleOmbre", "{}",                              \
     "Ombre portée du champ entier (Couleur/DécalageX/DécalageY/Flou/Étendue).", \
     "Drop shadow of the whole control (Color/OffsetX/OffsetY/Blur/Spread).", \
     .nameEN="Shadow"},                                                      \
    {"Degrade",       "D\xc3\xa9grad\xc3\xa9", "styleDegrade", "{}",         \
     "Dégradé de fond du champ (Type/Angle/CouleurDébut/CouleurFin ; prime sur CouleurFond).", \
     "Background gradient of the control (Type/Angle/StartColor/EndColor; overrides BackgroundColor).", \
     .nameEN="Gradient"}

/* styleCadreExterieurBase — type PLAT de la surcharge par etat du cadre
 *. La brique seule : PAS de membre Etats (fin de chaine,
 * recursion impossible). */
static const OwlField FIELDS_styleCadreExterieurBase[] = {
    CADRE_BRICK_FIELDS,
    {0},
};

/* styleCadreExtérieur — sous-element conteneur (, allege ,
 * ombre , dégradé , etats ). Un cadre n'a pas de
 * texte : PAS de Couleur (texte) ni Police. Feuilles pertinentes seules :
 * CouleurFond + Retraits + Cadre + Ombre + Dégradé + le membre Etats
 * (surcharges par etat, indexe via [etat]). */
static const OwlField FIELDS_styleCadreExterieur[] = {
    CADRE_BRICK_FIELDS,
    {"Etats", NULL, "styleCadreExterieurEtats", "{}",
     "Surcharges par etat du cadre (survol/appuyé/focus/grisé). "
     "Chaque etat est un style plat : ..Etats[survol]..CouleurFond = ...",
     "Per-state overrides of the frame (hover/pressed/focus/disabled). "
     "Each state is a flat style: ..Etats[hover]..CouleurFond = ...",
     .nameEN="States"},
    {0},
};

/* styleLibellé — modele de style typé du Champ Libellé. Struct de
 * sous-elements : Libellé (brique texte, styleLibelléSaisie) + CadreExtérieur
 * (conteneur, styleCadreExtérieur). Pas de props plates a la racine. */
static const OwlField FIELDS_styleLibelle[] = {
    {"Libelle",        "Libell\xc3\xa9",  "styleLibelleSaisie", "{}",
     "Sous-element texte du libellé (statique).",
     "Label text sub-element (static).",
     .nameEN="Label"},
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element conteneur qui enveloppe le libellé (surcharges par état via Etats).",
     "Container sub-element wrapping the label (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {0},
};

/* styleImage — modele de style typé du Champ Image. Un seul
 * sous-element : CadreExtérieur (la boite : CouleurFond + Cadre + Retraits).
 * La source de l'image et le mode d'echelle sont des props top-level du champ
 * (Image, Cadrage), pas du Style. Statique (aucun etat). */
static const OwlField FIELDS_styleImage[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-\xc3\xa9l\xc3\xa9ment conteneur qui enveloppe l'image (surcharges par état via Etats).",
     "Container sub-element wrapping the image (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {0},
};

/* styleCodeBarre — modele de style typé du Champ CodeBarres. Un
 * seul sous-element : CadreExtérieur (la boite : CouleurFond + Cadre + Retraits).
 * Dissocie de styleImage : le style Image se complexifiera (libellé), celui du
 * code-barres reste la boite. Statique (aucun etat). */
static const OwlField FIELDS_styleCodeBarre[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-\xc3\xa9l\xc3\xa9ment conteneur qui enveloppe le code-barres (surcharges par état via Etats).",
     "Container sub-element wrapping the barcode (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {0},
};

/* styleConteneur — modele de style typé du Champ Conteneur. Un
 * seul sous-element : CadreExtérieur (la boite : CouleurFond + Cadre +
 * Retraits). Aucun texte (groupeur passif), aucun etat. Statique. */
static const OwlField FIELDS_styleConteneur[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-\xc3\xa9l\xc3\xa9ment conteneur qui enveloppe le groupe (statique).",
     "Container sub-element wrapping the group (static).",
     .nameEN="OuterFrame"},
    {0},
};

/* styleCoche — sous-element marque de coche de l'Interrupteur.
 * Une seule feuille : Couleur (couleur de la marque cochee ; -1 = non posee). */
static const OwlField FIELDS_styleCoche[] = {
    {"Couleur", NULL, "entier", "-1",
     "Couleur de la marque de coche (RVB packé BGR ; -1 = defaut).",
     "Check mark color (RGB packed BGR ; -1 = default).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {0},
};

/* styleInterrupteur — modele de style typé du Champ Interrupteur.
 * Struct de sous-elements : CadreExtérieur (le conteneur), TexteOptions (le
 * texte des cases, brique texte), Coche (la marque cochee) et CadreCoche (la
 * boite de la coche). Pas de props plates a la racine. */
static const OwlField FIELDS_styleInterrupteur[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element conteneur qui enveloppe les cases (surcharges par état via Etats).",
     "Container sub-element wrapping the checkboxes (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {"TexteOptions", NULL, "styleLibelleSaisie", "{}",
     "Sous-element texte des cases (Couleur/Police, statique).",
     "Option text sub-element (Color/Font, static).",
     .nameEN="OptionText"},
    {"Coche", NULL, "styleCoche", "{}",
     "Sous-element marque de coche (Couleur, statique).",
     "Check mark sub-element (Color, static).",
     .nameEN="CheckMark"},
    {"CadreCoche", NULL, "styleCadreExterieur", "{}",
     "Sous-element boite de la coche (CouleurFond + Cadre, statique).",
     "Check box sub-element (BackgroundColor + Border, static).",
     .nameEN="CheckBox"},
    {0},
};

/* stylePuce — sous-element rond du bouton radio (pendant de
 * styleCoche pour le Sélecteur). Une seule feuille : Couleur (couleur du rond
 * selectionne ; -1 = non posee). */
static const OwlField FIELDS_stylePuce[] = {
    {"Couleur", NULL, "entier", "-1",
     "Couleur de la puce du bouton radio (RVB packé BGR ; -1 = defaut).",
     "Radio bullet color (RGB packed BGR ; -1 = default).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {0},
};

/* styleSélecteur — modele de style typé du Champ Sélecteur (boutons
 * radio exclusifs). Pendant du styleInterrupteur, adapte radio : CadreExtérieur
 * (le conteneur des N options, styleCadreExtérieur reutilise), TexteOptions (le
 * texte de chaque option, brique texte), Puce (le rond selectionne, stylePuce)
 * et CadrePuce (la boite du rond, styleCadreExtérieur : CouleurFond + Cadre en
 * QSS ::indicator). Pas de props plates a la racine. */
static const OwlField FIELDS_styleSelecteur[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element conteneur qui enveloppe les options (surcharges par état via Etats).",
     "Container sub-element wrapping the options (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {"TexteOptions", NULL, "styleLibelleSaisie", "{}",
     "Sous-element texte des options (Couleur/Police, statique).",
     "Option text sub-element (Color/Font, static).",
     .nameEN="OptionText"},
    {"Puce", NULL, "stylePuce", "{}",
     "Sous-element rond du bouton radio (Couleur, statique).",
     "Radio bullet sub-element (Color, static).",
     .nameEN="Bullet"},
    {"CadrePuce", NULL, "styleCadreExterieur", "{}",
     "Sous-element boite du rond (CouleurFond + Cadre, statique).",
     "Bullet box sub-element (BackgroundColor + Border, static).",
     .nameEN="BulletBox"},
    {0},
};

/* styleListe — modele de style typé du Champ Liste (list box).
 * Trois sous-elements : CadreExtérieur (le cadre + fond de la liste,
 * styleCadreExtérieur reutilise), LibelléLignes (le texte des items, brique
 * texte styleLibelléSaisie) et LigneSélectionnée (le texte de l'item
 * selectionne, brique texte). Pas de props plates a la racine. */
static const OwlField FIELDS_styleListe[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element cadre et fond de la liste (surcharges par état via Etats).",
     "Container sub-element: list frame and background (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {"LibelleLignes", "Libell\xc3\xa9Lignes", "styleLibelleSaisie", "{}",
     "Sous-element texte des lignes (Couleur/Police, statique).",
     "Row text sub-element (Color/Font, static).",
     .nameEN="RowLabel"},
    {"LigneSelectionnee", "LigneS\xc3\xa9lectionn\xc3\xa9""e", "styleLibelleSaisie", "{}",
     "Sous-element texte de la ligne selectionnee (Couleur/Police, statique).",
     "Selected row text sub-element (Color/Font, static).",
     .nameEN="SelectedRow"},
    {0},
};

/* styleCombo — modele de style typé du Champ Combo (liste
 * deroulante). Cinq sous-elements : Libellé (le libellé integre, brique texte),
 * CadreExtérieur (le conteneur englobant), ZoneSaisie (le champ combo, brique
 * stateful), LibelléLignes (le texte des items du popup, brique texte) et
 * LigneSélectionnée (l'item surligné, brique texte). Pas de props plates a la
 * racine. */
static const OwlField FIELDS_styleCombo[] = {
    {"Libelle",        "Libell\xc3\xa9",  "styleLibelleSaisie", "{}",
     "Sous-element libellé integre du combo (Couleur/Police, statique).",
     "Embedded combo label sub-element (Color/Font, static).",
     .nameEN="Label"},
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element conteneur qui enveloppe le libellé et le champ combo (surcharges par état via Etats).",
     "Container sub-element wrapping the label and the combo field (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {"ZoneSaisie",     NULL,             "styleZoneSaisie", "{}",
     "Sous-element champ combo (zone editable/affichee). Porte les etats visuels "
     "via ..Etats[survol/focus/grisé/lectureSeule].",
     "Combo field sub-element (editable/displayed zone). Carries visual states "
     "via ..Etats[hover/focus/disabled/read-only].",
     .nameEN="InputZone"},
    {"LibelleLignes", "Libell\xc3\xa9Lignes", "styleLibelleSaisie", "{}",
     "Sous-element texte des items du popup (Couleur/Police, statique).",
     "Popup item text sub-element (Color/Font, static).",
     .nameEN="RowLabel"},
    {"LigneSelectionnee", "LigneS\xc3\xa9lectionn\xc3\xa9""e", "styleLibelleSaisie", "{}",
     "Sous-element texte de l'item selectionne du popup (Couleur/Police, statique).",
     "Selected popup item text sub-element (Color/Font, static).",
     .nameEN="SelectedRow"},
    {0},
};

/* styleQuadrillage — sous-element des separateurs (grille) du
 * Champ Table. Une seule feuille : Couleur (couleur des lignes de separation ;
 * -1 = defaut du theme). */
static const OwlField FIELDS_styleQuadrillage[] = {
    {"Couleur", NULL, "entier", "-1",
     "Couleur des separateurs du quadrillage (RVB packé BGR ; -1 = defaut).",
     "Gridline separator color (RGB packed BGR ; -1 = default).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {0},
};

/* styleTable — modele de style typé du Champ Table (tableau).
 * Huit sous-elements : CadreExtérieur (le cadre + fond), TitreColonne (le texte
 * des en-tetes), LibelléLignes (le texte des cellules), LignesPaires /
 * LignesImpaires (le fond alterne), LigneSélectionnée / LigneSurvolée (le texte
 * + fond des lignes sous interaction) et Quadrillage (la couleur des
 * separateurs). Pas de props plates a la racine. */
static const OwlField FIELDS_styleTable[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element cadre et fond du tableau (surcharges par état via Etats).",
     "Container sub-element: table frame and background (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {"TitreColonne", NULL, "styleLibelleSaisie", "{}",
     "Sous-element texte des en-tetes de colonne (Couleur/Police, statique).",
     "Column header text sub-element (Color/Font, static).",
     .nameEN="ColumnTitle"},
    {"LibelleLignes", "Libell\xc3\xa9Lignes", "styleLibelleSaisie", "{}",
     "Sous-element texte des cellules de ligne (Couleur/Police, statique).",
     "Row cell text sub-element (Color/Font, static).",
     .nameEN="RowLabel"},
    {"LignesPaires", NULL, "styleLibelleSaisie", "{}",
     "Sous-element fond des lignes de rang pair (CouleurFond, statique).",
     "Even-rank row background sub-element (BackgroundColor, static).",
     .nameEN="EvenRows"},
    {"LignesImpaires", NULL, "styleLibelleSaisie", "{}",
     "Sous-element fond des lignes de rang impair (CouleurFond, statique).",
     "Odd-rank row background sub-element (BackgroundColor, static).",
     .nameEN="OddRows"},
    {"LigneSelectionnee", "LigneS\xc3\xa9lectionn\xc3\xa9""e", "styleLibelleSaisie", "{}",
     "Sous-element texte + fond de la ligne selectionnee (Couleur/CouleurFond/Police, statique).",
     "Selected row text + background sub-element (Color/BackgroundColor/Font, static).",
     .nameEN="SelectedRow"},
    {"LigneSurvolee", "LigneSurvol\xc3\xa9""e", "styleLibelleSaisie", "{}",
     "Sous-element texte + fond de la ligne survolee (Couleur/CouleurFond, statique).",
     "Hovered row text + background sub-element (Color/BackgroundColor, static).",
     .nameEN="HoveredRow"},
    {"Quadrillage", NULL, "styleQuadrillage", "{}",
     "Sous-element couleur des separateurs du quadrillage (statique).",
     "Gridline separator color sub-element (static).",
     .nameEN="Gridlines"},
    {0},
};

/* styleJauge — modele de style typé du Champ Jauge. Trois
 * sous-elements : CadreExtérieur (piste + bordure + fond, avec Etats),
 * Remplissage (barre de progression : CouleurFond) et Libellé (texte du
 * pourcentage : Couleur/Police). */
static const OwlField FIELDS_styleJauge[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element piste, bordure et fond de la jauge (surcharges par état via Etats).",
     "Track, border and background sub-element of the gauge (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {"Remplissage", NULL, "styleLibelleSaisie", "{}",
     "Sous-element barre de progression : la portion remplie (CouleurFond).",
     "Progress bar sub-element: the filled portion (BackgroundColor).",
     .nameEN="Fill"},
    {"Libelle", "Libell\xc3\xa9", "styleLibelleSaisie", "{}",
     "Sous-element texte du pourcentage affiche (Couleur/Police, statique).",
     "Displayed percentage text sub-element (Color/Font, static).",
     .nameEN="Label"},
    {0},
};

/* styleColonne — modele de style typé d'une Colonne : surcharge
 * locale du styleTable. Sous-ensemble de styleTable SANS CadreExtérieur ni
 * Quadrillage (propres au tableau entier). Une feuille non renseignee sur la
 * colonne retombe sur le styleTable. */
static const OwlField FIELDS_styleColonne[] = {
    {"TitreColonne", NULL, "styleLibelleSaisie", "{}",
     "Surcharge du texte de l'en-tete de cette colonne (Couleur/Police, statique).",
     "Overrides this column header text (Color/Font, static).",
     .nameEN="ColumnTitle"},
    {"LibelleLignes", "Libell\xc3\xa9Lignes", "styleLibelleSaisie", "{}",
     "Surcharge du texte des cellules de cette colonne (Couleur/Police, statique).",
     "Overrides this column cell text (Color/Font, static).",
     .nameEN="RowLabel"},
    {"LignesPaires", NULL, "styleLibelleSaisie", "{}",
     "Surcharge du fond des lignes paires de cette colonne (CouleurFond, statique).",
     "Overrides this column even-row background (BackgroundColor, static).",
     .nameEN="EvenRows"},
    {"LignesImpaires", NULL, "styleLibelleSaisie", "{}",
     "Surcharge du fond des lignes impaires de cette colonne (CouleurFond, statique).",
     "Overrides this column odd-row background (BackgroundColor, static).",
     .nameEN="OddRows"},
    {"LigneSelectionnee", "LigneS\xc3\xa9lectionn\xc3\xa9""e", "styleLibelleSaisie", "{}",
     "Surcharge du texte + fond de la ligne selectionnee sur cette colonne (statique).",
     "Overrides selected-row text + background on this column (static).",
     .nameEN="SelectedRow"},
    {"LigneSurvolee", "LigneSurvol\xc3\xa9""e", "styleLibelleSaisie", "{}",
     "Surcharge du texte + fond de la ligne survolee sur cette colonne (statique).",
     "Overrides hovered-row text + background on this column (static).",
     .nameEN="HoveredRow"},
    {0},
};

/* styleZoneRépétée — modele de style typé du Champ Zone répétée.
 * Sept sous-elements. Defauts sobres : fonds transparents, cadres invisibles,
 * pas de bleu de selection (≠ styleTable). CadreExtérieur = le widget entier,
 * CadreInterne = chaque repetition (distinct du Séparateur). */
static const OwlField FIELDS_styleZoneRepetee[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element cadre et fond du champ entier (surcharges par état via Etats).",
     "Container sub-element: whole control frame and background (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {"CadreInterne", NULL, "styleCadreExterieur", "{}",
     "Sous-element cadre de chaque repetition (distinct du separateur, statique).",
     "Per-repetition frame sub-element (distinct from the separator, static).",
     .nameEN="InnerFrame"},
    {"LignesImpaires", NULL, "styleLibelleSaisie", "{}",
     "Sous-element fond des repetitions de rang impair (CouleurFond, statique).",
     "Odd-rank repetition background sub-element (BackgroundColor, static).",
     .nameEN="OddRows"},
    {"LignesPaires", NULL, "styleLibelleSaisie", "{}",
     "Sous-element fond des repetitions de rang pair (CouleurFond, statique).",
     "Even-rank repetition background sub-element (BackgroundColor, static).",
     .nameEN="EvenRows"},
    {"LigneSelectionnee", "LigneS\xc3\xa9lectionn\xc3\xa9""e", "styleLibelleSaisie", "{}",
     "Sous-element texte + fond de la repetition selectionnee (statique).",
     "Selected repetition text + background sub-element (static).",
     .nameEN="SelectedRow"},
    {"LigneSurvolee", "LigneSurvol\xc3\xa9""e", "styleLibelleSaisie", "{}",
     "Sous-element texte + fond de la repetition survolee (statique).",
     "Hovered repetition text + background sub-element (static).",
     .nameEN="HoveredRow"},
    {"Separateur", "S\xc3\xa9parateur", "styleQuadrillage", "{}",
     "Sous-element couleur du trait de separation entre repetitions (statique).",
     "Separator line color sub-element between repetitions (static).",
     .nameEN="Separator"},
    {0},
};

/* styleZoneBoutonBase — type PLAT de la surcharge par etat du bouton
 *. La brique seule : PAS de membre Etats (fin de chaine). */
static const OwlField FIELDS_styleZoneBoutonBase[] = {
    STYLE_BRICK_FIELDS,
    {0},
};

/* styleZoneBouton — sous-element bouton : la brique + le membre
 * Etats (collection de surcharges par etat, indexee via [etat]). Palette du
 * bouton : survol/appuyé/focus/grisé. */
static const OwlField FIELDS_styleZoneBouton[] = {
    STYLE_BRICK_FIELDS,
    {"Etats", NULL, "styleZoneBoutonEtats", "{}",
     "Surcharges par etat du bouton (survol/appuyé/focus/grisé). "
     "Chaque etat est un style plat : ..Etats[appuyé]..CouleurFond = ...",
     "Per-state overrides of the button (hover/pressed/focus/disabled). "
     "Each state is a flat style: ..Etats[pressed]..CouleurFond = ...",
     .nameEN="States"},
    {0},
};

/* styleImageBoutonBase — type PLAT de la surcharge d'icône par état
 *. Une seule feuille : Image (source de l'icône substituée dans
 * l'état). Pas de membre Etats (fin de chaîne). */
static const OwlField FIELDS_styleImageBoutonBase[] = {
    {"Image", NULL, "Image", "\"\"",
     "Icône substituée dans l'état (Image, chemin de fichier, ou Icône du catalogue).",
     "Icon substituted in the state (Image, file path, or catalog Icon).",
     .nameEN="Image"},
    {0},
};

/* styleImageBouton — sous-element d'un styleBouton decrivant l'icone
 * ( lot B, recadre , , etats ). Feuilles :
 * Image (source de l'icone, VARIANT Image|chaine|Icone, unifiee avec la prop
 * plate BTN..Image), Position (entier contraint a positionImageBouton) et Etats
 * (surcharges d'icone par etat : survol/grisé uniquement). */
static const OwlField FIELDS_styleImageBouton[] = {
    {"Image", NULL, "Image", "\"\"",
     "Icône du bouton (Image, chemin de fichier, ou Icône du catalogue). "
     "Equivalente au raccourci plat ..Image du bouton.",
     "Button icon (Image, file path, or catalog Icon). "
     "Equivalent to the button flat shortcut ..Image.",
     .nameEN="Image"},
    {"Position", NULL, "entier", "posBtnImageGauche",
     "Position de l'icône par rapport au texte (posBtnImageGauche/Droite/Haut/Bas, "
     "posBtnImageSeule, posBtnTexteSeul).",
     "Icon position relative to the text (posBtnImageGauche/Droite/Haut/Bas, "
     "posBtnImageSeule, posBtnTexteSeul).",
     .allowedBase=&E_PositionImageBouton, .nameEN="Position"},
    {"Etats", NULL, "styleImageBoutonEtats", "{}",
     "Surcharges d'icône par état : ..Etats[survol]..Image et "
     "..Etats[grisé]..Image (seuls états offerts pour l'icône).",
     "Per-state icon overrides: ..Etats[hover]..Image and "
     "..Etats[disabled]..Image (only states offered for the icon).",
     .nameEN="States"},
    {0},
};

/* styleBouton — modele de style typé du Champ Bouton (, ).
 * Struct de sous-elements : Libellé (le contenu texte stylable, brique
 * interactive stateful, styleZoneBouton) + Image (placement de l'icone :
 * Position, styleImageBouton) + CadreExtérieur (conteneur, styleCadreExtérieur).
 * Pas de props plates a la racine. */
static const OwlField FIELDS_styleBouton[] = {
    {"Libelle", "Libell\xc3\xa9", "styleZoneBouton", "{}",
     "Sous-element du texte du bouton (libellé). "
     "Porte les etats visuels via ..Etats[survol/appuyé/focus/grisé].",
     "Button text (caption) sub-element. "
     "Carries the visual states via ..Etats[hover/pressed/focus/disabled].",
     .nameEN="Caption"},
    {"Image", NULL, "styleImageBouton", "{}",
     "Sous-element de placement de l'icône du bouton : Position (par rapport au texte).",
     "Button icon placement sub-element: Position (relative to the text).",
     .nameEN="Image"},
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element conteneur qui enveloppe le bouton (surcharges par état via Etats).",
     "Container sub-element wrapping the button (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {0},
};

/*  (ticket C) — styleMenuPrincipal : brique texte MINIMALE des
 * surfaces stateful du menu (Barre + OptionMenu). Couleur + CouleurFond +
 * Police, PAS de Retraits ni de Cadre (un menu classique ne les stylise pas).
 * Sentinelles -1 = « non pose » -> repli sur le defaut du theme au rendu. */
#define MENU_TEXT_BRICK_FIELDS                                                \
    {"Couleur",     NULL, "entier", "-1",                                     \
     "Couleur du texte (RVB packé BGR ; -1 = défaut du thème).",             \
     "Text color (RGB packed BGR ; -1 = theme default).",                     \
     .allowedBase=&E_Couleur, .nameEN="Color"},                              \
    {"CouleurFond", NULL, "entier", "-1",                                     \
     "Couleur de fond (RVB packé BGR ; -1 = défaut du thème).",             \
     "Background color (RGB packed BGR ; -1 = theme default).",               \
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"},                    \
    {"Police",      NULL, "Police", "Police par defaut",                      \
     "Descripteur de police imbrique (Nom/Taille/Gras/Italique/...).",        \
     "Embedded font descriptor (Name/Size/Bold/Italic/...).",                \
     .nameEN="Font"}

/*  — surcharge par etat : couleurs seules, sans Police (Qt ne rend pas
 * de police par etat sur un menu -> on ne l'expose pas). */
#define MENU_TEXT_BRICK_COLORS                                                \
    {"Couleur",     NULL, "entier", "-1",                                     \
     "Couleur du texte (RVB packé BGR ; -1 = défaut du thème).",             \
     "Text color (RGB packed BGR ; -1 = theme default).",                     \
     .allowedBase=&E_Couleur, .nameEN="Color"},                              \
    {"CouleurFond", NULL, "entier", "-1",                                     \
     "Couleur de fond (RVB packé BGR ; -1 = défaut du thème).",             \
     "Background color (RGB packed BGR ; -1 = theme default).",               \
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"}

/* styleMenuPrincipalBarreBase / styleMenuPrincipalOptionBase — surcharge PLATE
 * par etat (ce que designe ..Etats[survol]). Couleurs seules, PAS de Police
 * ni de membre Etats (fin de chaine). */
static const OwlField FIELDS_styleMenuPrincipalBarreBase[] = {
    MENU_TEXT_BRICK_COLORS,
    {0},
};

/* styleMenuPrincipalBarre — surface barre de menu (QMenuBar). Stateful :
 * brique texte de base + membre Etats (survol). */
static const OwlField FIELDS_styleMenuPrincipalBarre[] = {
    MENU_TEXT_BRICK_FIELDS,
    {"Etats", NULL, "styleMenuPrincipalBarreEtats", "{}",
     "Surcharge par etat de la barre (survol). "
     "..Etats[survol]..CouleurFond = ...",
     "Per-state override of the bar (hover). "
     "..Etats[hover]..CouleurFond = ...",
     .nameEN="States"},
    {0},
};

static const OwlField FIELDS_styleMenuPrincipalOptionBase[] = {
    MENU_TEXT_BRICK_COLORS,
    {0},
};

/* styleMenuPrincipalOption — surface d'une rangee de menu (QMenu::item).
 * Stateful : brique texte de base + membre Etats (survol). */
static const OwlField FIELDS_styleMenuPrincipalOption[] = {
    MENU_TEXT_BRICK_FIELDS,
    {"Etats", NULL, "styleMenuPrincipalOptionEtats", "{}",
     "Surcharge par etat de la rangee (survol : item survolé, courant, "
     "ou parent ouvert). ..Etats[survol]..CouleurFond = ...",
     "Per-state override of the row (hover: hovered, current, or open-parent "
     "row). ..Etats[hover]..CouleurFond = ...",
     .nameEN="States"},
    {0},
};

/* styleMenuPrincipalSousMenu — surface du panneau deroulant (QMenu). Statique :
 * conteneur sans texte. Fond + separateur + bordure (couleur/epaisseur/arrondi
 * du liseré du panneau). */
static const OwlField FIELDS_styleMenuPrincipalSousMenu[] = {
    {"CouleurFond", NULL, "entier", "-1",
     "Couleur de fond du panneau déroulant (RVB packé BGR ; -1 = défaut du thème).",
     "Drop-down panel background color (RGB packed BGR ; -1 = theme default).",
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"},
    {"CouleurSeparateur", "CouleurS\xc3\xa9parateur", "entier", "-1",
     "Couleur du trait de séparation entre les rangées (RVB packé BGR ; -1 = défaut du thème).",
     "Separator line color between rows (RGB packed BGR ; -1 = theme default).",
     .allowedBase=&E_Couleur, .nameEN="SeparatorColor"},
    {"Arrondi", NULL, "entier", "0",
     "Rayon d'arrondi des coins du panneau en pixels (0 = coins droits).",
     "Panel corner radius in pixels (0 = square corners).",
     .nameEN="CornerRadius"},
    {"Epaisseur", NULL, "entier", "1",
     "Epaisseur du liseré du panneau en pixels (0 = liseré invisible).",
     "Panel border thickness in pixels (0 = invisible border).",
     .nameEN="Thickness"},
    {"CouleurBordure", NULL, "entier", "-1",
     "Couleur du liseré du panneau (RVB packé BGR ; -1 = défaut du thème).",
     "Panel border color (RGB packed BGR ; -1 = theme default).",
     .allowedBase=&E_Couleur, .nameEN="BorderColor"},
    {0},
};

/* styleMenuPrincipal — modele de style typé du menu classique de fenetre.
 * Composite de 3 surfaces : Barre / SousMenu / OptionMenu. */
static const OwlField FIELDS_styleMenuPrincipal[] = {
    {"Barre", NULL, "styleMenuPrincipalBarre", "{}",
     "Surface barre de menu (la bande horizontale). Stateful : porte l'état survol via Etats.",
     "Menu bar surface (the horizontal strip). Stateful: carries the hover state via Etats.",
     .nameEN="Bar"},
    {"SousMenu", NULL, "styleMenuPrincipalSousMenu", "{}",
     "Surface du panneau déroulant (conteneur). Statique : CouleurFond, CouleurSéparateur, Arrondi, Epaisseur, CouleurBordure.",
     "Drop-down panel surface (container). Static: BackgroundColor, SeparatorColor, CornerRadius, Thickness, BorderColor.",
     .nameEN="Submenu"},
    {"OptionMenu", NULL, "styleMenuPrincipalOption", "{}",
     "Surface d'une rangée de menu (option terminale ou rangée ouvrant un sous-menu). "
     "Stateful : porte l'état survol via Etats.",
     "Menu row surface (terminal option or sub-menu-opening row). "
     "Stateful: carries the hover state via Etats.",
     .nameEN="MenuOption"},
    {0},
};

/* styleMenuContextuel — modele de style typé du menu contextuel.
 * 2 surfaces SEULEMENT (SousMenu / OptionMenu), reutilise les surfaces de
 * styleMenuPrincipal. PAS de Barre (un contextuel n'en a pas). */
static const OwlField FIELDS_styleMenuContextuel[] = {
    {"SousMenu", NULL, "styleMenuPrincipalSousMenu", "{}",
     "Surface du panneau déroulant (conteneur). Statique : CouleurFond, CouleurSéparateur, Arrondi, Epaisseur, CouleurBordure.",
     "Drop-down panel surface (container). Static: BackgroundColor, SeparatorColor, CornerRadius, Thickness, BorderColor.",
     .nameEN="Submenu"},
    {"OptionMenu", NULL, "styleMenuPrincipalOption", "{}",
     "Surface d'une rangée de menu (option terminale ou rangée ouvrant un sous-menu). "
     "Stateful : porte l'état survol via Etats.",
     "Menu row surface (terminal option or sub-menu-opening row). "
     "Stateful: carries the hover state via Etats.",
     .nameEN="MenuOption"},
    {0},
};

/*  (lot 5d) — styleOnglet : brique texte partagee d'un volet
 * (Couleur, CouleurFond, Police). Sentinelles -1 = « non pose ». */
#define VOLET_TEXT_BRICK_FIELDS                                               \
    {"Couleur",     NULL, "entier", "-1",                                     \
     "Couleur du texte de la languette (RVB packé BGR ; -1 = défaut du thème).", \
     "Tab caption text color (RGB packed BGR ; -1 = theme default).",         \
     .allowedBase=&E_Couleur, .nameEN="Color"},                              \
    {"CouleurFond", NULL, "entier", "-1",                                     \
     "Couleur de fond de la languette (RVB packé BGR ; -1 = défaut du thème).", \
     "Tab background color (RGB packed BGR ; -1 = theme default).",           \
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"},                    \
    {"Police",      NULL, "Police", "Police par defaut",                      \
     "Descripteur de police imbrique (Nom/Taille/Gras/Italique/...).",        \
     "Embedded font descriptor (Name/Size/Bold/Italic/...).",                \
     .nameEN="Font"}

/* styleVoletBase — surcharge PLATE par etat d'un volet (ce que designe
 * ..Etats[survol]). Couleurs seules, PAS de Police par etat, PAS de membre
 * Etats (fin de chaine). */
static const OwlField FIELDS_styleVoletBase[] = {
    {"Couleur",     NULL, "entier", "-1",
     "Couleur du texte de la languette (RVB packé BGR ; -1 = défaut du thème).",
     "Tab caption text color (RGB packed BGR ; -1 = theme default).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {"CouleurFond", NULL, "entier", "-1",
     "Couleur de fond de la languette (RVB packé BGR ; -1 = défaut du thème).",
     "Tab background color (RGB packed BGR ; -1 = theme default).",
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"},
    {0},
};

/* styleVolet — surface d'un volet (la languette). Type PARTAGE par VoletActif
 * et VoletInactif. Stateful : brique texte de base + membre Etats (survol). */
static const OwlField FIELDS_styleVolet[] = {
    VOLET_TEXT_BRICK_FIELDS,
    {"Arrondi", NULL, "entier", "-1",
     "Rayon d'arrondi des coins de la languette en pixels (-1 = coins du thème).",
     "Corner radius of the tab in pixels (-1 = theme corners).",
     .nameEN="CornerRadius"},
    {"Etats", NULL, "styleVoletEtats", "{}",
     "Surcharge par etat du volet (survol). "
     "..Etats[survol]..CouleurFond = ...",
     "Per-state override of the pane (hover). "
     "..Etats[hover]..CouleurFond = ...",
     .nameEN="States"},
    {0},
};

/* styleCadreExtérieurOnglet — CadreExtérieur RESTREINT du pane d'un
 * Champ Onglet. Uniquement CouleurFond + Cadre simple (Couleur/Epaisseur/
 * Arrondi). Distinct du styleCadreExtérieur complet (Etats/Dégradé/Ombre/
 * Retraits/Trait/Bords), reserve aux champs qui savent piloter ces props. */
static const OwlField FIELDS_styleCadreExterieurOnglet[] = {
    {"CouleurFond", NULL, "entier", "-1",
     "Couleur de fond du pane (RVB packé BGR ; -1 = pas de fond).",
     "Pane background color (RGB packed BGR ; -1 = no fill).",
     .allowedBase=&E_Couleur, .nameEN="BackgroundColor"},
    {"Cadre",       NULL, "CadreOnglet", "{}",
     "Bordure du pane (Couleur/Epaisseur/Arrondi).",
     "Pane border (Color/Thickness/CornerRadius).",
     .nameEN="Border"},
    {0},
};

/* styleOnglet — modele de style typé du Champ Onglet. Trois sous-elements
 * composites : CadreExtérieur (fond + bordure du pane, type restreint
 * styleCadreExtérieurOnglet) + 2 surfaces VoletActif / VoletInactif (meme
 * type styleVolet). Plus aucune prop scalaire a la racine. */
static const OwlField FIELDS_styleOnglet[] = {
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieurOnglet", "{}",
     "Sous-\xc3\xa9l\xc3\xa9ment conteneur du pane (la zone sous les languettes) : fond + bordure (Cadre..Couleur/Épaisseur/Arrondi).",
     "Container sub-element of the pane (the area below the tabs): background + border (Cadre..Couleur/Épaisseur/Arrondi).",
     .nameEN="OuterFrame"},
    {"VoletActif", NULL, "styleVolet", "{}",
     "Surface du volet affiché (languette sélectionnée). Stateful : porte l'état survol via Etats.",
     "Displayed pane surface (selected tab). Stateful: carries the hover state via Etats.",
     .nameEN="ActivePane"},
    {"VoletInactif", NULL, "styleVolet", "{}",
     "Surface des volets non affichés (languettes non sélectionnées). Stateful : porte l'état survol via Etats.",
     "Non-displayed panes surface (unselected tabs). Stateful: carries the hover state via Etats.",
     .nameEN="InactivePane"},
    {0},
};

/* styleSaisie — modele de style typé du champ Saisie (, refonte).
 * Struct pur de sous-elements : ZoneSaisie / Libellé / CadreExtérieur. Plus de
 * props plates a la racine (le crochet [etat] ne vit que sur ZoneSaisie..Etats).
 * Ce tableau est le contrat que l'inspecteur editeur derivera. */
static const OwlField FIELDS_styleSaisie[] = {
    {"ZoneSaisie",     NULL,             "styleZoneSaisie", "{}",
     "Sous-element zone de saisie (texte editable). Porte les etats visuels via "
     "..Etats[survol/focus/grisé/lectureSeule].",
     "Input zone sub-element (editable text). Carries the visual states via "
     "..Etats[hover/focus/disabled/read-only].",
     .nameEN="InputZone"},
    {"Libelle",        "Libell\xc3\xa9",  "styleLibelleSaisie", "{}",
     "Sous-element libellé integre (statique).",
     "Embedded label sub-element (static).",
     .nameEN="Label"},
    {"CadreExterieur", "CadreExt\xc3\xa9rieur", "styleCadreExterieur", "{}",
     "Sous-element conteneur qui enveloppe le libellé et la zone (surcharges par état via Etats).",
     "Container sub-element wrapping the label and the zone (per-state overrides via Etats).",
     .nameEN="OuterFrame"},
    {0},
};

/* CadreOnglet — cadre restreint du CadreExtérieur d'un Champ Onglet.
 * Bordure simple : Couleur/Epaisseur/Arrondi. PAS de Trait/Bords/Coins (non
 * pilotables sur un pane d'onglet). Type dedie pour ne pas exposer, dans
 * explain/vscode-data/LSP, des props que le compilateur onglet rejette. */
static const OwlField FIELDS_CadreOnglet[] = {
    {"Couleur",   NULL, "entier", "0",
     "Couleur du cadre (RVB packé BGR ; 0 = noir).",
     "Border color (RGB packed BGR ; 0 = black).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {"Epaisseur", NULL, "entier", "0",
     "Epaisseur du cadre en pixels. 0 = cadre invisible.",
     "Border thickness in pixels. 0 = invisible border.",
     .nameEN="Thickness"},
    {"Arrondi",   NULL, "entier", "0",
     "Rayon d'arrondi des coins en pixels (0 = coins droits).",
     "Corner radius in pixels (0 = square corners).",
     .nameEN="CornerRadius"},
    {0},
};

/* Cadre — canon. 5 proprietes essentielles V1.
 * Trait : entier contraint a l'enum traitCadre (TraitContinu/Tiret/Pointille/...).
 * Bords : entier combinable a l'enum bordsCadre (BordHaut+BordDroite+...). */
static const OwlField FIELDS_Cadre[] = {
    {"Couleur",   NULL, "entier", "0",
     "Couleur du cadre (RVB packé BGR ; 0 = noir).",
     "Border color (RGB packed BGR ; 0 = black).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {"Epaisseur", NULL, "entier", "0",
     "Epaisseur du cadre en pixels. 0 = cadre invisible.",
     "Border thickness in pixels. 0 = invisible border.",
     .nameEN="Thickness"},
    {"Trait",     NULL, "entier", "TraitContinu",
     "Style du trait (TraitContinu, TraitTiret, TraitPointille, ...).",
     "Line style (TraitContinu, TraitTiret, TraitPointille, ...).",
     .allowedBase=&E_TraitCadre, .nameEN="LineStyle"},
    {"Arrondi",   NULL, "entier", "0",
     "Rayon d'arrondi des coins en pixels (0 = coins droits).",
     "Corner radius in pixels (0 = square corners).",
     .nameEN="CornerRadius"},
    {"Bords",     NULL, "entier", "BordTous",
     "Combinaison de BordHaut/Droite/Bas/Gauche, ou BordAucun/BordTous (defaut).",
     "Combination of BorderTop/Right/Bottom/Left, or BorderNone/BorderAll (default).",
     .allowedBase=&E_BordsCadre, .allowedOptions=&E_BordsCadre, .nameEN="Edges"},
    /* Sous-objets ajoutes en B.13 : exposes par le template runtime mais
     * qui manquaient a la declaration FIELDS_Cadre (incoherence remontee
     * lors de la migration bilingue — comme Definition.Parametres/Champs/
     * TypeNom). Type sous-jacent Coin (Largeur, Hauteur) et Trait (Couleur,
     * Epaisseur, Type). */
    {"CoinHautGauche", NULL, "Coin", "{}",
     "Sous-objet : taille du coin haut-gauche (Largeur, Hauteur).",
     "Sub-object: top-left corner size (Width, Height).",
     .nameEN="TopLeftCorner"},
    {"CoinHautDroite", NULL, "Coin", "{}",
     "Sous-objet : taille du coin haut-droite.",
     "Sub-object: top-right corner size.",
     .nameEN="TopRightCorner"},
    {"CoinBasGauche",  NULL, "Coin", "{}",
     "Sous-objet : taille du coin bas-gauche.",
     "Sub-object: bottom-left corner size.",
     .nameEN="BottomLeftCorner"},
    {"CoinBasDroite",  NULL, "Coin", "{}",
     "Sous-objet : taille du coin bas-droite.",
     "Sub-object: bottom-right corner size.",
     .nameEN="BottomRightCorner"},
    {"TraitHaut",      NULL, "Trait", "{}",
     "Sous-objet : trait du bord haut (Couleur, Epaisseur, Type).",
     "Sub-object: top border line (Color, Thickness, Type).",
     .nameEN="BorderTop"},
    {"TraitBas",       NULL, "Trait", "{}",
     "Sous-objet : trait du bord bas.",
     "Sub-object: bottom border line.",
     .nameEN="BorderBottom"},
    {"TraitGauche",    NULL, "Trait", "{}",
     "Sous-objet : trait du bord gauche.",
     "Sub-object: left border line.",
     .nameEN="BorderLeft"},
    {"TraitDroite",    NULL, "Trait", "{}",
     "Sous-objet : trait du bord droite.",
     "Sub-object: right border line.",
     .nameEN="BorderRight"},
    {0},
};

/* Police — canon. 7 proprietes essentielles V1.
 * Le sous-champ Nom accepte une chaine libre OU une constante de la
 * famille iPolice* (allowedBase=&E_IFamily) qui propose 11 polices
 * predefinies au LSP : iPoliceLiberationSans, iPoliceArial, etc. */
static const OwlField FIELDS_Police[] = {
    {"Nom",       NULL,             "cha\xc3\xaene", "\"\"",
     "Nom de la famille de police. Chaine libre (ex: \"Arial\") ou constante iPolice* (iPoliceLiberationSans, iPoliceArial, ...).",
     "Font family name. Free string (eg: \"Arial\") or iPolice* constant (iPoliceLiberationSans, iPoliceArial, ...).",
     .allowedBase=&E_IFamily, .nameEN="Name"},
    {"Taille",    NULL,             "entier",  "11",
     "Taille en points typographiques (positif).",
     "Size in typographic points (positive).",
     .nameEN="Size"},
    {"Gras",      NULL,             "bool\xc3\xa9""en", "Faux",
     "Vrai = poids gras.",
     "True = bold weight.",
     .nameEN="Bold"},
    {"Italique",  NULL,             "bool\xc3\xa9""en", "Faux",
     "Vrai = inclinaison italique.",
     "True = italic style.",
     .nameEN="Italic"},
    {"Souligne", "Soulign\xc3\xa9", "bool\xc3\xa9""en", "Faux",
     "Vrai = soulignement du texte.",
     "True = underline text.",
     .nameEN="Underlined"},
    {"Barre",   "Barr\xc3\xa9",     "bool\xc3\xa9""en", "Faux",
     "Vrai = texte barre (strikethrough).",
     "True = strikethrough text.",
     .nameEN="StrikeOut"},
    {"Couleur",   NULL,             "entier",  "0",
     "Couleur du texte (RVB packé BGR ; 0 = noir).",
     "Text color (RGB packed BGR ; 0 = black).",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {0},
};

/* Projet — catalogue de métadonnées de première brique. Champs chaîne
 * optionnels (défaut ""), + Langues (tableau, obligatoire). Catalogue figé :
 * un champ hors liste est rejeté au compile-time (cf. compiler.c bloc
 * PROPRIETES Projet). Theme = fichier de styles nommés référencé par le projet. */
static const OwlField FIELDS_Projet[] = {
    {"Description", NULL, "cha\xc3\xaene", "\"\"",
     "Description courte du projet.",
     "Short project description.",
     .nameEN="Description"},
    {"Auteur",      NULL, "cha\xc3\xaene", "\"\"",
     "Auteur ou éditeur du projet.",
     "Project author or publisher.",
     .nameEN="Author"},
    {"Licence",     NULL, "cha\xc3\xaene", "\"\"",
     "Licence du projet.",
     "Project license.",
     .nameEN="License"},
    {"Langues",     NULL, "tableau de constantes de langue", "[]",
     "Langues du projet : tableau de constantes langueXxx officielles, "
     "toujours entre crochets, m\xc3\xaame pour une seule langue. La "
     "premi\xc3\xa8" "re est la langue par d\xc3\xa9" "faut (langue source). "
     "Exemple : Langues = [langueFran\xc3\xa7" "ais, langueAnglais] ; "
     "une seule langue s'\xc3\xa9" "crit Langues = [langueFran\xc3\xa7" "ais].",
     "Project languages: array of official langueXxx constants, always "
     "between brackets, even for a single language. The first one is the "
     "default (source) language. "
     "Example: Languages = [languageFrench, languageEnglish]; a single "
     "language is written Languages = [languageFrench].",
     .nameEN="Languages"},
    {"Theme",       "Th\xc3\xa8me", "Th\xc3\xa8me", "\"\"",
     "Th\xc3\xa8me appliqu\xc3\xa9 au projet : r\xc3\xa9" "f\xc3\xa9" "rence "
     "vers une instance d\xc3\xa9" "clar\xc3\xa9" "e `est un Th\xc3\xa8me` "
     "dans un compagnon (styles nomm\xc3\xa9s). Non renseign\xc3\xa9 = aucun "
     "th\xc3\xa8me.",
     "Theme applied to the project: reference to an instance declared as a "
     "Theme in a companion file (named styles). Unset = no theme.",
     .nameEN="Theme"},
    {"FenetreDemarrage", "Fen\xc3\xaatreD\xc3\xa9marrage", "Fen\xc3\xaatre",
     "\"\"",
     "Fen\xc3\xaatre ouverte au lancement du projet : r\xc3\xa9" "f\xc3\xa9"
     "rence (sans guillemets) vers une instance d\xc3\xa9" "clar\xc3\xa9" "e "
     "`est une Fen\xc3\xaatre` dans un compagnon. Non renseign\xc3\xa9 = "
     "aucune ouverture automatique (projet sans interface).",
     "Window opened when the project starts: reference (no quotes) to an "
     "instance declared as a Window in a companion file. Unset = no automatic "
     "opening (headless project).",
     .nameEN="StartupWindow"},
    {"FormatDate",  NULL, "cha\xc3\xaene", "\"\"",
     "Format de date par d\xc3\xa9" "faut du projet : gabarit de jetons "
     "(JJ, MM, AAAA...) appliqu\xc3\xa9 aux champs Date qui ne portent pas "
     "eux-m\xc3\xaames de MasqueAffichage. Non renseign\xc3\xa9 = format par "
     "d\xc3\xa9" "faut JJ/MM/AAAA.",
     "Project default date format: token template (DD, MM, YYYY...) applied "
     "to Date controls that do not carry their own DisplayMask. Unset = "
     "default DD/MM/YYYY format.",
     .nameEN="DateFormat"},
    {"FormatHeure", NULL, "cha\xc3\xaene", "\"\"",
     "Format d'heure par d\xc3\xa9" "faut du projet : gabarit de jetons "
     "(HH, MI, SS...) appliqu\xc3\xa9 aux champs Heure qui ne portent pas "
     "eux-m\xc3\xaames de MasqueAffichage. Non renseign\xc3\xa9 = format par "
     "d\xc3\xa9" "faut HH:MI.",
     "Project default time format: token template (HH, MI, SS...) applied "
     "to Time controls that do not carry their own DisplayMask. Unset = "
     "default HH:MI format.",
     .nameEN="TimeFormat"},
    {"FormatDuree", "FormatDur\xc3\xa9" "e", "cha\xc3\xaene", "\"\"",
     "Format de dur\xc3\xa9" "e par d\xc3\xa9" "faut du projet : gabarit de "
     "jetons (JJ, HH, MI, SS...) appliqu\xc3\xa9 aux champs Dur\xc3\xa9" "e qui "
     "ne portent pas eux-m\xc3\xaames de MasqueAffichage. MI = minutes, HH non "
     "plafonn\xc3\xa9 \xc3\xa0 23. Non renseign\xc3\xa9 = format par d\xc3\xa9" "faut "
     "HH:MI:SS.",
     "Project default duration format: token template (JJ, HH, MI, SS...) "
     "applied to Duration controls that do not carry their own DisplayMask. "
     "MI = minutes, HH not capped at 23. Unset = default HH:MI:SS format.",
     .nameEN="DurationFormat"},
    {"FormatNombre", NULL, "FormatNombre", "{}",
     "Format d'affichage num\xc3\xa9rique par d\xc3\xa9" "faut du projet : "
     "litt\xc3\xa9ral inline `{ S\xc3\xa9parateurMilliers = ..., "
     "S\xc3\xa9parateurD\xc3\xa9" "cimal = ..., ChiffresApr\xc3\xa8sVirgule = "
     "... }`, h\xc3\xa9rit\xc3\xa9 par les champs num\xc3\xa9riques sans "
     "FormatNombre propre. Non renseign\xc3\xa9 = affichage plat courant.",
     "Project default numeric display format: inline literal `{ "
     "ThousandsSeparator = ..., DecimalSeparator = ..., DecimalPlaces = ... }`, "
     "inherited by numeric controls without their own NumberFormat. Unset = "
     "current plain display.",
     .nameEN="NumberFormat"},
    {"FormatMonetaire", "FormatMon\xc3\xa9taire", "FormatMon\xc3\xa9taire", "{}",
     "Format d'affichage mon\xc3\xa9taire par d\xc3\xa9" "faut du projet : "
     "litt\xc3\xa9ral inline `{ Symbole = ..., S\xc3\xa9parateurMilliers = ..., "
     "ChiffresApr\xc3\xa8sVirgule = ... }`, h\xc3\xa9rit\xc3\xa9 par les champs "
     "mon\xc3\xa9taires sans FormatMon\xc3\xa9taire propre. Non renseign\xc3\xa9 = "
     "affichage mon\xc3\xa9taire plat courant.",
     "Project default currency display format: inline literal `{ Symbol = ..., "
     "ThousandsSeparator = ..., DecimalPlaces = ... }`, inherited by currency "
     "controls without their own CurrencyFormat. Unset = current plain "
     "display.",
     .nameEN="CurrencyFormat"},
    {"ActiveAvertissementConventionDeNommage", NULL, "booléen", "Faux",
     "Active la vérification des préfixes de nommage des champs. À Vrai, "
     "chaque champ dont le nom ne commence pas par le préfixe canonique de "
     "son type (BTN_ pour un Bouton, SAI_ pour une Saisie, COL_ pour une "
     "Colonne...) déclenche un avertissement non bloquant. Non renseigné ou "
     "Faux = aucune vérification. La table des préfixes est unique et figée "
     "par le langage : elle ne se configure ni par projet ni par utilisateur. "
     "La liste complète des préfixes est décrite dans la charte des préfixes "
     "de nommage.",
     "Enables naming-prefix checking for controls. When True, any control "
     "whose name does not start with the canonical prefix of its type (BTN_ "
     "for a Button, SAI_ for an Input, COL_ for a Column...) raises a "
     "non-blocking warning. Unset or False = no checking. The prefix table is "
     "unique and fixed by the language: it cannot be configured per project or "
     "per user. The full list of prefixes is described in the naming-prefix "
     "charter.",
     .nameEN="EnableNamingConventionWarning"},
    {0},
};

/* pdfParametre — canon. 5 proprietes. Les champs Version,
 * Qualité, FormatCouleur, AffichagePage et Zoom du panel canon ne sont pas honorés
 * par le moteur PDF et ont été retirés (cf ). */
static const OwlField FIELDS_pdfParametre[] = {
    {"MotDePasseModification", NULL,                "cha\xc3\xaene", "\"\"",
     "Mot de passe owner (modification des permissions).",
     "Owner password (permissions modification).",
     .nameEN="ModificationPassword"},
    {"MotDePasseOuverture",    NULL,                "cha\xc3\xaene", "\"\"",
     "Mot de passe d'ouverture du PDF.",
     "PDF open password.",
     .nameEN="OpenPassword"},
    {"Protection",             NULL,                "entier",  "0",
     "Combinaison de constantes iPDFA*/iPDFA1b/iPDFA3b et iProtection*.",
     "Combination of iPDFA*/iPDFA1b/iPDFA3b and iProtection* constants.",
     .allowedBase=&E_pdfProtection, .allowedOptions=&E_pdfProtection,
     .nameEN="Protection"},
    {"Signet",                 NULL,                "bool\xc3\xa9""en", "Faux",
     "Vrai = un signet par page automatique.",
     "True = automatic bookmark per page.",
     .nameEN="Bookmark"},
    {"Unicode",                NULL,                "bool\xc3\xa9""en", "Vrai",
     "PDF au format Unicode (OWL UTF-8 natif, défaut Vrai).",
     "Unicode PDF (OWL UTF-8 native, default true).",
     .nameEN="Unicode"},
    {0},
};

/* Sous-types composés du Cadre : declares pour permettre `coin est un Coin`,
 * `trait est un Trait` cote user (etaient orphelins du registry — exposés
 * comme types de field dans FIELDS_Cadre mais non declarables standalone). */
static const OwlField FIELDS_Coin[] = {
    {"Largeur", NULL, "entier", "0", "Largeur (rayon horizontal) du coin arrondi, en pixels.",
     "Width (horizontal radius) of the rounded corner, in pixels.", .nameEN="Width"},
    {"Hauteur", NULL, "entier", "0", "Hauteur (rayon vertical) du coin arrondi, en pixels.",
     "Height (vertical radius) of the rounded corner, in pixels.", .nameEN="Height"},
    {0},
};

static const OwlField FIELDS_Trait[] = {
    {"Couleur",   NULL, "entier", "0",            "Couleur du trait (RVB packé BGR ; 0 = noir).",
     "Line color (RGB packed BGR ; 0 = black).", .nameEN="Color"},
    {"Epaisseur", NULL, "entier", "0",            "Épaisseur en pixels (0 = bord invisible).",
     "Thickness in pixels (0 = invisible border).", .nameEN="Thickness"},
    {"Type",      NULL, "entier", "TraitContinu", "Style de trait.",
     "Line style.", .allowedBase=&E_TraitCadre, .nameEN="Style"},
    {0},
};

/* Option : item d'un Sélecteur / Combo / Liste etc. */
static const OwlField FIELDS_Option[] = {
    {"Libelle",        "Libellé", "chaîne", "\"\"", "Texte affiché à l'utilisateur.",
     "Label displayed to the user.", .nameEN="Label"},
    {"SousLibelle",    "Sous-libellé", "chaîne", "\"\"",
     "Texte secondaire affiché sous le libellé principal (style Vista, optionnel).",
     "Secondary text displayed under the main label (Vista style, optional).",
     .nameEN="SubLabel"},
    {"ValeurRenvoyee", "ValeurRenvoyée", "chaîne", "\"\"",
     "Valeur retournée à la lecture si l'option est sélectionnée.",
     "Value returned when the option is selected.", .nameEN="ReturnedValue"},
    {"Image",          NULL, "chaîne", "\"\"", "Chemin de l'image associée (optionnel).",
     "Path to the associated image (optional).", .nameEN="Image"},
    {0},
};

/* UUID — aucun field public exposé. Le storage interne `_bytes` est opaque
 * côté code OWL (préfixe `_`), accessible uniquement via DonneUUID et la
 * méthode .VersChaîne (sérialisation). Pattern fields = NULL (cf zipArchive,
 * MockNative, Thème). */

/* durée — type primitif (valeur entière en millisecondes au runtime, chemin
 * mot-clé TOKEN_DUREE au compilateur). Ces fields ne sont PAS un template
 * runtime (pas de register_global_instance) : c'est la métadonnée de
 * CONSTRUCTION du littéral `{ Heure = 2, Minute = 30 }`, consommée par la
 * complétion LSP (chemin registry-driven générique). STRICTEMENT les
 * composantes de construction fines (jours..millisecondes) — les totaux
 * accesseurs (.EnMillisecondes, .EnHeures…) sont exposés via le catalogue de
 * propriétés builtin (compiler.c::PROPS_DURATION), pas ici : on lit avec les
 * accesseurs, on écrit avec les composantes. Noms alignés sur la table canon
 * de construction du compilateur (compiler.c::DUREE_COMPOSANTES). */
static const OwlField FIELDS_Duree[] = {
    {"Jour",         NULL, "entier", "0", "Composante jours de la durée.",
     "Days component of the duration.",         .nameEN="Day"},
    {"Heure",        NULL, "entier", "0", "Composante heures de la durée.",
     "Hours component of the duration.",        .nameEN="Hour"},
    {"Minute",       NULL, "entier", "0", "Composante minutes de la durée.",
     "Minutes component of the duration.",      .nameEN="Minute"},
    {"Seconde",      NULL, "entier", "0", "Composante secondes de la durée.",
     "Seconds component of the duration.",      .nameEN="Second"},
    {"Milliseconde", NULL, "entier", "0", "Composante millisecondes de la durée.",
     "Milliseconds component of the duration.", .nameEN="Millisecond"},
    {0},
};

static const OwlTypeDef TYPES[] = {
    { .canonical      = "httpRequ\xc3\xaate",   /* httpRequête */
      .canonicalAscii = "httpRequete",
      .aliases        = ALIAS_httpRequete,
      .category       = "HTTP",
      .descFR         = "Structure pour construire une requête HTTP(S).",
      .descEN         = "Structure to build an HTTP(S) request.",
      .fields         = FIELDS_httpRequete,
      .nameEN         = "httpRequest",
      .genre          = OWL_GENRE_F },   /* la requête */
    { .canonical      = "httpR\xc3\xa9ponse",   /* httpRéponse */
      .canonicalAscii = "httpReponse",
      .aliases        = ALIAS_httpReponse,
      .category       = "HTTP",
      .descFR         = "Résultat d'un HTTPEnvoie.",
      .descEN         = "Result of HTTPEnvoie.",
      .fields         = FIELDS_httpReponse,
      .nameEN         = "httpResponse",
      .genre          = OWL_GENRE_F },   /* la réponse */
    { .canonical      = "AWSSession",
      .canonicalAscii = "AWSSession",
      .aliases        = NULL,
      .category       = "AWS",
      .descFR         = "Identifiants AWS statiques (région, clé d'accès, clé secrète, jeton de session et endpoint optionnels). Passé en premier paramètre d'AWSRequête.",
      .descEN         = "Static AWS credentials (region, access key, secret key, optional session token and endpoint). Passed as the first parameter of AWSRequest.",
      .fields         = FIELDS_AWSSession,
      .nameEN         = "AWSSession",
      .genre          = OWL_GENRE_F },   /* la session */
    { .canonical      = "pdfParam\xc3\xa8tre",  /* pdfParamètre */
      .canonicalAscii = "pdfParametre",
      .aliases        = ALIAS_pdfParametre,
      .category       = "PDF",
      .descFR         = "Regroupe les options de génération d'un PDF (niveau de gris, PDF/A, signets, protection). À passer à iParamètrePDF.",
      .descEN         = "Bundles the PDF generation options (grayscale, PDF/A, bookmarks, protection). Passed to iParametrePDF.",
      .fields         = FIELDS_pdfParametre,
      .nameEN         = "pdfParameter",
      .genre          = OWL_GENRE_M },   /* le paramètre */
    { .canonical      = "Police",
      .canonicalAscii = "Police",
      .aliases        = ALIAS_Police,
      .category       = "Affichage",
      .descFR         = "Descripteur de police de caractères. Sous-propriétés : Nom, Taille, Gras, Italique, Souligné, Barré, Couleur. Utilisable sur les Libellés (Fenêtre + État) via Police..Nom = ... etc.",
      .descEN         = "Font descriptor. Sub-properties: Name, Size, Bold, Italic, Underlined, StrikeOut, Color. Usable on Labels (Window + Report) via Font..Name = ... etc.",
      .fields         = FIELDS_Police,
      .nameEN         = "Font",
      .genre          = OWL_GENRE_F },   /* la police */
    { .canonical      = "Cadre",
      .canonicalAscii = "Cadre",
      .aliases        = ALIAS_Cadre,
      .category       = "Affichage",
      .descFR         = "Descripteur de cadre. Sous-propriétés : Couleur, Epaisseur, Trait, Arrondi, Bords. Utilisable sur tout champ visuel.",
      .descEN         = "Border descriptor. Sub-properties: Color, Thickness, LineStyle, CornerRadius, Edges. Usable on any visual field.",
      .fields         = FIELDS_Cadre,
      .nameEN         = "Frame",
      .genre          = OWL_GENRE_M },   /* le cadre */
    { .canonical      = "CadreOnglet",
      .canonicalAscii = "CadreOnglet",
      .aliases        = ALIAS_CadreOnglet,
      .category       = "Affichage",
      .descFR         = "Descripteur de cadre restreint du pane d'un Champ Onglet. Sous-propriétés : Couleur, Epaisseur, Arrondi. Bordure simple, sans trait ni bords individuels.",
      .descEN         = "Restricted border descriptor for a Tab field's pane. Sub-properties: Color, Thickness, CornerRadius. Simple border, no line style nor per-edge control.",
      .fields         = FIELDS_CadreOnglet,
      .nameEN         = "TabFrame",
      .genre          = OWL_GENRE_M },   /* le cadre */
    { .canonical      = "Coin",
      .aliases        = ALIAS_Coin,
      .category       = "Affichage",
      .descFR         = "Sous-type d'un Cadre : taille d'un coin arrondi (Largeur, Hauteur en pixels). Utilisé par Cadre..CoinHautGauche/CoinHautDroite/CoinBasGauche/CoinBasDroite.",
      .descEN         = "Sub-type of Cadre: rounded corner size (Width, Height in pixels). Used by Cadre..TopLeftCorner/TopRightCorner/BottomLeftCorner/BottomRightCorner.",
      .fields         = FIELDS_Coin,
      .nameEN         = "Corner",
      .genre          = OWL_GENRE_M },   /* le coin */
    { .canonical      = "Trait",
      .aliases        = ALIAS_Trait,
      .category       = "Affichage",
      .descFR         = "Sous-type d'un Cadre : descripteur d'un trait par bord (Couleur, Epaisseur, Type). Utilisé par Cadre..TraitHaut/TraitBas/TraitGauche/TraitDroite.",
      .descEN         = "Sub-type of Cadre: per-side line descriptor (Color, Thickness, Style). Used by Cadre..BorderTop/BorderBottom/BorderLeft/BorderRight.",
      .fields         = FIELDS_Trait,
      .nameEN         = "Stroke",
      .genre          = OWL_GENRE_M },   /* le trait */
    { .canonical      = "Style",
      .canonicalAscii = "Style",
      .aliases        = ALIAS_Style,
      .category       = "Affichage",
      .descFR         = "Descripteur d'apparence aggrégé. Sous-propriétés : Couleur (texte), CouleurFond, RetraitGauche/Droit/Haut/Bas, CurseurSouris, Police (composé), Cadre (composé). Applicable sur tout champ visuel via 'champ..Style = STY_X'.",
      .descEN         = "Aggregated appearance descriptor. Sub-properties: Color (text), BackgroundColor, LeftPadding/RightPadding/TopPadding/BottomPadding, MouseCursor, Font (composed), Border (composed). Apply to any visual field via 'field..Style = STY_X'.",
      .fields         = FIELDS_Style,
      .nameEN         = "Style",
      .genre          = OWL_GENRE_M },   /* le style */
    { .canonical      = "styleSaisie",
      .canonicalAscii = "styleSaisie",
      .aliases        = ALIAS_styleSaisie,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du champ Saisie. Struct de sous-éléments : ZoneSaisie (zone éditable, avec états visuels), Libellé (statique), CadreExtérieur (le conteneur, surcharges par état via Etats). États visuels via 'ZoneSaisie..Etats[survol/focus/grisé/lectureSeule]'. Appliqué via 'SAI_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Input field. Struct of sub-elements: InputZone (editable zone, with visual states), Label (static), OuterFrame (the container, per-state overrides via Etats). Visual states via 'InputZone..Etats[hover/focus/disabled/read-only]'. Applied via 'SAI_X..Style = STY_Y'.",
      .fields         = FIELDS_styleSaisie,
      .nameEN         = "styleInput",
      .genre          = OWL_GENRE_M },   /* le style de saisie */
    { .canonical      = "styleZoneSaisie",
      .canonicalAscii = "styleZoneSaisie",
      .aliases        = ALIAS_styleZoneSaisie,
      .category       = "Affichage",
      .descFR         = "Sous-élément zone de saisie d'un styleSaisie. Props de base (Couleur, CouleurFond, Retraits, Police, Cadre) + membre Etats (surcharges par état, indexé via [survol/focus/grisé/lectureSeule]).",
      .descEN         = "Input-zone sub-element of a styleSaisie. Base props (Color, BackgroundColor, paddings, Font, Border) + Etats member (per-state overrides, indexed via [hover/focus/disabled/read-only]).",
      .fields         = FIELDS_styleZoneSaisie,
      .nameEN         = "styleInputZone",
      .genre          = OWL_GENRE_F },   /* la zone de saisie */
    { .canonical      = "styleZoneSaisieBase",
      .canonicalAscii = "styleZoneSaisieBase",
      .aliases        = ALIAS_styleZoneSaisieBase,
      .category       = "Affichage",
      .descFR         = "Style plat d'une surcharge par état de la zone de saisie (ce que désigne ..Etats[état]). Props de base seules : ni Etats, ni sous-éléments (fin de chaîne).",
      .descEN         = "Flat style of a per-state override of the input zone (what ..Etats[state] targets). Base props only: no Etats, no sub-elements (end of chain).",
      .fields         = FIELDS_styleZoneSaisieBase,
      .nameEN         = "styleInputZoneBase",
      .genre          = OWL_GENRE_M },   /* le style de base */
    { .canonical      = "styleZoneSaisieEtats",
      .canonicalAscii = "styleZoneSaisieEtats",
      .aliases        = ALIAS_styleZoneSaisieEtats,
      .category       = "Affichage",
      .descFR         = "Collection des surcharges par état de la zone de saisie. Indexée par l'état via ..Etats[survol/focus/grisé/lectureSeule] ; chaque état est un styleZoneSaisieBase.",
      .descEN         = "Collection of per-state overrides of the input zone. Indexed by state via ..Etats[hover/focus/disabled/read-only]; each state is a styleZoneSaisieBase.",
      .fields         = NULL,  /* collection : slots internes _Survol/... indexes via [etat] */
      .nameEN         = "styleInputZoneStates",
      .genre          = OWL_GENRE_M },   /* le jeu d'états */
    { .canonical      = "styleLibell\xc3\xa9Saisie",   /* styleLibelléSaisie */
      .canonicalAscii = "styleLibelleSaisie",
      .aliases        = ALIAS_styleLibelleSaisie,
      .category       = "Affichage",
      .descFR         = "Sous-élément libellé intégré d'un styleSaisie. Props de base (Couleur, CouleurFond, Retraits, Police, Cadre). Statique (sans surcharges par état).",
      .descEN         = "Embedded label sub-element of a styleSaisie. Base props (Color, BackgroundColor, paddings, Font, Border). Static (no per-state overrides).",
      .fields         = FIELDS_styleLibelleSaisie,
      .nameEN         = "styleInputLabel",
      .genre          = OWL_GENRE_M },   /* le style de libellé de saisie */
    { .canonical      = "styleLibell\xc3\xa9",   /* styleLibellé */
      .canonicalAscii = "styleLibelle",
      .aliases        = ALIAS_styleLibelle,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Libellé. Struct de sous-éléments : Libellé (le texte, statique) et CadreExtérieur (le conteneur, surcharges par état via Etats). Appliqué via 'LIB_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Label field. Struct of sub-elements: Label (the text, static) and OuterFrame (the container, per-state overrides via Etats). Applied via 'LIB_X..Style = STY_Y'.",
      .fields         = FIELDS_styleLibelle,
      .nameEN         = "styleLabel",
      .genre          = OWL_GENRE_M },   /* le style de libellé */
    { .canonical      = "styleImage",
      .canonicalAscii = "styleImage",
      .aliases        = ALIAS_styleImage,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Image. Un seul sous-élément : CadreExtérieur (la boîte : couleur de fond, cadre, retraits). La source de l'image et le mode d'échelle sont des propriétés du champ (Image, Cadrage), pas du Style. Le CadreExtérieur porte des surcharges par état (survol/focus/appuyé/grisé) via Etats. Appliqué via 'IMG_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Image field. A single sub-element: OuterFrame (the box: background color, border, paddings). The image source and framing mode are field properties (Image, Cadrage), not part of the Style. The OuterFrame carries per-state overrides (hover/focus/pressed/disabled) via Etats. Applied via 'IMG_X..Style = STY_Y'.",
      .fields         = FIELDS_styleImage,
      .nameEN         = "styleImage",
      .genre          = OWL_GENRE_M },   /* le style d'image */
    { .canonical      = "styleCodeBarre",
      .canonicalAscii = "styleCodeBarre",
      .aliases        = ALIAS_styleCodeBarre,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ CodeBarres. Un seul sous-élément : CadreExtérieur (la boîte : couleur de fond, cadre, retraits). Le contenu et la symbologie sont des propriétés du champ (Contenu, TypeCodeBarres, Option), pas du Style. Le CadreExtérieur porte des surcharges par état (survol/focus/appuyé/grisé) via Etats. Appliqué via 'CBA_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the BarCode field. A single sub-element: OuterFrame (the box: background color, border, paddings). Content and symbology are field properties (Contenu, TypeCodeBarres, Option), not part of the Style. The OuterFrame carries per-state overrides (hover/focus/pressed/disabled) via Etats. Applied via 'CBA_X..Style = STY_Y'.",
      .fields         = FIELDS_styleCodeBarre,
      .nameEN         = "styleBarCode",
      .genre          = OWL_GENRE_M },   /* le style de code-barres */
    { .canonical      = "styleConteneur",
      .canonicalAscii = "styleConteneur",
      .aliases        = ALIAS_styleConteneur,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Conteneur. Un seul sous-élément : CadreExtérieur (la boîte : couleur de fond, cadre, retraits). Aucun texte (groupeur passif). Statique (aucun état). Appliqué via 'CNT_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Container field. A single sub-element: OuterFrame (the box: background color, border, paddings). No text (passive group). Static (no state). Applied via 'CNT_X..Style = STY_Y'.",
      .fields         = FIELDS_styleConteneur,
      .nameEN         = "styleContainer",
      .genre          = OWL_GENRE_M },   /* le style de conteneur */
    { .canonical      = "styleInterrupteur",
      .canonicalAscii = "styleInterrupteur",
      .aliases        = ALIAS_styleInterrupteur,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Interrupteur (cases à cocher). Struct de sous-éléments : CadreExtérieur (le conteneur des cases), TexteOptions (le texte de chaque case : couleur, police), Coche (la marque cochée) et CadreCoche (la boîte de la coche : couleur de fond, cadre). Seul le CadreExtérieur porte des surcharges par état (survol/focus/appuyé/grisé, via Etats) ; les autres sous-éléments sont statiques. Appliqué via 'INT_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Switch field (checkboxes). Struct of sub-elements: OuterFrame (the checkbox container), OptionText (each option text: color, font), CheckMark (the check mark) and CheckBox (the check box: background color, border). Only the OuterFrame carries per-state overrides (hover/focus/pressed/disabled, via Etats); the other sub-elements are static. Applied via 'INT_X..Style = STY_Y'.",
      .fields         = FIELDS_styleInterrupteur,
      .nameEN         = "styleSwitch",
      .genre          = OWL_GENRE_M },   /* le style d'interrupteur */
    { .canonical      = "styleCoche",
      .canonicalAscii = "styleCoche",
      .aliases        = ALIAS_styleCoche,
      .category       = "Affichage",
      .descFR         = "Sous-élément marque de coche d'un styleInterrupteur. Une seule feuille : Couleur (couleur de la marque cochée). Statique.",
      .descEN         = "Check mark sub-element of a styleInterrupteur. A single leaf: Color (checked mark color). Static.",
      .fields         = FIELDS_styleCoche,
      .nameEN         = "styleCheckMark",
      .genre          = OWL_GENRE_F },   /* la coche */
    { .canonical      = "styleS\xc3\xa9lecteur",   /* styleSélecteur */
      .canonicalAscii = "styleSelecteur",
      .aliases        = ALIAS_styleSelecteur,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Sélecteur (boutons radio exclusifs). Struct de sous-éléments : CadreExtérieur (le conteneur des options), TexteOptions (le texte de chaque option : couleur, police), Puce (le rond sélectionné) et CadrePuce (la boîte du rond : couleur de fond, cadre). Seul le CadreExtérieur porte des surcharges par état (survol/focus/appuyé/grisé, via Etats) ; les autres sous-éléments sont statiques. Appliqué via 'SEL_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Selector field (exclusive radio buttons). Struct of sub-elements: OuterFrame (the option container), OptionText (each option text: color, font), Bullet (the selected bullet) and BulletBox (the bullet box: background color, border). Only the OuterFrame carries per-state overrides (hover/focus/pressed/disabled, via Etats); the other sub-elements are static. Applied via 'SEL_X..Style = STY_Y'.",
      .fields         = FIELDS_styleSelecteur,
      .nameEN         = "styleSelector",
      .genre          = OWL_GENRE_M },   /* le style de sélecteur */
    { .canonical      = "stylePuce",
      .canonicalAscii = "stylePuce",
      .aliases        = ALIAS_stylePuce,
      .category       = "Affichage",
      .descFR         = "Sous-élément rond d'un styleSélecteur. Une seule feuille : Couleur (couleur du rond sélectionné). Statique.",
      .descEN         = "Bullet sub-element of a styleSélecteur. A single leaf: Color (selected bullet color). Static.",
      .fields         = FIELDS_stylePuce,
      .nameEN         = "styleRadioMark",
      .genre          = OWL_GENRE_F },   /* la puce */
    { .canonical      = "styleListe",
      .canonicalAscii = "styleListe",
      .aliases        = ALIAS_styleListe,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Liste (list box). Struct de sous-éléments : CadreExtérieur (le cadre et le fond de la liste), LibelléLignes (le texte des lignes : couleur, police) et LigneSélectionnée (le texte de la ligne sélectionnée : couleur, police). Seul le CadreExtérieur porte des surcharges par état (survol/focus/appuyé/grisé, via Etats) ; les autres sous-éléments sont statiques. Appliqué via 'LISTE_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the List field (list box). Struct of sub-elements: OuterFrame (the list frame and background), RowLabel (row text: color, font) and SelectedRow (selected row text: color, font). Only the OuterFrame carries per-state overrides (hover/focus/pressed/disabled, via Etats); the other sub-elements are static. Applied via 'LISTE_X..Style = STY_Y'.",
      .fields         = FIELDS_styleListe,
      .nameEN         = "styleList",
      .genre          = OWL_GENRE_M },   /* le style de liste */
    { .canonical      = "styleCombo",
      .canonicalAscii = "styleCombo",
      .aliases        = ALIAS_styleCombo,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Combo (liste déroulante). Struct de sous-éléments : Libellé (le libellé intégré : couleur, police), CadreExtérieur (le conteneur englobant), ZoneSaisie (le champ combo, avec états visuels), LibelléLignes (le texte des items du popup) et LigneSélectionnée (l'item surligné). Appliqué via 'COMBO_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Combo field (drop-down list). Struct of sub-elements: Label (embedded label: color, font), OuterFrame (wrapping container), InputZone (the combo field, with visual states), RowLabel (popup item text) and SelectedRow (highlighted item). Applied via 'COMBO_X..Style = STY_Y'.",
      .fields         = FIELDS_styleCombo,
      .nameEN         = "styleCombobox",
      .genre          = OWL_GENRE_M },   /* le style de combo */
    { .canonical      = "styleQuadrillage",
      .canonicalAscii = "styleQuadrillage",
      .aliases        = ALIAS_styleQuadrillage,
      .category       = "Affichage",
      .descFR         = "Sous-élément des séparateurs (grille) d'un styleTable. Une seule feuille : Couleur (couleur des lignes de séparation ; -1 = défaut du thème). Statique.",
      .descEN         = "Gridline separator sub-element of a styleTable. A single leaf: Color (separator line color; -1 = theme default). Static.",
      .fields         = FIELDS_styleQuadrillage,
      .nameEN         = "styleGridlines",
      .genre          = OWL_GENRE_M },   /* le quadrillage */
    { .canonical      = "styleTable",
      .canonicalAscii = "styleTable",
      .aliases        = ALIAS_styleTable,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Table (tableau). Struct de sous-éléments : CadreExtérieur (le cadre et le fond), TitreColonne (le texte des en-têtes), LibelléLignes (le texte des cellules), LignesPaires et LignesImpaires (le fond alterné des lignes), LigneSélectionnée et LigneSurvolée (le texte et le fond des lignes sous interaction) et Quadrillage (la couleur des séparateurs). Seul le CadreExtérieur porte des surcharges par état (survol/focus/appuyé/grisé, via Etats) ; les autres sous-éléments sont statiques. Appliqué via 'TAB_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Table field. Struct of sub-elements: OuterFrame (frame and background), ColumnTitle (header text), RowLabel (cell text), EvenRows and OddRows (alternating row background), SelectedRow and HoveredRow (text and background of rows under interaction) and Gridlines (separator color). Only the OuterFrame carries per-state overrides (hover/focus/pressed/disabled, via Etats); the other sub-elements are static. Applied via 'TAB_X..Style = STY_Y'.",
      .fields         = FIELDS_styleTable,
      .nameEN         = "styleTable",
      .genre          = OWL_GENRE_M },   /* le style de table */
    { .canonical      = "styleJauge",
      .canonicalAscii = "styleJauge",
      .aliases        = ALIAS_styleJauge,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Jauge (barre de progression). Struct de sous-éléments : CadreExtérieur (la piste, la bordure et le fond), Remplissage (la barre de progression : couleur de la portion remplie) et Libellé (le texte du pourcentage : couleur, police). Seul le CadreExtérieur porte des surcharges par état (survol/focus/appuyé/grisé, via Etats) ; les autres sous-éléments sont statiques. Appliqué via 'JAU_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Gauge field (progress bar). Struct of sub-elements: OuterFrame (track, border and background), Fill (the progress bar: filled portion color) and Label (percentage text: color, font). Only the OuterFrame carries per-state overrides (hover/focus/pressed/disabled, via Etats); the other sub-elements are static. Applied via 'JAU_X..Style = STY_Y'.",
      .fields         = FIELDS_styleJauge,
      .nameEN         = "styleGauge",
      .genre          = OWL_GENRE_F },   /* la jauge → F */
    { .canonical      = "styleColonne",
      .canonicalAscii = "styleColonne",
      .aliases        = ALIAS_styleColonne,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé d'une Colonne de Table : surcharge locale du styleTable, colonne par colonne. Sous-ensemble de styleTable sans CadreExtérieur ni Quadrillage (propres au tableau entier) : TitreColonne, LibelléLignes, LignesPaires, LignesImpaires, LigneSélectionnée, LigneSurvolée. Une feuille non renseignée retombe sur le styleTable. Appliqué via 'COL_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor of a Table Column: local override of the styleTable, per column. Subset of styleTable without OuterFrame or Gridlines (specific to the whole table): ColumnTitle, RowLabel, EvenRows, OddRows, SelectedRow, HoveredRow. An unset leaf falls back to the styleTable. Applied via 'COL_X..Style = STY_Y'.",
      .fields         = FIELDS_styleColonne,
      .nameEN         = "styleColumn",
      .genre          = OWL_GENRE_F },   /* le style de colonne (la colonne → F) */
    { .canonical      = "styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e",   /* styleZoneRépétée */
      .canonicalAscii = "styleZoneRepetee",
      .aliases        = ALIAS_styleZoneRepetee,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Zone répétée (conteneur à répétition). Struct de sous-éléments : CadreExtérieur (le cadre et le fond du champ entier), CadreInterne (le cadre de chaque répétition), LignesImpaires et LignesPaires (le fond alterné des répétitions), LigneSélectionnée et LigneSurvolée (le texte et le fond des répétitions sous interaction) et Séparateur (la couleur du trait entre répétitions). Défauts sobres : fonds transparents, cadres invisibles. Appliqué via 'ZR_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Looper field (repeating container). Struct of sub-elements: OuterFrame (whole control frame and background), InnerFrame (per-repetition frame), OddRows and EvenRows (alternating repetition background), SelectedRow and HoveredRow (text and background of repetitions under interaction) and Separator (line color between repetitions). Sober defaults: transparent backgrounds, invisible frames. Applied via 'ZR_X..Style = STY_Y'.",
      .fields         = FIELDS_styleZoneRepetee,
      .nameEN         = "styleLooper",
      .genre          = OWL_GENRE_F },   /* le style de zone répétée (la zone → F) */
    { .canonical      = "styleCadreExt\xc3\xa9rieur",   /* styleCadreExtérieur */
      .canonicalAscii = "styleCadreExterieur",
      .aliases        = ALIAS_styleCadreExterieur,
      .category       = "Affichage",
      .descFR         = "Sous-élément conteneur d'un champ stylé : enveloppe le contenu (libellé, zone de saisie…). Un cadre n'a pas de texte : props de base CouleurFond, Retraits et Cadre + membre Etats (surcharges par état, indexé via [survol/appuyé/focus/grisé]).",
      .descEN         = "Container sub-element of a styled control: wraps the content (label, input zone…). A frame has no text: base props BackgroundColor, paddings and Border + Etats member (per-state overrides, indexed via [hover/pressed/focus/disabled]).",
      .fields         = FIELDS_styleCadreExterieur,
      .nameEN         = "styleOuterFrame",
      .genre          = OWL_GENRE_M },   /* le cadre extérieur */
    { .canonical      = "styleCadreExterieurBase",
      .canonicalAscii = "styleCadreExterieurBase",
      .aliases        = ALIAS_styleCadreExterieurBase,
      .category       = "Affichage",
      .descFR         = "Style plat d'une surcharge par état du cadre extérieur (ce que désigne ..Etats[état]). Props de base seules (CouleurFond, Retraits, Cadre, Ombre, Dégradé) : ni Etats, ni texte (fin de chaîne).",
      .descEN         = "Flat style of a per-state override of the outer frame (what ..Etats[state] targets). Base props only (BackgroundColor, paddings, Border, Shadow, Gradient): no Etats, no text (end of chain).",
      .fields         = FIELDS_styleCadreExterieurBase,
      .nameEN         = "styleOuterFrameBase",
      .genre          = OWL_GENRE_M },   /* le style de base */
    { .canonical      = "styleCadreExterieurEtats",
      .canonicalAscii = "styleCadreExterieurEtats",
      .aliases        = ALIAS_styleCadreExterieurEtats,
      .category       = "Affichage",
      .descFR         = "Collection des surcharges par état du cadre extérieur. Indexée par l'état via ..Etats[survol/appuyé/focus/grisé] ; chaque état est un styleCadreExterieurBase.",
      .descEN         = "Collection of per-state overrides of the outer frame. Indexed by state via ..Etats[hover/pressed/focus/disabled]; each state is a styleCadreExterieurBase.",
      .fields         = NULL,  /* collection : slots internes _Survol/... indexes via [etat] */
      .nameEN         = "styleOuterFrameStates",
      .genre          = OWL_GENRE_M },   /* le jeu d'états */
    { .canonical      = "styleOmbre",
      .canonicalAscii = "styleOmbre",
      .aliases        = ALIAS_styleOmbre,
      .category       = "Affichage",
      .descFR         = "Sous-objet ombre portée d'un styleCadreExtérieur (l'ombre du champ entier). Props : Couleur (de l'ombre ou du glow), DécalageX et DécalageY (offset), Flou (rayon ; 0 = pas d'ombre), Étendue (épaississement). Glow = ombre colorée, décalages 0, gros flou.",
      .descEN         = "Drop shadow sub-object of a styleCadreExtérieur (the whole control shadow). Props: Color (shadow or glow color), OffsetX and OffsetY (offset), Blur (radius; 0 = no shadow), Spread (spread). Glow = colored shadow, zero offsets, large blur.",
      .fields         = FIELDS_styleOmbre,
      .nameEN         = "styleShadow",
      .genre          = OWL_GENRE_F },   /* l'ombre */
    { .canonical      = "styleD\xc3\xa9grad\xc3\xa9",   /* styleDégradé */
      .canonicalAscii = "styleDegrade",
      .aliases        = ALIAS_styleDegrade,
      .category       = "Affichage",
      .descFR         = "Sous-objet dégradé de fond d'un styleCadreExtérieur (le fond dégradé du champ entier). Props : Type (dégradéAucun/dégradéLinéaire/dégradéRadial ; défaut dégradéAucun = pas de dégradé), Angle (0 = horizontal, 90 = vertical ; ignoré en radial), CouleurDébut et CouleurFin (arrêts). Le dégradé actif prime sur CouleurFond.",
      .descEN         = "Background gradient sub-object of a styleCadreExtérieur (the whole control gradient fill). Props: Type (gradientNone/gradientLinear/gradientRadial; default gradientNone = no gradient), Angle (0 = horizontal, 90 = vertical; ignored when radial), StartColor and EndColor (stops). An active gradient overrides BackgroundColor.",
      .fields         = FIELDS_styleDegrade,
      .nameEN         = "styleGradient",
      .genre          = OWL_GENRE_M },   /* le dégradé */
    { .canonical      = "styleBouton",
      .canonicalAscii = "styleBouton",
      .aliases        = ALIAS_styleBouton,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Bouton. Struct de sous-éléments : Libellé (le texte, avec états visuels), Image (l'icône : source Image + Position) et CadreExtérieur (le conteneur, surcharges par état via Etats). États visuels via 'Libellé..Etats[survol/appuyé/focus/grisé]'. Appliqué via 'BTN_X..Style = STY_Y'.",
      .descEN         = "Typed style descriptor for the Button field. Struct of sub-elements: Caption (the text, with visual states), Image (the icon: Image source + Position) and OuterFrame (the container, per-state overrides via Etats). Visual states via 'Caption..Etats[hover/pressed/focus/disabled]'. Applied via 'BTN_X..Style = STY_Y'.",
      .fields         = FIELDS_styleBouton,
      .nameEN         = "styleButton",
      .genre          = OWL_GENRE_M },   /* le style de bouton */
    { .canonical      = "styleImageBouton",
      .canonicalAscii = "styleImageBouton",
      .aliases        = ALIAS_styleImageBouton,
      .category       = "Affichage",
      .descFR         = "Sous-élément icône d'un styleBouton. Deux feuilles : Image (source de l'icône — valeur Image, chemin de fichier ou Icône, équivalente au raccourci plat ..Image) et Position (position par rapport au texte, contrainte à positionImageBouton).",
      .descEN         = "Icon sub-element of a styleBouton. Two leaves: Image (icon source — Image value, file path or Icon, equivalent to the flat shortcut ..Image) and Position (position relative to the text, constrained to positionImageBouton).",
      .fields         = FIELDS_styleImageBouton,
      .nameEN         = "styleButtonImage",
      .genre          = OWL_GENRE_F },   /* l'image du bouton */
    { .canonical      = "styleImageBoutonBase",
      .canonicalAscii = "styleImageBoutonBase",
      .aliases        = ALIAS_styleImageBoutonBase,
      .category       = "Affichage",
      .descFR         = "Feuille plate d'une surcharge d'icône par état (ce que désigne ..Etats[état] sur l'icône du bouton). Une seule feuille Image (source de l'icône substituée), pas de membre Etats (fin de chaîne).",
      .descEN         = "Flat leaf of a per-state icon override (what ..Etats[state] targets on the button icon). Single Image leaf (substituted icon source), no Etats member (end of chain).",
      .fields         = FIELDS_styleImageBoutonBase,
      .nameEN         = "styleButtonImageBase",
      .genre          = OWL_GENRE_F },   /* l'image de l'état */
    { .canonical      = "styleImageBoutonEtats",
      .canonicalAscii = "styleImageBoutonEtats",
      .aliases        = ALIAS_styleImageBoutonEtats,
      .category       = "Affichage",
      .descFR         = "Collection des surcharges d'icône par état du bouton. Indexée via ..Etats[survol/grisé] ; chaque état est un styleImageBoutonBase. Seuls survol et grisé sont offerts pour l'icône.",
      .descEN         = "Collection of per-state icon overrides of the button. Indexed via ..Etats[hover/disabled]; each state is a styleImageBoutonBase. Only hover and disabled are offered for the icon.",
      .fields         = NULL,  /* collection : slots internes _Survol/_Grise indexes via [etat] */
      .nameEN         = "styleButtonImageStates",
      .genre          = OWL_GENRE_M },   /* le jeu d'états d'icône */
    { .canonical      = "styleZoneBouton",
      .canonicalAscii = "styleZoneBouton",
      .aliases        = ALIAS_styleZoneBouton,
      .category       = "Affichage",
      .descFR         = "Sous-élément bouton d'un styleBouton. Props de base (Couleur, CouleurFond, Retraits, Police, Cadre) + membre Etats (surcharges par état, indexé via [survol/appuyé/focus/grisé]).",
      .descEN         = "Button sub-element of a styleBouton. Base props (Color, BackgroundColor, paddings, Font, Border) + Etats member (per-state overrides, indexed via [hover/pressed/focus/disabled]).",
      .fields         = FIELDS_styleZoneBouton,
      .nameEN         = "styleButtonZone",
      .genre          = OWL_GENRE_M },   /* le style de bouton */
    { .canonical      = "styleZoneBoutonBase",
      .canonicalAscii = "styleZoneBoutonBase",
      .aliases        = ALIAS_styleZoneBoutonBase,
      .category       = "Affichage",
      .descFR         = "Style plat d'une surcharge par état du bouton (ce que désigne ..Etats[état]). Props de base seules : ni Etats, ni sous-éléments (fin de chaîne).",
      .descEN         = "Flat style of a per-state override of the button (what ..Etats[state] targets). Base props only: no Etats, no sub-elements (end of chain).",
      .fields         = FIELDS_styleZoneBoutonBase,
      .nameEN         = "styleButtonZoneBase",
      .genre          = OWL_GENRE_M },   /* le style de base */
    { .canonical      = "styleZoneBoutonEtats",
      .canonicalAscii = "styleZoneBoutonEtats",
      .aliases        = ALIAS_styleZoneBoutonEtats,
      .category       = "Affichage",
      .descFR         = "Collection des surcharges par état du bouton. Indexée par l'état via ..Etats[survol/appuyé/focus/grisé] ; chaque état est un styleZoneBoutonBase.",
      .descEN         = "Collection of per-state overrides of the button. Indexed by state via ..Etats[hover/pressed/focus/disabled]; each state is a styleZoneBoutonBase.",
      .fields         = NULL,  /* collection : slots internes _Survol/... indexes via [etat] */
      .nameEN         = "styleButtonZoneStates",
      .genre          = OWL_GENRE_M },   /* le jeu d'états */
    /*  (ticket C) — styleMenuPrincipal : style typé du menu classique
     * de fenetre. 3 surfaces (Barre/SousMenu/OptionMenu) + Base/Etats sur les
     * surfaces stateful (Barre, OptionMenu). */
    { .canonical      = "styleMenuPrincipal",
      .canonicalAscii = "styleMenuPrincipal",
      .aliases        = ALIAS_styleMenuPrincipal,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du menu classique de fenêtre. Composite de 3 surfaces : Barre (la bande horizontale : couleur de fond, couleur du texte, police), SousMenu (le panneau déroulant : couleur de fond, couleur de séparateur) et OptionMenu (une rangée de menu : couleur de fond, couleur du texte, police). La Barre et OptionMenu portent une surcharge de l'état survol via Etats ; le SousMenu est statique. Toute propriété non posée retombe sur le défaut du thème. Appliqué via un bloc 'Style' dans le MenuPrincipal.",
      .descEN         = "Typed style descriptor for the classic window menu. Composite of 3 surfaces: Bar (the horizontal strip: background color, text color, font), Submenu (the drop-down panel: background color, separator color) and MenuOption (a menu row: background color, text color, font). Bar and MenuOption carry a hover-state override via Etats; Submenu is static. Any unset property falls back to the theme default. Applied via a 'Style' block inside the MenuPrincipal.",
      .fields         = FIELDS_styleMenuPrincipal,
      .nameEN         = "styleMainMenu",
      .genre          = OWL_GENRE_M },   /* le style de menu */
    { .canonical      = "styleMenuPrincipalBarre",
      .canonicalAscii = "styleMenuPrincipalBarre",
      .aliases        = ALIAS_styleMenuPrincipalBarre,
      .category       = "Affichage",
      .descFR         = "Surface barre d'un styleMenuPrincipal (la bande horizontale). Props de base Couleur, CouleurFond, Police + membre Etats (surcharge de l'état survol).",
      .descEN         = "Bar surface of a styleMenuPrincipal (the horizontal strip). Base props Color, BackgroundColor, Font + Etats member (hover-state override).",
      .fields         = FIELDS_styleMenuPrincipalBarre,
      .nameEN         = "styleMainMenuBar",
      .genre          = OWL_GENRE_F },   /* la barre */
    { .canonical      = "styleMenuPrincipalBarreBase",
      .canonicalAscii = "styleMenuPrincipalBarreBase",
      .aliases        = ALIAS_styleMenuPrincipalBarreBase,
      .category       = "Affichage",
      .descFR         = "Style plat d'une surcharge par état de la barre (ce que désigne Barre..Etats[survol]). Couleurs seules (Couleur, CouleurFond) : pas de Police par état, pas de membre Etats (fin de chaîne).",
      .descEN         = "Flat style of a per-state override of the bar (what Bar..Etats[hover] targets). Colors only (Color, BackgroundColor): no per-state font, no Etats member (end of chain).",
      .fields         = FIELDS_styleMenuPrincipalBarreBase,
      .nameEN         = "styleMainMenuBarBase",
      .genre          = OWL_GENRE_M },   /* le style de base */
    { .canonical      = "styleMenuPrincipalBarreEtats",
      .canonicalAscii = "styleMenuPrincipalBarreEtats",
      .aliases        = ALIAS_styleMenuPrincipalBarreEtats,
      .category       = "Affichage",
      .descFR         = "Collection des surcharges par état de la barre. Indexée par l'état via ..Etats[survol] ; chaque état est un styleMenuPrincipalBarreBase.",
      .descEN         = "Collection of per-state overrides of the bar. Indexed by state via ..Etats[hover]; each state is a styleMenuPrincipalBarreBase.",
      .fields         = NULL,  /* collection : slot interne _Survol indexe via [etat] */
      .nameEN         = "styleMainMenuBarStates",
      .genre          = OWL_GENRE_M },   /* le jeu d'états */
    { .canonical      = "styleMenuPrincipalSousMenu",
      .canonicalAscii = "styleMenuPrincipalSousMenu",
      .aliases        = ALIAS_styleMenuPrincipalSousMenu,
      .category       = "Affichage",
      .descFR         = "Surface du panneau déroulant d'un styleMenuPrincipal (le conteneur des rangées). Statique : CouleurFond (fond du panneau), CouleurSéparateur (trait entre les rangées), Arrondi / Epaisseur / CouleurBordure (liseré du panneau).",
      .descEN         = "Drop-down panel surface of a styleMenuPrincipal (the row container). Static: BackgroundColor (panel background), SeparatorColor (line between rows), CornerRadius / Thickness / BorderColor (panel border).",
      .fields         = FIELDS_styleMenuPrincipalSousMenu,
      .nameEN         = "styleMainMenuSubmenu",
      .genre          = OWL_GENRE_M },   /* le sous-menu */
    { .canonical      = "styleMenuPrincipalOption",
      .canonicalAscii = "styleMenuPrincipalOption",
      .aliases        = ALIAS_styleMenuPrincipalOption,
      .category       = "Affichage",
      .descFR         = "Surface d'une rangée de menu d'un styleMenuPrincipal (option terminale ou rangée ouvrant un sous-menu). Props de base Couleur, CouleurFond, Police + membre Etats (surcharge de l'état survol).",
      .descEN         = "Menu row surface of a styleMenuPrincipal (terminal option or sub-menu-opening row). Base props Color, BackgroundColor, Font + Etats member (hover-state override).",
      .fields         = FIELDS_styleMenuPrincipalOption,
      .nameEN         = "styleMainMenuOption",
      .genre          = OWL_GENRE_F },   /* l'option */
    { .canonical      = "styleMenuPrincipalOptionBase",
      .canonicalAscii = "styleMenuPrincipalOptionBase",
      .aliases        = ALIAS_styleMenuPrincipalOptionBase,
      .category       = "Affichage",
      .descFR         = "Style plat d'une surcharge par état d'une rangée (ce que désigne OptionMenu..Etats[survol]). Couleurs seules (Couleur, CouleurFond) : pas de Police par état, pas de membre Etats (fin de chaîne).",
      .descEN         = "Flat style of a per-state override of a row (what MenuOption..Etats[hover] targets). Colors only (Color, BackgroundColor): no per-state font, no Etats member (end of chain).",
      .fields         = FIELDS_styleMenuPrincipalOptionBase,
      .nameEN         = "styleMainMenuOptionBase",
      .genre          = OWL_GENRE_M },   /* le style de base */
    { .canonical      = "styleMenuPrincipalOptionEtats",
      .canonicalAscii = "styleMenuPrincipalOptionEtats",
      .aliases        = ALIAS_styleMenuPrincipalOptionEtats,
      .category       = "Affichage",
      .descFR         = "Collection des surcharges par état d'une rangée. Indexée par l'état via ..Etats[survol] ; chaque état est un styleMenuPrincipalOptionBase.",
      .descEN         = "Collection of per-state overrides of a row. Indexed by state via ..Etats[hover]; each state is a styleMenuPrincipalOptionBase.",
      .fields         = NULL,  /* collection : slot interne _Survol indexe via [etat] */
      .nameEN         = "styleMainMenuOptionStates",
      .genre          = OWL_GENRE_M },   /* le jeu d'états */
    /*  — styleMenuContextuel : style typé du menu contextuel (clic
     * droit). 2 surfaces SEULEMENT (SousMenu/OptionMenu), reutilise les surfaces
     * de styleMenuPrincipal ; pas de Barre. */
    { .canonical      = "styleMenuContextuel",
      .canonicalAscii = "styleMenuContextuel",
      .aliases        = ALIAS_styleMenuContextuel,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du menu contextuel (clic droit). Composite de 2 surfaces : SousMenu (le panneau déroulant : couleur de fond, couleur de séparateur, bordure) et OptionMenu (une rangée de menu : couleur de fond, couleur du texte, police). OptionMenu porte une surcharge de l'état survol via Etats ; le SousMenu est statique. Contrairement au menu classique, il n'y a pas de surface Barre (un menu contextuel n'a pas de bande horizontale). Toute propriété non posée retombe sur le défaut du thème. Appliqué via un bloc 'Style' dans le MenuContextuel.",
      .descEN         = "Typed style descriptor for the context menu (right-click). Composite of 2 surfaces: Submenu (the drop-down panel: background color, separator color, border) and MenuOption (a menu row: background color, text color, font). MenuOption carries a hover-state override via Etats; Submenu is static. Unlike the classic menu, there is no Bar surface (a context menu has no horizontal strip). Any unset property falls back to the theme default. Applied via a 'Style' block inside the MenuContextuel.",
      .fields         = FIELDS_styleMenuContextuel,
      .nameEN         = "styleContextMenu",
      .genre          = OWL_GENRE_M },   /* le style de menu contextuel */
    /*  (lot 5d) — styleOnglet : style typé du Champ Onglet. Racine
     * CouleurFond (fond du pane) + 2 surfaces VoletActif/VoletInactif (meme type
     * styleVolet, stateful survol). */
    { .canonical      = "styleOnglet",
      .canonicalAscii = "styleOnglet",
      .aliases        = ALIAS_styleOnglet,
      .category       = "Affichage",
      .descFR         = "Descripteur de style typé du Champ Onglet. Trois sous-éléments : CadreExtérieur (fond et bordure du pane, la zone sous les languettes ; type styleCadreExtérieur) + deux surfaces VoletActif (la languette sélectionnée) et VoletInactif (les languettes non sélectionnées), de même type styleVolet. Chaque volet porte Couleur (texte), CouleurFond, Police et une surcharge de l'état survol via Etats. Toute propriété non posée retombe sur le défaut du thème. Appliqué via un bloc 'Style' dans le Champ Onglet.",
      .descEN         = "Typed style descriptor for the Tab field. Three sub-elements: OuterFrame (pane background and border, the area below the tabs; type styleCadreExtérieur) + two surfaces ActivePane (the selected tab) and InactivePane (the unselected tabs), both of type styleVolet. Each pane carries Color (text), BackgroundColor, Font and a hover-state override via Etats. Any unset property falls back to the theme default. Applied via a 'Style' block inside the Tab field.",
      .fields         = FIELDS_styleOnglet,
      .nameEN         = "styleTab",
      .genre          = OWL_GENRE_M },   /* le style d'onglet */
    { .canonical      = "styleCadreExt\xc3\xa9rieurOnglet",   /* styleCadreExtérieurOnglet */
      .canonicalAscii = "styleCadreExterieurOnglet",
      .aliases        = ALIAS_styleCadreExterieurOnglet,
      .category       = "Affichage",
      .descFR         = "CadreExtérieur restreint du pane d'un Champ Onglet : fond et bordure de la zone sous les languettes. Props : CouleurFond + Cadre (Couleur/Epaisseur/Arrondi). Sans état, dégradé, ombre ni retraits (contrairement au styleCadreExtérieur complet des autres champs).",
      .descEN         = "Restricted outer frame of a Tab field's pane: background and border of the area below the tabs. Props: BackgroundColor + Border (Color/Thickness/CornerRadius). No state, gradient, shadow nor paddings (unlike the full styleCadreExtérieur of other controls).",
      .fields         = FIELDS_styleCadreExterieurOnglet,
      .nameEN         = "styleTabOuterFrame",
      .genre          = OWL_GENRE_M },   /* le cadre extérieur */
    { .canonical      = "styleVolet",
      .canonicalAscii = "styleVolet",
      .aliases        = ALIAS_styleVolet,
      .category       = "Affichage",
      .descFR         = "Surface d'un volet d'un styleOnglet (la languette et son en-tête). Type partagé par VoletActif et VoletInactif. Props de base Couleur, CouleurFond, Police, Arrondi (rayon des coins) + membre Etats (surcharge de l'état survol).",
      .descEN         = "Pane surface of a styleOnglet (the tab and its header). Type shared by ActivePane and InactivePane. Base props Color, BackgroundColor, Font, CornerRadius (corner radius) + Etats member (hover-state override).",
      .fields         = FIELDS_styleVolet,
      .nameEN         = "stylePane",
      .genre          = OWL_GENRE_M },   /* le style de volet */
    { .canonical      = "styleVoletBase",
      .canonicalAscii = "styleVoletBase",
      .aliases        = ALIAS_styleVoletBase,
      .category       = "Affichage",
      .descFR         = "Style plat d'une surcharge par état d'un volet (ce que désigne VoletActif..Etats[survol]). Couleurs seules (Couleur, CouleurFond) : pas de Police par état, pas de membre Etats (fin de chaîne).",
      .descEN         = "Flat style of a per-state override of a pane (what ActivePane..Etats[hover] targets). Colors only (Color, BackgroundColor): no per-state font, no Etats member (end of chain).",
      .fields         = FIELDS_styleVoletBase,
      .nameEN         = "stylePaneBase",
      .genre          = OWL_GENRE_M },   /* le style de base */
    { .canonical      = "styleVoletEtats",
      .canonicalAscii = "styleVoletEtats",
      .aliases        = ALIAS_styleVoletEtats,
      .category       = "Affichage",
      .descFR         = "Collection des surcharges par état d'un volet. Indexée par l'état via ..Etats[survol] ; chaque état est un styleVoletBase.",
      .descEN         = "Collection of per-state overrides of a pane. Indexed by state via ..Etats[hover]; each state is a styleVoletBase.",
      .fields         = NULL,  /* collection : slot interne _Survol indexe via [etat] */
      .nameEN         = "stylePaneStates",
      .genre          = OWL_GENRE_M },   /* le jeu d'états */
    { .canonical      = "Projet",
      .canonicalAscii = "Projet",
      .aliases        = ALIAS_Projet,
      .category       = "Projet",
      .descFR         = "Métadonnées déclaratives du projet, écrites dans le fichier projet.owl sous la forme 'MonApp est un Projet PROPRIETES ... FIN FIN'. L'identifiant choisi dans la déclaration (ici MonApp) devient une variable globale du projet, lisible en cascade depuis un autre fichier via ce même nom : MonApp..Auteur. Catalogue figé : Langues (liste de constantes langueXxx, la 1ère étant la langue par défaut) est obligatoire — au moins une langue ; Description, Auteur, Licence (chaînes) sont optionnels ; Thème (optionnel) est une référence typée vers une instance « X est un Thème » déclarée dans un fichier voisin (ex. theme.owl), pas une chaîne. Un champ hors catalogue est rejeté à la compilation.",
      .descEN         = "Declarative project metadata, written in the projet.owl file as 'MyApp is a Project PROPERTIES ... END END'. The identifier chosen in the declaration (here MyApp) becomes a project-wide global, readable via cascade from another file through that same name: MyApp..Author. Fixed catalog: Langues (list of langueXxx constants, the first being the default language) is mandatory — at least one language; Description, Auteur, Licence (strings) are optional; Theme (optional) is a typed reference to an instance 'X is a Theme' declared in a neighbor file (e.g. theme.owl), not a string. An unknown field is rejected at compile time.",
      .fields         = FIELDS_Projet,
      .nameEN         = "Project",
      .genre          = OWL_GENRE_M },   /* le projet */
    { .canonical  = "Chrono",
      .category   = "Chrono",
      .descFR     = "Chronomètre (avec pause/reprise).",
      .descEN     = "Stopwatch (with pause/resume).",
      .fields     = FIELDS_Chrono,
      .nameEN     = "Chrono",
      .genre      = OWL_GENRE_M },   /* le chrono(mètre) */
    { .canonical  = "Timer",
      .category   = "Timer",
      .descFR     = "Minuteur périodique : appelle une procédure sans paramètre toutes les ..Intervalle millisecondes pendant une boucle d'événements. ..Répète = Faux pour un déclenchement unique. Piloté par TimerDémarre / TimerArrête (ou les méthodes .Démarre() / .Arrête()).",
      .descEN     = "Periodic timer: calls a parameterless procedure every ..Interval milliseconds during an event loop. ..Repeat = False for a single trigger. Driven by TimerStart / TimerStop (or the .Start() / .Stop() methods).",
      .fields     = FIELDS_Timer,
      .onFieldSet = owl_timer_on_field_set,
      .nameEN     = "Timer",
      .genre      = OWL_GENRE_M },   /* le timer / minuteur */
    { .canonical  = "UUID",
      .category   = "UUID",
      .descFR     = "Identifiant unique de taille fixe 128 bits — c'est l'unique taille du type : il n'existe pas de variante 256 bits. Versions supportées : v4 (aléatoire, RFC 4122) et v7 (timestamp epoch-ms + aléatoire, RFC 9562, sortable lexicographiquement) ; pas d'autre version (v1/v3/v5). Construit via DonneUUID() ou DonneUUID(uuidV7). Sérialisation via .VersChaîne([uuidFormatStandard | uuidFormatCompact | uuidFormatAccolades]) — 36, 32 ou 38 caractères. Représentation interne opaque (16 octets, non exposés directement). Pour un jeton ou une clé aléatoire de taille arbitraire (256 bits ou plus), utiliser HasardSécuriséOctets(n).",
      .descEN     = "Fixed-size 128-bit unique identifier — 128 bits is the type's only size: there is no 256-bit variant. Supported versions: v4 (random, RFC 4122) and v7 (epoch-ms timestamp + random, RFC 9562, lexicographically sortable); no other version (v1/v3/v5). Built via GetUUID() or GetUUID(uuidV7). Serialization via .ToString([uuidFormatStandard | uuidFormatCompact | uuidFormatBraces]) — 36, 32, or 38 characters. Opaque internal representation (16 bytes, not directly exposed). For an arbitrary-size random token or key (256-bit and beyond), use SecureRandomBytes(n).",
      .fields     = NULL,
      .coercible  = true,
      .nameEN     = "UUID",
      /*  — genre lu par l'article EN->FR : sans lui la declaration
       * revient en `est a UUID`. 57 `est un` releves dans le depot. */
      .genre      = OWL_GENRE_M },   /* un identifiant */
    { .canonical  = "Image",
      .category   = "Multimedia",
      .descFR     = "Variable Image : représentation interne d'une image raster RGBA. Formats de chargement supportés : PNG, JPEG, BMP, GIF (premier frame), TGA, PSD. Manipulable via les fonctions de dessin (dStylo, dPoint, dLigne, etc.) et la syntaxe MaImage[x, y] pour l'accès pixel.",
      .descEN     = "Image variable: internal representation of a raster RGBA image. Supported load formats: PNG, JPEG, BMP, GIF (first frame), TGA, PSD. Manipulable via drawing functions (dPen, dDot, dLine, etc.) and the MyImage[x, y] syntax for pixel access.",
      .fields     = FIELDS_Image,
      .coercible  = true,
      .nameEN     = "Image",
      .genre      = OWL_GENRE_F },   /* une image */
    { .canonical  = "FichierDisque",
      .category   = "Fichiers",
      .descFR     = "Fichier texte sur disque.",
      .descEN     = "Text file on disk.",
      .fields     = FIELDS_FichierDisque,
      .coercible  = true,
      .aliases    = ALIAS_FichierDisque,
      .nameEN     = "DiskFile",
      .genre      = OWL_GENRE_M },   /* le fichier disque */
    { .canonical      = "FichierT\xc3\xa9l\xc3\xa9vers\xc3\xa9",   /* FichierTéléversé */
      .canonicalAscii = "FichierTeleverse",
      .category       = "HTTP",
      .descFR         = "Fichier reçu via un formulaire multipart en paramètre direct de surRequête. Métadonnées (NomFichier/Taille/TypeMime) lisibles sans lire le contenu.",
      .descEN         = "File received through a multipart form as a direct surRequete parameter. Metadata (FileName/Size/MimeType) readable without reading the content.",
      .fields         = FIELDS_FichierTeleverse,
      .aliases        = ALIAS_FichierTeleverse,
      .nameEN         = "UploadedFile",
      .genre          = OWL_GENRE_M },   /* le fichier téléversé */
    { .canonical  = "Definition",
      .category   = "Introspection",
      .descFR     = "Description runtime d'un type (retournée par RécupèreDéfinition).",
      .descEN     = "Runtime description of a type (returned by GetDefinition).",
      .fields     = FIELDS_Definition,
      .nameEN     = "Definition",
      .genre      = OWL_GENRE_F },   /* une définition */
    { .canonical    = "MockNative",
      .category     = "Test",
      .descFR       = "Simulation d'une fonction native, créée par MockCrée, pour intercepter et vérifier ses appels dans les tests.",
      .descEN       = "A mock of a native function, created by MockCreate, used to intercept and check its calls in tests.",
      .fields       = NULL,
      .hasDefault   = true,
      .defaultValue = NIL_VAL,
      .nameEN       = "MockNative",
      .genre        = OWL_GENRE_M },   /* un mock — 194 `est un` releves */
    { .canonical      = "FormatNum\xc3\xa9" "rique",   /* FormatNumérique */
      .canonicalAscii = "FormatNumerique",
      .aliases        = ALIAS_FormatNumerique,
      .category       = "FormatNum\xc3\xa9" "rique",
      .descFR         = "Type structuré pour configurer NumériqueVersChaîne (alignement, signe, séparateurs, notation, préfixe…). Alternative au masque chaîne printf-like.",
      .descEN         = "Structured type for NumToString (alignment, sign, separators, notation, prefix…). Alternative to printf-like string mask.",
      .fields         = FIELDS_FormatNumerique,
      .nameEN         = "NumberFormat",
      .genre          = OWL_GENRE_M },   /* le format numérique */
    { .canonical      = "FormatNombre",
      .canonicalAscii = "FormatNombre",
      .category       = "FormatNombre",
      .descFR         = "Format d'affichage d'un nombre : séparateur décimal, séparateur de milliers, nombre de décimales. Porté par le projet (hérité par les champs) ou posé directement sur un champ.",
      .descEN         = "Display format of a number: decimal separator, thousands separator, decimal count. Carried by the project (inherited by controls) or set directly on a control.",
      .fields         = FIELDS_FormatNombre,
      /* nameEN distinct de "NumberFormat" : ce dernier est deja le canon EN de
       * FormatNumérique (type conversion). Deux types ne peuvent pas partager la
       * meme forme EN (le translate round-trip EN->FR deviendrait ambigu). Le
       * champ Projet et la propriete Saisie gardent, eux, l'ecriture bilingue
       * FormatNombre/NumberFormat (portee par conteneur, sans collision). */
      .nameEN         = "NumberDisplayFormat",
      .genre          = OWL_GENRE_M },   /* le format nombre */
    { .canonical      = "FormatMon\xc3\xa9taire",   /* FormatMonétaire */
      .canonicalAscii = "FormatMonetaire",
      .category       = "FormatMon\xc3\xa9taire",
      .descFR         = "Format d'affichage d'un montant : symbole et sa position, séparateur décimal, séparateur de milliers, nombre de décimales, motif du négatif. Porté par le projet (hérité par les champs) ou posé directement sur un champ.",
      .descEN         = "Display format of a currency amount: symbol and its position, decimal separator, thousands separator, decimal count, negative pattern. Carried by the project (inherited by controls) or set directly on a control.",
      .fields         = FIELDS_FormatMonetaire,
      .nameEN         = "CurrencyFormat",
      .genre          = OWL_GENRE_M },   /* le format monétaire */
    { .canonical      = "dur\xc3\xa9" "e",   /* durée */
      .canonicalAscii = "duree",
      .category       = "Temporel",
      .descFR         = "Intervalle de temps. Au runtime, une valeur entière en millisecondes. Un littéral se construit avec les composantes fines Jour/Heure/Minute/Seconde/Milliseconde (`{ Heure = 2, Minute = 30 }`) ; la lecture des totaux se fait via les accesseurs EnJours/EnHeures/EnMinutes/EnSecondes/EnMillisecondes.",
      .descEN         = "Time interval. At runtime, an integer number of milliseconds. A literal is built with the fine components Day/Hour/Minute/Second/Millisecond (`{ Hour = 2, Minute = 30 }`) ; reading totals is done via the InDays/InHours/InMinutes/InSeconds/InMilliseconds accessors.",
      .fields         = FIELDS_Duree,
      .primitiveLiteral = true,  /* durée = primitif ; fields = métadonnée du littéral */
      .nameEN         = "duration",
      .genre          = OWL_GENRE_F },   /* une durée */
#ifdef OWL_HAS_SQL
    /* SQL.2.a : types reifies du moteur SQL embarque. Absents d'owl-cli
     * (build sans OWL_HAS_SQL) qui ne linke pas la lib owl_sql. */
    { .canonical      = "sqlConnexion",
      .canonicalAscii = "sqlConnexion",
      .aliases        = ALIAS_sqlConnexion,
      .category       = "SQL",
      .descFR         = "Descripteur d'une connexion à une base de données (Provider, Source, Serveur, BaseDeDonnées, Utilisateur, MotDePasse).",
      .descEN         = "Descriptor for a database connection (Provider, Source, Server, Database, User, Password).",
      .fields         = FIELDS_sqlConnexion,
      .nameEN         = "sqlConnection",
      .genre          = OWL_GENRE_F },   /* la connexion */
    { .canonical      = "sqlRequ\xc3\xaa" "te",   /* sqlRequête */
      .canonicalAscii = "sqlRequete",
      .aliases        = ALIAS_sqlRequete,
      .category       = "SQL",
      .descFR         = "Template SQL paramétré : .Code (chaîne avec placeholders {nom}) + .Paramètres (tableau associatif).",
      .descEN         = "Parameterized SQL template: .Code (string with {name} placeholders) + .Parameters (associative array).",
      .fields         = FIELDS_sqlRequete,
      .nameEN         = "sqlQuery",
      .genre          = OWL_GENRE_F },   /* la requête */
    { .canonical      = "sqlR\xc3\xa9" "ponse",   /* sqlRéponse */
      .canonicalAscii = "sqlReponse",
      .aliases        = ALIAS_sqlReponse,
      .category       = "SQL",
      .descFR         = "Résultat d'un SQLEnvoie : .Succès, .Lignes (tableau), .Occurrence, .LignesAffectées.",
      .descEN         = "Result of a SQLSend: .Success, .Rows (array), .Count, .AffectedRows.",
      .fields         = FIELDS_sqlReponse,
      .nameEN         = "sqlResponse",
      .genre          = OWL_GENRE_F },   /* la réponse */
#endif /* OWL_HAS_SQL */
    { .canonical      = "ftpClient",
      .canonicalAscii = "ftpClient",
      .category       = "FTP",
      .descFR         = "Descripteur d'une connexion à un serveur FTP (Serveur, Utilisateur, MotDePasse, Port, Mode, Connectée).",
      .descEN         = "Descriptor for a connection to an FTP server (Server, User, Password, Port, Mode, Connected).",
      .fields         = FIELDS_ftpClient,
      .nameEN         = "ftpClient",
      .genre          = OWL_GENRE_F },   /* la connexion */
    { .canonical      = "Option",
      .category       = "UI",
      .descFR         = "Item d'un Champ Sélecteur, Combo, Liste : Libellé affiché + ValeurRenvoyée à la lecture, avec sous-libellé et image optionnels.",
      .descEN         = "Item of a RadioGroup, ComboBox or List control: displayed Label + ReturnedValue on read, with optional sub-label and image.",
      .fields         = FIELDS_Option,
      .nameEN         = "Option",
      .genre          = OWL_GENRE_F },   /* une option */
    { .canonical      = "zipArchive",
      .category       = "Compression",
      .descFR         = "Archive compressée (ZIP, TAR, GZIP, 7Z…) ouverte par zipOuvre ou créée par zipCrée. Se manipule via les fonctions zipXxx (ajout, extraction, liste de fichiers…).",
      .descEN         = "A compressed archive (ZIP, TAR, GZIP, 7Z…) opened with zipOpen or created with zipCreate. Handled through the zipXxx functions (add, extract, list files…).",
      .fields         = NULL,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "zipArchive",
      .genre          = OWL_GENRE_F },   /* une archive */
    /*  — Couleur (type compose canon). Declare dans
     * stdlib_color.c avec ses 8 fields (Rouge/Vert/Bleu/Teinte/Saturation/
     * Luminosite/Opacite/Couleur), son onFieldSet rebalance et son parser
     * de coerce-in (entier RVB packed -> 8 fields cobreants). */
    /*  — Type abstrait pour referencer un champ UI (alias d'un
     * ObjUiControl). Declarable en variable scalaire locale :
     *   champTrouve est un Champ = GR_X[i]
     * Pas un widget instanciable cote DSL : sert juste a typer une ref
     * opaque vers un controle deja existant (pour l'iteration sur Groupes
     * et le typage des indexations `GR_X[i]`). */
    { .canonical      = "Champ",
      .canonicalAscii = "Champ",
      .aliases        = ALIAS_Champ,
      .category       = "UI",
      .descFR         = "Référence générique vers un champ d'interface, quel que soit son genre (Libellé, Bouton, Image, Table…). Permet d'écrire du code qui manipule un contrôle sans présumer son genre concret : parcours d'un Groupe, indexation `GR_X[i]`, ou paramètre de procédure acceptant n'importe quel champ. Les propriétés accessibles via `champ..Propriété` dépendent du genre réel du contrôle référencé. Pour déclarer un champ concret dans une fenêtre, préfixer le genre : `BTN_OK est un Champ Bouton`.",
      .descEN         = "Generic reference to an interface field, whatever its kind (Label, Button, Image, Table…). Lets code manipulate a control without assuming its concrete kind: iterating over a Group, indexing `GR_X[i]`, or as a procedure parameter accepting any field. Properties reachable through `field..Property` depend on the referenced control's actual kind. To declare a concrete field in a window, prefix the kind: `BTN_OK est un Champ Bouton`.",
      .fields         = NULL,
      .nameEN         = "Control",
      .genre          = OWL_GENRE_M },   /* le champ */
    /*  — Sous-type composé de `tableau de Champ` + dispatch
     * broadcast specialise. Declarable au top-level fenetre ou en
     * variable locale procedure. API mutation reutilise les natives
     * tableau (Ajoute / Supprime / SupprimeTout / Cherche / ..Occurrence).
     * Broadcast : `GR_X..Prop = val` propage aux membres exposant la prop,
     * no-op silencieux sur les autres. */
    { .canonical      = "Groupe",
      .canonicalAscii = "Groupe",
      .aliases        = ALIAS_Groupe,
      .category       = "UI",
      .descFR         = "Groupe de champs UI. Sous-type composé d'un tableau de Champ avec dispatch broadcast : `GR_X..Visible = Faux` propage aux membres qui exposent la propriete, no-op silencieux sur les autres. Mutation via les natives tableau (Ajoute, Supprime, Cherche, ..Occurrence, indexation [i]).",
      .descEN         = "Group of UI fields. Composed sub-type of an array of Field with broadcast dispatch: `GR_X..Visible = False` propagates to members exposing the property, silent no-op on the others. Mutation via array natives (Add, Delete, Seek, ..Occurrence, [i] indexing).",
      .fields         = NULL,
      .nameEN         = "Group",
      .genre          = OWL_GENRE_M },   /* le groupe */
};

/* Note on ValPI: registered separately because it's the only real-valued const. */
#define C_INT(n, cat, v, df, de, en) { .name = (n), .category = (cat), .value = INTEGER_VAL(v), .descFR = (df), .descEN = (de), .nameEN = (en) }
#define C_STR(n, cat, s, len, df, de) \
    { .name = (n), .category = (cat), .value = NIL_VAL, .descFR = (df), .descEN = (de) }
/* String values are instantiated at init time from the static table below
 * because NIL_VAL initialisation avoids depending on the VM at static-init. */

typedef struct {
    const char* name;
    const char* value;
    int         length;
    const char* descFR;
    const char* descEN;
    const char* nameEN;
} ConstStringSpec;

/*  lot 1 — deux constantes de fin de ligne, et deux seulement : LF
 * (1 octet) et CRLF (2 octets). Les formes heritees RC et CR ont quitte ce
 * tableau, donc le registre : cf. EOL_LEGACY_ALIASES juste en dessous. */
static const ConstStringSpec STRING_CONSTS[] = {
    {"CRLF",  "\r\n",     2, "Caract(13)+Caract(10) — fin de ligne des formats et protocoles (HTTP, CSV RFC 4180, fichier Windows)", "Charact(13)+Charact(10) — end of line for formats and protocols (HTTP, CSV RFC 4180, Windows files)", "CRLF"},
    {"LF",    "\n",       1, "Caract(10) — saut de ligne d'OWL, ce que produit une chaîne multiligne", "Charact(10) — OWL line break, what a multiline string produces", "LF"},
    {"TAB",   "\t",       1, "Caract(9) — tabulation", "Charact(9) — tab", "TAB"},
    {"EOT",   "\x04",     1, "Caract(4) — End Of Transmission", "End Of Transmission", "EOT"},
    {"ESC",   "\x1b",     1, "Caract(27) — touche Echap", "Charact(27) — Escape key", "ESC"},
    {"ESP",   "\xc2\xa0", 2, "Caract(160) — espace insécable (NBSP)", "non-breaking space", "ESP"},
    {0},
};

/* Couche de compatibilite des formes heritees de fin de ligne ( lot 1).
 *
 * RC et CR sont enregistres dans vm.globals — donc resolus par la meme voie
 * que n'importe quel identifiant, casse et accents insensibles via
 * vm.globals_canon_idx — mais VOLONTAIREMENT absents du registre : ni
 * completion, ni survol, ni `owl list`, ni --vscode-data, ni `owl explain`.
 * Ils cessent d'etre des symboles du langage tout en laissant compiler le
 * code existant. `owl fmt` les reecrit en CRLF (entrees CAT_EOL_LEGACY de
 * fmt_keywords[] dans formatter.c) : la reecriture est sure, la valeur est
 * identique de part et d'autre.
 *
 * Historique de la collision : RC (« Retour Chariot ») vaut CR+LF, alors que
 * son homonyme anglais CR designe le seul octet 13. Les deux noms valaient
 * pourtant la meme paire dans le langage d'origine ; l'extension OWL qui
 * donnait 1 octet a CR creait a elle seule l'ambiguite. Pour l'octet 13 seul,
 * la seule ecriture est desormais Caract(13).
 *
 * N'ajoute rien ici : cette table est une couche de migration fermee, pas un
 * mecanisme d'alias a usage general. */
static const ConstStringSpec EOL_LEGACY_ALIASES[] = {
    {"RC",    "\r\n",     2, NULL, NULL, NULL},
    {"CR",    "\r\n",     2, NULL, NULL, NULL},
    {0},
};

/* Indirection-related constants — no home module of their own (used by
 * compiler/reflection intrinsics). Everything else has moved to its
 * owning stdlib_*.c next to the enum it belongs to. */
static const OwlConstDef INDIRECTION_CONSTS[] = {
    C_INT("indFenêtre",  "Indirection", OWL_IND_WINDOW,   "accès à une fenêtre désignée par un nom calculé", "access a window designated by a computed name", "indWindow"),
    C_INT("indChamp",    "Indirection", OWL_IND_CHAMP,    "accès à un champ désigné par un nom calculé", "access a control designated by a computed name", "indControl"),
    C_INT("indVariable", "Indirection", OWL_IND_VARIABLE, "accès à une variable globale désignée par un nom calculé", "access a global variable designated by a computed name", "indVariable"),
};

/* fnbAuto — valeur du champ ChiffresAprèsVirgule d'un FormatNombre signifiant
 * "auto/court" : l'affichage suit la forme plate courte, sans arrondi fixe. */
static const OwlConstDef FORMAT_NOMBRE_CONSTS[] = {
    C_INT("fnbAuto", "FormatNombre", OWL_FNB_AUTO,
          "Décimales « auto/court » d'un FormatNombre : affichage plat court, sans arrondi fixe.",
          "\"Auto/short\" decimals of a NumberFormat: short flat display, no fixed rounding.",
          "fnbAuto"),
};

/*  — constantes des enums du type FormatMonétaire (PositionSymbole,
 * FormatNégatif). Formes EN portées par nameEN. */
static const OwlConstDef FORMAT_MONETAIRE_CONSTS[] = {
    C_INT("posAvant", "FormatMon\xc3\xa9taire", OWL_POSSYM_AVANT,
          "Symbole monétaire affiché avant le montant (ex. \"€1 234,50\").",
          "Currency symbol shown before the amount (e.g. \"€1,234.50\").",
          "posBefore"),
    C_INT("posApr\xc3\xa8s", "FormatMon\xc3\xa9taire", OWL_POSSYM_APRES,
          "Symbole monétaire affiché après le montant (défaut, ex. \"1 234,50 €\").",
          "Currency symbol shown after the amount (default, e.g. \"1,234.50 €\").",
          "posAfter"),
    C_INT("fmnMoins", "FormatMon\xc3\xa9taire", OWL_FMN_MOINS,
          "Négatif affiché avec un signe '-' (défaut).",
          "Negative shown with a '-' sign (default).",
          "fmnMinus"),
    C_INT("fmnParenth\xc3\xa8ses", "FormatMon\xc3\xa9taire", OWL_FMN_PARENTHESES,
          "Négatif affiché entre parenthèses (style comptable).",
          "Negative shown in parentheses (accounting style).",
          "fmnParentheses"),
};

static const OwlEnum* const FORMAT_MONETAIRE_ENUMS[] = {
    &E_positionSymbole, &E_formatMonetaireNegatif,
};

#undef C_INT
#undef C_STR

/* Called from vm_init() after stdlib_init(). */
void register_builtin_types(void) {
    /* 1. Runtime registration — create templates in vm.globals so user code
     *    can write `req est un httpRequête`. */
    /* IMPORTANT GC : chaque template doit etre roote (vm_push) entre sa
     * creation et son installation finale dans vm.globals, sinon une
     * allocation intermediaire (build_X_template suivant qui alloue beaucoup
     * de strings + ObjInstance) peut declencher le GC et collecter le
     * template precedent — provoquant des Definition.Nom = "Trait" / "Police"
     * aleatoires sur RecupereDefinitionType("httpRequete") (cf. test
     * test_reflection.owl). */
    ObjInstance* httpReq = build_httpRequete_template();
    vm_push(OBJ_VAL(httpReq));
    ObjInstance* httpRep = build_httpReponse_template();
    vm_push(OBJ_VAL(httpRep));

    register_global_instance("httpRequete", httpReq);
    register_global_instance("httpReponse", httpRep);
    register_global_instance("httpRequ\xc3\xaate", httpReq);   /* httpRequête */
    register_global_instance("httpR\xc3\xa9ponse", httpRep);   /* httpRéponse */
    vm_pop(); vm_pop();

    /* AWSSession — identifiants AWS statiques. */
    ObjInstance* awsSess = build_AWSSession_template();
    vm_push(OBJ_VAL(awsSess));
    register_global_instance("AWSSession", awsSess);
    vm_pop();

    /* pdfParametre — canon. */
    ObjInstance* pdfParam = build_pdfParametre_template();
    vm_push(OBJ_VAL(pdfParam));
    register_global_instance("pdfParametre",            pdfParam);
    register_global_instance("pdfParam\xc3\xa8tre",     pdfParam);  /* pdfParamètre */
    vm_pop();

    /* Police — canon. Type composé pour les Libellés
     * (Fenêtre + Etat) : Police..Nom, Police..Taille, etc. */
    ObjInstance* policeTpl = build_Police_template();
    register_global_instance("Police", policeTpl);

    /* Cadre — canon. Type composé pour les bordures :
     * Cadre..Couleur, Cadre..Epaisseur, Cadre..Trait, Cadre..Arrondi, Cadre..Bords.
     * Premier rocher de l'enabler Style. */
    ObjInstance* cadreTpl = build_Cadre_template();
    register_global_instance("Cadre", cadreTpl);

    /*  — CadreOnglet : cadre restreint du pane d'onglet (Couleur/
     * Epaisseur/Arrondi). Registre pour que le template styleCadreExtérieurOnglet
     * clone un Cadre sans Trait/Bords/Coins. */
    register_global_instance("CadreOnglet", build_CadreOnglet_template());

    /*  — Couleur (type compose canon). Template alloue dans
     * stdlib_color.c (proximite du parser + onFieldSet + helpers HSL). */
    extern ObjInstance* build_Couleur_template(void);
    register_global_instance("Couleur", build_Couleur_template());

    /* Lot inline literal : sous-types Coin et Trait registres pour permettre
     * `Cadre..CoinHautGauche = { Largeur=2, Hauteur=3 }` recursif via
     * __StructClone(Coin). Pas exposes au user comme types declarables
     * (interne au type Cadre). */
    register_global_instance("Coin",  build_Coin_template());
    register_global_instance("Trait", build_Trait_template());

    /* Option — type generique pour Champ Selecteur / Interrupteur. Permet
     * `Options = [{Libelle="...", SousLibelle="..."}, ...]` : la prop
     * Options expose enum_name="Option" pour qu'instance_literal trouve
     * le target type, le brace literal cree alors une ObjInstance(Option)
     * a laquelle on greffe les fields Libelle/SousLibelle/etc. */
    {
        ObjString* nm = obj_copy_string("Option", 6);
        ObjInstance* opt = obj_new_instance(nm);
        inst_field_set_str(opt, "Libelle",        "");
        inst_field_set_str(opt, "SousLibelle",    "");
        inst_field_set_str(opt, "ValeurRenvoyee", "");
        inst_field_set_str(opt, "Image",          "");
        register_global_instance("Option", opt);
    }

    /* Style — descripteur d'apparence aggrégé (D.2 enabler Lot D).
     * Sous-champs plats (Couleur/CouleurFond/Retraits) + sous-objets
     * composés (Police, Cadre). Application via champ..Style = STY_X. */
    ObjInstance* styleTpl = build_Style_template();
    register_global_instance("Style", styleTpl);

    /* styleSaisie — modele de style typé du champ Saisie (, refonte).
     * Struct pur de sous-elements ZoneSaisie/Libellé/CadreExtérieur. Les sous-
     * elements sont des types a part entiere (declarables, cascade CODE). */
    {
        ObjInstance* zoneBaseTpl = build_styleZoneSaisieBase_template();
        vm_push(OBJ_VAL(zoneBaseTpl));
        register_global_instance("styleZoneSaisieBase",  zoneBaseTpl);
        register_global_instance("styleInputZoneBase",   zoneBaseTpl);  /* alias EN */

        ObjInstance* zoneEtatsTpl = build_styleZoneSaisieEtats_template();
        vm_push(OBJ_VAL(zoneEtatsTpl));
        register_global_instance("styleZoneSaisieEtats", zoneEtatsTpl);
        register_global_instance("styleInputZoneStates", zoneEtatsTpl);  /* alias EN */

        ObjInstance* zoneTpl = build_styleZoneSaisie_template();
        vm_push(OBJ_VAL(zoneTpl));
        register_global_instance("styleZoneSaisie",      zoneTpl);
        register_global_instance("styleInputZone",       zoneTpl);  /* alias EN */

        ObjInstance* libelleSaisieTpl = build_styleLibelleSaisie_template();
        vm_push(OBJ_VAL(libelleSaisieTpl));
        register_global_instance("styleLibell\xc3\xa9Saisie", libelleSaisieTpl);  /* styleLibelléSaisie */
        register_global_instance("styleLibelleSaisie",   libelleSaisieTpl);  /* alias ASCII */
        register_global_instance("styleInputLabel",      libelleSaisieTpl);  /* alias EN */

        /* styleOmbre — sous-objet ombre du CadreExtérieur. Declaré
         * standalone (`est un styleOmbre`) + réutilisé imbriqué dans chaque
         * styleCadreExtérieur. */
        ObjInstance* ombreTpl = build_styleOmbre_template();
        vm_push(OBJ_VAL(ombreTpl));
        register_global_instance("styleOmbre",  ombreTpl);
        register_global_instance("styleShadow", ombreTpl);  /* alias EN */

        /* styleDégradé — sous-objet dégradé du CadreExtérieur.
         * Declaré standalone (`est un styleDégradé`) + réutilisé imbriqué dans
         * chaque styleCadreExtérieur. */
        ObjInstance* degradeTpl = build_styleDegrade_template();
        vm_push(OBJ_VAL(degradeTpl));
        register_global_instance("styleD\xc3\xa9grad\xc3\xa9", degradeTpl);  /* styleDégradé */
        register_global_instance("styleDegrade",  degradeTpl);  /* alias ASCII */
        register_global_instance("styleGradient", degradeTpl);  /* alias EN */

        /* styleCadreExterieurBase / Etats — surcharge par etat du
         * cadre exterieur (survol/appuyé/focus/grisé). Declarés standalone pour
         * que __StructClone les trouve lors de la cascade `..Etats[etat]..X`. */
        ObjInstance* cadreBaseTpl = build_styleCadreExterieurBase_template();
        vm_push(OBJ_VAL(cadreBaseTpl));
        register_global_instance("styleCadreExterieurBase", cadreBaseTpl);
        register_global_instance("styleOuterFrameBase",     cadreBaseTpl);  /* alias EN */

        ObjInstance* cadreEtatsTpl = build_styleCadreExterieurEtats_template();
        vm_push(OBJ_VAL(cadreEtatsTpl));
        register_global_instance("styleCadreExterieurEtats", cadreEtatsTpl);
        register_global_instance("styleOuterFrameStates",    cadreEtatsTpl);  /* alias EN */

        ObjInstance* cadreTpl = build_styleCadreExterieur_template();
        vm_push(OBJ_VAL(cadreTpl));
        register_global_instance("styleCadreExt\xc3\xa9rieur", cadreTpl);  /* styleCadreExtérieur */
        register_global_instance("styleCadreExterieur",  cadreTpl);  /* alias ASCII */
        register_global_instance("styleOuterFrame",      cadreTpl);  /* alias EN */

        ObjInstance* styleSaisieTpl = build_styleSaisie_template();
        vm_push(OBJ_VAL(styleSaisieTpl));
        register_global_instance("styleSaisie", styleSaisieTpl);
        register_global_instance("styleInput",  styleSaisieTpl);  /* alias EN */

        /* styleLibellé — modele widget du Champ Libellé. */
        ObjInstance* styleLibelleTpl = build_styleLibelle_template();
        vm_push(OBJ_VAL(styleLibelleTpl));
        register_global_instance("styleLibell\xc3\xa9", styleLibelleTpl);  /* styleLibellé */
        register_global_instance("styleLibelle",        styleLibelleTpl);  /* alias ASCII */
        register_global_instance("styleLabel",          styleLibelleTpl);  /* alias EN */

        /* styleImage — modele widget du Champ Image. Un seul
         * sous-element : CadreExtérieur (la boite). Meme nom FR/EN. */
        ObjInstance* styleImageTpl = build_styleImage_template();
        vm_push(OBJ_VAL(styleImageTpl));
        register_global_instance("styleImage", styleImageTpl);

        /* styleCodeBarre — modele widget du Champ CodeBarres. Meme
         * structure qu'un styleImage (CadreExtérieur), type dedie. Alias EN
         * styleBarCode. */
        ObjInstance* styleCodeBarreTpl = build_styleCodeBarre_template();
        vm_push(OBJ_VAL(styleCodeBarreTpl));
        register_global_instance("styleCodeBarre", styleCodeBarreTpl);
        register_global_instance("styleBarCode", styleCodeBarreTpl);  /* alias EN */

        /* styleConteneur — modele widget du Champ Conteneur. Un
         * seul sous-element : CadreExtérieur (la boite). Alias EN styleContainer. */
        ObjInstance* styleConteneurTpl = build_styleConteneur_template();
        vm_push(OBJ_VAL(styleConteneurTpl));
        register_global_instance("styleConteneur", styleConteneurTpl);
        register_global_instance("styleContainer", styleConteneurTpl);  /* alias EN */

        /* styleInterrupteur — modele widget du Champ Interrupteur.
         * Sous-elements : CadreExtérieur + TexteOptions + Coche + CadreCoche. */
        ObjInstance* styleCocheTpl = build_styleCoche_template();
        vm_push(OBJ_VAL(styleCocheTpl));
        register_global_instance("styleCoche",     styleCocheTpl);
        register_global_instance("styleCheckMark", styleCocheTpl);  /* alias EN */

        ObjInstance* styleInterrupteurTpl = build_styleInterrupteur_template();
        vm_push(OBJ_VAL(styleInterrupteurTpl));
        register_global_instance("styleInterrupteur", styleInterrupteurTpl);
        register_global_instance("styleSwitch",       styleInterrupteurTpl);  /* alias EN */

        /* styleSélecteur — modele widget du Champ Sélecteur (radio).
         * Sous-elements : CadreExtérieur + TexteOptions + Puce + CadrePuce. */
        ObjInstance* stylePuceTpl = build_stylePuce_template();
        vm_push(OBJ_VAL(stylePuceTpl));
        register_global_instance("stylePuce",      stylePuceTpl);
        register_global_instance("styleRadioMark", stylePuceTpl);  /* alias EN */

        ObjInstance* styleSelecteurTpl = build_styleSelecteur_template();
        vm_push(OBJ_VAL(styleSelecteurTpl));
        register_global_instance("styleS\xc3\xa9lecteur", styleSelecteurTpl);  /* styleSélecteur */
        register_global_instance("styleSelecteur",       styleSelecteurTpl);  /* alias ASCII */
        register_global_instance("styleSelector",        styleSelecteurTpl);  /* alias EN */

        /* styleListe — modele widget du Champ Liste (list box).
         * Sous-elements : CadreExtérieur + LibelléLignes + LigneSélectionnée. */
        ObjInstance* styleListeTpl = build_styleListe_template();
        vm_push(OBJ_VAL(styleListeTpl));
        register_global_instance("styleListe", styleListeTpl);
        register_global_instance("styleList",  styleListeTpl);  /* alias EN */

        /* styleCombo — modele widget du Champ Combo. Composite :
         * Libellé + CadreExtérieur + ZoneSaisie (structure Saisie) +
         * LibelléLignes + LigneSélectionnée (dropdown Liste). */
        ObjInstance* styleComboTpl = build_styleCombo_template();
        vm_push(OBJ_VAL(styleComboTpl));
        register_global_instance("styleCombo",    styleComboTpl);
        register_global_instance("styleCombobox", styleComboTpl);  /* alias EN */

        /* styleTable / styleColonne — modele widget du Champ Table.
         * styleTable : 8 sous-elements (CadreExtérieur, TitreColonne,
         * LibelléLignes, LignesPaires, LignesImpaires, LigneSélectionnée,
         * LigneSurvolée, Quadrillage). styleColonne : override par colonne, 6
         * sous-elements (sans CadreExtérieur ni Quadrillage). */
        ObjInstance* styleQuadrillageTpl = build_styleQuadrillage_template();
        vm_push(OBJ_VAL(styleQuadrillageTpl));
        register_global_instance("styleQuadrillage", styleQuadrillageTpl);
        register_global_instance("styleGridlines",   styleQuadrillageTpl);  /* alias EN */

        ObjInstance* styleTableTpl = build_styleTable_template();
        vm_push(OBJ_VAL(styleTableTpl));
        register_global_instance("styleTable", styleTableTpl);  /* "Table" identique FR/EN, pas d'alias distinct */

        /* styleJauge — modele widget du Champ Jauge. 3 sous-elements
         * (CadreExtérieur, Remplissage, Libellé). */
        ObjInstance* styleJaugeTpl = build_styleJauge_template();
        vm_push(OBJ_VAL(styleJaugeTpl));
        register_global_instance("styleJauge", styleJaugeTpl);
        register_global_instance("styleGauge", styleJaugeTpl);  /* alias EN */

        ObjInstance* styleColonneTpl = build_styleColonne_template();
        vm_push(OBJ_VAL(styleColonneTpl));
        register_global_instance("styleColonne", styleColonneTpl);
        register_global_instance("styleColumn",  styleColonneTpl);  /* alias EN */

        /* styleZoneRépétée — modele widget du Champ Zone répétée.
         * 7 sous-elements (CadreExtérieur, CadreInterne, LignesImpaires,
         * LignesPaires, LigneSélectionnée, LigneSurvolée, Séparateur). Defauts
         * sobres : aucun bleu de selection, contrairement a styleTable. */
        ObjInstance* styleZrTpl = build_styleZoneRepetee_template();
        vm_push(OBJ_VAL(styleZrTpl));
        register_global_instance("styleZoneR\xc3\xa9p\xc3\xa9t\xc3\xa9""e", styleZrTpl);  /* styleZoneRépétée */
        register_global_instance("styleZoneRepetee", styleZrTpl);  /* alias ASCII */
        register_global_instance("styleLooper",      styleZrTpl);  /* alias EN */

        /* styleBouton — modele widget du Champ Bouton. Sous-element
         * Bouton stateful (palette survol/appuyé/focus/grisé) + CadreExtérieur
         * reutilise. */
        ObjInstance* btnBaseTpl = build_styleZoneBoutonBase_template();
        vm_push(OBJ_VAL(btnBaseTpl));
        register_global_instance("styleZoneBoutonBase",  btnBaseTpl);
        register_global_instance("styleButtonZoneBase",  btnBaseTpl);  /* alias EN */

        ObjInstance* btnEtatsTpl = build_styleZoneBoutonEtats_template();
        vm_push(OBJ_VAL(btnEtatsTpl));
        register_global_instance("styleZoneBoutonEtats", btnEtatsTpl);
        register_global_instance("styleButtonZoneStates", btnEtatsTpl);  /* alias EN */

        ObjInstance* btnZoneTpl = build_styleZoneBouton_template();
        vm_push(OBJ_VAL(btnZoneTpl));
        register_global_instance("styleZoneBouton",      btnZoneTpl);
        register_global_instance("styleButtonZone",      btnZoneTpl);  /* alias EN */

        /* styleImageBoutonBase / Etats — surcharge d'icone par etat
         * (survol/grisé). */
        ObjInstance* btnImgBaseTpl = build_styleImageBoutonBase_template();
        vm_push(OBJ_VAL(btnImgBaseTpl));
        register_global_instance("styleImageBoutonBase",  btnImgBaseTpl);
        register_global_instance("styleButtonImageBase",  btnImgBaseTpl);  /* alias EN */

        ObjInstance* btnImgEtatsTpl = build_styleImageBoutonEtats_template();
        vm_push(OBJ_VAL(btnImgEtatsTpl));
        register_global_instance("styleImageBoutonEtats",  btnImgEtatsTpl);
        register_global_instance("styleButtonImageStates", btnImgEtatsTpl);  /* alias EN */

        /* styleImageBouton ( lot B) — sous-element icone declarable. */
        ObjInstance* btnImageTpl = build_styleImageBouton_template();
        vm_push(OBJ_VAL(btnImageTpl));
        register_global_instance("styleImageBouton",     btnImageTpl);
        register_global_instance("styleButtonImage",     btnImageTpl);  /* alias EN */

        ObjInstance* styleBoutonTpl = build_styleBouton_template();
        vm_push(OBJ_VAL(styleBoutonTpl));
        register_global_instance("styleBouton", styleBoutonTpl);
        register_global_instance("styleButton", styleBoutonTpl);  /* alias EN */
        vm_pop();  /* styleBoutonTpl */

        /* styleMenuPrincipal — style typé du menu classique.
         * 3 surfaces (Barre/SousMenu/OptionMenu) + Base/Etats sur les surfaces
         * stateful. register_global_instance enracine dans la table des types :
         * pop immediat sur.  */
        {
            ObjInstance* t;
            t = build_styleMenuPrincipalBarreBase_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuPrincipalBarreBase", t);
            register_global_instance("styleMainMenuBarBase",        t);
            vm_pop();

            t = build_styleMenuPrincipalBarreEtats_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuPrincipalBarreEtats", t);
            register_global_instance("styleMainMenuBarStates",       t);
            vm_pop();

            t = build_styleMenuPrincipalBarre_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuPrincipalBarre", t);
            register_global_instance("styleMainMenuBar",        t);
            vm_pop();

            t = build_styleMenuPrincipalSousMenu_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuPrincipalSousMenu", t);
            register_global_instance("styleMainMenuSubmenu",       t);
            vm_pop();

            t = build_styleMenuPrincipalOptionBase_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuPrincipalOptionBase", t);
            register_global_instance("styleMainMenuOptionBase",      t);
            vm_pop();

            t = build_styleMenuPrincipalOptionEtats_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuPrincipalOptionEtats", t);
            register_global_instance("styleMainMenuOptionStates",     t);
            vm_pop();

            t = build_styleMenuPrincipalOption_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuPrincipalOption", t);
            register_global_instance("styleMainMenuOption",      t);
            vm_pop();

            t = build_styleMenuPrincipal_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuPrincipal", t);
            register_global_instance("styleMainMenu",      t);
            vm_pop();

            /*  — styleMenuContextuel : 2 surfaces (SousMenu/OptionMenu),
             * pas de Barre. Reutilise les surfaces deja enregistrees ci-dessus. */
            t = build_styleMenuContextuel_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleMenuContextuel", t);
            register_global_instance("styleContextMenu",    t);
            vm_pop();

            /*  (lot 5d) — styleOnglet : racine + volet partage
             * (VoletActif/VoletInactif) + Base/Etats de la surcharge survol. */
            t = build_styleVoletBase_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleVoletBase",  t);
            register_global_instance("stylePaneBase",   t);
            vm_pop();

            t = build_styleVoletEtats_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleVoletEtats",  t);
            register_global_instance("stylePaneStates",  t);
            vm_pop();

            t = build_styleVolet_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleVolet", t);
            register_global_instance("stylePane",  t);
            vm_pop();

            /*  — CadreExtérieur restreint du pane d'onglet. */
            t = build_styleCadreExterieurOnglet_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleCadreExt\xc3\xa9rieurOnglet", t);  /* styleCadreExtérieurOnglet */
            register_global_instance("styleCadreExterieurOnglet",  t);  /* alias ASCII */
            register_global_instance("styleTabOuterFrame",         t);  /* alias EN */
            vm_pop();

            t = build_styleOnglet_template();
            vm_push(OBJ_VAL(t));
            register_global_instance("styleOnglet", t);
            register_global_instance("styleTab",    t);
            vm_pop();
        }
        vm_pop();  /* btnImageTpl */
        vm_pop();  /* btnImgEtatsTpl */
        vm_pop();  /* btnImgBaseTpl */
        vm_pop();  /* btnZoneTpl */
        vm_pop();  /* btnEtatsTpl */
        vm_pop();  /* btnBaseTpl */

        vm_pop();  /* styleInterrupteurTpl */
        vm_pop();  /* styleCocheTpl */
        vm_pop();  /* styleCodeBarreTpl */
        vm_pop();  /* styleImageTpl */
        vm_pop();  /* styleLibelleTpl */
        vm_pop();  /* styleSaisieTpl */
        vm_pop();  /* cadreTpl */
        vm_pop();  /* cadreEtatsTpl */
        vm_pop();  /* cadreBaseTpl */
        vm_pop();  /* libelleSaisieTpl */
        vm_pop();  /* zoneTpl */
        vm_pop();  /* zoneEtatsTpl */
        vm_pop();  /* zoneBaseTpl */
    }

    /* Projet — métadonnées déclaratives (projet.owl). Catalogue figé de 3
     * champs chaîne, validé au compile-time. */
    ObjInstance* projetTpl = build_Projet_template();
    vm_push(OBJ_VAL(projetTpl));
    register_global_instance("Projet",  projetTpl);
    register_global_instance("Project", projetTpl);  /* alias EN */
    vm_pop();

    register_global_instance("Definition",  build_Definition_template());
    register_global_instance("Chrono",        build_Chrono_template());
    register_global_instance("Timer",         build_Timer_template());
    register_global_instance("FichierDisque", build_FichierDisque_template());

    /* FichierTéléversé — fichier multipart reçu en paramètre de surRequête
     *. Template partagé pour les formes FR (accentuée + ASCII) et
     * l'alias EN. */
    {
        ObjInstance* uploadTpl = build_FichierTeleverse_template();
        vm_push(OBJ_VAL(uploadTpl));
        register_global_instance("FichierT\xc3\xa9l\xc3\xa9vers\xc3\xa9", uploadTpl); /* FichierTéléversé */
        register_global_instance("FichierTeleverse", uploadTpl);   /* alias ASCII */
        register_global_instance("UploadedFile",     uploadTpl);   /* alias EN */
        vm_pop();
    }

    /* UUID — type reifié (canon). Storage interne _bytes
     * (16 octets). Affichage canonique généré par
     * stdlib_uuid.c::owl_uuid_format_canonical (hook depuis
     * vm.c::value_to_string). */
    register_global_instance("UUID", build_UUID_template());

    /* Image — type natif. Lot I.0
     * pose les fondations : ObjInstance template avec 7 propriétés. */
    extern ObjInstance* build_Image_template(void);
    register_global_instance("Image", build_Image_template());

#ifdef OWL_HAS_SQL
    /* SQL.2.a : types reifies pour le moteur SQL embarque.
     * sqlConnexion descripteur (avec _handle SQLite cache), sqlRequete
     * template SQL + parametres nommes, sqlReponse resultat structure.
     * Absents d'owl-cli (build sans OWL_HAS_SQL). */
    {
        ObjInstance* sqlCnx = build_sqlConnexion_template();
        vm_push(OBJ_VAL(sqlCnx));
        ObjInstance* sqlReq = build_sqlRequete_template();
        vm_push(OBJ_VAL(sqlReq));
        ObjInstance* sqlRep = build_sqlReponse_template();
        vm_push(OBJ_VAL(sqlRep));

        register_global_instance("sqlConnexion",                    sqlCnx);
        register_global_instance("sqlConnection",                   sqlCnx);  /* alias EN */
        register_global_instance("sqlRequ\xc3\xaate",               sqlReq);  /* sqlRequête */
        register_global_instance("sqlRequete",                      sqlReq);  /* alias ASCII */
        register_global_instance("sqlQuery",                        sqlReq);  /* alias EN */
        register_global_instance("sqlR\xc3\xa9ponse",               sqlRep);  /* sqlRéponse */
        register_global_instance("sqlReponse",                      sqlRep);  /* alias ASCII */
        register_global_instance("sqlResponse",                     sqlRep);  /* alias EN */

        vm_pop(); vm_pop(); vm_pop();
    }
#endif /* OWL_HAS_SQL */

    /* ftpClient — client FTP en clair. Ungated : la couche net_tcp est dans
     * le core (comme HTTP), pas dans la lib SQL. */
    {
        ObjInstance* ftpTpl = build_ftpClient_template();
        register_global_instance("ftpClient", ftpTpl);
    }

    /* FormatNumérique — single template, both names point to it. */
    {
        ObjInstance* fnTpl = build_FormatNumerique_template();
        register_global_instance("FormatNumerique", fnTpl);
        register_global_instance("FormatNum\xc3\xa9" "rique", fnTpl);  /* FormatNumérique */
    }

    /* FormatNombre — type léger d'héritage de format projet → champ. */
    {
        ObjInstance* fnbTpl = build_FormatNombre_template();
        register_global_instance("FormatNombre", fnbTpl);
    }

    /*  — FormatMonétaire — type d'héritage de format monétaire. */
    {
        ObjInstance* fmnTpl = build_FormatMonetaire_template();
        register_global_instance("FormatMonetaire", fmnTpl);
        register_global_instance("FormatMon\xc3\xa9taire", fmnTpl);  /* FormatMonétaire */
    }

    /* INDIRECTION_CONSTS : auto-enregistre par owl_registry_register_consts
     * plus bas (ligne 458). Pas de for-each manuel necessaire. */

    /* String "Constantes diverses". Null géré ailleurs. */
    for (int i = 0; STRING_CONSTS[i].name != NULL; i++) {
        register_global_string(STRING_CONSTS[i].name, STRING_CONSTS[i].value, STRING_CONSTS[i].length);
    }

    /* Formes heritees RC/CR : resolvables comme globales, absentes du registre
     * (le for-each de metadonnees plus bas n'itere que STRING_CONSTS). */
    for (int i = 0; EOL_LEGACY_ALIASES[i].name != NULL; i++) {
        register_global_string(EOL_LEGACY_ALIASES[i].name,
                               EOL_LEGACY_ALIASES[i].value,
                               EOL_LEGACY_ALIASES[i].length);
    }

    /* ValPI — seul réel. */
    /*  : migration vers canon obj_globals_set. */
    obj_globals_set("ValPI", REAL_VAL(3.14159265358979323));

    /* 2. Registry metadata — so --vscode-data and --doc can enumerate. */
    owl_registry_register_types (TYPES, (int)(sizeof(TYPES)/sizeof(TYPES[0])));
    owl_registry_register_consts(INDIRECTION_CONSTS,
        (int)(sizeof(INDIRECTION_CONSTS)/sizeof(INDIRECTION_CONSTS[0])));
    owl_registry_register_consts(FORMAT_NOMBRE_CONSTS,
        (int)(sizeof(FORMAT_NOMBRE_CONSTS)/sizeof(FORMAT_NOMBRE_CONSTS[0])));
    owl_registry_register_consts(FORMAT_MONETAIRE_CONSTS,
        (int)(sizeof(FORMAT_MONETAIRE_CONSTS)/sizeof(FORMAT_MONETAIRE_CONSTS[0])));
    owl_registry_register_enums(FORMAT_MONETAIRE_ENUMS,
        (int)(sizeof(FORMAT_MONETAIRE_ENUMS)/sizeof(FORMAT_MONETAIRE_ENUMS[0])));

    /* Build OwlConstDef entries for the string + ValPI constants at runtime
     * (we can't use INTEGER_VAL/OBJ_VAL in static initialisers). */
    {
        static OwlConstDef g_str_consts[16];
        int n = 0;
        for (int i = 0; STRING_CONSTS[i].name != NULL && n < 15; i++) {
            Value v;
            Value stored;
            ObjString* k = obj_copy_string(STRING_CONSTS[i].name, (int)strlen(STRING_CONSTS[i].name));
            if (table_get(&vm.globals, k, &stored)) {
                v = stored;
            } else {
                v = OBJ_VAL(obj_copy_string(STRING_CONSTS[i].value, STRING_CONSTS[i].length));
            }
            g_str_consts[n].name     = STRING_CONSTS[i].name;
            g_str_consts[n].category = "Constantes diverses";
            g_str_consts[n].value    = v;
            g_str_consts[n].descFR   = STRING_CONSTS[i].descFR;
            g_str_consts[n].descEN   = STRING_CONSTS[i].descEN;
            g_str_consts[n].nameEN   = STRING_CONSTS[i].nameEN;
            n++;
        }
        /* ValPI */
        g_str_consts[n].name     = "ValPI";
        g_str_consts[n].category = "Constantes diverses";
        g_str_consts[n].value    = REAL_VAL(3.14159265358979323);
        g_str_consts[n].descFR   = "Valeur approximative de Pi (3.14159265358979323).";
        g_str_consts[n].descEN   = "Approximate value of Pi.";
        g_str_consts[n].nameEN   = "ValPI";
        n++;
        owl_registry_register_consts(g_str_consts, n);
    }
}
