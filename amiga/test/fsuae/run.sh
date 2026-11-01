#!/bin/bash
# usage: run.sh <pal|ntsc> <out_prefix> <boot_seconds> [inject-script]
# Boots build/a1200/zookeeper_a1200.adf in FS-UAE (A1200 model, 68EC020, 2 MB chip, no other memory) headless under
# Xvfb.  The optional inject-script is `source`d after the boot delay with DISPLAY
# set; it can call
#     key <xdotool args>   - send keys to the emulator window
#     shot NAME            - window screenshot  -> <out_prefix>_NAME.png
#     full NAME            - FS-UAE native full-frame screenshot (752x572, whole raster,
#                            1 lores pixel = 2 px) -> <out_prefix>_NAME_full.png
here=$(cd "$(dirname "$0")" && pwd)
std=$1; out=$2; secs=${3:-15}; script=$4
tmp=$(mktemp -d)
cfg="$here/.run_$$.fs-uae"
: "${KICK:?set KICK=/path/to/kickstart.rom (test only - never part of the deliverables)}"
ADF=${ADF:-$here/../../platform/build/exb_a1200.adf}
sed -e "s|@KICK@|$KICK|" -e "s|@ADF@|$ADF|" -e "s|^\[end\]|screenshots_output_dir = $tmp\n[end]|" "$here/$std.fs-uae" > "$cfg"
cd "$here"
if [ -n "$AUDIO_OUT" ]; then   # record the emulated Paula output through OpenAL-soft's wave backend
  cat > "$tmp/alsoft.conf" <<EOT
[general]
drivers=wave
frequency=44100
channels=stereo
sample-type=int16
[wave]
file=$AUDIO_OUT
EOT
  export ALSOFT_CONF="$tmp/alsoft.conf"
fi
export OUT=$out TMPSHOTS=$tmp
xvfb-run -a -s "-screen 0 1024x768x24" bash -c '
  fs-uae "'"$cfg"'" >"'"$out"'.log" 2>&1 & P=$!
  sleep '"$secs"'
  WID=$(xdotool search --onlyvisible --name "FS-UAE" | head -1); xdotool windowfocus $WID
  key()  { xdotool "$@"; }
  shot() { import -window root "'"$out"'_$1.png"; }
  full() { xdotool keydown F12; sleep 0.3; xdotool key s; sleep 0.3; xdotool keyup F12; sleep 1.5
           cp "'"$tmp"'"/fs-uae-full-*.png "'"$out"'_$1_full.png" 2>/dev/null; rm -f "'"$tmp"'"/*; }
  [ -n "'"$script"'" ] && source "'"$script"'"
  kill $P; wait $P 2>/dev/null'
rm -rf "$tmp" "$cfg"
