/**
 * @licence app begin@
 * Copyright (C) 2011-2012  BMW AG
 *
 * This file is part of COVESA Project Dlt Viewer.
 *
 * Contributions are licensed to the COVESA Alliance under one or more
 * Contribution License Agreements.
 *
 * \copyright
 * This Source Code Form is subject to the terms of the
 * Mozilla Public License, v. 2.0. If a  copy of the MPL was not distributed with
 * this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * \file SearchDialog.cpp
 * For further information see http://www.covesa.global/.
 * @licence end@
 */

#include "searchdialog.h"
#include "ui_searchdialog.h"
#include "qdltoptmanager.h"
#include "qdltfile.h"
#include "tablemodel.h"
#include "decodecacheservice.h"

#include <dltmessagematcher.h>

#include <QApplication>
#include <QMessageBox>
#include <QPixmap>
#include <QSettings>
#include <QSignalBlocker>
#include <QDateTime>

#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

/**
 * @brief Constructor for SearchDialog.
 * @param parent Parent widget.
 */
SearchDialog::SearchDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SearchDialog)
{
    ui->setupUi(this);

    connect(&m_findAllWatcher, &QFutureWatcher<std::vector<std::uint64_t>>::finished,
            this, &CSearchDialog::onFindAllFinished);

    regexpCheckBox = ui->checkBoxRegExp;
    match = false;
    startLine = -1;
    indexingStartTimeMs = 0;

    lineEdits.append(ui->lineEditSearch);
    table = nullptr;

    // at start we want to know if single step search or "fill search table mode" is active !
    bool checked = QDltSettingsManager::getInstance()->value("other/search/checkBoxSearchIndex", bool(true)).toBool();
    ui->checkBoxFindAll->setChecked(checked);

    checked = QDltSettingsManager::getInstance()->value("other/search/checkBoxHeader", bool(true)).toBool();
    ui->checkBoxHeader->setChecked(checked);

    checked = QDltSettingsManager::getInstance()->value("other/search/checkBoxCasesensitive", bool(true)).toBool();
    ui->checkBoxCaseSensitive->setChecked(checked);

    checked = QDltSettingsManager::getInstance()->value("other/search/checkBoxRegEx", bool(true)).toBool();
    ui->checkBoxRegExp->setChecked(checked);

    ui->stackedWidgetRange->setCurrentIndex(0); // default Timestamp range
    connect(ui->radioTimestamp, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked)
            ui->stackedWidgetRange->setCurrentIndex(0);
    });
    connect(ui->radioTime, &QRadioButton::toggled, this, [this] (bool checked) {
        if (checked) {
            // switch from timestamp range to time range requires time range reset
            m_timeRangeResetNeeded = true;
            ui->stackedWidgetRange->setCurrentIndex(1);
        }
    });
    // user interaction with time range edits sets need for reset to false
    connect(ui->dateTimeStart, &QDateTimeEdit::dateTimeChanged, this, [this]() {
        m_timeRangeResetNeeded = false;
    });
    connect(ui->dateTimeEnd, &QDateTimeEdit::dateTimeChanged, this, [this]() {
        m_timeRangeResetNeeded = false;
    });

    // OK button triggers find next
    connect(this, &CSearchDialog::accepted, this, &CSearchDialog::findNextClicked);

    fSilentMode = !QDltOptManager::getInstance()->issilentMode();

    updateColorbutton();
}

/**
 * @brief Destructor for SearchDialog.
 */
SearchDialog::~SearchDialog()
{
    // Wait for workers so lambdas cannot outlive this dialog or its UI state.
    if (m_findAllWatcher.isRunning())
    {
        isSearchCancelled.store(true, std::memory_order_relaxed);
        m_findAllWatcher.future().cancel();
        m_findAllWatcher.waitForFinished();
    }

    clearCacheHistory();
    delete ui;
}

/**
 * @brief Selects and focuses the text in the line edit.
 */
void SearchDialog::selectText(){ui->lineEditText->setFocus();ui->lineEditText->selectAll();}
/**
 * @brief Sets the header checkbox state.
 * @param header True to check, false to uncheck.
 */
void SearchDialog::setHeader(bool header) { ui->checkBoxHeader->setCheckState(header?Qt::Checked:Qt::Unchecked);}
/**
 * @brief Sets the payload checkbox state.
 * @param payload True to check, false to uncheck.
 */
void SearchDialog::setPayload(bool payload) { ui->checkBoxPayload->setCheckState(payload?Qt::Checked:Qt::Unchecked);}
/**
 * @brief Sets the case sensitive checkbox state.
 * @param caseSensitive True to check, false to uncheck.
 */
void SearchDialog::setCaseSensitive(bool caseSensitive) { ui->checkBoxCaseSensitive->setCheckState(caseSensitive?Qt::Checked:Qt::Unchecked);}
/**
 * @brief Sets the regular expression checkbox state.
 * @param regExp True to check, false to uncheck.
 */
void SearchDialog::setRegExp(bool regExp) { ui->checkBoxRegExp->setCheckState(regExp?Qt::Checked:Qt::Unchecked);}
/**
 * @brief Sets the nextClicked flag.
 * @param next True if next is clicked.
 */
void SearchDialog::setNextClicked(bool next){nextClicked = next;}
/**
 * @brief Sets the match flag.
 * @param matched True if match is found.
 */
void SearchDialog::setMatch(bool matched){match=matched;}


