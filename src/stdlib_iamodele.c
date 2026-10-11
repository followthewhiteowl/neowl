// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_iamodele.c — Type `IAModèle` / `AIModel` (, phase 1 SOCLE).
 *
 * Une couche d'API haut-niveau au-dessus du Tenseur : au lieu de
 * gerer poids, biais, forward et boucle a la main, on CONSTRUIT un modele en
 * empilant des couches fluentes, puis on l'INTERROGE via `.Infère`.
 *
 * Modele mental (3 concepts) :
 *   - IAModèle    : la chose qu'on construit et interroge. Il tient une LISTE
 *                   ORDONNEE de couches (collecte explicite, aucun scan
 *                   magique d'attributs).
 *   - Couche      : une methode fluente sur le modele. Dense (poids in×out +
 *                   biais out, initialises a la creation) ou activation
 *                   (ReLU / Sigmoïde / Tanh, sans poids).
 *   - .Infère(x)  : passe une entree (un echantillon Tenseur 1D OU un lot
 *                   Tenseur 2D B×in) a travers chaque couche dans l'ordre.
 *
 * Cette phase pose : le type + les couches + le forward + l'init des poids
 * (He par defaut, Xavier au choix) + les accesseurs d'introspection
 * (.Poids/.Biais/..NbCouches). Pas d'entrainement (phases ulterieures).
 *
 * Le forward REUTILISE les ops differentiables du Tenseur (produit matriciel,
 * addition avec broadcast du biais, activations) — aucune math nouvelle. Un
 * echantillon 1D est promu en lot (1×in) en interne pour emprunter le meme
 * chemin matriciel, puis la sortie (1×out) est ramenee a un Tenseur 1D.
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_methods.h"
#include "owl_registry.h"
#include "onnx_import.h"
#include "onnx_export.h"
#include "owl_image.h"
#include "memory.h"
#include "sentinels.h"
#include "vm.h"

/*  — .Détecte : OwlImage* d'une Value Image (meme convention
 * extern que stdlib_dataset.c, faute d'en-tete public dedie). */
extern OwlImage* owl_image_handle_from_value(Value v);

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parser JSON natif OWL (stdlib_json.c) — reutilise pour .Charge : le meme
 * lecteur que ChaineVersJSON, donc format garanti compatible avec ce que
 * .Sauve emet. Declare en extern faute d'en-tete dedie (meme convention que
 * main.c / stdlib_parametre.c). */
extern ObjJson* json_parse_cstr(const char* src, int len);

#ifndef OWL_IA_PI
#define OWL_IA_PI 3.14159265358979323846
#endif

/* Hyperparametres Adam (optimAdam) : taux de decroissance des moments d'ordre 1
 * (m) et 2 (v) et epsilon de stabilite au denominateur. Valeurs canoniques. */
#define OWL_ADAM_BETA1 0.9
#define OWL_ADAM_BETA2 0.999
#define OWL_ADAM_EPS   1e-8

/*  — momentum de la moyenne exponentielle des stats mobiles de la
 * BatchNorm (run = (1-m)*run + m*lot). Valeur standard. */
#define OWL_BN_MOMENTUM 0.1

/* ================================================================
 * Initialisation aleatoire des poids
 * ================================================================ */

/* Reel uniforme dans [0, 1) — meme source globale que Hasard()/MatriceAléatoire. */
static double ia_rand_uniform(void) {
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

/* Reel normal centre reduit (Box-Muller). u1 borne dans (0, 1) pour eviter
 * log(0). Une seule des deux sorties Box-Muller est utilisee (suffisant). */
static double ia_rand_normal(void) {
    double u1 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0);
    double u2 = (double)rand() / ((double)RAND_MAX + 1.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * OWL_IA_PI * u2);
}

/* ================================================================
 * Construction
 * ================================================================ */

/* Factory interne emise par le compilateur pour `mon est un IAModèle` sans
 * init : instance FRAICHE et vide a chaque declaration (le modele est
 * mutable, un singleton partage casserait tout). */
static Value fn_iamodele_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_iamodele());
}

/*  — Factory interne pour `d est un Détection` : instance neutre (boite
 * vide, classe 0, score 0). En pratique une Détection est produite par
 * `.Détecte`, pas declaree a la main ; la factory existe pour la symetrie du
 * registry (declaration + element d'un `tableau de Détection`). */
static Value fn_detection_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    ObjArray* box = obj_new_array();
    vm_push(OBJ_VAL(box));   /* root pendant l'alloc de la Détection */
    Value d = OBJ_VAL(obj_new_detection(OBJ_VAL(box), 0, 0.0));
    vm_pop();
    return d;
}

/* ================================================================
 * Empilement de couches (methodes fluentes)
 * ================================================================ */

/* .AjouteCoucheDense(entrées, sorties[, init]) — cree poids (in×out) + biais
 * (out), initialise les poids selon `init` (defaut initHe), biais a zero,
 * empile. Rend le receveur (chainage fluide). */
static Value fn_iamodele_ajoute_dense(int argCount, Value* args) {
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (!IS_NUMBER(args[1]) || !IS_NUMBER(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheDense",
                              "arg", "dimensions",
                              "expected", "entrees et sorties entiers",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int in  = (int)AS_NUMBER(args[1]);
    int out = (int)AS_NUMBER(args[2]);
    if (in <= 0 || out <= 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheDense",
                              "arg", "dimensions",
                              "expected", "entrees et sorties strictement positifs",
                              "actual", "dimension nulle ou negative", NULL);
        return NIL_VAL;
    }
    int64_t init = OWL_INIT_HE;
    if (argCount >= 4) {
        if (!IS_NUMBER(args[3])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheDense",
                                  "arg", "init",
                                  "expected", "initHe ou initXavier",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
        init = (int64_t)AS_NUMBER(args[3]);
    }

    /* Poids (in×out) : le modele (args[0]) est roote par le dispatch. On ROOTE
     * les poids sur la pile pendant l'alloc du biais (le second alloc peut
     * declencher un GC qui, sans ca, collecterait les poids fraichement crees). */
    ObjTensor* W = obj_new_tensor_matrix(in, out);
    vm_push(OBJ_VAL(W));
    ObjTensor* b = obj_new_tensor(out);   /* biais zero-init par obj_new_tensor */

    /* A partir d'ici, plus aucune allocation OWL (ecriture de doubles + un
     * realloc C-heap du tableau de couches). */
    double* wd = tensor_cells(W->data);
    int nel = in * out;
    if (init == OWL_INIT_XAVIER) {
        /* Xavier : uniforme dans [-borne, borne], borne = racine(6/(in+out)). */
        double bound = sqrt(6.0 / ((double)in + (double)out));
        for (int i = 0; i < nel; i++) {
            wd[i] = (ia_rand_uniform() * 2.0 - 1.0) * bound;
        }
    } else {
        /* He (defaut) : normal, ecart-type racine(2/in). Compense la moitie du
         * signal ecrasee par ReLU en aval. */
        double std = sqrt(2.0 / (double)in);
        for (int i = 0; i < nel; i++) {
            wd[i] = ia_rand_normal() * std;
        }
    }

    obj_iamodele_push_layer(m, IA_LAYER_DENSE, W, b);
    vm_pop();   /* W — desormais roote via le modele */
    return args[0];
}

/* Couche d'activation sans poids : empile puis rend le receveur. */
static Value ia_add_activation(Value* args, IALayerKind kind) {
    obj_iamodele_push_layer(AS_IAMODELE(args[0]), kind, NULL, NULL);
    return args[0];
}
static Value fn_iamodele_ajoute_relu(int argCount, Value* args) {
    (void)argCount; return ia_add_activation(args, IA_LAYER_RELU);
}
static Value fn_iamodele_ajoute_sigmoide(int argCount, Value* args) {
    (void)argCount; return ia_add_activation(args, IA_LAYER_SIGMOID);
}
static Value fn_iamodele_ajoute_tanh(int argCount, Value* args) {
    (void)argCount; return ia_add_activation(args, IA_LAYER_TANH);
}

/*  — .DébutBranches() / .BeginBranches() : ouvre un bloc a branches
 * paralleles. Au forward, ce marqueur CAPTURE le tenseur courant x (le point de
 * divergence) et ouvre la 1re branche. Empile un marqueur sans poids, incremente
 * la profondeur de blocs ouverts (equilibre verifie a la fusion / au forward).
 * Rend le receveur. */
static Value fn_iamodele_debut_branches(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    obj_iamodele_push_layer(m, IA_LAYER_BRANCH_BEGIN, NULL, NULL);
    m->branchDepth++;
    return args[0];
}

/*  — .Gèle() / .Freeze() : marque NON entrainables TOUTES les couches
 * presentes au moment de l'appel (parcours du tableau). Une couche ajoutee
 * ENSUITE reste entrainable (frozen faux par construction) — c'est ce qui permet
 * de figer un corps pre-entraine puis d'ajouter une tete qui, elle, apprend. Le
 * forward continue de traverser les couches gelees ; seule leur mise a jour par
 * l'optimiseur est sautee. Rend le receveur. */
static Value fn_iamodele_gele(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    for (int li = 0; li < m->count; li++) m->layers[li].frozen = true;
    return args[0];
}

/*  — .Dégèle() / .Unfreeze() : reactive l'entrainement sur TOUTES les
 * couches, y compris celles gelees precedemment. Rend le receveur. */
static Value fn_iamodele_degele(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    for (int li = 0; li < m->count; li++) m->layers[li].frozen = false;
    return args[0];
}

/*  — .RetireDernièreCouche() / .RemoveLastLayer() : retire LA dernière
 * couche empilee (2e brique de l'idiome transfert : retirer la tete d'origine
 * d'un modele pre-entraine avant d'en greffer une nouvelle). Decremente count :
 * la couche retiree cesse d'etre une racine GC (le tracage OBJ_IAMODELE parcourt
 * strictement `layers[0..count)`), donc ses Tenseurs poids/biais deviennent
 * collectables. Le slot libere est efface par hygiene (coupe toute reference
 * residuelle avant une eventuelle reutilisation par un ajout ulterieur). Deux
 * refus : modele vide (rien a retirer) et derniere couche = marqueur de bloc a
 * branches (retrait au milieu d'un bloc ambigu). Retirer une couche gelee est
 * permis (le gel concerne l'entrainement, pas la structure). Rend le receveur. */
static Value fn_iamodele_retire_derniere_couche(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (m->count == 0) {
        owl_emit_native_error(E_NATIVE_IA_RETRAIT_MODELE_VIDE,
                              "RetireDerni\xc3\xa8reCouche", NULL);
        return args[0];
    }
    IALayerKind lastKind = m->layers[m->count - 1].kind;
    if (lastKind == IA_LAYER_BRANCH_BEGIN || lastKind == IA_LAYER_BRANCH_NEXT
        || lastKind == IA_LAYER_MERGE_ADD || lastKind == IA_LAYER_MERGE_CONCAT) {
        owl_emit_native_error(E_NATIVE_IA_RETRAIT_MARQUEUR_BRANCHE,
                              "RetireDerni\xc3\xa8reCouche", NULL);
        return args[0];
    }
    int idx = m->count - 1;
    /* Couper la racine GC : effacer les Obj* du slot retire (le tracage ne
     * regarde plus au-dela de count-1, mais l'hygiene evite tout Obj* obsolete
     * si le slot est reutilise par un ajout suivant). */
    m->layers[idx].weights      = NULL;
    m->layers[idx].bias         = NULL;
    m->layers[idx].gamma        = NULL;
    m->layers[idx].beta         = NULL;
    m->layers[idx].running_mean = NULL;
    m->layers[idx].running_var  = NULL;
    m->count--;
    return args[0];
}

/*  — .BrancheSuivante() / .NextBranch() : clot la branche courante et
 * rouvre une branche repartant du MEME x. Les couches ajoutees ensuite vont dans
 * cette nouvelle branche. Erreur IMMEDIATE si aucun bloc n'est ouvert. Rend le
 * receveur. */
static Value fn_iamodele_branche_suivante(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (m->branchDepth <= 0) {
        owl_emit_native_error(E_NATIVE_IA_BRANCHE_SANS_DEBUT,
                              "BrancheSuivante", NULL);
        return args[0];
    }
    obj_iamodele_push_layer(m, IA_LAYER_BRANCH_NEXT, NULL, NULL);
    return args[0];
}

/*  — .FusionneParAddition() / .MergeByAdd() : clot le bloc et fusionne
 * toutes les branches par addition element-par-element (dans l'ordre de
 * declaration). Au forward, chaque branche est executee depuis x, une branche
 * vide valant l'identite (laisse passer x) — un residuel a la ResNet s'exprime
 * donc comme une branche F(x) + une branche vide. Erreur IMMEDIATE si aucun bloc
 * n'est ouvert. Rend le receveur. */
static Value fn_iamodele_fusionne_addition(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (m->branchDepth <= 0) {
        owl_emit_native_error(E_NATIVE_IA_BRANCHE_SANS_DEBUT,
                              "FusionneParAddition", NULL);
        return args[0];
    }
    obj_iamodele_push_layer(m, IA_LAYER_MERGE_ADD, NULL, NULL);
    m->branchDepth--;
    return args[0];
}

/*  — .FusionneParConcaténation() / .MergeByConcat() : clot le bloc et
 * fusionne toutes les branches en CONCATENANT leurs sorties sur l'axe canal
 * (index 1 de [N,C,H,W]), dans l'ordre de declaration -> sortie
 * [N, C1+C2+..., H, W]. Contrairement a la fusion par addition, les canaux ont
 * le droit de differer d'une branche a l'autre ; seules les dimensions N,H,W
 * doivent coincider (verifie au forward). Erreur IMMEDIATE si aucun bloc n'est
 * ouvert. Rend le receveur. */
static Value fn_iamodele_fusionne_concat(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (m->branchDepth <= 0) {
        owl_emit_native_error(E_NATIVE_IA_BRANCHE_SANS_DEBUT,
                              "FusionneParConcat\xc3\xa9nation", NULL);
        return args[0];
    }
    obj_iamodele_push_layer(m, IA_LAYER_MERGE_CONCAT, NULL, NULL);
    m->branchDepth--;
    return args[0];
}

/*  +  — .AjouteCoucheConvolution2D(canauxEntrée,
 * canauxSortie, tailleNoyau, pas, remplissage[, group]). Cree un noyau
 * Cout×(Cin/group)×k×k (init He, ecart type racine(2/((Cin/group)·k·k))) + un
 * biais 1×Cout×1×1 a zero, memorise pas, remplissage et group (dans le champ
 * window, libre pour la conv), empile. Rend le receveur (chainage). */
static Value fn_iamodele_ajoute_conv2d(int argCount, Value* args) {
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (argCount < 6) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheConvolution2D",
                              "arg", "parametres",
                              "expected", "canauxEntree, canauxSortie, tailleNoyau, pas, remplissage",
                              "actual", "nombre d'arguments insuffisant", NULL);
        return NIL_VAL;
    }
    int lastArg = (argCount >= 7) ? 6 : 5;
    for (int a = 1; a <= lastArg; a++) {
        if (!IS_NUMBER(args[a])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheConvolution2D",
                                  "arg", "parametres",
                                  "expected", "cinq entiers (group optionnel)",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
    }
    int cin    = (int)AS_NUMBER(args[1]);
    int cout   = (int)AS_NUMBER(args[2]);
    int ksize  = (int)AS_NUMBER(args[3]);
    int stride = (int)AS_NUMBER(args[4]);
    int pad    = (int)AS_NUMBER(args[5]);
    int group  = (argCount >= 7) ? (int)AS_NUMBER(args[6]) : 1;
    if (cin <= 0 || cout <= 0 || ksize <= 0 || stride < 1 || pad < 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheConvolution2D",
                              "arg", "parametres",
                              "expected", "canaux/noyau > 0, pas >= 1, remplissage >= 0",
                              "actual", "valeur hors bornes", NULL);
        return NIL_VAL;
    }
    if (group < 1 || cin % group != 0 || cout % group != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheConvolution2D",
                              "arg", "group",
                              "expected", "un nombre de groupes >= 1 divisant les canaux d'entree ET de sortie",
                              "actual", "group ne divise pas les canaux", NULL);
        return NIL_VAL;
    }
    int cinG = cin / group;   /* canaux d'entree par groupe = 2e dim du noyau */
    /* Poids Cout×(Cin/group)×k×k rootes pendant l'alloc du biais (second alloc
     * pouvant declencher un GC). Ensuite plus aucune allocation OWL. */
    int wshape[4] = { cout, cinG, ksize, ksize };
    ObjTensor* W = obj_new_tensor_nd(4, wshape);
    vm_push(OBJ_VAL(W));
    int bshape[4] = { 1, cout, 1, 1 };
    ObjTensor* b = obj_new_tensor_nd(4, bshape);   /* biais zero-init */

    double* wd = tensor_cells(W->data);
    int fanIn = cinG * ksize * ksize;
    double std = sqrt(2.0 / (double)fanIn);
    int nel = cout * cinG * ksize * ksize;
    for (int i = 0; i < nel; i++) wd[i] = ia_rand_normal() * std;

    obj_iamodele_push_layer(m, IA_LAYER_CONV2D, W, b);
    m->layers[m->count - 1].stride = stride;
    m->layers[m->count - 1].pad    = pad;
    m->layers[m->count - 1].window = group;   /*  : group (champ libre) */
    vm_pop();   /* W — desormais roote via le modele */
    return args[0];
}

/*  +  +  — .AjouteCoucheMaxPooling(taille[, pas[,
 * remplissage]]) / .AjouteCoucheMoyennePooling(taille[, pas[, remplissage]]).
 * Couche sans poids : memorise la taille de fenetre, le pas (defaut = fenetre
 * -> non chevauchant) et le remplissage (defaut 0), empile. Le remplissage est
 * range dans L->pad (le champ pad de conv, inutilise par le pooling). */
static Value ia_add_pooling(Value* args, int argCount, IALayerKind kind,
                            const char* nm) {
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (!IS_NUMBER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "taille",
                              "expected", "taille de fenetre entiere",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int k = (int)AS_NUMBER(args[1]);
    if (k < 1) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                              "arg", "taille",
                              "expected", "une taille de fenetre >= 1",
                              "actual", "taille nulle ou negative", NULL);
        return NIL_VAL;
    }
    int s = k;   /* pas par defaut = fenetre */
    if (argCount >= 3) {
        if (!IS_NUMBER(args[2])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                  "arg", "pas",
                                  "expected", "un pas entier",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
        s = (int)AS_NUMBER(args[2]);
        if (s < 1) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                  "arg", "pas",
                                  "expected", "un pas >= 1",
                                  "actual", "pas nul ou negatif", NULL);
            return NIL_VAL;
        }
    }
    int pad = 0;   /* remplissage par defaut = 0 */
    if (argCount >= 4) {
        if (!IS_NUMBER(args[3])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                  "arg", "remplissage",
                                  "expected", "un remplissage entier",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
        pad = (int)AS_NUMBER(args[3]);
        if (pad < 0 || pad >= k) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, nm,
                                  "arg", "remplissage",
                                  "expected", "un remplissage entre 0 et la fenetre - 1",
                                  "actual", "remplissage hors bornes", NULL);
            return NIL_VAL;
        }
    }
    obj_iamodele_push_layer(m, kind, NULL, NULL);
    m->layers[m->count - 1].window = k;
    m->layers[m->count - 1].stride = s;
    m->layers[m->count - 1].pad = pad;
    return args[0];
}
static Value fn_iamodele_ajoute_maxpooling(int argCount, Value* args) {
    return ia_add_pooling(args, argCount, IA_LAYER_MAXPOOL, "AjouteCoucheMaxPooling");
}
static Value fn_iamodele_ajoute_moyennepooling(int argCount, Value* args) {
    return ia_add_pooling(args, argCount, IA_LAYER_AVGPOOL, "AjouteCoucheMoyennePooling");
}

/*  — .AjouteCoucheNormalisationLot(canaux[, epsilon]). Cree gamma a 1
 * et beta a 0 (taille = canaux), memorise epsilon (defaut 1e-5), empile. */
static Value fn_iamodele_ajoute_normalisationlot(int argCount, Value* args) {
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (!IS_NUMBER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheNormalisationLot",
                              "arg", "canaux",
                              "expected", "nombre de canaux entier",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int C = (int)AS_NUMBER(args[1]);
    if (C <= 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheNormalisationLot",
                              "arg", "canaux",
                              "expected", "un nombre de canaux strictement positif",
                              "actual", "valeur nulle ou negative", NULL);
        return NIL_VAL;
    }
    double eps = 1e-5;
    if (argCount >= 3) {
        if (!IS_NUMBER(args[2])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AjouteCoucheNormalisationLot",
                                  "arg", "epsilon",
                                  "expected", "epsilon reel",
                                  "actual", "type non pris en charge", NULL);
            return NIL_VAL;
        }
        eps = AS_NUMBER(args[2]);
    }
    /* gamma/beta + stats mobiles (moyenne 0, variance 1 : identite a l'init).
     * Chaque alloc anterieure reste rootee sur la pile pendant l'alloc suivante
     * (chacune peut declencher un GC) ; obj_iamodele_push_layer ne fait qu'un
     * realloc C-heap (aucune alloc OWL). */
    ObjTensor* g = obj_new_tensor(C);
    vm_push(OBJ_VAL(g));
    ObjTensor* be = obj_new_tensor(C);   /* beta zero-init */
    vm_push(OBJ_VAL(be));
    ObjTensor* rm = obj_new_tensor(C);   /* moyenne mobile zero-init */
    vm_push(OBJ_VAL(rm));
    ObjTensor* rv = obj_new_tensor(C);   /* variance mobile a 1 */
    double* gd  = tensor_cells(g->data);
    double* rvd = tensor_cells(rv->data);
    for (int i = 0; i < C; i++) { gd[i] = 1.0; rvd[i] = 1.0; }

    obj_iamodele_push_layer(m, IA_LAYER_BATCHNORM, NULL, NULL);
    m->layers[m->count - 1].gamma        = g;
    m->layers[m->count - 1].beta         = be;
    m->layers[m->count - 1].running_mean = rm;
    m->layers[m->count - 1].running_var  = rv;
    m->layers[m->count - 1].eps          = eps;
    vm_pop();   /* rm */
    vm_pop();   /* be */
    vm_pop();   /* g — desormais rootes via le modele */
    return args[0];
}

/*  — .AjouteCoucheAplatissement() : couche sans poids qui aplatit
 * B×C×H×W en B×(C·H·W) (pont entre les couches spatiales et denses). */
static Value fn_iamodele_ajoute_aplatissement(int argCount, Value* args) {
    (void)argCount;
    obj_iamodele_push_layer(AS_IAMODELE(args[0]), IA_LAYER_FLATTEN, NULL, NULL);
    return args[0];
}

/* ================================================================
 * Forward (.Infère)
 * ================================================================ */

/* Resout une methode fluente du Tenseur (enregistree par stdlib_tenseur_init,
 * toujours faite avant tout appel a .Infère). Rend NULL si absente. */
static NativeFn ia_tensor_method(const char* name) {
    return method_lookup(METHOD_TYPE_TENSOR, name, (int)strlen(name), NULL);
}

/*  — une couche porte-t-elle des poids entrainables ? Dense (poids +
 * biais) et Conv2D (noyau + biais) en ont ; activations/pooling/flatten non ;
 * BatchNorm porte gamma/beta (traites a part de weights/bias). */
static bool ia_layer_has_weights(IALayerKind kind) {
    return kind == IA_LAYER_DENSE || kind == IA_LAYER_CONV2D;
}

/*  — une couche a-t-elle des parametres a entrainer ? Dense/Conv
 * (poids + biais) et BatchNorm (gamma + beta). Les deux parametres sont exposes
 * de facon uniforme (A/B) pour que la boucle d'optimisation (SGD/Adam) traite
 * les trois familles par le meme chemin — sinon les poids conv/BN ne
 * s'entraineraient pas en silence. */
static bool ia_layer_trainable(IALayerKind kind) {
    return kind == IA_LAYER_DENSE || kind == IA_LAYER_CONV2D
        || kind == IA_LAYER_BATCHNORM;
}
static ObjTensor* ia_param_a(IALayer* L) {
    return (L->kind == IA_LAYER_BATCHNORM) ? L->gamma : L->weights;
}
static ObjTensor* ia_param_b(IALayer* L) {
    return (L->kind == IA_LAYER_BATCHNORM) ? L->beta : L->bias;
}
static void ia_set_param_a(IALayer* L, ObjTensor* t) {
    if (L->kind == IA_LAYER_BATCHNORM) L->gamma = t; else L->weights = t;
}
static void ia_set_param_b(IALayer* L, ObjTensor* t) {
    if (L->kind == IA_LAYER_BATCHNORM) L->beta = t; else L->bias = t;
}

/*  — applique UNE couche `L` a `cur` (rooté au sommet de la pile par
 * le caller). Rend le Tenseur produit (non roote — a rooter par le caller avant
 * toute autre allocation) ou NULL sur erreur. Chaque intermediaire (h avant
 * l'ajout du biais) est roote localement. Partage entre .Infère et .Entraîne :
 * un seul chemin de forward pour toutes les couches. */
static ObjTensor* ia_layer_forward(IALayer* L, ObjTensor* cur, bool eval) {
    switch (L->kind) {
        case IA_LAYER_DENSE: {
            NativeFn pm = ia_tensor_method("ProduitMatriciel");
            Value margs[2] = { OBJ_VAL(cur), OBJ_VAL(L->weights) };
            Value r = pm(2, margs);
            if (!IS_TENSOR(r)) return NULL;
            ObjTensor* h = AS_TENSOR(r);
            vm_push(OBJ_VAL(h));
            ObjTensor* hb = obj_tensor_binop(TENSOR_OP_ADD, h, L->bias);
            vm_pop();
            return hb;
        }
        case IA_LAYER_CONV2D: {
            NativeFn cv = ia_tensor_method("Convolution2D");
            int group = L->window > 0 ? L->window : 1;   /*  */
            Value cargs[5] = { OBJ_VAL(cur), OBJ_VAL(L->weights),
                               REAL_VAL((double)L->stride), REAL_VAL((double)L->pad),
                               REAL_VAL((double)group) };
            Value r = cv(5, cargs);
            if (!IS_TENSOR(r)) return NULL;
            ObjTensor* h = AS_TENSOR(r);
            vm_push(OBJ_VAL(h));   /* root h pendant l'ajout du biais [1,C,1,1] */
            ObjTensor* hb = obj_tensor_binop(TENSOR_OP_ADD, h, L->bias);
            vm_pop();
            return hb;
        }
        case IA_LAYER_MAXPOOL:
        case IA_LAYER_AVGPOOL: {
            int win = L->window;
            if (L->kind == IA_LAYER_AVGPOOL && win <= 0) {
                /* Moyenne spatiale globale (GlobalAveragePool importe) : fenetre =
                 * toute la carte, resolue a k = H ici (l'op MoyennePooling k×k sur
                 * une carte H×H produit un unique H×H -> C×1×1, moyenne exacte, et
                 * conserve l'autograd pour le fine-tune). Exige une carte carree. */
                if (cur->ndim < 4) return NULL;
                int H = cur->shape[2], Wd = cur->shape[3];
                if (H != Wd) {
                    owl_emit_native_error(E_NATIVE_IA_GLOBALPOOL_NON_CARRE,
                                          "Inf\xc3\xa8re", NULL);
                    return NULL;
                }
                win = H;
            }
            int st = L->stride > 0 ? L->stride : win;   /*  : pas (defaut = fenetre) */
            int pad = L->pad;                            /*  : remplissage (defaut 0) */
            NativeFn pf = ia_tensor_method(L->kind == IA_LAYER_MAXPOOL
                                           ? "MaxPooling" : "MoyennePooling");
            Value pargs[4] = { OBJ_VAL(cur), REAL_VAL((double)win),
                               REAL_VAL((double)st), REAL_VAL((double)pad) };
            Value r = pf(4, pargs);
            return IS_TENSOR(r) ? AS_TENSOR(r) : NULL;
        }
        case IA_LAYER_BATCHNORM: {
            /*  — .Entraîne (eval=Faux) : stats du lot + MAJ EMA des stats
             * mobiles. .Infère (eval=Vrai) : stats mobiles figees -> inference
             * correcte image par image (lot de 1). cur roote par le caller. */
            return tensor_batchnorm_forward(cur, L->gamma, L->beta, L->eps,
                                            L->running_mean, L->running_var,
                                            OWL_BN_MOMENTUM, eval);
        }
        case IA_LAYER_FLATTEN: {
            /* B×C×H×W -> B×(C·H·W). cur roote (pile) pendant l'unique alloc. */
            int B = cur->shape[0];
            int rest = tensor_numel(cur->data) / B;
            int sh[2] = { B, rest };
            return obj_tensor_reshape(cur, 2, sh);
        }
        case IA_LAYER_RELU:
        case IA_LAYER_SIGMOID:
        case IA_LAYER_TANH: {
            const char* mn = (L->kind == IA_LAYER_RELU)    ? "ReLU"
                           : (L->kind == IA_LAYER_SIGMOID) ? "Sigmoid"
                                                           : "Tanh";
            NativeFn af = ia_tensor_method(mn);
            Value aargs[1] = { OBJ_VAL(cur) };
            Value r = af(1, aargs);
            return IS_TENSOR(r) ? AS_TENSOR(r) : NULL;
        }
        case IA_LAYER_SILU: {
            /* SiLU (Swish) = x · sigmoide(x). Compose l'activation sigmoide et la
             * multiplication element-par-element existantes : le backward decoule
             * de la chaine autograd (SIGMOID puis MUL), aucun gradient nouveau a
             * ecrire. cur est roote par le caller ; la sortie sigmoide fraiche
             * doit l'etre pendant l'alloc du MUL (le GC ne scanne pas la C-stack). */
            NativeFn sf = ia_tensor_method("Sigmoid");
            Value sargs[1] = { OBJ_VAL(cur) };
            Value r = sf(1, sargs);
            if (!IS_TENSOR(r)) return NULL;
            ObjTensor* sig = AS_TENSOR(r);
            vm_push(OBJ_VAL(sig));
            ObjTensor* out = obj_tensor_binop(TENSOR_OP_MUL, cur, sig);
            vm_pop();
            return out;
        }
        case IA_LAYER_BRANCH_BEGIN:
        case IA_LAYER_BRANCH_NEXT:
        case IA_LAYER_MERGE_ADD:
        case IA_LAYER_MERGE_CONCAT:
            /*  +  — les marqueurs de branches pilotent
             * l'execution des branches paralleles : traites directement dans
             * ia_walk_layers, jamais ici. */
            return cur;
    }
    return NULL;
}

