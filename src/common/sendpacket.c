/* $Id$ */

/*
 *   Copyright (c) 2001-2010 Aaron Turner <aturner at synfin dot net>
 *   Copyright (c) 2013-2014 Fred Klassen <tcpreplay at appneta dot com> - AppNeta Inc.
 *
 *   The Tcpreplay Suite of tools is free software: you can redistribute it 
 *   and/or modify it under the terms of the GNU General Public License as 
 *   published by the Free Software Foundation, either version 3 of the 
 *   License, or with the authors permission any later version.
 *
 *   The Tcpreplay Suite is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with the Tcpreplay Suite.  If not, see <http://www.gnu.org/licenses/>.
 */

 /* sendpacket.[ch] is my attempt to write a universal packet injection
  * API for BPF, libpcap, libdnet, and Linux's PF_PACKET.  I got sick
  * and tired dealing with libnet bugs and its lack of active maintenence,
  * but unfortunately, libpcap frame injection support is relatively new
  * and not everyone uses Linux, so I decided to support all four as
  * best as possible.  If your platform/OS/hardware supports an additional
  * injection method, then by all means add it here (and send me a patch).
  *
  * Anyways, long story short, for now the order of preference is:
  * 1. TX_RING
  * 2. PF_PACKET
  * 3. BPF
  * 4. libdnet
  * 5. pcap_inject()
  * 6. pcap_sendpacket()
  *
  * Right now, one big problem with the pcap_* methods is that libpcap
  * doesn't provide a reliable method of getting the MAC address of
  * an interface (required for tcpbridge).
  * You can use PF_PACKET or BPF to get that, but if your system suports
  * those, might as well inject directly without going through another
  * level of indirection.
  *
  * Please note that some of this code was copied from Libnet 1.1.3
  */

#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/socket.h>

#include "config.h"
#include "defines.h"
#include "common.h"
#include "sendpacket.h"

#ifdef FORCE_INJECT_TX_RING
/* TX_RING uses PF_PACKET API so don't undef it here */
#undef HAVE_LIBDNET
#undef HAVE_PCAP_INJECT
#undef HAVE_PCAP_SENDPACKET
#undef HAVE_BPF
#endif

#ifdef FORCE_INJECT_PF_PACKET
#undef HAVE_TX_RING
#undef HAVE_LIBDNET
#undef HAVE_PCAP_INJECT
#undef HAVE_PCAP_SENDPACKET
#undef HAVE_BPF
#endif

#ifdef FORCE_INJECT_LIBDNET
#undef HAVE_TX_RING
#undef HAVE_PF_PACKET
#undef HAVE_PCAP_INJECT
#undef HAVE_PCAP_SENDPACKET
#undef HAVE_BPF
#endif

#ifdef FORCE_INJECT_BPF
#undef HAVE_TX_RING
#undef HAVE_LIBDNET
#undef HAVE_PCAP_INJECT
#undef HAVE_PCAP_SENDPACKET
#undef HAVE_PF_PACKET
#endif

#ifdef FORCE_INJECT_PCAP_INJECT
#undef HAVE_TX_RING
#undef HAVE_LIBDNET
#undef HAVE_PCAP_SENDPACKET
#undef HAVE_BPF
#undef HAVE_PF_PACKET
#endif

#ifdef FORCE_INJECT_PCAP_SENDPACKET
#undef HAVE_TX_RING
#undef HAVE_LIBDNET
#undef HAVE_PCAP_INJECT
#undef HAVE_BPF
#undef HAVE_PF_PACKET
#endif

#if (defined HAVE_WINPCAP && defined HAVE_PCAP_INJECT)
#undef HAVE_PCAP_INJECT /* configure returns true for some odd reason */
#endif

#if !defined HAVE_PCAP_INJECT && !defined HAVE_PCAP_SENDPACKET && !defined HAVE_LIBDNET && !defined HAVE_PF_PACKET && !defined HAVE_BPF && !defined TX_RING
#error You need pcap_inject() or pcap_sendpacket() from libpcap, libdnet, Linux's PF_PACKET/TX_RING or *BSD's BPF
#endif


#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif
#ifdef HAVE_NET_ROUTE_H
#include <net/route.h>
#endif
#include <stdlib.h>
#include <unistd.h>

#ifdef HAVE_NETMAP
#include <sys/mman.h>
#ifdef linux
#   include <linux/ethtool.h>
#   include <linux/sockios.h>
#endif /* linux */
static int nm_do_ioctl (sendpacket_t *sp, u_long what, int subcmd);
static sendpacket_t *sendpacket_open_netmap(const char *device, char *errbuf);
#if NETMAP_API >= 10
#define NETMAP_TX_RING_EMPTY nm_ring_empty
#else
#define nm_ring_space(ring) \
    (ring->avail)
#endif /* NETMAP_API < 10 */
#endif /* HAVE_NETMAP */

#ifdef HAVE_PF_PACKET
#undef INJECT_METHOD

/* give priority to TX_RING */
#ifndef HAVE_TX_RING
#define INJECT_METHOD "PF_PACKET send()"
#else
#define INJECT_METHOD "PF_PACKET / TX_RING"
#endif

#include <fcntl.h>
#include <sys/utsname.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_ether.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>

#ifdef HAVE_TX_RING
#include "txring.h"
#endif

#ifndef __GLIBC__
typedef int socklen_t;
#endif

static sendpacket_t *sendpacket_open_pf(const char *, char *);
static struct tcpr_ether_addr *sendpacket_get_hwaddr_pf(sendpacket_t *);
static int get_iface_index(int fd, const char *device, char *);

#endif /* HAVE_PF_PACKET */

#if defined HAVE_BPF && ! defined INJECT_METHOD
#undef INJECT_METHOD
#define INJECT_METHOD "bpf send()"

#include <net/bpf.h>
#include <sys/sysctl.h>
#include <net/route.h>
#include <net/if.h>
#include <sys/uio.h>
#include <net/if_dl.h> // used for get_hwaddr_bpf()

static sendpacket_t *sendpacket_open_bpf(const char *, char *) _U_;
static struct tcpr_ether_addr *sendpacket_get_hwaddr_bpf(sendpacket_t *) _U_;

#endif /* HAVE_BPF */

#if defined HAVE_LIBDNET && ! defined INJECT_METHOD
#undef INJECT_METHOD
#define INJECT_METHOD "libdnet eth_send()"
/* need to undef these which are pulled in via defines.h, prior to importing dnet.h */
#undef icmp_id
#undef icmp_seq
#undef icmp_data
#undef icmp_mask
#ifdef HAVE_DNET_H
#include <dnet.h>
#endif
#ifdef HAVE_DUMBNET_H
#include <dumbnet.h>
#endif

