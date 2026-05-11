/*-------------------------------------------------------------------------
 *
 * qgeom.c
 *	  Type IO and constructor / reader for the qgeom CUSTOM-toaster
 *	  probe (Phase 1.a: transient form only).
 *
 *	  At Phase 1.a, qgeom values are plain (non-CUSTOM) varlenas. The
 *	  type registers with CREATE EXTENSION but does not yet install any
 *	  toastapi hooks. Hooks arrive in Phase 1.b.
 *
 * IDENTIFICATION
 *	  contrib/qgeom/qgeom.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "qgeom.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "varatt.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

void
_PG_init(void)
{
	/* nothing yet at Phase 1.a */
}


PG_FUNCTION_INFO_V1(qgeom_in);
Datum
qgeom_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	char	   *cur = str;
	char	   *colon;
	int32		geom_kind;
	int			cap;
	int			n;
	Datum	   *elems;
	ArrayType  *coords;
	struct varlena *result;

	colon = strchr(cur, ':');
	if (colon == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("qgeom_in: missing 'kind:' prefix in \"%s\"", str)));

	*colon = '\0';
	geom_kind = pg_strtoint32(cur);
	*colon = ':';
	cur = colon + 1;

	cap = 16;
	n = 0;
	elems = (Datum *) palloc(sizeof(Datum) * cap);

	while (*cur != '\0')
	{
		char	   *sep;
		char	   *end;
		double		x, y;

		sep = strchr(cur, ',');
		if (sep == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("qgeom_in: missing comma in vertex near \"%s\"", cur)));
		*sep = '\0';
		x = float8in_internal(cur, NULL, "double precision", cur, NULL);
		*sep = ',';
		cur = sep + 1;

		end = strchr(cur, ';');
		if (end != NULL)
			*end = '\0';
		y = float8in_internal(cur, NULL, "double precision", cur, NULL);
		if (end != NULL)
		{
			*end = ';';
			cur = end + 1;
		}
		else
			cur += strlen(cur);

		if (n + 2 > cap)
		{
			cap *= 2;
			elems = (Datum *) repalloc(elems, sizeof(Datum) * cap);
		}
		elems[n++] = Float8GetDatum(x);
		elems[n++] = Float8GetDatum(y);
	}

	if (n == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("qgeom_in: zero vertices in \"%s\"", str)));

	coords = construct_array(elems, n, FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, 'd');
	pfree(elems);

	result = qgeom_build_transient_from_xy(geom_kind, coords);
	pfree(coords);

	PG_RETURN_POINTER(result);
}


PG_FUNCTION_INFO_V1(qgeom_out);
Datum
qgeom_out(PG_FUNCTION_ARGS)
{
	struct varlena *raw = PG_GETARG_VARLENA_PP(0);
	QgeomMeta  *meta;
	float8	   *coords;
	StringInfoData buf;
	int			i;

	meta = QGEOM_TRANSIENT_META(raw);
	coords = (float8 *) QGEOM_TRANSIENT_PAYLOAD(raw);

	initStringInfo(&buf);
	appendStringInfo(&buf, "%d:", meta->geom_kind);

	for (i = 0; i < meta->npoints; i++)
	{
		char	   *xs = float8out_internal(coords[2 * i + 0]);
		char	   *ys = float8out_internal(coords[2 * i + 1]);

		if (i > 0)
			appendStringInfoChar(&buf, ';');
		appendStringInfo(&buf, "%s,%s", xs, ys);
		pfree(xs);
		pfree(ys);
	}

	PG_RETURN_CSTRING(buf.data);
}


PG_FUNCTION_INFO_V1(qgeom_send);
Datum
qgeom_send(PG_FUNCTION_ARGS)
{
	struct varlena *raw = PG_GETARG_VARLENA_PP(0);
	QgeomMeta  *meta = QGEOM_TRANSIENT_META(raw);
	char	   *payload = QGEOM_TRANSIENT_PAYLOAD(raw);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, 1);				/* version */
	pq_sendbyte(&buf, meta->geom_kind);
	pq_sendint32(&buf, meta->npoints);
	pq_sendbytes(&buf, payload, meta->payload_len);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


PG_FUNCTION_INFO_V1(qgeom_recv);
Datum
qgeom_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	uint8		version;
	uint8		geom_kind;
	int32		npoints;
	int32		expected_payload_len;
	int			i;
	Datum	   *elems;
	ArrayType  *coords;
	struct varlena *result;
	const char *payload;

	version = pq_getmsgbyte(buf);
	if (version != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("qgeom_recv: unsupported binary version %u", version)));

	geom_kind = pq_getmsgbyte(buf);
	npoints = pq_getmsgint(buf, 4);

	if (npoints <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("qgeom_recv: npoints must be > 0; got %d", npoints)));

	expected_payload_len = npoints * 16;
	payload = pq_getmsgbytes(buf, expected_payload_len);

	elems = (Datum *) palloc(sizeof(Datum) * (size_t) npoints * 2);
	for (i = 0; i < npoints; i++)
	{
		float8	x, y;
		memcpy(&x, payload + 16 * i + 0, sizeof(float8));
		memcpy(&y, payload + 16 * i + 8, sizeof(float8));
		elems[2 * i + 0] = Float8GetDatum(x);
		elems[2 * i + 1] = Float8GetDatum(y);
	}

	coords = construct_array(elems, npoints * 2, FLOAT8OID,
							 sizeof(float8), FLOAT8PASSBYVAL, 'd');
	pfree(elems);

	result = qgeom_build_transient_from_xy((int32) geom_kind, coords);
	pfree(coords);

	PG_RETURN_POINTER(result);
}


PG_FUNCTION_INFO_V1(qgeom_from_xy);
Datum
qgeom_from_xy(PG_FUNCTION_ARGS)
{
	int32		geom_kind = PG_GETARG_INT32(0);
	ArrayType  *coords = PG_GETARG_ARRAYTYPE_P(1);
	struct varlena *result;

	result = qgeom_build_transient_from_xy(geom_kind, coords);
	PG_RETURN_POINTER(result);
}


PG_FUNCTION_INFO_V1(qgeom_to_xy);
Datum
qgeom_to_xy(PG_FUNCTION_ARGS)
{
	struct varlena *raw = PG_GETARG_VARLENA_PP(0);
	ArrayType  *coords = qgeom_extract_xy_from_transient(raw);
	PG_RETURN_ARRAYTYPE_P(coords);
}
