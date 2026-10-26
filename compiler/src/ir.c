#include "ir.h"

#include <string.h>

JirModule *jir_module_create(CtArena *arena, JogTypes *types) {
    if (!arena || !types) {
        return NULL;
    }
    JirModule *module = ct_arena_alloc(arena, sizeof(*module), _Alignof(JirModule));
    if (!module) {
        return NULL;
    }
    memset(module, 0, sizeof(*module));
    module->arena = arena;
    module->types = types;
    module->main_func = -1;
    module->init_func = -1;
    ct_da_init(&module->funcs, sizeof(JirFunc *));
    ct_da_init(&module->globals, sizeof(JirGlobal *));
    return module;
}

void jir_module_destroy(JirModule *module) {
    if (!module) {
        return;
    }
    for (size_t i = 0; i < module->funcs.count; i++) {
        JirFunc *func = *(JirFunc **)ct_da_at(&module->funcs, i);
        for (size_t b = 0; b < func->blocks.count; b++) {
            ct_da_free(&jir_block_at(func, (uint32_t)b)->insts);
        }
        ct_da_free(&func->blocks);
        ct_da_free(&func->locals);
    }
    ct_da_free(&module->funcs);
    ct_da_free(&module->globals);
}

int32_t jir_func_add(JirModule *module, const char *name, const char *jog_name, JirLinkage linkage,
                     JogType *fn_type, const char *const *param_names) {
    if (!module || !name || !fn_type || fn_type->kind != JOG_TY_FN || module->funcs.count >= INT32_MAX) {
        return -1;
    }
    JirFunc *func = ct_arena_alloc(module->arena, sizeof(*func), _Alignof(JirFunc));
    if (!func) {
        return -1;
    }
    memset(func, 0, sizeof(*func));
    func->name = name;
    func->jog_name = jog_name ? jog_name : name;
    func->linkage = linkage;
    func->type = fn_type;
    func->param_count = (uint32_t)fn_type->as.fn.param_count;
    ct_da_init(&func->locals, sizeof(JirLocal));
    ct_da_init(&func->blocks, sizeof(JirBlock));
    for (uint32_t i = 0; i < func->param_count; i++) {
        JirLocal local = {param_names ? param_names[i] : NULL, fn_type->as.fn.params[i], true, false};
        if (!ct_da_push(&func->locals, &local)) {
            return -1;
        }
    }
    if (!ct_da_push(&module->funcs, &func)) {
        return -1;
    }
    return (int32_t)(module->funcs.count - 1);
}

JirFunc *jir_func(const JirModule *module, uint32_t index) {
    if (!module || index >= module->funcs.count) {
        return NULL;
    }
    return *(JirFunc **)ct_da_at((CtDa *)&module->funcs, index);
}

uint32_t jir_local_add(JirFunc *func, const char *name, JogType *type) {
    JirLocal local = {name, type, false, false};
    ct_da_push(&func->locals, &local);
    return (uint32_t)(func->locals.count - 1);
}

JirLocal *jir_local_at(const JirFunc *func, uint32_t index) {
    return ct_da_at((CtDa *)&func->locals, index);
}

uint32_t jir_block_add(JirFunc *func, const char *label) {
    JirBlock block;
    memset(&block, 0, sizeof(block));
    block.label = label;
    ct_da_init(&block.insts, sizeof(JirInst));
    ct_da_push(&func->blocks, &block);
    return (uint32_t)(func->blocks.count - 1);
}

JirBlock *jir_block_at(const JirFunc *func, uint32_t index) {
    return ct_da_at((CtDa *)&func->blocks, index);
}

JirInst *jir_emit(JirFunc *func, uint32_t block, JirOp op, int32_t dst) {
    JirBlock *b = jir_block_at(func, block);
    if (!b) {
        return NULL;
    }
    JirInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = op;
    inst.dst = dst;
    if (!ct_da_push(&b->insts, &inst)) {
        return NULL;
    }
    return ct_da_at(&b->insts, b->insts.count - 1);
}

