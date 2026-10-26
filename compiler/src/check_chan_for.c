#include "check_chan_for.h"

#include <string.h>

/* Chan<T> is the distinct pointer the coroutine slice builds.
   for v in ch binds v as a constant of T. A second name is not that form. */

static bool is_chan(const JogType *type) {
    return type && type->kind == JOG_TY_DISTINCT && type->as.nominal.package == NULL &&
           type->as.nominal.type_arg_count == 1 && type->as.nominal.name && strcmp(type->as.nominal.name, "Chan") == 0;
}

int jog_check_chan_for(Check *ck, JogNode *node, const JogType *base, JogType **elem) {
    JogType *error;
    if (elem) {
        *elem = NULL;
    }
    if (!is_chan(base)) {
        return 0;
    }
    error = jog_check_error_type(ck);
    if (!node || node->as.for_.second) {
        jog_check_fail(ck, node ? node->pos : (JogPos){0}, "for-in over a channel has one name");
        if (elem) {
            *elem = error;
        }
        return -1;
    }
    if (!base->as.nominal.type_args || !base->as.nominal.type_args[0]) {
        jog_check_fail(ck, node->pos, "channel element has no type");
        if (elem) {
            *elem = error;
        }
        return -1;
    }
    if (elem) {
        *elem = base->as.nominal.type_args[0];
    }
    return 1;
}
