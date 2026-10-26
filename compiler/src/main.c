#include "commands.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

typedef struct Command {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *usage;
    const char *summary;
} Command;

static const Command g_commands[] = {
    {"build", jog_cmd_build,
     "jog build [package] [-o file] [--release | --release-fast] [--keep-c] [--target wasm32]",
     "compile to an executable"},
    {"run", jog_cmd_run, "jog run [package] [--target wasm32] [-- args...]", "build and run"},
    {"test", jog_cmd_test, "jog test [package | --all]", "run *.test.jog tests"},
    {"check", jog_cmd_check, "jog check [package]", "run the frontend only"},
    {"emit-c", jog_cmd_emit_c, "jog emit-c [package]", "print the generated C"},
    {"emit-ir", jog_cmd_emit_ir, "jog emit-ir [package]", "print the IR"},
    {"parse", jog_cmd_parse, "jog parse <file>", "print the AST of a file"},
    {"lex", jog_cmd_lex, "jog lex <file>", "print the tokens of a file"},
};

static void print_usage(FILE *out) {
    fprintf(out, "usage: jog <command> [arguments]\n\ncommands:\n");
    for (size_t i = 0; i < sizeof g_commands / sizeof g_commands[0]; i++) {
        fprintf(out, "  %-70s %s\n", g_commands[i].usage, g_commands[i].summary);
    }
    fprintf(out, "  %-70s %s\n", "jog version", "print the compiler version");
    fprintf(out, "  %-70s %s\n", "jog help", "show this message");
    fprintf(out, "\nbuild, run, test, check, emit-c, and emit-ir accept --module <name>=<dir>, repeatable,\n"
                 "for each library module that jog.mod requires.\n");
}

int jog_cmd_not_implemented(const char *name, int milestone) {
    fprintf(stderr, "jog: '%s' is not implemented yet (milestone %d, see docs/plans/V1Compiler.md)\n", name,
            milestone);
    return 2;
}

#if defined(_WIN32)
/* The C runtime's argv is in the ANSI code page, which loses characters
   outside it. The UTF-16 command line is converted to UTF-8 instead. The
   blocks live until the process exits. */
static char **utf8_argv(int *argc_out) {
    int count = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    char **out = NULL;
    if (!wide || count < 0) {
        return NULL;
    }
    out = calloc((size_t)count + 1, sizeof(char *));
    for (int i = 0; out && i < count; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        out[i] = need > 0 ? malloc((size_t)need) : NULL;
        if (!out[i] || WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, out[i], need, NULL, NULL) != need) {
            out = NULL;
        }
    }
    LocalFree(wide);
    if (out) {
        *argc_out = count;
    }
    return out;
}
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
    char **wide_argv = utf8_argv(&argc);
    if (!wide_argv) {
        fputs("jog: cannot read the command line\n", stderr);
        return 2;
    }
    argv = wide_argv;
#endif
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }
    const char *name = argv[1];
    if (strcmp(name, "help") == 0 || strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (strcmp(name, "version") == 0 || strcmp(name, "--version") == 0) {
        printf("jog %s\n", JOG_VERSION);
        return 0;
    }
    for (size_t i = 0; i < sizeof g_commands / sizeof g_commands[0]; i++) {
        if (strcmp(name, g_commands[i].name) == 0) {
            return g_commands[i].run(argc - 2, argv + 2);
        }
    }
    fprintf(stderr, "jog: unknown command '%s'\n\n", name);
    print_usage(stderr);
    return 2;
}
