/*
 *  (C) 2010 by Computer System Laboratory, IIS, Academia Sinica, Taiwan.
 *      See COPYRIGHT in top-level directory.
 */

#include <stdlib.h>
#include "exec-all.h"
#include "tcg-op.h"
#include "helper.h"
#define GEN_HELPER 1
#include "helper.h"
#include "optimization.h"

extern uint8_t *optimization_ret_addr;

/*
 * Shadow Stack
 */
list_t *shadow_hash_list;

/*
 * shadow_hash_func()
 *  Modify from tb_jmp_cache_hash_func()
 */
static inline unsigned int shadow_hash_func(target_ulong guest_eip) {
    target_ulong tmp;
    tmp = guest_eip ^ (guest_eip >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (((tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK)
            | (tmp & TB_JMP_ADDR_MASK));
}

static inline void shack_init(CPUState *env)
{
    env->shack = (uint64_t *)malloc(SHACK_SIZE * sizeof(uint64_t));
    env->shack_top = env->shack;
    env->shack_end = env->shack + SHACK_SIZE;
    env->shadow_hash_list = calloc(4096, sizeof(struct shadow_pair));
    env->shadow_ret_count = 0;
    env->shadow_ret_addr = (unsigned long *)malloc(SHACK_SIZE * sizeof(unsigned long));
}

/*
 * shack_set_shadow()
 *  Insert a guest eip to host eip pair if it is not yet created.
 */
void shack_set_shadow(CPUState *env, target_ulong guest_eip, unsigned long *host_eip)
{
    /* find entry in hash table */
    struct shadow_pair *entry = (struct shadow_pair *)env->shadow_hash_list
                                + shadow_hash_func(guest_eip);
    /* iterate through the chain */
    while(entry) {
        if(entry->guest_eip == guest_eip) {
            /* fill in shadow pair */
            *entry->shadow_slot = (unsigned long)host_eip;
            return ;
        }
        entry = (struct shadow_pair *)entry->l.next;
    }
}

inline void insert_unresolved_eip(CPUState *env, target_ulong next_eip, unsigned long*host_eip)
{

}

/*
 * helper_shack_flush()
 *  Reset shadow stack.
 */
void helper_shack_flush(CPUState *env)
{
    printf("get %p\n", (void *)env);
    /* when to free the pairs? */
}

/*
 * push_shack()
 *  Push next guest eip into shadow stack.
 */
void push_shack(CPUState *env, TCGv_ptr cpu_env, target_ulong next_eip)
{
    tb_page_addr_t phys_pc = get_page_addr_code(env, next_eip);
    TranslationBlock *tb;
    for(tb = tb_phys_hash[tb_phys_hash_func(phys_pc)];; tb = tb->phys_hash_next) {
        if(!tb) {
            /* not yet translated, add an entry in hash table */
            list_t *head = (list_t *)((struct shadow_pair *)env->shadow_hash_list
                                                            + shadow_hash_func(next_eip));
            struct shadow_pair *p = (struct shadow_pair *)malloc(sizeof(struct shadow_pair));
            p->guest_eip = next_eip;
            /* shadow_slot point to the reserved host_eip space in shadow_ret_addr */
            /* why? can't we just store it in pair? */
            p->shadow_slot = env->shadow_ret_addr + env->shadow_ret_count;
            if(*(uintptr_t *)head) {
                /* perform chaining */
                p->l.next = head->next;
                p->l.prev = head;
                head->next->prev = (list_t *)p;
                head->next = (list_t *)p;
            } else {
                /* copy entry */
                *(struct shadow_pair *)head = *p;
                head->next = NULL;
                head->prev = NULL;
                free(p);
            }
            break;
        }
        else if(tb->pc == next_eip &&
                tb->page_addr[0] == (phys_pc & TARGET_PAGE_MASK)) {
            /* translated, push host_eip into shadow_ret_addr */
            env->shadow_ret_addr[env->shadow_ret_count] = (unsigned long)tb->tc_ptr;
            break;
        }
    }
    /*
     * we can only push the addr of host_eip
     * since it might not been translated here
     */
    /* Generate TCG codes */
    TCGv_ptr tcg_shack_top = tcg_temp_local_new_ptr();
    TCGv_ptr tcg_shack_end = tcg_temp_local_new_ptr();
    int l_shack_notfull = gen_new_label();

    // load shack
    tcg_gen_ld_ptr(tcg_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_ld_ptr(tcg_shack_end, cpu_env, offsetof(CPUState, shack_end));
    /* if stack is not full(top < end) */
    tcg_gen_brcond_ptr(TCG_COND_LT, tcg_shack_top, tcg_shack_end, l_shack_notfull);
        /* TODO: flush stack */
//        gen_helper_shack_flush(cpu_env);
    gen_set_label(l_shack_notfull);
    /* push source PC */
    tcg_gen_addi_ptr(tcg_shack_top, tcg_shack_top, sizeof(tcg_target_ulong) * 2);
    tcg_gen_st_ptr(tcg_const_ptr(next_eip), tcg_shack_top, -sizeof(tcg_target_ulong));
    /* push target PC address */
    tcg_gen_st_ptr(tcg_const_ptr((target_ulong)(env->shadow_ret_addr
                                + env->shadow_ret_count)), tcg_shack_top, -2 * sizeof(tcg_target_ulong));
    tcg_gen_st_ptr(tcg_shack_top, cpu_env, offsetof(CPUState, shack_top));

    tcg_temp_free_ptr(tcg_shack_end);
    tcg_temp_free_ptr(tcg_shack_top);

    env->shadow_ret_count++;
}

/*
 * pop_shack()
 *  Pop next host eip from shadow stack.
 */
void pop_shack(TCGv_ptr cpu_env, TCGv next_eip)
{
    TCGv_ptr tcg_shack_top = tcg_temp_local_new_ptr();
    TCGv guest_eip = tcg_temp_local_new();
    TCGv_ptr host_eip_p = tcg_temp_local_new();
    TCGv host_eip = tcg_temp_local_new();
    int l_end = gen_new_label();

    tcg_gen_ld_ptr(tcg_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_ld_tl(guest_eip, tcg_shack_top, -sizeof(tcg_target_ulong));
    /* if next addr is not same */
    tcg_gen_brcond_tl(TCG_COND_NE, guest_eip, next_eip, l_end);
    tcg_gen_ld_ptr(host_eip_p, tcg_shack_top, -2 * sizeof(tcg_target_ulong));
    tcg_gen_ld_tl(host_eip, host_eip_p, 0);
    /* Pop stack */
    tcg_gen_addi_ptr(tcg_shack_top, tcg_shack_top, -2 * sizeof(tcg_target_ulong));
    tcg_gen_st_ptr(tcg_shack_top, cpu_env, offsetof(CPUState, shack_top));
    tcg_gen_brcond_tl(TCG_COND_EQ, host_eip, tcg_const_tl(0), l_end);

    /* Jump to host_eip */
    *gen_opc_ptr++ = INDEX_op_jmp;
    *gen_opparam_ptr++ = GET_TCGV_PTR(host_eip);

    gen_set_label(l_end);
    tcg_temp_free_ptr(tcg_shack_top);
    tcg_temp_free_ptr(host_eip_p);
    tcg_temp_free(host_eip);
    tcg_temp_free(guest_eip);
}

/*
 * Indirect Branch Target Cache
 */
__thread int update_ibtc;

/*
 * helper_lookup_ibtc()
 *  Look up IBTC. Return next host eip if cache hit or
 *  back-to-dispatcher stub address if cache miss.
 */
void *helper_lookup_ibtc(target_ulong guest_eip)
{
    return optimization_ret_addr;
}

/*
 * update_ibtc_entry()
 *  Populate eip and tb pair in IBTC entry.
 */
void update_ibtc_entry(TranslationBlock *tb)
{
}

/*
 * ibtc_init()
 *  Create and initialize indirect branch target cache.
 */
static inline void ibtc_init(CPUState *env)
{
}

/*
 * init_optimizations()
 *  Initialize optimization subsystem.
 */
int init_optimizations(CPUState *env)
{
    shack_init(env);
    ibtc_init(env);

    return 0;
}

/*
 * vim: ts=8 sts=4 sw=4 expandtab
 */
