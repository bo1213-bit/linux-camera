# Linux 相机应用开发（V4L2）学习文档

> 面向「机器人远程遥控与遥测系统」项目：从端（LicheeRV Nano / SG2002）采集摄像头图像。
> 本文只讲 **V4L2 采集**这一层，编码、RTP 传输、多线程放到后续文档。
> 目标：看懂 V4L2 的调用流程，自己写出一个能抓帧的程序。

---

## 1. 总览：V4L2 是什么

- **V4L2 = Video4Linux2**，Linux 内核里视频设备的统一框架（采集、输出、调参）。
- 无论是 USB 免驱摄像头（UVC），还是板载 MIPI-CSI 摄像头，驱动最终都通过 V4L2 暴露成一个**字符设备节点** `/dev/videoN`。
- 用户态程序通过 **`ioctl` 系统调用** 与驱动交互，不是读写文件那么简单。

```
应用程序（你的 C 程序）
        │  ioctl / open / mmap / select
        ▼
    /dev/video0  (V4L2 字符设备)
        │
        ▼
    内核 V4L2 核心 + 摄像头驱动 (UVC / ISP / sensor)
        │
        ▼
    摄像头硬件 → 帧数据填进内核缓冲区
```

---

## 2. 开发前准备

### 2.1 需要的东西

| 项目 | 说明 |
|---|---|
| 头文件 | `#include <linux/videodev2.h>`（所有结构体和 ioctl 宏都在这里） |
| 链接库 | **不需要**额外库，纯 `ioctl` 即可（`libv4l2` 可选，用于兼容包装） |
| 其他头 | `fcntl.h` `unistd.h` `sys/ioctl.h` `sys/mman.h` `sys/select.h` `errno.h` |
| 权限 | 摄像头设备通常属于 `video` 组，用户需在组内或 `sudo` |

### 2.2 先用命令行工具摸清设备（强烈建议先做）

```bash
# 1. 看有哪些视频设备
ls -l /dev/video*

# 2. v4l2-ctl 枚举设备（一般已随 v4l-utils 安装）
v4l2-ctl --list-devices

# 3. 看某设备支持哪些格式/分辨率/帧率（很重要！决定你代码里填什么）
v4l2-ctl -d /dev/video0 --list-formats-ext
```

> 先跑 `--list-formats-ext`，把摄像头支持的分辨率、像素格式、帧率抄下来，再动手写代码，能少走很多弯路。
> 如果没装 v4l-utils：`sudo apt-get install v4l-utils`。

---

## 3. V4L2 采集的 10 步状态机（核心）

V4L2 采集是**固定顺序**的 `ioctl` 套路，顺序错了就会失败。记住这个顺序：

| 步骤 | 操作 | 关键 API / ioctl | 说明 |
|---|---|---|---|
| ① | 打开设备 | `open("/dev/video0", O_RDWR)` | 拿到文件描述符 fd |
| ② | 查询能力 | `VIDIOC_QUERYCAP` | 确认是采集设备、支持流式 I/O |
| ③ | 设置格式 | `VIDIOC_S_FMT` | 分辨率 + 像素格式（YUYV/MJPEG） |
| ④ | 申请缓冲 | `VIDIOC_REQBUFS` | 向内核要 N 个缓冲（mmap 方式） |
| ⑤ | 映射缓冲 | `mmap(...)` | 把内核缓冲映射进用户空间 |
| ⑥ | 缓冲入队 | `VIDIOC_QBUF` | 所有空缓冲交给内核去填数据 |
| ⑦ | 开始采集 | `VIDIOC_STREAMON` | 硬件开始出图 |
| ⑧ | 采集循环 | `select` → `VIDIOC_DQBUF` → 处理 → `VIDIOC_QBUF` | 取帧 → 用 → 还回，反复 |
| ⑨ | 停止采集 | `VIDIOC_STREAMOFF` | 停流 |
| ⑩ | 清理 | `munmap` + `close` | 释放映射和 fd |

**第 ⑧ 步是精髓**：`DQBUF`（出队）取出一帧 → 处理（存文件/编码/发送）→ `QBUF`（入队）还回去，N 个缓冲循环复用——这就是你 `learning_gaps.md` 里第 8 条说的「环形缓冲区」。

---

## 4. 完整采集流程：从打开到取帧（含 mmap 与帧获取）

