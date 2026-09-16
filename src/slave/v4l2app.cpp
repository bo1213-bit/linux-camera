// v4l2app.cpp —— 多平面(Multi-planar) V4L2 相机采集类实现（学习版）
// 与单平面版的区别：一帧不再是"一块连续内存"，而是 N 个 plane（NV12 = Y 平面 + UV 平面）。
//   1. 缓冲类型用 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE（凡是单平面的 _CAPTURE 全换 _MPLANE）
//   2. v4l2_buffer 要挂一个 v4l2_plane 数组：buf.m.planes = planes; buf.length = plane个数
//   3. mmap/QBUF/DQBUF 都按 plane 逐个处理；mmap 偏移用 planes[j].m.mem_offset
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

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type); // 停流（若没开流，返回错被忽略，无害）

    // 多平面：每个 buffer 的每个 plane 都要 munmap（单平面只 munmap 一块，现在逐 plane）
    for (unsigned i = 0; i < nBuffers_; i++)
        for (unsigned j = 0; j < nPlanes_; j++)
            if (buffers_[i][j] && buffers_[i][j] != MAP_FAILED)
                munmap(buffers_[i][j], buffersLen_[i][j]);

    ::close(fd_); // 用全局 close，关闭设备文件描述符
    fd_ = -1;
    return true;
}

// 初始化采集：QUERYCAP → S_FMT → S_PARM → REQBUFS → QUERYBUF/mmap → QBUF → STREAMON
// 多平面版：每步的 type 都用 _MPLANE；buffer 挂 plane 数组；mmap 按 plane 逐个做。
int v4l2_APP::v4l2_getframe(v4l2_work app_work, v4l2_format fmt_my)
{
    // ① 查能力 —— 顺带打印是不是多平面，正好回答"怎么知道它是多平面"
    if (ioctl(fd_, VIDIOC_QUERYCAP, &app_work.cap) < 0)
    {
        std::cerr << "QUERYCAP failed" << std::endl;
        return -1;
    }
    // device_caps 比 capabilities 更准（后者含整个驱动含 subdev）；
    // 仅当 capabilities 里有 V4L2_CAP_DEVICE_CAPS 置位时 device_caps 才有效。
    __u32 caps = (app_work.cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                 ? app_work.cap.device_caps
                 : app_work.cap.capabilities;
    bool isMplane = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE;
    bool isSingle = caps & V4L2_CAP_VIDEO_CAPTURE;
    std::cout << "[QUERYCAP] "
              << (isMplane ? "Multi-planar" : "")
              << (isMplane && isSingle ? " + " : "")
              << (isSingle ? "Single-planar" : "")
              << "  caps=0x" << std::hex << caps << std::dec << std::endl;
    // 若这里"只看到 Multi-planar、没有 Single-planar"，说明这台设备必须走 _MPLANE API，
    // 原来的单平面代码必失败——这就是排查结论。

    // ② 设格式 —— 用 fmt.fmt.pix_mp，type 用 _MPLANE
    app_work.fmt = fmt_my;   // fmt_my.type 在 main 里已设成 _MPLANE
    if (ioctl(fd_, VIDIOC_S_FMT, &app_work.fmt) < 0)
    {
        std::cerr << "S_FMT failed" << std::endl;
        return -1;
    }
    // ⚠️ 必须读回：驱动协商后的 num_planes / 每平面大小才是真的，别信 main 里填的值
    nPlanes_ = app_work.fmt.fmt.pix_mp.num_planes;
    if (nPlanes_ == 0 || nPlanes_ > VIDEO_MAX_PLANES)
    {
        std::cerr << "bad num_planes=" << nPlanes_ << std::endl;
        return -1;
    }

    // ③ 帧率（type 也要 _MPLANE）
    app_work.parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd_, VIDIOC_G_PARM, &app_work.parm);
    app_work.parm.parm.capture.timeperframe.numerator = 1;
    app_work.parm.parm.capture.timeperframe.denominator = 30; // 30 fps
    ioctl(fd_, VIDIOC_S_PARM, &app_work.parm);

    // ④ 申请缓冲 —— type 用 _MPLANE
    struct v4l2_requestbuffers req;
    req.count = 4;                                       // 申请 4 个缓冲
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;                       // 零拷贝
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
    {
        std::cerr << "Failed to request buffers" << std::endl;
        return -1;
    }
    nBuffers_ = req.count; // 记录内核实际给的个数，后面 munmap 循环要用它，别硬写 4

    // ⑤ 查每块 buffer + 按 plane 逐个 mmap + 入队
    //    这三步是"开流前的准备"，一个字节的画面数据到 STREAMON+DQBUF 才进来：
    //    QUERYBUF = 问内核这块 plane 的长度/偏移；mmap = 把内核内存映射进用户空间；
    //    QBUF = 把空缓冲交还内核（"空盒子上传送带，让司机装货"）。
    for (unsigned i = 0; i < nBuffers_; i++)
    {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}}; // 这一块 buffer 的 plane 信息（输出参数）
        struct v4l2_buffer buf = {0};
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = i;       // ← index 是输入：我要查第 i 块
        buf.m.planes = planes;  // ← 多平面关键：把 plane 数组挂上去
        buf.length   = nPlanes_; // ← 告诉驱动 planes 数组里有几个 plane

        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
        {
            std::cerr << "Failed to query buffer" << std::endl;
            return -1;
        }
        // 查完后 planes[j].length / planes[j].m.mem_offset 就绪

        // 每个 plane 单独 mmap（单平面一个 buffer 只 mmap 一次，多平面按 plane 逐个）
        for (unsigned j = 0; j < nPlanes_; j++)
        {
            void *mapped = mmap(NULL,                            // 1. 地址：让内核挑
                                planes[j].length,                // 2. 长度：QUERYBUF 查出来的
                                PROT_READ | PROT_WRITE,          // 3. 权限：可读可写
                                MAP_SHARED,                      // 4. 标志：共享（零拷贝关键）
                                fd_,                             // 5. 设备 fd
                                planes[j].m.mem_offset);         // 6. 偏移：注意是 mem_offset（单平面是 buf.m.offset）
            if (mapped == MAP_FAILED) // mmap 失败返回 MAP_FAILED，不是 NULL
            {
                std::cerr << "mmap buf " << i << " plane " << j << " failed" << std::endl;
                return -1;
            }
            buffers_[i][j]    = mapped;          // 保存映射指针
            buffersLen_[i][j] = planes[j].length; // 保存长度，munmap / QBUF 填 length 都要用
        }

        // 紧接着入队（planes[j].length 已由 QUERYBUF 填好，直接复用）
        // ⚠️ 这步是开流前必做：没 QBUF，内核不知道这块归它用，STREAMON 后永远出不了帧。
        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0)
        {
            std::cerr << "Failed to queue buffer" << std::endl;
            return -1;
        }
    }

    // ⑥ 开流（type 用 _MPLANE，传 &type 不是 &buf！）
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0)
    {
        std::cerr << "STREAMON failed" << std::endl;
        return -1;
    }

    return 1;
}

