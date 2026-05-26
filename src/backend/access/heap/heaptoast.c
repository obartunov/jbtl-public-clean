/*-------------------------------------------------------------------------
 *
 * heaptoast.c
 *	  Heap-specific definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heaptoast.c
 *
 *
 * INTERFACE ROUTINES
 *		heap_toast_insert_or_update -
 *			Try to make a given tuple fit into one page by compressing
 *			or moving off attributes
 *
 *		heap_toast_delete -
 *			Reclaim toast storage when a tuple is deleted
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/tableam.h"
#include "access/toast_helper.h"
#include "access/toast_hook.h"
#include "access/toast_internals.h"
#include "access/toasterapi.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "utils/fmgroids.h"
#include "utils/snapmgr.h"
#include "access/typelifecycle.h"


/* ----------
 * heap_toast_delete -
 *
 *	Cascaded delete toast-entries on DELETE
 * ----------
 */
void
heap_toast_delete(Relation rel, HeapTuple oldtup, bool is_speculative)
{
	TupleDesc	tupleDesc;
	Datum		toast_values[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];

	/*
	 * We should only ever be called for tuples of plain relations or
	 * materialized views --- recursing on a toast rel is bad news.
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * Get the tuple descriptor and break down the tuple into fields.
	 *
	 * NOTE: it's debatable whether to use heap_deform_tuple() here or just
	 * heap_getattr() only the varlena columns.  The latter could win if there
	 * are few varlena columns and many non-varlena ones. However,
	 * heap_deform_tuple costs only O(N) while the heap_getattr way would cost
	 * O(N^2) if there are many varlena columns, so it seems better to err on
	 * the side of linear cost.  (We won't even be here unless there's at
	 * least one varlena column, by the way.)
	 */
	tupleDesc = rel->rd_att;

	Assert(tupleDesc->natts <= MaxHeapAttributeNumber);
	heap_deform_tuple(oldtup, tupleDesc, toast_values, toast_isnull);

	/* Do the real work. */
	toast_delete_external(rel, toast_values, toast_isnull, is_speculative);

	/*
	 * W2.3a: stock toast_delete_external only frees attribute-level external
	 * datums.  A split parent (a value of a type with a registered lifecycle
	 * routine; currently jsonb) is physically inline, so its nested cold payload
	 * (ordinary TOAST values referenced by type-owned descriptors) is invisible
	 * to that pass.  Ask each such attribute's routine for the embedded refs and
	 * delete them here, on the same execution-time path as the stock deletion,
	 * using the stock toast_delete_datum so MVCC/visibility of the child TOAST
	 * values follows ordinary rules.
	 */
	for (int i = 0; i < tupleDesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		const TypeLifecycleRoutine *routine;
		List	   *refs;
		ListCell   *lc;

		if (toast_isnull[i] || att->attlen != -1)
			continue;
		routine = lookup_type_lifecycle_routine(att->atttypid);
		if (routine == NULL || routine->collect_external_refs == NULL)
			continue;
		/* generic guard: an external parent is not an inline split parent */
		if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
			continue;
		if (routine->has_external_refs == NULL ||
			!routine->has_external_refs(toast_values[i]))
			continue;

		refs = routine->collect_external_refs(toast_values[i]);
		foreach(lc, refs)
		{
			struct varlena *ref = (struct varlena *) lfirst(lc);

			toast_delete_datum(rel, PointerGetDatum(ref), is_speculative);
			pfree(ref);
		}
		list_free(refs);
	}
}

/*
 * HeapTupleHasNestedExternal
 *
 * W2.3a delete gate.  Cheap check: deform only varlena attributes of a type
 * with a registered lifecycle routine (currently jsonb) and ask the routine's
 * O(top-level) has_external_refs predicate whether any carries nested
 * descriptors.  Returns false fast for the common (non-split) case.
 */
bool
HeapTupleHasNestedExternal(Relation rel, HeapTuple tup)
{
	TupleDesc	tupleDesc = rel->rd_att;
	int			numAttrs = tupleDesc->natts;

	for (int i = 0; i < numAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		Datum		val;
		bool		isnull;
		const TypeLifecycleRoutine *routine;

		if (att->attlen != -1)
			continue;
		routine = lookup_type_lifecycle_routine(att->atttypid);
		if (routine == NULL || routine->has_external_refs == NULL)
			continue;

		val = heap_getattr(tup, i + 1, tupleDesc, &isnull);
		if (isnull)
			continue;
		if (VARATT_IS_EXTERNAL(DatumGetPointer(val)))
			continue;			/* external parent is not a split parent */
		if (routine->has_external_refs(val))
			return true;
	}
	return false;
}

