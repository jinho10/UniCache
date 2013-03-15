/******************************************************************************
 * mcdctl.c
 *
 * mortar control
 *
 * Copyright (c) 2012, Jinho Hwang, The George Washington University
 */

#include <xen/mcd.h>
#include <xen/tmem.h>
#include <xen/rbtree.h>
#include <xen/radix-tree.h>
#include <xen/list.h>
#include <xen/time.h>
#include <asm/domain.h>
#include <asm/p2m.h>
#include <xen/event.h>

/* for public/io/ring.h macros */
#define xen_mb()   mb()
#define xen_rmb()  rmb()
#define xen_wmb()  wmb()

#define mcd_event_ring_lock_init(_d)  spin_lock_init(&(_d)->mcd_event.ring_lock)
#define mcd_event_ring_lock(_d)       spin_lock(&(_d)->mcd_event.ring_lock)
#define mcd_event_ring_unlock(_d)     spin_unlock(&(_d)->mcd_event.ring_lock)

//#define MY_TRACE_ON
#ifdef MY_TRACE_ON
#define my_trace() printk("%s (%d) \n", __FUNCTION__, __LINE__);
#else
#define my_trace()
#endif

#define MCD_EVENT_RING_THRESHOLD 4

int mcdctl_enabled = 0;

LOCAL int mcdctl_page_out(mcd_hashT_t *mh, mcd_event_request_t *rsp);
LOCAL int mcdctl_page_in(mcd_hashT_t *mh, mcd_event_request_t *rsp);
//void mcd_event_unpause_vcpus(struct domain *d);

LOCAL int mcd_event_enable(struct domain *d, mfn_t ring_mfn, mfn_t shared_mfn)
{
    int rc = 0;

    /* Map ring and shared pages */
    d->mcd_event.ring_page = map_domain_page(mfn_x(ring_mfn));
    if ( d->mcd_event.ring_page == NULL )
        goto err;

    d->mcd_event.shared_page = map_domain_page(mfn_x(shared_mfn));
    if ( d->mcd_event.shared_page == NULL )
        goto err_ring;

    // TODO check this... whether we need this or just using ring for notification...
    // TODO however, ring notification should have some delay incurred before receiving...

    /* Allocate event channel */
    rc = alloc_unbound_xen_event_channel(d->vcpu[0], current->domain->domain_id);
    if ( rc < 0 )
        goto err_shared;

    // XXX since we use data as a buffer.. this is the way to avoid future conflict
    memcpy(((mcd_event_shared_page_t *)d->mcd_event.shared_page)->data, &rc, sizeof(int));
    d->mcd_event.xen_port = rc;

    /* Prepare ring buffer */
    FRONT_RING_INIT(&d->mcd_event.front_ring,
                    (mcd_event_sring_t *)d->mcd_event.ring_page,
                    PAGE_SIZE);

printk("ring buffer size = %d \n", (&(d->mcd_event.front_ring))->nr_ents);

    mcd_event_ring_lock_init(d);

    /* Wake any VCPUs paused for memory events */
    //mcd_event_unpause_vcpus(d);

    init_mcdctl();

    return 0;

 err_shared:
    unmap_domain_page(d->mcd_event.shared_page);
    d->mcd_event.shared_page = NULL;
 err_ring:
    unmap_domain_page(d->mcd_event.ring_page);
    d->mcd_event.ring_page = NULL;
 err:
    return 1;
}

LOCAL int mcd_event_disable(struct domain *d)
{
    unmap_domain_page(d->mcd_event.ring_page);
    d->mcd_event.ring_page = NULL;

    unmap_domain_page(d->mcd_event.shared_page);
    d->mcd_event.shared_page = NULL;

    exit_mcdctl();

    return 0;
}

