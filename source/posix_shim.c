/* posix_shim.c -- glibc-isms the GDroid bionic binaries import that newlib
 * either lacks outright or only offers without the locale parameter.
 *
 * Every entry here is a thin forward to the locale-free newlib equivalent.
 * That is correct for the single-locale world Horizon lives in: the engine
 * never asks for anything but the process locale, so ignoring the handle
 * changes no observable behaviour.
 *
 * Distributed under the MIT license. See LICENSE for details.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <locale.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "posix_shim.h"

/* ------------------------------------------------------------------ */
/* _FORTIFY_SOURCE entry points                                         */
/* ------------------------------------------------------------------ */

size_t shim___fread_chk(void *ptr, size_t size, size_t nmemb, size_t buflen, FILE *stream) {
  if (buflen && size && nmemb > buflen / size)
    return 0;
  return fread(ptr, size, nmemb, stream);
}

size_t shim___fwrite_chk(const void *ptr, size_t size, size_t nmemb, size_t buflen, FILE *stream) {
  if (buflen && size && nmemb > buflen / size)
    return 0;
  return fwrite(ptr, size, nmemb, stream);
}

int shim___open_2(const char *path, int flags) {
  return open(path, flags);
}

ssize_t shim___readlink_chk(const char *path, char *buf, size_t size, size_t buflen) {
  if (buflen && size > buflen)
    size = buflen;
  return readlink(path, buf, size);
}

/* ------------------------------------------------------------------ */
/* narrow ctype, _l variants                                            */
/* ------------------------------------------------------------------ */

#define DEF_CTYPE_L(name, fn) \
  int shim_##name##_l(int c, void *loc) { (void)loc; return fn(c); }

DEF_CTYPE_L(isdigit,  isdigit)
DEF_CTYPE_L(islower,  islower)
DEF_CTYPE_L(isupper,  isupper)
DEF_CTYPE_L(isxdigit, isxdigit)
DEF_CTYPE_L(tolower,  tolower)
DEF_CTYPE_L(toupper,  toupper)

/* ------------------------------------------------------------------ */
/* wide ctype, _l variants                                              */
/* ------------------------------------------------------------------ */

#define DEF_WCTYPE_L(name, fn) \
  int shim_##name##_l(wint_t c, void *loc) { (void)loc; return fn(c); }

DEF_WCTYPE_L(iswalpha,  iswalpha)
DEF_WCTYPE_L(iswblank,  iswblank)
DEF_WCTYPE_L(iswcntrl,  iswcntrl)
DEF_WCTYPE_L(iswdigit,  iswdigit)
DEF_WCTYPE_L(iswlower,  iswlower)
DEF_WCTYPE_L(iswprint,  iswprint)
DEF_WCTYPE_L(iswpunct,  iswpunct)
DEF_WCTYPE_L(iswspace,  iswspace)
DEF_WCTYPE_L(iswupper,  iswupper)
DEF_WCTYPE_L(iswxdigit, iswxdigit)
DEF_WCTYPE_L(towlower,  towlower)
DEF_WCTYPE_L(towupper,  towupper)

/* ------------------------------------------------------------------ */
/* collation / transformation                                           */
/* ------------------------------------------------------------------ */

int shim_strcoll_l(const char *a, const char *b, void *loc) {
  (void)loc;
  return strcoll(a, b);
}

size_t shim_strxfrm_l(char *dst, const char *src, size_t n, void *loc) {
  (void)loc;
  return strxfrm(dst, src, n);
}

int shim_wcscoll_l(const wchar_t *a, const wchar_t *b, void *loc) {
  (void)loc;
  return wcscoll(a, b);
}

size_t shim_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, void *loc) {
  (void)loc;
  return wcsxfrm(dst, src, n);
}

/* ------------------------------------------------------------------ */
/* numeric parsing, _l variants                                         */
/* ------------------------------------------------------------------ */

long long shim_strtoll_l(const char *nptr, char **endptr, int base, void *loc) {
  (void)loc;
  return strtoll(nptr, endptr, base);
}

