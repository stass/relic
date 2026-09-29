CREATE TABLE par (x BIGINT, y BIGINT);
COPY par FROM 'par.tsv' (DELIMITER '\t', HEADER false);

WITH RECURSIVE dist(x, y, d) AS (
	SELECT x, y, 1 FROM par
	UNION
	SELECT dist.x, par.y, dist.d + 1 FROM dist JOIN par ON dist.y = par.x
	WHERE dist.d < 5
)
SELECT x, y, d FROM dist;
