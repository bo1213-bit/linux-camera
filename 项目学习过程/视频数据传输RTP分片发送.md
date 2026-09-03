# Linux 相机应用开发（三）· 视频数据传输（RTP over UDP 分片发送）

> 接上一篇《采集线程与线程安全队列.md》：帧已经从摄像头里拷出来、在队列里排好队了。
> 本篇讲路线图的第 ④ 步：把队列里的一帧 MJPEG **拆成多个 UDP 包发到网络上**，接收端再**按序重组**。
> 本篇解决 `learning_gaps.md` 里的第 1、2、3、4 条盲区。

---

## 0. 先回答一个方向问题：视频为什么走 UDP，而不是 TCP？

上一篇结尾留了个悬念——"视频走 UDP 不重传、控制走 TCP 要重传"。这一节先把这层窗户纸捅破，后面每一节都是它的推论。

| | 视频（UDP） | 控制指令（TCP） |
|---|---|---|
| 要什么 | **实时性** | **可靠性** |
| 丢一包会怎样 | 花一下屏，1/30 秒的事，无感 | 指令丢了 = 机器人没收到 = 出错 |
| 等重传的代价 | 等一个往返，晚到的帧已经过时了 | 宁可多等几十毫秒，也要送到 |
| 底层保证 | 无连接，发出去就发出去，丢了没人管 | 有序、可靠、自动重传 |
| 电话号码 | 无连接，快 | 有连接，要握手 |

一句话：**视频宁可"丢"，也不能"慢"；控制宁可"慢"，也不能"丢"。** 这就是两条通道用两个协议的根本原因。这也是 `learning_gaps.md` 第 4 条要你记住的结论。

---

## 1. 一个致命的尺寸问题：一帧装不进一个 UDP 包

### 1.1 先算一笔账（learning_gaps 第 1 条）

- 一帧 MJPEG（640×480）：约 **15~40KB**（压缩后变长，每帧不一样）。
- 一个 UDP 包能装多少？不是想装多少装多少，受 **MTU** 限制：

```
以太网链路层 MTU = 1500 字节
    ├─ IP 头（IPv4，无选项）：20 字节
    ├─ UDP 头：8 字节
    └─ 留给「有效负载」：1500 - 20 - 8 = 1472 字节
```

再保守一点（预留 VLAN 等额外开销），工程上通常取 **1400 字节** 作为每片负载上限。

**结论**：一帧 15~40KB，一个 UDP 包最多装 1.4KB → **一帧必须拆成 11~29 个 UDP 包**才能发出去。这就是"分片"的由来。

### 1.2 为什么不"加大 UDP 包"而是拆片？

UDP 超过 MTU 会导致 **IP 层分片**（fragmentation）：IP 自己把大包切开、接收端 IP 再拼回来。听起来省事，但：

1. **一个 IP 分片丢了，整个 UDP 包全废**（IP 层不重传，拼不齐就丢，浪费更大）。
2. 应用层拿到的是"一整个包"，**丢在哪儿你都看不见**，没法做到"丢一片只影响一点点"。
3. 有些网络设备直接丢弃分片报文，兼容性差。

所以正确做法是**应用层自己分片**：你明确地把一帧切成 N 片，每片是一个独立、完整的 UDP 包，丢一片只丢那一片。这才有后面"检测丢包、选择性丢弃"的余地。

---

## 2. 分片头要带哪些信息（learning_gaps 第 2 条）

把一帧拆成 N 片、乱序发出去，接收端拿到的是一串**没有顺序、可能缺、可能重复**的碎片。它凭什么把这些碎片拼回一帧？答案是：**每片前面挂一个"分片头"，告诉接收端这片是谁的、排第几、还有几片。**

### 2.1 最少需要三个字段

`learning_gaps.md` 第 2 条已经点出来了（那是你自己踩过的坑）：

