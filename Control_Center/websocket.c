#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libwebsockets.h>
#include <time.h>
#include <signal.h>
#include <cjson/cJSON.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
    typedef HANDLE pthread_t;
    #define pthread_create(win_thread, attr, func, arg) \
        (*(win_thread) = CreateThread(NULL, 0, func, arg, 0, NULL))
    #define pthread_join(win_thread, status) (WaitForSingleObject(win_thread, INFINITE), CloseHandle(win_thread))
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <pthread.h>
#endif

#define DEFAULT_PROTOCOL "xiaozhi-protocol"

// 客户端数据结构
struct client_data {
    int completed;
    int established;
    int message_sent;
    time_t last_send_time;
    struct lws *wsi;
};

static int force_exit = 0;
static struct lws_context *g_context = NULL;
static struct client_data *g_client_data = NULL;

// 信号处理
static void sighandler(int sig) {
    force_exit = 1;
    lws_cancel_service(lws_get_context(sig)); // 通知事件循环退出
}

// 连接参数结构
struct connection_params {
    const char *access_token;
    const char *device_id;
    const char *client_id;
};

// 全局连接参数实例
static struct connection_params conn_params = {
    "test-token",
    "ee:a4:ee:f8:cc:7c",
    "d90612cb-fd9c-4ddd-9981-a27b238983b7"
};

// 声明函数，模拟外部函数
void ProcessBinDataFrmServer(void *data, size_t len);
void ProcessTxtDataFrmServer(unsigned char *msg, size_t len);
void ProcessHello(cJSON *json);
void ProcessTTS(cJSON *json);
void ProcessSTT(cJSON *json);

// UDP音频线程
#ifdef _WIN32
DWORD WINAPI AudioThread(LPVOID arg)
#else
void* AudioThread(void* arg)
#endif
{
    struct client_data *data = (struct client_data *)arg;
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[4096];

    #ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            fprintf(stderr, "[音频线程] WSAStartup失败\n");
            return 1;
        }
    #endif

    // 创建UDP套接字
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("[音频线程] UDP套接字创建失败");
        return 1;
    }

    // 设置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(9001);  // 端口5555
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // 绑定套接字
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[音频线程] UDP绑定失败");
        #ifdef _WIN32
            closesocket(sockfd);
            WSACleanup();
        #else
            close(sockfd);
        #endif
        return 1;
    }

    fprintf(stderr, "[音频线程] 开始监听UDP端口 5555\n");

    // 循环读取数据并发送到WebSocket
    while (!force_exit && data->established && !data->completed) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                        (struct sockaddr*)&client_addr, &client_len);

        if (n > 0) {
            // 检查WebSocket连接是否仍然有效
            if (data->wsi && g_context) {
                // 不使用不存在的函数，而是直接尝试写入
                unsigned char *pkt = malloc(LWS_PRE + n);
                if (pkt) {
                    memcpy(&pkt[LWS_PRE], buffer, n);
                    
                    int result = lws_write(data->wsi, &pkt[LWS_PRE], n, LWS_WRITE_BINARY);
                    if (result < 0) {
                        fprintf(stderr, "[音频线程] WebSocket发送失败\n");
                    } else {
                        //fprintf(stderr, "[音频线程] 发送 %d 字节音频数据到WebSocket\n", n);
                    }
                    free(pkt);
                }
            }
        } else if (n < 0) {
            perror("[音频线程] 接收UDP数据失败");
            break;
        }
    }

    fprintf(stderr, "[音频线程] 结束\n");

    #ifdef _WIN32
        closesocket(sockfd);
        WSACleanup();
    #else
        close(sockfd);
    #endif

    #ifdef _WIN32
        return 0;
    #else
        return NULL;
    #endif
}

