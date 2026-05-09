/*-------------------------------------------------------------------------
 *
 * jsonb_toaster_lite_update_probe.c
 *	  L2.1a: dry-run update probe.
 *
 * Surfaces the locator + validation decisions that L2.1b's real
 * physical-rewrite update will use, *without* touching storage.
 * Every counter the L2.1 acceptance test pins is computed here
 * exactly the same way the actual update will compute it; the only
 * difference is that this entry point returns the prediction
 * instead of issuing chunk-row UPDATEs.
 *
 * SQL signature:
 *
 *	jbtl_update_probe(j jsonb, key text, new_value jsonb)
 *	    RETURNS (
 *	        can_fast_update      bool,
 *	        fallback_reason      text,
 *	        value_offset         int,
 *	        old_len              int,
 *	        new_len              int,
 *	        chunks_total         int,
 *	        chunks_rewritten     int,
 *	        chunks_untouched     int,
 *	        write_fraction       float8,
 *	        rewrite_amplification int
 *	    )
 *
 * Decision tree mirrors @yoda's L2.1 spec:
 *
 *	1.  Unwrap to JBTL_POINTER (only POINTER mode supported in L2.1).
 *	    JBTL_PLAIN_JSONB / JBTL_POINTER_COMPRESSED_CHUNKS / non-CUSTOM
 *	    -> can_fast_update=false, fallback_reason='not POINTER mode'.
 *
 *	2.  Probe the old value's offset and encoded length via
 *	    jbtl_toast_fetch_object_field; this is the same locator the
 *	    L1.4 fast read uses.  fallback=true (key is a container, or
 *	    >50%-of-body, or root not object) -> fallback_reason='not in
 *	    fast-path universe'.  Key not found -> fallback_reason='missing
 *	    key'.
 *
 *	3.  Encode new_value into the SAME jsonb binary representation the
 *	    writer uses, then take the value's encoded length.  L2.1
 *	    accepts only same-length scalar updates (numeric/string/bool/
 *	    null).  new_len != old_len -> fallback_reason='length-changing'.
 *	    Type mismatch (encoded JEntry tag differs) -> fallback_reason=
 *	    'type-changing'.
 *
 *	4.  Walk the toast relation with the same BT scan jbtl_chunk_inspect
 *	    uses, count the chunks whose [raw_offset, raw_offset+raw_size)
 *	    overlaps the value range [value_offset, value_offset+old_len).
 *	    chunks_rewritten = overlap count;
 *	    chunks_untouched = chunks_total - chunks_rewritten;
 *	    write_fraction   = chunks_rewritten / chunks_total;
 *	    rewrite_amplification is reserved for future edge cases (chunk
 *	    that re-encrypts more bytes than it reads); for L2.1 it is 1
 *	    on the success path and 0 on fallback.
 *
 * No storage mutation occurs.  AccessShareLock on the toast relation
 * matches what jbtl_chunk_inspect already takes.
 *
 * The actual physical rewrite (L2.1b) will reuse the locator, the
 * length-validation step, and the chunk-overlap walk.  Keeping them
 * here in one entry point lets L2.1b call this same code path
 * internally with a write hook attached, without splitting the
 * acceptance contract across two files.
 *
 * Copyright (c) 2026, Postgres Professional
 *
 * IDENTIFICATION
 *	  contrib/jsonb_toaster_lite/jsonb_toaster_lite_update_probe.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

#include "varatt_custom.h"

#include "jsonb_toaster_lite.h"


static bool jbtl_update_probe_extract_value(Jsonb *new_doc,
											const char *key, int keylen,
											int32 *out_len,
											enum jbvType *out_type);


/*
 * jbtl_update_probe_extract_jsonb_value
 *	Look up `key` in `new_doc` (a parsed jsonb), and report the
 *	encoded byte length of just the value portion (the bytes that
 *	would land in the chunk stream at the corresponding offset, NOT
 *	including the JEntry header which lives in the prefix area).
 *
 *	Returns true on success and writes *out_len (and *out_jbv_type
 *	for the type-compatibility check); false if `new_doc` does not
 *	contain `key` at the top level, or root is not an object.
 *
 *	This mirrors the writer's encoding rules so that the prediction
 *	matches what L2.1b will physically write.
 */
