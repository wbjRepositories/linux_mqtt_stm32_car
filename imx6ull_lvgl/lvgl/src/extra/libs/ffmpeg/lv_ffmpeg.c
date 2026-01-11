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

//#define FRAME_DEF_REFR_PERIOD   33  /*[ms]*/

// 【修改】定时器频率改为更快的固定频率，以便精细控制同步
// 不要再用视频帧率，建议 10ms 或 5ms
#undef FRAME_DEF_REFR_PERIOD
#define FRAME_DEF_REFR_PERIOD 5 

// 音频输出参数配置 (PCM)
#define AUDIO_OUT_RATE 44100
#define AUDIO_OUT_CHANNELS 2
#define AUDIO_OUT_FORMAT SND_PCM_FORMAT_S16_LE


/**********************
 *      TYPEDEFS
 **********************/
struct ffmpeg_context_s {
    AVFormatContext * fmt_ctx;
    AVCodecContext * video_dec_ctx;
    AVStream * video_stream;
    uint8_t * video_src_data[4];
    uint8_t * video_dst_data[4];
    struct SwsContext * sws_ctx;
    AVFrame * frame;
    AVPacket pkt;
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
    uint8_t * audio_out_buffer;  // 音频输出缓冲
    int audio_out_buffer_size;
    // 新增成员同步相关
    double video_pts;      // 当前解码出来的视频帧的显示时间 (秒)
    double audio_clock;    // 当前音频播放到的时间 (秒)
    double video_time_base; // 视频流的时间基准
};

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
static int ffmpeg_get_frame_refr_period(struct ffmpeg_context_s * ffmpeg_ctx);
static uint8_t * ffmpeg_get_img_data(struct ffmpeg_context_s * ffmpeg_ctx);
static int ffmpeg_update_next_frame(struct ffmpeg_context_s * ffmpeg_ctx);
static int ffmpeg_output_video_frame(struct ffmpeg_context_s * ffmpeg_ctx);
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

void lv_ffmpeg_init(void)
{
    lv_img_decoder_t * dec = lv_img_decoder_create();
    lv_img_decoder_set_info_cb(dec, decoder_info);
    lv_img_decoder_set_open_cb(dec, decoder_open);
    lv_img_decoder_set_close_cb(dec, decoder_close);

#if LV_FFMPEG_AV_DUMP_FORMAT == 0
    av_log_set_level(AV_LOG_QUIET);
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

snd_pcm_t *pcm_handle = NULL;
snd_pcm_hw_params_t *hwparams = NULL;
/**
 * 
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
    snd_pcm_hw_params_set_format(pcm_handle, hwparams,SND_PCM_FORMAT_S16_LE);
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

    lv_timer_pause(player->timer);

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

    // int period = ffmpeg_get_frame_refr_period(player->ffmpeg_ctx);

    // if(period > 0) {
    //     LV_LOG_INFO("frame refresh period = %d ms, rate = %d fps",
    //                 period, 1000 / period);
    //     lv_timer_set_period(player->timer, period);
    // }
    // else {
    //     LV_LOG_WARN("unable to get frame refresh period");
    // }
    // 强制设为一个较小的检查周期
    lv_timer_set_period(player->timer, 10); // 10ms 检查一次

    res = LV_RES_OK;

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

    lv_timer_t * timer = player->timer;

    switch(cmd) {
        case LV_FFMPEG_PLAYER_CMD_START:
            av_seek_frame(player->ffmpeg_ctx->fmt_ctx,
                          0, 0, AVSEEK_FLAG_BACKWARD);
            lv_timer_resume(timer);
            LV_LOG_INFO("ffmpeg player start");
            break;
        case LV_FFMPEG_PLAYER_CMD_STOP:
            av_seek_frame(player->ffmpeg_ctx->fmt_ctx,
                          0, 0, AVSEEK_FLAG_BACKWARD);
            lv_timer_pause(timer);
            LV_LOG_INFO("ffmpeg player stop");
            break;
        case LV_FFMPEG_PLAYER_CMD_PAUSE:
            lv_timer_pause(timer);
            LV_LOG_INFO("ffmpeg player pause");
            break;
        case LV_FFMPEG_PLAYER_CMD_RESUME:
            lv_timer_resume(timer);
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

        if(ffmpeg_update_next_frame(ffmpeg_ctx) < 0) {
            ffmpeg_close(ffmpeg_ctx);
            LV_LOG_ERROR("ffmpeg update frame failed");
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

static int ffmpeg_output_video_frame(struct ffmpeg_context_s * ffmpeg_ctx)
{
    int ret = -1;

    int width = ffmpeg_ctx->video_dec_ctx->width;
    int height = ffmpeg_ctx->video_dec_ctx->height;
    AVFrame * frame = ffmpeg_ctx->frame;

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

static int ffmpeg_decode_packet(AVCodecContext * dec, const AVPacket * pkt,
                                struct ffmpeg_context_s * ffmpeg_ctx)
{
    int ret = 0;

    /* submit the packet to the decoder */
    ret = avcodec_send_packet(dec, pkt);
    if(ret < 0) {
        LV_LOG_ERROR("Error submitting a packet for decoding (%s)",
                     av_err2str(ret));
        return ret;
    }

    /* get all the available frames from the decoder */
    while(ret >= 0) {
        ret = avcodec_receive_frame(dec, ffmpeg_ctx->frame);
        if(ret < 0) {

            /* those two return values are special and mean there is
             * no output frame available,
             * but there were no errors during decoding
             */
            if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                return 0;
            }

            LV_LOG_ERROR("Error during decoding (%s)", av_err2str(ret));
            return ret;
        }

        /* write the frame data to output file */
        if(dec->codec->type == AVMEDIA_TYPE_VIDEO) {
            ret = ffmpeg_output_video_frame(ffmpeg_ctx);
        }

        av_frame_unref(ffmpeg_ctx->frame);
        if(ret < 0) {
            LV_LOG_WARN("ffmpeg_decode_packet ended %d", ret);
            return ret;
        }
    }

    return 0;
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

