-- @options: cache-budget=1024 buffer-pool=65536
CREATE TABLE small (id BIGINT PRIMARY KEY, category VARCHAR(16), embedding VECTOR(8));
INSERT INTO small VALUES
    (1, 'a', '[1, 0, 0, 0, 0, 0, 0, 0]'),
    (2, 'b', '[0, 1, 0, 0, 0, 0, 0, 0]'),
    (3, 'a', '[0.9, 0.1, 0, 0, 0, 0, 0, 0]');
SELECT id, L2_DISTANCE(embedding, '[1, 0, 0, 0, 0, 0, 0, 0]') AS d FROM small ORDER BY d;
SELECT id FROM small WHERE category = 'a' ORDER BY id;
UPDATE small SET embedding = '[0, 0, 1, 0, 0, 0, 0, 0]' WHERE id = 1;
SELECT id, L2_DISTANCE(embedding, '[1, 0, 0, 0, 0, 0, 0, 0]') AS d FROM small ORDER BY d;
DELETE FROM small WHERE id = 2;
SELECT id FROM small ORDER BY id;
-- !stop
SELECT id FROM small ORDER BY id;
