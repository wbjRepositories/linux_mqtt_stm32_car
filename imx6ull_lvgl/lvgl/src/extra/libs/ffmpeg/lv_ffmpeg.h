/**
 * @file lv_ffmpeg.h
 *
 */
#ifndef LV_FFMPEG_H
#define LV_FFMPEG_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "../../../lvgl.h"
#include <libavformat/avformat.h>
#if LV_USE_FFMPEG != 0

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
struct ffmpeg_context_s;

extern const lv_obj_class_t lv_ffmpeg_player_class;

typedef struct {
    lv_img_t img;
    lv_timer_t * timer;
    lv_img_dsc_t imgdsc;
    bool auto_restart;
    struct ffmpeg_context_s * ffmpeg_ctx;
} lv_ffmpeg_player_t;

typedef enum {
    LV_FFMPEG_PLAYER_CMD_START,
    LV_FFMPEG_PLAYER_CMD_STOP,
    LV_FFMPEG_PLAYER_CMD_PAUSE,
    LV_FFMPEG_PLAYER_CMD_RESUME,
    _LV_FFMPEG_PLAYER_CMD_LAST
} lv_ffmpeg_player_cmd_t;

// 包队列
typedef struct {
    AVPacketList *first, *last;
    int nb_packets;
    int size;
    pthread_mutex_t mutex; // 互斥锁保护队列
    pthread_cond_t cond;   // 条件变量（用于唤醒）
    int abort_request;
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
    int demux_pause_request;                    // 解码暂停标志
    int playback_end;                           // 播放结束标志
    float playback_speed;       //播放速度，几倍速
};

void set_demux_pause(lv_obj_t * arg, int flag);
void set_playback_speed(lv_obj_t * arg, float speed);
int get_playback_end(lv_obj_t * arg);
/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Register FFMPEG image decoder
 */
void lv_ffmpeg_init(void);

/**
 * Get the number of frames contained in the file
 * @param path image or video file name
 * @return Number of frames, less than 0 means failed
 */
int lv_ffmpeg_get_frame_num(const char * path);

/**
 * Create ffmpeg_player object
 * @param parent pointer to an object, it will be the parent of the new player
 * @return pointer to the created ffmpeg_player
 */
lv_obj_t * lv_ffmpeg_player_create(lv_obj_t * parent);

/**
 * Set the path of the file to be played
 * @param obj pointer to a ffmpeg_player object
 * @param path video file path
 * @return LV_RES_OK: no error; LV_RES_INV: can't get the info.
 */
lv_res_t lv_ffmpeg_player_set_src(lv_obj_t * obj, const char * path);

/**
 * Set command control video player
 * @param obj pointer to a ffmpeg_player object
 * @param cmd control commands
 */
void lv_ffmpeg_player_set_cmd(lv_obj_t * obj, lv_ffmpeg_player_cmd_t cmd);

/**
 * Set the video to automatically replay
 * @param obj pointer to a ffmpeg_player object
 * @param en true: enable the auto restart
 */
void lv_ffmpeg_player_set_auto_restart(lv_obj_t * obj, bool en);

void lv_ffmpeg_player_get_total_time(lv_obj_t * arg, char* buf);
void lv_ffmpeg_player_get_current_time(lv_obj_t * arg, char* buf);
void lv_ffmpeg_player_seek(lv_obj_t * arg, char* buf);

lv_res_t lv_ffmpeg_player_reset_src(lv_obj_t * obj, const char * path);

/*=====================
 * Other functions
 *====================*/

/**********************
 *      MACROS
 **********************/

#endif /*LV_USE_FFMPEG*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_FFMPEG_H*/