static int ffmpeg_get_frame_refr_period(struct ffmpeg_context_s * ffmpeg_ctx)
{
    int avg_frame_rate_num = ffmpeg_ctx->video_stream->avg_frame_rate.num;
    if(avg_frame_rate_num > 0) {
        int period = 1000 * (int64_t)ffmpeg_ctx->video_stream->avg_frame_rate.den
                     / avg_frame_rate_num;
        return period;
    }

    return -1;
}

// 初始化音频解码器和重采样器
static int open_audio_stream(struct ffmpeg_context_s * ctx) {
    int ret;
    // 1. 查找音频流
    ret = av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ret < 0) return -1;
    ctx->audio_stream_idx = ret;
    ctx->audio_stream = ctx->fmt_ctx->streams[ret];

    // 2. 打开解码器 (参考原有 video 类似的逻辑)
    AVCodec *dec = avcodec_find_decoder(ctx->audio_stream->codecpar->codec_id);
    ctx->audio_dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx->audio_dec_ctx, ctx->audio_stream->codecpar);
    avcodec_open2(ctx->audio_dec_ctx, dec, NULL);

    // 3. 初始化重采样 (将任意格式转为 44.1k Stereo S16LE)
    // 注意：FFmpeg 新版本 API 可能有所不同 (swr_alloc_set_opts2)，这里用旧版示例
    ctx->swr_ctx = swr_alloc_set_opts(NULL,
                                      av_get_default_channel_layout(AUDIO_OUT_CHANNELS),
                                      AV_SAMPLE_FMT_S16, // 输出格式
                                      AUDIO_OUT_RATE,    // 输出采样率
                                      av_get_default_channel_layout(ctx->audio_dec_ctx->channels),
                                      ctx->audio_dec_ctx->sample_fmt,
                                      ctx->audio_dec_ctx->sample_rate,
                                      0, NULL);
    swr_init(ctx->swr_ctx);

    // 4. 初始化 ALSA (使用你代码中定义的全局变量 pcm_handle)
    if (pcm_handle) { // 假设外部已经初始化了，或者在这里初始化
        // snd_pcm_open... snd_pcm_set_params... 
        // 实际项目中建议在这里配置 ALSA 参数匹配 AUDIO_OUT_RATE
        
    }
    alsa_init();
    
    // 获取视频的时间基准 (用于 PTS 计算)
    if (ctx->video_stream) {
        ctx->video_time_base = av_q2d(ctx->video_stream->time_base);
    }
    
    return 0;
}

