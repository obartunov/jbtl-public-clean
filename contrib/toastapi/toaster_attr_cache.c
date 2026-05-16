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
 *	Invalidation:    via CacheRegisterRelcacheCallback, registered at
 *	                 _PG_init time (not lazily) so the callback is
 *	                 active before any backend can populate the cache.
 *	                 Any relcache event on a relid drops every
 *	                 (relid, *) entry; relid == InvalidOid drops
 *	                 everything (matches the global-invalidation
 *	                 semantics the relcache callback uses for
 *	                 "drop all" events).
 *
 *	Lookup contract: a non-found entry triggers a one-time catalog
 *	probe (attopts_get_toaster_opts + SearchTsrCache) under the
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
}			ToasterAttrCacheKey;

typedef struct ToasterAttrCacheEntry
{
	ToasterAttrCacheKey key;	/* HASH_BLOBS key */
	Oid			toasterid;		/* InvalidOid means "no toaster bound" */
	TsrRoutine	routine;		/* embedded copy; valid iff
								 * OidIsValid(toasterid) */
}			ToasterAttrCacheEntry;

static HTAB *toaster_attr_cache = NULL;

/*
 * Invalidation callback.  Walks the cache, dropping all entries that
 * match the given relid (or everything, when relid == InvalidOid).
 *
 * The HASH_REMOVE call can only fail if the entry we are iterating
 * over disappears between hash_seq_search() returning it and the
 * removal call.  That cannot happen in a single-threaded backend:
 * relcache invalidations are processed synchronously and no other
 * code path mutates this HTAB.  Treat any disappearance as a sign
 * of state corruption and raise ERROR rather than silently logging
 * a WARNING that the operator would likely miss.
 */
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
				elog(ERROR,
					 "toaster attr cache corrupted: missing entry at relid=%u attnum=%d",
					 entry->key.relid, (int) entry->key.attnum);
		}
	}
}

/*
 * One-time initialization of the cache HTAB and the relcache
 * invalidation callback.  Called from _PG_init so the callback is
 * registered before any backend can populate the cache; this avoids
 * a window where the cache would hold stale entries between a DDL
 * event and the next call to ToasterAttrCacheLookup.
 */
void
ToasterAttrCacheInit(void)
{
	HASHCTL		ctl;

	Assert(toaster_attr_cache == NULL);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(ToasterAttrCacheKey);
	ctl.entrysize = sizeof(ToasterAttrCacheEntry);
	ctl.hcxt = CacheMemoryContext;

	toaster_attr_cache = hash_create("toaster attr cache",
									 64, &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	CacheRegisterRelcacheCallback(toaster_attr_cache_invalidate, (Datum) 0);
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

	/*
	 * Defensive: if _PG_init has not run (e.g., the provider was not loaded
	 * via shared_preload_libraries), the cache is uninitialised and we cannot
	 * look anything up.  _PG_init rejects that case, but stay defensive
	 * against future call paths.
	 */
	if (toaster_attr_cache == NULL)
		return NULL;

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
		 * (pg_attribute semantics); our internal AttrNumber is 0-based by v1
		 * convention (see contrib/toastapi/README.toastapi).
		 */
		toasterid_str = attopts_get_toaster_opts(rel, attnum + 1,
												 ATT_TOASTER_NAME);

		if (toasterid_str != NULL)
		{
			Oid			toasterid = atoi(toasterid_str);

			if (OidIsValid(toasterid))
			{
				source = SearchTsrCache(toasterid);
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
