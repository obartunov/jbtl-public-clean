/*-------------------------------------------------------------------------
 *
 * jsonb_toaster_lite_subtree_refs.c
 *	Edge catalog for SUBTREE storage.
 *
 *	Schema (created by extension SQL):
 *	 jbtl_subtree_refs (
 *	 parent_toastrelid oid,
 *	 parent_valueid oid,
 *	 child_toastrelid oid,
 *	 child_valueid oid,
 *	 PRIMARY KEY (parent_toastrelid, parent_valueid,
 *	 child_toastrelid, child_valueid));
 *	 INDEX (child_toastrelid, child_valueid);
 *
 *	C API:
 *	 jbtl_subtree_refs_insert — add (parent, child) edge
 *	 jbtl_subtree_refs_delete_one — remove one edge; return
 *	 remaining refcount for child
 *	 jbtl_subtree_refs_child_orphan — true iff no edges remain
 *	 jbtl_subtree_refs_parent_id_in_use — used by allocator probe
 *	 jbtl_alloc_subtree_parent_valueid — Option 1 loop allocator
 *
 *	SQL-callable:
 *	 jbtl_subtree_refs_check — count dead edges (child gone)
 *	 jbtl_subtree_refs_gc — drop dead edges
 *
 *	Test helpers (used by regression tests to exercise the API
 *	without going through tsr_toast):
 *	 jbtl_test_alloc_parent_valueid
 *	 jbtl_test_refs_insert
 *	 jbtl_test_refs_delete_one
 *	 jbtl_test_refs_parent_id_in_use
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "commands/extension.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "jsonb_toaster_lite.h"

/* Bound on the synthetic-OID retry loop. */
#define LITE_OID_LOOP_MAX	1024

/*
 * Resolve the refs table OID and its two index OIDs.
 *
 *	Looks them up in the extension's installation schema (NOT
 *	hard-coded `public`), so the extension works correctly when
 *	installed via `CREATE EXTENSION ... SCHEMA myschema`.
 *
 *	No cache: we hit pg_extension + pg_namespace + pg_class on
 *	every call. The simple correctness cost-benefit (per @yoda
 *	pre-commit directive): correctness beats micro-optimization.
 *	A cache would need CacheRegisterRelcacheCallback to handle
 *	DROP+CREATE EXTENSION cycles in long-running backends; until
 *	that callback is wired up, the cache is a hazard, not a win.
 *	Lookup cost is one syscache hit per resolve, dwarfed by the
 *	actual catalog DML it precedes.
 */
static void
jbtl_refs_resolve_oids(Oid *out_relid, Oid *out_pkidx, Oid *out_childidx)
{
	Oid			ext_oid;
	Oid			ext_schema;
	Oid			relid;
	Oid			pkidx;
	Oid			childidx;

	ext_oid = get_extension_oid("jsonb_toaster_lite", false);
	ext_schema = get_extension_schema(ext_oid);

	relid = get_relname_relid("jbtl_subtree_refs", ext_schema);
	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("jsonb_toaster_lite: jbtl_subtree_refs not found"),
				 errhint("Did you CREATE EXTENSION jsonb_toaster_lite?")));

	pkidx = get_relname_relid("jbtl_subtree_refs_pkey", ext_schema);
	childidx = get_relname_relid("jbtl_subtree_refs_child_idx", ext_schema);
	if (!OidIsValid(pkidx) || !OidIsValid(childidx))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("jsonb_toaster_lite: refs indexes not found")));

	*out_relid = relid;
	*out_pkidx = pkidx;
	*out_childidx = childidx;
}

/*
 * Probe whether (parent_toastrelid, parent_valueid) already has any
 * edge in the refs table. Uses SnapshotDirty so in-progress edges
 * from concurrent transactions and from earlier statements in the
 * same transaction are visible (per spec invariant I-3 point 4).
 *
 * Uses the leading two columns of the PK index for efficient probe.
 */
bool jbtl_subtree_refs_parent_id_in_use(Oid parent_toastrelid,
										Oid parent_valueid);

