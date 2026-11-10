#!/bin/sh
# Renders proto1 mockups to PNG with headless Chrome.
cd "$(dirname "$0")"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
shot() { # out url w h scale
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --allow-file-access-from-files \
    --force-device-scale-factor="${5:-1}" --window-size="$3,$4" --virtual-time-budget=1500 \
    --screenshot="$PWD/$1" "file://$PWD/src/$2" >/dev/null 2>&1
  echo "$1"
}
if [ "$1" != "web" ]; then
for s in send countdown running done live liveshift gamepad connect empty; do :; done
shot console-01-send.png "console.html?s=send" 1920 1080
shot console-02-countdown.png "console.html?s=countdown" 1920 1080
shot console-03-sending.png "console.html?s=running" 1920 1080
shot console-04-sent.png "console.html?s=done" 1920 1080
shot console-05-live-keyboard.png "console.html?s=live" 1920 1080
shot console-06-live-keyboard-shift.png "console.html?s=liveshift" 1920 1080
shot console-07-live-gamepad-passthrough.png "console.html?s=gamepad" 1920 1080
shot console-08-connect.png "console.html?s=connect" 1920 1080
shot console-09-first-run-no-usb.png "console.html?s=empty" 1920 1080
fi