unsigned long long shim_strtoull_l(const char *nptr, char **endptr, int base, void *loc) {
  (void)loc;
  return strtoull(nptr, endptr, base);
}

/* newlib's strtold is the double path on this target; the engine only uses it
 * for config values, so the extra precision is not load-bearing. */
long double shim_strtold_l(const char *nptr, char **endptr, void *loc) {
  (void)loc;
  return (long double)strtod(nptr, endptr);
}

/* ------------------------------------------------------------------ */
/* time formatting                                                      */
/* ------------------------------------------------------------------ */

size_t shim_strftime_l(char *s, size_t max, const char *format,
                       const struct tm *tm, void *loc) {
  (void)loc;
  return strftime(s, max, format, tm);
}


/* The locale object lifecycle (newlocale / freelocale / uselocale) is left to
 * newlib: it declares them in <locale.h> with locale_t, so any definition here
 * with a different signature would be a conflicting type. Horizon has exactly
 * one locale, and newlib's no-op handles behave correctly for that. */

/* ------------------------------------------------------------------ */
/* Linux-only calls the bionic binaries import                          */
/*                                                                      */
/* Declared weak so that, where newlib happens to provide a definition, */
/* the linker prefers newlib and these are simply discarded. Each one   */
/* reports failure rather than pretending to work, which keeps the      */
/* engine on its fallback paths instead of acting on a fake success.    */
/* ------------------------------------------------------------------ */

#include <errno.h>

#define GMOD_WEAK __attribute__((weak))

GMOD_WEAK int prctl(int option, ...) {
  (void)option;
  errno = EINVAL;
  return -1;
}

/* mremap(MAP_FAILED) so callers take their allocation-failed branch */
GMOD_WEAK void *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...) {
  (void)old_address; (void)old_size; (void)new_size; (void)flags;
  errno = ENOMEM;
  return (void *)-1;
}

GMOD_WEAK int memfd_create(const char *name, unsigned int flags) {
  (void)name; (void)flags;
  errno = ENOSYS;
  return -1;
}

GMOD_WEAK int malloc_info(int options, FILE *fp) {
  (void)options; (void)fp;
  errno = ENOSYS;
  return -1;
}

GMOD_WEAK int sched_getaffinity(int pid, size_t cpusetsize, void *mask) {
  (void)pid;
  if (mask && cpusetsize >= 4) memset(mask, 0, cpusetsize);
  errno = ENOSYS;
  return -1;
}

GMOD_WEAK int sched_setaffinity(int pid, size_t cpusetsize, const void *mask) {
  (void)pid; (void)cpusetsize; (void)mask;
  errno = ENOSYS;
  return -1;
}

GMOD_WEAK void openlog(const char *ident, int option, int facility) {
  (void)ident; (void)option; (void)facility;
}

GMOD_WEAK void closelog(void) { }

GMOD_WEAK void syslog(int priority, const char *format, ...) {
  (void)priority; (void)format;
}
GMOD_WEAK int mprotect(void *addr, size_t len, int prot) {
  (void)addr; (void)len; (void)prot;
  return 0;
}

GMOD_WEAK int madvise(void *addr, size_t len, int advice) {
  (void)addr; (void)len; (void)advice;
  return 0;
}

GMOD_WEAK int getpriority(int which, id_t who) {
  (void)which; (void)who;
  return 0;
}

GMOD_WEAK int setpriority(int which, id_t who, int prio) {
  (void)which; (void)who; (void)prio;
  return 0;
}

GMOD_WEAK long sendfile(int out_fd, int in_fd, long *offset, size_t count) {
  (void)out_fd; (void)in_fd; (void)offset; (void)count;
  errno = ENOSYS;
  return -1;
}

/* ---- stdio handles ----
 * newlib turns stdin/stdout/stderr into _REENT member access, so their
 * address is not a link-time constant. Hand the import table the address of
 * these variables instead, and fill in the values before loading modules. */
