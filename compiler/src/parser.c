#include "phases.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Recursive descent for declarations, types, statements, and expressions.

   jog_parse returns a FILE node after every syntax error and returns NULL
   only when the arena runs out. Recovery skips to the next SEP or to a '}'
   at the same brace depth, so the rest of the file is still parsed.

   quote stays rejected; it is post-V1. A trailing lambda is the '{' form
   on a call. Parenthesized lambdas are recognized by scanning to the
   matching ')' and checking for ':' or '->'. In an expression, '<' after
   an identifier is type arguments only when that parse succeeds and the
   next token can follow a generic name; otherwise it is less-than. '>>'
   inside type arguments is two '>' tokens. if, when, and go are
   expressions, so a statement that starts with one is that expression
   used as a statement. The heads of if, when, for, and with do not take a
   trailing lambda, so '{' opens the body. A SEP before else is ignored.
   Struct literals stay calls. `...T: name` is a pack parameter. A bare
   `...` is the extern C variadic and is not a pack.

   A 'with (' whose first
   parameter is 'name:' is a declaration context clause; a function type's
   own 'with' lists types ('with Logger' or 'with (Logger)'). struct,
   interface, and enum may omit '{...}', which is an empty body, so
   '@opaque struct Name' matches the C interop examples. '?' after a pointer
   wraps the pointer ('*T?' is '(*T)?') because the pointee is a non-null
   type; a slice or array element uses a full type, so '[]T?' is a slice
   of 'T?'. `expect` and `actual` are identifiers except immediately
   before `fn` at the top level, where they mark an expect or actual
   function. */

typedef enum FnMode {
    FN_REGULAR = 0,
    FN_EXTERN,
    FN_INTERFACE,
    FN_EXPECT,
    FN_ACTUAL
} FnMode;

typedef struct Parser {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    const char *path;
    const JogToken *toks;
    size_t count;
    size_t at;
    bool pending_gt;
    uint32_t gt_line;
    uint32_t gt_column;
    bool panic;
    bool oom;
    bool saw_non_import;
    int quiet; /* >0: keep panic, but do not emit diagnostics */
} Parser;

typedef struct ParserMark {
    size_t at;
    bool pending_gt;
    uint32_t gt_line;
    uint32_t gt_column;
    bool panic;
} ParserMark;

static void error_here(Parser *p, const char *fmt, ...);
static void complain(Parser *p, const char *fmt, ...);
static JogNode *parse_type(Parser *p);
static JogNode *parse_expr(Parser *p);
static JogNode *parse_expr_trail(Parser *p, bool trail);
static JogNode *parse_slice_bound(Parser *p, bool trail);
static JogNode *parse_block(Parser *p);
static JogList parse_type_params(Parser *p);
static bool type_can_start(JogTokenKind kind);
static JogList parse_param_list(Parser *p, bool allow_variadic, bool allow_defaults, const char *pack_reject,
                                bool *variadic);
static JogNode *parse_if(Parser *p);
static JogNode *parse_when(Parser *p);
static JogNode *parse_go(Parser *p);
static JogNode *parse_lambda(Parser *p);
static JogNode *parse_trailing_lambda(Parser *p);
static JogList parse_attributes(Parser *p);
static JogList parse_expr_type_args(Parser *p);
static bool at_paren_lambda(const Parser *p);

/* ---- diagnostics and tokens ---- */

static void report(Parser *p, bool fatal, const char *fmt, va_list args) {
    if (p->oom || p->panic) {
        return;
    }
    if (fatal) {
        p->panic = true;
    }
    if (p->quiet > 0) {
        return;
    }
    char message[384];
    vsnprintf(message, sizeof message, fmt, args);
    if (!p->diag) {
        return;
    }
    const JogToken *t = NULL;
    uint32_t line = 1;
    uint32_t column = 1;
    if (p->pending_gt) {
        line = p->gt_line;
        column = p->gt_column;
    } else if (p->at < p->count) {
        t = &p->toks[p->at];
        line = t->line;
        column = t->column;
    }
    CtPos pos = {p->path, line, column};
    ct_diag_emit(p->diag, CT_SEVERITY_ERROR, pos, "%s", message);
}

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void error_here(Parser *p, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    report(p, true, fmt, args);
    va_end(args);
}

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void complain(Parser *p, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    report(p, false, fmt, args);
    va_end(args);
}

static bool give_up(const Parser *p) {
    return p->oom || p->panic;
}

static const JogToken *cur(const Parser *p) {
    if (p->pending_gt || p->at >= p->count) {
        return NULL;
    }
    return &p->toks[p->at];
}

static JogTokenKind peek_kind(const Parser *p) {
    if (p->pending_gt) {
        return JOG_TOK_GT;
    }
    if (p->at >= p->count) {
        return JOG_TOK_EOF;
    }
    return p->toks[p->at].kind;
}

static JogTokenKind peek_n(const Parser *p, size_t n) {
    if (p->pending_gt) {
        if (n == 0) {
            return JOG_TOK_GT;
        }
        n--;
    }
    if (p->at + n >= p->count) {
        return JOG_TOK_EOF;
    }
    return p->toks[p->at + n].kind;
}

static bool is_eof(const Parser *p) {
    return peek_kind(p) == JOG_TOK_EOF;
}

static JogPos here(const Parser *p) {
    JogPos pos;
    pos.file = p->path;
    if (p->pending_gt) {
        pos.line = p->gt_line;
        pos.column = p->gt_column;
        return pos;
    }
    const JogToken *t = cur(p);
    if (!t) {
        pos.line = 1;
        pos.column = 1;
        return pos;
    }
    pos.line = t->line;
    pos.column = t->column;
    return pos;
}

static void advance(Parser *p) {
    if (p->pending_gt) {
        p->pending_gt = false;
        return;
    }
    if (p->at + 1 < p->count) {
        p->at++;
    }
}

static bool check(const Parser *p, JogTokenKind kind) {
    return peek_kind(p) == kind;
}

static bool eat(Parser *p, JogTokenKind kind) {
    if (!check(p, kind)) {
        return false;
    }
    advance(p);
    return true;
}

static bool expect(Parser *p, JogTokenKind kind, const char *what) {
    if (eat(p, kind)) {
        return true;
    }
    error_here(p, "expected %s, found %s", what, jog_token_spelling(peek_kind(p)));
    return false;
}

static void skip_seps(Parser *p) {
    while (eat(p, JOG_TOK_SEP)) {
    }
}

/* Closes a type-argument list. '>>' yields one '>' and leaves the other. */
static bool at_gt(const Parser *p) {
    if (p->pending_gt) {
        return true;
    }
    JogTokenKind kind = peek_kind(p);
    return kind == JOG_TOK_GT || kind == JOG_TOK_SHR;
}

static bool eat_gt(Parser *p) {
    if (p->pending_gt) {
        p->pending_gt = false;
        return true;
    }
    if (check(p, JOG_TOK_GT)) {
        advance(p);
        return true;
    }
    if (check(p, JOG_TOK_SHR)) {
        const JogToken *t = cur(p);
        if (t) {
            p->gt_line = t->line;
            p->gt_column = t->column + 1;
        }
        advance(p);
        p->pending_gt = true;
        return true;
    }
    return false;
}

static void recover(Parser *p) {
    int depth = 0;
    while (!is_eof(p)) {
        JogTokenKind kind = peek_kind(p);
        if (kind == JOG_TOK_LBRACE) {
            depth++;
            advance(p);
            continue;
        }
        if (kind == JOG_TOK_RBRACE) {
            if (depth == 0) {
                break;
            }
            depth--;
            advance(p);
            continue;
        }
        if (kind == JOG_TOK_SEP && depth == 0) {
            advance(p);
            break;
        }
        advance(p);
    }
    p->panic = false;
}

/* ---- nodes and lists ---- */

static JogList empty_list(void) {
    JogList list = {0};
    return list;
}

static JogNames empty_names(void) {
    JogNames names = {0};
    return names;
}

static bool start_da(Parser *p, CtDa *da, size_t item_size) {
    if (!ct_da_init(da, item_size)) {
        p->oom = true;
        ct_da_free(da);
        return false;
    }
    return true;
}

static JogList take_nodes(Parser *p, CtDa *da) {
    bool oom = false;
    JogList list = jog_list_from_da(p->arena, da, &oom);
    if (oom) {
        p->oom = true;
    }
    ct_da_free(da);
    return list;
}

static JogNames take_names(Parser *p, CtDa *da) {
    bool oom = false;
    JogNames names = jog_names_from_da(p->arena, da, &oom);
    if (oom) {
        p->oom = true;
    }
    ct_da_free(da);
    return names;
}

static void push_node(Parser *p, CtDa *da, JogNode *node) {
    if (!node || p->oom) {
        return;
    }
    if (!ct_da_push(da, &node)) {
        p->oom = true;
    }
}

static void push_name(Parser *p, CtDa *da, const char *name) {
    if (p->oom) {
        return;
    }
    if (!name) {
        name = "";
    }
    if (!ct_da_push(da, &name)) {
        p->oom = true;
    }
}

static JogNode *make_at(Parser *p, JogNodeKind kind, JogPos pos) {
    if (p->oom) {
        return NULL;
    }
    JogNode *node = jog_node_new(p->arena, kind, pos);
    if (!node) {
        p->oom = true;
    }
    return node;
}

static const char *copy_span(Parser *p, const char *text, size_t len) {
    char *copy = ct_arena_alloc(p->arena, len + 1, 1);
    if (!copy) {
        p->oom = true;
        return "";
    }
    if (len > 0 && text) {
        memcpy(copy, text, len);
    }
    copy[len] = '\0';
    return copy;
}

static const char *ident_text(const Parser *p) {
    const JogToken *t = cur(p);
    if (!t || !t->value) {
        return "";
    }
    return t->value;
}

static bool same_place(const Parser *p, size_t at, bool pending) {
    return p->at == at && p->pending_gt == pending;
}

/* ---- names, arguments, parameters ---- */

static bool parse_dot_names(Parser *p, CtDa *names) {
    if (!check(p, JOG_TOK_IDENT)) {
        error_here(p, "expected a name, found %s", jog_token_spelling(peek_kind(p)));
        return false;
    }
    push_name(p, names, ident_text(p));
    advance(p);
    while (check(p, JOG_TOK_DOT) && peek_n(p, 1) == JOG_TOK_IDENT) {
        advance(p);
        push_name(p, names, ident_text(p));
        advance(p);
    }
    return true;
}

static JogList parse_args(Parser *p) {
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    while (!give_up(p) && !is_eof(p) && !check(p, JOG_TOK_RPAREN)) {
        JogPos pos = here(p);
        const char *name = NULL;
        if (check(p, JOG_TOK_IDENT) && peek_n(p, 1) == JOG_TOK_COLON) {
            name = ident_text(p);
            advance(p);
            advance(p);
        }
        JogNode *value = parse_expr(p);
        JogNode *arg = make_at(p, JOG_NODE_ARG, pos);
        if (arg) {
            arg->as.arg.name = name;
            arg->as.arg.value = value;
            push_node(p, &da, arg);
        }
        if (!eat(p, JOG_TOK_COMMA)) {
            break;
        }
    }
    return take_nodes(p, &da);
}

static int is_keyword(JogTokenKind kind) {
    return kind >= JOG_TOK_KW_FN && kind <= JOG_TOK_KW_QUOTE;
}

/* A keyword spelling used as a name. The notes bind `val` and `in`. */
static const char *keyword_name(Parser *p) {
    const char *spelling = jog_token_spelling(peek_kind(p));
    const char *name = ct_intern_cstr(p->intern, spelling ? spelling : "");
    if (!name) {
        p->oom = true;
        return "";
    }
    return name;
}

static JogNode *parse_param(Parser *p, bool allow_defaults) {
    JogPos pos = here(p);
    /* A keyword such as `val` can be a parameter name: `fn put(val: i32)`. */
    int keyword = is_keyword(peek_kind(p)) && peek_n(p, 1) == JOG_TOK_COLON;
    if (!check(p, JOG_TOK_IDENT) && !keyword) {
        error_here(p, "expected a parameter name, found %s", jog_token_spelling(peek_kind(p)));
        return NULL;
    }
    const char *name = keyword ? keyword_name(p) : ident_text(p);
    advance(p);
    expect(p, JOG_TOK_COLON, "':'");
    JogNode *type = NULL;
    if (!give_up(p)) {
        type = parse_type(p);
    }
    JogNode *fallback = NULL;
    if (!give_up(p) && eat(p, JOG_TOK_ASSIGN)) {
        if (!allow_defaults) {
            complain(p, "this parameter cannot have a default value");
        }
        fallback = parse_expr(p);
    }
    JogNode *node = make_at(p, JOG_NODE_PARAM, pos);
    if (!node) {
        return NULL;
    }
    node->as.param.name = name;
    node->as.param.type = type;
    node->as.param.default_value = fallback;
    return node;
}

/* `...T: name`. The element type is param.type; the checker makes the
   local a slice of that type. pack_reject, when set, forbids the form. */
