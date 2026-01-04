#ifndef __CAMERA_DEFS_H__
#define __CAMERA_DEFS_H__

#include <linux/types.h>
#include <linux/ioctl.h>

#define DRIVER_NAME "rpi_camera_driver"
#define MODULE_NAME "rpi_camera"

/* Camera parameters */
#define CAMERA_WIDTH  640    // Độ rộng frame
#define CAMERA_HEIGHT 480    // Độ cao frame
#define CAMERA_FPS    30     // Frame per second

/* V4L2 định dạng pixel - YUYV (16-bit) */
#define CAMERA_FMT V4L2_PIX_FMT_YUYV

/* DMA channel */
#define DMA_CHANNEL 5        // Kênh DMA an toàn trên RPi

/* Buffer configuration */
#define NUM_BUFFERS 4        // Số buffer DMA
#define BYTES_PER_PIXEL 2    // YUYV = 2 bytes/pixel

/* Device info */
#define DEVICE_NAME "rpi_camera"

#endif /* __CAMERA_DEFS_H__ */