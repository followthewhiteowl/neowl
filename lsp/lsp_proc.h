// SPDX-License-Identifier: AGPL-3.0-only
/* lsp_proc.h —  : exécution BORNÉE d'un sous-processus pour le LSP.
 *
 * Isolé de lsp_handlers.c parce que l'implémentation Windows a besoin de
 * <windows.h>, dont les symboles (TokenType, ...) entrent en conflit avec
 * le scanner OWL inclus par lsp_handlers.c. Ce fichier est aussi, pour la
 * même raison, le domicile des primitives de lecture stdin (PeekNamedPipe).
 */
#ifndef LSP_PROC_H
#define LSP_PROC_H

/* Délai par défaut au-delà duquel `owl --check` est considéré bloqué. */
#define LSP_CHECK_TIMEOUT_MS 15000

typedef enum {
    LSP_RUN_OK,          /* process terminé, sortie capturée */
    LSP_RUN_TIMEOUT,     /* délai dépassé, process tué */
    LSP_RUN_SPAWN_FAIL   /* impossible de lancer le process */
} LspRunStatus;

/* Lance `<exePath> [--lang <lang>] --check <checkTarget>`, capture
 * stdout/stderr dans `out` (NUL-terminé, tronqué à cap-1). Tue le process si
 * > timeoutMs. `*outLen` reçoit le nombre d'octets capturés.
 *
 * BLOQUANT : ne l'appeler que hors boucle d'événements (harness de test).
 *
 * `lang` : "en" pour des diagnostics en anglais, NULL ou autre
 * pour le défaut FR (aucun flag passé). */
LspRunStatus lsp_run_check_bounded(const char* exePath,
                                   const char* checkTarget,
                                   const char* lang,
                                   char* out, int cap,
                                   int timeoutMs, int* outLen);

/* ================================================================
 *  Lot 1 — check ASYNCHRONE (start / poll / abort)
 *
 * Le LSP est mono-thread : un spawn bloquant fige la boucle d'événements
 * (~115 ms à chaud, ~5 s à froid — chargement + scan antivirus d'un binaire
 * de 100 Mo). Le check part donc en arrière-plan et son résultat est récolté
 * par sondages successifs, sans jamais bloquer.
 * ================================================================ */

typedef struct LspCheckJob LspCheckJob;

/* Spawne `<exePath> [--lang en] --check <checkTarget> [--project <projectRoot>]
 * [--overlay-stdin]` et rend la main immédiatement. Renvoie NULL si le process
 * n'a pas pu démarrer.
 *
 * `projectRoot` : NULL ou "" = check legacy (checkTarget est autonome : fichier
 * seul ou dossier projet). Non-vide + checkTarget = fichier => mode fichier
 * ACTIF avec contexte projet <projectRoot> : le projet est hydraté
 * pour la résolution cross-fichier mais SEUL <checkTarget> est rapporté, au
 * format mono {"errors":[...],"warnings":[...]}. Rapide (un fichier compilé).
 *
 * `overlayJson` : NULL ou "" = aucun overlay (le check lit le disque).
 * Non-vide = on passe `--overlay-stdin` à l'enfant et on lui écrit cet overlay
 * JSON ({"overlays":[{path,content}]}) sur stdin, intégralement, puis EOF
 * ( Lot 2). L'appelant garde la propriété de `overlayJson` : il est
 * copié dans le pipe avant le retour et peut être libéré aussitôt.
 *
 * Le chrono `timeoutMs` est armé APRÈS la création du process. */
LspCheckJob* lsp_check_start(const char* exePath,
                             const char* checkTarget,
                             const char* projectRoot,
                             const char* lang,
                             const char* overlayJson,
                             int timeoutMs);

/* Fait avancer le job SANS BLOQUER : draine ce que l'enfant a déjà écrit et
 * regarde s'il a terminé (ou dépassé son délai — auquel cas il est tué).
 *
 * Renvoie 0 tant que le job tourne (rien d'autre n'est écrit).
 * Renvoie 1 quand il est terminé : `*outStatus` reçoit le verdict,
 * `*outBuf` un buffer NUL-terminé QUE L'APPELANT POSSÈDE ET DOIT `free`
 * (non-NULL sauf OOM, même quand la sortie est vide), `*outLen` sa longueur.
 * Le job est alors récolté et libéré : le handle ne doit plus être réutilisé. */
int lsp_check_poll(LspCheckJob* job, LspRunStatus* outStatus,
                   char** outBuf, int* outLen);

/* Tue l'enfant et libère le job sans récolter sa sortie. */
void lsp_check_abort(LspCheckJob* job);

/* ================================================================
 *  Lot 2a — process `owl --check-server` RÉSIDENT
 *
 * Le chargement du binaire (~100 Mo) coûte ~150 ms à chaud, ~5 s à froid,
 * avant tout travail utile. `lsp_check_start` sert donc les checks via un
 * process résident lancé UNE fois, et ne retombe sur un spawn dédié que si
 * celui-ci n'a pas pu démarrer (sinon : plus aucun diagnostic).
 * ================================================================ */

/* Démarre le résident s'il ne tourne pas déjà. À appeler tôt (notification
 * `initialized`) pour que le coût de premier chargement soit payé hors frappe.
 * Renvoie 1 s'il tourne. */
int lsp_check_server_warmup(const char* exePath);

/* Arrête le résident (EOF sur son stdin, puis kill en garde-fou). Idempotent.
 * Également branché sur `atexit` : aucun `owl` ne doit survivre au LSP. */
void lsp_check_server_stop(void);

/* 1 si le résident tourne (observabilité / tests). */
int lsp_check_server_is_alive(void);

/* Pause courte (attente active des appelants synchrones). */
void lsp_proc_sleep_ms(int ms);

/* ================================================================
 *  Lot 1 — lecture stdin avec attente bornée
 *
 * La boucle LSP doit pouvoir *attendre* un message client tout en récoltant
 * périodiquement le check en arrière-plan. `lsp_stdin_wait` donne ce tick.
 * Corollaire : toute la lecture de stdin passe par ces primitives (buffer
 * interne explicite). Mélanger avec <stdio.h> ré-introduirait le piège
 * classique — des octets déjà avalés par le buffer stdio qu'aucun peek au
 * niveau OS ne voit, donc un message dormant pendant tout le tick.
 * ================================================================ */

/* 1 = des octets sont disponibles (buffer interne ou OS), 0 = délai écoulé,
 * -1 = EOF/erreur. Quand stdin n'est pas un pipe (console, fichier redirigé),
 * l'attente n'est pas observable : on renvoie 1 (dégradation sûre — pas de
 * tick, mais jamais de message dormant). */
int lsp_stdin_wait(int timeoutMs);

/* Lit jusqu'au '\n' inclus (bloquant). Renvoie le nombre d'octets écrits dans
 * `dst` (NUL-terminé, tronqué à cap-1), 0 sur EOF. */
int lsp_stdin_read_line(char* dst, int cap);

/* Lit exactement `n` octets (bloquant). Renvoie le nombre d'octets lus,
 * < n uniquement sur EOF. */
int lsp_stdin_read_exact(char* dst, int n);

#endif /* LSP_PROC_H */
