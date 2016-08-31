/*-
 * Copyright (c) 2013-2014 The University of Oslo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/param.h>
#include <sys/stat.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <iconv.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "conv-tools.h"

static iconv_t fwdconv, revconv;
static int errcnt;

static const char *enc8 =
    "iso8859-1";		/* presumed 8-bit encoding */

static const char *excl;	/* exclude */
static regex_t exclre;

static int opt_0;		/* use '\0' as separator */
static int opt_7;		/* print 7-bit names */
static int opt_8;		/* print 8-bit non-UTF names */
static int opt_d;		/* debug */
static int opt_F;		/* force rename */
static int opt_n;		/* dry run (with -r) */
static int opt_p;		/* print names */
static int opt_r;		/* rename non-UTF files */
static int opt_t;		/* undocumented test mode */
static int opt_u;		/* print UTF names */
static int opt_w;		/* print WTF names */

#define debug(lvl, ...) \
	do { if (opt_d >= lvl) fprintf(stderr, __VA_ARGS__); } while (0)

typedef enum { nc_8bit = -1, nc_ascii = 0, nc_utf8 = 1, nc_wtf8 = 2 } nameclass;

/*
 * Classify a string: returns
 *
 *  nc_utf8 if the string is valid UTF-8
 *  nc_ascii if the string contains no 8-bit characters
 *  nc_8bit if the string contains 8-bit characters but is not valid UTF-8
 */
static nameclass
classify(const unsigned char *str)
{
	unsigned char mask, pattern;
	unsigned int codepoint;
	int i, j, k, n8;

	/*
	 * A running count of 8-bit characters is kept in n8.
	 *
	 * Within a potential UTF-8 multi-byte sequence, k is the expected
	 * length of the sequence minus the initial byte and j is the
	 * number of bytes that remain.  Otherwise, j and k are both 0.
	 */
	debug(2, "classify %s\n", str);
	for (i = j = k = n8 = 0; str[i] != '\0'; ++i) {
		if ((str[i] & 0x80) == 0) {
			/* 7-bit character */
			if (k > 0)
				goto non_utf8;
			continue;
		}
		/* 8-bit character */
		++n8;
		debug(3, "%02x %02x j = %d k = %d\n",
		    str[i], str[i] & 0xc0, j, k);
		if ((str[i] & 0xc0) == 0x80) {
			/*
			 * 10xxxxxx: either non-UTF encoding or subsequent
			 * byte of UTF-8 sequence.
			 */
			if (k == 0)
				goto non_utf8;
			codepoint = (codepoint << 6) | (str[i] & 0x3f);
			--j;
			if (j == 0) {
				/* this was the last byte of the sequence */
				debug(3, "codepoint: U+%06x\n", codepoint);
				/*
				 * Ugly check for overlong encoding.
				 *
				 * First, compute the minimum codepoint
				 * bit length for the given UTF-8 sequence
				 * length.  This is basically one more
				 * than the maximum codepoint bit length
				 * for a sequence one byte shorter.
				 *
				 * Next, compare this to the actual bit
				 * length of the decoded codepoint.
				 */
				j = (k > 1 ? k * 5 + 1 : 7) + 1;
				debug(3, "minimum bit length: %d\n", j);
				for (k = 0; codepoint >= 1U << k; ++k)
					/* nothing */;
				debug(3, "codepoint bit length: %d\n", k);
				if (j > k) {
					debug(3, "overlong encoding\n");
					goto non_utf8;
				}
				/* check range */
				if (codepoint > 0x10ffff) {
					debug(3, "out-of-range codepoint\n");
					goto non_utf8;
				}
				j = k = 0;
			}
			continue;
		}
		/*
		 * All remaining possibilities are either non-UTF
		 * encodings or the first byte in a multibyte UTF-8
		 * sequence.
		 */
		if (k > 0)
			goto non_utf8;
		for (mask = 0xe0, pattern = 0xc0; ++k < 7;
		     mask = mask >> 1 | 0x80, pattern = pattern >> 1 | 0x80) {
			if ((str[i] & mask) == pattern) {
				codepoint = str[i] & ~mask;
				break;
			}
		}
		if (k == 7)
			goto non_utf8;
		j = k;
		debug(3, "entering %d-byte sequence\n", k + 1);
	}
	/* incomplete multibyte sequence */
	if (j != 0)
		goto non_utf8;
	/*
	 * We reached the end with no errors; this was either valid UTF-8
	 * or pure 7-bit ASCII.
	 */
	if (n8 > 0) {
		debug(2, "UTF-8\n");
		return (nc_utf8);
	} else {
		debug(2, "ASCII\n");
		return (nc_ascii);
	}
non_utf8:
	debug(2, "non-UTF 8-bit encoding\n");
	return (nc_8bit);
}