LOCAL void mcd_event_put_request(struct domain *d, mcd_event_request_t *req)
{
    mcd_event_front_ring_t *front_ring;
    RING_IDX req_prod;

my_trace()

    mcd_event_ring_lock(d);

    front_ring = &d->mcd_event.front_ring;
    req_prod = front_ring->req_prod_pvt;

    /* Copy request */
    memcpy(RING_GET_REQUEST(front_ring, req_prod), req, sizeof(*req));
    req_prod++;

    /* Update ring */
    front_ring->req_prod_pvt = req_prod;
    RING_PUSH_REQUESTS(front_ring);

    mcd_event_ring_unlock(d);

my_trace()

    // TODO check whether I have to use notifying through channel or just ring.. ???
    notify_via_xen_event_channel(d, d->mcd_event.xen_port);
}

LOCAL void mcd_event_get_response(struct domain *d, mcd_event_response_t *rsp)
{
    mcd_event_front_ring_t *front_ring;
    RING_IDX rsp_cons;

    mcd_event_ring_lock(d);

    front_ring = &d->mcd_event.front_ring;
    rsp_cons = front_ring->rsp_cons;

    /* Copy response */
    memcpy(rsp, RING_GET_RESPONSE(front_ring, rsp_cons), sizeof(*rsp));
    rsp_cons++;

    /* Update ring */
    front_ring->rsp_cons = rsp_cons;
    front_ring->sring->rsp_event = rsp_cons + 1;

    mcd_event_ring_unlock(d);
}

void dump(char* data, int size)
{
    int i;
    for(i = 0; i < size; i++) {
        printk("%c", data[i]);
    }
    printk("\n");
}

// TODO fix this for testing... pingpoing... be careful this time...
LOCAL int mcd_event_resume(struct domain *d)
{
    int rc = 0;
    mcd_event_response_t rsp;

    mcd_event_get_response(d, &rsp);

my_trace()

    if ( rsp.flags == 0 ) {
        printk("something is wrong.. flags should not be zero\n");
        goto final;
    }

    if_set(rsp.flags, MCD_EVENT_OP_FLAGS_ERROR) {
        printk("something is wrong.. flags indicates ERROR \n");
        goto final;
    }

    if_set(rsp.flags, MCD_EVENT_OP_FLAGS_END) {
        goto end;
    }

    // only here to notify mcd to continue
    if_set(rsp.flags, MCD_EVENT_OP_FLAGS_FINAL) {
        #ifdef MCDCTL_TESTING
        xfree((void*)rsp.mcd_data); // XXX testing... to be deleted.. managed by mcd
        printk("------ if this prints, it is wrong operation....\n");
        #endif
        goto final;
    } 

my_trace()

    switch ( rsp.type ) {
    case MCD_EVENT_OP_PUT:
        rc = mcdctl_page_out((mcd_hashT_t*)rsp.mcd_data, &rsp);
        if ( rc != 0 ) goto final;
        break;
    case MCD_EVENT_OP_GET:
        rc = mcdctl_page_in((mcd_hashT_t*)rsp.mcd_data, &rsp);
        if ( rc != 0 ) goto final;
        break;
    }

my_trace()

    return 0;

final:

my_trace()

    switch ( rsp.type ) {
    case MCD_EVENT_OP_PUT:
        minf_put_unlock();

        //unpause((struct vcpu*)rsp.vcpu);

        break;
    case MCD_EVENT_OP_GET:
        minf_get_unlock();
        break;
    }
    cpu_relax(); // suspicious

my_trace()
end:

    return 0;
}

/*
void mcd_event_unpause_vcpus(struct domain *d)
{
    struct vcpu *v;

    for_each_vcpu ( d, v )
        if ( test_and_clear_bit(_VPF_mcd_event, &v->pause_flags) )
            vcpu_wake(v);
}
*/

/*
void mcd_event_mark_and_pause(struct vcpu *v)
{
    set_bit(_VPF_mcd_event, &v->pause_flags);
    vcpu_sleep_nosync(v);
}
*/

