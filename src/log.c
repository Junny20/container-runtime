#include "runtime/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static log_level_t g_threshold = LOG_INFO;

static const char *level_name(log_level_t l)
{
    switch (l) {
    case LOG_DEBUG: return "debug";
    case LOG_INFO:  return "info";
    case LOG_WARN:  return "warn";
    case LOG_ERROR: return "error";
    default:        return "?";
    }
}

void log_init(const char *env)
{
    if (!env) { g_threshold = LOG_INFO; return; }
    if      (!strcmp(env, "debug")) g_threshold = LOG_DEBUG;
    else if (!strcmp(env, "info"))  g_threshold = LOG_INFO;
    else if (!strcmp(env, "warn"))  g_threshold = LOG_WARN;
    else if (!strcmp(env, "error")) g_threshold = LOG_ERROR;
    else                            g_threshold = LOG_INFO;
}

static void vemit(log_level_t level, const char *fmt, va_list ap)
{
    if (level < g_threshold)
        return;
    fprintf(stderr, "[%s] ", level_name(level));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void log_emit(log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vemit(level, fmt, ap);
    va_end(ap);
}

void log_errno(const char *fmt, ...)
{
    int saved = errno;
    if (LOG_ERROR < g_threshold)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[error] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s\n", strerror(saved));
    errno = saved;
}
