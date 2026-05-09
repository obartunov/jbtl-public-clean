/*-------------------------------------------------------------------------
 *
 * jsonb_toaster_lite.h
 *	  Internal types and prototypes for jsonb_toaster_lite.
 *
 * Source-of-truth provenance:
 *	  Internals are ported from postgrespro/postgres@jsonb_toaster
 *	  (HEAD 583a292), specifically:
 *	    - contrib/jsonb_toaster/jsonb_toaster.h (mode tags, structs,
 *	      pointer macros, pointer-constructor externs)
 *	    - contrib/jsonb_toaster/jsonb_toast_internals.c lines 40..386
 *	      (pointer-constructor function bodies)
 *	  All jsonx_* / Jsonx* / JSONX_* names from the postgrespro source
 *	  are renamed to jbtl_* / Jbtl* / JBTL_* in this contrib.  Original
 *	  names appear only as port-trace comments next to each ported entity.
 *
 * GSON discipline:
 *	  This contrib is GSON-free by construction.  No symbols from the
 *	  postgrespro generic-jsonb dispatch family are introduced here.
 *	  Encoder and decoder logic for jsonb (including the KVMap from
 *	  K1) lives in core src/backend/utils/adt/jsonb_util.c and is
 *	  consumed via the public utils/jsonb.h API only.
 *
 * Copyright (c) 2026, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */
#ifndef JSONB_TOASTER_LITE_H
#define JSONB_TOASTER_LITE_H

#include "postgres.h"
#include "access/genam.h"
#include "access/toast_compression.h"
#include "fmgr.h"
#include "storage/itemptr.h"
#include "utils/jsonb.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"
#include "varatt.h"

#include "varatt_custom.h"


/* ---- pointer mode tags (top 4 bits of the 32-bit custom-pointer header)
 *
 *	Ported names:  JSONX_*  ->  JBTL_*
 *
 *	Layout: bits 28..31 form a small enum tag; bits 0..27 are mode-specific
 *	payload (e.g. number of inline TIDs in DIRECT_TIDS modes).
 *
 *	JBTL_PLAIN_JSONB means the custom-varlena holds an inline jsonb body
 *	with no out-of-line TOAST chunks.  Other modes hold a varatt_external
 *	plus optional inline tail (TID list, diff bytes, etc.).
 *
 *	The JSONX_CHUCKED_ARRAY tag (0x70000000 in the source) is intentionally
 *	NOT ported in this commit — chunked arrays are deferred.
 *
 *	The bit values are preserved verbatim so a forensic diff against the
 *	postgrespro source is trivial.
 */
#define JBTL_POINTER_TYPE_MASK				0xF0000000
#define JBTL_PLAIN_JSONB					0x00000000
#define JBTL_POINTER						0x10000000
#define JBTL_POINTER_DIRECT_TIDS			0x20000000
#define JBTL_POINTER_DIRECT_TIDS_COMP		0x30000000
#define JBTL_POINTER_COMPRESSED_CHUNKS		0x40000000
#define JBTL_POINTER_DIFF					0x50000000
#define JBTL_POINTER_DIFF_COMP				0x60000000
#define JBTL_POINTER_SUBTREE				0x70000000	/* M5.0a: parent body
														 * inline + JEntries
														 * may have
														 * ISCONTAINER_PTR */


/* ---- M5.0a: JEntry type bit for subtree pointer.
 *
 *	Master jsonb's JENTRY_TYPEMASK is 0x70000000 with ISCONTAINER at
 *	0x50000000.  Slots 0x60000000 and 0x70000000 are unused.  Postgrespro
 *	uses 0x60000000 for ISCONTAINER_PTR; we adopt the same slot in lite's
 *	body format so a future merge with master / postgrespro is mechanical.
 *
 *	A JEntry tagged JBTL_JENTRY_ISCONTAINER_PTR points to a
 *	JbtlToastedContainerPointer payload at the value's byte offset
 *	(INTALIGN'd) inside the body's value-data area.
 *
 *	JBTL_JBC_TOBJECT_TOASTED is a non-mandatory hint bit on the parent
 *	container header.  When set, readers know the body has at least one
 *	ISCONTAINER_PTR; when clear, readers can skip the JEntry-walk fast
 *	path.  M5.0a always sets it for emitted SUBTREE bodies but does not
 *	rely on it for correctness — every JEntry is checked individually.
 */