static sendpacket_t *sendpacket_open_libdnet(const char *, char *) _U_;
static struct tcpr_ether_addr *sendpacket_get_hwaddr_libdnet(sendpacket_t *) _U_;
#endif /* HAVE_LIBDNET */

#if (defined HAVE_PCAP_INJECT || defined HAVE_PCAP_SENDPACKET) && !(defined HAVE_PF_PACKET || defined BPF || defined HAVE_LIBDNET)
static sendpacket_t *sendpacket_open_pcap(const char *, char *) _U_;
static struct tcpr_ether_addr *sendpacket_get_hwaddr_pcap(sendpacket_t *) _U_;
#endif /* HAVE_PCAP_INJECT || HAVE_PACKET_SENDPACKET */

#if defined HAVE_PCAP_INJECT && ! defined INJECT_METHOD
#undef INJECT_METHOD
#define INJECT_METHOD "pcap_inject()"
#elif defined HAVE_PCAP_SENDPACKET && ! defined INJECT_METHOD
#undef INJECT_METHOD
#define INJECT_METHOD "pcap_sendpacket()"
#endif

static void sendpacket_seterr(sendpacket_t *sp, const char *fmt, ...);
static sendpacket_t * sendpacket_open_khial(const char *, char *) _U_;
static struct tcpr_ether_addr * sendpacket_get_hwaddr_khial(sendpacket_t *) _U_;

#ifdef HAVE_NETMAP
static inline uint32_t get_netmap_buf_avail(struct netmap_ring *ring)
{
    uint32_t avail;
#if NETMAP_API > 4
            avail = nm_ring_space(ring);
#else
            avail = ring->avail;
#endif

    return avail;
}
#endif /* HAVE_NETMAP */

/**
 * returns number of bytes sent on success or -1 on error
 * Note: it is theoretically possible to get a return code >0 and < len
 * which for most people would be considered an error (the packet wasn't fully sent)
 * so you may want to test for recode != len too.
 *
 * Most socket API's have two interesting errors: ENOBUFS & EAGAIN.  ENOBUFS
 * is usually due to the kernel buffers being full.  EAGAIN happens when you
 * try to send traffic faster then the PHY allows.
 */
