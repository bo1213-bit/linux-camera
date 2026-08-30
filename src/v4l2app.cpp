// v4l2app.cpp —— Linux V4L2 相机采集程序（学习版）
// 目标：用 C++ 类封装 V4L2 采集流程，目前已完成类框架，逐步往里填 ioctl 实现。

#include <iostream>
#include <linux/videodev2.h>   // V4L2 所有结构体和 ioctl 宏都在这里
#include <fcntl.h>             // open() / O_RDWR 等
#include <unistd.h>            // close() 等
#include <sys/ioctl.h> 


// v4l2_work：把 V4L2 采集过程中要用到的几个参数结构体集中在一起，
// 便于在类的成员函数之间整体传递（也是 v4l2_getfarem 的参数）。
struct v4l2_work{
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
class v4l2_APP{
    public:
    // 构造：fd_ 初始化为 -1，表示"尚未打开"
    v4l2_APP():fd_(-1){}

    // 析构：对象销毁时自动关闭设备，防止 fd 泄漏
    ~v4l2_APP(){closeDevice();}

    // 打开摄像头设备（如 "/dev/video0"），成功返回 true
    bool openDevice(const char* dev);

    // 获取一帧画面（待实现 V4L2 采集核心逻辑）
    int v4l2_getfarem(v4l2_work app_work,v4l2_format fmt_my);

    // 关闭设备、释放资源
    bool closeDevice();


    private:
    int fd_;                    // 摄像头设备的文件描述符，私有，外部不可直接访问
    struct v4l2_work work_que;  // V4L2 参数集合，成员函数之间共用
};

// 打开设备：调用系统 open() 拿到 fd
bool v4l2_APP::openDevice(const char* dev)
{
    fd_=open(dev,O_RDWR);   // 注意：这里 open 是全局的系统调用（成员函数已改名，无冲突）
    if(fd_<0)
    {
        return false;       // 打开失败（设备不存在/被占用/无权限）
    }else
    {
        return true;
    }
}

// 关闭设备：释放 fd（TODO: 目前是空实现，需补上 ::close(fd_) 与 STREAMOFF/munmap）
bool v4l2_APP::closeDevice()
{
    return true;
}

// 获取一帧（TODO: 待实现，按文档的 10 步流程填入 QUERYCAP/S_FMT/REQBUFS/mmap/QBUF/DQBUF 等）
int v4l2_APP::v4l2_getfarem(v4l2_work app_work,v4l2_format fmt_my)
{
    // TODO: 实现采集逻辑后，return 0 或实际帧大小
    char capabilte[3]={0};
    if(ioctl(fd_,VIDIOC_QUERYCAP,&app_work.cap)<0)
    {
        return -1;
    }

    app_work.fmt=fmt_my;
    ioctl(fd_, VIDIOC_S_FMT, &app_work.fmt);

    app_work.parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_G_PARM, &app_work.parm);
    app_work.parm.parm.capture.timeperframe.numerator   = 1;
    app_work.parm.parm.capture.timeperframe.denominator = 30;      // 30 fps
    ioctl(fd_, VIDIOC_S_PARM, &app_work.parm);   



    return 1;
}
