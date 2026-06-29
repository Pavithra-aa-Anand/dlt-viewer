#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QToolBar>
#include <QTableView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QCoreApplication>
#include <QTimer>
#include <QFileDialog>

#include "crlffilterwindow.h"
#include "mainwindow.h"
#include "fieldnames.h"
#include "qdltfile.h"
#include "qdltexporter.h"
#include "qdltsettingsmanager.h"

CrlfFilterWindow::CrlfFilterWindow(QObject* parent) : QObject(parent) {
    m_sourceModelOfDLT = nullptr;
    m_crlfProjectionModel = nullptr;
    m_crlfWindow = nullptr;
    m_crlfTableView = nullptr;
    m_statusLabel = nullptr;
    m_dltFile = nullptr;
    m_pluginManager = nullptr;
    m_messageStore = nullptr;
    m_indexService = nullptr;
    m_externalDecodeCacheService = nullptr;
    
    rebuildTimer = new QTimer(this);
    rebuildTimer->setSingleShot(true);
    rebuildTimer->setInterval(500);
    connect(rebuildTimer, &QTimer::timeout, this, &CrlfFilterWindow::onRebuildTimerTimeout);
    
    m_lastFilteredMessageCount = -1;
    m_rebuildScheduled = false;
    m_rebuildInProgress = false;
}

void CrlfFilterWindow::invalidateCache() {
    m_crlfCache.clear();
    m_messageDataCache.clear();
    m_lastCacheValidCount = -1;
    m_bulkCrlfIndexBuilt = false;
    CDecodeCacheService *activeDecodeCache = m_externalDecodeCacheService ? m_externalDecodeCacheService : &m_decodeCacheService;
    if (m_dltFile) {
        activeDecodeCache->clearForFile(m_dltFile);
    } else {
        activeDecodeCache->clear();
    }
}

// Check if a message contains CRLF characters
bool CrlfFilterWindow::containsCrlf(const QString& payload) {
    return payload.contains("\r") || payload.contains("\n");
}

// Update window title and status label with message count
void CrlfFilterWindow::updateMessageCount(int count) {
    QString countText = QString("CRLF Messages (%1 found)").arg(count);
    if (crlfWindow) {
        crlfWindow->setWindowTitle(countText);
    }
    if (statusLabel) {
        statusLabel->setText(QString("Total CRLF messages: %1 ").arg(count));
    }
}

// Apply column settings to table view
void CrlfFilterWindow::applyColumnSettings() {
    if (!crlfTableView || !crlfFilterProxy) {
        return;
    }
    
    auto settings = QDltSettingsManager::getInstance();
    for (int col = 0; col < crlfFilterProxy->columnCount(); ++col) {
        bool show = FieldNames::getColumnShown(static_cast<FieldNames::Fields>(col), settings);
        crlfTableView->setColumnHidden(col, !show);
        if (show) {
            int width = FieldNames::getColumnWidth(static_cast<FieldNames::Fields>(col), settings);
            crlfTableView->setColumnWidth(col, width);
        }
    }
}