static bool
jbtl_update_probe_extract_value(Jsonb *new_doc, const char *key, int keylen,
								int32 *out_len, enum jbvType *out_type)
{
	JsonbContainer *jbc = &new_doc->root;
	JsonbValue	keyv;
	JsonbValue *valv;

	if (!JsonContainerIsObject(jbc))
		return false;

	keyv.type = jbvString;
	keyv.val.string.val = (char *) key;
	keyv.val.string.len = keylen;

	valv = findJsonbValueFromContainer(jbc, JB_FOBJECT, &keyv);
	if (valv == NULL)
		return false;

	*out_type = valv->type;

	switch (valv->type)
	{
		case jbvNumeric:
			/*
			 * Numeric is stored INTALIGN'd, with the Numeric varlena
			 * itself laid down verbatim.  The value's encoded length
			 * is VARSIZE_ANY of the Numeric.  This is what the L1.4
			 * fast read returns as value_byte_length.
			 */
			*out_len = (int32) VARSIZE_ANY(valv->val.numeric);
			break;

		case jbvString:
			*out_len = valv->val.string.len;
			break;

		case jbvBool:
		case jbvNull:
			/*
			 * Bool and null carry their state in the JEntry tag, not
			 * in the body.  The body length is zero.  L2.1 does not
			 * yet support these (the same-length contract is
			 * trivially met but the JEntry-only update is a separate
			 * mechanism).  We report length 0 here and let the
			 * type-check step decline.
			 */
			*out_len = 0;
			break;

		default:
			/* jbvArray / jbvObject reach here as jbvBinary in
			 * findJsonbValueFromContainer, so any other type is
			 * unexpected. */
			return false;
	}

	pfree(valv);
	return true;
}


PG_FUNCTION_INFO_V1(jbtl_update_probe);

