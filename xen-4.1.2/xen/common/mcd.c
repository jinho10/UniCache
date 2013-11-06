/******************************************************************************
 * mcd.c
 *
 * Memcached
 *
 * Copyright (c) 2012, Jinho Hwang, The George Washington University
 */

#include <xen/mcd.h>
#include <xen/tmem.h>
#include <xen/rbtree.h>
#include <xen/radix-tree.h>
#include <xen/list.h>
#include <xen/time.h>
//#include <xen/spinlock.h>

/*------------------------------------------------------------------------------
 - Debug & Testing Definitions
 ------------------------------------------------------------------------------*/

// XXX this is only-testing purpose.. needs to be deleted..
//#define MCD_OVERHEAD_TESTING
#define MCD_QUERY_TIME_TESTING
//#define WORKLOAD_STAT_TEST

#define my_trace() printk("%s (%d) \n", __FUNCTION__, __LINE__);
//#define my_trace()

extern int mcdctl_enabled;
int mcdctl_user_enabled = 1; // default yes

unsigned int total_weight = 0;

// XXX find a max value for length and min value
uint64_t max_get_put_time = 0;
uint64_t min_get_put_time = ~(0ULL); 

uint64_t max_val_length = 0;
uint64_t min_val_length = ~(0ULL);

uint64_t max_key_length = 0;
uint64_t min_key_length = ~(0ULL);

uint32_t hit_total = 1;
uint32_t hit_success = 0;

uint32_t num_get_request = 0;
uint32_t num_put_request = 0;

#define MAX_QUERY_TIME  1000
uint32_t query_time[MAX_QUERY_TIME]; // 1 sec
#define MAX_REACCESS_TIME_MS 1000
#define MAX_REACCESS_TIME 600
uint32_t reaccess_time_ms[MAX_REACCESS_TIME_MS]; // 1 min
uint32_t reaccess_time[MAX_REACCESS_TIME]; // 10 min

typedef struct ft_measure {
    uint32_t time_ms;
    uint32_t freq;
} ft_measure;
#define N100_USERS       100000
#define N200_USERS       200000
#define NUM_USERS       N100_USERS
ft_measure ftm[NUM_USERS];
uint32_t ftm_cnt = 0;
uint32_t print_cnt = 0;

uint32_t reset_time = 60; // 1 min

#define MOVE_TO_SSD         0
#define DO_NOT_MOVE_TO_SSD  1

/*------------------------------------------------------------------------------
 - Configuration Settings
 ------------------------------------------------------------------------------*/

//#define MCD_LINEAR_PROBE
#define MCD_HASH_CHAINING

// TODO lock protection: each struct has a lock
LOCAL g_mcd_data_t mcd_data;
uint64_t max_mem_bytes; // maximum memory size

uint64_t current_free_memory_bytes; // current system memory

#define set_query_time(_mh, _ms, _m) \
    _mh->query_time_ms = _ms;\
    if ( _ms > sd._m.query_max )\
        sd._m.query_max = _ms; \
    P_REC_AVG_CAL(mcd_dom, _ms);

#define set_val_size(_mh, _val, _m) \
    _mh->val_size = _val;\
    if ( _val > sd._m.query_max )\
        sd._m.query_max = _val;

// scaled x100 ( alpha, beta = x100, time, size = x10)
#define LOC(_mh, _m) (sd._m.alpha * (_mh->put_time*10/now))
#define QUR(_mh, _m) (sd._m.beta * (10 - _mh->query_time_ms*10/sd._m.query_max))
#define VAL(_mh, _m) ((100 - sd._m.alpha - sd._m.beta) * (_mh->val_size*10/sd._m.val_max))
#define SCORE(_mh, _m) (LOC(_mh, _m) + QUR(_mh, _m) + VAL(_mh, _m))/1000

// does ++dom->nr_keys happen after * calculation???
#define P_REC_AVG_CAL(dom, nd) dom->rec_cost_avg = (dom->rec_cost_avg * dom->nr_keys + nd)/(++dom->nr_keys)
#define P_REC_AVG(dom) (dom->rec_cost_avg)

g_score_data_t sd;

#define MCD_CACHE_MODE_SHARED       0
#define MCD_CACHE_MODE_PARTITION    1
#define MCD_CACHE_MODE_MAX          2
int cache_mode = MCD_CACHE_MODE_SHARED;

#define MCD_REPLACE_MODE_LFRU        0
#define MCD_REPLACE_MODE_FAST        1
#define MCD_REPLACE_MODE_SCORE       2
#define MCD_REPLACE_MODE_MAX         3
int replacement_mode = MCD_REPLACE_MODE_LFRU;

#define MCD_PARTITION_MODE_BE       0
#define MCD_PARTITION_MODE_WB       1
#define MCD_PARTITION_MODE_AD       2
#define MCD_PARTITION_MODE_MAX      3
int partition_mode = MCD_PARTITION_MODE_BE;

/*------------------------------------------------------------------------------
 - Global Variables
 ------------------------------------------------------------------------------*/

/* MCD Shared Hash List */
LOCAL spinlock_t g_mcd_domain_list_lock; 
LOCAL struct list_head g_mcd_domain_list;

#ifdef MCD_LINEAR_PROBE
LOCAL spinlock_t g_mcd_shared_hash_lock;
LOCAL struct list_head g_mcd_shared_hash_list;
#elif defined(MCD_HASH_CHAINING)
#define MAX_LIST_HEAD   100
LOCAL spinlock_t g_mcd_shared_hash_lock[MAX_LIST_HEAD];
LOCAL struct list_head g_mcd_shared_hash_list[MAX_LIST_HEAD];
#endif
// TODO lock protection

/* MCDCTL Shared Hash List */
LOCAL spinlock_t g_mcdctl_shared_hash_lock;
LOCAL struct list_head g_mcdctl_shared_hash_list[MAX_LIST_HEAD];

// mcd-to-mcdctl control knob (mcdctl is message-based)
spinlock_t minf_get_lock_v;
spinlock_t minf_put_lock_v;

// debugging variables
//static uint32_t mcdctl_put_in = 0;
//static uint32_t mcdctl_put_out = 0;

// XXX lock needed ???
spinlock_t free_lock_t;
mcdctl_hashT_t *freelist = NULL; // free list

#define MAX_PAGES_FOR_MCDCTL    40960
void* mcdctl_pages[MAX_PAGES_FOR_MCDCTL]; // 4 page, enough?
int mcdctl_pages_cnt = 0;

/*------------------------------------------------------------------------------
 - Initial Flags
 ------------------------------------------------------------------------------*/

EXPORT bool_t __read_mostly use_mcd_flag = 0;
boolean_param("mcd", use_mcd_flag);

EXPORT bool_t __read_mostly mcd_enabled_flag = 0;

                                    
/*------------------------------------------------------------------------------
 - Local Function Definitions
 ------------------------------------------------------------------------------*/

LOCAL inline void mcd_copy_to_guest_buf_offset(mcd_va_t buf, int off,
                                           char *mcdbuf, int len);

LOCAL int shared_cache_free(uint64_t amount);

LOCAL int partition_cache_free(uint64_t amount, domid_t domid);
//LOCAL int partition_cf_best_effort(uint64_t amount, domid_t domid);
LOCAL int partition_cf_weight_based(uint64_t amount, domid_t domid);
LOCAL int partition_cf_adaptive_dynamic(uint64_t amount, domid_t domid);

LOCAL void dom_usage_update(mcd_domain_t *dom);
LOCAL uint64_t get_free_mem_size(void);
LOCAL uint64_t get_tot_mem_size(void);
LOCAL mcd_hashT_t *create_mcd_hashT(domid_t domid, uint32_t key_size, 
                                    uint32_t val_size);
LOCAL mcdctl_hashT_t *create_mcdctl_hashT(mcd_hashT_t *mh, domid_t domid);
LOCAL inline mcd_domain_t *find_mcd_dom(domid_t domid);
LOCAL void remove_hashT_wo_page_out(mcd_hashT_t *mh);
LOCAL int mcdctl_start(void);

/*------------------------------------------------------------------------------
 - Testing Functions (TODO this is temporary function to test)
 ------------------------------------------------------------------------------*/

#define N_TO_U(_n)    (_n / 1000UL)
uint64_t time_elapsed_us(uint64_t usec)
{
    return N_TO_U(NOW()) - usec;
}

void usleep(uint64_t usec)
{
    uint64_t cur_time = N_TO_U(NOW());
    uint32_t temp = 0;

    if ( usec == 0 ) return;

    while ( 1 ) {
        temp ++;
        if ( time_elapsed_us(cur_time) >= usec ) {
            break;
        }
    }

    //printk("usec = %ld, sleep = %ld \n", usec, time_elapsed_us(cur_time));
}

/*------------------------------------------------------------------------------
 - Utility Functions
 ------------------------------------------------------------------------------*/

/*
 * Random Int Value Generator
 */
uint32_t rand_val(int seed)
{
    const long  a =      16807;  // Multiplier
    const long  m = 2147483647;  // Modulus
    const long  q =     127773;  // m div a
    const long  r =       2836;  // m mod a
    static long x =          0;  // Random int value
    long        x_div_q;         // x divided by q
    long        x_mod_q;         // x modulo q
    long        x_new;           // New x value

    // Set the seed if argument is non-zero and then return zero
    if (seed > 0)
    {
        x = seed;
        return(0.0);
    }

    // RNG using integer arithmetic
    x_div_q = x / q;
    x_mod_q = x % q;
    x_new = (a * x_mod_q) - (r * x_div_q);
    if (x_new > 0)
        x = x_new;
    else
        x = x_new + m;

    return (int)x;
}

/* superFastHash */
#define get16bits(d) (*((const uint16_t *) (d)))
uint32_t getHash (const char * data, int len) 
{
    uint32_t hash = len, tmp; 
    int rem; 
                     
    if (len <= 0 || data == NULL) return 0;
                     
    rem = len & 3; 
    len >>= 2;   
                     
    for (;len > 0; len--) {
        hash += get16bits (data);
        tmp = (get16bits (data+2) << 11) ^ hash;
        hash = (hash << 16) ^ tmp; 
        data += 2*sizeof (uint16_t);
        hash += hash >> 11;
    }                
                     
    switch (rem) {
    case 3: 
        hash += get16bits (data);
        hash ^= hash << 16;
        hash ^= data[sizeof (uint16_t)] << 18;
        hash += hash >> 11;
        break;
    case 2: 
        hash += get16bits (data);
        hash ^= hash << 11;
        hash += hash >> 17;
        break;
    case 1: hash += *data;
        hash ^= hash << 10;
        hash += hash >> 1;
        break;
    }                
                     
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;

    return hash; 
}

// to safeguard the malloc fail due to the system usage
#define RESIDUAL_BYTES  PAGE_SIZE * 1000
LOCAL uint64_t get_free_sys_mem(void)
{
    int64_t free = (((uint64_t)avail_domheap_pages() * PAGE_SIZE) - RESIDUAL_BYTES);
    return (free > 0LL) ? (uint64_t)free : 0ULL;
}

LOCAL uint64_t get_tot_mem_size(void)
{
    uint64_t tot_bytes = (TOT_USED_BYTES() + get_free_sys_mem());
    if ( max_mem_bytes == 0ULL )
        return tot_bytes;
    return (tot_bytes <= max_mem_bytes) ? tot_bytes : max_mem_bytes;
}

LOCAL uint64_t get_free_mem_size(void)
{
    int64_t free_bytes = get_tot_mem_size() - TOT_USED_BYTES();
    return (free_bytes <= 0LL) ? 0ULL : (uint64_t)free_bytes;
}

/*------------------------------------------------------------------------------
 - SSD Functions
 ------------------------------------------------------------------------------*/

// called when mcdctl module is called
EXPORT int init_mcdctl(void)
{
    mcdctl_start();

    return 0;
}

EXPORT void exit_mcdctl(void)
{
    int i;

    for (i = 0; i < mcdctl_pages_cnt; i++) {
        if ( mcdctl_pages[i] )
            xfree((void*)mcdctl_pages[i]);
        mcdctl_pages[i] = NULL;
    }

    mspin_lock(&free_lock_t);
    freelist = NULL;
    mspin_unlock(&free_lock_t);

    mcdctl_pages_cnt = 0;
}


LOCAL int mcdctl_start(void)
{
    {
        int i;
        for(i = 0; i < MAX_LIST_HEAD; i++) {
            INIT_LIST_HEAD(&g_mcdctl_shared_hash_list[i]);
        }
    }

    mspin_lock_init(&g_mcdctl_shared_hash_lock);

    mspin_lock_init(&minf_get_lock_v);
    mspin_lock_init(&minf_put_lock_v);
    mspin_lock_init(&free_lock_t);

    freelist = NULL;
    mcdctl_pages_cnt = 0;

    return 0;
}

