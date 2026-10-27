#include "phases.h"

#include <string.h>

/* Indented S-expression dump of a Jog tree. Leaves stay on one line.
   A node with children puts each child on its own line and closes on a
   line of its own. Optional parts are omitted when they are absent:
   public visibility, a missing return type, a null initializer. */

static void print_node(const JogNode *node, CtSb *out, int ind);

static void nl(CtSb *out, int ind) {
    ct_sb_putc(out, '\n');
    for (int i = 0; i < ind; i++) {
        ct_sb_append_cstr(out, "  ");
    }
}

static void close_node(CtSb *out, int ind, bool multiline) {
    if (multiline) {
        nl(out, ind);
    }
    ct_sb_putc(out, ')');
}

static void print_child(const JogNode *node, CtSb *out, int ind) {
    nl(out, ind);
    print_node(node, out, ind);
}

static void print_list(JogList list, CtSb *out, int ind) {
    for (size_t i = 0; i < list.count; i++) {
        print_child(list.items ? list.items[i] : NULL, out, ind);
    }
}

static const char *vis_text(JogVisibility vis) {
    switch (vis) {
    case JOG_VIS_PACKAGE:
        return " private";
    case JOG_VIS_FILE:
        return " private(file)";
    case JOG_VIS_SCOPE:
        return " private(scope)";
    default:
        return "";
    }
}

static void print_path(JogNames path, CtSb *out) {
    for (size_t i = 0; i < path.count; i++) {
        if (i > 0) {
            ct_sb_putc(out, '.');
        }
        ct_sb_append_cstr(out, path.items && path.items[i] ? path.items[i] : "?");
    }
}

static void print_escaped(CtSb *out, const char *text, size_t len) {
    ct_sb_putc(out, '"');
    if (!text) {
        len = 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
        case '\n':
            ct_sb_append_cstr(out, "\\n");
            break;
        case '\t':
            ct_sb_append_cstr(out, "\\t");
            break;
        case '\r':
            ct_sb_append_cstr(out, "\\r");
            break;
        case '\\':
            ct_sb_append_cstr(out, "\\\\");
            break;
        case '"':
            ct_sb_append_cstr(out, "\\\"");
            break;
        default:
            if (c < 0x20 || c == 0x7F) {
                ct_sb_appendf(out, "\\x%02X", c);
            } else {
                ct_sb_putc(out, (char)c);
            }
            break;
        }
    }
    ct_sb_putc(out, '"');
}

static void print_type_name(const JogNode *node, CtSb *out, int ind) {
    ct_sb_append_cstr(out, "(TYPE_NAME");
    if (node->as.type_name.path.count > 0) {
        ct_sb_putc(out, ' ');
        print_path(node->as.type_name.path, out);
    }
    if (node->as.type_name.args.count == 0) {
        ct_sb_putc(out, ')');
        return;
    }
    print_list(node->as.type_name.args, out, ind + 1);
    close_node(out, ind, true);
}

static void print_elem_type(const JogNode *node, CtSb *out, int ind, const char *kind) {
    ct_sb_appendf(out, "(%s%s", kind, node->as.type_elem.is_const ? " const" : "");
    print_child(node->as.type_elem.elem, out, ind + 1);
    close_node(out, ind, true);
}

