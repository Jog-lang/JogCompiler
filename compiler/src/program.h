#ifndef JOG_PROGRAM_H
#define JOG_PROGRAM_H

#include "ast.h"
#include "types.h"

#include "arena.h"
#include "da.h"
#include "diag.h"
#include "file.h"
#include "intern.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The whole program being compiled: every package reachable from the root,
   std included. The loader builds it, the checker annotates it, lowering
   reads it. Everything lives in arena.

   Contract owner: see docs/plans/V1Parallel.md. */

typedef struct JogPackage JogPackage;

typedef struct JogSourceFile {
    CtFile file;
    CtDa tokens;   /* JogToken; kept for diagnostics and tools */
    JogNode *ast;  /* JOG_NODE_FILE */
    bool is_test;  /* *.test.jog */
} JogSourceFile;

/* OS-suffixed sources for one name on a jog.mod `targets` line.
   The current target's files are also in JogPackage.files and are checked.
   Every other target is parsed only. No existing field changed meaning. */
typedef struct JogTargetSources {
    const char *os; /* windows, linux, macos, or wasm */
    JogSourceFile **files;
    size_t file_count;
} JogTargetSources;

struct JogPackage {
    const char *path;      /* canonical import path, e.g. "net.http"; "" for the root package;
                              "<module>" and "<module>.<rest>" for a --module dependency */
    const char *name;      /* last path segment, or the module name for the root */
    const char *dir;       /* folder on disk */
    bool is_std;
    JogSourceFile **files; /* sorted by file name */
    size_t file_count;
    JogPackage **imports;  /* distinct packages imported by any file */
    size_t import_count;
    void *scope;           /* the resolver's package scope; opaque to everyone else */
    const char *module_name; /* owning module: the root's name, a --module name, or "std" */
    const char *module_root; /* that module's folder; @embed stays inside it */
    /* From the owning module's `targets` line. NULL when the line is absent;
       then only the current target is checked. No existing field changed meaning. */
    const char **targets;
    size_t target_count;
    JogTargetSources *target_sources; /* one group per targets entry; NULL when absent */
    /* Other-target OS files owned here so their tokens can be freed once.
       target_sources aliases these and the current target's files. */
    JogSourceFile **platform_files;
    size_t platform_file_count;
};

/* One --module <name>=<dir>. dir is the module's source folder. */
typedef struct JogModuleArg {
    const char *name;
    const char *dir;
} JogModuleArg;

typedef enum JogBuildMode {
    JOG_BUILD_DEBUG = 0,
    JOG_BUILD_RELEASE,
    JOG_BUILD_RELEASE_FAST /* no bounds checks */
} JogBuildMode;

typedef struct JogOptions {
    JogBuildMode mode;
    bool include_tests; /* load *.test.jog files */
    uint32_t ptr_size;  /* 8 for x64, 4 for wasm32 */
    bool start_package; /* also load the package that contains the start path (jog check) */
} JogOptions;

typedef struct JogProgram {
    CtArena *arena;
    CtDiag *diag;
    CtIntern *intern;
    JogTypes *types;
    JogOptions options;
    const char *module_root;  /* folder containing jog.mod */
    const char *module_name;
    const char *std_root;     /* the std directory; import std.io is std_root/io */
    const char *runtime_root; /* folder containing jogrt.h and jogrt.c */
    JogPackage **packages;    /* dependency order: every package after the ones it imports */
    size_t package_count;
    JogPackage *root;
    const char **link_libs; /* link names for this target: the root's, then each module's in
                               --module order; a repeated name keeps its last position */
    size_t link_lib_count;
    const JogModuleArg *module_args; /* --module mappings in command-line order; set before jog_load */
    size_t module_arg_count;
    bool is_library;                 /* the root jog.mod says kind library */
} JogProgram;

#endif
