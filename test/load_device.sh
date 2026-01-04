#!/bin/bash

# Tạo platform device để module probe

# Bước 1: Thêm device vào device tree hoặc tạo platform device động

# Cách 1: Tạo device tree overlay (nếu sử dụng RPi device tree)
# Tạo file: rpi-camera. dts
cat > rpi-camera.dts << 'EOF'
/dts-v1/;
/plugin/;

/ {
    compatible = "brcm,bcm2835";
    
    fragment@0 {
        target-path = "/";
        __overlay__ {
            rpi_camera {
                compatible = "rpi-camera-driver";
                status = "okay";
            };
        };
    };
};
EOF

# Biên dịch device tree overlay
dtc -O dtb -o rpi-camera.dtbo rpi-camera.dts

# Cách 2: Tạo platform device động qua sysfs (dễ hơn cho test)
echo "Creating platform device..."
echo "rpi_camera_driver 0" > /sys/bus/platform/devices/add 2>/dev/null || \
    echo "rpi_camera_driver" > /sys/bus/platform/drivers/rpi_camera_driver/bind 2>/dev/null || \
    echo "Device creation via sysfs may require manual device tree setup"

echo "Done"