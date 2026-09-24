CREATE TABLE edge (x TEXT, y TEXT);
.import edge.tsv edge

WITH RECURSIVE path(x, y) AS (
	SELECT x, y FROM edge
	UNION
	SELECT edge.x, path.y FROM edge JOIN path ON edge.y = path.x
)
SELECT y FROM path WHERE x = 'n0';
