/*-------------------------------------------------------------------------
 *
 * toastapi_internals.c
 *	  internal functions used by Pluggable TOAST API
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/toastapi_internals.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "varatt.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_depend.h"
#include "catalog/dependency.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"
#include "utils/fmgroids.h"
#include "utils/regproc.h"

#include "parser/parse_type.h"
#include "parser/scansup.h"

#include "funcapi.h"
#include "toastapi.h"
#include "toastapi_internals.h"
#include "pg_toaster.h"

Relation
get_rel_from_relname(text *relname_text, LOCKMODE lockmode, AclMode aclmode)
{
	RangeVar   *relvar;
	Relation	rel;
	AclResult	aclresult;

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text));
	rel = table_openrv(relvar, lockmode);

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  aclmode);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	return rel;
}

/*
 * Convert a handler function name to an Oid.  If the return type of the
 * function doesn't match the given toaster type, an error is raised.
 *
 * This function either return valid function Oid or throw an error.
 */
Oid
lookup_toaster_handler_func(List *handler_name)
{
	Oid			handlerOid = InvalidOid;
	Oid			funcargtypes[1] = {INTERNALOID};

	if (handler_name == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("handler function is not specified")));

	/* handlers have one argument of type internal */
	handlerOid = LookupFuncName(handler_name, 1, funcargtypes, false);

	if (get_func_rettype(handlerOid) != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						get_func_name(handlerOid),
						format_type_extended(INTERNALOID, -1, 0))));

	return handlerOid;
}

/*
 * Transform Toaster handler description from Object identity form
 * into function OID
 *
 * This function always returns OID - valid or not
 */
Oid get_toaster_handler_oid(char *pro_name)
{
	List	   *names = NIL;
	int			nargs = 0;
	Oid			argtypes[FUNC_MAX_ARGS];
	FuncCandidateList clist = NULL;
	int			fgc_flags = 0;

	if(!pro_name)
		PG_RETURN_OID(InvalidOid);

	parseNameAndArgTypes(pro_name, false, &names, &nargs, argtypes, NULL);

	if(names)
		clist = FuncnameGetCandidates(names, nargs, NIL, false, false, false, true, &fgc_flags);

	if(clist)
	{
		for (; clist; clist = clist->next)
		{
			if (memcmp(clist->args, argtypes, nargs * sizeof(Oid)) == 0)
				PG_RETURN_OID(clist->oid);
		}
	}
	PG_RETURN_OID(InvalidOid);
}

/*
 * Transform Toaster handler OID into identity - full machine-readable
 * declaration stored in PG_TOASTER table to be independent of OID
 * changes during pg_upgrade
 *
 * Returns C-string
 */
char *get_toaster_handler_identity(Oid procoid)
{
	char	   *objidentity = NULL;
	ObjectAddress address;

	address.classId = ProcedureRelationId;
	address.objectId = procoid;
	address.objectSubId = InvalidOid;

	if (is_objectclass_supported(address.classId))
	{
		HeapTuple	objtup;
		Relation	catalog = table_open(address.classId, AccessShareLock);

		objtup = get_catalog_object_by_oid(catalog,
										   get_object_attnum_oid(address.classId),
										   address.objectId);
		if (objtup != NULL)
		{
			bool		isnull;
			AttrNumber	nspAttnum;
			AttrNumber	nameAttnum;

			nspAttnum = get_object_attnum_namespace(address.classId);
			if (nspAttnum != InvalidAttrNumber)
			{
				heap_getattr(objtup, nspAttnum,
										  RelationGetDescr(catalog), &isnull);
				if (isnull)
					elog(ERROR, "invalid null namespace in object %u/%u/%d",
						 address.classId, address.objectId, address.objectSubId);
			}

			if (get_object_namensp_unique(address.classId))
			{
				nameAttnum = get_object_attnum_name(address.classId);
				if (nameAttnum != InvalidAttrNumber)
				{
					Datum		nameDatum;

					nameDatum = heap_getattr(objtup, nameAttnum,
											 RelationGetDescr(catalog), &isnull);
					if (isnull)
						elog(ERROR, "invalid null name in object %u/%u/%d",
							 address.classId, address.objectId, address.objectSubId);
					quote_identifier(NameStr(*(DatumGetName(nameDatum))));
				}
			}
		}

		table_close(catalog, AccessShareLock);
	}

	objidentity = getObjectIdentity(&address, true);
	return objidentity;
}

