#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

/* ==================== 配置定义 ==================== */
#define UDP_FORWARD_IP      "127.0.0.1"
#define UDP_FORWARD_PORT    9002
#define UDP_AUDIO_IP        "127.0.0.1"
#define UDP_AUDIO_PORT      9001
#define UDP_AUDIO_MAX_SIZE  4096
#define DEFAULT_PROTOCOL    "xiaozhi-protocol"

/* ==================== 全局变量 ==================== */
static volatile sig_atomic_t interrupted = 0;
static struct lws *client_wsi = NULL;
static int hello_sent = 0;
static char g_session_id[128] = {0};

/* UDP 转发 */
static int g_udp_forward_sockfd = -1;
static struct sockaddr_in g_udp_forward_addr;

/* 音频线程 */
static pthread_t g_audio_thread = 0;
static volatile int g_audio_thread_running = 0;
static int g_udp_audio_sockfd = -1;
static struct sockaddr_in g_udp_audio_addr;

/* 连接参数 */
struct connection_params {
    const char *access_token;
    const char *version;
    const char *device_id;
    const char *client_id;
};

static struct connection_params conn_params = {
    "test-token",
    "1",
    "ee:a4:ee:f8:cc:7c",
    "d90612cb-fd9c-4ddd-9981-a27b238983b7"
};

/* ==================== 函数声明 ==================== */
static int init_udp_forward(void);
static void cleanup_udp_forward(void);
static int init_udp_audio_recv(void);
static void cleanup_udp_audio_recv(void);
static void *AudioThread(void *arg);
static void start_audio_thread(void);
static void stop_audio_thread(void);
static void StartListen(struct lws *wsi);
static void ProcessBinDataFrmServer(unsigned char *data, size_t len);
static void ProcessTxtDataFrmServer(const char *data, size_t len);
static void ProcessHello(cJSON *root);
static void ProcessTTS(cJSON *root);
static void ProcessSTT(cJSON *root);

/* ==================== UDP 转发初始化 ==================== */
static int init_udp_forward(void)
{
    g_udp_forward_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_forward_sockfd < 0) {
        perror("❌ 创建 UDP 转发 Socket 失败");
        return -1;
    }
    
    memset(&g_udp_forward_addr, 0, sizeof(g_udp_forward_addr));
    g_udp_forward_addr.sin_family = AF_INET;
    g_udp_forward_addr.sin_port = htons(UDP_FORWARD_PORT);
    g_udp_forward_addr.sin_addr.s_addr = inet_addr(UDP_FORWARD_IP);
    
    printf("✓ UDP 转发初始化成功，目标：%s:%d\n", UDP_FORWARD_IP, UDP_FORWARD_PORT);
    return 0;
}

/* ==================== UDP 转发清理 ==================== */
static void cleanup_udp_forward(void)
{
    if (g_udp_forward_sockfd >= 0) {
        close(g_udp_forward_sockfd);
        g_udp_forward_sockfd = -1;
    }
}

/* ==================== UDP 音频接收初始化 ==================== */
static int init_udp_audio_recv(void)
{
    int optval = 1;
    
    g_udp_audio_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_audio_sockfd < 0) {
        perror("❌ 创建 UDP 音频接收 Socket 失败");
        return -1;
    }
    
    setsockopt(g_udp_audio_sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(g_udp_audio_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    memset(&g_udp_audio_addr, 0, sizeof(g_udp_audio_addr));
    g_udp_audio_addr.sin_family = AF_INET;
    g_udp_audio_addr.sin_port = htons(UDP_AUDIO_PORT);
    g_udp_audio_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(g_udp_audio_sockfd, (struct sockaddr *)&g_udp_audio_addr, sizeof(g_udp_audio_addr)) < 0) {
        perror("❌ 绑定 UDP 音频接收端口失败");
        close(g_udp_audio_sockfd);
        g_udp_audio_sockfd = -1;
        return -1;
    }
    
    printf("✓ UDP 音频接收初始化成功，监听：%s:%d\n", UDP_AUDIO_IP, UDP_AUDIO_PORT);
    return 0;
}

