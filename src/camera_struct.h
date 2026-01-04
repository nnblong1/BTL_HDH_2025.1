#ifndef __CAMERA_STRUCT_H__
#define __CAMERA_STRUCT_H__

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-v4l2.h>

/* Cấu trúc chính của driver */
struct camera_dev {
    /* V4L2 core */
    struct v4l2_device v4l2_dev;
    struct video_device vdev;
    struct v4l2_ctrl_handler ctrl_handler;

    /* Videobuf2 - Buffer management */
    struct vb2_queue queue;

    /* Mutex để bảo vệ concurrent access */
    struct mutex lock;

    /* DMA allocator context (NULL nếu kernel tự quản lý) */
    void *dma_alloc_ctx;

    /* Device reference */
    struct device *dev;
    struct platform_device *pdev;

    /* I2C client cho IMX219 sensor */
    struct i2c_client *i2c_client;

    /* Interrupt */
    int irq;

    /* Hardware state */
    int streaming;
    u32 frame_count;

    /* Format hiện tại */
    struct v4l2_format current_fmt;

    /* Driver-owned list of queued buffers and lock to protect it */
    struct list_head queued_list;   /* list of struct camera_buffer */
    spinlock_t queued_lock;
};

#endif /* __CAMERA_STRUCT_H__ */