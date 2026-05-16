/*-------------------------------------------------------------------------
 *
 * jsonb_toaster_lite.c
 *	 TOAST/storage layer for jsonb -- handler entry points.
 *
 * The toaster handler dispatches the per-row tsr_* callbacks
 * (validate, toast, detoast, delete, copy, update) registered with
 * core via the toastapi extension.
 *
 * Storage strategy:
 *	 - a jsonb body smaller than max_inline_size goes inline as a
 *	 JBTL_PLAIN_JSONB custom-varlena (no chunks written);
 *	 - a larger body is pushed through jbtl_toast_save_datum into
 *	 the toast relation as plain (uncompressed) chunks, and the
 *	 resulting bare TOAST pointer is wrapped in a JBTL_POINTER
 *	 custom-varlena so that core dispatches reads to our
 *	 tsr_detoast.
 *
 * Internals (chunk machinery, pointer constructors, sliced detoast
 * iterator, diff applier) were originally ported from
 * postgrespro/postgres@jsonb_toaster. All identifiers carry the
 * jbtl_/Jbtl/JBTL_ namespace; original names appear only in the
 * per-function port-trace comments.
 *
 * Copyright (c) 2026, Postgres Professional
 *
 * IDENTIFICATION
 *	 contrib/jsonb_toaster_lite/jsonb_toaster_lite.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/toast_compression.h"
#include "access/toast_hook.h"
#include "access/toast_internals.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"
#include "varatt.h"

#include "access/toast_custom.h"
#include "toastapi.h"

#include "jsonb_toaster_lite.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(jsonb_toaster_lite_handler);
PG_FUNCTION_INFO_V1(jbtl_slice_probe);
PG_FUNCTION_INFO_V1(jbtl_chunk_inspect);


/*
 * GUC: when true, jbtl_toast routes large jsonb through the
 * per-chunk-compressed writer (compress_chunks=true) and wraps the
 * result in JBTL_POINTER_COMPRESSED_CHUNKS. When false, the
 * plain-chunks path is used (JBTL_POINTER). Default is false so that
 * pre-existing tests (validate matrix, plain round-trip, plain slice)
 * are not affected; the compressed-chunks regression switches it on.
 */
static bool jbtl_compress_chunks = false;

/*
 * subtree storage GUCs.
 *
 *	enable_subtree_storage governs whether tsr_toast spills large
 *	top-level container values into separate child toast chains and
 *	emits JBTL_POINTER_SUBTREE. Default off — the subtree storage
 *	machinery is gated until wires the production writer.
 *
 *	subtree_spill_threshold is the minimum container value payload
 *	size, in bytes, that triggers the spill. Values smaller than
 *	this remain inlined in the parent body. Default 4 KB per @yoda
 *	 spec section 17.
 *
 *	Both variables are user-set: per-session adjustment is supported
 *	(useful for benches that toggle the feature without restart).
 *	Reads of these variables happen exclusively in tsr_toast; itself only registers the names.
 */
bool jbtl_enable_subtree_storage = false;
int  jbtl_subtree_spill_threshold = 4096;

void		_PG_init(void);
void		_PG_fini(void);


/*
 * SUBTREE VACUUM FULL / CLUSTER safety gate.
 *
 *	On heap rewrite (VACUUM FULL, CLUSTER) core copies CUSTOM
 *	varlenas as-is and independently renumbers chunk_ids in the
 *	toast relation -- without invoking any tsr_* callback.  That
 *	would silently orphan jbtl_subtree_refs edges and leave the
 *	embedded varatt_external inside SUBTREE varlenas pointing at
 *	chunk_ids that no longer exist.  The cleaned hook surface
 *	cannot intercept the rewrite path because no callback fires
 *	(see commit message for the rewriteheap.c:619 reasoning).
 *
 *	This module therefore refuses the command upstream via a
 *	ProcessUtility_hook.  The hook is installed ONLY when the
 *	extension is loaded via shared_preload_libraries; otherwise
 *	the gate is inactive and SUBTREE writes raise a one-shot
 *	WARNING to inform the user.  Recovery from a refused command
 *	is documented in the ERROR's errhint.
 */

/*
 * Gate state: true iff jbtl_ProcessUtility is installed as our
 * ProcessUtility_hook.  Set in _PG_init only when extension is
 * loaded via shared_preload_libraries.  Read by writer paths that
 * create SUBTREE rows to warn the user when the gate is not active.
 */
bool jbtl_gate_active = false;

/* Chained predecessor, restored in _PG_fini. */
static ProcessUtility_hook_type jbtl_prev_ProcessUtility = NULL;

/*
 * Resolve the OID of the jsonb_toaster_lite_handler function in
 * pg_proc.  This is what pgpro_toast stores as 'pgpro_toasthandler'
 * in pg_attribute.attoptions for a column managed by our toaster.
 *
 *	The cache is populated lazily by name lookup via the pg_proc
 *	syscache.  Multiple matching rows are not expected (function
 *	name is unique per schema; jsonb_toaster_lite installs into
 *	exactly one schema), but if encountered we just pick the
 *	first one -- the gate is permissive on ambiguity.
 *
 *	Also populated eagerly by the handler function itself on its
 *	first invocation (fcinfo->flinfo->fn_oid), which is the
 *	common path because core caches the TsrRoutine after the
 *	first toaster handler call.
 */
static Oid jbtl_handler_oid_cache = InvalidOid;

static Oid
jbtl_get_handler_oid(void)
{
	CatCList   *catlist;
	int			i;
	Oid			result = InvalidOid;

	if (OidIsValid(jbtl_handler_oid_cache))
		return jbtl_handler_oid_cache;

	catlist = SearchSysCacheList1(PROCNAMEARGSNSP,
								  CStringGetDatum("jsonb_toaster_lite_handler"));
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	proctup = &catlist->members[i]->tuple;
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);

		if (procform->pronargs == 0 ||
			procform->pronargs == 1)	/* internal arg, or no arg */
		{
			result = procform->oid;
			break;
		}
	}
	ReleaseSysCacheList(catlist);

	if (OidIsValid(result))
		jbtl_handler_oid_cache = result;
	return result;
}

/*
 * Return the 1-based attribute number of the first column of `rel`
 * managed by the jsonb_toaster_lite toaster, or -1 if no such
 * column exists.
 *
 *	pgpro_toast.set_toaster() records the toaster handler OID on
 *	pg_attribute.attoptions as 'pgpro_toasthandler=<NNN>'.  We
 *	read attoptions for each column via the pg_attribute syscache,
 *	parse the option list via untransformRelOptions(), and compare
 *	the recorded handler OID with the cached jbtl handler OID.
 *	This avoids depending on toastapi private internals (which
 *	are not exported as PGDLLEXPORT symbols) and on a hypothetical
 *	pg_attribute.atttoaster column (which does not exist in this
 *	cleaned tree).
 */
static int
jbtl_find_managed_attr(Relation rel)
{
	Oid			handler_oid;
	int			i;

	handler_oid = jbtl_get_handler_oid();
	if (!OidIsValid(handler_oid))
		return -1;

	for (i = 1; i <= RelationGetNumberOfAttributes(rel); i++)
	{
		HeapTuple	atttup;
		Datum		optsdat;
		bool		isnull;
		List	   *opts;
		ListCell   *lc;
		Form_pg_attribute attform;
		int			match = -1;

		atttup = SearchSysCache2(ATTNUM,
								 ObjectIdGetDatum(RelationGetRelid(rel)),
								 Int16GetDatum((int16) i));
		if (!HeapTupleIsValid(atttup))
			continue;

		attform = (Form_pg_attribute) GETSTRUCT(atttup);
		if (attform->attisdropped || attform->attlen != -1)
		{
			ReleaseSysCache(atttup);
			continue;
		}

		optsdat = SysCacheGetAttr(ATTNUM, atttup,
								  Anum_pg_attribute_attoptions, &isnull);
		if (isnull)
		{
			ReleaseSysCache(atttup);
			continue;
		}

		opts = untransformRelOptions(optsdat);
		foreach(lc, opts)
		{
			DefElem    *de = (DefElem *) lfirst(lc);
			Oid			opthandler;

			if (strcmp(de->defname, "pgpro_toasthandler") != 0)
				continue;

			opthandler = (Oid) strtoul(defGetString(de), NULL, 10);
			if (opthandler == handler_oid)
			{
				match = i;
				break;
			}
		}
		list_free_deep(opts);
		ReleaseSysCache(atttup);

		if (match > 0)
			return match;
	}
	return -1;
}

/*
 * Sequentially scan `rel`, looking for a row where attribute `attno`
 * holds a CUSTOM varlena whose mode is JBTL_POINTER_SUBTREE.
 *
 *	The probe walks live tuples under SnapshotAny (we don't care
 *	about visibility: even dead-but-not-yet-vacuumed SUBTREE rows
 *	have catalog edges that the rewrite would orphan).
 *
 *	The scan stops at the first match.  In the common case the
 *	first heap page already settles the question; large tables
 *	without SUBTREE rows pay one full seqscan, but that scan would
 *	be dwarfed by the VACUUM FULL it gates.
 */
static bool
jbtl_table_has_subtree_row(Relation rel, int attno)
{
	TableScanDesc scan;
	HeapTuple	tup;
	TupleDesc	desc = RelationGetDescr(rel);
	Snapshot	snap;
	bool		found = false;

	/*
	 * Scan under the current MVCC snapshot, not SnapshotAny.  Dead
	 * tuples will be reclaimed by VACUUM FULL anyway, and their
	 * jbtl_subtree_refs edges were already deleted by tsr_delete
	 * when the UPDATE/DELETE made them dead -- so they cannot
	 * leave orphans.  Only live SUBTREE rows are unsafe to rewrite.
	 *
	 *	Note: we use GetActiveSnapshot() rather than
	 *	GetTransactionSnapshot() because the gate runs inside
	 *	ProcessUtility_hook, where the active snapshot is the one
	 *	the eventual VACUUM FULL will see.
	 */
	snap = GetActiveSnapshot();
	scan = table_beginscan(rel, snap, 0, NULL, 0);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Datum		d;
		bool		isnull;
		struct varlena *attr;

		d = heap_getattr(tup, attno, desc, &isnull);
		if (isnull)
			continue;

		attr = (struct varlena *) DatumGetPointer(d);
		if (VARATT_IS_CUSTOM(attr))
		{
			uint32		mode =
				JBTL_CUSTOM_PTR_GET_HEADER(attr) & JBTL_POINTER_TYPE_MASK;

			if (mode == JBTL_POINTER_SUBTREE)
			{
				found = true;
				break;
			}
		}
	}
	table_endscan(scan);
	return found;
}

/*
 * Refuse VACUUM FULL / CLUSTER on `target_oid` if it currently
 * carries any SUBTREE-mode row in a jsonb_toaster_lite-managed
 * column.
 *
 *	The target's AccessExclusiveLock is acquired by the gate
 *	before the row scan, so a concurrent INSERT cannot slip a
 *	SUBTREE row past us.  The lock is held until end-of-transaction;
 *	the subsequent VACUUM FULL re-acquires (which is a no-op since
 *	we already hold it) and proceeds.
 *
 *	If `target_oid` has no jsonb_toaster_lite column at all, the
 *	gate returns immediately without scanning.
 */
static void
jbtl_check_relation_for_subtree(Oid target_oid, const char *cmd_name)
{
	Relation	rel;
	int			attno;
	bool		has_subtree;

	if (!OidIsValid(target_oid))
		return;

	/*
	 * Take AccessExclusiveLock so that no concurrent transaction
	 * can sneak a SUBTREE row in between our scan and the actual
	 * VACUUM FULL.  This is the lock VACUUM FULL would have taken
	 * anyway; acquiring it here is conservative but correct.
	 */
	rel = try_table_open(target_oid, AccessExclusiveLock);
	if (rel == NULL)
		return;

	/* Skip non-plain-table relkinds; only base tables can have toaster. */
	if (rel->rd_rel->relkind != RELKIND_RELATION &&
		rel->rd_rel->relkind != RELKIND_MATVIEW)
	{
		table_close(rel, NoLock);
		return;
	}

	attno = jbtl_find_managed_attr(rel);
	if (attno < 0)
	{
		/* No jbtl-managed column: nothing to protect. */
		table_close(rel, NoLock);
		return;
	}

	/*
	 * G1 cheap prechecks.  Both run under the AEL we just acquired,
	 * so the catalog state we observe cannot change before the
	 * eventual VACUUM FULL.
	 *
	 *	(a) Reltoastrelid invalid: a relation without a toast
	 *	relation cannot hold SUBTREE rows (SUBTREE references
	 *	chunks in pg_toast.<rel>).  Skip the heap scan.
	 *
	 *	(b) jbtl_subtree_refs holds no edge for this toast relation:
	 *	the writer invariant (jbtl_try_spill_subtree inserts an edge
	 *	in the same transaction as the SUBTREE row) guarantees that
	 *	no edge for our reltoastrelid implies no live SUBTREE row
	 *	for this table.  Under AEL no concurrent writer can change
	 *	this between probe and rewrite.  Skip the heap scan.
	 *
	 *	A false-positive in (b) -- catalog edge exists but no live
	 *	heap row carries SUBTREE state (an old orphan from a
	 *	pre-M9.1 leak) -- merely makes us fall through to the heap
	 *	scan, which returns the authoritative answer.  A
	 *	false-negative is impossible by the writer invariant.
	 */
	{
		Oid			my_toastrelid = rel->rd_rel->reltoastrelid;

		if (!OidIsValid(my_toastrelid))
		{
			table_close(rel, NoLock);
			return;
		}

		if (!jbtl_subtree_refs_any_for_toastrelid(my_toastrelid))
		{
			table_close(rel, NoLock);
			return;
		}
	}

	has_subtree = jbtl_table_has_subtree_row(rel, attno);
	table_close(rel, NoLock);

	if (has_subtree)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s is not supported on jsonb_toaster_lite "
						"SUBTREE-managed data",
						cmd_name),
				 errdetail("Core's heap-rewrite path copies SUBTREE "
						   "varlenas as-is and renumbers chunk_ids in "
						   "the toast relation, leaving entries in "
						   "jbtl_subtree_refs pointing at non-existent "
						   "chunks."),
				 errhint("Re-toast every SUBTREE row out of SUBTREE "
						 "storage by forcing a fresh datum through "
						 "the writer, e.g.:\n"
						 "    SET jsonb_toaster_lite.enable_subtree_storage = off;\n"
						 "    UPDATE <table> SET <jb_column> = <jb_column>::text::jsonb;\n"
						 "(An UPDATE that keeps the same in-memory "
						 "datum -- such as SET col = col -- does NOT "
						 "re-toast, because the value passes through "
						 "the writer unchanged.)  "
						 "%s is then permitted.",
						 cmd_name)));
}

/*
 * ProcessUtility_hook callback.
 *
 *	Intercepts T_VacuumStmt with VACOPT_FULL and T_ClusterStmt.  Other
 *	statements pass through to the prior installed hook (or the
 *	standard implementation).
 *
 *	Multi-relation VACUUM (FULL) is checked per relation; first
 *	offender raises.  ClusterStmt with a NULL relation (database-wide
 *	CLUSTER on previously-CLUSTERed tables) is left for the next
 *	hook -- the gate fires only on a named target.  Database-wide
 *	CLUSTER is rare and there is no clean way to enumerate "all
 *	tables that have ever been clustered" from within a
 *	ProcessUtility_hook without re-implementing get_tables_to_cluster;
 *	a later milestone can extend this.
 */
