# Makefile for RPi Camera Driver

# Module output
obj-m += rpi_camera.o

# Source files - path phải tương đối từ Makefile
rpi_camera-objs := src/camera_main.o \
                   src/camera_vb2.o \
                   src/camera_v4l2.o \
                   src/camera_irq.o

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Default build target
all: modules

# Build modules
modules:
	@echo "Building RPi Camera Driver..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean build artifacts
clean:
	@echo "Cleaning..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install module
install: modules
	@echo "Installing module..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

# Load module
load: modules
	@echo "Loading module..."
	sudo insmod rpi_camera.ko

# Unload module
unload:
	@echo "Unloading module..."
	sudo rmmod rpi_camera

# Reload module (unload + load)
reload: unload load

# View kernel log
dmesg:
	@echo "Kernel logs:"
	sudo dmesg | tail -20

# Help
help:
	@echo "Available targets:"
	@echo "  make              - Build module"
	@echo "  make clean        - Clean build"
	@echo "  make install      - Install module"
	@echo "  make load         - Load module (requires sudo)"
	@echo "  make unload       - Unload module (requires sudo)"
	@echo "  make reload       - Reload module"
	@echo "  make dmesg        - View kernel logs"