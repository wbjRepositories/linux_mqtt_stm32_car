/**
 * @file lv_ffmpeg.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_ffmpeg.h"
#if LV_USE_FFMPEG != 0

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>

#include <alsa/asoundlib.h>
#include <libswresample/swresample.h>
#include <pthread.h>
#include <unistd.h>

/*********************
 *      DEFINES
 *********************/
#if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8
    #define AV_PIX_FMT_TRUE_COLOR AV_PIX_FMT_RGB8
#elif LV_COLOR_DEPTH == 16
    #if LV_COLOR_16_SWAP == 0
        #define AV_PIX_FMT_TRUE_COLOR AV_PIX_FMT_RGB565LE
    #else
        #define AV_PIX_FMT_TRUE_COLOR AV_PIX_FMT_RGB565BE
    #endif
#elif LV_COLOR_DEPTH == 32
    #define AV_PIX_FMT_TRUE_COLOR AV_PIX_FMT_BGR0
#else
    #error Unsupported  LV_COLOR_DEPTH
#endif

#define MY_CLASS &lv_ffmpeg_player_class


// 音频输出参数配置 (PCM)
#define AUDIO_OUT_RATE 44100
#define AUDIO_OUT_CHANNELS 2
#define AUDIO_OUT_FORMAT SND_PCM_FORMAT_S16_LE


snd_pcm_t *pcm_handle = NULL;
snd_pcm_hw_params_t *hwparams = NULL;

/**********************
 *      TYPEDEFS
 **********************/
// 包队列
typedef struct {
    AVPacketList *first, *last;
    int nb_packets;
    int size;
    pthread_mutex_t mutex; // 互斥锁保护队列
    pthread_cond_t cond;   // 条件变量（用于唤醒）
} PacketQueue;

struct ffmpeg_context_s {
    AVFormatContext * fmt_ctx;
    AVCodecContext * video_dec_ctx;
    AVStream * video_stream;
    uint8_t * video_src_data[4];
    uint8_t * video_dst_data[4];
    struct SwsContext * sws_ctx;
    int video_stream_idx;
    int video_src_linesize[4];
    int video_dst_linesize[4];
    enum AVPixelFormat video_dst_pix_fmt;
    bool has_alpha;
    // 新增成员音频相关
    int audio_stream_idx;
    AVCodecContext * audio_dec_ctx;
    AVStream * audio_stream;
    struct SwrContext * swr_ctx; // 重采样上下文
    // 新增成员同步相关
    double video_time_base;     // 视频流的时间基准
    double audio_clock;         // 当前音频播放到的时间（基准时间，秒）

     // 队列
    PacketQueue videoq;
    PacketQueue audioq;
    
    // 控制标志
    int abort_request;          // 退出标志
    int pause_request;          // 暂停标志
    int seek_req;               // 是否请求了Seek
    int64_t seek_pos;           // Seek的位置
    float playback_speed;       // 倍速 (1.0, 1.5, 2.0 等)
};

extern int demux_pause_request;    // 解码暂停标志
extern int playback_end;           // 播放结束标志

// lvgl定时器处理同步
extern pthread_mutex_t timer_mutex;
// 输出上下文同步锁
pthread_mutex_t ctx_mutex = PTHREAD_MUTEX_INITIALIZER;

#pragma pack(1)

struct lv_img_pixel_color_s {
    lv_color_t c;
    uint8_t alpha;
};

#pragma pack()

/**********************
 *  STATIC PROTOTYPES
 **********************/

static lv_res_t decoder_info(lv_img_decoder_t * decoder, const void * src, lv_img_header_t * header);
static lv_res_t decoder_open(lv_img_decoder_t * dec, lv_img_decoder_dsc_t * dsc);
static void decoder_close(lv_img_decoder_t * dec, lv_img_decoder_dsc_t * dsc);

static struct ffmpeg_context_s * ffmpeg_open_file(const char * path);
static void ffmpeg_close(struct ffmpeg_context_s * ffmpeg_ctx);
static void ffmpeg_close_src_ctx(struct ffmpeg_context_s * ffmpeg_ctx);
static void ffmpeg_close_dst_ctx(struct ffmpeg_context_s * ffmpeg_ctx);
static int ffmpeg_image_allocate(struct ffmpeg_context_s * ffmpeg_ctx);
static int ffmpeg_get_img_header(const char * path, lv_img_header_t * header);
static uint8_t * ffmpeg_get_img_data(struct ffmpeg_context_s * ffmpeg_ctx);
static int ffmpeg_output_video_frame(struct ffmpeg_context_s * ffmpeg_ctx ,AVFrame *frame);
static bool ffmpeg_pix_fmt_has_alpha(enum AVPixelFormat pix_fmt);
static bool ffmpeg_pix_fmt_is_yuv(enum AVPixelFormat pix_fmt);

static void lv_ffmpeg_player_constructor(const lv_obj_class_t * class_p, lv_obj_t * obj);
static void lv_ffmpeg_player_destructor(const lv_obj_class_t * class_p, lv_obj_t * obj);

#if LV_COLOR_DEPTH != 32
    static void convert_color_depth(uint8_t * img, uint32_t px_cnt);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/
