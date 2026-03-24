cmake_minimum_required(VERSION 3.10)
project(Audio_Processing C)

set(CMAKE_C_STANDARD 99)

# 交叉编译工具链设置
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 设置交叉编译器
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Opus 库路径配置
set(OPUS_ROOT "/home/ubuntu/opus-1.4/install_t113")
set(OPUS_INCLUDE_DIR "${OPUS_ROOT}/include")
set(OPUS_LIB_DIR "${OPUS_ROOT}/lib")

# ALSA 库路径配置
set(ALSA_ROOT "/home/ubuntu/alsa/install_t113/usr")
set(ALSA_INCLUDE_DIR "${ALSA_ROOT}/include")
set(ALSA_LIB_DIR "${ALSA_ROOT}/lib")

# 指定 OpenSSL 路径
set(OPENSSL_ROOT "/home/ubuntu/openssl_arm_install")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT}/include")
set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT}/lib/libssl.a")
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT}/lib/libcrypto.a")

# 查找 Opus 库
find_library(OPUS_LIB opus HINTS ${OPUS_LIB_DIR})

# 查找 ALSA 库
find_library(ALSA_LIB asound HINTS ${ALSA_LIB_DIR})

# 包含目录
include_directories(${OPUS_INCLUDE_DIR})
include_directories(${ALSA_INCLUDE_DIR})
include_directories(${WEB_INCLUDE_DIR})
include_directories(${OPENSSL_INCLUDE_DIR})
# 可执行文件
add_executable(xiaozhi_opus xiaozhi_opus.c)
add_executable(sound sound.c)
# 链接库
target_link_libraries(xiaozhi_opus ${OPUS_LIB} ${ALSA_LIB} pthread m)
target_link_libraries(sound ${OPUS_LIB} ${ALSA_LIB} pthread m)
# 运行时库路径
set_target_properties(xiaozhi_opus PROPERTIES
    BUILD_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}:${OPENSSL_SSL_LIBRARY}"
    INSTALL_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}:${OPENSSL_SSL_LIBRARY}"
)

set_target_properties(sound PROPERTIES
    BUILD_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}:${OPENSSL_SSL_LIBRARY}"
    INSTALL_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}:${OPENSSL_SSL_LIBRARY}"
)