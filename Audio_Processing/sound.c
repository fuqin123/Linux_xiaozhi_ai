#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <opus/opus.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// 防止ALSA库与pthread库之间的结构体定义冲突
#define _STRUCT_TIMESPEC
#include <pthread.h>

#define SAMPLE_RATE 48000
#define CHANNELS 2
#define FORMAT SND_PCM_FORMAT_S16_LE
#define PERIOD_TIME 60000  // 60ms in microseconds
#define FRAME_SIZE 2880    // 48000 * 0.06 (60ms) = 2880 frames
#define BUFFER_TIME (PERIOD_TIME * 50)  // 4倍period作为buffer时间

// 定义缓冲区大小和数量
#define BUFFER_COUNT 20  // 增加缓冲区数量
#define MAX_ENCODED_SIZE 4000

// UDP端口定义
#define RECORD_PORT 9001  // record线程发送数据到此端口
#define PLAY_PORT   9002    // play线程从此端口接收数据

// 共享缓冲区结构
typedef struct {
    unsigned char data[MAX_ENCODED_SIZE];
    int size;
    int seq_num;
    int valid;  // 标记缓冲区是否有效
} audio_buffer_t;

// 全局缓冲区数组和控制变量
audio_buffer_t audio_buffers[BUFFER_COUNT];
int write_index = 0;
int read_index = 0;
int buffer_full_count = 0;

pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_ready = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_empty = PTHREAD_COND_INITIALIZER;

// 创建Opus编码器
OpusEncoder* create_opus_encoder() {
    int error;
    OpusEncoder *encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK) {
        fprintf(stderr, "无法创建Opus编码器: %s\n", opus_strerror(error));
        return NULL;
    }
    
    // 设置编码参数
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(64000));  // 64kbps
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10));  // 最高质量
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_AUTO));
    
    return encoder;
}

// 创建Opus解码器
OpusDecoder* create_opus_decoder() {
    int error;
    OpusDecoder *decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
    if (error != OPUS_OK) {
        fprintf(stderr, "无法创建Opus解码器: %s\n", opus_strerror(error));
        return NULL;
    }
    return decoder;
}

// 创建UDP套接字
int create_udp_socket() {
    int sockfd;
    
    // 创建UDP套接字
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("无法创建UDP套接字");
        return -1;
    }
    
    return sockfd;
}

// 创建UDP服务器套接字并绑定到指定端口
int create_udp_server_socket(int port) {
    int sockfd;
    struct sockaddr_in server_addr;
    
    // 创建UDP套接字
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("无法创建UDP套接字");
        return -1;
    }
    
    // 配置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    // 绑定套接字到端口
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("绑定UDP套接字失败");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