static void print_fn(const JogNode *node, CtSb *out, int ind) {
    ct_sb_append_cstr(out, "(FN");
    if (node->as.fn.name && node->as.fn.name[0]) {
        ct_sb_putc(out, ' ');
        ct_sb_append_cstr(out, node->as.fn.name);
    }
    ct_sb_append_cstr(out, vis_text(node->visibility));
    if (node->as.fn.is_extern) {
        ct_sb_append_cstr(out, " extern");
    }
    if (node->as.fn.is_expect) {
        ct_sb_append_cstr(out, " expect");
    }
    if (node->as.fn.is_actual) {
        ct_sb_append_cstr(out, " actual");
    }
    if (node->as.fn.variadic) {
        ct_sb_append_cstr(out, " variadic");
    }
    bool multi = false;
    if (node->attrs.count > 0) {
        print_list(node->attrs, out, ind + 1);
        multi = true;
    }
    if (node->as.fn.receiver || node->as.fn.receiver_is_ptr) {
        nl(out, ind + 1);
        ct_sb_append_cstr(out, node->as.fn.receiver_is_ptr ? "recv *" : "recv");
        if (node->as.fn.receiver) {
            print_child(node->as.fn.receiver, out, ind + 2);
        }
        multi = true;
    }
    if (node->as.fn.type_params.count > 0) {
        print_list(node->as.fn.type_params, out, ind + 1);
        multi = true;
    }
    if (node->as.fn.params.count > 0) {
        print_list(node->as.fn.params, out, ind + 1);
        multi = true;
    }
    if (node->as.fn.contexts.count > 0) {
        nl(out, ind + 1);
        ct_sb_append_cstr(out, "with");
        print_list(node->as.fn.contexts, out, ind + 2);
        multi = true;
    }
    if (node->as.fn.ret) {
        nl(out, ind + 1);
        ct_sb_append_cstr(out, "->");
        print_child(node->as.fn.ret, out, ind + 2);
        multi = true;
    }
    if (node->as.fn.body) {
        nl(out, ind + 1);
        if (node->as.fn.expr_body) {
            ct_sb_putc(out, '=');
            print_child(node->as.fn.body, out, ind + 2);
        } else {
            print_node(node->as.fn.body, out, ind + 1);
        }
        multi = true;
    }
    close_node(out, ind, multi);
}

static void print_binding(const JogNode *node, CtSb *out, int ind) {
    ct_sb_appendf(out, "(%s", jog_node_kind_name(node->kind));
    ct_sb_append_cstr(out, vis_text(node->visibility));
    if (!node->as.binding.destructure && node->as.binding.names.count == 1 && node->as.binding.names.items) {
        ct_sb_putc(out, ' ');
        ct_sb_append_cstr(out, node->as.binding.names.items[0]);
    }
    bool multi = false;
    if (node->attrs.count > 0) {
        print_list(node->attrs, out, ind + 1);
        multi = true;
    }
    if (node->as.binding.destructure) {
        nl(out, ind + 1);
        ct_sb_append_cstr(out, "names");
        for (size_t i = 0; i < node->as.binding.names.count; i++) {
            ct_sb_putc(out, ' ');
            ct_sb_append_cstr(out, node->as.binding.names.items[i]);
        }
        multi = true;
    }
    if (node->as.binding.type) {
        print_child(node->as.binding.type, out, ind + 1);
        multi = true;
    }
    if (node->as.binding.init) {
        nl(out, ind + 1);
        ct_sb_putc(out, '=');
        print_child(node->as.binding.init, out, ind + 2);
        multi = true;
    }
    close_node(out, ind, multi);
}

static void print_aggregate(const JogNode *node, CtSb *out, int ind) {
    ct_sb_appendf(out, "(%s", jog_node_kind_name(node->kind));
    if (node->as.aggregate.name && node->as.aggregate.name[0]) {
        ct_sb_putc(out, ' ');
        ct_sb_append_cstr(out, node->as.aggregate.name);
    }
    ct_sb_append_cstr(out, vis_text(node->visibility));
    bool multi = false;
    if (node->attrs.count > 0) {
        print_list(node->attrs, out, ind + 1);
        multi = true;
    }
    if (node->as.aggregate.type_params.count > 0) {
        print_list(node->as.aggregate.type_params, out, ind + 1);
        multi = true;
    }
    if (node->as.aggregate.tag_type) {
        nl(out, ind + 1);
        ct_sb_append_cstr(out, ":");
        print_child(node->as.aggregate.tag_type, out, ind + 2);
        multi = true;
    }
    if (node->as.aggregate.members.count > 0) {
        print_list(node->as.aggregate.members, out, ind + 1);
        multi = true;
    }
    close_node(out, ind, multi);
}

static void print_call(const JogNode *node, CtSb *out, int ind) {
    ct_sb_append_cstr(out, "(CALL");
    print_child(node->as.call.callee, out, ind + 1);
    print_list(node->as.call.args, out, ind + 1);
    if (node->as.call.trailing) {
        nl(out, ind + 1);
        ct_sb_append_cstr(out, "trailing");
        print_child(node->as.call.trailing, out, ind + 2);
    }
    if (node->as.call.contexts.count > 0) {
        nl(out, ind + 1);
        ct_sb_append_cstr(out, "with");
        print_list(node->as.call.contexts, out, ind + 2);
    }
    close_node(out, ind, true);
}