/**
 * @brief Sets the onceClicked flag.
 * @param clicked True if once is clicked.
 */
void SearchDialog::setOnceClicked(bool clicked){onceClicked=clicked;}
/**
 * @brief Appends a QLineEdit to the list.
 * @param lineEdit Pointer to QLineEdit.
 */
void SearchDialog::appendLineEdit(QLineEdit *lineEdit){ lineEdits->append(lineEdit);}

/**
 * @brief Gets the text from the line edit.
 * @return Text string.
 */
QString SearchDialog::getText() { return ui->lineEditText->text(); }

/**
 * @brief Aborts the current search operation.
 */
void SearchDialog::abortSearch()
{
    isSearchCancelled = true;
}

void CSearchDialog::abortSearch()
{
    isSearchCancelled.store(true, std::memory_order_relaxed);
    if (m_findAllWatcher.isRunning())
    {
        m_findAllWatcher.future().cancel();
        m_findAllWatcher.waitForFinished();
    }
}

void CSearchDialog::reportProgress(int progress)
{
    emit searchProgressValueChanged(progress);
}

void CSearchDialog::publishPartialMatches(const std::vector<std::uint64_t> &matches)
{
    if (matches.empty() || !m_searchtablemodel)
        return;

    m_searchtablemodel->add_SearchResultEntries(matches);
    emit refreshedSearchIndex();
}

void CSearchDialog::startParallelFindAll(QRegularExpression searchTextRegExp)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    Q_UNUSED(searchTextRegExp);
    // Parallel Find-All is only enabled for Qt6+.
    return;
