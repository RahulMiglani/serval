/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#ifndef _SERVICE_H_
#define _SERVICE_H_

#include <serval/lock.h>
#include <serval/list.h>
#include <serval/atomic.h>
#include <serval/skbuff.h>
#include <serval/dst.h>
#include <serval/sock.h>
#include "bst.h"

struct service_id;

struct service_entry {
	union {
		struct dst_entry dst;
	} u;
        struct bst_node *node;
        struct list_head dest_list, *dest_pos;
        struct sock *sk;
        rwlock_t destlock;
        atomic_t refcnt;
};

struct dest {
        struct list_head lh;
        struct net_device *dev;
        int dstlen;
        unsigned char dst[]; /* Must be last */
};

typedef enum {
        SERVICE_ENTRY_LOCAL,
        SERVICE_ENTRY_GLOBAL,
        SERVICE_ENTRY_ANY
} service_entry_type_t;

struct net_device *service_entry_get_dev(struct service_entry *se, 
                                         const char *ifname);
int service_entry_remove_dest_by_dev(struct service_entry *se, 
                                     const char *ifname);
int service_entry_remove_dest(struct service_entry *se, 
                              const void *dst, int dstlen);
int service_entry_add_dest(struct service_entry *se, 
                           const void *dst,
                           int dstlen,
                           struct net_device *dev,
                           gfp_t alloc);
void service_entry_dest_iterate_begin(struct service_entry *se);
void service_entry_dest_iterate_end(struct service_entry *se);
struct dest *service_entry_dest_next(struct service_entry *se);
int service_entry_dest_fill(struct service_entry *se, void *dst, 
                            int dstlen);

int service_add(struct service_id *srvid, unsigned int prefix_bits,
		const void *dst, int dstlen, struct net_device *dev, 
                struct sock *sk, gfp_t alloc);
void service_del(struct service_id *srvid, unsigned int prefix_bits);
void service_del_dest(struct service_id *srvid, unsigned int prefix_bits,
                      const void *dst, int dstlen);
int service_del_dest_all(const void *dst, int dstlen);
int service_del_dev_all(const char *devname);
struct service_entry *service_find_type(struct service_id *srvid, 
                                        service_entry_type_t type);

static inline struct service_entry *service_find(struct service_id *srvid)
{
        return service_find_type(srvid, SERVICE_ENTRY_ANY);
}

void service_entry_hold(struct service_entry *se);
void service_entry_put(struct service_entry *se);
int service_entry_print(struct service_entry *se, char *buf, int buflen);
int services_print(char *buf, int buflen);

static inline struct service_entry *skb_service_entry(struct sk_buff *skb)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
#define _skb_refdst dst
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
#define _skb_refdst _skb_dst
#endif
        if (skb->_skb_refdst == 0)
                return NULL;
        return (struct service_entry *)skb->_skb_refdst;        
}

static inline void skb_set_service_entry(struct sk_buff *skb, 
                                         struct service_entry *se)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
        skb->dst = (struct dst_entry *)se;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35)
        skb->_skb_dst = (unsigned long)se;
#else
        skb->_skb_refdst = (unsigned long)se;
#endif
}

#endif /* _SERVICE_H_ */
