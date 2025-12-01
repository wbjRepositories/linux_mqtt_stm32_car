#include "task_mqtt.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "jsmn.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define UART_RX_BUF_SIZE 256
#define DEVICE_NAME "esp32"
#define MQTT_USER   "mqtt"
#define MQTT_PASS   "123456"
#define	MQTT_TOPIC_NAME_RCV			"mqtt/control"
#define	MQTT_TOPIC_NAME_PUB			"mqtt/report"
#define MQTT_IP			"192.168.1.10"
#define MQTT_PORT		"1883"


extern SemaphoreHandle_t mqtt_ok;
extern SemaphoreHandle_t mqtt_resond;
extern QueueHandle_t Queue_car_recv;
extern QueueHandle_t Queue_car_send;

uint8_t UART_dma_rx_buff[UART_RX_BUF_SIZE];    // DMA接收缓冲区
uint8_t UART_FrameBuf[UART_RX_BUF_SIZE];      // 提取后的帧数据
uint16_t UART_FrameLen = 0;
car_status_t car_status;
uint8_t json_buf[256];


//Mqtt通信任务，通过串口向ESP32发送AT指令实现通信
void Task_Mqtt( void * argv)
{
	//连接mqtt服务器
	xSemaphoreTake(mqtt_ok, portMAX_DELAY);
	uint8_t *buff = (uint8_t *)"AT+MQTTUSERCFG=0,1,\"" DEVICE_NAME "\",\"" MQTT_USER "\",\"" MQTT_PASS "\",0,0,\"\"\r\n";
	HAL_UART_Transmit(&huart3, buff, strlen((char *)buff), 1000);
	printf("buff : %s\n", buff);
	xSemaphoreTake(mqtt_ok, portMAX_DELAY);
	buff = (uint8_t *)"AT+MQTTCONN=0,\"" MQTT_IP "\"," MQTT_PORT ",1\r\n";
	HAL_UART_Transmit(&huart3, buff, strlen((char *)buff), 1000);
	printf("buff : %s\n", buff);
	//订阅mqtt主题
	xSemaphoreTake(mqtt_ok, portMAX_DELAY);
	buff = (uint8_t *)"AT+MQTTSUB=0,\"" MQTT_TOPIC_NAME_RCV "\",1\r\n";
	HAL_UART_Transmit(&huart3, buff, strlen((char *)buff), 1000);
	printf("buff : %s\n", buff);

	while(1)
	{
		//发送消息
		xSemaphoreTake(mqtt_resond, portMAX_DELAY);
		char payload[256];
		sprintf(payload,
						"{\\\"current_speed\\\":\\\"%d\\\"\\,\\\"speed_max\\\":\\\"%d\\\"\\,\\\"direction\\\":\\\"%d\\\"\\,\\\"dirct_speed\\\":\\\"%d\\\"}",
						car_status.current_speed,
						car_status.speed_max,
						car_status.direction,
						car_status.dirct_speed);

		static char cmd[512];
		sprintf(cmd,
						"AT+MQTTPUB=0,\"%s\",\"%s\",1,0\r\n",
						MQTT_TOPIC_NAME_PUB,
						payload);

		buff = (uint8_t *)cmd;
		
		HAL_UART_Transmit(&huart3, buff, strlen((char *)buff), 1000);
		printf("buff:%s\n",buff);
		
		vTaskDelay(1000);
	}
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s)
{
    if (tok->type == JSMN_STRING &&
        (int)strlen(s) == (tok->end - tok->start) &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0)
    {
        return 0; // match
    }
    return -1;
}


int json_to_car_status(const char *json, car_status_t *status)
{
    jsmn_parser parser;
    jsmntok_t tokens[32];
    int ret, i;

    jsmn_init(&parser);
    ret = jsmn_parse(&parser, json, strlen(json), tokens, 32);

    if (ret < 0)
        return -1; // JSON 格式错误

    if (ret < 1 || tokens[0].type != JSMN_OBJECT)
        return -2; // 根节点不是对象

    for (i = 1; i < ret; i++)
    {
        if (jsoneq(json, &tokens[i], "current_speed") == 0)
        {
            status->current_speed = atoi(json + tokens[i + 1].start);
            i++;
        }
        else if (jsoneq(json, &tokens[i], "speed_max") == 0)
        {
            status->speed_max = atoi(json + tokens[i + 1].start);
            i++;
        }
        else if (jsoneq(json, &tokens[i], "direction") == 0)
        {
            status->direction = atoi(json + tokens[i + 1].start);
            i++;
        }
				else if (jsoneq(json, &tokens[i], "dirct_speed") == 0)
        {
            status->dirct_speed = atoi(json + tokens[i + 1].start);
            i++;
        }
    }

    return 0; // success
}


void UART_FrameReceived(uint8_t *data, uint16_t len)
{
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		
		printf("mqtt data:%s\n", data);
		if(strstr((char *)data, "OK") != NULL)
		{
			xSemaphoreGiveFromISR(mqtt_ok, &xHigherPriorityTaskWoken);
		}
		else if(strstr((char *)data, "WIFI GOT IP") != NULL)
		{
			xSemaphoreGiveFromISR(mqtt_ok, &xHigherPriorityTaskWoken);
		}
		else if(strstr((char *)data, "ERROR") != NULL)
		{
			printf("mqtt err\n");
		}
		else if(strstr((char *)data, "MQTTSUBRECV") != NULL)
		{
			printf("mqtt receive!!!\n");
			
			char *json_start = strchr((char *)data, '{');   // 查找 JSON 起点
			char *json_end   = strrchr((char *)data, '}');  // 查找 JSON 终点

			if(json_start && json_end)
			{
					int json_len = json_end - json_start + 1;
					memcpy(json_buf, json_start, json_len);
					json_buf[json_len] = '\0';   // 加字符串结束符
			}

			
			json_to_car_status((char *)json_buf, &car_status);
			
			BaseType_t xHigherPriorityTaskWoken = pdFALSE;
			xQueueSendFromISR(Queue_car_recv, &car_status, &xHigherPriorityTaskWoken);
			printf("car_status.current_speed = %d\n", car_status.current_speed);
			printf("car_status.direction = %d\n", car_status.direction);
			printf("car_status.speed_max = %d\n", car_status.speed_max);
			printf("car_status.dirct_speed = %d\n", car_status.dirct_speed);
		}
		memset(UART_FrameBuf, 0, UART_RX_BUF_SIZE);
		
}


void UART_IdleCallback(UART_HandleTypeDef *huart)
{
    // 停止DMA
    HAL_UART_DMAStop(huart);

    // 获取当前接收的字节数
    UART_FrameLen = UART_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart->hdmarx);

    // 复制数据到FrameBuf
    memcpy(UART_FrameBuf, UART_dma_rx_buff, UART_FrameLen);

    // 重新开启DMA接收
    HAL_UART_Receive_DMA(huart, UART_dma_rx_buff, UART_RX_BUF_SIZE);
		//__HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE);
	
    // 处理帧数据
    UART_FrameReceived(UART_FrameBuf, UART_FrameLen);
}
