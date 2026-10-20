#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#define FP_FILE_MAX (4u * 1024u * 1024u)

static volatile sig_atomic_t g_stop_requested = 0;

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop_requested = 1;
        return TRUE;
    }
    return FALSE;
}
#else
static void posix_signal_handler(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}
#endif

int64_t fp_now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (int64_t)time(NULL) * 1000;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}

void fp_sleep_ms(int ms)
{
    if (ms <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec req;
    struct timespec rem;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) {
        req = rem;
    }
#endif
}

int fp_parse_long(const char *s, long min, long max, long *out)
{
    char *end = NULL;
    long value;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }
    if (value < min || value > max) {
        return -1;
    }
    *out = value;
    return 0;
}

int fp_isspace_c(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

char *fp_trim(char *s)
{
    char *start;
    char *end;

    if (s == NULL) {
        return NULL;
    }
    start = s;
    while (*start != '\0' && fp_isspace_c(*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    end = s + strlen(s);
    while (end > s && fp_isspace_c(end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

int fp_strcpy_bounded(char *dst, size_t cap, const char *src)
{
    size_t len;

    if (dst == NULL || cap == 0 || src == NULL) {
        return -1;
    }
    len = strlen(src);
    if (len >= cap) {
        return -1;
    }
    memcpy(dst, src, len + 1);
    return 0;
}

char *fp_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    copy = (char *)malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

static int fp_mkdir_one(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0) {
        return 0;
    }
#else
    if (mkdir(path, 0700) == 0) {
        return 0;
    }
#endif
    return errno == EEXIST ? 0 : -1;
}

int fp_mkdir_p(const char *dir)
{
    char *copy;
    char *p;

    if (dir == NULL || *dir == '\0') {
        return -1;
    }
    copy = fp_strdup(dir);
    if (copy == NULL) {
        return -1;
    }
    for (p = copy; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            char saved;
            if (p == copy) {
                continue;
            }
            /* Do not try to create a Windows drive prefix such as "E:". */
            if (p > copy && p[-1] == ':') {
                continue;
            }
            saved = *p;
            *p = '\0';
            if (fp_mkdir_one(copy) != 0) {
                free(copy);
                return -1;
            }
            *p = saved;
        }
    }
    if (fp_mkdir_one(copy) != 0) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

int fp_mkdir_parent(const char *path)
{
    char *copy;
    char *slash;
    int rc;

    if (path == NULL) {
        return -1;
    }
    copy = fp_strdup(path);
    if (copy == NULL) {
        return -1;
    }
    slash = strrchr(copy, '/');
#ifdef _WIN32
    {
        char *backslash = strrchr(copy, '\\');
        if (backslash != NULL && (slash == NULL || backslash > slash)) {
            slash = backslash;
        }
    }
#endif
    if (slash == NULL) {
        free(copy);
        return 0;
    }
    *slash = '\0';
    if (copy[0] == '\0') {
        free(copy);
        return 0;
    }
    rc = fp_mkdir_p(copy);
    free(copy);
    return rc;
}

int fp_read_file(const char *path, char **out, size_t *len,
                 char *err, size_t errcap)
{
    FILE *fp;
    long size;
    char *buf;
    size_t got;

    if (out != NULL) {
        *out = NULL;
    }
    if (len != NULL) {
        *len = 0;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: %s", path, strerror(errno));
        }
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: cannot seek", path);
        }
        fclose(fp);
        return -1;
    }
    size = ftell(fp);
    if (size < 0 || (unsigned long)size > FP_FILE_MAX) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: file too large (max %u bytes)",
                     path, FP_FILE_MAX);
        }
        fclose(fp);
        return -1;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: out of memory", path);
        }
        fclose(fp);
        return -1;
    }
    got = fread(buf, 1, (size_t)size, fp);
    if (got != (size_t)size && ferror(fp)) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: read error: %s", path, strerror(errno));
        }
        free(buf);
        fclose(fp);
        return -1;
    }
    buf[got] = '\0';
    fclose(fp);
    if (out != NULL) {
        *out = buf;
    } else {
        free(buf);
    }
    if (len != NULL) {
        *len = got;
    }
    return 0;
}

