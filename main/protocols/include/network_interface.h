#pragma once


#include <memory>
#include "mqtt.h"
#include "http.h"
#include "udp.h"
#include "tcp.h"

class NetworkInterface {
public:
    virtual ~NetworkInterface() = default;

    virtual std::unique_ptr<Mqtt> CreateMqtt(int connect_id = -1) = 0;
    virtual std::unique_ptr<Udp> CreateUdp(int connect_id = -1) = 0;
    virtual std::unique_ptr<Tcp> CreateTcp(int connect_id = -1) = 0;
    virtual std::unique_ptr<Http> CreateHttp(int connect_id = -1) = 0;
};
