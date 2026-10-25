// language: C, file: wisp-code/tui.h
#ifndef TUI_H
#define TUI_H

#include <stddef.h>

int  tui_init(void);
void tui_shutdown(void);

int  tui_rows(void);
int  tui_cols(void);

void tui_clear(void);
void tui_flush(void);
void tui_goto(int row, int col);

void tui_hline(int row, int col0, int col1, char ch);
void tui_puts_at(int row, int col, const char *s);
void tui_puts_at_inv(int row, int col, const char *s);

int  tui_getch(void);        /* blocking */
int  tui_try_getch(void);    /* non-blocking, -1 if none */

// language: C, file: wisp-code/tui.h — add to the public interface
void tui_puts_at_padded(int row, int col, int right_limit, const char *s);

int tui_check_resize(void);

#endif