/*
 * Basically hides call to recordDependencyOn for not to mess with
 * ObjectAddress objects.
 * Order is base object class and id first, dependent object second
 */
void toaster_create_dependency(Oid base_class_id, Oid base_object_id,
	Oid depend_class_id, Oid depend_object_id, char dependency_type)
{
	ObjectAddress baseobject, toasterobject;

	baseobject.classId = base_class_id;
	baseobject.objectId = base_object_id;
	baseobject.objectSubId = 0;
	toasterobject.objectSubId = 0;
	toasterobject.classId = depend_class_id;
	toasterobject.objectId = depend_object_id;

	recordDependencyOn(&toasterobject, &baseobject, dependency_type);
}

static void
toaster_attopts_init_ext(ToastAttrContext *cxt, Relation rel,
						 const char *attname, int attnum, bool for_update, Oid toasterid)
{
	Form_pg_attribute att;
	HeapTuple	tuple;
	bool		isnull;
	Oid			relid = RelationGetRelid(rel);

	cxt->attrel_lockmode = for_update ? RowShareLock : AccessShareLock;
	cxt->attrel = table_open(AttributeRelationId, cxt->attrel_lockmode);

	tuple = attname ? SearchSysCacheAttName(relid, attname) : SearchSysCacheAttNum(relid, attnum);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						attname, RelationGetRelationName(rel))));

	cxt->atttup = tuple;
	att = (Form_pg_attribute) GETSTRUCT(tuple);

	if (att->attnum <= 0 && for_update)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						attname)));

	/* validate toaster, if needed */
	if (OidIsValid(toasterid))
		validateToaster(toasterid, att->atttypid, att->attstorage,
						att->attcompression, rel->rd_rel->relam, false);

	cxt->attnum = att->attnum;

	cxt->attoptions =
		SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions,
						&isnull);

	if (isnull)
		cxt->attoptions = (Datum) 0;
}

void
toaster_attopts_init(ToastAttrContext *cxt, Relation rel,
					 const char *attname, bool for_update, Oid toasterid)
{
	toaster_attopts_init_ext(cxt, rel, attname, -1, for_update, toasterid);
}

void
toaster_attopts_free(ToastAttrContext *cxt)
{
	ReleaseSysCache(cxt->atttup);
	table_close(cxt->attrel, cxt->attrel_lockmode);
}

char *
toaster_attopts_get(ToastAttrContext *cxt, char *optname)
{
	List	   *o_list = untransformRelOptions(cxt->attoptions);
	ListCell   *cell;

	foreach(cell, o_list)
	{
		DefElem    *def = lfirst(cell);

		if (!strcmp(def->defname, optname))
			return pstrdup(defGetString(def));
	}

	return NULL;
}

void
toaster_attopts_set(ToastAttrContext *cxt, char *optname, char *optval, int order)
{
	List	   *o_list = untransformRelOptions(cxt->attoptions);
	ListCell   *cell;
	int			l_idx = 0;

	foreach(cell, o_list)
	{
		DefElem    *def = lfirst(cell);

		if (!strcmp(def->defname, optname))
		{
			o_list = list_delete_nth_cell(o_list, l_idx);
			break;
		}

		l_idx++;
	}

	if (order < 0)
		o_list = lcons(makeDefElem(optname, (Node *) makeString(optval), -1), o_list);
	else if (order == 0 && l_idx > 0)
		o_list = list_insert_nth(o_list, 1, makeDefElem(optname, (Node *) makeString(optval), -1));
	else
		o_list = lappend(o_list, makeDefElem(optname, (Node *) makeString(optval), -1));

	cxt->attoptions = transformRelOptions((Datum) 0, o_list, NULL, NULL, false, false);
}

void
toaster_attopts_clear(ToastAttrContext *cxt, char *optname)
{
	List	   *o_list = list_make1(makeDefElem(optname, NULL, -1));

	cxt->attoptions = transformRelOptions(cxt->attoptions, o_list, NULL, NULL, false, true);
}