#else
    if (!file)
        return;

    if (m_findAllWatcher.isRunning())
    {
        isSearchCancelled.store(true, std::memory_order_relaxed);
        m_findAllWatcher.future().cancel();
        return;
    }

    isSearchCancelled.store(false, std::memory_order_relaxed);
    m_findAllUiUpdateTimer.restart();
    m_findAllLastUiUpdateMs = 0;
    m_findAllAddedSinceLastUiUpdate = 0;

    m_searchtablemodel->clear_SearchResults();
    emit refreshedSearchIndex();

    // Use an implicitly shared projection so workers get a stable view without a UI-thread copy.
    const SearchProjectionSnapshot projection(file);
    const int total = projection.size();

    if (total <= 0)
    {
        emit searchProgressChanged(false);
        return;
    }

    const bool msgIdEnabled = QDltSettingsManager::getInstance()->value("startup/showMsgId", true).toBool();
    const QString msgIdFormat = QDltSettingsManager::getInstance()->value("startup/msgIdFormat", "0x%x").toString();
    const bool pluginsEnabled = QDltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool();

    // Optimization: only decode when payload search is enabled.
    const bool payloadEnabled = getPayload();
    const bool doDecode = pluginsEnabled && payloadEnabled;

    const bool headerEnabled = getHeader();
    const bool caseSensitive = getCaseSensitive();
    const bool useRegExp = getRegExp();
    const QString searchText = getText();

    const QString apid = stApid;
    const QString ctid = stCtid;

    const bool timestampRangeEnabled = (ui->radioTimestamp->isChecked() && is_TimeStampSearchSelected);
    const double tsStart = dTimeStampStart;
    const double tsStop = dTimeStampStop;
    const bool timeRangeEnabled = ui->radioTime->isChecked();
    const QDateTime timeStart = ui->dateTimeStart->dateTime();
    const QDateTime timeEnd = ui->dateTimeEnd->dateTime();
    const DltMessageMatcher::Pattern searchPattern = useRegExp
        ? DltMessageMatcher::Pattern{searchTextRegExp}
        : DltMessageMatcher::Pattern{searchText};
    const auto createMatcher = [=]() {
        DltMessageMatcher matcher;
        matcher.setCaseSentivity(caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
        matcher.setSearchAppId(apid);
        matcher.setSearchCtxId(ctid);
        if (timestampRangeEnabled)
            matcher.setTimestampRange(tsStart, tsStop);
        if (timeRangeEnabled)
            matcher.setTimeRange(timeStart, timeEnd);
        if (msgIdEnabled)
            matcher.setMessageIdFormat(msgIdFormat);
        matcher.setHeaderSearchEnabled(headerEnabled);
        matcher.setPayloadSearchEnabled(payloadEnabled);
        return matcher;
    };

    // QDltFile I/O is guarded by mutexQDlt; setMsg/toStringX work on local copies —
    // parallel workers are safe when plugin decode is not needed.
    // Fall back to single-thread when doDecode=true (plugins are not thread-safe).
    const QPointer<CSearchDialog> dlg(this);
    QDltFile* filePtr = file;
    const bool dltv2Support = filePtr->getDLTv2Support();
    QDltPluginManager* pluginPtr = pluginManager;
    CDecodeCacheService* decodeCache = m_decodeCacheService;
    QThreadPool *findAllPool = &findAllThreadPool();

    auto future = QtConcurrent::run([=]() -> std::vector<std::uint64_t> {
        if (!doDecode)
        {
            // Parallel path: each worker does its own file read + parse + match.
            const int workerCount = findAllPool->maxThreadCount();
            const int maxChunkSize = 20000;
            const int chunkCount = qMax(1, qMax(qMin(total, workerCount * 8),
                                                 (total + maxChunkSize - 1) / maxChunkSize));
            const int chunkSize  = qMax(1, (total + chunkCount - 1) / chunkCount);

            QVector<SearchWorkChunk> chunks;
            chunks.reserve(chunkCount);
            for (int b = 0; b < total; b += chunkSize)
                chunks.push_back({b, qMin(b + chunkSize, total)});

            std::atomic<int> completedChunks{0};
            const int nChunks = chunks.size();

            auto mapChunk = [=, &completedChunks](const SearchWorkChunk &chunk) -> std::vector<std::uint64_t> {
                QFile workerReader;
                QDltMsg msg;
                DltMessageMatcher matcher = createMatcher();
                std::vector<std::uint64_t> localMatches;
                localMatches.reserve(qMax(1, (chunk.end - chunk.begin) / 16));

                for (int i = chunk.begin; i < chunk.end; ++i)
                {
                    if (dlg && dlg->isSearchCancelled.load(std::memory_order_relaxed))
                        break;

                    const int msgIndex = projection.globalIndexAt(i);
                    if (msgIndex < 0)
                        continue;

                    const QByteArray messageBytes = filePtr->getMsg(msgIndex, workerReader);
                    if (messageBytes.isEmpty())
                        continue;
                    if (!msg.setMsg(messageBytes, true, dltv2Support)
                        && (dltv2Support || !msg.setMsg(messageBytes, true, true)))
                        continue;
                    msg.setIndex(msgIndex);

                    if (matcher.match(msg, searchPattern))
                        localMatches.push_back(static_cast<std::uint64_t>(msgIndex));
                }

                const int done = completedChunks.fetch_add(1, std::memory_order_relaxed) + 1;
                const int progress = done * 99 / nChunks;
                if (dlg)
                {
                    QMetaObject::invokeMethod(dlg, [dlg, progress]() {
                        if (dlg) dlg->reportProgress(progress);
                    }, Qt::QueuedConnection);
                    // Show matches as each chunk completes, instead of waiting for the whole search to finish.
                    if (!localMatches.empty())
                    {
                        QMetaObject::invokeMethod(dlg, [dlg, batch = localMatches]() {
                            if (dlg) dlg->publishPartialMatches(batch);
                        }, Qt::QueuedConnection);
                    }
                }

                return localMatches;
            };

            auto reduceMatches = [](std::vector<std::uint64_t> &result,
                                    const std::vector<std::uint64_t> &chunk) {
                result.insert(result.end(), chunk.begin(), chunk.end());
            };

            auto matches = QtConcurrent::blockingMappedReduced<std::vector<std::uint64_t>>(
                findAllPool, chunks, mapChunk, reduceMatches,
                QtConcurrent::OrderedReduce | QtConcurrent::SequentialReduce);

            if (dlg)
                QMetaObject::invokeMethod(dlg, [dlg]() {
                    if (dlg) dlg->reportProgress(100);
                }, Qt::QueuedConnection);
            return matches;
        }

        // File loading and parsing are parallel; shared decoder plugins remain serialized.
        std::vector<std::uint64_t> matches;
        matches.reserve(qMax(1, total / 16));
        const int workerCount = findAllPool->maxThreadCount();
        const int maxChunkSize = 20000;
        const int chunkCount = qMax(1, qMax(qMin(total, workerCount * 8),
                                             (total + maxChunkSize - 1) / maxChunkSize));
        const int chunkSize = qMax(1, (total + chunkCount - 1) / chunkCount);
        QThreadStorage<QFile*> workerReaders;

        for (int begin = 0; begin < total; begin += chunkSize)
        {
            const int end = qMin(begin + chunkSize, total);
            QVector<int> rows;
            rows.reserve(end - begin);
            for (int row = begin; row < end; ++row)
                rows.push_back(row);

            auto loadMessage = [=, &workerReaders](int row) -> LoadedSearchMessage {
                const int msgIndex = projection.globalIndexAt(row);
                if (msgIndex < 0)
                    return {std::numeric_limits<std::uint64_t>::max(), {}};

                QFile *workerReader = workerReaders.localData();
                if (!workerReader)
                {
                    workerReader = new QFile;
                    workerReaders.setLocalData(workerReader);
                }

                const QByteArray messageBytes = filePtr->getMsg(msgIndex, *workerReader);
                if (messageBytes.isEmpty())
                    return {std::numeric_limits<std::uint64_t>::max(), {}};

                QDltMsg message;
                if (!message.setMsg(messageBytes, true, dltv2Support)
                    && (dltv2Support || !message.setMsg(messageBytes, true, true)))
                    return {std::numeric_limits<std::uint64_t>::max(), {}};
                message.setIndex(msgIndex);
                return {static_cast<std::uint64_t>(msgIndex), std::move(message)};
            };

            auto appendLoaded = [](std::vector<LoadedSearchMessage> &result,
                                   const LoadedSearchMessage &loaded) {
                if (loaded.index != std::numeric_limits<std::uint64_t>::max())
                    result.push_back(loaded);
            };

            const auto loadedMessages = QtConcurrent::blockingMappedReduced<std::vector<LoadedSearchMessage>>(
                findAllPool, rows, loadMessage, appendLoaded,
                QtConcurrent::OrderedReduce | QtConcurrent::SequentialReduce);

            DltMessageMatcher matcher = createMatcher();
            const std::size_t matchesBeforeChunk = matches.size();
            for (const LoadedSearchMessage &loaded : loadedMessages)
            {
                if (dlg && dlg->isSearchCancelled.load(std::memory_order_relaxed))
                    break;

                QDltMsg message = loaded.message;
                decodeCache->decode(pluginPtr, dlg ? dlg->fSilentMode : 0, message);
                if (matcher.match(message, searchPattern))
                    matches.push_back(loaded.index);
            }

            if (dlg)
            {
                const int progress = qMin(99, static_cast<int>(static_cast<qint64>(end) * 99 / total));
                QMetaObject::invokeMethod(dlg, [dlg, progress]() {
                    if (dlg) dlg->reportProgress(progress);
                }, Qt::QueuedConnection);
                // Show matches as each chunk completes, instead of waiting for the whole search to finish.
                if (matches.size() > matchesBeforeChunk)
                {
                    std::vector<std::uint64_t> newMatches(matches.begin() + matchesBeforeChunk, matches.end());
                    QMetaObject::invokeMethod(dlg, [dlg, newMatches]() {
                        if (dlg) dlg->publishPartialMatches(newMatches);
                    }, Qt::QueuedConnection);
                }
            }

            if (dlg && dlg->isSearchCancelled.load(std::memory_order_relaxed))
                break;
        }

        if (dlg)
            QMetaObject::invokeMethod(dlg, [dlg]() {
                if (dlg) dlg->reportProgress(100);
            }, Qt::QueuedConnection);
        return matches;
    });

    m_findAllWatcher.setFuture(future);
#endif
}

