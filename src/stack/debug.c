/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
#include <serval/debug.h>
#if defined(OS_USER)
#include <pthread.h>
#endif

/* Debug level */
unsigned int debug = LOG_LEVEL_DBG;

static const char *log_level_str[] = {
        [ 0 ] = "UNDEF",
        [LOG_LEVEL_CRIT] = "CRIT",
	[LOG_LEVEL_ERR] = "ERR",
        [LOG_LEVEL_WARN] = "WARN",
	[LOG_LEVEL_INF] = "INF",
	[LOG_LEVEL_DBG] = "DBG"
};

#if defined(OS_LINUX_KERNEL)
extern int log_vprintk(const char *levelstr, const char *func, 
                       const char *fmt, va_list args);
#endif

void logme(log_level_t level, const char *func, const char *format, ...)
{
	va_list ap;
        
        if ((unsigned int)level > debug)
                return;
        
#if defined(OS_LINUX_KERNEL)
        switch (level) {
        case LOG_LEVEL_ERR:
        case LOG_LEVEL_WARN:
        case LOG_LEVEL_CRIT:
                pr_alert("{%d}[%3s]%s: ", 
                         task_pid_nr(current), 
                         log_level_str[level], func);
                va_start(ap, format);
                vprintk(format, ap);
                va_end(ap);
        case LOG_LEVEL_DBG:
        case LOG_LEVEL_INF:
                va_start(ap, format);
                log_vprintk(log_level_str[level], func, format, ap);
                va_end(ap);
                break;
        }
#endif
#if defined(OS_USER)
	{
		FILE *s = stdout;

		switch (level) {
		case LOG_LEVEL_DBG:
		case LOG_LEVEL_INF:
			s = stdout;
			break;
		case LOG_LEVEL_ERR:
		case LOG_LEVEL_WARN:
		case LOG_LEVEL_CRIT:
			s = stderr;
			break;
		}

                va_start(ap, format);
		fprintf(s, "%s{%010ld}[%3s]%s: ", 
			get_strtime(), (long)pthread_self(), 
                        log_level_str[level], func);
		vfprintf(s, format, ap);
                va_end(ap);
		fflush(s);
	}
#endif
}
