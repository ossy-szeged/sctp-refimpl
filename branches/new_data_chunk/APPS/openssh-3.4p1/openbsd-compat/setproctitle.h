/* $Id: setproctitle.h,v 1.1.1.1 2004-06-23 13:07:28 randall Exp $ */

#ifndef _BSD_SETPROCTITLE_H
#define _BSD_SETPROCTITLE_H

#include "config.h"

#ifndef HAVE_SETPROCTITLE
void setproctitle(const char *fmt, ...);
#endif

#endif /* _BSD_SETPROCTITLE_H */