bool
jbtl_subtree_refs_parent_id_in_use(Oid parent_toastrelid,
								   Oid parent_valueid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData skey[2];
	bool		found;
	SnapshotData snap;
	Oid			refs_relid;
	Oid			refs_pkidx;
	Oid			refs_childidx;

	jbtl_refs_resolve_oids(&refs_relid, &refs_pkidx, &refs_childidx);
	rel = table_open(refs_relid, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_jbtl_refs_parent_toastrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parent_toastrelid));
	ScanKeyInit(&skey[1],
				Anum_jbtl_refs_parent_valueid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parent_valueid));

	/*
	 * SnapshotDirty makes the probe see committed-and-in-progress
	 * rows. See spec I-3 point 4 (POST-COMMIT addendum): the probe
	 * is BEST-EFFORT. The PK constraint on jbtl_subtree_refs is
	 * the canonical enforcer of parent_valueid uniqueness; a
	 * concurrent allocator that races past this probe will hit a
	 * unique-violation in CatalogTupleInsert and abort txn.
	 */
	InitDirtySnapshot(snap);

	scan = systable_beginscan(rel, refs_pkidx, true,
							  &snap, 2, skey);
	found = (systable_getnext(scan) != NULL);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return found;
}

/*
 * Allocator: hand out a synthetic parent_valueid that's unique
 * within parent_toastrelid namespace, considering both real
 * chunk_ids in pg_toast.<rel> AND committed-or-in-progress
 * parent_valueids in jbtl_subtree_refs.
 *
 *	GetNewOidWithIndex on the toast index excludes real chunk_ids
 *	(clause a — guaranteed).
 *	jbtl_subtree_refs_parent_id_in_use excludes committed and
 *	in-progress parent_valueids (clause b — BEST EFFORT).
 *
 *	Race semantics (per @yoda pre-commit clarification):
 *
 *	 This loop is best-effort uniqueness. Two concurrent backends
 *	 can both see "cand=42 not in use" (SnapshotDirty probe) and
 *	 both proceed to refs_insert with parent_valueid=42. Whichever
 *	 CatalogTupleInsert commits first wins; the loser hits
 *	 ERRCODE_UNIQUE_VIOLATION on the refs PK and the loser's txn
 *	 aborts.
 *
 *	 The CANONICAL ENFORCER of parent_valueid uniqueness is the PK
 *	 constraint on jbtl_subtree_refs. This probe is an early-out
 *	 to avoid wasted work in the common (non-racing) case.
 *
 *	 does NOT retry on race. A user-facing INSERT that loses
 *	 the race observes a unique-violation error and rolls back.
 *	 This is acceptable; we may add advisory
 *	 locking on parent_toastrelid for the spill operation, or
 *	 implement allocator-level retry.
 *
 *	Bounded loop: LITE_OID_LOOP_MAX cap. Termination follows the
 *	standard GetNewOidWithIndex argument: toast OID space is
 *	sparse compared to the number of edges + chunks per relation.
 */
Oid jbtl_alloc_subtree_parent_valueid(Relation toastrel,
									  Relation toastidx);

Oid
jbtl_alloc_subtree_parent_valueid(Relation toastrel, Relation toastidx)
{
	int			attempts;
	Oid			toastrelid = RelationGetRelid(toastrel);

	for (attempts = 0; attempts < LITE_OID_LOOP_MAX; attempts++)
	{
		Oid			cand = GetNewOidWithIndex(toastrel,
											  RelationGetRelid(toastidx),
											  (AttrNumber) 1);

		/*
		 * Clause (a): cand is unique among real chunk_ids in
		 * pg_toast.<toastrel> — guaranteed by GetNewOidWithIndex
		 * against toastidx.
		 *
		 * Clause (b): cand is not already a parent_valueid in
		 * jbtl_subtree_refs for this parent_toastrelid. Probe
		 * with SnapshotDirty.
		 */
		if (!jbtl_subtree_refs_parent_id_in_use(toastrelid, cand))
			return cand;
	}

	ereport(ERROR,
			(errmsg("jsonb_toaster_lite: could not allocate unique "
					"parent_valueid for toastrelid=%u after %d attempts",
					toastrelid, attempts)));
	return InvalidOid;	/* keep compiler happy */
}

/*
 * Insert a (parent, child) edge. Caller has already allocated
 * parent_valueid via jbtl_alloc_subtree_parent_valueid for the
 * parent_toastrelid namespace. Failure (e.g. PK collision) ereports
 * — per spec I-3 point 2, edge insert failure aborts the txn.
 */
void jbtl_subtree_refs_insert(Oid parent_toastrelid,
							  Oid parent_valueid,
							  Oid child_toastrelid,
							  Oid child_valueid);

