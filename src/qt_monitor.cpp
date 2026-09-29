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

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

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
        srcBox_ = new QComboBox(); srcBox_->addItems({"TCP 模拟器", "串口 (STM32)", "网关 (TCP RAW)"});
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
            if (!connected_) return;   // 未进入已连接态(含网关握手中/阈值回读被拒)时不覆盖状态栏提示
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
        // 落库计数只能取"增量": device_history.db 是跨次运行的持久文件, rowCount() 是 COUNT(*)。
        // 早期版本直接拿绝对值与 N 比, 于是这个自检只有在空库时才可能通过 —— 本地复跑与 CI 都必挂。
        const int rowsBefore = dbSink_.isOpen() ? dbSink_.rowCount() : 0;
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
        const int rows = dbSink_.isOpen() ? dbSink_.rowCount() : -1;
        const int newRows = rows - rowsBefore;
        int pts = 0; for (auto* s : series_) pts += s->count();
        bool pass = (st.ok == (uint64_t)N && drained == (size_t)N && rows >= 0 && newRows == N
                    && pts == N && st.crcErr == 0);
        char line[220];
        std::snprintf(line, sizeof(line),
                      "VERIFY ok=%llu crcErr=%llu drained=%llu db_rows=%d(+%d) chart_pts=%d N=%d db=%s -> %s\n",
                      (unsigned long long)st.ok, (unsigned long long)st.crcErr,
                      (unsigned long long)drained, rows, newRows, pts, N,
                      dbSink_.isOpen() ? "open" : "MISSING", pass ? "PASS" : "FAIL");
        std::printf("%s", line); std::fflush(stdout);
        QString p = QCoreApplication::applicationDirPath() + "/qt_demo_stat.txt";
        if (FILE* f = std::fopen(p.toLocal8Bit().constData(), "w")) { std::fputs(line, f); std::fclose(f); }
        return pass ? 0 : 1;
    }

    // 演示钩子: 启动即连网关 (TCP RAW)，选第三项源。host/port 走 hostEdit_/portEdit_。
    void demoConnectGateway(const QString& host, const QString& port) {
        srcBox_->setCurrentIndex(2);
        onSourceChanged(2);
        hostEdit_->setText(host);
        portEdit_->setText(port);
        toggleConnect();
    }

    // --verify-gateway <host> <port> <ms>: T1.2 本轮可复现验收。
    //   双连接隔离混流: raw 腿只订阅 RAW 收字节流, stats 腿只发 STATS 取文本计数。
    //   判据(帧计数等式): 网关 STATS.ok 本轮增量 == Qt 本地解码条数, 且 crc_err==0。
    //   时序对齐: 先握手 RAW 生效(丢弃 +RAW\n 回显但保留其后帧字节) -> 取基线 -> 开窗口喂解码。
    //   无心跳(--hb=0 语义): 上游 simulator 每 200ms 一批帧驱动 ok 增长, 窗口内必有帧否则判 FAIL。
    int runVerifyGateway(const QString& host, const QString& portStr, int runMs) {
        const unsigned short port = static_cast<unsigned short>(portStr.toUShort());
        auto fail = [this](const char* msg) {
            char line[200]; std::snprintf(line, sizeof(line), "VERIFY_GW -> FAIL (%s)\n", msg);
            std::printf("%s", line); std::fflush(stdout); return 1;
        };
        QTcpSocket raw, stats;
        raw.connectToHost(host, port);
        if (!raw.waitForConnected(2000)) return fail("raw 腿连不上网关");
        stats.connectToHost(host, port);
        if (!stats.waitForConnected(2000)) return fail("stats 腿连不上网关");

        // 1) raw 腿握手: 写 RAW\n, 只剥 +RAW 回显行, 行后字节留着喂解码
        raw.write("RAW\n"); raw.flush();
        QByteArray hs;
        while (!hs.contains('\n')) {
            if (!raw.waitForReadyRead(2000)) return fail("未收到 +RAW 回显");
            hs.append(raw.readAll());
        }
        int nl = hs.indexOf('\n');
        if (QString::fromLatin1(hs.left(nl)) != "+RAW") return fail("握手回显非 +RAW");
        QByteArray preRest = hs.mid(nl + 1);          // 回显后可能紧跟帧字节
        if (!preRest.isEmpty())                       // 这批字节已从 socket 读出，不 push 就丢（会凭空少几帧）
            acq_.pushBytes(reinterpret_cast<const uint8_t*>(preRest.constData()), static_cast<size_t>(preRest.size()),
                           static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()));

        // 2) 基线（稳定快照）: raw 已生效，把已到达字节排干后取网关 ok，
        //    与本地解码同起点。用稳定快照避免“在途帧”让基线本身带偏差。
        long long ok0 = stableStatsSnapshot(stats, raw);
        if (ok0 < 0) return fail("STATS 基线读取失败");
        const uint64_t local0 = acq_.stat().ok;   // 稳定快照内部已排干 raw，此处 local 已追平 ok0

        // 3) 窗口: 从 raw 腿收字节流喂解码, 至 runMs 到
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + runMs;
        while (QDateTime::currentMSecsSinceEpoch() < deadline) {
            if (raw.waitForReadyRead(50)) {
                QByteArray b = raw.readAll();
                if (!b.isEmpty())
                    acq_.pushBytes(reinterpret_cast<const uint8_t*>(b.constData()), static_cast<size_t>(b.size()),
                                   static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()));
            }
        }

        // 4) 末值（稳定快照）+ 等式判定：两端均静默后才取值，避免“网关刚解一批、
        //    字节还在我腿里没排干”导致的偶发差 0~4 帧。
        long long ok1 = stableStatsSnapshot(stats, raw);
        if (ok1 < 0) return fail("STATS 末值读取失败");

        // T1.2 §7-1: 阈值权威回读核对 —— 在纯命令的 stats 腿上下发本地 RULE 并回读比对 low/high。
        //   stats 腿不订阅 RAW, 只有文本命令应答, 与 raw 腿互不干扰(无混流)。
        std::string thrDiag;
        const long long thrBad = verifyThresholdReadbackCmd(stats, thrDiag);
        const bool thrOk = (thrBad == 0);

        const auto st = acq_.stat();
        const long long gwDelta = ok1 - ok0;
        const long long localDelta = static_cast<long long>(st.ok) - static_cast<long long>(local0);
        // 帧计数等式：网关 STATS.ok 增量 == 本地解码条数。
        //   允许一个批次(=sim 每轮 4 帧)的边界容差：两条独立 TCP 连接对 5Hz 连续源无法
        //   原子采样，基线/末值各可能差一个在途批次。此容差掩盖不了真实丢字节：慢客户端撕坏
        //   RAW 流会让 local 持续低于 gw 多批、或 torn 帧使 crcErr>0（本地解码器对残帧必报）。
        const long long kBatch = 4;                     // device_simulator 每轮 4 通道 = 一个原子批次
        const long long diff = gwDelta - localDelta;
        bool pass = (st.crcErr == 0) && (localDelta > 0) && (diff <= kBatch && diff >= -kBatch) && thrOk;
        char line[240];
        std::snprintf(line, sizeof(line),
                      "VERIFY_GW gw=%lld:%lld(+%lld) local=%lld:%lld(+%lld) diff=%lld crcErr=%llu thrBad=%lld win=%dms -> %s\n",
                      ok0, ok1, gwDelta, (long long)local0, (long long)st.ok, localDelta,
                      diff, (unsigned long long)st.crcErr, thrBad, runMs,
                      pass ? "PASS" : "FAIL");
        std::printf("%s", line); std::fflush(stdout);
        if (thrBad != 0)          // 阈值回读不一致/协议错: 另起一行给可定位诊断, 不塞进主行(免得撑爆固定缓冲)
            std::printf("VERIFY_GW thr_diag=%s\n", thrBad < 0 ? "protocol-error" : thrDiag.c_str());
        QString p = QCoreApplication::applicationDirPath() + "/qt_gateway_verify.txt";
        if (FILE* f = std::fopen(p.toLocal8Bit().constData(), "w")) { std::fputs(line, f); std::fclose(f); }
        raw.disconnectFromHost(); stats.disconnectFromHost();
        return pass ? 0 : 1;
    }