// 采集线程主循环：select 等帧 → DQBUF（按 plane 取）→ 拼成一块 → 立刻 QBUF → 塞队列
// 下游 FrameQueue / UDP 看到的还是 Frame{data, size} 老接口，不用改——
// 因为这里把多 plane 拼成了一块连续内存再 push。
void v4l2_APP::loopread(FrameQueue &que)
{
    struct v4l2_buffer buf = {0};   // {0} 清零，别留垃圾值
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;

    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {{0}}; // DQBUF 时驱动往这里写每个 plane 的实际数据信息
    buf.m.planes = planes;
    buf.length   = nPlanes_;

    while (running_)
    {
        // ① select 带 2 秒超时等帧：有帧才往下走；超时则回循环头，顺带查 running_
        fd_set fds;
        struct timeval tv = {2, 0};
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        if (select(fd_ + 1, &fds, NULL, NULL, &tv) <= 0) continue;

        // ② 取帧（出队：内核告诉我是第几块，planes[j].bytesused 告诉我每个 plane 实际多少字节）
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) continue;

        // ③ 把多个 plane 拼成一块连续内存 —— 这样下游 FrameQueue/UDP 完全不用改
        //    注意用 bytesused（实际负载）不是 length（固定容量）；再减 data_offset（一般 0，严谨写法要减）
        size_t total = 0;
        for (unsigned j = 0; j < nPlanes_; j++)
            total += planes[j].bytesused - planes[j].data_offset;

        unsigned char *copy = new unsigned char[total];
        size_t off = 0;
        for (unsigned j = 0; j < nPlanes_; j++)
        {
            size_t len = planes[j].bytesused - planes[j].data_offset;
            memcpy(copy + off,
                   (unsigned char *)buffers_[buf.index][j] + planes[j].data_offset,
                   len);
            off += len;
        }

        // ④ 还回前，把每个 plane 的 length 填好（QBUF 要校验 length）
        for (unsigned j = 0; j < nPlanes_; j++)
            planes[j].length = buffersLen_[buf.index][j];
        ioctl(fd_, VIDIOC_QBUF, &buf); // 立刻还回（别等消费端！内核马上能继续用这块）

        // ⑤ 塞队列（包成 Frame；满了队列自己丢最旧帧）
        que.push(Frame(copy, total));
    }
}

// 停止采集循环：把 running_ 置 false，loopread 下一轮 while 检查到就退
void v4l2_APP::stopCapture()
{
    running_ = false;
}