// TODO check this function whether it affects any performance...
LOCAL int mcd_event_check_ring(struct domain *d)
{
    //struct vcpu *curr = current;
    int free_requests;
    int ring_full;

    if ( !d->mcd_event.ring_page )
        return -1;

    mcd_event_ring_lock(d);

    free_requests = RING_FREE_REQUESTS(&d->mcd_event.front_ring);
    if ( unlikely(free_requests < 2) )
    {
        gdprintk(XENLOG_INFO, "free request slots: %d\n", free_requests);
        WARN_ON(free_requests == 0);
    }
    ring_full = free_requests < MCD_EVENT_RING_THRESHOLD ? 1 : 0;

    /* XXX this should not happen...
    if ( (curr->domain->domain_id == d->domain_id) && ring_full )
    {
        set_bit(_VPF_mem_event, &curr->pause_flags);
        vcpu_sleep_nosync(curr);
    }
    */

    mcd_event_ring_unlock(d);

    return ring_full;
}

LOCAL int mcd_event_op(struct domain *d, mcd_event_op_t *mec, XEN_GUEST_HANDLE(void) u_domctl)
{
    int rc;

    /* XXX yes, I do.. so what?
    if ( unlikely(d == current->domain) )
    {
        gdprintk(XENLOG_INFO, "Tried to do a memory paging op on itself.\n");
        return -EINVAL;
    }
    */

    if ( unlikely(d->is_dying) )
    {
        gdprintk(XENLOG_INFO, "Ignoring memory paging op on dying domain %u\n",
                 d->domain_id);
        return 0;
    }

    if ( unlikely(d->vcpu == NULL) || unlikely(d->vcpu[0] == NULL) )
    {
        gdprintk(XENLOG_INFO,
                 "Memory paging op on a domain (%u) with no vcpus\n",
                 d->domain_id);
        return -EINVAL;
    }

    /* TODO: XSM hook */
#if 0
    rc = xsm_mcd_event_control(d, mec->op);
    if ( rc )
        return rc;
#endif

    rc = -ENOSYS;

    switch( mec->op )
    {
    case MCD_EVENT_OP_ENABLE:
    {
        struct domain *dom_mcd_event = current->domain;
        struct vcpu *v = current;
        unsigned long ring_addr = mec->ring_addr;
        unsigned long shared_addr = mec->shared_addr;
        l1_pgentry_t l1e;
        unsigned long gfn;
        p2m_type_t p2mt;
        mfn_t ring_mfn;
        mfn_t shared_mfn;

        /* Only one xenpaging at a time. If xenpaging crashed,
            * the cache is in an undefined state and so is the guest
            */
        rc = -EBUSY;
        if ( d->mcd_event.ring_page )
            break;

        /* Currently only EPT is supported */
        /*
        rc = -ENODEV;
        if ( !(hap_enabled(d) &&
                (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL)) )
            break;
        */

        /* Get MFN of ring page */
        guest_get_eff_l1e(v, ring_addr, &l1e);
        gfn = l1e_get_pfn(l1e);
        ring_mfn = gfn_to_mfn(p2m_get_hostp2m(dom_mcd_event), gfn, &p2mt);

        rc = -EINVAL;
        if ( unlikely(!mfn_valid(mfn_x(ring_mfn))) )
            break;

        /* Get MFN of shared page */
        guest_get_eff_l1e(v, shared_addr, &l1e);
        gfn = l1e_get_pfn(l1e);
        shared_mfn = gfn_to_mfn(p2m_get_hostp2m(dom_mcd_event), gfn, &p2mt);

        rc = -EINVAL;
        if ( unlikely(!mfn_valid(mfn_x(shared_mfn))) )
            break;

        rc = -EINVAL;
        if ( mcd_event_enable(d, ring_mfn, shared_mfn) != 0 )
            break;

        rc = 0;

        mcdctl_enabled = 1;
    }
    break;

    case MCD_EVENT_OP_RESUME:
    {
        rc = mcd_event_resume(d);
    } break;

    case MCD_EVENT_OP_DISABLE:
    {
        rc = mcd_event_disable(d);
        mcdctl_enabled = 0;
    } break;

    default:
        rc = -ENOSYS;
        break;
    }

    return rc;
}

