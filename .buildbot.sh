#!/bin/sh
#
# Build script for continuous integration.

set -e
mkdir out && cd out
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF -Denable_parallel_mark=ON -Dbuild_tests=ON -Denable_gc_assertions=ON ../
ninja && ninja test
