#include "moar.h"
#include "internal.h"

#if MVM_JIT_ARCH == MVM_JIT_ARCH_X64
static MVMint8 free_gpr[] = {
    X64_FREE_GPR(MVM_JIT_REGNAME)
};
static MVMint8 free_num[] = {
    X64_SSE(MVM_JIT_REGNAME)
};
#else
static MVMint8 free_gpr[] = { -1 };
static MVMint8 free_num[] = { -1 };
#endif

#define NUM_GPR sizeof(free_gpr)
#define NEXT_REG(x) (((x)+1)%NUM_GPR)

/* UNUSED - Register lock bitmap macros */
#define REGISTER_IS_LOCKED(a, n) ((a)->reg_lock &  (1 << (n)))
#define LOCK_REGISTER(a, n)   ((a)->reg_lock |=  (1 << (n)))
#define UNLOCK_REGISTER(a,n)  ((a)->reg_lock &= ~(1 << (n)))

#ifndef MAX
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#endif

/* Live range of nodes */
struct LiveRange {
    MVMint32 range_start, range_end;
    MVMint32 num_use;
    /* Index into the buffer */
    MVMint32 buf_idx;
};


struct RegisterAllocator {
    /* Live range of nodes */
    struct LiveRange *live_ranges;
    /* Nodes refered to by tiles */
    MVM_DYNAR_DECL(MVMJitExprNode, tile_nodes);

    /* Lookup tables */
    MVMJitValueDescriptor **values_by_node;
    MVMJitValueDescriptor  *values_by_register[MVM_JIT_MAX_GPR];

    MVMJitCompiler *compiler;

    /* Register giveout ring */
    MVMint8 free_reg[NUM_GPR];
    MVMint32 reg_give, reg_take;

    /* Last use of each register */
    MVMint32 last_use[MVM_JIT_MAX_GPR];
};


void MVM_jit_register_allocator_init(MVMThreadContext *tc, struct RegisterAllocator *allocator,
                                     MVMJitCompiler *compiler, MVMJitTileList *list) {
    /* Store live ranges */
    MVM_DYNAR_INIT(allocator->tile_nodes, NUM_GPR);

    /* Initialize usable register ring */
    memcpy(allocator->free_reg, free_gpr, NUM_GPR);
    allocator->reg_give  = 0;
    allocator->reg_take  = 0;

    /* last use table */
    memset(allocator->last_use, -1, sizeof(allocator->last_use));

    /* create lookup tables */
    allocator->values_by_node = MVM_calloc(list->tree->nodes_num, sizeof(void*));
    memset(allocator->values_by_register, 0, sizeof(allocator->values_by_register));
    allocator->live_ranges    = MVM_calloc(list->tree->nodes_num, sizeof(struct LiveRange));

    allocator->compiler       = compiler;
}

void MVM_jit_register_allocator_deinit(MVMThreadContext *tc, struct RegisterAllocator *allocator) {
    MVM_free(allocator->values_by_node);
    MVM_free(allocator->live_ranges);
    MVM_free(allocator->tile_nodes);
}

#define NYI(x) MVM_oops(tc, #x " NYI");


static MVMint8 alloc_register(MVMThreadContext *tc, struct RegisterAllocator *allocator, MVMJitStorageClass reg_cls) {
    MVMint8 reg_num;
    if (reg_cls == MVM_JIT_STORAGE_FPR) {
        NYI(numeric_regs);
    } else {
        if (NEXT_REG(allocator->reg_take) == allocator->reg_give) {
            /* Out of registers, spill something */
            NYI(spill_something);
        }
        /* Use a circular handout scheme for the 'fair use' of registers */
        reg_num       = allocator->free_reg[allocator->reg_take];
        /* mark it for debugging purposes */
        allocator->free_reg[allocator->reg_take] = 0xff;
        allocator->reg_take = NEXT_REG(allocator->reg_take);
    }
    /* MVM_jit_log(tc, "Allocated register %d at order nr %d\n", reg_num, cl->order_nr); */
    return reg_num;
}

