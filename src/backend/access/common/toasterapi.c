/*-------------------------------------------------------------------------
 *
 * toasterapi.c
 *	  Storage for the Pluggable TOAST API resolver hooks and
 *	  per-column dispatch helpers.
 *
 *	Both resolver hooks default to NULL.  The provider extension
 *	(typically contrib/toastapi) installs them at _PG_init time;
 *	when no provider is loaded both stay NULL and core falls back to
 *	vanilla TOAST behavior on writes, ERRORs on reads of a CUSTOM
 *	varlena.
 *
 *	See src/include/access/toasterapi.h for the ownership contract
 *	and Option A resolver split.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 2016-2026, Postgres Professional
 *
 * src/backend/access/common/toasterapi.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/toast_compression.h"
#include "access/toast_custom.h"
#include "access/toasterapi.h"
#include "catalog/pg_attribute.h"
#include "utils/rel.h"

get_toaster_id_for_rel_hook_type		get_toaster_id_for_rel_hook = NULL;
get_toaster_routine_for_id_hook_type	get_toaster_routine_for_id_hook = NULL;

/*
 * Sanity-check the routine the provider just handed us.  We require
 * the provider's compile-time view of TsrRoutine to match core's:
 * any divergence in size means struct layout drift and dispatching
 * would read/write the wrong slot.
 *
 * Called from each dispatch helper after a successful resolve.  Cheap
 * (one Size compare) and is the only ABI marker we keep.
 */
static inline void
toasterapi_check_routine_size(const TsrRoutine *routine)
{
	if (routine->tsr_size != sizeof(TsrRoutine))
		elog(ERROR,
			 "toaster routine size mismatch: provider says %zu, core expects %zu",
			 routine->tsr_size, sizeof(TsrRoutine));
}

/*
 * Compose the two resolver hooks (Option A) to get the routine bound
 * to a column.  Returns NULL when no provider is loaded, no toaster
 * is bound to the column, or the provider does not recognize the
 * Oid.  Fills toasterid out-param on success (used by callers that
 * need to set ToasterContextData.toasterid before dispatch).
 */
static const TsrRoutine *
resolve_routine_for_rel(Relation rel, AttrNumber attnum, Oid *out_toasterid)
{
	Oid				toasterid;
	const TsrRoutine *routine;

	*out_toasterid = InvalidOid;

	if (get_toaster_id_for_rel_hook == NULL ||
		get_toaster_routine_for_id_hook == NULL)
		return NULL;

	toasterid = get_toaster_id_for_rel_hook(rel, attnum);
	if (!OidIsValid(toasterid))
		return NULL;

	routine = get_toaster_routine_for_id_hook(toasterid);
	if (routine == NULL)
		return NULL;

	toasterapi_check_routine_size(routine);
	*out_toasterid = toasterid;
	return routine;
}

/*
 * Helper for write-side TOAST call sites: resolve the per-column
 * routine, fill the context, derive compression id from pg_attribute,
 * and dispatch to tsr_toast.
 *
 * Returns the new (CUSTOM) Datum on success.  Returns (Datum) 0 if
 * the caller should fall through to vanilla toast_save_datum, which
 * happens when:
 *   - no provider is loaded (resolver hooks NULL);
 *   - rel hook returns InvalidOid (no toaster bound to this column);
 *   - id hook returns NULL (provider does not recognize the Oid);
 *   - the resolved routine doesn't implement tsr_toast.
 *
 * Strict-error mode is reserved for the read side; on write we
 * tolerate the absence and let vanilla TOAST take over.
 */
Datum
dispatch_toaster_toast(Relation rel, AttrNumber attnum, Datum value,
					   int max_inline_size, int am_options)
{
	ToasterContextData	tcxt;
	const TsrRoutine   *routine;
	TupleDesc			tupdesc;
	Form_pg_attribute	att;
	ToastCompressionId	cmid;
	Oid					toasterid;

	routine = resolve_routine_for_rel(rel, attnum, &toasterid);
	if (routine == NULL || routine->tsr_toast == NULL)
		return (Datum) 0;

	if (!OidIsValid(rel->rd_rel->reltoastrelid))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("toast relation is missing for toasted attribute "
						"%d of relation %u",
						(int) attnum, RelationGetRelid(rel))));

	memset(&tcxt, 0, sizeof(tcxt));
	tcxt.rel = rel;
	tcxt.toasterid = toasterid;
	tcxt.toastreloid = rel->rd_rel->reltoastrelid;
	tcxt.attnum = (int) attnum + 1;
	tcxt.options = am_options;

	tupdesc = RelationGetDescr(rel);
	att = TupleDescAttr(tupdesc, attnum);

	if (att->attstorage == TYPSTORAGE_PLAIN ||
		att->attstorage == TYPSTORAGE_EXTERNAL)
	{
		cmid = TOAST_INVALID_COMPRESSION_ID;
	}
	else
	{
		switch (att->attcompression != InvalidCompressionMethod ?
				att->attcompression : default_toast_compression)
		{
			case TOAST_PGLZ_COMPRESSION:
				cmid = TOAST_PGLZ_COMPRESSION_ID;
				break;
			case TOAST_LZ4_COMPRESSION:
				cmid = TOAST_LZ4_COMPRESSION_ID;
				break;
			default:
				Assert(false);
				cmid = TOAST_INVALID_COMPRESSION_ID;
				break;
		}
	}

	return routine->tsr_toast(&tcxt, value, value,
							  max_inline_size, am_options,
							  att->attstorage, cmid);
}