int
sendpacket(sendpacket_t *sp, const u_char *data, size_t len, struct pcap_pkthdr *pkthdr)
{
    int retcode = 0, val;
    static u_char buffer[10000]; /* 10K bytes, enough for jumbo frames + pkthdr
                                  * larger than page size so made static to
                                  * prevent page misses on stack
                                  */
    static const size_t buffer_payload_size = sizeof(buffer) + sizeof(struct pcap_pkthdr);
#ifdef HAVE_NETMAP
    struct netmap_ring *txring;
    struct netmap_slot *slot;
    char *p;
    uint32_t cur, avail;
    bool tx_queue_empty;
#endif

    assert(sp);
    assert(data);

    if (len <= 0)
        return -1;

TRY_SEND_AGAIN:
    sp->attempt ++;


    switch (sp->handle_type) {
        case SP_TYPE_KHIAL:

            memcpy(buffer, pkthdr, sizeof(struct pcap_pkthdr));
            memcpy(buffer + sizeof(struct pcap_pkthdr), data, min(len, buffer_payload_size));

            /* tell the kernel module which direction the traffic is going */
            if (sp->cache_dir == TCPR_DIR_C2S) {  /* aka PRIMARY */
                val = KHIAL_DIRECTION_RX;
                if (ioctl(sp->handle.fd, KHIAL_SET_DIRECTION, (void *)&val) < 0) {
                    sendpacket_seterr(sp, "Error setting direction on %s: %s (%d)",
                            sp->device, strerror(errno), errno);
                    return -1;
                }
            } else if (sp->cache_dir == TCPR_DIR_S2C) {
                val = KHIAL_DIRECTION_TX;
                if (ioctl(sp->handle.fd, KHIAL_SET_DIRECTION, (void *)&val) < 0) {
                    sendpacket_seterr(sp, "Error setting direction on %s: %s (%d)",
                            sp->device, strerror(errno), errno);
                    return -1;
                }
            }

            /* write the pkthdr + packet data all at once */
            retcode = write(sp->handle.fd, (void *)buffer, sizeof(struct pcap_pkthdr) + len);
            retcode -= sizeof(struct pcap_pkthdr); /* only record packet bytes we sent, not pcap data too */
                    
            if (retcode < 0 && !sp->abort) {
                switch(errno) {
                    case EAGAIN:
                        sp->retry_eagain ++;
                        goto TRY_SEND_AGAIN;
                        break;
                    case ENOBUFS:
                        sp->retry_enobufs ++;
                        goto TRY_SEND_AGAIN;
                        break;
                    default:
                        sendpacket_seterr(sp, "Error with %s [" COUNTER_SPEC "]: %s (errno = %d)",
                                "khial", sp->sent + sp->failed + 1, strerror(errno), errno);
                }
                break;
            }

            break;

            /* Linux PF_PACKET and TX_RING */
        case SP_TYPE_PF_PACKET:
        case SP_TYPE_TX_RING:
#if defined HAVE_PF_PACKET
#ifdef HAVE_TX_RING
            retcode = (int)txring_put(sp->tx_ring, data, len);
#else
            retcode = (int)send(sp->handle.fd, (void *)data, len, 0);
#endif

            /* out of buffers, or hit max PHY speed, silently retry
             * as long as we're not told to abort
             */
            if (retcode < 0 && !sp->abort) {
                switch (errno) {
                    case EAGAIN:
                        sp->retry_eagain ++;
                        goto TRY_SEND_AGAIN;
                        break;
                    case ENOBUFS:
                        sp->retry_enobufs ++;
                        goto TRY_SEND_AGAIN;
                        break;

                    default:
                        sendpacket_seterr(sp, "Error with %s [" COUNTER_SPEC "]: %s (errno = %d)", 
                                INJECT_METHOD, sp->sent + sp->failed + 1, strerror(errno), errno);
                }
            }

#endif /* HAVE_PF_PACKET */

            break;


        /* BPF */
        case SP_TYPE_BPF:
#if defined HAVE_BPF
            retcode = write(sp->handle.fd, (void *)data, len);

            /* out of buffers, or hit max PHY speed, silently retry */
            if (retcode < 0 && !sp->abort) {
                switch (errno) {
                    case EAGAIN:
                        sp->retry_eagain ++;
                        goto TRY_SEND_AGAIN;
                        break;

                    case ENOBUFS:
                        sp->retry_enobufs ++;
                        goto TRY_SEND_AGAIN;
                        break;

                    default:
                        sendpacket_seterr(sp, "Error with %s [" COUNTER_SPEC "]: %s (errno = %d)", 
                                INJECT_METHOD, sp->sent + sp->failed + 1, strerror(errno), errno);
                }
            }
#endif
            break;

        /* Libdnet */
        case SP_TYPE_LIBDNET:

#if defined HAVE_LIBDNET
            retcode = eth_send(sp->handle.ldnet, (void*)data, (size_t)len);

            /* out of buffers, or hit max PHY speed, silently retry */
            if (retcode < 0 && !sp->abort) {
                switch (errno) {
                    case EAGAIN:
                        sp->retry_eagain ++;
                        goto TRY_SEND_AGAIN;
                        break;

                    case ENOBUFS:
                        sp->retry_enobufs ++;
                        goto TRY_SEND_AGAIN;
                        break;

                    default:
                        sendpacket_seterr(sp, "Error with %s [" COUNTER_SPEC "]: %s (errno = %d)", 
                                INJECT_METHOD, sp->sent + sp->failed + 1, strerror(errno), errno);
                }
            }
#endif
            break;

        case SP_TYPE_LIBPCAP:
#if (defined HAVE_PCAP_INJECT || defined HAVE_PCAP_SENDPACKET)
#if defined HAVE_PCAP_INJECT
            /* 
             * pcap methods don't seem to support ENOBUFS, so we just straight fail
             * is there a better way???
             */
            retcode = pcap_inject(sp->handle.pcap, (void*)data, len);
#elif defined HAVE_PCAP_SENDPACKET
            retcode = pcap_sendpacket(sp->handle.pcap, data, (int)len);
#endif

            /* out of buffers, or hit max PHY speed, silently retry */
            if (retcode < 0 && !sp->abort) {
                switch (errno) {
                    case EAGAIN:
                        sp->retry_eagain ++;
                        goto TRY_SEND_AGAIN;
                        break;

                    case ENOBUFS:
                        sp->retry_enobufs ++;
                        goto TRY_SEND_AGAIN;
                        break;

                    default:
                        sendpacket_seterr(sp, "Error with %s [" COUNTER_SPEC "]: %s (errno = %d)", 
                                INJECT_METHOD, sp->sent + sp->failed + 1, pcap_geterr(sp->handle.pcap), errno);
                }
            }
#if defined HAVE_PCAP_SENDPACKET
            /* 
             * pcap_sendpacket returns 0 on success, not the packet length! 
             * hence, we have to fix retcode to be more standard on success
             */
            if (retcode == 0)
                retcode = len;
#endif /* HAVE_PCAP_SENDPACKET */

#endif /* HAVE_PCAP_INJECT || HAVE_PCAP_SENDPACKET */

            break;

        case SP_TYPE_NETMAP:
#ifdef HAVE_NETMAP
            txring = NETMAP_TXRING(sp->nm_if, 0);
            avail = get_netmap_buf_avail(txring);
            while (avail == 0) {
                struct pollfd pfd;

                /* send TX interrupt signal just in case */
                ioctl(sp->handle.fd, NIOCTXSYNC, NULL);
                pfd.fd = sp->handle.fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                if (poll(&pfd, 1, 1000) <= 0) {
                    if (sp->abort)
                        return retcode;

                    dbgx(2, "netmap timeout empty=%d avail=%u bufsize=%d\n",
                            NETMAP_TX_RING_EMPTY(txring),
                            avail, txring->nr_buf_size);
                    goto TRY_SEND_AGAIN;
                }

                /*
                 * Do not remove this even though it looks redundant.
                 * Overall performance is increased with this restart
                 * of the TX queue.
                 */
                ioctl(sp->handle.fd, NIOCTXSYNC, NULL);
                avail = get_netmap_buf_avail(txring);

                dbgx(2, "netmap poll empty=%d avail=%u bufsize=%d\n",
                        NETMAP_TX_RING_EMPTY(txring),
                        avail, txring->nr_buf_size);
            }

            /*
             * send
             */
            cur = txring->cur;
            slot = &txring->slot[cur];
            p = NETMAP_BUF(txring, slot->buf_idx);
            memcpy(p, data, min(len, txring->nr_buf_size));
            slot->len = len;

            /* let kernel know that packet is available */
#if NETMAP_API >= 10
            dbgx(3, "netmap cur=%d slot index=%d flags=0x%x empty=%d avail=%u bufsize=%d\n",
                    cur, slot->buf_idx, slot->flags, NETMAP_TX_RING_EMPTY(txring),
                    nm_ring_space(txring), txring->nr_buf_size);
            cur = nm_ring_next(txring, cur);
            tx_queue_empty = nm_ring_empty(txring);
            txring->head = cur;
#else
            dbgx(3, "netmap cur=%d slot index=%d flags=0x%x empty=%d avail=%u bufsize=%d\n",
                    cur, slot->buf_idx, slot->flags, NETMAP_TX_RING_EMPTY(txring),
                    txring->avail, txring->nr_buf_size);
            cur = NETMAP_RING_NEXT(txring, cur);
            tx_queue_empty = NETMAP_TX_RING_EMPTY(txring);
            txring->avail--;
#endif
            txring->cur = cur;
            retcode = len;

            /*
             *  If the queue is empty, tell netmap that packets are ready to TX
             */
            if (tx_queue_empty)
                ioctl(sp->handle.fd, NIOCTXSYNC, NULL); /* generate TX interrupt */

#endif /* HAVE_NETMAP */
            break;

        default:
            errx(-1, "Unsupported sp->handle_type = %d", sp->handle_type);
    } /* end case */

    if (retcode < 0) {
        sp->failed ++;
    } else if (retcode != (int)len) {
        sendpacket_seterr(sp, "Only able to write %d bytes out of %u bytes total",
                retcode, len);
        sp->trunc_packets ++;
    } else {
        sp->bytes_sent += len;
        sp->sent ++;
    }
    return retcode;
}

/**
 * Open the given network device name and returns a sendpacket_t struct
 * pass the error buffer (in case there's a problem) and the direction
 * that this interface represents
 */
