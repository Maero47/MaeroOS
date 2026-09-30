#pragma once
#include "../include/stdio.h"

#define MAERO_HASH_MAX 160

int maero_password_verify(const char *stored, const char *password);
int maero_password_hash(const char *salt, const char *password, char *out, int out_cap);
int maero_password_make_salt(const char *user, char *out, int out_cap);

/* /etc/passwd has 7 fields, /etc/shadow 9; lines longer than this are refused. */
#define MAERO_LINE_MAX 512
#define MAERO_PASSWD_FIELDS 7
#define MAERO_SHADOW_FIELDS 9

/* Split a passwd/shadow line on every ':' in place, keeping empty fields
 * (strtok_r would merge "a::b" into two fields).  Trailing CR/LF is
 * stripped.  Returns the field count, or -1 if there are more than max. */
int maero_split_fields(char *line, char **fields, int max);
/* Strict decimal uid/gid parser: returns 1 and sets *out, or 0. */
int maero_parse_id(const char *s, int *out);
/* fgets() that skips (rather than splits) lines longer than cap-1 bytes.
 * Returns 1 with a line in buf, 0 at EOF. */
int maero_read_line(FILE *fp, char *buf, int cap);
/* Copy USER's stored hash field from /etc/shadow into out.  Returns 1 if
 * the user has an entry (the field may still be a lock marker). */
int maero_shadow_hash(const char *user, char *out, int out_cap);
/* Prompt on stdout and read a line from stdin with echo off.  Returns 0 on EOF. */
int maero_read_password(const char *prompt, char *buf, int cap);
/* Clear a buffer that held a password or hash (not optimised away). */
void maero_wipe(void *p, int n);