#define JBTL_JENTRY_ISCONTAINER_PTR		0x60000000
#define JBTL_JBE_ISCONTAINER_PTR(je_)	\
	(((je_) & JENTRY_TYPEMASK) == JBTL_JENTRY_ISCONTAINER_PTR)

#define JBTL_JBC_TOBJECT_TOASTED		0x10000000	/* hint on root header */

/*
 * Inline payload stored at the value-data position of an ISCONTAINER_PTR
 * JEntry.  Layout:	  [ JEntry hdr — copy of original child container header ]
 *	  [ varatt_external — 18 bytes — pointer to child toast chain ]
 *
 *	Total: VARHDRSZ_CUSTOM-aligned size = 4 + TOAST_POINTER_SIZE = 22
 *	bytes.  The JEntry's length field encodes this total size.
 *
 *	The header field carries the original container's 4-byte header
 *	(type bits + count) so the reader can dispatch without first
 *	fetching the child chunks just to learn the type.
 */
typedef struct JbtlToastedContainerPointer
{
	JEntry			header;
	char			data[FLEXIBLE_ARRAY_MEMBER];	/* varatt_external bytes */
} JbtlToastedContainerPointer;


/*
 * M5.0b-3: versioned header at the front of every JBTL_POINTER_SUBTREE
 * payload emitted by the production writer.  16 bytes, 4-byte aligned.
 *
 *	version = 0  →  M5.0a fixture legacy.  No header at the front;
 *	                payload begins immediately with the parent body.
 *	                Reader keeps support; writers do not emit.  Such
 *	                rows are NEVER admitted to jbtl_subtree_refs.
 *
 *	version = 1  →  Production writer emits this.  Header carries the
 *	                synthetic parent_valueid and parent_toastrelid so
 *	                delete dispatch (M5.0b-4) can key refs without
 *	                walking up to the heap tuple.
 *
 *	header_size  →  Size in bytes of the JbtlSubtreeHeader itself.
 *	                Matches sizeof(JbtlSubtreeHeader) for v1.  Reader
 *	                uses this to find the start of the parent body
 *	                without compile-time-baking the offset.
 */
typedef struct JbtlSubtreeHeader
{
	uint8			version;
	uint8			flags;			/* reserved, MUST be 0 in v1 */
	uint16			header_size;	/* 16 in v1 */
	Oid				parent_valueid;
	Oid				parent_toastrelid;
} JbtlSubtreeHeader;

#define JBTL_SUBTREE_HEADER_V1		1


/* ---- custom-pointer header layout macros
 *
 *	Ported names: JSONX_CUSTOM_PTR_*  ->  JBTL_CUSTOM_PTR_*
 *
 *	Inside a VARATT_CUSTOM varlena, the layout is:
 *	    [ varatt_custom header ][ INTALIGN pad ][ uint32 mode-tag ][ data ]
 *
 *	JBTL_CUSTOM_PTR_HEADER_SIZE is the offset from the start of the
 *	varlena to the first byte of `data`.  The macros below let callers
 *	read the mode tag and access the data area.
 */
#define JBTL_CUSTOM_PTR_HEADER_SIZE \
	(INTALIGN(VARATT_CUSTOM_SIZE(0)) + sizeof(uint32))

#define JBTL_CUSTOM_PTR_GET_HEADER(ptr) \
	(*(uint32 *)((char *) (ptr) + INTALIGN(VARATT_CUSTOM_SIZE(0))))

#define JBTL_CUSTOM_PTR_GET_DATA(ptr) \
	((char *) (ptr) + JBTL_CUSTOM_PTR_HEADER_SIZE)

#define JBTL_CUSTOM_PTR_GET_DATA_SIZE(ptr) \
	(VARATT_CUSTOM_SIZE(VARATT_CUSTOM_GET_DATA_SIZE(ptr)) - JBTL_CUSTOM_PTR_HEADER_SIZE)


