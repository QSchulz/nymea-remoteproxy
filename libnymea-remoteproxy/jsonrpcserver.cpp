#include "engine.h"
#include "jsonrpcserver.h"
#include "loggingcategories.h"
#include "jsonrpc/jsontypes.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QCoreApplication>

namespace remoteproxy {

JsonRpcServer::JsonRpcServer(QObject *parent) :
    JsonHandler(parent)
{
    // Methods
    QVariantMap params; QVariantMap returns;

    params.clear(); returns.clear();
    setDescription("Hello", "Once connected to this server, a client can get information about the server by saying Hello. "
                            "The response informs the client about the server.");
    setParams("Hello", params);
    returns.insert("server", JsonTypes::basicTypeToString(JsonTypes::String));
    returns.insert("name", JsonTypes::basicTypeToString(JsonTypes::String));
    returns.insert("version", JsonTypes::basicTypeToString(JsonTypes::String));
    returns.insert("apiVersion", JsonTypes::basicTypeToString(JsonTypes::String));
    setReturns("Hello", returns);

    params.clear(); returns.clear();
    setDescription("Introspect", "Introspect this API.");
    setParams("Introspect", params);
    returns.insert("methods", JsonTypes::basicTypeToString(JsonTypes::Object));
    returns.insert("types", JsonTypes::basicTypeToString(JsonTypes::Object));
    returns.insert("notifications", JsonTypes::basicTypeToString(JsonTypes::Object));
    setReturns("Introspect", returns);

    QMetaObject::invokeMethod(this, "setup", Qt::QueuedConnection);
}

JsonRpcServer::~JsonRpcServer()
{
    qCDebug(dcJsonRpc()) << "Shutting down Json RPC server";
    foreach (JsonHandler *handler, m_handlers.values()) {
        unregisterHandler(handler);
    }
}

QString JsonRpcServer::name() const
{
    return "RemoteProxy";
}

JsonReply *JsonRpcServer::Hello(const QVariantMap &params, ProxyClient *proxyClient) const
{
    Q_UNUSED(params)
    Q_UNUSED(proxyClient)

    QVariantMap data;
    data.insert("server", QCoreApplication::applicationName());
    data.insert("name", Engine::instance()->serverName());
    data.insert("version", QCoreApplication::applicationVersion());
    data.insert("apiVersion", API_VERSION_STRING);

    return createReply(data);
}

JsonReply *JsonRpcServer::Introspect(const QVariantMap &params, ProxyClient *proxyClient) const
{
    Q_UNUSED(params)
    Q_UNUSED(proxyClient)

    qCDebug(dcJsonRpc()) << "Introspect called.";

    QVariantMap data;
    data.insert("types", JsonTypes::allTypes());

    QVariantMap methods;
    foreach (JsonHandler *handler, m_handlers) {
        methods.unite(handler->introspect(QMetaMethod::Method));
    }

    data.insert("methods", methods);

    QVariantMap signalsMap;
    foreach (JsonHandler *handler, m_handlers) {
        signalsMap.unite(handler->introspect(QMetaMethod::Signal));
    }

    data.insert("notifications", signalsMap);

    return createReply(data);
}

void JsonRpcServer::sendResponse(ProxyClient *client, int commandId, const QVariantMap &params)
{
    QVariantMap response;
    response.insert("id", commandId);
    response.insert("status", "success");
    response.insert("params", params);

    QByteArray data = QJsonDocument::fromVariant(response).toJson(QJsonDocument::Compact);
    qCDebug(dcJsonRpcTraffic()) << "Sending data:" << data;
    client->interface()->sendData(client->clientId(), data);
}

void JsonRpcServer::sendErrorResponse(ProxyClient *client, int commandId, const QString &error)
{
    QVariantMap errorResponse;
    errorResponse.insert("id", commandId);
    errorResponse.insert("status", "error");
    errorResponse.insert("error", error);

    QByteArray data = QJsonDocument::fromVariant(errorResponse).toJson(QJsonDocument::Compact);
    qCDebug(dcJsonRpcTraffic()) << "Sending data:" << data;
    client->interface()->sendData(client->clientId(), data);
}

QString JsonRpcServer::formatAssertion(const QString &targetNamespace, const QString &method, JsonHandler *handler, const QVariantMap &data) const
{
    QJsonDocument doc = QJsonDocument::fromVariant(handler->introspect(QMetaMethod::Method).value(targetNamespace + "." + method));
    QJsonDocument doc2 = QJsonDocument::fromVariant(data);
    return QString("\nMethod: %1\nTemplate: %2\nValue: %3")
            .arg(targetNamespace + "." + method)
            .arg(QString(doc.toJson(QJsonDocument::Indented)))
            .arg(QString(doc2.toJson(QJsonDocument::Indented)));
}

void JsonRpcServer::registerHandler(JsonHandler *handler)
{
    m_handlers.insert(handler->name(), handler);
    qCDebug(dcJsonRpc()) << "Register handler" << handler->name();
    for (int i = 0; i < handler->metaObject()->methodCount(); ++i) {
        QMetaMethod method = handler->metaObject()->method(i);
        if (method.methodType() == QMetaMethod::Signal && QString(method.name()).contains(QRegExp("^[A-Z]"))) {
            QObject::connect(handler, method, this, metaObject()->method(metaObject()->indexOfSlot("sendNotification(QVariantMap)")));
        }
    }
}

void JsonRpcServer::unregisterHandler(JsonHandler *handler)
{
    m_handlers.remove(handler->name());
    qCDebug(dcJsonRpc()) << "Unregister handler" << handler->name();

    for (int i = 0; i < handler->metaObject()->methodCount(); ++i) {
        QMetaMethod method = handler->metaObject()->method(i);
        if (method.methodType() == QMetaMethod::Signal && QString(method.name()).contains(QRegExp("^[A-Z]"))) {
            QObject::connect(handler, method, this, metaObject()->method(metaObject()->indexOfSlot("sendNotification(QVariantMap)")));
        }
    }
}

void JsonRpcServer::setup()
{
    registerHandler(this);
    registerHandler(new AuthenticationHandler(this));
}

void JsonRpcServer::asyncReplyFinished()
{
    JsonReply *reply = static_cast<JsonReply *>(sender());
    ProxyClient *proxyClient = m_asyncReplies.take(reply);

    reply->deleteLater();

    if (!proxyClient) {
        qCWarning(dcJsonRpc()) << "Got an async reply but the client does not exist any more";
        return;
    }

    if (!reply->timedOut()) {
        Q_ASSERT_X(reply->handler()->validateReturns(reply->method(), reply->data()).first
                   ,"validating return value", formatAssertion(reply->handler()->name(), reply->method(), reply->handler(), reply->data()).toLatin1().data());
        sendResponse(proxyClient, reply->commandId(), reply->data());
        if (!reply->success()) {
            // Disconnect this client since the request was not successfully
            proxyClient->interface()->killClientConnection(proxyClient->clientId());
        }
    } else {
        sendErrorResponse(proxyClient, reply->commandId(), "Command timed out");
        // Disconnect this client since he requested something that created a timeout
        proxyClient->interface()->killClientConnection(proxyClient->clientId());
    }
}

void JsonRpcServer::registerClient(ProxyClient *proxyClient)
{
    qCDebug(dcJsonRpc()) << "Register client" << proxyClient;
    if (m_clients.contains(proxyClient)) {
        qCWarning(dcJsonRpc()) << "Client already registered" << proxyClient;
        return;
    }
    m_clients.append(proxyClient);
}

void JsonRpcServer::unregisterClient(ProxyClient *proxyClient)
{
    qCDebug(dcJsonRpc()) << "Unregister client" << proxyClient;
    if (!m_clients.contains(proxyClient)) {
        qCWarning(dcJsonRpc()) << "Client was not registered" << proxyClient;
        return;
    }

    m_clients.removeAll(proxyClient);
}

void JsonRpcServer::processData(ProxyClient *proxyClient, const QByteArray &data)
{
    if (!m_clients.contains(proxyClient))
        return;

    qCDebug(dcJsonRpcTraffic()) << "Incoming data from" << proxyClient << ": " << data;

    QJsonParseError error;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(data, &error);

    if(error.error != QJsonParseError::NoError) {
        qCWarning(dcJsonRpc) << "Failed to parse JSON data" << data << ":" << error.errorString();
        sendErrorResponse(proxyClient, -1, QString("Failed to parse JSON data: %1").arg(error.errorString()));
        proxyClient->interface()->killClientConnection(proxyClient->clientId());
        return;
    }

    QVariantMap message = jsonDoc.toVariant().toMap();

    bool success = false;
    int commandId = message.value("id").toInt(&success);
    if (!success) {
        qCWarning(dcJsonRpc()) << "Error parsing command. Missing \"id\":" << message;
        sendErrorResponse(proxyClient, -1, "Error parsing command. Missing 'id'");
        proxyClient->interface()->killClientConnection(proxyClient->clientId());
        return;
    }

    QStringList commandList = message.value("method").toString().split('.');
    if (commandList.count() != 2) {
        qCWarning(dcJsonRpc) << "Error parsing method.\nGot:" << message.value("method").toString() << "\nExpected: \"Namespace.method\"";
        sendErrorResponse(proxyClient, commandId, QString("Error parsing method. Got: '%1'', Expected: 'Namespace.method'").arg(message.value("method").toString()));
        proxyClient->interface()->killClientConnection(proxyClient->clientId());
        return;
    }

    QString targetNamespace = commandList.first();
    QString method = commandList.last();

    JsonHandler *handler = m_handlers.value(targetNamespace);
    if (!handler) {
        sendErrorResponse(proxyClient, commandId, "No such namespace");
        proxyClient->interface()->killClientConnection(proxyClient->clientId());
        return;
    }

    if (!handler->hasMethod(method)) {
        sendErrorResponse(proxyClient, commandId, "No such namespace");
        proxyClient->interface()->killClientConnection(proxyClient->clientId());
        return;
    }

    QVariantMap params = message.value("params").toMap();

    QPair<bool, QString> validationResult = handler->validateParams(method, params);
    if (!validationResult.first) {
        sendErrorResponse(proxyClient, commandId,  "Invalid params: " + validationResult.second);
        proxyClient->interface()->killClientConnection(proxyClient->clientId());
        return;
    }

    JsonReply *reply;
    QMetaObject::invokeMethod(handler, method.toLatin1().data(), Q_RETURN_ARG(JsonReply*, reply), Q_ARG(QVariantMap, params), Q_ARG(ProxyClient *, proxyClient));
    if (reply->type() == JsonReply::TypeAsync) {
        m_asyncReplies.insert(reply, proxyClient);
        reply->setClientId(proxyClient->clientId());
        reply->setCommandId(commandId);
        connect(reply, &JsonReply::finished, this, &JsonRpcServer::asyncReplyFinished);
        reply->startWait();
    } else {
        Q_ASSERT_X((targetNamespace == "RemoteProxy" && method == "Introspect") || handler->validateReturns(method, reply->data()).first
                   ,"validating return value", formatAssertion(targetNamespace, method, handler, reply->data()).toLatin1().data());
        reply->setClientId(proxyClient->clientId());
        reply->setCommandId(commandId);
        sendResponse(proxyClient, commandId, reply->data());
    }
}

}