static void
jbtl_ProcessUtility(PlannedStmt *pstmt,
					const char *queryString,
					bool readOnlyTree,
					ProcessUtilityContext context,
					ParamListInfo params,
					QueryEnvironment *queryEnv,
					DestReceiver *dest,
					QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, VacuumStmt))
	{
		VacuumStmt *vs = (VacuumStmt *) parsetree;
		bool		is_full = false;
		ListCell   *lc;

		foreach(lc, vs->options)
		{
			DefElem    *opt = (DefElem *) lfirst(lc);

			if (strcmp(opt->defname, "full") == 0)
			{
				is_full = defGetBoolean(opt);
				if (is_full)
					break;
			}
		}

		if (is_full && vs->is_vacuumcmd)
		{
			ListCell   *lc2;

			foreach(lc2, vs->rels)
			{
				VacuumRelation *vr = (VacuumRelation *) lfirst(lc2);
				Oid			target_oid;

				if (vr->relation == NULL)
					continue;	/* "VACUUM FULL" without table: shouldn't happen with FULL but skip */

				/*
				 * vr->oid may already be resolved by the caller; if not,
				 * fall back to RangeVarGetRelid (read-only no-error
				 * resolution; the actual VACUUM will re-resolve and
				 * error if missing).
				 */
				target_oid = vr->oid;
				if (!OidIsValid(target_oid))
					target_oid = RangeVarGetRelid(vr->relation,
												  NoLock, true /* missing_ok */);

				jbtl_check_relation_for_subtree(target_oid, "VACUUM FULL");
			}
		}
	}
	else if (IsA(parsetree, RepackStmt))
	{
		RepackStmt *rs = (RepackStmt *) parsetree;

		/*
		 * RepackStmt covers CLUSTER, REPACK, and the internal
		 * VACUUMFULL dispatch (the VacuumStmt path above catches
		 * the top-level VACUUM FULL command; this branch is the
		 * direct CLUSTER / REPACK form).
		 *
		 * Database-wide CLUSTER (relation == NULL) is left for the
		 * next hook -- the gate fires only on a named target.
		 * Enumerating "all tables that have ever been clustered"
		 * here would duplicate get_tables_to_cluster; a later
		 * milestone can extend this.
		 */
		if (rs->relation != NULL &&
			(rs->command == REPACK_COMMAND_CLUSTER ||
			 rs->command == REPACK_COMMAND_REPACK ||
			 rs->command == REPACK_COMMAND_VACUUMFULL))
		{
			Oid			target_oid;
			const char *cmd_name =
				(rs->command == REPACK_COMMAND_CLUSTER) ? "CLUSTER" :
				(rs->command == REPACK_COMMAND_REPACK)  ? "REPACK"  :
														  "VACUUM FULL";

			target_oid = RangeVarGetRelid(rs->relation->relation,
										  NoLock, true /* missing_ok */);
			jbtl_check_relation_for_subtree(target_oid, cmd_name);
		}
	}

	/* Chain. */
	if (jbtl_prev_ProcessUtility)
		(*jbtl_prev_ProcessUtility) (pstmt, queryString, readOnlyTree,
									 context, params, queryEnv,
									 dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("jsonb_toaster_lite.compress_chunks",
							 "Use per-chunk pglz compression in jsonb_toaster_lite writer.",
							 NULL,
							 &jbtl_compress_chunks,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	/*
	 * subtree storage gating. See declarations above.
	 *
	 *	Names recorded here; first reader is
	 *	tsr_toast. Until then these are observable via SHOW but do
	 *	not affect any code path.
	 */
	DefineCustomBoolVariable("jsonb_toaster_lite.enable_subtree_storage",
							 "Enable subtree storage on initial INSERT.",
							 "When on, large top-level container values are "
							 "stored in separate toast chains and referenced "
							 "from the parent body via JBTL_POINTER_SUBTREE. "
							 "Production initial spill lands.",
							 &jbtl_enable_subtree_storage,
							 false,	/* boot value: OFF */
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("jsonb_toaster_lite.subtree_spill_threshold",
							"Minimum container value size (bytes) to spill into a subtree chain.",
							"Top-level container values smaller than this "
							"threshold are kept inline in the parent body. "
							"Only consulted when enable_subtree_storage is on.",
							&jbtl_subtree_spill_threshold,
							4096,		/* boot value */
							256,		/* min: smaller is meaningless */
							1024 * 1024 * 1024,	/* max: 1 GB sanity */
							PGC_USERSET,
							GUC_UNIT_BYTE,
							NULL, NULL, NULL);

	MarkGUCPrefixReserved("jsonb_toaster_lite");

	/*
	 * Install core dispatch hook for jsonb_object_field on
	 * CUSTOM-toasted jsonb. EXCLUSIVE single-installer: this
	 * extension claims ownership; a later loader overwrites
	 * silently. Counterpart unset in _PG_fini (advisory; PostgreSQL
	 * does not guarantee _PG_fini will be called).
	 */
	Toastapi_jsonb_object_field_hook = jbtl_jsonb_object_field_hook_fn;

	/*
	 * ProcessUtility_hook is the VACUUM FULL / CLUSTER safety gate
	 * for SUBTREE-managed data (see banner above jbtl_ProcessUtility).
	 *
	 * It MUST be installed via shared_preload_libraries to guarantee
	 * the hook is active in every backend.  A backend that has not
	 * yet LOAD-ed the module would silently bypass the gate, leaving
	 * the orphan-refs leak open.  We therefore install the hook
	 * ONLY when this _PG_init runs as part of shared_preload_libraries
	 * processing.  If the module was loaded another way (LOAD, or
	 * implicit load on first reference to a SQL-callable function),
	 * we set jbtl_gate_active = false so that paths which create
	 * SUBTREE rows can emit a clear WARNING about the unsafe state.
	 */
	if (process_shared_preload_libraries_in_progress)
	{
		jbtl_prev_ProcessUtility = ProcessUtility_hook;
		ProcessUtility_hook = jbtl_ProcessUtility;
		jbtl_gate_active = true;
	}
}

void
_PG_fini(void)
{
	/*
	 * Best-effort: only release the hook if we still own it. No
	 * predecessor restoration in this prototype (single-installer
	 * contract; chaining is a separate upstream design).
	 */
	if (Toastapi_jsonb_object_field_hook == jbtl_jsonb_object_field_hook_fn)
		Toastapi_jsonb_object_field_hook = NULL;

	/*
	 * Restore the ProcessUtility chain.  Best-effort: only release
	 * if we still own the top of the chain (another extension may
	 * have stacked on top of us, in which case _PG_fini cannot
	 * unwind cleanly -- PostgreSQL does not guarantee unloading
	 * order anyway).
	 */
	if (ProcessUtility_hook == jbtl_ProcessUtility)
		ProcessUtility_hook = jbtl_prev_ProcessUtility;
}

/* ---- callbacks ---------------------------------------------------------- */

/*
 * Validate a CREATE/ALTER attaching jsonb_toaster_lite to a column.
 *
 *	Accept: typeoid == JSONBOID with EXTENDED storage.
 *	Reject: anything else.
 */
static bool
jbtl_validate(Oid toasteroid, Oid typeoid, char storage, char compression,
			  Oid amoid, bool false_ok)
{
	if (typeoid == JSONBOID &&
		(storage == TYPSTORAGE_EXTENDED || storage == TYPSTORAGE_EXTERNAL))
		return true;

	if (!false_ok)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" supports only type \"%s\" with EXTENDED or EXTERNAL storage",
						"jsonb_toaster_lite", "jsonb")));

	return false;
}

#define JBTL_NOT_IMPL(opname) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("jsonb_toaster_lite: %s is not yet implemented", \
					(opname))))


/*
 * tsr_toast
 *
 *	If the input value is small enough to fit inline as a
 *	JBTL_PLAIN_JSONB custom-varlena, return one and skip toast-table
 *	chunk writes entirely. Otherwise call jbtl_toast_save_datum (or
 *	save_datum_ext when compress_chunks is on) to write chunks, and
 *	wrap the resulting bare TOAST pointer in a JBTL_POINTER (or
 *	JBTL_POINTER_COMPRESSED_CHUNKS) custom-varlena so reads come back
 *	through tsr_detoast.
 *
 *	Two arguments are intentionally not consulted:
 *
 *	 cmid the column's declared compression method (e.g. PGLZ
 *	 or LZ4). This contrib uses PGLZ unconditionally for
 *	 per-chunk compression — see the mapping
 *	 decision and the writer's pglz_compress call. LZ4
 *	 per-chunk would require an additional reader branch
 *	 and is deferred. The argument is acknowledged via
 *	 the (void) below to make the intent explicit and
 *	 quiet any future -Wunused warning.
 *
 *	 old_value the column's pre-update toasted value.  We
 *	 do not reuse old toast rows, so every UPDATE
 *	 currently re-toasts the new value in full and
 *	 dereferences the old custom-varlena (core's
 *	 tsr_delete is invoked separately). Partial rewrite
 *	 and old-value reuse land later (DIFF mode); until
 *	 then an UPDATE of a 60 KB jsonb writes 60 KB of new
 *	 toast rows even if only a leaf field changed. The
 *	 (void) below makes the intent explicit.
 *
 *	max_inline_size from the caller bounds the size of a returned
 *	custom-varlena that lives inline in the heap tuple. For an
 *	external-chunk pointer the actual on-tuple footprint is
 *	JBTL_CUSTOM_PTR_HEADER_SIZE + TOAST_POINTER_SIZE; for an inline
 *	plain-jsonb wrap it is JBTL_CUSTOM_PTR_HEADER_SIZE + VARHDRSZ +
 *	body_len.
 */
/*
 *  helper: try to emit a JBTL_POINTER_SUBTREE custom-varlena
 * for the given input. Returns Datum 0 (PointerGetDatum(NULL)) when
 * spill does not apply, in which case the caller falls through to
 * the existing chunked-write path.
 *
 *	Triggers:
 *	 - GUC jbtl_enable_subtree_storage on (caller already checked)
 *	 - input body is an object
 *	 - at least one top-level value is a container with payload
 *	 >= jbtl_subtree_spill_threshold
 *	 - rewritten parent custom-varlena fits within max_inline_size
 *
 *	Per-spilled-child sequence (per spec section 18.I-3):
 *	 1. write child body to its own toast chain
 *	 2. allocate parent_valueid via Option-1 allocator (once per
 *	 spill, NOT per child)
 *	 3. build new parent body with ISCONTAINER_PTR slots
 *	 4. wrap in JBTL_POINTER_SUBTREE v1
 *	 5. insert refs edges (one per child)
 *	 6. ANY edge insert failure → ereport (CatalogTupleInsert in
 *	 jbtl_subtree_refs_insert handles this). Hard invariant.
 */
static Datum
jbtl_try_spill_subtree(ToasterContext tcxt, struct varlena *attr,
					   int max_inline_size)
{
	JsonbContainer *root;
	int			N;
	int			n_jentries;
	int			val_base;
	int32		threshold = jbtl_subtree_spill_threshold;
	JEntry	   *jentries;
	char	   *base_addr;
	int32		key_area_size = 0;
	int			k;
	int			n_spilled = 0;
	int		   *spill_idx;	/* per-spill: index k in 0..N-1 */
	int32	   *spill_off;	/* per-spill: byte offset within data area */
	int32	   *spill_len;	/* per-spill: original value length */
	Datum	   *spill_child_ptrs;	/* per-spill: full on-disk varlena */
	int32		new_body_size;
	char	   *new_body;
	JEntry	   *new_jentries;
	int32		hdr_je_size;
	int32		new_value_payload_size;
	Relation	toastrel;
	Relation	toastidx;
	List	   *idxlist;
	Oid			parent_valueid;
	Oid			parent_toastrelid;
	struct varlena *result;
	int			j;

	/*
	 * Eligibility checks.
	 */
	root = (JsonbContainer *) VARDATA_ANY(attr);
	if (!JsonContainerIsObject(root))
		return (Datum) 0;
	if (JsonContainerHasKVMap(root))
		return (Datum) 0;	/* scope */

	N = JsonContainerSize(root);
	if (N == 0)
		return (Datum) 0;
	n_jentries = 2 * N;
	val_base = N;
	jentries = root->children;
	base_addr = (char *) (jentries + n_jentries);

	/*
	 * Reject offset-cache flag on any value JEntry.
	 *
	 * NOTE: HAS_OFF on a key JEntry is harmless (writer copies keys
	 * byte-identical, reader walks back through unchanged key offsets).
	 * HAS_OFF on a value JEntry is the dangerous case, because:
	 *  - the spillable slot itself may carry HAS_OFF (when its index is
	 *    a stride hit, e.g. val_base + 0 for N == JENTRYOFFSETSTRIDE),
	 *    and overwriting it to ISCONTAINER_PTR without HAS_OFF leaves
	 *    the original JEntry's cached-offset semantics in a torn state;
	 *  - downstream value JEntries with HAS_OFF still carry pre-spill
	 *    cached offsets that no longer match the new data area where
	 *    the spilled value has shrunk from val_len to TOAST_POINTER
	 *    payload size.
	 *
	 * The previous loop started at val_base + 1, missing the val_base
	 * slot itself.  When the alphabetically-first key happens to be
	 * the large spillable child and N == JENTRYOFFSETSTRIDE, that slot
	 * carries HAS_OFF and the resulting SUBTREE row is unreadable
	 * (malloc corruption observed on detoast).  Start at val_base.
	 */
	for (k = val_base; k < n_jentries; k++)
		if (jentries[k] & JENTRY_HAS_OFF)
			return (Datum) 0;

	/* Sum key lengths. */
	for (k = 0; k < N; k++)
		key_area_size += (int32) getJsonbLength(root, k);

	/*
	 * Pass 1: identify spill candidates. A candidate is a value
	 * JEntry that's an ISCONTAINER and whose length >= threshold.
	 * Allocate temp arrays sized to N (worst case all values spill).
	 */
	spill_idx = (int *) palloc(N * sizeof(int));
	spill_off = (int32 *) palloc(N * sizeof(int32));
	spill_len = (int32 *) palloc(N * sizeof(int32));
	spill_child_ptrs = (Datum *) palloc(N * sizeof(Datum));

	for (k = 0; k < N; k++)
	{
		JEntry		je = jentries[val_base + k];
		int32		vlen = (int32) (je & JENTRY_OFFLENMASK);

		if ((je & JENTRY_TYPEMASK) == JENTRY_ISCONTAINER &&
			vlen >= threshold)
		{
			spill_idx[n_spilled] = k;
			spill_off[n_spilled] = (int32) getJsonbOffset(root, val_base + k);
			spill_len[n_spilled] = vlen;
			n_spilled++;
		}
	}

	if (n_spilled == 0)
	{
		pfree(spill_idx); pfree(spill_off); pfree(spill_len);
		pfree(spill_child_ptrs);
		return (Datum) 0;
	}

	/*
	 * Compute new body size first to reject if it wouldn't fit
	 * inline anyway. Rewriting an ISCONTAINER (length=val_len)
	 * into ISCONTAINER_PTR with payload size = sizeof(JEntry) +
	 * TOAST_POINTER_SIZE = 22 bytes.
	 */
	hdr_je_size = (int32) sizeof(uint32) + n_jentries * (int32) sizeof(JEntry);
	new_value_payload_size = (int32) sizeof(JEntry) + TOAST_POINTER_SIZE;
	new_body_size = (int32) VARSIZE_ANY_EXHDR(attr);
	for (j = 0; j < n_spilled; j++)
		new_body_size += new_value_payload_size - spill_len[j];

	{
		int32		wrapped_size = JBTL_CUSTOM_PTR_HEADER_SIZE +
			(int32) sizeof(JbtlSubtreeHeader) + new_body_size;

		if (wrapped_size > max_inline_size)
		{
			pfree(spill_idx); pfree(spill_off); pfree(spill_len);
			pfree(spill_child_ptrs);
			return (Datum) 0;
		}
	}

	/*
	 * About to create a SUBTREE row + insert refs edges.  If the
	 * ProcessUtility gate is not active (extension not loaded via
	 * shared_preload_libraries in this backend), warn once per
	 * statement: a subsequent VACUUM FULL or CLUSTER will silently
	 * leave orphan edges in jbtl_subtree_refs.
	 *
	 *	Note: this only warns the writer; it does NOT refuse the
	 *	write, because the writer state is otherwise correct and
	 *	the data is readable.  The user can recover with
	 *	jbtl_subtree_refs_gc() after the rewrite, or add
	 *	shared_preload_libraries = 'jsonb_toaster_lite' and
	 *	restart to enable the gate.
	 */
	if (!jbtl_gate_active)
		ereport(WARNING,
				(errcode(ERRCODE_WARNING),
				 errmsg("jsonb_toaster_lite: SUBTREE write without "
						"VACUUM FULL safety gate"),
				 errdetail("The ProcessUtility hook that refuses "
						   "VACUUM FULL / CLUSTER on SUBTREE-managed "
						   "data is only installed when "
						   "jsonb_toaster_lite is loaded via "
						   "shared_preload_libraries.  This backend "
						   "did not load it that way, so a subsequent "
						   "VACUUM FULL on this table could leave "
						   "orphan entries in jbtl_subtree_refs."),
				 errhint("Add 'jsonb_toaster_lite' to "
						 "shared_preload_libraries and restart the "
						 "server before relying on the gate.")));

	/*
	 * Open the heap row's toast relation + first index for child
	 * chain writes and parent_valueid allocation.
	 */
	parent_toastrelid = tcxt->rel->rd_rel->reltoastrelid;
	if (!OidIsValid(parent_toastrelid))
	{
		pfree(spill_idx); pfree(spill_off); pfree(spill_len);
		pfree(spill_child_ptrs);
		return (Datum) 0;
	}

	toastrel = table_open(parent_toastrelid, RowExclusiveLock);
	idxlist = RelationGetIndexList(toastrel);
	if (idxlist == NIL)
	{
		table_close(toastrel, RowExclusiveLock);
		pfree(spill_idx); pfree(spill_off); pfree(spill_len);
		pfree(spill_child_ptrs);
		return (Datum) 0;
	}
	toastidx = index_open(linitial_oid(idxlist), RowExclusiveLock);

	/*
	 * Pass 2: write each child as its own toast chain. Use the
	 * lite writer (jbtl_toast_save_datum) so chunks live in the
	 * same toast relation as everything else.
	 */
	for (j = 0; j < n_spilled; j++)
	{
		const char *value_bytes = base_addr + spill_off[j];
		int32		vlen = spill_len[j];
		struct varlena *child_v;

		child_v = (struct varlena *) palloc(vlen + VARHDRSZ);
		SET_VARSIZE(child_v, vlen + VARHDRSZ);
		memcpy(VARDATA(child_v), value_bytes, vlen);

		spill_child_ptrs[j] =
			jbtl_toast_save_datum(tcxt->rel, PointerGetDatum(child_v),
								  NULL, 0);
		pfree(child_v);
	}

	/*
	 * Allocate one synthetic parent_valueid via the
	 * Option-1 allocator.
	 */
	parent_valueid = jbtl_alloc_subtree_parent_valueid(toastrel, toastidx);

	index_close(toastidx, RowExclusiveLock);
	table_close(toastrel, RowExclusiveLock);
	list_free(idxlist);

	/*
	 * Build the new parent body.
	 *	[ hdr ][ JEntries ][ keys ][ values w/ pointers at spill slots ]
	 */
	new_body = (char *) palloc(new_body_size);
	{
		uint32		new_hdr = root->header | JBTL_JBC_TOBJECT_TOASTED;

		memcpy(new_body, &new_hdr, sizeof(new_hdr));
	}

	new_jentries = (JEntry *) (new_body + sizeof(uint32));
	memcpy(new_jentries, jentries, n_jentries * sizeof(JEntry));
	for (j = 0; j < n_spilled; j++)
	{
		new_jentries[val_base + spill_idx[j]] =
			JBTL_JENTRY_ISCONTAINER_PTR |
			(new_value_payload_size & JENTRY_OFFLENMASK);
	}

	/* Keys: byte-identical block. */
	memcpy(new_body + hdr_je_size, base_addr, key_area_size);

	/* Values, walking spill slots in order. */
	{
		int32		dst_pos = hdr_je_size + key_area_size;
		int32		src_pos_in_data = key_area_size;
		int			next_spill = 0;

		for (k = 0; k < N; k++)
		{
			int32		this_len =
				(int32) getJsonbLength(root, val_base + k);

			if (next_spill < n_spilled && spill_idx[next_spill] == k)
			{
				/* Write JbtlToastedContainerPointer payload. */
				JbtlToastedContainerPointer *payload =
					(JbtlToastedContainerPointer *) (new_body + dst_pos);
				const char *child_bare =
					DatumGetPointer(spill_child_ptrs[next_spill]);

				/* Copy child container header (first 4 bytes of value). */
				memcpy(&payload->header,
					   base_addr + src_pos_in_data,
					   sizeof(JEntry));
				memcpy(payload->data, child_bare, TOAST_POINTER_SIZE);

				dst_pos += new_value_payload_size;
				src_pos_in_data += this_len;
				next_spill++;
			}
			else
			{
				memcpy(new_body + dst_pos,
					   base_addr + src_pos_in_data, this_len);
				dst_pos += this_len;
				src_pos_in_data += this_len;
			}
		}
		Assert(dst_pos == new_body_size);
	}

	/*
	 * Wrap in v1 SUBTREE custom-varlena.
	 */
	result = jbtl_toast_make_pointer_subtree_v1(tcxt->toasterid,
												parent_valueid,
												parent_toastrelid,
												new_body, new_body_size);

	/*
	 * Insert refs edges — one per child. Failure aborts txn per
	 * spec invariant I-3.2 (CatalogTupleInsert ereports on PK
	 * collision).
	 */
	for (j = 0; j < n_spilled; j++)
	{
		struct varatt_external child_ext;

		VARATT_EXTERNAL_GET_POINTER(child_ext,
									DatumGetPointer(spill_child_ptrs[j]));
		jbtl_subtree_refs_insert(parent_toastrelid, parent_valueid,
								 child_ext.va_toastrelid,
								 child_ext.va_valueid);
	}

	pfree(new_body);
	for (j = 0; j < n_spilled; j++)
		pfree(DatumGetPointer(spill_child_ptrs[j]));
	pfree(spill_idx); pfree(spill_off); pfree(spill_len);
	pfree(spill_child_ptrs);

	return PointerGetDatum(result);
}


