/*-------------------------------------------------------------------------
 *
 * jsonb_toaster_lite_object_field.c
 *	 KVMap-aware top-level object field lookup over sliced TOAST.
 *
 *	 Provides a fast path for `j->'key'` and `j->>'key'` that, instead
 *	 of detoasting the whole jsonb, reads only:
 *	 1. a small prefix of the body (container header + 2N JEntries
 *	 + optional KVMap + key area), then
 *	 2. the byte range that the looked-up value occupies.
 *
 *	 The lookup itself mirrors core's getKeyJsonValueFromContainer:
 *	 binary search by length-then-lex on key JEntries, then KVMap
 *	 redirection to the physical value index.
 *
 *	 Out of scope (caller falls back to core's full-detoast +
 *	 jsonb_object_field path):
 *	 - non-object root containers
 *	 - nested-container values (the fast path returns scalars only)
 *	 - JBTL_PLAIN_JSONB inline mode (already cheap; no slice savings)
 *	 - non-CUSTOM varlenas (default toaster, etc.)
 *
 * Copyright (c) 2026, Postgres Professional
 *
 * IDENTIFICATION
 *	 contrib/jsonb_toaster_lite/jsonb_toaster_lite_object_field.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/toast_hook.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"
#include "varatt.h"
#include "varatt_custom.h"

#include "jsonb_toaster_lite.h"


/*
 * Initial prefix-fetch size for the lookup. 4 KB covers the
 * structural part (container header + JEntries + KVMap + key area)
 * for realistic objects (up to a few hundred fields with sub-100B
 * keys). Picked as the smaller of this constant and the toast
 * extsize.
 *
 *	If the structural part doesn't fit, we issue a second slice for
 *	the precise needed range; if even that fails, the function bails
 *	out via the fallback flag and the caller does a full detoast.
 *
 *	1024 covers a typical small/medium realistic object's structure
 *	(header + ~30 JEntries + KVMap + ~200 B keys = ~500 B) plus a
 *	100-200 B value. Larger objects refetch on first miss; the
 *	cost of an extra BT-walk is paid only for those.
 */
#define JBTL_OF_INITIAL_PREFIX_BYTES		1024


/*
 * jbtl_compare_jsonb_string
 *
 *	Same ordering as core's static lengthCompareJsonbString:
 *	shorter strings sort before longer ones; equal-length compares
 *	memcmp. Reimplemented here because the core symbol is static.
 */
static inline int
jbtl_compare_jsonb_string(const char *a, int alen,
						  const char *b, int blen)
{
	if (alen == blen)
		return memcmp(a, b, alen);
	return alen > blen ? 1 : -1;
}


/*
 * jbtl_kvmap_entry
 *
 *	Read a single KVMap entry by logical index. Mirrors core's
 *	JSONB_KVMAP_ENTRY macro, but takes the raw (kvmap_ptr, entry_size)
 *	pair instead of a JsonbKVMap descriptor — we don't go through
 *	core's static initKVMap.
 */
static inline int32
jbtl_kvmap_entry(const void *kvmap_ptr, int entry_size, int index)
{
	if (entry_size == 0)
		return index;			/* identity (no-map case) */
	if (entry_size == 1)
		return ((const uint8 *) kvmap_ptr)[index];
	if (entry_size == 2)
		return ((const uint16 *) kvmap_ptr)[index];
	return ((const int32 *) kvmap_ptr)[index];
}


/*
 * jbtl_fetch_slice_dispatch
 *
 *	Dispatch a slice fetch to the right reader based on mode. Both
 *	readers return a varlena with VARHDRSZ + slice_payload_len bytes;
 *	out_chunks_total / out_chunks_fetched are populated by the reader.
 *	out_pages_touched is opt-in (probe-only): pass NULL to disable
 *	the secondary metric scan on production fast paths.
 */
