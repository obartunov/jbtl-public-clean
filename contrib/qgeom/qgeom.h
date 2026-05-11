/*-------------------------------------------------------------------------
 *
 * qgeom.h
 *	  Public defs for the qgeom CUSTOM-toaster probe.
 *
 *	  qgeom is a value-level "known-type physical representation
 *	  provider" probe. The provider pattern tested here is the same
 *	  one cleaned jsonb_toaster / JBTL exemplifies: a known logical
 *	  type with an alternative physical representation, type-specific
 *	  fast paths, core-owned lifecycle, and strict fallback to
 *	  ordinary semantics.
 *
 *	  At Phase 1.a, qgeom is a plain (non-CUSTOM) varlena type only.
 *	  The CUSTOM stored form arrives in Phase 1.b through the toaster
 *	  hook.
 *
 * IDENTIFICATION
 *	  contrib/qgeom/qgeom.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QGEOM_H
#define QGEOM_H

#include "postgres.h"
#include "fmgr.h"

#define QGEOM_KIND_POINT		1
#define QGEOM_KIND_LINESTRING	2
#define QGEOM_KIND_POLYGON		3

/*
 * QgeomMeta — the inline metadata band (44 bytes).
 * Lives at the start of every qgeom value's payload area, regardless
 * of form (transient or stored).
 */
typedef struct QgeomMeta
{
	uint8		geom_kind;
	uint8		flags;
	int16		reserved;
	int32		npoints;
	int32		payload_len;	/* == npoints * 16 */
	int32		reserved2;
	float8		mbr_xmin;
	float8		mbr_ymin;
	float8		mbr_xmax;
	float8		mbr_ymax;
} QgeomMeta;

StaticAssertDecl(sizeof(QgeomMeta) == 44,
				 "QgeomMeta must be exactly 44 bytes; layout is part of the wire format");

/*
 * Form-agnostic accessors (Phase 1.a: transient form only).
 * Transient qgeom layout: [VARHDR][QgeomMeta][payload bytes].
 * Phase 1.b will add CUSTOM-form branches gated on VARATT_IS_CUSTOM(raw).
 */
#define QGEOM_TRANSIENT_META(raw) \
	((QgeomMeta *) VARDATA_ANY(raw))

#define QGEOM_TRANSIENT_PAYLOAD(raw) \
	((char *) VARDATA_ANY(raw) + sizeof(QgeomMeta))

#define QGEOM_TRANSIENT_PAYLOAD_LEN(raw) \
	((int32) (VARSIZE_ANY_EXHDR(raw) - sizeof(QgeomMeta)))

extern struct varlena *qgeom_build_transient_from_xy(int32 geom_kind,
													 ArrayType *coords);
extern ArrayType *qgeom_extract_xy_from_transient(struct varlena *raw);

#endif							/* QGEOM_H */
