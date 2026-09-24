CREATE TABLE edge (x VARCHAR, y VARCHAR);
COPY edge FROM 'edge.tsv' (DELIMITER '\t', HEADER false);

WITH RECURSIVE "from"(x, y) AS (
	SELECT x, y FROM edge
	UNION
	SELECT "from".x, edge.y FROM "from" JOIN edge ON "from".y = edge.x
)
SELECT y FROM "from" WHERE x = 'n17';
