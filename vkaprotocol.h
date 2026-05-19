#pragma once

#include "protocol.h"


namespace mappi {

namespace antenna {

class VkaProtocol :
  public Protocol
{
public :
  typedef quint8 seq_t;

public :
  // debug==true — расширенный пакет с целевыми углами (MTU 42),
  // debug==false — штатный пакет (MTU 30).
  explicit VkaProtocol(bool debug = true);
  virtual ~VkaProtocol();

  bool isDebug() const { return debug_; }

  virtual bool isText() const { return true; }
  virtual bool isAsyncExec() const { return false; }
  virtual int mtu() const ;
  virtual const char* type() const { return debug_ ? "vka protocol (debug)" : "vka protocol"; }

  virtual QByteArray pack(int id, float azimut, float elevat);
  virtual QByteArray packAzimut(int id, float v);
  virtual QByteArray packElevat(int id, float v);

  // Команда движения в формате V1.1 (42 байта):
  //   DLE seq id "T"+ss(2)+ms(3) az(±5) el(±5) az_vel(±5) el_vel(±5) CRC ETX
  // cmd: 1 — наведение, 2 — сопровождение (в V1.1 эквивалентны).
  // packet_time_ms — метка времени пакета в миллисекундах (обёрнутая
  // вызывающей стороной по 100000). Разбирается внутри на ss(0..99) и ms(0..999).
  QByteArray packMove(int cmd, float azimut, float elevat,
                      float az_vel, float el_vel, qint64 packet_time_ms);

  virtual bool unpack(const QByteArray& buf, Drive* azimut, Drive* elevat, QString* error = nullptr);

  virtual bool split(const QByteArray& buf) const;
  virtual bool split(const QByteArray& buf, int mtu) const;

private :
  float getAngle(const QString& buf) const;
  QString setAngle(float v) const;

  QString crc(const QString& buf) const;

private :
  const bool debug_;
  seq_t seq_;
};

}

}