const lv_obj_class_t lv_ffmpeg_player_class = {
    .constructor_cb = lv_ffmpeg_player_constructor,
    .destructor_cb = lv_ffmpeg_player_destructor,
    .instance_size = sizeof(lv_ffmpeg_player_t),
    .base_class = &lv_img_class
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
/**
 * 初始化队列 (辅助函数)
 */
void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

/**
 * 入队函数 (生产者)
 *
 * @param q   队列指针
 * @param pkt 要入队的 Packet。注意：入队后，pkt 的数据所有权转移给队列，
 *            调用者不应再释放 pkt 中的 buf，建议入队后调用 av_packet_unref 或不再使用原 pkt。
 * @return    0 表示成功，<0 表示失败
 */
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;

    // 1. 分配节点内存
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1) {
        return -1;
    }
    
    // 2. 将传入的 pkt 内容（引用）移动到新节点中
    // 注意：这里是浅拷贝结构体，pkt 内部的数据指针（buf）被接管
    pkt1->pkt = *pkt; 
    pkt1->next = NULL;

    // 3. 加锁访问队列
    pthread_mutex_lock(&q->mutex);

    // 4. 更新链表指针
    if (!q->last) {
        // 如果队列为空，头尾都指向新节点
        q->first = pkt1;
    } else {
        // 否则，将新节点挂在末尾
        q->last->next = pkt1;
    }
    q->last = pkt1;

    // 5. 更新统计数据
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    // 6. 发送信号唤醒等待在 packet_queue_get 的消费者线程
    pthread_cond_signal(&q->cond);

    // 7. 解锁
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

/**
 * 将 Packet 插入队列头部 (LIFO 行为，或者用于高优先级消息如 Flush)
 * 
 * @param q   队列指针
 * @param pkt 要插入的包
 * @return    0 成功，<0 失败
 */
int packet_queue_put_front(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;

    // 1. 分配节点内存
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1) {
        return -1;
    }

    // 2. 引用计数拷贝（安全做法）
    // 如果是特殊的 flush_pkt（data为NULL），av_packet_ref 也能正常处理
    if (av_packet_ref(&pkt1->pkt, pkt) < 0) {
        av_free(pkt1);
        return -1;
    }
    
    // 3. 加锁
    pthread_mutex_lock(&q->mutex);

    // 4. 处理链表指针（关键逻辑）
    pkt1->next = q->first; // 新节点的 next 指向当前的头
    q->first = pkt1;       // 新节点成为新的头

    // 5. 处理特殊情况：如果队列原本为空
    if (!q->last) {
        q->last = pkt1; // 头尾都是同一个节点
    }

    // 6. 更新统计
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    // 7. 唤醒等待的线程
    pthread_cond_signal(&q->cond);

    // 8. 解锁
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

/**
 * 出队函数 (消费者)
 *
 * @param q     队列指针
 * @param pkt   用于接收出队数据的 Packet 指针
 * @param block 是否阻塞：1 为阻塞等待，0 为非阻塞立即返回
 * @return      1 表示成功获取，0 表示队列为空且非阻塞，-1 表示错误或退出
 */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    // 1. 加锁
    pthread_mutex_lock(&q->mutex);

    for (;;) {
        pkt1 = q->first;

        if (pkt1) {
            // --- 情况 A: 队列不为空 ---
            
            // 2. 取出头节点，移动 first 指针
            q->first = pkt1->next;
            if (!q->first) {
                q->last = NULL;
            }

            // 3. 更新统计数据
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);

            // 4. 将数据返回给调用者
            *pkt = pkt1->pkt;
            
            // 5. 释放节点内存（注意：不释放 pkt 内部的数据，因为数据已转移给 *pkt）
            av_free(pkt1);
            
            ret = 1;
            break;
        } else if (!block) {
            // --- 情况 B: 队列为空且设为非阻塞 ---
            ret = 0;
            break;
        } else {
            // --- 情况 C: 队列为空且设为阻塞 ---
            // 等待条件变量唤醒（会暂时释放锁，被唤醒后重新持有锁）
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }

    // 6. 解锁
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

/**
 * 清空队列中的所有包，释放内存，重置计数
 * 
 * @param q 队列指针
 */
void packet_queue_flush(PacketQueue *q) {
    AVPacketList *pkt, *pkt1;

    // 1. 加锁，防止在清理过程中有其他线程写入或读取
    pthread_mutex_lock(&q->mutex);

    // 2. 遍历链表释放所有节点
    for (pkt = q->first; pkt; pkt = pkt1) {
        pkt1 = pkt->next; // 先保存下一个节点的指针
        
        // 关键步骤：释放 AVPacket 内部的数据缓冲
        // 这对应于 packet_queue_put 中的 av_packet_ref
        av_packet_unref(&pkt->pkt);
        
        // 释放节点本身的内存
        av_free(pkt);
    }

    // 3. 重置队列状态
    q->last = NULL;
    q->first = NULL;
    q->nb_packets = 0;
    q->size = 0;

    // 4. 解锁
    pthread_mutex_unlock(&q->mutex);
}

/**
 * 销毁队列并释放残留 Packet (辅助函数)
 */
