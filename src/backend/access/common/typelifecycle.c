/*-------------------------------------------------------------------------
 *
 * typelifecycle.c
 *	  Internal in-core registry: type OID -> TypeLifecycleRoutine.
 *
 * See typelifecycle.h for the design and ownership rules.  This file holds
 * only the registry mechanism (register + lookup by typid).  It knows nothing
 * about any concrete type: jsonb, qgeom, bytea, etc. register their own
 * routines from their own modules.
 *
 * The registry is a small process-local static table.  The expected number of
 * registered types is tiny (jsonb today; qgeom/bytea/extension types later),
 * so a linear array is appropriate and avoids dynamic-memory / hash overhead
 * on the lookup path.  Lookup is O(n) over a handful of entries and is only
 * called from write/lifecycle/rewrite sites that already deformed the tuple.
 *
 * src/backend/access/common/typelifecycle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/typelifecycle.h"

#define MAX_TYPE_LIFECYCLE_ROUTINES 8

typedef struct TypeLifecycleEntry
{
	Oid			typid;
	const TypeLifecycleRoutine *routine;
}			TypeLifecycleEntry;

static TypeLifecycleEntry type_lifecycle_table[MAX_TYPE_LIFECYCLE_ROUTINES];
static int	type_lifecycle_count = 0;

/*
 * RegisterTypeLifecycleRoutine
 *		Bind a lifecycle routine to a type OID.
 *
 * Called from in-core init (for in-core types) or _PG_init (for extension
 * types), before any DML can reach a lifecycle path for that type.  Re-
 * registering the same typid replaces the routine (lets an extension override
 * during development); duplicate typids are not stored twice.
 */
void
RegisterTypeLifecycleRoutine(Oid typid, const TypeLifecycleRoutine *routine)
{
	Assert(OidIsValid(typid));
	Assert(routine != NULL);

	/* replace if already present */
	for (int i = 0; i < type_lifecycle_count; i++)
	{
		if (type_lifecycle_table[i].typid == typid)
		{
			type_lifecycle_table[i].routine = routine;
			return;
		}
	}

	if (type_lifecycle_count >= MAX_TYPE_LIFECYCLE_ROUTINES)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many type lifecycle routines registered (max %d)",
						MAX_TYPE_LIFECYCLE_ROUTINES)));

	type_lifecycle_table[type_lifecycle_count].typid = typid;
	type_lifecycle_table[type_lifecycle_count].routine = routine;
	type_lifecycle_count++;
}

/*
 * lookup_type_lifecycle_routine
 *		Return the routine registered for typid, or NULL if none.
 *
 * This is the ONLY thing generic heap/rewrite code learns about types: it asks
 * by atttypid and either gets a routine (call the needed callback) or NULL
 * (ordinary path).  It never learns which concrete type it is.
 */
const TypeLifecycleRoutine *
lookup_type_lifecycle_routine(Oid typid)
{
	for (int i = 0; i < type_lifecycle_count; i++)
	{
		if (type_lifecycle_table[i].typid == typid)
			return type_lifecycle_table[i].routine;
	}
	return NULL;
}