void* record_audio(void* arg) {
    snd_pcm_t *capture_handle;
    snd_pcm_hw_params_t *hw_params;
    OpusEncoder *encoder;
    short *pcm_buffer;
    unsigned char *encoded_buffer;
    int err;
    int frame_count = 0;
    size_t max_data_bytes = 4000;  // Opus编码后数据的最大可能大小
    int seq_num = 0;
    
    // 创建UDP套接字
    int udp_sockfd = create_udp_socket();
    if (udp_sockfd < 0) {
        exit(EXIT_FAILURE);
    }
    
    // 配置目标地址
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(RECORD_PORT);
    dest_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // 创建Opus编码器
    encoder = create_opus_encoder();
    if (!encoder) {
        exit(EXIT_FAILURE);
    }
    
    // 打开PCM设备
    if ((err = snd_pcm_open(&capture_handle, "hw:audiocodec", SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "无法打开音频设备: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    // 配置PCM设备
    if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
        fprintf(stderr, "无法分配硬件参数结构: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_any(capture_handle, hw_params)) < 0) {
        fprintf(stderr, "无法初始化硬件参数: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_set_access(capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "无法设置访问类型: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_set_format(capture_handle, hw_params, FORMAT)) < 0) {
        fprintf(stderr, "无法设置采样格式: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_set_rate_near(capture_handle, hw_params, &(unsigned int){SAMPLE_RATE}, 0)) < 0) {
        fprintf(stderr, "无法设置采样率: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_set_channels(capture_handle, hw_params, CHANNELS)) < 0) {
        fprintf(stderr, "无法设置声道数: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    
    // 设置period为60ms
    unsigned int period_time = PERIOD_TIME;
    if ((err = snd_pcm_hw_params_set_period_time_near(capture_handle, hw_params, &period_time, 0)) < 0) {
        fprintf(stderr, "无法设置period时间: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    
    // 设置buffer时间为4倍period
    unsigned int buffer_time = BUFFER_TIME;
    if ((err = snd_pcm_hw_params_set_buffer_time_near(capture_handle, hw_params, &buffer_time, 0)) < 0) {
        fprintf(stderr, "无法设置buffer时间: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params(capture_handle, hw_params)) < 0) {
        fprintf(stderr, "无法设置硬件参数: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    snd_pcm_hw_params_free(hw_params);

    if ((err = snd_pcm_prepare(capture_handle)) < 0) {
        fprintf(stderr, "无法准备音频接口: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    
    // 创建缓冲区
    pcm_buffer = malloc(FRAME_SIZE * CHANNELS * sizeof(short));
    encoded_buffer = malloc(max_data_bytes);
    
    printf("开始录音，每个片段60ms，编码为Opus格式，并通过UDP发送到端口 %d...\n", RECORD_PORT);

    // 录音循环
    while (1) {
        // 读取音频数据
        int frames_read = snd_pcm_readi(capture_handle, pcm_buffer, FRAME_SIZE);
        if (frames_read == -EPIPE) {
            fprintf(stderr, "录音缓冲区欠载 (overrun)\n");
            snd_pcm_prepare(capture_handle);
            continue;
        } else if (frames_read < 0) {
            fprintf(stderr, "录音错误: %s\n", snd_strerror(frames_read));
            break;
        }
        
        if (frames_read == 0) continue;  // 没有数据
        
        // 编码为Opus格式
        int encoded_bytes = opus_encode(encoder, pcm_buffer, frames_read, encoded_buffer, max_data_bytes);
        if (encoded_bytes < 0) {
            fprintf(stderr, "Opus编码错误: %s\n", opus_strerror(encoded_bytes));
            break;
        }
        
        // 通过UDP发送编码后的数据
        ssize_t sent_bytes = sendto(udp_sockfd, encoded_buffer, encoded_bytes, 0, 
                                   (struct sockaddr*)&dest_addr, sizeof(dest_addr));
        if (sent_bytes < 0) {
            perror("发送UDP数据失败");
            break;
        }
        
        frame_count += frames_read;
    }

    printf("录音完成\n");

    // 清理资源
    free(pcm_buffer);
    free(encoded_buffer);
    opus_encoder_destroy(encoder);
    snd_pcm_close(capture_handle);
    close(udp_sockfd);
    
    return NULL;
}

void* play_audio(void* arg) {
    snd_pcm_t *playback_handle;
    snd_pcm_hw_params_t *hw_params;
    OpusDecoder *decoder;
    short *pcm_buffer;
    int err;
    
    // 创建UDP服务器套接字，监听端口5556
    int udp_sockfd = create_udp_server_socket(PLAY_PORT);
    if (udp_sockfd < 0) {
        exit(EXIT_FAILURE);
    }
    
    // 创建Opus解码器
    decoder = create_opus_decoder();
    if (!decoder) {
        exit(EXIT_FAILURE);
    }
    
    // 打开PCM设备
    if ((err = snd_pcm_open(&playback_handle, "default", SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "无法打开音频设备: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    // 配置PCM设备
    if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
        fprintf(stderr, "无法分配硬件参数结构: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_any(playback_handle, hw_params)) < 0) {
        fprintf(stderr, "无法初始化硬件参数: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_set_access(playback_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "无法设置访问类型: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_set_format(playback_handle, hw_params, FORMAT)) < 0) {
        fprintf(stderr, "无法设置采样格式: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_set_rate_near(playback_handle, hw_params, &(unsigned int){SAMPLE_RATE}, 0)) < 0) {
        fprintf(stderr, "无法设置采样率: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params_set_channels(playback_handle, hw_params, CHANNELS)) < 0) {
        fprintf(stderr, "无法设置声道数: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    
    // 设置period时间
    unsigned int period_time = PERIOD_TIME;
    if ((err = snd_pcm_hw_params_set_period_time_near(playback_handle, hw_params, &period_time, 0)) < 0) {
        fprintf(stderr, "无法设置period时间: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    
    // 设置buffer时间为4倍period
    unsigned int buffer_time = BUFFER_TIME;
    if ((err = snd_pcm_hw_params_set_buffer_time_near(playback_handle, hw_params, &buffer_time, 0)) < 0) {
        fprintf(stderr, "无法设置buffer时间: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    if ((err = snd_pcm_hw_params(playback_handle, hw_params)) < 0) {
        fprintf(stderr, "无法设置硬件参数: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }

    snd_pcm_hw_params_free(hw_params);

    if ((err = snd_pcm_prepare(playback_handle)) < 0) {
        fprintf(stderr, "无法准备音频接口: %s\n", snd_strerror(err));
        exit(EXIT_FAILURE);
    }
    
    // 创建缓冲区
    pcm_buffer = malloc(FRAME_SIZE * CHANNELS * sizeof(short));
    
    printf("开始从端口 %d 接收Opus数据并播放...\n", PLAY_PORT);

    // 初始化一些变量用于处理underrun情况
    int pending_frames = 0;
    int silent_frames = 0;
    const int silent_frame_threshold = 5; // 允许最多5个静默帧来填充
    
    // 播放循环
    while (1) {
        // 接收UDP数据
        unsigned char encoded_buffer[MAX_ENCODED_SIZE];
        ssize_t received_bytes = recv(udp_sockfd, encoded_buffer, MAX_ENCODED_SIZE, 0);
        if (received_bytes < 0) {
            perror("接收UDP数据失败");
            continue; // 继续尝试接收
        }
        
        // 解码Opus数据
        int decoded_samples = opus_decode(decoder, encoded_buffer, received_bytes, pcm_buffer, FRAME_SIZE, 0);
        if (decoded_samples < 0) {
            fprintf(stderr, "Opus解码错误: %s\n", opus_strerror(decoded_samples));
            continue;
        }
        
        // 播放解码后的音频
        int frames_written = snd_pcm_writei(playback_handle, pcm_buffer, decoded_samples);
        if (frames_written == -EPIPE) {
            fprintf(stderr, "播放缓冲区不足 (underrun)\n");
            snd_pcm_prepare(playback_handle);  // 恢复PCM状态
            
            // 尝试重新发送数据
            frames_written = snd_pcm_writei(playback_handle, pcm_buffer, decoded_samples);
            if (frames_written == -EPIPE) {
                fprintf(stderr, "重试仍然失败\n");
                continue;
            }
        } else if (frames_written < 0) {
            fprintf(stderr, "写入音频数据错误: %s\n", snd_strerror(frames_written));
            break;
        }
        
        // 如果写入的帧数少于期望值，补充静默数据
        if (frames_written < decoded_samples) {
            fprintf(stderr, "写入的帧数少于预期: %d/%d\n", frames_written, decoded_samples);
            
            // 用静默数据填充剩余空间
            if (frames_written < FRAME_SIZE) {
                memset((char*)pcm_buffer + frames_written * CHANNELS * sizeof(short), 
                       0, 
                       (FRAME_SIZE - frames_written) * CHANNELS * sizeof(short));
                
                int remaining_frames = FRAME_SIZE - frames_written;
                snd_pcm_writei(playback_handle, 
                               (char*)pcm_buffer + frames_written * CHANNELS * sizeof(short),
                               remaining_frames);
            }
        }
    }

    printf("播放完成\n");

    // 清理资源
    free(pcm_buffer);
    opus_decoder_destroy(decoder);
    snd_pcm_close(playback_handle);
    close(udp_sockfd);
    
    return NULL;
}

int main(int argc, char *argv[]) {
    pthread_t record_thread, play_thread;
    int ret;

    if (argc != 2) {
        fprintf(stderr, "用法: %s <rp>\n", argv[0]);
        fprintf(stderr, "  rp - 同时录音和播放，实现音频实时转发\n");
        fprintf(stderr, "示例:\n");
        fprintf(stderr, "  %s rp  # 同时录音和播放\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (strcmp(argv[1], "rp") == 0) {
        // 创建录音线程
        ret = pthread_create(&record_thread, NULL, record_audio, NULL);
        if (ret) {
            fprintf(stderr, "无法创建录音线程: %d\n", ret);
            exit(EXIT_FAILURE);
        }

        // 创建播放线程
        ret = pthread_create(&play_thread, NULL, play_audio, NULL);
        if (ret) {
            fprintf(stderr, "无法创建播放线程: %d\n", ret);
            exit(EXIT_FAILURE);
        }

        // 等待线程结束
        pthread_join(record_thread, NULL);
        pthread_join(play_thread, NULL);
    } else {
        fprintf(stderr, "无效的操作。请使用 'rp' 进行录音和播放。\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}