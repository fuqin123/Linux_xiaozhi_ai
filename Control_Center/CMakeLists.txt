cmake_minimum_required(VERSION 3.10)
project(xiaozhi_http C)

# 设置 C 标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 交叉编译工具链设置
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 设置交叉编译器
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# 指定自定义 libcurl 路径
set(CURL_ROOT "/home/ubuntu/curl/curl-7.71.1/_install_arm")
set(CURL_INCLUDE_DIR "${CURL_ROOT}/include")
set(CURL_LIBRARY "${CURL_ROOT}/lib/libcurl.a")

# 指定 OpenSSL 路径
set(OPENSSL_ROOT "/home/ubuntu/openssl-1.1.1w/openssl_arm_install")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT}/include")
set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT}/lib/libssl.a")
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT}/lib/libcrypto.a")

# Opus 库路径配置
set(OPUS_ROOT "/home/ubuntu/opus-1.4/install_t113")
set(OPUS_INCLUDE_DIR "${OPUS_ROOT}/include")
set(OPUS_LIB_DIR "${OPUS_ROOT}/lib")

# ALSA 库路径配置
set(ALSA_ROOT "/home/ubuntu/alsa/install_t113/usr")
set(ALSA_INCLUDE_DIR "${ALSA_ROOT}/include")
set(ALSA_LIB_DIR "${ALSA_ROOT}/lib")

# 指定 libwebsockets 路径
set(LIBWEBSOCKETS_ROOT "/home/ubuntu/websocket/libwebsockets/build/install_t113")
set(LIBWEBSOCKETS_INCLUDE_DIR "${LIBWEBSOCKETS_ROOT}/include")
set(LIBWEBSOCKETS_LIB_DIR "${LIBWEBSOCKETS_ROOT}/lib")

# 指定 zlib 路径（修复：去掉多余的 /usr）
set(ZLIB_LIB_DIR "/home/ubuntu/t113/t113-linux/out/t113/evb1_auto/buildroot/buildroot/target/usr/lib")

# 指定cjson 库路径
set(CJSON_ROOT "/home/ubuntu/cjson/cJSON-1.7.10/install_t113")
set(CJSON_INCLUDE_DIR "${CJSON_ROOT}/include")
set(CJSON_LIB_DIR "${CJSON_ROOT}/lib")

# 修改：使用动态库而非静态库
set(CJSON_LIB "${CJSON_LIB_DIR}/libcjson.so")
# 设置可执行文件输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# 添加可执行文件
add_executable(xiaozhi_http xiaozhi_http.c)

# 添加 websocket 测试可执行文件
add_executable(websocket_test websocket_test.c)
add_executable(websocket websocket.c)
# 包含头文件路径
target_include_directories(xiaozhi_http PRIVATE 
    ${CURL_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${OPUS_INCLUDE_DIR}
    ${ALSA_INCLUDE_DIR}
)

# websocket_test 包含头文件路径
target_include_directories(websocket_test PRIVATE 
    ${LIBWEBSOCKETS_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${CJSON_INCLUDE_DIR}
)

target_include_directories(websocket PRIVATE 
    ${LIBWEBSOCKETS_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${CJSON_INCLUDE_DIR}
)

# 查找库文件
find_library(OPUS_LIB opus HINTS ${OPUS_LIB_DIR})
find_library(ALSA_LIB asound HINTS ${ALSA_LIB_DIR})

# 直接使用 zlib 库路径（避免 find_library 找到系统库）
set(ZLIB_LIB "${ZLIB_LIB_DIR}/libz.so")

# 使用动态库
set(WEBSOCKETS_LIB "${LIBWEBSOCKETS_LIB_DIR}/libwebsockets.so")

# 链接库及依赖
target_link_libraries(xiaozhi_http 
    ${CURL_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPUS_LIB}
    ${ALSA_LIB}
    pthread
    dl
)

# websocket_test 链接库及依赖（使用 start-group 处理循环依赖）
target_link_libraries(websocket_test 
    -Wl,--start-group
    ${WEBSOCKETS_LIB}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    -Wl,--end-group
    pthread
    dl
    ${ZLIB_LIB}
    ${CJSON_LIB}
)

target_link_libraries(websocket
    -Wl,--start-group
    ${WEBSOCKETS_LIB}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    -Wl,--end-group
    pthread
    dl
    ${ZLIB_LIB}
    ${CJSON_LIB}
)

# 添加库搜索路径（CMake 3.10 兼容方式）
set_target_properties(websocket_test PROPERTIES
    LINK_FLAGS "-L${OPENSSL_ROOT}/lib -L${LIBWEBSOCKETS_LIB_DIR} -L${ZLIB_LIB_DIR}"
    LINK_FLAGS "-L${OPENSSL_ROOT}/lib -L${LIBWEBSOCKETS_LIB_DIR} -L${ZLIB_LIB_DIR} -L${CJSON_LIB_DIR}"
)

set_target_properties(websocket PROPERTIES
    LINK_FLAGS "-L${OPENSSL_ROOT}/lib -L${LIBWEBSOCKETS_LIB_DIR} -L${ZLIB_LIB_DIR}"
    LINK_FLAGS "-L${OPENSSL_ROOT}/lib -L${LIBWEBSOCKETS_LIB_DIR} -L${ZLIB_LIB_DIR} -L${CJSON_LIB_DIR}"
)

# 设置编译选项
target_compile_options(xiaozhi_http PRIVATE -Wall -Wextra)
target_compile_options(websocket_test PRIVATE -Wall -Wextra)
target_compile_options(websocket PRIVATE -Wall -Wextra)
# 安装规则
install(TARGETS xiaozhi_http DESTINATION bin)
install(TARGETS websocket_test DESTINATION bin)
install(TARGETS websocket DESTINATION bin)