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
