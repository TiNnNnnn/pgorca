-- A partitioned child must retain its external statistics context even when
-- it has no outer references: partition consumers are an independent guard.
SELECT p.k
FROM dsl_budget_partitioned AS p
JOIN dsl_eq_left AS l ON p.k = l.k
ORDER BY p.k;
