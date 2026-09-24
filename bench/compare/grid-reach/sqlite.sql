CREATE TABLE edge (x TEXT, y TEXT);
.import edge.tsv edge

WITH RECURSIVE reach(x) AS (
	SELECT y FROM edge WHERE x = 'c0_0'
	UNION
	SELECT edge.y FROM reach JOIN edge ON reach.x = edge.x
)
SELECT x FROM reach;
