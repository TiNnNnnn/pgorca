-- pg_orca ordered-set aggregate regression test
-- Ported from Apache Cloudberry src/test/regress/sql/percentile.sql
-- (commit b1e80d9 "fix(orca): avoid crash on ordered-set aggregate without
-- direct args")

LOAD 'pg_orca';
SET pg_orca.enable_orca = on;
SET client_min_messages = warning;

CREATE TABLE perct (a int, b int);
INSERT INTO perct SELECT i, i / 10 FROM generate_series(1, 100) i;
ANALYZE perct;

-- mode() is the only built-in ordered-set aggregate with no direct argument.
-- CUtils::FHasOrderedAggToSplit used to dereference the (empty) direct-args
-- child unconditionally, which tripped a GPOS_ASSERT in Debug builds and
-- segfaulted in Release builds during ORCA preprocessing.  ORCA must plan
-- mode() as a plain ordered-set aggregate instead of trying to split it into
-- the gp_percentile rewrite.
explain (costs off) select mode() within group (order by b) from perct;
select mode() within group (order by b) from perct;
select b, mode() within group (order by a) from perct group by b order by b;

-- Mix mode() with a splittable percentile and a regular aggregate.
select mode() within group (order by b),
	percentile_cont(0.6) within group (order by a), count(*) from perct;
select b, mode() within group (order by a),
	percentile_cont(0.5) within group (order by a), count(*) from perct group by b order by b;

-- mode() over empty input, over a grouped key, and with a filter.
select mode() within group (order by a) from perct where a < 0;
select a % 2 as parity, mode() within group (order by b) from perct group by a % 2 order by parity;
select mode() within group (order by a) filter (where b = 3) from perct;

-- mode() over a text column (no direct arg, non-numeric sort key).
create table perct_text (a int, b text);
insert into perct_text select i, 'v' || (i % 7) from generate_series(1, 50) i;
analyze perct_text;
select mode() within group (order by b) from perct_text;
select a % 3 as g, mode() within group (order by b) from perct_text group by a % 3 order by g;

drop table perct_text;
drop table perct;
