// v4l2app.cpp —— Linux V4L2 相机采集类实现（学习版）
// 只保留 v4l2_APP 的方法实现；声明在 v4l2app.hpp，入口在 main.cpp。

#include "v4l2app.hpp"

#include <iostream>
#include <fcntl.h>       // open() / O_RDWR 等
#include <unistd.h>      // close() 等
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>  // select() / fd_set
#include <cstring>       // memcpy()

// 析构：对象销毁时自动关闭设备，防止 fd 泄漏
v4l2_APP::~v4l2_APP()
{
    closeDevice();
}

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

// 初始化采集：QUERYCAP → S_FMT → S_PARM → REQBUFS → QUERYBUF/mmap → QBUF → STREAMON
int v4l2_APP::v4l2_getframe(v4l2_work app_work, v4l2_format fmt_my)
{
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

// 采集线程主循环：select 等帧 → DQBUF → 拷贝一份 → 立刻 QBUF → 塞队列
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