static struct varlena *
jbtl_fetch_slice_dispatch(struct varatt_external *toast_pointer,
						  uint32 mode,
						  int32 sliceoffset, int32 slicelength,
						  int32 *out_chunks_total,
						  int32 *out_chunks_fetched,
						  int32 *out_pages_touched)
{
	if (mode == JBTL_POINTER)
	{
		return jbtl_toast_fetch_slice_plain(toast_pointer,
											sliceoffset, slicelength,
											out_chunks_total,
											out_chunks_fetched,
											out_pages_touched);
	}
	else if (mode == JBTL_POINTER_COMPRESSED_CHUNKS)
	{
		int32		dec_unused = 0;
		int32		bytes_unused = 0;

		return jbtl_toast_fetch_compressed_chunks(toast_pointer,
												  sliceoffset, slicelength,
												  out_chunks_total,
												  out_chunks_fetched,
												  &dec_unused,
												  out_pages_touched,
												  &bytes_unused);
	}
	else
	{
		elog(ERROR, "jbtl_fetch_slice_dispatch: unsupported mode 0x%08X", mode);
		return NULL;			/* keep compiler quiet */
	}
}


/*
 * jbtl_toast_fetch_object_field
 *	Sliced top-level object field lookup.
 *
 *	Top-level meaning: the input jsonb's root container is an object,
 *	and we look up a key directly there (no nested paths). Caller
 *	is responsible for unwrapping our custom-varlena to a
 *	(toast_pointer, mode) pair.
 *
 *	On success: returns a JsonbValue with the looked-up value
 *	(scalar types only — string, number, bool, null), populates
 *	`*out_chunks_total`, `*out_chunks_fetched` (sum across both
 *	prefix and value fetches), `*out_value_byte_offset` (offset
 *	within the body bytes), `*out_value_byte_length`, and sets
 *	`*out_fallback = false`.
 *
 *	On "key not found": returns NULL, sets `*out_fallback = false`.
 *
 *	On "use the slow path": returns NULL, sets `*out_fallback = true`.
 *	This happens for non-object roots, nested-container value types,
 *	and structurally surprising headers.
 *
 *	The fallback flag is the caller's signal to invoke
 *	jsonb_object_field_text or similar on a fully-detoasted value.
 *
 *	out_pages_touched is opt-in (probe-only). Pass NULL on
 *	production fast paths. When non-NULL, after the read
 *	completes the function does one additional cheap btree scan
 *	over the chunk range it actually touched and writes the
 *	count of distinct toast pages. See jbtl_count_pages_in_chunk_range.
 */
