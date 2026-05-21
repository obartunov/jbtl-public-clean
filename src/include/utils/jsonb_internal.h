/*-------------------------------------------------------------------------
 *
 * jsonb_internal.h
 *	  Internal declarations shared between jsonb_util.c and jsonfuncs.c.
 *
 *	This header is NOT a stable API. It exists to keep
 *	implementation-detail helpers out of the public jsonb.h header.
 *	Out-of-tree code should not depend on anything declared here; the
 *	declarations may change shape or disappear without notice.
 *
 * Copyright (c) 2014-2026, PostgreSQL Global Development Group
 *
 * src/include/utils/jsonb_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef JSONB_INTERNAL_H
#define JSONB_INTERNAL_H

#include "utils/jsonb.h"

/*
 * Sliced-read helper for top-level jsonb -> 'key' / jsonb ->> 'key'
 * lookups when the source body is an external on-disk TOAST varlena.
 * Avoids detoasting the whole body for a single scalar key by fetching
 * a structural prefix via detoast_attr_slice, locating the key with
 * the same logical rules as getKeyJsonValueFromContainer, and (when
 * safe) fetching only the value's byte range.
 *
 * Return value tells the caller exactly what happened:
 *
 *   JSONB_KEY_LOOKUP_FOUND
 *       *res holds a fully self-contained scalar JsonbValue. Any
 *       internal TOAST-slice memory used during the lookup has been
 *       freed before return. The caller can materialise the result
 *       (JsonbValueToJsonb / JsonbValueAsText) at any time, including
 *       across executor step boundaries.
 *
 *   JSONB_KEY_LOOKUP_MISSING
 *       The key was proven absent from the object. *res is untouched.
 *       The caller should return SQL NULL.
 *
 *   JSONB_KEY_LOOKUP_FALLBACK
 *       The helper cannot answer (inline / non-external Datum,
 *       non-object root, nested-container value, value past prefix on
 *       compressed external, value exceeding the conservative
 *       half-body cap, an unexpected scalar type, etc.). *res is
 *       untouched. The caller should fall through to the
 *       PG_DETOAST_DATUM + getKeyJsonValueFromContainer path.
 *
 * The helper may detect some physical inconsistencies (JEntry walk
 * past body size, header size mismatch, short slice fetch) and raise
 * ERRCODE_DATA_CORRUPTED. The pre-existing slow path does NOT perform
 * equivalent bounds checks: on the same corrupt bytes it may segfault,
 * return undefined data, or rarely succeed by accident. The fast path
 * therefore tightens corruption detection for already-corrupt rows.
 * This is a behaviour change: queries that previously crashed or
 * returned garbage on corrupt jsonb bodies may now raise a clean
 * ERRCODE_DATA_CORRUPTED.
 */
typedef enum JsonbKeyLookupResult
{
	JSONB_KEY_LOOKUP_FOUND,
	JSONB_KEY_LOOKUP_MISSING,
	JSONB_KEY_LOOKUP_FALLBACK,
} JsonbKeyLookupResult;

extern JsonbKeyLookupResult getKeyJsonValueFromExternal(Datum raw,
														const char *keyVal,
														int keyLen,
														JsonbValue *res);

#endif							/* JSONB_INTERNAL_H */
