/* linenoise.c -- VERSION 1.0
 *
 * Guerrilla line editing library against the idea that a line editing lib
 * needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 *
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When linenoiseClearScreen() is called, two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "linenoise.h"

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096
static char *unsupported_term[] = {"dumb","cons25","emacs",NULL};
static linenoiseCompletionCallback *completionCallback = NULL;

static struct termios orig_termios; /* In order to restore at exit.*/
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int atexit_registered = 0; /* Register atexit just 1 time. */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState ls;

enum KEY_ACTION{
	KEY_NULL = 0,	    /* NULL */
	CTRL_A = 1,         /* Ctrl+a */
	CTRL_B = 2,         /* Ctrl-b */
	CTRL_C = 3,         /* Ctrl-c */
	CTRL_D = 4,         /* Ctrl-d */
	CTRL_E = 5,         /* Ctrl-e */
	CTRL_F = 6,         /* Ctrl-f */
	CTRL_H = 8,         /* Ctrl-h */
	TAB = 9,            /* Tab */
	CTRL_K = 11,        /* Ctrl+k */
	CTRL_L = 12,        /* Ctrl+l */
	ENTER = 13,         /* Enter */
	CTRL_N = 14,        /* Ctrl-n */
	CTRL_P = 16,        /* Ctrl-p */
	CTRL_T = 20,        /* Ctrl-t */
	CTRL_U = 21,        /* Ctrl+u */
	CTRL_W = 23,        /* Ctrl+w */
	ESC = 27,           /* Escape */
	BACKSPACE =  127    /* Backspace */
};

static void linenoiseAtExit(void);
int linenoiseHistoryAdd(const char *line, void *data);

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (lndebug_fp == NULL) { \
            lndebug_fp = fopen("/tmp/lndebug.txt","a"); \
            fprintf(lndebug_fp, \
            "[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d\n", \
            (int)l->len,(int)l->pos,(int)l->oldpos,plen,rows,rpos, \
            (int)l->maxrows,old_rows); \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(fmt, ...)
#endif

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void linenoiseSetMultiLine(int ml) {
    mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int isUnsupportedTerm(void) {
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term,unsupported_term[j])) return 1;
    return 0;
}

