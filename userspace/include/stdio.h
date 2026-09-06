#pragma once
#include <stddef.h>
#include <stdarg.h>

/* Opaque FILE type */
typedef struct _FILE FILE;

#define EOF (-1)
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* Standard streams — defined in stdio.c */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Open / close */
FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
int    fclose(FILE *f);
int    fflush(FILE *f);
int    setvbuf(FILE *stream, char *buf, int mode, size_t size);

/* Read / write */
size_t fread(void *ptr, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t sz, size_t n, FILE *f);
int    fputc(int c, FILE *f);
int    fgetc(FILE *f);
int    putc(int c, FILE *f);
int    getc(FILE *f);
int    fputs(const char *s, FILE *f);
char  *fgets(char *buf, int size, FILE *f);
int    ungetc(int c, FILE *f);

/* Seek */
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);

/* Status */
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fileno(FILE *f);

/* Formatted output */
int    vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
int    snprintf(char *buf, size_t cap, const char *fmt, ...);
int    asprintf(char **strp, const char *fmt, ...);
int    dprintf(int fd, const char *fmt, ...);

/* Formatted input */
int    sscanf(const char *s, const char *fmt, ...);
int    remove(const char *path);
int    rename(const char *oldp, const char *newp);
int    fscanf(FILE *f, const char *fmt, ...);
int    scanf(const char *fmt, ...);
int    vsscanf(const char *s, const char *fmt, va_list ap);
long   getdelim(char **lineptr, size_t *n, int delim, FILE *stream);
long   getline(char **lineptr, size_t *n, FILE *stream);

/* Char I/O shortcuts */
int    putchar(int c);
int    puts(const char *s);
int    getchar(void);

/* Error reporting */
void   perror(const char *s);

/* Seek whence constants */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
