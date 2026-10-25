// language: C, file: wisp-code/highlight.c

#include "compat/posix.h"

#include "highlight.h"
#include "skin.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ================================================================== */
/*  Low-level writer                                                  */
/* ================================================================== */

static int g_sink = 0;

static void w(const char *s) {
    if (g_sink || !s || !s[0]) return;
    ssize_t n = write(STDOUT_FILENO, s, ( unsigned int )strlen(s));
    (void)n;
}

static void wn(const char *s, int len) {
    if (g_sink || !s || len <= 0) return;
    ssize_t n = write(STDOUT_FILENO, s, (size_t)len);
    (void)n;
}

/* Rotating slot accessor — same trick as skin_slot(), but local so a
 * single hl_emit_line() can hold several theme colors live at once
 * without them clobbering each other. skin_slot() rotates through 4
 * buffers, which is not enough here (keyword + string + comment +
 * number can all be live). */
static const char *slot(theme_slot_t s) {
    static char bufs[8][24];
    static int  next = 0;
    char *b = bufs[next];
    next = (next + 1) & 7;
    if (theme_fg(s, b, sizeof(bufs[0])) == 0) b[0] = '\0';
    return b;
}

/* ================================================================== */
/*  Extension -> language                                             */
/* ================================================================== */

typedef struct {
    const char *ext;
    hl_lang_t   lang;
} ext_map_t;

static const ext_map_t EXT_MAP[] = {
    /* C and friends */
    { ".c",     HL_LANG_C     },
    { ".h",     HL_LANG_C     },
    { ".m",     HL_LANG_C     },
    { ".mm",    HL_LANG_C     },
    { ".conf",  HL_LANG_C     },
    { ".cfg",   HL_LANG_C     },
    { ".ini",   HL_LANG_C     },
    { ".cc",    HL_LANG_CPP   },
    { ".cpp",   HL_LANG_CPP   },
    { ".cxx",   HL_LANG_CPP   },
    { ".hpp",   HL_LANG_CPP   },
    { ".hh",    HL_LANG_CPP   },
    { ".cmake", HL_LANG_CPP   },

    { ".py",    HL_LANG_PY    },

    { ".js",    HL_LANG_JS    },
    { ".jsx",   HL_LANG_JS    },
    { ".ts",    HL_LANG_JS    },
    { ".tsx",   HL_LANG_JS    },

    { ".go",    HL_LANG_GO    },
    { ".rs",    HL_LANG_RUST  },
    { ".java",  HL_LANG_JAVA  },
    { ".kt",    HL_LANG_JAVA  },
    { ".rb",    HL_LANG_RUBY  },

    { ".sh",    HL_LANG_SH    },
    { ".bash",  HL_LANG_SH    },
    { ".zsh",   HL_LANG_SH    },
    { ".mk",    HL_LANG_SH    },
    { ".make",  HL_LANG_SH    },
    { ".run",   HL_LANG_SH    },

    { ".json",  HL_LANG_JSON  },
    { ".yaml",  HL_LANG_YAML  },
    { ".yml",   HL_LANG_YAML  },
    { ".toml",  HL_LANG_YAML  },

    { ".html",  HL_LANG_HTML  },
    { ".css",   HL_LANG_CSS   },
    { ".scss",  HL_LANG_CSS   },

    { ".md",    HL_LANG_MD    },
    { ".txt",   HL_LANG_MD    },

    { ".sql",   HL_LANG_SQL   },
    { ".php",   HL_LANG_PHP   },
    { ".pl",    HL_LANG_PERL  },
    { ".lua",   HL_LANG_LUA   },
    { ".r",     HL_LANG_R     },
    { ".swift", HL_LANG_SWIFT },

    { NULL, 0 }
};

hl_lang_t hl_lang_for_path(const char *path) {
    if (!path || !path[0]) return HL_LANG_NONE;
    const char *dot = strrchr(path, '.');
    if (!dot) return HL_LANG_NONE;

    for (int i = 0; EXT_MAP[i].ext; i++) {
        if (strcasecmp(dot, EXT_MAP[i].ext) == 0) return EXT_MAP[i].lang;
    }
    return HL_LANG_NONE;
}

/* ================================================================== */
/*  Per-language keyword tables                                       */
/*                                                                    */
/*  Kept minimal. Anything a reasonable reader would highlight,       */
/*  nothing a compiler needs. Sorted is not required — the lookup is  */
/*  a linear scan over a small table and the containing function is   */
/*  called once per identifier.                                       */
/*                                                                    */
/*  MAX_KW is generous headroom: the largest table is SQL at 64,      */
/*  the next is C++ at ~110. If a table ever needs 200+ entries,      */
/*  stop and reconsider — a table that large is a sign the            */
/*  highlighter is overreaching.                                      */
/* ================================================================== */

