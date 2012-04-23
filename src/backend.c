/*
 * Copyright (c) 2011 and 2012, Dustin Lundquist <dustin@null-ptr.net>
 * Copyright (c) 2011 Manuel Kasper <mk@neon1.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, 
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <strings.h> /* strncasecmp */
#include <ctype.h> /* tolower */
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <pcre.h>
#include <errno.h>
#include <syslog.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "backend.h"
#include "util.h"


static void free_backend(struct Backend *);


struct Backend *
add_backend(struct Backend_head *head, const char *hostname, const char *address, int port) {
    struct Backend *backend;
    const char *reerr;
    int reerroffset;
    char *ch;

    backend = calloc(1, sizeof(struct Backend));
    if (backend == NULL) {
        syslog(LOG_CRIT, "calloc failed");
        return NULL;
    }

    backend->hostname = strdup(hostname);
    if (backend->hostname == NULL) {
        syslog(LOG_CRIT, "strdup failed");
        free_backend(backend);
        return NULL;
    }

    backend->address = strdup(address);
    if (backend->address == NULL) {
        syslog(LOG_CRIT, "strdup failed");
        free_backend(backend);
        return NULL;
    }
    /* Store address as lower case */
    for (ch = backend->address; *ch == '\0'; ch++)
        *ch = tolower(*ch);
    

    backend->port = port;

    backend->hostname_re = pcre_compile(hostname, 0, &reerr, &reerroffset, NULL);
    if (backend->hostname_re == NULL) {
        syslog(LOG_CRIT, "Regex compilation failed: %s, offset %d", reerr, reerroffset);
        free_backend(backend);
        return NULL;
    }

    syslog(LOG_DEBUG, "Parsed %s %s %d", hostname, address, port);
    STAILQ_INSERT_TAIL(head, backend, entries);

    return backend;
}

struct Backend *
lookup_backend(const struct Backend_head *head, const char *hostname) {
    struct Backend *iter;

    if (hostname == NULL)
        hostname = "";

    STAILQ_FOREACH(iter, head, entries) {
        if (pcre_exec(iter->hostname_re, NULL, hostname, strlen(hostname), 0, 0, NULL, 0) >= 0) {
            syslog(LOG_DEBUG, "%s matched %s", iter->hostname, hostname);
            return iter;
        } else {
            syslog(LOG_DEBUG, "%s didn't match %s", iter->hostname, hostname);
        }
    }
    return NULL;
}

void
remove_backend(struct Backend_head *head, struct Backend *backend) {
    STAILQ_REMOVE(head, backend, Backend, entries);
    free_backend(backend);
}

static void
free_backend(struct Backend *backend) {
    if (backend->hostname != NULL)
        free(backend->hostname);
    if (backend->address != NULL)
        free(backend->address);
    if (backend->hostname_re != NULL)
        pcre_free(backend->hostname_re);
    free(backend);
}

int
open_backend_socket(struct Backend *b, const char *req_hostname) {
    int sockfd = -1, error;
    struct addrinfo hints, *results, *iter;
    const char *cause = NULL;
    char portstr[6]; /* port numbers are < 65536 */

    const char *target_hostname = b->address;
    if (strcmp(target_hostname, "*") == 0)
        target_hostname = req_hostname;

    snprintf(portstr, 6, "%d", b->port);
    syslog(LOG_DEBUG, "Connecting to %s:%s", target_hostname, portstr);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    error = getaddrinfo(target_hostname, portstr, &hints, &results);
    if (error != 0) {
        syslog(LOG_NOTICE, "Lookup error: %s", gai_strerror(error));
        return -1;
    }

    for (iter = results; iter; iter = iter->ai_next) {
        sockfd = socket(iter->ai_family, iter->ai_socktype, iter->ai_protocol);
        if (sockfd < 0) {
            cause = "socket";
            continue;
        }

        if (connect(sockfd, iter->ai_addr, iter->ai_addrlen) < 0) {
            cause = "connect";
            close(sockfd);
            sockfd = -1;
            continue;
        }

        break;  /* okay we got one */
    }
    if (sockfd < 0)
        syslog(LOG_ERR, "%s error: %s", cause, strerror(errno));

    freeaddrinfo(results);
    return sockfd;
}
