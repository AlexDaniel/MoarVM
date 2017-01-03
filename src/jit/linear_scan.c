#include "moar.h"
#include "internal.h"

#define __COMMA__ ,
static MVMint8 available_gpr[] = {
    MVM_JIT_ARCH_AVAILABLE_GPR(MVM_JIT_REG)
};
static MVMint8 available_num[] = {
    MVM_JIT_ARCH_NUM(MVM_JIT_REG)
};
/* bitmap, so make it '|' to combine the shifted register numbers */
#undef __COMMA__
#define __COMMA__ |
#define SHIFT(x) (1 << (MVM_JIT_REG(x)))
static const MVMint64 NVR_GPR_BITMAP = MVM_JIT_ARCH_NONVOLATILE_GPR(SHIFT);
#undef SHIFT
#undef __COMMA__


#define MAX_ACTIVE sizeof(available_gpr)
#define NYI(x) MVM_oops(tc, #x  "not yet implemented")

typedef struct {
    MVMint32 key;
    MVMint32 idx;
} UnionFind;


typedef struct ValueRef ValueRef;
struct ValueRef {
    MVMint32  tile_idx;
    MVMint32  value_idx;
    ValueRef *next;
};

static inline MVMint32 is_def(ValueRef *v) {
    return (v->value_idx == 0);
}

typedef struct {
    /* double-ended queue of value refs */
    ValueRef *first, *last;

    /* We can have at most two synthetic tiles, one attached to the first
     * definition and one to the last use... we could also point directly into
     * the values array of the tile, but it is not directly necessary */
    MVMint32    synth_pos[2];
    MVMJitTile *synthetic[2];

    MVMint8            register_spec;
    MVMJitStorageClass reg_cls;
    MVMint32           reg_num;
} LiveRange;

/* quick accessors for common checks */
static inline MVMint32 first_ref(LiveRange *r) {
    MVMint32 a = r->first == NULL        ? INT32_MAX : r->first->tile_idx;
    MVMint32 b = r->synthetic[0] == NULL ? INT32_MAX : r->synth_pos[0];
    return MIN(a,b);
}

static inline MVMint32 last_ref(LiveRange *r) {
    MVMint32 a = r->last == NULL         ? -1 : r->last->tile_idx;
    MVMint32 b = r->synthetic[1] == NULL ? -1 : r->synth_pos[1];
    return MAX(a,b);
}



typedef struct {
    /* Sets of values */
    UnionFind *sets;

    /* single buffer for uses, definitions */
    ValueRef *refs;
    MVMint32  refs_num;

    /* All values ever defined by the register allcoator */
    MVM_VECTOR_DECL(LiveRange, values);

    /* 'Currently' active values */
    MVMint32 active_top;
    MVMint32 active[MAX_ACTIVE];

    /* Values still left to do (heap) */
    MVM_VECTOR_DECL(MVMint32, worklist);
    /* Retired values (to be assigned registers) (heap) */
    MVM_VECTOR_DECL(MVMint32, retired);

    /* Register handout ring */
    MVMint8   reg_ring[MAX_ACTIVE];
    MVMint32  reg_give, reg_take;

    MVMint32 spill_top;
} RegisterAllocator;


UnionFind * value_set_find(UnionFind *sets, MVMint32 key) {
    while (sets[key].key != key) {
        key = sets[key].key;
    }
    return sets + key;
}


MVMint32 value_set_union(UnionFind *sets, LiveRange *values, MVMint32 a, MVMint32 b) {
    LiveRange *ra = values + sets[a].idx, *rb = values + sets[b].idx;
    ValueRef *head, *tail;
    if (first_ref(rb) < first_ref(ra)) {
        MVMint32 t = a; a = b; b = t;
    }
    sets[b].key = a; /* point b to a */
    /* merge value ref sets */
    if (first_ref(ra) <= first_ref(rb)) {
        head = ra->first;
        ra->first = ra->first->next;
    } else {
        head = rb->first;
        rb->first = rb->first->next;
    }
    tail = head;
    while (ra->first != NULL && rb->first != NULL) {
        if (ra->first->tile_idx <= rb->first->tile_idx) {
            tail->next  = ra->first;
            ra->first   = ra->first->next;
        } else {
            tail->next  = rb->first;
            rb->first   = rb->first->next;
        }
    }
    while (ra->first != NULL) {
        tail->next = ra->first;
        ra->first  = ra->first->next;
    }
    while (rb->first != NULL) {
        tail->next  = rb->first;
        rb->first   = rb->first->next;
    }
    values[sets[a].idx].first = head;
    values[sets[a].idx].last  = tail;
    return a;
}


