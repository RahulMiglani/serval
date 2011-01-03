/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <scaffold/debug.h>
#include <scaffold/lock.h>
#include <scaffold/timer.h>
#include <scaffold/sock.h>
#include <scaffold/wait.h>
#include <pthread.h>

#define RCV_BUF_DEFAULT 1000
#define SND_BUF_DEFAULT 1000

LIST_HEAD(proto_list);
DEFINE_RWLOCK(proto_list_lock);

static void sock_def_destruct(struct sock *sk)
{

}

static void sock_def_wakeup(struct sock *sk)
{

}

static void sock_def_readable(struct sock *sk, int bytes)
{

}

static void sock_def_write_space(struct sock *sk)
{

}

static int sock_def_backlog_rcv(struct sock *sk, struct sk_buff *skb)
{
	return 0;
}

static inline void sock_lock_init(struct sock *sk)
{
	spin_lock_init(&(sk)->sk_lock.slock);
        sk->sk_lock.owned = 0;
}

void sock_init_data(struct socket *sock, struct sock *sk)
{
	skb_queue_head_init(&sk->sk_receive_queue);
	skb_queue_head_init(&sk->sk_write_queue);

	sk->sk_send_head	=	NULL;
	init_timer(&sk->sk_timer);
	sk->sk_net              =       &init_net;
	sk->sk_rcvbuf		=	RCV_BUF_DEFAULT;
	sk->sk_sndbuf		=       SND_BUF_DEFAULT;
	sk->sk_state		=	0;
	sk_set_socket(sk, sock);
	sock_set_flag(sk, SOCK_ZAPPED);
        
        if (sock) {
		sk->sk_type	=	sock->type;
		sk->sk_wq	=	sock->wq;
		sock->sk	=	sk;
	} else
		sk->sk_wq	=	NULL;

	sk->sk_state_change	=	sock_def_wakeup;
	sk->sk_data_ready	=	sock_def_readable;
	sk->sk_write_space	=	sock_def_write_space;
	sk->sk_destruct		=	sock_def_destruct;
	sk->sk_backlog_rcv	=	sock_def_backlog_rcv;
	sk->sk_write_pending	=	0;
	sk->sk_rcvtimeo		=	MAX_SCHEDULE_TIMEOUT;
	sk->sk_sndtimeo		=	MAX_SCHEDULE_TIMEOUT;

        rwlock_init(&sk->sk_callback_lock);
	atomic_set(&sk->sk_refcnt, 1);
	atomic_set(&sk->sk_drops, 0);
}

static struct sock *sk_prot_alloc(struct proto *prot, int family)
{
	struct sock *sk;

	sk = (struct sock *)malloc(prot->obj_size);

	if (sk) {
                memset(sk, 0, prot->obj_size);
	}

	return sk;
}

#define get_net(n) n

static void sock_net_set(struct sock *sk, struct net *net)
{
	/* TODO: make sure this is ok. Should be since we have no
	   network namespaces anyway. */
	sk->sk_net = net;
}

struct sock *sk_alloc(struct net *net, int family, gfp_t priority,
		      struct proto *prot)
{
	struct sock *sk = NULL;

	sk = sk_prot_alloc(prot, family);

	if (sk) {
		sk->sk_family = family;
		/*
		 * See comment in struct sock definition to understand
		 * why we need sk_prot_creator -acme
		 */
		sk->sk_prot = prot;
		sock_lock_init(sk);
		sock_net_set(sk, get_net(net));
		atomic_set(&sk->sk_wmem_alloc, 1);
	}

	return sk;
}

static void __sk_free(struct sock *sk)
{
	if (sk->sk_destruct)
		sk->sk_destruct(sk);

        free(sk);
}

void sk_free(struct sock *sk)

{        
        /*
	 * We substract one from sk_wmem_alloc and can know if
	 * some packets are still in some tx queue.
	 * If not null, sock_wfree() will call __sk_free(sk) later
	 */
	if (atomic_dec_and_test(&sk->sk_wmem_alloc))
		__sk_free(sk);
}

void sock_wfree(struct sk_buff *skb)
{
	struct sock *sk = skb->sk;
	unsigned int len = skb->truesize;

	if (!sock_flag(sk, SOCK_USE_WRITE_QUEUE)) {
		/*
		 * Keep a reference on sk_wmem_alloc, this will be released
		 * after sk_write_space() call
		 */
		atomic_sub(len - 1, &sk->sk_wmem_alloc);
		sk->sk_write_space(sk);
		len = 1;
	}
	/*
	 * if sk_wmem_alloc reaches 0, we must finish what sk_free()
	 * could not do because of in-flight packets
	 */
	if (atomic_sub_and_test(len, &sk->sk_wmem_alloc))
		__sk_free(sk);
}

/*
 * Read buffer destructor automatically called from kfree_skb.
 */
void sock_rfree(struct sk_buff *skb)
{
	struct sock *sk = skb->sk;

	atomic_sub(skb->truesize, &sk->sk_rmem_alloc);
	/* sk_mem_uncharge(skb->sk, skb->truesize); */
}

void sk_common_release(struct sock *sk)
{
	if (sk->sk_prot->destroy)
		sk->sk_prot->destroy(sk);

	sk->sk_prot->unhash(sk);

	sock_orphan(sk);

	sock_put(sk);
}

int proto_register(struct proto *prot, int ignore)
{
	write_lock(&proto_list_lock);
	list_add(&prot->node, &proto_list);
	/* assign_proto_idx(prot); */
	write_unlock(&proto_list_lock);

	return 0;
}

void proto_unregister(struct proto *prot)
{
        write_lock(&proto_list_lock);
	/* release_proto_idx(prot); */
	list_del(&prot->node);
	write_unlock(&proto_list_lock);
}


int sk_wait_data(struct sock *sk, long *timeo)
{
        return 0;
}



void sk_reset_timer(struct sock *sk, struct timer_list* timer,
                    unsigned long expires)
{
}

void sk_stop_timer(struct sock *sk, struct timer_list* timer)
{
}

int sock_queue_rcv_skb(struct sock *sk, struct sk_buff *skb)
{
        return 0;
}

int sock_queue_err_skb(struct sock *sk, struct sk_buff *skb)
{
        return 0;
}

void lock_sock(struct sock *sk)
{
        spin_lock(&sk->sk_lock.slock);
        sk->sk_lock.owned = 1;
}

void __release_sock(struct sock *sk)
{
        struct sk_buff *skb = sk->sk_backlog.head;

        do {
                sk->sk_backlog.head = sk->sk_backlog.tail = NULL;
                bh_unlock_sock(sk);

                do {
                        struct sk_buff *next = skb->next;

                        skb->next = NULL;
                        sk_backlog_rcv(sk, skb);

                        /*
                         * We are in process context here with softirqs
                         * disabled, use cond_resched_softirq() to preempt.
                         * This is safe to do because we've taken the backlog
                         * queue private:
                         */
                        //cond_resched_softirq();

                        skb = next;
                } while (skb != NULL);

                bh_lock_sock(sk);
        } while ((skb = sk->sk_backlog.head) != NULL);

        /*
         * Doing the zeroing here guarantee we can not loop forever
         * while a wild producer attempts to flood us.
         */
        sk->sk_backlog.len = 0;
}

void release_sock(struct sock *sk)
{
        
        sk->sk_lock.owned = 0;
        spin_unlock(&sk->sk_lock.slock);
}
