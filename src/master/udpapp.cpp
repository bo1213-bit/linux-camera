// udpapp.cpp —— 主机端：UDP 分片接收 + 重组 + 管道喂 ffplay 实时预览
// （与从机 src/slave/udpapp.cpp 协议一致）
//
// 协议回顾（和从机 udpapp.cpp 的发送端对齐）：
//   每个包 = FragHeader(12字节) + 一段图像数据(≤1400字节)
//   一帧被拆成若干片发出，片0带新帧号开始，末片 FLAG_END 置位
//   接收端按 frag_seq 把各片填到累积区对应位置，收齐(frag_total 片全到)即整帧
//
// 实时预览原理：
//   不再 open/写/close 文件，而是用 popen 启一个 ffplay 子进程，把它的 stdin
//   当成"一个会动的视频文件"往里写。ffplay 被告知输入是 640x480 的 NV12 裸流，
//   只要不停把整帧 fwrite 进去，它就会逐帧解码显示，形成实时画面。

#include "udpapp.hpp"

#include <sys/socket.h>
#include <arpa/inet.h>   // htons() / inet_addr() / INADDR_ANY
#include <cstring>      // memset() / memcpy()
#include <cstdio>       // snprintf() / popen() / pclose() / fwrite()
#include <unistd.h>     // close()
#include <iostream>

// ===== 接收端辅助函数（文件内私有，只在本 .cpp 用）=====

// 开新帧：清空旧的组装区，按新帧号分配累积区与 got 数组
static void beginFrame(Assembler &as, const FragHeader &hdr)
{
    delete[] as.buf;  as.buf  = nullptr;
    delete[] as.got;  as.got  = nullptr;

    as.cur_frame_id = hdr.frame_id;
    as.frag_total   = hdr.frag_total;
    as.buf = new uint8_t[(size_t)hdr.frag_total * MAX_PAYLOAD];
    as.got = new bool[hdr.frag_total]();   // 括号 = 值初始化，全 false
    as.got_count = 0;
    as.last_len  = 0;
}

// 收齐交付后：清空组装区，等下一帧的片 0 来开张
static void resetFrame(Assembler &as)
{
    delete[] as.buf;  as.buf  = nullptr;
    delete[] as.got;  as.got  = nullptr;
    as.cur_frame_id = 0;
    as.frag_total   = 0;
    as.got_count    = 0;
    as.last_len     = 0;
}

// ===== 构造：建接收 socket + 绑定端口 + 启动 ffplay =====
udpapp::udpapp() : rsock_(-1), play_(nullptr)
{
    // ① 建一个数据报 socket（接收用）
    rsock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (rsock_ < 0)
    {
        std::cerr << "create socket failed" << std::endl;
        return;
    }

    // ② 绑定接收端口：占住 5004，和发送端约定一致
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(RECV_PORT);   // 占住 5004，和从机发送目标端口一致
    local.sin_addr.s_addr = INADDR_ANY;          // 不挑来源 IP，谁来都收
    if (bind(rsock_, (struct sockaddr *)&local, sizeof(local)) < 0)
    {
        std::cerr << "bind port " << RECV_PORT << " failed" << std::endl;
        close(rsock_);
        rsock_ = -1;
        return;
    }

    // ③ 启动 ffplay 子进程，把它的 stdin 接成管道。
    //   ffplay 参数：-f rawvideo 按裸流读；-video_size / -pixel_format 告诉它分辨率和格式；
    //   -fflags nobuffer -flags low_delay 尽量低延迟；- 进入时从 stdin 读数据。
    //   ⚠️ 新版 ffplay(6.x)不认 -pix_fmt，要用 -pixel_format，否则报
    //      "Failed to set value 'nv12' for option 'pix_fmt': Option not found" 退出。
    //   （管道是单向的：本程序往 play_ 写 → ffplay 从它的 stdin 读。）
    play_ = popen("ffplay -f rawvideo -video_size 640x480 -pixel_format nv12 "
                  "-fflags nobuffer -flags low_delay -autoexit -", "w");
    if (!play_)
    {
        std::cerr << "popen ffplay failed (装 ffmpeg 了吗？)" << std::endl;
    }
    else
    {
        std::cout << "[receiver] listening on port " << RECV_PORT
                  << "  →  ffplay " << FRAME_W << "x" << FRAME_H << " NV12"
                  << std::endl;
    }
}

udpapp::~udpapp()
{
    if (play_) pclose(play_);
    if (rsock_ >= 0) close(rsock_);
}

// ===== 接收端：绑端口 → 收包 → 按序号落槽 → 收齐写进 ffplay 管道 =====
void udpapp::udpdata_rece()
{
    if (rsock_ < 0)
    {
        std::cerr << "socket not ready" << std::endl;
        return;
    }
    if (!play_)
    {
        std::cerr << "ffplay not running" << std::endl;
        return;
    }

    Assembler as;                 // "正在拼的帧"状态
    uint8_t pkt[1500];            // 收包缓冲（够装单个 UDP 包）
    struct sockaddr_in src;       // recvfrom 填：谁发来的（本程序不管它）
    socklen_t srclen = sizeof(src);

    int shown = 0;                // 已显示帧数

    while (true)
    {
        int n = recvfrom(rsock_, pkt, sizeof(pkt), 0, (struct sockaddr *)&src, &srclen);
        if (n < (int)sizeof(FragHeader)) continue;   // 连头都不够，丢

        FragHeader hdr;
        memcpy(&hdr, pkt, sizeof(hdr));
        uint8_t *data     = pkt + sizeof(hdr);
        int      data_len = n - sizeof(hdr);

        if (hdr.magic != FRAG_MAGIC) continue;       // 不是我们的包
        if (hdr.frag_total == 0)    continue;       // 异常包

        // ② 新帧开始？（片 0 且 帧号变了）→ 清空旧的，按新帧初始化
        if (hdr.frag_seq == 0 && hdr.frame_id != as.cur_frame_id)
            beginFrame(as, hdr);

        // ③ 属于当前帧吗？不是就丢（旧帧迟到的片 / 乱序）
        if (hdr.frame_id != as.cur_frame_id) continue;

        // ④ 序号越界防御：seq 超出本帧片数，丢（防止数组越界写）
        if (hdr.frag_seq >= as.frag_total) continue;

        // ⑤ 填槽（重复片跳过，不重复计数）
        if (!as.got[hdr.frag_seq])
        {
            as.got[hdr.frag_seq] = true;
            as.got_count++;
            memcpy(as.buf + (size_t)hdr.frag_seq * MAX_PAYLOAD, data, data_len);
            if (hdr.flags & FLAG_END) as.last_len = data_len;   // 记末片真实长度
        }

        // ⑥ 收齐了？还原整帧大小 = 前面满片 + 末片真实长度
        if (as.got_count == as.frag_total)
        {
            size_t frame_size = (size_t)(as.frag_total - 1) * MAX_PAYLOAD + as.last_len;

            // 拼好了！整帧写进 ffplay 的 stdin，ffplay 窗口实时刷新这一帧
            // （fwrite 是阻塞写：ffplay 消费慢时会自然形成反压，不会爆缓冲）
            fwrite(as.buf, 1, frame_size, play_);
            fflush(play_);   // 立刻推给 ffplay，别在 FILE 缓冲里积着

            if (++shown % 30 == 0)   // 每 30 帧打印一行，证明还在动（不刷屏）
                std::cout << "[receiver] " << shown << " frames shown (frame_id="
                          << as.cur_frame_id << ")" << std::endl;

            resetFrame(as);    // 清空，等下一帧的片 0
        }
    }

    close(rsock_);
}
