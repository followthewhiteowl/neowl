// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Socle Message Queue — E1 « pont de livraison ».
 *
 * Voir stdlib_mq.h pour le contrat. Ce fichier est la SEULE moitie du
 * socle MQ qui connait la VM / les Value : la glue vm_call_reentrant +
 * le rooting GC des handlers vivent ici. net_reacteur.c reste 100 %
 * C-heap et n'inclut ni vm.h ni object.h (isolation GC §8 Q1).
 */

#include "stdlib_mq.h"

#include "vm.h"
#include "object.h"
#include "gc.h"

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600
  #endif
  #include <winsock2.h>
#else
  #include <sys/socket.h>
#endif

/* ================================================================
 * Registre d'abonnements
 *
 * Tableau STATIQUE de taille fixe : chaque slot a une adresse stable,
 * enregistree une fois pour toutes comme racine GC persistante (gc.h).
 * C'est ce qui tient les handlers en vie a travers les cycles GC sans
 * fonction _mark_roots() dediee ni edit de gc.c. Les slots inactifs
 * valent NIL_VAL (marques comme NIL, inoffensif).
 * ================================================================ */

static Value g_mq_handlers[OWL_MQ_MAX_ABONNEMENTS];
static bool  g_mq_actif[OWL_MQ_MAX_ABONNEMENTS];
static bool  g_mq_socle_initialise = false;

static bool mq_valeur_appelable(Value v) {
    return IS_FUNCTION(v) || IS_NATIVE(v);
}

static bool mq_handle_valide(OwlMqHandle h) {
    return h > 0 && h <= OWL_MQ_MAX_ABONNEMENTS && g_mq_actif[h - 1];
}

void owl_mq_registre_init(void) {
    /* Idempotent : le socle MQ est partage par tous les clients protocolaires
     * (MQTT aujourd'hui, STOMP/AMQP demain). Un 2e appel ne doit PAS vider les
     * slots — sinon un 2e client clobbererait les abonnements actifs du 1er.
     * L'init reel (vidage + enregistrement des racines GC) est one-shot. */
    if (g_mq_socle_initialise) return;

    for (int i = 0; i < OWL_MQ_MAX_ABONNEMENTS; i++) {
        g_mq_handlers[i] = NIL_VAL;
        g_mq_actif[i]    = false;
        owl_register_persistent_value_root(&g_mq_handlers[i]);
    }
    g_mq_socle_initialise = true;
}

void stdlib_mq_init(void) {
    /* Point d'init central et neutre du socle Message Queue : appele une fois
     * par owl_stdlib_init, AVANT tout client protocolaire, pour que MQTT et les
     * futurs clients partagent le meme registre d'abonnements sans le
     * reinitialiser (owl_mq_registre_init est idempotent). */
    owl_mq_registre_init();
}

void owl_mq_registre_libere(void) {
    for (int i = 0; i < OWL_MQ_MAX_ABONNEMENTS; i++) {
        g_mq_handlers[i] = NIL_VAL;
        g_mq_actif[i]    = false;
    }
}

OwlMqHandle owl_mq_abonne(Value handler) {
    if (!mq_valeur_appelable(handler)) return OWL_MQ_HANDLE_NUL;
    for (int i = 0; i < OWL_MQ_MAX_ABONNEMENTS; i++) {
        if (!g_mq_actif[i]) {
            g_mq_handlers[i] = handler;   /* devient racine GC (slot deja enregistre) */
            g_mq_actif[i]    = true;
            return (OwlMqHandle)(i + 1);
        }
    }
    return OWL_MQ_HANDLE_NUL;  /* registre plein */
}

bool owl_mq_desabonne(OwlMqHandle h) {
    if (!mq_handle_valide(h)) return false;
    g_mq_handlers[h - 1] = NIL_VAL;
    g_mq_actif[h - 1]    = false;
    return true;
}

bool owl_mq_handler(OwlMqHandle h, Value* out) {
    if (!mq_handle_valide(h)) return false;
    *out = g_mq_handlers[h - 1];
    return true;
}

/* ================================================================
 * Livraison C -> OWL
 *
 * Marshalle `payload` en une Value chaine OWL (UTF-8 = octets bruts,
 * contrat socle) puis invoque le handler via vm_call_reentrant, dans le
 * respect de la frontiere de reentrance bornee (cf. http_exec.c) :
 * callee pousse, args au-dessus, stackTop remis au repere au retour.
 * ================================================================ */

void owl_mq_deliver(OwlMqHandle h, const char* payload, size_t len) {
    if (!mq_handle_valide(h)) return;

    Value handler = g_mq_handlers[h - 1];   /* deja racine GC persistante */

    Value* repere = vm.stackTop;            /* frontiere de depilage propre */
    vm_push(handler);                       /* callee au-dessus du repere */
    /* ObjString fraiche : push immediat en racine GC (gc-root-obj-fresh).
     * Binaire -> Buffer par protocole = , pas ici. */
    ObjString* charge = obj_copy_string(payload ? payload : "", (int)len);
    vm_push(OBJ_VAL(charge));

    (void)vm_call_reentrant(handler, 1);

    /* La livraison ne consomme aucune valeur de retour : rejette tout
     * residu de pile (retour du handler) et retrouve le repere. */
    vm.stackTop = repere;
}

/* ================================================================
 * Ancrage reacteur (pompe)
 * ================================================================ */

static void mq_reacteur_lecture_cb(OwlReacteur* r, owl_socket_fd_t fd,
                                   short evts_prets, void* user) {
    OwlMqHandle h = (OwlMqHandle)(intptr_t)user;

    if (evts_prets & OWL_REAC_LECTURE) {
        char tampon[4096];
        int n = (int)recv(fd, tampon, (int)sizeof(tampon), 0);
        if (n > 0) {
            /* Framing trivial du socle : le bloc lu EST le payload. Le
             * decoupage protocolaire (length-prefix, ligne, trame AMQP...)
             * sera fait par le client en amont d'E1. */
            owl_mq_deliver(h, tampon, (size_t)n);
            return;
        }
        /* n == 0 (EOF) ou n < 0 (erreur) : le pair a raccroche. */
        owl_reacteur_retire_fd(r, fd);
        return;
    }

    if (evts_prets & OWL_REAC_ERREUR) {
        owl_reacteur_retire_fd(r, fd);
    }
}

bool owl_mq_reacteur_branche(OwlReacteur* r, owl_socket_fd_t fd, OwlMqHandle h) {
    if (r == NULL || !mq_handle_valide(h)) return false;
    return owl_reacteur_ajoute_fd(r, fd, OWL_REAC_LECTURE,
                                  mq_reacteur_lecture_cb, (void*)(intptr_t)h);
}
