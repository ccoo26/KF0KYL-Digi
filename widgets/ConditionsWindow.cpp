#include "ConditionsWindow.hpp"

#include <cmath>
#include <cstdio>
#include <QtNumeric>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QTimer>
#include <QFrame>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QCloseEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QToolTip>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QProcess>
#include <QFileDialog>
#include <QtMath>
#include <QPointer>
#include <QtEndian>
#include <memory>
#include <cstring>
#include <QScrollArea>
#include <QStandardPaths>
#include <QDirIterator>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <algorithm>
#include <QLocale>
#include <QComboBox>
#include <QCheckBox>
#include <QIcon>
#include <QPixmap>
#include <QSet>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QMenu>
#include <QAction>
#include <QToolButton>
#include <QFile>
#include <QTextStream>
#include <QFontMetricsF>
#include <QPolygonF>
#include <QButtonGroup>
#include <QActionGroup>
#include <QDesktopServices>
#include <QLineEdit>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSlider>
#include "PskLive.hpp"
#include "CallsignInfo.hpp"
#include <QPushButton>
#include <QProgressBar>
#include <QColorDialog>
#include <QRadialGradient>
#include <QLinearGradient>

#include "revision_utils.hpp"
#include "Win11Theme.hpp"

#include "moc_ConditionsWindow.cpp"

namespace
{
  double const pi = 3.14159265358979323846;
  double rad (double d) {return d * pi / 180.;}
  double deg (double r) {return r * 180. / pi;}

  QString swpc_base () {return "https://services.swpc.noaa.gov/";}
  QString kc2g_base () {return "https://prop.kc2g.com/";}
  QString news_base (QString const& url, QString const&) {return url;}
  QString sdo_base () {return "https://sdo.gsfc.nasa.gov/";}

  // where the sun is overhead right now (degrees)
  void subsolar_point (QDateTime const& utc, double& lat, double& lon)
  {
    double const jd = utc.toMSecsSinceEpoch () / 86400000.0 + 2440587.5;
    double const n = jd - 2451545.0;
    double const L = std::fmod (280.460 + 0.9856474 * n, 360.);
    double const g = rad (std::fmod (357.528 + 0.9856003 * n, 360.));
    double const lambda = rad (L + 1.915 * std::sin (g) + 0.020 * std::sin (2 * g));
    double const eps = rad (23.439 - 0.0000004 * n);
    double const decl = std::asin (std::sin (eps) * std::sin (lambda));
    double const ra = std::atan2 (std::cos (eps) * std::sin (lambda), std::cos (lambda));
    double const gmst = std::fmod (18.697374558 + 24.06570982441908 * n, 24.);
    lat = deg (decl);
    lon = deg (ra) - gmst * 15.;
    while (lon < -180.) lon += 360.;
    while (lon > 180.) lon -= 360.;
  }

  // height of the sun above the horizon at a place (degrees)
  double sun_elevation (QDateTime const& utc, double lat, double lon)
  {
    double slat, slon;
    subsolar_point (utc, slat, slon);
    double const s = std::sin (rad (lat)) * std::sin (rad (slat))
      + std::cos (rad (lat)) * std::cos (rad (slat)) * std::cos (rad (lon - slon));
    return deg (std::asin (qBound (-1., s, 1.)));
  }

  // today's sunrise and sunset (UTC) at a place; invalid when the sun
  // doesn't rise or set
  void sun_times (double lat, double lon, QDateTime& rise, QDateTime& set)
  {
    rise = set = QDateTime {};
    auto const now = QDateTime::currentDateTimeUtc ();
    // look from local midnight (by longitude) through the next 24 hours
    auto start = QDateTime {now.date (), QTime {0, 0}, Qt::UTC}.addSecs (static_cast<int> (-lon / 15. * 3600.));
    if (start.secsTo (now) > 86400) start = start.addDays (1);
    if (start > now) start = start.addDays (-1);
    double prev = sun_elevation (start, lat, lon);
    for (int m = 2; m <= 1440; m += 2)
      {
        auto const t = start.addSecs (m * 60);
        double const e = sun_elevation (t, lat, lon);
        if (prev < -0.833 && e >= -0.833 && !rise.isValid ()) rise = t;
        if (prev >= -0.833 && e < -0.833 && !set.isValid ()) set = t;
        prev = e;
      }
  }

  // a number from a JSON value that may be a number or a quoted number
  double number (QJsonValue const& v)
  {
    if (v.isDouble ()) return v.toDouble ();
    if (v.isString ())
      {
        bool ok;
        double const d = v.toString ().trimmed ().toDouble (&ok);
        if (ok) return d;
      }
    return qQNaN ();
  }

  // NOAA files come as a list of objects, or (older format) a list of
  // lists with the names in the first row; give back a list of objects
  QList<QJsonObject> records (QByteArray const& data)
  {
    QList<QJsonObject> out;
    auto const doc = QJsonDocument::fromJson (data);
    if (doc.isObject ()) {out << doc.object (); return out;}
    if (!doc.isArray ()) return out;
    auto const array = doc.array ();
    QStringList names;
    for (auto const& item : array)
      {
        if (item.isObject ())
          {
            out << item.toObject ();
          }
        else if (item.isArray ())
          {
            auto const row = item.toArray ();
            if (names.isEmpty ())
              {
                for (auto const& n : row) names << n.toString ();
                continue;
              }
            QJsonObject o;
            for (int i = 0; i < row.size () && i < names.size (); ++i) o[names[i]] = row[i];
            out << o;
          }
      }
    return out;
  }

  double field (QJsonObject const& o, QStringList const& names)
  {
    for (auto const& name : names)
      {
        for (auto it = o.begin (); it != o.end (); ++it)
          {
            if (0 == it.key ().compare (name, Qt::CaseInsensitive))
              {
                double const d = number (it.value ());
                if (!qIsNaN (d)) return d;
              }
          }
      }
    return qQNaN ();
  }

  // rough day/night band outlook from solar flux and the K index
  // 0 poor, 1 fair, 2 good
  int band_estimate (int band_group, bool day, double sfi, double k)
  {
    if (qIsNaN (sfi)) return -1;
    int level = 0;
    switch (band_group)
      {
      case 0:                   // 80-40 m
        level = day ? 1 : 2;
        break;
      case 1:                   // 30-20 m
        level = day ? (sfi >= 90 ? 2 : 1) : (sfi >= 150 ? 2 : sfi >= 100 ? 1 : 0);
        break;
      case 2:                   // 17-15 m
        level = day ? (sfi >= 120 ? 2 : sfi >= 90 ? 1 : 0) : (sfi >= 200 ? 1 : 0);
        break;
      case 3:                   // 12-10 m
        level = day ? (sfi >= 150 ? 2 : sfi >= 110 ? 1 : 0) : 0;
        break;
      }
    if (!qIsNaN (k))
      {
        if (k >= 7) level -= 2;
        else if (k >= 5) level -= 1;
      }
    return qBound (0, level, 2);
  }

  QColor level_color (int level)
  {
    switch (level)
      {
      case 2: return QColor {"#3FB950"};
      case 1: return QColor {"#D29922"};
      case 0: return QColor {"#F85149"};
      default: return QColor {"#808080"};
      }
  }

  struct Station
  {
    double lat;
    double lon;
    QDateTime heard;
    int snr;
    bool cq;
    bool to_me;
    bool approx;
    QColor highlight;           // colour of the decode in Band Activity
  };


  // where a place is from another: angular distance (radians) and
  // initial heading (degrees from north)
  void path_to (double lat1, double lon1, double lat2, double lon2, double& dist, double& bearing)
  {
    double const p1 = rad (lat1), p2 = rad (lat2), dl = rad (lon2 - lon1);
    double const c = std::sin (p1) * std::sin (p2) + std::cos (p1) * std::cos (p2) * std::cos (dl);
    dist = std::acos (qBound (-1., c, 1.));
    bearing = deg (std::atan2 (std::sin (dl) * std::cos (p2),
                               std::cos (p1) * std::sin (p2) - std::sin (p1) * std::cos (p2) * std::cos (dl)));
    if (bearing < 0) bearing += 360.;
  }

  // the place reached going dist radians from a place on a heading
  void destination (double lat, double lon, double bearing, double dist, double& lat2, double& lon2)
  {
    double const p1 = rad (lat), b = rad (bearing);
    double const p2 = std::asin (qBound (-1., std::sin (p1) * std::cos (dist) + std::cos (p1) * std::sin (dist) * std::cos (b), 1.));
    double l2 = rad (lon) + std::atan2 (std::sin (b) * std::sin (dist) * std::cos (p1),
                                        std::cos (dist) - std::sin (p1) * std::sin (p2));
    lat2 = deg (p2);
    lon2 = std::fmod (deg (l2) + 540., 360.) - 180.;
  }

  // six-character Maidenhead locator for a place
  QString latlon_to_grid (double lat, double lon)
  {
    double const x = qBound (0., lon + 180., 359.99999);
    double const y = qBound (0., lat + 90., 179.99999);
    QString g;
    g += QChar ('A' + static_cast<int> (x / 20.));
    g += QChar ('A' + static_cast<int> (y / 10.));
    g += QChar ('0' + static_cast<int> (std::fmod (x, 20.) / 2.));
    g += QChar ('0' + static_cast<int> (std::fmod (y, 10.)));
    g += QChar ('a' + static_cast<int> (std::fmod (x, 2.) * 12.));
    g += QChar ('a' + static_cast<int> (std::fmod (y, 1.) * 24.));
    return g;
  }

  QString latlon_text (double lat, double lon)
  {
    return QString {"%1%2%3 %4%5%6"}
      .arg (std::abs (lat), 0, 'f', 1).arg (QChar {0x00B0}).arg (lat >= 0 ? 'N' : 'S')
      .arg (std::abs (lon), 0, 'f', 1).arg (QChar {0x00B0}).arg (lon >= 0 ? 'E' : 'W');
  }

  // call signs are shown with a slashed zero, like a QSL card
  QString slashed (QString call)
  {
    return call.replace ('0', QChar {0x00D8});
  }

  // X-ray class (A, B, C, M, X) and a colour level for a flux in W/m2
  QString xray_class (double flux, int * level = nullptr)
  {
    char cls = 'A';
    double base = 1e-8;
    if (flux >= 1e-4) {cls = 'X'; base = 1e-4;}
    else if (flux >= 1e-5) {cls = 'M'; base = 1e-5;}
    else if (flux >= 1e-6) {cls = 'C'; base = 1e-6;}
    else if (flux >= 1e-7) {cls = 'B'; base = 1e-7;}
    if (level) *level = cls == 'X' ? 0 : cls == 'M' ? 1 : 2;
    return QString {"%1%2"}.arg (cls).arg (flux / base, 0, 'f', 1);
  }

  // the NCDXF/IARU beacons, in the order they transmit; each sends for
  // ten seconds on 14.100, then moves up to 18.110 and so on, so the
  // whole cycle takes three minutes
  struct BeaconDef {char const * call; char const * place; char const * grid;};
  BeaconDef const ncdxf[18] = {
    {"4U1UN", "United Nations, New York", "FN30AS"},
    {"VE8AT", "Inuvik, Canada", "CP38GH"},
    {"W6WX", "Mt. Umunhum, California", "CM97BD"},
    {"KH6RS", "Maui, Hawaii", "BL10TS"},
    {"ZL6B", "Masterton, New Zealand", "RE78TW"},
    {"VK6RBP", "Rolystone, Australia", "OF87AV"},
    {"JA2IGY", "Mt. Asama, Japan", "PM84JK"},
    {"RR9O", "Novosibirsk, Russia", "NO14KX"},
    {"VR2B", "Hong Kong", "OL72BG"},
    {"4S7B", "Colombo, Sri Lanka", "MJ96WV"},
    {"ZS6DN", "Pretoria, South Africa", "KG44DC"},
    {"5Z4B", "Kikuyu, Kenya", "KI88KS"},
    {"4X6TU", "Tel Aviv, Israel", "KM72JB"},
    {"OH2B", "Lohja, Finland", "KP20EH"},
    {"CS3B", "Madeira", "IM12JT"},
    {"LU4AA", "Buenos Aires, Argentina", "GF05TJ"},
    {"OA4B", "Lima, Peru", "FH17MW"},
    {"YV5B", "Caracas, Venezuela", "FJ69CC"},
  };
  char const * const beacon_freqs[5] = {"14.100", "18.110", "21.150", "24.930", "28.200"};
  QColor beacon_color (int band)
  {
    static QColor const colors[5] = {QColor {"#4FC3F7"}, QColor {"#81C784"}, QColor {"#FFD54F"},
                                     QColor {"#FF8A65"}, QColor {"#F06292"}};
    return band >= 0 && band < 5 ? colors[band] : QColor {"#9AA4B2"};
  }
  // which beacon is on band b (0 = 14.100) in this ten-second slot
  int beacon_on (int slot, int band) {return ((slot - band) % 18 + 18) % 18;}

  // coastlines, borders and state lines (Natural Earth 50 m, public
  // domain) as lines of longitude/latitude, and the land as polygons, so
  // the map stays sharp however far in it's zoomed
  struct MapVectors
  {
    QVector<QPolygonF> lines[3];       // coast, country borders, states and provinces
    QPainterPath land;                 // in degrees: x longitude, y latitude
  };
  MapVectors const& map_vectors ()
  {
    static MapVectors v;
    static bool loaded = false;
    if (loaded) return v;
    loaded = true;
    auto read_u32 = [] (QByteArray const& b, int& at) {
      quint32 n = 0;
      if (at + 4 <= b.size ()) memcpy (&n, b.constData () + at, 4);
      at += 4;
      return qFromLittleEndian (n);
    };
    auto read_pt = [] (QByteArray const& b, int& at) {
      qint16 xy[2] = {0, 0};
      if (at + 4 <= b.size ()) memcpy (xy, b.constData () + at, 4);
      at += 4;
      return QPointF {qFromLittleEndian (xy[0]) / 100., qFromLittleEndian (xy[1]) / 100.};
    };
    QFile lf {":/map_lines.bin"};
    if (lf.open (QIODevice::ReadOnly))
      {
        auto const b = lf.readAll ();
        int at = 4;
        quint32 const sets = read_u32 (b, at);
        for (quint32 s = 0; s < sets && s < 3; ++s)
          {
            quint32 const n = read_u32 (b, at);
            for (quint32 i = 0; i < n && at < b.size (); ++i)
              {
                quint32 const m = read_u32 (b, at);
                QPolygonF line;
                line.reserve (m);
                for (quint32 k = 0; k < m; ++k) line << read_pt (b, at);
                v.lines[s] << line;
              }
          }
      }
    QFile pf {":/map_land.bin"};
    if (pf.open (QIODevice::ReadOnly))
      {
        auto const b = pf.readAll ();
        int at = 4;
        quint32 const rings = read_u32 (b, at);
        v.land.setFillRule (Qt::OddEvenFill);
        for (quint32 i = 0; i < rings && at < b.size (); ++i)
          {
            quint32 const m = read_u32 (b, at);
            QPolygonF ring;
            ring.reserve (m);
            for (quint32 k = 0; k < m; ++k) ring << read_pt (b, at);
            v.land.addPolygon (ring);
            v.land.closeSubpath ();
          }
      }
    return v;
  }

  // the colours for VOACAP reliability, 0 to 1
  // CQ and ITU zones (HB9HIL/hamradio-zones-geojson, MIT licence), in
  // degrees: x longitude, y latitude
  struct Zone {int number; QPointF label; QVector<QPolygonF> rings;};
  QVector<Zone> const& zones (int set)        // 0 CQ, 1 ITU
  {
    static QVector<Zone> z[2];
    static bool loaded = false;
    if (!loaded)
      {
        loaded = true;
        QFile f {":/map_zones.bin"};
        if (f.open (QIODevice::ReadOnly))
          {
            auto const b = f.readAll ();
            int at = 4;
            auto u32 = [&b, &at] {quint32 n = 0; if (at + 4 <= b.size ()) memcpy (&n, b.constData () + at, 4); at += 4; return qFromLittleEndian (n);};
            auto pt = [&b, &at] {
              qint16 xy[2] = {0, 0};
              if (at + 4 <= b.size ()) memcpy (xy, b.constData () + at, 4);
              at += 4;
              return QPointF {qFromLittleEndian (xy[0]) / 100., qFromLittleEndian (xy[1]) / 100.};
            };
            quint32 const sets = u32 ();
            for (quint32 k = 0; k < sets && k < 2; ++k)
              {
                quint32 const n = u32 ();
                for (quint32 i = 0; i < n && at < b.size (); ++i)
                  {
                    Zone zone;
                    zone.number = int (u32 ());
                    zone.label = pt ();
                    quint32 const rings = u32 ();
                    for (quint32 r = 0; r < rings && at < b.size (); ++r)
                      {
                        quint32 const m = u32 ();
                        QPolygonF ring;
                        for (quint32 j = 0; j < m; ++j) ring << pt ();
                        zone.rings << ring;
                      }
                    z[k] << zone;
                  }
              }
          }
      }
    return z[qBound (0, set, 1)];
  }

  // colours for MHz: DRAP's absorbed frequencies, the MUF
  QColor mhz_colour (double mhz, double top)
  {
    struct Stop {double at; QColor c;};
    static Stop const stops[] = {{0., QColor {40, 40, 200}}, {0.25, QColor {0, 170, 255}}, {0.45, QColor {0, 220, 110}},
                                 {0.65, QColor {255, 225, 0}}, {0.85, QColor {255, 120, 0}}, {1., QColor {255, 30, 60}}};
    double const v = qBound (0., mhz / top, 1.);
    for (int i = 0; i < 5; ++i)
      {
        if (v <= stops[i + 1].at)
          {
            double const t = (v - stops[i].at) / (stops[i + 1].at - stops[i].at);
            auto const& a = stops[i].c;
            auto const& b = stops[i + 1].c;
            return QColor::fromRgbF (a.redF () + (b.redF () - a.redF ()) * t, a.greenF () + (b.greenF () - a.greenF ()) * t,
                                     a.blueF () + (b.blueF () - a.blueF ()) * t);
          }
      }
    return stops[5].c;
  }

  QColor VoacapGrid_colour (double v)
  {
    struct Stop {double at; QColor c;};
    // green is a sure thing (100%), fading through yellow and blue down
    // to red for no chance (0%)
    static Stop const stops[] = {{0., QColor {255, 40, 40}}, {0.33, QColor {0, 120, 255}},
                                 {0.66, QColor {255, 225, 0}}, {1., QColor {0, 230, 70}}};
    int const last = sizeof stops / sizeof stops[0] - 1;
    v = qBound (0., v, 1.);
    for (int i = 0; i < last; ++i)
      {
        if (v <= stops[i + 1].at)
          {
            double const t = (v - stops[i].at) / (stops[i + 1].at - stops[i].at);
            auto const& a = stops[i].c;
            auto const& b = stops[i + 1].c;
            return QColor::fromRgbF (a.redF () + (b.redF () - a.redF ()) * t, a.greenF () + (b.greenF () - a.greenF ()) * t,
                                     a.blueF () + (b.blueF () - a.blueF ()) * t);
          }
      }
    return stops[last].c;
  }

  // places for the names shown when you point at the map
  struct City {QString name; double lat; double lon; QString label; bool capital;};
  QVector<City> const& cities ()
  {
    static QVector<City> list;
    if (list.isEmpty ())
      {
        QFile file {":/map_cities.txt"};
        if (file.open (QIODevice::ReadOnly | QIODevice::Text))
          {
            QTextStream in {&file};
            in.setCodec ("UTF-8");
            while (!in.atEnd ())
              {
                auto const line = in.readLine ();
                if (line.startsWith ('#')) continue;
                auto const parts = line.split ('\t');
                if (parts.size () < 5) continue;
                QString name = parts[0];
                if (parts[1].size () && parts[1] != parts[0]) name += ", " + parts[1];
                name += ", " + parts[2];
                list << City {name, parts[3].toDouble (), parts[4].toDouble (), parts[0], parts.size () > 6 && parts[6] == "1"};
              }
          }
      }
    return list;
  }
}

