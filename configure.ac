AC_PREREQ([2.63])
AC_INIT([conv-tools], [20160831], [des@des.no], [conv-tools],
  [http://github.com/dag-erling/conv-tools])
AC_CONFIG_SRCDIR([lib/conv-tools.h])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_HEADERS([config.h])

############################################################################
#
# Toolchain
#

# C compiler and features
AC_LANG(C)
AC_PROG_CC
AC_PROG_CC_STDC
AC_PROG_CPP
AC_GNU_SOURCE
AC_C_CONST

# other programs
AC_PROG_INSTALL

############################################################################
#
# Build options
#

# Developer-friendly compiler flags
AC_ARG_ENABLE([debugging-symbols],
    AS_HELP_STRING([--enable-debugging-symbols],
	[enable debugging symbols (default is NO)]),
    [CFLAGS="${CFLAGS} -O0 -g -fno-inline"])
AC_ARG_ENABLE([developer-warnings],
    AS_HELP_STRING([--enable-developer-warnings],
	[enable strict warnings (default is NO)]),
    [CFLAGS="${CFLAGS} -Wall -Wextra"])
AC_ARG_ENABLE([werror],
    AS_HELP_STRING([--enable-werror],
	[use -Werror (default is NO)]),
    [CFLAGS="${CFLAGS} -Werror"])

############################################################################
#
# Output
#

AC_CONFIG_FILES([
    Makefile
    lib/Makefile
    bin/Makefile
    bin/dirconv/Makefile
    bin/mixconv/Makefile
])
AC_OUTPUT
