scons --clean
scons platform=ios target=template_debug metal=True arch=arm64 vulkan=False debug_symbols=yes
#scons --clean
#scons platform=ios target=template_release metal=True arch=arm64 vulkan=False
#scons --clean
#scons platform=ios target=template_debug ios_simulator=yes arch=arm64 metal=True vulkan=False debug_symbols=yes
#scons --clean
#scons platform=ios target=template_release ios_simulator=yes arch=arm64 metal=True vulkan=False

mkdir -p templates
rm -rf templates/ios
cp -r misc/dist/ios_xcode templates/ios
cp bin/libgodot.ios.template_debug.arm64.a templates/ios/libgodot.ios.debug.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_release.arm64.a templates/ios/libgodot.ios.release.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_debug.arm64.simulator.a templates/ios/libgodot.ios.debug.xcframework/ios-arm64_x86_64-simulator/libgodot.a
cp bin/libgodot.ios.template_release.arm64.simulator.a templates/ios/libgodot.ios.release.xcframework/ios-arm64_x86_64-simulator/libgodot.a
sed -i '' '/<string>x86_64<\/string>/d' templates/ios/libgodot.ios.debug.xcframework/Info.plist
sed -i '' '/<string>x86_64<\/string>/d' templates/ios/libgodot.ios.release.xcframework/Info.plist
rm templates/ios.zip
cd templates/ios
zip -0 -r ../ios.zip *
cd ../../
rm -rf templates/ios

rm templates.tpz
zip -0 -r templates templates
mv templates.zip templates.tpz
