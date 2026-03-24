# Linux_xiaozhi_ai

基于全志 T113-S3 芯片实现的小智 AI 对话功能，全程功能代码通过阿里千问生成。

## 项目概述

本项目实现了一个基于全志 T113-S3 芯片的智能语音对话系统，主要功能包括：

- 音频采集与编码（使用 Opus 编解码器）
- 音频播放与解码
- 设备激活与认证
- WebSocket 通信与服务器交互
- 语音识别与合成

## 目录结构

```
├── Audio_Processing/    # 音频处理模块
│   ├── CMakeLists.txt   # CMake 构建文件
│   └── xiaozhi_opus.c   # 音频采集、编码、解码和播放实现
├── Control_Center/      # 控制中心模块
│   ├── CMakeLists.txt   # CMake 构建文件
│   ├── websocket_test.c # WebSocket 客户端实现
│   └── xiaozhi_http.c   # 设备激活与 HTTP 通信
└── README.md            # 项目文档
```

## 模块说明

### 1. Audio_Processing 模块

负责音频的采集、编码、解码和播放，主要功能包括：

- 使用 ALSA 库进行音频采集和播放
- 使用 Opus 编解码器进行音频压缩和解压缩
- 通过 UDP 协议与控制中心进行音频数据传输
- 支持实时音频处理和低延迟播放

### 2. Control_Center 模块

负责设备激活、WebSocket 连接和音频数据转发，主要功能包括：

- 设备激活与认证（通过 HTTP 请求）
- WebSocket 客户端实现，与远程服务器建立连接
- 音频数据转发（从 UDP 接收音频数据并通过 WebSocket 发送）
- 处理服务器返回的音频数据并转发给音频处理模块

## 技术栈

- **编程语言**：C
- **音频处理**：ALSA、Opus
- **网络通信**：UDP、WebSocket、HTTP
- **JSON 解析**：cJSON
- **构建系统**：CMake

## 依赖项

- `libopus`：Opus 编解码器库
- `libasound2`：ALSA 音频库
- `libcurl`：HTTP 客户端库
- `libwebsockets`：WebSocket 客户端库
- `libcjson`：JSON 解析库

## 编译与运行

### 交叉编译环境设置

本项目使用交叉编译工具链为全志 T113-S3 芯片构建，需要设置以下环境：

1. **交叉编译器**：`arm-linux-gnueabihf-gcc`
2. **依赖库**：需要在交叉编译环境中构建以下库
   - Opus 编解码器库
   - ALSA 音频库
   - libcurl 库
   - libwebsockets 库
   - cJSON 库
   - OpenSSL 库

### 编译步骤

1. **准备交叉编译环境**

   确保已安装交叉编译工具链和所有依赖库。

2. **编译项目**

```bash
# 编译 Audio_Processing 模块
cd Audio_Processing
mkdir build && cd build
cmake ..
make

# 编译 Control_Center 模块
cd ../../Control_Center
mkdir build && cd build
cmake ..
make
```

### 依赖库构建说明

项目的 CMakeLists.txt 文件中指定了依赖库的路径，需要根据实际情况调整：

- **Opus 库**：`/home/ubuntu/opus-1.4/install_t113`
- **ALSA 库**：`/home/ubuntu/alsa/install_t113/usr`
- **OpenSSL 库**：`/home/ubuntu/openssl_arm_install` 或 `/home/ubuntu/openssl-1.1.1w/openssl_arm_install`
- **libcurl 库**：`/home/ubuntu/curl/curl-7.71.1/_install_arm`
- **libwebsockets 库**：`/home/ubuntu/websocket/libwebsockets/build/install_t113`
- **cJSON 库**：`/home/ubuntu/cjson/cJSON-1.7.10/install_t113`
- **zlib 库**：`/home/ubuntu/t113/t113-linux/out/t113/evb1_auto/buildroot/buildroot/target/usr/lib`

### 运行步骤

1. 首先运行控制中心模块（WebSocket 客户端）

```bash
cd Control_Center/build
./websocket_test
```

2. 然后运行音频处理模块

```bash
cd Audio_Processing/build
./xiaozhi_opus
```

## 工作流程

1. **设备激活**：控制中心通过 HTTP 请求与服务器进行设备激活认证
2. **建立连接**：控制中心通过 WebSocket 与远程服务器建立连接
3. **音频采集**：音频处理模块采集麦克风输入的音频数据
4. **音频编码**：使用 Opus 编码器对音频数据进行压缩
5. **数据传输**：通过 UDP 将编码后的音频数据发送给控制中心
6. **转发数据**：控制中心通过 WebSocket 将音频数据转发给远程服务器
7. **处理响应**：服务器处理音频数据（如语音识别、AI 对话）并返回结果
8. **音频播放**：控制中心将服务器返回的音频数据通过 UDP 转发给音频处理模块，由其解码并播放

## 配置说明

### 音频处理模块配置

- `SAMPLE_RATE`：采样率（默认 24000 Hz）
- `CHANNELS`：音频通道数（默认 1，单声道）
- `FRAME_DURATION_MS`：帧时长（默认 60 ms）
- `UDP_SEND_PORT`：UDP 发送端口（默认 9001）
- `UDP_RECV_PORT`：UDP 接收端口（默认 9002）

### 控制中心配置

- `UDP_FORWARD_IP`：UDP 转发目标 IP（默认 127.0.0.1）
- `UDP_FORWARD_PORT`：UDP 转发目标端口（默认 9002）
- `UDP_AUDIO_IP`：音频接收 IP（默认 127.0.0.1）
- `UDP_AUDIO_PORT`：音频接收端口（默认 9001）
- `xiaozhi_server`：WebSocket 服务器配置
  - `hostname`：服务器地址（默认 api.tenclass.net）
  - `port`：服务器端口（默认 443）
  - `path`：WebSocket 路径（默认 /xiaozhi/v1/）

## 故障排除

1. **音频采集失败**：检查 ALSA 配置和设备权限
2. **WebSocket 连接失败**：检查网络连接和服务器地址配置
3. **音频播放卡顿**：调整缓冲区大小和帧时长
4. **设备激活失败**：检查网络连接和设备 ID 配置

## 注意事项

- 本项目基于全志 T113-S3 芯片开发，在其他平台上可能需要调整配置
- 运行前请确保已安装所有依赖项
- 请确保网络连接正常，以便与远程服务器通信
- 音频设备需要正确配置并具有足够的权限
