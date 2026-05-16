/*-------------------------------------------------------------------------
 *
 * toaster_cache.c
 *	  Pluggable TOAST API internal cache implementation
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/toaster_cache.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/regproc.h"

#include "toastapi.h"
#include "toaster_cache.h"
#include "toastapi_internals.h"
#include "pg_toaster.h"

static List	*ToasterCache = NIL;

static TsrRoutine *
SearchTsrCacheInternal(Oid toasterOid, bool isHandlerOid)
{
	ListCell		   *lc;
	ToasterCacheEntry  *entry;
	MemoryContext		ctx;

	if (list_length(ToasterCache) > 0)
	{
		/* fast path */
		entry = (ToasterCacheEntry*)linitial(ToasterCache);
		if (entry->toasterOid == toasterOid)
			return entry->routine;
	}

	/* didn't find in first position */
	ctx = MemoryContextSwitchTo(CacheMemoryContext);

	for_each_from(lc, ToasterCache, 0)
	{
		entry = (ToasterCacheEntry*)lfirst(lc);

		if (entry->toasterOid == toasterOid)
		{
			goto out;
		}
	}

	/* did not find entry, make a new one */
	entry = palloc(sizeof(*entry));

	entry->toasterOid = toasterOid;
	entry->routine = isHandlerOid ? GetTsrRoutine(toasterOid) : GetTsrRoutineByOid(toasterOid, false);

	ToasterCache = lappend(ToasterCache, entry);

out:
	MemoryContextSwitchTo(ctx);

	return entry->routine;
}

/*
 * SearchTsrCache - get cached toaster routine by toaster oid.
 * Emits an error if toaster doesn't exist.
 */
TsrRoutine *
SearchTsrCache(Oid toasterOid)
{
	return SearchTsrCacheInternal(toasterOid, false);
}

/*
 * SearchTsrHandlerCache - get cached toaster routine by toaster handler oid.
 * Emits an error if toaster doesn't exist.
 */
TsrRoutine*
SearchTsrHandlerCache(Oid toastHandlerOid)
{
	return SearchTsrCacheInternal(toastHandlerOid, true);
}

static void
reportMissingToastMethod(const char *method_name, Oid tsrhandler)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("mandatory function %s is not present in toaster routine returned by toast handler function %u",
					"toast()", tsrhandler))); /* get_toaster_name(tsroid) */
}

/*
 * GetRoutine - call the specified toaster handler routine to get
 * its TsrRoutine struct, which will be palloc'd in the caller's context.
 */
TsrRoutine *
GetTsrRoutine(Oid tsrhandler)
{
	TsrRoutine *routine;
	FmgrInfo	flinfo;
	Datum		result;

	LOCAL_FCINFO(fcinfo_tsrhandler, 1);
	fmgr_info(tsrhandler, &flinfo);

	InitFunctionCallInfoData(*fcinfo_tsrhandler, &flinfo, 1, InvalidOid, NULL, NULL);
	fcinfo_tsrhandler->args[0].isnull = true;
	fcinfo_tsrhandler->args[0].value = (Datum) 0;

	result = FunctionCallInvoke(fcinfo_tsrhandler);

	if (fcinfo_tsrhandler->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("toaster handler function %u did not return a %s",
						tsrhandler, "struct TsrRoutine")));

	routine = (TsrRoutine *) DatumGetPointer(result);

	if (routine == NULL || routine->tsr_size != sizeof(TsrRoutine))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("toaster handler function %u did not return a %s",
						tsrhandler, "struct TsrRoutine")));

	if (!routine->tsr_validate)
		reportMissingToastMethod("tsr_validate()", tsrhandler);

	if (!routine->tsr_toast)
		reportMissingToastMethod("tsr_toast()", tsrhandler);

	if (!routine->tsr_detoast)
		reportMissingToastMethod("tsr_detoast()", tsrhandler);

	return routine;
}

/*
 * GetTsrRoutineByOid - look up the handler of the toaster
 * with the given OID, and get its TsrRoutine struct.
 *
 * If the given OID isn't a valid toaster, returns NULL if
 * noerror is true, else throws error.
 */
TsrRoutine *
GetTsrRoutineByOid(Oid tsroid, bool noerror)
{
	Relation	rel;
	Oid			idx_oid;
	bool		idx_found = false;
	List	   *indexlist;
	ListCell   *lc;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	int			nkeys = 0;
	regproc		tsrhandler = InvalidOid;

	rel = get_rel_from_relname(cstring_to_text(PG_TOASTER_NAME), AccessShareLock, ACL_SELECT);

	/* Try to find oid index */
	indexlist = RelationGetIndexList(rel);

	foreach(lc, indexlist)
	{
		Relation	idx_rel;

		idx_oid = lfirst_oid(lc);
		idx_rel = index_open(idx_oid, AccessShareLock);

		if (idx_rel->rd_index->indisvalid &&
			IndexRelationGetNumberOfKeyAttributes(idx_rel) == 1 &&
			idx_rel->rd_index->indkey.values[0] == Anum_pg_toaster_oid &&
			idx_rel->rd_rel->relam == BTREE_AM_OID)
			idx_found = true;

		index_close(idx_rel, AccessShareLock);

		if (idx_found)
			break;
	}

	list_free(indexlist);

	if (idx_found)
	{
		ScanKeyInit(&key[nkeys],
					Anum_pg_toaster_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					tsroid);
		nkeys++;
	}

	/* Find toaster by oid */
	scan = systable_beginscan(rel, idx_found ? idx_oid : InvalidOid,
							  idx_found, NULL, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_toaster toaster = (Form_pg_toaster) GETSTRUCT(tup);

		if (tsroid == toaster->oid)
		{
			tsrhandler = (regproc) get_toaster_handler_oid(text_to_cstring(&toaster->tsrhandler));
			break;
		}
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * Get the handler function oid, verifying the toaster type while at it.
	 */
	if (!RegProcedureIsValid(tsrhandler))
	{
		if (noerror)
			return NULL;

		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("toaster \"%u\" does not have a handler",
						tsroid)));
	}

	/* And finally, call the handler function to get the API struct. */
	return GetTsrRoutine(tsrhandler);
}

/*
 * could toaster operates with given type and access method?
 * If it can't then validate method should emit an error if false_ok = false
 */
bool
validateToaster(Oid toasteroid, Oid typeoid,
				char storage, char compression, Oid amoid, bool false_ok)
{
	TsrRoutine *tsrroutine;
	bool		result;

	if (!TypeIsToastable(typeoid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("data type %s can not be toasted",
						format_type_be(typeoid))));

	tsrroutine = GetTsrRoutineByOid(toasteroid, false_ok);

	/* if false_ok == false then GetTsrRoutineByOid emits an error */
	if (tsrroutine == NULL)
		return false;

	result = tsrroutine->tsr_validate(toasteroid, typeoid,
									  storage, compression,
									  amoid, false_ok);

	pfree(tsrroutine);

	Assert(result || false_ok);

	return result;
}