static int ffmpeg_update_next_frame(struct ffmpeg_context_s * ctx)
{
    int ret = 0;
    while(1) {
        ret = av_read_frame(ctx->fmt_ctx, &ctx->pkt);
        if(ret < 0) return -1; // 文件结束或错误

        // --- 情况 A: 读到视频包 ---
        if (ctx->pkt.stream_index == ctx->video_stream_idx) {
            // 发送包到解码器
            ret = avcodec_send_packet(ctx->video_dec_ctx, &ctx->pkt);
            if (ret < 0) { av_packet_unref(&ctx->pkt); continue; }

            // 接收解码后的帧
            ret = avcodec_receive_frame(ctx->video_dec_ctx, ctx->frame);
            if (ret == 0) {
                // 【关键】记录当前视频帧的 PTS (换算成秒)
                // 如果 frame->best_effort_timestamp 无效，尝试 frame->pts
                int64_t pts = ctx->frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) pts = 0;
                
                ctx->video_pts = pts * ctx->video_time_base;

                // 转换图像格式给 LVGL (原有逻辑)
                ffmpeg_output_video_frame(ctx);
                
                av_packet_unref(&ctx->pkt);
                return 0; // 成功获取一帧视频，返回给上层去显示
            }
        }
        // --- 情况 B: 读到音频包 ---
        else if (ctx->pkt.stream_index == ctx->audio_stream_idx) {
            ret = avcodec_send_packet(ctx->audio_dec_ctx, &ctx->pkt);
            if (ret == 0) {
                while (avcodec_receive_frame(ctx->audio_dec_ctx, ctx->frame) >= 0) {
                    // 1. 重采样音频数据
                    // 计算输出样本数
                    int out_samples = av_rescale_rnd(swr_get_delay(ctx->swr_ctx, ctx->audio_dec_ctx->sample_rate) +
                                        ctx->frame->nb_samples, AUDIO_OUT_RATE, ctx->audio_dec_ctx->sample_rate, AV_ROUND_UP);
                    
                    // 确保缓冲区够大 (简化处理)
                    uint8_t *buffer = malloc(out_samples * AUDIO_OUT_CHANNELS * 2); 

                    int real_samples = swr_convert(ctx->swr_ctx, &buffer, out_samples, 
                                                   (const uint8_t **)ctx->frame->data, ctx->frame->nb_samples);
                    // 2. 写入 ALSA (播放声音)
                    if (pcm_handle && real_samples > 0) {
                        // 注意：这里是阻塞写入，可能会稍微影响 UI 流畅度，但在简单实现中是必要的
                        snd_pcm_sframes_t frames_written = snd_pcm_writei(pcm_handle, buffer, real_samples);
                        if (frames_written < 0) {
                            frames_written = snd_pcm_recover(pcm_handle, frames_written, 0);
                            if (frames_written < 0)
                            {
                                fprintf(stderr, "The error cannot be recovered: %s\n", snd_strerror(frames_written));
                                break;
                            }
                        }
                        // 3. 【关键】更新音频时钟
                        // 音频时钟 = 当前音频包 PTS。
                        // 更精确的做法是：音频PTS + (已写入声卡但在缓冲区的延迟)
                        // 简单做法：直接用音频包的时间戳                        
                        double audio_pts = ctx->frame->pts * av_q2d(ctx->audio_stream->time_base);
                        ctx->audio_clock = audio_pts;
                    }
                    free(buffer);
                }
            }
        }
        av_packet_unref(&ctx->pkt);
        // 继续循环，直到找到视频包或文件结束
    }
}

// static int ffmpeg_update_next_frame(struct ffmpeg_context_s * ffmpeg_ctx)
// {
//     int ret = 0;

//     while(1) {

//         /* read frames from the file */
//         if(av_read_frame(ffmpeg_ctx->fmt_ctx, &(ffmpeg_ctx->pkt)) >= 0) {
//             bool is_image = false;

//             /* check if the packet belongs to a stream we are interested in,
//              * otherwise skip it
//              */
//             if(ffmpeg_ctx->pkt.stream_index == ffmpeg_ctx->video_stream_idx) {
//                 ret = ffmpeg_decode_packet(ffmpeg_ctx->video_dec_ctx,
//                                            &(ffmpeg_ctx->pkt), ffmpeg_ctx);
//                 is_image = true;
//             }

//             av_packet_unref(&(ffmpeg_ctx->pkt));

//             if(ret < 0) {
//                 LV_LOG_WARN("video frame is empty %d", ret);
//                 break;
//             }

//             /* Used to filter data that is not an image */
//             if(is_image) {
//                 break;
//             }
//         }
//         else {
//             ret = -1;
//             break;
//         }
//     }

//     return ret;
// }

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
    // 3. 【最关键】设置协议白名单，允许 concat 和 file 互相调用
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

    ffmpeg_ctx->frame = av_frame_alloc();

    if(ffmpeg_ctx->frame == NULL) {
        LV_LOG_ERROR("Could not allocate frame");
        return -1;
    }

    /* initialize packet, set data to NULL, let the demuxer fill it */
    av_init_packet(&ffmpeg_ctx->pkt);
    ffmpeg_ctx->pkt.data = NULL;
    ffmpeg_ctx->pkt.size = 0;

    return 0;
}

