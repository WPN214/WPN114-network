#pragma once

#include <QObject>
#include <QQmlParserStatus>
#include <QQmlPropertyValueSource>
#include <QVariant>
#include <QVector>
#include <QDataStream>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QJsonObject>
#include <QJsonDocument>
#include <QtDebug>
#include <QQmlEngine>

#include <source/tree.hpp>
#include <dependencies/mongoose/mongoose.h>
#include <vector>
#include <thread>

#define CSTR(_qstring) _qstring.toStdString().c_str()

//-------------------------------------------------------------------------------------------------
template<typename _Valuetype> void
from_stream(QVariantList& arglst, QDataStream& stream);

//-------------------------------------------------------------------------------------------------
template<> void
from_stream<QString>(QVariantList& arglist, QDataStream& stream);

//=================================================================================================
struct OSCMessage
//=================================================================================================
{
    QString
    m_method;

    QVariant
    m_arguments;

    //---------------------------------------------------------------------------------------------
    OSCMessage() {}

    //---------------------------------------------------------------------------------------------
    OSCMessage(QString method, QVariant arguments) :
        m_method(method), m_arguments(arguments) {}

    //---------------------------------------------------------------------------------------------
    QByteArray
    encode() const
    //---------------------------------------------------------------------------------------------
    {
        QByteArray data;
        QString tt = typetag(m_arguments).prepend(',');
        data.append(m_method);

        auto pads = 4-(m_method.count()%4);
        while (pads--) data.append((char)0);

        data.append(tt);
        pads = 4-(tt.count()%4);

        while (pads--) data.append((char)0);
        append(data, m_arguments);

        return data;
    }

    //---------------------------------------------------------------------------------------------
    OSCMessage(QByteArray const& data);

    //---------------------------------------------------------------------------------------------
    QString
    typetag(QVariant const& argument) const
    //---------------------------------------------------------------------------------------------
    {
        switch (argument.type()) {
            case QVariant::Bool: return argument.value<bool>() ? "T" : "F";
            case QVariant::Int: return "i";
            case QVariant::Double: return "f";
            case QVariant::String: return "s";
            case QVariant::Vector2D: return "ff";
            case QVariant::Vector3D: return "fff";
            case QVariant::Vector4D: return "ffff";
        }

        if (argument.type() == QVariant::List ||
            strcmp(argument.typeName(), "QJSValue") == 0)
        {
            // if argument is QVariantList or QJSValue
            // recursively parse arguments
            QString tag;
            for (const auto& sub: argument.toList())
                 tag.append(OSCMessage::typetag(sub));
            return tag;
        }

        return "N";
    }

    //---------------------------------------------------------------------------------------------
    void
    append(QByteArray& data, QVariant const& argument) const
    // parse an OSC argument, integrate it with a byte array
    //---------------------------------------------------------------------------------------------
    {
        QDataStream stream(&data, QIODevice::ReadWrite);
        stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
        stream.skipRawData(data.size());

        switch(argument.type())
        {
        case QVariant::Bool:        stream << argument.value<bool>(); break;
        case QVariant::Int:         stream << argument.value<int>(); break;
        case QVariant::Double:      stream << argument.value<float>(); break;
        case QVariant::Vector2D:    stream << argument.value<QVector2D>(); break;
        case QVariant::Vector3D:    stream << argument.value<QVector3D>(); break;
        case QVariant::Vector4D:    stream << argument.value<QVector4D>(); break;

        case QVariant::String: {
            QByteArray str = argument.toString().toUtf8();
            auto pads = 4-(str.count()%4);
            while (pads--) str.append('\0');
            data.append(str);
            return;
        }
        case QVariant::List: {
            for (const auto& sub : argument.value<QVariantList>())
                 OSCMessage::append(data, sub);
        }
        }
    }
};