/*
 * heap_toast_update_nested_cleanup
 *
 * W2.3b ordinary-UPDATE nested lifecycle.  Called from the UPDATE path of
 * heap_toast_insert_or_update (oldtup != NULL), after the new tuple is formed
 * and while both old and new attribute values are visible in the ttc.
 *
 * For each lifecycle-managed attribute (currently jsonb) that carries split
 * cold payload in the old and/or new value, compute the nested external refs
 * of each side and
 * delete (old \ new) by va_valueid through the stock toast_delete_datum.  This
 * is exactly the lifecycle the stock attribute-level path performs for changed
 * external datums (toast_helper.c TOASTCOL_NEEDS_DELETE_OLD), but for our inline
 * split parent the stock path does not see the nested descriptors, so we do it
 * here on the same execution-time path with the same stock deletion primitive.
 *
 * MVCC: toast_delete_datum uses simple_heap_delete, an MVCC delete (sets xmax);
 * the old child chunks remain visible to any snapshot that can still see the old
 * heap version and are physically reclaimed by ordinary toast-rel autovacuum
 * once no snapshot needs them.  Rollback restores them with the old heap row.
 *
 * old \ new only: refs present on both sides (reuse, same valueid) are kept;
 * refs new-only are freshly created payload and kept.  No reuse is *created*
 * here -- without W2.4 the new producer emits fresh valueids, so in practice
 * the intersection is empty and all old children are deleted, which is the
 * correct no-orphan behaviour.
 */
static void
heap_toast_update_nested_cleanup(ToastTupleContext *ttc)
{
	Relation	rel = ttc->ttc_rel;
	TupleDesc	tupleDesc = rel->rd_att;
	int			numAttrs = tupleDesc->natts;

	/* UPDATE only: caller guarantees ttc_oldvalues != NULL */
	for (int i = 0; i < numAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		Datum		oldval,
					newval;
		List	   *old_refs,
				   *new_refs;
		ListCell   *lc;
		const TypeLifecycleRoutine *routine;

		if (att->attlen != -1)
			continue;
		routine = lookup_type_lifecycle_routine(att->atttypid);
		if (routine == NULL || routine->collect_external_refs == NULL ||
			routine->has_external_refs == NULL)
			continue;
		if (ttc->ttc_oldisnull[i])
			continue;

		oldval = ttc->ttc_oldvalues[i];
		if (VARATT_IS_EXTERNAL(DatumGetPointer(oldval)))
			continue;			/* external parent is not a split parent */
		if (!routine->has_external_refs(oldval))
			continue;			/* old side carries no nested cold payload */

		old_refs = routine->collect_external_refs(oldval);

		/*
		 * Collect new-side refs only when the new value is a non-null,
		 * non-external split parent; otherwise new_refs stays NIL and every old
		 * ref is deleted (covers metadata UPDATE with fresh valueids, payload
		 * replace, and split -> non-split fallback).
		 */
		new_refs = NIL;
		if (!ttc->ttc_isnull[i])
		{
			newval = ttc->ttc_values[i];
			if (!VARATT_IS_EXTERNAL(DatumGetPointer(newval)) &&
				routine->has_external_refs(newval))
				new_refs = routine->collect_external_refs(newval);
		}

		/* Delete old \ new, comparing by va_valueid within this toast rel. */
		foreach(lc, old_refs)
		{
			struct varlena *oref = (struct varlena *) lfirst(lc);
			struct varatt_external oext;
			bool		kept = false;
			ListCell   *lc2;

			VARATT_EXTERNAL_GET_POINTER(oext, oref);

			foreach(lc2, new_refs)
			{
				struct varlena *nref = (struct varlena *) lfirst(lc2);
				struct varatt_external next;

				VARATT_EXTERNAL_GET_POINTER(next, nref);
				if (next.va_valueid == oext.va_valueid &&
					next.va_toastrelid == oext.va_toastrelid)
				{
					kept = true;	/* reused: present on both sides */
					break;
				}
			}

			if (!kept)
				toast_delete_datum(rel, PointerGetDatum(oref), false);
			pfree(oref);
		}
		list_free(old_refs);

		foreach(lc, new_refs)
			pfree(lfirst(lc));
		list_free(new_refs);
	}
}

/*
 * heap_check_no_split_values_for_rewrite
 *
 * See header comment.  Implementation: a plain MVCC table scan of live rows.
 * On the first row whose attribute (of a type with a registered lifecycle
 * routine; currently jsonb) reports embedded split refs, raise.  The
 * caller (cluster_rel, before rebuild_relation) already holds
 * AccessExclusiveLock, so no new split row can slip in between this check and
 * the rewrite.
 */