#define MAX_KW 160

typedef struct {
    const char *kw[MAX_KW];
    int         n;
} kw_set_t;

static int kw_set_has(const kw_set_t *ks, const char *word, int len,
                      int case_insensitive) {
    if (!ks || !word) return 0;
    if (len <= 0 || len > 32) return 0;

    char buf[33];
    memcpy(buf, word, (size_t)len);
    buf[len] = '\0';

    for (int i = 0; i < ks->n; i++) {
        const char *kw = ks->kw[i];
        if (!kw) continue;
        int hit = case_insensitive
                  ? (strcasecmp(kw, buf) == 0)
                  : (strcmp(kw, buf) == 0);
        if (hit) return 1;
    }
    return 0;
}

static const kw_set_t KW_C = {{
    "auto","break","case","char","const","continue","default","do",
    "double","else","enum","extern","float","for","goto","if","inline",
    "int","long","register","restrict","return","short","signed",
    "sizeof","static","struct","switch","typedef","union","unsigned",
    "void","volatile","while","_Bool","_Complex","_Atomic","_Static_assert",
    "_Thread_local","NULL","true","false",
    "size_t","ssize_t","int8_t","int16_t","int32_t","int64_t",
    "uint8_t","uint16_t","uint32_t","uint64_t","bool",
    "FILE","va_list","ptrdiff_t","wchar_t",
}, 62};

static const kw_set_t KW_CPP = {{
    "alignas","alignof","and","and_eq","asm","bitand","bitor","bool",
    "catch","char16_t","char32_t","class","compl","concept","consteval",
    "constexpr","constinit","const_cast","co_await","co_return","co_yield",
    "decltype","delete","dynamic_cast","explicit","export","false",
    "friend","mutable","namespace","new","noexcept","not","not_eq",
    "nullptr","operator","or","or_eq","private","protected","public",
    "reinterpret_cast","requires","static_assert","static_cast",
    "template","this","thread_local","throw","true","try","typeid",
    "typename","using","virtual","wchar_t","xor","xor_eq",
    "auto","break","case","char","const","continue","default","do",
    "double","else","enum","extern","float","for","goto","if","inline",
    "int","long","register","return","short","signed","sizeof",
    "static","struct","switch","typedef","union","unsigned","void",
    "volatile","while",
}, 80};

static const kw_set_t KW_PY = {{
    "and","as","assert","async","await","break","class","continue",
    "def","del","elif","else","except","False","finally","for","from",
    "global","if","import","in","is","lambda","None","nonlocal","not",
    "or","pass","raise","return","True","try","while","with","yield",
    "match","case","self","cls","__init__","__name__","__main__",
}, 46};

static const kw_set_t KW_JS = {{
    "async","await","break","case","catch","class","const","continue",
    "debugger","default","delete","do","else","export","extends",
    "false","finally","for","function","if","import","in","instanceof",
    "let","new","null","of","return","static","super","switch","this",
    "throw","true","try","typeof","undefined","var","void","while",
    "with","yield","interface","type","enum","implements","private",
    "protected","public","readonly","declare","namespace","abstract",
    "as","from","get","set","any","string","number","boolean","symbol",
}, 60};

static const kw_set_t KW_GO = {{
    "break","case","chan","const","continue","default","defer","else",
    "fallthrough","for","func","go","goto","if","import","interface",
    "map","package","range","return","select","struct","switch","type",
    "var","nil","true","false","iota",
    "int","int8","int16","int32","int64","uint","uint8","uint16",
    "uint32","uint64","uintptr","float32","float64","complex64",
    "complex128","string","bool","byte","rune","error","any",
    "make","new","len","cap","append","copy","delete","panic","recover",
}, 55};

static const kw_set_t KW_RUST = {{
    "as","async","await","break","const","continue","crate","dyn",
    "else","enum","extern","false","fn","for","if","impl","in","let",
    "loop","match","mod","move","mut","pub","ref","return","self",
    "Self","static","struct","super","trait","true","type","unsafe",
    "use","where","while","yield","box","try",
    "i8","i16","i32","i64","i128","isize","u8","u16","u32","u64",
    "u128","usize","f32","f64","bool","char","str","String","Vec",
    "Option","Result","Some","None","Ok","Err","Box","Rc","Arc",
}, 62};

static const kw_set_t KW_JAVA = {{
    "abstract","assert","boolean","break","byte","case","catch","char",
    "class","const","continue","default","do","double","else","enum",
    "extends","final","finally","float","for","goto","if","implements",
    "import","instanceof","int","interface","long","native","new",
    "package","private","protected","public","return","short","static",
    "strictfp","super","switch","synchronized","this","throw","throws",
    "transient","try","void","volatile","while","true","false","null",
    "var","record","sealed","permits","yield","fun","val",
}, 58};

