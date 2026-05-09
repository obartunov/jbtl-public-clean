/*-------------------------------------------------------------------------
 *
 * jsonb_toaster_lite_chain.c
 *	 Chunk machinery for jsonb_toaster_lite (writer side and
 *	 reader side).
 *
 * Source-of-truth provenance:
 *	 Originally ported from postgrespro/postgres@jsonb_toaster
 *	 (HEAD 583a292), contrib/jsonb_toaster/jsonb_toast_internals.c,
 *	 with all jsonx_, Jsonx, and JSONX_ names renamed to
 *	 jbtl_, Jbtl, and JBTL_. Original names appear only in the
 *	 per-function port-trace comments.
 *
 * Adaptations relative to the postgrespro source:
 *	 - postgrespro init_toast_snapshot(Snapshot) is replaced by our
 *	 master's get_toast_snapshot() which returns the snapshot
 *	 directly. The local SnapshotData on the stack is no longer
 *	 needed for the delete path; we pass get_toast_snapshot() into
 *	 systable_beginscan_ordered().
 *	 - All other PG API signatures (heap_insert, index_insert,
 *	 simple_heap_delete, heap_abort_speculative, toast_open_indexes,
 *	 toast_close_indexes, toastrel_valueid_exists, toastid_valueid_exists,
 *	 GetNewOidWithIndex, systable_beginscan_ordered,
 *	 systable_getnext_ordered, systable_endscan_ordered) are unchanged
 *	 from postgrespro and used verbatim.
 *
 * Copyright (c) 2026, Postgres Professional
 *
 * IDENTIFICATION
 *	 contrib/jsonb_toaster_lite/jsonb_toaster_lite_chain.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/toast_internals.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "common/pg_lzcompress.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

#include "jsonb_toaster_lite.h"


/*
 * Per-chunk-compressed format header size: the offset from the start
 * of a 4B_C compressed-inline varlena to its first payload byte
 * (= varhdrsz + va_tcinfo). Internal writer arithmetic only;
 * readers should use VARDATA_COMPRESSED_GET_EXTSIZE.
 * VARDATA_COMPRESSED_GET_COMPRESS_METHOD instead of looking at this.
 *
 *	Equivalent to postgrespro's TOAST_COMPRESS_HDRSZ macro, which
 *	master no longer exposes. Computed via offsetof so it stays
 *	correct under any future struct-layout evolution.
 */
#define JBTL_COMPRESSED_HDR_SIZE \
	((int) offsetof(varattrib_4b, va_compressed.va_data))


/*
 * Minimum byte savings required to use the per-chunk-compressed shape
 * over the per-chunk-raw fallback shape.
 *
 *	Per-chunk row sizes:
 *	 raw fallback = chunk_input_size + VARHDRSZ (4-byte hdr)
 *	 compressed = compressed_payload + JBTL_COMPRESSED_HDR (4 hdr + 4 tcinfo)
 *
 *	Compressed wins exactly when
 *	 compressed_payload + JBTL_COMPRESSED_HDR
 *	 < chunk_input_size + VARHDRSZ - JBTL_COMPRESS_MIN_BENEFIT
 *
 *	JBTL_COMPRESS_MIN_BENEFIT = 2 means we require pglz to save at
 *	least 2 bytes more than the per-chunk-header overhead difference
 *	(JBTL_COMPRESSED_HDR - VARHDRSZ = 4) — i.e., compressed payload
 *	must be at least 6 bytes smaller than the uncompressed input for
 *	this row to be worth compressing. Below that the raw fallback's
 *	smaller header wins anyway and we save the decompression cost on
 *	read.
 *
 *	Value 2 preserved verbatim from postgrespro
 *	(jsonb_toast_internals.c:543). If profiling later argues for a
 *	different threshold, this is the one knob to turn.
 */
#define JBTL_COMPRESS_MIN_BENEFIT	2


/*
 * jbtl_toast_write_slice
 *	Ported from jsonx_toast_write_slice (internals.c:484..621).
 *
 *	Insert `slice_data` of length `slice_length` into the TOAST table,
 *	chunked at TOAST_MAX_CHUNK_SIZE bytes per row, optionally with
 *	per-chunk pglz compression (`compress_chunks=true`).
 *
 *	chunk_tids:
 *	 Reserved parameter; both current callers pass NULL. Was used
 *	 by an earlier DIRECT_TIDS storage mode that has been removed.
 *
 *	compress_chunks:
 *	 If true, attempt pglz on each chunk individually. If the
 *	 compressed payload is not actually smaller than max_chunks_size
 *	 minus a 2-byte safety margin, fall back to uncompressed for
 *	 that chunk. Note: when chunks are per-chunk compressed,
 *	 column 1 of the toast tuple stores the running byte offset of
 *	 the last byte of the chunk inside the original payload, NOT
 *	 chunk_seq. This lets the reader binary-search by offset.
 *
 *	Index entries are inserted directly via index_insert() to avoid
 *	the overhead of FormIndexDatum, since toast indexes are known to
 *	be a btree on (oid, chunk_seq).
 */
