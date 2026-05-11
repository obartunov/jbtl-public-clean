# contrib/qgeom/Makefile

MODULE_big = qgeom
OBJS = \
	$(WIN32RES) \
	qgeom.o \
	qgeom_parser.o

EXTENSION = qgeom
DATA = qgeom--1.0.sql
PGFILEDESC = "qgeom - CUSTOM-toaster probe (Phase 1.a transient form)"

REGRESS = qgeom_transient

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/qgeom
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