LOCAL void mcdctl_new_malloc(void)
{
    mcdctl_hashT_t *mch;
    char *buf;
    int i;

    if ( !mcdctl_enabled || !mcdctl_user_enabled )
        return;

    if ( mcdctl_pages_cnt >= MAX_PAGES_FOR_MCDCTL ) {
        printk("mcdctl_pages_cnt >= MAX_PAGES_FOR_MCDCTL \n");
        return;
    }

    // need more space for mini headers
    buf = (char*)_xmalloc(PAGE_SIZE, 0);
    if ( buf == NULL ) {
        printk("_xmalloc failed in mcdctl_new_malloc \n");
        return;
    }
    mcdctl_pages[mcdctl_pages_cnt++] = (void*)buf; // XXX needed for flush

    mspin_lock(&free_lock_t);
    freelist = (mcdctl_hashT_t*)buf;
    mspin_unlock(&free_lock_t);

    // making linked-list
    mch = freelist;
    for (i = 0; i < (PAGE_SIZE/sizeof(mcdctl_hashT_t) - 1); i++) {
        mcdctl_hashT_t *mch2 = (mcdctl_hashT_t*)(((char*)mch) + sizeof(mcdctl_hashT_t));
        mch->next = mch2;
        mch2->next = NULL;
        mch = mch2;
    }
}

LOCAL mcdctl_hashT_t* mcdctl_malloc(domid_t domid)
{
    mcdctl_hashT_t *mch = NULL;

    if ( !mcdctl_enabled || !mcdctl_user_enabled )
        return mch;

    if ( freelist == NULL ) 
        mcdctl_new_malloc();

    mspin_lock(&free_lock_t);
    if ( freelist != NULL ) {
        mch = freelist;
        freelist = freelist->next;
    } 
    mspin_unlock(&free_lock_t);

    return mch;
}

LOCAL void mcdctl_free(mcdctl_hashT_t *mch)
{
    if ( !mcdctl_enabled || !mcdctl_user_enabled )
        return;

    if ( mch != NULL )
        list_del(&mch->hash_list);

    mspin_lock(&free_lock_t);
    if ( freelist == NULL ) {
        freelist = mch;
        freelist->next = NULL;
    } else {
        mch->next = freelist;
        freelist = mch;
    }
    mspin_unlock(&free_lock_t);
}

LOCAL int mcd_page_out(mcd_hashT_t *mh)
{
    int rc = 0;
    mcdctl_hashT_t *mch;

    if ( !mcdctl_enabled || !mcdctl_user_enabled )
        return -1;

    // XXX for put testing..... a bit complicated to deal with all the situations...
    // XXX for now, just keep this way for functional testing
    rc = minf_page_out(mh);
    if ( rc == 0 ) {
        // TODO this needs to be cristal clear on operation, otherwise system will stop...

        minf_put_lock();
        minf_put_unlock();

        // XXX change mh to mini mh to keep the small size of data for outsourced data
        // mcdctl_hashT_t -> mcdctl_hashT_t

        //another way.. but not necessary
        //vcpu_pause(current);
    } else
        return rc;

    // error occurred...
    if_set(mh->flags, MCD_MINF_ERROR)
        return -1;

    // keep it in the mcdctl list
    mch = create_mcdctl_hashT(mh, mh->domain);
    if ( mch != NULL ) {
        if ( cache_mode == MCD_CACHE_MODE_SHARED ) {
            mspin_lock(&g_mcdctl_shared_hash_lock);
            list_add_tail(&mch->hash_list, &g_mcdctl_shared_hash_list[mch->hash % MAX_LIST_HEAD]);
            mspin_unlock(&g_mcdctl_shared_hash_lock);
        } else {
            mcd_domain_t *mcd_dom = find_mcd_dom(mh->domain);

            mspin_lock(&mcd_dom->mcdctl_lock);
            list_add_tail(&mch->hash_list, &mcd_dom->mcdctl_hash_list_head);
            mspin_unlock(&mcd_dom->mcdctl_lock);
        }
    } else {
        // XXX I should remove the one just paged out
        rc = -1;
    }
    

    return rc;
}

// memory should have been freeed to accomodate the one from outside..
LOCAL mcd_hashT_t* mcd_page_in(mcdctl_hashT_t *mch)
{
    mcd_hashT_t *mh = NULL;
    int rc = 0;

    if ( !mcdctl_enabled || !mcdctl_user_enabled )
        return mh;

    // XXX change mh to mini mh to keep the small size of data for outsourced data
    // mcdctl_hashT_t -> mcdctl_hashT_t
    mh = create_mcd_hashT(mch->domain, mch->key_size, mch->val_size);
    if ( mh == NULL ) {
        printk("failed to create_mcd_hashT in mcd_page_in\n");
        return mh;
    }

    mh->hash = mch->hash;

    // XXX do we have to do this??? shit.... xen-dom0 is asynchronous (message-based)
    // XXX can we use lock instead???
    // XXX for get testing... this is wrong... due to many iteration... I need separate interfaces...
    rc = minf_page_in(mh);
    if ( rc == 0 ) {
        // TODO this needs to be cristal clear on operation, otherwise system will stop...

        minf_get_lock();
        minf_get_unlock();
    }

    if_set(mh->flags, MCD_MINF_ERROR) {
        remove_hashT_wo_page_out(mh);

        printk("MCD_MINF_ERROR occurred.. something is wrong.. check this..\n");

        mh = NULL;
    }

    mspin_lock(&g_mcdctl_shared_hash_lock);
    if ( mh != NULL )
        mcdctl_free(mch);
    mspin_unlock(&g_mcdctl_shared_hash_lock);

    return mh;
}

LOCAL void mcdctl_flush(void)
{
    int i;

    // data structure flush
    for(i = 0; i < MAX_LIST_HEAD; i++) { // key length
        struct list_head *hash = &g_mcdctl_shared_hash_list[i];
        mcdctl_hashT_t *curr;
        mcdctl_hashT_t *latedel = NULL;

        mspin_lock(&g_mcdctl_shared_hash_lock);
        list_for_each_entry(curr, hash, hash_list) {
            if ( latedel == NULL ) {
                latedel = curr;
                continue;
            } else {
                mcdctl_free(latedel);
                latedel = curr;
            }
        }
        if ( latedel )
            mcdctl_free(latedel);

        // XXX reinitialize for safety???
        INIT_LIST_HEAD(&g_mcdctl_shared_hash_list[i]);

        mspin_unlock(&g_mcdctl_shared_hash_lock);
    }

    // mcdctl interface
    minf_page_flush();

    // just clear unnecessary data here
    exit_mcdctl();
}

LOCAL void mcdctl_dom_flush(mcd_domain_t *mcd_dom)
{
    mcdctl_hashT_t *curr;
    mcdctl_hashT_t *latedel = NULL;
    struct list_head *head;

    head = &mcd_dom->mcdctl_hash_list_head;
    mspin_lock(&mcd_dom->mcdctl_lock);
    list_for_each_entry(curr, head, hash_list) {
        if ( latedel == NULL ) {
            latedel = curr;
            continue;
        } else {
            mcdctl_free(latedel);
            latedel = curr;
        }
    }
    if ( latedel )
        mcdctl_free(latedel);

    // reinitialize
    INIT_LIST_HEAD(&mcd_dom->mcdctl_hash_list_head);

    mspin_unlock(&mcd_dom->mcdctl_lock);

    minf_page_dom_flush(mcd_dom->domid);
}

/*------------------------------------------------------------------------------
 - Main Functions
 ------------------------------------------------------------------------------*/

void *mcd_malloc(uint64_t size, uint64_t align, domid_t domid)
{
    uint64_t max_bytes = get_tot_mem_size();
    void *val = NULL;
    int rc = -ERR_NO;

#define SAFETY_BYTES PAGE_SIZE

    if ( size == 0 ) return NULL;

//printk("(%lu + %lu + 4096) >= %lu) \n", TOT_USED_BYTES(), size, max_bytes);

    if ( (TOT_USED_BYTES() + size + SAFETY_BYTES) >= max_bytes ) {
        if ( cache_mode == MCD_CACHE_MODE_SHARED ) {
            rc = shared_cache_free(size + SAFETY_BYTES);
        } else {
            rc = partition_cache_free(size + SAFETY_BYTES, domid);
        }
    }

    if ( rc == -ERR_NO )
        val = _xmalloc(size,align);

    // second trial
    if ( val == NULL ) {
        if ( cache_mode == MCD_CACHE_MODE_SHARED ) {
            rc = shared_cache_free(size + SAFETY_BYTES);
        } else {
            rc = partition_cache_free(size + SAFETY_BYTES, domid);
        }
    }

    if ( rc != -ERR_NO )
        return NULL;

    return (val != NULL) ? val : _xmalloc(size,align);
}

LOCAL int attach_mcd(mcd_domain_t *mcd_dom, domid_t domid) 
{
    struct domain *d = rcu_lock_domain_by_id(domid);

    if ( d == NULL )
        return -ERR_XENFAULT;

    if ( !d->is_dying ) {
        d->mcd = (void*)mcd_dom;
//        mspin_lock(&mcd_dom->lock);
        mcd_dom->domain = d;
//        mspin_unlock(&mcd_dom->lock);
    }

    rcu_unlock_domain(d);

    return -ERR_NO;
}

LOCAL void remove_hashT(mcd_hashT_t *mh)
{
    if ( mh == NULL || mh->magic != MAGIC ) return;

    mspin_lock(&mh->lock);
    list_del(&mh->hash_list);
    mspin_unlock(&mh->lock);

    /* XXX send to outside */
    mcd_page_out(mh);

    mh->magic = 0; // no more valid
    xfree((void*)mh);
}

LOCAL void remove_hashT_wo_page_out(mcd_hashT_t *mh)
{
    if ( mh == NULL || mh->magic != MAGIC ) return;

    mspin_lock(&mh->lock);
    list_del(&mh->hash_list);
    mspin_unlock(&mh->lock);

    mh->magic = 0; // no more valid
    xfree((void*)mh);
}

LOCAL uint64_t p_remove_hashT(mcd_domain_t *dom, mcd_hashT_t *mh, int flag)
{
    uint64_t bytes = 0;

    if ( mh == NULL || mh->magic != MAGIC ) return 0;

    mspin_lock(&mcd_data.lock);
    mcd_data.pri_nr_pages -= mh->nr_pages;
    mcd_data.pri_nr_bytes -= mh->nr_bytes;
    mcd_data.pri_nr_keys --;
    mspin_unlock(&mcd_data.lock);

    if ( dom != NULL ) {
//        mspin_lock(&dom->lock);
        dom->nr_used_pages -= mh->nr_pages;
        dom->nr_used_bytes -= mh->nr_bytes;
        dom->nr_keys --;
//        mspin_unlock(&dom->lock);
    }

    bytes = mh->nr_bytes;

    if ( flag == MOVE_TO_SSD && mh->flags == MCD_HASHT_NORMAL )
        remove_hashT(mh);
    else 
        remove_hashT_wo_page_out(mh);

    dom_usage_update(dom);

    return bytes;
}

LOCAL uint64_t s_remove_hashT(mcd_hashT_t *mh, int flag)
{
    uint64_t bytes = 0;
    mcd_domain_t *mcd_dom;

    if ( mh == NULL || mh->magic != MAGIC ) return 0;

    //printk("[STR] s_remove_hashT with flag(%d), TOT_USED_BYTE(%lu)\n", flag, TOT_USED_BYTES());

    mspin_lock(&mcd_data.lock);
    mcd_data.shr_nr_pages -= mh->nr_pages;
    mcd_data.shr_nr_bytes -= mh->nr_bytes;
    mcd_data.shr_nr_keys --;
    mspin_unlock(&mcd_data.lock);

    mcd_dom = find_mcd_dom(mh->domain);
    // keeping track of each data for best-effort
    if ( mcd_dom != NULL ) {
//        mspin_lock(&mcd_dom->lock);
        mcd_dom->nr_used_pages -= mh->nr_pages;
        mcd_dom->nr_used_bytes -= mh->nr_bytes;
        mcd_dom->nr_keys --;
//        mspin_unlock(&mcd_dom->lock);
    }

    bytes = mh->nr_bytes;

    if ( flag == MOVE_TO_SSD && mh->flags == MCD_HASHT_NORMAL )
        remove_hashT(mh);
    else 
        remove_hashT_wo_page_out(mh);

    //printk("[END] s_remove_hashT with flag(%d), TOT_USED_BYTE(%lu), bytes(%lu) \n", flag, TOT_USED_BYTES(), bytes);

    return bytes;
}

/*
 * Logging Functions
 */