/* ---- pointer-constructor externs
 *
 *	Function bodies live in jsonb_toaster_lite_pointers.c.  Originally
 *	ported from postgrespro/jsonb_toast_internals.c lines 40..386, minus
 *	the array/chunked-array makers, then trimmed in cleanup-1 to the
 *	subset that has live runtime users.
 *
 *	Trimmed in cleanup-1 (had zero callers in jsonb_toaster_lite):
 *	  jbtl_make_toast_pointer
 *	  jbtl_write_toast_pointer
 *	  jbtl_init_toasted_container_pointer
 *	  jbtl_init_toasted_container_pointer_from_iterator
 *	  jbtl_toast_pointer_size
 *	  jbtl_toast_make_pointer_diff
 *	together with their supporting types
 *	  JbtlToastBuffer, JbtlCompressedChunk,
 *	  JbtlFetchDatumIteratorData (+ typedef JbtlFetchDatumIterator),
 *	  JbtlDetoastIteratorData    (+ typedef JbtlDetoastIterator),
 *	  JbtlToastedContainerPointerData,
 *	  JbtlPointerDiff.
 *	The postgrespro source tree retains the originals; if a future
 *	milestone needs DIFF mode or a state-machine iterator, re-port
 *	from there.
 *
 *	Mode-tag bit values (JBTL_POINTER_DIRECT_TIDS / DIRECT_TIDS_COMP /
 *	DIFF / DIFF_COMP) are preserved in this header above for forensic
 *	comparability with postgrespro even though only PLAIN_JSONB,
 *	POINTER, and POINTER_COMPRESSED_CHUNKS are emitted by the writer.
 */

extern char *
jbtl_write_custom_toast_pointer_header(char *ptr, Oid toasterid, uint32 header,
									   int datalen, int rawsize);

extern struct varlena *
jbtl_toast_make_plain_pointer(Oid toasterid, JsonbContainer *jbc, int len);

extern struct varlena *
jbtl_toast_make_pointer_with_tids(Oid toasterid,
								  struct varatt_external *toast_pointer,
								  int data_size, ItemPointer *chunk_tids);

extern struct varlena *
jbtl_toast_make_pointer_compressed_chunks(Oid toasterid,
										  struct varatt_external *toast_pointer,
										  int rawsize);

extern struct varlena *
jbtl_toast_wrap_in_jbtl_pointer(Oid toasterid,
								struct varatt_external *toast_pointer);


/* ---- DIFF overlay (L2.1b; bodies in chain.c).
 *
 *	Format mirrors postgrespro/jsonb_toaster's JsonxPointerDiff:
 *	a single byte-range overwrite onto an existing CUSTOM-toasted
 *	base value.  Stored inline in the parent tuple as the tail of
 *	a JBTL_POINTER_DIFF (or JBTL_POINTER_DIFF_COMP for a compressed
 *	base) custom-pointer:
 *
 *	  [varatt_custom header]
 *	  [varatt_external base — points at unchanged TOAST chain]
 *	  [JbtlPointerDiff: int32 offset; char data[diff_len]]
 *
 *	`offset` is the byte offset within the assembled jsonb body
 *	where the overlay applies.  `diff_len` is implicit: it equals
 *	(inline_size − offsetof(JbtlPointerDiff, data)).
 *
 *	By construction this format encodes a fixed-byte-count overwrite
 *	at one offset.  It cannot represent insert/delete/shift; the
 *	emitter must check that old and new bytes have identical
 *	encoded length, and the emitter must check that exactly one
 *	contiguous byte range differs.
 *
 *	Hard limits enforced by jbtl_update:
 *	  - top-level scalar field only (no nested paths)
 *	  - same-length byte replacement only
 *	  - single-shot: if old is already JBTL_POINTER_DIFF, decline
 *	    and let core retoast (rebase to fresh base)
 *	  - arrays out of scope
 */
typedef struct JbtlPointerDiff
{
	int32		offset;
	char		data[FLEXIBLE_ARRAY_MEMBER];
}			JbtlPointerDiff;

extern struct varlena *
jbtl_toast_make_pointer_diff(Oid toasterid,
							 struct varatt_external *base_pointer,
							 bool compressed_chunks,
							 int32 diff_offset, int32 diff_len,
							 const void *diff_data);


