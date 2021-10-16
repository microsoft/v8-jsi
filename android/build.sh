#!/bin/bash

usage() {
  echo "usage: $0 [-p platform][--platform platform] [-f flavor][--flavor flavor]"
  echo "-p platform, --platform platform:     the platform to build for (platform options: x64 (default), x86, arm, arm64)"
  echo "-f flavor, --flavor flavor:           the build flavor (flavor options: debug (default), ship)"
  echo "-h, --help:                           this help message"
}

# defining params

BUILD_PLATFORM="x64" # default
BUILD_FLAVOR="debug"

while [ "${1:-}" != "" ]; do
  case "$1" in
    "-p" | "--platform")
      shift
      case "$1" in
        "x64" | "x86" | "arm" | "arm64")
          BUILD_PLATFORM="$1"
          ;;
        *)
          echo "Invalid option: $1"
          usage
          exit 1
          ;;
      esac
      ;;
    "-f" | "--flavor")
      shift
      case "$1" in
        "debug" | "ship")
          BUILD_FLAVOR="$1"
          ;;
        *)
          echo "Invalid option: $1"
          usage
          exit 1
          ;;
      esac
      ;;
    "-h" | "--help")
      usage
      exit 0
      ;;
    *)
      echo "Invalid flag: $1"
      usage
      exit 1
      ;;
  esac
  shift
done

echo "Building v8-jsi Android $BUILD_PLATFORM $BUILD_FLAVOR"

# cleaning output
rm -rf out

# install depot_tools
if [ ! -d build ]; then
  mkdir build
fi
cd build
echo "Installing depot_tools"
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=`pwd`/depot_tools:$PATH

# checkout the V8 codebase
source ../build.config
echo "Downloading codebase for V8 $V8_TAG"
fetch v8
cd v8/
git fetch --tags
git checkout tags/$V8_TAG

# install dependencies
echo "Installing dependencies for Android"
./build/install-build-deps-android.sh
echo "target_os = ['android']" >> ../.gclient
gclient sync

# apply build and source patches
echo "Applying patches"
ln -s ../.. jsi
pushd build && git apply < ../jsi/scripts/patch/build.patch
popd && git apply < jsi/scripts/patch/src.patch

# install Android NDK r21b
pushd third_party

# TODO: use a docker image for dependencies instead
if [ ! -f android_ndk_r21b/source.properties ]; then
  echo "Installing Android NDK r21b"
  curl -o android_ndk_r21b.zip https://dl.google.com/android/repository/android-ndk-r21b-linux-x86_64.zip
  unzip android_ndk_r21b.zip
  mv android-ndk-r21b android_ndk_r21b
  rm -rf android_ndk_r21b.zip
else
  echo "Android NDK installed"
fi
popd

# configure 
echo "Setting build configuration"
rm -rf out
config_args="target_os=\"android\" target_cpu=\"$BUILD_PLATFORM\" v8_enable_i18n_support=false v8_target_cpu=\"$BUILD_PLATFORM\" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root=\"//third_party/android_ndk_r21b\" android_ndk_version=\"r21b\" android_ndk_major_version=21"

if [ "$BUILD_FLAVOR" == "ship" ]; then
  config_args="${config_args} is_debug=false is_official_build=true"
else
  config_args="${config_args} is_debug=true"
fi

echo $config_args
echo "Building targets for $BUILD_PLATFORM $BUILD_FLAVOR"

gn gen out/$BUILD_PLATFORM/$BUILD_FLAVOR --args="$config_args"

# build

echo "Running build for $BUILD_PLATFORM $BUILD_FLAVOR"

ninja -C out/$BUILD_PLATFORM/$BUILD_FLAVOR v8jsi

# packaging

echo "Packaging"

mkdir -p jsi/out/android/headers/include/jsi
cp jsi/V8Runtime.h jsi/out/android/headers/include # headers
cp jsi/build.config jsi/out/android # config file
cp third_party/android_ndk_r21b/source.properties jsi/out/android/ndk_source.properties # ndk source.properties
cp jsi/jsi/{decorator.h,instrumentation.h,jsi-inl.h,jsi.h,JSIDynamic.h,jsilib.h,threadsafe.h} jsi/out/android/headers/include/jsi # jsi headers

mkdir -p jsi/out/android/lib
mkdir -p jsi/out/android/lib/$BUILD_PLATFORM/$BUILD_FLAVOR

# TODO: add unstripped lib after ADO pipeline works due to large size
cp -r out/$BUILD_PLATFORM/$BUILD_FLAVOR/{libv8jsi.so,args.gn} jsi/out/android/lib/$BUILD_PLATFORM/$BUILD_FLAVOR
