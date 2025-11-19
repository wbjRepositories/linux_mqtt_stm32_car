#include "task_mqtt.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>

#define DEVICE_NAME "esp32"
#define MQTT_USER   "mqtt"
#define MQTT_PASS   "123456"
#define	MQTT_TOPIC_NAME_RCV			"mqtt/control"
#define	MQTT_TOPIC_NAME_PUB			"mqtt/report"
#define MQTT_IP			"192.168.1.8"
#define MQTT_PORT		"1883"


extern SemaphoreHandle_t mqtt_ok;

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
		//xSemaphoreTake(mqtt_ok, portMAX_DELAY);
		//buff = (uint8_t *)"AT+MQTTPUB=0," MQTT_TOPIC_NAME_PUB ",\"test\",1,0\r\n";
		//HAL_UART_Transmit(&huart3, buff, strlen((char *)buff), 1000);
		vTaskDelay(1000);
	}
}