void
jbtl_toast_write_slice(Relation toastrel, Relation *toastidxs,
					   int num_indexes, int validIndex,
					   Oid valueid, int32 value_size,
					   int32 slice_length, char *slice_data, int options,
					   ItemPointerData *chunk_tids, bool compress_chunks)
{
	CommandId	mycid = GetCurrentCommandId(true);
	TupleDesc	toasttupDesc = toastrel->rd_att;
	/*
	 * Per-chunk scratch buffer. Allocated once per call rather than
	 * declared on the stack (postgrespro source put this on-stack as a
	 * 2 KB union; with BLCKSZ=32K the equivalent grows to 8 KB and is
	 * fragile in deep call stacks). heap_form_tuple makes its own
	 * copy of the data, so we can reuse the buffer across iterations
	 * and pfree at the end.
	 *
	 * The buffer is sized to hold either:
	 *  - a 4B raw-fallback varlena (4-byte header + max_chunks_size)
	 *  - a 4B_C compressed-inline varlena (header + va_tcinfo +
	 *  PGLZ_MAX_OUTPUT(input_cap) bytes) — by the input_cap math in
	 *  the compress-branch, this is also bounded by
	 *  max_chunks_size + VARHDRSZ.
	 */
	int32		max_chunks_size = TOAST_MAX_CHUNK_SIZE;
	char	   *chunk_data;
	int32		chunk_size;
	int32		chunk_seq = 0;
	int32		chunk_offset = 0;
	Datum		t_values[3];
	bool		t_isnull[3];

	chunk_data = (char *) palloc(max_chunks_size + VARHDRSZ);

	Assert(chunk_offset == 0);

	/*
	 * Initialize constant parts of the tuple data
	 */
	t_values[0] = ObjectIdGetDatum(valueid);
	t_values[2] = PointerGetDatum(chunk_data);
	t_isnull[0] = false;
	t_isnull[1] = false;
	t_isnull[2] = false;

	/*
	 * Split up the item into chunks
	 */
	while (slice_length > 0)
	{
		HeapTuple	toasttup;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Calculate the size of this chunk
		 */
		chunk_size = 0;

		/*
		 * Build a tuple and store it
		 */
		if (compress_chunks)
		{
			/*
			 * Per-chunk pglz compression.
			 *
			 *	Master's pglz_compress is the 4-arg form: it returns the
			 *	compressed payload size, or a negative number if the
			 *	input is incompressible / dest is too small. There is
			 *	no "input bytes consumed" out-parameter as in PG16devel,
			 *	so we explicitly bound the input slice we pass in such
			 *	that PGLZ_MAX_OUTPUT(input) plus the per-chunk header
			 *	cannot overrun the chunk_data buffer.
			 *
			 *	On success the chunk is written as a standard 4B_C
			 *	compressed-inline varlena: header + va_tcinfo +
			 *	pglz output. va_tcinfo encodes (method << 30) | extsize
			 *	where extsize = uncompressed bytes this chunk represents.
			 *	The reader detects this row by VARATT_IS_COMPRESSED().
			 *
			 *	On failure (or insufficient compression), fall through
			 *	to the raw branch below. The compress-vs-fallback
			 *	threshold is captured by JBTL_COMPRESS_MIN_BENEFIT
			 *	(see its definition above for the byte-arithmetic
			 *	derivation).
			 *
			 *	chunk_size_orig records the *uncompressed* bytes this
			 *	chunk consumes from slice_data; it is what gets added
			 *	to chunk_offset and subtracted from slice_length at
			 *	loop tail, regardless of whether the chunk was actually
			 *	compressed.
			 */
			int32		chunk_size_orig;
			int32		input_cap;
			int32		compressed_chunk_size;

			/*
			 * Bound the input so that PGLZ_MAX_OUTPUT(input) +
			 * JBTL_COMPRESSED_HDR_SIZE <= max_chunks_size + VARHDRSZ
			 * (the chunk_data buffer's capacity). PGLZ_MAX_OUTPUT(n)
			 * = n + 4, so we want:
			 *  input + 4 + JBTL_COMPRESSED_HDR_SIZE <= max + VARHDRSZ
			 *  input <= max + VARHDRSZ - 4 - JBTL_COMPRESSED_HDR_SIZE
			 */
			input_cap = max_chunks_size + VARHDRSZ - 4 - JBTL_COMPRESSED_HDR_SIZE;
			chunk_size_orig = Min(input_cap, slice_length);

			compressed_chunk_size =
				pglz_compress(slice_data, chunk_size_orig,
							  ((char *) chunk_data) + JBTL_COMPRESSED_HDR_SIZE,
							  PGLZ_strategy_default);

			if (compressed_chunk_size >= 0 &&
				compressed_chunk_size + JBTL_COMPRESSED_HDR_SIZE <
				chunk_size_orig + VARHDRSZ - JBTL_COMPRESS_MIN_BENEFIT)
			{
				varattrib_4b *vlp = (varattrib_4b *) chunk_data;

				vlp->va_compressed.va_tcinfo =
					(((uint32) TOAST_PGLZ_COMPRESSION_ID) << VARLENA_EXTSIZE_BITS) |
					(((uint32) chunk_size_orig) & VARLENA_EXTSIZE_MASK);
				SET_VARSIZE_COMPRESSED(chunk_data,
									   compressed_chunk_size + JBTL_COMPRESSED_HDR_SIZE);

				chunk_size = chunk_size_orig;	/* uncompressed bytes this row covers */
			}
			else
				chunk_size = 0;					/* fall through to raw */
		}

		if (chunk_size <= 0)
		{
			chunk_size = Min(max_chunks_size, slice_length);
			SET_VARSIZE(chunk_data, chunk_size + VARHDRSZ);
			memcpy(VARDATA(chunk_data), slice_data, chunk_size);
		}

		t_values[1] = Int32GetDatum(compress_chunks ? chunk_offset + chunk_size - 1 /* last offset of this chunk */ : chunk_seq);
		chunk_seq++;

		toasttup = heap_form_tuple(toasttupDesc, t_values, t_isnull);

		heap_insert(toastrel, toasttup, mycid, options, NULL);

		if (chunk_tids)
			memcpy(&chunk_tids[chunk_seq - 1], &toasttup->t_self,
				   sizeof(ItemPointerData));

		if (!HeapTupleIsHeapOnly(toasttup))
		/*
		 * Create the index entry. We cheat a little here by not using
		 * FormIndexDatum: this relies on the knowledge that the index columns
		 * are the same as the initial columns of the table for all the
		 * indexes. We also cheat by not providing an IndexInfo: this is okay
		 * for now because btree doesn't need one, but we might have to be
		 * more honest someday.
		 *
		 * Note also that there had better not be any user-created index on
		 * the TOAST table, since we don't bother to update anything else.
		 */
		for (int i = 0; i < num_indexes; i++)
		{
			/* Only index relations marked as ready can be updated */
			if (toastidxs[i]->rd_index->indisready)
				index_insert(toastidxs[i], t_values, t_isnull,
							 &(toasttup->t_self),
							 toastrel,
							 toastidxs[i]->rd_index->indisunique ?
							 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
							 false, NULL);
		}

		/*
		 * Free memory
		 */
		heap_freetuple(toasttup);

		/*
		 * Move on to next chunk
		 */
		chunk_offset += chunk_size;
		slice_length -= chunk_size;
		slice_data += chunk_size;
	}

	pfree(chunk_data);
}


