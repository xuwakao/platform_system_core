/*
 * Copyright (C) 2007-2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_PTHREADS
#include <pthread.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if (FAKE_LOG_DEVICE == 0)
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <time.h>
#include <unistd.h>

#include <log/logd.h>
#include <log/logger.h>
#include <log/log_read.h>
#include <private/android_filesystem_config.h>

#define LOG_BUF_SIZE 1024

#if FAKE_LOG_DEVICE
/* This will be defined when building for the host. */
#include "fake_log_device.h"
#endif

static int __write_to_log_init(log_id_t, struct iovec *vec, size_t nr);
static int (*write_to_log)(log_id_t, struct iovec *vec, size_t nr) = __write_to_log_init;
#ifdef HAVE_PTHREADS
static pthread_mutex_t log_init_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

#define UNUSED  __attribute__((__unused__))

static int logd_fd = -1;
#if FAKE_LOG_DEVICE
#define WEAK __attribute__((weak))
static int log_fds[(int)LOG_ID_MAX] = { -1, -1, -1, -1 };
#endif

/*
 * This is used by the C++ code to decide if it should write logs through
 * the C code.  Basically, if /dev/socket/logd is available, we're running in
 * the simulator rather than a desktop tool and want to use the device.
 */
static enum {
    kLogUninitialized, kLogNotAvailable, kLogAvailable
} g_log_status = kLogUninitialized;
int __android_log_dev_available(void)
{
    if (g_log_status == kLogUninitialized) {
        if (access("/dev/socket/logdw", W_OK) == 0)
            g_log_status = kLogAvailable;
        else
            g_log_status = kLogNotAvailable;
    }

    return (g_log_status == kLogAvailable);
}

static int __write_to_log_null(log_id_t log_fd UNUSED, struct iovec *vec UNUSED,
                               size_t nr UNUSED)
{
    return -1;
}

static int __write_to_log_kernel(log_id_t log_id, struct iovec *vec, size_t nr)
{
#if FAKE_LOG_DEVICE
    ssize_t ret;
    int log_fd;

    if (/*(int)log_id >= 0 &&*/ (int)log_id < (int)LOG_ID_MAX) {
        log_fd = log_fds[(int)log_id];
    } else {
        return EBADF;
    }
    do {
        ret = fakeLogWritev(log_fd, vec, nr);
    } while (ret < 0 && errno == EINTR);

    return ret;
#else
    if (logd_fd == -1) {
        return -1;
    }
    if (getuid() == AID_LOGD) {
        /*
         * ignore log messages we send to ourself.
         * Such log messages are often generated by libraries we depend on
         * which use standard Android logging.
         */
        return 0;
    }
    /*
     *  struct {
     *      // what we provide
     *      typeof_log_id_t  log_id;
     *      u16              tid;
     *      log_time         realtime;
     *      // caller provides
     *      union {
     *          struct {
     *              char     prio;
     *              char     payload[];
     *          } string;
     *          struct {
     *              uint32_t tag
     *              char     payload[];
     *          } binary;
     *      };
     *  };
     */
    static const unsigned header_length = 3;
    struct iovec newVec[nr + header_length];
    typeof_log_id_t log_id_buf = log_id;
    uint16_t tid = gettid();

    newVec[0].iov_base   = (unsigned char *) &log_id_buf;
    newVec[0].iov_len    = sizeof_log_id_t;
    newVec[1].iov_base   = (unsigned char *) &tid;
    newVec[1].iov_len    = sizeof(tid);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    log_time realtime_ts;
    realtime_ts.tv_sec = ts.tv_sec;
    realtime_ts.tv_nsec = ts.tv_nsec;

    newVec[2].iov_base   = (unsigned char *) &realtime_ts;
    newVec[2].iov_len    = sizeof(log_time);

    size_t i;
    for (i = header_length; i < nr + header_length; i++) {
        newVec[i].iov_base = vec[i-header_length].iov_base;
        newVec[i].iov_len  = vec[i-header_length].iov_len;
    }

    /* The write below could be lost, but will never block. */
    return writev(logd_fd, newVec, nr + header_length);
#endif
}

#if FAKE_LOG_DEVICE
static const char *LOG_NAME[LOG_ID_MAX] = {
    [LOG_ID_MAIN] = "main",
    [LOG_ID_RADIO] = "radio",
    [LOG_ID_EVENTS] = "events",
    [LOG_ID_SYSTEM] = "system"
};

const WEAK char *android_log_id_to_name(log_id_t log_id)
{
    if (log_id >= LOG_ID_MAX) {
        log_id = LOG_ID_MAIN;
    }
    return LOG_NAME[log_id];
}
#endif

