CREATE TABLE depends (x TEXT, y TEXT);
CREATE TABLE done (x TEXT);
CREATE TABLE task (x TEXT);
.import depends.tsv depends
.import done.tsv done
.import task.tsv task

WITH RECURSIVE waits(x, y) AS (
	SELECT x, y FROM depends
	WHERE NOT EXISTS (SELECT 1 FROM done WHERE done.x = depends.y)
	UNION
	SELECT depends.x, waits.y FROM depends JOIN waits ON depends.y = waits.x
),
blocked(x) AS (
	SELECT DISTINCT x FROM waits
),
ready(x) AS (
	SELECT x FROM task
	WHERE NOT EXISTS (SELECT 1 FROM done WHERE done.x = task.x)
	AND NOT EXISTS (SELECT 1 FROM blocked WHERE blocked.x = task.x)
)
SELECT x FROM ready;
