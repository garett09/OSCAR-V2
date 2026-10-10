/* ComboCheckDelegate — styled checkbox delegate for QComboBox dropdowns.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * Items with Qt::CheckStateRole set receive a 16px rounded checkbox indicator
 * (matching the context-menu style); items without it are drawn normally. */

#pragma once

#include <QPainter>
#include <QPixmap>
#include <QStyledItemDelegate>

/*! \brief Draws OSCAR's style-independent checkbox indicator into \a rect.
 *
 * Rounded box with a grey outline and white fill when unchecked, or a blue fill
 * with a white tick when checked. Fixed colours keep it visible regardless of the
 * platform style or palette (the native macOS 27 style draws no item-view
 * indicators at all). Shared by ComboCheckDelegate and TreeCheckDelegate. */
inline void drawOscarCheckBox(QPainter *painter, const QRect &rect, bool checked)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(checked ? QColor("#005a9e") : QColor("#888888"), 1));
    painter->setBrush(checked ? QColor("#0078d4") : Qt::white);
    painter->drawRoundedRect(rect, 3, 3);

    if (checked) {
        static const QPixmap tick(":/icons/white_tick.png");
        if (!tick.isNull()) {
            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter->drawPixmap(rect.adjusted(2, 2, -2, -2), tick);
        }
    }
    painter->restore();
}

class ComboCheckDelegate : public QStyledItemDelegate
{
public:
    explicit ComboCheckDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QVariant checkState = index.data(Qt::CheckStateRole);
        if (!checkState.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        painter->save();

        // Full-width background
        if (opt.state & QStyle::State_Selected)
            painter->fillRect(opt.rect, opt.palette.highlight());
        else
            painter->fillRect(opt.rect, opt.palette.base());

        // Checkbox indicator
        const int pad = 5;
        const int sz  = 16;
        int yc = opt.rect.center().y();
        QRect ir(opt.rect.left() + pad, yc - sz / 2, sz, sz);

        drawOscarCheckBox(painter, ir, checkState.toInt() == Qt::Checked);

        // Text
        QRect textRect = opt.rect.adjusted(pad + sz + pad, 0, -pad, 0);
        painter->setFont(opt.font);
        painter->setPen((opt.state & QStyle::State_Selected)
                        ? opt.palette.highlightedText().color()
                        : opt.palette.text().color());
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, opt.text);

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        if (index.data(Qt::CheckStateRole).isValid())
            s.setHeight(qMax(s.height(), 26));
        return s;
    }
};
