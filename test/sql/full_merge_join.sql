-- Full merge join distribution-spec derivation regression.
-- Port of apache/cloudberry#1896.
--
-- CPhysicalFullMergeJoin::PdsDerive used to unconditionally return the
-- outer child's distribution spec whenever it was Universal, ignoring the
-- inner child's real distribution.  In MPP this mislabeled a gathered
-- Singleton inner child as Universal and produced an illegal slice
-- ("unexpected gang size: N" from ExecInitMotion).  In single-node pg_orca
-- the same shape is reachable (a provably-empty outer side derives
-- Universal while regular tables derive Singleton); the fix keeps the
-- derived property honest by propagating the inner child's distribution.
--
-- The merge join implementation is forced via disable_xform, since the
-- choice between hash join and merge join for a FULL JOIN is cost-based.

LOAD 'pg_orca';
SET pg_orca.enable_orca = on;
SET client_min_messages = warning;

select disable_xform('CXformFullOuterJoin2HashJoin');

create table mj_empty (c1 int);
create table mj_real (c1 int);
create table mj_cross (c1 int);
insert into mj_real select i from generate_series(1,2) i;
insert into mj_cross select i from generate_series(1,3) i;
analyze mj_empty;
analyze mj_real;
analyze mj_cross;

-- Show the plan: comma-join + full join against a provably-empty side.
-- This is the originally-crashing shape upstream; the plan must be a legal
-- merge full join through ORCA.
explain (costs off, timing off, summary off) select * from mj_cross, mj_real full join (select c1 from mj_empty where false) e on mj_real.c1 = e.c1;

-- Correctness: cross-check the ORCA result against the Postgres planner.
select count(*) as orca_count from mj_cross, mj_real full join (select c1 from mj_empty where false) e on mj_real.c1 = e.c1;
SET pg_orca.enable_orca = off;
select count(*) as planner_count from mj_cross, mj_real full join (select c1 from mj_empty where false) e on mj_real.c1 = e.c1;
SET pg_orca.enable_orca = on;

-- Same defect, independently triggered: a partitioned table with zero leaf
-- partitions is another way to make ORCA derive Universal for the "empty"
-- side (the original upstream bug report's shape).
create table mj_part (c1 int) partition by range (c1);
explain (costs off, timing off, summary off) select * from mj_cross, mj_real full join mj_part on mj_real.c1 = mj_part.c1;
select count(*) as part_count from mj_cross, mj_real full join mj_part on mj_real.c1 = mj_part.c1;

select enable_xform('CXformFullOuterJoin2HashJoin');

drop table mj_empty;
drop table mj_real;
drop table mj_cross;
drop table mj_part;
