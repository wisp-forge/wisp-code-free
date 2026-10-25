// language: C, file: wisp-code/tui.c
#include "tui.h"
#include "skin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

/* ================================================================== */
/*  Windows console backend                                           */
/* ================================================================== */

#include <windows.h>
#include "compat/posix.h"

static HANDLE  g_in  = INVALID_HANDLE_VALUE;
static HANDLE  g_out = INVALID_HANDLE_VALUE;
static DWORD   g_saved_in_mode  = 0;
static DWORD   g_saved_out_mode = 0;
static int     g_inited = 0;
static volatile LONG g_winch_flag = 0;

/* Windows cmd.exe renders through a single console font. Fonts that
 * lack the Braille Patterns block (U+2800-U+28FF) render the spinner
 * as boxes or question marks. Try a font that has them, in order of
 * preference, and remember whether we succeeded. */
static int g_console_utf8_ok = 0;

static void try_set_console_font(HANDLE out) {
    static const wchar_t *candidates[] = {
        L"Cascadia Mono",     /* Windows 11, or Windows Terminal install */
        L"Cascadia Code",
        L"Consolas",          /* ships with Windows 7+; has Braille */
        L"Lucida Console",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        CONSOLE_FONT_INFOEX cfi;
        memset(&cfi, 0, sizeof(cfi));
        cfi.cbSize = sizeof(cfi);
        cfi.nFont = 0;
        cfi.dwFontSize.X = 0;
        cfi.dwFontSize.Y = 16;
        cfi.FontFamily = FF_DONTCARE;
        cfi.FontWeight = FW_NORMAL;
        wcsncpy(cfi.FaceName, candidates[i], LF_FACESIZE - 1);
        cfi.FaceName[LF_FACESIZE - 1] = L'\0';
        if (SetCurrentConsoleFontEx(out, FALSE, &cfi)) {
            g_console_utf8_ok = 1;
            return;
        }
    }
}

static BOOL WINAPI winch_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_WINDOW_BUFFER_SIZE_EVENT) {
        InterlockedExchange(&g_winch_flag, 1);
        return TRUE;
    }
    return FALSE;
}

int tui_init(void) {

    SetConsoleOutputCP(CP_UTF8);
    
    SetConsoleCP(CP_UTF8);

    try_set_console_font(g_out);

    g_in  = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE) return -1;

    if (!GetConsoleMode(g_in,  &g_saved_in_mode))  return -1;
    if (!GetConsoleMode(g_out, &g_saved_out_mode)) return -1;

    DWORD in_mode = g_saved_in_mode;
    in_mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
                 ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT);
    in_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT
             | ENABLE_WINDOW_INPUT
             | ENABLE_EXTENDED_FLAGS;
    if (!SetConsoleMode(g_in, in_mode)) return -1;

    DWORD out_mode = g_saved_out_mode;
    out_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING
              | DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(g_out, out_mode)) return -1;

    /* Register for resize notifications. We use a console event
     * handler so CTRL_WINDOW_BUFFER_SIZE_EVENT sets the flag the
     * main loop polls. */
    //SetConsoleCtrlHandler(winch_handler, TRUE);

    /* alt screen, hide cursor, clear, enable bracketed paste */
    const char *seq = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H\x1b[?2004h";
    DWORD w = 0;
    WriteFile(g_out, seq, (DWORD)strlen(seq), &w, NULL);

    /* Force ConPTY to flush its output buffer. It defers the actual
    * render until it sees a "natural" break, and a program that goes
    * straight from init to a full-screen draw never produces one.
    * The DSR cursor-position query triggers a reply on the input
    * channel, and ConPTY flushes the output channel to process it. */
    const char *kick = "\x1b[6n";
    WriteFile(g_out, kick, (DWORD)strlen(kick), &w, NULL);

    /* Drain the reply. ConPTY sends ESC [ row ; col R. If it doesn't
    * arrive in 100 ms, move on — the flush still happened. */
    DWORD deadline = GetTickCount() + 100;
    for (;;) {
        DWORD now = GetTickCount();
        if (now >= deadline) break;

        DWORD n = 0;
        if (!GetNumberOfConsoleInputEvents(g_in, &n) || n == 0) {
            Sleep(10);
            continue;
        }

        INPUT_RECORD rec;
        DWORD read = 0;
        if (!ReadConsoleInputA(g_in, &rec, 1, &read) || read != 1) break;

        if (rec.EventType == KEY_EVENT) {
            /* The reply comes through as individual key events, one per
            * byte. Consume them and drop them — they aren't user input. */
            continue;
        }
    }
    
    g_inited = 1;
    return 0;
}

void tui_shutdown(void) {
    if (!g_inited) return;

    const char *seq = "\x1b[?2004l\x1b[?25h\x1b[?1049l";
    DWORD w = 0;
    WriteFile(g_out, seq, (DWORD)strlen(seq), &w, NULL);

    SetConsoleMode(g_in,  g_saved_in_mode);
    SetConsoleMode(g_out, g_saved_out_mode);
    //SetConsoleCtrlHandler(winch_handler, FALSE);

    g_inited = 0;
}

