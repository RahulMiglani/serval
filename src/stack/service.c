/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- 
 *
 * Serval's service table.
 *
 * Authors: Erik Nordström <enordstr@cs.princeton.edu>
 * 
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation; either version 2 of
 *	the License, or (at your option) any later version.
 */
#include <serval/platform.h>
#include <serval/netdevice.h>
#include <serval/atomic.h>
#include <serval/debug.h>
#include <serval/list.h>
#include <serval/lock.h>
#include <serval/dst.h>
#include <netinet/serval.h>
#if defined(OS_USER)
#include <stdlib.h>
#include <errno.h>
#endif

#include "service.h"
#include "bst.h"

#define get_service(n) bst_node_private(n, struct service_entry)
#define find_service_entry(tbl, prefix, bits)                           \
        get_service(bst_find_longest_prefix(tbl->tree, prefix, bits))

struct service_table {
        struct bst tree;
        struct bst_node_ops srv_ops;
        uint32_t instances;
        uint32_t services;
        atomic_t bytes_resolved;
        atomic_t packets_resolved;
        atomic_t bytes_dropped;
        atomic_t packets_dropped;
        rwlock_t lock;
};

static int service_entry_init(struct bst_node *n);
static void service_entry_destroy(struct bst_node *n);

static struct service_table srvtable;
static struct service_id default_service;

static struct dest *dest_create(const void *dst, int dstlen, 
                                const void *dest_out, uint32_t weight,
                                gfp_t alloc) 
{
        struct dest *de;

        if (dstlen == 0 && dest_out == NULL)
                return NULL;

        de = (struct dest *) MALLOC(sizeof(*de) + dstlen, alloc);

        if (!de)
                return NULL;

        memset(de, 0, sizeof(*de) + dstlen);
        de->weight = weight;
        de->dstlen = dstlen;

        if (dstlen > 0) {
                if (dest_out != NULL) {
                        de->dest_out.dev = (struct net_device *)dest_out;
                        dev_hold(de->dest_out.dev);
                }
                memcpy(de->dst, dst, dstlen);
        } else {
                de->dest_out.sk = (struct sock*) dest_out;
                sock_hold(de->dest_out.sk);
                de->dstlen = 0;
        }

        INIT_LIST_HEAD(&de->lh);

        return de;
}

static void dest_free(struct dest *de) 
{
        if (!is_sock_dest(de) && de->dest_out.dev)
                dev_put(de->dest_out.dev);
        else if (is_sock_dest(de) && de->dest_out.sk)
                sock_put(de->dest_out.sk);
        FREE(de);
}

static struct dest_set *dset_create(uint16_t flags, 
                                    uint32_t priority, 
                                    gfp_t alloc) {
        struct dest_set *dset;

        dset = (struct dest_set *) MALLOC(sizeof(*dset), alloc);

        if (!dset)
                return NULL;

        memset(dset, 0, sizeof(*dset));
        dset->flags = flags;
        dset->priority = priority;

        INIT_LIST_HEAD(&dset->ds);
        INIT_LIST_HEAD(&dset->dest_list);

        return dset;
}

static void dset_free(struct dest_set *dset) 
{
        struct dest *de;
       
        while (!list_empty(&dset->dest_list)) {
                de = list_first_entry(&dset->dest_list, struct dest, lh);
                list_del(&de->lh);
                dest_free(de);
        }
        FREE(dset);
}

static struct dest *__service_entry_get_dev(struct service_entry *se, 
                                            const char *ifname) 
{
        struct dest *de;
        //struct net_device *dev = NULL;
        struct dest_set* dset = NULL;
        
        list_for_each_entry(dset, &se->dest_set, ds) {
                list_for_each_entry(de, &dset->dest_list, lh) {
                        if (!is_sock_dest(de) && de->dest_out.dev && 
                            strcmp(de->dest_out.dev->name, ifname) == 0) {
                                return de;
                        }
                }
        }

        return NULL;
}

enum {
        MATCH_NO_PROTOCOL = -1,
        MATCH_ANY_PROTOCOL = 0,
};

static struct dest * __service_entry_get_dest(struct service_entry *se, 
                                              const void *dst,
                                              int dstlen,
                                              const void *dest_out,
                                              struct dest_set **dset_p,
                                              int protocol) 
{
        struct dest *de = NULL;
        struct dest_set* dset = NULL;
        //const struct net_device *dev = (const struct net_device *)dest_out;

        list_for_each_entry(dset, &se->dest_set, ds) {
                list_for_each_entry(de, &dset->dest_list, lh) {
                        if (is_sock_dest(de) && dstlen == 0) {
                                struct sock *sk = de->dest_out.sk;
                                if (sk->sk_protocol == protocol || 
                                    protocol == MATCH_ANY_PROTOCOL) {
                                        if (dset_p)
                                                *dset_p = dset;
                                        return de;
                                }
                        } else if (!is_sock_dest(de) && 
                                   memcmp(de->dst, dst, dstlen) == 0 
                                   /* && 
                                      (!dev || dev->ifindex == de->dest_out.dev->ifindex) */) {
                                if (dset_p)
                                        *dset_p = dset;
                                return de;
                        }
                }
        }

        return NULL;
}

