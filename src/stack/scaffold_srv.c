/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <scaffold/platform.h>
#include <scaffold/platform_tcpip.h>
#include <scaffold/skbuff.h>
#include <scaffold/debug.h>
#include <scaffold_sock.h>
#include <scaffold/netdevice.h>
#include <scaffold_srv.h>
#include <scaffold_ipv4.h>
#include <netinet/scaffold.h>
#if defined(OS_LINUX_KERNEL)
#include <linux/if_ether.h>
#elif !defined(OS_ANDROID)
#include <netinet/if_ether.h>
#endif
#if defined(OS_USER)
#include <signal.h>
#endif
#include "service.h"

extern int scaffold_tcp_rcv(struct sk_buff *);
extern int scaffold_udp_rcv(struct sk_buff *);

static int scaffold_srv_syn_rcv(struct sock *sk, 
                                struct scaffold_hdr *sfh,
                                struct sk_buff *skb)
{
        struct scaffold_request_sock *rsk;
        struct scaffold_service_ext *srv_ext = 
                (struct scaffold_service_ext *)(sfh + 1);
        unsigned int hdr_len = ntohs(sfh->length);
        int err = 0;
        
        /* Cache this service FIXME: should not assume ETH_ALEN here. */
        err = service_add(&srv_ext->src_srvid, sizeof(srv_ext->src_srvid), 
                          skb->dev, 
                          SCAFFOLD_SKB_CB(skb)->hard_addr, 
                          ETH_ALEN, GFP_ATOMIC);
        
        if (err < 0) {
                LOG_ERR("could not cache service for incoming packet\n");
        }
        
        if (sk->sk_ack_backlog >= sk->sk_max_ack_backlog) 
                goto drop;
        
        err = scaffold_sk(sk)->af_ops->conn_request(sk, skb);
        
        if (err < 0)
                goto drop;

        rsk = scaffold_rsk_alloc(GFP_ATOMIC);

        if (!rsk) {
                bh_unlock_sock(sk);
                return -ENOMEM;
        }
        
        list_add(&rsk->lh, &scaffold_sk(sk)->syn_queue);
        
        SCAFFOLD_SKB_CB(skb)->pkttype = SCAFFOLD_PKT_SYNACK;

        /* Push back the Scaffold header again to make IP happy */
        skb_push(skb, hdr_len);
        skb_reset_transport_header(skb);

        memcpy(&sfh->dst_sid, &sfh->src_sid, sizeof(sfh->src_sid));
        memcpy(&scaffold_sk(sk)->peer_sockid, &sfh->src_sid, sizeof(sfh->src_sid));
        memcpy(&sfh->src_sid, &scaffold_sk(sk)->local_sockid, 
               sizeof(scaffold_sk(sk)->local_sockid));

        memcpy(&scaffold_sk(sk)->peer_srvid, &srv_ext->src_srvid, 
               sizeof(srv_ext->src_srvid));

        sfh->flags |= SFH_ACK;

        err = scaffold_ipv4_build_and_send_pkt(skb, sk, 
                                               ip_hdr(skb)->saddr, NULL);

done:        
        return err;
drop:
        FREE_SKB(skb);
        goto done;
}

static struct scaffold_request_sock *
scaffold_srv_request_sock_handle(struct sock *sk,
                                 struct sock_id *sid)
{
        return NULL;
}

static int scaffold_srv_listen_state_process(struct sock *sk,
                                             struct scaffold_hdr *sfh,
                                             struct sk_buff *skb)
{
        int err = 0;                         

        if (sfh->flags & SFH_ACK) {
                struct scaffold_request_sock *rsk;
                LOG_DBG("ACK recv\n");
                rsk = scaffold_srv_request_sock_handle(sk, &sfh->dst_sid);

                /*
                sk->sk_state_change(sk);
                
                if (sk->sk_socket)
                        sk_wake_async(sk, SOCK_WAKE_IO, POLL_OUT);
                */
                FREE_SKB(skb);
        } else if (sfh->flags & SFH_SYN) {
                LOG_DBG("SYN recv\n");
                err = scaffold_srv_syn_rcv(sk, sfh, skb);
        }

        return err;
}

static int scaffold_srv_request_state_process(struct sock *sk, 
                                              struct scaffold_hdr *sfh,
                                              struct sk_buff *skb)
{
        int err = 0;
        
