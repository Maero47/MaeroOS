#pragma once
#include <stddef.h>

void  *malloc(size_t size);
void  *realloc(void *ptr, size_t size);
void  *calloc(size_t nmemb, size_t size);
void   free(void *ptr);
void  *sbrk(int increment);
int    atoi(const char *s);
void   exit(int status);
double atof(const char *s);
int    system(const char *command);
long   atol(const char *s);
long long atoll(const char *s);
long   strtol(const char *s, char **endp, int base);
long long strtoll(const char *s, char **endp, int base);
unsigned long strtoul(const char *s, char **endp, int base);
double strtod(const char *s, char **endp);
long double strtold(const char *s, char **endp);
char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    mkstemp(char *template);
void   qsort(void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
int    abs(int v);
long   labs(long v);

extern char **environ;