//
// The map. Four looks: satellite pictures of the Earth by day with the
// lights of the cities at night (NASA Blue Marble and Black Marble,
// public domain), the same by day only, terrain with the sea floor
// (NASA Blue Marble with bathymetry), or plain land and borders (Natural
// Earth, public domain) tinted to suit the colour scheme. Drawn flat, or
// centred on you (azimuthal equidistant: straight lines from the middle
// are beam headings, and the distance from the middle is the distance
// from you). Optional overlays: gray line, a lat/long or Maidenhead grid,
// NOAA aurora forecast, NASA rain and snow and the NCDXF beacons.
// Stations heard in the last 30 minutes are drawn in the colours the
// main window highlights them with.
//
class ConditionsMap final
  : public QWidget
{
public:
  enum Style {DayNight = 0, Satellite = 1, Countries = 2, Terrain = 3};
  enum Projection {Flat = 0, Azimuthal = 1, Robinson = 2, NearHalf = 3};
  enum Grid {NoGrid = 0, LatLong = 1, Maidenhead = 2, MaidenheadFine = 3, Tropics = 4, Beams = 5, CQZones = 6, ITUZones = 7};
  struct Beacon {QString call; QString place; double lat; double lon; int band;};

  explicit ConditionsMap (QWidget * parent)
    : QWidget {parent}
    , land_ {QImage {":/map_land.png"}.convertToFormat (QImage::Format_Grayscale8)}
    , net_ {new QNetworkAccessManager {this}}
    , detail_timer_ {new QTimer {this}}
  {
    setMinimumSize (360, 180);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking (true);
    // when zoomed in, sharper pictures for just the part in view
    detail_timer_->setSingleShot (true);
    detail_timer_->setInterval (450);
    QObject::connect (detail_timer_, &QTimer::timeout, [this] {request_detail ();});
    for (auto const& b : ncdxf)
      {
        double lat, lon;
        ConditionsWindow::grid_to_latlon (b.grid, lat, lon);
        beacons << Beacon {b.call, b.place, lat, lon, -1};
      }
    retint ();
    update_night ();
  }

  // options
  Style style {DayNight};
  Projection projection {Flat};
  Grid grid {NoGrid};
  bool gray_line {true};
  bool show_aurora {false};
  bool show_rain {false};
  bool show_beacons {false};
  bool show_cities {false};
  bool show_prop {false};
  QString prop_label;                // what the propagation layer shows
  bool show_cq {true};
  bool show_me {true};
  bool show_called {true};
  bool show_others {true};
  int worked_mode {0};               // 0 worked before, 1 worked today, 2 hidden
  bool show_worked_grids {false};
  bool show_psk {false};
  QString band_label;                // the band the radio is on, e.g. "20m"
  QSet<QString> worked_calls;        // in the log on this band
  QSet<QString> worked_grids;        // four-character grids in the log on this band
  QSet<QString> today_calls;         // the same, logged since 00:00 UTC
  QSet<QString> today_grids;
  QSet<QString> const& calls_shown () const {return 1 == worked_mode ? today_calls : worked_calls;}
  QSet<QString> all_grids;           // the same on every band
  QSet<QString> today_all_grids;
  bool grids_all_bands {false};
  QSet<QString> const& grids_shown () const
  {
    if (grids_all_bands) return 1 == worked_mode ? today_all_grids : all_grids;
    return 1 == worked_mode ? today_grids : worked_grids;
  }
  QColor grid_color;                 // invalid: the scheme's second accent
  int grid_opacity {40};             // percent
  struct Spot {QString call; QString grid; QString band; double lat; double lon; int snr; double mhz; QDateTime when;};
  QVector<Spot> psk_spots;           // stations that heard you (PSK Reporter), the ones shown
  QDateTime psk_updated;
  QString psk_error;
  bool psk_all_bands {false};        // every band, each in its own colour
  bool psk_live {false};             // the live feed is connected
  QDateTime psk_last;                // when the last report came in
  int psk_minutes {30};
  QColor psk_strong {"#3DDC84"}, psk_fair {"#FFD54A"}, psk_weak {"#FF7A45"};
  QHash<QString, QColor> band_colors;
  QColor others_color;               // invalid: grey
  QHash<QString, Spot> psk_store;    // every report kept, one per station and band

  // the reports to draw: recent enough, and on this band unless all bands
  void rebuild_psk ()
  {
    auto const cutoff = QDateTime::currentDateTimeUtc ().addSecs (-60 * qMax (60, psk_minutes));
    for (auto it = psk_store.begin (); it != psk_store.end (); )
      {
        if (it->when < cutoff) it = psk_store.erase (it);
        else ++it;
      }
    auto const shown_from = QDateTime::currentDateTimeUtc ().addSecs (-60 * psk_minutes);
    psk_spots.clear ();
    for (auto const& sp : psk_store)
      {
        if (sp.when < shown_from) continue;
        if (!psk_all_bands && !band_label.isEmpty () && sp.band != band_label) continue;
        psk_spots << sp;
      }
  }
  bool miles {false};
  QColor cq_color {"#3FB950"};
  QColor my_call_color {"#FF4D4D"};
  QColor tx_color {"#FFD54A"};
  QColor worked_color {"#B58CFF"};
  QHash<QString, Station> stations;
  QSet<QString> called;              // stations you transmitted to
  QVector<Beacon> beacons;
  std::function<void (QString const&, Qt::KeyboardModifiers)> on_double_click;
  std::function<void (int)> on_legend;        // a legend chip clicked: 0 CQ, 1 you, 2 called, 3 others, 4 worked
  std::function<void (QString const&, QString const&)> on_colour;   // a colour in the legend double-clicked: key, name

  // something under the stations changed: build the picture again
  void changed ()
  {
    composite_ = scaled_ = projected_ = overlay_ = QImage {};
    scaled_key_.clear ();
    projected_key_.clear ();
    if (detail_timer_) detail_timer_->start ();
    grid_key_.clear ();
    update ();
  }

  void set_style (Style s)
  {
    style = s;
    if ((DayNight == style || Satellite == style) && day_.isNull ())
      {
        // load the satellite pictures the first time they're wanted
        day_ = QImage {":/map_day.jpg"}.convertToFormat (QImage::Format_RGB32);
        night_lights_ = QImage {":/map_night.jpg"}.convertToFormat (QImage::Format_RGB32);
        day_small_ = day_.scaled (1024, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        night_small_ = night_lights_.scaled (1024, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
      }
    if (Terrain == style && terrain_.isNull ())
      {
        terrain_ = QImage {":/map_terrain.jpg"}.convertToFormat (QImage::Format_RGB32);
      }
    rebuild_base ();
  }

  void retint ()
  {
    bool const dark = Win11Theme::isDark ();
    auto const pal = palette ();
    // dark schemes: land and borders take on the scheme's accent colour
    // (red land in Night Red, green in Digi Green and so on)
    auto mix = [] (QColor const& a, QColor const& b, double f) {
      return QColor::fromRgbF (a.redF () * (1 - f) + b.redF () * f,
                               a.greenF () * (1 - f) + b.greenF () * f,
                               a.blueF () * (1 - f) + b.blueF () * f);
    };
    auto const window = pal.window ().color ();
    auto const accent = pal.highlight ().color ();
    // dark schemes: clearly lighter land than sea, bright borders
    QColor const sea = dark ? window.darker (150) : QColor {"#BFD7EA"};
    QColor const land_color = dark ? mix (QColor {"#2B3442"}, accent, 0.38) : QColor {"#E8E3C8"};
    QColor const border_color = dark ? mix (accent, QColor {"#FFFFFF"}, 0.45) : QColor {"#8A8466"};
    sea_color_ = sea;
    land_color_ = land_color;
    border_color_ = border_color;
    countries_ = QImage {land_.size (), QImage::Format_ARGB32_Premultiplied};
    countries_.fill (sea);
    QPainter p {&countries_};
    p.drawImage (0, 0, tinted (land_, land_color));
    rebuild_base ();
  }

  void update_night ()
  {
    // 360 x 180: one pixel per degree, smoothed when drawn
    int const W = 360, H = 180;
    night_ = QImage {W, H, QImage::Format_ARGB32_Premultiplied};
    shade_.resize (W * H);
    double slat, slon;
    subsolar_point (QDateTime::currentDateTimeUtc (), slat, slon);
    sun_lat_ = slat;
    sun_lon_ = slon;
    double const sd = std::sin (rad (slat)), cd = std::cos (rad (slat));
    QVector<double> coslon (W);
    for (int x = 0; x < W; ++x) coslon[x] = std::cos (rad (-180. + x + 0.5 - slon));
    for (int y = 0; y < H; ++y)
      {
        double const lat = 90. - y - 0.5;
        double const a = std::sin (rad (lat)) * sd, b = std::cos (rad (lat)) * cd;
        auto * line = reinterpret_cast<QRgb *> (night_.scanLine (y));
        for (int x = 0; x < W; ++x)
          {
            double const el = deg (std::asin (qBound (-1., a + b * coslon[x], 1.)));
            // full darkness 12 degrees below the horizon, fading up to sunrise
            double const f = qBound (0., (0.8 - el) / 12.8, 1.);
            shade_[y * W + x] = f;
            line[x] = qPremultiply (qRgba (0, 0, 20, static_cast<int> (f * 195)));
          }
      }
    rebuild_base ();
  }

  void set_my (double lat, double lon)
  {
    my_lat_ = lat;
    my_lon_ = lon;
    lut_key_.clear ();                // the centred map moves with you
    changed ();
  }
  void set_dx (QString const& call, double lat, double lon) {dx_call_ = call; dx_lat_ = lat; dx_lon_ = lon; update ();}

  void set_aurora (QImage const& img) {aurora_ = img; changed ();}
  // the map coloured by DRAP or the MUF: the picture, what it is and the top of its scale
  void set_conditions (QImage const& img, QString const& label, double top) {cond_ = img; cond_label = label; cond_top = top; changed ();}
  int colour_by {0};                  // 0 nothing, 1 DRAP, 2 MUF now
  QString cond_label;
  double cond_top {35.};
  QVector<QPair<QPointF, double>> muf_points;   // ionosondes: where, MUF
  void set_rain (QImage const& img) {rain_ = img; changed ();}
  void set_prop (QImage const& img) {prop_ = img; changed ();}

  void set_projection (Projection p)
  {
    if (p == projection) return;
    projection = p;
    reset_view ();
  }

  // the flat map's zoom with the whole world in view, poles and all
  double min_zoom () const
  {
    if (Flat != projection || width () <= 0 || height () <= 0) return 1.;
    return qMin (width () / 360., height () / 180.) / qMax (width () / 360., height () / 180.);
  }

  // Fit: the whole world, pole to pole
  void fit_world ()
  {
    zoom_ = min_zoom ();
    clon_ = 0.;
    clat_ = 0.;
    az_pan_ = QPointF {};
    clamp_view ();
    view_changed ();
  }

  // back to the whole map
  void reset_view ()
  {
    zoom_ = 1.;
    clon_ = 0.;
    clat_ = 12.;                       // a little north: more land, less Antarctica
    az_pan_ = QPointF {};
    clamp_view ();
    view_changed ();
  }

protected:
  void resizeEvent (QResizeEvent *) override
  {
    clamp_view ();
    view_changed ();
  }

  void view_changed ()
  {
    scaled_key_.clear ();
    projected_key_.clear ();
    grid_key_.clear ();
    if (detail_timer_) detail_timer_->start ();
    update ();
  }

  // ---- zoom and pan ----
  // flat: pixels per degree; at zoom 1 the map just covers the widget
  double flat_scale () const {return zoom_ * qMax (width () / 360., height () / 180.);}
  QRectF world_rect () const
  {
    double const s = flat_scale ();
    return {width () / 2. + (-180. - clon_) * s, height () / 2. - (90. - clat_) * s, 360. * s, 180. * s};
  }
  // centred: the radius of the world and where its middle is
  double az_radius () const
  {
    if (Robinson == projection) return zoom_ * qMin ((width () - 4.) / 5.3326, (height () - 4.) / 2.7046);
    return zoom_ * (qMin (width (), height ()) / 2. - 2.);
  }
  bool round_world () const {return Azimuthal == projection || NearHalf == projection;}
  // how far from you the edge of a centred map is
  double edge_distance () const {return NearHalf == projection ? pi / 2. : pi;}
  // whether a place is on the side of the world this view shows
  bool on_view (double lat, double lon, double margin = 0.995) const
  {
    if (!round_world ()) return true;
    double dist, bearing;
    path_to (centre_lat (), centre_lon (), lat, lon, dist, bearing);
    return dist <= edge_distance () * margin;
  }

  // Robinson: lengths of the parallels and their heights, every 5 degrees
  static double robinson_x (double lat)
  {
    static double const X[] = {1.0000, 0.9986, 0.9954, 0.9900, 0.9822, 0.9730, 0.9600, 0.9427, 0.9216, 0.8962,
                               0.8679, 0.8350, 0.7986, 0.7597, 0.7186, 0.6732, 0.6213, 0.5722, 0.5322};
    double const a = qBound (0., std::abs (lat) / 5., 17.9999);
    int const i = int (a);
    return X[i] + (X[i + 1] - X[i]) * (a - i);
  }
  static double robinson_y (double lat)
  {
    static double const Y[] = {0.0000, 0.0620, 0.1240, 0.1860, 0.2480, 0.3100, 0.3720, 0.4340, 0.4958, 0.5571,
                               0.6176, 0.6769, 0.7346, 0.7903, 0.8435, 0.8936, 0.9394, 0.9761, 1.0000};
    double const a = qBound (0., std::abs (lat) / 5., 17.9999);
    int const i = int (a);
    double const y = Y[i] + (Y[i + 1] - Y[i]) * (a - i);
    return lat < 0 ? -y : y;
  }
  static double robinson_lat (double y)
  {
    static double const Y[] = {0.0000, 0.0620, 0.1240, 0.1860, 0.2480, 0.3100, 0.3720, 0.4340, 0.4958, 0.5571,
                               0.6176, 0.6769, 0.7346, 0.7903, 0.8435, 0.8936, 0.9394, 0.9761, 1.0000};
    double const ay = std::abs (y);
    int i = 0;
    while (i < 17 && Y[i + 1] < ay) ++i;
    double const lat = 5. * (i + (ay - Y[i]) / (Y[i + 1] - Y[i]));
    return y < 0 ? -lat : lat;
  }
  // the outline of the Robinson world
  QPainterPath robinson_outline () const
  {
    QPainterPath path;
    QVector<QPointF> pts;
    for (int lat = -90; lat <= 90; lat += 2) pts << to_point (lat, 180.);
    for (int lat = 90; lat >= -90; lat -= 2) pts << to_point (lat, -180.);
    path.addPolygon (QPolygonF {pts});
    path.closeSubpath ();
    return path;
  }
  QPointF az_centre () const {return QPointF {width () / 2., height () / 2.} + az_pan_;}

  void clamp_view ()
  {
    if (width () <= 0 || height () <= 0) return;
    zoom_ = qMax (zoom_, min_zoom ());
    double const s = flat_scale ();
    double const hw = width () / 2. / s, hh = height () / 2. / s;
    clon_ = hw >= 180. ? 0. : qBound (-180. + hw, clon_, 180. - hw);
    clat_ = hh >= 90. ? 0. : qBound (-90. + hh, clat_, 90. - hh);
    double const r = az_radius ();
    double const len = std::hypot (az_pan_.x (), az_pan_.y ());
    if (zoom_ <= 1.0001) az_pan_ = QPointF {};
    else if (len > r) az_pan_ *= r / len;
  }

  void zoom_at (QPointF const& pt, double factor)
  {
    double const nz = qBound (min_zoom (), zoom_ * factor, 12.);
    if (std::abs (nz - zoom_) < 1e-6) return;
    if (Flat == projection)
      {
        double lat, lon;
        bool const ok = from_point (pt, lat, lon);
        zoom_ = nz;
        if (ok)
          {
            double const s = flat_scale ();
            clon_ = lon - (pt.x () - width () / 2.) / s;
            clat_ = lat + (pt.y () - height () / 2.) / s;
          }
      }
    else
      {
        auto const c = az_centre ();
        double const k = nz / zoom_;
        az_pan_ = pt - (pt - c) * k - QPointF {width () / 2., height () / 2.};
        zoom_ = nz;
      }
    clamp_view ();
    view_changed ();
  }

  void wheelEvent (QWheelEvent * e) override
  {
#if QT_VERSION >= QT_VERSION_CHECK (5, 14, 0)
    auto const pos = e->position ();
#else
    auto const pos = e->posF ();
#endif
    zoom_at (pos, std::pow (1.0015, e->angleDelta ().y ()));
    e->accept ();
  }

  void mouseReleaseEvent (QMouseEvent * e) override
  {
    if (dragging_)
      {
        dragging_ = false;
        setCursor (Qt::ArrowCursor);
        view_changed ();
      }
    pressed_ = false;
    QWidget::mouseReleaseEvent (e);
  }

  // the flat map: the picture (sharper where there is one for the
  // view), the overlays, then the lines
  void draw_flat (QPainter& p, bool quick)
  {
    QRectF const world = world_rect ();
    QRectF const vis = world & QRectF {rect ()};
    auto from_rect = [&world, &vis] (QImage const& img) {
      return QRectF {(vis.left () - world.left ()) / world.width () * img.width (),
          (vis.top () - world.top ()) / world.height () * img.height (),
          vis.width () / world.width () * img.width (),
          vis.height () / world.height () * img.height ()};
    };
    p.setRenderHint (QPainter::SmoothPixmapTransform, !quick);
    if (Countries == style)
      {
        // land and sea drawn as shapes: sharp at any zoom
        p.fillRect (vis, sea_color_);
        double const s = flat_scale ();
        QTransform t;
        t.translate (width () / 2. - clon_ * s, height () / 2. + clat_ * s);
        t.scale (s, -s);
        p.save ();
        p.setRenderHint (QPainter::Antialiasing, !quick);
        p.setTransform (t, true);
        p.setPen (Qt::NoPen);
        p.setBrush (land_color_);
        p.drawPath (map_vectors ().land);
        p.restore ();
      }
    else
      {
        p.drawImage (vis, base_, from_rect (base_));
        if (detail_.style == style && !detail_.image.isNull ())
          {
            auto const tl = to_point (detail_.lat1, detail_.lon0);
            auto const br = to_point (detail_.lat0, detail_.lon1);
            p.drawImage (QRectF {tl, br}, detail_.image);
          }
      }
    auto const& ov = overlay ();
    p.drawImage (vis, ov, from_rect (ov));
    draw_lines (p, quick);
  }

  // + - and Fit buttons in the top right corner
  void draw_zoom_buttons (QPainter& p)
  {
    zoom_rects_.clear ();
    double const b = 28, gap = 6;
    double x = width () - 12 - b;
    QString const labels[] = {QObject::tr ("Fit"), QString {QChar {0x2212}}, "+"};
    auto f = font ();
    f.setBold (true);
    p.setFont (f);
    for (int i = 0; i < 3; ++i)
      {
        QRectF const r {x - (i == 0 ? 10 : 0), 12, b + (i == 0 ? 10 : 0), b};
        zoom_rects_.prepend (r);
        p.setPen (QPen {QColor {255, 255, 255, 40}, 1});
        p.setBrush (QColor {13, 17, 23, 190});
        p.drawRoundedRect (r, 9, 9);
        p.setPen (i == 0 && zoom_ <= min_zoom () + 0.0001 ? QColor {233, 236, 241, 90} : QColor {233, 236, 241});
        p.drawText (r, Qt::AlignCenter, labels[i]);
        x -= b + gap + (i == 0 ? 10 : 0);
      }
  }


  void paintEvent (QPaintEvent *) override
  {
    QPainter p {this};
    p.setRenderHint (QPainter::Antialiasing);
    auto const r = map_rect ();
    // rounded corners; the round world too when centred on you
    QPainterPath shape;
    shape.addRoundedRect (QRectF {rect ()}, 12, 12);
    if (round_world ())
      {
        QPainterPath world;
        world.addEllipse (az_centre (), az_radius (), az_radius ());
        shape = shape.intersected (world);
      }
    else if (Robinson == projection)
      {
        shape = shape.intersected (robinson_outline ());
      }
    else if (zoom_ < 0.9999)
      {
        // zoomed out to the whole world: nothing in the margins beside it
        QPainterPath world;
        world.addRect (world_rect ());
        shape = shape.intersected (world);
      }
    p.setClipPath (shape);
    if (Flat == projection)
      {
        auto const key = QString {"%1/%2/%3/%4x%5"}.arg (zoom_).arg (clon_).arg (clat_).arg (width ()).arg (height ());
        if (dragging_)
          {
            draw_flat (p, true);                 // quick while the map is moving
          }
        else
          {
            if (scaled_key_ != key)
              {
                // the view drawn once, then reused until something changes
                scaled_ = QImage {size () * devicePixelRatioF (), QImage::Format_ARGB32_Premultiplied};
                scaled_.setDevicePixelRatio (devicePixelRatioF ());
                scaled_.fill (Qt::transparent);
                QPainter sp {&scaled_};
                sp.setRenderHint (QPainter::Antialiasing);
                draw_flat (sp, false);
                scaled_key_ = key;
              }
            p.drawImage (0, 0, scaled_);
          }
      }
    else
      {
        auto const key = QString {"%1/%2,%3/%4x%5/%6,%7"}.arg (zoom_).arg (az_pan_.x ()).arg (az_pan_.y ())
          .arg (width ()).arg (height ()).arg (centre_lat ()).arg (centre_lon ());
        if (dragging_ && !projected_.isNull ())
          {
            p.drawImage (az_pan_ - projected_pan_, projected_);   // shift the last picture while moving
          }
        else
          {
            if (projected_key_ != key) {project (); projected_key_ = key; projected_pan_ = az_pan_;}
            p.drawImage (0, 0, projected_);
          }
        if (!dragging_) draw_lines (p, false);
      }
    if (grid != NoGrid) draw_grid (p, r);

    // the sun
    auto const sun = to_point (sun_lat_, sun_lon_);
    p.setPen (QPen {QColor {"#FFB000"}, 2});
    p.setBrush (QColor {"#FFD54A"});
    p.drawEllipse (sun, 7, 7);

    if (show_worked_grids) draw_worked_grids (p);
    if (show_cities) draw_cities (p);
    if (show_beacons) draw_beacons (p);
    if (show_psk) draw_psk (p);

    // great circle to the station being worked
    if (!qIsNaN (my_lat_) && !qIsNaN (dx_lat_))
      {
        QPen pen {QColor {Win11Theme::token ("accent")}, 2.2};
        pen.setDashPattern ({4., 3.});
        p.setPen (pen);
        draw_great_circle (p, my_lat_, my_lon_, dx_lat_, dx_lon_);
      }

    // stations heard in the last 30 minutes, in Band Activity's colours
    auto const now = QDateTime::currentDateTimeUtc ();
    QColor const neutral = others_color.isValid () ? others_color : Win11Theme::isDark () ? QColor {"#9AA4B2"} : QColor {"#5A6472"};
    for (auto it = stations.cbegin (); it != stations.cend (); ++it)
      {
        auto const& s = it.value ();
        double const age = s.heard.secsTo (now) / 1800.;
        if (age > 1.) continue;
        bool const was_called = called.contains (it.key ());
        QColor c;
        if (2 != worked_mode && calls_shown ().contains (it.key ()))
          {
            // logged: the worked colour from the moment it's in the log
            c = worked_color;
          }
        else if (s.to_me)
          {
            if (!show_me) continue;
            c = my_call_color;
          }
        else if (was_called)
          {
            if (!show_called) continue;
            c = tx_color;
          }
        else if (s.cq)
          {
            if (!show_cq) continue;
            c = s.highlight.isValid () ? s.highlight : cq_color;
          }
        else
          {
            if (!show_others) continue;
            c = s.highlight.isValid () ? s.highlight : neutral;
          }
        c.setAlphaF (qBound (0.35, 1. - age * 0.65, 1.));
        auto const pt = to_point (s.lat, s.lon);
        double const radius = s.to_me || was_called ? 5.5 : 4.5;
        // a soft glow round each dot
        QColor halo = c;
        halo.setAlphaF (c.alphaF () * 0.28);
        p.setPen (Qt::NoPen);
        p.setBrush (halo);
        p.drawEllipse (pt, radius + 4, radius + 4);
        if (s.approx)
          {
            // only the country is known: a faint ring, not a dot
            c.setAlphaF (c.alphaF () * 0.55);
            p.setPen (QPen {c, 2.});
            p.setBrush (Qt::NoBrush);
            p.drawEllipse (pt, radius, radius);
          }
        else
          {
            p.setPen (QPen {QColor {0, 0, 0, 170}, 1});
            p.setBrush (c);
            p.drawEllipse (pt, radius, radius);
          }
      }

    // home: a rounded square in the scheme's accent colour
    if (!qIsNaN (my_lat_))
      {
        auto const home = to_point (my_lat_, my_lon_);
        QColor const accent {Win11Theme::token ("accent")};
        QColor glow = accent;
        glow.setAlpha (70);
        p.setPen (Qt::NoPen);
        p.setBrush (glow);
        p.drawEllipse (home, 13, 13);
        p.setPen (QPen {Qt::white, 2.5});
        p.setBrush (accent);
        p.drawRoundedRect (QRectF {home.x () - 6, home.y () - 6, 12, 12}, 4, 4);
      }
    if (!qIsNaN (dx_lat_))
      {
        auto const dx = to_point (dx_lat_, dx_lon_);
        p.setPen (QPen {Qt::white, 2});
        p.setBrush (my_call_color);
        p.drawEllipse (dx, 6, 6);
        auto f = font ();
        f.setBold (true);
        outlined_text (p, dx + QPointF {10, 5}, slashed (dx_call_), f, Qt::white);
      }
    p.setClipping (false);
    if (round_world () || Robinson == projection)
      {
        p.setPen (QPen {QColor {Win11Theme::token ("border")}, 1});
        p.setBrush (Qt::NoBrush);
        if (Robinson == projection) p.drawPath (robinson_outline ());
        else p.drawEllipse (az_centre (), az_radius (), az_radius ());
      }
    draw_legend (p, r);
    draw_layer_notes (p);
    draw_zoom_buttons (p);
    if (show_prop) draw_prop_scale (p);
    if (colour_by && !cond_.isNull ()) draw_mhz_scale (p);
  }

  // what the DRAP or MUF colours mean, bottom right (above Propagation's)
  void draw_mhz_scale (QPainter& p)
  {
    auto f = font ();
    f.setPointSizeF (qMax (6.5, f.pointSizeF () * 0.85));
    QFontMetricsF fm {f};
    double const bar_w = 140, h = fm.height () + 8;
    auto const ends = QObject::tr ("0–%1 MHz").arg (cond_top, 0, 'f', 0);
    double const w = fm.boundingRect (cond_label).width () + bar_w + fm.boundingRect (ends).width () + 40;
    double const lift = show_prop ? h + 20 : 0.;
    QRectF const panel {width () - w - 12., height () - h - 36. - lift, w, h + 12};
    p.setPen (QPen {QColor {255, 255, 255, 40}, 1});
    p.setBrush (QColor {13, 17, 23, 190});
    p.drawRoundedRect (panel, 12, 12);
    p.setFont (f);
    p.setPen (QColor {233, 236, 241});
    double x = panel.left () + 12;
    double const mid = panel.center ().y ();
    p.drawText (QPointF {x, mid + fm.ascent () / 2 - 1}, cond_label);
    x += fm.boundingRect (cond_label).width () + 8;
    QLinearGradient g {QPointF {x, 0}, QPointF {x + bar_w, 0}};
    for (int i = 0; i <= 5; ++i) g.setColorAt (i / 5., mhz_colour (cond_top * i / 5., cond_top));
    p.setPen (Qt::NoPen);
    p.setBrush (g);
    p.drawRoundedRect (QRectF {x, mid - 4, bar_w, 8}, 4, 4);
    p.setPen (QColor {233, 236, 241, 190});
    p.drawText (QPointF {x + bar_w + 8, mid + fm.ascent () / 2 - 1}, ends);
  }

  // what the propagation colours mean, bottom right
  void draw_prop_scale (QPainter& p)
  {
    auto f = font ();
    f.setPointSizeF (qMax (6.5, f.pointSizeF () * 0.85));
    QFontMetricsF fm {f};
    double const bar_w = 120, h = fm.height () + 8;
    auto const ends = QObject::tr ("0–100%");
    double const w = fm.boundingRect (prop_label).width () + bar_w + fm.boundingRect (ends).width () + 40;
    QRectF const panel {width () - w - 12., height () - h - 36., w, h + 12};
    p.setPen (QPen {QColor {255, 255, 255, 40}, 1});
    p.setBrush (QColor {13, 17, 23, 190});
    p.drawRoundedRect (panel, 12, 12);
    p.setFont (f);
    p.setPen (QColor {233, 236, 241});
    double x = panel.left () + 12;
    double const mid = panel.center ().y ();
    p.drawText (QPointF {x, mid + fm.ascent () / 2 - 1}, prop_label);
    x += fm.boundingRect (prop_label).width () + 8;
    QLinearGradient g {QPointF {x, 0}, QPointF {x + bar_w, 0}};
    for (int i = 0; i <= 4; ++i) g.setColorAt (i / 4., VoacapGrid_colour (i / 4.));
    p.setPen (Qt::NoPen);
    p.setBrush (g);
    p.drawRoundedRect (QRectF {x, mid - 4, bar_w, 8}, 4, 4);
    p.setPen (QColor {233, 236, 241, 190});
    p.drawText (QPointF {x + bar_w + 8, mid + fm.ascent () / 2 - 1}, ends);
  }

  // which stations are shown: chips in a see-through bar on the map;
  // click one to hide or show those stations
  void draw_legend (QPainter& p, QRect const& r)
  {
    QColor const neutral = others_color.isValid () ? others_color : Win11Theme::isDark () ? QColor {"#9AA4B2"} : QColor {"#5A6472"};
    struct Item {QString text; QColor color; bool on;};
    Item const items[] = {
      {QObject::tr ("CQ"), cq_color, show_cq},
      {QObject::tr ("Calling you"), my_call_color, show_me},
      {QObject::tr ("You called"), tx_color, show_called},
      {QObject::tr ("Others"), neutral, show_others},
      {1 == worked_mode ? QObject::tr ("Worked today") : QObject::tr ("Worked before"), worked_color, 2 != worked_mode},
    };
    auto f = font ();
    f.setPointSizeF (qMax (6.5, f.pointSizeF () * 0.85));
    QFontMetricsF fm {f};
    double const h = fm.height () + 8;
    double x = 8;
    legend_rects_.clear ();
    key_rects_.clear ();
    QVector<QRectF> chips;
    for (auto const& it : items)
      {
        double const w = fm.boundingRect (it.text).width () + 26;
        chips << QRectF {x, 0, w, h};
        x += w + 6;
      }
    auto const ring_text = QObject::tr ("Country only");
    double const ring_w = fm.boundingRect (ring_text).width () + 22;
    double const panel_w = x + ring_w + 4;
    Q_UNUSED (r);
    QPointF const origin {12., height () - h - 24.};
    QRectF const panel {origin.x (), origin.y (), panel_w, h + 12};
    p.setPen (QPen {QColor {255, 255, 255, 40}, 1});
    p.setBrush (QColor {13, 17, 23, 190});
    p.drawRoundedRect (panel, 12, 12);
    p.setFont (f);
    for (int i = 0; i < chips.size (); ++i)
      {
        auto const cr = chips[i].translated (origin + QPointF {0, 6});
        legend_rects_ << cr;
        p.setPen (Qt::NoPen);
        p.setBrush (items[i].on ? QColor {255, 255, 255, 30} : QColor {0, 0, 0, 0});
        p.drawRoundedRect (cr, h / 2, h / 2);
        QColor dot = items[i].color;
        if (!items[i].on) dot.setAlpha (90);
        p.setBrush (dot);
        p.drawEllipse (QPointF {cr.left () + 11, cr.center ().y ()}, 4, 4);
        p.setPen (items[i].on ? QColor {233, 236, 241} : QColor {233, 236, 241, 110});
        auto tf = f;
        tf.setStrikeOut (!items[i].on);
        p.setFont (tf);
        p.drawText (QPointF {cr.left () + 19, cr.center ().y () + fm.ascent () / 2 - 1}, items[i].text);
      }
    p.setFont (f);
    if (show_psk)
      {
        // what the Heard me line colours mean: strength, or the band when
        // every band is shown
        struct Key {QString text; QColor color; QString colour_key;};
        QVector<Key> keys;
        if (psk_all_bands)
          {
            QSet<QString> present;
            for (auto const& sp : psk_spots) present << sp.band;
            for (auto const& b : band_order ())
              {
                if (present.contains (b)) keys << Key {b, band_color (b), "Band_" + b};
              }
            if (keys.isEmpty ()) keys << Key {QObject::tr ("no reports yet"), QColor {"#9AA4B2"}, QString {}};
          }
        else
          {
            keys << Key {QObject::tr ("Strong  %1 dB or better").arg (QString {"\u2212"} + "5"), psk_strong, "Strong"}
                 << Key {QObject::tr ("Fair  %1 to %2 dB").arg (QString {"\u2212"} + "6", QString {"\u2212"} + "15"), psk_fair, "Fair"}
                 << Key {QObject::tr ("Weak  below %1 dB").arg (QString {"\u2212"} + "15"), psk_weak, "Weak"};
          }
        auto bold = f;
        bold.setBold (true);
        QFontMetricsF const bfm {bold};
        auto const title = QObject::tr ("Heard me:");
        double kw = 14 + bfm.horizontalAdvance (title) + 10;
        for (auto const& k : keys) kw += 24 + fm.horizontalAdvance (k.text) + 12;
        QRectF const key_panel {panel.right () + 8, panel.top (), kw, panel.height ()};
        p.setPen (QPen {QColor {255, 255, 255, 40}, 1});
        p.setBrush (QColor {13, 17, 23, 190});
        p.drawRoundedRect (key_panel, 12, 12);
        double kx = key_panel.left () + 14;
        double const cy = key_panel.center ().y ();
        p.setFont (bold);
        p.setPen (QColor {233, 236, 241});
        p.drawText (QPointF {kx, cy + fm.ascent () / 2 - 1}, title);
        kx += bfm.horizontalAdvance (title) + 10;
        p.setFont (f);
        for (auto const& k : keys)
          {
            QColor const c = k.color;
            p.setPen (QPen {c, 2.5});
            p.drawLine (QPointF {kx, cy}, QPointF {kx + 16, cy});
            p.setPen (QPen {QColor {0, 0, 0, 190}, 1});
            p.setBrush (c);
            QPolygonF d;
            d << QPointF {kx + 8, cy - 4} << QPointF {kx + 12, cy} << QPointF {kx + 8, cy + 4} << QPointF {kx + 4, cy};
            p.drawPolygon (d);
            p.setPen (QColor {233, 236, 241});
            p.drawText (QPointF {kx + 22, cy + fm.ascent () / 2 - 1}, k.text);
            if (k.colour_key.size ())
              {
                key_rects_ << qMakePair (QRectF {kx - 3, key_panel.top () + 3, 24 + fm.horizontalAdvance (k.text) + 6, key_panel.height () - 6},
                                         qMakePair (k.colour_key, k.text.section ("  ", 0, 0)));
              }
            kx += 24 + fm.horizontalAdvance (k.text) + 12;
          }
      }
    QPointF const ring {origin.x () + x + 10, origin.y () + 6 + h / 2};
    p.setPen (QPen {QColor {233, 236, 241, 170}, 1.5});
    p.setBrush (Qt::NoBrush);
    p.drawEllipse (ring, 3.5, 3.5);
    p.setPen (QColor {233, 236, 241, 170});
    p.drawText (QPointF {ring.x () + 8, ring.y () + fm.ascent () / 2 - 1}, ring_text);
  }

  void mousePressEvent (QMouseEvent * e) override
  {
    for (int i = 0; i < zoom_rects_.size (); ++i)
      {
        if (zoom_rects_[i].contains (e->pos ()))
          {
            QPointF const middle {width () / 2., height () / 2.};
            if (0 == i) zoom_at (middle, 1.5);
            else if (1 == i) zoom_at (middle, 1. / 1.5);
            else fit_world ();
            return;
          }
      }
    for (int i = 0; i < legend_rects_.size (); ++i)
      {
        if (legend_rects_[i].contains (e->pos ()))
          {
            if (on_legend) on_legend (i);
            return;
          }
      }
    if (Qt::LeftButton == e->button ())
      {
        // the start of a drag to move the map
        pressed_ = true;
        press_pos_ = e->pos ();
        press_clon_ = clon_;
        press_clat_ = clat_;
        press_pan_ = az_pan_;
      }
    QWidget::mousePressEvent (e);
  }

  // the visible station nearest a point, within a few pixels
  QString station_at (QPointF const& pos) const
  {
    auto const now = QDateTime::currentDateTimeUtc ();
    QString best;
    double best_d = 100.;     // 10 pixels
    for (auto it = stations.cbegin (); it != stations.cend (); ++it)
      {
        if (it->heard.secsTo (now) > 1800 || !shown (it.key (), it.value ())) continue;
        auto const pt = to_point (it->lat, it->lon);
        double const d = std::pow (pt.x () - pos.x (), 2) + std::pow (pt.y () - pos.y (), 2);
        if (d < best_d) {best_d = d; best = it.key ();}
      }
    return best;
  }

  int beacon_at (QPointF const& pos) const
  {
    if (!show_beacons) return -1;
    for (int i = 0; i < beacons.size (); ++i)
      {
        auto const pt = to_point (beacons[i].lat, beacons[i].lon);
        if (std::pow (pt.x () - pos.x (), 2) + std::pow (pt.y () - pos.y (), 2) < 81.) return i;
      }
    return -1;
  }

  bool shown (QString const& call, Station const& s) const
  {
    if (s.to_me) return show_me;
    if (called.contains (call)) return show_called;
    if (2 != worked_mode && calls_shown ().contains (call)) return true;
    if (s.cq) return show_cq;
    return show_others;
  }

  void mouseDoubleClickEvent (QMouseEvent * e) override
  {
    // a colour in the legend or the Heard me key: change it
    static char const * const legend_keys[] = {"CQ", "CallingMe", "Called", "Others", "Worked"};
    for (int i = 0; i < legend_rects_.size () && i < 5; ++i)
      {
        if (legend_rects_[i].contains (e->pos ()))
          {
            // the first click switched it: put that back
            if (on_legend)
              {
                on_legend (i);
                if (4 == i) on_legend (i);
              }
            if (on_colour) on_colour (legend_keys[i], QString {});
            return;
          }
      }
    for (auto const& k : key_rects_)
      {
        if (k.first.contains (e->pos ()))
          {
            if (on_colour) on_colour (k.second.first, k.second.second);
            return;
          }
      }
    auto const call = station_at (e->pos ());
    if (call.size () && on_double_click) on_double_click (call, e->modifiers ());
  }

  void mouseMoveEvent (QMouseEvent * e) override
  {
    if (pressed_ && (e->buttons () & Qt::LeftButton))
      {
        QPoint const d = e->pos () - press_pos_;
        if (!dragging_ && d.manhattanLength () > 4 && (zoom_ > 1.0001 || Flat == projection)) dragging_ = true;
        if (dragging_)
          {
            if (Flat == projection)
              {
                double const s = flat_scale ();
                clon_ = press_clon_ - d.x () / s;
                clat_ = press_clat_ + d.y () / s;
              }
            else
              {
                az_pan_ = press_pan_ + d;
              }
            clamp_view ();
            setCursor (Qt::ClosedHandCursor);
            QToolTip::hideText ();
            update ();
            return;
          }
      }
    bool over_legend = false;
    for (auto const& lr : legend_rects_) over_legend = over_legend || lr.contains (e->pos ());
    for (auto const& zr : zoom_rects_) over_legend = over_legend || zr.contains (e->pos ());
    for (auto const& k : key_rects_) over_legend = over_legend || k.first.contains (e->pos ());
    setCursor (over_legend || station_at (e->pos ()).size () ? Qt::PointingHandCursor : Qt::ArrowCursor);
    // keep the place under the pointer up to date while a tip is showing
    if (QToolTip::isVisible ()) show_tip (e->pos (), e->globalPos ());
    QWidget::mouseMoveEvent (e);
  }

  bool event (QEvent * e) override
  {
    if (e->type () == QEvent::ToolTip)
      {
        auto * help = static_cast<QHelpEvent *> (e);
        show_tip (help->pos (), help->globalPos ());
        return true;
      }
    return QWidget::event (e);
  }

  void show_tip (QPoint const& pos, QPoint const& global)
  {
    for (int i = 0; i < legend_rects_.size (); ++i)
      {
        if (legend_rects_[i].contains (pos))
          {
            QToolTip::showText (global, QObject::tr ("Click to show or hide. Double-click to change the colour."), this);
            return;
          }
      }
    for (auto const& k : key_rects_)
      {
        if (k.first.contains (pos))
          {
            QToolTip::showText (global, QObject::tr ("Double-click to change the colour"), this);
            return;
          }
      }
    auto const call = station_at (pos);
    if (call.size ())
      {
        auto const& s = stations[call];
        QString what;
        if (2 != worked_mode && calls_shown ().contains (call))
          what = 1 == worked_mode ? QObject::tr ("worked today on %1").arg (band_label) : QObject::tr ("worked before on %1").arg (band_label);
        else if (s.to_me) what = QObject::tr ("calling you");
        else if (called.contains (call)) what = QObject::tr ("you called");
        else if (s.cq) what = QObject::tr ("calling CQ");
        QToolTip::showText (global,
                            QString {"<b>%1</b> &nbsp;%2 dB &nbsp;%3 UTC &nbsp;%4<br/>%5"}
                            .arg (slashed (call)).arg (s.snr).arg (s.heard.toString ("hh:mm")).arg (what)
                            .arg ((s.approx ? QObject::tr ("Location approximate: no grid heard yet, shown at the middle of the country") + "<br/>" : QString {})
                                 + QObject::tr ("Double-click to call")), this);
        return;
      }
    int const spot = psk_at (pos);
    if (spot >= 0)
      {
        auto const& sp = psk_spots[spot];
        double dist = 0., bearing = 0.;
        if (!qIsNaN (my_lat_)) path_to (my_lat_, my_lon_, sp.lat, sp.lon, dist, bearing);
        double const km = dist * 6371.;
        int const mins = int (sp.when.secsTo (QDateTime::currentDateTimeUtc ()) / 60);
        QToolTip::showText (global,
                            QObject::tr ("<b>%1</b> heard you at %2 dB<br/>%3 MHz (%7) · %4 %5 · %6 min ago<br/><i>PSK Reporter</i>")
                            .arg (slashed (sp.call)).arg (sp.snr).arg (sp.mhz, 0, 'f', 3)
                            .arg (qRound (miles ? km * 0.621371 : km)).arg (miles ? "mi" : "km").arg (mins).arg (sp.band), this);
        return;
      }
    int const b = beacon_at (pos);
    if (b >= 0)
      {
        auto const& beacon = beacons[b];
        QString text = QString {"<b>%1</b> &nbsp;%2<br/>"}.arg (slashed (beacon.call)).arg (beacon.place);
        if (beacon.band >= 0) text += QObject::tr ("Sending on %1 MHz now").arg (beacon_freqs[beacon.band]);
        else text += QObject::tr ("NCDXF beacon, not sending this moment");
        QToolTip::showText (global, text, this);
        return;
      }
    double lat, lon;
    if (!from_point (pos, lat, lon))
      {
        QToolTip::hideText ();
        return;
      }
    QString text;
    // the nearest city within a few pixels
    double best_d = 144.;
    for (auto const& c : cities ())
      {
        auto const pt = to_point (c.lat, c.lon);
        double const d = std::pow (pt.x () - pos.x (), 2) + std::pow (pt.y () - pos.y (), 2);
        if (d < best_d) {best_d = d; text = "<b>" + c.name.toHtmlEscaped () + "</b><br/>";}
      }
    text += QObject::tr ("Grid %1 &nbsp;%2").arg (latlon_to_grid (lat, lon)).arg (latlon_text (lat, lon));
    if (!qIsNaN (my_lat_))
      {
        double dist, bearing;
        path_to (my_lat_, my_lon_, lat, lon, dist, bearing);
        double const km = dist * 6371.;
        text += "<br/>" + QObject::tr ("%1 %2 from you, heading %3%4")
          .arg (QLocale {}.toString (qRound (miles ? km * 0.621371 : km)))
          .arg (miles ? QObject::tr ("miles") : QObject::tr ("km"))
          .arg (qRound (bearing)).arg (QChar {0x00B0});
      }
    QToolTip::showText (global, text, this);
  }

private:
  void rebuild_base ()
  {
    switch (style)
      {
      case Countries:
        base_ = countries_;
        break;

      case Terrain:
        if (terrain_.isNull ()) {base_ = countries_; break;}
        base_ = terrain_.convertToFormat (QImage::Format_ARGB32_Premultiplied);
        break;

      case Satellite:
        if (day_.isNull ()) {base_ = countries_; break;}
        base_ = day_.convertToFormat (QImage::Format_ARGB32_Premultiplied);
        break;

      case DayNight:
        {
          if (day_.isNull () || shade_.isEmpty ()) {base_ = countries_; break;}
          // blend the day picture into the city lights across the gray line
          base_ = QImage {day_.size (), QImage::Format_RGB32};
          int const W = base_.width (), H = base_.height ();
          for (int y = 0; y < H; ++y)
            {
              auto const * d = reinterpret_cast<QRgb const *> (day_.constScanLine (y));
              auto const * n = reinterpret_cast<QRgb const *> (night_lights_.constScanLine (y));
              auto * out = reinterpret_cast<QRgb *> (base_.scanLine (y));
              int const sy = qMin (179, y * 180 / H);
              for (int x = 0; x < W; ++x)
                {
                  double const f = shade_[sy * 360 + qMin (359, x * 360 / W)];
                  out[x] = qRgb (static_cast<int> (qRed (d[x]) * (1 - f) + qRed (n[x]) * f),
                                 static_cast<int> (qGreen (d[x]) * (1 - f) + qGreen (n[x]) * f),
                                 static_cast<int> (qBlue (d[x]) * (1 - f) + qBlue (n[x]) * f));
                }
            }
          base_ = base_.convertToFormat (QImage::Format_ARGB32_Premultiplied);
        }
        break;
      }
    if (DayNight == style && detail_.style == DayNight) blend_detail ();    // the gray line has moved
    changed ();
  }

  // the shading, rain, propagation and aurora on a clear picture of the
  // whole world, drawn over whichever map is under it
  QImage const& overlay ()
  {
    if (overlay_.isNull ())
      {
        overlay_ = QImage {2048, 1024, QImage::Format_ARGB32_Premultiplied};
        overlay_.fill (Qt::transparent);
        QPainter p {&overlay_};
        p.setRenderHint (QPainter::SmoothPixmapTransform);
        auto const all = overlay_.rect ();
        // the day/night picture already shows the night side
        if (gray_line && style != DayNight)
          {
            // lighter shading on the plain map so the land still shows at night
            p.setOpacity (Countries == style ? 0.6 : 1.);
            p.drawImage (all, night_);
            p.setOpacity (1.);
          }
        if (colour_by && !cond_.isNull ())
          {
            p.setOpacity (0.62);
            p.drawImage (all, cond_);
            p.setOpacity (1.);
          }
        if (show_rain && !rain_.isNull ()) p.drawImage (all, rain_);
        if (show_prop && !prop_.isNull ()) p.drawImage (all, prop_);
        if (show_aurora && !aurora_.isNull ()) p.drawImage (all, aurora_);
      }
    return overlay_;
  }

  // the whole world: map and overlays together
  QImage const& composite ()
  {
    if (composite_.isNull ())
      {
        composite_ = base_.convertToFormat (QImage::Format_ARGB32_Premultiplied);
        if (composite_.constBits () == base_.constBits ()) composite_ = base_.copy ();
        QPainter p {&composite_};
        p.setRenderHint (QPainter::SmoothPixmapTransform);
        p.drawImage (composite_.rect (), overlay ());
      }
    return composite_;
  }

  // ---- sharper pictures when zoomed in: NASA GIBS (public) ----
  struct Detail
  {
    QImage image;
    double lon0 {0}, lat0 {0}, lon1 {0}, lat1 {0};
    int style {-1};
    double ppd {0};                    // pixels per degree
  };

  // the longitudes and latitudes in view, or false when the whole world is
  bool view_bounds (double& lon0, double& lat0, double& lon1, double& lat1) const
  {
    if (Flat == projection)
      {
        double const s = flat_scale ();
        lon0 = qMax (-180., clon_ - width () / 2. / s);
        lon1 = qMin (180., clon_ + width () / 2. / s);
        lat0 = qMax (-90., clat_ - height () / 2. / s);
        lat1 = qMin (90., clat_ + height () / 2. / s);
        return true;
      }
    // centred: look at points over the widget
    lon0 = lat0 = 1e9;
    lon1 = lat1 = -1e9;
    int const n = 24;
    for (int i = 0; i <= n; ++i)
      {
        for (int j = 0; j <= n; ++j)
          {
            double lat, lon;
            if (!from_point (QPointF {width () * i / double (n), height () * j / double (n)}, lat, lon)) continue;
            lon0 = qMin (lon0, lon); lon1 = qMax (lon1, lon);
            lat0 = qMin (lat0, lat); lat1 = qMax (lat1, lat);
          }
      }
    if (lon1 <= lon0 || lat1 <= lat0) return false;
    // round a pole or across the date line: take every longitude
    if (lon1 - lon0 > 200.) {lon0 = -180.; lon1 = 180.;}
    return true;
  }

  double view_ppd () const
  {
    double const dpr = devicePixelRatioF ();
    if (Robinson == projection) return az_radius () * 0.8487 * pi / 180. * dpr;
    if (NearHalf == projection) return az_radius () / 90. * dpr;
    return (Flat == projection ? flat_scale () : az_radius () / 180.) * dpr;
  }

  QString gibs_url (QString const& layer, double lon0, double lat0, double lon1, double lat1, int w, int h) const
  {
    QUrlQuery q;
    q.addQueryItem ("SERVICE", "WMS");
    q.addQueryItem ("REQUEST", "GetMap");
    q.addQueryItem ("VERSION", "1.3.0");
    q.addQueryItem ("LAYERS", layer);
    q.addQueryItem ("STYLES", "");
    q.addQueryItem ("CRS", "EPSG:4326");
    // WMS 1.3.0 with EPSG:4326 takes latitude first
    q.addQueryItem ("BBOX", QString {"%1,%2,%3,%4"}.arg (lat0, 0, 'f', 4).arg (lon0, 0, 'f', 4).arg (lat1, 0, 'f', 4).arg (lon1, 0, 'f', 4));
    q.addQueryItem ("WIDTH", QString::number (w));
    q.addQueryItem ("HEIGHT", QString::number (h));
    q.addQueryItem ("FORMAT", "image/jpeg");
    QUrl url {"https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi"};
    url.setQuery (q);
    return url.toString ();
  }

  void request_detail ()
  {
    if (Countries == style || zoom_ < 1.6)
      {
        // the whole world's pictures are sharp enough
        if (!detail_.image.isNull ()) {detail_ = Detail {}; view_changed_keep_detail ();}
        return;
      }
    double lon0, lat0, lon1, lat1;
    if (!view_bounds (lon0, lat0, lon1, lat1)) return;
    double const v_lon0 = lon0, v_lon1 = lon1, v_lat0 = lat0, v_lat1 = lat1;
    // a margin round the view so a little panning needs nothing new
    double const mx = (lon1 - lon0) * 0.15, my = (lat1 - lat0) * 0.15;
    lon0 = qMax (-180., lon0 - mx); lon1 = qMin (180., lon1 + mx);
    lat0 = qMax (-90., lat0 - my); lat1 = qMin (90., lat1 + my);
    double use_ppd = view_ppd ();
    int w = qRound ((lon1 - lon0) * use_ppd), h = qRound ((lat1 - lat0) * use_ppd);
    int const biggest = 4096;
    if (qMax (w, h) > biggest)
      {
        use_ppd *= double (biggest) / qMax (w, h);
        w = qRound ((lon1 - lon0) * use_ppd);
        h = qRound ((lat1 - lat0) * use_ppd);
      }
    // already have it?
    if (detail_.style == style && !detail_.image.isNull () && detail_.ppd >= use_ppd * 0.85
        && detail_.lon0 <= v_lon0 && detail_.lon1 >= v_lon1 && detail_.lat0 <= v_lat0 && detail_.lat1 >= v_lat1) return;
    if (w < 16 || h < 16) return;
    QStringList layers;
    if (Terrain == style) layers << "BlueMarble_ShadedRelief_Bathymetry";
    else layers << "BlueMarble_NextGeneration";
    if (DayNight == style) layers << "VIIRS_Black_Marble";
    for (auto& r : detail_replies_) if (r) r->abort ();
    detail_replies_.clear ();
    auto pending = std::make_shared<QVector<QImage>> (layers.size ());
    auto left = std::make_shared<int> (layers.size ());
    int const for_style = style;
    for (int i = 0; i < layers.size (); ++i)
      {
        QNetworkRequest req {QUrl {gibs_url (layers[i], lon0, lat0, lon1, lat1, w, h)}};
        req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
        auto * reply = net_->get (req);
        detail_replies_ << reply;
        QObject::connect (reply, &QNetworkReply::finished, this, [=] {
            reply->deleteLater ();
            if (QNetworkReply::NoError == reply->error ()) (*pending)[i].loadFromData (reply->readAll ());
            if (--*left) return;
            if (for_style != style) return;
            for (auto const& img : *pending) if (img.isNull ()) return;     // offline: keep the whole-world pictures
            detail_ = Detail {};
            detail_.lon0 = lon0; detail_.lon1 = lon1; detail_.lat0 = lat0; detail_.lat1 = lat1;
            detail_.style = for_style;
            detail_.ppd = use_ppd;
            detail_day_ = (*pending)[0].convertToFormat (QImage::Format_RGB32);
            detail_night_ = pending->size () > 1 ? (*pending)[1].convertToFormat (QImage::Format_RGB32) : QImage {};
            if (DayNight == for_style) blend_detail ();
            else detail_.image = detail_day_.convertToFormat (QImage::Format_ARGB32_Premultiplied);
            view_changed_keep_detail ();
          });
      }
  }

  // day and night pictures of the part in view, blended across the gray line
  void blend_detail ()
  {
    if (detail_day_.isNull () || detail_night_.isNull () || shade_.isEmpty ()) return;
    int const W = detail_day_.width (), H = detail_day_.height ();
    auto const night = detail_night_.scaled (W, H);
    QImage out {W, H, QImage::Format_RGB32};
    for (int y = 0; y < H; ++y)
      {
        double const lat = detail_.lat1 - (y + 0.5) / H * (detail_.lat1 - detail_.lat0);
        int const sy = qBound (0, static_cast<int> (90. - lat), 179);
        auto const * d = reinterpret_cast<QRgb const *> (detail_day_.constScanLine (y));
        auto const * n = reinterpret_cast<QRgb const *> (night.constScanLine (y));
        auto * o = reinterpret_cast<QRgb *> (out.scanLine (y));
        for (int x = 0; x < W; ++x)
          {
            double const lon = detail_.lon0 + (x + 0.5) / W * (detail_.lon1 - detail_.lon0);
            int const sx = qBound (0, static_cast<int> (lon + 180.), 359);
            double const f = shade_[sy * 360 + sx];
            o[x] = qRgb (static_cast<int> (qRed (d[x]) * (1 - f) + qRed (n[x]) * f),
                         static_cast<int> (qGreen (d[x]) * (1 - f) + qGreen (n[x]) * f),
                         static_cast<int> (qBlue (d[x]) * (1 - f) + qBlue (n[x]) * f));
          }
      }
    detail_.image = out.convertToFormat (QImage::Format_ARGB32_Premultiplied);
  }

  void view_changed_keep_detail ()
  {
    scaled_key_.clear ();
    projected_key_.clear ();
    update ();
  }

  // coastlines, borders and (zoomed in) state lines, sharp at any zoom
  void draw_lines (QPainter& p, bool quick)
  {
    auto const& v = map_vectors ();
    bool const plain = Countries == style;
    QColor const coast = plain ? border_color_ : QColor {255, 255, 255, 120};
    QColor border = plain ? border_color_ : QColor {255, 255, 255, 150};
    QColor state = plain ? border_color_ : QColor {255, 255, 255, 90};
    if (plain) state.setAlphaF (0.45);
    bool const states = zoom_ >= 2.5;
    p.save ();
    p.setRenderHint (QPainter::Antialiasing, !quick);
    p.setBrush (Qt::NoBrush);
    if (Flat == projection)
      {
        double const s = flat_scale ();
        QTransform t;
        t.translate (width () / 2. - clon_ * s, height () / 2. + clat_ * s);
        t.scale (s, -s);
        p.setTransform (t, true);
        auto pen_for = [] (QColor const& c, double w) {QPen pen {c, w}; pen.setCosmetic (true); return pen;};
        p.setPen (pen_for (coast, 0.9));
        for (auto const& l : v.lines[0]) p.drawPolyline (l);
        p.setPen (pen_for (border, 1.0));
        for (auto const& l : v.lines[1]) p.drawPolyline (l);
        if (states)
          {
            p.setPen (pen_for (state, 0.7));
            for (auto const& l : v.lines[2]) p.drawPolyline (l);
          }
      }
    else
      {
        auto const key = QString {"%1/%2,%3/%4x%5/%6,%7/%8"}.arg (zoom_).arg (az_pan_.x ()).arg (az_pan_.y ())
          .arg (width ()).arg (height ()).arg (centre_lat ()).arg (centre_lon ()).arg (states);
        if (key != az_lines_key_)
          {
            az_lines_key_ = key;
            for (int k = 0; k < 3; ++k)
              {
                az_lines_[k] = QPainterPath {};
                if (2 == k && !states) continue;
                for (auto const& l : v.lines[k])
                  {
                    QVector<QPointF> pts;
                    pts.reserve (l.size ());
                    for (auto const& pt : l) pts << to_point (pt.y (), pt.x ());
                    add_line (az_lines_[k], pts);
                  }
              }
          }
        p.setPen (QPen {coast, 0.9});
        p.drawPath (az_lines_[0]);
        p.setPen (QPen {border, 1.0});
        p.drawPath (az_lines_[1]);
        if (states)
          {
            p.setPen (QPen {state, 0.7});
            p.drawPath (az_lines_[2]);
          }
      }
    p.restore ();
  }

  double centre_lat () const {return qIsNaN (my_lat_) ? 0. : my_lat_;}
  double centre_lon () const {return qIsNaN (my_lon_) ? 0. : my_lon_;}

  // the flat picture redrawn centred on you; a table of which source
  // pixel goes where is worked out once for each size
  void project ()
  {
    int const w = width (), h = height ();
    auto const lut_key = QString {"%1/%2,%3/%4x%5/%6,%7/%8"}.arg (zoom_).arg (az_pan_.x ()).arg (az_pan_.y ())
      .arg (w).arg (h).arg (centre_lat ()).arg (centre_lon ()).arg (projection);
    if (lut_key_ != lut_key)
      {
        // where on Earth each pixel is, worked out once for each view
        lut_key_ = lut_key;
        lut_lat_.resize (w * h);
        lut_lon_.resize (w * h);
        double const p0 = rad (centre_lat ()), l0 = rad (centre_lon ());
        double const sp0 = std::sin (p0), cp0 = std::cos (p0);
        auto const c = az_centre ();
        double const R = az_radius ();
        for (int y = 0; y < h && !round_world (); ++y)
          {
            // Robinson and the others: one place at a time
            for (int x = 0; x < w; ++x)
              {
                double lat, lon;
                if (!from_point (QPointF {x + 0.5, y + 0.5}, lat, lon)) {lut_lat_[y * w + x] = 999.f; continue;}
                lut_lat_[y * w + x] = static_cast<float> (lat);
                lut_lon_[y * w + x] = static_cast<float> (lon);
              }
          }
        double const reach = edge_distance ();
        for (int y = 0; y < h && round_world (); ++y)
          {
            for (int x = 0; x < w; ++x)
              {
                double const dx = (x + 0.5 - c.x ()) / R, dy = (y + 0.5 - c.y ()) / R;
                double const rho = std::sqrt (dx * dx + dy * dy);
                if (rho > 1.) {lut_lat_[y * w + x] = 999.f; continue;}
                double const ang = rho * reach, az = std::atan2 (dx, -dy);
                double const sc = std::sin (ang), cc = std::cos (ang);
                double const lat = std::asin (qBound (-1., sp0 * cc + cp0 * sc * std::cos (az), 1.));
                double const lon = l0 + std::atan2 (std::sin (az) * sc * cp0, cc - sp0 * std::sin (lat));
                lut_lat_[y * w + x] = static_cast<float> (deg (lat));
                lut_lon_[y * w + x] = static_cast<float> (std::fmod (deg (lon) + 540., 360.) - 180.);
              }
          }
      }
    auto const& base = base_;
    auto const& ov = overlay ();
    bool const sharp = detail_.style == style && !detail_.image.isNull ();
    int const BW = base.width (), BH = base.height ();
    int const OW = ov.width (), OH = ov.height ();
    int const DW = sharp ? detail_.image.width () : 0, DH = sharp ? detail_.image.height () : 0;
    projected_ = QImage {w, h, QImage::Format_ARGB32_Premultiplied};
    projected_.fill (Qt::transparent);
    auto const * bin = reinterpret_cast<QRgb const *> (base.constBits ());
    auto const * oin = reinterpret_cast<QRgb const *> (ov.constBits ());
    auto const * din = sharp ? reinterpret_cast<QRgb const *> (detail_.image.constBits ()) : nullptr;
    for (int y = 0; y < h; ++y)
      {
        auto * out = reinterpret_cast<QRgb *> (projected_.scanLine (y));
        for (int x = 0; x < w; ++x)
          {
            float const lat = lut_lat_[y * w + x];
            if (lat > 900.f) continue;
            float const lon = lut_lon_[y * w + x];
            QRgb c;
            if (din && lon >= detail_.lon0 && lon < detail_.lon1 && lat > detail_.lat0 && lat <= detail_.lat1)
              {
                int const sx = qBound (0, static_cast<int> ((lon - detail_.lon0) / (detail_.lon1 - detail_.lon0) * DW), DW - 1);
                int const sy = qBound (0, static_cast<int> ((detail_.lat1 - lat) / (detail_.lat1 - detail_.lat0) * DH), DH - 1);
                c = din[sy * DW + sx];
              }
            else
              {
                int const sx = qBound (0, static_cast<int> ((lon + 180.f) / 360.f * BW), BW - 1);
                int const sy = qBound (0, static_cast<int> ((90.f - lat) / 180.f * BH), BH - 1);
                c = bin[sy * BW + sx];
              }
            // the overlays on top (premultiplied "over")
            int const ox = qBound (0, static_cast<int> ((lon + 180.f) / 360.f * OW), OW - 1);
            int const oy = qBound (0, static_cast<int> ((90.f - lat) / 180.f * OH), OH - 1);
            QRgb const o = oin[oy * OW + ox];
            int const a = qAlpha (o);
            if (a)
              {
                int const ia = 255 - a;
                c = qRgba (qRed (o) + qRed (c) * ia / 255, qGreen (o) + qGreen (c) * ia / 255,
                           qBlue (o) + qBlue (c) * ia / 255, 255);
              }
            out[x] = c | 0xFF000000u;
          }
      }
  }

  QRect map_rect () const
  {
    if (round_world ())
      {
        double const R = az_radius ();
        return QRectF {az_centre () - QPointF {R, R}, QSizeF {2 * R, 2 * R}}.toAlignedRect ();
      }
    if (Robinson == projection)
      {
        double const R = az_radius ();
        return QRectF {az_centre () - QPointF {2.6663 * R, 1.3523 * R}, QSizeF {5.3326 * R, 2.7046 * R}}.toAlignedRect ();
      }
    return world_rect ().toAlignedRect ();
  }

public:
  QPointF to_point (double lat, double lon) const
  {
    if (Flat == projection)
      {
        double const s = flat_scale ();
        return {width () / 2. + (lon - clon_) * s, height () / 2. - (lat - clat_) * s};
      }
    if (Robinson == projection)
      {
        double const R = az_radius ();
        auto const c = az_centre ();
        double l = lon;
        while (l > 180.) l -= 360.;
        while (l < -180.) l += 360.;
        return {c.x () + 0.8487 * R * robinson_x (lat) * rad (l), c.y () - 1.3523 * R * robinson_y (lat)};
      }
    double dist, bearing;
    path_to (centre_lat (), centre_lon (), lat, lon, dist, bearing);
    double const rho = dist / edge_distance () * az_radius ();
    auto const c = az_centre ();
    return {c.x () + rho * std::sin (rad (bearing)), c.y () - rho * std::cos (rad (bearing))};
  }

  bool from_point (QPointF const& pt, double& lat, double& lon) const
  {
    if (Flat == projection)
      {
        double const s = flat_scale ();
        lon = clon_ + (pt.x () - width () / 2.) / s;
        lat = clat_ - (pt.y () - height () / 2.) / s;
        return lon >= -180. && lon <= 180. && lat >= -90. && lat <= 90.;
      }
    auto const c = az_centre ();
    double const R = az_radius ();
    if (Robinson == projection)
      {
        double const y = (c.y () - pt.y ()) / (1.3523 * R);
        if (std::abs (y) > 1.) return false;
        lat = robinson_lat (y);
        lon = deg ((pt.x () - c.x ()) / (0.8487 * R * robinson_x (lat)));
        return lon >= -180. && lon <= 180.;
      }
    double const dx = (pt.x () - c.x ()) / R, dy = (pt.y () - c.y ()) / R;
    double const rho = std::sqrt (dx * dx + dy * dy);
    if (rho > 1.) return false;
    destination (centre_lat (), centre_lon (), deg (std::atan2 (dx, -dy)), rho * edge_distance (), lat, lon);
    return true;
  }

private:
  // a line through places, broken where it jumps across the map
  void add_line (QPainterPath& path, QVector<QPointF> const& pts) const
  {
    double const jump = map_rect ().width () / (Flat == projection || Robinson == projection ? 2. : 4.);
    for (int i = 0; i < pts.size (); ++i)
      {
        if (0 == i || std::hypot (pts[i].x () - pts[i - 1].x (), pts[i].y () - pts[i - 1].y ()) > jump) path.moveTo (pts[i]);
        else path.lineTo (pts[i]);
      }
  }

  void meridian (QPainterPath& path, double lon, double step) const
  {
    QVector<QPointF> pts;
    for (double lat = -90.; lat <= 90.001; lat += step) pts << to_point (qBound (-89.9, lat, 89.9), lon);
    add_line (path, pts);
  }

  void parallel (QPainterPath& path, double lat, double step) const
  {
    QVector<QPointF> pts;
    for (double lon = -180.; lon <= 180.001; lon += step) pts << to_point (lat, qBound (-179.99, lon, 179.99));
    add_line (path, pts);
  }

  void draw_grid (QPainter& p, QRect const& r)
  {
    // the lines only change with the size, the view or where you are
    auto const key = QString {"%1/%2/%3x%4/%5,%6/%7/%8,%9"}.arg (grid).arg (projection).arg (r.width ()).arg (r.height ())
      .arg (centre_lat ()).arg (centre_lon ()).arg (zoom_).arg (clon_ + az_pan_.x ()).arg (clat_ + az_pan_.y ())
      + QString {"/%1,%2"}.arg (my_lat_).arg (my_lon_);
    if (key != grid_key_)
      {
        grid_key_ = key;
        grid_major_ = grid_minor_ = QPainterPath {};
        double const step = Flat == projection ? 10. : 2.;
        if (LatLong == grid)
          {
            for (int lon = -165; lon <= 180; lon += 15) meridian (grid_major_, lon, step);
            for (int lat = -75; lat <= 75; lat += 15) parallel (grid_major_, lat, step);
          }
        else if (Tropics == grid)
          {
            parallel (grid_major_, 0., step);
            for (double lat : {23.44, -23.44, 66.56, -66.56}) parallel (grid_minor_, lat, step);
          }
        else if (Beams == grid)
          {
            // beam headings every 30 degrees and distance every 2,500 km from you
            if (!qIsNaN (my_lat_))
              {
                for (int b = 0; b < 360; b += 30)
                  {
                    QVector<QPointF> pts;
                    for (double d = 0.; d <= 0.999 * pi; d += pi / 180.)
                      {
                        double lat, lon;
                        destination (my_lat_, my_lon_, b, d, lat, lon);
                        pts << to_point (lat, lon);
                      }
                    add_line (0 == b % 90 ? grid_major_ : grid_minor_, pts);
                  }
                for (double km = 2500.; km < 20000.; km += 2500.)
                  {
                    QVector<QPointF> pts;
                    for (int b = 0; b <= 360; b += 2)
                      {
                        double lat, lon;
                        destination (my_lat_, my_lon_, b, km / 6371., lat, lon);
                        pts << to_point (lat, lon);
                      }
                    add_line (0 == int (km) % 5000 ? grid_major_ : grid_minor_, pts);
                  }
              }
          }
        else if (CQZones == grid || ITUZones == grid)
          {
            for (auto const& z : zones (CQZones == grid ? 0 : 1))
              {
                for (auto const& ring : z.rings)
                  {
                    QVector<QPointF> pts;
                    // long straight edges bend on the centred maps, so fill them in
                    for (int i = 0; i < ring.size (); ++i)
                      {
                        if (i)
                          {
                            auto const a = ring[i - 1], b = ring[i];
                            int const n = Flat == projection ? 1 : qMax (1, int (std::hypot (b.x () - a.x (), b.y () - a.y ()) / 2.));
                            for (int k = 1; k < n; ++k)
                              {
                                pts << to_point (a.y () + (b.y () - a.y ()) * k / n, a.x () + (b.x () - a.x ()) * k / n);
                              }
                          }
                        pts << to_point (ring[i].y (), ring[i].x ());
                      }
                    add_line (grid_major_, pts);
                  }
              }
          }
        else
          {
            for (int lon = -160; lon < 180; lon += 20) meridian (grid_major_, lon, step);
            for (int lat = -80; lat < 90; lat += 10) parallel (grid_major_, lat, step);
            if (MaidenheadFine == grid)
              {
                // the squares inside each field: 2 by 1 degrees
                double const fine = Flat == projection ? 90. : 3.;
                for (int lon = -178; lon < 180; lon += 2) if (lon % 20) meridian (grid_minor_, lon, fine);
                for (int lat = -89; lat < 90; ++lat) if (lat % 10) parallel (grid_minor_, lat, Flat == projection ? 180. : 3.);
              }
          }
      }
    p.setBrush (Qt::NoBrush);
    if (!grid_minor_.isEmpty () && Tropics != grid)
      {
        p.setPen (QPen {QColor {255, 255, 255, Beams == grid ? 55 : 32}, 0.6});
        p.drawPath (grid_minor_);
      }
    if (Tropics == grid)
      {
        QPen pen {QColor {255, 213, 74, 150}, 1};
        pen.setDashPattern ({6., 4.});
        p.setPen (pen);
        p.drawPath (grid_minor_);
      }
    p.setPen (QPen {CQZones == grid || ITUZones == grid ? QColor {255, 235, 140, 150} : QColor {255, 255, 255, 80}, 1});
    p.drawPath (grid_major_);

    auto f = font ();
    f.setPointSizeF (qMax (6., f.pointSizeF () * 0.8));
    QColor const label {255, 255, 255, 170};
    if (LatLong == grid)
      {
        if (Flat != projection || r.width () < 500) return;
        for (int lat = -60; lat <= 60; lat += 30)
          {
            auto const pt = to_point (lat, -180.);
            outlined_text (p, QPointF {qMax (0., pt.x ()) + 4, pt.y () - 3}, QString {"%1%2"}.arg (lat).arg (QChar {0x00B0}), f, label);
          }
        for (int lon = -150; lon <= 150; lon += 30)
          {
            auto const pt = to_point (-90., lon);
            outlined_text (p, QPointF {pt.x () + 3, qMin (double (height ()), pt.y ()) - 5}, QString {"%1%2"}.arg (lon).arg (QChar {0x00B0}), f, label);
          }
        return;
      }
    if (Tropics == grid)
      {
        if (r.width () < 400) return;
        struct L {double lat; char const * text;};
        L const lines[] = {{0., QT_TRANSLATE_NOOP ("ConditionsWindow", "Equator")}, {23.44, QT_TRANSLATE_NOOP ("ConditionsWindow", "Tropic of Cancer")},
                           {-23.44, QT_TRANSLATE_NOOP ("ConditionsWindow", "Tropic of Capricorn")},
                           {66.56, QT_TRANSLATE_NOOP ("ConditionsWindow", "Arctic Circle")}, {-66.56, QT_TRANSLATE_NOOP ("ConditionsWindow", "Antarctic Circle")}};
        for (auto const& l : lines)
          {
            double const lon = Flat == projection ? clon_ - width () / 2. / flat_scale () + 2. : -150.;
            if (!on_view (l.lat, lon, 0.9)) continue;
            auto const pt = to_point (l.lat, qBound (-178., lon, 178.));
            outlined_text (p, QPointF {qMax (4., pt.x () + 4), pt.y () - 3}, QCoreApplication::translate ("ConditionsWindow", l.text), f, label);
          }
        return;
      }
    if (Beams == grid)
      {
        if (qIsNaN (my_lat_) || r.width () < 400) return;
        for (int b = 0; b < 360; b += 30)
          {
            double lat, lon;
            destination (my_lat_, my_lon_, b, 3000. / 6371., lat, lon);
            if (!on_view (lat, lon)) continue;
            outlined_text (p, to_point (lat, lon) + QPointF {3, -3}, QString {"%1%2"}.arg (b).arg (QChar {0x00B0}), f, label);
          }
        bool const miles = QLocale::system ().measurementSystem () == QLocale::ImperialUSSystem;
        for (double km = 5000.; km < 20000.; km += 5000.)
          {
            double lat, lon;
            destination (my_lat_, my_lon_, 15., km / 6371., lat, lon);
            if (!on_view (lat, lon)) continue;
            auto const text = miles ? QString {"%L1 mi"}.arg (qRound (km * 0.621371 / 100.) * 100) : QString {"%L1 km"}.arg (km, 0, 'f', 0);
            outlined_text (p, to_point (lat, lon) + QPointF {3, 12}, text, f, label);
          }
        return;
      }
    if (CQZones == grid || ITUZones == grid)
      {
        if (r.width () < 300) return;
        auto bold = f;
        bold.setBold (true);
        QFontMetricsF const fm {bold};
        for (auto const& z : zones (CQZones == grid ? 0 : 1))
          {
            if (!on_view (z.label.y (), z.label.x (), 0.9)) continue;
            auto const text = QString::number (z.number);
            auto const pt = to_point (z.label.y (), z.label.x ());
            outlined_text (p, pt - QPointF {fm.horizontalAdvance (text) / 2., -fm.ascent () / 2.}, text, bold, QColor {255, 235, 140});
          }
        return;
      }
    if (r.width () < 500) return;
    QVector<QRectF> used;
    for (int i = 0; i < 18; ++i)
      {
        for (int j = 0; j < 18; ++j)
          {
            QString field;
            field += QChar ('A' + i);
            field += QChar ('A' + j);
            if (Flat == projection)
              {
                if (r.width () < 600) return;
                auto const pt = to_point (-90. + (j + 1) * 10., -180. + i * 20.);
                outlined_text (p, pt + QPointF {3, f.pointSizeF () + 3}, field, f, label);
              }
            else
              {
                double const lat = -90. + j * 10. + 5., lon = -180. + i * 20. + 10.;
                if (!on_view (lat, lon, Azimuthal == projection ? 0.6 : 0.9)) continue;      // too squeezed out near the edge
                auto const pt = to_point (lat, lon);
                QFontMetricsF fm {f};
                QRectF const box {pt.x () - fm.boundingRect (field).width () / 2. - 2, pt.y () - fm.ascent () / 2. - 2,
                    fm.boundingRect (field).width () + 4, fm.ascent () + 4};
                // near the pole the fields crowd together; leave out labels that would overlap
                bool crowded = false;
                for (auto const& other : used) if (other.intersects (box)) {crowded = true; break;}
                if (crowded) continue;
                used << box;
                outlined_text (p, pt - QPointF {fm.boundingRect (field).width () / 2., -fm.ascent () / 2.}, field, f, label);
              }
          }
      }
  }

  // grid squares in the log on this band, shaded
  void draw_worked_grids (QPainter& p)
  {
    QColor fill = grid_color.isValid () ? grid_color : QColor {Win11Theme::token ("accent2")};
    QColor edge = fill;
    fill.setAlpha (qBound (5, grid_opacity, 100) * 255 / 100);
    edge.setAlpha (qMin (255, fill.alpha () + 95));
    p.save ();
    p.setRenderHint (QPainter::Antialiasing);
    p.setPen (QPen {edge, 1});
    p.setBrush (fill);
    for (auto const& g : grids_shown ())
      {
        if (g.size () < 4) continue;
        int const f1 = g[0].toUpper ().unicode () - 'A', f2 = g[1].toUpper ().unicode () - 'A';
        int const d1 = g[2].unicode () - '0', d2 = g[3].unicode () - '0';
        if (f1 < 0 || f1 > 17 || f2 < 0 || f2 > 17 || d1 < 0 || d1 > 9 || d2 < 0 || d2 > 9) continue;
        double const lon0 = f1 * 20. - 180. + d1 * 2., lat0 = f2 * 10. - 90. + d2;
        // trace the square's edges, so it bends the right way on the centred map
        QPolygonF poly;
        bool ok = true;
        auto add = [&] (double lat, double lon) {
          if (!on_view (lat, lon, 0.97)) ok = false;
          poly << to_point (lat, lon);
        };
        for (int i = 0; i <= 4; ++i) add (lat0, lon0 + i * 0.5);
        for (int i = 1; i <= 2; ++i) add (lat0 + i * 0.5, lon0 + 2.);
        for (int i = 1; i <= 4; ++i) add (lat0 + 1., lon0 + 2. - i * 0.5);
        for (int i = 1; i < 2; ++i) add (lat0 + 1. - i * 0.5, lon0);
        if (!ok) continue;
        if (Flat == projection)
          {
            // the flat map repeats east and west: draw it where it shows
            double const span = 360. * flat_scale ();
            QRectF const b = poly.boundingRect ();
            for (double shift : {-span, 0., span})
              {
                if (b.translated (shift, 0).intersects (rect ())) p.drawPolygon (poly.translated (shift, 0));
              }
          }
        else p.drawPolygon (poly);
      }
    p.restore ();
  }

  // stations that heard you, from PSK Reporter: a line to each, coloured
  // by how well they heard you
public:
  QColor psk_color (int snr) const
  {
    if (snr >= -5) return psk_strong;
    if (snr >= -15) return psk_fair;
    return psk_weak;
  }

  static QStringList const& band_order ()
  {
    static QStringList const bands {"160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m", "2m", "70cm"};
    return bands;
  }

  static QColor default_band_color (QString const& band)
  {
    static QHash<QString, QColor> const colours {
      {"160m", QColor {"#A78BFA"}}, {"80m", QColor {"#F472B6"}}, {"60m", QColor {"#FB923C"}}, {"40m", QColor {"#EF4444"}},
      {"30m", QColor {"#FACC15"}}, {"20m", QColor {"#3DDC84"}}, {"17m", QColor {"#22D3EE"}}, {"15m", QColor {"#3B82F6"}},
      {"12m", QColor {"#A3E635"}}, {"10m", QColor {"#E879F9"}}, {"6m", QColor {"#F97316"}}, {"2m", QColor {"#FFFFFF"}},
      {"70cm", QColor {"#94A3B8"}},
    };
    return colours.value (band, QColor {"#9AA4B2"});
  }

  QColor band_color (QString const& band) const
  {
    auto const c = band_colors.value (band);
    return c.isValid () ? c : default_band_color (band);
  }

  QColor spot_color (Spot const& sp) const
  {
    return psk_all_bands ? band_color (sp.band) : psk_color (sp.snr);
  }

private:

  void draw_psk (QPainter& p)
  {
    if (qIsNaN (my_lat_)) return;
    p.save ();
    p.setRenderHint (QPainter::Antialiasing);
    for (auto const& sp : psk_spots)
      {
        QColor c = spot_color (sp);
        c.setAlpha (150);
        p.setPen (QPen {c, 1.4});
        draw_great_circle (p, my_lat_, my_lon_, sp.lat, sp.lon);
      }
    for (auto const& sp : psk_spots)
      {
        auto const pt = to_point (sp.lat, sp.lon);
        p.setPen (QPen {QColor {0, 0, 0, 190}, 1});
        p.setBrush (spot_color (sp));
        // a small diamond, so they don't look like stations you hear
        QPolygonF d;
        d << pt + QPointF {0, -5} << pt + QPointF {5, 0} << pt + QPointF {0, 5} << pt + QPointF {-5, 0};
        p.drawPolygon (d);
      }
    p.restore ();
  }

  int psk_at (QPoint const& pos) const
  {
    if (!show_psk) return -1;
    for (int i = 0; i < psk_spots.size (); ++i)
      {
        auto const pt = to_point (psk_spots[i].lat, psk_spots[i].lon);
        if (std::pow (pt.x () - pos.x (), 2) + std::pow (pt.y () - pos.y (), 2) < 49.) return i;
      }
    return -1;
  }

  // a note in the top left corner for the worked grids and PSK Reporter layers
  void draw_layer_notes (QPainter& p)
  {
    QStringList lines;
    if (show_psk)
      {
        auto const now = QDateTime::currentDateTimeUtc ();
        QString head = QObject::tr ("Heard me");
        if (psk_live)
          {
            head += QObject::tr (" · live");
            if (psk_last.isValid ())
              {
                qint64 const secs = qMax<qint64> (0, psk_last.secsTo (now));
                head += secs < 120 ? QObject::tr (" · last report %1 s ago").arg (secs)
                                   : QObject::tr (" · last report %1 min ago").arg (secs / 60);
              }
          }
        QSet<QString> bands;
        for (auto const& sp : psk_spots) bands << sp.band;
        QString const where = psk_all_bands ? QObject::tr ("all bands") : band_label;
        QString const span = QObject::tr ("the last %1 min").arg (psk_minutes);
        if (psk_spots.isEmpty ())
          {
            if (!psk_live && !psk_error.isEmpty ()) lines << QObject::tr ("Heard me: PSK Reporter %1").arg (psk_error);
            else if (!psk_live && !psk_updated.isValid ()) lines << QObject::tr ("Heard me: asking PSK Reporter who heard you…");
            else lines << head + QObject::tr (" · nobody reported hearing you on %1 in %2").arg (where, span);
          }
        else
          {
            QString best;
            double best_km = -1.;
            for (auto const& sp : psk_spots)
              {
                double dist = 0., bearing = 0.;
                if (!qIsNaN (my_lat_)) path_to (my_lat_, my_lon_, sp.lat, sp.lon, dist, bearing);
                if (dist * 6371. > best_km) {best_km = dist * 6371.; best = sp.call;}
              }
            double const shown = miles ? best_km * 0.621371 : best_km;
            QString count;
            if (psk_all_bands)
              {
                count = 1 == psk_spots.size () ? QObject::tr ("1 station") : QObject::tr ("%1 stations").arg (psk_spots.size ());
                count += 1 == bands.size () ? QObject::tr (" on 1 band") : QObject::tr (" on %1 bands").arg (bands.size ());
              }
            else
              {
                count = 1 == psk_spots.size () ? QObject::tr ("1 station on %1").arg (band_label)
                                               : QObject::tr ("%1 stations on %2").arg (psk_spots.size ()).arg (band_label);
              }
            lines << head + QObject::tr (" · %1 in %2").arg (count, span)
                  + QObject::tr (" · farthest %1, %L2 %3").arg (slashed (best)).arg (qRound (shown)).arg (miles ? "mi" : "km");
          }
      }
    if (show_worked_grids)
      {
        QString const where = grids_all_bands ? QObject::tr ("all bands") : band_label;
        lines << (1 == worked_mode ? QObject::tr ("Grids worked today on %1: %2").arg (where).arg (grids_shown ().size ())
                                   : QObject::tr ("Worked grids on %1: %2").arg (where).arg (grids_shown ().size ()));
      }
    if (lines.isEmpty ()) return;
    auto f = font ();
    f.setPointSizeF (qMax (6.5, f.pointSizeF () * 0.85));
    QFontMetricsF const fm {f};
    double w = 0.;
    for (auto const& l : lines) w = qMax (w, fm.horizontalAdvance (l));
    double const lh = fm.height () + 3;
    QRectF const box {12., 12., w + 24., lines.size () * lh + 12.};
    p.save ();
    p.setRenderHint (QPainter::Antialiasing);
    p.setPen (QPen {QColor {255, 255, 255, 40}, 1});
    p.setBrush (QColor {13, 17, 23, 190});
    p.drawRoundedRect (box, 10, 10);
    p.setFont (f);
    p.setPen (QColor {233, 236, 241});
    for (int i = 0; i < lines.size (); ++i)
      {
        p.drawText (QPointF {box.left () + 12., box.top () + 6. + i * lh + fm.ascent ()}, lines[i]);
      }
    p.restore ();
  }

  // city names, biggest first; a name is left out where it would run into
  // one already drawn, so zooming in shows more of them
  void draw_cities (QPainter& p)
  {
    auto f = font ();
    f.setPointSizeF (qMax (6.5, f.pointSizeF () * 0.82));
    QFontMetricsF const fm {f};
    QRectF const area = QRectF {rect ()}.adjusted (2, 2, -2, -2);
    QVector<QRectF> used;
    p.save ();
    p.setRenderHint (QPainter::Antialiasing);
    for (auto const& c : cities ())
      {
        if (used.size () >= 400) break;
        auto pt = to_point (c.lat, c.lon);
        if (Flat == projection)
          {
            // the flat map repeats east and west
            double const span = 360. * flat_scale ();
            while (pt.x () < area.left ()) pt.rx () += span;
            while (pt.x () > area.right ()) pt.rx () -= span;
          }
        else
          {
            if (!on_view (c.lat, c.lon)) continue;
          }
        if (!area.contains (pt)) continue;
        QRectF const box {pt.x () - 5, pt.y () - fm.ascent () / 2. - 3, fm.horizontalAdvance (c.label) + 15, fm.height () + 6};
        if (!area.contains (box)) continue;
        bool clash = false;
        for (auto const& u : used) if (u.intersects (box)) {clash = true; break;}
        if (clash) continue;
        used << box;
        p.setPen (QPen {QColor {0, 0, 0, 200}, 1.2});
        p.setBrush (c.capital ? QColor {"#FFD54A"} : QColor {"#FFFFFF"});
        if (c.capital) p.drawRect (QRectF {pt.x () - 2.6, pt.y () - 2.6, 5.2, 5.2});
        else p.drawEllipse (pt, 2.4, 2.4);
        outlined_text (p, pt + QPointF {6, fm.ascent () / 2. - 1}, c.label, f, QColor {"#FFFFFF"});
      }
    p.restore ();
  }

  void draw_beacons (QPainter& p)
  {
    auto f = font ();
    f.setPointSizeF (qMax (6., f.pointSizeF () * 0.85));
    f.setBold (true);
    for (auto const& b : beacons)
      {
        auto const pt = to_point (b.lat, b.lon);
        QPolygonF tri;
        tri << pt + QPointF {0, -7} << pt + QPointF {6, 4.5} << pt + QPointF {-6, 4.5};
        if (b.band >= 0)
          {
            p.setPen (QPen {QColor {0, 0, 0, 200}, 1});
            p.setBrush (beacon_color (b.band));
            p.drawPolygon (tri);
            outlined_text (p, pt + QPointF {8, 4}, slashed (b.call), f, beacon_color (b.band));
          }
        else
          {
            p.setPen (QPen {QColor {230, 230, 230, 200}, 1.4});
            p.setBrush (QColor {0, 0, 0, 90});
            p.drawPolygon (tri);
          }
      }
  }

  void draw_great_circle (QPainter& p, double lat1, double lon1, double lat2, double lon2)
  {
    double dist, bearing;
    path_to (lat1, lon1, lat2, lon2, dist, bearing);
    if (dist < 1e-6) return;
    QVector<QPointF> pts;
    for (int i = 0; i <= 120; ++i)
      {
        double lat, lon;
        destination (lat1, lon1, bearing, dist * i / 120., lat, lon);
        pts << to_point (lat, lon);
      }
    QPainterPath path;
    add_line (path, pts);
    p.setBrush (Qt::NoBrush);
    p.drawPath (path);
  }

  // text with a dark edge so it reads on any background
  static void outlined_text (QPainter& p, QPointF const& at, QString const& s, QFont const& f, QColor const& color)
  {
    QPainterPath text;
    text.addText (at, f, s);
    p.setPen (QPen {QColor {0, 0, 0, 200}, 3});
    p.setBrush (Qt::NoBrush);
    p.drawPath (text);
    p.setPen (Qt::NoPen);
    p.setBrush (color);
    p.drawPath (text);
  }

  // mask (white = land) to an image of one colour with the mask as alpha
  static QImage tinted (QImage const& mask, QColor const& color)
  {
    QImage out {mask.size (), QImage::Format_ARGB32_Premultiplied};
    out.fill (color);
    out.setAlphaChannel (mask);
    return out;
  }

  QImage land_;
  QImage countries_;
  QImage day_;
  QImage night_lights_;
  QImage day_small_;
  QImage night_small_;
  QImage terrain_;
  QImage base_;
  QImage composite_;
  QImage scaled_;
  QString scaled_key_;
  QImage projected_;
  QString projected_key_;
  QPointF projected_pan_;
  QVector<float> lut_lat_;
  QVector<float> lut_lon_;
  QString lut_key_;
  QImage overlay_;
  QColor sea_color_;
  QColor land_color_;
  QColor border_color_;
  QNetworkAccessManager * net_;
  QTimer * detail_timer_;
  Detail detail_;
  QImage detail_day_;
  QImage detail_night_;
  QVector<QPointer<QNetworkReply>> detail_replies_;
  QString az_lines_key_;
  QPainterPath az_lines_[3];
  double zoom_ {1.};
  double clon_ {0.};
  double clat_ {12.};
  QPointF az_pan_;
  bool pressed_ {false};
  bool dragging_ {false};
  QPoint press_pos_;
  double press_clon_ {0.};
  double press_clat_ {0.};
  QPointF press_pan_;
  QVector<QRectF> zoom_rects_;
  QImage night_;
  QVector<double> shade_;
  QImage aurora_;
  QImage cond_;
  QImage rain_;
  QImage prop_;
  QVector<QRectF> legend_rects_;
  QVector<QPair<QRectF, QPair<QString, QString>>> key_rects_;     // Heard me key: where, colour key, name
  QString grid_key_;
  QPainterPath grid_major_;
  QPainterPath grid_minor_;
  double sun_lat_ {0.};
  double sun_lon_ {0.};
  double my_lat_ {qQNaN ()};
  double my_lon_ {qQNaN ()};
  double dx_lat_ {qQNaN ()};
  double dx_lon_ {qQNaN ()};
  QString dx_call_;
};

//
// Solar X-rays over the last day from the GOES satellites, on a log
// scale with the flare classes marked
//
//
// Buttons in a row that carries on onto the next line when it runs out
// of room
//
class FlowLayout final
  : public QLayout
{
public:
  explicit FlowLayout (QWidget * parent, int spacing) : QLayout {parent}, spacing_ {spacing} {setContentsMargins (0, 0, 0, 0);}
  ~FlowLayout () {while (auto * item = takeAt (0)) delete item;}
  void addItem (QLayoutItem * item) override {items_ << item;}
  int count () const override {return items_.size ();}
  QLayoutItem * itemAt (int i) const override {return items_.value (i);}
  QLayoutItem * takeAt (int i) override {return i >= 0 && i < items_.size () ? items_.takeAt (i) : nullptr;}
  Qt::Orientations expandingDirections () const override {return {};}
  bool hasHeightForWidth () const override {return true;}
  int heightForWidth (int w) const override {return place (QRect {0, 0, w, 0}, false);}
  void setGeometry (QRect const& r) override {QLayout::setGeometry (r); place (r, true);}
  QSize sizeHint () const override {return minimumSize ();}
  QSize minimumSize () const override
  {
    QSize size;
    for (auto * item : items_) size = size.expandedTo (item->minimumSize ());
    return size;
  }
  // the last one sits at the far right of the last line
  void set_last_right (bool on) {last_right_ = on;}
  void insert_widget (int i, QWidget * w)
  {
    addChildWidget (w);
    items_.insert (qBound (0, i, int (items_.size ())), new QWidgetItem {w});
    invalidate ();
  }

private:
  int place (QRect const& r, bool apply) const
  {
    int x = r.x (), y = r.y (), line = 0;
    for (int i = 0; i < items_.size (); ++i)
      {
        auto * item = items_[i];
        if (item->isEmpty ()) continue;
        auto const hint = item->sizeHint ();
        if (x > r.x () && x + hint.width () > r.right () + 1)
          {
            x = r.x ();
            y += line + spacing_;
            line = 0;
          }
        int px = x;
        if (last_right_ && i == items_.size () - 1) px = qMax (x, r.right () + 1 - hint.width ());
        if (apply) item->setGeometry (QRect {QPoint {px, y}, hint});
        x += hint.width () + spacing_;
        line = qMax (line, hint.height ());
      }
    return y + line - r.y ();
  }
  QList<QLayoutItem *> items_;
  int spacing_;
  bool last_right_ {false};
};

//
// Ham radio headlines scrolling along under the map's buttons; click one
// to read it
//
class NewsTicker final
  : public QWidget
{
public:
  explicit NewsTicker (QWidget * parent)
    : QWidget {parent}
    , timer_ {new QTimer {this}}
  {
    setMouseTracking (true);
    setFixedHeight (QFontMetrics {font ()}.height () + 10);
    timer_->setInterval (30);
    QObject::connect (timer_, &QTimer::timeout, this, [this] {
        if (!hover_) offset_ += 1.;
        if (total_ > 0 && offset_ > total_) offset_ -= total_;
        update ();
      });
  }

  struct Item {QString source; QString title; QString link;};
  void set_items (QVector<Item> const& items) {items_ = items; update ();}
  void set_message (QString const& m) {message_ = m; update ();}

protected:
  void showEvent (QShowEvent *) override {timer_->start ();}
  void hideEvent (QHideEvent *) override {timer_->stop ();}
  void enterEvent (QEvent *) override {hover_ = true;}
  void leaveEvent (QEvent *) override {hover_ = false; setCursor (Qt::ArrowCursor);}

  void paintEvent (QPaintEvent *) override
  {
    QPainter p {this};
    p.setRenderHint (QPainter::Antialiasing);
    p.setPen (QPen {QColor {Win11Theme::token ("border")}, 1});
    p.setBrush (QColor {Win11Theme::token ("card")});
    p.drawRoundedRect (QRectF {rect ()}.adjusted (0.5, 0.5, -0.5, -0.5), 8, 8);
    auto bold = font ();
    bold.setBold (true);
    QFontMetricsF const fm {font ()}, bfm {bold};
    double const lead = bfm.horizontalAdvance (QObject::tr ("NEWS")) + 22;
    double const base = height () / 2. + fm.ascent () / 2. - 1;
    rects_.clear ();
    p.save ();
    p.setClipRect (QRectF {lead, 0, width () - lead - 4, double (height ())});
    if (items_.isEmpty ())
      {
        p.setPen (QColor {Win11Theme::token ("text_secondary")});
        p.drawText (QPointF {lead, base}, message_);
        total_ = 0;
      }
    else
      {
        // the headlines in a loop, as often as it takes to fill the width
        QColor const text {Win11Theme::token ("text")}, muted {Win11Theme::token ("text_secondary")};
        double total = 0;
        QVector<double> widths;
        for (auto const& it : items_)
          {
            double const w = bfm.horizontalAdvance (it.source) + 8 + fm.horizontalAdvance (it.title) + 40;
            widths << w;
            total += w;
          }
        total_ = total;
        double x = lead - offset_;
        while (x < width ())
          {
            for (int i = 0; i < items_.size () && x < width (); ++i)
              {
                if (x + widths[i] > lead)
                  {
                    p.setFont (bold);
                    p.setPen (QColor {Win11Theme::token ("accent")});
                    p.drawText (QPointF {x, base}, items_[i].source);
                    double const tx = x + bfm.horizontalAdvance (items_[i].source) + 8;
                    p.setFont (font ());
                    p.setPen (text);
                    p.drawText (QPointF {tx, base}, items_[i].title);
                    p.setPen (muted);
                    p.drawText (QPointF {tx + fm.horizontalAdvance (items_[i].title) + 15, base}, QString {QChar {0x25C6}});
                    rects_ << qMakePair (QRectF {x, 0, widths[i] - 20, double (height ())}, i);
                  }
                x += widths[i];
              }
          }
      }
    p.restore ();
    p.setFont (bold);
    p.setPen (QColor {Win11Theme::token ("title")});
    p.drawText (QPointF {10, base}, QObject::tr ("NEWS"));
  }

  void mouseMoveEvent (QMouseEvent * e) override
  {
    int const i = item_at (e->pos ());
    setCursor (i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (i >= 0) QToolTip::showText (e->globalPos (), QObject::tr ("%1: %2<br/><i>Click to read it</i>").arg (items_[i].source, items_[i].title.toHtmlEscaped ()), this);
  }

  void mouseReleaseEvent (QMouseEvent * e) override
  {
    int const i = item_at (e->pos ());
    if (i >= 0 && items_[i].link.startsWith ("http")) QDesktopServices::openUrl (QUrl {items_[i].link});
  }

private:
  int item_at (QPoint const& pos) const
  {
    for (auto const& r : rects_) if (r.first.contains (pos)) return r.second;
    return -1;
  }

  QTimer * timer_;
  QVector<Item> items_;
  QString message_;
  double offset_ {0.};
  double total_ {0.};
  bool hover_ {false};
  QVector<QPair<QRectF, int>> rects_;
};

class XRayGraph final
  : public QWidget
{
public:
  explicit XRayGraph (QWidget * parent)
    : QWidget {parent}
  {
    setMinimumSize (240, 110);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Expanding);
    setToolTip (QObject::tr ("<p>Solar X-rays from the GOES satellites over the last day (NOAA).</p>"
                             "<p><b>Red:</b> 0.1-0.8 nm, the one flare classes come from. <b>Blue:</b> 0.05-0.4 nm.</p>"
                             "<p>M and X class flares can black out HF on the daylight side of the Earth for minutes to hours.</p>"));
  }

  QVector<QPointF> long_band;          // x: seconds since 1970, y: W/m2
  QVector<QPointF> short_band;

protected:
  void paintEvent (QPaintEvent *) override
  {
    QPainter p {this};
    p.setRenderHint (QPainter::Antialiasing);
    QColor const text {Win11Theme::token ("text")};
    QColor const muted {Win11Theme::token ("text_secondary")};
    QColor const line {Win11Theme::token ("border")};
    auto small = font ();
    small.setPointSizeF (qMax (6.5, small.pointSizeF () * 0.85));
    QFontMetricsF fm {small};
    auto title = small;
    title.setCapitalization (QFont::AllUppercase);
    title.setWeight (QFont::DemiBold);
    title.setLetterSpacing (QFont::PercentageSpacing, 110);
    auto big = font ();
    big.setBold (true);
    big.setPointSizeF (big.pointSizeF () * 1.6);
    QFontMetricsF bfm {big};

    // title and the class now, large
    p.setFont (title);
    p.setPen (muted);
    p.drawText (QPointF {0, bfm.ascent () - 2}, QObject::tr ("X-ray flux · 24 h"));
    if (long_band.size ())
      {
        int level;
        auto const cls = xray_class (long_band.last ().y (), &level);
        p.setFont (big);
        p.setPen (level_color (level));
        p.drawText (QPointF {width () - bfm.boundingRect (cls).width () - 2, bfm.ascent ()}, cls);
      }

    QRectF const plot = QRectF (rect ()).adjusted (0, bfm.height () + 6, -(fm.boundingRect ("X").width () + 10), -(fm.height () + 2));
    if (plot.width () < 20 || plot.height () < 20) return;
    double const lo = -9., hi = -3.;
    auto y_of = [&] (double flux) {
      double const l = std::log10 (qMax (flux, 1e-12));
      return plot.bottom () - (qBound (lo, l, hi) - lo) / (hi - lo) * plot.height ();
    };
    double const now = QDateTime::currentMSecsSinceEpoch () / 1000.;
    double const start = now - 24 * 3600.;
    auto x_of = [&] (double t) {return plot.left () + (t - start) / (now - start) * plot.width ();};

    // flare classes: A from 1e-8, B, C, M and X from 1e-4; M and X tinted
    p.setFont (small);
    char const classes[] = "ABCMX";
    for (int i = 0; i < 5; ++i)
      {
        double const y1 = y_of (std::pow (10., -8 + i)), y2 = y_of (std::pow (10., -7 + i));
        if (i >= 3) p.fillRect (QRectF {plot.left (), y2, plot.width (), y1 - y2}, i == 3 ? QColor {255, 170, 0, 18} : QColor {255, 70, 70, 22});
        p.setPen (QPen {line, 1, Qt::DashLine});
        p.drawLine (QPointF {plot.left (), y1}, QPointF {plot.right (), y1});
        p.setPen (muted);
        p.drawText (QPointF {plot.right () + 6, (y1 + y2) / 2. + fm.ascent () / 2. - 1}, QString {QChar (classes[i])});
      }
    for (int h = 6; h < 24; h += 6)
      {
        double const x = x_of (now - h * 3600.);
        auto const label = QString {"-%1h"}.arg (h);
        p.drawText (QPointF {x - fm.boundingRect (label).width () / 2., plot.bottom () + fm.ascent () + 2}, label);
      }
    p.drawText (QPointF {plot.right () - fm.boundingRect (QObject::tr ("now")).width (), plot.bottom () + fm.ascent () + 2}, QObject::tr ("now"));

    auto path_of = [&] (QVector<QPointF> const& series, QPainterPath& line_path, QPainterPath& area) {
      bool started = false;
      double last_t = 0;
      QPointF first, last;
      for (auto const& pt : series)
        {
          if (pt.x () < start || pt.y () <= 0) continue;
          QPointF const at {x_of (pt.x ()), y_of (pt.y ())};
          if (!started || pt.x () - last_t > 900.) line_path.moveTo (at);
          else line_path.lineTo (at);
          if (!started) {first = at; area.moveTo (QPointF {at.x (), plot.bottom ()}); area.lineTo (at);}
          else area.lineTo (at);
          started = true;
          last_t = pt.x ();
          last = at;
        }
      if (started) {area.lineTo (QPointF {last.x (), plot.bottom ()}); area.closeSubpath ();}
    };
    p.save ();
    p.setClipRect (plot);
    QPainterPath short_line, short_area, long_line, long_area;
    path_of (short_band, short_line, short_area);
    path_of (long_band, long_line, long_area);
    QLinearGradient fill {plot.topLeft (), plot.bottomLeft ()};
    fill.setColorAt (0, QColor {255, 90, 90, 110});
    fill.setColorAt (1, QColor {255, 90, 90, 0});
    p.setPen (Qt::NoPen);
    p.setBrush (fill);
    p.drawPath (long_area);
    p.setBrush (Qt::NoBrush);
    p.setPen (QPen {QColor {79, 163, 255, 200}, 1.3});
    p.drawPath (short_line);
    p.setPen (QPen {QColor {"#FF5A5A"}, 1.8});
    p.drawPath (long_line);
    p.restore ();
  }
};

//
// The latest picture of the Sun from NASA's Solar Dynamics Observatory,
// cut round with a glow
//
class SunImage final
  : public QWidget
{
public:
  explicit SunImage (QWidget * parent)
    : QWidget {parent}
  {
    int const side = qMax (150, 8 * QFontMetrics {font ()}.height ());
    setMinimumSize (side, side);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Expanding);
  }
  QSize sizeHint () const override {return minimumSize ();}

  void set_image (QImage const& img) {image_ = img; scaled_ = QImage {}; update ();}
  void set_message (QString const& m) {message_ = m; update ();}
  void set_caption (QString const& c) {caption_ = c; update ();}
  void set_corners (QStringList const& c) {if (c != corners_) {corners_ = c; update ();}}
  // a widget kept just to the right of the picture, off the Sun
  void set_side_widget (QWidget * w)
  {
    side_ = w;
    w->setParent (this);
    setMinimumWidth (minimumHeight () + w->sizeHint ().width () + 6);
    place_side ();
  }


protected:
  void paintEvent (QPaintEvent *) override
  {
    QPainter p {this};
    p.setRenderHint (QPainter::Antialiasing);
    p.setRenderHint (QPainter::SmoothPixmapTransform);
    QRectF const r = picture_rect ();
    if (image_.isNull ())
      {
        p.setPen (QColor {Win11Theme::token ("text_secondary")});
        p.drawText (rect (), Qt::AlignCenter | Qt::TextWordWrap, message_);
        return;
      }
    // NASA's whole picture, square and untrimmed, so flares and
    // prominences show all round the edge of the Sun
    if (scaled_.size () != r.size ().toSize ())
      {
        scaled_ = image_.scaled (r.size ().toSize (), Qt::KeepAspectRatio, Qt::SmoothTransformation);
      }
    QPainterPath frame;
    frame.addRoundedRect (r, 6, 6);
    p.setClipPath (frame);
    p.fillRect (r, Qt::black);
    p.drawImage (r.topLeft (), scaled_);
    // where the Sun is from you, in the corners like HamClock
    auto f = font ();
    f.setBold (true);
    f.setPointSizeF (qMax (7., f.pointSizeF () * 0.95));
    p.setFont (f);
    QFontMetricsF const fm {f};
    auto corner = [&] (QString const& text, Qt::Alignment align) {
      if (text.isEmpty ()) return;
      QRectF const area = r.adjusted (6, 4, -6, -4);
      QRectF box {0, 0, fm.horizontalAdvance (text) + 8, fm.height () + 2};
      box.moveLeft (align & Qt::AlignLeft ? area.left () : area.right () - box.width ());
      box.moveTop (align & Qt::AlignTop ? area.top () : area.bottom () - box.height ());
      p.setPen (Qt::NoPen);
      p.setBrush (QColor {0, 0, 0, 120});
      p.drawRoundedRect (box, 4, 4);
      p.setPen (QColor {255, 170, 60});
      p.drawText (box, Qt::AlignCenter, text);
    };
    corner (corners_.value (0), Qt::AlignTop | Qt::AlignLeft);
    corner (corners_.value (1), Qt::AlignTop | Qt::AlignRight);
    corner (corners_.value (2), Qt::AlignBottom | Qt::AlignLeft);
    if (caption_.size ())
      {
        // a movie frame's time, bottom middle
        QRectF box {0, 0, fm.horizontalAdvance (caption_) + 8, fm.height () + 2};
        box.moveCenter ({r.center ().x (), r.bottom () - 4 - box.height () / 2});
        if (corners_.value (2).size ()) box.moveTop (box.top () - box.height () - 2);
        p.setPen (Qt::NoPen);
        p.setBrush (QColor {0, 0, 0, 150});
        p.drawRoundedRect (box, 4, 4);
        p.setPen (QColor {Win11Theme::token ("text")});
        p.drawText (box, Qt::AlignCenter, caption_);
      }
    p.setClipping (false);
  }

private:
  // the square picture and its side widget, centred together
  QRectF picture_rect () const
  {
    int const gap = side_ ? side_->sizeHint ().width () + 6 : 0;
    int const s = qMax (10, qMin (width () - gap, height ()) - 2);
    return {(width () - s - gap) / 2., (height () - s) / 2., double (s), double (s)};
  }
  void place_side ()
  {
    if (!side_) return;
    side_->resize (side_->sizeHint ());
    auto const r = picture_rect ();
    side_->move (qRound (r.right ()) + 6, qRound (r.top ()));
  }
  void resizeEvent (QResizeEvent *) override {place_side ();}
  bool event (QEvent * e) override
  {
    // the menu's text changed size
    if (QEvent::LayoutRequest == e->type ()) {place_side (); update ();}
    return QWidget::event (e);
  }
  QWidget * side_ {nullptr};
  QImage image_;
  QImage scaled_;
  QStringList corners_;

  QString message_;
  QString caption_;
};

//
// The gray line tool: over the next day, when you and the DX station are
// in twilight, and when you both are at once
//
class GrayLineChart final
  : public QWidget
{
public:
  explicit GrayLineChart (QWidget * parent)
    : QWidget {parent}
  {
    setMinimumSize (520, 150);
  }

  struct Place {QString name; QString grid; double lat {qQNaN ()}; double lon {qQNaN ()};};
  Place de, dx;
  QDateTime start;

  // 0 night, 1 twilight, 2 day, every 5 minutes for a day
  static QVector<int> states (Place const& pl, QDateTime const& from)
  {
    QVector<int> out;
    for (int i = 0; i < 288; ++i)
      {
        if (qIsNaN (pl.lat)) {out << -1; continue;}
        double const e = sun_elevation (from.addSecs (i * 300), pl.lat, pl.lon);
        out << (e < -6. ? 0 : e <= 3. ? 1 : 2);
      }
    return out;
  }

  // the times both are in twilight
  QVector<QPair<QDateTime, QDateTime>> both () const
  {
    QVector<QPair<QDateTime, QDateTime>> out;
    auto const a = states (de, start), b = states (dx, start);
    int run = -1;
    for (int i = 0; i <= 288; ++i)
      {
        bool const on = i < 288 && 1 == a[i] && 1 == b[i];
        if (on && run < 0) run = i;
        if (!on && run >= 0)
          {
            out << qMakePair (start.addSecs (run * 300), start.addSecs (i * 300));
            run = -1;
          }
      }
    return out;
  }

protected:
  void paintEvent (QPaintEvent *) override
  {
    QPainter p {this};
    p.setRenderHint (QPainter::Antialiasing);
    QFontMetricsF const fm {font ()};
    double const label_w = 110, left = label_w, right = width () - 6.;
    double const bar_h = 22;
    QColor const day {242, 193, 78, 90}, dusk {"#FF9F43"}, night {"#16263D"}, both_c {Win11Theme::token ("accent")};
    QColor const text {Win11Theme::token ("text")}, muted {Win11Theme::token ("text_secondary")};
    auto draw_row = [&] (double y, Place const& pl) {
      auto bold = font ();
      bold.setBold (true);
      p.setFont (bold);
      p.setPen (text);
      p.drawText (QPointF {0, y + fm.ascent () - 2}, slashed (pl.name.isEmpty () ? pl.grid : pl.name));
      p.setFont (font ());
      p.setPen (muted);
      p.drawText (QPointF {0, y + bar_h + 2}, pl.grid);
      QRectF const bar {left, y, right - left, bar_h};
      QPainterPath clip;
      clip.addRoundedRect (bar, 5, 5);
      p.save ();
      p.setClipPath (clip);
      p.fillRect (bar, QColor {12, 19, 14});
      p.setRenderHint (QPainter::Antialiasing, false);
      auto const st = states (pl, start);
      double const step = bar.width () / 288.;
      for (int i = 0; i < 288; ++i)
        {
          if (st[i] < 0) continue;
          p.fillRect (QRectF {left + i * step, y, step + 0.6, bar_h}, 0 == st[i] ? night : 1 == st[i] ? dusk : day);
        }
      p.restore ();
    };
    draw_row (4, de);
    draw_row (4 + bar_h + 20, dx);
    double const y3 = 4 + 2 * (bar_h + 20);
    p.setPen (muted);
    p.drawText (QPointF {0, y3 + fm.ascent ()}, QObject::tr ("Both"));
    double const step = (right - left) / 288.;
    p.setPen (Qt::NoPen);
    p.setBrush (both_c);
    for (auto const& r : both ())
      {
        double const x0 = left + start.secsTo (r.first) / 300. * step, x1 = left + start.secsTo (r.second) / 300. * step;
        p.drawRoundedRect (QRectF {x0, y3, qMax (4., x1 - x0), bar_h - 4}, 4, 4);
      }
    // hours along the bottom
    double const y4 = y3 + bar_h + 4;
    p.setPen (muted);
    for (int h = 0; h <= 24; h += 6)
      {
        double const x = left + h * 12 * step;
        auto const t = start.addSecs (h * 3600).toString ("hh:mm");
        auto const label = 0 == h ? QObject::tr ("now %1").arg (t) : 24 == h ? t + " UTC" : t;
        double const w = fm.horizontalAdvance (label);
        double const lx = 0 == h ? x : 24 == h ? x - w : x - w / 2;
        p.drawText (QPointF {lx, y4 + fm.ascent ()}, label);
      }
  }
};

//
// Your call sign, large, in a colour or a two-colour blend; double-click
// to choose
//
class CallSign final
  : public QWidget
{
public:
  explicit CallSign (QWidget * parent)
    : QWidget {parent}
  {
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip (QObject::tr ("Double-click to change the colour of your call sign"));
    setCursor (Qt::PointingHandCursor);
  }

  QString text;
  QColor first;
  QColor second;
  bool gradient {true};
  std::function<void ()> on_double_click;

  QFont call_font () const
  {
    auto f = QApplication::font ();
    f.setPointSizeF ((f.pointSizeF () > 0 ? f.pointSizeF () : 9.) * 2.8);
    f.setWeight (QFont::Black);
    f.setLetterSpacing (QFont::PercentageSpacing, 103);
    return f;
  }
  QSize sizeHint () const override
  {
    QFontMetricsF fm {call_font ()};
    return {static_cast<int> (fm.boundingRect (text).width () + 8), static_cast<int> (fm.height () + 4)};
  }
  QSize minimumSizeHint () const override {return {40, sizeHint ().height ()};}

protected:
  void paintEvent (QPaintEvent *) override
  {
    QPainter p {this};
    p.setRenderHint (QPainter::Antialiasing);
    auto const f = call_font ();
    QFontMetricsF fm {f};
    double const w = fm.boundingRect (text).width ();
    double const x = 0;
    QPainterPath path;
    path.addText (QPointF {x, (height () + fm.ascent () - fm.descent ()) / 2.}, f, text);
    if (gradient && second.isValid ())
      {
        QLinearGradient g {QPointF {x, 0}, QPointF {x + w, 0}};
        g.setColorAt (0, first);
        g.setColorAt (1, second);
        p.fillPath (path, g);
      }
    else
      {
        p.fillPath (path, first);
      }
  }

  void mouseDoubleClickEvent (QMouseEvent *) override
  {
    if (on_double_click) on_double_click ();
  }
};

//
// A picture beside your call: the KF0KYL Digi logo unless you pick your
// own. It takes the room the box has and hides when there isn't enough.
//
class StationLogo final
  : public QWidget
{
public:
  explicit StationLogo (QWidget * parent)
    : QWidget {parent}
  {
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Expanding);
    setToolTip (QObject::tr ("Double-click to choose the picture beside your call"));
  }
  QSize sizeHint () const override {return {220, 130};}
  QSize minimumSizeHint () const override {return {0, 0};}

  void set_image (QImage const& img) {image_ = img; scaled_ = QImage {}; update ();}
  std::function<void ()> on_double_click;

protected:
  void paintEvent (QPaintEvent *) override
  {
    if (image_.isNull () || width () < 90 || height () < 50) return;
    QPainter p {this};
    QRect const area = rect ().adjusted (4, 2, -4, -2);
    QSize const fit = image_.size ().scaled (area.size (), Qt::KeepAspectRatio);
    if (scaled_.size () != fit)
      {
        scaled_ = image_.scaled (fit * devicePixelRatioF (), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled_.setDevicePixelRatio (devicePixelRatioF ());
      }
    p.drawImage (QPoint {area.center ().x () - fit.width () / 2, area.center ().y () - fit.height () / 2}, scaled_);
  }

  void mouseDoubleClickEvent (QMouseEvent *) override
  {
    if (on_double_click) on_double_click ();
  }

private:
  QImage image_;
  QImage scaled_;
};

//
// VOACAP between you and the DX station: how likely each band is to
// work through the day, hour by hour (UTC)
//
class VoacapGrid final
  : public QWidget
{
public:
  static int const bands = 9;
  explicit VoacapGrid (QWidget * parent)
    : QWidget {parent}
  {
    setMinimumHeight (150);
    setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking (true);
  }

  static char const * band_name (int b)
  {
    static char const * const names[bands] = {"80", "60", "40", "30", "20", "17", "15", "12", "10"};
    return names[b];
  }

  // the band shown on the map (-1: none), and a click on a band's row
  int selected {-1};
  std::function<void (int)> on_band;

  // reliability 0..1 and signal to noise, [band][hour UTC]; empty when unknown
  QVector<QVector<float>> rel;
  QVector<QVector<float>> snr;

  static QColor colour (double v) {return VoacapGrid_colour (v);}

protected:
  QRectF cells () const
  {
    QFontMetricsF fm {font ()};
    double const left = fm.boundingRect ("80").width () + 8;
    return QRectF {left, 0, width () - left - 1, height () - fm.height () - 4};
  }

  void paintEvent (QPaintEvent *) override
  {
    QPainter p {this};
    p.setRenderHint (QPainter::Antialiasing);
    QColor const muted {Win11Theme::token ("text_secondary")};
    auto small = font ();
    small.setPointSizeF (qMax (6.5, small.pointSizeF () * 0.85));
    p.setFont (small);
    QFontMetricsF fm {small};
    auto const area = cells ();
    double const cw = area.width () / 24., ch = area.height () / bands;
    int const now = QDateTime::currentDateTimeUtc ().time ().hour ();
    for (int b = 0; b < bands; ++b)
      {
        double const y = area.top () + b * ch;      // 80 m at the top
        if (b == selected)
          {
            // the band on the map: its label in the accent colour, the row
            // outlined (below) so its squares keep their real colours
            auto bold = small;
            bold.setBold (true);
            p.setFont (bold);
            p.setPen (QColor {Win11Theme::token ("accent")});
          }
        else
          {
            p.setFont (small);
            p.setPen (muted);
          }
        p.drawText (QRectF {0, y, area.left () - 6, ch}, Qt::AlignRight | Qt::AlignVCenter, band_name (b));
        p.setFont (small);
        for (int h = 0; h < 24; ++h)
          {
            QRectF const cell {area.left () + h * cw + 0.75, y + 0.75, cw - 1.5, ch - 1.5};
            QColor c;
            if (b < rel.size () && h < rel[b].size ())
              {
                double const v = rel[b][h];
                c = colour (v);
                c.setAlphaF (0.25 + 0.75 * v);
              }
            else
              {
                c = QColor {Win11Theme::token ("ctrl")};
              }
            p.setPen (Qt::NoPen);
            p.setBrush (c);
            p.drawRoundedRect (cell, 2, 2);
          }
      }
    if (selected >= 0 && selected < bands)
      {
        p.setPen (QPen {QColor {Win11Theme::token ("accent")}, 2.});
        p.setBrush (Qt::NoBrush);
        p.drawRoundedRect (QRectF {area.left () - 1, area.top () + selected * ch - 1, area.width () + 2, ch + 2}, 4, 4);
      }
    // the hour now
    p.setPen (QPen {QColor {Win11Theme::token ("text")}, 1.5});
    p.setBrush (Qt::NoBrush);
    p.drawRoundedRect (QRectF {area.left () + now * cw + 0.5, area.top () + 0.5, cw - 1, area.height () - 1}, 3, 3);
    p.setPen (muted);
    for (int h = 0; h < 24; h += 6)
      {
        p.drawText (QPointF {area.left () + h * cw + 1, area.bottom () + fm.ascent () + 3},
                    18 == h ? QObject::tr ("18 UTC") : QString::number (h));
      }
  }

  int band_at (QPoint const& pos) const
  {
    auto const area = cells ();
    if (pos.y () < area.top () || pos.y () >= area.bottom ()) return -1;
    return qBound (0, static_cast<int> ((pos.y () - area.top ()) / (area.height () / bands)), bands - 1);
  }

  void mousePressEvent (QMouseEvent * e) override
  {
    int const b = band_at (e->pos ());
    if (b >= 0 && on_band) on_band (b);
  }

  void mouseMoveEvent (QMouseEvent * e) override
  {
    setCursor (band_at (e->pos ()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    auto const area = cells ();
    if (!area.contains (e->pos ()) || rel.isEmpty ())
      {
        QToolTip::hideText ();
        return;
      }
    int const h = qBound (0, static_cast<int> ((e->pos ().x () - area.left ()) / (area.width () / 24.)), 23);
    int const b = qBound (0, static_cast<int> ((e->pos ().y () - area.top ()) / (area.height () / bands)), bands - 1);
    if (b >= rel.size () || h >= rel[b].size ()) return;
    auto text = QObject::tr ("<b>%1 m at %2:00 UTC</b><br/>Reliability %3%")
      .arg (band_name (b)).arg (h, 2, 10, QChar {'0'}).arg (qRound (rel[b][h] * 100));
    if (b < snr.size () && h < snr[b].size ()) text += QObject::tr ("<br/>Signal to noise %1 dB in 1 Hz").arg (qRound (snr[b][h]));
    text += "<br/>" + (b == selected ? QObject::tr ("Click to take this band off the map") : QObject::tr ("Click to show this band on the map"));
    QToolTip::showText (e->globalPos (), text, this);
  }
};

ConditionsWindow::ConditionsWindow (QSettings * settings, QWidget * parent)
  : QWidget {parent, Qt::Window}
  , settings_ {settings}
  , network_ {new QNetworkAccessManager {this}}
  , refresh_timer_ {new QTimer {this}}
  , clock_timer_ {new QTimer {this}}
  , sfi_ {qQNaN ()}
  , ssn_ {qQNaN ()}
  , kp_ {qQNaN ()}
  , a_index_ {qQNaN ()}
  , wind_ {qQNaN ()}
  , bz_ {qQNaN ()}
  , bt_ {qQNaN ()}
  , xray_ {qQNaN ()}
{
  setObjectName ("ConditionsWindow");
  setWindowTitle (tr ("Conditions - %1").arg (QApplication::applicationName ()));
  sun_timer_ = new QTimer {this};
  sun_timer_->setInterval (15 * 60 * 1000);       // SDO puts up new pictures every 15 minutes or so
  connect (sun_timer_, &QTimer::timeout, this, &ConditionsWindow::fetch_sun);
  build_ui ();

  refresh_timer_->setInterval (15 * 60 * 1000);   // NOAA updates every few minutes; no need to ask more
  connect (refresh_timer_, &QTimer::timeout, this, &ConditionsWindow::refresh);
  clock_timer_->setInterval (1000);
  connect (clock_timer_, &QTimer::timeout, this, &ConditionsWindow::tick);
  overlay_timer_ = new QTimer {this};
  overlay_timer_->setInterval (30 * 60 * 1000);    // aurora and rain: every 30 minutes
  connect (overlay_timer_, &QTimer::timeout, this, &ConditionsWindow::refresh_overlays);

  settings_->beginGroup ("Conditions");
  restoreGeometry (settings_->value ("geometry").toByteArray ());
  settings_->endGroup ();
}

ConditionsWindow::~ConditionsWindow ()
{
  // shut the PSK Reporter live feed down quietly before the map and the
  // rest of this window are taken apart: its "disconnected" news would
  // otherwise try to redraw a map that's already gone (crash on close)
  if (psk_live_)
    {
      psk_live_->disconnect (this);
      delete psk_live_;
      psk_live_ = nullptr;
    }
}

QFrame * ConditionsWindow::make_box (QString const& key, QString const& title, QVBoxLayout *& body)
{
  auto * frame = new QFrame {this};
  frame->setObjectName ("conditionsBox");
  body = new QVBoxLayout {frame};
  body->setContentsMargins (14, 11, 14, 11);
  body->setSpacing (4);
  if (title.size ())
    {
      body->addWidget (box_title (title, frame));
    }
  boxes_[key] = frame;
  return frame;
}

QLabel * ConditionsWindow::box_title (QString const& title, QWidget * parent)
{
  // small grey capitals, like the mockup
  auto * label = new QLabel {title, parent};
  label->setObjectName ("boxTitle");
  auto f = label->font ();
  f.setCapitalization (QFont::AllUppercase);
  f.setLetterSpacing (QFont::PercentageSpacing, 112);
  label->setFont (f);
  return label;
}

QAbstractButton * ConditionsWindow::make_chip (QString const& text, QString const& tip, QWidget * parent)
{
  auto * chip = new QToolButton {parent};
  chip->setObjectName ("chip");
  chip->setText (text);
  chip->setToolTip (tip);
  chip->setCheckable (true);
  chip->setCursor (Qt::PointingHandCursor);
  return chip;
}

QWidget * ConditionsWindow::make_segments (QStringList const& labels, QStringList const& tips, QButtonGroup *& group, QWidget * parent)
{
  // a row of buttons where one is picked, drawn as one rounded control
  auto * frame = new QFrame {parent};
  frame->setObjectName ("seg");
  auto * h = new QHBoxLayout {frame};
  h->setContentsMargins (2, 2, 2, 2);
  h->setSpacing (0);
  group = new QButtonGroup {frame};
  group->setExclusive (true);
  for (int i = 0; i < labels.size (); ++i)
    {
      auto * b = new QPushButton {frame};
      b->setObjectName ("segButton");
      b->setFocusPolicy (Qt::NoFocus);
      b->setText (labels[i]);
      if (i < tips.size ()) b->setToolTip (tips[i]);
      b->setCheckable (true);
      b->setCursor (Qt::PointingHandCursor);
      // room for the bold text of the picked one, so nothing gets cut short
      auto bold = QApplication::font ();
      bold.setBold (true);
      b->setMinimumWidth (QFontMetrics {bold}.boundingRect (labels[i]).width () + 28);
      b->setSizePolicy (QSizePolicy::Fixed, QSizePolicy::Fixed);
      group->addButton (b, i);
      h->addWidget (b);
    }
  return frame;
}

void ConditionsWindow::show_call_colors ()
{
  // a little pop-up under the call sign with colours to pick from
  auto * popup = new QFrame {this, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint};
  popup->setAttribute (Qt::WA_DeleteOnClose);
  popup->setAttribute (Qt::WA_TranslucentBackground);
  popup->setObjectName ("callPopupOuter");
  auto * outer = new QVBoxLayout {popup};
  outer->setContentsMargins (0, 0, 0, 0);
  auto * card = new QFrame {popup};
  card->setObjectName ("callPopup");
  outer->addWidget (card);
  auto * v = new QVBoxLayout {card};
  v->setContentsMargins (16, 14, 16, 14);
  v->setSpacing (10);
  v->addWidget (box_title (tr ("Call sign color"), card));

  struct Swatch {QColor a; QColor b;};
  QVector<Swatch> const swatches {
    {QColor {Win11Theme::token ("accent")}, QColor {Win11Theme::token ("accent2")}},
    {QColor {"#FF5F6D"}, QColor {"#FFC371"}},
    {QColor {"#11998E"}, QColor {"#38EF7D"}},
    {QColor {"#F953C6"}, QColor {"#B91D73"}},
    {QColor {"#4CC2FF"}, {}}, {QColor {"#3FB950"}, {}}, {QColor {"#FFD54A"}, {}}, {QColor {"#FF4D4D"}, {}},
    {QColor {"#FF8A3C"}, {}}, {QColor {"#C084FC"}, {}}, {QColor {"#F472B6"}, {}}, {QColor {"#E9ECF1"}, {}},
  };
  auto * grid = new QGridLayout;
  grid->setSpacing (8);
  int n = 0;
  for (auto const& sw : swatches)
    {
      auto * b = new QToolButton {card};
      b->setObjectName ("swatch");
      b->setFixedSize (30, 26);
      b->setCursor (Qt::PointingHandCursor);
      b->setStyleSheet (sw.b.isValid ()
                        ? QString {"QToolButton {background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %1, stop:1 %2);}"}.arg (sw.a.name ()).arg (sw.b.name ())
                        : QString {"QToolButton {background: %1;}"}.arg (sw.a.name ()));
      connect (b, &QToolButton::clicked, this, [this, sw] {
          call_first_ = sw.a;
          if (sw.b.isValid ()) {call_second_ = sw.b; call_gradient_ = true;}
          else call_gradient_ = false;
          call_auto_ = false;
          save_call_colors ();
        });
      grid->addWidget (b, n / 8, n % 8);
      ++n;
    }
  v->addLayout (grid);

  auto * row = new QHBoxLayout;
  row->setSpacing (8);
  QButtonGroup * kind;
  row->addWidget (make_segments ({tr ("Gradient"), tr ("Solid")}, {tr ("Blend two colors"), tr ("One color")}, kind, card));
  kind->button (call_gradient_ ? 0 : 1)->setChecked (true);
  connect (kind, static_cast<void (QButtonGroup::*) (QAbstractButton *)> (&QButtonGroup::buttonClicked), this, [this, kind] (QAbstractButton * b) {
      int const id = kind->id (b);
      call_gradient_ = 0 == id;
      call_auto_ = false;
      save_call_colors ();
    });
  auto * custom = new QToolButton {card};
  custom->setObjectName ("chip");
  custom->setText (tr ("Custom..."));
  connect (custom, &QToolButton::clicked, this, [this, popup] {
      popup->hide ();
      auto const c = QColorDialog::getColor (call_first_, this, tr ("Call sign color"));
      if (c.isValid ())
        {
          if (call_gradient_)
            {
              auto const c2 = QColorDialog::getColor (call_second_.isValid () ? call_second_ : c, this, tr ("Second color of the blend"));
              if (c2.isValid ()) call_second_ = c2;
            }
          call_first_ = c;
          call_auto_ = false;
          save_call_colors ();
        }
      popup->close ();
    });
  row->addWidget (custom);
  row->addStretch ();
  v->addLayout (row);

  auto * bottom = new QHBoxLayout;
  auto * preview = new CallSign {card};
  preview->text = call_label_->text;
  preview->setAttribute (Qt::WA_TransparentForMouseEvents);
  preview->setObjectName ("plain");
  bottom->addWidget (preview, 1);
  auto * reset = new QToolButton {card};
  reset->setObjectName ("chip");
  reset->setText (tr ("Reset"));
  reset->setToolTip (tr ("Back to the color scheme's own colors"));
  connect (reset, &QToolButton::clicked, this, [this] {
      call_auto_ = true;
      call_gradient_ = true;
      save_call_colors ();
    });
  auto * done = new QToolButton {card};
  done->setObjectName ("doneButton");
  done->setText (tr ("Done"));
  connect (done, &QToolButton::clicked, popup, &QWidget::close);
  bottom->addWidget (reset);
  bottom->addWidget (done);
  v->addLayout (bottom);

  // keep the preview in step
  auto sync = [this, preview] {
    preview->first = call_label_->first;
    preview->second = call_label_->second;
    preview->gradient = call_label_->gradient;
    preview->update ();
  };
  sync ();
  connect (this, &ConditionsWindow::call_colors_changed, preview, sync);

  popup->setStyleSheet (styleSheet ());
  popup->adjustSize ();
  popup->move (call_label_->mapToGlobal (QPoint {0, call_label_->height () + 4}));
  popup->show ();
}

void ConditionsWindow::apply_station_picture ()
{
  // "" is the KF0KYL Digi logo, "none" is nothing, anything else a file
  QImage img;
  if ("none" != station_picture_)
    {
      if (station_picture_.size ()) img.load (station_picture_);
      if (img.isNull ()) img.load (":/station_logo.png");
    }
  station_logo_->set_image (img);
}

void ConditionsWindow::show_station_pictures ()
{
  auto * popup = new QFrame {this, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint};
  popup->setAttribute (Qt::WA_DeleteOnClose);
  popup->setAttribute (Qt::WA_TranslucentBackground);
  popup->setObjectName ("callPopupOuter");
  auto * outer = new QVBoxLayout {popup};
  outer->setContentsMargins (0, 0, 0, 0);
  auto * card = new QFrame {popup};
  card->setObjectName ("callPopup");
  outer->addWidget (card);
  auto * v = new QVBoxLayout {card};
  v->setContentsMargins (16, 14, 16, 14);
  v->setSpacing (10);
  v->addWidget (box_title (tr ("Station picture"), card));
  auto * tiles = new QHBoxLayout;
  tiles->setSpacing (10);
  auto tile = [card] (QString const& text, QIcon const& icon, bool on) {
    auto * b = new QToolButton {card};
    b->setObjectName ("pictureTile");
    b->setCheckable (true);
    b->setChecked (on);
    b->setText (text);
    b->setIcon (icon);
    b->setIconSize ({84, 56});
    b->setToolButtonStyle (Qt::ToolButtonTextUnderIcon);
    b->setFixedSize (108, 92);
    b->setCursor (Qt::PointingHandCursor);
    return b;
  };
  bool const own = station_picture_.size () && "none" != station_picture_;
  QPixmap own_pm;
  if (own) own_pm.load (station_picture_);
  auto * logo = tile (tr ("KF0KYL Digi"), QIcon {QPixmap {":/station_logo.png"}}, station_picture_.isEmpty ());
  auto * yours = tile (own ? tr ("Yours") : tr ("Your own..."), own_pm.isNull () ? QIcon {} : QIcon {own_pm}, own);
  auto * none = tile (tr ("None"), QIcon {}, "none" == station_picture_);
  if (own_pm.isNull ()) yours->setText ("+\n" + tr ("Your own..."));
  for (auto * b : {logo, yours, none}) tiles->addWidget (b);
  v->addLayout (tiles);
  auto * note = new QLabel {tr ("Your own: a PNG or JPG (a club logo, a QSL design, a photo). It's shown beside your call and sized to fit the box."), card};
  note->setObjectName ("muted");
  note->setWordWrap (true);
  note->setMaximumWidth (350);
  v->addWidget (note);
  auto choose = [this, logo, yours, none] (QString const& picture) {
    station_picture_ = picture;
    settings_->beginGroup ("Conditions");
    settings_->setValue ("StationPicture", station_picture_);
    settings_->endGroup ();
    apply_station_picture ();
    logo->setChecked (picture.isEmpty ());
    none->setChecked ("none" == picture);
    yours->setChecked (picture.size () && "none" != picture);
  };
  connect (logo, &QToolButton::clicked, this, [choose] {choose ({});});
  connect (none, &QToolButton::clicked, this, [choose] {choose ("none");});
  connect (yours, &QToolButton::clicked, this, [this, popup, choose, yours] {
      popup->hide ();
      auto const file = QFileDialog::getOpenFileName (this, tr ("Picture beside your call"), {},
                                                       tr ("Pictures (*.png *.jpg *.jpeg *.bmp *.gif)"));
      if (file.size () && !QImage {file}.isNull ())
        {
          choose (file);
          yours->setIcon (QIcon {QPixmap {file}});
          yours->setText (tr ("Yours"));
        }
      else
        {
          choose (station_picture_);         // put the buttons back as they were
        }
      popup->close ();
    });
  auto * bottom = new QHBoxLayout;
  bottom->addStretch ();
  auto * reset = new QToolButton {card};
  reset->setObjectName ("chip");
  reset->setText (tr ("Reset"));
  reset->setToolTip (tr ("Back to the KF0KYL Digi logo"));
  connect (reset, &QToolButton::clicked, this, [choose] {choose ({});});
  auto * done = new QToolButton {card};
  done->setObjectName ("doneButton");
  done->setText (tr ("Done"));
  connect (done, &QToolButton::clicked, popup, &QWidget::close);
  bottom->addWidget (reset);
  bottom->addWidget (done);
  v->addLayout (bottom);
  popup->setStyleSheet (styleSheet ());
  popup->adjustSize ();
  auto pos = station_logo_->mapToGlobal (QPoint {0, station_logo_->height () / 2});
  popup->move (pos);
  popup->show ();
}

void ConditionsWindow::save_call_colors ()
{
  settings_->beginGroup ("Conditions");
  settings_->setValue ("CallColorAuto", call_auto_);
  settings_->setValue ("CallColor1", call_first_.name ());
  settings_->setValue ("CallColor2", call_second_.name ());
  settings_->setValue ("CallGradient", call_gradient_);
  settings_->endGroup ();
  apply_call_colors ();
}

void ConditionsWindow::apply_call_colors ()
{
  if (call_auto_)
    {
      call_label_->first = QColor {Win11Theme::token ("accent")};
      call_label_->second = QColor {Win11Theme::token ("accent2")};
      call_label_->gradient = call_gradient_;
    }
  else
    {
      call_label_->first = call_first_;
      call_label_->second = call_second_;
      call_label_->gradient = call_gradient_;
    }
  call_label_->update ();
  Q_EMIT call_colors_changed ();
}

void ConditionsWindow::add_box_action (QString const& key, QString const& text)
{
  auto * action = boxes_menu_->addAction (text);
  action->setCheckable (true);
  settings_->beginGroup ("Conditions");
  action->setChecked (settings_->value ("Box_" + key, true).toBool ());
  settings_->endGroup ();
  box_actions_[key] = action;
  boxes_[key]->setVisible (action->isChecked ());
  connect (action, &QAction::toggled, this, [this, key] (bool on) {
      boxes_[key]->setVisible (on);
      settings_->beginGroup ("Conditions");
      settings_->setValue ("Box_" + key, on);
      settings_->endGroup ();
      update_containers ();
      if (on && "sun" == key && isVisible ()) fetch_sun ();
    });
}

bool ConditionsWindow::box_on (QString const& key) const
{
  auto * action = box_actions_.value (key);
  return action && action->isChecked ();
}

void ConditionsWindow::update_containers ()
{
  // hide a row or column when every box in it is switched off
  top_row_->setVisible (box_on ("clock") || box_on ("xray") || box_on ("sun") || box_on ("beacons"));
  left_scroll_->setVisible (box_on ("dx") || box_on ("voacap"));
  // the boxes left in the column share its height: VOACAP's chart grows
  // when it's shown, otherwise the boxes keep their size at the top
  if (side_)
    {
      // the DX box keeps a set height (see fit_dx_box); VOACAP takes the rest
      bool const grow = box_on ("voacap");
      fit_dx_box ();
      auto * dx = boxes_["dx"];
      side_->setStretchFactor (dx, 0);
      side_->setStretchFactor (boxes_["voacap"], grow ? 1 : 0);
      side_->setStretchFactor (side_filler_, grow ? 0 : 1);
      side_filler_->setVisible (!grow);
    }
  if (box_on ("voacap")) schedule_voacap ();
  if (box_on ("sun") && isVisible ()) sun_timer_->start (); else sun_timer_->stop ();
}

// The DX box keeps one height whatever station is in it, so VOACAP below
// it never changes size, and its extra room goes to a big photo.
void ConditionsWindow::fit_dx_box ()
{
  auto * dx = boxes_.value ("dx");
  if (!dx || !side_) return;
  auto * info = qobject_cast<CallsignInfo *> (call_info_);
  int const line = QFontMetrics {QApplication::font ()}.lineSpacing ();
  // margins, title, the QRZ details with the small photo, then the table
  // of grid, local time, sun, short and long path
  int const details = info ? info->compactHeight () : 2 * line + 24;
  int const normal = 22 + line + 4 + details + 8 + 5 * (line + 2) + 6;
  // the room the old row of space weather tiles took now goes to the photo
  bool const roomy = box_on ("dx");
  int const freed = roomy ? qRound (line * 5.5) + 12 : 0;
  if (info)
    {
      info->setBigPhoto (roomy);
      if (auto * box = qobject_cast<QVBoxLayout *> (dx->layout ())) box->setStretchFactor (info, roomy ? 1000 : 0);
    }
  dx->setFixedHeight (normal + freed);
}

void ConditionsWindow::build_ui ()
{
  boxes_menu_ = new QMenu {this};
  // everything scrolls if very large text makes it bigger than the screen
  auto * scroll = new QScrollArea {this};
  scroll->setObjectName ("plainScroll");
  scroll->setFrameShape (QFrame::NoFrame);
  scroll->setWidgetResizable (true);
  auto * content = new QWidget {scroll};
  content->setObjectName ("plain");
  scroll->setWidget (content);
  scroll->viewport ()->setObjectName ("plain");
  auto * whole = new QVBoxLayout {this};
  whole->setContentsMargins (0, 0, 0, 0);
  whole->addWidget (scroll);
  auto * outer = new QVBoxLayout {content};
  outer->setContentsMargins (14, 14, 14, 10);
  outer->setSpacing (12);
  QVBoxLayout * body;

  // ---- top row: call and clocks, X-ray graph, the Sun, beacons ----
  top_row_ = new QWidget {this};
  auto * top = new QHBoxLayout {top_row_};
  top->setContentsMargins (0, 0, 0, 0);
  top->setSpacing (12);
  // tall enough for its text at any text size
  top_row_->setMaximumHeight (qMax (215, 14 * QFontMetrics {QApplication::font ()}.height ()));

  auto * clock_box = make_box ("clock", {}, body);
  auto * clock_row = new QHBoxLayout;
  clock_row->setSpacing (16);
  // the call, clock and date sit in a column of fixed width, so the
  // picture beside them keeps its size as the seconds tick over
  clock_column_ = new QWidget {clock_box};
  clock_column_->setObjectName ("plain");
  auto * clock_left = new QVBoxLayout {clock_column_};
  clock_left->setContentsMargins (0, 0, 0, 0);
  clock_left->setSpacing (4);
  clock_row->addWidget (clock_column_);
  station_logo_ = new StationLogo {clock_box};
  station_logo_->setObjectName ("plain");
  station_logo_->on_double_click = [this] {show_station_pictures ();};
  clock_row->addWidget (station_logo_, 1);
  body->addLayout (clock_row);
  call_label_ = new CallSign {clock_box};
  call_label_->setSizePolicy (QSizePolicy::Preferred, QSizePolicy::Fixed);
  call_label_->setObjectName ("plain");
  call_label_->on_double_click = [this] {show_call_colors ();};
  utc_clock_ = new QLabel {clock_box};
  utc_clock_->setTextFormat (Qt::RichText);
  utc_date_ = new QLabel {clock_box};
  utc_date_->setObjectName ("muted");
  local_clock_ = new QLabel {clock_box};      // kept for the local time, shown in the date line
  local_clock_->hide ();
  // you: grid, where that is, sunrise and sunset
  de_info_ = new QLabel {clock_box};
  de_info_->setTextFormat (Qt::RichText);
  de_info_->setObjectName ("plain");
  clock_left->addWidget (box_title (tr ("Station"), clock_box));
  clock_left->addStretch ();
  clock_left->addWidget (call_label_);
  clock_left->addWidget (utc_clock_);
  clock_left->addWidget (utc_date_);
  clock_left->addSpacing (2);
  clock_left->addWidget (de_info_);
  clock_left->addStretch ();
  top->addWidget (clock_box, 32);
  settings_->beginGroup ("Conditions");
  station_picture_ = settings_->value ("StationPicture", QString {}).toString ();
  settings_->endGroup ();
  apply_station_picture ();
  settings_->beginGroup ("Conditions");
  call_auto_ = settings_->value ("CallColorAuto", true).toBool ();
  call_first_ = QColor {settings_->value ("CallColor1", "#4CC2FF").toString ()};
  call_second_ = QColor {settings_->value ("CallColor2", "#7C5CFF").toString ()};
  call_gradient_ = settings_->value ("CallGradient", true).toBool ();
  settings_->endGroup ();

  auto * xray_box = make_box ("xray", {}, body);
  xray_graph_ = new XRayGraph {xray_box};
  xray_graph_->setObjectName ("plain");
  body->addWidget (xray_graph_);
  top->addWidget (xray_box, 30);

  // the picture fills the box: no title row, the menu sits on the picture
  auto * sun_box = make_box ("sun", {}, body);
  body->setContentsMargins (6, 6, 6, 6);
  // all black, so the picture has no edges
  sun_box->setStyleSheet ("QFrame#conditionsBox {background-color: #000000;}");
  sun_image_ = new SunImage {sun_box};
  sun_image_->setObjectName ("plain");
  sun_image_->set_message (tr ("The Sun from NASA SDO"));
  sun_image_->setToolTip (tr ("The latest picture of the Sun from NASA's Solar Dynamics Observatory. "
                              "Bright spots in the ultraviolet views are active regions that can flare."));
  // every picture SDO puts up, picked from a menu by the title
  {
    struct Pic {char const * channel; QString text; QString tip;};
    QVector<Pic> const pics {
      {"211193171", tr ("Composite (211 · 193 · 171 %1)").arg (QChar {0x00C5}), tr ("Three ultraviolet views in one: the corona and active regions")},
      {"HMIB", tr ("Magnetogram"), tr ("Magnetic field: black and white are opposite polarities")},
      {"HMIIC", tr ("6173 %1 (visible light)").arg (QChar {0x00C5}), tr ("Visible light: sunspots")},
      {"0131", tr ("131 %1 (flares)").arg (QChar {0x00C5}), tr ("The hottest material: flares light up here")},
      {"0171", tr ("171 %1").arg (QChar {0x00C5}), tr ("Corona: loops over active regions")},
      {"0193", tr ("193 %1").arg (QChar {0x00C5}), tr ("Corona: active regions and coronal holes")},
      {"0211", tr ("211 %1").arg (QChar {0x00C5}), tr ("Active regions and coronal holes")},
      {"0304", tr ("304 %1").arg (QChar {0x00C5}), tr ("Chromosphere: filaments and prominences")},
      {"rotate", tr ("Rotate through them all"), tr ("A different picture every 20 seconds")},
    };
    auto * menu = new QMenu {sun_box};
    sun_pictures_ = new QActionGroup {menu};
    settings_->beginGroup ("Conditions");
    auto const saved = settings_->value ("SunPicture", "0193").toString ();
    bool const movie = settings_->value ("SunMovie", false).toBool ();
    settings_->endGroup ();
    for (auto const& pic : pics)
      {
        if (QLatin1String {"rotate"} == pic.channel) menu->addSeparator ();
        auto * a = menu->addAction (pic.text);
        a->setCheckable (true);
        a->setData (QString {pic.channel});
        a->setToolTip (pic.tip);
        a->setChecked (saved == pic.channel);
        sun_pictures_->addAction (a);
        if (QLatin1String {"rotate"} != pic.channel) sun_channels_ << pic.channel;
      }
    if (!sun_pictures_->checkedAction ()) sun_pictures_->actions ()[5]->setChecked (true);
    menu->setToolTipsVisible (true);
    menu->addSeparator ();
    sun_movie_action_ = menu->addAction (tr ("Show movie (last 48 hours)"));
    sun_movie_action_->setCheckable (true);
    sun_movie_action_->setChecked (movie);
    connect (menu->addAction (tr ("Gray line tool...")), &QAction::triggered, this, &ConditionsWindow::gray_line_tool);
    sun_pick_ = new QToolButton {sun_box};
    sun_pick_->setPopupMode (QToolButton::InstantPopup);
    sun_pick_->setMenu (menu);
    sun_pick_->setToolTip (tr ("Which picture of the Sun, the last two days as a movie, and the gray line tool"));
    auto picked = [this] {
      auto const choice = sun_choice ();
      settings_->beginGroup ("Conditions");
      settings_->setValue ("SunPicture", choice);
      settings_->setValue ("SunMovie", sun_movie_action_->isChecked ());
      settings_->endGroup ();
      sun_pick_->setText (sun_pictures_->checkedAction ()->text ().section (" (", 0, 0)
                          .replace (tr ("Rotate through them all"), tr ("Rotate")));
      if ("rotate" == choice && !sun_movie_action_->isChecked ()) sun_rotate_->start ();
      else sun_rotate_->stop ();
      if (sun_movie_action_->isChecked ()) load_sun_movie ();
      else
        {
          stop_sun_movie ();
          fetch_sun ();
        }
    };
    sun_rotate_ = new QTimer {this};
    sun_rotate_->setInterval (20000);
    connect (sun_rotate_, &QTimer::timeout, this, [this] {
        int const i = sun_channels_.indexOf (sun_channel_);
        show_sun_channel (sun_channels_.value ((i + 1) % sun_channels_.size ()));
      });
    sun_movie_timer_ = new QTimer {this};
    sun_movie_timer_->setInterval (150);
    connect (sun_movie_timer_, &QTimer::timeout, this, [this] {
        if (sun_frames_.isEmpty ()) return;
        sun_frame_ = (sun_frame_ + 1) % (sun_frames_.size () + 8);     // a pause at the end
        int const i = qMin (sun_frame_, sun_frames_.size () - 1);
        sun_image_->set_image (sun_frames_[i].second);
        sun_image_->set_caption (sun_frames_[i].first.toString ("d MMM hh:mm") + " UTC");
      });
    for (auto * a : sun_pictures_->actions ()) connect (a, &QAction::triggered, this, picked);
    connect (sun_movie_action_, &QAction::triggered, this, picked);
    sun_pick_->setText (sun_pictures_->checkedAction ()->text ().section (" (", 0, 0)
                        .replace (tr ("Rotate through them all"), tr ("Rotate")));
    if ("rotate" == sun_choice () && !movie) sun_rotate_->start ();
  }
  // no menu button: double-click the Sun for the choice of pictures
  sun_pick_->hide ();
  sun_image_->setToolTip (tr ("Double-click to choose the picture of the Sun, the movie or the gray line tool"));
  sun_image_->installEventFilter (this);
  body->addWidget (sun_image_, 1);
  top->addWidget (sun_box, 15);

  // ---- space weather: a small box of readouts in the top bar ----
  {
    auto * spacewx = make_box ("spacewx", tr ("Space weather"), body);
    spacewx->setToolTip (tr ("<p>Space weather from NOAA's Space Weather Prediction Center. "
                             "Hover over a reading for what it means.</p>"));
    auto * grid = new QGridLayout;
    grid->setHorizontalSpacing (10);
    grid->setVerticalSpacing (0);
    struct {char const * key; char const * caption; char const * tip; int row; int col;} const defs[] = {
      {"sfi", QT_TR_NOOP ("SFI"), QT_TR_NOOP ("10.7 cm solar flux index. Higher is better for the high bands."), 0, 0},
      {"ssn", QT_TR_NOOP ("SSN"), QT_TR_NOOP ("Sunspot number reported by NOAA for the last full day."), 1, 0},
      {"k", QT_TR_NOOP ("K"), QT_TR_NOOP ("Planetary K index, 0 to 9. Low is quiet; 5 or more is a geomagnetic storm."), 2, 0},
      {"a", QT_TR_NOOP ("A"), QT_TR_NOOP ("Planetary A index for the last 24 hours. Under 10 is quiet."), 3, 0},
      {"xray", QT_TR_NOOP ("X-ray"), QT_TR_NOOP ("Solar X-ray level now. M and X class flares can black out HF on the daylight side."), 0, 2},
      {"wind", QT_TR_NOOP ("Wind"), QT_TR_NOOP ("Solar wind speed in km/s. Fast wind (over 500) can disturb the bands."), 1, 2},
      {"bz", QT_TR_NOOP ("Bz"), QT_TR_NOOP ("North-south part of the solar magnetic field, in nT. Strongly negative (southward) means storms are more likely."), 2, 2},
    };
    for (auto const& d : defs)
      {
        Tile t;
        t.caption = new QLabel {tr (d.caption), spacewx};
        t.caption->setObjectName ("muted");
        t.caption->setToolTip (tr (d.tip));
        t.value = new QLabel {QString {QChar {0x2014}}, spacewx};
        t.value->setTextFormat (Qt::RichText);
        t.value->setToolTip (tr (d.tip));
        t.note = new QLabel {spacewx};
        t.note->setObjectName ("muted");
        t.note->setToolTip (tr (d.tip));
        t.tip = tr (d.tip);
        // each reading takes two rows: the value, then a word on what it means
        grid->addWidget (t.caption, 2 * d.row, d.col, Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget (t.value, 2 * d.row, d.col + 1, Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget (t.note, 2 * d.row + 1, d.col + 1, Qt::AlignLeft | Qt::AlignTop);
        tiles_[d.key] = t;
      }
    grid->setColumnStretch (1, 1);
    grid->setColumnStretch (3, 1);
    body->addLayout (grid);
    body->addStretch ();
    spacewx_time_ = new QLabel {spacewx};
    spacewx_time_->setObjectName ("muted");
    body->addWidget (spacewx_time_, 0, Qt::AlignRight);
    top->addWidget (spacewx, 13);
  }

  auto * beacon_box = make_box ("beacons", tr ("NCDXF beacons"), body);
  beacon_box->setToolTip (tr ("<p>The NCDXF/IARU beacons take turns sending for ten seconds on each band, "
                              "at 100 watts. If you can hear one, the band is open to it.</p>"
                              "<p>Switch on <b>Beacons</b> over the map to see where they are.</p>"));
  beacon_rows_[0] = new QLabel {beacon_box};
  beacon_rows_[0]->setTextFormat (Qt::RichText);
  body->addWidget (beacon_rows_[0]);
  beacon_bar_ = new QProgressBar {beacon_box};
  beacon_bar_->setObjectName ("beaconBar");
  beacon_bar_->setRange (0, 100);
  beacon_bar_->setTextVisible (false);
  beacon_bar_->setFixedHeight (4);
  body->addWidget (beacon_bar_);
  beacon_next_ = new QLabel {beacon_box};
  beacon_next_->setObjectName ("muted");
  body->addWidget (beacon_next_);
  body->addStretch ();
  top->addWidget (beacon_box, 10);
  // left to right: station, beacons, X-ray, space weather, the Sun
  {
    QWidget * const order[] = {clock_box, beacon_box, xray_box, boxes_.value ("spacewx"), sun_box};
    int const stretch[] = {32, 10, 30, 13, 15};
    for (auto * w : order) top->removeWidget (w);
    for (int i = 0; i < 5; ++i) top->addWidget (order[i], stretch[i]);
  }
  outer->addWidget (top_row_);


  // ---- you, the DX station and the band outlook beside the map ----
  auto * middle = new QHBoxLayout;
  middle->setSpacing (12);
  left_column_ = new QWidget {this};
  auto * side = new QVBoxLayout {left_column_};
  side->setContentsMargins (0, 0, 0, 0);
  side->setSpacing (12);
  left_column_->setObjectName ("plain");

  auto * dx_box = make_box ("dx", tr ("DX"), body);
  dx_info_ = new QLabel {dx_box};
  dx_info_->setWordWrap (true);
  dx_info_->setTextFormat (Qt::RichText);
  body->addWidget (dx_info_);
  body->addStretch (1);              // extra room goes under the details
  side->addWidget (dx_box);

  auto * voacap_box = make_box ("voacap", {}, body);
  {
    auto * head = new QHBoxLayout;
    head->addWidget (box_title (tr ("VOACAP · DE → DX"), voacap_box));
    head->addStretch ();
    voacap_call_ = new QLabel {voacap_box};
    voacap_call_->setStyleSheet ("font-weight: 600;");
    head->addWidget (voacap_call_);
    body->addLayout (head);
  }
  voacap_box->setToolTip (tr ("<p>VOACAP's prediction for the path from you to the DX station: how likely each band "
                              "is to work, hour by hour (UTC), for the month and the sunspot level now. The outlined "
                              "column is this hour.</p><p>Both ends are taken as simple antennas with no gain; "
                              "a good beam does better.</p>"));
  {
    auto * settings_row = new QHBoxLayout;
    settings_row->setSpacing (4);
    voacap_power_ = new QComboBox {voacap_box};
    for (int w : {5, 10, 15, 20, 25, 50, 100, 200, 500, 1000, 1500}) voacap_power_->addItem (QString {"%1 W"}.arg (w), w);
    voacap_power_->setToolTip (tr ("Your transmitter power"));
    voacap_mode_ = new QComboBox {voacap_box};
    // signal to noise each mode needs, in a 1 Hz bandwidth
    struct {char const * name; double snr;} const modes[] = {{"FT8", 13.}, {"FT4", 17.}, {"WSPR", 6.}, {"CW", 24.}, {"SSB", 44.}};
    for (auto const& m : modes) voacap_mode_->addItem (m.name, m.snr);
    voacap_mode_->setToolTip (tr ("The mode, which sets how strong a signal has to be to get through"));
    voacap_toa_ = new QComboBox {voacap_box};
    for (double a : {0.5, 1., 3., 5., 10., 15.}) voacap_toa_->addItem (QString {"%1%2"}.arg (a).arg (QChar {0x00B0}), a);
    voacap_toa_->setToolTip (tr ("The lowest take-off angle your antenna radiates at. Low antennas and verticals "
                                 "near buildings: 5 to 10%1. High antennas over open ground: 1 to 3%1.").arg (QChar {0x00B0}));
    for (auto * combo : {voacap_power_, voacap_mode_, voacap_toa_})
      {
        combo->setSizeAdjustPolicy (QComboBox::AdjustToContents);
        settings_row->addWidget (combo);
      }
    settings_row->addStretch ();
    body->addLayout (settings_row);
    settings_row = new QHBoxLayout;
    settings_row->addWidget (make_segments ({tr ("Short"), tr ("Long")}, {tr ("Short path"), tr ("Long path, the other way round the world")},
                                            voacap_path_, voacap_box));
    settings_row->addStretch ();
    body->addLayout (settings_row);
  }
  voacap_grid_ = new VoacapGrid {voacap_box};
  voacap_grid_->setObjectName ("plain");
  voacap_grid_->on_band = [this] (int b) {select_prop_band (b);};
  body->addWidget (voacap_grid_, 1);
  voacap_now_ = new QLabel {voacap_box};
  voacap_now_->setWordWrap (true);
  voacap_now_->setTextFormat (Qt::RichText);
  body->addWidget (voacap_now_);
  settings_->beginGroup ("Conditions");
  voacap_power_->setCurrentIndex (qMax (0, voacap_power_->findData (settings_->value ("VoacapPower", 100).toInt ())));
  voacap_mode_->setCurrentIndex (qMax (0, voacap_mode_->findText (settings_->value ("VoacapMode", "FT8").toString ())));
  voacap_toa_->setCurrentIndex (qMax (0, voacap_toa_->findData (settings_->value ("VoacapTOA", 3.).toDouble ())));
  voacap_path_->button (settings_->value ("VoacapLongPath", false).toBool () ? 1 : 0)->setChecked (true);
  settings_->endGroup ();
  voacap_timer_ = new QTimer {this};
  voacap_timer_->setSingleShot (true);
  voacap_timer_->setInterval (600);
  connect (voacap_timer_, &QTimer::timeout, this, &ConditionsWindow::run_voacap);
  area_timer_ = new QTimer {this};
  area_timer_->setSingleShot (true);
  area_timer_->setInterval (800);
  connect (area_timer_, &QTimer::timeout, this, &ConditionsWindow::run_area);
  auto voacap_changed = [this] {
    settings_->beginGroup ("Conditions");
    settings_->setValue ("VoacapPower", voacap_power_->currentData ());
    settings_->setValue ("VoacapMode", voacap_mode_->currentText ());
    settings_->setValue ("VoacapTOA", voacap_toa_->currentData ());
    settings_->setValue ("VoacapLongPath", 1 == voacap_path_->checkedId ());
    settings_->endGroup ();
    schedule_voacap ();
  };
  for (auto * combo : {voacap_power_, voacap_mode_, voacap_toa_})
    {
      connect (combo, static_cast<void (QComboBox::*) (int)> (&QComboBox::currentIndexChanged), this, voacap_changed);
    }
  connect (voacap_path_, static_cast<void (QButtonGroup::*) (QAbstractButton *)> (&QButtonGroup::buttonClicked), this, voacap_changed);
  side->addWidget (voacap_box);

  side_ = side;
  side_filler_ = new QWidget {left_column_};      // takes up the space when no box can
  side->addWidget (side_filler_, 1);
  // the column scrolls if its boxes don't fit the window's height
  left_scroll_ = new QScrollArea {this};
  left_scroll_->setObjectName ("plainScroll");
  left_scroll_->setWidget (left_column_);
  left_scroll_->setWidgetResizable (true);
  left_scroll_->setFrameShape (QFrame::NoFrame);
  left_scroll_->setHorizontalScrollBarPolicy (Qt::ScrollBarAlwaysOff);
  left_scroll_->setFixedWidth (310);
  left_scroll_->viewport ()->setObjectName ("plain");
  middle->addWidget (left_scroll_);

  // the map, in its own card
  auto * map_card = new QFrame {this};
  map_card->setObjectName ("conditionsBox");
  auto * map_column = new QVBoxLayout {map_card};
  map_column->setContentsMargins (12, 12, 12, 12);
  map_column->setSpacing (10);
  map_ = new ConditionsMap {map_card};
  map_->setObjectName ("plain");
  map_->on_double_click = [this] (QString const& call, Qt::KeyboardModifiers modifiers) {
    Q_EMIT station_double_clicked (call, modifiers);
  };
  news_ = new NewsTicker {map_card};
  news_->setObjectName ("plain");      // the card shows round its rounded corners
  news_->hide ();
  news_->set_message (tr ("Getting the headlines..."));
  map_column->addWidget (build_map_bar ());
  map_column->addWidget (news_);
  map_column->addWidget (map_, 1);
  middle->addWidget (map_card, 1);
  outer->addLayout (middle, 1);

  status_ = new QLabel {this};
  status_->setObjectName ("muted");
  status_->setWordWrap (true);
  outer->addWidget (status_);

  // the Boxes menu
  add_box_action ("clock", tr ("Call sign and clock"));
  add_box_action ("xray", tr ("X-ray graph"));
  add_box_action ("sun", tr ("The Sun (NASA SDO)"));
  add_box_action ("beacons", tr ("NCDXF beacons"));
  add_box_action ("spacewx", tr ("Space weather"));
  add_box_action ("dx", tr ("DX station"));
  add_box_action ("voacap", tr ("VOACAP DE-DX"));
  add_box_action ("bands", tr ("Band outlook"));
  update_containers ();

  resize (1280, 860);
  apply_colors ();
  update_beacons ();
}

QWidget * ConditionsWindow::build_map_bar ()
{
  auto * bar = new QWidget {this};
  bar->setObjectName ("plain");
  map_bar_ = bar;
  auto * rows = new QVBoxLayout {bar};
  rows->setContentsMargins (0, 0, 0, 0);
  rows->setSpacing (8);
  auto * row1 = new QHBoxLayout;
  row1->setSpacing (8);
  auto * row2_widget = new QWidget {bar};
  row2_widget->setObjectName ("plain");
  auto * row2 = new FlowLayout {row2_widget, 8};
  row2->set_last_right (true);
  rows->addLayout (row1);
  rows->addWidget (row2_widget);

  // which map, and how it's drawn: three menus; the switches they set
  // are kept, hidden, so earlier settings carry over
  auto * styles = make_segments ({tr ("Day/Night"), tr ("Satellite"), tr ("Countries"), tr ("Terrain")}, {}, style_group_, bar);
  auto * projections = make_segments ({tr ("Flat"), tr ("Centred on you"), tr ("Robinson"), tr ("Centred on you, near half")}, {},
                                      projection_group_, bar);
  styles->hide ();
  projections->hide ();
  grid_box_ = new QComboBox {bar};
  grid_box_->addItem (tr ("No grid"));
  grid_box_->addItem (tr ("Lat/long"));
  grid_box_->addItem (tr ("Maidenhead fields"));
  grid_box_->addItem (tr ("Maidenhead squares"));
  grid_box_->addItem (tr ("Equator and tropics"));
  grid_box_->addItem (tr ("Beam headings and distance"));
  grid_box_->addItem (tr ("CQ zones"));
  grid_box_->addItem (tr ("ITU zones"));
  grid_box_->hide ();
  auto menu_button = [bar] (QString const& tip) {
    auto * b = new QToolButton {bar};
    b->setPopupMode (QToolButton::InstantPopup);
    b->setToolTip (tip);
    b->setMenu (new QMenu {b});
    b->menu ()->setToolTipsVisible (true);
    return b;
  };
  auto heading = [] (QMenu * m, QString const& text) {
    auto * a = m->addAction (text);
    auto f = a->font ();
    f.setBold (true);
    f.setCapitalization (QFont::AllUppercase);
    f.setPointSizeF (qMax (6.5, f.pointSizeF () * 0.85));
    a->setFont (f);
    a->setEnabled (false);
  };
  style_button_ = menu_button (tr ("The map under everything, and whether it's coloured by conditions"));
  {
    auto * m = style_button_->menu ();
    heading (m, tr ("The Earth"));
    auto * earth = new QActionGroup {m};
    QStringList const names {tr ("Day/Night"), tr ("Satellite"), tr ("Countries"), tr ("Terrain")};
    QStringList const tips {tr ("The Earth by day, city lights on the night side"), tr ("The Earth by day everywhere"),
                            tr ("Plain land and borders in this color scheme"), tr ("Mountains and the sea floor")};
    for (int i = 0; i < 4; ++i)
      {
        auto * a = m->addAction (names[i]);
        a->setCheckable (true);
        a->setToolTip (tips[i]);
        earth->addAction (a);
        connect (a, &QAction::triggered, this, [this, i] {style_group_->button (i)->click ();});
        connect (m, &QMenu::aboutToShow, a, [this, a, i] {a->setChecked (style_group_->checkedId () == i);});
      }
    m->addSeparator ();
    heading (m, tr ("Coloured by conditions"));
    colour_by_ = new QActionGroup {m};
    QStringList const cond {tr ("Nothing"), tr ("DRAP absorption (NOAA)"), tr ("MUF now (ionosondes, KC2G)")};
    QStringList const cond_tips {
      tr ("Just the map"),
      tr ("<p>NOAA's D-region absorption: the highest frequency losing 1 dB or more going through the bottom of the "
          "ionosphere. After a flare the daylight side lights up: bands under that number are hard going.</p>"),
      tr ("<p>The maximum usable frequency for a 3,000 km hop, now, from ionosondes round the world "
          "(prop.kc2g.com). Bands under the MUF can skip from that part of the world.</p>")};
    settings_->beginGroup ("Conditions");
    int const by = qBound (0, settings_->value ("MapColourBy", 0).toInt (), 2);
    settings_->endGroup ();
    for (int i = 0; i < 3; ++i)
      {
        auto * a = m->addAction (cond[i]);
        a->setCheckable (true);
        a->setChecked (i == by);
        a->setData (i);
        a->setToolTip (cond_tips[i]);
        colour_by_->addAction (a);
      }
  }
  projection_button_ = menu_button (tr ("How the round Earth is laid out flat"));
  {
    auto * m = projection_button_->menu ();
    heading (m, tr ("Projection"));
    auto * group = new QActionGroup {m};
    struct P {int id; QString text; QString tip;};
    QVector<P> const ps {
      {0, tr ("Flat"), tr ("The whole world, north up")},
      {2, tr ("Robinson"), tr ("The whole world with rounded sides, less stretched near the poles")},
      {1, tr ("Centred on you"), tr ("<p>Azimuthal: a straight line from the middle is the beam heading to that place, and the "
                                     "farther out, the farther away. The edge is the far side of the world.</p>")},
      {3, tr ("Centred on you, near half"), tr ("<p>The same, but only the half of the world nearest you (out to 10,000 km), "
                                                "so it's bigger.</p>")},
    };
    for (auto const& pr : ps)
      {
        auto * a = m->addAction (pr.text);
        a->setCheckable (true);
        a->setToolTip (pr.tip);
        group->addAction (a);
        int const id = pr.id;
        connect (a, &QAction::triggered, this, [this, id] {projection_group_->button (id)->click ();});
        connect (m, &QMenu::aboutToShow, a, [this, a, id] {a->setChecked (projection_group_->checkedId () == id);});
      }
  }
  grid_button_ = menu_button (tr ("<p>Lines over the map.</p><p>Point at the map to see the grid square, the nearest city and how far it is from you.</p>"));
  {
    auto * m = grid_button_->menu ();
    heading (m, tr ("Grid lines"));
    auto * group = new QActionGroup {m};
    struct G {int id; QString tip;};
    QVector<G> const gs {
      {0, tr ("No lines")}, {4, tr ("The equator, the tropics and the polar circles")}, {1, tr ("Lines every 15 degrees")},
      {2, tr ("Maidenhead fields AA to RR")}, {3, tr ("The fields and the squares inside them")},
      {5, tr ("Beam headings every 30 degrees and distance rings every 2,500 km from you")},
      {6, tr ("The 40 CQ zones")}, {7, tr ("The 90 ITU zones")},
    };
    for (auto const& g : gs)
      {
        auto * a = m->addAction (grid_box_->itemText (g.id));
        a->setCheckable (true);
        a->setToolTip (g.tip);
        group->addAction (a);
        int const id = g.id;
        connect (a, &QAction::triggered, this, [this, id] {grid_box_->setCurrentIndex (id);});
        connect (m, &QMenu::aboutToShow, a, [this, a, id] {a->setChecked (grid_box_->currentIndex () == id);});
      }
  }
  row1->addWidget (style_button_);
  row1->addWidget (projection_button_);
  row1->addWidget (grid_button_);
  gray_box_ = make_chip (tr ("Gray line"), tr ("Shade the night side of the Earth"), bar);
  aurora_box_ = make_chip (tr ("Aurora"), tr ("NOAA's 30-minute aurora forecast. Strong aurora can absorb or reflect HF and VHF signals near the poles."), bar);
  rain_box_ = make_chip (tr ("Rain"), tr ("Where it's raining or snowing over the last half hour or so, from NASA satellites"), bar);
  news_box_ = make_chip (tr ("News"), tr ("Ham radio headlines from ARRL, AMSAT and DX-World, and news about KF\u00D8KYL Digi, scrolling under these buttons. Click one to read it."), bar);
  cities_box_ = make_chip (tr ("Cities"), tr ("Names of cities. Zoom in to see more of them. Capitals have a square."), bar);
  beacons_box_ = make_chip (tr ("Beacons"), tr ("The 18 NCDXF/IARU beacons. The ones sending now are filled in the color of their band."), bar);
  prop_box_ = make_chip (tr ("Propagation"), tr ("<p>VOACAP's map for one band from you: how likely it is to reach each part of the world "
                                                  "this hour, at the power, mode and take-off angle set in the VOACAP box.</p>"
                                                  "<p>Click a band in the VOACAP box to choose it.</p>"), bar);
  worked_grids_box_ = make_chip (tr ("Worked grids"), tr ("<p>Shade the grid squares in your log, on the band you're on or on every band.</p>"
                                                          "<p>Click the arrow for the choices.</p>"), bar);
  psk_box_ = make_chip (tr ("Heard me"), tr ("<p>Lines to the stations that reported hearing you, from PSK Reporter. "
                                             "Reports arrive live, seconds after a station hears you.</p>"
                                             "<p>Click the arrow to choose the band, how far back to show and the colours.</p>"), bar);
  {
    // the arrow beside Worked grids and Heard me opens their choices
    auto * grids_menu = new QMenu {worked_grids_box_};
    auto * grids_bands = new QActionGroup {grids_menu};
    auto * grids_this = grids_menu->addAction (tr ("This band only"));
    grids_all_action_ = grids_menu->addAction (tr ("All bands"));
    for (auto * a : {grids_this, grids_all_action_}) {a->setCheckable (true); grids_bands->addAction (a);}
    grids_menu->addSeparator ();
    auto * later = grids_menu->addAction (tr ("Confirmed (LoTW, QSL) colours are coming in 1.3.0"));
    later->setEnabled (false);
    grids_menu->addSeparator ();
    connect (grids_menu->addAction (tr ("Colours…")), &QAction::triggered, this, &ConditionsWindow::map_colours_dialog);

    auto * psk_menu = new QMenu {psk_box_};
    auto * psk_bands = new QActionGroup {psk_menu};
    auto * psk_this = psk_menu->addAction (tr ("This band only"));
    psk_all_action_ = psk_menu->addAction (tr ("All bands (lines coloured by band)"));
    for (auto * a : {psk_this, psk_all_action_}) {a->setCheckable (true); psk_bands->addAction (a);}
    psk_menu->addSeparator ();
    psk_live_action_ = psk_menu->addAction (tr ("Live (reports as they arrive)"));
    psk_live_action_->setCheckable (true);
    auto * minutes = psk_menu->addMenu (tr ("Show the last"));
    psk_minutes_group_ = new QActionGroup {minutes};
    for (int m : {15, 30, 60})
      {
        auto * a = minutes->addAction (tr ("%1 minutes").arg (m));
        a->setCheckable (true);
        a->setData (m);
        psk_minutes_group_->addAction (a);
      }
    psk_menu->addSeparator ();
    connect (psk_menu->addAction (tr ("Colours…")), &QAction::triggered, this, &ConditionsWindow::map_colours_dialog);

    for (auto * chip : {worked_grids_box_, psk_box_})
      {
        static_cast<QToolButton *> (chip)->setPopupMode (QToolButton::MenuButtonPopup);
      }
    static_cast<QToolButton *> (worked_grids_box_)->setMenu (grids_menu);
    static_cast<QToolButton *> (psk_box_)->setMenu (psk_menu);

    settings_->beginGroup ("Conditions");
    (settings_->value ("WorkedGridsAllBands", false).toBool () ? grids_all_action_ : grids_this)->setChecked (true);
    (settings_->value ("HeardMeAllBands", false).toBool () ? psk_all_action_ : psk_this)->setChecked (true);
    psk_live_action_->setChecked (settings_->value ("HeardMeLive", true).toBool ());
    int const mins = settings_->value ("HeardMeMinutes", 30).toInt ();
    bool picked = false;
    for (auto * a : psk_minutes_group_->actions ()) if (a->data ().toInt () == mins) {a->setChecked (true); picked = true;}
    if (!picked) psk_minutes_group_->actions ()[1]->setChecked (true);
    settings_->endGroup ();

    auto options = [this] {
      map_->grids_all_bands = grids_all_action_->isChecked ();
      map_->psk_all_bands = psk_all_action_->isChecked ();
      map_->psk_minutes = psk_minutes_group_->checkedAction () ? psk_minutes_group_->checkedAction ()->data ().toInt () : 30;
      map_->rebuild_psk ();
      settings_->beginGroup ("Conditions");
      settings_->setValue ("WorkedGridsAllBands", map_->grids_all_bands);
      settings_->setValue ("HeardMeAllBands", map_->psk_all_bands);
      settings_->setValue ("HeardMeLive", psk_live_action_->isChecked ());
      settings_->setValue ("HeardMeMinutes", map_->psk_minutes);
      settings_->endGroup ();
      update_psk_live ();
      map_->update ();
    };
    for (auto * a : {grids_this, grids_all_action_, psk_this, psk_all_action_, psk_live_action_})
      {
        connect (a, &QAction::triggered, this, options);
      }
    for (auto * a : psk_minutes_group_->actions ()) connect (a, &QAction::triggered, this, options);
    map_->grids_all_bands = grids_all_action_->isChecked ();
    map_->psk_all_bands = psk_all_action_->isChecked ();
    map_->psk_minutes = psk_minutes_group_->checkedAction ()->data ().toInt ();
    load_map_colours ();
    map_->on_colour = [this] (QString const& key, QString const& name) {pick_map_colour (key, name);};
  }
  for (auto * b : {gray_box_, aurora_box_, rain_box_, cities_box_, beacons_box_, worked_grids_box_, psk_box_, prop_box_, news_box_}) row2->addWidget (b);
  row1->addStretch ();
  // the band outlook: day and night for each group of bands
  {
    auto * outlook = new QWidget {bar};
    outlook->setObjectName ("plain");
    outlook->setToolTip (tr ("<p><b>Band outlook</b>, day and night: a rough estimate from the solar flux and "
                             "K index, not a forecast for any one path.</p>"));
    auto * h = new QHBoxLayout {outlook};
    h->setContentsMargins (0, 0, 0, 0);
    h->setSpacing (5);
    auto * head = new QVBoxLayout;
    head->setSpacing (0);
    auto * title = box_title (tr ("Bands"), outlook);
    title->setAlignment (Qt::AlignRight);
    auto * dn = new QLabel {tr ("day · night"), outlook};
    dn->setObjectName ("muted");
    auto f = dn->font ();
    f.setPointSizeF (qMax (6.5, f.pointSizeF () * 0.82));
    dn->setFont (f);
    dn->setAlignment (Qt::AlignRight);
    head->addWidget (title);
    head->addWidget (dn);
    h->addLayout (head);
    h->addSpacing (4);
    char const * groups[] = {"80–40", "30–20", "17–15", "12–10"};
    for (int g = 0; g < 4; ++g)
      {
        if (g) h->addSpacing (7);
        h->addWidget (new QLabel {QString::fromUtf8 (groups[g]), outlook});
        for (int d = 0; d < 2; ++d)
          {
            auto * cell = new QLabel {QString {QChar {0x2014}}, outlook};
            cell->setAlignment (Qt::AlignCenter);
            cell->setFont (f);
            band_cells_[g][d] = cell;
            h->addWidget (cell);
          }
      }
    outlook->setSizePolicy (QSizePolicy::Fixed, QSizePolicy::Preferred);
    boxes_["bands"] = outlook;
    row1->addWidget (outlook);
    row1_ = row1;
    row2_ = row2;
  }
  auto * boxes = new QToolButton {bar};
  boxes->setText (tr ("Boxes"));
  boxes->setToolTip (tr ("Choose which boxes this window shows"));
  boxes->setPopupMode (QToolButton::InstantPopup);
  boxes->setMenu (boxes_menu_);
  row2->addWidget (boxes);

  // which stations: the legend on the map; these switches hold the choice
  show_cq_ = new QCheckBox {bar};
  show_me_ = new QCheckBox {bar};
  show_called_ = new QCheckBox {bar};
  show_others_ = new QCheckBox {bar};
  for (auto * box : {show_cq_, show_me_, show_called_, show_others_}) box->hide ();
  map_->on_legend = [this] (int i) {
    QAbstractButton * boxes[] = {show_cq_, show_me_, show_called_, show_others_};
    if (i >= 0 && i < 4) boxes[i]->toggle ();
    if (4 == i)
      {
        // Worked before, then Worked today, then hidden
        worked_mode_ = (worked_mode_ + 1) % 3;
        map_->worked_mode = worked_mode_;
        save_map_options ();
        map_->update ();
      }
  };

  // restore and react
  settings_->beginGroup ("Conditions");
  style_group_->button (qBound (0, settings_->value ("MapStyle", 0).toInt (), 3))->setChecked (true);
  projection_group_->button (qBound (0, settings_->value ("MapProjection", 0).toInt (), 3))->setChecked (true);
  // older betas had a single Grid squares check box
  int const old_grid = settings_->value ("GridSquares", false).toBool () ? 2 : 0;
  grid_box_->setCurrentIndex (qBound (0, settings_->value ("MapGrid", old_grid).toInt (), grid_box_->count () - 1));
  gray_box_->setChecked (settings_->value ("GrayLine", true).toBool ());
  aurora_box_->setChecked (settings_->value ("Aurora", false).toBool ());
  rain_box_->setChecked (settings_->value ("Rain", false).toBool ());
  news_box_->setChecked (settings_->value ("News", false).toBool ());
  beacons_box_->setChecked (settings_->value ("Beacons", false).toBool ());
  cities_box_->setChecked (settings_->value ("Cities", false).toBool ());
  prop_band_ = qBound (0, settings_->value ("PropBand", 4).toInt (), VoacapGrid::bands - 1);     // 20 m
  prop_box_->setChecked (settings_->value ("Propagation", false).toBool ());
  show_cq_->setChecked (settings_->value ("ShowCQ", true).toBool ());
  show_me_->setChecked (settings_->value ("ShowCallingMe", true).toBool ());
  show_called_->setChecked (settings_->value ("ShowCalled", true).toBool ());
  show_others_->setChecked (settings_->value ("ShowOthers", true).toBool ());
  worked_mode_ = qBound (0, settings_->value ("WorkedMode", 0).toInt (), 2);
  map_->worked_mode = worked_mode_;
  worked_grids_box_->setChecked (settings_->value ("WorkedGrids", false).toBool ());
  psk_box_->setChecked (settings_->value ("HeardMe", false).toBool ());
  settings_->endGroup ();

  auto apply = [this] {
    map_->set_projection (static_cast<ConditionsMap::Projection> (projection_group_->checkedId ()));
    map_->grid = static_cast<ConditionsMap::Grid> (grid_box_->currentIndex ());
    map_->gray_line = gray_box_->isChecked ();
    map_->show_aurora = aurora_box_->isChecked ();
    map_->show_rain = rain_box_->isChecked ();
    map_->colour_by = colour_by_->checkedAction () ? colour_by_->checkedAction ()->data ().toInt () : 0;
    if (news_) news_->setVisible (news_box_->isChecked ());
    // the menus' buttons say what's chosen
    style_button_->setText (style_group_->checkedButton () ? style_group_->checkedButton ()->text () : QString {});
    if (map_->colour_by) style_button_->setText (style_button_->text () + " + " + (1 == map_->colour_by ? tr ("DRAP") : tr ("MUF")));
    QStringList const short_grids {tr ("No grid"), tr ("Lat/long"), tr ("Fields"), tr ("Squares"), tr ("Tropics"),
                                   tr ("Beam headings"), tr ("CQ zones"), tr ("ITU zones")};
    grid_button_->setText (short_grids.value (grid_box_->currentIndex ()));
    QStringList const short_proj {tr ("Flat"), tr ("Centred"), tr ("Robinson"), tr ("Near half")};
    projection_button_->setText (short_proj.value (projection_group_->checkedId ()));
    QTimer::singleShot (0, this, [this] {fit_outlook (); update_status ();});
    map_->show_beacons = beacons_box_->isChecked ();
    map_->show_cities = cities_box_->isChecked ();
    map_->show_prop = prop_box_->isChecked ();
    voacap_grid_->selected = prop_box_->isChecked () ? prop_band_ : -1;
    voacap_grid_->update ();
    map_->show_cq = show_cq_->isChecked ();
    map_->show_me = show_me_->isChecked ();
    map_->show_called = show_called_->isChecked ();
    map_->show_others = show_others_->isChecked ();
    map_->show_worked_grids = worked_grids_box_->isChecked ();
    bool const psk_was = map_->show_psk;
    map_->show_psk = psk_box_->isChecked ();
    if (map_->show_psk != psk_was)
      {
        update_psk_live ();
        if (map_->show_psk) fetch_psk ();
      }
    gray_box_->setEnabled (style_group_->checkedId () != ConditionsMap::DayNight);
    // the buttons are in the order people look for them; the map's own
    // numbers are the ones saved by earlier betas
    static int const styles[] = {ConditionsMap::DayNight, ConditionsMap::Satellite, ConditionsMap::Countries, ConditionsMap::Terrain};
    map_->set_style (static_cast<ConditionsMap::Style> (styles[qBound (0, style_group_->checkedId (), 3)]));   // redraws
    save_map_options ();
  };
  for (auto * group : {style_group_, projection_group_})
    {
      connect (group, static_cast<void (QButtonGroup::*) (QAbstractButton *)> (&QButtonGroup::buttonClicked), this, apply);
    }
  connect (grid_box_, static_cast<void (QComboBox::*) (int)> (&QComboBox::currentIndexChanged), this, apply);
  for (auto * box : {gray_box_, cities_box_, beacons_box_, static_cast<QAbstractButton *> (show_cq_), static_cast<QAbstractButton *> (show_me_),
                     static_cast<QAbstractButton *> (show_called_), static_cast<QAbstractButton *> (show_others_),
                     worked_grids_box_, psk_box_})
    {
      connect (box, &QAbstractButton::toggled, this, apply);
    }
  connect (prop_box_, &QAbstractButton::toggled, this, [this, apply] (bool on) {
      apply ();
      if (on) schedule_area ();
    });
  for (auto * a : colour_by_->actions ())
    {
      connect (a, &QAction::triggered, this, [this, apply] {
          apply ();
          if (isVisible ()) refresh_overlays ();
        });
    }
  connect (news_box_, &QAbstractButton::toggled, this, [this, apply] (bool on) {
      apply ();
      if (on) fetch_news ();
    });
  for (auto * box : {aurora_box_, rain_box_})
    {
      connect (box, &QAbstractButton::toggled, this, [this, apply] {
          apply ();
          if (isVisible ()) refresh_overlays ();
        });
    }
  QTimer::singleShot (0, this, apply);
  QTimer::singleShot (0, this, [this] {if (news_box_->isChecked ()) fetch_news ();});
  update_legend ();
  return bar;
}

void ConditionsWindow::save_map_options ()
{
  settings_->beginGroup ("Conditions");
  settings_->setValue ("MapStyle", style_group_->checkedId ());
  settings_->setValue ("MapProjection", projection_group_->checkedId ());
  settings_->setValue ("MapGrid", grid_box_->currentIndex ());
  settings_->remove ("GridSquares");
  settings_->setValue ("GrayLine", gray_box_->isChecked ());
  settings_->setValue ("Aurora", aurora_box_->isChecked ());
  settings_->setValue ("News", news_box_->isChecked ());
  if (colour_by_) settings_->setValue ("MapColourBy", colour_by_->checkedAction () ? colour_by_->checkedAction ()->data ().toInt () : 0);
  settings_->setValue ("Rain", rain_box_->isChecked ());
  settings_->setValue ("Beacons", beacons_box_->isChecked ());
  settings_->setValue ("Cities", cities_box_->isChecked ());
  settings_->setValue ("Propagation", prop_box_->isChecked ());
  settings_->setValue ("PropBand", prop_band_);
  settings_->setValue ("ShowCQ", show_cq_->isChecked ());
  settings_->setValue ("ShowCallingMe", show_me_->isChecked ());
  settings_->setValue ("ShowCalled", show_called_->isChecked ());
  settings_->setValue ("ShowOthers", show_others_->isChecked ());
  settings_->setValue ("WorkedMode", worked_mode_);
  settings_->setValue ("WorkedGrids", worked_grids_box_->isChecked ());
  settings_->setValue ("HeardMe", psk_box_->isChecked ());
  settings_->endGroup ();
}

void ConditionsWindow::update_legend ()
{
  // the legend is drawn on the map, in these colours
  map_->update ();
}

void ConditionsWindow::set_highlight_colors (QColor const& cq, QColor const& my_call, QColor const& tx)
{
  if (cq.isValid ()) cq_color_ = cq;
  if (my_call.isValid ()) my_call_color_ = my_call;
  if (tx.isValid ()) tx_color_ = tx;
  apply_map_colours ();
}

namespace
{
  // what can be chosen in Map colours, with the built in colours (an
  // invalid colour follows Settings > Colors or the scheme)
  struct MapColour {char const * key; char const * text; char const * group; QColor fallback;};
  QVector<MapColour> const& map_colour_list ()
  {
    static QVector<MapColour> const list {
      {"CQ", QT_TRANSLATE_NOOP ("ConditionsWindow", "Calling CQ"), "Stations", QColor {}},
      {"CallingMe", QT_TRANSLATE_NOOP ("ConditionsWindow", "Calling you"), "Stations", QColor {}},
      {"Called", QT_TRANSLATE_NOOP ("ConditionsWindow", "You called"), "Stations", QColor {}},
      {"Worked", QT_TRANSLATE_NOOP ("ConditionsWindow", "Worked"), "Stations", QColor {"#B58CFF"}},
      {"Others", QT_TRANSLATE_NOOP ("ConditionsWindow", "Others"), "Stations", QColor {}},
      {"Grid", QT_TRANSLATE_NOOP ("ConditionsWindow", "Worked grids"), "Layers", QColor {}},
      {"Strong", QT_TRANSLATE_NOOP ("ConditionsWindow", "Heard me, strong"), "Layers", QColor {"#3DDC84"}},
      {"Fair", QT_TRANSLATE_NOOP ("ConditionsWindow", "Heard me, fair"), "Layers", QColor {"#FFD54A"}},
      {"Weak", QT_TRANSLATE_NOOP ("ConditionsWindow", "Heard me, weak"), "Layers", QColor {"#FF7A45"}},
    };
    return list;
  }
}

void ConditionsWindow::load_map_colours ()
{
  map_colours_.clear ();
  settings_->beginGroup ("MapColours");
  for (auto const& k : settings_->childKeys ())
    {
      if ("GridOpacity" == k) continue;
      QColor const c {settings_->value (k).toString ()};
      if (c.isValid ()) map_colours_[k] = c;
    }
  map_->grid_opacity = qBound (5, settings_->value ("GridOpacity", 40).toInt (), 100);
  settings_->endGroup ();
  apply_map_colours ();
}

void ConditionsWindow::apply_map_colours ()
{
  auto pick = [this] (char const * key, QColor const& otherwise) {
    auto const c = map_colours_.value (key);
    return c.isValid () ? c : otherwise;
  };
  map_->cq_color = pick ("CQ", cq_color_);
  map_->my_call_color = pick ("CallingMe", my_call_color_);
  map_->tx_color = pick ("Called", tx_color_);
  map_->worked_color = pick ("Worked", QColor {"#B58CFF"});
  map_->others_color = map_colours_.value ("Others");
  map_->grid_color = map_colours_.value ("Grid");
  map_->psk_strong = pick ("Strong", QColor {"#3DDC84"});
  map_->psk_fair = pick ("Fair", QColor {"#FFD54A"});
  map_->psk_weak = pick ("Weak", QColor {"#FF7A45"});
  map_->band_colors.clear ();
  for (auto it = map_colours_.cbegin (); it != map_colours_.cend (); ++it)
    {
      if (it.key ().startsWith ("Band_")) map_->band_colors[it.key ().mid (5)] = it.value ();
    }
  update_legend ();
  map_->update ();
}

// a colour double-clicked in the map's legend or Heard me key
void ConditionsWindow::pick_map_colour (QString const& key, QString const& name)
{
  QString title = name;
  for (auto const& m : map_colour_list ()) if (key == m.key) title = tr (m.text);
  QColor now = map_colours_.value (key);
  if (!now.isValid ())
    {
      if ("CQ" == key) now = cq_color_;
      else if ("CallingMe" == key) now = my_call_color_;
      else if ("Called" == key) now = tx_color_;
      else if ("Others" == key) now = Win11Theme::isDark () ? QColor {"#9AA4B2"} : QColor {"#5A6472"};
      else if (key.startsWith ("Band_")) now = map_->band_color (key.mid (5));
      else for (auto const& m : map_colour_list ()) if (key == m.key) now = m.fallback;
    }
  QColor const c = QColorDialog::getColor (now, this, tr ("Map colour: %1").arg (title));
  if (!c.isValid ()) return;
  map_colours_[key] = c;
  settings_->beginGroup ("MapColours");
  settings_->setValue (key, c.name ());
  settings_->endGroup ();
  apply_map_colours ();
}

// Map colours: the dots, the worked grids and the Heard me lines
void ConditionsWindow::map_colours_dialog ()
{
  QDialog dialog {this};
  dialog.setWindowTitle (tr ("Map colours"));
  auto * outer = new QVBoxLayout {&dialog};
  auto * intro = new QLabel {tr ("Colours for the map. Ones you leave alone follow Settings > Colors and the colour scheme."), &dialog};
  intro->setWordWrap (true);
  outer->addWidget (intro);
  auto * columns = new QHBoxLayout;
  outer->addLayout (columns);
  auto chosen = map_colours_;
  int opacity = map_->grid_opacity;

  // the colour each button shows now
  auto current = [this, &chosen] (QString const& key) -> QColor {
    auto const c = chosen.value (key);
    if (c.isValid ()) return c;
    if ("CQ" == key) return cq_color_;
    if ("CallingMe" == key) return my_call_color_;
    if ("Called" == key) return tx_color_;
    if ("Others" == key) return Win11Theme::isDark () ? QColor {"#9AA4B2"} : QColor {"#5A6472"};
    if ("Grid" == key) return QColor {Win11Theme::token ("accent2")};
    if (key.startsWith ("Band_")) return ConditionsMap::default_band_color (key.mid (5));
    for (auto const& m : map_colour_list ()) if (key == m.key) return m.fallback;
    return QColor {};
  };
  QVector<QPair<QPushButton *, QString>> buttons;
  auto paint = [&] (QPushButton * b, QString const& key) {
    QColor const c = current (key);
    b->setStyleSheet (QString {"QPushButton {background: %1; border: 1px solid %2; border-radius: 4px; min-width: 44px; min-height: 18px;}"}
                      .arg (c.name ()).arg (c.lightness () > 140 ? "#00000060" : "#FFFFFF60"));
    b->setProperty ("w11_keep", true);
  };
  auto add_button = [&] (QGridLayout * grid, int row, int col, QString const& key, QString const& text) {
    auto * label = new QLabel {text, &dialog};
    auto * b = new QPushButton {&dialog};
    b->setCursor (Qt::PointingHandCursor);
    b->setToolTip (tr ("Choose a colour"));
    paint (b, key);
    connect (b, &QPushButton::clicked, &dialog, [&, b, key, text] {
        QColor const c = QColorDialog::getColor (current (key), &dialog, text);
        if (c.isValid ())
          {
            chosen[key] = c;
            paint (b, key);
          }
      });
    buttons << qMakePair (b, key);
    grid->addWidget (label, row, col);
    grid->addWidget (b, row, col + 1);
  };

  auto * left = new QVBoxLayout;
  auto * right = new QVBoxLayout;
  columns->addLayout (left);
  columns->addSpacing (18);
  columns->addLayout (right);
  auto heading = [&dialog] (QString const& text) {
    auto * l = new QLabel {text, &dialog};
    auto f = l->font ();
    f.setBold (true);
    l->setFont (f);
    return l;
  };
  left->addWidget (heading (tr ("Stations")));
  auto * stations = new QGridLayout;
  left->addLayout (stations);
  int row = 0;
  for (auto const& m : map_colour_list ())
    {
      if (QLatin1String {"Stations"} == m.group) add_button (stations, row++, 0, m.key, tr (m.text));
    }
  left->addSpacing (8);
  left->addWidget (heading (tr ("Layers")));
  auto * layers = new QGridLayout;
  left->addLayout (layers);
  row = 0;
  for (auto const& m : map_colour_list ())
    {
      if (QLatin1String {"Layers"} == m.group) add_button (layers, row++, 0, m.key, tr (m.text));
      if (QLatin1String {"Grid"} == m.key)
        {
          auto * slider = new QSlider {Qt::Horizontal, &dialog};
          slider->setRange (5, 100);
          slider->setValue (opacity);
          auto * shown = new QLabel {tr ("%1%").arg (opacity), &dialog};
          connect (slider, &QSlider::valueChanged, &dialog, [&opacity, shown] (int v) {
              opacity = v;
              shown->setText (QString {"%1%"}.arg (v));
            });
          layers->addWidget (new QLabel {tr ("Worked grids, how solid"), &dialog}, row, 0);
          auto * h = new QHBoxLayout;
          h->addWidget (slider);
          h->addWidget (shown);
          layers->addLayout (h, row++, 1);
        }
    }
  left->addStretch ();
  right->addWidget (heading (tr ("Heard me on all bands")));
  auto * bands = new QGridLayout;
  right->addLayout (bands);
  row = 0;
  for (auto const& b : ConditionsMap::band_order ())
    {
      add_button (bands, row / 2, (row % 2) * 2, "Band_" + b, b);
      ++row;
    }
  right->addStretch ();

  auto * box = new QDialogButtonBox {QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults, &dialog};
  box->button (QDialogButtonBox::RestoreDefaults)->setText (tr ("Reset to defaults"));
  connect (box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect (box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect (box->button (QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dialog, [&] {
      chosen.clear ();
      opacity = 40;
      for (auto const& b : buttons) paint (b.first, b.second);
      for (auto * s : dialog.findChildren<QSlider *> ()) s->setValue (40);
    });
  outer->addWidget (box);
  if (QDialog::Accepted != dialog.exec ()) return;

  settings_->beginGroup ("MapColours");
  settings_->remove ("");
  for (auto it = chosen.cbegin (); it != chosen.cend (); ++it) settings_->setValue (it.key (), it.value ().name ());
  settings_->setValue ("GridOpacity", opacity);
  settings_->endGroup ();
  map_colours_ = chosen;
  map_->grid_opacity = opacity;
  apply_map_colours ();
}

void ConditionsWindow::mark_called (QString const& call)
{
  if (call.isEmpty ()) return;
  map_->called.insert (call);
  if (isVisible ()) map_->update ();
}

void ConditionsWindow::refresh_overlays ()
{
  if (aurora_box_->isChecked ())
    {
      QNetworkRequest req {QUrl {swpc_base () + "json/ovation_aurora_latest.json"}};
      req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
      auto * reply = network_->get (req);
      connect (reply, &QNetworkReply::finished, this, [this, reply] {
          reply->deleteLater ();
          if (QNetworkReply::NoError != reply->error ())
            {
              overlay_status_ = tr ("The aurora forecast couldn't be reached.");
              return;
            }
          // [longitude 0-359, latitude -90..90, chance %]
          auto const doc = QJsonDocument::fromJson (reply->readAll ());
          auto const coords = doc.object ().value ("coordinates").toArray ();
          QImage img {360, 181, QImage::Format_ARGB32_Premultiplied};
          img.fill (Qt::transparent);
          for (auto const& v : coords)
            {
              auto const a = v.toArray ();
              if (a.size () < 3) continue;
              int lon = a[0].toInt ();
              int const lat = a[1].toInt ();
              int const chance = a[2].toInt ();
              if (chance < 3) continue;
              if (lon >= 180) lon -= 360;
              int const x = qBound (0, lon + 180, 359), y = qBound (0, 90 - lat, 180);
              int const alpha = qBound (40, chance * 4, 220);
              // green, turning red where it's strongest
              QColor const c = chance >= 50 ? QColor {255, 70, 70} : chance >= 20 ? QColor {255, 220, 60} : QColor {60, 255, 120};
              img.setPixel (x, y, qPremultiply (qRgba (c.red (), c.green (), c.blue (), alpha)));
            }
          map_->set_aurora (img);
          overlay_status_.clear ();
        });
    }
  if (rain_box_->isChecked ())
    {
      // NASA GIBS: rain and snow rate (IMERG), the latest image
      QUrl url {"https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi"};
      QUrlQuery q;
      q.addQueryItem ("SERVICE", "WMS");
      q.addQueryItem ("REQUEST", "GetMap");
      q.addQueryItem ("VERSION", "1.1.1");
      q.addQueryItem ("LAYERS", "IMERG_Precipitation_Rate");
      q.addQueryItem ("SRS", "EPSG:4326");
      q.addQueryItem ("BBOX", "-180,-90,180,90");
      q.addQueryItem ("WIDTH", "1024");
      q.addQueryItem ("HEIGHT", "512");
      q.addQueryItem ("FORMAT", "image/png");
      q.addQueryItem ("TRANSPARENT", "TRUE");
      url.setQuery (q);
      QNetworkRequest req {url};
      req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
      auto * reply = network_->get (req);
      connect (reply, &QNetworkReply::finished, this, [this, reply] {
          reply->deleteLater ();
          QImage img;
          if (QNetworkReply::NoError == reply->error ()) img.loadFromData (reply->readAll ());
          if (img.isNull ())
            {
              overlay_status_ = tr ("Rain and snow couldn't be reached.");
              return;
            }
          map_->set_rain (img.convertToFormat (QImage::Format_ARGB32_Premultiplied));
        });
    }
  if (colour_by_ && colour_by_->checkedAction () && colour_by_->checkedAction ()->data ().toInt ()) fetch_conditions_map ();
  bool const any = aurora_box_->isChecked () || rain_box_->isChecked ()
    || (colour_by_ && colour_by_->checkedAction () && colour_by_->checkedAction ()->data ().toInt ());
  if (any) overlay_timer_->start ();
  else overlay_timer_->stop ();
}

// the map coloured by NOAA's DRAP or by the MUF from ionosondes
void ConditionsWindow::fetch_conditions_map ()
{
  int const by = colour_by_->checkedAction ()->data ().toInt ();
  if (1 == by)
    {
      QNetworkRequest req {QUrl {swpc_base () + "text/drap_global_frequencies.txt"}};
      req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
      auto * reply = network_->get (req);
      connect (reply, &QNetworkReply::finished, this, [this, reply] {
          reply->deleteLater ();
          if (QNetworkReply::NoError != reply->error ())
            {
              overlay_status_ = tr ("DRAP couldn't be reached.");
              return;
            }
          // a table: longitudes across the top, then "lat | MHz MHz ..."
          QVector<double> lons;
          QVector<QPair<double, QVector<double>>> rows;
          double top = 0.;
          for (auto const& raw : QString::fromLatin1 (reply->readAll ()).split ('\n'))
            {
              auto const line = raw.trimmed ();
              if (line.isEmpty () || line.startsWith ('#') || QRegularExpression {"^-+$"}.match (line).hasMatch ()) continue;
              if (line.contains ('|'))
                {
                  auto const lat = line.section ('|', 0, 0).trimmed ().toDouble ();
                  QVector<double> v;
                  for (auto const& f : line.section ('|', 1).split (QRegularExpression {"\\s+"})) if (f.size ()) v << f.toDouble ();
                  for (double x : v) top = qMax (top, x);
                  rows << qMakePair (lat, v);
                }
              else if (lons.isEmpty ())
                {
                  for (auto const& f : line.split (QRegularExpression {"\\s+"})) if (f.size ()) lons << f.toDouble ();
                }
            }
          if (rows.isEmpty () || lons.size () < 2)
            {
              overlay_status_ = tr ("DRAP couldn't be read.");
              return;
            }
          // a gentle scale when it's quiet, up to 35 MHz in a big flare
          double const scale = qMax (10., qCeil (top / 5.) * 5.);
          QImage img {int (lons.size ()), int (rows.size ()), QImage::Format_ARGB32_Premultiplied};
          for (int y = 0; y < rows.size (); ++y)
            {
              for (int x = 0; x < lons.size (); ++x)
                {
                  double const v = rows[y].second.value (x, 0.);
                  QColor c = mhz_colour (v, scale);
                  // nothing absorbed: leave the map clear
                  int const a = v <= 0.05 ? 0 : qBound (90, int (110 + v / scale * 145), 235);
                  img.setPixel (x, y, qPremultiply (qRgba (c.red (), c.green (), c.blue (), a)));
                }
            }
          map_->set_conditions (img.scaled (720, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                                tr ("DRAP: absorbed up to"), scale);
        });
      return;
    }
  // the MUF at each ionosonde, spread between them
  QNetworkRequest req {QUrl {kc2g_base () + "api/stations.json"}};
  req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
  auto * reply = network_->get (req);
  connect (reply, &QNetworkReply::finished, this, [this, reply] {
      reply->deleteLater ();
      if (QNetworkReply::NoError != reply->error ())
        {
          // the footer says when VOACAP's prediction is standing in
          predicted_muf ();
          return;
        }
      struct P {double lat; double lon; double muf;};
      QVector<P> points;
      auto const now = QDateTime::currentDateTimeUtc ();
      for (auto const& v : QJsonDocument::fromJson (reply->readAll ()).array ())
        {
          auto const o = v.toObject ();
          auto const st = o.value ("station").toObject ();
          double const muf = o.value ("mufd").toDouble (qQNaN ());
          double lat = st.value ("latitude").toVariant ().toDouble ();
          double lon = st.value ("longitude").toVariant ().toDouble ();
          auto const when = QDateTime::fromString (o.value ("time").toString ().left (19), Qt::ISODate);
          if (qIsNaN (muf) || muf <= 0.) continue;
          if (when.isValid () && QDateTime {when.date (), when.time (), Qt::UTC}.secsTo (now) > 3 * 3600) continue;
          if (lon > 180.) lon -= 360.;
          points << P {lat, lon, muf};
        }
      if (points.isEmpty ())
        {
          predicted_muf ();
          return;
        }
      // weighted by distance; far from any ionosonde it fades out
      QImage img {360, 180, QImage::Format_ARGB32_Premultiplied};
      QVector<double> sl, cl, so, co;
      for (auto const& pt : points)
        {
          sl << std::sin (rad (pt.lat)); cl << std::cos (rad (pt.lat));
          so << std::sin (rad (pt.lon)); co << std::cos (rad (pt.lon));
        }
      for (int y = 0; y < 180; ++y)
        {
          double const lat = rad (89.5 - y);
          double const s1 = std::sin (lat), c1 = std::cos (lat);
          for (int x = 0; x < 360; ++x)
            {
              double const lon = rad (-179.5 + x);
              double const s2 = std::sin (lon), c2 = std::cos (lon);
              double sum = 0., weights = 0., nearest = 1e9;
              for (int i = 0; i < points.size (); ++i)
                {
                  double const cosd = s1 * sl[i] + c1 * cl[i] * (c2 * co[i] + s2 * so[i]);
                  double const d = std::acos (qBound (-1., cosd, 1.));
                  nearest = qMin (nearest, d);
                  double const w = 1. / std::pow (qMax (d, 0.01), 3.);
                  sum += w * points[i].muf;
                  weights += w;
                }
              double const muf = sum / weights;
              QColor const c = mhz_colour (muf, 35.);
              int const a = qBound (0, int (220 * (1. - (nearest - 0.25) / 0.35)), 200);
              img.setPixel (x, y, qPremultiply (qRgba (c.red (), c.green (), c.blue (), a)));
            }
        }
      muf_predicted_ = false;
      map_->set_conditions (img.scaled (720, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), tr ("MUF now · prop.kc2g.com (KC2G)"), 35.);
      update_status ();
    });
}

// headlines from ARRL, AMSAT and DX-World, once an hour while News is on
void ConditionsWindow::fetch_news ()
{
  if (!news_timer_)
    {
      news_timer_ = new QTimer {this};
      news_timer_->setInterval (60 * 60 * 1000);
      connect (news_timer_, &QTimer::timeout, this, &ConditionsWindow::fetch_news);
    }
  if (!news_box_->isChecked ())
    {
      news_timer_->stop ();
      return;
    }
  news_timer_->start ();
  struct Feed {QString source; QString url;};
  QVector<Feed> const feeds {
    {"ARRL", news_base ("https://www.arrl.org/news/rss", "arrl")},
    {"AMSAT", news_base ("https://www.amsat.org/feed/", "amsat")},
    {"DX-World", news_base ("https://www.dx-world.net/feed/", "dxworld")},
  };
  auto * got = new QMap<int, QVector<NewsTicker::Item>>;
  auto * mine = new QVector<NewsTicker::Item>;
  auto * left = new int {int (feeds.size ()) + 1};
  // when every request is back: the program's own messages first, then
  // the headlines taking turns, one from each source
  auto finish = [this, got, mine, left] {
    if (--*left) return;
    QVector<NewsTicker::Item> all {*mine};
    for (int i = 0; i < 6; ++i)
      {
        for (auto it = got->cbegin (); it != got->cend (); ++it) if (i < it->size ()) all << it.value ()[i];
      }
    news_->set_items (all);
    if (all.isEmpty ()) news_->set_message (tr ("The headlines couldn't be reached. Will try again in an hour."));
    delete got;
    delete mine;
    delete left;
  };
  // messages from the program's author: a plain text file kept on the
  // "news" branch of the GitHub repository, one message per line, with an
  // optional link after a "|"; lines starting with # are notes
  {
    QNetworkRequest req {QUrl {news_base ("https://raw.githubusercontent.com/ccoo26/KF0KYL-Digi/news/news.txt", "own")}};
    req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
#if QT_VERSION >= QT_VERSION_CHECK (5, 9, 0)
    req.setAttribute (QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
    auto * reply = network_->get (req);
    connect (reply, &QNetworkReply::finished, this, [reply, mine, finish] {
        reply->deleteLater ();
        if (QNetworkReply::NoError == reply->error ())
          {
            for (auto const& raw : QString::fromUtf8 (reply->readAll ()).split ('\n'))
              {
                auto const line = raw.trimmed ();
                if (line.isEmpty () || line.startsWith ('#')) continue;
                int const bar = line.lastIndexOf ('|');
                NewsTicker::Item item {QString {"KF%1KYL Digi"}.arg (QChar {0x00D8}), line.simplified (), {}};
                if (bar > 0 && line.mid (bar + 1).trimmed ().startsWith ("http"))
                  {
                    item.title = line.left (bar).simplified ();
                    item.link = line.mid (bar + 1).trimmed ();
                  }
                if (item.title.size () && mine->size () < 10) *mine << item;
              }
          }
        finish ();
      });
  }
  for (int f = 0; f < feeds.size (); ++f)
    {
      QNetworkRequest req {QUrl {feeds[f].url}};
      req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
#if QT_VERSION >= QT_VERSION_CHECK (5, 9, 0)
      req.setAttribute (QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
      auto * reply = network_->get (req);
      auto const source = feeds[f].source;
      connect (reply, &QNetworkReply::finished, this, [reply, source, f, got, finish] {
          reply->deleteLater ();
          if (QNetworkReply::NoError == reply->error ())
            {
              // RSS items or Atom entries: the newest few
              QXmlStreamReader xml {reply->readAll ()};
              QVector<NewsTicker::Item> items;
              NewsTicker::Item item;
              bool in_item = false;
              while (!xml.atEnd () && items.size () < 6)
                {
                  xml.readNext ();
                  if (xml.isStartElement ())
                    {
                      auto const name = xml.name ().toString ();
                      if ("item" == name || "entry" == name) {in_item = true; item = NewsTicker::Item {source, {}, {}};}
                      else if (in_item && "title" == name) item.title = xml.readElementText ().simplified ();
                      else if (in_item && "link" == name)
                        {
                          auto const href = xml.attributes ().value ("href").toString ();
                          item.link = href.size () ? href : xml.readElementText ().trimmed ();
                        }
                    }
                  else if (xml.isEndElement () && ("item" == xml.name () || "entry" == xml.name ()))
                    {
                      in_item = false;
                      if (item.title.size ()) items << item;
                    }
                }
              (*got)[f] = items;
            }
          finish ();
        });
    }
}

// the band outlook in words when there's room, coloured dots when not
void ConditionsWindow::fit_outlook ()
{
  auto * outlook = boxes_.value ("bands");
  if (!outlook || !map_bar_ || !row1_ || !row2_) return;
  // what the menus beside it take
  int used = 0;
  for (auto * w : {static_cast<QWidget *> (style_button_), static_cast<QWidget *> (projection_button_), static_cast<QWidget *> (grid_button_)})
    {
      used += w->sizeHint ().width () + 8;
    }
  // the outlook's width in words and in dots, measured while showing
  if (compact_outlook_) outlook_dots_width_ = outlook->sizeHint ().width ();
  else outlook_words_width_ = outlook->sizeHint ().width ();
  int const room = map_bar_->width () - used - 16;
  int const dots = outlook_dots_width_ ? outlook_dots_width_ : outlook_words_width_ * 3 / 5;
  // words in the top row if they fit, else dots, else dots down among the buttons
  bool const compact = outlook_words_width_ > room;
  bool const below = compact && dots > room;
  bool const was_below = row2_->indexOf (outlook) >= 0;
  if (below != was_below)
    {
      if (below)
        {
          row1_->removeWidget (outlook);
          static_cast<FlowLayout *> (row2_)->insert_widget (row2_->count () - 1, outlook);
        }
      else
        {
          row2_->removeWidget (outlook);
          row1_->addWidget (outlook);
        }
      outlook->show ();
    }
  if (compact == compact_outlook_) return;
  compact_outlook_ = compact;
  update_bands ();
}

void ConditionsWindow::resizeEvent (QResizeEvent * e)
{
  QWidget::resizeEvent (e);
  fit_outlook ();
}


void ConditionsWindow::apply_colors ()
{
  auto tok = [] (char const * name) {return Win11Theme::token (name);};
  QColor const accent {tok ("accent")};
  double base = QApplication::font ().pointSizeF ();
  if (base <= 0) base = 9.;
  setStyleSheet (QString {R"(
QWidget#ConditionsWindow {background-color: %1;}
QWidget#plain {background: transparent;}
QScrollArea#plainScroll {background: transparent; border: none;}
QFrame#callPopupOuter {background: transparent;}
QFrame#conditionsTile, QFrame#conditionsBox {background-color: %2; border: 1px solid %3; border-radius: 16px;}
QLabel#boxTitle {color: %13; font-weight: 600; font-size: %9pt;}
QLabel#muted {color: %4;}
QToolButton#chip {background: transparent; color: %4; border: 1px solid %3; border-radius: 13px; padding: 3px 11px; font-weight: normal;}
QToolButton#chip:hover {border-color: %5; color: %6;}
QToolButton#chip:checked {background: %7; color: %6; border: 1px solid transparent; font-weight: normal;}
QToolButton#chip:disabled {color: %10; border-color: %3; background: transparent;}
QToolButton#chip[popupMode="1"] {padding-right: 25px;}
QToolButton#chip::menu-button {background: transparent; border: none; border-left: 1px solid %3; width: 20px;
                                border-top-right-radius: 13px; border-bottom-right-radius: 13px;}
QToolButton#chip::menu-button:hover {background: %8;}
QToolButton#chip::menu-arrow {image: url(:/win11/down_@theme.png); width: 9px; height: 9px;}
QFrame#seg {background: %8; border: 1px solid %3; border-radius: 14px;}
QPushButton#segButton {background: transparent; color: %4; border: none; border-radius: 11px; padding: 3px 11px; font-weight: normal;}
QPushButton#segButton:hover {color: %6;}
QPushButton#segButton:checked {background: %5; color: %11; font-weight: 600;}
QProgressBar#beaconBar {background: %8; border: none; border-radius: 2px;}
QProgressBar#beaconBar::chunk {background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %5, stop:1 #7C5CFF); border-radius: 2px;}
QFrame#callPopup {background-color: %2; border: 1px solid %12; border-radius: 16px;}
QToolButton#swatch {border: 2px solid transparent; border-radius: 8px;}
QToolButton#swatch:hover {border: 2px solid %6;}
QToolButton#pictureTile {background: %8; color: %6; border: 1px solid %3; border-radius: 12px; padding: 6px;}
QToolButton#pictureTile:checked {border: 2px solid %5; background: %8;}
QToolButton#pictureTile:hover {border-color: %5;}
QToolButton#doneButton {background: %5; color: %11; border: none; border-radius: 13px; padding: 3px 14px; font-weight: 600;}
)"}
                 .arg (tok ("bg"), tok ("card"), tok ("border"), tok ("text_secondary"), accent.name (), tok ("text"))
                 .arg (QString {"rgba(%1, %2, %3, 46)"}.arg (accent.red ()).arg (accent.green ()).arg (accent.blue ()))
                 .arg (tok ("ctrl"))
                 .arg (base * 0.82, 0, 'f', 1)
                 .arg (Win11Theme::isDark () ? "#5A6170" : "#A6ADB9")
                 .arg (tok ("accent_text"))
                 .arg (tok ("border_strong"))
                 .arg (tok ("title"))
                 .replace ("@theme", Win11Theme::isDark () ? "dark" : "light"));
  utc_date_->setStyleSheet (QString {});
  // the font may have changed size: give each picker button room for its text
  for (auto * b : findChildren<QPushButton *> ("segButton"))
    {
      auto bold = QApplication::font ();
      bold.setBold (true);
      b->setMinimumWidth (QFontMetrics {bold}.boundingRect (b->text ()).width () + 28);
    }
  map_->retint ();
  apply_call_colors ();
  update_legend ();
  update_tiles ();
  update_bands ();
  update_de_dx ();
  xray_graph_->update ();
}

void ConditionsWindow::scheme_changed ()
{
  // called by the main window after View > Color Scheme changes
  apply_colors ();
}

void ConditionsWindow::showEvent (QShowEvent * e)
{
  QWidget::showEvent (e);
  apply_colors ();                     // the program's font and colours are settled by now
  schedule_area ();
  clock_timer_->start ();
  refresh_timer_->start ();
  refresh_overlays ();
  tick ();
  map_->update_night ();
  update_containers ();
  // fetch now unless the numbers are recent
  if (!updated_.isValid () || updated_.secsTo (QDateTime::currentDateTimeUtc ()) > 10 * 60) refresh ();
  if (box_on ("sun")) fetch_sun ();
}

void ConditionsWindow::hideEvent (QHideEvent * e)
{
  // nothing to fetch or draw while it's closed
  clock_timer_->stop ();
  refresh_timer_->stop ();
  overlay_timer_->stop ();
  sun_timer_->stop ();
  QWidget::hideEvent (e);
}

void ConditionsWindow::closeEvent (QCloseEvent * e)
{
  settings_->beginGroup ("Conditions");
  settings_->setValue ("geometry", saveGeometry ());
  settings_->endGroup ();
  QWidget::closeEvent (e);
}

// the widest the call, clock and date can ever be at the current text size
void ConditionsWindow::fix_clock_width ()
{
  if (!clock_column_) return;
  double base = QApplication::font ().pointSizeF ();
  if (base <= 0) base = 9.;
  auto const key = QString {"%1|%2|%3|%4"}.arg (base).arg (QApplication::font ().family ())
    .arg (utc_date_->font ().toString ()).arg (call_label_->sizeHint ().width ()) + de_info_->text ();
  if (key == clock_column_key_) return;
  clock_column_key_ = key;

  auto widest_digit = [] (QFontMetricsF const& fm) {
    double w = 0;
    for (QChar d = '0'; d <= '9'; d = QChar {d.unicode () + 1}) w = qMax (w, fm.horizontalAdvance (d));
    return w;
  };
  QFont big = QApplication::font ();
  big.setPointSizeF (base * 3.6);
  big.setWeight (QFont::Light);
  QFontMetricsF const big_fm {big};
  QFont small = QApplication::font ();
  small.setPointSizeF (base * 1.3);
  QFontMetricsF const small_fm {small};
  double const clock_w = 6 * widest_digit (big_fm) + 2 * big_fm.horizontalAdvance (':')
    + small_fm.horizontalAdvance (QString {QChar {0x00A0}} + "UTC");

  // every day and month name, with the widest digits for the date and local time
  QFontMetricsF const date_fm {utc_date_->font ()};
  QString const nn {2, QChar {'0'}};
  double date_w = 0;
  QDate const sample {2026, 1, 28};
  for (int m = 0; m < 12; ++m)
    {
      for (int d = 0; d < 7; ++d)
        {
          auto const day = sample.addMonths (m).addDays (d);
          auto const text = tr ("%1 · Local %2").arg (QLocale {}.toString (day, "ddd d MMM yyyy"))
            .arg (QTime {12, 59}.toString ("h:mm AP"));
          date_w = qMax (date_w, date_fm.horizontalAdvance (text));
        }
    }
  date_w += 2 * (widest_digit (date_fm) - date_fm.horizontalAdvance ('1')) + 2 * widest_digit (date_fm);

  int const width = qCeil (qMax (qMax (qMax (clock_w, date_w), double (call_label_->sizeHint ().width ())),
                                 double (de_info_->sizeHint ().width ()))) + 8;
  clock_column_->setFixedWidth (width);
}

// the Sun's bearing and height from you, and its next rise or set
void ConditionsWindow::update_sun_corners (QDateTime const& now)
{
  double lat, lon;
  if (!sun_image_ || !grid_to_latlon (my_grid_, lat, lon))
    {
      if (sun_image_) sun_image_->set_corners ({});
      return;
    }
  double slat, slon, dist, bearing;
  subsolar_point (now, slat, slon);
  path_to (lat, lon, slat, slon, dist, bearing);
  double const el = sun_elevation (now, lat, lon);
  QDateTime rise, set;
  sun_times (lat, lon, rise, set);
  // the next of the two still to come; tomorrow's when both have passed
  QString next;
  if (rise.isValid () && rise > now && (!set.isValid () || set < now || rise < set)) next = tr ("Rise %1").arg (rise.toString ("hh:mm"));
  else if (set.isValid () && set > now) next = tr ("Set %1").arg (set.toString ("hh:mm"));
  else if (rise.isValid ()) next = tr ("Rise %1").arg (rise.toString ("hh:mm"));
  sun_image_->set_corners ({tr ("Az %1%2").arg (qRound (bearing)).arg (QChar {0x00B0}),
                            tr ("El %1%2").arg (qRound (el)).arg (QChar {0x00B0}),
                            next, {}});
}

void ConditionsWindow::tick ()
{
  auto const now = QDateTime::currentDateTimeUtc ();
  double base = QApplication::font ().pointSizeF ();
  if (base <= 0) base = 9.;
  fix_clock_width ();
  auto const muted = Win11Theme::token ("text_secondary");
  utc_clock_->setText (QString {"<span style=\"font-size:%1pt; font-weight:300;\">%2</span>"
                                "<span style=\"font-size:%1pt; font-weight:300; color:%3;\">:%4</span>"
                                "<span style=\"font-size:%5pt; color:%3;\">&nbsp;UTC</span>"}
                       .arg (base * 3.6, 0, 'f', 1).arg (now.toString ("hh:mm")).arg (muted)
                       .arg (now.toString ("ss")).arg (base * 1.3, 0, 'f', 1));
  utc_date_->setText (tr ("%1 · Local %2").arg (QLocale {}.toString (now.date (), "ddd d MMM yyyy"))
                      .arg (now.toLocalTime ().toString ("h:mm AP")));
  update_sun_corners (now);
  update_beacons ();
  if (0 == now.time ().second ())
    {
      map_->update_night ();
      // forget stations not heard for half an hour
      for (auto it = map_->stations.begin (); it != map_->stations.end ();)
        {
          if (it->heard.secsTo (now) > 1800) it = map_->stations.erase (it);
          else ++it;
        }
      update_de_dx ();
      xray_graph_->update ();
      if (0 == now.time ().minute ()) schedule_voacap ();      // a new hour on the chart
      else voacap_grid_->update ();
    }
}

void ConditionsWindow::update_beacons ()
{
  auto const now = QDateTime::currentDateTimeUtc ();
  int const msecs = QTime {0, 0}.msecsTo (now.time ());
  int const secs = msecs / 1000;
  int const slot = (secs % 180) / 10;
  auto const muted = Win11Theme::token ("text_secondary");
  // a narrow box: band and call; where each one is, when you point at it
  QString rows {"<table cellspacing=\"0\" cellpadding=\"2\">"};
  QStringList places;
  for (int b = 0; b < 5; ++b)
    {
      auto const& beacon = ncdxf[beacon_on (slot, b)];
      rows += QString {"<tr><td><span style=\"color:%1\">&#9679;</span>&nbsp;</td><td>%2&nbsp;&nbsp;</td>"
                       "<td><b>%3</b></td></tr>"}
        .arg (beacon_color (b).name ()).arg (beacon_freqs[b]).arg (slashed (beacon.call));
      places << QString {"%1 &nbsp;<b>%2</b> &nbsp;%3"}.arg (beacon_freqs[b]).arg (slashed (beacon.call))
        .arg (QString {beacon.place}.toHtmlEscaped ());
    }
  Q_UNUSED (muted);
  beacon_rows_[0]->setText (rows + "</table>");
  beacon_rows_[0]->setToolTip (places.join ("<br/>"));
  beacon_bar_->setValue ((msecs % 10000) / 100);
  beacon_next_->setText (tr ("Next in %1 s").arg (10 - secs % 10));
  if (slot != beacon_slot_)
    {
      beacon_slot_ = slot;
      for (int i = 0; i < map_->beacons.size (); ++i) map_->beacons[i].band = -1;
      for (int b = 0; b < 5; ++b) map_->beacons[beacon_on (slot, b)].band = b;
      if (map_->show_beacons) map_->update ();
    }
}

QString ConditionsWindow::sun_choice () const
{
  auto * a = sun_pictures_ ? sun_pictures_->checkedAction () : nullptr;
  return a ? a->data ().toString () : QString {"0193"};
}

namespace
{
  QString sun_url (QString const& channel)
  {
    return QString {"%1assets/img/latest/latest_1024_%2.jpg"}.arg (sdo_base (), channel);
  }
}

// one picture: from the last 15 minutes' copy or fetched fresh
void ConditionsWindow::show_sun_channel (QString const& channel)
{
  sun_channel_ = channel;
  sun_image_->set_caption (QString {});
  auto const cached = sun_cache_.value (channel);
  if (!cached.second.isNull () && cached.first.secsTo (QDateTime::currentDateTimeUtc ()) < 14 * 60)
    {
      sun_image_->set_image (cached.second);
      return;
    }
  QNetworkRequest req {QUrl {sun_url (channel)}};
  req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
#if QT_VERSION >= QT_VERSION_CHECK (5, 9, 0)
  req.setAttribute (QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
  auto * reply = network_->get (req);
  connect (reply, &QNetworkReply::finished, this, [this, reply, channel] {
      reply->deleteLater ();
      QImage img;
      if (QNetworkReply::NoError == reply->error ()) img.loadFromData (reply->readAll ());
      if (!img.isNull ()) sun_cache_[channel] = qMakePair (QDateTime::currentDateTimeUtc (), img);
      if (channel != sun_channel_ || sun_movie_action_->isChecked ()) return;   // changed while loading
      if (img.isNull ())
        {
          sun_image_->set_image (QImage {});
          sun_image_->set_message (tr ("The Sun picture couldn't be reached. Will try again in 15 minutes."));
          return;
        }
      sun_image_->set_image (img);
      sun_image_->setToolTip (tr ("The Sun from NASA's Solar Dynamics Observatory, fetched %1 UTC. "
                                  "Bright spots in the ultraviolet views are active regions that can flare.")
                              .arg (QDateTime::currentDateTimeUtc ().toString ("hh:mm")));
    });
}

void ConditionsWindow::fetch_sun ()
{
  if (!box_on ("sun")) return;
  if (sun_movie_action_ && sun_movie_action_->isChecked ())
    {
      load_sun_movie ();
    }
  else
    {
      auto choice = sun_choice ();
      if ("rotate" == choice) choice = sun_channel_.size () ? sun_channel_ : QString {"0193"};
      if (sun_image_->toolTip ().isEmpty () || sun_channel_ != choice) sun_image_->set_message (tr ("Loading..."));
      sun_cache_.remove (choice);
      show_sun_channel (choice);
    }
  if (isVisible ()) sun_timer_->start ();
}

void ConditionsWindow::stop_sun_movie ()
{
  sun_movie_timer_->stop ();
  sun_frames_.clear ();
  sun_frames_channel_.clear ();
  sun_image_->set_caption (QString {});
}

// the last two days, about one picture an hour, from SDO's browse pages
void ConditionsWindow::load_sun_movie ()
{
  auto channel = sun_choice ();
  if ("rotate" == channel) channel = "0193";
  sun_rotate_->stop ();
  if (sun_frames_channel_ == channel && sun_frames_.size ()
      && sun_frames_.last ().first.secsTo (QDateTime::currentDateTimeUtc ()) < 90 * 60)
    {
      if (!sun_movie_timer_->isActive ()) sun_movie_timer_->start ();
      return;
    }
  sun_movie_timer_->stop ();
  sun_frames_.clear ();
  sun_frames_channel_ = channel;
  sun_frame_ = 0;
  sun_image_->set_image (QImage {});
  sun_image_->set_message (tr ("Loading the last 48 hours..."));
  auto const now = QDateTime::currentDateTimeUtc ();
  auto * names = new QMap<QDateTime, QString>;
  auto * pending = new int {3};
  for (int d = 0; d < 3; ++d)
    {
      auto const day = now.date ().addDays (-d);
      auto const dir = QString {"%1assets/img/browse/%2/"}.arg (sdo_base (), day.toString ("yyyy/MM/dd"));
      QNetworkRequest req {QUrl {dir}};
      req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
      auto * reply = network_->get (req);
      connect (reply, &QNetworkReply::finished, this, [this, reply, dir, channel, names, pending, now] {
          reply->deleteLater ();
          if (QNetworkReply::NoError == reply->error ())
            {
              QRegularExpression const re {QString {"(\\d{8}_\\d{6})_512_%1\\.jpg"}.arg (channel)};
              auto it = re.globalMatch (QString::fromLatin1 (reply->readAll ()));
              while (it.hasNext ())
                {
                  auto const m = it.next ();
                  auto t = QDateTime::fromString (m.captured (1), "yyyyMMdd_hhmmss");
                  t.setTimeSpec (Qt::UTC);
                  if (t.isValid () && t.secsTo (now) <= 48 * 3600) names->insert (t, dir + m.captured (0));
                }
            }
          if (--*pending) return;
          // about one an hour
          QVector<QPair<QDateTime, QString>> picks;
          QDateTime last;
          for (auto i = names->cbegin (); i != names->cend (); ++i)
            {
              if (!last.isValid () || last.secsTo (i.key ()) >= 55 * 60)
                {
                  picks << qMakePair (i.key (), i.value ());
                  last = i.key ();
                }
            }
          delete names;
          delete pending;
          if (channel != sun_frames_channel_ || !sun_movie_action_->isChecked ()) return;
          if (picks.isEmpty ())
            {
              sun_image_->set_message (tr ("The movie couldn't be loaded. Will try again in 15 minutes."));
              sun_frames_channel_.clear ();
              return;
            }
          sun_frames_wanted_ = picks.size ();
          auto * got = new QMap<QDateTime, QImage>;
          auto * left = new int {int (picks.size ())};
          for (auto const& pick : picks)
            {
              QNetworkRequest r {QUrl {pick.second}};
              r.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
              auto * frame = network_->get (r);
              auto const when = pick.first;
              connect (frame, &QNetworkReply::finished, this, [this, frame, when, got, left, channel] {
                  frame->deleteLater ();
                  QImage img;
                  if (QNetworkReply::NoError == frame->error ()) img.loadFromData (frame->readAll ());
                  if (!img.isNull ()) got->insert (when, img);
                  bool const done = 0 == --*left;
                  if (channel == sun_frames_channel_ && sun_movie_action_->isChecked () && !done)
                    {
                      sun_image_->set_message (tr ("Loading the last 48 hours... %1%")
                                               .arg (100 * (sun_frames_wanted_ - *left) / qMax (1, sun_frames_wanted_)));
                    }
                  if (!done) return;
                  if (channel == sun_frames_channel_ && sun_movie_action_->isChecked ())
                    {
                      for (auto i = got->cbegin (); i != got->cend (); ++i) sun_frames_ << qMakePair (i.key (), i.value ());
                      if (sun_frames_.isEmpty ()) sun_image_->set_message (tr ("The movie couldn't be loaded. Will try again in 15 minutes."));
                      else sun_movie_timer_->start ();
                    }
                  delete got;
                  delete left;
                });
            }
        });
    }
}

// when you and the DX station are both in the gray line
void ConditionsWindow::gray_line_tool ()
{
  if (gray_line_)
    {
      gray_line_->raise ();
      gray_line_->activateWindow ();
      return;
    }
  auto * dialog = new QDialog {this};
  gray_line_ = dialog;
  dialog->setAttribute (Qt::WA_DeleteOnClose);
  connect (dialog, &QObject::destroyed, this, [this] {gray_line_ = nullptr;});
  dialog->setWindowTitle (tr ("Gray line: you and DX"));
  auto * v = new QVBoxLayout {dialog};
  auto * intro = new QLabel {tr ("When you and the DX station are both in twilight, the low bands can open along the gray line. "
                                 "The highlighted times are when you're both in it, over the next 24 hours."), dialog};
  intro->setWordWrap (true);
  v->addWidget (intro);
  auto * chart = new GrayLineChart {dialog};
  v->addWidget (chart, 1);
  auto * key = new QLabel {dialog};
  key->setTextFormat (Qt::RichText);
  v->addWidget (key);
  auto * row = new QHBoxLayout;
  row->addWidget (new QLabel {tr ("DX grid:"), dialog});
  auto * grid = new QLineEdit {dialog};
  grid->setMaxLength (8);
  grid->setPlaceholderText (tr ("e.g. PM95"));
  grid->setFixedWidth (110);
  grid->setText (dx_grid_);
  row->addWidget (grid);
  auto * from_dx = new QLabel {tr ("(from the DX box until you type one)"), dialog};
  from_dx->setObjectName ("muted");
  row->addWidget (from_dx);
  row->addStretch ();
  auto * close = new QPushButton {tr ("Close"), dialog};
  connect (close, &QPushButton::clicked, dialog, &QDialog::close);
  row->addWidget (close);
  v->addLayout (row);

  auto refresh = [this, chart, key, grid] {
    auto const now = QDateTime::currentDateTimeUtc ();
    chart->start = QDateTime {now.date (), QTime {now.time ().hour (), now.time ().minute () / 5 * 5}, Qt::UTC};
    chart->de = GrayLineChart::Place {};
    chart->de.name = my_call_;
    chart->de.grid = my_grid_.left (6).toUpper ();
    grid_to_latlon (my_grid_, chart->de.lat, chart->de.lon);
    auto const g = grid->text ().trimmed ().toUpper ();
    chart->dx = GrayLineChart::Place {};
    chart->dx.name = g == dx_grid_.toUpper () ? dx_call_ : QString {};
    chart->dx.grid = g;
    if (!grid_to_latlon (g, chart->dx.lat, chart->dx.lon)) chart->dx.lat = chart->dx.lon = qQNaN ();
    chart->update ();
    auto const muted = Win11Theme::token ("text_secondary");
    auto const sw = [] (char const * c) {return QString {"<span style=\"background:%1\">&nbsp;&nbsp;&nbsp;&nbsp;</span>"}.arg (c);};
    QString text = sw ("#5C5230") + " " + tr ("Day") + " &nbsp; " + sw ("#FF9F43") + " " + tr ("Twilight") + " &nbsp; "
      + sw ("#16263D") + " " + tr ("Night") + " &nbsp;&nbsp;&nbsp; ";
    if (qIsNaN (chart->de.lat)) text += QString {"<span style=\"color:%1\">%2</span>"}.arg (muted, tr ("Put your grid in Settings > General."));
    else if (qIsNaN (chart->dx.lat)) text += QString {"<span style=\"color:%1\">%2</span>"}.arg (muted, tr ("Type the DX station's grid, or pick a station."));
    else
      {
        QStringList times;
        for (auto const& r : chart->both ())
          {
            times << QString {"<b>%1–%2</b>"}.arg (r.first.toString ("hh:mm"), r.second.toString ("hh:mm"))
              + QString {" <span style=\"color:%1\">(%2 local)</span>"}.arg (muted, r.first.toLocalTime ().toString ("h:mm AP"));
          }
        text += times.isEmpty () ? tr ("Not both in twilight at once in the next 24 hours.")
                                 : tr ("Both in twilight: %1 UTC").arg (times.join (", "));
      }
    key->setText (text);
  };
  connect (grid, &QLineEdit::textChanged, dialog, refresh);
  auto * timer = new QTimer {dialog};
  timer->setInterval (60000);
  connect (timer, &QTimer::timeout, dialog, refresh);
  connect (this, &ConditionsWindow::dx_changed_for_tools, dialog, [grid, this] {grid->setText (dx_grid_);});
  timer->start ();
  refresh ();
  dialog->resize (720, 300);
  dialog->show ();
}

void ConditionsWindow::refresh ()
{
  if (pending_ > 0) return;
  failed_ = 0;
  auto const swpc = swpc_base ();
  request (swpc + "products/summary/10cm-flux.json", &ConditionsWindow::on_flux);
  request (swpc + "products/noaa-planetary-k-index.json", &ConditionsWindow::on_kindex);
  request (swpc + "products/summary/solar-wind-speed.json", &ConditionsWindow::on_wind);
  request (swpc + "products/summary/solar-wind-mag-field.json", &ConditionsWindow::on_mag);
  request (swpc + "json/goes/primary/xrays-1-day.json", &ConditionsWindow::on_xray);
  request (swpc + "text/daily-solar-indices.txt", &ConditionsWindow::on_daily);
  status_->setText (tr ("Getting the latest space weather from NOAA..."));
}

void ConditionsWindow::request (QString const& url, void (ConditionsWindow::*handler) (QByteArray const&))
{
  QNetworkRequest req {QUrl {url}};
  req.setRawHeader ("User-Agent", http_user_agent ().toUtf8 ());
#if QT_VERSION >= QT_VERSION_CHECK (5, 9, 0)
  req.setAttribute (QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
  ++pending_;
  auto * reply = network_->get (req);
  connect (reply, &QNetworkReply::finished, this, [this, reply, handler] {
      reply->deleteLater ();
      --pending_;
      if (QNetworkReply::NoError == reply->error ())
        {
          (this->*handler) (reply->readAll ());
        }
      else
        {
          ++failed_;
        }
      if (0 == pending_)
        {
          updated_ = QDateTime::currentDateTimeUtc ();
          update_tiles ();
          update_bands ();
          schedule_voacap ();             // the solar flux sets VOACAP's sunspot number
          update_status ();
        }
    });
}

// the credits and anything that couldn't be reached, under the map; kept
// in step with what's switched on
void ConditionsWindow::update_status ()
{
  if (!updated_.isValid ()) return;
  QString text = tr ("Space weather and aurora: NOAA Space Weather Prediction Center, updated %1 UTC. "
                     "Sun: NASA SDO. Maps: NASA Blue Marble and Black Marble, NASA GIBS when zoomed, Natural Earth; rain and snow: NASA GIBS.")
    .arg (updated_.toString ("hh:mm"));
  if (map_->colour_by == 1) text += ' ' + tr ("D-region absorption: NOAA SWPC D-RAP.");
  if (map_->colour_by == 2) text += ' ' + (muf_predicted_
                                           ? tr ("MUF: predicted from you by VOACAP, as prop.kc2g.com couldn't be reached.")
                                           : tr ("MUF: prop.kc2g.com, by Andrew Rodland, KC2G."));
  if (ConditionsMap::CQZones == map_->grid || ConditionsMap::ITUZones == map_->grid) text += ' ' + tr ("Zones: HB9HIL (MIT licence).");
  if (overlay_status_.size ()) text += ' ' + overlay_status_;
  if (failed_) text += ' ' + tr ("(%n item(s) couldn't be reached; will try again in 15 minutes)", "", failed_);
  status_->setText (text);
}

void ConditionsWindow::on_flux (QByteArray const& data)
{
  auto const recs = records (data);
  if (recs.size ()) sfi_ = field (recs.last (), {"flux"});
}

void ConditionsWindow::on_kindex (QByteArray const& data)
{
  auto const recs = records (data);
  if (recs.isEmpty ()) return;
  kp_ = field (recs.last (), {"Kp", "kp_index"});
  // the A index is the average of the last eight 3-hour values
  double sum = 0;
  int n = 0;
  for (int i = recs.size () - 1; i >= 0 && n < 8; --i)
    {
      double const a = field (recs[i], {"a_running", "ap"});
      if (!qIsNaN (a)) {sum += a; ++n;}
    }
  a_index_ = n ? sum / n : qQNaN ();
}

void ConditionsWindow::on_wind (QByteArray const& data)
{
  auto const recs = records (data);
  if (recs.size ()) wind_ = field (recs.last (), {"proton_speed", "WindSpeed", "speed"});
}

void ConditionsWindow::on_mag (QByteArray const& data)
{
  auto const recs = records (data);
  if (recs.isEmpty ()) return;
  bz_ = field (recs.last (), {"bz_gsm", "Bz"});
  bt_ = field (recs.last (), {"bt", "Bt"});
}


void ConditionsWindow::on_xray (QByteArray const& data)
{
  // a day of one-minute readings in two wavebands
  auto const recs = records (data);
  QVector<QPointF> long_band, short_band;
  for (auto const& r : recs)
    {
      double const flux = field (r, {"flux"});
      if (qIsNaN (flux) || flux <= 0) continue;
      auto const t = QDateTime::fromString (r.value ("time_tag").toString (), Qt::ISODate);
      if (!t.isValid ()) continue;
      QPointF const pt {t.toMSecsSinceEpoch () / 1000., flux};
      auto const energy = r.value ("energy").toString ();
      if ("0.1-0.8nm" == energy) long_band << pt;
      else if ("0.05-0.4nm" == energy) short_band << pt;
    }
  if (long_band.size ())
    {
      xray_ = long_band.last ().y ();
      xray_graph_->long_band = long_band;
      xray_graph_->short_band = short_band;
      xray_graph_->update ();
    }
}

void ConditionsWindow::on_daily (QByteArray const& data)
{
  // last line of the daily table: year month day flux sunspots ...
  auto const lines = QString::fromLatin1 (data).split ('\n');
  for (int i = lines.size () - 1; i >= 0; --i)
    {
      auto const line = lines[i].trimmed ();
      if (line.isEmpty () || !line[0].isDigit ()) continue;
      auto const parts = line.split (QRegularExpression {"\\s+"});
      if (parts.size () > 4)
        {
          bool ok;
          double const ssn = parts[4].toDouble (&ok);
          if (ok && ssn >= 0) ssn_ = ssn;
          if (qIsNaN (sfi_))
            {
              double const f = parts[3].toDouble (&ok);
              if (ok && f > 0) sfi_ = f;
            }
        }
      break;
    }
}

void ConditionsWindow::update_tiles ()
{
  // a reading, with its meaning in the tooltip; K and A always show their
  // colour, X-ray, wind and Bz only when they're a worry
  auto set = [this] (QString const& key, QString const& value, QString const& note, int level, QString const& unit) {
    auto const& t = tiles_[key];
    if (!t.value) return;
    bool const always = "k" == key || "a" == key;
    bool const plain = "sfi" == key || "ssn" == key;
    QString colour = Win11Theme::token ("text");
    if (level >= 0 && !plain && (always || level < 2)) colour = level_color (level).name ();
    double const base = QApplication::font ().pointSizeF () > 0 ? QApplication::font ().pointSizeF () : 9.;
    QString text = QString {"<span style=\"color: %1; font-weight: 600; font-size: %2pt;\">%3</span>"}
      .arg (colour).arg (base * 1.15, 0, 'f', 1).arg (value.toHtmlEscaped ());
    if (unit.size ())
      {
        text += QString {"<span style=\"color: %1; font-size: %2pt;\">&nbsp;%3</span>"}
          .arg (Win11Theme::token ("text_secondary")).arg (base * 0.85, 0, 'f', 1).arg (unit);
      }
    t.value->setText (text);
    t.note->setText (note);
    t.note->setStyleSheet (QString {"QLabel {font-size: %1pt; padding-bottom: 4px;}"}.arg (base * 0.85, 0, 'f', 1));
  };
  QString const none {QChar {0x2014}};
  if (qIsNaN (sfi_)) set ("sfi", none, {}, -1, {});
  else set ("sfi", QString::number (qRound (sfi_)), sfi_ >= 150 ? tr ("high") : sfi_ >= 100 ? tr ("moderate") : tr ("low"),
            sfi_ >= 150 ? 2 : sfi_ >= 90 ? 1 : 0, {});
  if (qIsNaN (ssn_)) set ("ssn", none, {}, -1, {});
  else set ("ssn", QString::number (qRound (ssn_)), {}, -1, {});
  if (qIsNaN (kp_)) set ("k", none, {}, -1, {});
  else set ("k", QString::number (kp_, 'f', kp_ < 10 && std::fmod (kp_, 1.) > 0.01 ? 1 : 0),
            kp_ >= 5 ? tr ("storm") : kp_ >= 4 ? tr ("active") : kp_ >= 3 ? tr ("unsettled") : tr ("quiet"),
            kp_ >= 5 ? 0 : kp_ >= 3 ? 1 : 2, {});
  if (qIsNaN (a_index_)) set ("a", none, {}, -1, {});
  else set ("a", QString::number (qRound (a_index_)),
            a_index_ >= 30 ? tr ("stormy") : a_index_ >= 10 ? tr ("unsettled") : tr ("quiet"),
            a_index_ >= 30 ? 0 : a_index_ >= 10 ? 1 : 2, {});
  if (qIsNaN (xray_)) set ("xray", none, {}, -1, {});
  else
    {
      int level;
      auto const cls = xray_class (xray_, &level);
      set ("xray", cls, level < 2 ? tr ("flare") : tr ("normal"), level, {});
    }
  if (qIsNaN (wind_)) set ("wind", none, {}, -1, {});
  else set ("wind", QString::number (qRound (wind_)), wind_ >= 500 ? tr ("fast") : tr ("normal"),
            wind_ >= 600 ? 0 : wind_ >= 500 ? 1 : 2, tr ("km/s"));
  if (qIsNaN (bz_)) set ("bz", none, {}, -1, {});
  else set ("bz", QString {"%1"}.arg (bz_, 0, 'f', 0).replace ('-', QChar {0x2212}),
            bz_ <= -10 ? tr ("south, storms") : bz_ < 0 ? tr ("south") : tr ("north"),
            bz_ <= -10 ? 0 : bz_ <= -5 ? 1 : 2, tr ("nT"));
  if (spacewx_time_) spacewx_time_->setText (updated_.isValid () ? tr ("NOAA \u00B7 %1 UTC").arg (updated_.toString ("hh:mm")) : QString {});
}

void ConditionsWindow::update_bands ()
{
  QStringList const words {tr ("Poor"), tr ("Fair"), tr ("Good")};
  for (int g = 0; g < 4; ++g)
    {
      for (int dn = 0; dn < 2; ++dn)
        {
          int const level = band_estimate (g, 0 == dn, sfi_, kp_);
          auto * cell = band_cells_[g][dn];
          if (level < 0)
            {
              cell->setText (QString {QChar {0x2014}});
              cell->setStyleSheet (QString {"QLabel {color: %1;}"}.arg (Win11Theme::token ("text_secondary")));
              continue;
            }
          cell->setText (compact_outlook_ ? QString {} : words[level]);
          cell->setToolTip (QString {"%1: %2"}.arg (dn ? tr ("Night") : tr ("Day"), words[level]));
          QColor const c = level_color (level);
          if (compact_outlook_) cell->setStyleSheet (QString {"QLabel {background-color: %1; border-radius: 7px;}"}.arg (c.name ()));
          else cell->setStyleSheet (QString {"QLabel {color: %1; background-color: rgba(%2, %3, %4, 38); border: 1px solid rgba(%2, %3, %4, 80);"
                                             " border-radius: 6px; padding: 0px 5px; font-weight: 600;}"}
                                    .arg (c.name ()).arg (c.red ()).arg (c.green ()).arg (c.blue ()));
          if (compact_outlook_) cell->setFixedSize (14, 14);
          else
            {
              cell->setMinimumSize (0, 0);
              cell->setMaximumSize (QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
              cell->setMinimumWidth (cell->sizeHint ().width ());
            }
        }
    }
  if (auto * outlook = boxes_.value ("bands")) outlook->updateGeometry ();
  QTimer::singleShot (0, this, [this] {fit_outlook ();});
}

void ConditionsWindow::update_de_dx ()
{
  auto const now = QDateTime::currentDateTimeUtc ();
  auto const muted = Win11Theme::token ("text_secondary");
  double base = QApplication::font ().pointSizeF ();
  if (base <= 0) base = 9.;
  auto big = [base] (QString const& s) {
    return QString {"<div style=\"font-size:%1pt; font-weight:700; margin-bottom:4px;\">%2</div>"}.arg (base * 1.7, 0, 'f', 1).arg (s);
  };
  auto row = [muted] (QString const& key, QString const& value) {
    return QString {"<tr><td style=\"color:%1; padding-right:12px;\">%2</td><td>%3</td></tr>"}.arg (muted, key, value);
  };
  auto sun_line = [] (double lat, double lon) {
    QDateTime rise, set;
    sun_times (lat, lon, rise, set);
    return QString {"↑ %1 &nbsp; ↓ %2 UTC"}
      .arg (rise.isValid () ? rise.toString ("hh:mm") : QString {"--:--"})
      .arg (set.isValid () ? set.toString ("hh:mm") : QString {"--:--"});
  };
  QString const table {"<table cellspacing=\"0\" cellpadding=\"1\">"};

  // you, under the clock
  QString de;
  double my_lat, my_lon;
  bool const have_me = grid_to_latlon (my_grid_, my_lat, my_lon);
  if (have_me)
    {
      auto const key = [muted] (QString const& k) {return QString {"<span style=\"color:%1\">%2</span>&nbsp;"}.arg (muted, k);};
      de = key (tr ("Grid")) + my_grid_.left (6).toUpper () + QString {" <span style=\"color:%1\">·</span> "}.arg (muted)
        + latlon_text (my_lat, my_lon) + "<br/>" + key (tr ("Sun")) + sun_line (my_lat, my_lon);
    }
  else
    {
      de = QString {"<span style=\"color:%1\">%2</span>"}.arg (muted, tr ("Put your grid in Settings"));
    }
  de_info_->setText (de);

  // the station being worked
  double dx_lat, dx_lon;
  bool have_dx = dx_call_.size () && grid_to_latlon (dx_grid_, dx_lat, dx_lon);
  bool approx = false;
  if (!have_dx && dx_call_.size () && map_->stations.contains (dx_call_))
    {
      dx_lat = map_->stations[dx_call_].lat;
      dx_lon = map_->stations[dx_call_].lon;
      approx = map_->stations[dx_call_].approx;
      have_dx = true;
    }
  QString dx;
  if (dx_call_.isEmpty ())
    {
      dx = QString {"<span style=\"color:%1\">%2</span>"}.arg (muted, tr ("Nobody yet. Double-click a station in Band Activity or on the map."));
    }
  else
    {
      // the call is in the QRZ details when they're showing
      bool const qrz_shown = call_info_ && !call_info_->isHidden ();
      if (qrz_shown) dx = QString {"<div style=\"height:4px\"></div>"};
      else dx = big (slashed (dx_call_).toHtmlEscaped ());
      if (!have_dx)
        {
          dx += QString {"<div style=\"color:%1\">%2</div>"}.arg (muted, tr ("No grid square yet."));
        }
      else
        {
          dx += table + row (tr ("Grid"), (approx ? tr ("country only") : dx_grid_.left (6).toUpper ()) + " · " + latlon_text (dx_lat, dx_lon));
          // local time from the longitude; time zones don't always follow it
          int const offset = qRound (dx_lon / 15.);
          dx += row (tr ("Local"), "≈ " + now.addSecs (offset * 3600).toString ("h:mm AP"));
          dx += row (tr ("Sun"), sun_line (dx_lat, dx_lon));
          if (have_me)
            {
              double dist, bearing;
              path_to (my_lat, my_lon, dx_lat, dx_lon, dist, bearing);
              double const km = dist * 6371., long_km = 40030. - km;
              auto distance = [this] (double km) {
                return QString {"%1 %2"}.arg (QLocale {}.toString (qRound (miles_ ? km * 0.621371 : km)))
                  .arg (miles_ ? tr ("mi") : tr ("km"));
              };
              dx += row (tr ("Short path"), QString {"%1 · %2%3"}.arg (distance (km)).arg (qRound (bearing)).arg (QChar {0x00B0}));
              dx += row (tr ("Long path"), QString {"%1 · %2%3"}.arg (distance (long_km))
                         .arg (qRound (std::fmod (bearing + 180., 360.))).arg (QChar {0x00B0}));
            }
          dx += "</table>";
        }
    }
  dx_info_->setText (dx);
  fit_dx_box ();
}

void ConditionsWindow::schedule_voacap ()
{
  if (voacap_timer_) voacap_timer_->start ();
  schedule_area ();
}

void ConditionsWindow::schedule_area ()
{
  if (area_timer_ && prop_box_ && prop_box_->isChecked ()) area_timer_->start ();
}

void ConditionsWindow::select_prop_band (int band)
{
  // the same band again takes it off the map
  if (prop_box_->isChecked () && band == prop_band_)
    {
      prop_box_->setChecked (false);
      return;
    }
  prop_band_ = band;
  map_->set_prop (QImage {});
  if (prop_box_->isChecked ())
    {
      voacap_grid_->selected = band;
      voacap_grid_->update ();
      save_map_options ();
      schedule_area ();
    }
  else
    {
      prop_box_->setChecked (true);         // shows it and works it out
    }
}

// the sunspot number VOACAP is given: worked out from today's solar flux
double ConditionsWindow::effective_ssn () const
{
  double ssn = 100.;
  if (!qIsNaN (sfi_))
    {
      double const disc = 0.728 * 0.728 + 4. * 0.00089 * (sfi_ - 63.7);
      ssn = disc > 0 ? (-0.728 + std::sqrt (disc)) / (2. * 0.00089) : 0.;
    }
  else if (!qIsNaN (ssn_)) ssn = ssn_;
  return qBound (0., ssn, 300.);
}

// When prop.kc2g.com can't be reached, the MUF layer shows VOACAP's
// prediction instead: the highest frequency from you to each place,
// worked out for this month, hour and sunspot number
void ConditionsWindow::predicted_muf ()
{
  if (muf_process_ || !colour_by_ || !colour_by_->checkedAction ()
      || 2 != colour_by_->checkedAction ()->data ().toInt ()) return;
  // one VOACAP at a time
  if (area_process_)
    {
      QTimer::singleShot (5000, this, &ConditionsWindow::predicted_muf);
      return;
    }
  double lat, lon;
  if (!grid_to_latlon (my_grid_, lat, lon)) return;
  auto const program = voacap_program ();
  if (program.isEmpty ()) return;
  auto const now = QDateTime::currentDateTimeUtc ();
  int const hour = now.time ().hour () ? now.time ().hour () : 24;
  auto pos = [] (double v, char p, char n) {return QString {"%1%2"}.arg (std::abs (v), 6, 'f', 2).arg (v >= 0 ? p : n);};
  auto nine = [] (QString const& first, QString const& rest) {
    QString s = first;
    for (int i = 0; i < 8; ++i) s += rest;
    return s;
  };
  QStringList const lines {
    "Model    :VOACAP",
    "Colors   :Black    :Blue     :Ignore   :Ignore   :Red      :Black with shading",
    "Cities   :Receive.cty",
    "Nparms   :    1",
    "Parameter:MUF      0",
    QString {"Transmit :%1   %2   %3 %4"}.arg (pos (lat, 'N', 'S')).arg (pos (lon, 'E', 'W')).arg ("DE", -20).arg ("Short"),
    QString {"Pcenter  :%1   %2   %3"}.arg (pos (lat, 'N', 'S')).arg (pos (lon, 'E', 'W')).arg ("DE", -20),
    "Area     :    -180.0     180.0     -90.0      90.0",
    "Gridsize :   91    1",
    "Method   :   30",
    "Coeffs   :CCIR",
    "Months   :" + nine (QString {"%1"}.arg (double (now.date ().month ()), 7, 'f', 2), "   0.00"),
    "Ssns     :" + nine (QString {"%1"}.arg (qRound (effective_ssn ()), 7), "      0"),
    "Hours    :" + nine (QString {"%1"}.arg (hour, 7), "      0"),
    "Freqs    :" + nine (QString {"%1"}.arg (14.074, 7, 'f', 3), "  0.000"),
    "System   :  145     3.000   90    3.000     0.100",
    "Fprob    : 1.00 1.00 1.00 0.00",
    QString {"Rec Ants :[%1]  gain=   0.0   0.0"}.arg ("default /isotrope", -21),
    QString {"Tx Ants  :[%1]  0.000  -1.0 %2"}.arg ("default /isotrope", -21).arg (0.1, 10, 'f', 4),
  };
  auto const root = voacap_root ();
  QDir {root}.mkpath ("areadata/kf0kylmuf");
  QFile input {root + "/areadata/kf0kylmuf/muf.voa"};
  if (!input.open (QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
  input.write (lines.join ('\n').toLatin1 () + '\n');
  input.close ();
  QFile::remove (root + "/areadata/kf0kylmuf/muf.vg1");
  muf_process_ = new QProcess {this};
  muf_process_->setWorkingDirectory (root + "/run");
  connect (muf_process_, static_cast<void (QProcess::*) (int, QProcess::ExitStatus)> (&QProcess::finished), this,
           [this, root] (int, QProcess::ExitStatus) {
             muf_process_->deleteLater ();
             muf_process_ = nullptr;
             on_predicted_muf (root + "/areadata/kf0kylmuf/muf.vg1");
           });
  connect (muf_process_, &QProcess::errorOccurred, this, [this] (QProcess::ProcessError error) {
      if (QProcess::FailedToStart != error || !muf_process_) return;
      muf_process_->deleteLater ();
      muf_process_ = nullptr;
    });
  muf_process_->start (program, {"-s", root, "area", "calc", "kf0kylmuf/muf.voa"});
}

void ConditionsWindow::on_predicted_muf (QString const& path)
{
  if (!colour_by_ || !colour_by_->checkedAction () || 2 != colour_by_->checkedAction ()->data ().toInt ()) return;
  QFile file {path};
  if (!file.open (QIODevice::ReadOnly | QIODevice::Text)) return;
  file.readLine ();
  file.readLine ();
  auto const header = QString::fromLatin1 (file.readLine ());
  int const muf_end = header.indexOf ("   MUF") + 6;
  if (muf_end < 6) return;
  int const N = 91;
  QVector<float> field (N * N, -1.f);
  while (!file.atEnd ())
    {
      auto const line = QString::fromLatin1 (file.readLine ());
      if (line.size () < muf_end) continue;
      bool ok1, ok2, ok3;
      double const lat = line.mid (6, 10).toDouble (&ok1);
      double const lon = line.mid (16, 10).toDouble (&ok2);
      double const muf = line.mid (muf_end - 6, 6).toDouble (&ok3);
      if (!ok1 || !ok2 || !ok3 || muf <= 0.) continue;
      int const x = qBound (0, qRound ((lon + 180.) / 360. * (N - 1)), N - 1);
      int const y = qBound (0, qRound ((90. - lat) / 180. * (N - 1)), N - 1);
      field[y * N + x] = static_cast<float> (muf);
    }
  QImage img {N, N, QImage::Format_ARGB32_Premultiplied};
  int filled = 0;
  for (int y = 0; y < N; ++y)
    {
      for (int x = 0; x < N; ++x)
        {
          double const muf = field[y * N + x];
          if (muf <= 0.) {img.setPixel (x, y, 0); continue;}
          ++filled;
          QColor const c = mhz_colour (muf, 35.);
          img.setPixel (x, y, qPremultiply (qRgba (c.red (), c.green (), c.blue (), 170)));
        }
    }
  if (!filled) return;
  muf_predicted_ = true;
  map_->set_conditions (img.scaled (720, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                        tr ("MUF predicted from you \u00B7 VOACAP"), 35.);
  update_status ();
}

void ConditionsWindow::run_area ()
{
  if (!prop_box_->isChecked () || !isVisible ()) return;
  if (area_process_) {area_again_ = true; return;}
  if (muf_process_) {QTimer::singleShot (5000, this, &ConditionsWindow::schedule_area); return;}
  double lat, lon;
  if (!grid_to_latlon (my_grid_, lat, lon)) return;
  auto const program = voacap_program ();
  if (program.isEmpty ()) return;
  static double const freqs[VoacapGrid::bands] = {3.573, 5.357, 7.074, 10.136, 14.074, 18.100, 21.074, 24.915, 28.074};
  double const ssn = effective_ssn ();
  auto const now = QDateTime::currentDateTimeUtc ();
  int const hour = now.time ().hour () ? now.time ().hour () : 24;
  int const watts = voacap_power_->currentData ().toInt ();
  bool const long_path = 1 == voacap_path_->checkedId ();
  auto pos = [] (double v, char p, char n) {return QString {"%1%2"}.arg (std::abs (v), 6, 'f', 2).arg (v >= 0 ? p : n);};
  auto nine = [] (QString const& first, QString const& rest) {
    QString s = first;
    for (int i = 0; i < 8; ++i) s += rest;
    return s;
  };
  // fixed columns again, as VOACAP's area cards are read
  QStringList const lines {
    "Model    :VOACAP",
    "Colors   :Black    :Blue     :Ignore   :Ignore   :Red      :Black with shading",
    "Cities   :Receive.cty",
    "Nparms   :    1",
    "Parameter:REL      0",
    QString {"Transmit :%1   %2   %3 %4"}.arg (pos (lat, 'N', 'S')).arg (pos (lon, 'E', 'W')).arg ("DE", -20).arg (long_path ? "Long " : "Short"),
    QString {"Pcenter  :%1   %2   %3"}.arg (pos (lat, 'N', 'S')).arg (pos (lon, 'E', 'W')).arg ("DE", -20),
    "Area     :    -180.0     180.0     -90.0      90.0",
    "Gridsize :  181    1",
    "Method   :   30",
    "Coeffs   :CCIR",
    "Months   :" + nine (QString {"%1"}.arg (double (now.date ().month ()), 7, 'f', 2), "   0.00"),
    "Ssns     :" + nine (QString {"%1"}.arg (qRound (ssn), 7), "      0"),
    "Hours    :" + nine (QString {"%1"}.arg (hour, 7), "      0"),
    "Freqs    :" + nine (QString {"%1"}.arg (freqs[prop_band_], 7, 'f', 3), "  0.000"),
    QString {"System   :  145%1   90%2     3.000     0.100"}.arg (voacap_toa_->currentData ().toDouble (), 10, 'f', 3)
      .arg (qRound (voacap_mode_->currentData ().toDouble ()), 5),
    "Fprob    : 1.00 1.00 1.00 0.00",
    QString {"Rec Ants :[%1]  gain=   0.0   0.0"}.arg ("default /isotrope", -21),
    QString {"Tx Ants  :[%1]  0.000  -1.0 %2"}.arg ("default /isotrope", -21).arg (watts / 1000., 10, 'f', 4),
  };
  auto const root = voacap_root ();
  QDir {root}.mkpath ("areadata/kf0kyl");
  QFile input {root + "/areadata/kf0kyl/kf0kyl.voa"};
  if (!input.open (QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
  input.write (lines.join ('\n').toLatin1 () + '\n');
  input.close ();
  QFile::remove (root + "/areadata/kf0kyl/kf0kyl.vg1");
  map_->prop_label = tr ("Working out %1 m\u2026").arg (VoacapGrid::band_name (prop_band_));
  map_->update ();
  int const band = prop_band_;
  QString const label = tr ("%1 m %2 \u00B7 %3 W %4 \u00B7 %5 UTC").arg (VoacapGrid::band_name (band))
    .arg (long_path ? tr ("long path") : tr ("from you")).arg (watts)
    .arg (voacap_mode_->currentText ()).arg (hour % 24, 2, 10, QChar {'0'});
  area_process_ = new QProcess {this};
  area_process_->setWorkingDirectory (root + "/run");
  connect (area_process_, static_cast<void (QProcess::*) (int, QProcess::ExitStatus)> (&QProcess::finished), this,
           [this, root, band, label] (int, QProcess::ExitStatus) {
             area_process_->deleteLater ();
             area_process_ = nullptr;
             if (band == prop_band_ && !area_again_) on_area (root + "/areadata/kf0kyl/kf0kyl.vg1", label);
             if (area_again_) {area_again_ = false; schedule_area ();}
           });
  connect (area_process_, &QProcess::errorOccurred, this, [this] (QProcess::ProcessError error) {
      if (QProcess::FailedToStart != error || !area_process_) return;
      area_process_->deleteLater ();
      area_process_ = nullptr;
    });
  area_process_->start (program, {"-s", root, "area", "calc", "kf0kyl/kf0kyl.voa"});
}

void ConditionsWindow::on_area (QString const& path, QString const& label)
{
  // one line per grid point; latitude, longitude and REL in fixed columns
  QFile file {path};
  if (!file.open (QIODevice::ReadOnly | QIODevice::Text))
    {
      map_->prop_label = tr ("VOACAP couldn't make this map");
      map_->update ();
      return;
    }
  file.readLine ();
  file.readLine ();
  auto const header = QString::fromLatin1 (file.readLine ());
  int const rel_end = header.indexOf ("   REL") + 6;
  if (rel_end < 6) return;
  // VOACAP's numbers on a grid of points...
  int const N = 181;
  QVector<float> field (N * N, 0.f);
  double best = 0.;
  while (!file.atEnd ())
    {
      auto const line = QString::fromLatin1 (file.readLine ());
      if (line.size () < rel_end) continue;
      bool ok1, ok2, ok3;
      double const lat = line.mid (6, 10).toDouble (&ok1);
      double const lon = line.mid (16, 10).toDouble (&ok2);
      double const rel = line.mid (rel_end - 6, 6).toDouble (&ok3);
      if (!ok1 || !ok2 || !ok3) continue;
      int const x = qBound (0, qRound ((lon + 180.) / 360. * (N - 1)), N - 1);
      int const y = qBound (0, qRound ((90. - lat) / 180. * (N - 1)), N - 1);
      field[y * N + x] = static_cast<float> (qBound (0., rel, 1.));
      best = qMax (best, rel);
    }
  // softened a little across neighbouring points, so the steps VOACAP
  // makes between them don't show as jagged edges
  for (int pass = 0; pass < 2; ++pass)
    {
      QVector<float> soft (field.size ());
      for (int y = 0; y < N; ++y)
        {
          for (int x = 0; x < N; ++x)
            {
              double sum = 0, weight = 0;
              for (int dy = -1; dy <= 1; ++dy)
                {
                  for (int dx = -1; dx <= 1; ++dx)
                    {
                      int const yy = y + dy;
                      int const xx = (x + dx + N - 1) % (N - 1);     // round the world east-west
                      if (yy < 0 || yy >= N) continue;
                      double const w = (dx ? 1 : 2) * (dy ? 1 : 2);
                      sum += field[yy * N + xx] * w;
                      weight += w;
                    }
                }
              soft[y * N + x] = static_cast<float> (sum / weight);
            }
        }
      field = soft;
    }
  // ...blended smoothly between the points first and only then coloured,
  // with the weakest areas fading out rather than stopping in a hard edge
  int const W = 1440, H = 720;
  QImage img {W, H, QImage::Format_ARGB32_Premultiplied};
  for (int y = 0; y < H; ++y)
    {
      double const gy = (y + 0.5) / H * (N - 1);
      int const y0 = qMin (static_cast<int> (gy), N - 2);
      double const fy = gy - y0;
      auto * out = reinterpret_cast<QRgb *> (img.scanLine (y));
      for (int x = 0; x < W; ++x)
        {
          double const gx = (x + 0.5) / W * (N - 1);
          int const x0 = qMin (static_cast<int> (gx), N - 2);
          double const fx = gx - x0;
          double const sx = fx, sy = fy;
          double const v = (field[y0 * N + x0] * (1 - sx) + field[y0 * N + x0 + 1] * sx) * (1 - sy)
            + (field[(y0 + 1) * N + x0] * (1 - sx) + field[(y0 + 1) * N + x0 + 1] * sx) * sy;
          double const a = qMin (0.82, 0.30 + 0.52 * v);   // red stays visible where the band is closed
          if (a < 0.004) {out[x] = 0; continue;}
          QColor c = VoacapGrid_colour (v);
          c.setAlphaF (a);
          out[x] = qPremultiply (c.rgba ());
        }
    }
  // say how good the best of it is: long path is often weak everywhere
  map_->prop_label = label + tr (" \u00B7 best %1%").arg (qRound (best * 100));
  map_->set_prop (img);
}

QString ConditionsWindow::voacap_root ()
{
  // a writable copy of VOACAP's data; the engine writes into its run folder
  auto const base = QStandardPaths::writableLocation (QStandardPaths::AppLocalDataLocation);
  QDir root {base + "/voacap/itshfbc"};
  auto const marker = root.filePath ("kf0kyl-data-1");
  if (!QFile::exists (marker))
    {
      QDirIterator it {":/voacap/itshfbc", QDirIterator::Subdirectories};
      while (it.hasNext ())
        {
          auto const from = it.next ();
          if (QFileInfo {from}.isDir ()) continue;
          auto const rel = from.mid (QString {":/voacap/itshfbc/"}.size ());
          auto const to = root.filePath (rel);
          QDir {}.mkpath (QFileInfo {to}.absolutePath ());
          QFile::remove (to);
          QFile::copy (from, to);
          QFile::setPermissions (to, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
        }
      for (auto const& sub : {"run", "areadata", "area_inv"}) root.mkpath (sub);
      QFile m {marker};
      m.open (QIODevice::WriteOnly);
    }
  return root.absolutePath ();
}

QString ConditionsWindow::voacap_program ()
{
  auto const dir = QCoreApplication::applicationDirPath ();
#ifdef Q_OS_WIN
  QString const name {"voacapl.exe"};
#else
  QString const name {"voacapl"};
#endif
  for (auto const& path : {dir + '/' + name, dir + "/contrib/voacapl/" + name, dir + "/../bin/" + name})
    {
      if (QFileInfo {path}.isExecutable ()) return path;
    }
  return {};
}

void ConditionsWindow::run_voacap ()
{
  if (!box_on ("voacap") || !isVisible ()) return;
  if (voacap_process_)
    {
      voacap_again_ = true;            // one already running: go again when it's done
      return;
    }
  voacap_call_->setText (slashed (dx_call_).toHtmlEscaped ());
  double lat1, lon1, lat2, lon2;
  bool const have_me = grid_to_latlon (my_grid_, lat1, lon1);
  bool have_dx = dx_call_.size () && grid_to_latlon (dx_grid_, lat2, lon2);
  if (!have_dx && dx_call_.size () && map_->stations.contains (dx_call_))
    {
      lat2 = map_->stations[dx_call_].lat;
      lon2 = map_->stations[dx_call_].lon;
      have_dx = true;
    }
  auto const muted = Win11Theme::token ("text_secondary");
  auto say = [this, muted] (QString const& text) {
    voacap_grid_->rel.clear ();
    voacap_grid_->snr.clear ();
    voacap_grid_->update ();
    voacap_now_->setText (QString {"<span style=\"color:%1\">%2</span>"}.arg (muted, text));
  };
  if (!have_me) {say (tr ("Put your grid in Settings > General.")); return;}
  if (!have_dx) {say (tr ("Pick a DX station with a grid square to see the path.")); return;}
  auto const program = voacap_program ();
  if (program.isEmpty ()) {say (tr ("The VOACAP engine wasn't found next to the program.")); return;}

  // sunspot number from today's solar flux: SFI = 63.7 + 0.728 R + 0.00089 R^2
  double ssn = 100.;
  if (!qIsNaN (sfi_))
    {
      double const disc = 0.728 * 0.728 + 4. * 0.00089 * (sfi_ - 63.7);
      ssn = disc > 0 ? (-0.728 + std::sqrt (disc)) / (2. * 0.00089) : 0.;
    }
  else if (!qIsNaN (ssn_))
    {
      ssn = ssn_;
    }
  ssn = qBound (0., ssn, 300.);
  auto const now = QDateTime::currentDateTimeUtc ();
  double const kw = voacap_power_->currentData ().toInt () / 1000.;
  double const snr = voacap_mode_->currentData ().toDouble ();
  double const toa = voacap_toa_->currentData ().toDouble ();
  bool const long_path = 1 == voacap_path_->checkedId ();
  // the cards are read in fixed columns, so the spacing matters
  auto latlon = [] (double v, char pos, char neg, int width) {
    return QString {"%1%2"}.arg (std::abs (v), width, 'f', 2).arg (v >= 0 ? pos : neg);
  };
  QStringList lines {
    "LINEMAX      55       number of lines-per-page",
    "COEFFS    CCIR",
    "TIME          1   24    1    1",
    QString {"MONTH     %1%2"}.arg (now.date ().year (), 5).arg (double (now.date ().month ()), 5, 'f', 2),
    QString {"SUNSPOT   %1."}.arg (qRound (ssn), 4),
    "LABEL     DE                  DX",
    QString {"CIRCUIT   %1%2%3%4  %5     0"}.arg (latlon (lat1, 'N', 'S', 5)).arg (latlon (lon1, 'E', 'W', 9))
      .arg (latlon (lat2, 'N', 'S', 9)).arg (latlon (lon2, 'E', 'W', 9)).arg (long_path ? 'L' : 'S'),
    QString {"SYSTEM       1. 145.%1  90.%2 3.00 0.10"}.arg (toa, 5, 'f', 2).arg (snr, 5, 'f', 1),
    "FPROB      1.00 1.00 1.00 0.00",
    QString {"ANTENNA       1    1    2   30     0.000[default/isotrope     ]  0.0%1"}.arg (kw, 10, 'f', 4),
    "ANTENNA       2    2    2   30     0.000[default/isotrope     ]  0.0    0.0000",
    "FREQUENCY  3.57 5.36 7.0710.1414.0718.1021.0724.9228.07 0.00 0.00",
    "METHOD       30    0",
    "EXECUTE",
    "QUIT",
  };
  auto const root = voacap_root ();
  QFile input {root + "/run/kf0kyl.dat"};
  if (!input.open (QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
      say (tr ("VOACAP couldn't write its input file."));
      return;
    }
  input.write (lines.join ('\n').toLatin1 () + '\n');
  input.close ();
  QFile::remove (root + "/run/kf0kyl.out");

  voacap_process_ = new QProcess {this};
  voacap_process_->setWorkingDirectory (root + "/run");
  connect (voacap_process_, static_cast<void (QProcess::*) (int, QProcess::ExitStatus)> (&QProcess::finished), this,
           [this, root] (int, QProcess::ExitStatus) {
             voacap_process_->deleteLater ();
             voacap_process_ = nullptr;
             on_voacap (root + "/run/kf0kyl.out");
             if (voacap_again_) {voacap_again_ = false; schedule_voacap ();}
           });
  connect (voacap_process_, &QProcess::errorOccurred, this, [this] (QProcess::ProcessError error) {
      if (QProcess::FailedToStart != error || !voacap_process_) return;
      voacap_process_->deleteLater ();
      voacap_process_ = nullptr;
      voacap_now_->setText (tr ("VOACAP couldn't be started."));
    });
  voacap_process_->start (program, {"-s", root, "kf0kyl.dat", "kf0kyl.out"});
  voacap_ran_ = now;
}

void ConditionsWindow::on_voacap (QString const& path)
{
  // blocks for each hour: a FREQ line starting with the hour, then a line
  // per value; REL and SNR give a number for the MUF column and then one
  // for each of our nine frequencies
  QFile file {path};
  if (!file.open (QIODevice::ReadOnly | QIODevice::Text))
    {
      voacap_now_->setText (tr ("VOACAP didn't give a prediction for this path."));
      return;
    }
  QVector<QVector<float>> rel (VoacapGrid::bands, QVector<float> (24, 0.f));
  QVector<QVector<float>> snr (VoacapGrid::bands, QVector<float> (24, 0.f));
  int hour = -1;
  int found = 0;
  while (!file.atEnd ())
    {
      auto const line = QString::fromLatin1 (file.readLine ()).trimmed ();
      auto const parts = line.simplified ().split (' ');
      if (parts.size () < 3) continue;
      if (parts.last () == "FREQ")
        {
          bool ok;
          double const h = parts[0].toDouble (&ok);
          hour = ok ? static_cast<int> (std::lround (h)) % 24 : -1;
          continue;
        }
      if (hour < 0) continue;
      bool const is_rel = parts.last () == "REL";
      bool const is_snr = parts.last () == "SNR";
      if (!is_rel && !is_snr) continue;
      for (int b = 0; b < VoacapGrid::bands && b + 1 < parts.size () - 1; ++b)
        {
          bool ok;
          double const v = parts[b + 1].toDouble (&ok);
          if (!ok) continue;
          if (is_rel) rel[b][hour] = static_cast<float> (v);
          else snr[b][hour] = static_cast<float> (v);
        }
      if (is_rel) ++found;
    }
  if (found < 12)
    {
      voacap_grid_->rel.clear ();
      voacap_grid_->update ();
      voacap_now_->setText (tr ("VOACAP didn't give a prediction for this path."));
      return;
    }
  voacap_grid_->rel = rel;
  voacap_grid_->snr = snr;
  voacap_grid_->update ();
  // the best bands this hour
  int const now = QDateTime::currentDateTimeUtc ().time ().hour ();
  QVector<int> order;
  for (int b = 0; b < VoacapGrid::bands; ++b) order << b;
  std::sort (order.begin (), order.end (), [&rel, now] (int a, int b) {return rel[a][now] > rel[b][now];});
  QStringList best;
  for (int i = 0; i < 3; ++i)
    {
      int const b = order[i];
      if (rel[b][now] < 0.05) break;
      auto const text = QString {"%1m %2%"}.arg (VoacapGrid::band_name (b)).arg (qRound (rel[b][now] * 100));
      best << (0 == i ? "<b>" + text + "</b>" : text);
    }
  voacap_now_->setText (best.isEmpty () ? tr ("Now: no band is likely to work this path.")
                        : tr ("Now: %1").arg (best.join (" · ")));
}

void ConditionsWindow::set_my_call (QString const& call)
{
  auto const was = my_call_;
  my_call_ = call.trimmed ().toUpper ();
  if (my_call_ != was && psk_live_action_)
    {
      map_->psk_store.clear ();
      map_->rebuild_psk ();
      update_psk_live ();
    }
  call_label_->text = slashed (my_call_);
  call_label_->updateGeometry ();
  call_label_->update ();
  update_de_dx ();
}

void ConditionsWindow::set_my_grid (QString const& grid)
{
  if (grid == my_grid_) return;
  my_grid_ = grid;
  double lat, lon;
  if (grid_to_latlon (grid, lat, lon)) map_->set_my (lat, lon);
  update_de_dx ();
  schedule_voacap ();
}

void ConditionsWindow::set_miles (bool miles)
{
  miles_ = miles;
  map_->miles = miles;
  update_de_dx ();
}

void ConditionsWindow::set_call_info (QWidget * info)
{
  call_info_ = info;
  auto * box = qobject_cast<QVBoxLayout *> (dx_info_->parentWidget ()->layout ());
  if (!box) return;
  info->setParent (dx_info_->parentWidget ());
  // on the box's own background, not a panel of its own
  info->setStyleSheet ("QFrame#callsign_info {background: transparent; border: none;} QFrame#callsign_info QLabel {background: transparent;}");
  int const at = box->indexOf (dx_info_);
  box->insertWidget (at < 0 ? 1 : at, info);
  info->installEventFilter (this);
  update_containers ();
  update_de_dx ();
}

bool ConditionsWindow::eventFilter (QObject * o, QEvent * e)
{
  // double-click the Sun: its menu of pictures where you clicked
  if (o == sun_image_ && QEvent::MouseButtonDblClick == e->type () && sun_pick_ && sun_pick_->menu ())
    {
      sun_pick_->menu ()->popup (static_cast<QMouseEvent *> (e)->globalPos ());
      return true;
    }
  // the QRZ details showing or not: the call above the table follows
  if (o == call_info_ && (QEvent::Show == e->type () || QEvent::Hide == e->type ()))
    {
      QTimer::singleShot (0, this, [this] {update_de_dx ();});
    }
  return QWidget::eventFilter (o, e);
}

void ConditionsWindow::set_dx (QString const& call, QString const& grid)
{
  dx_call_ = call;
  dx_grid_ = grid;
  remember_grid (call, grid);
  double lat, lon;
  if (call.size () && grid_to_latlon (grid, lat, lon))
    {
      map_->set_dx (call, lat, lon);
    }
  else if (call.size () && map_->stations.contains (call))
    {
      auto const& s = map_->stations[call];
      map_->set_dx (call, s.lat, s.lon);
    }
  else
    {
      map_->set_dx (call, qQNaN (), qQNaN ());
    }
  update_de_dx ();
  schedule_voacap ();
  Q_EMIT dx_changed_for_tools ();
}

void ConditionsWindow::add_station (QString const& call, QString const& grid, int snr, bool cq, bool to_me,
                                    QColor const& highlight, double country_lat, double country_lon)
{
  if (call.isEmpty ()) return;
  double lat, lon;
  bool approx = false;
  if (grid_to_latlon (grid, lat, lon)) remember_grid (call, grid);
  else
    {
      // keep a better position heard earlier: on the map, or remembered
      // from any message, the DX Grid box or QRZ this session
      if (map_->stations.contains (call) && !map_->stations[call].approx)
        {
          lat = map_->stations[call].lat;
          lon = map_->stations[call].lon;
        }
      else if (known_grids_.contains (call) && grid_to_latlon (known_grids_[call], lat, lon))
        {
        }
      else if (!qIsNaN (country_lat) && !qIsNaN (country_lon))
        {
          lat = country_lat;
          lon = country_lon;
          approx = true;
        }
      else
        {
          return;
        }
    }
  map_->stations[call] = Station {lat, lon, QDateTime::currentDateTimeUtc (), snr, cq, to_me, approx, highlight};
  if (isVisible ()) map_->update ();
}

// a station's grid, kept for the session whatever clears the map; a longer
// (more exact) grid replaces a shorter one, and a dot already on the map moves
void ConditionsWindow::remember_grid (QString const& call, QString const& grid)
{
  double lat, lon;
  if (call.isEmpty () || !grid_to_latlon (grid, lat, lon)) return;
  auto const g = grid.trimmed ().toUpper ();
  auto const old = known_grids_.value (call);
  if (old.size () > g.size () && old.startsWith (g.left (4))) return;
  known_grids_[call] = g;
  if (map_->stations.contains (call))
    {
      auto& st = map_->stations[call];
      st.lat = lat;
      st.lon = lon;
      st.approx = false;
      if (isVisible ()) map_->update ();
    }
}

void ConditionsWindow::clear_stations ()
{
  map_->stations.clear ();
  map_->update ();
}

bool ConditionsWindow::grid_to_latlon (QString const& grid_in, double& lat, double& lon)
{
  auto const g = grid_in.trimmed ().toUpper ();
  if (g.size () < 4) return false;
  if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !g[2].isDigit () || !g[3].isDigit ()) return false;
  if (g.startsWith ("RR73")) return false;        // a message, not a grid
  lon = (g[0].unicode () - 'A') * 20. - 180. + (g[2].unicode () - '0') * 2.;
  lat = (g[1].unicode () - 'A') * 10. - 90. + (g[3].unicode () - '0') * 1.;
  if (g.size () >= 6 && g[4] >= 'A' && g[4] <= 'X' && g[5] >= 'A' && g[5] <= 'X')
    {
      lon += (g[4].unicode () - 'A') * (2. / 24.) + 1. / 24.;
      lat += (g[5].unicode () - 'A') * (1. / 24.) + 0.5 / 24.;
    }
  else
    {
      lon += 1.;
      lat += 0.5;
    }
  return true;
}

void ConditionsWindow::set_worked (QString const& band, QSet<QString> const& calls, QSet<QString> const& grids,
                                   QSet<QString> const& today_calls, QSet<QString> const& today_grids,
                                   QSet<QString> const& all_grids, QSet<QString> const& today_all_grids)
{
  map_->all_grids = all_grids;
  map_->today_all_grids = today_all_grids;
  map_->today_calls = today_calls;
  map_->today_grids = today_grids;
  bool const band_changed = band != map_->band_label;
  map_->band_label = band;
  map_->worked_calls = calls;
  map_->worked_grids = grids;
  // every band's reports are kept, so changing band just picks others
  if (band_changed) map_->rebuild_psk ();
  map_->update ();
}

namespace
{
  QString mhz_band (double f)
  {
    struct Band {double lo, hi; char const * name;};
    static Band const bands[] = {
      {1.8, 2.0, "160m"}, {3.5, 4.0, "80m"}, {5.25, 5.45, "60m"}, {7.0, 7.3, "40m"}, {10.1, 10.15, "30m"},
      {14.0, 14.35, "20m"}, {18.068, 18.168, "17m"}, {21.0, 21.45, "15m"}, {24.89, 24.99, "12m"},
      {28.0, 29.7, "10m"}, {50.0, 54.0, "6m"}, {144.0, 148.0, "2m"}, {420.0, 450.0, "70cm"},
    };
    for (auto const& b : bands) if (f >= b.lo && f <= b.hi) return b.name;
    return QString {};
  }
}

// the live feed from PSK Reporter: on while Heard me is shown and Live is
// ticked
void ConditionsWindow::update_psk_live ()
{
  bool const want = map_->show_psk && psk_live_action_ && psk_live_action_->isChecked () && !my_call_.isEmpty ();
  if (!psk_live_)
    {
      if (!want) return;
      psk_live_ = new PskLive {this};
      connect (psk_live_, &PskLive::report, this, [this] (PskLive::Report const& r) {
          ConditionsMap::Spot sp;
          sp.call = r.receiver;
          sp.grid = r.grid;
          sp.snr = r.snr;
          sp.mhz = r.mhz;
          sp.band = r.band.isEmpty () ? mhz_band (r.mhz) : r.band;
          sp.when = r.when.isValid () ? r.when : QDateTime::currentDateTimeUtc ();
          if (!grid_to_latlon (sp.grid, sp.lat, sp.lon)) return;
          auto const key = sp.call + '|' + sp.band;
          auto it = map_->psk_store.find (key);
          if (it == map_->psk_store.end () || it->when <= sp.when) map_->psk_store[key] = sp;
          map_->psk_last = QDateTime::currentDateTimeUtc ();
          map_->rebuild_psk ();
          if (isVisible ()) map_->update ();
        });
      connect (psk_live_, &PskLive::state_changed, this, [this] {
          map_->psk_live = psk_live_->connected ();
          if (map_->psk_live) map_->psk_error.clear ();
          map_->update ();
        });
    }
  psk_live_->listen (want ? my_call_ : QString {});
  map_->psk_live = want && psk_live_->connected ();
  // the note says how long ago the last report was: keep it current
  if (!psk_tick_)
    {
      psk_tick_ = new QTimer {this};
      psk_tick_->setInterval (5000);
      connect (psk_tick_, &QTimer::timeout, this, [this] {
          map_->rebuild_psk ();
          if (isVisible ()) map_->update ();
        });
    }
  if (map_->show_psk) psk_tick_->start ();
  else psk_tick_->stop ();
}

// who heard you over the last hour, from PSK Reporter's query: when Heard
// me is switched on, then every 15 minutes (every 5 while the live feed is
// down); they ask that it's not checked more often than every five minutes
void ConditionsWindow::fetch_psk ()
{
  if (!psk_timer_)
    {
      psk_timer_ = new QTimer {this};
      psk_timer_->setInterval (5 * 60 * 1000);
      connect (psk_timer_, &QTimer::timeout, this, &ConditionsWindow::fetch_psk);
    }
  if (!map_->show_psk || my_call_.isEmpty ())
    {
      psk_timer_->stop ();
      return;
    }
  if (!psk_timer_->isActive ()) psk_timer_->start ();
  auto const now = QDateTime::currentDateTimeUtc ();
  int const wait = map_->psk_live ? 15 * 60 - 30 : 5 * 60 - 30;
  if (psk_asked_.isValid () && psk_asked_.secsTo (now) < wait) return;
  psk_asked_ = now;
  QUrl url {"https://retrieve.pskreporter.info/query"};
  QUrlQuery q;
  q.addQueryItem ("senderCallsign", my_call_);
  q.addQueryItem ("flowStartSeconds", "-3600");
  q.addQueryItem ("rronly", "1");
  q.addQueryItem ("appcontact", "KF0KYL Digi");
  url.setQuery (q);
  QNetworkRequest req {url};
  req.setHeader (QNetworkRequest::UserAgentHeader, "KF0KYL Digi");
  auto * reply = network_->get (req);
  connect (reply, &QNetworkReply::finished, this, [this, reply] {
      reply->deleteLater ();
      if (reply->error ())
        {
          // only worth saying when the live feed isn't working either
          map_->psk_error = tr ("couldn't be reached, will try again");
          map_->update ();
          return;
        }
      map_->psk_error.clear ();
      QXmlStreamReader xml {reply->readAll ()};
      while (!xml.atEnd ())
        {
          xml.readNext ();
          if (!xml.isStartElement () || xml.name () != QLatin1String {"receptionReport"}) continue;
          auto const a = xml.attributes ();
          ConditionsMap::Spot sp;
          sp.call = a.value ("receiverCallsign").toString ().toUpper ();
          sp.grid = a.value ("receiverLocator").toString ();
          sp.snr = a.value ("sNR").toInt ();
          sp.mhz = a.value ("frequency").toDouble () / 1e6;
          sp.band = mhz_band (sp.mhz);
          sp.when = QDateTime::fromSecsSinceEpoch (a.value ("flowStartSeconds").toLongLong (), Qt::UTC);
          if (sp.call.isEmpty () || sp.band.isEmpty () || !grid_to_latlon (sp.grid, sp.lat, sp.lon)) continue;
          // one line per station and band: the most recent report
          auto const key = sp.call + '|' + sp.band;
          auto it = map_->psk_store.find (key);
          if (it == map_->psk_store.end () || it->when < sp.when) map_->psk_store[key] = sp;
        }
      map_->rebuild_psk ();
      map_->psk_updated = QDateTime::currentDateTimeUtc ();
      map_->update ();
    });
}
