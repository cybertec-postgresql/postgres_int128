-- int128: type I/O, arithmetic, comparison, bitwise, aggregates
CREATE EXTENSION IF NOT EXISTS int128;

--
-- Input/output and range
--
SELECT '0'::int128, '1'::int128, '-1'::int128;
SELECT '170141183460469231731687303715884105727'::int128 AS max;
SELECT '-170141183460469231731687303715884105728'::int128 AS min;
SELECT ' 42 '::int128, '+42'::int128, '-0'::int128;
SELECT '170141183460469231731687303715884105728'::int128;
SELECT '-170141183460469231731687303715884105729'::int128;
SELECT 'abc'::int128;
SELECT ''::int128;
SELECT '1.5'::int128;
SELECT '1e10'::int128;

--
-- Type properties
--
SELECT pg_typeof(1::int128), pg_typeof(1::int128 + 1);
SELECT typlen, typbyval, typalign, typstorage, typcategory
FROM pg_type WHERE typname = 'int128';

--
-- Arithmetic
--
SELECT 1::int128 + 2, 1::int128 - 2, 2::int128 * 3, 7::int128 / 2, 7::int128 % 2;
SELECT (-7::int128) / 2, (-7::int128) % 2;
SELECT -5::int128, abs(-5::int128), @(-5::int128);
SELECT div(7::int128, 2), mod(7::int128, 2);
SELECT gcd(12::int128, 18), lcm(4::int128, 6);
SELECT gcd(0::int128, 5), lcm(0::int128, 5);
SELECT gcd(-12::int128, 18), lcm(-4::int128, 6);

-- overflow
SELECT '170141183460469231731687303715884105727'::int128 + 1;
SELECT '-170141183460469231731687303715884105728'::int128 - 1;
SELECT '170141183460469231731687303715884105727'::int128 * 2;
SELECT '-170141183460469231731687303715884105728'::int128 / -1;
SELECT '-170141183460469231731687303715884105728'::int128 % -1;
SELECT abs('-170141183460469231731687303715884105728'::int128);
SELECT -('-170141183460469231731687303715884105728'::int128);
SELECT 1::int128 / 0;
SELECT 1::int128 % 0;

--
-- Bitwise operators
--
SELECT 12::int128 & 10, 12::int128 | 10, 12::int128 # 10, ~0::int128;
SELECT 1::int128 << 10, 1024::int128 >> 3, (-8::int128) >> 1;
SELECT 1::int128 << 127, 1::int128 << 128, 1::int128 << -1;
SELECT (-1::int128) >> 127, (-1::int128) >> 128, (-1::int128) >> -1;

--
-- Comparison operators
--
SELECT 1::int128 = 1, 1::int128 <> 2, 1::int128 < 2, 2::int128 <= 2,
       3::int128 > 2, 3::int128 >= 3;
SELECT int128_cmp(1::int128, 2::int128),
       int128_cmp(2::int128, 2::int128),
       int128_cmp(3::int128, 2::int128);

--
-- Sorting
--
SELECT v FROM (VALUES (3::int128), (-1::int128), (2::int128), (0::int128)) s(v)
ORDER BY v;
SELECT v FROM (VALUES (3::int128), (-1::int128), (2::int128), (0::int128)) s(v)
ORDER BY v DESC;

--
-- Hashing
--
SELECT int128_hash('0'::int128) = int128_hash('0'::int128) AS same,
       int128_hash('1'::int128) <> int128_hash('2'::int128) AS different;
SELECT int128_hash_extended('0'::int128, 0) = int128_hash_extended('0'::int128, 0) AS same,
       int128_hash_extended('0'::int128, 0) <> int128_hash_extended('0'::int128, 1) AS different;

--
-- Binary send format
--
SELECT int128_send('1'::int128);
SELECT int128_send('-1'::int128);
SELECT int128_send('170141183460469231731687303715884105727'::int128);

--
-- Aggregates
--
CREATE TABLE int128_agg (v int128);
INSERT INTO int128_agg VALUES (1), (2), (3), (NULL), (-4);
SELECT count(v), sum(v), avg(v), min(v), max(v) FROM int128_agg;
SELECT sum(v), avg(v), min(v), max(v) FROM int128_agg WHERE false;
SELECT sum(v) FROM (VALUES ('170141183460469231731687303715884105727'::int128),
                           (1::int128)) s(v);
SELECT avg(v) FROM (VALUES (1::int128), (2::int128)) s(v);
SELECT avg(v) FROM (VALUES (-1::int128), (-2::int128)) s(v);
DROP TABLE int128_agg;

--
-- Window functions (in_range support)
--
SELECT v, count(*) OVER (ORDER BY v RANGE BETWEEN 2 PRECEDING AND CURRENT ROW)
FROM (VALUES (1::int128), (2::int128), (5::int128), (6::int128), (10::int128)) s(v)
ORDER BY v;
SELECT v, count(*) OVER (ORDER BY v RANGE BETWEEN CURRENT ROW AND 2 FOLLOWING)
FROM (VALUES (1::int128), (2::int128), (5::int128), (6::int128), (10::int128)) s(v)
ORDER BY v;
SELECT v, count(*) OVER (ORDER BY v RANGE BETWEEN 2 PRECEDING AND 2 FOLLOWING)
FROM (VALUES (1::int128), (2::int128), (5::int128), (6::int128), (10::int128)) s(v)
ORDER BY v;

--
-- Type integration
--
SELECT v, count(*) FROM (VALUES (1::int128), (1::int128), (2::int128)) s(v)
GROUP BY v ORDER BY v;
SELECT DISTINCT v FROM (VALUES (1::int128), (1::int128), (2::int128)) s(v)
ORDER BY v;
SELECT ARRAY[1::int128, 2::int128] @> ARRAY[2::int128];
SELECT (1::int128, 2::int128) < (1::int128, 3::int128);
CREATE DOMAIN int128_pos AS int128 CHECK (VALUE > 0);
SELECT 5::int128_pos;
SELECT (-5)::int128_pos;
DROP DOMAIN int128_pos;
COPY (SELECT v FROM (VALUES (1::int128), (-2::int128), (3::int128)) s(v)
      ORDER BY v) TO STDOUT;

--
-- Operator class validation
--
SELECT opcname, amvalidate(oid) FROM pg_opclass
WHERE opcname LIKE 'int128%' ORDER BY 1;

--
-- Range type support
--
SELECT '[1,10)'::int128range @> 5::int128;
SELECT int128range(1::int128, 10::int128);
SELECT lower('[1,10)'::int128range), upper('[1,10)'::int128range);
SELECT '[1,10)'::int128range && '[5,15)'::int128range;
SELECT '{[1,10)}'::int128multirange;
CREATE TABLE int128_range_test (r int128range);
INSERT INTO int128_range_test VALUES ('[1,10)'), ('[5,15)');
CREATE INDEX int128_range_test_idx ON int128_range_test USING gist (r);
SELECT * FROM int128_range_test WHERE r && '[7,8)'::int128range ORDER BY r;
DROP TABLE int128_range_test;
