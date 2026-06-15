#ifndef FILTERGROUPLOGS_H
#define FILTERGROUPLOGS_H

#include <QObject>
#include <QInputDialog>
#include <QFileDialog>
#include <QProgressDialog>
#include <QTableView>
#include <QAbstractTableModel>

#include <map>
#include <vector>

#include "qdltfile.h"
#include "qdltpluginmanager.h"
#include "projectiontablemodel.h"
#include "messagestore.h"
#include "indexservice.h"
#include "decodecacheservice.h"

class filtergrouplogs : public QObject {
    Q_OBJECT

public:
  //! Construct the ECU grouping helper.
  explicit filtergrouplogs(QObject* parent = nullptr);
  //! Extract unique ECU IDs from a DLT file path.
  QStringList extractEcuIds(const QString& dltFilePath);
  //! Set the source DLT table model.
  void setSourceModel(QAbstractTableModel* model);
  //! Set the active QDltFile instance.
  void setDltFile(QDltFile* dltFile);
  //! Set the plugin manager used for decoding.
  void setPluginManager(QDltPluginManager* pluginManager);
  //! Set the message store service used for message access.
  void setMessageStore(CMessageStore *messageStore);
  //! Set the shared index service.
  void setIndexService(const CIndexService *indexService);
  //! Set the shared decode cache service.
  void setDecodeCacheService(CDecodeCacheService *decodeCacheService);
  //! Build one tab per ECU ID.
  void ecuIdTabs();
  //! Open merge dialog for selecting ECU tabs.
  void openMergeTabsDialog();
  //! Merge selected ECU tabs into one combined tab.
  void mergeTabs();
  //! Handle tab close and update tracking state.
  void onTabCloseRequested(int index);
  //! Export the currently selected filtered ECU logs.
  void onExportFilteredLogsClicked();

  private :
    QAbstractTableModel* sourceModelOfDLT;
    QTabWidget* mergedTabWidget;
    QDltFile* dltFile;
    QDltPluginManager* pluginManager;
    CMessageStore *messageStore;
    const CIndexService *indexService;
    CDecodeCacheService *decodeCacheService;

    QMap<QString, QWidget*> mergedTabs;
    QMap<QWidget*, QStringList> tabToSelectedIds;
    QMap<int, QString> indexofMergedTabs;
    QMap<QString, QTableView*> ecuTabViews;
    std::map<QString, std::vector<int>> ecuSourceRowProjection;

    QSet<QString> selectedEcuIdSet;
    QStringList extractedEcuIds;

    int ecuColumnIndex = 4;
};

#endif // FILTERGROUPLOGS_H

