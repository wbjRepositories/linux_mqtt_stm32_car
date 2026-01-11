#define _DEFAULT_SOURCE

#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <linux/input.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include "common.h"
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>

#include "../lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
#include "lvgl/src/extra/libs/ffmpeg/lv_ffmpeg.h"

#define MAX_FINGERS 2      		//支持的最大手指数量
#define LV_HOR_RES_MAX 1024		//屏幕水平像素数量
#define LV_VER_RES_MAX 600		//屏幕垂直像素数量
#define SHM_NAME "/lvgl_mqtt_shm"			//mqtt共享内存名字
#define SHM_FRM_NAME     "/shm_frm"         //camera共享内存名字
#define SEM_SUB_NAME "/lvgl_mqtt_sem_sub"			//发布信号量名字
#define BUFF_SIZE LV_HOR_RES_MAX * LV_VER_RES_MAX		//buff字节数
#define CAR_MAX_SPEED   64     //默认最大行进速度
#define CAM_W  320//640      //摄像头像素宽
#define CAM_H  240//480      //摄像头像素高

#define VIDEO_DIR   "/mnt/sd"
#define MAX_FILES 1024       // 假设最大文件数，防止内存爆掉，可根据实际情况调整
#define PLAYLIST_PATH   "/mnt/sd/playlist.ffconcat"

typedef struct {
    int tracking_id;		//手指id
    int x;					//手指在屏幕的X轴位置
    int y;					//手指在屏幕的Y轴位置
    bool pressed;			//是否按下
} touch_point_t;			//触摸点状态

struct camera_frm_struct
{
    pthread_mutex_t camera_frm_mutex;
    pthread_cond_t camera_frm_cond;
    int new_frame_ready; // 0表示空闲，1表示数据已准备好 没有标志位会死锁
    uint32_t frm[];
};
struct camera_frm_struct *camera_frm;

static touch_point_t points[MAX_FINGERS];	//记录当前的多个触摸点状态
static int ts_fd = -1;				//屏幕事件句柄
static car_status_t *car_status;	//当前小车状态
static lv_obj_t *advance;			//前进按钮
static lv_obj_t *retreat;			//后退按钮
static lv_obj_t *turn_left;			//左转按钮
static lv_obj_t *turn_right;		//右转按钮
static lv_obj_t * speed_slider;     //行进速度滑动条
static lv_obj_t * speed_slider_label;       //行进速度滑动条标签
static lv_obj_t * dirct_speed_slider;       //转向速度滑动条
static lv_obj_t * dirct_speed_slider_label;       //转向速度滑动条标签
static lv_obj_t * page_main;             //主页面
static lv_obj_t * page_camera;           //摄像头页面
static lv_obj_t * page_player;          //播放器页面
static lv_obj_t * player;               //播放器
static lv_obj_t * video_time;           //视频时间
static lv_obj_t * current_time;           //视频当前时间
static lv_obj_t * video_progress;        //视频进度条
static sem_t *mqtt_sem_sub;			//用来同步mqtt消息进程的信号量
static int shm_fd;					//共享内存文件句柄
static lv_img_dsc_t cam_img_dsc;        //摄像头像素描述
static lv_obj_t *cam_img_obj;           //摄像头对象
static pid_t camera = -1;
static pid_t camera_p2p = -1;
static pid_t camera_cs = -1;

//杀死摄像头进程
void kill_camera(pid_t *pid)
{
    if(*pid > 0){
        if (kill(*pid, SIGINT) == 0) {
            printf("Camera terminated : %d\n", *pid);
            *pid = -1;
        } else {
            perror("kill failed");
        }
    }
}
void run_camera(char *s)
{
    char *argv[] = {"/root/camera", s, NULL};
    execvp("/root/camera", argv);
    perror("execvp camera err");
}

// qsort比较函数
int compare_filenames(const void *s1, const void *s2)
{
    return strcmp(*(const char **)s1, *(const char **)s2);
}

