/*-------------------------------------------------------------------------
 *
 * jsonb_toaster_lite_layout_map.c
 *	  L14 layout-map probe (debug/observability only).
 *
 *	  Reports per-path physical locality classification for a fixed
 *	  set of paths the L14 headline payload contains.  Filesystem
 *	  analogy:
 *	    inline area              — root prefix on page 0
 *	    extent map               — KVMap + JEntries + offset cache
 *	    extent                   — contiguous byte range (chunk_lo..chunk_hi)
 *	    physical I/O unit        — TOAST page
 *	    fragmentation_ratio      — distinct_pages / minimum_compact_pages
 *	    class                    — inline / compact_range / large_extent
 *	                              / needs_staged / fallback
 *
 *	The probe does NOT change runtime behaviour.  It detoasts the
 *	value once into memory, walks fixed paths via core's
 *	findJsonbValueFromContainer / getJsonbOffset / getJsonbLength,
 *	then maps byte ranges to chunk and TOAST-page ranges using
 *	jbtl_count_pages_in_chunk_range.
 *
 *	Scope is intentionally fixed: 10 hardcoded paths matching the
 *	L14 generator's shape (object with key1/key2/key3/key4, where
 *	key2/key4 are flat arrays).  No path parser, no general tool.
 *	If applied to a value that does not match the L14 shape, paths
 *	that can't be resolved get class='fallback' with byte_offset=0.
 *
 * Copyright (c) 2026, Postgres Professional
 *
 * IDENTIFICATION
 *	  contrib/jsonb_toaster_lite/jsonb_toaster_lite_layout_map.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/tuplestore.h"
#include "varatt.h"

#include "varatt_custom.h"

#include "jsonb_toaster_lite.h"


/*
 * For decoding "expected layout" without calling a parser.  Each row
 * describes one path we want to interrogate, what kind of step it is
 * (top-level-object-key vs array-index-into-named-array), and which
 * outer key + index to use.
 *
 * For top-level scalar/container paths (key1, key2, key3, key4) the
 * step kind is STEP_OBJECT_KEY and arr_index is unused.
 *
 * For array-element paths (key2[N], key4[N]) the step is implicitly
 * "object-key then array-index"; arr_index is the index.
 */
typedef enum L14StepKind
{
	STEP_OBJECT_KEY,			/* top-level field of root object */
	STEP_ARRAY_ELEMENT			/* element [arr_index] of named array */
} L14StepKind;

typedef struct L14PathSpec
{
	const char *display;		/* string for the 'path' output column */
	L14StepKind kind;
	const char *key;			/* top-level key name */
	int			arr_index;		/* element index when kind == STEP_ARRAY_ELEMENT */
} L14PathSpec;


/*
 * The fixed L14 path inventory.  Order matters only for output
 * stability across runs.
 */
static const L14PathSpec l14_paths[] = {
	{"key1",         STEP_OBJECT_KEY,    "key1", 0},
	{"key3",         STEP_OBJECT_KEY,    "key3", 0},
	{"key2",         STEP_OBJECT_KEY,    "key2", 0},
	{"key4",         STEP_OBJECT_KEY,    "key4", 0},
	{"key2[0]",      STEP_ARRAY_ELEMENT, "key2", 0},
	{"key2[31]",     STEP_ARRAY_ELEMENT, "key2", 31},
	{"key2[32]",     STEP_ARRAY_ELEMENT, "key2", 32},
	{"key2[5000]",   STEP_ARRAY_ELEMENT, "key2", 5000},
	{"key2[9999]",   STEP_ARRAY_ELEMENT, "key2", 9999},
	{"key4[500]",    STEP_ARRAY_ELEMENT, "key4", 500}
};

#define L14_N_PATHS  (sizeof(l14_paths) / sizeof(l14_paths[0]))


/*
 * State accumulated during one path resolution pass.  Filled in
 * partially as we walk the path; what's filled determines the class.
 */