/* create a new live range object and return a reference */
MVMint32 live_range_init(RegisterAllocator *alc) {
    LiveRange *range;
    MVMint32 idx = alc->values_num++;
    MVM_VECTOR_ENSURE_SIZE(alc->values, idx);
    range = &alc->values[idx];
    range->first        = NULL;
    range->last         = NULL;
    range->synthetic[0] = NULL;
    range->synthetic[1] = NULL;
    return idx;
}

/* append ref to end of queue */
static void live_range_add_ref(RegisterAllocator *alc, LiveRange *range, MVMint32 tile_idx, MVMint32 value_idx) {
    ValueRef *ref = alc->refs + alc->refs_num++;
    ref->tile_idx  = tile_idx;
    ref->value_idx = value_idx;

    if (range->first == NULL) {
        range->first = ref;
    }
    if (range->last != NULL) {
        range->last->next = ref;
    }
    range->last = ref;
    ref->next   = NULL;
}

static inline MVMint32 live_range_is_empty(LiveRange *range) {
    return (range->first == NULL &&
            range->synthetic[0] == NULL &&
            range->synthetic[1] == NULL);
}

static inline void heap_swap(MVMint32 *heap, MVMint32 a, MVMint32 b) {
    MVMint32 t = heap[a];
    heap[a]    = heap[b];
    heap[b]    = t;
}

/* Functions to maintain a heap of references to the live ranges */
void live_range_heap_down(LiveRange *values, MVMint32 *heap, MVMint32 top, MVMint32 item) {
    while (item < top) {
        MVMint32 left = item * 2 + 1;
        MVMint32 right = left + 1;
        MVMint32 swap;
        if (right < top) {
            swap = first_ref(&values[heap[left]]) < first_ref(&values[heap[right]]) ? left : right;
        } else if (left < top) {
            swap = left;
        } else {
            break;
        }
        if (first_ref(&values[heap[swap]]) < first_ref(&values[heap[item]])) {
            heap_swap(heap, swap, item);
            item       = swap;
        } else {
            break;
        }
    }
}

void live_range_heap_up(LiveRange *values, MVMint32 *heap, MVMint32 item) {
    while (item > 0) {
        MVMint32 parent = (item-1)/2;
        if (first_ref(&values[heap[parent]]) < first_ref(&values[heap[item]])) {
            heap_swap(heap, item, parent);
            item = parent;
        } else {
            break;
        }
    }
}

MVMint32 live_range_heap_pop(LiveRange *values, MVMint32 *heap, size_t *top) {
    MVMint32 v = heap[0];
    MVMint32 t = --(*top);
    /* pop by swap and heap-down */
    heap[0]    = heap[t];
    live_range_heap_down(values, heap, t, 0);
    return v;
}

void live_range_heap_push(LiveRange *values, MVMint32 *heap, MVMint32 *top, MVMint32 v) {
    /* NB, caller should use MVM_ENSURE_SPACE prior to calling */
    MVMint32 t = (*top)++;
    heap[t] = v;
    live_range_heap_up(values, heap, t);
}

void live_range_heapify(LiveRange *values, MVMint32 *heap, MVMint32 top) {
    MVMint32 i = top, mid = top/2;
    while (i-- > mid) {
        live_range_heap_up(values, heap, i);
    }
}


/* register assignment logic */
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define NEXT_IN_RING(a,x) (((x)+1) == ARRAY_SIZE(a) ? 0 : ((x)+1))
MVMint8 get_register(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitStorageClass reg_cls) {
    /* ignore storage class for now */
    MVMint8 reg_num;
    reg_num       = alc->reg_ring[alc->reg_take];
    if (reg_num >= 0) {
        /* not empty */
        alc->reg_ring[alc->reg_take] = -1; /* mark used */
        alc->reg_take = NEXT_IN_RING(alc->reg_ring, alc->reg_take);
    }
    return reg_num;
}

void free_register(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitStorageClass reg_cls, MVMint8 reg_num) {
    if (alc->reg_give == alc->reg_take) {
        MVM_oops(tc, "Trying to release more registers than fit into the ring");
    }
    alc->reg_ring[alc->reg_give] = reg_num;
    alc->reg_give = NEXT_IN_RING(alc->reg_ring, alc->reg_give);
}

