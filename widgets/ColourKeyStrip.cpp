#include "ColourKeyStrip.hpp"

#include <QPainter>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QFontMetricsF>
#include <QEvent>
#include <QLabel>
#include <cmath>

#include "Win11Theme.hpp"

namespace
{
  double const gap = 5.;
  double const pad = 7.;
}

ColourKeyStrip::ColourKeyStrip (QWidget * parent)
  : QWidget {parent}
{
  setMouseTracking (true);
  QSizePolicy policy {QSizePolicy::Expanding, QSizePolicy::Preferred};
  policy.setHeightForWidth (true);
  setSizePolicy (policy);
}

QString ColourKeyStrip::short_name (DecodeHighlightingModel::Highlight h)
{
  using H = DecodeHighlightingModel::Highlight;
  switch (h)
    {
    case H::CQ: return tr ("CQ");
    case H::MyCall: return tr ("My call");
    case H::Tx: return tr ("Sent");
    case H::DXCC: return tr ("New DXCC");
    case H::DXCCBand: return tr ("New DXCC on band");
    case H::Grid: return tr ("New grid");
    case H::GridBand: return tr ("New grid on band");
    case H::Call: return tr ("New call");
    case H::CallBand: return tr ("New call on band");
    case H::Continent: return tr ("New continent");
    case H::ContinentBand: return tr ("New continent on band");
    case H::CQZone: return tr ("New CQ zone");
    case H::CQZoneBand: return tr ("New CQ zone on band");
    case H::ITUZone: return tr ("New ITU zone");
    case H::ITUZoneBand: return tr ("New ITU zone on band");
    case H::LotW: return tr ("LoTW user");
    }
  return QString {};
}

void ColourKeyStrip::set_items (DecodeHighlightingModel::HighlightItems const& items)
{
  chips_.clear ();
  for (auto const& item : items)
    {
      if (!item.enabled_) continue;
      QColor const bg = Qt::NoBrush != item.background_.style () ? item.background_.color () : QColor {};
      QColor const fg = Qt::NoBrush != item.foreground_.style () ? item.foreground_.color () : QColor {};
      if (!bg.isValid () && !fg.isValid ()) continue;
      chips_ << Chip {short_name (item.type_), fg, bg, item.type_, QRectF {}};
    }
  updateGeometry ();
  update ();
}

void ColourKeyStrip::set_lead (QWidget * lead)
{
  lead_ = lead;
  lead_->setParent (this);
  lead_->show ();
  updateGeometry ();
}

void ColourKeyStrip::set_chips_shown (bool on)
{
  chips_shown_ = on;
  updateGeometry ();
  update ();
}

// the title's real size, measured as it's drawn: bold, all capitals and
// letter-spaced, which the label's own size hint doesn't allow for on every
// system (Ubuntu's font cut "BAND ACTIVITY" short)
QSize ColourKeyStrip::lead_size () const
{
  if (!lead_) return {};
  lead_->ensurePolished ();
  QSize size = lead_->sizeHint ();
  if (auto * label = qobject_cast<QLabel *> (lead_))
    {
      auto f = label->font ();
      f.setBold (true);
      QString text = label->text ();
      if (QFont::AllUppercase == f.capitalization ()) text = text.toUpper ();
      // measure without spacing, then add the spacing ourselves
      auto plain = f;
      plain.setLetterSpacing (QFont::PercentageSpacing, 100.);
      QFontMetricsF const fm {plain};
      double const base = fm.horizontalAdvance (text);
      double spacing = 0.;
      if (QFont::PercentageSpacing == f.letterSpacingType ())
        {
          spacing = (f.letterSpacing () - 100.) / 100. * base;
        }
      else spacing = f.letterSpacing () * text.size ();
      int const m = label->contentsMargins ().left () + label->contentsMargins ().right () + 2 * label->margin () + 6;
      int const w = int (std::ceil (base + qMax (0., spacing))) + m;
      size.setWidth (qMax (size.width (), w));
      size.setHeight (qMax (size.height (), int (std::ceil (fm.height ())) + 2));
    }
  return size;
}

void ColourKeyStrip::place_lead ()
{
  if (!lead_) return;
  auto const hint = lead_size ();
  lead_->setGeometry (0, int ((row_height () - hint.height ()) / 2.), hint.width (), hint.height ());
}

double ColourKeyStrip::row_height () const
{
  QFontMetricsF const fm {font ()};
  double h = fm.height () + 6;
  if (lead_) h = qMax (h, double (lead_size ().height ()));
  return h;
}