LOCAL int mcdc_stat_display(mcd_va_t buf, int off, uint32_t len, bool_t use_long)
{
    char info[BSIZE];
    int n = 0, sum = 0;

        n += scnprintf(info, BSIZE, "shared_p:%d,tot_bytes_used:%lu(%lu:%lu),tot_bytes_free(%luM):%lu,nr_keys:%u\n",
                                    get_num_shared_page(),
                                    TOT_USED_BYTES(), 
				    mcd_data.pri_nr_bytes, mcd_data.shr_nr_bytes, 
                                    (max_mem_bytes/1000000), get_free_mem_size(),
                                    TOT_USED_KEYS());

    /*
    n += scnprintf(info, BSIZE, "tot_pages:%d,pri_pages:%d,shr_pages:%d,"
                                "tot_bytes_used:%ld,tot_bytes_free:%ld,pri_bytes:%ld,shr_bytes:%ld,"
                                "nr_pri_keys:%d,nr_shr_keys:%d\n",
                                mcd_data.pri_nr_pages + mcd_data.shr_nr_pages, 
                                mcd_data.pri_nr_pages,
                                mcd_data.shr_nr_pages,
                                mcd_data.pri_nr_bytes + mcd_data.shr_nr_bytes, 
                                current_free_memory_bytes,
                                mcd_data.pri_nr_bytes, 
                                mcd_data.shr_nr_bytes, 
                                mcd_data.pri_nr_keys,
                                mcd_data.shr_nr_keys);
    */

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    return sum;
}

#if 0
LOCAL int mcdc_domain_display(mcd_domain_t *mcd_dom, mcd_va_t buf, 
                               int off, uint32_t len, bool_t use_long)
{
    char info[BSIZE];
    int n = 0, sum = 0;
    struct domain *dom = domptr(mcd_dom);

    n += scnprintf(info, BSIZE, "tot:%d,max:%d,shr:%d,xenheap:%d,nr_avail:%d\n",
                                dom->tot_pages, dom->max_pages, dom->shr_pages.counter, 
                                dom->xenheap_pages, mcd_dom->nr_avail
                                );

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    // more prints with the same format

    return sum;
}
#endif

#ifdef WORKLOAD_STAT_TEST
LOCAL int mcdc_workload_stat(mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0, i = 0;

    //n += scnprintf(info, BSIZE, "time:%"PRIx64":%"PRIx64",val_len:%"PRIx64":%"PRIx64",key_len:%"PRIx64":%"PRIx64",hit(%"PRIx64"):%u\n",
    n += scnprintf(info, BSIZE, "time:%lu:%lu,val_len:%lu:%lu,key_len:%lu:%lu,hit(%lu):%u,get/put:%u:%u\n",
                    min_get_put_time, max_get_put_time,
                    min_val_length, max_val_length,
                    min_key_length, max_key_length, 
                    get_tot_mem_size(), (hit_success * 100)/hit_total, num_get_request, num_put_request);

    // XXX pushing out to dmesg
    for(i = 0; i < MAX_QUERY_TIME; i++) {
        //n += scnprintf(info+n, BSIZE-n, "%u,", query_time[i]);
        printk("%u,", query_time[i]);
    }
    //n += scnprintf(info+n, BSIZE-n, "\n");
    printk("\n");

    for(i = 0; i < MAX_REACCESS_TIME_MS; i++) {
        //n += scnprintf(info+n, BSIZE-n, "%u,", reaccess_time[i]);
        printk("%u,", reaccess_time_ms[i]);
    }
    //n += scnprintf(info+n, BSIZE-n, "\n");
    printk("\n");

    for(i = 0; i < MAX_REACCESS_TIME; i++) {
        //n += scnprintf(info+n, BSIZE-n, "%u,", reaccess_time[i]);
        printk("%u,", reaccess_time[i]);
    }
    //n += scnprintf(info+n, BSIZE-n, "\n");
    printk("\n");

    for(i = print_cnt; i < ftm_cnt && i < (print_cnt + NUM_USERS/10); i++) {
        printk("%u$%u;", ftm[i].time_ms, ftm[i].freq);
    }
    print_cnt = i; // next time...

    if ( i >= ftm_cnt )  { // reset
        memset(ftm, 0, NUM_USERS * sizeof(ft_measure));
        ftm_cnt = 0;
        print_cnt = 0;
        printk("ftm.... done...\n");
    }

    // key and value lengths
    /* cloudstone is too simple...
    for(i = 0; i < MAX_LIST_HEAD; i++) { // key length
        struct list_head *hash = &g_mcd_shared_hash_list[i];
        mcd_hashT_t *curr;

        list_for_each_entry(curr, hash, hash_list) {
            printk("%u,", curr->key_size);
        }
    }
    printk("\n");

    for(i = 0; i < MAX_LIST_HEAD; i++) {
        struct list_head *hash = &g_mcd_shared_hash_list[i];
        mcd_hashT_t *curr;

        list_for_each_entry(curr, hash, hash_list) {
            printk("%u,", curr->val_size);
        }
    }
    printk("\n");
    */

    // XXX - reset statistics...
    max_get_put_time = 0;
    min_get_put_time = ~(0ULL); 
    max_val_length = 0;
    min_val_length = ~(0ULL);
    max_key_length = 0;
    min_key_length = ~(0ULL);
    hit_total = 1;
    hit_success = 0;
    num_get_request = 0;
    num_put_request = 0;

    memset(query_time, 0, MAX_QUERY_TIME);
    memset(reaccess_time, 0, MAX_REACCESS_TIME);
    memset(reaccess_time_ms, 0, MAX_REACCESS_TIME_MS);

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    // more prints with the same format

    return sum;
}
#endif

enum display {
    DISPLAY_DOMAIN_STATS
};

LOCAL int mcdc_header(mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0;
    char *rstr = NULL;
    char *pstr = NULL;

    switch( replacement_mode ) {
    case MCD_REPLACE_MODE_LFRU:
        rstr = "lfru";
        break;
    case MCD_REPLACE_MODE_FAST:
        rstr = "fast";
        break;
    case MCD_REPLACE_MODE_SCORE:
        rstr = "score";
        break;
    }

    switch ( partition_mode ) {
    case MCD_PARTITION_MODE_BE:
        pstr = "best-effort";
        break;
    case MCD_PARTITION_MODE_WB:
        pstr = "weight-based";
        break;
    case MCD_PARTITION_MODE_AD: 
        pstr = "adaptive-dynamic";
        break;
        break;
    }

    n += scnprintf(info, BSIZE, "Memcached Statistics (%s | %s | mcd %s) \n", rstr, pstr, (mcdctl_enabled && mcdctl_user_enabled)? "connected":"not connected");

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    return sum;
}

LOCAL int mcdc_domain_header(mcd_domain_t *dom, mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0;

    //n += scnprintf(info, BSIZE, "dom:%d,weight:%d,ratio:%d,avail:%d,used:%d,bytes:%ld,curr:%d,", 
    //            dom->domid, dom->weight, dom->ratio, dom->nr_avail, dom->nr_used_pages, dom->nr_used_bytes, dom->curr);

    n += scnprintf(info, BSIZE, "dom:%d,weight:%d,ratio:%d,bytes:%ld,missrate:%d(%u/%u)\n", 
                dom->domid, dom->weight, dom->ratio, dom->nr_used_bytes, MISSRATE(dom), dom->get_succ, dom->get_fail);

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    return sum;
}

LOCAL int mcdc_shared_header(mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0;

    n += scnprintf(info, BSIZE, "shared:,");

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    return sum;
}

/*
 * Hash Functions: Private vs. Shared
 */
/*
LOCAL void mcd_printop(mcd_arg_t *op) 
{
    printk("option:%d, key_size:%d, val_size:%d, ", 
            op->option, 
            op->key_size,
            op->val_size);

    // key
    {
        // TODO : this needs to be moved to persistent location
        char buf[op->key_size];
        int i;

        //raw_copy_from_guest(buf, guest_handle_cast(op->key, void), op->key_size); // TODO confirm
        copy_from_guest(buf, guest_handle_cast(op->key, void), op->key_size);

        for(i = 0; i < op->key_size; i++) {
            printk("%c", buf[i]);
        }
    }
    printk(", ");

    // value
    {
        // TODO : this needs to be moved to persistent location
        char buf[op->val_size];
        int i;

        //raw_copy_from_guest(buf, guest_handle_cast(op->val, void), op->value_size); // TODO confirm
        copy_from_guest(buf, guest_handle_cast(op->val, void), op->val_size);

        for(i = 0; i < op->val_size; i++) {
            printk("%c", buf[i]);
        }
    }
    printk("\n");
}
*/

LOCAL int mcdc_hashT_display(mcd_hashT_t *data, mcd_va_t buf, int off, uint32_t len)
{
    char info[BSIZE];
    int n = 0, sum = 0, i = 0;
    s_time_t now = NOW();

    n += scnprintf(info, BSIZE, "nr_pages:%u,ref:%u,passed_time:%"PRIx64",hash:%u,key_size:%u,val_size:%u,key:",
                                data->nr_pages, data->ref, (uint64_t)(now - data->put_time), data->hash, data->key_size, 
                                data->val_size);

    for(i = 0; i < data->key_size; i++) {
        n += scnprintf(info+n, BSIZE-n, "%c", data->key_val[i]);
    }

    n += scnprintf(info+n, BSIZE-n, "\n");

    if ( sum + n >= len )
        return sum;

    mcd_copy_to_guest_buf_offset(buf, off+sum, info, n+1);
    sum += n;

    // more prints with the same format

    return sum;
}

/* 
 * Keep in Mind: only here allocate memory 
 * So, keep track of my much memory is used
 */
LOCAL mcd_hashT_t *create_mcd_hashT(domid_t domid, uint32_t key_size, uint32_t val_size)
{
    uint64_t size = sizeof(mcd_hashT_t) + key_size + val_size;
    mcd_hashT_t *mh = NULL;

    // Allocate page aligned
    if ( size > PAGE_SIZE ) {
        size += (PAGE_SIZE - size % PAGE_SIZE);
    } else {
        if ( (size % MEM_ALIGN) > 0 ) {
            size += (MEM_ALIGN - (size % MEM_ALIGN));
        }
    }

    mh = (mcd_hashT_t *) mcd_malloc(size, 0, domid);

    if ( mh == NULL ) {
        printk("mcd_malloc failed to create_hashT size = %lu \n", size);
    } else {
        memset(mh, 0, sizeof(mcd_hashT_t));

        mspin_lock_init(&mh->lock);

        mh->magic = MAGIC;

        mh->domain = domid;
        mh->nr_pages = (size / PAGE_SIZE) + ((size % PAGE_SIZE > 0) ? 1:0);
        mh->nr_bytes = size;
        mh->ref = 0;
        mh->put_time = NOW(); // ns
        mh->key_size = key_size;
        //mh->val_size = val_size;

        set_val_size(mh, val_size, mem)

        mh->key_val = ((char*)mh) + sizeof(mcd_hashT_t); // TODO confirmed

        if ( val_size == 0 )
            mh->flags = MCD_HASHT_MEASUREMENT; // do not send out to ssd
        else
            mh->flags = MCD_HASHT_NORMAL;

        mspin_lock(&mcd_data.lock);

        if ( cache_mode == MCD_CACHE_MODE_SHARED ) {
            mcd_domain_t *mcd_dom = find_mcd_dom(domid);

            mcd_data.shr_nr_pages += mh->nr_pages;
            mcd_data.shr_nr_bytes += mh->nr_bytes;
            mcd_data.shr_nr_keys ++;

            // keeping track of each data when best-effort
//            mspin_lock(&mcd_dom->lock);
            mcd_dom->nr_used_pages += mh->nr_pages;
            mcd_dom->nr_used_bytes += mh->nr_bytes;
            mcd_dom->nr_keys ++;
//            mspin_unlock(&mcd_dom->lock);
        } else {
            mcd_domain_t *mcd_dom = find_mcd_dom(domid);
            mcd_data.pri_nr_pages += mh->nr_pages;
            mcd_data.pri_nr_bytes += mh->nr_bytes;
            mcd_data.pri_nr_keys ++;

//            mspin_lock(&mcd_dom->lock);
            mcd_dom->nr_used_pages += mh->nr_pages;
            mcd_dom->nr_used_bytes += mh->nr_bytes;
            mcd_dom->nr_keys ++;
//            mspin_unlock(&mcd_dom->lock);
        }
        mspin_unlock(&mcd_data.lock);
    }

    return mh;
}

