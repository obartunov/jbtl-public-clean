/*-------------------------------------------------------------------------
 *
 * typelifecycle_builtins.c
 *	  Aggregate registration of in-core type lifecycle routines.
 *
 * The generic registry (typelifecycle.c) must not know any concrete type.
 * This file is the one place that DOES know the list of in-core types that
 * provide a TypeLifecycleRoutine, and registers them all through a single
 * entry point.  The init path (postinit.c) calls this one function instead of
 * naming individual types, so adding a future in-core type (e.g. an in-core
 * geometry) is a one-line change here, not a change in postinit.c.
 *
 * Extension types are NOT registered here.  An extension registers its own
 * type via RegisterTypeLifecycleRoutine() from its _PG_init.  See the contract
 * note below.
 *
 * --- Extension lifecycle provider contract -------------------------------
 * A lifecycle routine must be present in every backend that can touch tables
 * containing split values of that type, BEFORE any such DML/rewrite runs.
 * In-core types satisfy this by registering here, on the InitPostgres path.
 *
 * An extension type cannot register on that path, so the practical initial
 * model is: load the extension via shared_preload_libraries (or another
 * mechanism that guarantees _PG_init runs in every relevant backend before
 * DML).  If the provider is absent in a backend, lookup_type_lifecycle_routine
 * returns NULL and the generic path is taken; for a type that relies on
 * lifecycle relocation this would risk orphaning cold payload during rewrite,
 * so the preload guarantee is required, not optional.
 *
 * We deliberately do NOT add: catalog type->library binding, lazy library
 * loading, CREATE TOASTER, set_toaster, va_toasterid, or a read-side resolver.
 * -------------------------------------------------------------------------
 *
 * src/backend/access/common/typelifecycle_builtins.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/typelifecycle.h"
#include "utils/jsonb.h"

/*
 * RegisterAllInCoreTypeLifecycleRoutines
 *		Register every in-core type that provides a TypeLifecycleRoutine.
 *
 * Called once per backend from the init path, after the relcache is ready and
 * before any DML can reach a lifecycle path.  Add future in-core types here.
 */
void
RegisterAllInCoreTypeLifecycleRoutines(void)
{
	jsonb_register_lifecycle_routine();
	/* future in-core lifecycle types register here */
}