void packet_queue_destroy(PacketQueue *q) {
    // packet_queue_flush(q); // 需实现 flush 清空逻辑
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

// 解码线程
void *demux_thread(void *arg)
{
    struct ffmpeg_context_s *ctx = (struct ffmpeg_context_s *)arg;
    AVPacket *pkt = av_packet_alloc();
    int ret = 0;
    while(!ctx->abort_request) {
        if (demux_pause_request) {usleep(100000); continue;}
         // 1. 处理 Seek (拉进度条)
        if (ctx->seek_req) {
            ctx->seek_req = 0;
            int64_t seek_target = ctx->seek_pos;
            if (av_seek_frame(ctx->fmt_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) >= 0) {
                // 清空队列
                packet_queue_flush(&ctx->videoq);
                packet_queue_flush(&ctx->audioq);
                // 刷新解码器缓冲
                avcodec_flush_buffers(ctx->video_dec_ctx);
                avcodec_flush_buffers(ctx->audio_dec_ctx);

                printf("seek_req000\n");
            }
            printf("seek_req\n");
        }

        // 2. 队列满了就休息一下（防止内存爆掉），提前缓存10mb/5mb
        if (ctx->videoq.size > 10 * 1024 * 1024 || ctx->audioq.size > 5 * 1024 * 1024) {
            usleep(10000); 
            continue;
        }
        pthread_mutex_lock(&ctx_mutex);
        ret = av_read_frame(ctx->fmt_ctx, pkt);
        pthread_mutex_unlock(&ctx_mutex);
        if (ret < 0)
        {
            // 文件结束或错误
            av_log(NULL, AV_LOG_INFO, "File end or error.\n");
            demux_pause_request = 1;
        }
        // 情况 A: 读到视频包
        if (pkt->stream_index == ctx->video_stream_idx) {
            // 发送包到解码器
            ret = packet_queue_put(&ctx->videoq, pkt);
            if (ret < 0)
            {
                exit(0);
            }
        }
        // 情况 B: 读到音频包
        else if (pkt->stream_index == ctx->audio_stream_idx) {
            ret = packet_queue_put(&ctx->audioq, pkt);
            if (ret < 0)
            {
                exit(0);
            }
        }
        else
        {
            av_packet_unref(pkt);
        }
    }
    return NULL;
}

// 音频处理线程，维护音频时钟
void *audio_processing_thread(void *arg)
{
    struct ffmpeg_context_s *ctx = (struct ffmpeg_context_s *)arg;
    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();
    int ret = 0;
    int count; // 队列读取为空计数
    while (!ctx->abort_request)
    {
        if (ctx->pause_request) {usleep(20000); continue;}
        ret = packet_queue_get(&ctx->audioq, &pkt, 0);
        if (ret < 0)
        {
            fprintf(stderr, "An error occurred while reading the queue.");
            return NULL;
        }
        else if (ret == 0)
        {
            count++;
            printf("count:%d\n",count);
            if (count > 10)  // 如果连续多次读取队列都为空，代表读完了，则停止播放。
            {
                ctx->pause_request = 1;
                playback_end = 1;
            }
            usleep(10000);
            continue;
        }
        count = 0;
        
        ret = avcodec_send_packet(ctx->audio_dec_ctx, &pkt);
        if (ret == 0) {
            while (avcodec_receive_frame(ctx->audio_dec_ctx, frame) >= 0) {
                // 1. 重采样音频数据
                // 计算输出样本数
                int out_samples = av_rescale_rnd(swr_get_delay(ctx->swr_ctx, ctx->audio_dec_ctx->sample_rate) +
                                    frame->nb_samples, AUDIO_OUT_RATE, ctx->audio_dec_ctx->sample_rate, AV_ROUND_UP);
                
                // 确保缓冲区够大
                uint8_t *buffer = malloc(out_samples * AUDIO_OUT_CHANNELS * 2); 

                int real_samples = swr_convert(ctx->swr_ctx, &buffer, out_samples, 
                                                (const uint8_t **)frame->data, frame->nb_samples);
                // 2. 写入 ALSA (播放声音)
                if (pcm_handle && real_samples > 0) {
                    // 这里是阻塞写入，可能会稍微影响 UI 流畅度，但在简单实现中是必要的
                    snd_pcm_sframes_t frames_written = snd_pcm_writei(pcm_handle, buffer, real_samples);
                    if (frames_written < 0) {
                        frames_written = snd_pcm_recover(pcm_handle, frames_written, 0);
                        if (frames_written < 0)
                        {
                            fprintf(stderr, "The error cannot be recovered: %s\n", snd_strerror(frames_written));
                            break;
                        }
                    }
                    // 3. 更新音频时钟
                    // 音频时钟 = 当前音频包 PTS。
                    // 更精确的做法是：音频PTS - (已写入声卡但在缓冲区的延迟)。让音频提前放，然后加上缓冲区延时正好和视频同步
                    // 简单做法：直接用音频包的时间戳                        
                    double audio_pts = frame->pts * av_q2d(ctx->audio_stream->time_base);
                    snd_pcm_sframes_t delay_frames;
                    snd_pcm_delay(pcm_handle, &delay_frames);
                    ctx->audio_clock = audio_pts - (double)(delay_frames/ctx->audio_dec_ctx->sample_rate);
                }
                free(buffer);
            }
        }
        av_packet_unref(&pkt);
    }
    return NULL;
}

// 视频处理线程，显示到屏幕上。
void *video_processing_thread(void *arg)
{
    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)arg;
    struct ffmpeg_context_s *ctx = player->ffmpeg_ctx;
    lv_obj_t * obj = (lv_obj_t *)player;
    if(!ctx) return NULL;
    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();
    while (!ctx->abort_request)
    {
        if (ctx->pause_request) {usleep(20000); continue;}
        packet_queue_get(&ctx->videoq, &pkt, 1);
        int ret = avcodec_send_packet(ctx->video_dec_ctx, &pkt);
        if (ret == 0)
        {
            // 接收解码后的帧
            ret = avcodec_receive_frame(ctx->video_dec_ctx, frame);
            if (ret < 0) {
                printf("a\n");
            }

            // 音视频同步逻辑
            double video_pts = frame->best_effort_timestamp * av_q2d(ctx->video_stream->time_base);
            double diff = video_pts - ctx->audio_clock;
            double sync_threshold = 0.03; // 30ms 的同步阈值

            // 视频比音频快
            if (diff > sync_threshold)
            {
                packet_queue_put_front(&ctx->videoq, &pkt);
                continue;
            }
            // 视频比音频慢
            if (diff < -sync_threshold) 
            {
                av_packet_unref(&pkt);
                continue;
            }

            // 转换图像格式给 LVGL
            ffmpeg_output_video_frame(ctx, frame);
            
            av_packet_unref(&pkt);

            pthread_mutex_lock(&timer_mutex);
            lv_img_cache_invalidate_src(lv_img_get_src(obj));
            lv_obj_invalidate(obj);
            pthread_mutex_unlock(&timer_mutex);

            usleep(10000);
        }
    }
    return NULL;
}
/**
 *  获取文件总时长。
 * @param arg lv_ffmpeg_player_t *类型参数。
 * @param buf 输出的时间。
 */
