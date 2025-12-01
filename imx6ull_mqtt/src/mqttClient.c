#define _DEFAULT_SOURCE

#include <MQTTClient.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdlib.h>
#include "common.h"
#include "jsmn.h"
#include <stdbool.h>
#include <errno.h>

#define MQTT_SERVER_URI     "192.168.1.10"      //mqtt服务器地址
#define CLIENT_ID           "imx6ull_id"        //mqtt设备id
#define KEEP_ALIVE_INTERVAL 60                  //心跳请求时间
#define CLEAN_SESSION       false                   //是否清除会话
#define USERNAME            "mqtt"                  //服务器用户名
#define PASSWORD            "123456"                //服务器密码
#define WILL_TOPIC_NAME     "mqtt/willTopic"        //遗言主题
#define WILL_MSG            "will msg test"         //遗言消息
#define WILL_RETAINED       1                       //是否保留遗言
#define WILL_QOS            0                       //遗言qos等级
#define MQTT_TOPIC_NAME_PUB     "mqtt/control"      //发送消息主题 
#define MQTT_TOPIC_NAME_RCV     "mqtt/report"       //接收消息主题
#define SHM_NAME                "/lvgl_mqtt_shm"			    //共享内存名字
#define SEM_RECV_NAME           "/lvgl_mqtt_sem_recv"			//接收信号量名字
#define SEM_SUB_NAME            "/lvgl_mqtt_sem_sub"			//发布信号量名字
#define MQTT_RETAINED       1                       //是否保留消息
#define MQTT_QOS            0                       //消息qos等级

static car_status_t *car_status;    //小车状态
static int shm_fd;                  //car_status共享内存句柄
static sem_t *mqtt_sem_recv;		//用来同步mqtt消息进程的信号量
static sem_t *mqtt_sem_sub;			//用来同步mqtt消息进程的信号量
char json_buf[256];                 //用来存放json


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

//josn转car_status结构体
int json_to_car_status(const char *json, car_status_t *status)
{
    jsmn_parser parser;
    jsmntok_t tokens[64];  // 足够解析你的 JSON
    int ret, i;

    jsmn_init(&parser);
    ret = jsmn_parse(&parser, json, strlen(json), tokens, 64);

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

//mqtt连接丢失回调
static void connLost(void* context, char* cause)
{
    printf("\nConnect lost!\n");
};

//mqtt接收消息回调
static int msgArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
    printf("message->payload:%s\n",message->payload);
    char *json_start = strchr((char *)message->payload, '{');   // 查找 JSON 起点
    char *json_end   = strrchr((char *)message->payload, '}');  // 查找 JSON 终点

    if(json_start && json_end)
    {
            int json_len = json_end - json_start + 1;
            memcpy(json_buf, json_start, json_len);
            json_buf[json_len] = '\0';   // 加字符串结束符
    }
    printf("json_buf:%s\n",json_buf);
    json_to_car_status((char *)json_buf, car_status);

    printf("Message arrived\n");
    // printf("topicName: %.*s\n", topicName);
    // printf("msglen:%d\n", message->payloadlen);
    // printf("msgQos:%d\n", message->qos);
    // printf("msg:speed_max=%d\n",car_status->speed_max);
    // printf("topicName:%s\n", topicName);

    sem_post(mqtt_sem_recv);

    //必须释放
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
};

//发布消息（QoS1/2）确认送达回调
static void deliveryComplete(void* context, MQTTClient_deliveryToken dt)
{
    printf("Message with token value %d delivery confirmed\n", dt);
};


int main(void)
{
    fprintf(stdout, "mqttClient runing!\n");
    //创建共享内存
    if((shm_fd = shm_open(SHM_NAME, O_RDWR, 0644)) < 0)
    {
        fprintf(stderr, "mqttClient : shm_open %s err,errno=%d(%s)\n",SHM_NAME, errno,strerror(errno));
        return -1;
    }
    //映射共享内存
    car_status = (car_status_t *)mmap(NULL, sizeof(car_status_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(car_status == MAP_FAILED)
    {
        perror("map err");
        return -1;
    }
    close(shm_fd);

    // 创建信号量
   if(((mqtt_sem_sub = sem_open(SEM_SUB_NAME, O_RDWR)) == SEM_FAILED) 
        || ((mqtt_sem_recv = sem_open(SEM_RECV_NAME, O_RDWR)) == SEM_FAILED))
    {
        munmap(car_status, sizeof(car_status_t));
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_SUB_NAME);
        sem_unlink(SEM_RECV_NAME);
        fprintf(stderr, "mqttClient : sem_open %s err,errno=%d(%s)\n",SEM_SUB_NAME, errno,strerror(errno));
        exit(1);
    }

    //创建 mqtt 客户端对象
    MQTTClient client;
    MQTTClient_connectOptions conn_opt = MQTTClient_connectOptions_initializer;
    MQTTClient_willOptions will_opt = MQTTClient_willOptions_initializer;
    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int ret;
    if(MQTTCLIENT_SUCCESS != 
        (ret = MQTTClient_create(&client, MQTT_SERVER_URI, CLIENT_ID,MQTTCLIENT_PERSISTENCE_NONE, NULL)))
    {
        fprintf(stderr, "mqttClient : MQTTClient_create %s err,errno=%d(%s)\n",MQTT_SERVER_URI, errno,strerror(errno));
        return ret;
    }

    //设置回调
    if(MQTTCLIENT_SUCCESS != (ret = MQTTClient_setCallbacks(client, NULL, connLost, msgArrived, deliveryComplete)))
    {
        fprintf(stderr, "mqttClient : MQTTClient_setCallbacks %s err,errno=%d(%s)\n",MQTT_SERVER_URI, errno,strerror(errno));
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
    
    //连接 MQTT 服务器
    if(MQTTCLIENT_SUCCESS != (ret = MQTTClient_connect(client, &conn_opt)))
    {
        perror("MQTTClient_connect err\n");
        return ret;
    }

    printf("\nMQTT server connection successful!\n");

    //订阅主题
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
        msg.qos = MQTT_QOS;
        msg.retained = MQTT_RETAINED;
        //发布消息
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