/*  — execute UN bloc a branches paralleles. A l'entree, `li` pointe sur
 * le marqueur BRANCH_BEGIN et OBJ_VAL(x) est au SOMMET de la pile VM (le tenseur
 * de divergence, rooté par le caller). Chaque branche est rejouee DEPUIS x, dans
 * l'ordre de declaration ; leurs sorties sont additionnees element-par-element.
 * Une branche vide (aucune couche entre deux marqueurs) vaut l'identite = x.
 *
 * Discipline GC (le GC ne scanne pas la C-stack, et il ne DEPLACE jamais les Obj
 * — mark & sweep) : x reste rooté par le caller pendant tout le bloc. On empile
 * successivement les sorties de branches ; chacune, une fois posee, sert d'ancre
 * de root aux allocations des branches suivantes. Le merge additionne dans
 * l'ordre : acc = out1, puis acc = add(acc, outk). L'accumulateur reste au
 * sommet, GC-visible, pendant chaque add. Rend le tenseur fusionne AVEC x
 * toujours au sommet (l'accumulateur est depile juste avant le retour ; le caller
 * ne fait AUCUNE allocation avant de le re-rooter) et pose *pli sur l'index qui
 * suit le MERGE_ADD. Rend NULL sur erreur (pile laissee avec le seul slot x). */
#define IA_MAX_BRANCHES 64
static ObjTensor* ia_run_branch_block(ObjIAModele* m, ObjTensor* x, bool eval,
                                      int* pli) {
    int li = *pli + 1;   /* premiere couche apres BRANCH_BEGIN */
    ObjTensor* acc = NULL;   /* accumulateur (fusion incrementale), toujours au sommet */
    int accPushed = 0;       /* 1 quand acc occupe un slot de pile a depiler */
    int nbranch = 0;

    /*  — mode de fusion du bloc : premier marqueur MERGE_* rencontre a
     * partir d'ici (pas d'imbrication, refusee plus bas). Determine si les
     * sorties de branches sont additionnees (MERGE_ADD) ou concatenees sur l'axe
     * canal (MERGE_CONCAT). */
    IALayerKind mergeKind = IA_LAYER_MERGE_ADD;
    for (int s = li; s < m->count; s++) {
        if (m->layers[s].kind == IA_LAYER_MERGE_ADD) { mergeKind = IA_LAYER_MERGE_ADD; break; }
        if (m->layers[s].kind == IA_LAYER_MERGE_CONCAT) { mergeKind = IA_LAYER_MERGE_CONCAT; break; }
    }

    while (true) {
        /* Rejoue la branche courante depuis x : slot de travail au sommet. */
        vm_push(OBJ_VAL(x));
        ObjTensor* bcur = x;
        while (li < m->count
               && m->layers[li].kind != IA_LAYER_BRANCH_NEXT
               && m->layers[li].kind != IA_LAYER_MERGE_ADD
               && m->layers[li].kind != IA_LAYER_MERGE_CONCAT) {
            IALayer* L = &m->layers[li];
            if (L->kind == IA_LAYER_BRANCH_BEGIN) {
                /* Branches imbriquees : hors perimetre (refus explicite plutot
                 * qu'execution silencieuse fausse). */
                owl_emit_native_error(E_NATIVE_IA_BRANCHE_OUVERT, "Inf\xc3\xa8re", NULL);
                vm_pop();                 /* slot de travail */
                if (accPushed) vm_pop();  /* acc */
                return NULL;
            }
            ObjTensor* next = ia_layer_forward(L, bcur, eval);
            if (next == NULL) {
                vm_pop();                 /* slot de travail */
                if (accPushed) vm_pop();  /* acc */
                return NULL;
            }
            vm_pop();                     /* ancien bcur */
            vm_push(OBJ_VAL(next));
            bcur = next;
            li++;
        }
        /* bcur = sortie de cette branche (au sommet). Branche vide -> bcur == x. */
        nbranch++;
        if (nbranch > IA_MAX_BRANCHES) {
            vm_pop();
            if (accPushed) vm_pop();
            owl_emit_native_error(E_NATIVE_IA_BRANCHE_OUVERT, "Inf\xc3\xa8re", NULL);
            return NULL;
        }
        if (acc == NULL) {
            /* 1re branche : le slot de travail devient le slot de l'accumulateur. */
            acc = bcur;
            accPushed = 1;
        } else {
            /* Pile : [..., x, acc, bcur]. Les deux operandes rootes pendant l'op. */
            ObjTensor* combined = NULL;
            if (mergeKind == IA_LAYER_MERGE_CONCAT) {
                /* Concatenation canal : formes egales sur tous les axes sauf le
                 * canal (verifie par obj_tensor_concat_ch qui rend NULL sinon). */
                combined = obj_tensor_concat_ch(acc, bcur);
                if (combined == NULL) {
                    owl_emit_native_error(E_NATIVE_IA_BRANCHE_CONCAT_FORME, "Inf\xc3\xa8re", NULL);
                    vm_pop();   /* bcur */
                    vm_pop();   /* acc */
                    return NULL;
                }
            } else {
                if (!tensor_shapes_equal(acc, bcur)) {
                    owl_emit_native_error(E_NATIVE_IA_BRANCHE_FORME, "Inf\xc3\xa8re", NULL);
                    vm_pop();   /* bcur */
                    vm_pop();   /* acc */
                    return NULL;
                }
                combined = obj_tensor_binop(TENSOR_OP_ADD, acc, bcur);
                if (combined == NULL) {
                    vm_pop();   /* bcur */
                    vm_pop();   /* acc */
                    return NULL;
                }
            }
            vm_pop();   /* bcur */
            vm_pop();   /* ancien acc */
            vm_push(OBJ_VAL(combined));
            acc = combined;
        }
        /* Marqueur de fin de branche. */
        if (li >= m->count) {
            /* Bloc ouvert jamais fusionne. */
            owl_emit_native_error(E_NATIVE_IA_BRANCHE_OUVERT, "Inf\xc3\xa8re", NULL);
            if (accPushed) vm_pop();
            return NULL;
        }
        if (m->layers[li].kind == IA_LAYER_MERGE_ADD
            || m->layers[li].kind == IA_LAYER_MERGE_CONCAT) { li++; break; }
        li++;   /* BRANCH_NEXT : passe a la branche suivante */
    }

    /* Depile l'accumulateur ; x reste au sommet pour le caller (aucune alloc
     * avant que le caller ne re-roote la valeur rendue). */
    if (accPushed) vm_pop();
    *pli = li;
    return acc;
}

/*  — marche partagee sur toutes les couches, utilisee par .Infère
 * (eval=Vrai) et .Entraîne (eval=Faux). Les couches sequentielles passent par
 * ia_layer_forward ; un bloc a branches paralleles (BRANCH_BEGIN ... MERGE_ADD)
 * est delegue a ia_run_branch_block.
 *
 * Precondition : OBJ_VAL(cur) est au SOMMET de la pile VM (rooté par le caller).
 * Chaque op de couche alloue ; l'entree de travail (sommet) doit survivre. Rend
 * le tenseur final (au sommet) ou NULL. Dans LES DEUX cas l'unique slot de
 * travail reste au sommet : le caller le depile lui-meme (les trois appelants —
 * .Infère, perte de validation, boucle d'entrainement — depilent ce slot apres
 * le retour, succes comme echec). Sur echec d'un bloc a branches,
 * ia_run_branch_block laisse deja x (ce slot) au sommet. */
static ObjTensor* ia_walk_layers(ObjIAModele* m, ObjTensor* cur, bool eval) {
    int li = 0;
    while (li < m->count) {
        IALayer* L = &m->layers[li];
        if (L->kind == IA_LAYER_BRANCH_BEGIN) {
            /* cur (= x, sommet) reste rooté ; le bloc rend le tenseur fusionne. */
            ObjTensor* merged = ia_run_branch_block(m, cur, eval, &li);
            if (merged == NULL) return NULL;   /* x (slot de travail) laisse au caller */
            vm_pop();                     /* x */
            vm_push(OBJ_VAL(merged));
            cur = merged;
            continue;
        }
        if (L->kind == IA_LAYER_BRANCH_NEXT || L->kind == IA_LAYER_MERGE_ADD
            || L->kind == IA_LAYER_MERGE_CONCAT) {
            /* Marqueur de branche hors d'un bloc ouvert (modele charge/bricole) :
             * refus defensif plutot que comportement silencieux. */
            owl_emit_native_error(E_NATIVE_IA_BRANCHE_SANS_DEBUT, "Inf\xc3\xa8re", NULL);
            return NULL;   /* slot de travail laisse au caller */
        }
        ObjTensor* next = ia_layer_forward(L, cur, eval);
        if (next == NULL) return NULL;   /* slot de travail laisse au caller */
        /* Remplace le cur de travail (sommet) par next — aucune alloc entre les
         * deux, next reste sain. */
        vm_pop();
        vm_push(OBJ_VAL(next));
        cur = next;
        li++;
    }
    return cur;
}

/*  — coeur du forward d'inference, execute SOUS g_no_grad (arme par
 * l'appelant fn_iamodele_infere) : aucun noeud de graphe ni buffer grad n'est
 * alloue, les activations intermediaires deviennent collectables au fil de
 * l'eau. `input` reste rooté par l'appelant via args[1] (slot de pile VM) durant
 * tout ce forward. */
/*  — exécuteur de graphe général, défini plus bas (section import ONNX).
 * Déclaré ici car appelé par fn_iamodele_infere. */
static Value ia_graph_forward(ObjIAModele* m, ObjTensor* input);

static Value ia_infere_forward(ObjIAModele* m, ObjTensor* input) {
    /* Un echantillon 1D est promu en (1×in) pour emprunter le chemin matriciel,
     * puis la sortie ramenee en 1D. Un lot 2D ou une entree spatiale 4D
     * (modele convolutif) passe TEL QUEL en entree : pas de promotion. */
    bool was1D = (input->ndim == 1);
    /* Un unique echantillon : soit une entree 1D (promue ci-dessus), soit une
     * entree spatiale de rang >= 3 dont la dimension de lot vaut 1 (image unique
     * 4D 1×C×H×W d'un classifieur convolutif). Dans ce cas, une sortie 2D
     * 1×classes est demotee en Tenseur 1D, coherent avec le chemin dense
     *. Le seuil est rang >= 3 (et non >= 2) pour ne PAS toucher un
     * lot dense 2D de taille 1 (entree 1×in), dont la sortie 2D 1×out reste 2D
     * (semantique lot). Un vrai lot (shape[0] > 1) garde aussi sa sortie 2D. */
    bool singleSample = was1D || (input->ndim >= 3 && input->shape[0] == 1);

    /* cur reste ROOTE au sommet de la pile pendant tout le forward : chaque op
     * de couche alloue un nouveau Tenseur, et l'entree de l'op doit survivre a
     * cette allocation. Les poids/biais des couches survivent via le modele
     * (args[0], roote par le dispatch, trace ses tenseurs). */
    ObjTensor* cur = input;
    if (was1D) {
        /* Promotion (1×in) : input roote via args[1] pendant l'alloc. */
        int n = tensor_numel(input->data);
        ObjTensor* p = obj_new_tensor_matrix(1, n);
        double* pd = tensor_cells(p->data);
        double* id = tensor_cells(input->data);
        for (int i = 0; i < n; i++) pd[i] = id[i];
        cur = p;
    }
    vm_push(OBJ_VAL(cur));

    ObjTensor* out = ia_walk_layers(m, cur, /*eval=*/true);
    if (out == NULL) { vm_pop(); return NIL_VAL; }
    cur = out;

    if (singleSample && cur->ndim == 2 && cur->shape[0] == 1) {
        /* Demote (1×out) -> Tenseur 1D : cur roote (pile) pendant l'alloc.
         * Gate stricte cur->ndim == 2 : une carte de features 4D (conv sans tete
         * dense) reste intacte. */
        ObjMatrix* cm = (ObjMatrix*)cur->data;
        int out = cm->cols;
        ObjTensor* v = obj_new_tensor(out);
        double* vd = tensor_cells(v->data);
        for (int i = 0; i < out; i++) vd[i] = cm->data[i];
        vm_pop();
        vm_push(OBJ_VAL(v));
        cur = v;
    }

    vm_pop();   /* cur */
    return OBJ_VAL(cur);
}

/* .Infère(x) / .Infer(x) — forward. x est un Tenseur 1D (echantillon) ou 2D
 * (lot B×entrees). Rend un Tenseur de meme rang (1D->1D, 2D->2D). En interne,
 * un 1D est promu en (1×in), tout le forward se fait en 2D via les ops
 * matricielles du Tenseur, puis la sortie (1×out) est ramenee en 1D.
 *
 *  — l'inference n'a AUCUN besoin du graphe autograd : on arme g_no_grad
 * autour du forward (pas de noeud d'op ni de buffer grad -> pic memoire coupe).
 * Sauvegarde/restauration EXCEPTION-SAFE : l'ancienne valeur est restauree meme
 * si le forward echoue (une erreur native est un soft-error qui rend NIL, jamais
 * un longjmp — le restore est donc toujours atteint). On ne force pas `faux`
 * aveuglement : un `.Infère` imbrique (peu probable) retrouve l'etat exact. */
static Value fn_iamodele_infere(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (!IS_TENSOR(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Inf\xc3\xa8re",
                              "arg", "entree",
                              "expected", "Tenseur 1D ou 2D",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjTensor* input = AS_TENSOR(args[1]);
    bool savedNoGrad = g_no_grad;
    g_no_grad = true;
    /*  — modèle importé en graphe général (DAG à fan-out) : exécuteur
     * topologique. Sinon chemin séquentiel classique (couches layers[]). */
    Value result = (m->ngnodes > 0)
                 ? ia_graph_forward(m, input)
                 : ia_infere_forward(m, input);
    g_no_grad = savedNoGrad;
    return result;
}

/* ================================================================
 * Introspection (.Poids / .Biais)
 * ================================================================ */

/* Rend le Tenseur (poids ou biais) de la n-ieme couche DENSE (index base-1,
 * les activations ne comptent pas). `wantWeights` choisit poids vs biais. */
static Value ia_dense_tensor(Value* args, bool wantWeights, const char* name) {
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (!IS_NUMBER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, name,
                              "arg", "indice",
                              "expected", "entier (indice de couche dense, base 1)",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int n = (int)AS_NUMBER(args[1]);
    int weightIdx = 0;
    for (int i = 0; i < m->count; i++) {
        if (!ia_layer_has_weights(m->layers[i].kind)) continue;
        weightIdx++;
        if (weightIdx == n) {
            return OBJ_VAL(wantWeights ? m->layers[i].weights
                                       : m->layers[i].bias);
        }
    }
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, name,
                          "arg", "indice",
                          "expected", "indice d'une couche a poids existante (dense ou convolution)",
                          "actual", "hors plage", NULL);
    return NIL_VAL;
}
static Value fn_iamodele_poids(int argCount, Value* args) {
    (void)argCount; return ia_dense_tensor(args, true, "Poids");
}
static Value fn_iamodele_biais(int argCount, Value* args) {
    (void)argCount; return ia_dense_tensor(args, false, "Biais");
}

/* ================================================================
 * OptionsEntraînement — entrainement SGD (.Entraîne)
 * ================================================================ */

/* Factory interne emise par le compilateur pour `opts est un
 * OptionsEntraînement` : instance FRAICHE avec les defauts a chaque
 * declaration (config mutable, un singleton partage casserait tout). */
static Value fn_options_entrainement_nouveau(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_options_entrainement());
}

/* Extrait les echantillons [start, start+count) le long du PREMIER axe d'un
 * Tenseur source en une FEUILLE fraiche de meme forme trailing (count×...). Le
 * premier axe est l'axe de lot : 2D (N×cols) pour un modele dense, 4D
 * (N×C×H×W) pour un modele convolutif. obj_new_tensor_nd se root pendant
 * l'alloc de sa donnee ; ensuite plus aucune allocation OWL (copie de doubles). */
static ObjTensor* ia_build_batch(ObjTensor* src, int start, int count) {
    int perSample = tensor_numel(src->data) / src->shape[0];
    int outShape[OWL_TENSOR_MAX_RANK];
    outShape[0] = count;
    for (int i = 1; i < src->ndim; i++) outShape[i] = src->shape[i];
    ObjTensor* t = obj_new_tensor_nd(src->ndim, outShape);
    double* out = tensor_cells(t->data);
    double* sd  = tensor_cells(src->data);
    memcpy(out, sd + (size_t)start * (size_t)perSample,
           (size_t)count * (size_t)perSample * sizeof(double));
    return t;
}

/* Mise a jour Adam d'UN parametre (poids ou biais). Lit valeur + gradient sur
 * `src` (l'ancien Tenseur, encore roote via le modele), met a jour les moments
 * m/v EN PLACE (tableaux C-heap `mArr`/`vArr`, memes formes que le parametre),
 * applique la correction de biais (bc1 = 1 - β1^t, bc2 = 1 - β2^t, communs a
 * tous les parametres du pas courant) et rend une FEUILLE FRAICHE θ_new — meme
 * pattern que DescenteGradient : l'ancien Tenseur garde son gradient, le modele
 * remplace sa reference. obj_new_tensor* se root pendant l'alloc de sa donnee ;
 * au-dela, plus aucune allocation OWL (lecture/ecriture de doubles). */
static ObjTensor* ia_adam_step(ObjTensor* src, double* mArr, double* vArr,
                               double taux, double bc1, double bc2) {
    /* Feuille de MEME forme N-D que le parametre (poids 2D, biais 1D, noyau
     * conv 4D depuis ) : obj_new_tensor_nd preserve le rang. */
    ObjTensor* t = obj_new_tensor_nd(src->ndim, src->shape);
    int     nel = tensor_numel(src->data);
    double* out = tensor_cells(t->data);
    double* sd  = tensor_cells(src->data);
    double* sg  = tensor_cells(src->grad);
    for (int i = 0; i < nel; i++) {
        double g = sg[i];
        mArr[i] = OWL_ADAM_BETA1 * mArr[i] + (1.0 - OWL_ADAM_BETA1) * g;
        vArr[i] = OWL_ADAM_BETA2 * vArr[i] + (1.0 - OWL_ADAM_BETA2) * g * g;
        double mhat = mArr[i] / bc1;
        double vhat = vArr[i] / bc2;
        out[i] = sd[i] - taux * mhat / (sqrt(vhat) + OWL_ADAM_EPS);
    }
    return t;
}

/*  — weight decay (regularisation L2), forme COUPLEE classique : on
 * ajoute lambda*poids au gradient BRUT du parametre AVANT la mise a jour, de
 * sorte que le gradient effectif devienne g + lambda*w. Applique in-place sur
 * le gradient du Tenseur courant (consomme aussitot par DescenteGradient / Adam,
 * puis remplace par une feuille fraiche). Choix couple (pas AdamW decouple) :
 * meme terme pour SGD et Adam, simple et coherent. lambda == 0 -> sortie
 * immediate, gradient intact -> non-regression EXACTE des entrainements sans
 * weight decay. */
static void ia_apply_weight_decay(ObjTensor* p, double lambda) {
    if (lambda == 0.0) return;
    int     nel = tensor_numel(p->data);
    double* d   = tensor_cells(p->data);
    double* g   = tensor_cells(p->grad);
    for (int i = 0; i < nel; i++) g[i] += lambda * d[i];
}

/*  — perte de validation d'une passe complete. Métrique de sélection
 * UNIQUE du save-best / early-stopping : la perte (plus basse = meilleure).
 * Choix face à l'accuracy (argmax) : universelle — elle a un sens sur la
 * classification ET la régression (un modèle dense(1,1) sur y=2x n'a pas de
 * classe à argmaxer), alors que l'accuracy n'en aurait pas. Forward SOUS
 * g_no_grad (comme .Infère, ) : aucun noeud de graphe ni buffer grad,
 * donc aucun effet sur les gradients d'entrainement (Rétropropage n'est jamais
 * appelé ici). Xv/Yv rootés par le caller ; on pousse un slot de travail au
 * sommet pour ia_walk_layers, rééquilibré avant de rendre. Rend HUGE_VAL si le
 * forward ou la perte échoue (jamais retenu comme "meilleur"). */
static double ia_compute_val_loss(ObjIAModele* m, ObjTensor* Xv, ObjTensor* Yv,
                                  NativeFn lossFn) {
    bool   savedNoGrad = g_no_grad;
    g_no_grad = true;
    double loss = HUGE_VAL;
    vm_push(OBJ_VAL(Xv));   /* slot de travail (sommet) pour ia_walk_layers */
    ObjTensor* out = ia_walk_layers(m, Xv, /*eval=*/true);
    if (out != NULL) {
        Value largs[2] = { OBJ_VAL(out), OBJ_VAL(Yv) };
        Value lr = lossFn(2, largs);
        if (IS_TENSOR(lr)) loss = tensor_cells(AS_TENSOR(lr)->data)[0];
    }
    vm_pop();   /* out en succes, dernier cur en echec — un seul slot laissé */
    g_no_grad = savedNoGrad;
    return loss;
}

/*  — snapshot des VALEURS de tous les parametres entrainables dans des
 * buffers C-heap deja alloues (bufA/bufB indexes par couche, tailles = numel des
 * params). Copie de doubles uniquement, aucun objet OWL : les tenseurs poids
 * sont reassignes a chaque pas (ia_set_param_* cree de nouveaux tenseurs), donc
 * on capture les valeurs, pas la reference. Les 3 familles entrainables (dense,
 * conv, BN gamma/beta) passent par le meme couple param_a/param_b que SGD/Adam.
 *
 *  — les couches BatchNorm portent AUSSI un etat NON entrainable mais
 * indispensable a l'inference eval : running_mean/running_var (stats mobiles
 * mises a jour par EMA a chaque epoque, ). Sans les capturer, le
 * save-best restaure gamma/beta de la meilleure epoque mais laisse les stats
 * mobiles de la derniere -> BN desynchronise -> sortie effondree. bufRM/bufRV
 * sont indexes par couche (NULL hors BN), dimensionnes par le numel des stats
 * mobiles (= nombre de canaux). Le tenseur des stats est reutilise en place
 * (tensor_batchnorm_forward ecrit dans son data), donc on capture les valeurs. */
static void ia_snapshot_params(ObjIAModele* m, double** bufA, double** bufB,
                               double** bufRM, double** bufRV) {
    for (int li = 0; li < m->count; li++) {
        IALayer* L = &m->layers[li];
        if (!ia_layer_trainable(L->kind) || L->frozen) continue;   /*  : couche gelee sautee par l'optimiseur */
        ObjTensor* a = ia_param_a(L);
        ObjTensor* b = ia_param_b(L);
        memcpy(bufA[li], tensor_cells(a->data),
               (size_t)tensor_numel(a->data) * sizeof(double));
        memcpy(bufB[li], tensor_cells(b->data),
               (size_t)tensor_numel(b->data) * sizeof(double));
        if (L->kind == IA_LAYER_BATCHNORM) {
            memcpy(bufRM[li], tensor_cells(L->running_mean->data),
                   (size_t)tensor_numel(L->running_mean->data) * sizeof(double));
            memcpy(bufRV[li], tensor_cells(L->running_var->data),
                   (size_t)tensor_numel(L->running_var->data) * sizeof(double));
        }
    }
}

/*  — réécrit les valeurs snapshotées dans les tenseurs de params
 * COURANTS (memes formes que lors du snapshot : architecture figée pendant
 * l'entrainement). On écrit dans le data du tenseur courant plutot que de
 * remplacer la reference : inutile d'allouer, et la forme est garantie
 * identique.
 *
 *  — restaure aussi running_mean/running_var des couches BatchNorm
 * (memes formes, architecture figée), pour que les stats mobiles restaurées
 * restent coherentes avec gamma/beta de la meme epoque. */
static void ia_restore_params(ObjIAModele* m, double** bufA, double** bufB,
                              double** bufRM, double** bufRV) {
    for (int li = 0; li < m->count; li++) {
        IALayer* L = &m->layers[li];
        if (!ia_layer_trainable(L->kind) || L->frozen) continue;   /*  : couche gelee sautee par l'optimiseur */
        ObjTensor* a = ia_param_a(L);
        ObjTensor* b = ia_param_b(L);
        memcpy(tensor_cells(a->data), bufA[li],
               (size_t)tensor_numel(a->data) * sizeof(double));
        memcpy(tensor_cells(b->data), bufB[li],
               (size_t)tensor_numel(b->data) * sizeof(double));
        if (L->kind == IA_LAYER_BATCHNORM) {
            memcpy(tensor_cells(L->running_mean->data), bufRM[li],
                   (size_t)tensor_numel(L->running_mean->data) * sizeof(double));
            memcpy(tensor_cells(L->running_var->data), bufRV[li],
                   (size_t)tensor_numel(L->running_var->data) * sizeof(double));
        }
    }
}

/* .Entraîne(X, Y, opts) / .Train — boucle d'entrainement complete, mute le
 * modele EN PLACE (aucune valeur de retour). X = Tenseur 2D (N×entrees),
 * Y = Tenseur 2D (N×sorties). Pour chaque epoque, decoupe en mini-lots de
 * ..TailleLot (ordre deterministe, pas de melange : les problemes cibles sont
 * convexes) ; par lot : forward a travers les couches -> perte (selon ..Perte)
 * -> Rétropropage -> mise a jour SGD de TOUS les poids/biais denses via
 * DescenteGradient(taux). Le garde-fou reset-defaut du gradient (chaque
 * Rétropropage remet a zero les gradients du graphe) evite tout zero-grad
 * manuel entre les pas. */
static Value fn_iamodele_entraine(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);
    if (!IS_TENSOR(args[1]) || !IS_TENSOR(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "donnees",
                              "expected", "X et Y Tenseurs 2D",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    if (!IS_OPTIONS_ENTRAINEMENT(args[3])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "options",
                              "expected", "OptionsEntra\xc3\xaenement",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjTensor* X = AS_TENSOR(args[1]);
    ObjTensor* Y = AS_TENSOR(args[2]);
    ObjOptionsEntrainement* opts = AS_OPTIONS_ENTRAINEMENT(args[3]);

    /* X : lot 2D (N×entrees) pour un modele dense, ou 4D (N×C×H×W) pour un
     * modele convolutif. Y : cible 2D (N×sorties). Le premier axe
     * est toujours l'axe de lot. */
    if (X->ndim < 2 || !tensor_is_2d(Y)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "donnees",
                              "expected", "X (2D N x entrees, ou 4D N x C x H x L) et Y 2D (N x sorties)",
                              "actual", "rang inattendu", NULL);
        return NIL_VAL;
    }
    ObjMatrix* Ym = (ObjMatrix*)Y->data;
    int N = X->shape[0];
    if (Ym->rows != N) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Entra\xc3\xaene",
                              "arg", "donnees",
                              "expected", "X et Y avec le meme nombre d'echantillons",
                              "actual", "nombres de lignes differents", NULL);
        return NIL_VAL;
    }

    int    epochs = opts->epochs;
    int    bs     = opts->batchSize < 1 ? 1 : opts->batchSize;
    double taux   = opts->learningRate;
    double lambda = opts->weightDecay;   /*  : coeff L2 (0.0 = off) */

    /*  — split validation DÉTERMINISTE : les floor(N·fraction) DERNIERS
     * échantillons de (X, Y) sont réservés pour la validation, l'entrainement se
     * fait sur les nTrain premiers. Déterministe (pilier 2) : à données fixes le
     * split est reproductible ; le user mélange en amont s'il le souhaite (aucun
     * mélange interne). Convention "derniers échantillons" : simple à raisonner
     * et sans coût de permutation. fraction 0.0 -> nVal 0 -> nTrain N -> AUCUN
     * changement (non-régression EXACTE, cf. test_IAModeleEntraineNonRegression).
     * Le sous-ensemble validation est constitué de tenseurs déjà matérialisés :
     * aucune augmentation ne peut s'y appliquer (elle vit au niveau Dataset). */
    double valFrac  = opts->validationFraction;
    int    keepBest = opts->keepBestModel;   /* bool 0/1 */
    int    patience = opts->stopPatience;
    int    nVal     = (valFrac > 0.0) ? (int)floor((double)N * valFrac) : 0;
    if (nVal > N) nVal = N;
    int    nTrain   = N - nVal;
    /* Éval validation utile uniquement si un mécanisme la consomme (save-best ou
     * early-stopping) et qu'il reste des échantillons de validation. */
    bool   doValEval = (nVal > 0) && (keepBest || patience > 0);

    /*  — buffers de save-best : copie C-heap des valeurs des params
     * entrainables à l'époque de MEILLEURE perte de validation. Alloués une fois
     * (par couche), libérés en fin d'appel. NULL si le save-best n'est pas actif
     * -> zéro surcout mémoire aux défauts. */
    double **bestA = NULL, **bestB = NULL;
    /*  — buffers des stats mobiles BN (running_mean/running_var), memes
     * conventions que bestA/bestB : indexes par couche, NULL hors BatchNorm. */
    double **bestRM = NULL, **bestRV = NULL;
    bool   haveBest = false;
    double bestValLoss = HUGE_VAL;
    int    epochsSinceImprove = 0;
    if (doValEval && keepBest) {
        bestA = calloc((size_t)m->count, sizeof(double*));
        bestB = calloc((size_t)m->count, sizeof(double*));
        bestRM = calloc((size_t)m->count, sizeof(double*));
        bestRV = calloc((size_t)m->count, sizeof(double*));
        for (int li = 0; li < m->count; li++) {
            IALayer* L = &m->layers[li];
            if (!ia_layer_trainable(L->kind) || L->frozen) continue;   /*  : couche gelee sautee par l'optimiseur */
            bestA[li] = malloc((size_t)tensor_numel(ia_param_a(L)->data) * sizeof(double));
            bestB[li] = malloc((size_t)tensor_numel(ia_param_b(L)->data) * sizeof(double));
            if (L->kind == IA_LAYER_BATCHNORM) {
                bestRM[li] = malloc((size_t)tensor_numel(L->running_mean->data) * sizeof(double));
                bestRV[li] = malloc((size_t)tensor_numel(L->running_var->data) * sizeof(double));
            }
        }
    }

    NativeFn retro    = ia_tensor_method("R\xc3\xa9tropropage");
    NativeFn descente = ia_tensor_method("DescenteGradient");
    NativeFn lossFn   = (opts->loss == OWL_PERTE_ERREUR_QUADRATIQUE)
                      ? ia_tensor_method("ErreurQuadratique")
                      : ia_tensor_method("EntropieCrois\xc3\xa9" "e");

    /* Etat Adam (optimAdam) : par parametre (poids ET biais de chaque couche
     * dense), deux moments m et v de meme forme, init a zero, remis a zero a
     * chaque appel de .Entraîne. Tableaux C-heap indexes par la couche (les
     * emplacements des couches d'activation restent NULL, jamais dereferences).
     * Ils vivent le temps de l'appel, ne sont jamais exposes a l'utilisateur.
     * Le compteur de pas `t` est GLOBAL a l'entrainement (partage par tous les
     * parametres) et incremente a chaque mini-lot. */
    bool useAdam = (opts->optimizer == OWL_OPTIM_ADAM);
    double **mW = NULL, **vW = NULL, **mB = NULL, **vB = NULL;
    long adamStep = 0;
    if (useAdam) {
        mW = calloc((size_t)m->count, sizeof(double*));
        vW = calloc((size_t)m->count, sizeof(double*));
        mB = calloc((size_t)m->count, sizeof(double*));
        vB = calloc((size_t)m->count, sizeof(double*));
        for (int li = 0; li < m->count; li++) {
            IALayer* L = &m->layers[li];
            if (!ia_layer_trainable(L->kind) || L->frozen) continue;   /*  : couche gelee sautee par l'optimiseur */
            int nw = tensor_numel(ia_param_a(L)->data);
            int nb = tensor_numel(ia_param_b(L)->data);
            mW[li] = calloc((size_t)nw, sizeof(double));
            vW[li] = calloc((size_t)nw, sizeof(double));
            mB[li] = calloc((size_t)nb, sizeof(double));
            vB[li] = calloc((size_t)nb, sizeof(double));
        }
    }

    /*  — callback de progression ..SurÉpoque/..OnEpoch : reference de
     * procedure rappelee a la fin de chaque epoque avec (numÉpoque 1-based,
     * totalÉpoques, perte moyenne de l'epoque). Non affecte -> zero surcout :
     * la perte moyenne n'est accumulee QUE si un callback est present, et sa
     * lecture (scalaire deja calcule pour la retropropagation) ne change rien
     * au resultat de l'entrainement. opts (args[3]) reste rooté sur la pile
     * durant tout l'appel natif, donc cb l'est via le tracage GC du champ. */
    bool  hasCallback = IS_CALLABLE(opts->onEpoch);
    Value cb          = opts->onEpoch;

    for (int e = 0; e < epochs; e++) {
        /*  — taux effectif de l'epoque, recalcule UNE FOIS au debut de
         * chaque epoque (par epoque, PAS par mini-lot). planConstant : taux
         * inchange. planCosinus (recuit cosinus) : taux * 0.5 * (1 + cos(pi*e/E))
         * sur E = `epochs` epoques, e etant 0-based. e=0 -> taux plein
         * (cos(0)=1) ; e=E-1 -> proche de 0. */
        double tauxEff = taux;
        if (opts->lrSchedule == OWL_PLAN_COSINUS && epochs > 0) {
            tauxEff = taux * 0.5 *
                      (1.0 + cos(OWL_IA_PI * (double)e / (double)epochs));
        }
        double epochLossSum = 0.0;
        int    epochBatches = 0;
        /*  — entrainement sur les nTrain premiers échantillons
         * uniquement (= N quand la validation est désactivée, boucle inchangée). */
        for (int start = 0; start < nTrain; start += bs) {
            int count = (start + bs <= nTrain) ? bs : (nTrain - start);

            /* Mini-lot : deux feuilles fraiches. Xb rooté pendant l'alloc de Yb.
             * Pile finale [Xb, Yb, cur] : Xb au fond (redondant mais inoffensif),
             * Yb rooté jusqu'a la perte, cur (=sommet) echange pendant le
             * forward. */
            ObjTensor* Xb = ia_build_batch(X, start, count);
            vm_push(OBJ_VAL(Xb));
            ObjTensor* Yb = ia_build_batch(Y, start, count);
            vm_push(OBJ_VAL(Yb));
            ObjTensor* cur = Xb;
            vm_push(OBJ_VAL(cur));

            ObjTensor* out = ia_walk_layers(m, cur, /*eval=*/false);
            bool ok = (out != NULL);
            if (ok) cur = out;   /* out est au sommet de pile (rooté) */

            if (ok) {
                /* Perte scalaire (cur = sortie, Yb = cible, tous deux rootés)
                 * puis retropropagation (aucune allocation OWL). */
                Value largs[2] = { OBJ_VAL(cur), OBJ_VAL(Yb) };
                Value lr = lossFn(2, largs);
                if (IS_TENSOR(lr)) {
                    /*  — accumule la perte scalaire du mini-lot pour la
                     * moyenne d'epoque (uniquement si un callback l'attend). Le
                     * scalaire vient d'etre calcule pour la retropropagation ;
                     * on le lit avant toute autre allocation OWL. */
                    if (hasCallback) {
                        ObjTensor* lt = AS_TENSOR(lr);
                        epochLossSum += tensor_cells(lt->data)[0];
                        epochBatches++;
                    }
                    Value bargs[1] = { lr };
                    retro(1, bargs);
                }
            }

            vm_pop();   /* cur */
            vm_pop();   /* Yb */
            vm_pop();   /* Xb */

            if (!ok) goto cleanup;

            /* Mise a jour des poids/biais denses (feuille fraiche par parametre).
             * Le modele (args[0]) reste roote ; chaque nouveau Tenseur est
             * enraciné via la couche des son assignation, avant l'allocation
             * suivante. Les gradients viennent d'etre poses par Rétropropage
             * dans les Tenseurs poids/biais actuels (rootés via le modele). */
            if (useAdam) {
                /* Adam : t global incremente une fois par mini-lot, correction
                 * de biais commune a tous les parametres du pas courant. */
                adamStep++;
                double bc1 = 1.0 - pow(OWL_ADAM_BETA1, (double)adamStep);
                double bc2 = 1.0 - pow(OWL_ADAM_BETA2, (double)adamStep);
                for (int li = 0; li < m->count; li++) {
                    IALayer* L = &m->layers[li];
                    if (!ia_layer_trainable(L->kind) || L->frozen) continue;   /*  : couche gelee sautee par l'optimiseur */
                    /*  — weight decay couple : g += lambda*w avant le pas. */
                    ia_apply_weight_decay(ia_param_a(L), lambda);
                    ia_apply_weight_decay(ia_param_b(L), lambda);
                    ia_set_param_a(L, ia_adam_step(ia_param_a(L), mW[li], vW[li],
                                                   tauxEff, bc1, bc2));
                    ia_set_param_b(L, ia_adam_step(ia_param_b(L), mB[li], vB[li],
                                                   tauxEff, bc1, bc2));
                }
            } else {
                /* SGD : poids - taux*gradient, sans etat accumule. */
                for (int li = 0; li < m->count; li++) {
                    IALayer* L = &m->layers[li];
                    if (!ia_layer_trainable(L->kind) || L->frozen) continue;   /*  : couche gelee sautee par l'optimiseur */
                    /*  — weight decay couple : g += lambda*w avant le pas. */
                    ia_apply_weight_decay(ia_param_a(L), lambda);
                    Value wargs[2] = { OBJ_VAL(ia_param_a(L)), REAL_VAL(tauxEff) };
                    Value wr = descente(2, wargs);
                    if (IS_TENSOR(wr)) ia_set_param_a(L, AS_TENSOR(wr));
                    ia_apply_weight_decay(ia_param_b(L), lambda);
                    Value bargs[2] = { OBJ_VAL(ia_param_b(L)), REAL_VAL(tauxEff) };
                    Value br = descente(2, bargs);
                    if (IS_TENSOR(br)) ia_set_param_b(L, AS_TENSOR(br));
                }
            }
        }

        /*  — éval validation APRÈS la passe d'entrainement de l'époque :
         * perte de validation sur les nVal derniers échantillons, en no-grad.
         * Sélection sur perte STRICTEMENT plus basse (< bestValLoss) : la 1re
         * époque bat toujours HUGE_VAL. Sur amélioration -> snapshot des poids si
         * save-best actif + compteur de patience remis à 0 ; sinon compteur
         * incrémenté. Xv/Yv : feuilles fraiches rootées le temps de l'éval. */
        if (doValEval) {
            ObjTensor* Xv = ia_build_batch(X, nTrain, nVal);
            vm_push(OBJ_VAL(Xv));
            ObjTensor* Yv = ia_build_batch(Y, nTrain, nVal);
            vm_push(OBJ_VAL(Yv));
            double vloss = ia_compute_val_loss(m, Xv, Yv, lossFn);
            vm_pop();   /* Yv */
            vm_pop();   /* Xv */
            if (vloss < bestValLoss) {
                bestValLoss = vloss;
                epochsSinceImprove = 0;
                if (keepBest) { ia_snapshot_params(m, bestA, bestB, bestRM, bestRV); haveBest = true; }
            } else {
                epochsSinceImprove++;
            }
        }

        /*  — fin d'epoque : rappel du callback de progression. La pile
         * est equilibree ici (Xb/Yb/cur poppes a chaque mini-lot). Args :
         * numÉpoque 1-based, totalÉpoques, perte moyenne de l'epoque. Si le
         * callback leve, l'exception se propage : vm_call_reentrant purge l'etat
         * VM, on libere l'etat Adam C-heap et on rend la main (le dispatch
         * detecte l'unwind via frameSentinel et propage). */
        if (hasCallback) {
            double avgLoss = (epochBatches > 0)
                           ? epochLossSum / (double)epochBatches : 0.0;
            vm_push(cb);
            vm_push(INTEGER_VAL(e + 1));
            vm_push(INTEGER_VAL(epochs));
            vm_push(REAL_VAL(avgLoss));
            if (vm_call_reentrant(cb, 3) != INTERPRET_OK) goto cleanup;
            vm_pop();  /* resultat du callback (ignore) */
        }

        /*  — early-stopping : après avoir joué (et compté via le
         * callback) l'époque courante, si la validation n'a plus progressé depuis
         * `patience` époques consécutives, on arrête la boucle. Combiné au
         * save-best -> le modèle final porte déjà les poids de la meilleure
         * époque (restaurés ci-dessous). */
        if (patience > 0 && doValEval && epochsSinceImprove >= patience) break;
    }

    /*  — restauration des poids de la meilleure époque validation. Placé
     * APRÈS la boucle (atteint aussi bien par un arrêt anticipé que par la fin
     * normale) mais court-circuité par `goto cleanup` sur erreur d'entrainement. */
    if (keepBest && haveBest) ia_restore_params(m, bestA, bestB, bestRM, bestRV);

cleanup:
    if (useAdam) {
        for (int li = 0; li < m->count; li++) {
            free(mW[li]); free(vW[li]); free(mB[li]); free(vB[li]);
        }
        free(mW); free(vW); free(mB); free(vB);
    }
    /*  — libération des buffers de save-best (NULL si inactif). Les
     * slots des couches non entrainables sont restés NULL : free(NULL) est un
     * no-op sûr. */
    if (bestA != NULL) {
        for (int li = 0; li < m->count; li++) {
            free(bestA[li]); free(bestB[li]);
            free(bestRM[li]); free(bestRV[li]);   /*  — NULL hors BN, free sûr */
        }
        free(bestA); free(bestB);
        free(bestRM); free(bestRV);
    }
    return NIL_VAL;
}

