#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define VIDEO_DEVICE "/dev/video0"
#define NUM_BUFFERS 4
#define IMAGE_WIDTH 640
#define IMAGE_HEIGHT 480

struct buffer {
    void *start;
    size_t length;
};

static struct buffer *buffers;
static unsigned int num_buffers;

/**
 * xioctl - Wrapper cho ioctl call
 */
static int xioctl(int fh, int request, void *arg) {
    int r;
    do {
        r = ioctl(fh, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/**
 * init_device - Khởi tạo device
 */
static void init_device(int fd) {
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    
    /* Query capabilities */
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        exit(EXIT_FAILURE);
    }
    
    printf("Driver: %s\n", cap. driver);
    printf("Card: %s\n", cap. card);
    printf("Bus: %s\n", cap. bus_info);
    printf("Capabilities: 0x%08x\n", cap.capabilities);
    
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "Device does not support video capture\n");
        exit(EXIT_FAILURE);
    }
    
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming\n");
        exit(EXIT_FAILURE);
    }
    
    /* Set format */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = IMAGE_WIDTH;
    fmt.fmt.pix.height = IMAGE_HEIGHT;
    fmt.fmt.pix. pixelformat = V4L2_PIX_FMT_YUYV;
    
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        exit(EXIT_FAILURE);
    }
    
    printf("Format set to %ux%u\n", fmt. fmt.pix.width, fmt.fmt.pix.height);
    
    /* Request buffers */
    memset(&req, 0, sizeof(req));
    req.count = NUM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req. memory = V4L2_MEMORY_MMAP;
    
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        exit(EXIT_FAILURE);
    }
    
    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffers\n");
        exit(EXIT_FAILURE);
    }
    
    num_buffers = req.count;
    buffers = calloc(num_buffers, sizeof(*buffers));
    
    printf("Allocated %u buffers\n", num_buffers);
    
    /* Map buffers */
    for (unsigned int i = 0; i < num_buffers; ++i) {
        struct v4l2_buffer buf;
        
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            exit(EXIT_FAILURE);
        }
        
        buffers[i].length = buf.length;
        buffers[i]. start = mmap(NULL, buf.length,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, buf.m.offset);
        
        if (buffers[i]. start == MAP_FAILED) {
            perror("mmap");
            exit(EXIT_FAILURE);
        }
        
        printf("Buffer %u:  %zu bytes at %p\n", i, buffers[i].length, buffers[i].start);
    }
}

/**
 * start_capturing - Bắt đầu capture
 */
static void start_capturing(int fd) {
    enum v4l2_buf_type type;
    
    /* Queue all buffers */
    for (unsigned int i = 0; i < num_buffers; ++i) {
        struct v4l2_buffer buf;
        
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            exit(EXIT_FAILURE);
        }
    }
    
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        exit(EXIT_FAILURE);
    }
    
    printf("Video streaming started\n");
}

/**
 * capture_frame - Capture một frame
 */
static void capture_frame(int fd, int frame_count) {
    struct v4l2_buffer buf;
    
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    /* Dequeue buffer */
    if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("VIDIOC_DQBUF");
        return;
    }
    
    printf("Frame %d:  Buffer index %u, size %u bytes, timestamp %llu. %u\n",
           frame_count, buf.index, buf.bytesused,
           buf.timestamp.tv_sec, buf.timestamp.tv_usec);
    
    /* Save frame to file (optional) */
    char filename[64];
    sprintf(filename, "frame_%03d.yuyv", frame_count);
    FILE *fp = fopen(filename, "wb");
    if (fp) {
        fwrite(buffers[buf.index].start, 1, buf.bytesused, fp);
        fclose(fp);
        printf("  -> Saved to %s\n", filename);
    }
    
    /* Queue buffer again */
    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        perror("VIDIOC_QBUF");
    }
}

/**
 * stop_capturing - Dừng capture
 */
static void stop_capturing(int fd) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("VIDIOC_STREAMOFF");
    }
    
    printf("Video streaming stopped\n");
}

/**
 * uninit_device - Giải phóng resources
 */
static void uninit_device(void) {
    for (unsigned int i = 0; i < num_buffers; ++i) {
        if (munmap(buffers[i].start, buffers[i].length) < 0) {
            perror("munmap");
        }
    }
    
    free(buffers);
    printf("Device uninitialized\n");
}

/**
 * main - Hàm chính
 */
int main(int argc, char **argv) {
    int fd;
    int num_frames = 10;
    
    if (argc > 1) {
        num_frames = atoi(argv[1]);
    }
    
    printf("=== Simple Camera Capture Test ===\n");
    printf("Opening device:  %s\n", VIDEO_DEVICE);
    
    /* Open device */
    fd = open(VIDEO_DEVICE, O_RDWR, 0);
    if (fd < 0) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    
    printf("Device opened (fd=%d)\n\n", fd);
    
    /* Initialize device */
    init_device(fd);
    
    printf("\n");
    
    /* Start capturing */
    start_capturing(fd);
    
    printf("\nCapturing %d frames...\n\n", num_frames);
    
    /* Capture frames */
    for (int i = 0; i < num_frames; ++i) {
        capture_frame(fd, i + 1);
        sleep(1);  // Wait 1 second between frames
    }
    
    printf("\n");
    
    /* Stop capturing */
    stop_capturing(fd);
    
    /* Cleanup */
    uninit_device();
    close(fd);
    
    printf("Done!\n");
    return EXIT_SUCCESS;
}