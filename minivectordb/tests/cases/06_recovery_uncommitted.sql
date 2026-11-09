CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(16), v VECTOR(2));
INSERT INTO t VALUES (1, 'one', '[1, 0]');
BEGIN;
INSERT INTO t VALUES (2, 'two', '[0, 1]');
-- !kill
SELECT id, name FROM t ORDER BY id;