static JogNode *parse_pack_param(Parser *p, const char *pack_reject) {
    JogPos pos = here(p);
    if (pack_reject) {
        complain(p, "%s", pack_reject);
    }
    advance(p); /* ... */
    JogNode *type = NULL;
    if (!give_up(p)) {
        type = parse_type(p);
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_COLON, "':'");
    }
    const char *name = NULL;
    if (!give_up(p)) {
        int keyword = is_keyword(peek_kind(p));
        if (!check(p, JOG_TOK_IDENT) && !keyword) {
            error_here(p, "expected a parameter name, found %s", jog_token_spelling(peek_kind(p)));
        } else {
            name = keyword ? keyword_name(p) : ident_text(p);
            advance(p);
        }
    }
    if (!give_up(p) && eat(p, JOG_TOK_ASSIGN)) {
        complain(p, "a pack parameter cannot have a default value");
        (void)parse_expr(p);
    }
    JogNode *node = make_at(p, JOG_NODE_PARAM, pos);
    if (!node) {
        return NULL;
    }
    node->as.param.name = name;
    node->as.param.type = type;
    node->as.param.pack = true;
    return node;
}

static JogList parse_param_list(Parser *p, bool allow_variadic, bool allow_defaults, const char *pack_reject,
                                bool *variadic) {
    *variadic = false;
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    while (!give_up(p) && !is_eof(p) && !check(p, JOG_TOK_RPAREN)) {
        /* `...T: name` is a pack. A bare `...` is C variadic. */
        if (check(p, JOG_TOK_ELLIPSIS) && type_can_start(peek_n(p, 1))) {
            push_node(p, &da, parse_pack_param(p, pack_reject));
            if (!eat(p, JOG_TOK_COMMA)) {
                break;
            }
            continue;
        }
        if (eat(p, JOG_TOK_ELLIPSIS)) {
            if (!allow_variadic) {
                complain(p, "variadic parameters are only allowed on extern functions");
            }
            if (*variadic) {
                complain(p, "a function can have only one '...'");
            }
            *variadic = true;
            if (eat(p, JOG_TOK_COMMA)) {
                error_here(p, "'...' must be the last parameter");
            }
            break;
        }
        push_node(p, &da, parse_param(p, allow_defaults));
        if (!eat(p, JOG_TOK_COMMA)) {
            break;
        }
    }
    return take_nodes(p, &da);
}

/* ---- types ---- */

static bool type_can_start(JogTokenKind kind) {
    switch (kind) {
    case JOG_TOK_IDENT:
    case JOG_TOK_STAR:
    case JOG_TOK_LBRACKET:
    case JOG_TOK_LPAREN:
    case JOG_TOK_KW_CONST:
    case JOG_TOK_KW_ESCAPING:
    case JOG_TOK_KW_EXTERN:
        return true;
    default:
        return false;
    }
}

static JogList parse_type_args(Parser *p) {
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    advance(p); /* < */
    if (at_gt(p)) {
        error_here(p, "expected a type argument");
        eat_gt(p);
        return take_nodes(p, &da);
    }
    while (!give_up(p) && !is_eof(p)) {
        skip_seps(p);
        if (at_gt(p)) {
            break;
        }
        size_t at = p->at;
        bool pending = p->pending_gt;
        JogNode *type = parse_type(p);
        push_node(p, &da, type);
        if (give_up(p)) {
            break;
        }
        if (same_place(p, at, pending)) {
            error_here(p, "expected a type argument");
            break;
        }
        skip_seps(p);
        if (!eat(p, JOG_TOK_COMMA)) {
            break;
        }
    }
    if (!give_up(p) && !eat_gt(p)) {
        error_here(p, "expected '>', found %s", jog_token_spelling(peek_kind(p)));
    }
    return take_nodes(p, &da);
}

/* Rule 2: '(', ')', ']', '}', ',', ';', '.', '?.', '{', or a newline. */
static bool keeps_type_args(JogTokenKind kind) {
    switch (kind) {
    case JOG_TOK_LPAREN:
    case JOG_TOK_RPAREN:
    case JOG_TOK_RBRACKET:
    case JOG_TOK_RBRACE:
    case JOG_TOK_COMMA:
    case JOG_TOK_SEP:
    case JOG_TOK_DOT:
    case JOG_TOK_QDOT:
    case JOG_TOK_LBRACE:
        return true;
    default:
        return false;
    }
}

static ParserMark mark_here(const Parser *p) {
    ParserMark mark;
    mark.at = p->at;
    mark.pending_gt = p->pending_gt;
    mark.gt_line = p->gt_line;
    mark.gt_column = p->gt_column;
    mark.panic = p->panic;
    return mark;
}

static void mark_back(Parser *p, ParserMark mark) {
    p->at = mark.at;
    p->pending_gt = mark.pending_gt;
    p->gt_line = mark.gt_line;
    p->gt_column = mark.gt_column;
    p->panic = mark.panic;
}

/* After an expression identifier, try type arguments. On failure the parser
   is back at '<' and the list is empty, so '<' is parsed as less-than. */
static JogList parse_expr_type_args(Parser *p) {
    ParserMark mark = mark_here(p);
    p->quiet++;
    JogList trial = parse_type_args(p);
    bool ok = !p->panic && !p->oom && trial.count > 0 && keeps_type_args(peek_kind(p));
    bool oom = p->oom;
    p->quiet--;
    mark_back(p, mark);
    if (oom) {
        p->oom = true;
        return empty_list();
    }
    if (!ok) {
        return empty_list();
    }
    return parse_type_args(p);
}

static JogList parse_type_params(Parser *p) {
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    advance(p); /* < */
    if (at_gt(p)) {
        error_here(p, "expected a type parameter");
        eat_gt(p);
        return take_nodes(p, &da);
    }
    while (!give_up(p) && !is_eof(p)) {
        skip_seps(p);
        if (at_gt(p)) {
            break;
        }
        if (!check(p, JOG_TOK_IDENT)) {
            error_here(p, "expected a type parameter, found %s", jog_token_spelling(peek_kind(p)));
            break;
        }
        JogPos pos = here(p);
        const char *name = ident_text(p);
        advance(p);
        JogNode *constraint = NULL;
        if (!give_up(p) && eat(p, JOG_TOK_COLON)) {
            constraint = parse_type(p);
        }
        JogNode *param = make_at(p, JOG_NODE_TYPE_PARAM, pos);
        if (param) {
            param->as.type_param.name = name;
            param->as.type_param.constraint = constraint;
            push_node(p, &da, param);
        }
        if (give_up(p)) {
            break;
        }
        skip_seps(p);
        if (!eat(p, JOG_TOK_COMMA)) {
            break;
        }
    }
    if (!give_up(p) && !eat_gt(p)) {
        error_here(p, "expected '>', found %s", jog_token_spelling(peek_kind(p)));
    }
    return take_nodes(p, &da);
}

static JogNode *parse_type_name(Parser *p) {
    JogPos pos = here(p);
    CtDa names;
    if (!start_da(p, &names, sizeof(const char *))) {
        return NULL;
    }
    if (!parse_dot_names(p, &names)) {
        ct_da_free(&names);
        return NULL;
    }
    JogList args = empty_list();
    if (!give_up(p) && check(p, JOG_TOK_LT)) {
        args = parse_type_args(p);
    }
    JogNode *node = make_at(p, JOG_NODE_TYPE_NAME, pos);
    if (!node) {
        ct_da_free(&names);
        return NULL;
    }
    node->as.type_name.path = take_names(p, &names);
    node->as.type_name.args = args;
    return node;
}

/* Receiver type arguments are bare names: List<T>, not List<[]i32>. */
static JogNode *parse_receiver_name(Parser *p) {
    JogPos pos = here(p);
    if (!check(p, JOG_TOK_IDENT)) {
        error_here(p, "expected a receiver type, found %s", jog_token_spelling(peek_kind(p)));
        return NULL;
    }
    const char *name = ident_text(p);
    advance(p);
    CtDa args;
    if (!start_da(p, &args, sizeof(JogNode *))) {
        return NULL;
    }
    if (eat(p, JOG_TOK_LT)) {
        while (!give_up(p) && !is_eof(p) && !at_gt(p)) {
            skip_seps(p);
            if (at_gt(p)) {
                break;
            }
            if (!check(p, JOG_TOK_IDENT)) {
                error_here(p, "expected a type parameter, found %s", jog_token_spelling(peek_kind(p)));
                break;
            }
            JogPos arg_pos = here(p);
            const char *arg_name = ident_text(p);
            advance(p);
            CtDa one;
            JogNode *arg = NULL;
            if (start_da(p, &one, sizeof(const char *))) {
                push_name(p, &one, arg_name);
                arg = make_at(p, JOG_NODE_TYPE_NAME, arg_pos);
                if (arg) {
                    arg->as.type_name.path = take_names(p, &one);
                } else {
                    ct_da_free(&one);
                }
            }
            push_node(p, &args, arg);
            skip_seps(p);
            if (!eat(p, JOG_TOK_COMMA)) {
                break;
            }
        }
        if (!give_up(p) && !eat_gt(p)) {
            error_here(p, "expected '>', found %s", jog_token_spelling(peek_kind(p)));
        }
    }
    JogNode *node = make_at(p, JOG_NODE_TYPE_NAME, pos);
    if (!node) {
        ct_da_free(&args);
        return NULL;
    }
    CtDa path;
    if (!start_da(p, &path, sizeof(const char *))) {
        ct_da_free(&args);
        return NULL;
    }
    push_name(p, &path, name);
    node->as.type_name.path = take_names(p, &path);
    node->as.type_name.args = take_nodes(p, &args);
    return node;
}

static JogList parse_fn_type_contexts(Parser *p) {
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    advance(p); /* with */
    while (!give_up(p) && !is_eof(p)) {
        skip_seps(p);
        push_node(p, &da, parse_type(p));
        if (give_up(p)) {
            break;
        }
        skip_seps(p);
        if (!eat(p, JOG_TOK_COMMA)) {
            break;
        }
    }
    return take_nodes(p, &da);
}

/* 'with (name:' belongs to a declaration, not to the function type. */
static bool with_is_param_clause(const Parser *p) {
    return check(p, JOG_TOK_KW_WITH) && peek_n(p, 1) == JOG_TOK_LPAREN && peek_n(p, 2) == JOG_TOK_IDENT &&
           peek_n(p, 3) == JOG_TOK_COLON;
}

static JogNode *finish_fn_type(Parser *p, JogPos pos, JogList params) {
    JogNode *node = make_at(p, JOG_NODE_TYPE_FN, pos);
    if (!node) {
        return NULL;
    }
    node->as.type_fn.params = params;
    node->as.type_fn.ret = parse_type(p);
    if (!give_up(p) && check(p, JOG_TOK_KW_WITH) && !with_is_param_clause(p)) {
        node->as.type_fn.contexts = parse_fn_type_contexts(p);
    }
    return node;
}

static JogNode *parse_paren_type(Parser *p) {
    JogPos pos = here(p);
    advance(p); /* ( */
    if (eat(p, JOG_TOK_RPAREN)) {
        if (eat(p, JOG_TOK_ARROW)) {
            return finish_fn_type(p, pos, empty_list());
        }
        return make_at(p, JOG_NODE_TYPE_UNIT, pos);
    }
    CtDa params;
    if (!start_da(p, &params, sizeof(JogNode *))) {
        return NULL;
    }
    bool comma = false;
    push_node(p, &params, parse_type(p));
    while (!give_up(p) && eat(p, JOG_TOK_COMMA)) {
        if (check(p, JOG_TOK_RPAREN)) {
            break;
        }
        comma = true;
        push_node(p, &params, parse_type(p));
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    if (!give_up(p) && eat(p, JOG_TOK_ARROW)) {
        JogList list = take_nodes(p, &params);
        return finish_fn_type(p, pos, list);
    }
    JogList list = take_nodes(p, &params);
    if (!comma) {
        return list.count > 0 ? list.items[0] : NULL;
    }
    JogNode *tuple = make_at(p, JOG_NODE_TYPE_TUPLE, pos);
    if (!tuple) {
        return NULL;
    }
    tuple->as.type_tuple.elems = list;
    return tuple;
}

static JogNode *parse_bracket_type(Parser *p) {
    JogPos pos = here(p);
    advance(p); /* [ */
    if (eat(p, JOG_TOK_RBRACKET)) {
        bool is_const = eat(p, JOG_TOK_KW_CONST);
        JogNode *elem = parse_type(p);
        JogNode *node = make_at(p, JOG_NODE_TYPE_SLICE, pos);
        if (!node) {
            return NULL;
        }
        node->as.type_elem.elem = elem;
        node->as.type_elem.is_const = is_const;
        return node;
    }
    bool infer = false;
    JogNode *len = NULL;
    const JogToken *t = cur(p);
    if (t && t->kind == JOG_TOK_IDENT && t->value && strcmp(t->value, "_") == 0 && peek_n(p, 1) == JOG_TOK_RBRACKET) {
        infer = true;
        advance(p);
    } else if (!give_up(p)) {
        len = parse_expr(p);
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_RBRACKET, "']'");
    }
    if (!give_up(p) && eat(p, JOG_TOK_KW_CONST)) {
        complain(p, "const is not allowed on an array type");
    }
    JogNode *elem = NULL;
    if (!give_up(p)) {
        elem = parse_type(p);
    }
    JogNode *node = make_at(p, JOG_NODE_TYPE_ARRAY, pos);
    if (!node) {
        return NULL;
    }
    node->as.type_array.len = len;
    node->as.type_array.infer_len = infer;
    node->as.type_array.elem = elem;
    return node;
}