static Datum
jbtl_toast(ToasterContext tcxt, Datum value, Datum old_value,
		   int max_inline_size, int am_options, char att_storage,
		   ToastCompressionId cmid)
{
	struct varlena *attr;
	Datum		toasted_datum;
	struct varatt_external toast_ptr;

	/*
	 * Acknowledge intentionally-ignored arguments. See the function
	 * banner above for rationale.
	 */
	(void) cmid;			/* PGLZ-only by design. */
	(void) old_value;		/* full re-toast on UPDATE; + DIFF */
	(void) att_storage;		/* validate already restricted to EXTENDED|EXTERNAL */

	/*
	 * If input is already a JBTL custom-pointer, pass it through
	 * unchanged. This happens when tsr_update returned the new value
	 * as a custom varlena (e.g. the SUBTREE fixture path) and
	 * core still calls tsr_toast on the result during the standard
	 * heap_update flow. Without this guard we would re-wrap the
	 * custom varlena in another JBTL_POINTER and write its raw bytes
	 * (including the inner custom header) into a fresh toast chain —
	 * which produces an extra orphan valueid and corrupts subsequent
	 * reads.
	 */
	attr = (struct varlena *) DatumGetPointer(value);
	if (VARATT_IS_CUSTOM(attr))
		return value;

	/*
	 * Detoast input fully if it arrives as an EXTERNAL or COMPRESSED
	 * varlena. We then have a plain in-memory jsonb body to chunk.
	 */
	if (VARATT_IS_EXTENDED(attr))
		attr = detoast_attr(attr);

	/*
	 * Inline path: the whole body fits in max_inline_size when wrapped
	 * in a JBTL_PLAIN_JSONB custom-varlena. We return the wrapped
	 * value; core stores it inline in the heap tuple.
	 */
	{
		Size		body_len = VARSIZE_ANY_EXHDR(attr);
		Size		inline_wrap_size =
			JBTL_CUSTOM_PTR_HEADER_SIZE + VARHDRSZ + body_len;

		if ((int) inline_wrap_size <= max_inline_size)
		{
			JsonbContainer *jbc = (JsonbContainer *) VARDATA_ANY(attr);

			return PointerGetDatum(
				jbtl_toast_make_plain_pointer(tcxt->toasterid,
											  jbc, body_len));
		}
	}

	/*
	 * subtree spill path.
	 *
	 *	When enable_subtree_storage is on, look at the top-level
	 *	object values and spill any container value larger than the
	 *	threshold into its own toast chain, replacing it inline with
	 *	a JBTL_JENTRY_ISCONTAINER_PTR + JbtlToastedContainerPointer.
	 *	Wrap the rewritten parent body in a JBTL_POINTER_SUBTREE
	 *	custom-varlena with a v1 header carrying the synthetic
	 *	parent_valueid + parent_toastrelid. Insert one refs edge per
	 *	spilled child.
	 *
	 *	Falls through to the existing JBTL_POINTER chunk-write path
	 *	when:
	 *	 - GUC is off (default)
	 *	 - body is not a plain object
	 *	 - no top-level container value exceeds threshold
	 *	 - rewritten parent body wouldn't fit in max_inline_size
	 *	 (rare; we'd lose the parent-locality win and might as
	 *	 well chunk the whole body)
	 *
	 *	Scope limits per @yoda directive:
	 *	 - top-level object only (arrays deferred to a future commit)
	 *	 - one-level spill, no recursion
	 *	 - no update reuse (every UPDATE re-spills from scratch)
	 *	 - no copy semantics
	 */
	if (jbtl_enable_subtree_storage)
	{
		Datum		spilled = jbtl_try_spill_subtree(tcxt, attr,
													 max_inline_size);

		if (spilled != (Datum) 0)
			return spilled;
	}

	/*
	 * External path: write chunks via the writer. Compress per-chunk
	 * iff jbtl_compress_chunks GUC is set; in that case wrap in
	 * JBTL_POINTER_COMPRESSED_CHUNKS so reads go through the
	 * focused compressed-chunks reader. Otherwise plain chunks +
	 * JBTL_POINTER.
	 */
	if (jbtl_compress_chunks)
	{
		Datum		raw_value;
		struct varlena *bare_ptr;

		/*
		 * jbtl_toast_save_datum_ext does the heavy lifting: opens the
		 * toast relation, picks a valueid, and calls
		 * jbtl_toast_write_slice with compress_chunks=true. We pass
		 * compress_chunks=true explicitly; the writer falls back to
		 * raw on a per-row basis if pglz cannot beat the threshold.
		 *
		 * In the compress_chunks=true branch save_datum_ext returns
		 * a JBTL_POINTER_COMPRESSED_CHUNKS custom-varlena directly
		 * (built by jbtl_toast_make_pointer_compressed_chunks), so we
		 * do NOT wrap again. See save_datum_ext's tail section.
		 */
		raw_value = PointerGetDatum(attr);
		toasted_datum = jbtl_toast_save_datum_ext(tcxt->rel,
												  tcxt->toasterid,
												  raw_value,
												  NULL,	/* oldexternal */
												  am_options,
												  NULL,	/* p_chunk_tids_ptr */
												  NULL,	/* chunk_tids */
												  true);	/* compress_chunks */

		bare_ptr = (struct varlena *) DatumGetPointer(toasted_datum);
		Assert(VARATT_IS_CUSTOM(bare_ptr));
		Assert((JBTL_CUSTOM_PTR_GET_HEADER(bare_ptr) & JBTL_POINTER_TYPE_MASK) ==
			   JBTL_POINTER_COMPRESSED_CHUNKS);

		return toasted_datum;
	}

	/*
	 * Plain-chunks path: write plain chunks via jbtl_toast_save_datum,
	 * then wrap the bare TOAST pointer it returns in a JBTL_POINTER
	 * custom-varlena.
	 */
	toasted_datum = jbtl_toast_save_datum(tcxt->rel, PointerGetDatum(attr),
										  NULL, am_options);

	Assert(VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(toasted_datum)));
	VARATT_EXTERNAL_GET_POINTER(toast_ptr,
								DatumGetPointer(toasted_datum));

	{
		struct varlena *result =
			jbtl_toast_wrap_in_jbtl_pointer(tcxt->toasterid, &toast_ptr);

		pfree(DatumGetPointer(toasted_datum));
		return PointerGetDatum(result);
	}
}


/*
 * tsr_detoast (full read + real sliced read for plain chunks)
 *
 *	Core invokes us when it sees VARATT_IS_CUSTOM and the toaster ID
 *	matches. Inspect the JBTL mode tag and dispatch:
 *
 *	 JBTL_PLAIN_JSONB -> body lives inline in the custom-varlena;
 *	 build a fresh varlena copy and return.
 *	 Slice is materialized in memory because
 *	 the body is fully present anyway.
 *	 JBTL_POINTER -> the inline tail is a varatt_external; for
 *	 full read (length < 0 or covers the whole
 *	 body) call jbtl_toast_fetch_full_plain;
 *	 for a proper slice call
 *	 jbtl_toast_fetch_slice_plain, which uses
 *	 table_relation_fetch_toast_slice with a
 *	 non-zero sliceoffset, causing
 *	 heap_fetch_toast_slice to read only the
 *	 chunks overlapping [offset, offset+length).
 *
 *	 covers the plain-chunk slice case only. Per-chunk
 *	decompression and the JBTL_POINTER_COMPRESSED_CHUNKS path land in
 *	 together with the writer-side pglz reimplementation.
 *
 *	Other JBTL_POINTER_* modes (DIRECT_TIDS, COMPRESSED_CHUNKS, DIFF)
 *	are written by no / code path and so cannot
 *	legitimately arrive here yet. We refuse them with a clear error.
 */
