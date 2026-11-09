CREATE TABLE items (id BIGINT PRIMARY KEY, label VARCHAR(32), vec VECTOR(8));
INSERT INTO items VALUES
    (10, 'a', '[1, 0, 0, 0, 0, 0, 0, 0]'),
    (20, 'b', '[0, 1, 0, 0, 0, 0, 0, 0]'),
    (30, 'a', '[0.9, 0.1, 0, 0, 0, 0, 0, 0]'),
    (40, 'c', '[0, 0, 1, 0, 0, 0, 0, 0]');
SELECT id, L2_DISTANCE(vec, '[0, 0, 0, 0, 0, 0, 0, 0]') AS distance FROM items ORDER BY distance;
SELECT id, L2_DISTANCE(vec, '[0, 0, 0, 0, 0, 0, 0, 0]') AS distance FROM items ORDER BY distance LIMIT 2;
SELECT id, L2_DISTANCE(vec, '[1, 0, 0, 0, 0, 0, 0, 0]') AS distance FROM items WHERE label = 'a' ORDER BY distance LIMIT 1;
SELECT id FROM items WHERE L2_DISTANCE(vec, '[1, 0, 0, 0, 0, 0, 0, 0]') < 0.5 ORDER BY id;
SELECT id, L2_DISTANCE(vec, '[1, 0, 0, 0, 0, 0, 0, 0]') AS d FROM items ORDER BY d DESC LIMIT 3;