/*-------------------------------------------------------------------------
 *
 * typelifecycle.h
 *	  Internal in-core registry mapping a type OID to a set of lifecycle
 *	  callbacks for type-owned external references (split values).
 *
 * This is NOT the user-facing toaster API.  There is:
 *	  no CREATE TOASTER / set_toaster;
 *	  no catalog binding;
 *	  no per-value toasterid dispatch;
 *	  no read-side resolver / CUSTOM carrier dependency.
 *
 * A "split" value is an ordinary (inline-stock) varlena of a type that keeps
 * some payload out-of-line as type-owned external references embedded inside
 * the value (e.g. jsonb with JENTRY_ISTOASTED descriptors).  Generic heap /
 * rewrite code must not know the concrete type: it only asks, for an
 * attribute's typid, whether a TypeLifecycleRoutine is registered, and if so
 * invokes the callback needed at that point.  All knowledge of the on-disk
 * layout, of what is warm vs cold, and of the read path stays type-local.
 *
 * Ownership rules (fixed; see W3 M1 design):
 *	- has_external_refs is header-only / cheap: it takes only a Datum, may not
 *	  receive a Relation/AttrNumber, and must not detoast or hit the catalog.
 *	- collect_external_refs is a FORMAT EXPERT: it returns the embedded refs;
 *	  generic code performs the ordinary toast deletion.  It does not delete.
 *	- copy_or_relocate performs relocation ATOMICALLY and returns the rewritten
 *	  value: it walks descriptors, relocates cold payload into the new toast
 *	  relation, and updates refs.  Generic code never learns the layout.
 *
 * src/include/access/typelifecycle.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPELIFECYCLE_H
#define TYPELIFECYCLE_H

#include "postgres.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"

/*
 * Context passed to the write/lifecycle/rewrite callbacks, which run only
 * where a typed context exists (a Relation is in scope).  has_external_refs
 * deliberately does NOT take this: it must stay header-only.
 */
typedef struct TypeLifecycleContext
{
	Relation	rel;			/* relation owning the attribute */
	int			attnum;			/* 1-based attribute number */
	uint32		options;		/* heap/toast insert options */
	uint32		max_inline_size;	/* core-derived warm/cold threshold hint */
} TypeLifecycleContext;

typedef struct TypeLifecycleRoutine
{
	/*
	 * Cheap, header-only: does this value contain type-owned external refs?
	 * Takes only the Datum so it cannot detoast or consult the catalog.
	 */
	bool		(*has_external_refs) (Datum value);

	/*
	 * Format expert: return the embedded on-disk external refs for cleanup.
	 * Generic code deletes them via the ordinary toast mechanism.  Does NOT
	 * delete anything itself.
	 */
	List	   *(*collect_external_refs) (Datum value);

	/*
	 * Create the split representation if needed (toast-time create).  May
	 * ignore old_value.
	 *
	 * CONTRACT: when nothing is split, return the input value unchanged (same
	 * pointer); when split, return a new value.  Generic code detects whether
	 * a split happened by pointer identity (result != input), not by an
	 * out-param.  A routine that rebuilds an identical value on a no-op would
	 * break this and must not.
	 */
	Datum		(*toast_or_split) (Datum value, Datum old_value,
								   const TypeLifecycleContext *ctx);

	/*
	 * Optional byte-exact reuse on update.  May be NULL (then generic code
	 * falls back to full re-split).
	 */
	Datum		(*update_or_reuse) (Datum new_value, Datum old_value,
									const TypeLifecycleContext *ctx);

	/*
	 * Rewrite / VACUUM FULL / CLUSTER relocation.  Walks descriptors, fetches
	 * the old raw (compressed) payload, saves it into the new toast relation
	 * (ctx->rel), updates the embedded refs, and returns the rewritten value.
	 * Performs relocation atomically; generic code does not see the layout.
	 */
	Datum		(*copy_or_relocate) (Datum value,
									 const TypeLifecycleContext *ctx);
}			TypeLifecycleRoutine;

/*
 * Registration is static, in-core C only.  In-core types register at a
 * controlled init point before any DML can reach a lifecycle path; extension
 * types may register from their _PG_init later.  No SQL, no catalog.
 */
extern void RegisterTypeLifecycleRoutine(Oid typid,
										 const TypeLifecycleRoutine *routine);

/*
 * Generic heap/rewrite uses only this: given an attribute's type OID, is a
 * lifecycle routine registered?  Returns NULL when not (ordinary path).
 */
extern const TypeLifecycleRoutine *lookup_type_lifecycle_routine(Oid typid);

/*
 * Aggregate registration of all in-core lifecycle types.  Implemented in
 * typelifecycle_builtins.c; called once per backend from the init path.  The
 * generic registry above stays type-agnostic; the builtins file is the one
 * place that knows the in-core type list.
 */
extern void RegisterAllInCoreTypeLifecycleRoutines(void);

#endif							/* TYPELIFECYCLE_H */
