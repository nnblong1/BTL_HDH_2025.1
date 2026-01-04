#!/bin/bash

set -e

DRIVER_NAME="rpi_camera"
MODULE_PATH="../${DRIVER_NAME}.ko"

echo "=== RPi Camera Driver Loader ==="

# Kiểm tra module file
if [ ! -f "$MODULE_PATH" ]; then
    echo "Error: Module not found at $MODULE_PATH"
    echo "Please run 'make' in the parent directory first"
    exit 1
fi

# Unload nếu đã load
echo "Checking if module already loaded..."
if lsmod | grep -q "$DRIVER_NAME"; then
    echo "Module already loaded, unloading..."
    sudo rmmod "$DRIVER_NAME"
    sleep 1
fi

# Load module
echo "Loading module from $MODULE_PATH"
sudo insmod "$MODULE_PATH"

echo "Module loaded successfully!"
echo ""
echo "Checking module:"
lsmod | grep "$DRIVER_NAME"

echo ""
echo "Kernel messages:"
sudo dmesg | tail -10

echo ""
echo "Video devices:"
ls -la /dev/video* 2>/dev/null || echo "No video devices found yet"

echo ""
echo "To unload:  sudo rmmod $DRIVER_NAME"