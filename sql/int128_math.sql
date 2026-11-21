-- int128: mathematical and statistical functions
CREATE EXTENSION IF NOT EXISTS int128;

SET extra_float_digits = 0;

--
-- Rounding and truncation
--
SELECT round(12345::int128, -2), round(12355::int128, -2),
       round((-12355)::int128, -2), round(123::int128, 2);
SELECT trunc(12355::int128, -2), trunc((-12355)::int128, -2),
       trunc(123::int128, 2);
SELECT round(123::int128), trunc(123::int128), ceil(123::int128),
       ceiling(123::int128), floor(123::int128);
SELECT sign((-5)::int128), sign(0::int128), sign(5::int128);
SELECT round(5::int128, -1), round((-5)::int128, -1),
       round(4::int128, -1), round((-4)::int128, -1);
SELECT round('170141183460469231731687303715884105727'::int128, -1);
SELECT round('-170141183460469231731687303715884105728'::int128, -1);
SELECT trunc('-170141183460469231731687303715884105728'::int128, -1);

--
-- Powers, roots and logarithms
--
SELECT power(2::int128, 10::int128), pow(2::int128, 10::int128),
       power(2::int128, (-1)::int128);
SELECT sqrt(144::int128), sqrt(2::int128);
SELECT cbrt(27::int128);
SELECT exp(1::int128), ln(1::int128), log(100::int128),
       log10(1000::int128), log(2::int128, 8::int128);
SELECT factorial(5::int128), factorial(0::int128);
SELECT factorial((-1)::int128);

--
-- Trigonometric functions
--
SELECT sin(0::int128), cos(0::int128), tan(0::int128), cot(1::int128);
SELECT asin(1::int128), acos(1::int128), atan(1::int128),
       atan2(1::int128, 1::int128);
SELECT sinh(0::int128), cosh(0::int128), tanh(0::int128),
       asinh(0::int128), acosh(1::int128), atanh(0::int128);
SELECT degrees(1::int128), radians(180::int128);

--
-- Integer utilities
--
SELECT bit_count(0::int128), bit_count(1::int128),
       bit_count(255::int128), bit_count((-1)::int128);
SELECT width_bucket(5::int128, 0::int128, 10::int128, 2),
       width_bucket((-1)::int128, 0::int128, 10::int128, 2),
       width_bucket(10::int128, 0::int128, 10::int128, 2);
SELECT width_bucket(0::int128, 0::int128, 10::int128, 0);
SELECT width_bucket(0::int128, 10::int128, 10::int128, 2);
SELECT random(5::int128, 5::int128);
SELECT random(1::int128, 10::int128) BETWEEN 1 AND 10 AS in_range;
SELECT random(10::int128, 1::int128);
SELECT to_char(12345::int128, 'FM999,999');

--
-- Series and utility functions
--
SELECT generate_series(1::int128, 5::int128);
SELECT generate_series(1::int128, 10::int128, 3::int128);
SELECT generate_series(5::int128, 1::int128, (-2)::int128);
SELECT generate_series(1::int128, 5::int128, 0::int128);
SELECT scale(123::int128), min_scale(123::int128), trim_scale(123::int128);
SELECT int128_inc(5::int128), int128_dec(5::int128);
SELECT int128_inc('170141183460469231731687303715884105727'::int128);
SELECT int128_dec('-170141183460469231731687303715884105728'::int128);
SELECT pg_size_pretty(1024::int128), pg_size_pretty(1048576::int128);
SELECT int128_dist(10::int128, 3::int128), 10::int128 <-> 3::int128;

--
-- Statistical aggregates
--
SELECT stddev_samp(v), stddev_pop(v), var_samp(v), var_pop(v)
FROM (VALUES (1::int128), (2::int128), (3::int128), (4::int128)) s(v);
SELECT stddev_samp(v), stddev_pop(v), var_samp(v), var_pop(v)
FROM (VALUES (1::int128)) s(v);
SELECT stddev_samp(v), stddev_pop(v), var_samp(v), var_pop(v)
FROM (VALUES (1::int128), (NULL)) s(v);
SELECT stddev_samp(v), stddev_pop(v), var_samp(v), var_pop(v)
FROM (VALUES (1::int128)) s(v) WHERE false;
