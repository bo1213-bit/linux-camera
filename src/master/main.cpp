// main.cpp —— 主机端入口：收 UDP 分片 → 重组 → 管道喂 ffplay 实时预览
// 对应从机 src/slave/main.cpp（采集 + 分片发送）。两端共用同一套分片协议。
//
// 跑起来后：占住 5004 端口收包，每收齐一帧写进 ffplay 子进程的 stdin，
// ffplay 弹窗实时显示画面（640×480 NV12）。

#include <iostream>
#include "udpapp.hpp"

int main()
{
    udpapp net;

    // 阻塞在这里：持续收包、重组、喂 ffplay，Ctrl+C 退出
    net.udpdata_rece();

    return 0;
}
