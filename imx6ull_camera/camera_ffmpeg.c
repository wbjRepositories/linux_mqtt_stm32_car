#include <stdio.h>
#include <fcntl.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>
#include <libavutil/log.h>
#include <libavutil/audio_fifo.h>

#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <time.h>

#include <sys/resource.h>

// #define FILE_NMAE_OUTPUT    "/mnt/sd/video_%Y%m%d_%H%M%S.mov"
// #define FILE_NMAE_OUTPUT    "/mnt/sd/video.mov"
// #define FILE_NMAE_OUTPUT     "/mnt/sd/video_%03d.mov"
#define FILE_NMAE_OUTPUT     "/mnt/sd/video_%Y%m%d_%H%M%S.mov"
// #define FILE_FORMAT_OUTPUT  "mov"
#define AUDIO_OUT_RATE  44100
#define SEGMENT_DURATION_SEC 20.0   // 每个分段大概 20 秒
#define LIST_FILENAME        "/mnt/sd/playlist.ffconcat"

#define SERVER_ADDRESS      "rtmp://192.168.1.15/live/myCamera"
#define NET_PROTOCOL        "flv"
#define UDP_ADDRESS         "udp://192.168.1.15:5000"
#define UDP_PROTOCOL        "mpegts"

//#define GOTO_ERR(s)     if(ret < 0){av_log(NULL, AV_LOG_ERROR, s);goto _ERROR;}
#define GOTO_ERR(s)     if(ret < 0){av_log(NULL, AV_LOG_ERROR, s, av_err2str(ret));goto _ERROR;}                        
#define RET_ERR(s)     if(ret < 0){av_log(NULL, AV_LOG_ERROR, s, av_err2str(ret)); return ret;}  

int ret = -1;
AVFormatContext *video_in_ctx  = NULL;
AVFormatContext *audio_in_ctx  = NULL;
AVFormatContext *out_ctx  = NULL;
AVInputFormat *input_format = NULL;

AVDictionary *input_dic = NULL;
AVDictionary *output_dic = NULL;

AVStream *audio_out_stream = NULL;
AVStream *video_out_stream = NULL;

AVPacket *vpkt_in = NULL;
AVPacket *vpkt_out = NULL;
AVPacket *apkt_in = NULL;
AVPacket *apkt_out = NULL;

AVCodec *vdecoder = NULL;
AVCodecContext *vdecoder_ctx = NULL;
AVCodec *adecoder = NULL;
AVCodecContext *adecoder_ctx = NULL;
AVFrame* vframe_decode = NULL;
AVFrame *vframe_encode = NULL;
AVFrame *aframe_decode = NULL;
AVFrame *aframe_encode = NULL;
AVFrame *resampled_frame = NULL;

AVCodec *vencoder = NULL;
AVCodecContext *vencoder_ctx = NULL;
AVCodec *aencoder = NULL;
AVCodecContext *aencoder_ctx = NULL;
struct SwrContext * swr_ctx = NULL;

// AVCodecParameters *vcodecpar;
// AVCodecParameters *acodecpar;

struct SwsContext * sws_ctx = NULL;


// 定义在全局或上下文结构体中
AVAudioFifo *audio_fifo = NULL;

int64_t audio_next_pts = 0;


int video_stream_index = -1;
int audio_stream_index = -1;

int64_t video_start_pts = 0;
int64_t video_start_dts = 0;
int64_t audio_start_pts = 0;
int64_t audio_start_dts = 0;


int is_running = 1; // 控制子线程退出的标志
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int64_t total_audio_samples = 0;
pthread_t audio_thread_id;

int shm_frm_fd;
int width = 0;
int height = 0;
pthread_mutexattr_t mutex_attr;
pthread_condattr_t cond_attr;
int64_t now_us = 0;
int64_t start_time_us;

// int file_index; // 文件索引
char filename[64]; // 文件名
int64_t seg_start_pts; // 当前分段的起始 PTS
int64_t last_pts;      // 记录上一帧的 PTS 用于计算时长

enum CAMERA_STATUS {
    CAMERA,
    CAMERA_P2P,
    CAMERA_CS
};
enum CAMERA_STATUS camera_status;

#define SHM_FRM_NAME     "/shm_frm" //摄像头信号量
struct camera_frm_struct
{
    pthread_mutex_t camera_frm_mutex; // 24 bytes
    pthread_cond_t camera_frm_cond;   // 48 bytes
    int new_frame_ready;              // 4 bytes (Total: 76)
    
    // === 新增填充代码 ===
    // 填充 52 字节，使得头部总大小变为 128 字节 (128 是 32 的倍数)
    // 这样 frm 就会从 128 字节偏移处开始，满足 SIMD 对齐要求
    char _padding[52];               
    
    uint32_t frm[];                   // 柔性数组，从 offset 128 开始
};
struct camera_frm_struct *camera_frm;