第 3 节是"步骤清单"，这一节把你上手时最容易卡壳的**完整链路**按调用顺序串一遍：每一步给出「做什么 → 用哪个结构体 → 关键字段 → 代码」。特别注意夹在中间的 **mmap**（系统调用，不是 ioctl）和**真正的取帧动作**（DQBUF/QBUF 循环）——这两处上一版漏了。

| 步骤 | 操作 | 用到的结构体 / API |
|---|---|---|
| ① | 打开设备 | `open()` 系统调用 |
| ② | 查能力 | `struct v4l2_capability`（QUERYCAP） |
| ③ | 设格式 | `struct v4l2_format`（S_FMT） |
| ④ | 设帧率 | `struct v4l2_streamparm`（S_PARM，可选，见第 7 节） |
| ⑤ | 申请缓冲 | `struct v4l2_requestbuffers`（REQBUFS） |
| ⑥ | 查每块缓冲 | `struct v4l2_buffer`（QUERYBUF，index 是**输入**） |
| ⑦ | 映射到用户空间 | `mmap()` 系统调用 |
| ⑧ | 缓冲入队（注册） | `struct v4l2_buffer`（QBUF） |
| ⑨ | 开始采集 | `enum v4l2_buf_type`（STREAMON） |
| ⑩ | 取帧循环 | `struct v4l2_buffer`（DQBUF 取 → 用 → QBUF 还） |
| ⑪ | 收尾 | STREAMOFF / `munmap` / `close` |

### ①② 打开 + 查能力

```c
int fd = open("/dev/video0", O_RDWR);          // 拿到文件描述符 fd
struct v4l2_capability cap;
ioctl(fd, VIDIOC_QUERYCAP, &cap);
// cap.capabilities & V4L2_CAP_VIDEO_CAPTURE  是否采集设备
// cap.capabilities & V4L2_CAP_STREAMING      是否支持 mmap 流式
```

### ③ 设格式（S_FMT 是"协商"，必须读回）

```c
struct v4l2_format fmt = {0};
fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
fmt.fmt.pix.width       = 640;
fmt.fmt.pix.height      = 480;
fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;   // 或 YUYV，见第 6 节
fmt.fmt.pix.field       = V4L2_FIELD_NONE;      // 逐行，无隔行
ioctl(fd, VIDIOC_S_FMT, &fmt);

// ⚠️ 驱动可能返回跟请求不一样的实际值，设完一定要读回：
//   fmt.fmt.pix.width / height / pixelformat  实际值
//   fmt.fmt.pix.sizeimage                    一帧数据字节数（存文件/算缓冲用）
```

### ④ 设帧率（可选，详见第 7 节）

```c
struct v4l2_streamparm parm = {0};
parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
ioctl(fd, VIDIOC_G_PARM, &parm);
parm.parm.capture.timeperframe.numerator   = 1;
parm.parm.capture.timeperframe.denominator = 30;   // 30 fps
ioctl(fd, VIDIOC_S_PARM, &parm);
```

### ⑤ 申请缓冲（REQBUFS → 内核分配）

```c
struct v4l2_requestbuffers req = {0};
req.count  = 4;                              // 想要 4 个
req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
req.memory = V4L2_MEMORY_MMAP;               // mmap 方式（零拷贝）
ioctl(fd, VIDIOC_REQBUFS, &req);
// req.count 会变成内核实际分配的个数，后面循环用 req.count，别硬写 4
```

### ⑥⑦ 查每块信息 + mmap 映射（最容易漏的一步）

`REQBUFS` 只是"让内核分配内存"，**还没映射到用户空间**。要先 `QUERYBUF` 逐个查每块的长度和偏移，才知道 mmap 该映射多长、从哪映射：

```c
for (int i = 0; i < req.count; i++) {
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = i;                          // ← index 是"输入"：我要查第 i 块
    ioctl(fd, VIDIOC_QUERYBUF, &buf);
    // 查完，buf.length / buf.m.offset 就绪了

    // mmap 是系统调用，6 个参数：
    void *p = mmap(NULL,                    // 1 地址：NULL 让内核挑
                   buf.length,              // 2 长度：QUERYBUF 查出来的
                   PROT_READ | PROT_WRITE,  // 3 权限：可读可写
                   MAP_SHARED,              // 4 标志：共享（零拷贝关键）
                   fd,                      // 5 设备 fd
                   buf.m.offset);           // 6 偏移：QUERYBUF 查出来的
    if (p == MAP_FAILED) { /* 失败返回 MAP_FAILED，不是 NULL */ }

    // ⚠️ 紧接着把这块入队（第 ⑧ 步）
}
```

