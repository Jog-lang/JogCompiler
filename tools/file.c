#include "file.h"
#include "fs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void emit_read_error(CtDiag *diag, const char *path, const char *reason) {
    CtPos pos = {0};
    pos.file = path;
    if (reason && reason[0]) {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot read '%s': %s", path ? path : "", reason);
    } else {
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot read '%s'", path ? path : "");
    }
}

bool ct_file_read(CtArena *arena, CtDiag *diag, const char *path, CtFile *out) {
    if (out) {
        out->path = NULL;
        out->data = NULL;
        out->size = 0;
    }
    if (!arena || !path || !out) {
        emit_read_error(diag, path, "invalid arguments");
        return false;
    }

    FILE *fp = ct_fs_fopen(path, "rb");
    if (!fp) {
        int err = errno;
        CtPos pos = {0};
        pos.file = path;
        ct_diag_emit(diag, CT_SEVERITY_ERROR, pos, "cannot open '%s': %s", path, strerror(err));
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        int err = errno;
        fclose(fp);
        emit_read_error(diag, path, strerror(err));
        return false;
    }
    long end = ftell(fp);
    if (end < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        int err = errno;
        fclose(fp);
        emit_read_error(diag, path, err ? strerror(err) : "cannot determine length");
        return false;
    }

    size_t size = (size_t)end;
    if (size == SIZE_MAX) {
        fclose(fp);
        emit_read_error(diag, path, "file is too large");
        return false;
    }

    char *data = ct_arena_alloc(arena, size + 1, 1);
    if (!data) {
        fclose(fp);
        emit_read_error(diag, path, "out of memory");
        return false;
    }

    size_t nread = 0;
    if (size > 0) {
        nread = fread(data, 1, size, fp);
    }
    int read_error = ferror(fp);
    int err = errno;
    fclose(fp);
    if (nread != size || read_error) {
        emit_read_error(diag, path, read_error && err ? strerror(err) : "short read");
        return false;
    }
    data[size] = '\0';

    char *path_copy = ct_arena_strdup(arena, path);
    if (!path_copy) {
        emit_read_error(diag, path, "out of memory");
        return false;
    }

    out->path = path_copy;
    out->data = data;
    out->size = size;
    return true;
}