/* ==================== UDP 音频接收清理 ==================== */
static void cleanup_udp_audio_recv(void)
{
    if (g_udp_audio_sockfd >= 0) {
        shutdown(g_udp_audio_sockfd, SHUT_RD);
        close(g_udp_audio_sockfd);
        g_udp_audio_sockfd = -1;
    }
}

/* ==================== 音频线程（参考 websocket.c 结构） ==================== */
static void *AudioThread(void *arg)
{
    unsigned char buffer[UDP_AUDIO_MAX_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    printf("🎤 [音频线程] 启动，监听 UDP %s:%d\n", UDP_AUDIO_IP, UDP_AUDIO_PORT);
    
    while (g_audio_thread_running && !interrupted) {
        ssize_t recv_len = recvfrom(g_udp_audio_sockfd, buffer, sizeof(buffer), 0,
                                    (struct sockaddr *)&client_addr, &addr_len);
        
        if (recv_len < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("⚠️  UDP 音频接收失败");
            usleep(10000);
            continue;
        }
        
        // 检查结束标志（4 字节 int，值为 0）
        if (recv_len == sizeof(int)) {
            int flag = 0;
            memcpy(&flag, buffer, sizeof(int));
            if (flag == 0) {
                printf("🎤 [音频线程] 收到结束标志，等待新数据...\n");
                usleep(100000);
                continue;
            }
        }
        
        // 通过 WebSocket 发送二进制数据（使用 malloc 创建缓冲区）
        if (client_wsi != NULL && g_audio_thread_running && !interrupted) {
            unsigned char *pkt = malloc(LWS_PRE + recv_len);
            if (pkt) {
                memcpy(&pkt[LWS_PRE], buffer, recv_len);
                
                int sent = lws_write(client_wsi, &pkt[LWS_PRE], recv_len, LWS_WRITE_BINARY);
                if (sent < 0) {
                    fprintf(stderr, "⚠️  WebSocket 发送失败\n");
                } else if (sent < recv_len) {
                    fprintf(stderr, "⚠️  WebSocket 发送不完整：%d/%zd 字节\n", sent, recv_len);
                }
                
                free(pkt);
            } else {
                fprintf(stderr, "⚠️  内存分配失败\n");
            }
        }
    }
    
    printf("🎤 [音频线程] 退出\n");
    pthread_exit(NULL);
}

/* ==================== 启动音频线程 ==================== */
static void start_audio_thread(void)
{
    if (g_audio_thread_running) {
        printf("⚠️  音频线程已在运行\n");
        return;
    }
    
    if (init_udp_audio_recv() != 0) {
        fprintf(stderr, "❌ UDP 音频接收初始化失败\n");
        return;
    }
    
    g_audio_thread_running = 1;
    
    if (pthread_create(&g_audio_thread, NULL, AudioThread, NULL) != 0) {
        perror("❌ 创建音频线程失败");
        g_audio_thread_running = 0;
        cleanup_udp_audio_recv();
        return;
    }
    
    printf("✓ 音频线程已启动\n");
}

/* ==================== 停止音频线程 ==================== */
static void stop_audio_thread(void)
{
    if (!g_audio_thread_running) {
        return;
    }
    
    printf("🛑 停止音频线程...\n");
    g_audio_thread_running = 0;
    
    cleanup_udp_audio_recv();
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    
    if (pthread_timedjoin_np(g_audio_thread, NULL, &ts) != 0) {
        printf("⚠️  音频线程等待超时，强制取消\n");
        pthread_cancel(g_audio_thread);
        pthread_join(g_audio_thread, NULL);
    }
    
    g_audio_thread = 0;
    printf("✓ 音频线程已停止\n");
}

/* ==================== StartListen - 发送监听开始消息 ==================== */
static void StartListen(struct lws *wsi)
{
    if (wsi == NULL || interrupted) {
        return;
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", "");
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", "start");
    cJSON_AddStringToObject(root, "mode", "auto");
    
    char *msg = cJSON_PrintUnformatted(root);
    if (msg == NULL) {
        cJSON_Delete(root);
        return;
    }
    
    printf("📤 [发送] listen 消息：%s\n", msg);
    
    // 使用 malloc 创建缓冲区（参考 websocket.c）
    unsigned char *buf = malloc(LWS_PRE + strlen(msg));
    if (!buf) {
        cJSON_Delete(root);
        free(msg);
        return;
    }
    
    memcpy(&buf[LWS_PRE], msg, strlen(msg));
    
    int sent = lws_write(wsi, &buf[LWS_PRE], strlen(msg), LWS_WRITE_TEXT);
    if (sent < (int)strlen(msg)) {
        printf("❌ 发送 listen 消息失败\n");
    }
    
    free(buf);
    cJSON_Delete(root);
    free(msg);
}

/* ==================== 二进制数据处理 ==================== */
static void ProcessBinDataFrmServer(unsigned char *data, size_t len)
{
    if (g_udp_forward_sockfd >= 0 && !interrupted && len > 0) {
        ssize_t sent = sendto(g_udp_forward_sockfd, data, len, 0,
                              (struct sockaddr *)&g_udp_forward_addr, 
                              sizeof(g_udp_forward_addr));
        if (sent < 0) {
            perror("❌ UDP 转发失败");
        }
    }
}

/* ==================== 文本数据处理 ==================== */
static void ProcessTxtDataFrmServer(const char *data, size_t len)
{
    printf("📝 [文本数据] 收到：%.*s\n", (int)len, data);
    
    cJSON *root = cJSON_Parse(data);
    if (root == NULL) {
        printf("❌ JSON 解析失败\n");
        return;
    }
    
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_item) || type_item->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }
    
    const char *msg_type = type_item->valuestring;
    printf("📋 消息类型：%s\n", msg_type);
    
    if (strcmp(msg_type, "hello") == 0) {
        ProcessHello(root);
    } else if (strcmp(msg_type, "tts") == 0) {
        ProcessTTS(root);
    } else if (strcmp(msg_type, "stt") == 0) {
        ProcessSTT(root);
    }
    
    cJSON_Delete(root);
}

