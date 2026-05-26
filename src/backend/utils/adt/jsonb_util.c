/*-------------------------------------------------------------------------
 *
 * jsonb_util.c
 *	  converting between Jsonb and JsonbValues, and iterating.
 *
 * Copyright (c) 2014-2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_util.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/toast_internals.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"

/*
 * Maximum number of elements in an array (or key/value pairs in an object).
 * This is limited by two things: the size of the JEntry array must fit
 * in MaxAllocSize, and the number of elements (or pairs) must fit in the bits
 * reserved for that in the JsonbContainer.header field.
 *
 * (The total size of an array's or object's elements is also limited by
 * JENTRY_OFFLENMASK, but we're not concerned about that here.)
 */
#define JSONB_MAX_ELEMS (Min(MaxAllocSize / sizeof(JsonbValue), JB_CMASK))
#define JSONB_MAX_PAIRS (Min(MaxAllocSize / sizeof(JsonbPair), JB_CMASK))

static void fillJsonbValue(JsonbContainer *container, int index,
						   char *base_addr, uint32 offset,
						   JsonbValue *result);
static bool equalsJsonbScalarValue(JsonbValue *a, JsonbValue *b);
static int	compareJsonbScalarValue(JsonbValue *a, JsonbValue *b);
static Jsonb *convertToJsonb(JsonbValue *val);
static void convertJsonbValue(StringInfo buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbArray(StringInfo buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbObject(StringInfo buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbScalar(StringInfo buffer, JEntry *header, JsonbValue *scalarVal);

static int	reserveFromBuffer(StringInfo buffer, int len);
static void appendToBuffer(StringInfo buffer, const void *data, int len);
static void copyToBuffer(StringInfo buffer, int offset, const void *data, int len);
static short padBufferToInt(StringInfo buffer);

/* KVMap helpers (K1.2) — used by writer and reader paths in K1.3. */
static int	estimateJsonbValueSize(const JsonbValue *jbv);
static void *initKVMap(JsonbKVMap *kvmap, void *pentries,
					   int field_count, bool sorted);
static int	int_pair_size_cmp(const void *a, const void *b);

/*
 * GUC: when true, convertJsonbObject() emits a KVMap-bearing
 * container with values sorted by estimated size (small first).
 * Default: off — physical layout is bit-for-bit identical to a
 * pre-K1 PostgreSQL until a session opts in.
 */
bool		jsonb_sort_field_values = false;

/*
 * Internal (non-GUC) override used only by the W2.x split producer.  When the
 * producer assembles its stock-layout parent it sets this for the duration of
 * that single JsonbValueToJsonb() call, so convertJsonbObject() never emits a
 * KVMap regardless of the session jsonb_sort_field_values setting.  This keeps
 * W2.x independent of the legacy K1 writer policy by construction, not merely
 * "while the GUC happens to be off".  It is always restored (PG_TRY) and is
 * never exposed to users.
 */
static bool jsonb_force_stock_layout = false;

/* W2.4 reuse instrumentation (developer scaffold; see jsonb_reuse_stats()). */
uint64		jsonb_reuse_attempts = 0;
uint64		jsonb_reuse_size_mismatch = 0;
uint64		jsonb_reuse_memcmp_match = 0;
uint64		jsonb_reuse_memcmp_mismatch = 0;
uint64		jsonb_reuse_toast_saves = 0;

static JsonbIterator *iteratorFromContainer(JsonbContainer *container, JsonbIterator *parent);
static JsonbIterator *freeAndGetParent(JsonbIterator *it);
static JsonbParseState *pushState(JsonbInState *pstate);
static void appendKey(JsonbInState *pstate, JsonbValue *string, bool needCopy);
static void appendValue(JsonbInState *pstate, JsonbValue *scalarVal, bool needCopy);
static void appendElement(JsonbInState *pstate, JsonbValue *scalarVal, bool needCopy);
static void copyScalarSubstructure(JsonbValue *v, MemoryContext outcontext);
static int	lengthCompareJsonbStringValue(const void *a, const void *b);
static int	lengthCompareJsonbString(const char *val1, int len1,
									 const char *val2, int len2);
static int	lengthCompareJsonbPair(const void *a, const void *b, void *binequal);
static void uniqueifyJsonbObject(JsonbValue *object, bool unique_keys,
								 bool skip_nulls);
static void pushJsonbValueScalar(JsonbInState *pstate,
								 JsonbIteratorToken seq,
								 JsonbValue *scalarVal);

void
JsonbToJsonbValue(Jsonb *jsonb, JsonbValue *val)
{
	val->type = jbvBinary;
	val->val.binary.data = &jsonb->root;
	val->val.binary.len = VARSIZE(jsonb) - VARHDRSZ;
}

/*
 * Turn an in-memory JsonbValue into a Jsonb for on-disk storage.
 *
 * Generally we find it more convenient to directly iterate through the Jsonb
 * representation and only really convert nested scalar values.
 * JsonbIteratorNext() does this, so that clients of the iteration code don't
 * have to directly deal with the binary representation (JsonbDeepContains() is
 * a notable exception, although all exceptions are internal to this module).
 * In general, functions that accept a JsonbValue argument are concerned with
 * the manipulation of scalar values, or simple containers of scalar values,
 * where it would be inconvenient to deal with a great amount of other state.
 */
Jsonb *
JsonbValueToJsonb(JsonbValue *val)
{
	Jsonb	   *out;

	if (IsAJsonbScalar(val))
	{
		/* Scalar value, so wrap it in an array */
		JsonbInState pstate = {0};
		JsonbValue	scalarArray;

		scalarArray.type = jbvArray;
		scalarArray.val.array.rawScalar = true;
		scalarArray.val.array.nElems = 1;

		pushJsonbValue(&pstate, WJB_BEGIN_ARRAY, &scalarArray);
		pushJsonbValue(&pstate, WJB_ELEM, val);
		pushJsonbValue(&pstate, WJB_END_ARRAY, NULL);

		out = convertToJsonb(pstate.result);
	}
	else if (val->type == jbvObject || val->type == jbvArray)
	{
		out = convertToJsonb(val);
	}
	else
	{
		Assert(val->type == jbvBinary);
		out = palloc(VARHDRSZ + val->val.binary.len);
		SET_VARSIZE(out, VARHDRSZ + val->val.binary.len);
		memcpy(VARDATA(out), val->val.binary.data, val->val.binary.len);
	}

	return out;
}

/*
 * Get the offset of the variable-length portion of a Jsonb node within
 * the variable-length-data part of its container.  The node is identified
 * by index within the container's JEntry array.
 */
uint32
getJsonbOffset(const JsonbContainer *jc, int index)
{
	uint32		offset = 0;
	int			i;

	/*
	 * Start offset of this entry is equal to the end offset of the previous
	 * entry.  Walk backwards to the most recent entry stored as an end
	 * offset, returning that offset plus any lengths in between.
	 */
	for (i = index - 1; i >= 0; i--)
	{
		offset += JBE_OFFLENFLD(jc->children[i]);
		if (JBE_HAS_OFF(jc->children[i]))
			break;
	}

	return offset;
}

/*
 * Get the length of the variable-length portion of a Jsonb node.
 * The node is identified by index within the container's JEntry array.
 */
uint32
getJsonbLength(const JsonbContainer *jc, int index)
{
	uint32		off;
	uint32		len;

	/*
	 * If the length is stored directly in the JEntry, just return it.
	 * Otherwise, get the begin offset of the entry, and subtract that from
	 * the stored end+1 offset.
	 */
	if (JBE_HAS_OFF(jc->children[index]))
	{
		off = getJsonbOffset(jc, index);
		len = JBE_OFFLENFLD(jc->children[index]) - off;
	}
	else
		len = JBE_OFFLENFLD(jc->children[index]);

	return len;
}

/*
 * BT comparator worker function.  Returns an integer less than, equal to, or
 * greater than zero, indicating whether a is less than, equal to, or greater
 * than b.  Consistent with the requirements for a B-Tree operator class
 *
 * Strings are compared lexically, in contrast with other places where we use a
 * much simpler comparator logic for searching through Strings.  Since this is
 * called from B-Tree support function 1, we're careful about not leaking
 * memory here.
 */
int
compareJsonbContainers(JsonbContainer *a, JsonbContainer *b)
{
	JsonbIterator *ita,
			   *itb;
	int			res = 0;

	ita = JsonbIteratorInit(a);
	itb = JsonbIteratorInit(b);

	do
	{
		JsonbValue	va,
					vb;
		JsonbIteratorToken ra,
					rb;

		ra = JsonbIteratorNext(&ita, &va, false);
		rb = JsonbIteratorNext(&itb, &vb, false);

		if (ra == rb)
		{
			if (ra == WJB_DONE)
			{
				/* Decisively equal */
				break;
			}

			if (ra == WJB_END_ARRAY || ra == WJB_END_OBJECT)
			{
				/*
				 * There is no array or object to compare at this stage of
				 * processing.  jbvArray/jbvObject values are compared
				 * initially, at the WJB_BEGIN_ARRAY and WJB_BEGIN_OBJECT
				 * tokens.
				 */
				continue;
			}

			if (va.type == vb.type)
			{
				switch (va.type)
				{
					case jbvString:
					case jbvNull:
					case jbvNumeric:
					case jbvBool:
						res = compareJsonbScalarValue(&va, &vb);
						break;
					case jbvArray:

						/*
						 * This could be a "raw scalar" pseudo array.  That's
						 * a special case here though, since we still want the
						 * general type-based comparisons to apply, and as far
						 * as we're concerned a pseudo array is just a scalar.
						 */
						if (va.val.array.rawScalar != vb.val.array.rawScalar)
							res = (va.val.array.rawScalar) ? -1 : 1;

						/*
						 * There should be an "else" here, to prevent us from
						 * overriding the above, but we can't change the sort
						 * order now, so there is a mild anomaly that an empty
						 * top level array sorts less than null.
						 */
						if (va.val.array.nElems != vb.val.array.nElems)
							res = (va.val.array.nElems > vb.val.array.nElems) ? 1 : -1;
						break;
					case jbvObject:
						if (va.val.object.nPairs != vb.val.object.nPairs)
							res = (va.val.object.nPairs > vb.val.object.nPairs) ? 1 : -1;
						break;
					case jbvBinary:
						elog(ERROR, "unexpected jbvBinary value");
						break;
					case jbvDatetime:
						elog(ERROR, "unexpected jbvDatetime value");
						break;
					case jbvToasted:
						/*
						 * Unreachable: JENTRY_ISTOASTED fields are materialized
						 * to their original scalar/binary value in fillJsonbValue
						 * before the iterator hands them to comparison.  Guarded
						 * defensively so a stray producer-only value can never
						 * silently mis-sort.
						 */
						elog(ERROR, "unexpected jbvToasted value");
						break;
				}
			}
			else
			{
				/* Type-defined order */
				res = (va.type > vb.type) ? 1 : -1;
			}
		}
		else
		{
			/*
			 * It's not possible for one iterator to report end of array or
			 * object while the other one reports something else, because we
			 * would have detected a length mismatch when we processed the
			 * container-start tokens above.  Likewise we can't see WJB_DONE
			 * from one but not the other.  So we have two different-type
			 * containers, or a container and some scalar type, or two
			 * different scalar types.  Sort on the basis of the type code.
			 */
			Assert(ra != WJB_DONE && ra != WJB_END_ARRAY && ra != WJB_END_OBJECT);
			Assert(rb != WJB_DONE && rb != WJB_END_ARRAY && rb != WJB_END_OBJECT);

			Assert(va.type != vb.type);
			Assert(va.type != jbvBinary);
			Assert(vb.type != jbvBinary);
			/* Type-defined order */
			res = (va.type > vb.type) ? 1 : -1;
		}
	}
	while (res == 0);

	while (ita != NULL)
	{
		JsonbIterator *i = ita->parent;

		pfree(ita);
		ita = i;
	}
	while (itb != NULL)
	{
		JsonbIterator *i = itb->parent;

		pfree(itb);
		itb = i;
	}

	return res;
}

/*
 * Find value in object (i.e. the "value" part of some key/value pair in an
 * object), or find a matching element if we're looking through an array.  Do
 * so on the basis of equality of the object keys only, or alternatively
 * element values only, with a caller-supplied value "key".  The "flags"
 * argument allows the caller to specify which container types are of interest.
 *
 * This exported utility function exists to facilitate various cases concerned
 * with "containment".  If asked to look through an object, the caller had
 * better pass a Jsonb String, because their keys can only be strings.
 * Otherwise, for an array, any type of JsonbValue will do.
 *
 * In order to proceed with the search, it is necessary for callers to have
 * both specified an interest in exactly one particular container type with an
 * appropriate flag, as well as having the pointed-to Jsonb container be of
 * one of those same container types at the top level. (Actually, we just do
 * whichever makes sense to save callers the trouble of figuring it out - at
 * most one can make sense, because the container either points to an array
 * (possibly a "raw scalar" pseudo array) or an object.)
 *
 * Note that we can return a jbvBinary JsonbValue if this is called on an
 * object, but we never do so on an array.  If the caller asks to look through
 * a container type that is not of the type pointed to by the container,
 * immediately fall through and return NULL.  If we cannot find the value,
 * return NULL.  Otherwise, return palloc()'d copy of value.
 */
JsonbValue *
findJsonbValueFromContainer(JsonbContainer *container, uint32 flags,
							JsonbValue *key)
{
	JEntry	   *children = container->children;
	int			count = JsonContainerSize(container);

	Assert((flags & ~(JB_FARRAY | JB_FOBJECT)) == 0);

	/* Quick out without a palloc cycle if object/array is empty */
	if (count <= 0)
		return NULL;

	if ((flags & JB_FARRAY) && JsonContainerIsArray(container))
	{
		JsonbValue *result = palloc_object(JsonbValue);
		char	   *base_addr = (char *) (children + count);
		uint32		offset = 0;
		int			i;

		for (i = 0; i < count; i++)
		{
			fillJsonbValue(container, i, base_addr, offset, result);

			if (key->type == result->type)
			{
				if (equalsJsonbScalarValue(key, result))
					return result;
			}

			JBE_ADVANCE_OFFSET(offset, children[i]);
		}

		pfree(result);
	}
	else if ((flags & JB_FOBJECT) && JsonContainerIsObject(container))
	{
		/* Object key passed by caller must be a string */
		Assert(key->type == jbvString);

		return getKeyJsonValueFromContainer(container, key->val.string.val,
											key->val.string.len, NULL);
	}

	/* Not found */
	return NULL;
}

/*
 * Find value by key in Jsonb object and fetch it into 'res', which is also
 * returned.
 *
 * 'res' can be passed in as NULL, in which case it's newly palloc'ed here.
 */
JsonbBoundedLookupStatus
getKeyJsonValueFromContainerBounded(const JsonbContainer *container,
									Size available_len,
									const char *keyVal, int keyLen,
									JsonbValue *res,
									JsonbBoundedLookupResult *meta)
{
	JsonbContainer *cont = unconstify(JsonbContainer *, container);
	JEntry	   *children = cont->children;
	int			count;
	char	   *baseAddr;
	bool		has_kvmap;
	int			kvmap_entry_size;
	JsonbKVMap	kvmap;
	Size		structural_end;
	uint32		stopLow,
				stopHigh;
	bool		bounded = (available_len != JSONB_AVAIL_UNBOUNDED);

	/* Need at least the container header to learn count/flags. */
	if (bounded && available_len < offsetof(JsonbContainer, children))
	{
		if (meta)
			meta->required_len = offsetof(JsonbContainer, children);
		return JSONB_BLOOKUP_NEED_MORE;
	}

	Assert(JsonContainerIsObject(container));
	count = JsonContainerSize(container);

	/* Empty object: conclusion valid from the header alone. */
	if (count <= 0)
		return JSONB_BLOOKUP_NOT_FOUND;

	has_kvmap = JsonContainerHasKVMap(container);
	kvmap_entry_size = has_kvmap ? JSONB_KVMAP_ENTRY_SIZE(count) : 0;

	/*
	 * Bound checks run only for a bounded (sliced) container.  On the unbounded
	 * hot path we add no offset walks at all: structural_end stays 0 and the
	 * NEED_MORE staircase is skipped, so the cost matches the original lookup
	 * (getJsonbOffset(count) in particular is NOT computed when unbounded).
	 */
	structural_end = 0;
	if (bounded)
	{
		Size		key_area_end;

		/* header + 2N JEntries + optional INTALIGN'd KVMap (JEntries only) */
		structural_end = offsetof(JsonbContainer, children) +
			(Size) count * 2 * sizeof(JEntry) +
			(has_kvmap ? INTALIGN((Size) count * kvmap_entry_size) : 0);
		if (available_len < structural_end)
		{
			if (meta)
				meta->required_len = structural_end;
			return JSONB_BLOOKUP_NEED_MORE;
		}

		/*
		 * Keys occupy [structural_end, structural_end + key_area_len).  The
		 * binary search may probe any key, so the whole key area must be present
		 * before comparing key bytes or concluding NOT_FOUND.
		 */
		key_area_end = structural_end + getJsonbOffset(container, count);
		if (available_len < key_area_end)
		{
			if (meta)
				meta->required_len = key_area_end;
			return JSONB_BLOOKUP_NEED_MORE;
		}
	}

	baseAddr = initKVMap(&kvmap, (char *) (children + count * 2),
						 count, has_kvmap);

	stopLow = 0;
	stopHigh = count;
	while (stopLow < stopHigh)
	{
		uint32		stopMiddle;
		int			difference;
		const char *candidateVal;
		int			candidateLen;

		stopMiddle = stopLow + (stopHigh - stopLow) / 2;

		candidateVal = baseAddr + getJsonbOffset(container, stopMiddle);
		candidateLen = getJsonbLength(container, stopMiddle);

		difference = lengthCompareJsonbString(candidateVal, candidateLen,
											  keyVal, keyLen);

		if (difference == 0)
		{
			/* Found our key; resolve the corresponding value's physical slot. */
			int			index = JSONB_KVMAP_ENTRY(&kvmap, stopMiddle) + count;
			uint32		val_off = getJsonbOffset(container, index);
			uint32		val_len = getJsonbLength(container, index);

			/*
			 * A value is COLD only if it has body bytes that fall outside the
			 * slice.  val_len is the JEntry-derived body length, so the COLD
			 * decision needs no value bytes at all.  Zero-length bodies (null,
			 * bool, empty string) read nothing and are therefore always
			 * resolvable once the metadata (keys/KVMap) is available -- hence the
			 * val_len > 0 guard (D3).
			 *
			 * cold_offset is the RAW slot offset from the container start (D4).
			 * For alignment-sensitive types (numeric, nested container) the
			 * actual body begins at INTALIGN(slot_offset); the returned
			 * [cold_offset, cold_offset + cold_len) range is a safe fetch
			 * envelope (it includes any leading alignment padding).  No separate
			 * aligned_offset is exposed: the intended COLD action is a full
			 * detoast fallback, for which the envelope is sufficient.
			 */
			if (bounded && val_len > 0 &&
				available_len < structural_end + val_off + val_len)
			{
				if (meta)
				{
					meta->cold_offset = (uint32) structural_end + val_off;
					meta->cold_len = val_len;
				}
				return JSONB_BLOOKUP_COLD;
			}

			fillJsonbValue(cont, index, baseAddr, val_off, res);
			return JSONB_BLOOKUP_FOUND;
		}
		else
		{
			if (difference < 0)
				stopLow = stopMiddle + 1;
			else
				stopHigh = stopMiddle;
		}
	}

	return JSONB_BLOOKUP_NOT_FOUND;
}

/*
 * Find value by key in Jsonb object and fetch it into 'res', which is also
 * returned.
 *
 * 'res' can be passed in as NULL, in which case it's newly palloc'ed here.
 *
 * Thin unbounded wrapper over getKeyJsonValueFromContainerBounded(): the whole
 * container is present, so the result is FOUND or NOT_FOUND only.
 */
JsonbValue *
getKeyJsonValueFromContainer(JsonbContainer *container,
							 const char *keyVal, int keyLen, JsonbValue *res)
{
	JsonbValue *target = res ? res : palloc_object(JsonbValue);
	JsonbBoundedLookupStatus st;

	st = getKeyJsonValueFromContainerBounded(container, JSONB_AVAIL_UNBOUNDED,
											 keyVal, keyLen, target, NULL);

	if (st == JSONB_BLOOKUP_FOUND)
		return target;

	if (!res)
		pfree(target);
	return NULL;
}

/*
 * Get i-th value of a Jsonb array.
 *
 * Returns palloc()'d copy of the value, or NULL if it does not exist.
 */
JsonbValue *
getIthJsonbValueFromContainer(JsonbContainer *container, uint32 i)
{
	JsonbValue *result;
	char	   *base_addr;
	uint32		nelements;

	if (!JsonContainerIsArray(container))
		elog(ERROR, "not a jsonb array");

	nelements = JsonContainerSize(container);
	base_addr = (char *) &container->children[nelements];

	if (i >= nelements)
		return NULL;

	result = palloc_object(JsonbValue);

	fillJsonbValue(container, i, base_addr,
				   getJsonbOffset(container, i),
				   result);

	return result;
}

/*
 * A helper function to fill in a JsonbValue to represent an element of an
 * array, or a key or value of an object.
 *
 * The node's JEntry is at container->children[index], and its variable-length
 * data is at base_addr + offset.  We make the caller determine the offset
 * since in many cases the caller can amortize that work across multiple
 * children.  When it can't, it can just call getJsonbOffset().
 *
 * A nested array or object will be returned as jbvBinary, ie. it won't be
 * expanded.
 */
/*
 * materializeToastedValue
 *
 * W2.1: given the value bytes of a JENTRY_ISTOASTED field (a JsonbToastedDatum
 * followed by a varatt_external), fetch the out-of-line ordinary TOAST value and
 * rebuild the original JsonbValue.  The materialized bytes are palloc'd in the
 * current memory context so they outlive the temporary detoast buffer.  This is
 * the single point where a toasted descriptor becomes a normal value; no
 * consumer above fillJsonbValue ever sees the descriptor.
 */
static void
materializeToastedValue(const char *desc_addr, JsonbValue *result)
{
	JsonbToastedDatum hdr;
	struct varatt_external toast_ptr;
	struct varlena *reconstructed;
	char		ref[VARHDRSZ_EXTERNAL + sizeof(struct varatt_external)];
	Jsonb	   *child;

	/* copy header + pointer out for aligned access */
	memcpy(&hdr, desc_addr, sizeof(JsonbToastedDatum));
	memcpy(&toast_ptr,
		   desc_addr + offsetof(JsonbToastedDatum, reserved) + sizeof(uint16),
		   sizeof(struct varatt_external));

	/* build a proper external TOAST reference varlena to hand to detoast */
	SET_VARTAG_EXTERNAL(ref, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(ref), &toast_ptr, sizeof(struct varatt_external));

	/*
	 * The out-of-line value was stored by the writer as an ordinary standalone
	 * jsonb datum (scalars are wrapped as a single-element rawScalar array, as
	 * JsonbValueToJsonb does).  Detoast it and decode uniformly: a rawScalar
	 * array yields its single element; any other container is returned as
	 * jbvBinary.  This keeps reader and writer on one representation and needs
	 * no per-type bytes interpretation.
	 */
	/* detoast_attr handles external fetch AND decompression (external_attr does
	 * not decompress). */
	reconstructed = detoast_attr((struct varlena *) ref);
	child = (Jsonb *) reconstructed;

	if (JsonContainerIsScalar(&child->root))
	{
		/* rawScalar array of one element: extract element 0 into result */
		JsonbValue *elem = getIthJsonbValueFromContainer(&child->root, 0);

		*result = *elem;
		pfree(elem);

		/*
		 * String/numeric bodies point into the detoasted buffer; copy them so
		 * they outlive it.  (Container elements cannot occur inside a rawScalar
		 * wrapper.)
		 */
		if (result->type == jbvString)
		{
			char	   *buf = palloc(result->val.string.len);

			memcpy(buf, result->val.string.val, result->val.string.len);
			result->val.string.val = buf;
		}
		else if (result->type == jbvNumeric)
		{
			int			nlen = VARSIZE(result->val.numeric);
			Numeric		buf = (Numeric) palloc(nlen);

			memcpy(buf, result->val.numeric, nlen);
			result->val.numeric = buf;
		}
	}
	else
	{
		/* nested object/array: hand back a binary view, copied to outlive buf */
		int			len = VARSIZE_ANY_EXHDR(reconstructed);
		char	   *buf = palloc(len);

		memcpy(buf, VARDATA_ANY(reconstructed), len);
		result->type = jbvBinary;
		result->val.binary.data = (JsonbContainer *) buf;
		result->val.binary.len = len;
	}

	(void) hdr;					/* orig_jbe_type retained for diagnostics only */
}

static void
fillJsonbValue(JsonbContainer *container, int index,
			   char *base_addr, uint32 offset,
			   JsonbValue *result)
{
	JEntry		entry = container->children[index];

	if (JBE_ISNULL(entry))
	{
		result->type = jbvNull;
	}
	else if (JBE_ISSTRING(entry))
	{
		result->type = jbvString;
		result->val.string.val = base_addr + offset;
		result->val.string.len = getJsonbLength(container, index);
		Assert(result->val.string.len >= 0);
	}
	else if (JBE_ISNUMERIC(entry))
	{
		result->type = jbvNumeric;
		result->val.numeric = (Numeric) (base_addr + INTALIGN(offset));
	}
	else if (JBE_ISBOOL_TRUE(entry))
	{
		result->type = jbvBool;
		result->val.boolean = true;
	}
	else if (JBE_ISBOOL_FALSE(entry))
	{
		result->type = jbvBool;
		result->val.boolean = false;
	}
	else if (JBE_ISTOASTED(entry))
	{
		/* W2.1: out-of-line cold payload; materialize lazily, invisibly. */
		materializeToastedValue(base_addr + INTALIGN(offset), result);
	}
	else
	{
		Assert(JBE_ISCONTAINER(entry));
		result->type = jbvBinary;
		/* Remove alignment padding from data pointer and length */
		result->val.binary.data = (JsonbContainer *) (base_addr + INTALIGN(offset));
		result->val.binary.len = getJsonbLength(container, index) -
			(INTALIGN(offset) - offset);
	}
}

/*
 * Push JsonbValue into JsonbInState.
 *
 * Used, for example, when parsing JSON input.
 *
 * *pstate is typically initialized to all-zeroes, except that the caller
 * may provide outcontext and/or escontext.  (escontext is ignored by this
 * function and its subroutines, however.)
 *
 * "seq" tells what is being pushed (start/end of array or object, key,
 * value, etc).  WJB_DONE is not used here, but the other values of
 * JsonbIteratorToken are.  We assume the caller passes a valid sequence
 * of values.
 *
 * The passed "jbval" is typically transient storage, such as a local variable.
 * We will copy it into the outcontext (CurrentMemoryContext by default).
 * If outcontext isn't NULL, we will also make copies of any pass-by-reference
 * scalar values.
 *
 * Only sequential tokens pertaining to non-container types should pass a
 * JsonbValue.  There is one exception -- WJB_BEGIN_ARRAY callers may pass a
 * "raw scalar" pseudo array to append it - the actual scalar should be passed
 * next and it will be added as the only member of the array.
 *
 * Values of type jbvBinary, which are rolled up arrays and objects,
 * are unpacked before being added to the result.
 *
 * At the end of construction of a JsonbValue, pstate->result will reference
 * the top-level JsonbValue object.
 */
void
pushJsonbValue(JsonbInState *pstate, JsonbIteratorToken seq,
			   JsonbValue *jbval)
{
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken tok;
	int			i;

	/*
	 * pushJsonbValueScalar handles all cases not involving pushing a
	 * container object as an ELEM or VALUE.
	 */
	if (!jbval || IsAJsonbScalar(jbval) || jbval->type == jbvToasted ||
		(seq != WJB_ELEM && seq != WJB_VALUE))
	{
		pushJsonbValueScalar(pstate, seq, jbval);
		return;
	}

	/* If an object or array is pushed, recursively push its contents */
	if (jbval->type == jbvObject)
	{
		pushJsonbValue(pstate, WJB_BEGIN_OBJECT, NULL);
		for (i = 0; i < jbval->val.object.nPairs; i++)
		{
			pushJsonbValue(pstate, WJB_KEY, &jbval->val.object.pairs[i].key);
			pushJsonbValue(pstate, WJB_VALUE, &jbval->val.object.pairs[i].value);
		}
		pushJsonbValue(pstate, WJB_END_OBJECT, NULL);
		return;
	}

	if (jbval->type == jbvArray)
	{
		pushJsonbValue(pstate, WJB_BEGIN_ARRAY, NULL);
		for (i = 0; i < jbval->val.array.nElems; i++)
		{
			pushJsonbValue(pstate, WJB_ELEM, &jbval->val.array.elems[i]);
		}
		pushJsonbValue(pstate, WJB_END_ARRAY, NULL);
		return;
	}

	/* Else it must be a jbvBinary value; push its contents */
	Assert(jbval->type == jbvBinary);

	it = JsonbIteratorInit(jbval->val.binary.data);

	/* ... with a special case for pushing a raw scalar */
	if ((jbval->val.binary.data->header & JB_FSCALAR) &&
		pstate->parseState != NULL)
	{
		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_BEGIN_ARRAY);
		Assert(v.type == jbvArray && v.val.array.rawScalar);

		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_ELEM);

		pushJsonbValueScalar(pstate, seq, &v);

		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_END_ARRAY);
		Assert(it == NULL);

		return;
	}

	while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
		pushJsonbValueScalar(pstate, tok,
							 tok < WJB_BEGIN_ARRAY ||
							 (tok == WJB_BEGIN_ARRAY &&
							  v.val.array.rawScalar) ? &v : NULL);
}