/* ================================================================
 * Persistance (.Sauve / .Charge) — format JSON natif OWL
 *
 * Le modele est serialise en JSON `{"couches":[...]}`. Chaque couche dense
 * ecrit son type, sa matrice de poids (in×out) et son vecteur de biais (out) ;
 * chaque activation n'ecrit que son type. Les reels sortent en PLEINE
 * PRECISION via owl_real_to_str (plus court decimal qui round-trip bit-a-bit,
 * exactement la primitive du serialiseur JSON natif) — un modele sauve puis
 * recharge redonne des poids/biais bit-identiques, donc un .Infère identique.
 * ================================================================ */

/* Petit tampon C-heap a croissance geometrique pour composer le JSON. Aucune
 * allocation OWL ici (pas de GC en jeu cote .Sauve). */
typedef struct { char* buf; size_t len; size_t cap; } IABuf;

static void iabuf_reserve(IABuf* b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t want = b->cap ? b->cap : 256;
        while (want < b->len + extra + 1) want *= 2;
        b->buf = realloc(b->buf, want);
        b->cap = want;
    }
}
static void iabuf_puts(IABuf* b, const char* s) {
    size_t n = strlen(s);
    iabuf_reserve(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}
static void iabuf_putc(IABuf* b, char c) {
    iabuf_reserve(b, 1);
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}
static void iabuf_putd(IABuf* b, double v) {
    char tmp[40];
    owl_real_to_str(v, tmp, sizeof(tmp));   /* plus court decimal round-trip */
    iabuf_puts(b, tmp);
}
static void iabuf_puti(IABuf* b, int v) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%d", v);
    iabuf_puts(b, tmp);
}

/* Ecrit un tableau JSON plat des `n` premiers doubles de `cells` (poids/biais/
 * gamma/beta serialises a plat, la forme etant portee separement). */
static void iabuf_put_flat(IABuf* b, const double* cells, int n) {
    iabuf_putc(b, '[');
    for (int i = 0; i < n; i++) {
        if (i > 0) iabuf_putc(b, ',');
        iabuf_putd(b, cells[i]);
    }
    iabuf_putc(b, ']');
}

/* Ecrit la forme N-D d'un tenseur comme tableau JSON d'entiers. */
static void iabuf_put_shape(IABuf* b, const ObjTensor* t) {
    iabuf_putc(b, '[');
    for (int i = 0; i < t->ndim; i++) {
        if (i > 0) iabuf_putc(b, ',');
        iabuf_puti(b, t->shape[i]);
    }
    iabuf_putc(b, ']');
}

static const char* ia_kind_tag(IALayerKind kind) {
    switch (kind) {
        case IA_LAYER_DENSE:     return "dense";
        case IA_LAYER_RELU:      return "relu";
        case IA_LAYER_SIGMOID:   return "sigmoide";
        case IA_LAYER_SILU:      return "silu";
        case IA_LAYER_CONV2D:    return "conv2d";
        case IA_LAYER_MAXPOOL:   return "maxpool";
        case IA_LAYER_AVGPOOL:   return "avgpool";
        case IA_LAYER_BATCHNORM: return "batchnorm";
        case IA_LAYER_FLATTEN:   return "flatten";
        case IA_LAYER_BRANCH_BEGIN: return "branche_debut";
        case IA_LAYER_BRANCH_NEXT:  return "branche_suivante";
        case IA_LAYER_MERGE_ADD:    return "fusion_addition";
        case IA_LAYER_MERGE_CONCAT: return "fusion_concatenation";
        default:                 return "tanh";
    }
}

/* .Sauve(chemin) / .Save + IAModèleSauve(mon, chemin) / AIModelSave. Serialise
 * le modele en JSON et l'ecrit dans `chemin`. Rend Vrai si l'ecriture reussit,
 * Faux (avec ErreurDétectée) si le chemin est invalide ou l'ecriture echoue. */
static Value fn_iamodele_sauve(int argCount, Value* args) {
    (void)argCount;
    if (!IS_IAMODELE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IAMod\xc3\xa8leSauve",
                              "arg", "modele",
                              "expected", "IAMod\xc3\xa8le",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IAMod\xc3\xa8leSauve",
                              "arg", "chemin",
                              "expected", "chaine",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    ObjIAModele* m = AS_IAMODELE(args[0]);
    const char*  path = AS_CSTRING(args[1]);

    IABuf b = { NULL, 0, 0 };
    iabuf_puts(&b, "{\"couches\":[");
    for (int li = 0; li < m->count; li++) {
        IALayer* L = &m->layers[li];
        if (li > 0) iabuf_putc(&b, ',');
        iabuf_puts(&b, "{\"type\":\"");
        iabuf_puts(&b, ia_kind_tag(L->kind));
        iabuf_putc(&b, '"');
        switch (L->kind) {
            case IA_LAYER_DENSE: {
                ObjMatrix* wm = (ObjMatrix*)L->weights->data;
                iabuf_puts(&b, ",\"poids\":[");
                for (int r = 0; r < wm->rows; r++) {
                    if (r > 0) iabuf_putc(&b, ',');
                    iabuf_putc(&b, '[');
                    for (int c = 0; c < wm->cols; c++) {
                        if (c > 0) iabuf_putc(&b, ',');
                        iabuf_putd(&b, wm->data[r * wm->cols + c]);
                    }
                    iabuf_putc(&b, ']');
                }
                iabuf_puts(&b, "],\"biais\":[");
                double* bd = tensor_cells(L->bias->data);
                int nb = tensor_numel(L->bias->data);
                for (int i = 0; i < nb; i++) {
                    if (i > 0) iabuf_putc(&b, ',');
                    iabuf_putd(&b, bd[i]);
                }
                iabuf_putc(&b, ']');
                break;
            }
            case IA_LAYER_CONV2D: {
                /* Noyau Cout×Cin×kH×kW (forme + valeurs a plat) + biais 1×Cout×1×1
                 * (a plat, forme reconstruite au chargement) + pas/remplissage. */
                iabuf_puts(&b, ",\"forme\":");
                iabuf_put_shape(&b, L->weights);
                iabuf_puts(&b, ",\"poids\":");
                iabuf_put_flat(&b, tensor_cells(L->weights->data),
                               tensor_numel(L->weights->data));
                iabuf_puts(&b, ",\"biais\":");
                iabuf_put_flat(&b, tensor_cells(L->bias->data),
                               tensor_numel(L->bias->data));
                iabuf_puts(&b, ",\"pas\":");
                iabuf_puti(&b, L->stride);
                iabuf_puts(&b, ",\"remplissage\":");
                iabuf_puti(&b, L->pad);
                iabuf_puts(&b, ",\"groupe\":");
                iabuf_puti(&b, L->window > 0 ? L->window : 1);   /*  */
                break;
            }
            case IA_LAYER_MAXPOOL:
            case IA_LAYER_AVGPOOL: {
                iabuf_puts(&b, ",\"fenetre\":");
                iabuf_puti(&b, L->window);
                iabuf_puts(&b, ",\"pas\":");
                iabuf_puti(&b, L->stride);
                iabuf_puts(&b, ",\"remplissage\":");
                iabuf_puti(&b, L->pad);
                break;
            }
            case IA_LAYER_BATCHNORM: {
                iabuf_puts(&b, ",\"gamma\":");
                iabuf_put_flat(&b, tensor_cells(L->gamma->data),
                               tensor_numel(L->gamma->data));
                iabuf_puts(&b, ",\"beta\":");
                iabuf_put_flat(&b, tensor_cells(L->beta->data),
                               tensor_numel(L->beta->data));
                /*  — stats mobiles : indispensables a l'inference (mode
                 * eval), sinon un modele recharge normaliserait avec des stats
                 * par defaut (moyenne 0, variance 1) et perdrait sa justesse. */
                iabuf_puts(&b, ",\"moyenne_mobile\":");
                iabuf_put_flat(&b, tensor_cells(L->running_mean->data),
                               tensor_numel(L->running_mean->data));
                iabuf_puts(&b, ",\"variance_mobile\":");
                iabuf_put_flat(&b, tensor_cells(L->running_var->data),
                               tensor_numel(L->running_var->data));
                iabuf_puts(&b, ",\"eps\":");
                iabuf_putd(&b, L->eps);
                break;
            }
            default:
                /* relu/sigmoide/tanh/flatten : le type seul suffit. */
                break;
        }
        /*  — etat gele de la couche (defaut faux, omis a la relecture
         * d'un modele anterieur -> entrainable). */
        iabuf_puts(&b, ",\"gele\":");
        iabuf_puts(&b, L->frozen ? "true" : "false");
        iabuf_putc(&b, '}');
    }
    iabuf_puts(&b, "]}");

    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        free(b.buf);
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "IAMod\xc3\xa8leSauve",
                              "native", "IAMod\xc3\xa8leSauve",
                              "path", path, "detail", strerror(errno), NULL);
        return BOOL_VAL(false);
    }
    size_t written = fwrite(b.buf, 1, b.len, f);
    fclose(f);
    bool ok = (written == b.len);
    free(b.buf);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_IO_FAILURE, "IAMod\xc3\xa8leSauve",
                              "native", "IAMod\xc3\xa8leSauve",
                              "path", path, "detail", "ecriture partielle", NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Cherche un champ par cle dans un objet JSON sans allouer d'ObjString (scan
 * direct, cles courtes). Rend NULL si absent ou si `obj` n'est pas un objet. */
static ObjJson* ia_json_field(const ObjJson* obj, const char* key) {
    if (obj == NULL || obj->jtype != JSON_T_OBJECT) return NULL;
    for (int i = 0; i < obj->as.objectVal.count; i++) {
        ObjString* k = obj->as.objectVal.keys[i];
        if (k != NULL && strcmp(k->chars, key) == 0) {
            return obj->as.objectVal.values[i];
        }
    }
    return NULL;
}

/* Lit un noeud JSON numerique en double, qu'il soit entier (biais 0 ecrit "0")
 * ou reel. Tout autre type -> 0. */
static double ia_json_number(const ObjJson* j) {
    if (j == NULL) return 0.0;
    switch (j->jtype) {
        case JSON_T_INT:  return (double)j->as.intVal;
        case JSON_T_REAL: return j->as.realVal;
        default:          return 0.0;
    }
}

/* Emet une erreur recuperable de chargement et rend Faux. Facteur commun des
 * chemins d'echec de .Charge (fichier absent, vide, JSON illisible, structure
 * inattendue) — tous laissent ErreurDétectée levee sans crasher. */
static Value ia_charge_echec(const char* path, const char* detail) {
    owl_emit_native_error(E_NATIVE_IO_FAILURE, "IAMod\xc3\xa8leCharge",
                          "native", "IAMod\xc3\xa8leCharge",
                          "path", path, "detail", detail, NULL);
    return BOOL_VAL(false);
}

/* .Charge(chemin) / .Load + IAModèleCharge(mon, chemin) / AIModelLoad. Lit le
 * fichier JSON, parse, et RECONSTRUIT les couches de `mon` AVEC les poids lus
 * (chemin interne : cree directement les Tenseurs poids/biais, ne repasse pas
 * par AjouteCoucheDense qui re-initialiserait au hasard). Le modele appelant
 * est remis a vide avant reconstruction. Rend Vrai en cas de succes, Faux (avec
 * ErreurDétectée) sinon — jamais de crash. */
static Value fn_iamodele_charge(int argCount, Value* args) {
    (void)argCount;
    if (!IS_IAMODELE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IAMod\xc3\xa8leCharge",
                              "arg", "modele",
                              "expected", "IAMod\xc3\xa8le",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IAMod\xc3\xa8leCharge",
                              "arg", "chemin",
                              "expected", "chaine",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    ObjIAModele* m = AS_IAMODELE(args[0]);
    const char*  path = AS_CSTRING(args[1]);

    FILE* f = fopen(path, "rb");
    if (f == NULL) return ia_charge_echec(path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > INT_MAX - 1) {
        fclose(f);
        return ia_charge_echec(path, "fichier vide ou trop grand");
    }
    char* src = malloc((size_t)size + 1);
    size_t rd = fread(src, 1, (size_t)size, f);
    src[rd] = '\0';
    fclose(f);

    ObjJson* root = json_parse_cstr(src, (int)rd);
    free(src);
    if (root == NULL) return ia_charge_echec(path, "JSON illisible");

    /* root rootee sur la pile VM pendant toute la reconstruction : chaque
     * obj_new_tensor* peut declencher un GC, root et ses tableaux (poids/biais)
     * doivent survivre. Le modele (args[0]) reste roote par le dispatch. */
    vm_push(OBJ_VAL(root));

    ObjJson* couches = ia_json_field(root, "couches");
    if (couches == NULL || couches->jtype != JSON_T_ARRAY) {
        vm_pop();
        return ia_charge_echec(path, "structure inattendue (couches absentes)");
    }

    /* Reconstruction depuis zero : le modele appelant repart vide. Les anciens
     * Tenseurs (s'il y en avait) ne sont plus references et seront collectes. */
    m->count = 0;
    m->branchDepth = 0;   /*  — recompte au fil des marqueurs charges */

    for (int i = 0; i < couches->as.arrayVal.count; i++) {
        ObjJson* layer   = couches->as.arrayVal.items[i];
        ObjJson* typeNode = ia_json_field(layer, "type");
        if (typeNode == NULL || typeNode->jtype != JSON_T_STRING) {
            vm_pop();
            return ia_charge_echec(path, "couche sans type");
        }
        const char* t = typeNode->as.strVal->chars;

        if (strcmp(t, "dense") == 0) {
            ObjJson* poids = ia_json_field(layer, "poids");
            ObjJson* biais = ia_json_field(layer, "biais");
            if (poids == NULL || poids->jtype != JSON_T_ARRAY
                || poids->as.arrayVal.count == 0) {
                vm_pop();
                return ia_charge_echec(path, "couche dense sans poids");
            }
            ObjJson* row0 = poids->as.arrayVal.items[0];
            if (row0 == NULL || row0->jtype != JSON_T_ARRAY) {
                vm_pop();
                return ia_charge_echec(path, "poids mal formes");
            }
            int rows = poids->as.arrayVal.count;
            int cols = row0->as.arrayVal.count;

            /* Toutes les lignes de poids doivent avoir la meme longueur. Un
             * JSON aux lignes inegales est rejete (ni zero-remplissage ni
             * troncature silencieuse) — meme traitement que les autres cas
             * malformes. Verifie AVANT toute allocation : seul `root` est
             * empile ici, un seul vm_pop suffit. */
            for (int r = 0; r < rows; r++) {
                ObjJson* rowNode = poids->as.arrayVal.items[r];
                if (rowNode == NULL || rowNode->jtype != JSON_T_ARRAY
                    || rowNode->as.arrayVal.count != cols) {
                    vm_pop();
                    return ia_charge_echec(path, "lignes de poids de longueur inegale");
                }
            }

            /* W (rows×cols) rootee pendant l'alloc du biais (le second alloc
             * peut declencher un GC). Ensuite plus aucune allocation OWL. */
            ObjTensor* W = obj_new_tensor_matrix(rows, cols);
            vm_push(OBJ_VAL(W));
            ObjTensor* bT = obj_new_tensor(cols);   /* biais zero-init */

            double* wd = tensor_cells(W->data);
            for (int r = 0; r < rows; r++) {
                ObjJson* rowNode = poids->as.arrayVal.items[r];
                if (rowNode == NULL || rowNode->jtype != JSON_T_ARRAY) continue;
                int rc = rowNode->as.arrayVal.count;
                for (int c = 0; c < cols && c < rc; c++) {
                    wd[r * cols + c] = ia_json_number(rowNode->as.arrayVal.items[c]);
                }
            }
            if (biais != NULL && biais->jtype == JSON_T_ARRAY) {
                double* bd = tensor_cells(bT->data);
                int bc = biais->as.arrayVal.count;
                for (int k = 0; k < cols && k < bc; k++) {
                    bd[k] = ia_json_number(biais->as.arrayVal.items[k]);
                }
            }
            obj_iamodele_push_layer(m, IA_LAYER_DENSE, W, bT);
            vm_pop();   /* W — desormais roote via le modele */
        } else if (strcmp(t, "conv2d") == 0) {
            ObjJson* forme = ia_json_field(layer, "forme");
            ObjJson* poids = ia_json_field(layer, "poids");
            ObjJson* biais = ia_json_field(layer, "biais");
            ObjJson* pasN  = ia_json_field(layer, "pas");
            ObjJson* padN  = ia_json_field(layer, "remplissage");
            ObjJson* grpN  = ia_json_field(layer, "groupe");   /*  */
            if (forme == NULL || forme->jtype != JSON_T_ARRAY
                || forme->as.arrayVal.count != 4
                || poids == NULL || poids->jtype != JSON_T_ARRAY) {
                vm_pop();
                return ia_charge_echec(path, "couche conv2d mal formee");
            }
            int wshape[4];
            int wnel = 1;
            for (int a = 0; a < 4; a++) {
                wshape[a] = (int)ia_json_number(forme->as.arrayVal.items[a]);
                wnel *= wshape[a];
            }
            if (wnel <= 0 || poids->as.arrayVal.count != wnel) {
                vm_pop();
                return ia_charge_echec(path, "poids conv incoherents avec la forme");
            }
            int cout = wshape[0];

            /* Noyau (Cout×Cin×kH×kW) rootee pendant l'alloc du biais (second
             * alloc pouvant declencher un GC). Ensuite plus aucune alloc OWL. */
            ObjTensor* W = obj_new_tensor_nd(4, wshape);
            vm_push(OBJ_VAL(W));
            int bshape[4] = { 1, cout, 1, 1 };
            ObjTensor* bT = obj_new_tensor_nd(4, bshape);   /* biais zero-init */

            double* wd = tensor_cells(W->data);
            for (int e = 0; e < wnel; e++) {
                wd[e] = ia_json_number(poids->as.arrayVal.items[e]);
            }
            if (biais != NULL && biais->jtype == JSON_T_ARRAY) {
                double* bd = tensor_cells(bT->data);
                int bc = biais->as.arrayVal.count;
                for (int e = 0; e < cout && e < bc; e++) {
                    bd[e] = ia_json_number(biais->as.arrayVal.items[e]);
                }
            }
            obj_iamodele_push_layer(m, IA_LAYER_CONV2D, W, bT);
            m->layers[m->count - 1].stride = (pasN != NULL) ? (int)ia_json_number(pasN) : 1;
            m->layers[m->count - 1].pad    = (padN != NULL) ? (int)ia_json_number(padN) : 0;
            /* Retrocompat : un modele anterieur a  n'a pas de groupe -> 1. */
            m->layers[m->count - 1].window = (grpN != NULL) ? (int)ia_json_number(grpN) : 1;
            vm_pop();   /* W — desormais roote via le modele */
        } else if (strcmp(t, "maxpool") == 0 || strcmp(t, "avgpool") == 0) {
            ObjJson* fen = ia_json_field(layer, "fenetre");
            ObjJson* pasN = ia_json_field(layer, "pas");
            ObjJson* padN = ia_json_field(layer, "remplissage");
            IALayerKind kind = (strcmp(t, "maxpool") == 0)
                             ? IA_LAYER_MAXPOOL : IA_LAYER_AVGPOOL;
            int fenV = (fen != NULL) ? (int)ia_json_number(fen) : 1;
            obj_iamodele_push_layer(m, kind, NULL, NULL);
            m->layers[m->count - 1].window = fenV;
            /* Retrocompat : un modele anterieur a /1802 n'a ni pas ni
             * remplissage -> pas = fenetre (non chevauchant), remplissage = 0. */
            m->layers[m->count - 1].stride = (pasN != NULL) ? (int)ia_json_number(pasN) : fenV;
            m->layers[m->count - 1].pad    = (padN != NULL) ? (int)ia_json_number(padN) : 0;
        } else if (strcmp(t, "batchnorm") == 0) {
            ObjJson* gN   = ia_json_field(layer, "gamma");
            ObjJson* beN  = ia_json_field(layer, "beta");
            ObjJson* epsN = ia_json_field(layer, "eps");
            /*  — stats mobiles : optionnelles (un modele anterieur ne les
             * contient pas -> defaut moyenne 0 / variance 1, comme a l'init). */
            ObjJson* mmN  = ia_json_field(layer, "moyenne_mobile");
            ObjJson* vmN  = ia_json_field(layer, "variance_mobile");
            if (gN == NULL || gN->jtype != JSON_T_ARRAY
                || beN == NULL || beN->jtype != JSON_T_ARRAY) {
                vm_pop();
                return ia_charge_echec(path, "couche batchnorm mal formee");
            }
            int C = gN->as.arrayVal.count;
            /* Chaque tenseur alloue reste roote sur la pile pendant l'alloc
             * suivante (chacune peut GC) ; obj_iamodele_push_layer ne fait qu'un
             * realloc C-heap (aucune alloc OWL). */
            ObjTensor* g = obj_new_tensor(C);
            vm_push(OBJ_VAL(g));
            ObjTensor* be = obj_new_tensor(beN->as.arrayVal.count);
            vm_push(OBJ_VAL(be));
            ObjTensor* rm = obj_new_tensor(C);   /* moyenne mobile zero-init */
            vm_push(OBJ_VAL(rm));
            ObjTensor* rv = obj_new_tensor(C);   /* variance mobile */
            double* gd = tensor_cells(g->data);
            for (int e = 0; e < C; e++) gd[e] = ia_json_number(gN->as.arrayVal.items[e]);
            double* bed = tensor_cells(be->data);
            int bec = beN->as.arrayVal.count;
            for (int e = 0; e < bec; e++) bed[e] = ia_json_number(beN->as.arrayVal.items[e]);
            double* rmd = tensor_cells(rm->data);
            double* rvd = tensor_cells(rv->data);
            for (int e = 0; e < C; e++) rvd[e] = 1.0;   /* defaut si absent */
            if (mmN != NULL && mmN->jtype == JSON_T_ARRAY) {
                for (int e = 0; e < C && e < mmN->as.arrayVal.count; e++)
                    rmd[e] = ia_json_number(mmN->as.arrayVal.items[e]);
            }
            if (vmN != NULL && vmN->jtype == JSON_T_ARRAY) {
                for (int e = 0; e < C && e < vmN->as.arrayVal.count; e++)
                    rvd[e] = ia_json_number(vmN->as.arrayVal.items[e]);
            }
            obj_iamodele_push_layer(m, IA_LAYER_BATCHNORM, NULL, NULL);
            m->layers[m->count - 1].gamma        = g;
            m->layers[m->count - 1].beta         = be;
            m->layers[m->count - 1].running_mean = rm;
            m->layers[m->count - 1].running_var  = rv;
            m->layers[m->count - 1].eps          = (epsN != NULL) ? ia_json_number(epsN) : 1e-5;
            vm_pop();   /* rm */
            vm_pop();   /* be */
            vm_pop();   /* g — desormais rootes via le modele */
        } else if (strcmp(t, "flatten") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_FLATTEN, NULL, NULL);
        } else if (strcmp(t, "relu") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_RELU, NULL, NULL);
        } else if (strcmp(t, "sigmoide") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_SIGMOID, NULL, NULL);
        } else if (strcmp(t, "tanh") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_TANH, NULL, NULL);
        } else if (strcmp(t, "silu") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_SILU, NULL, NULL);
        } else if (strcmp(t, "branche_debut") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_BRANCH_BEGIN, NULL, NULL);
            m->branchDepth++;
        } else if (strcmp(t, "branche_suivante") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_BRANCH_NEXT, NULL, NULL);
        } else if (strcmp(t, "fusion_addition") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_MERGE_ADD, NULL, NULL);
            if (m->branchDepth > 0) m->branchDepth--;
        } else if (strcmp(t, "fusion_concatenation") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_MERGE_CONCAT, NULL, NULL);
            if (m->branchDepth > 0) m->branchDepth--;
        } else {
            vm_pop();
            return ia_charge_echec(path, "type de couche inconnu");
        }
        /*  — restaure l'etat gele de la couche qui vient d'etre poussee.
         * Absent (modele anterieur a la feature) -> faux (entrainable), retrocompat. */
        ObjJson* geleN = ia_json_field(layer, "gele");
        m->layers[m->count - 1].frozen =
            (geleN != NULL && geleN->jtype == JSON_T_BOOL && geleN->as.boolVal);
    }

    vm_pop();   /* root */
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * Import ONNX (.ChargeONNX / .LoadONNX) — , lot 1
 *
 * Le lecteur protobuf (onnx_import.c) produit un OnnxModel neutre (C-heap,
 * VM-free). Ici on TRADUIT son graphe en couches IAModèle : chaque node ONNX
 * pris DANS L'ORDRE (graphe sequentiel de classif) devient une couche OWL, ses
 * poids charges depuis les initializers references par ses inputs. Les Tenseurs
 * OWL sont alloues et rootes ICI (concern GC isole cote stdlib) — le modele
 * `m` reste roote par le dispatch pendant toute la reconstruction.
 * ================================================================ */