/* 
   The returned net_device will have an increased reference count, so
   a put is necessary following a successful call to this
   function.
*/
struct net_device *service_entry_get_dev(struct service_entry *se, 
                                         const char *ifname) 
{
        struct dest *de = NULL;

        read_lock_bh(&se->destlock);

        de = __service_entry_get_dev(se, ifname);

        if (de)
                dev_hold(de->dest_out.dev);

        read_unlock_bh(&se->destlock);

        return de ? de->dest_out.dev : NULL;
}

static void dset_add_dest(struct dest_set* dset, struct dest* de) 
{
        list_add_tail(&de->lh, &dset->dest_list);
        dset->normalizer += de->weight;
        dset->count++;
}

static void service_entry_insert_dset(struct service_entry *se, 
                                      struct dest_set *dset) 
{

        struct dest_set *pos = NULL;
        list_for_each_entry(pos, &se->dest_set, ds) {
                if (pos->priority < dset->priority) {
                        list_add_tail(&dset->ds, &pos->ds);
                        return;
                }
        }
        list_add_tail(&dset->ds, &se->dest_set);
}

static struct dest_set *__service_entry_get_dset(struct service_entry *se, 
                                                 uint32_t priority) 
{
        struct dest_set *pos = NULL;

        list_for_each_entry(pos, &se->dest_set, ds) {
                if (pos->priority == priority)
                        return pos;
        }

        return NULL;
}

static int __service_entry_add_dest(struct service_entry *se, 
                                    uint16_t flags, uint32_t priority,
                                    uint32_t weight, const void *dst, 
                                    int dstlen, const void *dest_out, 
                                    gfp_t alloc) 
{
        struct dest_set *dset = NULL;
        struct dest *de;
        
        de = __service_entry_get_dest(se, dst, dstlen,
                                      (const struct net_device *)dest_out, 
                                      &dset,
                                      dstlen == 0 ? 
                                      ((struct sock *)dest_out)->sk_protocol :
                                      MATCH_NO_PROTOCOL);

        if (de) {
                if (is_sock_dest(de)) {
                        return -EADDRINUSE;
                }
                LOG_INF("Identical service entry already exists\n");
                return 0;
        }
        
        de = dest_create(dst, dstlen, dest_out, weight, alloc);

        if (!de)
                return -ENOMEM;

        dset = __service_entry_get_dset(se, priority);

        if (!dset) {
                dset = dset_create(flags, priority, alloc);

                if (!dset) {
                        dest_free(de);
                        return -ENOMEM;
                }
                service_entry_insert_dset(se, dset);
        }

        dset_add_dest(dset, de);

        se->count++;

        return 1;
}

int service_entry_add_dest(struct service_entry *se, uint16_t flags, 
                           uint32_t priority, uint32_t weight, 
                           const void *dst, int dstlen, 
                           const void *dest_out, gfp_t alloc) 
{
        int ret = 0;

        write_lock_bh(&se->destlock);
        /* 
           NOTE: we ignore the alloc argument here and always use
           GFP_ATOMIC since we hold a spinlock. If we want to allow
           GFP_KERNEL, we should probably restructure the code so that
           new memory is allocated before we lock the table.
         */
        ret = __service_entry_add_dest(se, flags, priority, 
                                       weight, dst, dstlen, 
                                       dest_out, GFP_ATOMIC);
        write_unlock_bh(&se->destlock);

        return ret;
}

static void dset_remove_dest(struct dest_set* dset, struct dest* de) 
{
        dset->normalizer -= de->weight;
        list_del(&de->lh);
        dset->count--;
}

static int __service_entry_modify_dest(struct service_entry *se, 
                                       uint16_t flags, uint32_t priority,
                                       uint32_t weight, 
                                       const void *dst, 
                                       int dstlen, 
                                       const void *new_dst, 
                                       int new_dstlen, 
                                       const void *dest_out, 
                                       gfp_t alloc) 
{
        struct dest_set *dset = NULL;
        struct dest *de = __service_entry_get_dest(se, dst, dstlen, 
                                                   dest_out, &dset,
                                                   dstlen == 0 ? 
                                                   ((struct sock *)dest_out)->sk_protocol :
                                                   MATCH_NO_PROTOCOL);
        
        if (!de) {
                LOG_DBG("Could not get dest entry\n");
                return 0;
        }

        if (new_dstlen == de->dstlen && new_dst)
                memcpy(de->dst, new_dst, new_dstlen);

        if (dset->priority != priority) {
                struct dest_set *ndset;

                ndset = __service_entry_get_dset(se, priority);

                if (!ndset) {
                        ndset = dset_create(flags, priority, alloc);

                        if (!ndset)
                                return -ENOMEM;

                        service_entry_insert_dset(se, ndset);
                }

                dset_remove_dest(dset, de);

                if (dset->count == 0) {
                        list_del(&dset->ds);
                        dset_free(dset);
                }

                de->weight = weight;
                dset_add_dest(ndset, de);
        } else {
                /*adjust the normalizer*/
                dset->normalizer -= de->weight;
                de->weight = weight;
                dset->normalizer += de->weight;
        }
        dset->flags = flags;

        return 1;
}

