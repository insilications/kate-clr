/* This file is part of the KDE libraries
   Copyright (C) 2001 Joseph Wenninger <jowenn@kde.org>
   Copyright (C) 2002 John Firebaugh <jfirebaugh@kde.org>
   Copyright (C) 2001 by Victor Röder <Victor_Roeder@GMX.de>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

/******** Partly based on the ArgHintWidget of Qt3 by Trolltech AS *********/
/* Trolltech doesn't mind, if we license that piece of code as LGPL, because there isn't much
 * left from the desigener code */

#include "katecodecompletion.h"
#include "katecodecompletion.moc"

#include "katecodecompletion_arghint.h"
#include "katedocument.h"
#include "kateview.h"
#include "katerenderer.h"
#include "kateconfig.h"

#include <kdebug.h>

#include <qwhatsthis.h>
#include <qvbox.h>
#include <qlistbox.h>
#include <qtimer.h>
#include <qtooltip.h>
#include <qapplication.h>
#include <qsizegrip.h>
#include <qfontmetrics.h>
/**
 * This class is used as the codecompletion listbox. It can be resized according to its contents,
 *  therfor the needed size is provided by sizeHint();
 *@short Listbox showing codecompletion
 *@author Jonas B. Jacobi <j.jacobi@gmx.de>
 */
class CCListBox : public QListBox{
public:
  /**
    @short Create a new CCListBox
    @param view The KateView, CCListBox is displayed in
   */
    CCListBox(KateView* view, QWidget* parent = 0, const char* name = 0, WFlags f = 0):QListBox(parent, name, f), m_view(view){
    };

    QSize sizeHint()  const {
        int count = this->count();
        int height = 20;
        int tmpwidth = 8;
        //FIXME the height is for some reasons at least 3 items heigh, even if there is only one item in the list
        if (count > 0)
            if(count < 11)
                height =  count * itemHeight(0);
            else  {
                height = 10 * itemHeight(0);
                tmpwidth += verticalScrollBar()->width();
            }

        int maxcount = 0, tmpcount = 0;
        for (int i = 0; i < count; ++i)
            if ( (tmpcount = fontMetrics().width(text(i)) ) > maxcount)
                    maxcount = tmpcount;

        if (maxcount > QApplication::desktop()->width()){
            tmpwidth = QApplication::desktop()->width() - 5;
            height += horizontalScrollBar()->height();
        } else
            tmpwidth += maxcount;
        return QSize(tmpwidth,height);

    };

private:
  KateView* m_view;

};

class CompletionItem : public QListBoxText
{
public:
  CompletionItem( QListBox* lb, KTextEditor::CompletionEntry entry )
    : QListBoxText( lb )
    , m_entry( entry )
  {
    if( entry.postfix == "()" ) { // should be configurable
      setText( entry.prefix + " " + entry.text + entry.postfix );
    } else {
      setText( entry.prefix + " " + entry.text + " " + entry.postfix);
    }
  }

  KTextEditor::CompletionEntry m_entry;
};


KateCodeCompletion::KateCodeCompletion( KateView* view )
  : QObject( view, "Kate Code Completion" )
  , m_view( view )
  , m_commentLabel( 0 )
{
  m_completionPopup = new QVBox( 0, 0, WType_Popup );
  m_completionPopup->setFrameStyle( QFrame::Box | QFrame::Plain );
  m_completionPopup->setLineWidth( 1 );

  m_completionListBox = new CCListBox( view,  m_completionPopup );
  m_completionListBox->setFrameStyle( QFrame::NoFrame );
  m_completionListBox->setCornerWidget( new QSizeGrip( m_completionListBox) );

  m_completionListBox->installEventFilter( this );

  m_completionPopup->resize(m_completionListBox->sizeHint() + QSize(2,2));
  m_completionPopup->installEventFilter( this );
  m_completionPopup->setFocusProxy( m_completionListBox );

  m_pArgHint = new KDevArgHint( m_view );
  connect( m_pArgHint, SIGNAL(argHintHidden()),
           this, SIGNAL(argHintHidden()) );

  connect( m_view, SIGNAL(cursorPositionChanged()),
           this, SLOT(slotCursorPosChanged()) );
}

bool KateCodeCompletion::codeCompletionVisible () {
  return m_completionPopup->isVisible();
}