LOCAL mcd_hashT_t *create_hashT(mcd_arg_t *op, domid_t domid)
{
    //uint64_t size = sizeof(mcd_hashT_t) + op->key_size + op->val_size;
    mcd_hashT_t *mh = NULL;

    mh = create_mcd_hashT(domid, op->key_size, op->val_size);
    if ( mh != NULL ) {
        copy_from_guest(mh->key_val, guest_handle_cast(op->key, void), op->key_size);
        copy_from_guest((mh->key_val + op->key_size),
                        guest_handle_cast(op->val, void), op->val_size);

        // Copy value size back to user space for confirmation
        copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));

        mh->hash = getHash(mh->key_val, op->key_size);
    }

    return mh;
}

LOCAL mcdctl_hashT_t *create_mcdctl_hashT(mcd_hashT_t *mh, domid_t domid)
{
    mcdctl_hashT_t *mch = NULL;

    mch = (mcdctl_hashT_t *) mcdctl_malloc(domid);
    if ( mh != NULL ) {
        memset(mch, 0, sizeof(mcdctl_hashT_t));

        mch->domain = mh->domain;
        mch->hash = mh->hash;
        mch->key_size = mh->key_size;
        mch->val_size = mh->val_size;
    } else {
        //printk("mcdctl_malloc failed to create_hashT size\n");
    }

    return mch;
}

/*
 * Free up memory (caching algorithm)
 */

LOCAL void dom_usage_update(mcd_domain_t *dom)
{
    uint32_t ratio;
    uint32_t dom_ratio;
    uint64_t total;

    if ( dom == NULL ) {
        return;
    }

    total = get_tot_mem_size();
    if ( total == 0ULL ) { // impossible.. something is wrong
        ratio = 0;
    } else {
        ratio = (uint32_t) ((dom->nr_used_bytes * 100ULL) / total);
    }

    dom_ratio = (dom->ratio == 0) ? 100 : dom->ratio;

//    mspin_lock(&dom->lock);
    dom->curr = (dom->ratio <= 0) ? 0 : (uint32_t)((ratio * 100) / dom_ratio);
//    mspin_unlock(&dom->lock);
}

LOCAL void dom_usage_update_all(void)
{
    mcd_domain_t* dom;

    current_free_memory_bytes = get_free_mem_size();
    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        if ( dom->domid == 0 ) continue;
        dom_usage_update(dom);
    }
}

LOCAL void dom_weight_update(void)
{
    mcd_domain_t*   dom;

    if ( total_weight <= 0 ) {
        total_weight = 0;
        return;
    }

    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
//        mspin_lock(&dom->lock);
        dom->ratio = (dom->weight * 100)/total_weight;
//        mspin_unlock(&dom->lock);
    }
}

LOCAL int shared_cache_free(uint64_t amount)
{
    s_time_t now = NOW();

    mcd_hashT_t     *curr;
    mcd_hashT_t     *latedel;

    s_time_t maxtime;
    uint32_t minref;

#ifdef MCD_LINEAR_PROBE
    uint64_t maxloop = 0;
#endif
    uint64_t tot = 0;
    int i;
    int visited = 0;

    //printk("[STR] %lu bytes need to Freeup, used bytes (%lu) \n", amount, TOT_USED_BYTES());

#define MAX_LINEAR_PROBE 100

#ifdef MCD_LINEAR_PROBE

    while ( maxloop > MAX_LINEAR_PROBE ) {
        maxtime = 0;
        minref = (((uint32_t)~0ull>>1));
        latedel = NULL;

        mspin_lock(&g_mcd_shared_hash_lock);
        list_for_each_entry(curr, &g_mcd_shared_hash_list, hash_list) {
            if ( latedel != NULL ) { 
                tot += s_remove_hashT(latedel, MOVE_TO_SSD);
                //printk("shared freed = %d\n", tot);
                latedel = NULL;
            }

            // TO Policy : TODO check time 
            if ( (now - curr->put_time)/1000000000ull > MCD_EXP_TIME ) {
                latedel = curr;
                continue;
            }

            // Find LRU
            if ( (now - curr->put_time) > maxtime ) {
                maxtime = now - curr->put_time;
                latedel = curr;
            }

            // Find LFU
            if ( curr->ref < minref ) {
                minref = curr->ref;
                latedel = curr;
            }
        }
        mspin_unlock(&g_mcd_shared_hash_lock);

        if ( tot > amount ) return -ERR_NO;
        maxloop ++;
    }

#elif defined(MCD_HASH_CHAINING)

#define sremove(_p, _f) { tot += s_remove_hashT(_p, MOVE_TO_SSD); _p = NULL; visited = 1; }
    switch ( replacement_mode ) {
    case MCD_REPLACE_MODE_LFRU:
    {
        while ( tot < amount ) {
            visited = 0;
            for(i = 0; i < MAX_LIST_HEAD; i++) {
                struct list_head *hash = &g_mcd_shared_hash_list[i];
                mcd_hashT_t *lru = NULL, *lfu = NULL;
                maxtime = 0;
                minref = (((uint32_t)~0ull>>1));

                mspin_lock(&g_mcd_shared_hash_lock[i]);
                list_for_each_entry(curr, hash, hash_list) {
                    /* TO Policy : TODO check time 
                    if ( (now - curr->put_time)/1000000000 > MCD_EXP_TIME ) {
                        //printk("time : %ld - %ld = %ld > %d ? \n", now, curr->put_time, ((now - curr->put_time)/1000000000), MCD_EXP_TIME);
                        latedel = curr;
                        continue;
                    }
                    */

                    // LRU
                    if ( (now - curr->put_time) > maxtime ) {
                        maxtime = now - curr->put_time;
                        lru = curr;
                        continue; // not to have same thing
                    }

                    // LFU
                    if ( curr->ref < minref ) {
                        minref = curr->ref;
                        lfu = curr;
                    }
                }
                if ( lru )
                    sremove(lru, 0)

                if ( lfu )
                    sremove(lfu, 0)

                mspin_unlock(&g_mcd_shared_hash_lock[i]);

                if ( tot > amount )
                    break;
            }

            // safety escape loop
            if ( visited == 0 )
                break;
        }

        // Force to suit the size when not enough
        if ( tot < amount ) {
            for(i = 0; i < MAX_LIST_HEAD; i++) {
                struct list_head *hash = &g_mcd_shared_hash_list[i];
                latedel = NULL;

                mspin_lock(&g_mcd_shared_hash_lock[i]);
                list_for_each_entry(curr, hash, hash_list) {
                    if ( latedel != curr ) { 
                        sremove(latedel, 0)
                        if ( tot > amount )
                            break;
                    }
                    latedel = curr;
                }
                if ( latedel )
                    sremove(latedel, 0)

                mspin_unlock(&g_mcd_shared_hash_lock[i]);

                if ( tot > amount ) 
                    break;
            }
        }
    } break;
    case MCD_REPLACE_MODE_FAST:
    {
        // Force to suit the size
        if ( tot < amount ) {
            for(i = 0; i < MAX_LIST_HEAD; i++) {
                struct list_head *hash = &g_mcd_shared_hash_list[i];
                latedel = NULL;

                mspin_lock(&g_mcd_shared_hash_lock[i]);
                list_for_each_entry(curr, hash, hash_list) {
                    if ( latedel != curr ) { 
                        sremove(latedel, 0)
                        if ( tot > amount ) 
                            break;
                    }
                    latedel = curr;
                }
                if ( latedel )
                    sremove(latedel, 0)
                mspin_unlock(&g_mcd_shared_hash_lock[i]);

                if ( tot > amount ) 
                    break;
            }
        }
    } break;
    case MCD_REPLACE_MODE_SCORE:
    {
        while ( tot < amount ) {
            visited = 0;
            //printk("[STR] tot(%lu) amount(%lu) \n", tot, amount);
            for(i = 0; i < MAX_LIST_HEAD; i++) {
                struct list_head *hash = &g_mcd_shared_hash_list[i];
                mcd_hashT_t *min_score = NULL;
                minref = (((uint32_t)~0ull>>1));

                mspin_lock(&g_mcd_shared_hash_lock[i]);
                list_for_each_entry(curr, hash, hash_list) {
                    // min score
                    uint32_t score = SCORE(curr, mem);
                    if ( score < minref ) {
                        minref = score;
                        min_score = curr;
                    }
                }
                if ( min_score )
                    sremove(min_score, 0)

                mspin_unlock(&g_mcd_shared_hash_lock[i]);

                if ( tot > amount )
                    break;
            }
            //printk("[END] tot(%lu) amount(%lu) \n", tot, amount);

            // safety escape loop
            if ( visited == 0 )
                break;
        }

        // Force to suit the size when not enough
        if ( tot < amount ) {
            for(i = 0; i < MAX_LIST_HEAD; i++) {
                struct list_head *hash = &g_mcd_shared_hash_list[i];
                latedel = NULL;

                mspin_lock(&g_mcd_shared_hash_lock[i]);
                list_for_each_entry(curr, hash, hash_list) {
                    if ( latedel != curr ) { 
                        sremove(latedel, 0)
                        if ( tot > amount )
                            break;
                    }
                    latedel = curr;
                }
                if ( latedel )
                    sremove(latedel, 0)

                mspin_unlock(&g_mcd_shared_hash_lock[i]);

                if ( tot > amount ) 
                    break;
            }
        }
    } break;
    }
#endif

    //printk("[END] %lu bytes need to Freeup, used bytes (%lu) \n", amount, TOT_USED_BYTES());

    if ( tot < amount ) 
        return -ERR_NOTMEM;

    return -ERR_NO;
}

// find a overuse dom and free from it

LOCAL int partition_cf_weight_based(uint64_t amount, domid_t domid)
{
    // domid = 0 -> brutally free memory

    s_time_t now = NOW();

    mcd_hashT_t     *curr = NULL;
    mcd_hashT_t     *latedel = NULL;
    mcd_domain_t*   dom = NULL;

    s_time_t maxtime = 0;
    uint32_t minref = 0;

    uint64_t tot = 0;
    uint32_t fair_rate = 0;
    int visited = 0;
    mcd_hashT_t *lru = NULL, *lfu = NULL;

    //printk("%lu bytes Freeup for dom (%d)\n", amount, domid);

my_trace()

    dom_usage_update_all();

#define MCD_MAX_LOOP 100
#define FAIR_RATE(dom) (dom->curr / (dom->ratio + 1))
#define premove(_d, _p, _f) { tot += p_remove_hashT(_d, _p, _f); _p = NULL; visited = 1; }

    // Brutally free up randomly
    if ( domid == 0 ) {
        list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
            latedel = NULL;
            list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {
                if ( latedel != NULL ) { 
                    premove(dom, latedel, MOVE_TO_SSD)
                    if ( tot > amount ) 
                        return -ERR_NO;
                }
                latedel = curr;
            }

            if ( latedel != NULL ) {
                premove(dom, latedel, MOVE_TO_SSD)
                if ( tot > amount )
                    return -ERR_NO;
            }
        }
    } 

    // Weight-based free up
    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        if ( dom->domid == domid ) {
            fair_rate = FAIR_RATE(dom);
        }
    }

#if 0
// this is removed from condition
// TO Policy : TODO check time
if ( (now - curr->put_time)/1000000000 > MCD_EXP_TIME ) {
    latedel = curr;
    continue;
}
#endif


// algorithm common part
#define REPLACE_MODE_SUBROUTINE()                                              \
        maxtime = 0;                                                           \
        minref = (((uint32_t)~0ull>>1));                                       \
        lru = NULL; lru = NULL;                                                \
        switch ( replacement_mode ) {                                          \
        case MCD_REPLACE_MODE_LFRU:                                            \
        {                                                                      \
            mspin_lock(&dom->lock);                                            \
            list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {   \
                if ( (now - curr->put_time) > maxtime ) {                      \
                    maxtime = now - curr->put_time;                            \
                    lru = curr;                                                \
                    continue;                                                  \
                }                                                              \
                if ( curr->ref < minref ) {                                    \
                    minref = curr->ref;                                        \
                    lfu = curr;                                                \
                }                                                              \
            }                                                                  \
            if ( lru != lfu ) {                                                \
                if ( lru )                                                     \
                    premove(dom, lru, MOVE_TO_SSD)                             \
            }                                                                  \
            if ( lfu )                                                         \
                premove(dom, lfu, MOVE_TO_SSD)                                 \
            mspin_unlock(&dom->lock);                                          \
        } break;                                                               \
        case MCD_REPLACE_MODE_FAST:                                            \
        {                                                                      \
            latedel = NULL;                                                    \
            mspin_lock(&dom->lock);                                            \
            list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {   \
                if ( latedel )                                                 \
                    premove(dom, latedel, MOVE_TO_SSD)                         \
                latedel = curr;                                                \
            }                                                                  \
            if ( latedel )                                                     \
                premove(dom, latedel, MOVE_TO_SSD)                             \
            mspin_unlock(&dom->lock);                                          \
        } break;                                                               \
        case MCD_REPLACE_MODE_SCORE:                                           \
        {                                                                      \
            mcd_hashT_t *min_score = NULL;                                     \
            mspin_lock(&dom->lock);                                            \
            list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {   \
                uint32_t score = SCORE(curr, mem);                             \
                if ( score < minref ) {                                        \
                    minref = score;                                            \
                    min_score = curr;                                          \
                }                                                              \
            }                                                                  \
            if ( min_score )                                                   \
                premove(dom, min_score, MOVE_TO_SSD)                           \
            mspin_unlock(&dom->lock);                                          \
        } break;                                                               \
        }