typedef struct L14PathResult
{
	const char *path;
	const char *kind;			/* "scalar" / "object" / "array" / "missing" */
	int32		byte_offset;	/* absolute offset within jsonb body */
	int32		byte_length;	/* encoded length of the value */
	int32		chunk_lo;
	int32		chunk_hi;
	int32		distinct_pages;
	int32		parent_metadata_pages;	/* pages required for the
										 * structural traversal up to
										 * (not including) this path's
										 * own value bytes */
	double		fragmentation_ratio;	/* distinct_pages / minimum_compact */
	const char *class_name;		/* one of inline / compact_range /
								 * large_extent / needs_staged /
								 * fallback */
	bool		fallback;		/* true if any step couldn't be resolved */
} L14PathResult;


/*
 * Helpers
 */

static int32
chunk_no_for_offset(int32 byte_offset)
{
	return byte_offset / TOAST_MAX_CHUNK_SIZE;
}

/*
 * Compute the byte offset of the data area within a JsonbContainer,
 * given the container header at `jc`.
 *
 * Object layout: [4B header][N JEntries for keys][N JEntries for values]
 *                [optional KVMap, INTALIGN'd][key area][value data area]
 * Array layout : [4B header][N JEntries for elements][value data area]
 *
 * (KVMap only exists when JB_FOBJECT_KVMAP is set, i.e. for objects only.)
 *
 * Mirrors the formula L1.4 uses for objects (jsonb_toaster_lite_object_field.c
 * around the min_prefix calculation), generalised to arrays.
 */
static int32
jbc_data_area_offset(JsonbContainer *jc)
{
	int			N = JsonContainerSize(jc);
	bool		is_object = JsonContainerIsObject(jc);
	bool		has_kvmap = JsonContainerHasKVMap(jc);
	int			n_jentries = is_object ? 2 * N : N;
	int			kvmap_entry_size = has_kvmap ? JSONB_KVMAP_ENTRY_SIZE(N) : 0;

	return (int32) (sizeof(uint32) +
					n_jentries * sizeof(JEntry) +
					INTALIGN(N * kvmap_entry_size));
}

/*
 * Sum of key-area bytes for an object container — sum of getJsonbLength
 * over the first N entries (which are keys in the JEntries layout).
 * Returns 0 for an array container (called only on objects).
 */
static int32
jbc_key_area_bytes(JsonbContainer *jc)
{
	int			N = JsonContainerSize(jc);
	int32		bytes = 0;
	int			i;

	if (!JsonContainerIsObject(jc))
		return 0;

	for (i = 0; i < N; i++)
		bytes += (int32) getJsonbLength(jc, i);

	return bytes;
}

/*
 * Classify a resolved path into one of the documented classes.  Inputs:
 *	r       — partly-filled L14PathResult (byte_offset/length/pages set,
 *	          parent_metadata_pages set)
 *	body_total_pages — total distinct pages of the document
 *
 * Rules (filesystem-language summary):
 *	- fallback        : couldn't resolve (missing key, surprising shape)
 *	- inline          : single-byte-range scalar that lives in page 0
 *	                    along with other prefix metadata
 *	- compact_range   : entire range fits in <=2 pages and value is
 *	                    not the whole document
 *	- large_extent    : range covers >50% of total pages of the doc
 *	                    or >5 pages absolute (large multi-page extent)
 *	- needs_staged    : range is small but parent_metadata_pages > 1,
 *	                    so the lookup itself touches multiple pages
 *	                    of metadata before reaching the value
 */
static const char *
classify_path(L14PathResult *r, int32 body_total_pages)
{
	if (r->fallback)
		return "fallback";

	if (r->distinct_pages == 1 && r->byte_offset < TOAST_MAX_CHUNK_SIZE * 4)
		return "inline";

	if (r->distinct_pages > body_total_pages / 2 || r->distinct_pages > 5)
		return "large_extent";

	if (r->parent_metadata_pages > 1 && r->distinct_pages <= 2)
		return "needs_staged";

	if (r->distinct_pages <= 2)
		return "compact_range";

	return "needs_staged";
}


