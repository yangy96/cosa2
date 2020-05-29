#!/bin/bash

if [ "$TRAVIS_OS_NAME" == "osx" ]; then
    ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
    export PATH=/usr/local/bin:/usr/local/sbin:$PATH
    brew install gnu-getopt gmp flex
    brew upgrade flex
    export PATH="/usr/local/opt/flex/bin:$PATH"
    export LDFLAGS="-L/usr/local/opt/flex/lib"
    export CPPFLAGS="-I/usr/local/opt/flex/include"
    # HACK: remove old version of FlexLexer.h that shows up in include path first
    # see comments here: https://github.com/vmware/cascade/issues/170
    sudo rm /Library/Developer/CommandLineTools/usr/include/Flex*
    sudo rm /Applications/Xcode-9.2.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/include/Flex*
else
    echo "NOT in OSX -- nothing to do"
fi
