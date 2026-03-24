#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <libwebsockets.h>

// 声明在http.c中的函数
int http_main();
int perform_json_post_request(const char* url, const char* uuid, const char* mac);

// 声明在websocket.c中的函数
int websocket_main(int argc, char **argv);

int main(int argc, char **argv) {
    printf("控制中心程序启动...\n");
    
    // 初始化libcurl
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        fprintf(stderr, "libcurl初始化失败: %s\n", curl_easy_strerror(res));
        return 1;
    }
    
    printf("\n========================================\n");
    printf("开始设备激活流程...\n");
    printf("========================================\n");
    
    // 循环激活设备直到成功
    int activation_result;
    int activation_attempts = 0;
    const int max_attempts = 100; // 设置最大尝试次数，防止无限循环
    
    do {
        activation_attempts++;
        printf("第 %d 次激活尝试...\n", activation_attempts);
        
        activation_result = perform_json_post_request(
            "https://api.tenclass.net/xiaozhi/ota/", 
            "12345678-1234-1234-1234-123456789012", 
            "d8:f3:bc:80:23:69"
        );
        
        if (activation_result == 0) {
            printf("设备激活成功!\n");
            break; // 激活成功，退出循环
        } else if (activation_result == 1) {
            printf("设备需要继续激活，等待激活码...\n");
            // 等待一段时间再继续下一次请求
            #ifdef _WIN32
                Sleep(5000); // Windows下使用Sleep，参数是毫秒
            #else
                sleep(5); // Linux/Unix下使用sleep，参数是秒
            #endif
        } else {
            printf("设备激活请求失败，稍后重试...\n");
            #ifdef _WIN32
                Sleep(5000); // Windows下使用Sleep，参数是毫秒
            #else
                sleep(5); // Linux/Unix下使用sleep，参数是秒
            #endif
        }        
        
    } while (activation_result != 0); // 激活未成功时继续循环
    
    if (activation_result == 0) {
        printf("设备激活成功完成，总共尝试了 %d 次。\n", activation_attempts);
    } else {
        printf("设备激活失败，在 %d 次尝试后仍未成功。\n", activation_attempts);
    }
    
    printf("\n========================================\n");
    printf("开始与服务交互 (WebSocket)...\n");
    printf("========================================\n");
    
    // 调用websocket.c的代码与服务交互
    // 如果需要传递参数给websocket，可以在这里设置
    int websocket_result = websocket_main(argc, argv);
    
    if (websocket_result == 0) {
        printf("WebSocket通信成功完成!\n");
    } else {
        printf("WebSocket通信出现问题!\n");
    }
    
    // 清理libcurl资源
    curl_global_cleanup();
    
    printf("\n========================================\n");
    printf("控制中心程序执行完毕\n");
    printf("========================================\n");
    
    return 0;
}