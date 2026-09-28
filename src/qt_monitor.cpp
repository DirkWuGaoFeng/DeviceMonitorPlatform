// src/qt_monitor.cpp — Qt6 上位机 (做实版): 真实设备(TCP/串口) → 滚动曲线 → 危急值告警 → SQLite
//
// 仅在 -DDMP_BUILD_QT=ON 时构建。依赖 Qt6: Core/Gui/Widgets/Charts/Sql/SerialPort。
// 相比早期骨架的三处升级:
//   1) 数据源可切换: TCP(:port, 接 device_simulator) 或 串口(COMx, 接真实 STM32)
//   2) 曲线真正滚动: 采集与 UI 解耦, QTimer 批量 drain 后按时间窗刷新
//   3) 落库 + 告警横幅 + 状态栏计数(ok/crcErr/drop) 全部接通
#include "dmp/frame_protocol.h"
#include "dmp/acquisition.h"
#include "dmp/storage.h"   // TeeSink

#include <cstdio>

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTcpSocket>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTimer>
#include <QDateTime>
#include <QPainter>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QTabWidget>
#include <QSlider>
#include <QMessageBox>

#include <vector>
#include <QList>
#include <QVector>
#include <QPointF>

// Qt6: 图表类位于全局命名空间 (Qt5 时代的 QtCharts 命名空间已移除)

// ---- SQLite 落库 Sink: 批量事务写入, 提高吞吐 ----
class SqliteSink : public dmp::Sink {
public:
    bool open(const QString& path) {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "dmp_hist");
        db.setDatabaseName(path);
        if (!db.open()) { lastErr_ = db.lastError().text(); return false; }
        QSqlQuery q(db);
        q.exec("CREATE TABLE IF NOT EXISTS samples("
               "ts INTEGER, seq INTEGER, channel INTEGER, type INTEGER, value REAL)");
        q.exec("PRAGMA journal_mode=WAL");
        db_ = db;
        return true;
    }
    void begin() { db_.transaction(); begun_ = true; }
    void onSample(const dmp::Sample& s) override {
        QSqlQuery q(db_);
        q.prepare("INSERT INTO samples VALUES(?,?,?,?,?)");
        q.addBindValue(static_cast<qlonglong>(s.recv_ts));
        q.addBindValue(s.seq);
        q.addBindValue(s.channel);
        q.addBindValue(static_cast<int>(s.type));
        q.addBindValue(s.value);
        q.exec();
    }
    void commit() { if (begun_) { db_.commit(); begun_ = false; } }
    QString error() const { return lastErr_; }
    bool isOpen() const { return db_.isOpen(); }
    // 历史回放: 读指定通道的 (相对秒, 值) 序列 (以首行时间为基准归零)
    QVector<QPointF> loadSeries(int channel) {
        QVector<QPointF> pts;
        QSqlQuery q(db_);
        q.prepare("SELECT ts, value FROM samples WHERE channel=? ORDER BY ts ASC");
        q.addBindValue(channel);
        if (!q.exec()) return pts;
        while (q.next()) pts.append(QPointF(q.value(0).toLongLong() / 1000.0, q.value(1).toDouble()));
        if (!pts.isEmpty()) { double t0 = pts.first().x(); for (auto& p : pts) p.setX(p.x() - t0); }
        return pts;
    }
    int rowCount() const {
        QSqlQuery q(db_);
        if (q.exec("SELECT COUNT(*) FROM samples") && q.next()) return q.value(0).toInt();
        return -1;
    }
private:
    QSqlDatabase db_;
    QString lastErr_;
    bool begun_ = false;
};

