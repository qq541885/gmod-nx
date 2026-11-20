/* posix_shim.h -- declarations for the bionic-compat shims and the Linux-only
 * calls that newlib does not provide.
 *
 * imports.c takes the address of every one of these, so they all need a visible
 * declaration. The Linux-only ones are also declared weak in the definition
 * site: where newlib happens to supply an implementation, the linker prefers
 * newlib and discards ours.
 *
 * Distributed under the MIT license. See LICENSE for details.
 */

#ifndef __POSIX_SHIM_H__
#define __POSIX_SHIM_H__

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <sys/types.h>

/* ---- _FORTIFY_SOURCE entry points ---- */
size_t  shim___fread_chk(void *ptr, size_t size, size_t nmemb, size_t buflen, FILE *stream);
size_t  shim___fwrite_chk(const void *ptr, size_t size, size_t nmemb, size_t buflen, FILE *stream);
int     shim___open_2(const char *path, int flags);
ssize_t shim___readlink_chk(const char *path, char *buf, size_t size, size_t buflen);

/* ---- narrow ctype, _l variants ---- */
int shim_isdigit_l(int c, void *loc);
int shim_islower_l(int c, void *loc);
int shim_isupper_l(int c, void *loc);
int shim_isxdigit_l(int c, void *loc);
int shim_tolower_l(int c, void *loc);
int shim_toupper_l(int c, void *loc);

/* ---- wide ctype, _l variants ---- */
int shim_iswalpha_l(int c, void *loc);
int shim_iswblank_l(int c, void *loc);
int shim_iswcntrl_l(int c, void *loc);
int shim_iswdigit_l(int c, void *loc);
int shim_iswlower_l(int c, void *loc);
int shim_iswprint_l(int c, void *loc);
int shim_iswpunct_l(int c, void *loc);
int shim_iswspace_l(int c, void *loc);
int shim_iswupper_l(int c, void *loc);
int shim_iswxdigit_l(int c, void *loc);
int shim_towlower_l(int c, void *loc);
int shim_towupper_l(int c, void *loc);

/* ---- collation / transformation ---- */
int    shim_strcoll_l(const char *a, const char *b, void *loc);
size_t shim_strxfrm_l(char *dst, const char *src, size_t n, void *loc);
int    shim_wcscoll_l(const wchar_t *a, const wchar_t *b, void *loc);
size_t shim_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, void *loc);

/* ---- numeric parsing / time ---- */
long long          shim_strtoll_l(const char *nptr, char **endptr, int base, void *loc);
unsigned long long shim_strtoull_l(const char *nptr, char **endptr, int base, void *loc);
long double        shim_strtold_l(const char *nptr, char **endptr, void *loc);
size_t             shim_strftime_l(char *s, size_t max, const char *format,
                                   const struct tm *tm, void *loc);

/* ---- Linux-only calls, weak so newlib wins where it can ---- */
int      prctl(int option, ...);
void    *mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...);
int      memfd_create(const char *name, unsigned int flags);
int      malloc_info(int options, FILE *fp);
int      sched_getaffinity(int pid, size_t cpusetsize, void *mask);
int      sched_setaffinity(int pid, size_t cpusetsize, const void *mask);
void     openlog(const char *ident, int option, int facility);
void     closelog(void);
void     syslog(int priority, const char *format, ...);


/* ---- memory / socket / scheduling: no bare-metal header declares these ---- */
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t len, int advice);
int shutdown(int sockfd, int how);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int prio);
long sendfile(int out_fd, int in_fd, long *offset, size_t count);

/* MAP_* / PROT_* for callers that only pass them through to mprotect */
#ifndef PROT_NONE
#define PROT_NONE  0
#endif
#ifndef PROT_READ
#define PROT_READ  1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 2
#endif
#ifndef PROT_EXEC
#define PROT_EXEC  4
#endif

#endif