Datum
jbtl_update_probe(PG_FUNCTION_ARGS)
{
	Datum		raw_datum = PG_GETARG_DATUM(0);
	text	   *key_text = PG_GETARG_TEXT_PP(1);
	Jsonb	   *new_doc = PG_GETARG_JSONB_P(2);
	struct varlena *raw = (struct varlena *) DatumGetPointer(raw_datum);

	const char *key = VARDATA_ANY(key_text);
	int			keylen = VARSIZE_ANY_EXHDR(key_text);

	uint32		mode;
	struct varatt_external ext_ptr;

	bool		can_fast_update = false;
	const char *fallback_reason = "init";
	int32		value_offset = 0;
	int32		old_len = 0;
	int32		new_len = 0;
	int32		chunks_total = 0;
	int32		chunks_rewritten = 0;
	int32		chunks_untouched = 0;
	double		write_fraction = 0.0;
	int32		rewrite_amplification = 0;

	TupleDesc	tupdesc;
	Datum		values[10];
	bool		isnull[10] = {false, false, false, false, false,
							  false, false, false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "function returning record called in context that cannot accept type record");
	tupdesc = BlessTupleDesc(tupdesc);

	/* ---- Step 1: must be JBTL_POINTER (plain chunks) -------------- */

	if (!jbtl_unwrap_to_toast_pointer(raw, &mode, &ext_ptr))
	{
		fallback_reason = "not a jsonb_toaster_lite pointer";
		goto done;
	}

	if (mode != JBTL_POINTER)
	{
		/*
		 * JBTL_PLAIN_JSONB has no chunks to rewrite (the body is
		 * inline); JBTL_POINTER_COMPRESSED_CHUNKS would force us to
		 * decompress, splice, and recompress — outside L2.1 scope.
		 */
		if (mode == JBTL_PLAIN_JSONB)
			fallback_reason = "inline (JBTL_PLAIN_JSONB), no chunks to rewrite";
		else if (mode == JBTL_POINTER_COMPRESSED_CHUNKS)
			fallback_reason = "compressed chunks not supported in L2.1";
		else
			fallback_reason = "unsupported pointer mode";
		goto done;
	}

	/* ---- Step 2: locate the old value via the L1.4 fast path ------ */

	{
		bool		probe_fallback = false;
		int32		dummy_chunks_fetched = 0;
		JsonbValue *old_jbv;
		enum jbvType old_type;

		old_jbv = jbtl_toast_fetch_object_field(&ext_ptr, mode,
												key, keylen,
												&chunks_total,
												&dummy_chunks_fetched,
												&value_offset,
												&old_len,
												&probe_fallback,
												NULL);

		if (probe_fallback)
		{
			/*
			 * The L1.4 fast read declined for this key — typical
			 * reasons: value is a container, value is >50% of body,
			 * root is not an object.  L2.1 piggybacks on the same
			 * acceptance set: if the read can't slice it, the write
			 * can't slice it either.
			 */
			fallback_reason = "key not in L1.4 fast-path universe (container or oversized)";
			goto done;
		}

		if (old_jbv == NULL)
		{
			fallback_reason = "missing key";
			goto done;
		}

		old_type = old_jbv->type;

		/* ---- Step 3: encode new value, check same length & type --- */

		{
			enum jbvType new_type;

			if (!jbtl_update_probe_extract_value(new_doc, key, keylen,
												 &new_len, &new_type))
			{
				fallback_reason = "new value document missing key at top level";
				pfree(old_jbv);
				goto done;
			}

			if (new_type != old_type)
			{
				fallback_reason = "type-changing update";
				pfree(old_jbv);
				goto done;
			}

			/*
			 * L2.1 accepts only the body-bearing scalar types.  Bool
			 * and null carry their state in the JEntry tag, which is
			 * stored in the prefix area; updating them is a separate
			 * mechanism (rewrite the JEntry, not the body) and is
			 * deferred.
			 */
			if (new_type == jbvBool || new_type == jbvNull)
			{
				fallback_reason = "JEntry-only update (bool/null) not supported in L2.1";
				pfree(old_jbv);
				goto done;
			}

			if (new_type != jbvNumeric && new_type != jbvString)
			{
				fallback_reason = "unsupported scalar type";
				pfree(old_jbv);
				goto done;
			}

			if (new_len != old_len)
			{
				fallback_reason = "length-changing update";
				pfree(old_jbv);
				goto done;
			}
		}

		pfree(old_jbv);
	}

	/* ---- Step 4: count chunks overlapping [value_offset, value_offset+old_len) ---- */

	{
		Relation	toastrel;
		Relation   *toastidxs;
		int			num_indexes;
		int			validIndex;
		ScanKeyData toastkey;
		SysScanDesc toastscan;
		HeapTuple	ttup;
		int32		chunk_no = 0;
		int32		max_chunk = TOAST_MAX_CHUNK_SIZE;
		int32		val_lo = value_offset;
		int32		val_hi = value_offset + old_len;
		int32		visited = 0;

		toastrel = table_open(ext_ptr.va_toastrelid, AccessShareLock);
		validIndex = toast_open_indexes(toastrel,
										AccessShareLock,
										&toastidxs,
										&num_indexes);

		ScanKeyInit(&toastkey, (AttrNumber) 1,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(ext_ptr.va_valueid));
		toastscan = systable_beginscan_ordered(toastrel,
											   toastidxs[validIndex],
											   get_toast_snapshot(),
											   1, &toastkey);

		while ((ttup = systable_getnext_ordered(toastscan,
												ForwardScanDirection)) != NULL)
		{
			Pointer		chunk;
			bool		isnull2;
			int32		raw_size;
			int32		raw_offset;
			int32		chunk_lo;
			int32		chunk_hi;

			chunk = DatumGetPointer(fastgetattr(ttup, 3,
												toastrel->rd_att, &isnull2));
			Assert(!isnull2);

			if (VARATT_IS_COMPRESSED(chunk))
				raw_size = (int32) VARDATA_COMPRESSED_GET_EXTSIZE(chunk);
			else if (!VARATT_IS_EXTENDED(chunk))
				raw_size = (int32) (VARSIZE(chunk) - VARHDRSZ);
			else if (VARATT_IS_SHORT(chunk))
				raw_size = (int32) (VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT);
			else
				raw_size = 0;

			/* Plain mode: chunks tile the raw payload at fixed stride. */
			raw_offset = chunk_no * max_chunk;

			chunk_lo = raw_offset;
			chunk_hi = raw_offset + raw_size;

			if (chunk_lo < val_hi && chunk_hi > val_lo)
				chunks_rewritten++;

			chunk_no++;
			visited++;
		}

		systable_endscan_ordered(toastscan);
		toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
		table_close(toastrel, AccessShareLock);

		/*
		 * chunks_total from the L1.4 probe matches the visited count
		 * here; if they ever disagree, that's a bug worth surfacing
		 * loudly.
		 */
		if (chunks_total != visited)
			elog(WARNING,
				 "jbtl_update_probe: chunks_total mismatch: probe=%d scan=%d",
				 chunks_total, visited);
	}

	/* ---- Step 5: derived metrics --------------------------------- */

	chunks_untouched = chunks_total - chunks_rewritten;

	if (chunks_total > 0)
		write_fraction = (double) chunks_rewritten / (double) chunks_total;
	else
		write_fraction = 0.0;

	/*
	 * For L2.1 same-length scalar update, every chunk we touch
	 * receives one rewrite of equal-or-similar bytes.  Define
	 * rewrite_amplification = (bytes written) / (bytes changed); for
	 * the same-length case the new value occupies the same byte
	 * range as the old one, so per-chunk this is exactly 1.  We
	 * report 1 on the success path; future edge cases where a single
	 * value crosses chunk boundaries and forces a tail-shift will
	 * change this to >= 1.
	 */
	if (chunks_rewritten > 0)
		rewrite_amplification = 1;
	else
		rewrite_amplification = 0;

	can_fast_update = true;
	fallback_reason = "ok";

done:
	values[0] = BoolGetDatum(can_fast_update);
	values[1] = PointerGetDatum(cstring_to_text(fallback_reason));
	values[2] = Int32GetDatum(value_offset);
	values[3] = Int32GetDatum(old_len);
	values[4] = Int32GetDatum(new_len);
	values[5] = Int32GetDatum(chunks_total);
	values[6] = Int32GetDatum(chunks_rewritten);
	values[7] = Int32GetDatum(chunks_untouched);
	values[8] = Float8GetDatum(write_fraction);
	values[9] = Int32GetDatum(rewrite_amplification);

	tuple = heap_form_tuple(tupdesc, values, isnull);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