// Creates a single window displaying all CRLF messages
void CrlfFilterWindow::createCrlfWindow() {
    // Validate prerequisites
    if (!m_dltFile || m_dltFile->size() == 0) {
        QMessageBox::information(nullptr, "No DLT file", "No DLT file is currently loaded.");
        return;
    }
    
    if (!m_sourceModelOfDLT) {
        QMessageBox::critical(nullptr, "Error", "No source model available for CRLF filtering.");
        return;
    }
    
    if (crlfWindow && crlfWindow->isVisible()) {
        crlfWindow->raise();
        crlfWindow->activateWindow();
        return;
    }

    QWidget* parentWidget = qobject_cast<QWidget*>(parent());
    
    // Create the model if it doesn't exist
    if (!m_crlfProjectionModel) {
        m_crlfProjectionModel = new CProjectionTableModel(this);
    }
    m_crlfProjectionModel->setSourceModel(m_sourceModelOfDLT);
    
    // Check if no filtered messages exist
    int totalFilteredMessages = m_dltFile->sizeFilter();
    if (totalFilteredMessages == 0) {
        QMessageBox::information(parentWidget, "No Messages", "No messages are available for CRLF filtering.");
        return;
    }

    if(crlfFilterProxy->rowCount() == 0)
    {
        QMessageBox::information(parentWidget, "No CRLF Messages", 
            "No messages containing CRLF characters (\\r, \\n, or \\r\\n) were found in the current DLT file.");
        return;
    }

    crlfWindow = new QWidget(parentWidget);
    crlfWindow->setAttribute(Qt::WA_DeleteOnClose);
    crlfWindow->resize(1200, 700);
    
    connect(crlfWindow, &QWidget::destroyed, this, &CrlfFilterWindow::cleanup);
    connect(crlfWindow, &QWidget::destroyed, this, &QObject::deleteLater);
    
    crlfWindow->setWindowFlags(Qt::Window);
    
    QVBoxLayout* layout = new QVBoxLayout(crlfWindow);

    QToolBar* toolbar = new QToolBar;
    QHBoxLayout* topRowLayout = new QHBoxLayout();
    topRowLayout->addStretch();
    topRowLayout->addWidget(toolbar);
    layout->addLayout(topRowLayout);

    QPushButton* exportButton = new QPushButton("Export CRLF Messages");
    exportButton->setToolTip("Export all CRLF messages to DLT file");
    toolbar->addWidget(exportButton);
    connect(exportButton, &QPushButton::clicked, this, &CrlfFilterWindow::onExportFilteredCrlfLogsClicked);

    crlfTableView = new QTableView;
    crlfTableView->setModel(crlfFilterProxy);
    crlfTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    crlfTableView->setAlternatingRowColors(true);
    crlfTableView->verticalHeader()->setVisible(false);
    crlfTableView->setSortingEnabled(false);
    crlfTableView->horizontalHeader()->setSortIndicatorShown(false);
    crlfTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    crlfTableView->horizontalHeader()->setStretchLastSection(true); // Enable stretching for better payload display
    crlfTableView->setWordWrap(false); // Disable word wrap but allow horizontal scrolling
    applyColumnSettings();
    
    connect(crlfTableView, &QTableView::doubleClicked, this, &CrlfFilterWindow::onCrlfMessageDoubleClicked);
    layout->addWidget(crlfTableView);
      
    statusLabel = new QLabel();
    
    // Apply theme-appropriate styling
    QPalette palette = statusLabel->palette();
    bool isDarkMode = palette.color(QPalette::Window).lightness() < palette.color(QPalette::WindowText).lightness();
    statusLabel->setStyleSheet(isDarkMode ? 
        "QLabel { padding: 5px; background-color: #3c3c3c; border-top: 1px solid #555; color: #ffffff; }" :
        "QLabel { padding: 5px; background-color: #f0f0f0; border-top: 1px solid #ccc; color: #000000; }");
    
    layout->addWidget(statusLabel);

    // Update window title and status with message count
    updateMessageCount(m_crlfProjectionModel->rowCount());
    
    // Apply column settings and initialize tracking state
    applyColumnSettings();
    m_lastFilteredMessageCount = m_dltFile->sizeFilter();

    crlfWindow->show();
    crlfWindow->raise();
    crlfWindow->activateWindow();
}