// ---- 曲线 Sink: 每通道维护滚动时间窗内的点集, drain 后整体 replace ----
class ChartSink : public dmp::Sink {
public:
    static constexpr int   kMaxPoints = 600;                 // 每通道最多点数
    explicit ChartSink(std::vector<QLineSeries*> series) : series_(std::move(series)) {
        bufs_.resize(series_.size());
    }
    void onSample(const dmp::Sample& s) override {
        if (s.channel >= series_.size()) return;
        if (!t0_) t0_ = s.recv_ts;                            // 首帧作为时间基准
        double x = double(s.recv_ts - *t0_) / 1000.0;         // 相对秒
        auto& buf = bufs_[s.channel];
        buf.append(QPointF(x, s.value));
        if (buf.size() > kMaxPoints) buf.removeFirst();
        if (x > maxX_) maxX_ = x;
    }
    // UI 定时器在 drain 之后调用: 把缓冲刷到曲线并滚动 X 轴
    void present(double windowSec) {
        for (size_t i = 0; i < series_.size(); ++i) series_[i]->replace(bufs_[i]);
        double lo = (maxX_ > windowSec) ? (maxX_ - windowSec) : 0.0;
        double hi = (maxX_ > windowSec) ? maxX_ : windowSec;
        axisX_->setRange(lo, hi);
    }
    void setAxisX(QValueAxis* a) { axisX_ = a; }
    void reset() { for (auto& b : bufs_) b.clear(); t0_.reset(); maxX_ = 0; }
private:
    std::vector<QLineSeries*> series_;
    QVector<QVector<QPointF>> bufs_;
    std::optional<uint64_t>   t0_;
    double                    maxX_ = 0;
    QValueAxis*               axisX_ = nullptr;
};

