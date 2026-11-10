-- A rolled back transaction must not take the rows of the following
-- statements with it: the pages of the rolled back transaction are restored
-- and the buffer pool has to forget them.
CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(4));
BEGIN;
INSERT INTO t VALUES (1, 'alpha', '[1, 0, 0, 0]');
INSERT INTO t VALUES (2, 'beta', '[0, 1, 0, 0]');
UPDATE t SET category = 'gamma' WHERE id = 1;
SELECT id, category FROM t ORDER BY id;
ROLLBACK;
SELECT id, category FROM t ORDER BY id;
INSERT INTO t VALUES (3, 'alpha', '[0, 0, 1, 0]');
SELECT id, category FROM t ORDER BY id;
BEGIN;
INSERT INTO t VALUES (4, 'beta', '[0, 0, 0, 1]');
ROLLBACK;
INSERT INTO t VALUES (5, 'beta', '[1, 1, 1, 1]');
SELECT id, category FROM t ORDER BY id;