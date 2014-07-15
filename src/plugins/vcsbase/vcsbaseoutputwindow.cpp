/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "vcsbaseoutputwindow.h"

#include <coreplugin/editormanager/editormanager.h>

#include <utils/fileutils.h>
#include <utils/outputformatter.h>

#include <QPlainTextEdit>
#include <QTextCharFormat>
#include <QContextMenuEvent>
#include <QTextBlock>
#include <QMenu>
#include <QAction>
#include <QTextBlockUserData>

#include <QPointer>
#include <QTextCodec>
#include <QDir>
#include <QTextStream>
#include <QTime>
#include <QPoint>
#include <QFileInfo>

/*!
    \class VcsBase::VcsBaseOutputWindow

    \brief The VcsBaseOutputWindow class is an output window for Version Control
    System commands and other output (Singleton).

    Installed by the base plugin and accessible for the other plugins
    via static instance()-accessor. Provides slots to append output with
    special formatting.

    It is possible to associate a repository with plain log text, enabling
    an "Open" context menu action over relative file name tokens in the text
    (absolute paths will also work). This can be used for "status" logs,
    showing modified file names, allowing the user to open them.
*/

namespace VcsBase {

namespace Internal {

// Store repository along with text blocks
class RepositoryUserData : public QTextBlockUserData
{
public:
    explicit RepositoryUserData(const QString &repo) : m_repository(repo) {}
    const QString &repository() const { return m_repository; }

private:
    const QString m_repository;
};

// A plain text edit with a special context menu containing "Clear" and
// and functions to append specially formatted entries.
class OutputWindowPlainTextEdit : public QPlainTextEdit
{
public:
    explicit OutputWindowPlainTextEdit(QWidget *parent = 0);
    ~OutputWindowPlainTextEdit();

    void appendLines(QString const& s, const QString &repository = QString());
    void appendLinesWithStyle(QString const& s, enum VcsBaseOutputWindow::MessageStyle style, const QString &repository = QString());

protected:
    virtual void contextMenuEvent(QContextMenuEvent *event);

private:
    void setFormat(enum VcsBaseOutputWindow::MessageStyle style);
    QString identifierUnderCursor(const QPoint &pos, QString *repository = 0) const;

    const QTextCharFormat m_defaultFormat;
    QTextCharFormat m_errorFormat;
    QTextCharFormat m_warningFormat;
    QTextCharFormat m_commandFormat;
    QTextCharFormat m_messageFormat;
    Utils::OutputFormatter *m_formatter;
};

OutputWindowPlainTextEdit::OutputWindowPlainTextEdit(QWidget *parent) :
    QPlainTextEdit(parent),
    m_defaultFormat(currentCharFormat()),
    m_errorFormat(m_defaultFormat),
    m_warningFormat(m_defaultFormat),
    m_commandFormat(m_defaultFormat),
    m_messageFormat(m_defaultFormat)
{
    setReadOnly(true);
    setUndoRedoEnabled(false);
    setFrameStyle(QFrame::NoFrame);
    m_errorFormat.setForeground(Qt::red);
    m_warningFormat.setForeground(Qt::darkYellow);
    m_commandFormat.setFontWeight(QFont::Bold);
    m_messageFormat.setForeground(Qt::blue);
    m_formatter = new Utils::OutputFormatter;
    m_formatter->setPlainTextEdit(this);
}

OutputWindowPlainTextEdit::~OutputWindowPlainTextEdit()
{
    delete m_formatter;
}

// Search back for beginning of word
static inline int firstWordCharacter(const QString &s, int startPos)
{
    for ( ; startPos >= 0 ; startPos--) {
        if (s.at(startPos).isSpace())
            return startPos + 1;
    }
    return 0;
}

QString OutputWindowPlainTextEdit::identifierUnderCursor(const QPoint &widgetPos, QString *repository) const
{
    if (repository)
        repository->clear();
    // Get the blank-delimited word under cursor. Note that
    // using "SelectWordUnderCursor" does not work since it breaks
    // at delimiters like '/'. Get the whole line
    QTextCursor cursor = cursorForPosition(widgetPos);
    const int cursorDocumentPos = cursor.position();
    cursor.select(QTextCursor::BlockUnderCursor);
    if (!cursor.hasSelection())
        return QString();
    QString block = cursor.selectedText();
    // Determine cursor position within line and find blank-delimited word
    const int cursorPos = cursorDocumentPos - cursor.block().position();
    const int blockSize = block.size();
    if (cursorPos < 0 || cursorPos >= blockSize || block.at(cursorPos).isSpace())
        return QString();
    // Retrieve repository if desired
    if (repository)
        if (QTextBlockUserData *data = cursor.block().userData())
            *repository = static_cast<const RepositoryUserData*>(data)->repository();
    // Find first non-space character of word and find first non-space character past
    const int startPos = firstWordCharacter(block, cursorPos);
    int endPos = cursorPos;
    for ( ; endPos < blockSize && !block.at(endPos).isSpace(); endPos++) ;
    return endPos > startPos ? block.mid(startPos, endPos - startPos) : QString();
}

void OutputWindowPlainTextEdit::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = createStandardContextMenu();
    // Add 'open file'
    QString repository;
    const QString token = identifierUnderCursor(event->pos(), &repository);
    QAction *openAction = 0;
    if (!token.isEmpty()) {
        // Check for a file, expand via repository if relative
        QFileInfo fi(token);
        if (!repository.isEmpty() && !fi.isFile() && fi.isRelative())
            fi = QFileInfo(repository + QLatin1Char('/') + token);
        if (fi.isFile())  {
            menu->addSeparator();
            openAction = menu->addAction(VcsBaseOutputWindow::tr("Open \"%1\"").
                                         arg(QDir::toNativeSeparators(fi.fileName())));
            openAction->setData(fi.absoluteFilePath());
        }
    }
    // Add 'clear'
    menu->addSeparator();
    QAction *clearAction = menu->addAction(VcsBaseOutputWindow::tr("Clear"));