/*
 * Do the actual pushing, with only scalar or pseudo-scalar-array values
 * accepted.
 */
static void
pushJsonbValueScalar(JsonbInState *pstate, JsonbIteratorToken seq,
					 JsonbValue *scalarVal)
{
	JsonbParseState *ppstate;
	JsonbValue *val;
	MemoryContext outcontext;

	switch (seq)
	{
		case WJB_BEGIN_ARRAY:
			Assert(!scalarVal || scalarVal->val.array.rawScalar);
			ppstate = pushState(pstate);
			val = &ppstate->contVal;
			val->type = jbvArray;
			val->val.array.nElems = 0;
			val->val.array.rawScalar = (scalarVal &&
										scalarVal->val.array.rawScalar);
			if (scalarVal && scalarVal->val.array.nElems > 0)
			{
				/* Assume that this array is still really a scalar */
				Assert(scalarVal->type == jbvArray);
				ppstate->size = scalarVal->val.array.nElems;
			}
			else
			{
				ppstate->size = 4;	/* initial guess at array size */
			}
			outcontext = pstate->outcontext ? pstate->outcontext : CurrentMemoryContext;
			val->val.array.elems = MemoryContextAlloc(outcontext,
													  sizeof(JsonbValue) *
													  ppstate->size);
			break;
		case WJB_BEGIN_OBJECT:
			Assert(!scalarVal);
			ppstate = pushState(pstate);
			val = &ppstate->contVal;
			val->type = jbvObject;
			val->val.object.nPairs = 0;
			ppstate->size = 4;	/* initial guess at object size */
			outcontext = pstate->outcontext ? pstate->outcontext : CurrentMemoryContext;
			val->val.object.pairs = MemoryContextAlloc(outcontext,
													   sizeof(JsonbPair) *
													   ppstate->size);
			break;
		case WJB_KEY:
			Assert(scalarVal->type == jbvString);
			appendKey(pstate, scalarVal, true);
			break;
		case WJB_VALUE:
			Assert(IsAJsonbScalar(scalarVal) || scalarVal->type == jbvToasted);
			appendValue(pstate, scalarVal, true);
			break;
		case WJB_ELEM:
			Assert(IsAJsonbScalar(scalarVal) || scalarVal->type == jbvToasted);
			appendElement(pstate, scalarVal, true);
			break;
		case WJB_END_OBJECT:
			ppstate = pstate->parseState;
			uniqueifyJsonbObject(&ppstate->contVal,
								 ppstate->unique_keys,
								 ppstate->skip_nulls);
			pg_fallthrough;
		case WJB_END_ARRAY:
			/* Steps here common to WJB_END_OBJECT case */
			Assert(!scalarVal);
			ppstate = pstate->parseState;
			val = &ppstate->contVal;

			/*
			 * Pop stack and push current array/object as value in parent
			 * array/object, or return it as the final result.  We don't need
			 * to re-copy any scalars that are in the data structure.
			 */
			pstate->parseState = ppstate = ppstate->next;
			if (ppstate)
			{
				switch (ppstate->contVal.type)
				{
					case jbvArray:
						appendElement(pstate, val, false);
						break;
					case jbvObject:
						appendValue(pstate, val, false);
						break;
					default:
						elog(ERROR, "invalid jsonb container type");
				}
			}
			else
				pstate->result = val;
			break;
		default:
			elog(ERROR, "unrecognized jsonb sequential processing token");
	}
}