class MonitorWindow : public QMainWindow {
public:
    MonitorWindow() {
        setWindowTitle("DeviceMonitorPlatform — 上位机");

        auto* central = new QWidget(this);
        auto* root = new QVBoxLayout(central);

        // ---- 顶部: 数据源选择 (TCP / 串口) ----
        auto* bar = new QHBoxLayout();
        srcBox_ = new QComboBox(); srcBox_->addItems({"TCP 模拟器", "串口 (STM32)"});
        hostEdit_ = new QLineEdit("127.0.0.1");
        portEdit_ = new QLineEdit("9000");
        comBox_   = new QComboBox();
        baudEdit_ = new QLineEdit("115200");
        connectBtn_ = new QPushButton("连接");
        bar->addWidget(new QLabel("源:"));
        bar->addWidget(srcBox_);
        bar->addWidget(new QLabel("主机:")); bar->addWidget(hostEdit_);
        bar->addWidget(new QLabel("端口:")); bar->addWidget(portEdit_);
        bar->addWidget(new QLabel("COM:"));  bar->addWidget(comBox_);
        bar->addWidget(new QLabel("波特:")); bar->addWidget(baudEdit_);
        bar->addWidget(connectBtn_);
        root->addLayout(bar);

        status_ = new QLabel("未连接");   // 必须先创建再使用 (曾经空指针崩溃点)
        status_->setStyleSheet("font-weight:bold;");
        root->addWidget(status_);
        // ---- 告警横幅 + 确认(消声) 演练 (IEC C 类: 声光报警 + 人工确认) ----
        auto* alarmRow = new QHBoxLayout();
        alarm_ = new QLabel("");
        alarm_->setStyleSheet("color:white; background:#b00020; padding:6px; font-weight:bold;");
        alarm_->hide();
        ackBtn_ = new QPushButton("确认告警"); ackBtn_->hide();
        alarmRow->addWidget(alarm_, 1);
        alarmRow->addWidget(ackBtn_);
        root->addLayout(alarmRow);

        // ---- 中部: 4 通道滚动曲线 ----
        chart_ = new QChart();
        chart_->setTitle("多通道实时监测");
        const char* names[4] = { "体温℃", "心率bpm", "血氧%", "浓度" };
        for (int i = 0; i < 4; ++i) {
            auto* s = new QLineSeries();
            s->setName(names[i]);
            chart_->addSeries(s);
            series_.push_back(s);
        }
        axisX_ = new QValueAxis(); axisX_->setTitleText("时间 (s)"); axisX_->setRange(0, 30);
        axisY_ = new QValueAxis(); axisY_->setRange(0, 160);
        chart_->addAxis(axisX_, Qt::AlignBottom);
        chart_->addAxis(axisY_, Qt::AlignLeft);
        for (auto* s : series_) { s->attachAxis(axisX_); s->attachAxis(axisY_); }
        auto* live = new QWidget();
        { auto* ll = new QVBoxLayout(live); ll->setContentsMargins(0, 0, 0, 0);
          auto* view = new QChartView(chart_); view->setRenderHint(QPainter::Antialiasing);
          ll->addWidget(view); }
        auto* histPage = new QWidget();
        buildHistoryTab(histPage);
        tabs_ = new QTabWidget();
        tabs_->addTab(live, "实时监测");
        tabs_->addTab(histPage, "历史回放");
        root->addWidget(tabs_, 1);
        setCentralWidget(central);

        // ---- 采集链: tee(曲线 + 落库); 生产在传输回调, 消费在 UI 定时器 ----
        chartSink_.reset(new ChartSink(series_));
        chartSink_->setAxisX(axisX_);
        if (dbSink_.open(QCoreApplication::applicationDirPath() + "/device_history.db")) {
            acq_.setSink(&tee_);
            tee_.add(chartSink_.get()); tee_.add(&dbSink_);
        } else {
            acq_.setSink(chartSink_.get());
            status_->setText(QString("SQLite 不可用: %1").arg(dbSink_.error()));
        }
        acq_.addRule({dmp::TYPE_HEART_RATE,  50.f, 110.f, "心率超出危急范围"});
        acq_.addRule({dmp::TYPE_TEMPERATURE, 35.f, 42.f,  "体温超出范围"});
        acq_.addRule({dmp::TYPE_SPO2,        94.f, 100.f, "血氧偏低"});
        acq_.setAlarmCallback([this](const dmp::AlarmEvent& e) {
            ++alarmCount_;
            bool fresh = acked_;
            if (fresh) { ++unacked_; acked_ = false; }
            alarm_->setText(QString("⚠ %1 (CH%2 = %3)  ·  告警 #%4%5")
                                .arg(QString::fromStdString(e.message))
                                .arg(e.sample.channel)
                                .arg(e.sample.value, 0, 'f', 1)
                                .arg(alarmCount_)
                                .arg(unacked_ > 0 ? QString("  未确认:%1").arg(unacked_) : QString()));
            alarm_->show();
            ackBtn_->show();
            if (soundOn_ && fresh) QApplication::beep();   // 首个未确认告警响一次, 避免刷屏
        });

        connect(comBox_, QOverload<int>::of(&QComboBox::activated),
                [this](int){ fillComPorts(); });
        connect(srcBox_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                [this](int idx){ onSourceChanged(idx); });
        connect(connectBtn_, &QPushButton::clicked, [this]{ toggleConnect(); });
        connect(ackBtn_, &QPushButton::clicked, [this]{ acked_ = true; unacked_ = 0; alarm_->hide(); ackBtn_->hide(); });

        uiTimer_.start(50);   // 20 fps
        connect(&uiTimer_, &QTimer::timeout, [this]{
            dbSink_.begin();
            acq_.drain(256);
            dbSink_.commit();
            chartSink_->present(30.0);
            auto st = acq_.stat();
            status_->setText(QString("已连接 %1  ·  ok=%2  crcErr=%3  drop=%4")
                                 .arg(connectedVia()).arg(st.ok).arg(st.crcErr).arg(acq_.dropped()));
        });

        fillComPorts();
    }

public:
    // 无头/演示钩子: 启动即连 TCP, runMs>0 到时打印解码统计+落库行数并退出 (供 CI 冒烟)
    void demoConnectTcp(const QString& host, const QString& port, int runMs) {
        srcBox_->setCurrentIndex(0);
        hostEdit_->setText(host);
        portEdit_->setText(port);
        toggleConnect();
        if (runMs > 0) QTimer::singleShot(runMs, this, [this]{ dumpStatsAndQuit(); });
    }

    // 演示钩子: 启动即连串口(真实 STM32)。port 为空则用下拉框当前项; 已连则不动作。
    void demoConnectSerial(const QString& port) {
        if (connected_) return;
        srcBox_->setCurrentIndex(1);
        onSourceChanged(1);
        if (!port.isEmpty()) {
            int i = comBox_->findData(port);
            if (i >= 0) comBox_->setCurrentIndex(i);
        }
        toggleConnect();
    }