// Exports all filtered CRLF DLT logs to a file
void CrlfFilterWindow::onExportFilteredCrlfLogsClicked() {
    if (!m_dltFile || !m_crlfProjectionModel || !m_sourceModelOfDLT || !m_crlfTableView || !m_crlfWindow) {
        QMessageBox::information(nullptr, "Export Error", "No CRLF data available to export or window is not properly initialized.");
        return;
    }
    
    int rowCount = crlfFilterProxy->rowCount();
    if (rowCount == 0) {
        QMessageBox::information(crlfWindow, "Export", "No CRLF messages to export.");
        return;
    }
    
    QString fileName = QFileDialog::getSaveFileName(crlfWindow, "Export CRLF Messages", 
                                                    "crlf_messages.dlt", 
                                                    "DLT Files (*.dlt);;All Files (*)");
    if (fileName.isEmpty()) {
        return;
    }

    QProgressDialog* progress = nullptr;
    bool showExportProgress = !isMainWindowBusy();
    
    if (showExportProgress) {
        progress = new QProgressDialog("Exporting CRLF messages...", "Cancel", 0, rowCount, crlfWindow);
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumDuration(0);  // Show immediately
        progress->setValue(0);  // Initialize progress value
        progress->show();
        QCoreApplication::processEvents();  // Force progress dialog to appear
    }

    try {
        QModelIndexList selectedIndices;
        selectedIndices.reserve(rowCount);

        for (int row = 0; row < rowCount; ++row) {
            if (progress && progress->wasCanceled()) {
                return;
            }

            const int sourceRow = m_crlfProjectionModel->sourceRowForRow(row);
            const QModelIndex sourceIndex = m_sourceModelOfDLT->index(sourceRow, 0);
            if (sourceIndex.isValid()) {
                selectedIndices.append(sourceIndex);
            }

            if (progress && (row % 5 == 0 || row == rowCount - 1)) {
                progress->setValue(row + 1);
                progress->setLabelText(QString("Processing message %1 of %2...").arg(row + 1).arg(rowCount));
                QCoreApplication::processEvents();
            }
        }

        if (selectedIndices.isEmpty()) {
            if (progress) {
                progress->close();
                delete progress;
            }
            QMessageBox::warning(crlfWindow, "Export Error", "No valid CRLF messages found to export.");
            return;
        }

        if (progress) {
            progress->setLabelText("Creating DLT file...");
            progress->setRange(0, 0);  // Indeterminate progress for file export
        }
        QCoreApplication::processEvents();

        QDltExporter* exporter = new QDltExporter(
            m_dltFile,                                    // Source DLT file
            fileName,                                   // Output filepath
            m_pluginManager,                              // Plugin manager (can be nullptr)
            QDltExporter::FormatDlt,                    // Export in DLT format
            QDltExporter::SelectionSelected,            // Export selected messages
            &selectedIndices,                           // List of valid model indices
            0,                                          // Automatic time settings
            0,                                          // UTC offset
            0,                                          // DST
            ',',                                        // Delimiter (not used for DLT format)
            "DLTVIEWER",                                // Signature
            nullptr                                     // No parent to avoid threading issues
            );

        exporter->exportMessages();
        delete exporter;

        if (progress) {
            progress->setLabelText("Export completed successfully!");
            QCoreApplication::processEvents();
            progress->close();
            delete progress;
        }
        QMessageBox::information(crlfWindow, "Export Complete", 
                               QString("Successfully exported %1 CRLF messages to %2")
                               .arg(selectedIndices.size()).arg(fileName));

    } catch (const std::exception &e) {
        if (progress) {
            progress->close();
            delete progress;
        }
        QMessageBox::critical(crlfWindow, "Export Error", QString("Failed to export: %1").arg(e.what()));
    } catch (...) {
        if (progress) {
            progress->close();
            delete progress;
        }
        QMessageBox::critical(crlfWindow, "Export Error", "An unexpected error occurred during export.");
    }
}