int service_entry_modify_dest(struct service_entry *se, 
                              uint16_t flags, uint32_t priority,
                              uint32_t weight, 
                              const void *dst, 
                              int dstlen, 
                              const void *new_dst, 
                              int new_dstlen, 
                              const void *dest_out,
                              gfp_t alloc) 
{
        int ret = 0;
        
        write_lock_bh(&se->destlock);
        ret = __service_entry_modify_dest(se, flags, priority, weight, 
                                          dst, dstlen, 
                                          new_dst, new_dstlen,
                                          dest_out, alloc);
        write_unlock_bh(&se->destlock);

        return ret;
}


static void __service_entry_inc_dest_stats(struct service_entry *se, 
                                           const void *dst, int dstlen, 
                                           int packets, int bytes) 
{
        struct dest_set* dset = NULL;
        struct dest *de = __service_entry_get_dest(se, dst, dstlen, 
                                                   NULL, &dset, 
                                                   MATCH_NO_PROTOCOL);

        if (!de)
                return;

        if (packets > 0) {
                atomic_add(packets, &de->packets_resolved);
                atomic_add(bytes, &de->bytes_resolved);

                atomic_add(packets, &se->packets_resolved);
                atomic_add(bytes, &se->bytes_resolved);

                atomic_add(packets, &srvtable.packets_resolved);
                atomic_add(bytes, &srvtable.bytes_resolved);
        } else {
                atomic_add(-packets, &de->packets_dropped);
                atomic_add(-bytes, &de->bytes_dropped);

                atomic_add(-packets, &se->packets_dropped);
                atomic_add(-bytes, &se->bytes_dropped);

                atomic_add(-packets, &srvtable.packets_dropped);
                atomic_add(-bytes, &srvtable.bytes_dropped);
        }

}
void service_entry_inc_dest_stats(struct service_entry *se, 
                                  const void* dst, int dstlen, 
                                  int packets, int bytes) 
{
        /*using a read lock since we are atomically updating stats and not modifying the dset/dest itself*/
        read_lock_bh(&se->destlock);
        __service_entry_inc_dest_stats(se, dst, dstlen, packets, bytes);
        read_unlock_bh(&se->destlock);
}

int __service_entry_remove_dest_by_dev(struct service_entry *se, 
                                       const char *ifname) 
{
        struct dest *de;
        struct dest *dtemp = NULL;
        struct dest_set* dset = NULL;
        struct dest_set* dsetemp = NULL;
        int count = 0;

        list_for_each_entry_safe(dset, dsetemp, &se->dest_set, ds) {
                list_for_each_entry_safe(de, dtemp, &dset->dest_list, lh) {
                        if (!is_sock_dest(de) && de->dest_out.dev && 
                            strcmp(de->dest_out.dev->name, ifname) == 0) {
                                dset_remove_dest(dset, de);
                                dest_free(de);

                                if (dset->count == 0) {
                                        list_del(&dset->ds);
                                        dset_free(dset);
                                }
                                se->count--;
                                count++;
                        }
                }
        }

        return count;
}

int service_entry_remove_dest_by_dev(struct service_entry *se, 
                                     const char *ifname) {
        int ret;

        local_bh_disable();
        write_lock(&se->destlock);
        
        ret = __service_entry_remove_dest_by_dev(se, ifname);
        
        if (ret > 0) {
                srvtable.instances--;
        }
        
        write_unlock(&se->destlock);
        
        write_lock(&srvtable.lock);

        if (list_empty(&se->dest_set)) {
                /* Removing the node also puts the service entry */
                bst_node_remove(se->node);
                srvtable.services--;
        }

        write_unlock(&srvtable.lock);
        local_bh_enable();
        return ret;
}

int __service_entry_remove_dest(struct service_entry *se, 
                                const void *dst, int dstlen,
                                struct dest_stats* dstats) 
{
        struct dest *de;
        struct dest_set* dset = NULL;
        
        list_for_each_entry(dset, &se->dest_set, ds) {
                list_for_each_entry(de, &dset->dest_list, lh) {
                        if ((is_sock_dest(de) && dstlen == 0) || 
                           (!is_sock_dest(de) && memcmp(de->dst, dst,
                                                        dstlen) == 0)) {
                                dset_remove_dest(dset, de);

                                if (dstats) {
                                        dstats->packets_resolved = atomic_read(&de->packets_resolved);
                                        dstats->bytes_resolved = atomic_read(&de->bytes_resolved);
                                        dstats->packets_dropped = atomic_read(&de->packets_dropped);
                                        dstats->bytes_dropped = atomic_read(&de->bytes_dropped);
                                }
                                
                                dest_free(de);
                                
                                if (dset->count == 0) {
                                        list_del(&dset->ds);
                                        dset_free(dset);
                                }
                                se->count--;
                                return 1;
                        }
                }
        }
        return 0;
}

int service_entry_remove_dest(struct service_entry *se, 
                              const void *dst, int dstlen,
                              struct dest_stats *dstats) 
{
        int ret;

        local_bh_disable();

        write_lock(&se->destlock);
        ret = __service_entry_remove_dest(se, dst, dstlen, dstats);

        if (ret > 0) {
                srvtable.instances--;
        }
        write_unlock(&se->destlock);

        write_lock(&srvtable.lock);

