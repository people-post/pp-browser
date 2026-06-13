#!/bin/sh -e

LSHPACK=src/lshpack
mkdir -p $LSHPACK-copy
cp \
  $LSHPACK/huff-tables.h \
  $LSHPACK/lshpack.c \
  $LSHPACK/lshpack.h \
  $LSHPACK/lsxpack_header.h \
    $LSHPACK-copy/.

LSQPACK=src/liblsquic/ls-qpack
mkdir -p $LSQPACK-copy
cp \
  $LSQPACK/huff-tables.h \
  $LSQPACK/lsqpack.c \
  $LSQPACK/lsqpack.h \
  $LSQPACK/lsxpack_header.h \
    $LSQPACK-copy/.