// WebSocket回调函数
static int callback_echo(struct lws *wsi, enum lws_callback_reasons reason, 
                         void *user, void *in, size_t len) {
    struct client_data *data = (struct client_data *)user;
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            fprintf(stderr, "[成功] WebSocket连接已建立\n");
            data->established = 1;
            data->wsi = wsi;
            g_client_data = data; // 保存全局引用以便在线程中使用
            
            // 启动音频线程
            #ifdef _WIN32
                HANDLE audio_thread;
                if (CreateThread(NULL, 0, AudioThread, data, 0, NULL) == NULL) {
                    fprintf(stderr, "[错误] 创建音频线程失败\n");
                } else {
                    fprintf(stderr, "[信息] 音频线程已启动\n");
                }
            #else
                pthread_t audio_thread;
                if (pthread_create(&audio_thread, NULL, AudioThread, data) != 0) {
                    fprintf(stderr, "[错误] 创建音频线程失败\n");
                } else {
                    fprintf(stderr, "[信息] 音频线程已启动\n");
                    
                    // 分离线程，这样它可以在完成后自动清理资源
                    pthread_detach(audio_thread);
                }
            #endif
            
            // 请求可写回调以发送第一条消息
            lws_callback_on_writable(wsi);
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE:
        {
            // 获取接收数据的信息，区分二进制和文本数据
            if (lws_frame_is_binary(wsi)) {
                // 二进制数据
                //fprintf(stderr, "[收到二进制数据] %zu 字节\n", len);
                ProcessBinDataFrmServer(in, len);
            } else {
                // 文本数据
                fprintf(stderr, "[收到文本数据] %.*s\n", (int)len, (char *)in);
                ProcessTxtDataFrmServer((unsigned char *)in, len);
            }
            break;
        }
            
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (data->established && !data->message_sent) {
                fprintf(stderr, "[发送] 发送初始消息...\n");
                unsigned char buf[LWS_PRE + 1024];
                unsigned char *p = &buf[LWS_PRE];
                
                // 发送指定的JSON消息
                const char *json_msg = "{\n    \"type\": \"hello\",\n    \"version\": 1,\n    \"transport\": \"websocket\",\n    \"features\": {\n        \"mcp\": true\n    },\n    \"audio_params\": {\n        \"format\": \"opus\",\n        \"sample_rate\": 16000,\n        \"channels\": 1,\n        \"frame_duration\": 60\n    }\n}";
                
                int n = strlen(json_msg);
                memcpy(p, json_msg, n);
                printf("📝 发送：%d 字节 %.*s\n", (int)n, &buf[LWS_PRE]);
                if (lws_write(wsi, p, n, LWS_WRITE_TEXT) == n) {
                    data->message_sent = 1;
                    fprintf(stderr, "[成功] 初始消息已发送\n");
                } else {
                    fprintf(stderr, "[错误] 写入失败\n");
                    data->completed = 1;
                }
            }
            break;
            
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            fprintf(stderr, "[错误] 连接失败: %s\n", in ? (char *)in : "未知错误");
            data->completed = 1;
            break;
            
        case LWS_CALLBACK_CLIENT_CLOSED:
            fprintf(stderr, "[信息] 连接已关闭\n");
            data->completed = 1;
            break;
            
        case LWS_CALLBACK_WSI_DESTROY:
            fprintf(stderr, "[信息] 连接资源已释放\n");
            break;
            
        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
            // 在HTTP握手完成时检查状态码
            {
                int http_status = lws_http_client_http_response(wsi);
                if (http_status) {
                    fprintf(stderr, "[信息] HTTP响应状态码: %d\n", http_status);
                }
            }
            break;
            
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            // 在握手阶段添加自定义头部
            {
                unsigned char **p = (unsigned char **)in;
                unsigned char *end = (*p) + len;
                
                if (lws_add_http_header_by_name(wsi, 
                    (unsigned char *)"Authorization:", 
                    (unsigned char *)conn_params.access_token, 
                    strlen(conn_params.access_token), 
                    p, end)) {
                    return -1;
                }
                
                if (lws_add_http_header_by_name(wsi, 
                    (unsigned char *)"Protocol-Version:", 
                    (unsigned char *)"1", 
                    1, 
                    p, end)) {
                    return -1;
                }
                
                if (lws_add_http_header_by_name(wsi, 
                    (unsigned char *)"Device-Id:", 
                    (unsigned char *)conn_params.device_id, 
                    strlen(conn_params.device_id), 
                    p, end)) {
                    return -1;
                }
                
                if (lws_add_http_header_by_name(wsi, 
                    (unsigned char *)"Client-Id:", 
                    (unsigned char *)conn_params.client_id, 
                    strlen(conn_params.client_id), 
                    p, end)) {
                    return -1;
                }
            }
            break;
            
        default:
            break;
    }
    
    return 0;
}