void
jbtl_subtree_refs_insert(Oid parent_toastrelid,
						 Oid parent_valueid,
						 Oid child_toastrelid,
						 Oid child_valueid)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_jbtl_subtree_refs];
	bool		nulls[Natts_jbtl_subtree_refs];
	Oid			refs_relid;
	Oid			refs_pkidx;
	Oid			refs_childidx;

	jbtl_refs_resolve_oids(&refs_relid, &refs_pkidx, &refs_childidx);
	rel = table_open(refs_relid, RowExclusiveLock);

	memset(nulls, false, sizeof(nulls));
	values[Anum_jbtl_refs_parent_toastrelid - 1] = ObjectIdGetDatum(parent_toastrelid);
	values[Anum_jbtl_refs_parent_valueid - 1] = ObjectIdGetDatum(parent_valueid);
	values[Anum_jbtl_refs_child_toastrelid - 1] = ObjectIdGetDatum(child_toastrelid);
	values[Anum_jbtl_refs_child_valueid - 1] = ObjectIdGetDatum(child_valueid);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/*
	 * CatalogTupleInsert handles index updates and PK-violation
	 * ereport. Hard invariant: any failure here aborts txn.
	 */
	CatalogTupleInsert(rel, tup);

	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

/*
 * Delete one (parent, child) edge. Returns remaining refcount for
 * the child (number of OTHER parents still referencing this child).
 * Caller uses the return value to decide whether to physically
 * delete the child toast chain:
 *	 if remaining == 0: jbtl_toast_delete_datum(child_external)
 *
 * If the row to delete is not found, ereport — production code paths
 * MUST have inserted the edge before reaching delete; absence
 * indicates state corruption worth surfacing.
 */
int jbtl_subtree_refs_delete_one(Oid parent_toastrelid,
								 Oid parent_valueid,
								 Oid child_toastrelid,
								 Oid child_valueid);

int
jbtl_subtree_refs_delete_one(Oid parent_toastrelid,
							 Oid parent_valueid,
							 Oid child_toastrelid,
							 Oid child_valueid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData skey[4];
	HeapTuple	tup;
	bool		found = false;
	int			remaining;
	Oid			refs_relid;
	Oid			refs_pkidx;
	Oid			refs_childidx;

	jbtl_refs_resolve_oids(&refs_relid, &refs_pkidx, &refs_childidx);
	rel = table_open(refs_relid, RowExclusiveLock);

	ScanKeyInit(&skey[0],
				Anum_jbtl_refs_parent_toastrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parent_toastrelid));
	ScanKeyInit(&skey[1],
				Anum_jbtl_refs_parent_valueid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parent_valueid));
	ScanKeyInit(&skey[2],
				Anum_jbtl_refs_child_toastrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(child_toastrelid));
	ScanKeyInit(&skey[3],
				Anum_jbtl_refs_child_valueid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(child_valueid));

	scan = systable_beginscan(rel, refs_pkidx, true,
							  NULL, 4, skey);
	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		CatalogTupleDelete(rel, &tup->t_self);
		found = true;
	}
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("jsonb_toaster_lite: edge not found "
						"(parent=%u/%u child=%u/%u)",
						parent_toastrelid, parent_valueid,
						child_toastrelid, child_valueid)));

	/*
	 * Count remaining edges to this child. Use SnapshotSelf so the
	 * scan sees the effect of CatalogTupleDelete above (the row is
	 * tombstoned with xmax = current xid; SnapshotSelf treats own-
	 * txn writes as visible-and-applied, MVCC does not). Without
	 * this, the count includes the just-deleted row and we'd
	 * incorrectly leak the child chain.
	 */
	rel = table_open(refs_relid, AccessShareLock);
	ScanKeyInit(&skey[0],
				Anum_jbtl_refs_child_toastrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(child_toastrelid));
	ScanKeyInit(&skey[1],
				Anum_jbtl_refs_child_valueid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(child_valueid));

	scan = systable_beginscan(rel, refs_childidx, true,
							  SnapshotSelf, 2, skey);
	remaining = 0;
	while ((tup = systable_getnext(scan)) != NULL)
		remaining++;
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return remaining;
}

/*
 * Returns true iff no edges remain referencing (child_toastrelid,
 * child_valueid). Convenience wrapper over the inverse-index
 * scan.
 */
bool jbtl_subtree_refs_child_orphan(Oid child_toastrelid,
									Oid child_valueid);