/* Freeing a register makes it available again */
static void free_register(MVMThreadContext *tc, struct RegisterAllocator *allocator, MVMJitStorageClass reg_cls, MVMint8 reg_num) {
    if (reg_cls == MVM_JIT_STORAGE_FPR) {
        NYI(numeric_regs);
    } else {
        MVMint32 i;
        for (i = 0; i < sizeof(free_gpr); i++) {
            if (free_gpr[i] == reg_num)
                goto ok;
        }
        MVM_oops(tc, "This is not a free register!");
    ok:
        if (allocator->reg_give == allocator->reg_take) {
            MVM_oops(tc, "Trying to free too many registers");
        }
        allocator->free_reg[allocator->reg_give] = reg_num;
        allocator->reg_give                      = NEXT_REG(allocator->reg_give);
        allocator->values_by_register[reg_num]   = NULL;
    }
}

static void assign_value(MVMThreadContext *tc, struct RegisterAllocator *allocator, MVMint32 node, MVMJitStorageClass st_cls, MVMint16 st_pos) {
    MVMJitValueDescriptor *value = MVM_spesh_alloc(tc, allocator->compiler->graph->sg, sizeof(MVMJitValueDescriptor));
    value->node = node;
    value->st_cls = st_cls;
    value->st_pos = st_pos;
    value->range_start  = allocator->live_ranges[node].range_start;
    value->range_end    = allocator->live_ranges[node].range_end;
    value->next_by_node = allocator->values_by_node[node];
    if (st_cls == MVM_JIT_STORAGE_GPR) {
        value->next_by_position     = allocator->values_by_register[st_pos];
        allocator->last_use[st_pos] = MAX(allocator->last_use[st_pos], value->range_end);
        allocator->values_by_register[st_pos] = value;
    }
    allocator->values_by_node[node] = value;
}

static void expire_values(MVMThreadContext *tc, struct RegisterAllocator *allocator, MVMint32 order_nr) {
    MVMint32 i;
    for (i = 0; i < NUM_GPR; i++) {
        MVMint8 reg_num = free_gpr[i];
        if (allocator->last_use[reg_num] == order_nr) {
            free_register(tc, allocator, MVM_JIT_STORAGE_GPR, reg_num);
        }
    }

}

/* Get nodes and arguments refered to by a tile */
static void get_tile_nodes(MVMThreadContext *tc, struct RegisterAllocator *allocator,
                           MVMJitTile *tile, MVMJitExprTree *tree) {
    MVMJitExprNode node = tile->node;
    MVMJitExprNode buffer[16];
    const MVMJitTileTemplate *template = tile->template;

    /* Assign tile arguments and compute the refering nodes */
    switch (tree->nodes[node]) {
    case MVM_JIT_IF:
    {
        buffer[0] = tree->nodes[node+2];
        buffer[1] = tree->nodes[node+3];
        tile->num_values = 2;
        break;
    }
    case MVM_JIT_ARGLIST:
    {
        /* NB, arglist can conceivably use more than 7 values, although it can
         * safely overflow into args, we may want to find a better solution */
        MVMint32 i;
        tile->num_values = tree->nodes[node+1];
        for (i = 0; i < tile->num_values; i++) {
            MVMint32 carg = tree->nodes[node+2+i];
            buffer[i]     = tree->nodes[carg+1];
        }
        break;
    }
    case MVM_JIT_DO:
    {
        MVMint32 nchild  = tree->nodes[node+1];
        buffer[0]        = tree->nodes[node+1+nchild];
        tile->num_values = 1;
        break;
    }
    default:
    {
        MVMint32 i, j, k, num_nodes, value_bitmap;
        num_nodes        = MVM_jit_expr_tree_get_nodes(tc, tree, node, tile->template->path, buffer);
        value_bitmap     = tile->template->value_bitmap;
        tile->num_values = template->num_values;
        j = 0;
        k = 0;
        /* splice out args from node refs */
        for (i = 0; i < num_nodes; i++) {
            if (value_bitmap & 1) {
                buffer[j++]     = buffer[i];
            } else {
                tile->args[k++] = buffer[i];
            }
            value_bitmap >>= 1;
        }
        break;
    }
    }
    /* copy to tile-node buffer */
    MVM_DYNAR_APPEND(allocator->tile_nodes, buffer, tile->num_values);
}



