/*-------------------------------------------------------------------------
 *
 * bytea_toaster.c
 *		Appendable bytea toaster.
 *
 * Portions Copyright (c) 2016-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1990-1993, Regents of the University of California
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 *	  contrib/bytea_toaster/bytea_toaster.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "access/toast_compression.h"
#include "executor/tuptable.h"
#include "toast_extended.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/memutils.h"

#include "toastapi.h"
#include "access/toast_custom.h"

PG_MODULE_MAGIC;

#define BYTEA_INVALID_VERSION	0
#define BYTEA_FIRST_VERSION		1

typedef struct AppendableToastData
{
	varatt_external ptr;
	AppendableToastVersion version;
	int32		inline_tail_size;
	char	   *inline_tail_data; /* [FLEXIBLE_ARRAY_MEMBER]; */
} AppendableToastData;

#define VARATT_CUSTOM_APPENDABLE_HDRSZ \
	offsetof(AppendableToastData, inline_tail_size) + sizeof(int32)

#define VARATT_CUSTOM_APPENDABLE_SIZE(inline_size) \
	VARATT_CUSTOM_SIZE(VARATT_CUSTOM_APPENDABLE_HDRSZ + (inline_size))

#define VARATT_CUSTOM_GET_APPENDABLE_DATA(attr, data) \
do { \
	varattrib_1b_e *attrc = (varattrib_1b_e *)(attr); \
	Assert(VARATT_IS_CUSTOM(attrc)); \
	Assert(VARSIZE_CUSTOM(attrc) >= VARATT_CUSTOM_APPENDABLE_SIZE(0)); \
	memcpy(&(data), VARATT_CUSTOM_GET_DATA(attrc), VARATT_CUSTOM_APPENDABLE_HDRSZ); \
	(data).inline_tail_data = VARATT_CUSTOM_GET_DATA(attrc) + VARATT_CUSTOM_APPENDABLE_HDRSZ; \
} while (0)

static bool
bytea_toaster_validate(Oid toasteroid, Oid typeoid, char storage, char compression,
					   Oid amoid, bool false_ok)

{
	if (typeoid == BYTEAOID &&
		storage == TYPSTORAGE_EXTERNAL)
		return true;

	if (!false_ok)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" supports only type \"%s\" and uncompressed storage",
						"bytea_toaster", "bytea")));

	return false;
}

static Datum
bytea_toaster_make_pointer(Oid toasterid, struct varatt_external *ptr,
						   AppendableToastVersion version,
						   Size inline_tail_size, char **pdata)
{
	Size		size = VARATT_CUSTOM_APPENDABLE_SIZE(inline_tail_size);
	struct varlena *result = palloc0(size);
	AppendableToastData result_data = {0};

	SET_VARTAG_EXTERNAL(result, VARTAG_CUSTOM);

	if (ptr->va_rawsize + inline_tail_size > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("atribute length too large")));

	VARATT_CUSTOM_SET_TOASTERID(result, toasterid);
	VARATT_CUSTOM_SET_DATA_RAW_SIZE(result, ptr->va_rawsize + inline_tail_size);
	VARATT_CUSTOM_SET_DATA_SIZE(result, VARATT_CUSTOM_APPENDABLE_HDRSZ + inline_tail_size);

	result_data.ptr = *ptr;
	result_data.version = version;
	result_data.inline_tail_size = inline_tail_size;
	memcpy(VARATT_CUSTOM_GET_DATA(result), &result_data, VARATT_CUSTOM_APPENDABLE_HDRSZ);
	if (pdata)
		*pdata = VARATT_CUSTOM_GET_DATA(result) + VARATT_CUSTOM_APPENDABLE_HDRSZ;

	return PointerGetDatum(result);
}

static bool
bytea_toaster_check_visibility(SysScanDesc toastscan, AppendableToastVersion attrversion,
							   char **chunkdata, int32 *chunksize)
{
	AppendableToastVersion chunkversion;
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *)(toastscan->slot);
	TM_Result res;

	Assert(*chunksize > sizeof(chunkversion));
	memcpy(&chunkversion, *chunkdata, sizeof(chunkversion));

	Assert(TTS_IS_BUFFERTUPLE(toastscan->slot));
	Assert(BufferIsValid(bslot->buffer));

	LockBuffer(bslot->buffer, BUFFER_LOCK_SHARE);
	res = HeapTupleSatisfiesUpdate(bslot->base.tuple, GetCurrentCommandId(false),
									   bslot->buffer);
	LockBuffer(bslot->buffer, BUFFER_LOCK_UNLOCK);

	if (res == TM_Ok || res == TM_SelfModified || res == TM_BeingModified)
		return chunkversion <= attrversion ? true : false;
	else if (res == TM_Invisible) /* the tail chunk may become invisible after update */
		return chunkversion == attrversion ? true : false;
	else
		return false;
}