static void print_node(const JogNode *node, CtSb *out, int ind) {
    if (!node) {
        ct_sb_append_cstr(out, "null");
        return;
    }
    switch (node->kind) {
    case JOG_NODE_FILE:
        ct_sb_append_cstr(out, "(FILE");
        if (node->as.file.imports.count == 0 && node->as.file.decls.count == 0) {
            ct_sb_putc(out, ')');
            break;
        }
        print_list(node->as.file.imports, out, ind + 1);
        print_list(node->as.file.decls, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_IMPORT:
        ct_sb_append_cstr(out, "(IMPORT ");
        print_path(node->as.import.path, out);
        if (node->as.import.alias) {
            ct_sb_appendf(out, " as %s", node->as.import.alias);
        }
        ct_sb_append_cstr(out, vis_text(node->visibility));
        if (node->attrs.count == 0) {
            ct_sb_putc(out, ')');
        } else {
            print_list(node->attrs, out, ind + 1);
            close_node(out, ind, true);
        }
        break;
    case JOG_NODE_FN:
        print_fn(node, out, ind);
        break;
    case JOG_NODE_VAR:
    case JOG_NODE_VAL:
    case JOG_NODE_CONTEXT:
        print_binding(node, out, ind);
        break;
    case JOG_NODE_TYPE_DECL: {
        ct_sb_append_cstr(out, "(TYPE_DECL");
        if (node->as.type_decl.name) {
            ct_sb_putc(out, ' ');
            ct_sb_append_cstr(out, node->as.type_decl.name);
        }
        ct_sb_append_cstr(out, vis_text(node->visibility));
        if (node->attrs.count > 0) {
            print_list(node->attrs, out, ind + 1);
        }
        if (node->as.type_decl.type_params.count > 0) {
            print_list(node->as.type_decl.type_params, out, ind + 1);
        }
        nl(out, ind + 1);
        ct_sb_putc(out, node->as.type_decl.distinct ? ':' : '=');
        if (node->as.type_decl.target) {
            print_child(node->as.type_decl.target, out, ind + 2);
        }
        close_node(out, ind, true);
        break;
    }
    case JOG_NODE_STRUCT:
    case JOG_NODE_INTERFACE:
    case JOG_NODE_ENUM:
        print_aggregate(node, out, ind);
        break;
    case JOG_NODE_EXTERN_BLOCK:
        ct_sb_append_cstr(out, "(EXTERN_BLOCK");
        ct_sb_append_cstr(out, vis_text(node->visibility));
        print_list(node->attrs, out, ind + 1);
        print_list(node->as.extern_block.fns, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_ATTRIBUTE:
        ct_sb_appendf(out, "(ATTRIBUTE %s", node->as.attribute.name ? node->as.attribute.name : "");
        if (node->as.attribute.has_parens && node->as.attribute.args.count == 0) {
            ct_sb_append_cstr(out, " ()");
        }
        if (node->as.attribute.args.count == 0) {
            ct_sb_putc(out, ')');
        } else {
            print_list(node->as.attribute.args, out, ind + 1);
            close_node(out, ind, true);
        }
        break;
    case JOG_NODE_FIELD: {
        ct_sb_appendf(out, "(FIELD %s%s", node->as.field.name ? node->as.field.name : "",
                      node->as.field.is_var ? " var" : "");
        ct_sb_append_cstr(out, vis_text(node->visibility));
        bool multi = false;
        if (node->attrs.count > 0) {
            print_list(node->attrs, out, ind + 1);
            multi = true;
        }
        if (node->as.field.type) {
            print_child(node->as.field.type, out, ind + 1);
            multi = true;
        }
        if (node->as.field.default_value) {
            nl(out, ind + 1);
            ct_sb_putc(out, '=');
            print_child(node->as.field.default_value, out, ind + 2);
            multi = true;
        }
        close_node(out, ind, multi);
        break;
    }
    case JOG_NODE_EMBED:
        ct_sb_append_cstr(out, "(EMBED");
        print_child(node->as.embed.type, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_TYPE_SET:
        ct_sb_append_cstr(out, "(TYPE_SET");
        for (size_t i = 0; i < node->as.type_set.terms.count; i++) {
            bool approx = node->as.type_set.approximate && node->as.type_set.approximate[i];
            nl(out, ind + 1);
            if (approx) {
                ct_sb_append_cstr(out, "~");
                print_child(node->as.type_set.terms.items[i], out, ind + 2);
            } else {
                print_node(node->as.type_set.terms.items[i], out, ind + 1);
            }
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_VARIANT:
        ct_sb_appendf(out, "(VARIANT %s", node->as.variant.name ? node->as.variant.name : "");
        if (node->as.variant.fields.count == 0 && !node->as.variant.value) {
            ct_sb_putc(out, ')');
            break;
        }
        print_list(node->as.variant.fields, out, ind + 1);
        if (node->as.variant.value) {
            nl(out, ind + 1);
            ct_sb_putc(out, '=');
            print_child(node->as.variant.value, out, ind + 2);
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_PARAM:
        ct_sb_appendf(out, "(PARAM %s%s", node->as.param.name ? node->as.param.name : "",
                      node->as.param.pack ? " pack" : "");
        if (!node->as.param.type && !node->as.param.default_value) {
            ct_sb_putc(out, ')');
            break;
        }
        if (node->as.param.type) {
            print_child(node->as.param.type, out, ind + 1);
        }
        if (node->as.param.default_value) {
            nl(out, ind + 1);
            ct_sb_putc(out, '=');
            print_child(node->as.param.default_value, out, ind + 2);
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_TYPE_PARAM:
        ct_sb_appendf(out, "(TYPE_PARAM %s", node->as.type_param.name ? node->as.type_param.name : "");
        if (!node->as.type_param.constraint) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.type_param.constraint, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_TYPE_NAME:
        print_type_name(node, out, ind);
        break;
    case JOG_NODE_TYPE_PTR:
        print_elem_type(node, out, ind, "TYPE_PTR");
        break;
    case JOG_NODE_TYPE_SLICE:
        print_elem_type(node, out, ind, "TYPE_SLICE");
        break;
    case JOG_NODE_TYPE_ARRAY:
        ct_sb_append_cstr(out, "(TYPE_ARRAY");
        if (node->as.type_array.infer_len) {
            ct_sb_append_cstr(out, " _");
        }
        if (!node->as.type_array.infer_len && node->as.type_array.len) {
            print_child(node->as.type_array.len, out, ind + 1);
        }
        print_child(node->as.type_array.elem, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_TYPE_BYTES:
        ct_sb_appendf(out, "(TYPE_BYTES%s", node->as.type_bytes.is_const ? " const" : "");
        if (!node->as.type_bytes.len) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.type_bytes.len, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_TYPE_OPTION:
        ct_sb_append_cstr(out, "(TYPE_OPTION");
        print_child(node->as.type_elem.elem, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_TYPE_TUPLE:
        ct_sb_append_cstr(out, "(TYPE_TUPLE");
        print_list(node->as.type_tuple.elems, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_TYPE_FN:
        ct_sb_append_cstr(out, "(TYPE_FN");
        if (node->as.type_fn.is_extern) {
            ct_sb_append_cstr(out, " extern");
        }
        if (node->as.type_fn.escaping) {
            ct_sb_append_cstr(out, " escaping");
        }
        print_list(node->as.type_fn.params, out, ind + 1);
        nl(out, ind + 1);
        ct_sb_append_cstr(out, "->");
        print_child(node->as.type_fn.ret, out, ind + 2);
        if (node->as.type_fn.contexts.count > 0) {
            nl(out, ind + 1);
            ct_sb_append_cstr(out, "with");
            print_list(node->as.type_fn.contexts, out, ind + 2);
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_TYPE_UNIT:
        ct_sb_append_cstr(out, "(TYPE_UNIT)");
        break;
    case JOG_NODE_BLOCK:
        ct_sb_append_cstr(out, "(BLOCK");
        if (node->as.block.stmts.count == 0) {
            ct_sb_putc(out, ')');
            break;
        }
        print_list(node->as.block.stmts, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_EXPR_STMT:
        ct_sb_append_cstr(out, "(EXPR_STMT");
        print_child(node->as.expr_stmt.expr, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_ASSIGN:
        ct_sb_appendf(out, "(ASSIGN %s", jog_token_spelling(node->as.assign.op));
        print_child(node->as.assign.target, out, ind + 1);
        print_child(node->as.assign.value, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_DESTRUCTURE:
        ct_sb_append_cstr(out, "(DESTRUCTURE");
        print_list(node->as.destructure.targets, out, ind + 1);
        nl(out, ind + 1);
        ct_sb_putc(out, '=');
        print_child(node->as.destructure.value, out, ind + 2);
        close_node(out, ind, true);
        break;
    case JOG_NODE_RETURN:
        ct_sb_append_cstr(out, "(RETURN");
        if (node->as.ret.values.count == 0) {
            ct_sb_putc(out, ')');
            break;
        }
        print_list(node->as.ret.values, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_BREAK:
        ct_sb_append_cstr(out, "(BREAK)");
        break;
    case JOG_NODE_CONTINUE:
        ct_sb_append_cstr(out, "(CONTINUE)");
        break;
    case JOG_NODE_INT_LIT:
    case JOG_NODE_FLOAT_LIT:
        ct_sb_appendf(out, "(%s %s)", jog_node_kind_name(node->kind), node->as.literal.text ? node->as.literal.text : "");
        break;
    case JOG_NODE_STRING_LIT:
    case JOG_NODE_CSTRING_LIT:
    case JOG_NODE_CHAR_LIT:
    case JOG_NODE_HEXBLOB_LIT:
        ct_sb_appendf(out, "(%s ", jog_node_kind_name(node->kind));
        print_escaped(out, node->as.literal.text, node->as.literal.len);
        ct_sb_putc(out, ')');
        break;
    case JOG_NODE_BOOL_LIT:
        ct_sb_appendf(out, "(BOOL %s)", node->as.literal.value ? "true" : "false");
        break;
    case JOG_NODE_NULL_LIT:
        ct_sb_append_cstr(out, "(NULL)");
        break;
    case JOG_NODE_INTERP:
        ct_sb_append_cstr(out, "(INTERP");
        print_list(node->as.interp.parts, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_IDENT:
    case JOG_NODE_DOT_VARIANT:
        ct_sb_appendf(out, "(%s %s", jog_node_kind_name(node->kind), node->as.ident.name ? node->as.ident.name : "");
        if (node->as.ident.type_args.count == 0) {
            ct_sb_putc(out, ')');
        } else {
            print_list(node->as.ident.type_args, out, ind + 1);
            close_node(out, ind, true);
        }
        break;
    case JOG_NODE_TUPLE:
        ct_sb_append_cstr(out, "(TUPLE");
        print_list(node->as.tuple.elems, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_UNARY:
    case JOG_NODE_FORCE:
        ct_sb_appendf(out, "(%s %s", jog_node_kind_name(node->kind), jog_token_spelling(node->as.unary.op));
        print_child(node->as.unary.operand, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_BINARY:
        ct_sb_appendf(out, "(BINARY %s", jog_token_spelling(node->as.binary.op));
        print_child(node->as.binary.lhs, out, ind + 1);
        print_child(node->as.binary.rhs, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_CAST:
        ct_sb_appendf(out, "(CAST %s", jog_token_spelling(node->as.cast.op));
        print_child(node->as.cast.expr, out, ind + 1);
        print_child(node->as.cast.type, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_IS:
        ct_sb_append_cstr(out, "(IS");
        print_child(node->as.is.expr, out, ind + 1);
        print_child(node->as.is.type, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_CALL:
        print_call(node, out, ind);
        break;
    case JOG_NODE_ARG:
        ct_sb_append_cstr(out, "(ARG");
        if (node->as.arg.name) {
            ct_sb_putc(out, ' ');
            ct_sb_append_cstr(out, node->as.arg.name);
        }
        print_child(node->as.arg.value, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_INDEX:
        ct_sb_append_cstr(out, "(INDEX");
        print_child(node->as.index.base, out, ind + 1);
        print_child(node->as.index.index, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_SLICE:
        ct_sb_append_cstr(out, "(SLICE");
        print_child(node->as.slice.base, out, ind + 1);
        print_child(node->as.slice.lo, out, ind + 1);
        print_child(node->as.slice.hi, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_MEMBER:
        ct_sb_appendf(out, "(MEMBER%s", node->as.member.safe ? " ?." : "");
        print_child(node->as.member.base, out, ind + 1);
        nl(out, ind + 1);
        if (node->as.member.name) {
            ct_sb_append_cstr(out, node->as.member.name);
        } else {
            ct_sb_appendf(out, ".%u", (unsigned)node->as.member.tuple_index);
        }
        if (node->as.member.type_args.count > 0) {
            print_list(node->as.member.type_args, out, ind + 1);
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_BUILTIN:
        ct_sb_appendf(out, "(BUILTIN %s", node->as.builtin.name ? node->as.builtin.name : "");
        if (node->as.builtin.args.count == 0) {
            ct_sb_putc(out, ')');
        } else {
            print_list(node->as.builtin.args, out, ind + 1);
            close_node(out, ind, true);
        }
        break;
    case JOG_NODE_WITH:
        ct_sb_append_cstr(out, "(WITH");
        if (node->as.with.values.count == 0 && !node->as.with.body) {
            ct_sb_putc(out, ')');
            break;
        }
        print_list(node->as.with.values, out, ind + 1);
        print_child(node->as.with.body, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_DEFER:
    case JOG_NODE_ERRDEFER:
        ct_sb_appendf(out, "(%s", jog_node_kind_name(node->kind));
        if (!node->as.defer.body) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.defer.body, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_FOR: {
        const char *form = "forever";
        if (node->as.for_.form == JOG_FOR_COND) {
            form = "cond";
        } else if (node->as.for_.form == JOG_FOR_IN) {
            form = "in";
        }
        ct_sb_appendf(out, "(FOR %s", form);
        if (node->as.for_.first) {
            ct_sb_putc(out, ' ');
            ct_sb_append_cstr(out, node->as.for_.first);
            if (node->as.for_.second) {
                ct_sb_putc(out, ' ');
                ct_sb_append_cstr(out, node->as.for_.second);
            }
        }
        if (!node->as.for_.expr && !node->as.for_.body) {
            ct_sb_putc(out, ')');
            break;
        }
        if (node->as.for_.expr) {
            print_child(node->as.for_.expr, out, ind + 1);
        }
        print_child(node->as.for_.body, out, ind + 1);
        close_node(out, ind, true);
        break;
    }
    case JOG_NODE_ATTR_BLOCK:
        ct_sb_append_cstr(out, "(ATTR_BLOCK");
        if (node->attrs.count == 0 && !node->as.attr_block.body) {
            ct_sb_putc(out, ')');
            break;
        }
        print_list(node->attrs, out, ind + 1);
        print_child(node->as.attr_block.body, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_IF:
        ct_sb_append_cstr(out, "(IF");
        if (!node->as.if_.cond && !node->as.if_.then_block && !node->as.if_.else_branch) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.if_.cond, out, ind + 1);
        print_child(node->as.if_.then_block, out, ind + 1);
        if (node->as.if_.else_branch) {
            nl(out, ind + 1);
            ct_sb_append_cstr(out, "else");
            print_child(node->as.if_.else_branch, out, ind + 2);
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_LAMBDA:
        ct_sb_append_cstr(out, "(LAMBDA");
        if (node->as.lambda.implicit_it) {
            ct_sb_append_cstr(out, " it");
        }
        if (node->as.lambda.params.count == 0 && !node->as.lambda.ret && !node->as.lambda.body) {
            ct_sb_putc(out, ')');
            break;
        }
        if (node->as.lambda.params.count > 0) {
            print_list(node->as.lambda.params, out, ind + 1);
        }
        if (node->as.lambda.ret) {
            nl(out, ind + 1);
            ct_sb_append_cstr(out, "->");
            print_child(node->as.lambda.ret, out, ind + 2);
        }
        if (node->as.lambda.expr_body) {
            nl(out, ind + 1);
            ct_sb_putc(out, '=');
            print_child(node->as.lambda.body, out, ind + 2);
        } else {
            print_child(node->as.lambda.body, out, ind + 1);
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_ARRAY_LIT:
        ct_sb_append_cstr(out, "(ARRAY_LIT");
        if (!node->as.array_lit.type && node->as.array_lit.elems.count == 0) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.array_lit.type, out, ind + 1);
        print_list(node->as.array_lit.elems, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_WHEN:
        ct_sb_append_cstr(out, "(WHEN");
        if (!node->as.when.subject && node->as.when.arms.count == 0) {
            ct_sb_putc(out, ')');
            break;
        }
        if (node->as.when.subject) {
            print_child(node->as.when.subject, out, ind + 1);
        }
        print_list(node->as.when.arms, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_WHEN_ARM:
        ct_sb_append_cstr(out, "(WHEN_ARM");
        if (node->as.when_arm.conds.count == 0) {
            ct_sb_append_cstr(out, " else");
        }
        if (node->as.when_arm.conds.count == 0 && !node->as.when_arm.body) {
            ct_sb_putc(out, ')');
            break;
        }
        print_list(node->as.when_arm.conds, out, ind + 1);
        if (node->as.when_arm.body) {
            print_child(node->as.when_arm.body, out, ind + 1);
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_GO:
        ct_sb_append_cstr(out, "(GO");
        if (node->as.go.args.count == 0 && node->as.go.contexts.count == 0 && !node->as.go.body) {
            ct_sb_putc(out, ')');
            break;
        }
        if (node->as.go.args.count > 0) {
            nl(out, ind + 1);
            ct_sb_append_cstr(out, "args");
            print_list(node->as.go.args, out, ind + 2);
        }
        if (node->as.go.contexts.count > 0) {
            nl(out, ind + 1);
            ct_sb_append_cstr(out, "with");
            print_list(node->as.go.contexts, out, ind + 2);
        }
        if (node->as.go.body) {
            print_child(node->as.go.body, out, ind + 1);
        }
        close_node(out, ind, true);
        break;
    case JOG_NODE_PAT_VARIANT:
        ct_sb_appendf(out, "(PAT_VARIANT %s", node->as.pat_variant.name ? node->as.pat_variant.name : "");
        if (node->as.pat_variant.fields.count == 0) {
            ct_sb_putc(out, ')');
            break;
        }
        print_list(node->as.pat_variant.fields, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_PAT_FIELD:
        ct_sb_append_cstr(out, "(PAT_FIELD");
        if (node->as.pat_field.name) {
            ct_sb_putc(out, ' ');
            ct_sb_append_cstr(out, node->as.pat_field.name);
        }
        if (!node->as.pat_field.pattern) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.pat_field.pattern, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_PAT_BIND:
        ct_sb_appendf(out, "(PAT_BIND %s)", node->as.ident.name ? node->as.ident.name : "");
        break;
    case JOG_NODE_PAT_IN:
        ct_sb_append_cstr(out, "(PAT_IN");
        if (!node->as.pat_expr.expr) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.pat_expr.expr, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_PAT_IS:
        ct_sb_append_cstr(out, "(PAT_IS");
        if (!node->as.pat_is.type) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.pat_is.type, out, ind + 1);
        close_node(out, ind, true);
        break;
    case JOG_NODE_PAT_EXPR:
        ct_sb_append_cstr(out, "(PAT_EXPR");
        if (!node->as.pat_expr.expr) {
            ct_sb_putc(out, ')');
            break;
        }
        print_child(node->as.pat_expr.expr, out, ind + 1);
        close_node(out, ind, true);
        break;
    default:
        ct_sb_appendf(out, "(%s)", jog_node_kind_name(node->kind));
        break;
    }
}

void jog_ast_print(const JogNode *node, CtSb *out) {
    if (!out) {
        return;
    }
    if (!node) {
        ct_sb_append_cstr(out, "(null)\n");
        return;
    }
    print_node(node, out, 0);
    ct_sb_putc(out, '\n');
}
