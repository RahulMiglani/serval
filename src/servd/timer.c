/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <poll.h>
#include "timer.h"
#include "debug.h"

#define USECS_PER_SEC 1000000L

#define timeval_normalize(t) {                          \
                if ((t)->tv_usec >= USECS_PER_SEC) {    \
                        (t)->tv_usec -= USECS_PER_SEC;  \
                        (t)->tv_sec++;                  \
                } else if ((t)->tv_usec < 0) {          \
                        (t)->tv_usec += USECS_PER_SEC;  \
                        (t)->tv_sec--;                  \
                }                                       \
        }

#define timeval_add_usec(t1, usec) do {                 \
                (t1)->tv_sec += usec / USECS_PER_SEC;   \
                (t1)->tv_usec += usec % USECS_PER_SEC;  \
                timeval_normalize(t1);                  \
        } while (0)

#define timeval_add(t1, t2) do {                \
                (t1)->tv_usec += (t2)->tv_usec; \
                (t1)->tv_sec += (t2)->tv_sec;   \
                timeval_normalize(t1);          \
        } while (0)

#define timeval_sub(t1, t2) do {                \
                (t1)->tv_usec -= (t2)->tv_usec; \
                (t1)->tv_sec -= (t2)->tv_sec;   \
                timeval_normalize(t1);          \
        } while (0)

#define timeval_nz(t) ((t)->tv_sec != 0 || (t)->tv_usec != 0)
#define timeval_lt(t1, t2) ((t1)->tv_sec < (t2)->tv_sec || \
                            ((t1)->tv_sec == (t2)->tv_sec &&    \
                             (t1)->tv_usec < (t2)->tv_usec))
#define timeval_gt(t1, t2) (timeval_lt(t2, t1))
#define timeval_ge(t1, t2) (!timeval_lt(t1, t2))
#define timeval_le(t1, t2) (!timeval_gt(t1, t2))
#define timeval_eq(t1, t2) ((t1)->tv_sec == (t2)->tv_sec &&     \
                            (t1)->tv_usec == (t2)->tv_usec)

static struct list_head timer_list = { &timer_list, &timer_list };
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static int pipefd[2] = { -1, -1 };
static pthread_t main_thr;

struct timer *timer_new_callback(int (*callback)(struct timer *), 
				 void *data)
{
	struct timer *t = malloc(sizeof(struct timer));

	if (!t)
		return NULL;

	timer_init(t);
	t->callback = callback;
	t->data = data;
	
	return t;
}

int timer_list_get_signal(void)
{
        return pipefd[0];
}

static int timer_list_signal_raise(void)
{
        char s = 'w';
        struct pollfd fds;
        int ret = 0;
        
        memset(&fds, 0, sizeof(fds));
        fds.fd = pipefd[0];
        fds.events = POLLIN;
        fds.revents = 0;

        ret = poll(&fds, 1, 0);

        if (ret == 1) {
                /* Signal already raised */
                return 0;
        } else if (ret == 0)  {
                ret = write(pipefd[1], &s, 1);
        }

        return ret;
}

static int timer_list_signal_lower(void)
{
        struct pollfd fds;
        char s = 0;
        int ret = 0;
        
        memset(&fds, 0, sizeof(fds));
        fds.fd = pipefd[0];
        fds.events = POLLIN;
        fds.revents = 0;

        ret = poll(&fds, 1, 0);

        if (ret == 1) {
                ret = read(pipefd[0], &s, 1);
        }

        return ret;
}

void timer_free(struct timer *t)
{
	free(t);
}

void timer_init(struct timer *t)
{
	memset(t, 0, sizeof(*t));
	INIT_LIST_HEAD(&t->lh);
}

int timer_add(struct timer *t)
{
        struct timer *pos;

	if (timer_scheduled(t))
		return -1;
        
        gettimeofday(&t->timeout, NULL);        
        timeval_add_usec(&t->timeout, t->expires);

	pthread_mutex_lock(&timer_lock);        
        list_for_each_entry(pos, &timer_list, lh) {
                if (timeval_lt(&t->timeout, &pos->timeout)) {
                        list_add(&t->lh, &pos->lh);
                        goto found;
                }
        }

        list_add_tail(&t->lh, &timer_list);
found:

        /* If another thread than the main thread
         * added a new timer first in the list, then
         * raise the signal to make the main thread
         * reschedule itself to reflect the new
         * timeout */
        if (!pthread_equal(main_thr, pthread_self()) &&
            timer_list.next == &t->lh) {
                timer_list_signal_raise();
        }

	pthread_mutex_unlock(&timer_lock);
	
	return 1;
}

static void _timer_del(struct timer *t)
{        
	list_del(&t->lh);
	INIT_LIST_HEAD(&t->lh);
}

void timer_del(struct timer *t)
{
	pthread_mutex_lock(&timer_lock);
        _timer_del(t);
	pthread_mutex_unlock(&timer_lock);
}

int timer_next_timeout(unsigned long *timeout)
{
	struct timer *t;
        struct timeval now, later;

	pthread_mutex_lock(&timer_lock);

	if (list_empty(&timer_list)) {
		pthread_mutex_unlock(&timer_lock);
                timer_list_signal_lower();
		return 0;
	}

        gettimeofday(&now, NULL);

	t = list_first_entry(&timer_list, struct timer, lh);       
        memcpy(&later, &t->timeout, sizeof(t->timeout));
        timeval_sub(&later, &now);
	*timeout = later.tv_sec * 1000000 + later.tv_usec;

	pthread_mutex_unlock(&timer_lock);

        timer_list_signal_lower();

	return 1;
}

int timer_next_timeout_timeval(struct timeval *timeout)
{
	struct timer *t;
        struct timeval now;

	pthread_mutex_lock(&timer_lock);

	if (list_empty(&timer_list)) {
		pthread_mutex_unlock(&timer_lock);
		return 0;
	}

        gettimeofday(&now, NULL);

	t = list_first_entry(&timer_list, struct timer, lh);
	memcpy(timeout, &t->timeout, sizeof(*timeout));
        timeval_sub(timeout, &now);

        if (timeout->tv_sec < 0)
                timeout->tv_sec = timeout->tv_usec = 0;

	pthread_mutex_unlock(&timer_lock);

	return 1;
}

int timer_handle_timeout(void)
{
	struct timer *t;
	int ret = 0;
       
	pthread_mutex_lock(&timer_lock);

	if (list_empty(&timer_list)) {
		pthread_mutex_unlock(&timer_lock);
		return 0;
	}
	
	t = list_first_entry(&timer_list, struct timer, lh);
	list_del(&t->lh);
	INIT_LIST_HEAD(&t->lh);

	pthread_mutex_unlock(&timer_lock);

	if (t->callback)
		ret = t->callback(t);

	return ret;
}

void timer_list_destroy(void)
{
	pthread_mutex_lock(&timer_lock);

	while (1) {
		struct timer *t;
		
		if (list_empty(&timer_list))
			break;
		
		t = list_first_entry(&timer_list, struct timer, lh);
		if (t->destruct)
                        t->destruct(t);
                _timer_del(t);
        }
	pthread_mutex_unlock(&timer_lock);	
}

int timer_list_init(void)
{
        int ret;

        ret = pipe(pipefd);

        if (ret == -1) {
                LOG_ERR("pipe failed: %s\n",
                        strerror(errno));
        }

        main_thr = pthread_self();

        return ret;
}

void timer_list_fini(void)
{
        if (pipefd[0] != -1)
                close(pipefd[0]);
        
        if (pipefd[1] != -1)
                close(pipefd[1]);

        timer_list_destroy();
}