// Sets the source model for DLT data
void CrlfFilterWindow::setSourceModel(QAbstractTableModel* model) {
    // Disconnect from previous model if any
    if (m_sourceModelOfDLT) {
        disconnect(m_sourceModelOfDLT, nullptr, this, nullptr);
    }
    
    m_sourceModelOfDLT = model;
    
    if (m_sourceModelOfDLT) {
        connect(m_sourceModelOfDLT, &QAbstractTableModel::modelReset, this, &CrlfFilterWindow::onSourceModelReset);
        connect(m_sourceModelOfDLT, &QAbstractTableModel::layoutChanged, this, &CrlfFilterWindow::onSourceModelDataChanged);
        
        if (QObject* parentObj = parent()) {
            disconnect(parentObj, SIGNAL(dltFileLoaded()), this, SLOT(onSourceModelDataChanged()));
            connect(parentObj, SIGNAL(dltFileLoaded()), this, SLOT(onSourceModelDataChanged()));
        }
    }

    if(crlfFilterProxy)
    {
        crlfFilterProxy->setSourceModel(sourceModelOfDLT);
    }
}

// Sets the DLT file reference
void CrlfFilterWindow::setDltFile(QDltFile* file) {
    m_dltFile = file;
    m_lastFilteredMessageCount = -1;  // Reset tracking state
    
    // Update window if visible and file is available
    if (m_crlfWindow && m_crlfWindow->isVisible() && m_dltFile) {
        if (m_dltFile->size() == 0) {
            onSourceModelReset();
        } else if (!rebuildScheduled && !rebuildTimer->isActive() && !rebuildInProgress) {
            rebuildScheduled = true;
            rebuildTimer->start();
        }
    }
}

// Sets the plugin manager reference
void CrlfFilterWindow::setPluginManager(QDltPluginManager* manager) {
    m_pluginManager = manager;
}

void CrlfFilterWindow::setMessageStore(CMessageStore *messageStore)
{
    this->m_messageStore = messageStore;
}

void CrlfFilterWindow::setIndexService(const CIndexService *indexService)
{
    this->m_indexService = indexService;
}

void CrlfFilterWindow::setDecodeCacheService(CDecodeCacheService *decodeCacheService)
{
    m_externalDecodeCacheService = decodeCacheService;
}

// Cleanup method to properly disconnect from models/signals
void CrlfFilterWindow::cleanup() {
    rebuildScheduled = false;
    rebuildInProgress = false;
    lastBuildCanceled = false;
    
    if (rebuildTimer && rebuildTimer->isActive()) {
        rebuildTimer->stop();
    }
    
    // Disconnect from source model to prevent further updates
    if (m_sourceModelOfDLT) {
        disconnect(m_sourceModelOfDLT, nullptr, this, nullptr);
    }
    
    if (crlfTableView && crlfFilterProxy) {
        crlfTableView->setModel(nullptr);
    }
    
    if (crlfFilterProxy) {
        crlfFilterProxy->deleteLater();
    }
    
    // Reset all pointers (no individual null checks needed)
    m_crlfTableView = nullptr;
    m_crlfProjectionModel = nullptr;
    m_statusLabel = nullptr;
    m_sourceModelOfDLT = nullptr;
    m_crlfWindow = nullptr;
    m_dltFile = nullptr;
    m_pluginManager = nullptr;
}

// Handle double-click on CRLF message row to navigate to main window
void CrlfFilterWindow::onCrlfMessageDoubleClicked(const QModelIndex& index) {
    if (!index.isValid() || !m_crlfProjectionModel || !m_dltFile) {
        return;
    }
    
    const int sourceRow = crlfFilterProxy->sourceRowAt(index.row());
    if(sourceRow < 0 || !dltFile)
    {
        return;
    }

    CIndexService localIndexService;
    const CIndexService *activeIndexService = m_indexService ? m_indexService : &localIndexService;
    const std::vector<int> filteredProjection =
        activeIndexService->snapshotProjection(buildActiveFilteredProjection(m_dltFile));

    if (sourceRow >= static_cast<int>(filteredProjection.size())) {
        return;
    }

    const int actualPosition = filteredProjection.at(static_cast<std::size_t>(sourceRow));
    if (actualPosition < 0 || actualPosition >= m_dltFile->size()) {
        return;
    }

    emit jumpToMessageRequested(absolutePosition);

    if (QWidget* parentWidget = qobject_cast<QWidget*>(parent())) {
        parentWidget->raise();
        parentWidget->activateWindow();
    }
}