/* ==================== Hello 消息处理 ==================== */
static void ProcessHello(cJSON *root)
{
    printf("✅ [Hello] 服务器握手响应\n");
    if (client_wsi) {
        StartListen(client_wsi);
    }
}

/* ==================== TTS 消息处理 ==================== */
static void ProcessTTS(cJSON *root)
{
    printf("🔊 [TTS] 文本转语音消息\n");
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(state) && state->valuestring != NULL) {
        printf("  状态：%s\n", state->valuestring);
        
        if (strcmp(state->valuestring, "stop") == 0 && !interrupted) {
            printf("🔄 TTS 播放结束，自动开始监听...\n");
            if (client_wsi) {
                StartListen(client_wsi);
            }
        }
    }
}

/* ==================== STT 消息处理 ==================== */
static void ProcessSTT(cJSON *root)
{
    printf("🎤 [STT] 语音转文本消息\n");
    
    cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text) && text->valuestring != NULL) {
        printf("  识别文本：%s\n", text->valuestring);
    }
}

/* ==================== 信号处理 ==================== */
static void sigint_handler(int sig) {
    printf("\n⚠️  收到信号 %d，正在关闭...\n", sig);
    interrupted = 1;
}

/* ==================== WebSocket 回调 ==================== */
static int callback_echo(struct lws *wsi, enum lws_callback_reasons reason,
                         void *user, void *in, size_t len) {

    unsigned char buf[LWS_PRE + 2048];
    unsigned char *p = &buf[LWS_PRE];
    
    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
    {
        unsigned char **hdr_ptr = (unsigned char **)in;
        unsigned char *end = (*hdr_ptr) + len;
        
        lws_add_http_header_by_name(wsi, (unsigned char *)"Authorization:", 
            (unsigned char *)conn_params.access_token, strlen(conn_params.access_token), hdr_ptr, end);
        lws_add_http_header_by_name(wsi, (unsigned char *)"Protocol-Version:", 
            (unsigned char *)conn_params.version, strlen(conn_params.version), hdr_ptr, end);    
        lws_add_http_header_by_name(wsi, (unsigned char *)"Device-Id:", 
            (unsigned char *)conn_params.device_id, strlen(conn_params.device_id), hdr_ptr, end);
        lws_add_http_header_by_name(wsi, (unsigned char *)"Client-Id:", 
            (unsigned char *)conn_params.client_id, strlen(conn_params.client_id), hdr_ptr, end);
        break;
    }
    
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("✅ 连接成功！\n");
        client_wsi = wsi;
        hello_sent = 0;
        start_audio_thread();
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        printf("❌ 连接错误：%s\n", in ? (char *)in : "未知错误");
        interrupted = 1;
        break;
        
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (!hello_sent) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "type", "hello");
            cJSON_AddNumberToObject(root, "version", 1);
            cJSON_AddStringToObject(root, "transport", "websocket");
            
            cJSON *features = cJSON_CreateObject();
            cJSON_AddBoolToObject(features, "mcp", true);
            cJSON_AddItemToObject(root, "features", features);

            cJSON *audio_params = cJSON_CreateObject();
            cJSON_AddStringToObject(audio_params, "format", "opus");
            cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
            cJSON_AddNumberToObject(audio_params, "channels", 1);
            cJSON_AddNumberToObject(audio_params, "frame_duration", 60);
            cJSON_AddItemToObject(root, "audio_params", audio_params);
            
            char *msg = cJSON_PrintUnformatted(root);

            size_t msg_len = strlen(msg);
            memcpy(&buf[LWS_PRE], msg, msg_len);
            printf("📝 发送：%.*s\n", (int)msg_len, &buf[LWS_PRE]);
            int n = lws_write(wsi, &buf[LWS_PRE], msg_len, LWS_WRITE_TEXT);
            if (n < (int)msg_len) {
                printf("❌ 发送失败\n");
            }
            
            cJSON_Delete(root);
            free(msg);
            hello_sent = 1;
        }
        break;
    
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (lws_frame_is_binary(wsi)) {
            ProcessBinDataFrmServer((unsigned char *)in, len);
        } else {
            ProcessTxtDataFrmServer((const char *)in, len);
        }
        break;
    
    case LWS_CALLBACK_CLIENT_CLOSED:
        printf("🔒 连接关闭\n");
        stop_audio_thread();
        client_wsi = NULL;
        interrupted = 1;
        break;
        
    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { DEFAULT_PROTOCOL, callback_echo, 4096, 0, NULL, 0 },
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