// 辅助宏：将值限制在 0-255 范围内
#define CLAMP(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))

static void close_current_segment(void);

void sig_handler(int sig)
{
    if (camera_status == CAMERA)
        close_current_segment();
    av_log(NULL, AV_LOG_INFO, "Camera: Received %d signal\n", sig);
    exit(0);
}


// /**
//  * 将 YUVJ422P (Full Range) 转换为 ARGB8888
//  * 
//  * 格式说明:
//  * YUVJ422P: 
//  *   - Y: 全分辨率
//  *   - U, V: 宽度减半 (Width/2)，高度不变 (Height)
//  *   - Range: 0-255 (Full Range)
//  */
// void YUVJ422P_to_ARGB8888(uint8_t *src_data[3], int src_linesize[3], 
//                           int width, int height, uint8_t *dst_buffer) {
    
//     // 1. 获取平面指针
//     uint8_t *y_plane = src_data[0];
//     uint8_t *u_plane = src_data[1];
//     uint8_t *v_plane = src_data[2];

//     // 2. 获取步长 (Stride)
//     // 务必使用 linesize，不要假设它等于 width，否则图像会倾斜或越界
//     int y_stride = src_linesize[0];
//     int u_stride = src_linesize[1];
//     int v_stride = src_linesize[2];

//     for (int y = 0; y < height; y++) {
//         for (int x = 0; x < width; x++) {
            
//             // --- 获取 YUV 数据 ---
            
//             // Y 坐标: 1:1
//             int Y_val = y_plane[y * y_stride + x];

//             // UV 坐标: 
//             // 422P 特性: 水平减半 (x/2), 垂直不减 (y 保持不变)
//             int uv_x = x / 2; 
//             int uv_y = y;     // <--- 420P 这里是 y/2，422P 这里是 y
            
//             // 计算 UV 在数组中的偏移量
//             int uv_offset = uv_y * u_stride + uv_x;

//             int U_val = u_plane[uv_offset] - 128;
//             int V_val = v_plane[uv_offset] - 128;

//             // --- 转换公式 (JPEG Full Range) ---
//             // 整数运算优化，避免浮点数
//             // R = Y + 1.402 * V
//             // G = Y - 0.344136 * U - 0.714136 * V
//             // B = Y + 1.772 * U
            
//             int R = Y_val + ((359 * V_val) >> 8);
//             int G = Y_val - ((88 * U_val + 183 * V_val) >> 8);
//             int B = Y_val + ((454 * U_val) >> 8);

//             // --- 写入目标 Buffer (ARGB) ---
            
//             // 计算目标索引 (每个像素 4 字节)
//             // 再次提醒: 确保 dst_buffer 分配了 width * height * 4 大小
//             long dst_idx = (y * width + x) * 4;

//             // 逐字节写入，完全避免 Bus Error / 内存对齐问题
//             dst_buffer[dst_idx + 0] = CLAMP(B); // Blue
//             dst_buffer[dst_idx + 1] = CLAMP(G); // Green
//             dst_buffer[dst_idx + 2] = CLAMP(R); // Red
//             dst_buffer[dst_idx + 3] = 255;      // Alpha
//         }
//     }
// }

