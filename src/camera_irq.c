#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <media/videobuf2-v4l2.h>
#include "camera_struct.h"
#include "camera_defs.h"

/**
 * camera_irq_handler - Xử lý interrupt khi frame capture hoàn tất
 * 
 * DMA controller sẽ gữi interrupt sau khi hoàn thành transfer một frame.
 * Ở handler này, chúng ta sẽ:
 * 1. Kiểm tra status của DMA
 * 2. Lấy buffer đầu tiên trong queue
 * 3. Mark buffer as done
 * 4. Queue buffer tiếp theo
 */
static irqreturn_t camera_irq_handler(int irq, void *dev_id) {
    struct camera_dev *cam = (struct camera_dev *)dev_id;
    struct vb2_v4l2_buffer *vbuf;
    unsigned long flags;
    
    if (! cam->streaming) {
        pr_debug("%s: IRQ received but not streaming\n", DRIVER_NAME);
        return IRQ_HANDLED;
    }
    
    pr_debug("%s: Frame interrupt - frame %u\n", DRIVER_NAME, cam->frame_count);
    
    /* 
     * Lấy buffer đầu tiên trong queue
     * 
     * Thực tế:  Bạn cần lấy buffer được setup bên buf_queue()
     * Và lấy địa chỉ DMA từ nó để so sánh với CSI status
     */
    
    spin_lock_irqsave(&cam->queue. done_lock, flags);
    
    /* 
     * Giả sử DMA đã hoàn tất: 
     * 1. Lấy buffer từ queue
     * 2. Thiết lập timestamp
     * 3. Mark buffer done
     * 4. Queue buffer tiếp theo
     */
    if (! list_empty(&cam->queue.queued_list)) {
        /* Lấy buffer đầu tiên */
        struct vb2_buffer *vb = 
            list_first_entry(&cam->queue.queued_list, struct vb2_buffer, queued_entry);
        vbuf = to_vb2_v4l2_buffer(vb);
        
        /* Thiết lập timestamp */
        vbuf->vb2_buf.timestamp = ktime_get_ns();
        vbuf->sequence = cam->frame_count;
        vbuf->field = V4L2_FIELD_NONE;
        
        /* Đánh dấu buffer là ready để dequeue */
        vb2_buffer_done(&vbuf->vb2_buf, VB2_BUF_STATE_DONE);
        
        cam->frame_count++;
        
        pr_debug("%s: Buffer done - timestamp: %llu\n", 
                 DRIVER_NAME, vbuf->vb2_buf.timestamp);
    }
    
    spin_unlock_irqrestore(&cam->queue. done_lock, flags);
    
    return IRQ_HANDLED;
}

int camera_irq_setup(struct camera_dev *cam) {
    int ret;
    
    if (!cam->irq) {
        pr_err("%s: No IRQ assigned\n", DRIVER_NAME);
        return -EINVAL;
    }
    
    ret = request_irq(cam->irq, camera_irq_handler, 
                      IRQF_TRIGGER_RISING | IRQF_SHARED,
                      DRIVER_NAME, cam);
    if (ret < 0) {
        pr_err("%s: Failed to request IRQ %d\n", DRIVER_NAME, cam->irq);
        return ret;
    }
    
    pr_info("%s: IRQ %d registered\n", DRIVER_NAME, cam->irq);
    return 0;
}

void camera_irq_cleanup(struct camera_dev *cam) {
    if (cam->irq)
        free_irq(cam->irq, cam);
}