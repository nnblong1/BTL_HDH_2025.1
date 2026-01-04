#!/bin/bash

set -e

echo "=== RPi Camera Driver Test ==="

DEVICE="/dev/video0"

# Kiểm tra device
if [ ! -e "$DEVICE" ]; then
    echo "Error: $DEVICE not found"
    echo "Make sure the driver is loaded and device tree is configured"
    exit 1
fi

echo "Testing device:  $DEVICE"

# 1. Query capabilities
echo ""
echo "1. Querying device capabilities..."
v4l2-ctl -d "$DEVICE" --info

# 2. List formats
echo ""
echo "2. Available formats:"
v4l2-ctl -d "$DEVICE" --list-formats

# 3. Get current format
echo ""
echo "3. Current format:"
v4l2-ctl -d "$DEVICE" --get-fmt-video

# 4. Set format
echo ""
echo "4. Setting format to 640x480 YUYV..."
v4l2-ctl -d "$DEVICE" --set-fmt-video=width=640,height=480,pixelformat=YUYV

# 5. Get all info
echo ""
echo "5. All device info:"
v4l2-ctl -d "$DEVICE" --all

echo ""
echo "Test completed!"