bool jir_set_args(JirModule *module, JirInst *inst, const JirOperand *args, size_t count) {
    if (!module || !inst || (count > 0 && !args)) {
        return false;
    }
    inst->args = NULL;
    inst->arg_count = 0;
    if (count == 0) {
        return true;
    }
    JirOperand *copy = ct_arena_alloc(module->arena, count * sizeof(*copy), _Alignof(JirOperand));
    if (!copy) {
        return false;
    }
    memcpy(copy, args, count * sizeof(*copy));
    inst->args = copy;
    inst->arg_count = count;
    return true;
}

void jir_terminate(JirFunc *func, uint32_t block, JirTerm term) {
    JirBlock *b = jir_block_at(func, block);
    if (b) {
        b->term = term;
    }
}

int32_t jir_global_add(JirModule *module, const char *name, JogType *type, bool is_const) {
    if (!module || !name || !type || module->globals.count >= INT32_MAX) {
        return -1;
    }
    JirGlobal *global = ct_arena_alloc(module->arena, sizeof(*global), _Alignof(JirGlobal));
    if (!global) {
        return -1;
    }
    memset(global, 0, sizeof(*global));
    global->name = name;
    global->type = type;
    global->is_const = is_const;
    if (!ct_da_push(&module->globals, &global)) {
        return -1;
    }
    return (int32_t)(module->globals.count - 1);
}

JirGlobal *jir_global(const JirModule *module, uint32_t index) {
    if (!module || index >= module->globals.count) {
        return NULL;
    }
    return *(JirGlobal **)ct_da_at((CtDa *)&module->globals, index);
}

/* ---- operands ---- */

JirOperand jir_none(void) {
    JirOperand op;
    memset(&op, 0, sizeof(op));
    return op;
}

JirOperand jir_local(const JirFunc *func, uint32_t index) {
    JirOperand op = jir_none();
    op.kind = JIR_OPND_LOCAL;
    op.as.local = index;
    JirLocal *local = func ? jir_local_at(func, index) : NULL;
    op.type = local ? local->type : NULL;
    return op;
}

JirOperand jir_int(JogType *type, uint64_t bits) {
    JirOperand op = jir_none();
    op.kind = JIR_OPND_INT;
    op.type = type;
    op.as.bits = bits;
    return op;
}

JirOperand jir_float(JogType *type, double value) {
    JirOperand op = jir_none();
    op.kind = JIR_OPND_FLOAT;
    op.type = type;
    op.as.f = value;
    return op;
}

JirOperand jir_string(JogType *type, const char *ptr, size_t len) {
    JirOperand op = jir_none();
    op.kind = JIR_OPND_STRING;
    op.type = type;
    op.as.str.ptr = ptr;
    op.as.str.len = len;
    return op;
}

JirOperand jir_cstring(JogType *type, const char *ptr, size_t len) {
    JirOperand op = jir_string(type, ptr, len);
    op.kind = JIR_OPND_CSTRING;
    return op;
}

JirOperand jir_null(JogType *type) {
    JirOperand op = jir_none();
    op.kind = JIR_OPND_NULL;
    op.type = type;
    return op;
}

JirOperand jir_global_ref(const JirModule *module, uint32_t index) {
    JirOperand op = jir_none();
    op.kind = JIR_OPND_GLOBAL;
    op.as.global = index;
    JirGlobal *global = jir_global(module, index);
    if (global) {
        op.type = jog_type_ptr(module->types, global->type, global->is_const);
    }
    return op;
}

JirOperand jir_func_ref(const JirModule *module, uint32_t index) {
    JirOperand op = jir_none();
    op.kind = JIR_OPND_FUNC;
    op.as.func = index;
    JirFunc *func = jir_func(module, index);
    op.type = func ? func->type : NULL;
    return op;
}

/* ---- verification ---- */

typedef struct Verifier {
    const JirModule *module;
    CtDiag *diag;
    const JirFunc *func;
    uint32_t block;
    bool ok;
} Verifier;

static void fail(Verifier *v, JogPos pos, const char *what) {
    CtPos at = {pos.file, pos.line, pos.column};
    ct_diag_emit(v->diag, CT_SEVERITY_ERROR, at, "internal: invalid IR in %s, block %u: %s",
                 v->func ? v->func->jog_name : "?", (unsigned)v->block, what);
    v->ok = false;
}

