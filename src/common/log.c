#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int g_level = FP_LOG_INFO;

static const char *const k_level_names[] = {
    "ERROR", "WARN", "INFO", "DEBUG"
};

void fp_log_set_level(int level)
{
    if (level < FP_LOG_ERROR) {
        level = FP_LOG_ERROR;
    }
    if (level > FP_LOG_DEBUG) {
        level = FP_LOG_DEBUG;
    }
    g_level = level;
}

int fp_log_get_level(void)
{
    return g_level;
}

int fp_log_level_parse(const char *text)
{
    if (text == NULL) {
        return -1;
    }
    if (strcmp(text, "error") == 0) {
        return FP_LOG_ERROR;
    }
    if (strcmp(text, "warn") == 0 || strcmp(text, "warning") == 0) {
        return FP_LOG_WARN;
    }
    if (strcmp(text, "info") == 0) {
        return FP_LOG_INFO;
    }
    if (strcmp(text, "debug") == 0) {
        return FP_LOG_DEBUG;
    }
    return -1;
}

const char *fp_log_level_name(int level)
{
    if (level < FP_LOG_ERROR || level > FP_LOG_DEBUG) {
        return "?";
    }
    return k_level_names[level];
}

int fp_log_enabled(int level)
{
    return level <= g_level;
}

#ifdef _WIN32
static void format_timestamp(char *buf, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             (int)st.wYear, (int)st.wMonth, (int)st.wDay,
             (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
             (int)st.wMilliseconds);
}
#else
static void format_timestamp(char *buf, size_t cap)
{
    struct timespec ts;
    struct tm tm_now;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
    }
    localtime_r(&ts.tv_sec, &tm_now);
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             (long)(ts.tv_nsec / 1000000));
}
#endif

void fp_log(int level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char stamp[64];

    if (!fp_log_enabled(level)) {
        return;
    }
    format_timestamp(stamp, sizeof(stamp));
    fprintf(stderr, "%s [%-5s] %s: ",
            stamp, fp_log_level_name(level), tag != NULL ? tag : "-");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
