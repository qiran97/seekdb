-- A transaction can create and drop tables.  A rollback has to remove the
-- table that was created and restore the table that was dropped.
CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(4));
INSERT INTO t VALUES (1, 'alpha', '[1, 0, 0, 0]');
BEGIN;
CREATE TABLE u (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(4));
INSERT INTO u VALUES (5, 'beta', '[0, 1, 0, 0]');
SELECT id, category FROM u ORDER BY id;
ROLLBACK;
SELECT id, category FROM u ORDER BY id;
SELECT id, category FROM t ORDER BY id;
BEGIN;
DROP TABLE t;
SELECT id, category FROM t ORDER BY id;
ROLLBACK;
SELECT id, category FROM t ORDER BY id;
SET search_mode = 'ann';
SELECT id, L2_DISTANCE(v, '[1, 0, 0, 0]') AS d FROM t ORDER BY d LIMIT 1;
SET search_mode = 'exact';
SELECT id, L2_DISTANCE(v, '[1, 0, 0, 0]') AS d FROM t ORDER BY d LIMIT 1;