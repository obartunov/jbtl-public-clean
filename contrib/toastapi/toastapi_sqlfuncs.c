/*-------------------------------------------------------------------------
 *
 * toastapi_sqlfuncs.c
 *	  SQL interface for Pluggable TOAST API
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/toastapi_sqlfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "varatt.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/indexing.h"
#include "catalog/dependency.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_proc.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "toastapi.h"
#include "toastapi_internals.h"
#include "toastapi_sqlfuncs.h"
#include "pg_toaster.h"

PG_FUNCTION_INFO_V1(add_toaster);

Datum
add_toaster(PG_FUNCTION_ARGS)
{
	Relation	tsrrel;
	Oid			tsroid;
	Oid			tsrhandleroid;
	char	   *tsrname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *tsrhandler = text_to_cstring(PG_GETARG_TEXT_PP(1));
	List	   *namelist;

	/* Must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create toaster \"%s\"",
						tsrname),
				 errhint("Must be superuser to create a toaster.")));

	namelist = stringToQualifiedNameList(tsrhandler, NULL);

	/*
	 * Get the handler function oid, verifying the toaster type while at it.
	 */
	tsrhandleroid = lookup_toaster_handler_func(namelist);

	tsrrel = get_rel_from_relname(cstring_to_text(PG_TOASTER_NAME), RowExclusiveLock, ACL_INSERT);
	check_pg_toaster_table(tsrrel);
	tsroid = get_toaster_by_name(tsrrel, tsrname, NULL, NULL);

	if (!OidIsValid(tsroid))
	{
		do
		{
			tsroid = GetNewObjectId();
		} while(toaster_oid_is_used(tsrrel, tsroid));
		toaster_create(tsrrel, tsrname, tsrhandler, tsroid, tsrhandleroid);
	}

	table_close(tsrrel, RowExclusiveLock);

	PG_RETURN_OID(tsroid);
}

PG_FUNCTION_INFO_V1(set_toaster);

Datum
set_toaster(PG_FUNCTION_ARGS)
{
	ToastAttrContext cxt;
	Relation	rel;
	Relation	tsrrel;
	char	   *tsrname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	text	   *relname = PG_GETARG_TEXT_PP(1);
	char	   *attname = text_to_cstring(PG_GETARG_TEXT_PP(2));
	Oid			tsroid;
	Oid			tsrhandler;
	char		str[12];
	char		nstr[12];
	int			len = 0;
	int			attnum pg_attribute_unused();

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to assign toaster \"%s\"",
						tsrname),
				 errhint("Must be superuser to assign a toaster.")));

	/* Get relation oid by name */
	rel = get_rel_from_relname(relname, ExclusiveLock, ACL_SELECT);

	/* Get toaster id by name */
	tsrrel = get_rel_from_relname(cstring_to_text(PG_TOASTER_NAME), AccessShareLock, ACL_SELECT);
	check_pg_toaster_table(tsrrel);
	tsroid = get_toaster_by_name(tsrrel, tsrname, &tsrhandler, NULL);
	table_close(tsrrel, AccessShareLock);

	if (!OidIsValid(tsroid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("cannot find toaster with name \"%s\"", tsrname)));

	if (!OidIsValid(tsrhandler))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("toaster \"%s\" does not have a handler", tsrname),
				 errhint("Was extension containing toaster \"%s\" dropped?", tsrname)));

	/* Find attribute and check whether toaster is applicable to it */
	toaster_attopts_init(&cxt, rel, attname, true, tsroid);

	/* Check toaster handler and routine */
	(void) SearchTsrHandlerCache(tsrhandler);

	/* Set toaster variables - oid, toast relation id, handler for fast access */
	len = pg_ltoa(tsrhandler, str);
	if (len <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid handler OID \"%u\"",
						tsrhandler)));

	len = pg_ltoa(tsroid, nstr);
	if (len <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid toaster OID \"%u\"",
						tsroid)));

	toaster_attopts_set(&cxt, ATT_HANDLER_NAME, str, -1);
	toaster_attopts_set(&cxt, ATT_TOASTER_NAME, nstr, -1);
	toaster_attopts_update(&cxt);
	toaster_attopts_free(&cxt);

	table_close(rel, ExclusiveLock);

	PG_RETURN_OID(tsroid);
}

