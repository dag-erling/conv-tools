#!/bin/sh

[ -d m4 ] || mkdir m4
aclocal -I m4
autoheader
automake -a -c --foreign
autoconf