static const kw_set_t KW_RUBY = {{
    "BEGIN","END","alias","and","begin","break","case","class","def",
    "defined?","do","else","elsif","end","ensure","false","for","if",
    "in","module","next","nil","not","or","redo","rescue","retry",
    "return","self","super","then","true","undef","unless","until",
    "when","while","yield","require","require_relative","attr_accessor",
    "attr_reader","attr_writer","puts","print","p",
}, 46};

static const kw_set_t KW_SH = {{
    "if","then","else","elif","fi","for","while","until","do","done",
    "case","esac","in","function","return","exit","break","continue",
    "local","export","readonly","declare","typeset","unset","set",
    "source","alias","unalias","shift","trap","eval","exec","read",
    "echo","printf","cd","pwd","true","false",
    "sudo","env","make","grep","sed","awk","cat","ls","rm","cp","mv",
    "mkdir","rmdir","touch","chmod","chown","find","xargs","tee",
}, 62};

/* SQL is case-insensitive: kw_set_has() is called with ci=1 for this
 * language, so a single uppercase table covers both `select` and
 * `SELECT`. The lowercase mirror that used to live here was pure
 * duplication and pushed the table past MAX_KW. */
static const kw_set_t KW_SQL = {{
    "SELECT","FROM","WHERE","INSERT","INTO","VALUES","UPDATE","SET",
    "DELETE","CREATE","TABLE","INDEX","VIEW","DROP","ALTER","ADD",
    "COLUMN","PRIMARY","KEY","FOREIGN","REFERENCES","UNIQUE","NOT",
    "NULL","DEFAULT","CHECK","JOIN","INNER","LEFT","RIGHT","OUTER",
    "FULL","ON","AS","AND","OR","IN","LIKE","BETWEEN","IS","EXISTS",
    "GROUP","BY","ORDER","HAVING","LIMIT","OFFSET","UNION","ALL",
    "DISTINCT","CASE","WHEN","THEN","ELSE","END","COUNT","SUM","AVG",
    "MIN","MAX","ASC","DESC",
}, 64};

static const kw_set_t KW_PHP = {{
    "abstract","and","array","as","break","callable","case","catch",
    "class","clone","const","continue","declare","default","die","do",
    "echo","else","elseif","empty","enddeclare","endfor","endforeach",
    "endif","endswitch","endwhile","eval","exit","extends","final",
    "finally","fn","for","foreach","function","global","goto","if",
    "implements","include","include_once","instanceof","insteadof",
    "interface","isset","list","match","namespace","new","or","print",
    "private","protected","public","readonly","require","require_once",
    "return","static","switch","throw","trait","try","unset","use",
    "var","while","xor","yield","true","false","null","int","float",
    "string","bool","void","mixed","object","self","parent",
}, 70};

static const kw_set_t KW_PERL = {{
    "my","our","local","state","sub","use","require","package","if",
    "elsif","else","unless","while","until","for","foreach","do",
    "last","next","redo","return","die","warn","print","say","defined",
    "undef","and","or","not","eq","ne","lt","gt","le","ge","cmp",
}, 40};

static const kw_set_t KW_LUA = {{
    "and","break","do","else","elseif","end","false","for","function",
    "goto","if","in","local","nil","not","or","repeat","return","then",
    "true","until","while","self",
    "print","pairs","ipairs","type","tostring","tonumber","require",
}, 30};

static const kw_set_t KW_R = {{
    "if","else","for","while","repeat","function","return","in","next",
    "break","TRUE","FALSE","NULL","Inf","NaN","NA","NA_integer_",
    "NA_real_","NA_complex_","NA_character_","library","require",
}, 22};

static const kw_set_t KW_SWIFT = {{
    "associatedtype","class","deinit","enum","extension","fileprivate",
    "func","import","init","inout","internal","let","open","operator",
    "private","protocol","public","rethrows","static","struct",
    "subscript","typealias","var","break","case","continue","default",
    "defer","do","else","fallthrough","for","guard","if","in","repeat",
    "return","switch","where","while","as","catch","false","is","nil",
    "super","self","Self","throw","throws","true","try","weak",
    "unowned","Some","None","String","Int","Double","Bool","Array",
    "Dictionary","Set",
}, 62};