void
heap_check_no_split_values_for_rewrite(Relation rel)
{
	TupleDesc	tupleDesc = RelationGetDescr(rel);
	bool		has_split_type = false;
	TableScanDesc scan;
	TupleTableSlot *slot;
	Snapshot	snapshot;

	/* Fast out: nothing to do unless the relation has a lifecycle-managed column. */
	for (int i = 0; i < tupleDesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);

		if (att->attlen == -1 &&
			lookup_type_lifecycle_routine(att->atttypid) != NULL)
		{
			has_split_type = true;
			break;
		}
	}
	if (!has_split_type)
		return;

	snapshot = GetActiveSnapshot();
	slot = table_slot_create(rel, NULL);
	scan = table_beginscan(rel, snapshot, 0, NULL, 0);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		for (int i = 0; i < tupleDesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
			Datum		val;
			bool		isnull;
			const TypeLifecycleRoutine *routine;

			if (att->attlen != -1)
				continue;
			routine = lookup_type_lifecycle_routine(att->atttypid);
			if (routine == NULL || routine->has_external_refs == NULL)
				continue;

			val = slot_getattr(slot, i + 1, &isnull);
			if (isnull)
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(val)))
				continue;
			if (routine->has_external_refs(val))
			{
				/* Clean up scan state before erroring. */
				table_endscan(scan);
				ExecDropSingleTupleTableSlot(slot);
				ereport(ERROR,
						errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("cannot rewrite table \"%s\" containing split values that block rewrite",
							   RelationGetRelationName(rel)),
						errdetail("Heap rewrite (VACUUM FULL / CLUSTER / REPACK) would renumber TOAST chunks without updating the nested descriptors embedded in inline split parents, orphaning the cold payload."),
						errhint("Rewrite is not supported while the table holds split rows. Re-store the affected rows so their large top-level values are no longer relocated out of line (set the column STORAGE to PLAIN or MAIN, or reduce the oversized values below the relocation threshold), then retry."));
			}
		}
	}

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
}

/*
 * heap_compute_data_size_without_attr
 * Calculate data size if attribite attno is minimal.
 */
static int
heap_compute_data_size_without_attr(TupleDesc tupleDesc,
									Datum *toast_values,
									bool *toast_isnull, int attno)
{
	/* 8 bytes required to fit varattrib_4b */
	char		tmp[8] = {0};	/* silence compiler warning */

	int			size;
	Datum	   *pvalue = &toast_values[attno];
	Datum		old_value = *pvalue;

	*pvalue = PointerGetDatum(&tmp);
	SET_VARSIZE(DatumGetPointer(*pvalue), VARHDRSZ);//sizeof(struct varlena));

	size = heap_compute_data_size(tupleDesc, toast_values, toast_isnull);

	*pvalue = old_value;

	return size;
}

/*
 * heap_toast_tuple_externalize
 * Externalize attribute attno to fit maxDataLen. Custom toaster could use
 * different strategies and keep part of value in toaster pointer
 */
static void
heap_toast_tuple_externalize(ToastTupleContext *ttc, int attno,
							 int maxDataLen)
{
	int		max_inline_size = 0;

	/*
	 * Only reserve inline room for a CUSTOM pointer if THIS column has
	 * a toaster bound to it.  Previously the heuristic keyed off "any
	 * resolver hook installed", which gave every column on a database
	 * with any loaded provider a smaller inline budget — penalising
	 * non-CUSTOM columns whenever a provider was loaded.  Resolve the
	 * column-specific toaster id via the rel-hook; only allocate the
	 * inline budget when the id is valid.
	 *
	 * Note: the rel hook receives attno in the 0-based convention
	 * established by the v1 dispatch helpers (dispatch_toaster_*).
	 * AttrNumber is used as the wire type but values stay 0-based for
	 * compatibility with the provider's internal cache lookup.
	 */
	if (get_toaster_id_for_rel_hook != NULL)
	{
		Oid			toasterid;

		toasterid = get_toaster_id_for_rel_hook(ttc->ttc_rel,
												(AttrNumber) attno);
		if (OidIsValid(toasterid))
		{
			int		size = heap_compute_data_size_without_attr(ttc->ttc_rel->rd_att,
															   ttc->ttc_values,
															   ttc->ttc_isnull,
															   attno);

			max_inline_size = Max(0, maxDataLen - size);
		}
	}

	toast_tuple_externalize(ttc, attno, max_inline_size, ttc->ttc_am_options);
}