// 生成播放列表
int generate_playlist(void)
{
    DIR *d = NULL;
    struct dirent *dir = NULL;
    char **file_list = NULL;
    int count = 0;
    int ret = 0;
    d = opendir(VIDEO_DIR);
    if (!d)
    {
        perror("无法打开目录："VIDEO_DIR);
        return -1;
    }
    file_list = (char **)malloc(MAX_FILES * sizeof(char *));
    if (file_list == NULL)
    {
        perror("内存分配失败");
        closedir(d);
        return -1;
    }
    
    while ((dir = readdir(d)) != NULL)
    {
        if (dir->d_type != DT_REG && dir->d_type != DT_UNKNOWN)
        {
            continue;
        }
        // printf("strstr:%s\n",strstr(dir->d_name, "video"));
        // printf("strcmp:%d\n",strcmp(strrchr(dir->d_name, '.'), ".mov"));
        if (strstr(dir->d_name, "video") != NULL && strcmp(strrchr(dir->d_name, '.'), ".mov") == 0)
        {
            if (count < MAX_FILES)
            {
                file_list[count] = strdup(dir->d_name);
                if (file_list[count])
                {
                    count++;
                } else 
                {
                    printf("警告：视频文件数量超过最大限制 %d，部分文件将被忽略\n", MAX_FILES);
                    break;
                }
            }
        }
    }
    closedir(d);
    if (count == 0)
    {
        printf("未在 %s 找到任何.mov文件\n", VIDEO_DIR);
    }
    qsort(file_list, count, sizeof(char *), compare_filenames);

    FILE *f = fopen(PLAYLIST_PATH, "w");
    if (f == NULL)
    {
        perror("无法创建 playlist.ffconcat");
        ret = -1;
    }
    else
    {
        fprintf(f, "ffconcat version 1.0\n");
        for (int i = 0; i < count; i++) 
        {
            fprintf(f, "file '%s/%s'\n", VIDEO_DIR, file_list[i]);
            //fprintf(f, "file '%s'\n", file_list[i]);
        }
        printf("成功生成播放列表: %s，包含 %d 个片段\n", PLAYLIST_PATH, count);
        fclose(f);
    }
    
    for (int i = 0; i < count; i++)
    {
        free(file_list[i]);
    }
    free(file_list);

    return ret;
}

/*
*	检测屏幕所有触摸事件并记录位置信息
*/
void * touch_thread(void *arg)
{
    int current_slot = 0;		//标记当前事件组是哪个手指
    struct input_event ev;		//存放事件

    while(1) {
        while (read(ts_fd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EV_ABS) {
                switch (ev.code) {
                case ABS_MT_SLOT:
                    current_slot = ev.value;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (ev.value < 0)
                        points[current_slot].pressed = false;
                    else {
                        points[current_slot].pressed = true;
                        points[current_slot].tracking_id = ev.value;
                    }
                    break;
                case ABS_MT_POSITION_X:
                    points[current_slot].x = ev.value;
                    break;
                case ABS_MT_POSITION_Y:
                    points[current_slot].y = ev.value;
                    break;
                }
            }
        }
        usleep(1000); // 小睡 1ms，防止线程占用率太高
    }
    return NULL;
}



