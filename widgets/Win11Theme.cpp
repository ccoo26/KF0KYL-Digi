#include "Win11Theme.hpp"

#include <QApplication>
#include <QStyleFactory>
#include <QPalette>
#include <QColor>
#include <QEvent>
#include <QWidget>
#include <QHash>
#include <QLabel>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QGuiApplication>
#include <QTimer>
#include <QPointer>
#include <QWindow>

#ifdef Q_OS_WIN
#include <QLibrary>
#include <windows.h>
#endif

#include "qt_helpers.hpp"

#include "moc_Win11Theme.cpp"

bool Win11Theme::dark_ {false};

namespace
{
  // Colour tokens loosely following the Windows 11 Fluent design kit
  struct Tokens
  {
    char const * bg;            // window background (Mica base)
    char const * layer;         // slightly raised layer (status bar, headers)
    char const * card;          // card / content surfaces
    char const * ctrl;          // control face
    char const * ctrl_hover;
    char const * ctrl_press;
    char const * border;        // subtle stroke
    char const * border_strong; // control bottom stroke
    char const * text;
    char const * text_secondary;
    char const * text_disabled;
    char const * accent;
    char const * accent_hover;
    char const * accent_press;
    char const * accent_text;   // text on accent fill
    char const * input;         // text box face
    char const * scroll_thumb;
    char const * selection;
  };

  Tokens const dark_tokens {
    "#0F1115", "#13161B", "#181B21", "#20242C", "#282D37", "#1B1F26",
    "#2A2F39", "#3A404C", "#E9ECF1", "#8A93A3", "#5A6170",
    "#4CC2FF", "#6BCDFF", "#3AA9E6", "#06121C",
    "#1C2027", "#3A404C", "#1D4A66"
  };

  Tokens const light_tokens {
    "#EEF1F5", "#E6EAF0", "#FFFFFF", "#F4F6F9", "#E9EDF3", "#DFE4EC",
    "#E2E6EC", "#C9D0DA", "#16191F", "#6B7484", "#A6ADB9",
    "#0A6CFF", "#2B80FF", "#0858D1", "#FFFFFF",
    "#FFFFFF", "#B8C0CC", "#CFE0FF"
  };

  // more colour schemes, same token order as above
  Tokens const midnight_tokens {
    "#0F1626", "#0B111E", "#16203A", "#1B2742", "#24335A", "#18223B",
    "#2A3A5E", "#3B4F7A", "#E8EEFF", "#A9B6D6", "#5B6A8C",
    "#4FC3F7", "#47B0DE", "#3E9CC4", "#000000",
    "#121B30", "#3E5078", "#1E4E7A"
  };

  // for low vision: pure black, white text, bright yellow accents
  Tokens const contrast_tokens {
    "#000000", "#000000", "#0A0A0A", "#000000", "#262626", "#3A3A3A",
    "#FFFFFF", "#FFFFFF", "#FFFFFF", "#FFFF00", "#A0A0A0",
    "#FFFF00", "#FFEA00", "#E6D200", "#000000",
    "#000000", "#FFFFFF", "#0000C8"
  };

  // dark red on black keeps your eyes adjusted for night operating
  Tokens const night_red_tokens {
    "#120404", "#0E0303", "#1C0707", "#220909", "#2E0C0C", "#1A0606",
    "#3A1010", "#5A1A1A", "#FF6B6B", "#C04848", "#6A2626",
    "#FF3B3B", "#E63535", "#CC2F2F", "#000000",
    "#160505", "#5A1A1A", "#5A1414"
  };

  // the green of the KF0KYL Digi logo
  // Digi Green: the logo's colours. Black, white text, blue for picked
  // things, lime green for titles, switches and Monitor. High contrast.
  Tokens const green_tokens {
    "#000000", "#050806", "#070B08", "#0F1711", "#16231A", "#0B120D",
    "#1F3A24", "#2F5A36", "#FFFFFF", "#B9C7BC", "#5E6F62",
    "#0A9BF0", "#2AAAF5", "#0884CC", "#FFFFFF",
    "#050806", "#2F5A36", "#0A4A73"
  };
  char const * const digi_lime = "#8CF010";

