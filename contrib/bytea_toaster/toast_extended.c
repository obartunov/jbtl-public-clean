/*-------------------------------------------------------------------------
 *
 * toast_extended.c
 *	  Functions for internal use by appendable bytea toaster.
 *
 * Copyright (c) 2000-2022, PostgreSQL Global Development Group
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 *	  contrib/bytea_toaster/toast_extended.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/toast_internals.h"
#include "toast_extended.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

typedef struct ToastExtContext
{
	Relation	toastrel;
	Relation   *toastidxs;
	ScanKeyData toastkey[3];
	SysScanDesc toastscan;
	HeapTuple	toasttup;

	Oid			valueid;
	int			valid_index;
	int			num_indexes;
	int			nscankeys;

	int32		expectedchunk;
	int			startchunk;
	int			endchunk;
	int			totalchunks;

	int32		total_size;
	int32		chunk_data_size;
	int32		chunk_header_size;
	ToastChunkVisibilityCheck visibility_check;
	AppendableToastVersion attrversion;
} ToastExtContext;


static void
toast_write_slice(ToastExtContext *cxt,
				  int32 slice_offset, int32 slice_length, char *slice_data, int options,
				  void *chunk_header);

/* Open TOAST relation and indexes, start scan if needed. */
static void
toast_init_context(ToastExtContext *cxt, Oid toastrelid, Oid valueid,
				   LOCKMODE	lockmode,
				   int start_offset, int end_offset, int total_size,
				   int32 chunk_header_size,
				   ToastChunkVisibilityCheck visibility_check,
				   AppendableToastVersion attrversion)
{
	int			nscankeys;
	int			chunk_data_size = TOAST_MAX_CHUNK_SIZE - chunk_header_size;

	cxt->toastrel = table_open(toastrelid, lockmode);
	cxt->valueid = valueid;
	cxt->total_size = total_size;

	/* Look for the valid index of toast relation */
	cxt->valid_index = toast_open_indexes(cxt->toastrel, lockmode,
										  &cxt->toastidxs, &cxt->num_indexes);

	cxt->startchunk = start_offset / chunk_data_size;
	cxt->endchunk = end_offset > 0 ? (end_offset - 1) / chunk_data_size : -1;
	cxt->totalchunks = (total_size + chunk_data_size - 1) / chunk_data_size;

	cxt->expectedchunk = cxt->startchunk;

	cxt->chunk_data_size = chunk_data_size;
	cxt->chunk_header_size = chunk_header_size;
	cxt->visibility_check = visibility_check;
	cxt->attrversion = attrversion;

	cxt->toasttup = NULL;

	/*
	 * Don't need scan if valueid is not known (toast_save_datum()) or
	 * first fetched chunk is past the data end (toast_update_datum()).
	 */
	if (!OidIsValid(valueid) ||
		cxt->startchunk * chunk_data_size >= total_size)
	{
		cxt->toastscan = NULL;
		return;
	}

	/* Setup a scan key to find chunks with matching va_valueid */
	ScanKeyInit(&cxt->toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(valueid));

	/*
	 * No additional condition if fetching all chunks. Otherwise, use an
	 * equality condition for one chunk, and a range condition otherwise.
	 */
	if (cxt->startchunk == 0 && cxt->endchunk == cxt->totalchunks - 1)
		nscankeys = 1;
	else if (cxt->startchunk == cxt->endchunk)
	{
		ScanKeyInit(&cxt->toastkey[1],
					(AttrNumber) 2,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(cxt->startchunk));
		nscankeys = 2;
	}
	else
	{
		ScanKeyInit(&cxt->toastkey[1],
					(AttrNumber) 2,
					BTGreaterEqualStrategyNumber, F_INT4GE,
					Int32GetDatum(cxt->startchunk));
		nscankeys = 2;

		if (cxt->endchunk >= 0)
		{
			ScanKeyInit(&cxt->toastkey[2],
						(AttrNumber) 2,
						BTLessEqualStrategyNumber, F_INT4LE,
						Int32GetDatum(cxt->endchunk));
			nscankeys = 3;
		}
	}

	/*
	 * Prepare for scan.
	 *
	 * Find all the chunks.  (We don't actually care whether we see them in
	 * sequence or not, but since we've already locked the index we might as
	 * well use systable_beginscan_ordered.)
	 */
	cxt->toastscan = systable_beginscan_ordered(cxt->toastrel,
												cxt->toastidxs[cxt->valid_index],
												get_toast_snapshot(),
												nscankeys,
												cxt->toastkey);
}