static void ffmpeg_close_src_ctx(struct ffmpeg_context_s * ffmpeg_ctx)
{
    avcodec_free_context(&(ffmpeg_ctx->video_dec_ctx));
    avformat_close_input(&(ffmpeg_ctx->fmt_ctx));
    av_frame_free(&(ffmpeg_ctx->frame));
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

static void lv_ffmpeg_player_frame_update_cb(lv_timer_t * timer)
{
    lv_obj_t * obj = (lv_obj_t *)timer->user_data;
    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)obj;
    struct ffmpeg_context_s * ctx = player->ffmpeg_ctx;

    if(!ctx) return;

    // --- 音视频同步逻辑 ---
    double diff = ctx->video_pts - ctx->audio_clock;
    double sync_threshold = 0.03; // 30ms 的同步阈值

    if (diff > sync_threshold) {
        // [情况1]：视频比音频快 (Video PTS > Audio Clock)
        // 动作：等待。不解码下一帧，也不刷新 UI，直接返回。
        // 因为定时器是 10ms 一次，稍后会再进来检查。
        return; 
    }
    
    if (diff < -sync_threshold) {
        // [情况2]：视频比音频慢 (Video PTS < Audio Clock)
        // 动作：丢帧 (Drop Frame)。
        // 连续解码，直到追上音频
        // 这里做一个简单的丢帧循环（限制次数防止卡死）
        int drop_count = 0;
        while (diff < -sync_threshold && drop_count < 5) {
             LV_LOG_WARN("Skipping frame (Video Lag: %.3f)", diff);
             if (ffmpeg_update_next_frame(ctx) < 0) break; // 解码下一帧
             diff = ctx->video_pts - ctx->audio_clock;     // 重新计算差异
             drop_count++;
        }
    }

    // [情况3]：时间刚好 (或追赶上了) -> 渲染显示
    
    // 刷新 UI 显示当前这一帧
#if LV_COLOR_DEPTH != 32
    if(ctx->has_alpha) {
        convert_color_depth((uint8_t *)(player->imgdsc.data),
                            player->imgdsc.header.w * player->imgdsc.header.h);
    }
#endif
    // 准备下一帧数据 (解码放到缓冲区，等待下一次定时器来判断时间)
    // 这一步很重要：我们显示完当前帧后，立刻去解下一帧拿到它的 PTS，
    // 这样下次定时器进来时才有数据做比较。
    int has_next = ffmpeg_update_next_frame(ctx);
    if(has_next < 0) {
        lv_ffmpeg_player_set_cmd(obj, player->auto_restart ? LV_FFMPEG_PLAYER_CMD_START : LV_FFMPEG_PLAYER_CMD_STOP);
    }

    lv_img_cache_invalidate_src(lv_img_get_src(obj));
    lv_obj_invalidate(obj);
}

// static void lv_ffmpeg_player_frame_update_cb(lv_timer_t * timer)
// {
//     lv_obj_t * obj = (lv_obj_t *)timer->user_data;
//     lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)obj;

//     if(!player->ffmpeg_ctx) {
//         return;
//     }

//     int has_next = ffmpeg_update_next_frame(player->ffmpeg_ctx);

//     if(has_next < 0) {
//         lv_ffmpeg_player_set_cmd(obj, player->auto_restart ? LV_FFMPEG_PLAYER_CMD_START : LV_FFMPEG_PLAYER_CMD_STOP);
//         return;
//     }

// #if LV_COLOR_DEPTH != 32
//     if(player->ffmpeg_ctx->has_alpha) {
//         convert_color_depth((uint8_t *)(player->imgdsc.data),
//                             player->imgdsc.header.w * player->imgdsc.header.h);
//     }
// #endif

//     lv_img_cache_invalidate_src(lv_img_get_src(obj));
//     lv_obj_invalidate(obj);
// }

static void lv_ffmpeg_player_constructor(const lv_obj_class_t * class_p,
                                         lv_obj_t * obj)
{
    LV_TRACE_OBJ_CREATE("begin");

    lv_ffmpeg_player_t * player = (lv_ffmpeg_player_t *)obj;

    player->auto_restart = false;
    player->ffmpeg_ctx = NULL;
    player->timer = lv_timer_create(lv_ffmpeg_player_frame_update_cb,
                                    FRAME_DEF_REFR_PERIOD, obj);
    lv_timer_pause(player->timer);

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