> **mmap 只是"给你一个能碰这块内存的指针"，一个字都没告诉内核"这块归你用"。数据还没进来。**

### ⑧ 缓冲入队 = 注册（QBUF，开流前必须做）

```c
// 循环里 mmap 完每一块，立刻 QBUF：
ioctl(fd, VIDIOC_QBUF, &buf);              // 把空缓冲交给内核去填
```

**这是最容易被漏掉的关键一步**：REQBUFS 只是"预订了 4 个盒子"，mmap 只是"拿到了 4 个盒子的门牌号"。**没 QBUF，内核根本不知道这 4 块是给它装数据的**——STREAMON 后硬件在队列里取不到盒子，没地方写，永远出不了帧。

用快递打比方：

| 动作 | 比喻 |
|---|---|
| REQBUFS | 预订 4 个快递盒 |
| mmap | 拿到 4 个盒子的门牌号 |
| **QBUF** | **把空盒子放到传送带、交给司机装货** |
| STREAMON | 司机发车（硬件开始采图） |
| DQBUF | 从传送带另一头取走装好的盒子 |
| 再 QBUF | 把空盒子放回传送带，循环复用 |

### ⑨ 开流 + ⑩ 取帧循环（真正的"取数据"在这里）

```c
enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
ioctl(fd, VIDIOC_STREAMON, &type);         // ⑨ 开流（注意传 &type，不是 &buf！）

struct v4l2_buffer frame = {0};
frame.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
frame.memory = V4L2_MEMORY_MMAP;

while (还要抓帧) {
    ioctl(fd, VIDIOC_DQBUF, &frame);       // 出队：内核把"填满的那块"编号给我
    // frame.index       ← 这次是"输出"：拿到的是第几块
    // frame.bytesused   ← 这一帧实际字节数（MJPEG 每帧不一样，用它！）

    unsigned char *data = (unsigned char *)buffers[frame.index];  // 用帧：数据就在这
    // ... 存文件 / 编码 / 发送 ...

    ioctl(fd, VIDIOC_QBUF, &frame);        // 还回：这块重新入队，装下一帧
}
```

**`frame.index` 的方向会反转，是最大的坑**：
- `QUERYBUF` / `QBUF` 时，index 是你**告诉内核**"第几块"——**输入**。
- `DQBUF` 后，index 是内核**告诉你**"填好的是第几块"——**输出**。

**`bytesused` vs `length`**：`length` 是这块的固定容量，`bytesused` 是这一帧的实际大小（MJPEG 变长压缩，每帧不一样）。**存文件/发网络必须用 `bytesused`**，用 `length` 会写进一堆垃圾字节。

### ⑪ 收尾

```c
ioctl(fd, VIDIOC_STREAMOFF, &type);        // 停流
for (int i = 0; i < req.count; i++)
    munmap(buffers[i], lengths[i]);        // 逐个解除映射（munmap 是 mmap 的逆操作）
close(fd);                                  // 关闭设备
```

---

## 5. 三种 I/O 方式对比

| 方式 | 原理 | 优缺点 |
|---|---|---|
| **read/write** | `read()` 拷贝读数据 | 最简单，但多一次拷贝、慢，一般不用 |
| **mmap** | 把内核缓冲映射进用户空间，直接读写 | **零拷贝**，最快，✅ 本项目用这个 |
| userptr | 用户自己分配内存给驱动 | 灵活，但驱动支持少，一般不用 |

本项目（720p@30fps 实时）必须用 **mmap**，省一次内存拷贝。

---

## 6. 像素格式（重点：YUYV vs MJPEG）

摄像头常见输出两种格式，**直接影响你后面的编码方案**：

| 格式 | 宏 | 特点 | 数据量（640×480） |
|---|---|---|---|
| **YUYV** | `V4L2_PIX_FMT_YUYV` | 原始未压缩 YUV 422 | 640×480×2 = 614400 字节 ≈ 600KB/帧 |
| **MJPEG** | `V4L2_PIX_FMT_MJPEG` | 每帧是一个完整的 JPEG 图 | 约 15~40KB/帧 |

**关键区别**：
- **MJPEG**：`DQBUF` 出来的一帧数据**本身就是一个完整的 JPEG 文件**（含 SOI/EOI 标记），直接写文件就是 `.jpg`，直接封装成 RTP 就能传——**省掉编码环节**，本项目首推。
- **YUYV**：是原始像素，数据量大，需要额外编码（H.264/MJPEG），才有意义传输。

