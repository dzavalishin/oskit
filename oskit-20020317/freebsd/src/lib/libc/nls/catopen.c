/*	$\Id: catopen.c,v 1.1 1995/03/30 12:47:25 jkh Exp $ */

/*
 * Written by J.T. Conklin, 10/05/94
 * Public domain.
 */

#include <sys/cdefs.h>

#ifdef __indr_reference
__indr_reference(_catopen,catopen);
#else

#include <nl_types.h>

extern nl_catd _catopen __P((__const char *, int));

nl_catd
catopen(name, oflag)
	__const char *name;
	int oflag;
{
	return _catopen(name, oflag);
}

#endif
