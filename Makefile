# Makefile —— Linux 相机采集 + UDP 分片发送
# 用法：
#   make          编译生成 ./camera
#   make run      编译并以默认目标地址运行
#   make clean    清除目标文件和可执行文件
#
# 交叉编译板端时改 CXX / 目标地址即可，见文件末尾说明。

# ---- 编译器与工具链 ----
CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2
LDFLAGS  ?=

# pthread：旧版 gcc 需要显式 -lpthread；新版自动链接，加上无害
LIBS := -lpthread

# ---- 目录与文件 ----
SRC_DIR := src
BUILD_DIR := build
TARGET := camera

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

# ---- 要硬编码的目标地址（接收端 IP:PORT）----
# 改这里，或 make run RECV_IP=192.168.2.50
RECV_IP  ?= 192.168.1.100
RECV_PORT ?= 5004

.PHONY: all run clean

all: $(TARGET)

# 链接：把 .o 链接成可执行文件
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

# 编译规则：每个 .cpp 编译成 .o，头文件变动也会触发重编（-MMD 生成依赖）
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) -MMD -c $< -o $@

# 首次编译时确保 build/ 目录存在
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	@echo "目标地址: $(RECV_IP):$(RECV_PORT)（如果跟代码里不一致，请改 udpapp.cpp）"
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# 把 .d 依赖文件自动 include 进来（-MMD 生成的）
-include $(OBJS:.o=.d)