#pragma once

typedef long regoff_t;

typedef struct {
    int re_nsub;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NEWLINE  4
#define REG_NOSUB    8

#define REG_NOTBOL   1
#define REG_NOTEOL   2

#define REG_NOMATCH  1

int regcomp(regex_t *preg, const char *regex, int cflags);
int regexec(const regex_t *preg, const char *string, unsigned long nmatch,
            regmatch_t pmatch[], int eflags);
unsigned long regerror(int errcode, const regex_t *preg, char *errbuf,
                       unsigned long errbuf_size);
void regfree(regex_t *preg);
