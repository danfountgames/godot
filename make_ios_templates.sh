scons --clean
scons platform=ios target=template_debug metal=True vulkan=False debug_symbols=yes arch=arm64
scons platform=ios target=template_release dev_build=yes metal=True vulkan=False arch=arm64 
#scons platform=ios target=template_debug metal=True vulkan=False debug_symbols=yes arch=arm64 ios_simulator=yes
#scons platform=ios target=template_release dev_build=yes metal=True vulkan=False arch=arm64 ios_simulator=yes

mkdir -p templates
rm -rf templates/ios
cp -r misc/dist/ios_xcode templates/ios
cp bin/libgodot.ios.template_debug.arm64.a templates/ios/libgodot.ios.debug.xcframework/ios-arm64/libgodot.a
cp bin/libgodot.ios.template_release.arm64.a templates/ios/libgodot.ios.release.xcframework/ios-arm64/libgodot.a
#cp bin/libgodot.ios.template_debug.arm64.simulator.a templates/ios/libgodot.ios.debug.xcframework/ios-arm64-simulator/libgodot.a
#cp bin/libgodot.ios.template_release.arm64.simulator.a templates/ios/libgodot.ios.release.xcframework/ios-arm64-simulator/libgodot.a
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
