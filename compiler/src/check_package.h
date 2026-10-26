#ifndef JOG_CHECK_PACKAGE_H
#define JOG_CHECK_PACKAGE_H

#include "check_body.h"

/* Cross-package names, bytes/slice .ptr and .len, pointer `as`, and @test.
   Not a contract header. Public names are visible from any package.
   private and private(package) stay in the declaring package.
   private(file) and top-level private(scope) stay in the declaring file. */

/* 1 when member is .ptr or .len of bytes, str, or a slice, or that view
   has no such field (a diagnostic is already reported).
   0 when the caller should keep checking.
   -1 when the checker has stopped. The base expression is already typed. */
int jog_check_package_view(Check *ck, CheckJob *job, JogNode *member, JogType **out);

/* 1 when assign writes .ptr or .len of a mutable bytes or slice (the value
   is checked, or an error is already reported). 0 when it is not that view.
   -1 when the checker has stopped. */
int jog_check_package_view_assign(Check *ck, CheckJob *job, JogBlockScope *scope, JogNode *assign);

/* True when `as` from `from` to `to` reinterprets a pointer. Niche *T? counts
   as a pointer. The conversion is unchecked. */
bool jog_check_package_cast(const Check *ck, const JogType *from, const JogType *to);

/* @test functions live in *.test.jog, take no parameters, and return ().
   A with clause is allowed. The test harness passes top-level contexts
   of those types. The Testing interface is not provided in this slice. */
void jog_check_package_tests(Check *ck);

#endif