//my_trace()
    // Find others first for fair share
    if ( tot < amount ) {
        while ( tot <  amount ) {
            visited = 0;
            list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                if ( dom->domid == domid ) continue;
                if ( FAIR_RATE(dom) > fair_rate ) {
                    REPLACE_MODE_SUBROUTINE()
                }
                if ( tot > amount ) 
                    break;
            }
            if ( visited == 0 ) 
                break;
            visited = 0;
        }
    }

//my_trace()

    // self when > 100
    if ( tot < amount ) {
        while ( tot < amount ) {
            visited = 0;
            list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                if ( dom->domid != domid ) continue;
                if ( dom->curr > 100 ) {
                    REPLACE_MODE_SUBROUTINE()
                }
                if ( tot > amount ) 
                    break;
            }
            if ( visited == 0 ) 
                break;
            visited = 0;
        }
    }
//my_trace()

    // Others check when > 100
    if ( tot < amount ) {
        while ( tot < amount ) {
            visited = 0;
            list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                if ( dom->domid == domid ) continue;
                if ( dom->curr > 100 ) {
                    REPLACE_MODE_SUBROUTINE()
                }
                if ( tot > amount ) 
                    break;
            }
            if ( visited == 0 ) 
                break;
            visited = 0;
        }
    }
//my_trace()

    // Self release
    if ( tot < amount ) {
        while ( tot < amount ) {
            visited = 0;
            list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                if ( dom->domid != domid ) continue;
                if ( dom->curr > 0 ) {
                    REPLACE_MODE_SUBROUTINE()
                }
                if ( tot > amount ) 
                    break;
            }
            if ( visited == 0 ) 
                break;
            visited = 0;
        }
    }
//my_trace()

    // Self brutally release
    if ( tot < amount && replacement_mode == MCD_REPLACE_MODE_LFRU ) {
        while ( tot < amount ) {
            list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
                if ( dom->domid != domid ) continue;
                if ( dom->curr <= 0 ) break;

                latedel = NULL;
                list_for_each_entry(curr, &dom->mcd_hash_list_head, hash_list) {
                    if ( latedel )
                        premove(dom, latedel, MOVE_TO_SSD)
                    latedel = curr;
                }
                if ( latedel )
                    premove(dom, latedel, MOVE_TO_SSD)
                if ( tot > amount ) 
                    break;
            }
        }
    }
//my_trace()

    if ( tot < amount ) 
        return -ERR_NOTMEM;

    return -ERR_NO;
}

/*
 * This tries to maximize overall performance
 * by maximizing the performance of the worst performed application
 */
LOCAL int partition_cf_adaptive_dynamic(uint64_t amount, domid_t domid)
{
    s_time_t now = NOW();

    mcd_hashT_t     *curr = NULL;
    mcd_hashT_t     *latedel = NULL;
    mcd_domain_t*   dom = NULL;

    s_time_t maxtime = 0;
    uint32_t minref = 0;

    uint64_t tot = 0;
    int visited = 0;
    mcd_hashT_t *lru = NULL, *lfu = NULL;

    mcd_domain_t *hr_dom = NULL;
    uint32_t hr_val = ~(0x00000000);
    uint32_t max_rec_cost = 1; // just in case
    uint32_t cost;

//printk("[STR] adaptive_dynamic, amount(%lu), tot(%lu)\n", amount, tot);

    // XXX XXX I need to make this more adaptable 
    // when hr_dom has no data, I need to find another dom to free up the memory

// TODO I have two parameters... [0, 100]
#define COST(dom,max) ((MISSRATE(dom) * (P_REC_AVG(dom)*100)/max)/100)

    // find the largest recovery average cost
    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
//printk("dom(%u), P_REC_AVG(%u) \n", dom->domid, P_REC_AVG(dom));
        if ( P_REC_AVG(dom) > max_rec_cost ) {
            max_rec_cost = P_REC_AVG(dom);
        }
    }

//printk("found the largest recovery average: %d\n", max_rec_cost);

    // find the minimum cost
    // can be himself
    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        cost = COST(dom, max_rec_cost); 

// debugging...
//printk("max_rec_cost(%u), dom(%u), cost(%u), hr_val(%u)\n", max_rec_cost, dom->domid, cost, hr_val);

        if ( cost < hr_val ) {
            hr_val = cost;
            hr_dom = dom;
        }
    }

    if ( hr_dom == NULL ) {
        //printk("%s domain is not found...\n", __FUNCTION__);
        //return -ERR_NOEXIST;
        hr_dom = find_mcd_dom(domid);
    }

    dom = hr_dom;

    // free this guy
    if ( dom ) {
        while ( tot <  amount ) {
            visited = 0;
            REPLACE_MODE_SUBROUTINE()

            if ( visited == 0 ) 
                break;
        }
    }

    // self
    dom = find_mcd_dom(domid);
    while ( tot <  amount ) {
        visited = 0;
        REPLACE_MODE_SUBROUTINE()

        if ( visited == 0 ) 
            break;
    }

//printk("[END] adaptive_dynamic, amount(%lu), tot(%lu)\n", amount, tot);

    if ( tot < amount ) 
        return -ERR_NOTMEM;

    return -ERR_NO;
}

LOCAL int partition_cache_free(uint64_t amount, domid_t domid)
{
    int rc = -ERR_NO;
    switch ( partition_mode ) {
#if 0
    // this is handled by cache_mode == SHARED
    case MCD_PARTITION_MODE_BE:
        partition_cf_best_effort(amount, domid);
        break;
#endif

    // XXX need lock for looping???
    case MCD_PARTITION_MODE_WB:
        rc = partition_cf_weight_based(amount, domid);
        break;
    case MCD_PARTITION_MODE_AD: 
        rc = partition_cf_adaptive_dynamic(amount, domid);
        break;
    default:
        // do nothing
        break;
    }

    return rc;
}

LOCAL mcd_hashT_t *find_mcd_hashT(int option, struct list_head *head, uint32_t hash, char* key, uint32_t key_size)
{
    mcd_hashT_t *curr;
    mcd_hashT_t *found = NULL;

    list_for_each_entry(curr, head, hash_list) {
        if ( curr->hash == hash ) {
            found = curr;
            break;
            /*
            int i;
            int is_same = 1;
            for(i = 0; i < key_size; i++) {
                if ( curr->key_val[i] != key[i] ) {
                    is_same = 0;
                    break;
                }
            }

            if ( is_same ) {
                found = curr;
                break;
            } 
            */
        }
    }

    return found;
}

LOCAL mcdctl_hashT_t *find_mcdctl_hashT(int option, struct list_head *head, uint32_t hash)
{
    mcdctl_hashT_t *curr;
    mcdctl_hashT_t *found = NULL;

    list_for_each_entry(curr, head, hash_list) {
        if ( curr->hash == hash ) {
            found = curr;
            break;
        }
    }

    return found;
}

/* 
 * Private 
 */
LOCAL int p_mcd_check(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcdctl_hashT_t *mch;
    char key[op->key_size];
    uint32_t hash;
    int error = -ERR_NOEXIST;
    int rc = -ERR_NO;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

    mh = find_mcd_hashT(MCDOPT_private, &mcd_dom->mcd_hash_list_head, hash, key, op->key_size);

    if ( mh == NULL ) {
        mch = find_mcdctl_hashT(MCDOPT_private, &mcd_dom->mcdctl_hash_list_head, hash);

        if ( mch == NULL ) {
            copy_to_guest(guest_handle_cast(op->r_val_size, void), &error, sizeof(uint32_t));
        } else {
            copy_to_guest(guest_handle_cast(op->r_val_size, void), &mch->val_size, sizeof(uint32_t));
            rc = RET_GET_SSD;
        }
    } else {
        copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));
        rc = RET_GET_MEM;
    }

#ifdef MCD_QUERY_TIME_TESTING
    // XXX jinho - checking get and put time difference..
    if ( mh == NULL ) {
        mh = create_mcd_hashT(mcd_dom->domid, op->key_size, 0); // header only
        if ( mh != NULL ) {
//printk("check mcd added ...hash = %u \n", hash);
            mh->hash = hash;
            copy_from_guest(mh->key_val, guest_handle_cast(op->key, void), op->key_size);

            mspin_lock(&mcd_dom->lock);
            list_add_tail(&mh->hash_list, &mcd_dom->mcd_hash_list_head);
            mspin_unlock(&mcd_dom->lock);
        } else {
//printk("check mcd added failed......hash = %u \n", hash);
        }

        //copy_to_guest(guest_handle_cast(op->r_val_size, void), &error, sizeof(uint32_t));
    }
#endif

    return rc;
}

LOCAL int p_mcd_remove(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcdctl_hashT_t *mch;
    char key[op->key_size];
    uint32_t hash;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

    mh = find_mcd_hashT(MCDOPT_private, &mcd_dom->mcd_hash_list_head, hash, key, op->key_size);

    if ( mh == NULL ) {
        mch = find_mcdctl_hashT(MCDOPT_private, &mcd_dom->mcdctl_hash_list_head, hash);
        if ( mch != NULL ) {
            minf_page_del(mch->domain, mch->hash);

            mspin_lock(&mcd_dom->mcdctl_lock);
            mcdctl_free(mch);
            mspin_unlock(&mcd_dom->mcdctl_lock);
        }
    } else {
        mspin_lock(&mcd_dom->lock);
        p_remove_hashT(mcd_dom, mh, DO_NOT_MOVE_TO_SSD);
        mspin_unlock(&mcd_dom->lock);
    }

    return -ERR_NO;
}

LOCAL int p_mcd_put(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcdctl_hashT_t *mch;
    char key[op->key_size];
    uint32_t hash = 0;
    uint32_t query_time_ms = 0;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

    mspin_lock(&mcd_dom->lock);
    mh = find_mcd_hashT(MCDOPT_private, &mcd_dom->mcd_hash_list_head, hash, key, op->key_size);
    mspin_unlock(&mcd_dom->lock);

    if ( mh != NULL ) {
        uint64_t time_diff_ns = NOW() - mh->put_time;
        uint32_t time_diff_ms = (uint32_t)(time_diff_ns / 1000000ULL);

        query_time_ms = time_diff_ms;

        mspin_lock(&mcd_dom->lock);
        p_remove_hashT(mcd_dom, mh, MOVE_TO_SSD);
        mspin_unlock(&mcd_dom->lock);
    } else {
        mch = find_mcdctl_hashT(MCDOPT_private, &mcd_dom->mcdctl_hash_list_head, hash);
        if ( mch != NULL ) {
            minf_page_del(mch->domain, mch->hash);

            mspin_lock(&mcd_dom->mcdctl_lock);
            mcdctl_free(mch);
            mspin_unlock(&mcd_dom->mcdctl_lock);
        }
    }

//    if ( mh == NULL ) {
        mh = create_hashT(op, mcd_dom->domid);
        if ( mh == NULL ) 
            return -ERR_MCDFAULT;

        set_query_time(mh, query_time_ms, mem)

//printk("query_time = %u\n", query_time_ms);

        mspin_lock(&mcd_dom->lock);
        list_add_tail(&mh->hash_list, &mcd_dom->mcd_hash_list_head);
        mspin_unlock(&mcd_dom->lock);
//    }

    copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));

    return -ERR_NO;
}

LOCAL int p_mcd_get(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcdctl_hashT_t *mch;
    char key[op->key_size];
    uint32_t hash;
    int rc = -ERR_NO;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

    mh = find_mcd_hashT(MCDOPT_private, &mcd_dom->mcd_hash_list_head, hash, key, op->key_size);
    if ( mh == NULL ) {
        mch = find_mcdctl_hashT(MCDOPT_private, &mcd_dom->mcdctl_hash_list_head, hash);

        if ( mch != NULL ) {
            mh = mcd_page_in(mch); // this also remove mch
            rc = RET_GET_SSD;
        }

	if ( mh != NULL ) {
	    mspin_lock(&mcd_dom->lock);
	    list_add_tail(&mh->hash_list, &mcd_dom->mcd_hash_list_head);
	    mspin_unlock(&mcd_dom->lock);
	}
    } else {
            rc = RET_GET_MEM;
    }

    if ( mh == NULL )
        return -ERR_MCDFAULT;

    mspin_lock(&mh->lock);
    mh->ref ++;
    mh->put_time = NOW();
    mspin_unlock(&mh->lock);

    if ( mh->val_size > op->val_size ) return -ERR_MCDFAULT;
    copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));
    copy_to_guest(guest_handle_cast(op->val, void), (mh->key_val + mh->key_size), mh->val_size);

    return rc;
}