/*
 * jbtl_toast_save_datum_ext
 *	Ported from jsonx_toast_save_datum_ext (internals.c:623..819).
 *
 *	Push a datum into the TOAST relation associated with `rel`,
 *	building a varatt_external pointer to be returned to the caller
 *	wrapped in either a bare TOAST pointer, a JBTL_POINTER_DIRECT_TIDS
 *	custom pointer (when chunk_tids/p_chunk_tids_ptr is supplied), or
 *	a JBTL_POINTER_COMPRESSED_CHUNKS custom pointer (when
 *	compress_chunks is true and chunk_tids is NULL).
 *
 *	Walks four parameter shapes from the caller:
 *	 1. compressed source datum -> preserve compression in
 *	 va_extinfo, write payload as-is.
 *	 2. SHORT-header datum -> normalize to standard 4-byte header
 *	 prior to splitting into chunks.
 *	 3. plain datum -> straightforward.
 *	 4. table-rewrite (CLUSTER/VACUUM FULL) -> reuse old toast OID
 *	 and short-circuit the data-write loop if the same OID is
 *	 already present in the new toast table.
 */
Datum
jbtl_toast_save_datum_ext(Relation rel, Oid toasterid, Datum value,
						  struct varlena *oldexternal, int options,
						  struct varlena **p_chunk_tids_ptr,
						  ItemPointerData *chunk_tids,
						  bool compress_chunks)
{
	Relation	toastrel;
	Relation   *toastidxs;
	struct varlena *result;
	struct varatt_external toast_pointer;
	char	   *data_p;
	int32		data_todo;
	Pointer		dval = DatumGetPointer(value);
	int			num_indexes;
	int			validIndex;

	Assert(!VARATT_IS_EXTERNAL(DatumGetPointer(value)));

	/*
	 * Open the toast relation and its indexes. We can use the index to check
	 * uniqueness of the OID we assign to the toasted item, even though it has
	 * additional columns besides OID.
	 */
	toastrel = table_open(rel->rd_rel->reltoastrelid, RowExclusiveLock);

	/* Open all the toast indexes and look for the valid one */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Get the data pointer and length, and compute va_rawsize and va_extinfo.
	 *
	 * va_rawsize is the size of the equivalent fully uncompressed datum, so
	 * we have to adjust for short headers.
	 *
	 * va_extinfo stored the actual size of the data payload in the toast
	 * records and the compression method in first 2 bits if data is
	 * compressed.
	 */
	if (VARATT_IS_SHORT(dval))
	{
		data_p = VARDATA_SHORT(dval);
		data_todo = VARSIZE_SHORT(dval) - VARHDRSZ_SHORT;
		toast_pointer.va_rawsize = data_todo + VARHDRSZ;	/* as if not short */
		toast_pointer.va_extinfo = data_todo;
	}
	else if (VARATT_IS_COMPRESSED(dval))
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		/* rawsize in a compressed datum is just the size of the payload */
		toast_pointer.va_rawsize = VARDATA_COMPRESSED_GET_EXTSIZE(dval) + VARHDRSZ;

		/* set external size and compression method */
		VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(toast_pointer, data_todo,
													 VARDATA_COMPRESSED_GET_COMPRESS_METHOD(dval));
		/* Assert that the numbers look like it's compressed */
		Assert(VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));
	}
	else
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		toast_pointer.va_rawsize = VARSIZE(dval);
		toast_pointer.va_extinfo = data_todo;
	}

	/*
	 * Insert the correct table OID into the result TOAST pointer.
	 *
	 * Normally this is the actual OID of the target toast table, but during
	 * table-rewriting operations such as CLUSTER, we have to insert the OID
	 * of the table's real permanent toast table instead. rd_toastoid is set
	 * if we have to substitute such an OID.
	 */
	if (OidIsValid(rel->rd_toastoid))
		toast_pointer.va_toastrelid = rel->rd_toastoid;
	else
		toast_pointer.va_toastrelid = RelationGetRelid(toastrel);

	/*
	 * Choose an OID to use as the value ID for this toast value.
	 *
	 * Normally we just choose an unused OID within the toast table. But
	 * during table-rewriting operations where we are preserving an existing
	 * toast table OID, we want to preserve toast value OIDs too. So, if
	 * rd_toastoid is set and we had a prior external value from that same
	 * toast table, re-use its value ID. If we didn't have a prior external
	 * value (which is a corner case, but possible if the table's attstorage
	 * options have been changed), we have to pick a value ID that doesn't
	 * conflict with either new or existing toast value OIDs.
	 */
	if (!OidIsValid(rel->rd_toastoid))
	{
		/* normal case: just choose an unused OID */
		toast_pointer.va_valueid =
			GetNewOidWithIndex(toastrel,
							   RelationGetRelid(toastidxs[validIndex]),
							   (AttrNumber) 1);
	}
	else
	{
		/* rewrite case: check to see if value was in old toast table */
		toast_pointer.va_valueid = InvalidOid;
		if (oldexternal != NULL)
		{
			struct varatt_external old_toast_pointer;

			Assert(VARATT_IS_EXTERNAL_ONDISK(oldexternal));
			/* Must copy to access aligned fields */
			VARATT_EXTERNAL_GET_POINTER(old_toast_pointer, oldexternal);
			if (old_toast_pointer.va_toastrelid == rel->rd_toastoid)
			{
				/* This value came from the old toast table; reuse its OID */
				toast_pointer.va_valueid = old_toast_pointer.va_valueid;

				/*
				 * There is a corner case here: the table rewrite might have
				 * to copy both live and recently-dead versions of a row, and
				 * those versions could easily reference the same toast value.
				 * When we copy the second or later version of such a row,
				 * reusing the OID will mean we select an OID that's already
				 * in the new toast table. Check for that, and if so, just
				 * fall through without writing the data again.
				 */
				if (toastrel_valueid_exists(toastrel,
											toast_pointer.va_valueid))
				{
					/* Match, so short-circuit the data storage loop below */
					data_todo = 0;
				}
			}
		}
		if (toast_pointer.va_valueid == InvalidOid)
		{
			/*
			 * new value; must choose an OID that doesn't conflict in either
			 * old or new toast table
			 */
			do
			{
				toast_pointer.va_valueid =
					GetNewOidWithIndex(toastrel,
									   RelationGetRelid(toastidxs[validIndex]),
									   (AttrNumber) 1);
			} while (toastid_valueid_exists(rel->rd_toastoid,
											toast_pointer.va_valueid));
		}
	}

	if (chunk_tids)
		compress_chunks = false;

	jbtl_toast_write_slice(toastrel, toastidxs, num_indexes, validIndex,
						   toast_pointer.va_valueid, 0, data_todo, data_p,
						   options, chunk_tids, compress_chunks);

	/*
	 * Done - close toast relation and its indexes but keep the lock until
	 * commit, so as a concurrent reindex done directly on the toast relation
	 * would be able to wait for this transaction.
	 */
	toast_close_indexes(toastidxs, num_indexes, NoLock);
	table_close(toastrel, NoLock);

	if (compress_chunks)
		result = jbtl_toast_make_pointer_compressed_chunks(toasterid,
														   &toast_pointer,
														   toast_pointer.va_rawsize);
	else
	{
		/*
		 * Create the TOAST pointer value that we'll return
		 */
		result = (struct varlena *) palloc(TOAST_POINTER_SIZE);
		SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK);
		memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));
	}

	return PointerGetDatum(result);
}