  // a soft paper-coloured light scheme, easier on the eyes than white
  Tokens const paper_tokens {
    "#F4ECD8", "#EDE3CB", "#FBF6EA", "#F7F0DF", "#EFE5CE", "#E6DABE",
    "#D9CBA8", "#BBA77A", "#3B2F1E", "#6B5A40", "#A8977A",
    "#9A5B13", "#A86A22", "#B37A33", "#FFFFFF",
    "#FFFBF2", "#BBA77A", "#EBD9AE"
  };

  struct SchemeInfo {char const * name; Tokens const * tokens; bool dark;};
  SchemeInfo const schemes[] = {
    {QT_TRANSLATE_NOOP ("Win11Theme", "Light"), &light_tokens, false},
    {QT_TRANSLATE_NOOP ("Win11Theme", "Dark"), &dark_tokens, true},
    {QT_TRANSLATE_NOOP ("Win11Theme", "Midnight Blue"), &midnight_tokens, true},
    {QT_TRANSLATE_NOOP ("Win11Theme", "High Contrast"), &contrast_tokens, true},
    {QT_TRANSLATE_NOOP ("Win11Theme", "Night Red"), &night_red_tokens, true},
    {QT_TRANSLATE_NOOP ("Win11Theme", "Digi Green"), &green_tokens, true},
    {QT_TRANSLATE_NOOP ("Win11Theme", "Paper"), &paper_tokens, false},
  };
  int const scheme_count = sizeof schemes / sizeof schemes[0];

  int current_scheme {0};
  Tokens const& current_tokens () {return *schemes[current_scheme].tokens;}
  bool digi () {return &green_tokens == schemes[current_scheme].tokens;}

  // Parts of the main window set their own colours with fixed values
  // written for the plain Light and Dark schemes. In the other schemes
  // those values are swapped for the scheme's own as they are applied.
  // softer versions of the strong colours the program sets for states
  // (transmitting, alerts and so on), so they sit well with the cards
  QString soften (QString sheet)
  {
    QList<QPair<QString, QString>> const map {
      {"#ff0000", "#E5484D"}, {"background-color: red", "background-color: #E5484D"},
      {"background-color:red", "background-color:#E5484D"},
      {"#ffff00", "#F5C542"}, {"background-color: yellow", "background-color: #F5C542"},
      {"#6CCB5F", "#3FB950"}, {"#00ffff", "#4CD6E8"}, {"#66ffff", "#4CD6E8"},
      {"#99ffff", "#7FDCEB"}, {"background-color: cyan", "background-color: #4CD6E8"},
    };
    for (auto const& p : map) sheet.replace (p.first, p.second, Qt::CaseInsensitive);
    return sheet;
  }

  QString remap_sheet (QString sheet, QWidget * w = nullptr)
  {
    auto const& t = current_tokens ();
    // rounder corners, no old 3D outset borders
    sheet.replace (QRegularExpression {"border-radius:\\s*\\d+px"}, "border-radius: 9px");
    sheet.replace (QRegularExpression {"border-style:\\s*outset;?"}, "");
    sheet.replace (QRegularExpression {"border-color:\\s*black;?"}, "border-color: transparent;");
    sheet = soften (sheet);
    // the selected band button takes the scheme's accent colour
    if (w && QRegularExpression {"^pb\\d+$"}.match (w->objectName ()).hasMatch () && sheet.contains ("#3FB950"))
      {
        sheet.replace ("#3FB950", t.accent).replace ("#000000", t.accent_text);
      }
    // the dial frequency and clock displays get rounded corners
    if (w && (w->objectName () == "labDialFreq" || w->objectName () == "labUTC"))
      {
        sheet += " QLabel {border-radius: 12px; padding: 2px 10px;}";
      }
    // coloured labels in the status bar become pills
    if (w && qobject_cast<QLabel *> (w) && w->parentWidget () && w->parentWidget ()->inherits ("QStatusBar")
        && sheet.contains ("background-color"))
      {
        sheet += " QLabel {border-radius: 9px; padding: 1px 10px; font-weight: 600;}";
      }
    // Digi Green: the "on" greens (Monitor, Receiving) in the logo's lime
    if (digi ()) sheet.replace ("#3FB950", digi_lime, Qt::CaseInsensitive);
    QList<QPair<QString, QString>> const map {
      {"#3A3A3A", t.ctrl}, {"#4D4D4D", t.border}, {"#F0F0F0", t.text},
      {"#1F1F1F", t.input}, {"#2D2D2D", t.ctrl}, {"#373737", t.ctrl_hover},
      {"#202020", t.bg}, {"#2B2B2B", t.card},
      {"#e1e1e1", t.ctrl}, {"#adadad", t.border},
    };
    for (auto const& p : map) sheet.replace (p.first, p.second, Qt::CaseInsensitive);
    return sheet;
  }