int fp_write_file_atomic(const char *path, const char *data, size_t len,
                         char *err, size_t errcap)
{
    char *tmp;
    size_t tmp_len;
    FILE *fp;
    size_t written;
    int rc = -1;

    if (path == NULL || data == NULL) {
        return -1;
    }
    tmp_len = strlen(path) + 32;
    tmp = (char *)malloc(tmp_len);
    if (tmp == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }
#ifdef _WIN32
    snprintf(tmp, tmp_len, "%s.tmp.%ld", path, (long)_getpid());
#else
    snprintf(tmp, tmp_len, "%s.tmp.%ld", path, (long)getpid());
#endif
    fp = fopen(tmp, "wb");
    if (fp == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: %s", tmp, strerror(errno));
        }
        free(tmp);
        return -1;
    }
    written = fwrite(data, 1, len, fp);
    if (written != len) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: write error: %s", tmp, strerror(errno));
        }
        fclose(fp);
        remove(tmp);
        free(tmp);
        return -1;
    }
    if (fflush(fp) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: flush error: %s", tmp, strerror(errno));
        }
        fclose(fp);
        remove(tmp);
        free(tmp);
        return -1;
    }
    if (fclose(fp) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s: close error: %s", tmp, strerror(errno));
        }
        remove(tmp);
        free(tmp);
        return -1;
    }
#ifndef _WIN32
    (void)chmod(tmp, 0600);
#else
    /* Make the configuration owner-readable only where supported. */
    (void)_chmod(tmp, _S_IREAD | _S_IWRITE);
    /* Windows rename() refuses to replace an existing file. */
    (void)remove(path);
#endif
    if (rename(tmp, path) == 0) {
        rc = 0;
    } else if (err != NULL && errcap > 0) {
        snprintf(err, errcap, "%s: rename failed: %s", path, strerror(errno));
    }
    if (rc != 0) {
        remove(tmp);
    }
    free(tmp);
    return rc;
}

int fp_constant_time_eq(const char *a, size_t alen,
                        const char *b, size_t blen)
{
    size_t max = alen > blen ? alen : blen;
    unsigned char diff = (unsigned char)(alen != blen);
    size_t i;

    for (i = 0; i < max; i++) {
        unsigned char ca = i < alen ? (unsigned char)a[i] : 0u;
        unsigned char cb = i < blen ? (unsigned char)b[i] : 0u;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

int fp_split_lines_inplace(char *text, char ***out, size_t *count)
{
    char **lines = NULL;
    size_t n = 0;
    size_t cap = 0;
    char *p;

    if (out != NULL) {
        *out = NULL;
    }
    if (count != NULL) {
        *count = 0;
    }
    if (text == NULL || *text == '\0') {
        return 0;
    }
    p = text;
    while (*p != '\0') {
        char *nl = strchr(p, '\n');
        size_t len;
        if (nl != NULL) {
            *nl = '\0';
        }
        len = strlen(p);
        if (len > 0 && p[len - 1] == '\r') {
            p[len - 1] = '\0';
        }
        if (n == cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            char **grown = (char **)realloc(lines, new_cap * sizeof(char *));
            if (grown == NULL) {
                free(lines);
                return -1;
            }
            lines = grown;
            cap = new_cap;
        }
        lines[n++] = p;
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    if (out != NULL) {
        *out = lines;
    } else {
        free(lines);
    }
    if (count != NULL) {
        *count = n;
    }
    return 0;
}

void fp_free_lines(char **lines, size_t count)
{
    (void)count;
    free(lines);
}

int fp_install_signal_handlers(void)
{
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        return -1;
    }
    return 0;
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = posix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
#endif
}

int fp_stop_requested(void)
{
    return g_stop_requested != 0;
}

void fp_request_stop(void)
{
    g_stop_requested = 1;
}
