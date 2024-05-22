/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <unistd.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "io-util.h"
#include "netlog-manager.h"

#define RFC_5424_NILVALUE "-"
#define RFC_5424_PROTOCOL 1

#define SEND_TIMEOUT_USEC (200 * USEC_PER_MSEC)

static int sendmsg_loop(Manager *m, struct msghdr *mh) {
        ssize_t n;
        int r;

        assert(m);
        assert(m->socket >= 0);
        assert(mh);

        for (;;) {
                n = sendmsg(m->socket, mh, MSG_NOSIGNAL);
                if (n >= 0) {
                        log_debug("Successful sendmsg: %zd bytes", n);
                        return 0;
                }

                if (errno == EINTR)
                        continue;

                if (errno != EAGAIN)
                        return -errno;

                r = fd_wait_for_event(m->socket, POLLOUT, SEND_TIMEOUT_USEC);
                if (r < 0)
                        return r;
                if (r == 0)
                        return -ETIMEDOUT;
        }

        return 0;
}

static int network_send(Manager *m, struct iovec *iovec, unsigned n_iovec) {
        struct msghdr mh = {
                .msg_iov = iovec,
                .msg_iovlen = n_iovec,
        };

        assert(m);
        assert(iovec);
        assert(n_iovec > 0);

        if (m->address.sockaddr.sa.sa_family == AF_INET) {
                mh.msg_name = &m->address.sockaddr.sa;
                mh.msg_namelen = sizeof(m->address.sockaddr.in);
        } else if (m->address.sockaddr.sa.sa_family == AF_INET6) {
                mh.msg_name = &m->address.sockaddr.sa;
                mh.msg_namelen = sizeof(m->address.sockaddr.in6);
        } else
                return -EAFNOSUPPORT;

        return sendmsg_loop(m, &mh);
}

static int protocol_send(Manager *m, struct iovec *iovec, unsigned n_iovec) {
        int r;

        switch (m->protocol) {
                case SYSLOG_TRANSMISSION_PROTOCOL_DTLS:
                        r = dtls_datagram_writev(m->dtls, iovec, n_iovec);
                        if (r < 0 && r != -EAGAIN) {
                                manager_connect(m);
                                return r;
                        }
                        break;
                case SYSLOG_TRANSMISSION_PROTOCOL_TLS:
                        r = tls_stream_writev(m->tls, iovec, n_iovec);
                        if (r < 0 && r != -EAGAIN) {
                                manager_connect(m);
                                return r;
                        }
                        break;
                default:
                       r = network_send(m, iovec, n_iovec);
                        if (r < 0 && r != -EAGAIN) {
                                manager_connect(m);
                                return r;
                        }
                        break;
        }

        return 0;
}

/* rfc3339 timestamp format: yyyy-mm-ddthh:mm:ss[.frac]<+/->zz:zz */
static void format_rfc3339_timestamp(const struct timeval *tv, char *header_time, size_t header_size) {
        char gm_buf[sizeof("+0530") + 1];
        struct tm tm;
        time_t t;
        size_t written;
        int r;

        assert(header_time);

        t = tv ? tv->tv_sec : ((time_t) (now(CLOCK_REALTIME) / USEC_PER_SEC));
        localtime_r(&t, &tm);

        written = strftime(header_time, header_size, "%Y-%m-%dT%T", &tm);
        assert(written != 0);
        header_time += written;
        header_size -= written;

        /* add fractional part */
        if (tv) {
                r = snprintf(header_time, header_size, ".%06ld", tv->tv_usec);
                assert(r > 0 && (size_t)r < header_size);
                header_time += r;
                header_size -= r;
        }

        /* format the timezone according to RFC */
        xstrftime(gm_buf, "%z", &tm);
        r = snprintf(header_time, header_size, "%.3s:%.2s ", gm_buf, gm_buf + 3);
        assert(r > 0 && (size_t)r < header_size);
}

/* The Syslog Protocol RFC5424 format :
 * <pri>version sp timestamp sp hostname sp app-name sp procid sp msgid sp [sd-id]s sp msg
 */