    // 单进程确定性自测: 不跨进程走 socket, 直接把合成帧喂进真实 Qt 采集管线
    // (Acquisition 解码 -> ChartSink 曲线 -> SqliteSink 落库), 到时打印统计并退出。
    void startSelfTest(int ms) {
        selfTestTimer_ = new QTimer(this);
        QObject::connect(selfTestTimer_, &QTimer::timeout, [this]{
            static uint16_t seq = 0; static int n = 0;
            dmp::Sample s;
            s.seq = seq++; s.channel = static_cast<uint8_t>(n % 4);
            s.type = static_cast<dmp::SampleType>((n % 4) + 1);
            s.value = 36.f + (n % 4) * 12.f + (seq % 5) * 0.1f;
            auto f = dmp::encodeFrame(s);
            acq_.pushBytes(f.data(), f.size(),
                           static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()));
            ++n;
        });
        selfTestTimer_->start(120);
        if (ms > 0) QTimer::singleShot(ms, this, [this]{ dumpStatsAndQuit(); });
    }

    // 完全同步的端到端自检 (不依赖定时器/事件循环): 把 N 帧喂进真实 Qt 对象管线
    // Acquisition 解码 -> drain -> TeeSink -> ChartSink(曲线点) + SqliteSink(落库行),
    // 校各项计数一致后写 qt_demo_stat.txt, 以退出码表示 PASS(0)/FAIL(1)。
    int runVerify() {
        const int N = 50;
        std::vector<uint8_t> stream;
        for (int i = 0; i < N; ++i) {
            dmp::Sample s;
            s.seq = static_cast<uint16_t>(i); s.channel = static_cast<uint8_t>(i % 4);
            s.type = static_cast<dmp::SampleType>((i % 4) + 1); s.value = 37.f + i * 0.1f;
            auto f = dmp::encodeFrame(s);
            stream.insert(stream.end(), f.begin(), f.end());
        }
        acq_.pushBytes(stream.data(), stream.size(), 1000u);
        size_t drained = acq_.drain(1024);
        chartSink_->present(30.0);                         // 把缓冲刷到 QLineSeries
        auto st = acq_.stat();
        int rows = dbSink_.rowCount();
        int pts = 0; for (auto* s : series_) pts += s->count();
        bool pass = (st.ok == (uint64_t)N && drained == (size_t)N && rows == N && pts == N && st.crcErr == 0);
        char line[220];
        std::snprintf(line, sizeof(line),
                      "VERIFY ok=%llu crcErr=%llu drained=%llu db_rows=%d chart_pts=%d N=%d -> %s\n",
                      (unsigned long long)st.ok, (unsigned long long)st.crcErr,
                      (unsigned long long)drained, rows, pts, N, pass ? "PASS" : "FAIL");
        std::printf("%s", line); std::fflush(stdout);
        QString p = QCoreApplication::applicationDirPath() + "/qt_demo_stat.txt";
        if (FILE* f = std::fopen(p.toLocal8Bit().constData(), "w")) { std::fputs(line, f); std::fclose(f); }
        return pass ? 0 : 1;
    }

