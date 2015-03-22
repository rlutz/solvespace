## exposed/Makefile.am

AM_CPPFLAGS = -I$(top_srcdir)/include

noinst_PROGRAMS  = CDemo

CDemo_SOURCES = CDemo.c

CDemo_LDADD =

if HAVE_FLTK
CDemo_SOURCES += ../src/posixutil.cpp
endif
if WIN32
CDemo_SOURCES += ../src/win32/w32util.cpp
endif

CDemo_LDADD += ../src/libslvs.la

EXTRA_DIST = \
	DOC.txt \
	Makefile.msvc \
	VbDemo.vb

## end exposed/Makefile.am
