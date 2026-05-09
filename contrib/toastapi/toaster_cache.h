/*-------------------------------------------------------------------------
 *
 * toaster_cache.h
 *	  Pluggable TOAST API internal cache struct definitions
 *
 *
 * Copyright (c) 2016-2023, Postgres Professional
 *
 * IDENTIFICATION
 * contrib/toastapi/toaster_cache.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOASTERCACHE_H
#define TOASTERCACHE_H

#include "toastapi.h"

typedef struct ToasterCacheEntry
{
	Oid			toasterOid;
	TsrRoutine *routine;
} ToasterCacheEntry;

typedef struct ToastrelCacheEntry
{
	Oid 		relid;
	int16		attnum;
} ToastrelCacheEntry;

#endif