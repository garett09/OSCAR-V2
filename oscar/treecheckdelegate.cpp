/* TreeCheckDelegate — style-independent checkbox delegate for tree/list views.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "treecheckdelegate.h"
#include "combocheckdelegate.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace {
//! Horizontal gap between the cell edge, the check box and the text.
constexpr int kPad = 4;
//! Largest check box drawn; shrinks to fit short rows.
constexpr int kMaxBox = 16;
//! Smallest check box drawn.
constexpr int kMinBox = 10;
}

QRect TreeCheckDelegate::checkRect(const QRect &cell, bool hasText)
{
    const int sz = qBound(kMinBox, cell.height() - 4, kMaxBox);
    const int x = hasText ? cell.left() + kPad : cell.left() + (cell.width() - sz) / 2;
    return QRect(x, cell.top() + (cell.height() - sz) / 2, sz, sz);
}

QRect TreeCheckDelegate::textRect(const QRect &cell)
{
    QRect r = cell;
    r.setLeft(checkRect(cell, true).right() + 1 + kPad);
    return r;
}

void TreeCheckDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    const QVariant state = index.data(Qt::CheckStateRole);
    if (!state.isValid()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    const bool hasText = !opt.text.isEmpty();

    // Let the style draw the cell background, selection and focus rect, but with no
    // indicator and no text: those are drawn below so they can be laid out together.
    QStyleOptionViewItem panelOpt = opt;
    panelOpt.features &= ~(QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDisplay);
    panelOpt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &panelOpt, painter, widget);

    if (hasText) {
        const QRect tr = textRect(opt.rect).adjusted(0, 0, -kPad, 0);
        const QString text = opt.fontMetrics.elidedText(opt.text, opt.textElideMode, tr.width());

        // Same colour-group choice the styles make for item text.
        QPalette pal = opt.palette;
        const bool enabled = opt.state & QStyle::State_Enabled;
        pal.setCurrentColorGroup(!enabled ? QPalette::Disabled
                                 : (opt.state & QStyle::State_Active) ? QPalette::Normal
                                                                      : QPalette::Inactive);
        const QPalette::ColorRole role = (opt.state & QStyle::State_Selected)
                                         ? QPalette::HighlightedText : QPalette::Text;

        painter->save();
        painter->setFont(opt.font);
        style->drawItemText(painter, tr, Qt::AlignLeft | Qt::AlignVCenter, pal, enabled, text, role);
        painter->restore();
    }

    drawOscarCheckBox(painter, checkRect(opt.rect, hasText),
                      static_cast<Qt::CheckState>(state.toInt()) == Qt::Checked);
}

QSize TreeCheckDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    if (index.data(Qt::CheckStateRole).isValid())
        s.setHeight(qMax(s.height(), kMaxBox + 4));
    return s;
}

void TreeCheckDelegate::updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                                             const QModelIndex &index) const
{
    if (!index.data(Qt::CheckStateRole).isValid()) {
        QStyledItemDelegate::updateEditorGeometry(editor, option, index);
        return;
    }
    // Keep the editor clear of the check box, matching where paint() puts the text.
    editor->setGeometry(textRect(option.rect));
}

/*! Toggles the check state on a left click inside the drawn box, or on Space/Select.
 *  Mirrors QStyledItemDelegate::editorEvent() except that hit-testing uses checkRect()
 *  rather than the style's indicator rectangle, which need not match what we draw. */
bool TreeCheckDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                    const QStyleOptionViewItem &option, const QModelIndex &index)
{
    const Qt::ItemFlags flags = model->flags(index);
    if (!(flags & Qt::ItemIsUserCheckable) || !(flags & Qt::ItemIsEnabled)
        || !(option.state & QStyle::State_Enabled))
        return false;

    const QVariant value = index.data(Qt::CheckStateRole);
    if (!value.isValid())
        return false;

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: {
        auto *me = static_cast<QMouseEvent *>(event);
        const bool hasText = !index.data(Qt::DisplayRole).toString().isEmpty();
        if (me->button() != Qt::LeftButton
            || !checkRect(option.rect, hasText).contains(me->position().toPoint()))
            return false;
        // Swallow press and double-click inside the box so they neither start an
        // edit nor toggle twice; the release does the toggle.
        if (event->type() != QEvent::MouseButtonRelease)
            return true;
        break;
    }
    case QEvent::KeyPress: {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key != Qt::Key_Space && key != Qt::Key_Select)
            return false;
        break;
    }
    default:
        return false;
    }

    Qt::CheckState state = static_cast<Qt::CheckState>(value.toInt());
    if (flags & Qt::ItemIsUserTristate)
        state = static_cast<Qt::CheckState>((state + 1) % 3);
    else
        state = (state == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
    return model->setData(index, state, Qt::CheckStateRole);
}