  // check boxes are drawn as sliding switches; the pictures are made for
  // the scheme in use, as its accent colour changes
  QString switch_image (Tokens const& t, bool on, bool disabled)
  {
    static int const W = 60, H = 34;      // twice size, for sharp screens
    QDir const dir {QDir::tempPath () + "/kf0kyldigi-theme"};
    dir.mkpath (".");
    auto const name = dir.filePath (QString {"switch_%1_%2_%3_%4.png"}
                                    .arg (QString {&t == &green_tokens ? digi_lime : t.accent}.mid (1)).arg (QString {t.border_strong}.mid (1))
                                    .arg (on ? "on" : "off").arg (disabled ? "dis" : "en"));
    if (!QFile::exists (name))
      {
        QImage img {W, H, QImage::Format_ARGB32_Premultiplied};
        img.fill (Qt::transparent);
        QPainter p {&img};
        p.setRenderHint (QPainter::Antialiasing);
        QColor track = on ? QColor {&t == &green_tokens ? digi_lime : t.accent} : QColor {t.border_strong};
        QColor knob = on ? QColor {t.accent_text} : QColor {t.text_secondary};
        if (on) knob = QColor {"#FFFFFF"};
        if (disabled) {track.setAlpha (90); knob.setAlpha (120);}
        p.setPen (Qt::NoPen);
        p.setBrush (track);
        p.drawRoundedRect (QRectF {1, 1, W - 2., H - 2.}, (H - 2) / 2., (H - 2) / 2.);
        p.setBrush (knob);
        double const d = H - 10.;
        p.drawEllipse (QRectF {on ? W - 5. - d : 5., 5., d, d});
        p.end ();
        img.save (name);
      }
    return QString {name}.replace ('\\', '/');
  }

