#define _DEFAULT_SOURCE

#include <MQTTClient.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdlib.h>
#include "common.h"

#define MQTT_SERVER_URI     "192.168.1.8"
#define CLIENT_ID           "imx6ull_id"
#define KEEP_ALIVE_INTERVAL 60
#define CLEAN_SESSION       0
#define USERNAME            "mqtt"
#define PASSWORD            "123456"
#define WILL_TOPIC_NAME     "mqtt/willTopic"
#define WILL_MSG            "will msg test"
#define WILL_RETAINED       1
#define WILL_QOS            0
#define MQTT_TOPIC_NAME_PUB     "mqtt/control"
#define MQTT_TOPIC_NAME_RCV     "mqtt/report"
#define SHM_NAME                "/lvgl_mqtt_shm"			    //共享内存名字
#define SEM_RECV_NAME           "/lvgl_mqtt_sem_recv"			//接收信号量名字
#define SEM_SUB_NAME            "/lvgl_mqtt_sem_sub"			//发布信号量名字

static car_status_t *car_status;    //小车状态
static int shm_fd;                  //car_status共享内存句柄
static sem_t *mqtt_sem_recv;		//用来同步mqtt消息进程的信号量
static sem_t *mqtt_sem_sub;			//用来同步mqtt消息进程的信号量

//mqtt连接丢失回调
static void connLost(void* context, char* cause)
{
    printf("\nConnect lost!\n");
};

//mqtt接收消息回调
static int msgArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
    // car_status_t *car_status_echo = message->payload;

    // printf("Message arrived\n");
    // printf("topicName: %.*s\n", topicName);
    // printf("msglen:%d\n", message->payloadlen);
    // printf("msgQos:%d\n", message->qos);
    // printf("msg:current_speed=%d\n",car_status_echo->current_speed);
    // printf("topicName:%s\n", topicName);
    
    car_status = message->payload;


    printf("Message arrived\n");
    printf("topicName: %.*s\n", topicName);
    printf("msglen:%d\n", message->payloadlen);
    printf("msgQos:%d\n", message->qos);
    printf("msg:current_speed=%d\n",car_status->current_speed);
    printf("topicName:%s\n", topicName);


    sem_post(mqtt_sem_recv);

    //必须释放
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
};

//发布消息（QoS1/2）确认送达回调
static void deliveryComplete(void* context, MQTTClient_deliveryToken dt)
{
    printf("Message with token value %d delivery confirmed\n", dt);
};




int main(void)
{
    if((shm_fd = shm_open(SHM_NAME, O_RDWR, 0644)) < 0)
    {
        perror("shm_open err");
        return -1;
    }
    

    car_status = (car_status_t *)mmap(NULL, sizeof(car_status_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(car_status == MAP_FAILED)
    {
        perror("map err");
        return -1;
    }
    close(shm_fd);

   if(((mqtt_sem_sub = sem_open(SEM_SUB_NAME, O_RDWR)) == SEM_FAILED) 
        || ((mqtt_sem_recv = sem_open(SEM_RECV_NAME, O_RDWR)) == SEM_FAILED))
    {
        munmap(car_status, sizeof(car_status_t));
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_SUB_NAME);
        sem_unlink(SEM_RECV_NAME);
        perror("sem_open err");
        exit(1);
    }


    MQTTClient client;
    MQTTClient_connectOptions conn_opt = MQTTClient_connectOptions_initializer;
    MQTTClient_willOptions will_opt = MQTTClient_willOptions_initializer;
    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int ret;
    if(MQTTCLIENT_SUCCESS != 
        (ret = MQTTClient_create(&client, MQTT_SERVER_URI, CLIENT_ID,MQTTCLIENT_PERSISTENCE_NONE, NULL)))
    {
        perror("MQTTClient_create err");
        return ret;
    }

    if(MQTTCLIENT_SUCCESS != (ret = MQTTClient_setCallbacks(client, NULL, connLost, msgArrived, deliveryComplete)))
    {
        perror("MQTTClient_setCallbacks err");
        return ret;
    }

    will_opt.topicName = WILL_TOPIC_NAME;
    will_opt.message = WILL_MSG;
    will_opt.retained = WILL_RETAINED;
    will_opt.qos = WILL_QOS;

    conn_opt.keepAliveInterval = KEEP_ALIVE_INTERVAL;
    conn_opt.cleansession = CLEAN_SESSION;
    conn_opt.username = USERNAME;
    conn_opt.password = PASSWORD;
    conn_opt.will = &will_opt;
    
    if(MQTTCLIENT_SUCCESS != (ret = MQTTClient_connect(client, &conn_opt)))
    {
        perror("MQTTClient_connect err\n");
        return ret;
    }

    printf("\nMQTT server connection successful!\n");

    if(MQTTCLIENT_SUCCESS != (ret = MQTTClient_subscribe(client, MQTT_TOPIC_NAME_RCV, 0)))
    {
        perror("MQTTClient_subscribe err");
        return ret;
    }

    while (1)
    {
        sem_wait(mqtt_sem_sub);
        char payload[128];
        sprintf(payload, "{\"current_speed\":\"%d\",\"speed_max\":\"%d\",\"direction\":\"%d\",\"dirct_speed\":\"%d\"}",
            car_status->current_speed, car_status->speed_max, car_status->direction, car_status->dirct_speed);
        msg.payload = payload;
        printf("payload : %s\n", payload);
        msg.payloadlen = strlen(payload);
        msg.qos = 0;
        msg.retained = 1;
        if(MQTTCLIENT_SUCCESS != (ret = MQTTClient_publishMessage(client, MQTT_TOPIC_NAME_PUB, &msg, &token)))
        {
            perror("MQTTClient_publishMessage err\n");
            return ret;
        }
    }
    

    while (1)
    {
        sleep(10);
    }
    
}