static char *
convertstr(iconv_t conv, char *str)
{
	char *cip, *cop;	/* conversion in / out buffer pointers */
	size_t cilen, colen;	/* conversion in / out buffer lengths */
	size_t convlen;		/* conversion length */
	char *utfstr;
	int serrno;

	/* prepare input / output */
	cip = str;
	cilen = strlen(cip);
	colen = 2 * cilen;
	if ((utfstr = malloc(colen + 1)) == NULL)
		err(1, "malloc()");
	cop = utfstr;

	/* reset conversion state */
	iconv(conv, NULL, NULL, NULL, NULL);

	/* convert */
	convlen = iconv(conv, &cip, &cilen, &cop, &colen);
	if (convlen == (size_t)-1) {
		serrno = errno;
		free(utfstr);
		errno = serrno;
		return (NULL);
	}
	*cop = '\0';
	return (utfstr);
}

/*
 * Resize a buffer so it will fit at least len characters plus a
 * terminating NUL, with a minimum allocation of 64 bytes.
 */
static char *
growbuf(char **buf, size_t *size, int len)
{
	char *tmpbuf;
	size_t tmpsize;

	tmpbuf = *buf;
	tmpsize = *size;
	if (tmpsize < 64)
		tmpsize = 64;
	while ((size_t)(len + 1) >= tmpsize)
		tmpsize *= 2;
	if ((tmpbuf = realloc(tmpbuf, tmpsize)) == NULL)
		return (NULL);
	*buf = tmpbuf;
	*size = tmpsize;
	return (*buf);
}

