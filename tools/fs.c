#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "fs.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#endif

static int is_separator(char c) {
    return c == '/' || c == '\\';
}

static bool name_matches(const char *name, const char *suffix) {
    if (name[0] == '.') {
        return false;
    }
    if (!suffix) {
        return true;
    }
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    return suffix_len <= name_len && memcmp(name + name_len - suffix_len, suffix, suffix_len) == 0;
}

static int compare_names(const void *a, const void *b) {
    const char *left = *(const char *const *)a;
    const char *right = *(const char *const *)b;
    return strcmp(left, right);
}

static void emit_dir_error(CtDiag *diag, const char *dir, const char *reason) {
    CtPos pos = {0};
    pos.file = dir;
    ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot list directory '%s': %s", dir ? dir : "", reason);
}

char *ct_path_join(CtArena *arena, const char *a, const char *b) {
    if (!arena || !a || !b) {
        return NULL;
    }
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    if (a_len == 0) {
        return ct_arena_strdup(arena, b);
    }
    int need_sep = !is_separator(a[a_len - 1]);
    char *joined = ct_arena_alloc(arena, a_len + (size_t)need_sep + b_len + 1, 1);
    if (!joined) {
        return NULL;
    }
    memcpy(joined, a, a_len);
    if (need_sep) {
        joined[a_len] = '/';
    }
    memcpy(joined + a_len + (size_t)need_sep, b, b_len + 1);
    return joined;
}

#if defined(_WIN32)

wchar_t *ct_fs_widen(const char *utf8) {
    if (!utf8) {
        return NULL;
    }
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    wchar_t *wide = malloc((size_t)need * sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, need) != need) {
        free(wide);
        return NULL;
    }
    return wide;
}

char *ct_fs_narrow(CtArena *arena, const wchar_t *wide) {
    if (!arena || !wide) {
        return NULL;
    }
    int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (need <= 0) {
        return NULL;
    }
    char *utf8 = ct_arena_alloc(arena, (size_t)need, 1);
    if (!utf8) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, need, NULL, NULL) != need) {
        return NULL;
    }
    return utf8;
}