/*
 * Push a new JsonbParseState onto the JsonbInState's stack
 *
 * As a notational convenience, the new state's address is returned.
 * The caller must initialize the new state's contVal and size fields.
 */
static JsonbParseState *
pushState(JsonbInState *pstate)
{
	MemoryContext outcontext = pstate->outcontext ? pstate->outcontext : CurrentMemoryContext;
	JsonbParseState *ns = MemoryContextAlloc(outcontext,
											 sizeof(JsonbParseState));

	ns->next = pstate->parseState;
	/* This module never changes these fields, but callers can: */
	ns->unique_keys = false;
	ns->skip_nulls = false;

	pstate->parseState = ns;
	return ns;
}

/*
 * pushJsonbValue() worker:  Append a pair key to pstate
 */
static void
appendKey(JsonbInState *pstate, JsonbValue *string, bool needCopy)
{
	JsonbParseState *ppstate = pstate->parseState;
	JsonbValue *object = &ppstate->contVal;
	JsonbPair  *pair;

	Assert(object->type == jbvObject);
	Assert(string->type == jbvString);

	if (object->val.object.nPairs >= ppstate->size)
	{
		if (unlikely(object->val.object.nPairs >= JSONB_MAX_PAIRS))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of jsonb object pairs exceeds the maximum allowed (%zu)",
							JSONB_MAX_PAIRS)));
		ppstate->size = Min(ppstate->size * 2, JSONB_MAX_PAIRS);
		object->val.object.pairs = repalloc(object->val.object.pairs,
											sizeof(JsonbPair) * ppstate->size);
	}

	pair = &object->val.object.pairs[object->val.object.nPairs];
	pair->key = *string;
	pair->order = object->val.object.nPairs;

	if (needCopy)
		copyScalarSubstructure(&pair->key, pstate->outcontext);
}

/*
 * pushJsonbValue() worker:  Append a pair value to pstate
 */
static void
appendValue(JsonbInState *pstate, JsonbValue *scalarVal, bool needCopy)
{
	JsonbValue *object = &pstate->parseState->contVal;
	JsonbPair  *pair;

	Assert(object->type == jbvObject);

	pair = &object->val.object.pairs[object->val.object.nPairs];
	pair->value = *scalarVal;
	object->val.object.nPairs++;

	if (needCopy)
		copyScalarSubstructure(&pair->value, pstate->outcontext);
}

/*
 * pushJsonbValue() worker:  Append an array element to pstate
 */
static void
appendElement(JsonbInState *pstate, JsonbValue *scalarVal, bool needCopy)
{
	JsonbParseState *ppstate = pstate->parseState;
	JsonbValue *array = &ppstate->contVal;
	JsonbValue *elem;

	Assert(array->type == jbvArray);

	if (array->val.array.nElems >= ppstate->size)
	{
		if (unlikely(array->val.array.nElems >= JSONB_MAX_ELEMS))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of jsonb array elements exceeds the maximum allowed (%zu)",
							JSONB_MAX_ELEMS)));
		ppstate->size = Min(ppstate->size * 2, JSONB_MAX_ELEMS);
		array->val.array.elems = repalloc(array->val.array.elems,
										  sizeof(JsonbValue) * ppstate->size);
	}

	elem = &array->val.array.elems[array->val.array.nElems];
	*elem = *scalarVal;
	array->val.array.nElems++;

	if (needCopy)
		copyScalarSubstructure(elem, pstate->outcontext);
}

/*
 * Copy any infrastructure of a scalar JsonbValue into the outcontext,
 * adjusting the pointer(s) in *v.
 *
 * We need not deal with containers here, as the routines above ensure
 * that they are built fresh.
 */
static void
copyScalarSubstructure(JsonbValue *v, MemoryContext outcontext)
{
	MemoryContext oldcontext;

	/* Nothing to do if caller did not specify an outcontext */
	if (outcontext == NULL)
		return;
	switch (v->type)
	{
		case jbvNull:
		case jbvBool:
			/* pass-by-value, nothing to do */
			break;
		case jbvString:
			{
				char	   *buf = MemoryContextAlloc(outcontext,
													 v->val.string.len);

				memcpy(buf, v->val.string.val, v->val.string.len);
				v->val.string.val = buf;
			}
			break;
		case jbvToasted:
			{
				char	   *buf = MemoryContextAlloc(outcontext,
													 v->val.toasted.len);

				memcpy(buf, v->val.toasted.data, v->val.toasted.len);
				v->val.toasted.data = buf;
			}
			break;
		case jbvNumeric:
			oldcontext = MemoryContextSwitchTo(outcontext);
			v->val.numeric =
				DatumGetNumeric(datumCopy(NumericGetDatum(v->val.numeric),
										  false, -1));
			MemoryContextSwitchTo(oldcontext);
			break;
		case jbvDatetime:
			switch (v->val.datetime.typid)
			{
				case DATEOID:
				case TIMEOID:
				case TIMESTAMPOID:
				case TIMESTAMPTZOID:
					/* pass-by-value, nothing to do */
					break;
				case TIMETZOID:
					/* pass-by-reference */
					oldcontext = MemoryContextSwitchTo(outcontext);
					v->val.datetime.value = datumCopy(v->val.datetime.value,
													  false, TIMETZ_TYPLEN);
					MemoryContextSwitchTo(oldcontext);
					break;
				default:
					elog(ERROR, "unexpected jsonb datetime type oid %u",
						 v->val.datetime.typid);
			}
			break;
		default:
			elog(ERROR, "invalid jsonb scalar type");
	}
}

/*
 * Given a JsonbContainer, expand to JsonbIterator to iterate over items
 * fully expanded to in-memory representation for manipulation.
 *
 * See JsonbIteratorNext() for notes on memory management.
 */
JsonbIterator *
JsonbIteratorInit(JsonbContainer *container)
{
	return iteratorFromContainer(container, NULL);
}

/*
 * Get next JsonbValue while iterating
 *
 * Caller should initially pass their own, original iterator.  They may get
 * back a child iterator palloc()'d here instead.  The function can be relied
 * on to free those child iterators, lest the memory allocated for highly
 * nested objects become unreasonable, but only if callers don't end iteration
 * early (by breaking upon having found something in a search, for example).
 *
 * Callers in such a scenario, that are particularly sensitive to leaking
 * memory in a long-lived context may walk the ancestral tree from the final
 * iterator we left them with to its oldest ancestor, pfree()ing as they go.
 * They do not have to free any other memory previously allocated for iterators
 * but not accessible as direct ancestors of the iterator they're last passed
 * back.
 *
 * Returns "Jsonb sequential processing" token value.  Iterator "state"
 * reflects the current stage of the process in a less granular fashion, and is
 * mostly used here to track things internally with respect to particular
 * iterators.
 *
 * Clients of this function should not have to handle any jbvBinary values
 * (since recursive calls will deal with this), provided skipNested is false.
 * It is our job to expand the jbvBinary representation without bothering them
 * with it.  However, clients should not take it upon themselves to touch array
 * or Object element/pair buffers, since their element/pair pointers are
 * garbage.
 *
 * *val is not meaningful when the result is WJB_DONE, WJB_END_ARRAY or
 * WJB_END_OBJECT.  However, we set val->type = jbvNull in those cases,
 * so that callers may assume that val->type is always well-defined.
 */
