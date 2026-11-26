// -*- Mode: C++ -*-
#ifndef WIN11_THEME_HPP_
#define WIN11_THEME_HPP_

#include <QObject>
#include <QString>
#include <QFont>
#include <QRect>
#include <QSize>

class QApplication;
class QWidget;

//
// Win11Theme - Windows 11 (Fluent-inspired) look for WSJT-X.
//
// Provides light and dark Qt style sheets plus matching palettes, and
// on Windows asks DWM for a dark title bar and rounded corners on every
// top-level window. Only presentation is touched; no radio, audio,
// timing or decoder behaviour depends on anything in here.
//
class Win11Theme final
  : public QObject
{
  Q_OBJECT

public:
  // install Fusion style and the window event filter, call once at start up
  static void install (QApplication *);

  // apply light or dark theme, extra is appended (e.g. a user style sheet)
  static void apply (bool dark, QFont const& font, QString const& extra = QString {});

  static bool isDark () {return dark_;}

  // colour schemes: 0 Light, 1 Dark, then Midnight Blue, High Contrast,
  // Night Red, Digi Green and Paper
  static int schemeCount ();
  static QString schemeName (int);
  static bool schemeIsDark (int);
  static int scheme ();
  static void applyScheme (int scheme, QFont const& font, QString const& extra = QString {});

  // re-apply title bar attributes to one window
  static void updateTitleBar (QWidget *);
  static void fit_to_screen (QWidget *);
  // the part of the screen a window may use (not under the taskbar, dock
  // or Ubuntu's top bar)
  static QRect usable_area (QWidget *);
  // the title bar and borders the window system adds around a window
  // (a safe guess until the window system has said)
  static QSize frame_extra (QWidget *);

  // colours used by code that styles individual widgets
  static QString controlBackground ();  // normal button face
  static QString controlBorder ();      // normal button border
  static QString inputBackground ();    // line edit / combo face
  // any colour of the scheme in use: bg, layer, card, ctrl, ctrl_hover,
  // border, border_strong, text, text_secondary, accent, accent_text, input,
  // title (small headings), accent2 (second colour of blends)
  static QString token (QString const& name);
  static bool isDigiGreen ();
  static int digiGreenScheme ();        // the default for a new install

protected:
  bool eventFilter (QObject *, QEvent *) override;

private:
  explicit Win11Theme (QObject * parent) : QObject {parent} {}
  static QString styleSheet (bool dark);
  static bool dark_;
};

#endif
