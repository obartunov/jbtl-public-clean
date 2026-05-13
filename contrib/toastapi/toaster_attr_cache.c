/*-------------------------------------------------------------------------
 *
 * toaster_attr_cache.c
 *	  Per-backend (relid, attnum) -> TsrRoutine cache for the
 *	  Pluggable TOAST API.
 *
 *	This cache replaces the Relation.rd_toastcache / rd_toastcachecxt
 *	fields that were carried in core RelationData.  The provider owns
 *	the cache, its memory, and its invalidation lifecycle.  Core never
 *	sees these objects.
 *
 *	Memory context:  CacheMemoryContext.
 *	Invalidation:    via CacheRegisterRelcacheCallback.  Any relcache
 *	                 event on a relid drops every (relid, *) entry from
 *	                 the cache; relid == InvalidOid drops everything
 *	                 (this matches the global-invalidation semantics
 *	                 the relcache callback uses for "drop all" events).
 *
 *	Lookup contract: a non-found entry triggers a one-time catalog
 *	probe (attopts_get_toaster_opts + GetTsrRoutineByOid) under the
 *	caller's lock on the relation.  Negative results are cached too
 *	(toasterid = InvalidOid) so the catalog probe doesn't repeat per
 *	row.
 *
 *	Returns a TsrRoutine * whose lifetime is the entire cache entry's
 *	lifetime (i.e., until relcache invalidation).  Per the routine
 *	ownership contract in access/toasterapi.h, core MUST NOT retain
 *	this pointer past the immediate call.
 *
 *
 * Portions Copyright (c) 2016-2026, Postgres Professional
 *
 * contrib/toastapi/toaster_attr_cache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/toasterapi.h"
#include "toastapi.h"
#include "toastapi_internals.h"
#include "toastapi_sqlfuncs.h"
#include "toaster_attr_cache.h"
#include "utils/catcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

typedef struct ToasterAttrCacheKey
{
	Oid			relid;
	AttrNumber	attnum;
} ToasterAttrCacheKey;

typedef struct ToasterAttrCacheEntry
{
	ToasterAttrCacheKey key;	/* HASH_BLOBS key */
	Oid			toasterid;		/* InvalidOid means "no toaster bound" */
	TsrRoutine	routine;		/* embedded copy; valid iff OidIsValid(toasterid) */
} ToasterAttrCacheEntry;

static HTAB *toaster_attr_cache = NULL;
static bool toaster_attr_cache_callback_installed = false;

static void
toaster_attr_cache_invalidate(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	ToasterAttrCacheEntry *entry;

	if (toaster_attr_cache == NULL)
		return;

	hash_seq_init(&status, toaster_attr_cache);
	while ((entry = (ToasterAttrCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (relid == InvalidOid || entry->key.relid == relid)
		{
			if (hash_search(toaster_attr_cache, &entry->key,
							HASH_REMOVE, NULL) == NULL)
				elog(WARNING, "toaster_attr_cache corrupted at relid=%u attnum=%d",
					 entry->key.relid, (int) entry->key.attnum);
		}
	}
}

static void
toaster_attr_cache_init_if_needed(void)
{
	HASHCTL		ctl;

	if (toaster_attr_cache != NULL)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(ToasterAttrCacheKey);
	ctl.entrysize = sizeof(ToasterAttrCacheEntry);
	ctl.hcxt = CacheMemoryContext;

	toaster_attr_cache = hash_create("toaster attr cache",
									 64, &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	if (!toaster_attr_cache_callback_installed)
	{
		CacheRegisterRelcacheCallback(toaster_attr_cache_invalidate, (Datum) 0);
		toaster_attr_cache_callback_installed = true;
	}
}

/*
 * Look up the TsrRoutine bound to (rel, attnum), populating the cache
 * on a miss.  Returns NULL if no toaster is registered for the column;
 * negative results are cached too.
 *
 * Caller is expected to hold a lock on rel sufficient to read its
 * attribute options (typically AccessShareLock or higher from the
 * surrounding query/DDL).
 */
TsrRoutine *
ToasterAttrCacheLookup(Relation rel, AttrNumber attnum, Oid *out_toasterid)
{
	ToasterAttrCacheKey key;
	ToasterAttrCacheEntry *entry;
	bool		found;

	toaster_attr_cache_init_if_needed();

	key.relid = RelationGetRelid(rel);
	key.attnum = attnum;

	entry = (ToasterAttrCacheEntry *) hash_search(toaster_attr_cache, &key,
												  HASH_ENTER, &found);

	if (!found)
	{
		char	   *toasterid_str;
		TsrRoutine *source = NULL;

		entry->toasterid = InvalidOid;
		memset(&entry->routine, 0, sizeof(entry->routine));

		/*
		 * Catalog probe.  attopts_get_toaster_opts uses 1-based attnum
		 * (pg_attribute semantics).
		 */
#ifdef TOASTER_HANDLER_OID_IS_TOASTER_ID
		toasterid_str = attopts_get_toaster_opts(rel, attnum + 1,
												 ATT_HANDLER_NAME);
#else
		toasterid_str = attopts_get_toaster_opts(rel, attnum + 1,
												 ATT_TOASTER_NAME);
#endif

		if (toasterid_str != NULL)
		{
			Oid			toasterid = atoi(toasterid_str);

			if (OidIsValid(toasterid))
			{
#ifdef TOASTER_HANDLER_OID_IS_TOASTER_ID
				source = SearchTsrHandlerCache(toasterid);
#else
				source = SearchTsrCache(toasterid);
#endif
				if (source != NULL)
				{
					entry->toasterid = toasterid;
					memcpy(&entry->routine, source, sizeof(entry->routine));
				}
			}
		}
	}

	if (!OidIsValid(entry->toasterid))
		return NULL;

	if (out_toasterid != NULL)
		*out_toasterid = entry->toasterid;
	return &entry->routine;
}
