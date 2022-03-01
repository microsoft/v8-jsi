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
ln -sfn $SOURCES_PATH jsi # for the patches to work
pushd build && git apply < ../jsi/scripts/patch/build.patch
popd && git apply < jsi/scripts/patch/src.patch

# install Android NDK
pushd third_party

# TODO: use a docker image for dependencies instead
if [ ! -f android_ndk_$NDK_VERSION/source.properties ]; then
  echo "Installing Android NDK $NDK_VERSION"
  NDK_URL="https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip"

  if [[ $NDK_VERSION < "r23b" ]]; then # since the download link is different for older versions
    NDK_URL="https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux-x86_64.zip"
  fi

  curl -o android_ndk_$NDK_VERSION.zip $NDK_URL
  unzip android_ndk_$NDK_VERSION.zip
  mv android-ndk-$NDK_VERSION android_ndk_$NDK_VERSION
  rm -rf android_ndk_$NDK_VERSION.zip
else
  echo "Android NDK $NDK_VERSION installed"
fi
popd

# remove unused code
rm -rf "$BUILD_PATH/v8/test/test262/data/tools"
rm -rf "$BUILD_PATH/v8/third_party/catapult"
rm -rf "$BUILD_PATH/v8/third_party/google_benchmark"
rm -rf "$BUILD_PATH/v8/third_party/perfetto"
rm -rf "$BUILD_PATH/v8/third_party/protobuf"
rm -rf "$BUILD_PATH/v8/tools/clusterfuzz"
rm -rf "$BUILD_PATH/v8/tools/turbolizer"

# configure

NDK_R_VERSION=$(echo "$NDK_VERSION" | sed 's/r\([0-9]\{1,2\}[a-z]\).*/\1/')
NDK_MAJOR_VERSION=$(echo "$NDK_R_VERSION" | sed 's/\([0-9]\{1,2\}\).*/\1/') # for major version in build args
BUILD_OUTPUT_PATH="$BUILD_PATH/v8/out/$BUILD_PLATFORM/$BUILD_FLAVOR"

echo "Setting build configuration"
rm -rf $BUILD_OUTPUT_PATH
config_args="target_os=\"android\" target_cpu=\"$BUILD_PLATFORM\" v8_enable_i18n_support=false v8_target_cpu=\"$BUILD_PLATFORM\" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root=\"//third_party/android_ndk_$NDK_VERSION\" android_ndk_version=\"$NDK_VERSION\" android_ndk_major_version=$NDK_MAJOR_VERSION"

if [ "$BUILD_FLAVOR" == "ship" ]; then
  config_args="${config_args} is_debug=false is_official_build=true"
else
  config_args="${config_args} is_debug=true"
fi

echo $config_args
echo "Building targets for $BUILD_PLATFORM $BUILD_FLAVOR"

gn gen $BUILD_OUTPUT_PATH --args="$config_args"

# build

echo "Running build for $BUILD_PLATFORM $BUILD_FLAVOR"

NUM_THREADS=$((`nproc` * 2))
ninja -j $NUM_THREADS -C $BUILD_OUTPUT_PATH v8jsi

# packaging

echo "Packaging"

mkdir -p $OUTPUT_PATH/android/headers/include/jsi
cp jsi/V8Runtime.h $OUTPUT_PATH/android/headers/include # headers
cp jsi/build.config $OUTPUT_PATH/android # config file
cp jsi/ReactNative.V8Jsi.Android.nuspec $OUTPUT_PATH/android # nuspec including unstripped libs
cp third_party/android_ndk_$NDK_VERSION/source.properties $OUTPUT_PATH/android/ndk_source.properties # ndk source.properties
cp jsi/jsi/{decorator.h,instrumentation.h,jsi-inl.h,jsi.h,JSIDynamic.h,jsilib.h,threadsafe.h} $OUTPUT_PATH/android/headers/include/jsi # jsi headers

mkdir -p $OUTPUT_PATH/android/lib/"droid$BUILD_PLATFORM"/$BUILD_FLAVOR/x-none

cp -r $BUILD_OUTPUT_PATH/{libv8jsi.so,args.gn,lib.unstripped} $OUTPUT_PATH/android/lib/"droid$BUILD_PLATFORM"/$BUILD_FLAVOR/x-none

echo "Done!"
