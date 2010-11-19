/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <scaffold/platform.h>
#include <scaffold/skbuff.h>
#include <scaffold_sock.h>
#include <scaffold_ipv4.h>
#include <input.h>

extern int scaffold_tcp_rcv(struct sk_buff *);
extern int scaffold_udp_rcv(struct sk_buff *);

/* Taken from Click */
uint16_t in_cksum(const unsigned char *addr, int len)
{
    int nleft = len;
    const uint16_t *w = (const uint16_t *)addr;
    uint32_t sum = 0;
    uint16_t answer = 0;

    /*
     * Our algorithm is simple, using a 32 bit accumulator (sum), we add
     * sequential 16 bit words to it, and at the end, fold back all the
     * carry bits from the top 16 bits into the lower 16 bits.
     */
    while (nleft > 1)  {
            sum += *w++;
            nleft -= 2;
    }
    
    /* mop up an odd byte, if necessary */
    if (nleft == 1) {
            *(unsigned char *)(&answer) = *(const unsigned char *)w ;
            sum += answer;
    }
    
    /* add back carry outs from top 16 bits to low 16 bits */
    sum = (sum & 0xffff) + (sum >> 16);
    sum += (sum >> 16);
    /* guaranteed now that the lower 16 bits of sum are correct */

    answer = ~sum;              /* truncate to 16 bits */
    return answer;
}

int scaffold_ipv4_rcv(struct sk_buff *skb)
{
	struct iphdr *iph = ip_hdr(skb);
	unsigned int hdr_len = iph->ihl << 2;
	int ret = INPUT_OK;
	char srcstr[18];
       
	LOG_DBG("received IPv4 packet from %s hdr_len=%u prot=%u\n",
		inet_ntop(AF_INET, &iph->saddr, srcstr, 18), 
		hdr_len, iph->protocol);

        /* Check if this is not a SCAFFOLD packet */
        if (1 /* !is_scaffold_packet */)
                return INPUT_DELIVER;

        if (!pskb_may_pull(skb, hdr_len))
                goto inhdr_error;
        
        skb_pull(skb, hdr_len);

	skb_reset_transport_header(skb);

	switch (iph->protocol) {
	case IPPROTO_ICMP:
		LOG_DBG("icmp packet\n");
                ret = INPUT_DELIVER;
		break;
	case IPPROTO_UDP:
                ret = scaffold_udp_rcv(skb);
		break;
	case IPPROTO_TCP:
                ret = scaffold_tcp_rcv(skb);
		break;
	default:
		LOG_DBG("packet type=%u\n", iph->protocol);
	}

	return ret;
inhdr_error:
        return INPUT_DROP;

}

int scaffold_ipv4_xmit_skb(struct sock *sk, struct sk_buff *skb)
{
        struct iphdr *iph = ip_hdr(skb);

        skb_push(skb, sizeof(struct iphdr));
	skb_reset_network_header(skb);

        /* Add IP header */
        iph->ihl = 5;

        /* Add MAC header */

        /* Transmit */

        return -1;
}