LOCAL int p_mcd_flush(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcd_hashT_t *prev = NULL;

    if ( mcd_dom == NULL ) 
        return -ERR_NOEXIST;

    // Hash Table Memory Cleanup
    mspin_lock(&mcd_dom->lock);
    list_for_each_entry(mh, &mcd_dom->mcd_hash_list_head, hash_list) {
        if ( prev == NULL ) { // first item
            prev = mh;
            continue;
        }
        p_remove_hashT(mcd_dom, prev, DO_NOT_MOVE_TO_SSD);
        prev = mh;
    }
    if ( prev != NULL )  // last item
        p_remove_hashT(mcd_dom, prev, DO_NOT_MOVE_TO_SSD);

    INIT_LIST_HEAD(&mcd_dom->mcd_hash_list_head);

    mspin_unlock(&mcd_dom->lock);

    // mcdctl interface
    mcdctl_dom_flush(mcd_dom);

    return -ERR_NO;
}

/* 
 * Shared 
 */
LOCAL int s_mcd_check(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcdctl_hashT_t *mch;
    char key[op->key_size];
    uint32_t hash;
    uint32_t error = -ERR_NOEXIST;
    int rc = -ERR_NO;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

//printk("s_mcd_check... hash = %u \n", hash);

#ifdef MCD_LINEAR_PROBE
    mh = find_mcd_hashT(MCDOPT_shared, &g_mcd_shared_hash_list, hash, key, op->key_size);
#elif defined(MCD_HASH_CHAINING)
    mh = find_mcd_hashT(MCDOPT_shared, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD], hash, key, op->key_size);
#endif

    if ( mh == NULL ) {
        mch = find_mcdctl_hashT(MCDOPT_shared, &g_mcdctl_shared_hash_list[hash % MAX_LIST_HEAD], hash);

        if ( mch == NULL ) {
            copy_to_guest(guest_handle_cast(op->r_val_size, void), &error, sizeof(uint32_t));
        } else {
            copy_to_guest(guest_handle_cast(op->r_val_size, void), &mch->val_size, sizeof(uint32_t));
            rc = RET_GET_SSD;
        }
    } else {
        copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));
        rc = RET_GET_MEM;
    }

#ifdef MCD_QUERY_TIME_TESTING
    // XXX jinho - checking get and put time difference..
    if ( mh == NULL ) {
        mh = create_mcd_hashT(mcd_dom->domid, op->key_size, 0); // header only
        if ( mh != NULL ) {

//printk("check mcd added ...hash = %u \n", hash);

            mh->hash = hash;
            copy_from_guest(mh->key_val, guest_handle_cast(op->key, void), op->key_size);

#ifdef MCD_LINEAR_PROBE
            mspin_lock(&g_mcd_shared_hash_lock);
            list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list);
            mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
            mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
            list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD]);
            mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
#endif
        } else {
//printk("check mcd added failed......hash = %u \n", hash);
        }

        //copy_to_guest(guest_handle_cast(op->r_val_size, void), &error, sizeof(uint32_t));
    }
#endif

    return -ERR_NO;
}

LOCAL int s_mcd_put(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcdctl_hashT_t *mch;
    char key[op->key_size];
    uint32_t hash = 0;
    uint32_t query_time_ms = 0;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

//printk("s_mcd_put... key %s, hash = %u, key_size = %u, val_size = %u \n", hash, op->key_size, op_val_size);


#ifdef MCD_LINEAR_PROBE
    mspin_lock(&g_mcd_shared_hash_lock);
    mh = find_mcd_hashT(MCDOPT_shared, &g_mcd_shared_hash_list, hash, key, op->key_size);
    mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
    mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
    mh = find_mcd_hashT(MCDOPT_shared, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD], hash, key, op->key_size);
    mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
#endif

    // XXX - jinho statistics ... checking get and put time difference...
    if ( mh != NULL ) {
        uint64_t time_diff_ns = NOW() - mh->put_time;
        uint32_t time_diff_ms = (uint32_t)(time_diff_ns / 1000000ULL);

        query_time_ms = time_diff_ms;

        if ( time_diff_ns > max_get_put_time )
            max_get_put_time = time_diff_ns;

        if ( time_diff_ns < min_get_put_time )
            min_get_put_time = time_diff_ns;

//printk("------ mh != NULL... hash = %u, time = %u ms \n", hash, time_diff_ms);

        if ( time_diff_ms >= MAX_QUERY_TIME )
            query_time[MAX_QUERY_TIME - 1]++;
        else 
            query_time[time_diff_ms]++;

#ifdef MCD_LINEAR_PROBE
        mspin_lock(&g_mcd_shared_hash_lock);
        s_remove_hashT(mh, DO_NOT_MOVE_TO_SSD);
        mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
        mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
        s_remove_hashT(mh, DO_NOT_MOVE_TO_SSD);
        mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
#endif
    } else {

//printk("mh == NULL... hash = %u \n", hash);

        mch = find_mcdctl_hashT(MCDOPT_shared, &g_mcdctl_shared_hash_list[hash % MAX_LIST_HEAD], hash);
        if ( mch != NULL ) {
            minf_page_del(mch->domain, mch->hash);

            mspin_lock(&g_mcdctl_shared_hash_lock);
            mcdctl_free(mch);
            mspin_unlock(&g_mcdctl_shared_hash_lock);
        }
    }

    num_put_request++;

    // XXX - jinho checking length statistics...
    if ( op->val_size > max_val_length )
        max_val_length = op->val_size;

    if ( op->val_size < min_val_length )
        min_val_length = op->val_size;

    if ( op->key_size > max_key_length )
        max_key_length = op->key_size;

    if ( op->key_size < min_key_length )
        min_key_length = op->key_size;

//    if ( mh == NULL ) {

        mh = create_hashT(op, mcd_dom->domid);
        if ( mh == NULL ) 
            return -ERR_MCDFAULT;

        set_query_time(mh, query_time_ms, mem)

#ifdef MCD_LINEAR_PROBE
        mspin_lock(&g_mcd_shared_hash_lock);
        list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list);
        mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
        mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
        list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD]);
        mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
#endif
//    }

    copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));

#ifdef MCD_OVERHEAD_TESTING
    if ( mcdctl_enabled ) {
        mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
        s_remove_hashT(mh, MOVE_TO_SSD); // do page_out
        mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
    }
#endif

    return -ERR_NO;
}

LOCAL int s_mcd_get(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcdctl_hashT_t *mch;
    char key[op->key_size];
    uint32_t hash;
    int rc = -ERR_NO;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);

//printk("s_mcd_get... hash = %u \n", hash);

#ifdef MCD_LINEAR_PROBE
    mspin_lock(&g_mcd_shared_hash_lock);
    mh = find_mcd_hashT(MCDOPT_shared, &g_mcd_shared_hash_list, hash, key, op->key_size);
    mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
    mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
    mh = find_mcd_hashT(MCDOPT_shared, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD], hash, key, op->key_size);
    mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
#endif

    if ( mh == NULL ) {
        mch = find_mcdctl_hashT(MCDOPT_shared, &g_mcdctl_shared_hash_list[hash % MAX_LIST_HEAD], hash);

        if ( mch != NULL ) {
            mh = mcd_page_in(mch); // this also remove mch
            rc = RET_GET_SSD;
        }

	if ( mh != NULL ) {
            #ifdef MCD_LINEAR_PROBE
            mspin_lock(&g_mcd_shared_hash_lock);
            list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list);
            mspin_unlock(&g_mcd_shared_hash_lock);
            #elif defined(MCD_HASH_CHAINING)
            mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
            list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD]);
            mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
            #endif
	}
    } else {
        rc = RET_GET_MEM;
    }

    // statistics
    if ( mh != NULL ) { 
        uint64_t time_diff_ns = NOW() - mh->put_time;
        uint32_t time_diff_ms = (uint32_t)(time_diff_ns / 1000000ULL);
        uint32_t time_diff_sec = (uint32_t)(time_diff_ns / 1000000000ULL);

        if ( time_diff_sec == 0 ) {
            if ( time_diff_ms >= MAX_REACCESS_TIME_MS )
                reaccess_time_ms[MAX_REACCESS_TIME_MS - 1]++;
            else 
                reaccess_time[time_diff_ms]++;
        } else {
            if ( time_diff_sec >= MAX_REACCESS_TIME )
                reaccess_time[MAX_REACCESS_TIME - 1]++;
            else
                reaccess_time[time_diff_sec]++;
        }

        if ( mh->val_size > 0 && ftm_cnt < NUM_USERS ) {
            ftm[ftm_cnt].time_ms = time_diff_ms;
            ftm[ftm_cnt].freq = mh->ref;
            ftm_cnt ++;
        }
    }

    // XXX - jinho workload hit rate testing...
    hit_total++;
    num_get_request++;

    // XXX - jinho workload hit rate testing...
    hit_success++;

    if ( mh == NULL ) 
        return -ERR_MCDFAULT;

    mspin_lock(&mh->lock);
    mh->ref ++;
    mh->put_time = NOW();
    mspin_unlock(&mh->lock);

    // check allocated mem is bigger than val size
    if ( mh->val_size > op->val_size ) 
        return -ERR_MCDFAULT;

    copy_to_guest(guest_handle_cast(op->r_val_size, void), &mh->val_size, sizeof(uint32_t));
    copy_to_guest(guest_handle_cast(op->val, void), (mh->key_val + mh->key_size), mh->val_size);

#ifdef MCD_OVERHEAD_TESTING
    if ( mcdctl_enabled ) {
#ifdef MCD_LINEAR_PROBE
        mspin_lock(&g_mcd_shared_hash_lock);
        list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list);
        s_remove_hashT(mh, MOVE_TO_SSD); // should page out again..
        mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
        mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
        list_add_tail(&mh->hash_list, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD]);
        s_remove_hashT(mh, MOVE_TO_SSD); // should page out again..
        mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
#endif
    }
#endif

    return rc;
}

LOCAL int s_mcd_remove(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
    mcd_hashT_t *mh;
    mcdctl_hashT_t *mch;
    char key[op->key_size];
    uint32_t hash;

    copy_from_guest(key, guest_handle_cast(op->key, void), op->key_size);
    hash = getHash(key, op->key_size);


#ifdef MCD_LINEAR_PROBE
    mspin_lock(&g_mcd_shared_hash_lock);
    mh = find_mcd_hashT(MCDOPT_shared, &g_mcd_shared_hash_list, hash, key, op->key_size);
    mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
    mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
    mh = find_mcd_hashT(MCDOPT_shared, &g_mcd_shared_hash_list[hash % MAX_LIST_HEAD], hash, key, op->key_size);
    mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
#endif

    if ( mh == NULL ) {
        mch = find_mcdctl_hashT(MCDOPT_shared, &g_mcdctl_shared_hash_list[hash % MAX_LIST_HEAD], hash);
        if ( mch != NULL ) {
            minf_page_del(mch->domain, mch->hash);

            mspin_lock(&g_mcdctl_shared_hash_lock);
            mcdctl_free(mch);
            mspin_unlock(&g_mcdctl_shared_hash_lock);
        }
    } else {
#ifdef MCD_LINEAR_PROBE
        mspin_lock(&g_mcd_shared_hash_lock);
        s_remove_hashT(mh, DO_NOT_MOVE_TO_SSD);
        mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
        mspin_lock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
        s_remove_hashT(mh, DO_NOT_MOVE_TO_SSD);
        mspin_unlock(&g_mcd_shared_hash_lock[hash % MAX_LIST_HEAD]);
#endif
    }

    return -ERR_NO;
}

