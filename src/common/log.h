/*
 * Tiny leveled logger.  Output goes to stderr with a local timestamp,
 * level tag and caller supplied connection tag.  Hot paths must guard
 * expensive argument construction with fp_log_enabled().
 */
#ifndef FP_LOG_H
#define FP_LOG_H

#include <stddef.h>

enum fp_log_level {
    FP_LOG_ERROR = 0,
    FP_LOG_WARN = 1,
    FP_LOG_INFO = 2,
    FP_LOG_DEBUG = 3
};

void fp_log_set_level(int level);
int fp_log_get_level(void);
int fp_log_level_parse(const char *text);
const char *fp_log_level_name(int level);

int fp_log_enabled(int level);

#if defined(__GNUC__) || defined(__clang__)
void fp_log(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#else
void fp_log(int level, const char *tag, const char *fmt, ...);
#endif

#define FP_LOGE(tag, ...) \
    do { if (fp_log_enabled(FP_LOG_ERROR)) fp_log(FP_LOG_ERROR, (tag), __VA_ARGS__); } while (0)
#define FP_LOGW(tag, ...) \
    do { if (fp_log_enabled(FP_LOG_WARN)) fp_log(FP_LOG_WARN, (tag), __VA_ARGS__); } while (0)
#define FP_LOGI(tag, ...) \
    do { if (fp_log_enabled(FP_LOG_INFO)) fp_log(FP_LOG_INFO, (tag), __VA_ARGS__); } while (0)
#define FP_LOGD(tag, ...) \
    do { if (fp_log_enabled(FP_LOG_DEBUG)) fp_log(FP_LOG_DEBUG, (tag), __VA_ARGS__); } while (0)

#endif /* FP_LOG_H */