        if (list_empty(&se->dest_set)) {
                /* Removing the node also puts the service entry */
                bst_node_remove(se->node);
                srvtable.services--;
        }

        write_unlock(&srvtable.lock);
        local_bh_enable();

        return ret;
}

static struct service_entry *service_entry_create(gfp_t alloc) 
{
        struct service_entry *se;

        se = (struct service_entry *) MALLOC(sizeof(*se), alloc);

        if (!se)
                return NULL;

        memset(se, 0, sizeof(*se));

        INIT_LIST_HEAD(&se->dest_set);
        rwlock_init(&se->destlock);
        atomic_set(&se->refcnt, 1);

        return se;
}

int service_entry_init(struct bst_node *n) 
{
        return 0;
}

void __service_entry_free(struct service_entry *se) 
{
        struct dest_set *dset;
        
        while (!list_empty(&se->dest_set)) {
                dset = list_first_entry(&se->dest_set, 
                                        struct dest_set, ds);
                list_del(&dset->ds);
                dset_free(dset);
        }

        rwlock_destroy(&se->destlock);
        FREE(se);
}

void service_entry_hold(struct service_entry *se) 
{
        atomic_inc(&se->refcnt);
}

void service_entry_put(struct service_entry *se) 
{
        if (atomic_dec_and_test(&se->refcnt))
                __service_entry_free(se);
}

static void service_entry_free(struct service_entry *se) 
{
        service_entry_put(se);
}

void service_entry_destroy(struct bst_node *n) 
{
        struct service_entry* se = get_service(n);
        
        /* TODO - necessary for full dest del */
        srvtable.instances -= se->count;
        service_entry_put(get_service(n));
}

void service_resolution_iter_init(struct service_resolution_iter* iter, 
                                  struct service_entry *se,
                                  iter_mode_t mode) 
{
        /* lock the se, take the top priority entry and determine the
         * extent of iteration */
        struct dest_set *dset;

        memset(iter, 0, sizeof(*iter));
        iter->entry = se;
        read_lock_bh(&se->destlock);

        if (se->count == 0 || list_empty(&se->dest_set))
                return;

        dset = list_first_entry(&se->dest_set, struct dest_set, ds);

        /* round robin or sample */
        if (mode == SERVICE_ITER_ALL || (dset->flags & SVSF_MULTICAST)) {
                iter->dest_pos = dset->dest_list.next;
                iter->destset = dset;
        } else {
#define SAMPLE_SHIFT 32
                struct dest *dst = NULL;
                uint64_t sample, sumweight = 0;
#if defined(OS_LINUX_KERNEL)
                uint32_t rand;
                unsigned long rem;
                get_random_bytes(&rand, sizeof(rand));
                sample = rand;
                sample = sample << SAMPLE_SHIFT;
                rem = 0xffffffff;
                rem = do_div(sample, rem);

                /*sample = (sample << SAMPLE_SHIFT) / 0xffffffff; */
#else
                sample = random();
                sample = (sample << SAMPLE_SHIFT) / RAND_MAX;
#endif
                sample = sample * dset->normalizer;

                /*
                  LOG_DBG("sample=%llu normalizer=%u\n", 
                        sample, dset->normalizer);
                */
                list_for_each_entry(dst, &dset->dest_list, lh) {
                        uint64_t weight = dst->weight;
                        
                        sumweight += (weight << SAMPLE_SHIFT);

                        if (sample <= sumweight) {
                                iter->dest_pos = &dst->lh;
                                iter->destset = NULL;
                                return;
                        }
                }
                
                if (dst) {
                        iter->dest_pos = &dst->lh;
                        iter->destset = NULL;
                }
        }
}

void service_resolution_iter_destroy(struct service_resolution_iter* iter) 
{
        iter->dest_pos = NULL;
        iter->destset = NULL;
        read_unlock_bh(&iter->entry->destlock);
}

struct dest *service_resolution_iter_next(struct service_resolution_iter* iter)
{
        struct dest *dst;

        iter->last_pos = iter->dest_pos;

        if (iter->dest_pos == NULL)
                return NULL;

        dst = list_entry(iter->dest_pos, struct dest, lh);

        if (iter->destset) {
                if (iter->dest_pos == &iter->destset->dest_list) {
                        dst = NULL;
                } else {
                        iter->dest_pos = dst->lh.next;
                }
        } else {
                iter->dest_pos = NULL;
        }

        return dst;
}

void service_resolution_iter_inc_stats(struct service_resolution_iter *iter, 
                                       int packets, int bytes) 
{
        struct dest *dst = NULL;

        if (iter == NULL)
                return;

        if (packets > 0) {
                if (iter->last_pos == NULL)
                        return;

                dst = list_entry(iter->last_pos, struct dest, lh);

                atomic_add(packets, &dst->packets_resolved);
                atomic_add(bytes, &dst->bytes_resolved);

                atomic_add(packets, &iter->entry->packets_resolved);
                atomic_add(bytes, &iter->entry->bytes_resolved);

                atomic_add(packets, &srvtable.packets_resolved);
                atomic_add(bytes, &srvtable.bytes_resolved);

        } else {
                if (iter->last_pos != NULL) {
                        dst = list_entry(iter->last_pos, struct dest, lh);
                        atomic_add(-packets, &dst->packets_dropped);
                        atomic_add(-bytes, &dst->bytes_dropped);
                }

                atomic_add(-packets, &iter->entry->packets_dropped);
                atomic_add(-bytes, &iter->entry->bytes_dropped);

                atomic_add(-packets, &srvtable.packets_dropped);
                atomic_add(-bytes, &srvtable.bytes_dropped);
        }
}

