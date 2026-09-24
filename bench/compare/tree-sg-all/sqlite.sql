CREATE TABLE parent (p TEXT, c TEXT);
.import parent.tsv parent

WITH RECURSIVE sg(x, y) AS (
	SELECT c, c FROM parent
	UNION
	SELECT a.c, b.c
	FROM parent AS a
	JOIN sg ON a.p = sg.x
	JOIN parent AS b ON b.p = sg.y
)
SELECT x, y FROM sg;