void *audio_sample(void *argv)
{
    // 【重要】获取音频参数，用于计算样本数
    // 通常 ALSA 采集的是 S16LE (2字节)，单声道或双声道
    AVCodecParameters *par = audio_in_ctx->streams[audio_stream_index]->codecpar;
    int bytes_per_sample = av_get_bytes_per_sample(par->format);
    int channels = par->channels;
    
    // 如果获取失败，给一个默认值（视你的硬件而定，通常 8000Hz 16bit 可能是 2字节）
    if (bytes_per_sample == 0) bytes_per_sample = 2; 
    if (channels == 0) channels = 2; 


    if (camera_status == CAMERA_CS || camera_status == CAMERA_P2P)
    {
        // // 在循环外分配临时的重采样输出 frame (大小可以稍微大一点，用于承接重采样后的数据)
        resampled_frame = av_frame_alloc();
        resampled_frame->nb_samples = 4096; // 预估最大值
        resampled_frame->format = aencoder_ctx->sample_fmt;
        resampled_frame->channel_layout = aencoder_ctx->channel_layout;
        resampled_frame->sample_rate = aencoder_ctx->sample_rate;
        av_frame_get_buffer(resampled_frame, 0);

        // 用于送给编码器的固定大小 frame (AAC通常是1024)
        aframe_encode = av_frame_alloc();
        aframe_encode->nb_samples = aencoder_ctx->frame_size; // 1024
        aframe_encode->format = aencoder_ctx->sample_fmt;
        aframe_encode->channel_layout = aencoder_ctx->channel_layout;
        aframe_encode->sample_rate = aencoder_ctx->sample_rate;
        av_frame_get_buffer(aframe_encode, 0);
    }

    while (is_running)
    {
        ret = av_read_frame(audio_in_ctx, apkt_in);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "audio sample err");
            return NULL;
        }
        if (ret == 0)
        {
            if (apkt_in->stream_index == audio_stream_index)
            {
                
                if (camera_status == CAMERA_CS || camera_status == CAMERA_P2P)
                {
                    ret = avcodec_send_packet(adecoder_ctx, apkt_in);
                    if (ret < 0)
                    {
                        av_log(NULL, AV_LOG_ERROR, "Failed to send audio packet.%s\n", av_err2str(ret));
                        return NULL;
                    }
                    while (ret >= 0)
                    {
                        ret = avcodec_receive_frame(adecoder_ctx, aframe_decode);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        
                        // 1. 重采样 (将解码数据 aframe_decode 转为 AAC 需要的格式，存入 resampled_frame)
                        // 注意：Swr 需要根据输入样本数计算输出样本数
                        int max_dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, aencoder_ctx->sample_rate) + aframe_decode->nb_samples,
                                                                aencoder_ctx->sample_rate, aencoder_ctx->sample_rate, AV_ROUND_UP); // 假设输入输出采样率相同，如果不同要改这里
                        
                        // 确保 resampled_frame 空间够大
                        if (max_dst_nb_samples > resampled_frame->nb_samples) {
                            av_frame_make_writable(resampled_frame); // 简化的扩容逻辑，实际可能需要重新alloc buffer
                        }

                        int converted_samples = swr_convert(swr_ctx, 
                                                            resampled_frame->data, max_dst_nb_samples,
                                                            (const uint8_t **)aframe_decode->data, aframe_decode->nb_samples);
                        
                        // 2. 将重采样后的数据写入 FIFO 队列
                        av_audio_fifo_write(audio_fifo, (void **)resampled_frame->data, converted_samples);

                        // 3. 当 FIFO 中数据足够一帧 (1024 samples) 时，取出编码
                        while (av_audio_fifo_size(audio_fifo) >= aencoder_ctx->frame_size) 
                        {
                            
                            // 从 FIFO 读取 1024 个样本到 aframe_encode
                            ret = av_audio_fifo_read(audio_fifo, (void **)aframe_encode->data, aencoder_ctx->frame_size);
                            
                            // 设置 PTS (这非常重要，必须连续)
                            aframe_encode->pts = audio_next_pts;
                            audio_next_pts += aframe_encode->nb_samples;

                            // 发送给编码器
                            ret = avcodec_send_frame(aencoder_ctx, aframe_encode);
                            if (ret < 0) {
                                av_log(NULL, AV_LOG_ERROR, "Error sending frame to encoder\n");
                                break;
                            }

                            // 接收编码后的包
                            while (ret >= 0) 
                            {
                                ret = avcodec_receive_packet(aencoder_ctx, apkt_out);
                                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;

                                apkt_out->stream_index = audio_out_stream->index;

                                // 时间基转换：从编码器时间基(1/SampleRate) 转为 封装格式时间基(通常FLV是1/1000)
                                av_packet_rescale_ts(apkt_out, aencoder_ctx->time_base, audio_out_stream->time_base);
                                
                                pthread_mutex_lock(&lock);
                                ret = av_interleaved_write_frame(out_ctx, apkt_out);
                                if (ret < 0)
                                {
                                    av_log(out_ctx, AV_LOG_ERROR, "write error:%s\n",av_err2str(ret));
                                }
                                pthread_mutex_unlock(&lock);
                                
                                av_packet_unref(apkt_out);
                            }
                        }   
                    }
                }


                if (camera_status == CAMERA)
                {
                    apkt_in->stream_index = audio_out_stream->index;
                    // ============== 修改开始 ==============
                    // 计算当前包里有多少个样本
                    int samples_in_pkt = apkt_in->size / (bytes_per_sample * channels);

                    // 根据已经采集的总样本数，手动计算 PTS
                    // 公式：PTS = 总样本数 / 采样率
                    // 使用 av_rescale_q 转换到输出流的时间基
                    // printf("转换前：apkt->pts:%lld\n",apkt_in->pts);
                    apkt_in->pts = av_rescale_q(total_audio_samples, (AVRational){1, AUDIO_OUT_RATE}, audio_out_stream->time_base);
                    apkt_in->dts = apkt_in->pts;
                    // printf("转换后：apkt->pts:%lld\n",apkt_in->pts);
                    apkt_in->duration = av_rescale_q(samples_in_pkt, (AVRational){1, AUDIO_OUT_RATE}, audio_out_stream->time_base);
                    // printf("apkt_in->duration:%lld\n",apkt_in->duration);
                    // 累加样本数
                    total_audio_samples += samples_in_pkt;

                    // printf("samples_in_pkt:%d\n",samples_in_pkt);
                    // printf("total_audio_samples:%lld\n",total_audio_samples);


                    pthread_mutex_lock(&lock);
                    ret = av_interleaved_write_frame(out_ctx, apkt_in);
                    if (ret < 0)
                    {
                        av_log(out_ctx, AV_LOG_ERROR, "write error:%s\n",av_err2str(ret));
                    }
                    pthread_mutex_unlock(&lock);
                }
            }
        }
        av_packet_unref(apkt_in);
    }
    return NULL;
}