/* End scan, close relation and indexes. */
static void
toast_free_context(ToastExtContext *cxt, LOCKMODE lockmode)
{
	if (cxt->toastscan)
		systable_endscan_ordered(cxt->toastscan);
	toast_close_indexes(cxt->toastidxs, cxt->num_indexes, lockmode);
	table_close(cxt->toastrel, lockmode);
}

/* ----------
 * toast_save_datum -
 *
 *	Save one single datum into the secondary relation and return
 *	a Datum reference for it.
 *
 * rel: the main relation we're working with (not the toast rel!)
 * value: datum to be pushed to toast storage
 * oldexternal: if not NULL, toast pointer previously representing the datum
 * options: options to be passed to heap_insert() for toast rows
 * ----------
 */
Datum
toast_save_datum_ext(Relation rel, Oid toastrelid, Oid toasteroid, Datum value,
					 struct varlena *oldexternal, int options, int attnum,
					 void *chunk_header, int chunk_header_size)
{
	ToastExtContext cxt;
	struct varlena *result;
	struct varatt_external toast_pointer;
	char	   *data_p;
	int32		data_todo;
	Pointer		dval = DatumGetPointer(value);

	Assert(!(VARATT_IS_EXTERNAL(DatumGetPointer(value))));

	Assert(OidIsValid(toastrelid));

	/*
	 * Open the toast relation and its indexes.  We can use the index to check
	 * uniqueness of the OID we assign to the toasted item, even though it has
	 * additional columns besides OID.
	 */
	toast_init_context(&cxt, toastrelid, InvalidOid, RowExclusiveLock,
					   0, 0, 0, chunk_header_size, NULL, 0);

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
	 * of the table's real permanent toast table instead.  rd_toastoid is set
	 * if we have to substitute such an OID.
	 */
	if (OidIsValid(rel->rd_toastoid))
		toast_pointer.va_toastrelid = rel->rd_toastoid;
	else
		toast_pointer.va_toastrelid = toastrelid;

	/*
	 * Choose an OID to use as the value ID for this toast value.
	 *
	 * Normally we just choose an unused OID within the toast table.  But
	 * during table-rewriting operations where we are preserving an existing
	 * toast table OID, we want to preserve toast value OIDs too.  So, if
	 * rd_toastoid is set and we had a prior external value from that same
	 * toast table, re-use its value ID.  If we didn't have a prior external
	 * value (which is a corner case, but possible if the table's attstorage
	 * options have been changed), we have to pick a value ID that doesn't
	 * conflict with either new or existing toast value OIDs.
	 */
	if (!OidIsValid(rel->rd_toastoid))
	{
		/* normal case: just choose an unused OID */
		toast_pointer.va_valueid =
			GetNewOidWithIndex(cxt.toastrel,
							   RelationGetRelid(cxt.toastidxs[cxt.valid_index]),
							   (AttrNumber) 1);
	}
	else
	{
		/*
		 * We can't reuse and share value ids because of possible
		 * existence of multiple versions of the chunks.
		 */
#if 0
		/* rewrite case: check to see if value was in old toast table */
		toast_pointer.va_valueid = InvalidOid;

		if (oldexternal != NULL)
		{
			struct varatt_external old_toast_pointer;

			Assert(VARATT_IS_EXTERNAL_ONDISK(oldexternal) || VARATT_IS_CUSTOM(oldexternal));

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
				 * in the new toast table.  Check for that, and if so, just
				 * fall through without writing the data again.
				 *
				 * While annoying and ugly-looking, this is a good thing
				 * because it ensures that we wind up with only one copy of
				 * the toast value when there is only one copy in the old
				 * toast table.  Before we detected this case, we'd have made
				 * multiple copies, wasting space; and what's worse, the
				 * copies belonging to already-deleted heap tuples would not
				 * be reclaimed by VACUUM.
				 */
				if (toastrel_valueid_exists(cxt.toastrel,
											toast_pointer.va_valueid))
				{
					/* Match, so short-circuit the data storage loop below */
					data_todo = 0;
				}
			}
		}

		if (toast_pointer.va_valueid == InvalidOid)
#endif
		{

			/*
			 * new value; must choose an OID that doesn't conflict in either
			 * old or new toast table
			 */
			do
			{
				toast_pointer.va_valueid =
					GetNewOidWithIndex(cxt.toastrel,
									   RelationGetRelid(cxt.toastidxs[cxt.valid_index]),
									   (AttrNumber) 1);
			} while (toastid_valueid_exists(rel->rd_toastoid,
											toast_pointer.va_valueid));
		}
	}

	cxt.valueid = toast_pointer.va_valueid;

	toast_write_slice(&cxt, 0, data_todo, data_p, options, chunk_header);

	/*
	 * Done - close toast relation and its indexes but keep the lock until
	 * commit, so as a concurrent reindex done directly on the toast relation
	 * would be able to wait for this transaction.
	 */
	toast_free_context(&cxt, NoLock);

	/*
	 * Create the TOAST pointer value that we'll return
	 */
	result = (struct varlena *) palloc(TOAST_POINTER_SIZE);
	SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));

	return PointerGetDatum(result);
}

