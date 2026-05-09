/*-------------------------------------------------------------------------
 *
 * pg_toastrel.h
 *	  definition of the "generalized toaster" system catalog (pg_toastrel)
 *	  alike system catalog - for convenience
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/pg_toastrel.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TOASTREL_H
#define PG_TOASTREL_H

#include "postgres.h"

#define ToastrelRelationId 9881
#define ToastrelOidIndexId 9882
#define ToastrelKeyIndexId 9883
#define ToastrelRelIndexId 9884
#define ToastrelTsrIndexId 9885

#define Anum_pg_toastrel_oid 1
#define Anum_pg_toastrel_toasteroid 2
#define Anum_pg_toastrel_relid 3
#define Anum_pg_toastrel_toastentid 4
#define Anum_pg_toastrel_attnum 5
#define Anum_pg_toastrel_version 6
#define Anum_pg_toastrel_flag 7
#define Anum_pg_toastrel_toastoptions 8

#define Natts_pg_toastrel 8

/* ----------------
 *		pg_toastrel definition.  cpp turns this into
 *		typedef struct FormData_pg_toastrel
 * ----------------
 */
typedef struct FormData_pg_toastrel
{
	Oid			oid;			   /* oid */
   Oid			toasteroid;		/* oid */
   Oid			relid;		   /* oid */
   Oid			toastentid;		/* oid */
   int16			attnum;		   /* oid */
   int16       version;
   char		   flag;	         /* Cleanup flag */
	char		   toastoptions;	/* Toast options */
} FormData_pg_toastrel;

/* ----------------
 *		Form_pg_toastrel corresponds to a pointer to a tuple with
 *		the format of pg_toastrel relation.
 * ----------------
 */
typedef FormData_pg_toastrel *Form_pg_toastrel;
#endif							/* PG_TOASTREL_H */