static int __write_to_log_init(log_id_t log_id, struct iovec *vec, size_t nr)
{
#ifdef HAVE_PTHREADS
    pthread_mutex_lock(&log_init_lock);
#endif

    if (write_to_log == __write_to_log_init) {
        write_to_log = __write_to_log_kernel;

#if FAKE_LOG_DEVICE
        int i;
        for (i = 0; i < LOG_ID_MAX; i++) {
            char buf[sizeof("/dev/log_system")];
            snprintf(buf, sizeof(buf), "/dev/log_%s", android_log_id_to_name(i));
            log_fds[i] = fakeLogOpen(buf, O_WRONLY);
        }
#else
        int sock = socket(PF_UNIX, SOCK_DGRAM, 0);
        if (sock != -1) {
            if (fcntl(sock, F_SETFL, O_NONBLOCK) == -1) {
                /* NB: Loss of content */
                close(sock);
                sock = -1;
            } else {
                struct sockaddr_un un;
                memset(&un, 0, sizeof(struct sockaddr_un));
                un.sun_family = AF_UNIX;
                strcpy(un.sun_path, "/dev/socket/logdw");

                connect(sock, (struct sockaddr *)&un, sizeof(struct sockaddr_un));
            }
        } else {
            write_to_log = __write_to_log_null;
        }
        logd_fd = sock;
#endif
    }

#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&log_init_lock);
#endif

    return write_to_log(log_id, vec, nr);
}

int __android_log_write(int prio, const char *tag, const char *msg)
{
    struct iovec vec[3];
    log_id_t log_id = LOG_ID_MAIN;
    char tmp_tag[32];

    if (!tag)
        tag = "";

    /* XXX: This needs to go! */
    if (!strcmp(tag, "HTC_RIL") ||
        !strncmp(tag, "RIL", 3) || /* Any log tag with "RIL" as the prefix */
        !strncmp(tag, "IMS", 3) || /* Any log tag with "IMS" as the prefix */
        !strcmp(tag, "AT") ||
        !strcmp(tag, "GSM") ||
        !strcmp(tag, "STK") ||
        !strcmp(tag, "CDMA") ||
        !strcmp(tag, "PHONE") ||
        !strcmp(tag, "SMS")) {
            log_id = LOG_ID_RADIO;
            // Inform third party apps/ril/radio.. to use Rlog or RLOG
            snprintf(tmp_tag, sizeof(tmp_tag), "use-Rlog/RLOG-%s", tag);
            tag = tmp_tag;
    }

    vec[0].iov_base   = (unsigned char *) &prio;
    vec[0].iov_len    = 1;
    vec[1].iov_base   = (void *) tag;
    vec[1].iov_len    = strlen(tag) + 1;
    vec[2].iov_base   = (void *) msg;
    vec[2].iov_len    = strlen(msg) + 1;

    return write_to_log(log_id, vec, 3);
}

int __android_log_buf_write(int bufID, int prio, const char *tag, const char *msg)
{
    struct iovec vec[3];
    char tmp_tag[32];

    if (!tag)
        tag = "";

    /* XXX: This needs to go! */
    if ((bufID != LOG_ID_RADIO) &&
         (!strcmp(tag, "HTC_RIL") ||
        !strncmp(tag, "RIL", 3) || /* Any log tag with "RIL" as the prefix */
        !strncmp(tag, "IMS", 3) || /* Any log tag with "IMS" as the prefix */
        !strcmp(tag, "AT") ||
        !strcmp(tag, "GSM") ||
        !strcmp(tag, "STK") ||
        !strcmp(tag, "CDMA") ||
        !strcmp(tag, "PHONE") ||
        !strcmp(tag, "SMS"))) {
            bufID = LOG_ID_RADIO;
            // Inform third party apps/ril/radio.. to use Rlog or RLOG
            snprintf(tmp_tag, sizeof(tmp_tag), "use-Rlog/RLOG-%s", tag);
            tag = tmp_tag;
    }

    vec[0].iov_base   = (unsigned char *) &prio;
    vec[0].iov_len    = 1;
    vec[1].iov_base   = (void *) tag;
    vec[1].iov_len    = strlen(tag) + 1;
    vec[2].iov_base   = (void *) msg;
    vec[2].iov_len    = strlen(msg) + 1;

    return write_to_log(bufID, vec, 3);
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[LOG_BUF_SIZE];

    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);

    return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE];

    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);

    return __android_log_write(prio, tag, buf);
}

int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char buf[LOG_BUF_SIZE];

    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);

    return __android_log_buf_write(bufID, prio, tag, buf);
}

void __android_log_assert(const char *cond, const char *tag,
                          const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
        va_end(ap);
    } else {
        /* Msg not provided, log condition.  N.B. Do not use cond directly as
         * format string as it could contain spurious '%' syntax (e.g.
         * "%d" in "blocks%devs == 0").
         */
        if (cond)
            snprintf(buf, LOG_BUF_SIZE, "Assertion failed: %s", cond);
        else
            strcpy(buf, "Unspecified assertion failed");
    }

    __android_log_write(ANDROID_LOG_FATAL, tag, buf);

    __builtin_trap(); /* trap so we have a chance to debug the situation */
}

int __android_log_bwrite(int32_t tag, const void *payload, size_t len)
{
    struct iovec vec[2];

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = (void*)payload;
    vec[1].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 2);
}

/*
 * Like __android_log_bwrite, but takes the type as well.  Doesn't work
 * for the general case where we're generating lists of stuff, but very
 * handy if we just want to dump an integer into the log.
 */
int __android_log_btwrite(int32_t tag, char type, const void *payload,
                          size_t len)
{
    struct iovec vec[3];

    vec[0].iov_base = &tag;
    vec[0].iov_len = sizeof(tag);
    vec[1].iov_base = &type;
    vec[1].iov_len = sizeof(type);
    vec[2].iov_base = (void*)payload;
    vec[2].iov_len = len;

    return write_to_log(LOG_ID_EVENTS, vec, 3);
}
