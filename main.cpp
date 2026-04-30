#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>
#include <cmath>
#include <cstdlib>

// Shared queue between logger thread and gui sender ----------

struct SensorReading {

    qint64 timestamp_ms;
    double temperature;
    double humidity;

};

QList<SensorReading> g_pendingForGUI;
QMutex g_queueMutex;

// Data generator -----------

SensorReading generateReading() {

    static double phase = 0.0;
    phase += 0.01;

    SensorReading r;
    r.timestamp_ms = QDateTime::currentMSecsSinceEpoch();
    r.temperature = 22.0 + 5.0 * std::sin(phase) + (rand() % 100) / 200.0;
    r.humidity = 55.0 + 3.0 * std::cos(phase *0.7) + (rand() % 100) / 300.0;
    return r;
}

// CSV logger ----------
// Opens telemetry_log.csv once and keeps it open for the session

class CsvLogger : public QObject {

    Q_OBJECT
public:
    explicit CsvLogger(const QString& filePath, QObject* parent = nullptr)
        : QObject(parent), m_file(filePath), m_stream(&m_file) {

        if (m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            m_stream << "timestamp_ms, temperature, humidity\n";
            m_stream.flush();
            qInfo() << "[Logger] Writing to" << filePath;
        } else {
            qWarning() << "[Logger] Could not open" << filePath;
        }
    }

    void write(const SensorReading& r) {

        // Only this object touches the file

        m_stream << r.timestamp_ms << ","
                 << QString::number(r.temperature, 'f', 3) << ","
                 << QString::number(r.humidity, 'f', 3) << "\n";
        m_stream.flush();
    }

private :
    QFile m_file;
    QTextStream m_stream;

};

// ---------- TCP Server ----------
// Listens on port 9000 accepting one client at a time
// The GUI-send timer runs at 10Hz and drains pending readings from the shared queue, sends only latest to save badwidth

class SensorServer : public QObject {

    Q_OBJECT
public:
    explicit SensorServer(CsvLogger* logger, QObject* parent = nullptr)
        : QObject(parent), m_logger(logger), m_client(nullptr) {

        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, &SensorServer::onNewConnection);

        if (!m_server->listen(QHostAddress::Any, 9000)) {
            qWarning() << "[Server] Failed to listen:" << m_server->errorString();
        } else {
            qInfo() << "[Server] Listening on post 9000";
        }

        // ----------100Hz data generator & CSV logger ----------
        m_logTimer = new QTimer(this);
        connect(m_logTimer, &QTimer::timeout, this, &SensorServer::onLogTick);
        m_logTimer->start(10); // 10ms == 100Hz

        // ----------10Hz GUI sender ---------
        m_sendTimer = new QTimer(this);
        connect(m_sendTimer, &QTimer::timeout, this, &SensorServer::onSendTick);
        m_sendTimer->start(100); // 100ms == 10Hz
    }

private slots:
    void onLogTick() {

        // Runs at 100Hz: generate, log to CSV, que for GUI
        SensorReading r = generateReading();
        m_logger->write(r); // always write to csv

        QMutexLocker lock(&g_queueMutex);
        g_pendingForGUI.append(r); // que for the 10Hz sender
    }

    void onSendTick() {

        // Runs at 10Hz - send only if client is connected
        if (!m_client || !m_client->isOpen()) return;

        QMutexLocker lock(&g_queueMutex);
        if (g_pendingForGUI.isEmpty()) return;

        // Send only the most recent reading, saving bandwidth
        SensorReading latest = g_pendingForGUI.last();
        g_pendingForGUI.clear();
        lock.unlock();

        // Format "TEMP:22.413,HUM:56.200\n"
        QString payload = QString("TEMP:%1,HUM%2\n")
                              .arg(latest.temperature, 0, 'f', 3)
                              .arg(latest.humidity, 0, 'f', 3);

        m_client->write(payload.toUtf8());
    }

    void onNewConnection() {

        if (m_client) {
            m_client->disconnectFromHost();
            m_client->deleteLater();

        }
        m_client = m_server->nextPendingConnection();
        connect(m_client, &QTcpSocket::disconnected, this, [this]() {
            qInfo() << "[Server] Client disconnected";
            m_client->deleteLater();
            m_client = nullptr;

        });
        qInfo() << "[Server] Client connected:" << m_client->peerAddress().toString();

    }

private:
    QTcpServer* m_server;
    QTcpSocket* m_client;
    CsvLogger* m_logger;
    QTimer* m_logTimer;
    QTimer* m_sendTimer;
};

// ----------- Main ----------

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Set up code that uses the Qt event loop here.
    // Call QCoreApplication::quit() or QCoreApplication::exit() to quit the application.
    // A not very useful example would be including
    // #include <QTimer>
    // near the top of the file and calling
    // QTimer::singleShot(5000, &a, &QCoreApplication::quit);
    // which quits the application after 5 seconds.

    // If you do not need a running Qt event loop, remove the call
    // to QCoreApplication::exec() or use the Non-Qt Plain C++ Application template.




    CsvLogger logger("telemetry_log.csv");
    SensorServer server(&logger);

    qInfo() << "[Main] Server running. Press Ctrl+C to exit.";

    return app.exec();
}