static void
dirconv_r(char **pathbuf, size_t *pathsize, int pathlen)
{
	DIR *dir;
	struct dirent *ent;
	struct stat sb, utfsb;
	int entlen, utflen, serrno;
	char *name, *path, *utfname, *utfpath;
	size_t utfsize;
	nameclass nc;
	int selected;

	path = *pathbuf;
	debug(1, "entering %s\n", path);
	if ((dir = opendir(path)) == NULL) {
		warn("opendir(%s)", path);
		++errcnt;
		return;
	}
	/*
	 * Why I Hate Unix, #237846: the only way to tell whether
	 * readdir(3) failed or just hit the end is to clear errno before
	 * calling it and inspect it afterwards.
	 */
	while ((errno = 0, utfname = NULL, ent = readdir(dir)) != NULL) {
		name = ent->d_name;

		/* skip . and .. */
		if (strcmp(name, ".") == 0 ||
		    strcmp(name, "..") == 0)
			continue;

		/* apply exclusion filter */
		if (excl != NULL &&
		    regexec(&exclre, name, 0, NULL, 0) == 0) {
			debug(1, "exclude %s\n", name);
			continue;
		}

		/* check path buffer size and expand if necessary */
		entlen = strlen(name);
		if (!growbuf(pathbuf, pathsize, pathlen + 1 + entlen))
			err(1, "realloc()");

		/* append entry name */
		path[pathlen] = '/';
		strcpy(path + pathlen + 1, name);

		/* ent->d_type is not reliable on older kernels */
		if (lstat(path, &sb) != 0) {
			warn("lstat(%s)", path);
			++errcnt;
			continue;
		}

		/* classify */
		nc = classify((unsigned char *)name);

		/* additional step: if UTF, check for WTF */
		if (nc == nc_utf8 &&
		    (utfname = convertstr(revconv, name)) != NULL &&
		    classify((unsigned char *)utfname) == nc_utf8)
			nc = nc_wtf8;

		/* select */
		selected = ((nc == nc_ascii && opt_7) ||
		    (nc == nc_8bit && opt_8) ||
		    (nc == nc_utf8 && opt_u) ||
		    (nc == nc_wtf8 && opt_w));

		/* print */
		if (opt_p && selected)
			printf("%s%c", path, opt_0 ? '\0' : '\n');

		/* rename if requested */
		if (opt_r && selected && (nc == nc_8bit || nc == nc_wtf8)) {
			if (utfname == NULL &&
			    (utfname = convertstr(fwdconv, name)) == NULL) {
				warn("iconv(%s) failed", name);
				++errcnt;
			} else {
				utflen = pathlen + 1 + strlen(utfname);
				utfsize = utflen + 1;
				if ((utfpath = malloc(utfsize)) == NULL)
					err(1, "malloc()");
				snprintf(utfpath, utfsize, "%.*s/%s",
				    pathlen, path, utfname);
				printf("%s -> %s\n", path, utfpath);
				if (opt_n) {
					/* dry-run */
				} else if (!opt_F && lstat(utfpath, &utfsb) == 0) {
					/* converted name already exists */
					errno = EEXIST;
					warn("%s", utfpath);
					++errcnt;
				} else if (rename(path, utfpath) != 0) {
					/* rename failed */
					warn("rename(%s, %s)", path, utfpath);
					++errcnt;
				} else if (S_ISDIR(sb.st_mode)) {
					/* update pathbuf before descending */
					if (!growbuf(pathbuf, pathsize, utfsize))
						err(1, "realloc()");
					path = *pathbuf;
					strcpy(path, utfpath);
					entlen = utflen - 1 - pathlen;
				}
				free(utfpath);
			}
		}

		/* if a directory, descend */
		if (S_ISDIR(sb.st_mode))
			dirconv_r(pathbuf, pathsize, pathlen + 1 + entlen);

		/* done */
		free(utfname);
	}
	/* cut back to original length */
	path[pathlen] = '\0';

	/* close and inspect errno */
	serrno = errno;
	closedir(dir);
	errno = serrno;
	if (errno != 0) {
		++errcnt;
		warn("readdir(%s)", path);
	}
}

static void
dirconv(const char *path)
{
	char *pathbuf;
	size_t pathsize;
	int pathlen;

	pathsize = PATH_MAX;
	if ((pathbuf = malloc(pathsize)) == NULL)
		err(1, "malloc()");
	if ((realpath(path, pathbuf)) == NULL) {
		warn("realpath(%s)", path);
		++errcnt;
		return;
	}
	pathlen = strlen(pathbuf);
	dirconv_r(&pathbuf, &pathsize, pathlen);
	free(pathbuf);
}

/*
 * A handful of UTF-8 unit tests.  All of these are properly encoded
 * UTF-8, but some are either overlong or out of range or both.  Since
 * classify() doesn't tell us anything more than "not valid UTF-8", these
 * tests aren't currently very useful.
 */