//=================================================================================================
class Connection : public QObject
//=================================================================================================
{
    Q_OBJECT

    mg_connection*
    m_udp_connection = nullptr;

    mg_connection*
    m_ws_connection = nullptr;

    mg_mgr
    m_mgr;

    uint16_t
    m_udp_port = 0;

    QString
    m_host_ip,
    m_host_udp;

public:

    //---------------------------------------------------------------------------------------------
    Connection() {}

    //---------------------------------------------------------------------------------------------
    Connection(mg_connection* ws_connection) :
        m_ws_connection(ws_connection)
    //---------------------------------------------------------------------------------------------
    {
        char addr[32], port[5];
        mg_sock_addr_to_str(&ws_connection->sa, addr, sizeof(addr), MG_SOCK_STRINGIFY_IP);
        mg_sock_addr_to_str(&ws_connection->sa, addr, sizeof(addr), MG_SOCK_STRINGIFY_PORT);
        m_host_ip = addr;
        m_host_ip.append(":");
        m_host_ip.append(port);
    }

    //---------------------------------------------------------------------------------------------
    Connection(Connection const& cp) :
        m_ws_connection     (cp.m_ws_connection),
        m_udp_connection    (cp.m_udp_connection),
        m_udp_port          (cp.m_udp_port),
        m_host_ip           (cp.m_host_ip) {}

    //---------------------------------------------------------------------------------------------
    virtual
    ~Connection() override{}

    //---------------------------------------------------------------------------------------------
    Connection&
    operator=(Connection const& cp)
    //---------------------------------------------------------------------------------------------
    {
        m_ws_connection     = cp.m_ws_connection;
        m_udp_connection    = cp.m_udp_connection;
        m_udp_port          = cp.m_udp_port;
        m_host_ip           = cp.m_host_ip;

        return *this;
    }

    //---------------------------------------------------------------------------------------------
    mg_connection*
    mgc() { return m_ws_connection; }

    //---------------------------------------------------------------------------------------------
    void
    set_udp(uint16_t udp)
    //---------------------------------------------------------------------------------------------
    {
        m_udp_port = udp;
        m_host_udp = m_host_ip;
        m_host_udp.prepend("udp://");
        m_host_udp.append(":");
        m_host_udp.append(QString::number(m_udp_port));

        mg_mgr_init(&m_mgr, this);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE
    QString address() const { return m_host_ip; }

    //-------------------------------------------------------------------------------------------------
    Q_SLOT void
    on_value_changed(QVariant value)
    //-------------------------------------------------------------------------------------------------
    {
        auto node = qobject_cast<Node*>(QObject::sender());
        writeOSC(node->path(), value.toList(), node->critical());
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    writeOSC(QString method, QVariantList arguments, bool critical = false)
    //-------------------------------------------------------------------------------------------------
    {
        OSCMessage msg(method, arguments);
        auto b_arr = msg.encode();

        if (critical) {
             mg_send_websocket_frame(m_ws_connection, WEBSOCKET_OP_BINARY,
             b_arr.data(), b_arr.count());
        } else {
            m_udp_connection = mg_connect(&m_mgr, m_host_udp.toStdString().c_str(), nullptr);
            mg_send(m_udp_connection, b_arr.data(), b_arr.count());
        }
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    writeText(QString text)
    //-------------------------------------------------------------------------------------------------
    {
        auto utf8 = text.toUtf8();
        mg_send_websocket_frame(m_ws_connection, WEBSOCKET_OP_TEXT, utf8.data(), utf8.count());
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    writeJson(QJsonObject object)
    //-------------------------------------------------------------------------------------------------
    {
        auto doc = QJsonDocument(object).toJson(QJsonDocument::Compact);
        mg_send_websocket_frame(m_ws_connection, WEBSOCKET_OP_TEXT, doc.data(), doc.count());
    }
};

Q_DECLARE_METATYPE(Connection)
Q_DECLARE_METATYPE(mg_connection*)
Q_DECLARE_METATYPE(http_message*)
Q_DECLARE_METATYPE(websocket_message*)

#include <dependencies/qzeroconf/qzeroconf.h>

//=================================================================================================
class NetworkDevice : public QObject, public QQmlParserStatus
//=================================================================================================
{
    Q_OBJECT

    Q_PROPERTY      (QQmlListProperty<Node> subnodes READ subnodes)
    Q_INTERFACES    (QQmlParserStatus)
    Q_CLASSINFO     ("DefaultProperty", "subnodes")

protected:

    Tree
    m_tree;

    QZeroConf
    m_zeroconf;

public:

    //---------------------------------------------------------------------------------------------
    void
    classBegin() override {}

    //---------------------------------------------------------------------------------------------
    void
    componentComplete() override {}

    //---------------------------------------------------------------------------------------------
    Q_INVOKABLE Node*
    get(QString path) { return m_tree.find(path); }

    Q_INVOKABLE Tree*
    tree() { return &m_tree; }

    //---------------------------------------------------------------------------------------------
    Q_INVOKABLE QVariant
    value(QString path)
    //---------------------------------------------------------------------------------------------
    {
        if  (auto node = m_tree.find(path))
             return node->value();
        else return QVariant();
    }

    // --------------------------------------------------------------------------------------------
    QQmlListProperty<Node>
    subnodes()
    // returns subnodes (QML format)
    // --------------------------------------------------------------------------------------------
    {
        return QQmlListProperty<Node>(
               this, this,
               &NetworkDevice::append_subnode,
               &NetworkDevice::nsubnodes,
               &NetworkDevice::subnode,
               &NetworkDevice::clear_subnodes);
    }

    // --------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    append_subnode(Node* node) { m_tree.link(node); }
    // appends a subnode to this Node children

    // --------------------------------------------------------------------------------------------
    Q_INVOKABLE int
    nsubnodes() { return m_tree.root()->nsubnodes(); }
    // returns this Node' subnodes count

    // --------------------------------------------------------------------------------------------
    Q_INVOKABLE Node*
    subnode(int index) { return m_tree.root()->subnode(index); }
    // returns this Node' subnode at index

    // --------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    clear_subnodes() {  }

    // --------------------------------------------------------------------------------------------
    static void
    append_subnode(QQmlListProperty<Node>* list, Node* node)
    // static Qml version, see above
    {
        reinterpret_cast<NetworkDevice*>(list->data)->append_subnode(node);
    }

    // --------------------------------------------------------------------------------------------
    static int
    nsubnodes(QQmlListProperty<Node>* list)
    // static Qml version, see above
    {
        return reinterpret_cast<NetworkDevice*>(list)->nsubnodes();
    }

    // --------------------------------------------------------------------------------------------
    static Node*
    subnode(QQmlListProperty<Node>* list, int index)
    // static Qml version, see above
    {
        return reinterpret_cast<NetworkDevice*>(list)->subnode(index);
    }

    // --------------------------------------------------------------------------------------------
    static void
    clear_subnodes(QQmlListProperty<Node>* list)
    // static Qml version, see above
    {
        reinterpret_cast<NetworkDevice*>(list)->clear_subnodes();
    }

};

//-------------------------------------------------------------------------------------------------
static QJsonObject
ServerExtensions =
//-------------------------------------------------------------------------------------------------
{
    { "ACCESS", false },
    { "VALUE", true },
    { "RANGE", false },
    { "DESCRIPTION", false },
    { "TAGS", false },
    { "EXTENDED_TYPE", true },
    { "UNIT", false },
    { "CRITICAL", true },
    { "CLIPMODE", false },
    { "LISTEN", true },
    { "PATH_CHANGED", false },
    { "PATH_REMOVED", true },
    { "PATH_ADDED", true },
    { "PATH_RENAMED", false },
    { "OSC_STREAMING", true },
    { "HTML", false },
    { "ECHO", false }
};

//=================================================================================================
class Server : public NetworkDevice
//=================================================================================================
{
    Q_OBJECT

    Q_PROPERTY (int tcp READ tcp WRITE set_tcp)
    Q_PROPERTY (int udp READ udp WRITE set_udp)
    Q_PROPERTY (QString name READ name WRITE set_name) // zeroconf
    Q_PROPERTY (bool singleton READ singleton WRITE set_singleton)

    std::vector<Connection>
    m_connections;

    mg_connection
    *m_tcp_connection = nullptr,
    *m_udp_connection = nullptr;

    mg_mgr
    m_tcp,
    m_udp;

    uint16_t
    m_tcp_port = 5678,
    m_udp_port = 1234;

    std::thread
    m_mgthread;

    bool
    m_running = false;

    QString
    m_name = "wpn114";

public:

    //-------------------------------------------------------------------------------------------------
    Q_SIGNAL void
    connection(Connection connection);

    Q_SIGNAL void
    disconnection(Connection connection);

    Q_SIGNAL void
    oscMessageReceived(OSCMessage message);

    Q_SIGNAL void
    httpRequestReceived(QString request);

    Q_SIGNAL void
    websocketMessageReceived(QString message);

    //-------------------------------------------------------------------------------------------------
    Server()
    //-------------------------------------------------------------------------------------------------
    {
        mg_mgr_init(&m_tcp, this);
        mg_mgr_init(&m_udp, this);
    }

    //-------------------------------------------------------------------------------------------------
    void
    componentComplete() override
    //-------------------------------------------------------------------------------------------------
    {
        char s_tcp[5], s_udp[5], udp_hdr[32] = "udp://";

        sprintf(s_tcp, "%d", m_tcp_port);
        sprintf(s_udp, "%d", m_udp_port);
        strcat(udp_hdr, s_udp);

        m_tcp_connection = mg_bind(&m_tcp, s_tcp, ws_event_handler);
        m_udp_connection = mg_bind(&m_udp, udp_hdr, udp_event_handler);
        mg_set_protocol_http_websocket(m_tcp_connection);

        m_zeroconf.startServicePublish(CSTR(m_name), "_oscjson._tcp", "local", m_tcp_port);
        m_running = true;

        poll();
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    stop()
    //-------------------------------------------------------------------------------------------------
    {
        m_running = false;
        m_mgthread.join();
    }

    //-------------------------------------------------------------------------------------------------
    virtual
    ~Server() override
    //-------------------------------------------------------------------------------------------------
    {
        stop();
        mg_mgr_free(&m_tcp);
        mg_mgr_free(&m_udp);
    }

    //-------------------------------------------------------------------------------------------------
    void
    poll()
    //-------------------------------------------------------------------------------------------------
    {
        m_mgthread = std::thread(&Server::server_poll, this);
    }

    //-------------------------------------------------------------------------------------------------
    void
    server_poll()
    //-------------------------------------------------------------------------------------------------
    {
        while (m_running) {
            mg_mgr_poll(&m_tcp, 200);
            mg_mgr_poll(&m_udp, 200);
        }
    }

    //-------------------------------------------------------------------------------------------------
    static void
    ws_event_handler(mg_connection* mgc, int event, void* data)
    //-------------------------------------------------------------------------------------------------
    {
        auto server = static_cast<Server*>(mgc->mgr->user_data);

        switch(event)
        {
        case MG_EV_RECV:
        {
            break;
        }
        case MG_EV_WEBSOCKET_HANDSHAKE_DONE:
        {
            QMetaObject::invokeMethod(server, "on_connection",
                Qt::QueuedConnection, Q_ARG(mg_connection*, mgc));
            break;
        }
        case MG_EV_WEBSOCKET_FRAME:
        {
            QMetaObject::invokeMethod(server, "on_websocket_frame",
                Qt::QueuedConnection,
                Q_ARG(mg_connection*, mgc),
                Q_ARG(websocket_message*, static_cast<websocket_message*>(data)));
            break;
        }
        case MG_EV_HTTP_REQUEST:
        {
            http_message* hm = static_cast<http_message*>(data);
            auto uri = QString::fromUtf8(hm->uri.p, hm->uri.len);
            auto qst = QString::fromUtf8(hm->query_string.p, hm->query_string.len);

            QMetaObject::invokeMethod(server, "on_http_request",
                Qt::QueuedConnection,
                Q_ARG(mg_connection*, mgc),
                Q_ARG(QString, uri),
                Q_ARG(QString, qst));

            break;
        }
        case MG_EV_CLOSE:
        {
            QMetaObject::invokeMethod(server, "on_disconnection",
                Qt::QueuedConnection,
                Q_ARG(mg_connection*, mgc));
            break;
        }
        }
    }

    //-------------------------------------------------------------------------------------------------
    static void
    udp_event_handler(mg_connection* mgc, int event, void* data)
    //-------------------------------------------------------------------------------------------------
    {
        auto server = static_cast<Server*>(mgc->mgr->user_data);

        switch(event) {
            case MG_EV_RECV:
            QMetaObject::invokeMethod(server, "on_udp_datagram",
                Qt::QueuedConnection,
                Q_ARG(mg_connection*, mgc));
                break;
        }
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_connection(mg_connection* con)
    // this method dwells in the qt thread, it has to be invoked with a queued connection
    // from the mgr callback
    //-------------------------------------------------------------------------------------------------
    {
        m_connections.emplace_back(con);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_disconnection(mg_connection* connection)
    //-------------------------------------------------------------------------------------------------
    {

    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_websocket_frame(mg_connection* mgc, websocket_message* message)
    //-------------------------------------------------------------------------------------------------
    {
        QByteArray frame(reinterpret_cast<const char*>(message->data),
                         message->size);

        if (message->flags & WEBSOCKET_OP_TEXT)
        {
            // it would have to be json
            auto doc = QJsonDocument::fromJson(frame);
            auto obj = doc.object();

            auto command = obj["COMMAND"].toString();

            Connection* sender = nullptr;
            for (auto& connection : m_connections)
                if (connection.mgc() == mgc)
                    sender = &connection;

            assert(sender);

            if (command == "LISTEN" || command == "IGNORE")
            {
                auto target = obj["DATA"].toString();

                if (auto node = m_tree.find(target)) {
                    if (command == "LISTEN")
                         QObject::connect(node, &Node::valueChanged, sender, &Connection::on_value_changed);
                    else QObject::disconnect(node, &Node::valueChanged, sender, &Connection::on_value_changed);
                }
            }

            else if (command == "START_OSC_STREAMING") {
                uint16_t port = obj["DATA"].toObject()["LOCAL_SERVER_PORT"].toInt();
                sender->set_udp(port);                

                // at this point it is safe to validate the oscquery connection
                // and send it back to qml
                emit connection(*sender);
            }

            emit websocketMessageReceived(frame);
        }

        else if (message->flags & WEBSOCKET_OP_BINARY) {
            // it would have to be OSC
            OSCMessage oscmg(frame);

            if (auto node = m_tree.find(oscmg.m_method))
                node->set_value(oscmg.m_arguments);

            emit oscMessageReceived(oscmg);
        }
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_udp_datagram(mg_connection* connection)
    //-------------------------------------------------------------------------------------------------
    {
        QByteArray cdg(connection->recv_mbuf.buf,
                       connection->recv_mbuf.len);

        OSCMessage msg(cdg);
        emit oscMessageReceived(msg);
    }

    //-------------------------------------------------------------------------------------------------
    QJsonObject const
    info() const
    //-------------------------------------------------------------------------------------------------
    {
        QJsonObject info  {
            { "NAME", m_name },
            { "OSC_PORT", m_udp_port },
            { "OSC_TRANSPORT", "UDP" },
            { "EXTENSIONS", ServerExtensions }
        };

        return info;
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_http_request(mg_connection* connection, QString uri, QString query)
    //-------------------------------------------------------------------------------------------------
    {
        if (query == "HOST_INFO") {
            QJsonDocument doc(info());
            auto ba = doc.toJson(QJsonDocument::Compact);
            mg_send_head(connection, 200, ba.count(), "Content-Type: application/json; charset=utf-8");
            mg_send(connection, ba.data(), ba.count());
        }
        else
        {
            // query root
            QJsonDocument doc(m_tree.query(uri));
            auto ba = doc.toJson(QJsonDocument::Compact);
            mg_send_head(connection, 200, ba.count(), "Content-Type: application/json; charset=utf-8");
            mg_send(connection, ba.data(), ba.count());
        }

        emit httpRequestReceived(uri+query);
    }

    //-------------------------------------------------------------------------------------------------
    Q_SLOT void
    on_node_added(Node* node)
    //-------------------------------------------------------------------------------------------------
    {
        if (m_connections.empty())
            return;

        QJsonObject command, data;
        command.insert("COMMAND", "PATH_ADDED");
        data.insert(node->name(), node->to_json());
        command.insert("DATA", data);

        for (auto& connection : m_connections)
            connection.writeJson(command);
    }

    //-------------------------------------------------------------------------------------------------
    Q_SLOT void
    on_node_removed(Node* node)
    //-------------------------------------------------------------------------------------------------
    {
        if (m_connections.empty())
            return;

        QJsonObject command;
        command.insert("COMMAND", "PATH_REMOVED");
        command.insert("DATA", node->path());

        for (auto& connection : m_connections)
            connection.writeJson(command);
    }

    //-------------------------------------------------------------------------------------------------
    bool
    running() const { return m_running; }

    //-------------------------------------------------------------------------------------------------
    bool
    singleton() const { return m_tree.singleton(); }

    //-------------------------------------------------------------------------------------------------
    QString
    name() const { return m_name; }

    //-------------------------------------------------------------------------------------------------
    uint16_t
    tcp() const { return m_tcp_port; }

    uint16_t
    udp() const { return m_udp_port; }

    //-------------------------------------------------------------------------------------------------
    void
    set_singleton(bool singleton) { m_tree.set_singleton(singleton); }

    //-------------------------------------------------------------------------------------------------
    void
    set_tcp(uint16_t port)
    {
        m_tcp_port = port;
    }

    //-------------------------------------------------------------------------------------------------
    void
    set_udp(uint16_t port)
    {
        m_udp_port = port;
    }

    //-------------------------------------------------------------------------------------------------
    void
    set_name(QString name)
    {
        m_name = name;
    }
};

//=================================================================================================
class Client : public NetworkDevice
//=================================================================================================
{
    Q_OBJECT

    Q_PROPERTY   (QString host READ host WRITE set_host)
    Q_PROPERTY   (int port READ port WRITE set_port)

    Q_INTERFACES (QQmlParserStatus)

    Connection
    m_connection;

    std::thread
    m_thread;

    mg_mgr
    m_mgr;

    QString
    m_host;

    uint16_t
    m_port = 0;

    bool
    m_running = false;

public:

    //-------------------------------------------------------------------------------------------------
    Q_SIGNAL void
    connected();

    Q_SIGNAL void
    disconnected();

    //-------------------------------------------------------------------------------------------------
    Client()
    //-------------------------------------------------------------------------------------------------
    {
        mg_mgr_init(&m_mgr, this);
        mg_bind(&m_mgr, "udp://1234", event_handler);
    }        

    //-------------------------------------------------------------------------------------------------
    virtual
    ~Client() override
    //-------------------------------------------------------------------------------------------------
    {
        stop();
        mg_mgr_free(&m_mgr);
    }

    //-------------------------------------------------------------------------------------------------
    void
    stop()
    //-------------------------------------------------------------------------------------------------
    {
        m_running = false;
        m_thread.join();
    }

    //-------------------------------------------------------------------------------------------------
    QString
    host() const { return m_host; }

    uint16_t
    port() const { return m_port; }

    //-------------------------------------------------------------------------------------------------
    void
    set_host(QString host)
    //-------------------------------------------------------------------------------------------------
    {
        m_host = host;
    }

    //-------------------------------------------------------------------------------------------------
    void
    set_port(uint16_t port)
    //-------------------------------------------------------------------------------------------------
    {
        m_port = port;
    }

    //-------------------------------------------------------------------------------------------------
    virtual void
    componentComplete() override
    //-------------------------------------------------------------------------------------------------
    {
        if (m_host.startsWith("zc://")) {
            QObject::connect(
                &m_zeroconf, &QZeroConf::serviceAdded,
                this, &Client::on_zeroconf_service_added);
            m_zeroconf.startBrowser("_oscjson._tcp");
        } else {
            connect();
        }
    }

    //-------------------------------------------------------------------------------------------------
    void
    on_zeroconf_service_added(QZeroConfService service)
    //-------------------------------------------------------------------------------------------------
    {
        auto host = m_host;
        host.remove("zc://");

        if (service->name() == host) {
            m_host = service->ip().toString();
            m_port = service->port();
            connect();
            m_zeroconf.stopBrowser();
        }
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    connect()
    //-------------------------------------------------------------------------------------------------
    {        
        if (m_host.startsWith("ws://")) {
            if (m_host.contains(":")) {
                m_port = std::stoi(m_host.split("/").last().toStdString());
                m_connection = mg_connect_ws(&m_mgr, event_handler, CSTR(m_host), nullptr, nullptr);
            } else {
                QString addr(m_host);
                addr.append(":");
                addr.append(QString::number(m_port));
                m_connection = mg_connect_ws(&m_mgr, event_handler, CSTR(addr), nullptr, nullptr);
            }
        } else {
            QString addr("ws://");
            addr.append(m_host);

            if (!addr.contains(":")) {
                addr.append(":");
                addr.append(QString::number(m_port));
            }

            m_connection = mg_connect_ws(&m_mgr, event_handler, CSTR(addr), nullptr, nullptr);
        }

        m_running = true;
        m_thread = std::thread(&Client::poll, this);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    request(QString req)
    //-------------------------------------------------------------------------------------------------
    {
        QString addr(m_host);
        addr.append(":");
        addr.append(QString::number(m_port));
        addr.append(req);

        auto mgc = mg_connect_http(&m_mgr, event_handler, addr.toUtf8().data(), nullptr, nullptr);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    listen(QString uri)
    //-------------------------------------------------------------------------------------------------
    {
        QJsonObject command;
        command["COMMAND"] = "LISTEN";
        command["DATA"] = uri;
        m_connection.writeJson(command);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    ignore(QString uri)
    //-------------------------------------------------------------------------------------------------
    {
        QJsonObject command;
        command["COMMAND"] = "IGNORE";
        command["DATA"] = uri;
        m_connection.writeJson(command);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    send(QString uri, QVariant arguments, bool critical)
    //-------------------------------------------------------------------------------------------------
    {
        OSCMessage message(uri, arguments);
        m_connection.writeOSC(uri, arguments.toList(), critical);
    }

    //-------------------------------------------------------------------------------------------------
    void
    poll()
    //-------------------------------------------------------------------------------------------------
    {
        while (m_running)
               mg_mgr_poll(&m_mgr, 200);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_connected()
    // send host info request as well as namespace query
    //-------------------------------------------------------------------------------------------------
    {
        request("/?HOST_INFO");
        request("/");
    }

    //-------------------------------------------------------------------------------------------------
    void
    parse_json(QByteArray const& frame)
    //-------------------------------------------------------------------------------------------------
    {
        auto object = QJsonDocument::fromJson(frame).object();

        if (object.contains("COMMAND"))
        {
            auto type = object["COMMAND"].toString();
            auto data = object["DATA"].toObject();

            if (type == "PATH_ADDED") {
                for (auto& key : data.keys()) {
                    auto objn = data[key].toObject();
                    auto node = m_tree.find_or_create(objn["FULL_PATH"].toString());
                    node->update(objn);
                }
            }

            else if (type == "PATH_REMOVED") {

            }
        }

        else if (object.contains("FULL_PATH"))
        {

        }

        else if (object.contains("OSC_PORT"))
        {
            m_connection.set_udp(object["OSC_PORT"].toInt());

            QJsonObject command, data;

            command.insert  ("COMMAND", "START_OSC_STREAMING");
            data.insert     ("LOCAL_SERVER_PORT", 1234);
            data.insert     ("LOCAL_SENDER_PORT", 0);
            command.insert  ("DATA", data);

            m_connection.writeJson(command);            
            emit connected();
        }

    }

    //-------------------------------------------------------------------------------------------------
    void
    parse_osc(QByteArray const& data)
    //-------------------------------------------------------------------------------------------------
    {
        OSCMessage msg(data);
        if (auto node = m_tree.find(msg.m_method))
            node->set_value(msg.m_arguments);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_http_reply(http_message* reply)
    //-------------------------------------------------------------------------------------------------
    {
        QByteArray body(reply->body.p, reply->body.len);
        parse_json(body);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_websocket_frame(websocket_message* message)
    // for commands/osc messages
    //-------------------------------------------------------------------------------------------------
    {
        QByteArray frame(reinterpret_cast<const char*>(message->data), message->size);

        if (message->flags & WEBSOCKET_OP_TEXT)
            parse_json(frame);

        else if (message->flags & WEBSOCKET_OP_BINARY)
            parse_osc(frame);
    }

    //-------------------------------------------------------------------------------------------------
    Q_INVOKABLE void
    on_udp_datagram(mg_connection* connection)
    //-------------------------------------------------------------------------------------------------
    {
        QByteArray cdg(connection->recv_mbuf.buf,
                       connection->recv_mbuf.len);
        parse_osc(cdg);
    }

    //-------------------------------------------------------------------------------------------------
    static void
    event_handler(mg_connection* mgc, int event, void* data)
    //-------------------------------------------------------------------------------------------------
    {
        auto client = static_cast<Client*>(mgc->mgr->user_data);

        switch(event)
        {
        case MG_EV_WEBSOCKET_HANDSHAKE_DONE:
            QMetaObject::invokeMethod(client, "on_connected", Qt::QueuedConnection);
            break;
        case MG_EV_WEBSOCKET_FRAME:
        {
            auto wm = static_cast<websocket_message*>(data);
            QMetaObject::invokeMethod(client, "on_websocket_frame",
                Qt::QueuedConnection,
                Q_ARG(websocket_message*, wm));
            break;
        }
        case MG_EV_HTTP_REPLY:
        {
            http_message* reply = static_cast<http_message*>(data);
            mgc->flags != MG_F_CLOSE_IMMEDIATELY;

            QMetaObject::invokeMethod(client, "on_http_reply",
                Qt::QueuedConnection,
                Q_ARG(http_message*, reply));
            break;
        }
        }
    }
};
