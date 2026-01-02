#include <stdio.h>
#include <fcntl.h>
#include <libavutil/log.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavutil/time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/signal.h>

#define FILE_NMAE_OUTPUT    "/mnt/sd/video_%Y%m%d_%H%M%S.mov"
#define FILE_FORMAT_OUTPUT  "mov"

//#define GOTO_ERR(s)     if(ret < 0){av_log(NULL, AV_LOG_ERROR, s);goto _ERROR;}
#define GOTO_ERR(s)     if(ret < 0){av_log(NULL, AV_LOG_ERROR, s, av_err2str(ret));goto _ERROR;}                        

int ret = -1;
AVFormatContext *video_in_ctx  = NULL;
AVFormatContext *audio_in_ctx  = NULL;
AVFormatContext *out_ctx  = NULL;
AVInputFormat *input_format = NULL;

AVDictionary *input_dic = NULL;
AVDictionary *output_dic = NULL;

AVStream *audio_stream = NULL;
AVStream *video_stream = NULL;

AVPacket *vpkt = NULL;
AVPacket *apkt = NULL;

AVCodec *vcodec = NULL;
AVCodecContext *vcodec_ctx = NULL;
AVFrame *frame = NULL;

int video_stream_index = -1;
int audio_stream_index = -1;

int64_t video_start_pts = 0;
int64_t video_start_dts = 0;
int64_t audio_start_pts = 0;
int64_t audio_start_dts = 0;

int frame_count = 0;
int max_frames = 300; // 采集300帧后退出

int is_running = 1; // 控制子线程退出的标志
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int64_t total_audio_samples = 0;
pthread_t audio_thread_id;

#define SHM_FRM_NAME     "/shm_frm" //摄像头信号量
struct camera_frm_struct
{
    pthread_mutex_t camera_frm_mutex;
    pthread_cond_t camera_frm_cond;
    int new_frame_ready; // 0表示空闲，1表示数据已准备好
    uint32_t frm[];
};
struct camera_frm_struct *camera_frm;


// 辅助宏：将值限制在 0-255 范围内
#define CLAMP(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))


void sig_handler(int sig)
{
    is_running = 0;
    pthread_join(audio_thread_id, NULL);
    av_write_trailer(out_ctx);
}


/**
 * 将 YUVJ422P (Full Range) 转换为 ARGB8888
 * 
 * 格式说明:
 * YUVJ422P: 
 *   - Y: 全分辨率
 *   - U, V: 宽度减半 (Width/2)，高度不变 (Height)
 *   - Range: 0-255 (Full Range)
 */
void YUVJ422P_to_ARGB8888(uint8_t *src_data[3], int src_linesize[3], 
                          int width, int height, uint8_t *dst_buffer) {
    
    // 1. 获取平面指针
    uint8_t *y_plane = src_data[0];
    uint8_t *u_plane = src_data[1];
    uint8_t *v_plane = src_data[2];

    // 2. 获取步长 (Stride)
    // 务必使用 linesize，不要假设它等于 width，否则图像会倾斜或越界
    int y_stride = src_linesize[0];
    int u_stride = src_linesize[1];
    int v_stride = src_linesize[2];

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            
            // --- 获取 YUV 数据 ---
            
            // Y 坐标: 1:1
            int Y_val = y_plane[y * y_stride + x];

            // UV 坐标: 
            // 422P 特性: 水平减半 (x/2), 垂直不减 (y 保持不变)
            int uv_x = x / 2; 
            int uv_y = y;     // <--- 420P 这里是 y/2，422P 这里是 y
            
            // 计算 UV 在数组中的偏移量
            int uv_offset = uv_y * u_stride + uv_x;

            int U_val = u_plane[uv_offset] - 128;
            int V_val = v_plane[uv_offset] - 128;

            // --- 转换公式 (JPEG Full Range) ---
            // 整数运算优化，避免浮点数
            // R = Y + 1.402 * V
            // G = Y - 0.344136 * U - 0.714136 * V
            // B = Y + 1.772 * U
            
            int R = Y_val + ((359 * V_val) >> 8);
            int G = Y_val - ((88 * U_val + 183 * V_val) >> 8);
            int B = Y_val + ((454 * U_val) >> 8);

            // --- 写入目标 Buffer (ARGB) ---
            
            // 计算目标索引 (每个像素 4 字节)
            // 再次提醒: 确保 dst_buffer 分配了 width * height * 4 大小
            long dst_idx = (y * width + x) * 4;

            // 逐字节写入，完全避免 Bus Error / 内存对齐问题
            dst_buffer[dst_idx + 0] = CLAMP(B); // Blue
            dst_buffer[dst_idx + 1] = CLAMP(G); // Green
            dst_buffer[dst_idx + 2] = CLAMP(R); // Red
            dst_buffer[dst_idx + 3] = 255;      // Alpha
        }
    }
}

