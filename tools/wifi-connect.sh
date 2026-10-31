#!/bin/sh
# usage: wifi-connect <ssid> [password]
set -e
ssid="$1"
pass="$2"
if [ -z "$ssid" ]; then
    echo "usage: wifi-connect SSID [PASSWORD]" >&2
    exit 1
fi
mkdir -p /var/run/wpa_supplicant
{
    echo "ctrl_interface=/var/run/wpa_supplicant"
    echo "update_config=1"
    if [ -n "$pass" ]; then
        wpa_passphrase "$ssid" "$pass"
    else
        printf 'network={\n\tssid="%s"\n\tkey_mgmt=NONE\n}\n' "$ssid"
    fi
} > /etc/wpa_supplicant/wpa_supplicant.conf
killall wpa_supplicant 2>/dev/null || true
sleep 1
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf
sleep 2
wpa_cli -i wlan0 status || true
echo "--- dhcp ---"
udhcpc -i wlan0 -q -n -t 10 || true

# The phone RTC is not battery backed and can come up decades off, which
# makes every TLS certificate look expired ("server certificate not
# trusted" from apk).  Now that there is a link, fetch the real time.
year=$(date +%Y 2>/dev/null || echo 0)
if [ "$year" -gt 2030 ] || [ "$year" -lt 2024 ]; then
    echo "--- ntp (clock is $year) ---"
    ntpd -q -n -p ntp.aliyun.com || ntpd -q -n -p ntp.tencent.com || true
fi

ip addr show wlan0 || true