JsonbValue *
jbtl_toast_fetch_object_field(struct varatt_external *toast_pointer,
							  uint32 mode,
							  const char *key, int keylen,
							  int32 *out_chunks_total,
							  int32 *out_chunks_fetched,
							  int32 *out_value_byte_offset,
							  int32 *out_value_byte_length,
							  bool *out_fallback,
							  int32 *out_pages_touched)
{
	struct varlena *prefix;
	int32		attrsize = VARATT_EXTERNAL_GET_EXTSIZE(*toast_pointer);
	int32		prefix_size = JBTL_OF_INITIAL_PREFIX_BYTES;
	int32		chunks_total = 0;
	int32		chunks_fetched_first = 0;
	int32		chunks_fetched_total = 0;
	/*
	 * For pages we union across all chunks our prefix/value fetches
	 * touched. Tracked as a chunk-index range [pages_lo, pages_hi]
	 * so we can do one secondary metric scan at the end instead of
	 * three. Initialised to "no chunks touched yet."
	 */
	int32		pages_lo = INT32_MAX;
	int32		pages_hi = -1;

	char	   *body;
	int32		body_len;
	JsonbContainer *jc;
	int			N;
	bool		has_kvmap;
	int			kvmap_entry_size;
	int			min_prefix;
	int			key_area_end;
	void	   *kvmap_ptr;
	char	   *base_addr;
	int			stop_low,
				stop_high,
				found_idx;
	int32		physical_value_idx;
	JEntry		value_jentry;
	uint32		value_offset_in_data;
	uint32		value_len;
	int			value_byte_end_in_body;
	char	   *value_bytes;
	JsonbValue *result;
	int			i;

	*out_fallback = false;

	/* Step 1: fetch a prefix that covers (we hope) the structural part. */
	if (prefix_size > attrsize)
		prefix_size = attrsize;

	prefix = jbtl_fetch_slice_dispatch(toast_pointer, mode,
									   0, prefix_size,
									   &chunks_total,
									   &chunks_fetched_first,
									   NULL);
	chunks_fetched_total += chunks_fetched_first;
	if (out_pages_touched)
	{
		int32		lo = 0;
		int32		hi = (prefix_size - 1) / TOAST_MAX_CHUNK_SIZE;

		if (lo < pages_lo) pages_lo = lo;
		if (hi > pages_hi) pages_hi = hi;
	}

	body = VARDATA(prefix);
	body_len = VARSIZE(prefix) - VARHDRSZ;

	/*
	 * Step 2: parse the container header. 4 bytes minimum. If we
	 * don't even have that, give up and fall back.
	 */
	if (body_len < (int) sizeof(uint32))
	{
		*out_fallback = true;
		goto out;
	}

	jc = (JsonbContainer *) body;

	if (!JsonContainerIsObject(jc))
	{
		/* root is not an object -> out of scope */
		*out_fallback = true;
		goto out;
	}

	N = JsonContainerSize(jc);
	if (N == 0)
	{
		/* empty object -> key not found, fast path completes here */
		goto out;
	}

	has_kvmap = JsonContainerHasKVMap(jc);
	kvmap_entry_size = has_kvmap ? JSONB_KVMAP_ENTRY_SIZE(N) : 0;

	/*
	 * KVMap is INTALIGN'd in the on-disk layout (writer pads with
	 * zeros to ALIGNOF_INT after the raw kvmap_entry_size * N bytes).
	 * core's initKVMap returns INTALIGN(N * entry_size) past the
	 * JEntries. We must do the same when computing where the data
	 * area starts.
	 */
	min_prefix = (int) sizeof(uint32) + 8 * N + INTALIGN(N * kvmap_entry_size);

	/*
	 * Step 3: ensure prefix covers JEntries + KVMap. Refetch if not.
	 */
	if (body_len < min_prefix)
	{
		/* Refetch enough.  Aim for min_prefix + a generous key margin. */
		int32		better_size = min_prefix + 4096;
		int32		chunks_total_2 = 0;
		int32		chunks_fetched_2 = 0;

		if (better_size > attrsize)
			better_size = attrsize;

		pfree(prefix);

		prefix = jbtl_fetch_slice_dispatch(toast_pointer, mode,
										   0, better_size,
										   &chunks_total_2,
										   &chunks_fetched_2,
										   NULL);
		chunks_total = chunks_total_2;
		chunks_fetched_total += chunks_fetched_2;
		if (out_pages_touched)
		{
			int32		lo = 0;
			int32		hi = (better_size - 1) / TOAST_MAX_CHUNK_SIZE;

			if (lo < pages_lo) pages_lo = lo;
			if (hi > pages_hi) pages_hi = hi;
		}

		body = VARDATA(prefix);
		body_len = VARSIZE(prefix) - VARHDRSZ;
		jc = (JsonbContainer *) body;

		if (body_len < min_prefix)
		{
			/* Still not enough — give up. */
			*out_fallback = true;
			goto out;
		}
	}

	/*
	 * Step 4: ensure prefix covers the key area (sum of key lengths
	 * starting at the data area). We can compute this from JEntries.
	 */
	key_area_end = min_prefix;
	for (i = 0; i < N; i++)
		key_area_end += getJsonbLength(jc, i);

	if (body_len < key_area_end)
	{
		int32		better_size = key_area_end;
		int32		chunks_total_2 = 0;
		int32		chunks_fetched_2 = 0;

		if (better_size > attrsize)
			better_size = attrsize;

		pfree(prefix);

		prefix = jbtl_fetch_slice_dispatch(toast_pointer, mode,
										   0, better_size,
										   &chunks_total_2,
										   &chunks_fetched_2,
										   NULL);
		chunks_total = chunks_total_2;
		chunks_fetched_total += chunks_fetched_2;
		if (out_pages_touched)
		{
			int32		lo = 0;
			int32		hi = (better_size - 1) / TOAST_MAX_CHUNK_SIZE;

			if (lo < pages_lo) pages_lo = lo;
			if (hi > pages_hi) pages_hi = hi;
		}

		body = VARDATA(prefix);
		body_len = VARSIZE(prefix) - VARHDRSZ;
		jc = (JsonbContainer *) body;

		if (body_len < key_area_end)
		{
			*out_fallback = true;
			goto out;
		}
	}

	/* Step 5: binary-search for the key. */
	kvmap_ptr = has_kvmap ? body + sizeof(uint32) + 8 * N : NULL;
	base_addr = body + min_prefix;

	stop_low = 0;
	stop_high = N;
	found_idx = -1;
	while (stop_low < stop_high)
	{
		int			mid = stop_low + (stop_high - stop_low) / 2;
		const char *cand_val = base_addr + getJsonbOffset(jc, mid);
		int			cand_len = getJsonbLength(jc, mid);
		int			diff = jbtl_compare_jsonb_string(cand_val, cand_len,
													key, keylen);

		if (diff == 0)
		{
			found_idx = mid;
			break;
		}
		if (diff < 0)
			stop_low = mid + 1;
		else
			stop_high = mid;
	}

	if (found_idx < 0)
	{
		/* key not found, but fast path completed */
		goto out;
	}

	/* Step 6: KVMap-redirect to physical value index. */
	physical_value_idx =
		jbtl_kvmap_entry(kvmap_ptr, kvmap_entry_size, found_idx) + N;

	value_jentry = jc->children[physical_value_idx];
	value_offset_in_data = getJsonbOffset(jc, physical_value_idx);
	value_len = getJsonbLength(jc, physical_value_idx);

	if (out_value_byte_offset)
		*out_value_byte_offset = min_prefix + value_offset_in_data;
	if (out_value_byte_length)
		*out_value_byte_length = value_len;

	/* Step 7: bail on nested values; out of  scope. */
	if (JBE_ISCONTAINER(value_jentry))
	{
		*out_fallback = true;
		goto out;
	}

	/*
	 * Step 7b: bail when the value occupies more than ~half the body.
	 * In that case fetching only the value would still touch most
	 * chunks; doing it AFTER a structural prefix fetch costs more
	 * total chunk reads than a single full detoast would. Pure
	 * heuristic; tunable. Reading the whole body via the existing
	 * full path then doing the lookup in memory is what the caller
	 * does on fallback, and that's strictly cheaper here.
	 *
	 *	The 50 % threshold is conservative — even a 49 %-of-body
	 *	value would force the value-fetch to overlap the prefix
	 *	fetch in chunk space, where each overlap pays a duplicate
	 *	BT-walk row. If profiling later argues for a tighter
	 *	bound (say 70 %), this is the one knob to turn.
	 */
	if ((int64) value_len * 2 > (int64) attrsize)
	{
		*out_fallback = true;
		goto out;
	}

	/* Step 8: fetch the value bytes (possibly already in prefix).
	 *
	 *	Numerics and nested-container values are INTALIGN'd in the data
	 *	area: the writer pads before them so the actual payload starts
	 *	at INTALIGN(unpadded_offset). The pad bytes are NOT counted
	 *	in JEntry length. So for any value type we may need to fetch
	 *	an extra `pad` bytes at the front; in the result construction
	 *	we skip those pad bytes for types that need them. Strings and
	 *	bool/null have pad=0 because no preceding alignment is added
	 *	for them.
	 */
	{
		uint32		pad = INTALIGN(value_offset_in_data) - value_offset_in_data;
		int32		fetch_len = (int32) (value_len + pad);

		value_byte_end_in_body =
			min_prefix + (int) value_offset_in_data + fetch_len;

		if (body_len >= value_byte_end_in_body)
		{
			value_bytes = base_addr + value_offset_in_data;
		}
		else
		{
			struct varlena *value_chunk;
			int32		chunks_total_v = 0;
			int32		chunks_fetched_v = 0;

			value_chunk = jbtl_fetch_slice_dispatch(toast_pointer, mode,
													min_prefix + value_offset_in_data,
													fetch_len,
													&chunks_total_v,
													&chunks_fetched_v,
													NULL);
			chunks_fetched_total += chunks_fetched_v;
			value_bytes = VARDATA(value_chunk);
			if (out_pages_touched)
			{
				int32		val_start = min_prefix + value_offset_in_data;
				int32		lo = val_start / TOAST_MAX_CHUNK_SIZE;
				int32		hi = (val_start + fetch_len - 1) / TOAST_MAX_CHUNK_SIZE;

				if (lo < pages_lo) pages_lo = lo;
				if (hi > pages_hi) pages_hi = hi;
			}
		}

		/* Step 9: construct the JsonbValue. */
		result = (JsonbValue *) palloc(sizeof(JsonbValue));

		if (JBE_ISNULL(value_jentry))
		{
			result->type = jbvNull;
		}
		else if (JBE_ISSTRING(value_jentry))
		{
			/*
			 * Strings get no alignment padding; pad is 0 here. The
			 * JsonbValue points directly at our fetched bytes. Caller
			 * is responsible for materializing into stable memory before
			 * the prefix/value buffer goes out of scope.
			 */
			result->type = jbvString;
			result->val.string.val = value_bytes;
			result->val.string.len = value_len;
		}
		else if (JBE_ISNUMERIC(value_jentry))
		{
			/*
			 * Numerics are INTALIGN'd; skip pad bytes at the front of
			 * the fetched value buffer. pad was computed as
			 * INTALIGN(value_offset_in_data) - value_offset_in_data,
			 * so value_bytes + pad lands exactly on the Numeric struct.
			 */
			result->type = jbvNumeric;
			result->val.numeric = (Numeric) (value_bytes + pad);
		}
		else if (JBE_ISBOOL_TRUE(value_jentry))
		{
			result->type = jbvBool;
			result->val.boolean = true;
		}
		else if (JBE_ISBOOL_FALSE(value_jentry))
		{
			result->type = jbvBool;
			result->val.boolean = false;
		}
		else
		{
			/* Should be unreachable; defensive fallback. */
			*out_fallback = true;
			goto out;
		}
	}

	if (out_chunks_total)
		*out_chunks_total = chunks_total;
	if (out_chunks_fetched)
		*out_chunks_fetched = chunks_fetched_total;
	if (out_pages_touched)
	{
		if (pages_hi >= pages_lo)
			*out_pages_touched =
				jbtl_count_pages_in_chunk_range(toast_pointer,
												pages_lo, pages_hi);
		else
			*out_pages_touched = 0;
	}

	return result;

out:
	if (out_chunks_total)
		*out_chunks_total = chunks_total;
	if (out_chunks_fetched)
		*out_chunks_fetched = chunks_fetched_total;
	if (out_pages_touched)
	{
		if (pages_hi >= pages_lo)
			*out_pages_touched =
				jbtl_count_pages_in_chunk_range(toast_pointer,
												pages_lo, pages_hi);
		else
			*out_pages_touched = 0;
	}
	return NULL;
}


