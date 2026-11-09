CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(4));
INSERT INTO t VALUES (1, 'alpha', '[1,0,0,0]');
BEGIN;
INSERT INTO t VALUES (8, 'gamma', '[1,0,0,0]');
BEGIN;
ROLLBACK;
SELECT id, category FROM t ORDER BY id;