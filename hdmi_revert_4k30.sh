#!/system/bin/sh

delay="${1:-25}"
sleep "$delay"

setprop persist.vendor.color.HDMI-A-1 RGB-8bit
# Display mode must come from the system/EDID. Do not force HDMI resolution here.
setprop vendor.display.timeline "$(date +%s)"
