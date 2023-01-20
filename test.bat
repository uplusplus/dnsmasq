adb shell pkill dnsmasq
adb push .\libs\arm64-v8a\dnsmasq /data/local/tmp/
adb push init.dnsmasq /data/local/tmp/init.dnsmasq
adb shell chmod +x /data/local/tmp/dnsmasq
adb shell "cd /data/local/tmp/;sh init.dnsmasq" 