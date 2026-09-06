#pragma once

#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_INFO 6
#define LOG_AUTH 9
#define LOG_DAEMON 24
#define LOG_PID 1

void openlog(const char *ident, int option, int facility);
void syslog(int priority, const char *format, ...);
void vsyslog(int priority, const char *format, va_list ap);
void closelog(void);
