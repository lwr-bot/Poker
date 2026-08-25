#include "health_http_server.hpp"

#include <muduo/net/http/HttpRequest.h>
#include <muduo/net/http/HttpResponse.h>

#include <string>
#include <utility>

namespace poker::server {

HealthHttpServer::HealthHttpServer(muduo::net::EventLoop* loop,
                                   const muduo::net::InetAddress& address,
                                   observability::MetricsRegistry& metrics,
                                   Readiness readiness)
    : server_(loop, address, "PokerHealth"),
      metrics_(metrics),
      readiness_(std::move(readiness)) {
    server_.setHttpCallback([this](const auto& request, auto* response) {
        handle(request, response);
    });
}

void HealthHttpServer::start() {
    server_.start();
}

void HealthHttpServer::handle(const muduo::net::HttpRequest& request,
                              muduo::net::HttpResponse* response) const {
    response->setCloseConnection(true);
    if (request.path() == "/health/live") {
        response->setStatusCode(muduo::net::HttpResponse::k200Ok);
        response->setStatusMessage("OK");
        response->setContentType("application/json; charset=utf-8");
        response->setBody("{\"status\":\"live\"}\n");
        return;
    }
    if (request.path() == "/health/ready") {
        const bool ready = readiness_ && readiness_();
        response->setStatusCode(static_cast<muduo::net::HttpResponse::HttpStatusCode>(
            ready ? 200 : 503));
        response->setStatusMessage(ready ? "OK" : "Service Unavailable");
        response->setContentType("application/json; charset=utf-8");
        response->setBody(ready ? "{\"status\":\"ready\"}\n"
                                : "{\"status\":\"not_ready\"}\n");
        return;
    }
    if (request.path() == "/metrics") {
        response->setStatusCode(muduo::net::HttpResponse::k200Ok);
        response->setStatusMessage("OK");
        response->setContentType("text/plain; version=0.0.4; charset=utf-8");
        response->setBody(metrics_.renderPrometheus());
        return;
    }
    response->setStatusCode(muduo::net::HttpResponse::k404NotFound);
    response->setStatusMessage("Not Found");
    response->setContentType("text/plain; charset=utf-8");
    response->setBody("not found\n");
}

}  // namespace poker::server