JsonbIteratorToken
JsonbIteratorNext(JsonbIterator **it, JsonbValue *val, bool skipNested)
{
	if (*it == NULL)
	{
		val->type = jbvNull;
		return WJB_DONE;
	}

	/*
	 * When stepping into a nested container, we jump back here to start
	 * processing the child. We will not recurse further in one call, because
	 * processing the child will always begin in JBI_ARRAY_START or
	 * JBI_OBJECT_START state.
	 */
recurse:
	switch ((*it)->state)
	{
		case JBI_ARRAY_START:
			/* Set v to array on first array call */
			val->type = jbvArray;
			val->val.array.nElems = (*it)->nElems;

			/*
			 * v->val.array.elems is not actually set, because we aren't doing
			 * a full conversion
			 */
			val->val.array.rawScalar = (*it)->isScalar;
			(*it)->curIndex = 0;
			(*it)->curDataOffset = 0;
			(*it)->curValueOffset = 0;	/* not actually used */
			/* Set state for next call */
			(*it)->state = JBI_ARRAY_ELEM;
			return WJB_BEGIN_ARRAY;

		case JBI_ARRAY_ELEM:
			if ((*it)->curIndex >= (*it)->nElems)
			{
				/*
				 * All elements within array already processed.  Report this
				 * to caller, and give it back original parent iterator (which
				 * independently tracks iteration progress at its level of
				 * nesting).
				 */
				*it = freeAndGetParent(*it);
				val->type = jbvNull;
				return WJB_END_ARRAY;
			}

			fillJsonbValue((*it)->container, (*it)->curIndex,
						   (*it)->dataProper, (*it)->curDataOffset,
						   val);

			JBE_ADVANCE_OFFSET((*it)->curDataOffset,
							   (*it)->children[(*it)->curIndex]);
			(*it)->curIndex++;

			if (!IsAJsonbScalar(val) && !skipNested)
			{
				/* Recurse into container. */
				*it = iteratorFromContainer(val->val.binary.data, *it);
				goto recurse;
			}
			else
			{
				/*
				 * Scalar item in array, or a container and caller didn't want
				 * us to recurse into it.
				 */
				return WJB_ELEM;
			}

		case JBI_OBJECT_START:
			/* Set v to object on first object call */
			val->type = jbvObject;
			val->val.object.nPairs = (*it)->nElems;

			/*
			 * v->val.object.pairs is not actually set, because we aren't
			 * doing a full conversion
			 */
			(*it)->curIndex = 0;
			(*it)->curDataOffset = 0;

			/*
			 * For KVMap-less objects, value JEntries follow the key JEntries
			 * one-for-one in physical order, so we can amortize value-offset
			 * computation by initializing curValueOffset to the first value's
			 * offset and JBE_ADVANCE_OFFSET-ing it later.  For KVMap objects
			 * the physical value order does not match the logical key order;
			 * we resolve each value's offset by getJsonbOffset() on demand
			 * (see JBI_OBJECT_VALUE) and curValueOffset is not used.
			 */
			if (JsonContainerHasKVMap((*it)->container))
				(*it)->curValueOffset = 0;	/* unused */
			else
				(*it)->curValueOffset = getJsonbOffset((*it)->container,
													   (*it)->nElems);
			/* Set state for next call */
			(*it)->state = JBI_OBJECT_KEY;
			return WJB_BEGIN_OBJECT;

		case JBI_OBJECT_KEY:
			if ((*it)->curIndex >= (*it)->nElems)
			{
				/*
				 * All pairs within object already processed.  Report this to
				 * caller, and give it back original containing iterator
				 * (which independently tracks iteration progress at its level
				 * of nesting).
				 */
				*it = freeAndGetParent(*it);
				val->type = jbvNull;
				return WJB_END_OBJECT;
			}
			else
			{
				/* Return key of a key/value pair.  */
				fillJsonbValue((*it)->container, (*it)->curIndex,
							   (*it)->dataProper, (*it)->curDataOffset,
							   val);
				if (val->type != jbvString)
					elog(ERROR, "unexpected jsonb type as object key");

				/* Set state for next call */
				(*it)->state = JBI_OBJECT_VALUE;
				return WJB_KEY;
			}

		case JBI_OBJECT_VALUE:
			{
				/*
				 * Map logical key index to physical value JEntry index.
				 * For KVMap-less objects the kvmap descriptor's entry_size
				 * is zero and JSONB_KVMAP_ENTRY returns curIndex unchanged,
				 * yielding the original `curIndex + nElems` behaviour at no
				 * extra cost.
				 */
				int			value_index =
					JSONB_KVMAP_ENTRY(&(*it)->kvmap, (*it)->curIndex)
					+ (*it)->nElems;
				uint32		value_offset;

				/* Set state for next call */
				(*it)->state = JBI_OBJECT_KEY;

				if ((*it)->kvmap.entry_size)
					value_offset = getJsonbOffset((*it)->container, value_index);
				else
					value_offset = (*it)->curValueOffset;

				fillJsonbValue((*it)->container, value_index,
							   (*it)->dataProper, value_offset,
							   val);

				JBE_ADVANCE_OFFSET((*it)->curDataOffset,
								   (*it)->children[(*it)->curIndex]);
				if (!(*it)->kvmap.entry_size)
					JBE_ADVANCE_OFFSET((*it)->curValueOffset,
									   (*it)->children[(*it)->curIndex + (*it)->nElems]);
				(*it)->curIndex++;

				/*
				 * Value may be a container, in which case we recurse with new,
				 * child iterator (unless the caller asked not to, by passing
				 * skipNested).
				 */
				if (!IsAJsonbScalar(val) && !skipNested)
				{
					*it = iteratorFromContainer(val->val.binary.data, *it);
					goto recurse;
				}
				else
					return WJB_VALUE;
			}
	}

	elog(ERROR, "invalid jsonb iterator state");
	/* satisfy compilers that don't know that elog(ERROR) doesn't return */
	val->type = jbvNull;
	return WJB_DONE;
}

/*
 * Initialize an iterator for iterating all elements in a container.
 */
static JsonbIterator *
iteratorFromContainer(JsonbContainer *container, JsonbIterator *parent)
{
	JsonbIterator *it;

	it = palloc0_object(JsonbIterator);
	it->container = container;
	it->parent = parent;
	it->nElems = JsonContainerSize(container);

	/* Array starts just after header */
	it->children = container->children;

	switch (container->header & (JB_FARRAY | JB_FOBJECT))
	{
		case JB_FARRAY:
			it->dataProper =
				(char *) it->children + it->nElems * sizeof(JEntry);
			it->isScalar = JsonContainerIsScalar(container);
			/* This is either a "raw scalar", or an array */
			Assert(!it->isScalar || it->nElems == 1);

			/* Arrays carry no KVMap; identity mapping. */
			initKVMap(&it->kvmap, NULL, 0, false);

			it->state = JBI_ARRAY_START;
			break;

		case JB_FOBJECT:
			{
				/*
				 * For an object, dataProper sits past the 2N JEntries and,
				 * if JB_FOBJECT_KVMAP is set, past an INTALIGN'd KVMap of
				 * `kvmap_entry_size` bytes per pair.  initKVMap returns the
				 * pointer to the first byte of the keys/values data area.
				 */
				bool		has_kvmap = JsonContainerHasKVMap(container);
				char	   *after_jentries =
					(char *) it->children + it->nElems * sizeof(JEntry) * 2;

				it->dataProper = initKVMap(&it->kvmap, after_jentries,
										   it->nElems, has_kvmap);
				it->state = JBI_OBJECT_START;
				break;
			}

		default:
			elog(ERROR, "unknown type of jsonb container");
	}

	return it;
}

/*
 * JsonbIteratorNext() worker:	Return parent, while freeing memory for current
 * iterator
 */
static JsonbIterator *
freeAndGetParent(JsonbIterator *it)
{
	JsonbIterator *v = it->parent;

	pfree(it);
	return v;
}

/*
 * Worker for "contains" operator's function
 *
 * Formally speaking, containment is top-down, unordered subtree isomorphism.
 *
 * Takes iterators that belong to some container type.  These iterators
 * "belong" to those values in the sense that they've just been initialized in
 * respect of them by the caller (perhaps in a nested fashion).
 *
 * "val" is lhs Jsonb, and mContained is rhs Jsonb when called from top level.
 * We determine if mContained is contained within val.
 */
bool
JsonbDeepContains(JsonbIterator **val, JsonbIterator **mContained)
{
	JsonbValue	vval,
				vcontained;
	JsonbIteratorToken rval,
				rcont;

	/*
	 * Guard against stack overflow due to overly complex Jsonb.
	 *
	 * Functions called here independently take this precaution, but that
	 * might not be sufficient since this is also a recursive function.
	 */
	check_stack_depth();

	rval = JsonbIteratorNext(val, &vval, false);
	rcont = JsonbIteratorNext(mContained, &vcontained, false);

	if (rval != rcont)
	{
		/*
		 * The differing return values can immediately be taken as indicating
		 * two differing container types at this nesting level, which is
		 * sufficient reason to give up entirely (but it should be the case
		 * that they're both some container type).
		 */
		Assert(rval == WJB_BEGIN_OBJECT || rval == WJB_BEGIN_ARRAY);
		Assert(rcont == WJB_BEGIN_OBJECT || rcont == WJB_BEGIN_ARRAY);
		return false;
	}
	else if (rcont == WJB_BEGIN_OBJECT)
	{
		Assert(vval.type == jbvObject);
		Assert(vcontained.type == jbvObject);

		/*
		 * If the lhs has fewer pairs than the rhs, it can't possibly contain
		 * the rhs.  (This conclusion is safe only because we de-duplicate
		 * keys in all Jsonb objects; thus there can be no corresponding
		 * optimization in the array case.)  The case probably won't arise
		 * often, but since it's such a cheap check we may as well make it.
		 */
		if (vval.val.object.nPairs < vcontained.val.object.nPairs)
			return false;

		/* Work through rhs "is it contained within?" object */
		for (;;)
		{
			JsonbValue *lhsVal; /* lhsVal is from pair in lhs object */
			JsonbValue	lhsValBuf;

			rcont = JsonbIteratorNext(mContained, &vcontained, false);

			/*
			 * When we get through caller's rhs "is it contained within?"
			 * object without failing to find one of its values, it's
			 * contained.
			 */
			if (rcont == WJB_END_OBJECT)
				return true;

			Assert(rcont == WJB_KEY);
			Assert(vcontained.type == jbvString);

			/* First, find value by key... */
			lhsVal =
				getKeyJsonValueFromContainer((*val)->container,
											 vcontained.val.string.val,
											 vcontained.val.string.len,
											 &lhsValBuf);
			if (!lhsVal)
				return false;

			/*
			 * ...at this stage it is apparent that there is at least a key
			 * match for this rhs pair.
			 */
			rcont = JsonbIteratorNext(mContained, &vcontained, true);

			Assert(rcont == WJB_VALUE);

			/*
			 * Compare rhs pair's value with lhs pair's value just found using
			 * key
			 */
			if (lhsVal->type != vcontained.type)
			{
				return false;
			}
			else if (IsAJsonbScalar(lhsVal))
			{
				if (!equalsJsonbScalarValue(lhsVal, &vcontained))
					return false;
			}
			else
			{
				/* Nested container value (object or array) */
				JsonbIterator *nestval,
						   *nestContained;

				Assert(lhsVal->type == jbvBinary);
				Assert(vcontained.type == jbvBinary);

				nestval = JsonbIteratorInit(lhsVal->val.binary.data);
				nestContained = JsonbIteratorInit(vcontained.val.binary.data);

				/*
				 * Match "value" side of rhs datum object's pair recursively.
				 * It's a nested structure.
				 *
				 * Note that nesting still has to "match up" at the right
				 * nesting sub-levels.  However, there need only be zero or
				 * more matching pairs (or elements) at each nesting level
				 * (provided the *rhs* pairs/elements *all* match on each
				 * level), which enables searching nested structures for a
				 * single String or other primitive type sub-datum quite
				 * effectively (provided the user constructed the rhs nested
				 * structure such that we "know where to look").
				 *
				 * In other words, the mapping of container nodes in the rhs
				 * "vcontained" Jsonb to internal nodes on the lhs is
				 * injective, and parent-child edges on the rhs must be mapped
				 * to parent-child edges on the lhs to satisfy the condition
				 * of containment (plus of course the mapped nodes must be
				 * equal).
				 */
				if (!JsonbDeepContains(&nestval, &nestContained))
					return false;
			}
		}
	}
	else if (rcont == WJB_BEGIN_ARRAY)
	{
		JsonbValue *lhsConts = NULL;
		uint32		nLhsElems = vval.val.array.nElems;

		Assert(vval.type == jbvArray);
		Assert(vcontained.type == jbvArray);

		/*
		 * Handle distinction between "raw scalar" pseudo arrays, and real
		 * arrays.
		 *
		 * A raw scalar may contain another raw scalar, and an array may
		 * contain a raw scalar, but a raw scalar may not contain an array. We
		 * don't do something like this for the object case, since objects can
		 * only contain pairs, never raw scalars (a pair is represented by an
		 * rhs object argument with a single contained pair).
		 */
		if (vval.val.array.rawScalar && !vcontained.val.array.rawScalar)
			return false;

		/* Work through rhs "is it contained within?" array */
		for (;;)
		{
			rcont = JsonbIteratorNext(mContained, &vcontained, true);

			/*
			 * When we get through caller's rhs "is it contained within?"
			 * array without failing to find one of its values, it's
			 * contained.
			 */
			if (rcont == WJB_END_ARRAY)
				return true;

			Assert(rcont == WJB_ELEM);

			if (IsAJsonbScalar(&vcontained))
			{
				if (!findJsonbValueFromContainer((*val)->container,
												 JB_FARRAY,
												 &vcontained))
					return false;
			}
			else
			{
				uint32		i;

				/*
				 * If this is first container found in rhs array (at this
				 * depth), initialize temp lhs array of containers
				 */
				if (lhsConts == NULL)
				{
					uint32		j = 0;

					/* Make room for all possible values */
					lhsConts = palloc_array(JsonbValue, nLhsElems);

					for (i = 0; i < nLhsElems; i++)
					{
						/* Store all lhs elements in temp array */
						rcont = JsonbIteratorNext(val, &vval, true);
						Assert(rcont == WJB_ELEM);

						if (vval.type == jbvBinary)
							lhsConts[j++] = vval;
					}

					/* No container elements in temp array, so give up now */
					if (j == 0)
						return false;

					/* We may have only partially filled array */
					nLhsElems = j;
				}

				/* XXX: Nested array containment is O(N^2) */
				for (i = 0; i < nLhsElems; i++)
				{
					/* Nested container value (object or array) */
					JsonbIterator *nestval,
							   *nestContained;
					bool		contains;

					nestval = JsonbIteratorInit(lhsConts[i].val.binary.data);
					nestContained = JsonbIteratorInit(vcontained.val.binary.data);

					contains = JsonbDeepContains(&nestval, &nestContained);

					if (nestval)
						pfree(nestval);
					if (nestContained)
						pfree(nestContained);
					if (contains)
						break;
				}

				/*
				 * Report rhs container value is not contained if couldn't
				 * match rhs container to *some* lhs cont
				 */
				if (i == nLhsElems)
					return false;
			}
		}
	}
	else
	{
		elog(ERROR, "invalid jsonb container type");
	}

	elog(ERROR, "unexpectedly fell off end of jsonb container");
	return false;
}

