AM_CPPFLAGS = -I$(top_srcdir)/include

noinst_PROGRAMS = CDemo
CDemo_SOURCES = CDemo.c dummy.cpp
CDemo_LDADD = ../src/libslvs.la

dummy.cpp:
	$(AM_V_GEN): > $@

EXTRA_DIST = \
	DOC.txt \
	Makefile.msvc \
	VbDemo.vb
