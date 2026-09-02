// v4l2app.cpp —— Linux V4L2 相机采集程序（学习版）
// 目标：用 C++ 类封装 V4L2 采集流程，目前已完成类框架，逐步往里填 ioctl 实现。

#include <iostream>
#include <linux/videodev2.h> // V4L2 所有结构体和 ioctl 宏都在这里
#include <fcntl.h>           // open() / O_RDWR 等
#include <unistd.h>          // close() 等
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>         // snprintf() 拼文件名
#include <sys/select.h>    // select() / fd_set
#include <cstring>         // memcpy()
#include <thread>          // std::thread
#include <atomic>          // std::atomic<bool>
#include <functional>      // std::ref()
#include "frame.hpp"       // 本地头文件用引号


// v4l2_work：把 V4L2 采集过程中要用到的几个参数结构体集中在一起，
// 便于在类的成员函数之间整体传递（也是 v4l2_getfarem 的参数）。
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
    // 用 VIDIOC_REQBUFS 向内核申请缓冲，字段及数据类型：
    //   req.count  __u32  想要几个缓冲（如 4 个循环复用）；调用后变成实际分配个数
    //   req.type   __u32  缓冲类型（V4L2_BUF_TYPE_VIDEO_CAPTURE）
    //   req.memory __u32  内存方式（V4L2_MEMORY_MMAP，零拷贝）
    struct v4l2_requestbuffers req = {0};

    // parm —— 流参数（struct v4l2_streamparm）。
    // 用 VIDIOC_S_PARM 设置采集帧率，字段及数据类型：
    //   parm.type                                  __u32  类型
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
    ~v4l2_APP() { closeDevice(); }

    // 打开摄像头设备（如 "/dev/video0"），成功返回 true
    bool openDevice(const char *dev);

    // 获取一帧画面（待实现 V4L2 采集核心逻辑）
    int v4l2_getframe(v4l2_work app_work, v4l2_format fmt_my);

    void loopread(FrameQueue &que);

    // 停止采集循环：把 running_ 置 false，让 loopread 退出
    void stopCapture();

    // 关闭设备、释放资源
    bool closeDevice();

private:
    int fd_;                   // 摄像头设备的文件描述符，私有，外部不可直接访问
    struct v4l2_work work_que; // V4L2 参数集合，成员函数之间共用
    void *buffers_[4];         // 映射的缓冲区指针数组，最多 4 个缓冲
    size_t buffersLen_[4];     // 每个缓冲映射的长度（munmap 解除映射时要用）
    unsigned nBuffers_ = 0;             // 实际分配的缓冲个数（REQBUFS 返回的 req.count）
    std::atomic<bool> running_{true};   // 采集循环标志位，loopread() 循环用；跨线程读写必须原子
};

// 打开设备：调用系统 open() 拿到 fd
bool v4l2_APP::openDevice(const char *dev)
{
    fd_ = open(dev, O_RDWR); // 注意：这里 open 是全局的系统调用（成员函数已改名，无冲突）
    if (fd_ < 0)
    {
        return false; // 打开失败（设备不存在/被占用/无权限）
    }
    else
    {
        return true;
    }
}

// 关闭设备：停流 → 解除映射 → 关闭 fd（逆着采集步骤释放资源）
bool v4l2_APP::closeDevice()
{
    if (fd_ < 0)
        return false; // 从未打开，没有可清理的东西

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type); // 停流（若没开流，返回错被忽略，无害）

    for (unsigned i = 0; i < nBuffers_; i++)
        munmap(buffers_[i], buffersLen_[i]); // 一个个解除映射

    ::close(fd_); // 用全局 close，关闭设备文件描述符
    fd_ = -1;
    return true;
}