/*
 * Hash a JsonbValue scalar value, mixing the hash value into an existing
 * hash provided by the caller.
 *
 * Some callers may wish to independently XOR in JB_FOBJECT and JB_FARRAY
 * flags.
 */
void
JsonbHashScalarValue(const JsonbValue *scalarVal, uint32 *hash)
{
	uint32		tmp;

	/* Compute hash value for scalarVal */
	switch (scalarVal->type)
	{
		case jbvNull:
			tmp = 0x01;
			break;
		case jbvString:
			tmp = DatumGetUInt32(hash_any((const unsigned char *) scalarVal->val.string.val,
										  scalarVal->val.string.len));
			break;
		case jbvNumeric:
			/* Must hash equal numerics to equal hash codes */
			tmp = DatumGetUInt32(DirectFunctionCall1(hash_numeric,
													 NumericGetDatum(scalarVal->val.numeric)));
			break;
		case jbvBool:
			tmp = scalarVal->val.boolean ? 0x02 : 0x04;

			break;
		default:
			elog(ERROR, "invalid jsonb scalar type");
			tmp = 0;			/* keep compiler quiet */
			break;
	}

	/*
	 * Combine hash values of successive keys, values and elements by rotating
	 * the previous value left 1 bit, then XOR'ing in the new
	 * key/value/element's hash value.
	 */
	*hash = pg_rotate_left32(*hash, 1);
	*hash ^= tmp;
}

/*
 * Hash a value to a 64-bit value, with a seed. Otherwise, similar to
 * JsonbHashScalarValue.
 */
void
JsonbHashScalarValueExtended(const JsonbValue *scalarVal, uint64 *hash,
							 uint64 seed)
{
	uint64		tmp;

	switch (scalarVal->type)
	{
		case jbvNull:
			tmp = seed + 0x01;
			break;
		case jbvString:
			tmp = DatumGetUInt64(hash_any_extended((const unsigned char *) scalarVal->val.string.val,
												   scalarVal->val.string.len,
												   seed));
			break;
		case jbvNumeric:
			tmp = DatumGetUInt64(DirectFunctionCall2(hash_numeric_extended,
													 NumericGetDatum(scalarVal->val.numeric),
													 UInt64GetDatum(seed)));
			break;
		case jbvBool:
			if (seed)
				tmp = DatumGetUInt64(DirectFunctionCall2(hashcharextended,
														 BoolGetDatum(scalarVal->val.boolean),
														 UInt64GetDatum(seed)));
			else
				tmp = scalarVal->val.boolean ? 0x02 : 0x04;

			break;
		default:
			elog(ERROR, "invalid jsonb scalar type");
			break;
	}

	*hash = ROTATE_HIGH_AND_LOW_32BITS(*hash);
	*hash ^= tmp;
}

/*
 * Are two scalar JsonbValues of the same type a and b equal?
 */
static bool
equalsJsonbScalarValue(JsonbValue *a, JsonbValue *b)
{
	if (a->type == b->type)
	{
		switch (a->type)
		{
			case jbvNull:
				return true;
			case jbvString:
				return lengthCompareJsonbStringValue(a, b) == 0;
			case jbvNumeric:
				return DatumGetBool(DirectFunctionCall2(numeric_eq,
														PointerGetDatum(a->val.numeric),
														PointerGetDatum(b->val.numeric)));
			case jbvBool:
				return a->val.boolean == b->val.boolean;

			default:
				elog(ERROR, "invalid jsonb scalar type");
		}
	}
	elog(ERROR, "jsonb scalar type mismatch");
	return false;
}

/*
 * Compare two scalar JsonbValues, returning -1, 0, or 1.
 *
 * Strings are compared using the default collation.  Used by B-tree
 * operators, where a lexical sort order is generally expected.
 */
static int
compareJsonbScalarValue(JsonbValue *a, JsonbValue *b)
{
	if (a->type == b->type)
	{
		switch (a->type)
		{
			case jbvNull:
				return 0;
			case jbvString:
				return varstr_cmp(a->val.string.val,
								  a->val.string.len,
								  b->val.string.val,
								  b->val.string.len,
								  DEFAULT_COLLATION_OID);
			case jbvNumeric:
				return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
														 PointerGetDatum(a->val.numeric),
														 PointerGetDatum(b->val.numeric)));
			case jbvBool:
				if (a->val.boolean == b->val.boolean)
					return 0;
				else if (a->val.boolean > b->val.boolean)
					return 1;
				else
					return -1;
			default:
				elog(ERROR, "invalid jsonb scalar type");
		}
	}
	elog(ERROR, "jsonb scalar type mismatch");
	return -1;
}


/*
 * Functions for manipulating the resizable buffer used by convertJsonb and
 * its subroutines.
 */

/*
 * Reserve 'len' bytes, at the end of the buffer, enlarging it if necessary.
 * Returns the offset to the reserved area. The caller is expected to fill
 * the reserved area later with copyToBuffer().
 */
static int
reserveFromBuffer(StringInfo buffer, int len)
{
	int			offset;

	/* Make more room if needed */
	enlargeStringInfo(buffer, len);

	/* remember current offset */
	offset = buffer->len;

	/* reserve the space */
	buffer->len += len;

	/*
	 * Keep a trailing null in place, even though it's not useful for us; it
	 * seems best to preserve the invariants of StringInfos.
	 */
	buffer->data[buffer->len] = '\0';

	return offset;
}

/*
 * Copy 'len' bytes to a previously reserved area in buffer.
 */
static void
copyToBuffer(StringInfo buffer, int offset, const void *data, int len)
{
	memcpy(buffer->data + offset, data, len);
}

/*
 * A shorthand for reserveFromBuffer + copyToBuffer.
 */
static void
appendToBuffer(StringInfo buffer, const void *data, int len)
{
	int			offset;

	offset = reserveFromBuffer(buffer, len);
	copyToBuffer(buffer, offset, data, len);
}


/*
 * Append padding, so that the length of the StringInfo is int-aligned.
 * Returns the number of padding bytes appended.
 */
static short
padBufferToInt(StringInfo buffer)
{
	int			padlen,
				p,
				offset;

	padlen = INTALIGN(buffer->len) - buffer->len;

	offset = reserveFromBuffer(buffer, padlen);

	/* padlen must be small, so this is probably faster than a memset */
	for (p = 0; p < padlen; p++)
		buffer->data[offset + p] = '\0';

	return padlen;
}

/* ----------------------------------------------------------------
 * KVMap helpers (K1.2)
 *
 *	estimateJsonbValueSize  — quick upper-bound estimate of the on-disk
 *	    size of a single JsonbValue.  Used by convertJsonbObject() to
 *	    sort an object's values by size when jsonb_sort_field_values
 *	    is on; the exact figure is not required, only the relative
 *	    ordering it produces.
 *
 *	initKVMap  — given a pointer just past the 2N JEntries of an
 *	    on-disk container and the pair count, set up a JsonbKVMap
 *	    descriptor and return the pointer to the byte after the KVMap
 *	    (where the data area starts).  When `sorted` is false, the
 *	    descriptor is initialized to entry_size = 0 (identity mapping)
 *	    and the input pointer is returned unchanged.
 *
 *	int_pair_size_cmp  — qsort comparator for the (size, index)
 *	    auxiliary array used to drive the sort.
 *
 * No callsites use these functions yet; they are introduced in K1.3.
 * ---------------------------------------------------------------- */

/*
 * The estimator must be a strict upper bound to be safe for buffer
 * planning, but it is also used directly as a sort key.  We follow
 * postgrespro/jsonb_toaster's logic: scalar payload + JEntry per
 * value; recurse into nested arrays/objects; for jbvBinary, recurse
 * into the contained JsonbValue.  Padding bytes (alignment) are
 * ignored at this level — they cannot tilt the small-vs-large
 * comparison meaningfully.
 */
static int
estimateJsonbValueSize(const JsonbValue *jbv)
{
	int			size;

	switch (jbv->type)
	{
		case jbvNull:
		case jbvBool:
			return sizeof(JEntry);

		case jbvString:
			return sizeof(JEntry) + jbv->val.string.len;

		case jbvNumeric:
			return sizeof(JEntry) + VARSIZE_ANY(jbv->val.numeric);

		case jbvDatetime:
			/* shouldn't appear inside a finished container, but be safe */
			return sizeof(JEntry) + 32;

		case jbvArray:
			{
				int			i;

				/* container header + N JEntries + recursive sizes */
				size = sizeof(JEntry) + sizeof(uint32) +
					sizeof(JEntry) * jbv->val.array.nElems;
				for (i = 0; i < jbv->val.array.nElems; i++)
					size += estimateJsonbValueSize(&jbv->val.array.elems[i]);
				return size;
			}

		case jbvObject:
			{
				int			i;

				/* container header + 2N JEntries + recursive sizes for k+v */
				size = sizeof(JEntry) + sizeof(uint32) +
					2 * sizeof(JEntry) * jbv->val.object.nPairs;
				for (i = 0; i < jbv->val.object.nPairs; i++)
				{
					size += estimateJsonbValueSize(&jbv->val.object.pairs[i].key);
					size += estimateJsonbValueSize(&jbv->val.object.pairs[i].value);
				}
				return size;
			}

		case jbvBinary:
			/*
			 * The serialized size of a binary container is exactly
			 * its length on disk (header + data).  Add one JEntry
			 * for the parent slot.
			 */
			return sizeof(JEntry) + jbv->val.binary.len;

		case jbvToasted:
			/* JEntry + up to 3 bytes INTALIGN pad + descriptor bytes */
			return sizeof(JEntry) + 3 + jbv->val.toasted.len;

		default:
			elog(ERROR, "unrecognized jsonb value type: %d", (int) jbv->type);
			return 0;
	}
}

/*
 * Initialize a JsonbKVMap descriptor over an on-disk region.
 *
 *	pentries: pointer to the first byte after the 2N JEntries.
 *	field_count: number of key-value pairs (N).
 *	sorted: whether a KVMap is actually present at pentries.
 *
 * Returns the pointer to the first byte after the KVMap, suitably
 * aligned: this is the start of the keys/values data area.
 */
static void *
initKVMap(JsonbKVMap *kvmap, void *pentries, int field_count, bool sorted)
{
	if (sorted)
	{
		kvmap->map.entries = pentries;
		kvmap->entry_size = JSONB_KVMAP_ENTRY_SIZE(field_count);

		return (char *) pentries +
			INTALIGN(field_count * kvmap->entry_size);
	}
	else
	{
		kvmap->entry_size = 0;
		kvmap->map.entries = NULL;

		return pentries;
	}
}

/*
 * qsort comparator for { int size; int32 index; } pairs.  Sort
 * ascending by size; index is the tiebreaker only because qsort
 * is not guaranteed stable.  Stability is not required for
 * correctness — the writer re-checks whether the sort actually
 * moved any element before deciding to emit a KVMap — but a
 * deterministic order across platforms makes test output stable.
 */
static int
int_pair_size_cmp(const void *a, const void *b)
{
	const struct
	{
		int			size;
		int32		index;
	}		   *pa = a,
			   *pb = b;

	if (pa->size != pb->size)
		return pa->size - pb->size;
	return pa->index - pb->index;
}