void assign_register(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list,
                     MVMint32 lv, MVMJitStorageClass reg_cls,  MVMint8 reg_num) {
    /* What to do here:
     * - update tiles using this live range to refer to this register
     * - update allocator to mark this register as used by this live range */
    LiveRange *range = alc->values + lv;
    ValueRef *ref;
    MVMint32 i;

    range->reg_cls   = reg_cls;
    range->reg_num   = reg_num;
    for (ref = range->first; ref != NULL; ref = ref->next) {
        MVMJitTile *tile = list->items[ref->tile_idx];
        tile->values[ref->value_idx] = reg_num;
    }

    for (i = 0; i < 2; i++) {
        MVMJitTile *tile = range->synthetic[i];
        if (tile != NULL) {
            tile->values[i] = reg_num;
        }
    }
}


static void determine_live_ranges(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list) {
    MVMJitExprTree *tree = list->tree;
    MVMint32 i, j;

    alc->sets = MVM_calloc(tree->nodes_num, sizeof(UnionFind));
    /* TODO: add count for ARGLIST refs, which can be > 3 per 'tile' */
    alc->refs = MVM_calloc(list->items_num * 4, sizeof(ValueRef));

    MVM_VECTOR_INIT(alc->values,   list->items_num);
    MVM_VECTOR_INIT(alc->worklist, list->items_num);

    for (i = 0; i < list->items_num; i++) {
        MVMJitTile *tile = list->items[i];
        MVMint32    node = tile->node;
        /* Each of the following counts as either an alias or as a PHI (in case
         * of IF), and thus these are not actual definitions */
        if (tile->op == MVM_JIT_COPY) {
            MVMint32 ref        = tree->nodes[tile->node + 1];
            alc->sets[node].key = ref; /* point directly to actual definition */
        } else if (tile->op == MVM_JIT_DO && MVM_JIT_TILE_YIELDS_VALUE(tile)) {
            MVMint32 nchild     = tree->nodes[tile->node + 1];
            MVMint32 ref        = tree->nodes[tile->node + nchild];
            alc->sets[node].key = ref;
        } else if (tile->op == MVM_JIT_IF) {
            MVMint32 left_cond   = tree->nodes[tile->node + 2];
            MVMint32 right_cond  = tree->nodes[tile->node + 3];
            /* NB; this may cause a conflict, in which case we can resolve it by
             * creating a new live range or inserting a copy */
            alc->sets[node].key  = value_set_union(alc->sets, alc->values, left_cond, right_cond);
        } else {
            /* create a live range if necessary */
            if (MVM_JIT_TILE_YIELDS_VALUE(tile)) {
                MVMint8 register_spec = MVM_JIT_REGISTER_FETCH(tile->register_spec, 0);
                MVMint32 idx          = live_range_init(alc);
                alc->sets[node].key   = node;
                alc->sets[node].idx   = idx;
                live_range_add_ref(alc, alc->values + idx, i, 0);
                if (MVM_JIT_REGISTER_HAS_REQUIREMENT(register_spec)) {
                    alc->values[idx].register_spec = register_spec;
                }
            }

            /* account for uses */
            for (j = 0; j < tile->num_refs; j++) {
                MVMint8  register_spec = MVM_JIT_REGISTER_FETCH(tile->register_spec, j+1);
                if (MVM_JIT_REGISTER_HAS_REQUIREMENT(register_spec)) {
                    /* TODO - this may require resolving conflicting register
                     * specifications */
                    NYI(use_register_spec);
                }
                if (MVM_JIT_REGISTER_IS_USED(register_spec)) {
                    MVMint32 idx = value_set_find(alc->sets, tile->refs[j])->idx;
                    live_range_add_ref(alc, alc->values + idx, i, j + 1);
                }
            }
        }
    }
}

/* The code below needs some thinking... */
static void active_set_add(MVMThreadContext *tc, RegisterAllocator *alc, MVMint32 a) {
    /* the original linear-scan heuristic for spilling is to take the last value
     * in the set to expire, freeeing up the largest extent of code... that is a
     * reasonably good heuristic, albeit not essential to the concept of linear
     * scan. It makes sense to keep the stack ordered at all times (simplest by
     * use of insertion sort). Although insertion sort is O(n^2), n is never
     * large in this case (32 for RISC architectures, maybe, if we ever support
     * them; 7 for x86-64. So the time spent on insertion sort is always small
     * and bounded by a constant, hence O(1). Yes, algorithmics works this way
     * :-) */
    MVMint32 i;
    for (i = 0; i < alc->active_top; i++) {
        MVMint32 b = alc->active[i];
        if (last_ref(&alc->values[b]) > last_ref(&alc->values[a])) {
            /* insert a before b */
            memmove(alc->active + i + 1, alc->active + i, sizeof(MVMint32)*(alc->active_top - i));
            alc->active[i] = b;
            alc->active_top++;
            return;
        }
    }
    /* append at the end */
    alc->active[alc->active_top++] = a;
}



