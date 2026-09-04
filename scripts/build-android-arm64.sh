#!/bin/bash
set -e

git submodule init
git submodule update

curl -L -o android-ndk-r27d-linux.zip \
  https://dl.google.com/android/repository/android-ndk-r27d-linux.zip
unzip -q android-ndk-r27d-linux.zip
export ANDROID_NDK_HOME="$PWD/android-ndk-r27d"
export NDK_HOME="$PWD/android-ndk-r27d"

./waf configure \
    configure -T release \
    --build-games=csso \
    --togles \
    --android=aarch64,host,21 \
    --prefix=./output \
    --disable-warns \

./waf build -j$(nproc)
