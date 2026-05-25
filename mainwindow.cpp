#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "include/console.h"
#include "include/mytimer.h"

#include "circularbuffer.h"
#include <qwt_plot_canvas.h>

#include <QMessageBox>
#include <QSplitter>
#include <QGraphicsView>
#include <QWidget>
#include <QGroupBox>
#include <QDateTime>
#include <QSettings>
#include <QTimer>

#include <QDebug>

// команды контроллера (соответствуют V1.1)
static const int DEFAULT_SEND_CMD   = 2;    // сопровождение
static const int DEFAULT_SEND_PERIOD_MS = 100;  // 0.1 c

// Обёртка поля времени пакета: 60 секунд * 1000 мс — младшая часть реального
// времени (секунда в минуте + миллисекунды).
static const qint64 PACKET_TIME_WRAP_MS = 60 * 1000;

// Визуальные параметры разделителей в тулбаре.
// Ширина самой вертикальной линии, px.
static const int TOOLBAR_SEP_WIDTH_PX = 2;
// Отступ слева/справа от линии — определяет воздух между группами (как на
// панели задач Windows). Полный зазор между иконками соседних групп =
// icon_padding + margin_left + SEP_WIDTH + margin_right + icon_padding.
static const int TOOLBAR_SEP_MARGIN_PX = 12;
// Вертикальные отступы линии от верха/низа тулбара.
static const int TOOLBAR_SEP_VMARGIN_PX = 6;
// Общий зазор между обычными элементами тулбара внутри группы.
static const int TOOLBAR_ITEM_SPACING_PX = 4;

static const int MAX_BUFFER_SIZE = 8192;  // bytes
static const int PERIOD_DRAW = 100; //мсек
static const int KOL_POINT = 5000;  // количество точек на графике с интервалом в PERIOD_DRAW

MainWindow::MainWindow(QWidget *parent)
  : QMainWindow(parent)
  , ui(new Ui::MainWindow)
  , m_settings(new SettingsDialog(this))
  , m_serial(new QSerialPort(this))
  , m_console(new Console())
  , m_timer(new myTimer(this))
  , m_control_serial(new QSerialPort(this))
  , m_send_timer(new QTimer(this))
{
  ui->setupUi(this);

  m_send_timer->setInterval(DEFAULT_SEND_PERIOD_MS);
  m_send_timer->setTimerType(Qt::PreciseTimer);
  connect(m_send_timer, &QTimer::timeout, this, &MainWindow::sendMoveCommand);

  // Явно стилизуем тулбар, чтобы разделители между группами были видны
  // на тёмной теме и имели настраиваемое «дыхание» вокруг линии.
  ui->toolBar->setStyleSheet(QString(
      "QToolBar { spacing: %1px; }"
      "QToolBar::separator {"
      "  background-color: rgba(160, 160, 160, 160);"
      "  width: %2px;"
      "  margin-left: %3px;"
      "  margin-right: %3px;"
      "  margin-top: %4px;"
      "  margin-bottom: %4px;"
      "}")
      .arg(TOOLBAR_ITEM_SPACING_PX)
      .arg(TOOLBAR_SEP_WIDTH_PX)
      .arg(TOOLBAR_SEP_MARGIN_PX)
      .arg(TOOLBAR_SEP_VMARGIN_PX));

  initActionsConnections();

  m_console->setEnabled(false);
  m_console->setLocalEchoEnabled(true);

  terminalTabFill();

  showStatusMessage(ui->statusbar, tr("Disconnected"));

  connect(m_serial, &QSerialPort::readyRead, this, &MainWindow::readData);
  connect(m_console, &Console::getData, this, &MainWindow::writeData);

  // управляющий порт
  connect(m_control_serial, &QSerialPort::readyRead, this, &MainWindow::readControlData);
  connect(m_control_serial, &QSerialPort::errorOccurred, this, &MainWindow::handleControlError);


  double period = PERIOD_DRAW; //мсек
  double numpoint = KOL_POINT;
  d_plot_az = new Plot(this, -250, 250, period, numpoint);
  d_plot_el = new Plot(this, 0, 110, period, numpoint);

  d_delta_plot_az = new Plot(this, -4, 4, period, numpoint);
  d_delta_plot_el = new Plot(this, -4, 4, period, numpoint);

  ui->tabWidget_2->addTab(d_plot_az,QObject::tr("Азимут"));
  ui->tabWidget_2->addTab(d_plot_el,QObject::tr("Угол места"));
  ui->tabWidget_2->addTab(d_delta_plot_az,QObject::tr("Азимут (дельта)"));
  ui->tabWidget_2->addTab(d_delta_plot_el,QObject::tr("Угол места (дельта)"));
}

