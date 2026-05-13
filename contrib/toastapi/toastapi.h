/*-------------------------------------------------------------------------
 *
 * toastapi.h
 *	  Pluggable TOAST API definitions and Toaster routine definition
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/toastapi.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOASTAPI_H
#define TOASTAPI_H

#include "postgres.h"
#include "varatt.h"
#include "fmgr.h"
#include "access/toast_compression.h"
#include "utils/relcache.h"
#include "access/toast_custom.h"

#define TOASTER_HANDLEROID 8888

#define PG_TOASTER_NAME "pgpro_toast.pg_toaster"
#define PG_TOASTREL_NAME "pgpro_toast.pg_toastrel"

#define REL_TOASTER_NAME "pgpro_toasteroid"
#define REL_BASEREL_NAME "pgpro_basereloid"

#define ATT_TOASTER_NAME "pgpro_toasteroid"
#define ATT_HANDLER_NAME "pgpro_toasthandler"
#define ATT_TOASTREL_NAME "pgpro_toastreloid"
#define ATT_NTOASTERS_NAME "pgpro_ntoasters"

/*
 * Macro to fetch the possibly-unaligned contents of an EXTERNAL datum
 * into a local "struct varatt_external" toast pointer.  This should be
 * just a memcpy, but some versions of gcc seem to produce broken code
 * that assumes the datum contents are aligned.  Introducing an explicit
 * intermediate "varattrib_1b_e *" variable seems to fix it.
 */
#define VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_EXTERNAL(attre), sizeof(toast_pointer)); \
} while (0)

/* Size of an EXTERNAL datum that contains a standard TOAST pointer */
#define TOAST_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external))

/* Size of an EXTERNAL datum that contains an indirection pointer */
#define INDIRECT_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_indirect))

#define VARATT_TOASTER_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_TOASTER(attre)); \
	Assert(VARSIZE_TOASTER(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_TOASTER(attre), sizeof(toast_pointer)); \
} while (0)

/* Size of an EXTERNAL datum that contains a custom TOAST pointer */
#define TOASTER_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_custom))

typedef struct TsrRoutine TsrRoutine;

typedef struct ToasterContextData
{
	TsrRoutine *toaster;
	Relation	rel;
	Oid			toasterid;
	Oid			toastreloid;
	int			attnum;
	int			options;
} ToasterContextData;

typedef ToasterContextData *ToasterContext;

/*
 * Callback function signatures --- see toaster.sgml for more info.
 */

/* Toaster function */
typedef Datum (*toaster_toast_function) (ToasterContext tcxt,
										 Datum value,
										 Datum old_value,
										 int max_inline_size,
										 int am_options,
										 char att_storage,
										 ToastCompressionId cmid);

/* Update toast function, optional */
typedef Datum (*toaster_update_function) (ToasterContext tcxt,
										  Datum new_value,
										  Datum old_value,
										  int am_options);

/* Copy toast function, optional */
typedef Datum (*toaster_copy_function) (ToasterContext tcxt,
										Datum value,
										int am_options);

/* Delete toast function, optional */
typedef void (*toaster_delete_function) (ToasterContext tcxt,
										 Datum value,
										 bool is_speculative);

/* Detoast function */
typedef Datum (*toaster_detoast_function) (ToasterContext tcxt,
										   Datum toast_ptr,
										   int offset,
										   int length);

/* Return virtual table of functions, optional */
/* validate definition of a toaster Oid */
typedef bool (*toaster_validate_function) (Oid toasteroid, Oid typeoid,
										   char storage, char compression,
										   Oid amoid, bool false_ok);

/*
 * API struct for Toaster.
 *
 * Note this must be stored in a single palloc'd chunk of memory.
 */

#define TSR_ROUTINE_MAGIC	0x54747252	/* "TsrR" */

struct TsrRoutine
{
	uint32		tsr_magic;

	/* mandatory interface functions */
	toaster_validate_function tsr_validate;
	toaster_toast_function tsr_toast;
	toaster_detoast_function tsr_detoast;
	/* optional interface functions */
	toaster_update_function tsr_update;
	toaster_copy_function tsr_copy;
	toaster_delete_function tsr_delete;
};

static inline TsrRoutine *
MakeTsrRoutine(void)
{
	TsrRoutine *tsr = palloc0(sizeof(*tsr));

	tsr->tsr_magic = TSR_ROUTINE_MAGIC;

	return tsr;
}

/* Functions in toastapi.c */
extern PGDLLEXPORT void _PG_init(void);
extern PGDLLEXPORT TsrRoutine *GetTsrRoutine(Oid tsrhandler);
extern PGDLLEXPORT TsrRoutine *GetTsrRoutineByOid(Oid tsroid, bool noerror);
extern PGDLLEXPORT TsrRoutine *SearchTsrCache(Oid tsroid);
extern PGDLLEXPORT TsrRoutine *SearchTsrHandlerCache(Oid tsrhandleroid);
extern PGDLLEXPORT bool	validateToaster(Oid toasteroid, Oid typeoid, char storage,
							char compression, Oid amoid, bool false_ok);

#endif							/* TOASTAPI_H */