bool
jbtl_subtree_refs_child_orphan(Oid child_toastrelid, Oid child_valueid)
{
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData skey[2];
	bool		orphan;
	Oid			refs_relid;
	Oid			refs_pkidx;
	Oid			refs_childidx;

	jbtl_refs_resolve_oids(&refs_relid, &refs_pkidx, &refs_childidx);
	rel = table_open(refs_relid, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_jbtl_refs_child_toastrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(child_toastrelid));
	ScanKeyInit(&skey[1],
				Anum_jbtl_refs_child_valueid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(child_valueid));

	scan = systable_beginscan(rel, refs_childidx, true,
							  NULL, 2, skey);
	orphan = (systable_getnext(scan) == NULL);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return orphan;
}

/* =======================================================================
 *	SQL-callable: check / gc.
 * ======================================================================= */

/*
 * Returns count of edges whose child chunk_id no longer exists in
 * pg_toast.<child_toastrelid>. Probes via syscache miss / catalog
 * scan; a simple per-row check suffices since
 * refs cardinality is small in tests.
 */
PG_FUNCTION_INFO_V1(jbtl_subtree_refs_check);
Datum
jbtl_subtree_refs_check(PG_FUNCTION_ARGS)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	int			dead = 0;
	Oid			refs_relid;
	Oid			refs_pkidx;
	Oid			refs_childidx;

	jbtl_refs_resolve_oids(&refs_relid, &refs_pkidx, &refs_childidx);
	rel = table_open(refs_relid, AccessShareLock);

	scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Datum		v[Natts_jbtl_subtree_refs];
		bool		isnull[Natts_jbtl_subtree_refs];
		Oid			child_relid;
		Oid			child_vid;
		Relation	toastrel;
		ScanKeyData tk;
		SysScanDesc tscan;
		HeapTuple	ttup;
		bool		found;

		heap_deform_tuple(tup, RelationGetDescr(rel), v, isnull);
		child_relid = DatumGetObjectId(v[Anum_jbtl_refs_child_toastrelid - 1]);
		child_vid = DatumGetObjectId(v[Anum_jbtl_refs_child_valueid - 1]);

		/*
		 * Open the child's toast relation and probe its primary
		 * index for chunk_id == child_vid. Skip if relation gone
		 * (counts as dead edge).
		 */
		toastrel = try_table_open(child_relid, AccessShareLock);
		if (toastrel == NULL)
		{
			dead++;
			continue;
		}

		{
			Relation	toastidx;
			List	   *idxlist = RelationGetIndexList(toastrel);

			if (idxlist == NIL)
			{
				table_close(toastrel, AccessShareLock);
				dead++;
				continue;
			}
			toastidx = index_open(linitial_oid(idxlist), AccessShareLock);

			ScanKeyInit(&tk, (AttrNumber) 1,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(child_vid));
			tscan = systable_beginscan(toastrel, RelationGetRelid(toastidx),
									   true, NULL, 1, &tk);
			ttup = systable_getnext(tscan);
			found = HeapTupleIsValid(ttup);
			systable_endscan(tscan);
			index_close(toastidx, AccessShareLock);
			list_free(idxlist);
		}

		table_close(toastrel, AccessShareLock);

		if (!found)
			dead++;
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	PG_RETURN_INT32(dead);
}

/*
 * Drop dead edges; returns count removed.
 */
