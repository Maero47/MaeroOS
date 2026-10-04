#pragma once
#include <utmpx.h>

/* struct utmp is struct utmpx under its traditional names. */
#define utmp      utmpx
#define ut_name   ut_user
#define ut_time   ut_tv.tv_sec
#define ut_addr   ut_addr_v6[0]
#define e_termination __e_termination
#define e_exit        __e_exit

#define setutent  setutxent
#define endutent  endutxent
#define getutent  getutxent
#define getutid   getutxid
#define getutline getutxline
#define pututline pututxline
#define utmpname  utmpxname
#define updwtmp   updwtmpx

/* Append one record to wtmp for `line` (login(3) family). */
void logwtmp(const char *line, const char *name, const char *host);
