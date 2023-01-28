 call D:\d\android-ndk-r21b\ndk-build.cmd GEN_COMPILE_COMMANDS_DB=true NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_ABI=arm64-v8a

adb shell pkill dnsmasq
adb push .\libs\arm64-v8a\dnsmasq /data/local/tmp/
adb push init.dnsmasq /data/local/tmp/init.dnsmasq
adb shell chmod +x /data/local/tmp/dnsmasq
adb shell "cd /data/local/tmp/;sh init.dnsmasq" 