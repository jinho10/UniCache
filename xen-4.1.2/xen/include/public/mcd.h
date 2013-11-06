/******************************************************************************
 * mcd.h
 * 
 * Guest OS interface to Xen Memcached
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (c) 2004, K A Fraser
 */

#ifndef __XEN_PUBLIC_MCD_H__
#define __XEN_PUBLIC_MCD_H__

#include "xen.h"
#include "io/ring.h"

/* version of ABI */
#define XEN_MCD_VERSION         1

/* TODO modifying Special errno values */
#define EFROZEN                 1000
#define EEMPTY                  1001

/* bit operation */
#define if_set(_f, _b) if (_f & _b)
#define if_not_set(_f, _b) if (!(_f & _b))
#define cl_bit(_f, _b) (_f &= ~(_b))
#define st_bit(_f, _b) (_f |= _b)

typedef xen_pfn_t mcd_mfn_t;
typedef XEN_GUEST_HANDLE(void) mcd_va_t;

/*
 * Basic operations of Hypervisor Memcached
 */
#define XENMCD_cache_get        1
#define XENMCD_cache_put        2
#define XENMCD_cache_remove     3
#define XENMCD_cache_check      4
#define XENMCD_cache_getsize    5
#define XENMCD_cache_flush      6
#define XENMCD_stat_get         7

typedef struct mcd_arg {
#define MCDOPT_private          1
#define MCDOPT_shared           2
    uint32_t option;
    uint32_t key_size;
    uint32_t val_size;
    mcd_va_t r_val_size;
    mcd_va_t key;
    mcd_va_t val;
} mcd_arg_t;
DEFINE_XEN_GUEST_HANDLE(mcd_arg_t);

/*
 * Returns the output of statistics
 */
#define XENMCD_display_stats        100
typedef struct mcd_display_stats {
    uint32_t domid;
    uint32_t option;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_display_stats_t;
DEFINE_XEN_GUEST_HANDLE(mcd_display_stats_t);

/*
 * Controls
 */
#define XENMCD_memsize              101
typedef struct mcd_memsize {
    uint32_t memsize;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_memsize_t;
DEFINE_XEN_GUEST_HANDLE(mcd_memsize_t);

#define XENMCD_weight               102
typedef struct mcd_weight {
    uint32_t domid;
    uint32_t weight;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_weight_t;
DEFINE_XEN_GUEST_HANDLE(mcd_weight_t);

#define XENMCD_cache_mode           103
typedef struct mcd_cache_mode {
    /* 0: sharing, 1: replacement */
    uint32_t which;
    uint32_t cache_mode;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_cache_mode_t;
DEFINE_XEN_GUEST_HANDLE(mcd_cache_mode_t);

#define XENMCD_param                104
typedef struct mcd_param {
    uint32_t which;
    uint32_t mode;
    uint32_t param;
    uint32_t buf_len;
    mcd_va_t buf;
} mcd_param_t;
DEFINE_XEN_GUEST_HANDLE(mcd_param_t);

/*
 * Mcd Event
 */

#define XENMCD_mcdctl_op             200

#ifndef uint64_aligned_t
#define uint64_aligned_t uint64_t
#endif

struct mcd_event_op {
    uint32_t       op;
#define MCD_EVENT_OP_ENABLE     0
#define MCD_EVENT_OP_RESUME     1
#define MCD_EVENT_OP_DISABLE    2
#define MCD_EVENT_OP_ADD_SHARED_PAGE 3

    uint32_t       mode;

    /* OP_ENABLE */
    uint64_aligned_t shared_addr;  /* IN:  Virtual address of shared page */
    uint64_aligned_t ring_addr;    /* IN:  Virtual address of ring page */

    /* Other OPs */
    uint64_aligned_t gfn;          /* IN:  gfn of page being operated on */
};
typedef struct mcd_event_op mcd_event_op_t;
DEFINE_XEN_GUEST_HANDLE(mcd_event_op_t);


struct xen_mcdctl {
    uint32_t cmd;
#define XEN_MCDCTL_mcd_event_op     0
    uint32_t interface_version;     /* XEN_MCD_VERSION */
    domid_t  domain;
    union {
        struct mcd_event_op      mcd_event_op;
    } u;
};
typedef struct xen_mcdctl xen_mcdctl_t;
DEFINE_XEN_GUEST_HANDLE(xen_mcdctl_t);


/* Memory event type */
#define MEM_EVENT_TYPE_SHARED   0
#define MEM_EVENT_TYPE_PAGING   1
#define MEM_EVENT_TYPE_ACCESS   2

/* Memory event flags */
#define MEM_EVENT_FLAG_VCPU_PAUSED  (1 << 0)
#define MEM_EVENT_FLAG_DROP_PAGE    (1 << 1)

/* Reasons for the memory event request */
#define MEM_EVENT_REASON_UNKNOWN     0    /* typical reason */
#define MEM_EVENT_REASON_VIOLATION   1    /* access violation, GFN is address */
#define MEM_EVENT_REASON_CR0         2    /* CR0 was hit: gfn is CR0 value */
#define MEM_EVENT_REASON_CR3         3    /* CR3 was hit: gfn is CR3 value */
#define MEM_EVENT_REASON_CR4         4    /* CR4 was hit: gfn is CR4 value */
#define MEM_EVENT_REASON_INT3        5    /* int3 was hit: gla/gfn are RIP */
#define MEM_EVENT_REASON_SINGLESTEP  6    /* single step was invoked: gla/gfn are RIP */

typedef struct mcd_event_shared_page {
    char data[4096];
} mcd_event_shared_page_t;

typedef struct mcd_event_st {
    uint16_t type;
#define MCD_EVENT_OP_PUT            0
#define MCD_EVENT_OP_GET            1
#define MCD_EVENT_OP_DEL            2
#define MCD_EVENT_OP_DOM_CREATE     3
#define MCD_EVENT_OP_DOM_FLUSH      4
#define MCD_EVENT_OP_DOM_DESTROY    5
#define MCD_EVENT_OP_FLUSH_ALL      6
#define MCD_EVENT_REQ_ADD_SHARED_PAGE 11
    domid_t  domain;

    uint32_t flags;
#define MCD_EVENT_OP_FLAGS_NEW      (1 << 0)
#define MCD_EVENT_OP_FLAGS_CONT     (1 << 1)    /* in order to be safe */
#define MCD_EVENT_OP_FLAGS_FINAL    (1 << 2)
#define MCD_EVENT_OP_FLAGS_END      (1 << 3)
#define MCD_EVENT_OP_FLAGS_ERROR    (1 << 4)

    uint32_t hash;
    uint32_t totsize;   /* key + value */
    uint32_t accsize;   /* this is location info for buffer */
    uint32_t cursize;   /* real size in shared page */
    int      fd;        /* file descriptor */
    /*void*    vcpu;*/      /* unpause after paging in/out */
    void*    mcd_data;  /* get this back with response */
} mcd_event_request_t, mcd_event_response_t;
DEFINE_RING_TYPES(mcd_event, mcd_event_request_t, mcd_event_response_t);

/*
 * Test for Hypercall: Print to dmesg
 */
#define XENMCD_hypercall_test       1000

#endif /* __XEN_PUBLIC_MCD_H__ */
/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
