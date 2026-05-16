/*-------------------------------------------------------------------------
 *
 * toast_hook.h
 *	  Type-specific operator fast-path hooks that interact with CUSTOM
 *	  TOAST varlenas.
 *
 *	After the TsrRoutine resolver introduction, the six lifecycle hooks
 *	(toast/update/copy/delete/detoast/size) that previously lived here
 *	are dispatched through TsrRoutine + access/toasterapi.h.  This header
 *	now retains only the jsonb operator fast-path hook, which is NOT a
 *	TOAST lifecycle method but a type-specific shortcut.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/toast_hook.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOASTHOOK_H
#define TOASTHOOK_H

#include "fmgr.h"

/*
 * Hook for plugins that supply a fast path for `jsonb -> text` key
 * access on CUSTOM-TOASTED jsonb values.  Called from
 * jsonb_object_field() in core, but ONLY when the raw input datum
 * is a CUSTOM external varlena (cheap inline VARATT_IS_CUSTOM check,
 * no detoast).  Default (non-CUSTOM) jsonb never reaches the hook.
 *
 * EXCLUSIVE single-installer hook in this prototype: there is no
 * chaining.  An extension that installs a callback owns the hook;
 * a later loader overwrites silently.  Chaining (predecessor save
 * + invoke on not-handled) is a separate upstream design.
 *
 * Contract:
 *   raw_jb  - raw, possibly toasted jsonb Datum; the callback decides
 *             whether to detoast.
 *   key     - the lookup key (text *).
 *   isnull  - OUT, valid iff the function returns true.
 *   result  - OUT, valid iff the function returns true and *isnull
 *             is false.
 *
 * Return:
 *   true   - hook handled this lookup; core returns the result/isnull.
 *   false  - not handled; core continues with its normal
 *            jsonb_object_field body.  The hook callback MUST NOT
 *            call back into jsonb_object_field, DirectFunctionCall*,
 *            OidFunctionCall*, or any other core fallback wrapper:
 *            core handles the fallback itself once the callback
 *            returns false.
 */
typedef bool (*Toastapi_jsonb_object_field_hook_type) (Datum raw_jb,
													   text *key,
													   bool *isnull,
													   Datum *result);

extern PGDLLIMPORT Toastapi_jsonb_object_field_hook_type
Toastapi_jsonb_object_field_hook;

#endif							/* TOASTHOOK_H */
