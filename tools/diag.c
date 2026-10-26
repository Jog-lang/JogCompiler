#include "diag.h"

#include <stdarg.h>
#include <string.h>

typedef struct CtDiagMsg {
    struct CtDiagMsg *next;
    CtSeverity severity;
    const char *file;
    unsigned line;
    unsigned column;
    const char *message;
} CtDiagMsg;

struct CtDiag {
    CtArena *arena;
    CtDiagMsg *head;
    CtDiagMsg *tail;
    size_t error_count;
};

static const char *severity_name(CtSeverity severity) {
    switch (severity) {
    case CT_SEVERITY_NOTE:
        return "note";
    case CT_SEVERITY_WARNING:
        return "warning";
    case CT_SEVERITY_ERROR:
        return "error";
    }
    return "note";
}

static char *format_message(CtArena *arena, const char *fmt, va_list args) {
    va_list sizing;
    va_copy(sizing, args);
    int needed = vsnprintf(NULL, 0, fmt, sizing);
    va_end(sizing);
    if (needed < 0) {
        return ct_arena_strdup(arena, fmt);
    }

    char *text = ct_arena_alloc(arena, (size_t)needed + 1, 1);
    if (!text) {
        return NULL;
    }
    if (vsnprintf(text, (size_t)needed + 1, fmt, args) < 0) {
        text[0] = '\0';
    }
    return text;
}

CtDiag *ct_diag_create(CtArena *arena) {
    if (!arena) {
        return NULL;
    }
    CtDiag *diag = ct_arena_alloc(arena, sizeof(*diag), _Alignof(CtDiag));
    if (!diag) {
        return NULL;
    }
    diag->arena = arena;
    diag->head = NULL;
    diag->tail = NULL;
    diag->error_count = 0;
    return diag;
}

void ct_diag_emit(CtDiag *diag, CtSeverity severity, CtPos pos, const char *fmt, ...) {
    if (!diag || !diag->arena || !fmt) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    char *message = format_message(diag->arena, fmt, args);
    va_end(args);
    if (!message) {
        return;
    }

    const char *file = NULL;
    if (pos.file) {
        file = ct_arena_strdup(diag->arena, pos.file);
        if (!file) {
            return;
        }
    }

    CtDiagMsg *msg = ct_arena_alloc(diag->arena, sizeof(*msg), _Alignof(CtDiagMsg));
    if (!msg) {
        return;
    }
    msg->next = NULL;
    msg->severity = severity;
    msg->file = file;
    msg->line = pos.line;
    msg->column = pos.column;
    msg->message = message;

    if (diag->tail) {
        diag->tail->next = msg;
    } else {
        diag->head = msg;
    }
    diag->tail = msg;
    if (severity == CT_SEVERITY_ERROR) {
        diag->error_count++;
    }
}

size_t ct_diag_error_count(const CtDiag *diag) {
    if (!diag) {
        return 0;
    }
    return diag->error_count;
}

void ct_diag_print(const CtDiag *diag, FILE *out) {
    if (!diag || !out) {
        return;
    }
    for (const CtDiagMsg *msg = diag->head; msg; msg = msg->next) {
        const char *severity = severity_name(msg->severity);
        const char *text = msg->message ? msg->message : "";
        int has_file = msg->file && msg->file[0];
        if (msg->line == 0) {
            if (has_file) {
                fprintf(out, "%s: %s: %s\n", msg->file, severity, text);
            } else {
                fprintf(out, "%s: %s\n", severity, text);
            }
        } else if (has_file) {
            fprintf(out, "%s:%u:%u: %s: %s\n", msg->file, msg->line, msg->column, severity, text);
        } else {
            fprintf(out, "%u:%u: %s: %s\n", msg->line, msg->column, severity, text);
        }
    }
}

static void json_string(FILE *out, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    fputc('"', out);
    for (; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (c < 0x20) {
                fprintf(out, "\\u%04x", (unsigned)c);
            } else {
                fputc(c, out);
            }
            break;
        }
    }
    fputc('"', out);
}

void ct_diag_print_json(const CtDiag *diag, FILE *out) {
    if (!diag || !out) {
        return;
    }
    for (const CtDiagMsg *msg = diag->head; msg; msg = msg->next) {
        fputs("{\"file\":", out);
        json_string(out, msg->file ? msg->file : "");
        fprintf(out, ",\"line\":%u,\"column\":%u,\"severity\":", msg->line, msg->column);
        json_string(out, severity_name(msg->severity));
        fputs(",\"message\":", out);
        json_string(out, msg->message ? msg->message : "");
        fputs("}\n", out);
    }
}