// Handle when source model data changes
void CrlfFilterWindow::onSourceModelDataChanged() {
    // Early returns for invalid states
    if (!m_crlfWindow || !m_crlfWindow->isVisible() || !m_dltFile) {
        if (!m_dltFile && m_crlfWindow && m_crlfWindow->isVisible()) {
            onSourceModelReset();
        }
        return;
    }
    
    // Handle empty file state immediately
    int currentFilteredCount = m_dltFile->sizeFilter();
    if (m_dltFile->size() == 0 || currentFilteredCount == 0) {
        m_lastFilteredMessageCount = 0;
        onSourceModelReset();
        return;
    }
    
    // Check for significant data changes that require cache invalidation
    int significantChange = abs(currentFilteredCount - lastFilteredMessageCount);
    
    // For filter changes, always invalidate cache since different messages may be visible even if the count is similar
    if (lastFilteredMessageCount > 0 && significantChange > 0) {
        // Any change in filtered count means different messages are visible - invalidate cache
        this->invalidateCache();
    }
    bool countChanged = (currentFilteredCount != lastFilteredMessageCount);
    
    // Additional validation: During model transitions, delay rebuild for stability
    if (m_sourceModelOfDLT && m_sourceModelOfDLT->rowCount() != currentFilteredCount) {
        // Model is in transition - schedule rebuild with delay for stability
        if (!rebuildScheduled && !rebuildTimer->isActive() && !rebuildInProgress) {
            rebuildScheduled = true;
            rebuildTimer->setInterval(750);
            rebuildTimer->start();
        }
        return;
    }

    rebuildTimer->setInterval(500);

    if (!rebuildScheduled && !rebuildTimer->isActive() && !rebuildInProgress) {
        rebuildScheduled = true;
        rebuildTimer->start();
    }
}

// Handle when source model is reset/cleared
void CrlfFilterWindow::onSourceModelReset() {
    if (rebuildTimer->isActive()) {
        rebuildTimer->stop();
    }
    rebuildScheduled = false;
    rebuildInProgress = false;
    lastFilteredMessageCount = -1;

    if (!crlfWindow || !crlfWindow->isVisible() || !crlfFilterProxy) {
        return;
    }
    
    // Don't show empty window during transitions - schedule rebuild instead
    if (m_dltFile && m_dltFile->sizeFilter() > 0) {
        // Schedule rebuild rather than showing empty window
        rebuildScheduled = true;
        rebuildTimer->start();
    } else {
        crlfFilterProxy->setRowReferences(QVector<int>());

        if (crlfTableView) {
            applyColumnSettings();
        }
        updateMessageCount(0);
    }
}