/*
 * jbtl_toast_save_datum
 *	Ported from jsonx_toast_save_datum (internals.c:821..833).
 *
 *	Convenience wrapper: store a datum into the toast table with no
 *	custom toaster ID, no chunk-TID array, and no per-chunk compression.
 *	Returned datum is a bare TOAST pointer. Suitable for the simplest
 *	store path used by the acceptance test.
 */
Datum
jbtl_toast_save_datum(Relation rel, Datum value,
					  struct varlena *oldexternal, int options)
{
	return jbtl_toast_save_datum_ext(rel, InvalidOid, value, oldexternal,
									 options, NULL, NULL, false);
}


/*
 * jbtl_toast_delete_datum
 *	Ported from jsonx_toast_delete_datum (internals.c:835..904).
 *
 *	Delete every row in the toast relation that backs the given
 *	external on-disk varlena. No-op if the supplied datum is not an
 *	on-disk external.
 *
 *	Adaptation: postgrespro init_toast_snapshot(&SnapshotToast) is
 *	replaced with our master's get_toast_snapshot(), which returns
 *	the snapshot directly. The local SnapshotData is therefore not
 *	needed.
 */
void
jbtl_toast_delete_datum(Datum value, bool is_speculative)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	struct varatt_external toast_pointer;
	Relation	toastrel;
	Relation   *toastidxs;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	HeapTuple	toasttup;
	int			num_indexes;
	int			validIndex;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		return;

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/*
	 * Open the toast relation and its indexes
	 */
	toastrel = table_open(toast_pointer.va_toastrelid, RowExclusiveLock);

	/* Fetch valid relation used for process */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Setup a scan key to find chunks with matching va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
	 * Find all the chunks. (We don't actually care whether we see them in
	 * sequence or not, but since we've already locked the index we might as
	 * well use systable_beginscan_ordered.)
	 */
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   get_toast_snapshot(), 1, &toastkey);
	while ((toasttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, delete it
		 */
		if (is_speculative)
			heap_abort_speculative(toastrel, &toasttup->t_self);
		else
			simple_heap_delete(toastrel, &toasttup->t_self);
	}

	/*
	 * End scan and close relations but keep the lock until commit, so as a
	 * concurrent reindex done directly on the toast relation would be able to
	 * wait for this transaction.
	 */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, NoLock);
	table_close(toastrel, NoLock);
}


/*
 * jbtl_toast_fetch_full_plain
 *	Plain-chunk full-read counterpart of jbtl_toast_save_datum.
 *
 *	NOT a verbatim port of postgrespro jsonx_create_detoast_iterator +
 *	jsonx_fetch_datum_iterate. The postgrespro iterator is a 1500+
 *	LOC state machine that handles per-chunk-compressed chunks, sliced
 *	reads, and diff overlays. does not need any of that.
 *
 *	Logic is the minimal common subset extracted from those source
 *	functions:
 *
 *	 1. allocate a result varlena of size attrsize + VARHDRSZ
 *	 (mirrors postgrespro internals.c:1059 where the iterator
 *	 sets ressize to attrsize and writes data into a buffer of
 *	 that size; mirrors core toast_fetch_datum at detoast.c:407)
 *
 *	 2. table_open the toast relation in AccessShareLock (same lock
 *	 level as core toast_fetch_datum)
 *
 *	 3. table_relation_fetch_toast_slice with sliceoffset=0,
 *	 slicelength=attrsize -- tableam dispatches to the
 *	 heap-specific implementation, which performs the indexed
 *	 systable scan over (oid, chunk_seq), validates each chunk,
 *	 and assembles them into the result buffer. This is the
 *	 same primitive core toast_fetch_datum uses (detoast.c:423).
 *
 *	 4. table_close, return the assembled varlena.
 *
 *	When lands per-chunk decompression and sliced read, this
 *	function will be replaced by a port of jsonx_fetch_datum_iterate
 *	driven through a JbtlFetchDatumIteratorData state machine. Until
 *	then the ported pointer/struct types in jsonb_toaster_lite.h are
 *	declared but unused at runtime; that is acceptable here.
 *
 *	Caller must have validated that toast_pointer.va_rawsize > 0 and
 *	that the value is uncompressed (no per-chunk compression).
 */
