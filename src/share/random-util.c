/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h>
#include <linux/random.h>
#include <stdint.h>

#ifdef HAVE_SYS_AUXV_H
#include <sys/auxv.h>
#endif

#if USE_SYS_RANDOM_H
#  include <sys/random.h>
#else
#  include <linux/random.h>
#endif

#include "fd-util.h"
#include "io-util.h"
#include "process-util.h"
#include "missing.h"
#include "random-util.h"
#include "time-util.h"

int dev_urandom(void *p, size_t n) {
        static int have_syscall = -1;

        _cleanup_close_ int fd = -1;
        int r;

        /* Gathers some randomness from the kernel. This call will
         * never block, and will always return some data from the
         * kernel, regardless if the random pool is fully initialized
         * or not. It thus makes no guarantee for the quality of the
         * returned entropy, but is good enough for our usual usecases
         * of seeding the hash functions for hashtable */

        /* Use the getrandom() syscall unless we know we don't have
         * it, or when the requested size is too large for it. */
        if (have_syscall != 0 || (size_t) (int) n != n) {
                r = getrandom(p, n, GRND_NONBLOCK);
                if (r == (int) n) {
                        have_syscall = true;
                        return 0;
                }

                if (r < 0) {
                        if (errno == ENOSYS)
                                /* we lack the syscall, continue with
                                 * reading from /dev/urandom */
                                have_syscall = false;
                        else if (errno == EAGAIN)
                                /* not enough entropy for now. Let's
                                 * remember to use the syscall the
                                 * next time, again, but also read
                                 * from /dev/urandom for now, which
                                 * doesn't care about the current
                                 * amount of entropy.  */
                                have_syscall = true;
                        else
                                return -errno;
                } else
                        /* too short read? */
                        return -ENODATA;
        }

        fd = open("/dev/urandom", O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return errno == ENOENT ? -ENOSYS : -errno;

        return loop_read_exact(fd, p, n, true);
}

void initialize_srand(void) {
        static bool srand_called = false;
        unsigned x;
#ifdef HAVE_SYS_AUXV_H
        void *auxv;
#endif

        if (srand_called)
                return;

#ifdef HAVE_SYS_AUXV_H
        /* The kernel provides us with 16 bytes of entropy in auxv, so let's try to make use of that to seed the
         * pseudo-random generator. It's better than nothing... */

        auxv = (void*) getauxval(AT_RANDOM);
        if (auxv) {
                assert_cc(sizeof(x) < 16);
                memcpy(&x, auxv, sizeof(x));
        } else
#endif
                x = 0;


        x ^= (unsigned) now(CLOCK_REALTIME);
        x ^= (unsigned) gettid();

        srand(x);
        srand_called = true;
}

void random_bytes(void *p, size_t n) {
        uint8_t *q;
        int r;

        r = dev_urandom(p, n);
        if (r >= 0)
                return;

        /* If some idiot made /dev/urandom unavailable to us, he'll
         * get a PRNG instead. */

        initialize_srand();

        for (q = p; q < (uint8_t*) p + n; q ++)
                *q = rand();
}