/* Raw mode: 1960 magic shit. */
int linenoiseEnableRawMode(int fd) {
    struct termios raw;

    if (!isatty(STDIN_FILENO)) goto fatal;
    if (!atexit_registered) {
        atexit(linenoiseAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    ls.rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

void linenoiseDisableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    if (ls.rawmode && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
        ls.rawmode = 0;
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
static int getCursorPosition(int ifd, int ofd) {
    char buf[32];
    int cols, rows;
    unsigned int i = 0;

    /* Report cursor location */
    if (write(ofd, "\x1b[6n", 4) != 4) return -1;

    /* Read the response: ESC [ rows ; cols R */
    while (i < sizeof(buf)-1) {
        if (read(ifd,buf+i,1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    /* Parse it. */
    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf+2,"%d;%d",&rows,&cols) != 2) return -1;
    return cols;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(int ifd, int ofd) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        int start, cols;

        /* Get the initial position so we can restore it later. */
        start = getCursorPosition(ifd,ofd);
        if (start == -1) goto failed;

        /* Go to right margin and get position. */
        if (write(ofd,"\x1b[999C",6) != 6) goto failed;
        cols = getCursorPosition(ifd,ofd);
        if (cols == -1) goto failed;

        /* Restore position. */
        if (cols > start) {
            char seq[32];
            snprintf(seq,32,"\x1b[%dD",cols-start);
            if (write(ofd,seq,strlen(seq)) == -1) {
                /* Can't recover... */
            }
        }
        return cols;
    } else {
        return ws.ws_col;
    }

failed:
    return 80;
}

/* Clear the screen. Used to handle ctrl+l */
void linenoiseClearScreen(void) {
    if (write(STDOUT_FILENO,"\x1b[H\x1b[2J",7) <= 0) {
        /* nothing to do, just to avoid warning. */
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void linenoiseBeep(void) {
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void freeCompletions(linenoiseCompletions *lc) {
    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);
    if (lc->cvec != NULL)
        free(lc->cvec);
}

/* This is an helper function for linenoiseEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition. */
static int completeLine(struct linenoiseState *ls) {
    linenoiseCompletions lc = {0, 0, NULL};
    int nread, nwritten, hint_len, hint_line_count, char_count;
    char c = 0, *common, *hint;
    struct winsize w;

    /* Hint is only the string after the last space */
    hint = strrchr(ls->buf, ' ');
    if (!hint) {
        hint = ls->buf;
    } else {
        ++hint;
    }

    completionCallback(ls->buf, hint, &lc);
    if (lc.len == 0) {
        linenoiseBeep();
    } else {
        unsigned int i, j;

        /* Learn the longest common part */
        common = strdup(lc.cvec[0]);
        for (i = 1; i < lc.len; ++i) {
            for (j = 0; j < strlen(lc.cvec[i]); ++j) {
                if (lc.cvec[i][j] != common[j]) {
                    break;
                }
            }
            common[j] = '\0';
        }

        /* Path completions have a different hint */
        if (lc.path && strrchr(hint, '/')) {
            hint = strrchr(hint, '/');
            ++hint;
        }

        /* Show completion */
        if ((lc.len == 1) && (common[strlen(common) - 1] != '/')) {
            nwritten = snprintf(hint, ls->buflen - (hint - ls->buf), "%s ", common);
        } else {
            nwritten = snprintf(hint, ls->buflen - (hint - ls->buf), "%s", common);
        }
        free(common);
        ls->len = ls->pos = (hint - ls->buf) + nwritten;
        linenoiseRefreshLine();

        /* A single hint */
        if (lc.len == 1) {
            freeCompletions(&lc);
            return 0;
        }

        /* Read a char */
        nread = read(ls->ifd,&c,1);
        if (nread <= 0) {
            freeCompletions(&lc);
            return -1;
        }

        /* Not a tab */
        if (c != 9) {
            freeCompletions(&lc);
            return c;
        }

        /* Learn terminal window size */
        ioctl(ls->ifd, TIOCGWINSZ, &w);

        /* Learn the longest hint */
        hint_len = strlen(lc.cvec[0]);
        for (i = 1; i < lc.len; ++i) {
            if (strlen(lc.cvec[i]) > (unsigned)hint_len) {
                hint_len = strlen(lc.cvec[i]);
            }
        }

        /* Learn the number of hints that fit a line */
        hint_line_count = 0;
        while (1) {
            char_count = 0;
            if (hint_line_count) {
                char_count += hint_line_count * (hint_len + 2);
            }
            char_count += hint_len;

            /* Too much */
            if (char_count > w.ws_col) {
                break;
            }

            /* Still fits */
            ++hint_line_count;
        }

        /* No hint fits, too bad */
        if (!hint_line_count) {
            freeCompletions(&lc);
            return c;
        }

        while (c == 9) {
            /* Second tab */
            linenoiseDisableRawMode(ls->ifd);
            printf("\n");
            for (i = 0; i < lc.len; ++i) {
                printf("%-*s", hint_len, lc.cvec[i]);
                /* Line full or last hint */
                if (((i + 1) % hint_line_count == 0) || (i == lc.len - 1)) {
                    printf("\n");
                } else {
                    printf("  ");
                }
            }
            linenoiseEnableRawMode(ls->ifd);
            linenoiseRefreshLine();

            /* Read a char */
            nread = read(ls->ifd,&c,1);
            if (nread <= 0) {
                freeCompletions(&lc);
                return -1;
            }
        }
    }

    freeCompletions(&lc);
    return c; /* Return last read character */
}

/* Register a callback function to be called for tab-completion. */
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn) {
    completionCallback = fn;
}

/* This function can be called in user completion callback to fill
 * path completion for them. hint parameter is actually the whole path
 * and buf is unused, but included to match the completion callback prototype. */
void linenoisePathCompletion(const char *buf, const char *hint, linenoiseCompletions *lc) {
    const char *ptr;
    char *full_path, *hint_ptr, match[FILENAME_MAX + 2];
    DIR *dir;
    struct dirent *ent;
    struct stat st;

    (void)buf;

    lc->path = 1;

    ptr = strrchr(hint, '/');

    /* new relative path */
    if (ptr == NULL) {
        full_path = malloc(2 + FILENAME_MAX + 1);
        strcpy(full_path, "./");

        ptr = hint;
    } else {
        full_path = malloc((int)(ptr - hint) + FILENAME_MAX + 1);
        ++ptr;
        sprintf(full_path, "%.*s", (int)(ptr - hint), hint);
    }
    hint_ptr = full_path + strlen(full_path);

    dir = opendir(full_path);
    if (dir == NULL) {
        free(full_path);
        return;
    }

    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        if (!strncmp(ptr, ent->d_name, strlen(ptr))) {
            /* is it a directory? */
            strcpy(hint_ptr, ent->d_name);
            if (stat(full_path, &st)) {
                /* skip this item */
                continue;
            }

            strcpy(match, ent->d_name);
            if (S_ISDIR(st.st_mode)) {
                strcat(match, "/");
            }

            linenoiseAddCompletion(lc, match);
        }
    }

    free(full_path);
    closedir(dir);
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str) {
    size_t len = strlen(str);
    char *copy, **cvec;

    copy = malloc(len+1);
    if (copy == NULL) return;
    memcpy(copy,str,len+1);
    cvec = realloc(lc->cvec,sizeof(char*)*(lc->len+1));
    if (cvec == NULL) {
        free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
};

static void abInit(struct abuf *ab) {
    ab->b = NULL;
    ab->len = 0;
}

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b,ab->len+len);

    if (new == NULL) return;
    memcpy(new+ab->len,s,len);
    ab->b = new;
    ab->len += len;
}

static void abFree(struct abuf *ab) {
    free(ab->b);
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshSingleLine(struct linenoiseState *l) {
    char seq[64];
    size_t plen = strlen(l->prompt);
    int fd = l->ofd;
    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    struct abuf ab;

    while((plen+pos) >= l->cols) {
        buf++;
        len--;
        pos--;
    }
    while (plen+len > l->cols) {
        len--;
    }

    abInit(&ab);
    /* Cursor to left edge */
    snprintf(seq,64,"\r");
    abAppend(&ab,seq,strlen(seq));
    /* Write the prompt and the current buffer content */
    abAppend(&ab,l->prompt,strlen(l->prompt));
    abAppend(&ab,buf,len);
    /* Erase to right */
    snprintf(seq,64,"\x1b[0K");
    abAppend(&ab,seq,strlen(seq));
    /* Move cursor to original position. */
    snprintf(seq,64,"\r\x1b[%dC", (int)(pos+plen));
    abAppend(&ab,seq,strlen(seq));
    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    abFree(&ab);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshMultiLine(struct linenoiseState *l) {
    char seq[64];
    int plen = strlen(l->prompt);
    int rows = (plen+l->len+l->cols-1)/l->cols; /* rows used by current buf. */
    int rpos = (plen+l->oldpos+l->cols)/l->cols; /* cursor relative row. */
    int rpos2; /* rpos after refresh. */
    int col; /* colum position, zero-based. */
    int old_rows = l->maxrows;
    int fd = l->ofd, j;
    struct abuf ab;

    /* Update maxrows if needed. */
    if (rows > (int)l->maxrows) l->maxrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    abInit(&ab);
    if (old_rows-rpos > 0) {
        lndebug("go down %d", old_rows-rpos);
        snprintf(seq,64,"\x1b[%dB", old_rows-rpos);
        abAppend(&ab,seq,strlen(seq));
    }

    /* Now for every row clear it, go up. */
    for (j = 0; j < old_rows-1; j++) {
        lndebug("clear+up");
        snprintf(seq,64,"\r\x1b[0K\x1b[1A");
        abAppend(&ab,seq,strlen(seq));
    }

    /* Clean the top line. */
    lndebug("clear");
    snprintf(seq,64,"\r\x1b[0K");
    abAppend(&ab,seq,strlen(seq));

    /* Write the prompt and the current buffer content */
    abAppend(&ab,l->prompt,strlen(l->prompt));
    abAppend(&ab,l->buf,l->len);

    /* If we are at the very end of the screen with our prompt, we need to
     * emit a newline and move the prompt to the first column. */
    if (l->pos &&
        l->pos == l->len &&
        (l->pos+plen) % l->cols == 0)
    {
        lndebug("<newline>");
        abAppend(&ab,"\n",1);
        snprintf(seq,64,"\r");
        abAppend(&ab,seq,strlen(seq));
        rows++;
        if (rows > (int)l->maxrows) l->maxrows = rows;
    }

    /* Move cursor to right position. */
    rpos2 = (plen+l->pos+l->cols)/l->cols; /* current cursor relative row. */
    lndebug("rpos2 %d", rpos2);

    /* Go up till we reach the expected positon. */
    if (rows-rpos2 > 0) {
        lndebug("go-up %d", rows-rpos2);
        snprintf(seq,64,"\x1b[%dA", rows-rpos2);
        abAppend(&ab,seq,strlen(seq));
    }

    /* Set column. */
    col = (plen+(int)l->pos) % (int)l->cols;
    lndebug("set col %d", 1+col);
    if (col)
        snprintf(seq,64,"\r\x1b[%dC", col);
    else
        snprintf(seq,64,"\r");
    abAppend(&ab,seq,strlen(seq));

    lndebug("\n");
    l->oldpos = l->pos;

    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    abFree(&ab);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
void linenoiseRefreshLine(void) {
    if (mlmode)
        refreshMultiLine(&ls);
    else
        refreshSingleLine(&ls);
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
int linenoiseEditInsert(struct linenoiseState *l, char c) {
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            l->pos++;
            l->len++;
            l->buf[l->len] = '\0';
            if ((!mlmode && l->plen+l->len < l->cols) /* || mlmode */) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                if (write(l->ofd,&c,1) == -1) return -1;
            } else {
                linenoiseRefreshLine();
            }
        } else {
            memmove(l->buf+l->pos+1,l->buf+l->pos,l->len-l->pos);
            l->buf[l->pos] = c;
            l->len++;
            l->pos++;
            l->buf[l->len] = '\0';
            linenoiseRefreshLine();
        }
    }
    return 0;
}

/* Move cursor on the left. */
void linenoiseEditMoveLeft(struct linenoiseState *l) {
    if (l->pos > 0) {
        l->pos--;
        linenoiseRefreshLine();
    }
}

/* Move cursor on the right. */
void linenoiseEditMoveRight(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos++;
        linenoiseRefreshLine();
    }
}

/* Move cursor to the start of the line. */
void linenoiseEditMoveHome(struct linenoiseState *l) {
    if (l->pos != 0) {
        l->pos = 0;
        linenoiseRefreshLine();
    }
}

/* Move cursor to the end of the line. */
void linenoiseEditMoveEnd(struct linenoiseState *l) {
    if (l->pos != l->len) {
        l->pos = l->len;
        linenoiseRefreshLine();
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
void linenoiseEditHistoryNext(struct linenoiseState *l, int dir) {
    if (l->history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        free(l->history[l->history_len - 1 - l->history_index].line);

        l->history[l->history_len - 1 - l->history_index].line = strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if (l->history_index >= l->history_len) {
            l->history_index = l->history_len-1;
            return;
        }
        strncpy(l->buf,l->history[l->history_len - 1 - l->history_index].line,l->buflen);
        l->buf[l->buflen-1] = '\0';
        l->len = l->pos = strlen(l->buf);
        linenoiseRefreshLine();
    }
}

/* Callback used for freeing user history data. */
void linenoiseHistoryDataFree(void (*hist_data_free)(void *data)) {
    ls.hist_data_free = hist_data_free;
}

static void linenoiseHistItemFree(int hist_idx) {
    free(ls.history[hist_idx].line);
    if (ls.hist_data_free) {
        ls.hist_data_free(ls.history[hist_idx].data);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
void linenoiseEditDelete(struct linenoiseState *l) {
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf+l->pos,l->buf+l->pos+1,l->len-l->pos-1);
        l->len--;
        l->buf[l->len] = '\0';
        linenoiseRefreshLine();
    }
}

/* Backspace implementation. */
void linenoiseEditBackspace(struct linenoiseState *l) {
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf+l->pos-1,l->buf+l->pos,l->len-l->pos);
        l->pos--;
        l->len--;
        l->buf[l->len] = '\0';
        linenoiseRefreshLine();
    }
}

/* Delete the previosu word, maintaining the cursor at the start of the
 * current word. */
void linenoiseEditDeletePrevWord(struct linenoiseState *l) {
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos--;
    while (l->pos > 0 && l->buf[l->pos-1] != ' ')
        l->pos--;
    diff = old_pos - l->pos;
    memmove(l->buf+l->pos,l->buf+old_pos,l->len-old_pos+1);
    l->len -= diff;
    linenoiseRefreshLine();
}

/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */
static int linenoiseEdit(int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt)
{
    /* Populate the linenoise state that we pass to functions implementing
     * specific editing functionalities. */
    ls.ifd = stdin_fd;
    ls.ofd = stdout_fd;
    ls.buf = buf;
    ls.buflen = buflen;
    ls.prompt = prompt;
    ls.plen = strlen(prompt);
    ls.oldpos = ls.pos = 0;
    ls.len = 0;
    ls.cols = getColumns(stdin_fd, stdout_fd);
    ls.maxrows = 0;
    ls.history_index = 0;

    /* Buffer starts empty. */
    ls.buf[0] = '\0';
    ls.buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    linenoiseHistoryAdd("", NULL);

    if (write(ls.ofd,prompt,ls.plen) == -1) return -1;
    while(1) {
        char c;
        int nread;
        char seq[3];

        nread = read(ls.ifd,&c,1);
        if (nread <= 0) return ls.len;

        /* Only autocomplete when the callback is set. It returns < 0 when
         * there was an error reading from fd. Otherwise it will return the
         * character that should be handled next. */
        if (c == 9 && completionCallback != NULL) {
            c = completeLine(&ls);
            /* Return on errors */
            if (c < 0) return ls.len;
            /* Read next character when 0 */
            if (c == 0) continue;
        }

        switch(c) {
        case ENTER:    /* enter */
            ls.history_len--;
            linenoiseHistItemFree(ls.history_len);
            if (mlmode) linenoiseEditMoveEnd(&ls);
            return (int)ls.len;
        case CTRL_C:     /* ctrl-c */
            errno = EAGAIN;
            return -1;
        case BACKSPACE:   /* backspace */
        case 8:     /* ctrl-h */
            linenoiseEditBackspace(&ls);
            break;
        case CTRL_D:     /* ctrl-d, remove char at right of cursor, or if the
                            line is empty, act as end-of-file. */
            if (ls.len > 0) {
                linenoiseEditDelete(&ls);
            } else {
                ls.history_len--;
                linenoiseHistItemFree(ls.history_len);
                return -1;
            }
            break;
        case CTRL_T:    /* ctrl-t, swaps current character with previous. */
            if (ls.pos > 0 && ls.pos < ls.len) {
                int aux = buf[ls.pos-1];
                buf[ls.pos-1] = buf[ls.pos];
                buf[ls.pos] = aux;
                if (ls.pos != ls.len-1) ls.pos++;
                linenoiseRefreshLine();
            }
            break;
        case CTRL_B:     /* ctrl-b */
            linenoiseEditMoveLeft(&ls);
            break;
        case CTRL_F:     /* ctrl-f */
            linenoiseEditMoveRight(&ls);
            break;
        case CTRL_P:    /* ctrl-p */
            linenoiseEditHistoryNext(&ls, LINENOISE_HISTORY_PREV);
            break;
        case CTRL_N:    /* ctrl-n */
            linenoiseEditHistoryNext(&ls, LINENOISE_HISTORY_NEXT);
            break;
        case ESC:    /* escape sequence */
            /* Read the next two bytes representing the escape sequence.
             * Use two calls to handle slow terminals returning the two
             * chars at different times. */
            if (read(ls.ifd,seq,1) == -1) break;
            if (read(ls.ifd,seq+1,1) == -1) break;

            /* ESC [ sequences. */
            if (seq[0] == '[') {
                if (seq[1] >= '0' && seq[1] <= '9') {
                    /* Extended escape, read additional byte. */
                    if (read(ls.ifd,seq+2,1) == -1) break;
                    if (seq[2] == '~') {
                        switch(seq[1]) {
                        case '3': /* Delete key. */
                            linenoiseEditDelete(&ls);
                            break;
                        }
                    }
                } else {
                    switch(seq[1]) {
                    case 'A': /* Up */
                        linenoiseEditHistoryNext(&ls, LINENOISE_HISTORY_PREV);
                        break;
                    case 'B': /* Down */
                        linenoiseEditHistoryNext(&ls, LINENOISE_HISTORY_NEXT);
                        break;
                    case 'C': /* Right */
                        linenoiseEditMoveRight(&ls);
                        break;
                    case 'D': /* Left */
                        linenoiseEditMoveLeft(&ls);
                        break;
                    case 'H': /* Home */
                        linenoiseEditMoveHome(&ls);
                        break;
                    case 'F': /* End*/
                        linenoiseEditMoveEnd(&ls);
                        break;
                    }
                }
            }

            /* ESC O sequences. */
            else if (seq[0] == 'O') {
                switch(seq[1]) {
                case 'H': /* Home */
                    linenoiseEditMoveHome(&ls);
                    break;
                case 'F': /* End*/
                    linenoiseEditMoveEnd(&ls);
                    break;
                }
            }
            break;
        default:
            if (linenoiseEditInsert(&ls,c)) return -1;
            break;
        case CTRL_U: /* Ctrl+u, delete the whole line. */
            buf[0] = '\0';
            ls.pos = ls.len = 0;
            linenoiseRefreshLine();
            break;
        case CTRL_K: /* Ctrl+k, delete from current to end of line. */
            buf[ls.pos] = '\0';
            ls.len = ls.pos;
            linenoiseRefreshLine();
            break;
        case CTRL_A: /* Ctrl+a, go to the start of the line */
            linenoiseEditMoveHome(&ls);
            break;
        case CTRL_E: /* ctrl+e, go to the end of the line */
            linenoiseEditMoveEnd(&ls);
            break;
        case CTRL_L: /* ctrl+l, clear screen */
            linenoiseClearScreen();
            linenoiseRefreshLine();
            break;
        case CTRL_W: /* ctrl+w, delete previous word */
            linenoiseEditDeletePrevWord(&ls);
            break;
        }
    }
    return ls.len;
}

/* This special mode is used by linenoise in order to print scan codes
 * on screen for debugging / development purposes. It is implemented
 * by the linenoise_example program using the --keycodes option. */
void linenoisePrintKeyCodes(void) {
    char quit[4];

    printf("Linenoise key codes debugging mode.\n"
            "Press keys to see scan codes. Type 'quit' at any time to exit.\n");
    if (linenoiseEnableRawMode(STDIN_FILENO) == -1) return;
    memset(quit,' ',4);
    while(1) {
        char c;
        int nread;

        nread = read(STDIN_FILENO,&c,1);
        if (nread <= 0) continue;
        memmove(quit,quit+1,sizeof(quit)-1); /* shift string to left. */
        quit[sizeof(quit)-1] = c; /* Insert current char on the right. */
        if (memcmp(quit,"quit",sizeof(quit)) == 0) break;

        printf("'%c' %02x (%d) (type quit to exit)\n",
            isprint(c) ? c : '?', (int)c, (int)c);
        printf("\r"); /* Go left edge manually, we are in raw mode. */
        fflush(stdout);
    }
    linenoiseDisableRawMode(STDIN_FILENO);
}

/* This function calls the line editing function linenoiseEdit() using
 * the STDIN file descriptor set in raw mode. */
static int linenoiseRaw(char *buf, size_t buflen, const char *prompt) {
    int count;

    if (buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(STDIN_FILENO)) {
        /* Not a tty: read from file / pipe. */
        if (fgets(buf, buflen, stdin) == NULL) return -1;
        count = strlen(buf);
        if (count && buf[count-1] == '\n') {
            count--;
            buf[count] = '\0';
        }
    } else {
        /* Interactive editing. */
        if (linenoiseEnableRawMode(STDIN_FILENO) == -1) return -1;
        count = linenoiseEdit(STDIN_FILENO, STDOUT_FILENO, buf, buflen, prompt);
        linenoiseDisableRawMode(STDIN_FILENO);
        printf("\n");
    }
    return count;
}

/* The high level function that is the main API of the linenoise library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *linenoise(const char *prompt) {
    char buf[LINENOISE_MAX_LINE];
    int count;

    if (isUnsupportedTerm()) {
        size_t len;

        printf("%s",prompt);
        fflush(stdout);
        if (fgets(buf,LINENOISE_MAX_LINE,stdin) == NULL) return NULL;
        len = strlen(buf);
        while(len && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            len--;
            buf[len] = '\0';
        }
        return strdup(buf);
    } else {
        count = linenoiseRaw(buf,LINENOISE_MAX_LINE,prompt);
        if (count == -1) return NULL;
        return strdup(buf);
    }
}

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void freeHistory(void) {
    if (ls.history) {
        int i;

        for (i = 0; i < ls.history_len; i++) {
            linenoiseHistItemFree(i);
        }
        free(ls.history);
    }
}

/* At exit we'll try to fix the terminal to the initial conditions. */
static void linenoiseAtExit(void) {
    linenoiseDisableRawMode(STDIN_FILENO);
    freeHistory();
}

/* This is the API call to add a new entry in the linenoise history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int linenoiseHistoryAdd(const char *line, void *data) {
    char *linecopy;

    if (history_max_len == 0) return 0;

    /* Initialization on first call. */
    if (ls.history == NULL) {
        ls.history = malloc(history_max_len*sizeof *ls.history);
        if (ls.history == NULL) return 0;
        memset(ls.history,0,(history_max_len*sizeof *ls.history));
    }

    /* Don't add duplicated lines. */
    if (ls.history_len && !strcmp(ls.history[ls.history_len-1].line, line)) {
        if (ls.hist_data_free) {
            ls.hist_data_free(ls.history[ls.history_len-1].data);
        }
        ls.history[ls.history_len-1].data = data;
        return 0;
    }

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (ls.history_len == history_max_len) {
        linenoiseHistItemFree(0);
        memmove(ls.history,ls.history+1,(history_max_len-1)*sizeof *ls.history);
        ls.history_len--;
    }
    ls.history[ls.history_len].line = linecopy;
    ls.history[ls.history_len].data = data;
    ls.history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int linenoiseHistorySetMaxLen(int len) {
    struct linenoiseHistItem *new;

    if (len < 1) return 0;
    if (ls.history) {
        int tocopy = ls.history_len;

        new = malloc(len*sizeof *new);
        if (new == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {
            int j;

            for (j = 0; j < tocopy-len; j++) linenoiseHistItemFree(j);
            tocopy = len;
        }
        memset(new,0,len*sizeof *new);
        memcpy(new,ls.history+(ls.history_len-tocopy), tocopy*sizeof *ls.history);
        free(ls.history);
        ls.history = new;
    }
    history_max_len = len;
    if (ls.history_len > history_max_len)
        ls.history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int linenoiseHistorySave(const char *filename) {
    FILE *fp = fopen(filename,"w");
    int j;

    if (fp == NULL) return -1;
    for (j = 0; j < ls.history_len; j++)
        fprintf(fp,"%s\n",ls.history[j].line);
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int linenoiseHistoryLoad(const char *filename) {
    FILE *fp = fopen(filename,"r");
    char buf[LINENOISE_MAX_LINE];

    if (fp == NULL) return -1;

    while (fgets(buf,LINENOISE_MAX_LINE,fp) != NULL) {
        char *p;

        p = strchr(buf,'\r');
        if (!p) p = strchr(buf,'\n');
        if (p) *p = '\0';
        linenoiseHistoryAdd(buf,NULL);
    }
    fclose(fp);
    return 0;
}
