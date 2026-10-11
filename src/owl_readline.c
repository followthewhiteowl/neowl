// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_readline.h"
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <termios.h>
#endif

/* ================================================================
 * HISTORY
 * ================================================================ */

#define MAX_HISTORY 500
#define MAX_LINE    4096

static char* history[MAX_HISTORY];
static int historyCount = 0;

void owl_history_add(const char* line) {
    if (!line || !line[0]) return;
    /* Skip if same as last entry */
    if (historyCount > 0 && strcmp(history[historyCount - 1], line) == 0) return;

    if (historyCount >= MAX_HISTORY) {
        free(history[0]);
        memmove(history, history + 1, (MAX_HISTORY - 1) * sizeof(char*));
        historyCount--;
    }
    history[historyCount++] = safe_strdup(line);
}

void owl_history_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0]) owl_history_add(line);
    }
    fclose(f);
}

void owl_history_save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < historyCount; i++) {
        fprintf(f, "%s\n", history[i]);
    }
    fclose(f);
}

void owl_history_free(void) {
    for (int i = 0; i < historyCount; i++) free(history[i]);
    historyCount = 0;
}

/* ================================================================
 * COMPLETION
 * ================================================================ */

static OwlCompletionCallback completionCallback = NULL;

/* Temporary storage for completions during a callback */
static char** completions = NULL;
static int completionCount = 0;
static int completionCapacity = 0;

void owl_completion_set_callback(OwlCompletionCallback cb) {
    completionCallback = cb;
}

void owl_completion_add(const char* text) {
    if (completionCount >= completionCapacity) {
        completionCapacity = completionCapacity < 16 ? 16 : completionCapacity * 2;
        completions = realloc(completions, completionCapacity * sizeof(char*));
    }
    completions[completionCount++] = safe_strdup(text);
}

static void completions_free(void) {
    for (int i = 0; i < completionCount; i++) free(completions[i]);
    completionCount = 0;
}

/* ================================================================
 * CLEAR SCREEN
 * ================================================================ */

void owl_clear_screen(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(h, &info);
    DWORD written;
    COORD origin = {0, 0};
    DWORD size = info.dwSize.X * info.dwSize.Y;
    FillConsoleOutputCharacterA(h, ' ', size, origin, &written);
    FillConsoleOutputAttribute(h, info.wAttributes, size, origin, &written);
    SetConsoleCursorPosition(h, origin);
#else
    printf("\033[2J\033[H");
    fflush(stdout);
#endif
}

/* ================================================================
 * LINE EDITOR -- Windows implementation
 * ================================================================ */

#ifdef _WIN32

/* Key codes from _getch() */
#define KEY_EXTENDED  0
#define KEY_EXTENDED2 0xE0
#define KEY_UP        72
#define KEY_DOWN      80
#define KEY_LEFT      75
#define KEY_RIGHT     77
#define KEY_HOME      71
#define KEY_END       79
#define KEY_DELETE    83
#define KEY_BACKSPACE 8
#define KEY_ENTER     13
#define KEY_TAB       9
#define KEY_CTRL_C    3
#define KEY_CTRL_D    4
#define KEY_CTRL_L    12

static void refresh_line(const char* prompt, const char* buf, int len, int pos) {
    /* Move cursor to start of line, clear, rewrite */
    printf("\r%s%.*s", prompt, len, buf);
    /* Clear any leftover characters */
    printf("   "); /* small clear margin */
    /* Move cursor to correct position */
    printf("\r%s", prompt);
    for (int i = 0; i < pos; i++) putchar(buf[i]);
    fflush(stdout);
}