sendpacket_t *
sendpacket_open(const char *device, char *errbuf, tcpr_dir_t direction,
        sendpacket_type_t sendpacket_type)
{
    sendpacket_t *sp;
    struct stat sdata;

    assert(device);
    assert(errbuf);

    errbuf[0] = '\0';
    /* khial is universal */
    if (stat(device, &sdata) == 0) {
        if (((sdata.st_mode & S_IFMT) == S_IFCHR)) { 

            sp = sendpacket_open_khial(device, errbuf);

        } else {
            switch (sdata.st_mode & S_IFMT) {
              case S_IFBLK:
                  errx(-1, "\"%s\" is a block device and is not a valid Tcpreplay device",
                      device);
                  break;
                  break;
              case S_IFDIR:
                  errx(-1, "\"%s\" is a directory and is not a valid Tcpreplay device",
                      device);
                  break;
              case S_IFIFO:
                  errx(-1, "\"%s\" is a FIFO and is not a valid Tcpreplay device",
                      device);
                  break;
              case S_IFLNK:
                  errx(-1, "\"%s\" is a symbolic link and is not a valid Tcpreplay device",
                      device);
                  break;
              case S_IFREG:
                  errx(-1, "\"%s\" is a file and is not a valid Tcpreplay device",
                      device);
                  break;
              default:
                  errx(-1, "\"%s\" is not a valid Tcpreplay device",
                      device);
                break;
              }
        }
    } else {
#ifdef HAVE_NETMAP
        if (sendpacket_type == SP_TYPE_NETMAP)
            sp = sendpacket_open_netmap(device, errbuf);
        else
#endif
#if defined HAVE_PF_PACKET
            sp = sendpacket_open_pf(device, errbuf);
#elif defined HAVE_BPF
            sp = sendpacket_open_bpf(device, errbuf);
#elif defined HAVE_LIBDNET
            sp = sendpacket_open_libdnet(device, errbuf);
#elif (defined HAVE_PCAP_INJECT || defined HAVE_PCAP_SENDPACKET)
            sp = sendpacket_open_pcap(device, errbuf);
#else
#error "No defined packet injection method for sendpacket_open()"
#endif
    }

    if (sp) {
        sp->open = 1;
        sp->cache_dir = direction;
    } else {
        errx(-1, "failed to open device %s", device);
    }
    return sp;
}

/**
 * Get packet stats for the given sendpacket_t
 */
size_t
sendpacket_getstat(sendpacket_t *sp, char *buf, size_t buf_size)
{
    size_t offset;

    assert(sp);
    assert(buf);

    memset(buf, 0, buf_size);
    offset = snprintf(buf, buf_size, "Statistics for network device: %s\n"
            "\tAttempted packets:         " COUNTER_SPEC "\n"
            "\tSuccessful packets:        " COUNTER_SPEC "\n"
            "\tFailed packets:            " COUNTER_SPEC "\n"
            "\tTruncated packets:         " COUNTER_SPEC "\n"
            "\tRetried packets (ENOBUFS): " COUNTER_SPEC "\n"
            "\tRetried packets (EAGAIN):  " COUNTER_SPEC "\n",
            sp->device, sp->attempt, sp->sent, sp->failed, sp->trunc_packets,
            sp->retry_enobufs, sp->retry_eagain);

    if (sp->flow_packets && offset > 0) {
        offset += snprintf(&buf[offset], buf_size - offset,
                "\tFlows total:               " COUNTER_SPEC "\n"
                "\tFlows unique:              " COUNTER_SPEC "\n"
                "\tFlows expired:             " COUNTER_SPEC "\n"
                "\tFlow packets:              " COUNTER_SPEC "\n"
                "\tNon-flow packets:          " COUNTER_SPEC "\n"
                "\tInvalid flow packets:      " COUNTER_SPEC "\n",
                sp->flows, sp->flows_expired, sp->flows_expired, sp->flow_packets,
                sp->flow_non_flow_packets, sp->flows_invalid_packets);
    }

    return offset;
}

/**
 * close the given sendpacket
 */
int
sendpacket_close(sendpacket_t *sp)
{
    assert(sp);
    switch(sp->handle_type) {
        case SP_TYPE_KHIAL:
            close(sp->handle.fd);
            break;

        case SP_TYPE_BPF:
#if (defined HAVE_PCAP_INJECT || defined HAVE_PCAP_SENDPACKET)
            close(sp->handle.fd);
#endif
            break;

        case SP_TYPE_PF_PACKET:
        case SP_TYPE_TX_RING:
#ifdef HAVE_PF_PACKET
            close(sp->handle.fd);
#endif
            break;

        case SP_TYPE_LIBPCAP:
#ifdef HAVE_LIBPCAP
            pcap_close(sp->handle.pcap);
#endif
            break;

        case SP_TYPE_LIBDNET:
#ifdef HAVE_LIBDNET            
            eth_close(sp->handle.ldnet);
#endif
            break;

        case SP_TYPE_LIBNET:
            err(-1, "Libnet is no longer supported!");
            break;

        case SP_TYPE_NETMAP:
#ifdef HAVE_NETMAP
            fprintf(stderr, "Switching network driver for %s to normal mode... ",
                    sp->device);
            fflush(NULL);
              /* flush any remaining packets */
            ioctl(sp->handle.fd, NIOCTXSYNC, NULL);

#ifdef linux
            /* restore original settings:
             * - generic-segmentation-offload
             * - tcp-segmentation-offload
             * - rx-checksumming
             * - tx-checksumming
             */
            sp->data = sp->gso;
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_SGSO);
            sp->data = sp->tso;
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_STSO);
            sp->data = sp->rxcsum;
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_SRXCSUM);
            sp->data = sp->txcsum;
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_STXCSUM);
#endif /* linux */

            /* restore interface to normal mode */
#if NETMAP_API < 10
            ioctl(sp->handle.fd, NIOCUNREGIF, NULL);
#endif
            munmap(sp->mmap_addr, sp->mmap_size);
            close(sp->handle.fd);
            notice("done!");
#endif /* HAVE_NETMAP */
            break;

        case SP_TYPE_NONE:
            err(-1, "no injector selected!");
            break;
    }
    safe_free(sp);
    return 0;
}

/**
 * returns the Layer 2 address of the interface current
 * open.  on error, return NULL
 */
