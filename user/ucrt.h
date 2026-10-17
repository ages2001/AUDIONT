/* AUDIONT - tiny C runtime for NT 3.1-compatible Win32 binaries (no CRTDLL/MSVCRT) */
#ifndef AUDIONT_UCRT_H
#define AUDIONT_UCRT_H
#define WINVER 0x0400
#define _WIN32_WINNT 0x0400
#include <windows.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
int   u_wcslen(const WCHAR *s);
void  u_wcscpy(WCHAR *d, const WCHAR *s);
void  u_wcscat(WCHAR *d, const WCHAR *s);
int   u_wcsicmp(const WCHAR *a, const WCHAR *b);
const WCHAR *u_wcsistr(const WCHAR *h, const WCHAR *n);
int   u_strlen(const char *s);
int   u_watoi(const WCHAR *s);
WCHAR **u_argv(int *argc);
void  u_printf(const char *fmt, ...);        /* to stdout (console) */
void  u_out(const char *s, int n);
extern HANDLE u_stdout;
#endif