static Datum
jbtl_detoast(ToasterContext tcxt, Datum toast_ptr, int offset, int length)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(toast_ptr);
	uint32		mode;
	int32		raw_size;

	Assert(VARATT_IS_CUSTOM(attr));

	mode = JBTL_CUSTOM_PTR_GET_HEADER(attr) & JBTL_POINTER_TYPE_MASK;
	raw_size = (int32) VARATT_CUSTOM_GET_DATA_RAW_SIZE(attr);

	switch (mode)
	{
		case JBTL_PLAIN_JSONB:
			{
				/*
				 * Inline body sits in JBTL_CUSTOM_PTR_GET_DATA(attr) as a
				 * full varlena (4-byte header + jsonb body). Slice is
				 * materialized in memory because the body is fully
				 * present in the row anyway -- there is no I/O to save.
				 */
				char	   *body = JBTL_CUSTOM_PTR_GET_DATA(attr);
				int32		body_size = VARSIZE(body);
				int32		body_payload = body_size - VARHDRSZ;

				if (length < 0 || (offset == 0 && length >= body_payload))
				{
					struct varlena *result =
						(struct varlena *) palloc(body_size);
					memcpy(result, body, body_size);
					return PointerGetDatum(result);
				}
				else
				{
					int32		want_off = offset < 0 ? 0 :
						offset > body_payload ? body_payload : offset;
					int32		want_len = length < 0 ? 0 : length;
					struct varlena *slice;

					if (want_len > body_payload - want_off)
						want_len = body_payload - want_off;

					slice = (struct varlena *) palloc(want_len + VARHDRSZ);
					SET_VARSIZE(slice, want_len + VARHDRSZ);
					memcpy(VARDATA(slice),
						   (char *) body + VARHDRSZ + want_off,
						   want_len);
					return PointerGetDatum(slice);
				}
			}

		case JBTL_POINTER:
			{
				/*
				 * The inline tail is a TOAST_POINTER_SIZE-byte bare
				 * external pointer. For full reads delegate to
				 * jbtl_toast_fetch_full_plain; for slices use
				 * jbtl_toast_fetch_slice_plain so heap-side fetcher
				 * reads only chunks overlapping the requested range.
				 */
				struct varatt_external ext_ptr;
				char	   *bare = JBTL_CUSTOM_PTR_GET_DATA(attr);
				int32		body_payload =
					raw_size > VARHDRSZ ? raw_size - VARHDRSZ : raw_size;

				Assert(VARATT_IS_EXTERNAL_ONDISK(bare));
				VARATT_EXTERNAL_GET_POINTER(ext_ptr, bare);

				if (length < 0 || (offset == 0 && length >= body_payload))
				{
					/*
					 * Full read: assemble entire body, return varlena
					 * that begins with VARHDRSZ + jsonb body.
					 */
					return PointerGetDatum(jbtl_toast_fetch_full_plain(&ext_ptr,
																	   NULL));
				}
				else
				{
					/*
					 * Real slice: only chunks overlapping the requested
					 * byte range will be read from disk. Returned varlena
					 * carries exactly the requested slicelength bytes in
					 * its VARDATA, *not* a jsonb body. The caller (core's
					 * detoast_attr_slice) is responsible for interpreting
					 * the slice payload appropriately.
					 */
					return PointerGetDatum(
						jbtl_toast_fetch_slice_plain(&ext_ptr,
													 offset, length,
													 NULL, NULL, NULL));
				}
			}

		case JBTL_POINTER_COMPRESSED_CHUNKS:
			{
				/*
				 * Per-chunk-compressed path. The inline tail
				 * is the bare TOAST pointer; we hand it to the focused
				 * compressed-chunks reader, which decides between full
				 * read and slice based on the requested offset/length.
				 *
				 *	The reader walks rows in chunk_seq order (= last
				 *	byte offset for this mode), decompresses compressed
				 *	rows whole and memcpys raw fallback rows directly,
				 *	stopping as soon as a row's first byte exceeds the
				 *	requested range.
				 */
				struct varatt_external ext_ptr;
				char	   *bare = JBTL_CUSTOM_PTR_GET_DATA(attr);
				int32		body_payload =
					raw_size > VARHDRSZ ? raw_size - VARHDRSZ : raw_size;
				struct varlena *result;
				int32		want_off;
				int32		want_len;

				Assert(VARATT_IS_EXTERNAL_ONDISK(bare));
				VARATT_EXTERNAL_GET_POINTER(ext_ptr, bare);

				if (length < 0 || (offset == 0 && length >= body_payload))
				{
					/* Full read. */
					want_off = 0;
					want_len = body_payload;
					result = jbtl_toast_fetch_compressed_chunks(&ext_ptr,
																want_off,
																want_len,
																NULL, NULL,
																NULL, NULL,
																NULL);
					/*
					 * For full reads the caller in core (detoast_attr)
					 * expects a varlena that begins with VARHDRSZ + body
					 * and reports its total size; that is exactly what
					 * the reader emits when sliceoffset=0 and
					 * slicelength=full.
					 */
					return PointerGetDatum(result);
				}
				else
				{
					want_off = offset;
					want_len = length;
					result = jbtl_toast_fetch_compressed_chunks(&ext_ptr,
																want_off,
																want_len,
																NULL, NULL,
																NULL, NULL,
																NULL);
					return PointerGetDatum(result);
				}
			}

		case JBTL_POINTER_DIFF:
		case JBTL_POINTER_DIFF_COMP:
			{
				/*
				 * DIFF overlay. The inline tail is:
				 *  [varatt_external base][JbtlPointerDiff: offset, data[]]
				 *
				 * Apply path: fetch the FULL base body (delegating to
				 * the plain or compressed-chunks reader depending on
				 * mode), then memcpy the diff data over the result at
				 * `offset`. The buffer is sized at base.va_rawsize
				 * (the DIFF format never extends past base length —
				 * that invariant is enforced by the writer-side
				 * same-byte-length gate).
				 *
				 * Slicing is handled by re-slicing the assembled body
				 * after the overlay is applied. Correctness-first;
				 * future work can push the slice into the base read.
				 */
				struct varatt_external ext_ptr;
				char	   *bare = JBTL_CUSTOM_PTR_GET_DATA(attr);
				char	   *inline_data;
				int32		inline_size;
				JbtlPointerDiff *diff;
				int32		diff_size;
				int32		body_size;
				int32		body_payload;
				struct varlena *full;

				Assert(VARATT_IS_EXTERNAL_ONDISK(bare));
				VARATT_EXTERNAL_GET_POINTER(ext_ptr, bare);

				/*
				 * Inline-tail layout: varatt_external (TOAST_POINTER_SIZE)
				 * + JbtlPointerDiff at offset TOAST_POINTER_SIZE.
				 * Diff data length is implicit:
				 *  total custom data area − varatt_external − diff hdr.
				 *
				 * JBTL_CUSTOM_PTR_GET_DATA_SIZE(attr) returns the size of
				 * the data area starting at JBTL_CUSTOM_PTR_GET_DATA(attr).
				 */
				inline_data = bare + TOAST_POINTER_SIZE;
				inline_size =
					(int32) JBTL_CUSTOM_PTR_GET_DATA_SIZE(attr)
					- (int32) TOAST_POINTER_SIZE;
				diff = (JbtlPointerDiff *) inline_data;
				diff_size = inline_size - (int32) offsetof(JbtlPointerDiff, data);

				/* Fetch the base body in full. */
				if (mode == JBTL_POINTER_DIFF)
				{
					full = jbtl_toast_fetch_full_plain(&ext_ptr, NULL);
				}
				else
				{
					/* compressed base: full read via compressed reader */
					int32		base_attrsize =
						VARATT_EXTERNAL_GET_EXTSIZE(ext_ptr);
					int32		base_payload =
						base_attrsize > VARHDRSZ
						? base_attrsize - VARHDRSZ
						: base_attrsize;

					full = jbtl_toast_fetch_compressed_chunks(&ext_ptr,
															  0,
															  base_payload,
															  NULL, NULL,
															  NULL, NULL,
															  NULL);
				}

				body_size = (int32) VARSIZE(full);
				body_payload = body_size - VARHDRSZ;

				/*
				 * Apply the overlay. Bounds: diff->offset is an
				 * offset within the body PAYLOAD (not the varlena),
				 * so the destination is VARDATA(full) + diff_offset.
				 */
				if (diff->offset < 0 ||
					diff->offset + diff_size > body_payload)
				{
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("jsonb_toaster_lite: DIFF overlay out of bounds "
									"(offset=%d, size=%d, body_payload=%d)",
									diff->offset, diff_size, body_payload)));
				}

				memcpy(VARDATA(full) + diff->offset, diff->data, diff_size);

				/* Apply caller's slicing over the assembled body. */
				if (length < 0 || (offset == 0 && length >= body_payload))
				{
					return PointerGetDatum(full);
				}
				else
				{
					int32		want_off = offset < 0 ? 0 :
						offset > body_payload ? body_payload : offset;
					int32		want_len = length < 0 ? 0 : length;
					struct varlena *slice;

					if (want_len > body_payload - want_off)
						want_len = body_payload - want_off;

					slice = (struct varlena *) palloc(want_len + VARHDRSZ);
					SET_VARSIZE(slice, want_len + VARHDRSZ);
					memcpy(VARDATA(slice),
						   VARDATA(full) + want_off,
						   want_len);
					pfree(full);
					return PointerGetDatum(slice);
				}
			}

		case JBTL_POINTER_SUBTREE:
			{
				/*
				 *  subtree-aware reader.
				 *
				 *	The custom-varlena's data area holds an inline parent
				 *	body — a regular jsonb container, except that some
				 *	JEntries may have type JBTL_JENTRY_ISCONTAINER_PTR
				 *	indicating that the value-data slot at that offset
				 *	contains a JbtlToastedContainerPointer rather than a
				 *	plain inline value. Each ISCONTAINER_PTR slot points
				 *	to a separate toast chain (the "child").
				 *
				 *	Reader strategy:
				 *	 1. count children, fetch each child body fully
				 *	 2. allocate output buffer sized for the assembled
				 *	 body
				 *	 3. copy parent header (clearing the
				 *	 JBTL_JBC_TOBJECT_TOASTED hint bit since the
				 *	 output body has no subtree pointers)
				 *	 4. rewrite JEntries: every ISCONTAINER_PTR becomes
				 *	 ISCONTAINER with length = child body payload size
				 *	 5. copy KVMap (if present), keys, and values —
				 *	 substituting child bodies at ISCONTAINER_PTR
				 *	 slot positions
				 *	 6. apply caller-requested slicing
				 *
				 *	Recursion is NOT supported in child bodies are
				 *	assumed to be plain jsonb (no nested ISCONTAINER_PTR).
				 *	A future milestone may extend this when recursive
				 *	spill is implemented.
				 */
				char	   *payload = JBTL_CUSTOM_PTR_GET_DATA(attr);
				int32		payload_size = (int32) JBTL_CUSTOM_PTR_GET_DATA_SIZE(attr);
				char	   *parent_body;
				int32		parent_size;
				JsonbContainer *root;
				int			N;
				bool		is_object;
				int			n_jentries;
				int			i;

				/*
				 * version dispatch. v0 = no header. v1 = JbtlSubtreeHeader prefix. Anything
				 * else is corruption.
				 *
				 * Detection: a v1 header always has version == 1 in
				 * the first byte; v0 has the parent body's first byte
				 * which is the low byte of the JsonbContainer header.
				 * That low byte encodes object/array bits and count;
				 * for any non-empty container it is large (object
				 * with at least one entry → count >= 1, plus type
				 * bits set) and never equals 1.
				 *
				 * Concretely: JsonContainer header has JB_FOBJECT or
				 * JB_FARRAY in the high bits and count in the low
				 * 24 bits. A header value with low byte == 1 would
				 * mean a 1-entry object/array with no type bits set
				 * in the low byte — impossible because JB_FOBJECT
				 * (0x20000000) has its low byte 0 anyway, but
				 * JB_FARRAY etc. similarly do not collide with
				 * uint8 == 1 in the LSB. v0 fixture has been
				 * verified to never produce payload[0] == 1.
				 */
				if (payload_size >= (int32) sizeof(JbtlSubtreeHeader) &&
					(uint8) payload[0] == JBTL_SUBTREE_HEADER_V1)
				{
					const JbtlSubtreeHeader *hdr =
						(const JbtlSubtreeHeader *) payload;

					if (hdr->header_size != sizeof(JbtlSubtreeHeader))
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("jsonb_toaster_lite: SUBTREE v1 header_size mismatch (%u vs %zu)",
										hdr->header_size,
										sizeof(JbtlSubtreeHeader))));
					parent_body = payload + hdr->header_size;
					parent_size = payload_size - hdr->header_size;
				}
				else
				{
					/* v0: payload is the body. */
					parent_body = payload;
					parent_size = payload_size;
				}
				{

				/*
				 * Per-child slot table: parsed from the parent body in
				 * one pass. For each value index k that has a child
				 * pointer, we record the child's varatt_external and
				 * its assembled body (fetched lazily during pass 2).
				 */
				typedef struct
				{
					int			value_idx;	/* k in 0..N-1 */
					int32		old_off;	/* byte offset of the inline
											 * pointer payload relative to
											 * the start of value-data area */
					int32		old_len;	/* byte length of the inline
											 * pointer payload */
					struct varatt_external ext;
					struct varlena *child_full;	/* assembled child body */
				}			ChildSlot;

				ChildSlot  *slots;
				int			nslots = 0;
				int32		child_total_extra = 0;	/* sum (child_payload − old_len) */
				JEntry	   *parent_children;
				int32		data_area_offset;
				int32		key_area_size = 0;
				int			k;
				int32		assembled_payload;
				int32		assembled_size;
				struct varlena *out;
				char	   *outp;
				JEntry	   *out_jentries;
				int32		cur_off_in_data;
				int			body_payload;

				if (parent_size < (int32) sizeof(uint32))
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("jsonb_toaster_lite: SUBTREE parent body too small (%d)",
									parent_size)));

				root = (JsonbContainer *) parent_body;
				is_object = JsonContainerIsObject(root);
				N = JsonContainerSize(root);
				n_jentries = is_object ? 2 * N : N;
				parent_children = root->children;

				slots = (ChildSlot *) palloc0(N * sizeof(ChildSlot));

				/*
				 * Compute data_area_offset (where the data area starts):
				 * 4-byte header + 2N-or-N JEntries + INTALIGN'd KVMap.
				 * Note: lite L1 currently does not emit KVMap-bearing
				 * objects through the SUBTREE path; if/when it does,
				 * JsonContainerHasKVMap() picks up the bit.
				 */
				{
					int			n_kvmap_bytes = 0;

					if (is_object && JsonContainerHasKVMap(root))
						n_kvmap_bytes = INTALIGN(N * JSONB_KVMAP_ENTRY_SIZE(N));

					data_area_offset =
						(int32) sizeof(uint32) +
						n_jentries * (int32) sizeof(JEntry) +
						n_kvmap_bytes;
				}

				/* Sum of key lengths (objects only); arrays go straight to values. */
				if (is_object)
				{
					for (k = 0; k < N; k++)
						key_area_size += (int32) getJsonbLength(root, k);
				}

				/*
				 * Pass 1: scan value JEntries (indices N..2N-1 for
				 * objects, 0..N-1 for arrays); record ISCONTAINER_PTR
				 * slots; fetch each child body fully.
				 */
				{
					int			val_base = is_object ? N : 0;

					for (k = 0; k < N; k++)
					{
						JEntry		je = parent_children[val_base + k];

						if (JBTL_JBE_ISCONTAINER_PTR(je))
						{
							ChildSlot  *slot = &slots[nslots++];
							int32		val_off_within_data =
								(int32) getJsonbOffset(root, val_base + k);
							int32		val_len = (int32) (je & JENTRY_OFFLENMASK);
							const char *payload_addr;
							const JbtlToastedContainerPointer *ptr;

							slot->value_idx = k;
							slot->old_off = val_off_within_data;
							slot->old_len = val_len;

							/*
							 * Locate the JbtlToastedContainerPointer
							 * payload. In objects, value data starts
							 * AFTER the keys area. Note that
							 * getJsonbOffset for value index N+k returns
							 * an offset RELATIVE TO THE DATA AREA (which
							 * begins with keys for objects), so we add
							 * data_area_offset.
							 */
							payload_addr = parent_body + data_area_offset + val_off_within_data;

							if (val_len < (int32) (sizeof(JEntry) + TOAST_POINTER_SIZE))
								ereport(ERROR,
										(errcode(ERRCODE_DATA_CORRUPTED),
										 errmsg("jsonb_toaster_lite: ISCONTAINER_PTR payload too small (%d)",
												val_len)));

							ptr = (const JbtlToastedContainerPointer *) payload_addr;
							/*
							 * ptr->data[0..TOAST_POINTER_SIZE-1] is a
							 * full on-disk varlena pointer (2-byte header
							 * + 18-byte varatt_external). Use the macro
							 * that strips the header and gives us the
							 * varatt_external proper.
							 */
							VARATT_EXTERNAL_GET_POINTER(slot->ext, ptr->data);

							if (!VARATT_EXTERNAL_GET_EXTSIZE(slot->ext))
								ereport(ERROR,
										(errcode(ERRCODE_DATA_CORRUPTED),
										 errmsg("jsonb_toaster_lite: SUBTREE child has zero ext size")));

							/*
							 * Fetch child fully via plain reader. In
							 *  children are always plain (no
							 * compressed-chunks nor nested SUBTREE);
							 * compressed-base support lands later.
							 */
							slot->child_full = jbtl_toast_fetch_full_plain(&slot->ext, NULL);

							child_total_extra +=
								((int32) VARSIZE(slot->child_full) - VARHDRSZ) - val_len;
						}
					}
				}

				/*
				 * Compute assembled body size and allocate output buffer.
				 *
				 * If there are no child pointers, the SUBTREE body is
				 * effectively a plain inline body and we just hand it
				 * back wrapped in VARHDRSZ.
				 */
				assembled_payload = parent_size + child_total_extra;
				assembled_size = assembled_payload + VARHDRSZ;
				out = (struct varlena *) palloc(assembled_size);
				SET_VARSIZE(out, assembled_size);
				outp = VARDATA(out);

				/*
				 * Copy header (clearing JBTL_JBC_TOBJECT_TOASTED hint bit
				 * since assembled body has no child pointers).
				 */
				{
					uint32		hdr = root->header & ~((uint32) JBTL_JBC_TOBJECT_TOASTED);

					memcpy(outp, &hdr, sizeof(hdr));
				}

				/*
				 * Rewrite JEntries. Walk in JEntry order; for each
				 * value JEntry (index N..2N-1 for object) that's an
				 * ISCONTAINER_PTR, replace with ISCONTAINER + new length.
				 */
				out_jentries = (JEntry *) (outp + sizeof(uint32));
				{
					int			val_base = is_object ? N : 0;
					int			next_slot = 0;

					for (i = 0; i < n_jentries; i++)
					{
						JEntry		je = parent_children[i];
						JEntry		new_je;

						if (i >= val_base &&
							next_slot < nslots &&
							slots[next_slot].value_idx == (i - val_base))
						{
							ChildSlot  *slot = &slots[next_slot++];
							int32		child_payload =
								(int32) VARSIZE(slot->child_full) - VARHDRSZ;

							/*
							 * Replace ISCONTAINER_PTR with ISCONTAINER.
							 * Preserve JENTRY_HAS_OFF flag if any.
							 * Length field becomes child's body
							 * payload size.
							 */
							new_je = (je & ~JENTRY_TYPEMASK & ~JENTRY_OFFLENMASK)
								| JENTRY_ISCONTAINER
								| (child_payload & JENTRY_OFFLENMASK);
						}
						else
						{
							new_je = je;
						}
						out_jentries[i] = new_je;
					}
				}

				/*
				 * Copy KVMap + keys + values. We can copy in one shot
				 * up to the first ISCONTAINER_PTR value, then handle
				 * each child slot, then continue.
				 *
				 * Layout reminder for an object:
				 *  [hdr(4)][JEntries(4*2N)][KVMap(opt INTALIGN'd)]
				 *  [keys][values]
				 *
				 * Key area is identical in old and new. Values differ
				 * only at child-slot positions.
				 */
				outp += sizeof(uint32) + n_jentries * sizeof(JEntry);

				/* KVMap + keys: byte-identical block from parent body. */
				{
					int32		hdr_jentries_size =
						(int32) sizeof(uint32) +
						n_jentries * (int32) sizeof(JEntry);
					int32		kvmap_keys_size =
						(data_area_offset - hdr_jentries_size) + key_area_size;

					memcpy(outp,
						   parent_body + hdr_jentries_size,
						   kvmap_keys_size);
					outp += kvmap_keys_size;
				}

				/* Values: walk slot-by-slot with parent-body offsets. */
				cur_off_in_data = key_area_size; /* values start AFTER keys */
				{
					int			val_base = is_object ? N : 0;
					int			next_slot = 0;

					for (k = 0; k < N; k++)
					{
						int32		val_off_within_data =
							(int32) getJsonbOffset(root, val_base + k);
						int32		val_len =
							(int32) getJsonbLength(root, val_base + k);

						/* Catch up bytes between cursor and this value. */
						if (val_off_within_data > cur_off_in_data)
						{
							memcpy(outp,
								   parent_body + data_area_offset + cur_off_in_data,
								   val_off_within_data - cur_off_in_data);
							outp += val_off_within_data - cur_off_in_data;
							cur_off_in_data = val_off_within_data;
						}

						if (next_slot < nslots && slots[next_slot].value_idx == k)
						{
							ChildSlot  *slot = &slots[next_slot++];
							int32		child_payload =
								(int32) VARSIZE(slot->child_full) - VARHDRSZ;

							/* Substitute child body bytes */
							memcpy(outp, VARDATA(slot->child_full), child_payload);
							outp += child_payload;
							cur_off_in_data += val_len;	/* skip old PTR slot */
							pfree(slot->child_full);
						}
						else
						{
							memcpy(outp,
								   parent_body + data_area_offset + cur_off_in_data,
								   val_len);
							outp += val_len;
							cur_off_in_data += val_len;
						}
					}
				}

				pfree(slots);

				/*
				 * Apply caller-requested slicing over assembled body.
				 */
				body_payload = assembled_payload;

				if (length < 0 || (offset == 0 && length >= body_payload))
					return PointerGetDatum(out);
				else
				{
					int32		want_off = offset < 0 ? 0 :
						offset > body_payload ? body_payload : offset;
					int32		want_len = length < 0 ? 0 : length;
					struct varlena *slice;

					if (want_len > body_payload - want_off)
						want_len = body_payload - want_off;

					slice = (struct varlena *) palloc(want_len + VARHDRSZ);
					SET_VARSIZE(slice, want_len + VARHDRSZ);
					memcpy(VARDATA(slice),
						   VARDATA(out) + want_off,
						   want_len);
					pfree(out);
					return PointerGetDatum(slice);
				}
				}	/* close v0/v1 dispatch block */
			}

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("jsonb_toaster_lite: tsr_detoast for mode 0x%08X is not yet implemented",
							mode)));
			return (Datum) 0;	/* keep the compiler happy */
	}
}


/*
 * tsr_delete
 *
 *	If the value is a JBTL_POINTER custom-varlena, unwrap it and
 *	delete the corresponding toast rows. Inline JBTL_PLAIN_JSONB
 *	values have nothing to delete from the toast relation; just
 *	return.
 */