static JogNode *parse_non_null(Parser *p) {
    if (give_up(p)) {
        return NULL;
    }
    JogPos pos = here(p);
    JogTokenKind kind = peek_kind(p);
    if (kind == JOG_TOK_STAR) {
        advance(p);
        bool is_const = eat(p, JOG_TOK_KW_CONST);
        JogNode *elem = parse_non_null(p);
        JogNode *node = make_at(p, JOG_NODE_TYPE_PTR, pos);
        if (!node) {
            return NULL;
        }
        node->as.type_elem.elem = elem;
        node->as.type_elem.is_const = is_const;
        return node;
    }
    if (kind == JOG_TOK_LBRACKET) {
        return parse_bracket_type(p);
    }
    if (kind == JOG_TOK_LPAREN) {
        return parse_paren_type(p);
    }
    if (kind == JOG_TOK_KW_CONST) {
        advance(p);
        if (!check(p, JOG_TOK_IDENT)) {
            error_here(p, "expected a type name after 'const'");
            return NULL;
        }
        JogNode *named = parse_type_name(p);
        if (named) {
            named->as.type_name.is_const = true;
        }
        return named;
    }
    if (kind == JOG_TOK_KW_ESCAPING || kind == JOG_TOK_KW_EXTERN) {
        bool is_extern = kind == JOG_TOK_KW_EXTERN;
        advance(p);
        if (!check(p, JOG_TOK_LPAREN)) {
            error_here(p, "expected a function type after '%s'", is_extern ? "extern" : "escaping");
            return NULL;
        }
        JogNode *fn = parse_paren_type(p);
        if (fn && fn->kind == JOG_NODE_TYPE_FN) {
            fn->pos = pos;
            if (is_extern) {
                fn->as.type_fn.is_extern = true;
            } else {
                fn->as.type_fn.escaping = true;
            }
        } else if (fn) {
            error_here(p, "expected '->' in a function type");
        }
        return fn;
    }
    if (kind == JOG_TOK_IDENT) {
        return parse_type_name(p);
    }
    error_here(p, "expected a type, found %s", jog_token_spelling(kind));
    return NULL;
}

static JogNode *parse_type(Parser *p) {
    if (give_up(p)) {
        return NULL;
    }
    JogNode *inner = parse_non_null(p);
    if (!inner || give_up(p)) {
        return inner;
    }
    if (!eat(p, JOG_TOK_QUESTION)) {
        return inner;
    }
    JogNode *opt = make_at(p, JOG_NODE_TYPE_OPTION, inner->pos);
    if (!opt) {
        return NULL;
    }
    opt->as.type_elem.elem = inner;
    return opt;
}

/* ---- expressions ---- */

static int infix_prec(JogTokenKind kind) {
    switch (kind) {
    case JOG_TOK_PIPEPIPE:
        return 1;
    case JOG_TOK_AMPAMP:
        return 2;
    case JOG_TOK_EQ:
    case JOG_TOK_NEQ:
    case JOG_TOK_LT:
    case JOG_TOK_LE:
    case JOG_TOK_GT:
    case JOG_TOK_GE:
        return 3;
    case JOG_TOK_KW_IS:
        return 4;
    case JOG_TOK_CMP3:
        return 5;
    case JOG_TOK_ELVIS:
        return 6;
    case JOG_TOK_DOTDOT:
        return 7;
    case JOG_TOK_PIPE:
        return 8;
    case JOG_TOK_CARET:
        return 9;
    case JOG_TOK_AMP:
        return 10;
    case JOG_TOK_SHL:
    case JOG_TOK_SHR:
        return 11;
    case JOG_TOK_PLUS:
    case JOG_TOK_MINUS:
        return 12;
    case JOG_TOK_STAR:
    case JOG_TOK_SLASH:
    case JOG_TOK_PERCENT:
        return 13;
    case JOG_TOK_KW_AS:
    case JOG_TOK_AS_Q:
        return 14;
    default:
        return -1;
    }
}

static bool non_assoc(JogTokenKind kind) {
    switch (kind) {
    case JOG_TOK_EQ:
    case JOG_TOK_NEQ:
    case JOG_TOK_LT:
    case JOG_TOK_LE:
    case JOG_TOK_GT:
    case JOG_TOK_GE:
    case JOG_TOK_KW_IS:
    case JOG_TOK_CMP3:
    case JOG_TOK_DOTDOT:
        return true;
    default:
        return false;
    }
}

static bool is_prefix(JogTokenKind kind) {
    return kind == JOG_TOK_MINUS || kind == JOG_TOK_BANG || kind == JOG_TOK_TILDE || kind == JOG_TOK_STAR ||
           kind == JOG_TOK_AMP;
}

static bool is_assign_op(JogTokenKind kind) {
    switch (kind) {
    case JOG_TOK_ASSIGN:
    case JOG_TOK_PLUS_ASSIGN:
    case JOG_TOK_MINUS_ASSIGN:
    case JOG_TOK_STAR_ASSIGN:
    case JOG_TOK_SLASH_ASSIGN:
    case JOG_TOK_PERCENT_ASSIGN:
    case JOG_TOK_AMP_ASSIGN:
    case JOG_TOK_PIPE_ASSIGN:
    case JOG_TOK_CARET_ASSIGN:
    case JOG_TOK_SHL_ASSIGN:
    case JOG_TOK_SHR_ASSIGN:
        return true;
    default:
        return false;
    }
}

static void reject_later_expr(Parser *p) {
    error_here(p, "expected an expression, found %s", jog_token_spelling(peek_kind(p)));
}

static JogNode *parse_string_piece(Parser *p, JogNodeKind kind) {
    const JogToken *t = cur(p);
    JogPos pos = here(p);
    JogNode *node = make_at(p, kind, pos);
    if (!node) {
        return NULL;
    }
    if (t && t->value) {
        node->as.literal.text = t->value;
        node->as.literal.len = t->value_len;
    } else {
        node->as.literal.text = "";
        node->as.literal.len = 0;
    }
    advance(p);
    return node;
}

static JogNode *parse_interp(Parser *p) {
    JogPos pos = here(p);
    CtDa parts;
    if (!start_da(p, &parts, sizeof(JogNode *))) {
        return NULL;
    }
    push_node(p, &parts, parse_string_piece(p, JOG_NODE_STRING_LIT));
    while (!give_up(p) && !is_eof(p)) {
        push_node(p, &parts, parse_expr(p));
        if (check(p, JOG_TOK_STR_MID)) {
            push_node(p, &parts, parse_string_piece(p, JOG_NODE_STRING_LIT));
            continue;
        }
        if (check(p, JOG_TOK_STR_TAIL)) {
            push_node(p, &parts, parse_string_piece(p, JOG_NODE_STRING_LIT));
            break;
        }
        error_here(p, "expected the rest of an interpolated string");
        break;
    }
    JogNode *node = make_at(p, JOG_NODE_INTERP, pos);
    if (!node) {
        ct_da_free(&parts);
        return NULL;
    }
    node->as.interp.parts = take_nodes(p, &parts);
    return node;
}