void lv_ffmpeg_player_get_total_time(lv_obj_t * arg, char* buf)
{
    // 安全检查：防止空指针崩溃
    if (arg == NULL || buf == NULL) {
        return;
    }

    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)arg;
    struct ffmpeg_context_s *ctx = player->ffmpeg_ctx;

    // 检查 FFmpeg 上下文是否已初始化
    if (ctx == NULL || ctx->fmt_ctx == NULL) {
        sprintf(buf, "00:00:00");
        return;
    }

    int64_t duration_us = ctx->fmt_ctx->duration;

    // 检查时长是否有效
    if (duration_us == AV_NOPTS_VALUE) {
        // 如果是直播流或无法检测时长，显示 00:00:00
        sprintf(buf, "00:00:00");
        return;
    }

    // 计算时长
    // 为了避免浮点精度问题，使用整数运算 + 四舍五入
    // duration_us / 1000000 (AV_TIME_BASE)
    int64_t total_seconds = (duration_us + 500000) / AV_TIME_BASE; // +500000 用于四舍五入

    int hours = total_seconds / 3600;
    int mins  = (total_seconds % 3600) / 60;
    int secs  = total_seconds % 60;

    // 格式化输出
    // 使用 %02d 确保是 "05:01:09" 而不是 "5:1:9"
    sprintf(buf, "%02d:%02d:%02d", hours, mins, secs);
}

/**
 *  获取音视频当前时间。
 * @param arg lv_ffmpeg_player_t *类型参数。
 * @param buf 输出的时间。
 */
void lv_ffmpeg_player_get_current_time(lv_obj_t * arg, char* buf)
{
    // 安全检查：防止空指针崩溃
    if (arg == NULL || buf == NULL) {
        return;
    }

    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)arg;
    struct ffmpeg_context_s *ctx = player->ffmpeg_ctx;

    // 检查 FFmpeg 上下文是否已初始化
    if (ctx == NULL || ctx->fmt_ctx == NULL) {
        sprintf(buf, "00:00:00");
        return;
    }

    int64_t current_seconds = (int64_t)ctx->audio_clock;

    int hours = current_seconds / 3600;
    int mins  = (current_seconds % 3600) / 60;
    int secs  = current_seconds % 60;

    sprintf(buf, "%02d:%02d:%02d", hours, mins, secs);
}


void lv_ffmpeg_player_seek(lv_obj_t * arg, char* buf)
{
    // 安全检查：防止空指针崩溃
    if (arg == NULL || buf == NULL) {
        return;
    }

    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)arg;
    struct ffmpeg_context_s *ctx = player->ffmpeg_ctx;

    // 检查 FFmpeg 上下文是否已初始化
    if (ctx == NULL || ctx->fmt_ctx == NULL) {
        sprintf(buf, "00:00:00");
        return;
    }

    int hour, minute, second, pos;
    sscanf(buf, "%d:%d:%d", &hour, &minute, &second);
    pos = hour * 3600 + minute * 60 + second;

    ctx->seek_pos = pos * AV_TIME_BASE;
    ctx->seek_req = 1;

    printf("seek\n");
}


void lv_ffmpeg_init(void)
{
    lv_img_decoder_t * dec = lv_img_decoder_create();
    lv_img_decoder_set_info_cb(dec, decoder_info);
    lv_img_decoder_set_open_cb(dec, decoder_open);
    lv_img_decoder_set_close_cb(dec, decoder_close);

#if LV_FFMPEG_AV_DUMP_FORMAT == 0
    av_log_set_level(AV_LOG_INFO);
#endif
}

int lv_ffmpeg_get_frame_num(const char * path)
{
    int ret = -1;
    struct ffmpeg_context_s * ffmpeg_ctx = ffmpeg_open_file(path);

    if(ffmpeg_ctx) {
        ret = ffmpeg_ctx->video_stream->nb_frames;
        ffmpeg_close(ffmpeg_ctx);
    }

    return ret;
}


/**
 * 初始化alsa
 */
int alsa_init(void)
{
    int ret = 0;
    ret = snd_pcm_open(&pcm_handle, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0)
    {
        fprintf(stderr, "The audio device failed to open:%s", snd_strerror(ret));
        goto _FAILED;
    }
    ret = snd_pcm_hw_params_malloc(&hwparams);
    if (ret < 0)
    {
        fprintf(stderr, "Failed to allocate parameter space:%s", snd_strerror(ret));
        goto _FAILED;
    }
    ret = snd_pcm_hw_params_any(pcm_handle, hwparams);
    if (ret < 0)
    {
        fprintf(stderr, "Initialization parameters failed:%s", snd_strerror(ret));
        goto _FAILED;
    }

    snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_handle, hwparams,AUDIO_OUT_FORMAT);
    snd_pcm_hw_params_set_channels(pcm_handle, hwparams, 2);
    snd_pcm_hw_params_set_rate(pcm_handle, hwparams, AUDIO_OUT_RATE, 0);
    snd_pcm_hw_params_set_period_size(pcm_handle, hwparams, 1024, 0);
    snd_pcm_hw_params_set_buffer_size(pcm_handle, hwparams, 16*1024);
    snd_pcm_hw_params(pcm_handle, hwparams);

    snd_pcm_prepare(pcm_handle);