static void
jbtl_delete(ToasterContext tcxt, Datum value, bool is_speculative)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	uint32		mode;

	if (!VARATT_IS_CUSTOM(attr))
	{
		/*
		 * Defensive: core should not invoke us on non-custom varlenas
		 * for our toaster, but handle gracefully.
		 */
		return;
	}

	mode = JBTL_CUSTOM_PTR_GET_HEADER(attr) & JBTL_POINTER_TYPE_MASK;

	switch (mode)
	{
		case JBTL_PLAIN_JSONB:
			/* No toast rows; nothing to delete. */
			return;

		case JBTL_POINTER_SUBTREE:
			{
				/*
				 * version-aware SUBTREE delete dispatch.
				 *
				 *	v0: payload = parent body,
				 *		no header, no refs. Unconditional child
				 *		delete — correct only because v0 rows are
				 *		synthesised one-per-test and never shared.
				 *		Documented as test-only.
				 *
				 *	v1: payload = JbtlSubtreeHeader
				 *		+ parent body. For each child pointer:
				 *		 1. delete the (parent_vid, child_vid) edge
				 *		 in jbtl_subtree_refs
				 *		 2. if remaining refcount on the child is
				 *		 zero, delete the child toast chain
				 *		 3. if remaining > 0, keep the child chain
				 *		 (some other parent still references it)
				 *
				 *	Hard invariant: every v1 SUBTREE row has matching
				 *	refs edges ( production spill enforces
				 *	this; jbtl_subtree_refs_delete_one ereports if
				 *	the edge is missing — surfaces state corruption).
				 */
				char	   *payload = JBTL_CUSTOM_PTR_GET_DATA(attr);
				int32		payload_size = (int32) JBTL_CUSTOM_PTR_GET_DATA_SIZE(attr);
				char	   *parent_body;
				int32		parent_size;
				bool		is_v1 = false;
				Oid			hdr_parent_valueid = InvalidOid;
				Oid			hdr_parent_toastrelid = InvalidOid;
				JsonbContainer *root;
				int			N;
				int			n_jentries;
				int			val_base;
				int32		data_area_offset;
				int			n_kvmap_bytes = 0;
				int			i;

				if (payload_size < (int32) sizeof(uint32))
					return;

				/* Version dispatch — same detection rule as reader. */
				if (payload_size >= (int32) sizeof(JbtlSubtreeHeader) &&
					(uint8) payload[0] == JBTL_SUBTREE_HEADER_V1)
				{
					const JbtlSubtreeHeader *hdr =
						(const JbtlSubtreeHeader *) payload;
					Oid			tcxt_toastrelid;

					is_v1 = true;
					hdr_parent_valueid = hdr->parent_valueid;
					hdr_parent_toastrelid = hdr->parent_toastrelid;
					parent_body = payload + hdr->header_size;
					parent_size = payload_size - hdr->header_size;

					/*
					 * Sanity: the header's parent_toastrelid MUST match
					 * the heap row's reltoastrelid (we wrote it that
					 * way in jbtl_try_spill_subtree). A mismatch means
					 * either:
					 *  - corruption (e.g. ALTER ... ATTACH PARTITION
					 *  that didn't go through the safe-copy path),
					 *  - bug in spill that wrote the wrong oid,
					 *  - some other path bypassed tsr_copy and grafted
					 *  the custom-varlena across heap relations.
					 *
					 * In all cases refs lookup keyed on the header's
					 * value would not match the actually-allocated
					 * edges; better to ereport than to silently leak
					 * or corrupt refcount state.
					 */
					tcxt_toastrelid = tcxt->rel->rd_rel->reltoastrelid;
					if (OidIsValid(tcxt_toastrelid) &&
						hdr_parent_toastrelid != tcxt_toastrelid)
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("jsonb_toaster_lite: SUBTREE v1 header parent_toastrelid=%u does not match relation reltoastrelid=%u",
										hdr_parent_toastrelid,
										tcxt_toastrelid)));
				}
				else
				{
					/* v0 fixture path */
					parent_body = payload;
					parent_size = payload_size;
				}

				if (parent_size < (int32) sizeof(uint32))
					return;

				root = (JsonbContainer *) parent_body;
				N = JsonContainerSize(root);
				n_jentries = JsonContainerIsObject(root) ? 2 * N : N;
				val_base = JsonContainerIsObject(root) ? N : 0;

				if (JsonContainerIsObject(root) && JsonContainerHasKVMap(root))
					n_kvmap_bytes =
						INTALIGN(N * JSONB_KVMAP_ENTRY_SIZE(N));

				data_area_offset =
					(int32) sizeof(uint32) +
					n_jentries * (int32) sizeof(JEntry) +
					n_kvmap_bytes;

				for (i = val_base; i < n_jentries; i++)
				{
					JEntry		je = root->children[i];

					if (JBTL_JBE_ISCONTAINER_PTR(je))
					{
						int32		val_off =
							(int32) getJsonbOffset(root, i);
						const char *payload_addr =
							parent_body + data_area_offset + val_off;
						const JbtlToastedContainerPointer *ptr =
							(const JbtlToastedContainerPointer *) payload_addr;
						char		bare_copy[TOAST_POINTER_SIZE];
						struct varatt_external child_ext;

						memcpy(bare_copy, ptr->data, TOAST_POINTER_SIZE);
						VARATT_EXTERNAL_GET_POINTER(child_ext, ptr->data);

						if (is_v1)
						{
							int		remaining;

							/*
							 * Decrement the (parent, child) edge.
							 * Returns count of OTHER edges still
							 * pointing at this child.
							 */
							remaining = jbtl_subtree_refs_delete_one(
								hdr_parent_toastrelid,
								hdr_parent_valueid,
								child_ext.va_toastrelid,
								child_ext.va_valueid);

							if (remaining == 0)
								jbtl_toast_delete_datum(
									PointerGetDatum(bare_copy),
									is_speculative);
							/* else: keep child; another parent owns it */
						}
						else
						{
							/*
							 * v0 fixture: never admitted to refs,
							 * never shared. Unconditional delete.
							 */
							jbtl_toast_delete_datum(
								PointerGetDatum(bare_copy),
								is_speculative);
						}
					}
				}
				return;
			}

		case JBTL_POINTER:
		case JBTL_POINTER_COMPRESSED_CHUNKS:
		case JBTL_POINTER_DIFF:
		case JBTL_POINTER_DIFF_COMP:
			{
				/*
				 * Build a bare-TOAST varlena from the inline tail and
				 * pass it to jbtl_toast_delete_datum, which expects an
				 * on-disk external. All four pointer modes carry a
				 * varatt_external as their FIRST inline element:
				 *
				 *  POINTER, POINTER_COMPRESSED_CHUNKS:
				 *  [varatt_external]
				 *
				 *  POINTER_DIFF, POINTER_DIFF_COMP:
				 *  [varatt_external] [JbtlPointerDiff inline tail]
				 *
				 * Deletion is the same operation: remove the toast rows
				 * matching the base valueid. The DIFF tail lives only
				 * in the parent tuple and disappears with it; nothing
				 * to delete in pg_toast for the overlay itself.
				 */
				char	   *bare = JBTL_CUSTOM_PTR_GET_DATA(attr);
				char		bare_copy[TOAST_POINTER_SIZE];

				Assert(VARATT_IS_EXTERNAL_ONDISK(bare));
				memcpy(bare_copy, bare, TOAST_POINTER_SIZE);

				jbtl_toast_delete_datum(PointerGetDatum(bare_copy),
										is_speculative);
				return;
			}

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("jsonb_toaster_lite: tsr_delete for mode 0x%08X is not yet implemented",
							mode)));
	}
}


/*
 * Optional callbacks still stubbed.
 *
 *	tsr_copy will be wired alongside the per-field-toasting writer in
 *	a later milestone. tsr_update is the partial-rewrite fast path
 *	(diff/append) -- deferred.
 */
/*
 * jbtl_copy — tsr_copy entry point.
 *
 *	Triggered by toastapi_copy when a CUSTOM varlena is being copied
 *	into a new heap row that keeps the same toaster id (e.g.
 *	INSERT INTO ... SELECT FROM, CTAS, ALTER TABLE ... SET TYPE that
 *	preserves attoptions). The caller's contract: returning Datum 0
 *	tells core to detoast the value and re-toast it through the
 *	normal tsr_toast path.
 *
 *	 copy-safety guard (per @yoda directive):
 *
 *	 - SUBTREE v1: a copy that preserved the custom-pointer
 *	 verbatim would inherit the OLD parent_valueid AND the OLD
 *	 refs edges — but the NEW heap row needs ITS OWN parent
 *	 identity and ITS OWN refs edges. Returning Datum 0 here
 *	 forces core to detoast the SUBTREE row to vanilla jsonb;
 *	 when core re-toasts via tsr_toast, the spill path
 *	 re-allocates a fresh parent_valueid and writes new refs
 *	 edges for the new heap row. Refcount semantics stay
 *	 correct.
 *
 *	 - SUBTREE v0: also Datum 0 — the same
 *	 detoast/retoast path is safe, since v0 rows hold no refs
 *	 state to preserve.
 *
 *	 - POINTER / POINTER_COMPRESSED_CHUNKS / DIFF / etc.: same
 *	 Datum 0 path. No refs to track; detoast/retoast through
 *	 tsr_toast emits a fresh chain for the new row.
 *
 *	 - PLAIN_JSONB inline: same Datum 0 path, no chain involved.
 *
 *	Net effect: every SUBTREE v1 copy goes through detoast →
 *	retoast. When the destination column has enable_subtree_storage
 *	on, retoast re-spills into a fresh SUBTREE v1 with new
 *	parent_valueid and one fresh refs edge per spilled child. When
 *	the destination has it off, retoast emits JBTL_POINTER chunks.
 *	Either way refs remain consistent.
 *
 *	Trade-off: detoast+retoast is expensive (assembled body up to
 *	tens of MB) but correctness wins over efficiency.
 *	 may add a refs-aware shallow copy (increment edge for
 *	the new parent's parent_valueid) once update reuse semantics
 * are stable.
 */
static Datum
jbtl_copy(ToasterContext tcxt, Datum value, int am_options)
{
	(void) tcxt;
	(void) value;
	(void) am_options;

	/*
	 * Return 0 to signal core: detoast the value and re-toast it
	 * via the normal tsr_toast path. This is correct for every
	 * mode (PLAIN, POINTER, POINTER_*, DIFF*, SUBTREE v0, SUBTREE
	 * v1). See banner above for the SUBTREE-specific rationale.
	 */
	return (Datum) 0;
}

/*
 * jbtl_update — tsr_update entry point.
 *
 *	Reachable via the β bridge in core toast_helper.c when old is a
 *	JBTL custom-pointer and new is a regular in-memory varlena (the
 *	typical jsonb_set output in master), or via the standard
 *	both-CUSTOM path.
 *
 *	Algorithm (same-length top-level scalar; v1 + v2a rebase):
 *	 1. Decline (return Datum 0) for everything except the narrow
 *	 happy path:
 *	 - old must be JBTL_POINTER, JBTL_POINTER_COMPRESSED_CHUNKS, or
 *	 JBTL_POINTER_DIFF (v2a rebase: re-emit a fresh DIFF over the
 *	 SAME embedded base, discarding the old inline diff).
 *	 JBTL_POINTER_DIFF_COMP remains fallback in this snapshot.
 *	 - new must be a regular jsonb varlena (Datum holding a
 *	 Jsonb pointer).
 *	 - Both bodies must be same total byte length (the BASE
 *	 va_rawsize == the new value).
 *	 - Diff between base body and new body must be exactly one
 *	 contiguous byte range, and that range must lie entirely
 *	 within the value-data area of the root container (i.e. the
 *	 change affects only one same-length scalar value at the top
 *	 level — no JEntry/key area changes).
 *	 2. If all conditions hold, build a JBTL_POINTER_DIFF (or
 *	 JBTL_POINTER_DIFF_COMP for compressed base) custom-pointer
 *	 wrapping the unchanged base varatt_external + inline diff.
 *	 For old=DIFF, the new diff is computed against the ORIGINAL
 *	 base body (chunks at the embedded valueid), NOT against the
 *	 current materialized value with the old diff applied.
 *	 3. Otherwise return Datum 0; core falls back to the standard
 *	 delete+toast (rebase) path.
 *
 *	Safety boundaries (out of scope for this milestone):
 *	 - length-changing scalar replacement → declines (different
 *	 body length detected at step 1)
 *	 - array element replacement → declines (would not produce a
 *	 single contiguous byte-range diff, or would also disturb
 *	 JEntry-area bytes)
 *	 - nested path → declines (similar)
 *	 - stacked DIFF → impossible by construction (rebase always
 *	 emits a single fresh diff against the original base)
 *	 - DIFF_COMP-on-DIFF_COMP rebase → declines (deferred)
 *
 *	The diagnostic counter `jbtl_update_call_count` records every
 *	call; tests assert it.
 */
static int jbtl_update_call_count = 0;
static int jbtl_update_diff_emitted_count = 0;

/*
 * Diagnostic counters for the M9.2 narrow SUBTREE reuse path.
 *
 *	subtree_reuse_attempts — how many times jbtl_update entered the
 *	  SUBTREE branch (i.e. old_mode == JBTL_POINTER_SUBTREE and
 *	  new_value is plain).
 *
 *	subtree_reuse_success — how many of those returned a non-zero
 *	  Datum (reuse path took effect).  attempts - success = number of
 *	  declines that fell through to the standard detoast+retoast.
 *
 *	subtree_children_reused — total number of SUBTREE children whose
 *	  on-disk chain was preserved across an UPDATE.  Sums across all
 *	  successful reuses.  Tests assert this is > 0 for T1/T2 and
 *	  unchanged for T3.
 */
static int jbtl_update_subtree_reuse_attempts = 0;
static int jbtl_update_subtree_reuse_success = 0;
static int jbtl_update_subtree_children_reused = 0;

/*
 * jbtl_update_subtree_reuse — narrow M-B v0 sub-object reuse.
 *
 *	Triggered from jbtl_update when:
 *	  - old_mode == JBTL_POINTER_SUBTREE (v1; v0 fixture is declined)
 *	  - new_v is a plain (non-CUSTOM, non-compressed, non-EXTERNAL)
 *	    jsonb varlena
 *
 *	Conditions for reuse (all must hold; any miss → return Datum 0
 *	to fall back to the standard detoast+retoast):
 *	  1. Root in old is a jsonb object without KVMap (the spill
 *	     writer never emits KVMap; v1 SUBTREE rows are guaranteed
 *	     KVMap-free).
 *	  2. Root in new is a jsonb object.
 *	  3. Same N keys in same JEntry order, byte-identical key area.
 *	     (Strict shape match — any key add/remove/reorder declines.)
 *	  4. For every value slot k that is ISCONTAINER_PTR in old, the
 *	     corresponding logical sub-value in new is byte-identical to
 *	     the assembled child body fetched from the old SUBTREE child
 *	     chain.  If even one such slot's contents differ, decline
 *	     (the entire row needs a fresh write to avoid mixing reused
 *	     and freshly-written children with inconsistent edges).
 *	  5. For non-ISCONTAINER_PTR value slots: simply copy the new
 *	     inline bytes into the new body (this is where the value
 *	     change being applied actually lives).
 *
 *	On success:
 *	  - allocate a new parent_valueid via
 *	    jbtl_alloc_subtree_parent_valueid
 *	  - insert one (new_parent_vid → child_vid) edge per reused child
 *	    BEFORE returning, so that when jbtl_update's caller (or our
 *	    own delete path) runs jbtl_delete on the old SUBTREE varlena,
 *	    jbtl_subtree_refs_delete_one observes remaining > 0 on each
 *	    reused child and preserves the child toast chain
 *	  - construct a new SUBTREE v1 body with the SAME embedded
 *	    JbtlToastedContainerPointer bytes at reused slots and the
 *	    new inline bytes at non-SUBTREE slots
 *	  - wrap via jbtl_toast_make_pointer_subtree_v1
 *	  - delete the old SUBTREE row by calling jbtl_delete on old_v
 *	    BEFORE returning (because jbtl_update's standard "approve,
 *	    delete old, keep new" path on the CUSTOM-return branch
 *	    deletes the old TOAST chain via jbtl_toast_delete_datum on
 *	    the inline varatt_external bytes — that is correct for
 *	    POINTER-family modes whose data area starts with a TOAST
 *	    pointer, but NOT for SUBTREE whose data area starts with
 *	    JbtlSubtreeHeader.  So we own the delete for the SUBTREE
 *	    case and the caller must NOT run the POINTER-style delete.)
 *
 *	Returns the new CUSTOM Datum on success, or Datum 0 to decline.
 */
static Datum
jbtl_update_subtree_reuse(ToasterContext tcxt,
						  struct varlena *new_v,
						  struct varlena *old_v)
{
	char	   *old_payload;
	int32		old_payload_size;
	JbtlSubtreeHeader old_hdr;
	const char *old_parent_body;
	int32		old_parent_size;
	JsonbContainer *old_root;
	JsonbContainer *new_root;
	int			N;
	int			n_jentries;
	int			val_base;
	int32		old_data_area_offset;
	int32		new_data_area_offset;
	int32		key_area_size;
	JEntry	   *old_jentries;
	JEntry	   *new_jentries;
	int			k;
	int			n_reused = 0;
	struct
	{
		int			value_idx;	/* k in 0..N-1 */
		const char *ptr_payload_addr;	/* JbtlToastedContainerPointer in old body */
		struct varatt_external child_ext;
		struct varlena *child_full;	/* assembled body for comparison */
	}		   *reused_slots = NULL;

	jbtl_update_subtree_reuse_attempts++;

	/*
	 * GUC gate: if the user has explicitly turned SUBTREE storage off
	 * (e.g. as part of the "force re-toast out of SUBTREE" recovery
	 * recipe documented by the VACUUM FULL gate), we MUST decline.
	 * Otherwise the reuse path would silently keep the row in SUBTREE
	 * format across what the user intended as a normalization UPDATE,
	 * leaving the VACUUM FULL gate permanently active for that row.
	 *
	 *	This is a correctness requirement, not an optimization.  The
	 *	GUC's contract is "do not produce new SUBTREE rows"; emitting
	 *	a new SUBTREE root (even one that reuses old children) is
	 *	still emitting a SUBTREE row.
	 */
	if (!jbtl_enable_subtree_storage)
		return (Datum) 0;

	/*
	 * Pre-decline guards on new_v (the plain-jsonb sanity checks are
	 * also applied by the caller, but repeating them here makes the
	 * helper safe to call standalone).
	 */
	if (VARATT_IS_CUSTOM(new_v) ||
		VARATT_IS_COMPRESSED(new_v) ||
		VARATT_IS_SHORT(new_v) ||
		VARATT_IS_EXTERNAL(new_v))
		return (Datum) 0;

	/*
	 * Parse old SUBTREE varlena.  We require v1; v0 fixture rows are
	 * not refs-tracked and are out of scope here.
	 */
	old_payload = JBTL_CUSTOM_PTR_GET_DATA(old_v);
	old_payload_size = (int32) JBTL_CUSTOM_PTR_GET_DATA_SIZE(old_v);
	if (old_payload_size < (int32) sizeof(JbtlSubtreeHeader))
		return (Datum) 0;
	if ((uint8) old_payload[0] != JBTL_SUBTREE_HEADER_V1)
		return (Datum) 0;	/* v0: skip */
	memcpy(&old_hdr, old_payload, sizeof(old_hdr));
	if (old_hdr.header_size != sizeof(JbtlSubtreeHeader))
		return (Datum) 0;
	old_parent_body = old_payload + old_hdr.header_size;
	old_parent_size = old_payload_size - old_hdr.header_size;

