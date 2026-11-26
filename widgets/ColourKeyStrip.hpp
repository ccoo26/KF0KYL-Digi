#ifndef COLOUR_KEY_STRIP_HPP__
#define COLOUR_KEY_STRIP_HPP__

// What the Band Activity highlight colours mean, always in view beside
// the panel's title. Edit… opens Settings at the Colors tab.

#include <QWidget>
#include <QFont>
#include <QString>
#include <QVector>
#include <QColor>
#include <QRectF>
#include <QString>

#include "models/DecodeHighlightingModel.hpp"

class ColourKeyStrip final
  : public QWidget
{
  Q_OBJECT

public:
  explicit ColourKeyStrip (QWidget * parent = nullptr);

  void set_items (DecodeHighlightingModel::HighlightItems const&);
  // the panel's title, shown first on the top line (the key wraps under it)
  void set_lead (QWidget *);
  // the colours shown or just the title
  void set_chips_shown (bool);
  // the short name a highlight has in the key
  QString chip_text (DecodeHighlightingModel::Highlight h) const {return short_name (h);}
  QSize sizeHint () const override;
  QSize minimumSizeHint () const override;
  bool hasHeightForWidth () const override {return true;}
  int heightForWidth (int) const override;

  Q_SIGNAL void edit ();
  // a chip double-clicked: the highlight it stands for
  Q_SIGNAL void colour_clicked (DecodeHighlightingModel::Highlight);

protected:
  void paintEvent (QPaintEvent *) override;
  void mouseMoveEvent (QMouseEvent *) override;
  void mouseReleaseEvent (QMouseEvent *) override;
  void mouseDoubleClickEvent (QMouseEvent *) override;
  void resizeEvent (QResizeEvent *) override;
  bool event (QEvent *) override;
  bool eventFilter (QObject *, QEvent *) override;

private:
  struct Chip {QString text; QColor fg; QColor bg; DecodeHighlightingModel::Highlight type; mutable QRectF rect;};
  static QString short_name (DecodeHighlightingModel::Highlight);
  // where everything goes at this width; returns the height needed
  double place (double width) const;
  double row_height () const;
  QSize lead_size () const;
  QFont lead_font () const;
  QString lead_text () const;
  void place_lead ();
  QVector<Chip> chips_;
  mutable QRectF edit_rect_;
  QWidget * lead_ {nullptr};
  bool chips_shown_ {true};
};

#endif
