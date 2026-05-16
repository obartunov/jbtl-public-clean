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
#include "access/toasterapi.h"

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

/*
 * TsrRoutine, ToasterContextData, and the callback function-pointer
 * typedefs (toaster_toast_function, ...) are now declared in
 * src/include/access/toasterapi.h, which is included above.  The
 * extension implementation continues to construct and own these.
 */

/* Functions in toastapi.c */
extern PGDLLEXPORT void _PG_init(void);
extern PGDLLEXPORT TsrRoutine * GetTsrRoutine(Oid tsrhandler);
extern PGDLLEXPORT TsrRoutine * GetTsrRoutineByOid(Oid tsroid, bool noerror);
extern PGDLLEXPORT TsrRoutine * SearchTsrCache(Oid tsroid);
extern PGDLLEXPORT TsrRoutine * SearchTsrHandlerCache(Oid tsrhandleroid);
extern PGDLLEXPORT bool validateToaster(Oid toasteroid, Oid typeoid, char storage,
										char compression, Oid amoid, bool false_ok);

#endif							/* TOASTAPI_H */