static void
bytea_toaster_delete(ToasterContext tcxt, Datum val, bool is_speculative)
{
	AppendableToastData data;
	char		ptr[TOAST_POINTER_SIZE];

	Assert(VARATT_IS_CUSTOM(DatumGetPointer(val)));
	VARATT_CUSTOM_GET_APPENDABLE_DATA(val, data);

	SET_VARTAG_EXTERNAL(ptr, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(ptr), &data.ptr, sizeof(data.ptr));

	toast_delete_datum_ext(PointerGetDatum(ptr), is_speculative,
						   sizeof(AppendableToastVersion),
						   bytea_toaster_check_visibility, data.version);
}

static Datum
bytea_toaster_copy(ToasterContext tcxt, Datum newval, int options)
{
	Datum		detoasted_newval;
	Datum		toasted_newval;
	struct varatt_external toast_ptr;
	AppendableToastVersion version = BYTEA_FIRST_VERSION;

	detoasted_newval = PointerGetDatum(detoast_attr((struct varlena *) DatumGetPointer(newval)));

	toasted_newval = toast_save_datum_ext(tcxt->rel,
										  tcxt->toastreloid,
										  tcxt->toasterid,
										  detoasted_newval, NULL,
										  options, tcxt->attnum,
										  &version, sizeof(version));

	Assert(VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(toasted_newval)));
	VARATT_EXTERNAL_GET_POINTER(toast_ptr, DatumGetPointer(toasted_newval));

	pfree(DatumGetPointer(toasted_newval));
	if (detoasted_newval != newval)
		pfree(DatumGetPointer(detoasted_newval));

	return bytea_toaster_make_pointer(tcxt->toasterid, &toast_ptr, version, 0, NULL);
}

static Datum
bytea_toaster_toast_append(ToasterContext tcxt, AppendableToastData *new_data, int options)
{
	char		ptr[TOAST_POINTER_SIZE];
	AppendableToastVersion version = new_data->version + 1;
	struct varatt_external toast_ptr = new_data->ptr;
	Size		toasted_size = VARATT_EXTERNAL_GET_EXTSIZE(toast_ptr);
	Size		new_size = toasted_size + new_data->inline_tail_size;

	SET_VARTAG_EXTERNAL(ptr, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(ptr), &toast_ptr, sizeof(toast_ptr));

	toast_update_datum(PointerGetDatum(ptr),
					   new_data->inline_tail_data,
					   toasted_size,
					   new_data->inline_tail_size,
					   &version, sizeof(version),
					   bytea_toaster_check_visibility, new_data->version, options);

	toast_ptr.va_rawsize = new_size + VARHDRSZ;
	VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(toast_ptr, new_size, TOAST_PGLZ_COMPRESSION_ID);

	return bytea_toaster_make_pointer(tcxt->toasterid, &toast_ptr, version, 0, NULL);
}

static Datum
bytea_toaster_toast(ToasterContext tcxt, Datum newval, Datum oldval,
					int max_inline_size, int options,
					char attstorage, ToastCompressionId cmid)
{
	Assert(attstorage == TYPSTORAGE_EXTERNAL);
	Assert(cmid == TOAST_INVALID_COMPRESSION_ID);

	if (VARATT_CUSTOM_APPENDABLE_SIZE(0) > max_inline_size)	/* FIXME */
		return (Datum) 0;

	if (VARATT_IS_CUSTOM(DatumGetPointer(newval)) &&
		VARATT_CUSTOM_GET_TOASTERID(DatumGetPointer(newval)) == tcxt->toasterid)
	{
		AppendableToastData new_data;

		VARATT_CUSTOM_GET_APPENDABLE_DATA(newval, new_data);

		if (new_data.ptr.va_toastrelid == tcxt->toastreloid)
			return bytea_toaster_toast_append(tcxt, &new_data, options);
	}

	return bytea_toaster_copy(tcxt, newval, options);
}