int tui_rows(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(g_out, &info)) return 24;
    int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return rows > 0 ? rows : 24;
}

int tui_cols(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(g_out, &info)) return 80;
    int cols = info.srWindow.Right - info.srWindow.Left + 1;
    return cols > 0 ? cols : 80;
}

void tui_clear(void) {
    const char *seq = "\x1b[2J\x1b[H";
    DWORD w = 0;
    WriteFile(g_out, seq, (DWORD)strlen(seq), &w, NULL);
}

void tui_flush(void) { /* stdout is unbuffered via WriteFile; no-op */ }

void tui_goto(int row, int col) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    if (n > 0) {
        DWORD w = 0;
        WriteFile(g_out, buf, (DWORD)n, &w, NULL);
    }
}

void tui_hline(int row, int col0, int col1, char ch) {
    if (col1 <= col0) return;
    tui_goto(row, col0);
    char buf[256];
    int span = col1 - col0;
    while (span > 0) {
        int chunk = span > (int)sizeof(buf) ? (int)sizeof(buf) : span;
        memset(buf, ch, chunk);
        DWORD w = 0;
        WriteFile(g_out, buf, (DWORD)chunk, &w, NULL);
        span -= chunk;
    }
}

void tui_puts_at(int row, int col, const char *s) {
    int cols = tui_cols();
    tui_goto(row, col);
    int max = cols - col + 1;
    if (max <= 0) return;
    int n = (int)strlen(s);
    if (n > max) n = max;
    if (n > 0) {
        DWORD w = 0;
        WriteFile(g_out, s, (DWORD)n, &w, NULL);
    }
}

void tui_puts_at_inv(int row, int col, const char *s) {
    DWORD w = 0;
    WriteFile(g_out, "\x1b[7m", 4, &w, NULL);
    tui_puts_at(row, col, s);
    WriteFile(g_out, "\x1b[0m", 4, &w, NULL);
}

/* ---- key translation ------------------------------------------- */

/* Windows gives us KEY_EVENT records. POSIX gives us bytes. To make
 * the rest of the codebase see the same input on both platforms, we
 * buffer the bytes the current key press translates to, and return
 * them one at a time. */

static unsigned char g_keybuf[16];
static int           g_keybuf_len = 0;
static int           g_keybuf_pos = 0;

static void push_seq(const char *s) {
    size_t n = strlen(s);
    if (n > sizeof(g_keybuf)) n = sizeof(g_keybuf);
    memcpy(g_keybuf, s, n);
    g_keybuf_len = (int)n;
    g_keybuf_pos = 0;
}

int tui_try_getch(void) {
    if (g_keybuf_pos < g_keybuf_len) {
        return (int)g_keybuf[g_keybuf_pos++];
    }

    for (;;) {
        DWORD n = 0;
        if (!GetNumberOfConsoleInputEvents(g_in, &n) || n == 0) {
            return -1;
        }

        INPUT_RECORD rec;
        DWORD read = 0;
        if (!ReadConsoleInputA(g_in, &rec, 1, &read) || read != 1) {
            return -1;
        }

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            InterlockedExchange(&g_winch_flag, 1);
            continue;
        }

        if (rec.EventType != KEY_EVENT) continue;
        if (!rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        char ch = rec.Event.KeyEvent.uChar.AsciiChar;
        DWORD mods = rec.Event.KeyEvent.dwControlKeyState;

        if ((mods & LEFT_ALT_PRESSED) || (mods & RIGHT_ALT_PRESSED)) {
            if (vk >= 'A' && vk <= 'Z') {
                char buf[3];
                buf[0] = 0x1b;
                buf[1] = (char)('a' + (vk - 'A'));
                buf[2] = '\0';
                push_seq(buf);
                return (int)g_keybuf[g_keybuf_pos++];
            }
        }

        if (mods & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
            if (vk >= 'A' && vk <= 'Z') {
                return (int)('A' + (vk - 'A')) - 'A' + 1;
            }
        }

        switch (vk) {
            case VK_UP:     push_seq("\x1b[A");  break;
            case VK_DOWN:   push_seq("\x1b[B");  break;
            case VK_RIGHT:  push_seq("\x1b[C");  break;
            case VK_LEFT:   push_seq("\x1b[D");  break;
            case VK_HOME:   push_seq("\x1b[H");  break;
            case VK_END:    push_seq("\x1b[F");  break;
            case VK_PRIOR:  push_seq("\x1b[5~"); break;
            case VK_NEXT:   push_seq("\x1b[6~"); break;
            case VK_DELETE: push_seq("\x1b[3~"); break;
            case VK_INSERT: push_seq("\x1b[2~"); break;
            case VK_ESCAPE: return 0x1b;
            case VK_RETURN: return 0x0d;
            case VK_BACK:   return 0x7f;
            case VK_TAB:    return 0x09;
            case VK_SPACE:  return 0x20;
            default: break;
        }

        if (ch != 0) return (int)(unsigned char)ch;
    }
}

int tui_getch(void) {
    for (;;) {
        int c = tui_try_getch();
        if (c >= 0) return c;
        Sleep(15);
    }
}

