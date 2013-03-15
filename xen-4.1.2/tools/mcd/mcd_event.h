/*****************************************************************************
 * tools/mcd/mcd_event.h
 *
 * mcd event structures.
 *
 * Copyright (c) 2012 The George Washington University (Jinho Hwang)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#ifndef __XEN_MCD_EVENT_H__
#define __XEN_MCD_EVENT_H__


#include "spinlock.h"
#include "xc.h"
#include <xc_private.h>

#include <xen/event_channel.h>
#include <xen/mcd.h>


#define mcd_event_ring_lock_init(_m)  spin_lock_init(&(_m)->ring_lock)
#define mcd_event_ring_lock(_m)       spin_lock(&(_m)->ring_lock)
#define mcd_event_ring_unlock(_m)     spin_unlock(&(_m)->ring_lock)


typedef struct mcd_event {
    domid_t domain_id;
    xc_evtchn *xce_handle;
    int port;
    mcd_event_back_ring_t back_ring;
    mcd_event_shared_page_t *shared_page;
    /*int num_shared_page;*/
    /*mcd_event_shared_page_t* shared_page[];*/
    void *ring_page;
    spinlock_t ring_lock;
} mcd_event_t;


#endif // __XEN_MCD_EVENT_H__


/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
