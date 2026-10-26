#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Driver stream (D), milestone 4. jog test loads *.test.jog, builds the
   module, and runs every @test function. // expect: lines in those files
   are the whole stdout. A failed build, a mismatch, or a non-zero status
   is a diagnostic, and the command exits with that status.
   Declared here so commands.h does not gain a signature. Both live in
   cmd_build.c. */
int jog_drive_test(const char *start, const char **modules, int module_count);
int jog_parse_module_flag(int argc, char **argv, int *i, const char **out);

static int usage(void) {
    fputs("usage: jog test [package | --all] [--module name=dir ...]\n", stderr);
    return 2;
}

int jog_cmd_test(int argc, char **argv) {
    const char *package = NULL;
    const char **modules = NULL;
    int module_count = 0;
    int all = 0;
    int status = 0;
    int i;
    modules = malloc(((size_t)argc + 1) * sizeof(const char *));
    if (!modules) {
        fputs("jog: out of memory\n", stderr);
        return 1;
    }
    for (i = 0; i < argc; i++) {
        const char *module = NULL;
        int flag = jog_parse_module_flag(argc, argv, &i, &module);
        if (flag == 2) {
            free((void *)modules);
            return 2;
        }
        if (flag == 0) {
            modules[module_count++] = module;
            continue;
        }
        if (strcmp(argv[i], "--all") == 0) {
            if (all || package) {
                free((void *)modules);
                return usage();
            }
            all = 1;
            continue;
        }
        if (argv[i][0] == '-' || package || all) {
            free((void *)modules);
            return usage();
        }
        package = argv[i];
    }
    /* --all is the module found from the current directory. The loader
       still compiles the root package and the packages it imports. */
    status = jog_drive_test(all ? NULL : package, modules, module_count);
    free((void *)modules);
    return status;
}
