/*-------------------------------------------------------------------------
 *
 * dummy_toaster.c
 *		Dummy toaster for tests
 *
 * Portions Copyright (c) 2023, Postgres Professional
 *
 * IDENTIFICATION
 *	  contrib/toastapi/dummy_toaster.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/toast_compression.h"
#include "access/toast_internals.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"

#include "toastapi.h"
#include "toastapi_internals.h"

static bool
dummy_toaster_validate(Oid toasteroid, Oid typeoid,
					   char storage, char compression,
					   Oid amoid, bool false_ok)
{
	if (typeoid != BYTEAOID)
	{
		if (false_ok)
			return false;

		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("toaster \"%s\" does not support type %s",
						get_toaster_name(toasteroid),
						format_type_be(typeoid))));
	}

	if (storage != TYPSTORAGE_EXTENDED)
	{
		if (false_ok)
			return false;

		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("toaster \"%s\" supports only %s",
						get_toaster_name(toasteroid), "STORAGE EXTENDED")));
	}

	if (compression != TOAST_PGLZ_COMPRESSION_ID)
	{
		if (false_ok)
			return false;

		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("toaster \"%s\" supports only pglz compression",
						get_toaster_name(toasteroid))));
	}

	return true;
}

static void
dummy_toaster_delete(ToasterContext tcxt, Datum oldval, bool is_speculative)
{
	void	   *ptr = VARATT_CUSTOM_GET_DATA(DatumGetPointer(oldval));

	elog(INFO, "dummy_toaster_delete");

	toast_delete_datum(tcxt->rel, PointerGetDatum(ptr), is_speculative);
}

static Datum
dummy_toaster_copy(ToasterContext tcxt, Datum newval, int options)
{
	elog(INFO, "dummy_toaster_copy");
	return (Datum) 0;
}

static Datum
dummy_toaster_toast(ToasterContext tcxt, Datum newval, Datum oldval,
					int max_inline_size, int options,
					char attstorage, ToastCompressionId cmid)
{
	Datum		toasted;
	void	   *res;
	int			size = VARATT_CUSTOM_SIZE(TOAST_POINTER_SIZE);

	elog(INFO, "dummy_toaster_toast");

	if (size > max_inline_size)
		return (Datum) 0;

	res = palloc(size);

	toasted = toast_save_datum(tcxt->rel, newval,
							   (struct varlena *) DatumGetPointer(oldval),
							   options);
	Assert(VARSIZE_ANY(DatumGetPointer(toasted)) == TOAST_POINTER_SIZE);

	SET_VARTAG_EXTERNAL(res, VARTAG_CUSTOM);
	VARATT_CUSTOM_SET_TOASTERID(res, tcxt->toasterid);
	VARATT_CUSTOM_SET_DATA_RAW_SIZE(res, VARSIZE_ANY(DatumGetPointer(newval)));
	VARATT_CUSTOM_SET_DATA_SIZE(res, TOAST_POINTER_SIZE);
	memcpy(VARATT_CUSTOM_GET_DATA(res), DatumGetPointer(toasted),
		   VARSIZE_ANY(DatumGetPointer(toasted)));

	return PointerGetDatum(res);
}

static Datum
dummy_toaster_update(ToasterContext tcxt, Datum newval, Datum oldval, int options)
{
	elog(INFO, "dummy_toaster_update");
	return (Datum) 0;
}

static Datum
dummy_toaster_detoast(ToasterContext tcxt, Datum toastptr,
					  int sliceoffset, int slicelength)
{
	void	   *ptr = VARATT_CUSTOM_GET_DATA(DatumGetPointer(toastptr));

	elog(INFO, "dummy_toaster_detoast");

	return PointerGetDatum(detoast_attr_slice(ptr, sliceoffset, slicelength));
}

PG_FUNCTION_INFO_V1(dummy_toaster_handler);

Datum
dummy_toaster_handler(PG_FUNCTION_ARGS)
{
	TsrRoutine *tsr = palloc0(sizeof(TsrRoutine));

	tsr->tsr_size = sizeof(TsrRoutine);
	tsr->tsr_toast = dummy_toaster_toast;
	tsr->tsr_delete = dummy_toaster_delete;
	tsr->tsr_copy = dummy_toaster_copy;
	tsr->tsr_update = dummy_toaster_update;
	tsr->tsr_detoast = dummy_toaster_detoast;
	tsr->tsr_validate = dummy_toaster_validate;

	PG_RETURN_POINTER(tsr);
}
