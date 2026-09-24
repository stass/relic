CREATE TABLE par (x INTEGER, y INTEGER);
.import par.tsv par

WITH RECURSIVE tc(x, y) AS (
	SELECT x, y FROM par
	UNION
	SELECT par.x, tc.y FROM par JOIN tc ON par.y = tc.x
)
SELECT x, y FROM tc;
