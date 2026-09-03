#include "udpapp.hpp"

#include <sys/socket.h>
#include <arpa/inet.h>   // htons() / inet_addr() / INADDR_ANY
#include <cstring>       // memset() / memcpy()
#include <cstdio>        // snprintf()
#include <unistd.h>      // close()
#include <fcntl.h>       // open()（存文件验证用）

// ===== 接收端辅助函数（文件内私有，只在本 .cpp 用）=====
namespace {

// 开新帧：清空旧的组装区，按新帧号分配累积区与 got 数组
void beginFrame(Assembler &as, const FragHeader &hdr)
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
void resetFrame(Assembler &as)
{
    delete[] as.buf;  as.buf  = nullptr;
    delete[] as.got;  as.got  = nullptr;
    as.cur_frame_id = 0;
    as.frag_total   = 0;
    as.got_count    = 0;
    as.last_len     = 0;
}

} // namespace

// ===== 构造：建发送 socket + 填发送目标 =====
udpapp::udpapp() : sock_(-1)
{
    // ① 建一个数据报 socket（发送用）
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0)
    {
        // 发送失败后续 sendto 会失败，这里简单处理
    }

    // ② 填目标地址：发给谁（接收端 IP + 端口，两端约定一致）
    memset(&dst_, 0, sizeof(dst_));
    dst_.sin_family      = AF_INET;
    dst_.sin_port        = htons(5004);
    dst_.sin_addr.s_addr = inet_addr("192.168.1.100");   // ← 改成接收端的 IP
}

udpapp::~udpapp()
{
    if (sock_ >= 0)
        close(sock_);
}

// ===== 发送端：一帧拆多片发出去 =====
void udpapp::udpdata_send(const unsigned char *data, size_t size, uint32_t frame_id)
{
    // 一共拆几片？ size / 1400 向上取整
    uint16_t total = (uint16_t)((size + MAX_PAYLOAD - 1) / MAX_PAYLOAD);

    for (uint16_t i = 0; i < total; i++)
    {
        size_t off = (size_t)i * MAX_PAYLOAD;   // 本片数据在整帧里的起始偏移
        size_t len = size - off;                // 本片数据长度
        if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;

        // 拼一个「头 + 数据」的完整包（头和数据必须一次 sendto，不能分两次！）
        unsigned char buf[MAX_PAYLOAD + sizeof(FragHeader)];

        FragHeader hdr;
        hdr.frame_id   = frame_id;
        hdr.frag_seq   = i;
        hdr.frag_total = total;
        hdr.payload    = (uint16_t)len;
        hdr.flags      = (i == total - 1) ? FLAG_END : 0;   // 末片才置 END
        hdr.magic      = FRAG_MAGIC;

        memcpy(buf, &hdr, sizeof(hdr));              // 先拷 12 字节头
        memcpy(buf + sizeof(hdr), data + off, len);  // 再拷本片数据

        sendto(sock_, buf, sizeof(hdr) + len, 0,
               (const struct sockaddr *)&dst_, sizeof(dst_));
    }
}

// ===== 接收端：绑端口 → 收包 → 按序号落槽 → 收齐还原成帧 =====
void udpapp::udpdata_rece()
{
    // ① 接收专用 socket（和发送的 sock_ 分开），绑定自己的端口
    int rsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rsock < 0) return;

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(5004);       // 占住 5004，和发送端约定一致
    local.sin_addr.s_addr = INADDR_ANY;        // 不挑来源 IP，谁来都收
    if (bind(rsock, (struct sockaddr *)&local, sizeof(local)) < 0)
    {
        close(rsock);
        return;
    }

    Assembler as;                 // "正在拼的帧"状态
    uint8_t pkt[1500];            // 收包缓冲（够装单个 UDP 包）
    struct sockaddr_in src;       // recvfrom 填：谁发来的（本程序不管它）
    socklen_t srclen = sizeof(src);

    int saved = 0;                // 存文件的编号

    while (true)
    {
        int n = recvfrom(rsock, pkt, sizeof(pkt), 0, (struct sockaddr *)&src, &srclen);
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

            // 拼好了！先存成 jpg 验证（下一步换成显示/解码）
            char name[64];
            snprintf(name, sizeof(name), "frame_recv_%d.jpg", saved++);
            int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0)
            {
                write(fd, as.buf, frame_size);
                close(fd);
            }

            resetFrame(as);    // 清空，等下一帧的片 0
        }
    }

    close(rsock);
}