/*-------------------------------------------------------------------------
 *
 * toasterapi.c
 *	  Storage for the Pluggable TOAST API resolver hooks.
 *
 *	Both resolver hooks default to NULL.  The provider extension
 *	(typically contrib/toastapi) installs them at _PG_init time;
 *	when no provider is loaded both stay NULL and core falls back to
 *	vanilla TOAST behavior on writes, ERRORs on reads of a CUSTOM
 *	varlena.
 *
 *	See src/include/access/toasterapi.h for the ownership contract.
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

toaster_routine_for_rel_hook_type	get_toaster_routine_for_rel_hook = NULL;
toaster_routine_for_id_hook_type	get_toaster_routine_for_id_hook = NULL;

/*
 * Helper for write-side TOAST call sites: resolve the per-column
 * routine, fill the context, derive compression id from pg_attribute,
 * and dispatch to tsr_toast.
 *
 * Returns the new (CUSTOM) Datum on success.  Returns (Datum) 0 if
 * the caller should fall through to vanilla toast_save_datum, which
 * happens when:
 *   - no provider is loaded (resolver hook NULL);
 *   - resolver returns NULL (no toaster bound to this column);
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
	TsrRoutine		   *routine;
	TupleDesc			tupdesc;
	Form_pg_attribute	att;
	ToastCompressionId	cmid;

	if (get_toaster_routine_for_rel_hook == NULL)
		return (Datum) 0;

	memset(&tcxt, 0, sizeof(tcxt));
	routine = get_toaster_routine_for_rel_hook(rel, attnum, &tcxt);
	if (routine == NULL || routine->tsr_toast == NULL)
		return (Datum) 0;

	if (!OidIsValid(rel->rd_rel->reltoastrelid))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("toast relation is missing for toasted attribute "
						"%d of relation %u",
						(int) attnum, RelationGetRelid(rel))));

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
	TsrRoutine		   *routine;

	if (get_toaster_routine_for_rel_hook == NULL)
		return;

	memset(&tcxt, 0, sizeof(tcxt));
	routine = get_toaster_routine_for_rel_hook(rel, attnum, &tcxt);
	if (routine == NULL || routine->tsr_delete == NULL)
		return;

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
	TsrRoutine		   *routine;
	struct varlena	   *new_val;
	struct varlena	   *old_val;
	Oid					old_toasterid;

	if (get_toaster_routine_for_rel_hook == NULL)
		return (Datum) 0;

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

	memset(&tcxt, 0, sizeof(tcxt));
	routine = get_toaster_routine_for_rel_hook(rel, attnum, &tcxt);
	if (routine == NULL || tcxt.toasterid != old_toasterid)
		return (Datum) 0;
	if (routine->tsr_update == NULL)
		return (Datum) 0;

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
	TsrRoutine		   *routine;
	Oid					value_toasterid;

	if (get_toaster_routine_for_rel_hook == NULL)
		return (Datum) 0;

	value_toasterid = VARATT_CUSTOM_GET_TOASTERID(DatumGetPointer(value));

	memset(&tcxt, 0, sizeof(tcxt));
	routine = get_toaster_routine_for_rel_hook(rel, attnum, &tcxt);
	if (routine == NULL || tcxt.toasterid != value_toasterid ||
		routine->tsr_copy == NULL)
		return (Datum) 0;

	tcxt.options = am_options;
	return routine->tsr_copy(&tcxt, value, am_options);
}
