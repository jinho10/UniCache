/******************************************************************************
 * mcd.h
 *
 * Memcached
 *
 * Copyright (c) 2012, Jinho Hwang, The George Washington University
 */

#ifndef __MCD_H__
#define __MCD_H__

#include <xen/config.h>
#include <xen/mm.h> /* heap alloc/free */
#include <xen/xmalloc.h> /* xmalloc/xfree */
#include <xen/sched.h>  /* struct domain */
#include <xen/guest_access.h> /* copy_from_guest */
#include <xen/hash.h> /* hash_long */
#include <xen/domain_page.h> /* __map_domain_page */
#include <public/mcd.h>

#define EXPORT // indicates code other modules are dependent upon
#define FORWARD
#define LOCAL   static

/*
 * Need to Synchronize with Linux Counterpart
 * Linux Error: 1000 - 1999
 * Xen Error: 2000 - 2999
 */
#define ERR_NO          -1
#define ERR_NOEXIST     2000
#define ERR_PARAM       2001
#define ERR_XENFAULT    2002
#define ERR_MCDFAULT    2003
#define ERR_NOMCD       2004
#define ERR_NOTMEM      2005

#define RET_GET_MEM     3000
#define RET_GET_SSD     3001

#ifndef MEM_ALIGN
#define MEM_ALIGN (sizeof(void *) * 2)
#endif

// If there is not get op for MCD_EXP_TIME, we will remove the entry
#define MCD_EXP_TIME 500 //sec

#define mspin_lock_init(l) { spin_lock_init(l);\
        /*(printk("lock_init %s (%d) \n", __FUNCTION__, __LINE__);*/ }

#define mspin_lock(l) { spin_lock(l);\
        /*printk("locked %s (%d) \n", __FUNCTION__, __LINE__);*/ }

#define mspin_unlock(l) { spin_unlock(l);\
        /*printk("unlocked %s (%d) \n", __FUNCTION__, __LINE__);*/ }

#define TO_MB(_a)       ((_a) / 1000000)
#define TOT_USED_MB()   ((mcd_data.pri_nr_bytes + mcd_data.shr_nr_bytes)/1000000)
#define TOT_USED_BYTES()   ((mcd_data.pri_nr_bytes + mcd_data.shr_nr_bytes))
#define TOT_USED_KEYS()   ((mcd_data.pri_nr_keys + mcd_data.shr_nr_keys))

#define mcd_dom_is_dying(p) ((p)->domain->is_dying)
#define domain_is_dying(p) ((p)->is_dying)
#define MCD_DOM_ID_NULL ((domid_t)((domid_t)-1L))

/*
 * Domain Management
 */
typedef struct mcd_hashT {
    struct list_head hash_list;
#define MAGIC 989898
    uint32_t magic; // for safety... 

    spinlock_t lock;

    domid_t  domain;
#define MCD_HASHT_NORMAL        0
#define MCD_HASHT_MEASUREMENT   1
    uint16_t flags;
#define MCD_MINF_ERROR      (1 << 0)

    uint32_t nr_pages;
    uint32_t nr_bytes;
    uint32_t ref;
    s_time_t put_time;
    uint32_t query_time_ms; // ignore zero

    uint32_t hash;
    uint32_t key_size;
    uint32_t val_size;
    char*    key_val;
} mcd_hashT_t;

typedef struct mcdctl_hashT {
    struct list_head hash_list;
    struct mcdctl_hashT *next;
    domid_t  domain;
    uint16_t flags; /* TBD */
    uint32_t hash;
    uint32_t key_size;
    uint32_t val_size;
} mcdctl_hashT_t;

typedef struct mcd_domain {
    struct list_head mcd_list;
    struct list_head mcd_hash_list_head;
    struct list_head mcdctl_hash_list_head; /* mcdctl - outsourced */

    spinlock_t      lock;
    spinlock_t      mcdctl_lock;

#define mcd_domid(m)    (m)->domid
    domid_t         domid;
#define domptr(m)   (m)->domain
    struct domain   *domain;

    /* In-memory data */
    uint32_t        nr_avail; // can be used, but we need policy
    uint32_t        nr_keys;
    uint32_t        nr_used_pages;
    uint64_t        nr_used_bytes;

    /* TODO Out-memory data */
    //uint32_t        mcdctl_nr_used_pages;
    //uint64_t        mcdctl_nr_used_bytes;

#define DOM_WEIGHT_DEFAULT  100
    unsigned int    weight; // default 100
    unsigned int    ratio; // 0 - 100 in total
    unsigned int    curr; // 0 - (over 100 is ok)

    // adaptive-dynamic partitioning
    s_time_t        reset;
#define HIT(_d) _d->get_succ++
    uint32_t        get_succ;
#define MISS(_d) _d->get_fail++
    uint32_t        get_fail;
#define HITRATE(_d) _d->hitrate
#define MISSRATE(_d) (100 - _d->hitrate)
#define SET_HITRATE(_d) _d->hitrate = (_d->get_succ * 100)/(_d->get_succ + _d->get_fail)
#define RESET_HITRATE(_d) { _d->hitrate = 0; _d->get_succ = 0; _d->get_fail = 0; }
    uint32_t        hitrate; // hitrate = get_succ * 100 / (get_succ + get_fail)

    uint32_t        rec_cost_avg; // average recovery cost
} mcd_domain_t;