    // Run
    QAction *action = menu->exec(event->globalPos());
    if (action) {
        if (action == clearAction) {
            clear();
            return;
        }
        if (action == openAction) {
            const QString fileName = action->data().toString();
            Core::EditorManager::openEditor(fileName);
        }
    }
    delete menu;
}

void OutputWindowPlainTextEdit::appendLines(QString const& s, const QString &repository)
{
    if (s.isEmpty())
        return;

    const int previousLineCount = document()->lineCount();

    const QChar newLine(QLatin1Char('\n'));
    const QChar lastChar = s.at(s.size() - 1);
    const bool appendNewline = (lastChar != QLatin1Char('\r') && lastChar != newLine);
    m_formatter->appendMessage(appendNewline ? s + newLine : s, currentCharFormat());

    // Scroll down
    moveCursor(QTextCursor::End);
    ensureCursorVisible();
    if (!repository.isEmpty()) {
        // Associate repository with new data.
        QTextBlock block = document()->findBlockByLineNumber(previousLineCount);
        for ( ; block.isValid(); block = block.next())
            block.setUserData(new RepositoryUserData(repository));
    }
}

void OutputWindowPlainTextEdit::appendLinesWithStyle(QString const& s, enum VcsBaseOutputWindow::MessageStyle style, const QString &repository)
{
    setFormat(style);

    if (style == VcsBaseOutputWindow::Command) {
        const QString timeStamp = QTime::currentTime().toString(QLatin1String("\nHH:mm "));
        appendLines(timeStamp + s, repository);
    }
    else {
        appendLines(s, repository);
    }

    setCurrentCharFormat(m_defaultFormat);
}

void OutputWindowPlainTextEdit::setFormat(enum VcsBaseOutputWindow::MessageStyle style)
{
    switch (style) {
    case VcsBaseOutputWindow::Warning:
        setCurrentCharFormat(m_warningFormat);
        break;
    case VcsBaseOutputWindow::Error:
        setCurrentCharFormat(m_errorFormat);
        break;
    case VcsBaseOutputWindow::Message:
        setCurrentCharFormat(m_messageFormat);
        break;
    case VcsBaseOutputWindow::Command:
        setCurrentCharFormat(m_commandFormat);
        break;
    default:
    case VcsBaseOutputWindow::None:
        setCurrentCharFormat(m_defaultFormat);
        break;
    }
}

} // namespace Internal

// ------------------- VcsBaseOutputWindowPrivate
class VcsBaseOutputWindowPrivate
{
public:
    static VcsBaseOutputWindow *instance;
    Internal::OutputWindowPlainTextEdit *plainTextEdit();

    QPointer<Internal::OutputWindowPlainTextEdit> m_plainTextEdit;
    QString repository;
    QRegExp passwordRegExp;
};

// Create log editor on demand. Some errors might be logged
// before CorePlugin::extensionsInitialized() pulls up the windows.

Internal::OutputWindowPlainTextEdit *VcsBaseOutputWindowPrivate::plainTextEdit()
{
    if (!m_plainTextEdit)
        m_plainTextEdit = new Internal::OutputWindowPlainTextEdit();
    return m_plainTextEdit;
}

VcsBaseOutputWindow *VcsBaseOutputWindowPrivate::instance = 0;

VcsBaseOutputWindow::VcsBaseOutputWindow() :
    d(new VcsBaseOutputWindowPrivate)
{
    d->passwordRegExp = QRegExp(QLatin1String("://([^@:]+):([^@]+)@"));
    Q_ASSERT(d->passwordRegExp.isValid());
    VcsBaseOutputWindowPrivate::instance = this;
}

QString VcsBaseOutputWindow::filterPasswordFromUrls(const QString &input)
{
    int pos = 0;
    QString result = input;
    while ((pos = d->passwordRegExp.indexIn(result, pos)) >= 0) {
        QString tmp = result.left(pos + 3) + d->passwordRegExp.cap(1) + QLatin1String(":***@");
        int newStart = tmp.count();
        tmp += result.mid(pos + d->passwordRegExp.matchedLength());
        result = tmp;
        pos = newStart;
    }
    return result;
}