struct varlena *
jbtl_toast_fetch_full_plain(struct varatt_external *toast_pointer,
							int32 *out_pages_touched)
{
	Relation	toastrel;
	struct varlena *result;
	int32		attrsize;

	attrsize = VARATT_EXTERNAL_GET_EXTSIZE(*toast_pointer);

	/*
	 * Refuse to handle compressed externals here. 's sole job is
	 * plain-chunk full read; full TOAST-level compression decompression
	 * is core's job and our writer does not produce compressed externals
	 * unless the source datum was already compressed. In that latter
	 * case the caller (the tsr_detoast wrapper) must handle decompression
	 * by delegating to core detoast_attr on the unwrapped bare pointer.
	 */
	Assert(!VARATT_EXTERNAL_IS_COMPRESSED(*toast_pointer));

	result = (struct varlena *) palloc(attrsize + VARHDRSZ);
	SET_VARSIZE(result, attrsize + VARHDRSZ);

	if (attrsize == 0)
	{
		if (out_pages_touched)
			*out_pages_touched = 0;
		return result;			/* probably shouldn't happen */
	}

	toastrel = table_open(toast_pointer->va_toastrelid, AccessShareLock);

	table_relation_fetch_toast_slice(toastrel,
									 toast_pointer->va_valueid,
									 attrsize,
									 0,			/* sliceoffset */
									 attrsize,	/* slicelength = full */
									 result);

	table_close(toastrel, AccessShareLock);

	/*
	 * Probe-only metric. When the caller wants page-level counts
	 * (passes a non-NULL out_pages_touched), do a second sysscan
	 * over every chunk of the value, accumulating distinct
	 * blockno's. This is OFF the production read path: production
	 * callers pass NULL.
	 */
	if (out_pages_touched)
	{
		int32		chunks_total =
			((attrsize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;
		*out_pages_touched =
			jbtl_count_pages_in_chunk_range(toast_pointer,
											0, chunks_total - 1);
	}

	return result;
}


/*
 * jbtl_toast_fetch_slice_plain
 *	Real sliced read for plain chunks.
 *
 *	Uses table_relation_fetch_toast_slice with non-zero sliceoffset
 *	and slicelength == requested length. The heap-side implementation
 *	(heap_fetch_toast_slice in src/backend/access/heap/heaptoast.c)
 *	computes
 *	 startchunk = sliceoffset / TOAST_MAX_CHUNK_SIZE
 *	 endchunk = (sliceoffset + slicelength - 1) / TOAST_MAX_CHUNK_SIZE
 *	and runs a BT-range systable scan on (valueid, chunkidx) restricted
 *	to that interval. Only chunks that overlap the requested byte range
 *	are physically read from disk.
 *
 *	This implementation deliberately does NOT replicate the postgrespro
 *	jsonx_create_detoast_iterator + jsonx_fetch_datum_iterate state
 *	machine. For plain (uncompressed) chunks, that machinery offers no
 *	functional uplift over a single call to table_relation_fetch_toast_slice
 *	with the right sliceoffset/slicelength: heap_fetch_toast_slice already
 *	uses the index range to fetch only the chunks that overlap the
 *	requested byte interval, which is exactly what acceptance asks
 *	for ("chunks_fetched_for_slice < chunks_total"). The iterator state
 *	machine is only required when per-chunk decompression must be threaded
 *	across slice calls; that is work and lands together with the
 *	per-chunk decompression port itself.
 *
 *	The result is a varlena of exactly slicelength + VARHDRSZ bytes
 *	carrying the requested byte range. Caller pfrees it.
 *
 *	Counters (optional out-parameters):
 *	 *out_chunks_total = ceil(attrsize / TOAST_MAX_CHUNK_SIZE)
 *	 *out_chunks_fetched = endchunk - startchunk + 1
 *	(both NULL-tolerant, populated only when non-NULL)
 *
 *	The caller is expected to clamp sliceoffset and slicelength to
 *	the value's range; we re-clamp defensively here as well.
 */
struct varlena *
jbtl_toast_fetch_slice_plain(struct varatt_external *toast_pointer,
							 int32 sliceoffset, int32 slicelength,
							 int32 *out_chunks_total,
							 int32 *out_chunks_fetched,
							 int32 *out_pages_touched)
{
	Relation	toastrel;
	struct varlena *result;
	int32		attrsize;
	int32		max_chunk = TOAST_MAX_CHUNK_SIZE;
	int32		startchunk;
	int32		endchunk;
	int32		chunks_total;

	attrsize = VARATT_EXTERNAL_GET_EXTSIZE(*toast_pointer);

	/*
	 * Refuse to handle compressed externals here (M8). Same contract
	 * as jbtl_toast_fetch_full_plain: an external pointer marked
	 * VARATT_EXTERNAL_IS_COMPRESSED carries a TOAST-level pglz/lz4
	 * compression bit (compression of the WHOLE value, not per-chunk),
	 * which our writer only emits when the source datum arrived
	 * already-compressed and we preserved the bit in save_datum_ext.
	 * Calling our slice path on such a pointer would treat the
	 * compressed bytes as raw bytes — silent corruption. The caller
	 * (tsr_detoast wrapper) is responsible for routing that case to
	 * core's detoast_attr first.
	 */
	Assert(!VARATT_EXTERNAL_IS_COMPRESSED(*toast_pointer));

	/* Clamp the requested slice into the valid range. */
	if (sliceoffset < 0)
		sliceoffset = 0;
	if (sliceoffset > attrsize)
		sliceoffset = attrsize;
	if (slicelength < 0)
		slicelength = 0;
	if (slicelength > attrsize - sliceoffset)
		slicelength = attrsize - sliceoffset;

	chunks_total = attrsize == 0 ? 0
		: ((attrsize - 1) / max_chunk) + 1;
	if (out_chunks_total)
		*out_chunks_total = chunks_total;

	/*
	 * Allocate a varlena of slicelength + VARHDRSZ. This is the buffer
	 * heap_fetch_toast_slice writes into; it interprets `result` as a
	 * varlena whose VARDATA region is `slicelength` bytes wide and
	 * starts logically at byte `sliceoffset` of the toasted value.
	 */
	result = (struct varlena *) palloc(slicelength + VARHDRSZ);
	SET_VARSIZE(result, slicelength + VARHDRSZ);

	if (slicelength == 0)
	{
		if (out_chunks_fetched)
			*out_chunks_fetched = 0;
		if (out_pages_touched)
			*out_pages_touched = 0;
		return result;
	}

	startchunk = sliceoffset / max_chunk;
	endchunk = (sliceoffset + slicelength - 1) / max_chunk;
	if (out_chunks_fetched)
		*out_chunks_fetched = endchunk - startchunk + 1;

	toastrel = table_open(toast_pointer->va_toastrelid, AccessShareLock);

	table_relation_fetch_toast_slice(toastrel,
									 toast_pointer->va_valueid,
									 attrsize,
									 sliceoffset,
									 slicelength,
									 result);

	table_close(toastrel, AccessShareLock);

	/*
	 * Probe-only: when the caller wants page-level counts, do the
	 * second metric scan. This is gated on a non-NULL out parameter
	 * so production fast paths stay on the single
	 * table_relation_fetch_toast_slice call.
	 */
	if (out_pages_touched)
		*out_pages_touched =
			jbtl_count_pages_in_chunk_range(toast_pointer,
											startchunk, endchunk);

	return result;
}


/*
 * jbtl_toast_extract_chunk_fields
 *	Decode a per-row tuple of the toast relation: extract the
 *	sequence number, the chunk data area, the chunk size (in
 *	uncompressed bytes), and — for per-chunk-compressed values —
 *	the compression method.
 *
 *	Adapted from jsonx_toast_extract_chunk_fields (internals.c:1216..
 *	1271). Two changes against the postgrespro source:
 *
 *	 (a) macro renames against current master:
 *	 TOAST_COMPRESS_EXTSIZE(c)
 *	 -> VARDATA_COMPRESSED_GET_EXTSIZE(c)
 *	 TOAST_COMPRESS_METHOD(c)
 *	 -> VARDATA_COMPRESSED_GET_COMPRESS_METHOD(c)
 *
 *	 (b) explicit `compressed_chunks_mode` boolean argument
 *	 replacing the postgrespro pattern of "non-NULL
 *	 compression_method pointer means the value is in
 *	 compressed-chunks mode". That pattern overloaded a
 *	 pure out-parameter with a hidden mode-flag, which made
 *	 misuse silent: a caller in plain-chunks mode that
 *	 happened to pass a non-NULL pointer would have its
 *	 sequential chunk index reinterpreted as a last-byte
 *	 offset and silently produce garbage. Splitting the
 *	 semantics makes the contract explicit.
 *
 *	Outputs:
 *	 *seqno
 *	 compressed_chunks_mode = false:
 *	 sequential chunk index 0..N-1 (column 2 raw value).
 *	 compressed_chunks_mode = true:
 *	 FIRST-byte offset of this chunk in the uncompressed
 *	 payload (column 2 raw value, which is last-byte offset,
 *	 minus chunksize - 1).
 *	 *chunkdata
 *	 compressed row : pointer to the WHOLE 4B_C compressed
 *	 varlena (the caller decompresses by
 *	 reaching past JBTL_COMPRESSED_HDR_SIZE
 *	 into the pglz output).
 *	 raw row : pointer to VARDATA / VARDATA_SHORT.
 *	 *chunksize
 *	 uncompressed bytes this chunk represents in the original
 *	 payload.
 *	 *out_method (NULL-tolerant)
 *	 TOAST_PGLZ_COMPRESSION_ID for compressed rows,
 *	 TOAST_INVALID_COMPRESSION_ID for raw rows.
 */
static void
jbtl_toast_extract_chunk_fields(Relation toastrel, TupleDesc toasttupDesc,
								Oid valueid, HeapTuple ttup,
								bool compressed_chunks_mode,
								int32 *seqno,
								char **chunkdata, int *chunksize,
								ToastCompressionId *out_method)
{
	Pointer		chunk;
	bool		isnull;
	ToastCompressionId method = TOAST_INVALID_COMPRESSION_ID;

	*seqno = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
	Assert(!isnull);

	chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
	Assert(!isnull);

	if (VARATT_IS_COMPRESSED(chunk) && compressed_chunks_mode)
	{
		*chunksize = VARDATA_COMPRESSED_GET_EXTSIZE(chunk);
		method = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(chunk);
		*chunkdata = (char *) chunk;
	}
	else if (!VARATT_IS_EXTENDED(chunk))
	{
		*chunksize = VARSIZE(chunk) - VARHDRSZ;
		*chunkdata = VARDATA(chunk);
	}
	else if (VARATT_IS_SHORT(chunk))
	{
		/*
		 * The writer in this contrib never emits a SHORT-header chunk;
		 * we always SET_VARSIZE on the per-chunk buffer with a 4-byte
		 * header, and heap_form_tuple does not down-convert that to a
		 * 1-byte header for values that are at least
		 * TOAST_MAX_CHUNK_SIZE-sized. This branch is therefore
		 * unreachable from data this contrib produced, but is kept for
		 * read-side robustness in case a third party ever stores
		 * SHORT-header chunks under our toaster.
		 */
		*chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
		*chunkdata = VARDATA_SHORT(chunk);
	}
	else
	{
		/* should never happen */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("found toasted toast chunk for toast value %u in %s",
						valueid, RelationGetRelationName(toastrel))));
		*chunksize = 0;			/* keep compiler quiet */
		*chunkdata = NULL;
	}

	if (compressed_chunks_mode)
		*seqno -= *chunksize - 1;

	if (out_method)
		*out_method = method;
}



