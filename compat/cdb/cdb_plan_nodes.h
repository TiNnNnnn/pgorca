/*
 * compat/cdb/cdb_plan_nodes.h
 *
 * Stub definitions for Cloudberry MPP plan node types that do not exist
 * in PostgreSQL 18. In pg_orca (single-node mode) these nodes are never
 * actually generated, but the translation code still references their types.
 *
 * Only the types the translation layer actually instantiates live here.
 * Nodes that pg_orca translates into native PG18 plans instead (Sequence →
 * folded away, DynamicSeqScan / PartitionSelector → CustomScan, ShareInputScan
 * → subplans) have no stub at all.
 */
#ifndef COMPAT_CDB_PLAN_NODES_H
#define COMPAT_CDB_PLAN_NODES_H

#include "postgres.h"
#include "nodes/plannodes.h"

/*
 * Node tags for GPDB/CBDB-only plan nodes that do not exist in PG18.
 *
 * We assign distinct out-of-range values (>= 5000) rather than T_Invalid (0)
 * so that the executor's "unrecognized node type: N" error message carries a
 * meaningful, grep-able number instead of 0 which is hard to attribute.
 * PG18's highest NodeTag is ~479, so 5000+ is safely out of range.
 *
 * The values mirror the relative ordering from Cloudberry's nodes.h so they
 * are stable across rebuilds. Gaps (5002, 5004, 5005, 5007) are tags whose
 * nodes pg_orca no longer stubs; leave them unused so the live tags keep their
 * historical values.
 */
#define T_Motion				  ((NodeTag) 5001)
#define T_SplitUpdate			  ((NodeTag) 5003)
#define T_AssertOp				  ((NodeTag) 5006)
#define T_DynamicIndexScan		  ((NodeTag) 5008)
#define T_DynamicIndexOnlyScan	  ((NodeTag) 5009)
#define T_DynamicBitmapHeapScan	  ((NodeTag) 5010)
#define T_DynamicBitmapIndexScan  ((NodeTag) 5011)
#define T_DynamicForeignScan	  ((NodeTag) 5012)

/* MASTER_CONTENT_ID used in segment logic */
#define MASTER_CONTENT_ID (-1)

/* GangType — how a slice's executor gang is configured */
typedef enum GangType
{
	GANGTYPE_UNALLOCATED,
	GANGTYPE_ENTRYDB_READER,
	GANGTYPE_SINGLETON_READER,
	GANGTYPE_PRIMARY_READER,
	GANGTYPE_PRIMARY_WRITER
} GangType;

/* DirectDispatchInfo — used inside PlanSlice */
typedef struct DirectDispatchInfo
{
	bool		isDirectDispatch;
	List	   *contentIds;
	bool		haveProcessedAnyCalculations;
} DirectDispatchInfo;

/*
 * PlanSlice — one execution slice (gang) in an MPP query.
 * In single-node PG18 mode this is never populated, but the type must exist.
 */
typedef struct PlanSlice
{
	int			sliceIndex;
	int			parentIndex;
	GangType	gangType;
	int			numsegments;
	int			segindex;
	DirectDispatchInfo directDispatch;
} PlanSlice;

typedef enum MotionType
{
	MOTIONTYPE_GATHER,
	MOTIONTYPE_GATHER_SINGLE,
	MOTIONTYPE_HASH,
	MOTIONTYPE_BROADCAST,
	MOTIONTYPE_EXPLICIT,
	MOTIONTYPE_OUTER_QUERY
} MotionType;

/*
 * Motion — data redistribution node (MPP only).
 * The full struct is defined here so the translation code compiles.
 * In single-node PG18 mode these nodes are never generated at execution.
 */
typedef struct Motion
{
	Plan		plan;
	int			motionID;
	MotionType	motionType;
	/* sorting support */
	bool		sendSorted;
	int			numSortCols;
	AttrNumber *sortColIdx;
	Oid		   *sortOperators;
	Oid		   *collations;
	bool	   *nullsFirst;
	/* hash redistribution */
	int			numHashExprs;
	List	   *hashExprs;
	Oid		   *hashFuncs;
	int			numHashSegments;
	AttrNumber	segidColIdx;
} Motion;

/*
 * DynamicIndexScan / DynamicBitmapHeapScan / ... —
 * GPDB-specific scan nodes for partitioned tables.
 * These use partition pruning at runtime. In PG18, ORCA should generate
 * Append plans instead, but we need the types to compile.
 */
typedef struct DynamicIndexScan
{
	IndexScan	indexscan;
	List	   *partOids;
	List	   *join_prune_paramids;
} DynamicIndexScan;

typedef struct DynamicIndexOnlyScan
{
	IndexScan	indexscan;
	List	   *partOids;
	List	   *join_prune_paramids;
} DynamicIndexOnlyScan;

typedef struct DynamicForeignScan
{
	ForeignScan	foreignscan;
	List	   *partOids;
	List	   *join_prune_paramids;
} DynamicForeignScan;

typedef struct DynamicBitmapHeapScan
{
	BitmapHeapScan bitmapheapscan;
	List	   *partOids;
	List	   *join_prune_paramids;
} DynamicBitmapHeapScan;

typedef struct DynamicBitmapIndexScan
{
	BitmapIndexScan biscan;
	List	   *partOids;
	List	   *join_prune_paramids;
} DynamicBitmapIndexScan;

/*
 * SplitUpdate — used for UPDATE with distribution key changes (MPP).
 * Not generated in single-node mode.
 */
typedef struct SplitUpdate
{
	Plan		plan;
	int			numHashFilterCols;
	AttrNumber *hashFilterColIdx;
	Oid		   *hashFilterFuncs;
	AttrNumber	actionColIdx;	/* attribute number of the action column */
} SplitUpdate;

/*
 * AssertOp — GPDB plan node that enforces a constraint with a custom error.
 * Not generated in single-node mode.
 */
typedef struct AssertOp
{
	Plan		plan;
	int			errcode;	/* SQLSTATE error code */
	List	   *errmessage;	/* list of error messages (Const nodes) */
} AssertOp;

#endif /* COMPAT_CDB_PLAN_NODES_H */
