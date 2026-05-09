/*-------------------------------------------------------------------------
 *
 * toast_hook.h
 *	  Hooks for TOAST API
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/toast_hook.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOASTHOOK_H
#define TOASTHOOK_H

#include "postgres.h"
#include "varatt.h"
#include "fmgr.h"
#include "utils/guc.h"
#include "storage/lockdefs.h"
#include "access/toast_helper.h"

/* Hook for plugins to get control in Toast, Detoast and TOAST init() */
typedef Datum (*Toastapi_toast_hook_type) (Relation rel,
										   int attnum,
										   Datum value,
										   int max_length,
										   int am_options);

typedef Datum (*Toastapi_update_hook_type) (Relation rel,
											int attnum,
											Datum new_value,
											Datum old_value,
											int am_options);

typedef Datum (*Toastapi_copy_hook_type) (Relation rel,
										  int attnum,
										  Datum value,
										  int am_options);

typedef void (*Toastapi_delete_hook_type) (Relation rel,
										   int attnum,
										   Datum value,
										   bool is_speculative);

typedef Datum (*Toastapi_detoast_hook_type) (Datum value,
											 int offset,
											 int length);

typedef Datum (*Toastapi_repl_hook_type) (Oid, Datum,
											 int, int);
typedef Datum (*Toastapi_vacuum_hook_type) (Oid, Datum,
											 int, int);
typedef void *(*Toastapi_vtable_hook_type) (Datum value);

#define TOASTREL_VACUUM_FULL_DISABLED 0x01
typedef int (*Toastapi_relinfo_hook_type) (Relation main_rel,
										   Relation toast_rel);

typedef Size (*Toastapi_size_hook_type) (const void *ptr,
										 ToastPtrSizeType sz_type);

extern PGDLLIMPORT Toastapi_toast_hook_type Toastapi_toast_hook;
extern PGDLLIMPORT Toastapi_copy_hook_type Toastapi_copy_hook;
extern PGDLLIMPORT Toastapi_update_hook_type Toastapi_update_hook;
extern PGDLLIMPORT Toastapi_detoast_hook_type Toastapi_detoast_hook;
extern PGDLLIMPORT Toastapi_repl_hook_type Toastapi_repl_hook;
extern PGDLLIMPORT Toastapi_vacuum_hook_type Toastapi_vacuum_hook;
extern PGDLLIMPORT Toastapi_delete_hook_type Toastapi_delete_hook;
extern PGDLLIMPORT Toastapi_vtable_hook_type Toastapi_vtable_hook;
extern PGDLLIMPORT Toastapi_relinfo_hook_type Toastapi_relinfo_hook;
extern PGDLLIMPORT Toastapi_size_hook_type Toastapi_size_hook;

#endif							/* TOASTHOOK_H */