| 字段 | 回答的问题 | 缺了会怎样 |
|---|---|---|
| **帧号** frame_id | 这片属于哪一帧？ | 两帧的碎片混在一起，花屏 |
| **分片序号** frag_seq | 这是帧内第几片？ | 不知道贴到哪个位置，顺序乱 |
| **总片数** frag_total | 什么时候算收齐？ | 永远不知道这帧拼完没有 |

### 2.2 结构体定义

```c
#include <cstdint>

// 每个 UDP 包的负载 = 这个头(12字节) + 一段 JPEG 数据
struct FragHeader {
    uint32_t frame_id;    // 帧号：每帧 +1（同一帧的所有片共享同一个值）
    uint16_t frag_seq;    // 分片序号：本帧内第几片，从 0 开始
    uint16_t frag_total;  // 总片数：本帧一共拆成几片
    uint16_t payload;     // 本片实际负载字节数（末片通常不满 1400）
    uint8_t  flags;       // 位标志：bit0 = FLAG_END（末片）
    uint8_t  magic;       // 魔数 0xA5：收到包先对一下，防止接错数据
};

#define FLAG_END 0x01     // flags 的 bit0：这是本帧最后一片
```

> **`payload` 和 `FLAG_END` 其实是"冗余"的**——`payload` 能从 UDP 报文的长度推出来（总长 − 12），末片也能从 `frag_seq == frag_total - 1` 判断出来。把它们显式写进头里，是为了让接收端逻辑更直白、少算一步，也方便加校验。**先保证正确，再谈精简。**

### 2.3 这些字段和真正的 RTP 是什么关系？

不骗你：上面这套"自定义头"不是标准的 RTP，是**教学简化版**。真正的 RTP（RFC 3550）头长这样，但概念是一一对应的：

| 我们的自定义头 | RTP（RFC 3550） | 说明 |
|---|---|---|
| `frame_id`（帧号） | **timestamp**（时间戳，32位） | 同一帧的所有片共享同一时间戳 |
| `frag_seq`（分片序号） | **sequence number**（序号，16位） | 每个 RTP 包 +1 |
| `FLAG_END`（末片标志） | **marker bit**（M 位） | M=1 表示"这是这一帧的最后一个包" |
| —— | payload type（负载类型，7位） | 说明负载是什么（JPEG=26），接收端才知道怎么解 |
| —— | SSRC（同步源，32位） | 标识"谁在发"（多路流区分用） |
| `magic`（魔数） | 版本号 V（2位） | 都是"防错位"的快速校验 |

> 本项目是局域网内、点对点、MJPEG 这一种负载——这个简化够用了。真做产品（要对接标准播放器、多路流、跨厂商互通）才用 RFC 3550 + RFC 2435（JPEG over RTP）。**学习阶段先吃透"分片 + 重组"这套思想，RTP 头到时候只是换个字段名。**

---

## 3. 发送端：把一帧拆成 N 片发出去

发送线程从队列 `pop()` 出一帧，按 1400 字节把它切段，每段配一个分片头，`sendto` 出去。

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>

#define MAX_PAYLOAD 1400      // 每片最多装的 JPEG 字节数（留足 IP/UDP 头）
#define FRAG_MAGIC  0xA5