// Rebuild the CRLF data model with current DLT file data
void CrlfFilterWindow::rebuildCrlfModel() {
    if (!crlfFilterProxy) {
        return;
    }
    
    if (!m_dltFile || m_dltFile->size() == 0) {
        // No file or empty file - clear the model
        crlfFilterProxy->removeRows(0, crlfFilterProxy->rowCount());
        updateMessageCount(0);
        return;
    }
    
    // Clear existing data
    crlfFilterProxy->removeRows(0, crlfFilterProxy->rowCount());
    
    // Check if no filtered messages exist
    int totalFilteredMessages = m_dltFile->sizeFilter();
    if (totalFilteredMessages == 0) {
        crlfFilterProxy->setRowReferences(QVector<int>());
        updateMessageCount(0);
        lastFilteredMessageCount = 0;
        return;
    }

    const int rowsToProcess = qMin(totalFilteredMessages, sourceModelOfDLT ? sourceModelOfDLT->rowCount() : 0);

    QVector<int> crlfRows;
    crlfRows.reserve(qMax(1, rowsToProcess / 10));

    bool needsProgress = (totalFilteredMessages > 2000) && !isMainWindowBusy();

    QProgressDialog* buildProgress = nullptr;
    if (needsProgress) {
        buildProgress = new QProgressDialog("Rebuilding CRLF data...", "Cancel", 0, totalFilteredMessages, crlfWindow);
        buildProgress->setWindowModality(Qt::WindowModal);
        buildProgress->setMinimumDuration(0);
        buildProgress->show();
    }

    int processCount = 0;
    for (int i = 0; i < rowsToProcess; i++) {
        if (buildProgress && buildProgress->wasCanceled()) {
            lastBuildCanceled = true;
            crlfFilterProxy->setRowReferences(QVector<int>());
            buildProgress->close();
            delete buildProgress;
            lastFilteredMessageCount = totalFilteredMessages;
            return;
        }

        if(dltFile)
        {
            const int absoluteRow = dltFile->getMsgFilterPos(i);
            if(absoluteRow >= 0 && absoluteRow < dltFile->size())
            {
                QDltMsg msg;
                if(dltFile->getMsg(absoluteRow, msg))
                {
                    msg.setIndex(absoluteRow);
                    if(pluginManager &&
                       QDltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool())
                    {
                        pluginManager->decodeMsg(msg, false);
                    }

                    const QString decodedPayload = msg.toStringPayload();
                    if(containsCrlf(decodedPayload))
                    {
                        crlfRows.append(i);
                    }
                }
            }
        }

        processCount++;
        if (buildProgress && processCount % 200 == 0) {
            buildProgress->setValue(i);
            QCoreApplication::processEvents();
        }
    }

    if (buildProgress) {
        buildProgress->close();
        delete buildProgress;
    }

    crlfFilterProxy->setRowReferences(crlfRows);

    updateMessageCount(crlfRows.size());
    lastFilteredMessageCount = totalFilteredMessages;

    applyColumnSettings();
}

// Debounced rebuild triggered by timer
void CrlfFilterWindow::onRebuildTimerTimeout() {
    rebuildScheduled = false;
    
    if (!crlfWindow || !crlfWindow->isVisible()) {
        return;
    }
    
    if (rebuildInProgress) {
        return;
    }
    
    if (!m_dltFile || m_dltFile->size() == 0) {
        onSourceModelReset();
    } else {
        rebuildCrlfModel();
    }
}

// Public method to refresh the CRLF window with latest data
void CrlfFilterWindow::refreshWindow() {
    if (m_crlfWindow && m_dltFile && m_crlfProjectionModel) {
        m_crlfProjectionModel->setSourceModel(m_sourceModelOfDLT);
        rebuildCrlfModel();
    } else if (!m_crlfWindow && m_dltFile) {
        // Window was closed but object still exists - recreate the window
        createCrlfWindow();
    }
}

// Public method to show and activate the CRLF window
void CrlfFilterWindow::showAndActivate() {
    if (crlfWindow) {
        crlfWindow->activateWindow();
        crlfWindow->raise();
        crlfWindow->show();
    } else if (dltFile) {
        createCrlfWindow();
    }
}

// Public method to close the CRLF window
void CrlfFilterWindow::closeWindow() {
    if (crlfWindow) {
        crlfWindow->close();
    }
}