char* owl_readline(const char* prompt) {
    /* If stdin is not a terminal (piped input), use simple fgets */
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    if (!GetConsoleMode(hIn, &mode)) {
        /* Not a console -- piped input */
        static char fbuf[MAX_LINE];
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(fbuf, sizeof(fbuf), stdin)) return NULL;
        size_t flen = strlen(fbuf);
        if (flen > 0 && fbuf[flen - 1] == '\n') fbuf[flen - 1] = '\0';
        return safe_strdup(fbuf);
    }

    char buf[MAX_LINE];
    int len = 0;
    int pos = 0; /* cursor position */
    int histIdx = historyCount; /* browsing index */

    printf("%s", prompt);
    fflush(stdout);

    for (;;) {
        int c = _getch();

        if (c == KEY_ENTER) {
            putchar('\n');
            buf[len] = '\0';
            return safe_strdup(buf);
        }

        if (c == KEY_CTRL_C) {
            putchar('\n');
            buf[0] = '\0';
            return safe_strdup(buf); /* empty line, not EOF */
        }

        if (c == KEY_CTRL_D) {
            if (len == 0) return NULL; /* EOF on empty line */
            continue;
        }

        if (c == KEY_CTRL_L) {
            owl_clear_screen();
            printf("%s%.*s", prompt, len, buf);
            fflush(stdout);
            refresh_line(prompt, buf, len, pos);
            continue;
        }

        if (c == KEY_BACKSPACE) {
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos);
                pos--;
                len--;
                refresh_line(prompt, buf, len, pos);
            }
            continue;
        }

        if (c == KEY_TAB) {
            /* Tab completion */
            if (!completionCallback) continue;
            completions_free();
            completionCallback(buf, len);
            if (completionCount == 0) continue;

            if (completionCount == 1) {
                /* Single match: replace buffer */
                len = (int)strlen(completions[0]);
                if (len >= MAX_LINE) len = MAX_LINE - 1;
                memcpy(buf, completions[0], len);
                pos = len;
                refresh_line(prompt, buf, len, pos);
            } else {
                /* Multiple matches: show them */
                putchar('\n');
                for (int i = 0; i < completionCount; i++) {
                    printf("  %s", completions[i]);
                    if ((i + 1) % 4 == 0 || i == completionCount - 1) putchar('\n');
                    else printf("    ");
                }
                printf("%s%.*s", prompt, len, buf);
                fflush(stdout);
                refresh_line(prompt, buf, len, pos);
            }
            completions_free();
            continue;
        }

        /* Extended keys (arrows, home, end, delete) */
        if (c == KEY_EXTENDED || c == KEY_EXTENDED2) {
            int ext = _getch();
            switch (ext) {
                case KEY_LEFT:
                    if (pos > 0) { pos--; refresh_line(prompt, buf, len, pos); }
                    break;
                case KEY_RIGHT:
                    if (pos < len) { pos++; refresh_line(prompt, buf, len, pos); }
                    break;
                case KEY_UP:
                    if (histIdx > 0) {
                        histIdx--;
                        len = (int)strlen(history[histIdx]);
                        if (len >= MAX_LINE) len = MAX_LINE - 1;
                        memcpy(buf, history[histIdx], len);
                        pos = len;
                        refresh_line(prompt, buf, len, pos);
                    }
                    break;
                case KEY_DOWN:
                    if (histIdx < historyCount - 1) {
                        histIdx++;
                        len = (int)strlen(history[histIdx]);
                        if (len >= MAX_LINE) len = MAX_LINE - 1;
                        memcpy(buf, history[histIdx], len);
                        pos = len;
                        refresh_line(prompt, buf, len, pos);
                    } else {
                        histIdx = historyCount;
                        len = 0; pos = 0;
                        refresh_line(prompt, buf, len, pos);
                    }
                    break;
                case KEY_HOME:
                    pos = 0;
                    refresh_line(prompt, buf, len, pos);
                    break;
                case KEY_END:
                    pos = len;
                    refresh_line(prompt, buf, len, pos);
                    break;
                case KEY_DELETE:
                    if (pos < len) {
                        memmove(buf + pos, buf + pos + 1, len - pos - 1);
                        len--;
                        refresh_line(prompt, buf, len, pos);
                    }
                    break;
            }
            continue;
        }

        /* Regular character */
        if (c >= 32 && c < 127 && len < MAX_LINE - 1) {
            if (pos < len) {
                memmove(buf + pos + 1, buf + pos, len - pos);
            }
            buf[pos] = (char)c;
            pos++;
            len++;
            refresh_line(prompt, buf, len, pos);
        }
    }
}

#else
/* ================================================================
 * POSIX fallback -- simple fgets wrapper for now
 * ================================================================ */

char* owl_readline(const char* prompt) {
    static char buf[MAX_LINE];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return safe_strdup(buf);
}

#endif