PG_FUNCTION_INFO_V1(reset_toaster);

Datum
reset_toaster(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	char	   *attname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ToastAttrContext cxt;
	Relation	rel;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to reset toaster for table \"%s\"",
						text_to_cstring(relname)),
				 errhint("Must be superuser to reset a toaster.")));

	rel = get_rel_from_relname(relname, ExclusiveLock, ACL_UPDATE);

	toaster_attopts_init(&cxt, rel, attname, true, InvalidOid);
	toaster_attopts_clear(&cxt, ATT_TOASTER_NAME);
	toaster_attopts_clear(&cxt, ATT_HANDLER_NAME);
	toaster_attopts_update(&cxt);
	toaster_attopts_free(&cxt);

	table_close(rel, ExclusiveLock);

	PG_RETURN_OID(InvalidOid);
}

PG_FUNCTION_INFO_V1(get_toaster);

Datum get_toaster(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	char	   *attname = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Relation	rel;
	ToastAttrContext cxt;
	Oid			tsroid = InvalidOid;
	char	   *tsrname;
	char 		*tsroid_str;

	rel = get_rel_from_relname(relname, AccessShareLock, ACL_SELECT);

	toaster_attopts_init(&cxt, rel, attname, false, InvalidOid);
	tsroid_str = toaster_attopts_get(&cxt, ATT_TOASTER_NAME);
	if(tsroid_str)
		tsroid = atoi(tsroid_str);
	toaster_attopts_free(&cxt);

	table_close(rel, AccessShareLock);

	if(!OidIsValid(tsroid))
		PG_RETURN_NULL();

	tsrname = get_toaster_name(tsroid);

	elog(NOTICE,"%s", tsrname);
	PG_RETURN_OID(tsroid);
}

PG_FUNCTION_INFO_V1(drop_toaster);

Datum
drop_toaster(PG_FUNCTION_ARGS)
{
	char	   *tsrname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Relation	tsrrel;
	ItemPointerData tid;
	Oid			tsroid;

	/* Must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to drop toaster \"%s\"",
						tsrname),
				 errhint("Must be superuser to drop a toaster.")));

	tsrrel = get_rel_from_relname(cstring_to_text(PG_TOASTER_NAME), RowExclusiveLock, ACL_DELETE);
	check_pg_toaster_table(tsrrel);
	tsroid = get_toaster_by_name(tsrrel, tsrname, NULL, &tid);

	if (!OidIsValid(tsroid))
	{
		table_close(tsrrel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot find toaster with name \"%s\"",
						tsrname),
				 errhint("Probably toaster does not exist.")));
	}

	if (toaster_is_used(tsroid))
	{
		table_close(tsrrel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("toaster \"%s\" is used and cannot be dropped",
						tsrname),
				 errhint("Reset toaster for all columns it is used for.")));
	}


	CatalogTupleDelete(tsrrel, &tid);

	table_close(tsrrel, RowExclusiveLock);

	PG_RETURN_OID(tsroid);
}

PG_FUNCTION_INFO_V1(get_toaster_id);

Datum get_toaster_id(PG_FUNCTION_ARGS)
{
	char	   *tsrname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Relation	tsrrel;
	Oid			tsroid;

	tsrrel = get_rel_from_relname(cstring_to_text(PG_TOASTER_NAME), AccessShareLock, ACL_SELECT);
	check_pg_toaster_table(tsrrel);
	tsroid = get_toaster_by_name(tsrrel, tsrname, NULL, NULL);

	table_close(tsrrel, AccessShareLock);

	PG_RETURN_OID(tsroid);
}
