CREATE TABLE edge (x VARCHAR, y VARCHAR);
COPY edge FROM 'edge.tsv' (DELIMITER '\t', HEADER false);

WITH RECURSIVE reach(x) AS (
	SELECT y FROM edge WHERE x = 'n0'
	UNION
	SELECT edge.y FROM reach JOIN edge ON reach.x = edge.x
)
SELECT x FROM reach;
