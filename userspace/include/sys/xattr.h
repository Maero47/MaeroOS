#pragma once
#include <stddef.h>
#include <sys/types.h>

ssize_t getxattr(const char *path, const char *name, void *value, size_t size);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size);
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size);
ssize_t listxattr(const char *path, char *list, size_t size);
ssize_t llistxattr(const char *path, char *list, size_t size);
ssize_t flistxattr(int fd, char *list, size_t size);
ssize_t setxattr(const char *path, const char *name, const void *value,
                 size_t size, int flags);
ssize_t lsetxattr(const char *path, const char *name, const void *value,
                  size_t size, int flags);
ssize_t fsetxattr(int fd, const char *name, const void *value,
                  size_t size, int flags);
