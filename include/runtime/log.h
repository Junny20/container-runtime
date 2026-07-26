/* log.h - minimal leveled logging helpers.
 *
 * The runtime prints diagnostics to stderr so that stdout can remain clean for
 * machine-readable output (e.g. the JSON emitted by the `state` verb). Levels
 * are compared against a global threshold set once at startup from $RT_LOG.
 */
#ifndef RUNTIME_LOG_H
#define RUNTIME_LOG_H

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

/* Initialize the log threshold. `env` is typically getenv("RT_LOG"); NULL or an
 * unrecognized value defaults to LOG_INFO. Accepts: debug, info, warn, error. */
void log_init(const char *env);

/* Emit a formatted message at `level`. Messages below the threshold are dropped.
 * A trailing newline is added automatically. */
void log_emit(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* log_errno emits at LOG_ERROR and appends ": <strerror(errno)>". Use right
 * after a failed syscall so the cause is preserved before errno is clobbered. */
void log_errno(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#define log_debug(...) log_emit(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_emit(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_emit(LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_emit(LOG_ERROR, __VA_ARGS__)

#endif /* RUNTIME_LOG_H */