	if (old_parent_size < (int32) sizeof(uint32))
		return (Datum) 0;

	old_root = (JsonbContainer *) old_parent_body;
	if (!JsonContainerIsObject(old_root))
		return (Datum) 0;
	if (JsonContainerHasKVMap(old_root))
		return (Datum) 0;	/* writer never emits KVMap; defensive */

	N = JsonContainerSize(old_root);
	if (N == 0)
		return (Datum) 0;
	n_jentries = 2 * N;
	val_base = N;

	/*
	 * Parse new_v root.
	 */
	{
		int32 new_payload = (int32) VARSIZE(new_v) - VARHDRSZ;

		if (new_payload < (int32) sizeof(uint32))
			return (Datum) 0;
		new_root = (JsonbContainer *) VARDATA(new_v);
		if (!JsonContainerIsObject(new_root))
			return (Datum) 0;
		if (JsonContainerHasKVMap(new_root))
			return (Datum) 0;
		if (JsonContainerSize(new_root) != N)
			return (Datum) 0;	/* shape change */
	}

	/* Data area offsets (no KVMap in either old or new). */
	old_data_area_offset =
		(int32) sizeof(uint32) + n_jentries * (int32) sizeof(JEntry);
	new_data_area_offset = old_data_area_offset;	/* same N → same layout */

	old_jentries = old_root->children;
	new_jentries = new_root->children;

	/*
	 * Key area must be byte-identical AND key-JEntry array must match
	 * exactly.  This pins same key set and same order; lengths and
	 * offset-cache bits must agree.
	 */
	key_area_size = 0;
	for (k = 0; k < N; k++)
	{
		if (old_jentries[k] != new_jentries[k])
			return (Datum) 0;
		key_area_size += (int32) getJsonbLength(old_root, k);
	}
	if (memcmp((const char *) old_root + old_data_area_offset,
			   (const char *) new_root + new_data_area_offset,
			   key_area_size) != 0)
		return (Datum) 0;

	/*
	 * Reject offset-cache flags on value JEntries in both old and new.
	 * The writer's spill path rejects them too (jsonb_toaster_lite.c:656),
	 * and getJsonbOffset()/getJsonbLength() semantics differ between
	 * direct-length and cached-offset entries; the new body we emit
	 * MUST use direct lengths everywhere, so any HAS_OFF in new also
	 * forces decline.
	 */
	for (k = val_base + 1; k < n_jentries; k++)
		if ((old_jentries[k] & JENTRY_HAS_OFF) ||
			(new_jentries[k] & JENTRY_HAS_OFF))
			return (Datum) 0;

	/*
	 * Pass 1: scan old value JEntries, identify ISCONTAINER_PTR
	 * slots, fetch each child body fully for the byte-equality
	 * check.  Allocate temp arrays sized to N (worst case all slots
	 * are children).
	 */
	reused_slots = palloc(N * sizeof(*reused_slots));

	{
		for (k = 0; k < N; k++)
		{
			JEntry old_je = old_jentries[val_base + k];

			if (JBTL_JBE_ISCONTAINER_PTR(old_je))
			{
				/* Old has SUBTREE child at slot k. */
				int32		val_off =
					(int32) getJsonbOffset(old_root, val_base + k);
				const char *ptr_addr =
					old_parent_body + old_data_area_offset + val_off;
				const JbtlToastedContainerPointer *ptr =
					(const JbtlToastedContainerPointer *) ptr_addr;
				struct varatt_external ext;
				struct varlena *child_full;
				JEntry		new_je = new_jentries[val_base + k];
				const char *new_value_addr;
				int32		new_value_len;
				int32		child_payload;

				/*
				 * In new, the same slot must be a container value of
				 * the same kind (object/array) as the child body's
				 * stored header.  Otherwise decline.
				 */
				if (!JBE_ISCONTAINER(new_je))
				{
					int j;
					for (j = 0; j < n_reused; j++)
						pfree(reused_slots[j].child_full);
					pfree(reused_slots);
					return (Datum) 0;
				}

				VARATT_EXTERNAL_GET_POINTER(ext, ptr->data);
				if (!VARATT_EXTERNAL_GET_EXTSIZE(ext))
				{
					int j;
					for (j = 0; j < n_reused; j++)
						pfree(reused_slots[j].child_full);
					pfree(reused_slots);
					return (Datum) 0;
				}

				/* Fetch child body fully. */
				child_full = jbtl_toast_fetch_full_plain(&ext, NULL);
				child_payload = (int32) VARSIZE(child_full) - VARHDRSZ;

				/*
				 * Locate new's value k within its data area.  For
				 * objects, value JEntries' offsets are RELATIVE TO
				 * THE DATA AREA (which begins with keys).
				 */
				{
					int32 new_v_off =
						(int32) getJsonbOffset(new_root, val_base + k);
					new_value_addr =
						(const char *) new_root + new_data_area_offset + new_v_off;
					new_value_len = (int32) (new_je & JENTRY_OFFLENMASK);
				}

				if (child_payload != new_value_len ||
					memcmp(VARDATA(child_full), new_value_addr, new_value_len) != 0)
				{
					/* Child body changed; decline whole row. */
					int j;
					pfree(child_full);
					for (j = 0; j < n_reused; j++)
						pfree(reused_slots[j].child_full);
					pfree(reused_slots);
					return (Datum) 0;
				}

				/*
				 * Reuse candidate confirmed.  Save everything we need
				 * to embed the same pointer into the new body.
				 */
				reused_slots[n_reused].value_idx = k;
				reused_slots[n_reused].ptr_payload_addr = ptr_addr;
				reused_slots[n_reused].child_ext = ext;
				reused_slots[n_reused].child_full = child_full;
				n_reused++;
			}
			else
			{
				/*
				 * Old slot k is an inline value.  We will copy the
				 * new inline bytes for this slot into the new body.
				 * Both old and new must agree on container-ness; if
				 * old is non-container and new is container, the new
				 * value's bytes are larger and the new body needs a
				 * different shape.  Accept only matching kinds.
				 */
				JEntry new_je = new_jentries[val_base + k];

				if (JBE_ISCONTAINER(old_je) != JBE_ISCONTAINER(new_je))
				{
					int j;
					for (j = 0; j < n_reused; j++)
						pfree(reused_slots[j].child_full);
					pfree(reused_slots);
					return (Datum) 0;
				}
			}
		}
	}

	/*
	 * If no SUBTREE children were reused, this path adds no value
	 * over the standard detoast+retoast (which would emit a fresh
	 * SUBTREE if GUC says so, or a POINTER otherwise).  Decline and
	 * let the fallback do its work.
	 */
	if (n_reused == 0)
	{
		pfree(reused_slots);
		return (Datum) 0;
	}

	/*
	 * All conditions met.  Build the new SUBTREE row.
	 *
	 *	Layout of the new parent body is the SAME as the old's:
	 *	  [4-byte header][2N JEntries][key area][value area]
	 *	with ISCONTAINER_PTR slots carrying JbtlToastedContainerPointer
	 *	(4 bytes JEntry header + TOAST_POINTER_SIZE bytes).
	 *
	 *	The JEntry array we emit:
	 *	  - keys: copy old's key JEntries (already byte-equal to new's)
	 *	  - values: for ISCONTAINER_PTR slots, write
	 *	    JBTL_JENTRY_ISCONTAINER_PTR | payload_size
	 *	    (same as old's value JEntry).  For inline slots, take new's
	 *	    JEntry as-is.
	 *
	 *	Key area: byte-identical between old and new; we copy old's.
	 *
	 *	Value area: at each ISCONTAINER_PTR slot copy the same
	 *	JbtlToastedContainerPointer bytes from old; at each inline slot
	 *	copy the new value bytes.
	 */
	{
		int32		new_body_size;
		char	   *new_body;
		JEntry	   *out_jentries;
		int32		old_value_payload_size = (int32) sizeof(JEntry) + TOAST_POINTER_SIZE;
		int32		new_value_area_offset;
		int32		new_v_data_area_offset =
			(int32) sizeof(uint32) + n_jentries * (int32) sizeof(JEntry);
		const char *new_v_data_area =
			(const char *) new_root + new_v_data_area_offset;
		Oid			toastrelid = old_hdr.parent_toastrelid;
		Relation	toastrel;
		Relation	toastidx;
		List	   *idxlist;
		Oid			new_parent_valueid;
		struct varlena *result;
		int			ri;

		/*
		 * Total new body size = headers + same key area + reused
		 * pointer payloads + inline value bytes.
		 */
		new_body_size = old_data_area_offset + key_area_size;
		new_body_size += n_reused * old_value_payload_size;
		/* Inline slots: sum lengths from new's JEntries. */
		for (k = 0; k < N; k++)
		{
			JEntry old_je = old_jentries[val_base + k];
			JEntry new_je = new_jentries[val_base + k];

			if (!JBTL_JBE_ISCONTAINER_PTR(old_je))
				new_body_size += (int32) (new_je & JENTRY_OFFLENMASK);
		}

		new_body = palloc(new_body_size);

		/* Header: copy old's (already has JBTL_JBC_TOBJECT_TOASTED hint). */
		memcpy(new_body, &old_root->header, sizeof(uint32));

		out_jentries = (JEntry *) (new_body + sizeof(uint32));

		/* Key JEntries: byte-equal to new's; copy from old. */
		memcpy(out_jentries, old_jentries, N * sizeof(JEntry));

		/* Value JEntries. */
		for (k = 0; k < N; k++)
		{
			JEntry old_je = old_jentries[val_base + k];
			JEntry new_je = new_jentries[val_base + k];

			if (JBTL_JBE_ISCONTAINER_PTR(old_je))
				out_jentries[val_base + k] =
					JBTL_JENTRY_ISCONTAINER_PTR |
					(old_value_payload_size & JENTRY_OFFLENMASK);
			else
				out_jentries[val_base + k] = new_je;
		}

		/* Key area: byte-identical, copy from old. */
		memcpy(new_body + old_data_area_offset,
			   old_parent_body + old_data_area_offset,
			   key_area_size);

		/* Value area. */
		new_value_area_offset = old_data_area_offset + key_area_size;
		{
			int32 dst_pos = new_value_area_offset;
			int   next_reused = 0;

			for (k = 0; k < N; k++)
			{
				JEntry old_je = old_jentries[val_base + k];

				if (JBTL_JBE_ISCONTAINER_PTR(old_je))
				{
					/* Copy the SAME JbtlToastedContainerPointer bytes from old. */
					Assert(next_reused < n_reused &&
						   reused_slots[next_reused].value_idx == k);
					memcpy(new_body + dst_pos,
						   reused_slots[next_reused].ptr_payload_addr,
						   old_value_payload_size);
					dst_pos += old_value_payload_size;
					next_reused++;
				}
				else
				{
					JEntry new_je = new_jentries[val_base + k];
					int32 nlen = (int32) (new_je & JENTRY_OFFLENMASK);
					int32 new_v_off =
						(int32) getJsonbOffset(new_root, val_base + k);

					memcpy(new_body + dst_pos,
						   new_v_data_area + new_v_off,
						   nlen);
					dst_pos += nlen;
				}
			}
			Assert(dst_pos == new_body_size);
		}

		/*
		 * Allocate new parent_valueid and insert refs edges BEFORE
		 * returning.  Ordering is critical: when jbtl_update calls
		 * jbtl_delete on the old SUBTREE varlena, the v1 delete
		 * dispatch will call jbtl_subtree_refs_delete_one for each
		 * old (old_parent_vid, child_vid) edge; with our new
		 * (new_parent_vid, child_vid) edges already in the catalog,
		 * remaining > 0 on every reused child and its toast chain is
		 * preserved.
		 */
		toastrel = table_open(toastrelid, RowExclusiveLock);
		idxlist = RelationGetIndexList(toastrel);
		if (idxlist == NIL)
		{
			int j;
			table_close(toastrel, RowExclusiveLock);
			for (j = 0; j < n_reused; j++)
				pfree(reused_slots[j].child_full);
			pfree(reused_slots);
			pfree(new_body);
			return (Datum) 0;
		}
		toastidx = index_open(linitial_oid(idxlist), RowExclusiveLock);
		new_parent_valueid =
			jbtl_alloc_subtree_parent_valueid(toastrel, toastidx);
		index_close(toastidx, RowExclusiveLock);
		table_close(toastrel, RowExclusiveLock);
		list_free(idxlist);

		for (ri = 0; ri < n_reused; ri++)
		{
			jbtl_subtree_refs_insert(toastrelid, new_parent_valueid,
									 reused_slots[ri].child_ext.va_toastrelid,
									 reused_slots[ri].child_ext.va_valueid);
		}

		/* Wrap in v1 SUBTREE custom-varlena. */
		result = jbtl_toast_make_pointer_subtree_v1(tcxt->toasterid,
													new_parent_valueid,
													toastrelid,
													new_body, new_body_size);

		/*
		 * Delete the old SUBTREE row's edges and chain via the
		 * standard SUBTREE-aware delete path.  Each old edge
		 * (old_parent_vid, child_vid) for a reused child decrements
		 * but does not orphan the child (remaining > 0 thanks to
		 * the new edge inserted above).  For any old child that is
		 * NOT in our reused set (impossible in v0 because we require
		 * full child-set coverage — every old ISCONTAINER_PTR has a
		 * matching new byte-equal slot), the child chain would be
		 * deleted.  That path is exercised by tests where the user
		 * changes a child: byte-identity fails, we decline, and the
		 * standard detoast+retoast handles the row.
		 */
		jbtl_delete(tcxt, PointerGetDatum(old_v), false /* not speculative */);

		/* Cleanup. */
		{
			int j;
			for (j = 0; j < n_reused; j++)
				pfree(reused_slots[j].child_full);
		}
		pfree(reused_slots);
		pfree(new_body);

		jbtl_update_subtree_reuse_success++;
		jbtl_update_subtree_children_reused += n_reused;

		return PointerGetDatum(result);
	}
}