void MVM_jit_register_allocate(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitTileList *list) {
    struct RegisterAllocator allocator;
    MVMJitExprTree *tree = list->tree;
    MVMJitValueDescriptor *value;
    MVMint32 i, j;
    MVMint8 reg;

    /* Allocate tables used in register */
    MVM_jit_register_allocator_init(tc, &allocator, compiler, list);

    /* Compute live ranges for each node */
    for (i = 0; i < list->items_num; i++) {
        MVMint32 buf_idx = allocator.tile_nodes_num;
        MVMJitTile *tile = list->items[i];
        if (tile->template == NULL) /* pseudotiles */
            continue;
        allocator.live_ranges[tile->node].range_start = i;
        allocator.live_ranges[tile->node].range_end   = i;
        allocator.live_ranges[tile->node].buf_idx     = buf_idx;
        get_tile_nodes(tc, &allocator, tile, tree);
        for (j = 0; j < tile->num_values; j++) {
            MVMint32 ref_node = allocator.tile_nodes[buf_idx + j];
            allocator.live_ranges[ref_node].range_end = i;
            allocator.live_ranges[ref_node].num_use++;
        }
    }

    /* Assign registers */
    for (i = 0; i < list->items_num; i++) {
        MVMJitTile *tile = list->items[i];
        MVMint32 buf_idx = allocator.live_ranges[tile->node].buf_idx;
        if (tile->template == NULL)
            continue;
        /* TODO; ensure that register values are live */
        for (j = 0; j < tile->num_values; j++) {
            MVMint32 ref_node = allocator.tile_nodes[buf_idx + j];
            tile->values[j+1] = allocator.values_by_node[ref_node];
        }

        /* allocate input register if necessary */
        switch(tree->nodes[tile->node]) {
        case MVM_JIT_COPY:
        {
            /* use same register as input  */
            MVMint32 ref_node = allocator.tile_nodes[buf_idx];
            value             = allocator.values_by_node[ref_node];
            assign_value(tc, &allocator, tile->node, value->st_cls, value->st_pos);
            break;
        }
        case MVM_JIT_TC:
            /* TODO, this isn't really portable, we should have register
             * attributes assigned to the tile itself */
            assign_value(tc, &allocator, tile->node, MVM_JIT_STORAGE_NVR, MVM_JIT_REG_TC);
            break;
        case MVM_JIT_CU:
            assign_value(tc, &allocator, tile->node, MVM_JIT_STORAGE_NVR, MVM_JIT_REG_CU);
            break;
        case MVM_JIT_LOCAL:
            assign_value(tc, &allocator, tile->node, MVM_JIT_STORAGE_NVR, MVM_JIT_REG_LOCAL);
            break;
        case MVM_JIT_STACK:
            assign_value(tc, &allocator, tile->node, MVM_JIT_STORAGE_NVR, MVM_JIT_REG_STACK);
            break;
        default:
            if (tile->template->vtype == MVM_JIT_REG) {
                /* allocate a register for the result */
                if (tile->num_values > 0 &&
                    tile->values[1]->st_cls == MVM_JIT_STORAGE_GPR &&
                    tile->values[1]->range_end == i) {
                    /* First register expires immediately, therefore we can safely cross-assign */
                    assign_value(tc, &allocator, tile->node, tile->values[1]->st_cls, tile->values[1]->st_pos);
                } else {
                    reg = alloc_register(tc, &allocator, MVM_JIT_STORAGE_GPR);
                    assign_value(tc, &allocator, tile->node, MVM_JIT_STORAGE_GPR, reg);
                }
            }
            break;
        }
        tile->values[0] = allocator.values_by_node[tile->node];
        if (tile->values[0] != NULL) {
            tile->values[0]->size = tree->info[tile->node].size;
        }
        expire_values(tc, &allocator, i);
    }
    MVM_jit_register_allocator_deinit(tc, &allocator);
}