/*
*	触摸屏幕回调函数，将每个手指的坐标数据传给lvgl
*/
void my_touch_read_cb(lv_indev_drv_t * drv, lv_indev_data_t * data)
{
    int slot = (int)drv->user_data;
    if (points[slot].pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = points[slot].x;
        data->point.y = points[slot].y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/*
*	初始化触摸输入设备
*/
void lv_indev_init(void)
{
    // 打开触摸设备
    ts_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if(ts_fd < 0) {
        perror("open event");
        return;
    }

    // 启动触摸解析线程
    pthread_t pid;
    pthread_create(&pid, NULL, touch_thread, NULL);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touch_read_cb;
    indev_drv.user_data = (void *)(intptr_t)0;
    lv_indev_drv_register(&indev_drv);
    printf("LVGL indev registered for slot %d\n", 0);

    static lv_indev_drv_t indev_drv1;
    lv_indev_drv_init(&indev_drv1);
    indev_drv1.type = LV_INDEV_TYPE_POINTER;
    indev_drv1.read_cb = my_touch_read_cb;
    indev_drv1.user_data = (void *)(intptr_t)1;
    lv_indev_drv_register(&indev_drv1);
    printf("LVGL indev registered for slot %d\n", 1);
}

/*
*	按键回调函数
*/
static void btn_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED) {
        lv_obj_t *target = lv_event_get_target(e);
        if(target == advance)
        {
            car_status->current_speed = car_status->speed_max;
        }
        if (target == retreat)
        {
            car_status->current_speed = -car_status->speed_max;
        }
        if (target == turn_left)
        {
            car_status->direction = car_status->dirct_speed;
        }
        if (target == turn_right)
        {
            car_status->direction = -car_status->dirct_speed;
        }
        sem_post(mqtt_sem_sub);
    }
    else if(code == LV_EVENT_RELEASED)
    {
        lv_obj_t *target = lv_event_get_target(e);
        if(target == advance)
        {
            car_status->current_speed = 0;
        }
        if (target == retreat)
        {
            car_status->current_speed = 0;
        }
        if (target == turn_left)
        {
            car_status->direction = 0;
        }
        if (target == turn_right)
        {
            car_status->direction = 0;
        }
        sem_post(mqtt_sem_sub);
    }
}

/*
*	三角形绘制函数回调
*/
static void triangle_draw_cb(lv_event_t * e)
{
    lv_obj_t * obj = lv_event_get_target(e);
    lv_draw_ctx_t * draw_ctx = lv_event_get_draw_ctx(e);

    lv_coord_t w = lv_obj_get_width(obj);
    lv_coord_t h = lv_obj_get_height(obj);
    
    lv_area_t * coords = &obj->coords;
    lv_point_t A, B, C;
    if(obj == advance)
    {
        A.x = coords->x1 + w / 2;
        A.y = coords->y1;          // 顶部 y1
        
        B.x = coords->x1;          // 左侧 x1
        B.y = coords->y2;          // 底部 y2
        
        C.x = coords->x2;          // 右侧 x2
        C.y = coords->y2;          // 底部 y2
    }
    else if(obj == retreat)
    {
        A.x = coords->x1;
        A.y = coords->y1;          
        
        B.x = coords->x2;          
        B.y = coords->y1;          
        
        C.x = coords->x1 + w / 2;          
        C.y = coords->y2;          
    }
    else if(obj == turn_left)
    {
        A.x = coords->x1;
        A.y = coords->y1 + h / 2;          
        
        B.x = coords->x2;          
        B.y = coords->y1;          
        
        C.x = coords->x2;          
        C.y = coords->y2;          
    }
    else if(obj == turn_right)
    {
        A.x = coords->x1;
        A.y = coords->y1;          
        
        B.x = coords->x1;          
        B.y = coords->y2;          
        
        C.x = coords->x2;          
        C.y = coords->y1 + h / 2;          
    }
    lv_point_t tri[3] = {A,B,C};
    lv_draw_rect_dsc_t draw_dsc;
    lv_draw_rect_dsc_init(&draw_dsc);
    draw_dsc.bg_color = lv_color_hex(0xFF0000); // 设置颜色
    draw_dsc.bg_opa = LV_OPA_COVER;
    draw_dsc.bg_color = lv_palette_main(LV_PALETTE_BLUE);
    lv_draw_polygon(draw_ctx, &draw_dsc, tri, 3);
}

// 作用：计算向量叉乘，用来判断方向
static int32_t my_cross_product(const lv_point_t * p, lv_point_t * p1, lv_point_t * p2) {
    return (int32_t)(p->x - p2->x) * (p1->y - p2->y) - 
           (int32_t)(p1->x - p2->x) * (p->y - p2->y);
}

// 作用：利用上面的数学计算，判断点 p 是否在三角形 v1-v2-v3 里面
bool is_point_in_triangle(const lv_point_t * p, lv_point_t * v1, lv_point_t * v2, lv_point_t * v3) {
    // 调用上面的辅助函数计算三次
    int32_t d1 = my_cross_product(p, v1, v2);
    int32_t d2 = my_cross_product(p, v2, v3);
    int32_t d3 = my_cross_product(p, v3, v1);

    if(((d1 > 0) && (d2 > 0) && (d3 > 0)) || ((d1 < 0) && (d2 < 0) && (d3 < 0)))
    {
        return true;
    }
    else
    {
        return false;
    }
}

