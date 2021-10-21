#!/bin/bash

# defining params

BUILD_PLATFORM="x64" # default
BUILD_FLAVOR="debug"
SOURCE_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
SOURCES_PATH="$SOURCE_DIR" # this won't be supplied by the user; should be android folder
OUTPUT_PATH="$SOURCE_DIR/out"

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
        "debug" | "release")
          BUILD_FLAVOR="$1"
          ;;
        *)
          echo "Invalid option: $1"
          usage
          exit 1
          ;;
      esac
      ;;
    "-s" | "--sourcesPath")
      shift
      SOURCES_PATH="$1"
      ;;
    "-o" | "--outputPath")
      shift
      OUTPUT_PATH="$1"
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

BUILD_PATH="$SOURCES_PATH/build"

echo "Building v8-jsi Android $BUILD_PLATFORM $BUILD_FLAVOR"

# cleaning output
rm -rf $OUTPUT_PATH/*

# install depot_tools
if [ ! -d $BUILD_PATH ]; then
  mkdir $BUILD_PATH
fi
cd $BUILD_PATH
echo "Installing depot_tools"
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=$BUILD_PATH/depot_tools:$PATH

# checkout the V8 codebase
source $SOURCES_PATH/build.config
echo "Downloading codebase for V8 $V8_TAG"
fetch v8
cd v8/
git fetch --tags
git checkout tags/$V8_TAG

# install dependencies
echo "Installing dependencies for Android"
sudo ./build/install-build-deps-android.sh # since this requires password
echo "target_os = ['android']" >> ../.gclient
gclient sync

# apply build and source patches
echo "Applying patches"
pushd build && git apply < $SOURCES_PATH/scripts/patch/build.patch
popd && git apply < $SOURCES_PATH/scripts/patch/src.patch

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

BUILD_OUTPUT_PATH="$BUILD_PATH/v8/out/$BUILD_PLATFORM/$BUILD_FLAVOR"
echo "Setting build configuration"
rm -rf $BUILD_PATH/v8/out/*
config_args="target_os=\"android\" target_cpu=\"$BUILD_PLATFORM\" v8_enable_i18n_support=false v8_target_cpu=\"$BUILD_PLATFORM\" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root=\"//third_party/android_ndk_r21b\" android_ndk_version=\"r21b\" android_ndk_major_version=21"

if [ "$BUILD_FLAVOR" == "release" ]; then
  config_args="${config_args} is_debug=false is_official_build=true"
else
  config_args="${config_args} is_debug=true"
fi

echo $config_args
echo "Building targets for $BUILD_PLATFORM $BUILD_FLAVOR"

gn gen $BUILD_OUTPUT_PATH --args="$config_args"

# build

echo "Running build for $BUILD_PLATFORM $BUILD_FLAVOR"

ninja -C $BUILD_OUTPUT_PATH v8jsi

# packaging

echo "Packaging"

mkdir -p $OUTPUT_PATH/android/headers/include/jsi
cp $SOURCES_PATH/V8Runtime.h $OUTPUT_PATH/android/headers/include # headers
cp $SOURCES_PATH/build.config $OUTPUT_PATH/android # config file
cp $SOURCES_PATH/ReactNative.V8JSI.Android.nuspec $OUTPUT_PATH/android # nuspec
cp third_party/android_ndk_r21b/source.properties $OUTPUT_PATH/android/ndk_source.properties # ndk source.properties
cp $SOURCES_PATH/jsi/{decorator.h,instrumentation.h,jsi-inl.h,jsi.h,JSIDynamic.h,jsilib.h,threadsafe.h} $OUTPUT_PATH/android/headers/include/jsi # jsi headers

mkdir -p $OUTPUT_PATH/android/lib/$BUILD_PLATFORM/$BUILD_FLAVOR

# TODO: add unstripped lib after ADO pipeline works due to large size
cp -r $BUILD_OUTPUT_PATH/{libv8jsi.so,args.gn} $OUTPUT_PATH/android/lib/$BUILD_PLATFORM/$BUILD_FLAVOR