static const kw_set_t KW_MD = {{ "" }, 0};   /* headings handled inline */
static const kw_set_t KW_JSON = {{ "true","false","null" }, 3};
static const kw_set_t KW_YAML = {{ "true","false","null","yes","no","on","off","~" }, 8};
static const kw_set_t KW_CSS  = {{ "" }, 0};   /* property: handled inline */
static const kw_set_t KW_HTML = {{ "" }, 0};   /* tag: handled inline */
static const kw_set_t KW_NONE = {{ "" }, 0};

static const kw_set_t *kw_for_lang(hl_lang_t lang) {
    switch (lang) {
        case HL_LANG_C:      return &KW_C;
        case HL_LANG_CPP:    return &KW_CPP;
        case HL_LANG_PY:     return &KW_PY;
        case HL_LANG_JS:     return &KW_JS;
        case HL_LANG_GO:     return &KW_GO;
        case HL_LANG_RUST:   return &KW_RUST;
        case HL_LANG_JAVA:   return &KW_JAVA;
        case HL_LANG_RUBY:   return &KW_RUBY;
        case HL_LANG_SH:     return &KW_SH;
        case HL_LANG_SQL:    return &KW_SQL;
        case HL_LANG_PHP:    return &KW_PHP;
        case HL_LANG_PERL:   return &KW_PERL;
        case HL_LANG_LUA:    return &KW_LUA;
        case HL_LANG_R:      return &KW_R;
        case HL_LANG_SWIFT:  return &KW_SWIFT;
        case HL_LANG_JSON:   return &KW_JSON;
        case HL_LANG_YAML:   return &KW_YAML;
        case HL_LANG_MD:     return &KW_MD;
        case HL_LANG_CSS:    return &KW_CSS;
        case HL_LANG_HTML:   return &KW_HTML;
        default:             return &KW_NONE;
    }
}

/* ================================================================== */
/*  Comment/string syntax per language                                */
/* ================================================================== */

typedef struct {
    /* line comment markers */
    const char *line_comment[3];     /* NULL-terminated list */
    /* block comment delimiters, or NULL */
    const char *block_open;
    const char *block_close;
    /* string delimiters — single and double for most languages,
     * backtick for JS/Go/Rust raw strings */
    int         has_single;
    int         has_double;
    int         has_backtick;
    /* Python triple-quote support */
    int         has_triple;
} lang_syntax_t;

static const lang_syntax_t SYN_C = {
    { "//", NULL, NULL }, "/*", "*/", 1, 1, 0, 0
};
static const lang_syntax_t SYN_CPP = {
    { "//", NULL, NULL }, "/*", "*/", 1, 1, 0, 0
};
static const lang_syntax_t SYN_PY = {
    { "#", NULL, NULL }, NULL, NULL, 1, 1, 0, 1
};
static const lang_syntax_t SYN_JS = {
    { "//", NULL, NULL }, "/*", "*/", 1, 1, 1, 0
};
static const lang_syntax_t SYN_GO = {
    { "//", NULL, NULL }, "/*", "*/", 0, 1, 1, 0
};
static const lang_syntax_t SYN_RUST = {
    { "//", NULL, NULL }, "/*", "*/", 0, 1, 0, 0
};
static const lang_syntax_t SYN_JAVA = {
    { "//", NULL, NULL }, "/*", "*/", 1, 1, 0, 0
};
static const lang_syntax_t SYN_RUBY = {
    { "#", NULL, NULL }, NULL, NULL, 1, 1, 0, 0
};
static const lang_syntax_t SYN_SH = {
    { "#", NULL, NULL }, NULL, NULL, 1, 1, 0, 0
};
static const lang_syntax_t SYN_SQL = {
    { "--", NULL, NULL }, "/*", "*/", 1, 1, 0, 0
};
static const lang_syntax_t SYN_PHP = {
    { "//", "#", NULL }, "/*", "*/", 1, 1, 0, 0
};
static const lang_syntax_t SYN_PERL = {
    { "#", NULL, NULL }, NULL, NULL, 1, 1, 0, 0
};
static const lang_syntax_t SYN_LUA = {
    { "--", NULL, NULL }, NULL, NULL, 1, 1, 0, 0
};
static const lang_syntax_t SYN_R = {
    { "#", NULL, NULL }, NULL, NULL, 1, 1, 0, 0
};
static const lang_syntax_t SYN_SWIFT = {
    { "//", NULL, NULL }, "/*", "*/", 0, 1, 0, 0
};
static const lang_syntax_t SYN_JSON = {
    { NULL, NULL, NULL }, NULL, NULL, 0, 1, 0, 0
};
static const lang_syntax_t SYN_YAML = {
    { "#", NULL, NULL }, NULL, NULL, 0, 1, 0, 0
};
static const lang_syntax_t SYN_MD = {
    { NULL, NULL, NULL }, NULL, NULL, 0, 0, 1, 0
};
static const lang_syntax_t SYN_CSS = {
    { "/*", NULL, NULL }, "/*", "*/", 0, 1, 0, 0
};
static const lang_syntax_t SYN_HTML = {
    { NULL, NULL, NULL }, "<!--", "-->", 0, 1, 0, 0
};
static const lang_syntax_t SYN_NONE = {
    { NULL, NULL, NULL }, NULL, NULL, 0, 0, 0, 0
};

