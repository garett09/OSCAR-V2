/* TreeCheckDelegate — style-independent checkbox delegate for tree/list views.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#pragma once

#include <QStyledItemDelegate>

/*! \class TreeCheckDelegate
 *  \brief Draws and toggles user-checkable items without relying on the platform style.
 *
 * On macOS 27 the native style draws no check indicators in item views (QTBUG-150017),
 * and the boxes were also reported invisible there under Fusion, so the Preferences
 * Events/Waveforms trees showed empty Name and Overview cells (GitLab #318).
 *
 * This delegate paints the indicator itself with drawOscarCheckBox() and performs its
 * own hit-testing, so the drawn box and the clickable area always coincide. The cell
 * background, selection, text and focus rect are still drawn by the style. Items
 * without Qt::CheckStateRole data fall through to QStyledItemDelegate unchanged.
 *
 * Layout: when the item has text, the box sits at the left with the text after it;
 * when the text is empty (e.g. the Overview column) the box is centred in the cell. */
class TreeCheckDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit TreeCheckDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override;

protected:
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option, const QModelIndex &index) override;

private:
    //! Rectangle of the check box within \a cell; centred when \a hasText is false.
    static QRect checkRect(const QRect &cell, bool hasText);

    //! Area left for text and editors to the right of the check box.
    static QRect textRect(const QRect &cell);
};
