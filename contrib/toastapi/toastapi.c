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

static Toastapi_toast_hook_type toastapi_toast_hook = NULL;
static Toastapi_detoast_hook_type toastapi_detoast_hook = NULL;
static Toastapi_size_hook_type toastapi_size_hook = NULL;
static Toastapi_copy_hook_type toastapi_copy_hook = NULL;
static Toastapi_update_hook_type toastapi_update_hook = NULL;
static Toastapi_delete_hook_type toastapi_delete_hook = NULL;

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

static TsrRoutine *
get_toaster_for_attr(Relation rel, int attnum, ToasterContext cxt)
{
	RelToastCache *cache = get_toaster_cache_for_attr(rel, attnum);

	if (!cache)
		return NULL;

	if (cxt)
	{
		cxt->rel = rel;
		cxt->toastreloid = rel->rd_rel->reltoastrelid;
		cxt->toasterid = cache->toasterid;
		cxt->toaster = &cache->routine;
		cxt->attnum = attnum + 1;
	}

	return &cache->routine;
}

static Datum
toastapi_toast(Relation rel, int attnum, Datum value, int max_inline_len, int options)
{
	ToasterContextData tcxt;
	TsrRoutine *toaster = get_toaster_for_attr(rel, attnum, &tcxt);
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Form_pg_attribute att = TupleDescAttr(tupdesc, attnum);
	ToastCompressionId cmid;

	if (!toaster)
		return (Datum) 0;

	if (!OidIsValid(rel->rd_rel->reltoastrelid))
		elog(ERROR, "toast relation is missing for toasted attribute %d of relation %u",
			 attnum, RelationGetRelid(rel));

	if (att->attstorage == TYPSTORAGE_PLAIN ||
		att->attstorage == TYPSTORAGE_EXTERNAL)
	{
		cmid = TOAST_INVALID_COMPRESSION_ID;
	}
	else
	{
		switch (att->attcompression != InvalidCompressionMethod ? att->attcompression : default_toast_compression)
		{
			case TOAST_PGLZ_COMPRESSION:
				cmid = TOAST_PGLZ_COMPRESSION_ID;
				break;
			case TOAST_LZ4_COMPRESSION:
				cmid = TOAST_LZ4_COMPRESSION_ID;
				break;
			default:
				Assert(false);
				cmid = TOAST_INVALID_COMPRESSION_ID;
				break;
		}
	}

	return toaster->tsr_toast(&tcxt, value, value,
							  max_inline_len, options,
							  att->attstorage, cmid);
}

static Size
toastapi_size(const void *ptr, ToastPtrSizeType sz_type)
{
	if (sz_type == TPTR_DATUM_SIZE ||
		sz_type == TPTR_STORAGE_SIZE) /* FIXME */
		return offsetof(varatt_custom, va_toasterdata) + VARATT_CUSTOM_GET_DATA_SIZE(ptr);
	else if (sz_type == TPTR_RAW_SIZE)
		return VARATT_CUSTOM_GET_DATA_RAW_SIZE(ptr);
	else
		elog(ERROR, "invalid toastapi_size() request");

	return 0; /* avoid warning */
}

static TsrRoutine *
get_toaster_for_ptr(Relation rel, int attnum, Datum toast_ptr, ToasterContext tcxt)
{
	struct varlena *custom_toast_ptr = (struct varlena *) DatumGetPointer(toast_ptr);
	Oid			toasterid;
	TsrRoutine *toaster = NULL;
	RelToastCache *cache;

	Assert(VARATT_IS_CUSTOM(custom_toast_ptr));
	toasterid = VARATT_CUSTOM_GET_TOASTERID(custom_toast_ptr);

	if (rel && attnum >= 0 &&
		(cache = get_toaster_cache_for_attr(rel, attnum)) &&
		cache->toasterid == toasterid)
		toaster = &cache->routine;
	else
#ifdef TOASTER_HANDLER_OID_IS_TOASTER_ID
		toaster = SearchTsrHandlerCache(toasterid);
#else
		toaster = SearchTsrCache(toasterid);
#endif

	if (tcxt)
	{
		tcxt->rel = rel;
		tcxt->toasterid = toasterid;
		tcxt->toastreloid = rel ? rel->rd_rel->reltoastrelid : InvalidOid;
		tcxt->toaster = toaster;
		tcxt->attnum = attnum + 1;
	}

	return toaster;
}

