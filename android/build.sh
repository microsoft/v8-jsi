# defining params
platform_list="x86 x64 arm arm64"

# removing output
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
cd build && git apply < ../jsi/scripts/patch/build.patch
cd .. && git apply < jsi/scripts/patch/src.patch

# install Android NDK r21b
cd third_party
# not the best way to check for NDK - should push this to repo / add a docker image for this and other deps
if [ ! -d android_ndk_r21b ]; then
  echo "Installing Android NDK r21b"
  curl -o android_ndk_r21b.zip https://dl.google.com/android/repository/android-ndk-r21b-linux-x86_64.zip
  unzip android_ndk_r21b.zip
  mv android-ndk-r21b android_ndk_r21b
  rm -rf android_ndk_r21b.zip
else
  echo "Android NDK installed"
fi
cd ..

# # configure 
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

mkdir -p jsi/out/android/headers/include
cp jsi/{V8Runtime.h,V8Runtime_impl.h,V8Platform.h,FileUtils.h} jsi/out/android/headers/include # headers

mkdir -p jsi/out/android/lib

for platform in $platform_list; do
  mkdir -p jsi/out/android/lib/$platform/{debug,ship}
  cp -r out/$platform/debug/{libv8jsi.so,args.gn,lib.unstripped} jsi/out/android/lib/$platform/debug/
  cp -r out/$platform/ship/{libv8jsi.so,args.gn,lib.unstripped} jsi/out/android/lib/$platform/ship/
done


# add files to a package
# nuspec
# nuget pack
# ado pipeline

# # Copy
# rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx86/debug/x-none/* 
# cp out/x86/debug/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx86/debug/x-none/ 

# rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx86/ship/x-none/* 
# cp out/x86/ship/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx86/ship/x-none/ 

# rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx64/debug/x-none/* 
# cp out/x64/debug/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx64/debug/x-none/ 

# rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx64/ship/x-none/* 
# cp out/x64/ship/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx64/ship/x-none/ 

# rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm/debug/x-none/* 
# cp out/arm/debug/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm/debug/x-none/ 

# rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm/ship/x-none/* 
# cp out/arm/ship/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm/ship/x-none/ 

# rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm64/debug/x-none/* 
# cp out/arm64/debug/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm64/debug/x-none/ 

# rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm64/ship/x-none/* 
# cp out/arm64/ship/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm64/ship/x-none/ 

# cp jsi/V8Runtime.h ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/headers/include/jsi/V8Runtime.h

# rn tester
