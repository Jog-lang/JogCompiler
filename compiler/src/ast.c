#include "ast.h"

#include <string.h>

static const char *const g_kind_names[JOG_NODE_COUNT] = {
#define JOG_NODE_NAME(kind, member) #kind,
    JOG_NODE_KINDS(JOG_NODE_NAME)
#undef JOG_NODE_NAME
};

JogNode *jog_node_new(CtArena *arena, JogNodeKind kind, JogPos pos) {
    JogNode *node = ct_arena_alloc(arena, sizeof(*node), _Alignof(JogNode));
    if (!node) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->pos = pos;
    return node;
}

static void *copy_items(CtArena *arena, const CtDa *da, size_t item_size, bool *oom) {
    if (!da || da->count == 0) {
        return NULL;
    }
    void *items = ct_arena_alloc(arena, da->count * item_size, _Alignof(void *));
    if (!items) {
        if (oom) {
            *oom = true;
        }
        return NULL;
    }
    memcpy(items, da->data, da->count * item_size);
    return items;
}

JogList jog_list_from_da(CtArena *arena, const CtDa *da, bool *oom) {
    JogList list = {0};
    list.items = copy_items(arena, da, sizeof(JogNode *), oom);
    list.count = list.items ? da->count : 0;
    return list;
}

JogNames jog_names_from_da(CtArena *arena, const CtDa *da, bool *oom) {
    JogNames names = {0};
    names.items = copy_items(arena, da, sizeof(const char *), oom);
    names.count = names.items ? da->count : 0;
    return names;
}

const char *jog_node_kind_name(JogNodeKind kind) {
    if ((unsigned)kind >= JOG_NODE_COUNT) {
        return "?";
    }
    return g_kind_names[kind];
}
