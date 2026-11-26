#include "PskLive.hpp"

#include <QTcpSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

namespace
{
  char const * const host = "mqtt.pskreporter.info";
  quint16 const port = 1883;
  int const keep_alive = 60;        // seconds
}

PskLive::PskLive (QObject * parent)
  : QObject {parent}
  , socket_ {new QTcpSocket {this}}
  , ping_ {new QTimer {this}}
  , retry_ {new QTimer {this}}
{
  qRegisterMetaType<PskLive::Report> ("PskLive::Report");
  ping_->setInterval (keep_alive * 1000 / 2);
  retry_->setSingleShot (true);
  connect (ping_, &QTimer::timeout, this, [this] {send (0xC0, {});});
  connect (retry_, &QTimer::timeout, this, &PskLive::open);
  connect (socket_, &QTcpSocket::connected, this, [this] {
      // CONNECT: protocol MQTT 3.1.1, clean session, keep alive, a client id
      QByteArray body = string ("MQTT");
      body.append (char (4));
      body.append (char (0x02));
      body.append (char (keep_alive >> 8)).append (char (keep_alive & 0xff));
      body.append (string (QString {"kf0kyldigi-%1"}.arg (QRandomGenerator::global ()->generate (), 8, 16, QChar {'0'}).toLatin1 ()));
      send (0x10, body);
    });
  connect (socket_, &QTcpSocket::readyRead, this, &PskLive::read);
  connect (socket_, &QTcpSocket::disconnected, this, [this] {
      bool const was = subscribed_;
      subscribed_ = false;
      ping_->stop ();
      if (was) Q_EMIT state_changed ();
      if (!call_.isEmpty ())
        {
          // try again, waiting longer each time up to 5 minutes
          ++failures_;
          retry_->start (qMin (300, 5 * failures_ * failures_) * 1000);
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK (5, 15, 0)
  connect (socket_, &QTcpSocket::errorOccurred, this, [this] (QAbstractSocket::SocketError) {
#else
  connect (socket_, QOverload<QAbstractSocket::SocketError>::of (&QAbstractSocket::error), this, [this] (QAbstractSocket::SocketError) {
#endif
      if (QAbstractSocket::UnconnectedState == socket_->state () && !call_.isEmpty () && !retry_->isActive ())
        {
          ++failures_;
          retry_->start (qMin (300, 5 * failures_ * failures_) * 1000);
          Q_EMIT state_changed ();
        }
    });
}

PskLive::~PskLive ()
{
  // going away: stop listening to the socket first so closing it can't
  // set off retries or "state changed" news to windows that are closing
  call_.clear ();
  retry_->stop ();
  ping_->stop ();
  socket_->disconnect (this);
  blockSignals (true);
  socket_->abort ();
}

QByteArray PskLive::string (QByteArray const& s)
{
  QByteArray out;
  out.append (char (s.size () >> 8)).append (char (s.size () & 0xff));
  return out + s;
}

void PskLive::send (quint8 type, QByteArray const& body)
{
  if (QAbstractSocket::ConnectedState != socket_->state ()) return;
  QByteArray packet;
  packet.append (char (type));
  int len = body.size ();
  do
    {
      quint8 digit = len % 128;
      len /= 128;
      if (len > 0) digit |= 0x80;
      packet.append (char (digit));
    }
  while (len > 0);
  socket_->write (packet + body);
}

void PskLive::listen (QString const& call)
{
  auto const c = call.trimmed ().toUpper ();
  if (c == call_) return;
  close ();
  call_ = c;
  failures_ = 0;
  if (!call_.isEmpty ()) open ();
}

void PskLive::open ()
{
  if (call_.isEmpty () || QAbstractSocket::UnconnectedState != socket_->state ()) return;
  buffer_.clear ();
  socket_->connectToHost (host, port);
}

void PskLive::close ()
{
  retry_->stop ();
  ping_->stop ();
  bool const was = subscribed_;
  subscribed_ = false;
  call_.clear ();
  socket_->abort ();
  if (was) Q_EMIT state_changed ();
}

void PskLive::read ()
{
  buffer_ += socket_->readAll ();
  while (buffer_.size () >= 2)
    {
      // fixed header: type, then the remaining length in 7-bit pieces
      int len = 0, shift = 0, at = 1;
      bool complete = false;
      while (at < buffer_.size () && at <= 4)
        {
          quint8 const digit = quint8 (buffer_[at++]);
          len |= (digit & 0x7f) << shift;
          shift += 7;
          if (!(digit & 0x80)) {complete = true; break;}
        }
      if (!complete || buffer_.size () < at + len) return;
      quint8 const type = quint8 (buffer_[0]);
      QByteArray const body = buffer_.mid (at, len);
      buffer_.remove (0, at + len);

      switch (type >> 4)
        {
        case 2:                 // CONNACK
          if (body.size () >= 2 && 0 == body[1])
            {
              // reports of this call being heard, any band and mode
              QByteArray sub;
              sub.append (char (0)).append (char (1));
              sub.append (string (QString {"pskr/filter/v2/+/+/%1/#"}.arg (call_).toLatin1 ()));
              sub.append (char (0));
              send (0x82, sub);
              ping_->start ();
            }
          break;

        case 9:                 // SUBACK
          subscribed_ = true;
          failures_ = 0;
          Q_EMIT state_changed ();
          break;

        case 3:                 // PUBLISH
          {
            if (body.size () < 2) break;
            int const topic_len = (quint8 (body[0]) << 8) | quint8 (body[1]);
            int payload_at = 2 + topic_len;
            if ((type >> 1) & 3) payload_at += 2;     // packet id, if QoS > 0
            auto const o = QJsonDocument::fromJson (body.mid (payload_at)).object ();
            Report r;
            r.receiver = o.value ("rc").toString ().toUpper ();
            r.grid = o.value ("rl").toString ();
            r.band = o.value ("b").toString ().toLower ();
            r.mhz = o.value ("f").toDouble () / 1e6;
            r.snr = o.value ("rp").toInt ();
            r.mode = o.value ("md").toString ();
            r.when = QDateTime::fromSecsSinceEpoch (qint64 (o.value ("t").toDouble ()), Qt::UTC);
            if (r.receiver.isEmpty () || r.grid.size () < 4) break;
            last_report_ = QDateTime::currentDateTimeUtc ();
            Q_EMIT report (r);
            break;
          }

        default:                // PINGRESP and anything else
          break;
        }
    }
}