static const lang_syntax_t *syn_for_lang(hl_lang_t lang) {
    switch (lang) {
        case HL_LANG_C:      return &SYN_C;
        case HL_LANG_CPP:    return &SYN_CPP;
        case HL_LANG_PY:     return &SYN_PY;
        case HL_LANG_JS:     return &SYN_JS;
        case HL_LANG_GO:     return &SYN_GO;
        case HL_LANG_RUST:   return &SYN_RUST;
        case HL_LANG_JAVA:   return &SYN_JAVA;
        case HL_LANG_RUBY:   return &SYN_RUBY;
        case HL_LANG_SH:     return &SYN_SH;
        case HL_LANG_SQL:    return &SYN_SQL;
        case HL_LANG_PHP:    return &SYN_PHP;
        case HL_LANG_PERL:   return &SYN_PERL;
        case HL_LANG_LUA:    return &SYN_LUA;
        case HL_LANG_R:      return &SYN_R;
        case HL_LANG_SWIFT:  return &SYN_SWIFT;
        case HL_LANG_JSON:   return &SYN_JSON;
        case HL_LANG_YAML:   return &SYN_YAML;
        case HL_LANG_MD:     return &SYN_MD;
        case HL_LANG_CSS:    return &SYN_CSS;
        case HL_LANG_HTML:   return &SYN_HTML;
        default:             return &SYN_NONE;
    }
}

/* ================================================================== */
/*  Tokenizer state                                                   */
/* ================================================================== */

typedef enum {
    TS_NORMAL = 0,
    TS_BLOCK_COMMENT,
    TS_TRIPLE_SINGLE,
    TS_TRIPLE_DOUBLE,
} ts_mode_t;

typedef struct {
    hl_lang_t          lang;
    const kw_set_t    *kw;
    const lang_syntax_t *syn;
    ts_mode_t           mode;
} hl_state_t;

void *hl_state_new(hl_lang_t lang) {
    if (lang == HL_LANG_NONE) return NULL;
    hl_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->lang = lang;
    s->kw   = kw_for_lang(lang);
    s->syn  = syn_for_lang(lang);
    s->mode = TS_NORMAL;
    return s;
}

void hl_state_free(void *state) {
    free(state);
}

/* ================================================================== */
/*  Token emission                                                    */
/* ================================================================== */

static void emit_kw(void)   { w(slot(TH_ACCENT)); w(C_BOLD); }
static void emit_str(void)  { w(slot(TH_OK)); }
static void emit_com(void)  { w(slot(TH_MUTED)); w(C_ITALIC); }
static void emit_num(void)  { w(slot(TH_WARN)); }
static void emit_type(void) { w(slot(TH_TITLE)); }
static void emit_pre(void)  { w(slot(TH_PROMPT)); }
static void emit_id(void)   { w(C_INFO); }

/* ================================================================== */
/*  Language-specific inline recognizers                              */
/*                                                                    */
/*  These run at the start of a line, before the generic tokenizer.   */
/*  They only fire when the line clearly is what they expect; if the  */
/*  detection is even slightly uncertain, they return 0 and the       */
/*  generic path handles it.                                          */
/* ================================================================== */

/* Markdown heading: line starts with 1-6 '#' followed by space. */
static int try_md_heading(const char *p, int len, int *consumed) {
    int i = 0;
    while (i < len && p[i] == '#') i++;
    if (i == 0 || i > 6) return 0;
    if (i < len && p[i] != ' ') return 0;
    *consumed = i;
    return 1;
}

/* CSS: a property name at the start of a line, followed by ':',
 * stopping at the first ':' — and the line does not start with a
 * selector-ish character. Conservative: only fires when the line is
 * indented or starts with a letter, and the colon appears before any
 * '{' or ';'. */
static int try_css_property(const char *p, int len, int *prop_len) {
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    int start = i;
    while (i < len &&
           (isalnum((unsigned char)p[i]) || p[i] == '-' || p[i] == '_')) {
        i++;
    }
    if (i == start) return 0;
    if (i >= len || p[i] != ':') return 0;
    /* reject if the segment before ':' contains '{' — that's a
     * selector, not a property */
    for (int k = start; k < i; k++) {
        if (p[k] == '{') return 0;
    }
    *prop_len = i - start;
    return 1;
}