static void update_ffconcat(const char *filename, double duration) {
    FILE *fp = fopen(LIST_FILENAME, "a");
    if (!fp) {
        perror("Failed to open ffconcat file");
        return;
    }
    
    // 如果是第一个文件，写入头部
    if (ftell(fp) == 0) {
        fprintf(fp, "ffconcat version 1.0\n");
    }

    fprintf(fp, "file '%s'\n", filename);
    // 手动写入 duration
    fprintf(fp, "duration %.6f\n", duration); 
    
    // 刷新缓冲区，确保掉电不丢失
    fflush(fp);
    fclose(fp);
    printf("[Concat] Added %s with duration %.6f\n", filename, duration);
}

static int open_new_segment(void)
{
    printf("open_new_segment\n");
    time_t raw_time;
    struct tm time_info;

    time(&raw_time);
    localtime_r(&raw_time, &time_info);

    // snprintf(filename, sizeof(filename), FILE_NMAE_OUTPUT, file_index);
    strftime(filename, sizeof(filename), FILE_NMAE_OUTPUT, &time_info);
    if (camera_status == CAMERA)
        ret = avformat_alloc_output_context2(&out_ctx, NULL, NULL, filename);
    if (camera_status == CAMERA_CS)
        ret = avformat_alloc_output_context2(&out_ctx, NULL, NET_PROTOCOL, SERVER_ADDRESS);
    if (camera_status == CAMERA_P2P)
        ret = avformat_alloc_output_context2(&out_ctx, NULL, UDP_PROTOCOL, UDP_ADDRESS);
    RET_ERR("NO MEMORY!:%s\n");
    //创建视频流
    video_out_stream = avformat_new_stream(out_ctx, NULL);
    if (video_out_stream == NULL)
    {
        av_log(out_ctx, AV_LOG_ERROR, "Failed to create the video stream.\n");
        ret = -1;
        // goto _ERROR;
    }
    //创建音频流
    audio_out_stream = avformat_new_stream(out_ctx, NULL);
    if (audio_out_stream == NULL)
    {
        av_log(out_ctx, AV_LOG_ERROR, "Failed to create the audio stream.\n");
        ret = -1;
        // goto _ERROR;
    }
    //查找视频流并拷贝
    video_stream_index = av_find_best_stream(video_in_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream_index < 0)
    {
        av_log(out_ctx, AV_LOG_ERROR, "No video stream was found.\n");
        ret = -1;
        // goto _ERROR;
    }
    // vcodecpar = video_in_ctx->streams[video_stream_index]->codecpar;
    ret = avcodec_parameters_copy(video_out_stream->codecpar, video_in_ctx->streams[video_stream_index]->codecpar);
    RET_ERR("Video parameters cannot be copied:%s\n");
    video_out_stream->codecpar->codec_tag = 0;

    //查找音频流并拷贝
    audio_stream_index = av_find_best_stream(audio_in_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audio_stream_index < 0)
    {
        av_log(out_ctx, AV_LOG_ERROR, "No audio stream was found.\n");
        ret = -1;
        // goto _ERROR;
    }
    // acodecpar = audio_in_ctx->streams[audio_stream_index]->codecpar;
    ret = avcodec_parameters_copy(audio_out_stream->codecpar, audio_in_ctx->streams[audio_stream_index]->codecpar);
    RET_ERR("Audio parameters cannot be copied:%s\n");
    audio_out_stream->codecpar->codec_tag = 0;

    printf("audio_in_ctx->streams[audio_stream_index]->codecpar->sample_rate:%d\n",audio_in_ctx->streams[audio_stream_index]->codecpar->sample_rate);
    printf("audio_out_stream->codecpar->sample_rate:%d\n",audio_out_stream->codecpar->sample_rate);

    //设置声道布局掩码。不写会提醒 not writing 'chan' tag due to lack of channel information
    if (audio_out_stream->codecpar->channel_layout == 0) {
        audio_out_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    }

    // // 使用segment自动生成ffconcat文件
    // av_dict_set(&output_dic, "segment_time", "5", 0);
    // av_dict_set(&output_dic, "reset_timestamps", "1", 0);
    // av_dict_set(&output_dic, "strftime", "1", 0);
    // av_dict_set(&output_dic, "segment_format", FILE_FORMAT_OUTPUT, 0);
    // av_dict_set(&output_dic, "segment_list", "/mnt/sd/playlist.ffconcat", 0);
    // av_dict_set(&output_dic, "segment_list_type", "ffconcat", 0);
    // // 设置列表实时更新（写完一个分片就刷新一次列表）
    // av_dict_set(&output_dic, "segment_list_flags", "live", 0);
    // // 设置 segment_list_size 为 0，表示保留所有历史记录，不删除旧的条目
    // av_dict_set(&output_dic, "segment_list_size", "0", 0);

    if (!vdecoder_ctx)
    {
        // 给视频宽高赋值，只需要赋值一次。
        width = video_out_stream->codecpar->width; 
        height = video_out_stream->codecpar->height;

        //找解码器
        printf("vcodecpar->codec_id:%d\n",video_out_stream->codecpar->codec_id);
        vdecoder = avcodec_find_decoder(video_out_stream->codecpar->codec_id);
        if (vdecoder == NULL)
        {
            av_log(NULL, AV_LOG_ERROR, "Decoder not found.\n");
            return -1;
        }
        vdecoder_ctx = avcodec_alloc_context3(vdecoder);
        if (vdecoder_ctx == NULL)
        {
            av_log(NULL, AV_LOG_ERROR, "Decoder failed to allocate memory.\n");
            return -1;
        }
        ret = avcodec_parameters_to_context(vdecoder_ctx, video_out_stream->codecpar);
        RET_ERR("Failed to copy parameters to the decoder.\n");


        ret = avcodec_open2(vdecoder_ctx, vdecoder, NULL);
        RET_ERR("Failed to open the decoder.\n");


        //uint8_t *rgb_buffer = (uint8_t*)malloc(video_out_stream->codecpar->width * video_out_stream->codecpar->height * 3);
        int shm_frm_fd;
        if((shm_frm_fd = shm_open(SHM_FRM_NAME, O_RDWR, 0644)) < 0)
        {
            fprintf(stderr, "camera:shm_open %s err, errno=%d(%s)\n",SHM_FRM_NAME,errno,strerror(errno));
            return -1;
        }

        if(ftruncate(shm_frm_fd, sizeof(struct camera_frm_struct) + width * height * 4 ) < 0)
        {
            perror("ftruncate err");
            return -1;
        }
        
        //uint32_t *frm;
        //uint32_t frm[CAM_W * CAM_H * 4];
        printf("width:%d\n",width);
        printf("height:%d\n",height);
        if((camera_frm = mmap(NULL, sizeof(struct camera_frm_struct) + width * height * 4 ,PROT_READ | PROT_WRITE, MAP_SHARED, shm_frm_fd, 0)) == MAP_FAILED)
        {
            perror("mmap err");
            return -1;
        }

        // 1. 初始化互斥锁属性
        pthread_mutexattr_init(&mutex_attr);
        // 设置为进程间共享
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        
        // 2. 初始化条件变量属性
        pthread_condattr_init(&cond_attr);
        // 设置为进程间共享
        pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&camera_frm->camera_frm_mutex, &mutex_attr);
        pthread_cond_init(&camera_frm->camera_frm_cond, &cond_attr);
    }


    if (camera_status == CAMERA_CS || camera_status == CAMERA_P2P)
    {
        if (!adecoder_ctx)
        {
            //找解码器
            printf("vcodecpar->codec_id:%d\n",audio_out_stream->codecpar->codec_id);
            adecoder = avcodec_find_decoder(audio_out_stream->codecpar->codec_id);
            if (adecoder == NULL)
            {
                av_log(NULL, AV_LOG_ERROR, "Decoder not found.\n");
                return -1;
            }
            adecoder_ctx = avcodec_alloc_context3(adecoder);
            if (adecoder_ctx == NULL)
            {
                av_log(NULL, AV_LOG_ERROR, "Decoder failed to allocate memory.\n");
                return -1;
            }
            ret = avcodec_parameters_to_context(adecoder_ctx, audio_out_stream->codecpar);
            RET_ERR("Failed to copy parameters to the decoder.\n");


            ret = avcodec_open2(adecoder_ctx, adecoder, NULL);
            RET_ERR("Failed to open the decoder.\n");
        }


        if (!vencoder_ctx)
        {
            vencoder = avcodec_find_encoder_by_name("libopenh264");
            if (vencoder == NULL)
            {
                av_log(NULL, AV_LOG_ERROR, "Encoder not found.\n");
                return -1;
            }
            vencoder_ctx = avcodec_alloc_context3(vencoder);
            if (vencoder_ctx == NULL)
            {
                av_log(NULL, AV_LOG_ERROR, "Encoder failed to allocate memory.\n");
                return -1;
            }
            vencoder_ctx->width = video_out_stream->codecpar->width;
            vencoder_ctx->height = video_out_stream->codecpar->height;
            vencoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            vencoder_ctx->time_base = (AVRational){1, 1000};
            vencoder_ctx->framerate = (AVRational){1000, 1};
            
            vencoder_ctx->bit_rate = 200000;
            vencoder_ctx->rc_max_rate = 200000;
            vencoder_ctx->rc_buffer_size = 400000;
            vencoder_ctx->max_b_frames = 0;
            vencoder_ctx->gop_size = 20; // 每2秒一个I帧

            ret = avcodec_open2(vencoder_ctx, vencoder, NULL);
            RET_ERR("Failed to open the encoder.\n");

            avcodec_parameters_from_context(video_out_stream->codecpar, vencoder_ctx);
        }

        if (!aencoder_ctx)
        {
            aencoder = avcodec_find_encoder_by_name("aac");
            if (aencoder == NULL)
            {
                av_log(NULL, AV_LOG_ERROR, "Encoder not found.\n");
                return -1;
            }
            aencoder_ctx = avcodec_alloc_context3(aencoder);
            if (aencoder_ctx == NULL)
            {
                av_log(NULL, AV_LOG_ERROR, "Encoder failed to allocate memory.\n");
                return -1;
            }

            aencoder_ctx->sample_rate = AUDIO_OUT_RATE;
            aencoder_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
            aencoder_ctx->channels = 2; // 确保设置通道数
            aencoder_ctx->time_base = (AVRational){1, AUDIO_OUT_RATE};
            aencoder_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

            // 【关键修改】RTMP/FLV 必须设置此标志，否则无法生成 AAC 头部信息
            if (out_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
                aencoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            ret = avcodec_open2(aencoder_ctx, aencoder, NULL);
            RET_ERR("Failed to open the audio encoder.\n");

            // 在初始化阶段（aencoder_ctx 打开后）
            audio_fifo = av_audio_fifo_alloc(aencoder_ctx->sample_fmt, aencoder_ctx->channels, 1);

            avcodec_parameters_from_context(audio_out_stream->codecpar, aencoder_ctx);
        }

        swr_ctx = swr_alloc_set_opts(NULL,
                                    aencoder_ctx->channel_layout,
                                    aencoder_ctx->sample_fmt,
                                    aencoder_ctx->sample_rate,
                                    aencoder_ctx->channel_layout,
                                    AV_SAMPLE_FMT_S16,
                                    aencoder_ctx->sample_rate,
                                    0, NULL);
        swr_init(swr_ctx);

        vframe_decode = av_frame_alloc();
    }
    
    video_out_stream->time_base = vdecoder_ctx->time_base;
    audio_out_stream->time_base = (AVRational){1, AUDIO_OUT_RATE}; 

    //打开输出文件
    if (camera_status == CAMERA)
        ret = avio_open2(&out_ctx->pb, filename, AVIO_FLAG_READ_WRITE, NULL, NULL);
    if (camera_status == CAMERA_CS)
        ret = avio_open2(&out_ctx->pb, SERVER_ADDRESS, AVIO_FLAG_READ_WRITE, NULL, NULL);
    if (camera_status == CAMERA_P2P)
        ret = avio_open2(&out_ctx->pb, UDP_ADDRESS, AVIO_FLAG_WRITE, NULL, NULL);
    RET_ERR("Audio parameters cannot be copied:%s\n");
    
    ret = avformat_write_header(out_ctx, NULL);
    RET_ERR("Header writing error.\n");

    is_running = 1;
    pthread_create(&audio_thread_id, NULL, audio_sample, NULL);


    printf("[Segment] Opened %s\n", filename);
}