/*
 * Helper for write-side delete call sites.  Resolves the routine and
 * dispatches to tsr_delete.  Silent no-op if no toaster is bound, to
 * preserve current behavior on rows that predate the provider.
 */
void
dispatch_toaster_delete(Relation rel, AttrNumber attnum, Datum value,
						bool is_speculative)
{
	ToasterContextData	tcxt;
	const TsrRoutine   *routine;
	Oid					toasterid;

	routine = resolve_routine_for_rel(rel, attnum, &toasterid);
	if (routine == NULL || routine->tsr_delete == NULL)
		return;

	memset(&tcxt, 0, sizeof(tcxt));
	tcxt.rel = rel;
	tcxt.toasterid = toasterid;
	tcxt.toastreloid = OidIsValid(rel->rd_rel->reltoastrelid)
					   ? rel->rd_rel->reltoastrelid : InvalidOid;
	tcxt.attnum = (int) attnum + 1;

	routine->tsr_delete(&tcxt, value, is_speculative);
}

/*
 * Helper for write-side UPDATE call sites.  Returns the rewritten
 * CUSTOM Datum on success, or (Datum) 0 to indicate that the caller
 * should fall back to delete-and-retoast.  Falls back when:
 *   - no provider is loaded;
 *   - no toaster is bound to this column;
 *   - the new value is CUSTOM but identifies a different toaster
 *     than the old value;
 *   - the column's bound toaster does not match the old value's
 *     toaster (i.e. someone re-bound the toaster after the original
 *     write);
 *   - the routine does not implement tsr_update.
 *
 * The first two guards match toastapi_update's original logic; this
 * keeps existing behavior byte-identical across the conversion.
 */
Datum
dispatch_toaster_update(Relation rel, AttrNumber attnum,
						Datum new_value, Datum old_value, int am_options)
{
	ToasterContextData	tcxt;
	const TsrRoutine   *routine;
	struct varlena	   *new_val;
	struct varlena	   *old_val;
	Oid					old_toasterid;
	Oid					col_toasterid;

	new_val = (struct varlena *) DatumGetPointer(new_value);
	old_val = (struct varlena *) DatumGetPointer(old_value);
	Assert(VARATT_IS_CUSTOM(old_val));

	old_toasterid = VARATT_CUSTOM_GET_TOASTERID(old_val);

	if (VARATT_IS_CUSTOM(new_val))
	{
		Oid new_toasterid = VARATT_CUSTOM_GET_TOASTERID(new_val);

		if (new_toasterid != old_toasterid)
			return (Datum) 0;
	}

	routine = resolve_routine_for_rel(rel, attnum, &col_toasterid);
	if (routine == NULL || col_toasterid != old_toasterid)
		return (Datum) 0;
	if (routine->tsr_update == NULL)
		return (Datum) 0;

	memset(&tcxt, 0, sizeof(tcxt));
	tcxt.rel = rel;
	tcxt.toasterid = col_toasterid;
	tcxt.toastreloid = OidIsValid(rel->rd_rel->reltoastrelid)
					   ? rel->rd_rel->reltoastrelid : InvalidOid;
	tcxt.attnum = (int) attnum + 1;
	tcxt.options = am_options;

	return routine->tsr_update(&tcxt, new_value, old_value, am_options);
}

/*
 * Helper for write-side COPY call sites (CTAS, ALTER TABLE rewrites).
 * Returns the new Datum on success, or (Datum) 0 to indicate caller
 * fallback.  The value is expected to be CUSTOM and its toasterid
 * must match the column's bound toaster.
 */
Datum
dispatch_toaster_copy(Relation rel, AttrNumber attnum, Datum value,
					  int am_options)
{
	ToasterContextData	tcxt;
	const TsrRoutine   *routine;
	Oid					col_toasterid;
	Oid					value_toasterid;

	value_toasterid = VARATT_CUSTOM_GET_TOASTERID(DatumGetPointer(value));

	routine = resolve_routine_for_rel(rel, attnum, &col_toasterid);
	if (routine == NULL || col_toasterid != value_toasterid ||
		routine->tsr_copy == NULL)
		return (Datum) 0;

	memset(&tcxt, 0, sizeof(tcxt));
	tcxt.rel = rel;
	tcxt.toasterid = col_toasterid;
	tcxt.toastreloid = OidIsValid(rel->rd_rel->reltoastrelid)
					   ? rel->rd_rel->reltoastrelid : InvalidOid;
	tcxt.attnum = (int) attnum + 1;
	tcxt.options = am_options;

	return routine->tsr_copy(&tcxt, value, am_options);
}