/* HTML / XML: a line that starts (after indent) with '<' and a
 * letter, '/', or '!'. Highlights the tag name and any attribute
 * names up to the first '>'. Minimal — no attribute-value parsing
 * beyond the string tokenizer's normal handling. */
static int try_html_tag(const char *p, int len, int *tag_start, int *tag_len) {
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i >= len || p[i] != '<') return 0;
    i++;
    if (i < len && (p[i] == '/' || p[i] == '!' || p[i] == '?')) i++;
    int s = i;
    while (i < len &&
           (isalnum((unsigned char)p[i]) || p[i] == '-' || p[i] == '_')) {
        i++;
    }
    if (i == s) return 0;
    *tag_start = s;
    *tag_len = i - s;
    return 1;
}

/* ================================================================== */
/*  Main line emitter                                                 */
/* ================================================================== */

/* Recognized token classes, used only to decide when to leave the
 * current color. Each emits its own color reset before the next. */
static void emit_sep(void) { w(C_RESET); }

void hl_emit_line(void *state, const char *line, int len, int sink) {
    if (!line) return;
    if (len < 0) len = (int)strlen(line);
    if (len == 0) return;

    int prev_sink = g_sink;
    g_sink = sink;

    /* No state — plain output, no color. */
    if (!state) { wn(line, len); g_sink = prev_sink; return; }

    hl_state_t *s = (hl_state_t *)state;

    /* State-carried: we are inside a block comment or triple string
     * from the previous line. Scan forward for the closer; if not
     * found, the whole line is comment/string. */
    if (s->mode != TS_NORMAL) {
        const char *closer = NULL;
        int closer_len = 0;

        if (s->mode == TS_BLOCK_COMMENT) {
            closer = s->syn->block_close;
            closer_len = closer ? (int)strlen(closer) : 0;
        } else if (s->mode == TS_TRIPLE_SINGLE) {
            closer = "'''";
            closer_len = 3;
        } else if (s->mode == TS_TRIPLE_DOUBLE) {
            closer = "\"\"\"";
            closer_len = 3;
        }

        if (!closer) {
            /* Shouldn't happen — reset defensively. */
            s->mode = TS_NORMAL;
        } else {
            int found = -1;
            for (int i = 0; i + closer_len <= len; i++) {
                if (memcmp(line + i, closer, (size_t)closer_len) == 0) {
                    found = i;
                    break;
                }
            }

            if (found < 0) {
                emit_com();
                wn(line, len);
                emit_sep();
                g_sink = prev_sink;
                return;
            }

            emit_com();
            wn(line, found + closer_len);
            emit_sep();

            /* Recurse on the remainder as normal text, preserving
             * the sink setting. */
            s->mode = TS_NORMAL;
            hl_emit_line(state, line + found + closer_len,
                         len - found - closer_len, sink);
            g_sink = prev_sink;
            return;
        }
    }

    /* Language-specific line preludes. Each tries to consume a
     * prefix and emit it in a special color. If none fires, the
     * generic tokenizer runs from position 0. */
    int i = 0;

    if (s->lang == HL_LANG_MD) {
        int hashes = 0;
        if (try_md_heading(line, len, &hashes)) {
            emit_kw();
            wn(line, hashes);
            emit_sep();
            i = hashes;
        }
    } else if (s->lang == HL_LANG_CSS) {
        int plen = 0;
        if (try_css_property(line, len, &plen)) {
            int indent = 0;
            while (indent < len && (line[indent] == ' ' || line[indent] == '\t'))
                indent++;
            wn(line, indent);
            emit_type();
            wn(line + indent, plen);
            emit_sep();
            i = indent + plen;
        }
    } else if (s->lang == HL_LANG_HTML) {
        int ts = 0, tl = 0;
        if (try_html_tag(line, len, &ts, &tl)) {
            wn(line, ts);
            emit_kw();
            wn(line + ts, tl);
            emit_sep();
            i = ts + tl;
        }
    }

    /* Generic tokenizer. Walks bytes; may look ahead up to 3 bytes. */
    while (i < len) {
        char c = line[i];

        /* Block comment opener. */
        const char *bo = s->syn->block_open;
        if (bo) {
            int bl = (int)strlen(bo);
            if (i + bl <= len && memcmp(line + i, bo, (size_t)bl) == 0) {
                /* Look for the closer on the same line. */
                const char *bc = s->syn->block_close;
                int cl = bc ? (int)strlen(bc) : 0;
                int end = -1;
                for (int k = i + bl; k + cl <= len; k++) {
                    if (memcmp(line + k, bc, (size_t)cl) == 0) {
                        end = k;
                        break;
                    }
                }
                if (end >= 0) {
                    emit_com();
                    wn(line + i, end + cl - i);
                    emit_sep();
                    i = end + cl;
                    continue;
                } else {
                    emit_com();
                    wn(line + i, len - i);
                    emit_sep();
                    s->mode = TS_BLOCK_COMMENT;
                    g_sink = prev_sink;
                    return;
                }
            }
        }

        /* Line comment opener. */
        for (int m = 0; s->syn->line_comment[m]; m++) {
            const char *lc = s->syn->line_comment[m];
            int lcl = (int)strlen(lc);
            if (i + lcl <= len && memcmp(line + i, lc, (size_t)lcl) == 0) {
                /* Special case: SQL and others use "--", but CSS
                 * uses slash-star only and doesn't reach here. Also
                 * guard: in C, a line comment marker inside a string
                 * is already consumed. */
                emit_com();
                wn(line + i, len - i);
                emit_sep();
                g_sink = prev_sink;
                return;
            }
        }

        /* Python triple-quoted strings. */
        if (s->syn->has_triple) {
            if (i + 3 <= len &&
                ((line[i] == '\'' && line[i+1] == '\'' && line[i+2] == '\'') ||
                 (line[i] == '"'  && line[i+1] == '"'  && line[i+2] == '"'))) {
                char q = line[i];
                const char *closer = (q == '\'') ? "'''" : "\"\"\"";
                int end = -1;
                for (int k = i + 3; k + 3 <= len; k++) {
                    if (memcmp(line + k, closer, 3) == 0) {
                        end = k;
                        break;
                    }
                }
                if (end >= 0) {
                    emit_str();
                    wn(line + i, end + 3 - i);
                    emit_sep();
                    i = end + 3;
                    continue;
                } else {
                    emit_str();
                    wn(line + i, len - i);
                    emit_sep();
                    s->mode = (q == '\'') ? TS_TRIPLE_SINGLE : TS_TRIPLE_DOUBLE;
                    g_sink = prev_sink;
                    return;
                }
            }
        }

        /* String literals. */
        int is_squote = (c == '\'') && s->syn->has_single;
        int is_dquote = (c == '"')  && s->syn->has_double;
        int is_bquote = (c == '`')  && s->syn->has_backtick;

        if (is_squote || is_dquote || is_bquote) {
            char quote = c;
            int j = i + 1;
            while (j < len) {
                if (line[j] == '\\' && j + 1 < len) { j += 2; continue; }
                if (line[j] == quote) { j++; break; }
                j++;
            }
            emit_str();
            wn(line + i, j - i);
            emit_sep();
            i = j;
            continue;
        }

        /* Preprocessor directive (C family). */
        if (s->lang == HL_LANG_C || s->lang == HL_LANG_CPP) {
            if (c == '#' && (i == 0 ||
                (i > 0 && (line[i-1] == '\n' || line[i-1] == ' ')))) {
                /* Only color the directive word, not the whole line. */
                int j = i + 1;
                while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
                int s0 = j;
                while (j < len && (isalpha((unsigned char)line[j]) || line[j] == '_'))
                    j++;
                if (j > s0) {
                    emit_pre();
                    wn(line + s0, j - s0);
                    emit_sep();
                    i = j;
                    continue;
                }
            }
        }

        /* Numbers. Recognize decimal, hex, octal, binary, floats,
         * and trailing suffix letters (u, l, f, etc.). */
        if (isdigit((unsigned char)c)) {
            int j = i;
            if (c == '0' && j + 1 < len &&
                (line[j+1] == 'x' || line[j+1] == 'X' ||
                 line[j+1] == 'b' || line[j+1] == 'B' ||
                 line[j+1] == 'o' || line[j+1] == 'O')) {
                j += 2;
                while (j < len && (isxdigit((unsigned char)line[j]) ||
                                   line[j] == '_')) j++;
            } else {
                while (j < len && (isdigit((unsigned char)line[j]) ||
                                   line[j] == '_')) j++;
                if (j < len && line[j] == '.') {
                    j++;
                    while (j < len && (isdigit((unsigned char)line[j]) ||
                                       line[j] == '_')) j++;
                }
                if (j < len && (line[j] == 'e' || line[j] == 'E')) {
                    int k = j + 1;
                    if (k < len && (line[k] == '+' || line[k] == '-')) k++;
                    if (k < len && isdigit((unsigned char)line[k])) {
                        j = k;
                        while (j < len && isdigit((unsigned char)line[j])) j++;
                    }
                }
            }
            /* suffix */
            while (j < len && (line[j] == 'u' || line[j] == 'U' ||
                               line[j] == 'l' || line[j] == 'L' ||
                               line[j] == 'f' || line[j] == 'F')) j++;
            emit_num();
            wn(line + i, j - i);
            emit_sep();
            i = j;
            continue;
        }

        /* Identifiers and keywords. */
        if (isalpha((unsigned char)c) || c == '_') {
            int j = i;
            while (j < len && (isalnum((unsigned char)line[j]) ||
                               line[j] == '_')) j++;
            int wlen = j - i;

            /* A keyword? SQL is case-insensitive. */
            int ci = (s->lang == HL_LANG_SQL);
            if (kw_set_has(s->kw, line + i, wlen, ci)) {
                emit_kw();
                wn(line + i, wlen);
                emit_sep();
            } else {
                /* Uppercase-leading or CamelCase identifiers that
                 * look like types get the type color. This catches
                 * `MyStruct`, `Vec`, `HashMap`, `True`, etc. without
                 * needing a per-language type list. */
                int looks_type = (isupper((unsigned char)line[i]) != 0);
                if (looks_type) {
                    emit_type();
                    wn(line + i, wlen);
                    emit_sep();
                } else {
                    emit_id();
                    wn(line + i, wlen);
                }
            }
            i = j;
            continue;
        }

        /* Anything else: punctuation, operators, whitespace. Emit
         * verbatim in default color; collapse the run to avoid
         * flickering color resets on every byte. */
        {
            int j = i + 1;
            while (j < len) {
                char d = line[j];
                if (isalnum((unsigned char)d) || d == '_') break;
                if (d == '\'' || d == '"' || d == '`') break;
                if (s->syn->block_open &&
                    j + (int)strlen(s->syn->block_open) <= len &&
                    memcmp(line + j, s->syn->block_open,
                           strlen(s->syn->block_open)) == 0) break;
                int stop = 0;
                for (int m = 0; s->syn->line_comment[m]; m++) {
                    int lcl = (int)strlen(s->syn->line_comment[m]);
                    if (j + lcl <= len &&
                        memcmp(line + j, s->syn->line_comment[m],
                               (size_t)lcl) == 0) { stop = 1; break; }
                }
                if (stop) break;
                j++;
            }
            emit_id();
            wn(line + i, j - i);
            emit_sep();
            i = j;
        }
    }

    emit_sep();
    g_sink = prev_sink;
}