/* ---- M5.0a: subtree-aware parent constructor.
 *
 *	Wraps a parent body (regular varlena jsonb that may already contain
 *	JBTL_JENTRY_ISCONTAINER_PTR entries with JbtlToastedContainerPointer
 *	payloads at value positions) in a JBTL_POINTER_SUBTREE custom-varlena.
 *
 *	The parent body lives entirely inline in the custom-varlena's data
 *	area; there is no separate parent toast chain.  Children referenced
 *	by ISCONTAINER_PTR entries DO live in their own toast chains
 *	(written separately by the caller via jbtl_toast_save_datum).
 *
 *	M5.0a uses this only from a test-fixture path; production writer
 *	(initial spill in tsr_toast) lands in M5.0b.
 */
extern struct varlena *
jbtl_toast_make_pointer_subtree(Oid toasterid,
								const char *parent_body, int32 parent_body_size);

/*
 * M5.0b-3 production constructor.  Produces a JBTL_POINTER_SUBTREE
 * custom-varlena with a v1 JbtlSubtreeHeader prefix carrying the
 * synthetic parent_valueid and the heap row's reltoastrelid.  The
 * caller must have already allocated parent_valueid via
 * jbtl_alloc_subtree_parent_valueid and inserted refs edges before
 * (or transactionally with) the heap row that will hold the
 * resulting custom-varlena.
 */
extern struct varlena *
jbtl_toast_make_pointer_subtree_v1(Oid toasterid,
								   Oid parent_valueid,
								   Oid parent_toastrelid,
								   const char *parent_body,
								   int32 parent_body_size);


/* ---- chunk writer externs (L1.2a; bodies live in
 *      jsonb_toaster_lite_chain.c).  Ported from postgrespro
 *      jsonb_toast_internals.c lines 484..904.
 */

extern void
jbtl_toast_write_slice(Relation toastrel, Relation *toastidxs,
					   int num_indexes, int validIndex,
					   Oid valueid, int32 value_size,
					   int32 slice_length, char *slice_data, int options,
					   ItemPointerData *chunk_tids, bool compress_chunks);

extern Datum
jbtl_toast_save_datum_ext(Relation rel, Oid toasterid, Datum value,
						  struct varlena *oldexternal, int options,
						  struct varlena **p_chunk_tids_ptr,
						  ItemPointerData *chunk_tids,
						  bool compress_chunks);

extern Datum
jbtl_toast_save_datum(Relation rel, Datum value,
					  struct varlena *oldexternal, int options);

extern void
jbtl_toast_delete_datum(Datum value, bool is_speculative);


/* ---- chunk reader externs (L1.2b; bodies in
 *      jsonb_toaster_lite_chain.c).  Plain-chunk full read only.
 *
 *      Sliced read and per-chunk decompression land in L1.2c.
 */

extern struct varlena *
jbtl_toast_fetch_full_plain(struct varatt_external *toast_pointer,
							int32 *out_pages_touched);

extern struct varlena *
jbtl_toast_fetch_slice_plain(struct varatt_external *toast_pointer,
							 int32 sliceoffset, int32 slicelength,
							 int32 *out_chunks_total,
							 int32 *out_chunks_fetched,
							 int32 *out_pages_touched);


/* ---- compressed-chunks reader (L1.2c-2; bodies in chain.c).
 *
 *	Used when the JBTL pointer mode is JBTL_POINTER_COMPRESSED_CHUNKS.
 *	Performs whole-chunk pglz decompression and supports both full
 *	read and arbitrary slice via a single entry point.  Mixed
 *	compressed/raw chunks within one value are handled per row by
 *	VARATT_IS_COMPRESSED detection.
 */

extern struct varlena *
jbtl_toast_fetch_compressed_chunks(struct varatt_external *toast_pointer,
								   int32 sliceoffset, int32 slicelength,
								   int32 *out_chunks_total,
								   int32 *out_chunks_fetched,
								   int32 *out_chunks_decompressed,
								   int32 *out_pages_touched,
								   int32 *out_bytes_decompressed);


