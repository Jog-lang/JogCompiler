#ifndef JOG_LOWER_PACKAGE_H
#define JOG_LOWER_PACKAGE_H

#include "lower_internal.h"

/* True when an imported package has a runtime initializer. Each such package,
   and the root package when it has runtime initializers too, gets its own
   init function. The module init calls those functions in loader order:
   a package runs after the packages it imports. False when every runtime
   initializer is in the root package, so the caller keeps one inlined init. */
bool jog_lower_package_inits(Lower *L);

/* When tests were loaded, the program entry calls each @test function in
   source order and does not call fn main. No @test leaves main_func alone. */
bool jog_lower_package_tests(Lower *L);

/* True when node is a pointer `as` and *out is set. */
bool jog_lower_package_cast(Lower *L, JogNode *node, JirOperand src, JirOperand *out);

/* True when node is .ptr or .len of bytes or a slice and *out is set. */
bool jog_lower_package_view(Lower *L, JogNode *node, JirOperand *out);

/* True when node assigns .ptr or .len of a mutable bytes or slice. */
bool jog_lower_package_view_assign(Lower *L, JogNode *node);

#endif