  // %1 style tokens are replaced by name, see fill() below
  char const * const qss_template = R"QSS(
QWidget {
  background-color: @bg;
  color: @text;
  selection-background-color: @selection;
  selection-color: @text;
}
QWidget:disabled { color: @text_disabled; }
QLabel, QCheckBox, QRadioButton { background: transparent; }

QMainWindow::separator { background: @bg; width: 8px; height: 8px; }

/* ---- menus ---- */
QMenuBar { background: @bg; padding: 4px 6px; color: @text_secondary; }
QMenuBar::item { background: transparent; padding: 5px 11px; border-radius: 8px; color: @text_secondary; }
QMenuBar::item:selected { background: @ctrl; color: @text; }
QMenuBar::item:pressed { background: @ctrl_hover; color: @text; }
QMenu {
  background: @card;
  border: 1px solid @border;
  border-radius: 12px;
  padding: 6px;
}
QMenu::item { padding: 7px 28px 7px 26px; border-radius: 8px; background: transparent; }
QMenu::item:selected { background: @ctrl_hover; }
QMenu::item:disabled { color: @text_disabled; }
QMenu::separator { height: 1px; background: @border; margin: 5px 8px; }

/* ---- buttons: soft rounded, no hard bottom edge ---- */
QPushButton, QToolButton {
  background-color: @ctrl;
  color: @text;
  border: 1px solid @border;
  border-radius: 9px;
  padding: 5px 10px;
  min-height: 18px;
}
QPushButton:hover, QToolButton:hover { background-color: @ctrl_hover; border-color: @border_strong; }
QPushButton:pressed, QToolButton:pressed { background-color: @ctrl_press; color: @text_secondary; }
QPushButton:checked, QToolButton:checked {
  background-color: @accent;
  color: @accent_text;
  border: 1px solid @accent;
}
QPushButton:checked:hover, QToolButton:checked:hover { background-color: @accent_hover; }
QPushButton:disabled, QToolButton:disabled {
  background-color: @ctrl_press;
  color: @text_disabled;
  border-color: @border;
}
QPushButton:default { border: 1px solid @accent; }
QToolButton { padding: 4px 9px; }
QToolButton::menu-indicator { image: url(:/win11/down_@theme.png); width: 9px; height: 9px; subcontrol-position: right center; right: 6px; }
QToolButton[popupMode="2"] { padding-right: 22px; }
/* Halt Tx always reads as a stop button */
QPushButton#stopTxButton { color: #FF6B6B; border: 1px solid rgba(229, 72, 77, 150); background-color: rgba(229, 72, 77, 28); }
QPushButton#stopTxButton:hover { background-color: rgba(229, 72, 77, 60); }
QLabel#lh_decodes_title_label, QLabel#rh_decodes_title_label { color: @title; font-weight: 600; }

/* ---- text inputs ---- */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QDateTimeEdit, QDateEdit, QTimeEdit {
  background-color: @input;
  color: @text;
  border: 1px solid @border;
  border-radius: 8px;
  padding: 3px 7px;
  min-height: 20px;
}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover,
QDateTimeEdit:hover { border-color: @border_strong; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus,
QDateTimeEdit:focus {
  background-color: @input;
  border: 1px solid @accent;
}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
  color: @text_disabled;
  background-color: @ctrl_press;
}
QComboBox QAbstractItemView {
  background: @card;
  border: 1px solid @border;
  border-radius: 10px;
  padding: 4px;
  outline: 0;
  selection-background-color: @ctrl_hover;
  selection-color: @text;
}

/* spin box and combo box arrows */
QSpinBox, QDoubleSpinBox, QDateTimeEdit, QDateEdit, QTimeEdit { padding-right: 20px; }
QAbstractSpinBox::up-button {
  subcontrol-origin: border; subcontrol-position: top right;
  width: 18px; border: none; background: transparent;
  margin: 2px 3px 0 0; border-radius: 5px;
}
QAbstractSpinBox::down-button {
  subcontrol-origin: border; subcontrol-position: bottom right;
  width: 18px; border: none; background: transparent;
  margin: 0 3px 2px 0; border-radius: 5px;
}
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: @ctrl_hover; }
QAbstractSpinBox::up-arrow { image: url(:/win11/up_@theme.png); width: 10px; height: 10px; }
QAbstractSpinBox::down-arrow { image: url(:/win11/down_@theme.png); width: 10px; height: 10px; }
QAbstractSpinBox::up-arrow:disabled, QAbstractSpinBox::up-arrow:off,
QAbstractSpinBox::down-arrow:disabled, QAbstractSpinBox::down-arrow:off { image: none; }
QComboBox { padding-right: 18px; }
QComboBox::drop-down {
  subcontrol-origin: padding; subcontrol-position: top right;
  width: 18px; border: none; background: transparent;
}
QComboBox::down-arrow { image: url(:/win11/down_@theme.png); width: 10px; height: 10px; }

/* ---- check boxes are sliding switches ---- */
QCheckBox, QRadioButton { spacing: 7px; }
QCheckBox::indicator { width: 30px; height: 17px; border: none; background: transparent; image: url(@switch_off); }
QCheckBox::indicator:checked, QCheckBox::indicator:indeterminate { image: url(@switch_on); }
QCheckBox::indicator:disabled { image: url(@switch_dis_off); }
QCheckBox::indicator:checked:disabled { image: url(@switch_dis_on); }
QRadioButton::indicator {
  width: 16px; height: 16px;
  border: 2px solid @border_strong; border-radius: 10px;
  background: transparent;
}
QRadioButton::indicator:hover { border-color: @text_secondary; }
QRadioButton::indicator:checked {
  width: 8px; height: 8px;
  border: 6px solid @accent; border-radius: 10px;
  background: @accent_text;
}
QRadioButton::indicator:disabled { border-color: @border; }

/* ---- text areas, lists, tables: cards ---- */
QTextEdit, QPlainTextEdit, QTextBrowser, QListView, QListWidget, QTreeView,
QTableView, QTableWidget {
  background-color: @card;
  color: @text;
  border: 1px solid @border;
  border-radius: 12px;
  padding: 4px;
}
QHeaderView::section {
  background-color: @card;
  color: @text_secondary;
  border: none;
  border-bottom: 1px solid @border;
  padding: 5px 7px;
  font-weight: 600;
}
QTableCornerButton::section { background: @card; border: none; }

/* ---- tabs: pills ---- */
QTabWidget::pane {
  background: @card;
  border: 1px solid @border;
  border-radius: 12px;
  top: 4px;
}
QTabBar { background: transparent; }
QTabBar::tab {
  background: transparent;
  color: @text_secondary;
  padding: 6px 14px;
  margin: 2px 3px 2px 0;
  border: none;
  border-radius: 9px;
}
QTabBar::tab:hover { color: @text; background: @ctrl; }
QTabBar::tab:selected { color: @accent_text; background: @accent; }