/*
 * jbtl_count_pages_in_chunk_range
 *	Probe-only helper.  Walks the toast relation a second time over
 *	the same (valueid, chunk_seq) range as a slice fetch, counts the
 *	number of distinct toast pages those tuples live on, and returns
 *	the count.  Used by debug/observability surfaces only — DO NOT
 *	call from production fast paths.
 *
 *	Cost: one extra btree descent + one tuple-by-tuple sysscan over
 *	the affected chunks.  No chunk_data column is decoded, only TIDs.
 */
extern int32
jbtl_count_pages_in_chunk_range(struct varatt_external *toast_pointer,
								int32 startchunk, int32 endchunk);


/*
 * jbtl_toast_count_chunks
 *	Count toast rows for a given valueid via a narrow BT scan with no
 *	chunk_data read.  Used by the slice path of
 *	jbtl_toast_fetch_compressed_chunks to populate out_chunks_total
 *	(closed-form chunks_total is unavailable when raw and compressed
 *	chunks mix in one value).
 */
extern int32
jbtl_toast_count_chunks(struct varatt_external *toast_pointer);


/* ---- L1.4: KVMap-aware top-level object field lookup
 *	(bodies in jsonb_toaster_lite_object_field.c).
 *
 *	Sliced read path that, instead of detoasting the whole jsonb,
 *	reads only the structural prefix (container header + JEntries +
 *	optional KVMap + key area) plus the byte range the looked-up
 *	value occupies.  Out of L1.4 scope: nested-container values,
 *	non-object roots, JBTL_PLAIN_JSONB inline mode, non-CUSTOM
 *	varlenas — for all of those, the function sets *out_fallback
 *	and the SQL handlers route to core's jsonb_object_field.
 */

extern JsonbValue *
jbtl_toast_fetch_object_field(struct varatt_external *toast_pointer,
							  uint32 mode,
							  const char *key, int keylen,
							  int32 *out_chunks_total,
							  int32 *out_chunks_fetched,
							  int32 *out_value_byte_offset,
							  int32 *out_value_byte_length,
							  bool *out_fallback,
							  int32 *out_pages_touched);


/* ---- L2.1a: shared locator helper used by both the read path and
 *	the dry-run update probe.  Body in jsonb_toaster_lite_object_field.c.
 *
 *	Returns true if `raw` is a JBTL custom-varlena pointing at on-disk
 *	chunks (JBTL_POINTER or JBTL_POINTER_COMPRESSED_CHUNKS); fills
 *	*out_mode and *out_ext.  Returns false for inline JBTL_PLAIN_JSONB,
 *	non-CUSTOM varlenas, or unsupported modes.
 */
extern bool
jbtl_unwrap_to_toast_pointer(struct varlena *raw,
							 uint32 *out_mode,
							 struct varatt_external *out_ext);


/* ---- M5.0b-2: subtree refs catalog -----------------------------------
 *
 *	Schema constants for jbtl_subtree_refs.  See spec section 18.I-3
 *	and M5.0b plan section 2.
 */
#define Anum_jbtl_refs_parent_toastrelid	1
#define Anum_jbtl_refs_parent_valueid		2
#define Anum_jbtl_refs_child_toastrelid		3
#define Anum_jbtl_refs_child_valueid		4
#define Natts_jbtl_subtree_refs				4

/*
 * C API exposed by jsonb_toaster_lite_subtree_refs.c.  Production
 * callers land in M5.0b-3 (tsr_toast spill) and M5.0b-4 (delete
 * dispatch + copy hook).
 */
extern void jbtl_subtree_refs_insert(Oid parent_toastrelid,
									 Oid parent_valueid,
									 Oid child_toastrelid,
									 Oid child_valueid);

extern int	jbtl_subtree_refs_delete_one(Oid parent_toastrelid,
										 Oid parent_valueid,
										 Oid child_toastrelid,
										 Oid child_valueid);

extern bool jbtl_subtree_refs_child_orphan(Oid child_toastrelid,
										   Oid child_valueid);

extern bool jbtl_subtree_refs_parent_id_in_use(Oid parent_toastrelid,
											   Oid parent_valueid);

extern Oid	jbtl_alloc_subtree_parent_valueid(Relation toastrel,
											  Relation toastidx);

#endif							/* JSONB_TOASTER_LITE_H */