int mcdctl_op(XEN_GUEST_HANDLE(xen_mcdctl_t) u_mcdctl)
{
    int ret = 0;
    struct xen_mcdctl curop, *op = &curop;

    if ( copy_from_guest(op, u_mcdctl, 1) )
        return -EFAULT;

    /* should be done super function
    if ( op->interface_version != XEN_MCD_VERSION )
        return -EACCES;
    */

    switch ( op->cmd ) 
    {
    case XEN_MCDCTL_mcd_event_op:
    {
        struct domain *d;
 
        ret = -ESRCH;
        //d = rcu_lock_domain_by_id(mcdctl->domain);
        d = get_domain_by_id(op->domain);
        if ( d != NULL )
        {
            ret = mcd_event_op(d, &op->u.mcd_event_op,
                               guest_handle_cast(u_mcdctl, void));
            //rcu_unlock_domain(d);
            copy_to_guest(u_mcdctl, op, 1);
        } 
    } break;
    default:
    {
        // do nothing
    } break;
    }

    return ret;
}

// XXX I should manage this well...
LOCAL int mcdctl_page_out(mcd_hashT_t *mh, mcd_event_request_t *rsp)
{
    struct domain *d = get_domain_by_id(0);
    mcd_event_request_t req;

my_trace()

    if ( mcd_event_check_ring(d) )
        return -1;

    memset(&req, 0, sizeof(mcd_event_request_t));

    req.type = MCD_EVENT_OP_PUT;
    req.domain = mh->domain; // XXX this must be reason to crash..
    //req.v = (void*)current;

    if ( rsp == NULL ) {
        st_bit(req.flags, MCD_EVENT_OP_FLAGS_NEW);
    } else {
        req.accsize = rsp->accsize;
        req.fd = rsp->fd;
    }

    req.mcd_data = (void*)mh;
    req.hash = mh->hash;
    req.totsize = (mh->key_size + mh->val_size);

    if ( (req.totsize - req.accsize) > PAGE_SIZE ) {

        memcpy(((mcd_event_shared_page_t *)d->mcd_event.shared_page)->data, (mh->key_val + req.accsize), PAGE_SIZE);

        req.cursize = PAGE_SIZE;

        // XXX think about the error happens in user size.. needs to be reliable
        req.accsize += PAGE_SIZE; 

        st_bit(req.flags, MCD_EVENT_OP_FLAGS_CONT);

        if ( (req.totsize - req.accsize) == 0 )
            st_bit(req.flags, MCD_EVENT_OP_FLAGS_FINAL);

    } else {

        memcpy(((mcd_event_shared_page_t *)d->mcd_event.shared_page)->data, (mh->key_val + req.accsize), (req.totsize - req.accsize));

        req.cursize = req.totsize - req.accsize;
        req.accsize += (req.totsize - req.accsize); // should be same as totsize

        // debugging...
        /*
        if ( req.totsize != req.accsize )
            printk("something is wrong in req/rsp management %d != %d \n", req.totsize, req.accsize);
        */

        st_bit(req.flags, MCD_EVENT_OP_FLAGS_FINAL);
    }

    mcd_event_put_request(d, &req);

my_trace()

    return 0;
}

LOCAL int mcdctl_page_in(mcd_hashT_t *mh, mcd_event_request_t *rsp)
{
    struct domain *d = get_domain_by_id(0);
    mcd_event_request_t req;

    if ( mcd_event_check_ring(d) )
        return -1;

    memset(&req, 0, sizeof(mcd_event_request_t));

    req.type = MCD_EVENT_OP_GET;
    req.domain = mh->domain;
    req.hash = mh->hash;
    req.totsize = (mh->key_size + mh->val_size);
    req.mcd_data = (void*)mh;

    if ( rsp == NULL ) {
        // XXX let's not allocate here... should be done by mcd.c
        /*
        mh->key_val = mcd_malloc((mh->key_size + mh->val_size), 0, mh->domain);
        if ( mh->key_val == NULL ) {
            printk("no memory for page_in\n");
            return -1;
        }
        */

        st_bit(req.flags, MCD_EVENT_OP_FLAGS_NEW);
    } else {

        memcpy((mh->key_val + rsp->accsize), ((mcd_event_shared_page_t *)d->mcd_event.shared_page)->data, rsp->cursize);

        req.accsize += (rsp->accsize + rsp->cursize);
        req.fd = rsp->fd;
    }

    mcd_event_put_request(d, &req);

    return 0;
}