static void close_current_segment(void)
{
    // 停止录制流程
    is_running = 0;
    pthread_join(audio_thread_id, NULL);

    av_write_trailer(out_ctx);

    double seg_duration = (double)(last_pts - seg_start_pts) * av_q2d(video_out_stream->time_base);

    update_ffconcat(filename, seg_duration);
    
    ret = 0;
    
    if (out_ctx && out_ctx->pb)
        avio_closep(&out_ctx->pb);
    if (out_ctx)
        avformat_free_context(out_ctx);
    if (input_dic)
        av_dict_free(&input_dic);

    // file_index++;

    printf("close_current_segment\n");
}

int main(int argc, char *argv[])
{
    //参数校验
    if (argc < 2)
    {
        perror("The number of parameters must not be less than 2.");
        return 0;
    }
    char *cmd = argv[1];
    printf("cmd:%s\n",cmd);
    if (strcmp(cmd, "camera") == 0)
    {
        camera_status = CAMERA;
    }
    else if (strcmp(cmd, "camera_p2p") == 0)
    {
        camera_status = CAMERA_P2P;
    }
    else if (strcmp(cmd, "camera_cs") == 0)
    {
        camera_status = CAMERA_CS;
    }
    else
    {
        perror("Incompatible parameters！");
        return 0;
    }
    printf("camera_status:%d\n",camera_status);

    // 提高优先级
    setpriority(PRIO_PROCESS, 0, -10);

    avdevice_register_all();

    av_log_set_level(AV_LOG_INFO);
    ret = avformat_network_init();
    GOTO_ERR("Network flow initialization error:%s\n");

    signal(SIGINT, sig_handler);
    vpkt_in = av_packet_alloc();
    if (vpkt_in == NULL)
    {
        av_log(out_ctx, AV_LOG_ERROR, "video Package allocation failed.\n");
        ret = -1;
        goto _ERROR;
    }
    vpkt_out = av_packet_alloc();
    if (vpkt_out == NULL)
    {
        av_log(out_ctx, AV_LOG_ERROR, "video Package allocation failed.\n");
        ret = -1;
        goto _ERROR;
    }

    apkt_in = av_packet_alloc();
    if (apkt_in == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Incorrect allocation of package space.");
        goto _ERROR;
    }

    apkt_out = av_packet_alloc();
    if (apkt_out == NULL)
    {
        av_log(out_ctx, AV_LOG_ERROR, "video Package allocation failed.\n");
        ret = -1;
        goto _ERROR;
    }

    vframe_encode = av_frame_alloc();
    if (vframe_encode == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Frame allocation failed.\n");
        goto _ERROR;
    }

    aframe_decode = av_frame_alloc();
    if (aframe_decode == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Frame allocation failed.\n");
        goto _ERROR;
    }

    //选择摄像输入格式
    input_format = av_find_input_format("v4l2");
    //设置摄像头参数
    ret = av_dict_set(&input_dic, "input_format", "mjpeg", 0);
    GOTO_ERR("Failed to set parameter input_format of the camera:%s");
    ret = av_dict_set(&input_dic, "video_size", "320x240", 0);
    GOTO_ERR("Failed to set parameter video_size of the camera:%s");
    ret = av_dict_set(&input_dic, "framerate", "10", 0);
    GOTO_ERR("Failed to set parameter framerate of the camera:%s");
    //打开摄像头
    ret = avformat_open_input(&video_in_ctx, "/dev/video1", input_format, &input_dic);
    GOTO_ERR("Failed to open video device: %s\n");
    //video_in_ctx->flags |= AVFMT_FLAG_NONBLOCK;

    //选择麦克风输入格式
    input_format = av_find_input_format("alsa");
    //设置麦克风参数
    av_dict_free(&input_dic);
    ret = av_dict_set(&input_dic, "sample_rate", "44100", 0);
    ret = av_dict_set(&input_dic, "thread_queue_size", "4096", 0);
    GOTO_ERR("Failed to set parameter framerate of the mic:%s");
    //打开麦克风
    ret = avformat_open_input(&audio_in_ctx, "hw:1,0", input_format, &input_dic);
    GOTO_ERR("Failed to open audio device: %s\n");
    //audio_in_ctx->flags |= AVFMT_FLAG_NONBLOCK;
    //创建输出上下文。如果用了segment会自动创建文件。
    // ret = avformat_alloc_output_context2(&out_ctx, NULL, NULL, FILE_NMAE_OUTPUT);
    
    
    start_time_us = av_gettime(); // 获取当前的微秒时间
    while (1)
    {
        ret = open_new_segment();
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Failed to open a new file.\n");
            goto _ERROR;
        }

        while (1)
        {
            ret = av_read_frame(video_in_ctx, vpkt_in);
            GOTO_ERR("Error in reading video packet.\n");
            if(ret == 0)
            {
                if (vpkt_in->stream_index == video_stream_index)
                {
                    if (camera_status == CAMERA)
                    {
                        vpkt_in->stream_index = video_out_stream->index;

                        now_us = av_gettime();
        
                        // 2. 计算从开始到现在经过了多少时间
                        int64_t pts_us = now_us - start_time_us;

                        // 3. 将微秒(us)转换为输出流的 time_base
                        // av_gettime() 返回的是微秒 (1/1,000,000 秒)
                        vpkt_in->pts = av_rescale_q(pts_us, (AVRational){1, 1000000}, video_out_stream->time_base);
                        vpkt_in->dts = vpkt_in->pts;

                        //记录当前分段最后一包的pts
                        last_pts = vpkt_in->pts;

                        vpkt_in->duration = av_rescale_q(1000000 / 10, (AVRational){1, 1000000}, video_out_stream->time_base);
                        vpkt_in->pos = -1;
                    }

                    int ret = avcodec_send_packet(vdecoder_ctx, vpkt_in);
                    GOTO_ERR("Failed to send video package:%s\n");
                    
                    ret = avcodec_receive_frame(vdecoder_ctx, vframe_encode);
                    if (ret == 0)
                    {
                        if (camera_status == CAMERA_CS || camera_status == CAMERA_P2P)
                        {
                            if(!sws_ctx)
                            {
                                sws_ctx = sws_getContext(vframe_encode->width, vframe_encode->height, vframe_encode->format, 
                                vframe_encode->width, vframe_encode->height, AV_PIX_FMT_YUV420P, SWS_POINT, 
                                NULL, NULL, NULL);

                                vframe_decode->width = vframe_encode->width;
                                vframe_decode->height = vframe_encode->height;
                                vframe_decode->format = AV_PIX_FMT_YUV420P;
                                av_frame_get_buffer(vframe_decode, 0);
                            }
                            
                            sws_scale(sws_ctx, (const uint8_t *const *)vframe_encode->data, vframe_encode->linesize,
                            0, vframe_encode->height, vframe_decode->data, vframe_decode->linesize);
                            
                            av_frame_copy_props(vframe_decode, vframe_encode);

                            ret = avcodec_send_frame(vencoder_ctx, vframe_decode);
                            GOTO_ERR("An error occurred while sending the data vframe_encode.%s");
                            while (ret >= 0)
                            {
                                ret = avcodec_receive_packet(vencoder_ctx, vpkt_out);
                                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                                    // EAGAIN: 暂时没包了，跳出循环去喂下一帧
                                    // EOF: 编码结束
                                    break;
                                } else if (ret < 0) {
                                    printf("Error in encoding.\n");
                                    break;
                                }

                                vpkt_out->stream_index = video_out_stream->index;
                                now_us = av_gettime();
            
                                // 计算从开始到现在经过了多少时间
                                int64_t pts_us = now_us - start_time_us;

                                // 将微秒(us)转换为输出流的 time_base
                                // av_gettime() 返回的是微秒 (1/1,000,000 秒)
                                vpkt_out->pts = av_rescale_q(pts_us, (AVRational){1, 1000000}, video_out_stream->time_base);
                                vpkt_out->dts = vpkt_out->pts;

                                //记录当前分段最后一包的pts
                                last_pts = vpkt_out->pts;

                                vpkt_out->duration = av_rescale_q(1000000 / 10, (AVRational){1, 1000000}, video_out_stream->time_base);
                                vpkt_out->pos = -1;

                                pthread_mutex_lock(&lock);
                                av_interleaved_write_frame(out_ctx, vpkt_out);
                                if (ret < 0)
                                {
                                    av_log(out_ctx, AV_LOG_ERROR, "write error:%s\n",av_err2str(ret));
                                }
                                pthread_mutex_unlock(&lock);   
                                av_packet_unref(vpkt_out);
                            }
                            // av_frame_unref(vframe_decode);
                        }
                        

                        if (camera_status == CAMERA)
                        {
                            if(!sws_ctx)
                            {
                                sws_ctx = sws_getContext(vframe_encode->width, vframe_encode->height, vframe_encode->format, 
                                vframe_encode->width, vframe_encode->height, AV_PIX_FMT_BGRA, SWS_POINT, 
                                NULL, NULL, NULL);

                            }
                            pthread_mutex_lock(&camera_frm->camera_frm_mutex);   //上锁
                            // YUVJ422P_to_ARGB8888(vframe_encode->data, vframe_encode->linesize, width, height, (uint8_t *)camera_frm->frm);
                            
                            // 定义输出数组的步长 (stride)
                            int dest_linesize[4] = {width * 4, 0, 0, 0}; 
                            uint8_t *dest_data[4] = {(uint8_t *)camera_frm->frm, NULL, NULL, NULL};

                            sws_scale(sws_ctx, (const uint8_t *const *)vframe_encode->data, vframe_encode->linesize, 0, vframe_encode->height, dest_data, dest_linesize);

                            camera_frm->new_frame_ready = 1;
                            while (camera_frm->new_frame_ready == 1)
                            {
                                pthread_cond_wait(&camera_frm->camera_frm_cond, &camera_frm->camera_frm_mutex);
                            }
                            pthread_mutex_unlock(&camera_frm->camera_frm_mutex);   //解锁

                            double current_seg_dur = (double)(vpkt_in->pts - seg_start_pts) * av_q2d(video_out_stream->time_base);

                            if (current_seg_dur >= SEGMENT_DURATION_SEC) 
                            {
                                
                                close_current_segment();
                                seg_start_pts = vpkt_in->pts;
                                break;
                            }
                    
                            pthread_mutex_lock(&lock);
                            av_interleaved_write_frame(out_ctx, vpkt_in);
                            if (ret < 0)
                            {
                                av_log(out_ctx, AV_LOG_ERROR, "write error:%s\n",av_err2str(ret));
                            }
                            pthread_mutex_unlock(&lock);      
                        }
                    }             
                }
            }
            av_packet_unref(vpkt_in);
        }
    }

_ERROR:
    if (vpkt_in)
        av_packet_free(&vpkt_in);
    if (apkt_in)
        av_packet_free(&apkt_in);
    if(video_in_ctx)
        avformat_close_input(&video_in_ctx); 
    if(audio_in_ctx)
        avformat_close_input(&audio_in_ctx);
    // if (out_ctx && out_ctx->pb)
    //     avio_close(out_ctx->pb);
    if (out_ctx)
        avformat_free_context(out_ctx);
    if (input_dic)
        av_dict_free(&input_dic);


    av_log(NULL, AV_LOG_INFO, "end\n");

    return ret;
}