/*
 * Given a JsonbValue, convert to Jsonb. The result is palloc'd.
 */
static Jsonb *
convertToJsonb(JsonbValue *val)
{
	StringInfoData buffer;
	JEntry		jentry;
	Jsonb	   *res;

	/* Should not already have binary representation */
	Assert(val->type != jbvBinary);

	/* Allocate an output buffer. It will be enlarged as needed */
	initStringInfo(&buffer);

	/* Make room for the varlena header */
	reserveFromBuffer(&buffer, VARHDRSZ);

	convertJsonbValue(&buffer, &jentry, val, 0);

	/*
	 * Note: the JEntry of the root is discarded. Therefore the root
	 * JsonbContainer struct must contain enough information to tell what kind
	 * of value it is.
	 */

	res = (Jsonb *) buffer.data;

	SET_VARSIZE(res, buffer.len);

	return res;
}

/*
 * Subroutine of convertJsonb: serialize a single JsonbValue into buffer.
 *
 * The JEntry header for this node is returned in *header.  It is filled in
 * with the length of this value and appropriate type bits.  If we wish to
 * store an end offset rather than a length, it is the caller's responsibility
 * to adjust for that.
 *
 * If the value is an array or an object, this recurses. 'level' is only used
 * for debugging purposes.
 */
static void
convertJsonbValue(StringInfo buffer, JEntry *header, JsonbValue *val, int level)
{
	check_stack_depth();

	if (!val)
		return;

	/*
	 * A JsonbValue passed as val should never have a type of jbvBinary, and
	 * neither should any of its sub-components. Those values will be produced
	 * by convertJsonbArray and convertJsonbObject, the results of which will
	 * not be passed back to this function as an argument.
	 */

	if (val->type == jbvToasted || IsAJsonbScalar(val))
		convertJsonbScalar(buffer, header, val);
	else if (val->type == jbvArray)
		convertJsonbArray(buffer, header, val, level);
	else if (val->type == jbvObject)
		convertJsonbObject(buffer, header, val, level);
	else
		elog(ERROR, "unknown type of jsonb container to convert");
}

static void
convertJsonbArray(StringInfo buffer, JEntry *header, JsonbValue *val, int level)
{
	int			base_offset;
	int			jentry_offset;
	int			i;
	int			totallen;
	uint32		containerhead;
	int			nElems = val->val.array.nElems;

	/* Remember where in the buffer this array starts. */
	base_offset = buffer->len;

	/* Align to 4-byte boundary (any padding counts as part of my data) */
	padBufferToInt(buffer);

	/*
	 * Construct the header Jentry and store it in the beginning of the
	 * variable-length payload.
	 */
	containerhead = nElems | JB_FARRAY;
	if (val->val.array.rawScalar)
	{
		Assert(nElems == 1);
		Assert(level == 0);
		containerhead |= JB_FSCALAR;
	}

	appendToBuffer(buffer, &containerhead, sizeof(uint32));

	/* Reserve space for the JEntries of the elements. */
	jentry_offset = reserveFromBuffer(buffer, sizeof(JEntry) * nElems);

	totallen = 0;
	for (i = 0; i < nElems; i++)
	{
		JsonbValue *elem = &val->val.array.elems[i];
		int			len;
		JEntry		meta;

		/*
		 * Convert element, producing a JEntry and appending its
		 * variable-length data to buffer
		 */
		convertJsonbValue(buffer, &meta, elem, level + 1);

		len = JBE_OFFLENFLD(meta);
		totallen += len;

		/*
		 * Bail out if total variable-length data exceeds what will fit in a
		 * JEntry length field.  We check this in each iteration, not just
		 * once at the end, to forestall possible integer overflow.
		 */
		if (totallen > JENTRY_OFFLENMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb array elements exceeds the maximum of %d bytes",
							JENTRY_OFFLENMASK)));

		/*
		 * Convert each JB_OFFSET_STRIDE'th length to an offset.
		 */
		if ((i % JB_OFFSET_STRIDE) == 0)
			meta = (meta & JENTRY_TYPEMASK) | totallen | JENTRY_HAS_OFF;

		copyToBuffer(buffer, jentry_offset, &meta, sizeof(JEntry));
		jentry_offset += sizeof(JEntry);
	}

	/* Total data size is everything we've appended to buffer */
	totallen = buffer->len - base_offset;

	/* Check length again, since we didn't include the metadata above */
	if (totallen > JENTRY_OFFLENMASK)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("total size of jsonb array elements exceeds the maximum of %d bytes",
						JENTRY_OFFLENMASK)));

	/* Initialize the header of this node in the container's JEntry array */
	*header = JENTRY_ISCONTAINER | totallen;
}

static void
convertJsonbObject(StringInfo buffer, JEntry *header, JsonbValue *val, int level)
{
	int			base_offset;
	int			jentry_offset;
	int			i;
	int			totallen;
	uint32		containerheader;
	int			nPairs = val->val.object.nPairs;
	int			reserved_size;
	int			kvmap_entry_size = 0;
	bool		sorted_values = jsonb_sort_field_values &&
		!jsonb_force_stock_layout && nPairs > 1;
	bool		object_has_toasted = false;
	struct
	{
		int			size;
		int32		index;
	}		   *values = sorted_values
		? palloc(sizeof(*values) * nPairs)
		: NULL;

	/*
	 * If the GUC is on and the object has more than one pair, decide
	 * whether emitting a KVMap is worthwhile.  We sort an auxiliary
	 * array of (estimated_size, original_index) pairs by size; if the
	 * sort actually reorders anything, KVMap is emitted.  When all
	 * values happen to already be in size order (e.g. an empty or
	 * size-uniform object), we fall back to the unsorted layout to
	 * keep the output bit-identical to the no-KVMap case.
	 */
	if (sorted_values)
	{
		for (i = 0; i < nPairs; i++)
		{
			values[i].index = i;
			values[i].size = estimateJsonbValueSize(&val->val.object.pairs[i].value);
		}

		qsort(values, nPairs, sizeof(*values), int_pair_size_cmp);

		sorted_values = false;
		for (i = 0; i < nPairs; i++)
		{
			if (values[i].index != i)
			{
				kvmap_entry_size = JSONB_KVMAP_ENTRY_SIZE(nPairs);
				sorted_values = true;
				break;
			}
		}
	}

	/* Remember where in the buffer this object starts. */
	base_offset = buffer->len;

	/* Align to 4-byte boundary (any padding counts as part of my data) */
	padBufferToInt(buffer);

	/*
	 * W2.3a: record in the container header whether any value is a cold-payload
	 * descriptor (jbvToasted -> JENTRY_ISTOASTED).  This lets the delete-side
	 * gate detect split parents in O(1) without scanning JEntries.  Checked
	 * over the logical pairs here, before values are serialized.
	 */
	{
		bool		has_toasted = false;

		for (i = 0; i < nPairs; i++)
		{
			if (val->val.object.pairs[i].value.type == jbvToasted)
			{
				has_toasted = true;
				break;
			}
		}
		object_has_toasted = has_toasted;
	}

	/*
	 * Construct the header Jentry and store it in the beginning of the
	 * variable-length payload.  Set JB_FOBJECT_KVMAP iff we are about
	 * to emit a KVMap; the JB_FOBJECT bit is preserved unconditionally
	 * so existing readers that only check JB_FOBJECT keep working.
	 */
	containerheader = nPairs | JB_FOBJECT |
		(sorted_values ? JB_FOBJECT_KVMAP : 0) |
		(object_has_toasted ? JB_FHAS_TOASTED : 0);
	appendToBuffer(buffer, &containerheader, sizeof(uint32));

	/*
	 * Reserve space for the 2N JEntries plus, when present, an
	 * INTALIGN'd KVMap region (kvmap_entry_size bytes per pair).
	 */
	reserved_size = sizeof(JEntry) * nPairs * 2;
	if (sorted_values)
		reserved_size += INTALIGN(kvmap_entry_size * nPairs);

	jentry_offset = reserveFromBuffer(buffer, reserved_size);

	/* Write the KVMap entries before any keys/values. */
	if (sorted_values)
	{
		int			kvmap_offset = jentry_offset + sizeof(JEntry) * nPairs * 2;

		for (i = 0; i < nPairs; i++)
		{
			uint8		entry1;
			uint16		entry2;
			uint32		entry4;
			void	   *pentry;

			if (kvmap_entry_size == 1)
			{
				entry1 = (uint8) i;
				pentry = &entry1;
			}
			else if (kvmap_entry_size == 2)
			{
				entry2 = (uint16) i;
				pentry = &entry2;
			}
			else
			{
				entry4 = (uint32) i;
				pentry = &entry4;
			}

			copyToBuffer(buffer,
						 kvmap_offset + values[i].index * kvmap_entry_size,
						 pentry, kvmap_entry_size);
		}

		/* Zero-fill the alignment padding for deterministic output. */
		if ((kvmap_entry_size * nPairs) % ALIGNOF_INT)
			memset(buffer->data + kvmap_offset + kvmap_entry_size * nPairs,
				   0,
				   ALIGNOF_INT - (kvmap_entry_size * nPairs) % ALIGNOF_INT);
	}

	/*
	 * Iterate over the keys, then over the values, since that is the ordering
	 * we want in the on-disk representation.
	 */
	totallen = 0;
	for (i = 0; i < nPairs; i++)
	{
		JsonbPair  *pair = &val->val.object.pairs[i];
		int			len;
		JEntry		meta;

		/*
		 * Convert key, producing a JEntry and appending its variable-length
		 * data to buffer
		 */
		convertJsonbScalar(buffer, &meta, &pair->key);

		len = JBE_OFFLENFLD(meta);
		totallen += len;

		/*
		 * Bail out if total variable-length data exceeds what will fit in a
		 * JEntry length field.  We check this in each iteration, not just
		 * once at the end, to forestall possible integer overflow.
		 */
		if (totallen > JENTRY_OFFLENMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb object elements exceeds the maximum of %d bytes",
							JENTRY_OFFLENMASK)));

		/*
		 * Convert each JB_OFFSET_STRIDE'th length to an offset.
		 */
		if ((i % JB_OFFSET_STRIDE) == 0)
			meta = (meta & JENTRY_TYPEMASK) | totallen | JENTRY_HAS_OFF;

		copyToBuffer(buffer, jentry_offset, &meta, sizeof(JEntry));
		jentry_offset += sizeof(JEntry);
	}
	for (i = 0; i < nPairs; i++)
	{
		/*
		 * When sorted_values is on, walk the original pairs array in
		 * the new physical order: position i in the on-disk data area
		 * is occupied by pair[values[i].index].  When KVMap is absent
		 * (sorted_values false), val_index == i and we behave exactly
		 * as before.
		 */
		int			val_index = sorted_values ? values[i].index : i;
		JsonbPair  *pair = &val->val.object.pairs[val_index];
		int			len;
		JEntry		meta;

		/*
		 * Convert value, producing a JEntry and appending its variable-length
		 * data to buffer
		 */
		convertJsonbValue(buffer, &meta, &pair->value, level + 1);

		len = JBE_OFFLENFLD(meta);
		totallen += len;

		/*
		 * Bail out if total variable-length data exceeds what will fit in a
		 * JEntry length field.  We check this in each iteration, not just
		 * once at the end, to forestall possible integer overflow.
		 */
		if (totallen > JENTRY_OFFLENMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb object elements exceeds the maximum of %d bytes",
							JENTRY_OFFLENMASK)));

		/*
		 * Convert each JB_OFFSET_STRIDE'th length to an offset.
		 */
		if (((i + nPairs) % JB_OFFSET_STRIDE) == 0)
			meta = (meta & JENTRY_TYPEMASK) | totallen | JENTRY_HAS_OFF;

		copyToBuffer(buffer, jentry_offset, &meta, sizeof(JEntry));
		jentry_offset += sizeof(JEntry);
	}

	if (values)
		pfree(values);

	/* Total data size is everything we've appended to buffer */
	totallen = buffer->len - base_offset;

	/* Check length again, since we didn't include the metadata above */
	if (totallen > JENTRY_OFFLENMASK)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("total size of jsonb object elements exceeds the maximum of %d bytes",
						JENTRY_OFFLENMASK)));

	/* Initialize the header of this node in the container's JEntry array */
	*header = JENTRY_ISCONTAINER | totallen;
}

static void
convertJsonbScalar(StringInfo buffer, JEntry *header, JsonbValue *scalarVal)
{
	int			numlen;
	short		padlen;

	switch (scalarVal->type)
	{
		case jbvNull:
			*header = JENTRY_ISNULL;
			break;

		case jbvString:
			appendToBuffer(buffer, scalarVal->val.string.val, scalarVal->val.string.len);

			*header = scalarVal->val.string.len;
			break;

		case jbvNumeric:
			numlen = VARSIZE_ANY(scalarVal->val.numeric);
			padlen = padBufferToInt(buffer);

			appendToBuffer(buffer, scalarVal->val.numeric, numlen);

			*header = JENTRY_ISNUMERIC | (padlen + numlen);
			break;

		case jbvBool:
			*header = (scalarVal->val.boolean) ?
				JENTRY_ISBOOL_TRUE : JENTRY_ISBOOL_FALSE;
			break;

		case jbvToasted:
			/* cold-payload descriptor: INTALIGN like numeric, then bytes */
			padlen = padBufferToInt(buffer);
			appendToBuffer(buffer, scalarVal->val.toasted.data,
						   scalarVal->val.toasted.len);
			*header = JENTRY_ISTOASTED | (padlen + scalarVal->val.toasted.len);
			break;

		case jbvDatetime:
			{
				char		buf[MAXDATELEN + 1];
				size_t		len;

				JsonEncodeDateTime(buf,
								   scalarVal->val.datetime.value,
								   scalarVal->val.datetime.typid,
								   &scalarVal->val.datetime.tz);
				len = strlen(buf);
				appendToBuffer(buffer, buf, len);

				*header = len;
			}
			break;

		default:
			elog(ERROR, "invalid jsonb scalar type");
	}
}

