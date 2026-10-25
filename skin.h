// language: C, file: wisp-code/skin.h
#ifndef SKIN_H
#define SKIN_H

#include "theme.h"
#include <stddef.h>

/* Box-drawing glyphs: unchanged, chosen at compile time. */
#define BX_TL "┌"
#define BX_TR "┐"
#define BX_BL "└"
#define BX_BR "┘"
#define BX_H  "─"
#define BX_V  "│"
#define BX_LT "├"
#define BX_RT "┤"

/* ------------------------------------------------------------------ *
 *  Color slots                                                       *
 *                                                                    *
 *  Each C_* name resolves to a `const char *` holding an ANSI SGR    *
 *  sequence for the current theme's slot. Resolution happens on      *
 *  every use so a theme switch takes effect on the next frame        *
 *  without any cache invalidation.                                   *
 *                                                                    *
 *  skin_slot() rotates through four internal buffers, so a caller    *
 *  may reasonably hold two or three of these live at once (e.g.      *
 *  C_BOLD then C_ERR then text) without one overwriting another.     *
 *                                                                    *
 *  IMPORTANT: two C_* names cannot be juxtaposed into one longer     *
 *  literal. `C_BOLD C_ACCENT` was valid when both were string        *
 *  literals; it is a syntax error now that they are function calls.  *
 *  Use emit_all(C_BOLD, C_ACCENT, NULL) or two separate write()      *
 *  calls instead.                                                    *
 *                                                                    *
 *  Under WISP_NO_COLOR every name expands to the empty string and   *
 *  the functions still compile — theme_fg returns 0 and we hand back *
 *  "".                                                                *
 * ------------------------------------------------------------------ */

#ifdef WISP_NO_COLOR
  #define C_RESET  ""
  #define C_DIM    ""
  #define C_BOLD   ""
  #define C_ITALIC ""
  #define C_BG     ""
  #define C_BORDER ""
  #define C_SPLIT  ""
  #define C_TITLE  ""
  #define C_ACCENT ""
  #define C_PROMPT ""
  #define C_PATH   ""
  #define C_OK     ""
  #define C_ERR    ""
  #define C_WARN   ""
  #define C_INFO   ""
  #define C_MUTED  ""
  #define C_USER   ""
  #define C_AI     ""
  #define C_SYSTEM ""
  #define C_DATE   ""
  #define C_LINENO ""
  #define C_HINT   ""
  #define C_HDR_BG ""
#else
  /* text attributes are theme-independent */
  #define C_RESET  "\x1b[0m"
  #define C_DIM    "\x1b[2m"
  #define C_BOLD   "\x1b[1m"
  #define C_ITALIC "\x1b[3m"

  /* slot-driven — resolved at every use through theme.c */
  #define C_BG     skin_slot(TH_BG)
  #define C_BORDER skin_slot(TH_BORDER)
  #define C_SPLIT  skin_slot(TH_SPLIT)
  #define C_TITLE  skin_slot(TH_TITLE)
  #define C_ACCENT skin_slot(TH_ACCENT)
  #define C_PROMPT skin_slot(TH_PROMPT)
  #define C_PATH   skin_slot(TH_PATH)
  #define C_OK     skin_slot(TH_OK)
  #define C_ERR    skin_slot(TH_ERR)
  #define C_WARN   skin_slot(TH_WARN)
  #define C_INFO   skin_slot(TH_INFO)
  #define C_MUTED  skin_slot(TH_MUTED)
  #define C_USER   skin_slot(TH_USER)
  #define C_AI     skin_slot(TH_AI)
  #define C_SYSTEM skin_slot(TH_SYSTEM)
  #define C_DATE   skin_slot(TH_PATH)     /* no separate slot — reuse */
  #define C_LINENO skin_slot(TH_LINENO)
  #define C_HINT   skin_slot(TH_HINT)
  #define C_HDR_BG ""                     /* reserved, unused */
#endif

#endif /* wisp_SKIN_H */