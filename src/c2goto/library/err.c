#include <err.h>

#undef warn
#undef warnx

void warn(const char *fmt, ...)
{
  (void)*fmt;
}

void warnx(const char *fmt, ...)
{
  (void)*fmt;
}