/* Copie les `count` initializers reference par un input dans un tenseur OWL
 * deja alloue (tronque/limite au min des deux tailles). */
static void ia_onnx_fill(ObjTensor* dst, const OnnxTensor* src, int64_t limit) {
    double* d = tensor_cells(dst->data);
    int64_t n = (src->count < limit) ? src->count : limit;
    for (int64_t k = 0; k < n; k++) d[k] = src->data[k];
}

/*  — materialise l'attribut `value` d'un nœud Constant (un OnnxTensor,
 * ex. les facteurs Resize [1,1,2,2]) en Tenseur OWL, forme conservee. Point
 * d'alloc unique (obj_new_tensor_nd) puis copie de doubles : le resultat est sain
 * sans rooting, le caller l'affecte aussitot a un gnode (trace GC). Rend NULL si
 * la valeur est vide, incoherente, ou de rang hors bornes. */
static ObjTensor* ia_onnx_const_to_tensor(const OnnxTensor* t) {
    if (t == NULL || t->count <= 0 || t->data == NULL) return NULL;
    int ndim = t->ndim;
    int shape[OWL_TENSOR_MAX_RANK];
    if (ndim < 1) {                         /* scalaire ONNX -> Tenseur 1D [1] */
        ndim = 1; shape[0] = 1;
    } else {
        if (ndim > OWL_TENSOR_MAX_RANK) return NULL;
        int64_t nel = 1;
        for (int a = 0; a < ndim; a++) { shape[a] = (int)t->dims[a]; nel *= t->dims[a]; }
        if (nel != t->count) return NULL;
    }
    ObjTensor* ct = obj_new_tensor_nd(ndim, shape);
    ia_onnx_fill(ct, t, t->count);
    return ct;
}

/* Index du node qui PRODUIT le tenseur `name` (-1 si aucun — cas d'une entree
 * du graphe ou d'un initializer). Sert a construire la carte producteur pour la
 * detection du motif residuel non sequentiel. */
static int ia_onnx_producer(const OnnxGraph* g, const char* name) {
    if (name == NULL) return -1;
    for (int i = 0; i < g->nnodes; i++) {
        const OnnxNode* n = &g->nodes[i];
        for (int o = 0; o < n->noutputs; o++)
            if (n->outputs[o] != NULL && strcmp(n->outputs[o], name) == 0) return i;
    }
    return -1;
}

/* Attributs communs a MaxPool / AveragePool : lit kernel_shape (fenetre carree)
 * et strides (pas carre, defaut = fenetre ;  : un pas != fenetre est
 * supporte -> fenetres chevauchantes).  : lit pads (remplissage carre
 * et symetrique ; un remplissage asymetrique ou non carre reste hors perimetre).
 * Rend NULL + *window_out + *stride_out + *pad_out en succes, sinon un message
 * (formate dans buf, nommant `nm`). */
static const char* ia_onnx_pool_window(const OnnxNode* n, const char* nm,
                                       int* window_out, int* stride_out,
                                       int* pad_out,
                                       char* buf, size_t buflen) {
    const OnnxAttr* ksA = onnx_node_attr(n, "kernel_shape");
    if (ksA == NULL || ksA->nints < 1) {
        snprintf(buf, buflen, "%s sans kernel_shape", nm);
        return buf;
    }
    for (int a = 1; a < ksA->nints; a++) {
        if (ksA->ints[a] != ksA->ints[0]) {
            snprintf(buf, buflen, "fenetre %s non carree non supportee", nm);
            return buf;
        }
    }
    int window = (int)ksA->ints[0];
    if (window <= 0) {
        snprintf(buf, buflen, "fenetre %s invalide", nm);
        return buf;
    }
    int stride = window;   /* defaut ONNX : pas = fenetre */
    const OnnxAttr* strA = onnx_node_attr(n, "strides");
    if (strA != NULL && strA->nints >= 1) {
        for (int a = 1; a < strA->nints; a++) {
            if (strA->ints[a] != strA->ints[0]) {
                snprintf(buf, buflen, "pas %s non carre non supporte", nm);
                return buf;
            }
        }
        stride = (int)strA->ints[0];
        if (stride <= 0) {
            snprintf(buf, buflen, "pas %s invalide", nm);
            return buf;
        }
    }
    /* ONNX pads = [debut_h, debut_w, fin_h, fin_w]. On accepte un remplissage
     * carre ET symetrique (toutes les valeurs egales) ; sinon hors perimetre. */
    int pad = 0;
    const OnnxAttr* padsA = onnx_node_attr(n, "pads");
    if (padsA != NULL && padsA->nints > 0) {
        for (int a = 1; a < padsA->nints; a++) {
            if (padsA->ints[a] != padsA->ints[0]) {
                snprintf(buf, buflen, "remplissage %s asymetrique non supporte", nm);
                return buf;
            }
        }
        pad = (int)padsA->ints[0];
        if (pad < 0 || pad >= window) {
            snprintf(buf, buflen, "remplissage %s hors bornes", nm);
            return buf;
        }
    }
    *window_out = window;
    *stride_out = stride;
    *pad_out = pad;
    return NULL;
}

/* Bloc a branches paralleles detecte autour d'un node de fusion (Add ou Concat) :
 * N sous-chaines divergeant d'un tenseur commun x (le « fork ») et convergeant
 * sur le node de fusion.
 *  - `starts[k]` = premier node de la branche k (ordre de declaration = ordre des
 *    entrees du node de fusion) — BRANCH_BEGIN emis avant starts[0] (capture x),
 *    BRANCH_NEXT avant chaque starts[k>=1] ; -1 = branche VIDE (residuel identite
 *    Add : l'entree du saut EST x, BRANCH_NEXT emis au node de fusion) ;
 *  - `nbranch` = nombre de branches ;
 *  - `merge`   = index du node de fusion (-> MERGE_ADD ou MERGE_CONCAT) ;
 *  - `kind`    = mode de fusion.
 * Add couvre le residuel identite (une branche F(x), l'autre = x brut) ET le
 * residuel a projection (deux branches non triviales). Concat
 * generalise a N branches non vides fusionnees sur l'axe canal (module « feu »
 * SqueezeNet, blocs Inception). */
#define IA_ONNX_MAX_RESIDUALS 256
#define IA_ONNX_MAX_CHAIN 8192
typedef struct {
    int         starts[IA_MAX_BRANCHES];
    int         nbranch;
    int         merge;
    IALayerKind kind;
} IaOnnxBranchBlock;

/* Tenseur de divergence (fork) commun aux entrees in0 et in1 : chaine d'ancetres
 * de in0 (suivant inputs[0]) puis remontee de in1 jusqu'au 1er tenseur commun.
 * Rend NULL si aucune divergence commune. */
static const char* ia_onnx_find_fork(const OnnxGraph* g, const char* in0,
                                     const char* in1) {
    const char* anc[IA_ONNX_MAX_CHAIN];
    int na = 0;
    anc[na++] = in0;
    int p = ia_onnx_producer(g, in0);
    while (p >= 0 && na < IA_ONNX_MAX_CHAIN) {
        const char* ain = (g->nodes[p].ninputs > 0) ? g->nodes[p].inputs[0] : NULL;
        if (ain == NULL) break;
        anc[na++] = ain;
        p = ia_onnx_producer(g, ain);
    }
    const char* u = in1;
    int guard = 0;
    while (u != NULL && guard++ < IA_ONNX_MAX_CHAIN) {
        for (int k = 0; k < na; k++)
            if (strcmp(u, anc[k]) == 0) return u;
        int pu = ia_onnx_producer(g, u);
        if (pu < 0) break;
        u = (g->nodes[pu].ninputs > 0) ? g->nodes[pu].inputs[0] : NULL;
    }
    return NULL;
}

/*  — motif SiLU (Swish) : Mul(t, Sigmoid(t)), ou les DEUX entrees du Mul
 * remontent au MEME tenseur producteur t, l'une directement, l'autre via un node
 * Sigmoid. Vrai si le node `mul` (Mul a 2 entrees) forme un SiLU : une entree est
 * produite par un Sigmoid dont l'entree est exactement l'autre operande. Un Mul
 * dont les deux entrees ne convergent pas ainsi n'est PAS un SiLU (Mul general,
 * hors perimetre). */
static bool ia_onnx_mul_is_silu(const OnnxGraph* g, const OnnxNode* mul) {
    if (mul->op_type == NULL || strcmp(mul->op_type, "Mul") != 0 || mul->ninputs != 2)
        return false;
    const char* a = mul->inputs[0];
    const char* b = mul->inputs[1];
    if (a == NULL || b == NULL) return false;
    for (int swap = 0; swap < 2; swap++) {
        const char* sigOut = (swap == 0) ? a : b;   /* entree candidate = sortie Sigmoid */
        const char* other  = (swap == 0) ? b : a;   /* l'autre entree = le t direct */
        int p = ia_onnx_producer(g, sigOut);
        if (p < 0) continue;
        const OnnxNode* sn = &g->nodes[p];
        if (sn->op_type == NULL || strcmp(sn->op_type, "Sigmoid") != 0) continue;
        if (sn->ninputs < 1 || sn->inputs[0] == NULL) continue;
        if (strcmp(sn->inputs[0], other) == 0) return true;   /* Sigmoid(other) . other */
    }
    return false;
}

/*  — vrai si le node `sig` (Sigmoid) est consomme par un Mul formant un
 * SiLU : sa sortie ET son entree alimentent un meme Mul. Dans ce cas le Sigmoid
 * ne doit PAS emettre de couche a l'import — la couche SiLU est emise au Mul, et
 * le tenseur courant reste t (le Sigmoid saute, comme un biais replie). */
static bool ia_onnx_sigmoid_is_silu(const OnnxGraph* g, const OnnxNode* sig) {
    if (sig->noutputs < 1 || sig->ninputs < 1) return false;
    const char* sout = sig->outputs[0];
    const char* sin  = sig->inputs[0];
    if (sout == NULL || sin == NULL) return false;
    for (int j = 0; j < g->nnodes; j++) {
        const OnnxNode* mn = &g->nodes[j];
        if (mn->op_type == NULL || strcmp(mn->op_type, "Mul") != 0 || mn->ninputs != 2)
            continue;
        const char* x = mn->inputs[0];
        const char* y = mn->inputs[1];
        if (x == NULL || y == NULL) continue;
        if ((strcmp(x, sout) == 0 && strcmp(y, sin) == 0) ||
            (strcmp(y, sout) == 0 && strcmp(x, sin) == 0))
            return true;
    }
    return false;
}

/* Premier node de la sous-chaine (suivant inputs[0]) produisant `t` et forkant a
 * `x`. Rend -2 si la branche est VIDE (t == x), sinon l'index du node dont
 * l'entree d'activation est x (ou dont l'entree n'a pas de producteur = racine du
 * graphe). Rend -1 si x n'est pas atteint (chaine non lineaire hors perimetre). */
static int ia_onnx_branch_first(const OnnxGraph* g, const char* t, const char* x) {
    if (t == NULL || x == NULL) return -1;
    if (strcmp(t, x) == 0) return -2;   /* branche vide (identite) */
    int p = ia_onnx_producer(g, t);
    while (p >= 0) {
        const char* ain = g->nodes[p].inputs != NULL && g->nodes[p].ninputs > 0
                        ? g->nodes[p].inputs[0] : NULL;
        if (ain == NULL) return p;              /* entree = racine du graphe */
        if (strcmp(ain, x) == 0) return p;      /* ce node consomme le fork */
        int pp = ia_onnx_producer(g, ain);
        if (pp < 0) return p;                   /* ain = entree/initializer = racine */
        p = pp;
    }
    return -1;
}

/* Pre-passe : repere les blocs a branches (graphe non sequentiel).
 *  - Add a 2 entrees : residuel. On reconstruit la chaine d'ancetres (suivant
 *    inputs[0]) de la 1re entree, remonte la 2e jusqu'au fork x. La branche
 *    apparaissant en premier dans le graphe devient la branche 1 (l'addition
 *    binaire etant exacte a l'ordre pres, ce choix n'affecte pas la valeur et
 *    preserve le round-trip du residuel identite ou F precede le saut).
 *  - Concat a N entrees, axe canal (axis=1) : fusion par concatenation
 *   . Chaque entree est une branche divergeant du meme fork x ;
 *    l'ordre des CANAUX dans la sortie suit l'ordre des entrees du Concat, donc
 *    on n'accepte le motif que si le node de debut de chaque branche croit avec
 *    l'ordre des entrees (l'ordre des marqueurs, dicte par l'ordre des nodes,
 *    coincide alors avec l'ordre des entrees). Les cas non canoniques ne sont
 *    pas enregistres : le handler Concat leve une erreur claire.
 * Rend le nombre de blocs, -1 si trop. */
static int ia_onnx_detect_branch_blocks(const OnnxGraph* g, IaOnnxBranchBlock* out) {
    int nres = 0;
    for (int i = 0; i < g->nnodes; i++) {
        const OnnxNode* n = &g->nodes[i];
        if (n->op_type == NULL) continue;

        if (strcmp(n->op_type, "Add") == 0) {
            if (n->ninputs != 2) continue;
            const char* in0 = n->inputs[0];
            const char* in1 = n->inputs[1];
            if (ia_onnx_producer(g, in0) < 0 || ia_onnx_producer(g, in1) < 0)
                continue;   /* une entree = tenseur brut/initializer -> pas ce motif */
            const char* fork = ia_onnx_find_fork(g, in0, in1);
            if (fork == NULL) continue;   /* pas de divergence commune */

            int sA = ia_onnx_branch_first(g, in0, fork);
            int sB = ia_onnx_branch_first(g, in1, fork);
            if (sA == -1 || sB == -1) continue;   /* chaine hors perimetre */
            if (sA == -2 && sB == -2) continue;   /* deux branches vides -> hors perimetre */

            int begin, next;
            if (sA == -2) {          /* branche in0 vide : la non-vide (in1) passe 1re */
                begin = sB; next = -1;
            } else if (sB == -2) {   /* branche in1 vide (residuel identite) */
                begin = sA; next = -1;
            } else {                 /* deux branches non triviales (projection) */
                begin = (sA < sB) ? sA : sB;
                next  = (sA < sB) ? sB : sA;
            }
            if (nres >= IA_ONNX_MAX_RESIDUALS) return -1;
            out[nres].kind      = IA_LAYER_MERGE_ADD;
            out[nres].nbranch   = 2;
            out[nres].starts[0] = begin;
            out[nres].starts[1] = next;
            out[nres].merge     = i;
            nres++;

        } else if (strcmp(n->op_type, "Concat") == 0) {
            const OnnxAttr* axA = onnx_node_attr(n, "axis");
            if (axA == NULL || !axA->has_i || axA->i != 1) continue;   /* axe non canal -> handler erreur */
            if (n->ninputs < 2 || n->ninputs > IA_MAX_BRANCHES) continue;
            /* Toutes les entrees = sorties de nodes (sous-chaines), pas des bruts. */
            bool ok = true;
            for (int k = 0; k < n->ninputs; k++) {
                if (n->inputs[k] == NULL || ia_onnx_producer(g, n->inputs[k]) < 0) { ok = false; break; }
            }
            if (!ok) continue;
            const char* fork = ia_onnx_find_fork(g, n->inputs[0], n->inputs[1]);
            if (fork == NULL) continue;

            int starts[IA_MAX_BRANCHES];
            bool increasing = true;
            for (int k = 0; k < n->ninputs; k++) {
                int s = ia_onnx_branch_first(g, n->inputs[k], fork);
                if (s < 0) { ok = false; break; }   /* -1 hors perimetre ; -2 (branche identite) non geree en concat */
                starts[k] = s;
                if (k > 0 && starts[k] <= starts[k - 1]) increasing = false;
            }
            if (!ok || !increasing) continue;   /* motif non canonique -> handler erreur */

            if (nres >= IA_ONNX_MAX_RESIDUALS) return -1;
            out[nres].kind    = IA_LAYER_MERGE_CONCAT;
            out[nres].nbranch = n->ninputs;
            for (int k = 0; k < n->ninputs; k++) out[nres].starts[k] = starts[k];
            out[nres].merge   = i;
            nres++;
        }
    }
    return nres;
}

/* Detecte le motif « biais emis en node Add separe » : le node i+1 est un
 * Add a 2 entrees dont l'une est la sortie du node `n` (couche precedente) et
 * l'AUTRE un initializer (constante). Rend l'initializer = biais
 * element-par-element de la couche precedente et pose *addIdx = i+1 ; NULL si
 * le motif ne s'applique pas. Distinct du residuel-Add (2 entrees = sorties de
 * nodes, aucun initializer) : ia_onnx_producer rend -1 sur un initializer, donc
 * la pre-passe residuelle ignore deja ce cas. */
static const OnnxTensor* ia_onnx_bias_add_after(const OnnxGraph* g, int i,
                                                const OnnxNode* n, int* addIdx) {
    *addIdx = -1;
    if (i + 1 >= g->nnodes || n->noutputs <= 0 || n->outputs[0] == NULL) return NULL;
    const OnnxNode* nx = &g->nodes[i + 1];
    if (nx->op_type == NULL || strcmp(nx->op_type, "Add") != 0 || nx->ninputs != 2)
        return NULL;
    const char* out = n->outputs[0];
    const char* other = NULL;
    if (nx->inputs[0] != NULL && strcmp(nx->inputs[0], out) == 0)      other = nx->inputs[1];
    else if (nx->inputs[1] != NULL && strcmp(nx->inputs[1], out) == 0) other = nx->inputs[0];
    if (other == NULL) return NULL;
    const OnnxTensor* bt = onnx_find_init(g, other);
    if (bt == NULL) return NULL;   /* l'autre entree est une sortie de node -> residuel, pas un biais */
    *addIdx = i + 1;
    return bt;
}

/* Resout l'operande POIDS d'un MatMul designe par `name` en une matrice 2D
 * [in x out] et rend l'initializer source (donnees row-major inchangees) :
 *  - cas direct : `name` est un initializer de rang 2 -> [dims0 x dims1] ;
 *  - cas indirect : `name` est la sortie d'un node Reshape d'un initializer
 *    (repli de constante par l'exporteur, ex. CNTK aplatit les poids denses
 *    [.,.,.,out] en amont du MatMul). Le shape-tensor cible est un INT64 non
 *    decode par le lecteur, mais un aplatissement row-major garde la derniere
 *    dimension comme colonnes : out = derniere dim source, in = reste.
 * Rend NULL si `name` ne correspond a aucun de ces motifs. */
static const OnnxTensor* ia_onnx_resolve_matmul_weight(const OnnxGraph* g,
                                                       const char* name,
                                                       int* in, int* out) {
    const OnnxTensor* t = onnx_find_init(g, name);
    if (t != NULL) {
        if (t->ndim != 2) return NULL;
        *in = (int)t->dims[0];
        *out = (int)t->dims[1];
        return t;
    }
    int p = ia_onnx_producer(g, name);
    if (p < 0) return NULL;
    const OnnxNode* rn = &g->nodes[p];
    if (rn->op_type == NULL || strcmp(rn->op_type, "Reshape") != 0 || rn->ninputs < 1)
        return NULL;
    const OnnxTensor* src = onnx_find_init(g, rn->inputs[0]);
    if (src == NULL || src->ndim < 2) return NULL;
    int o = (int)src->dims[src->ndim - 1];
    if (o <= 0) return NULL;
    int64_t nin = src->count / o;
    if (nin <= 0 || nin * (int64_t)o != src->count) return NULL;
    *in = (int)nin;
    *out = o;
    return src;
}

/* Vrai si `name` est produit par un node Reshape dont l'entree est un
 * initializer : node de PREPARATION DE POIDS (repli de constante), hors du flux
 * d'activation — a ignorer dans la sequence de couches, il est resolu
 * directement par le MatMul qui le consomme (cf. ia_onnx_resolve_matmul_weight). */
static bool ia_onnx_is_weight_reshape(const OnnxGraph* g, const OnnxNode* n) {
    if (n->op_type == NULL || strcmp(n->op_type, "Reshape") != 0 || n->ninputs < 1)
        return false;
    return onnx_find_init(g, n->inputs[0]) != NULL;
}

/* Traduit le graphe ONNX en couches de `m` (remis a vide au prealable). Rend
 * NULL en succes, sinon un message d'erreur (peut pointer dans `buf` pour les
 * messages nommant une op). N'alloue rien cote OnnxModel (lecture seule). */
