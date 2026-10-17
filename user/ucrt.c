#include "ucrt.h"
HANDLE u_stdout;
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *x, const void *y, size_t n) { const unsigned char *a = x, *b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
int u_wcslen(const WCHAR *s) { int n = 0; while (s[n]) n++; return n; }
void u_wcscpy(WCHAR *d, const WCHAR *s) { while ((*d++ = *s++)) ; }
void u_wcscat(WCHAR *d, const WCHAR *s) { u_wcscpy(d + u_wcslen(d), s); }
static WCHAR lw(WCHAR c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int u_wcsicmp(const WCHAR *a, const WCHAR *b) { while (*a && lw(*a) == lw(*b)) { a++; b++; } return lw(*a) - lw(*b); }
const WCHAR *u_wcsistr(const WCHAR *h, const WCHAR *n)
{
    int ln = u_wcslen(n), i;
    for (; *h; h++) { for (i = 0; i < ln && h[i] && lw(h[i]) == lw(n[i]); i++) ; if (i == ln) return h; }
    return 0;
}
int u_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
int u_watoi(const WCHAR *s) { int v = 0, neg = 0; if (*s == '-') { neg = 1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return neg ? -v : v; }

WCHAR **u_argv(int *argc)
{
    static WCHAR buf[1024]; static WCHAR *av[32];
    WCHAR *p = GetCommandLineW(), *o = buf; int n = 0;
    while (*p && n < 31) {
        int q = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        av[n++] = o;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') { q = !q; p++; continue; }
            if (o < buf + 1023) *o++ = *p;
            p++;
        }
        *o++ = 0;
    }
    av[n] = 0; *argc = n; return av;
}

void u_out(const char *s, int n)
{
    DWORD w;
    if (!u_stdout) u_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (u_stdout && u_stdout != INVALID_HANDLE_VALUE) WriteFile(u_stdout, s, n, &w, 0);
}

void u_printf(const char *fmt, ...)
{
    char buf[1024]; va_list ap; int n;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n > 0) u_out(buf, n);
}
void __main(void) {}