static DWORD attributes(const char *path) {
    wchar_t *wide = ct_fs_widen(path);
    if (!wide) {
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD attrs = GetFileAttributesW(wide);
    free(wide);
    return attrs;
}

bool ct_fs_is_dir(const char *path) {
    if (!path) {
        return false;
    }
    DWORD attrs = attributes(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool ct_fs_is_file(const char *path) {
    if (!path) {
        return false;
    }
    DWORD attrs = attributes(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

FILE *ct_fs_fopen(const char *path, const char *mode) {
    wchar_t *wide_path = ct_fs_widen(path);
    wchar_t *wide_mode = ct_fs_widen(mode);
    FILE *fp = NULL;
    if (wide_path && wide_mode) {
        fp = _wfopen(wide_path, wide_mode);
    }
    free(wide_path);
    free(wide_mode);
    return fp;
}

bool ct_fs_remove(const char *path) {
    wchar_t *wide = ct_fs_widen(path);
    bool ok = wide && DeleteFileW(wide);
    free(wide);
    return ok;
}

bool ct_fs_mkdir(const char *path) {
    wchar_t *wide = ct_fs_widen(path);
    bool ok = wide && CreateDirectoryW(wide, NULL);
    free(wide);
    return ok;
}

char *ct_fs_getenv(CtArena *arena, const char *name) {
    wchar_t *wide_name = ct_fs_widen(name);
    char *value = NULL;
    if (!arena || !wide_name) {
        free(wide_name);
        return NULL;
    }
    DWORD need = GetEnvironmentVariableW(wide_name, NULL, 0);
    if (need > 0) {
        wchar_t *buf = malloc((size_t)need * sizeof(wchar_t));
        if (buf) {
            DWORD got = GetEnvironmentVariableW(wide_name, buf, need);
            if (got < need) {
                value = ct_fs_narrow(arena, buf);
            }
            free(buf);
        }
    } else if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
        value = ct_arena_strdup(arena, "");
    }
    free(wide_name);
    return value;
}

bool ct_fs_list(CtArena *arena, CtDiag *diag, const char *dir, CtFsKind kind, const char *suffix, CtDa *out) {
    if (!arena || !dir || !out || out->item_size != sizeof(const char *)) {
        emit_dir_error(diag, dir, "invalid arguments");
        return false;
    }
    char *pattern = ct_path_join(arena, dir, "*");
    if (!pattern) {
        emit_dir_error(diag, dir, "out of memory");
        return false;
    }
    wchar_t *wide_pattern = ct_fs_widen(pattern);
    if (!wide_pattern) {
        emit_dir_error(diag, dir, "path is not valid UTF-8");
        return false;
    }

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(wide_pattern, &entry);
    free(wide_pattern);
    if (find == INVALID_HANDLE_VALUE) {
        emit_dir_error(diag, dir, GetLastError() == ERROR_PATH_NOT_FOUND ? "no such directory" : "cannot open");
        return false;
    }

    size_t first = out->count;
    bool ok = true;
    do {
        bool is_dir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_dir != (kind == CT_FS_DIRS)) {
            continue;
        }
        char *name = ct_fs_narrow(arena, entry.cFileName);
        if (!name) {
            emit_dir_error(diag, dir, "out of memory");
            ok = false;
            break;
        }
        if (!name_matches(name, suffix)) {
            continue;
        }
        if (!ct_da_push(out, &name)) {
            emit_dir_error(diag, dir, "out of memory");
            ok = false;
            break;
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);

    if (out->count > first) {
        qsort((const char **)out->data + first, out->count - first, sizeof(const char *), compare_names);
    }
    return ok;
}

#else

static bool push_name(CtArena *arena, CtDa *out, const char *name) {
    const char *copy = ct_arena_strdup(arena, name);
    return copy && ct_da_push(out, &copy);
}

bool ct_fs_is_dir(const char *path) {
    struct stat info;
    return path && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

bool ct_fs_is_file(const char *path) {
    struct stat info;
    return path && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

FILE *ct_fs_fopen(const char *path, const char *mode) {
    if (!path || !mode) {
        return NULL;
    }
    return fopen(path, mode);
}

bool ct_fs_remove(const char *path) {
    return path && remove(path) == 0;
}

bool ct_fs_mkdir(const char *path) {
    return path && mkdir(path, 0777) == 0;
}

char *ct_fs_getenv(CtArena *arena, const char *name) {
    const char *value = name ? getenv(name) : NULL;
    if (!arena || !value) {
        return NULL;
    }
    return ct_arena_strdup(arena, value);
}

bool ct_fs_list(CtArena *arena, CtDiag *diag, const char *dir, CtFsKind kind, const char *suffix, CtDa *out) {
    if (!arena || !dir || !out || out->item_size != sizeof(const char *)) {
        emit_dir_error(diag, dir, "invalid arguments");
        return false;
    }
    DIR *handle = opendir(dir);
    if (!handle) {
        emit_dir_error(diag, dir, strerror(errno));
        return false;
    }

    size_t first = out->count;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (!name_matches(entry->d_name, suffix)) {
            continue;
        }
        char *full = ct_path_join(arena, dir, entry->d_name);
        if (!full) {
            emit_dir_error(diag, dir, "out of memory");
            ok = false;
            break;
        }
        bool is_dir = ct_fs_is_dir(full);
        if (is_dir != (kind == CT_FS_DIRS) || (!is_dir && !ct_fs_is_file(full))) {
            continue;
        }
        if (!push_name(arena, out, entry->d_name)) {
            emit_dir_error(diag, dir, "out of memory");
            ok = false;
            break;
        }
    }
    closedir(handle);

    if (out->count > first) {
        qsort((const char **)out->data + first, out->count - first, sizeof(const char *), compare_names);
    }
    return ok;
}

#endif