static int format_rfc5424(Manager *m,
                          int severity,
                          int facility,
                          const char *identifier,
                          const char *message,
                          const char *hostname,
                          const char *pid,
                          const struct timeval *tv,
                          const char *syslog_structured_data,
                          const char *syslog_msgid) {

        char header_time[FORMAT_TIMESTAMP_MAX];
        char header_priority[sizeof("<   >1 ")];
        struct iovec iov[14];
        uint8_t makepri;
        int n = 0, r;

        assert(m);
        assert(message);

        makepri = (facility << 3) + severity;

        /* First: priority field Second: Version  '<pri>version' */
        r = snprintf(header_priority, sizeof(header_priority), "<%i>%i ", makepri, RFC_5424_PROTOCOL);
        assert(r > 0 && (size_t)r < sizeof(header_priority));
        IOVEC_SET_STRING(iov[n++], header_priority);

        /* Third: timestamp */
        format_rfc3339_timestamp(tv, header_time, sizeof(header_time));
        IOVEC_SET_STRING(iov[n++], header_time);

        /* Fourth: hostname */
        if (hostname)
                IOVEC_SET_STRING(iov[n++], hostname);
        else
                IOVEC_SET_STRING(iov[n++], RFC_5424_NILVALUE);

        IOVEC_SET_STRING(iov[n++], " ");

        /* Fifth: identifier */
        if (identifier)
                IOVEC_SET_STRING(iov[n++], identifier);
        else
                IOVEC_SET_STRING(iov[n++], RFC_5424_NILVALUE);

        IOVEC_SET_STRING(iov[n++], " ");

        /* Sixth: procid */
        if (pid)
                IOVEC_SET_STRING(iov[n++], pid);
        else
                IOVEC_SET_STRING(iov[n++], RFC_5424_NILVALUE);

        IOVEC_SET_STRING(iov[n++], " ");

        /* Seventh: msgid */
        if (syslog_msgid)
                IOVEC_SET_STRING(iov[n++], syslog_msgid);
        else
                IOVEC_SET_STRING(iov[n++], RFC_5424_NILVALUE);

        IOVEC_SET_STRING(iov[n++], " ");

        /* Eighth: [structured-data] */
        if (m->structured_data)
                IOVEC_SET_STRING(iov[n++], m->structured_data);
        else if (syslog_structured_data)
                IOVEC_SET_STRING(iov[n++], syslog_structured_data);
        else
                IOVEC_SET_STRING(iov[n++], RFC_5424_NILVALUE);

        IOVEC_SET_STRING(iov[n++], " ");

        /* Ninth: message */
        IOVEC_SET_STRING(iov[n++], message);

        /* Last Optional newline message separator, if not implicitly terminated by end of UDP frame
         * De facto standard: separate messages by a newline
         */
        if (m->protocol == SYSLOG_TRANSMISSION_PROTOCOL_TCP)
                IOVEC_SET_STRING(iov[n++], "\n");

        return protocol_send(m, iov, n);
}

static int format_rfc3339(Manager *m,
                          int severity,
                          int facility,
                          const char *identifier,
                          const char *message,
                          const char *hostname,
                          const char *pid,
                          const struct timeval *tv) {

        char header_priority[sizeof("<   >1 ")];
        char header_time[FORMAT_TIMESTAMP_MAX];
        struct iovec iov[14];
        uint8_t makepri;
        int n = 0, r;

        assert(m);
        assert(message);

        makepri = (facility << 3) + severity;

        /* rfc3339
         * <35>Oct 12 22:14:15 client_machine su: 'su root' failed for joe on /dev/pts/2
         */

        /* First: priority field '<pri>' */
        r = snprintf(header_priority, sizeof(header_priority), "<%i>", makepri);
        assert(r > 0 && (size_t)r < sizeof(header_priority));
        IOVEC_SET_STRING(iov[n++], header_priority);

        /* Third: timestamp */
        format_rfc3339_timestamp(tv, header_time, sizeof(header_time));
        IOVEC_SET_STRING(iov[n++], header_time);

        /* Fourth: hostname */
        if (hostname)
                IOVEC_SET_STRING(iov[n++], hostname);
        else
                IOVEC_SET_STRING(iov[n++], RFC_5424_NILVALUE);

        IOVEC_SET_STRING(iov[n++], " ");

        /* Fifth: identifier */
        if (identifier)
                IOVEC_SET_STRING(iov[n++], identifier);
        else
                IOVEC_SET_STRING(iov[n++], RFC_5424_NILVALUE);

        IOVEC_SET_STRING(iov[n++], "[");

        /* Sixth: procid */
        if (pid)
                IOVEC_SET_STRING(iov[n++], pid);
        else
                IOVEC_SET_STRING(iov[n++], RFC_5424_NILVALUE);

        IOVEC_SET_STRING(iov[n++], "]: ");

        /* Ninth: message */
        IOVEC_SET_STRING(iov[n++], message);

        /* Last Optional newline message separator, if not implicitly terminated by end of UDP frame
         * De facto standard: separate messages by a newline
         */
        if (m->protocol == SYSLOG_TRANSMISSION_PROTOCOL_TCP)
                IOVEC_SET_STRING(iov[n++], "\n");

        return protocol_send(m, iov, n);
}

