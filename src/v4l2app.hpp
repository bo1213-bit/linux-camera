#pragma once

#include <linux/videodev2.h> // V4L2 所有结构体和 ioctl 宏都在这里
#include <cstddef>           // size_t
#include <atomic>            // std::atomic<bool>
#include "frame.hpp"         // FrameQueue（loopread 的参数类型用）

// v4l2_work：把 V4L2 采集过程中要用到的几个参数结构体集中在一起，
// 便于在类的成员函数之间整体传递（也是 v4l2_getframe 的参数）。
struct v4l2_work
{
    // cap —— 设备能力描述（struct v4l2_capability）。
    // 用 VIDIOC_QUERYCAP 查询后填充，字段及数据类型如下：
    //   cap.driver       __u8[16]  驱动名字符串（如 "uvcvideo"）
    //   cap.card         __u8[32]  设备名字符串（如 "USB Camera"）
    //   cap.bus_info     __u8[32]  总线信息字符串
    //   cap.version      __u32     驱动版本号
    //   cap.capabilities __u32     能力位标志（判断是否采集 V4L2_CAP_VIDEO_CAPTURE、
    //                               是否支持流式 V4L2_CAP_STREAMING）
    //   cap.device_caps  __u32     设备能力位
    struct v4l2_capability cap;

    // fmt —— 图像格式（struct v4l2_format，实际格式在 fmt.fmt.pix 里）。
    // 用 VIDIOC_S_FMT 设置、VIDIOC_G_FMT 读取，关键字段及数据类型：
    //   fmt.type                  __u32  缓冲类型（V4L2_BUF_TYPE_VIDEO_CAPTURE）
    //   fmt.fmt.pix.width         __u32  分辨率宽（如 640）
    //   fmt.fmt.pix.height        __u32  分辨率高（如 480）
    //   fmt.fmt.pix.pixelformat   __u32  像素格式 fourcc（V4L2_PIX_FMT_YUYV / MJPEG）
    //   fmt.fmt.pix.field         __u32  场序（一般 V4L2_FIELD_NONE）
    //   fmt.fmt.pix.bytesperline  __u32  每行字节数
    //   fmt.fmt.pix.sizeimage     __u32  一帧数据总字节数（存文件/发网络要用它）
    struct v4l2_format fmt = {0};

    // req —— 缓冲申请（struct v4l2_requestbuffers）。
    //   req.count  __u32  想要几个缓冲（如 4）；调用后变成实际分配个数
    //   req.type   __u32  缓冲类型（V4L2_BUF_TYPE_VIDEO_CAPTURE）
    //   req.memory __u32  内存方式（V4L2_MEMORY_MMAP，零拷贝）
    struct v4l2_requestbuffers req = {0};

    // parm —— 流参数（struct v4l2_streamparm），设采集帧率。
    //   parm.parm.capture.timeperframe.numerator   __u32  分子（如 1）
    //   parm.parm.capture.timeperframe.denominator __u32  分母（如 30 → 30fps）
    struct v4l2_streamparm parm = {0};
};

// v4l2_APP —— 相机采集类，用 RAII 管理文件描述符的生命周期。
class v4l2_APP
{
public:
    // 构造：fd_ 初始化为 -1，表示"尚未打开"
    v4l2_APP() : fd_(-1) {}

    // 析构：对象销毁时自动关闭设备，防止 fd 泄漏
    ~v4l2_APP();

    // 打开摄像头设备（如 "/dev/video0"），成功返回 true
    bool openDevice(const char *dev);

    // 初始化采集（QUERYCAP/S_FMT/S_PARM/REQBUFS/mmap/QBUF/STREAMON 一次性做完）
    int v4l2_getframe(v4l2_work app_work, v4l2_format fmt_my);

    // 采集线程循环：select 等帧 → DQBUF → 拷贝 → 塞队列
    void loopread(FrameQueue &que);

    // 停止采集循环：running_ 置 false，让 loopread 退出
    void stopCapture();

    // 关闭设备、释放资源
    bool closeDevice();

private:
    int fd_;                   // 摄像头设备的文件描述符，私有
    struct v4l2_work work_que; // V4L2 参数集合，成员函数之间共用
    void *buffers_[4];         // 映射的缓冲区指针数组，最多 4 个缓冲
    size_t buffersLen_[4];     // 每个缓冲映射的长度（munmap 时要用）
    unsigned nBuffers_ = 0;             // 实际分配的缓冲个数（REQBUFS 返回的 req.count）
    std::atomic<bool> running_{true};   // 采集循环标志位；跨线程读写必须原子
};