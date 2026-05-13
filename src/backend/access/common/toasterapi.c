/*-------------------------------------------------------------------------
 *
 * toasterapi.c
 *	  Storage for the Pluggable TOAST API resolver hooks.
 *
 *	Both resolver hooks default to NULL.  The provider extension
 *	(typically contrib/toastapi) installs them at _PG_init time;
 *	when no provider is loaded both stay NULL and core falls back to
 *	vanilla TOAST behavior on writes, ERRORs on reads of a CUSTOM
 *	varlena.
 *
 *	See src/include/access/toasterapi.h for the ownership contract.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 2016-2026, Postgres Professional
 *
 * src/backend/access/common/toasterapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/toasterapi.h"

toaster_routine_for_rel_hook_type	get_toaster_routine_for_rel_hook = NULL;
toaster_routine_for_id_hook_type	get_toaster_routine_for_id_hook = NULL;