int manager_push_to_network(Manager *m,
                            int severity,
                            int facility,
                            const char *identifier,
                            const char *message,
                            const char *hostname,
                            const char *pid,
                            const struct timeval *tv,
                            const char *syslog_structured_data,
                            const char *syslog_msgid) {

        int r;

        assert(m);

        switch (m->protocol) {
                case SYSLOG_TRANSMISSION_PROTOCOL_DTLS:
                        if (!m->dtls->connected) {
                                r = manager_connect(m);
                                if (r < 0)
                                        return r;
                        }

                        break;
                case SYSLOG_TRANSMISSION_PROTOCOL_TLS:
                        if (!m->tls->connected) {
                                r = manager_connect(m);
                                if (r < 0)
                                        return r;
                        }
                        break;
                default:
                        if (!m->connected) {
                                r = manager_connect(m);
                                if (r < 0)
                                        return r;
                        }
                        break;
        }

        if (m->log_format == SYSLOG_TRANSMISSION_LOG_FORMAT_RFC_5424)
               r = format_rfc5424(m, severity, facility, identifier, message, hostname, pid, tv, syslog_structured_data, syslog_msgid);
        else
               r = format_rfc3339(m, severity, facility, identifier, message, hostname, pid, tv);

        if (r < 0)
               return r;

        return 0;
}

void manager_close_network_socket(Manager *m) {
       assert(m);

        if (m->protocol == SYSLOG_TRANSMISSION_PROTOCOL_TCP && m->socket >= 0) {
                int r = shutdown(m->socket, SHUT_RDWR);
                if (r < 0)
                        log_error_errno(errno, "Failed to shutdown netlog socket: %m");
        }

        m->connected = false;
        m->socket = safe_close(m->socket);
}

int manager_network_connect_socket(Manager *m) {
        _cleanup_free_ char *pretty = NULL;
        union sockaddr_union sa;
        socklen_t salen;
        int r;

        assert(m);
        assert(m->socket >= 0);

        switch (m->address.sockaddr.sa.sa_family) {
                case AF_INET:
                        sa = (union sockaddr_union) {
                        .in.sin_family = m->address.sockaddr.sa.sa_family,
                        .in.sin_port = m->address.sockaddr.in.sin_port,
                        .in.sin_addr = m->address.sockaddr.in.sin_addr,
                };
                        salen = sizeof(sa.in);
                        break;
                case AF_INET6:
                        sa = (union sockaddr_union) {
                        .in6.sin6_family = m->address.sockaddr.sa.sa_family,
                        .in6.sin6_port = m->address.sockaddr.in6.sin6_port,
                        .in6.sin6_addr = m->address.sockaddr.in6.sin6_addr,
                };
                        salen = sizeof(sa.in6);
                        break;
                default:
                        return -EAFNOSUPPORT;
        }

        r = sockaddr_pretty(&m->address.sockaddr.sa, salen, true, true, &pretty);
        if (r < 0)
                return r;

        log_debug("Connecting to remote server: '%s'", pretty);

        r = connect(m->socket, &m->address.sockaddr.sa, salen);
        if (r < 0 && errno != EINPROGRESS)
                return log_error_errno(errno, "Failed to connect to remote server='%s': %m", pretty);

        if (errno != EINPROGRESS)
                log_debug("Connected to remote server: '%s'", pretty);
        else
                log_debug("Connection in progress to remote server: '%s'", pretty);

        return 0;
}