/*
 * jbtl_toast_fetch_compressed_chunks
 *	Focused per-chunk-compressed reader.
 *
 *	NOT a port of jsonx_fetch_datum_iterate. That function is a
 *	multiplexer over plain / compressed_chunks / direct-TIDs.
 *	compressed-TIDs / diff modes through a state machine. 's
 *	scope is compressed_chunks only with mixed compressed/raw rows
 *	tolerated; a focused walker is significantly simpler.
 *
 *	One scan, three things at once:
 *	 - count chunks_total (every row for this valueid is enumerated);
 *	 - for rows whose [first_byte..last_byte] overlaps the requested
 *	 slice, copy the overlap into the result buffer (decompressing
 *	 if the row carries a 4B_C compressed varlena);
 *	 - count chunks_fetched and chunks_decompressed.
 *
 *	Why a single non-range scan instead of a BT range scan on
 *	[startbyte, endbyte]:
 *	 relation just to compute chunks_total. Folding the count
 *	 into the same scan eliminates the visibility race between
 *	 the two passes and the duplicate open/close work.
 *	 - the per-row decision "does this chunk overlap [startbyte,
 *	 endbyte]?" is a comparison on first_byte/last_byte which we
 *	 already need for the copy step, so the bookkeeping cost is
 *	 a few branches per row.
 *	 - the bandwidth cost is ONE extra fastgetattr per non-overlap
 *	 row, plus the index seek to read the full key set. For the
 *	 sizes the path targets (toast values up to a few
 *	 MB), this is a few dozen rows of wasted scan-but-no-decode
 *	 in exchange for a clean single-snapshot walk.
 *
 *	Decompression is whole-chunk via pglz_decompress(check_complete=
 *	true). Master has no toast_decompress_iterate (postgrespro-only),
 *	so incremental decompression is not available here; whole-chunk
 *	decode is fine because chunks are <= TOAST_MAX_CHUNK_SIZE.
 *
 *	Mixed compressed/raw rows: handled transparently because the row
 *	is dispatched through extract_chunk_fields, which splits on
 *	VARATT_IS_COMPRESSED per row. The writer guarantees that
 *	last-byte-offset (column 2) is monotonically increasing within
 *	one valueid regardless of per-row compression decisions.
 *
 *	Counters (NULL-tolerant):
 *	 *out_chunks_total = total rows for this valueid in the
 *	 toast relation
 *	 *out_chunks_fetched = subset that overlapped the slice and
 *	 had bytes copied into result
 *	 *out_chunks_decompressed = subset of chunks_fetched that were
 *	 compressed and pglz-decompressed
 */
