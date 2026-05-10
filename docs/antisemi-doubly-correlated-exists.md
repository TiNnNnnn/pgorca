# Doubly-correlated EXISTS+NOT_EXISTS Hoisting Fix

## Problem

ORCA falls back to the PG standard planner for the following pattern:

```sql
select a.thousand from tenk1 a, tenk1 b
where a.thousand = b.thousand
  and exists ( select 1 from tenk1 c where b.hundred = c.hundred
                   and not exists ( select 1 from tenk1 d
                                    where a.thousand = d.thousand ) );
```

The inner `NOT_EXISTS(d WHERE a.thousand=d.thousand)` uses `a.thousand`, which becomes
a **skip-level outer reference** from inside the `EXISTS(c ...)` body. ORCA's
`CSubqueryHandler` cannot unnest such doubly-correlated existential patterns and
currently throws an assertion.

## Approach: Predicate Hoisting

Hoist the nested `NOT_EXISTS` out of the `EXISTS` body before unnesting, so each
existential becomes a single-level correlated subquery that `CSubqueryHandler` can
handle independently.

```
-- Before hoisting
EXISTS(c WHERE b.hundred=c.hundred AND NOT_EXISTS(d WHERE a.thousand=d.thousand))

-- After hoisting
AND(
  EXISTS(c WHERE b.hundred=c.hundred),
  NOT_EXISTS(d WHERE a.thousand=d.thousand)
)
```

Both resulting existentials reference only one outer scope and can be unnested as
semi/anti-semi Apply joins producing a hash join plan.

## Files Changed

| File | Change |
|------|--------|
| `libgpopt/include/gpopt/xforms/CXformSimplifySubquery.h` | Declare `FScalarHoistNestedExistentials` and `PexprHoistNestedExistentials` |
| `libgpopt/src/xforms/CXformSimplifySubquery.cpp` | Implement `FHoistNestedExistential` (hoisting logic); `FScalarHoistNestedExistentials` (scalar-only entry point that bypasses CNormalizer); guard in `FSimplifyExistential` suppressing count(*) rewrite when a correlated nested existential is present |
| `libgpopt/src/xforms/CXformSubqJoin2Apply.cpp` | After the first `PexprSubqueryUnnest` returns nullptr, call `FScalarHoistNestedExistentials` on the scalar predicate, rebuild a `LogicalSelect`, and retry `PexprSubqueryUnnest` |

## Current Status: BLOCKED

### Symptom

ORCA triggers an unexpected assertion failure before the hoisting fallback runs.
pg.log (as of 2026-05-10):

```
CSubqueryHandler.cpp:83: Failed assertion: pexprScalar->Pop()->FScalar()
Stack:
1    CException::Raise
2    CSubqueryHandler::AssertValidArguments + 415
3    CSubqueryHandler::FProcess + 69
4    CXformSubqueryUnnest::PexprSubqueryUnnest + 367
5    CXformSubqJoin2Apply::Transform + 678
6    CXformSubqueryUnnest::Transform + 322
```

`PexprSubqueryUnnest` asserts that `pexpr[1]` (the scalar child) is scalar, but it
receives an expression whose child at index 1 is relational. The assertion fires at
the **first** call to `PexprSubqueryUnnest` (line 358 in `CXformSubqJoin2Apply.cpp`),
so the hoisting fallback block (lines 361–388) never executes.

### Root Cause (unresolved)

Expected flow inside `CXformSubqJoin2Apply::Transform`:

1. `PexprSeparateSubqueryPreds(pexpr)` → `LogicalSelect(join, EXISTS(...))` — child[1] is scalar ✓
2. `PexprSubqueryPushDown(pexprSelect)` → returns another `LogicalSelect` — child[1] should be scalar ✓
3. `PexprSubqueryUnnest(pexprSubqsPushedDown)` → **assertion fires**

Code review of `PexprSubqueryPushDown` shows it always returns
`CExpression(LogicalSelectOp, newJoin, newScalar)`, so child[1] should always be
scalar. Why the assertion fires is not yet determined.

### Debugging Constraints

- `fopen("/tmp/hoist_trace.txt", "a")` inside the backend returns NULL — the backend
  process cannot create new files this way even though `/tmp` is world-writable.
  GPOS-level traces (`GPOS_ASSERT`, `GPOS_TRACE`) do appear in `pg.log`.
- Binary: build and install `.so` are identical (md5 verified), both 108 MB, built
  2026-05-10 07:03.

### Candidate Next Steps

1. Add `GPOS_ASSERT((*pexprSubqsPushedDown)[1]->Pop()->FScalar())` immediately before
   the call at line 358. If this fires, the pg.log stack trace will show exactly what
   expression was passed.
2. Add `GPOS_ASSERT(COperator::EopLogicalSelect == pexprSubqsPushedDown->Pop()->Eopid())`
   to confirm `pexprSubqsPushedDown` is really a `LogicalSelect`.
3. Check whether ORCA is applying `CXformSubqJoin2Apply` to a *different* subtree in
   the expression tree (not the outer `a,b` join) where the expression structure does
   not produce a `LogicalSelect`.

## Cleanup TODO

Once the assertion is fixed and the plan shows `Optimizer: pg_orca`:

- Remove all `fopen`/`fprintf` debug traces from `CXformSubqJoin2Apply.cpp` and
  `CXformSimplifySubquery.cpp`.
- Run `test/test.sh --pg-tests --ignore-plans` to confirm no regressions.
- Commit on branch `antisemi`.