void KateCodeCompletion::showCompletionBox(
    QValueList<KTextEditor::CompletionEntry> complList, int offset, bool casesensitive )
{
  emit aboutToShowCompletionBox();

  kdDebug(13035) << "showCompletionBox " << endl;

  m_caseSensitive = casesensitive;
  m_complList = complList;
  m_offset = offset;
  m_view->cursorPositionReal( &m_lineCursor, &m_colCursor );
  m_colCursor -= offset;

  updateBox( true );
}

bool KateCodeCompletion::eventFilter( QObject *o, QEvent *e )
{
  if ( o != m_completionPopup &&
       o != m_completionListBox &&
       o != m_completionListBox->viewport() )
    return false;

   if ( e->type() == QEvent::MouseButtonDblClick  ) {
    doComplete();
    return false;
   }

   if ( e->type() == QEvent::MouseButtonPress ) {
    QTimer::singleShot(0, this, SLOT(showComment()));
    return false;
   }

   if ( e->type() == QEvent::KeyPress ) {
    QKeyEvent *ke = (QKeyEvent*)e;
    if( /*(ke->key() == Key_Left)  || (ke->key() == Key_Right) ||*///what are <- and -> used for??
        (ke->key() == Key_Up)    || (ke->key() == Key_Down ) ||
        (ke->key() == Key_Home ) || (ke->key() == Key_End)   ||
        (ke->key() == Key_Prior) || (ke->key() == Key_Next )) {
      QTimer::singleShot(0,this,SLOT(showComment()));
      return false;
    }
    if( ke->key() == Key_Enter || ke->key() == Key_Return ) {
      doComplete();
      return false;
    }

    if( ke->key() == Key_Escape ) {
      abortCompletion();
      m_view->setFocus();
      return false;
    }

    // redirect the event to the editor
    if( ke->key() == Key_Backspace) {
      m_view->backspace();
    } else {
      QApplication::sendEvent( m_view->m_viewInternal, e );
    }
    if( m_colCursor > m_view->cursorColumnReal() ) {
      // the cursor is too far left
      kdDebug(13035) << "Aborting Codecompletion after sendEvent" << endl;
      kdDebug(13035) << m_view->cursorColumnReal() << endl;
      abortCompletion();
      m_view->setFocus();
      return true;
    }
    updateBox();
    return true;
  }

  if( e->type() == QEvent::FocusOut )
    abortCompletion();
  return false;
}

void KateCodeCompletion::doComplete()
{
  CompletionItem* item = static_cast<CompletionItem*>(
     m_completionListBox->item(m_completionListBox->currentItem()));

  if( item == 0 )
    return;

  QString text = item->m_entry.text;
  QString currentLine = m_view->currentTextLine();
  int len = m_view->cursorColumnReal() - m_colCursor;
  QString currentComplText = currentLine.mid(m_colCursor,len);
  QString add = text.mid(currentComplText.length());
  if( item->m_entry.postfix == "()" )
    add += "(";

  emit filterInsertString(&(item->m_entry),&add);
  m_view->insertText(add);

  complete( item->m_entry );
  m_view->setFocus();
}

void KateCodeCompletion::abortCompletion()
{
  m_completionPopup->hide();
  delete m_commentLabel;
  m_commentLabel = 0;
  emit completionAborted();
}

void KateCodeCompletion::complete( KTextEditor::CompletionEntry entry )
{
  m_completionPopup->hide();
  delete m_commentLabel;
  m_commentLabel = 0;
  emit completionDone( entry );
  emit completionDone();
}