static const char* ia_onnx_map_graph(ObjIAModele* m, const OnnxGraph* g,
                                     char* buf, size_t buflen) {
    m->count = 0;
    m->branchDepth = 0;

    /* Pre-passe : reconnaissance des blocs a branches (graphe non sequentiel).
     * Determine, AVANT d'emettre la moindre couche, ou ouvrir chaque bloc
     * (BRANCH_BEGIN avant le 1er node de la 1re branche), passer aux branches
     * suivantes (BRANCH_NEXT avant leur 1er node) et fusionner (MERGE_ADD a l'Add
     * ou MERGE_CONCAT au Concat). */
    IaOnnxBranchBlock res[IA_ONNX_MAX_RESIDUALS];
    int nres = ia_onnx_detect_branch_blocks(g, res);
    if (nres < 0) return "trop de blocs a branches (hors perimetre)";

    /* Node Add consomme comme biais d'un MatMul precedent (motif dense alternatif
     * MatMul+Add) : replie dans la couche Dense, saute lors de son propre tour. */
    int skipAdd = -1;

    for (int i = 0; i < g->nnodes; i++) {
        const OnnxNode* n = &g->nodes[i];
        const char* op = (n->op_type != NULL) ? n->op_type : "";

        if (i == skipAdd) continue;   /* biais MatMul deja replie */

        /* Ouverture d'un bloc dont la 1re branche demarre a ce node : le point de
         * divergence x est le tenseur courant (sortie du node precedent), capture
         * par BRANCH_BEGIN au forward. Passage a une branche suivante (BRANCH_NEXT)
         * quand ce node est le 1er d'une branche >= 2. Les branches VIDES
         * (starts[k] == -1, residuel identite) sont ouvertes au node de fusion. */
        for (int r = 0; r < nres; r++) {
            if (res[r].starts[0] == i) {
                obj_iamodele_push_layer(m, IA_LAYER_BRANCH_BEGIN, NULL, NULL);
                m->branchDepth++;
            }
        }
        for (int r = 0; r < nres; r++) {
            for (int k = 1; k < res[r].nbranch; k++) {
                if (res[r].starts[k] == i)
                    obj_iamodele_push_layer(m, IA_LAYER_BRANCH_NEXT, NULL, NULL);
            }
        }

        if (strcmp(op, "Conv") == 0) {
            if (n->ninputs < 2) return "Conv sans tenseur de poids";
            const OnnxTensor* wt = onnx_find_init(g, n->inputs[1]);
            if (wt == NULL || wt->ndim != 4) return "poids Conv absents ou de rang != 4";
            int wshape[4];
            int64_t wnel = 1;
            for (int a = 0; a < 4; a++) { wshape[a] = (int)wt->dims[a]; wnel *= wt->dims[a]; }
            if (wnel <= 0 || wt->count != wnel) return "poids Conv incoherents avec la forme";
            int cout = wshape[0];

            const OnnxAttr* strA = onnx_node_attr(n, "strides");
            int stride = 1;
            if (strA != NULL && strA->nints > 0) {
                for (int a = 1; a < strA->nints; a++) {
                    if (strA->ints[a] != strA->ints[0])
                        return "pas Conv non isotrope non supporte";
                }
                stride = (int)strA->ints[0];
            }

            const OnnxAttr* padsA = onnx_node_attr(n, "pads");
            const OnnxAttr* apA = onnx_node_attr(n, "auto_pad");
            int pad = 0;
            if (padsA != NULL && padsA->nints > 0) {
                /* ONNX pads = [debut_h, debut_w, fin_h, fin_w]. OWL n'accepte
                 * qu'un remplissage symetrique unique. */
                for (int a = 1; a < padsA->nints; a++) {
                    if (padsA->ints[a] != padsA->ints[0])
                        return "remplissage Conv asymetrique non supporte";
                }
                pad = (int)padsA->ints[0];
            } else if (apA != NULL && apA->s != NULL) {
                /* auto_pad : la VALEUR (chaine) est desormais decodee, on applique
                 * le remplissage exact au lieu de supposer SAME.
                 *  - VALID / NOTSET : aucun remplissage automatique (pad reste 0 ;
                 *    NOTSET utiliserait des pads explicites, absents ici).
                 *  - SAME_UPPER / SAME_LOWER : remplissage conservant la carte.
                 *    Pour un noyau carre impair a pas 1, il vaut (k-1)/2 de chaque
                 *    cote (symetrique). Les cas non carre / pair / pas != 1
                 *    donneraient un remplissage asymetrique -> hors perimetre. */
                if (strcmp(apA->s, "VALID") == 0 || strcmp(apA->s, "NOTSET") == 0) {
                    pad = 0;
                } else if (strcmp(apA->s, "SAME_UPPER") == 0
                        || strcmp(apA->s, "SAME_LOWER") == 0) {
                    int kh = wshape[2], kw = wshape[3];
                    if (kh != kw)      return "auto_pad Conv SAME avec noyau non carre non supporte";
                    if ((kh % 2) == 0) return "auto_pad Conv SAME avec noyau pair non supporte";
                    if (stride != 1)   return "auto_pad Conv SAME a pas different de 1 non supporte";
                    pad = (kh - 1) / 2;
                } else {
                    return "auto_pad Conv valeur non reconnue";
                }
            }

            /*  — convolution groupee : l'attribut ONNX `group`
             * partitionne les canaux. Les poids ONNX sont deja de forme
             * [Cout, Cin/group, kH, kW] (wshape[1] = Cin/group), donc aucun
             * remodelage : on lit le groupe et on le porte sur la couche. */
            const OnnxAttr* grpA = onnx_node_attr(n, "group");
            int group = 1;
            if (grpA != NULL && grpA->has_i && grpA->i > 0) group = (int)grpA->i;
            if (cout % group != 0) return "group Conv ne divise pas les canaux de sortie";

            ObjTensor* W = obj_new_tensor_nd(4, wshape);
            vm_push(OBJ_VAL(W));
            int bshape[4] = { 1, cout, 1, 1 };
            ObjTensor* bT = obj_new_tensor_nd(4, bshape);   /* biais zero-init */
            ia_onnx_fill(W, wt, wnel);
            if (n->ninputs >= 3) {
                const OnnxTensor* bt = onnx_find_init(g, n->inputs[2]);
                if (bt != NULL) ia_onnx_fill(bT, bt, cout);
            } else {
                /* Biais emis en node Add separe (Add(sortie_Conv, initializer)) :
                 * replie dans le biais du Conv, saute l'Add a son tour. Le biais
                 * est typiquement [Cout] ou reshape [Cout,1,1] — meme count. */
                int addIdx;
                const OnnxTensor* bt = ia_onnx_bias_add_after(g, i, n, &addIdx);
                if (bt != NULL) { ia_onnx_fill(bT, bt, cout); skipAdd = addIdx; }
            }
            obj_iamodele_push_layer(m, IA_LAYER_CONV2D, W, bT);
            m->layers[m->count - 1].stride = stride;
            m->layers[m->count - 1].pad    = pad;
            m->layers[m->count - 1].window = group;   /*  */
            vm_pop();   /* W — roote via le modele */

        } else if (strcmp(op, "Gemm") == 0) {
            if (n->ninputs < 2) return "Gemm sans tenseur de poids";
            const OnnxTensor* B = onnx_find_init(g, n->inputs[1]);
            if (B == NULL || B->ndim != 2) return "poids Gemm absents ou de rang != 2";
            const OnnxAttr* tbA = onnx_node_attr(n, "transB");
            bool tb = (tbA != NULL && tbA->has_i && tbA->i != 0);
            const OnnxAttr* alA = onnx_node_attr(n, "alpha");
            const OnnxAttr* beA = onnx_node_attr(n, "beta");
            double alpha = (alA != NULL && alA->has_f) ? alA->f : 1.0;
            double beta  = (beA != NULL && beA->has_f) ? beA->f : 1.0;
            /* Dense OWL calcule X@W + b avec W de forme (entrees x sorties). Gemm
             * calcule alpha*(A@op(B)) + beta*C. transB=1 => B de forme
             * (sorties x entrees) => W = alpha * transpose(B). Sinon B est deja
             * (entrees x sorties) => W = alpha * B. */
            int Brows = (int)B->dims[0], Bcols = (int)B->dims[1];
            int in, out;
            if (tb) { out = Brows; in = Bcols; } else { in = Brows; out = Bcols; }
            if (in <= 0 || out <= 0) return "dimensions Gemm invalides";
            if ((int64_t)in * out != B->count) return "poids Gemm incoherents avec la forme";

            ObjTensor* W = obj_new_tensor_matrix(in, out);
            vm_push(OBJ_VAL(W));
            ObjTensor* bT = obj_new_tensor(out);   /* biais zero-init */
            double* wd = tensor_cells(W->data);
            for (int r = 0; r < in; r++) {
                for (int c = 0; c < out; c++) {
                    double v = tb ? B->data[(int64_t)c * in + r]
                                  : B->data[(int64_t)r * out + c];
                    wd[r * out + c] = alpha * v;
                }
            }
            if (n->ninputs >= 3) {
                const OnnxTensor* C = onnx_find_init(g, n->inputs[2]);
                if (C != NULL) {
                    double* bd = tensor_cells(bT->data);
                    int64_t nc = (C->count < out) ? C->count : out;
                    for (int64_t e = 0; e < nc; e++) bd[e] = beta * C->data[e];
                }
            }
            obj_iamodele_push_layer(m, IA_LAYER_DENSE, W, bT);
            vm_pop();   /* W — roote via le modele */

        } else if (strcmp(op, "BatchNormalization") == 0) {
            /* inputs : X, scale(gamma), B(beta), input_mean, input_var. */
            if (n->ninputs < 5) return "BatchNormalization sans ses 5 entrees";
            const OnnxTensor* gm = onnx_find_init(g, n->inputs[1]);
            const OnnxTensor* bt = onnx_find_init(g, n->inputs[2]);
            const OnnxTensor* mn = onnx_find_init(g, n->inputs[3]);
            const OnnxTensor* vr = onnx_find_init(g, n->inputs[4]);
            if (gm == NULL || bt == NULL || mn == NULL || vr == NULL)
                return "parametres BatchNormalization absents";
            int C = (int)gm->count;
            if (C <= 0) return "BatchNormalization sans canaux";
            const OnnxAttr* epsA = onnx_node_attr(n, "epsilon");
            double eps = (epsA != NULL && epsA->has_f) ? epsA->f : 1e-5;

            ObjTensor* g_ = obj_new_tensor(C); vm_push(OBJ_VAL(g_));
            ObjTensor* be = obj_new_tensor(C); vm_push(OBJ_VAL(be));
            ObjTensor* rm = obj_new_tensor(C); vm_push(OBJ_VAL(rm));
            ObjTensor* rv = obj_new_tensor(C);
            ia_onnx_fill(g_, gm, C);
            ia_onnx_fill(be, bt, C);
            ia_onnx_fill(rm, mn, C);
            ia_onnx_fill(rv, vr, C);
            obj_iamodele_push_layer(m, IA_LAYER_BATCHNORM, NULL, NULL);
            m->layers[m->count - 1].gamma        = g_;
            m->layers[m->count - 1].beta         = be;
            m->layers[m->count - 1].running_mean = rm;
            m->layers[m->count - 1].running_var  = rv;
            m->layers[m->count - 1].eps          = eps;
            vm_pop(); vm_pop(); vm_pop();   /* rm, be, g_ — rootes via le modele */

        } else if (strcmp(op, "Relu") == 0) {
            obj_iamodele_push_layer(m, IA_LAYER_RELU, NULL, NULL);

        } else if (strcmp(op, "Sigmoid") == 0) {
            /* Sigmoid isole -> activation sigmoide. Sigmoid dont sortie ET entree
             * alimentent un meme Mul = branche interne d'un SiLU : on le saute, la
             * couche SiLU est emise au Mul (le tenseur courant reste t). */
            if (!ia_onnx_sigmoid_is_silu(g, n))
                obj_iamodele_push_layer(m, IA_LAYER_SIGMOID, NULL, NULL);

        } else if (strcmp(op, "Mul") == 0) {
            /*  — SiLU (Swish) : Mul(t, Sigmoid(t)), les deux entrees
             * convergeant sur le meme t (l'une via Sigmoid, saute juste avant).
             * Le tenseur courant est t -> emet une activation SiLU. Un Mul general
             * (entrees distinctes, broadcast, tete DFL) sort du perimetre. */
            if (ia_onnx_mul_is_silu(g, n)) {
                obj_iamodele_push_layer(m, IA_LAYER_SILU, NULL, NULL);
            } else {
                snprintf(buf, buflen, "operation ONNX non supportee : Mul hors motif SiLU");
                return buf;
            }

        } else if (strcmp(op, "MaxPool") == 0) {
            int window = 0, stride = 0, pad = 0;
            const char* e = ia_onnx_pool_window(n, "MaxPool", &window, &stride, &pad, buf, buflen);
            if (e != NULL) return e;
            obj_iamodele_push_layer(m, IA_LAYER_MAXPOOL, NULL, NULL);
            m->layers[m->count - 1].window = window;
            m->layers[m->count - 1].stride = stride;
            m->layers[m->count - 1].pad = pad;

        } else if (strcmp(op, "AveragePool") == 0) {
            int window = 0, stride = 0, pad = 0;
            const char* e = ia_onnx_pool_window(n, "AveragePool", &window, &stride, &pad, buf, buflen);
            if (e != NULL) return e;
            obj_iamodele_push_layer(m, IA_LAYER_AVGPOOL, NULL, NULL);
            m->layers[m->count - 1].window = window;
            m->layers[m->count - 1].stride = stride;
            m->layers[m->count - 1].pad = pad;

        } else if (strcmp(op, "GlobalAveragePool") == 0) {
            /* Moyenne sur TOUTE la carte spatiale H×W -> C×1×1. La taille exacte
             * de la carte n'est connue qu'au forward (elle depend de l'entree) ;
             * on marque une fenetre globale (window = 0) resolue en k = H a
             * l'inference (cf. ia_layer_forward). */
            obj_iamodele_push_layer(m, IA_LAYER_AVGPOOL, NULL, NULL);
            m->layers[m->count - 1].window = 0;

        } else if (strcmp(op, "MatMul") == 0) {
            /* Dense alternatif : MatMul (poids in×out) eventuellement suivi d'un
             * Add (biais). torch exporte plutot en Gemm ; ce chemin couvre les
             * exporteurs qui emettent MatMul(+Add). L'operande poids peut etre a
             * l'une OU l'autre entree (CNTK ecrit Times(W, donnee)), et peut etre
             * un initializer 2D direct OU la sortie d'un Reshape d'initializer
             * (poids denses aplatis en amont) — resolu par le helper. L'autre
             * entree est la donnee (sortie de node d'activation). */
            if (n->ninputs < 2) return "MatMul sans second operande";
            int in = 0, out = 0;
            const OnnxTensor* B = ia_onnx_resolve_matmul_weight(g, n->inputs[1], &in, &out);
            if (B == NULL)
                B = ia_onnx_resolve_matmul_weight(g, n->inputs[0], &in, &out);
            if (B == NULL) return "poids MatMul absents ou de rang != 2";
            if (in <= 0 || out <= 0) return "dimensions MatMul invalides";
            if ((int64_t)in * out != B->count) return "poids MatMul incoherents avec la forme";

            /* Le node suivant est-il un Add(biais) sur la sortie du MatMul ?
             * Meme motif « biais en node Add separe » que Conv -> biais du Dense. */
            int addIdx;
            const OnnxTensor* biasT = ia_onnx_bias_add_after(g, i, n, &addIdx);
            if (biasT != NULL) skipAdd = addIdx;

            ObjTensor* W = obj_new_tensor_matrix(in, out);
            vm_push(OBJ_VAL(W));
            ObjTensor* bT = obj_new_tensor(out);   /* biais zero-init */
            double* wd = tensor_cells(W->data);
            for (int64_t e = 0; e < (int64_t)in * out; e++) wd[e] = B->data[e];
            if (biasT != NULL) {
                double* bd = tensor_cells(bT->data);
                int64_t nc = (biasT->count < out) ? biasT->count : out;
                for (int64_t e = 0; e < nc; e++) bd[e] = biasT->data[e];
            }
            obj_iamodele_push_layer(m, IA_LAYER_DENSE, W, bT);
            vm_pop();   /* W — roote via le modele */

        } else if (strcmp(op, "Add") == 0) {
            /* Un Add n'est acceptable que comme fusion d'un bloc a branches
             * (repere en pre-passe). Tout autre Add = graphe non sequentiel hors
             * perimetre. Pour un residuel identite (2e branche vide), BRANCH_NEXT
             * n'a pas encore ete emis (aucun node de 2e branche) : on l'emet ici,
             * ouvrant la branche identite avant la fusion. */
            const IaOnnxBranchBlock* blk = NULL;
            for (int r = 0; r < nres; r++) {
                if (res[r].merge == i && res[r].kind == IA_LAYER_MERGE_ADD) { blk = &res[r]; break; }
            }
            if (blk == NULL)
                return "Add non reconnu (bloc a branches ou biais MatMul attendu)";
            /* Ouvre les branches vides (starts[k] == -1) restees en attente. */
            for (int k = 1; k < blk->nbranch; k++)
                if (blk->starts[k] < 0)
                    obj_iamodele_push_layer(m, IA_LAYER_BRANCH_NEXT, NULL, NULL);
            obj_iamodele_push_layer(m, IA_LAYER_MERGE_ADD, NULL, NULL);
            if (m->branchDepth > 0) m->branchDepth--;

        } else if (strcmp(op, "Concat") == 0) {
            /*  — un Concat n'est acceptable que comme fusion par
             * concatenation canal d'un bloc a branches (repere en pre-passe :
             * axe canal, entrees = branches d'un x commun, ordre canonique).
             * Sinon : erreur claire, jamais de mapping silencieux. */
            const IaOnnxBranchBlock* blk = NULL;
            for (int r = 0; r < nres; r++) {
                if (res[r].merge == i && res[r].kind == IA_LAYER_MERGE_CONCAT) { blk = &res[r]; break; }
            }
            if (blk == NULL) {
                const OnnxAttr* axA = onnx_node_attr(n, "axis");
                if (axA != NULL && axA->has_i && axA->i != 1)
                    return "Concat sur un axe autre que l'axe canal (axis=1) non supporte";
                return "Concat non reconnu (fusion de branches sur l'axe canal attendue)";
            }
            obj_iamodele_push_layer(m, IA_LAYER_MERGE_CONCAT, NULL, NULL);
            if (m->branchDepth > 0) m->branchDepth--;

        } else if (strcmp(op, "Reshape") == 0 && ia_onnx_is_weight_reshape(g, n)) {
            /* Reshape d'un initializer = repli de constante de poids (hors flux
             * d'activation) : n'emet aucune couche, le MatMul consommateur le
             * resout directement (ia_onnx_resolve_matmul_weight). */

        } else if (strcmp(op, "Flatten") == 0 || strcmp(op, "Reshape") == 0) {
            /* Flatten (axis=1) et Reshape (d'une activation) vers 2D avant la
             * couche dense se ramenent a l'aplatissement B x (C.H.W) d'OWL. */
            obj_iamodele_push_layer(m, IA_LAYER_FLATTEN, NULL, NULL);

        } else if (strcmp(op, "Dropout") == 0) {
            /* Dropout est une couche de regularisation d'entrainement : a
             * l'inference c'est l'identite (aucun effet). On l'ignore — aucune
             * couche emise. Le modele OWL etant purement sequentiel, sauter le
             * node revient a rebrancher sa sortie sur son entree : la couche
             * suivante consomme la sortie de la couche precedente, soit
             * exactement l'entree du Dropout. Le second output ONNX (mask) est
             * ignore. Le training-mode dropout (input training_mode=vrai) n'est
             * pas notre cas : on importe pour inferer, l'identite est correcte. */

        } else {
            snprintf(buf, buflen, "operation ONNX non supportee : %s", op);
            return buf;
        }
    }
    if (m->branchDepth != 0) return "bloc a branches ouvert jamais fusionne";
    return NULL;
}

/* ================================================================
 * Exécuteur de graphe général — DAG à fan-out
 *
 * Chemin de FALLBACK activé quand le mapping séquentiel (ia_onnx_map_graph)
 * échoue : le graphe ONNX est reconstruit en nœuds explicites lisant/écrivant
 * une TABLE DE VALEURS (un slot par tenseur ONNX d'activation). Le fan-out (un
 * tenseur relu par N nœuds) est natif. Les modèles que le chemin séquentiel sait
 * traiter n'entrent JAMAIS ici : leur comportement (et le transfert) reste
 * inchangé. L'exécuteur est inférence-first (sous g_no_grad) mais réutilise les
 * ops tenseur différentiables, donc reste entraînable par construction.
 * ================================================================ */

/* Alloue un nœud de graphe zéro-initialisé (croissance géométrique du tableau
 * C-heap). Le nœud est immédiatement dans m->gnodes[0..ngnodes) : ses Tenseurs
 * poids/biais, dès qu'affectés, sont tracés par le GC via le modèle (aucun
 * vm_push nécessaire pour les protéger d'une alloc ultérieure). */
static IAGraphNode* ia_gnode_new(ObjIAModele* m) {
    if (m->ngnodes >= m->gcapacity) {
        int nc = (m->gcapacity == 0) ? 8 : (m->gcapacity * 2);
        m->gnodes = (IAGraphNode*)realloc(m->gnodes,
                                          (size_t)nc * sizeof(IAGraphNode));
        m->gcapacity = nc;
    }
    IAGraphNode* nd = &m->gnodes[m->ngnodes++];
    memset(nd, 0, sizeof(*nd));
    return nd;
}

/* Charge un Conv ONNX dans la couche `L` (déjà dans un gnode -> tracée). Mêmes
 * règles que le chemin séquentiel (stride isotrope, remplissage symétrique ou
 * auto_pad SAME/VALID, group). Les Tenseurs sont affectés à `L` juste après
 * chaque alloc (rooting par le modèle). Rend NULL en succès, sinon un message. */
static const char* ia_graph_load_conv(const OnnxGraph* g, const OnnxNode* n,
                                      IALayer* L) {
    if (n->ninputs < 2) return "Conv sans tenseur de poids";
    const OnnxTensor* wt = onnx_find_init(g, n->inputs[1]);
    if (wt == NULL || wt->ndim != 4) return "poids Conv absents ou de rang != 4";
    int wshape[4];
    int64_t wnel = 1;
    for (int a = 0; a < 4; a++) { wshape[a] = (int)wt->dims[a]; wnel *= wt->dims[a]; }
    if (wnel <= 0 || wt->count != wnel) return "poids Conv incoherents avec la forme";
    int cout = wshape[0];

    int stride = 1;
    const OnnxAttr* strA = onnx_node_attr(n, "strides");
    if (strA != NULL && strA->nints > 0) {
        for (int a = 1; a < strA->nints; a++)
            if (strA->ints[a] != strA->ints[0]) return "pas Conv non isotrope non supporte";
        stride = (int)strA->ints[0];
    }
    int pad = 0;
    const OnnxAttr* padsA = onnx_node_attr(n, "pads");
    const OnnxAttr* apA   = onnx_node_attr(n, "auto_pad");
    if (padsA != NULL && padsA->nints > 0) {
        for (int a = 1; a < padsA->nints; a++)
            if (padsA->ints[a] != padsA->ints[0]) return "remplissage Conv asymetrique non supporte";
        pad = (int)padsA->ints[0];
    } else if (apA != NULL && apA->s != NULL) {
        if (strcmp(apA->s, "VALID") == 0 || strcmp(apA->s, "NOTSET") == 0) {
            pad = 0;
        } else if (strcmp(apA->s, "SAME_UPPER") == 0 || strcmp(apA->s, "SAME_LOWER") == 0) {
            int kh = wshape[2], kw = wshape[3];
            if (kh != kw)      return "auto_pad Conv SAME avec noyau non carre non supporte";
            if ((kh % 2) == 0) return "auto_pad Conv SAME avec noyau pair non supporte";
            if (stride != 1)   return "auto_pad Conv SAME a pas different de 1 non supporte";
            pad = (kh - 1) / 2;
        } else {
            return "auto_pad Conv valeur non reconnue";
        }
    }
    int group = 1;
    const OnnxAttr* grpA = onnx_node_attr(n, "group");
    if (grpA != NULL && grpA->has_i && grpA->i > 0) group = (int)grpA->i;
    if (cout % group != 0) return "group Conv ne divise pas les canaux de sortie";

    ObjTensor* W = obj_new_tensor_nd(4, wshape);
    L->weights = W;                                  /* roote via le modele */
    int bshape[4] = { 1, cout, 1, 1 };
    ObjTensor* bT = obj_new_tensor_nd(4, bshape);    /* W survit (trace) */
    L->bias = bT;
    ia_onnx_fill(W, wt, wnel);
    if (n->ninputs >= 3) {
        const OnnxTensor* bt = onnx_find_init(g, n->inputs[2]);
        if (bt != NULL) ia_onnx_fill(bT, bt, cout);
    }
    L->kind   = IA_LAYER_CONV2D;
    L->stride = stride;
    L->pad    = pad;
    L->window = group;
    return NULL;
}

/* Charge un Gemm ONNX en couche Dense (W = alpha·[transpose] B, biais = beta·C). */
static const char* ia_graph_load_gemm(const OnnxGraph* g, const OnnxNode* n,
                                      IALayer* L) {
    if (n->ninputs < 2) return "Gemm sans tenseur de poids";
    const OnnxTensor* B = onnx_find_init(g, n->inputs[1]);
    if (B == NULL || B->ndim != 2) return "poids Gemm absents ou de rang != 2";
    const OnnxAttr* tbA = onnx_node_attr(n, "transB");
    bool tb = (tbA != NULL && tbA->has_i && tbA->i != 0);
    const OnnxAttr* alA = onnx_node_attr(n, "alpha");
    const OnnxAttr* beA = onnx_node_attr(n, "beta");
    double alpha = (alA != NULL && alA->has_f) ? alA->f : 1.0;
    double beta  = (beA != NULL && beA->has_f) ? beA->f : 1.0;
    int Brows = (int)B->dims[0], Bcols = (int)B->dims[1];
    int in, out;
    if (tb) { out = Brows; in = Bcols; } else { in = Brows; out = Bcols; }
    if (in <= 0 || out <= 0) return "dimensions Gemm invalides";
    if ((int64_t)in * out != B->count) return "poids Gemm incoherents avec la forme";

    ObjTensor* W = obj_new_tensor_matrix(in, out);
    L->weights = W;
    ObjTensor* bT = obj_new_tensor(out);
    L->bias = bT;
    double* wd = tensor_cells(W->data);
    for (int r = 0; r < in; r++)
        for (int c = 0; c < out; c++) {
            double v = tb ? B->data[(int64_t)c * in + r] : B->data[(int64_t)r * out + c];
            wd[r * out + c] = alpha * v;
        }
    if (n->ninputs >= 3) {
        const OnnxTensor* C = onnx_find_init(g, n->inputs[2]);
        if (C != NULL) {
            double* bd = tensor_cells(bT->data);
            int64_t nc = (C->count < out) ? C->count : out;
            for (int64_t e = 0; e < nc; e++) bd[e] = beta * C->data[e];
        }
    }
    L->kind = IA_LAYER_DENSE;
    return NULL;
}

/* Charge un BatchNormalization ONNX (gamma/beta/moyenne/variance + eps). */
static const char* ia_graph_load_bn(const OnnxGraph* g, const OnnxNode* n,
                                    IALayer* L) {
    if (n->ninputs < 5) return "BatchNormalization sans ses 5 entrees";
    const OnnxTensor* gm = onnx_find_init(g, n->inputs[1]);
    const OnnxTensor* bt = onnx_find_init(g, n->inputs[2]);
    const OnnxTensor* mn = onnx_find_init(g, n->inputs[3]);
    const OnnxTensor* vr = onnx_find_init(g, n->inputs[4]);
    if (gm == NULL || bt == NULL || mn == NULL || vr == NULL)
        return "parametres BatchNormalization absents";
    int C = (int)gm->count;
    if (C <= 0) return "BatchNormalization sans canaux";
    const OnnxAttr* epsA = onnx_node_attr(n, "epsilon");
    double eps = (epsA != NULL && epsA->has_f) ? epsA->f : 1e-5;

    ObjTensor* g_ = obj_new_tensor(C); L->gamma        = g_;
    ObjTensor* be = obj_new_tensor(C); L->beta         = be;
    ObjTensor* rm = obj_new_tensor(C); L->running_mean = rm;
    ObjTensor* rv = obj_new_tensor(C); L->running_var  = rv;
    ia_onnx_fill(g_, gm, C);
    ia_onnx_fill(be, bt, C);
    ia_onnx_fill(rm, mn, C);
    ia_onnx_fill(rv, vr, C);
    L->kind = IA_LAYER_BATCHNORM;
    L->eps  = eps;
    return NULL;
}

/* Vrai si `name` est consommé par au moins un nœud du graphe (sert à repérer la
 * sortie du graphe : produite mais consommée par personne). */
static bool ia_onnx_consumed(const OnnxGraph* g, const char* name) {
    if (name == NULL) return false;
    for (int i = 0; i < g->nnodes; i++)
        for (int k = 0; k < g->nodes[i].ninputs; k++)
            if (g->nodes[i].inputs[k] != NULL &&
                strcmp(g->nodes[i].inputs[k], name) == 0) return true;
    return false;
}

/* Opérande `t` d'un Mul formant un SiLU (Mul(t, Sigmoid(t))) : l'entrée du Mul
 * qui est aussi l'entrée du Sigmoid produisant l'autre opérande. NULL sinon. */
static const char* ia_onnx_silu_t(const OnnxGraph* g, const OnnxNode* mul) {
    if (mul->ninputs != 2) return NULL;
    const char* a = mul->inputs[0];
    const char* b = mul->inputs[1];
    if (a == NULL || b == NULL) return NULL;
    for (int swap = 0; swap < 2; swap++) {
        const char* sigOut = (swap == 0) ? a : b;
        const char* other  = (swap == 0) ? b : a;
        int p = ia_onnx_producer(g, sigOut);
        if (p < 0) continue;
        const OnnxNode* sn = &g->nodes[p];
        if (sn->op_type != NULL && strcmp(sn->op_type, "Sigmoid") == 0 &&
            sn->ninputs >= 1 && sn->inputs[0] != NULL &&
            strcmp(sn->inputs[0], other) == 0) return other;
    }
    return NULL;
}

/* Table nom de tenseur ONNX -> slot de la table de valeurs. */
typedef struct { const char* name; int slot; } IaNameSlot;
static int ia_ns_get(const IaNameSlot* map, int nmap, const char* name) {
    if (name == NULL) return -1;
    for (int i = 0; i < nmap; i++)
        if (map[i].name != NULL && strcmp(map[i].name, name) == 0) return map[i].slot;
    return -1;
}

/* Exécute le graphe général de `m` sur `input`. La table de valeurs est un
 * ObjArray de `nvalues` slots, ROOTÉ sur la pile VM pendant tout le forward :
 * c'est la racine GC des activations intermédiaires (le point sensible de ce
 * ticket). Chaque nœud lit ses slots d'entrée, calcule via une op tenseur
 * existante, écrit sa/ses sortie(s) — un slot écrit est immédiatement rooté par
 * l'ObjArray. Rend le Tenseur du slot de sortie (image unique -> 1D comme le
 * chemin séquentiel), NIL_VAL sur erreur (graphe non topologique ou op qui
 * échoue). Exécuté sous g_no_grad par l'appelant. */
static Value ia_graph_forward(ObjIAModele* m, ObjTensor* input) {
    ObjArray* vt = obj_new_array();
    vm_push(OBJ_VAL(vt));
    for (int i = 0; i < m->nvalues; i++) obj_array_push(vt, NIL_VAL);
    /* Plus aucun push sur vt : le buffer `values` ne bouge plus, on écrit les
     * slots directement (écriture d'un slot déjà alloué = pas d'alloc). */
    Value* slots = vt->elements.values;
    slots[m->graphInSlot] = OBJ_VAL(input);

    bool singleSample = (input->ndim >= 3 && input->shape[0] == 1);

    for (int ni = 0; ni < m->ngnodes; ni++) {
        IAGraphNode* nd = &m->gnodes[ni];
        ObjTensor* in[IA_GRAPH_MAX_ARITY];
        for (int k = 0; k < nd->ninputs; k++) {
            Value v = slots[nd->inputs[k]];
            if (!IS_TENSOR(v)) { vm_pop(); return NIL_VAL; }  /* slot non produit */
            in[k] = AS_TENSOR(v);
        }

        switch (nd->op) {
            case IA_GOP_LAYER: {
                ObjTensor* out = ia_layer_forward(&nd->layer, in[0], /*eval=*/true);
                if (out == NULL) { vm_pop(); return NIL_VAL; }
                slots[nd->outputs[0]] = OBJ_VAL(out);
                break;
            }
            case IA_GOP_SPLIT: {
                int cstart = 0;
                for (int k = 0; k < nd->noutputs; k++) {
                    ObjTensor* out = obj_tensor_slice_ch(in[0], cstart, nd->split[k]);
                    if (out == NULL) { vm_pop(); return NIL_VAL; }
                    slots[nd->outputs[k]] = OBJ_VAL(out);   /* rooté aussitôt */
                    cstart += nd->split[k];
                }
                break;
            }
            case IA_GOP_CONCAT: {
                int axis = nd->axes[0];
                if (axis < 0) axis += in[0]->ndim;
                ObjTensor* acc = in[0];
                int outSlot = nd->outputs[0];
                slots[outSlot] = OBJ_VAL(acc);
                for (int k = 1; k < nd->ninputs; k++) {
                    /* Axe canal : garde le nœud differentiable (transfert inchange) ;
                     * autre axe : concat forward-only general. */
                    ObjTensor* c = (axis == 1) ? obj_tensor_concat_ch(acc, in[k])
                                               : obj_tensor_concat_axis(acc, in[k], axis);
                    if (c == NULL) { vm_pop(); return NIL_VAL; }
                    acc = c;
                    slots[outSlot] = OBJ_VAL(acc);
                }
                break;
            }
            case IA_GOP_TRANSPOSE: {
                int ndm = in[0]->ndim;
                int perm[OWL_TENSOR_MAX_RANK];
                if (nd->naxes == 0) {           /* perm absent : inversion des axes */
                    for (int i = 0; i < ndm; i++) perm[i] = ndm - 1 - i;
                } else {
                    if (nd->naxes != ndm) { vm_pop(); return NIL_VAL; }
                    for (int i = 0; i < ndm; i++) perm[i] = nd->axes[i];
                }
                ObjTensor* out = obj_tensor_transpose(in[0], perm);
                if (out == NULL) { vm_pop(); return NIL_VAL; }
                slots[nd->outputs[0]] = OBJ_VAL(out);
                break;
            }
            case IA_GOP_SOFTMAX: {
                ObjTensor* out = obj_tensor_softmax(in[0], nd->axes[0]);
                if (out == NULL) { vm_pop(); return NIL_VAL; }
                slots[nd->outputs[0]] = OBJ_VAL(out);
                break;
            }
            case IA_GOP_SLICE: {
                ObjTensor* out = obj_tensor_slice(in[0], nd->naxes, nd->axes,
                                                  nd->sl_start, nd->sl_end, nd->sl_step);
                if (out == NULL) { vm_pop(); return NIL_VAL; }
                slots[nd->outputs[0]] = OBJ_VAL(out);
                break;
            }
            case IA_GOP_ELTWISE: {
                ObjTensor* a; ObjTensor* b;
                if (nd->elt_const_side == 1)      { a = nd->layer.weights; b = in[0]; }
                else if (nd->elt_const_side == 2) { a = in[0]; b = nd->layer.weights; }
                else                              { a = in[0]; b = in[1]; }
                /* a et b rootes : slots via vt, constante via m->gnodes[].layer.weights. */
                ObjTensor* out = obj_tensor_eltwise(nd->elt_op, a, b);
                if (out == NULL) { vm_pop(); return NIL_VAL; }
                slots[nd->outputs[0]] = OBJ_VAL(out);
                break;
            }
            case IA_GOP_RESHAPE: {
                /* Forme cible : 0 = garde la dimension d'entree, -1 = deduite du
                 * nombre total d'elements (semantique ONNX Reshape). */
                int tgt[OWL_TENSOR_MAX_RANK];
                int nt = nd->naxes;
                int numel = tensor_numel(in[0]->data);
                int prod = 1, inferIdx = -1;
                for (int i = 0; i < nt; i++) {
                    int d = nd->axes[i];
                    if (d == 0) d = (i < in[0]->ndim) ? in[0]->shape[i] : 1;
                    if (d < 0) { inferIdx = i; tgt[i] = 1; }
                    else       { tgt[i] = d; prod *= d; }
                }
                if (inferIdx >= 0) {
                    if (prod <= 0 || numel % prod != 0) { vm_pop(); return NIL_VAL; }
                    tgt[inferIdx] = numel / prod;
                    prod *= tgt[inferIdx];
                }
                if (prod != numel) { vm_pop(); return NIL_VAL; }   /* forme incoherente */
                ObjTensor* out = obj_tensor_reshape(in[0], nt, tgt);
                if (out == NULL) { vm_pop(); return NIL_VAL; }
                slots[nd->outputs[0]] = OBJ_VAL(out);
                break;
            }
            case IA_GOP_ADD: {
                ObjTensor* acc = in[0];
                int outSlot = nd->outputs[0];
                slots[outSlot] = OBJ_VAL(acc);
                for (int k = 1; k < nd->ninputs; k++) {
                    if (!tensor_shapes_equal(acc, in[k])) { vm_pop(); return NIL_VAL; }
                    ObjTensor* c = obj_tensor_binop(TENSOR_OP_ADD, acc, in[k]);
                    if (c == NULL) { vm_pop(); return NIL_VAL; }
                    acc = c;
                    slots[outSlot] = OBJ_VAL(acc);
                }
                break;
            }
            case IA_GOP_REDUCEMEAN: {
                ObjTensor* cur = in[0];
                int origNdim = cur->ndim;
                int ax[OWL_TENSOR_MAX_RANK];
                int nax = nd->naxes;
                for (int a = 0; a < nax; a++) {
                    int v = nd->axes[a];
                    ax[a] = (v < 0) ? v + origNdim : v;
                }
                for (int a = 0; a < nax; a++)      /* tri décroissant (rangs stables) */
                    for (int b2 = a + 1; b2 < nax; b2++)
                        if (ax[b2] > ax[a]) { int t = ax[a]; ax[a] = ax[b2]; ax[b2] = t; }
                int outSlot = nd->outputs[0];
                slots[outSlot] = OBJ_VAL(cur);
                NativeFn mean = ia_tensor_method("Moyenne");
                for (int a = 0; a < nax; a++) {
                    Value margs[2] = { OBJ_VAL(cur), REAL_VAL((double)(ax[a] + 1)) };
                    Value r = mean(2, margs);
                    if (!IS_TENSOR(r)) { vm_pop(); return NIL_VAL; }
                    cur = AS_TENSOR(r);
                    slots[outSlot] = OBJ_VAL(cur);
                }
                if (nd->keepdims) {
                    int shp[OWL_TENSOR_MAX_RANK];
                    for (int d = 0; d < origNdim; d++) shp[d] = in[0]->shape[d];
                    for (int a = 0; a < nax; a++) shp[ax[a]] = 1;
                    ObjTensor* rs = obj_tensor_reshape(cur, origNdim, shp);
                    cur = rs;
                    slots[outSlot] = OBJ_VAL(cur);
                }
                break;
            }
            case IA_GOP_CONST: {
                /* Tenseur constant (facteurs Resize) : pose dans son slot, deja
                 * trace via les poids du gnode. */
                slots[nd->outputs[0]] = OBJ_VAL(nd->layer.weights);
                break;
            }
            case IA_GOP_RESIZE: {
                int sy, sx;
                if (nd->ninputs == 2) {
                    /* scales dans un slot (sortie d'un nœud Constant) =
                     * [sN, sC, sy, sx] : facteurs sur les 2 derniers axes. */
                    ObjTensor* scales = in[1];
                    int c = tensor_numel(scales->data);
                    if (c < 2) { vm_pop(); return NIL_VAL; }
                    double* sc = tensor_cells(scales->data);
                    sy = (int)(sc[c - 2] + 0.5);   /* facteur entier (asymmetric/floor) */
                    sx = (int)(sc[c - 1] + 0.5);
                } else {
                    /* facteurs figes resolus au build (scales initializer). */
                    sy = nd->split[0];
                    sx = nd->split[1];
                }
                ObjTensor* out = obj_tensor_upsample_nearest(in[0], sy, sx);
                if (out == NULL) { vm_pop(); return NIL_VAL; }
                slots[nd->outputs[0]] = OBJ_VAL(out);   /* rooté aussitôt */
                break;
            }
        }
    }

    if (m->graphOutSlot < 0 || !IS_TENSOR(slots[m->graphOutSlot])) {
        vm_pop();
        return NIL_VAL;
    }
    ObjTensor* out = AS_TENSOR(slots[m->graphOutSlot]);
    if (singleSample && out->ndim == 2 && out->shape[0] == 1) {
        /* Image unique -> Tenseur 1D [classes] (sémantique ). out reste
         * rooté via la table pendant l'alloc du 1D. */
        ObjMatrix* cm = (ObjMatrix*)out->data;
        int oc = cm->cols;
        ObjTensor* v = obj_new_tensor(oc);
        double* vd = tensor_cells(v->data);
        for (int i = 0; i < oc; i++) vd[i] = cm->data[i];
        vm_pop();
        return OBJ_VAL(v);
    }
    vm_pop();
    return OBJ_VAL(out);
}

