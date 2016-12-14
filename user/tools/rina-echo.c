/*
 * Copyright (C) 2015-2016 Nextworks
 * Author: Vincenzo Maffione <v.maffione@gmail.com>
 *
 * This file is part of rlite.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/time.h>
#include <assert.h>
#include <endian.h>
#include <signal.h>
#include <poll.h>
#include <sys/select.h>

#include <rina/api.h>


#define SDU_SIZE_MAX    65535
#define MAX_CLIENTS     3

struct rl_rr {
    int cfd;
    const char *cli_appl_name;
    const char *srv_appl_name;
    char *dif_name;
    struct rina_flow_spec flowspec;
};

#define MAX(a,b) ((a)>(b) ? (a) : (b))

struct selfd {
#define SELFD_S_ALLOC   1
#define SELFD_S_WRITE   2
#define SELFD_S_READ    3
#define SELFD_S_NONE    4
#define SELFD_S_ACCEPT  5
    int state;
    int fd;
};

static int
client(struct rl_rr *rr)
{
    const char *msg = "Hello guys, this is a test message!";
    char buf[SDU_SIZE_MAX];
    struct selfd sfds[1];
    fd_set rdfs, wrfs;
    int maxfd;
    int p = 1;
    int ret;
    int size;
    int i;

    /* Start flow allocations in parallel, without waiting for completion. */

    for (i = 0; i < p; i ++) {
        sfds[i].state = SELFD_S_ALLOC;
        sfds[i].fd = rina_flow_alloc(rr->dif_name, rr->cli_appl_name,
                                     rr->srv_appl_name, &rr->flowspec,
                                     RINA_F_NOWAIT);
        if (sfds[i].fd < 0) {
            perror("rina_flow_alloc()");
            return sfds[i].fd;
        }
    }

    for (;;) {
        FD_ZERO(&rdfs);
        FD_ZERO(&wrfs);
        maxfd = 0;

        for (i = 0; i < p; i++) {
            switch (sfds[i].state) {
            case SELFD_S_WRITE:
                FD_SET(sfds[i].fd, &wrfs);
                break;
            case SELFD_S_READ:
            case SELFD_S_ALLOC:
                FD_SET(sfds[i].fd, &rdfs);
                break;
            case SELFD_S_NONE:
                /* Do nothing */
                break;
            }

            maxfd = MAX(maxfd, sfds[i].fd);
        }

        if (maxfd <= 0) {
            /* Nothing more to process. */
            break;
        }

        ret = select(maxfd + 1, &rdfs, &wrfs, NULL, NULL);
        if (ret < 0) {
            perror("select()\n");
            return ret;
        } else if (ret == 0) {
            /* Timeout */
            printf("Timeout occurred\n");
            break;
        }

        for (i = 0; i < p; i++) {
            switch (sfds[i].state) {
            case SELFD_S_ALLOC:
                if (FD_ISSET(sfds[i].fd, &rdfs)) {
                    /* Complete flow allocation, replacing the fd. */
                    sfds[i].fd = rina_flow_alloc_wait(sfds[i].fd);
                    if (sfds[i].fd < 0) {
                        perror("rina_flow_alloc_wait()");
                        return sfds[i].fd;
                    }
                    sfds[i].state = SELFD_S_WRITE;
                    printf("Flow %d allocated\n", i);
                }
                break;

            case SELFD_S_WRITE:
                if (FD_ISSET(sfds[i].fd, &wrfs)) {
                    strncpy(buf, msg, SDU_SIZE_MAX);
                    size = strlen(buf) + 1;

                    ret = write(sfds[i].fd, buf, size);
                    if (ret != size) {
                        if (ret < 0) {
                            perror("write(buf)");
                        } else {
                            printf("Partial write %d/%d\n", ret, size);
                        }
                    }
                    sfds[i].state = SELFD_S_READ;
                }
                break;

            case SELFD_S_READ:
                if (FD_ISSET(sfds[i].fd, &rdfs)) {
                    /* Ready to read. */
                    ret = read(sfds[i].fd, buf, sizeof(buf));
                    if (ret < 0) {
                        perror("read(buf");
                    }
                    buf[ret] = '\0';
                    printf("Response: '%s'\n", buf);
                    close(sfds[i].fd);
                    sfds[i].fd = -1;

                    sfds[i].state = SELFD_S_NONE;
                    printf("Flow %d deallocated\n", i);
                }
                break;
            }
        }
    }

    return 0;
}