/* ----------
 * heap_toast_insert_or_update -
 *
 *	Delete no-longer-used toast-entries and create new ones to
 *	make the new tuple fit on INSERT or UPDATE
 *
 * Inputs:
 *	newtup: the candidate new tuple to be inserted
 *	oldtup: the old row version for UPDATE, or NULL for INSERT
 *	options: options to be passed to heap_insert() for toast rows
 * Result:
 *	either newtup if no toasting is needed, or a palloc'd modified tuple
 *	that is what should actually get stored
 *
 * NOTE: neither newtup nor oldtup will be modified.  This is a change
 * from the pre-8.1 API of this routine.
 * ----------
 */
HeapTuple
heap_toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup,
							uint32 options)
{
	HeapTuple	result_tuple;
	TupleDesc	tupleDesc;
	int			numAttrs;

	Size		maxDataLen;
	Size		hoff;

	bool		toast_isnull[MaxHeapAttributeNumber];
	bool		toast_oldisnull[MaxHeapAttributeNumber];
	Datum		toast_values[MaxHeapAttributeNumber];
	Datum		toast_oldvalues[MaxHeapAttributeNumber];
	ToastAttrInfo toast_attr[MaxHeapAttributeNumber];
	ToastTupleContext ttc;

	/*
	 * Ignore the INSERT_SPECULATIVE option. Speculative insertions/super
	 * deletions just normally insert/delete the toast values. It seems
	 * easiest to deal with that here, instead on, potentially, multiple
	 * callers.
	 */
	options &= ~HEAP_INSERT_SPECULATIVE;

	/*
	 * We should only ever be called for tuples of plain relations or
	 * materialized views --- recursing on a toast rel is bad news.
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * Get the tuple descriptor and break down the tuple(s) into fields.
	 */
	tupleDesc = rel->rd_att;
	numAttrs = tupleDesc->natts;

	Assert(numAttrs <= MaxHeapAttributeNumber);
	heap_deform_tuple(newtup, tupleDesc, toast_values, toast_isnull);
	if (oldtup != NULL)
		heap_deform_tuple(oldtup, tupleDesc, toast_oldvalues, toast_oldisnull);

	/* ----------
	 * Prepare for toasting
	 * ----------
	 */
	ttc.ttc_rel = rel;
	ttc.ttc_values = toast_values;
	ttc.ttc_isnull = toast_isnull;
	if (oldtup == NULL)
	{
		ttc.ttc_oldvalues = NULL;
		ttc.ttc_oldisnull = NULL;
	}
	else
	{
		ttc.ttc_oldvalues = toast_oldvalues;
		ttc.ttc_oldisnull = toast_oldisnull;
	}
	ttc.ttc_attr = toast_attr;
	ttc.ttc_am_options = options;
	toast_tuple_init(&ttc);

	/* ----------
	 * Compress and/or save external until data fits into target length
	 *
	 *	1: Inline compress attributes with attstorage EXTENDED, and store very
	 *	   large attributes with attstorage EXTENDED or EXTERNAL external
	 *	   immediately
	 *	2: Store attributes with attstorage EXTENDED or EXTERNAL external
	 *	3: Inline compress attributes with attstorage MAIN
	 *	4: Store attributes with attstorage MAIN external
	 * ----------
	 */

	/* compute header overhead --- this should match heap_form_tuple() */
	hoff = SizeofHeapTupleHeader;
	if ((ttc.ttc_flags & TOAST_HAS_NULLS) != 0)
		hoff += BITMAPLEN(numAttrs);
	hoff = MAXALIGN(hoff);
	/* now convert to a limit on the tuple data size */
	maxDataLen = RelationGetToastTupleTarget(rel, TOAST_TUPLE_TARGET) - hoff;

	/* ----------
	 * W2.2 cold-payload pre-pass (create only) for lifecycle-managed types
	 * (currently jsonb).
	 *
	 * Before the ordinary compress/externalize loop, give each large
	 * lifecycle-managed attribute a chance (via toast_or_split) to move its
	 * large payload out of line as ordinary TOAST values, leaving a small parent
	 * (warm values + type-owned descriptors).  This runs here -- after
	 * toast_tuple_init (which zeroes ttc_flags and records tai_size / colflags)
	 * but before any compression or externalization -- so the split sees the raw
	 * structure and so the loop below sees the already-small parent and need
	 * not externalize it as a whole.
	 *
	 * We touch only attributes that would be toasted anyway (tai_size >
	 * maxDataLen); the split is otherwise a no-op.  When a value is replaced we
	 * must refresh tai_size (find_biggest_attribute reads it) and raise
	 * TOAST_NEEDS_CHANGE ourselves, since the loop may now leave the small
	 * parent untouched and the tuple is rebuilt from toast_values[] only when
	 * that flag is set.
	 * ----------
	 */
	if (rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			i;

		for (i = 0; i < numAttrs; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
			bool		did_split;
			Datum		newval;
			Datum		oldval = (Datum) 0;
			bool		old_isnull = true;
			const TypeLifecycleRoutine *routine;
			TypeLifecycleContext lctx;

			if ((toast_attr[i].tai_colflags & TOASTCOL_IGNORE) != 0)
				continue;		/* NULL / PLAIN / non-varlena / reused */
			if (att->attlen != -1)
				continue;
			routine = lookup_type_lifecycle_routine(att->atttypid);
			if (routine == NULL || routine->toast_or_split == NULL)
				continue;

			if (toast_attr[i].tai_size <= maxDataLen)
				continue;		/* would not be toasted; nothing to gain */

			/* W2.4: on UPDATE, pass the old value so unchanged cold children
			 * can be reused (key-based, byte-exact) instead of re-saved. */
			if (oldtup != NULL)
			{
				oldval = toast_oldvalues[i];
				old_isnull = toast_oldisnull[i];
			}

			lctx.rel = rel;
			lctx.attnum = i + 1;
			lctx.options = options;
			lctx.max_inline_size = maxDataLen;
			newval = routine->toast_or_split(toast_values[i],
											 old_isnull ? (Datum) 0 : oldval,
											 &lctx);
			/* did_split: routine returned a different (rewritten) value */
			did_split = (DatumGetPointer(newval) != DatumGetPointer(toast_values[i]));
			if (did_split)
			{
				toast_values[i] = newval;
				toast_attr[i].tai_size = VARSIZE_ANY(DatumGetPointer(newval));
				ttc.ttc_flags |= TOAST_NEEDS_CHANGE;
			}
		}
	}

	/*
	 * Look for attributes with attstorage EXTENDED to compress.  Also find
	 * large attributes with attstorage EXTENDED or EXTERNAL, and store them
	 * external.
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, true, false);
		if (biggest_attno < 0)
			break;

		/*
		 * Attempt to compress it inline, if it has attstorage EXTENDED
		 */
		if (TupleDescAttr(tupleDesc, biggest_attno)->attstorage == TYPSTORAGE_EXTENDED)
			toast_tuple_try_compression(&ttc, biggest_attno);
		else
		{
			/*
			 * has attstorage EXTERNAL, ignore on subsequent compression
			 * passes
			 */
			toast_attr[biggest_attno].tai_colflags |= TOASTCOL_INCOMPRESSIBLE;
		}

		/*
		 * If this value is by itself more than maxDataLen (after compression
		 * if any), push it out to the toast table immediately, if possible.
		 * This avoids uselessly compressing other fields in the common case
		 * where we have one long field and several short ones.
		 *
		 * XXX maybe the threshold should be less than maxDataLen?
		 */
		if (toast_attr[biggest_attno].tai_size > maxDataLen &&
			rel->rd_rel->reltoastrelid != InvalidOid)
			heap_toast_tuple_externalize(&ttc, biggest_attno, maxDataLen);
	}

	/*
	 * Second we look for attributes of attstorage EXTENDED or EXTERNAL that
	 * are still inline, and make them external.  But skip this if there's no
	 * toast table to push them to.
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, false, false);
		if (biggest_attno < 0)
			break;
		heap_toast_tuple_externalize(&ttc, biggest_attno, maxDataLen);
	}

	/*
	 * Round 3 - this time we take attributes with storage MAIN into
	 * compression
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, true, true);
		if (biggest_attno < 0)
			break;

		toast_tuple_try_compression(&ttc, biggest_attno);
	}

	/*
	 * Finally we store attributes of type MAIN externally.  At this point we
	 * increase the target tuple size, so that MAIN attributes aren't stored
	 * externally unless really necessary.
	 */
	maxDataLen = TOAST_TUPLE_TARGET_MAIN - hoff;

	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, false, true);
		if (biggest_attno < 0)
			break;

		heap_toast_tuple_externalize(&ttc, biggest_attno, maxDataLen);
	}

	/*
	 * In the case we toasted any values, we need to build a new heap tuple
	 * with the changed values.
	 */
	if ((ttc.ttc_flags & TOAST_NEEDS_CHANGE) != 0)
	{
		HeapTupleHeader olddata = newtup->t_data;
		HeapTupleHeader new_data;
		int32		new_header_len;
		int32		new_data_len;
		int32		new_tuple_len;

		/*
		 * Calculate the new size of the tuple.
		 *
		 * Note: we used to assume here that the old tuple's t_hoff must equal
		 * the new_header_len value, but that was incorrect.  The old tuple
		 * might have a smaller-than-current natts, if there's been an ALTER
		 * TABLE ADD COLUMN since it was stored; and that would lead to a
		 * different conclusion about the size of the null bitmap, or even
		 * whether there needs to be one at all.
		 */
		new_header_len = SizeofHeapTupleHeader;
		if ((ttc.ttc_flags & TOAST_HAS_NULLS) != 0)
			new_header_len += BITMAPLEN(numAttrs);
		new_header_len = MAXALIGN(new_header_len);
		new_data_len = heap_compute_data_size(tupleDesc,
											  toast_values, toast_isnull);
		new_tuple_len = new_header_len + new_data_len;

		/*
		 * Allocate and zero the space needed, and fill HeapTupleData fields.
		 */
		result_tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + new_tuple_len);
		result_tuple->t_len = new_tuple_len;
		result_tuple->t_self = newtup->t_self;
		result_tuple->t_tableOid = newtup->t_tableOid;
		new_data = (HeapTupleHeader) ((char *) result_tuple + HEAPTUPLESIZE);
		result_tuple->t_data = new_data;

		/*
		 * Copy the existing tuple header, but adjust natts and t_hoff.
		 */
		memcpy(new_data, olddata, SizeofHeapTupleHeader);
		HeapTupleHeaderSetNatts(new_data, numAttrs);
		new_data->t_hoff = new_header_len;

		/* Copy over the data, and fill the null bitmap if needed */
		heap_fill_tuple(tupleDesc,
						toast_values,
						toast_isnull,
						(char *) new_data + new_header_len,
						new_data_len,
						&(new_data->t_infomask),
						((ttc.ttc_flags & TOAST_HAS_NULLS) != 0) ?
						new_data->t_bits : NULL);
	}
	else
		result_tuple = newtup;

	/*
	 * W2.3b: on UPDATE, delete nested cold payload that the old split parent
	 * referenced but the new one does not (old \ new).  Stock cleanup below
	 * only handles attribute-level external; our inline split parent needs this
	 * nested pass.  UPDATE only -- oldtup carries the prior values.
	 */
	if (oldtup != NULL)
		heap_toast_update_nested_cleanup(&ttc);

	toast_tuple_cleanup(&ttc);

	return result_tuple;
}