private:
    void dumpStatsAndQuit() {
        auto st = acq_.stat();
        char line[160];
        std::snprintf(line, sizeof(line), "[qt] ok=%llu crcErr=%llu drop=%llu db_rows=%d\n",
                      (unsigned long long)st.ok, (unsigned long long)st.crcErr,
                      (unsigned long long)acq_.dropped(), dbSink_.rowCount());
        std::printf("%s", line); std::fflush(stdout);
        // 写到 exe 所在绝对目录 (避免启动 CWD 不定导致产物落在别处)
        QString statPath = QCoreApplication::applicationDirPath() + "/qt_demo_stat.txt";
        if (FILE* f = std::fopen(statPath.toLocal8Bit().constData(), "w")) { std::fputs(line, f); std::fclose(f); }
        qApp->quit();
    }

    QString connectedVia() const {
        if (tcp_) return QString("TCP %1:%2").arg(hostEdit_->text()).arg(portEdit_->text());
        if (serial_ && serial_->isOpen()) return QString("串口 %1@%2").arg(serial_->portName()).arg(baudEdit_->text());
        return "—";
    }

    void fillComPorts() {
        QString cur = comBox_->currentData().toString();
        comBox_->blockSignals(true);
        comBox_->clear();
        for (const auto& info : QSerialPortInfo::availablePorts())
            comBox_->addItem(QString("%1 (%2)").arg(info.portName(), info.description()), info.portName());
        int i = comBox_->findData(cur); if (i >= 0) comBox_->setCurrentIndex(i);
        comBox_->blockSignals(false);
    }

    void onSourceChanged(int idx) {
        bool tcp = (idx == 0);
        hostEdit_->setEnabled(tcp); portEdit_->setEnabled(tcp);
        comBox_->setEnabled(!tcp);  baudEdit_->setEnabled(!tcp);
    }

    void disconnectAll() {
        if (tcp_) { tcp_->disconnectFromHost(); tcp_->deleteLater(); tcp_ = nullptr; }
        if (serial_) { if (serial_->isOpen()) serial_->close(); serial_->deleteLater(); serial_ = nullptr; }
    }

    void toggleConnect() {
        if (connected_) { disconnectAll(); connected_ = false; connectBtn_->setText("连接"); chartSink_->reset(); return; }
        disconnectAll();
        auto onBytes = [this](const QByteArray& b) {
            if (b.isEmpty()) return;
            acq_.pushBytes(reinterpret_cast<const uint8_t*>(b.constData()),
                           static_cast<size_t>(b.size()),
                           static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()));
        };
        if (srcBox_->currentIndex() == 0) {          // TCP
            tcp_ = new QTcpSocket(this);
            QObject::connect(tcp_, &QTcpSocket::readyRead, this, [this, onBytes]{ onBytes(tcp_->readAll()); });
            QObject::connect(tcp_, &QTcpSocket::connected, this, [this]{ connectBtn_->setText("断开"); connected_ = true; });
            QObject::connect(tcp_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
                status_->setText("TCP 错误: " + tcp_->errorString());
            });
            tcp_->connectToHost(hostEdit_->text(), static_cast<uint16_t>(portEdit_->text().toUShort()));
        } else {                                     // Serial
            serial_ = new QSerialPort(this);
            QString com = comBox_->currentData().toString();
            if (com.isEmpty()) { status_->setText("未找到串口"); return; }
            serial_->setPortName(com);
            serial_->setBaudRate(baudEdit_->text().toInt());
            serial_->setDataBits(QSerialPort::Data8);
            serial_->setParity(QSerialPort::NoParity);
            serial_->setStopBits(QSerialPort::OneStop);
            serial_->setFlowControl(QSerialPort::NoFlowControl);
            if (!serial_->open(QIODevice::ReadOnly)) {
                status_->setText("串口打开失败: " + serial_->errorString()); return;
            }
            QObject::connect(serial_, &QSerialPort::readyRead, this, [this, onBytes]{ onBytes(serial_->readAll()); });
            connectBtn_->setText("断开"); connected_ = true;
        }
    }

    void applyHistWindow(double left) {
        if (!histAxisX_) return;
        histAxisX_->setRange(left, left + kHistWindow);
        histAxisY_->setRange(0, 160);
    }

    void loadHistory() {
        if (!dbSink_.isOpen()) { if (histInfo_) histInfo_->setText("数据库未打开, 无法回放"); return; }
        const int ch = histChan_->currentIndex();
        QVector<QPointF> pts = dbSink_.loadSeries(ch);
        histSeries_->replace(pts);
        histSpan_ = pts.isEmpty() ? 0.0 : pts.last().x();
        histSlider_->blockSignals(true);
        histSlider_->setRange(0, qMax(0, static_cast<int>(histSpan_)));
        histSlider_->setValue(0);
        histSlider_->blockSignals(false);
        applyHistWindow(0);
        histInfo_->setText(QString("通道 %1 · 历史点 %2 · 跨度 %3 s · 总落库 %4 行 · 窗口 %5 s")
                               .arg(ch).arg(pts.size()).arg(histSpan_, 0, 'f', 1)
                               .arg(dbSink_.rowCount()).arg(static_cast<int>(kHistWindow)));
    }

    void buildHistoryTab(QWidget* page) {
        auto* lay = new QVBoxLayout(page);
        auto* bar = new QHBoxLayout();
        bar->addWidget(new QLabel("通道:"));
        histChan_ = new QComboBox(); histChan_->addItems({"0 体温℃", "1 心率bpm", "2 血氧%", "3 浓度"});
        histLoad_ = new QPushButton("载入历史");
        histInfo_ = new QLabel("点击[载入历史]读取 SQLite device_history.db");
        bar->addWidget(histChan_);
        bar->addWidget(histLoad_);
        bar->addWidget(histInfo_, 1);
        lay->addLayout(bar);

        histChart_  = new QChart(); histChart_->setTitle("历史回放 (读自 SQLite)");
        histSeries_ = new QLineSeries(); histSeries_->setName("历史");
        histChart_->addSeries(histSeries_);
        histAxisX_ = new QValueAxis(); histAxisX_->setTitleText("相对时间 (s)"); histAxisX_->setRange(0, kHistWindow);
        histAxisY_ = new QValueAxis(); histAxisY_->setRange(0, 160);
        histChart_->addAxis(histAxisX_, Qt::AlignBottom);
        histChart_->addAxis(histAxisY_, Qt::AlignLeft);
        histSeries_->attachAxis(histAxisX_); histSeries_->attachAxis(histAxisY_);
        auto* hv = new QChartView(histChart_); hv->setRenderHint(QPainter::Antialiasing);
        lay->addWidget(hv, 1);

        auto* srow = new QHBoxLayout();
        srow->addWidget(new QLabel("时间轴:"));
        histSlider_ = new QSlider(Qt::Horizontal); histSlider_->setRange(0, 0);
        srow->addWidget(histSlider_, 1);
        lay->addLayout(srow);

        connect(histLoad_, &QPushButton::clicked, [this]{ loadHistory(); });
        connect(histChan_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int){ loadHistory(); });
        connect(histSlider_, &QSlider::valueChanged, [this](int v){
            applyHistWindow(static_cast<double>(v));
            histInfo_->setText(QString("回放窗口: [%1, %2] s").arg(v).arg(v + static_cast<int>(kHistWindow)));
        });
    }

    // 传输
    QTcpSocket*  tcp_ = nullptr;
    QSerialPort* serial_ = nullptr;
    bool         connected_ = false;

    // UI 控件
    QComboBox* srcBox_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QLineEdit* portEdit_ = nullptr;
    QComboBox* comBox_ = nullptr;
    QLineEdit* baudEdit_ = nullptr;
    QPushButton* connectBtn_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* alarm_  = nullptr;
    QChart* chart_ = nullptr;
    QValueAxis* axisX_ = nullptr;
    QValueAxis* axisY_ = nullptr;
    std::vector<QLineSeries*> series_;
    QTimer uiTimer_;
    QTimer* selfTestTimer_ = nullptr;

    // 采集
    dmp::Acquisition acq_;
    dmp::TeeSink     tee_;
    SqliteSink       dbSink_;
    std::unique_ptr<ChartSink> chartSink_;

    // ---- 告警(确认/计数/消声) ----
    QPushButton* ackBtn_    = nullptr;
    int  alarmCount_ = 0;
    int  unacked_    = 0;
    bool acked_      = true;
    bool soundOn_    = true;

    // ---- 历史回放页 ----
    QTabWidget*   tabs_       = nullptr;
    QComboBox*    histChan_   = nullptr;
    QPushButton*  histLoad_   = nullptr;
    QLabel*       histInfo_   = nullptr;
    QSlider*      histSlider_ = nullptr;
    QChart*       histChart_  = nullptr;
    QLineSeries*  histSeries_ = nullptr;
    QValueAxis*   histAxisX_  = nullptr;
    QValueAxis*   histAxisY_  = nullptr;
    double        histSpan_   = 0;
    static constexpr double kHistWindow = 30.0;   // 回放窗口(秒)
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    MonitorWindow w;
    w.resize(980, 640);

    // --verify: 同步自检, 不起事件循环, 直接返回退出码 (CI 友好)
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]) == "--verify") return w.runVerify();
    }
    w.show();

    // 演示/CI 钩子: --demo-tcp <host> <port> <runms>  或  --selftest <ms>
    for (int i = 1; i + 3 < argc; ++i) {
        if (QString(argv[i]) == "--demo-tcp") {
            w.demoConnectTcp(QString(argv[i + 1]), QString(argv[i + 2]), QString(argv[i + 3]).toInt());
            break;
        }
    }
    // --demo-serial [COM口]: 启动即连串口真机 (如 --demo-serial COM4; 省略则用当前下拉项)
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]) == "--demo-serial") {
            const QString port = (i + 1 < argc && QString(argv[i + 1]).startsWith("COM"))
                                     ? QString(argv[i + 1]) : QString();
            w.demoConnectSerial(port);
            break;
        }
    }
    for (int i = 1; i + 1 < argc; ++i) {
        if (QString(argv[i]) == "--selftest") {
            w.startSelfTest(QString(argv[i + 1]).toInt());
            break;
        }
    }
    return app.exec();
}
