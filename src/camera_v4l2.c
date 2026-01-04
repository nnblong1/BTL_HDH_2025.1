#include <linux/kernel.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include "camera_struct.h"
#include "camera_defs.h"

/**
 * vidioc_querycap - Trả về khả năng của device
 * 
 * Được gọi khi user gọi VIDIOC_QUERYCAP.
 * Cho phép user biết driver hỗ trợ những gì.
 */
static int vidioc_querycap(struct file *file, void *priv, 
                           struct v4l2_capability *cap) {
    struct camera_dev *cam = video_drvdata(file);
    
    strscpy(cap->driver, DRIVER_NAME, sizeof(cap->driver));
    strscpy(cap->card, "RPi Camera Driver", sizeof(cap->card));
    strscpy(cap->bus_info, "platform:camera", sizeof(cap->bus_info));
    
    /* Khả năng hỗ trợ */
    cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    cap->device_caps = cap->capabilities;
    
    pr_info("%s: Querycap called\n", DRIVER_NAME);
    return 0;
}

/**
 * vidioc_enum_fmt_vid_cap - Liệt kê các format hỗ trợ
 * 
 * Cho phép user biết những format nào có thể capture.
 */
static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
                                   struct v4l2_fmtdesc *f) {
    struct camera_dev *cam = video_drvdata(file);
    
    /* Hiện tại chỉ hỗ trợ 1 format:  YUYV */
    if (f->index != 0)
        return -EINVAL;
    
    f->pixelformat = CAMERA_FMT;
    strscpy(f->description, "YUV 4: 2:2 (YUYV)", sizeof(f->description));
    f->flags = 0;
    
    pr_debug("%s: Enum format - index: %u, format: %c%c%c%c\n", 
             DRIVER_NAME, f->index,
             (CAMERA_FMT >> 0) & 0xFF,
             (CAMERA_FMT >> 8) & 0xFF,
             (CAMERA_FMT >> 16) & 0xFF,
             (CAMERA_FMT >> 24) & 0xFF);
    
    return 0;
}

/**
 * vidioc_try_fmt_vid_cap - Kiểm tra và điều chỉnh format
 * 
 * Hàm này không thay đổi state của driver,
 * chỉ kiểm tra xem format có hợp lệ không.
 */
static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
                                  struct v4l2_format *f) {
    struct camera_dev *cam = video_drvdata(file);
    
    /* Kiểm tra format */
    if (f->fmt.pix.pixelformat != CAMERA_FMT)
        return -EINVAL;
    
    /* Thiết lập resolution cố định */
    f->fmt.pix.width = CAMERA_WIDTH;
    f->fmt.pix.height = CAMERA_HEIGHT;
    f->fmt.pix.field = V4L2_FIELD_NONE;
    
    /* Tính bytes per line (stride) */
    f->fmt.pix.bytesperline = CAMERA_WIDTH * BYTES_PER_PIXEL;
    
    /* Tính tổng kích thước image */
    f->fmt.pix.sizeimage = CAMERA_WIDTH * CAMERA_HEIGHT * BYTES_PER_PIXEL;
    
    /* Colorspace */
    f->fmt. pix.colorspace = V4L2_COLORSPACE_SRGB;
    f->fmt.pix. ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
    f->fmt.pix.quantization = V4L2_QUANTIZATION_DEFAULT;
    f->fmt.pix.xfer_func = V4L2_XFER_FUNC_DEFAULT;
    
    pr_debug("%s: Try format - %ux%u, %u bytes\n", DRIVER_NAME,
             f->fmt.pix. width, f->fmt.pix.height, f->fmt.pix.sizeimage);
    
    return 0;
}

/**
 * vidioc_g_fmt_vid_cap - Lấy format hiện tại
 */
static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
                                struct v4l2_format *f) {
    struct camera_dev *cam = video_drvdata(file);
    
    f->fmt.pix.pixelformat = CAMERA_FMT;
    f->fmt.pix.width = CAMERA_WIDTH;
    f->fmt.pix.height = CAMERA_HEIGHT;
    f->fmt.pix.field = V4L2_FIELD_NONE;
    f->fmt.pix.bytesperline = CAMERA_WIDTH * BYTES_PER_PIXEL;
    f->fmt.pix.sizeimage = CAMERA_WIDTH * CAMERA_HEIGHT * BYTES_PER_PIXEL;
    f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    
    pr_info("%s: Get format called\n", DRIVER_NAME);
    return 0;
}

/**
 * vidioc_s_fmt_vid_cap - Thiết lập format
 */
static int vidioc_s_fmt_vid_cap(struct file *file, void *priv,
                                struct v4l2_format *f) {
    struct camera_dev *cam = video_drvdata(file);
    int ret;
    
    /* Kiểm tra format trước */
    ret = vidioc_try_fmt_vid_cap(file, priv, f);
    if (ret < 0)
        return ret;
    
    /* Lưu format hiện tại */
    memcpy(&cam->current_fmt, f, sizeof(*f));
    
    pr_info("%s: Format set to %ux%u\n", DRIVER_NAME,
            cam->current_fmt.fmt.pix.width, cam->current_fmt.fmt.pix.height);
    
    return 0;
}

/**
 * vidioc_g_parm - Lấy parameters (FPS, ...)
 */
static int vidioc_g_parm(struct file *file, void *priv,
                         struct v4l2_streamparm *parm) {
    struct camera_dev *cam = video_drvdata(file);
    
    parm->parm.capture. capability = V4L2_CAP_TIMEPERFRAME;
    parm->parm.capture.timeperframe. numerator = 1;
    parm->parm.capture.timeperframe.denominator = CAMERA_FPS;
    parm->parm.capture.readbuffers = NUM_BUFFERS;
    
    pr_debug("%s: Get parm - FPS:  %u\n", DRIVER_NAME, CAMERA_FPS);
    return 0;
}

/* Định nghĩa ioctl_ops */
static const struct v4l2_ioctl_ops camera_ioctl_ops = {
    .vidioc_querycap         = vidioc_querycap,
    .vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
    .vidioc_try_fmt_vid_cap  = vidioc_try_fmt_vid_cap,
    . vidioc_g_fmt_vid_cap    = vidioc_g_fmt_vid_cap,
    .vidioc_s_fmt_vid_cap    = vidioc_s_fmt_vid_cap,
    . vidioc_g_parm          = vidioc_g_parm,
    
    /* Videobuf2 ioctls - tự động handle */
    .vidioc_reqbufs          = vb2_ioctl_reqbufs,
    .vidioc_querybuf         = vb2_ioctl_querybuf,
    .vidioc_qbuf             = vb2_ioctl_qbuf,
    .vidioc_dqbuf            = vb2_ioctl_dqbuf,
    .vidioc_streamon         = vb2_ioctl_streamon,
    .vidioc_streamoff        = vb2_ioctl_streamoff,
};

const struct v4l2_ioctl_ops *camera_get_ioctl_ops(void) {
    return &camera_ioctl_ops;
}