/*  SPDX-License-Identifier: LGPL-2.0-or-later

    SPDX-FileCopyrightText: 2007, 2009 Joseph Wenninger <jowenn@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "katequickopen.h"
#include "katequickopenmodel.h"

#include "kateapp.h"
#include "katemainwindow.h"
#include "kateviewmanager.h"

#include <ktexteditor/document.h>
#include <ktexteditor/view.h>

#include <KAboutData>
#include <KActionCollection>
#include <KLineEdit>
#include <KLocalizedString>
#include <KPluginFactory>

#include <QBoxLayout>
#include <QCoreApplication>
#include <QDesktopWidget>
#include <QEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QTextDocument>
#include <QPainter>

#include <kfts_fuzzy_match.h>

class QuickOpenFilterProxyModel : public QSortFilterProxyModel
{

public:
    QuickOpenFilterProxyModel(QObject *parent = nullptr) : QSortFilterProxyModel(parent)
    {}

protected:
    bool lessThan(const QModelIndex &sourceLeft, const QModelIndex &sourceRight) const override
    {
        int l = sourceLeft.data(KateQuickOpenModel::Score).toInt();
        int r = sourceRight.data(KateQuickOpenModel::Score).toInt();
        return l < r;
    }

    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (pattern.isEmpty())
            return true;
        const QString fileName = sourceModel()->index(sourceRow, 0, sourceParent).data().toString();

        // match
        int score = 0;
        auto res = kfts::fuzzy_match(pattern, fileName, score);

        // store the score for sorting later
        auto idx = sourceModel()->index(sourceRow, 0, sourceParent);
        sourceModel()->setData(idx, score, KateQuickOpenModel::Score);

        return res;
    }

public Q_SLOTS:
    void setFilterText(const QString& text)
    {
        beginResetModel();
        pattern = text;
        endResetModel();
    }

private:
    QString pattern;
};

class QuickOpenStyleDelegate : public QStyledItemDelegate {

public:
    QuickOpenStyleDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem options = option;
        initStyleOption(&options, index);

        QTextDocument doc;

        QString str = index.data().toString();

        kfts::to_fuzzy_matched_display_string(m_filterString, str, QStringLiteral("<b>"), QStringLiteral("</b>"));

        doc.setHtml(str);
        doc.setDocumentMargin(2);

        painter->save();

        // paint background
        if (option.state & QStyle::State_Selected) {
            painter->fillRect(option.rect, option.palette.highlight());
        } else {
            painter->fillRect(option.rect, option.palette.base());
        }

        options.text = QString(); // clear old text
        options.widget->style()->drawControl(QStyle::CE_ItemViewItem, &options, painter, options.widget);

        // draw text
        painter->translate(option.rect.x(), option.rect.y());
        doc.drawContents(painter);

        painter->restore();
    }

public Q_SLOTS:
    void setFilterString(const QString& text)
    {
        m_filterString = text;
    }

private:
    QString m_filterString;
};

Q_DECLARE_METATYPE(QPointer<KTextEditor::Document>)

KateQuickOpen::KateQuickOpen(QWidget *parent, KateMainWindow *mainWindow)
    : QWidget(parent)
    , m_mainWindow(mainWindow)
{
    QVBoxLayout *layout = new QVBoxLayout();
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);

    m_inputLine = new KLineEdit();
    setFocusProxy(m_inputLine);
    m_inputLine->setPlaceholderText(i18n("Quick Open Search"));

    layout->addWidget(m_inputLine);

    m_listView = new QTreeView();
    layout->addWidget(m_listView, 1);
    m_listView->setTextElideMode(Qt::ElideLeft);

    m_base_model = new KateQuickOpenModel(m_mainWindow, this);

    m_model = new QuickOpenFilterProxyModel(this);
    m_model->setFilterRole(Qt::DisplayRole);
    m_model->setSortRole(KateQuickOpenModel::Score);
    m_model->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_model->setSortCaseSensitivity(Qt::CaseInsensitive);
    m_model->setFilterKeyColumn(Qt::DisplayRole);

    QuickOpenStyleDelegate* delegate = new QuickOpenStyleDelegate(this);
    m_listView->setItemDelegateForColumn(0, delegate);

    connect(m_inputLine, &KLineEdit::textChanged, m_model, &QuickOpenFilterProxyModel::setFilterText);
    connect(m_inputLine, &KLineEdit::textChanged, delegate, &QuickOpenStyleDelegate::setFilterString);
    connect(m_inputLine, &KLineEdit::textChanged, this, [this](){ m_listView->viewport()->update(); });
    connect(m_inputLine, &KLineEdit::returnPressed, this, &KateQuickOpen::slotReturnPressed);
    connect(m_model, &QSortFilterProxyModel::rowsInserted, this, &KateQuickOpen::reselectFirst);
    connect(m_model, &QSortFilterProxyModel::rowsRemoved, this, &KateQuickOpen::reselectFirst);

    connect(m_listView, &QTreeView::activated, this, &KateQuickOpen::slotReturnPressed);

    m_listView->setModel(m_model);
    m_listView->setSortingEnabled(true);
    m_model->setSourceModel(m_base_model);

    m_inputLine->installEventFilter(this);
    m_listView->installEventFilter(this);
    m_listView->setHeaderHidden(true);
    m_listView->setRootIsDecorated(false);
}

bool KateQuickOpen::eventFilter(QObject *obj, QEvent *event)
{
    // catch key presses + shortcut overrides to allow to have ESC as application wide shortcut, too, see bug 409856
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (obj == m_inputLine) {
            const bool forward2list = (keyEvent->key() == Qt::Key_Up) || (keyEvent->key() == Qt::Key_Down) || (keyEvent->key() == Qt::Key_PageUp) || (keyEvent->key() == Qt::Key_PageDown);
            if (forward2list) {
                QCoreApplication::sendEvent(m_listView, event);
                return true;
            }

            if (keyEvent->key() == Qt::Key_Escape) {
                m_mainWindow->slotWindowActivated();
                m_inputLine->clear();
                keyEvent->accept();
                return true;
            }
        } else {
            const bool forward2input = (keyEvent->key() != Qt::Key_Up) && (keyEvent->key() != Qt::Key_Down) && (keyEvent->key() != Qt::Key_PageUp) && (keyEvent->key() != Qt::Key_PageDown) && (keyEvent->key() != Qt::Key_Tab) &&
                (keyEvent->key() != Qt::Key_Backtab);
            if (forward2input) {
                QCoreApplication::sendEvent(m_inputLine, event);
                return true;
            }
        }
    }

    // hide on focus out, if neither input field nor list have focus!
    else if (event->type() == QEvent::FocusOut && !(m_inputLine->hasFocus() || m_listView->hasFocus())) {
        m_mainWindow->slotWindowActivated();
        m_inputLine->clear();
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

void KateQuickOpen::reselectFirst()
{
    int first = 0;
    if (m_mainWindow->viewManager()->sortedViews().size() > 1 && m_model->rowCount() > 1)
        first = 1;

    QModelIndex index = m_model->index(first, 0);
    m_listView->setCurrentIndex(index);
}

void KateQuickOpen::update()
{
    m_base_model->refresh();
    m_listView->resizeColumnToContents(0);

    // If we have a very long file name we restrict the size of the first column
    // to take at most half of the space. Otherwise it would look odd.
    int colw0 = m_listView->header()->sectionSize(0); // file name
    int colw1 = m_listView->header()->sectionSize(1); // file path
    if (colw0 > colw1) {
        m_listView->setColumnWidth(0, (colw0 + colw1) / 2);
    }
    reselectFirst();
}

void KateQuickOpen::slotReturnPressed()
{
    const auto index = m_listView->model()->index(m_listView->currentIndex().row(), KateQuickOpenModel::Columns::FilePath);
    auto url = index.data(Qt::UserRole).toUrl();
    m_mainWindow->wrapper()->openUrl(url);
    m_mainWindow->slotWindowActivated();
    m_inputLine->clear();
}

void KateQuickOpen::setMatchMode(int mode)
{
    m_model->setFilterKeyColumn(mode);
}

int KateQuickOpen::matchMode()
{
    return m_model->filterKeyColumn();
}

void KateQuickOpen::setListMode(KateQuickOpenModel::List mode)
{
    m_base_model->setListMode(mode);
}

KateQuickOpenModel::List KateQuickOpen::listMode() const
{
    return m_base_model->listMode();
}