void CSearchDialog::onFindAllFinished()
{
    const bool cancelled = m_findAllWatcher.isCanceled() || isSearchCancelled.load(std::memory_order_relaxed);
    std::vector<std::uint64_t> matches;
    if (!m_findAllWatcher.isCanceled())
        matches = m_findAllWatcher.future().result();

    if (cancelled)
    {
        // Keep the partial results already published incrementally.
        emit refreshedSearchIndex();
        emit searchProgressChanged(false);
        match = m_searchtablemodel && m_searchtablemodel->rowCount(QModelIndex()) > 0;
        return;
    }

    // Replace whatever was shown incrementally with the authoritative, correctly-ordered result.
    if (m_searchtablemodel)
    {
        m_searchtablemodel->clear_SearchResults();
        if (!matches.empty())
            m_searchtablemodel->add_SearchResultEntries(matches);
    }

    // Ensure the last batch of incremental updates is reflected.
    emit refreshedSearchIndex();

    emit searchProgressChanged(false);

    // Do not rebuild the model here; it was built incrementally during reduce.
    cacheSearchHistory();

    match = !matches.empty();
    const int colourResult = match ? 1 : 0;

    for (int i = 0; i < lineEdits.size(); ++i)
        setSearchColour(lineEdits.at(i), colourResult);

    if (!match)
        setStartLine(-1);

}

bool CSearchDialog::getHeader()
{
    return (ui->checkBoxHeader->checkState() == Qt::Checked);
}

/**
 * @brief Returns the state of the payload checkbox.
 * @return True if checked, false otherwise.
 */
bool SearchDialog::getPayload()
{
    return (ui->checkBoxPayload->checkState() == Qt::Checked);
}

/**
 * @brief Returns the state of the regular expression checkbox.
 * @return True if checked, false otherwise.
 */
bool SearchDialog::getRegExp()
{
    return (ui->checkBoxRegExp->checkState() == Qt::Checked);
}

/**
 * @brief Returns the state of nextClicked flag.
 * @return True if next is clicked.
 */
bool SearchDialog::getNextClicked(){return nextClicked;}
/**
 * @brief Returns the state of onceClicked flag.
 * @return True if once is clicked.
 */
bool SearchDialog::getOnceClicked(){return onceClicked;}

/**
 * @brief Gets the APID text.
 * @return APID string.
 */
QString SearchDialog::getApIDText(){ return ui->apIdlineEdit->text();}
/**
 * @brief Gets the CTID text.
 * @return CTID string.
 */
QString SearchDialog::getCtIDText(){ return ui->ctIdlineEdit->text();}

/**
 * @brief Gets the start timestamp text.
 * @return Start timestamp string.
 */
QString SearchDialog::getTimeStampStart()
{
    //qDebug() << "content of start time" << ui->timeStartlineEdit->text()<< __LINE__;
    return ui->lineEditTimestampStart->text();
}

/**
 * @brief Gets the end timestamp text.
 * @return End timestamp string.
 */
QString SearchDialog::getTimeStampEnd()
{
    //qDebug() << "content of end time" << ui->timeEndlineEdit->text() << __LINE__;
    return ui->lineEditTimestampEnd->text();
}

/**
 * @brief Returns the state of the case sensitive checkbox.
 * @return True if checked, false otherwise.
 */
bool SearchDialog::getCaseSensitive()
{
    //qDebug() << "getCaseSensitive is" << ui->checkBoxCaseSensitive->checkState() << __LINE__;
    return (ui->checkBoxCaseSensitive->checkState() == Qt::Checked);
}

/**
 * @brief Returns the state of the search to index checkbox.
 * @return True if checked, false otherwise.
 */
bool SearchDialog::searchtoIndex()
{
    //qDebug() << "searchtoIndex is" << ui->checkBoxSearchIndex->checkState() << __LINE__;
    return (ui->checkBoxFindAll->checkState() == Qt::Checked);
}


/**
 * @brief Returns whether search should start from the beginning.
 * @return True if starting from beginning.
 */
bool SearchDialog::getSearchFromBeginning()
{
    return (ui->radioButtonPosBeginning->isChecked());
}

/**
 * @brief Sets the start line for search.
 * @param start Line number to start search.
 */