LOCAL int mcdctl_page_del(domid_t domid, uint32_t hash) 
{
    struct domain *d = get_domain_by_id(0);
    mcd_event_request_t req;

    if ( mcd_event_check_ring(d) )
        return -1;

    memset(&req, 0, sizeof(mcd_event_request_t));

    req.type = MCD_EVENT_OP_DEL;
    req.domain = domid;
    req.hash = hash;

    mcd_event_put_request(d, &req);

    return 0;
}

LOCAL int mcdctl_page_dom_create(domid_t domid)
{
    struct domain *d = get_domain_by_id(0);
    mcd_event_request_t req;

    if ( mcd_event_check_ring(d) )
        return -1;

    memset(&req, 0, sizeof(mcd_event_request_t));

    req.type = MCD_EVENT_OP_DOM_CREATE;
    req.domain = domid;

    mcd_event_put_request(d, &req);

    return 0;
}

LOCAL int mcdctl_page_dom_destroy(domid_t domid)
{
    struct domain *d = get_domain_by_id(0);
    mcd_event_request_t req;

    if ( mcd_event_check_ring(d) )
        return -1;

    memset(&req, 0, sizeof(mcd_event_request_t));

    req.type = MCD_EVENT_OP_DOM_DESTROY;
    req.domain = domid;

    mcd_event_put_request(d, &req);

    return 0;
}

LOCAL int mcdctl_page_dom_flush(domid_t domid)
{
    struct domain *d = get_domain_by_id(0);
    mcd_event_request_t req;

    if ( mcd_event_check_ring(d) )
        return -1;

    memset(&req, 0, sizeof(mcd_event_request_t));

    req.type = MCD_EVENT_OP_DOM_FLUSH;
    req.domain = domid;

    mcd_event_put_request(d, &req);

    return 0;
}


LOCAL int mcdctl_page_flush(void)
{
    struct domain *d = get_domain_by_id(0);
    mcd_event_request_t req;

    if ( mcd_event_check_ring(d) )
        return -1;

    memset(&req, 0, sizeof(mcd_event_request_t));

    req.type = MCD_EVENT_OP_FLUSH_ALL;

    mcd_event_put_request(d, &req);

    return 0;
}

/* 
 * Interfaces to MCD core module
 */ 
int minf_page_out(mcd_hashT_t *mh)
{
    int rc = 0;
    if ( !mcdctl_enabled || mh == NULL ) return -1;

    minf_put_lock();

    rc = mcdctl_page_out(mh, NULL);
    if ( rc != 0 )
        minf_put_unlock();

    return rc;
}

int minf_page_in(mcd_hashT_t *mh)
{
    int rc = 0;
    if ( !mcdctl_enabled || mh == NULL ) return -1;

    minf_get_lock();
    
    rc = mcdctl_page_in(mh, NULL);
    if ( rc != 0 )
        minf_get_unlock();

    return rc;
}

int minf_page_del(domid_t domid, uint32_t hash) 
{
    int rc = 0;
    if ( !mcdctl_enabled ) return -1;

    rc = mcdctl_page_del(domid, hash);
    if ( rc != 0 ) { /* do nothing */ }

    return rc;
}

int minf_page_dom_create(domid_t domid)
{
    int rc = 0;
    if ( !mcdctl_enabled ) return -1;

    mcdctl_page_dom_create(domid);
    if ( rc != 0 ) { /* do nothing */ }

    return rc;
}

int minf_page_dom_flush(domid_t domid)
{
    int rc = 0;
    if ( !mcdctl_enabled ) return -1;

    mcdctl_page_dom_flush(domid);
    if ( rc != 0 ) { /* do nothing */ }

    return rc;
}

