/*-------------------------------------------------------------------------
 *
 * toaster_attr_cache.h
 *	  Public interface of the provider-side (relid, attnum) -> TsrRoutine
 *	  cache.
 *
 *	See toaster_attr_cache.c for the contract.
 *
 *
 * Portions Copyright (c) 2016-2026, Postgres Professional
 *
 * contrib/toastapi/toaster_attr_cache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOASTER_ATTR_CACHE_H
#define TOASTER_ATTR_CACHE_H

#include "access/toasterapi.h"
#include "utils/rel.h"

extern void ToasterAttrCacheInit(void);
extern TsrRoutine * ToasterAttrCacheLookup(Relation rel, AttrNumber attnum,
										   Oid *out_toasterid);

#endif							/* TOASTER_ATTR_CACHE_H */
