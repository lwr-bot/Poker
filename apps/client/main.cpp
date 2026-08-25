#include "network_client.hpp"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml>

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName("PokerTable");
    QGuiApplication::setOrganizationName("PokerServer");

    qmlRegisterType<poker::client::NetworkClient>("Poker.Client", 1, 0, "NetworkClient");
    QQmlApplicationEngine engine;
    engine.loadFromModule("Poker.Client", "Main");
    return engine.rootObjects().isEmpty() ? 1 : application.exec();
}