MainWindow::~MainWindow()
{
  if (m_send_timer) m_send_timer->stop();
  delete protocol_;
  delete control_protocol_;
  delete d_plot_el;
  delete d_plot_az;
  delete d_delta_plot_az;
  delete d_delta_plot_el;

  delete ui;
  delete m_console;
  delete m_serial;
  delete m_control_serial;
  delete m_settings;
  delete m_timer;
}

void MainWindow::initActionsConnections()
{
  connect(ui->actionSerialSettings, &QAction::triggered, m_settings, &SettingsDialog::show);
  connect(m_settings, &SettingsDialog::settingsApplied, ui->actionSerialConnect, &QAction::setEnabled);
  connect(m_settings, &SettingsDialog::settingsApplied, ui->actionControlSerialConnect, &QAction::setEnabled);
  connect(m_settings, &SettingsDialog::settingsApplied, this, &MainWindow::writeSettings);
  connect(ui->actionConsoleClear, &QAction::triggered, m_console, &Console::clear);

  connect(ui->actionSerialConnect, &QAction::triggered, this, &MainWindow::openSerialPort);
  connect(ui->actionSerialDisconnect, &QAction::triggered, this, &MainWindow::closeSerialPort);

  connect(ui->actionControlSerialConnect, &QAction::triggered, this, &MainWindow::openControlPort);
  connect(ui->actionControlSerialDisconnect, &QAction::triggered, this, &MainWindow::closeControlPort);
  connect(ui->actionControlSendMove, &QAction::triggered, this, &MainWindow::sendMoveCommand);
  connect(ui->actionControlSendStart, &QAction::triggered, this, &MainWindow::startPeriodicSending);
  connect(ui->actionControlSendStop,  &QAction::triggered, this, &MainWindow::stopPeriodicSending);

  // период из правой панели
  connect(ui->sendPeriodSpin, QOverload<int>::of(&QSpinBox::valueChanged),
          this, &MainWindow::applySendPeriod);
  m_send_timer->setInterval(ui->sendPeriodSpin->value());

  connect(m_serial, &QSerialPort::errorOccurred, this, &MainWindow::handleError);
}

void MainWindow::showStatusMessage(QStatusBar * statusBar, const QString &message)
{
  statusBar->QStatusBar::showMessage(message);
}

void MainWindow::writeSettings()
{
  QSettings settings("MySoft", "terminal2");
  SettingsDialog::Settings _t_settings = m_settings->settings();

  settings.beginGroup("SerialSettings");
  settings.setValue("name", _t_settings.name);
  settings.setValue("baudRate", _t_settings.baudRate);
  settings.setValue("dataBits", _t_settings.dataBits);
  settings.setValue("parity", _t_settings.parity);
  settings.setValue("stopBits", _t_settings.stopBits);
  settings.setValue("flowControl", _t_settings.flowControl);
  settings.setValue("localEchoEnabled", _t_settings.localEchoEnabled);
  settings.endGroup();

  SettingsDialog::Settings _c_settings = m_settings->controlSettings();
  settings.beginGroup("ControlSerialSettings");
  settings.setValue("name", _c_settings.name);
  settings.setValue("baudRate", _c_settings.baudRate);
  settings.setValue("dataBits", _c_settings.dataBits);
  settings.setValue("parity", _c_settings.parity);
  settings.setValue("stopBits", _c_settings.stopBits);
  settings.setValue("flowControl", _c_settings.flowControl);
  settings.endGroup();
}

void MainWindow::handleError(QSerialPort::SerialPortError error)
{
  if (error == QSerialPort::ResourceError) {
    QMessageBox::critical(this, tr("Critical Error"), m_serial->errorString());
    closeSerialPort();
  }
}

void MainWindow::handleControlError(QSerialPort::SerialPortError error)
{
  if (error == QSerialPort::ResourceError) {
    QMessageBox::critical(this, tr("Critical Error (control)"), m_control_serial->errorString());
    closeControlPort();
  }
}

void MainWindow::closeSerialPort()
{
  if (m_serial->isOpen())
    m_serial->close();
  //m_console->setEnabled(false);
  ui->actionSerialConnect->setEnabled(true);
  ui->actionSerialDisconnect->setEnabled(false);
  ui->actionSerialSettings->setEnabled(true);
  showStatusMessage(ui->statusbar, tr("Disconnected"));
}

