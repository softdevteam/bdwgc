#!/bin/sh
#
# Build script for continuous integration.

set -e
mkdir out && cd out
cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF -Denable_parallel_mark=ON -Dbuild_tests=ON -Denable_gc_assertions=ON ../
make -j
ctest

# Build alloy and test it against our BDWGC fork

cd ../

export CARGO_HOME="`pwd`/.cargo"
export RUSTUP_HOME="`pwd`/.rustup"

curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs > rustup.sh
sh rustup.sh --default-host x86_64-unknown-linux-gnu \
    --default-toolchain nightly \
    --no-modify-path \
    --profile minimal \
    -y
export PATH=`pwd`/.cargo/bin/:$PATH
