const std = @import("std");

pub const TokenKind = enum {
    eof,
    ident,
    int,
    float,
    string,
    cstring,
    char,
    // keywords
    kw_fn,
    kw_struct,
    kw_object,
    kw_interface,
    kw_type,
    kw_val,
    kw_var,
    kw_return,
    kw_if,
    kw_else,
    kw_with,
    kw_null,
    kw_as,
    kw_true,
    kw_false,
    kw_extern,
    kw_export,
    kw_expect,
    kw_actual,
    kw_panic,
    kw_while,
    kw_break,
    kw_for,
    kw_continue,
    kw_defer,
    kw_errdefer,
    kw_enum,
    kw_when,
    kw_import,
    kw_in,
    kw_private,
    kw_require,
    kw_is,
    // punct
    at,
    slash,
    pipe,
    ampamp,
    pipepipe,
    percent,
    caret,
    tilde,
    lte,
    gte,
    /// `<=>` three-way compare (method `op<=>`).
    spaceship,
    shl,
    shr,
    arrow, // ->
    ellipsis, // ...
    dotdot, // ..
    pluseq,
    minuseq,
    stareq,
    slasheq,
    percenteq,
    lparen,
    rparen,
    lbrace,
    rbrace,
    lbracket,
    rbracket,
    comma,
    colon,
    semicolon,
    dot,
    dot_q, // ?.
    star,
    amp,
    eq,
    eqeq,
    neq,
    plus,
    minus,
    lt,
    gt,
    bang,
    bang_bang, // !!
    question,
};

pub const Token = struct {
    kind: TokenKind,
    text: []const u8,
    line: u32,
    col: u32,
};