/* ================================================================== */
/*  Diff rendering                                                    */
/*                                                                    */
/*  Simpler than the tokenizer. Line-prefix driven. The state is      */
/*  caller-owned so it persists across the wrapped-line iterator in   */
/*  console.c — but diffs are never wrapped, so in practice it just   */
/*  toggles between sections as the parser walks the body.            */
/* ================================================================== */

/* Strip leading whitespace and return the number of bytes skipped. */
static int skip_ws(const char *p, int len) {
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    return i;
}

void hl_emit_diff_line(hl_diff_state_t *state, void *hl,
                       const char *line, int len, int sink) {
    if (!line) return;
    if (len < 0) len = (int)strlen(line);

    int prev_sink = g_sink;
    g_sink = sink;

    int lead = skip_ws(line, len);
    const char *t = line + lead;

    /* Structural markers. Red for OLD, green for NEW, dim chrome
     * for the section header and separator. These are the only
     * lines that carry diff color; the content between them is
     * tokenized normally so HTML/C/JS still reads. */
    if (strncmp(t, "<<< OLD", 7) == 0) {
        if (state) *state = HL_DIFF_OLD;
        w(slot(TH_ERR));
        w(C_BOLD);
        wn(line, len);
        w(C_RESET);
        g_sink = prev_sink;
        return;
    }
    if (strncmp(t, ">>> NEW", 7) == 0) {
        if (state) *state = HL_DIFF_NEW;
        w(slot(TH_OK));
        w(C_BOLD);
        wn(line, len);
        w(C_RESET);
        g_sink = prev_sink;
        return;
    }
    if (strncmp(t, "===", 3) == 0) {
        if (state) *state = HL_DIFF_NEUTRAL;
        w(slot(TH_MUTED));
        wn(line, len);
        w(C_RESET);
        g_sink = prev_sink;
        return;
    }
    if (strncmp(t, "--- section ", 12) == 0) {
        w(slot(TH_MUTED));
        wn(line, len);
        w(C_RESET);
        g_sink = prev_sink;
        return;
    }

    /* Content line. If we have a language handle, tokenize it; if
     * not, print plain. We intentionally do NOT tint the whole line
     * by diff state — the markers above already carry that signal,
     * and tinting would fight with the token colors. */
    if (hl) {
        hl_emit_line(hl, line, len, sink);
    } else {
        w(C_INFO);
        wn(line, len);
        w(C_RESET);
    }

    g_sink = prev_sink;
}