struct tcpr_ether_addr *
sendpacket_get_hwaddr(sendpacket_t *sp)
{
    struct tcpr_ether_addr *addr;
    assert(sp);

    /* if we already have our MAC address stored, just return it */
    if (memcmp(&sp->ether, "\x00\x00\x00\x00\x00\x00", ETHER_ADDR_LEN) != 0)
        return &sp->ether;

    if (sp->handle_type == SP_TYPE_KHIAL) {
        addr = sendpacket_get_hwaddr_khial(sp);
    } else {    
#if defined HAVE_PF_PACKET
        addr = sendpacket_get_hwaddr_pf(sp);
#elif defined HAVE_BPF
        addr = sendpacket_get_hwaddr_bpf(sp);
#elif defined HAVE_LIBDNET
        addr = sendpacket_get_hwaddr_libdnet(sp);
#elif (defined HAVE_PCAP_INJECT || defined HAVE_PCAP_SENDPACKET)
        addr = sendpacket_get_hwaddr_pcap(sp);
#endif
    }
    return addr;
}

/**
 * returns the error string
 */
char *
sendpacket_geterr(sendpacket_t *sp)
{
    assert(sp);
    return sp->errbuf;
}

/**
 * Set's the error string
 */
static void
sendpacket_seterr(sendpacket_t *sp, const char *fmt, ...)
{
    va_list ap;

    assert(sp);

    va_start(ap, fmt);
    if (fmt != NULL)
        (void)vsnprintf(sp->errbuf, SENDPACKET_ERRBUF_SIZE, fmt, ap);
    va_end(ap);

    sp->errbuf[(SENDPACKET_ERRBUF_SIZE-1)] = '\0'; // be safe
}


#if (defined HAVE_PCAP_INJECT || defined HAVE_PCAP_SENDPACKET) && !(defined HAVE_PF_PACKET || defined BPF || defined HAVE_LIBDNET)
/**
 * Inner sendpacket_open() method for using libpcap
 */
static sendpacket_t *
sendpacket_open_pcap(const char *device, char *errbuf)
{
    pcap_t *pcap;
    sendpacket_t *sp;
#ifdef BIOCSHDRCMPLT
    u_int spoof_eth_src = 1;
    int fd;
#endif

    assert(device);
    assert(errbuf);

    dbg(1, "sendpacket: using Libpcap");

    /* open_pcap_live automatically fills out our errbuf for us */
    if ((pcap = pcap_open_live(device, 0, 0, 0, errbuf)) == NULL)
        return NULL;

    sp = (sendpacket_t *)safe_malloc(sizeof(sendpacket_t));
    strlcpy(sp->device, device, sizeof(sp->device));
    sp->handle.pcap = pcap;

#ifdef BIOCSHDRCMPLT
    /* 
     * Only systems using BPF on the backend need this... 
     * other systems don't have ioctl and will get compile errors.
     */
    fd = pcap_get_selectable_fd(pcap);
    if (ioctl(fd, BIOCSHDRCMPLT, &spoof_eth_src) == -1)
        errx(-1, "Unable to enable source MAC spoof support: %s", strerror(errno));
#endif
    sp->handle_type = SP_TYPE_LIBPCAP;

    return sp;
}

/**
 * Get the hardware MAC address for the given interface using libpcap
 */
static struct tcpr_ether_addr *
sendpacket_get_hwaddr_pcap(sendpacket_t *sp)
{
    assert(sp);
    sendpacket_seterr(sp, "Error: sendpacket_get_hwaddr() not yet supported for pcap injection");
    return NULL;
}
#endif /* HAVE_PCAP_INJECT || HAVE_PCAP_SENDPACKET */

#if defined HAVE_LIBDNET
/**
 * Inner sendpacket_open() method for using libdnet
 */
static sendpacket_t * 
sendpacket_open_libdnet(const char *device, char *errbuf)
{
    eth_t *ldnet;
    sendpacket_t *sp;

    assert(device);
    assert(errbuf);

    dbg(1, "sendpacket: using Libdnet");

    if ((ldnet = eth_open(device)) == NULL)
        return NULL;

    sp = (sendpacket_t *)safe_malloc(sizeof(sendpacket_t));
    strlcpy(sp->device, device, sizeof(sp->device));
    sp->handle.ldnet = ldnet;
    sp->handle_type = SP_TYPE_LIBDNET;    
    return sp;    
}

/**
 * Get the hardware MAC address for the given interface using libdnet
 */
static struct tcpr_ether_addr *
sendpacket_get_hwaddr_libdnet(sendpacket_t *sp)
{
    struct tcpr_ether_addr *addr;
    int ret;
    assert(sp);

    ret = eth_get(sp->handle.ldnet, (eth_addr_t *)addr);

    if (addr == NULL || ret < 0) {
        sendpacket_seterr(sp, "Error getting hwaddr via libdnet: %s", strerror(errno));
        return NULL;
    }

    memcpy(&sp->ether, addr, sizeof(struct tcpr_ether_addr));
    return(&sp->ether);
}
#endif /* HAVE_LIBDNET */

#ifdef HAVE_NETMAP
/**
 * ioctl support for netmap
 */
static int nm_do_ioctl (sendpacket_t *sp, u_long what, int subcmd) {
    struct ifreq ifr;
    int error;
    int fd;
#ifdef linux
    struct ethtool_value eval;
#endif

    assert(sp);

    fd = socket (AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        dbg(1, "ioctl error: cannot get device control socket.\n");
        return -1;
    }

    bzero (&ifr, sizeof(ifr));
    strncpy (ifr.ifr_name, sp->device, sizeof(ifr.ifr_name));

    switch (what) {
    case SIOCSIFFLAGS:
        ifr.ifr_flags = sp->if_flags >> 16;
        ifr.ifr_flags = sp->if_flags & 0xffff;
        break;

#ifdef linux
    case SIOCETHTOOL:
        eval.cmd = subcmd;
        eval.data = sp->data;
        ifr.ifr_data = (caddr_t)&eval;
        dbgx(1, "ioctl SIOCETHTOOL subcmd=%d data=%u", subcmd, eval.data);
        break;
#endif
    }

    error = ioctl (fd, what, &ifr);
    if (error)
        goto done;

    switch (what) {
    case SIOCGIFFLAGS:
        sp->if_flags = (ifr.ifr_flags << 16) | (0xffff & ifr.ifr_flags);
        dbgx(1, "SIOCGIFFLAGS flags are 0x%x", sp->if_flags);
        break;

#ifdef linux
    case SIOCETHTOOL:
        switch (subcmd) {
        case ETHTOOL_GGSO:
            sp->gso = eval.data;
            dbgx(1, "ioctl SIOCETHTOOL ETHTOOL_GGSO=%u", eval.data);
            break;

        case ETHTOOL_GTSO:
            sp->tso = eval.data;
            dbgx(1, "ioctl SIOCETHTOOL ETHTOOL_GTSO=%u", eval.data);
            break;

        case ETHTOOL_GRXCSUM:
            sp->rxcsum = eval.data;
            dbgx(1, "ioctl SIOCETHTOOL ETHTOOL_GRXCSUM=%u", eval.data);
            break;

        case ETHTOOL_GTXCSUM:
            sp->txcsum = eval.data;
            dbgx(1, "ioctl SIOCETHTOOL ETHTOOL_GTXCSUM=%u", eval.data);
            break;
        }
        break;
#endif

    }

done:
    close (fd);

    if (error)
        warnx("ioctl error %d %lu:%d", error, what, subcmd);
    return error;
}

