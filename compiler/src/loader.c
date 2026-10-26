/* realpath and readlink are X/Open. This has to precede every system header. */
#if !defined(_WIN32)
#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < 700
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif

#include "phases.h"

#include "lexer.h"

#include "version.h"

#include "fs.h"
#include "map.h"
#include "str.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

/* Package loader.

   jog.mod is found by walking parents of start, which may be a file or a
   directory. The manifest is jog.mod v2 (docs/design/Packages.md): a module
   line, then version, kind, jog, require, export, link, and link.<os>
   lines. A V1 file (module and link lines) is a valid v2 file. Each
   --module <name>=<dir> names a library whose jog.mod is <dir>/jog.mod.

   The first segment of an import in module M is, in order: std, the
   standard library at std_root (JOG_ROOT/std when JOG_ROOT is set and
   non-empty, otherwise <directory of this executable>/../std); a module M
   requires, at its --module folder; otherwise a folder under M's root.
   Package paths are canonical: as written in the root module, prefixed
   with the module name in a dependency, so sdl's `import video` is
   sdl.video. A folder named std, or named like a required module, is an
   error.

   *.jog files in a package folder are lexed and parsed. *.test.jog files
   are included only when options.include_tests is set, and only in the
   root module. A file named <name>.<os>.jog or <name>.<os>.test.jog is
   loaded only when <os> is the target: windows, linux, macos, wasm, or
   posix (linux or macos). The target is the host, or wasm when ptr_size
   is 4. A dotted name whose last segment is not one of those, such as
   a.b.jog, is an ordinary file. Packages are ordered so each one comes
   after the packages it imports. An import that reaches a package already
   on the load stack is a cycle; the diagnostic names the path chain, and
   that back-edge is not recorded. */

#define LOAD_ERROR(loader, pos, ...)                                                   \
    do {                                                                               \
        (loader)->failed = true;                                                      \
        ct_diag_emit((loader)->program->diag, CT_SEVERITY_ERROR, (pos), __VA_ARGS__); \
    } while (0)

enum {
    PKG_GRAY = 1,
    PKG_DONE = 2,
    PKG_FAIL = 3
};

/* A require or export line: a module name, or a dotted package path. */
typedef struct ModLine {
    const char *text;
    CtPos pos;
} ModLine;

/* One jog.mod: the root's, or one given with --module. */
typedef struct Module {
    const char *name;
    const char *root; /* absolute folder that holds jog.mod */
    CtPos pos;        /* the name on the module line */
    bool is_root;
    bool is_library;
    bool required; /* a require line of some loaded jog.mod names it */
    bool bad;      /* a --module whose jog.mod failed; already reported */
    CtDa requires; /* ModLine */
    CtDa exports;  /* ModLine */
    CtDa links;    /* const char *, the link lines for this target */
} Module;

typedef struct PkgState {
    JogPackage *pkg;
    Module *mod; /* NULL for std */
    int color;
    CtDa imports; /* JogPackage *, while the package is loading */
} PkgState;

typedef struct Loader {
    JogProgram *program;
    CtMap *by_path;
    CtDa states; /* PkgState * */
    CtDa stack;  /* const char *, import paths of packages being loaded */
    CtDa order;  /* JogPackage * */
    CtPos module_pos;
    const char *start;     /* the start path as given */
    const char *start_dir; /* absolute folder of the start path */
    Module root_mod;
    CtDa mods; /* Module *, in --module order */
    bool failed;
} Loader;

typedef struct Tok {
    const char *ptr;
    size_t len;
    unsigned column;
} Tok;

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static char *dup_range(CtArena *arena, const char *text, size_t len) {
    char *out = ct_arena_alloc(arena, len + 1, 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

/* Parent directory. A root ("/", "C:\") is its own parent. A relative path
   with no separator returns ".". */
static char *parent_dir(CtArena *arena, const char *path) {
    size_t len = 0;
    size_t slash = 0;
    if (!arena || !path || path[0] == '\0') {
        return NULL;
    }
    len = strlen(path);
    while (len > 1 && is_sep(path[len - 1])) {
        if (len == 3 && path[1] == ':') {
            break;
        }
        len--;
    }
    if ((len == 1 && is_sep(path[0])) || (len == 2 && path[1] == ':') ||
        (len == 3 && path[1] == ':' && is_sep(path[2]))) {
        return dup_range(arena, path, len);
    }
    slash = len;
    while (slash > 0 && !is_sep(path[slash - 1])) {
        slash--;
    }
    if (slash == 0) {
        return ct_arena_strdup(arena, ".");
    }
    if (slash == 1) {
        return dup_range(arena, path, 1);
    }
    if (slash == 3 && path[1] == ':') {
        char *root = ct_arena_alloc(arena, 4, 1);
        if (!root) {
            return NULL;
        }
        root[0] = path[0];
        root[1] = ':';
        root[2] = path[slash - 1];
        root[3] = '\0';
        return root;
    }
    return dup_range(arena, path, slash - 1);
}

static int is_root_path(const char *path) {
    size_t len = 0;
    if (!path || path[0] == '\0') {
        return 1;
    }
    len = strlen(path);
    if (len == 1 && is_sep(path[0])) {
        return 1;
    }
    if (len == 2 && path[1] == ':') {
        return 1;
    }
    if (len == 3 && path[1] == ':' && is_sep(path[2])) {
        return 1;
    }
    return 0;
}

#if defined(_WIN32)

static char *absolute_path(CtArena *arena, const char *path) {
    wchar_t *wide = ct_fs_widen(path);
    wchar_t *buf = NULL;
    char *out = NULL;
    DWORD need = 0;
    DWORD got = 0;
    if (!wide) {
        return NULL;
    }
    need = GetFullPathNameW(wide, 0, NULL, NULL);
    buf = need > 0 ? malloc((size_t)need * sizeof(wchar_t)) : NULL;
    if (buf) {
        got = GetFullPathNameW(wide, need, buf, NULL);
        if (got > 0 && got < need) {
            out = ct_fs_narrow(arena, buf);
        }
    }
    free(buf);
    free(wide);
    return out;
}

static char *exe_path(CtArena *arena) {
    DWORD size = 32768;
    wchar_t *buf = malloc(size * sizeof(wchar_t));
    char *out = NULL;
    DWORD got = 0;
    if (!buf) {
        return NULL;
    }
    got = GetModuleFileNameW(NULL, buf, size);
    if (got > 0 && got < size) {
        out = ct_fs_narrow(arena, buf);
    }
    free(buf);
    return out;
}

#else

static char *absolute_path(CtArena *arena, const char *path) {
    char buf[4096];
    if (!realpath(path, buf)) {
        return NULL;
    }
    return ct_arena_strdup(arena, buf);
}

#if defined(__APPLE__)

static char *exe_path(CtArena *arena) {
    char buf[4096];
    uint32_t size = (uint32_t)sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) {
        return NULL;
    }
    return absolute_path(arena, buf);
}

#else

static char *exe_path(CtArena *arena) {
    char buf[4096];
    ssize_t got = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (got < 0 || (size_t)got >= sizeof buf - 1) {
        return NULL;
    }
    buf[got] = '\0';
    return ct_arena_strdup(arena, buf);
}

#endif
#endif

static int has_suffix(const char *name, const char *suffix) {
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    return suffix_len <= name_len && memcmp(name + name_len - suffix_len, suffix, suffix_len) == 0;
}

static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int is_ident_token(Tok tok) {
    size_t i = 0;
    if (tok.len == 0 || !is_ident_start(tok.ptr[0])) {
        return 0;
    }
    for (i = 1; i < tok.len; i++) {
        if (!is_ident_char(tok.ptr[i])) {
            return 0;
        }
    }
    return 1;
}

static int tok_is(Tok tok, const char *word) {
    size_t len = strlen(word);
    return tok.len == len && memcmp(tok.ptr, word, len) == 0;
}

static bool next_tok(const char *line, size_t len, size_t *index, Tok *tok) {
    size_t i = *index;
    size_t start = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= len || line[i] == '#') {
        *index = len;
        return false;
    }
    start = i;
    while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '#') {
        i++;
    }
    tok->ptr = line + start;
    tok->len = i - start;
    tok->column = (unsigned)start + 1;
    *index = i;
    return true;
}