void SearchDialog::setStartLine(long int start)
{
  startLine=start;
}


/**
 * @brief Sets the color of the search result in the line edit.
 * @param lineEdit Pointer to QLineEdit.
 * @param result Search result code.
 */
void SearchDialog::setSearchColour(QLineEdit *lineEdit,int result)
{
    QPalette palette = lineEdit->palette();
    QColor text0 = QColor(255,255,255);
    QColor text1 = QColor(0,0,0);
    QColor background0 = QColor(255,102,102);
    QColor background1 = QColor(255,255,255);

    if (QDltSettingsManager::UI_Colour::UI_Dark == QDltSettingsManager::getInstance()->uiColour)
    {
        background1 = QColor(31,31,31);
        text1 = QColor(255,255,255);
    }

    switch(result){
    case 0:
        palette.setColor(QPalette::Text,text0);
        lineEdit->setPalette(palette);
        palette.setColor(QPalette::Base,background0);
        lineEdit->setPalette(palette);
        break;
    case 1:
        palette.setColor(QPalette::Text,text1);
        lineEdit->setPalette(palette);
        palette.setColor(QPalette::Base,background1);
        lineEdit->setPalette(palette);
        break;
    }
}

/**
 * @brief Focuses the specified row in the table.
 * @param searchLine Row to focus.
 */
void SearchDialog::focusRow(long int searchLine)
{
    CTableModel *model = qobject_cast<CTableModel *>(table->model());
    if(!model || !table)
    {
        return;
    }

    if(searchLine < 0 || searchLine >= model->rowCount())
    {
        // Clear marker state without trying to navigate to an invalid model index.
        model->setMarker(-1, highlightColor);
        model->setLastSearchIndex(-1);
        if(table->selectionModel())
        {
            table->selectionModel()->clear();
        }
        table->viewport()->update();
        return;
    }

    QModelIndex idx = model->index(searchLine, 0, QModelIndex());
    //qDebug() << "Focus row in message table window" << searchLine << __FILE__ << __LINE__;

    table->scrollTo(idx, QAbstractItemView::EnsureVisible);
    table->scrollTo(idx, QAbstractItemView::PositionAtCenter);

    model->setMarker(searchLine, highlightColor);

    model->setLastSearchIndex(searchLine);
    if(table->selectionModel())
    {
        table->selectionModel()->clear();
    }
    table->viewport()->update();
}

/**
 * @brief Performs the search operation.
 * @return Search result code.
 */
int SearchDialog::find()
{
    isSearchCancelled.store(false, std::memory_order_relaxed);

    emit addActionHistory();
    QRegularExpression searchTextRegExpression;
    is_TimeStampSearchSelected = false;
    long int searchBorder;
    long int lStartLine;

    emit searchProgressChanged(true);

    if(file->sizeFilter()==0)
    {
        emit searchProgressChanged(false);
        return 0;
    }

   if( ( (match == true) || ( getSearchFromBeginning() == false )) && false == searchtoIndex() )
    {
        // single step search
        QModelIndexList list = table->selectionModel()->selection().indexes();
        if(list.count() > 0)
        {
            QModelIndex index;
            for(int num=0; num < list.count();num++)
            {
                index = list[num];
                if(index.column()==0)
                {
                    break;
                }
            }
            setStartLine(index.row());
        }
    }
   else
   {
      focusRow(-1);
   }


    if ( true == getSearchFromBeginning() )
    {
      //qDebug() << "Start from the beginning" << __LINE__;
    }
    else
    {
        if (table->selectionModel() != nullptr )
         {
          {
           if ( false == table->selectionModel()->selectedIndexes().isEmpty() )
            {
             if (table->selectionModel()->selectedIndexes().first().row() > -1)
              {
               lStartLine = table->selectionModel()->selectedIndexes().first().row();
               setStartLine( lStartLine );
              }
           }
          }
         }
    }

    searchBorder = startLine;
    if(searchBorder < 0 || searchtoIndex())
    {
        if(getNextClicked() || searchtoIndex())
        {
            searchBorder = file->sizeFilter()==0?0:file->sizeFilter()-1;
        }
        else
        {
            searchBorder = 0;
        }

    }

    if(getRegExp() == true)
    {
        searchTextRegExpression.setPattern(getText());
        if (searchTextRegExpression.isValid() == false)
        {
            if ( false == fSilentMode)
            {
            QMessageBox::warning(0, QString("Search"), QString("Invalid regular expression!"));
            }
            emit searchProgressChanged(false);
            return 1;
        }

        int options = QRegularExpression::DotMatchesEverythingOption;
        if (!getCaseSensitive())
            options |= QRegularExpression::CaseInsensitiveOption;
        searchTextRegExpression.setPatternOptions(static_cast<QRegularExpression::PatternOption>(options));
    }

    // check timestamp search pattern
    const QString timeStampStartTime = getTimeStampStart();
    const QString timeStampStopTime = getTimeStampEnd();

    if (!timeStampStartTime.isEmpty() && !timeStampStopTime.isEmpty())
    {
        dTimeStampStart = timeStampStartTime.toDouble();
        dTimeStampStop = timeStampStopTime.toDouble();
        if( (dTimeStampStop -  dTimeStampStart) >= 0 )
         {
         //qDebug() << "Timestamp search enabled" << dTimeStampStart << dTimeStampStop << __LINE__;
         is_TimeStampSearchSelected = true;
         }
        else
        {
         qDebug() << "Invalid timestamp range" << dTimeStampStart << dTimeStampStop << __LINE__;
         is_TimeStampSearchSelected = false;
         if ( false == fSilentMode)
         {
         QMessageBox::warning(0, QString("Search"), QString("Invalid timestamp range !"));
         }
         emit searchProgressChanged(false);
         return 1;
        }
    }

    // check APID and CTID search
    stApid = getApIDText();
    stCtid = getCtIDText();
    if( stApid.size() > 0 || stCtid.size() > 0 ) // so we need to consider what is given here
    {
        if( stApid.size() > 4 || stCtid.size() > 4 )
        {
            qDebug() << "Given APID or CTID exceeds limit !";
            if ( false == fSilentMode)
            {
            QMessageBox::warning(0, QString("Search"), QString("Given APID or CTID exceeds limit !"));
            }
            emit searchProgressChanged(false);
            return 2;
        }
    }

    if (searchtoIndex() == true)
    {
        // Find-All uses QtConcurrent on Qt6 and the existing single-threaded loop on Qt5.
    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        // Run Find All on a worker thread so the UI stays responsive.
        startParallelFindAll(searchTextRegExpression);
        return 1;
    #else
        findMessages(startLine, searchBorder, searchTextRegExpression);
        emit refreshedSearchIndex();
        cacheSearchHistory();
        match = (m_searchtablemodel && m_searchtablemodel->get_SearchResultListSize() > 0);
        emit searchProgressChanged(false);
        return match ? 1 : 0;
    #endif
    }

    findMessages(startLine,searchBorder,searchTextRegExpression);

    // Log whether the search term was found (works for both single-step and batch modes)
    bool searchFound = false;
    if (searchtoIndex()) {
        searchFound = (m_searchtablemodel->get_SearchResultListSize() > 0);
    }
    else {
        searchFound = match;
    }
    qDebug() << "Search term exists:" << (searchFound ? "true" : "false");
    emit searchProgressChanged(false);

    if(match == true )
    {
        return 1;
    }
    setStartLine(-1); // so we do not miss index 0 any longer ...
    return 0;
}