static void check_operand(Verifier *v, const JirOperand *op, JogPos pos) {
    switch (op->kind) {
    case JIR_OPND_LOCAL:
        if (op->as.local >= v->func->locals.count) {
            fail(v, pos, "local index out of range");
        }
        break;
    case JIR_OPND_GLOBAL:
        if (op->as.global >= v->module->globals.count) {
            fail(v, pos, "global index out of range");
        }
        break;
    case JIR_OPND_FUNC:
        if (op->as.func >= v->module->funcs.count) {
            fail(v, pos, "function index out of range");
        }
        break;
    default:
        break;
    }
    if (op->kind != JIR_OPND_NONE && !op->type) {
        fail(v, pos, "operand without a type");
    }
}

static bool produces_value(JirOp op) {
    switch (op) {
    case JIR_STORE:
    case JIR_CALL:
    case JIR_BOUNDS_CHECK:
    case JIR_NULL_CHECK:
    case JIR_PANIC:
        return false;
    default:
        return true;
    }
}

static void check_target(Verifier *v, uint32_t target, JogPos pos) {
    if (target >= v->func->blocks.count) {
        fail(v, pos, "branch target out of range");
    }
}

bool jir_verify(const JirModule *module, CtDiag *diag) {
    if (!module) {
        return false;
    }
    Verifier v = {module, diag, NULL, 0, true};
    for (size_t f = 0; f < module->funcs.count; f++) {
        JirFunc *func = jir_func(module, (uint32_t)f);
        v.func = func;
        v.block = 0;
        if (func->linkage == JIR_LINK_EXTERN) {
            if (func->blocks.count != 0) {
                fail(&v, func->pos, "extern function has a body");
            }
            continue;
        }
        if (func->blocks.count == 0) {
            fail(&v, func->pos, "function has no blocks");
            continue;
        }
        for (size_t b = 0; b < func->blocks.count; b++) {
            JirBlock *block = jir_block_at(func, (uint32_t)b);
            v.block = (uint32_t)b;
            for (size_t i = 0; i < block->insts.count; i++) {
                JirInst *inst = ct_da_at(&block->insts, i);
                if (produces_value(inst->op) && inst->dst < 0) {
                    fail(&v, inst->pos, "instruction needs a destination");
                }
                if (inst->dst >= 0 && (uint32_t)inst->dst >= func->locals.count) {
                    fail(&v, inst->pos, "destination out of range");
                }
                check_operand(&v, &inst->a, inst->pos);
                check_operand(&v, &inst->b, inst->pos);
                for (size_t k = 0; k < inst->arg_count; k++) {
                    check_operand(&v, &inst->args[k], inst->pos);
                }
                if (inst->op == JIR_ADDR) {
                    if (inst->a.kind != JIR_OPND_LOCAL) {
                        fail(&v, inst->pos, "addr needs a local operand");
                    } else if (inst->a.as.local < func->locals.count) {
                        jir_local_at(func, inst->a.as.local)->address_taken = true;
                    }
                }
            }
            JirTerm *term = &block->term;
            switch (term->kind) {
            case JIR_TERM_NONE:
                fail(&v, term->pos, "block is not terminated");
                break;
            case JIR_TERM_BR:
                check_target(&v, term->target, term->pos);
                break;
            case JIR_TERM_COND_BR:
                check_operand(&v, &term->value, term->pos);
                check_target(&v, term->target, term->pos);
                check_target(&v, term->else_target, term->pos);
                break;
            case JIR_TERM_SWITCH:
                check_operand(&v, &term->value, term->pos);
                check_target(&v, term->target, term->pos);
                for (size_t k = 0; k < term->case_count; k++) {
                    check_target(&v, term->cases[k].target, term->pos);
                }
                break;
            case JIR_TERM_RET:
                check_operand(&v, &term->value, term->pos);
                break;
            case JIR_TERM_UNREACHABLE:
                break;
            }
        }
    }
    return v.ok;
}