int service_resolution_iter_get_priority(struct service_resolution_iter* iter) 
{
        if (iter == NULL)
                return 0;

        if (iter->last_pos != NULL && iter->destset)
                return iter->destset->priority;

        return 0;
}

int service_resolution_iter_get_flags(struct service_resolution_iter* iter)
{
        if (iter == NULL)
                return 0;

        if (iter->last_pos != NULL && iter->destset)
                return iter->destset->flags;

        return 0;
}

/*
typedef enum {
        FORWARD,
        DEMUX,
        DELAY,
        DROP,        
} service_rule_type_t;

static const char *rule_type_names[] = {
        [FORWARD] = "FWD",
        [DEMUX] = "DMX",
        [DELAY] = "DLY",
        [DROP] = "DRP"
};
*/

static const char *protocol_to_str(int protocol)
{
        static char buf[20];
        
        switch (protocol) {
        case IPPROTO_TCP:
                sprintf(buf, "TCP");
                break;
        case IPPROTO_UDP:
                sprintf(buf, "UDP");
                break;
        default:
                sprintf(buf, "%d", protocol);
                break;
        }
        
        return buf;
}

static int __service_entry_print(struct bst_node *n, char *buf, int buflen) 
{
#define PREFIX_BUFLEN (sizeof(struct service_id)*2+4)
        char prefix[PREFIX_BUFLEN];
        struct service_entry *se = get_service(n);
        struct dest_set *dset;
        struct dest *de;
        char dststr[18]; /* Currently sufficient for IPv4 */
        int len = 0, tot_len = 0, find_size = 0;
        unsigned int bits = 0;
        char tmpbuf[200];

        if (buflen < 0) {
                buf = tmpbuf;
                buflen = sizeof(tmpbuf);
                find_size = 1;
        }

        read_lock_bh(&se->destlock);

        bst_node_print_prefix(n, prefix, PREFIX_BUFLEN);

        bits = bst_node_prefix_bits(n);

        list_for_each_entry(dset, &se->dest_set, ds) {
                list_for_each_entry(de, &dset->dest_list, lh) {
                        len = snprintf(buf + len, buflen - len, 
                                       "%-64s %-6u %-6u %-6u %-6u %-10u %-10u ", 
                                       prefix,
                                       bits,
                                       dset->flags, 
                                       dset->priority, 
                                       de->weight,
                                       atomic_read(&de->packets_resolved),
                                       atomic_read(&de->packets_dropped));

                        tot_len += len;

                        if (find_size)
                                len = 0;
                        else
                                len = tot_len;

                        if (is_sock_dest(de) && de->dest_out.sk) {
                                len = snprintf(buf + len, buflen - len, 
                                               "%-5s %s\n", 
                                               de->dest_out.sk ? 
                                               "sock" : "NULL",
                                               protocol_to_str(de->dest_out.sk->sk_protocol));
                                
                                tot_len += len;

                                if (find_size)
                                        len = 0;
                                else
                                        len = tot_len;
                        } else if (!is_sock_dest(de) && de->dest_out.dev) {
                                len = snprintf(buf + len, buflen - len, 
                                               "%-5s %s\n",
                                               de->dest_out.dev ? 
                                               de->dest_out.dev->name : "any",
                                               inet_ntop(AF_INET,
                                                         de->dst, 
                                                         dststr, 18));

                                tot_len += len;

                                if (find_size)
                                        len = 0;
                                else
                                        len = tot_len;
                        }
                }
        }
        
        read_unlock_bh(&se->destlock);
        
        return tot_len;
}

int service_entry_print(struct service_entry *se, char *buf, int buflen) 
{
        return __service_entry_print(se->node, buf, buflen);
}

void service_table_read_lock(void)
{
        read_lock_bh(&srvtable.lock);
}

void service_table_read_unlock(void)
{
        read_unlock_bh(&srvtable.lock);
}

int __service_table_print(char *buf, int buflen)
{
        int len = 0, tot_len = 0, find_size = 0;
        char tmp_buf[200];

        if (buflen < 0) {
                find_size = 1;
                buf = tmp_buf;
                buflen = 200;
        }

 #if defined(OS_USER)
        /* Adding this stuff prints garbage in the kernel */
        len = snprintf(buf, buflen, "instances: %i bytes resolved: "
                       "%i packets resolved: %i bytes dropped: "
                       "%i packets dropped %i\n",
                       srvtable.instances, 
                       atomic_read(&srvtable.bytes_resolved),
                       atomic_read(&srvtable.packets_resolved),
                       atomic_read(&srvtable.bytes_dropped),
                       atomic_read(&srvtable.packets_dropped));
        
        tot_len += len;
        
        /* If we are finding out the buffer size, only
           increment tot_len, not len. */

        if (find_size)
                len = 0;
        else
                len = tot_len;
#endif
        len = snprintf(buf + len, buflen + len, 
                       "%-64s %-6s %-6s %-6s %-6s %-10s %-10s %s\n", 
                       "prefix", "bits", "flags", "prio", "weight", 
                       "resolved", "dropped", "target(s)");
        
        tot_len += len;
        
        if (find_size)
                len = 0;
        else
                len = tot_len;
        
        len = bst_print(&srvtable.tree, buf + len, 
                        find_size ? -1 : buflen - len);

        tot_len += len;

        return tot_len;
}

