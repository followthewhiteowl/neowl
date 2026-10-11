// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_readline_h
#define owl_readline_h

/* ================================================================
 * OWL READLINE -- Portable line editor for the REPL
 *
 * Features:
 *   - Arrow keys (left/right for cursor, up/down for history)
 *   - Home/End, Backspace, Delete
 *   - History (in-memory + file persistence)
 *   - Tab completion via callback
 *   - Works on Windows (Console API) and POSIX (future)
 * ================================================================ */

/* Completion callback type */
typedef void (*OwlCompletionCallback)(const char* buf, int bufLen);

/* Add a completion candidate (called from within the callback) */
void owl_completion_add(const char* text);

/* Set the completion callback */
void owl_completion_set_callback(OwlCompletionCallback cb);

/* Read a line with editing. Returns malloc'd string, or NULL on EOF.
 * Caller must free(). */
char* owl_readline(const char* prompt);

/* History management */
void owl_history_add(const char* line);
void owl_history_load(const char* path);
void owl_history_save(const char* path);
void owl_history_free(void);

/* Clear screen */
void owl_clear_screen(void);

#endif
