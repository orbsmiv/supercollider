/*
    SuperCollider Qt IDE
    Copyright (c) 2012 Jakob Leben & Tim Blechmann
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "doc_manager.hpp"
#include "main.hpp"
#include "main_window.hpp"
#include "settings/manager.hpp"
#include "../widgets/code_editor/highlighter.hpp"
#include "../../common/SC_TextUtils.hpp"

#include <QPlainTextDocumentLayout>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QTextBlock>
#include <QApplication>

#include <yaml-cpp/node.h>
#include <yaml-cpp/parser.h>

using namespace ScIDE;

Document::Document(bool isPlainText, const QByteArray & id,
                    const QString & title, const QString & text ):
    mId(id),
    mDoc(new QTextDocument(text, this)),
    mTitle(title),
    mIndentWidth(4),
    mHighlighter(0)
{
    if (mId.isEmpty())
        mId = QUuid::createUuid().toString().toLatin1();
    if (mTitle.isEmpty())
        mTitle = tr("Untitled");

    mDoc->setDocumentLayout( new QPlainTextDocumentLayout(mDoc) );

    if (!isPlainText)
        mHighlighter = new SyntaxHighlighter(mDoc);

    connect( Main::instance(), SIGNAL(applySettingsRequest(Settings::Manager*)),
             this, SLOT(applySettings(Settings::Manager*)) );

    applySettings( Main::settings() );
}

void Document::setPlainText(bool set_plain_text)
{
    if (isPlainText() == set_plain_text)
        return;

    delete mHighlighter;
    mHighlighter = 0;

    if (!set_plain_text)
        mHighlighter = new SyntaxHighlighter(mDoc);
}

void Document::applySettings( Settings::Manager *settings )
{
    QFont font = settings->codeFont();
    int indentWidth = settings->value("IDE/editor/indentWidth").toInt();

    setDefaultFont(font);
    setIndentWidth(indentWidth);
}

void Document::deleteTrailingSpaces()
{
    QTextCursor cursor (textDocument());
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::EndOfBlock);
    QTextDocument * doc = textDocument();

    while( !cursor.atEnd() ) {
        while( (cursor.block().length() > 1) && doc->characterAt(cursor.position() - 1).isSpace())
            cursor.deletePreviousChar();

        cursor.movePosition(QTextCursor::NextBlock);
        cursor.movePosition(QTextCursor::EndOfBlock);
    }
    cursor.endEditBlock();
}

void Document::setDefaultFont( const QFont & font )
{
    mDoc->setDefaultFont( font );
    // update tab stop, since it depends on font:
    setIndentWidth( mIndentWidth );
    emit defaultFontChanged();
}

void Document::resetDefaultFont()
{
    Settings::Manager *settings = Main::settings();
    setDefaultFont( settings->codeFont() );
}

void Document::setIndentWidth( int numSpaces )
{
    mIndentWidth = numSpaces;

    QFontMetricsF fontMetrics( mDoc->defaultFont() );
    qreal tabStop = fontMetrics.width(' ') * numSpaces;

    QTextOption options = mDoc->defaultTextOption();
    options.setTabStop(tabStop);
    mDoc->setDefaultTextOption(options);
}

QString Document::textAsSCArrayOfCharCodes(int start = 0, int range = -1)
{
    QTextCursor cursor = QTextCursor(mDoc);
    cursor.setPosition(start, QTextCursor::MoveAnchor);
    if(range == -1){
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor, 1);
    } else {
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, range);
    }
    
    QByteArray stringBytes = cursor.selectedText().replace(QChar(0x2029), QChar('\n')).toUtf8();
    QString returnString = QString("[");
    for (int i = 0; i < stringBytes.size(); ++i) {
        returnString = returnString.append(QString::number(static_cast<int>(stringBytes.at(i)))).append(',');
    }
    returnString = returnString.append(QString("]"));
    return returnString;
}

void Document::setTextInRange(const QString text, int start, int range)
{
    QTextCursor cursor = QTextCursor(mDoc);
    cursor.setPosition(start, QTextCursor::MoveAnchor);
    if(range == -1){
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor, 1);
    } else {
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, range);
    }
    cursor.insertText(text);
}


DocumentManager::DocumentManager( Main *main, Settings::Manager * settings ):
    QObject(main)
{
    connect(&mFsWatcher, SIGNAL(fileChanged(QString)), this, SLOT(onFileChanged(QString)));

    connect(main, SIGNAL(storeSettingsRequest(Settings::Manager*)),
            this, SLOT(storeSettings(Settings::Manager*)));

    loadRecentDocuments( settings );
}

Document * DocumentManager::createDocument(bool isPlainText, const QByteArray & id,
                                           const QString & title, const QString & text  )
{
    Document *doc = new Document( isPlainText, id, title, text );
    mDocHash.insert( doc->id(), doc );
    return doc;
}

void DocumentManager::create()
{
    Document *doc = createDocument();

    Q_EMIT( opened(doc, 0, 0) );
}

Document *DocumentManager::open( const QString & path, int initialCursorPosition, int selectionLength, bool toRecent )
{
    QFileInfo info(path);
    QString cpath = info.canonicalFilePath();
    info.setFile(cpath);

    if (cpath.isEmpty()) {
        MainWindow::instance()->showStatusMessage (
                    tr("Cannot open file: %1 (file does not exist)").arg(path) );
        return 0;
    }

    // Check if file already opened
    for( DocIterator it = mDocHash.begin(); it != mDocHash.end(); ++it ) {
        Document *doc = it.value();
        if(doc->mFilePath == cpath) {
            Q_EMIT( showRequest(doc, initialCursorPosition, selectionLength) );
            if (toRecent) addToRecent(doc);
            return doc;
        }
    }

    // Open the file
    QFile file(cpath);
    if(!file.open(QIODevice::ReadOnly)) {
        MainWindow::instance()->showStatusMessage(
                    tr("Cannot open file for reading: %1").arg(cpath));
        return 0;
    }
    QByteArray bytes( file.readAll() );
    file.close();

    // strip .rtf
    bool isRTF = false;
    QString filePath = cpath;
    if (info.suffix() == QString("rtf")) {
        isRTF = true;

        filePath += QString(".scd");
        int result = rtf2txt(bytes.data());
        bytes = bytes.left(result);
        QMessageBox::warning(NULL, QString(tr("Opening RTF File")),
                             QString(tr("Warning: RTF file will be converted to plain-text scd file.")));
    }

    closeSingleUntitledIfUnmodified();

    const bool fileIsPlainText = !(info.suffix() == QString("sc") ||
                                   (info.suffix() == QString("scd")));

    Document *doc = createDocument( fileIsPlainText );
    doc->mDoc->setPlainText( decodeDocument(bytes) );
    doc->mDoc->setModified(false);
    doc->mFilePath = filePath;
    doc->mTitle = info.fileName();
    doc->mSaveTime = info.lastModified();

    if (!isRTF)
        mFsWatcher.addPath(cpath);

    Q_EMIT( opened(doc, initialCursorPosition, selectionLength) );

    if (toRecent) this->addToRecent(doc);

    return doc;
}

bool DocumentManager::reload( Document *doc )
{
    Q_ASSERT(doc);

    if (doc->mFilePath.isEmpty())
        return false;

    QFile file(doc->mFilePath);
    if(!file.open(QIODevice::ReadOnly)) {
        MainWindow::instance()->showStatusMessage(
                    tr("Cannot open file for reading: %1").arg(doc->mFilePath));
        return false;
    }

    QByteArray bytes( file.readAll() );
    file.close();

    doc->mDoc->setPlainText( decodeDocument(bytes) );
    doc->mDoc->setModified(false);

    QFileInfo info(doc->mFilePath);
    doc->mSaveTime = info.lastModified();

    if (!mFsWatcher.files().contains(doc->mFilePath))
        mFsWatcher.addPath(doc->mFilePath);

    return true;
}

Document * DocumentManager::documentForId(const QByteArray docID)
{
    Document * doc = mDocHash[docID];
    if(!doc) MainWindow::instance()->showStatusMessage(QString("Lookup failed for Document %1").arg(docID.constData()));
    return doc;
}

QString DocumentManager::decodeDocument(const QByteArray & bytes)
{
    QTextStream stream(bytes);
    stream.setCodec("UTF-8");
    stream.setAutoDetectUnicode(true);
    return stream.readAll();
}

void DocumentManager::close( Document *doc )
{
    Q_ASSERT(doc);

    if( mDocHash.remove(doc->id()) == 0 ) {
        qWarning("DocumentManager: trying to close an unmanaged document.");
        return;
    }

    if (!doc->mFilePath.isEmpty())
        mFsWatcher.removePath(doc->mFilePath);

    Q_EMIT( closed(doc) );
    delete doc;
}

bool DocumentManager::save( Document *doc )
{
    Q_ASSERT(doc);

    return doSaveAs( doc, doc->mFilePath );
}

bool DocumentManager::saveAs( Document *doc, const QString & path )
{
    Q_ASSERT(doc);

    if (path.isEmpty()) {
        qWarning() << "DocumentManager: the saving path is empty.";
        return false;
    }

    bool ok = doSaveAs( doc, path );
    if (ok)
        addToRecent(doc);
    return ok;
}

bool DocumentManager::doSaveAs( Document *doc, const QString & path )
{
    Q_ASSERT(doc);

    doc->deleteTrailingSpaces();


    QFile file(path);
    if(!file.open(QIODevice::WriteOnly)) {
        qWarning() << "DocumentManager: the file" << path << "could not be opened for writing.";
        return false;
    }

    QFileInfo info(path);
    QString cpath = info.canonicalFilePath();

    const bool pathChanged = ( !(doc->filePath().isEmpty()) && (cpath != doc->filePath()) );
    if (pathChanged)
        mFsWatcher.removePath(doc->filePath());

    QString str = doc->textDocument()->toPlainText();
    file.write(str.toUtf8());
    file.close();

    info.refresh();

    const bool fileIsPlainText = !(info.suffix() == QString("sc") ||
                                   (info.suffix() == QString("scd")));

    doc->mFilePath = cpath;
    doc->mTitle = info.fileName();
    doc->mDoc->setModified(false);
    doc->mSaveTime = info.lastModified();
    doc->setPlainText(fileIsPlainText);

    // Always try to start watching, because the file could have been removed:
    if (!mFsWatcher.files().contains(cpath))
        mFsWatcher.addPath(cpath);

    Q_EMIT(saved(doc));

    return true;
}

void DocumentManager::onFileChanged( const QString & path )
{
    DocIterator it;
    for( it = mDocHash.begin(); it != mDocHash.end(); ++it )
    {
        Document *doc = it.value();
        if (doc->mFilePath == path) {
            QFileInfo info(doc->mFilePath);
            if (doc->mSaveTime < info.lastModified()) {
                doc->mDoc->setModified(true);
                doc->mSaveTime = info.lastModified();
                emit changedExternally(doc);
            }
        }
    }
}

void DocumentManager::addToRecent( Document *doc )
{
    const QString &path = doc->mFilePath;
    int i = mRecent.indexOf(path);
    if (i != -1)
        mRecent.move( i, 0 );
    else {
        mRecent.prepend(path);
        if (mRecent.count() > mMaxRecent)
            mRecent.removeLast();
    }

    emit recentsChanged();
}

void DocumentManager::clearRecents()
{
    mRecent.clear();
    emit recentsChanged();
}

void DocumentManager::loadRecentDocuments( Settings::Manager *settings )
{
    QVariantList list = settings->value("IDE/recentDocuments").value<QVariantList>();
    mRecent.clear();
    foreach (const QVariant & var, list) {
        QString filePath = var.toString();
        if (QFile::exists(filePath))
            mRecent << filePath;
    }
}

void DocumentManager::storeSettings( Settings::Manager *settings )
{
    QVariantList list;
    foreach (const QString & path, mRecent)
        list << QVariant(path);

    settings->setValue("IDE/recentDocuments", QVariant::fromValue<QVariantList>(list));
}

void DocumentManager::closeSingleUntitledIfUnmodified()
{
    QList<Document*> openDocuments = documents();

    if (openDocuments.size() == 1) {
        Document * document = openDocuments.front();
        if (document->filePath().isEmpty() && !document->isModified())
            close(document);
    }
}

void DocumentManager::handleScLangMessage( const QString &selector, const QString &data )
{
    static QString requestDocListSelector("requestDocumentList");
    static QString newDocSelector("newDocument");
    static QString getDocTextSelector("getDocumentText");
    static QString setDocTextSelector("setDocumentText");
    static QString setCurrentDocSelector("setCurrentDocument");
    static QString closeDocSelector("closeDocument");
    static QString setDocTitleSelector("setDocumentTitle");

    if (selector == requestDocListSelector)
        handleDocListScRequest();

    if (selector == newDocSelector)
        handleNewDocScRequest(data);

    if (selector == getDocTextSelector)
        handleGetDocTextScRequest(data);

    if (selector == setDocTextSelector)
        handleSetDocTextScRequest(data);

    if (selector == setCurrentDocSelector)
        handleSetCurrentDocScRequest(data);

    if (selector == closeDocSelector)
        handleCloseDocScRequest(data);

    if (selector == setDocTitleSelector)
        handleSetDocTitleScRequest(data);
}

void DocumentManager::handleDocListScRequest()
{
    QList<Document*> docs = documents();
    QList<Document*>::Iterator it;
    QString command = QString("ScIDEDocument.syncDocs([");
    for (it = docs.begin(); it != docs.end(); ++it) {
        Document * doc = *it;
        QString docData = QString("[\'%1\', \'%2\', %3, %4],").arg(doc->id().constData()).arg(doc->title()).arg(doc->textAsSCArrayOfCharCodes(0, -1)).arg(doc->isModified());
        command = command.append(docData);
    }
    command = command.append("]);");
    Main::scProcess()->evaluateCode ( command, true );
}

void DocumentManager::handleNewDocScRequest( const QString & data )
{
    std::stringstream stream;
    stream << data.toStdString();
    YAML::Parser parser(stream);

    YAML::Node doc;
    if (parser.GetNextDocument(doc)) {
        if (doc.Type() != YAML::NodeType::Sequence)
            return;

        std::string title;
        bool success = doc[0].Read(title);
        if (!success)
            return;

        std::string text;
        success = doc[1].Read(text);
        if (!success)
            return;

        std::string id;
        success = doc[2].Read(id);
        if (!success)
            return;

        Document *document = createDocument( false, id.c_str(),
                                             QString::fromUtf8(title.c_str()),
                                             QString::fromUtf8(text.c_str()) );
        Q_EMIT( opened(document, 0, 0) );
    }
}

void DocumentManager::handleGetDocTextScRequest( const QString & data )
{
    std::stringstream stream;
    stream << data.toStdString();
    YAML::Parser parser(stream);

    YAML::Node doc;
    if (parser.GetNextDocument(doc)) {
        if (doc.Type() != YAML::NodeType::Sequence)
            return;

        std::string quuid;
        bool success = doc[0].Read(quuid);
        if (!success)
            return;

        std::string funcID;
        success = doc[1].Read(funcID);
        if (!success)
            return;

        int start;
        success = doc[2].Read(start);
        if (!success)
            return;

        int range;
        success = doc[3].Read(range);
        if (!success)
            return;

        QByteArray docID = QByteArray(quuid.c_str());

        Document *document = documentForId(docID);
        if(document){
            QString docText = document->textAsSCArrayOfCharCodes(start, range);

            QString command = QString("ScIDEDocument.executeAsyncResponse(\'%1\', %2.asAscii)").arg(QString(funcID.c_str()), docText);
            Main::scProcess()->evaluateCode ( command, true );
        }

    }
}

void DocumentManager::handleSetDocTextScRequest( const QString & data )
{
    std::stringstream stream;
    stream << data.toStdString();
    YAML::Parser parser(stream);

    YAML::Node doc;
    if (parser.GetNextDocument(doc)) {
        if (doc.Type() != YAML::NodeType::Sequence)
            return;

        std::string quuid;
        bool success = doc[0].Read(quuid);
        if (!success)
            return;

        std::string funcID;
        success = doc[1].Read(funcID);
        if (!success)
            return;

        std::string text;
        success = doc[2].Read(text);
        if (!success)
            return;

        int start;
        success = doc[3].Read(start);
        if (!success)
            return;

        int range;
        success = doc[4].Read(range);
        if (!success)
            return;

        QByteArray docID = QByteArray(quuid.c_str());

        Document *document = documentForId(docID);
        if(document){
            document->setTextInRange(QString(text.c_str()), start, range);

            QString command = QString("ScIDEDocument.executeAsyncResponse(\'%1\')").arg(QString(funcID.c_str()));
            Main::scProcess()->evaluateCode ( command, true );
        }

    }
}

void DocumentManager::handleSetCurrentDocScRequest( const QString & data )
{
    std::stringstream stream;
    stream << data.toStdString();
    YAML::Parser parser(stream);

    YAML::Node doc;
    if (parser.GetNextDocument(doc)) {
        if (doc.Type() != YAML::NodeType::Sequence)
            return;

        std::string quuid;
        bool success = doc[0].Read(quuid);
        if (!success)
            return;

        QByteArray docID = QByteArray(quuid.c_str());

        Document *document = documentForId(docID);
        if(document){
            showDocument(document);
        }
    }

}

void DocumentManager::handleCloseDocScRequest( const QString & data )
{
    std::stringstream stream;
    stream << data.toStdString();
    YAML::Parser parser(stream);

    YAML::Node doc;
    if (parser.GetNextDocument(doc)) {
        if (doc.Type() != YAML::NodeType::Sequence)
            return;

        std::string quuid;
        bool success = doc[0].Read(quuid);
        if (!success)
            return;

        QByteArray docID = QByteArray(quuid.c_str());

        Document *document = documentForId(docID);
        if(document){
            close(document);
        }
    }
}

void DocumentManager::handleSetDocTitleScRequest( const QString & data )
{
    std::stringstream stream;
    stream << data.toStdString();
    YAML::Parser parser(stream);

    YAML::Node doc;
    if (parser.GetNextDocument(doc)) {
        if (doc.Type() != YAML::NodeType::Sequence)
            return;

        std::string quuid;
        bool success = doc[0].Read(quuid);
        if (!success)
            return;

        std::string title;
        success = doc[1].Read(title);
        if (!success)
            return;

        QByteArray docID = QByteArray(quuid.c_str());

        Document *document = documentForId(docID);
        if(document)
        {
            document->mTitle = QString(title.c_str());
            Q_EMIT(titleChanged(document));
        }

    }

}
