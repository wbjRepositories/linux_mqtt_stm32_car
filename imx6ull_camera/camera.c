#define _DEFAULT_SOURCE

#include <stdio.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>

#define CAM_W  640  //摄像头像素宽
#define CAM_H  480  //摄像头像素高
#define SHM_FRM_NAME     "/shm_frm" //摄像头信号量
int camera_fd = -1;
const int MAX_RETRIES = 20; // 最多尝试20次
const int SLEEP_MS = 100;   // 每次休眠100ms

static inline uint8_t clamp255(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/*
 * 将yuyv格式转换为argb8888格式
 * yuyv：输入缓冲区（大小 = 宽度 * 高度 * 2）
 * out：输出缓冲区（uint32_t 像素）大小 = 宽度 * 高度
 * 假定内存中的 ARGB8888 格式为 0xAARRGGBB（大端逻辑打包到 uint32_t 中）。
 */
void yuyv_to_argb8888(const uint8_t *yuyv, uint32_t *out, int width, int height) {
    const uint8_t *in = yuyv;
    int pixels = width * height;
    int i_out = 0;

    // 每次循环处理两个像素 (4 bytes input -> 2 outputs)
    for (int i = 0; i < pixels; i += 2) {
        int y0 = in[0];
        int u  = in[1];
        int y1 = in[2];
        int v  = in[3];
        in += 4;

        // 转换第一个像素
        int C = y0 - 16;
        int D = u - 128;
        int E = v - 128;
        // 使用定点系数（乘以 256 进行缩放）
        int r = (298 * C + 409 * E + 128) >> 8;
        int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
        int b = (298 * C + 516 * D + 128) >> 8;
        uint8_t R = clamp255(r);
        uint8_t G = clamp255(g);
        uint8_t B = clamp255(b);
        out[i_out++] = (0xFFu << 24) | (R << 16) | (G << 8) | (B);

        // 转换第二个像素
        C = y1 - 16;
        r = (298 * C + 409 * E + 128) >> 8;
        g = (298 * C - 100 * D - 208 * E + 128) >> 8;
        b = (298 * C + 516 * D + 128) >> 8;
        R = clamp255(r);
        G = clamp255(g);
        B = clamp255(b);
        out[i_out++] = (0xFFu << 24) | (R << 16) | (G << 8) | (B);
    }
}

/**
 * 增加了重试功能的ioctl
 */
int xioctl(int fd, int request, void *arg)
{
    int r;
    int retries = 0;
    const int max_retries = 20; // 尝试20次
    
    do {
        r = ioctl(fd, request, arg);
        
        // 如果成功，直接返回
        if (r == 0) return 0;

        // 如果失败且原因是 Device busy，则等待并重试
        if (errno == EBUSY) {
            printf("Camera busy in ioctl cmd %d, waiting... (%d/%d)\n", request, retries+1, max_retries);
            usleep(100000); // 等待 100ms
            retries++;
        } else {
            // 如果是其他错误（如 EINVAL），直接返回错误
            return r;
        }
    } while (retries < max_retries);

    return r; // 超时仍失败
}

int main(void)
{
    fprintf(stdout, "camera runing!\n");

    camera_fd = open("/dev/video1", O_RDWR); // 或者你使用的具体打开函数
        
    if (camera_fd <= 0) {
        printf("Camera open err\n");
        return -1;; 
    }
        
    //查询设备功能
    struct v4l2_capability vcap;
    if(xioctl(camera_fd, VIDIOC_QUERYCAP, &vcap) < 0)
    {
        perror("ioctl1 err");
        close(camera_fd);
        return -1;
    }

    // 判断是否是视频采集设备
    if (!(V4L2_CAP_VIDEO_CAPTURE & vcap.capabilities)) {
        fprintf(stderr, "Error: No capture video device!\n");
        return -1;
    }

    // //枚举出摄像头所支持的所有像素格式以及描述信息
    // struct v4l2_fmtdesc fmtdesc;
    // fmtdesc.index = 0;
    // fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // while (0 == ioctl(camera_fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
    //     printf("fmt: %s <0x%x>\n", fmtdesc.description, fmtdesc.pixelformat);
    //     fmtdesc.index++;
    // }

    // //枚举摄像头所支持的所有视频采集分辨率
    // struct v4l2_frmsizeenum frmsize;
    // frmsize.index = 0;
    // frmsize.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // frmsize.pixel_format = V4L2_PIX_FMT_YUYV;
    // while (0 == ioctl(camera_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize)) {
    //     printf("frame_size<%d*%d>\n", frmsize.discrete.width, frmsize.discrete.height);
    //     frmsize.index++;
    // } 

    // //枚举摄像头所支持的所有视频采集帧率
    // struct v4l2_frmivalenum frmival;
    // frmival.index = 0;
    // frmival.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // frmival.pixel_format = V4L2_PIX_FMT_YUYV;
    // frmival.width = CAM_W;
    // frmival.height = CAM_H;
    // while (0 == ioctl(camera_fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival)) {
    //     printf("Frame interval<%ffps> \n", (float)frmival.discrete.denominator / (float)frmival.discrete.numerator);
    //     frmival.index++;
    // }

    struct v4l2_format fmt;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (0 > ioctl(camera_fd, VIDIOC_G_FMT, &fmt)) { //获取格式信息
        perror("ioctl2 error");
        return -1;
    }
    printf("width:%d, height:%d format:%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);

    fmt.fmt.pix.width = CAM_W;
    fmt.fmt.pix.height = CAM_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    if (0 > xioctl(camera_fd, VIDIOC_S_FMT, &fmt)) { //设置格式
        perror("ioctl3 error");
        close(camera_fd);
        return -1;
    }

    //获取当前的流类型相关参数
    struct v4l2_streamparm streamparm;
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(camera_fd, VIDIOC_G_PARM, &streamparm);
    float fps = (float)streamparm.parm.capture.timeperframe.denominator /
                (float)streamparm.parm.capture.timeperframe.numerator;
    printf("Current FPS: %.2f\n", fps);
    
    // //判断是否支持帧率设置
    // if (V4L2_CAP_TIMEPERFRAME & streamparm.parm.capture.capability) {
    // streamparm.parm.capture.timeperframe.numerator = 1;
    // streamparm.parm.capture.timeperframe.denominator = 30;//30fps
    // if (0 > ioctl(fd, VIDIOC_S_PARM, &streamparm)) {//设置流类型相关参数
    //     fprintf(stderr, "ioctl error: VIDIOC_S_PARM: %s\n", strerror(errno));
    // return -1;
    // }
    // }
    // else
    // fprintf(stderr, "不支持帧率设置");

    struct v4l2_requestbuffers reqbuf;
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.count = 3;
    // 申请 3 个帧缓冲
    reqbuf.memory = V4L2_MEMORY_MMAP;
    if (0 > ioctl(camera_fd, VIDIOC_REQBUFS, &reqbuf)) {
        fprintf(stderr, "ioctl error: VIDIOC_REQBUFS: %s\n", strerror(errno));
        return -1;
    }
    
    /* 建立内存映射 */
    struct v4l2_buffer buf;
    void *frm_base[3];
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    for (buf.index = 0; buf.index < 3; buf.index++) {
        ioctl(camera_fd, VIDIOC_QUERYBUF, &buf);
        frm_base[buf.index] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, camera_fd, buf.m.offset);
        if (MAP_FAILED == frm_base[buf.index]) {
            perror("mmap error");
            return -1;
        }
    }

    //入队操作
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    for (buf.index = 0; buf.index < 3; buf.index++) {
        if (0 > ioctl(camera_fd, VIDIOC_QBUF, &buf)) {
            perror("ioctl error");
            return -1;
        }
    }

    //开启视频采集
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (0 > ioctl(camera_fd, VIDIOC_STREAMON, &type)) {
        perror("ioctl error");
        return -1;
    }



    int shm_frm_fd;
    if((shm_frm_fd = shm_open(SHM_FRM_NAME, O_RDWR, 0644)) < 0)
    {
        fprintf(stderr, "camera:shm_open %s err, errno=%d(%s)\n",SHM_FRM_NAME,errno,strerror(errno));
        return -1;
    }

    if(ftruncate(shm_frm_fd, buf.length * 2) < 0)
    {
        perror("ftruncate err");
        return -1;
    }
    
    uint32_t *frm;
    if((frm = mmap(NULL, buf.length * 2, PROT_READ | PROT_WRITE, MAP_SHARED, shm_frm_fd, 0)) == MAP_FAILED)
    {
        perror("mmap err");
        return -1;
    }

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    for ( ; ; ) {
        for(buf.index = 0; buf.index < 3; buf.index++) {
            ioctl(camera_fd, VIDIOC_DQBUF, &buf);
            //出队
            // 读取帧缓冲的映射区、获取一帧数据
            // 处理这一帧数据
            yuyv_to_argb8888(frm_base[buf.index], frm, CAM_W, CAM_H);
            // 数据处理完之后、将当前帧缓冲入队、接着读取下一帧数据
            ioctl(camera_fd, VIDIOC_QBUF, &buf);
        }
    }

    //结束视频采集
    if (0 > ioctl(camera_fd, VIDIOC_STREAMOFF, &type)) {
        perror("ioctl error");
        return -1;
    }

    return 0;
}