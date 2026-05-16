/*-------------------------------------------------------------------------
 *
 * jsonb_toaster_lite_pointers.c
 *	 Custom-varlena pointer constructors for jsonb_toaster_lite.
 *
 * Source-of-truth provenance:
 *	 Ported from postgrespro/postgres@jsonb_toaster (HEAD 583a292),
 *	 contrib/jsonb_toaster/jsonb_toast_internals.c lines 40..386,
 *	 minus the array/chunked-array makers (jsonx_toast_make_pointer_array
 *	 and jsonx_toast_wrap_array_into_pointer at lines 171..208), and
 *	 with all jsonx_, Jsonx, and JSONX_ names renamed to
 *	 jbtl_, Jbtl, and JBTL_. Original names are noted in
 *	 the per-function port-trace comments.
 *
 *	 No chunk-machinery (writer, fetcher, detoast iterator) lives in
 *	 this file -- those are in jsonb_toaster_lite_chain.c.
 *
 * Copyright (c) 2026, Postgres Professional
 *
 * IDENTIFICATION
 *	 contrib/jsonb_toaster_lite/jsonb_toaster_lite_pointers.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "access/toast_custom.h"

#include "jsonb_toaster_lite.h"


/*
 * jbtl_write_custom_toast_pointer_header
 *	Ported from jsonxWriteCustomToastPointerHeader (internals.c:40..67).
 *
 *	Lay out a VARATT_CUSTOM varlena with INTALIGN'd 32-bit mode-tag and
 *	a `datalen`-byte data area. Returns a pointer to the first byte of
 *	the data area (suitably aligned), so the caller can fill it in.
 *
 *	`rawsize` is the original (pre-toast) size of the value; it is
 *	written into the varatt_custom header for downstream sliced detoast
 *	planning.
 */
char *
jbtl_write_custom_toast_pointer_header(char *ptr, Oid toasterid, uint32 header,
									   int datalen, int rawsize)
{
	Size		hdrsize = VARATT_CUSTOM_SIZE(0);
	Size		aligned_hdrsize = INTALIGN(hdrsize);
	Size		size = aligned_hdrsize + sizeof(header) + datalen;

	SET_VARTAG_EXTERNAL(ptr, VARTAG_CUSTOM);

	VARATT_CUSTOM_SET_TOASTERID(ptr, toasterid);
	VARATT_CUSTOM_SET_DATA_RAW_SIZE(ptr, rawsize);

	if (size - hdrsize > VARATT_CUSTOM_MAX_DATA_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("attribute length too large")));

	VARATT_CUSTOM_SET_DATA_SIZE(ptr, size - hdrsize);

	if (aligned_hdrsize != hdrsize)
		memset((char *) ptr + hdrsize, 0, aligned_hdrsize - hdrsize);

	*(uint32 *)((char *) ptr + aligned_hdrsize) = header;

	return (char *) ptr + aligned_hdrsize + sizeof(header);
}


/*
 * jbtl_toast_make_custom_pointer
 *	Ported from jsonx_toast_make_custom_pointer (internals.c:69..82).
 *
 *	Allocate a fresh varlena large enough to hold the JBTL custom-ptr
 *	header plus `datalen` bytes of payload, and lay out the header in
 *	place. Returns the varlena and yields the data-area pointer through
 *	*pdata. Caller fills the data area according to the chosen mode.
 */
static struct varlena *
jbtl_toast_make_custom_pointer(Oid toasterid, uint32 header,
							   int datalen, int rawsize, char **pdata)
{
	struct varlena *result = palloc(JBTL_CUSTOM_PTR_HEADER_SIZE + datalen);

	*pdata = jbtl_write_custom_toast_pointer_header((char *) result, toasterid,
													header, datalen, rawsize);

	Assert((intptr_t) *pdata == INTALIGN((intptr_t) *pdata));

	return result;
}


/*
 * jbtl_toast_make_plain_pointer
 *	Ported from jsonx_toast_make_plain_pointer (internals.c:83..96).
 *
 *	Wrap a fully-inline jsonb body (no out-of-line chunks) in a
 *	JBTL_PLAIN_JSONB custom-varlena. Used when the encoded jsonb is
 *	small enough to live inside the row plus its custom-pointer header.
 *
 *	`jbc` points to the in-memory JsonbContainer (header + JEntries +
 *	data area, exactly as a jsonb body looks after VARDATA(jb)).
 */