void MainWindow::openSerialPort()
{
  const SettingsDialog::Settings p = m_settings->settings();
  m_serial->setPortName(p.name);

  m_serial->setBaudRate(p.baudRate);
  m_serial->setDataBits(p.dataBits);
  m_serial->setParity(p.parity);
  m_serial->setStopBits(p.stopBits);
  m_serial->setFlowControl(p.flowControl);
  if (m_serial->open(QIODevice::ReadWrite)) {
    m_console->setEnabled(true);
    m_console->setLocalEchoEnabled(p.localEchoEnabled);
    ui->actionSerialConnect->setEnabled(false);
    ui->actionSerialDisconnect->setEnabled(true);
    ui->actionSerialSettings->setEnabled(false);
    showStatusMessage(ui->statusbar, tr("Connected to %1 : %2, %3, %4, %5, %6")
                      .arg(p.name).arg(p.stringBaudRate).arg(p.stringDataBits)
                      .arg(p.stringParity).arg(p.stringStopBits).arg(p.stringFlowControl));
  } else {
    QMessageBox::critical(this, tr("Error"), m_serial->errorString());
    showStatusMessage(ui->statusbar, tr("Open error"));
  }

  m_console->clear();
  m_console->printPromt();

  // при повторном открытии освобождаем старый экземпляр протокола
  if (protocol_) {
    delete protocol_;
    protocol_ = nullptr;
  }
  protocol_ = new mappi::antenna::VkaProtocol(/*debug=*/true);
  buf_rx_.clear();
}

void MainWindow::openControlPort()
{
  const SettingsDialog::Settings p = m_settings->controlSettings();
  m_control_serial->setPortName(p.name);
  m_control_serial->setBaudRate(p.baudRate);
  m_control_serial->setDataBits(p.dataBits);
  m_control_serial->setParity(p.parity);
  m_control_serial->setStopBits(p.stopBits);
  m_control_serial->setFlowControl(p.flowControl);

  if (m_control_serial->open(QIODevice::ReadWrite)) {
    ui->actionControlSerialConnect->setEnabled(false);
    ui->actionControlSerialDisconnect->setEnabled(true);
    ui->actionControlSendMove->setEnabled(true);
    ui->actionControlSendStart->setEnabled(true);
    ui->actionControlSendStop->setEnabled(false);
    showStatusMessage(ui->statusbar, tr("Control port connected to %1 : %2, %3, %4, %5, %6")
                      .arg(p.name).arg(p.stringBaudRate).arg(p.stringDataBits)
                      .arg(p.stringParity).arg(p.stringStopBits).arg(p.stringFlowControl));
  } else {
    QMessageBox::critical(this, tr("Error"), m_control_serial->errorString());
    showStatusMessage(ui->statusbar, tr("Control port open error"));
    return;
  }

  if (control_protocol_) {
    delete control_protocol_;
    control_protocol_ = nullptr;
  }
  control_protocol_ = new mappi::antenna::VkaProtocol(/*debug=*/false);
  buf_control_rx_.clear();
}

void MainWindow::closeControlPort()
{
  // Если шла периодическая отправка — остановить таймер.
  // stopPeriodicSending() сам выставит Start=true/Stop=false, но при
  // отсутствующем коннекте Start тоже должен быть выключен — исправляем ниже.
  stopPeriodicSending();

  if (m_control_serial->isOpen())
    m_control_serial->close();
  ui->actionControlSerialConnect->setEnabled(true);
  ui->actionControlSerialDisconnect->setEnabled(false);
  ui->actionControlSendMove->setEnabled(false);
  ui->actionControlSendStart->setEnabled(false);
  ui->actionControlSendStop->setEnabled(false);
  showStatusMessage(ui->statusbar, tr("Control port disconnected"));
}


void MainWindow::splitData( )
{
  if (!protocol_) return;

  int mtu = protocol_->mtu();
  while (mtu <= buf_rx_.size()) {
    if (protocol_->split(buf_rx_) &&
        protocol_->split(buf_rx_, mtu)) {
      QByteArray pack = buf_rx_.left(mtu);
      QString error;
      mappi::antenna::Drive azimut_;
      mappi::antenna::Drive elevat_;
      if (protocol_->unpack(pack, &azimut_, &elevat_, &error) == false)
      {
        qDebug() << error;
      } else {
        onDriveReceived(azimut_, elevat_, QStringLiteral("terminal"));
      }
      buf_rx_.remove(0, mtu);

      continue ;
    }

    buf_rx_.remove(0, 1);
  }

  if (MAX_BUFFER_SIZE < buf_rx_.size()) {
    qDebug() << buf_rx_.toHex();
    qDebug() << QObject::tr("RX: переполнение буфера, буфер будет очищен");
    buf_rx_.clear();
  }
}

void MainWindow::splitControlData()
{
  if (!control_protocol_) return;

  int mtu = control_protocol_->mtu();
  while (mtu <= buf_control_rx_.size()) {
    if (control_protocol_->split(buf_control_rx_) &&
        control_protocol_->split(buf_control_rx_, mtu)) {
      QByteArray pack = buf_control_rx_.left(mtu);
      QString error;
      mappi::antenna::Drive azimut_;
      mappi::antenna::Drive elevat_;
      if (control_protocol_->unpack(pack, &azimut_, &elevat_, &error) == false) {
        qDebug() << "[control]" << error;
      } else {
        onDriveReceived(azimut_, elevat_, QStringLiteral("control"));
      }
      buf_control_rx_.remove(0, mtu);
      continue;
    }

    buf_control_rx_.remove(0, 1);
  }

  if (MAX_BUFFER_SIZE < buf_control_rx_.size()) {
    qDebug() << buf_control_rx_.toHex();
    qDebug() << QObject::tr("RX control: переполнение буфера, буфер будет очищен");
    buf_control_rx_.clear();
  }
}


