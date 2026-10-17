/* pnp5.c: Plug and Play installation on Windows 2000 / XP */
void P5Log(const char *fmt, ...);            /* setup.c */
BOOL P5Wow64(void);
BOOL P5CopyInf(const char *inf);
int  P5Update(const char *inf, const char *section, const char *service, BOOL *reboot);
int  P5Remove(const char *service, const char *provider);
int  P5RunNative(const WCHAR *dir, const WCHAR *args);