/* Fetch chunk attributes: chunk_id, chunk_seq, chunk_data */
static void
toast_extract_chunk_fields(Relation toastrel, TupleDesc toasttupDesc,
						   Oid valueid, HeapTuple ttup, int32 *seqno,
						   char **chunkdata, int *chunksize)
{
	Pointer		chunk;
	bool		isnull;

	/*
	 * Have a chunk, extract the sequence number and the data
	 */
	*seqno = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
	Assert(!isnull);

	chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
	Assert(!isnull);

	if (!VARATT_IS_EXTENDED(chunk))
	{
		*chunksize = VARSIZE(chunk) - VARHDRSZ;
		*chunkdata = VARDATA(chunk);
	}
	else if (VARATT_IS_SHORT(chunk))
	{
		/* could happen due to heap_form_tuple doing its thing */
		*chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
		*chunkdata = VARDATA_SHORT(chunk);
	}
	else
	{
		/* should never happen */
		elog(ERROR, "found toasted toast chunk for toast value %u in %s",
			 valueid, RelationGetRelationName(toastrel));
		*chunksize = 0;		/* keep compiler quiet */
		*chunkdata = NULL;
	}
}

/*
 * Check if toast chunk was aborted.
 *
 * We always need to skip such chunks, other possibly visible chunks
 * are checked by ToastChunkVisibilityCheck.
 */
static bool
toast_tuple_is_aborted(HeapTupleHeader htup)
{
	TransactionId xmin;

	if (HeapTupleHeaderXminCommitted(htup))
		return false;

	if (HeapTupleHeaderXminInvalid(htup))
		return true;	/* aborted or crashed */

	xmin = HeapTupleHeaderGetXmin(htup);

	Assert(!HeapTupleHeaderXminInvalid(htup));

	if (TransactionIdIsInProgress(xmin)) /* FIXME TransactionIdIsCurrentTransactionId(xmin) */
		return false;

	if (TransactionIdDidCommit(xmin))
		return false;

	return true;	/* TransactionIdDidAbort(xmin) or crashed */
}

static void
toast_report_missing_chunk(ToastExtContext *cxt, int32 expectedchunk)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg_internal("missing chunk number %d for toast value %u in %s",
							 expectedchunk, cxt->valueid,
							 RelationGetRelationName(cxt->toastrel))));
}