/*  — remplit `nd` pour un nœud élément-par-élément (Add/Sub/Mul/Div) à
 * 2 opérandes avec diffusion. Un opérande peut être un initializer constant
 * (matérialisé en Tenseur porté par nd->layer.weights, tracé GC via le modèle) ;
 * l'autre est un slot d'activation. `nd` est déjà dans m->gnodes (alloc du const
 * protégée par ce rooting). Rend NULL en succès, sinon un message. */
static const char* ia_gnode_eltwise(const OnnxGraph* g, const OnnxNode* n,
                                    const IaNameSlot* map, int nmap,
                                    IAGraphNode* nd, int eltop) {
    if (n->ninputs != 2)
        return "operation ONNX non supportee : arithmetique a arite != 2";
    const OnnxTensor* c0 = onnx_find_init(g, n->inputs[0]);
    const OnnxTensor* c1 = onnx_find_init(g, n->inputs[1]);
    if (c0 != NULL && c1 != NULL)
        return "operation ONNX non supportee : arithmetique de deux constantes";
    nd->op = IA_GOP_ELTWISE;
    nd->elt_op = eltop;
    if (c1 != NULL) {                 /* slot OP const (tenseur - vecteur, / scalaire) */
        int s = ia_ns_get(map, nmap, n->inputs[0]);
        if (s < 0) return "graphe ONNX non topologique (arithmetique)";
        ObjTensor* ct = ia_onnx_const_to_tensor(c1);
        if (ct == NULL) return "arithmetique : constante de forme incoherente";
        nd->layer.weights = ct;
        nd->elt_const_side = 2;
        nd->inputs[0] = s; nd->ninputs = 1;
    } else if (c0 != NULL) {          /* const OP slot */
        int s = ia_ns_get(map, nmap, n->inputs[1]);
        if (s < 0) return "graphe ONNX non topologique (arithmetique)";
        ObjTensor* ct = ia_onnx_const_to_tensor(c0);
        if (ct == NULL) return "arithmetique : constante de forme incoherente";
        nd->layer.weights = ct;
        nd->elt_const_side = 1;
        nd->inputs[0] = s; nd->ninputs = 1;
    } else {                          /* slot OP slot */
        int s0 = ia_ns_get(map, nmap, n->inputs[0]);
        int s1 = ia_ns_get(map, nmap, n->inputs[1]);
        if (s0 < 0 || s1 < 0) return "graphe ONNX non topologique (arithmetique)";
        nd->elt_const_side = 0;
        nd->inputs[0] = s0; nd->inputs[1] = s1; nd->ninputs = 2;
    }
    return NULL;
}

/* Reconstruit `m` en graphe général depuis le graphe ONNX (chemin de fallback,
 * appelé seulement si ia_onnx_map_graph a échoué). Assigne un slot à l'entrée du
 * graphe et à chaque sortie d'activation d'un nœud ; les initializers (poids)
 * sont repliés dans les nœuds (jamais de slot). Rend NULL en succès (m->ngnodes
 * > 0), sinon un message (peut pointer dans buf), m laissé vide et cohérent. */
static const char* ia_onnx_build_graph(ObjIAModele* m, const OnnxGraph* g,
                                       char* buf, size_t buflen) {
    m->count = 0;
    m->branchDepth = 0;
    m->ngnodes = 0;
    m->nvalues = 0;
    m->graphInSlot = -1;
    m->graphOutSlot = -1;

    int cap = g->nnodes * 2 + 8;
    IaNameSlot* map = (IaNameSlot*)malloc((size_t)cap * sizeof(IaNameSlot));
    if (map == NULL) return "memoire insuffisante (graphe)";
    int nmap = 0;
    int nextSlot = 0;

    #define IA_GB_FAIL(msg) do { free(map); m->ngnodes = 0; return (msg); } while (0)

    /* Entrée du graphe : un tenseur consommé, non produit, non initializer. */
    const char* inputName = NULL;
    for (int i = 0; i < g->nnodes && inputName == NULL; i++) {
        for (int k = 0; k < g->nodes[i].ninputs; k++) {
            const char* nm = g->nodes[i].inputs[k];
            if (nm == NULL) continue;
            if (onnx_find_init(g, nm) != NULL) continue;
            if (ia_onnx_producer(g, nm) >= 0) continue;
            inputName = nm;
            break;
        }
    }
    if (inputName == NULL) IA_GB_FAIL("graphe ONNX sans entree identifiable");
    map[nmap].name = inputName; map[nmap].slot = nextSlot++; nmap++;
    m->graphInSlot = 0;

    for (int i = 0; i < g->nnodes; i++) {
        const OnnxNode* n = &g->nodes[i];
        const char* op = (n->op_type != NULL) ? n->op_type : "";

        /* Nœuds sans slot de sortie propre. */
        if (strcmp(op, "Sigmoid") == 0 && ia_onnx_sigmoid_is_silu(g, n)) continue;
        if (ia_onnx_is_weight_reshape(g, n)) continue;
        if (strcmp(op, "Dropout") == 0) {
            /* Identité à l'inférence : la sortie ALIASE le slot d'entrée. */
            int s = ia_ns_get(map, nmap, n->ninputs > 0 ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Dropout)");
            if (n->noutputs > 0 && n->outputs[0] != NULL && nmap < cap) {
                map[nmap].name = n->outputs[0]; map[nmap].slot = s; nmap++;
            }
            continue;
        }

        IAGraphNode* nd = ia_gnode_new(m);

        if (strcmp(op, "Conv") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Conv)");
            const char* e = ia_graph_load_conv(g, n, &nd->layer);
            if (e != NULL) IA_GB_FAIL(e);
            nd->op = IA_GOP_LAYER;
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "Gemm") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Gemm)");
            const char* e = ia_graph_load_gemm(g, n, &nd->layer);
            if (e != NULL) IA_GB_FAIL(e);
            nd->op = IA_GOP_LAYER;
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "MatMul") == 0) {
            /* MatMul(donnee, poids) sans le repli Reshape (traité comme Dense). */
            if (n->ninputs < 2) IA_GB_FAIL("MatMul sans second operande");
            int in = 0, out = 0;
            const OnnxTensor* B = ia_onnx_resolve_matmul_weight(g, n->inputs[1], &in, &out);
            const char* dataName = n->inputs[0];
            if (B == NULL) {
                B = ia_onnx_resolve_matmul_weight(g, n->inputs[0], &in, &out);
                dataName = n->inputs[1];
            }
            if (B == NULL) IA_GB_FAIL("poids MatMul absents ou de rang != 2");
            if (in <= 0 || out <= 0) IA_GB_FAIL("dimensions MatMul invalides");
            if ((int64_t)in * out != B->count) IA_GB_FAIL("poids MatMul incoherents avec la forme");
            int s = ia_ns_get(map, nmap, dataName);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (MatMul)");
            ObjTensor* W = obj_new_tensor_matrix(in, out);
            nd->layer.weights = W;
            ObjTensor* bT = obj_new_tensor(out);
            nd->layer.bias = bT;
            double* wd = tensor_cells(W->data);
            for (int64_t e = 0; e < (int64_t)in * out; e++) wd[e] = B->data[e];
            nd->layer.kind = IA_LAYER_DENSE;
            nd->op = IA_GOP_LAYER;
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "BatchNormalization") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (BatchNormalization)");
            const char* e = ia_graph_load_bn(g, n, &nd->layer);
            if (e != NULL) IA_GB_FAIL(e);
            nd->op = IA_GOP_LAYER;
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "Relu") == 0 || strcmp(op, "Sigmoid") == 0
                || strcmp(op, "Tanh") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (activation)");
            nd->op = IA_GOP_LAYER;
            nd->layer.kind = (strcmp(op, "Relu") == 0)    ? IA_LAYER_RELU
                           : (strcmp(op, "Sigmoid") == 0) ? IA_LAYER_SIGMOID
                                                          : IA_LAYER_TANH;
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "Mul") == 0) {
            /* SiLU : Mul(t, Sigmoid(t)) -> activation SiLU sur le slot de t. Hors
             * ce motif, Mul general element-par-element (broadcast, ). */
            const char* t = ia_onnx_silu_t(g, n);
            if (t != NULL) {
                int s = ia_ns_get(map, nmap, t);
                if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (SiLU)");
                nd->op = IA_GOP_LAYER;
                nd->layer.kind = IA_LAYER_SILU;
                nd->inputs[0] = s; nd->ninputs = 1;
            } else {
                const char* e = ia_gnode_eltwise(g, n, map, nmap, nd, IA_ELT_MUL);
                if (e != NULL) IA_GB_FAIL(e);
            }

        } else if (strcmp(op, "MaxPool") == 0 || strcmp(op, "AveragePool") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (pooling)");
            int window = 0, stride = 0, pad = 0;
            const char* e = ia_onnx_pool_window(n, op, &window, &stride, &pad, buf, buflen);
            if (e != NULL) IA_GB_FAIL(e);
            nd->op = IA_GOP_LAYER;
            nd->layer.kind = (strcmp(op, "MaxPool") == 0) ? IA_LAYER_MAXPOOL : IA_LAYER_AVGPOOL;
            nd->layer.window = window;
            nd->layer.stride = stride;
            nd->layer.pad = pad;
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "GlobalAveragePool") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (GlobalAveragePool)");
            nd->op = IA_GOP_LAYER;
            nd->layer.kind = IA_LAYER_AVGPOOL;
            nd->layer.window = 0;   /* fenetre globale, resolue au forward */
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "Flatten") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Flatten)");
            nd->op = IA_GOP_LAYER;
            nd->layer.kind = IA_LAYER_FLATTEN;
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "Reshape") == 0) {
            /*  — Reshape a la forme cible (initializer INT64, 0=garde
             * l'axe, -1=deduit) : necessaire a la tete DFL (reshape 3D/4D non
             * aplatissante). Sans forme statique, repli sur l'aplatissement 2D
             * (comportement historique des modeles de classification). */
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Reshape)");
            const OnnxTensor* shp = (n->ninputs >= 2) ? onnx_find_init(g, n->inputs[1]) : NULL;
            if (shp != NULL && shp->count >= 1) {
                if (shp->count > OWL_TENSOR_MAX_RANK)
                    IA_GB_FAIL("Reshape : rang cible hors bornes");
                nd->op = IA_GOP_RESHAPE;
                nd->inputs[0] = s; nd->ninputs = 1;
                nd->naxes = (int)shp->count;
                for (int k = 0; k < (int)shp->count; k++) nd->axes[k] = (int)shp->data[k];
            } else {
                nd->op = IA_GOP_LAYER;
                nd->layer.kind = IA_LAYER_FLATTEN;
                nd->inputs[0] = s; nd->ninputs = 1;
            }

        } else if (strcmp(op, "Split") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Split)");
            const OnnxAttr* axA = onnx_node_attr(n, "axis");
            if (axA == NULL || !axA->has_i || axA->i != 1)
                IA_GB_FAIL("operation ONNX non supportee : Split sur un axe autre que l'axe canal");
            if (n->noutputs < 1 || n->noutputs > IA_GRAPH_MAX_ARITY)
                IA_GB_FAIL("Split : nombre de sorties hors perimetre");
            /* Tailles de tranches : soit l'attribut `split` (opset < 13), soit un
             * SECOND INPUT initializer INT64 (opset 13+, cas YOLOv8 C2f). */
            const OnnxAttr* spA = onnx_node_attr(n, "split");
            if (spA != NULL && spA->nints >= 1) {
                if (spA->nints != n->noutputs)
                    IA_GB_FAIL("Split : nombre de tranches incoherent avec les sorties");
                for (int k = 0; k < spA->nints; k++) nd->split[k] = (int)spA->ints[k];
            } else if (n->ninputs >= 2) {
                const OnnxTensor* st = onnx_find_init(g, n->inputs[1]);
                if (st == NULL || st->count < 1)
                    IA_GB_FAIL("operation ONNX non supportee : Split sans tailles (repartition dynamique)");
                if ((int)st->count != n->noutputs)
                    IA_GB_FAIL("Split : nombre de tranches incoherent avec les sorties");
                for (int k = 0; k < n->noutputs; k++) nd->split[k] = (int)st->data[k];
            } else {
                IA_GB_FAIL("operation ONNX non supportee : Split sans tailles (repartition dynamique)");
            }
            nd->op = IA_GOP_SPLIT;
            nd->inputs[0] = s; nd->ninputs = 1;

        } else if (strcmp(op, "Concat") == 0) {
            /*  — Concat sur un axe quelconque (axe canal ou non). L'axe
             * (eventuellement negatif) est resolu au forward via le rang reel. */
            const OnnxAttr* axA = onnx_node_attr(n, "axis");
            if (axA == NULL || !axA->has_i)
                IA_GB_FAIL("operation ONNX non supportee : Concat sans axe");
            if (n->ninputs < 2 || n->ninputs > IA_GRAPH_MAX_ARITY)
                IA_GB_FAIL("Concat : nombre d'entrees hors perimetre");
            nd->op = IA_GOP_CONCAT;
            nd->ninputs = n->ninputs;
            nd->naxes = 1;
            nd->axes[0] = (int)axA->i;
            for (int k = 0; k < n->ninputs; k++) {
                if (onnx_find_init(g, n->inputs[k]) != NULL)
                    IA_GB_FAIL("operation ONNX non supportee : Concat d'une constante");
                int s = ia_ns_get(map, nmap, n->inputs[k]);
                if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Concat)");
                nd->inputs[k] = s;
            }

        } else if (strcmp(op, "Add") == 0) {
            if (n->ninputs < 2 || n->ninputs > IA_GRAPH_MAX_ARITY)
                IA_GB_FAIL("Add : nombre d'entrees hors perimetre");
            /* Add de deux activations (residuel, fusion) : nœud ADD differentiable.
             *  : Add avec un operande constant (biais/decalage) -> nœud
             * element-par-element avec diffusion. */
            bool hasConst = false;
            for (int k = 0; k < n->ninputs; k++)
                if (onnx_find_init(g, n->inputs[k]) != NULL) { hasConst = true; break; }
            if (!hasConst) {
                nd->op = IA_GOP_ADD;
                nd->ninputs = n->ninputs;
                for (int k = 0; k < n->ninputs; k++) {
                    int s = ia_ns_get(map, nmap, n->inputs[k]);
                    if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Add)");
                    nd->inputs[k] = s;
                }
            } else {
                const char* e = ia_gnode_eltwise(g, n, map, nmap, nd, IA_ELT_ADD);
                if (e != NULL) IA_GB_FAIL(e);
            }

        } else if (strcmp(op, "Sub") == 0) {
            const char* e = ia_gnode_eltwise(g, n, map, nmap, nd, IA_ELT_SUB);
            if (e != NULL) IA_GB_FAIL(e);

        } else if (strcmp(op, "Div") == 0) {
            const char* e = ia_gnode_eltwise(g, n, map, nmap, nd, IA_ELT_DIV);
            if (e != NULL) IA_GB_FAIL(e);

        } else if (strcmp(op, "Transpose") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Transpose)");
            const OnnxAttr* permA = onnx_node_attr(n, "perm");
            nd->op = IA_GOP_TRANSPOSE;
            nd->inputs[0] = s; nd->ninputs = 1;
            if (permA != NULL && permA->nints >= 1) {
                if (permA->nints > OWL_TENSOR_MAX_RANK)
                    IA_GB_FAIL("Transpose : rang hors bornes");
                nd->naxes = permA->nints;
                for (int k = 0; k < permA->nints; k++) nd->axes[k] = (int)permA->ints[k];
            } else {
                nd->naxes = 0;   /* perm absent : inversion des axes, resolue au forward */
            }

        } else if (strcmp(op, "Softmax") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Softmax)");
            const OnnxAttr* axA = onnx_node_attr(n, "axis");
            nd->op = IA_GOP_SOFTMAX;
            nd->inputs[0] = s; nd->ninputs = 1;
            nd->naxes = 1;
            nd->axes[0] = (axA != NULL && axA->has_i) ? (int)axA->i : -1;  /* defaut opset 13+ */

        } else if (strcmp(op, "Slice") == 0) {
            /* Slice ONNX opset 10+ : bornes en INPUTS (starts, ends, axes?, steps?),
             * tous initializers INT64 ( les decode). Bornes dynamiques
             * (sortie d'un nœud) hors perimetre. */
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (Slice)");
            if (n->ninputs < 3)
                IA_GB_FAIL("operation ONNX non supportee : Slice sans starts/ends");
            const OnnxTensor* startsT = onnx_find_init(g, n->inputs[1]);
            const OnnxTensor* endsT   = onnx_find_init(g, n->inputs[2]);
            if (startsT == NULL || endsT == NULL)
                IA_GB_FAIL("operation ONNX non supportee : Slice a bornes dynamiques");
            int nsl = (int)startsT->count;
            if (nsl < 1 || nsl > OWL_TENSOR_MAX_RANK || (int)endsT->count != nsl)
                IA_GB_FAIL("Slice : nombre de bornes incoherent");
            const char* axesName  = (n->ninputs >= 4) ? n->inputs[3] : NULL;
            const char* stepsName = (n->ninputs >= 5) ? n->inputs[4] : NULL;
            bool axesPresent  = axesName  != NULL && axesName[0]  != '\0';
            bool stepsPresent = stepsName != NULL && stepsName[0] != '\0';
            const OnnxTensor* axesT  = axesPresent  ? onnx_find_init(g, axesName)  : NULL;
            const OnnxTensor* stepsT = stepsPresent ? onnx_find_init(g, stepsName) : NULL;
            if ((axesPresent && axesT == NULL) || (stepsPresent && stepsT == NULL))
                IA_GB_FAIL("operation ONNX non supportee : Slice a axes/pas dynamiques");
            if ((axesT != NULL && (int)axesT->count != nsl) ||
                (stepsT != NULL && (int)stepsT->count != nsl))
                IA_GB_FAIL("Slice : axes/pas incoherents");
            nd->op = IA_GOP_SLICE;
            nd->inputs[0] = s; nd->ninputs = 1;
            nd->naxes = nsl;
            for (int k = 0; k < nsl; k++) {
                nd->axes[k]     = (axesT  != NULL) ? (int)axesT->data[k]  : k;
                nd->sl_start[k] = (int)startsT->data[k];
                nd->sl_end[k]   = (int)endsT->data[k];
                nd->sl_step[k]  = (stepsT != NULL) ? (int)stepsT->data[k] : 1;
                if (nd->sl_step[k] <= 0)
                    IA_GB_FAIL("operation ONNX non supportee : Slice a pas negatif ou nul");
            }

        } else if (strcmp(op, "ReduceMean") == 0) {
            int s = ia_ns_get(map, nmap, n->inputs != NULL ? n->inputs[0] : NULL);
            if (s < 0) IA_GB_FAIL("graphe ONNX non topologique (ReduceMean)");
            const OnnxAttr* axA = onnx_node_attr(n, "axes");
            if (axA == NULL || axA->nints < 1)
                IA_GB_FAIL("operation ONNX non supportee : ReduceMean sans axes (reduction totale)");
            if (axA->nints > OWL_TENSOR_MAX_RANK)
                IA_GB_FAIL("ReduceMean : trop d'axes");
            const OnnxAttr* kdA = onnx_node_attr(n, "keepdims");
            nd->op = IA_GOP_REDUCEMEAN;
            nd->inputs[0] = s; nd->ninputs = 1;
            nd->naxes = axA->nints;
            for (int k = 0; k < axA->nints; k++) nd->axes[k] = (int)axA->ints[k];
            nd->keepdims = (kdA != NULL && kdA->has_i && kdA->i != 0);

        } else if (strcmp(op, "Constant") == 0) {
            /* Produit un Tenseur constant depuis son attribut `value` (TensorProto),
             * pose dans un slot de la table de valeurs. Sert de source aux facteurs
             * d'echelle du Resize (cou FPN), forme reelle de l'export torch. */
            const OnnxAttr* valA = onnx_node_attr(n, "value");
            if (valA == NULL || valA->t == NULL)
                IA_GB_FAIL("operation ONNX non supportee : Constant sans attribut value tensoriel");
            ObjTensor* ct = ia_onnx_const_to_tensor(valA->t);
            if (ct == NULL) IA_GB_FAIL("Constant : valeur tensorielle vide ou de forme incoherente");
            nd->op = IA_GOP_CONST;
            nd->layer.weights = ct;      /* trace GC via les poids du gnode */
            nd->ninputs = 0;

        } else if (strcmp(op, "Resize") == 0) {
            /* Perimetre : mode nearest, coordinate_transformation_mode asymmetric,
             * nearest_mode floor (cou FPN des detecteurs). Entrees [X, roi, scales] :
             * roi (input 1) ignore en asymmetric ; scales (input 2) = sortie d'un
             * Constant (ou un initializer), lu au forward pour les facteurs sy,sx. */
            const OnnxAttr* modeA = onnx_node_attr(n, "mode");
            const char* mode = (modeA != NULL && modeA->s != NULL) ? modeA->s : "nearest";
            if (strcmp(mode, "nearest") != 0) {
                snprintf(buf, buflen, "operation ONNX non supportee : Resize mode %s", mode);
                IA_GB_FAIL(buf);
            }
            if (n->ninputs < 3)
                IA_GB_FAIL("operation ONNX non supportee : Resize sans entree scales");
            int sX = ia_ns_get(map, nmap, n->inputs[0]);
            if (sX < 0) IA_GB_FAIL("graphe ONNX non topologique (Resize)");
            nd->op = IA_GOP_RESIZE;
            nd->inputs[0] = sX;          /* X (roi input 1 ignore en asymmetric) */
            /* scales : soit un INITIALIZER (facteurs figes, cas YOLOv8 ou le Constant
             * a ete replie a l'export) -> resolus ici et stockes dans split[] ; soit
             * la sortie d'un nœud CONSTANT -> lue au forward dans son slot. */
            const OnnxTensor* scInit = onnx_find_init(g, n->inputs[2]);
            if (scInit != NULL) {
                if (scInit->count < 2 || scInit->data == NULL)
                    IA_GB_FAIL("operation ONNX non supportee : Resize a facteurs d'echelle incomplets");
                int sy = (int)(scInit->data[scInit->count - 2] + 0.5);
                int sx = (int)(scInit->data[scInit->count - 1] + 0.5);
                if (sy < 1 || sx < 1)
                    IA_GB_FAIL("operation ONNX non supportee : Resize a facteurs d'echelle < 1 (sous-echantillonnage)");
                nd->split[0] = sy; nd->split[1] = sx;
                nd->ninputs = 1;         /* facteurs figes : pas de slot scales */
            } else {
                int sScales = ia_ns_get(map, nmap, n->inputs[2]);
                if (sScales < 0)
                    IA_GB_FAIL("operation ONNX non supportee : Resize a facteurs d'echelle dynamiques");
                nd->inputs[1] = sScales;
                nd->ninputs = 2;         /* scales lu au forward dans le slot Constant */
            }

        } else {
            snprintf(buf, buflen, "operation ONNX non supportee : %s", op);
            IA_GB_FAIL(buf);
        }

        /* Slots de sortie : un par sortie d'activation du nœud. */
        for (int o = 0; o < n->noutputs; o++) {
            if (n->outputs[o] == NULL) continue;
            if (nmap >= cap) IA_GB_FAIL("graphe ONNX trop grand (table de noms)");
            nd->outputs[nd->noutputs] = nextSlot;
            map[nmap].name = n->outputs[o]; map[nmap].slot = nextSlot; nmap++;
            nextSlot++;
            nd->noutputs++;
        }
        if (nd->noutputs < 1) IA_GB_FAIL("nœud ONNX sans sortie");
    }

    /* Sortie du graphe : le tenseur DÉCLARÉ (GraphProto.output[0]). C'est le
     * contrat ONNX — l'heuristique « dernier nœud terminal » est fragile (un
     * modèle mal ordonné ou à plusieurs tenseurs terminaux sortirait le mauvais
     * tenseur en silence). Fallback sur l'heuristique uniquement si le graphe
     * ne déclare pas de sortie mappable (modèle dégénéré) : un vrai .onnx
     * déclare toujours ses sorties. */
    if (g->output_name != NULL) {
        int s = ia_ns_get(map, nmap, g->output_name);
        if (s >= 0) m->graphOutSlot = s;
    }
    if (m->graphOutSlot < 0) {
        for (int i = 0; i < g->nnodes; i++) {
            for (int o = 0; o < g->nodes[i].noutputs; o++) {
                const char* nm = g->nodes[i].outputs[o];
                if (nm == NULL || ia_onnx_consumed(g, nm)) continue;
                int s = ia_ns_get(map, nmap, nm);
                if (s >= 0) m->graphOutSlot = s;
            }
        }
    }
    m->nvalues = nextSlot;
    free(map);
    #undef IA_GB_FAIL

    if (m->ngnodes < 1) return "graphe ONNX vide";
    if (m->graphOutSlot < 0) return "graphe ONNX sans sortie identifiable";
    return NULL;
}

/* Erreur recuperable d'import ONNX : leve ErreurDétectée sans crasher. */
static Value ia_charge_onnx_echec(const char* path, const char* detail) {
    owl_emit_native_error(E_NATIVE_IO_FAILURE, "IAMod\xc3\xa8leChargeONNX",
                          "native", "IAMod\xc3\xa8leChargeONNX",
                          "path", path, "detail", detail, NULL);
    return BOOL_VAL(false);
}

/* .ChargeONNX(chemin) / .LoadONNX + IAModèleChargeONNX / AIModelLoadONNX.
 * Lit un fichier `.onnx` (protobuf) et RECONSTRUIT les couches de `mon` avec
 * les poids importes. Rend Vrai en succes, Faux (ErreurDétectée) sinon —
 * fichier absent, protobuf malforme/hostile, ou op hors perimetre. Jamais de
 * crash. */