LOCAL int s_mcd_flush(mcd_domain_t *mcd_dom, mcd_arg_t *op)
{
#ifdef MCD_LINEAR_PROBE
    mcd_hashT_t *mh;
    mcd_hashT_t *prev;

    // TODO check whether this is correct -> late free??
    prev = NULL;
    mspin_lock(&g_mcd_shared_hash_lock);
    list_for_each_entry(mh, &g_mcd_shared_hash_list, hash_list) {
        if ( prev != NULL ) {
            s_remove_hashT(prev, DO_NOT_MOVE_TO_SSD);
            prev = NULL;
        }
        prev = mh;
    }

    if ( prev != NULL ) // last item
        s_remove_hashT(prev, DO_NOT_MOVE_TO_SSD);

    mspin_unlock(&g_mcd_shared_hash_lock);

#elif defined(MCD_HASH_CHAINING)
    mcd_hashT_t *mh;
    mcd_hashT_t *prev;
    int i;

    for(i = 0; i < MAX_LIST_HEAD; i++) {
        struct list_head *hash_list = &g_mcd_shared_hash_list[i];
        
        prev = NULL;
        mspin_lock(&g_mcd_shared_hash_lock[i]);
        list_for_each_entry(mh, hash_list, hash_list) {
            if ( prev == NULL ) { // first item
                prev = mh;
                continue;
            }
            s_remove_hashT(prev, DO_NOT_MOVE_TO_SSD);
            prev = mh;
        }

        if ( prev != NULL ) // last item
            s_remove_hashT(prev, DO_NOT_MOVE_TO_SSD);

        // reinitialize
        INIT_LIST_HEAD(&g_mcd_shared_hash_list[i]);

        mspin_unlock(&g_mcd_shared_hash_lock[i]);
    }
#endif

    // mcdctl flush
    mcdctl_flush();

    return -ERR_NO;
}

LOCAL void s_mcd_flush_all(void)
{
    s_mcd_flush(NULL, NULL);
}

/*
 * Domain-related functions
 */
LOCAL void mcd_domain_free(mcd_domain_t *d)
{
    if( d == NULL ) 
        return;

    p_mcd_flush(d, NULL);

    mspin_lock(&d->lock);
    list_del(&d->mcd_list);
    mspin_unlock(&d->lock);

    xfree((void*)d);
}

LOCAL void p_mcd_flush_all(void)
{
    mcd_domain_t *dom;

    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        if ( dom->domid == 0 ) 
            continue;
        p_mcd_flush(dom, NULL);
    }

    // just clear unnecessary data here
    exit_mcdctl();
}

LOCAL void mcd_dom_params_reset(void)
{
    mcd_domain_t *dom;

    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        if ( dom->domid == 0 ) 
            continue;

        dom->reset = NOW();
        dom->get_succ = 0;
        dom->get_fail = 0;
        dom->hitrate = 0;
    }
}

LOCAL inline mcd_domain_t *mcd_domain_from_domid(domid_t did)
{
    mcd_domain_t *c;
    struct domain *d = rcu_lock_domain_by_id(did);
    if (d == NULL)
        return NULL;

    c = (mcd_domain_t *)(d->mcd);
    rcu_unlock_domain(d);

    return c;
}

LOCAL inline mcd_domain_t *mcd_get_mcd_domain_from_current(void)
{
    return (mcd_domain_t*)(current->domain->mcd);
}

LOCAL inline domid_t mcd_get_domid_from_current(void)
{
    return current->domain->domain_id;
}

LOCAL inline struct domain *mcd_get_domain_from_current(void)
{
    return (current->domain);
}

LOCAL inline mcd_domain_t *find_mcd_dom(domid_t domid)
{
    mcd_domain_t*   dom;

    list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
        if ( dom->domid == domid ) 
            break;
    }

    return dom;
}

LOCAL inline void mcd_copy_to_guest_buf_offset(mcd_va_t buf, int off,
                                           char *mcdbuf, int len)
{
    copy_to_guest_offset(buf, off, mcdbuf, len);
}

/*
 * Hypercall Operations Functions
 */
LOCAL int display_stats(domid_t dom_id, mcd_va_t buf, uint32_t len, bool_t use_long)
{
    mcd_domain_t*   dom;
    int off = 0;

    off += mcdc_header(buf, 0, len);
    off += mcdc_stat_display(buf, off, len-off, use_long);

    if( dom_id == MCD_DOM_ID_NULL ) {
        list_for_each_entry(dom, &g_mcd_domain_list, mcd_list) {
            off += mcdc_domain_header(dom, buf, off, len-off);
            //off += mcdc_domain_display(dom, buf, off, len-off, use_long);

            // detail
            if ( use_long ) {
                mcd_hashT_t *curr;
                struct list_head *head;

                head = &dom->mcd_hash_list_head;
                list_for_each_entry(curr, head, hash_list) { // queue list
                    off += mcdc_hashT_display(curr, buf, off, len-off);
                }
            }
        }
    } else {
        // when error, return -1;
    }

    // workload performance
#ifdef WORKLOAD_STAT_TEST
    off += mcdc_workload_stat(buf, off, len-off);
#endif

    // detail for shared queue
    if ( use_long ) {
        mcd_hashT_t *curr;
        int i;

        off += mcdc_shared_header(buf, off, len-off);
#ifdef MCD_LINEAR_PROBE
        mspin_lock(&g_mcd_shared_hash_lock);
        list_for_each_entry(curr, &g_mcd_shared_hash_list, hash_list) {
            off += mcdc_hashT_display(curr, buf, off, len-off);
        }
        mspin_unlock(&g_mcd_shared_hash_lock);
#elif defined(MCD_HASH_CHAINING)
        for(i = 0; i < MAX_LIST_HEAD; i++) {
            struct list_head *hash_list = &g_mcd_shared_hash_list[i];

            mspin_lock(&g_mcd_shared_hash_lock[i]);
            list_for_each_entry(curr, hash_list, hash_list) {
                off += mcdc_hashT_display(curr, buf, off, len-off);
            }
            mspin_unlock(&g_mcd_shared_hash_lock[i]);
        }
#endif
    }

    return -ERR_NO;
}

LOCAL int set_memsize(int memsize, mcd_va_t buf, uint32_t len)
{
    char info[BSIZE];
    int n = 0;
    int rc = -ERR_NO;
    
    //s_time_t prev = NOW();

    max_mem_bytes = ((uint64_t)memsize) * 1000000ULL;
    if ( max_mem_bytes != 0 && TOT_USED_BYTES() > max_mem_bytes ) {
        if ( cache_mode == MCD_CACHE_MODE_SHARED ) {
printk("set_memsize = %lu\n", (TOT_USED_BYTES() - max_mem_bytes));
            rc = shared_cache_free(TOT_USED_BYTES() - max_mem_bytes);
        } else {
            rc = partition_cache_free(TOT_USED_BYTES() - max_mem_bytes, 0);
        }
    }

    n += scnprintf(info, BSIZE, "Maximum memory size is set to %d MB", memsize);
    mcd_copy_to_guest_buf_offset(buf, 0, info, n);

    dom_usage_update_all();

    return -ERR_NO;
}

LOCAL int set_weight(domid_t domid, uint32_t weight, mcd_va_t buf, uint32_t len)
{
    char info[BSIZE];
    int n = 0;
    mcd_domain_t *mcd_dom = find_mcd_dom(domid);

    if ( mcd_dom == NULL ) {
        printk("mcd_dom for %d is NULL\n", domid);
        return -1;
    }

    total_weight += (weight - mcd_dom->weight);

    mspin_lock(&mcd_dom->lock);
    mcd_dom->weight = weight; 
    mspin_unlock(&mcd_dom->lock);

    dom_weight_update();
    
    n += scnprintf(info, BSIZE, "Weight of dom %d is set to %d", domid, weight);
    mcd_copy_to_guest_buf_offset(buf, 0, info, n);

    return -ERR_NO;
}

LOCAL int set_cache_mode(uint32_t which, uint32_t mode, mcd_va_t buf, uint32_t len)
{
    char info[BSIZE];
    char *str = NULL;
    uint32_t prev = cache_mode;
    int n = 0;

    switch ( which ) {
    case 0:
        if ( mode >= MCD_CACHE_MODE_MAX )
            cache_mode = MCD_CACHE_MODE_SHARED;
        else
            cache_mode = mode;
        str = (mode == MCD_CACHE_MODE_SHARED) ? "shared" : "private";

        if ( prev != cache_mode ) {
            (cache_mode == MCD_CACHE_MODE_SHARED) ? p_mcd_flush_all() : s_mcd_flush_all();
        }

        break;
    case 1:
        if ( mode >= MCD_REPLACE_MODE_MAX )
            replacement_mode = MCD_REPLACE_MODE_LFRU;
        else
            replacement_mode = mode;

        switch( replacement_mode ) {
        case MCD_REPLACE_MODE_LFRU:
            str = "lfru";
            break;
        case MCD_REPLACE_MODE_FAST:
            str = "fast";
            break;
        case MCD_REPLACE_MODE_SCORE:
            str = "score";
            break;
        }
        break;
    case 2: // also change cache_mode
        if ( mode >= MCD_PARTITION_MODE_MAX )
            partition_mode = MCD_PARTITION_MODE_BE;
        else
            partition_mode = mode;

        switch( partition_mode ) {
        case MCD_PARTITION_MODE_BE:
            str = "best-effort";

            // change the cache mode -> shared
            cache_mode = MCD_CACHE_MODE_SHARED;
            if ( prev != cache_mode ) {
                (cache_mode == MCD_CACHE_MODE_SHARED) ? p_mcd_flush_all() : s_mcd_flush_all();
            }

            break;
        case MCD_PARTITION_MODE_WB:
            str = "weight-based";

            cache_mode = MCD_CACHE_MODE_PARTITION;
            if ( prev != cache_mode ) {
                (cache_mode == MCD_CACHE_MODE_SHARED) ? p_mcd_flush_all() : s_mcd_flush_all();
            }
            break;
        case MCD_PARTITION_MODE_AD: 
            str = "adaptive-dynamic";

            cache_mode = MCD_CACHE_MODE_PARTITION;
            if ( prev != cache_mode ) {
                (cache_mode == MCD_CACHE_MODE_SHARED) ? p_mcd_flush_all() : s_mcd_flush_all();
            }
            break;
        default:
            str = "error";
            break;
        }
        break;
    default:
        str = "error";
        break;
    }

    n += scnprintf(info, BSIZE, "Set to %s mode", str);
    mcd_copy_to_guest_buf_offset(buf, 0, info, n);

    return -ERR_NO;
}

LOCAL int set_param(uint32_t which, uint32_t mode, uint32_t param, mcd_va_t buf, uint32_t len)
{
    char info[BSIZE];
    int n = 0;
    char *str = NULL;

    uint32_t alpha = 0;
    uint32_t beta = 0;

    int ret = -ERR_NO;

    switch ( which ) {
    case 0: // memory
        str = "memory";

        switch ( mode ) {
        case 0:
            sd.mem.alpha = param;                
            break;
        case 1:
            sd.mem.beta = param;                
            break;
        }

        alpha = sd.mem.alpha;
        beta = sd.mem.beta;
        break;
    case 1: // ssd
        str = "ssd";

        switch ( mode ) {
        case 0:
            sd.ssd.alpha = param;                
            break;
        case 1:
            sd.ssd.beta = param;                
            break;
        }

        alpha = sd.ssd.alpha;
        beta = sd.ssd.beta;
        break;
    case 2:
        reset_time = param; // sec
        break;
    case 25: // x
        switch ( param ) {
        case 1:
            mcd_dom_params_reset();
            (cache_mode == MCD_CACHE_MODE_SHARED) ? s_mcd_flush_all() : p_mcd_flush_all();
            break;
        case 2:
	    mcdctl_user_enabled = (mcdctl_user_enabled) ? 0:1;
	    break;
        case 3:
            ret = increase_shared_page(1);
	    break;
        default:
            break;
        }
        break;
    default:
        break;
    }

    n += scnprintf(info, BSIZE, "Params is set to (mem (%u, %u), ssd (%u, %u))", sd.mem.alpha, sd.mem.beta, sd.ssd.alpha, sd.ssd.beta);
    mcd_copy_to_guest_buf_offset(buf, 0, info, n);

    return ret;
}

LOCAL long mcd_display(XEN_GUEST_HANDLE(mcd_display_stats_t) arg)
{
    mcd_display_stats_t disp;

    if( copy_from_guest(&disp, arg, 1) ) 
        return -ERR_XENFAULT;

    display_stats(disp.domid, disp.buf, disp.buf_len, disp.option);

    return -ERR_NO;
}

LOCAL long mcd_memsize(XEN_GUEST_HANDLE(mcd_memsize_t) arg)
{
    mcd_memsize_t memsize;

    if( copy_from_guest(&memsize, arg, 1) ) 
        return -ERR_XENFAULT;

    set_memsize(memsize.memsize, memsize.buf, memsize.buf_len);

    return -ERR_NO;
}

LOCAL long mcd_weight(XEN_GUEST_HANDLE(mcd_weight_t) arg)
{
    mcd_weight_t weight;

    if( copy_from_guest(&weight, arg, 1) ) 
        return -ERR_XENFAULT;

    set_weight(weight.domid, weight.weight, weight.buf, weight.buf_len);

    return -ERR_NO;
}

