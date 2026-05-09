/*-------------------------------------------------------------------------
 *
 * toastapi_sqlfuncs.h
 *	  SQL interface definitions for Pluggable TOAST API
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/toastapi_sqlfuncs.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOASTAPISQL_H
#define TOASTAPISQL_H

#include "postgres.h"
#include "fmgr.h"

extern PGDLLEXPORT Datum add_toaster(PG_FUNCTION_ARGS);

extern PGDLLEXPORT Datum set_toaster(PG_FUNCTION_ARGS);

extern PGDLLEXPORT Datum drop_toaster(PG_FUNCTION_ARGS);

extern PGDLLEXPORT Datum get_toaster(PG_FUNCTION_ARGS);

extern PGDLLEXPORT Datum get_toaster_id(PG_FUNCTION_ARGS);

#endif