/*
 * Compare two jbvString JsonbValue values, a and b.
 *
 * This is a special qsort() comparator used to sort strings in certain
 * internal contexts where it is sufficient to have a well-defined sort order.
 * In particular, object pair keys are sorted according to this criteria to
 * facilitate cheap binary searches where we don't care about lexical sort
 * order.
 *
 * a and b are first sorted based on their length.  If a tie-breaker is
 * required, only then do we consider string binary equality.
 */
static int
lengthCompareJsonbStringValue(const void *a, const void *b)
{
	const JsonbValue *va = (const JsonbValue *) a;
	const JsonbValue *vb = (const JsonbValue *) b;

	Assert(va->type == jbvString);
	Assert(vb->type == jbvString);

	return lengthCompareJsonbString(va->val.string.val, va->val.string.len,
									vb->val.string.val, vb->val.string.len);
}

/*
 * Subroutine for lengthCompareJsonbStringValue
 *
 * This is also useful separately to implement binary search on
 * JsonbContainers.
 */
static int
lengthCompareJsonbString(const char *val1, int len1, const char *val2, int len2)
{
	if (len1 == len2)
		return memcmp(val1, val2, len1);
	else
		return len1 > len2 ? 1 : -1;
}

/*
 * qsort_arg() comparator to compare JsonbPair values.
 *
 * Third argument 'binequal' may point to a bool. If it's set, *binequal is set
 * to true iff a and b have full binary equality, since some callers have an
 * interest in whether the two values are equal or merely equivalent.
 *
 * N.B: String comparisons here are "length-wise"
 *
 * Pairs with equals keys are ordered such that the order field is respected.
 */
static int
lengthCompareJsonbPair(const void *a, const void *b, void *binequal)
{
	const JsonbPair *pa = (const JsonbPair *) a;
	const JsonbPair *pb = (const JsonbPair *) b;
	int			res;

	res = lengthCompareJsonbStringValue(&pa->key, &pb->key);
	if (res == 0 && binequal)
		*((bool *) binequal) = true;

	/*
	 * Guarantee keeping order of equal pair.  Unique algorithm will prefer
	 * first element as value.
	 */
	if (res == 0)
		res = (pa->order > pb->order) ? -1 : 1;

	return res;
}

/*
 * Sort and unique-ify pairs in JsonbValue object
 */
static void
uniqueifyJsonbObject(JsonbValue *object, bool unique_keys, bool skip_nulls)
{
	JsonbPair  *pairs = object->val.object.pairs;
	int			nPairs = object->val.object.nPairs;
	bool		hasNonUniq = false;

	Assert(object->type == jbvObject);

	if (nPairs > 1)
		qsort_arg(pairs, nPairs, sizeof(JsonbPair),
				  lengthCompareJsonbPair, &hasNonUniq);

	if (hasNonUniq && unique_keys)
		ereport(ERROR,
				errcode(ERRCODE_DUPLICATE_JSON_OBJECT_KEY_VALUE),
				errmsg("duplicate JSON object key value"));

	if (hasNonUniq || skip_nulls)
	{
		int			nNewPairs = 0;

		for (int i = 0; i < nPairs; i++)
		{
			JsonbPair  *ptr = pairs + i;

			/* Skip duplicate keys */
			if (nNewPairs > 0 &&
				lengthCompareJsonbStringValue(&pairs[nNewPairs - 1].key,
											  &ptr->key) == 0)
				continue;
			/* Skip null values, if told to */
			if (skip_nulls && ptr->value.type == jbvNull)
				continue;
			/* Emit this pair, but avoid no-op copy */
			if (i > nNewPairs)
				pairs[nNewPairs] = *ptr;
			nNewPairs++;
		}
		object->val.object.nPairs = nNewPairs;
	}
}

/* ----------------------------------------------------------------
 * jsonb_kvmap_debug(jsonb) -> jsonb
 *
 *	Test/debug helper exposing K1's physical-layout facts as a
 *	jsonb result.  Strictly read-only.  Used by the K1 regression
 *	test (src/test/regress/sql/jsonb_kvmap.sql) to assert that
 *	JB_FOBJECT_KVMAP, the KVMap entry size, and the resulting
 *	logical-to-physical mapping match expectations under both
 *	default-OFF and GUC-ON conditions.
 *
 *	Returns:
 *	  {
 *	    "has_kvmap": <bool>,
 *	    "npairs": <int>,
 *	    "kvmap_entry_size": <int>,        -- 0 when has_kvmap=false
 *	    "logical_to_physical": [...],     -- array of int
 *	    "value_offsets": { key: offset, ... },
 *	    "value_sizes":   { key: length, ... }
 *	  }
 *
 *	When the input root is not an object, returns
 *	  { "type": "<scalar/array>" }.
 *
 *	Offsets and sizes are reported in *bytes inside the data area*
 *	(not whole-document offsets).  This is the granularity that
 *	matters for KVMap evidence.
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(jsonb_kvmap_debug);
Datum
jsonb_kvmap_debug(PG_FUNCTION_ARGS)
{
	Jsonb		   *jb = PG_GETARG_JSONB_P(0);
	JsonbContainer *container = &jb->root;
	JsonbInState	out;
	int				count;
	bool			has_kvmap;
	JsonbKVMap		kvmap;
	JEntry		   *children;
	char		   *baseAddr;
	int				i;

	memset(&out, 0, sizeof(out));

	/* Non-object: report only the type. */
	if (!JsonContainerIsObject(container))
	{
		JsonbValue	t;

		pushJsonbValue(&out, WJB_BEGIN_OBJECT, NULL);
		t.type = jbvString;
		t.val.string.val = "type";
		t.val.string.len = 4;
		pushJsonbValue(&out, WJB_KEY, &t);
		t.val.string.val = JsonContainerIsArray(container)
			? (JsonContainerIsScalar(container) ? "scalar" : "array")
			: "other";
		t.val.string.len = strlen(t.val.string.val);
		pushJsonbValue(&out, WJB_VALUE, &t);
		pushJsonbValue(&out, WJB_END_OBJECT, NULL);
		PG_RETURN_POINTER(JsonbValueToJsonb(out.result));
	}

	count = JsonContainerSize(container);
	has_kvmap = JsonContainerHasKVMap(container);
	children = container->children;

	/*
	 * After this call, baseAddr points to the first byte of the keys
	 * area (past the KVMap if present) and kvmap describes the on-disk
	 * map.  For has_kvmap=false, kvmap.entry_size == 0 (identity).
	 */
	baseAddr = initKVMap(&kvmap, (char *) (children + count * 2),
						 count, has_kvmap);

	pushJsonbValue(&out, WJB_BEGIN_OBJECT, NULL);

	/* has_kvmap */
	{
		JsonbValue	k,
					v;

		k.type = jbvString;
		k.val.string.val = "has_kvmap";
		k.val.string.len = 9;
		pushJsonbValue(&out, WJB_KEY, &k);
		v.type = jbvBool;
		v.val.boolean = has_kvmap;
		pushJsonbValue(&out, WJB_VALUE, &v);
	}

	/* npairs */
	{
		JsonbValue	k,
					v;

		k.type = jbvString;
		k.val.string.val = "npairs";
		k.val.string.len = 6;
		pushJsonbValue(&out, WJB_KEY, &k);
		v.type = jbvNumeric;
		v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
															Int32GetDatum(count)));
		pushJsonbValue(&out, WJB_VALUE, &v);
	}

	/* kvmap_entry_size */
	{
		JsonbValue	k,
					v;

		k.type = jbvString;
		k.val.string.val = "kvmap_entry_size";
		k.val.string.len = 16;
		pushJsonbValue(&out, WJB_KEY, &k);
		v.type = jbvNumeric;
		v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
												Int32GetDatum(kvmap.entry_size)));
		pushJsonbValue(&out, WJB_VALUE, &v);
	}

	/* logical_to_physical: array of int (length = count) */
	{
		JsonbValue	k;

		k.type = jbvString;
		k.val.string.val = "logical_to_physical";
		k.val.string.len = 19;
		pushJsonbValue(&out, WJB_KEY, &k);
		pushJsonbValue(&out, WJB_BEGIN_ARRAY, NULL);
		for (i = 0; i < count; i++)
		{
			JsonbValue	v;
			int32		mapped = JSONB_KVMAP_ENTRY(&kvmap, i);

			v.type = jbvNumeric;
			v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
														Int32GetDatum(mapped)));
			pushJsonbValue(&out, WJB_ELEM, &v);
		}
		pushJsonbValue(&out, WJB_END_ARRAY, NULL);
	}

	/* value_offsets: {key -> physical offset (in bytes from baseAddr)} */
	{
		JsonbValue	k;

		k.type = jbvString;
		k.val.string.val = "value_offsets";
		k.val.string.len = 13;
		pushJsonbValue(&out, WJB_KEY, &k);
		pushJsonbValue(&out, WJB_BEGIN_OBJECT, NULL);
		for (i = 0; i < count; i++)
		{
			JsonbValue	keyv,
						v;
			int32		value_idx = JSONB_KVMAP_ENTRY(&kvmap, i) + count;
			uint32		off = getJsonbOffset(container, value_idx);
			const char *kbase = baseAddr + getJsonbOffset(container, i);
			int			klen = getJsonbLength(container, i);

			keyv.type = jbvString;
			keyv.val.string.val = (char *) kbase;
			keyv.val.string.len = klen;
			pushJsonbValue(&out, WJB_KEY, &keyv);
			v.type = jbvNumeric;
			v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
														Int32GetDatum((int32) off)));
			pushJsonbValue(&out, WJB_VALUE, &v);
		}
		pushJsonbValue(&out, WJB_END_OBJECT, NULL);
	}

	/* value_sizes: {key -> size of value in bytes} */
	{
		JsonbValue	k;

		k.type = jbvString;
		k.val.string.val = "value_sizes";
		k.val.string.len = 11;
		pushJsonbValue(&out, WJB_KEY, &k);
		pushJsonbValue(&out, WJB_BEGIN_OBJECT, NULL);
		for (i = 0; i < count; i++)
		{
			JsonbValue	keyv,
						v;
			int32		value_idx = JSONB_KVMAP_ENTRY(&kvmap, i) + count;
			uint32		vsize = getJsonbLength(container, value_idx);
			const char *kbase = baseAddr + getJsonbOffset(container, i);
			int			klen = getJsonbLength(container, i);

			keyv.type = jbvString;
			keyv.val.string.val = (char *) kbase;
			keyv.val.string.len = klen;
			pushJsonbValue(&out, WJB_KEY, &keyv);
			v.type = jbvNumeric;
			v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
												Int32GetDatum((int32) vsize)));
			pushJsonbValue(&out, WJB_VALUE, &v);
		}
		pushJsonbValue(&out, WJB_END_OBJECT, NULL);
	}

	pushJsonbValue(&out, WJB_END_OBJECT, NULL);
	PG_RETURN_POINTER(JsonbValueToJsonb(out.result));
}

/*
 * jsonb_toast_split_datum
 *
 * W2.2 toast-time split (create only).  Given a jsonb attribute value that is
 * large enough to be toasted, move each large top-level scalar payload out of
 * line as an ordinary TOAST value and return a compact parent that keeps the
 * small (warm) values inline and a JENTRY_ISTOASTED descriptor in place of each
 * moved value.
 *
 * The parent is rebuilt through the stock structural writer (pushJsonbValue /
 * JsonbValueToJsonb); the moved value is emitted as a jbvToasted scalar, so
 * alignment and JEntry stride follow stock rules with no byte surgery.  This is
 * the same producer path proven by W2.1; the only change is that the split
 * decision is now driven by value size at toast time rather than by an explicit
 * key argument.  W2.x operates on stock jsonb layout only -- no KVMap, no value
 * sorting, no jsonb_sort_field_values dependency.
 *
 * Eligibility (anything else returns the original datum unchanged):
 *   - the value's root is a top-level object (stock layout; KVMap not required);
 *   - only top-level *scalar* (string / numeric) values are considered, and
 *     only when their body is at least value_min bytes.  Nested containers and
 *     small metadata pass through untouched (no nested paths -- a W2.2 non-goal).
 *
 * The input is fully detoasted (external assembled, decompressed) via
 * DatumGetJsonbP before its structure is inspected, so a compressed/external
 * incoming datum is split on its raw form.
 *
 * Create only: no old-parent inspection, no descriptor reuse.  *did_split is
 * set true iff at least one value was moved out of line.
 */
