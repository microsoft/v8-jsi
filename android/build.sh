#!/bin/bash

# defining params
# TODO: should only build one platform by default - will change when ADO pipeline is set up

platform_list="x86 x64 arm arm64" 

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
for platform in $platform_list; do
  gn gen out/$platform/debug --args='target_os="android" target_cpu="'$platform'" is_debug=true v8_enable_i18n_support=false v8_target_cpu="'$platform'" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'
  gn gen out/$platform/ship --args='target_os="android" target_cpu="'$platform'" is_debug=false v8_enable_i18n_support=false v8_target_cpu="'$platform'" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'
done

# build
echo "Building"

for platform in $platform_list; do
  echo "Running build for $platform"
  ninja -C out/$platform/debug v8jsi && ninja -C out/$platform/ship v8jsi
done

# packaging

echo "Packaging"

mkdir -p jsi/out/android/headers/include/jsi
cp jsi/V8Runtime.h jsi/out/android/headers/include # headers
cp jsi/build.config jsi/out/android # config file
cp third_party/android_ndk_r21b/source.properties jsi/out/android/ndk_source.properties # ndk source.properties
cp jsi/jsi/{decorator.h,instrumentation.h,jsi-inl.h,jsi.h,JSIDynamic.h,jsilib.h,threadsafe.h} jsi/out/android/headers/include/jsi # jsi headers

mkdir -p jsi/out/android/lib

for platform in $platform_list; do
  mkdir -p jsi/out/android/lib/$platform/{debug,ship}

  # TODO: add unstripped lib after ADO pipeline works due to large size
  cp -r out/$platform/debug/{libv8jsi.so,args.gn} jsi/out/android/lib/$platform/debug/
  cp -r out/$platform/ship/{libv8jsi.so,args.gn} jsi/out/android/lib/$platform/ship/
done