/* Fetch next visible chunk from scan */
static bool
toast_fetch_next_chunk(ToastExtContext *cxt, char **chunkdata_ver,
					   int32 *chunksize_ver, ItemPointer chunk_tid)
{
	TupleDesc	toasttupDesc = cxt->toastrel->rd_att;
	HeapTuple	ttup;
	bool		versioned = cxt->chunk_header_size != 0;
	int32		curchunk;
	int32		expected_size;
	bool 		have_chunk = false;

	/*
	 * Catalog snapshots can be returned by GetOldestSnapshot() even if not
	 * registered or active. That easily hides bugs around not having a
	 * snapshot set up - most of the time there is a valid catalog snapshot.
	 * So additionally insist that the current snapshot is registered or
	 * active.
	 * Read the chunks by index
	 *
	 * The index is on (valueid, chunkidx) so they will come in order
	 */
	while ((ttup = systable_getnext_ordered(cxt->toastscan, ForwardScanDirection)) != NULL)
	{
		char	   *chunkdata;
		int32		chunksize;

		/* Have a chunk, extract the sequence number and the data */
		toast_extract_chunk_fields(cxt->toastrel, toasttupDesc, cxt->valueid, ttup,
								   &curchunk, &chunkdata, &chunksize);

		if (versioned)
		{
			/* Skip aborted chunks */
			if (toast_tuple_is_aborted(ttup->t_data))	// FIXME move upper
				continue;

			/*
			 * This is *not* MVCC visibility. This is special chunk visibility
			 * based on version numbers located at the beginning of the chunks.
			 */
			if (!cxt->visibility_check(cxt->toastscan, cxt->attrversion,
									   &chunkdata, &chunksize))
				continue;

			have_chunk = true;

			chunkdata += cxt->chunk_header_size;
			chunksize -= cxt->chunk_header_size;
		}

		*chunkdata_ver = chunkdata;
		*chunksize_ver = chunksize;

		if (chunk_tid)
			*chunk_tid = ttup->t_self;

		/*
		* Some checks on the data we've found
		*/
		if (curchunk != cxt->expectedchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					errmsg_internal("unexpected chunk number %d (expected %d) for toast value %u in %s",
									curchunk, cxt->expectedchunk, cxt->valueid,
									RelationGetRelationName(cxt->toastrel))));

		if (curchunk > cxt->endchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					errmsg_internal("unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
									curchunk,
									cxt->startchunk, cxt->endchunk, cxt->valueid,
									RelationGetRelationName(cxt->toastrel))));

		expected_size = curchunk < cxt->totalchunks - 1 ? cxt->chunk_data_size
			: cxt->total_size - ((cxt->totalchunks - 1) * cxt->chunk_data_size);

		Assert(*chunksize_ver == expected_size);

		if (*chunksize_ver != expected_size)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					errmsg_internal("unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s",
									*chunksize_ver, expected_size,
									curchunk, cxt->totalchunks, cxt->valueid,
									RelationGetRelationName(cxt->toastrel))));

		cxt->expectedchunk++;

		return true;
	}

	if (have_chunk)
	{
		/*
		 * Final checks that we successfully fetched the datum
		 */
		if (cxt->expectedchunk != (cxt->endchunk + 1))
			toast_report_missing_chunk(cxt, cxt->expectedchunk);
	}

	return false;
}

/*
 * Fetch a TOAST slice from a toast table.
 *
 * toastrel is the relation from which chunks are to be fetched.
 * valueid identifies the TOAST value from which chunks are being fetched.
 * attrsize is the total size of the TOAST value.
 * slice_offset is the byte offset within the TOAST value from which to fetch.
 * slice_length is the number of bytes to be fetched from the TOAST value.
 * result is the varlena into which the results should be written.
 */
void
toast_fetch_toast_slice(Oid toastrelid, Oid valueid,
						struct varlena *attr, int32 attrsize,
						int32 slice_offset, int32 slice_length,
						struct varlena *result, int32 chunk_header_size,
						ToastChunkVisibilityCheck visibility_check,
						AppendableToastVersion attrversion)
{
	ToastExtContext cxt;
	char	   *chunk_data;
	int			chunk_size;

	/* Open relation and indexes, start scan */
	toast_init_context(&cxt, toastrelid, valueid, AccessShareLock,
					   slice_offset, slice_offset + slice_length, attrsize,
					   chunk_header_size, visibility_check, attrversion);

	while (toast_fetch_next_chunk(&cxt, &chunk_data, &chunk_size, NULL))
	{
		/* Copy the data into proper place in our result */
		int32		chunk_offset = (cxt.expectedchunk - 1) * cxt.chunk_data_size;
		int32		copy_start;
		int32		copy_end;

		/* Skip unrelated chunk (should not happen) */
		if (slice_offset >= chunk_offset + chunk_size ||
			slice_offset + slice_length <= chunk_offset)
			elog(ERROR, "toast_fetch_next_chunk returned unrelated chunk for toast value %u in %s",
				 valueid, get_rel_name(toastrelid));

		copy_start = Max(0, slice_offset - chunk_offset);
		copy_end = Min(chunk_size, slice_offset + slice_length - chunk_offset);

		memcpy(VARDATA(result) +
			   chunk_offset - slice_offset + copy_start,
			   chunk_data + copy_start,
			   copy_end - copy_start);
	}

	/* End scan and close indexes */
	toast_free_context(&cxt, AccessShareLock);
}

