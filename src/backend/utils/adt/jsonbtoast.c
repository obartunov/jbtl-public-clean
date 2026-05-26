/*-------------------------------------------------------------------------
 *
 * jsonbtoast.c
 *	  jsonb TypeLifecycleRoutine: binds jsonb's split/cleanup/relocate logic
 *	  into the in-core type lifecycle registry (see access/typelifecycle.h).
 *
 * M1.1: the callbacks here wrap the existing jsonb functions with no behavior
 * change.  The six JSONBOID call sites in heaptoast.c still run as before; this
 * file only makes the routine exist and registers it.  Later milestones flip
 * the call sites to dispatch through the registry (M1.2) and wire the P5
 * copy_or_relocate (rewrite relocation) wires in at a later milestone.
 *
 * All jsonb format knowledge stays in jsonb code: this file is the only bridge
 * between the generic registry and jsonb's internal routines.
 *
 * src/backend/utils/adt/jsonbtoast.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/typelifecycle.h"
#include "catalog/pg_type_d.h"
#include "utils/jsonb.h"

/*
 * has_external_refs: header-only check.  Callers always strip VARATT_IS_EXTERNAL
 * before reaching a lifecycle path, so on a (non-external) inline split parent
 * jsonb_datum_has_toasted does not detoast; it inspects the jsonb header.
 */
static bool
jsonb_lifecycle_has_external_refs(Datum value)
{
	return jsonb_datum_has_toasted(value);
}

static List *
jsonb_lifecycle_collect_external_refs(Datum value)
{
	return jsonb_collect_external_refs(value);
}

static Datum
jsonb_lifecycle_toast_or_split(Datum value, Datum old_value,
							   const TypeLifecycleContext *ctx)
{
	bool		did_split = false;

	/*
	 * Wrap the existing producer.  old_value/old_isnull handling and the
	 * warm/cold threshold stay jsonb-local; ctx carries the relation and the
	 * core-derived max_inline_size hint, but we keep using the jsonb threshold
	 * to preserve identical behavior.
	 */
	return jsonb_toast_split_datum(ctx->rel, value, old_value,
								   /* old_isnull */ old_value == (Datum) 0,
								   JSONB_TOAST_SPLIT_VALUE_MIN,
								   ctx->options, &did_split);
}

/*
 * update_or_reuse and copy_or_relocate are reserved for later milestones
 * (update parity / rewrite relocation).
 */
static Datum
jsonb_lifecycle_copy_or_relocate(Datum value, const TypeLifecycleContext *ctx)
{
	/*
	 * Physical descriptor-walking relocation into the new toast relation
	 * (ctx->rel).  No JsonbIterator on cold values; raw compressed fetch +
	 * save with oldexternal so rd_toastoid reuse avoids decompress/recompress.
	 * Returns the original Datum unchanged when nothing needed relocating.
	 */
	return jsonb_rewrite_relocate_split(ctx->rel, value);
}

static const TypeLifecycleRoutine jsonb_lifecycle_routine =
{
	.has_external_refs = jsonb_lifecycle_has_external_refs,
	.collect_external_refs = jsonb_lifecycle_collect_external_refs,
	.toast_or_split = jsonb_lifecycle_toast_or_split,
	.update_or_reuse = NULL,
	.copy_or_relocate = jsonb_lifecycle_copy_or_relocate,
};

/*
 * Registered from a controlled in-core init point before any DML can reach a
 * jsonb lifecycle path.
 */
void
jsonb_register_lifecycle_routine(void)
{
	RegisterTypeLifecycleRoutine(JSONBOID, &jsonb_lifecycle_routine);
}
