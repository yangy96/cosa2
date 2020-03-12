#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
DEPS=$DIR/../deps

mkdir -p $DEPS

if [ -d "$DEPS/bison" ]; then
    echo "It appears bison has already been downloaded to $DEPS/bison"
    echo "If you'd like to rebuild, please delete it and run this script again"
    exit 1
fi

curl http://ftp.gnu.org/gnu/bison/bison-3.5.tar.xz --output $DEPS/bison-3.5.tar.xz

if [ ! -f "$DEPS/bison-3.5.tar.xz" ]; then
    echo "It seems like downloading bison to $DEPS/bison-3.5.tar.xz failed"
    exit 1
fi

cd $DEPS
tar -xf bison-3.5.tar.xz
rm bison-3.5.tar.xz
mv ./bison-3.5 ./bison
cd bison
mkdir install
./configure --prefix $DEPS/bison/install --exec-prefix $DEPS/bison/install
make -j$(nproc)
make install
cd $DIR

if [ ! -f "$DEPS/bison/install/bin/bison" ]; then
    echo "It seems like installing bison to $DEPS/bison/install failed"
    exit 1
fi