void *audio_sample(void *argv)
{
    apkt = av_packet_alloc();

    // 【重要】获取音频参数，用于计算样本数
    // 通常 ALSA 采集的是 S16LE (2字节)，单声道或双声道
    AVCodecParameters *par = audio_in_ctx->streams[audio_stream_index]->codecpar;
    int bytes_per_sample = av_get_bytes_per_sample(par->format);
    int channels = par->channels;
    
    // 如果获取失败，给一个默认值（视你的硬件而定，通常 8000Hz 16bit 可能是 2字节）
    if (bytes_per_sample == 0) bytes_per_sample = 2; 
    if (channels == 0) channels = 1; 

    while (is_running && frame_count < max_frames)
    {
        ret = av_read_frame(audio_in_ctx, apkt);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "audio sample err");
            return NULL;
        }
        
        if (ret == 0)
        {
            if (apkt->stream_index == audio_stream_index)
            {
                apkt->stream_index = audio_stream->index;

                // ============== 修改开始 ==============
                // 计算当前包里有多少个样本
                int samples_in_pkt = apkt->size / (bytes_per_sample * channels);

                // 根据已经采集的总样本数，手动计算 PTS
                // 公式：PTS = 总样本数 / 采样率
                // 使用 av_rescale_q 转换到输出流的时间基
                apkt->pts = av_rescale_q(total_audio_samples, (AVRational){1, 8000}, audio_stream->time_base);
                apkt->dts = apkt->pts;
                apkt->duration = av_rescale_q(samples_in_pkt, (AVRational){1, 8000}, audio_stream->time_base);
                
                // 累加样本数
                total_audio_samples += samples_in_pkt;

                pthread_mutex_lock(&lock);
                av_interleaved_write_frame(out_ctx, apkt);
                pthread_mutex_unlock(&lock);
            }
        }
        av_packet_unref(apkt);
    }
    av_packet_free(&apkt);
    return NULL;

    // ret = av_read_frame(audio_in_ctx, apkt);
    // if (ret < 0)
    // {
    //     av_log(NULL, AV_LOG_ERROR, "audio sample err1");
    //     exit -1;
    // }
    
    // audio_start_pts = apkt->pts;
    // audio_start_dts = apkt->dts;
    // while (is_running && frame_count < max_frames)
    // {
    //     ret = av_read_frame(audio_in_ctx, apkt);
    //     if (ret < 0)
    //     {
    //         av_log(NULL, AV_LOG_ERROR, "audio sample err");
    //         return NULL;
    //     }
        
    //     if(ret == 0)
    //     {
    //         if (apkt->stream_index == audio_stream_index)
    //         {
    //             apkt->stream_index = audio_stream->index;;
    //             apkt->pts -= audio_start_pts;
    //             apkt->dts -= audio_start_dts;
    //             av_packet_rescale_ts(apkt, audio_in_ctx->streams[audio_stream_index]->time_base, audio_stream->time_base);
    //             pthread_mutex_lock(&lock);
    //             av_interleaved_write_frame(out_ctx, apkt);
    //             pthread_mutex_unlock(&lock);
    //         }
    //     }
    //     av_packet_unref(apkt);
    // }
    // av_packet_free(&apkt);
    return NULL;
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
    if (strcmp(cmd, "camera") != 0 && strcmp(cmd, "playback") != 0 && strcmp(cmd, "camera_p2p") != 0 && strcmp(cmd, "camera(c/s)") != 0)
    {
        perror("Incompatible parameters！");
        return 0;
    }
    
    
    avdevice_register_all();

    av_log_set_level(AV_LOG_INFO);
    ret = avformat_network_init();
    GOTO_ERR("Network flow initialization error:%s\n");

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
    ret = av_dict_set(&input_dic, "sample_rate", "8000", 0);
    ret = av_dict_set(&input_dic, "thread_queue_size", "4096", 0);
    GOTO_ERR("Failed to set parameter framerate of the mic:%s");
    //打开麦克风
    ret = avformat_open_input(&audio_in_ctx, "hw:1,0", input_format, &input_dic);
    GOTO_ERR("Failed to open audio device: %s\n");
    //audio_in_ctx->flags |= AVFMT_FLAG_NONBLOCK;
    //创建输出上下文。如果用了segment会自动创建文件。
    ret = avformat_alloc_output_context2(&out_ctx, NULL, "segment", FILE_NMAE_OUTPUT);
    GOTO_ERR("NO MEMORY!:%s\n");
    //创建视频流
    video_stream = avformat_new_stream(out_ctx, NULL);
    if (video_stream == NULL)
    {
        av_log(out_ctx, AV_LOG_ERROR, "Failed to create the video stream.\n");
        ret = -1;
        goto _ERROR;
    }
    //创建音频流
    audio_stream = avformat_new_stream(out_ctx, NULL);
    if (audio_stream == NULL)
    {
        av_log(out_ctx, AV_LOG_ERROR, "Failed to create the audio stream.\n");
        ret = -1;
        goto _ERROR;
    }
    //查找视频流并拷贝
    video_stream_index = av_find_best_stream(video_in_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream_index < 0)
    {
        av_log(out_ctx, AV_LOG_ERROR, "No video stream was found.\n");
        ret = -1;
        goto _ERROR;
    }
    ret = avcodec_parameters_copy(video_stream->codecpar, video_in_ctx->streams[video_stream_index]->codecpar);
    GOTO_ERR("Video parameters cannot be copied:%s\n");
    video_stream->codecpar->codec_tag = 0;

    //查找音频流并拷贝
    audio_stream_index = av_find_best_stream(audio_in_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (audio_stream_index < 0)
    {
        av_log(out_ctx, AV_LOG_ERROR, "No audio stream was found.\n");
        ret = -1;
        goto _ERROR;
    }
    ret = avcodec_parameters_copy(audio_stream->codecpar, audio_in_ctx->streams[audio_stream_index]->codecpar);
    GOTO_ERR("Audio parameters cannot be copied:%s\n");
    audio_stream->codecpar->codec_tag = 0;

    //设置声道布局掩码。不写会提醒 not writing 'chan' tag due to lack of channel information
    if (audio_stream->codecpar->channel_layout == 0) {
        audio_stream->codecpar->channel_layout = AV_CH_LAYOUT_STEREO;
    }

    //打开输出文件
    // ret = avio_open2(&out_ctx->pb, FILE_NMAE_OUTPUT, AVIO_FLAG_READ_WRITE, NULL, NULL);
    // GOTO_ERR("Audio parameters cannot be copied:%s\n");
    
    // video_stream->time_base = video_in_ctx->streams[video_stream_index]->time_base;
    video_stream->time_base = (AVRational){1, 1000};
    audio_stream->time_base = (AVRational){1, 8000}; 
    // audio_stream->avg_frame_rate = (AVRational){8000, 1};
    //audio_stream->time_base = audio_in_ctx->streams[audio_stream_index]->time_base;

    av_dict_set(&output_dic, "segment_time", "10", 0);
    av_dict_set(&output_dic, "reset_timestamps", "1", 0);
    av_dict_set(&output_dic, "strftime", "1", 0);
    av_dict_set(&output_dic, "segment_format", FILE_FORMAT_OUTPUT, 0);

    ret = avformat_write_header(out_ctx, &output_dic);
    GOTO_ERR("Header writing error.\n");
    
    signal(SIGINT, sig_handler);

    vpkt = av_packet_alloc();
    if (vpkt == NULL)
    {
        av_log(out_ctx, AV_LOG_ERROR, "video Package allocation failed.\n");
        ret = -1;
        goto _ERROR;
    }

    // ret = av_read_frame(video_in_ctx, vpkt);
    // GOTO_ERR("Error in reading packet.\n");
    // video_start_pts = vpkt->pts;
    // video_start_dts = vpkt->dts;
    // av_packet_unref(vpkt);

    //找解码器
    vcodec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (vcodec == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Decoder not found.\n");
        goto _ERROR;
    }
    vcodec_ctx = avcodec_alloc_context3(vcodec);
    if (vcodec_ctx == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Decoder failed to allocate memory.\n");
        goto _ERROR;
    }
    ret = avcodec_parameters_to_context(vcodec_ctx, video_stream->codecpar);
    GOTO_ERR("Failed to copy parameters to the encoder.\n");

    ret = avcodec_open2(vcodec_ctx, vcodec, NULL);
    GOTO_ERR("Failed to open the encoder.\n");

    frame = av_frame_alloc();
    if (frame == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "Frame allocation failed.\n");
        goto _ERROR;
    }

    int width = video_stream->codecpar->width; 
    int height = video_stream->codecpar->height;
    //uint8_t *rgb_buffer = (uint8_t*)malloc(video_stream->codecpar->width * video_stream->codecpar->height * 3);
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
    if((camera_frm = mmap(NULL, sizeof(struct camera_frm_struct) + width * height * 4 ,PROT_READ | PROT_WRITE, MAP_SHARED, shm_frm_fd, 0)) == MAP_FAILED)
    {
        perror("mmap err");
        return -1;
    }

    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;

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

    pthread_create(&audio_thread_id, NULL, audio_sample, NULL);

    int64_t start_time_us = av_gettime(); // 获取当前的微秒时间

    while (1)
    {
        ret = av_read_frame(video_in_ctx, vpkt);
        GOTO_ERR("Error in reading video packet.\n");
        if(ret == 0)
        {
            if (vpkt->stream_index == video_stream_index)
            {
                vpkt->stream_index = video_stream->index;

                int64_t now_us = av_gettime();
        
                // 2. 计算从开始到现在经过了多少时间
                int64_t pts_us = now_us - start_time_us;

                // 3. 将微秒(us)转换为输出流的 time_base
                // av_gettime() 返回的是微秒 (1/1,000,000 秒)
                vpkt->pts = av_rescale_q(pts_us, (AVRational){1, 1000000}, video_stream->time_base);
                vpkt->dts = vpkt->pts;

                vpkt->duration = av_rescale_q(1, (AVRational){1, 1000000}, video_stream->time_base);
                vpkt->pos = -1;

                //av_packet_rescale_ts(vpkt, video_in_ctx->streams[video_stream_index]->time_base, video_stream->time_base);
                

                avcodec_send_packet(vcodec_ctx, vpkt);
                ret = avcodec_receive_frame(vcodec_ctx, frame);
                if (ret == 0)
                {
                    pthread_mutex_lock(&camera_frm->camera_frm_mutex);   //上锁
                    YUVJ422P_to_ARGB8888(frame->data, frame->linesize, width, height, (uint8_t *)camera_frm->frm);
                    camera_frm->new_frame_ready = 1;
                    while (camera_frm->new_frame_ready == 1)
                    {
                        pthread_cond_wait(&camera_frm->camera_frm_cond, &camera_frm->camera_frm_mutex);
                    }
                    pthread_mutex_unlock(&camera_frm->camera_frm_mutex);   //解锁
                }
                
                


                
                pthread_mutex_lock(&lock);
                av_interleaved_write_frame(out_ctx, vpkt);
                pthread_mutex_unlock(&lock);
                
                //frame_count++;
            }
            
        }
        av_packet_unref(vpkt);
    }

    // 停止录制流程
    is_running = 0;
    pthread_join(audio_thread_id, NULL);

    av_write_trailer(out_ctx);

    ret = 0;
_ERROR:
    if (vpkt)
        av_packet_free(&vpkt);
    if (apkt)
        av_packet_free(&apkt);
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