/*
*	三角形触摸函数回调
*/
static void triangle_hit_cb(lv_event_t * e) {
    lv_obj_t * obj = lv_event_get_target(e);
    lv_hit_test_info_t * info = lv_event_get_param(e);

    // 1. 获取当前触摸点坐标 (绝对坐标)
    const lv_point_t * touch_p = info->point;

    // 2. 获取按钮当前的绝对坐标范围
    // obj->coords 包含了 x1, y1 (左上角) 和 x2, y2 (右下角)
    lv_area_t * coords = &obj->coords;
    
    lv_coord_t w = lv_obj_get_width(obj);
    lv_coord_t h = lv_obj_get_height(obj);

    // A、B、C：三角形三个顶点
    lv_point_t A, B, C;

    if(obj == advance)
    {
        A.x = coords->x1 + w / 2;
        A.y = coords->y1;          // 顶部 y1
        
        B.x = coords->x1;          // 左侧 x1
        B.y = coords->y2;          // 底部 y2
        
        C.x = coords->x2;          // 右侧 x2
        C.y = coords->y2;          // 底部 y2
    }
    else if(obj == retreat)
    {
        A.x = coords->x1;
        A.y = coords->y1;          
        
        B.x = coords->x2;          
        B.y = coords->y1;          
        
        C.x = coords->x1 + w / 2;          
        C.y = coords->y2;          
    }
    else if(obj == turn_left)
    {
        A.x = coords->x1;
        A.y = coords->y1 + h / 2;          
        
        B.x = coords->x2;          
        B.y = coords->y1;          
        
        C.x = coords->x2;          
        C.y = coords->y2;          
    }
    else if(obj == turn_right)
    {
        A.x = coords->x1;
        A.y = coords->y1;          
        
        B.x = coords->x1;          
        B.y = coords->y2;          
        
        C.x = coords->x2;          
        C.y = coords->y1 + h / 2;          
    }

    info->res = is_point_in_triangle(touch_p, &A, &B, &C);
}

/**
 * 滑动条事件回调
 */