        scaffold_sock_set_state(sk, SCAFFOLD_CONNECTED);
        
        sk->sk_state_change(sk);
        sk_wake_async(sk, SOCK_WAKE_IO, POLL_OUT);

        sfh->flags = 0;
        sfh->flags |= SFH_ACK;

        err = scaffold_ipv4_build_and_send_pkt(skb, sk, 
                                               ip_hdr(skb)->saddr, NULL);

        return err;
}

static int scaffold_srv_connected_state_process(struct sock *sk, 
                                                struct scaffold_hdr *sfh,
                                                struct sk_buff *skb)
{
        int err = 0;
        
        return err;
}

static int scaffold_srv_do_rcv(struct sock *sk, 
                               struct scaffold_hdr *sfh, 
                               struct sk_buff *skb)
{
        int err = 0;

        switch (sk->sk_state) {
        case SCAFFOLD_CONNECTED:
                err = scaffold_srv_connected_state_process(sk, sfh, skb);
                break;
        case SCAFFOLD_REQUEST:
                err = scaffold_srv_request_state_process(sk, sfh, skb);
                break;
        case SCAFFOLD_LISTEN:
                err = scaffold_srv_listen_state_process(sk, sfh, skb);
                break;
        default:
                goto drop;
        }

        return err;
drop:
        FREE_SKB(skb);

        return err;
}

int scaffold_srv_rcv(struct sk_buff *skb)
{
        struct sock *sk = NULL;
        struct scaffold_hdr *sfh = 
                (struct scaffold_hdr *)skb_transport_header(skb);
        unsigned int hdr_len = ntohs(sfh->length);
        int err = 0;

        if (!pskb_may_pull(skb, hdr_len))
                goto drop;
        
        pskb_pull(skb, hdr_len);
        skb_reset_transport_header(skb);

        if (hdr_len < sizeof(struct scaffold_hdr))
                goto drop;

        /* If SYN and not ACK is set, we know for sure that we must
         * demux on service id */
        if (!(sfh->flags & SFH_SYN && !(sfh->flags & SFH_ACK))) {
                /* Ok, check if we can demux on socket id */
                sk = scaffold_sock_lookup_sockid(&sfh->dst_sid);
        }

        if (!sk) {
                /* Try to demux on service id */
                struct scaffold_service_ext *srv_ext = 
                        (struct scaffold_service_ext *)(sfh + 1);

                /* Check for service extension. We require that this
                 * extension always directly follows the main Scaffold
                 * header */
                if (hdr_len <= sizeof(struct scaffold_hdr))
                        goto drop;
                
                if (srv_ext->type != SCAFFOLD_SERVICE_EXT)
                        goto drop;
                
                sk = scaffold_sock_lookup_serviceid(&srv_ext->dst_srvid);
                
                if (!sk) {
                        LOG_ERR("No matching scaffold sock\n");
                        goto drop;
                }
        }
 
        bh_lock_sock_nested(sk);

        if (!sock_owned_by_user(sk)) {
                err = scaffold_srv_do_rcv(sk, sfh, skb);
        } 
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,34)
        else {
                sk_add_backlog(sk, skb);
        }
#else
        else if (unlikely(sk_add_backlog(sk, skb))) {
                bh_unlock_sock(sk);
                sock_put(sk);
                goto drop;
        }
#endif
        bh_unlock_sock(sk);
        sock_put(sk);

	return err;
drop:
        FREE_SKB(skb);
        return err;
}

#define EXTRA_HDR (20)
#define SCAFFOLD_MAX_HDR (MAX_HEADER + 20 +                             \
                          sizeof(struct scaffold_hdr) +                 \
                          sizeof(struct scaffold_service_ext) +         \
                          EXTRA_HDR)
/*
int scaffold_srv_connect(struct sock *sk, struct sk_buff *skb)
{
        struct scaffold_sock *ssk = scaffold_sk(sk);
        struct scaffold_hdr *sfh;
        struct scaffold_service_ext *srvext;
        struct sk_buff *skb;
        
        skb = ALLOC_SKB(SCAFFOLD_MAX_HDR, sk->sk_allocation);
        
        if (!skb)
                return -ENOBUFS;
        
	skb_set_owner_w(skb, sk);
        skb_reserve(skb, SCAFFOLD_MAX_HDR);

        srvext = (struct scaffold_service_ext *)skb_push(skb, sizeof(struct scaffold_service_ext));
        
        sfh = (struct scaffold_hdr *)skb_push(skb, sizeof(struct scaffold_hdr));
        
        memcpy(&sfh->dst_sid, &ssk->sockid, sizeof(ssk->sockid));
        SCAFFOLD_SKB_CB(skb)->pkttype = SCAFFOLD_PKT_DATA;
        
        FREE_SKB(skb);

        return 0;
}
*/

