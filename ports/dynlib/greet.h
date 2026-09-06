#ifndef GREET_H
#define GREET_H
/* A symbol that lives in an EXTERNAL shared library (libgreet.so.1). */
const char *greet_message(void);
int greet_add(int a, int b);
#endif
