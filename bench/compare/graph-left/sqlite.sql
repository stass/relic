CREATE TABLE edge (x TEXT, y TEXT);
.import edge.tsv edge

WITH RECURSIVE "from"(x, y) AS (
	SELECT x, y FROM edge
	UNION
	SELECT "from".x, edge.y FROM "from" JOIN edge ON "from".y = edge.x
)
SELECT y FROM "from" WHERE x = 'n17';