int service_table_print(char *buf, int buflen)
{
        int ret;
        read_lock_bh(&srvtable.lock);
        ret = __service_table_print(buf, buflen);
        read_unlock_bh(&srvtable.lock);
        return ret;
}

static int service_entry_local_match(struct bst_node *n)
{
        struct service_entry *se = get_service(n);
        struct dest *dst;

        dst = __service_entry_get_dest(se, NULL, 0, 
                                       NULL, NULL, 
                                       MATCH_ANY_PROTOCOL);
        
        if (dst && is_sock_dest(dst) && dst->dest_out.sk) 
                return 1;

        return 0;
}

static int service_entry_global_match(struct bst_node *n)
{
        struct service_entry *se = get_service(n);        
        struct dest *dst;

        dst = __service_entry_get_dest(se, NULL, 0, 
                                       NULL, NULL, 
                                       MATCH_NO_PROTOCOL);
        
        if (dst && !is_sock_dest(dst)) 
                return 1;

        return 0;
}

static int service_entry_any_match(struct bst_node *n)
{
        return 1;
}

static struct service_entry *__service_table_find(struct service_table *tbl,
                                                  struct service_id *srvid, 
                                                  int prefix, 
                                                  service_entry_type_t type) 
{
        struct service_entry *se = NULL;
        struct bst_node *n;
        int (*match)(struct bst_node *) = NULL;

        if (!srvid)
                return NULL;
        
        switch (type) {
        case SERVICE_ENTRY_LOCAL:
                match = service_entry_local_match;
                break;
        case SERVICE_ENTRY_GLOBAL:
        case SERVICE_ENTRY_EXACT:
                match = service_entry_global_match;
                break;
        case SERVICE_ENTRY_ANY:
                match = service_entry_any_match;
                break;
        }

        n = bst_find_longest_prefix_match(&tbl->tree, srvid, prefix, match);
        
        if (n) {
                if (type != SERVICE_ENTRY_EXACT ||
                    bst_node_prefix_bits(n) == prefix)
                        se = get_service(n);
        }

        return se;
}

static struct service_entry *service_table_find(struct service_table *tbl,
                                                struct service_id *srvid, 
                                                int prefix, 
                                                service_entry_type_t type)
{
        struct service_entry *se = NULL;

        read_lock_bh(&tbl->lock);

        se = __service_table_find(tbl, srvid, prefix, type);

        if (se)
                service_entry_hold(se);

        read_unlock_bh(&tbl->lock);

        return se;        
}

static struct sock* service_table_find_sock(struct service_table *tbl, 
                                            struct service_id *srvid,
                                            int prefix, int protocol) 
{
        struct service_entry *se = NULL;
        struct sock *sk = NULL;
        struct dest *dst = NULL;
        
        if (!srvid)
                return NULL;
        
        read_lock_bh(&tbl->lock);

        se = __service_table_find(tbl, srvid, prefix, SERVICE_ENTRY_LOCAL);
        
        if (se) {
                dst = __service_entry_get_dest(se, NULL, 0, 
                                               NULL, NULL, protocol);
                
                if (dst && is_sock_dest(dst)) {
                        sk = dst->dest_out.sk;
                        sock_hold(sk);
                }
        }
        
        read_unlock_bh(&tbl->lock);

        return sk;
}

static void service_table_get_stats(struct service_table *tbl, 
                                    struct table_stats *tstats) 
{
        
        /* TODO - not sure if the read lock here should be bh, since
         * this function will generally be called from a user-process
         * initiated netlink/ioctl/proc call
         */
        read_lock_bh(&tbl->lock);
        tstats->instances = tbl->instances;
        tstats->services = tbl->services;
        tstats->bytes_resolved = atomic_read(&tbl->bytes_resolved);
        tstats->packets_resolved = atomic_read(&tbl->packets_resolved);
        tstats->bytes_dropped = atomic_read(&tbl->bytes_dropped);
        tstats->packets_dropped = atomic_read(&tbl->packets_dropped);
        read_unlock_bh(&tbl->lock);

}
void service_get_stats(struct table_stats* tstats) 
{
        return service_table_get_stats(&srvtable, tstats);
}

struct service_entry *service_find_type(struct service_id *srvid, int prefix,
                                        service_entry_type_t type) 
{
        return service_table_find(&srvtable, srvid, prefix, type);
}

struct sock *service_find_sock(struct service_id *srvid, int prefix, int protocol) 
{
        return service_table_find_sock(&srvtable, srvid, prefix, protocol);
}

