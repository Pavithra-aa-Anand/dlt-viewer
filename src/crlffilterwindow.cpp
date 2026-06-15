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
#include "indexservice.h"
#include "qdltfileprojection.h"
#include "qdltfile.h"
#include "qdltexporter.h"
#include "qdltoptmanager.h"
#include "qdltsettingsmanager.h"

CrlfFilterWindow::CrlfFilterWindow(QObject* parent) : QObject(parent) {
    sourceModelOfDLT = nullptr;
    m_crlfProjectionModel = nullptr;
    m_crlfWindow = nullptr;
    m_crlfTableView = nullptr;
    m_statusLabel = nullptr;
    dltFile = nullptr;
    pluginManager = nullptr;
    messageStore = nullptr;
    indexService = nullptr;
    m_externalDecodeCacheService = nullptr;
    
    // Initialize debouncing mechanism
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(500);
    connect(m_rebuildTimer, &QTimer::timeout, this, &CrlfFilterWindow::onRebuildTimerTimeout);
    
    m_lastFilteredMessageCount = -1;
    m_rebuildScheduled = false;
    m_rebuildInProgress = false;
}

void CrlfFilterWindow::invalidateCache() {
    crlfCache.clear();
    messageDataCache.clear();
    lastCacheValidCount = -1;
    bulkCrlfIndexBuilt = false;
    CDecodeCacheService *activeDecodeCache = m_externalDecodeCacheService ? m_externalDecodeCacheService : &decodeCacheService;
    if (dltFile) {
        activeDecodeCache->clearForFile(dltFile);
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
    if (m_crlfWindow) {
        m_crlfWindow->setWindowTitle(countText);
    }
    if (m_statusLabel) {
        m_statusLabel->setText(QString("Total CRLF messages: %1 ").arg(count));
    }
}

// Apply column settings to table view
void CrlfFilterWindow::applyColumnSettings() {
    if (!m_crlfTableView || !m_crlfProjectionModel) {
        return;
    }
    
    auto settings = QDltSettingsManager::getInstance();
    for (int col = 0; col < m_crlfProjectionModel->columnCount(); ++col) {
        bool show = FieldNames::getColumnShown(static_cast<FieldNames::Fields>(col), settings);
        m_crlfTableView->setColumnHidden(col, !show);
        if (show) {
            int width = FieldNames::getColumnWidth(static_cast<FieldNames::Fields>(col), settings);
            m_crlfTableView->setColumnWidth(col, width);
        }
    }
}

// Creates a single window displaying all CRLF messages
void CrlfFilterWindow::createCrlfWindow() {
    // Validate prerequisites
    if (!dltFile || dltFile->size() == 0) {
        QMessageBox::information(nullptr, "No DLT file", "No DLT file is currently loaded.");
        return;
    }
    
    if (!sourceModelOfDLT) {
        QMessageBox::critical(nullptr, "Error", "No source model available for CRLF filtering.");
        return;
    }
    
    // Check if a CRLF window is already open (prevent multiple instances)
    if (m_crlfWindow && m_crlfWindow->isVisible()) {
        m_crlfWindow->raise();
        m_crlfWindow->activateWindow();
        return;
    }

    // First, prepare the data with progress dialog before creating window
    QWidget* parentWidget = qobject_cast<QWidget*>(parent());
    
    // Create the model if it doesn't exist
    if (!m_crlfProjectionModel) {
        m_crlfProjectionModel = new ProjectionTableModel(this);
    }
    m_crlfProjectionModel->setSourceModel(sourceModelOfDLT);
    
    // Check if no filtered messages exist
    int totalFilteredMessages = dltFile->sizeFilter();
    if (totalFilteredMessages == 0) {
        QMessageBox::information(parentWidget, "No Messages", "No messages are available for CRLF filtering.");
        return;
    }
    
    bool cancelled = false;
    const std::vector<int> projectionRows = buildCrlfProjectionRows(parentWidget, "Preparing CRLF data...", &cancelled);
    if (cancelled) {
        m_crlfProjectionModel->clearProjection();
        return;
    }
    m_crlfProjectionModel->setProjectionRows(projectionRows);
    const int addedCount = static_cast<int>(projectionRows.size());
    
    // Check if any CRLF messages were found
    if (addedCount == 0) {
        QMessageBox::information(parentWidget, "No CRLF Messages", 
            "No messages containing CRLF characters (\\r, \\n, or \\r\\n) were found in the current DLT file.");
        return;
    }

    // Data preparation successful, now create and show the window
    m_crlfWindow = new QWidget(parentWidget);
    m_crlfWindow->setAttribute(Qt::WA_DeleteOnClose);
    m_crlfWindow->resize(1200, 700);
    
    // Connect window close event to cleanup
    connect(m_crlfWindow, &QWidget::destroyed, this, &CrlfFilterWindow::cleanup);
    connect(m_crlfWindow, &QWidget::destroyed, this, &QObject::deleteLater);
    
    m_crlfWindow->setWindowFlags(Qt::Window);
    
    // Create layout and UI components
    QVBoxLayout* layout = new QVBoxLayout(m_crlfWindow);

    // Add toolbar with export button
    QToolBar* toolbar = new QToolBar;
    QHBoxLayout* topRowLayout = new QHBoxLayout();
    topRowLayout->addStretch();
    topRowLayout->addWidget(toolbar);
    layout->addLayout(topRowLayout);

    QPushButton* exportButton = new QPushButton("Export CRLF Messages");
    exportButton->setToolTip("Export all CRLF messages to DLT file");
    toolbar->addWidget(exportButton);
    connect(exportButton, &QPushButton::clicked, this, &CrlfFilterWindow::onExportFilteredCrlfLogsClicked);

    // Create table view with the already prepared model
    m_crlfTableView = new QTableView;
    m_crlfTableView->setModel(m_crlfProjectionModel);
    m_crlfTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_crlfTableView->setAlternatingRowColors(true);
    m_crlfTableView->verticalHeader()->setVisible(false);
    m_crlfTableView->setSortingEnabled(false);
    m_crlfTableView->horizontalHeader()->setSortIndicatorShown(false);
    m_crlfTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_crlfTableView->horizontalHeader()->setStretchLastSection(true); // Enable stretching for better payload display
    m_crlfTableView->setWordWrap(false); // Disable word wrap but allow horizontal scrolling
    // Apply column settings like main window
    applyColumnSettings();
    
    connect(m_crlfTableView, &QTableView::doubleClicked, this, &CrlfFilterWindow::onCrlfMessageDoubleClicked);
    layout->addWidget(m_crlfTableView);
      
    // Add status bar
    m_statusLabel = new QLabel();
    
    // Apply theme-appropriate styling
    QPalette palette = m_statusLabel->palette();
    bool isDarkMode = palette.color(QPalette::Window).lightness() < palette.color(QPalette::WindowText).lightness();
    m_statusLabel->setStyleSheet(isDarkMode ? 
        "QLabel { padding: 5px; background-color: #3c3c3c; border-top: 1px solid #555; color: #ffffff; }" :
        "QLabel { padding: 5px; background-color: #f0f0f0; border-top: 1px solid #ccc; color: #000000; }");
    
    layout->addWidget(m_statusLabel);

    // Update window title and status with message count
    updateMessageCount(m_crlfProjectionModel->rowCount());
    
    // Apply column settings and initialize tracking state
    applyColumnSettings();
    m_lastFilteredMessageCount = dltFile->sizeFilter();

    // Show window after everything is prepared
    m_crlfWindow->show();
    m_crlfWindow->raise();
    m_crlfWindow->activateWindow();
}

// Exports all filtered CRLF DLT logs to a file
void CrlfFilterWindow::onExportFilteredCrlfLogsClicked() {
    if (!dltFile || !m_crlfProjectionModel || !sourceModelOfDLT || !m_crlfTableView || !m_crlfWindow) {
        QMessageBox::information(nullptr, "Export Error", "No CRLF data available to export or window is not properly initialized.");
        return;
    }
    
    int rowCount = m_crlfProjectionModel->rowCount();
    if (rowCount == 0) {
        QMessageBox::information(m_crlfWindow, "Export", "No CRLF messages to export.");
        return;
    }
    
    QString fileName = QFileDialog::getSaveFileName(m_crlfWindow, "Export CRLF Messages", 
                                                    "crlf_messages.dlt", 
                                                    "DLT Files (*.dlt);;All Files (*)");
    if (fileName.isEmpty()) {
        return;
    }

    QProgressDialog progress("Exporting CRLF messages...", "Cancel", 0, rowCount, m_crlfWindow);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();

    try {
        QModelIndexList selectedIndices;
        selectedIndices.reserve(rowCount);

        // Map projection rows back to source model rows for export.
        for (int row = 0; row < rowCount; ++row) {
            if (progress.wasCanceled()) {
                return;
            }

            const int sourceRow = m_crlfProjectionModel->sourceRowForRow(row);
            const QModelIndex sourceIndex = sourceModelOfDLT->index(sourceRow, 0);
            if (sourceIndex.isValid()) {
                selectedIndices.append(sourceIndex);
            }
            
            // Update progress more frequently to ensure visibility
            if (row % 5 == 0 || row == rowCount - 1) {
                progress.setValue(row + 1);
                progress.setLabelText(QString("Processing message %1 of %2...").arg(row + 1).arg(rowCount));
                QCoreApplication::processEvents();
            }
        }
        
        if (selectedIndices.isEmpty()) {
            progress.close();
            QMessageBox::warning(m_crlfWindow, "Export Error", "No valid CRLF messages found to export.");
            return;
        }

        const int foundMessages = selectedIndices.size();
        const QString exportMessage = QString("Found %1 CRLF messages in current view").arg(foundMessages);
        
        // Create and configure the exporter with proper DLT format support
        progress.setLabelText("Creating DLT file...");
        progress.setRange(0, 0);  // Indeterminate progress for file export
        QCoreApplication::processEvents();
        
        QDltExporter* exporter = new QDltExporter(
            dltFile,                                    // Source DLT file
            fileName,                                   // Output filepath
            pluginManager,                              // Plugin manager (can be nullptr)
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
        
        // Export the messages
        exporter->exportMessages();
        delete exporter;

        progress.close();
        QMessageBox::information(m_crlfWindow, "Export Complete", 
                               QString("Successfully exported %1 CRLF messages to %2\n\n%3")
                               .arg(foundMessages).arg(fileName).arg(exportMessage));

    } catch (const std::exception &e) {
        progress.close();
        QMessageBox::critical(m_crlfWindow, "Export Error", QString("Failed to export: %1").arg(e.what()));
    } catch (...) {
        progress.close();
        QMessageBox::critical(m_crlfWindow, "Export Error", "An unexpected error occurred during export.");
    }
}

// Sets the source model for DLT data
void CrlfFilterWindow::setSourceModel(QAbstractTableModel* model) {
    // Disconnect from previous model if any
    if (sourceModelOfDLT) {
        disconnect(sourceModelOfDLT, nullptr, this, nullptr);
    }
    
    sourceModelOfDLT = model;
    
    if (sourceModelOfDLT) {
        connect(sourceModelOfDLT, &QAbstractTableModel::modelReset, this, &CrlfFilterWindow::onSourceModelReset);
        connect(sourceModelOfDLT, &QAbstractTableModel::layoutChanged, this, &CrlfFilterWindow::onSourceModelDataChanged);
        
        // Connect to parent's dltFileLoaded signal for file changes
        if (QObject* parentObj = parent()) {
            connect(parentObj, SIGNAL(dltFileLoaded()), this, SLOT(onSourceModelDataChanged()));
        }
    }
}

// Sets the DLT file reference
void CrlfFilterWindow::setDltFile(QDltFile* file) {
    dltFile = file;
    m_lastFilteredMessageCount = -1;  // Reset tracking state
    
    // Update window if visible and file is available
    if (m_crlfWindow && m_crlfWindow->isVisible() && dltFile) {
        if (dltFile->size() == 0) {
            onSourceModelReset();
        } else if (!m_rebuildScheduled && !m_rebuildTimer->isActive() && !m_rebuildInProgress) {
            m_rebuildScheduled = true;
            m_rebuildTimer->start();
        }
    }
}

// Sets the plugin manager reference
void CrlfFilterWindow::setPluginManager(QDltPluginManager* manager) {
    pluginManager = manager;
}

void CrlfFilterWindow::setMessageStore(CMessageStore *messageStore)
{
    messageStore = messageStore;
}

void CrlfFilterWindow::setIndexService(const CIndexService *indexService)
{
    indexService = indexService;
}

void CrlfFilterWindow::setDecodeCacheService(CDecodeCacheService *decodeCacheService)
{
    m_externalDecodeCacheService = decodeCacheService;
}

// Cleanup method to properly disconnect from models/signals
void CrlfFilterWindow::cleanup() {
    // Reset state flags first to prevent any new operations
    m_rebuildScheduled = false;
    m_rebuildInProgress = false;
    
    // Stop any pending rebuild operations
    if (m_rebuildTimer && m_rebuildTimer->isActive()) {
        m_rebuildTimer->stop();
    }
    
    // Disconnect from source model to prevent further updates
    if (sourceModelOfDLT) {
        disconnect(sourceModelOfDLT, nullptr, this, nullptr);
    }
    
    // Clean up UI components - only detach model if both objects exist
    if (m_crlfTableView && m_crlfProjectionModel) {
        m_crlfTableView->setModel(nullptr);
    }
    
    // Schedule proxy model for deletion
    if (m_crlfProjectionModel) {
        m_crlfProjectionModel->deleteLater();
    }
    
    // Reset all pointers (no individual null checks needed)
    m_crlfTableView = nullptr;
    m_crlfProjectionModel = nullptr;
    m_statusLabel = nullptr;
    sourceModelOfDLT = nullptr;
    m_crlfWindow = nullptr;
    dltFile = nullptr;
    pluginManager = nullptr;
}

// Handle double-click on CRLF message row to navigate to main window
void CrlfFilterWindow::onCrlfMessageDoubleClicked(const QModelIndex& index) {
    if (!index.isValid() || !m_crlfProjectionModel || !dltFile) {
        return;
    }

    const int sourceRow = m_crlfProjectionModel->sourceRowForRow(index.row());
    if (sourceRow < 0) {
        return;
    }

    CIndexService localIndexService;
    const CIndexService *activeIndexService = indexService ? indexService : &localIndexService;
    const std::vector<int> filteredProjection =
        activeIndexService->snapshotProjection(buildActiveFilteredProjection(dltFile));

    if (sourceRow >= static_cast<int>(filteredProjection.size())) {
        return;
    }

    const int actualPosition = filteredProjection.at(static_cast<std::size_t>(sourceRow));
    if (actualPosition < 0 || actualPosition >= dltFile->size()) {
        return;
    }

    emit jumpToMessageRequested(actualPosition);

    if (QWidget* parentWidget = qobject_cast<QWidget*>(parent())) {
        parentWidget->raise();
        parentWidget->activateWindow();
    }
}

// Handle when source model data changes
void CrlfFilterWindow::onSourceModelDataChanged() {
    // Early returns for invalid states
    if (!m_crlfWindow || !m_crlfWindow->isVisible() || !dltFile) {
        if (!dltFile && m_crlfWindow && m_crlfWindow->isVisible()) {
            onSourceModelReset();
        }
        return;
    }
    
    // Handle empty file state immediately
    int currentFilteredCount = dltFile->sizeFilter();
    if (dltFile->size() == 0 || currentFilteredCount == 0) {
        m_lastFilteredMessageCount = 0;
        onSourceModelReset();
        return;
    }
    
    // Check for significant data changes that require cache invalidation
    int significantChange = abs(currentFilteredCount - m_lastFilteredMessageCount);
    
    // For filter changes, always invalidate cache since different messages may be visible even if the count is similar
    if (m_lastFilteredMessageCount > 0 && significantChange > 0) {
        // Any change in filtered count means different messages are visible - invalidate cache
        this->invalidateCache();
    }
    
    // Additional validation: During model transitions, delay rebuild for stability
    if (sourceModelOfDLT && sourceModelOfDLT->rowCount() != currentFilteredCount) {
        // Model is in transition - schedule rebuild with delay for stability
        if (!m_rebuildScheduled && !m_rebuildTimer->isActive() && !m_rebuildInProgress) {
            m_rebuildScheduled = true;
            m_rebuildTimer->setInterval(750); // Longer delay for stability during transitions
            m_rebuildTimer->start();
        }
        return;
    }
    
    // Reset normal timer interval
    m_rebuildTimer->setInterval(500);
    
    // Even if the filtered count is unchanged, visible content may differ
    // (for example, filter criteria changed but cardinality stayed constant).
    // Rebuild to prevent stale CRLF rows.
    const bool countChanged = (currentFilteredCount != m_lastFilteredMessageCount);
    if (!countChanged) {
        this->invalidateCache();
    }
    
    // Avoid overlapping rebuild operations
    if (m_rebuildScheduled || m_rebuildTimer->isActive() || m_rebuildInProgress) {
        return;
    }
    
    // Schedule rebuild for both count changes and same-count content changes.
    m_rebuildScheduled = true;
    m_rebuildTimer->start();
}

// Handle when source model is reset/cleared
void CrlfFilterWindow::onSourceModelReset() {
    // Reset all state flags and counters
    if (m_rebuildTimer->isActive()) {
        m_rebuildTimer->stop();
    }
    m_rebuildScheduled = false;
    m_rebuildInProgress = false;
    m_lastFilteredMessageCount = -1;
    
    // Invalidate cache on model reset
    this->invalidateCache();
    
    // Early return if window not visible or proxy not available
    if (!m_crlfWindow || !m_crlfWindow->isVisible() || !m_crlfProjectionModel) {
        return;
    }
    
    // Don't show empty window during transitions - schedule rebuild instead
    if (dltFile && dltFile->sizeFilter() > 0) {
        // Schedule rebuild rather than showing empty window
        m_rebuildScheduled = true;
        m_rebuildTimer->start();
    } else {
        // Only clear if there's genuinely no data
        m_crlfProjectionModel->clearProjection();
        
        // Apply settings and update UI
        if (m_crlfTableView) {
            applyColumnSettings();
        }
        updateMessageCount(0);
    }
}

// Rebuild the CRLF data model with current DLT file data
void CrlfFilterWindow::rebuildCrlfModel() {
    if (!m_crlfProjectionModel) {
        return;
    }
    
    if (!dltFile || dltFile->size() == 0) {
        // No file or empty file - clear the model
        m_crlfProjectionModel->clearProjection();
        updateMessageCount(0);
        return;
    }
    
    // Check if no filtered messages exist
    int totalFilteredMessages = dltFile->sizeFilter();
    if (totalFilteredMessages == 0) {
        updateMessageCount(0);
        m_lastFilteredMessageCount = 0;
        return;
    }
    
    bool cancelled = false;
    const std::vector<int> projectionRows = buildCrlfProjectionRows(m_crlfWindow, "Rebuilding CRLF data...", &cancelled);
    if (cancelled) {
        m_crlfProjectionModel->clearProjection();
        updateMessageCount(0);
        m_lastFilteredMessageCount = totalFilteredMessages;
        return;
    }

    m_crlfProjectionModel->setProjectionRows(projectionRows);
    const int addedCount = static_cast<int>(projectionRows.size());
    
    // Update UI with final count
    updateMessageCount(addedCount);
    m_lastFilteredMessageCount = totalFilteredMessages;
    
    // Apply column settings
    applyColumnSettings();
}

// Debounced rebuild triggered by timer
void CrlfFilterWindow::onRebuildTimerTimeout() {
    m_rebuildScheduled = false;
    
    if (!m_crlfWindow || !m_crlfWindow->isVisible()) {
        return;
    }
    
    // Prevent overlapping rebuilds
    if (m_rebuildInProgress) {
        return;
    }
    
    if (!dltFile || dltFile->size() == 0) {
        onSourceModelReset();
    } else {
        m_rebuildInProgress = true;
        rebuildCrlfModel();
        m_rebuildInProgress = false;
    }
}

// Public method to refresh the CRLF window with latest data
void CrlfFilterWindow::refreshWindow() {
    if (m_crlfWindow && dltFile && m_crlfProjectionModel) {
        m_crlfProjectionModel->setSourceModel(sourceModelOfDLT);
        rebuildCrlfModel();
    } else if (!m_crlfWindow && dltFile) {
        // Window was closed but object still exists - recreate the window
        createCrlfWindow();
    }
}

// Public method to show and activate the CRLF window
void CrlfFilterWindow::showAndActivate() {
    if (m_crlfWindow) {
        m_crlfWindow->activateWindow();
        m_crlfWindow->raise();
        m_crlfWindow->show();
    }
}

// Public method to close the CRLF window
void CrlfFilterWindow::closeWindow() {
    if (m_crlfWindow) {
        m_crlfWindow->close();
    }
}

std::vector<int> CrlfFilterWindow::buildCrlfProjectionRows(QWidget *progressParent,
                                                           const QString &progressLabel,
                                                           bool *wasCancelled)
{
    std::vector<int> rows;
    if (wasCancelled)
        *wasCancelled = false;

    if (!dltFile)
        return rows;

    CIndexService localIndexService;
    const CIndexService *activeIndexService = indexService ? indexService : &localIndexService;
    const std::vector<int> filteredProjection =
        activeIndexService->snapshotProjection(buildActiveFilteredProjection(dltFile));
    const int totalFilteredMessages = static_cast<int>(filteredProjection.size());
    rows.reserve(static_cast<std::size_t>(qMax(0, totalFilteredMessages / 8)));

    const bool decodeEnabled = QDltSettingsManager::getInstance()->value("startup/pluginsEnabled", true).toBool();
    const int triggeredByUser = !QDltOptManager::getInstance()->issilentMode();

    QProgressDialog buildProgress(progressLabel, "Cancel", 0, totalFilteredMessages, progressParent);
    buildProgress.setWindowModality(Qt::WindowModal);
    buildProgress.setMinimumDuration(0);
    buildProgress.show();

    CDecodeCacheService *activeDecodeCache = m_externalDecodeCacheService ? m_externalDecodeCacheService : &decodeCacheService;
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
            gotMessage = activeDecodeCache->message(dltFile,
                                                    pluginManager,
                                                    globalIndex,
                                                    decodeEnabled,
                                                    triggeredByUser,
                                                    msg,
                                                    true);
        }

        if (!gotMessage && messageStore) {
            const MessageId messageId = messageStore->messageIdForGlobalIndex(globalIndex);
            gotMessage = (messageId != kInvalidMessageId) && messageStore->message(messageId, msg);
        }

        if (!gotMessage)
            continue;

        if (containsCrlf(msg.toStringPayload()))
            rows.push_back(sourceRow);

        if ((sourceRow % 100) == 0) {
            buildProgress.setValue(sourceRow);
            QCoreApplication::processEvents();
        }
    }

    buildProgress.setValue(totalFilteredMessages);
    buildProgress.close();
    return rows;
}

