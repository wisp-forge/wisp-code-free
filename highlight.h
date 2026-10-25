// language: C, file: wisp-code/highlight.h
#ifndef HIGHLIGHT_H
#define HIGHLIGHT_H

#include <stddef.h>

/* ================================================================== */
/*  Syntax highlighting — line-oriented, state-carrying              */
/*                                                                    */
/*  Scope: comment / string / keyword / number / type / preprocessor. */
/*  Nothing structural. No tree, no AST, no grammar. The tokenizer    */
/*  is deliberately forgiving: worst case a token stays uncolored,    */
/*  never wrong-colored. That is the whole design constraint — a      */
/*  miscolored identifier in a 200-line dump looks like a bug, an    */
/*  uncolored one looks like nothing.                                 */
/*                                                                    */
/*  The caller drives the tokenizer one wrapped body line at a time,  */
/*  matching console.c's existing body_next_line() loop. State that   */
/*  has to survive a line boundary (block comments, triple-quoted     */
/*  strings) lives in the opaque handle.                              */
/*                                                                    */
/*  Colors are pulled from the active theme's slots, so the           */
/*  highlighter inherits whatever theme is selected — no hardcoded    */
/*  ANSI anywhere in here.                                            */
/* ================================================================== */

typedef enum {
    HL_LANG_NONE = 0,
    HL_LANG_C,          /* .c .h .m .mm .conf .cfg .ini               */
    HL_LANG_CPP,        /* .cc .cpp .cxx .hpp .hh .cmake              */
    HL_LANG_PY,         /* .py                                        */
    HL_LANG_JS,         /* .js .jsx .ts .tsx                          */
    HL_LANG_GO,         /* .go                                        */
    HL_LANG_RUST,       /* .rs                                        */
    HL_LANG_JAVA,       /* .java .kt                                  */
    HL_LANG_RUBY,       /* .rb                                        */
    HL_LANG_SH,         /* .sh .bash .zsh .mk .make .run              */
    HL_LANG_JSON,       /* .json                                      */
    HL_LANG_YAML,       /* .yaml .yml .toml                           */
    HL_LANG_HTML,       /* .html                                      */
    HL_LANG_CSS,        /* .css .scss                                 */
    HL_LANG_MD,         /* .md .txt                                   */
    HL_LANG_SQL,        /* .sql                                       */
    HL_LANG_PHP,        /* .php                                       */
    HL_LANG_PERL,       /* .pl                                        */
    HL_LANG_LUA,        /* .lua                                       */
    HL_LANG_R,          /* .r                                         */
    HL_LANG_SWIFT,      /* .swift                                     */
} hl_lang_t;

/* Pick a language from a file path (extension-based). Returns
 * HL_LANG_NONE if the extension is unrecognized or the path is NULL. */
hl_lang_t hl_lang_for_path(const char *path);

/* Allocate / free the per-section tokenizer state. `lang` is fixed
 * for the lifetime of the handle. Returns NULL on allocation failure
 * or when lang == HL_LANG_NONE (caller should fall back to plain
 * output). */
void *hl_state_new(hl_lang_t lang);
void  hl_state_free(void *state);

/* Emit one line of source, tokenized and colored, to stdout.
 *
 * Writes ANSI sequences directly, matching the convention used by
 * draw_section(). Caller is responsible for positioning the cursor
 * first; this function writes exactly the bytes of the line, with
 * color transitions inline, and always ends with C_RESET.
 *
 * `line` is not necessarily NUL-terminated — `len` is authoritative.
 * A single trailing '\n' in the line is honored (emitted literally),
 * which lets the caller pass body_next_line()'s output unchanged.
 *
 * State is updated in place; the next call resumes mid-token if the
 * previous line ended inside a block comment or triple-quoted
 * string. */
void hl_emit_line(void *state, const char *line, int len, int sink);

/* ================================================================== */
/*  Diff rendering — for %%%EDIT OLD/NEW bodies                       */
/*                                                                    */
/*  Separate from the tokenizer because the input is already           */
/*  structured. `<<< OLD` / `>>> NEW` / `===` markers are detected by  */
/*  prefix; lines under OLD are tinted red, under NEW green, and the   */
/*  markers themselves are dim structural chrome. No tokenizing —      */
/*  the diff is meant to be scanned, not read.                        */
/* ================================================================== */

typedef enum {
    HL_DIFF_NEUTRAL = 0,   /* outside any section                    */
    HL_DIFF_OLD,           /* inside an OLD block                    */
    HL_DIFF_NEW,           /* inside a NEW block                     */
} hl_diff_state_t;

/* Emit one line of a diff body. `state` is updated in place: the
 * caller passes a pointer to a hl_diff_state_t initialized to
 * HL_DIFF_NEUTRAL and calls this once per line. Writes the line
 * verbatim (no re-wrapping), with a leading 2-space marker and the
 * appropriate color. Always ends with C_RESET. */
void hl_emit_diff_line(hl_diff_state_t *state, void *hl,
                       const char *line, int len, int sink);


#endif /* HIGHLIGHT_H */