void
toaster_attopts_update(ToastAttrContext *cxt)
{
	HeapTuple	newtuple;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];
	Datum		opts = cxt->attoptions;

	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (opts != (Datum) 0)
		repl_val[Anum_pg_attribute_attoptions - 1] = opts;
	else
		repl_null[Anum_pg_attribute_attoptions - 1] = true;
	repl_repl[Anum_pg_attribute_attoptions - 1] = true;

	newtuple = heap_modify_tuple(cxt->atttup, RelationGetDescr(cxt->attrel),
								 repl_val, repl_null, repl_repl);
	CatalogTupleUpdate(cxt->attrel, &newtuple->t_self, newtuple);

	heap_freetuple(newtuple);

	CommandCounterIncrement();
}

char *
attopts_get_toaster_opts(Relation rel, int attnum, char *optname)
{
	ToastAttrContext cxt;
	char	   *res;

	toaster_attopts_init_ext(&cxt, rel, NULL, attnum, false, InvalidOid);
	res = toaster_attopts_get(&cxt, optname);
	toaster_attopts_free(&cxt);

	return res;
}

Oid
get_toaster_by_name(Relation pg_toaster_rel, const char *tsrname,
					Oid *tsrhandler, ItemPointer tid)
{
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			tsroid = InvalidOid;
	const char *bare_name;
	const char *schema_name = NULL;
	char	   *schema_buf = NULL;
	const char *dot;

	/*
	 * Accept either "tsrname" or "schema.tsrname".  pg_toaster.tsrname
	 * stores unqualified names (NameData), but users commonly pass
	 * a schema-qualified form when the extension that registered the
	 * toaster lives outside the search path or for clarity.
	 *
	 * If a schema prefix is supplied, we use it to disambiguate
	 * (validating against the handler proc's namespace below).  If
	 * no prefix, we fall back to unique-name matching across the
	 * pg_toaster catalog (which is the historical behaviour).
	 *
	 * Note: this is a string split on the FIRST '.', not full SQL
	 * identifier parsing — quoted names with embedded dots are not
	 * handled.  Toaster names in practice are simple identifiers
	 * (e.g. "jsonb_toaster_lite"), so this is sufficient.
	 */
	dot = strchr(tsrname, '.');
	if (dot != NULL)
	{
		Size		schema_len = (Size) (dot - tsrname);

		schema_buf = palloc(schema_len + 1);
		memcpy(schema_buf, tsrname, schema_len);
		schema_buf[schema_len] = '\0';
		schema_name = schema_buf;
		bare_name = dot + 1;
	}
	else
	{
		bare_name = tsrname;
	}

	scan = systable_beginscan(pg_toaster_rel, InvalidOid, false, NULL, 0, NULL);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_toaster tsr = (Form_pg_toaster) GETSTRUCT(tup);

		if (namestrcmp(&tsr->tsrname, bare_name) != 0)
			continue;

		/*
		 * If caller passed a schema prefix, verify that the toaster's
		 * handler procedure lives in that schema.  This protects
		 * against the case where two extensions registered toasters
		 * with the same bare name in different schemas (rare but
		 * possible) — caller can disambiguate via the prefix.
		 */
		if (schema_name != NULL)
		{
			Oid			handler_oid =
				get_toaster_handler_oid(text_to_cstring(&tsr->tsrhandler));
			Oid			handler_namespace = InvalidOid;
			HeapTuple	proc_tup;

			if (!OidIsValid(handler_oid))
				continue;

			proc_tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler_oid));
			if (HeapTupleIsValid(proc_tup))
			{
				handler_namespace =
					((Form_pg_proc) GETSTRUCT(proc_tup))->pronamespace;
				ReleaseSysCache(proc_tup);
			}

			if (!OidIsValid(handler_namespace))
				continue;

			if (strcmp(get_namespace_name(handler_namespace), schema_name) != 0)
				continue;
		}

		tsroid = tsr->oid;

		if (tsrhandler)
			*tsrhandler = get_toaster_handler_oid(text_to_cstring(&tsr->tsrhandler));

		if (tid)
			ItemPointerCopy(&tup->t_self, tid);

		break;
	}

	systable_endscan(scan);

	if (schema_buf)
		pfree(schema_buf);

	return tsroid;
}