/*
 * jbtl_unwrap_to_toast_pointer
 *
 *	Given a possibly-CUSTOM raw varlena, decide whether we can take
 *	the fast path. On success, fills *out_mode and *out_ext and
 *	returns true. On any reason to fall back, returns false; out
 *	parameters are not modified.
 *
 *	Exported (non-static) because 's update probe in
 *	jsonb_toaster_lite_update_probe.c reuses the same locator step.
 */
bool
jbtl_unwrap_to_toast_pointer(struct varlena *raw,
							 uint32 *out_mode,
							 struct varatt_external *out_ext)
{
	uint32		mode;
	char	   *bare;

	if (!VARATT_IS_CUSTOM(raw))
		return false;

	mode = JBTL_CUSTOM_PTR_GET_HEADER(raw) & JBTL_POINTER_TYPE_MASK;

	if (mode != JBTL_POINTER && mode != JBTL_POINTER_COMPRESSED_CHUNKS)
		return false;

	bare = JBTL_CUSTOM_PTR_GET_DATA(raw);

	if (!VARATT_IS_EXTERNAL_ONDISK(bare))
		return false;

	VARATT_EXTERNAL_GET_POINTER(*out_ext, bare);
	*out_mode = mode;
	return true;
}


/*
 * jbtl_call_core_object_field
 *
 *	Call core's jsonb_object_field / jsonb_object_field_text on a
 *	fully-detoasted Jsonb, tolerating a NULL return.
 *	DirectFunctionCall2 cannot be used here because it elog()s on NULL
 *	return, but core returns SQL NULL for missing keys (->) and for
 *	JSON-null / non-scalar values (->>). We build a local
 *	FunctionCallInfo, call directly, and propagate the isnull flag.
 */