/* ---- grouping: cards ---- */
QGroupBox {
  background-color: @card;
  border: 1px solid @border;
  border-radius: 14px;
  margin-top: 20px;
  padding: 12px 8px 8px 8px;
}
QGroupBox::title {
  subcontrol-origin: margin;
  subcontrol-position: top left;
  left: 12px;
  padding: 0 4px;
  color: @title;
  background: transparent;
  font-weight: 600;
}
QFrame[frameShape="4"], QFrame[frameShape="5"] { color: @border; }  /* HLine / VLine */

/* ---- scroll bars: thin ---- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 3px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 3px; }
QScrollBar::handle:vertical { background: @scroll_thumb; min-height: 28px; border-radius: 3px; margin: 0 1px; }
QScrollBar::handle:horizontal { background: @scroll_thumb; min-width: 28px; border-radius: 3px; margin: 1px 0; }
QScrollBar::handle:hover { background: @text_secondary; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* ---- sliders ---- */
QSlider::groove:horizontal { height: 4px; background: @border; border-radius: 2px; }
QSlider::groove:vertical { width: 4px; background: @border; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
QSlider::add-page:vertical { background: @accent; border-radius: 2px; }
QSlider::handle:horizontal {
  background: #FFFFFF; border: 1px solid @border_strong;
  width: 14px; height: 14px; margin: -6px 0; border-radius: 8px;
}
QSlider::handle:vertical {
  background: #FFFFFF; border: 1px solid @border_strong;
  width: 14px; height: 14px; margin: 0 -6px; border-radius: 8px;
}
QSlider::handle:hover { border-color: @accent; }

/* ---- progress: slim gradient bar ---- */
QProgressBar {
  background-color: @ctrl;
  color: @text;
  border: none;
  border-radius: 6px;
  text-align: center;
  min-height: 12px;
}
QProgressBar::chunk { background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent, stop:1 #7C5CFF); border-radius: 6px; }

/* ---- splitters ---- */
QSplitter::handle { background: transparent; border-radius: 3px; }
QSplitter::handle:vertical { height: 8px; image: url(:/win11/grip_h_@theme.png); }
QSplitter::handle:horizontal { width: 8px; image: url(:/win11/grip_v_@theme.png); }
QSplitter::handle:hover { background: @border; }
QSplitter::handle:pressed { background: @accent; }

/* ---- status bar & tooltips ---- */
QStatusBar { background: @bg; border: none; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 1px 8px; }
QToolTip {
  background-color: @card;
  color: @text;
  border: 1px solid @border_strong;
  border-radius: 10px;
  padding: 6px 9px;
}

/* ---- dialogs ---- */
QDialog { background-color: @bg; }
QDialogButtonBox QPushButton { min-width: 76px; }
)QSS";

  QString fill (Tokens const& t, QString const& theme)
  {
    QString s {qss_template};
    s.replace ("@theme", theme);
    s.replace ("@switch_dis_off", switch_image (t, false, true));
    s.replace ("@switch_dis_on", switch_image (t, true, true));
    s.replace ("@switch_off", switch_image (t, false, false));
    s.replace ("@switch_on", switch_image (t, true, false));
    // small titles: lime in Digi Green, grey elsewhere
    s.replace ("@title", &t == &green_tokens ? QString {digi_lime} : QString {t.text_secondary});
    // longest names first so that e.g. @accent_text is not clobbered by @accent
    QList<QPair<QString, QString>> const map {
      {"@border_strong", t.border_strong},
      {"@text_secondary", t.text_secondary},
      {"@text_disabled", t.text_disabled},
      {"@accent_hover", t.accent_hover},
      {"@accent_press", t.accent_press},
      {"@accent_text", t.accent_text},
      {"@scroll_thumb", t.scroll_thumb},
      {"@ctrl_hover", t.ctrl_hover},
      {"@ctrl_press", t.ctrl_press},
      {"@selection", t.selection},
      {"@accent", t.accent},
      {"@border", t.border},
      {"@layer", t.layer},
      {"@input", t.input},
      {"@card", t.card},
      {"@ctrl", t.ctrl},
      {"@text", t.text},
      {"@bg", t.bg},
    };
    for (auto const& p : map)
      {
        s.replace (p.first, p.second);
      }
    return s;
  }

  QPalette make_palette (Tokens const& t)
  {
    QPalette p;
    QColor const text {t.text};
    QColor const disabled {t.text_disabled};
    p.setColor (QPalette::Window, QColor {t.bg});
    p.setColor (QPalette::WindowText, text);
    p.setColor (QPalette::Base, QColor {t.input});
    p.setColor (QPalette::AlternateBase, QColor {t.card});
    p.setColor (QPalette::ToolTipBase, QColor {t.card});
    p.setColor (QPalette::ToolTipText, text);
    p.setColor (QPalette::Text, text);
    p.setColor (QPalette::Button, QColor {t.ctrl});
    p.setColor (QPalette::ButtonText, text);
    p.setColor (QPalette::BrightText, Qt::red);
    p.setColor (QPalette::Link, QColor {t.accent});
    p.setColor (QPalette::Highlight, QColor {t.accent});
    p.setColor (QPalette::HighlightedText, QColor {t.accent_text});
    p.setColor (QPalette::Light, QColor {t.ctrl_hover});
    p.setColor (QPalette::Midlight, QColor {t.border});
    p.setColor (QPalette::Mid, QColor {t.border_strong});
    p.setColor (QPalette::Dark, QColor {t.layer});
    p.setColor (QPalette::Shadow, QColor {"#000000"});
#if QT_VERSION >= QT_VERSION_CHECK (5, 12, 0)
    p.setColor (QPalette::PlaceholderText, QColor {t.text_secondary});
#endif
    p.setColor (QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor (QPalette::Disabled, QPalette::Text, disabled);
    p.setColor (QPalette::Disabled, QPalette::ButtonText, disabled);
    return p;
  }

#ifdef Q_OS_WIN
  using DwmSetWindowAttributeFn = HRESULT (WINAPI *) (HWND, DWORD, LPCVOID, DWORD);

  DwmSetWindowAttributeFn dwm_set_attribute ()
  {
    static DwmSetWindowAttributeFn fn {nullptr};
    static bool tried {false};
    if (!tried)
      {
        tried = true;
        fn = reinterpret_cast<DwmSetWindowAttributeFn> (QLibrary::resolve ("dwmapi", "DwmSetWindowAttribute"));
      }
    return fn;
  }
#endif
}

void Win11Theme::install (QApplication * app)
{
  // Fusion renders style sheets consistently and honours the palette
  if (auto * fusion = QStyleFactory::create ("Fusion"))
    {
      app->setStyle (fusion);
    }
  app->installEventFilter (new Win11Theme {app});
}

QString Win11Theme::styleSheet (bool dark)
{
  Q_UNUSED (dark);
  return fill (current_tokens (), dark_ ? "dark" : "light");
}

int Win11Theme::schemeCount () {return scheme_count;}
QString Win11Theme::schemeName (int i)
{
  return i >= 0 && i < scheme_count ? tr (schemes[i].name) : QString {};
}
int Win11Theme::scheme () {return current_scheme;}
bool Win11Theme::schemeIsDark (int i) {return i >= 0 && i < scheme_count && schemes[i].dark;}

void Win11Theme::apply (bool dark, QFont const& font, QString const& extra)
{
  // the old two-way switch: Light or Dark
  applyScheme (dark ? 1 : 0, font, extra);
}

void Win11Theme::applyScheme (int scheme, QFont const& font, QString const& extra)
{
  current_scheme = qBound (0, scheme, scheme_count - 1);
  dark_ = schemes[current_scheme].dark;
  qApp->setPalette (make_palette (current_tokens ()));
  qApp->setFont (font);
  qApp->setStyleSheet (styleSheet (dark_) + extra + "* {" + font_as_stylesheet (font) + '}');
  // re-colour widgets that set their own colours
  for (auto * w : qApp->allWidgets ())
    {
      auto const original = w->property ("w11_original").toString ();
      if (!original.isEmpty ())
        {
          auto const mapped = remap_sheet (original, w);
          w->setProperty ("w11_applied", mapped);
          w->setStyleSheet (mapped);
        }
    }
  for (auto * w : qApp->topLevelWidgets ())
    {
      updateTitleBar (w);
    }
}

void Win11Theme::updateTitleBar (QWidget * w)
{
#ifdef Q_OS_WIN
  if (!w || !w->isWindow () || !w->testAttribute (Qt::WA_WState_Created)) return;
  if (auto fn = dwm_set_attribute ())
    {
      auto hwnd = reinterpret_cast<HWND> (w->winId ());
      BOOL value = dark_ ? TRUE : FALSE;
      // DWMWA_USE_IMMERSIVE_DARK_MODE is 20 on Windows 10 20H1 and later, 19 before
      if (FAILED (fn (hwnd, 20, &value, sizeof value)))
        {
          fn (hwnd, 19, &value, sizeof value);
        }
      // DWMWA_WINDOW_CORNER_PREFERENCE (33) = DWMWCP_ROUND (2), Windows 11 only
      DWORD corner = 2;
      fn (hwnd, 33, &corner, sizeof corner);
      // make the non-client area repaint now rather than on next activation
      SetWindowPos (hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
#else
  Q_UNUSED (w);
#endif
}

bool Win11Theme::isDigiGreen () {return digi ();}

int Win11Theme::digiGreenScheme ()
{
  for (int i = 0; i < scheme_count; ++i) if (schemes[i].tokens == &green_tokens) return i;
  return 1;
}

QString Win11Theme::token (QString const& name)
{
  auto const& t = current_tokens ();
  if (name == "bg") return t.bg;
  if (name == "layer") return t.layer;
  if (name == "card") return t.card;
  if (name == "ctrl") return t.ctrl;
  if (name == "ctrl_hover") return t.ctrl_hover;
  if (name == "border") return t.border;
  if (name == "border_strong") return t.border_strong;
  if (name == "text") return t.text;
  if (name == "text_secondary") return t.text_secondary;
  if (name == "accent") return t.accent;
  if (name == "accent_text") return t.accent_text;
  if (name == "input") return t.input;
  if (name == "title") return digi () ? digi_lime : t.text_secondary;
  if (name == "accent2") return digi () ? digi_lime : "#7C5CFF";       // second colour of blends
  return t.text;
}

QString Win11Theme::controlBackground ()
{
  return current_tokens ().ctrl;
}

QString Win11Theme::controlBorder ()
{
  return current_tokens ().border_strong;
}

QString Win11Theme::inputBackground ()
{
  return current_tokens ().input;
}


QRect Win11Theme::usable_area (QWidget * w)
{
  QScreen * screen {nullptr};
  if (w)
    {
      screen = QGuiApplication::screenAt (w->frameGeometry ().center ());
      if (!screen && w->windowHandle ()) screen = w->windowHandle ()->screen ();
    }
  if (!screen) screen = QGuiApplication::primaryScreen ();
  if (!screen) return {};
  QRect avail = screen->availableGeometry ();
#ifndef Q_OS_WIN
  // some Linux desktops (Ubuntu on Wayland) report the whole screen as
  // usable even though a top bar and dock sit on it: keep clear of the
  // top bar at least
  if (avail == screen->geometry () && avail.height () > 400) avail.setTop (avail.top () + 40);
#endif
  return avail;
}

QSize Win11Theme::frame_extra (QWidget * w)
{
  QSize extra {16, 40};
  if (w)
    {
      auto const frame = w->frameGeometry ();
      auto const inner = w->geometry ();
      if (frame.height () > inner.height ())
        {
          extra = {frame.width () - inner.width (), frame.height () - inner.height ()};
        }
    }
#ifndef Q_OS_WIN
  // Ubuntu's title bars (and their shadows) are taller than the guess
  extra = extra.expandedTo ({16, 48});
#endif
  return extra;
}

// keep a window inside the usable part of its screen (not under the
// taskbar): a size saved on a bigger screen, a larger font or a higher
// display scale could otherwise open it partly off the bottom
void Win11Theme::fit_to_screen (QWidget * w)
{
  // never touch a window that's maximized or full screen, even while it's
  // minimized (Ubuntu briefly reports it as "normal" when it's restored)
  if (!w || w->isMinimized ()
      || w->windowState () & (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized)) return;
  if (auto * handle = w->windowHandle ())
    {
      if (handle->windowStates () & (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized)) return;
    }
  QRect const avail = usable_area (w);
  if (!avail.isValid ()) return;
  QRect const inner = w->geometry ();
  auto const fx = frame_extra (w);
  int const extra_w = fx.width ();
  int const extra_h = fx.height ();
  // leave it alone if every corner is on some screen's usable area, so a
  // window stretched across two monitors on purpose stays that way
  QRect const outer {inner.x () - extra_w / 2, inner.y () - (extra_h - extra_w / 2),
                     inner.width () + extra_w, inner.height () + extra_h};
  auto on_a_screen = [] (QPoint const& pt) {
    for (auto * s : QGuiApplication::screens ()) if (s->availableGeometry ().contains (pt)) return true;
    return false;
  };
  if (on_a_screen (outer.topLeft ()) && on_a_screen (outer.topRight ())
      && on_a_screen (outer.bottomLeft ()) && on_a_screen (outer.bottomRight ())
      && inner.height () + extra_h <= avail.height ()
      && w->minimumSizeHint ().height () + extra_h <= avail.height ()) return;
  int const max_w = avail.width () - extra_w;
  int const max_h = avail.height () - extra_h;
  if (max_w < 200 || max_h < 150) return;
  // never let the smallest size the layout asks for be bigger than the screen
  QSize min = w->minimumSize ();
  if (min.width () > max_w || min.height () > max_h)
    {
      w->setMinimumSize (qMin (min.width (), max_w), qMin (min.height (), max_h));
    }
  else if (w->minimumSizeHint ().width () > max_w || w->minimumSizeHint ().height () > max_h)
    {
      w->setMinimumSize (qMin (w->minimumSizeHint ().width (), max_w), qMin (w->minimumSizeHint ().height (), max_h));
    }
  int const width = qMin (inner.width (), max_w);
  int const height = qMin (inner.height (), max_h);
  if (width != inner.width () || height != inner.height ()) w->resize (width, height);
  // then move it so all of it, title bar included, is on the screen
  int x = w->frameGeometry ().x ();
  int y = w->frameGeometry ().y ();
  int const fw = width + extra_w;
  int const fh = height + extra_h;
  x = qBound (avail.left (), x, avail.right () + 1 - fw);
  y = qBound (avail.top (), y, avail.bottom () + 1 - fh);
  if (x != w->frameGeometry ().x () || y != w->frameGeometry ().y ()) w->move (x, y);
}

bool Win11Theme::eventFilter (QObject * obj, QEvent * e)
{
  if (e->type () == QEvent::StyleChange)
    {
      auto * w = qobject_cast<QWidget *> (obj);
      // widgets that style themselves fully (the Conditions window) opt out
      if (w && !w->property ("w11_keep").toBool () && w->window ()->objectName () != "ConditionsWindow")
        {
          auto const sheet = w->styleSheet ();
          if (!sheet.isEmpty () && sheet != w->property ("w11_applied").toString ())
            {
              // a new sheet from the program: keep the original, apply the modern one
              w->setProperty ("w11_original", sheet);
              auto const mapped = remap_sheet (sheet, w);
              w->setProperty ("w11_applied", mapped);
              if (mapped != sheet) w->setStyleSheet (mapped);
            }
        }
    }
  // a window coming back from minimized (a "spontaneous" show from the
  // window system) is left exactly as it was: only fit a window when the
  // program itself opens it
  if (e->type () == QEvent::Show && !e->spontaneous ())
    {
      auto * w = qobject_cast<QWidget *> (obj);
      if (w && w->isWindow () && !w->property ("w11_fitted").toBool ())
        {
          auto const type = w->windowType ();
          if (type == Qt::Window || type == Qt::Dialog)
            {
              w->setProperty ("w11_fitted", true);
              fit_to_screen (w);
              // once more after the window system has added the title bar
              QPointer<QWidget> guard {w};
              QTimer::singleShot (150, w, [guard] {if (guard) fit_to_screen (guard);});
            }
        }
    }
  if (e->type () == QEvent::Close)
    {
      // only a window that was really closed (not minimized or hidden for a
      // moment) is checked against the screen again next time it opens
      if (auto * w = qobject_cast<QWidget *> (obj))
        {
          if (w->isWindow ()) w->setProperty ("w11_fitted", false);
        }
    }
#ifdef Q_OS_WIN
  if (e->type () == QEvent::Show || e->type () == QEvent::WinIdChange)
    {
      if (auto * w = qobject_cast<QWidget *> (obj))
        {
          if (w->isWindow ()) updateTitleBar (w);
        }
    }
#endif
  return false;
}