std::vector<int> CrlfFilterWindow::buildCrlfProjectionRows(QWidget *progressParent,
                                                           const QString &progressLabel,
                                                           bool *wasCancelled)
{
    std::vector<int> rows;
    if (wasCancelled)
        *wasCancelled = false;

    if (!m_dltFile)
        return rows;

    CIndexService localIndexService;
    const CIndexService *activeIndexService = m_indexService ? m_indexService : &localIndexService;
    const std::vector<int> filteredProjection =
        activeIndexService->snapshotProjection(buildActiveFilteredProjection(m_dltFile));
    const int totalFilteredMessages = static_cast<int>(filteredProjection.size());
    rows.reserve(static_cast<std::size_t>(qMax(0, totalFilteredMessages / 8)));

    const bool decodeEnabled = QDltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool();
    const int triggeredByUser = !QDltOptManager::getInstance()->issilentMode();

    QProgressDialog buildProgress(progressLabel, "Cancel", 0, totalFilteredMessages, progressParent);
    buildProgress.setWindowModality(Qt::WindowModal);
    buildProgress.setMinimumDuration(0);
    buildProgress.show();

    CDecodeCacheService *activeDecodeCache = m_externalDecodeCacheService ? m_externalDecodeCacheService : &m_decodeCacheService;
    QDltMsg msg;
    for (int sourceRow = 0; sourceRow < totalFilteredMessages; ++sourceRow) {
        if (buildProgress.wasCanceled()) {
            if (wasCancelled)
                *wasCancelled = true;
            break;
        }

        const int globalIndex = filteredProjection.at(static_cast<std::size_t>(sourceRow));
        if (globalIndex < 0)
            continue;

        bool gotMessage = false;
        if (activeDecodeCache) {
            gotMessage = activeDecodeCache->message(m_dltFile,
                                                    m_pluginManager,
                                                    globalIndex,
                                                    decodeEnabled,
                                                    triggeredByUser,
                                                    msg,
                                                    true);
        }

        if (!gotMessage && m_messageStore) {
            const MessageId messageId = m_messageStore->messageIdForGlobalIndex(globalIndex);
            gotMessage = (messageId != kInvalidMessageId) && m_messageStore->message(messageId, msg);
        }
        }
    }
    
    return data;
}

// Clear cache when file structure changes
void CrlfFilterWindow::invalidateCache() {
    crlfCache.clear();
    messageDataCache.clear();
    bulkCrlfIndexBuilt = false;
    lastCacheValidCount = -1;
}

// Build bulk CRLF index for all messages (one-time operation)
void CrlfFilterWindow::buildBulkCrlfIndex() {
    if (bulkCrlfIndexBuilt || !dltFile) {
        return;
    }
    
    crlfCache.clear();  // Clear existing cache before rebuilding
    int totalMessages = dltFile->sizeFilter();
    
    // Only show progress for large files
    QProgressDialog* progress = nullptr;
    if (totalMessages > 1000 && !isMainWindowBusy()) {
        QWidget* parentWidget = qobject_cast<QWidget*>(parent());
        progress = new QProgressDialog("Building CRLF index...", "Cancel", 0, totalMessages, parentWidget);
        progress->setWindowModality(Qt::ApplicationModal);
        progress->setMinimumDuration(0);
        progress->show();
    }
    
    // Process messages in chunks to reduce UI blocking
    const int chunkSize = 100;
    
    for (int i = 0; i < totalMessages; i += chunkSize) {
        if (progress && progress->wasCanceled()) {
            delete progress;
            return;
        }
        
        int endIndex = qMin(i + chunkSize, totalMessages);
        
        for (int j = i; j < endIndex; j++) {
            int actualPos = dltFile->getMsgFilterPos(j);
            if (actualPos >= 0 && actualPos < dltFile->size()) {
                QDltMsg msg;
                if (dltFile->getMsg(actualPos, msg)) {
                    if (pluginManager) {
                        pluginManager->decodeMsg(msg, true);
                    }
                    QString rawPayload = msg.toStringPayload();
                    if (containsCrlf(rawPayload)) {
                        crlfCache[actualPos] = true;   // Positive cache entry
                    } else {
                        crlfCache[actualPos] = false;  // Negative cache entry
                    }
                }
            }
        }
        
        if (progress) {
            progress->setValue(endIndex);
            QCoreApplication::processEvents();
        }
    }
    
    if (progress) {
        progress->close();
        delete progress;
    }
    
    bulkCrlfIndexBuilt = true;
}
