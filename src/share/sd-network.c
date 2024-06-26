/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>

#include "sd-network.h"

#include "alloc-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "macro.h"
#include "parse-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "util.h"

_public_ int sd_network_get_operational_state(char **state) {
        _cleanup_free_ char *s = NULL;
        int r;

        assert_return(state, -EINVAL);

        r = parse_env_file("/run/systemd/netif/state", NEWLINE, "OPER_STATE", &s, NULL);
        if (r == -ENOENT)
                return -ENODATA;
        if (r < 0)
                return r;
        if (isempty(s))
                return -ENODATA;

        *state = s;
        s = NULL;

        return 0;
}

static int network_get_strv(const char *key, char ***ret) {
        _cleanup_strv_free_ char **a = NULL;
        _cleanup_free_ char *s = NULL;
        int r;

        assert_return(ret, -EINVAL);

        r = parse_env_file("/run/systemd/netif/state", NEWLINE, key, &s, NULL);
        if (r == -ENOENT)
                return -ENODATA;
        if (r < 0)
                return r;
        if (isempty(s)) {
                *ret = NULL;
                return 0;
        }

        a = strv_split(s, " ");
        if (!a)
                return -ENOMEM;

        strv_uniq(a);
        r = strv_length(a);

        *ret = a;
        a = NULL;

        return r;
}

_public_ int sd_network_get_dns(char ***ret) {
        return network_get_strv("DNS", ret);
}

_public_ int sd_network_get_ntp(char ***ret) {
        return network_get_strv("NTP", ret);
}

_public_ int sd_network_get_search_domains(char ***ret) {
        return network_get_strv("DOMAINS", ret);
}

_public_ int sd_network_get_route_domains(char ***ret) {
        return network_get_strv("ROUTE_DOMAINS", ret);
}

static int network_link_get_string(int ifindex, const char *field, char **ret) {
        char path[strlen("/run/systemd/netif/links/") + DECIMAL_STR_MAX(ifindex) + 1];
        _cleanup_free_ char *s = NULL;
        int r;

        assert_return(ifindex > 0, -EINVAL);
        assert_return(ret, -EINVAL);

        xsprintf(path, "/run/systemd/netif/links/%i", ifindex);

        r = parse_env_file(path, NEWLINE, field, &s, NULL);
        if (r == -ENOENT)
                return -ENODATA;
        if (r < 0)
                return r;
        if (isempty(s))
                return -ENODATA;

        *ret = s;
        s = NULL;

        return 0;
}

static int network_link_get_strv(int ifindex, const char *key, char ***ret) {
        char path[strlen("/run/systemd/netif/links/") + DECIMAL_STR_MAX(ifindex) + 1];
        _cleanup_strv_free_ char **a = NULL;
        _cleanup_free_ char *s = NULL;
        int r;

        assert_return(ifindex > 0, -EINVAL);
        assert_return(ret, -EINVAL);

        xsprintf(path, "/run/systemd/netif/links/%i", ifindex);
        r = parse_env_file(path, NEWLINE, key, &s, NULL);
        if (r == -ENOENT)
                return -ENODATA;
        if (r < 0)
                return r;
        if (isempty(s)) {
                *ret = NULL;
                return 0;
        }

        a = strv_split(s, " ");
        if (!a)
                return -ENOMEM;

        strv_uniq(a);
        r = strv_length(a);

        *ret = a;
        a = NULL;

        return r;
}

_public_ int sd_network_link_get_setup_state(int ifindex, char **state) {
        return network_link_get_string(ifindex, "ADMIN_STATE", state);
}

_public_ int sd_network_link_get_network_file(int ifindex, char **filename) {
        return network_link_get_string(ifindex, "NETWORK_FILE", filename);
}

_public_ int sd_network_link_get_operational_state(int ifindex, char **state) {
        return network_link_get_string(ifindex, "OPER_STATE", state);
}

_public_ int sd_network_link_get_llmnr(int ifindex, char **llmnr) {
        return network_link_get_string(ifindex, "LLMNR", llmnr);
}

_public_ int sd_network_link_get_mdns(int ifindex, char **mdns) {
        return network_link_get_string(ifindex, "MDNS", mdns);
}

_public_ int sd_network_link_get_dnssec(int ifindex, char **dnssec) {
        return network_link_get_string(ifindex, "DNSSEC", dnssec);
}

_public_ int sd_network_link_get_dnssec_negative_trust_anchors(int ifindex, char ***nta) {
        return network_link_get_strv(ifindex, "DNSSEC_NTA", nta);
}

_public_ int sd_network_link_get_timezone(int ifindex, char **ret) {
        return network_link_get_string(ifindex, "TIMEZONE", ret);
}

_public_ int sd_network_link_get_dns(int ifindex, char ***ret) {
        return network_link_get_strv(ifindex, "DNS", ret);
}

_public_ int sd_network_link_get_ntp(int ifindex, char ***ret) {
        return network_link_get_strv(ifindex, "NTP", ret);
}

_public_ int sd_network_link_get_search_domains(int ifindex, char ***ret) {
        return network_link_get_strv(ifindex, "DOMAINS", ret);
}

_public_ int sd_network_link_get_route_domains(int ifindex, char ***ret) {
        return network_link_get_strv(ifindex, "ROUTE_DOMAINS", ret);
}