static void slider_event_cb(lv_event_t * e)
{
    lv_obj_t * slider = lv_event_get_target(e);
    char buf[8];
    int32_t slider_value = lv_slider_get_value(slider);
    if(slider == speed_slider)
    {
        lv_snprintf(buf, sizeof(buf), "%d%%", (int)slider_value);
        car_status->speed_max = 127 * slider_value / 100;
        lv_label_set_text(speed_slider_label, buf);
        lv_obj_align_to(speed_slider_label, slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    }
    else if(slider == dirct_speed_slider)
    {
        lv_snprintf(buf, sizeof(buf), "%d%%", (int)slider_value);
        car_status->dirct_speed = 127 * slider_value / 100;
        lv_label_set_text(dirct_speed_slider_label, buf);
        lv_obj_align_to(dirct_speed_slider_label, slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    }
    else if(slider == video_progress)
    {
        lv_snprintf(buf, sizeof(buf), "%d%%", (int)lv_slider_get_value(slider));
        lv_label_set_text(video_progress, buf);
        lv_obj_align_to(video_time, video_progress,LV_ALIGN_OUT_LEFT_MID, 0, 0);
    }
}

/**
 * 下拉框事件回调
 */
static void dropdown_set_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    if(code == LV_EVENT_VALUE_CHANGED) {
        char buf[32];
        lv_dropdown_get_selected_str(obj, buf, sizeof(buf));
        if(strcmp(buf, "camera") == 0)
        {
            if (camera <= 0)
                camera = fork();
            if(camera < 0)
            {
                perror("fork err");
            }
            else if(camera == 0)
            {
                //杀死其他摄像头进程
                kill_camera(&camera_p2p);
                kill_camera(&camera_cs);
                run_camera("camera");
            }
            else
            {
                //隐藏其他页面显示本页面
                lv_obj_add_flag(page_main, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(page_camera, LV_OBJ_FLAG_HIDDEN);
                //隐藏播放器并停止播放
                lv_obj_add_flag(page_player, LV_OBJ_FLAG_HIDDEN);
                lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_STOP);
                // lv_refr_now(NULL);
                // lv_obj_invalidate(lv_scr_act());
            }
        }
        else if (strcmp(buf, "camera(p2p)") == 0)
        {
            camera_p2p = fork();
            if(camera_p2p < 0)
            {
                perror("fork err");
            }
            else if(camera_p2p == 0)
            {
                //杀死其他摄像头进程
                kill_camera(&camera);
                kill_camera(&camera_cs);
                run_camera("camera_p2p");
                // char *argv[] = {"ffmpeg", "-f" ,"v4l2", "-input_format", "mjpeg", "-i", "/dev/video1", "-c:v", "copy" ,"-f" ,"mjpeg" ,"udp://192.168.1.10:5000", NULL};
                // execvp("ffmpeg", argv);
                // perror("execvp ffmpeg err");
            }
            else
            {
                //隐藏其他页面显示本页面
                lv_obj_clear_flag(page_main, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(page_camera, LV_OBJ_FLAG_HIDDEN);
                //隐藏播放器并停止播放
                lv_obj_add_flag(page_player, LV_OBJ_FLAG_HIDDEN);
                lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_STOP);
            }
        }
        else if (strcmp(buf, "camera(c/s)") == 0)
        {
            camera_cs = fork();
            if(camera_cs < 0)
            {
                perror("fork err");
            }
            else if(camera_cs == 0)
            {
                //杀死其他摄像头进程
                kill_camera(&camera);
                kill_camera(&camera_p2p);
                run_camera("camera(c/s)");
                // char *argv[] = {"ffmpeg", "-f" ,"v4l2", "-video_size", "640x480", "-framerate", "15", "-i", "/dev/video1", "-q", "10" ,"-f" ,"flv" ,"rtmp://192.168.1.19/live/myCamera", NULL};
                // execvp("ffmpeg", argv);
                // perror("execvp ffmpeg err");
            }
            else
            {
                //隐藏其他页面显示本页面
                lv_obj_clear_flag(page_main, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(page_camera, LV_OBJ_FLAG_HIDDEN);
                //隐藏播放器并停止播放
                lv_obj_add_flag(page_player, LV_OBJ_FLAG_HIDDEN);
                lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_STOP);
            }
        }
        else if(strcmp(buf, "controller") == 0)
        {
            //杀死其他摄像头进程
            kill_camera(&camera);
            kill_camera(&camera_p2p);
            kill_camera(&camera_cs);
            //隐藏其他页面显示本页面
            lv_obj_clear_flag(page_main, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(page_camera, LV_OBJ_FLAG_HIDDEN);
            //隐藏播放器并停止播放
            lv_obj_add_flag(page_player, LV_OBJ_FLAG_HIDDEN);
            lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_STOP);
        }
        else if(strcmp(buf, "playback") == 0)
        {
            //杀死其他摄像头进程
            kill_camera(&camera);
            kill_camera(&camera_p2p);
            kill_camera(&camera_cs);

            generate_playlist();

            lv_ffmpeg_player_set_src(player, PLAYLIST_PATH);

            lv_obj_add_flag(cam_img_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(page_player, LV_OBJ_FLAG_HIDDEN);
            lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_START);
            // lv_refr_now(NULL);
            // lv_obj_invalidate(lv_scr_act());
        }
    }
}


/*
*	为lvgl提供时间的线程
*/
static void *tick_thread(void *data)
{
    (void)data;
    while(1) {
        lv_tick_inc(5);
        usleep(5000);
    }
    return NULL;
}

/**
 * 
 */
void mqtt_recv_cb(struct _lv_timer_t *timer)
{
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%d%%", car_status->speed_max * 100 / 127);
    lv_slider_set_value(speed_slider, car_status->speed_max * 100 / 127, LV_ANIM_OFF);
    lv_label_set_text(speed_slider_label, buf);
    lv_obj_align_to(speed_slider_label, speed_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    lv_snprintf(buf, sizeof(buf), "%d%%", car_status->dirct_speed * 100 / 127);
    lv_label_set_text(dirct_speed_slider_label, buf);
    lv_obj_align_to(dirct_speed_slider_label, dirct_speed_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
}


/*
*	屏幕初始化函数
*/
void lv_disp_init(void)
{
    static lv_color_t buf1[BUFF_SIZE];
    static lv_color_t buf2[BUFF_SIZE];
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, BUFF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.hor_res = LV_HOR_RES_MAX;
    disp_drv.ver_res = LV_VER_RES_MAX;
    disp_drv.full_refresh = 1; 
    lv_disp_drv_register(&disp_drv);
}

/*
*	共享内存初始化函数
*/
void mqtt_shm_init(void)
{
    if((shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0644)) < 0)
    {
        fprintf(stderr, "lvgl:shm_open %s err,errno=%d(%s).", SHM_NAME, errno,strerror(errno));
        exit(1);
    }
    
    if(ftruncate(shm_fd, sizeof(car_status_t)) < 0)
    {
        perror("ftruncate err");
        exit(1);
    }

    car_status = (car_status_t *)mmap(NULL, sizeof(car_status_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    car_status->speed_max = CAR_MAX_SPEED;
    if(car_status == MAP_FAILED)
    {
        shm_unlink(SHM_NAME);
        perror("map err");
        exit(1);
    }
    close(shm_fd);
}

/*
*	信号量初始化函数
*/
void mqtt_sem_init(void)
{
    if((mqtt_sem_sub = sem_open(SEM_SUB_NAME, O_CREAT | O_RDWR, 0644, 0)) == SEM_FAILED) 
    {
        munmap(car_status, sizeof(car_status_t));
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_SUB_NAME);
        perror("sem_open err");
        exit(1);
    }
}

/*
*	收到Ctrl+C信号的处理函数
*/
void sig_handler(int sig)
{
    munmap(car_status, sizeof(car_status_t));
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_SUB_NAME);
    printf("LVGL: Received %d signal\n", sig);
    exit(sig);
}

/**
 * 摄像头初始化函数
 */
int camera_lvgl_init(lv_obj_t *parent)
{
    fprintf(stdout, "camera_lvgl_init runing!\n");
    int shm_frm_fd;
    if((shm_frm_fd = shm_open(SHM_FRM_NAME, O_RDWR | O_CREAT, 0644)) < 0)
    {
        fprintf(stderr, "lvgl:shm_open %s err,errno=%d(%s).", SHM_FRM_NAME, errno,strerror(errno));
        strerror(errno);
        return -1;
    }
    if(ftruncate(shm_frm_fd, sizeof(struct camera_frm_struct) + CAM_W * CAM_H * 4) < 0)
    {
        perror("lvgl:ftruncate err");
        return -1;
    }
    if((camera_frm = mmap(NULL, sizeof(struct camera_frm_struct) + CAM_W * CAM_H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, shm_frm_fd, 0)) == MAP_FAILED)
    {
        perror("lvgl:mmap err");
        return -1;
    }

    cam_img_dsc.header.always_zero = 0;
    cam_img_dsc.header.w = CAM_W;
    cam_img_dsc.header.h = CAM_H;
    cam_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA; /* 32bit with alpha */
    cam_img_dsc.data_size = CAM_W * CAM_H * 4;
    cam_img_dsc.data = (uint8_t *)camera_frm->frm; /* 指向不断更新的缓冲 */

    cam_img_obj = lv_img_create(parent);
    lv_img_set_src(cam_img_obj, &cam_img_dsc);
    lv_obj_align(cam_img_obj, LV_ALIGN_CENTER, 0, 0);
    return 0;
}

// 简单的日志回调函数
void my_log_cb(const char * buf)
{
    printf("%s", buf); // 直接打印到串口/终端
}

int main(void)
{
    fprintf(stdout, "lvgl runing!\n");
    lv_init();
    fbdev_init();
    lv_disp_init();
    lv_indev_init();
    mqtt_shm_init();
    mqtt_sem_init();

    // 启用日志
    lv_log_register_print_cb(my_log_cb);

    if(signal(SIGINT, sig_handler) == SIG_ERR)
    {
        perror("signal err");
        return -1;
    }

    //lvgl的时间线程
    pthread_t tid;
    pthread_create(&tid, NULL, tick_thread, NULL);

    //mqtt接收定时器
    lv_timer_create(mqtt_recv_cb, 100, NULL);

	//创建前进、后退、左转、右转按钮
    page_main = lv_obj_create(lv_scr_act());
    lv_obj_set_size(page_main, lv_pct(100), lv_pct(100));

    advance = lv_btn_create(page_main);
    lv_obj_set_size(advance, 200, 150);
    lv_obj_set_align(advance, LV_ALIGN_LEFT_MID);
    lv_obj_set_style_bg_opa(advance, 0, 0); 
    lv_obj_set_style_shadow_width(advance, 0, 0);
    lv_obj_add_event_cb(advance, triangle_draw_cb,LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(advance, LV_OBJ_FLAG_ADV_HITTEST); 
    lv_obj_add_event_cb(advance, triangle_hit_cb, LV_EVENT_HIT_TEST, NULL);
    lv_obj_add_event_cb(advance, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(advance, btn_cb, LV_EVENT_RELEASED, NULL);

    retreat = lv_btn_create(page_main);
    lv_obj_set_size(retreat, 200, 150);
    lv_obj_set_align(retreat, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_set_style_bg_opa(retreat, 0, 0); 
    lv_obj_set_style_shadow_width(retreat, 0, 0);
    lv_obj_add_event_cb(retreat, triangle_draw_cb,LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(retreat, LV_OBJ_FLAG_ADV_HITTEST); 
    lv_obj_add_event_cb(retreat, triangle_hit_cb, LV_EVENT_HIT_TEST, NULL);
    lv_obj_add_event_cb(retreat, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(retreat, btn_cb, LV_EVENT_RELEASED, NULL);

    turn_right = lv_btn_create(page_main);
    lv_obj_set_size(turn_right, 150, 200);
    lv_obj_align(turn_right, LV_ALIGN_BOTTOM_RIGHT, 0, -100);
     lv_obj_set_style_bg_opa(turn_right, 0, 0); 
    lv_obj_set_style_shadow_width(turn_right, 0, 0);
    lv_obj_add_event_cb(turn_right, triangle_draw_cb,LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(turn_right, LV_OBJ_FLAG_ADV_HITTEST); 
    lv_obj_add_event_cb(turn_right, triangle_hit_cb, LV_EVENT_HIT_TEST, NULL);
    lv_obj_add_event_cb(turn_right, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(turn_right, btn_cb, LV_EVENT_RELEASED, NULL);

    turn_left = lv_btn_create(page_main);
    lv_obj_set_size(turn_left, 150, 200);
    lv_obj_align_to(turn_left, turn_right, LV_ALIGN_OUT_LEFT_MID, -80 , 0);
    lv_obj_set_style_bg_opa(turn_left, 0, 0); 
    lv_obj_set_style_shadow_width(turn_left, 0, 0);
    lv_obj_add_event_cb(turn_left, triangle_draw_cb,LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(turn_left, LV_OBJ_FLAG_ADV_HITTEST); 
    lv_obj_add_event_cb(turn_left, triangle_hit_cb, LV_EVENT_HIT_TEST, NULL);
    lv_obj_add_event_cb(turn_left, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(turn_left, btn_cb, LV_EVENT_RELEASED, NULL);

    //创建行进速度滑动条
    speed_slider = lv_slider_create(page_main);
    lv_obj_align(speed_slider, LV_ALIGN_TOP_MID, 0, 50);
    lv_slider_set_range(speed_slider, 0, 100);
    lv_obj_add_event_cb(speed_slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    speed_slider_label = lv_label_create(page_main);
    lv_slider_set_value(speed_slider, CAR_MAX_SPEED * 100 / 127, LV_ANIM_OFF);
    lv_label_set_text_fmt(speed_slider_label, "%d%%", CAR_MAX_SPEED * 100 / 127);
    lv_obj_align_to(speed_slider_label, speed_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_t *speed_label = lv_label_create(page_main);
    lv_label_set_text(speed_label, "行进速度");
    lv_obj_set_style_text_font(speed_label, &lv_font_SiYuanHeiTi_Bold_20, 0);
    lv_obj_align_to(speed_label, speed_slider, LV_ALIGN_OUT_LEFT_MID, -20, 0);

    //创建转向速度滑动条
    dirct_speed_slider = lv_slider_create(page_main);
    lv_obj_align_to(dirct_speed_slider, speed_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 50);
    lv_obj_add_event_cb(dirct_speed_slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    dirct_speed_slider_label = lv_label_create(page_main);
    lv_label_set_text(dirct_speed_slider_label, "0%");
    lv_obj_align_to(dirct_speed_slider_label, dirct_speed_slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_t *dirct_speed_label = lv_label_create(page_main);
    lv_label_set_text(dirct_speed_label, "转向速度");
    lv_obj_set_style_text_font(dirct_speed_label, &lv_font_SiYuanHeiTi_Bold_20, 0);
    lv_obj_align_to(dirct_speed_label, dirct_speed_slider, LV_ALIGN_OUT_LEFT_MID, -20, 0);

    //创建摄像头图像对象
    page_camera = lv_obj_create(lv_scr_act());
    lv_obj_add_flag(page_camera, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(page_camera, lv_pct(100), lv_pct(100));

    //创建播放器页
    page_player = lv_obj_create(lv_scr_act());
    lv_obj_add_flag(page_player, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(page_player, lv_pct(100), lv_pct(100));
    //创建播放器
    player = lv_ffmpeg_player_create(page_player);
    // lv_obj_add_flag(player, LV_OBJ_FLAG_HIDDEN);
    // lv_ffmpeg_player_set_auto_restart(player, false);
    // lv_ffmpeg_player_set_cmd(player, LV_FFMPEG_PLAYER_CMD_STOP);
    lv_obj_center(player);
    //创建播放器进度条
    video_progress = lv_slider_create(page_player);
    lv_obj_add_event_cb(video_progress, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(video_progress, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_width(video_progress, 600);
    //创建视频时间
    video_time = lv_label_create(page_player);
    lv_label_set_text(video_time, "0%");
    lv_obj_align_to(video_time, video_progress,LV_ALIGN_OUT_RIGHT_MID, 30, 0);
    //创建视频当前时间
    current_time = lv_label_create(page_player);
    lv_label_set_text(current_time, "0%");
    lv_obj_align_to(current_time, video_progress, LV_ALIGN_OUT_LEFT_MID, -30, 0);


    //lv_ffmpeg_player_get_current_time()
    //lv_ffmpeg_player_get_total_time()
    //lv_ffmpeg_player_seek();






    //创建下拉框，用来切换摄像头模式和小车控制器模式
    lv_obj_t * mode_dd = lv_dropdown_create(lv_scr_act());
    lv_dropdown_set_options(mode_dd, "controller\n" "camera\n" "playback\n" "camera(p2p)\n" "camera(c/s)");
    lv_obj_align(mode_dd, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_add_event_cb(mode_dd, dropdown_set_cb, LV_EVENT_ALL, NULL);

    if(camera_lvgl_init(page_camera) < 0)
        return -1;

    while(1) {
        lv_timer_handler();	//处理lvgl任务
        if (!lv_obj_has_flag(page_camera, LV_OBJ_FLAG_HIDDEN)) 
        {
            if(pthread_mutex_trylock(&camera_frm->camera_frm_mutex) == 0)
            {
                //printf("camera_frm->new_frame_ready:%d\n",camera_frm->new_frame_ready);

                if (camera_frm->new_frame_ready == 1)
                {
                    lv_obj_invalidate(cam_img_obj);
                    camera_frm->new_frame_ready = 0;
                    pthread_cond_signal(&camera_frm->camera_frm_cond);//向条件变量发送信号
                }
                pthread_mutex_unlock(&camera_frm->camera_frm_mutex);
            }
            //重绘
        }
        usleep(5000);
    }

    return 0;
}

