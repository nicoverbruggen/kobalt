#include "device.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Read Kindle device status without stopping the stock UI"));
    parser.addHelpOption();
    parser.addOption({"https", "Also verify an HTTPS HEAD request using the system CA store.", "url"});
    parser.process(app);
    QUrl url;
    if (parser.isSet("https")) {
        url = QUrl(parser.value("https"));
        if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() || !url.userInfo().isEmpty()) {
            std::fprintf(stderr, "--https requires an HTTPS URL without credentials\n");
            return 2;
        }
    }
    auto device = kobalt::kindle::makeDevice();
    QNetworkAccessManager network;
    QJsonObject report;
    bool statusDone = false, networkDone = url.isEmpty();
    int result = 0;
    const auto finish = [&] {
        if (!statusDone || !networkDone)
            return;
        const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        device->stop();
        app.exit(result);
    };
    QObject::connect(device.get(), &kobalt::kindle::Device::initializedChanged, &app, [&] {
        const auto &state = device->state();
        report["battery"] = QJsonObject{{"percent", state.battery.percent}, {"charging", state.battery.charging},
                                       {"fullyCharged", state.battery.fullyCharged}};
        report["wifi"] = QJsonObject{{"enabled", state.wifi.enabled}, {"connected", state.wifi.connected},
                                    {"strength", state.wifi.strength}};
        report["bluetooth"] = QJsonObject{{"connected", state.bluetoothConnected}};
        report["light"] = QJsonObject{{"brightness", state.brightness}, {"maximumBrightness", state.maximumBrightness},
                                     {"brightnessPercent", kobalt::kindle::Device::levelToPercent(state.brightness, state.maximumBrightness)},
                                     {"warmth", state.warmth}, {"maximumWarmth", 24}};
        report["sleeping"] = state.sleeping;
        if (state.battery.percent < 0 || !state.hasLight() || !state.hasWarmth())
            result = 1;
        statusDone = true;
        finish();
    });
#ifndef QT_NO_SSL
    report["tls"] = QJsonObject{{"available", QSslSocket::supportsSsl()}, {"backend", QSslSocket::activeBackend()},
                               {"runtime", QSslSocket::sslLibraryVersionString()}};
#else
    report["tls"] = QJsonObject{{"available", false}};
#endif
    if (!url.isEmpty()) {
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setMaximumRedirectsAllowed(5);
        request.setTransferTimeout(15000);
        auto *reply = network.head(request);
        QObject::connect(reply, &QNetworkReply::finished, &app, [&, reply] {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = reply->error() == QNetworkReply::NoError && code >= 200 && code < 400;
            report["https"] = QJsonObject{{"url", reply->url().toString()}, {"status", code}, {"ok", ok},
                                         {"error", ok ? QString() : reply->errorString()}};
            if (!ok)
                result = 1;
            networkDone = true;
            reply->deleteLater();
            finish();
        });
    }
    QTimer::singleShot(20000, &app, [&] {
        report["timeout"] = true;
        result = 1;
        statusDone = networkDone = true;
        finish();
    });
    return app.exec();
}
