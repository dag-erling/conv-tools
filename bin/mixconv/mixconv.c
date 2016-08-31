/*-
 * Copyright (c) 2012-2014 The University of Oslo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
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

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "conv-tools.h"

static const char *enc8 =
    "iso8859-1";		/* presumed 8-bit encoding */

static const char *outname;
static int opt_d;		/* debug */
static int opt_t;		/* undocumented test mode */

/*
 * Convert a file that contains a mix of ISO8859-1 and UTF-8 to clean
 * UTF-8, assuming that a) each line uses one encoding or the other and b)
 * there are no instances of multiple consecutive non-ASCII characters.
 *
 * Both encodings have 7-bit ASCII as a common subset.  ISO8859-1 has 96
 * additional characters, all of which have bit 7 set; UTF-8, on the other
 * hand, encodes non-ASCII characters as sequences of two to six bytes,
 * each of which has bit 7 set.
 *
 * We read the input one line at a time and inspect each line byte by
 * byte.  If a byte has bit 7 set and neither of its neighbors do, we
 * assume that the line is encoded in ISO8859-1, and recode it.
 * Otherwise, we assume that the line is either plain ASCII or UTF-8 and
 * output it as-is.
 *
 * Caveat: an ISO8859-1-encoded line that contains multiple consecutive
 * non-ASCII characters but no isolated non-ASCII characters will be
 * incorrectly classified as UTF-8.
 */
static void
mixconv(iconv_t conv,
    FILE *infile, const char *inname, FILE *outfile, const char *outname)
{
	char *linebuf;		/* line buffer */
	size_t linesize;	/* size of line buffer */
	ssize_t linelen;	/* length of current line */
	unsigned int prev3;	/* bitmap of previous 3 bytes */
	char convbuf[80];	/* conversion output buffer */
	char *cip, *cop;	/* conversion in / out buffer pointers */
	size_t cilen, colen;	/* conversion in / out buffer lengths */
	size_t convlen;		/* conversion length */
	int i;

	linebuf = NULL;
	linesize = 0;
	while ((linelen = getline(&linebuf, &linesize, infile)) > 0) {
		/*
		 * The lower three bits of prev3 are copies of bit 7 of
		 * the current and previous two characters.  If at any
		 * time the value of these bits is 010, we have found an
		 * isolated non-ASCII character, which can only occur in a
		 * single-byte 8-bit encoding such as ISO8859-1.
		 *
		 * We include the terminating null character in order to
		 * properly detect a non-ASCII character at the end of a
		 * file that lacks a final newline character.
		 */
		for (i = 0, prev3 = 0; i <= linelen && prev3 != 0x02; ++i)
			prev3 = (prev3 << 1 & 0x07) |
			    (unsigned char)linebuf[i] >> 7;
		if (prev3 != 0x2) {
			/* no conversion necessary */
			if (fprintf(outfile, "%s", linebuf) < 0)
				err(1, "%s", outname);
			continue;
		}
		/* conversion required */
		if (opt_d) {
			fprintf(stderr, "<< %s", linebuf);
			if (linelen == 0 || linebuf[linelen - 1] != '\n')
				fprintf(stderr, "\n");
			fprintf(stderr, ">> ");
		}
		/* reset conversion state */
		iconv(conv, NULL, NULL, NULL, NULL);
		cip = linebuf;
		cilen = linelen;
		do {
			/* repeatedly convert as much as we have room for */
			cop = convbuf;
			colen = sizeof(convbuf) - 1;
			convlen = iconv(conv, &cip, &cilen, &cop, &colen);
			if (convlen == (size_t)-1 && errno != E2BIG)
				errx(1, "%s", inname);
			*cop = '\0';
			if (fprintf(outfile, "%s", convbuf) < 0)
				err(1, "%s", outname);
			if (opt_d)
				fprintf(stderr, "%s", convbuf);
		} while (convlen == (size_t)-1);
		if (opt_d)
			if (linelen == 0 || linebuf[linelen - 1] != '\n')
				fprintf(stderr, "\n");
	}
	free(linebuf);
	if (linelen < 0 && ferror(infile))
		err(1, "%s", inname);
}

static char test_input[] = {
	0xc3, 0xa6, 0x20, 0xc3, 0xb8, 0x20, 0xc3, 0xa5,
	0x0a,
	0x73, 0x6b, 0x6a, 0xe6, 0x72, 0x67, 0xe5, 0x72, 0x64, 0x73, 0xf8, 0x6c,
	0x0a,
	0xf8, 0x73, 0x74,
	0x0a,
	0x74, 0xf8, 0x73,
	0x0a,
	0x73, 0x74, 0xf8,
	0x0a,
	0xe5, 0x73,
	0x0a,
	0x73, 0xe5,
	0x0a,
	0xf8,
	0x0a,
	0xe5,
	0x00
};

static char test_output[] =
    "æ ø å\n"
    "skjærgårdsøl\n"
    "øst\n"
    "tøs\n"
    "stø\n"
    "ås\n"
    "så\n"
    "ø\n"
    "å";

static void
self_test(iconv_t conv)
{
	FILE *infile, *outfile;
	char outbuf[1024];

	if ((infile = fmemopen(test_input, sizeof test_input, "r")) == NULL)
		err(1, "fmemopen()");
	if ((outfile = fmemopen(outbuf, sizeof outbuf, "w")) == NULL)
		err(1, "fmemopen()");
	setbuf(outfile, NULL);
	mixconv(conv, infile, "test input", outfile, "test output");
	fclose(infile);
	fclose(outfile);
	if (memcmp(outbuf, test_output, sizeof test_output) != 0)
		errx(1, "test ouput does not match expected output");
}

static void
usage(void)
{

	fprintf(stderr, "usage: mixconv [-dv] [-f charset] [-o output] ...\n");
	fprintf(stderr, "       mixconv [-dv] -t\n");
	exit(1);
}

static void
version(void)
{

	fprintf(stderr,
	    "This is mixconv from %s.  Please report bugs to %s.\n",
	    PACKAGE_STRING, PACKAGE_BUGREPORT);
	exit(1);
}

int
main(int argc, char *argv[])
{
	const char *inname;
	FILE *infile, *outfile;
	iconv_t conv;
	int opt;

	while ((opt = getopt(argc, argv, "df:o:tv")) != -1)
		switch (opt) {
		case 'd':
			++opt_d;
			break;
		case 'f':
			enc8 = optarg;
			break;
		case 'o':
			outname = optarg;
			break;
		case 't':
			++opt_t;
			break;
		case 'v':
			version();
			break;
		default:
			usage();
		}

	argc -= optind;
	argv += optind;

	/* initialize iconv */
	if ((conv = iconv_open("utf8", enc8)) == (iconv_t)-1)
		err(1, "could not initialize iconv");

	/* run self-test */
	if (opt_t) {
		if (outname || argc > 0)
			usage();
		self_test(conv);
		exit(0);
	}

	/* open output file */
	if (outname) {
		if ((outfile = fopen(outname, "w")) == NULL)
			err(1, "%s", outname);
	} else {
		outname = "stdout";
		outfile = stdout;
	}

	/* process input */
	if (argc > 0) {
		while (argc--) {
			inname = *argv++;
			if ((infile = fopen(inname, "r")) == NULL)
				err(1, "%s", inname);
			mixconv(conv, infile, inname, outfile, outname);
			fclose(infile);
		}
	} else {
		inname = "standard input";
		infile = stdin;
		mixconv(conv, infile, inname, outfile, outname);
	}

	/* done */
	if (outname)
		fclose(outfile);
	iconv_close(conv);
	exit(0);
}