LOCAL long mcd_cache_mode(XEN_GUEST_HANDLE(mcd_cache_mode_t) arg)
{
    mcd_cache_mode_t mode;

    if( copy_from_guest(&mode, arg, 1) ) 
        return -ERR_XENFAULT;

    set_cache_mode(mode.which, mode.cache_mode, mode.buf, mode.buf_len);

    return -ERR_NO;
}

LOCAL int mcd_param(XEN_GUEST_HANDLE(mcd_param_t) arg)
{
    mcd_param_t param;
    int ret = -ERR_NO;

    if( copy_from_guest(&param, arg, 1) ) 
        return -ERR_XENFAULT;

    ret = set_param(param.which, param.mode, param.param, param.buf, param.buf_len);

    return ret;
}

// fn = get, put, remove, check
/*
#define mcd_sub_call(opt, fn, md, op) \
        ( (opt) == MCDOPT_private ) ? p_mcd_ ## fn ((md), (op)) : s_mcd_ ## fn ((md), (op))
*/

#define mcd_sub_call(opt, fn, md, op) \
        ( (cache_mode) != MCD_CACHE_MODE_SHARED ) ? p_mcd_ ## fn ((md), (op)) : s_mcd_ ## fn ((md), (op))

LOCAL long mcd_get(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, get, mcd_dom, &op);
}

LOCAL long mcd_stat_get(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;
    char buf[128];
    int size;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    // do here...
    size = snprintf(buf, 128, "%lu,%lu", get_tot_mem_size(), get_free_mem_size());

    printk("val_size = %d, size = %d, output = %s\n", op.val_size, size, buf);

    if ( size > op.val_size ) return -ERR_MCDFAULT;
    copy_to_guest(guest_handle_cast(op.r_val_size, void), &size, sizeof(uint32_t));
    copy_to_guest(guest_handle_cast(op.val, void), buf, size);

    return -ERR_NO;
}

LOCAL long mcd_put(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, put, mcd_dom, &op);
}

LOCAL long mcd_remove(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, remove, mcd_dom, &op);
}

LOCAL long mcd_check(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, check, mcd_dom, &op);
}

LOCAL long mcd_flush(mcd_domain_t *mcd_dom, XEN_GUEST_HANDLE(mcd_arg_t) arg)
{
    mcd_arg_t op;

    if ( copy_from_guest(&op, arg, 1) ) {
        printk("copy_from_gest failed\n");
        return -ERR_XENFAULT;
    }

    return mcd_sub_call(op.option, flush, mcd_dom, &op);
}

/*------------------------------------------------------------------------------
 - Export Functions
 ------------------------------------------------------------------------------*/

EXPORT long do_mcd_op(unsigned long cmd, XEN_GUEST_HANDLE(void) arg)
{
    mcd_domain_t *mcd_dom = mcd_get_mcd_domain_from_current();
    int rc = 0;

    if( !mcd_enabled_flag )
        return -ERR_NOMCD;

    if( mcd_dom != NULL && mcd_dom_is_dying(mcd_dom) )
        return -ERR_XENFAULT;

    /*
     * Memcached Operations
     */
    switch( cmd )
    {
    case XENMCD_cache_get:
        rc = mcd_get(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_stat_get:
        rc = mcd_stat_get(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_cache_put:
        //if ( (rand_val(0) % 100) <= ssd_usage_rate_percent ) usleep(ssd_w_latency_us); // TODO test function
        rc = mcd_put(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_cache_remove:
        rc = mcd_remove(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_cache_check:
    case XENMCD_cache_getsize:
        rc = mcd_check(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_cache_flush:
        rc = mcd_flush(mcd_dom, guest_handle_cast(arg, mcd_arg_t));
        break;

    case XENMCD_display_stats:
        rc = mcd_display(guest_handle_cast(arg, mcd_display_stats_t));
        break;

    case XENMCD_memsize:
        rc = mcd_memsize(guest_handle_cast(arg, mcd_memsize_t));
        break;

    case XENMCD_weight:
        rc = mcd_weight(guest_handle_cast(arg, mcd_weight_t));

        #ifdef MCDCTL_TESTING
        // XXX comment out when it is done
        mcdctl_fake_request_xen_to_dom0();
        #endif
        break;

    case XENMCD_cache_mode:
        rc = mcd_cache_mode(guest_handle_cast(arg, mcd_cache_mode_t));
        break;

    case XENMCD_param:
        rc = mcd_param(guest_handle_cast(arg, mcd_param_t));

        #ifdef MCDCTL_TESTING
        // XXX comment out when it is done
        mcdctl_fake_request_dom0_to_xen();
        #endif
        break;

    case XENMCD_mcdctl_op:
        rc = mcdctl_op(guest_handle_cast(arg, xen_mcdctl_t));
        break;

    case XENMCD_hypercall_test:
        printk(KERN_INFO "hypercall success...\n");
        break;

    default:
        break;
    }

    // Other works
    switch( cmd )
    {
    // Proportion can be changed...
    case XENMCD_cache_put:
    case XENMCD_cache_remove:
    case XENMCD_cache_flush:
    case XENMCD_memsize:
    case XENMCD_weight:
        dom_usage_update_all();

        break;
    case XENMCD_cache_check:
    case XENMCD_cache_get:

#define NSEC_TO_SEC(_nsec)      ((_nsec) / 1000000000ULL)
        /*
        if ( NSEC_TO_SEC(NOW() - mcd_dom->reset) > reset_time ) {
            mcd_dom->reset = NOW();
            RESET_HITRATE(mcd_dom);
        }
        */

        switch ( rc ) {
        case RET_GET_MEM:
        case RET_GET_SSD: // TODO
            HIT(mcd_dom);
            break;
        case -ERR_NO:
        default:
            MISS(mcd_dom);
            break;
        }
        SET_HITRATE(mcd_dom); // missrate = 100 - hitrate;
        break;
    case XENMCD_cache_getsize:
    case XENMCD_display_stats:
    case XENMCD_hypercall_test:
    default:
        break;
    }

    return rc;
}

EXPORT void minf_get_lock(void)
{
    mspin_lock(&minf_get_lock_v);
}

EXPORT void minf_get_unlock(void)
{
    mspin_unlock(&minf_get_lock_v);
}

EXPORT void minf_put_lock(void)
{
    mspin_lock(&minf_put_lock_v);
}

EXPORT void minf_put_unlock(void)
{
    mspin_unlock(&minf_put_lock_v);
}

EXPORT int mcd_domain_create(domid_t domid)
{
    // TODO we use some of hypervisor heap memory... needs to take account
    mcd_domain_t *mcd_dom = mcd_malloc(sizeof(mcd_domain_t), __alignof__(mcd_domain_t), domid);

    if( mcd_dom == NULL ) {
        printk(KERN_INFO "mcd_malloc failed.. OOM for domain(%d)\n", domid);
        goto fail;
    }
    memset(mcd_dom, 0, sizeof(mcd_domain_t));

    mcd_dom->domid = domid;
    mspin_lock_init(&mcd_dom->lock);
    mspin_lock_init(&mcd_dom->mcdctl_lock);
    if( !attach_mcd(mcd_dom, domid) ) {
        printk(KERN_ERR "attach_mcd failed for dom(%d)\n", domid);
        goto fail;
    }

    mcd_dom->nr_avail = 0;
    mcd_dom->nr_used_pages = 0;
    mcd_dom->nr_used_bytes = 0;
    mcd_dom->nr_keys = 0;
    mcd_dom->rec_cost_avg = 0;

    if ( domid > 0 ) {
        mcd_dom->weight = DOM_WEIGHT_DEFAULT;
        total_weight += DOM_WEIGHT_DEFAULT;
    } else {
        mcd_dom->weight = 0; // dom0
    }

    //printk("total_weight = %d\n", total_weight);

    mcd_dom->reset = NOW();
    mcd_dom->get_succ = 1;
    mcd_dom->get_fail = 1;
    mcd_dom->hitrate = 0;

    mspin_lock(&g_mcd_domain_list_lock);
    list_add_tail(&mcd_dom->mcd_list, &g_mcd_domain_list);
    INIT_LIST_HEAD(&mcd_dom->mcd_hash_list_head);
    INIT_LIST_HEAD(&mcd_dom->mcdctl_hash_list_head);
    mspin_unlock(&g_mcd_domain_list_lock);

    if ( domid > 0 ) 
        dom_weight_update();

    /* mcdctl */
    minf_page_dom_create(domid);

    return -ERR_NO;

fail:
    mcd_domain_free(mcd_dom);
    return -ERR_XENFAULT;
}

EXPORT void mcd_destroy(void *p)
{
    struct domain *dom = (struct domain*)p;

    if( dom == NULL ) return;

    if ( !domain_is_dying(dom) ) {
        printk("mcd: mcd_destroy can only destroy dying client\n");
        return;
    }

    /* mcdctl */
    minf_page_dom_destroy(dom->domain_id);

    total_weight -= ((mcd_domain_t*)dom->mcd)->weight;
    mcd_domain_free((mcd_domain_t*)dom->mcd);

    dom_weight_update();
}

EXPORT void mcd_mem_upt_trap(struct domain *dom)
{
    mcd_domain_t *mcd_dom;

    if( dom == NULL ) return;

    mcd_dom = (mcd_domain_t*) dom->mcd;

    mspin_lock(&mcd_dom->lock);
    if( dom->max_pages == ~0U ) // dom0
        mcd_dom->nr_avail = 0;
    else
        mcd_dom->nr_avail = dom->max_pages - dom->tot_pages;
    mspin_unlock(&mcd_dom->lock);

    if( mcd_dom->nr_avail < 0 || mcd_dom->nr_avail > dom->max_pages ) {
        printk("mcd: mem mgmt is wrong, so disable to use it\n");
        mcd_dom->nr_avail = 0;
    }
}

/*------------------------------------------------------------------------------
 - Trap Functions
 ------------------------------------------------------------------------------*/

/*
 * VM wants to use more memory, so determine whether we have to give back
 */
EXPORT void mcd_mem_inc_trap(struct domain *dom, unsigned int nr_pages) 
{
// TODO algorithm
/*
    mcd_domain_t *mcd_dom;

    if( dom == NULL ) return;
*/
}

/*
 * VM wants to surrender some memory. Do nothing
 */
EXPORT void mcd_mem_dec_trap(struct domain *dom, unsigned int nr_pages)
{
// TODO algorithm
/*
    mcd_domain_t *mcd_dom = (mcd_domain_t*) dom->mcd;

    if( dom == NULL ) return;
*/
}

/*------------------------------------------------------------------------------
 - Initial Functions
 ------------------------------------------------------------------------------*/

LOCAL int __init mcd_start(void)
{
    mcd_data.pri_nr_pages = 0;
    mcd_data.pri_nr_bytes = 0;
    mcd_data.shr_nr_pages = 0;
    mcd_data.shr_nr_bytes = 0;
    mcd_data.pri_nr_keys = 0;
    mcd_data.shr_nr_keys = 0;

    sd.mem.alpha = SCORE_MEM_ALPHA_DEF;
    sd.mem.beta = SCORE_MEM_BETA_DEF;
    sd.mem.query_max = 1;
    sd.mem.val_max = 1;

    // TO BE USED
    sd.ssd.alpha = SCORE_SSD_ALPHA_DEF;
    sd.ssd.beta = SCORE_SSD_BETA_DEF;
    sd.ssd.query_max = 1;
    sd.ssd.val_max = 1;

    max_mem_bytes = 0;

    INIT_LIST_HEAD(&g_mcd_domain_list);

    #ifdef MCD_LINEAR_PROBE
    INIT_LIST_HEAD(&g_mcd_shared_hash_list);
    mspin_lock_init(&g_mcd_shared_hash_lock);
    #elif defined(MCD_HASH_CHAINING)
    {
        int i;
        for(i = 0; i < MAX_LIST_HEAD; i++) {
            INIT_LIST_HEAD(&g_mcd_shared_hash_list[i]);
            mspin_lock_init(&g_mcd_shared_hash_lock[i]);
        }
    }
    #endif

    mspin_lock_init(&mcd_data.lock);
    mspin_lock_init(&g_mcd_domain_list_lock);

    mcdctl_start();

    return -ERR_NO;
}

LOCAL int __init init_mcd(void)
{
    // only if tmem enabled
#if 0
    if ( !tmh_enabled() )
        return -ERR_NOTMEM;
#endif

    if ( !use_mcd() )
        return -ERR_NOMCD;

    if ( mcd_start() ) {
        printk("mcd: initialized\n"); 
        mcd_enabled_flag = 1;
    }
    else
        printk("mcd: initialization FAILED\n");

    // TODO ssd test
    rand_val(777);

    return -ERR_NO;
}
__initcall(init_mcd);

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
