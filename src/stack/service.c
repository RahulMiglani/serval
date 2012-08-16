/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- 
 *
 * Serval's service table.
 *
 * Authors: Erik Nordström <enordstr@cs.princeton.edu>
 *          David Shue <dshue@cs.princeton.edu>
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
#if defined(OS_LINUX_KERNEL)
#include <serval_ipv4.h>
#endif
#include "service.h"
#include "radixtree.h"

#define get_service(n) radix_node_private(n, struct service_entry)

struct service_table {
        struct radix_tree tree;
        atomic_t bytes_resolved;
        atomic_t packets_resolved;
        atomic_t bytes_dropped;
        atomic_t packets_dropped;
        rwlock_t lock;
};

static struct service_table srvtable;

static struct target *target_create(service_rule_type_t type,
                                    const void *dst, int dstlen,
                                    const union target_out out, 
                                    uint32_t weight,
                                    gfp_t alloc) 
{
        struct target *t;

        if (dstlen == 0 && out.raw == NULL)
                return NULL;

        t = (struct target *)kmalloc(sizeof(*t) + dstlen, alloc);

        if (!t)
                return NULL;

        memset(t, 0, sizeof(*t) + dstlen);
        t->type = type;
        t->weight = weight;
        t->dstlen = dstlen;

        if (dstlen > 0) {
                if (out.raw != NULL) {
                        t->out.dev = out.dev;
                        dev_hold(t->out.dev);
                }
                memcpy(t->dst, dst, dstlen);
        } else {
                t->out.sk = out.sk;
                sock_hold(t->out.sk);
                t->dstlen = 0;
        }

        INIT_LIST_HEAD(&t->lh);

        return t;
}

static void target_free(struct target *t) 
{
        if (!is_sock_target(t) && t->out.dev)
                dev_put(t->out.dev);
        else if (is_sock_target(t) && t->out.sk)
                sock_put(t->out.sk);
        kfree(t);
}

static struct target_set *target_set_create(uint16_t flags, 
                                            uint32_t priority, 
                                            gfp_t alloc) {
        struct target_set *set;
        
        set = (struct target_set *)kmalloc(sizeof(*set), alloc);

        if (!set)
                return NULL;

        memset(set, 0, sizeof(*set));
        set->flags = flags;
        set->priority = priority;

        INIT_LIST_HEAD(&set->lh);
        INIT_LIST_HEAD(&set->list);

        return set;
}

static void target_set_free(struct target_set *set) 
{
        struct target *t;
       
        while (!list_empty(&set->list)) {
                t = list_first_entry(&set->list, struct target, lh);
                list_del(&t->lh);
                target_free(t);
        }
        kfree(set);
}

static struct target *__service_entry_get_dev(struct service_entry *se, 
                                              const char *ifname) 
{
        struct target *t;
        struct target_set* set = NULL;
        
        list_for_each_entry(set, &se->target_set, lh) {
                list_for_each_entry(t, &set->list, lh) {
                        if (!is_sock_target(t) && t->out.dev && 
                            strcmp(t->out.dev->name, ifname) == 0) {
                                return t;
                        }
                }
        }

        return NULL;
}

enum {
        MATCH_NO_PROTOCOL = -1,
        MATCH_ANY_PROTOCOL = 0,
};