/**
 * @brief Finds messages matching the search criteria.
 * @param searchLine Starting line for search.
 * @param searchBorder Border line for search.
 * @param searchTextRegExp Regular expression for search.
 */
void SearchDialog::findMessages(long int searchLine, long int searchBorder, QRegularExpression &searchTextRegExp)
{
    QDltMsg msg;
    int ctr = 0;
    Qt::CaseSensitivity is_Case_Sensitive = Qt::CaseInsensitive;

    // Start overall search timing
    qint64 searchStartTimeMs = QDateTime::currentMSecsSinceEpoch();
    starttime();

    if(getCaseSensitive() == true)
    {
        is_Case_Sensitive = Qt::CaseSensitive;
    }

    m_searchtablemodel->clear_SearchResults();

    // Start indexing timer for batch/index mode
    if (searchtoIndex()) {
        startIndexingTimeMs();
    }
    // Log search parameters (only for batch/index mode)
    QString searchText = getText();
    if (searchtoIndex()) {
        qDebug() << "Search string:" << searchText;
        qDebug() << "Total messages to search:" << file->sizeFilter();
    }
    bool msgIdEnabled=QDltSettingsManager::getInstance()->value("startup/showMsgId", true).toBool();
    QString msgIdFormat=QDltSettingsManager::getInstance()->value("startup/msgIdFormat", "0x%x").toString();

    DltMessageMatcher matcher;
    matcher.setCaseSentivity(is_Case_Sensitive);
    matcher.setSearchAppId(stApid);
    matcher.setSearchCtxId(stCtid);

    if (ui->radioTimestamp->isChecked() && is_TimeStampSearchSelected) {
        matcher.setTimestampRange(dTimeStampStart, dTimeStampStop);
    }
    if (ui->radioTime->isChecked()) {
        matcher.setTimeRange(ui->dateTimeStart->dateTime(), ui->dateTimeEnd->dateTime());
    }

    if (msgIdEnabled) {
        matcher.setMessageIdFormat(msgIdFormat);
    }
    matcher.setHeaderSearchEnabled(getHeader());
    matcher.setPayloadSearchEnabled(getPayload());
    const bool decodeEnabled = QDltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool();

    do
    {
        ctr++; // for file progress indication

        if(getNextClicked() || searchtoIndex())
        {
            searchLine++;
            if(searchLine >= filteredSize)
            {
                searchLine = 0;
            }
        }
        else // go back
        {
            searchLine--;
            if(searchLine <= -1)
            {
                searchLine = filteredSize-1;
            }
        }

        // Update progress every 0.5%
        if(searchLine%1000 == 0)
        {
            QApplication::processEvents();
            if (isSearchCancelled.load(std::memory_order_relaxed)) {
                break;
            }
            const int siPercent = static_cast<int>(ctr * 100.0 / filteredSize);
            emit searchProgressValueChanged(siPercent);
        }

        if(searchLine < 0 || searchLine >= filteredSize)
        {
            continue;
        }

        const int globalIndex = projection.globalIndexAt(static_cast<int>(searchLine));
        if(globalIndex < 0)
        {
            continue;
        }

        if(!m_decodeCacheService->message(file,
                                         pluginManager,
                                         globalIndex,
                                         decodeEnabled,
                                         fSilentMode,
                                         msg,
                                         true))
        {
            continue;
        }

        const bool matchFound = getRegExp() ? matcher.match(msg, searchTextRegExp) : matcher.match(msg, getText());
        if (!matchFound)
        {
            match = false;
            continue;
        }

        if (foundLine(searchLine, globalIndex))
            break;
        else
            continue;
    }
    while( searchBorder != searchLine );
    // Stop indexing timer for batch/index mode
    if (searchtoIndex()) {
        stopIndexingTimeMs();
    }
    stoptime();
    // Calculate and log overall search time with results
    qint64 searchEndTimeMs = QDateTime::currentMSecsSinceEpoch();
    qint64 totalSearchDurationMs = searchEndTimeMs - searchStartTimeMs;
    int resultsFound = searchtoIndex() ? m_searchtablemodel->get_SearchResultListSize() : (match ? 1 : 0);
    qDebug() << "Total search time [ms]:" << totalSearchDurationMs;
    qDebug() << "Results found:" << resultsFound;
}