int tui_check_resize(void) {
    if (InterlockedExchange(&g_winch_flag, 0) != 0) return 1;
    return 0;
}

void tui_puts_at_padded(int row, int col, int right_limit, const char *s) {
    if (right_limit < col) return;
    int cols = tui_cols();
    if (right_limit > cols) right_limit = cols;

    int width = right_limit - col + 1;
    int len = (int)strlen(s);
    if (len > width) len = width;

    tui_goto(row, col);
    if (len > 0) {
        DWORD w = 0;
        WriteFile(g_out, s, (DWORD)len, &w, NULL);
    }
    int pad = width - len;
    if (pad > 0) {
        char buf[256];
        while (pad > 0) {
            int chunk = pad > (int)sizeof(buf) ? (int)sizeof(buf) : pad;
            memset(buf, ' ', chunk);
            DWORD w = 0;
            WriteFile(g_out, buf, (DWORD)chunk, &w, NULL);
            pad -= chunk;
        }
    }
    DWORD w = 0;
    WriteFile(g_out, "\x1b[0m", 4, &w, NULL);
}

#else  /* POSIX */

/* ================================================================== */
/*  POSIX terminal backend — unchanged                                */
/* ================================================================== */

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

static struct termios g_saved;
static int g_inited = 0;

static volatile sig_atomic_t g_winch_flag = 0;

static void on_winch(int sig) {
    (void)sig;
    g_winch_flag = 1;
}

int tui_check_resize(void) {
    if (g_winch_flag) {
        g_winch_flag = 0;
        return 1;
    }
    return 0;
}

int tui_init(void) {
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return -1;
    struct termios raw = g_saved;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_oflag &= ~OPOST;
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;

    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

    signal(SIGWINCH, on_winch);

    /* alt screen, hide cursor, clear, enable bracketed paste */
    const char *seq = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H\x1b[?2004h";
    ssize_t w = write(STDOUT_FILENO, seq, strlen(seq));
    (void)w;
    g_inited = 1;
    return 0;
}

void tui_shutdown(void) {
    if (!g_inited) return;
    const char *seq = "\x1b[?2004l\x1b[?25h\x1b[?1049l";
    ssize_t w = write(STDOUT_FILENO, seq, strlen(seq));
    (void)w;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
    g_inited = 0;
}

int tui_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return 24;
    return ws.ws_row > 0 ? ws.ws_row : 24;
}

int tui_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return 80;
    return ws.ws_col > 0 ? ws.ws_col : 80;
}

void tui_clear(void) {
    ssize_t w = write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);
    (void)w;
}

void tui_flush(void) { /* stdout is unbuffered via write(); no-op */ }

void tui_goto(int row, int col) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    if (n > 0) { ssize_t w = write(STDOUT_FILENO, buf, n); (void)w; }
}

void tui_hline(int row, int col0, int col1, char ch) {
    if (col1 <= col0) return;
    tui_goto(row, col0);
    char buf[256];
    int span = col1 - col0;
    while (span > 0) {
        int chunk = span > (int)sizeof(buf) ? (int)sizeof(buf) : span;
        memset(buf, ch, chunk);
        ssize_t w = write(STDOUT_FILENO, buf, chunk);
        (void)w;
        span -= chunk;
    }
}

void tui_puts_at(int row, int col, const char *s) {
    int cols = tui_cols();
    tui_goto(row, col);
    int max = cols - col + 1;
    if (max <= 0) return;
    int n = (int)strlen(s);
    if (n > max) n = max;
    if (n > 0) { ssize_t w = write(STDOUT_FILENO, s, n); (void)w; }
}

void tui_puts_at_inv(int row, int col, const char *s) {
    ssize_t w;
    w = write(STDOUT_FILENO, "\x1b[7m", 4); (void)w;
    tui_puts_at(row, col, s);
    w = write(STDOUT_FILENO, "\x1b[0m", 4); (void)w;
}

int tui_try_getch(void) {
    unsigned char b;
    ssize_t r = read(STDIN_FILENO, &b, 1);
    if (r == 1) return b;
    return -1;
}

int tui_getch(void) {
    unsigned char b;
    for (;;) {
        ssize_t r = read(STDIN_FILENO, &b, 1);
        if (r == 1) return b;
        usleep(15000);
    }
}

void tui_puts_at_padded(int row, int col, int right_limit, const char *s) {
    if (right_limit < col) return;
    int cols = tui_cols();
    if (right_limit > cols) right_limit = cols;

    int width = right_limit - col + 1;
    int len = (int)strlen(s);
    if (len > width) len = width;

    ssize_t w;

    tui_goto(row, col);
    if (len > 0) {
        w = write(STDOUT_FILENO, s, len); (void)w;
    }
    int pad = width - len;
    if (pad > 0) {
        char buf[256];
        while (pad > 0) {
            int chunk = pad > (int)sizeof(buf) ? (int)sizeof(buf) : pad;
            memset(buf, ' ', chunk);
            w = write(STDOUT_FILENO, buf, chunk); (void)w;
            pad -= chunk;
        }
    }

    w = write(STDOUT_FILENO, "\x1b[0m", 4); (void)w;
}

#endif /* _WIN32 */