struct varlena *
jbtl_toast_fetch_compressed_chunks(struct varatt_external *toast_pointer,
								   int32 sliceoffset, int32 slicelength,
								   int32 *out_chunks_total,
								   int32 *out_chunks_fetched,
								   int32 *out_chunks_decompressed,
								   int32 *out_pages_touched,
								   int32 *out_bytes_decompressed)
{
	Relation	toastrel;
	Relation   *toastidxs;
	int			num_indexes;
	int			validIndex;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	int32		attrsize;
	int32		startbyte;
	int32		endbyte;
	int32		chunks_total = 0;
	int32		chunks_fetched = 0;
	int32		chunks_decompressed = 0;
	int32		bytes_decompressed = 0;
	Bitmapset  *pages = NULL;
	struct varlena *result;

	/*
	 * Forbidden: a TOAST-level compressed external pointer. Our writer
	 * never emits one for the JBTL_POINTER_COMPRESSED_CHUNKS mode (that
	 * would mean compressing-an-already-compressed payload), and our
	 * reader cannot make sense of one — the chunk-row scan we do here
	 * decodes per-chunk pglz, not value-level pglz. If the assert
	 * fires, the value was constructed by a code path that has not
	 * been considered: for example, a future writer that preserves the
	 * source-datum compression bit when wrapping in
	 * JBTL_POINTER_COMPRESSED_CHUNKS. Diagnose first; do not silently
	 * unwrap. (Was M8 in the post- review.)
	 */
	Assert(!VARATT_EXTERNAL_IS_COMPRESSED(*toast_pointer));

	attrsize = VARATT_EXTERNAL_GET_EXTSIZE(*toast_pointer);

	/* Clamp slice into [0, attrsize). */
	if (sliceoffset < 0)
		sliceoffset = 0;
	if (sliceoffset > attrsize)
		sliceoffset = attrsize;
	if (slicelength < 0 || slicelength > attrsize - sliceoffset)
		slicelength = attrsize - sliceoffset;

	startbyte = sliceoffset;
	endbyte = sliceoffset + slicelength - 1;

	result = (struct varlena *) palloc(slicelength + VARHDRSZ);
	SET_VARSIZE(result, slicelength + VARHDRSZ);

	if (slicelength == 0)
	{
		if (out_chunks_total)
			*out_chunks_total = 0;
		if (out_chunks_fetched)
			*out_chunks_fetched = 0;
		if (out_chunks_decompressed)
			*out_chunks_decompressed = 0;
		if (out_pages_touched)
			*out_pages_touched = 0;
		if (out_bytes_decompressed)
			*out_bytes_decompressed = 0;
		return result;
	}

	toastrel = table_open(toast_pointer->va_toastrelid, AccessShareLock);
	validIndex = toast_open_indexes(toastrel,
									AccessShareLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Single scan over all rows of this valueid in chunk_seq order.
	 * Use a single equality scan key on column 1 (valueid) — no range
	 * key on column 2 — so chunks_total can be counted in the same
	 * pass. Per-row, decide whether to copy bytes into the slice
	 * based on first_byte/last_byte vs [startbyte, endbyte].
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer->va_valueid));

	toastscan = systable_beginscan_ordered(toastrel,
										   toastidxs[validIndex],
										   get_toast_snapshot(),
										   1, &toastkey);

	while ((ttup = systable_getnext_ordered(toastscan,
											ForwardScanDirection)) != NULL)
	{
		int32		residx;
		char	   *chunkdata;
		int32		chunksize;
		ToastCompressionId compression_method = TOAST_INVALID_COMPRESSION_ID;
		int32		first_byte;
		int32		last_byte;
		int32		copy_from;
		int32		copy_to;
		int32		copy_len;
		int32		dst_off;
		char	   *uncompressed_buf = NULL;
		const char *src_for_copy;

		chunks_total++;

		jbtl_toast_extract_chunk_fields(toastrel, toastrel->rd_att,
										toast_pointer->va_valueid, ttup,
										true,	/* compressed_chunks_mode */
										&residx, &chunkdata, &chunksize,
										&compression_method);

		/*
		 * extract_chunk_fields converted residx to FIRST byte offset
		 * because we passed compressed_chunks_mode=true.
		 */
		first_byte = residx;
		last_byte = first_byte + chunksize - 1;

		/*
		 * No overlap with the requested slice on either side: skip
		 * the row but keep counting chunks_total in subsequent
		 * iterations. The two conjuncts cover the two non-overlap
		 * cases explicitly:
		 *  last_byte < startbyte : this chunk ends before the slice
		 *  first_byte > endbyte : this chunk starts after the slice
		 */
		if (last_byte < startbyte || first_byte > endbyte)
			continue;

		chunks_fetched++;

		/*
		 * Track which physical TOAST page this chunk lives on. Multiple
		 * chunks can share a page (~4 chunks per 8KB page), so the count
		 * of distinct pages is the honest physical-I/O metric, while
		 * chunks_fetched stays as a logical-decode count.
		 */
		if (out_pages_touched)
		{
			BlockNumber blk = ItemPointerGetBlockNumber(&ttup->t_self);
			pages = bms_add_member(pages, (int) blk);
		}

		/* Decompress the chunk to a scratch buffer if compressed. */
		if (compression_method != TOAST_INVALID_COMPRESSION_ID)
		{
			int32		out_len;

			if (compression_method != TOAST_PGLZ_COMPRESSION_ID)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("jsonb_toaster_lite: unsupported per-chunk compression method %d",
								(int) compression_method)));

			uncompressed_buf = (char *) palloc(chunksize);
			out_len = pglz_decompress(((char *) chunkdata) + JBTL_COMPRESSED_HDR_SIZE,
									  VARSIZE_ANY(chunkdata) - JBTL_COMPRESSED_HDR_SIZE,
									  uncompressed_buf,
									  chunksize,
									  true);
			if (out_len < 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("pglz_decompress failed for chunk at offset %d of toast value %u in %s",
								first_byte, toast_pointer->va_valueid,
								RelationGetRelationName(toastrel))));
			Assert(out_len == chunksize);

			src_for_copy = uncompressed_buf;
			chunks_decompressed++;
			bytes_decompressed += chunksize;
		}
		else
		{
			/* Raw-fallback row.  chunkdata already points at the bytes. */
			src_for_copy = chunkdata;
		}

		/*
		 * Compute the overlap of this chunk's [first_byte..last_byte]
		 * with the requested slice [startbyte..endbyte], and copy.
		 * Reaching here means the overlap is non-empty (last_byte >=
		 * startbyte AND first_byte <= endbyte by the test above).
		 */
		copy_from = first_byte > startbyte ? first_byte : startbyte;
		copy_to = last_byte < endbyte ? last_byte : endbyte;
		Assert(copy_from <= copy_to);
		copy_len = copy_to - copy_from + 1;
		dst_off = copy_from - startbyte;
		memcpy(VARDATA(result) + dst_off,
			   src_for_copy + (copy_from - first_byte),
			   copy_len);

		if (uncompressed_buf)
			pfree(uncompressed_buf);
	}

	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
	table_close(toastrel, AccessShareLock);

	if (out_chunks_total)
		*out_chunks_total = chunks_total;
	if (out_chunks_fetched)
		*out_chunks_fetched = chunks_fetched;
	if (out_chunks_decompressed)
		*out_chunks_decompressed = chunks_decompressed;
	if (out_pages_touched)
	{
		*out_pages_touched = bms_num_members(pages);
		bms_free(pages);
	}
	if (out_bytes_decompressed)
		*out_bytes_decompressed = bytes_decompressed;

	return result;
}


