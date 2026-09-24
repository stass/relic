CREATE TABLE parent (p VARCHAR, c VARCHAR);
COPY parent FROM 'parent.tsv' (DELIMITER '\t', HEADER false);

WITH RECURSIVE sg(x, y) AS (
	SELECT c, c FROM parent
	UNION
	SELECT a.c, b.c
	FROM parent AS a
	JOIN sg ON a.p = sg.x
	JOIN parent AS b ON b.p = sg.y
)
SELECT y FROM sg WHERE x = 't2048';