static const struct { const char *str; nameclass nc; } tests[] = {
	/* lowest allowed codepoint for each length */
	/* first is ASCII, last two are out of range */
	{ "\x01", nc_ascii },
	{ "\xc2\x80", nc_utf8 },
	{ "\xe0\xa0\x80", nc_utf8 },
	{ "\xf0\x90\x80\x80", nc_utf8 },
	{ "\xf8\x8f\x80\x80\x80", nc_8bit },
	{ "\xfc\x84\x80\x80\x80\x80", nc_8bit },

	/* highest allowed codepoint for each length */
	/* first is ASCII, last three are out of range */
	{ "\x7f", nc_ascii },
	{ "\xdf\xbf", nc_utf8 },
	{ "\xef\xbf\xbf", nc_utf8 },
	{ "\xf7\xbf\xbf\xbf", nc_8bit },
	{ "\xfb\xbf\xbf\xbf\xbf", nc_8bit },
	{ "\xfd\xbf\xbf\xbf\xbf\xbf", nc_8bit },

	/* overlong encodings for U+0 */
	{ "\xc0\x80", nc_8bit },
	{ "\xe0\x80\x80", nc_8bit },
	{ "\xf0\x80\x80\x80", nc_8bit },
	{ "\xf8\x80\x80\x80\x80", nc_8bit },
	{ "\xfc\x80\x80\x80\x80\x80", nc_8bit },

	/* highest in-range codepoint, lowest out-of-range codepoint */
	{ "\xf4\x8f\xbf\xbf", nc_utf8 },
	{ "\xf4\x90\x80\x80", nc_8bit },
};

static void
diagnostic(void)
{
	int i, n;

	n = sizeof tests / sizeof tests[0];
	printf("1..%d\n", n);
	for (i = 0; i < n; ++i) {
		if (classify((unsigned char *)tests[i].str) == tests[i].nc)
			printf("ok %d\n", i + 1);
		else
			printf("not ok %d\n", i + 1);
	}
}

static void
usage(void)
{

	fprintf(stderr, "usage: dirconv [-078dFhnpruw] [-f charset] "
	    "-x regex path ...\n");
	exit(1);
}

static void
version(void)
{

	fprintf(stderr,
	    "This is dirconv from %s.  Please report bugs to %s.\n",
	    PACKAGE_STRING, PACKAGE_BUGREPORT);
	exit(1);
}

int
main(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "078dFf:hnprtuvwx:")) != -1)
		switch (opt) {
		case '0':
			++opt_0;
			break;
		case '7':
			++opt_7;
			break;
		case '8':
			++opt_8;
			break;
		case 'd':
			++opt_d;
			break;
		case 'F':
			++opt_F;
			break;
		case 'f':
			enc8 = optarg;
			break;
		case 'h':
			usage();
			break;
		case 'n':
			++opt_n;
			break;
		case 'p':
			++opt_p;
			break;
		case 'r':
			++opt_r;
			break;
		case 't':
			++opt_t;
			break;
		case 'u':
			++opt_u;
			break;
		case 'v':
			version();
			break;
		case 'w':
			++opt_w;
			break;
		case 'x':
			excl = optarg;
			/* todo: concatenate multiple regexes */
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	/* undocumented test mode, all other options except -d are ignored */
	if (opt_t) {
		diagnostic();
		exit(0);
	}

	/* -n is intended for human consumption */
	if (opt_n)
		opt_0 = 0;

	/* default is -8p */
	if (!(opt_7 || opt_8 || opt_u || opt_w))
		opt_8 = 1;
	if (!(opt_r || opt_p))
		opt_p = 1;

	/* -F and -n only make sense with -r */
	if (opt_F && !opt_r)
		warnx("-F is meaningless without -r");
	if (opt_n && !opt_r)
		warnx("-n is meaningless without -r");

	/* initialize exclusion filter */
	if (excl != NULL)
		if (regcomp(&exclre, excl, REG_EXTENDED|REG_NOSUB) != 0)
			/* todo: print error message from regerror() */
			errx(1, "invalid exclusion filter regex");

	/* initialize iconv */
	if ((fwdconv = iconv_open("utf8", enc8)) == (iconv_t)-1 ||
	    (revconv = iconv_open(enc8, "utf8")) == (iconv_t)-1)
		err(1, "iconv initialization failed");

	/* process paths */
	while (argc--)
		dirconv(*argv++);

	iconv_close(fwdconv);
	iconv_close(revconv);
	if (excl != NULL)
		regfree(&exclre);
	exit(errcnt > 0);
}