/**
 * Inner sendpacket_open() method for using Linux version of netmap
 */
static sendpacket_t *
sendpacket_open_netmap(const char *device, char *errbuf)
{
    sendpacket_t *sp = NULL;
    struct nmreq nmr;

    assert(device);
    assert(errbuf);

    dbg(1, "sendpacket_open_netmap: using netmap");

    /* prep & return our sp handle */
    sp = (sendpacket_t *)safe_malloc(sizeof(sendpacket_t));

    /*
     * Open the netmap device to fetch the number of queues of our
     * interface.
     *
     * The first NIOCREGIF also detaches the card from the
     * protocol stack and may cause a reset of the card,
     * which in turn may take some time for the PHY to
     * reconfigure.
     */
    if ((sp->handle.fd = open ("/dev/netmap", O_RDWR)) < 0) {
        dbg(1, "sendpacket_open_netmap: Unable to access netmap");
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "Unable to access netmap.\n"
                "See INSTALL to learn how to set up netmap-capable network drivers.");
        goto OPEN_FAILED;
    }

    /*
     * Register the interface on the netmap device: from now on,
     * we can operate on the network interface without any
     * interference from the legacy network stack.
     *
     * Cards take a long time to reset the PHY.
     */
    fprintf(stderr, "Switching network driver for %s to netmap bypass mode... ",
            device);
    fflush(NULL);

    bzero (&nmr, sizeof(nmr));
    strncpy (nmr.nr_name, device, sizeof(nmr.nr_name));
    nmr.nr_version = NETMAP_API;

    if (ioctl (sp->handle.fd, NIOCREGIF, &nmr) == -1) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "Failure accessing netmap.\n"
                "\tRequest for netmap version %u failed.\n\tMake sure netmap driver is version %u.\n\tError=%s\n",
                NETMAP_API, NETMAP_API, strerror(errno));
        goto NETMAP_UP_FAILED;
    }

    sp->mmap_size = nmr.nr_memsize;

    dbgx(1, "sendpacket_open_netmap: mapping %d Kbytes queues=%d",
            sp->mmap_size >> 10, nmr.nr_tx_rings);
    sp->mmap_addr = (struct netmap_d *)mmap (0, sp->mmap_size,
            PROT_WRITE | PROT_READ, MAP_SHARED, sp->handle.fd, 0);

    if (sp->mmap_addr == MAP_FAILED ) {
        snprintf (errbuf, SENDPACKET_ERRBUF_SIZE, "mmap: %s", strerror (errno));
        goto MMAP_FAILED;
    }

    bzero (&nmr, sizeof(nmr));
    strncpy (nmr.nr_name, device, sizeof(nmr.nr_name));
    nmr.nr_version = NETMAP_API;
    strlcpy (sp->device, device, sizeof(sp->device));
    sp->nm_if = NETMAP_IF(sp->mmap_addr, nmr.nr_offset);
    sp->nmr = nmr;
    sp->handle_type = SP_TYPE_NETMAP;

    dbg(2, "Waiting 4 seconds for phy reset...");
    sleep (4);
    dbg(2, "Ready!");

    if (nm_do_ioctl(sp, SIOCGIFFLAGS, 0) < 0)
        goto NM_DO_IOCTL_FAILED;

    if ((sp->if_flags & IFF_UP) == 0) {
        dbgx(1, "%s is down, bringing up...", device);
        sp->if_flags |= IFF_UP;
    }

    /* set promiscuous mode */
    sp->if_flags |= IFF_PROMISC;
    if (nm_do_ioctl(sp, SIOCSIFFLAGS, 0) < 0)
        goto NM_DO_IOCTL_FAILED;

#ifdef linux
    /* disable:
     * - generic-segmentation-offload
     * - tcp-segmentation-offload
     * - rx-checksumming
     * - tx-checksumming
     */
    if (nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_GGSO) < 0 ||
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_GTSO) < 0 ||
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_GRXCSUM) < 0 ||
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_GTXCSUM) < 0)
        goto NM_DO_IOCTL_FAILED;

    sp->data = 0;
    if (nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_SGSO) < 0 ||
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_STSO) < 0 ||
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_SRXCSUM) < 0 ||
            nm_do_ioctl(sp, SIOCETHTOOL, ETHTOOL_STXCSUM))
        goto NM_DO_IOCTL_FAILED;

#endif

    notice("done!");

    return sp;

NM_DO_IOCTL_FAILED:
    snprintf (errbuf, SENDPACKET_ERRBUF_SIZE, "nm_do_ioctl: %s", strerror (errno));
    munmap(sp->mmap_addr, sp->mmap_size);
MMAP_FAILED:
#if NETMAP_API < 10
    ioctl(sp->handle.fd, NIOCUNREGIF, NULL);
#endif
NETMAP_UP_FAILED:
    close (sp->handle.fd);
OPEN_FAILED:
    safe_free(sp);

    notice("failed!");
    return NULL;
}
#endif /* HAVE_NETMAP */

#if defined HAVE_PF_PACKET
/**
 * Inner sendpacket_open() method for using Linux's PF_PACKET or TX_RING
 */