VcsBaseOutputWindow::~VcsBaseOutputWindow()
{
    VcsBaseOutputWindowPrivate::instance = 0;
    delete d;
}

QWidget *VcsBaseOutputWindow::outputWidget(QWidget *parent)
{
    if (d->m_plainTextEdit) {
        if (parent != d->m_plainTextEdit->parent())
            d->m_plainTextEdit->setParent(parent);
    } else {
        d->m_plainTextEdit = new Internal::OutputWindowPlainTextEdit(parent);
    }
    return d->m_plainTextEdit;
}

QWidgetList VcsBaseOutputWindow::toolBarWidgets() const
{
    return QWidgetList();
}

QString VcsBaseOutputWindow::displayName() const
{
    return tr("Version Control");
}

int VcsBaseOutputWindow::priorityInStatusBar() const
{
    return -1;
}

void VcsBaseOutputWindow::clearContents()
{
    if (d->m_plainTextEdit)
        d->m_plainTextEdit->clear();
}

void VcsBaseOutputWindow::visibilityChanged(bool visible)
{
    if (visible && d->m_plainTextEdit)
        d->m_plainTextEdit->setFocus();
}

void VcsBaseOutputWindow::setFocus()
{
}

bool VcsBaseOutputWindow::hasFocus() const
{
    return false;
}

bool VcsBaseOutputWindow::canFocus() const
{
    return false;
}

bool VcsBaseOutputWindow::canNavigate() const
{
    return false;
}

bool VcsBaseOutputWindow::canNext() const
{
    return false;
}

bool VcsBaseOutputWindow::canPrevious() const
{
    return false;
}

void VcsBaseOutputWindow::goToNext()
{
}

void VcsBaseOutputWindow::goToPrev()
{
}

void VcsBaseOutputWindow::setText(const QString &text)
{
    d->plainTextEdit()->setPlainText(text);
}

void VcsBaseOutputWindow::setData(const QByteArray &data)
{
    setText(QTextCodec::codecForLocale()->toUnicode(data));
}

void VcsBaseOutputWindow::appendSilently(const QString &text)
{
    append(text, None, true);
}

void VcsBaseOutputWindow::append(const QString &text, enum MessageStyle style, bool silently)
{
    d->plainTextEdit()->appendLinesWithStyle(text, style, d->repository);

    if (!silently && !d->plainTextEdit()->isVisible())
        popup(Core::IOutputPane::NoModeSwitch);
}

void VcsBaseOutputWindow::appendError(const QString &text)
{
    append(text, Error, false);
}

void VcsBaseOutputWindow::appendWarning(const QString &text)
{
    append(text, Warning, false);
}

// Helper to format arguments for log windows hiding common password
// options.
static inline QString formatArguments(const QStringList &args)
{
    const char passwordOptionC[] = "--password";

    QString rc;
    QTextStream str(&rc);
    const int size = args.size();
    // Skip authentication options
    for (int i = 0; i < size; i++) {
        const QString &arg = args.at(i);
        if (i)
            str << ' ';
        str << arg;
        if (arg == QLatin1String(passwordOptionC)) {
            str << " ********";
            i++;
        }
    }
    return rc;
}

QString VcsBaseOutputWindow::msgExecutionLogEntry(const QString &workingDir,
                                                  const Utils::FileName &executable,
                                                  const QStringList &arguments)
{
    const QString args = formatArguments(arguments);
    const QString nativeExecutable = executable.toUserOutput();
    if (workingDir.isEmpty())
        return tr("Executing: %1 %2").arg(nativeExecutable, args) + QLatin1Char('\n');
    return tr("Executing in %1: %2 %3").
            arg(QDir::toNativeSeparators(workingDir), nativeExecutable, args) + QLatin1Char('\n');
}

void VcsBaseOutputWindow::appendCommand(const QString &text)
{
    append(filterPasswordFromUrls(text), Command, true);
}

void VcsBaseOutputWindow::appendCommand(const QString &workingDirectory,
                                        const Utils::FileName &binary,
                                        const QStringList &args)
{
    appendCommand(msgExecutionLogEntry(workingDirectory, binary, args));
}

void VcsBaseOutputWindow::appendMessage(const QString &text)
{
    append(text, Message, true);
}

VcsBaseOutputWindow *VcsBaseOutputWindow::instance()
{
    if (!VcsBaseOutputWindowPrivate::instance) {
        VcsBaseOutputWindow *w = new VcsBaseOutputWindow;
        Q_UNUSED(w)
    }
    return VcsBaseOutputWindowPrivate::instance;
}

QString VcsBaseOutputWindow::repository() const
{
    return d->repository;
}

void VcsBaseOutputWindow::setRepository(const QString &r)
{
    d->repository = r;
}

void VcsBaseOutputWindow::clearRepository()
{
    d->repository.clear();
}

} // namespace VcsBase
