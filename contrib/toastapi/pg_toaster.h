/*-------------------------------------------------------------------------
 *
 * pg_toaster.h
 *	  definition of the "generalized toaster" system catalog (pg_toaster)
 *	  alike system catalog - for convenience
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/pg_toaster.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TOASTER_H
#define PG_TOASTER_H

#include "postgres.h"

#define Anum_pg_toaster_oid 1
#define Anum_pg_toaster_tsrname 2
#define Anum_pg_toaster_tsrhandler 3

#define Natts_pg_toaster 3

#define DEFAULT_TOASTER_OID 9864

typedef struct FormData_pg_toaster
{
	Oid			oid;			/* oid */

	/* toaster name */
	NameData	tsrname;

	/* handler function */
	text		tsrhandler;
} FormData_pg_toaster;

typedef FormData_pg_toaster *Form_pg_toaster;

#endif							/* PG_TOASTER_H */