static int service_table_modify(struct service_table *tbl, 
                                struct service_id *srvid,
                                uint16_t prefix_bits, 
                                uint16_t flags, 
                                uint32_t priority, 
                                uint32_t weight, 
                                const void *dst,
                                int dstlen, 
                                const void *new_dst,
                                int new_dstlen, 
                                const void *dest_out) 
{
        //struct service_entry *se;
        struct bst_node *n;
        int ret = 0;

        if (!srvid)
                return -EINVAL;
        
        if (memcmp(srvid, &default_service, sizeof(default_service)) == 0)
                prefix_bits = 0;

        read_lock_bh(&tbl->lock);

        n = bst_find_longest_prefix(&tbl->tree, srvid, prefix_bits);
        
        if (n && bst_node_prefix_bits(n) >= prefix_bits) {
                if (dst || dstlen == 0) {
                        LOG_DBG("Modifying service\n");
                        ret = service_entry_modify_dest(get_service(n), 
                                                        flags, priority, 
                                                        weight, dst, dstlen,
                                                        new_dst, new_dstlen,
                                                        dest_out, GFP_ATOMIC);
                }
                goto out;
        }
        
        ret = -EINVAL;

out: 
        read_unlock_bh(&tbl->lock);
        
        return ret;
}

int service_modify(struct service_id *srvid, 
                   uint16_t prefix_bits, 
                   uint16_t flags,
                   uint32_t priority, 
                   uint32_t weight, 
                   const void *dst, 
                   int dstlen,    
                   const void *new_dst, 
                   int new_dstlen, 
                   const void *dest_out) 
{
        return service_table_modify(&srvtable, srvid, prefix_bits, flags,
                                    priority, weight == 0 ? 1 : weight, 
                                    dst, dstlen, new_dst, new_dstlen, 
                                    dest_out);
}

static int service_table_add(struct service_table *tbl, 
                             struct service_id *srvid,
                             uint16_t prefix_bits, 
                             uint16_t flags, 
                             uint32_t priority, 
                             uint32_t weight, 
                             const void *dst,
                             int dstlen, 
                             const void *dest_out, 
                             gfp_t alloc) {
        struct service_entry *se;
        struct bst_node *n;
        int ret = 0;
        
        if (!srvid)
                return -EINVAL;

        if (memcmp(srvid, &default_service, sizeof(default_service)) == 0)
                prefix_bits = 0;

        write_lock_bh(&tbl->lock);

        n = bst_find_longest_prefix(&tbl->tree, srvid, prefix_bits);

        if (n && bst_node_prefix_bits(n) >= prefix_bits) {
                if (dst || dstlen == 0) {
                        /* 
                           NOTE: we ignore the alloc argument here and
                           always use GFP_ATOMIC since we hold a
                           spinlock. If we want to allow GFP_KERNEL,
                           we should probably restructure the code so
                           that new memory is allocated before we lock
                           the table.
                        */

                        ret = __service_entry_add_dest(get_service(n), 
                                                       flags, priority, 
                                                       weight, dst, dstlen,
                                                       dest_out, GFP_ATOMIC);
                }
                goto out;
        }
        
        se = service_entry_create(GFP_ATOMIC);

        if (!se) {
                ret = -ENOMEM;
                goto out;
        }

        if (dest_out) {
                ret = __service_entry_add_dest(se, flags, priority, 
                                               weight, dst, dstlen, dest_out,
                                               GFP_ATOMIC);

                if (ret < 0) {
                        service_entry_free(se);
                        ret = -ENOMEM;
                        goto out;

                }
        }

        se->node = bst_insert_prefix(&tbl->tree, &tbl->srv_ops, 
                                     se, srvid, prefix_bits, GFP_ATOMIC);

        if (!se->node) {
                service_entry_free(se);
                ret = -ENOMEM;
        } else {
                tbl->services++;
        }

out: 
        if (ret > 0) {
                tbl->instances++;
        }
        write_unlock_bh(&tbl->lock);
        
        return ret;
}

void service_inc_stats(int packets, int bytes) 
{
        /*only for drops*/
        if (packets < 0) {
                atomic_add(-packets, &srvtable.packets_dropped);
                atomic_add(-bytes, &srvtable.bytes_dropped);
        }
}


int service_add(struct service_id *srvid, 
                uint16_t prefix_bits, 
                uint16_t flags, 
                uint32_t priority,
                uint32_t weight, 
                const void *dst, 
                int dstlen, 
                const void *dest_out, 
                gfp_t alloc) 
{
        return service_table_add(&srvtable, srvid, 
                                 prefix_bits, flags, priority, 
                                 weight == 0 ? 1 : weight, dst, dstlen,
                                 dest_out, alloc);
}

static void service_table_del(struct service_table *tbl, 
                              struct service_id *srvid,
                              uint16_t prefix_bits) 
{
        int ret;

        write_lock_bh(&tbl->lock);
        
        ret = bst_remove_prefix(&tbl->tree, srvid, prefix_bits);
        
        if (ret > 0)
                tbl->services--;
        write_unlock_bh(&tbl->lock);
}

void service_del(struct service_id *srvid, uint16_t prefix_bits) 
{
        return service_table_del(&srvtable, srvid, prefix_bits);
}

