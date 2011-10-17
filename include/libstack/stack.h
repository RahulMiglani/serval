/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#ifndef _LIBSTACK_H
#define _LIBSTACK_H

#include <netinet/serval.h>
#include "callback.h"
#include "ctrlmsg.h"

int libstack_migrate_interface(const char *from_if,
		                       const char *to_if);

int libstack_migrate_flow(struct flow_id *from_flow,
                          const char *to_if);

int libstack_migrate_service(struct service_id *from_service,
                             const char *to_if);

int libstack_add_service(const struct service_id *srvid, 
                         unsigned int prefix_bits,
                         const struct in_addr *ipaddr);

int libstack_del_service(const struct service_id *srvid, 
                         unsigned int prefix_bits,
                         const struct in_addr *ipaddr);
int libstack_init(void);
void libstack_fini(void);

#endif /* LIBSTACK_H */
