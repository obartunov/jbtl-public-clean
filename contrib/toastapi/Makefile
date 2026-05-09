# contrib/toastapi/Makefile

MODULE_big = toastapi
EXTVERSION = 1.1
OBJS = \
	$(WIN32RES) \
	toastapi_internals.o \
	toastapi_sqlfuncs.o \
	toaster_cache.o \
	toastapi.o \
	dummy_toaster.o

EXTENSION = toastapi
DATA = toastapi--1.0.sql toastapi--1.0--1.1.sql
PGFILEDESC = "toastapi - Pluggable TOAST API"

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/toastapi/toastapi.conf
REGRESS = toastapi

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/toastapi
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
