/*-------------------------------------------------------------------------
 *
 * qgeom_parser.c
 *	  float8[] <-> qgeom transient varlena converters and MBR/npoints
 *	  computation. Pure functions on byte buffers and PG arrays.
 *
 * IDENTIFICATION
 *	  contrib/qgeom/qgeom_parser.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "qgeom.h"

#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"


struct varlena *
qgeom_build_transient_from_xy(int32 geom_kind, ArrayType *coords)
{
	int			n_elems;
	int			npoints;
	float8	   *coord_data;
	struct varlena *result;
	QgeomMeta  *meta;
	char	   *payload;
	int32		payload_len;
	int32		alloc_len;
	int			i;
	float8		xmin, ymin, xmax, ymax;

	if (geom_kind != QGEOM_KIND_POINT &&
		geom_kind != QGEOM_KIND_LINESTRING &&
		geom_kind != QGEOM_KIND_POLYGON)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("qgeom: unsupported geom_kind %d", geom_kind),
				 errhint("Use 1=POINT, 2=LINESTRING, 3=POLYGON.")));

	if (ARR_NDIM(coords) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("qgeom: coords must be a 1-D float8 array")));

	if (ARR_ELEMTYPE(coords) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("qgeom: coords must be float8[]")));

	n_elems = ArrayGetNItems(ARR_NDIM(coords), ARR_DIMS(coords));

	if (n_elems == 0 || (n_elems % 2) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("qgeom: coords must have an even, non-zero number of elements; got %d",
						n_elems)));

	if (ARR_NULLBITMAP(coords) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("qgeom: coords must not contain NULL elements")));

	npoints = n_elems / 2;
	payload_len = npoints * 16;

	alloc_len = VARHDRSZ + (int32) sizeof(QgeomMeta) + payload_len;
	result = (struct varlena *) palloc0(alloc_len);
	SET_VARSIZE(result, alloc_len);

	coord_data = (float8 *) ARR_DATA_PTR(coords);

	xmin = xmax = coord_data[0];
	ymin = ymax = coord_data[1];

	for (i = 1; i < npoints; i++)
	{
		float8	x = coord_data[2 * i + 0];
		float8	y = coord_data[2 * i + 1];

		if (x < xmin) xmin = x;
		if (x > xmax) xmax = x;
		if (y < ymin) ymin = y;
		if (y > ymax) ymax = y;
	}

	meta = (QgeomMeta *) VARDATA(result);
	meta->geom_kind   = (uint8) geom_kind;
	meta->flags       = 0;
	meta->reserved    = 0;
	meta->npoints     = npoints;
	meta->payload_len = payload_len;
	meta->reserved2   = 0;
	meta->mbr_xmin    = xmin;
	meta->mbr_ymin    = ymin;
	meta->mbr_xmax    = xmax;
	meta->mbr_ymax    = ymax;

	payload = (char *) VARDATA(result) + sizeof(QgeomMeta);
	memcpy(payload, coord_data, payload_len);

	return result;
}


ArrayType *
qgeom_extract_xy_from_transient(struct varlena *raw)
{
	QgeomMeta  *meta;
	char	   *payload;
	int			n_elems;
	Datum	   *elems;
	float8	   *coords;
	ArrayType  *result;
	int			i;

	meta = QGEOM_TRANSIENT_META(raw);
	payload = QGEOM_TRANSIENT_PAYLOAD(raw);

	n_elems = 2 * meta->npoints;

	elems = (Datum *) palloc(sizeof(Datum) * n_elems);
	coords = (float8 *) payload;

	for (i = 0; i < n_elems; i++)
		elems[i] = Float8GetDatum(coords[i]);

	result = construct_array(elems, n_elems, FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, 'd');

	pfree(elems);
	return result;
}
