#ifndef JOG_CHECK_CHAN_FOR_H
#define JOG_CHECK_CHAN_FOR_H

#include "check_body.h"

/* 1 when base is Chan<T> and *elem is T.
   0 when base is not a channel.
   -1 when it is a channel and the for is not valid. */

int jog_check_chan_for(Check *ck, JogNode *node, const JogType *base, JogType **elem);

#endif