/*
 * Resolve one path against the detoasted body, fill in the result
 * struct.  body_data points at the start of the jsonb body bytes
 * (= VARDATA of the detoasted varlena).  body_len is the body byte
 * length (not including VARHDRSZ).  ext_ptr is the bare external
 * pointer for page-counting via jbtl_count_pages_in_chunk_range
 * (or NULL if the value is inline JBTL_PLAIN_JSONB and there are no
 * toast rows).
 */
static void
resolve_l14_path(const L14PathSpec *spec, L14PathResult *out,
				 char *body_data, int32 body_len,
				 struct varatt_external *ext_ptr,
				 int32 *parent_meta_chunk_lo,
				 int32 *parent_meta_chunk_hi)
{
	JsonbContainer *root = (JsonbContainer *) body_data;
	JsonbValue	keyv;
	JsonbValue *valv;
	int32		root_data_off;
	int32		root_key_area;

	out->path = spec->display;
	out->kind = "missing";
	out->byte_offset = 0;
	out->byte_length = 0;
	out->chunk_lo = 0;
	out->chunk_hi = -1;
	out->distinct_pages = 0;
	out->parent_metadata_pages = 0;
	out->fragmentation_ratio = 0.0;
	out->fallback = false;
	out->class_name = NULL;

	/* Step 0: root must be an object. */
	if (body_len < (int32) sizeof(uint32) || !JsonContainerIsObject(root))
	{
		out->fallback = true;
		return;
	}

	/* Step 1: object lookup by key (the same first step for every L14 path). */
	keyv.type = jbvString;
	keyv.val.string.val = (char *) spec->key;
	keyv.val.string.len = strlen(spec->key);

	valv = findJsonbValueFromContainer(root, JB_FOBJECT, &keyv);
	if (valv == NULL)
	{
		out->fallback = true;
		return;
	}

	root_data_off = jbc_data_area_offset(root);
	root_key_area = jbc_key_area_bytes(root);

	if (spec->kind == STEP_OBJECT_KEY)
	{
		/*
		 * For top-level keys, valv->val points either at a scalar
		 * (Numeric/string) or at a binary subcontainer.  Its byte
		 * range is what we want.
		 */
		if (valv->type == jbvBinary)
		{
			out->kind = JsonContainerIsObject(valv->val.binary.data) ? "object"
				: JsonContainerIsArray(valv->val.binary.data) ? "array"
				: "container";
			out->byte_offset =
				(int32) ((char *) valv->val.binary.data - body_data);
			out->byte_length = valv->val.binary.len;
		}
		else if (valv->type == jbvNumeric)
		{
			out->kind = "scalar";
			out->byte_offset = (int32) ((char *) valv->val.numeric - body_data);
			out->byte_length = (int32) VARSIZE_ANY(valv->val.numeric);
		}
		else if (valv->type == jbvString)
		{
			out->kind = "scalar";
			out->byte_offset =
				(int32) ((const char *) valv->val.string.val - body_data);
			out->byte_length = valv->val.string.len;
		}
		else
		{
			/* bool/null/etc carry their state in the JEntry itself; no
			 * separate body bytes.  Mark as inline-by-construction:
			 * parent metadata is the only thing that needs reading. */
			out->kind = "scalar";
			out->byte_offset = root_data_off;	/* approximate: prefix area */
			out->byte_length = 0;
		}

		/*
		 * Parent metadata for a top-level key: the root container
		 * header + JEntries + KVMap + key area.  This is bytes
		 * [0 .. root_data_off + root_key_area).
		 */
		*parent_meta_chunk_lo = 0;
		*parent_meta_chunk_hi =
			chunk_no_for_offset(root_data_off + root_key_area - 1);
	}
	else
	{
		/*
		 * STEP_ARRAY_ELEMENT: outer value must be a binary array.
		 */
		JsonbContainer *arr;
		int			N;
		uint32		elem_off;
		uint32		elem_len;
		int32		arr_offset_in_body;
		int32		arr_data_off;

		if (valv->type != jbvBinary ||
			!JsonContainerIsArray(valv->val.binary.data))
		{
			out->fallback = true;
			return;
		}

		arr = valv->val.binary.data;
		N = JsonContainerSize(arr);

		if (spec->arr_index < 0 || spec->arr_index >= N)
		{
			out->fallback = true;
			return;
		}

		arr_offset_in_body = (int32) ((char *) arr - body_data);
		arr_data_off = jbc_data_area_offset(arr);

		elem_off = getJsonbOffset(arr, spec->arr_index);
		elem_len = getJsonbLength(arr, spec->arr_index);

		out->kind = "scalar";	/* L14 arrays hold md5 strings */
		out->byte_offset =
			arr_offset_in_body + arr_data_off + (int32) elem_off;
		out->byte_length = (int32) elem_len;

		/*
		 * Parent metadata for an array element: root metadata
		 * (header + JEntries + KVMap + key area) PLUS the array's
		 * own header + JEntries.  The array sits in the data area
		 * of the root, so its [start..data_start) is the second
		 * level of metadata.
		 */
		*parent_meta_chunk_lo = 0;
		*parent_meta_chunk_hi =
			chunk_no_for_offset(arr_offset_in_body + arr_data_off - 1);
	}

	pfree(valv);

	/* Map byte range to chunk range. */
	out->chunk_lo = chunk_no_for_offset(out->byte_offset);
	out->chunk_hi = chunk_no_for_offset(out->byte_offset + out->byte_length - 1);
	if (out->chunk_hi < out->chunk_lo)
		out->chunk_hi = out->chunk_lo;	/* zero-length value (jbvBool/etc) */

	/* Map chunk range to page count via the existing helper. */
	if (ext_ptr != NULL)
	{
		out->distinct_pages =
			jbtl_count_pages_in_chunk_range(ext_ptr,
											out->chunk_lo, out->chunk_hi);
	}
	else
	{
		out->distinct_pages = 0;	/* inline value */
	}

	/*
	 * fragmentation_ratio = distinct_pages / minimum_compact.
	 * minimum_compact = ceil(byte_length / page_size_bytes).
	 * For our purposes page_size_bytes = TOAST_MAX_CHUNK_SIZE * 4
	 * (4 chunks per page is the empirical upper bound on this build).
	 * For tiny values minimum_compact = 1 and the ratio equals
	 * distinct_pages directly.
	 */
	{
		int32		page_capacity = TOAST_MAX_CHUNK_SIZE * 4;
		int32		min_compact;

		if (out->byte_length <= 0)
			min_compact = 1;
		else
			min_compact = (out->byte_length + page_capacity - 1) / page_capacity;

		if (min_compact <= 0)
			min_compact = 1;

		if (out->distinct_pages > 0)
			out->fragmentation_ratio =
				(double) out->distinct_pages / (double) min_compact;
		else
			out->fragmentation_ratio = 0.0;
	}
}