static Datum
toastapi_detoast(Datum toast_ptr, int offset, int length)
{
	ToasterContextData tcxt;
	TsrRoutine *toaster;

#if 0 /* TODO */
	if (VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(toast_ptr)))
		return custom_detoast(toast_ptr, offset, length);
#endif

	toaster = get_toaster_for_ptr(NULL, -1, toast_ptr, &tcxt);

	return toaster->tsr_detoast(&tcxt, toast_ptr, offset, length);
}

static Datum
toastapi_update(Relation rel, int attnum,
				Datum new_value, Datum old_value, int am_options)
{
	struct varlena *new_val = (struct varlena *) DatumGetPointer(new_value);
	struct varlena *old_val = (struct varlena *) DatumGetPointer(old_value);
	ToasterContextData tcxt;
	TsrRoutine *toaster;
	Oid			old_toasterid;

	/* old_val must be CUSTOM (the column was already toaster-owned). */
	Assert(VARATT_IS_CUSTOM(old_val));

	old_toasterid = VARATT_CUSTOM_GET_TOASTERID(old_val);

	/* If the new value is also CUSTOM, require matching toasterid. */
	if (VARATT_IS_CUSTOM(new_val))
	{
		Oid new_toasterid = VARATT_CUSTOM_GET_TOASTERID(new_val);
		if (new_toasterid != old_toasterid)
			return (Datum) 0;
	}

	toaster = get_toaster_for_attr(rel, attnum, &tcxt);

	/* toaster was reset or another toaster was set, retoast value */
	if (!toaster || tcxt.toasterid != old_toasterid)
		return (Datum) 0;

	/* toaster does not support custom updates, retoast value */
	if (!toaster->tsr_update)
		return (Datum) 0;

	return toaster->tsr_update(&tcxt, new_value, old_value, am_options);
}

static Datum
toastapi_copy(Relation rel, int attnum, Datum value, int am_options)
{
	ToasterContextData tcxt;
	TsrRoutine *toaster = get_toaster_for_attr(rel, attnum, &tcxt);
	Oid			toasterid = VARATT_CUSTOM_GET_TOASTERID(DatumGetPointer(value));

	/* detoast value, if toaster was changed or tsr_copy() is not defined */
	if (!toaster || toasterid != tcxt.toasterid || !toaster->tsr_copy)
		return (Datum) 0;

	return toaster->tsr_copy(&tcxt, value, am_options);
}

static void
toastapi_delete(Relation rel, int attnum, Datum value, bool is_speculative)
{
	ToasterContextData tcxt;
	TsrRoutine *toaster = get_toaster_for_ptr(rel, attnum, value, &tcxt);

	if (!toaster->tsr_delete)
		return;

	toaster->tsr_delete(&tcxt, value, is_speculative);
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

	toastapi_toast_hook = Toastapi_toast_hook;
	toastapi_detoast_hook = Toastapi_detoast_hook;
	toastapi_size_hook = Toastapi_size_hook;
	toastapi_copy_hook = Toastapi_copy_hook;
	toastapi_update_hook = Toastapi_update_hook;
	toastapi_delete_hook = Toastapi_delete_hook;

	Toastapi_toast_hook = toastapi_toast;
	Toastapi_detoast_hook = toastapi_detoast;
	Toastapi_size_hook = toastapi_size;
	Toastapi_copy_hook = toastapi_copy;
	Toastapi_update_hook = toastapi_update;
	Toastapi_delete_hook = toastapi_delete;
}
