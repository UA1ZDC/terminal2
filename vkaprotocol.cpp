#include "vkaprotocol.h"
#include <QRegularExpression>
#include <cmath>

namespace mappi {

namespace antenna {

static const QString DLE("5555");
static const QString ETX("\r\n");

// Штатный формат: seq(2) id(2) state(4) azimut(6) elevat(6) crc(4) + \r\n
static const QString RESPONSE_PATTERN_NORMAL(
    "^(5555)(\\d{2})(\\d{4})([+-]\\d{5})([+-]\\d{5})(\\d{4})(.*\\r\\n)$");
static const int PAYLOAD_NORMAL = 22;
static const int MTU_NORMAL     = 30;

// Отладочный формат: те же поля + 2 целевых угла
static const QString RESPONSE_PATTERN_DEBUG(
    "^(5555)(\\d{2})(\\d{4})([+-]\\d{5})([+-]\\d{5})([+-]\\d{5})([+-]\\d{5})(\\d{4})(.*\\r\\n)$");
static const int PAYLOAD_DEBUG = 34;
static const int MTU_DEBUG     = 42;


VkaProtocol::VkaProtocol(bool debug) :
  debug_(debug),
  seq_(0)
{
}

VkaProtocol::~VkaProtocol()
{
}

int VkaProtocol::mtu() const
{
  return debug_ ? MTU_DEBUG : MTU_NORMAL;
}

QByteArray VkaProtocol::pack(int id, float azimut, float elevat)
{
  if (seq_ == 100)
    seq_ = 0;

  QString payload = QString("%1%2%3%4%5")
                    .arg(seq_++, 2, 10, QChar('0'))     // seq пакета
                    .arg(id, 2, 10, QChar('0'))         // id команды
                    .arg(0, 6, 10, QChar('0'))          // данные 1
                    .arg(setAngle(azimut))              // данные 2
                    .arg(setAngle(elevat));             // данные 3

  return QString("%1%2%3%4")
      .arg(DLE)                           // "5555"
      .arg(payload)                       // полезная нагрузка
      .arg(crc(payload))                  // LRC
      .arg(ETX)                           // "\r\n"
      .toLocal8Bit();
}

static QString setVelocity(float v)
{
  // Формат V1.1: знак + 2 целых + 3 тысячных. Масштаб 1000, диапазон ±99.999.
  if (v >  99.999f) v =  99.999f;
  if (v < -99.999f) v = -99.999f;
  return QString("%1%2")
      .arg(QChar(0 <= v ? '+' : '-'))
      .arg(int(::fabs(v) * 1000), 5, 10, QChar('0'));
}

// "T" + ss + msmsms из packet_time_ms (младшая часть реального времени,
// вызывающая сторона сама оборачивает значение по 60000 мс).
static QString packetTimeField(qint64 packet_time_ms)
{
  if (packet_time_ms < 0) packet_time_ms = 0;
  const int ss = static_cast<int>((packet_time_ms / 1000) % 100); // 0..59 after caller wrap
  const int ms = static_cast<int>(packet_time_ms % 1000);         // 0..999
  return QString("T%1%2")
      .arg(ss, 2, 10, QChar('0'))
      .arg(ms, 3, 10, QChar('0'));
}

QByteArray VkaProtocol::packMove(int cmd, float azimut, float elevat,
                                 float az_vel, float el_vel, qint64 packet_time_ms)
{
  if (seq_ == 100)
    seq_ = 0;

  QString payload = QString("%1%2%3%4%5%6%7")
                    .arg(seq_++, 2, 10, QChar('0'))         // seq пакета
                    .arg(cmd, 2, 10, QChar('0'))            // id команды (1/2)
                    .arg(packetTimeField(packet_time_ms))   // Данные_1: T + ss + msmsms
                    .arg(setAngle(azimut))                  // Данные_2: цель по азимуту
                    .arg(setAngle(elevat))                  // Данные_3: цель по углу места
                    .arg(setVelocity(az_vel))               // Данные_4: скорость по азимуту
                    .arg(setVelocity(el_vel));              // Данные_5: скорость по углу места

  return QString("%1%2%3%4")
      .arg(DLE)
      .arg(payload)
      .arg(crc(payload))
      .arg(ETX)
      .toLocal8Bit();
}

QByteArray VkaProtocol::packAzimut(int id, float v)
{
  // WARNING протокол не поддерживает асинхронное выполнение
  Q_UNUSED(id)
  Q_UNUSED(v)

  return QByteArray();
}

QByteArray VkaProtocol::packElevat(int id, float v)
{
  // WARNING протокол не поддерживает асинхронное выполнение
  Q_UNUSED(id)
  Q_UNUSED(v)

  return QByteArray();
}

bool VkaProtocol::unpack(const QByteArray& buf, Drive* azimut, Drive* elevat, QString* error /*=*/)
{
  QString str(buf);

  int pos = 0;

  const QString& pattern   = debug_ ? RESPONSE_PATTERN_DEBUG : RESPONSE_PATTERN_NORMAL;
  const int      payloadLen = debug_ ? PAYLOAD_DEBUG : PAYLOAD_NORMAL;

  // В обоих форматах хвостовая группа — последняя, в ней LRC + CRLF.
  const int tailGroup = debug_ ? 9 : 7;

  QRegularExpression rx(pattern);
  QRegularExpressionMatch _rxMatch = rx.match(str, pos);

  if (!_rxMatch.hasMatch()) {
    if (error) *error = QObject::tr("format fail");
    return false;
  }

  QString state = _rxMatch.captured(3);
  quint16 state_ctrl = state.left(2).toInt(nullptr, 16);

  QString tail = _rxMatch.captured(tailGroup);

  if ((DLE != _rxMatch.captured(1)) || (!tail.contains(ETX))) {
    if (error) *error = QObject::tr("match DLE, ETX fail");
    return false;
  }

  // Итоговая сверка CRC: удаляем \r\n из хвоста (остаётся 2 hex-символа LRC)
  // и сравниваем с CRC от полезной нагрузки.
  if (tail.replace(ETX, QString()) != crc(str.mid(DLE.length(), payloadLen))) {

    if (azimut != nullptr) azimut->state = Drive::CRC_NOT_VALID;
    if (elevat != nullptr) elevat->state = Drive::CRC_NOT_VALID;

    if (error) *error = QObject::tr("CRC fail");
    return false;
  }

  if (azimut != nullptr) {
    azimut->state = Drive::OK;
    ++azimut->seq;

    quint16 st = state_ctrl & 0xA000;
    switch (st) {
      case 0x8000 :
        azimut->state = Drive::FAIL;
      break;

      case 0x4000 :
        azimut->state = Drive::SENSOR_ANGLE_FAIL;
      break;

      default :
        // тайну обработки невалидной CRC знает только Юра
        azimut->self = getAngle(_rxMatch.captured(4));
      break;
    }
  }

  if (elevat != nullptr) {
    elevat->state = Drive::OK;
    ++elevat->seq;

    quint16 st = state_ctrl & 0x5000;
    switch (st) {
      case 0x2000 :
        elevat->state = Drive::FAIL;
      break;

      case 0x1000 :
        elevat->state = Drive::SENSOR_ANGLE_FAIL;
      break;

      default :
        // тайну обработки невалидной CRC знает только Юра
        elevat->self = getAngle(_rxMatch.captured(5));
      break;
    }
  }

  if (debug_) {
    if (azimut != nullptr) {
      azimut->dst = getAngle(_rxMatch.captured(6));
    }
    if (elevat != nullptr) {
      elevat->dst = getAngle(_rxMatch.captured(7));
    }
  }

  return true;
}

bool VkaProtocol::split(const QByteArray& buf) const
{
  return (buf.indexOf(ETX.toLocal8Bit(), 0) != -1);
}

bool VkaProtocol::split(const QByteArray& buf, int mtu) const
{
  return (buf.indexOf(ETX.toLocal8Bit(), 0) == mtu-ETX.length());
}

float VkaProtocol::getAngle(const QString& buf) const
{
  return (buf.toDouble() * 0.01);
}

QString VkaProtocol::setAngle(float v) const
{
  return QString("%1%2")
      .arg(QChar(0 <= v ? '+' : '-'))
      .arg(int(::abs(v) * 100), 5, 10, QChar('0'));
}

QString VkaProtocol::crc(const QString& buf) const
{
  QByteArray str(buf.toLocal8Bit());

  unsigned char sum = 0;
  for (int i = 0; i < str.size(); ++i)
    sum += str[i];

  return QString::number((256 - sum), 16)
      .toUpper();
}

}

}
