-- The order of a vector top-k query must be correct in both directions.
CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(2));
INSERT INTO t VALUES (1, 'alpha', '[1, 0]'), (2, 'beta', '[0, 1]'), (3, 'alpha', '[2, 2]'), (4, 'beta', '[3, 0]');
SELECT id, L2_DISTANCE(v, '[0, 0]') AS d FROM t ORDER BY d DESC;
SELECT id, L2_DISTANCE(v, '[0, 0]') AS d FROM t ORDER BY d DESC LIMIT 2;
SELECT id, L2_DISTANCE(v, '[0, 0]') AS d FROM t ORDER BY d LIMIT 2;
SELECT id, L2_DISTANCE(v, '[0, 0]') AS d FROM t WHERE category = 'beta' ORDER BY d DESC;
SELECT id, L2_DISTANCE(v, '[0, 0]') AS d FROM t WHERE category = 'alpha' ORDER BY d DESC LIMIT 10;
SELECT id FROM t WHERE category = 'alpha' ORDER BY id DESC LIMIT 10;