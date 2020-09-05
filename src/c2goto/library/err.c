#include <err.h>

#undef warn

void warn(const char *fmt, ...)
{
  (void)*fmt;
}

void warnx(const char *fmt, ...)
{
  (void)*fmt;
}