/* 
 * Main Structure for mcd
 */
typedef struct g_mcd_data {
    spinlock_t lock;

    /* In-memory data */
    //uint32_t tot_nr_pages;
    uint32_t pri_nr_pages;
    uint32_t shr_nr_pages;
    // NB. tot_nr_pages = pri_nr_pages + shr_nr_pages;

    uint64_t pri_nr_bytes;
    uint64_t shr_nr_bytes;
    // NB. tot_nr_bytes = pri_nr_bytes + shr_nr_bytes;

    uint32_t pri_nr_keys;
    uint32_t shr_nr_keys;

    /* TODO Out-memory data */
    //uint64_t mcdctl_pri_nr_bytes;
    //uint64_t mcdctl_shr_nr_bytes;
    // NB. tot_nr_bytes = pri_nr_bytes + shr_nr_bytes;

    //uint32_t mcdctl_pri_nr_keys;
    //uint32_t mcdctl_shr_nr_keys;
} g_mcd_data_t;

typedef struct g_score_param {
    uint32_t alpha;
    uint32_t beta;
    uint32_t query_max;
    uint32_t val_max;
} g_score_param_t;

typedef struct g_score_data {
#define SCORE_MEM_ALPHA_DEF    80
#define SCORE_MEM_BETA_DEF     10
    g_score_param_t mem;
#define SCORE_SSD_ALPHA_DEF    10
#define SCORE_SSD_BETA_DEF     80
    g_score_param_t ssd;
} g_score_data_t;

extern bool_t use_mcd_flag;
static inline bool_t use_mcd(void)
{
    return use_mcd_flag;
}

extern bool_t mcd_enabled_flag;
static inline bool_t mcd_enabled(void)
{
    return mcd_enabled_flag;
}

// TODO
//typedef XEN_GUEST_HANDLE(mcd_op_t) mcd_cli_op_t;
//extern long do_mcd_op(mcd_cli_op_t uops);

extern void mcd_destroy(void *p);

// TODO can be removed by calling this when it happens for the first time
// but for now, just make it when domain is created in domain.c
extern int mcd_domain_create(domid_t domid);

/* 
 * Mcd interface between mcd and mcdctl
 */
void minf_get_lock(void);
void minf_get_unlock(void);
void minf_put_lock(void);
void minf_put_unlock(void);

/* 
 * Memory ballooning traps
 */ 
extern void mcd_mem_inc_trap(struct domain *dom, unsigned int nr_pages);
extern void mcd_mem_dec_trap(struct domain *dom, unsigned int nr_pages);
extern void mcd_mem_upt_trap(struct domain *dom);


/* open for sharing */
uint32_t getHash (const char * data, int len);
void *mcd_malloc(uint64_t size, uint64_t align, domid_t domid);

/*
 * Mcd Event (mcd -> mcdctl)
 */
int mcdctl_op(XEN_GUEST_HANDLE(xen_mcdctl_t) u_mcdctl);

int get_num_shared_page(void);
int minf_page_out(mcd_hashT_t *mh);
int minf_page_in(mcd_hashT_t *mh);

int minf_page_dom_create(domid_t domid);
int minf_page_dom_destroy(domid_t domid);
int minf_page_del(domid_t domid, uint32_t hash);
int minf_page_flush(void);
int minf_page_dom_flush(domid_t domid);

int increase_shared_page(unsigned int num);

//#define MCDCTL_TESTING

#ifdef MCDCTL_TESTING
int mcdctl_fake_request_xen_to_dom0(void);
int mcdctl_fake_request_dom0_to_xen(void);
#endif

/*
 * Mcd Event (mcdctl -> mcd)
 */
int init_mcdctl(void);
void exit_mcdctl(void);

#endif /* __MCD_H__ */