static Value fn_iamodele_charge_onnx(int argCount, Value* args) {
    (void)argCount;
    if (!IS_IAMODELE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IAMod\xc3\xa8leChargeONNX",
                              "arg", "modele",
                              "expected", "IAMod\xc3\xa8le",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IAMod\xc3\xa8leChargeONNX",
                              "arg", "chemin",
                              "expected", "chaine",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    ObjIAModele* m    = AS_IAMODELE(args[0]);
    const char*  path = AS_CSTRING(args[1]);

    /* Etat graphe remis a zero : un re-import sur le meme objet ne doit pas
     * garder un graphe (ou l'inverse) d'un import precedent. */
    m->ngnodes = 0;
    m->nvalues = 0;
    m->graphInSlot = -1;
    m->graphOutSlot = -1;
    m->inputH = 0;   /*  — reinit avant capture (re-import propre) */
    m->inputW = 0;

    OnnxModel* onnx = NULL;
    char perr[256];
    if (!onnx_parse_file(path, &onnx, perr, sizeof(perr))) {
        return ia_charge_onnx_echec(path, perr);
    }

    /*  — forme d'entree declaree (NCHW) : les deux derniers axes sont
     * H et W. Capturee avant onnx_model_free. 0 si non declaree ou dim
     * symbolique (batch dynamique) -> .Détecte refusera faute de S. */
    int capH = 0, capW = 0;
    if (onnx->graph.input_ndim >= 2) {
        int64_t h = onnx->graph.input_dims[onnx->graph.input_ndim - 2];
        int64_t w = onnx->graph.input_dims[onnx->graph.input_ndim - 1];
        if (h > 0) capH = (int)h;
        if (w > 0) capW = (int)w;
    }

    /* Chemin séquentiel d'abord (modèles chaîne + blocs à branches) : préserve à
     * l'identique le comportement et le transfert des modèles déjà supportés. Si
     * le graphe n'est pas représentable en séquentiel (fan-out réel type Split /
     * C2f), on bascule sur l'exécuteur de graphe général — un pur
     * fallback, jamais emprunté par les modèles que le séquentiel sait traiter. */
    char merr[256];
    char gerr[256];
    const char* fail = ia_onnx_map_graph(m, &onnx->graph, merr, sizeof(merr));
    if (fail != NULL) {
        /* Sur échec du graphe, on remonte SON message (plus informatif sur l'op
         * bloquante réelle) ; le séquentiel n'était qu'une tentative. gerr et merr
         * sont des piles locales vivantes jusqu'au return. */
        fail = ia_onnx_build_graph(m, &onnx->graph, gerr, sizeof(gerr));
    }
    /* Copie le message avant de liberer l'arbre ONNX : `fail` peut pointer dans
     * merr/gerr (pile locale, survit) mais jamais dans l'arbre ONNX. */
    char failcopy[256];
    if (fail != NULL) {
        snprintf(failcopy, sizeof(failcopy), "%s", fail);
    }
    onnx_model_free(onnx);

    if (fail != NULL) {
        m->count = 0;   /* modele laisse dans un etat vide coherent sur echec */
        m->ngnodes = 0;
        return ia_charge_onnx_echec(path, failcopy);
    }
    m->inputH = capH;   /*  — forme d'entree valide seulement sur import reussi */
    m->inputW = capW;
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * Détection d'objets (.Détecte / .Detect) — 
 *
 * Pipeline de detection en un appel au-dessus d'un modele importe (tete YOLO ou
 * compatible) : redimensionne l'image a la forme d'entree du modele, infere,
 * decode la sortie [1, 4+classes, N], filtre par score, applique la suppression
 * non-maximale PAR CLASSE (helper partage avec ), puis remet les boites
 * a l'echelle de l'image d'origine. Rend un tableau de Détection trie par score
 * decroissant.
 * ================================================================ */

/* Remplit out[3*H*W] (CHW, canaux R,G,B normalises [0,1]) depuis une OwlImage
 * valide W*H (RGBA8 HWC, alpha ignore). Meme convention que ImageVersTenseur. */
static void ia_image_fill_chw(const OwlImage* img, int W, int H, double* out) {
    const uint8_t* px = owl_image_pixels(img);
    if (px == NULL) return;
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                out[c * H * W + y * W + x] = (double)px[(y * W + x) * 4 + c] / 255.0;
}

/* Tri global des detections gardees par score decroissant (tie-break index
 * croissant). Le pointeur de scores est passe via un file-static (qsort n'a pas
 * de contexte utilisateur portable). */
static const double* g_ia_det_scores = NULL;
static int ia_det_cmp_desc(const void* pa, const void* pb) {
    int ia = *(const int*)pa, ib = *(const int*)pb;
    double sa = g_ia_det_scores[ia], sb = g_ia_det_scores[ib];
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return ia < ib ? -1 : 1;
}

static Value fn_iamodele_detecte(int argCount, Value* args) {
    (void)argCount;
    ObjIAModele* m = AS_IAMODELE(args[0]);

    OwlImage* img = owl_image_handle_from_value(args[1]);
    if (img == NULL || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "D\xc3\xa9tecte",
                              "arg", "image",
                              "expected", "Image valide",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    if (!IS_NUMBER(args[2]) || !IS_NUMBER(args[3])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "D\xc3\xa9tecte",
                              "arg", "seuil",
                              "expected", "deux reels (seuilScore, seuilIoU)",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    double seuilScore = AS_NUMBER(args[2]);
    double seuilIoU   = AS_NUMBER(args[3]);

    int S  = m->inputH;
    int SW = m->inputW;
    if (S <= 0 || SW <= 0) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "D\xc3\xa9tecte",
                              "raison", "modele sans forme d'entree declaree "
                              "(importer un ONNX dont l'entree est typee NCHW)",
                              NULL);
        return NIL_VAL;
    }

    int Worig = owl_image_width(img);
    int Horig = owl_image_height(img);

    /* Copie redimensionnee (SW*S) : ne mutons pas l'image de l'utilisateur.
     * Etirement (stretch) — letterbox = raffinement futur. */
    OwlImage* resized = owl_image_new_from_rgba(Worig, Horig, owl_image_pixels(img));
    if (resized == NULL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "D\xc3\xa9tecte", NULL);
        return NIL_VAL;
    }
    owl_image_resize_to(resized, SW, S, OWL_RESIZE_ETIRE);

    /* Tenseur d'entree [1, 3, S, SW]. obj_new_tensor_nd se root pendant son
     * alloc ; on repush pour proteger `input` pendant le forward (qui alloue). */
    int inShape[4] = { 1, 3, S, SW };
    bool savedNoGrad = g_no_grad;
    g_no_grad = true;
    ObjTensor* input = obj_new_tensor_nd(4, inShape);
    vm_push(OBJ_VAL(input));
    ia_image_fill_chw(resized, SW, S, tensor_cells(input->data));
    owl_image_free(resized);

    Value outV = (m->ngnodes > 0) ? ia_graph_forward(m, input)
                                  : ia_infere_forward(m, input);
    g_no_grad = savedNoGrad;
    vm_pop();   /* input */

    if (!IS_TENSOR(outV)) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "D\xc3\xa9tecte",
                              "raison", "l'inference n'a pas produit de tenseur",
                              NULL);
        return NIL_VAL;
    }
    vm_push(outV);   /* root la sortie pendant le decodage + les allocations */
    ObjTensor* out = AS_TENSOR(outV);

    /* Sortie attendue [1, C, N] (ou [C, N]) : C = 4 (boite) + classes, N candidats. */
    int C, N;
    if (out->ndim == 3 && out->shape[0] == 1) {
        C = out->shape[1]; N = out->shape[2];
    } else if (out->ndim == 2) {
        C = out->shape[0]; N = out->shape[1];
    } else {
        vm_pop();
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "D\xc3\xa9tecte",
                              "raison", "sortie de forme inattendue "
                              "(attendu [1, 4+classes, N])", NULL);
        return NIL_VAL;
    }
    if (C < 5) {
        vm_pop();
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "D\xc3\xa9tecte",
                              "raison", "sortie sans canaux de classe "
                              "(attendu au moins 4 coords + 1 classe)", NULL);
        return NIL_VAL;
    }
    int numClasses = C - 4;
    const double* cells = tensor_cells(out->data);

    /* Facteurs de remise a l'echelle vers l'image d'origine (axes independants :
     * un resize par etirement peut avoir des facteurs X et Y differents). */
    double fX = (double)Worig / (double)SW;
    double fY = (double)Horig / (double)S;

    int cap = (N > 0) ? N : 1;
    double* boxbuf = (double*)malloc((size_t)cap * 4 * sizeof(double));
    double* scbuf  = (double*)malloc((size_t)cap * sizeof(double));
    int*    clsbuf = (int*)malloc((size_t)cap * sizeof(int));
    int*    cand   = (int*)malloc((size_t)cap * sizeof(int));
    int*    kept   = (int*)malloc((size_t)cap * sizeof(int));
    if (!boxbuf || !scbuf || !clsbuf || !cand || !kept) {
        free(boxbuf); free(scbuf); free(clsbuf); free(cand); free(kept);
        vm_pop();
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "D\xc3\xa9tecte", NULL);
        return NIL_VAL;
    }

    /* Decode + filtre par score : boites en ESPACE MODELE [x, y, largeur, hauteur]
     * (x = cx - w/2, y = cy - h/2). out[0][r][i] = cells[r*N + i]. */
    int ncand = 0;
    for (int i = 0; i < N; i++) {
        double cx = cells[0 * N + i];
        double cy = cells[1 * N + i];
        double w  = cells[2 * N + i];
        double h  = cells[3 * N + i];
        int best = 0;
        double bestScore = cells[4 * N + i];
        for (int c = 1; c < numClasses; c++) {
            double sc = cells[(4 + c) * N + i];
            if (sc > bestScore) { bestScore = sc; best = c; }
        }
        if (bestScore > seuilScore) {
            boxbuf[ncand * 4 + 0] = cx - w / 2.0;
            boxbuf[ncand * 4 + 1] = cy - h / 2.0;
            boxbuf[ncand * 4 + 2] = w;
            boxbuf[ncand * 4 + 3] = h;
            scbuf[ncand]  = bestScore;
            clsbuf[ncand] = best;
            ncand++;
        }
    }

    /* NMS PAR CLASSE (espace modele) : pour chaque classe, NMS gloutonne sur ses
     * candidats ; on accumule les gardes (helper partage avec SuppressionNonMax). */
    int nAllKept = 0;
    for (int cls = 0; cls < numClasses; cls++) {
        int nc = 0;
        for (int i = 0; i < ncand; i++)
            if (clsbuf[i] == cls) cand[nc++] = i;
        if (nc == 0) continue;
        nAllKept += owl_det_nms_greedy(boxbuf, scbuf, cand, nc, seuilIoU,
                                       kept + nAllKept);
    }

    /* Tri global des gardes par score decroissant. */
    g_ia_det_scores = scbuf;
    qsort(kept, (size_t)nAllKept, sizeof(int), ia_det_cmp_desc);
    g_ia_det_scores = NULL;

    /* Construit le tableau de Détection (boites remises a l'echelle image). */
    ObjArray* result = obj_new_array();
    vm_push(OBJ_VAL(result));
    for (int j = 0; j < nAllKept; j++) {
        int idx = kept[j];
        ObjArray* box = obj_new_array();
        vm_push(OBJ_VAL(box));
        obj_array_push(box, REAL_VAL(boxbuf[idx * 4 + 0] * fX));
        obj_array_push(box, REAL_VAL(boxbuf[idx * 4 + 1] * fY));
        obj_array_push(box, REAL_VAL(boxbuf[idx * 4 + 2] * fX));
        obj_array_push(box, REAL_VAL(boxbuf[idx * 4 + 3] * fY));
        ObjDetection* det = obj_new_detection(OBJ_VAL(box), clsbuf[idx], scbuf[idx]);
        vm_pop();               /* box (desormais reference par det->box) */
        vm_push(OBJ_VAL(det));  /* root det pendant le push resultat */
        obj_array_push(result, OBJ_VAL(det));
        vm_pop();               /* det */
    }
    vm_pop();   /* result */

    free(boxbuf); free(scbuf); free(clsbuf); free(cand); free(kept);
    vm_pop();   /* outV */
    owl_clear_error();
    return OBJ_VAL(result);
}

/* ================================================================
 * Export ONNX (.SauveONNX / .SaveONNX) — , lot 2
 *
 * Inverse EXACT du mapping d'import : chaque couche IAModèle devient un ou
 * plusieurs nodes ONNX, ses tenseurs deviennent des initializers. Les tenseurs
 * du graphe sont nommés pour chaîner (sortie d'un node = entrée du suivant) et
 * pour rematcher le motif à branches à la ré-import (un node Add dont les
 * entrées sont les sorties des branches divergeant du tenseur capturé au DÉBUT).
 * Le writer protobuf (onnx_export.c) fait le reste. Lecture seule des Tenseurs
 * OWL : aucune allocation OWL, donc aucun rooting GC nécessaire ici (le modèle
 * `m` reste rooté par le dispatch).
 * ================================================================ */

#define IA_ONNX_EXPORT_MAX_RES 64

/*  — contexte d'un bloc a branches ouvert pendant l'export : le tenseur
 * de divergence x (fork) et les sorties deja closes de ses branches. Une branche
 * vide re-emet directement le fork (identite), donc le round-trip d'un residuel
 * identite redonne bien Add(F(x), x). */
typedef struct {
    char fork[32];
    char outs[IA_MAX_BRANCHES][32];
    int  nouts;
} IaExportBranchCtx;

/* Traduit les couches de `m` en graphe ONNX construit dans `e`. Rend NULL en
 * succès, sinon un message d'erreur (peut pointer dans `buf`). */
static const char* ia_onnx_map_export(ObjIAModele* m, OnnxExport* e,
                                      char* buf, size_t buflen) {
    char cur[32];
    snprintf(cur, sizeof(cur), "input");
    int tcnt = 0;   /* compteur de tenseurs intermediaires (sorties de nodes) */
    int icnt = 0;   /* compteur d'initializers (noms de poids/biais/stats) */

    /* Pile des blocs a branches ouverts (fork + sorties de branches closes). */
    IaExportBranchCtx blocks[IA_ONNX_EXPORT_MAX_RES];
    int  sp = 0;

    for (int li = 0; li < m->count; li++) {
        IALayer* L = &m->layers[li];

        if (L->kind == IA_LAYER_BRANCH_BEGIN) {
            /* Le tenseur de divergence x = le tenseur courant (produit par le
             * node precedent). La 1re branche repart de x. */
            if (sp >= IA_ONNX_EXPORT_MAX_RES)
                return "trop de blocs a branches imbriques (hors perimetre)";
            snprintf(blocks[sp].fork, sizeof(blocks[sp].fork), "%s", cur);
            blocks[sp].nouts = 0;
            sp++;
            /* cur reste = fork : la 1re branche part de x (no-op explicite). */
            continue;
        }
        if (L->kind == IA_LAYER_BRANCH_NEXT) {
            if (sp == 0) return "branche suivante hors d'un bloc ouvert";
            IaExportBranchCtx* ctx = &blocks[sp - 1];
            if (ctx->nouts >= IA_MAX_BRANCHES)
                return "trop de branches (hors perimetre)";
            snprintf(ctx->outs[ctx->nouts], sizeof(ctx->outs[ctx->nouts]), "%s", cur);
            ctx->nouts++;
            /* La branche suivante repart du fork. */
            snprintf(cur, sizeof(cur), "%s", ctx->fork);
            continue;
        }
        if (L->kind == IA_LAYER_MERGE_ADD) {
            if (sp == 0) return "fusion hors d'un bloc ouvert";
            IaExportBranchCtx* ctx = &blocks[sp - 1];
            if (ctx->nouts >= IA_MAX_BRANCHES)
                return "trop de branches (hors perimetre)";
            /* Cloture de la derniere branche. */
            snprintf(ctx->outs[ctx->nouts], sizeof(ctx->outs[ctx->nouts]), "%s", cur);
            ctx->nouts++;
            /* Fusion par additions binaires enchainees, dans l'ordre des
             * branches : acc = out0, puis acc = Add(acc, outk). */
            char acc[32];
            snprintf(acc, sizeof(acc), "%s", ctx->outs[0]);
            for (int k = 1; k < ctx->nouts; k++) {
                OnnxExportNode* n = onnx_export_add_node(e, "Add");
                onnx_node_add_input(n, acc);
                onnx_node_add_input(n, ctx->outs[k]);
                snprintf(acc, sizeof(acc), "t%d", tcnt++);
                onnx_node_add_output(n, acc);
            }
            snprintf(cur, sizeof(cur), "%s", acc);
            sp--;
            continue;
        }
        if (L->kind == IA_LAYER_MERGE_CONCAT) {
            /*  — fusion par concatenation canal : un seul node Concat
             * (axis=1) prenant toutes les sorties de branches, dans l'ordre des
             * branches (= ordre des canaux). Inverse exact du mapping d'import. */
            if (sp == 0) return "fusion hors d'un bloc ouvert";
            IaExportBranchCtx* ctx = &blocks[sp - 1];
            if (ctx->nouts >= IA_MAX_BRANCHES)
                return "trop de branches (hors perimetre)";
            snprintf(ctx->outs[ctx->nouts], sizeof(ctx->outs[ctx->nouts]), "%s", cur);
            ctx->nouts++;
            OnnxExportNode* n = onnx_export_add_node(e, "Concat");
            for (int k = 0; k < ctx->nouts; k++)
                onnx_node_add_input(n, ctx->outs[k]);
            onnx_node_attr_int(n, "axis", 1);
            char cc[32];
            snprintf(cc, sizeof(cc), "t%d", tcnt++);
            onnx_node_add_output(n, cc);
            snprintf(cur, sizeof(cur), "%s", cc);
            sp--;
            continue;
        }

        switch (L->kind) {
            case IA_LAYER_CONV2D: {
                ObjTensor* W = L->weights;
                if (W->ndim != 4) return "poids Conv de rang != 4 non exportables";
                int Cout = W->shape[0];
                int64_t wdims[4] = { W->shape[0], W->shape[1], W->shape[2], W->shape[3] };
                char wn[32], bn[32];
                snprintf(wn, sizeof(wn), "w%d", icnt++);
                snprintf(bn, sizeof(bn), "w%d", icnt++);
                onnx_export_add_initializer(e, wn, wdims, 4,
                                            tensor_cells(W->data),
                                            tensor_numel(W->data));
                int64_t bdims[1] = { Cout };
                onnx_export_add_initializer(e, bn, bdims, 1,
                                            tensor_cells(L->bias->data),
                                            tensor_numel(L->bias->data));
                OnnxExportNode* n = onnx_export_add_node(e, "Conv");
                onnx_node_add_input(n, cur);
                onnx_node_add_input(n, wn);
                onnx_node_add_input(n, bn);
                int64_t ks[2] = { W->shape[2], W->shape[3] };
                int64_t st[2] = { L->stride, L->stride };
                int64_t pd[4] = { L->pad, L->pad, L->pad, L->pad };
                onnx_node_attr_ints(n, "kernel_shape", ks, 2);
                onnx_node_attr_ints(n, "strides", st, 2);
                onnx_node_attr_ints(n, "pads", pd, 4);
                snprintf(cur, sizeof(cur), "t%d", tcnt++);
                onnx_node_add_output(n, cur);
                break;
            }
            case IA_LAYER_DENSE: {
                ObjTensor* W = L->weights;
                if (W->ndim != 2) return "poids Dense de rang != 2 non exportables";
                int in  = W->shape[0];
                int out = W->shape[1];
                double* wd = tensor_cells(W->data);   /* row-major [in][out] */
                /* Gemm import (transB=1) lit B de forme [out, in] avec
                 * W_owl[i][o] = B[o][i]. On emet donc B transpose. */
                double* B = malloc((size_t)in * out * sizeof(double));
                if (B == NULL) return "memoire insuffisante (export Dense)";
                for (int o = 0; o < out; o++)
                    for (int i = 0; i < in; i++)
                        B[(int64_t)o * in + i] = wd[(int64_t)i * out + o];
                char wn[32], bn[32];
                snprintf(wn, sizeof(wn), "w%d", icnt++);
                snprintf(bn, sizeof(bn), "w%d", icnt++);
                int64_t wdims[2] = { out, in };
                onnx_export_add_initializer(e, wn, wdims, 2, B, (int64_t)in * out);
                free(B);
                int64_t bdims[1] = { out };
                onnx_export_add_initializer(e, bn, bdims, 1,
                                            tensor_cells(L->bias->data),
                                            tensor_numel(L->bias->data));
                OnnxExportNode* n = onnx_export_add_node(e, "Gemm");
                onnx_node_add_input(n, cur);
                onnx_node_add_input(n, wn);
                onnx_node_add_input(n, bn);
                onnx_node_attr_int(n, "transB", 1);
                snprintf(cur, sizeof(cur), "t%d", tcnt++);
                onnx_node_add_output(n, cur);
                break;
            }
            case IA_LAYER_BATCHNORM: {
                int C = tensor_numel(L->gamma->data);
                int64_t d[1] = { C };
                char gn[32], bn[32], mn[32], vn[32];
                snprintf(gn, sizeof(gn), "w%d", icnt++);
                snprintf(bn, sizeof(bn), "w%d", icnt++);
                snprintf(mn, sizeof(mn), "w%d", icnt++);
                snprintf(vn, sizeof(vn), "w%d", icnt++);
                onnx_export_add_initializer(e, gn, d, 1, tensor_cells(L->gamma->data), C);
                onnx_export_add_initializer(e, bn, d, 1, tensor_cells(L->beta->data), C);
                onnx_export_add_initializer(e, mn, d, 1, tensor_cells(L->running_mean->data), C);
                onnx_export_add_initializer(e, vn, d, 1, tensor_cells(L->running_var->data), C);
                OnnxExportNode* n = onnx_export_add_node(e, "BatchNormalization");
                onnx_node_add_input(n, cur);
                onnx_node_add_input(n, gn);
                onnx_node_add_input(n, bn);
                onnx_node_add_input(n, mn);
                onnx_node_add_input(n, vn);
                onnx_node_attr_float(n, "epsilon", L->eps);
                snprintf(cur, sizeof(cur), "t%d", tcnt++);
                onnx_node_add_output(n, cur);
                break;
            }
            case IA_LAYER_RELU: {
                OnnxExportNode* n = onnx_export_add_node(e, "Relu");
                onnx_node_add_input(n, cur);
                snprintf(cur, sizeof(cur), "t%d", tcnt++);
                onnx_node_add_output(n, cur);
                break;
            }
            case IA_LAYER_SIGMOID: {
                OnnxExportNode* n = onnx_export_add_node(e, "Sigmoid");
                onnx_node_add_input(n, cur);
                snprintf(cur, sizeof(cur), "t%d", tcnt++);
                onnx_node_add_output(n, cur);
                break;
            }
            case IA_LAYER_TANH: {
                OnnxExportNode* n = onnx_export_add_node(e, "Tanh");
                onnx_node_add_input(n, cur);
                snprintf(cur, sizeof(cur), "t%d", tcnt++);
                onnx_node_add_output(n, cur);
                break;
            }
            case IA_LAYER_MAXPOOL:
            case IA_LAYER_AVGPOOL: {
                if (L->kind == IA_LAYER_AVGPOOL && L->window <= 0) {
                    /* Fenetre globale (window == 0) -> GlobalAveragePool. */
                    OnnxExportNode* n = onnx_export_add_node(e, "GlobalAveragePool");
                    onnx_node_add_input(n, cur);
                    snprintf(cur, sizeof(cur), "t%d", tcnt++);
                    onnx_node_add_output(n, cur);
                } else {
                    const char* op = (L->kind == IA_LAYER_MAXPOOL)
                                     ? "MaxPool" : "AveragePool";
                    OnnxExportNode* n = onnx_export_add_node(e, op);
                    onnx_node_add_input(n, cur);
                    int64_t stv = L->stride > 0 ? L->stride : L->window;
                    int64_t ks[2] = { L->window, L->window };
                    int64_t st[2] = { stv, stv };
                    int64_t pd[4] = { L->pad, L->pad, L->pad, L->pad };
                    onnx_node_attr_ints(n, "kernel_shape", ks, 2);
                    onnx_node_attr_ints(n, "strides", st, 2);
                    onnx_node_attr_ints(n, "pads", pd, 4);
                    snprintf(cur, sizeof(cur), "t%d", tcnt++);
                    onnx_node_add_output(n, cur);
                }
                break;
            }
            case IA_LAYER_FLATTEN: {
                OnnxExportNode* n = onnx_export_add_node(e, "Flatten");
                onnx_node_add_input(n, cur);
                onnx_node_attr_int(n, "axis", 1);
                snprintf(cur, sizeof(cur), "t%d", tcnt++);
                onnx_node_add_output(n, cur);
                break;
            }
            default:
                snprintf(buf, buflen, "couche non exportable vers ONNX");
                return buf;
        }
    }

    if (sp != 0) return "bloc a branches ouvert jamais fusionne";

    /* Formes des ValueInfo I/O — requises par onnx.checker. Le rang se deduit de
     * la 1re couche a poids ; les dimensions absentes de l'IAModele (lot N, et
     * spatiales H/W pour la conv) sont emises symboliques. */
    OnnxDim in_dims[4];
    int in_ndim = 0;
    for (int li = 0; li < m->count; li++) {
        IALayer* L = &m->layers[li];
        if (L->kind == IA_LAYER_CONV2D && L->weights->ndim == 4) {
            in_dims[0] = (OnnxDim){ .param = "N" };
            in_dims[1] = (OnnxDim){ .value = L->weights->shape[1] };   /* Cin */
            in_dims[2] = (OnnxDim){ .param = "H" };
            in_dims[3] = (OnnxDim){ .param = "W" };
            in_ndim = 4;
            break;
        }
        if (L->kind == IA_LAYER_DENSE && L->weights->ndim == 2) {
            in_dims[0] = (OnnxDim){ .param = "N" };
            in_dims[1] = (OnnxDim){ .value = L->weights->shape[0] };   /* entree */
            in_ndim = 2;
            break;
        }
    }

    OnnxDim out_dims[2];
    int out_ndim = 0;
    for (int li = m->count - 1; li >= 0; li--) {
        IALayer* L = &m->layers[li];
        if (L->kind == IA_LAYER_DENSE && L->weights->ndim == 2) {
            out_dims[0] = (OnnxDim){ .param = "N" };
            out_dims[1] = (OnnxDim){ .value = L->weights->shape[1] };  /* classes */
            out_ndim = 2;
            break;
        }
    }

    onnx_export_set_io(e, "input", in_dims, in_ndim, cur, out_dims, out_ndim);
    return NULL;
}

/* Erreur recuperable d'export ONNX : leve ErreurDétectée sans crasher. */
static Value ia_sauve_onnx_echec(const char* path, const char* detail) {
    owl_emit_native_error(E_NATIVE_IO_FAILURE, "IAMod\xc3\xa8leSauveONNX",
                          "native", "IAMod\xc3\xa8leSauveONNX",
                          "path", path, "detail", detail, NULL);
    return BOOL_VAL(false);
}

/* .SauveONNX(chemin) / .SaveONNX + IAModèleSauveONNX / AIModelSaveONNX.
 * Serialise les couches de `mon` (et leurs poids) dans un fichier `.onnx`
 * (protobuf). Rend Vrai en succes, Faux (ErreurDétectée) sinon — chemin
 * invalide, couche hors perimetre, ou echec d'ecriture. */
static Value fn_iamodele_sauve_onnx(int argCount, Value* args) {
    (void)argCount;
    if (!IS_IAMODELE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IAMod\xc3\xa8leSauveONNX",
                              "arg", "modele",
                              "expected", "IAMod\xc3\xa8le",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IAMod\xc3\xa8leSauveONNX",
                              "arg", "chemin",
                              "expected", "chaine",
                              "actual", "type non pris en charge", NULL);
        return BOOL_VAL(false);
    }
    ObjIAModele* m    = AS_IAMODELE(args[0]);
    const char*  path = AS_CSTRING(args[1]);

    OnnxExport* e = onnx_export_new();
    if (e == NULL) return ia_sauve_onnx_echec(path, "memoire insuffisante");

    char merr[256];
    const char* fail = ia_onnx_map_export(m, e, merr, sizeof(merr));
    if (fail != NULL) {
        char failcopy[256];
        snprintf(failcopy, sizeof(failcopy), "%s", fail);
        onnx_export_free(e);
        return ia_sauve_onnx_echec(path, failcopy);
    }

    char werr[256];
    bool ok = onnx_export_write(e, path, werr, sizeof(werr));
    onnx_export_free(e);
    if (!ok) return ia_sauve_onnx_echec(path, werr);

    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * Registry
 * ================================================================ */

static const char* const ALIAS_iamodele[] = { "AIModel", NULL };
static const char* const ALIAS_options[]  = { "TrainingOptions", NULL };
static const char* const ALIAS_detection[] = { "Detection", NULL };

/*  — Détection : proprietes en LECTURE SEULE, renseignees par
 * IAModèle.Détecte. Le stockage reel vit dans ObjDetection ; la lecture
 * double-point est cablee dans vm.c (OP_STRUCT_GET). Ces entrees publient la
 * paire bilingue FR/EN pour l'explain LSP et `owl translate`. */
static const OwlField FIELDS_Detection[] = {
    {"Boite", "Bo\xc3\xaete", "tableau", "",
     "boite englobante [x, y, largeur, hauteur] en pixels de l'image d'origine "
     "(coin haut-gauche puis taille)",
     "bounding box [x, y, width, height] in pixels of the original image "
     "(top-left corner then size)",
     .nameEN="Box"},
    {"Classe", NULL, "entier", "0",
     "index de classe predite (0-based)",
     "predicted class index (0-based)",
     .nameEN="Class"},
    {"Score", NULL, "r\xc3\xa9""el", "0",
     "score de confiance dans [0,1]",
     "confidence score in [0,1]",
     .nameEN="Score"},
    {0},
};

/* Champs declaratifs. Le STOCKAGE reel vit dans ObjIAModele/
 * ObjOptionsEntrainement (objets natifs) et la lecture/ecriture double-point
 * est cablee dans vm.c (OP_STRUCT_GET/SET). Ces entrees ne servent donc PAS de
 * template runtime : elles publient la paire bilingue FR/EN de chaque propriete
 * pour l'explain LSP et pour `owl translate` (find_property_norm cote
 * formatter). Sans elles, la traduction laissait `opts..Époques` en FR au
 * milieu d'un code EN.
 *
 * IAModèle N'A PAS de tel bloc : ses membres single-dot sont des methodes
 * fluentes d'extension (`.AjouteCoucheDense`...). Declarer `fields` sur ce type
 * activerait la validation compile-time des membres, qui rejetterait alors les
 * methodes ("Membre 'AddDenseLayer' inexistant"). Sa seule prop double-point
 * ..NbCouches reste donc non traduite — trou connu, hors des 5 props ciblees. */
/*  — plans de decroissance du taux (valeurs de ..PlanTaux). Enum
 * stricte : le champ n'accepte qu'un de ses membres (validation compile-time
 * + filtre LSP), comme les autres enums d'options. Les membres sont des
 * OwlConstDef (PLAN_CONSTS) ; l'enum ne stocke que les references de noms. */
static const char* const E_planTaux_values[] = {
    "planConstant", "planCosinus", NULL
};
static const OwlEnum E_planTaux = {
    .name="planTaux", .nameAscii="planTaux", .nameEN="LRSchedule",
    .category="Intelligence artificielle",
    .descFR="Plan de decroissance du taux d'apprentissage : planConstant (taux "
            "fixe) ou planCosinus (recuit cosinus).",
    .descEN="Learning-rate decay schedule: planConstant (fixed rate) or "
            "planCosinus (cosine annealing).",
    .values=E_planTaux_values, .combinable=false,
};

