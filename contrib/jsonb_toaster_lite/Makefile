# contrib/jsonb_toaster_lite/Makefile

MODULE_big = jsonb_toaster_lite
OBJS = \
	$(WIN32RES) \
	jsonb_toaster_lite.o \
	jsonb_toaster_lite_chain.o \
	jsonb_toaster_lite_pointers.o \
	jsonb_toaster_lite_object_field.o \
	jsonb_toaster_lite_subtree_refs.o

EXTENSION = jsonb_toaster_lite
DATA = jsonb_toaster_lite--1.0.sql
PGFILEDESC = "jsonb_toaster_lite - TOAST/storage layer for jsonb"

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/jsonb_toaster_lite/jsonb_toaster_lite.conf
REGRESS = jsonb_toaster_lite jsonb_toaster_lite_roundtrip jsonb_toaster_lite_slice_plain jsonb_toaster_lite_slice_compressed jsonb_toaster_lite_object_field jsonb_toaster_lite_update_v1 jsonb_toaster_lite_diff_read_v1 jsonb_toaster_lite_vacuum_full jsonb_toaster_lite_subtree_ownership jsonb_toaster_lite_recursive_reuse jsonb_toaster_lite_nondefault_schema
EXTRA_INSTALL = contrib/toastapi

ifdef USE_PGXS
PG_CPPFLAGS = -I$(top_srcdir)/contrib/toastapi
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
PG_CPPFLAGS = -I$(top_srcdir)/contrib/toastapi
subdir = contrib/jsonb_toaster_lite
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