/*
 * jbtl_count_pages_in_chunk_range
 *	Probe-only helper. Walks the toast relation a second time over
 *	a range of chunk_seq values, collecting the set of distinct
 *	page numbers the matching tuples live on, and returns the
 *	cardinality. Only the TID is read per row -- the chunk_data
 *	column is not touched.
 *
 *	Used by the page-level metric surfacing in jbtl_slice_probe,
 *	jbtl_object_field_probe, and the L14 evidence regression.
 *	Must NOT be invoked from production fast paths -- it adds one
 *	full btree descent + a tuple-by-tuple sysscan over the affected
 *	chunks. Callers in the read path gate it on a non-NULL out
 *	parameter from the SQL probe layer.
 *
 *	startchunk/endchunk are inclusive. When the caller wants every
 *	chunk of the value, pass startchunk=0 and endchunk=chunks_total-1.
 */
int32
jbtl_count_pages_in_chunk_range(struct varatt_external *toast_pointer,
								int32 startchunk, int32 endchunk)
{
	Relation	toastrel;
	Relation   *toastidxs;
	int			num_indexes;
	int			validIndex;
	ScanKeyData toastkey[3];
	int			nkeys;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	Bitmapset  *pages = NULL;
	int32		count;

	if (endchunk < startchunk)
		return 0;

	toastrel = table_open(toast_pointer->va_toastrelid, AccessShareLock);
	validIndex = toast_open_indexes(toastrel,
									AccessShareLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Scan key shape mirrors heap_fetch_toast_slice's choice:
	 *	- equality on (valueid)
	 *	- range on (chunk_seq) when needed
	 */
	ScanKeyInit(&toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer->va_valueid));

	if (startchunk == endchunk)
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(startchunk));
		nkeys = 2;
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
		nkeys = 3;
	}

	toastscan = systable_beginscan_ordered(toastrel,
										   toastidxs[validIndex],
										   get_toast_snapshot(),
										   nkeys, toastkey);

	while ((ttup = systable_getnext_ordered(toastscan,
											ForwardScanDirection)) != NULL)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(&ttup->t_self);
		pages = bms_add_member(pages, (int) blk);
	}

	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
	table_close(toastrel, AccessShareLock);

	count = bms_num_members(pages);
	bms_free(pages);

	return count;
}