static Datum
jbtl_call_core_object_field(PGFunction core_fn, Jsonb *jb, Datum key_datum,
							bool *out_isnull)
{
	LOCAL_FCINFO(local_fcinfo, 2);
	Datum		result;

	InitFunctionCallInfoData(*local_fcinfo, NULL, 2, InvalidOid, NULL, NULL);

	local_fcinfo->args[0].value = JsonbPGetDatum(jb);
	local_fcinfo->args[0].isnull = false;
	local_fcinfo->args[1].value = key_datum;
	local_fcinfo->args[1].isnull = false;

	result = (*core_fn) (local_fcinfo);

	*out_isnull = local_fcinfo->isnull;
	return result;
}


/*
 * jbtl_object_field
 *	SQL: jbtl_object_field(jb jsonb, key text) RETURNS jsonb
 *
 *	Equivalent to `jb->key` but with KVMap-aware sliced TOAST fetch
 *	when jb is stored under jsonb_toaster_lite and its root is an
 *	object with scalar value at `key`. Falls back to core's
 *	jsonb_object_field for all other cases (nested values, non-object
 *	roots, default-toaster jsonb, inline jsonb_toaster_lite values).
 */
PG_FUNCTION_INFO_V1(jbtl_object_field);
Datum
jbtl_object_field(PG_FUNCTION_ARGS)
{
	Datum		raw_datum = PG_GETARG_DATUM(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	struct varlena *raw = (struct varlena *) DatumGetPointer(raw_datum);
	uint32		mode;
	struct varatt_external ext_ptr;
	JsonbValue *jbv;
	bool		fallback = false;

	if (!jbtl_unwrap_to_toast_pointer(raw, &mode, &ext_ptr))
	{
		/* Slow path: full detoast + core lookup. */
		Jsonb	   *jb = DatumGetJsonbP(raw_datum);

		bool		core_isnull = false;
		Datum		core_result =
			jbtl_call_core_object_field(jsonb_object_field, jb,
										PG_GETARG_DATUM(1), &core_isnull);
		if (core_isnull)
			PG_RETURN_NULL();
		return core_result;
	}

	jbv = jbtl_toast_fetch_object_field(&ext_ptr, mode,
										VARDATA_ANY(key),
										VARSIZE_ANY_EXHDR(key),
										NULL, NULL, NULL, NULL,
										&fallback,
										NULL);

	if (fallback)
	{
		Jsonb	   *jb = DatumGetJsonbP(raw_datum);

		bool		core_isnull = false;
		Datum		core_result =
			jbtl_call_core_object_field(jsonb_object_field, jb,
										PG_GETARG_DATUM(1), &core_isnull);
		if (core_isnull)
			PG_RETURN_NULL();
		return core_result;
	}

	if (jbv == NULL)
		PG_RETURN_NULL();

	PG_RETURN_JSONB_P(JsonbValueToJsonb(jbv));
}


/*
 * jbtl_object_field_text
 *	SQL: jbtl_object_field_text(jb jsonb, key text) RETURNS text
 *
 *	Equivalent to `jb->>key`. For scalar values via the fast path,
 *	formats them to text directly without round-tripping through
 *	JsonbValueToJsonb.
 */
PG_FUNCTION_INFO_V1(jbtl_object_field_text);
Datum
jbtl_object_field_text(PG_FUNCTION_ARGS)
{
	Datum		raw_datum = PG_GETARG_DATUM(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	struct varlena *raw = (struct varlena *) DatumGetPointer(raw_datum);
	uint32		mode;
	struct varatt_external ext_ptr;
	JsonbValue *jbv;
	bool		fallback = false;

	if (!jbtl_unwrap_to_toast_pointer(raw, &mode, &ext_ptr))
	{
		Jsonb	   *jb = DatumGetJsonbP(raw_datum);

		bool		core_isnull = false;
		Datum		core_result =
			jbtl_call_core_object_field(jsonb_object_field_text, jb,
										PG_GETARG_DATUM(1), &core_isnull);
		if (core_isnull)
			PG_RETURN_NULL();
		return core_result;
	}

	jbv = jbtl_toast_fetch_object_field(&ext_ptr, mode,
										VARDATA_ANY(key),
										VARSIZE_ANY_EXHDR(key),
										NULL, NULL, NULL, NULL,
										&fallback,
										NULL);

	if (fallback)
	{
		Jsonb	   *jb = DatumGetJsonbP(raw_datum);

		bool		core_isnull = false;
		Datum		core_result =
			jbtl_call_core_object_field(jsonb_object_field_text, jb,
										PG_GETARG_DATUM(1), &core_isnull);
		if (core_isnull)
			PG_RETURN_NULL();
		return core_result;
	}

	if (jbv == NULL)
		PG_RETURN_NULL();

	switch (jbv->type)
	{
		case jbvNull:
			/* JSON null is SQL NULL by ->> semantics. */
			PG_RETURN_NULL();

		case jbvString:
			PG_RETURN_TEXT_P(cstring_to_text_with_len(jbv->val.string.val,
													  jbv->val.string.len));

		case jbvNumeric:
			{
				char	   *str =
					DatumGetCString(DirectFunctionCall1(numeric_out,
														NumericGetDatum(jbv->val.numeric)));

				PG_RETURN_TEXT_P(cstring_to_text(str));
			}

		case jbvBool:
			PG_RETURN_TEXT_P(cstring_to_text(jbv->val.boolean ? "true" : "false"));

		default:
			/* Defensive: should not happen for the supported subset. */
			elog(ERROR, "jbtl_object_field_text: unexpected JsonbValue type %d",
				 (int) jbv->type);
			PG_RETURN_NULL();
	}
}


/*
 * jbtl_object_field_probe
 *	SQL: jbtl_object_field_probe(jb jsonb, key text)
 *	 RETURNS (chunks_total int, chunks_fetched int,
 *	 value_byte_offset int, value_byte_length int,
 *	 fallback bool, value_text text)
 *
 *	Test/observability helper exposing every counter the fast
 *	path produces. `value_text` is the value formatted to text (same
 *	as ->>) for non-fallback paths; NULL on fallback or key-not-found.
 *	`fallback` distinguishes "key absent" (fallback=false, value_text
 *	NULL) from "fast path declined" (fallback=true).
 */
PG_FUNCTION_INFO_V1(jbtl_object_field_probe);
Datum
jbtl_object_field_probe(PG_FUNCTION_ARGS)
{
	Datum		raw_datum = PG_GETARG_DATUM(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	struct varlena *raw = (struct varlena *) DatumGetPointer(raw_datum);
	uint32		mode;
	struct varatt_external ext_ptr;
	JsonbValue *jbv = NULL;
	bool		fallback = false;
	int32		chunks_total = 0;
	int32		chunks_fetched = 0;
	int32		toast_pages_touched = 0;
	int32		chunks_decompressed = 0;
	int32		bytes_decompressed = 0;
	int32		value_byte_offset = 0;
	int32		value_byte_length = 0;
	text	   *value_text = NULL;

	TupleDesc	tupdesc;
	Datum		values[9];
	bool		isnull[9] = {false, false, false, false, false,
							 false, false, false, true};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "function returning record called in context that cannot accept type record");
	tupdesc = BlessTupleDesc(tupdesc);

	if (!jbtl_unwrap_to_toast_pointer(raw, &mode, &ext_ptr))
	{
		fallback = true;
	}
	else
	{
		jbv = jbtl_toast_fetch_object_field(&ext_ptr, mode,
											VARDATA_ANY(key),
											VARSIZE_ANY_EXHDR(key),
											&chunks_total,
											&chunks_fetched,
											&value_byte_offset,
											&value_byte_length,
											&fallback,
											&toast_pages_touched);
	}

	/*
	 * For per-chunk-compressed mode, additionally surface decompression
	 * counts. We cannot easily get them fast path
	 * (it dispatches through jbtl_fetch_slice_dispatch which threw
	 * the per-call decompress count away). Acceptable for now:
	 * report 0 for plain, leave 0 for compressed too — the page count
	 * is the headline number. A future pass can plumb decompression
	 * counts through if needed.
	 */
	(void) chunks_decompressed;
	(void) bytes_decompressed;

	if (jbv != NULL && !fallback)
	{
		/* Format value to text. */
		switch (jbv->type)
		{
			case jbvString:
				value_text = cstring_to_text_with_len(jbv->val.string.val,
													  jbv->val.string.len);
				break;
			case jbvNumeric:
				{
					char	   *str =
						DatumGetCString(DirectFunctionCall1(numeric_out,
															NumericGetDatum(jbv->val.numeric)));

					value_text = cstring_to_text(str);
					break;
				}
			case jbvBool:
				value_text = cstring_to_text(jbv->val.boolean ? "true" : "false");
				break;
			case jbvNull:
				/* leave value_text NULL; jbvNull is JSON null */
				break;
			default:
				elog(ERROR, "jbtl_object_field_probe: unexpected JsonbValue type %d",
					 (int) jbv->type);
		}
	}

	values[0] = Int32GetDatum(chunks_total);
	values[1] = Int32GetDatum(chunks_fetched);
	values[2] = Int32GetDatum(value_byte_offset);
	values[3] = Int32GetDatum(value_byte_length);
	values[4] = BoolGetDatum(fallback);
	values[5] = Int32GetDatum(toast_pages_touched);
	values[6] = Int32GetDatum(chunks_decompressed);
	values[7] = Int32GetDatum(bytes_decompressed);

	if (value_text)
	{
		values[8] = PointerGetDatum(value_text);
		isnull[8] = false;
	}

	tuple = heap_form_tuple(tupdesc, values, isnull);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}


/*
 * jbtl_jsonb_object_field_hook_fn
 *
 *	Core dispatch-hook callback for jsonb_object_field on
 *	CUSTOM-toasted jsonb. Installed by _PG_init into
 *	Toastapi_jsonb_object_field_hook. Contract: see toast_hook.h.
 *
 *	Returns false to let core fall through to its vanilla body in
 *	the cases we cannot handle without a full detoast (unwrap
 *	rejection or fast-path-internal fallback signal). The callback
 *	must NOT call back into jsonb_object_field or any other core
 *	fallback wrapper: core handles that itself once we return false.
 */
bool
jbtl_jsonb_object_field_hook_fn(Datum raw_jb, text *key,
								bool *isnull, Datum *result)
{
	struct varlena *raw = (struct varlena *) DatumGetPointer(raw_jb);
	uint32		mode;
	struct varatt_external ext_ptr;
	JsonbValue *jbv;
	bool		fallback = false;

	/*
	 * Dispatch contract: core's jsonb_object_field gates this callback
	 * on VARATT_IS_CUSTOM(raw). Assert it here so any future caller
	 * that bypasses the dispatch site trips a debug build immediately
	 * rather than relying on jbtl_unwrap_to_toast_pointer's silent
	 * reject.
	 */
	Assert(VARATT_IS_CUSTOM(raw));

	if (!jbtl_unwrap_to_toast_pointer(raw, &mode, &ext_ptr))
		return false;

	jbv = jbtl_toast_fetch_object_field(&ext_ptr, mode,
										VARDATA_ANY(key),
										VARSIZE_ANY_EXHDR(key),
										NULL, NULL, NULL, NULL,
										&fallback,
										NULL);

	if (fallback)
		return false;

	if (jbv == NULL)
	{
		*isnull = true;
		*result = (Datum) 0;
	}
	else
	{
		*isnull = false;
		*result = PointerGetDatum(JsonbValueToJsonb(jbv));
	}
	return true;
}