void KateCodeCompletion::updateBox( bool newCoordinate )
{
  m_completionListBox->clear();

  QString currentLine = m_view->currentTextLine();
  int len = m_view->cursorColumnReal() - m_colCursor;
  QString currentComplText = currentLine.mid(m_colCursor,len);
/* No-one really badly wants those, or?
  kdDebug(13035) << "Column: " << m_colCursor << endl;
  kdDebug(13035) << "Line: " << currentLine << endl;
  kdDebug(13035) << "CurrentColumn: " << m_view->cursorColumnReal() << endl;
  kdDebug(13035) << "Len: " << len << endl;
  kdDebug(13035) << "Text: '" << currentComplText << "'" << endl;
  kdDebug(13035) << "Count: " << m_complList.count() << endl;
*/
  QValueList<KTextEditor::CompletionEntry>::Iterator it;
  if( m_caseSensitive ) {
    for( it = m_complList.begin(); it != m_complList.end(); ++it ) {
      if( (*it).text.startsWith(currentComplText) ) {
        new CompletionItem(m_completionListBox,*it);
      }
    }
  } else {
    currentComplText = currentComplText.upper();
    for( it = m_complList.begin(); it != m_complList.end(); ++it ) {
      if( (*it).text.upper().startsWith(currentComplText) ) {
        new CompletionItem(m_completionListBox,*it);
      }
    }
  }

  if( m_completionListBox->count() == 0 ||
      ( m_completionListBox->count() == 1 && // abort if we equaled the last item
        currentComplText == m_completionListBox->text(0).stripWhiteSpace() ) ) {
    abortCompletion();
    m_view->setFocus();
    return;
  }

    kdDebug(13035)<<"KateCodeCompletion::updateBox: Resizing widget"<<endl;
        m_completionPopup->resize(m_completionListBox->sizeHint() + QSize(2,2));
    QPoint p = m_view->mapToGlobal( m_view->cursorCoordinates() );
        int x = p.x();
        int y = p.y() ;
        if ( y + m_completionPopup->height() + m_view->renderer()->config()->fontMetrics( KateRendererConfig::ViewFont )->height() > QApplication::desktop()->height() )
                y -= (m_completionPopup->height() );
        else
                y += m_view->renderer()->config()->fontMetrics( KateRendererConfig::ViewFont )->height();

        if (x + m_completionPopup->width() > QApplication::desktop()->width())
                x = QApplication::desktop()->width() - m_completionPopup->width();

        m_completionPopup->move( QPoint(x,y) );

  m_completionListBox->setCurrentItem( 0 );
  m_completionListBox->setSelected( 0, true );
  m_completionListBox->setFocus();
  m_completionPopup->show();

  QTimer::singleShot(0,this,SLOT(showComment()));
}

void KateCodeCompletion::showArgHint ( QStringList functionList, const QString& strWrapping, const QString& strDelimiter )
{
  unsigned int line, col;
  m_view->cursorPositionReal( &line, &col );
  m_pArgHint->reset( line, col );
  m_pArgHint->setArgMarkInfos( strWrapping, strDelimiter );

  int nNum = 0;
  for( QStringList::Iterator it = functionList.begin(); it != functionList.end(); it++ )
  {
    kdDebug(13035) << "Insert function text: " << *it << endl;

    m_pArgHint->addFunction( nNum, ( *it ) );

    nNum++;
  }

  m_pArgHint->move(m_view->mapToGlobal(m_view->cursorCoordinates() + QPoint(0,m_view->renderer()->config()->fontMetrics( KateRendererConfig::ViewFont )->height())) );
  m_pArgHint->show();
}

void KateCodeCompletion::slotCursorPosChanged()
{
  m_pArgHint->cursorPositionChanged ( m_view, m_view->cursorLine(), m_view->cursorColumnReal() );
}

void KateCodeCompletion::showComment()
{
  CompletionItem* item = static_cast<CompletionItem*>(m_completionListBox->item(m_completionListBox->currentItem()));

  if( !item )
    return;
  if( item->m_entry.comment.isEmpty() )
    return;

  delete m_commentLabel;
  m_commentLabel = new KateCodeCompletionCommentLabel( 0, item->m_entry.comment );
  m_commentLabel->setFont(QToolTip::font());
  m_commentLabel->setPalette(QToolTip::palette());

  QPoint rightPoint = m_completionPopup->mapToGlobal(QPoint(m_completionPopup->width(),0));
  QPoint leftPoint = m_completionPopup->mapToGlobal(QPoint(0,0));
  QRect screen = QApplication::desktop()->screenGeometry( m_commentLabel->x11Screen() );
  QPoint finalPoint;
  if (rightPoint.x()+m_commentLabel->width() > screen.x() + screen.width())
    finalPoint.setX(leftPoint.x()-m_commentLabel->width());
  else
    finalPoint.setX(rightPoint.x());

  m_completionListBox->ensureCurrentVisible();

  finalPoint.setY(
    m_completionListBox->viewport()->mapToGlobal(m_completionListBox->itemRect(
      m_completionListBox->item(m_completionListBox->currentItem())).topLeft()).y());

  m_commentLabel->move(finalPoint);
  m_commentLabel->show();
}

// kate: space-indent on; indent-width 2; replace-tabs on;