int minf_page_dom_destroy(domid_t domid)
{
    int rc = 0;
    if ( !mcdctl_enabled ) return -1;

    mcdctl_page_dom_destroy(domid);
    if ( rc != 0 ) { /* do nothing */ }

    return rc;
}

int minf_page_flush(void)
{
    int rc = 0;
    if ( !mcdctl_enabled ) return -1;

    mcdctl_page_flush();
    if ( rc != 0 ) { /* do nothing */ }

    return rc;
}


#ifdef MCDCTL_TESTING
/*
 * Test Functions
 */
uint32_t hash;
// XXX testing function
// XXX if we call _xmallc(576) twice, xen dies due to xmem_alloc function used when < PAGE_SIZE
int mcdctl_fake_request_xen_to_dom0(void)
{
    mcd_hashT_t *mh;
    int key_size = 96;
    int val_size = 4000;
    int size = sizeof(mcd_hashT_t) + key_size + val_size;
    int i;

    if ( (size % MEM_ALIGN) > 0 ) {
        size += (MEM_ALIGN - (size % MEM_ALIGN));
    }

    mh = (mcd_hashT_t *) mcd_malloc(size, 0, 1);
    memset(mh, 0, sizeof(mcd_hashT_t));

    if ( mh == NULL ) {
        printk("no memory space \n");
        return -1;
    } else {
        printk("mcd_malloc allocated \n");
    }

    mh->domain = 1;
    //mh->flags |= MCD_DATA_IN_CACHE; // initial setting
    mh->nr_pages = (size / PAGE_SIZE) + ((size % PAGE_SIZE > 0) ? 1:0);
    mh->nr_bytes = size;
    mh->ref = 0;
    mh->put_time = NOW(); // ns
    mh->key_size = key_size;
    mh->val_size = val_size;

    mh->key_val = ((char*)mh) + sizeof(mcd_hashT_t); // TODO confirm

    // fill data
    for(i=0; i < (key_size + val_size); i++) {
        mh->key_val[sizeof(mcd_hashT_t) + i] = (i % 128);
    }

    mh->hash = getHash(mh->key_val, key_size);
    hash = mh->hash;

    //mcdctl_page_out(mh, NULL);
    minf_page_out(mh);

    printk("[%d:%d]................. locked... ... minf_page_out started \n", current->domain->domain_id, current->vcpu_id);

    // wait here until this minf_page_out finished...
    minf_put_lock();
    minf_put_unlock();

    printk("[%d:%d]................. unlocked... ... go back to hypercall \n", current->domain->domain_id, current->vcpu_id);

    return 0;
}

int mcdctl_fake_request_dom0_to_xen(void)
{
    mcd_hashT_t *mh;
    int key_size = 96;
    int val_size = 4000;
    int size = sizeof(mcd_hashT_t) + key_size + val_size;
    //int i;

    if ( (size % MEM_ALIGN) > 0 ) {
        size += (MEM_ALIGN - (size % MEM_ALIGN));
    }

    mh = (mcd_hashT_t *) mcd_malloc(size, 0, 1);
    memset(mh, 0, sizeof(mcd_hashT_t));

    if ( mh == NULL ) {
        printk("no memory space \n");
        return -1;
    } else {
        printk("mcd_malloc allocated \n");
    }

    mh->domain = 1;
    //mh->flags |= MCD_DATA_NOT_IN_CACHE;
    mh->nr_pages = (size / PAGE_SIZE) + ((size % PAGE_SIZE > 0) ? 1:0);
    mh->nr_bytes = size;
    mh->ref = 0;
    mh->put_time = 0; // ns
    mh->key_size = key_size;
    mh->val_size = val_size;

    mh->key_val = ((char*)mh) + sizeof(mcd_hashT_t); // TODO confirm

    // fill data
    /*
    for(i=0; i < (key_size + val_size); i++) {
        mh->key_val[sizeof(mcd_hashT_t) + i] = (i % 128);
    }
    */

    mh->hash = hash; // made already

    //mcdctl_page_in(mh, NULL);
    minf_page_in(mh);

    return 0;
}
#endif

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
