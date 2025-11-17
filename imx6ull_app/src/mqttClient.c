#define _DEFAULT_SOURCE

#include <MQTTClient.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include "common.h"

#define MQTT_SERVER_URI     "192.168.1.11"
#define CLIENT_ID           "imx6ull_id"
#define KEEP_ALIVE_INTERVAL 60
#define CLEAN_SESSION       0
#define USERNAME            "mqtt"
#define PASSWORD            "123456"

#define WILL_TOPIC_NAME     "mqtt/willTopic"
#define WILL_MSG            "will msg test"
#define WILL_RETAINED       1
#define WILL_QOS            0

#define MQTT_TOPIC_NAME     "mqtt/topic"

car_status_t *car_status;


static void connLost(void* context, char* cause)
{
    printf("\nConnect lost!\n");
};
static int msgArrived(void* context, char* topicName, int topicLen, MQTTClient_message* message)
{
    car_status_t *car_status_echo = message->payload;
    printf("Message arrived\n");

    printf("topicName: %.*s\n", topicName);
    printf("msglen:%d\n", message->payloadlen);
    printf("msgQos:%d\n", message->qos);
    printf("msg:current_speed=%d\n",car_status_echo->current_speed);

    printf("topicName:%s\n", topicName);
    
    
    
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
};
static void deliveryComplete(void* context, MQTTClient_deliveryToken dt)
{
    printf("Message with token value %d delivery confirmed\n", dt);
};

int main(void)
{
    int shm_fd;
    if((shm_fd = shm_open("/lvgl_mqtt_shm", O_RDWR, 0644)) < 0)
    {
        perror("shm_open err");
        return -1;
    }
    
    // int ret;
    // if((ret = ftruncate(shm_fd, sizeof(car_status_t))) < 0)
    // {
    //     perror("ftruncate err");
    //     return -1;
    // }

    car_status = (car_status_t *)mmap(NULL, sizeof(car_status_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(car_status == MAP_FAILED)
    {
        perror("map err");
        return -1;
    }
    close(shm_fd);

    sem_t *mqtt_sem = sem_open("/lvgl_mqtt_sem", O_RDWR);


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

    if(MQTTCLIENT_SUCCESS != (ret = MQTTClient_subscribe(client, MQTT_TOPIC_NAME, 0)))
    {
        perror("MQTTClient_subscribe err");
        return ret;
    }

    while (1)
    {
        sem_wait(mqtt_sem);
        msg.payload = car_status;
        printf("current_speed:%d\n",car_status->current_speed);
        msg.payloadlen = sizeof(car_status_t);
        msg.qos = 0;
        msg.retained = 1;
        if(MQTTCLIENT_SUCCESS != (ret = MQTTClient_publishMessage(client, MQTT_TOPIC_NAME, &msg, &token)))
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