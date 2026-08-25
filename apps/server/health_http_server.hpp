#ifndef POKER_SERVER_HEALTH_HTTP_SERVER_HPP
#define POKER_SERVER_HEALTH_HTTP_SERVER_HPP

#include "poker/observability/metrics.hpp"

#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include <muduo/net/http/HttpServer.h>

#include <functional>

namespace poker::server {

class HealthHttpServer {
public:
    using Readiness = std::function<bool()>;

    HealthHttpServer(muduo::net::EventLoop* loop,
                     const muduo::net::InetAddress& address,
                     observability::MetricsRegistry& metrics,
                     Readiness readiness);

    void start();

private:
    void handle(const muduo::net::HttpRequest& request,
                muduo::net::HttpResponse* response) const;

    muduo::net::HttpServer server_;
    observability::MetricsRegistry& metrics_;
    Readiness readiness_;
};

}  // namespace poker::server

#endif
