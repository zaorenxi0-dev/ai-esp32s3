#include "esp_network.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mqtt.h"
#include "esp_udp.h"
#include "http_client.h"
#include "esp_tcp.h"

EspNetwork::EspNetwork()
{
}

EspNetwork::~EspNetwork()
{
}

std::unique_ptr<Mqtt> EspNetwork::CreateMqtt(int connect_id)
{
    return std::make_unique<EspMqtt>();
}

 std::unique_ptr<Udp> EspNetwork::CreateUdp(int connect_id)
{
    return std::make_unique<EspUdp>();
}

std::unique_ptr<Http> EspNetwork::CreateHttp(int connect_id)
{
    return std::make_unique<HttpClient>(this, connect_id);
}

std::unique_ptr<Tcp> EspNetwork::CreateTcp(int connect_id)
{
    return std::make_unique<EspTcp>();
}