struct varlena *
jbtl_toast_make_plain_pointer(Oid toasterid, JsonbContainer *jbc, int len)
{
	char	   *data;
	int			datalen = VARHDRSZ + len;
	struct varlena *result =
		jbtl_toast_make_custom_pointer(toasterid, JBTL_PLAIN_JSONB,
									   datalen, datalen, &data);

	SET_VARSIZE(data, datalen);
	memcpy(data + VARHDRSZ, jbc, len);

	return result;
}


/*
 * jbtl_toast_make_pointer_compressed_chunks
 *	Ported from jsonx_toast_make_pointer_compressed_chunks
 *	(internals.c:123..140).
 *
 *	Build a JBTL_POINTER_COMPRESSED_CHUNKS custom-varlena: a TOAST
 *	pointer whose chunk rows carry per-chunk-compressed payload. Used
 *	by the sliced detoast iterator so reading byte-range [a..b) only
 *	needs to fetch and decompress the chunks that overlap that range.
 *
 *	No inline tail; the only inline data is the wrapped varatt_external.
 *
 *	Public: callers in chunk-machinery (writer)
 *	wrap a toast pointer in this mode after writing per-chunk-compressed
 *	rows.
 */
struct varlena *
jbtl_toast_make_pointer_compressed_chunks(Oid toasterid,
										  struct varatt_external *toast_pointer,
										  int rawsize)
{
	char	   *data;
	struct varlena *custom_ptr =
		jbtl_toast_make_custom_pointer(toasterid,
									   JBTL_POINTER_COMPRESSED_CHUNKS,
									   TOAST_POINTER_SIZE,
									   rawsize, &data);

	SET_VARTAG_EXTERNAL(data, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(data), toast_pointer, sizeof(*toast_pointer));

	return custom_ptr;
}


/*
 * jbtl_toast_wrap_in_jbtl_pointer
 *	Wrap a bare TOAST pointer in a JBTL_POINTER custom-varlena.
 *
 *	Symmetric to jbtl_toast_make_pointer_compressed_chunks, but for
 *	plain (uncompressed) chunks. The postgrespro source emits a bare
 *	(unwrapped) TOAST pointer for the "ntids=0, no diff, no
 *	compressed_chunks" case (the dispatcher branch at the end of
 *	jsonxMakeToastPointer). jsonb_toaster_lite needs the wrap to
 *	exist regardless of mode so that core routes reads through
 *	tsr_detoast and our reader is exercised.
 *
 *	This function is the single concrete builder for that wrap;
 *	tsr_toast calls it after jbtl_toast_save_datum returns the bare
 *	pointer.
 */
struct varlena *
jbtl_toast_wrap_in_jbtl_pointer(Oid toasterid,
								struct varatt_external *toast_pointer)
{
	char	   *data;
	struct varlena *custom_ptr =
		jbtl_toast_make_custom_pointer(toasterid,
									   JBTL_POINTER,
									   TOAST_POINTER_SIZE,
									   toast_pointer->va_rawsize, &data);

	SET_VARTAG_EXTERNAL(data, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(data), toast_pointer, sizeof(*toast_pointer));

	return custom_ptr;
}


/*
 * jbtl_toast_make_pointer_diff
 *	Build a JBTL_POINTER_DIFF (or JBTL_POINTER_DIFF_COMP) custom-varlena
 *	wrapping an unchanged base toast pointer plus an inline byte
 *	overlay.
 *
 *	Format mirrors postgrespro's jsonx_toast_make_pointer_diff:
 *
 *	 [varatt_custom header]
 *	 [varatt_external base — unchanged, points at existing chunks]
 *	 [JbtlPointerDiff: int32 offset; char data[diff_len]]
 *
 *	`diff_offset` is the byte offset within the assembled jsonb body
 *	where the overlay applies (taken absolute, just like the
 *	postgrespro original). `diff_len` is encoded implicitly as
 *	(inline_size - offsetof(JbtlPointerDiff, data)) at read time.
 *
 *	The outer custom-pointer keeps `va_rawsize = base.va_rawsize` —
 *	the BASE total size. This matches the historical contract and
 *	enforces same-byte-length replacement: the apply path allocates
 *	a buffer of base.va_rawsize and the overlay must not extend past
 *	[diff_offset + diff_len) within it.
 *
 *	Caller is responsible for verifying that diff_len matches the
 *	old-byte-length of the value at that offset; this constructor
 *	only assembles bytes.
 */