static CtPos manifest_pos(const CtFile *file, unsigned line, unsigned column) {
    CtPos pos;
    pos.file = file->path;
    pos.line = line;
    pos.column = column;
    return pos;
}

static const char *intern_tok(Loader *loader, Tok tok) {
    return ct_intern(loader->program->intern, tok.ptr, tok.len);
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int all_digits(const char *text, size_t len) {
    size_t i = 0;
    for (i = 0; i < len; i++) {
        if (!is_digit(text[i])) {
            return 0;
        }
    }
    return len > 0;
}

/* Digits with no leading zero, or a single 0. */
static int numeric_ident(const char *text, size_t len) {
    return all_digits(text, len) && (len == 1 || text[0] != '0');
}

/* MAJOR.MINOR.PATCH with an optional -prerelease. No leading v and no
   +build metadata. */
static int semver_valid(const char *text, size_t len) {
    size_t i = 0;
    int part = 0;
    for (part = 0; part < 3; part++) {
        size_t start = i;
        while (i < len && is_digit(text[i])) {
            i++;
        }
        if (!numeric_ident(text + start, i - start)) {
            return 0;
        }
        if (part < 2) {
            if (i >= len || text[i] != '.') {
                return 0;
            }
            i++;
        }
    }
    if (i == len) {
        return 1;
    }
    if (text[i] != '-') {
        return 0;
    }
    i++;
    for (;;) {
        size_t start = i;
        while (i < len && text[i] != '.') {
            char c = text[i];
            if (!is_digit(c) && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && c != '-') {
                return 0;
            }
            i++;
        }
        if (i == start || (all_digits(text + start, i - start) && !numeric_ident(text + start, i - start))) {
            return 0;
        }
        if (i == len) {
            return 1;
        }
        i++;
    }
}

static int cmp_numeric(const char *a, size_t a_len, const char *b, size_t b_len) {
    int c = 0;
    if (a_len != b_len) {
        return a_len < b_len ? -1 : 1;
    }
    c = memcmp(a, b, a_len);
    return c < 0 ? -1 : c > 0;
}

/* Semantic Versioning precedence of two valid versions. */
static int semver_cmp(const char *a, const char *b) {
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    size_t ai = 0;
    size_t bi = 0;
    int part = 0;
    int a_pre = 0;
    int b_pre = 0;
    for (part = 0; part < 3; part++) {
        size_t a_start = ai;
        size_t b_start = bi;
        int c = 0;
        while (ai < a_len && is_digit(a[ai])) {
            ai++;
        }
        while (bi < b_len && is_digit(b[bi])) {
            bi++;
        }
        c = cmp_numeric(a + a_start, ai - a_start, b + b_start, bi - b_start);
        if (c != 0) {
            return c;
        }
        if (part < 2) {
            ai++;
            bi++;
        }
    }
    a_pre = ai < a_len;
    b_pre = bi < b_len;
    if (!a_pre || !b_pre) {
        return a_pre == b_pre ? 0 : (a_pre ? -1 : 1);
    }
    ai++;
    bi++;
    for (;;) {
        size_t a_start = ai;
        size_t b_start = bi;
        size_t a_n = 0;
        size_t b_n = 0;
        int a_num = 0;
        int b_num = 0;
        int c = 0;
        int a_more = 0;
        int b_more = 0;
        while (ai < a_len && a[ai] != '.') {
            ai++;
        }
        while (bi < b_len && b[bi] != '.') {
            bi++;
        }
        a_n = ai - a_start;
        b_n = bi - b_start;
        a_num = all_digits(a + a_start, a_n);
        b_num = all_digits(b + b_start, b_n);
        if (a_num && b_num) {
            c = cmp_numeric(a + a_start, a_n, b + b_start, b_n);
        } else if (a_num != b_num) {
            c = a_num ? -1 : 1;
        } else {
            c = memcmp(a + a_start, b + b_start, a_n < b_n ? a_n : b_n);
            c = c < 0 ? -1 : c > 0;
            if (c == 0 && a_n != b_n) {
                c = a_n < b_n ? -1 : 1;
            }
        }
        if (c != 0) {
            return c;
        }
        a_more = ai < a_len;
        b_more = bi < b_len;
        if (!a_more || !b_more) {
            return a_more == b_more ? 0 : (a_more ? 1 : -1);
        }
        ai++;
        bi++;
    }
}

/* A require spec that starts with ./ ../ / \ or a drive letter is a path. */
static int is_path_spec(Tok tok) {
    if (tok.len >= 1 && (tok.ptr[0] == '/' || tok.ptr[0] == '\\')) {
        return 1;
    }
    if (tok.len >= 2 && tok.ptr[0] == '.' && tok.ptr[1] == '/') {
        return 1;
    }
    if (tok.len >= 3 && tok.ptr[0] == '.' && tok.ptr[1] == '.' && tok.ptr[2] == '/') {
        return 1;
    }
    return tok.len >= 2 && tok.ptr[1] == ':' &&
           ((tok.ptr[0] >= 'a' && tok.ptr[0] <= 'z') || (tok.ptr[0] >= 'A' && tok.ptr[0] <= 'Z'));
}

static int is_dotted_path(Tok tok) {
    size_t start = 0;
    size_t i = 0;
    for (i = 0; i <= tok.len; i++) {
        if (i == tok.len || tok.ptr[i] == '.') {
            Tok seg;
            seg.ptr = tok.ptr + start;
            seg.len = i - start;
            seg.column = tok.column;
            if (!is_ident_token(seg)) {
                return 0;
            }
            start = i + 1;
        }
    }
    return 1;
}

static int word_in(const char *text, size_t len, const char *const *words, size_t count) {
    size_t i = 0;
    for (i = 0; i < count; i++) {
        if (strlen(words[i]) == len && memcmp(words[i], text, len) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *const g_reserved_words[] = {"libdir",  "source",  "replace",  "config",
                                               "include", "exclude", "provides", "needs"};
static const char *const g_known_words[] = {"module", "version", "kind", "jog", "require", "export", "link"};

static int is_reserved_directive(Tok tok) {
    size_t base = 0;
    while (base < tok.len && tok.ptr[base] != '.') {
        base++;
    }
    if (word_in(tok.ptr, base, g_reserved_words, sizeof g_reserved_words / sizeof g_reserved_words[0])) {
        return 1;
    }
    /* <directive>.<config>: a build-configuration selector. */
    return base < tok.len &&
           word_in(tok.ptr, base, g_known_words, sizeof g_known_words / sizeof g_known_words[0]);
}

#if defined(_WIN32)
#define JOG_HOST_OS "windows"
#elif defined(__APPLE__)
#define JOG_HOST_OS "macos"
#else
#define JOG_HOST_OS "linux"
#endif

static const ModLine *find_line(const CtDa *lines, const char *text, size_t len) {
    size_t i = 0;
    for (i = 0; i < lines->count; i++) {
        const ModLine *line = ct_da_at((CtDa *)lines, i);
        if (line && line->text && strlen(line->text) == len && memcmp(line->text, text, len) == 0) {
            return line;
        }
    }
    return NULL;
}

/* A directive with exactly one word after it. */
static int one_word(Loader *loader, const CtFile *file, unsigned line_no, const Tok *toks, size_t n,
                    const char *what) {
    if (n < 2) {
        LOAD_ERROR(loader, manifest_pos(file, line_no, toks[0].column), "jog.mod: missing %s", what);
        return 0;
    }
    if (n > 2) {
        LOAD_ERROR(loader, manifest_pos(file, line_no, toks[2].column), "jog.mod: extra tokens after the %s", what);
        return 0;
    }
    return 1;
}

static int push_line(Loader *loader, CtDa *lines, Tok tok, CtPos pos) {
    ModLine line;
    line.text = intern_tok(loader, tok);
    line.pos = pos;
    if (!line.text || !ct_da_push(lines, &line)) {
        LOAD_ERROR(loader, pos, "out of memory");
        return 0;
    }
    return 1;
}

static bool parse_manifest(Loader *loader, const CtFile *file, Module *mod) {
    const char *data = file->data;
    size_t size = file->size;
    size_t at = 0;
    unsigned line_no = 1;
    int saw_module = 0;
    int saw_version = 0;
    int saw_kind = 0;
    int saw_jog = 0;
    int any_directive = 0;
    int ok = 1;
    int any_export = 0;
    CtPos first_export = {0};

    while (at < size) {
        size_t line_start = at;
        size_t line_len = 0;
        size_t cursor = 0;
        Tok toks[4];
        size_t n = 0;
        size_t i = 0;
        Tok first;
        CtPos pos;
        int quoted = 0;
        while (at < size && data[at] != '\n' && data[at] != '\r') {
            at++;
        }
        line_len = at - line_start;
        if (at < size && data[at] == '\r') {
            at++;
        }
        if (at < size && data[at] == '\n') {
            at++;
        }

        memset(toks, 0, sizeof toks);
        while (n < 4 && next_tok(data + line_start, line_len, &cursor, &toks[n])) {
            n++;
        }
        if (n == 0) {
            line_no++;
            continue;
        }
        first = toks[0];
        pos = manifest_pos(file, line_no, first.column);
        for (i = 0; i < n; i++) {
            if (toks[i].ptr[0] == '"') {
                LOAD_ERROR(loader, manifest_pos(file, line_no, toks[i].column),
                           "jog.mod: quoted words are reserved for a later version");
                quoted = 1;
                break;
            }
        }
        if (quoted) {
            ok = 0;
            any_directive = 1;
            line_no++;
            continue;
        }

        if (tok_is(first, "module")) {
            if (saw_module) {
                LOAD_ERROR(loader, pos, "jog.mod: duplicate module declaration");
                ok = 0;
            } else if (any_directive) {
                LOAD_ERROR(loader, pos, "jog.mod: module name must be the first declaration");
                ok = 0;
            } else if (n < 2) {
                LOAD_ERROR(loader, pos, "jog.mod: missing module name");
                ok = 0;
            } else if (n > 2 || !is_ident_token(toks[1])) {
                if (n > 2) {
                    LOAD_ERROR(loader, manifest_pos(file, line_no, toks[2].column),
                               "jog.mod: extra tokens after the module name");
                } else {
                    LOAD_ERROR(loader, manifest_pos(file, line_no, toks[1].column),
                               "jog.mod: module name '%.*s' is not an identifier", (int)toks[1].len, toks[1].ptr);
                }
                ok = 0;
            } else if (tok_is(toks[1], "std")) {
                LOAD_ERROR(loader, manifest_pos(file, line_no, toks[1].column),
                           "jog.mod: module name 'std' is reserved for the standard library");
                ok = 0;
            } else {
                mod->name = intern_tok(loader, toks[1]);
                mod->pos = manifest_pos(file, line_no, toks[1].column);
                if (!mod->name) {
                    LOAD_ERROR(loader, pos, "out of memory");
                    ok = 0;
                }
            }
            saw_module = 1;
            any_directive = 1;
            line_no++;
            continue;
        }

        any_directive = 1;
        if (!saw_module) {
            LOAD_ERROR(loader, pos, "jog.mod: expected 'module <name>' as the first declaration");
            ok = 0;
            line_no++;
            continue;
        }
        if (tok_is(first, "link") || (first.len > 5 && memcmp(first.ptr, "link.", 5) == 0)) {
            int keep = 1;
            if (first.len > 4) {
                Tok target;
                target.ptr = first.ptr + 5;
                target.len = first.len - 5;
                target.column = first.column + 5;
                if (!tok_is(target, "windows") && !tok_is(target, "linux") && !tok_is(target, "macos")) {
                    LOAD_ERROR(loader, manifest_pos(file, line_no, target.column),
                               "jog.mod: unknown target '%.*s'; expected windows, linux, or macos",
                               (int)target.len, target.ptr);
                    ok = 0;
                    line_no++;
                    continue;
                }
                /* wasm32 links no C libraries of its own. */
                keep = loader->program->options.ptr_size != 4 && tok_is(target, JOG_HOST_OS);
            }
            if (!one_word(loader, file, line_no, toks, n, "library name")) {
                ok = 0;
            } else if (keep) {
                const char *lib = intern_tok(loader, toks[1]);
                if (!lib || !ct_da_push(&mod->links, &lib)) {
                    LOAD_ERROR(loader, pos, "out of memory");
                    ok = 0;
                }
            }
        } else if (tok_is(first, "version")) {
            if (saw_version) {
                LOAD_ERROR(loader, pos, "jog.mod: duplicate version declaration");
                ok = 0;
            } else if (!one_word(loader, file, line_no, toks, n, "version")) {
                ok = 0;
            } else if (!semver_valid(toks[1].ptr, toks[1].len)) {
                LOAD_ERROR(loader, manifest_pos(file, line_no, toks[1].column),
                           "jog.mod: '%.*s' is not a version; expected MAJOR.MINOR.PATCH", (int)toks[1].len,
                           toks[1].ptr);
                ok = 0;
            }
            saw_version = 1;
        } else if (tok_is(first, "kind")) {
            if (saw_kind) {
                LOAD_ERROR(loader, pos, "jog.mod: duplicate kind declaration");
                ok = 0;
            } else if (!one_word(loader, file, line_no, toks, n, "kind")) {
                ok = 0;
            } else if (tok_is(toks[1], "library")) {
                mod->is_library = true;
            } else if (!tok_is(toks[1], "executable")) {
                LOAD_ERROR(loader, manifest_pos(file, line_no, toks[1].column),
                           "jog.mod: unknown kind '%.*s'; expected executable or library", (int)toks[1].len,
                           toks[1].ptr);
                ok = 0;
            }
            saw_kind = 1;
        } else if (tok_is(first, "jog")) {
            if (saw_jog) {
                LOAD_ERROR(loader, pos, "jog.mod: duplicate jog declaration");
                ok = 0;
            } else if (!one_word(loader, file, line_no, toks, n, "compiler version")) {
                ok = 0;
            } else if (!semver_valid(toks[1].ptr, toks[1].len)) {
                LOAD_ERROR(loader, manifest_pos(file, line_no, toks[1].column),
                           "jog.mod: '%.*s' is not a version; expected MAJOR.MINOR.PATCH", (int)toks[1].len,
                           toks[1].ptr);
                ok = 0;
            } else {
                const char *need = intern_tok(loader, toks[1]);
                if (!need) {
                    LOAD_ERROR(loader, pos, "out of memory");
                    ok = 0;
                } else if (semver_cmp(JOG_VERSION, need) < 0) {
                    LOAD_ERROR(loader, manifest_pos(file, line_no, toks[1].column),
                               "jog.mod: module '%s' needs jog %s or later; this is jog %s",
                               mod->name ? mod->name : "", need, JOG_VERSION);
                    ok = 0;
                }
            }
            saw_jog = 1;
        } else if (tok_is(first, "require")) {
            CtPos name_pos = manifest_pos(file, line_no, n > 1 ? toks[1].column : first.column);
            if (n < 2) {
                LOAD_ERROR(loader, pos, "jog.mod: missing module name");
                ok = 0;
            } else if (!is_ident_token(toks[1])) {
                LOAD_ERROR(loader, name_pos, "jog.mod: module name '%.*s' is not an identifier", (int)toks[1].len,
                           toks[1].ptr);
                ok = 0;
            } else if (n < 3) {
                LOAD_ERROR(loader, name_pos, "jog.mod: missing the path or version of module '%.*s'",
                           (int)toks[1].len, toks[1].ptr);
                ok = 0;
            } else if (n > 3) {
                LOAD_ERROR(loader, manifest_pos(file, line_no, toks[3].column),
                           "jog.mod: extra tokens after the path or version");
                ok = 0;
            } else if (tok_is(toks[1], "std")) {
                LOAD_ERROR(loader, name_pos, "jog.mod: std is built in and cannot be required");
                ok = 0;
            } else if (mod->name && tok_is(toks[1], mod->name)) {
                LOAD_ERROR(loader, name_pos, "jog.mod: module '%s' cannot require itself", mod->name);
                ok = 0;
            } else if (find_line(&mod->requires, toks[1].ptr, toks[1].len)) {
                LOAD_ERROR(loader, name_pos, "jog.mod: module '%.*s' is required twice", (int)toks[1].len,
                           toks[1].ptr);
                ok = 0;
            } else if (!is_path_spec(toks[2]) && !semver_valid(toks[2].ptr, toks[2].len)) {
                LOAD_ERROR(loader, manifest_pos(file, line_no, toks[2].column),
                           "jog.mod: '%.*s' is neither a path nor a version", (int)toks[2].len, toks[2].ptr);
                ok = 0;
            } else if (!push_line(loader, &mod->requires, toks[1], name_pos)) {
                ok = 0;
            }
        } else if (tok_is(first, "export")) {
            CtPos path_pos = manifest_pos(file, line_no, n > 1 ? toks[1].column : first.column);
            if (!one_word(loader, file, line_no, toks, n, "package path")) {
                ok = 0;
            } else if (!is_dotted_path(toks[1]) || tok_is(toks[1], "std") ||
                       (toks[1].len > 4 && memcmp(toks[1].ptr, "std.", 4) == 0)) {
                LOAD_ERROR(loader, path_pos, "jog.mod: export '%.*s' is not a package path of this module",
                           (int)toks[1].len, toks[1].ptr);
                ok = 0;
            } else if (find_line(&mod->exports, toks[1].ptr, toks[1].len)) {
                LOAD_ERROR(loader, path_pos, "jog.mod: package '%.*s' is exported twice", (int)toks[1].len,
                           toks[1].ptr);
                ok = 0;
            } else if (!push_line(loader, &mod->exports, toks[1], path_pos)) {
                ok = 0;
            } else if (!any_export) {
                any_export = 1;
                first_export = path_pos;
            }
        } else if (is_reserved_directive(first)) {
            LOAD_ERROR(loader, pos, "jog.mod: '%.*s' is reserved for a later version", (int)first.len, first.ptr);
            ok = 0;
        } else {
            LOAD_ERROR(loader, pos, "jog.mod: unknown declaration '%.*s'", (int)first.len, first.ptr);
            ok = 0;
        }
        line_no++;
    }

    if (!mod->name && !saw_module) {
        LOAD_ERROR(loader, manifest_pos(file, 1, 1), "jog.mod: missing module name");
        ok = 0;
    }
    if (any_export && !mod->is_library) {
        LOAD_ERROR(loader, first_export, "jog.mod: only a library can export packages; add 'kind library'");
        ok = 0;
    }
    return ok != 0 && mod->name != NULL;
}

static bool module_init(Module *mod) {
    memset(mod, 0, sizeof *mod);
    if (!ct_da_init(&mod->requires, sizeof(ModLine)) || !ct_da_init(&mod->exports, sizeof(ModLine)) ||
        !ct_da_init(&mod->links, sizeof(const char *))) {
        ct_da_free(&mod->requires);
        ct_da_free(&mod->exports);
        ct_da_free(&mod->links);
        return false;
    }
    return true;
}

static void module_free(Module *mod) {
    ct_da_free(&mod->requires);
    ct_da_free(&mod->exports);
    ct_da_free(&mod->links);
}

static void loader_free(Loader *loader) {
    size_t i = 0;
    for (i = 0; i < loader->states.count; i++) {
        PkgState *state = *(PkgState **)ct_da_at(&loader->states, i);
        if (state) {
            ct_da_free(&state->imports);
        }
    }
    for (i = 0; i < loader->mods.count; i++) {
        Module *mod = *(Module **)ct_da_at(&loader->mods, i);
        if (mod) {
            module_free(mod);
        }
    }
    module_free(&loader->root_mod);
    ct_da_free(&loader->mods);
    ct_da_free(&loader->states);
    ct_da_free(&loader->stack);
    ct_da_free(&loader->order);
}

static bool locate_roots(Loader *loader) {
    CtArena *arena = loader->program->arena;
    const char *env = ct_fs_getenv(arena, "JOG_ROOT");
    const char *base = NULL;
    const char *std_dir = NULL;
    const char *runtime_dir = NULL;
    CtPos pos = {0};
    if (env && env[0] != '\0') {
        pos.file = env;
        if (!ct_fs_is_dir(env)) {
            LOAD_ERROR(loader, pos, "JOG_ROOT '%s' is not a directory", env);
            return false;
        }
        base = absolute_path(arena, env);
        if (!base) {
            LOAD_ERROR(loader, pos, "cannot resolve JOG_ROOT '%s'", env);
            return false;
        }
    } else {
        char *exe = exe_path(arena);
        char *exe_dir = NULL;
        if (exe) {
            exe_dir = parent_dir(arena, exe);
            base = exe_dir ? parent_dir(arena, exe_dir) : NULL;
            if (!exe_dir || !base) {
                LOAD_ERROR(loader, pos, "out of memory");
                return false;
            }
        }
    }
    if (!base) {
        return true;
    }
    std_dir = ct_path_join(arena, base, "std");
    runtime_dir = ct_path_join(arena, base, "runtime");
    if (!std_dir || !runtime_dir) {
        LOAD_ERROR(loader, pos, "out of memory");
        return false;
    }
    if (ct_fs_is_dir(std_dir)) {
        loader->program->std_root = std_dir;
    }
    if (ct_fs_is_dir(runtime_dir)) {
        loader->program->runtime_root = runtime_dir;
    }
    return true;
}

static const char *find_manifest(Loader *loader, const char *dir, const char **root_out) {
    const char *current = dir;
    int guard = 0;
    for (guard = 0; guard < 256; guard++) {
        char *manifest = ct_path_join(loader->program->arena, current, "jog.mod");
        char *parent = NULL;
        if (!manifest) {
            LOAD_ERROR(loader, loader->module_pos, "out of memory");
            return NULL;
        }
        if (ct_fs_is_file(manifest)) {
            *root_out = current;
            return manifest;
        }
        if (is_root_path(current)) {
            break;
        }
        parent = parent_dir(loader->program->arena, current);
        if (!parent || strcmp(parent, current) == 0) {
            break;
        }
        current = parent;
    }
    return NULL;
}

static bool open_module(Loader *loader, const char *start) {
    CtArena *arena = loader->program->arena;
    CtPos pos = {0};
    const char *abs = NULL;
    const char *dir = NULL;
    const char *root = NULL;
    const char *manifest = NULL;
    const char *local_std = NULL;
    CtFile file;
    pos.file = start;
    if (!ct_fs_is_file(start) && !ct_fs_is_dir(start)) {
        LOAD_ERROR(loader, pos, "cannot find '%s'", start);
        return false;
    }
    abs = absolute_path(arena, start);
    if (!abs) {
        LOAD_ERROR(loader, pos, "cannot resolve '%s'", start);
        return false;
    }
    if (ct_fs_is_dir(start)) {
        dir = abs;
    } else {
        dir = parent_dir(arena, abs);
        if (!dir) {
            LOAD_ERROR(loader, pos, "out of memory");
            return false;
        }
    }
    loader->start = start;
    loader->start_dir = dir;
    manifest = find_manifest(loader, dir, &root);
    if (!manifest) {
        if (!loader->failed) {
            LOAD_ERROR(loader, pos, "cannot find jog.mod above '%s'", start);
        }
        return false;
    }
    loader->program->module_root = root;
    if (!ct_file_read(arena, loader->program->diag, manifest, &file)) {
        loader->failed = true;
        return false;
    }
    loader->root_mod.root = root;
    loader->root_mod.is_root = true;
    if (!parse_manifest(loader, &file, &loader->root_mod)) {
        return false;
    }
    loader->program->module_name = loader->root_mod.name;
    loader->program->is_library = loader->root_mod.is_library;
    loader->module_pos = loader->root_mod.pos;
    local_std = ct_path_join(arena, root, "std");
    if (!local_std) {
        LOAD_ERROR(loader, loader->module_pos, "out of memory");
        return false;
    }
    if (ct_fs_is_dir(local_std)) {
        LOAD_ERROR(loader, loader->module_pos, "folder 'std' shadows the standard library");
    }
    return true;
}

static Module *find_mapped(Loader *loader, const char *name) {
    size_t i = 0;
    for (i = 0; i < loader->mods.count; i++) {
        Module *mod = *(Module **)ct_da_at(&loader->mods, i);
        if (mod && mod->name && name && strcmp(mod->name, name) == 0) {
            return mod;
        }
    }
    return NULL;
}

/* A mapping that was already reported. Imports of it add no second error. */
static bool push_bad(Loader *loader, const char *name, const char *dir) {
    Module *mod = ct_arena_alloc(loader->program->arena, sizeof *mod, _Alignof(Module));
    if (!mod || !module_init(mod)) {
        return false;
    }
    mod->name = name;
    mod->root = dir;
    mod->bad = true;
    if (!ct_da_push(&loader->mods, &mod)) {
        module_free(mod);
        return false;
    }
    return true;
}

/* Each --module <name>=<dir> must name a library whose jog.mod is
   <dir>/jog.mod and says `module <name>`. */
static bool open_mapped(Loader *loader) {
    JogProgram *program = loader->program;
    CtArena *arena = program->arena;
    size_t i = 0;
    size_t j = 0;
    for (i = 0; i < program->module_arg_count; i++) {
        const JogModuleArg *arg = &program->module_args[i];
        const char *name = arg->name ? arg->name : "";
        const char *dir = arg->dir ? arg->dir : "";
        CtPos pos = {0};
        Tok tok;
        Module *mod = NULL;
        const char *abs = NULL;
        const char *manifest = NULL;
        CtFile file;
        int dup = 0;
        pos.file = dir;
        tok.ptr = name;
        tok.len = strlen(name);
        tok.column = 1;
        if (!is_ident_token(tok)) {
            LOAD_ERROR(loader, pos, "--module name '%s' is not an identifier", name);
            continue;
        }
        if (strcmp(name, "std") == 0) {
            LOAD_ERROR(loader, pos, "--module cannot map std; the standard library is built in");
            continue;
        }
        if (program->module_name && strcmp(name, program->module_name) == 0) {
            LOAD_ERROR(loader, pos, "--module %s: '%s' is the module being built", name, name);
            continue;
        }
        for (j = 0; j < i; j++) {
            if (program->module_args[j].name && strcmp(program->module_args[j].name, name) == 0) {
                dup = 1;
            }
        }
        if (dup) {
            LOAD_ERROR(loader, pos, "--module %s is given twice", name);
            continue;
        }
        if (!ct_fs_is_dir(dir)) {
            LOAD_ERROR(loader, pos, "--module %s: cannot find folder '%s'", name, dir);
            if (!push_bad(loader, name, dir)) {
                LOAD_ERROR(loader, pos, "out of memory");
                return false;
            }
            continue;
        }
        abs = absolute_path(arena, dir);
        manifest = abs ? ct_path_join(arena, abs, "jog.mod") : NULL;
        if (!abs || !manifest) {
            LOAD_ERROR(loader, pos, "cannot resolve '%s'", dir);
            if (!push_bad(loader, name, dir)) {
                LOAD_ERROR(loader, pos, "out of memory");
                return false;
            }
            continue;
        }
        if (!ct_fs_is_file(manifest)) {
            LOAD_ERROR(loader, pos, "--module %s: no jog.mod in '%s'", name, dir);
            if (!push_bad(loader, name, dir)) {
                LOAD_ERROR(loader, pos, "out of memory");
                return false;
            }
            continue;
        }
        mod = ct_arena_alloc(arena, sizeof *mod, _Alignof(Module));
        if (!mod || !module_init(mod)) {
            LOAD_ERROR(loader, pos, "out of memory");
            return false;
        }
        if (!ct_da_push(&loader->mods, &mod)) {
            module_free(mod);
            LOAD_ERROR(loader, pos, "out of memory");
            return false;
        }
        mod->root = abs;
        if (!ct_file_read(arena, program->diag, manifest, &file)) {
            loader->failed = true;
            mod->bad = true;
            mod->name = name;
            continue;
        }
        if (!parse_manifest(loader, &file, mod)) {
            mod->bad = true;
            mod->name = name;
            continue;
        }
        if (strcmp(mod->name, name) != 0) {
            LOAD_ERROR(loader, mod->pos, "jog.mod: module '%s' was given as --module %s=%s", mod->name, name, dir);
            mod->bad = true;
            mod->name = name;
            continue;
        }
        if (!mod->is_library) {
            LOAD_ERROR(loader, mod->pos,
                       "jog.mod: module '%s' is not a library; only a module with 'kind library' can be "
                       "given with --module",
                       name);
            mod->bad = true;
            mod->name = name;
        }
    }
    return true;
}

/* Every require names a module; a folder must not shadow it; and every
   mapping must be required by some loaded jog.mod. */
static void check_requires(Loader *loader, Module *mod) {
    size_t i = 0;
    for (i = 0; i < mod->requires.count; i++) {
        ModLine *line = ct_da_at(&mod->requires, i);
        Module *dep = find_mapped(loader, line->text);
        const char *folder = ct_path_join(loader->program->arena, mod->root, line->text);
        if (dep) {
            dep->required = true;
        }
        if (!folder) {
            LOAD_ERROR(loader, line->pos, "out of memory");
            return;
        }
        if (ct_fs_is_dir(folder)) {
            LOAD_ERROR(loader, line->pos, "folder '%s' shadows module '%s'", line->text, line->text);
        }
    }
}

static void check_modules(Loader *loader) {
    size_t i = 0;
    check_requires(loader, &loader->root_mod);
    for (i = 0; i < loader->mods.count; i++) {
        Module *mod = *(Module **)ct_da_at(&loader->mods, i);
        if (mod && !mod->bad) {
            check_requires(loader, mod);
        }
    }
    for (i = 0; i < loader->mods.count; i++) {
        Module *mod = *(Module **)ct_da_at(&loader->mods, i);
        CtPos pos = {0};
        if (!mod || mod->bad || mod->required) {
            continue;
        }
        pos.file = mod->root;
        LOAD_ERROR(loader, pos, "--module %s: no loaded jog.mod requires module '%s'", mod->name, mod->name);
    }
}

/* The root's link lines, then each module's in --module order. A name
   that appears again keeps only its last position. */
static bool merge_links(Loader *loader) {
    CtDa all;
    CtDa merged;
    size_t i = 0;
    size_t j = 0;
    bool ok = true;
    if (!ct_da_init(&all, sizeof(const char *)) || !ct_da_init(&merged, sizeof(const char *))) {
        ct_da_free(&all);
        LOAD_ERROR(loader, loader->module_pos, "out of memory");
        return false;
    }
    for (i = 0; ok && i <= loader->mods.count; i++) {
        Module *mod = i == 0 ? &loader->root_mod : *(Module **)ct_da_at(&loader->mods, i - 1);
        if (!mod || mod->bad) {
            continue;
        }
        for (j = 0; ok && j < mod->links.count; j++) {
            ok = ct_da_push(&all, ct_da_at(&mod->links, j));
        }
    }
    for (i = 0; ok && i < all.count; i++) {
        const char *lib = *(const char **)ct_da_at(&all, i);
        int later = 0;
        for (j = i + 1; j < all.count; j++) {
            if (strcmp(lib, *(const char **)ct_da_at(&all, j)) == 0) {
                later = 1;
                break;
            }
        }
        if (!later) {
            ok = ct_da_push(&merged, &lib);
        }
    }
    loader->program->link_libs = NULL;
    loader->program->link_lib_count = 0;
    if (ok && merged.count > 0) {
        const char **copied = ct_arena_alloc(loader->program->arena, merged.count * sizeof(const char *),
                                              _Alignof(const char *));
        if (copied) {
            memcpy(copied, merged.data, merged.count * sizeof(const char *));
            loader->program->link_libs = copied;
            loader->program->link_lib_count = merged.count;
        } else {
            ok = false;
        }
    }
    if (!ok) {
        LOAD_ERROR(loader, loader->module_pos, "out of memory");
    }
    ct_da_free(&all);
    ct_da_free(&merged);
    return ok;
}

static void free_source_tokens(CtDa *files) {
    size_t i = 0;
    for (i = 0; i < files->count; i++) {
        JogSourceFile *source = *(JogSourceFile **)ct_da_at(files, i);
        if (source) {
            ct_da_free(&source->tokens);
        }
    }
}

static CtPos from_jog(JogPos pos) {
    CtPos out;
    out.file = pos.file;
    out.line = pos.line;
    out.column = pos.column;
    return out;
}

static const char *dotted_path(Loader *loader, const char **segs, size_t count) {
    size_t len = 0;
    size_t i = 0;
    size_t at = 0;
    char *buf = NULL;
    if (count == 0) {
        return ct_intern_cstr(loader->program->intern, "");
    }
    if (count == 1) {
        return segs[0];
    }
    for (i = 0; i < count; i++) {
        if (!segs[i]) {
            return NULL;
        }
        len += strlen(segs[i]);
        if (i + 1 < count) {
            len++;
        }
    }
    buf = ct_arena_alloc(loader->program->arena, len + 1, 1);
    if (!buf) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        size_t n = strlen(segs[i]);
        memcpy(buf + at, segs[i], n);
        at += n;
        if (i + 1 < count) {
            buf[at++] = '.';
        }
    }
    buf[at] = '\0';
    return ct_intern(loader->program->intern, buf, at);
}

/* The canonical path of a package of a dependency: <module>.<path>. */
static const char *prefixed_path(Loader *loader, const char *prefix, const char *path) {
    size_t prefix_len = strlen(prefix);
    size_t path_len = strlen(path);
    char *buf = NULL;
    if (path_len == 0) {
        return prefix;
    }
    buf = ct_arena_alloc(loader->program->arena, prefix_len + 1 + path_len + 1, 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, prefix, prefix_len);
    buf[prefix_len] = '.';
    memcpy(buf + prefix_len + 1, path, path_len + 1);
    return ct_intern(loader->program->intern, buf, prefix_len + 1 + path_len);
}

/* base joined with segs[begin..count). */
static const char *package_dir(Loader *loader, const char *base, const char **segs, size_t begin, size_t count,
                               CtPos pos) {
    const char *dir = base;
    size_t i = 0;
    for (i = begin; i < count; i++) {
        dir = ct_path_join(loader->program->arena, dir, segs[i]);
        if (!dir) {
            LOAD_ERROR(loader, pos, "out of memory");
            return NULL;
        }
    }
    return dir;
}

static JogPackage *load_package(Loader *loader, Module *mod, const char *path, const char *name, const char *dir,
                                CtPos pos, int *back_edge);

static void report_cycle(Loader *loader, const char *target, CtPos pos) {
    CtSb sb;
    size_t start = 0;
    size_t i = 0;
    int found = 0;
    ct_sb_init(&sb);
    for (i = 0; i < loader->stack.count; i++) {
        const char *item = *(const char **)ct_da_at(&loader->stack, i);
        if (item && target && strcmp(item, target) == 0) {
            start = i;
            found = 1;
            break;
        }
    }
    if (!found) {
        start = 0;
    }
    for (i = start; i < loader->stack.count; i++) {
        const char *item = *(const char **)ct_da_at(&loader->stack, i);
        if (i > start && !ct_sb_append_cstr(&sb, " -> ")) {
            break;
        }
        if (!item || !ct_sb_append_cstr(&sb, item)) {
            break;
        }
    }
    if (sb.failed || !target || !ct_sb_append_cstr(&sb, " -> ") || !ct_sb_append_cstr(&sb, target)) {
        loader->failed = true;
        ct_diag_emit(loader->program->diag, CT_SEVERITY_ERROR, pos, "import cycle");
    } else {
        LOAD_ERROR(loader, pos, "import cycle: %s", ct_sb_cstr(&sb));
    }
    ct_sb_free(&sb);
}

static int already_imports(CtDa *imports, JogPackage *dep) {
    size_t i = 0;
    for (i = 0; i < imports->count; i++) {
        if (*(JogPackage **)ct_da_at(imports, i) == dep) {
            return 1;
        }
    }
    return 0;
}

/* rest is the package path inside the module, "" for its root package,
   which is always importable. No export lines export everything. */
static int is_exported(const Module *mod, const char *rest) {
    if (rest[0] == '\0' || mod->exports.count == 0) {
        return 1;
    }
    return find_line(&mod->exports, rest, strlen(rest)) != NULL;
}

static void collect_imports(Loader *loader, PkgState *state) {
    JogPackage *pkg = state->pkg;
    size_t file_index = 0;
    for (file_index = 0; file_index < pkg->file_count; file_index++) {
        JogSourceFile *source = pkg->files[file_index];
        size_t import_index = 0;
        if (!source || !source->ast || source->ast->kind != JOG_NODE_FILE) {
            continue;
        }
        for (import_index = 0; import_index < source->ast->as.file.imports.count; import_index++) {
            JogNode *node = source->ast->as.file.imports.items[import_index];
            const char **segs = NULL;
            size_t count = 0;
            CtPos pos;
            const char *ipath = NULL;
            const char *idir = NULL;
            const char *iname = NULL;
            int is_std = 0;
            int back = 0;
            JogPackage *dep = NULL;
            Module *target = NULL;
            if (!node || node->kind != JOG_NODE_IMPORT) {
                continue;
            }
            segs = node->as.import.path.items;
            count = node->as.import.path.count;
            pos = from_jog(node->pos);
            if (count == 0 || !segs || !segs[0]) {
                LOAD_ERROR(loader, pos, "empty import path");
                continue;
            }
            is_std = strcmp(segs[0], "std") == 0;
            ipath = dotted_path(loader, segs, count);
            if (!ipath) {
                LOAD_ERROR(loader, pos, "out of memory");
                continue;
            }
            target = state->mod;
            if (is_std) {
                if (!loader->program->std_root) {
                    LOAD_ERROR(loader, pos, "standard library was not found");
                    continue;
                }
                target = NULL;
                idir = package_dir(loader, loader->program->std_root, segs, 1, count, pos);
            } else if (state->mod && find_line(&state->mod->requires, segs[0], strlen(segs[0]))) {
                const char *rest = dotted_path(loader, segs + 1, count - 1);
                target = find_mapped(loader, segs[0]);
                if (!target) {
                    LOAD_ERROR(loader, pos,
                               "module '%s' is required by jog.mod but was not given with --module; build with "
                               "jog-build, or pass --module %s=<dir>",
                               segs[0], segs[0]);
                    continue;
                }
                if (target->bad) {
                    continue;
                }
                if (!rest) {
                    LOAD_ERROR(loader, pos, "out of memory");
                    continue;
                }
                if (!is_exported(target, rest)) {
                    LOAD_ERROR(loader, pos, "package '%s' is not exported by module '%s'", ipath, target->name);
                    continue;
                }
                idir = package_dir(loader, target->root, segs, 1, count, pos);
            } else {
                if (state->mod && !state->mod->is_root) {
                    ipath = prefixed_path(loader, state->mod->name, ipath);
                    if (!ipath) {
                        LOAD_ERROR(loader, pos, "out of memory");
                        continue;
                    }
                }
                idir = package_dir(loader, state->mod ? state->mod->root : loader->program->module_root, segs, 0,
                                   count, pos);
            }
            if (!idir) {
                continue;
            }
            if (!ct_fs_is_dir(idir)) {
                LOAD_ERROR(loader, pos, "cannot find package '%s'", ipath);
                continue;
            }
            iname = segs[count - 1];
            dep = load_package(loader, target, ipath, iname, idir, pos, &back);
            if (back) {
                report_cycle(loader, ipath, pos);
                continue;
            }
            if (!dep) {
                continue;
            }
            if (already_imports(&state->imports, dep)) {
                continue;
            }
            if (!ct_da_push(&state->imports, &dep)) {
                LOAD_ERROR(loader, pos, "out of memory");
            }
        }
    }
}

static int os_name_is(const char *text, size_t len, const char *word) {
    size_t n = strlen(word);
    return n == len && memcmp(text, word, len) == 0;
}

static int os_tag_is_known(const char *text, size_t len) {
    return os_name_is(text, len, "windows") || os_name_is(text, len, "linux") || os_name_is(text, len, "macos") ||
           os_name_is(text, len, "wasm") || os_name_is(text, len, "posix");
}

/* wasm32 is ptr_size 4, the same choice link.<os> makes. Anything else
   is the host the compiler itself was built for. */
static const char *load_target_os(const Loader *loader) {
    if (loader->program->options.ptr_size == 4) {
        return "wasm";
    }
    return JOG_HOST_OS;
}

static int os_tag_matches(const char *tag, size_t len, const char *target) {
    if (os_name_is(tag, len, "posix")) {
        return strcmp(target, "linux") == 0 || strcmp(target, "macos") == 0;
    }
    return os_name_is(tag, len, target);
}

/* 1 when name is loaded for target, before the *.test.jog filter.
   *is_test is set when the name ends in .test.jog. The OS tag is the
   segment immediately before .jog or .test.jog. posix means linux or
   macos. Any other segment, including a.b.jog, keeps the file. */
static int file_os_kept(const char *name, const char *target, int *is_test) {
    size_t n = strlen(name);
    const char *stem_end = NULL;
    const char *dot = NULL;
    const char *p = NULL;
    const char *tag = NULL;
    size_t tag_len = 0;
    *is_test = 0;
    if (has_suffix(name, ".test.jog")) {
        *is_test = 1;
        stem_end = name + n - 9;
    } else if (has_suffix(name, ".jog")) {
        stem_end = name + n - 4;
    } else {
        return 1;
    }
    for (p = name; p < stem_end; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot || dot == name) {
        return 1;
    }
    tag = dot + 1;
    tag_len = (size_t)(stem_end - tag);
    if (!os_tag_is_known(tag, tag_len)) {
        return 1;
    }
    return os_tag_matches(tag, tag_len, target);
}

/* Returns false when the folder contributes no source files, the directory
   cannot be listed, or memory runs out. Lex and parse errors set
   loader->failed and still keep the file. */
static int load_files(Loader *loader, PkgState *state, CtPos pos) {
    JogPackage *pkg = state->pkg;
    Module *mod = state->mod;
    int tests = loader->program->options.include_tests && mod && mod->is_root;
    CtDa names;
    CtDa files;
    size_t i = 0;
    int names_ready = 0;
    int files_ready = 0;
    int ok = 1;
    names_ready = ct_da_init(&names, sizeof(const char *));
    files_ready = ct_da_init(&files, sizeof(JogSourceFile *));
    if (!names_ready || !files_ready) {
        LOAD_ERROR(loader, pos, "out of memory");
        ct_da_free(&names);
        ct_da_free(&files);
        return 0;
    }
    if (!ct_fs_list(loader->program->arena, loader->program->diag, pkg->dir, CT_FS_FILES, ".jog", &names)) {
        loader->failed = true;
        ct_da_free(&names);
        ct_da_free(&files);
        return 0;
    }
    for (i = 0; i < names.count; i++) {
        const char *name = *(const char **)ct_da_at(&names, i);
        int is_test = 0;
        const char *full = NULL;
        JogSourceFile *source = NULL;
        size_t errors_before = 0;
        int lex_ok = 0;
        if (!name) {
            continue;
        }
        if (!file_os_kept(name, load_target_os(loader), &is_test)) {
            continue;
        }
        if (is_test && !tests) {
            continue;
        }
        full = ct_path_join(loader->program->arena, pkg->dir, name);
        source = ct_arena_alloc(loader->program->arena, sizeof(*source), _Alignof(JogSourceFile));
        if (!full || !source) {
            LOAD_ERROR(loader, pos, "out of memory");
            ok = 0;
            break;
        }
        memset(source, 0, sizeof(*source));
        source->is_test = is_test;
        if (!ct_da_init(&source->tokens, sizeof(JogToken))) {
            LOAD_ERROR(loader, pos, "out of memory");
            ok = 0;
            break;
        }
        if (!ct_file_read(loader->program->arena, loader->program->diag, full, &source->file)) {
            loader->failed = true;
            ct_da_free(&source->tokens);
            ok = 0;
            break;
        }
        errors_before = ct_diag_error_count(loader->program->diag);
        lex_ok = jog_lex(loader->program->arena, loader->program->diag, loader->program->intern, &source->file,
                         &source->tokens);
        if (!lex_ok && ct_diag_error_count(loader->program->diag) == errors_before) {
            LOAD_ERROR(loader, pos, "out of memory");
            ct_da_free(&source->tokens);
            ok = 0;
            break;
        }
        source->ast = jog_parse(loader->program->arena, loader->program->diag, loader->program->intern, &source->file,
                                &source->tokens);
        if (!source->ast) {
            LOAD_ERROR(loader, pos, "out of memory");
            ct_da_free(&source->tokens);
            ok = 0;
            break;
        }
        if (!lex_ok || ct_diag_error_count(loader->program->diag) != errors_before) {
            loader->failed = true;
        }
        if (!ct_da_push(&files, &source)) {
            LOAD_ERROR(loader, pos, "out of memory");
            ct_da_free(&source->tokens);
            ok = 0;
            break;
        }
    }
    if (!ok) {
        free_source_tokens(&files);
        ct_da_free(&names);
        ct_da_free(&files);
        return 0;
    }
    if (files.count == 0 && mod && mod->is_library && mod->exports.count > 0 && strcmp(pkg->dir, mod->root) == 0) {
        /* A library's root package may be empty when it lists exports. */
        pkg->files = NULL;
        pkg->file_count = 0;
        ct_da_free(&names);
        ct_da_free(&files);
        return 1;
    }
    if (files.count == 0) {
        if (pkg->path[0] == '\0') {
            LOAD_ERROR(loader, pos, "the root package has no source files");
        } else {
            LOAD_ERROR(loader, pos, "package '%s' has no source files", pkg->path);
        }
        ct_da_free(&names);
        ct_da_free(&files);
        return 0;
    }
    {
        JogSourceFile **copied = ct_arena_alloc(loader->program->arena, files.count * sizeof(JogSourceFile *),
                                                 _Alignof(JogSourceFile *));
        if (!copied) {
            LOAD_ERROR(loader, pos, "out of memory");
            free_source_tokens(&files);
            ct_da_free(&names);
            ct_da_free(&files);
            return 0;
        }
        memcpy(copied, files.data, files.count * sizeof(JogSourceFile *));
        pkg->files = copied;
        pkg->file_count = files.count;
    }
    ct_da_free(&names);
    ct_da_free(&files);
    return 1;
}

static int freeze_imports(Loader *loader, PkgState *state, CtPos pos) {
    if (state->imports.count == 0) {
        state->pkg->imports = NULL;
        state->pkg->import_count = 0;
        return 1;
    }
    {
        JogPackage **copied = ct_arena_alloc(loader->program->arena, state->imports.count * sizeof(JogPackage *),
                                              _Alignof(JogPackage *));
        if (!copied) {
            LOAD_ERROR(loader, pos, "out of memory");
            return 0;
        }
        memcpy(copied, state->imports.data, state->imports.count * sizeof(JogPackage *));
        state->pkg->imports = copied;
        state->pkg->import_count = state->imports.count;
    }
    return 1;
}

static JogPackage *load_package(Loader *loader, Module *mod, const char *path, const char *name, const char *dir,
                                CtPos pos, int *back_edge) {
    void *found = NULL;
    PkgState *state = NULL;
    JogPackage *pkg = NULL;
    int files_ok = 0;
    *back_edge = 0;
    if (!path || !name || !dir) {
        LOAD_ERROR(loader, pos, "out of memory");
        return NULL;
    }
    if (ct_map_get(loader->by_path, path, &found)) {
        state = found;
        if (state->color == PKG_GRAY) {
            *back_edge = 1;
            return state->pkg;
        }
        if (state->color == PKG_DONE) {
            return state->pkg;
        }
        return NULL;
    }

    state = ct_arena_alloc(loader->program->arena, sizeof(*state), _Alignof(PkgState));
    pkg = ct_arena_alloc(loader->program->arena, sizeof(*pkg), _Alignof(JogPackage));
    if (!state || !pkg) {
        LOAD_ERROR(loader, pos, "out of memory");
        return NULL;
    }
    memset(state, 0, sizeof(*state));
    memset(pkg, 0, sizeof(*pkg));
    pkg->path = path;
    pkg->name = name;
    pkg->dir = dir;
    pkg->is_std = mod == NULL;
    pkg->module_name = mod ? mod->name : "std";
    pkg->module_root = mod ? mod->root : loader->program->std_root;
    state->pkg = pkg;
    state->mod = mod;
    state->color = PKG_GRAY;
    if (!ct_da_init(&state->imports, sizeof(JogPackage *))) {
        LOAD_ERROR(loader, pos, "out of memory");
        return NULL;
    }
    if (!ct_map_put(loader->by_path, path, state) || !ct_da_push(&loader->states, &state) ||
        !ct_da_push(&loader->stack, &path)) {
        LOAD_ERROR(loader, pos, "out of memory");
        state->color = PKG_FAIL;
        return NULL;
    }

    files_ok = load_files(loader, state, pos);
    if (files_ok) {
        collect_imports(loader, state);
        if (!freeze_imports(loader, state, pos)) {
            files_ok = 0;
        }
    }
    if (loader->stack.count > 0) {
        loader->stack.count--;
    }
    if (!files_ok) {
        state->color = PKG_FAIL;
        return NULL;
    }
    state->color = PKG_DONE;
    if (!ct_da_push(&loader->order, &pkg)) {
        LOAD_ERROR(loader, pos, "out of memory");
        state->color = PKG_FAIL;
        return NULL;
    }
    return pkg;
}

/* With options.start_package, a start path below the module root also
   loads the package of that folder, whose import path is the folders
   between the root and it. The root does not have to import it. */
static void load_start_package(Loader *loader) {
    const char *root = loader->program->module_root;
    const char *dir = loader->start_dir;
    const char *rest = NULL;
    CtDa segs;
    CtPos pos = {0};
    size_t root_len = 0;
    const char *path = NULL;
    int back = 0;
    if (!root || !dir) {
        return;
    }
    root_len = strlen(root);
    if (strncmp(dir, root, root_len) != 0) {
        return;
    }
    rest = dir + root_len;
    while (is_sep(*rest)) {
        rest++;
    }
    if (*rest == '\0') {
        return;
    }
    pos.file = loader->start;
    if (!ct_da_init(&segs, sizeof(const char *))) {
        LOAD_ERROR(loader, pos, "out of memory");
        return;
    }
    while (*rest) {
        Tok tok;
        const char *seg = NULL;
        tok.ptr = rest;
        tok.len = 0;
        tok.column = 1;
        while (rest[tok.len] && !is_sep(rest[tok.len])) {
            tok.len++;
        }
        rest += tok.len;
        while (is_sep(*rest)) {
            rest++;
        }
        if (!is_ident_token(tok)) {
            LOAD_ERROR(loader, pos, "folder '%.*s' is not a package name", (int)tok.len, tok.ptr);
            ct_da_free(&segs);
            return;
        }
        seg = intern_tok(loader, tok);
        if (!seg || !ct_da_push(&segs, &seg)) {
            LOAD_ERROR(loader, pos, "out of memory");
            ct_da_free(&segs);
            return;
        }
    }
    if (strcmp(*(const char **)ct_da_at(&segs, 0), "std") == 0) {
        ct_da_free(&segs);
        return;
    }
    path = dotted_path(loader, segs.data, segs.count);
    if (!path) {
        LOAD_ERROR(loader, pos, "out of memory");
    } else {
        load_package(loader, &loader->root_mod, path, *(const char **)ct_da_at(&segs, segs.count - 1), dir, pos,
                     &back);
    }
    ct_da_free(&segs);
}

/* A library is checked through its exports: each one is loaded even when
   the root package does not import it. */
static void load_exports(Loader *loader) {
    Module *mod = &loader->root_mod;
    size_t i = 0;
    for (i = 0; i < mod->exports.count; i++) {
        ModLine *line = ct_da_at(&mod->exports, i);
        const char *segs[64];
        size_t count = 0;
        const char *at = line->text;
        const char *dir = NULL;
        int back = 0;
        while (*at && count < 64) {
            size_t len = 0;
            while (at[len] && at[len] != '.') {
                len++;
            }
            segs[count] = ct_intern(loader->program->intern, at, len);
            if (!segs[count]) {
                LOAD_ERROR(loader, line->pos, "out of memory");
                return;
            }
            count++;
            at += len;
            if (*at == '.') {
                at++;
            }
        }
        if (*at) {
            LOAD_ERROR(loader, line->pos, "jog.mod: export '%s' is nested too deeply", line->text);
            continue;
        }
        dir = package_dir(loader, mod->root, segs, 0, count, line->pos);
        if (!dir) {
            continue;
        }
        if (!ct_fs_is_dir(dir)) {
            LOAD_ERROR(loader, line->pos, "jog.mod: cannot find exported package '%s'", line->text);
            continue;
        }
        load_package(loader, mod, line->text, segs[count - 1], dir, line->pos, &back);
        if (back) {
            loader->failed = true;
        }
    }
}

/* Generated C names are jog_<path with . as _>__<name>, so packages of two
   modules must not map to the same prefix: app's lib_util and lib's
   lib.util would. */
static void check_c_names(Loader *loader) {
    CtMap *seen = ct_map_create(loader->program->arena);
    size_t i = 0;
    if (!seen) {
        LOAD_ERROR(loader, loader->module_pos, "out of memory");
        return;
    }
    for (i = 0; i < loader->order.count; i++) {
        JogPackage *pkg = *(JogPackage **)ct_da_at(&loader->order, i);
        const char *source = pkg->path[0] != '\0' ? pkg->path : pkg->name;
        size_t len = strlen(source);
        char *key = ct_arena_alloc(loader->program->arena, len + 1, 1);
        void *found = NULL;
        size_t k = 0;
        if (!key) {
            LOAD_ERROR(loader, loader->module_pos, "out of memory");
            return;
        }
        for (k = 0; k < len; k++) {
            key[k] = is_ident_char(source[k]) ? source[k] : '_';
        }
        key[len] = '\0';
        if (ct_map_get(seen, key, &found)) {
            JogPackage *other = found;
            if (other->module_name && pkg->module_name && strcmp(other->module_name, pkg->module_name) != 0) {
                LOAD_ERROR(loader, loader->module_pos,
                           "package '%s' of module '%s' and package '%s' of module '%s' have the same C name "
                           "prefix 'jog_%s'; rename one of them",
                           other->path[0] ? other->path : other->name, other->module_name,
                           pkg->path[0] ? pkg->path : pkg->name, pkg->module_name, key);
            }
            continue;
        }
        if (!ct_map_put(seen, key, pkg)) {
            LOAD_ERROR(loader, loader->module_pos, "out of memory");
            return;
        }
    }
}

static int publish_order(Loader *loader) {
    if (loader->order.count == 0) {
        loader->program->packages = NULL;
        loader->program->package_count = 0;
        return 1;
    }
    {
        JogPackage **copied = ct_arena_alloc(loader->program->arena, loader->order.count * sizeof(JogPackage *),
                                              _Alignof(JogPackage *));
        if (!copied) {
            LOAD_ERROR(loader, loader->module_pos, "out of memory");
            return 0;
        }
        memcpy(copied, loader->order.data, loader->order.count * sizeof(JogPackage *));
        loader->program->packages = copied;
        loader->program->package_count = loader->order.count;
    }
    return 1;
}

bool jog_load(JogProgram *program, const char *start) {
    CtPos pos = {0};
    Loader loader;
    size_t errors_before = 0;
    const char *root_path = NULL;
    int back = 0;
    JogPackage *root = NULL;
    int ok = 0;
    if (!program || !program->arena || !program->diag || !program->intern) {
        if (program && program->diag) {
            ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos,
                         "loader is missing an arena, diagnostics, or intern table");
        }
        return false;
    }
    errors_before = ct_diag_error_count(program->diag);
    if (!start || start[0] == '\0') {
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "missing start path");
        return false;
    }

    memset(&loader, 0, sizeof loader);
    loader.program = program;
    if (!ct_da_init(&loader.states, sizeof(PkgState *)) || !ct_da_init(&loader.stack, sizeof(const char *)) ||
        !ct_da_init(&loader.order, sizeof(JogPackage *)) || !ct_da_init(&loader.mods, sizeof(Module *)) ||
        !module_init(&loader.root_mod)) {
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
        loader_free(&loader);
        return false;
    }
    loader.by_path = ct_map_create(program->arena);
    if (!loader.by_path) {
        ct_diag_emit(program->diag, CT_SEVERITY_ERROR, pos, "out of memory");
        loader_free(&loader);
        return false;
    }
    if (!locate_roots(&loader) || !open_module(&loader, start) || !open_mapped(&loader)) {
        loader_free(&loader);
        return false;
    }
    check_modules(&loader);
    if (!merge_links(&loader)) {
        loader_free(&loader);
        return false;
    }

    root_path = ct_intern_cstr(program->intern, "");
    if (!root_path || !program->module_name || !program->module_root) {
        LOAD_ERROR(&loader, loader.module_pos, "out of memory");
        loader_free(&loader);
        return false;
    }
    root = load_package(&loader, &loader.root_mod, root_path, program->module_name, program->module_root,
                        loader.module_pos, &back);
    program->root = root;
    if (back) {
        loader.failed = true;
    }
    if (loader.root_mod.is_library) {
        load_exports(&loader);
    }
    if (program->options.start_package) {
        load_start_package(&loader);
    }
    check_c_names(&loader);
    if (!publish_order(&loader)) {
        loader.failed = true;
    }
    ok = !loader.failed && root && ct_diag_error_count(program->diag) == errors_before;
    loader_free(&loader);
    return ok;
}
