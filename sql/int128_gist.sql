-- int128: GiST index support
CREATE EXTENSION IF NOT EXISTS int128;

CREATE TABLE int128_gist (v int128);
INSERT INTO int128_gist
SELECT g::int128 * 1000000000000000000 FROM generate_series(1, 5000) g;
INSERT INTO int128_gist VALUES (NULL);
CREATE INDEX int128_gist_idx ON int128_gist USING gist (v);
ANALYZE int128_gist;

SET enable_seqscan = off;

EXPLAIN (COSTS OFF)
SELECT * FROM int128_gist WHERE v = '500000000000000000000'::int128;
SELECT * FROM int128_gist WHERE v = '500000000000000000000'::int128;

EXPLAIN (COSTS OFF)
SELECT * FROM int128_gist WHERE v > '4990000000000000000000'::int128
ORDER BY v LIMIT 3;
SELECT * FROM int128_gist WHERE v > '4990000000000000000000'::int128
ORDER BY v LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM int128_gist
WHERE v <> '500000000000000000000'::int128;
SELECT count(*) FROM int128_gist
WHERE v <> '500000000000000000000'::int128;

RESET enable_seqscan;

-- index-only scan through the fetch support function
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT v FROM int128_gist WHERE v = '500000000000000000000'::int128;
SELECT v FROM int128_gist WHERE v = '500000000000000000000'::int128;
RESET enable_seqscan;

--
-- Extreme values
--
CREATE TABLE int128_gist_extreme (v int128);
INSERT INTO int128_gist_extreme VALUES
    ('-170141183460469231731687303715884105728'),
    ('170141183460469231731687303715884105727'),
    (0), (-1), (1), (NULL);
CREATE INDEX int128_gist_extreme_idx ON int128_gist_extreme USING gist (v);

SET enable_seqscan = off;
SELECT * FROM int128_gist_extreme WHERE v = 0::int128;
SELECT * FROM int128_gist_extreme WHERE v < 0::int128 ORDER BY v;
SELECT * FROM int128_gist_extreme WHERE v > 0::int128 ORDER BY v;
RESET enable_seqscan;

--
-- KNN ordering
--
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT v FROM int128_gist WHERE v IS NOT NULL
ORDER BY v <-> '500300000000000000000'::int128 LIMIT 3;
SELECT v FROM int128_gist WHERE v IS NOT NULL
ORDER BY v <-> '500300000000000000000'::int128 LIMIT 3;
RESET enable_seqscan;

--
-- Exclusion constraint
--
CREATE TABLE int128_gist_excl (v int128, EXCLUDE USING gist (v WITH =));
INSERT INTO int128_gist_excl VALUES (1);
INSERT INTO int128_gist_excl VALUES (1);
INSERT INTO int128_gist_excl VALUES (2);
SELECT * FROM int128_gist_excl ORDER BY v;

DROP TABLE int128_gist, int128_gist_extreme, int128_gist_excl;
