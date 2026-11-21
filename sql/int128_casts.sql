-- int128: casts
CREATE EXTENSION IF NOT EXISTS int128;

SET extra_float_digits = 0;

--
-- Implicit casts from the integer types
--
SELECT 1::int2::int128, 1::int4::int128, 1::int8::int128;
SELECT 1::int2 + 1::int128, 1::int4 + 1::int128, 1::int8 + 1::int128;
SELECT 1::int128 = 1::int2, 1::int128 = 1::int4, 1::int128 = 1::int8;

--
-- numeric -> int128 (rounds half away from zero, like numeric -> int8)
--
SELECT 2.5::numeric::int128, (-2.5)::numeric::int128,
       3.5::numeric::int128, (-3.5)::numeric::int128,
       1.4::numeric::int128, (-1.4)::numeric::int128;
SELECT 0::numeric::int128, (-0.4)::numeric::int128;
SELECT 1e30::numeric::int128;
SELECT 1e40::numeric::int128;
SELECT 'NaN'::numeric::int128;
SELECT 'Infinity'::numeric::int128;
SELECT '-Infinity'::numeric::int128;

--
-- float8/float4 -> int128 (uses rint, like float8 -> int8)
--
SELECT 1.9::float8::int128, (-1.9)::float8::int128,
       1.5::float8::int128, 2.5::float8::int128, 3.5::float8::int128;
SELECT 1e30::float8::int128;
SELECT 1e40::float8::int128;
SELECT 'NaN'::float8::int128;
SELECT 'Infinity'::float8::int128;
SELECT 1.9::float4::int128, (-1.9)::float4::int128;

--
-- int128 -> numeric
--
SELECT 42::int128::numeric;
SELECT '170141183460469231731687303715884105727'::int128::numeric;
SELECT '-170141183460469231731687303715884105728'::int128::numeric;

--
-- int128 -> float8/float4
--
SELECT 42::int128::float8, 42::int128::float4;
SELECT '170141183460469231731687303715884105727'::int128::float8;

--
-- int128 -> smaller integer types
--
SELECT 123::int128::int2, 123::int128::int4, 123::int128::int8;
SELECT 40000::int128::int2;
SELECT 3000000000::int128::int4;
SELECT 9223372036854775808::int128::int8;
SELECT '-40000'::int128::int2;
SELECT '-3000000000'::int128::int4;
SELECT '-9223372036854775809'::int128::int8;

--
-- Assignment casts
--
CREATE TABLE int128_cast_test (v int128);
INSERT INTO int128_cast_test VALUES (1::int8), (2::int4), (3::int2), (4::numeric);
SELECT * FROM int128_cast_test ORDER BY v;
ALTER TABLE int128_cast_test ALTER COLUMN v TYPE int8;
SELECT * FROM int128_cast_test ORDER BY v;
DROP TABLE int128_cast_test;

--
-- Text I/O casts
--
SELECT '123'::text::int128;
SELECT 123::int128::text;