int scaffold_srv_xmit_skb(struct sock *sk, struct sk_buff *skb)
{
        struct scaffold_sock *ssk = scaffold_sk(sk);
	struct service_entry *se;
	struct net_device *dev;
        struct scaffold_hdr *sfh;
        struct scaffold_service_ext *srvext;
	int err = 0;

        srvext = (struct scaffold_service_ext *)skb_push(skb, sizeof(struct scaffold_service_ext));

        /* Add Scaffold service extension */
        srvext->type = SCAFFOLD_SERVICE_EXT;
        srvext->length = htons(sizeof(struct scaffold_service_ext));
        srvext->flags = 0;

        memcpy(&srvext->src_srvid, &ssk->local_srvid, sizeof(ssk->local_srvid));
        memcpy(&srvext->dst_srvid, &SCAFFOLD_SKB_CB(skb)->srvid, 
               sizeof(SCAFFOLD_SKB_CB(skb)->srvid));

        /* Add Scaffold header */
        sfh = (struct scaffold_hdr *)skb_push(skb, sizeof(struct scaffold_hdr));
        sfh->flags = 0;
        sfh->protocol = skb->protocol;
        sfh->length = htons(sizeof(struct scaffold_service_ext) + 
                            sizeof(struct scaffold_hdr));
        memcpy(&sfh->src_sid, &ssk->local_sockid, sizeof(ssk->local_sockid));
        memcpy(&sfh->src_sid, &ssk->peer_sockid, sizeof(ssk->peer_sockid));

	if (sk->sk_state == SCAFFOLD_CONNECTED) {
		err = scaffold_ipv4_xmit_skb(sk, skb);
                
		if (err < 0) {
			LOG_ERR("xmit failed\n");
			FREE_SKB(skb);
		}
		return err;
	}

	/* Unresolved packet, use service id */
	se = service_find(&SCAFFOLD_SKB_CB(skb)->srvid);
	
	if (!se) {
		LOG_ERR("service lookup failed\n");
		FREE_SKB(skb);
		return -EADDRNOTAVAIL;
	}

        /* Set appropriate flags. */
        switch (SCAFFOLD_SKB_CB(skb)->pkttype) {
        case SCAFFOLD_PKT_SYNACK:
                sfh->flags |= SFH_ACK;
        case SCAFFOLD_PKT_SYN:
                sfh->flags |= SFH_SYN;
                break;
        case SCAFFOLD_PKT_ACK:
                sfh->flags |= SFH_ACK;
                break;
        default:
                break;
        }
        skb->protocol = IPPROTO_SCAFFOLD;
        /* 
           Send on all interfaces listed for this service.
        */
	service_entry_dev_iterate_begin(se);
	
	dev = service_entry_dev_next(se);
	
	while (dev) {
		struct sk_buff *cskb;
		struct net_device *next_dev;
		
                LOG_DBG("tx dev=%s\n", dev->name);

		/* Remember the hardware destination */
		service_entry_dev_dst(se, SCAFFOLD_SKB_CB(skb)->hard_addr, 6);

		next_dev = service_entry_dev_next(se);
		
                if (next_dev == NULL) {
			cskb = skb;
		} else {
			cskb = skb_clone(skb, current ? 
					 GFP_KERNEL : GFP_ATOMIC);
			
			if (!cskb) {
				LOG_ERR("Allocation failed\n");
				FREE_SKB(skb);
				break;
			}
		}
                
		/* Set the output device */
		skb_set_dev(cskb, dev);

		err = scaffold_ipv4_xmit_skb(sk, cskb);
                
		if (err < 0) {
			LOG_ERR("xmit failed\n");
			FREE_SKB(cskb);
			break;
		}
		dev = next_dev;
	}
	
	service_entry_dev_iterate_end(se);
	service_entry_put(se);

	return err;
}