/* ----------
 * toast_flatten_tuple -
 *
 *	"Flatten" a tuple to contain no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 *
 *	Note: we expect the caller already checked HeapTupleHasExternal(tup),
 *	so there is no need for a short-circuit path.
 * ----------
 */
HeapTuple
toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc)
{
	HeapTuple	new_tuple;
	int			numAttrs = tupleDesc->natts;
	int			i;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/*
	 * Break down the tuple into fields.
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(tup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * Look at non-null varlena attributes
		 */
		if (!toast_isnull[i] && TupleDescCompactAttr(tupleDesc, i)->attlen == -1)
		{
			varlena    *new_value;

			new_value = (varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = detoast_external_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * Form the reconfigured tuple.
	 */
	new_tuple = heap_form_tuple(tupleDesc, toast_values, toast_isnull);

	/*
	 * Be sure to copy the tuple's identity fields.  We also make a point of
	 * copying visibility info, just in case anybody looks at those fields in
	 * a syscache entry.
	 */
	new_tuple->t_self = tup->t_self;
	new_tuple->t_tableOid = tup->t_tableOid;

	new_tuple->t_data->t_choice = tup->t_data->t_choice;
	new_tuple->t_data->t_ctid = tup->t_data->t_ctid;
	new_tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask |=
		tup->t_data->t_infomask & HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
	new_tuple->t_data->t_infomask2 |=
		tup->t_data->t_infomask2 & HEAP2_XACT_MASK;

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return new_tuple;
}


/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *	"Flatten" a tuple containing out-of-line toasted fields into a Datum.
 *	The result is always palloc'd in the current memory context.
 *
 *	We have a general rule that Datums of container types (rows, arrays,
 *	ranges, etc) must not contain any external TOAST pointers.  Without
 *	this rule, we'd have to look inside each Datum when preparing a tuple
 *	for storage, which would be expensive and would fail to extend cleanly
 *	to new sorts of container types.
 *
 *	However, we don't want to say that tuples represented as HeapTuples
 *	can't contain toasted fields, so instead this routine should be called
 *	when such a HeapTuple is being converted into a Datum.
 *
 *	While we're at it, we decompress any compressed fields too.  This is not
 *	necessary for correctness, but reflects an expectation that compression
 *	will be more effective if applied to the whole tuple not individual
 *	fields.  We are not so concerned about that that we want to deconstruct
 *	and reconstruct tuples just to get rid of compressed fields, however.
 *	So callers typically won't call this unless they see that the tuple has
 *	at least one external field.
 *
 *	On the other hand, in-line short-header varlena fields are left alone.
 *	If we "untoasted" them here, they'd just get changed back to short-header
 *	format anyway within heap_fill_tuple.
 * ----------
 */
Datum
toast_flatten_tuple_to_datum(HeapTupleHeader tup,
							 uint32 tup_len,
							 TupleDesc tupleDesc)
{
	HeapTupleHeader new_data;
	int32		new_header_len;
	int32		new_data_len;
	int32		new_tuple_len;
	HeapTupleData tmptup;
	int			numAttrs = tupleDesc->natts;
	int			i;
	bool		has_nulls = false;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = tup_len;
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tup;

	/*
	 * Break down the tuple into fields.
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(&tmptup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * Look at non-null varlena attributes
		 */
		if (toast_isnull[i])
			has_nulls = true;
		else if (TupleDescCompactAttr(tupleDesc, i)->attlen == -1)
		{
			varlena    *new_value;

			new_value = (varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value) ||
				VARATT_IS_COMPRESSED(new_value))
			{
				new_value = detoast_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * Calculate the new size of the tuple.
	 *
	 * This should match the reconstruction code in
	 * heap_toast_insert_or_update.
	 */
	new_header_len = SizeofHeapTupleHeader;
	if (has_nulls)
		new_header_len += BITMAPLEN(numAttrs);
	new_header_len = MAXALIGN(new_header_len);
	new_data_len = heap_compute_data_size(tupleDesc,
										  toast_values, toast_isnull);
	new_tuple_len = new_header_len + new_data_len;

	new_data = (HeapTupleHeader) palloc0(new_tuple_len);

	/*
	 * Copy the existing tuple header, but adjust natts and t_hoff.
	 */
	memcpy(new_data, tup, SizeofHeapTupleHeader);
	HeapTupleHeaderSetNatts(new_data, numAttrs);
	new_data->t_hoff = new_header_len;

	/* Set the composite-Datum header fields correctly */
	HeapTupleHeaderSetDatumLength(new_data, new_tuple_len);
	HeapTupleHeaderSetTypeId(new_data, tupleDesc->tdtypeid);
	HeapTupleHeaderSetTypMod(new_data, tupleDesc->tdtypmod);

	/* Copy over the data, and fill the null bitmap if needed */
	heap_fill_tuple(tupleDesc,
					toast_values,
					toast_isnull,
					(char *) new_data + new_header_len,
					new_data_len,
					&(new_data->t_infomask),
					has_nulls ? new_data->t_bits : NULL);

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return PointerGetDatum(new_data);
}


/* ----------
 * toast_build_flattened_tuple -
 *
 *	Build a tuple containing no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 *
 *	This is essentially just like heap_form_tuple, except that it will
 *	expand any external-data pointers beforehand.
 *
 *	It's not very clear whether it would be preferable to decompress
 *	in-line compressed datums while at it.  For now, we don't.
 * ----------
 */
HeapTuple
toast_build_flattened_tuple(TupleDesc tupleDesc,
							const Datum *values,
							const bool *isnull)
{
	HeapTuple	new_tuple;
	int			numAttrs = tupleDesc->natts;
	int			num_to_free;
	int			i;
	Datum		new_values[MaxTupleAttributeNumber];
	void	   *freeable_values[MaxTupleAttributeNumber];

	/*
	 * We can pass the caller's isnull array directly to heap_form_tuple, but
	 * we potentially need to modify the values array.
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	memcpy(new_values, values, numAttrs * sizeof(Datum));

	num_to_free = 0;
	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * Look at non-null varlena attributes
		 */
		if (!isnull[i] && TupleDescCompactAttr(tupleDesc, i)->attlen == -1)
		{
			varlena    *new_value;

			new_value = (varlena *) DatumGetPointer(new_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = detoast_external_attr(new_value);
				new_values[i] = PointerGetDatum(new_value);
				freeable_values[num_to_free++] = new_value;
			}
		}
	}

	/*
	 * Form the reconfigured tuple.
	 */
	new_tuple = heap_form_tuple(tupleDesc, new_values, isnull);

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < num_to_free; i++)
		pfree(freeable_values[i]);

	return new_tuple;
}

/*
 * Fetch a TOAST slice from a heap table.
 *
 * toastrel is the relation from which chunks are to be fetched.
 * valueid identifies the TOAST value from which chunks are being fetched.
 * attrsize is the total size of the TOAST value.
 * sliceoffset is the byte offset within the TOAST value from which to fetch.
 * slicelength is the number of bytes to be fetched from the TOAST value.
 * result is the varlena into which the results should be written.
 */
void
heap_fetch_toast_slice(Relation toastrel, Oid valueid, int32 attrsize,
					   int32 sliceoffset, int32 slicelength,
					   varlena *result)
{
	Relation   *toastidxs;
	ScanKeyData toastkey[3];
	TupleDesc	toasttupDesc = toastrel->rd_att;
	int			nscankeys;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	int32		expectedchunk;
	int32		totalchunks = ((attrsize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;
	int			startchunk;
	int			endchunk;
	int			num_indexes;
	int			validIndex;

	/* Look for the valid index of toast relation */
	validIndex = toast_open_indexes(toastrel,
									AccessShareLock,
									&toastidxs,
									&num_indexes);

	startchunk = sliceoffset / TOAST_MAX_CHUNK_SIZE;
	endchunk = (sliceoffset + slicelength - 1) / TOAST_MAX_CHUNK_SIZE;
	Assert(endchunk <= totalchunks);

	/* Set up a scan key to fetch from the index. */
	ScanKeyInit(&toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(valueid));

	/*
	 * No additional condition if fetching all chunks. Otherwise, use an
	 * equality condition for one chunk, and a range condition otherwise.
	 */
	if (startchunk == 0 && endchunk == totalchunks - 1)
		nscankeys = 1;
	else if (startchunk == endchunk)
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(startchunk));
		nscankeys = 2;
	}
	else
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTGreaterEqualStrategyNumber, F_INT4GE,
					Int32GetDatum(startchunk));
		ScanKeyInit(&toastkey[2],
					(AttrNumber) 2,
					BTLessEqualStrategyNumber, F_INT4LE,
					Int32GetDatum(endchunk));
		nscankeys = 3;
	}

	/* Prepare for scan */
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   get_toast_snapshot(), nscankeys, toastkey);

	/*
	 * Read the chunks by index
	 *
	 * The index is on (valueid, chunkidx) so they will come in order
	 */
	expectedchunk = startchunk;
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		int32		curchunk;
		Pointer		chunk;
		bool		isnull;
		char	   *chunkdata;
		int32		chunksize;
		int32		expected_size;
		int32		chcpystrt;
		int32		chcpyend;

		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		curchunk = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		if (!VARATT_IS_EXTENDED(chunk))
		{
			chunksize = VARSIZE(chunk) - VARHDRSZ;
			chunkdata = VARDATA(chunk);
		}
		else if (VARATT_IS_SHORT(chunk))
		{
			/* could happen due to heap_form_tuple doing its thing */
			chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
			chunkdata = VARDATA_SHORT(chunk);
		}
		else
		{
			/* should never happen */
			elog(ERROR, "found toasted toast chunk for toast value %u in %s",
				 valueid, RelationGetRelationName(toastrel));
			chunksize = 0;		/* keep compiler quiet */
			chunkdata = NULL;
		}

		/*
		 * Some checks on the data we've found
		 */
		if (curchunk != expectedchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk number %d (expected %d) for toast value %u in %s",
									 curchunk, expectedchunk, valueid,
									 RelationGetRelationName(toastrel))));
		if (curchunk > endchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
									 curchunk,
									 startchunk, endchunk, valueid,
									 RelationGetRelationName(toastrel))));
		expected_size = curchunk < totalchunks - 1 ? TOAST_MAX_CHUNK_SIZE
			: attrsize - ((totalchunks - 1) * TOAST_MAX_CHUNK_SIZE);
		if (chunksize != expected_size)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s",
									 chunksize, expected_size,
									 curchunk, totalchunks, valueid,
									 RelationGetRelationName(toastrel))));

		/*
		 * Copy the data into proper place in our result
		 */
		chcpystrt = 0;
		chcpyend = chunksize - 1;
		if (curchunk == startchunk)
			chcpystrt = sliceoffset % TOAST_MAX_CHUNK_SIZE;
		if (curchunk == endchunk)
			chcpyend = (sliceoffset + slicelength - 1) % TOAST_MAX_CHUNK_SIZE;

		memcpy(VARDATA(result) +
			   curchunk * TOAST_MAX_CHUNK_SIZE - sliceoffset + chcpystrt,
			   chunkdata + chcpystrt,
			   (chcpyend - chcpystrt) + 1);

		expectedchunk++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (expectedchunk != (endchunk + 1))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("missing chunk number %d for toast value %u in %s",
								 expectedchunk, valueid,
								 RelationGetRelationName(toastrel))));

	/* End scan and close indexes. */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
}
