CREATE TABLE par (x INTEGER, y INTEGER);
.import par.tsv par

WITH RECURSIVE sg(x, y) AS (
	SELECT y, y FROM par
	UNION
	SELECT a.x, b.x
	FROM par AS a
	JOIN sg ON a.y = sg.x
	JOIN par AS b ON b.y = sg.y
)
SELECT y FROM sg WHERE x = 500;