pub const Lexer = struct {
    src: []const u8,
    i: usize = 0,
    line: u32 = 1,
    col: u32 = 1,
    /// Last emitted token kind (for the Grammar newline → SEP rule).
    last_kind: TokenKind = .eof,
    has_last: bool = false,
    /// Bracket stack: '(', '[', '{' — SEP only when empty or top is '{'.
    brackets: [64]u8 = undefined,
    bracket_depth: usize = 0,
    /// Pending inserted SEP (emitted as `.semicolon`).
    pending_sep: bool = false,
    sep_line: u32 = 1,
    sep_col: u32 = 1,

    pub fn init(src: []const u8) Lexer {
        return .{ .src = src };
    }

    fn emit(self: *Lexer, t: Token) Token {
        self.noteEmitted(t.kind);
        return t;
    }

    pub fn next(self: *Lexer) Token {
        if (self.pending_sep) {
            self.pending_sep = false;
            const t: Token = .{
                .kind = .semicolon,
                .text = ";",
                .line = self.sep_line,
                .col = self.sep_col,
            };
            self.noteEmitted(t.kind);
            return t;
        }
        self.skipTrivia();
        if (self.pending_sep) {
            self.pending_sep = false;
            const t: Token = .{
                .kind = .semicolon,
                .text = ";",
                .line = self.sep_line,
                .col = self.sep_col,
            };
            self.noteEmitted(t.kind);
            return t;
        }
        if (self.i >= self.src.len) return self.tok(.eof, self.src[self.i..self.i]);

        const start_i = self.i;
        const start_line = self.line;
        const start_col = self.col;
        const c = self.src[self.i];

        // c"…" before treating `c` as an ident
        if (c == 'c' and self.i + 1 < self.src.len and self.src[self.i + 1] == '"') {
            self.advance(); // c
            self.advance(); // "
            while (self.i < self.src.len and self.src[self.i] != '"') {
                if (self.src[self.i] == '\\' and self.i + 1 < self.src.len) self.advance();
                self.advance();
            }
            if (self.i < self.src.len) self.advance();
            return self.emit(.{ .kind = .cstring, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (isIdentStart(c)) {
            self.advance();
            while (self.i < self.src.len and isIdentCont(self.src[self.i])) self.advance();
            const text = self.src[start_i..self.i];
            return self.emit(.{
                .kind = keyword(text),
                .text = text,
                .line = start_line,
                .col = start_col,
            });
        }
        if (c >= '0' and c <= '9') {
            self.advance();
            // Hex: 0x… / 0X…
            if ((c == '0') and self.i < self.src.len and (self.src[self.i] == 'x' or self.src[self.i] == 'X')) {
                self.advance();
                while (self.i < self.src.len and isHex(self.src[self.i])) self.advance();
                return self.emit(.{ .kind = .int, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
            }
            while (self.i < self.src.len and self.src[self.i] >= '0' and self.src[self.i] <= '9') self.advance();
            if (self.i + 1 < self.src.len and self.src[self.i] == '.' and self.src[self.i + 1] >= '0' and self.src[self.i + 1] <= '9') {
                self.advance(); // .
                while (self.i < self.src.len and self.src[self.i] >= '0' and self.src[self.i] <= '9') self.advance();
                return self.emit(.{ .kind = .float, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
            }
            return self.emit(.{ .kind = .int, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '"') {
            self.advance();
            while (self.i < self.src.len and self.src[self.i] != '"') {
                if (self.src[self.i] == '\\' and self.i + 1 < self.src.len) {
                    self.advance();
                }
                self.advance();
            }
            if (self.i < self.src.len) self.advance(); // closing "
            return self.emit(.{ .kind = .string, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }

        // two-char: ?.  (optional call / field)
        if (c == '?' and self.i + 1 < self.src.len and self.src[self.i + 1] == '.') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .dot_q, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '=' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .eqeq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '!' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .neq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '!' and self.i + 1 < self.src.len and self.src[self.i + 1] == '!') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .bang_bang, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '&' and self.i + 1 < self.src.len and self.src[self.i + 1] == '&') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .ampamp, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '|' and self.i + 1 < self.src.len and self.src[self.i + 1] == '|') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .pipepipe, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '<' and self.i + 2 < self.src.len and self.src[self.i + 1] == '=' and self.src[self.i + 2] == '>') {
            self.advance();
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .spaceship, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '<' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .lte, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '>' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .gte, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '<' and self.i + 1 < self.src.len and self.src[self.i + 1] == '<') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .shl, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '>' and self.i + 1 < self.src.len and self.src[self.i + 1] == '>') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .shr, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '-' and self.i + 1 < self.src.len and self.src[self.i + 1] == '>') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .arrow, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        // `...` before `..` so three dots are not `..` plus a leftover `.`.
        if (c == '.' and self.i + 2 < self.src.len and self.src[self.i + 1] == '.' and self.src[self.i + 2] == '.') {
            self.advance();
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .ellipsis, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '.' and self.i + 1 < self.src.len and self.src[self.i + 1] == '.') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .dotdot, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '+' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .pluseq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '-' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .minuseq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '*' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .stareq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        if (c == '%' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return self.emit(.{ .kind = .percenteq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }
        // Division vs // comment vs /= .
        if (c == '/') {
            if (self.i + 1 < self.src.len and self.src[self.i + 1] == '/') {
                // comment handled in skipTrivia; fall through shouldn't happen
            } else if (self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
                self.advance();
                self.advance();
                return self.emit(.{ .kind = .slasheq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
            } else {
                self.advance();
                return self.emit(.{ .kind = .slash, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
            }
        }
        // Char lit 'x' or '\n'
        if (c == '\'') {
            self.advance();
            if (self.i < self.src.len and self.src[self.i] == '\\' and self.i + 1 < self.src.len) {
                self.advance();
                self.advance();
            } else if (self.i < self.src.len) {
                self.advance();
            }
            if (self.i < self.src.len and self.src[self.i] == '\'') self.advance();
            return self.emit(.{ .kind = .char, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
        }

        self.advance();
        const kind: TokenKind = switch (c) {
            '(' => .lparen,
            ')' => .rparen,
            '{' => .lbrace,
            '}' => .rbrace,
            '[' => .lbracket,
            ']' => .rbracket,
            ',' => .comma,
            ':' => .colon,
            ';' => .semicolon,
            '.' => .dot,
            '*' => .star,
            '&' => .amp,
            '=' => .eq,
            '+' => .plus,
            '-' => .minus,
            '<' => .lt,
            '>' => .gt,
            '!' => .bang,
            '?' => .question,
            '@' => .at,
            '|' => .pipe,
            '%' => .percent,
            '^' => .caret,
            '~' => .tilde,
            else => .eof,
        };
        return self.emit(.{ .kind = kind, .text = self.src[start_i..self.i], .line = start_line, .col = start_col });
    }

    fn skipTrivia(self: *Lexer) void {
        while (self.i < self.src.len) {
            const c = self.src[self.i];
            if (c == ' ' or c == '\t' or c == '\r') {
                self.advance();
                continue;
            }
            if (c == '\n') {
                const nl_line = self.line;
                const nl_col = self.col;
                self.advance();
                if (self.shouldInsertSep() and !self.continuesLine()) {
                    self.pending_sep = true;
                    self.sep_line = nl_line;
                    self.sep_col = nl_col;
                    return;
                }
                continue;
            }
            if (c == '/' and self.i + 1 < self.src.len and self.src[self.i + 1] == '/') {
                while (self.i < self.src.len and self.src[self.i] != '\n') self.advance();
                continue;
            }
            // Block / doc comments: /* … */ and /** … */
            if (c == '/' and self.i + 1 < self.src.len and self.src[self.i + 1] == '*') {
                self.advance(); // /
                self.advance(); // *
                while (self.i + 1 < self.src.len) {
                    if (self.src[self.i] == '*' and self.src[self.i + 1] == '/') {
                        self.advance();
                        self.advance();
                        break;
                    }
                    // Block comment spanning lines counts as a newline for SEP.
                    if (self.src[self.i] == '\n') {
                        const nl_line = self.line;
                        const nl_col = self.col;
                        self.advance();
                        // Finish comment first; SEP decision after close is complex — treat
                        // in-comment newlines as trivia only (V1 inserts on the physical \n
                        // when ends_statement; keep simple: no SEP mid-comment).
                        _ = nl_line;
                        _ = nl_col;
                        continue;
                    }
                    self.advance();
                }
                continue;
            }
            break;
        }
    }

    fn shouldInsertSep(self: *Lexer) bool {
        if (!self.has_last) return false;
        if (!endsStatement(self.last_kind)) return false;
        if (self.bracket_depth == 0) return true;
        return self.brackets[self.bracket_depth - 1] == '{';
    }

    /// Lookahead: next significant line starts with `.`, `?.`, or `?:`.
    fn continuesLine(self: *Lexer) bool {
        var i = self.i;
        while (i < self.src.len) {
            const c = self.src[i];
            if (c == ' ' or c == '\t' or c == '\r' or c == '\n') {
                i += 1;
                continue;
            }
            if (c == '/' and i + 1 < self.src.len and self.src[i + 1] == '/') {
                i += 2;
                while (i < self.src.len and self.src[i] != '\n') i += 1;
                continue;
            }
            if (c == '/' and i + 1 < self.src.len and self.src[i + 1] == '*') {
                i += 2;
                while (i + 1 < self.src.len and !(self.src[i] == '*' and self.src[i + 1] == '/')) i += 1;
                if (i + 1 < self.src.len) i += 2;
                continue;
            }
            if (c == '.') return true;
            if (c == '?' and i + 1 < self.src.len and self.src[i + 1] == '.') return true;
            if (c == '?' and i + 1 < self.src.len and self.src[i + 1] == ':') return true;
            return false;
        }
        return false;
    }

    fn noteEmitted(self: *Lexer, kind: TokenKind) void {
        switch (kind) {
            .lparen => self.pushBracket('('),
            .lbracket => self.pushBracket('['),
            .lbrace => self.pushBracket('{'),
            .rparen => self.popBracket('('),
            .rbracket => self.popBracket('['),
            .rbrace => self.popBracket('{'),
            else => {},
        }
        self.last_kind = kind;
        self.has_last = true;
    }

    fn pushBracket(self: *Lexer, b: u8) void {
        if (self.bracket_depth < self.brackets.len) {
            self.brackets[self.bracket_depth] = b;
            self.bracket_depth += 1;
        }
    }

    fn popBracket(self: *Lexer, expected: u8) void {
        _ = expected;
        if (self.bracket_depth > 0) self.bracket_depth -= 1;
    }

    fn advance(self: *Lexer) void {
        if (self.i >= self.src.len) return;
        if (self.src[self.i] == '\n') {
            self.line += 1;
            self.col = 1;
        } else {
            self.col += 1;
        }
        self.i += 1;
    }

    fn tok(self: *Lexer, kind: TokenKind, text: []const u8) Token {
        return .{ .kind = kind, .text = text, .line = self.line, .col = self.col };
    }
};

fn endsStatement(kind: TokenKind) bool {
    return switch (kind) {
        .ident,
        .int,
        .float,
        .string,
        .cstring,
        .char,
        .rparen,
        .rbracket,
        .rbrace,
        .question,
        .bang_bang,
        .gt,
        .shr,
        .kw_return,
        .kw_break,
        .kw_continue,
        .kw_true,
        .kw_false,
        .kw_null,
        => true,
        else => false,
    };
}

fn isIdentStart(c: u8) bool {
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or c == '_';
}
fn isIdentCont(c: u8) bool {
    return isIdentStart(c) or (c >= '0' and c <= '9');
}
fn isHex(c: u8) bool {
    return (c >= '0' and c <= '9') or (c >= 'a' and c <= 'f') or (c >= 'A' and c <= 'F');
}

fn keyword(text: []const u8) TokenKind {
    const map = .{
        .{ "fn", TokenKind.kw_fn },
        .{ "struct", TokenKind.kw_struct },
        .{ "object", TokenKind.kw_object },
        .{ "interface", TokenKind.kw_interface },
        .{ "type", TokenKind.kw_type },
        .{ "val", TokenKind.kw_val },
        .{ "var", TokenKind.kw_var },
        .{ "return", TokenKind.kw_return },
        .{ "if", TokenKind.kw_if },
        .{ "else", TokenKind.kw_else },
        .{ "with", TokenKind.kw_with },
        .{ "null", TokenKind.kw_null },
        .{ "as", TokenKind.kw_as },
        .{ "true", TokenKind.kw_true },
        .{ "false", TokenKind.kw_false },
        .{ "extern", TokenKind.kw_extern },
        .{ "export", TokenKind.kw_export },
        .{ "expect", TokenKind.kw_expect },
        .{ "actual", TokenKind.kw_actual },
        .{ "panic", TokenKind.kw_panic },
        .{ "while", TokenKind.kw_while },
        .{ "break", TokenKind.kw_break },
        .{ "for", TokenKind.kw_for },
        .{ "continue", TokenKind.kw_continue },
        .{ "defer", TokenKind.kw_defer },
        .{ "errdefer", TokenKind.kw_errdefer },
        .{ "enum", TokenKind.kw_enum },
        .{ "when", TokenKind.kw_when },
        .{ "import", TokenKind.kw_import },
        .{ "in", TokenKind.kw_in },
        .{ "private", TokenKind.kw_private },
        .{ "require", TokenKind.kw_require },
        .{ "is", TokenKind.kw_is },
    };
    inline for (map) |e| {
        if (std.mem.eql(u8, text, e[0])) return e[1];
    }
    return .ident;
}

test "lex hello" {
    var lx = Lexer.init("fn main() {\n  print(\"hi\")\n}\n");
    try std.testing.expect(lx.next().kind == .kw_fn);
    try std.testing.expect(lx.next().kind == .ident);
}

test "lex inserts SEP after statement-ending newline" {
    // After `x` (ident) a newline becomes SEP so `*p` cannot continue as multiply.
    var lx = Lexer.init("var p = &x\n*p = 1\n");
    var saw_sep = false;
    var saw_star_after_sep = false;
    var prev_sep = false;
    while (true) {
        const t = lx.next();
        if (t.kind == .eof) break;
        if (t.kind == .semicolon) {
            saw_sep = true;
            prev_sep = true;
            continue;
        }
        if (prev_sep and t.kind == .star) saw_star_after_sep = true;
        prev_sep = false;
    }
    try std.testing.expect(saw_sep);
    try std.testing.expect(saw_star_after_sep);
}
