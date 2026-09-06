#pragma once

#define MAERO_HASH_MAX 160

int maero_password_verify(const char *stored, const char *password);
int maero_password_hash(const char *salt, const char *password, char *out, int out_cap);
int maero_password_make_salt(const char *user, char *out, int out_cap);