static JogNode *parse_paren_expr(Parser *p) {
    JogPos pos = here(p);
    advance(p);
    if (check(p, JOG_TOK_RPAREN)) {
        error_here(p, "expected an expression");
        advance(p);
        return NULL;
    }
    CtDa elems;
    if (!start_da(p, &elems, sizeof(JogNode *))) {
        return NULL;
    }
    bool many = false;
    push_node(p, &elems, parse_expr(p));
    while (!give_up(p) && eat(p, JOG_TOK_COMMA)) {
        if (check(p, JOG_TOK_RPAREN)) {
            break;
        }
        many = true;
        push_node(p, &elems, parse_expr(p));
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    JogList list = take_nodes(p, &elems);
    if (!many) {
        return list.count > 0 ? list.items[0] : NULL;
    }
    JogNode *tuple = make_at(p, JOG_NODE_TUPLE, pos);
    if (!tuple) {
        return NULL;
    }
    tuple->as.tuple.elems = list;
    return tuple;
}

static JogNode *parse_if(Parser *p) {
    JogPos pos = here(p);
    advance(p);
    JogNode *cond = NULL;
    if (!give_up(p)) {
        /* No trailing lambda in the head, so '{' opens the body. */
        cond = parse_expr_trail(p, false);
    }
    JogNode *then_block = NULL;
    if (!give_up(p)) {
        then_block = parse_block(p);
    }
    JogNode *else_branch = NULL;
    if (!give_up(p)) {
        skip_seps(p);
        if (eat(p, JOG_TOK_KW_ELSE)) {
            skip_seps(p);
            if (check(p, JOG_TOK_KW_IF)) {
                else_branch = parse_if(p);
            } else {
                else_branch = parse_block(p);
            }
        }
    }
    JogNode *node = make_at(p, JOG_NODE_IF, pos);
    if (!node) {
        return NULL;
    }
    node->as.if_.cond = cond;
    node->as.if_.then_block = then_block;
    node->as.if_.else_branch = else_branch;
    return node;
}

/* [4]T(...), [_]T(...), and []T(...). The bracket type stops at the '('.
   Without the '(', the brackets are the type itself: size([4]u8). */
static JogNode *parse_array_lit(Parser *p) {
    JogPos pos = here(p);
    JogNode *type = parse_bracket_type(p);
    if (type && !give_up(p) && !check(p, JOG_TOK_LPAREN)) {
        return type;
    }
    if (type) {
        pos = type->pos;
    }
    JogList elems = empty_list();
    if (!give_up(p)) {
        expect(p, JOG_TOK_LPAREN, "'('");
    }
    if (!give_up(p)) {
        elems = parse_args(p);
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    JogNode *node = make_at(p, JOG_NODE_ARRAY_LIT, pos);
    if (!node) {
        return NULL;
    }
    node->as.array_lit.type = type;
    node->as.array_lit.elems = elems;
    return node;
}

static JogNode *parse_primary(Parser *p) {
    if (give_up(p)) {
        return NULL;
    }
    JogPos pos = here(p);
    JogTokenKind kind = peek_kind(p);
    const JogToken *t = cur(p);
    switch (kind) {
    case JOG_TOK_IDENT: {
        const char *name = ident_text(p);
        advance(p);
        JogList args = empty_list();
        if (!give_up(p) && check(p, JOG_TOK_LT)) {
            args = parse_expr_type_args(p);
        }
        JogNode *node = make_at(p, JOG_NODE_IDENT, pos);
        if (!node) {
            return NULL;
        }
        node->as.ident.name = name;
        node->as.ident.type_args = args;
        return node;
    }
    case JOG_TOK_KW_IN: {
        /* `in` is also a value name in the notes (`in.close()`). */
        const char *name = ct_intern_cstr(p->intern, jog_token_spelling(kind));
        if (!name) {
            p->oom = true;
            return NULL;
        }
        JogNode *node = make_at(p, JOG_NODE_IDENT, pos);
        if (!node) {
            return NULL;
        }
        node->as.ident.name = name;
        advance(p);
        return node;
    }
    case JOG_TOK_INT:
    case JOG_TOK_FLOAT: {
        JogNodeKind lit = kind == JOG_TOK_INT ? JOG_NODE_INT_LIT : JOG_NODE_FLOAT_LIT;
        JogNode *node = make_at(p, lit, pos);
        if (!node || !t) {
            return node;
        }
        node->as.literal.text = copy_span(p, t->start, t->length);
        node->as.literal.len = t->length;
        advance(p);
        return node;
    }
    case JOG_TOK_STRING:
    case JOG_TOK_CSTRING:
    case JOG_TOK_CHAR:
    case JOG_TOK_HEXBLOB: {
        JogNodeKind lit = JOG_NODE_STRING_LIT;
        if (kind == JOG_TOK_CSTRING) {
            lit = JOG_NODE_CSTRING_LIT;
        } else if (kind == JOG_TOK_CHAR) {
            lit = JOG_NODE_CHAR_LIT;
        } else if (kind == JOG_TOK_HEXBLOB) {
            lit = JOG_NODE_HEXBLOB_LIT;
        }
        return parse_string_piece(p, lit);
    }
    case JOG_TOK_KW_TRUE:
    case JOG_TOK_KW_FALSE: {
        JogNode *node = make_at(p, JOG_NODE_BOOL_LIT, pos);
        if (!node) {
            return NULL;
        }
        node->as.literal.value = kind == JOG_TOK_KW_TRUE;
        node->as.literal.text = node->as.literal.value ? "true" : "false";
        node->as.literal.len = node->as.literal.value ? 4 : 5;
        advance(p);
        return node;
    }
    case JOG_TOK_KW_NULL:
        advance(p);
        return make_at(p, JOG_NODE_NULL_LIT, pos);
    case JOG_TOK_STR_HEAD:
        return parse_interp(p);
    case JOG_TOK_LPAREN:
        if (at_paren_lambda(p)) {
            return parse_lambda(p);
        }
        return parse_paren_expr(p);
    case JOG_TOK_DOT:
        if (peek_n(p, 1) == JOG_TOK_IDENT) {
            advance(p);
            JogNode *node = make_at(p, JOG_NODE_DOT_VARIANT, pos);
            if (!node) {
                return NULL;
            }
            node->as.ident.name = ident_text(p);
            advance(p);
            return node;
        }
        reject_later_expr(p);
        return NULL;
    case JOG_TOK_ATTR: {
        const char *name = t && t->value ? t->value : "";
        advance(p);
        if (!expect(p, JOG_TOK_LPAREN, "'('")) {
            return NULL;
        }
        JogList args = parse_args(p);
        expect(p, JOG_TOK_RPAREN, "')'");
        JogNode *node = make_at(p, JOG_NODE_BUILTIN, pos);
        if (!node) {
            return NULL;
        }
        node->as.builtin.name = name;
        node->as.builtin.args = args;
        return node;
    }
    case JOG_TOK_LBRACE:
        return parse_block(p);
    case JOG_TOK_LBRACKET:
        return parse_array_lit(p);
    case JOG_TOK_KW_WHEN:
    case JOG_TOK_KW_GO:
    case JOG_TOK_KW_QUOTE:
        reject_later_expr(p);
        return NULL;
    default:
        reject_later_expr(p);
        return NULL;
    }
}

static uint32_t tuple_index(Parser *p, const JogToken *t) {
    uint32_t acc = 0;
    bool any = false;
    bool bad = false;
    if (!t) {
        bad = true;
    }
    for (size_t i = 0; t && i < t->length; i++) {
        char c = t->start[i];
        if (c == '_') {
            continue;
        }
        if (c < '0' || c > '9') {
            bad = true;
            break;
        }
        any = true;
        uint32_t digit = (uint32_t)(c - '0');
        if (acc > (UINT32_MAX - digit) / 10u) {
            bad = true;
            break;
        }
        acc = acc * 10u + digit;
    }
    if (!any || bad) {
        error_here(p, "expected a decimal tuple index");
        return 0;
    }
    return acc;
}

static JogNode *parse_member(Parser *p, JogNode *base, JogPos pos, bool safe) {
    JogNode *node = make_at(p, JOG_NODE_MEMBER, pos);
    if (!node) {
        return NULL;
    }
    node->as.member.base = base;
    node->as.member.safe = safe;
    if (check(p, JOG_TOK_IDENT)) {
        node->as.member.name = ident_text(p);
        advance(p);
        /* Same rule as a name: obj.method<T>(x) is a generic call.
           A comparison such as obj.method < other backtracks. */
        if (!give_up(p) && check(p, JOG_TOK_LT)) {
            node->as.member.type_args = parse_expr_type_args(p);
        }
        return node;
    }
    if (!safe && check(p, JOG_TOK_INT)) {
        node->as.member.tuple_index = tuple_index(p, cur(p));
        advance(p);
        return node;
    }
    error_here(p, "expected a member name, found %s", jog_token_spelling(peek_kind(p)));
    return node;
}

static JogList parse_expr_list(Parser *p, bool trail) {
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    while (!give_up(p) && !is_eof(p) && !check(p, JOG_TOK_RPAREN)) {
        push_node(p, &da, parse_expr_trail(p, trail));
        if (!eat(p, JOG_TOK_COMMA)) {
            break;
        }
    }
    return take_nodes(p, &da);
}

static JogNode *parse_call(Parser *p, JogNode *callee) {
    JogPos pos = callee ? callee->pos : here(p);
    advance(p); /* ( */
    JogList args = parse_args(p);
    expect(p, JOG_TOK_RPAREN, "')'");
    JogNode *node = make_at(p, JOG_NODE_CALL, pos);
    if (!node) {
        return NULL;
    }
    node->as.call.callee = callee;
    node->as.call.args = args;
    return node;
}

static JogNode *make_slice(Parser *p, JogNode *base, JogNode *lo, JogNode *hi, JogPos pos) {
    JogNode *node = make_at(p, JOG_NODE_SLICE, pos);
    if (!node) {
        return NULL;
    }
    node->as.slice.base = base;
    node->as.slice.lo = lo;
    node->as.slice.hi = hi;
    return node;
}

static JogNode *parse_index_or_slice(Parser *p, JogNode *base, bool trail) {
    JogPos pos = here(p);
    advance(p); /* [ */
    if (eat(p, JOG_TOK_DOTDOT)) {
        JogNode *hi = NULL;
        if (!check(p, JOG_TOK_RBRACKET) && !give_up(p)) {
            hi = parse_slice_bound(p, trail);
        }
        expect(p, JOG_TOK_RBRACKET, "']'");
        return make_slice(p, base, NULL, hi, pos);
    }
    JogNode *mid = parse_slice_bound(p, trail);
    if (!give_up(p) && eat(p, JOG_TOK_DOTDOT)) {
        JogNode *hi = NULL;
        if (!check(p, JOG_TOK_RBRACKET) && !give_up(p)) {
            hi = parse_slice_bound(p, trail);
        }
        expect(p, JOG_TOK_RBRACKET, "']'");
        return make_slice(p, base, mid, hi, pos);
    }
    expect(p, JOG_TOK_RBRACKET, "']'");
    JogNode *node = make_at(p, JOG_NODE_INDEX, pos);
    if (!node) {
        return NULL;
    }
    node->as.index.base = base;
    node->as.index.index = mid;
    return node;
}

/* A bare '{...}' after a non-call becomes a call with no parenthesized args. */
static JogNode *attach_trailing(Parser *p, JogNode *node) {
    JogNode *lam = parse_trailing_lambda(p);
    if (node && node->kind == JOG_NODE_CALL && !node->as.call.trailing) {
        node->as.call.trailing = lam;
        return node;
    }
    JogPos pos = node ? node->pos : here(p);
    JogNode *call = make_at(p, JOG_NODE_CALL, pos);
    if (!call) {
        return NULL;
    }
    call->as.call.callee = node;
    call->as.call.trailing = lam;
    return call;
}

static JogNode *parse_postfix(Parser *p, bool trail) {
    JogNode *node = parse_primary(p);
    while (node && !give_up(p)) {
        if (check(p, JOG_TOK_DOT) || check(p, JOG_TOK_QDOT)) {
            bool safe = check(p, JOG_TOK_QDOT);
            JogPos pos = here(p);
            advance(p);
            node = parse_member(p, node, pos, safe);
        } else if (check(p, JOG_TOK_LPAREN)) {
            node = parse_call(p, node);
        } else if (check(p, JOG_TOK_LBRACKET)) {
            node = parse_index_or_slice(p, node, trail);
        } else if (check(p, JOG_TOK_BANGBANG)) {
            JogPos pos = here(p);
            advance(p);
            JogNode *force = make_at(p, JOG_NODE_FORCE, pos);
            if (!force) {
                return NULL;
            }
            force->as.unary.op = JOG_TOK_BANGBANG;
            force->as.unary.operand = node;
            node = force;
        } else if (check(p, JOG_TOK_KW_WITH) && peek_n(p, 1) == JOG_TOK_LPAREN && node->kind == JOG_NODE_CALL) {
            advance(p);
            expect(p, JOG_TOK_LPAREN, "'('");
            node->as.call.contexts = parse_expr_list(p, true);
            expect(p, JOG_TOK_RPAREN, "')'");
        } else if (trail && check(p, JOG_TOK_LBRACE)) {
            /* A '{' after a SEP is a new statement. The lexer inserted that SEP, so it is not seen here. */
            node = attach_trailing(p, node);
        } else {
            break;
        }
    }
    return node;
}

static bool type_closer(JogTokenKind kind) {
    return kind == JOG_TOK_RPAREN || kind == JOG_TOK_COMMA || kind == JOG_TOK_RBRACKET || kind == JOG_TOK_RBRACE ||
           kind == JOG_TOK_SEP || kind == JOG_TOK_EOF;
}

static JogNode *parse_unary(Parser *p, bool trail) {
    if (give_up(p)) {
        return NULL;
    }
    JogTokenKind kind = peek_kind(p);
    /* *const T is a type, not a dereference of a const value. */
    if (kind == JOG_TOK_STAR && peek_n(p, 1) == JOG_TOK_KW_CONST) {
        return parse_type(p);
    }
    if (!is_prefix(kind)) {
        return parse_postfix(p, trail);
    }
    JogPos pos = here(p);
    advance(p);
    JogNode *operand = parse_unary(p, trail);
    JogNode *node = make_at(p, JOG_NODE_UNARY, pos);
    if (!node) {
        return NULL;
    }
    node->as.unary.op = kind;
    node->as.unary.operand = operand;
    return node;
}

static JogNode *parse_prec(Parser *p, int min_prec, bool range_ok, bool trail) {
    if (give_up(p)) {
        return NULL;
    }
    JogNode *left = parse_unary(p, trail);
    if (!left) {
        return NULL;
    }
    /* T? binds loosest. Only when the next token closes a type argument,
       so a value expression does not swallow a following operator. */
    while (!give_up(p) && peek_kind(p) == JOG_TOK_QUESTION && type_closer(peek_n(p, 1))) {
        advance(p);
        JogNode *opt = make_at(p, JOG_NODE_TYPE_OPTION, left->pos);
        if (!opt) {
            return NULL;
        }
        opt->as.type_elem.elem = left;
        left = opt;
    }
    for (;;) {
        if (give_up(p)) {
            return left;
        }
        JogTokenKind op = peek_kind(p);
        /* A bracket uses '..' as the slice separator, so a bound stops there. */
        if (op == JOG_TOK_DOTDOT && !range_ok) {
            break;
        }
        int prec = infix_prec(op);
        if (prec < min_prec) {
            break;
        }
        JogPos pos = here(p);
        advance(p);
        if (op == JOG_TOK_KW_AS || op == JOG_TOK_AS_Q) {
            JogNode *type = parse_type(p);
            JogNode *cast = make_at(p, JOG_NODE_CAST, pos);
            if (!cast) {
                return NULL;
            }
            cast->as.cast.op = op;
            cast->as.cast.expr = left;
            cast->as.cast.type = type;
            left = cast;
        } else if (op == JOG_TOK_KW_IS) {
            JogNode *type = parse_type(p);
            JogNode *test = make_at(p, JOG_NODE_IS, pos);
            if (!test) {
                return NULL;
            }
            test->as.is.expr = left;
            test->as.is.type = type;
            left = test;
        } else {
            JogNode *right = parse_prec(p, prec + 1, range_ok, trail);
            JogNode *bin = make_at(p, JOG_NODE_BINARY, pos);
            if (!bin) {
                return NULL;
            }
            bin->as.binary.op = op;
            bin->as.binary.lhs = left;
            bin->as.binary.rhs = right;
            left = bin;
        }
        if (non_assoc(op) && infix_prec(peek_kind(p)) == prec) {
            error_here(p, "'%s' does not chain", jog_token_spelling(op));
            break;
        }
    }
    return left;
}

/* Rule 3: one token after the matching ')' decides a lambda. */
static bool at_paren_lambda(const Parser *p) {
    if (!check(p, JOG_TOK_LPAREN)) {
        return false;
    }
    int depth = 0;
    size_t n = 0;
    for (;;) {
        JogTokenKind kind = peek_n(p, n);
        if (kind == JOG_TOK_EOF) {
            return false;
        }
        if (kind == JOG_TOK_LPAREN) {
            depth++;
        } else if (kind == JOG_TOK_RPAREN) {
            depth--;
            if (depth == 0) {
                JogTokenKind next = peek_n(p, n + 1);
                return next == JOG_TOK_COLON || next == JOG_TOK_ARROW;
            }
        }
        n++;
    }
}

static JogNode *parse_lambda(Parser *p) {
    JogPos pos = here(p);
    advance(p); /* ( */
    CtDa params;
    if (!start_da(p, &params, sizeof(JogNode *))) {
        return NULL;
    }
    if (!check(p, JOG_TOK_RPAREN)) {
        while (!give_up(p) && !is_eof(p) && !check(p, JOG_TOK_RPAREN)) {
            JogPos param_pos = here(p);
            if (!check(p, JOG_TOK_IDENT)) {
                error_here(p, "expected a parameter name, found %s", jog_token_spelling(peek_kind(p)));
                break;
            }
            const char *name = ident_text(p);
            advance(p);
            JogNode *type = NULL;
            if (!give_up(p) && eat(p, JOG_TOK_COLON)) {
                type = parse_type(p);
            }
            JogNode *param = make_at(p, JOG_NODE_PARAM, param_pos);
            if (param) {
                param->as.param.name = name;
                param->as.param.type = type;
                push_node(p, &params, param);
            }
            if (give_up(p) || !eat(p, JOG_TOK_COMMA)) {
                break;
            }
            if (check(p, JOG_TOK_RPAREN)) {
                break;
            }
        }
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    JogNode *ret = NULL;
    if (!give_up(p) && eat(p, JOG_TOK_COLON)) {
        ret = parse_type(p);
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_ARROW, "'->'");
    }
    bool expr_body = false;
    JogNode *body = NULL;
    if (!give_up(p)) {
        if (check(p, JOG_TOK_LBRACE)) {
            body = parse_block(p);
        } else {
            expr_body = true;
            body = parse_expr(p);
        }
    }
    JogList list = take_nodes(p, &params);
    JogNode *node = make_at(p, JOG_NODE_LAMBDA, pos);
    if (!node) {
        return NULL;
    }
    node->as.lambda.params = list;
    node->as.lambda.ret = ret;
    node->as.lambda.body = body;
    node->as.lambda.expr_body = expr_body;
    node->as.lambda.implicit_it = false;
    return node;
}

static JogNode *parse_pattern(Parser *p, bool in_arm);

static bool pattern_stops(const Parser *p, size_t ahead, bool in_arm) {
    JogTokenKind kind = peek_n(p, ahead);
    if (in_arm) {
        return kind == JOG_TOK_ARROW || kind == JOG_TOK_COMMA || kind == JOG_TOK_RBRACE || kind == JOG_TOK_SEP ||
               kind == JOG_TOK_EOF;
    }
    return kind == JOG_TOK_RPAREN || kind == JOG_TOK_COMMA || kind == JOG_TOK_EOF;
}

/* Rule 7: IDENT(...) in a pattern is always a variant pattern. Name resolution
   decides whether it was really a call compared with ==. */
static JogNode *parse_variant_pat(Parser *p) {
    JogPos pos = here(p);
    const char *name = ident_text(p);
    advance(p);
    advance(p); /* ( */
    CtDa fields;
    if (!start_da(p, &fields, sizeof(JogNode *))) {
        return NULL;
    }
    if (!check(p, JOG_TOK_RPAREN)) {
        while (!give_up(p) && !is_eof(p) && !check(p, JOG_TOK_RPAREN)) {
            JogPos field_pos = here(p);
            const char *field_name = NULL;
            if (check(p, JOG_TOK_IDENT) && peek_n(p, 1) == JOG_TOK_COLON) {
                field_name = ident_text(p);
                advance(p);
                advance(p);
            }
            JogNode *pat = parse_pattern(p, false);
            JogNode *field = make_at(p, JOG_NODE_PAT_FIELD, field_pos);
            if (field) {
                field->as.pat_field.name = field_name;
                field->as.pat_field.pattern = pat;
                push_node(p, &fields, field);
            }
            if (give_up(p) || !eat(p, JOG_TOK_COMMA)) {
                break;
            }
            if (check(p, JOG_TOK_RPAREN)) {
                break;
            }
        }
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    JogList list = take_nodes(p, &fields);
    JogNode *node = make_at(p, JOG_NODE_PAT_VARIANT, pos);
    if (!node) {
        return NULL;
    }
    node->as.pat_variant.name = name;
    node->as.pat_variant.fields = list;
    return node;
}

static JogNode *parse_pattern(Parser *p, bool in_arm) {
    if (give_up(p)) {
        return NULL;
    }
    if (in_arm && check(p, JOG_TOK_KW_IS)) {
        JogPos pos = here(p);
        advance(p);
        JogNode *type = NULL;
        if (!give_up(p)) {
            type = parse_type(p);
        }
        JogNode *node = make_at(p, JOG_NODE_PAT_IS, pos);
        if (!node) {
            return NULL;
        }
        node->as.pat_is.type = type;
        return node;
    }
    if (check(p, JOG_TOK_KW_IN)) {
        JogPos pos = here(p);
        advance(p);
        JogNode *expr = NULL;
        if (!give_up(p)) {
            expr = parse_expr(p);
        }
        JogNode *node = make_at(p, JOG_NODE_PAT_IN, pos);
        if (!node) {
            return NULL;
        }
        node->as.pat_expr.expr = expr;
        return node;
    }
    if (check(p, JOG_TOK_IDENT) && peek_n(p, 1) == JOG_TOK_LPAREN) {
        return parse_variant_pat(p);
    }
    if (check(p, JOG_TOK_IDENT) && pattern_stops(p, 1, in_arm)) {
        JogPos pos = here(p);
        const char *name = ident_text(p);
        advance(p);
        if (name && strcmp(name, "_") == 0) {
            return make_at(p, JOG_NODE_PAT_WILDCARD, pos);
        }
        JogNode *node = make_at(p, JOG_NODE_PAT_BIND, pos);
        if (!node) {
            return NULL;
        }
        node->as.ident.name = name;
        return node;
    }
    JogPos pos = here(p);
    JogNode *expr = parse_expr(p);
    if (!expr) {
        return NULL;
    }
    JogNode *node = make_at(p, JOG_NODE_PAT_EXPR, pos);
    if (!node) {
        return NULL;
    }
    node->as.pat_expr.expr = expr;
    return node;
}

static JogNode *parse_when_arm(Parser *p) {
    JogPos pos = here(p);
    CtDa conds;
    if (!start_da(p, &conds, sizeof(JogNode *))) {
        return NULL;
    }
    if (!eat(p, JOG_TOK_KW_ELSE)) {
        while (!give_up(p) && !is_eof(p) && !check(p, JOG_TOK_ARROW) && !check(p, JOG_TOK_RBRACE)) {
            size_t at = p->at;
            bool pending = p->pending_gt;
            push_node(p, &conds, parse_pattern(p, true));
            if (give_up(p)) {
                break;
            }
            if (same_place(p, at, pending)) {
                error_here(p, "expected a pattern, found %s", jog_token_spelling(peek_kind(p)));
                break;
            }
            if (!eat(p, JOG_TOK_COMMA)) {
                break;
            }
            skip_seps(p);
        }
    }
    if (!give_up(p)) {
        skip_seps(p);
        expect(p, JOG_TOK_ARROW, "'->'");
    }
    JogNode *body = NULL;
    if (!give_up(p)) {
        if (check(p, JOG_TOK_LBRACE)) {
            body = parse_block(p);
        } else {
            body = parse_expr(p);
        }
    }
    JogList list = take_nodes(p, &conds);
    JogNode *arm = make_at(p, JOG_NODE_WHEN_ARM, pos);
    if (!arm) {
        return NULL;
    }
    arm->as.when_arm.conds = list;
    arm->as.when_arm.body = body;
    return arm;
}

static JogNode *parse_when(Parser *p) {
    JogPos pos = here(p);
    advance(p);
    JogNode *subject = NULL;
    if (!give_up(p)) {
        skip_seps(p);
        /* Rule 1: '{' after the subject opens the body, not a trailing lambda. */
        if (!check(p, JOG_TOK_LBRACE)) {
            subject = parse_expr_trail(p, false);
            if (!give_up(p)) {
                skip_seps(p);
            }
        }
    }
    bool opened = false;
    if (!give_up(p)) {
        opened = expect(p, JOG_TOK_LBRACE, "'{'");
    }
    CtDa arms;
    if (!start_da(p, &arms, sizeof(JogNode *))) {
        return NULL;
    }
    if (opened) {
        skip_seps(p);
        while (!p->oom && !is_eof(p) && !check(p, JOG_TOK_RBRACE)) {
            size_t at = p->at;
            bool pending = p->pending_gt;
            JogNode *arm = NULL;
            if (!give_up(p)) {
                arm = parse_when_arm(p);
            }
            if (p->panic) {
                recover(p);
            }
            push_node(p, &arms, arm);
            if (same_place(p, at, pending)) {
                if (!p->panic) {
                    error_here(p, "expected a when arm, found %s", jog_token_spelling(peek_kind(p)));
                }
                recover(p);
                if (same_place(p, at, pending)) {
                    advance(p);
                }
            }
            skip_seps(p);
        }
        if (!give_up(p)) {
            expect(p, JOG_TOK_RBRACE, "'}'");
        } else if (check(p, JOG_TOK_RBRACE)) {
            advance(p);
            p->panic = false;
        }
    }
    JogList list = take_nodes(p, &arms);
    JogNode *node = make_at(p, JOG_NODE_WHEN, pos);
    if (!node) {
        return NULL;
    }
    node->as.when.subject = subject;
    node->as.when.arms = list;
    return node;
}

/* go(stack: n) is a launch-argument list when ')' is followed by '{' or 'with'.
   Otherwise the parentheses are the body expression. */
static bool go_paren_is_launch(const Parser *p) {
    if (!check(p, JOG_TOK_LPAREN)) {
        return false;
    }
    int depth = 0;
    size_t n = 0;
    for (;;) {
        JogTokenKind kind = peek_n(p, n);
        if (kind == JOG_TOK_EOF) {
            return false;
        }
        if (kind == JOG_TOK_LPAREN) {
            depth++;
        } else if (kind == JOG_TOK_RPAREN) {
            depth--;
            if (depth == 0) {
                JogTokenKind next = peek_n(p, n + 1);
                return next == JOG_TOK_LBRACE || next == JOG_TOK_KW_WITH;
            }
        }
        n++;
    }
}

static JogNode *parse_go(Parser *p) {
    JogPos pos = here(p);
    advance(p);
    JogList args = empty_list();
    JogList contexts = empty_list();
    if (!give_up(p) && go_paren_is_launch(p)) {
        advance(p); /* ( */
        args = parse_args(p);
        if (!give_up(p)) {
            expect(p, JOG_TOK_RPAREN, "')'");
        }
    }
    if (!give_up(p) && check(p, JOG_TOK_KW_WITH)) {
        advance(p);
        if (!give_up(p)) {
            expect(p, JOG_TOK_LPAREN, "'('");
        }
        if (!give_up(p)) {
            contexts = parse_expr_list(p, true);
            expect(p, JOG_TOK_RPAREN, "')'");
        }
        if (!give_up(p) && contexts.count == 0) {
            complain(p, "expected an expression");
        }
    }
    JogNode *body = NULL;
    if (!give_up(p)) {
        if (check(p, JOG_TOK_LBRACE)) {
            body = parse_block(p);
        } else {
            body = parse_postfix(p, true);
        }
    }
    JogNode *node = make_at(p, JOG_NODE_GO, pos);
    if (!node) {
        return NULL;
    }
    node->as.go.args = args;
    node->as.go.contexts = contexts;
    node->as.go.body = body;
    return node;
}

static JogNode *parse_expr_trail(Parser *p, bool trail) {
    if (give_up(p)) {
        return NULL;
    }
    /* if, when, and go sit outside the operator grammar. quote is post-V1. */
    if (check(p, JOG_TOK_KW_IF)) {
        return parse_if(p);
    }
    if (check(p, JOG_TOK_KW_WHEN)) {
        return parse_when(p);
    }
    if (check(p, JOG_TOK_KW_GO)) {
        return parse_go(p);
    }
    if (check(p, JOG_TOK_KW_QUOTE)) {
        reject_later_expr(p);
        return NULL;
    }
    return parse_prec(p, 0, true, trail);
}

static JogNode *parse_expr(Parser *p) {
    return parse_expr_trail(p, true);
}

/* Index and slice bounds may contain '|', '+', and the rest, but '..' splits the slice. */
static JogNode *parse_slice_bound(Parser *p, bool trail) {
    return parse_prec(p, 0, false, trail);
}

/* ---- statements that declarations need ---- */

static JogNode *parse_binding(Parser *p, JogNodeKind kind, JogList attrs, JogVisibility vis);

static JogNode *parse_stmt(Parser *p);

static bool later_statement(JogTokenKind kind) {
    switch (kind) {
    case JOG_TOK_KW_ELSE:
    case JOG_TOK_KW_FN:
    case JOG_TOK_KW_TYPE:
    case JOG_TOK_KW_STRUCT:
    case JOG_TOK_KW_INTERFACE:
    case JOG_TOK_KW_ENUM:
    case JOG_TOK_KW_EXTERN:
    case JOG_TOK_KW_IMPORT:
    case JOG_TOK_KW_QUOTE:
        return true;
    default:
        return false;
    }
}

static JogNode *parse_return(Parser *p) {
    JogPos pos = here(p);
    advance(p);
    CtDa values;
    if (!start_da(p, &values, sizeof(JogNode *))) {
        return NULL;
    }
    if (!check(p, JOG_TOK_SEP) && !check(p, JOG_TOK_RBRACE) && !is_eof(p) && !give_up(p)) {
        while (!give_up(p) && !is_eof(p) && !check(p, JOG_TOK_SEP) && !check(p, JOG_TOK_RBRACE)) {
            push_node(p, &values, parse_expr(p));
            if (!eat(p, JOG_TOK_COMMA)) {
                break;
            }
        }
    }
    JogNode *node = make_at(p, JOG_NODE_RETURN, pos);
    if (!node) {
        ct_da_free(&values);
        return NULL;
    }
    node->as.ret.values = take_nodes(p, &values);
    return node;
}

static JogNode *parse_expr_stmt(Parser *p) {
    JogNode *expr = parse_expr(p);
    if (!expr) {
        return NULL;
    }
    if (!give_up(p) && is_assign_op(peek_kind(p))) {
        JogTokenKind op = peek_kind(p);
        JogPos pos = here(p);
        advance(p);
        JogNode *value = parse_expr(p);
        if (expr->kind == JOG_NODE_TUPLE && op == JOG_TOK_ASSIGN) {
            JogNode *node = make_at(p, JOG_NODE_DESTRUCTURE, pos);
            if (!node) {
                return NULL;
            }
            node->as.destructure.targets = expr->as.tuple.elems;
            node->as.destructure.value = value;
            return node;
        }
        JogNode *node = make_at(p, JOG_NODE_ASSIGN, pos);
        if (!node) {
            return NULL;
        }
        node->as.assign.op = op;
        node->as.assign.target = expr;
        node->as.assign.value = value;
        return node;
    }
    JogNode *stmt = make_at(p, JOG_NODE_EXPR_STMT, expr->pos);
    if (!stmt) {
        return NULL;
    }
    stmt->as.expr_stmt.expr = expr;
    return stmt;
}

static bool at_for_in(const Parser *p, bool *two) {
    if (!check(p, JOG_TOK_IDENT)) {
        return false;
    }
    if (peek_n(p, 1) == JOG_TOK_KW_IN) {
        *two = false;
        return true;
    }
    if (peek_n(p, 1) == JOG_TOK_COMMA && peek_n(p, 2) == JOG_TOK_IDENT && peek_n(p, 3) == JOG_TOK_KW_IN) {
        *two = true;
        return true;
    }
    return false;
}

static JogNode *parse_for(Parser *p) {
    JogPos pos = here(p);
    advance(p);
    JogNode *node = make_at(p, JOG_NODE_FOR, pos);
    if (!node) {
        return NULL;
    }
    if (check(p, JOG_TOK_LBRACE)) {
        node->as.for_.form = JOG_FOR_FOREVER;
        node->as.for_.body = parse_block(p);
        return node;
    }
    bool two = false;
    if (at_for_in(p, &two)) {
        node->as.for_.form = JOG_FOR_IN;
        node->as.for_.first = ident_text(p);
        advance(p);
        if (two) {
            advance(p); /* , */
            node->as.for_.second = ident_text(p);
            advance(p);
        }
        if (!give_up(p)) {
            expect(p, JOG_TOK_KW_IN, "'in'");
        }
        if (!give_up(p)) {
            node->as.for_.expr = parse_expr_trail(p, false);
        }
    } else {
        node->as.for_.form = JOG_FOR_COND;
        node->as.for_.expr = parse_expr_trail(p, false);
    }
    if (!give_up(p)) {
        node->as.for_.body = parse_block(p);
    }
    return node;
}

static JogNode *parse_defer_stmt(Parser *p, JogNodeKind kind) {
    JogPos pos = here(p);
    advance(p);
    JogNode *body = NULL;
    if (!give_up(p)) {
        if (check(p, JOG_TOK_LBRACE)) {
            body = parse_block(p);
        } else {
            body = parse_stmt(p);
        }
    }
    JogNode *node = make_at(p, kind, pos);
    if (!node) {
        return NULL;
    }
    node->as.defer.body = body;
    return node;
}

static JogNode *parse_with_stmt(Parser *p) {
    JogPos pos = here(p);
    advance(p);
    JogNode *node = make_at(p, JOG_NODE_WITH, pos);
    if (!node) {
        return NULL;
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_LPAREN, "'('");
    }
    if (!give_up(p)) {
        /* Rule 1: no trailing lambda at the top level of a with head. */
        node->as.with.values = parse_expr_list(p, false);
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    if (!give_up(p) && node->as.with.values.count == 0) {
        complain(p, "expected an expression");
    }
    if (!give_up(p)) {
        node->as.with.body = parse_block(p);
    }
    return node;
}

/* @attr { ... } is a statement. @name(...) with no following block is a builtin expression. */
static bool starts_attr_block(const Parser *p) {
    size_t n = 0;
    for (;;) {
        if (peek_n(p, n) != JOG_TOK_ATTR) {
            return false;
        }
        n++;
        if (peek_n(p, n) == JOG_TOK_LPAREN) {
            int depth = 0;
            for (;;) {
                JogTokenKind k = peek_n(p, n);
                if (k == JOG_TOK_EOF) {
                    return false;
                }
                if (k == JOG_TOK_LPAREN) {
                    depth++;
                } else if (k == JOG_TOK_RPAREN) {
                    depth--;
                }
                n++;
                if (depth == 0) {
                    break;
                }
            }
        }
        while (peek_n(p, n) == JOG_TOK_SEP) {
            n++;
        }
        if (peek_n(p, n) == JOG_TOK_LBRACE) {
            return true;
        }
        if (peek_n(p, n) != JOG_TOK_ATTR) {
            return false;
        }
    }
}

static JogNode *parse_attr_block(Parser *p) {
    JogPos pos = here(p);
    JogList attrs = parse_attributes(p);
    JogNode *body = NULL;
    if (!give_up(p)) {
        body = parse_block(p);
    }
    JogNode *node = make_at(p, JOG_NODE_ATTR_BLOCK, pos);
    if (!node) {
        return NULL;
    }
    node->attrs = attrs;
    node->as.attr_block.body = body;
    return node;
}

static JogNode *parse_stmt(Parser *p) {
    if (give_up(p)) {
        return NULL;
    }
    JogTokenKind kind = peek_kind(p);
    if (kind == JOG_TOK_KW_VAR) {
        return parse_binding(p, JOG_NODE_VAR, empty_list(), JOG_VIS_PUBLIC);
    }
    if (kind == JOG_TOK_KW_VAL) {
        return parse_binding(p, JOG_NODE_VAL, empty_list(), JOG_VIS_PUBLIC);
    }
    if (kind == JOG_TOK_KW_CONTEXT) {
        return parse_binding(p, JOG_NODE_CONTEXT, empty_list(), JOG_VIS_PUBLIC);
    }
    if (kind == JOG_TOK_KW_RETURN) {
        return parse_return(p);
    }
    if (kind == JOG_TOK_KW_BREAK || kind == JOG_TOK_KW_CONTINUE) {
        JogPos pos = here(p);
        advance(p);
        return make_at(p, kind == JOG_TOK_KW_BREAK ? JOG_NODE_BREAK : JOG_NODE_CONTINUE, pos);
    }
    if (kind == JOG_TOK_KW_FOR) {
        return parse_for(p);
    }
    if (kind == JOG_TOK_KW_DEFER || kind == JOG_TOK_KW_ERRDEFER) {
        JogNodeKind node_kind = kind == JOG_TOK_KW_DEFER ? JOG_NODE_DEFER : JOG_NODE_ERRDEFER;
        return parse_defer_stmt(p, node_kind);
    }
    if (kind == JOG_TOK_KW_WITH) {
        return parse_with_stmt(p);
    }
    if (kind == JOG_TOK_ATTR && starts_attr_block(p)) {
        return parse_attr_block(p);
    }
    if (later_statement(kind)) {
        error_here(p, "expected a statement, found %s", jog_token_spelling(kind));
        return NULL;
    }
    return parse_expr_stmt(p);
}

/* Caller has already consumed '{'. Consumes the matching '}'. */
static JogList parse_until_rbrace(Parser *p) {
    CtDa stmts;
    if (!start_da(p, &stmts, sizeof(JogNode *))) {
        return empty_list();
    }
    skip_seps(p);
    while (!p->oom && !is_eof(p) && !check(p, JOG_TOK_RBRACE)) {
        size_t at = p->at;
        bool pending = p->pending_gt;
        JogNode *stmt = parse_stmt(p);
        if (p->panic) {
            recover(p);
        }
        push_node(p, &stmts, stmt);
        if (same_place(p, at, pending)) {
            if (!p->panic) {
                error_here(p, "expected a statement, found %s", jog_token_spelling(peek_kind(p)));
            }
            recover(p);
            if (same_place(p, at, pending)) {
                advance(p);
            }
        }
        skip_seps(p);
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_RBRACE, "'}'");
    } else if (check(p, JOG_TOK_RBRACE)) {
        advance(p);
        p->panic = false;
    }
    return take_nodes(p, &stmts);
}

static JogNode *parse_block(Parser *p) {
    JogPos pos = here(p);
    expect(p, JOG_TOK_LBRACE, "'{'");
    JogList stmts = parse_until_rbrace(p);
    JogNode *node = make_at(p, JOG_NODE_BLOCK, pos);
    if (!node) {
        return NULL;
    }
    node->as.block.stmts = stmts;
    return node;
}

/* IDENT (',' IDENT)* '->' at the start of a trailing lambda. */
static bool at_lambda_params(const Parser *p) {
    if (!check(p, JOG_TOK_IDENT)) {
        return false;
    }
    size_t n = 1;
    while (peek_n(p, n) == JOG_TOK_COMMA && peek_n(p, n + 1) == JOG_TOK_IDENT) {
        n += 2;
    }
    return peek_n(p, n) == JOG_TOK_ARROW;
}

static JogList parse_lambda_params(Parser *p) {
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    for (;;) {
        JogPos pos = here(p);
        const char *name = ident_text(p);
        advance(p);
        JogNode *param = make_at(p, JOG_NODE_PARAM, pos);
        if (param) {
            param->as.param.name = name;
            push_node(p, &da, param);
        }
        if (!eat(p, JOG_TOK_COMMA)) {
            break;
        }
        if (!check(p, JOG_TOK_IDENT)) {
            error_here(p, "expected a parameter name, found %s", jog_token_spelling(peek_kind(p)));
            break;
        }
    }
    if (!give_up(p)) {
        expect(p, JOG_TOK_ARROW, "'->'");
    }
    return take_nodes(p, &da);
}

static JogNode *parse_trailing_lambda(Parser *p) {
    JogPos pos = here(p);
    expect(p, JOG_TOK_LBRACE, "'{'");
    bool implicit = true;
    JogList params = empty_list();
    if (!give_up(p) && at_lambda_params(p)) {
        implicit = false;
        params = parse_lambda_params(p);
    }
    JogList stmts = parse_until_rbrace(p);
    JogNode *body = make_at(p, JOG_NODE_BLOCK, pos);
    if (body) {
        body->as.block.stmts = stmts;
    }
    JogNode *node = make_at(p, JOG_NODE_LAMBDA, pos);
    if (!node) {
        return NULL;
    }
    node->as.lambda.params = params;
    node->as.lambda.body = body;
    node->as.lambda.implicit_it = implicit;
    return node;
}

/* ---- declarations ---- */

static JogNode *parse_attribute(Parser *p) {
    const JogToken *t = cur(p);
    JogPos pos = here(p);
    const char *name = t && t->value ? t->value : "";
    advance(p);
    JogNode *node = make_at(p, JOG_NODE_ATTRIBUTE, pos);
    if (!node) {
        return NULL;
    }
    node->as.attribute.name = name;
    if (eat(p, JOG_TOK_LPAREN)) {
        node->as.attribute.has_parens = true;
        if (!check(p, JOG_TOK_RPAREN)) {
            node->as.attribute.args = parse_args(p);
        }
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    return node;
}

static JogList parse_attributes(Parser *p) {
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    while (check(p, JOG_TOK_ATTR)) {
        push_node(p, &da, parse_attribute(p));
        /* A newline after ')' is a SEP; attributes ignore it. */
        skip_seps(p);
    }
    return take_nodes(p, &da);
}

static JogVisibility parse_visibility(Parser *p) {
    if (!eat(p, JOG_TOK_KW_PRIVATE)) {
        return JOG_VIS_PUBLIC;
    }
    if (!eat(p, JOG_TOK_LPAREN)) {
        skip_seps(p);
        return JOG_VIS_FILE;
    }
    JogVisibility vis = JOG_VIS_PACKAGE;
    if (!check(p, JOG_TOK_IDENT)) {
        error_here(p, "expected 'file', 'package', or 'scope'");
    } else {
        const char *level = ident_text(p);
        advance(p);
        if (strcmp(level, "file") == 0) {
            vis = JOG_VIS_FILE;
        } else if (strcmp(level, "package") == 0) {
            vis = JOG_VIS_PACKAGE;
        } else if (strcmp(level, "scope") == 0) {
            vis = JOG_VIS_SCOPE;
        } else {
            complain(p, "expected 'file', 'package', or 'scope'");
        }
    }
    expect(p, JOG_TOK_RPAREN, "')'");
    skip_seps(p);
    return vis;
}

static JogNames parse_bind_names(Parser *p, bool *destructure) {
    *destructure = false;
    CtDa names;
    if (!start_da(p, &names, sizeof(const char *))) {
        return empty_names();
    }
    if (eat(p, JOG_TOK_LPAREN)) {
        *destructure = true;
        while (!give_up(p) && !is_eof(p) && !check(p, JOG_TOK_RPAREN)) {
            /* `val (in, e1)` uses the keyword as a binding name. */
            int keyword = is_keyword(peek_kind(p));
            if (!check(p, JOG_TOK_IDENT) && !keyword) {
                error_here(p, "expected a name, found %s", jog_token_spelling(peek_kind(p)));
                break;
            }
            push_name(p, &names, keyword ? keyword_name(p) : ident_text(p));
            advance(p);
            if (!eat(p, JOG_TOK_COMMA)) {
                break;
            }
        }
        expect(p, JOG_TOK_RPAREN, "')'");
    } else if (check(p, JOG_TOK_IDENT)) {
        push_name(p, &names, ident_text(p));
        advance(p);
    } else {
        error_here(p, "expected a name, found %s", jog_token_spelling(peek_kind(p)));
    }
    return take_names(p, &names);
}

static JogNode *parse_binding(Parser *p, JogNodeKind kind, JogList attrs, JogVisibility vis) {
    JogPos pos = here(p);
    advance(p);
    JogNode *node = make_at(p, kind, pos);
    if (!node) {
        return NULL;
    }
    node->attrs = attrs;
    node->visibility = vis;
    bool destructure = false;
    if (kind == JOG_NODE_CONTEXT && check(p, JOG_TOK_LPAREN)) {
        error_here(p, "context binds one name");
    }
    if (kind == JOG_NODE_CONTEXT && check(p, JOG_TOK_IDENT)) {
        CtDa names;
        if (start_da(p, &names, sizeof(const char *))) {
            push_name(p, &names, ident_text(p));
            advance(p);
            node->as.binding.names = take_names(p, &names);
        }
    } else if (!give_up(p)) {
        node->as.binding.names = parse_bind_names(p, &destructure);
        node->as.binding.destructure = destructure;
    }
    if (!give_up(p) && eat(p, JOG_TOK_COLON)) {
        node->as.binding.type = parse_type(p);
    }
    if (!give_up(p) && eat(p, JOG_TOK_ASSIGN)) {
        node->as.binding.init = parse_expr(p);
    } else if (!give_up(p) && kind != JOG_NODE_VAR) {
        error_here(p, "expected '='");
    }
    return node;
}

static JogNode *parse_import(Parser *p, JogList attrs, JogVisibility vis) {
    JogPos pos = here(p);
    advance(p);
    CtDa names;
    if (!start_da(p, &names, sizeof(const char *))) {
        return NULL;
    }
    parse_dot_names(p, &names);
    const char *alias = NULL;
    if (!give_up(p) && eat(p, JOG_TOK_KW_AS)) {
        if (check(p, JOG_TOK_IDENT)) {
            alias = ident_text(p);
            advance(p);
        } else {
            error_here(p, "expected a name after 'as'");
        }
    }
    if (p->saw_non_import) {
        complain(p, "import must come before other declarations");
    }
    JogNode *node = make_at(p, JOG_NODE_IMPORT, pos);
    if (!node) {
        ct_da_free(&names);
        return NULL;
    }
    node->attrs = attrs;
    node->visibility = vis;
    node->as.import.path = take_names(p, &names);
    node->as.import.alias = alias;
    return node;
}

static bool is_overload_op(JogTokenKind kind) {
    switch (kind) {
    case JOG_TOK_PLUS:
    case JOG_TOK_MINUS:
    case JOG_TOK_STAR:
    case JOG_TOK_SLASH:
    case JOG_TOK_PERCENT:
    case JOG_TOK_AMP:
    case JOG_TOK_PIPE:
    case JOG_TOK_CARET:
    case JOG_TOK_SHL:
    case JOG_TOK_SHR:
    case JOG_TOK_TILDE:
    case JOG_TOK_BANG:
    case JOG_TOK_EQ:
    case JOG_TOK_NEQ:
    case JOG_TOK_LT:
    case JOG_TOK_LE:
    case JOG_TOK_GT:
    case JOG_TOK_GE:
    case JOG_TOK_CMP3:
    case JOG_TOK_PLUS_ASSIGN:
    case JOG_TOK_MINUS_ASSIGN:
    case JOG_TOK_STAR_ASSIGN:
    case JOG_TOK_SLASH_ASSIGN:
        return true;
    default:
        return false;
    }
}

static const char *intern_op_name(Parser *p, const char *spelling) {
    char buf[16];
    snprintf(buf, sizeof buf, "op%s", spelling);
    const char *name = ct_intern_cstr(p->intern, buf);
    if (!name) {
        p->oom = true;
        return "";
    }
    return name;
}

/* True when the next tokens are `op` and then an operator, as in `fn T.op+`. */
static bool at_op_method(const Parser *p) {
    const JogToken *t;
    JogTokenKind next;
    if (!check(p, JOG_TOK_IDENT)) {
        return false;
    }
    t = cur(p);
    if (!t || t->length != 2 || !t->start || t->start[0] != 'o' || t->start[1] != 'p') {
        return false;
    }
    next = peek_n(p, 1);
    return is_overload_op(next) || next == JOG_TOK_LBRACKET;
}

static bool parse_op_body(Parser *p, const char **name, JogTokenKind *op) {
    if (check(p, JOG_TOK_LBRACKET)) {
        advance(p);
        expect(p, JOG_TOK_RBRACKET, "']'");
        if (eat(p, JOG_TOK_ASSIGN)) {
            *name = intern_op_name(p, "[]=");
        } else {
            *name = intern_op_name(p, "[]");
        }
        /* '[' stands for both '[]' and '[]='; the name distinguishes them. */
        *op = JOG_TOK_LBRACKET;
        return !p->oom;
    }
    JogTokenKind kind = peek_kind(p);
    if (!is_overload_op(kind)) {
        error_here(p, "expected an operator, found %s", jog_token_spelling(kind));
        return false;
    }
    *op = kind;
    *name = intern_op_name(p, jog_token_spelling(kind));
    advance(p);
    return !p->oom;
}

static bool parse_fn_name(Parser *p, const char **name, JogTokenKind *op) {
    *name = "";
    *op = JOG_TOK_EOF;
    /* `op` is an identifier except at the start of an operator method. */
    if (at_op_method(p)) {
        advance(p);
        return parse_op_body(p, name, op);
    }
    if (eat(p, JOG_TOK_KW_OP)) {
        return parse_op_body(p, name, op);
    }
    if (check(p, JOG_TOK_IDENT)) {
        *name = ident_text(p);
        advance(p);
        return true;
    }
    error_here(p, "expected a function name, found %s", jog_token_spelling(peek_kind(p)));
    return false;
}

static bool angles_then_dot(const Parser *p) {
    if (p->pending_gt || peek_n(p, 1) != JOG_TOK_LT) {
        return false;
    }
    int depth = 0;
    for (size_t i = p->at + 1; i < p->count; i++) {
        JogTokenKind kind = p->toks[i].kind;
        if (kind == JOG_TOK_LT) {
            depth++;
        } else if (kind == JOG_TOK_GT) {
            depth--;
            if (depth == 0) {
                return i + 1 < p->count && p->toks[i + 1].kind == JOG_TOK_DOT;
            }
        } else if (kind == JOG_TOK_SHR) {
            depth -= 2;
            if (depth == 0) {
                return i + 1 < p->count && p->toks[i + 1].kind == JOG_TOK_DOT;
            }
            if (depth < 0) {
                return false;
            }
        } else if (kind == JOG_TOK_EOF) {
            return false;
        }
    }
    return false;
}

static bool paren_then_dot(const Parser *p) {
    if (p->pending_gt || peek_kind(p) != JOG_TOK_LPAREN) {
        return false;
    }
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    for (size_t i = p->at; i < p->count; i++) {
        JogTokenKind kind = p->toks[i].kind;
        if (kind == JOG_TOK_LPAREN) {
            paren++;
        } else if (kind == JOG_TOK_RPAREN) {
            paren--;
            if (paren == 0 && bracket == 0 && brace == 0) {
                return i + 1 < p->count && p->toks[i + 1].kind == JOG_TOK_DOT;
            }
            if (paren < 0) {
                return false;
            }
        } else if (kind == JOG_TOK_LBRACKET) {
            bracket++;
        } else if (kind == JOG_TOK_RBRACKET) {
            if (bracket > 0) {
                bracket--;
            }
        } else if (kind == JOG_TOK_LBRACE) {
            brace++;
        } else if (kind == JOG_TOK_RBRACE) {
            if (brace > 0) {
                brace--;
            }
        } else if (kind == JOG_TOK_EOF) {
            return false;
        }
    }
    return false;
}

static JogList parse_context_clause(Parser *p) {
    advance(p); /* with */
    expect(p, JOG_TOK_LPAREN, "'('");
    bool variadic = false;
    JogList params = parse_param_list(p, false, false, "a context parameter cannot be a pack", &variadic);
    expect(p, JOG_TOK_RPAREN, "')'");
    return params;
}

static bool starts_method(const Parser *p) {
    if (check(p, JOG_TOK_STAR)) {
        return true;
    }
    if (paren_then_dot(p)) {
        return true;
    }
    if (!check(p, JOG_TOK_IDENT)) {
        return false;
    }
    return peek_n(p, 1) == JOG_TOK_DOT || angles_then_dot(p);
}

static JogNode *parse_fn(Parser *p, JogList attrs, JogVisibility vis, FnMode mode) {
    JogPos pos = here(p);
    advance(p); /* fn */
    JogNode *node = make_at(p, JOG_NODE_FN, pos);
    if (!node) {
        return NULL;
    }
    node->attrs = attrs;
    node->visibility = vis;
    node->as.fn.is_extern = mode == FN_EXTERN;
    node->as.fn.is_expect = mode == FN_EXPECT;
    node->as.fn.is_actual = mode == FN_ACTUAL;
    node->as.fn.op = JOG_TOK_EOF;

    if (mode == FN_INTERFACE) {
        if (eat(p, JOG_TOK_STAR)) {
            node->as.fn.receiver_is_ptr = true;
        }
        parse_fn_name(p, &node->as.fn.name, &node->as.fn.op);
    } else if (mode == FN_EXTERN) {
        if (check(p, JOG_TOK_IDENT)) {
            node->as.fn.name = ident_text(p);
            advance(p);
        } else {
            error_here(p, "expected a function name, found %s", jog_token_spelling(peek_kind(p)));
        }
    } else if (starts_method(p)) {
        if (eat(p, JOG_TOK_STAR)) {
            node->as.fn.receiver_is_ptr = true;
        }
        if (check(p, JOG_TOK_LPAREN)) {
            advance(p);
            node->as.fn.receiver = parse_type(p);
            expect(p, JOG_TOK_RPAREN, "')'");
        } else {
            node->as.fn.receiver = parse_receiver_name(p);
        }
        expect(p, JOG_TOK_DOT, "'.'");
        if (!give_up(p)) {
            parse_fn_name(p, &node->as.fn.name, &node->as.fn.op);
        }
    } else {
        parse_fn_name(p, &node->as.fn.name, &node->as.fn.op);
    }

    if (!give_up(p) && check(p, JOG_TOK_LT)) {
        if (mode == FN_INTERFACE) {
            complain(p, "an interface method cannot have type parameters");
        } else if (mode == FN_EXTERN) {
            complain(p, "an extern function cannot have type parameters");
        }
        node->as.fn.type_params = parse_type_params(p);
    }

    bool variadic = false;
    if (!give_up(p)) {
        expect(p, JOG_TOK_LPAREN, "'('");
    }
    if (!give_up(p)) {
        bool defaults = mode == FN_REGULAR || mode == FN_INTERFACE || mode == FN_EXPECT || mode == FN_ACTUAL;
        const char *pack_reject = NULL;
        if (mode == FN_EXTERN) {
            pack_reject = "an extern function cannot have a pack parameter";
        } else if (mode == FN_INTERFACE) {
            pack_reject = "an interface method cannot have a pack parameter";
        }
        node->as.fn.params = parse_param_list(p, mode == FN_EXTERN, defaults, pack_reject, &variadic);
        node->as.fn.variadic = variadic;
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    if (!give_up(p) && eat(p, JOG_TOK_COLON)) {
        node->as.fn.ret = parse_type(p);
    }
    if (!give_up(p) && with_is_param_clause(p)) {
        if (mode == FN_EXTERN) {
            complain(p, "an extern function cannot declare contexts");
        }
        node->as.fn.contexts = parse_context_clause(p);
    }
    if (give_up(p)) {
        return node;
    }
    if (mode == FN_REGULAR || mode == FN_ACTUAL) {
        if (eat(p, JOG_TOK_ASSIGN)) {
            node->as.fn.expr_body = true;
            node->as.fn.body = parse_expr(p);
        } else if (check(p, JOG_TOK_LBRACE)) {
            node->as.fn.body = parse_block(p);
        } else {
            error_here(p, "expected a function body");
        }
    } else if (check(p, JOG_TOK_ASSIGN) || check(p, JOG_TOK_LBRACE)) {
        if (mode == FN_EXPECT) {
            error_here(p, "an expect function cannot have a body");
        } else {
            error_here(p, "this function cannot have a body");
        }
    }
    return node;
}

static JogNode *parse_type_decl(Parser *p, JogList attrs, JogVisibility vis) {
    JogPos pos = here(p);
    advance(p);
    JogNode *node = make_at(p, JOG_NODE_TYPE_DECL, pos);
    if (!node) {
        return NULL;
    }
    node->attrs = attrs;
    node->visibility = vis;
    if (check(p, JOG_TOK_IDENT)) {
        node->as.type_decl.name = ident_text(p);
        advance(p);
    } else {
        error_here(p, "expected a name, found %s", jog_token_spelling(peek_kind(p)));
    }
    if (!give_up(p) && check(p, JOG_TOK_LT)) {
        node->as.type_decl.type_params = parse_type_params(p);
    }
    if (give_up(p)) {
        return node;
    }
    if (eat(p, JOG_TOK_ASSIGN)) {
        node->as.type_decl.distinct = false;
    } else if (eat(p, JOG_TOK_COLON)) {
        node->as.type_decl.distinct = true;
    } else {
        error_here(p, "expected '=' or ':'");
        return node;
    }
    node->as.type_decl.target = parse_type(p);
    return node;
}

static JogNode *parse_field(Parser *p, JogList attrs, JogVisibility vis, bool is_var, bool allow_default) {
    JogPos pos = here(p);
    const char *name = ident_text(p);
    advance(p);
    expect(p, JOG_TOK_COLON, "':'");
    JogNode *type = NULL;
    if (!give_up(p)) {
        type = parse_type(p);
    }
    JogNode *fallback = NULL;
    if (!give_up(p) && eat(p, JOG_TOK_ASSIGN)) {
        if (!allow_default) {
            complain(p, "this member cannot have a default value");
        }
        fallback = parse_expr(p);
    }
    JogNode *node = make_at(p, JOG_NODE_FIELD, pos);
    if (!node) {
        return NULL;
    }
    node->attrs = attrs;
    node->visibility = vis;
    node->as.field.name = name;
    node->as.field.type = type;
    node->as.field.default_value = fallback;
    node->as.field.is_var = is_var;
    return node;
}

static JogNode *parse_struct_member(Parser *p) {
    JogList attrs = parse_attributes(p);
    bool saw_private = check(p, JOG_TOK_KW_PRIVATE);
    JogVisibility vis = parse_visibility(p);
    if (check(p, JOG_TOK_IDENT) && peek_n(p, 1) == JOG_TOK_COLON) {
        return parse_field(p, attrs, vis, false, true);
    }
    if (attrs.count == 0 && !saw_private && check(p, JOG_TOK_IDENT)) {
        JogNode *type = parse_type_name(p);
        JogNode *embed = make_at(p, JOG_NODE_EMBED, type ? type->pos : here(p));
        if (!embed) {
            return NULL;
        }
        embed->as.embed.type = type;
        return embed;
    }
    error_here(p, "expected a field or an embedded type, found %s", jog_token_spelling(peek_kind(p)));
    return NULL;
}

static bool *copy_flags(Parser *p, CtDa *da) {
    if (!da || da->count == 0) {
        return NULL;
    }
    bool *flags = ct_arena_alloc(p->arena, da->count * sizeof(bool), _Alignof(bool));
    if (!flags) {
        p->oom = true;
        return NULL;
    }
    memcpy(flags, da->data, da->count * sizeof(bool));
    return flags;
}

static JogNode *parse_type_set(Parser *p, JogPos pos, JogNode *first, bool first_approx) {
    CtDa terms;
    CtDa flags;
    if (!start_da(p, &terms, sizeof(JogNode *))) {
        return NULL;
    }
    if (!start_da(p, &flags, sizeof(bool))) {
        ct_da_free(&terms);
        return NULL;
    }
    push_node(p, &terms, first);
    if (!ct_da_push(&flags, &first_approx)) {
        p->oom = true;
    }
    while (!give_up(p)) {
        if (check(p, JOG_TOK_SEP) && peek_n(p, 1) == JOG_TOK_PIPE) {
            skip_seps(p);
        }
        if (!eat(p, JOG_TOK_PIPE)) {
            break;
        }
        skip_seps(p);
        bool approx = eat(p, JOG_TOK_TILDE);
        JogNode *type = parse_type(p);
        push_node(p, &terms, type);
        if (!ct_da_push(&flags, &approx)) {
            p->oom = true;
        }
    }
    JogNode *node = make_at(p, JOG_NODE_TYPE_SET, pos);
    if (!node) {
        ct_da_free(&terms);
        ct_da_free(&flags);
        return NULL;
    }
    node->as.type_set.approximate = copy_flags(p, &flags);
    ct_da_free(&flags);
    node->as.type_set.terms = take_nodes(p, &terms);
    return node;
}

static JogNode *parse_iface_member(Parser *p) {
    if (check(p, JOG_TOK_KW_VAL) || check(p, JOG_TOK_KW_VAR)) {
        bool is_var = check(p, JOG_TOK_KW_VAR);
        advance(p);
        if (!check(p, JOG_TOK_IDENT)) {
            error_here(p, "expected a property name, found %s", jog_token_spelling(peek_kind(p)));
            return NULL;
        }
        return parse_field(p, empty_list(), JOG_VIS_PUBLIC, is_var, false);
    }
    if (check(p, JOG_TOK_KW_FN)) {
        return parse_fn(p, empty_list(), JOG_VIS_PUBLIC, FN_INTERFACE);
    }
    JogPos pos = here(p);
    bool approx = eat(p, JOG_TOK_TILDE);
    if (!type_can_start(peek_kind(p))) {
        error_here(p, "expected an interface member, found %s", jog_token_spelling(peek_kind(p)));
        return NULL;
    }
    JogNode *type = parse_type(p);
    if (approx || check(p, JOG_TOK_PIPE) || (check(p, JOG_TOK_SEP) && peek_n(p, 1) == JOG_TOK_PIPE)) {
        return parse_type_set(p, pos, type, approx);
    }
    JogNode *embed = make_at(p, JOG_NODE_EMBED, pos);
    if (!embed) {
        return NULL;
    }
    embed->as.embed.type = type;
    return embed;
}

static JogNode *parse_variant(Parser *p) {
    if (!check(p, JOG_TOK_IDENT)) {
        error_here(p, "expected a variant name, found %s", jog_token_spelling(peek_kind(p)));
        return NULL;
    }
    JogPos pos = here(p);
    const char *name = ident_text(p);
    advance(p);
    JogList fields = empty_list();
    bool payload = false;
    if (!give_up(p) && eat(p, JOG_TOK_LPAREN)) {
        payload = true;
        bool variadic = false;
        fields = parse_param_list(p, false, true, "a pack parameter is only allowed on a function", &variadic);
        expect(p, JOG_TOK_RPAREN, "')'");
    }
    JogNode *value = NULL;
    bool has_value = false;
    if (!give_up(p) && eat(p, JOG_TOK_ASSIGN)) {
        has_value = true;
        value = parse_expr(p);
    }
    if (payload && has_value) {
        complain(p, "a variant cannot have both a payload and a tag value");
    }
    JogNode *node = make_at(p, JOG_NODE_VARIANT, pos);
    if (!node) {
        return NULL;
    }
    node->as.variant.name = name;
    node->as.variant.fields = fields;
    node->as.variant.value = value;
    return node;
}

static JogNode *parse_extern_fn(Parser *p) {
    JogList attrs = parse_attributes(p);
    if (check(p, JOG_TOK_KW_PRIVATE)) {
        complain(p, "extern functions do not take a visibility modifier");
        parse_visibility(p);
    }
    if (!check(p, JOG_TOK_KW_FN)) {
        error_here(p, "expected a function, found %s", jog_token_spelling(peek_kind(p)));
        return NULL;
    }
    return parse_fn(p, attrs, JOG_VIS_PUBLIC, FN_EXTERN);
}

static JogList parse_members(Parser *p, JogNode *(*parse_one)(Parser *p)) {
    CtDa da;
    if (!start_da(p, &da, sizeof(JogNode *))) {
        return empty_list();
    }
    skip_seps(p);
    while (!p->oom && !is_eof(p) && !check(p, JOG_TOK_RBRACE)) {
        size_t at = p->at;
        bool pending = p->pending_gt;
        JogNode *member = parse_one(p);
        bool recovered = false;
        if (p->panic) {
            recover(p);
            recovered = true;
        }
        push_node(p, &da, member);
        if (same_place(p, at, pending)) {
            if (!recovered) {
                error_here(p, "expected a member, found %s", jog_token_spelling(peek_kind(p)));
            }
            recover(p);
            if (same_place(p, at, pending)) {
                advance(p);
            }
            continue;
        }
        bool separated = recovered;
        if (eat(p, JOG_TOK_COMMA)) {
            separated = true;
        }
        while (eat(p, JOG_TOK_SEP)) {
            separated = true;
        }
        if (check(p, JOG_TOK_RBRACE) || is_eof(p)) {
            break;
        }
        if (!separated) {
            error_here(p, "expected ',' or a newline");
            recover(p);
        }
    }
    return take_nodes(p, &da);
}

static JogNode *parse_aggregate(Parser *p, JogNodeKind kind, JogList attrs, JogVisibility vis) {
    JogPos pos = here(p);
    advance(p);
    JogNode *node = make_at(p, kind, pos);
    if (!node) {
        return NULL;
    }
    node->attrs = attrs;
    node->visibility = vis;
    if (check(p, JOG_TOK_IDENT)) {
        node->as.aggregate.name = ident_text(p);
        advance(p);
    } else {
        error_here(p, "expected a name, found %s", jog_token_spelling(peek_kind(p)));
    }
    if (!give_up(p) && check(p, JOG_TOK_LT)) {
        node->as.aggregate.type_params = parse_type_params(p);
    }
    if (!give_up(p) && kind == JOG_NODE_ENUM && eat(p, JOG_TOK_COLON)) {
        node->as.aggregate.tag_type = parse_type(p);
    }
    if (give_up(p)) {
        return node;
    }
    if (!eat(p, JOG_TOK_LBRACE)) {
        return node;
    }
    if (kind == JOG_NODE_STRUCT) {
        node->as.aggregate.members = parse_members(p, parse_struct_member);
    } else if (kind == JOG_NODE_INTERFACE) {
        node->as.aggregate.members = parse_members(p, parse_iface_member);
    } else {
        node->as.aggregate.members = parse_members(p, parse_variant);
    }
    expect(p, JOG_TOK_RBRACE, "'}'");
    return node;
}

static JogNode *parse_extern_block(Parser *p, JogList attrs, JogVisibility vis) {
    JogPos pos = here(p);
    advance(p);
    JogNode *node = make_at(p, JOG_NODE_EXTERN_BLOCK, pos);
    if (!node) {
        return NULL;
    }
    node->attrs = attrs;
    node->visibility = vis;
    if (expect(p, JOG_TOK_LBRACE, "'{'")) {
        node->as.extern_block.fns = parse_members(p, parse_extern_fn);
        expect(p, JOG_TOK_RBRACE, "'}'");
    }
    return node;
}

static bool ident_word(const Parser *p, const char *word) {
    const JogToken *t;
    if (!check(p, JOG_TOK_IDENT) || !word) {
        return false;
    }
    t = cur(p);
    return t && t->value && strcmp(t->value, word) == 0;
}

static JogNode *parse_top(Parser *p) {
    JogList attrs = parse_attributes(p);
    bool saw_private = check(p, JOG_TOK_KW_PRIVATE);
    JogVisibility vis = parse_visibility(p);
    /* expect and actual are identifiers except immediately before fn. */
    if ((ident_word(p, "expect") || ident_word(p, "actual")) && peek_n(p, 1) == JOG_TOK_KW_FN) {
        int actual = ident_word(p, "actual");
        JogPos kw = here(p);
        JogNode *node;
        if (actual && saw_private) {
            complain(p, "an actual function cannot have a visibility modifier");
            vis = JOG_VIS_PUBLIC;
        }
        advance(p);
        node = parse_fn(p, attrs, vis, actual ? FN_ACTUAL : FN_EXPECT);
        if (node) {
            node->pos = kw;
        }
        return node;
    }
    switch (peek_kind(p)) {
    case JOG_TOK_KW_IMPORT:
        return parse_import(p, attrs, vis);
    case JOG_TOK_KW_FN:
        return parse_fn(p, attrs, vis, FN_REGULAR);
    case JOG_TOK_KW_VAR:
        return parse_binding(p, JOG_NODE_VAR, attrs, vis);
    case JOG_TOK_KW_VAL:
        return parse_binding(p, JOG_NODE_VAL, attrs, vis);
    case JOG_TOK_KW_CONTEXT:
        return parse_binding(p, JOG_NODE_CONTEXT, attrs, vis);
    case JOG_TOK_KW_TYPE:
        return parse_type_decl(p, attrs, vis);
    case JOG_TOK_KW_STRUCT:
        return parse_aggregate(p, JOG_NODE_STRUCT, attrs, vis);
    case JOG_TOK_KW_INTERFACE:
        return parse_aggregate(p, JOG_NODE_INTERFACE, attrs, vis);
    case JOG_TOK_KW_ENUM:
        return parse_aggregate(p, JOG_NODE_ENUM, attrs, vis);
    case JOG_TOK_KW_EXTERN:
        return parse_extern_block(p, attrs, vis);
    default:
        error_here(p, "expected a declaration, found %s", jog_token_spelling(peek_kind(p)));
        return NULL;
    }
}

JogNode *jog_parse(CtArena *arena, CtDiag *diag, CtIntern *intern, const CtFile *file, const CtDa *tokens) {
    if (!arena || !file) {
        return NULL;
    }
    Parser parser;
    memset(&parser, 0, sizeof parser);
    parser.arena = arena;
    parser.diag = diag;
    parser.intern = intern;
    parser.path = file->path;
    parser.toks = tokens && tokens->count ? (const JogToken *)tokens->data : NULL;
    parser.count = tokens ? tokens->count : 0;

    JogPos pos;
    pos.file = file->path;
    pos.line = 1;
    pos.column = 1;
    if (parser.count > 0) {
        pos.line = parser.toks[0].line;
        pos.column = parser.toks[0].column;
    }
    JogNode *root = make_at(&parser, JOG_NODE_FILE, pos);
    if (!root) {
        return NULL;
    }
    CtDa imports;
    CtDa decls;
    if (!start_da(&parser, &imports, sizeof(JogNode *))) {
        return NULL;
    }
    if (!start_da(&parser, &decls, sizeof(JogNode *))) {
        ct_da_free(&imports);
        return NULL;
    }

    size_t spins = 0;
    size_t limit = parser.count + 8;
    while (!parser.oom && !is_eof(&parser)) {
        if (++spins > limit) {
            error_here(&parser, "parser failed to advance");
            break;
        }
        skip_seps(&parser);
        if (is_eof(&parser)) {
            break;
        }
        size_t at = parser.at;
        bool pending = parser.pending_gt;
        JogNode *decl = parse_top(&parser);
        if (parser.panic) {
            recover(&parser);
        }
        if (decl) {
            if (decl->kind == JOG_NODE_IMPORT) {
                push_node(&parser, &imports, decl);
            } else {
                parser.saw_non_import = true;
                push_node(&parser, &decls, decl);
            }
        }
        if (same_place(&parser, at, pending)) {
            if (!parser.panic) {
                error_here(&parser, "expected a declaration, found %s", jog_token_spelling(peek_kind(&parser)));
            }
            recover(&parser);
            if (same_place(&parser, at, pending)) {
                advance(&parser);
            }
        }
    }

    root->as.file.imports = take_nodes(&parser, &imports);
    root->as.file.decls = take_nodes(&parser, &decls);
    if (parser.oom) {
        return NULL;
    }
    return root;
}