// 发送线程：从队列 pop 一帧，拆成多片 UDP 发出去
void sendFrame(int sock, struct sockaddr_in *dst, const Frame &f, uint32_t frame_id)
{
    // 这一帧总共要拆几片：size / 1400 向上取整
    uint16_t total = (uint16_t)((f.size + MAX_PAYLOAD - 1) / MAX_PAYLOAD);

    uint8_t *p = f.data;
    for (uint16_t i = 0; i < total; i++)
    {
        // 本片要发多少字节？末片通常不满 1400，前面都满
        size_t this_len = std::min((size_t)MAX_PAYLOAD, f.size - (size_t)i * MAX_PAYLOAD);

        // 拼一个「头 + 数据」的完整包（见下方 ⚠️）
        uint8_t buf[MAX_PAYLOAD + sizeof(FragHeader)];
        FragHeader hdr;
        hdr.frame_id   = frame_id;
        hdr.frag_seq   = i;
        hdr.frag_total = total;
        hdr.payload    = (uint16_t)this_len;
        hdr.flags      = (i == total - 1) ? FLAG_END : 0;   // 末片置 END
        hdr.magic      = FRAG_MAGIC;

        memcpy(buf, &hdr, sizeof(hdr));                     // 先拷头
        memcpy(buf + sizeof(hdr), p, this_len);             // 再拷数据

        sendto(sock, buf, sizeof(hdr) + this_len, 0,
               (struct sockaddr *)dst, sizeof(*dst));

        p += this_len;                                      // 数据指针前进
    }
}
```

### ⚠️ 最容易犯的错：别把"头"和"数据"分两次 sendto

对 TCP 来说，`write` 两次会自动拼成一条字节流，没关系；但 **UDP 每一次 `sendto` 都是一个独立的报文**。如果这样写：

```cpp
sendto(sock, &hdr, sizeof(hdr), ...);   // ❌ 这是【一个】UDP 包
sendto(sock, p, this_len, ...);         // ❌ 这是【另一个】UDP 包
```

接收端会收到**两个**包，还可能在中间被别的包插队、乱序。**头和数据必须拷进同一个 buffer，一次 `sendto` 发出去**（上面代码已经是这么做的）。

---

## 4. 接收端：把乱序的片拼回一帧

接收端是**有状态的**：它不是"来一片存一片"，而是维护一个"当前正在拼的帧"，碎片往里填，填满就交付。

### 4.1 接收端的四个动作

```
收一个包
   │
   ├─ magic 不对 ────────────────► 丢弃（不是我们的包 / 数据脏了）
   │
   ├─ frag_seq == 0 且 frame_id 变了 ─► 【开始了新的一帧】
   │                                  丢弃上一帧没拼完的，重置组装区
   │
   ├─ frame_id == 当前帧 ────────► 【属于当前帧】
   │                                  存进 frag_seq 对应的槽位
   │
   └─ 其他（旧帧的迟到碎片 / 乱序）──► 丢弃
```

### 4.2 代码骨架

```cpp
#include <sys/socket.h>
#include <netinet/in.h>

// 接收端组装区 —— 维护"当前正在拼的一帧"
struct Assembler {
    uint32_t cur_frame_id = 0;     // 正在拼哪一帧
    uint16_t frag_total    = 0;    // 这一帧一共几片
    uint8_t *buf           = nullptr;  // 帧数据累积区
    bool    *got           = nullptr;  // 每片收到没有（got[i] == true 表示第 i 片到了）
    int      got_count     = 0;    // 已经收到多少片
};