static int
server(struct rl_rr *rr)
{
    struct selfd sfds[MAX_CLIENTS + 1];
    char buf[SDU_SIZE_MAX];
    fd_set rdfs, wrfs;
    int n, ret;
    int maxfd;
    int i;

    /* In listen mode also register the application names. */
    ret = rina_register(rr->cfd, rr->dif_name, rr->srv_appl_name);
    if (ret) {
        perror("rina_register()");
        return ret;
    }

    sfds[0].state = SELFD_S_ACCEPT;
    sfds[0].fd = rr->cfd;

    for (i = 1; i <= MAX_CLIENTS; i++) {
        sfds[i].state = SELFD_S_NONE;
        sfds[i].fd = -1;
    }

    for (;;) {
        FD_ZERO(&rdfs);
        FD_ZERO(&wrfs);
        maxfd = 0;

        for (i = 0; i <= MAX_CLIENTS; i++) {
            switch (sfds[i].state) {
            case SELFD_S_WRITE:
                FD_SET(sfds[i].fd, &wrfs);
                break;
            case SELFD_S_READ:
            case SELFD_S_ACCEPT:
                FD_SET(sfds[i].fd, &rdfs);
                break;
            case SELFD_S_NONE:
                /* Do nothing */
                break;
            }

            maxfd = MAX(maxfd, sfds[i].fd);
        }

        assert(maxfd >= 0);

        ret = select(maxfd + 1, &rdfs, &wrfs, NULL, NULL);
        if (ret < 0) {
            perror("select()\n");
            return ret;
        } else if (ret == 0) {
            /* Timeout */
            printf("Timeout occurred\n");
            break;
        }

        for (i = 0; i <= MAX_CLIENTS; i++) {
            switch (sfds[i].state) {
            case SELFD_S_ACCEPT:
                if (FD_ISSET(sfds[i].fd, &rdfs)) {
                    int handle;
                    int j;

                    /* Look for a free slot. */
                    for (j = 1; j <= MAX_CLIENTS; j++) {
                        if (sfds[j].state == SELFD_S_NONE) {
                            break;
                        }
                    }

                    /* Receive flow allocation request without
                     * responding. */
                    handle = rina_flow_accept(sfds[i].fd, NULL, NULL,
                                              RINA_F_NORESP);
                    if (handle < 0) {
                        perror("rina_flow_accept()");
                        return handle;
                    }

                    /* Respond positively if we have found a slot. */
                    sfds[j].fd = rina_flow_respond(sfds[i].fd, handle,
                                                   j > MAX_CLIENTS ? -1 : 0);
                    if (sfds[j].fd < 0) {
                        perror("rina_flow_respond()");
                        return sfds[j].fd;
                    }

                    if (j > MAX_CLIENTS) {
                        sfds[j].state = SELFD_S_NONE;
                        sfds[j].fd = -1;
                    } else {
                        sfds[j].state = SELFD_S_READ;
                        printf("Accept client %d\n", j);
                    }
                }
                break;

            case SELFD_S_READ:
                if (FD_ISSET(sfds[i].fd, &rdfs)) {
                    /* File descriptor is ready for reading. */
                    n = read(sfds[i].fd, buf, sizeof(buf));
                    if (n < 0) {
                        perror("read(flow)");
                        return -1;
                    }

                    buf[n] = '\0';
                    printf("Request: '%s'\n", buf);

                    sfds[i].state = SELFD_S_WRITE;
                }
                break;

            case SELFD_S_WRITE:
                if (FD_ISSET(sfds[i].fd, &wrfs)) {
                    ret = write(sfds[i].fd, buf, n);
                    if (ret != n) {
                        if (ret < 0) {
                            perror("write(flow)");
                        } else {
                            printf("partial write");
                        }
                        return -1;
                    }

                    printf("Response sent back\n");
                    close(sfds[i].fd);
                    sfds[i].state = SELFD_S_NONE;
                    sfds[i].fd = -1;
                    printf("Close client %d\n", i);
                }
            }
        }
    }

    return 0;
}

static void
sigint_handler(int signum)
{
    exit(EXIT_SUCCESS);
}

static void
usage(void)
{
    printf("rl_rr [OPTIONS]\n"
        "   -h : show this help\n"
        "   -l : run in server mode (listen)\n"
        "   -d DIF : name of DIF to which register or ask to allocate a flow\n"
        "   -a APNAME : application process name/instance of the rl_rr client\n"
        "   -z APNAME : application process name/instance of the rl_rr server\n"
        "   -g NUM : max SDU gap to use for the data flow\n"
          );
}

int
main(int argc, char **argv)
{
    struct sigaction sa;
    struct rl_rr rr;
    const char *dif_name = NULL;
    int listen = 0;
    int ret;
    int opt;

    rr.cli_appl_name = "rl_rr-data:client";
    rr.srv_appl_name = "rl_rr-data:server";

    /* Start with a default flow configuration (unreliable flow). */
    rina_flow_spec_default(&rr.flowspec);

    while ((opt = getopt(argc, argv, "hld:a:z:g:")) != -1) {
        switch (opt) {
            case 'h':
                usage();
                return 0;

            case 'l':
                listen = 1;
                break;

            case 'd':
                dif_name = optarg;
                break;

            case 'a':
                rr.cli_appl_name = optarg;
                break;

            case 'z':
                rr.srv_appl_name = optarg;
                break;

            case 'g': /* Set max_sdu_gap flow specification parameter. */
                rr.flowspec.max_sdu_gap = atoll(optarg);
                break;

            default:
                printf("    Unrecognized option %c\n", opt);
                usage();
                return -1;
        }
    }

    /* Set some signal handler */
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret = sigaction(SIGINT, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        return ret;
    }
    ret = sigaction(SIGTERM, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGTERM)");
        return ret;
    }

    /* Initialization of RLITE application. */
    rr.cfd = rina_open();
    if (rr.cfd < 0) {
        perror("rina_open()");
        return rr.cfd;
    }

    rr.dif_name = dif_name ? strdup(dif_name) : NULL;

    if (listen) {
        server(&rr);

    } else {
        client(&rr);
    }

    return close(rr.cfd);
}
