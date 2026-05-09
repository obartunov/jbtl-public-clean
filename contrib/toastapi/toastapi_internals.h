/*-------------------------------------------------------------------------
 *
 * toastapi_internals.h
 *	  internal functions definitions used by Pluggable TOAST API
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/toastapi_internals.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOASTAPIINT_H
#define TOASTAPIINT_H

#include "postgres.h"
#include "fmgr.h"
#include "access/relation.h"
#include "storage/itemptr.h"
#include "utils/acl.h"

extern Relation
get_rel_from_relname(text *relname_text, LOCKMODE lockmode, AclMode aclmode);

typedef struct ToastAttrContext
{
	Relation	rel;
	Relation	attrel;
	int			attrel_lockmode;
	AttrNumber	attnum;
	HeapTuple	atttup;
	Datum		attoptions;
} ToastAttrContext;

Oid get_toaster_handler_oid(char *pro_name);
char *get_toaster_handler_identity(Oid procoid);

extern void check_pg_toaster_table(Relation rel);

extern void toaster_create_dependency(Oid base_class_id, Oid base_object_id,
	Oid depend_class_id, Oid depend_object_id, char dependency_type);

extern void toaster_attopts_init(ToastAttrContext *cxt, Relation rel,
								 const char *attname, bool for_update, Oid toasterid);
extern char *toaster_attopts_get(ToastAttrContext *cxt, char *optname);
extern void toaster_attopts_clear(ToastAttrContext *cxt, char *optname);
extern void toaster_attopts_set(ToastAttrContext *cxt, char *optname, char *optval, int order);
extern void toaster_attopts_update(ToastAttrContext *cxt);
extern void toaster_attopts_free(ToastAttrContext *cxt);

extern char *attopts_get_toaster_opts(Relation rel, int attnum, char *optname);

extern Oid lookup_toaster_handler_func(List *handler_name);

extern Oid get_toaster_by_name(Relation pg_toaster_rel, const char *tsrname,
							   Oid *tsrhandler, ItemPointer tid);
extern bool toaster_oid_is_used(Relation pg_toaster_rel, Oid tsroid);
extern char *get_toaster_name(Oid tsroid);
extern bool toaster_is_used(Oid tsroid);
extern void toaster_create(Relation rel, const char *tsrname, const char *tsrhandlername,
						   Oid tsroid, Oid tsrhandler);

#endif							/* TOASTAPIINT_H */
