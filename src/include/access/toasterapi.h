/*-------------------------------------------------------------------------
 *
 * toasterapi.h
 *	  Pluggable TOAST API:  TsrRoutine struct and resolver hooks.
 *
 *	This header makes the toaster routine struct visible to core so that
 *	core call sites can dispatch through it without depending on the
 *	provider extension's private headers.  It also declares the two
 *	resolver hooks the provider extension installs at _PG_init time:
 *
 *	  get_toaster_id_for_rel_hook(Relation rel, AttrNumber attnum)
 *
 *	    Used by write-side lifecycle paths (toast/update/copy/delete)
 *	    where the call site has a Relation and column number in hand.
 *	    Returns the bound toaster's Oid, or InvalidOid when no toaster
 *	    is bound to this column.  The caller then composes with
 *	    get_toaster_routine_for_id_hook to obtain the routine.
 *
 *	  get_toaster_routine_for_id_hook(Oid toasterid)
 *
 *	    Used by both write side (after the rel hook resolved the Oid)
 *	    and read side (varatt_custom.va_toasterid provides the Oid).
 *	    Returns the routine pointer, or NULL when the toaster identity
 *	    is unknown to the provider.
 *
 *	Both hooks may be NULL if no provider is loaded:
 *	  - write side with no provider, or InvalidOid from the rel hook:
 *	    core falls through to vanilla TOAST behavior;
 *	  - read side with no provider, or NULL from the id hook on a
 *	    CUSTOM varlena: core ERRORs.  A CUSTOM varlena in the heap
 *	    with no provider to decode it is unrecoverable, not a silent
 *	    miss.
 *
 *	Ownership and lifetime contract for const TsrRoutine *
 *	------------------------------------------------------
 *
 *	The provider extension owns every TsrRoutine pointer it returns.
 *	The resolver returns a `const TsrRoutine *`: core is contractually
 *	read-only.  Core's role is strictly:
 *
 *	  - dereference fields (read-only access to function pointers);
 *	  - call methods using the documented context/argument shapes;
 *	  - treat each resolver call as fresh — core MUST NOT memoize the
 *	    returned pointer across statement or transaction boundaries.
 *
 *	Core MUST NOT:
 *
 *	  - pfree, repalloc, or otherwise free the routine;
 *	  - mutate any field of the routine;
 *	  - copy the routine and dispatch through the copy;
 *	  - retain the pointer past the scope of the call site that
 *	    resolved it.
 *
 *	The provider MUST guarantee:
 *
 *	  - the pointer is valid for the entire duration of the call site
 *	    that resolved it (single method invocation);
 *	  - if the provider caches and returns the same pointer across
 *	    multiple resolver calls, the routine is immutable for the
 *	    cache lifetime;
 *	  - on relcache invalidation, the provider invalidates any cached
 *	    routine bindings before the next resolver call returns.
 *
 *	tsr_validate is part of the struct contract but is NOT dispatched
 *	via either resolver hook.  It is invoked at CREATE TOASTER time
 *	through the provider extension's catalog command.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 2016-2026, Postgres Professional
 *
 * src/include/access/toasterapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOASTERAPI_H
#define TOASTERAPI_H

#include "access/toast_compression.h"
#include "access/toast_custom.h"
#include "utils/relcache.h"

/*
 * Context passed to write-side lifecycle methods.  Stack-allocated by
 * the core call site, lives for the duration of one method call.
 * Provider MUST NOT store this pointer past method return.
 */
typedef struct ToasterContextData
{
	Relation		rel;
	Oid				toasterid;
	Oid				toastreloid;
	int				attnum;
	int				options;
} ToasterContextData;

typedef ToasterContextData *ToasterContext;

/*
 * Callback signatures.
 *
 * Write-side methods take ToasterContext (Relation + attnum available).
 * Read-side methods (detoast, size) take only the varlena because at
 * read time there is no Relation in scope; toaster identity comes
 * from varatt_custom.va_toasterid.
 */

/* Write side: serialize value into a CUSTOM varlena. */
typedef Datum (*toaster_toast_function) (ToasterContext tcxt,
										 Datum value,
										 Datum old_value,
										 int max_inline_size,
										 int am_options,
										 char att_storage,
										 ToastCompressionId cmid);

/* Write side: update an existing CUSTOM-stored value.  Optional. */
typedef Datum (*toaster_update_function) (ToasterContext tcxt,
										  Datum new_value,
										  Datum old_value,
										  int am_options);

/* Write side: copy CUSTOM-stored value across relations (CTAS, ALTER).  Optional. */
typedef Datum (*toaster_copy_function) (ToasterContext tcxt,
										Datum value,
										int am_options);

/* Write side: release storage for a CUSTOM value on row delete. */
typedef void (*toaster_delete_function) (ToasterContext tcxt,
										 Datum value,
										 bool is_speculative);

