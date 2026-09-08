-- pg_orca extended-statistics regression tests

LOAD 'pg_orca';
SET pg_orca.enable_orca = on;
SET client_min_messages = warning;

-- Ported from warehouse-pg PR #251.
--
-- Filter estimation with extended statistics must not crash when the memo
-- group's cached stats were extended incrementally (histograms appended for
-- columns that the colid -> attno mapping initially lacked). The ROLLUP is
-- expanded into a CTE whose consumers request different column sets, so the
-- stats of the dimension table's scan group get appended with the filter
-- column after the initial derivation.
DROP TABLE IF EXISTS extstats_fact, extstats_dim;
CREATE TABLE extstats_dim (d_sk int, d_filter int, d_a int, d_b int, d_c int);
CREATE TABLE extstats_fact (f_sk int, f_val int);
INSERT INTO extstats_dim SELECT g, g % 20, g % 3, g % 4, g % 5 FROM generate_series(1, 100) g;
INSERT INTO extstats_fact SELECT g % 100 + 1, g FROM generate_series(1, 1000) g;
CREATE STATISTICS extstats_dim_nd (ndistinct) ON d_a, d_b, d_c FROM extstats_dim;
ANALYZE extstats_dim;
ANALYZE extstats_fact;

SELECT count(*) FROM (
  SELECT d_a, d_b, d_c, sum(f_val) s
  FROM extstats_fact, extstats_dim
  WHERE f_sk = d_sk AND d_filter BETWEEN 5 AND 10
  GROUP BY ROLLUP(d_a, d_b, d_c)) x;

-- same query, but with a dependencies object so the filter goes through the
-- functional-dependency path of the extended-stats estimator as well
CREATE STATISTICS extstats_dim_dep (dependencies) ON d_filter, d_a, d_b FROM extstats_dim;
ANALYZE extstats_dim;

SELECT count(*) FROM (
  SELECT d_a, d_b, d_c, sum(f_val) s
  FROM extstats_fact, extstats_dim
  WHERE f_sk = d_sk AND d_filter = 7 AND d_a = 1 AND d_b = 3
  GROUP BY ROLLUP(d_a, d_b, d_c)) x;

DROP TABLE extstats_fact, extstats_dim;

-- ---------------------------------------------------------------------------
-- Functional-dependency selectivity
--
-- Deterministic data: fd_a has 100 equally frequent values, so P(fd_a = 1) is
-- 0.01; fd_b repeats fd_a on half the rows and is 999 on the other half, so
-- the dependency fd_b => fd_a has a degree of validity of 0.5, P(fd_b = 1) is
-- 0.005 and P(fd_b = 999) is 0.5.
--
-- The estimate must apply the backend's formula
--
--     P(a,b) = f * Min(P(a), P(b)) + (1-f) * P(a) * P(b)
--
-- i.e. the clause on the implied column must neither vanish from the estimate
-- nor push the conjunction above the more selective of the two clauses.
-- ---------------------------------------------------------------------------
DROP TABLE IF EXISTS extstats_fd;
CREATE TABLE extstats_fd (fd_a int, fd_b int);
INSERT INTO extstats_fd
  SELECT g % 100, CASE WHEN (g / 100) % 2 = 0 THEN g % 100 ELSE 999 END
  FROM generate_series(1, 10000) g;
CREATE STATISTICS extstats_fd_dep (dependencies) ON fd_a, fd_b FROM extstats_fd;
ANALYZE extstats_fd;

CREATE FUNCTION extstats_est(query text) RETURNS bigint AS $$
DECLARE
  plan json;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || query INTO plan;
  RETURN (plan -> 0 -> 'Plan' ->> 'Plan Rows')::numeric::bigint;
END;
$$ LANGUAGE plpgsql;

-- degree of validity of the dependency actually used below
SELECT dependencies::text FROM pg_stats_ext
WHERE statistics_name = 'extstats_fd_dep';

-- P(implying) = P(fd_b = 1) = 0.005 <= P(implied) = P(fd_a = 1) = 0.01, so
-- P(fd_a|fd_b) = 0.5 + 0.5 * 0.01 = 0.505 and the estimate is
-- 10000 * 0.005 * 0.505 = 25.25
SELECT extstats_est('SELECT * FROM extstats_fd WHERE fd_a = 1 AND fd_b = 1');

-- P(implying) = P(fd_b = 999) = 0.5 > P(implied) = 0.01, so the Min() branch
-- applies: P(fd_a|fd_b) = 0.5 * 0.01 / 0.5 + 0.5 * 0.01 = 0.015 and the
-- estimate is 10000 * 0.5 * 0.015 = 75. Without the Min() the bracket would
-- be 0.505 and the estimate 2525 -- above the 50 rows that fd_a = 1 alone
-- leaves after the fd_b filter, and worse than having no statistics object.
SELECT extstats_est('SELECT * FROM extstats_fd WHERE fd_a = 1 AND fd_b = 999');

-- A range predicate says nothing about the implied column and must not be
-- matched to the dependency: the estimate has to be the same as the one the
-- per-column path produces with no statistics object at all.
CREATE TEMP TABLE extstats_range_est AS
SELECT extstats_est('SELECT * FROM extstats_fd WHERE fd_a = 1 AND fd_b > 500') AS with_dep;
DROP STATISTICS extstats_fd_dep;
SELECT with_dep
         = extstats_est('SELECT * FROM extstats_fd WHERE fd_a = 1 AND fd_b > 500')
       AS range_pred_ignores_dependency
FROM extstats_range_est;
CREATE STATISTICS extstats_fd_dep (dependencies) ON fd_a, fd_b FROM extstats_fd;
ANALYZE extstats_fd;
DROP TABLE extstats_range_est;

-- IS NULL is an equality against a null constant and must not be matched
-- either; no row has a null fd_b, so the estimate stays at the floor
SELECT extstats_est('SELECT * FROM extstats_fd WHERE fd_a = 1 AND fd_b IS NULL');

-- a second equality on the implied column must not be counted twice
SELECT extstats_est('SELECT * FROM extstats_fd WHERE fd_a = 1 AND fd_a = 1 AND fd_b = 999');

-- the implied column keeps its post-filter distribution: after fd_a = 1 the
-- group-by over fd_a sees a single group
SELECT extstats_est('SELECT fd_a FROM extstats_fd WHERE fd_a = 1 AND fd_b = 999 GROUP BY fd_a');

-- with no pg_statistic row for the implied column (ALTER COLUMN TYPE keeps the
-- dependency statistics but drops the column statistics) the clause must get
-- the default selectivity, not "matches nothing"
ALTER TABLE extstats_fd ALTER COLUMN fd_a TYPE bigint;
SELECT count(*) FROM pg_stats WHERE tablename = 'extstats_fd' AND attname = 'fd_a';
SELECT extstats_est('SELECT * FROM extstats_fd WHERE fd_a = 1 AND fd_b = 999') > 1
       AS implied_without_stats_not_empty;

DROP FUNCTION extstats_est(text);
DROP TABLE extstats_fd;
