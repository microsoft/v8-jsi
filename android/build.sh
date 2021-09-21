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
fetch v8 && cd v8/
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
echo "Installing Android NDK r21b"
cd third_party
curl -o android_ndk_r21b.zip https://dl.google.com/android/repository/android-ndk-r21b-linux-x86_64.zip
unzip android_ndk_r21b.zip && mv android-ndk-r21b android_ndk_r21b
rm -rf android_ndk_r21b.zip && cd ..

# # configure 
echo "Setting build configuration"
gn gen out/x86/debug --args='target_os="android" target_cpu="x86" is_debug=true v8_enable_i18n_support=false v8_target_cpu="x86" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'
gn gen out/x86/ship --args='target_os="android" target_cpu="x86" is_debug=false v8_enable_i18n_support=false v8_target_cpu="x86" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'
gn gen out/x64/debug --args='target_os="android" target_cpu="x64" is_debug=true v8_enable_i18n_support=false v8_target_cpu="x64" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'
gn gen out/x64/ship --args='target_os="android" target_cpu="x64" is_debug=false v8_enable_i18n_support=false v8_target_cpu="x64" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'
gn gen out/arm/debug --args='target_os="android" target_cpu="arm" is_debug=true v8_enable_i18n_support=false v8_target_cpu="arm" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'
gn gen out/arm/ship --args='target_os="android" target_cpu="arm" is_debug=false v8_enable_i18n_support=false v8_target_cpu="arm" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'
gn gen out/arm64/debug --args='target_os="android" target_cpu="arm64" is_debug=true v8_enable_i18n_support=false v8_target_cpu="arm64" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'
gn gen out/arm64/ship --args='target_os="android" target_cpu="arm64" is_debug=false v8_enable_i18n_support=false v8_target_cpu="arm64" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'

# build
echo "Building"
ninja -C out/x86/debug v8jsi && ninja -C out/x86/ship v8jsi && ninja -C out/x64/debug v8jsi && ninja -C out/x64/ship v8jsi && ninja -C out/arm/debug v8jsi && ninja -C out/arm/ship v8jsi && ninja -C out/arm64/debug v8jsi && ninja -C out/arm64/ship v8jsi
mv out jsi

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