_FAILED:
    if (hwparams)
        snd_pcm_hw_params_free(hwparams);
    return ret;
}

lv_obj_t * lv_ffmpeg_player_create(lv_obj_t * parent)
{
    lv_obj_t * obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

lv_res_t lv_ffmpeg_player_set_src(lv_obj_t * obj, const char * path)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_res_t res = LV_RES_INV;

    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)obj;

    if(player->ffmpeg_ctx) {
        ffmpeg_close(player->ffmpeg_ctx);
        player->ffmpeg_ctx = NULL;
    }


    player->ffmpeg_ctx = ffmpeg_open_file(path);

    if(!player->ffmpeg_ctx) {
        LV_LOG_ERROR("ffmpeg file open failed: %s", path);
        goto failed;
    }

    if(ffmpeg_image_allocate(player->ffmpeg_ctx) < 0) {
        LV_LOG_ERROR("ffmpeg image allocate failed");
        ffmpeg_close(player->ffmpeg_ctx);
        goto failed;
    }

    bool has_alpha = player->ffmpeg_ctx->has_alpha;
    int width = player->ffmpeg_ctx->video_dec_ctx->width;
    int height = player->ffmpeg_ctx->video_dec_ctx->height;
    uint32_t data_size = 0;

    if(has_alpha) {
        data_size = width * height * LV_IMG_PX_SIZE_ALPHA_BYTE;
    }
    else {
        data_size = width * height * LV_COLOR_SIZE / 8;
    }

    player->imgdsc.header.always_zero = 0;
    player->imgdsc.header.w = width;
    player->imgdsc.header.h = height;
    player->imgdsc.data_size = data_size;
    player->imgdsc.header.cf = has_alpha ? LV_IMG_CF_TRUE_COLOR_ALPHA : LV_IMG_CF_TRUE_COLOR;
    player->imgdsc.data = ffmpeg_get_img_data(player->ffmpeg_ctx);

    lv_img_set_src(&player->img.obj, &(player->imgdsc));


    res = LV_RES_OK;

    // 初始化音视频包队列
    packet_queue_init(&player->ffmpeg_ctx->audioq);
    packet_queue_init(&player->ffmpeg_ctx->videoq);
    // 运行解码、音频、视频线程
    pthread_t demux_pid,audio_pid,video_pid;
    pthread_create(&demux_pid, NULL, demux_thread, player->ffmpeg_ctx);
    pthread_create(&audio_pid, NULL, audio_processing_thread, player->ffmpeg_ctx);
    pthread_create(&video_pid, NULL, video_processing_thread, player);

failed:
    return res;
}

void lv_ffmpeg_player_set_cmd(lv_obj_t * obj, lv_ffmpeg_player_cmd_t cmd)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)obj;

    if(!player->ffmpeg_ctx) {
        LV_LOG_ERROR("ffmpeg_ctx is NULL");
        return;
    }


    switch(cmd) {
        case LV_FFMPEG_PLAYER_CMD_START:
            pthread_mutex_lock(&ctx_mutex);
            av_seek_frame(player->ffmpeg_ctx->fmt_ctx,
                          0, 0, AVSEEK_FLAG_BACKWARD);
            pthread_mutex_unlock(&ctx_mutex);
            player->ffmpeg_ctx->pause_request = 0;
            LV_LOG_INFO("ffmpeg player start");
            break;
        case LV_FFMPEG_PLAYER_CMD_STOP:
            pthread_mutex_lock(&ctx_mutex);
            av_seek_frame(player->ffmpeg_ctx->fmt_ctx,
                          0, 0, AVSEEK_FLAG_BACKWARD);
            player->ffmpeg_ctx->pause_request = 1;
            pthread_mutex_unlock(&ctx_mutex);              
            LV_LOG_INFO("ffmpeg player stop");
            break;
        case LV_FFMPEG_PLAYER_CMD_PAUSE:
            pthread_mutex_lock(&ctx_mutex);
            player->ffmpeg_ctx->pause_request = 1;
            pthread_mutex_unlock(&ctx_mutex);
            LV_LOG_INFO("ffmpeg player pause");
            break;
        case LV_FFMPEG_PLAYER_CMD_RESUME:
            pthread_mutex_lock(&ctx_mutex);
            pthread_mutex_unlock(&ctx_mutex);
            player->ffmpeg_ctx->pause_request = 0;
            LV_LOG_INFO("ffmpeg player resume");
            break;
        default:
            LV_LOG_ERROR("Error cmd: %d", cmd);
            break;
    }
}

void lv_ffmpeg_player_set_auto_restart(lv_obj_t * obj, bool en)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)obj;
    player->auto_restart = en;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static lv_res_t decoder_info(lv_img_decoder_t * decoder, const void * src, lv_img_header_t * header)
{
    /* Get the source type */
    lv_img_src_t src_type = lv_img_src_get_type(src);

    if(src_type == LV_IMG_SRC_FILE) {
        const char * fn = src;

        if(ffmpeg_get_img_header(fn, header) < 0) {
            LV_LOG_ERROR("ffmpeg can't get image header");
            return LV_RES_INV;
        }

        return LV_RES_OK;
    }

    /* If didn't succeeded earlier then it's an error */
    return LV_RES_INV;
}

