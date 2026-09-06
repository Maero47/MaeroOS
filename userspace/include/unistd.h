#pragma once
#include <stddef.h>
#include <sys/types.h>

struct stat;

int  read(int fd, void *buf, int n);
int  write(int fd, const void *buf, int n);
int  open(const char *path, int flags, ...);
int  close(int fd);
int  fork(void);
int  execve(const char *path, char *const argv[], char *const envp[]);
void exit(int status);
void _exit(int status);
int  waitpid(int pid, int *status, int options);
int  getpid(void);
void *brk(void *addr);
int  pipe(int fd[2]);
int  dup2(int oldfd, int newfd);
int  chdir(const char *path);
int  lseek(int fd, int offset, int whence);
int  access(const char *path, int mode);
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int  mkdir(const char *path, int mode);
int  unlink(const char *path);
int  getdents64(int fd, void *buf, int count);
int  getcwd_syscall(char *buf, int size);
int  dup(int fd);
int  truncate(const char *path, int length);
int  ftruncate(int fd, int length);
int  getppid(void);
int  fcntl(int fd, int cmd, ...);
int  sched_yield(void);
int  setpgid(int pid, int pgid);
int  getpgrp(void);
int  openat(int dirfd, const char *path, int flags);
int  fstatat(int dirfd, const char *path, struct stat *buf, int flags);
int  mkdirat(int dirfd, const char *path, int mode);
int  fchmodat(int dirfd, const char *path, int mode, int flags);
int  fchmod(int fd, int mode);
int  fchown(int fd, int owner, int group);
int  readlinkat(int dirfd, const char *path, char *buf, int bufsiz);
int  faccessat(int dirfd, const char *path, int mode, int flags);
int  unlinkat(int dirfd, const char *path, int flags);
int  rmdir(const char *path);
int  getgroups(int size, gid_t list[]);
int  isatty(int fd);
int  umask(int mask);
int  kill(int pid, int sig);
int  ioctl(int fd, unsigned long req, ...);
int  rename(const char *old, const char *new_path);

#ifndef __MAEROS_TIMESPEC_DEFINED
#define __MAEROS_TIMESPEC_DEFINED
struct timespec { long tv_sec; long tv_nsec; };
#endif
struct timeval  { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
int  nanosleep(const struct timespec *req, struct timespec *rem);
int  gettimeofday(struct timeval *tv, void *tz);
int  usleep(unsigned int usec);
int  getrandom(void *buf, unsigned int buflen, unsigned int flags);
int  getentropy(void *buf, size_t buflen);
int  symlink(const char *target, const char *path);
int  readlink(const char *path, char *buf, int bufsiz);
int  link(const char *oldpath, const char *newpath);
int  fsync(int fd);
int  mknod(const char *path, int mode, int dev);
int  mkfifo(const char *path, int mode);
int  getuid(void);
int  getgid(void);
int  geteuid(void);
int  getegid(void);
int  setuid(int uid);
int  setgid(int gid);
int  seteuid(int uid);
int  setsid(void);
int  vfork(void);
int  execv(const char *path, char *const argv[]);
int  execvp(const char *file, char *const argv[]);
char *getcwd(char *buf, int size);
int  chroot(const char *path);
