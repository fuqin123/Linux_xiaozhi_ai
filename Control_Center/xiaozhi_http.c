#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>

/* ==================== 全局运行状态 ==================== */
static volatile int g_running = 1;

/* ==================== 函数声明 ==================== */
static void graceful_shutdown(void);
static void signal_handler(int sig);

/* ==================== 信号处理 ==================== */
static void signal_handler(int sig)
{
    printf("\n⚠️  收到信号 %d，正在关闭...\n", sig);
    g_running = 0;
}

/* ==================== 优雅退出处理 ==================== */
static void graceful_shutdown(void)
{
    printf("\n[主程序] 开始优雅退出...\n");
    g_running = 0;
    printf("[主程序] 退出处理完成\n");
}

/* ==================== HTTP 响应数据结构 ==================== */
struct ResponseData { 
    char *data; 
    size_t size; 
};

size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct ResponseData *mem = (struct ResponseData *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (ptr == NULL) { 
        printf("not enough memory\n"); 
        return 0; 
    }
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

void init_response(struct ResponseData *res) {
    res->data = malloc(1); 
    res->data[0] = 0; 
    res->size = 0;
}

void free_response(struct ResponseData *res) {
    if (res->data) { 
        free(res->data); 
        res->data = NULL; 
    }
}

/* ==================== 设备信息获取 ==================== */
int generate_uuid(char *uuid_str, size_t str_size) {
    FILE *fp = fopen("/proc/sys/kernel/random/uuid", "r");
    if (fp) {
        fgets(uuid_str, str_size, fp);
        fclose(fp);
        char *newline = strchr(uuid_str, '\n');
        if (newline) *newline = 0;
        return 0;
    }
    srand(time(NULL) ^ getpid());
    snprintf(uuid_str, str_size, "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
             rand() & 0xFFFF, rand() & 0xFFFF, rand() & 0xFFFF,
             (rand() & 0x0FFF) | 0x4000, (rand() & 0x3FFF) | 0x8000,
             rand() & 0xFFFF, rand() & 0xFFFF, rand() & 0xFFFF);
    return 0;
}

int get_mac_address(char *mac_str, size_t str_size, const char *if_name) {
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, if_name, IFNAMSIZ - 1);
    if (ioctl(sock_fd, SIOCGIFHWADDR, &ifr) < 0) { 
        close(sock_fd); 
        return -1; 
    }
    close(sock_fd);
    unsigned char *mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
    snprintf(mac_str, str_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

int get_ip_address(char *ip_str, size_t str_size, const char *if_name) {
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, if_name, IFNAMSIZ - 1);
    ifr.ifr_addr.sa_family = AF_INET;
    if (ioctl(sock_fd, SIOCGIFADDR, &ifr) < 0) { 
        close(sock_fd); 
        return -1; 
    }
    close(sock_fd);
    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    strncpy(ip_str, inet_ntoa(addr->sin_addr), str_size);
    return 0;
}

/* ==================== OTA 激活请求 ==================== */
int ota_activation_request(const char *device_id, const char *client_id, 
                           const char *mac_addr, const char *ip_addr,
                           struct ResponseData *res) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    struct curl_slist *headers = NULL;
    char device_id_header[256], client_id_header[256], user_agent_header[256];
    snprintf(device_id_header, sizeof(device_id_header), "Device-Id: %s", device_id);
    snprintf(client_id_header, sizeof(client_id_header), "Client-Id: %s", client_id);
    snprintf(user_agent_header, sizeof(user_agent_header), "User-Agent: t113-s3/1.0.1");
    
    headers = curl_slist_append(headers, device_id_header);
    headers = curl_slist_append(headers, client_id_header);
    headers = curl_slist_append(headers, user_agent_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    char post_data[1024];
    snprintf(post_data, sizeof(post_data),
             "{\"application\":{\"version\":\"1.0.1\",\"elf_sha256\":\"c8a8ecb6d6fbcda682494d9675cd1ead240ecf38bdde75282a42365a0e396033\"}}");
    
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.tenclass.net/xiaozhi/ota/");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)res);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    CURLcode res_code = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    
    return (res_code == CURLE_OK) ? (int)http_code : -1;
}

/* ==================== 检查激活状态 ==================== */
int check_activation_status(struct ResponseData *res, char *activation_code, size_t code_size) {
    if (!res->data) return 0;
    char *activation_pos = strstr(res->data, "\"activation\"");
    if (!activation_pos) return 1;
    char *code_pos = strstr(activation_pos, "\"code\"");
    if (!code_pos) return 0;
    char *colon_pos = strchr(code_pos, ':');
    if (!colon_pos) return 0;
    char *start = strchr(colon_pos + 1, '"');
    if (!start) return 0;
    start++;
    char *end = strchr(start, '"');
    if (!end) return 0;
    size_t code_len = end - start;
    if (code_len >= code_size) code_len = code_size - 1;
    strncpy(activation_code, start, code_len);
    activation_code[code_len] = 0;
    return 0;
}

/* ==================== 主函数 ==================== */
int main()
{
    CURLcode curl_global_init_code;
    struct ResponseData response;
    int is_activated = 0;
    
    char device_id[64] = {0};
    char client_id[64] = {0};
    char mac_addr[64] = {0};
    char ip_addr[64] = {0};
    char activation_code[64] = {0};
    
    const char *network_if = "eth0";
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    /* 生成 Client-ID */
    if (generate_uuid(client_id, sizeof(client_id)) != 0) {
        strncpy(client_id, "7b94d69a-9808-4c59-9c9b-704333b38aff", sizeof(client_id) - 1);
    }
    
    /* 获取 MAC 地址 */
    if (get_mac_address(mac_addr, sizeof(mac_addr), network_if) != 0) {
        if (get_mac_address(mac_addr, sizeof(mac_addr), "wlan0") != 0) {
            strncpy(mac_addr, "11:22:33:44:55:66", sizeof(mac_addr) - 1);
        }
    }
    strncpy(device_id, "EE:A4:EE:F8:CC:7C", sizeof(device_id) - 1);
    
    /* 获取 IP 地址 */
    if (get_ip_address(ip_addr, sizeof(ip_addr), network_if) != 0) {
        if (get_ip_address(ip_addr, sizeof(ip_addr), "wlan0") != 0) {
            strncpy(ip_addr, "192.168.1.11", sizeof(ip_addr) - 1);
        }
    }
    
    /* 初始化 CURL */
    curl_global_init_code = curl_global_init(CURL_GLOBAL_ALL);
    if (curl_global_init_code != CURLE_OK) {
        fprintf(stderr, "curl_global_init failed: %s\n", curl_easy_strerror(curl_global_init_code));
        return 1;
    }
    
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║              小智设备激活程序                      ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    printf("设备信息:\n");
    printf("  Device-Id: %s\n", device_id);
    printf("  Client-Id: %s\n", client_id);
    printf("  MAC: %s\n", mac_addr);
    printf("  IP: %s\n\n", ip_addr);
    
    /* OTA 激活检查循环 */
    printf("=== OTA 激活检查 ===\n");
    while (!is_activated && g_running) {
        init_response(&response);
        int status_code = ota_activation_request(device_id, client_id, mac_addr, ip_addr, &response);
        
        if (status_code == 200) {
            if (strstr(response.data, "\"error\"")) {
                printf("❌ 服务器返回错误，5 秒后重试...\n");
                sleep(5);
                free_response(&response);
                continue;
            }
            int need_activation = check_activation_status(&response, activation_code, sizeof(activation_code));
            if (need_activation) {
                is_activated = 1;
                printf("✅ 设备已注册\n");
            } else {
                printf("⚠️  需要激活，验证码：%s\n", activation_code);
                sleep(5);
            }
        } else {
            printf("❌ OTA 请求失败，状态码：%d\n", status_code);
            sleep(5);
        }
        free_response(&response);
    }
    
    /* 清理资源 */
    curl_global_cleanup();
    
    printf("\n=== 程序正常退出 ===\n");
    return 0;
}