static sendpacket_t *
sendpacket_open_pf(const char *device, char *errbuf)
{
    int mysocket;
    sendpacket_t *sp;
    struct ifreq ifr;
    struct sockaddr_ll sa;
    int n = 1, err;
    socklen_t errlen = sizeof(err);
    unsigned int UNUSED(mtu) = 1500;

    assert(device);
    assert(errbuf);

#if defined TX_RING
    dbg(1, "sendpacket: using TX_RING");
#else
    dbg(1, "sendpacket: using PF_PACKET");
#endif

    /* open our socket */
    if ((mysocket = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) < 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "socket: %s", strerror(errno));
        return NULL;
    }


    /* get the interface id for the device */
    if ((sa.sll_ifindex = get_iface_index(mysocket, device, errbuf)) < 0) {
        close(mysocket);
        return NULL;
    }

    /* bind socket to our interface id */
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ALL);
    if (bind(mysocket, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "bind error: %s", strerror(errno));
        close(mysocket);
        return NULL;
    }

    /* check for errors, network down, etc... */
    if (getsockopt(mysocket, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "error opening %s: %s", device,
                 strerror(errno));
        close(mysocket);
        return NULL;
    }

    if (err > 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "error opening %s: %s", device,
                 strerror(err));
        close(mysocket);
        return NULL;
    }

    /* get hardware type for our interface */
    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, device, sizeof(ifr.ifr_name));

    if (ioctl(mysocket, SIOCGIFHWADDR, &ifr) < 0) {
        close(mysocket);
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "Error getting hardware type: %s", 
                strerror(errno));
        return NULL;
    }

    /* make sure it's not loopback (PF_PACKET doesn't support it) */
    if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER)
        warnx("Unsupported physical layer type 0x%04x on %s.  Maybe it works, maybe it wont."
              "  See tickets #123/318", ifr.ifr_hwaddr.sa_family, device);

#ifdef SO_BROADCAST
    /*
     * man 7 socket
     *
     * Set or get the broadcast flag. When  enabled,  datagram  sockets
     * receive packets sent to a broadcast address and they are allowed
     * to send packets to a broadcast  address.   This  option  has no
     * effect on stream-oriented sockets.
     */
    if (setsockopt(mysocket, SOL_SOCKET, SO_BROADCAST, &n, sizeof(n)) == -1) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE,
                "SO_BROADCAST: %s", strerror(errno));
        close(mysocket);
        return NULL;
    }
#endif  /*  SO_BROADCAST  */

    /* prep & return our sp handle */
    sp = (sendpacket_t *)safe_malloc(sizeof(sendpacket_t));
    strlcpy(sp->device, device, sizeof(sp->device));
    sp->handle.fd = mysocket;

#ifdef HAVE_TX_RING
    /* Look up for MTU */
    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, sp->device, sizeof(ifr.ifr_name));

    if (ioctl(mysocket, SIOCGIFMTU, &ifr) < 0) {
        close(mysocket);
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "Error getting MTU: %s", strerror(errno));
        return NULL;
    }
    mtu = ifr.ifr_ifru.ifru_mtu;

    /* Init TX ring for sp->handle.fd socket */
    if ((sp->tx_ring = txring_init(sp->handle.fd, mtu)) == 0) { 
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "txring_init: %s", strerror(errno));
        close(mysocket);
        return NULL;
    }
    sp->handle_type = SP_TYPE_TX_RING;
#else
    sp->handle_type = SP_TYPE_PF_PACKET;
#endif
    return sp;
}

/**
 * get the interface index (necessary for sending packets w/ PF_PACKET)
 */
static int
get_iface_index(int fd, const char *device, char *errbuf) {
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, (const char *)device, sizeof(ifr.ifr_name));

    if (ioctl(fd, SIOCGIFINDEX, &ifr) == -1) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "ioctl: %s", strerror(errno));
        return (-1);
    }

    return ifr.ifr_ifindex;
}

/**
 * get's the hardware address via Linux's PF packet interface
 */
static struct tcpr_ether_addr *
sendpacket_get_hwaddr_pf(sendpacket_t *sp)
{
    struct ifreq ifr;
    int fd;

    assert(sp);

    if (!sp->open) {
        sendpacket_seterr(sp, "Unable to get hardware address on un-opened sendpacket handle");
        return NULL;
    }


    /* create dummy socket for ioctl */
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        sendpacket_seterr(sp, "Unable to open dummy socket for get_hwaddr: %s", strerror(errno));
        return NULL;
    }

    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, sp->device, sizeof(ifr.ifr_name));

    if (ioctl(fd, SIOCGIFHWADDR, (int8_t *)&ifr) < 0) {
        close(fd);
        sendpacket_seterr(sp, "Error getting hardware address: %s", strerror(errno));
        return NULL;
    }

    memcpy(&sp->ether, &ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
    close(fd);
    return(&sp->ether);
}
#endif /* HAVE_PF_PACKET */

#if defined HAVE_BPF
/**
 * Inner sendpacket_open() method for using BSD's BPF interface
 */
static sendpacket_t *
sendpacket_open_bpf(const char *device, char *errbuf)
{
    sendpacket_t *sp;
    char bpf_dev[10];
    int dev, mysocket, link_offset, link_type;
    struct ifreq ifr;
    struct bpf_version bv;
    u_int v;
#if defined(BIOCGHDRCMPLT) && defined(BIOCSHDRCMPLT)
    u_int spoof_eth_src = 1;
#endif

    assert(device);
    assert(errbuf);
    memset(&ifr, '\0', sizeof(struct ifreq));

    dbg(1, "sendpacket: using BPF");
    /* open socket */
    mysocket = -1;
    for (dev = 0; dev <= 9; dev ++) {
        memset(bpf_dev, '\0', sizeof(bpf_dev));
        snprintf(bpf_dev, sizeof(bpf_dev), "/dev/bpf%d", dev);
        if ((mysocket = open(bpf_dev, O_RDWR, 0)) > 0) {
            break;
        }
    }

    /* error?? */
    if (mysocket < 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE,
                 "Unable to open /dev/bpfX: %s", strerror(errno));
        errbuf[SENDPACKET_ERRBUF_SIZE -1] = '\0';
        return NULL;
    }

    /* get BPF version */
    if (ioctl(mysocket, BIOCVERSION, (caddr_t)&bv) < 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "Unable to get bpf version: %s", strerror(errno));
        return NULL;
    }

    if (bv.bv_major != BPF_MAJOR_VERSION || bv.bv_minor != BPF_MINOR_VERSION) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "Kernel's bpf version is out of date.");
        return NULL;
    }

    /* attach to device */
    strlcpy(ifr.ifr_name, device, sizeof(ifr.ifr_name));
    if (ioctl(mysocket, BIOCSETIF, (caddr_t)&ifr) < 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "Unable to bind %s to %s: %s",
                 bpf_dev, device, strerror(errno));
        return NULL;
    }

    /* get datalink type */
    if (ioctl(mysocket, BIOCGDLT, (caddr_t)&v) < 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "Unable to get datalink type: %s",
                 strerror(errno));
        return NULL;
    }

    /*
     *  NetBSD and FreeBSD BPF have an ioctl for enabling/disabling
     *  automatic filling of the link level source address.
     */
