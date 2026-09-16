// main.cpp —— 发送端入口：串起「打开 → 初始化 → 采集线程 → 分片发 UDP → 收尾」整条流程
// 接收端是另一边的程序（udpapp::udpdata_rece），两块板子各跑一个。

#include <iostream>
#include <thread>
#include <functional>   // std::ref()
#include <cstdint>      // uint32_t
#include "v4l2app.hpp"
#include "frame.hpp"
#include "udpapp.hpp"

int main()
{
    v4l2_APP cam;

    // ① 打开摄像头（设备号按实际 ls /dev/video* 改）
    if (!cam.openDevice("/dev/video0"))
    {
        std::cerr << "open device failed" << std::endl;
        return 1;
    }

    // ② 目标格式：640×480 NV12（多平面：Y 平面 + UV 平面，共 2 个 plane）
    //   NV12 是半平面 YUV：一个 Y 平面 + 一个交错的 UV 平面，物理上是两段内存，
    //   所以 CSI 设备把采集节点暴露成 multi-planar，必须用 _MPLANE API 采集。
    v4l2_format fmt = {0};
    fmt.type                  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // ← 多平面
    fmt.fmt.pix_mp.width       = 640;
    fmt.fmt.pix_mp.height      = 480;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;   // ← 多平面格式
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes  = 2;   // 给驱动的期望值；S_FMT 后以读回的为准

    v4l2_work work;   // 成员自带默认初始化器，无需 {0}（C++11 下 {0} 会编译报错）

    // ③ 初始化采集（QUERYCAP/S_FMT/REQBUFS/mmap/QBUF/STREAMON，一次性）
    if (cam.v4l2_getframe(work, fmt) < 0)
    {
        std::cerr << "capture init failed" << std::endl;
        return 1;
    }

    // ④ 有界队列（最多 4 帧），采集线程和发送线程共用这一个
    FrameQueue queue(4);

    // ⑤ 启动采集线程。std::ref 必须加：FrameQueue 里有 mutex，默认按值拷贝会编译报错
    std::thread producer(&v4l2_APP::loopread, &cam, std::ref(queue));

    // ⑥ UDP 发送对象（构造时已建 socket + 填目标地址；IP 在 udpapp.cpp 构造里改）
    udpapp net;

    // ⑦ 主线程当发送端：从队列取帧 → 拆包 → 发 UDP
    uint32_t frame_id = 0;
    Frame f;
    while (queue.pop(f))
    {
        net.udpdata_send(f.data, f.size, frame_id++);
        delete[] f.data;   // 生产者 new、消费者 delete，用完释放！
    }

    cam.stopCapture();     // running_ = false，让采集线程退出
    producer.join();       // 等采集线程真正结束
    return 0;
}