/* Take live ranges from active_set whose last use was after position and append them to the retired list */
static void active_set_expire(MVMThreadContext *tc, RegisterAllocator *alc, MVMint32 position) {
    MVMint32 i;
    for (i = 0; i < alc->active_top; i++) {
        MVMint32 v = alc->active[i];
        MVMint8 reg_num = alc->values[v].reg_num;
        if (last_ref(&alc->values[v]) > position) {
            break;
        } else {
            free_register(tc, alc, MVM_JIT_STORAGE_GPR, reg_num);
        }
    }

    /* shift off the first x values from the live set. */
    if (i > 0) {
        MVM_VECTOR_APPEND(alc->retired, alc->active, i);
        MVM_VECTOR_SHIFT(alc->active, i);
    }
}


static void spill_register(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list, MVMint32 position) {
    /* Spilling involves the following:
       - choosing a live range from the active set to spill
       - finding a place where to spill it
       - choosing whether to split this live range in a pre-spill and post-spill part
          - potentially spill only part of it
       - for each definition (in the spilled range),
          - make a new live range that
          - reuses the use and def pointer for the definition
          - insert a store just after the defintion
          - and if it lies in the future, put it on worklist, if it lies in the past, put it on the retired list
          - and update the definition to point to the newly created live range
       - for each use (in the spilled range)
          - make a new live range that reuses the use and def pointer for the use
          - insert a load just before the use
          - if it lies in the future, put it on the worklist, if it lies in the past, put it on the retired list
          - update the using tile to point to the newly created live range
       - remove it from the active set
    */
    MVM_oops(tc, "spill_register NYI");
}

static void spill_live_range(MVMThreadContext *tc, RegisterAllocator *alc, MVMint32 which) {
}

static void split_live_range(MVMThreadContext *tc, RegisterAllocator *alc, MVMint32 which, MVMint32 from, MVMint32 to) {
}


/* not sure if this is sufficiently general-purpose and unconfusing */
#define MVM_VECTOR_ASSIGN(a,b) do {             \
        a = b;                                  \
        a ## _top = b ## _top;                  \
        a ## _alloc = b ## _alloc;              \
    } while (0);


static void linear_scan(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list) {
    MVMint32 i, j;
    while (alc->worklist_num > 0) {
        MVMint32 v   = live_range_heap_pop(alc->values, alc->worklist, &alc->worklist_num);
        MVMint32 pos = first_ref(alc->values + v);
        MVMint8 reg;
        /* NB: should i wrap this in a separate loop to remove these? */
        if (live_range_is_empty(alc->values + v))
            continue;
        /* assign registers in loop */
        active_set_expire(tc, alc, pos);
        if (MVM_JIT_REGISTER_HAS_REQUIREMENT(alc->values[v].register_spec)) {
            reg = MVM_JIT_REGISTER_REQUIREMENT(alc->values[v].register_spec);
            if (NVR_GPR_BITMAP & (1 << reg)) {
                assign_register(tc, alc, list, v, MVM_JIT_STORAGE_NVR, reg);
            } else {
                /* TODO; might require swapping / spilling */
                NYI(general_purpose_register_spec);
            }
        } else {
            while ((reg = get_register(tc, alc, MVM_JIT_STORAGE_GPR)) < 0) {
                spill_register(tc, alc, list, pos);
            }
            assign_register(tc, alc, list, v, MVM_JIT_STORAGE_GPR, reg);
            active_set_add(tc, alc, v);
        }
    }
    /* flush active live ranges */
    active_set_expire(tc, alc, list->items_num + 1);
}


void MVM_jit_linear_scan_allocate(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitTileList *list) {
    RegisterAllocator alc;
    /* initialize allocator */

    alc.active_top = 0;
    memset(alc.active, -1, sizeof(alc.active));

    alc.reg_give = alc.reg_take = 0;
    memcpy(alc.reg_ring, available_gpr,
           sizeof(available_gpr));

    alc.spill_top = 0;

    /* run algorithm */
    determine_live_ranges(tc, &alc, list);
    linear_scan(tc, &alc, list);

    /* deinitialize allocator */
    MVM_free(alc.sets);
    MVM_free(alc.refs);
    MVM_free(alc.worklist);
    MVM_free(alc.retired);

    /* make edits effective */
    MVM_jit_tile_list_edit(tc, list);

}