/* Rewrite some chunks and/or append new chunks */
void
toast_update_datum(Datum value,
				   void *slice_data, int slice_offset, int slice_length,
				   void *chunk_header, int chunk_header_size,
				   ToastChunkVisibilityCheck visibility_check,
				   AppendableToastVersion attrversion, int options)
{
	ToastExtContext cxt;
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	struct varatt_external toast_pointer;
	int32		total_size;

	Assert(VARATT_IS_EXTERNAL_ONDISK(attr) || VARATT_IS_CUSTOM(attr));

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/* Data stored uncompressed */
	total_size = VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer);
	Assert(toast_pointer.va_rawsize == total_size + VARHDRSZ);

	/* Open the toast relation and its indexes */
	toast_init_context(&cxt,
					   toast_pointer.va_toastrelid,
					   toast_pointer.va_valueid, RowExclusiveLock,
					   slice_offset, slice_offset + slice_length, total_size,
					   chunk_header_size, visibility_check, attrversion);

	/* Update/append chunks */
	toast_write_slice(&cxt, slice_offset, slice_length, slice_data,
					  options, chunk_header);

	/* End scan and close indexes. */
	toast_free_context(&cxt, NoLock);
}

/* Subroutine for toast_save_datum_ext() and toast_update_datum() */
static void
toast_write_slice(ToastExtContext *cxt, int32 slice_offset,
				  int32 slice_length, char *slice_data, int options,
				  void *chunk_header)
{
	Relation	toastrel = cxt->toastrel;
	CommandId	mycid = GetCurrentCommandId(true);
	TupleDesc	toasttupDesc = toastrel->rd_att;
	union
	{
		struct varlena hdr;
		/* this is to make the union big enough for a chunk: */
		char		data[TOAST_MAX_CHUNK_SIZE + VARHDRSZ];
		/* ensure union is aligned well enough: */
		int32		align_it;
	}			chunk_data;
	int32		max_chunk_size = cxt->chunk_data_size;
	int32		chunk_size;
	int32		chunk_seq = slice_offset / max_chunk_size;
	int32		chunk_offset = chunk_seq * max_chunk_size;
	int32		last_old_chunk_seq = (cxt->total_size - 1) / max_chunk_size;
	Datum		t_values[3];
	bool		t_isnull[3];
	TU_UpdateIndexes upd_idxs = TU_All;

	/*
	 * Initialize constant parts of the tuple data
	 */
	t_values[0] = ObjectIdGetDatum(cxt->valueid);
	t_values[2] = PointerGetDatum(&chunk_data);
	t_isnull[0] = false;
	t_isnull[1] = false;
	t_isnull[2] = false;

	/*
	 * Split up the item into chunks
	 */
	while (slice_length > 0)
	{
		HeapTuple	toasttup;
		ItemPointerData old_tid = {0};

		int32		old_chunk_size = chunk_offset >= cxt->total_size ? 0 :
			Min(max_chunk_size, cxt->total_size - chunk_offset);

		int32		chunk_slice_start = slice_offset <= chunk_offset ?
			0 : slice_offset - chunk_offset;

		int32		copied_slice_size =
			Min(max_chunk_size - chunk_slice_start, slice_length);

		bool		rewrite_chunk =
			(slice_offset > chunk_offset &&
			 slice_offset < chunk_offset + max_chunk_size) ||
			slice_length < old_chunk_size;

		bool		is_update = cxt->toastscan && chunk_seq <= last_old_chunk_seq;

		CHECK_FOR_INTERRUPTS();

		/* Fetch old tuple and copy its data */
		if (is_update)
		{
			int32		old_chunk_size_2;
			char	   *old_chunk_data;
			bool		old_chunk_found =
				toast_fetch_next_chunk(cxt, &old_chunk_data, &old_chunk_size_2, &old_tid);

			if (!old_chunk_found)
				toast_report_missing_chunk(cxt, chunk_seq);

			Assert(old_chunk_size == old_chunk_size_2);

			if (rewrite_chunk)
				memcpy(VARDATA(&chunk_data) + cxt->chunk_header_size,
					   old_chunk_data, old_chunk_size_2);
		}

		/*
		 * Calculate the size of this chunk
		 */
		copied_slice_size = Min(max_chunk_size - chunk_slice_start, slice_length);
		chunk_size = Max(old_chunk_size, chunk_slice_start + copied_slice_size);

		/*
		 * Build a tuple and store it
		 */
		t_values[1] = Int32GetDatum(chunk_seq++);
		SET_VARSIZE(&chunk_data, chunk_size + cxt->chunk_header_size + VARHDRSZ);
		if (cxt->chunk_header_size > 0)
			memcpy(VARDATA(&chunk_data), chunk_header, cxt->chunk_header_size);
		memcpy(VARDATA(&chunk_data) + chunk_slice_start + cxt->chunk_header_size, slice_data, copied_slice_size);
		toasttup = heap_form_tuple(toasttupDesc, t_values, t_isnull);

		if (is_update)
		{
			TM_Result	result;
			TM_FailureData tmfd;
			LockTupleMode lockmode;

			result = heap_update(toastrel, &old_tid, toasttup,
								 mycid, 0, InvalidSnapshot, true,
								 &tmfd, &lockmode, &upd_idxs);

			switch (result)
			{
				case TM_Ok:
					/* done successfully */
					break;

				case TM_SelfModified:
					elog(ERROR, "TOAST tuple already updated by self");
					break;

				case TM_Updated:
					elog(ERROR, "TOAST tuple concurrently updated");
					break;

				case TM_Deleted:
					elog(ERROR, "TOAST tuple concurrently deleted");
					break;

				default:
					elog(ERROR, "unrecognized heap_update status: %u", result);
					break;
			}
		}
		else
			heap_insert(toastrel, toasttup, mycid, options, NULL);


		if (!HeapTupleIsHeapOnly(toasttup))
		/*
		 * Create the index entry.  We cheat a little here by not using
		 * FormIndexDatum: this relies on the knowledge that the index columns
		 * are the same as the initial columns of the table for all the
		 * indexes.  We also cheat by not providing an IndexInfo: this is okay
		 * for now because btree doesn't need one, but we might have to be
		 * more honest someday.
		 *
		 * Note also that there had better not be any user-created index on
		 * the TOAST table, since we don't bother to update anything else.
		 */
		for (int i = 0; i < cxt->num_indexes; i++)
		{
			/* Only index relations marked as ready can be updated */
			if (cxt->toastidxs[i]->rd_index->indisready)
				index_insert(cxt->toastidxs[i], t_values, t_isnull,
							 &(toasttup->t_self),
							 toastrel,
							 cxt->toastidxs[i]->rd_index->indisunique ?
							 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
							 false, NULL);
		}

		/* Free memory */
		heap_freetuple(toasttup);

		/* Move on to next chunk */
		chunk_offset += chunk_size;
		slice_length -= copied_slice_size;
		slice_data += copied_slice_size;
	}
}