// 实现 ProcessTxtDataFrmServer 函数
void ProcessTxtDataFrmServer(unsigned char *msg, size_t len) {
    if (msg && len > 0) {
        // 解析JSON数据
        cJSON *json = cJSON_Parse((char *)msg);
        if (json != NULL) {
            cJSON *type = cJSON_GetObjectItem(json, "type");
            if (type != NULL && cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ProcessHello(json);
                } else if (strcmp(type->valuestring, "tts") == 0) {
                    ProcessTTS(json);
                } else if (strcmp(type->valuestring, "stt") == 0) {
                    ProcessSTT(json);
                }
            }
            cJSON_Delete(json);
        }
    }
}

// 实现 ProcessHello 函数
void ProcessHello(cJSON *json) {
    fprintf(stderr, "[处理] Hello消息\n");
    // 在这里实现对Hello消息的处理逻辑
    StartListen(g_client_data->wsi);
}

// 实现 ProcessTTS 函数
void ProcessTTS(cJSON *json) {
    fprintf(stderr, "[处理] TTS消息\n");
    
    if (json == NULL) {
        fprintf(stderr, "[错误] JSON参数为空\n");
        return;
    }
    
    cJSON *state = cJSON_GetObjectItem(json, "state");
    if (state != NULL && cJSON_IsString(state)) {
        if (strcmp(state->valuestring, "stop") == 0) {
            fprintf(stderr, "[信息] TTS状态为stop，调用StartListen\n");
            // 调用StartListen函数
            if (g_client_data && g_client_data->wsi) {
                StartListen(g_client_data->wsi);
            } else {
                fprintf(stderr, "[错误] 无法获取有效的WebSocket实例\n");
            }
        } else {
            fprintf(stderr, "[信息] TTS状态为: %s\n", state->valuestring);
        }
    } else {
        fprintf(stderr, "[信息] 未找到state字段或state不是字符串\n");
    }
}

// 实现 ProcessSTT 函数
void ProcessSTT(cJSON *json) {
    fprintf(stderr, "[处理] STT消息\n");
    // 在这里实现对STT消息的处理逻辑
}

// 实现 ProcessBinDataFrmServer 函数
void ProcessBinDataFrmServer(void *data, size_t len) {
    if (data == NULL || len == 0) {
        return;
    }

#ifdef _WIN32
    // Windows平台网络初始化
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", result);
        return;
    }
#endif

    // 创建UDP套接字
    int sockfd;
    struct sockaddr_in server_addr;
    
#ifdef _WIN32
    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
    
    if (sockfd < 0) {
        perror("UDP套接字创建失败");
        #ifdef _WIN32
            WSACleanup();
        #endif
        return;
    }

    // 设置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(9002);  // 端口5556
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");  // 本地回环地址

    // 发送数据
    ssize_t sent_bytes;
#ifdef _WIN32
    sent_bytes = sendto(sockfd, (const char*)data, (int)len, 0, 
                       (struct sockaddr*)&server_addr, sizeof(server_addr));
#else
    sent_bytes = sendto(sockfd, data, len, 0, 
                       (struct sockaddr*)&server_addr, sizeof(server_addr));
#endif
    
    if (sent_bytes < 0) {
        perror("UDP数据发送失败");
    } else {
        //fprintf(stderr, "[UDP发送] 已发送 %zd 字节到 127.0.0.1:5556\n", sent_bytes);
    }

    // 关闭套接字
#ifdef _WIN32
    closesocket(sockfd);
    WSACleanup();
#else
    close(sockfd);
#endif
}

// StartListen函数：向WebSocket服务器发送指定的文本信息
void StartListen(struct lws *wsi) {
    if (!wsi) {
        fprintf(stderr, "[错误] WebSocket实例为空\n");
        return;
    }
    
    const char *json_msg = "{\"session_id\":\"\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}";
    int msg_len = strlen(json_msg);
    
    // 不使用不存在的函数，直接尝试写入
    unsigned char *buf = malloc(LWS_PRE + msg_len);
    if (!buf) {
        fprintf(stderr, "[错误] 内存分配失败\n");
        return;
    }
    
    memcpy(&buf[LWS_PRE], json_msg, msg_len);
    
    int result = lws_write(wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT);
    if (result < 0) {
        fprintf(stderr, "[错误] 发送listen消息失败，错误码: %d\n", result);
    } else {
        fprintf(stderr, "[成功] 已发送listen消息: %s\n", json_msg);
    }
    
    free(buf);
}

