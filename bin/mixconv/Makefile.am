AM_CPPFLAGS = -I$(top_srcdir)/lib
bin_PROGRAMS = mixconv
# mixconv_LDADD = $(top_builddir)/lib/libcv.a $(ICONV_LIBS)
mixconv_LDADD = $(ICONV_LIBS)
dist_man1_MANS = mixconv.1
TESTS = t_mixconv
EXTRA_DIST = $(TESTS)