/* Delete all visible toast chunks */
void
toast_delete_datum_ext(Datum value, bool is_speculative,
					   int32 chunk_header_size,
					   ToastChunkVisibilityCheck visibility_check,
					   AppendableToastVersion attrversion)
{
	ToastExtContext cxt;
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	struct varatt_external toast_pointer;
	ItemPointerData chunk_tid;
	char	   *chunk_data;
	int32		chunk_size;
	int32		total_size;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		return;

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/* Data is stored ucompressed */
	total_size = VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer);
	Assert(toast_pointer.va_rawsize == total_size + VARHDRSZ);

	/*
	 * Open the toast relation and its indexes
	 */
	toast_init_context(&cxt,
					   toast_pointer.va_toastrelid,
					   toast_pointer.va_valueid,
					   RowExclusiveLock,
					   0, total_size, total_size,
					   chunk_header_size, visibility_check, attrversion);

	/*
	 * Find all the chunks.  (We don't actually care whether we see them in
	 * sequence or not, but since we've already locked the index we might as
	 * well use systable_beginscan_ordered.)
	 */
	while (toast_fetch_next_chunk(&cxt, &chunk_data, &chunk_size, &chunk_tid))
	{
		/* Have a chunk, delete it */
		if (is_speculative)
			heap_abort_speculative(cxt.toastrel, &chunk_tid);
		else
			simple_heap_delete(cxt.toastrel, &chunk_tid);
	}

	/*
	 * End scan and close relations but keep the lock until commit, so as a
	 * concurrent reindex done directly on the toast relation would be able to
	 * wait for this transaction.
	 */
	toast_free_context(&cxt, NoLock);
}
