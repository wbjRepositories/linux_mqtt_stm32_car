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

#include "../lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"

#define MAX_FINGERS 2      		//支持的最大手指数量
#define LV_HOR_RES_MAX 1024		//屏幕水平像素数量
#define LV_VER_RES_MAX 600		//屏幕垂直像素数量
#define SHM_NAME "/lvgl_mqtt_shm"			//共享内存名字
#define SEM_NAME "/lvgl_mqtt_sem"			//信号量名字
#define BUFF1_SIZE LV_HOR_RES_MAX * 10		//buff1字节数
#define CAR_MAX_SPEED   128

typedef struct {
    int tracking_id;		//手指id
    int x;					//手指在屏幕的X轴位置
    int y;					//手指在屏幕的Y轴位置
    bool pressed;			//是否按下
} touch_point_t;			//触摸点状态


static touch_point_t points[MAX_FINGERS];	//记录当前的多个触摸点状态
static int ts_fd = -1;				//屏幕事件句柄
static car_status_t *car_status;	//当前小车状态
static lv_obj_t *advance;			//前进按钮
static lv_obj_t *retreat;			//后退按钮
static lv_obj_t *turn_left;			//左转按钮
static lv_obj_t *turn_right;		//右转按钮
static sem_t *mqtt_sem;				//用来同步mqtt消息进程的信号量
static int shm_fd;					//共享内存文件句柄

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
    lv_indev_t * indev = lv_indev_drv_register(&indev_drv);
    printf("LVGL indev registered for slot %d\n", 0);

    static lv_indev_drv_t indev_drv1;
    lv_indev_drv_init(&indev_drv1);
    indev_drv1.type = LV_INDEV_TYPE_POINTER;
    indev_drv1.read_cb = my_touch_read_cb;
    indev_drv1.user_data = (void *)(intptr_t)1;
    lv_indev_t * indev1 = lv_indev_drv_register(&indev_drv1);
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
            car_status->current_speed = 10;
            printf("speed:10\n");
        }
        if (target == retreat)
        {
            car_status->current_speed = -10;
            printf("speed:-10\n");
        }
        if (target == turn_left)
        {
            
        }
        if (target == turn_right)
        {
            
        }
        sem_post(mqtt_sem);
    }
    else if(code == LV_EVENT_RELEASED)
    {
        lv_obj_t *target = lv_event_get_target(e);
        if(target == advance)
        {
            car_status->current_speed = 0;
            printf("speed:0\n");
        }
        if (target == retreat)
        {
            car_status->current_speed = 0;
            printf("speed:0\n");
        }
        if (target == turn_left)
        {
            
        }
        if (target == turn_right)
        {
            
        }
        sem_post(mqtt_sem);
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

/*
*	屏幕初始化函数
*/
void lv_disp_init(void)
{
    static lv_color_t buf1[BUFF1_SIZE];
    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, BUFF1_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.hor_res = LV_HOR_RES_MAX;
    disp_drv.ver_res = LV_VER_RES_MAX;
    lv_disp_drv_register(&disp_drv);
}

/*
*	共享内存初始化函数
*/
void mqtt_shm_init(void)
{
    if((shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0644)) < 0)
    {
        perror("shm_open err");
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
    if((mqtt_sem = sem_open(SEM_NAME, O_CREAT | O_RDWR, 0644, 0)) == SEM_FAILED)
    {
        munmap(car_status, BUFF1_SIZE);
        shm_unlink(SHM_NAME);
        perror("sem_open err");
        exit(1);
    }
}

/*
*	收到Ctrl+C信号的处理函数
*/
void sig_handler(int sig)
{
    munmap(car_status, BUFF1_SIZE);
    shm_unlink(SHM_NAME);
    printf("接受到%d函数\n", sig);
    exit(sig);
}

int main(void)
{
    lv_init();
    fbdev_init();
    lv_disp_init();
    lv_indev_init();
    mqtt_shm_init();
    mqtt_sem_init();

    if(signal(SIGINT, sig_handler) == SIG_ERR)
    {
        perror("signal err");
        return -1;
    }

    //lvgl的时间线程
    pthread_t tid;
    pthread_create(&tid, NULL, tick_thread, NULL);

	//创建前进、后退、左转、右转按钮
    advance = lv_btn_create(lv_scr_act());
    lv_obj_set_size(advance, 200, 150);
    lv_obj_set_align(advance, LV_ALIGN_LEFT_MID);
    lv_obj_add_event_cb(advance, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(advance, btn_cb, LV_EVENT_RELEASED, NULL);

    retreat = lv_btn_create(lv_scr_act());
    lv_obj_set_size(retreat, 200, 150);
    lv_obj_set_align(retreat, LV_ALIGN_BOTTOM_LEFT);
    lv_obj_add_event_cb(retreat, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(retreat, btn_cb, LV_EVENT_RELEASED, NULL);

    turn_left = lv_btn_create(lv_scr_act());
    lv_obj_set_size(turn_left, 200, 150);
    lv_obj_set_align(turn_left, LV_ALIGN_BOTTOM_MID);
    lv_obj_add_event_cb(turn_left, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(turn_left, btn_cb, LV_EVENT_RELEASED, NULL);

    turn_right = lv_btn_create(lv_scr_act());
    lv_obj_set_size(turn_right, 200, 150);
    lv_obj_set_align(turn_right, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_add_event_cb(turn_right, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(turn_right, btn_cb, LV_EVENT_RELEASED, NULL);

    while(1) {
        lv_timer_handler();	//处理lvgl任务
        usleep(5000);
    }

    return 0;
}