// 协议定义
static struct lws_protocols protocols[] = {
    {
        "xiaozhi-protocol",  // 修改协议名称
        callback_echo,
        sizeof(struct client_data),
        4096,  // RX缓冲区大小
        0,     // ID（未使用）
        NULL,  // 每个会话的用户数据
        0      // TX包大小限制（0=无限制）
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

// 自定义连接信息
typedef struct {
    const char *hostname;
    int port;
    const char *path;
    int use_ssl;
} xiaozhi_server_t;

xiaozhi_server_t xiaozhi_server = {
    "api.tenclass.net",  // 目标服务器
    443,                 // 端口
    "/xiaozhi/v1/",      // 路径
    1                    // 使用SSL
};

int main(int argc, char **argv) {
    struct lws_context *context = NULL;
    struct lws_context_creation_info info;
    struct lws_client_connect_info ccinfo;
    struct client_data data = {0};
    int n = 0;
    
    // 设置全局变量
    g_context = context;
    g_client_data = &data;
    
    // 设置信号处理
    signal(SIGINT, sighandler);
    
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "小智WebSocket客户端 (libwebsockets)\n");
    fprintf(stderr, "版本: %s\n", lws_get_library_version());
    fprintf(stderr, "服务器: api.tenclass.net:443/xiaozhi/v1/\n");
    fprintf(stderr, "SSL: 是\n");
    fprintf(stderr, "========================================\n\n");
    
    // 初始化上下文
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.ssl_ca_filepath = "/etc/ssl/certs/ca-certificates.crt";
    // 创建上下文
    context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "[错误] 创建WebSocket上下文失败\n");
        return 1;
    }
    
    // 更新全局上下文指针
    g_context = context;
    
    // 设置连接信息
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = context;
    ccinfo.address = xiaozhi_server.hostname;
    ccinfo.port = xiaozhi_server.port;
    ccinfo.path = xiaozhi_server.path;
    ccinfo.host = xiaozhi_server.hostname;
    ccinfo.origin = xiaozhi_server.hostname;
    ccinfo.protocol = "xiaozhi-protocol";
    ccinfo.ssl_connection = xiaozhi_server.use_ssl ? LCCSCF_USE_SSL : 0;
    ccinfo.local_protocol_name = "xiaozhi-protocol";
    ccinfo.userdata = &data;

    // 准备HTTP头部
    // 格式: name1\0value1\0name2\0value2\0... 最后以两个连续\0结束
    static const char headers[] =
        "Authorization\0"
        "Bearer test-token\0"
        "Protocol-Version\0"
        "1\0"
        "Device-Id\0"
        "d8:f3:bc:80:23:69\0"
        "Client-Id\0"
        "12345678-1234-1234-1234-123456789012\0"
        "";
    
    // 连接服务器
    fprintf(stderr, "[连接] 正在连接到服务器api.tenclass.net:443...\n");
    struct lws *wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        fprintf(stderr, "[错误] 创建连接失败\n");
        lws_context_destroy(context);
        return 1;
    }
    
    // 事件循环
    fprintf(stderr, "[信息] 进入事件循环 (按Ctrl+C退出)\n");
    fprintf(stderr, "[信息] 等待连接建立...\n");
    
    time_t start_time = time(NULL);
    time_t last_send_time = start_time;
    int message_sent = 0; // 标记是否已发送初始消息
    
    while (n >= 0 && !data.completed && !force_exit) {
        n = lws_service(context, 50); // 50ms超时
        
        time_t now = time(NULL);
        
        // 检查超时
        if (now - start_time > 30) { // 30秒总超时
            fprintf(stderr, "[超时] 连接超时\n");
            break;
        }
        
        // 如果连接建立，发送初始消息
        if (data.established && !data.completed && message_sent == 0) {
            lws_callback_on_writable(data.wsi);
            message_sent = 1;
        }
    }
    
    // 清理资源
    fprintf(stderr, "\n[清理] 正在关闭连接...\n");
    lws_context_destroy(context);
    fprintf(stderr, "[结束] 程序结束\n");
    
    return 0;
}