/**
 * @brief Handles a found line during search.
 * @param searchLine Line number found.
 * @return True if single result found, false otherwise.
 */
bool SearchDialog::foundLine(long int searchLine)
{
    match = true;

    if (searchtoIndex() == true)
    {
        addToSearchIndex(globalIndex);
        emit refreshedSearchIndex();
    }
    else
    {
        focusRow(searchLine); // focus the line ein message table view
        setStartLine(searchLine);
        //qDebug() << "Single line hit in  " << searchLine << __LINE__;
        return true;//found single result, and breaking here
    }
    return false;//don't break search here
}

/**
 * @brief Slot for next button click event.
 */
void SearchDialog::on_pushButtonNext_clicked() // connected to main window line 424
{
    /* For every new search, start payLoad and EndpayLoad will be different and hence member variable storing previous found
     * value shoudl eb reset. */
    setNextClicked(true);
    int result = find();
    for(int i=0; i<lineEdits->size();i++)
    {
       setSearchColour(lineEdits->at(i),result);
    }
}

/**
 * @brief Slot for previous button click event.
 */
void SearchDialog::on_pushButtonPrevious_clicked()
{
    setNextClicked(false);
    int result = find();
    for(int i=0; i<lineEdits->size();i++)
    {
       setSearchColour(lineEdits->at(i),result);
    }
}

/**
 * @brief Handles find next action.
 */
void SearchDialog::findNextClicked()
{
    setNextClicked(true);

    // Find All runs asynchronously and updates colour on completion.
    if (searchtoIndex())
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        (void)find();
        return;
#else
        int result = find();
        for (int i = 0; i < lineEdits.size(); i++)
            setSearchColour(lineEdits.at(i), result);
        return;
#endif
    }

    int result = find();
    for(int i=0; i<lineEdits.size();i++)
        setSearchColour(lineEdits.at(i),result);
}

/**
 * @brief Handles find previous action.
 */
void SearchDialog::findPreviousClicked()
{
    setNextClicked(false);

    if (searchtoIndex())
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        (void)find();
        return;
#else
        int result = find();
        for (int i = 0; i < lineEdits.size(); i++)
            setSearchColour(lineEdits.at(i), result);
        return;
#endif
    }

    int result = find();
    for(int i=0; i<lineEdits.size();i++)
        setSearchColour(lineEdits.at(i),result);
}

/**
 * @brief Slot for line edit text edited event.
 * @param newText The new text.
 */
void SearchDialog::on_lineEditText_textEdited(QString newText)
{
        {
            // block signal so that it does not trigger a setText back on lineEdits->at(0)!
            QSignalBlocker signalBlocker(lineEdits.at(1));
            lineEdits.at(1)->setText(newText);
        }
        for(int i=0; i<lineEdits.size();i++){
            if(lineEdits.at(0)->text().isEmpty())
                setSearchColour(lineEdits.at(i),1);
        }
}
/**
 * @brief Handles text edited from toolbar.
 * @param newText The new text.
 */
void SearchDialog::textEditedFromToolbar(QString newText)
{
        {
            // block signal so that it does not trigger a setText back on lineEdits->at(1)!
            QSignalBlocker signalBlocker(lineEdits.at(0));
            lineEdits.at(0)->setText(newText);
        }
        for(int i=0; i<lineEdits.size();i++){
            if(lineEdits.at(0)->text().isEmpty())
                setSearchColour(lineEdits.at(i),1);
        }
}

/**
 * @brief Slot for color button click event.
 */
void SearchDialog::on_pushButtonColor_clicked()
{
    QString color = QDltSettingsManager::getInstance()->value("other/searchResultColor", QString("#00AAFF")).toString();
    QColor oldColor(color);
    QColor newColor = QColorDialog::getColor(oldColor, this, "Pick color for Search Highlight");
    if(false == newColor.isValid())
    {
        // User cancelled
        return;
    }

    QDltSettingsManager::getInstance()->setValue("other/searchResultColor", newColor.name());
    updateColorbutton();
}

/**
 * @brief Updates the color button icon.
 */
void SearchDialog::updateColorbutton()
{
    QString color = QDltSettingsManager::getInstance()->value("other/searchResultColor", QString("#00AAFF")).toString();
    QColor lhlColor(color);
    highlightColor = lhlColor;
    QPixmap px(12, 12);
    px.fill(highlightColor);
    ui->buttonHighlightColor->setIcon(px);
}


/**
 * @brief Adds a line to the search index.
 * @param searchLine Line number to add.
 */
