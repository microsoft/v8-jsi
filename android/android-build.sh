# This script is not a completely functional standalone script. As of now, the primary purpose is to document the commands issued.

mkdir v8-jsi-ws && cd v8-jsi-ws

git clone https://github.com/microsoft/v8-jsi.git
git checkout <our branch>

git clone git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=~/code/v8-jsi-ws/depot_tools/:$PATH
fetch v8
echo "target_os= ['android']" >> .gclient
gclient sync

cd v8

# Symlink v8-jsi
ln -s ../v8jsi/android jsi

# Apply build patches.
cd build && git apply < jsi/build.patch && cd ..

# Apply source patches
git apply < jsi/src.patch

# Install the required ndk and creat a symblink under third_party folder
ln -s /home/mganandraj/Android/android-ndk-r21b/ v8/third_party/android_ndk_r21b

# Configure 
gn gen out.gn.1/x86/debug --args='target_os="android" target_cpu="x86" is_debug=true v8_enable_i18n_support=false v8_target_cpu="x86" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'

gn gen out.gn.1/x86/ship --args='target_os="android" target_cpu="x86" is_debug=false v8_enable_i18n_support=false v8_target_cpu="x86" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'

gn gen out.gn.1/x64/debug --args='target_os="android" target_cpu="x64" is_debug=true v8_enable_i18n_support=false v8_target_cpu="x64" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'

gn gen out.gn.1/x64/ship --args='target_os="android" target_cpu="x64" is_debug=false v8_enable_i18n_support=false v8_target_cpu="x64" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'

gn gen out.gn.1/arm/debug --args='target_os="android" target_cpu="arm" is_debug=true v8_enable_i18n_support=false v8_target_cpu="arm" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'

gn gen out.gn.1/arm/ship --args='target_os="android" target_cpu="arm" is_debug=false v8_enable_i18n_support=false v8_target_cpu="arm" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'

gn gen out.gn.1/arm64/debug --args='target_os="android" target_cpu="arm64" is_debug=true v8_enable_i18n_support=false v8_target_cpu="arm64" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21'

gn gen out.gn.1/arm64/ship --args='target_os="android" target_cpu="arm64" is_debug=false v8_enable_i18n_support=false v8_target_cpu="arm64" is_component_build=false use_goma=false v8_use_snapshot=true v8_use_external_startup_data=false v8_static_library=false strip_debug_info=true symbol_level=0 strip_absolute_paths_from_debug_symbols=true android_ndk_root="//third_party/android_ndk_r21b" android_ndk_version="r21b" android_ndk_major_version=21 is_official_build=true'

# Build
ninja -C out.gn.1/x86/debug v8jsi && ninja -C out.gn.1/x86/ship v8jsi && ninja -C out.gn.1/x64/debug v8jsi && ninja -C out.gn.1/x64/ship v8jsi && ninja -C out.gn.1/arm/debug v8jsi && ninja -C out.gn.1/arm/ship v8jsi && ninja -C out.gn.1/arm64/debug v8jsi && ninja -C out.gn.1/arm64/ship v8jsi

# Copy
rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx86/debug/x-none/* 
cp out.gn.1/x86/debug/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx86/debug/x-none/ 

rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx86/ship/x-none/* 
cp out.gn.1/x86/ship/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx86/ship/x-none/ 

rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx64/debug/x-none/* 
cp out.gn.1/x64/debug/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx64/debug/x-none/ 

rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx64/ship/x-none/* 
cp out.gn.1/x64/ship/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidx64/ship/x-none/ 

rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm/debug/x-none/* 
cp out.gn.1/arm/debug/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm/debug/x-none/ 

rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm/ship/x-none/* 
cp out.gn.1/arm/ship/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm/ship/x-none/ 

rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm64/debug/x-none/* 
cp out.gn.1/arm64/debug/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm64/debug/x-none/ 

rm -rf ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm64/ship/x-none/* 
cp out.gn.1/arm64/ship/libv8jsi.so ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/droidarm64/ship/x-none/ 

cp jsi/V8Runtime.h ~/code/react-native-macos/ReactAndroid/packages/ReactNative.V8.Android.7.0.276.32-v1/headers/include/jsi/V8Runtime.h