static Datum
jbtl_update(ToasterContext tcxt, Datum new_value, Datum old_value,
			int am_options)
{
	struct varlena *new_v = (struct varlena *) DatumGetPointer(new_value);
	struct varlena *old_v = (struct varlena *) DatumGetPointer(old_value);
	uint32		old_mode;
	bool		base_compressed;
	struct varatt_external base_ext;
	char	   *old_bare;
	struct varlena *base_full;
	char	   *base_body;
	int32		base_payload;
	char	   *new_body;
	int32		new_payload;
	int32		i;
	int32		diff_lo = -1;
	int32		diff_hi = -1;
	struct varlena *result;

	/*
	 * Byte-identical no-op short-circuit (must run BEFORE the diagnostic
	 * counter and BEFORE any chain manipulation).
	 *
	 * The β bridge fires Toastapi_update_hook for any UPDATE where old
	 * is CUSTOM, including UPDATEs that do not change the jsonb column at
	 * all ("UPDATE t SET id = id") or set it byte-equal ("SET jb = jb").
	 * In those cases core hands us new_v == old_v at the bytes level: the
	 * heap row's new tuple still carries the same JBTL_POINTER varlena.
	 *
	 * The full "approve, delete old, keep new" path below would delete a
	 * toast chain that the heap row still references — a lifecycle
	 * violation that surfaces on the next read as "tuple concurrently
	 * deleted". Detect this case first and return new unchanged: no
	 * chain delete, no DIFF emission, no refs touch, no counter bump
	 * (the test pins jbtl_update_calls() == 0 for these scenarios).
	 */
	if (VARATT_IS_CUSTOM(new_v) &&
		VARSIZE_ANY(new_v) == VARSIZE_ANY(old_v) &&
		memcmp((char *) new_v, (char *) old_v, VARSIZE_ANY(old_v)) == 0)
		return new_value;

	jbtl_update_call_count++;

	/* Old must be a JBTL custom-pointer; the β bridge guarantees this. */
	Assert(VARATT_IS_CUSTOM(old_v));
	old_mode = JBTL_CUSTOM_PTR_GET_HEADER(old_v) & JBTL_POINTER_TYPE_MASK;

	/*
	 * Old-mode dispatch.
	 *
	 * SUBTREE branch (M9.2 narrow M-B v0): if old is SUBTREE and new
	 * is plain, attempt sub-object reuse.  On success we own the
	 * delete (jbtl_update_subtree_reuse calls jbtl_delete on old_v
	 * with the SUBTREE-aware cascade) and return a fresh CUSTOM
	 * varlena.  On decline (return Datum 0) fall through to the
	 * standard detoast+retoast path that the existing whitelist
	 * below would have taken.
	 */
	if (old_mode == JBTL_POINTER_SUBTREE)
	{
		if (!VARATT_IS_CUSTOM(new_v))
		{
			Datum reused = jbtl_update_subtree_reuse(tcxt, new_v, old_v);

			if (reused != (Datum) 0)
			{
				jbtl_update_call_count++;
				return reused;
			}
		}
		/*
		 * Fall through to decline.  Core's standard fallback then
		 * runs: detoast(old) → tsr_toast(new) → tsr_delete(old).
		 * Our tsr_delete (jbtl_delete) is SUBTREE-aware (version
		 * dispatch + refs cascade), so the old chain is cleaned
		 * correctly regardless of which mode the retoast emits.
		 * The retoast will reconstruct a fresh SUBTREE (if GUC
		 * enabled) with new child chains and new edges.
		 */
		return (Datum) 0;
	}

	/*
	 * Old-mode whitelist for v2a (POINTER-family modes).
	 *
	 * Eligible:
	 *   JBTL_POINTER                     v1 happy path
	 *   JBTL_POINTER_COMPRESSED_CHUNKS   v1 happy path (compressed base)
	 *   JBTL_POINTER_DIFF                v2a rebase
	 *
	 * Decline:
	 *   JBTL_POINTER_DIFF_COMP           v2a deferred
	 *   JBTL_PLAIN_JSONB                 inline body; nothing to share.
	 *   any other / future mode tag      out of scope.
	 */
	if (old_mode != JBTL_POINTER &&
		old_mode != JBTL_POINTER_COMPRESSED_CHUNKS &&
		old_mode != JBTL_POINTER_DIFF)
		return (Datum) 0;

	base_compressed = (old_mode == JBTL_POINTER_COMPRESSED_CHUNKS);

	/* Extract the base varatt_external. */
	old_bare = JBTL_CUSTOM_PTR_GET_DATA(old_v);
	if (!VARATT_IS_EXTERNAL_ONDISK(old_bare))
		return (Datum) 0;
	VARATT_EXTERNAL_GET_POINTER(base_ext, old_bare);

	/*
	 * New must be a plain (non-CUSTOM) varlena. When new IS already
	 * CUSTOM (e.g. SUBTREE constructed by the test fixture, or
	 * any future custom-pointer mode), the hook is contracted to
	 * "approve, delete old, and keep new as-is". We must:
	 *
	 *  1. Delete the old toast chain so it doesn't leak. Core's
	 *  NEEDS_DELETE_OLD path only fires when this hook returns 0
	 *  (decline), not on the keep-new branch — so we own delete.
	 *  2. Return new unchanged so core's heap_update flow does not
	 *  detoast new and then re-toast it (which would defeat the
	 *  SUBTREE / DIFF / etc. structure and write an extra orphan
	 *  chain).
	 *
	 * Rationale: toast_helper's update path sets `need_detoast=false`
	 * only after a non-zero return from this hook. If we returned 0
	 * (decline), line 278 of toast_helper.c would call
	 * detoast_external_attr on our CUSTOM value and then re-toast its
	 * assembled body via tsr_toast — defeating the structure. But
	 * the same non-zero return skips the NEEDS_DELETE_OLD marking, so
	 * we have to delete old ourselves here.
	 */
	if (VARATT_IS_CUSTOM(new_v))
	{
		/*
		 * old_bare points to the inline varatt_external bytes inside
		 * the JBTL_POINTER custom-varlena. jbtl_toast_delete_datum
		 * expects a full on-disk varlena (2-byte external header +
		 * 18-byte varatt_external). Build a local copy.
		 */
		char		old_full[TOAST_POINTER_SIZE];

		Assert(VARATT_IS_EXTERNAL_ONDISK(old_bare));
		memcpy(old_full, old_bare, TOAST_POINTER_SIZE);
		jbtl_toast_delete_datum(PointerGetDatum(old_full), false);

		return new_value;
	}

	/*
	 * The β bridge gates on !VARATT_IS_EXTERNAL_ONDISK(new_v) too,
	 * so by the time we get here new is a fresh in-memory varlena.
	 * It might be compressed — for now decline so we don't have to
	 * decompress in the writer; tsr_toast does the right thing on
	 * fallback.
	 */
	if (VARATT_IS_COMPRESSED(new_v) || VARATT_IS_SHORT(new_v) ||
		VARATT_IS_EXTERNAL(new_v))
		return (Datum) 0;

	/*
	 * Detoast the BASE body (without applying any future overlay —
	 * we're computing the overlay here). Use the appropriate reader
	 * for the base mode.
	 */
	if (base_compressed)
	{
		int32		base_attrsize = VARATT_EXTERNAL_GET_EXTSIZE(base_ext);
		int32		base_decoded_payload =
			base_attrsize > VARHDRSZ ? base_attrsize - VARHDRSZ : base_attrsize;

		base_full = jbtl_toast_fetch_compressed_chunks(&base_ext,
													   0,
													   base_decoded_payload,
													   NULL, NULL,
													   NULL, NULL,
													   NULL);
	}
	else
	{
		base_full = jbtl_toast_fetch_full_plain(&base_ext, NULL);
	}

	base_body = VARDATA(base_full);
	base_payload = (int32) VARSIZE(base_full) - VARHDRSZ;

	new_body = VARDATA(new_v);
	new_payload = (int32) VARSIZE(new_v) - VARHDRSZ;

	/*
	 * Length-changing → out of scope. This single check rules out
	 * any update where the new body has a different total byte
	 * length than the base; this includes length-changing scalar
	 * replacement, structural changes, and most container edits.
	 */
	if (base_payload != new_payload)
	{
		pfree(base_full);
		return (Datum) 0;
	}

	/*
	 * Find the first and last differing byte. If there is no
	 * difference at all, decline — tsr_update cannot be called for
	 * byte-identical updates (core's case 2 already handles them),
	 * but be defensive.
	 *
	 * If there is a difference, [diff_lo, diff_hi] gives the
	 * contiguous span we need to overlay.
	 */
	for (i = 0; i < base_payload; i++)
	{
		if (base_body[i] != new_body[i])
		{
			diff_lo = i;
			break;
		}
	}

	if (diff_lo < 0)
	{
		/* No difference at all; decline. */
		pfree(base_full);
		return (Datum) 0;
	}

	for (i = base_payload - 1; i >= diff_lo; i--)
	{
		if (base_body[i] != new_body[i])
		{
			diff_hi = i;
			break;
		}
	}

	Assert(diff_hi >= diff_lo);

	/*
	 * Top-level scalar bound: the diff range must fall ENTIRELY
	 * inside ONE non-container top-level value of the root object.
	 *
	 * Layout for an object root:
	 *  [4B header] [N JEntries for keys] [N JEntries for values]
	 *  [optional KVMap, INTALIGN'd] [key area] [value data area]
	 *
	 * For each value index k (0..N-1):
	 *  value_offset[k] = value_area_offset + getJsonbOffset(root, N+k)
	 *  value_length[k] = getJsonbLength(root, N+k)
	 *  value_jentry[k] = root->children[N+k]
	 *
	 * Accept the update only when:
	 *  - both diff_lo and diff_hi fall inside the same value k
	 *  - that value's JEntry is NOT a container (so changes are
	 *  contained within a top-level scalar)
	 *
	 * Anything that touches the header, JEntries, KVMap, key area,
	 * or crosses a value boundary, or lands inside a binary
	 * container value (array/object), is out of scope for
	 * and we decline.
	 */
	{
		JsonbContainer *root = (JsonbContainer *) base_body;
		int			N;
		bool		has_kvmap;
		int			n_jentries;
		int			kvmap_entry_size;
		int32		data_area_offset;
		int32		key_area_size = 0;
		int32		value_area_offset;
		int			k;
		bool		accepted = false;

		if (base_payload < (int32) sizeof(uint32) ||
			!JsonContainerIsObject(root))
		{
			pfree(base_full);
			return (Datum) 0;
		}

		N = JsonContainerSize(root);
		has_kvmap = JsonContainerHasKVMap(root);
		n_jentries = 2 * N;
		kvmap_entry_size = has_kvmap ? JSONB_KVMAP_ENTRY_SIZE(N) : 0;
		data_area_offset =
			(int32) sizeof(uint32) +
			n_jentries * (int32) sizeof(JEntry) +
			INTALIGN(N * kvmap_entry_size);

		for (k = 0; k < N; k++)
			key_area_size += (int32) getJsonbLength(root, k);

		value_area_offset = data_area_offset + key_area_size;

		if (diff_lo < value_area_offset)
		{
			pfree(base_full);
			return (Datum) 0;
		}

		/*
		 * Find which top-level value index contains diff_lo.
		 * Iterate values in JEntry order (k = 0..N-1) and accept
		 * only if [diff_lo, diff_hi] fits inside one non-container
		 * value range.
		 *
		 * getJsonbOffset(root, N+k) returns the offset of the k-th
		 * value RELATIVE TO THE START OF THE DATA AREA (which
		 * starts at data_area_offset and contains keys followed by
		 * values). So absolute offset of value k within the body
		 * is data_area_offset + getJsonbOffset(root, N+k).
		 */
		for (k = 0; k < N; k++)
		{
			int32	off = (int32) getJsonbOffset(root, N + k);
			int32	len = (int32) getJsonbLength(root, N + k);
			int32	v_lo = data_area_offset + off;
			int32	v_hi = v_lo + len - 1;

			if (diff_lo >= v_lo && diff_hi <= v_hi)
			{
				JEntry	je = root->children[N + k];

				if (JBE_ISCONTAINER(je))
				{
					/*
					 * Diff is inside an array/object value — out of
					 * scope. This catches scenario C: same-length
					 * array element replacement. postgrespro had a
					 * latent correctness bug here; we decline so core
					 * falls back to full retoast and produces the
					 * correct result.
					 */
					pfree(base_full);
					return (Datum) 0;
				}
				accepted = true;
				break;
			}
		}

		if (!accepted)
		{
			/*
			 * Diff range crosses value boundaries or lies in the
			 * gap between values (shouldn't happen in well-formed
			 * jsonb but be defensive).
			 */
			pfree(base_full);
			return (Datum) 0;
		}
	}

	/*
	 * All conditions met. Build a DIFF custom-pointer. Diff offset
	 * is the byte offset within the body PAYLOAD; diff length is
	 * (diff_hi - diff_lo + 1); diff data is the new bytes in that
	 * range.
	 */
	result = jbtl_toast_make_pointer_diff(tcxt->toasterid,
										  &base_ext,
										  base_compressed,
										  diff_lo,
										  diff_hi - diff_lo + 1,
										  new_body + diff_lo);

	pfree(base_full);

	jbtl_update_diff_emitted_count++;
	return PointerGetDatum(result);
}

PG_FUNCTION_INFO_V1(jbtl_update_calls);
Datum
jbtl_update_calls(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(jbtl_update_call_count);
}

PG_FUNCTION_INFO_V1(jbtl_update_calls_reset);
Datum
jbtl_update_calls_reset(PG_FUNCTION_ARGS)
{
	jbtl_update_call_count = 0;
	jbtl_update_diff_emitted_count = 0;
	jbtl_update_subtree_reuse_attempts = 0;
	jbtl_update_subtree_reuse_success = 0;
	jbtl_update_subtree_children_reused = 0;
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(jbtl_update_diffs_emitted);
Datum
jbtl_update_diffs_emitted(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(jbtl_update_diff_emitted_count);
}

/*
 * Diagnostic accessors for the M9.2 narrow SUBTREE reuse path.
 *
 *	jbtl_update_subtree_reuse_attempts_fn()  - times jbtl_update
 *	                                           entered SUBTREE branch
 *	jbtl_update_subtree_reuse_successes_fn() - times the branch
 *	                                           returned a reused row
 *	jbtl_update_subtree_children_reused_fn() - total children whose
 *	                                           chain was preserved
 *
 *	C names have _fn suffix to avoid collision with the static int
 *	counter variables.  SQL aliases drop the suffix.
 */
PG_FUNCTION_INFO_V1(jbtl_update_subtree_reuse_attempts_fn);
Datum
jbtl_update_subtree_reuse_attempts_fn(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(jbtl_update_subtree_reuse_attempts);
}

PG_FUNCTION_INFO_V1(jbtl_update_subtree_reuse_successes_fn);
Datum
jbtl_update_subtree_reuse_successes_fn(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(jbtl_update_subtree_reuse_success);
}

PG_FUNCTION_INFO_V1(jbtl_update_subtree_children_reused_fn);
Datum
jbtl_update_subtree_children_reused_fn(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(jbtl_update_subtree_children_reused);
}

/*
 * jbtl_test_subtree_spill_key
 *
 *	Args: rel oid, jb jsonb, key text
 *	Returns: jsonb (custom-varlena of mode JBTL_POINTER_SUBTREE)
 *
 *	One-shot manual spill: takes a regular jsonb body, finds the value
 *	at the given top-level key, writes that value as a separate toast
 *	chain in `rel`'s toast relation, then constructs a parent body
 *	where the JEntry at that key position has type
 *	JBTL_JENTRY_ISCONTAINER_PTR and the value-data slot carries the
 *	new chain's varatt_external. Wraps the parent body in a
 *	JBTL_POINTER_SUBTREE custom-varlena.
 *
 *	Restrictions (simplifying assumptions for ):
 *	 - input body must be a non-scalar object
 *	 - target key's value must be a container (jbvBinary)
 *	 - body must not use offset cache (no JEntry has JENTRY_HAS_OFF
 *	 on any value JEntry that follows the spilled one)
 *	 - input body must not have KVMap (i.e., not produced with
 *	 SET jsonb_sort_field_values=on)
 *
 *	These restrictions are sufficient for tests (3-key documents
 *	with one big array). 's production writer in tsr_toast will
 *	handle the general case.
 */
PG_FUNCTION_INFO_V1(jbtl_test_subtree_spill_key);
Datum
jbtl_test_subtree_spill_key(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Jsonb	   *jb = PG_GETARG_JSONB_P(1);
	text	   *key_t = PG_GETARG_TEXT_PP(2);
	Oid			toasterid = PG_GETARG_OID(3);
	const char *key_p = VARDATA_ANY(key_t);
	int			key_len = VARSIZE_ANY_EXHDR(key_t);
	JsonbContainer *root = &jb->root;
	int			N;
	int			n_jentries;
	int			val_base;
	int			k;
	int			match_idx = -1;
	JEntry		val_je;
	int32		val_off;
	int32		val_len;
	char	   *base_addr;
	char	   *value_bytes;
	struct varlena *child_varlena;
	int32		child_varlena_size;
	Relation	rel;
	Datum		child_datum;
	struct varatt_external child_ext;
	int32		new_body_size;
	char	   *new_body;
	JEntry	   *new_jentries;
	int32		hdr_je_size;
	int32		new_value_payload_size;
	JbtlToastedContainerPointer *new_payload;
	struct varlena *result;

	if (!JsonContainerIsObject(root))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("jbtl_test_subtree_spill_key: argument must be an object")));

	if (JsonContainerHasKVMap(root))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("jbtl_test_subtree_spill_key: KVMap-bearing objects "
						"not supported; "
						"use SET jsonb_sort_field_values=off")));

	N = JsonContainerSize(root);
	n_jentries = 2 * N;
	val_base = N;

	/* Locate the requested key by linear scan over key entries. */
	for (k = 0; k < N; k++)
	{
		int32		k_off = (int32) getJsonbOffset(root, k);
		int32		k_len = (int32) getJsonbLength(root, k);
		const char *kbase;

		base_addr = (char *) (root->children + n_jentries);
		kbase = base_addr + k_off;

		if (k_len == key_len && memcmp(kbase, key_p, key_len) == 0)
		{
			match_idx = k;
			break;
		}
	}

	if (match_idx < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("jbtl_test_subtree_spill_key: key \"%.*s\" not found",
						key_len, key_p)));

	val_je = root->children[val_base + match_idx];
	if ((val_je & JENTRY_TYPEMASK) != JENTRY_ISCONTAINER)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("jbtl_test_subtree_spill_key: value at \"%.*s\" "
						"is not a container; can't spill",
						key_len, key_p)));

	val_off = (int32) getJsonbOffset(root, val_base + match_idx);
	val_len = (int32) getJsonbLength(root, val_base + match_idx);

	/*
	 * Reject if any later value JEntry has JENTRY_HAS_OFF set, because
	 * we'd need to recompute its offset and we don't bother in this
	 * fixture.
	 */
	for (k = val_base + match_idx + 1; k < n_jentries; k++)
	{
		if (root->children[k] & JENTRY_HAS_OFF)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("jbtl_test_subtree_spill_key: body uses JEntry "
							"offset cache; not supported")));
	}

	hdr_je_size = (int32) sizeof(uint32) + n_jentries * (int32) sizeof(JEntry);
	base_addr = (char *) (root->children + n_jentries);
	value_bytes = base_addr + val_off;

	/*
	 * Wrap the value's container bytes in a fresh varlena and save it
	 * to the relation's toast. This is the "child" toast chain.
	 */
	child_varlena_size = val_len + VARHDRSZ;
	child_varlena = (struct varlena *) palloc(child_varlena_size);
	SET_VARSIZE(child_varlena, child_varlena_size);
	memcpy(VARDATA(child_varlena), value_bytes, val_len);

	rel = table_open(relid, AccessShareLock);
	child_datum = jbtl_toast_save_datum(rel, PointerGetDatum(child_varlena),
										NULL, 0);
	table_close(rel, NoLock);

	if (!VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(child_datum)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("jbtl_test_subtree_spill_key: jbtl_toast_save_datum "
						"did not return an on-disk external")));

	VARATT_EXTERNAL_GET_POINTER(child_ext, DatumGetPointer(child_datum));

	/*
	 * Build the new parent body. Layout of new body:
	 *  [hdr | JBTL_JBC_TOBJECT_TOASTED]
	 *  [JEntries — same except value at match_idx is rewritten]
	 *  [keys — unchanged]
	 *  [values 0..match_idx-1 — unchanged]
	 *  [JbtlToastedContainerPointer = JEntry header_copy + varatt_external]
	 *  [values match_idx+1..N-1 — unchanged]
	 *
	 * New value-payload size: 4 (JEntry header_copy) + TOAST_POINTER_SIZE.
	 * Net body delta: new_value_payload_size − val_len.
	 */
	new_value_payload_size = (int32) sizeof(JEntry) + TOAST_POINTER_SIZE;
	new_body_size = (int32) VARSIZE(jb) - VARHDRSZ
		- val_len + new_value_payload_size;

	new_body = (char *) palloc(new_body_size);

	/* Header with JBTL_JBC_TOBJECT_TOASTED hint bit. */
	{
		uint32		new_hdr = root->header | JBTL_JBC_TOBJECT_TOASTED;

		memcpy(new_body, &new_hdr, sizeof(new_hdr));
	}

	/* JEntries: copy and rewrite the one at val_base + match_idx. */
	new_jentries = (JEntry *) (new_body + sizeof(uint32));
	memcpy(new_jentries, root->children, n_jentries * sizeof(JEntry));
	new_jentries[val_base + match_idx] =
		JBTL_JENTRY_ISCONTAINER_PTR | (new_value_payload_size & JENTRY_OFFLENMASK);

	/* Keys: byte-identical block. */
	{
		int32		key_area_size = 0;

		for (k = 0; k < N; k++)
			key_area_size += (int32) getJsonbLength(root, k);

		memcpy(new_body + hdr_je_size,
			   base_addr,
			   key_area_size);

		/* Values up to match_idx (exclusive). */
		{
			int32		copied_val_bytes = 0;
			int32		dst_pos = hdr_je_size + key_area_size;
			int32		src_pos_in_data = key_area_size;

			for (k = 0; k < match_idx; k++)
			{
				int32		this_len =
					(int32) getJsonbLength(root, val_base + k);

				memcpy(new_body + dst_pos,
					   base_addr + src_pos_in_data,
					   this_len);
				dst_pos += this_len;
				src_pos_in_data += this_len;
				copied_val_bytes += this_len;
			}

			/* New value payload at match_idx slot. */
			new_payload = (JbtlToastedContainerPointer *) (new_body + dst_pos);
			new_payload->header = root->header;	/* copy of CHILD container's
												 * header is the same as the
												 * value's JEntry container
												 * indicator. Postgrespro stores
												 * the child-container's header
												 * here; we use root's header bits
												 * since we synthesised the child
												 * from a slice of root's body
												 * with the same container header
												 * shape (object/array + count).
												 *
												 * Actually we want the CHILD's
												 * container header — which equals
												 * the first 4 bytes of value_bytes.
												 */
			memcpy(&new_payload->header, value_bytes, sizeof(JEntry));
			memcpy(new_payload->data,
				   DatumGetPointer(child_datum),
				   TOAST_POINTER_SIZE);
			dst_pos += new_value_payload_size;
			src_pos_in_data += val_len;	/* skip old value bytes */

			/* Values after match_idx. */
			for (k = match_idx + 1; k < N; k++)
			{
				int32		this_len =
					(int32) getJsonbLength(root, val_base + k);

				memcpy(new_body + dst_pos,
					   base_addr + src_pos_in_data,
					   this_len);
				dst_pos += this_len;
				src_pos_in_data += this_len;
			}

			Assert(dst_pos == new_body_size);
		}
	}

	/*
	 * Wrap in JBTL_POINTER_SUBTREE custom-varlena. For the
	 * test fixture we pass InvalidOid as the toasterid because the
	 * read dispatcher in jbtl_detoast does not actually consult the
	 * toasterid field — it dispatches purely on the mode tag. 's
	 * production tsr_toast will set toasterid correctly via the normal
	 * toaster context.
	 */
	result = jbtl_toast_make_pointer_subtree(toasterid,
											 new_body, new_body_size);

	pfree(new_body);
	pfree(child_varlena);
	pfree(DatumGetPointer(child_datum));

	PG_RETURN_POINTER(result);
}