struct varlena *
jbtl_toast_make_pointer_diff(Oid toasterid,
							 struct varatt_external *base_pointer,
							 bool compressed_chunks,
							 int32 diff_offset, int32 diff_len,
							 const void *diff_data)
{
	JbtlPointerDiff *diff;
	char	   *data;
	int			datalen =
		TOAST_POINTER_SIZE + offsetof(JbtlPointerDiff, data) + diff_len;
	struct varlena *custom_ptr =
		jbtl_toast_make_custom_pointer(toasterid,
									   compressed_chunks ?
										   JBTL_POINTER_DIFF_COMP :
										   JBTL_POINTER_DIFF,
									   datalen,
									   base_pointer->va_rawsize, &data);

	SET_VARTAG_EXTERNAL(data, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(data), base_pointer, sizeof(*base_pointer));

	diff = (JbtlPointerDiff *) (data + TOAST_POINTER_SIZE);
	memcpy(&diff->offset, &diff_offset, sizeof(diff_offset));
	memcpy(diff->data, diff_data, diff_len);

	return custom_ptr;
}


/*
 * jbtl_toast_make_pointer_subtree
 *	Build a JBTL_POINTER_SUBTREE custom-varlena. The given parent_body
 *	bytes are copied verbatim into the custom-varlena's data area;
 *	caller is responsible for ensuring that any JEntries with
 *	JBTL_JENTRY_ISCONTAINER_PTR have a valid JbtlToastedContainerPointer
 *	payload at the corresponding value-data position.
 *
 *	The custom-varlena's va_rawsize is set to parent_body_size — that is
 *	the size of the AS-STORED body, not the assembled size after children
 *	are spliced in. Callers that need the assembled size must ask the
 *	reader (jbtl_detoast) which computes it dynamically.
 *
 *	 use: only from the test-fixture path. will produce the
 *	same custom-varlena from the production initial-spill path.
 */
struct varlena *
jbtl_toast_make_pointer_subtree(Oid toasterid,
								const char *parent_body, int32 parent_body_size)
{
	char	   *data;
	struct varlena *custom_ptr =
		jbtl_toast_make_custom_pointer(toasterid,
									   JBTL_POINTER_SUBTREE,
									   parent_body_size,
									   parent_body_size, &data);

	memcpy(data, parent_body, parent_body_size);

	return custom_ptr;
}


/*
 * jbtl_toast_make_pointer_subtree_v1 — production constructor.
 *
 *	Produces a JBTL_POINTER_SUBTREE custom-varlena with a v1
 *	JbtlSubtreeHeader prefix carrying parent identity. Reader
 *
 *	updated to skip the header before assembling.
 *
 *	Layout of the resulting payload:
 *	 [ JbtlSubtreeHeader (16 bytes, v1) ][ parent body bytes ]
 */
struct varlena *
jbtl_toast_make_pointer_subtree_v1(Oid toasterid,
								   Oid parent_valueid,
								   Oid parent_toastrelid,
								   const char *parent_body,
								   int32 parent_body_size)
{
	int32		payload_size = (int32) sizeof(JbtlSubtreeHeader) + parent_body_size;
	char	   *data;
	struct varlena *custom_ptr =
		jbtl_toast_make_custom_pointer(toasterid,
									   JBTL_POINTER_SUBTREE,
									   payload_size,
									   payload_size, &data);
	JbtlSubtreeHeader *hdr = (JbtlSubtreeHeader *) data;

	hdr->version = JBTL_SUBTREE_HEADER_V1;
	hdr->flags = 0;
	hdr->header_size = (uint16) sizeof(JbtlSubtreeHeader);
	hdr->parent_valueid = parent_valueid;
	hdr->parent_toastrelid = parent_toastrelid;

	memcpy(data + sizeof(JbtlSubtreeHeader), parent_body, parent_body_size);

	return custom_ptr;
}