static struct target * __service_entry_get_target(struct service_entry *se,
                                                  service_rule_type_t type,
                                                  const void *dst,
                                                  int dstlen,
                                                  const union target_out out,
                                                  struct target_set **set_p,
                                                  int protocol) 
{
        struct target *t = NULL;
        struct target_set* set = NULL;

        list_for_each_entry(set, &se->target_set, lh) {
                list_for_each_entry(t, &set->list, lh) {
                        if (t->type != type )
                                continue;

                        if (type == RULE_DEMUX) {
                                if (t->out.sk->sk_protocol == protocol || 
                                    protocol == MATCH_ANY_PROTOCOL) {
                                        if (set_p)
                                                *set_p = set;
                                        return t;
                                }
                        } else if (type == RULE_FORWARD && 
                                   memcmp(t->dst, dst, dstlen) == 0) {
                                if (set_p)
                                        *set_p = set;
                                return t;
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
        struct target *t = NULL;

        read_lock_bh(&se->lock);

        t = __service_entry_get_dev(se, ifname);

        if (t)
                dev_hold(t->out.dev);

        read_unlock_bh(&se->lock);

        return t ? t->out.dev : NULL;
}

static void target_set_add_target(struct target_set *set, 
                                  struct target *t) 
{
        list_add_tail(&t->lh, &set->list);
        set->normalizer += t->weight;
        set->count++;
}

static void service_entry_insert_target_set(struct service_entry *se, 
                                            struct target_set *set) 
{

        struct target_set *pos = NULL;
        list_for_each_entry(pos, &se->target_set, lh) {
                if (pos->priority < set->priority) {
                        list_add_tail(&set->lh, &pos->lh);
                        return;
                }
        }
        list_add_tail(&set->lh, &se->target_set);
}

static struct target_set *
__service_entry_get_target_set(struct service_entry *se, 
                               uint32_t priority) 
{
        struct target_set *pos = NULL;

        list_for_each_entry(pos, &se->target_set, lh) {
                if (pos->priority == priority)
                        return pos;
        }

        return NULL;
}

static void target_set_remove_target(struct target_set *set, struct target* t) 
{
        set->normalizer -= t->weight;
        list_del(&t->lh);
        set->count--;
}

static int __service_entry_modify_target(struct service_entry *se,
                                         service_rule_type_t type,
                                         uint16_t flags, uint32_t priority,
                                         uint32_t weight, 
                                         const void *dst, 
                                         int dstlen, 
                                         const void *new_dst, 
                                         int new_dstlen, 
                                         const union target_out out, 
                                         gfp_t alloc) 
{
        struct target_set *set = NULL;
        struct target *t;

        if (dstlen == 0) {
                LOG_ERR("Cannot modify socket entry\n");
                return -1;
        }
        
        t = __service_entry_get_target(se, type, dst, dstlen, 
                                       out, &set,
                                       MATCH_NO_PROTOCOL);
        
        if (!t) {
                LOG_DBG("Could not get target entry\n");
                return 0;
        }
        
#if defined(OS_LINUX_KERNEL)
        /* Make sure it makes sense to add this target address on this
           interface */
        {
                struct rtable *rt;
                __be32 dst_ip = *((__be32 *)new_dst);
                
                /* FIXME: This routing does not work as expected. It
                   returns a valid entry even if the dst_ip does not
                   really match the subnet/destination of the returned
                   routing entry. We're probably giving some weird
                   input... */
                rt = serval_ip_route_output(&init_net, 
                                            dst_ip,
                                            0, 0, 
                                            t->out.dev->ifindex);
                if (!rt)
                        return 0;
        }
#endif

        if (new_dstlen == t->dstlen && new_dst)
                memcpy(t->dst, new_dst, new_dstlen);

        if (set->priority != priority) {
                struct target_set *nset;

                nset = __service_entry_get_target_set(se, priority);
                
                if (!nset) {
                        nset = target_set_create(flags, priority, alloc);

                        if (!nset)
                                return -ENOMEM;

                        service_entry_insert_target_set(se, nset);
                }

                target_set_remove_target(set, t);

                if (set->count == 0) {
                        list_del(&set->lh);
                        target_set_free(set);
                }

                t->weight = weight;
                target_set_add_target(nset, t);
        } else {
                /*adjust the normalizer*/
                set->normalizer -= t->weight;
                t->weight = weight;
                set->normalizer += t->weight;
        }
        set->flags = flags;

        return 1;
}

int service_entry_modify_target(struct service_entry *se, 
                                service_rule_type_t type,
                                uint16_t flags, uint32_t priority,
                                uint32_t weight, 
                                const void *dst, 
                                int dstlen, 
                                const void *new_dst, 
                                int new_dstlen, 
                                const union target_out out,
                                gfp_t alloc) 
{
        int ret = 0;
        
        write_lock_bh(&se->lock);
        ret = __service_entry_modify_target(se, type, flags, priority, 
                                            weight, dst, dstlen, 
                                            new_dst, new_dstlen,
                                            out, alloc);
        write_unlock_bh(&se->lock);

        return ret;
}


static void __service_entry_inc_target_stats(struct service_entry *se, 
                                             service_rule_type_t type,
                                             const void *dst, int dstlen, 
                                             int packets, int bytes) 
{
        struct target_set* set = NULL;
        struct target *t = __service_entry_get_target(se, type, dst, dstlen, 
                                                      make_target(NULL), &set, 
                                                      MATCH_NO_PROTOCOL);

        if (!t)
                return;

        if (packets > 0) {
                atomic_add(packets, &t->packets_resolved);
                atomic_add(bytes, &t->bytes_resolved);

                atomic_add(packets, &se->packets_resolved);
                atomic_add(bytes, &se->bytes_resolved);

                atomic_add(packets, &srvtable.packets_resolved);
                atomic_add(bytes, &srvtable.bytes_resolved);
        } else {
                atomic_add(-packets, &t->packets_dropped);
                atomic_add(-bytes, &t->bytes_dropped);

                atomic_add(-packets, &se->packets_dropped);
                atomic_add(-bytes, &se->bytes_dropped);

                atomic_add(-packets, &srvtable.packets_dropped);
                atomic_add(-bytes, &srvtable.bytes_dropped);
        }

}
void service_entry_inc_target_stats(struct service_entry *se,
                                    service_rule_type_t type,
                                    const void* dst, int dstlen, 
                                    int packets, int bytes) 
{
        /* using a read lock since we are atomically updating stats and
           not modifying the set/target itself */
        read_lock_bh(&se->lock);
        __service_entry_inc_target_stats(se, type, dst, dstlen, packets, bytes);
        read_unlock_bh(&se->lock);
}

int __service_entry_remove_target_by_dev(struct service_entry *se, 
                                         const char *ifname) 
{
        struct target *t;
        struct target *dtemp = NULL;
        struct target_set* set = NULL;
        struct target_set* setemp = NULL;
        int count = 0;

        list_for_each_entry_safe(set, setemp, &se->target_set, lh) {
                list_for_each_entry_safe(t, dtemp, &set->list, lh) {
                        if (t->type == RULE_FORWARD && t->out.dev && 
                            strcmp(t->out.dev->name, ifname) == 0) {
                                target_set_remove_target(set, t);
                                target_free(t);

                                if (set->count == 0) {
                                        list_del(&set->lh);
                                        target_set_free(set);
                                }
                                se->count--;
                                count++;
                        }
                }
        }

        return count;
}

int __service_entry_remove_target(struct service_entry *se, 
                                  service_rule_type_t type,
                                  const void *dst, int dstlen,
                                  struct target_stats *stats) 
{
        struct target *t;
        struct target_set* set = NULL;
        
        list_for_each_entry(set, &se->target_set, lh) {
                list_for_each_entry(t, &set->list, lh) {
                        if (t->type == type && 
                            ((t->type == RULE_DEMUX && dstlen == 0) || 
                            (t->type == RULE_FORWARD && 
                             memcmp(t->dst, dst, dstlen) == 0))) {
                                target_set_remove_target(set, t);

                                if (stats) {
                                        stats->packets_resolved = atomic_read(&t->packets_resolved);
                                        stats->bytes_resolved = atomic_read(&t->bytes_resolved);
                                        stats->packets_dropped = atomic_read(&t->packets_dropped);
                                        stats->bytes_dropped = atomic_read(&t->bytes_dropped);
                                }
                                
                                target_free(t);
                                
                                if (set->count == 0) {
                                        list_del(&set->lh);
                                        target_set_free(set);
                                }
                                se->count--;
                                return 1;
                        }
                }
        }
        return 0;
}

int service_entry_remove_target(struct service_entry *se,
                                service_rule_type_t type,
                                const void *dst, int dstlen,
                                struct target_stats *stats) 
{
        int ret;

        local_bh_disable();

        write_lock(&se->lock);
        ret = __service_entry_remove_target(se, type, dst, dstlen, stats);

        write_unlock(&se->lock);

        write_lock(&srvtable.lock);

        if (list_empty(&se->target_set)) {
                /* Removing the node also puts the service entry */
                radix_node_remove(se->node, GFP_ATOMIC);
        }

        write_unlock(&srvtable.lock);
        local_bh_enable();

        return ret;
}

static struct service_entry *service_entry_create(struct service_table *tbl, 
                                                  gfp_t alloc) 
{
        struct service_entry *se;

        se = (struct service_entry *)kmalloc(sizeof(*se), alloc);

        if (!se)
                return NULL;

        memset(se, 0, sizeof(*se));
        se->tbl = tbl;
        INIT_LIST_HEAD(&se->target_set);
        rwlock_init(&se->lock);
        atomic_set(&se->refcnt, 1);

        return se;
}

void __service_entry_free(struct service_entry *se) 
{
        struct target_set *set;
        
        while (!list_empty(&se->target_set)) {
                set = list_first_entry(&se->target_set, 
                                       struct target_set, lh);
                list_del(&set->lh);
                target_set_free(set);
        }

        rwlock_destroy(&se->lock);
        kfree(se);
}

void service_entry_hold(struct service_entry *se) 
{
        atomic_inc(&se->refcnt);
        /* LOG_DBG("%p refcount=%u\n",
                se, atomic_read(&se->refcnt));
        */
}

void service_entry_put(struct service_entry *se) 
{
        /* LOG_DBG("%p refcount=%u\n",
                se, atomic_read(&se->refcnt));
        */
        if (atomic_dec_and_test(&se->refcnt))
                __service_entry_free(se);
}

static void service_entry_free(struct service_entry *se) 
{
        service_entry_put(se);
}

int service_iter_init(struct service_iter *iter, 
                      struct service_entry *se,
                      iter_mode_t mode) 
{
        /* lock the service entry, take the top priority entry and
         * determine the extent of iteration */
        struct target_set *set;

        memset(iter, 0, sizeof(*iter));
        
        iter->mode = mode;
        iter->entry = se;
        read_lock_bh(&se->lock);

        if (se->count == 0 || list_empty(&se->target_set))
                return -1;

        set = list_first_entry(&se->target_set, struct target_set, lh);

        if (!set)
                return -1;
        
        if (mode == SERVICE_ITER_ANYCAST &&
            set->flags & SVSF_MULTICAST)
                return -1;
 
        /* round robin or sample */
        if (mode == SERVICE_ITER_ALL ||
            mode == SERVICE_ITER_DEMUX ||
            mode == SERVICE_ITER_FORWARD) {
                iter->pos = set->list.next;
                iter->set = set;
        } else {
#define SAMPLE_SHIFT 32
                struct target *t = NULL;
                uint64_t sample, sumweight = 0;
#if defined(OS_LINUX_KERNEL)
                uint32_t rand;
                unsigned long rem;
                get_random_bytes(&rand, sizeof(rand));
                sample = rand;
                sample = sample << SAMPLE_SHIFT;
                rem = 0xffffffff;
                rem = do_div(sample, rem);
#else
                sample = random();
                sample = (sample << SAMPLE_SHIFT) / RAND_MAX;
#endif
                sample = sample * set->normalizer;

                /*
                  LOG_DBG("sample=%llu normalizer=%u\n", 
                  sample, set->normalizer);
                */
                list_for_each_entry(t, &set->list, lh) {
                        uint64_t weight = t->weight;
                        
                        sumweight += (weight << SAMPLE_SHIFT);

                        if (sample <= sumweight) {
                                iter->pos = &t->lh;
                                iter->set = NULL;
                                return 0;
                        }
                }
                
                if (t) {
                        iter->pos = &t->lh;
                        iter->set = NULL;
                }
        }
        return 0;
}

void service_iter_destroy(struct service_iter *iter) 
{
        iter->pos = NULL;
        iter->set = NULL;
        read_unlock_bh(&iter->entry->lock);
}

struct target *service_iter_next(struct service_iter *iter)
{
        struct target *t;

        iter->last_pos = iter->pos;

        if (iter->pos == NULL)
                return NULL;

        while (1) {
                t = list_entry(iter->pos, struct target, lh);
                
                if (iter->set) {
                        if (iter->pos == &iter->set->list) {
                                /* We've reached the head again. */
                                t = NULL;
                                break;
                        } else {
                                iter->pos = t->lh.next;
                                
                                if (iter->mode == SERVICE_ITER_ALL)
                                        break;
                                else if (iter->mode == SERVICE_ITER_DEMUX &&
                                         t->type == RULE_DEMUX)
                                        break;
                                else if (iter->mode == SERVICE_ITER_FORWARD &&
                                         t->type == RULE_FORWARD)
                                        break;
                        }
                } else {
                        iter->pos = NULL;
                        break;
                }
        }

        return t;
}

void service_iter_inc_stats(struct service_iter *iter, 
                                       int packets, int bytes) 
{
        struct target *dst = NULL;

        if (iter == NULL)
                return;

        if (packets > 0) {
                if (iter->last_pos == NULL)
                        return;

                dst = list_entry(iter->last_pos, struct target, lh);

                atomic_add(packets, &dst->packets_resolved);
                atomic_add(bytes, &dst->bytes_resolved);

                atomic_add(packets, &iter->entry->packets_resolved);
                atomic_add(bytes, &iter->entry->bytes_resolved);

                atomic_add(packets, &srvtable.packets_resolved);
                atomic_add(bytes, &srvtable.bytes_resolved);

        } else {
                if (iter->last_pos != NULL) {
                        dst = list_entry(iter->last_pos, struct target, lh);
                        atomic_add(-packets, &dst->packets_dropped);
                        atomic_add(-bytes, &dst->bytes_dropped);
                }

                atomic_add(-packets, &iter->entry->packets_dropped);
                atomic_add(-bytes, &iter->entry->bytes_dropped);

                atomic_add(-packets, &srvtable.packets_dropped);
                atomic_add(-bytes, &srvtable.bytes_dropped);
        }
}

int service_iter_get_priority(struct service_iter* iter) 
{
        if (iter == NULL)
                return 0;

        if (iter->last_pos != NULL && iter->set)
                return iter->set->priority;

        return 0;
}

int service_iter_get_flags(struct service_iter *iter)
{
        if (iter == NULL)
                return 0;

        if (iter->last_pos != NULL && iter->set)
                return iter->set->flags;

        return 0;
}

static const char *rule_str[] = {
        [RULE_FORWARD] = "FWD",
        [RULE_DEMUX] = "DMX",
        [RULE_DELAY] = "DLY",
        [RULE_DROP] = "DRP"
};

static const char *rule_to_str(service_rule_type_t type)
{
        return rule_str[type];
}

static const char *protocol_to_str(int protocol)
{
        static char buf[20];
        
        switch (protocol) {
        case IPPROTO_TCP:
                return "TCP";
        case IPPROTO_UDP:
                return "UDP";
        default:
                sprintf(buf, "%d", protocol);
                break;
        }
        
        return buf;
}

static int __service_entry_print(struct radix_node *n, void *arg) 
{
        struct service_entry *se = get_service(n);
        struct print_args {
                char *buf;
                int buflen;
                int totlen;
        } *args = (struct print_args *)arg;
        struct target_set *set;
        struct target *t;
        char *buf = args->buf + args->totlen;
        char dststr[18]; /* Currently sufficient for IPv4 */
        int len = 0, tot_len = 0, find_size = 0;
        char node_buf[sizeof(struct service_id)];
        const char *node_str;
        char tmpbuf[200];
        
        if (!radix_node_is_active(n))
                return 0;
        
        if (args->buflen < 0) {
                buf = tmpbuf;
                args->buflen = sizeof(tmpbuf);
                find_size = 1;
        }

        read_lock_bh(&se->lock);
        
        node_str = radix_node_print(n, node_buf, sizeof(node_buf));

        list_for_each_entry(set, &se->target_set, lh) {
                list_for_each_entry(t, &set->list, lh) {
                        len = snprintf(buf + len, args->buflen - len, 
                                       "%-64s %-4s %-5u %-6u %-6u %-8u %-7u ", 
                                       node_str,
                                       rule_to_str(t->type),
                                       set->flags, 
                                       set->priority, 
                                       t->weight,
                                       atomic_read(&t->packets_resolved),
                                       atomic_read(&t->packets_dropped));

                        tot_len += len;
                        
                        if (find_size)
                                len = 0;
                        else
                                len = tot_len;

                        if (t->type == RULE_DEMUX && t->out.sk) {
                                len = snprintf(buf + len, 
                                               args->buflen - len, 
                                               "%-5s %s\n", 
                                               t->out.sk ? 
                                               "sock" : "NULL",
                                               protocol_to_str(t->out.sk->sk_protocol));
                                
                                tot_len += len;

                                if (find_size)
                                        len = 0;
                                else
                                        len = tot_len;
                        } else if (t->type == RULE_FORWARD && t->out.dev) {
                                len = snprintf(buf + len, 
                                               args->buflen - len, 
                                               "%-5s %s\n",
                                               t->out.dev ? 
                                               t->out.dev->name : "any",
                                               inet_ntop(AF_INET,
                                                         t->dst, 
                                                         dststr, 18));

                                tot_len += len;

                                if (find_size)
                                        len = 0;
                                else
                                        len = tot_len;
                        }
                }
        }
        
        read_unlock_bh(&se->lock);

        args->buflen -= tot_len;
        args->totlen += tot_len;

        return tot_len;
}

int service_entry_print(struct service_entry *se, char *buf, int buflen) 
{
        struct {
                char *buf;
                int buflen;
                int totlen;
        } args = { buf, buflen, 0 };
        
        __service_entry_print(se->node, &args);

        return args.totlen;
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
        int len = 0, find_size = 0;
        char tmp_buf[200];
        struct print_args {
                char *buf;
                int buflen;
                int totlen;
        } args = { buf, buflen, 0 };

        if (buflen < 0) {
                find_size = 1;
                args.buf = tmp_buf;
                args.buflen = 200;
        }

#if defined(OS_USER)
        /* Adding this stuff prints garbage in the kernel */
        len = snprintf(args.buf, args.buflen, "bytes resolved: "
                       "%u packets resolved: %u bytes dropped: "
                       "%u packets dropped %u\n",
                       atomic_read(&srvtable.bytes_resolved),
                       atomic_read(&srvtable.packets_resolved),
                       atomic_read(&srvtable.bytes_dropped),
                       atomic_read(&srvtable.packets_dropped));
        
        args.totlen += len;
        
        /* If we are finding out the buffer size, only
           increment totlen, not len. */

        if (find_size)
                len = 0;
        else
                len = args.totlen;
#endif
        len = snprintf(args.buf + len, args.buflen - len, 
                       "%-64s %-4s %-5s %-6s %-6s %-8s %-7s %s\n", 
                       "prefix", "type", "flags", "prio", "weight", 
                       "resolved", "dropped", "target(s)");
        
        args.totlen += len;
        
        radix_tree_foreach(&srvtable.tree, __service_entry_print, &args);

        return args.totlen;
}

int service_table_print(char *buf, int buflen)
{
        int ret;
        read_lock_bh(&srvtable.lock);
        ret = __service_table_print(buf, buflen);
        read_unlock_bh(&srvtable.lock);
        return ret;
}

static int service_entry_local_match(struct radix_node *n)
{
        struct service_entry *se = get_service(n);
        struct target *t;

        t = __service_entry_get_target(se, RULE_DEMUX, NULL, 0, 
                                       make_target(NULL), NULL, 
                                       MATCH_ANY_PROTOCOL);
        
        if (t && t->out.sk) 
                return 1;

        return 0;
}

static int service_entry_global_match(struct radix_node *n)
{
        struct service_entry *se = get_service(n);        
        struct target *t;

        t = __service_entry_get_target(se, RULE_FORWARD, NULL, 0, 
                                       make_target(NULL), NULL, 
                                       MATCH_NO_PROTOCOL);
        
        if (t)
                return 1;

        return 0;
}

static int service_entry_any_match(struct radix_node *n)
{
        /* Any entry will match */        
        return 1;
}

static struct service_entry *__service_table_find(struct service_table *tbl,
                                                  struct service_id *srvid, 
                                                  rule_match_t match) 
{
        struct service_entry *se = NULL;
        struct radix_node *n;
        int (*func)(struct radix_node *) = NULL;

        if (!srvid)
                return NULL;
        
        switch (match) {
        case RULE_MATCH_LOCAL:
                func = service_entry_local_match;
                break;
        case RULE_MATCH_GLOBAL:
        case RULE_MATCH_EXACT:
                func = service_entry_global_match;
                break;
        case RULE_MATCH_ANY:
                func = service_entry_any_match;
                break;
        }

        n = radix_tree_find(&tbl->tree, srvid->s_sid, func);
        
        if (n) {
                if (match != RULE_MATCH_EXACT ||
                    !radix_node_is_wildcard(n))
                        se = get_service(n);
        }

        return se;
}

static struct service_entry *service_table_find(struct service_table *tbl,
                                                struct service_id *srvid, 
                                                rule_match_t match)
{
        struct service_entry *se = NULL;

        read_lock_bh(&tbl->lock);

        se = __service_table_find(tbl, srvid, match);

        if (se)
                service_entry_hold(se);

        read_unlock_bh(&tbl->lock);

        return se;        
}

static struct sock* service_table_find_sock(struct service_table *tbl, 
                                            struct service_id *srvid,
                                            int protocol) 
{
        struct service_entry *se = NULL;
        struct sock *sk = NULL;
        
        if (!srvid)
                return NULL;
        
        read_lock_bh(&tbl->lock);

        se = __service_table_find(tbl, srvid, RULE_MATCH_LOCAL);
        
        if (se) {
                struct target *t;
                t = __service_entry_get_target(se, RULE_DEMUX, NULL, 0, 
                                               make_target(NULL), 
                                               NULL, protocol);
                
                if (t) {
                        sk = t->out.sk;
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
        tstats->bytes_resolved = atomic_read(&tbl->bytes_resolved);
        tstats->packets_resolved = atomic_read(&tbl->packets_resolved);
        tstats->bytes_dropped = atomic_read(&tbl->bytes_dropped);
        tstats->packets_dropped = atomic_read(&tbl->packets_dropped);
        read_unlock_bh(&tbl->lock);
}

int service_get_id(const struct service_entry *se, struct service_id *srvid)
{
        if (!se)
                return -1;

        memset(srvid, 0, sizeof(*srvid));
        strcpy(srvid->s_sid, radix_node_get_key(se->node));
        return (int)radix_node_get_keylen(se->node);
}

void service_get_stats(struct table_stats* tstats) 
{
        return service_table_get_stats(&srvtable, tstats);
}

struct service_entry *service_find_type(struct service_id *srvid,
                                        rule_match_t match) 
{
        return service_table_find(&srvtable, srvid, match);
}

struct sock *service_find_sock(struct service_id *srvid, int protocol) 
{
        return service_table_find_sock(&srvtable, srvid, protocol);
}

static int service_table_modify(struct service_table *tbl,
                                struct service_id *srvid,
                                service_rule_type_t type,
                                uint16_t flags, 
                                uint32_t priority, 
                                uint32_t weight, 
                                const void *dst,
                                int dstlen, 
                                const void *new_dst,
                                int new_dstlen, 
                                const union target_out out) 
{
        struct radix_node *n;
        int ret = 0;

        if (!srvid)
                return -EINVAL;
        
        read_lock_bh(&tbl->lock);
        
        n = radix_tree_find(&tbl->tree, srvid->s_sid, NULL);
        
        if (n) {
                if (dst || dstlen == 0) {                        
                        ret = service_entry_modify_target(get_service(n), type, 
                                                          flags, priority, 
                                                          weight, dst, dstlen,
                                                          new_dst, new_dstlen,
                                                          out, GFP_ATOMIC);
                }
                goto out;
        }
        
        ret = -EINVAL;

 out: 
        read_unlock_bh(&tbl->lock);
        
        return ret;
}

int service_modify(struct service_id *srvid, 
                   service_rule_type_t type,
                   uint16_t flags,
                   uint32_t priority, 
                   uint32_t weight, 
                   const void *dst, 
                   int dstlen,    
                   const void *new_dst, 
                   int new_dstlen, 
                   const union target_out out) 
{
        return service_table_modify(&srvtable, srvid, type, flags,
                                    priority, weight == 0 ? 1 : weight, 
                                    dst, dstlen, new_dst, new_dstlen, 
                                    out);
}

static int service_table_add(struct service_table *tbl,
                             struct service_id *srvid,
                             service_rule_type_t type,
                             uint16_t flags, 
                             uint32_t priority, 
                             uint32_t weight, 
                             const void *dst,
                             int dstlen, 
                             const union target_out out, 
                             gfp_t alloc) {
        struct service_entry *se;
        struct radix_node *n = NULL;
        struct target_set *set = NULL;
        struct target *t = NULL;
        int ret = 0;
        
        if (!srvid)
                return -EINVAL;

        /* Sanity checks */
        switch (type) {
        case RULE_FORWARD:
                if (dstlen == 0 || dst == NULL)
                        return -EINVAL;
                break;
        case RULE_DEMUX:
                if (dstlen > 0)
                        return -EINVAL;
                break;
        case RULE_DROP:
        case RULE_DELAY:
                LOG_ERR("Rule %s not supported yet!\n",
                        rule_to_str(type));
                return -EINVAL;
        }

        se = service_entry_create(tbl, alloc);

        if (!se)
                return -ENOMEM;

        write_lock_bh(&tbl->lock);

        ret = radix_tree_add(&tbl->tree, srvid->s_sid,
                             se, &n, GFP_ATOMIC);
        
        if (ret == -1) {
                /* Insertion failed, assume memory allocation error */
                service_entry_free(se);
                write_unlock_bh(&tbl->lock);
                return -ENOMEM;
        } else if (ret == 0) {
                /* Found existing service entry - free the newly
                 * created entry and then check if the target exists
                 * in the found one. */
                service_entry_free(se);
                se = get_service(n);

                /* Set node to NULL to indicate that we are using an
                 * existing entry */
                n = NULL;

                /* Hold entry so that we can safely unlock the
                 * table */
                service_entry_hold(se);

                /* Unlock table and lock service entry instead */
                write_unlock_bh(&tbl->lock);
                
                read_lock_bh(&se->lock);

                t = __service_entry_get_target(se, type, dst, dstlen,
                                               out, &set,
                                               dstlen == 0 ? 
                                               out.sk->sk_protocol :
                                               MATCH_NO_PROTOCOL);

                set = __service_entry_get_target_set(se, priority);
                                
                read_unlock_bh(&se->lock);

                if (t) {
                        /* Found existing target, we are trying to
                         * insert a duplicate. */
                        service_entry_put(se);

                        if (is_sock_target(t)) {
                                /* A socket target should return
                                 * EADDRINUSE since this is typically
                                 * a result of a bind() */
                                return -EADDRINUSE;
                        }
                        LOG_INF("Identical service entry already exists\n");
                        return 0;
                }
        } else {
                /* Hold this entry since it is now in the table */
                service_entry_hold(se);
                /* We should add target to new service entry */
                write_unlock_bh(&tbl->lock);
        }

        t = target_create(type, dst, dstlen, out, weight, alloc);
        
        if (!t)
                goto fail_target;

        if (!set) {
                /* No existing set, we must create one */
                set = target_set_create(flags, priority, alloc);
                
                if (!set) {
                        target_free(t);
                        goto fail_target;
                }
                write_lock_bh(&se->lock);
                /* Insert the new set */
                service_entry_insert_target_set(se, set);
        } else {
                write_lock_bh(&se->lock);
        }

        target_set_add_target(set, t);

        se->count++;

        write_unlock_bh(&se->lock);
        
        service_entry_put(se);

        return ret;

fail_target:
        if (n) {
                /* If n is non-NULL, we created the entry
                 * above. Since we failed to create the target,
                 * and there is no existing target, we must
                 * remove the node from the tree and free
                 * it. */
                write_lock_bh(&tbl->lock);
                radix_node_remove(n, GFP_ATOMIC);
                write_unlock_bh(&tbl->lock);
                /* Must put twice to free entry */
                service_entry_put(se);
        }

        service_entry_put(se);

        return -ENOMEM;
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
                service_rule_type_t type,
                uint16_t flags, 
                uint32_t priority,
                uint32_t weight, 
                const void *dst, 
                int dstlen, 
                const union target_out out, 
                gfp_t alloc) 
{
        return service_table_add(&srvtable, srvid, 
                                 type, flags, priority, 
                                 weight == 0 ? 1 : weight, dst, dstlen,
                                 out, alloc);
}

static void service_table_del(struct service_table *tbl, 
                              struct service_id *srvid,
                              gfp_t alloc) 
{
        if (!srvid)
                return;

        write_lock_bh(&tbl->lock);
        radix_tree_remove(&tbl->tree, srvid->s_sid, GFP_ATOMIC);
        write_unlock_bh(&tbl->lock);
}

void service_del(struct service_id *srvid, gfp_t alloc) 
{
        return service_table_del(&srvtable, srvid, alloc);
}

static void service_table_del_target(struct service_table *tbl, 
                                     struct service_id *srvid,
                                     service_rule_type_t type,
                                     const void *dst, 
                                     int dstlen, 
                                     struct target_stats* stats,
                                     gfp_t alloc) 
{
        struct radix_node *n;
        struct service_entry *se;

        read_lock_bh(&tbl->lock);

        n = radix_tree_find(&tbl->tree, srvid->s_sid, NULL);

        if (!n) {
                write_unlock_bh(&tbl->lock);
                return;
        }

        se = get_service(n);
        service_entry_hold(se);
        read_unlock_bh(&tbl->lock);
        
        write_lock_bh(&se->lock);
        
        __service_entry_remove_target(se, type,
                                      dst, dstlen, stats);
        
        if (list_empty(&se->target_set)) {
                /* This remove must be GFP_ATOMIC since we are holding
                 * a lock */
                write_lock(&tbl->lock);
                radix_node_remove(n, GFP_ATOMIC);
                write_unlock(&tbl->lock);
        }

        write_unlock_bh(&se->lock);
        service_entry_put(se);
}

void service_del_target(struct service_id *srvid, 
                        service_rule_type_t type,
                        const void *dst, int dstlen,
                        struct target_stats* stats,
                        gfp_t alloc) 
{
        return service_table_del_target(&srvtable, srvid, type,
                                        dst, dstlen, stats, alloc);
}

static int del_dev_func(struct radix_node *n, void *arg) 
{
        struct service_entry *se = get_service(n);
        char *devname = (char *) arg;
        int ret = 0;
        
        if (!radix_node_is_active(n))
                return 0;

        /* We assume that this function is called with table locked
         * and bottom halves already disabled. Therefore, do not
         * disable/enable bottohalves here. */
        write_lock(&se->lock);
        
        ret = __service_entry_remove_target_by_dev(se, devname);
        
        if (ret == 1 && list_empty(&se->target_set)) {
                /* This remove must be atomic since we are holding locks */
                radix_node_remove(n, GFP_ATOMIC);
                write_unlock(&se->lock);
                service_entry_free(se);
        } else {
                write_unlock(&se->lock);
        }

        return ret;
}

static int service_table_del_dev_all(struct service_table *tbl, 
                                     const char *devname) 
{
        int ret = 0;

        write_lock(&tbl->lock);
        
        ret = radix_tree_foreach(&tbl->tree, del_dev_func, 
                                 (void *) devname);

        write_unlock_bh(&tbl->lock);

        return ret;
}

int service_del_dev_all(const char *devname) 
{
        return service_table_del_dev_all(&srvtable, devname);
}

static int del_target_func(struct radix_node *n, void *arg) 
{
        struct service_entry *se = get_service(n);
        struct _d {
                service_rule_type_t type;
                const void *d_dst;
                int d_len;
        } *d = (struct _d *)arg;
        int ret = 0;

        if (!radix_node_is_active(n))
                return 0;

        /* We assume that this function is called with table locked
         * and bottom halves already disabled. Therefore, do not
         * disable/enable bottohalves here. */
        write_lock(&se->lock);

        ret = __service_entry_remove_target(se, d->type, 
                                            d->d_dst, d->d_len, NULL);

        if (ret == 1 && list_empty(&se->target_set)) {
                radix_node_remove(n, GFP_ATOMIC);
                write_unlock(&se->lock);
                service_entry_free(se);
        } else {
                write_unlock(&se->lock);
        }

        return ret;
}

static int service_table_del_target_all(struct service_table *tbl,
                                        service_rule_type_t type,
                                        const void *dst, int dstlen) 
{
        int ret = 0;
        struct {
                service_rule_type_t type;
                const void *d_dst;
                int d_len;
        } d = { type, dst, dstlen };
        
        write_lock_bh(&tbl->lock);

        ret = radix_tree_foreach(&tbl->tree, del_target_func, &d);
                
        write_unlock_bh(&tbl->lock);

        return ret;
}

int service_del_target_all(service_rule_type_t type, 
                           const void *dst, int dstlen) 
{
        return service_table_del_target_all(&srvtable, type, dst, dstlen);
}

static void service_entry_destroy(struct radix_node *n)
{
        struct service_entry *se = get_service(n);
        LOG_DBG("freeing service entry\n");
        service_entry_put(se);
}

void __service_table_destroy(struct service_table *tbl) 
{
        radix_tree_destroy(&tbl->tree, service_entry_destroy);
}

void service_table_destroy(struct service_table *tbl) 
{
        write_lock_bh(&tbl->lock);
        __service_table_destroy(tbl);
        write_unlock_bh(&tbl->lock);
}

void service_table_init(struct service_table *tbl) 
{
        radix_tree_initialize(&tbl->tree);
        atomic_set(&tbl->packets_resolved, 0);
        atomic_set(&tbl->bytes_resolved, 0);
        atomic_set(&tbl->packets_dropped, 0);
        atomic_set(&tbl->bytes_dropped, 0);
        rwlock_init(&tbl->lock);
}

int __init service_init(void) 
{
        service_table_init(&srvtable);

        return 0;
}

void __exit service_fini(void) 
{
        service_table_destroy(&srvtable);
}
