/******************************************************************************
 * xc_mcd.c
 *
 * Copyright (C) 2012 The George Washington University (Jinho Hwang)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "xc_private.h"
#include <xen/mcd.h>

static int do_mcd_op(xc_interface *xch, unsigned long cmd, void *op, int op_size) 
{
    int ret;
    DECLARE_HYPERCALL;
    DECLARE_HYPERCALL_BOUNCE(op, op_size, XC_HYPERCALL_BUFFER_BOUNCE_BOTH);

    if ( xc_hypercall_bounce_pre(xch, op) ) {
        PERROR("Could not bounce buffer for mcd op hypercall");
        return -EFAULT;
    }

    hypercall.op = __HYPERVISOR_mcd_op;
    hypercall.arg[0] = cmd;
    hypercall.arg[1] = HYPERCALL_BUFFER_AS_ARG(op);
    if ((ret = do_xen_hypercall(xch, &hypercall)) < 0) {
        DPRINTF("mcd operation failed -- need to"
                " rebuild the user-space tool set?\n");
    }
    xc_hypercall_bounce_post(xch, op);

    return ret;
}

int xc_mcd_display_stats(xc_interface *xch, domid_t domid, void *buf, uint32_t buf_len, int is_detail)
{             
    mcd_display_stats_t op;
    DECLARE_HYPERCALL_BOUNCE(buf, buf_len, XC_HYPERCALL_BUFFER_BOUNCE_OUT);
    int rc;
              
    op.domid = domid;
    op.option = is_detail;
    op.buf_len = buf_len;

    if ( buf == NULL )
        return -EINVAL;

    if ( xc_hypercall_bounce_pre(xch, buf) ) {    
        PERROR("Could not bounce buffer for mcd control hypercall");
        return -ENOMEM;
    }         
              
    set_xen_guest_handle(op.buf, buf);
    rc = do_mcd_op(xch, XENMCD_display_stats, &op, sizeof(op));
    xc_hypercall_bounce_post(xch, buf);
              
    return rc;
}    

int xc_mcd_memsize(xc_interface *xch, void *buf, uint32_t buf_len, int memsize)
{             
    mcd_memsize_t op;
    DECLARE_HYPERCALL_BOUNCE(buf, buf_len, XC_HYPERCALL_BUFFER_BOUNCE_OUT);
    int rc;
              
    op.memsize = memsize;
    op.buf_len = buf_len;

    if ( buf == NULL )
        return -EINVAL;

    if ( xc_hypercall_bounce_pre(xch, buf) ) {    
        PERROR("Could not bounce buffer for mcd control hypercall");
        return -ENOMEM;
    }         
              
    set_xen_guest_handle(op.buf, buf);
    rc = do_mcd_op(xch, XENMCD_memsize, &op, sizeof(op));
    xc_hypercall_bounce_post(xch, buf);
              
    return rc;
}    

int xc_mcd_weight(xc_interface *xch, domid_t domid, void *buf, uint32_t buf_len, uint32_t weight)
{             
    mcd_weight_t op;
    DECLARE_HYPERCALL_BOUNCE(buf, buf_len, XC_HYPERCALL_BUFFER_BOUNCE_OUT);
    int rc;
              
    op.domid = domid;
    op.weight = weight;
    op.buf_len = buf_len;

    if ( buf == NULL )
        return -EINVAL;

    if ( xc_hypercall_bounce_pre(xch, buf) ) {    
        PERROR("Could not bounce buffer for mcd control hypercall");
        return -ENOMEM;
    }         
              
    set_xen_guest_handle(op.buf, buf);
    rc = do_mcd_op(xch, XENMCD_weight, &op, sizeof(op));
    xc_hypercall_bounce_post(xch, buf);
              
    return rc;
}    

int xc_mcd_cache_mode(xc_interface *xch, void *buf, uint32_t buf_len, uint32_t which, uint32_t cache_mode)
{             
    mcd_cache_mode_t op;
    DECLARE_HYPERCALL_BOUNCE(buf, buf_len, XC_HYPERCALL_BUFFER_BOUNCE_OUT);
    int rc;
              
    op.which = which;
    op.cache_mode = cache_mode;
    op.buf_len = buf_len;

    if ( buf == NULL )
        return -EINVAL;

    if ( xc_hypercall_bounce_pre(xch, buf) ) {    
        PERROR("Could not bounce buffer for mcd control hypercall");
        return -ENOMEM;
    }         
              
    set_xen_guest_handle(op.buf, buf);
    rc = do_mcd_op(xch, XENMCD_cache_mode, &op, sizeof(op));
    xc_hypercall_bounce_post(xch, buf);
              
    return rc;
}    

int xc_mcd_param(xc_interface *xch, void *buf, uint32_t buf_len, uint32_t which, uint32_t mode, uint32_t param)
{             
    mcd_param_t op;
    DECLARE_HYPERCALL_BOUNCE(buf, buf_len, XC_HYPERCALL_BUFFER_BOUNCE_OUT);
    int rc;
              
    op.which = which;
    op.param = param;
    op.mode = mode;
    op.buf_len = buf_len;

    if ( buf == NULL )
        return -EINVAL;

    if ( xc_hypercall_bounce_pre(xch, buf) ) {    
        PERROR("Could not bounce buffer for mcd control hypercall");
        return -ENOMEM;
    }         
              
    set_xen_guest_handle(op.buf, buf);
    rc = do_mcd_op(xch, XENMCD_param, &op, sizeof(op));
    xc_hypercall_bounce_post(xch, buf);
              
    return rc;
}    

/* 
 * MCD Event 
 */

int xc_mcd_event_control(xc_interface *xch, domid_t domain_id, unsigned int op,
                         unsigned int mode, void *shared_page, void *ring_page)
{
    DECLARE_MCDCTL;

    mcdctl.cmd = XEN_MCDCTL_mcd_event_op;
    mcdctl.domain = domain_id;
    mcdctl.u.mcd_event_op.op = op;
    mcdctl.u.mcd_event_op.mode = mode;

    mcdctl.u.mcd_event_op.shared_addr = (unsigned long)shared_page;
    mcdctl.u.mcd_event_op.ring_addr = (unsigned long)ring_page;

    return do_mcd_op(xch, XENMCD_mcdctl_op, &mcdctl, sizeof(mcdctl));
}

int xc_mcd_event_enable(xc_interface *xch, domid_t domain_id,
                        void *shared_page, void *ring_page)
{
    return xc_mcd_event_control(xch, domain_id,
                                MCD_EVENT_OP_ENABLE, 0,
                                shared_page, ring_page);
}

int xc_mcd_event_resume(xc_interface *xch, domid_t domain_id)
{
    return xc_mcd_event_control(xch, domain_id,
                                MCD_EVENT_OP_RESUME, 0,
                                NULL, NULL);
}

int xc_mcd_event_disable(xc_interface *xch, domid_t domain_id)
{
    return xc_mcd_event_control(xch, domain_id,
                                MCD_EVENT_OP_DISABLE, 0,
                                NULL, NULL);
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