**怎么知道摄像头支持哪个**：跑 `v4l2-ctl --list-formats-ext`，或代码里用 `VIDIOC_ENUM_FMT` 枚举。

> 附：一帧大小用 `fmt.fmt.pix.sizeimage` 或 DQBUF 后的 `buf.bytesused`，**不要自己算宽×高**（MJPEG 是变长压缩，算不出来的）。

---

## 7. 设置帧率

分辨率/格式之外，还可以设帧率（30fps 等）：

```c
struct v4l2_streamparm parm = {0};
parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
ioctl(fd, VIDIOC_G_PARM, &parm);                      // 先读
parm.parm.capture.timeperframe.numerator   = 1;
parm.parm.capture.timeperframe.denominator = 30;      // 30 fps
ioctl(fd, VIDIOC_S_PARM, &parm);                      // 再写
```

---

## 8. 阻塞等待新帧：select / poll

`VIDIOC_DQBUF` 默认**阻塞**，但更稳妥的写法是用 `select` 带超时，避免卡死：

```c
fd_set fds;
struct timeval tv = {2, 0};          // 2 秒超时
FD_ZERO(&fds);
FD_SET(fd, &fds);

int r = select(fd + 1, &fds, NULL, NULL, &tv);
if (r == -1)      { /* select 出错 */ }
else if (r == 0)  { /* 超时：没等到新帧，可打印日志或继续 */ }
else              { /* 有帧可读，此时再 DQBUF 不会一直卡住 */ }
```

> `select/poll` 与 `mmap` 不冲突：select 只是告诉你"有帧了"，DQBUF 才是真正拿帧。

---

## 9. 常见错误排查

| 现象 / errno | 原因 | 解决 |
|---|---|---|
| `open` 失败 `EACCES` | 没权限（不在 `video` 组） | `sudo` 或 `usermod -aG video $USER` |
| `open` 失败 `ENOENT` | 设备节点不对 | `ls /dev/video*` 确认编号 |
| `open` 失败 `EBUSY` | 设备被别的程序占用 | 关掉占用它的进程（如另一个采集程序） |
| ioctl 返回 `-1` 且 `EINTR` | 被信号打断（正常） | 封装 `xioctl` 重试：`do{...}while(r==-1 && errno==EINTR)` |
| `REQBUFS` 返回的 `count` 比请求少 | 驱动给不了那么多 | 用实际返回的 count |
| 画面花屏/全绿 | 分辨率/格式驱动不支持但你没读回 | S_FMT 后**必须读回实际值** |
| 帧数据大小不对 | 用 `buf.length` 存文件 | 改用 `buf.bytesused` |
| DQBUF 一直卡住 | 没 STREAMON，或缓冲没 QBUF | 检查步骤 ⑥⑦ 顺序 |
| 帧数据里杂音 | `buf.flags & V4L2_BUF_FLAG_ERROR` | 丢弃该帧，继续 |

---

## 10. 结合本项目的下一步路线图

```
① 先写"裸采集"程序（本文档内容）
   打开 → 设格式 → mmap → 循环 DQBUF/QBUF → 把帧 dump 到文件
   验证：抓到 MJPEG 存成 .jpg 能正常打开 ✅
        ↓
② 把采集逻辑封装成"采集线程"
   用互斥锁+条件变量的队列，把帧递给下游（替代学习文档里说的无锁队列）
        ↓
③ 视频编码（可选）
   已 MJPEG 就直接用；YUYV 才需要 H.264/软件编码
        ↓
④ RTP over UDP 分片发送（learning_gaps.md 总纲第 1~4 条）
        ↓
⑤ 多线程管道：采集 → 编码 → 发送，控制/遥测走 TCP
```

**第一步验收标准**：能抓出 10~30 帧，MJPEG 存成图片能打开、YUYV 存成 `.yuv` 文件能用工具看，就说明 V4L2 这层过关了，再往下走。

---

## 11. 参考资料

- 内核头文件源码：`/usr/include/linux/videodev2.h`（最权威，所有结构体字段注释都在里面）
- V4L2 官方文档（内核树）：`Documentation/userspace-api/media/v4l/`
- 经典参考实现：Linux 内核源码里的 `v4l2grab`、以及 linuxtv 上的 V4L2 capture 示例
- 工具：`v4l2-ctl`（v4l-utils 包）、`guvcview`（图形化看 UVC 摄像头）

---

*本文档只到 V4L2 采集为止，写代码时遇到具体报错，带着 errno 和报错信息回来问即可。*
