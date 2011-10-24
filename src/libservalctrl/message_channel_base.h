/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * message_channel.h
 *
 *  Created on: Feb 11, 2011
 *      Author: daveds
 */

#ifndef MESSAGE_CHANNEL_BASE_H_
#define MESSAGE_CHANNEL_BASE_H_

#include <common/platform.h>
#include <libservalctrl/message_channel.h>
#include <libservalctrl/task.h>
#include <netinet/in.h>
#include <sys/un.h>
#if defined(OS_LINUX)
#include <linux/netlink.h>
#endif
#include <netinet/serval.h>

typedef union {
    struct sockaddr sa;
    struct sockaddr_sv sv;
    struct sockaddr_un un;
#if defined(OS_LINUX)
    struct sockaddr_nl nl;
#endif
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
    struct {
        struct sockaddr_sv sv;
        union {
            struct sockaddr_in in;
            struct sockaddr_in in6;
        };
    } sv_in;
} channel_addr_t;

#define RECV_BUFFER_SIZE 2048

typedef struct message_channel_base {
    struct message_channel channel;
    int sock;
    int sock_type;
    int protocol;
    int running;
    int native_socket;
    channel_addr_t local;
    socklen_t local_len;
    channel_addr_t peer;
    socklen_t peer_len;
    task_handle_t task;
    /* receive buffer */
    size_t buffer_len;
    unsigned char *buffer;
} message_channel_base_t;

#define MAX_SEND_RETRIES 10

message_channel_base_t *message_channel_base_create(channel_key_t *key,
                                                    message_channel_ops_t *ops);
int message_channel_base_init(message_channel_base_t *base,
                              message_channel_type_t type, 
                              int sock_type,
                              int protocol,
                              const struct sockaddr *local,
                              socklen_t local_len,
                              const struct sockaddr *peer,
                              socklen_t peer_len,
                              message_channel_ops_t *ops);
int message_channel_base_equalfn(const message_channel_t *channel, const void *_key);
int message_channel_base_fillkey(const message_channel_t *channel, void *_key);
int message_channel_base_initialize(message_channel_t *channel);
void message_channel_base_finalize(message_channel_t *channel);
int message_channel_base_start(message_channel_t *channel);
void message_channel_base_stop(message_channel_t *channel);
int message_channel_base_get_local(message_channel_t *channel,
                                   struct sockaddr *addr,
                                   socklen_t *addrlen);
int message_channel_base_get_peer(message_channel_t *channel,
                                  struct sockaddr *addr,
                                  socklen_t *addrlen);
int message_channel_base_set_peer(message_channel_t *channel, 
                                  const struct sockaddr *addr, socklen_t len);
int message_channel_base_send_iov(message_channel_t *channel, struct iovec *iov,
                                  size_t veclen, size_t msglen);
int message_channel_base_send(message_channel_t *channel, 
                              void *msg, size_t msglen);

#endif /* MESSAGE_CHANNEL_BASE_H_ */
