#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/i2c.h>

/* V4L2 Headers */
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>

/* Videobuf2 Headers */
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>

#include "camera_struct.h"
#include "camera_defs.h"

/* Forward declarations */
extern int camera_queue_init(struct camera_dev *cam);
extern void camera_queue_release(struct camera_dev *cam);
extern const struct v4l2_ioctl_ops *camera_get_ioctl_ops(void);

/* File operations for V4L2 device */
static const struct v4l2_file_operations camera_fops = {
    .owner          = THIS_MODULE,
    .open           = v4l2_fh_open,
    .release        = vb2_fop_release,
    .poll           = vb2_fop_poll,
    .unlocked_ioctl = video_ioctl2,
    .mmap           = vb2_fop_mmap,
};

/* Cấu hình sensor qua I2C */
static int imx219_set_yuyv_format(struct i2c_client *client, int width, int height)
{
    int ret;

    /* Cấu hình độ phân giải */
    if (width >= 16 && height >= 16 && width <= 16376 && height <= 16376) {
        /* Lệnh I2C để đặt định dạng YUYV và độ phân giải (Giá trị giả định) */
        ret = i2c_smbus_write_byte_data(client, 0x0120, 0x30); // Dummy: YUYV
        if (ret < 0) {
            pr_err("Failed to set YUYV format for width=%d height=%d\n", width, height);
            return ret;
        }
    } else {
        pr_err("Unsupported resolution %dx%d for YUYV\n", width, height);
        return -EINVAL;
    }

    /* Bật streaming */
    ret = i2c_smbus_write_byte_data(client, 0x0100, 0x01); // Streaming ON
    if (ret < 0) {
        pr_err("Failed to start streaming on IMX219\n");
        return ret;
    }

    pr_info("IMX219 configured: YUYV %dx%d\n", width, height);
    return 0;
}
static int imx219_set_resolution(struct i2c_client *client, int width, int height)
{
    int ret;

    if (width == 640 && height == 480) {
        /* Cấu hình sensor IMX219 cho độ phân giải 640x480 */
        ret = i2c_smbus_write_byte_data(client, 0x0160, 0x01); // Dummy register
        if (ret < 0) {
            pr_err("Failed to set resolution 640x480\n");
            return ret;
        }
    } else {
        pr_err("Resolution %dx%d not supported\n", width, height);
        return -EINVAL;
    }

    return 0;
}
static int imx219_set_resolution_and_streaming(struct i2c_client *client, int width, int height)
{
    int ret;

    /* Cấu hình độ phân giải sensor - sử dụng dummy values */
    if (width == 640 && height == 480) {
        ret = i2c_smbus_write_byte_data(client, 0x01, 0x01); /* Register giả định */
        if (ret < 0) {
            pr_err("Failed to set resolution 640x480\n");
            return ret;
        }
    } else {
        pr_err("Resolution %dx%d not supported\n", width, height);
        return -EINVAL;
    }

    /* Bật chế độ streaming */
    ret = i2c_smbus_write_byte_data(client, 0x01, 0x01); /* Streaming ON */
    if (ret < 0) {
        pr_err("Failed to enable streaming on IMX219\n");
        return ret;
    }

    pr_info("Streaming started at %dx%d\n", width, height);
    return 0;
}

/* Start streaming */
static int start_streaming(struct vb2_queue *q, unsigned int count)
{
    struct camera_dev *cam = vb2_get_drv_priv(q);
    int ret;
    if (cam->i2c_client) {
        ret = imx219_set_resolution(cam->i2c_client, 640, 480);
        if (ret < 0) {
            pr_err("Failed to start sensor streaming\n");
            return ret;
        }
    } else {
        pr_err("I2C client not initialized\n");
        return -EINVAL;
    }

    cam->streaming = 1;
    pr_info("Camera streaming started at reslution 640x480\n");

    return 0;
}

/* Stop streaming */
static void stop_streaming(struct vb2_queue *q)
{
    struct camera_dev *cam = vb2_get_drv_priv(q);

    if (cam->i2c_client)
        i2c_smbus_write_byte_data(cam->i2c_client, 0x01, 0x00); /* Streaming OFF */

    cam->streaming = 0;
    pr_info("Camera streaming stopped\n");
}
static const struct vb2_ops camera_vb2_ops = {
    .start_streaming = start_streaming,
    .stop_streaming = stop_streaming,
};
/* Probe function */
static int camera_probe(struct platform_device *pdev)
{
    struct camera_dev *cam;
    struct v4l2_device *v4l2_dev;
    struct video_device *vdev;
    struct i2c_adapter *adapter;
    int ret = 0;

    cam = devm_kzalloc(&pdev->dev, sizeof(*cam), GFP_KERNEL);
    if (!cam) {
        pr_err("Failed to allocate memory for camera_dev\n");
        return -ENOMEM;
    }

    cam->dev = &pdev->dev;
    v4l2_dev = &cam->v4l2_dev;

    /* Get I2C adapter */
    adapter = i2c_get_adapter(1); /* Adapter ID 1 (check `i2cdetect -l`) */
    if (!adapter) {
        pr_err("Failed to get I2C adapter\n");
        return -ENODEV;
    }

    cam->i2c_client = i2c_new_dummy_device(adapter, 0x10); /* Địa chỉ I2C */
    i2c_put_adapter(adapter);
    if (IS_ERR(cam->i2c_client)) {
        pr_err("Failed to register I2C client\n");
        return PTR_ERR(cam->i2c_client);
    }

    ret = v4l2_device_register(&pdev->dev, v4l2_dev);
    if (ret)
        goto err_free_i2c;

    vdev = &cam->vdev;
    vdev->v4l2_dev = v4l2_dev;
    vdev->fops = &camera_fops;

    ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
    if (ret)
        goto err_unregister_v4l2;

    pr_info("Camera probed successfully\n");
    return 0;

err_unregister_v4l2:
    v4l2_device_unregister(v4l2_dev);
err_free_i2c:
    i2c_unregister_device(cam->i2c_client);
    return ret;
}

static struct platform_driver camera_driver = {
    .probe = camera_probe,
    
    // Add `remove` if cleanup logic is needed
    .driver = {
        .name = DRIVER_NAME,
    },
};

module_platform_driver(camera_driver);
MODULE_LICENSE("GPL");