bool
toaster_oid_is_used(Relation pg_toaster_rel, Oid tsroid)
{
	SysScanDesc	scan;
	HeapTuple	tup;
	bool			tsroid_is_used = false;

	scan = systable_beginscan(pg_toaster_rel, InvalidOid, false, NULL, 0, NULL);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_toaster tsr = (Form_pg_toaster) GETSTRUCT(tup);
		if(tsr->oid == tsroid)
		{
			tsroid_is_used = true;
			break;
		}
	}

	systable_endscan(scan);

	return tsroid_is_used;
}

void check_pg_toaster_table(Relation rel)
{
	TupleDesc tdesc = RelationGetDescr(rel);
	if(tdesc->natts < Natts_pg_toaster)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("relation \"%s\" has invalid number of columns",
						RelationGetRelationName(rel)),
				 errhint("Please perform ALTER EXTENSION toastapi UPDATE")));

	for(int i = 1; i <= Natts_pg_toaster; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tdesc, i - 1);
		if((i == Anum_pg_toaster_oid && att->atttypid != OIDOID)
			|| (i == Anum_pg_toaster_tsrname && att->atttypid != NAMEOID)
			|| (i == Anum_pg_toaster_tsrhandler && att->atttypid != TEXTOID))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
					 errmsg("column \"%s\" of relation \"%s\" has invalid type",
							NameStr(att->attname), RelationGetRelationName(rel))),
					 errhint("Please perform ALTER EXTENSION toastapi UPDATE"));
	}
}

char *
get_toaster_name(Oid tsroid)
{
	SysScanDesc scan;
	HeapTuple	tup;
	char	   *tsrname = NULL;
	Relation	pg_toaster_rel =
		get_rel_from_relname(cstring_to_text(PG_TOASTER_NAME), AccessShareLock, ACL_SELECT);

	check_pg_toaster_table(pg_toaster_rel);
	scan = systable_beginscan(pg_toaster_rel, InvalidOid, false, NULL, 0, NULL);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_toaster tsr = (Form_pg_toaster) GETSTRUCT(tup);

		if (tsr->oid == tsroid)
		{
			tsrname = pstrdup(NameStr(tsr->tsrname));
			break;
		}
	}

	systable_endscan(scan);
	table_close(pg_toaster_rel, AccessShareLock);

	return tsrname;
}

bool
toaster_is_used(Oid tsroid)
{
	Relation	attrel;
	SysScanDesc scan;
	HeapTuple	tup;
	char		tsroid_str[12];	/* sign, 10 digits and '\0' */
	bool		found = false;

	(void) pg_ltoa(tsroid, tsroid_str);

	attrel = table_open(AttributeRelationId, RowExclusiveLock);
	scan = systable_beginscan(attrel, InvalidOid, false,
							  NULL, 0, NULL);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		bool		isnull;
		ListCell   *cell;
		List	   *o_list;
		Datum		o_datum;

		o_datum = SysCacheGetAttr(ATTNAME, tup, Anum_pg_attribute_attoptions,
								  &isnull);
		o_list = untransformRelOptions(o_datum);

		foreach(cell, o_list)
		{
			DefElem    *def = (DefElem *) lfirst(cell);
			char	   *str = defGetString(def);

			if (!strcmp(def->defname, ATT_TOASTER_NAME) &&
				str &&
				!strcmp(tsroid_str, str))
			{
				found = true;
				break;
			}
		}
	}

	systable_endscan(scan);
	table_close(attrel, RowExclusiveLock);

	return found;
}

/* Insert tuple into pg_toaster */
void
toaster_create(Relation rel, const char *tsrname, const char *tsrhandlername, Oid tsroid, Oid tsrhandler)
{
	Datum		values[Natts_pg_toaster];
	bool		nulls[Natts_pg_toaster];
	NameData	tsrnmdata;
	HeapTuple	tup;
	char *pro_desc = NULL;

	pro_desc = get_toaster_handler_identity(tsrhandler);

	if(!pro_desc)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("handler function %d not found", tsrhandler)));

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	namestrcpy(&tsrnmdata, tsrname);

	values[Anum_pg_toaster_oid - 1] = ObjectIdGetDatum(tsroid);
	values[Anum_pg_toaster_tsrname - 1] = NameGetDatum(&tsrnmdata);
	values[Anum_pg_toaster_tsrhandler - 1] = CStringGetTextDatum(pro_desc);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
}