int manager_open_network_socket(Manager *m) {
        int r;

        assert(m);

        if (!IN_SET(m->address.sockaddr.sa.sa_family, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        switch (m->protocol) {
                case SYSLOG_TRANSMISSION_PROTOCOL_UDP:
                        m->socket = socket(m->address.sockaddr.sa.sa_family, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
                        break;
                case SYSLOG_TRANSMISSION_PROTOCOL_TCP:
                        m->socket = socket(m->address.sockaddr.sa.sa_family, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
                        break;
                default:
                        return -EPROTONOSUPPORT;
        }
        if (m->socket < 0)
                return log_error_errno(errno, "Failed to create socket: %m");;

        log_debug("Successfully created socket with fd='%d'", m->socket);

        switch (m->protocol) {
                case SYSLOG_TRANSMISSION_PROTOCOL_UDP: {
                        r = setsockopt_int(m->socket, IPPROTO_IP, IP_MULTICAST_LOOP, true);
                        if (r < 0)
                                log_debug_errno(errno, "UDP: Failed to set IP_MULTICAST_LOOP: %m");

                        if (m->send_buffer > 0) {
                                r = fd_set_sndbuf(m->socket, m->send_buffer, false);
                                if (r < 0)
                                        log_debug_errno(r, "UDP: SO_SNDBUF/SO_SNDBUFFORCE failed: %m");
                        }}

                        break;
                case SYSLOG_TRANSMISSION_PROTOCOL_TCP: {
                        if (m->no_delay) {
                                r = setsockopt_int(m->socket, IPPROTO_TCP, TCP_NODELAY, true);
                                if (r < 0)
                                        log_debug_errno(r, "Failed to enable TCP_NODELAY mode, ignoring: %m");
                        }

                        if (m->send_buffer > 0) {
                                r = fd_set_sndbuf(m->socket, m->send_buffer, false);
                                if (r < 0)
                                        log_debug_errno(r, "TCP: SO_SNDBUF/SO_SNDBUFFORCE failed: %m");
                        }

                        if (m->keep_alive) {
                                r = setsockopt_int(m->socket, SOL_SOCKET, SO_KEEPALIVE, true);
                                if (r < 0)
                                        log_debug_errno(r, "Failed to enable SO_KEEPALIVE: %m");
                        }

                        if (timestamp_is_set(m->keep_alive_time)) {
                                r = setsockopt_int(m->socket, SOL_TCP, TCP_KEEPIDLE, m->keep_alive_time / USEC_PER_SEC);
                                if (r < 0)
                                        log_debug_errno(r, "TCP_KEEPIDLE failed: %m");
                        }

                        if (m->keep_alive_interval > 0) {
                                r = setsockopt_int(m->socket, SOL_TCP, TCP_KEEPINTVL, m->keep_alive_interval / USEC_PER_SEC);
                                if (r < 0)
                                        log_debug_errno(r, "TCP_KEEPINTVL failed: %m");
                        }

                        if (m->keep_alive_cnt > 0) {
                                r = setsockopt_int(m->socket, SOL_TCP, TCP_KEEPCNT, m->keep_alive_cnt);
                                if (r < 0)
                                        log_debug_errno(r, "TCP_KEEPCNT failed: %m");
                        }}
                        break;
                default:
                        break;
        }

        r = fd_nonblock(m->socket, true);
        if (r < 0)
                log_debug_errno(errno, "Failed to set socket='%d' nonblock: %m", m->socket);

        r = manager_network_connect_socket(m);
        if (r < 0)
                goto fail;

        m->connected = true;
        return 0;

 fail:
        m->socket = safe_close(m->socket);
        return r;
}