static lv_res_t decoder_open(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{
    if(dsc->src_type == LV_IMG_SRC_FILE) {
        const char * path = dsc->src;

        struct ffmpeg_context_s * ffmpeg_ctx = ffmpeg_open_file(path);

        if(ffmpeg_ctx == NULL) {
            return LV_RES_INV;
        }

        if(ffmpeg_image_allocate(ffmpeg_ctx) < 0) {
            LV_LOG_ERROR("ffmpeg image allocate failed");
            ffmpeg_close(ffmpeg_ctx);
            return LV_RES_INV;
        }


        ffmpeg_close_src_ctx(ffmpeg_ctx);
        uint8_t * img_data = ffmpeg_get_img_data(ffmpeg_ctx);

#if LV_COLOR_DEPTH != 32
        if(ffmpeg_ctx->has_alpha) {
            convert_color_depth(img_data, dsc->header.w * dsc->header.h);
        }
#endif

        dsc->user_data = ffmpeg_ctx;
        dsc->img_data = img_data;

        /* The image is fully decoded. Return with its pointer */
        return LV_RES_OK;
    }

    /* If not returned earlier then it failed */
    return LV_RES_INV;
}

static void decoder_close(lv_img_decoder_t * decoder, lv_img_decoder_dsc_t * dsc)
{
    struct ffmpeg_context_s * ffmpeg_ctx = dsc->user_data;
    ffmpeg_close(ffmpeg_ctx);
}

#if LV_COLOR_DEPTH != 32

static void convert_color_depth(uint8_t * img, uint32_t px_cnt)
{
    lv_color32_t * img_src_p = (lv_color32_t *)img;
    struct lv_img_pixel_color_s * img_dst_p = (struct lv_img_pixel_color_s *)img;

    for(uint32_t i = 0; i < px_cnt; i++) {
        lv_color32_t temp = *img_src_p;
        img_dst_p->c = lv_color_hex(temp.full);
        img_dst_p->alpha = temp.ch.alpha;

        img_src_p++;
        img_dst_p++;
    }
}

#endif

static uint8_t * ffmpeg_get_img_data(struct ffmpeg_context_s * ffmpeg_ctx)
{
    uint8_t * img_data = ffmpeg_ctx->video_dst_data[0];

    if(img_data == NULL) {
        LV_LOG_ERROR("ffmpeg video dst data is NULL");
    }

    return img_data;
}

static bool ffmpeg_pix_fmt_has_alpha(enum AVPixelFormat pix_fmt)
{
    const AVPixFmtDescriptor * desc = av_pix_fmt_desc_get(pix_fmt);

    if(desc == NULL) {
        return false;
    }

    if(pix_fmt == AV_PIX_FMT_PAL8) {
        return true;
    }

    return (desc->flags & AV_PIX_FMT_FLAG_ALPHA) ? true : false;
}

static bool ffmpeg_pix_fmt_is_yuv(enum AVPixelFormat pix_fmt)
{
    const AVPixFmtDescriptor * desc = av_pix_fmt_desc_get(pix_fmt);

    if(desc == NULL) {
        return false;
    }

    return !(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->nb_components >= 2;
}

static int ffmpeg_output_video_frame(struct ffmpeg_context_s * ffmpeg_ctx, AVFrame *frame)
{
    int ret = -1;

    int width = ffmpeg_ctx->video_dec_ctx->width;
    int height = ffmpeg_ctx->video_dec_ctx->height;
    //AVFrame * frame = ffmpeg_ctx->frame;

    if(frame->width != width
       || frame->height != height
       || frame->format != ffmpeg_ctx->video_dec_ctx->pix_fmt) {

        /* To handle this change, one could call av_image_alloc again and
         * decode the following frames into another rawvideo file.
         */
        LV_LOG_ERROR("Width, height and pixel format have to be "
                     "constant in a rawvideo file, but the width, height or "
                     "pixel format of the input video changed:\n"
                     "old: width = %d, height = %d, format = %s\n"
                     "new: width = %d, height = %d, format = %s\n",
                     width,
                     height,
                     av_get_pix_fmt_name(ffmpeg_ctx->video_dec_ctx->pix_fmt),
                     frame->width, frame->height,
                     av_get_pix_fmt_name(frame->format));
        goto failed;
    }

    LV_LOG_TRACE("video_frame coded_n:%d", frame->coded_picture_number);

    /* copy decoded frame to destination buffer:
     * this is required since rawvideo expects non aligned data
     */
    av_image_copy(ffmpeg_ctx->video_src_data, ffmpeg_ctx->video_src_linesize,
                  (const uint8_t **)(frame->data), frame->linesize,
                  ffmpeg_ctx->video_dec_ctx->pix_fmt, width, height);

    if(ffmpeg_ctx->sws_ctx == NULL) {
        int swsFlags = SWS_BILINEAR;

        if(ffmpeg_pix_fmt_is_yuv(ffmpeg_ctx->video_dec_ctx->pix_fmt)) {

            /* When the video width and height are not multiples of 8,
             * and there is no size change in the conversion,
             * a blurry screen will appear on the right side
             * This problem was discovered in 2012 and
             * continues to exist in version 4.1.3 in 2019
             * This problem can be avoided by increasing SWS_ACCURATE_RND
             */
            if((width & 0x7) || (height & 0x7)) {
                LV_LOG_WARN("The width(%d) and height(%d) the image "
                            "is not a multiple of 8, "
                            "the decoding speed may be reduced",
                            width, height);
                swsFlags |= SWS_ACCURATE_RND;
            }
        }

        ffmpeg_ctx->sws_ctx = sws_getContext(
                                  width, height, ffmpeg_ctx->video_dec_ctx->pix_fmt,
                                  width, height, ffmpeg_ctx->video_dst_pix_fmt,
                                  swsFlags,
                                  NULL, NULL, NULL);
    }

    if(!ffmpeg_ctx->has_alpha) {
        int lv_linesize = sizeof(lv_color_t) * width;
        int dst_linesize = ffmpeg_ctx->video_dst_linesize[0];
        if(dst_linesize != lv_linesize) {
            LV_LOG_WARN("ffmpeg linesize = %d, but lvgl image require %d",
                        dst_linesize,
                        lv_linesize);
            ffmpeg_ctx->video_dst_linesize[0] = lv_linesize;
        }
    }

    ret = sws_scale(
              ffmpeg_ctx->sws_ctx,
              (const uint8_t * const *)(ffmpeg_ctx->video_src_data),
              ffmpeg_ctx->video_src_linesize,
              0,
              height,
              ffmpeg_ctx->video_dst_data,
              ffmpeg_ctx->video_dst_linesize);

failed:
    return ret;
}


static int ffmpeg_open_codec_context(int * stream_idx,
                                     AVCodecContext ** dec_ctx, AVFormatContext * fmt_ctx,
                                     enum AVMediaType type)
{
    int ret;
    int stream_index;
    AVStream * st;
    AVCodec * dec = NULL;
    AVDictionary * opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if(ret < 0) {
        LV_LOG_ERROR("Could not find %s stream in input file",
                     av_get_media_type_string(type));
        return ret;
    }
    else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if(dec == NULL) {
            LV_LOG_ERROR("Failed to find %s codec",
                         av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if(*dec_ctx == NULL) {
            LV_LOG_ERROR("Failed to allocate the %s codec context",
                         av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            LV_LOG_ERROR(
                "Failed to copy %s codec parameters to decoder context",
                av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders */
        if((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
            LV_LOG_ERROR("Failed to open %s codec",
                         av_get_media_type_string(type));
            return ret;
        }

        *stream_idx = stream_index;
    }

    return 0;
}

static int ffmpeg_get_img_header(const char * filepath,
                                 lv_img_header_t * header)
{
    int ret = -1;

    AVFormatContext * fmt_ctx = NULL;
    AVCodecContext * video_dec_ctx = NULL;
    int video_stream_idx;

    /* open input file, and allocate format context */
    if(avformat_open_input(&fmt_ctx, filepath, NULL, NULL) < 0) {
        LV_LOG_ERROR("Could not open source file %s", filepath);
        goto failed;
    }

    /* retrieve stream information */
    if(avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        LV_LOG_ERROR("Could not find stream information");
        goto failed;
    }

    if(ffmpeg_open_codec_context(&video_stream_idx, &video_dec_ctx,
                                 fmt_ctx, AVMEDIA_TYPE_VIDEO)
       >= 0) {
        bool has_alpha = ffmpeg_pix_fmt_has_alpha(video_dec_ctx->pix_fmt);

        /* allocate image where the decoded image will be put */
        header->w = video_dec_ctx->width;
        header->h = video_dec_ctx->height;
        header->always_zero = 0;
        header->cf = (has_alpha ? LV_IMG_CF_TRUE_COLOR_ALPHA : LV_IMG_CF_TRUE_COLOR);

        ret = 0;
    }

failed:
    avcodec_free_context(&video_dec_ctx);
    avformat_close_input(&fmt_ctx);

    return ret;
}


// 初始化音频解码器和重采样器
static int open_audio_stream(struct ffmpeg_context_s * ctx) {
    int ret;
    // 1. 查找音频流
    ret = av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ret < 0) return -1;
    ctx->audio_stream_idx = ret;
    ctx->audio_stream = ctx->fmt_ctx->streams[ret];

    // 2. 打开解码器
    AVCodec *dec = avcodec_find_decoder(ctx->audio_stream->codecpar->codec_id);
    ctx->audio_dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx->audio_dec_ctx, ctx->audio_stream->codecpar);
    avcodec_open2(ctx->audio_dec_ctx, dec, NULL);

    // 3. 初始化重采样 (将任意格式转为 44.1k Stereo S16LE)
    ctx->swr_ctx = swr_alloc_set_opts(NULL,
                                      av_get_default_channel_layout(AUDIO_OUT_CHANNELS),
                                      AV_SAMPLE_FMT_S16, // 输出格式
                                      AUDIO_OUT_RATE,    // 输出采样率
                                      av_get_default_channel_layout(ctx->audio_dec_ctx->channels),
                                      ctx->audio_dec_ctx->sample_fmt,
                                      ctx->audio_dec_ctx->sample_rate,
                                      0, NULL);
    swr_init(ctx->swr_ctx);

    // 4. 初始化 ALSA
    alsa_init();
    
    // 获取视频的时间基准 (用于 PTS 计算)
    if (ctx->video_stream) {
        ctx->video_time_base = av_q2d(ctx->video_stream->time_base);
    }
    
    return 0;
}


struct ffmpeg_context_s * ffmpeg_open_file(const char * path)
{
    if(path == NULL || strlen(path) == 0) {
        LV_LOG_ERROR("file path is empty");
        return NULL;
    }

    struct ffmpeg_context_s * ffmpeg_ctx = calloc(1, sizeof(struct ffmpeg_context_s));

    if(ffmpeg_ctx == NULL) {
        LV_LOG_ERROR("ffmpeg_ctx malloc failed");
        goto failed;
    }

    /* open input file, and allocate format context */
    
    AVDictionary *opts = NULL;
    // 1. 允许不安全的文件路径 (解决 absolute path 问题)
    av_dict_set(&opts, "safe", "0", 0);
    // 2. 允许自动探测格式
    av_dict_set(&opts, "probesize", "102400", 0);
    // 3. 设置协议白名单，允许 concat 和 file 互相调用
    // 如果没有这行，FFmpeg 可能会拒绝打开播放列表
    av_dict_set(&opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto,concat,subfile", 0);

    if(avformat_open_input(&(ffmpeg_ctx->fmt_ctx), path, NULL, &opts) < 0) {
        LV_LOG_ERROR("Could not open source file %s", path);
        goto failed;
    }

    // 释放字典
    av_dict_free(&opts);

    /* retrieve stream information */

    if(avformat_find_stream_info(ffmpeg_ctx->fmt_ctx, NULL) < 0) {
        LV_LOG_ERROR("Could not find stream information");
        goto failed;
    }

    if(ffmpeg_open_codec_context(
           &(ffmpeg_ctx->video_stream_idx),
           &(ffmpeg_ctx->video_dec_ctx),
           ffmpeg_ctx->fmt_ctx, AVMEDIA_TYPE_VIDEO)
       >= 0) {
        ffmpeg_ctx->video_stream = ffmpeg_ctx->fmt_ctx->streams[ffmpeg_ctx->video_stream_idx];

        ffmpeg_ctx->has_alpha = ffmpeg_pix_fmt_has_alpha(ffmpeg_ctx->video_dec_ctx->pix_fmt);

        ffmpeg_ctx->video_dst_pix_fmt = (ffmpeg_ctx->has_alpha ? AV_PIX_FMT_BGRA : AV_PIX_FMT_TRUE_COLOR);

        open_audio_stream(ffmpeg_ctx);
    }

#if LV_FFMPEG_AV_DUMP_FORMAT != 0
    /* dump input information to stderr */
    av_dump_format(ffmpeg_ctx->fmt_ctx, 0, path, 0);
#endif

    if(ffmpeg_ctx->video_stream == NULL) {
        LV_LOG_ERROR("Could not find video stream in the input, aborting");
        goto failed;
    }

    return ffmpeg_ctx;

failed:
    ffmpeg_close(ffmpeg_ctx);
    return NULL;
}

static int ffmpeg_image_allocate(struct ffmpeg_context_s * ffmpeg_ctx)
{
    int ret;

    /* allocate image where the decoded image will be put */
    ret = av_image_alloc(
              ffmpeg_ctx->video_src_data,
              ffmpeg_ctx->video_src_linesize,
              ffmpeg_ctx->video_dec_ctx->width,
              ffmpeg_ctx->video_dec_ctx->height,
              ffmpeg_ctx->video_dec_ctx->pix_fmt,
              4);

    if(ret < 0) {
        LV_LOG_ERROR("Could not allocate src raw video buffer");
        return ret;
    }

    LV_LOG_INFO("alloc video_src_bufsize = %d", ret);

    ret = av_image_alloc(
              ffmpeg_ctx->video_dst_data,
              ffmpeg_ctx->video_dst_linesize,
              ffmpeg_ctx->video_dec_ctx->width,
              ffmpeg_ctx->video_dec_ctx->height,
              ffmpeg_ctx->video_dst_pix_fmt,
              4);

    if(ret < 0) {
        LV_LOG_ERROR("Could not allocate dst raw video buffer");
        return ret;
    }

    LV_LOG_INFO("allocate video_dst_bufsize = %d", ret);


    return 0;
}

static void ffmpeg_close_src_ctx(struct ffmpeg_context_s * ffmpeg_ctx)
{
    avcodec_free_context(&(ffmpeg_ctx->video_dec_ctx));
    avformat_close_input(&(ffmpeg_ctx->fmt_ctx));
    if(ffmpeg_ctx->video_src_data[0] != NULL) {
        av_free(ffmpeg_ctx->video_src_data[0]);
        ffmpeg_ctx->video_src_data[0] = NULL;
    }
}

static void ffmpeg_close_dst_ctx(struct ffmpeg_context_s * ffmpeg_ctx)
{
    if(ffmpeg_ctx->video_dst_data[0] != NULL) {
        av_free(ffmpeg_ctx->video_dst_data[0]);
        ffmpeg_ctx->video_dst_data[0] = NULL;
    }
}

static void ffmpeg_close(struct ffmpeg_context_s * ffmpeg_ctx)
{
    if(ffmpeg_ctx == NULL) {
        LV_LOG_WARN("ffmpeg_ctx is NULL");
        return;
    }

    sws_freeContext(ffmpeg_ctx->sws_ctx);
    ffmpeg_close_src_ctx(ffmpeg_ctx);
    ffmpeg_close_dst_ctx(ffmpeg_ctx);
    free(ffmpeg_ctx);

    LV_LOG_INFO("ffmpeg_ctx closed");
}


static void lv_ffmpeg_player_constructor(const lv_obj_class_t * class_p,
                                         lv_obj_t * obj)
{
    LV_TRACE_OBJ_CREATE("begin");

    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)obj;

    player->auto_restart = false;
    player->ffmpeg_ctx = NULL;

    LV_TRACE_OBJ_CREATE("finished");
}

static void lv_ffmpeg_player_destructor(const lv_obj_class_t * class_p,
                                        lv_obj_t * obj)
{
    LV_TRACE_OBJ_CREATE("begin");

    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)obj;

    if(player->timer) {
        lv_timer_del(player->timer);
        player->timer = NULL;
    }

    lv_img_cache_invalidate_src(lv_img_get_src(obj));

    ffmpeg_close(player->ffmpeg_ctx);
    player->ffmpeg_ctx = NULL;

    LV_TRACE_OBJ_CREATE("finished");
}

#endif /*LV_USE_FFMPEG*/