void SearchDialog::addToSearchIndex(long int searchLine)
{
    //qDebug() << "Add hit global index to search table" << globalIndex << __LINE__;
    if(globalIndex >= 0)
        m_searchtablemodel->add_SearchResultEntry(globalIndex);
 }

/**
 * @brief Registers the search table model.
 * @param model Pointer to SearchTableModel.
 */
void SearchDialog::registerSearchTableModel(SearchTableModel *model)
{
    m_searchtablemodel = model;    
}


/**
 * @brief Loads search history from cache.
 */
void SearchDialog::loadSearchHistory()
{
    // getting text of the action button clicked to load search history.
    QAction *action = qobject_cast<QAction *>(sender());
    QString text;
    if(action)
    {
        text = action->text();
    }

    // creating a local list to store the indexes related to the key retrieved from the cache.
    std::vector<unsigned long> tmp;
    if(cachedHistoryKey.size() > 0)
    {
        tmp = cachedHistoryKey[text];

        //deleting the previous search list and adding the cached search obtained to the model.
        m_searchtablemodel->clear_SearchResults();
        for (std::size_t i = 0; i < tmp.size(); ++i)
        {
            m_searchtablemodel->add_SearchResultEntry(tmp.at(i));
        }
    }
    emit refreshedSearchIndex();
}

/**
 * @brief Caches the current search history.
 */
void SearchDialog::cacheSearchHistory()
{
    // if it is a new search then add all the indexes of the search to a list(m_searchHistory).
    QString searchBoxText = getText();  
    m_searchHistory.push_back(m_searchtablemodel->m_searchResultList);
    cachedHistoryKey.insert(searchBoxText, m_searchHistory.back());    
}

/**
 * @brief Clears the cached search history.
 */
void SearchDialog::clearCacheHistory()
{
    // obtaining the list of keys stored in cache
    cachedHistoryKey.clear();
}

/**
 * @brief Saves the search history to persistent storage.
 * @param searchHistory List of search history entries.
 */
void SearchDialog::saveSearchHistory(QStringList& searchHistory) {
    //To save the search history
    QSettings settings("MyApp", "SearchHistory");
    settings.beginWriteArray("history");
    int count = qMin(searchHistory.size(), 20);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        settings.setValue("entry", searchHistory.at(i));
    }
    settings.endArray();
}

/**
 * @brief Loads the search history list from persistent storage.
 * @param searchHistory List to populate with history entries.
 */
void SearchDialog::loadSearchHistoryList(QStringList& searchHistory)
{
  //To retrive the search history once DLT Viewer restarts
    QSettings settings("MyApp", "SearchHistory");
    searchHistory.clear();
    int size = settings.beginReadArray("history");
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        searchHistory.append(settings.value("entry").toString());
    }
    settings.endArray();
}

/**
 * @brief Slot for header checkbox toggled event.
 * @param checked True if checked.
 */
void SearchDialog::on_checkBoxHeader_toggled(bool checked)
{
   QDltSettingsManager::getInstance()->setValue("other/search/checkBoxHeader", checked);
}


/**
 * @brief Slot for search index checkbox toggled event.
 * @param checked True if checked.
 */
void SearchDialog::on_checkBoxSearchIndex_toggled(bool checked)
{
    QDltSettingsManager::getInstance()->setValue("other/search/checkBoxSearchIndex", checked);
    setStartLine(-1);
}

/**
 * @brief Slot for case sensitive checkbox toggled event.
 * @param checked True if checked.
 */
void SearchDialog::on_checkBoxCaseSensitive_toggled(bool checked)
{
    QDltSettingsManager::getInstance()->setValue("other/search/checkBoxCasesensitive", checked);
}

/**
 * @brief Slot for regular expression checkbox toggled event.
 * @param checked True if checked.
 */
void SearchDialog::on_checkBoxRegExp_toggled(bool checked)
{
    QDltSettingsManager::getInstance()->setValue("other/search/checkBoxRegEx", checked);
}


/**
 * @brief Starts the timer for search duration measurement.
 */
void SearchDialog::starttime(void)
{
long int temps;

#if defined(_MSC_VER)
   SYSTEMTIME systemtime;
   GetSystemTime(&systemtime);
   time_t timestamp_sec;
   time(&timestamp_sec);
   temps = (time_t)timestamp_sec;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    temps = (time_t)tv.tv_sec;
#endif
    searchseconds = temps;

}

/**
 * @brief Stops the timer and logs the search duration.
 */
void SearchDialog::stoptime(void)
{
long int temps;
long int dtemps;

#if defined(_MSC_VER)
   SYSTEMTIME systemtime;
   GetSystemTime(&systemtime);
   time_t timestamp_sec;
   time(&timestamp_sec);
   temps = (time_t)timestamp_sec;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    temps = (time_t)tv.tv_sec;
#endif

    dtemps = temps - searchseconds;
    qDebug() << "Time for search [s]" << dtemps;
}

// Starts the millisecond timer for indexing duration measurement.
void SearchDialog::startIndexingTimeMs(void)
{
    indexingStartTimeMs = QDateTime::currentMSecsSinceEpoch();
}

// Stops the millisecond timer and logs the indexing duration.
void SearchDialog::stopIndexingTimeMs(void)
{
    qint64 currentTimeMs = QDateTime::currentMSecsSinceEpoch();
    qint64 totalDurationMs = currentTimeMs - indexingStartTimeMs;
    qDebug() << "Total indexing time of search results [ms]:" << totalDurationMs;
}