// 获取一帧（TODO: 待实现，按文档的 10 步流程填入 QUERYCAP/S_FMT/REQBUFS/mmap/QBUF/DQBUF 等）
int v4l2_APP::v4l2_getframe(v4l2_work app_work, v4l2_format fmt_my)
{
    // TODO: 实现采集逻辑后，return 0 或实际帧大小
    char capabilte[3] = {0};
    if (ioctl(fd_, VIDIOC_QUERYCAP, &app_work.cap) < 0)
    {
        return -1;
    }

    app_work.fmt = fmt_my;
    ioctl(fd_, VIDIOC_S_FMT, &app_work.fmt);

    app_work.parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_G_PARM, &app_work.parm);
    app_work.parm.parm.capture.timeperframe.numerator = 1;
    app_work.parm.parm.capture.timeperframe.denominator = 30; // 30 fps
    ioctl(fd_, VIDIOC_S_PARM, &app_work.parm);

    struct v4l2_requestbuffers req;
    req.count = 4; // 申请 4 个缓冲
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP; // 零拷贝

    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
    {
        std::cerr << "Failed to request buffers" << std::endl;
        return -1;
    }
    nBuffers_ = req.count; // 记录内核实际给的个数，后面 munmap 循环要用它，别硬写 4

    struct v4l2_buffer buf;
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    for (int i = 0; i < req.count; i++)
    {
        buf.index = i;
        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
        {
            std::cerr << "Failed to query buffer" << std::endl;
            return -1;
        }

        // 查完 buffer 信息后，buf.length 和 buf.m.offset 已经就绪，接着映射：
        void *mapped = mmap(NULL,                   // 1. 地址：让内核挑
                            buf.length,             // 2. 长度：查出来的
                            PROT_READ | PROT_WRITE, // 3. 权限：可读可写
                            MAP_SHARED,             // 4. 标志：共享（零拷贝关键）
                            fd_,                    // 5. 设备：你的摄像头 fd
                            buf.m.offset);          // 6. 偏移：查出来的

        if (mapped == MAP_FAILED) // mmap 失败返回 MAP_FAILED，不是 NULL
        {
            std::cerr << "mmap failed" << std::endl;
            return -1;
        }

        buffers_[i] = mapped;        // 保存映射后的指针，后续可直接访问
        buffersLen_[i] = buf.length; // 保存这块的长度，munmap 时要用

        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
        {
            std::cerr << "Failed to queue buffer" << std::endl;
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0)
    {
        std::cerr << "STREAMON failed" << std::endl;
        return -1;
    }


    return 1;
}

void v4l2_APP::loopread(FrameQueue &que)
{
    struct v4l2_buffer buf = {0};   // {0} 清零，别留垃圾值
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    while (running_)
    {
        // ① select 带 2 秒超时等帧：有帧才往下走；超时则回循环头，顺带查 running_
        fd_set fds;
        struct timeval tv = {2, 0};
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        if (select(fd_ + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        // ② 取帧（出队：内核告诉我是第几块）
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) continue;

        // ③ 拷一份出来 —— 数据从此归队列管，跟内核那 4 块 mmap 缓冲无关
        unsigned char *copy = new unsigned char[buf.bytesused];
        memcpy(copy, buffers_[buf.index], buf.bytesused);

        // ④ 立刻还回（别等消费端！内核马上能继续用这块）
        ioctl(fd_, VIDIOC_QBUF, &buf);

        // ⑤ 塞队列（包成 Frame；满了队列自己丢最旧帧）
        que.push(Frame(copy, buf.bytesused));
    }
}

// 停止采集循环：把 running_ 置 false，loopread 下一轮 while 检查到就退
void v4l2_APP::stopCapture()
{
    running_ = false;
}

// 程序入口：串起「打开 → 初始化 → 采集线程 → 消费 → 收尾」整条流程
int main()
{
    v4l2_APP cam;

    // ① 打开摄像头（设备号按实际 ls /dev/video* 改）
    if (!cam.openDevice("/dev/video0"))
    {
        std::cerr << "open device failed" << std::endl;
        return 1;
    }

    // ② 目标格式：640×480 MJPEG（具体看摄像头 v4l2-ctl --list-formats-ext 支持啥）
    v4l2_format fmt = {0};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = 640;
    fmt.fmt.pix.height      = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; // 也可能是 YUYV，按摄像头定
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    v4l2_work work = {0};

    // ③ 初始化采集（QUERYCAP/S_FMT/REQBUFS/mmap/QBUF/STREAMON，一次性）
    if (cam.v4l2_getframe(work, fmt) < 0)
    {
        std::cerr << "capture init failed" << std::endl;
        return 1;
    }

    // ④ 有界队列（最多 4 帧），采集线程和消费线程共用这一个
    FrameQueue queue(4);

    // ⑤ 启动采集线程。std::ref 必须加：FrameQueue 里有 mutex，默认按值拷贝会编译报错
    std::thread producer(&v4l2_APP::loopread, &cam, std::ref(queue));

    // ⑥ 主线程当消费者：验证队列里拿到的是完整帧（下一步换成拆包发 UDP）
    int n = 0;
    Frame f;
    while (queue.pop(f))
    {
        char name[64];
        snprintf(name, sizeof(name), "frame_%d.jpg", n++);
        int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0)
        {
            write(fd, f.data, f.size);
            close(fd);
        }
        delete[] f.data;   // 生产者 new、消费者 delete，用完释放！
    }

    cam.stopCapture();     // running_ = false，让采集线程退出
    producer.join();       // 等采集线程真正结束
    return 0;
}