PG_FUNCTION_INFO_V1(jbtl_l14_layout_map);

Datum
jbtl_l14_layout_map(PG_FUNCTION_ARGS)
{
	Datum		raw_datum = PG_GETARG_DATUM(0);
	struct varlena *raw = (struct varlena *) DatumGetPointer(raw_datum);
	struct varlena *body_var;
	bool		body_needs_free = false;
	char	   *body_data;
	int32		body_len;
	struct varatt_external ext_ptr_storage;
	struct varatt_external *ext_ptr_for_pages = NULL;
	int32		body_total_pages = 0;

	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	int			i;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Step 1: detoast the value into a fully-materialised varlena
	 * we can walk linearly.  If the input is a JBTL_POINTER
	 * varlena we also extract the bare external pointer for the
	 * later page-counting step.
	 */
	if (VARATT_IS_CUSTOM(raw))
	{
		uint32		mode = JBTL_CUSTOM_PTR_GET_HEADER(raw) & JBTL_POINTER_TYPE_MASK;

		if (mode == JBTL_POINTER || mode == JBTL_POINTER_COMPRESSED_CHUNKS)
		{
			char	   *bare = JBTL_CUSTOM_PTR_GET_DATA(raw);

			Assert(VARATT_IS_EXTERNAL_ONDISK(bare));
			VARATT_EXTERNAL_GET_POINTER(ext_ptr_storage, bare);
			ext_ptr_for_pages = &ext_ptr_storage;

			/* Materialise body via a normal detoast. */
			body_var = detoast_attr(raw);
			body_needs_free = true;
		}
		else if (mode == JBTL_PLAIN_JSONB)
		{
			body_var = (struct varlena *) JBTL_CUSTOM_PTR_GET_DATA(raw);
			body_needs_free = false;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("jbtl_l14_layout_map: unsupported pointer mode 0x%08X",
							mode)));
		}
	}
	else
	{
		body_var = detoast_attr(raw);
		body_needs_free = true;
	}

	body_data = VARDATA(body_var);
	body_len = (int32) (VARSIZE(body_var) - VARHDRSZ);

	/*
	 * Step 2: total distinct pages of the entire toast value, used as
	 * the denominator for the "is this a large extent?" classification.
	 */
	if (ext_ptr_for_pages != NULL)
	{
		int32		attrsize = VARATT_EXTERNAL_GET_EXTSIZE(*ext_ptr_for_pages);
		int32		chunks_total = attrsize == 0 ? 0
			: ((attrsize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;

		if (chunks_total > 0)
			body_total_pages =
				jbtl_count_pages_in_chunk_range(ext_ptr_for_pages,
												0, chunks_total - 1);
	}

	/*
	 * Step 3: walk fixed paths and emit one tuple per path.
	 */
	for (i = 0; i < (int) L14_N_PATHS; i++)
	{
		L14PathResult r;
		int32		parent_lo = 0;
		int32		parent_hi = -1;
		Datum		row_values[10];
		bool		row_isnull[10] = {false, false, false, false,
									  false, false, false, false,
									  false, false};

		resolve_l14_path(&l14_paths[i], &r, body_data, body_len,
						 ext_ptr_for_pages, &parent_lo, &parent_hi);

		if (!r.fallback && ext_ptr_for_pages != NULL && parent_hi >= parent_lo)
		{
			r.parent_metadata_pages =
				jbtl_count_pages_in_chunk_range(ext_ptr_for_pages,
												parent_lo, parent_hi);
		}

		r.class_name = classify_path(&r, body_total_pages);

		row_values[0] = PointerGetDatum(cstring_to_text(r.path));
		row_values[1] = PointerGetDatum(cstring_to_text(r.kind));
		row_values[2] = Int32GetDatum(r.byte_offset);
		row_values[3] = Int32GetDatum(r.byte_length);
		row_values[4] = Int32GetDatum(r.chunk_lo);
		row_values[5] = Int32GetDatum(r.chunk_hi);
		row_values[6] = Int32GetDatum(r.distinct_pages);
		row_values[7] = Int32GetDatum(r.parent_metadata_pages);
		row_values[8] = Float8GetDatum(r.fragmentation_ratio);
		row_values[9] = PointerGetDatum(cstring_to_text(r.class_name));

		tuplestore_putvalues(tupstore, tupdesc, row_values, row_isnull);
	}

	if (body_needs_free)
		pfree(body_var);

	return (Datum) 0;
}