/* ==================== 主函数 ==================== */
int main(void) {
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
    
    struct lws_context *context;
    struct lws_context_creation_info ctx_info;
    struct lws_client_connect_info conn_info;
    
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);
    
    printf("========================================\n");
    printf("小智 WebSocket 客户端 (websocket_test)\n");
    printf("版本：%s\n", lws_get_library_version());
    printf("服务器：api.tenclass.net:443/xiaozhi/v1/\n");
    printf("========================================\n\n");
    
    if (init_udp_forward() != 0) {
        return 1;
    }
    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = protocols;
    ctx_info.gid = -1;
    ctx_info.uid = -1;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ctx_info.ssl_ca_filepath = "/etc/ssl/certs/ca-certificates.crt";
    
    context = lws_create_context(&ctx_info);
    if (!context) {
        fprintf(stderr, "创建上下文失败\n");
        cleanup_udp_forward();
        return 1;
    }
    
    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.context = context;
    conn_info.address = xiaozhi_server.hostname;
    conn_info.port = xiaozhi_server.port;
    conn_info.path = xiaozhi_server.path;
    conn_info.host = xiaozhi_server.hostname;
    conn_info.origin = xiaozhi_server.hostname;
    conn_info.protocol = "xiaozhi-protocol";
    conn_info.ssl_connection = xiaozhi_server.use_ssl ? LCCSCF_USE_SSL : 0;
    conn_info.local_protocol_name = "xiaozhi-protocol";
    
    printf("正在连接 wss://api.tenclass.net/xiaozhi/v1/ ...\n");
    
    struct lws *wsi = lws_client_connect_via_info(&conn_info);
    if (!wsi) {
        fprintf(stderr, "连接失败\n");
        lws_context_destroy(context);
        cleanup_udp_forward();
        return 1;
    }
    
    printf("[信息] 进入事件循环 (按 Ctrl+C 退出)\n");
    
    while (!interrupted) {
        lws_service(context, 50);
    }
    
    printf("正在清理资源...\n");
    
    if (g_audio_thread_running) {
        stop_audio_thread();
    }
    
    lws_context_destroy(context);
    cleanup_udp_forward();
    
    printf("程序结束\n");
    return 0;
}