static int network_link_get_ifindexes(int ifindex, const char *key, int **ret) {
        char path[strlen("/run/systemd/netif/links/") + DECIMAL_STR_MAX(ifindex) + 1];
        _cleanup_free_ int *ifis = NULL;
        _cleanup_free_ char *s = NULL;
        size_t allocated = 0, c = 0;
        const char *x;
        int r;

        assert_return(ifindex > 0, -EINVAL);
        assert_return(ret, -EINVAL);

        xsprintf(path, "/run/systemd/netif/links/%i", ifindex);
        r = parse_env_file(path, NEWLINE, key, &s, NULL);
        if (r == -ENOENT)
                return -ENODATA;
        if (r < 0)
                return r;
        if (isempty(s)) {
                *ret = NULL;
                return 0;
        }

        x = s;
        for (;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&x, &word, NULL, 0);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = parse_ifindex(word, &ifindex);
                if (r < 0)
                        return r;

                if (!GREEDY_REALLOC(ifis, allocated, c + 1))
                        return -ENOMEM;

                ifis[c++] = ifindex;
        }

        if (!GREEDY_REALLOC(ifis, allocated, c + 1))
                return -ENOMEM;
        ifis[c] = 0; /* Let's add a 0 ifindex to the end, to be nice*/

        *ret = ifis;
        ifis = NULL;

        return c;
}

_public_ int sd_network_link_get_carrier_bound_to(int ifindex, int **ret) {
        return network_link_get_ifindexes(ifindex, "CARRIER_BOUND_TO", ret);
}

_public_ int sd_network_link_get_carrier_bound_by(int ifindex, int **ret) {
        return network_link_get_ifindexes(ifindex, "CARRIER_BOUND_BY", ret);
}

static inline int MONITOR_TO_FD(sd_network_monitor *m) {
        return (int) (unsigned long) m - 1;
}

static inline sd_network_monitor* FD_TO_MONITOR(int fd) {
        return (sd_network_monitor*) (unsigned long) (fd + 1);
}

static int monitor_add_inotify_watch(int fd) {
        int k;

        k = inotify_add_watch(fd, "/run/systemd/netif/links/", IN_MOVED_TO|IN_DELETE);
        if (k >= 0)
                return 0;
        else if (errno != ENOENT)
                return -errno;

        k = inotify_add_watch(fd, "/run/systemd/netif/", IN_CREATE|IN_ISDIR);
        if (k >= 0)
                return 0;
        else if (errno != ENOENT)
                return -errno;

        k = inotify_add_watch(fd, "/run/systemd/", IN_CREATE|IN_ISDIR);
        if (k < 0)
                return -errno;

        return 0;
}

_public_ int sd_network_monitor_new(sd_network_monitor **m, const char *category) {
        _cleanup_close_ int fd = -1;
        int k;
        bool good = false;

        assert_return(m, -EINVAL);

        fd = inotify_init1(IN_NONBLOCK|IN_CLOEXEC);
        if (fd < 0)
                return -errno;

        if (!category || streq(category, "links")) {
                k = monitor_add_inotify_watch(fd);
                if (k < 0)
                        return k;

                good = true;
        }

        if (!good)
                return -EINVAL;

        *m = FD_TO_MONITOR(fd);
        fd = -1;

        return 0;
}

_public_ sd_network_monitor* sd_network_monitor_unref(sd_network_monitor *m) {
        int fd;

        if (m) {
                fd = MONITOR_TO_FD(m);
                close_nointr(fd);
        }

        return NULL;
}

_public_ int sd_network_monitor_flush(sd_network_monitor *m) {
        union inotify_event_buffer buffer;
        struct inotify_event *e;
        ssize_t l;
        int fd, k;

        assert_return(m, -EINVAL);

        fd = MONITOR_TO_FD(m);

        l = read(fd, &buffer, sizeof(buffer));
        if (l < 0) {
                if (errno == EAGAIN || errno == EINTR)
                        return 0;

                return -errno;
        }

        FOREACH_INOTIFY_EVENT(e, buffer, l) {
                if (e->mask & IN_ISDIR) {
                        k = monitor_add_inotify_watch(fd);
                        if (k < 0)
                                return k;

                        k = inotify_rm_watch(fd, e->wd);
                        if (k < 0)
                                return -errno;
                }
        }

        return 0;
}

_public_ int sd_network_monitor_get_fd(sd_network_monitor *m) {

        assert_return(m, -EINVAL);

        return MONITOR_TO_FD(m);
}

_public_ int sd_network_monitor_get_events(sd_network_monitor *m) {

        assert_return(m, -EINVAL);

        /* For now we will only return POLLIN here, since we don't
         * need anything else ever for inotify.  However, let's have
         * this API to keep our options open should we later on need
         * it. */
        return POLLIN;
}

_public_ int sd_network_monitor_get_timeout(sd_network_monitor *m, uint64_t *timeout_usec) {

        assert_return(m, -EINVAL);
        assert_return(timeout_usec, -EINVAL);

        /* For now we will only return (uint64_t) -1, since we don't
         * need any timeout. However, let's have this API to keep our
         * options open should we later on need it. */
        *timeout_usec = (uint64_t) -1;
        return 0;
}
