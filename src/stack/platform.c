/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <serval/platform.h>
#include <serval/debug.h>
#if defined(OS_LINUX_KERNEL)
#include <linux/time.h>
#endif
#if defined(OS_USER)
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#endif

/* Taken from Click */
uint16_t in_cksum(const void *data, size_t len)
{
        int nleft = len;
        const uint16_t *w = (const uint16_t *)data;
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

const char *mac_ntop(const void *src, char *dst, size_t size)
{	
	const char *mac = (const char *)src;

	if (size < 18)
		return NULL;

	sprintf(dst, "%02x:%02x:%02x:%02x:%02x:%02x",
		mac[0] & 0xff, 
                mac[1] & 0xff, 
                mac[2] & 0xff, 
                mac[3] & 0xff, 
                mac[4] & 0xff, 
                mac[5] & 0xff);

	return dst;
}

int mac_pton(const char *src, void *dst)
{
        return -1;
}

const char *get_strtime(void)
{
    static char buf[30];
    struct timeval now;
#if defined(OS_LINUX_KERNEL)
    do_gettimeofday(&now);
#endif
#if defined(OS_USER)
    gettimeofday(&now, NULL);
#endif

    snprintf(buf, 30, "%ld.%03ld", 
             (long)now.tv_sec, (long)(now.tv_usec / 1000));

    return buf;
}

#if defined(OS_LINUX_KERNEL)
#include <linux/inet.h>

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
        unsigned char *ip = (unsigned char *)src;

        if (size < 16 || af != AF_INET)
                return NULL;
        
        sprintf(dst, "%u.%u.%u.%u", 
                ip[0], ip[1], ip[2], ip[3]);
        
        return dst;
}
#endif

#if defined(OS_USER)

/* From http://groups.google.com/group/comp.lang.c/msg/52820a5d19679089 */
/***********************************************/
/* Locate the position of the highest bit set. */
/* A binary search is used.  The result is an  */
/* approximation of log2(n) [the integer part] */
/***********************************************/
int ilog2(unsigned long n)
{
        int i = (-1);
        /* Is there a bit on in the high word? */
        /* Else, all the high bits are already zero. */
        if (n & 0xffff0000) {
                i += 16;                /* Update our search position */
                n >>= 16;               /* Shift out lower (irrelevant) bits */
        }
        /* Is there a bit on in the high byte of the current word? */
        /* Else, all the high bits are already zero. */
        if (n & 0xff00) {
                i += 8;                 /* Update our search position */
                n >>= 8;                /* Shift out lower (irrelevant) bits */
        }
        /* Is there a bit on in the current nybble? */
        /* Else, all the high bits are already zero. */
        if (n & 0xf0) {
                i += 4;                 /* Update our search position */
                n >>= 4;                /* Shift out lower (irrelevant) bits */
        }
        /* Is there a bit on in the high 2 bits of the current nybble? */
        /* 0xc is 1100 in binary... */
        /* Else, all the high bits are already zero. */
        if (n & 0xc) {
                i += 2;                 /* Update our search position */
                n >>= 2;                /* Shift out lower (irrelevant) bits */
        }
        /* Is the 2nd bit on? [ 0x2 is 0010 in binary...] */
        /* Else, all the 2nd bit is already zero. */
        if (n & 0x2) {
                i++;                    /* Update our search position */
                n >>= 1;                /* Shift out lower (irrelevant) bit */
        }
        /* Is the lowest bit set? */
        if (n)
                i++;                    /* Update our search position */
        return i;
}

int memcpy_toiovec(struct iovec *iov, unsigned char *from, int len)
{
        if (!memcpy(iov->iov_base, from, len)) 
                return -EFAULT;

        iov->iov_len = len;

        return 0;
}

int memcpy_fromiovec(unsigned char *to, struct iovec *iov, int len)
{
        if (!memcpy(to, iov->iov_base, iov->iov_len))
                return -EFAULT;

        return 0;
}

int memcpy_fromiovecend(unsigned char *kdata, const struct iovec *iov,
			int offset, int len)
{
	/* Skip over the finished iovecs */
	while ((unsigned int)offset >= iov->iov_len) {
		offset -= iov->iov_len;
		iov++;
	}

	while (len > 0) {
		uint8_t *base = (uint8_t *)iov->iov_base + offset;
		int copy = min_t(unsigned int, len, iov->iov_len - offset);

		offset = 0;
		if (memcpy(kdata, base, copy))
			return -EFAULT;
		len -= copy;
		kdata += copy;
		iov++;
	}

	return 0;
}

#if !defined(HAVE_PPOLL)
#include <poll.h>
#include <signal.h>

int ppoll(struct pollfd fds[], nfds_t nfds, struct timespec *timeout, 
          sigset_t *set)
{
        int to = 0;
        sigset_t oldset;
        int ret;

        if (!timeout) {
                to = -1;
        } else if (timeout->tv_sec == 0 && timeout->tv_nsec == 0)  {
                to = 0;
        } else {
                to = timeout->tv_sec * 1000 + (timeout->tv_nsec / 1000000);
        }

        if (set) {
                /* TODO: make these operations atomic. */
                sigprocmask(SIG_SETMASK, set, &oldset);
                ret = poll(fds, nfds, to);
                sigprocmask(SIG_SETMASK, &oldset, NULL);
        } else {
                ret = poll(fds, nfds, to);
        }
        return ret;
}

#endif /* OS_ANDROID */

#endif /* OS_USER */