static Datum
bytea_toaster_update(ToasterContext tcxt, Datum newval, Datum oldval, int options)
{
	AppendableToastData old_data;
	AppendableToastData new_data;
	Oid			toastrelid = tcxt->toastreloid;

	/*
	 * Companion to β bridge: old_val must be CUSTOM (the column was
	 * already toaster-owned), but new_val may now be a regular varlena
	 * (e.g. a literal bytea returned from the SQL UPDATE).  The append
	 * optimization below only applies when new is also a CUSTOM
	 * appendable pointer; for any other new, decline so the core
	 * fallback retoasts plain new into a fresh chain.
	 */
	Assert(VARATT_IS_CUSTOM(DatumGetPointer(oldval)));
	if (!VARATT_IS_CUSTOM(DatumGetPointer(newval)))
		return (Datum) 0;

	VARATT_CUSTOM_GET_APPENDABLE_DATA(oldval, old_data);
	VARATT_CUSTOM_GET_APPENDABLE_DATA(newval, new_data);

	/*
	 * Append new chunks, if new value is from current toast relation
	 * and new value's pointer/version matches old's ones.
	 */
	if (new_data.ptr.va_toastrelid == toastrelid &&
		new_data.ptr.va_toastrelid == old_data.ptr.va_toastrelid &&
		new_data.ptr.va_valueid == old_data.ptr.va_valueid &&
		new_data.version == old_data.version &&
		memcmp(&old_data.ptr, &new_data.ptr, sizeof(old_data.ptr)) == 0)
	{
#if 1
		/*
		 * bytea_toaster_toast_append() will be called later from
		 * bytea_toaster_toast()
		 */
		return newval;
#else
		return bytea_toaster_toast_append(tcxt, &new_data, options);
#endif
	}

	/* Use default update algorithm with full retoast */
	return (Datum) 0;
}

static Datum
bytea_toaster_detoast(ToasterContext tcxt, Datum toastptr,
					  int sliceoffset, int slicelength)
{
	AppendableToastData data;
	struct varlena *result;
	int32		attrsize;
	int32		inline_size;
	int32		toasted_size;

	Assert(VARATT_IS_CUSTOM(DatumGetPointer(toastptr)));
	VARATT_CUSTOM_GET_APPENDABLE_DATA(toastptr, data);

	toasted_size = VARATT_EXTERNAL_GET_EXTSIZE(data.ptr);
	inline_size = data.inline_tail_size;

	attrsize = toasted_size + inline_size;

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		slicelength = 0;
	}

	/*
	 * When fetching a prefix of a compressed external datum, account for the
	 * space required by va_tcinfo, which is stored at the beginning as an
	 * int32 value.
	 */
	if (VARATT_EXTERNAL_IS_COMPRESSED(data.ptr) && slicelength > 0)
		slicelength = slicelength + sizeof(int32);

	/*
	 * Adjust length request if needed.  (Note: our sole caller,
	 * detoast_attr_slice, protects us against sliceoffset + slicelength
	 * overflowing.)
	 */
	if (((sliceoffset + slicelength) > attrsize) || slicelength < 0)
		slicelength = attrsize - sliceoffset;

	result = (struct varlena *) palloc(slicelength + VARHDRSZ);

	if (VARATT_EXTERNAL_IS_COMPRESSED(data.ptr))
		SET_VARSIZE_COMPRESSED(result, slicelength + VARHDRSZ);
	else
		SET_VARSIZE(result, slicelength + VARHDRSZ);

	if (sliceoffset + slicelength > attrsize - inline_size)
	{
		int32		size = Min(sliceoffset + slicelength - (attrsize - inline_size), inline_size);
		int32		inline_offset = Max(0, sliceoffset - (attrsize - inline_size));

		size = Min(size, slicelength);

		memcpy(VARDATA(result) + slicelength - size,
			   data.inline_tail_data + inline_offset, size);

		slicelength -= size;
	}

	if (slicelength > 0)
	{
		toast_fetch_toast_slice(data.ptr.va_toastrelid, data.ptr.va_valueid,
								(struct varlena *) toastptr,	/* XXX */
								toasted_size, sliceoffset, slicelength,
								result, sizeof(AppendableToastVersion),
								bytea_toaster_check_visibility, data.version);
	}

	return PointerGetDatum(result);
}

/*
 * Note: bytea_toaster_append() and bytea_toaster_vtable() used to live
 * here as the registrants of the appendable-bytea fast path on the
 * byteacat (`||`) operator.  The cleaned toastapi minimization
 * (commit 3ab8a37) removed the TsrRoutine.get_vtable field, so there
 * is no way to register them and core byteacat (src/backend/utils/adt/
 * bytea.c) no longer probes for them.  They became unreachable and
 * were removed.  Direct INSERT/UPDATE through tsr_toast/tsr_update
 * still produce appendable storage; only the `||`-operator fast path
 * is currently absent.  See contrib/toastapi/README.toastapi Section X
 * and contrib/bytea_toaster/README.bytea_toaster.
 */

PG_FUNCTION_INFO_V1(bytea_toaster_handler);
Datum
bytea_toaster_handler(PG_FUNCTION_ARGS)
{
	TsrRoutine *tsr = MakeTsrRoutine();

	tsr->tsr_validate = bytea_toaster_validate;
	tsr->tsr_toast = bytea_toaster_toast;
	tsr->tsr_detoast = bytea_toaster_detoast;
	tsr->tsr_delete = bytea_toaster_delete;
	tsr->tsr_copy = bytea_toaster_copy;
	tsr->tsr_update = bytea_toaster_update;

	PG_RETURN_POINTER(tsr);
}