#if defined(BIOCGHDRCMPLT) && defined(BIOCSHDRCMPLT)
    if (ioctl(mysocket, BIOCSHDRCMPLT, &spoof_eth_src) == -1) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE,
                 "Unable to enable spoofing src MAC: %s", strerror(errno));
        return NULL;
    }
#endif

    /* assign link type and offset */
    switch (v) {
    case DLT_SLIP:
        link_offset = 0x10;
        break;
    case DLT_RAW:
        link_offset = 0x0;
        break;
    case DLT_PPP:
        link_offset = 0x04;
        break;
    case DLT_EN10MB:
    default: /* default to Ethernet */
        link_offset = 0xe;
        break;
    }
#if _BSDI_VERSION - 0 > 199510
    switch (v) {
    case DLT_SLIP:
        v = DLT_SLIP_BSDOS;
        link_offset = 0x10;
        break;
    case DLT_PPP:
        v = DLT_PPP_BSDOS;
        link_offset = 0x04;
        break;
    }
#endif

    link_type = v;

    /* allocate our sp handle, and return it */
    sp = (sendpacket_t *)safe_malloc(sizeof(sendpacket_t));
    strlcpy(sp->device, device, sizeof(sp->device));
    sp->handle.fd = mysocket;
    //sp->link_type = link_type;
    //sp->link_offset = link_offset;
    sp->handle_type = SP_TYPE_BPF;

    return sp;
}

/**
 * Get the interface hardware MAC address when using BPF
 */
static struct tcpr_ether_addr *
sendpacket_get_hwaddr_bpf(sendpacket_t *sp)
{
    int mib[6];
    size_t len;
    int8_t *buf, *next, *end;
    struct if_msghdr *ifm;
    struct sockaddr_dl *sdl;

    assert(sp);

    mib[0] = CTL_NET;
    mib[1] = AF_ROUTE;
    mib[2] = 0;
    mib[3] = AF_LINK;
    mib[4] = NET_RT_IFLIST;
    mib[5] = 0;

    if (sysctl(mib, 6, NULL, &len, NULL, 0) == -1) {
        sendpacket_seterr(sp, "%s(): sysctl(): %s", __func__, strerror(errno));
        return NULL;
    }

    buf = (int8_t *)safe_malloc(len);

    if (sysctl(mib, 6, buf, &len, NULL, 0) == -1) {
        sendpacket_seterr(sp, "%s(): sysctl(): %s", __func__, strerror(errno));
        safe_free(buf);
        return NULL;
    }

    end = buf + len;
    for (next = buf; next < end; next += ifm->ifm_msglen) {
        ifm = (struct if_msghdr *)next;
        if (ifm->ifm_type == RTM_IFINFO) {
            sdl = (struct sockaddr_dl *)(ifm + 1);
            if (strncmp(&sdl->sdl_data[0], sp->device, sdl->sdl_len) == 0) {
                memcpy(&sp->ether, LLADDR(sdl), ETHER_ADDR_LEN);
                break;
            }
        }
    }
    safe_free(buf);
    return(&sp->ether);
}

#endif /* HAVE_BPF */

/**
 * Get the DLT type of the opened sendpacket
 * Return -1 if we can't figure it out, else return the DLT_ value
 */
int
sendpacket_get_dlt(sendpacket_t *sp)
{
    int dlt = DLT_EN10MB;

    if (sp->handle_type == SP_TYPE_KHIAL ||
            sp->handle_type == SP_TYPE_NETMAP) {
        /* always EN10MB */
        ;
    } else {
#if defined HAVE_BPF
    int rcode;

    if ((rcode = ioctl(sp->handle.fd, BIOCGDLT, &dlt)) < 0) {
        warnx("Unable to get DLT value for BPF device (%s): %s", sp->device, strerror(errno));
        return(-1);
    }
#elif defined HAVE_PF_PACKET || defined HAVE_LIBDNET
    /* use libpcap to get dlt */
    pcap_t *pcap;
    char errbuf[PCAP_ERRBUF_SIZE];
    if ((pcap = pcap_open_live(sp->device, 65535, 0, 0, errbuf)) == NULL) {
        warnx("Unable to get DLT value for %s: %s", sp->device, errbuf);
        return(-1);
    }
    dlt = pcap_datalink(pcap);
    pcap_close(pcap);
#elif defined HAVE_PCAP_SENDPACKET || defined HAVE_PCAP_INJECT
    dlt = pcap_datalink(sp->handle.pcap);
#endif
    }
    return dlt;
}

/**
 * \brief Returns a string of the name of the injection method being used
 */
const char *
sendpacket_get_method(sendpacket_t *sp)
{
    if (sp == NULL) {
        return INJECT_METHOD;
    } else if (sp->handle_type == SP_TYPE_KHIAL) {
        return "khial";
    } else if (sp->handle_type == SP_TYPE_NETMAP) {
        return "netmap";
    } else {
        return INJECT_METHOD;
    }
}

/**
 * Opens a character device for injecting packets directly into 
 * your kernel via a custom driver
 */
static sendpacket_t *
sendpacket_open_khial(const char *device, char *errbuf)
{
    int mysocket;
    sendpacket_t *sp;

    assert(device);
    assert(errbuf);

    if ((mysocket = open(device, O_WRONLY|O_EXCL)) < 0) {
        snprintf(errbuf, SENDPACKET_ERRBUF_SIZE, "error opening khial device: %s", strerror(errno));
        return NULL;
    }

    sp = (sendpacket_t *)safe_malloc(sizeof(sendpacket_t));
    strlcpy(sp->device, device, sizeof(sp->device));
    sp->handle.fd = mysocket;
    sp->handle_type = SP_TYPE_KHIAL;


    return sp;
}

/**
 * Get the hardware MAC address for the given interface using khial
 */
static struct tcpr_ether_addr *
sendpacket_get_hwaddr_khial(sendpacket_t *sp)
{
    assert(sp);
    sendpacket_seterr(sp, "Error: sendpacket_get_hwaddr() not yet supported for character devices");
    return NULL;
}

/**
 * \brief Cause the currently running sendpacket() call to stop
 */
void
sendpacket_abort(sendpacket_t *sp)
{
    assert(sp);

    sp->abort = true;
}
