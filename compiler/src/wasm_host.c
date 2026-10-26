#include "wasm_host.h"

#include "arena.h"
#include "da.h"
#include "fs.h"
#include "proc.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define HOST_PATH_SEP ';'
#else
#define HOST_PATH_SEP ':'
#endif

#if defined(_WIN32)
static int is_separator(char c) {
    return c == '/' || c == '\\';
}

static const char *base_name(const char *path) {
    const char *base = path;
    const char *p;
    for (p = path; *p; p++) {
        if (is_separator(*p)) {
            base = p + 1;
        }
    }
    return base;
}
#endif

static const char *file_in_dir(CtArena *arena, const char *dir, const char *name, bool *oom) {
    char *path = ct_path_join(arena, dir, name);
    if (!path) {
        *oom = true;
        return NULL;
    }
    if (ct_fs_is_file(path)) {
        return path;
    }
#if defined(_WIN32)
    if (!strchr(base_name(name), '.')) {
        size_t len = strlen(path);
        char *exe = ct_arena_alloc(arena, len + 5, 1);
        if (!exe) {
            *oom = true;
            return NULL;
        }
        memcpy(exe, path, len);
        memcpy(exe + len, ".exe", 5);
        if (ct_fs_is_file(exe)) {
            return exe;
        }
    }
#endif
    return NULL;
}

static const char *search_path(CtArena *arena, const char *name, bool *oom) {
    const char *path = ct_fs_getenv(arena, "PATH");
    const char *start;
    if (!path) {
        return NULL;
    }
    start = path;
    for (;;) {
        const char *end = start;
        char *dir = NULL;
        const char *found;
        while (*end && *end != HOST_PATH_SEP) {
            end++;
        }
        if (end == start) {
            dir = ct_arena_strdup(arena, ".");
        } else {
            size_t len = (size_t)(end - start);
            dir = ct_arena_alloc(arena, len + 1, 1);
            if (dir) {
                memcpy(dir, start, len);
                dir[len] = '\0';
            }
        }
        if (!dir) {
            *oom = true;
            return NULL;
        }
        found = file_in_dir(arena, dir, name, oom);
        if (*oom || found) {
            return found;
        }
        if (*end == '\0') {
            break;
        }
        start = end + 1;
    }
    return NULL;
}

bool jog_wasm_run(const char *wasm_path, const char *runtime_root, char **args, int nargs, const char *stdout_path,
                  int *exit_code, CtDiag *diag) {
    CtPos pos = {0};
    CtArena *arena = NULL;
    bool oom = false;
    const char *node = NULL;
    const char *script = NULL;
    int code = 0;
    pos.file = wasm_path;
    if (!exit_code || !wasm_path || wasm_path[0] == '\0' || !runtime_root || runtime_root[0] == '\0') {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot run wasm: missing module or runtime");
        return false;
    }
    if (nargs < 0 || (nargs > 0 && !args)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot run wasm: missing program arguments");
        return false;
    }
    if (!ct_fs_is_file(wasm_path)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "wasm module '%s' was not found", wasm_path);
        return false;
    }
    arena = ct_arena_create();
    if (!arena) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        return false;
    }
    node = search_path(arena, "node", &oom);
    script = ct_path_join(arena, runtime_root, "wasm/host.js");
    if (oom || !script) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
        ct_arena_destroy(arena);
        return false;
    }
    if (!node) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "Wasm host 'node' was not found on PATH");
        ct_arena_destroy(arena);
        return false;
    }
    if (!ct_fs_is_file(script)) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "Wasm host script '%s' was not found", script);
        ct_arena_destroy(arena);
        return false;
    }
    {
        CtDa argv;
        const char *slot = NULL;
        int a = 0;
        int ready = ct_da_init(&argv, sizeof(const char *));
        if (!ready) {
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
            ct_arena_destroy(arena);
            return false;
        }
        slot = node;
        if (!ct_da_push(&argv, &slot)) {
            ct_da_free(&argv);
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
            ct_arena_destroy(arena);
            return false;
        }
        slot = script;
        if (!ct_da_push(&argv, &slot)) {
            ct_da_free(&argv);
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
            ct_arena_destroy(arena);
            return false;
        }
        slot = wasm_path;
        if (!ct_da_push(&argv, &slot)) {
            ct_da_free(&argv);
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
            ct_arena_destroy(arena);
            return false;
        }
        for (a = 0; a < nargs; a++) {
            if (!args[a]) {
                ct_da_free(&argv);
                ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot run wasm: missing program arguments");
                ct_arena_destroy(arena);
                return false;
            }
            slot = args[a];
            if (!ct_da_push(&argv, &slot)) {
                ct_da_free(&argv);
                ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
                ct_arena_destroy(arena);
                return false;
            }
        }
        slot = NULL;
        if (!ct_da_push(&argv, &slot) || !argv.data) {
            ct_da_free(&argv);
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "out of memory");
            ct_arena_destroy(arena);
            return false;
        }
        if (!ct_proc_run(argv.data, stdout_path, NULL, &code)) {
            ct_da_free(&argv);
            ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot run the Wasm host '%s'", node);
            ct_arena_destroy(arena);
            return false;
        }
        ct_da_free(&argv);
    }
    *exit_code = code;
    ct_arena_destroy(arena);
    return true;
}