// 接收线程主循环
void recvLoop(int sock, Assembler &as)
{
    uint8_t pkt[1500];
    struct sockaddr_in src;
    socklen_t len = sizeof(src);

    while (running) {
        int n = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr*)&src, &len);
        if (n < (int)sizeof(FragHeader)) continue;   // 太小，连头都不够

        FragHeader hdr;
        memcpy(&hdr, pkt, sizeof(hdr));
        uint8_t *data = pkt + sizeof(hdr);
        int data_len  = n - sizeof(hdr);

        if (hdr.magic != FRAG_MAGIC) continue;       // ① 不是我们的包

        // ② 新的一帧开始了？
        if (hdr.frag_seq == 0 && hdr.frame_id != as.cur_frame_id) {
            // 上一帧没拼完就作废（要么丢了片，要么该帧被放弃）
            resetAssembler(as, hdr);
        }

        // ③ 属于当前帧吗？
        if (hdr.frame_id != as.cur_frame_id) continue;   // ④ 迟到旧片 / 乱序，丢

        // 存入对应槽位（重复的片覆盖，不重复计数）
        if (!as.got[hdr.frag_seq]) {
            as.got[hdr.frag_seq] = true;
            as.got_count++;
            memcpy(as.buf + (size_t)hdr.frag_seq * MAX_PAYLOAD, data, data_len);
        }

        // 收齐了？
        if (as.got_count == as.frag_total) {
            // 拼好了！交给解码 / 显示 / 存文件，然后重置
            deliverFrame(as.buf, as.cur_frame_size());
            resetAssembler(as, /*next frame*/);
        }
    }
}
```

### 4.3 拼齐之后怎么知道"整帧多大"？

分片长度大多固定 1400，**末片不满**。整帧大小 = `(frag_total - 1) * 1400 + 末片实际长度`。而末片长度就是它那片 `data_len`（或 `hdr.payload`）。所以在槽位里除了记住"到了没有"，还要记住"末片有多长"，收齐时加起来就是整帧字节数。

---

## 5. 序号只能"检测"丢包，不能"防止"丢包（learning_gaps 第 3 条）

这是最容易说错的点。`frame_id`、`frag_seq` 这些序号，本质只是**贴在包上的编号标签**。它们的能力边界要分清楚：

| 序号能做的 | 序号做不到的 |
|---|---|
| **检测**丢了一张（看到 105，就知道 104 丢了） | 让 104 不丢 |
| **纠正**乱序（按 frag_seq 塞回槽位） | 凭空变出丢的那片数据 |
| **丢弃**重复的包（got[i] 已经 true） | —— |

**丢包是"网络现实"，序号只能告诉你"丢了"这件事发生了，然后你决定怎么办。** 真正能"防止"丢包（把丢的找回来）的只有两个手段：

- **重传**：检测到丢 → 让对方重发一遍（要一个往返，慢）。
- **FEC 前向纠错**：发送时额外发冗余数据（如几分之一的校验包），丢一片能用冗余算回来（要多花带宽）。

`learning_gaps.md` 第 3 条要你记住的就是这句话：**序号是"检测/纠正器"，不是"防丢器"。**

---

## 6. 视频丢包选"丢弃"，不"重传"（learning_gaps 第 4 条）

那检测到丢了第 3 片，要不要让对方重发？

**视频场景：不重传，整帧丢弃。**

算一笔账：30fps 意味着每一帧只有 **33 毫秒**的"保质期"。重传要等一个往返（局域网还好、但也要几毫秒到几十毫秒；跨网络更久）。等重传的第 3 片慢悠悠到了，这一帧的其余 19 片早就过时、该显示下一帧了——**晚到的完整帧 = 废帧**。还不如直接放弃这一帧，播下一帧，观众只会看到 1/30 秒的轻微卡顿，几乎无感。

对照来看为什么控制指令要重传（走到 TCP 那层）：

| | 视频帧 | 控制指令 |
|---|---|---|
| 时效 | 33ms 就过时 | 晚几十毫秒执行也要执行 |
| 丢了的影响 | 花一下屏，可容忍 | 机器人没收到，可能出事故 |
| 策略 | **丢，播下一帧** | **重传，直到确认执行** |
| 协议 | UDP（不重传） | TCP + 应用层 ACK（重传） |

这正是"视频 UDP、控制 TCP"那句结论的完整展开。

> ⚠️ 注意"丢弃"也有条件：丢的是**帧内几片**，那整帧就没法拼了，该丢；但如果连续好多帧都丢（网络太差），就不是"丢一帧"能糊弄过去的了，得从源头解决（降分辨率/降帧率/换网络）。序号能帮你**统计**丢了多少，好发现这种恶化。

---

## 7. 串起来：组播/单播、端口、socket 四件套

### 7.1 发送端 socket（UDP 三行就绪）

```cpp
int sock = socket(AF_INET, SOCK_DGRAM, 0);      // ① 建 socket：数据报（UDP）

struct sockaddr_in dst;
dst.sin_family      = AF_INET;
dst.sin_port        = htons(5004);              // ② 目标端口（RTP 惯例用 5004）
dst.sin_addr.s_addr = inet_addr("192.168.1.100"); // ② 目标 IP（控制端地址）

