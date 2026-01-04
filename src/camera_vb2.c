#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h> /* nếu có trên hệ thống, còn không thì mem_ops cần điều chỉnh */
#include "camera_struct.h"
#include "camera_defs.h"

/*
 * Driver-owned buffer wrapper: mở rộng vb2_v4l2_buffer với list_head để
 * driver có thể quản lý danh sách buffer đã queued.
 */
struct camera_buffer {
    struct vb2_v4l2_buffer vb; /* must be first or at least contain vb2_buf */
    struct list_head list;     /* driver-managed list */
};

/* Queue setup: tính size của buffer */
static int queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
                       unsigned int *num_planes, unsigned int sizes[],
                       struct device *alloc_devs[])
{
    /* Không cần dùng cam ở đây, nhưng lấy để debug nếu cần */
    struct camera_dev *cam = vb2_get_drv_priv(q);

    if (*num_planes)
        return sizes[0] < CAMERA_WIDTH * CAMERA_HEIGHT * BYTES_PER_PIXEL ? -EINVAL : 0;

    *num_planes = 1;
    sizes[0] = CAMERA_WIDTH * CAMERA_HEIGHT * BYTES_PER_PIXEL;

    pr_info("%s: queue_setup: buffers=%u size=%u\n", DRIVER_NAME, *num_buffers, sizes[0]);
    (void)cam;
    return 0;
}

/* buf_queue: được gọi khi user-space QBUF */
static void buf_queue(struct vb2_buffer *vb)
{
    struct camera_buffer *cam_buf;
    struct camera_dev *cam = vb2_get_drv_priv(vb->vb2_queue);

    /* vb là struct vb2_buffer*, wrapper chứa vb2_v4l2_buffer.vb2_buf,
       nên lấy wrapper bằng container_of */
    cam_buf = container_of(vb, struct camera_buffer, vb.vb2_buf);

    /* Thêm buffer này vào danh sách queued của driver */
    spin_lock(&cam->queued_lock);
    list_add_tail(&cam_buf->list, &cam->queued_list);
    spin_unlock(&cam->queued_lock);

    /* Lấy địa chỉ DMA (nếu dùng vb2_dma_contig_memops) */
    /* Ghi chú: Hàm vb2_dma_contig_plane_dma_addr/hàm tương tự phụ thuộc mem_ops */
#ifdef HAVE_VB2_DMA_CONTIG_PLANE_DMA_ADDR
    {
        dma_addr_t dma_addr = vb2_dma_contig_plane_dma_addr(vb, 0);
        pr_debug("%s: buf_queue index=%d dma_addr=0x%pad\n", DRIVER_NAME, vb->index, &dma_addr);
        /* TODO: lập descriptor DMA / viết vào register hardware để trỏ vào dma_addr */
    }
#else
    pr_debug("%s: buf_queue index=%d (dma addr retrieval depends on mem_ops)\n",
             DRIVER_NAME, vb->index);
#endif

    /* Không kích hoạt hardware ở đây nếu streaming chưa bắt đầu; start_streaming sẽ làm */
}

/* start_streaming: bật camera + DMA */
static int start_streaming(struct vb2_queue *q, unsigned int count)
{
    struct camera_dev *cam = vb2_get_drv_priv(q);

    pr_info("%s: start_streaming: count=%u\n", DRIVER_NAME, count);

    cam->streaming = 1;
    cam->frame_count = 0;

    /*
     * TODO:
     * - Cấu hình sensor qua I2C (imx219_set_mode/format)
     * - Cấu hình CSI/ DMA controller với first queued buffer(s)
     * - Bật DMA / CSI
     */

    return 0;
}

/* stop_streaming: dừng camera + trả lại mọi buffer đang queued với trạng thái error */
static void stop_streaming(struct vb2_queue *q)
{
    struct camera_dev *cam = vb2_get_drv_priv(q);
    struct camera_buffer *cam_buf, *tmp;

    pr_info("%s: stop_streaming\n", DRIVER_NAME);

    cam->streaming = 0;

    /* Dừng hardware (DMA/CSI) trước - TODO */

    /* Trả lại tất cả buffer trong queued_list với trạng thái ERROR */
    spin_lock(&cam->queued_lock);
    list_for_each_entry_safe(cam_buf, tmp, &cam->queued_list, list) {
        list_del(&cam_buf->list);
        /* vb2_buffer_done expects struct vb2_buffer* */
        vb2_buffer_done(&cam_buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
    }
    spin_unlock(&cam->queued_lock);
}

/* vb2 ops */
static const struct vb2_ops vb2_ops = {
    .queue_setup    = queue_setup,
    .buf_queue      = buf_queue,
    .start_streaming = start_streaming,
    .stop_streaming = stop_streaming,
    /* wait_prepare/finish are optional; use vb2 helpers if needed */
};

/* Initialize vb2 queue */
int camera_queue_init(struct camera_dev *cam)
{
    struct vb2_queue *q = &cam->queue;
    int ret;

    q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    q->io_modes = VB2_MMAP | VB2_DMABUF;
    q->lock = &cam->lock;
    q->drv_priv = cam;
    /* IMPORTANT: use driver-owned wrapper size */
    q->buf_struct_size = sizeof(struct camera_buffer);
    q->ops = &vb2_ops;

    /*
     * mem_ops:
     * - nếu kernel cung cấp vb2_dma_contig_memops thì dùng để allocate contiguous DMA buffers
     * - nếu không, bạn cần chỉnh lại hoặc implement mem_ops phù hợp
     */
#ifdef USE_VB2_DMA_CONTIG_MEMOPS
    q->mem_ops = &vb2_dma_contig_memops;
#else
    /* fallback: vb2_core_memops, vb2_dma_sg_memops, ... tùy kernel/config */
    q->mem_ops = &vb2_dma_contig_memops; /* cố gắng dùng contig nếu có; nếu không có, bạn phải thay */
#endif

    q->buf_ops = NULL; /* not used for dma-contig */
    q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;

    INIT_LIST_HEAD(&cam->queued_list);
    spin_lock_init(&cam->queued_lock);

    ret = vb2_queue_init(q);
    if (ret < 0) {
        pr_err("%s: vb2_queue_init failed: %d\n", DRIVER_NAME, ret);
        return ret;
    }

    pr_info("%s: vb2 queue initialized\n", DRIVER_NAME);
    return 0;
}

void camera_queue_release(struct camera_dev *cam)
{
    vb2_queue_release(&cam->queue);
}