void MainWindow::recv(const mappi::antenna::Drive& az,const mappi::antenna::Drive& el )
{
  d_plot_az->addPoint(az.self);
  d_plot_el->addPoint(el.self);

  d_delta_plot_az->addPoint(az.dst);
  d_delta_plot_el->addPoint(el.dst);
}

void MainWindow::onDriveReceived(const mappi::antenna::Drive& az,
                                 const mappi::antenna::Drive& el,
                                 const QString& source)
{
  d_plot_az->addPoint(az.self);
  d_plot_el->addPoint(el.self);

  if (source == QStringLiteral("terminal")) {
    d_delta_plot_az->addPoint(az.dst);
    d_delta_plot_el->addPoint(el.dst);
  }
}

void MainWindow::readData()
{
  const QByteArray data = m_serial->readAll();
  buf_rx_.append(data);
  m_console->putData(data);

  splitData();
}

void MainWindow::readControlData()
{
  const QByteArray data = m_control_serial->readAll();
  buf_control_rx_.append(data);

  splitControlData();
}

void MainWindow::writeData(const QByteArray &data)
{
  m_serial->write(data);
}

void MainWindow::sendMoveCommand()
{
  if (!m_control_serial->isOpen() || !control_protocol_) {
    // При ручной отправке предупреждаем, при периодической — молчим
    // (чтобы таймер не спамил диалогами после внезапного разрыва).
    if (sender() == ui->actionControlSendMove) {
      QMessageBox::information(this, tr("Control"),
                               tr("Управляющий порт не подключён"));
    }
    return;
  }

  const float az   = static_cast<float>(ui->azTargetSpin->value());
  const float el   = static_cast<float>(ui->elTargetSpin->value());
  const float az_v = static_cast<float>(ui->azVelSpin->value());
  const float el_v = static_cast<float>(ui->elVelSpin->value());

  const QByteArray packet = control_protocol_->packMove(
      DEFAULT_SEND_CMD, az, el, az_v, el_v, m_packet_time_ms);
  m_control_serial->write(packet);
  qDebug() << "TX control:" << packet;

  // Счётчик времени двигаем только на тике таймера периодической отправки.
  // На разовой отправке он "замораживается" — пакет уйдёт с той же меткой,
  // что и предыдущий, но это ок: разовая отправка это штатное поведение.
  if (sender() == m_send_timer) {
    m_packet_time_ms = (m_packet_time_ms + m_send_timer->interval()) % PACKET_TIME_WRAP_MS;
  }
}

void MainWindow::startPeriodicSending()
{
  if (!m_control_serial->isOpen() || !control_protocol_) {
    QMessageBox::information(this, tr("Control"),
                             tr("Сначала подключите управляющий порт"));
    return;
  }
  // Единственный опрос системных часов — сейчас. Берём младшую часть
  // реального времени: секунда в минуте (0..59) * 1000 + миллисекунды (0..999).
  const QTime now = QTime::currentTime();
  m_packet_time_ms = static_cast<qint64>(now.second()) * 1000 + now.msec();

  m_send_timer->setInterval(ui->sendPeriodSpin->value());
  m_send_timer->start();
  ui->actionControlSendStart->setEnabled(false);
  ui->actionControlSendStop->setEnabled(true);
}

void MainWindow::stopPeriodicSending()
{
  m_send_timer->stop();
  // m_packet_time_ms не трогаем — если будет разовая отправка до следующего
  // старта, пакет уйдёт с последним известным значением.
  ui->actionControlSendStart->setEnabled(true);
  ui->actionControlSendStop->setEnabled(false);
}

void MainWindow::applySendPeriod(int periodMs)
{
  // Минимум 10 мс — чтобы случайно не выставить 0 и не залипнуть.
  m_send_timer->setInterval(qMax(10, periodMs));
}

void MainWindow::timerAction()
{
}

void MainWindow::replaceTab(QTabWidget * tabs, int index,
                            QWidget * replacement, const QString & label)
{
  Q_ASSERT(tabs && tabs->count() > index);
  tabs->removeTab(index);
  if (replacement) tabs->insertTab(index, replacement, label);

  tabs->setCurrentIndex(index);
}

void MainWindow::terminalTabFill()
{
  ui->splitter->replaceWidget(0, m_console);
  m_console->show();
}