static void service_table_del_dest(struct service_table *tbl, 
                                   struct service_id *srvid,
                                   uint16_t prefix_bits, 
                                   const void *dst, 
                                   int dstlen, 
                                   struct dest_stats* stats) {
        struct bst_node *n;
        int ret = 0;

        local_bh_disable();
        write_lock(&tbl->lock);

        n = bst_find_longest_prefix(&tbl->tree, srvid, prefix_bits);

        if (n) {
                write_lock(&get_service(n)->destlock);
                ret = __service_entry_remove_dest(get_service(n), 
                                                  dst, dstlen, stats);
                if (ret > 0) {
                        tbl->instances--;
                }
                write_unlock(&get_service(n)->destlock);

                if (list_empty(&get_service(n)->dest_set)) {
                        /* Removing the node also puts the service entry */
                        bst_node_remove(n);
                        tbl->services--;
                }
        }

        write_unlock(&tbl->lock);
        local_bh_enable();
}

void service_del_dest(struct service_id *srvid, 
                      uint16_t prefix_bits, 
                      const void *dst, int dstlen,
                      struct dest_stats* stats) 
{
        return service_table_del_dest(&srvtable, srvid, prefix_bits, 
                                      dst, dstlen, stats);
}

static int del_dev_func(struct bst_node *n, void *arg) 
{
        struct service_entry *se = get_service(n);
        char *devname = (char *) arg;
        int ret = 0, should_remove = 0;
        
        write_lock_bh(&se->destlock);
        
        ret = __service_entry_remove_dest_by_dev(se, devname);
        
        if (ret == 1 && list_empty(&se->dest_set))
                should_remove = 1;

        write_unlock_bh(&se->destlock);

        if (ret == 1) {
                /* TODO - global reference kludge - assume the write
                 * lock is already acquired*/
                srvtable.instances--;
        }

        if (should_remove) {
                /* Removing the node also puts the service entry */
                bst_node_remove(n);
                srvtable.services--;
        }

        return ret;
}

static int service_table_del_dev_all(struct service_table *tbl, 
                                     const char *devname) 
{
        int ret = 0;
        
        write_lock_bh(&tbl->lock);
        
        if (tbl->tree.root)
                ret = bst_subtree_func(tbl->tree.root, del_dev_func, 
                                       (void *) devname);
        write_unlock_bh(&tbl->lock);

        return ret;
}

int service_del_dev_all(const char *devname) 
{
        return service_table_del_dev_all(&srvtable, devname);
}

static int del_dest_func(struct bst_node *n, void *arg) 
{
        struct service_entry *se = get_service(n);
        struct _d {
                const void *d_dst;
                int d_len;
        } *d = (struct _d *)arg;
        int ret = 0, should_remove = 0;

        write_lock_bh(&se->destlock);

        ret = __service_entry_remove_dest(se, d->d_dst, d->d_len, NULL);

        if(ret == 1 && list_empty(&se->dest_set))
                should_remove = 1;

        write_unlock_bh(&se->destlock);

        if (ret == 1) {
                /* TODO - global reference kludge - assume the write
                 * lock is already acquired*/
                srvtable.instances--;
        }

        if (should_remove) {
                /* Removing the node also puts the service entry */
                bst_node_remove(n);
                srvtable.services--;
        }

        return ret;
}

static int service_table_del_dest_all(struct service_table *tbl, 
                                      const void *dst, int dstlen) 
{
        int ret = 0;
        struct {
                const void *d_dst;
                int d_len;
        } d = { dst, dstlen };
        
        write_lock_bh(&tbl->lock);

        if(tbl->tree.root)
                ret = bst_subtree_func(tbl->tree.root, del_dest_func, &d);

        write_unlock_bh(&tbl->lock);

        return ret;
}

int service_del_dest_all(const void *dst, int dstlen) 
{
        return service_table_del_dest_all(&srvtable, dst, dstlen);
}

void __service_table_destroy(struct service_table *tbl) 
{
        bst_destroy(&tbl->tree);
}

void service_table_destroy(struct service_table *tbl) 
{
        write_lock_bh(&tbl->lock);
        __service_table_destroy(tbl);
        write_unlock_bh(&tbl->lock);
}

void service_table_init(struct service_table *tbl) 
{
        memset(&default_service, 0, sizeof(default_service));
        bst_init(&tbl->tree);
        tbl->srv_ops.init = service_entry_init;
        tbl->srv_ops.destroy = service_entry_destroy;
        tbl->srv_ops.print = __service_entry_print;
        tbl->instances = 0;
        tbl->services = 0;
        atomic_set(&tbl->packets_resolved, 0);
        atomic_set(&tbl->bytes_resolved, 0);
        atomic_set(&tbl->packets_dropped, 0);
        atomic_set(&tbl->bytes_dropped, 0);
        rwlock_init(&tbl->lock);
}
/*
  #if defined(OS_USER)
  struct kmem_cache kmem_cachep = {
  .size = sizeof(struct service_entry)
  };
  #endif
*/
int __init service_init(void) 
{
        /*
          #if defined(OS_LINUX_KERNEL)
          service_dst_ops.kmem_cachep =
          kmem_cache_create("service_dst_cache",
          sizeof(struct service_entry), 0,
          SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);
          #else
          service_dst_ops.kmem_cachep = &kmem_cachep;
          #endif
        */
        service_table_init(&srvtable);

        return 0;
}

void __exit service_fini(void) 
{
        service_table_destroy(&srvtable);
}
