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
 *	  get_toaster_routine_for_rel_hook(Relation rel, AttrNumber attnum,
 *	                                   ToasterContext outcxt)
 *
 *	    Used by write-side lifecycle paths (toast/update/copy/delete)
 *	    where the call site has a Relation and column number in hand.
 *	    If outcxt is non-NULL, the resolver populates outcxt->toasterid,
 *	    outcxt->toastreloid (and any other identity fields it needs)
 *	    before returning, so the caller can pass &outcxt to the routine
 *	    methods that need toaster identity to embed into output
 *	    varlenas (VARATT_CUSTOM_SET_TOASTERID).
 *
 *	  get_toaster_routine_for_id_hook(Oid toasterid)
 *
 *	    Used by read-side paths (detoast, size) where the call site has
 *	    only a CUSTOM-tagged varlena.  The varlena self-identifies via
 *	    varatt_custom.va_toasterid (see access/toast_custom.h).
 *
 *	Both hooks return TsrRoutine *.  Both may be NULL if no provider is
 *	loaded; in that case core falls through to vanilla TOAST behavior
 *	on the write side and ERRORs on the read side (a CUSTOM varlena in
 *	the heap with no provider to decode it is unrecoverable, not a
 *	silent miss).
 *
 *	Ownership and lifetime contract for TsrRoutine *
 *	-----------------------------------------------
 *
 *	The provider extension owns every TsrRoutine pointer it returns.
 *	Core's role is strictly:
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
#include "nodes/nodes.h"
#include "postgres.h"
#include "utils/relcache.h"

/* magic word: ASCII "TsrR" */
#define TSR_ROUTINE_MAGIC	0x54747252

/*
 * Context passed to write-side lifecycle methods.  Stack-allocated by
 * the core call site, lives for the duration of one method call.
 * Provider MUST NOT store this pointer past method return.
 */
typedef struct ToasterContextData
{
	struct TsrRoutine *toaster;
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
 * NodeTag follows PG access-method conventions (cf. IndexAmRoutine,
 * TsmRoutine).  Future code may use IsA() once T_TsrRoutine is added
 * to nodes.h; until then, type carries T_Invalid.
 */
typedef struct TsrRoutine
{
	NodeTag			type;		/* future T_TsrRoutine */
	uint32			tsr_magic;	/* TSR_ROUTINE_MAGIC */

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
	 * 0004 makes it mandatory once the call sites flip from the flat
	 * Toastapi_size_hook to resolver-by-id dispatch.
	 */
	toaster_size_function		tsr_size;
} TsrRoutine;

/*
 * Convenience constructor for providers.  Sets magic and zeroes
 * everything else; provider fills in the methods.
 */
static inline TsrRoutine *
MakeTsrRoutine(void)
{
	TsrRoutine *tsr = palloc0(sizeof(*tsr));

	tsr->type = T_Invalid;		/* until T_TsrRoutine added */
	tsr->tsr_magic = TSR_ROUTINE_MAGIC;

	return tsr;
}

/*
 * Resolver hook types.
 *
 * The write-side resolver may optionally fill an output ToasterContext
 * with the toaster's identity (toasterid, toastreloid) so that the
 * caller can pass it to routine methods without re-querying the
 * catalog.  When outcxt is NULL the resolver still returns the
 * routine but skips the identity fill.
 */
typedef TsrRoutine *(*toaster_routine_for_rel_hook_type) (Relation rel,
														  AttrNumber attnum,
														  ToasterContext outcxt);
typedef TsrRoutine *(*toaster_routine_for_id_hook_type) (Oid toasterid);

/*
 * Resolver hook globals.  Installed by the provider extension at
 * _PG_init.  NULL when no provider is loaded.
 */
extern PGDLLIMPORT toaster_routine_for_rel_hook_type
				   get_toaster_routine_for_rel_hook;
extern PGDLLIMPORT toaster_routine_for_id_hook_type
				   get_toaster_routine_for_id_hook;

#endif							/* TOASTERAPI_H */
