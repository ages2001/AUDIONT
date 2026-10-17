#include <stdlib.h>
#include <string.h>
#include "audiont.h"
PVOID OsAlloc(ULONG n) { return calloc(1, n); }
void OsFree(PVOID p) { free(p); }
