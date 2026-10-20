/*
 * Small portability and file helpers.  Nothing in here performs network I/O.
 */
#ifndef FP_UTIL_H
#define FP_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Milliseconds from an unspecified monotonic origin. */
int64_t fp_now_ms(void);

/* Sleep for approximately ms milliseconds. */
void fp_sleep_ms(int ms);

/* Parse a full decimal string into [min, max].  Returns 0 or -1. */
int fp_parse_long(const char *s, long min, long max, long *out);

/* Trim ASCII whitespace in place; returns s. */
char *fp_trim(char *s);

/* ASCII whitespace test. */
int fp_isspace_c(char c);

/* Copy src into dst (NUL terminated).  Returns 0, or -1 when it does not fit. */
int fp_strcpy_bounded(char *dst, size_t cap, const char *src);

/* Duplicate a string.  Returns NULL only on allocation failure. */
char *fp_strdup(const char *s);

/* Create a directory and its parents (mode 0700).  Returns 0 or -1. */
int fp_mkdir_p(const char *dir);

/* Create the parent directories of a file path.  Returns 0 or -1. */
int fp_mkdir_parent(const char *path);

/*
 * Read a whole file (max 4 MiB).  On success *out is malloc'ed and
 * NUL-terminated, *len excludes the terminator.  Caller frees *out.
 */
int fp_read_file(const char *path, char **out, size_t *len,
                 char *err, size_t errcap);

/*
 * Write a file atomically (temp file + rename) with owner-only permissions
 * on POSIX.  Parent directories are not created.
 */
int fp_write_file_atomic(const char *path, const char *data, size_t len,
                         char *err, size_t errcap);

/* Constant-time comparison that does not leak length differences. */
int fp_constant_time_eq(const char *a, size_t alen,
                        const char *b, size_t blen);

/*
 * Split text into lines in place.  Handles LF and CRLF.  A trailing newline
 * does not create an extra empty line.  *out must be freed with
 * fp_free_lines().  Returns 0 or -1 (allocation failure).
 */
int fp_split_lines_inplace(char *text, char ***out, size_t *count);
void fp_free_lines(char **lines, size_t count);

/* Install SIGINT/SIGTERM (or console handler) that sets an internal flag. */
int fp_install_signal_handlers(void);

/* Non-zero after SIGINT/CTRL-C (or shutdown request). */
int fp_stop_requested(void);

/* Test helper: request a stop programmatically. */
void fp_request_stop(void);

#endif /* FP_UTIL_H */
