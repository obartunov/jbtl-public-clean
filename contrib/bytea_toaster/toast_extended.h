/*-------------------------------------------------------------------------
 *
 * toast_extended.h
 *	  Internal definitions for the bytea appendable toaster.
 *
 * Copyright (c) 2000-2022, PostgreSQL Global Development Group
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * contrib/bytea_toaster/toast_extended.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOAST_EXTENDED_H
#define TOAST_EXTENDED_H

#include "postgres.h"

#include "utils/rel.h"
#include "storage/itemptr.h"

/*
 * BYTEA_TOASTER_MAGIC and ByteaToastRoutine used to live here as the
 * append-fast-path registration ABI between bytea_toaster.c and core
 * byteacat (src/backend/utils/adt/varlena.c).  They were consumed via
 * TsrRoutine.get_vtable, which the cleaned toastapi removed
 * (commit 3ab8a37).  Both definitions became unreachable and were
 * deleted.
 *
 * The cleaned line does NOT replace this with a new Toast API hook.
 * A bytea-specific operator fast path does not belong on the general
 * Toast API surface (the Toast API is a lifecycle interface).  If
 * appendable bytea is ever reopened, the right shape is a type-local
 * bytea provider interface that the bytea type's operators can probe,
 * not a Toastapi_*_hook.  See contrib/toastapi/README.toastapi
 * Section X.
 */

typedef uint64 AppendableToastVersion;

typedef bool (*ToastChunkVisibilityCheck)(SysScanDesc toastscan, AppendableToastVersion attrversion,
										  char **chunkdata, int32 *chunksize);

extern Datum toast_save_datum_ext(Relation rel, Oid toastrelid, Oid toasteroid, Datum value,
								  struct varlena *oldexternal, int options, int attnum,
								  void *chunk_header, int chunk_header_size);

extern void
toast_fetch_toast_slice(Oid toastrelid, Oid valueid,
						struct varlena *attr, int32 attrsize,
						int32 sliceoffset, int32 slicelength,
						struct varlena *result, int32 header_size,
						ToastChunkVisibilityCheck visibility_check,
						AppendableToastVersion attrversion);

extern void
toast_update_datum(Datum value,
				   void *slice_data, int slice_offset, int slice_length,
				   void *chunk_header, int chunk_header_size,
				   ToastChunkVisibilityCheck visibility_check,
				   AppendableToastVersion attrversion, int options);

extern void
toast_delete_datum_ext(Datum value, bool is_speculative,
					   int32 header_size,
					   ToastChunkVisibilityCheck visibility_check,
					   AppendableToastVersion attrversion);

#endif							/* TOAST_EXTENDED_H */
