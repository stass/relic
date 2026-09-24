CREATE TABLE edge (x VARCHAR, y VARCHAR);
COPY edge FROM 'edge.tsv' (DELIMITER '\t', HEADER false);

WITH RECURSIVE path(x, y) AS (
	SELECT x, y FROM edge
	UNION
	SELECT edge.x, path.y FROM edge JOIN path ON edge.y = path.x
)
SELECT x, y FROM path;
