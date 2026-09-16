# Makefile —— Linux 相机采集 + UDP 分片收发
# 用法：
#   make            编译从机（采集+发送）和主机（接收+存盘），生成 build/slave/camera 和 build/master/receiver
#   make slave      只编译从机
#   make master     只编译主机
#   make run-slave  编译并以默认目标地址运行从机
#   make clean      清除所有编译产物
#
# 交叉编译板端时改 CXX 即可，见文件末尾说明。

# ---- 编译器与工具链 ----
CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2
LDFLAGS  ?=

# pthread：旧版 gcc 需要显式 -lpthread；新版自动链接，加上无害
LIBS := -lpthread

# ---- 目录结构 ----
# src/slave  = 从机（V4L2 采集 + UDP 分片发送），跑在 Luckfox 板子上
# src/master = 主机（UDP 收包 + 重组 + 存盘），跑在 PC 上
SRC_DIR     := src
BUILD_DIR   := build
SLAVE_SRC   := $(SRC_DIR)/slave
MASTER_SRC  := $(SRC_DIR)/master

SLAVE_TARGET  := $(BUILD_DIR)/slave/camera
MASTER_TARGET := $(BUILD_DIR)/master/receiver

# 从机源文件（main/v4l2app/udpapp），主机源文件（main/udpapp）
SLAVE_SRCS  := $(wildcard $(SLAVE_SRC)/*.cpp)
MASTER_SRCS := $(wildcard $(MASTER_SRC)/*.cpp)
SLAVE_OBJS  := $(patsubst $(SLAVE_SRC)/%.cpp,$(BUILD_DIR)/slave/%.o,$(SLAVE_SRCS))
MASTER_OBJS := $(patsubst $(MASTER_SRC)/%.cpp,$(BUILD_DIR)/master/%.o,$(MASTER_SRCS))

# ---- 要硬编码的目标地址（接收端 IP:PORT，从机用）----
# 改这里，或 make run-slave RECV_IP=192.168.2.50
RECV_IP   ?= 192.168.1.100
RECV_PORT ?= 5004

.PHONY: all slave master run-slave clean

all: slave master

slave: $(SLAVE_TARGET)
master: $(MASTER_TARGET)

# ---- 从机：链接成 camera（跑在板子上）----
$(SLAVE_TARGET): $(SLAVE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

# ---- 主机：链接成 receiver（跑在 PC 上）----
$(MASTER_TARGET): $(MASTER_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

# 编译规则：从机
$(BUILD_DIR)/slave/%.o: $(SLAVE_SRC)/%.cpp | $(BUILD_DIR)/slave
	$(CXX) $(CXXFLAGS) -I$(SLAVE_SRC) -MMD -c $< -o $@

# 编译规则：主机
$(BUILD_DIR)/master/%.o: $(MASTER_SRC)/%.cpp | $(BUILD_DIR)/master
	$(CXX) $(CXXFLAGS) -I$(MASTER_SRC) -MMD -c $< -o $@

$(BUILD_DIR)/slave:
	mkdir -p $(BUILD_DIR)/slave
$(BUILD_DIR)/master:
	mkdir -p $(BUILD_DIR)/master

run-slave: $(SLAVE_TARGET)
	@echo "目标地址: $(RECV_IP):$(RECV_PORT)（如果跟代码里不一致，请改 slave/udpapp.cpp）"
	$(SLAVE_TARGET)

clean:
	rm -rf $(BUILD_DIR)

# 把 .d 依赖文件自动 include 进来（-MMD 生成的）
-include $(SLAVE_OBJS:.o=.d) $(MASTER_OBJS:.o=.d)
