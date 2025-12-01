#ifndef _COMMON_H
#define _COMMON_H

typedef struct
{
    signed char current_speed;		//当前速度  范围:-128~127
    signed char speed_max;			//最大速度  范围:0~127
    signed char direction;			//运动方向（左右）
    signed char dirct_speed;        //小车转向速度
}car_status_t;						//小车的运动状态


#endif