FILE *gmod_stdin_p  = NULL;
FILE *gmod_stdout_p = NULL;
FILE *gmod_stderr_p = NULL;

void gmod_stdio_init(void) {
  gmod_stdin_p  = stdin;
  gmod_stdout_p = stdout;
  gmod_stderr_p = stderr;
}
/* ------------------------------------------------------------------ */
/* POSIX calls newlib's bare-metal target does not implement            */
/*                                                                      */
/* All weak: if a future newlib grows a real version, it wins. The       */
/* directory-relative forms collapse to their absolute equivalents      */
/* because every fd Horizon can hand out refers to the same namespace.   */
/* ------------------------------------------------------------------ */

#include <stdarg.h>
#include <signal.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

GMOD_WEAK int gmod_fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) {
  (void)dirfd; (void)flags;
  return chmod(pathname, mode);
}
GMOD_WEAK int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) {
  return gmod_fchmodat(dirfd, pathname, mode, flags);
}

GMOD_WEAK int fchown(int fd, uid_t owner, gid_t group) {
  (void)fd; (void)owner; (void)group;
  return 0;
}

GMOD_WEAK gid_t getgid(void)  { return 0; }
GMOD_WEAK gid_t getegid(void) { return 0; }

GMOD_WEAK DIR *fdopendir(int fd) {
  (void)fd;
  return NULL;
}

GMOD_WEAK int futimens(int fd, const struct timespec times[2]) {
  (void)fd; (void)times;
  return -1;
}

GMOD_WEAK int openat(int dirfd, const char *pathname, int flags, ...) {
  (void)dirfd;
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
  }
  return open(pathname, flags, mode);
}

GMOD_WEAK int unlinkat(int dirfd, const char *pathname, int flags) {
  (void)dirfd;
  return (flags & AT_REMOVEDIR) ? rmdir(pathname) : unlink(pathname);
}

GMOD_WEAK int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
  (void)olddirfd; (void)newdirfd;
  return rename(oldpath, newpath);
}

GMOD_WEAK int pipe2(int pipefd[2], int flags) {
  (void)flags;
  return pipe(pipefd);
}

GMOD_WEAK ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
  const off_t save = lseek(fd, 0, SEEK_CUR);
  if (save < 0) return -1;
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  const ssize_t got = read(fd, buf, count);
  (void)lseek(fd, save, SEEK_SET);
  return got;
}

GMOD_WEAK ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
  const off_t save = lseek(fd, 0, SEEK_CUR);
  if (save < 0) return -1;
  if (lseek(fd, offset, SEEK_SET) < 0) return -1;
  const ssize_t put = write(fd, buf, count);
  (void)lseek(fd, save, SEEK_SET);
  return put;
}

GMOD_WEAK int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value) {
  (void)which; (void)new_value;
  if (old_value) { old_value->it_interval.tv_sec = 0; old_value->it_interval.tv_usec = 0;
                   old_value->it_value.tv_sec = 0;    old_value->it_value.tv_usec = 0; }
  return 0;
}

GMOD_WEAK int sigemptyset(sigset_t *set) {
  if (set) *set = 0;
  return 0;
}

GMOD_WEAK int sigaddset(sigset_t *set, int signum) {
  (void)signum;
  if (set) *set = 0;
  return 0;
}

GMOD_WEAK int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  (void)how; (void)set;
  if (oldset) *oldset = 0;
  return 0;
}

GMOD_WEAK int sigaltstack(const stack_t *ss, stack_t *old_ss) {
  (void)ss;
  if (old_ss) { old_ss->ss_sp = NULL; old_ss->ss_size = 0; old_ss->ss_flags = 0; }
  return 0;
}

GMOD_WEAK long sysconf(int name) {
  switch (name) {
    case 30:  return 4096;   /* _SC_PAGESIZE */
    case 29:  return 4096;   /* _SC_PAGE_SIZE */
    case 2:   return 256;    /* _SC_OPEN_MAX */
    case 0:   return 1;      /* _SC_ARG_MAX-ish upper bound */
    default:  return -1;
  }
}