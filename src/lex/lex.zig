const std = @import("std");

pub const TokenKind = enum {
    eof,
    ident,
    int,
    string,
    // keywords
    kw_fn,
    kw_struct,
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
    // punct
    at,
    slash,
    pipe,
    ampamp,
    pipepipe,
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

    pub fn init(src: []const u8) Lexer {
        return .{ .src = src };
    }

    pub fn next(self: *Lexer) Token {
        self.skipTrivia();
        if (self.i >= self.src.len) return self.tok(.eof, self.src[self.i..self.i]);

        const start_i = self.i;
        const start_line = self.line;
        const start_col = self.col;
        const c = self.src[self.i];

        if (isIdentStart(c)) {
            self.advance();
            while (self.i < self.src.len and isIdentCont(self.src[self.i])) self.advance();
            const text = self.src[start_i..self.i];
            return .{
                .kind = keyword(text),
                .text = text,
                .line = start_line,
                .col = start_col,
            };
        }
        if (c >= '0' and c <= '9') {
            self.advance();
            while (self.i < self.src.len and self.src[self.i] >= '0' and self.src[self.i] <= '9') self.advance();
            return .{ .kind = .int, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
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
            return .{ .kind = .string, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
        }

        // two-char: ?.  (optional call / field)
        if (c == '?' and self.i + 1 < self.src.len and self.src[self.i + 1] == '.') {
            self.advance();
            self.advance();
            return .{ .kind = .dot_q, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
        }
        if (c == '=' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return .{ .kind = .eqeq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
        }
        if (c == '!' and self.i + 1 < self.src.len and self.src[self.i + 1] == '=') {
            self.advance();
            self.advance();
            return .{ .kind = .neq, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
        }
        if (c == '&' and self.i + 1 < self.src.len and self.src[self.i + 1] == '&') {
            self.advance();
            self.advance();
            return .{ .kind = .ampamp, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
        }
        if (c == '|' and self.i + 1 < self.src.len and self.src[self.i + 1] == '|') {
            self.advance();
            self.advance();
            return .{ .kind = .pipepipe, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
        }
        // Division vs // comment (comments already skipped in trivia when //).
        if (c == '/' and !(self.i + 1 < self.src.len and self.src[self.i + 1] == '/')) {
            self.advance();
            return .{ .kind = .slash, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
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
            else => .eof,
        };
        return .{ .kind = kind, .text = self.src[start_i..self.i], .line = start_line, .col = start_col };
    }

    fn skipTrivia(self: *Lexer) void {
        while (self.i < self.src.len) {
            const c = self.src[self.i];
            if (c == ' ' or c == '\t' or c == '\r') {
                self.advance();
                continue;
            }
            if (c == '\n') {
                self.advance();
                continue;
            }
            if (c == '/' and self.i + 1 < self.src.len and self.src[self.i + 1] == '/') {
                while (self.i < self.src.len and self.src[self.i] != '\n') self.advance();
                continue;
            }
            break;
        }
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

fn isIdentStart(c: u8) bool {
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or c == '_';
}
fn isIdentCont(c: u8) bool {
    return isIdentStart(c) or (c >= '0' and c <= '9');
}

fn keyword(text: []const u8) TokenKind {
    const map = .{
        .{ "fn", TokenKind.kw_fn },
        .{ "struct", TokenKind.kw_struct },
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