private:
    // 把 raw 腿已到达的字节排干喂解码；返回本次喂入的字节数。连续 quietMs 内无新字节则返回。
    long long drainQuiet(QTcpSocket& raw, int quietMs) {
        long long bytes = 0;
        while (raw.waitForReadyRead(quietMs)) {
            QByteArray b = raw.readAll();
            if (b.isEmpty()) break;
            bytes += b.size();
            acq_.pushBytes(reinterpret_cast<const uint8_t*>(b.constData()), static_cast<size_t>(b.size()),
                           static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch()));
        }
        return bytes;
    }

    // 稳定快照：反复“排干 -> 读网关 g1 -> 短等排干(计字节数 n) -> 读网关 g2”，
    //   直到 g1==g2 且 n==0。两者同时成立 = 这段区间网关未解新帧且 raw 腿零新字节
    //   → 两端都落在同一个批次间隙（sim 每批间隔 ~190ms >> 往返+短等 ~65ms）。
    //   此刻 g2 所对应的字节已全部送达并被排干入本地解码器 → 网关 ok 与本地解码严格同集。
    //   最多重试 80 次（~几秒）；仍拿不到完全静默快照则返回末次值，交给容差判定，不谎报。
    long long stableStatsSnapshot(QTcpSocket& stats, QTcpSocket& raw) {
        long long last = -1;
        for (int attempt = 0; attempt < 80; ++attempt) {
            drainQuiet(raw, 30);
            long long g1 = readStatsOk(stats);
            if (g1 < 0) return -1;
            long long n = drainQuiet(raw, 60);          // 短等一个 localhost 送达窗口
            long long g2 = readStatsOk(stats);
            if (g2 < 0) return -1;
            last = g2;
            if (g1 == g2 && n == 0) return g2;           // 两端均静默：真正对齐的快照点
        }
        drainQuiet(raw, 30);
        return last < 0 ? readStatsOk(stats) : last;
    }

    // 同步从 stats 腿读一行 STATS, 返回 ok 值 (失败 -1)。stats 腿不订阅, 只会收到命令应答。
    long long readStatsOk(QTcpSocket& s) {
        s.write("STATS\n"); s.flush();
        QByteArray buf;
        while (!buf.contains('\n')) {
            if (!s.waitForReadyRead(2000)) return -1;
            buf.append(s.readAll());
        }
        long long ok = -1, ce = 0, dr = 0;
        if (std::sscanf(buf.constData(), "STATS ok=%lld crc_err=%lld dropped=%lld", &ok, &ce, &dr) != 3) return -1;
        return ok;
    }

    // ---- T1.2 §7-1: 阈值权威回读核对 (Qt 为配置权威) ----
    // 依据本地 acq_.rules() 生成 "RULE <type> <low> <high>\n" 下发串。不带 msg：
    //   回读只比 low/high，且文案里的多字节/空格给行协议匹配添乱没有收益。
    QByteArray buildRulePush() const {
        QByteArray out;
        for (const auto& r : acq_.rules()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "RULE %d %g %g\n",
                          static_cast<int>(r.type), static_cast<double>(r.low), static_cast<double>(r.high));
            out += buf;
        }
        return out;
    }

    // 解析一条 RULES 回读行 "RULE type=%u name=%s low=%.3f high=%.3f msg=%s"。只取 type/low/high。
    //   注: RULE_ACK 行以 "RULE_ACK" 开头(第 5 字符是 '_' 非空格), 不匹配本模式的 "RULE " 前缀,
    //   且调用方已先用 startsWith("RULE type=") 过滤, 不会误喂 ACK 行。
    static bool parseRuleLine(const QByteArray& line, int& type, float& low, float& high) {
        char name[32] = {0}, msg[96] = {0};
        int t = 0; double lo = 0, hi = 0;
        const int n = std::sscanf(line.constData(),
            "RULE type=%d name=%31s low=%lf high=%lf msg=%95s", &t, name, &lo, &hi, msg);
        if (n < 4) return false;                       // 至少要读到 high 才算完整
        type = t; low = static_cast<float>(lo); high = static_cast<float>(hi);
        return true;
    }

    // 把回读集合(seen: type -> low/high)与本地规则逐条比 low/high。只看本地持有的类型
    //   (网关可能有其它类型规则, 与本端权威无关); 本地有而回读缺 => 记不一致。
    //   容差 1e-3 对齐网关 %.3f 打印精度。返回不一致条数, 诊断写入 diag(一致时为空)。
    int verifyThresholdAgainstLocal(const std::map<int, std::pair<float, float>>& seen, std::string& diag) const {
        int bad = 0;
        char buf[160];
        for (const auto& r : acq_.rules()) {
            const int t = static_cast<int>(r.type);
            auto it = seen.find(t);
            if (it == seen.end()) {
                ++bad;
                std::snprintf(buf, sizeof(buf), "type=%d 回读缺失; ", t);
                diag += buf;
                continue;
            }
            const float eps = 1e-3f;
            if (std::fabs(it->second.first - r.low) > eps || std::fabs(it->second.second - r.high) > eps) {
                ++bad;
                std::snprintf(buf, sizeof(buf), "type=%d low回读%.3f≠本地%.3f high回读%.3f≠本地%.3f; ",
                              t, static_cast<double>(it->second.first), static_cast<double>(r.low),
                              static_cast<double>(it->second.second), static_cast<double>(r.high));
                diag += buf;
            }
        }
        return bad;
    }

    // 纯命令腿(不订阅 RAW, 无混流)上做一次端到端阈值回读核对:
    //   下发本地 RULE 集 -> RULES(回读) -> HELP(屏障: 其 "COMMANDS" 应答界定 RULES 块结束,
    //   因为网关按序处理命令, HELP 应答必在全部 RULE 回读行之后到达)。
    //   返回不一致条数(0=一致), -1=协议/超时错。
    long long verifyThresholdReadbackCmd(QTcpSocket& cmd, std::string& diag) {
        QByteArray push = buildRulePush(); push += "RULES\n"; push += "HELP\n";
        cmd.write(push); cmd.flush();
        std::map<int, std::pair<float, float>> seen;
        QByteArray buf;
        for (int guard = 0; guard < 200; ++guard) {
            for (;;) {
                int nl = buf.indexOf('\n');
                if (nl < 0) break;
                QByteArray line = buf.left(nl); buf = buf.mid(nl + 1);
                if (line.startsWith("COMMANDS")) return verifyThresholdAgainstLocal(seen, diag);
                if (line.startsWith("RULE type=")) {
                    int t; float lo, hi;
                    if (parseRuleLine(line, t, lo, hi)) seen[t] = std::make_pair(lo, hi);
                }
            }
            if (!cmd.waitForReadyRead(2000)) return -1;
            buf.append(cmd.readAll());
        }
        return -1;
    }

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
        if (tcp_) {
            if (gwMode_) return QString("网关(TCP RAW) %1:%2").arg(hostEdit_->text()).arg(portEdit_->text());
            return QString("TCP %1:%2").arg(hostEdit_->text()).arg(portEdit_->text());
        }
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
        bool tcp = (idx != 1);                 // TCP 模拟器(0) 与 网关(2) 都用 host/port; 只有串口(1) 用 COM/波特
        hostEdit_->setEnabled(tcp); portEdit_->setEnabled(tcp);
        comBox_->setEnabled(!tcp);  baudEdit_->setEnabled(!tcp);
        if (idx == 2) portEdit_->setText("9100");   // 网关默认监听口 (run_gateway_serial.ps1 -Listen 9100)
    }

    void disconnectAll() {
        if (tcp_) { tcp_->disconnectFromHost(); tcp_->deleteLater(); tcp_ = nullptr; }
        if (serial_) { if (serial_->isOpen()) serial_->close(); serial_->deleteLater(); serial_ = nullptr; }
        gwMode_ = false; gwHandshakeDone_ = false; gwRxBuf_.clear(); gwReadback_.clear();
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
        if (srcBox_->currentIndex() == 0 || srcBox_->currentIndex() == 2) {   // TCP 模拟器 / 网关
            gwMode_ = (srcBox_->currentIndex() == 2);
            tcp_ = new QTcpSocket(this);
            QObject::connect(tcp_, &QTcpSocket::readyRead, this, [this, onBytes]{
                QByteArray b = tcp_->readAll();
                if (gwMode_ && !gwHandshakeDone_) {
                    // 命令态握手: 逐行吃掉 +RAW 之前的 RULE_ACK/RULES 回读文本; 命中 +RAW 后其后字节才是帧流。
                    gwRxBuf_.append(b);
                    for (;;) {
                        int nl = gwRxBuf_.indexOf('\n');
                        if (nl < 0) return;                       // 行还没收全, 等下一段
                        QByteArray line = gwRxBuf_.left(nl);       // 不含 \n
                        gwRxBuf_ = gwRxBuf_.mid(nl + 1);
                        if (line == "+RAW") {
                            gwHandshakeDone_ = true;
                            std::string diag;
                            const int bad = verifyThresholdAgainstLocal(gwReadback_, diag);
                            if (bad > 0) {                        // §7-1: 回读不一致 => 标红 + 拒绝进入已连接态 + 断开
                                status_->setText(QString("网关阈值回读不一致(%1 条): %2 — 拒绝连接")
                                                 .arg(bad).arg(QString::fromStdString(diag)));
                                disconnectAll(); connected_ = false; connectBtn_->setText("连接");
                                return;
                            }
                            connectBtn_->setText("断开"); connected_ = true;
                            if (!gwRxBuf_.isEmpty()) { onBytes(gwRxBuf_); gwRxBuf_.clear(); }
                            return;
                        }
                        if (line.startsWith("RULE type=")) {       // RULES 回读行(RULE_ACK 行不以 "RULE " 开头, 自然被跳过)
                            int t; float lo, hi;
                            if (parseRuleLine(line, t, lo, hi)) gwReadback_[t] = std::make_pair(lo, hi);
                        }
                    }
                }
                onBytes(b);
            });
            QObject::connect(tcp_, &QTcpSocket::connected, this, [this]{
                if (gwMode_) {
                    // T1.2 §7-1: 进入 RAW 透传前, 先在命令态下发本地阈值 + RULES 回读 + RAW; 一致后才允许"已连接"。
                    gwReadback_.clear(); gwHandshakeDone_ = false;
                    tcp_->write(buildRulePush());
                    tcp_->write("RULES\n");
                    tcp_->write("RAW\n");
                    tcp_->flush();
                    status_->setText("网关握手中：等待 +RAW 与阈值回读…");
                } else {
                    connectBtn_->setText("断开"); connected_ = true;
                }
            });
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
    // 网关 (TCP RAW) 模式：进入 RAW 透传前先在命令态下发本地阈值并回读核对(§7-1)，
    //   吃掉 +RAW 之前的 RULE_ACK/RULES 文本应答行，其后字节才交解码器（见 toggleConnect）。
    bool         gwMode_ = false;
    bool         gwHandshakeDone_ = false;
    QByteArray   gwRxBuf_;
    std::map<int, std::pair<float, float>> gwReadback_;   // type -> (low, high) 网关回读值

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
    // --verify-gateway <host> <port> <ms>: T1.2 无头验收帧计数等式 (不起事件循环)
    for (int i = 1; i + 3 < argc; ++i) {
        if (QString(argv[i]) == "--verify-gateway")
            return w.runVerifyGateway(QString(argv[i + 1]), QString(argv[i + 2]), QString(argv[i + 3]).toInt());
    }
    w.show();

    // --demo-gateway <host> <port>: GUI 启动即连网关 (TCP RAW)
    for (int i = 1; i + 2 < argc; ++i) {
        if (QString(argv[i]) == "--demo-gateway") {
            w.demoConnectGateway(QString(argv[i + 1]), QString(argv[i + 2]));
            break;
        }
    }

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