/* ---- handler ------------------------------------------------------------ */

Datum
jsonb_toaster_lite_handler(PG_FUNCTION_ARGS)
{
	TsrRoutine *tsr = palloc0(sizeof(TsrRoutine));

	tsr->tsr_size = sizeof(TsrRoutine);

	/*
	 * Self-identify our OID for the VACUUM FULL gate.  We need the
	 * handler's pg_proc OID to walk pg_attribute.attoptions and find
	 * columns managed by us; capturing fn_oid here is the simplest
	 * way that does not require name-based lookup (which is fragile
	 * and was crashing on argtypes==NULL/nargs==-1 in LookupFuncName).
	 */
	if (!OidIsValid(jbtl_handler_oid_cache) && fcinfo->flinfo != NULL)
		jbtl_handler_oid_cache = fcinfo->flinfo->fn_oid;

	tsr->tsr_validate = jbtl_validate;
	tsr->tsr_toast = jbtl_toast;
	tsr->tsr_detoast = jbtl_detoast;
	tsr->tsr_delete = jbtl_delete;
	tsr->tsr_copy = jbtl_copy;
	tsr->tsr_update = jbtl_update;

	PG_RETURN_POINTER(tsr);
}


/* ---- jbtl_slice_probe (test/observability helper) ----------------------- */

/*
 * jbtl_slice_probe(jb jsonb, sliceoffset int, slicelength int)
 *	 RETURNS (slice_bytes bytea, chunks_total int, chunks_fetched int,
 *	 toast_pages_touched int, chunks_decompressed int,
 *	 bytes_decompressed int)
 *
 *	Test probe used by the regression to verify that a slice
 *	read fetches strictly fewer chunks than a full read. Inspects the
 *	raw datum (without core auto-detoasting) so we can see the
 *	JBTL_POINTER custom-varlena and walk into our chunk-fetcher with
 *	counters.
 *
 *	Output columns:
 *	 slice_bytes the slice payload as bytea
 *	 chunks_total total chunks of the value (logical)
 *	 chunks_fetched chunks actually decoded from disk for this slice
 *	 toast_pages_touched count(distinct blockno) of the toast tuples
 *	 fetched. This is the honest physical-I/O
 *	 metric: ~4 chunks share an 8KB page, so
 *	 chunks_fetched can overstate page savings.
 *	 0 in compute-only mode (PLAIN_JSONB / fallback).
 *	 chunks_decompressed per-chunk pglz decompression count;
 *	 0 for plain mode, non-zero only in
 *	 JBTL_POINTER_COMPRESSED_CHUNKS.
 *	 bytes_decompressed total bytes produced by per-chunk pglz
 *	 decompression in this call; 0 for plain.
 *
 *	Behaviour by what arrives at PG_GETARG_DATUM(0):
 *	 - a JBTL_POINTER custom-varlena: extract bare ext-ptr, call
 *	 jbtl_toast_fetch_slice_plain, return the slice bytes plus
 *	 chunks_total / chunks_fetched / toast_pages_touched.
 *	 - a JBTL_POINTER_COMPRESSED_CHUNKS custom-varlena: same shape,
 *	 additionally surfaces chunks_decompressed / bytes_decompressed.
 *	 - a JBTL_PLAIN_JSONB custom-varlena: slice the inline body in
 *	 memory; all counters 0.
 *	 - any other shape (bare TOAST pointer, plain inline jsonb,
 *	 expanded, etc.): detoast in full first, then slice in memory;
 *	 all counters 0. This case is not the one the
 *	 acceptance test exercises but is useful for sanity checks
 *	 on small values.
 */
Datum
jbtl_slice_probe(PG_FUNCTION_ARGS)
{
	Datum		raw_datum = PG_GETARG_DATUM(0);
	int32		sliceoffset = PG_GETARG_INT32(1);
	int32		slicelength = PG_GETARG_INT32(2);
	struct varlena *raw = (struct varlena *) DatumGetPointer(raw_datum);
	struct varlena *slice;
	int32		chunks_total = 0;
	int32		chunks_fetched = 0;
	int32		toast_pages_touched = 0;
	int32		chunks_decompressed = 0;
	int32		bytes_decompressed = 0;
	int32		slice_payload_len;
	bytea	   *slice_bytes;

	TupleDesc	tupdesc;
	Datum		values[6];
	bool		isnull[6] = {false, false, false, false, false, false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "function returning record called in context that cannot accept type record");
	tupdesc = BlessTupleDesc(tupdesc);

	if (VARATT_IS_CUSTOM(raw))
	{
		uint32		mode = JBTL_CUSTOM_PTR_GET_HEADER(raw) & JBTL_POINTER_TYPE_MASK;

		if (mode == JBTL_POINTER)
		{
			struct varatt_external ext_ptr;
			char	   *bare = JBTL_CUSTOM_PTR_GET_DATA(raw);

			Assert(VARATT_IS_EXTERNAL_ONDISK(bare));
			VARATT_EXTERNAL_GET_POINTER(ext_ptr, bare);

			slice = jbtl_toast_fetch_slice_plain(&ext_ptr,
												 sliceoffset, slicelength,
												 &chunks_total,
												 &chunks_fetched,
												 &toast_pages_touched);
			slice_payload_len = VARSIZE(slice) - VARHDRSZ;
		}
		else if (mode == JBTL_POINTER_COMPRESSED_CHUNKS)
		{
			struct varatt_external ext_ptr;
			char	   *bare = JBTL_CUSTOM_PTR_GET_DATA(raw);

			Assert(VARATT_IS_EXTERNAL_ONDISK(bare));
			VARATT_EXTERNAL_GET_POINTER(ext_ptr, bare);

			slice = jbtl_toast_fetch_compressed_chunks(&ext_ptr,
													   sliceoffset, slicelength,
													   &chunks_total,
													   &chunks_fetched,
													   &chunks_decompressed,
													   &toast_pages_touched,
													   &bytes_decompressed);
			slice_payload_len = VARSIZE(slice) - VARHDRSZ;
		}
		else if (mode == JBTL_PLAIN_JSONB)
		{
			char	   *body = JBTL_CUSTOM_PTR_GET_DATA(raw);
			int32		body_payload = VARSIZE(body) - VARHDRSZ;
			int32		want_off = sliceoffset < 0 ? 0 :
				sliceoffset > body_payload ? body_payload : sliceoffset;
			int32		want_len = slicelength < 0 ? 0 : slicelength;

			if (want_len > body_payload - want_off)
				want_len = body_payload - want_off;

			slice = (struct varlena *) palloc(want_len + VARHDRSZ);
			SET_VARSIZE(slice, want_len + VARHDRSZ);
			memcpy(VARDATA(slice),
				   (char *) body + VARHDRSZ + want_off,
				   want_len);
			slice_payload_len = want_len;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("jbtl_slice_probe: mode 0x%08X is not supported",
							mode)));
			return (Datum) 0;	/* keep the compiler happy */
		}
	}
	else
	{
		/*
		 * Not one of our custom varlenas. Fall back to detoasting in
		 * full and slicing in memory. No chunk counters are meaningful
		 * here.
		 */
		struct varlena *full = detoast_attr(raw);
		int32		body_payload = VARSIZE(full) - VARHDRSZ;
		int32		want_off = sliceoffset < 0 ? 0 :
			sliceoffset > body_payload ? body_payload : sliceoffset;
		int32		want_len = slicelength < 0 ? 0 : slicelength;

		if (want_len > body_payload - want_off)
			want_len = body_payload - want_off;

		slice = (struct varlena *) palloc(want_len + VARHDRSZ);
		SET_VARSIZE(slice, want_len + VARHDRSZ);
		memcpy(VARDATA(slice),
			   (char *) full + VARHDRSZ + want_off,
			   want_len);
		slice_payload_len = want_len;
	}

	/*
	 * Wrap the slice payload as a bytea result. bytea has the same
	 * varlena layout as our slice so we can return it almost as-is,
	 * but to be safe we copy into a fresh bytea so the caller can
	 * pfree the input.
	 */
	slice_bytes = (bytea *) palloc(slice_payload_len + VARHDRSZ);
	SET_VARSIZE(slice_bytes, slice_payload_len + VARHDRSZ);
	memcpy(VARDATA(slice_bytes), VARDATA(slice), slice_payload_len);
	pfree(slice);

	values[0] = PointerGetDatum(slice_bytes);
	values[1] = Int32GetDatum(chunks_total);
	values[2] = Int32GetDatum(chunks_fetched);
	values[3] = Int32GetDatum(toast_pages_touched);
	values[4] = Int32GetDatum(chunks_decompressed);
	values[5] = Int32GetDatum(bytes_decompressed);

	tuple = heap_form_tuple(tupdesc, values, isnull);
	return HeapTupleGetDatum(tuple);
}


/* ---- jbtl_chunk_inspect (storage-layout observability) ----------------- */

/*
 * jbtl_chunk_inspect(jb jsonb)
 *	 RETURNS TABLE (chunk_no int, chunk_seq int, raw_offset int,
 *	 raw_size int, stored_size int,
 *	 is_compressed bool, compression_method int)
 *
 *	Per-row dump of the toast relation backing a jsonb_toaster_lite
 *	value. Used's regression test to confirm that
 *	per-chunk-compressed values actually have at least one
 *	VARATT_IS_COMPRESSED row, and to expose mixed compressed/raw
 *	streams.
 *
 *	Output columns:
 *	 chunk_no sequential row number 0..N-1 (in chunk_seq order)
 *	 chunk_seq raw value of column 2 (= last byte offset in
 *	 uncompressed payload for compressed-chunks mode,
 *	 = sequential index for plain-chunks mode)
 *	 raw_offset first byte offset of this chunk in the
 *	 uncompressed payload (computed; same as chunk_seq
 *	 in plain mode * TOAST_MAX_CHUNK_SIZE, or
 *	 chunk_seq - raw_size + 1 in compressed mode)
 *	 raw_size bytes this chunk represents in the uncompressed
 *	 payload
 *	 stored_size bytes this chunk occupies on disk (raw size of
 *	 the chunk_data column varlena, including header)
 *	 is_compressed TRUE if VARATT_IS_COMPRESSED on this row
 *	 compression_method 0=PGLZ, 1=LZ4, 2=invalid (raw)
 *
 *	Only invoked from regression tests; this is internal observability
 *	machinery. Pass-through values that are not stored as
 *	jsonb_toaster_lite custom-pointers (plain inline jsonb, regular
 *	external TOAST) yield zero rows.
 */
Datum
jbtl_chunk_inspect(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Datum		raw_datum;
	struct varlena *raw;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	raw_datum = PG_GETARG_DATUM(0);
	raw = (struct varlena *) DatumGetPointer(raw_datum);

	if (VARATT_IS_CUSTOM(raw))
	{
		uint32		mode = JBTL_CUSTOM_PTR_GET_HEADER(raw) & JBTL_POINTER_TYPE_MASK;

		if (mode == JBTL_POINTER || mode == JBTL_POINTER_COMPRESSED_CHUNKS)
		{
			bool		compressed_mode =
				(mode == JBTL_POINTER_COMPRESSED_CHUNKS);
			struct varatt_external ext_ptr;
			char	   *bare = JBTL_CUSTOM_PTR_GET_DATA(raw);
			Relation	toastrel;
			Relation   *toastidxs;
			int			num_indexes;
			int			validIndex;
			ScanKeyData toastkey;
			SysScanDesc toastscan;
			HeapTuple	ttup;
			int32		chunk_no = 0;
			int32		max_chunk = TOAST_MAX_CHUNK_SIZE;

			Assert(VARATT_IS_EXTERNAL_ONDISK(bare));
			VARATT_EXTERNAL_GET_POINTER(ext_ptr, bare);

			toastrel = table_open(ext_ptr.va_toastrelid, AccessShareLock);
			validIndex = toast_open_indexes(toastrel,
											AccessShareLock,
											&toastidxs,
											&num_indexes);

			ScanKeyInit(&toastkey, (AttrNumber) 1,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(ext_ptr.va_valueid));
			toastscan = systable_beginscan_ordered(toastrel,
												   toastidxs[validIndex],
												   get_toast_snapshot(),
												   1, &toastkey);

			while ((ttup = systable_getnext_ordered(toastscan,
													ForwardScanDirection))
				   != NULL)
			{
				Datum		row_values[8];
				bool		row_isnull[8] = {false, false, false,
											 false, false, false, false,
											 false};
				int32		seq;
				Pointer		chunk;
				bool		isnull;
				int32		raw_size;
				int32		stored_size;
				int32		raw_offset;
				bool		is_compressed;
				int32		method;
				int32		toast_blockno;

				seq = DatumGetInt32(fastgetattr(ttup, 2,
												toastrel->rd_att,
												&isnull));
				Assert(!isnull);

				chunk = DatumGetPointer(fastgetattr(ttup, 3,
													toastrel->rd_att,
													&isnull));
				Assert(!isnull);

				stored_size = (int32) VARSIZE_ANY(chunk);

				if (VARATT_IS_COMPRESSED(chunk))
				{
					is_compressed = true;
					raw_size = (int32) VARDATA_COMPRESSED_GET_EXTSIZE(chunk);
					method = (int32) VARDATA_COMPRESSED_GET_COMPRESS_METHOD(chunk);
				}
				else if (!VARATT_IS_EXTENDED(chunk))
				{
					is_compressed = false;
					raw_size = (int32) (VARSIZE(chunk) - VARHDRSZ);
					method = TOAST_INVALID_COMPRESSION_ID;
				}
				else if (VARATT_IS_SHORT(chunk))
				{
					is_compressed = false;
					raw_size = (int32) (VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT);
					method = TOAST_INVALID_COMPRESSION_ID;
				}
				else
				{
					is_compressed = false;
					raw_size = 0;
					method = TOAST_INVALID_COMPRESSION_ID;
				}

				if (compressed_mode)
					raw_offset = seq - raw_size + 1;	/* first byte */
				else
					raw_offset = chunk_no * max_chunk;

				toast_blockno = (int32) ItemPointerGetBlockNumber(&ttup->t_self);

				row_values[0] = Int32GetDatum(chunk_no);
				row_values[1] = Int32GetDatum(seq);
				row_values[2] = Int32GetDatum(raw_offset);
				row_values[3] = Int32GetDatum(raw_size);
				row_values[4] = Int32GetDatum(stored_size);
				row_values[5] = BoolGetDatum(is_compressed);
				row_values[6] = Int32GetDatum(method);
				row_values[7] = Int32GetDatum(toast_blockno);

				tuplestore_putvalues(tupstore, tupdesc,
									 row_values, row_isnull);
				chunk_no++;
			}

			systable_endscan_ordered(toastscan);
			toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
			table_close(toastrel, AccessShareLock);
		}
		/* Inline plain-jsonb: no toast rows, return empty set. */
	}
	/* Other shapes: empty set. */

	return (Datum) 0;
}