static const OwlField FIELDS_OptionsEntrainement[] = {
    {"Epoques",           "Époques", "entier", "1",  "nombre de passes complètes sur les données d'entraînement", "number of full passes over the training data", .nameEN="Epochs"},
    {"TailleLot",         NULL,      "entier", "32", "taille des mini-lots", "mini-batch size", .nameEN="BatchSize"},
    {"TauxApprentissage", NULL,      "réel",   "0.01", "pas de la mise à jour des poids", "weight update step", .nameEN="LearningRate"},
    {"Optimiseur",        NULL,      "entier", "optimSGD", "stratégie d'optimisation (optimSGD ou optimAdam)", "optimization strategy (optimSGD or optimAdam)", .nameEN="Optimizer"},
    {"Perte",             NULL,      "entier", "perteEntropieCrois\xc3\xa9""e", "fonction de perte (perteEntropieCroisée ou perteErreurQuadratique)", "loss function (lossCrossEntropy or lossQuadraticError)", .nameEN="Loss"},
    {"PlanTaux",          NULL,      "entier", "planConstant", "plan de décroissance du taux au fil des époques (planConstant pour un taux fixe, planCosinus pour un recuit cosinus)", "learning-rate decay schedule across epochs (planConstant for a fixed rate, planCosinus for cosine annealing)", .allowedBase=&E_planTaux, .nameEN="LRSchedule"},
    {"DecroissancePoids", "D\xc3\xa9""croissancePoids", "réel", "0.0", "coefficient de régularisation L2 (pénalité sur l'amplitude des poids) ajouté à la mise à jour ; 0.0 désactive", "L2 regularization coefficient (penalty on weight magnitude) added to the update; 0.0 disables it", .nameEN="WeightDecay"},
    {"FractionValidation", NULL, "réel", "0.0", "fraction du jeu d'entraînement réservée à la validation (les derniers échantillons, ex. 0.1 = 10 %) ; 0.0 désactive", "share of the training set held out for validation (the last samples, e.g. 0.1 = 10%); 0.0 disables it", .nameEN="ValidationFraction"},
    {"GardeMeilleurModele", "GardeMeilleurMod\xc3\xa8le", "bool\xc3\xa9""en", "Faux", "à la fin, garde les poids de l'époque où la perte de validation était la plus basse au lieu de ceux de la dernière époque", "at the end, keeps the weights from the epoch with the lowest validation loss instead of the last epoch's", .nameEN="KeepBestModel"},
    {"PatienceArret",     "PatienceArr\xc3\xaat", "entier", "0", "arrête l'entraînement quand la perte de validation ne progresse plus pendant ce nombre d'époques consécutives ; 0 désactive", "stops training when validation loss stops improving for this many consecutive epochs; 0 disables it", .nameEN="StopPatience"},
    {"SurEpoque",         "Sur\xc3\x89poque", "proc\xc3\xa9""dure", "", "procédure rappelée à la fin de chaque époque, reçoit (numéro d'époque, nombre total d'époques, perte moyenne de l'époque)", "procedure called at the end of each epoch, receives (epoch number, total epochs, average epoch loss)", .nameEN="OnEpoch", .callbackSig="entier,entier,r\xc3\xa9""el"},
    {0},
};

static const OwlEnum* const IAMODELE_ENUMS[] = { &E_planTaux };

static const OwlTypeDef IAMODELE_TYPES[] = {
    { .canonical      = "IAMod\xc3\xa8le",       /* IAModèle */
      .canonicalAscii = "IAModele",
      .aliases        = ALIAS_iamodele,
      .category       = "Intelligence artificielle",
      .descFR         = "Reseau de neurones construit en empilant des couches "
                        "fluentes : `.AjouteCoucheDense(entrees, sorties)` pour "
                        "une couche dense (poids initialises a la creation, "
                        "initHe par defaut ou initXavier), "
                        "`.AjouteCoucheReLU()`, `.AjouteCoucheSigmoïde()`, "
                        "`.AjouteCoucheTanh()` pour les activations. `.Infère(x)` "
                        "fait passer une entree (un echantillon 1D ou un lot 2D "
                        "B*entrees) a travers toutes les couches, dans l'ordre "
                        "d'ajout, et rend la sortie de meme rang. Branches "
                        "paralleles : `.DébutBranches()` capture l'activation "
                        "courante x, `.BrancheSuivante()` rouvre une branche "
                        "repartant du meme x, `.FusionneParAddition()` additionne "
                        "les sorties de toutes les branches (une branche vide vaut "
                        "l'identite -> un residuel s'ecrit une branche F(x) + une "
                        "branche vide, sortie = F(x) + x) et "
                        "`.FusionneParConcaténation()` les met bout a bout sur l'axe "
                        "des canaux (sortie [N, C1+C2+..., H, W] ; memes N, H, W "
                        "exiges). "
                        "Introspection : "
                        "`.Poids(n)` / `.Biais(n)` (n-ieme couche dense, base 1) "
                        "et `..NbCouches`. Pour charger un modele de langage "
                        "preentraine et generer du texte, voir `LLM`.",
      .descEN         = "Neural network built by stacking fluent layers: "
                        "`.AddDenseLayer(inputs, outputs)` for a dense layer "
                        "(weights initialized at creation, initHe by default or "
                        "initXavier), `.AddReLULayer()`, `.AddSigmoidLayer()`, "
                        "`.AddTanhLayer()` for activations. `.Infer(x)` runs an "
                        "input (a 1D sample or a 2D batch B*inputs) through every "
                        "layer, in insertion order, and returns an output of the "
                        "same rank. Parallel branches: `.BeginBranches()` captures "
                        "the current activation x, `.NextBranch()` reopens a branch "
                        "starting from the same x, `.MergeByAdd()` adds up the "
                        "outputs of all branches (an empty branch is the identity -> "
                        "a residual is written as one F(x) branch plus one empty "
                        "branch, output = F(x) + x) and `.MergeByConcat()` lays them "
                        "side by side along the channel axis (output [N, C1+C2+..., "
                        "H, W]; same N, H, W required). "
                        "Introspection: `.Weights(n)` / `.Bias(n)` "
                        "(n-th dense layer, 1-based) and `..LayerCount`. To load "
                        "a pretrained language model and generate text, see `LLM`.",
      .fields         = NULL,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "AIModel",
      .genre          = OWL_GENRE_M,          /* un IAModèle */
      .defaultFactoryNative = "__IAModeleNouveau" },

    { .canonical      = "OptionsEntra\xc3\xaenement",  /* OptionsEntraînement */
      .canonicalAscii = "OptionsEntrainement",
      .aliases        = ALIAS_options,
      .category       = "Intelligence artificielle",
      .descFR         = "Configuration d'un entrainement, passee a "
                        "`IAModèle.Entraîne(X, Y, options)`. Proprietes reglables "
                        "par double point : `..Époques` (nombre de passes sur les "
                        "donnees), `..TailleLot` (taille des mini-lots), "
                        "`..TauxApprentissage` (pas de la mise a jour des poids), "
                        "`..Optimiseur` (strategie, `optimSGD` ou `optimAdam`), "
                        "`..Perte` "
                        "(`perteEntropieCroisée` pour la classification, "
                        "`perteErreurQuadratique` pour la regression), "
                        "`..PlanTaux` (decroissance du taux, `planConstant` ou "
                        "`planCosinus`), `..DécroissancePoids` (regularisation "
                        "L2, 0.0 par defaut), `..FractionValidation` (part du jeu "
                        "reservee a la validation, 0.0 par defaut), "
                        "`..GardeMeilleurModèle` (garde les poids de la meilleure "
                        "epoque de validation, Faux par defaut) et `..PatienceArrêt` "
                        "(arret anticipe apres N epoques sans progres de la "
                        "validation, 0 par defaut). Defauts : "
                        "1 epoque, lot de 32, taux 0.01, optimSGD, entropie "
                        "croisee, taux constant, sans regularisation, sans "
                        "validation.",
      .descEN         = "Configuration of one training run, passed to "
                        "`AIModel.Train(X, Y, options)`. Properties set via the "
                        "double dot: `..Epochs` (number of passes over the data), "
                        "`..BatchSize` (mini-batch size), `..LearningRate` (weight "
                        "update step), `..Optimizer` (strategy, `optimSGD` or "
                        "`optimAdam`), "
                        "`..Loss` (`perteEntropieCroisée` for classification, "
                        "`perteErreurQuadratique` for regression), "
                        "`..LRSchedule` (rate decay, `planConstant` or "
                        "`planCosinus`), `..WeightDecay` (L2 regularization, "
                        "0.0 by default), `..ValidationFraction` (share of the "
                        "set held out for validation, 0.0 by default), "
                        "`..KeepBestModel` (keep the weights from the best "
                        "validation epoch, False by default) and `..StopPatience` "
                        "(early stop after N epochs with no validation "
                        "improvement, 0 by default). Defaults: 1 "
                        "epoch, batch of 32, rate 0.01, optimSGD, cross entropy, "
                        "constant rate, no regularization, no validation.",
      .fields         = FIELDS_OptionsEntrainement,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "TrainingOptions",
      .genre          = OWL_GENRE_M,          /* un OptionsEntraînement */
      .defaultFactoryNative = "__OptionsEntrainementNouveau" },

    { .canonical      = "D\xc3\xa9tection",   /* Détection */
      .canonicalAscii = "Detection",
      .aliases        = ALIAS_detection,
      .category       = "Intelligence artificielle",
      .descFR         = "Une detection d'objet rendue par `IAModèle.Détecte(image, "
                        "seuilScore, seuilIoU)`. Proprietes en lecture (double "
                        "point) : `..Boîte` (tableau de 4 reels [x, y, largeur, "
                        "hauteur] en pixels de l'image d'origine, coin haut-gauche "
                        "puis taille), `..Classe` (index de classe predite, "
                        "0-based) et `..Score` (confiance dans [0,1]). `.Détecte` "
                        "rend un tableau de Détection trie par score decroissant.",
      .descEN         = "An object detection returned by `AIModel.Detect(image, "
                        "scoreThreshold, iouThreshold)`. Read properties (double "
                        "dot): `..Box` (array of 4 reals [x, y, width, height] in "
                        "pixels of the original image, top-left corner then size), "
                        "`..Class` (predicted class index, 0-based) and `..Score` "
                        "(confidence in [0,1]). `.Detect` returns an array of "
                        "Detection sorted by decreasing score.",
      .fields         = FIELDS_Detection,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "Detection",
      .genre          = OWL_GENRE_F,          /* une Détection */
      .defaultFactoryNative = "__DetectionNouveau" },
};

/* nameEN identique au nom FR : noms propres (He, Xavier/Glorot) non traduits,
 * meme identifiant dans les deux langues — comme crypteAES128. */
#define C_INIT(nm, val, fr, en) { nm, "Intelligence artificielle", \
                                  INTEGER_VAL(val), fr, en, nm }
static const OwlConstDef INIT_CONSTS[] = {
    C_INIT("initHe", OWL_INIT_HE,
        "Initialisation des poids dite « He » : tirage centre d'ecart-type "
        "racine(2/entrees). Adaptee aux couches suivies d'une activation ReLU "
        "(compense la moitie du signal ecrasee). Strategie par defaut de "
        "AjouteCoucheDense.",
        "\"He\" weight initialization: centered draw with standard deviation "
        "sqrt(2/inputs). Suited to layers followed by a ReLU activation "
        "(compensates for the half of the signal it zeroes out). Default "
        "strategy of AddDenseLayer."),
    C_INIT("initXavier", OWL_INIT_XAVIER,
        "Initialisation des poids dite « Xavier » (ou Glorot) : tirage uniforme "
        "dans [-racine(6/(entrees+sorties)), +racine(6/(entrees+sorties))]. "
        "Adaptee aux couches suivies d'une activation tanh ou sigmoide.",
        "\"Xavier\" (or Glorot) weight initialization: uniform draw in "
        "[-sqrt(6/(inputs+outputs)), +sqrt(6/(inputs+outputs))]. Suited to "
        "layers followed by a tanh or sigmoid activation."),
};

/* Constantes d'entrainement : valeur de ..Optimiseur / ..Perte d'un
 * OptionsEntraînement. Meme identifiant FR/EN (termes du domaine utilises tels
 * quels, comme initHe). nameEN = nom canonique. */
#define C_TRAIN(nm, val, fr, en) { nm, "Intelligence artificielle", \
                                   INTEGER_VAL(val), fr, en, nm }
/* Variante avec nameEN explicite : les fonctions de perte sont des mots
 * descriptifs (pas des noms propres comme He/SGD/Adam) et ont donc une
 * forme anglaise canon distincte du nom FR. */
#define C_TRAIN_EN(nm, en_nm, val, fr, en) { nm, "Intelligence artificielle", \
                                   INTEGER_VAL(val), fr, en, en_nm }
static const OwlConstDef TRAIN_CONSTS[] = {
    C_TRAIN("optimSGD", OWL_OPTIM_SGD,
        "Optimiseur « descente de gradient stochastique » : a chaque mini-lot, "
        "chaque poids recule d'un pas proportionnel a son gradient "
        "(poids - tauxApprentissage * gradient), sans etat accumule.",
        "\"Stochastic gradient descent\" optimizer: on each mini-batch every "
        "weight steps back proportionally to its gradient "
        "(weight - learningRate * gradient), with no accumulated state."),
    C_TRAIN("optimAdam", OWL_OPTIM_ADAM,
        "Optimiseur « Adam » : par poids, garde deux moyennes mobiles du "
        "gradient (moment d'ordre 1) et de son carre (moment d'ordre 2), "
        "corrigees de leur biais de demarrage, pour un pas adapte a chaque "
        "poids. Converge souvent plus vite que optimSGD sans reglage fin du "
        "taux d'apprentissage. Etat interne remis a zero a chaque .Entraîne.",
        "\"Adam\" optimizer: per weight, keeps two running averages of the "
        "gradient (first moment) and its square (second moment), corrected for "
        "their start-up bias, giving a step tuned to each weight. Often "
        "converges faster than optimSGD without fine-tuning the learning rate. "
        "Internal state reset at each .Train."),
    C_TRAIN_EN("perteEntropieCrois\xc3\xa9" "e", "lossCrossEntropy",
        OWL_PERTE_ENTROPIE_CROISEE,
        "Fonction de perte « entropie croisee » pour la classification "
        "multi-classe : la derniere couche sort des scores bruts (logits), la "
        "softmax est appliquee en interne. Cible attendue en encodage un-parmi-n.",
        "\"Cross entropy\" loss for multi-class classification: the last layer "
        "outputs raw scores (logits) and the softmax is applied internally. "
        "Target expected in one-hot encoding."),
    C_TRAIN_EN("perteErreurQuadratique", "lossQuadraticError",
        OWL_PERTE_ERREUR_QUADRATIQUE,
        "Fonction de perte « erreur quadratique moyenne » pour la regression : "
        "moyenne des carres des ecarts entre la sortie du modele et la cible.",
        "\"Mean squared error\" loss for regression: mean of the squared "
        "differences between the model output and the target."),
    C_TRAIN_EN("planConstant", "scheduleConstant", OWL_PLAN_CONSTANT,
        "Plan de taux « constant » : le taux d'apprentissage reste egal a "
        "..TauxApprentissage pendant toute la duree de l'entrainement "
        "(comportement par defaut).",
        "\"Constant\" schedule: the learning rate stays equal to "
        "..LearningRate for the whole training run (default behavior)."),
    C_TRAIN_EN("planCosinus", "scheduleCosine", OWL_PLAN_COSINUS,
        "Plan de taux « recuit cosinus » : le taux part de ..TauxApprentissage "
        "et decroit vers ~0 en suivant une demi-cosinus sur le total des "
        "epoques. Recalcule au debut de chaque epoque.",
        "\"Cosine annealing\" schedule: the rate starts at ..LearningRate and "
        "decays toward ~0 following a half-cosine over the total number of "
        "epochs. Recomputed at the start of each epoch."),
};
#undef C_TRAIN
#undef C_TRAIN_EN
#undef C_INIT

void stdlib_iamodele_init(void) {
    owl_registry_register_types(IAMODELE_TYPES,
        (int)(sizeof(IAMODELE_TYPES) / sizeof(IAMODELE_TYPES[0])));
    owl_registry_register_consts(INIT_CONSTS,
        (int)(sizeof(INIT_CONSTS) / sizeof(INIT_CONSTS[0])));
    owl_registry_register_consts(TRAIN_CONSTS,
        (int)(sizeof(TRAIN_CONSTS) / sizeof(TRAIN_CONSTS[0])));
    owl_registry_register_enums(IAMODELE_ENUMS,
        (int)(sizeof(IAMODELE_ENUMS) / sizeof(IAMODELE_ENUMS[0])));

    /* Factory de declaration `mon est un IAModèle` (instance fraiche). Interne :
     * emise par le compilateur, jamais appelee explicitement par l'utilisateur. */
    owl_define_native("__IAModeleNouveau", fn_iamodele_nouveau, 0);
    /* Factory de declaration `opts est un OptionsEntraînement` (defauts frais). */
    owl_define_native("__OptionsEntrainementNouveau",
                      fn_options_entrainement_nouveau, 0);
    /* Factory de declaration `d est un Détection` (instance neutre, ). */
    owl_define_native("__DetectionNouveau", fn_detection_nouveau, 0);

    /* Couches fluentes. AjouteCoucheDense : receveur + entrees + sorties
     * [+ init] -> arite variadique (3 ou 4 avec le receveur). Activations :
     * receveur seul (arite 1). */
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheDense",
                        fn_iamodele_ajoute_dense, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddDenseLayer",
                        fn_iamodele_ajoute_dense, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheReLU",
                        fn_iamodele_ajoute_relu, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddReLULayer",
                        fn_iamodele_ajoute_relu, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheSigmo\xc3\xaf" "de",
                        fn_iamodele_ajoute_sigmoide, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddSigmoidLayer",
                        fn_iamodele_ajoute_sigmoide, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheTanh",
                        fn_iamodele_ajoute_tanh, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddTanhLayer",
                        fn_iamodele_ajoute_tanh, 1);

    /* Couches spatiales (, , ). Convolution2D :
     * receveur + 5 params [+ group] (arite 6 ou 7 -> variadique) ; pooling :
     * receveur + taille [+ pas] (arite variadique) ; NormalisationLot :
     * receveur + canaux [+ epsilon] (arite variadique) ; Aplatissement :
     * receveur seul (arite 1). Bilingue FR/EN. */
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheConvolution2D",
                        fn_iamodele_ajoute_conv2d, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddConvolution2DLayer",
                        fn_iamodele_ajoute_conv2d, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheMaxPooling",
                        fn_iamodele_ajoute_maxpooling, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddMaxPoolingLayer",
                        fn_iamodele_ajoute_maxpooling, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheMoyennePooling",
                        fn_iamodele_ajoute_moyennepooling, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddAveragePoolingLayer",
                        fn_iamodele_ajoute_moyennepooling, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheNormalisationLot",
                        fn_iamodele_ajoute_normalisationlot, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddBatchNormLayer",
                        fn_iamodele_ajoute_normalisationlot, -1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AjouteCoucheAplatissement",
                        fn_iamodele_ajoute_aplatissement, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "AddFlattenLayer",
                        fn_iamodele_ajoute_aplatissement, 1);

    /* Bloc a branches paralleles : marqueurs debut/suivante/fusion,
     * receveur seul (arite 1). Bilingue DébutBranches / BeginBranches,
     * BrancheSuivante / NextBranch, FusionneParAddition / MergeByAdd. Un residuel
     * a la ResNet = une branche F(x) + une branche vide (identite). */
    owl_register_method(METHOD_TYPE_IAMODELE, "D\xc3\xa9""butBranches",
                        fn_iamodele_debut_branches, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "BeginBranches",
                        fn_iamodele_debut_branches, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "BrancheSuivante",
                        fn_iamodele_branche_suivante, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "NextBranch",
                        fn_iamodele_branche_suivante, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "FusionneParAddition",
                        fn_iamodele_fusionne_addition, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "MergeByAdd",
                        fn_iamodele_fusionne_addition, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "FusionneParConcat\xc3\xa9nation",
                        fn_iamodele_fusionne_concat, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "MergeByConcat",
                        fn_iamodele_fusionne_concat, 1);

    /*  — gel/degel du corps pour le transfert : receveur seul (arite 1).
     * Bilingue Gèle / Freeze, Dégèle / Unfreeze. */
    owl_register_method(METHOD_TYPE_IAMODELE, "G\xc3\xa8le",
                        fn_iamodele_gele, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "Freeze",
                        fn_iamodele_gele, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "D\xc3\xa9g\xc3\xa8le",
                        fn_iamodele_degele, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "Unfreeze",
                        fn_iamodele_degele, 1);

    /*  — retrait de la derniere couche pour l'idiome transfert :
     * receveur seul (arite 1). Bilingue RetireDernièreCouche / RemoveLastLayer. */
    owl_register_method(METHOD_TYPE_IAMODELE, "RetireDerni\xc3\xa8reCouche",
                        fn_iamodele_retire_derniere_couche, 1);
    owl_register_method(METHOD_TYPE_IAMODELE, "RemoveLastLayer",
                        fn_iamodele_retire_derniere_couche, 1);

    /* Forward : receveur + entree (arite 2). Bilingue Infère / Infer. */
    owl_register_method(METHOD_TYPE_IAMODELE, "Inf\xc3\xa8re",
                        fn_iamodele_infere, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "Infer",
                        fn_iamodele_infere, 2);

    /* Introspection : receveur + indice (arite 2). Bilingue Poids/Weights,
     * Biais/Bias. */
    owl_register_method(METHOD_TYPE_IAMODELE, "Poids", fn_iamodele_poids, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "Weights", fn_iamodele_poids, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "Biais", fn_iamodele_biais, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "Bias", fn_iamodele_biais, 2);

    /* Entrainement : receveur + X + Y + options (arite 4). Bilingue
     * Entraîne / Train. */
    owl_register_method(METHOD_TYPE_IAMODELE, "Entra\xc3\xaene",
                        fn_iamodele_entraine, 4);
    owl_register_method(METHOD_TYPE_IAMODELE, "Train",
                        fn_iamodele_entraine, 4);

    /* Persistance fluente : receveur + chemin (arite 2). Bilingue
     * Sauve / Save, Charge / Load. Meme impl que les formes prefixees. */
    owl_register_method(METHOD_TYPE_IAMODELE, "Sauve", fn_iamodele_sauve, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "Save", fn_iamodele_sauve, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "Charge", fn_iamodele_charge, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "Load", fn_iamodele_charge, 2);

    /* Import ONNX : receveur + chemin (arite 2). Bilingue
     * ChargeONNX / LoadONNX. Meme impl que la forme prefixee. */
    owl_register_method(METHOD_TYPE_IAMODELE, "ChargeONNX",
                        fn_iamodele_charge_onnx, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "LoadONNX",
                        fn_iamodele_charge_onnx, 2);

    /* Détection d'objets : receveur + image + seuilScore + seuilIoU
     * (arite 4). Bilingue Détecte / Detect. Rend un tableau de Détection. */
    owl_register_method(METHOD_TYPE_IAMODELE, "D\xc3\xa9tecte",
                        fn_iamodele_detecte, 4);
    owl_register_method(METHOD_TYPE_IAMODELE, "Detect",
                        fn_iamodele_detecte, 4);

    /* Export ONNX (, lot 2) : receveur + chemin (arite 2). Bilingue
     * SauveONNX / SaveONNX. Meme impl que la forme prefixee. */
    owl_register_method(METHOD_TYPE_IAMODELE, "SauveONNX",
                        fn_iamodele_sauve_onnx, 2);
    owl_register_method(METHOD_TYPE_IAMODELE, "SaveONNX",
                        fn_iamodele_sauve_onnx, 2);

    /* Formes prefixees IAModèleSauve / IAModèleCharge (bilingues AIModelSave /
     * AIModelLoad) : memes impl que les methodes, exposees comme natives
     * globales avec metadonnees `owl explain`. */
    static const OwlParam p_sauve[] = {
        { "modele", "IAMod\xc3\xa8le",
          "Modele a serialiser.", NULL, NULL, false },
        { "chemin", "chaine",
          "Chemin du fichier JSON a ecrire.", NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_sauve[] = {
        { 2, p_sauve, "bool\xc3\xa9" "en",
          "Vrai si l'ecriture reussit, Faux (avec ErreurD\xc3\xa9tect\xc3\xa9" "e) sinon." },
        {0}
    };
    static const OwlParam p_charge[] = {
        { "modele", "IAMod\xc3\xa8le",
          "Modele a remplir : ses couches sont reconstruites avec les poids lus.",
          NULL, NULL, false },
        { "chemin", "chaine",
          "Chemin du fichier JSON a lire.", NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_charge[] = {
        { 2, p_charge, "bool\xc3\xa9" "en",
          "Vrai si le chargement reussit, Faux (avec ErreurD\xc3\xa9tect\xc3\xa9" "e) sinon." },
        {0}
    };
    static const OwlParam p_charge_onnx[] = {
        { "modele", "IAMod\xc3\xa8le",
          "Modele a remplir : ses couches sont reconstruites depuis le graphe ONNX.",
          NULL, NULL, false },
        { "chemin", "chaine",
          "Chemin du fichier .onnx a lire.", NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_charge_onnx[] = {
        { 2, p_charge_onnx, "bool\xc3\xa9" "en",
          "Vrai si l'import reussit, Faux (avec ErreurD\xc3\xa9tect\xc3\xa9" "e) sinon." },
        {0}
    };
    static const OwlParam p_sauve_onnx[] = {
        { "modele", "IAMod\xc3\xa8le",
          "Modele a exporter : ses couches et poids sont serialises en ONNX.",
          NULL, NULL, false },
        { "chemin", "chaine",
          "Chemin du fichier .onnx a ecrire.", NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_sauve_onnx[] = {
        { 2, p_sauve_onnx, "bool\xc3\xa9" "en",
          "Vrai si l'export reussit, Faux (avec ErreurD\xc3\xa9tect\xc3\xa9" "e) sinon." },
        {0}
    };
    static const OwlNativeDef DEFS_IAMODELE[] = {
        { .canonical = "IAMod\xc3\xa8leSauve",
          .canonicalAscii = "IAModeleSauve",
          .nameEN     = "AIModelSave",
          .category   = "Intelligence artificielle",
          .descFR     = "Serialise un IAModèle (couches, poids et biais) au format "
                        "JSON dans un fichier. Les reels sont ecrits en pleine "
                        "precision : un modele sauve puis recharge redonne "
                        "exactement les memes poids, donc la meme sortie de "
                        "`.Infère`. Rend Vrai si l'ecriture reussit, Faux (avec "
                        "ErreurDétectée) si le chemin est invalide. Equivalent de "
                        "la methode fluente `modele.Sauve(chemin)`.",
          .descEN     = "Serializes an AIModel (layers, weights and biases) to a "
                        "JSON file. Reals are written at full precision: a model "
                        "saved then reloaded yields exactly the same weights, hence "
                        "the same `.Infer` output. Returns True on success, False "
                        "(with ErrorDetected) if the path is invalid. Equivalent to "
                        "the fluent method `model.Save(path)`.",
          .signatures = sig_sauve,
          .impl       = fn_iamodele_sauve,
          .flags      = 0 },
        { .canonical = "IAMod\xc3\xa8leCharge",
          .canonicalAscii = "IAModeleCharge",
          .nameEN     = "AIModelLoad",
          .category   = "Intelligence artificielle",
          .descFR     = "Lit un fichier JSON ecrit par IAModèleSauve et reconstruit "
                        "les couches du modele fourni AVEC les poids lus (le modele "
                        "repart de zero). Apres l'appel, le modele est pret a "
                        "`.Infère` a l'identique, sans re-entrainement. Rend Vrai si "
                        "le chargement reussit, Faux (avec ErreurDétectée) si le "
                        "fichier est absent ou illisible. Equivalent de la methode "
                        "fluente `modele.Charge(chemin)`.",
          .descEN     = "Reads a JSON file written by AIModelSave and rebuilds the "
                        "given model's layers WITH the loaded weights (the model "
                        "starts fresh). After the call the model is ready to `.Infer` "
                        "identically, with no retraining. Returns True on success, "
                        "False (with ErrorDetected) if the file is missing or "
                        "unreadable. Equivalent to the fluent method "
                        "`model.Load(path)`.",
          .signatures = sig_charge,
          .impl       = fn_iamodele_charge,
          .flags      = 0 },
        { .canonical = "IAMod\xc3\xa8leChargeONNX",
          .canonicalAscii = "IAModeleChargeONNX",
          .nameEN     = "AIModelLoadONNX",
          .category   = "Intelligence artificielle",
          .descFR     = "Importe un reseau de classification depuis un fichier "
                        "`.onnx` (format ONNX) et reconstruit les couches du "
                        "modele fourni AVEC les poids importes (le modele repart "
                        "de zero). Les operations reconnues couvrent la convolution, "
                        "la normalisation par lot, ReLU, le pooling (max, moyen, "
                        "moyen global), l'aplatissement, les couches denses et les "
                        "blocs residuels (connexions de saut). Apres l'appel, le "
                        "modele est pret a `.Infère`. Rend Vrai en cas de succes, "
                        "Faux (avec ErreurDétectée) si le fichier est absent, "
                        "malforme, ou contient une operation hors perimetre. "
                        "Equivalent de la methode fluente `modele.ChargeONNX(chemin)`.",
          .descEN     = "Imports a classification network from an `.onnx` file "
                        "(ONNX format) and rebuilds the given model's layers WITH "
                        "the imported weights (the model starts fresh). Recognized "
                        "operations cover convolution, batch normalization, ReLU, "
                        "pooling (max, average, global average), flattening, dense "
                        "layers and residual blocks (skip connections). After the call "
                        "the model is ready to `.Infer`. Returns True on success, "
                        "False (with ErrorDetected) if the file is missing, "
                        "malformed, or contains an out-of-scope operation. "
                        "Equivalent to the fluent method `model.LoadONNX(path)`.",
          .signatures = sig_charge_onnx,
          .impl       = fn_iamodele_charge_onnx,
          .flags      = 0 },
        { .canonical = "IAMod\xc3\xa8leSauveONNX",
          .canonicalAscii = "IAModeleSauveONNX",
          .nameEN     = "AIModelSaveONNX",
          .category   = "Intelligence artificielle",
          .descFR     = "Exporte un reseau de classification vers un fichier "
                        "`.onnx` (format ONNX) : chaque couche du modele devient "
                        "une operation ONNX et ses poids des initializers. Les "
                        "operations produites couvrent la convolution, la "
                        "normalisation par lot, ReLU, le pooling (max, moyen, "
                        "moyen global), l'aplatissement, les couches denses et les "
                        "blocs residuels (connexions de saut). Le fichier ecrit "
                        "est relisible par `.ChargeONNX` a l'identique (memes poids, "
                        "meme sortie de `.Infère`) et par l'outillage ONNX standard. "
                        "Rend Vrai en cas de succes, Faux (avec ErreurDétectée) si "
                        "le chemin est invalide ou une couche est hors perimetre. "
                        "Equivalent de la methode fluente `modele.SauveONNX(chemin)`.",
          .descEN     = "Exports a classification network to an `.onnx` file (ONNX "
                        "format): each model layer becomes an ONNX operation and its "
                        "weights become initializers. Emitted operations cover "
                        "convolution, batch normalization, ReLU, pooling (max, "
                        "average, global average), flattening, dense layers and "
                        "residual blocks (skip connections). The written file is "
                        "reloadable by `.LoadONNX` identically (same weights, same "
                        "`.Infer` output) and by standard ONNX tooling. Returns True "
                        "on success, False (with ErrorDetected) if the path is "
                        "invalid or a layer is out of scope. Equivalent to the fluent "
                        "method `model.SaveONNX(path)`.",
          .signatures = sig_sauve_onnx,
          .impl       = fn_iamodele_sauve_onnx,
          .flags      = 0 },
    };
    owl_registry_register_module(DEFS_IAMODELE,
        (int)(sizeof(DEFS_IAMODELE) / sizeof(DEFS_IAMODELE[0])));
}