PG_FUNCTION_INFO_V1(jbtl_subtree_refs_gc);
Datum
jbtl_subtree_refs_gc(PG_FUNCTION_ARGS)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	int			removed = 0;
	List	   *to_delete = NIL;
	ListCell   *lc;
	Oid			refs_relid;
	Oid			refs_pkidx;
	Oid			refs_childidx;

	jbtl_refs_resolve_oids(&refs_relid, &refs_pkidx, &refs_childidx);
	rel = table_open(refs_relid, RowExclusiveLock);

	/* Pass 1: collect TIDs of dead edges (avoid concurrent deletion
	 * issues during scan). */
	scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Datum		v[Natts_jbtl_subtree_refs];
		bool		isnull[Natts_jbtl_subtree_refs];
		Oid			child_relid;
		Oid			child_vid;
		Relation	toastrel;
		bool		dead = false;

		heap_deform_tuple(tup, RelationGetDescr(rel), v, isnull);
		child_relid = DatumGetObjectId(v[Anum_jbtl_refs_child_toastrelid - 1]);
		child_vid = DatumGetObjectId(v[Anum_jbtl_refs_child_valueid - 1]);

		toastrel = try_table_open(child_relid, AccessShareLock);
		if (toastrel == NULL)
		{
			dead = true;
		}
		else
		{
			Relation	toastidx;
			List	   *idxlist = RelationGetIndexList(toastrel);
			ScanKeyData tk;
			SysScanDesc tscan;
			HeapTuple	ttup;

			if (idxlist == NIL)
			{
				dead = true;
			}
			else
			{
				toastidx = index_open(linitial_oid(idxlist), AccessShareLock);
				ScanKeyInit(&tk, (AttrNumber) 1,
							BTEqualStrategyNumber, F_OIDEQ,
							ObjectIdGetDatum(child_vid));
				tscan = systable_beginscan(toastrel,
										   RelationGetRelid(toastidx),
										   true, NULL, 1, &tk);
				ttup = systable_getnext(tscan);
				if (!HeapTupleIsValid(ttup))
					dead = true;
				systable_endscan(tscan);
				index_close(toastidx, AccessShareLock);
			}
			list_free(idxlist);
			table_close(toastrel, AccessShareLock);
		}

		if (dead)
		{
			ItemPointer tid = palloc(sizeof(ItemPointerData));

			ItemPointerCopy(&tup->t_self, tid);
			to_delete = lappend(to_delete, tid);
		}
	}
	table_endscan(scan);

	/* Pass 2: delete collected dead edges. */
	foreach(lc, to_delete)
	{
		ItemPointer tid = (ItemPointer) lfirst(lc);

		simple_heap_delete(rel, tid);
		removed++;
	}
	list_free_deep(to_delete);

	table_close(rel, RowExclusiveLock);

	PG_RETURN_INT32(removed);
}

/* =======================================================================
 *	 test helpers. These expose the internal API to SQL so
 *	acceptance pins can exercise it without going through tsr_toast
 *	(which doesn't yet emit edges; that's ).
 * ======================================================================= */

/*
 * jbtl_test_alloc_parent_valueid(rel regclass) → oid
 *	Open the relation's toast relation + first index, run the
 *	production allocator, return the synthetic OID.
 */
PG_FUNCTION_INFO_V1(jbtl_test_alloc_parent_valueid);
Datum
jbtl_test_alloc_parent_valueid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	Oid			toastrelid;
	Relation	toastrel;
	Relation	toastidx;
	List	   *idxlist;
	Oid			result;

	rel = table_open(relid, AccessShareLock);
	toastrelid = rel->rd_rel->reltoastrelid;
	table_close(rel, AccessShareLock);

	if (!OidIsValid(toastrelid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("relation has no toast table")));

	toastrel = table_open(toastrelid, RowExclusiveLock);
	idxlist = RelationGetIndexList(toastrel);
	if (idxlist == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("toast relation has no index")));
	toastidx = index_open(linitial_oid(idxlist), RowExclusiveLock);

	result = jbtl_alloc_subtree_parent_valueid(toastrel, toastidx);

	index_close(toastidx, RowExclusiveLock);
	table_close(toastrel, RowExclusiveLock);
	list_free(idxlist);

	PG_RETURN_OID(result);
}

PG_FUNCTION_INFO_V1(jbtl_test_refs_insert);
Datum
jbtl_test_refs_insert(PG_FUNCTION_ARGS)
{
	jbtl_subtree_refs_insert(PG_GETARG_OID(0), PG_GETARG_OID(1),
							 PG_GETARG_OID(2), PG_GETARG_OID(3));
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(jbtl_test_refs_delete_one);
Datum
jbtl_test_refs_delete_one(PG_FUNCTION_ARGS)
{
	int rem = jbtl_subtree_refs_delete_one(
		PG_GETARG_OID(0), PG_GETARG_OID(1),
		PG_GETARG_OID(2), PG_GETARG_OID(3));
	PG_RETURN_INT32(rem);
}

PG_FUNCTION_INFO_V1(jbtl_test_refs_parent_id_in_use);
Datum
jbtl_test_refs_parent_id_in_use(PG_FUNCTION_ARGS)
{
	bool used = jbtl_subtree_refs_parent_id_in_use(PG_GETARG_OID(0),
												   PG_GETARG_OID(1));
	PG_RETURN_BOOL(used);
}

PG_FUNCTION_INFO_V1(jbtl_test_refs_child_orphan);
Datum
jbtl_test_refs_child_orphan(PG_FUNCTION_ARGS)
{
	bool orphan = jbtl_subtree_refs_child_orphan(PG_GETARG_OID(0),
												 PG_GETARG_OID(1));
	PG_RETURN_BOOL(orphan);
}
