/*-------------------------------------------------------------------------
 *
 * toastapi.c
 *	  Pluggable TOAST API hooks implementations
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/toastapi.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "varatt.h"
#include "fmgr.h"
#include "access/toast_hook.h"
#include "access/genam.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_depend.h"
#include "miscadmin.h"
#include "toastapi.h"
#include "pg_toaster.h"
#include "toastapi_internals.h"
#include "utils/fmgroids.h"
#include "utils/varlena.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

/* FIXME handler oid stored instead of toaster oid in custom pointers */
/*
 *	TOAST API v1.1 uses TOASTER_ID and stores Toaster handler name instead of OID
 *	instead of OID. v1.2 would change TOASTER_ID to sequential numbering instead
 * of getting new system Object ID, to avoid problems with dump and logical replication
 */
/* #define TOASTER_HANDLER_OID_IS_TOASTER_ID */

typedef struct RelToastCache
{
	TsrRoutine	routine;
	Oid			toasterid;
	/* char toaster_options[FLEXIBLE_ARRAY_MEMBER]; */
} RelToastCache;

/* Placeholder for attributes without custom toasters */
static RelToastCache invalid_toast_cache;

static RelToastCache *
init_toaster_cache(Relation rel, Oid toasterid)
{
#ifdef TOASTER_HANDLER_OID_IS_TOASTER_ID
	TsrRoutine *toaster = SearchTsrHandlerCache(toasterid);
#else
	TsrRoutine *toaster = SearchTsrCache(toasterid);
#endif
	RelToastCache *cache = RelationToastCacheAlloc(rel, sizeof(RelToastCache));

	memcpy(&cache->routine, toaster, sizeof(*toaster));
	cache->toasterid = toasterid;

	return cache;
}

static RelToastCache *
get_toaster_cache_for_attr(Relation rel, int attnum)
{
	void	  **rd_toastcache = RelationGetToastCache(rel);
	RelToastCache *cache = rd_toastcache[attnum];

	if (!cache)
	{
		char	   *toasterid_str =
#ifdef TOASTER_HANDLER_OID_IS_TOASTER_ID
			attopts_get_toaster_opts(rel, attnum + 1, ATT_HANDLER_NAME);
#else
			attopts_get_toaster_opts(rel, attnum + 1, ATT_TOASTER_NAME);
#endif

		if (!toasterid_str)
			cache = &invalid_toast_cache;
		else
		{
			Oid toasterid = atoi(toasterid_str);

			if (OidIsValid(toasterid))
				cache = init_toaster_cache(rel, toasterid);
			else
				cache = &invalid_toast_cache;
		}

		/* reread rd_toastcache after possible relcache invalidations */
		rd_toastcache = RelationGetToastCache(rel);
		rd_toastcache[attnum] = cache;
	}

	if (cache == &invalid_toast_cache)
		return NULL;
	/* copy cache into current memory context, referencing rd_toastcache is unsafe */
	return memcpy(palloc(sizeof(*cache)), cache, sizeof(*cache));
}

/*
 * Routine-struct resolver implementations — Option A (two-hook split).
 *
 * Wired to get_toaster_id_for_rel_hook / get_toaster_routine_for_id_hook
 * at _PG_init.  Core call sites dispatch toast/update/copy/delete by
 * composing the two: rel-hook -> Oid -> id-hook -> routine.  Read side
 * (detoast/size) uses get_toaster_routine_for_id_hook directly with the
 * Oid taken from varatt_custom.va_toasterid.
 *
 * Lifetime contract: the returned pointer is valid for the immediate
 * call only.  Core must not retain it.  See access/toasterapi.h.
 */
static Oid
toastapi_get_id_for_rel(Relation rel, AttrNumber attnum)
{
	RelToastCache *cache = get_toaster_cache_for_attr(rel, (int) attnum);

	if (cache == NULL)
		return InvalidOid;

	return cache->toasterid;
}

static const TsrRoutine *
toastapi_get_routine_for_id(Oid toasterid)
{
	return GetTsrRoutineByOid(toasterid, /*noerror=*/true);
}

void _PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries. If not, report an ERROR.
	 */
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("TOASTAPI module must be loaded as shared library."),
				 errdetail("Add 'toastapi' into the shared_preload_libraries list.")));

	/*
	 * Install routine resolver hooks (Option A two-hook split).  Core
	 * call sites dispatch toast/update/copy/delete by composing
	 * id-hook(rel,attno) -> Oid -> routine-hook(Oid) -> routine.  Read
	 * side dispatches detoast/size through routine-hook(Oid) with the
	 * Oid taken from varatt_custom.va_toasterid.  The flat lifecycle
	 * hooks are gone after this lifecycle-conversion patch.
	 */
	get_toaster_id_for_rel_hook = toastapi_get_id_for_rel;
	get_toaster_routine_for_id_hook = toastapi_get_routine_for_id;
}
