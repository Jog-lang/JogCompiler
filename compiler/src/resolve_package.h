#ifndef JOG_RESOLVE_PACKAGE_H
#define JOG_RESOLVE_PACKAGE_H

#include "symbols.h"

/* Builds each package scope from top-level declarations and each file
   scope from imports and file-private names. Stores the package scope in
   JogPackage.scope. Public and private(package) names go in the package
   map. private(file), top-level private(scope), and imports go in
   scope->file_scopes, parallel to package->files. Those blocks fall
   through to the package map. scope->universe is the builtin scope passed
   here; jog_file_lookup falls through to it, jog_package_scope_lookup
   does not.

   A method (a function with a receiver) is recorded on its node and is
   not a package name. Functions in an extern block take the block's
   visibility, written onto the function node: the parser leaves those
   functions public because a modifier on the function itself is rejected.
   The discard name "_" is not bound. val and var may bind several names.
   The declaration node's symbol is the first name that was bound.

   An import's local name is its alias, or the last path segment. The
   symbol's package is the imported package. The path must name a package
   in program->packages. A package cannot import itself. Duplicate names
   in the same scope are errors, including a file-private name that
   matches a package-visible name. The same file-private name may be used
   in another file. Unused imports are not reported here.

   Returns false if program, its arena, diag, or intern is NULL, a scope
   cannot be built, or a diagnostic was emitted. universe may be NULL. */
bool jog_resolve_packages(JogProgram *program, const JogPackageScope *universe);

/* File scope, then its package scope, then that scope's universe.
   NULL if package_scope or name is NULL, file_index is out of range,
   or the name is not bound. name is compared by content. */
JogSymbol *jog_file_lookup(const JogPackageScope *package_scope, size_t file_index, const char *name);

/* Public symbols are visible from any package. private and
   private(package) are visible in the declaring package. private(file)
   and private(scope) are visible only in the declaring file: top-level
   private(scope) is private(file). A symbol with no declaring node is
   visible everywhere. An import is visible only in the file that wrote
   it. NULL is not visible. from_file is compared by content. */
bool jog_symbol_visible_from(const JogSymbol *symbol, const JogPackage *from_package, const char *from_file);

#endif
