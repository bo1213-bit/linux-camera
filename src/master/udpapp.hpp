#pragma once

#include <cstdint>      // uint32_t/uint16_t/uint8_t
#include <cstddef>      // size_t
#include <cstdio>       // FILE*（popen 管道）
#include <netinet/in.h> // struct sockaddr_in

// —— 分片协议常量（必须与从机 src/slave/udpapp.hpp 完全一致，否则收发对不上）——
#define MAX_PAYLOAD 1400   // 每片最多装的图像字节数（留足 IP/UDP 头空间）
#define FRAG_MAGIC  0xA5   // 魔数：接收端先对一下，防止接错数据
#define FLAG_END    0x01   // flags 的 bit0：本片是这一帧的最后一片
#define RECV_PORT   5004   // 接收端口（与从机发送目标端口一致）

// —— 图像格式（与从机 main.cpp 的 S_FMT 一致；ffplay 要靠它解析裸流）——
#define FRAME_W 640
#define FRAME_H 480

// —— 每个 UDP 包的负载 = 这个头(12字节) + 一段图像数据 ——
// 注意：这里只有元信息，不能放数据指针（指针只在本地有意义，发到对端是废地址）
struct FragHeader {
    uint32_t frame_id;    // 帧号（每帧+1，同一帧的所有片共享）
    uint16_t frag_seq;    // 分片序号（本帧内第几片，从 0 起）
    uint16_t frag_total;  // 总片数（接收端靠它判断"收齐没有"）
    uint16_t payload;     // 本片实际负载字节数（末片通常不满 1400）
    uint8_t  flags;       // 位标志：bit0 = FLAG_END（末片）
    uint8_t  magic;       // 魔数 0xA5
};

// —— 接收端的"正在拼的帧"状态（跨多个 recvfrom 调用保持）——
struct Assembler {
    uint32_t cur_frame_id = 0;        // 正在拼哪一帧
    uint16_t frag_total    = 0;       // 这帧一共几片
    uint8_t *buf           = nullptr; // 累积区：frag_total × MAX_PAYLOAD
    bool    *got           = nullptr; // got[i]：第 i 片收到没有
    int      got_count     = 0;       // 已收到几片（== frag_total 即收齐）
    size_t   last_len      = 0;       // 末片实际字节数（算整帧大小用）
};

// —— 主机端：UDP 接收器，收包 → 按序号重组 → 收齐喂给 ffplay 实时预览 ——
class udpapp {
public:
    // 构造：建接收 socket + 绑定 RECV_PORT + 启动 ffplay 子进程
    udpapp();
    // 析构：关 socket + 关管道
    ~udpapp();

    // 绑定端口，持续收包并按序号重组成一帧，收齐写入 ffplay 管道实时显示（阻塞）
    void udpdata_rece();

private:
    int    rsock_;        // 接收专用 socket
    FILE  *play_;         // ffplay 子进程的标准输入管道（popen 打开）
};
