/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <serval/ctrlmsg.h>
#include <net/sock.h>
#include <serval/debug.h>
#include <ctrl.h>

static struct sock *nl_sk = NULL;
static int peer_pid = -1;

extern ctrlmsg_handler_t handlers[];

static int ctrl_rcv_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	int flags, nlmsglen, skblen, ret = 0;
        struct ctrlmsg *cm;

        skblen = skb->len;
        nlmsglen = nlh->nlmsg_len;

        if (nlmsglen < sizeof(*nlh) || skblen < nlmsglen)
                return -EINVAL;

        peer_pid = nlh->nlmsg_pid;
        flags = nlh->nlmsg_flags;

        cm = (struct ctrlmsg *)NLMSG_DATA(nlh);
        
        if (cm->type >= _CTRLMSG_TYPE_MAX) {
                LOG_ERR("bad message type %u\n",
                        cm->type);
                ret = -EINVAL;
        } else {
                if (handlers[cm->type]) {
                        ret = handlers[cm->type](cm, peer_pid);
                        
                        if (ret < 0) {
                                LOG_ERR("handler failure for msg type %u\n",
                                        cm->type);
                        }
                } else {
                        LOG_ERR("no handler for msg  type %u\n",
                                cm->type);
                }
        }
        
        return ret;
}

static DEFINE_MUTEX(ctrl_mutex);

static void ctrl_rcv_skb(struct sk_buff *skb)
{
	mutex_lock(&ctrl_mutex);
	netlink_rcv_skb(skb, &ctrl_rcv_msg);
	mutex_unlock(&ctrl_mutex);
}

int ctrl_sendmsg(struct ctrlmsg *msg, int peer, int mask)
{
        struct sk_buff *skb;
        struct nlmsghdr *nlh;

        skb = alloc_skb(NLMSG_LENGTH(msg->len), mask);

        if (!skb)
                return -ENOMEM;

        NETLINK_CB(skb).dst_group = 1;
        nlh = (struct nlmsghdr *)skb_put(skb, NLMSG_LENGTH(msg->len));
        nlh->nlmsg_type = NLMSG_SERVAL;
        nlh->nlmsg_len = NLMSG_LENGTH(msg->len);
        
        memcpy(NLMSG_DATA(nlh), msg, msg->len);

        if (peer <= 0) {
                LOG_DBG("Broadcasting netlink msg len=%u\n", msg->len);
                return netlink_broadcast(nl_sk, skb, 0, 1, mask);
        }
        
        LOG_DBG("Unicasting netlink msg len=%u peer=%d\n", 
                msg->len, peer);

        return netlink_unicast(nl_sk, skb, peer, MSG_DONTWAIT);
}

int __init ctrl_init(void)
{
	nl_sk = netlink_kernel_create(&init_net, NETLINK_SERVAL, 1, 
				      ctrl_rcv_skb, NULL, THIS_MODULE);

	if (!nl_sk)
		return -ENOMEM;

        /* Allow non-root daemons to receive notifications. This is
           something that probably shouldn't be allowed in a
           production system. */
	netlink_set_nonroot(NETLINK_SERVAL, NL_NONROOT_RECV);

	return 0;
}

void __exit ctrl_fini(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25)
	struct sock *sk = nl_sk;

	if (sk) {
                nl_sk = NULL;
                sock_release(sk->sk_socket);
	}
#else
        netlink_kernel_release(nl_sk);
#endif
}

MODULE_ALIAS_NET_PF_PROTO(PF_NETLINK, NETLINK_SERVAL);
