CREATE TABLE par (x BIGINT, y BIGINT);
COPY par FROM 'par.tsv' (DELIMITER '\t', HEADER false);

WITH RECURSIVE tc(x, y) AS (
	SELECT x, y FROM par
	UNION
	SELECT par.x, tc.y FROM par JOIN tc ON par.y = tc.x
)
SELECT y FROM tc WHERE x = 500;