/* Read side: turn a CUSTOM varlena back into the normal-form Datum. */
typedef Datum (*toaster_detoast_function) (ToasterContext tcxt,
										   Datum toast_ptr,
										   int offset,
										   int length);

/*
 * Read side: report size of a CUSTOM varlena.  No ToasterContext: the
 * varlena's va_toasterid identifies the provider; sz_type selects
 * which size (raw, datum, storage).  Will be wired through the
 * resolver-by-id in the lifecycle-conversion patch (0004); declared
 * here so the struct shape is stable at 0003 land.
 */
typedef Size (*toaster_size_function) (const void *custom_varlena,
									   ToastPtrSizeType sz_type);

/* Catalog-time validation of (typeoid, storage, compression, amoid) tuple. */
typedef bool (*toaster_validate_function) (Oid toasteroid, Oid typeoid,
										   char storage, char compression,
										   Oid amoid, bool false_ok);

/*
 * Routine struct.  Must be palloc'd in a single chunk by the provider.
 *
 * tsr_size is the only ABI-stability marker.  Providers MUST set it to
 * sizeof(TsrRoutine) at construction.  Core verifies on the first
 * dispatch that the routine size matches its compile-time view of the
 * struct; a mismatch means the provider was built against a different
 * TsrRoutine layout (e.g., struct grew or shrank between core and
 * provider compilation) and is unsafe to dispatch.
 *
 * No NodeTag, no separate magic word.  Routines are not part of the
 * node system; ABI versioning lives in tsr_size only.
 */
typedef struct TsrRoutine
{
	Size		tsr_size;		/* must equal sizeof(TsrRoutine) */

	/* mandatory */
	toaster_validate_function	tsr_validate;
	toaster_toast_function		tsr_toast;
	toaster_detoast_function	tsr_detoast;

	/* optional — may be NULL */
	toaster_update_function		tsr_update;
	toaster_copy_function		tsr_copy;
	toaster_delete_function		tsr_delete;

	/*
	 * Read-side size resolver.  May be NULL in 0003 (this commit);
	 * 0004 makes it mandatory once the call sites flip to
	 * resolver-by-id dispatch.
	 */
	toaster_size_function		tsr_size_fn;
} TsrRoutine;

/*
 * Resolver hook types — Option A (two-hook split).
 *
 * Write side: (Relation, AttrNumber) -> Oid.  Returns InvalidOid when
 * no toaster is bound to the column.  Caller composes:
 *
 *     Oid toasterid = get_toaster_id_for_rel_hook(rel, attno);
 *     if (OidIsValid(toasterid))
 *         routine = get_toaster_routine_for_id_hook(toasterid);
 *
 * Read side: Oid -> const TsrRoutine *.  Oid comes from
 * varatt_custom.va_toasterid.  Returns NULL when the toaster identity
 * is unknown to the provider (e.g., extension was unloaded after the
 * CUSTOM value was written).  Core treats a NULL on the read side as
 * a hard ERROR — there is no silent fallback for a CUSTOM varlena
 * whose provider is unavailable.
 *
 * The asymmetric handling reflects the asymmetric data model:
 *
 *   write: catalog says "no custom toaster bound" -> vanilla TOAST
 *          is a correct, lossless fallback;
 *
 *   read:  the heap already contains a CUSTOM varlena with a
 *          va_toasterid; without the provider we cannot decode it.
 *          Silent fallback would corrupt query results, so ERROR.
 */
typedef Oid (*get_toaster_id_for_rel_hook_type) (Relation rel,
												 AttrNumber attnum);
typedef const TsrRoutine *(*get_toaster_routine_for_id_hook_type) (Oid toasterid);

/*
 * Resolver hook globals.  Installed by the provider extension at
 * _PG_init.  NULL when no provider is loaded.
 */
extern PGDLLIMPORT get_toaster_id_for_rel_hook_type
				   get_toaster_id_for_rel_hook;
extern PGDLLIMPORT get_toaster_routine_for_id_hook_type
				   get_toaster_routine_for_id_hook;

/*
 * Core helpers for dispatching toast lifecycle through the resolver.
 * They wrap routine lookup, context fill, and any per-column metadata
 * (e.g., compression method derivation from pg_attribute), so that
 * core call sites stay short and the metadata extraction lives in one
 * place.  Return (Datum) 0 when no provider/routine/method is
 * available, letting the caller fall through to vanilla TOAST.
 */
extern Datum dispatch_toaster_toast(Relation rel, AttrNumber attnum,
									Datum value, int max_inline_size,
									int am_options);
extern Datum dispatch_toaster_update(Relation rel, AttrNumber attnum,
									 Datum new_value, Datum old_value,
									 int am_options);
extern Datum dispatch_toaster_copy(Relation rel, AttrNumber attnum,
								   Datum value, int am_options);
extern void  dispatch_toaster_delete(Relation rel, AttrNumber attnum,
									 Datum value, bool is_speculative);

#endif							/* TOASTERAPI_H */
