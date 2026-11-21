# int128/Makefile

EXTENSION = int128
MODULE_big = int128
OBJS = src/int128.o
DATA = int128--1.0.sql
PGFILEDESC = "int128 - 128-bit signed integer type"

REGRESS = int128 int128_casts int128_index int128_gist int128_spgist int128_gin int128_math

SHLIB_LINK += -lm

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