Datum
jsonb_toast_split_datum(Relation rel, Datum value, Datum oldvalue,
						bool old_isnull, Size value_min,
						uint32 options, bool *did_split)
{
	Jsonb	   *jb;
	Jsonb	   *oldjb = NULL;
	JsonbContainer *oldroot = NULL;
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken tok;
	JsonbInState pstate = {0};
	bool		split_any = false;
	const char *curkey = NULL;	/* W2.4: key of the value currently emitted */
	int			curkeylen = 0;

	*did_split = false;

	/* Need a toast relation to move payload into. */
	if (!OidIsValid(rel->rd_rel->reltoastrelid))
		return value;

	/* Detoast fully so we inspect raw jsonb structure (handles compressed). */
	jb = DatumGetJsonbP(value);

	/* Eligible: any top-level jsonb object (stock layout; no KVMap dependency). */
	if (!JB_ROOT_IS_OBJECT(jb))
		return value;

	/*
	 * W2.4 reuse: on UPDATE, the old value lets us preserve unchanged cold
	 * children.  Detoast the old parent (it is inline/small) so we can read its
	 * descriptors by key without materializing the cold payload.  Reuse is only
	 * attempted when the old value is itself a split object.
	 */
	if (!old_isnull && DatumGetPointer(oldvalue) != (Pointer) 0)
	{
		oldjb = DatumGetJsonbP(oldvalue);
		if (JB_ROOT_IS_OBJECT(oldjb) && JB_ROOT_HAS_TOASTED(oldjb))
			oldroot = &oldjb->root;
	}

	/*
	 * skipNested = true: each top-level value arrives as a single JsonbValue
	 * (scalar, or jbvBinary for a nested container).  We therefore only ever
	 * see direct children of the root object -- "top-level only" holds by
	 * construction, with no depth bookkeeping.
	 */
	it = JsonbIteratorInit(&jb->root);
	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		switch (tok)
		{
			case WJB_BEGIN_OBJECT:
				pushJsonbValue(&pstate, WJB_BEGIN_OBJECT, NULL);
				break;
			case WJB_END_OBJECT:
				pushJsonbValue(&pstate, WJB_END_OBJECT, NULL);
				break;
			case WJB_KEY:
				curkey = v.val.string.val;
				curkeylen = v.val.string.len;
				pushJsonbValue(&pstate, WJB_KEY, &v);
				break;
			case WJB_VALUE:
				{
					bool		splitthis = false;

					/* Only large top-level scalar payloads are moved out. */
					if (v.type == jbvString)
						splitthis = ((Size) v.val.string.len >= value_min);
					else if (v.type == jbvNumeric)
						splitthis = ((Size) VARSIZE(v.val.numeric) >= value_min);

					if (splitthis)
					{
						Jsonb	   *child = JsonbValueToJsonb(&v);
						struct varatt_external old_ext;
						bool		reused = false;
						char	   *descbuf;
						JsonbValue	tv;
						struct varatt_external new_ext;

						/*
						 * W2.4 reuse: if the old object had this same key as a
						 * JENTRY_ISTOASTED child, and the new child is byte-exact
						 * equal to the old cold payload, reuse the old valueid
						 * instead of writing a fresh TOAST value.  Comparison is
						 * key-based and byte-exact (no false positive); a cheap
						 * size pre-check avoids the detoast when clearly changed.
						 */
						if (oldroot != NULL && curkey != NULL)
						{
							jsonb_reuse_attempts++;
							if (jsonb_find_old_toasted_ref(oldroot, curkey,
														   curkeylen, &old_ext))
							{
								if (old_ext.va_rawsize != (int32) VARSIZE(child))
								{
									jsonb_reuse_size_mismatch++;
								}
								else
								{
									struct varlena *oldref;
									struct varlena *oldfull;

									/* Reconstruct an on-disk ref to detoast old. */
									oldref = (struct varlena *)
										palloc(VARHDRSZ_EXTERNAL +
											   sizeof(struct varatt_external));
									SET_VARTAG_EXTERNAL(oldref, VARTAG_ONDISK);
									memcpy(VARDATA_EXTERNAL(oldref), &old_ext,
										   sizeof(struct varatt_external));
									oldfull = detoast_external_attr(oldref);

									if (VARSIZE(oldfull) == VARSIZE(child) &&
										memcmp(oldfull, child, VARSIZE(child)) == 0)
									{
										jsonb_reuse_memcmp_match++;
										reused = true;
									}
									else
										jsonb_reuse_memcmp_mismatch++;

									pfree(oldref);
									if ((Pointer) oldfull != (Pointer) NULL)
										pfree(oldfull);
								}
							}
						}

						if (reused)
						{
							/* Reuse old descriptor verbatim (same valueid). */
							memcpy(&new_ext, &old_ext, sizeof(struct varatt_external));
						}
						else
						{
							Datum		toasted = toast_save_datum(rel,
																   PointerGetDatum(child),
																   NULL, options);
							struct varlena *tptr =
								(struct varlena *) DatumGetPointer(toasted);

							jsonb_reuse_toast_saves++;
							if (!VARATT_IS_EXTERNAL_ONDISK(tptr))
								elog(ERROR, "jsonb_toast_split_datum: expected on-disk pointer");
							memcpy(&new_ext, VARDATA_EXTERNAL(tptr),
								   sizeof(struct varatt_external));
						}

						descbuf = palloc0(JSONB_TOASTED_DATUM_SIZE);
						((JsonbToastedDatum *) descbuf)->orig_jbe_type =
							(v.type == jbvNumeric) ? JBE_TOASTED_ORIG_NUMERIC :
							JBE_TOASTED_ORIG_STRING;
						memcpy(descbuf + offsetof(JsonbToastedDatum, reserved) + sizeof(uint16),
							   &new_ext, sizeof(struct varatt_external));

						tv.type = jbvToasted;
						tv.val.toasted.data = descbuf;
						tv.val.toasted.len = JSONB_TOASTED_DATUM_SIZE;
						pushJsonbValue(&pstate, WJB_VALUE, &tv);
						split_any = true;
					}
					else
						pushJsonbValue(&pstate, WJB_VALUE, &v);
					break;
				}
			default:
				elog(ERROR, "jsonb_toast_split_datum: unexpected token %d", tok);
		}
	}

	if (!split_any)
		return value;			/* nothing moved; keep original datum */

	*did_split = true;
	/*
	 * Assemble the parent with KVMap creation forced off for this build only,
	 * so the W2.x parent is stock layout even if jsonb_sort_field_values=on
	 * globally.  Restore the flag unconditionally.
	 */
	{
		Jsonb	   *parent;

		jsonb_force_stock_layout = true;
		PG_TRY();
		{
			parent = JsonbValueToJsonb(pstate.result);
		}
		PG_FINALLY();
		{
			jsonb_force_stock_layout = false;
		}
		PG_END_TRY();

		return PointerGetDatum(parent);
	}
}

/*
 * jsonb_container_has_toasted
 *
 * O(1) predicate: does this jsonb root container carry the JB_FHAS_TOASTED
 * flag, i.e. did the split producer emit at least one JENTRY_ISTOASTED
 * descriptor among its top-level entries?  The flag is set in convertJsonbObject
 * when any value is jbvToasted, so the delete gate need not scan JEntries.
 *
 * Design note (W2.3a): the flag occupies the high bit of the former count field
 * (JB_CMASK narrowed 0x0FFFFFFF -> 0x07FFFFFF).  The remaining 27-bit count
 * (max 134217727 entries per container) is far above any practical jsonb value,
 * and no reader uses the header outside the JB_CMASK / JsonContainer* macros, so
 * narrowing the mask is safe.  Existing on-disk jsonb keeps reading correctly:
 * the reclaimed bit could only have been set by a container with >= 134M
 * entries, which does not occur in practice.
 */
static bool
jsonb_container_has_toasted(const JsonbContainer *jc)
{
	return JsonContainerHasToasted(jc);
}

/*
 * jsonb_datum_has_toasted
 *
 * Cheap entry point over a raw jsonb Datum: returns true iff the value is a
 * split form carrying nested cold-payload descriptors.  Does NOT detoast the
 * datum's own external storage and does NOT materialize any descriptor; it only
 * reads the root header + top-level JEntries, which are present in the leading
 * bytes of the value.  Callers on the delete path use this as the gate.
 */
bool
jsonb_datum_has_toasted(Datum jsonbval)
{
	Jsonb	   *jb = DatumGetJsonbP(jsonbval);
	bool		result = jsonb_container_has_toasted(&jb->root);

	/* DatumGetJsonbP may detoast-copy; free if it returned a new chunk. */
	if ((Pointer) jb != DatumGetPointer(jsonbval))
		pfree(jb);
	return result;
}

/*
 * jsonb_collect_external_refs
 *
 * Walk the top-level JEntries of a split jsonb value and return, by value, the
 * varatt_external descriptor of every JENTRY_ISTOASTED field -- i.e. every cold
 * payload that lives out of line as an ordinary TOAST value.  Returns NIL when
 * the value carries no descriptors.
 *
 * The returned pointers are the ordinary on-disk TOAST pointers originally
 * produced by toast_save_datum in W2.2; each can be handed straight to the
 * stock toast_delete_datum.  No materialization, no detoast of cold values, no
 * recursion (W2.2 only toasts top-level scalars).
 */
List *
jsonb_collect_external_refs(Datum jsonbval)
{
	Jsonb	   *jb = DatumGetJsonbP(jsonbval);
	const JsonbContainer *jc;
	uint32		nentries;
	uint32		count;
	char	   *baseAddr;
	int			i;
	List	   *result = NIL;

	jc = &jb->root;

	if (JsonContainerIsObject(jc))
	{
		count = JsonContainerSize(jc);
		nentries = 2 * count;
	}
	else if (JsonContainerIsArray(jc))
	{
		count = JsonContainerSize(jc);
		nentries = count;
	}
	else
	{
		if ((Pointer) jb != DatumGetPointer(jsonbval))
			pfree(jb);
		return NIL;
	}

	/*
	 * Data area starts directly after the JEntry array (stock jsonb layout;
	 * no KVMap block).  Per-entry data offsets come from the canonical
	 * getJsonbOffset(), so we never re-derive the stride/HAS_OFF hybrid by hand.
	 */
	baseAddr = (char *) (jc->children + nentries);

	for (i = 0; i < (int) nentries; i++)
	{
		JEntry		entry = jc->children[i];

		if (JBE_ISTOASTED(entry))
		{
			uint32		thisoff = getJsonbOffset(jc, i);
			const JsonbToastedDatum *desc =
				(const JsonbToastedDatum *) (baseAddr + INTALIGN(thisoff));
			struct varlena *fake;

			/*
			 * Rebuild an on-disk EXTERNAL varlena from the descriptor's stored
			 * varatt_external so we can hand it to stock toast_delete_datum.
			 */
			fake = (struct varlena *)
				palloc(VARHDRSZ_EXTERNAL + sizeof(struct varatt_external));
			SET_VARTAG_EXTERNAL(fake, VARTAG_ONDISK);
			memcpy(VARDATA_EXTERNAL(fake),
				   (const char *) desc + offsetof(JsonbToastedDatum, reserved) + sizeof(uint16),
				   sizeof(struct varatt_external));

			result = lappend(result, fake);
		}
	}

	if ((Pointer) jb != DatumGetPointer(jsonbval))
		pfree(jb);
	return result;
}

/*
 * jsonb_find_old_toasted_ref
 *
 * W2.4 reuse helper.  Key-based lookup in an OLD jsonb root container: if key
 * [keyVal,keyLen) exists and its value is a JENTRY_ISTOASTED descriptor, copy
 * the embedded on-disk varatt_external into *ext_out and return true.  Does NOT
 * materialize (no detoast): only the descriptor bytes are read.  Reuse must be
 * key-based, so the value slot is resolved through the KVMap exactly as the
 * stock reader does, never by ordinal position.
 *
 * Returns false if the container is not a top-level object, the key is absent,
 * or the value for that key is not JENTRY_ISTOASTED.
 */
bool
jsonb_find_old_toasted_ref(const JsonbContainer *container,
						   const char *keyVal, int keyLen,
						   struct varatt_external *ext_out)
{
	JsonbContainer *cont = unconstify(JsonbContainer *, container);
	JEntry	   *children = cont->children;
	int			count;
	char	   *baseAddr;
	uint32		stopLow,
				stopHigh;

	if (!JsonContainerIsObject(container))
		return false;
	count = JsonContainerSize(container);
	if (count <= 0)
		return false;

	/*
	 * Stock jsonb object layout: keys occupy JEntry slots [0..count-1], values
	 * slots [count..2*count-1]; the value for key i lives at slot i + count.
	 * Data follows the 2N JEntries directly.  No KVMap, no value reordering --
	 * W2.x operates on stock layout only.
	 */
	baseAddr = (char *) (children + count * 2);

	stopLow = 0;
	stopHigh = count;
	while (stopLow < stopHigh)
	{
		uint32		stopMiddle = stopLow + (stopHigh - stopLow) / 2;
		const char *candidateVal = baseAddr + getJsonbOffset(container, stopMiddle);
		int			candidateLen = getJsonbLength(container, stopMiddle);
		int			difference = lengthCompareJsonbString(candidateVal, candidateLen,
														  keyVal, keyLen);

		if (difference == 0)
		{
			int			index = stopMiddle + count;
			JEntry		ventry = children[index];

			if (!JBE_ISTOASTED(ventry))
				return false;	/* key found but value is not toasted */

			{
				uint32		val_off = getJsonbOffset(container, index);
				const JsonbToastedDatum *desc =
					(const JsonbToastedDatum *) (baseAddr + INTALIGN(val_off));

				memcpy(ext_out,
					   (const char *) desc +
					   offsetof(JsonbToastedDatum, reserved) + sizeof(uint16),
					   sizeof(struct varatt_external));
				return true;
			}
		}
		else if (difference < 0)
			stopLow = stopMiddle + 1;
		else
			stopHigh = stopMiddle;
	}
	return false;
}