// ③ sendto 直接发，不用 connect（UDP 无连接）
```

### 7.2 接收端 socket（多了个 bind）

```cpp
int sock = socket(AF_INET, SOCK_DGRAM, 0);

struct sockaddr_in local;
local.sin_family      = AF_INET;
local.sin_port        = htons(5004);            // 监听同一端口
local.sin_addr.s_addr = INADDR_ANY;             // 任何网卡来的都收

bind(sock, (struct sockaddr*)&local, sizeof(local));  // ① bind：占住 5004 端口

// recvfrom（第 4 节已用）
```

> UDP 和 TCP 的 socket 套路不一样：TCP 客户端要 `connect`、服务端要 `listen` + `accept`（四件套），**UDP 没有这些**——发端 `sendto` 指定目标即可，收端只要 `bind` 个端口等着 `recvfrom`。记住这个区别，别把 TCP 的套路硬套过来。

### 7.3 编译

UDP / 网络函数也是标准库，不需要额外链接库：

```bash
g++ -std=c++11 -pthread v4l2app.cpp -o v4l2app
```

（`socket`/`sendto`/`recvfrom` 在 libc 里；`-pthread` 是因为我们上一步用了多线程。）

---

## 8. 常见坑汇总

| 坑 | 后果 | 解决 |
|---|---|---|
| 头和数据分两次 `sendto` | 变成两个包，可能被插队/错位 | 拼进一个 buffer 一次发 |
| 每片负载取 1472 不留余量 | 加了 VLAN 等头就被 IP 层分片，兼容性差 | 取 1400 保守值 |
| 接收端不判断 `frame_id` | 两帧碎片混拼，花屏 | 严格按"帧号+首片"切新帧 |
| 忘了"上一帧没拼完就作废" | 组装区越界/内存泄漏 | 新帧开始先重置组装区 |
| 重复片重复计数 | `got_count` 虚高，提前"收齐" | 判断 `got[i]` 已真就不再计 |
| 只记"到了没有"不记"末片长度" | 拼出来的整帧大小算错 | 单独存末片实际长度 |
| 拿序号当"防丢"指望 | 丢包照样丢，怪协议没用对 | 序号只能检测，防丢靠重传/FEC |
| 视频也搞重传 | 延迟越搞越大，实时性废了 | 视频丢帧，控制才重传 |

---

## 9. 本篇验收标准

- [ ] 一帧 20KB 左右的 MJPEG，能拆成 15 片左右逐包发出去，每包 ≤ 1440 字节（Wireshark 抓包能验证）。
- [ ] 接收端能把拆散的片**按正确顺序**拼回一帧，存成 `.jpg` 打开不花屏（说明重组逻辑对）。
- [ ] **故意丢一片**（比如接收端跳过某个 frag_seq），这一帧被正确识别为"没拼齐"并丢弃，不影响下一帧。
- [ ] 能说清楚：序号为什么只能"检测"不能"防止"丢包；视频为什么丢而不重传。

达到这四点，`learning_gaps.md` 第 1~4 条就真正过关了，进入下一步。

---

## 10. 下一步

```
② 采集线程 + 线程安全队列          ✅
③ 视频编码：MJPEG 跳过 ✅
        ↓
④ RTP over UDP 分片发送           ← 本篇 ✅
        ↓
⑤ 多线程管道：采集 → (编码) → 发送，控制/遥测走 TCP
```

第 ⑤ 步只要把三件事**用线程串起来**：采集线程（已有）→ 发送线程（本篇第 3 节）→ 再加接收端（本篇第 4 节），以及另一条 **TCP 控制通道**（那是 `learning_gaps.md` 第 5、6 条的领地，另一份文档讲）。

---

*本篇到"视频分片传输"为止。先把发送端 + 接收端跑通、Wireshark 抓到包、拼出的 jpg 能打开，再回来问下一步。*