// the title, then the chips in order, wrapping onto more lines as needed;
// Edit… follows the last one
double ColourKeyStrip::place (double width) const
{
  QFontMetricsF const fm {font ()};
  auto bold = font ();
  bold.setBold (true);
  QFontMetricsF const bfm {bold};
  double const line = row_height () + 3;
  double const ch = fm.height () + 4;
  double x = lead_ ? lead_size ().width () + 10 : 0.;
  double y = 0.;
  for (auto& c : chips_)
    {
      c.rect = QRectF {};
      if (!chips_shown_) continue;
      double const w = bfm.horizontalAdvance (c.text) + 2 * pad;
      if (x > 0 && x + w > width)
        {
          x = 0;
          y += line;
        }
      c.rect = QRectF {x, y + (row_height () - ch) / 2., w, ch};
      x += w + gap;
    }
  if (chips_shown_)
    {
      double const ew = fm.horizontalAdvance (tr ("Edit…")) + 8;
      if (x > 0 && x + ew > width)
        {
          x = 0;
          y += line;
        }
      edit_rect_ = QRectF {x + 2, y + (row_height () - ch) / 2., ew, ch};
    }
  else edit_rect_ = QRectF {};
  return y + row_height ();
}

int ColourKeyStrip::heightForWidth (int w) const
{
  return int (std::ceil (place (w)));
}

QSize ColourKeyStrip::sizeHint () const
{
  return QSize {400, heightForWidth (width () > 0 ? width () : 400)};
}

QSize ColourKeyStrip::minimumSizeHint () const
{
  int const w = lead_ ? lead_size ().width () : 60;
  return QSize {w, int (row_height ())};
}

void ColourKeyStrip::resizeEvent (QResizeEvent * e)
{
  QWidget::resizeEvent (e);
  place_lead ();
  // a different width can need a different number of lines
  if (heightForWidth (width ()) != height ()) updateGeometry ();
}

void ColourKeyStrip::paintEvent (QPaintEvent *)
{
  QPainter p {this};
  p.setRenderHint (QPainter::Antialiasing);
  place (width ());
  for (auto const& c : chips_)
    {
      if (c.rect.isNull ()) continue;
      QColor fg = c.fg;
      if (c.bg.isValid ())
        {
          p.setPen (Qt::NoPen);
          p.setBrush (c.bg);
          p.drawRoundedRect (c.rect, 4, 4);
          if (!fg.isValid ()) fg = c.bg.lightness () > 140 ? QColor {"#000000"} : QColor {"#FFFFFF"};
        }
      else
        {
          p.setPen (QPen {QColor {fg.red (), fg.green (), fg.blue (), 140}, 1});
          p.setBrush (Qt::NoBrush);
          p.drawRoundedRect (c.rect.adjusted (0.5, 0.5, -0.5, -0.5), 4, 4);
        }
      auto f = font ();
      f.setBold (true);
      p.setFont (f);
      p.setPen (fg);
      p.drawText (c.rect, Qt::AlignCenter, c.text);
    }
  if (!edit_rect_.isNull ())
    {
      auto f = font ();
      f.setUnderline (underMouse () && edit_rect_.contains (mapFromGlobal (QCursor::pos ())));
      p.setFont (f);
      p.setPen (QColor {Win11Theme::token ("accent")});
      p.drawText (edit_rect_, Qt::AlignVCenter | Qt::AlignLeft, tr ("Edit…"));
    }
}

void ColourKeyStrip::mouseMoveEvent (QMouseEvent * e)
{
  setCursor (edit_rect_.contains (e->pos ()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
  update ();
}

void ColourKeyStrip::mouseDoubleClickEvent (QMouseEvent * e)
{
  for (auto const& c : chips_)
    {
      if (c.rect.contains (e->pos ()))
        {
          Q_EMIT colour_clicked (c.type);
          return;
        }
    }
}

void ColourKeyStrip::mouseReleaseEvent (QMouseEvent * e)
{
  if (Qt::LeftButton == e->button () && edit_rect_.contains (e->pos ())) Q_EMIT edit ();
}

bool ColourKeyStrip::event (QEvent * e)
{
  if (QEvent::LayoutRequest == e->type ())
    {
      // the title's text changed (another mode): make room for all of it
      place_lead ();
      updateGeometry ();
      update ();
    }
  if (QEvent::ToolTip == e->type ())
    {
      auto * he = static_cast<QHelpEvent *> (e);
      if (edit_rect_.contains (he->pos ()))
        {
          QToolTip::showText (he->globalPos (), tr ("Change the colours in Settings > Colors"), this);
        }
      else
        {
          // every colour, first one wins when more than one applies
          QString text = tr ("<b>Band Activity colours</b>, top one wins. Double-click one to change it:");
          for (auto const& c : chips_)
            {
              QColor const swatch = c.bg.isValid () ? c.bg : c.fg;
              text += QString {"<br/><span style='background:%1;color:%1'>&nbsp;&nbsp;&nbsp;</span>&nbsp;%2"}.arg (swatch.name (), c.text.toHtmlEscaped ());
            }
          QToolTip::showText (he->globalPos (), text, this);
        }
      return true;
    }
  if (QEvent::Leave == e->type ()) update ();
  return QWidget::event (e);
}
