#include "manifest/gui/MainWindow.hpp"

#include "manifest/Database.hpp"
#include "manifest/HatariLauncher.hpp"
#include "manifest/Identifier.hpp"
#include "manifest/MenuDiskCatalog.hpp"
#include "manifest/MultiDiskDetector.hpp"
#include "manifest/Scanner.hpp"
#include "manifest/ScreenScraperCache.hpp"
#include "manifest/Version.hpp"
#include "manifest/gui/DiskTableModel.hpp"
#include "manifest/gui/FilterProxy.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QTextBrowser>
#include <QThread>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVariant>

namespace manifest::gui {

namespace {

constexpr const char* kSettingsOrg  = "ManifeST";
constexpr const char* kSettingsApp  = "ManifeST";
constexpr const char* kKeyGeometry  = "mainwindow/geometry";
constexpr const char* kKeyState     = "mainwindow/state";
constexpr const char* kKeyShowIdent    = "view/show_identified";
constexpr const char* kKeyShowContents = "view/show_contents";
constexpr const char* kKeyLastScan  = "scan/last_root";
constexpr const char* kKeyLastDb    = "db/last_path";

QString htmlEscape(const std::string& s) {
    return QString::fromStdString(s).toHtmlEscaped();
}

} // namespace

MainWindow::MainWindow(const QString& db_path, QWidget* parent)
    : QMainWindow(parent) {
    resize(1100, 700);

    dbPath_     = db_path;
    db_         = std::make_unique<Database>(db_path.toStdString());

    // Pick up data/tosec_titles.json if it's sitting alongside the binary
    // (either the source checkout layout or the installed location).
    std::optional<std::filesystem::path> tosec_json;
    const std::filesystem::path candidate = "data/tosec_titles.json";
    if (std::filesystem::exists(candidate)) tosec_json = candidate;
    identifier_ = std::make_unique<Identifier>(tosec_json);

    // Menu-disk catalog (Medway / Pompey / D-Bug / …) if present.
    const std::filesystem::path menu_json = "data/menu_disk_contents.json";
    if (std::filesystem::exists(menu_json)) {
        menu_catalog_ = std::make_unique<MenuDiskCatalog>(menu_json);
    }

    // ScreenScraper offline cache if present.
    const std::filesystem::path ss_json = "data/screenscraper_cache.json";
    if (std::filesystem::exists(ss_json)) {
        ss_cache_ = std::make_unique<ScreenScraperCache>(ss_json);
    }

    setWindowTitle(QStringLiteral("ManifeST — %1").arg(QFileInfo(dbPath_).fileName()));

    buildUi();
    buildSidebarDock();
    buildDetailDock();
    buildToolbar();
    buildMenus();
    buildStatusBar();
    buildScannerThread();
    refreshSidebar();
    restoreSettings();

    onSelectionChanged();
    updateDbDependentActionState();
}

MainWindow::~MainWindow() {
    if (scanThread_) {
        scanner_->requestCancel();
        scanThread_->quit();
        scanThread_->wait();
    }
}

void MainWindow::openDatabase(const QString& path) {
    if (!path.isEmpty() && path == dbPath_) return;

    // Stop the worker if one is running — it holds a Database reference
    // we're about to destroy. If we're currently in the "no DB open"
    // state there's no worker to tear down.
    if (scanThread_) {
        scanner_->requestCancel();
        scanThread_->quit();
        scanThread_->wait();
        scanThread_ = nullptr;
        scanner_    = nullptr;   // deleted via QThread::finished → deleteLater
    }

    std::unique_ptr<Database> new_db;
    try {
        new_db = std::make_unique<Database>(path.toStdString());
    } catch (const std::exception& e) {
        QMessageBox::warning(this, QStringLiteral("Open Database failed"),
            QString::fromStdString(e.what()));
        // Restore whatever scanner state we can: if we still have a DB,
        // rebuild the worker. Otherwise stay in the "no DB" state.
        if (db_) buildScannerThread();
        return;
    }

    db_      = std::move(new_db);
    dbPath_  = path;
    setWindowTitle(QStringLiteral("ManifeST — %1").arg(QFileInfo(dbPath_).fileName()));

    model_->setDatabase(*db_);
    model_->reload();
    proxy_->clearIdFilter();
    buildScannerThread();
    refreshSidebar();
    updateDbDependentActionState();
    statusLabel_->setText(QStringLiteral("Opened %1 — %2 disks")
        .arg(dbPath_).arg(model_->rowCount()));
}

void MainWindow::closeDatabase() {
    if (!db_) return;   // already closed

    if (scanThread_) {
        scanner_->requestCancel();
        scanThread_->quit();
        scanThread_->wait();
        scanThread_ = nullptr;
        scanner_    = nullptr;
    }

    const QString was = dbPath_;

    db_.reset();
    dbPath_.clear();

    model_->clearDatabase();
    proxy_->clearIdFilter();
    if (sidebar_) sidebar_->clear();
    onSelectionChanged();   // clears Details pane to "No selection"
    setWindowTitle(QStringLiteral("ManifeST — (no database)"));
    updateDbDependentActionState();

    statusLabel_->setText(QStringLiteral("Closed %1 — no database open").arg(was));
}

void MainWindow::updateDbDependentActionState() {
    const bool haveDb = static_cast<bool>(db_);
    actSaveDbAs_  ->setEnabled(haveDb);
    actCloseDb_   ->setEnabled(haveDb);
    actDeleteDb_  ->setEnabled(haveDb);
    actQuickScan_ ->setEnabled(haveDb);
    actDeepScan_  ->setEnabled(haveDb);
    actRescan_    ->setEnabled(haveDb && !lastScanRoot_.isEmpty());
    actLaunch_    ->setEnabled(haveDb && selectedModelRow() >= 0);
    actShowInFiles_->setEnabled(haveDb && selectedModelRow() >= 0);
}

void MainWindow::onCloseDatabase() {
    closeDatabase();
}

void MainWindow::onDeleteDatabase() {
    if (!db_ || dbPath_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Delete Database"),
            QStringLiteral("No database is currently open."));
        return;
    }

    // Collect the main DB + any SQLite WAL / SHM / legacy journal sidecars
    // so the warning can list exactly what's about to disappear.
    const QFileInfo main_fi(dbPath_);
    QStringList files_to_delete{dbPath_};
    for (const QString& suffix : {QStringLiteral("-wal"),
                                  QStringLiteral("-shm"),
                                  QStringLiteral("-journal")}) {
        const QString sp = dbPath_ + suffix;
        if (QFileInfo::exists(sp)) files_to_delete << sp;
    }

    const qint64 main_size = main_fi.size();
    const int    disk_rows = model_->rowCount();

    QMessageBox box(this);
    box.setIcon(QMessageBox::Critical);
    box.setWindowTitle(QStringLiteral("Delete Database — WARNING"));
    box.setText(QStringLiteral(
        "<b>This will permanently delete the currently open catalogue.</b>"));
    box.setInformativeText(QStringLiteral(
        "<p><b>Path:</b> %1<br>"
        "<b>Size:</b> %2 bytes<br>"
        "<b>Disks catalogued:</b> %3</p>"
        "<p><b>Files to be deleted:</b></p>"
        "<pre>%4</pre>"
        "<p>This <b>cannot be undone</b>. The image files on disk are "
        "<i>not</i> affected — only the catalogue database itself is removed.</p>"
        "<p>Your notes, tags, menu-disk content, detected games, and "
        "identification results will all be lost.</p>")
        .arg(main_fi.absoluteFilePath().toHtmlEscaped())
        .arg(main_size)
        .arg(disk_rows)
        .arg(files_to_delete.join('\n').toHtmlEscaped()));
    box.setStandardButtons(QMessageBox::Cancel);
    auto* deleteBtn = box.addButton(
        QStringLiteral("Delete Forever"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() != deleteBtn) {
        statusLabel_->setText(QStringLiteral("Delete cancelled."));
        return;
    }

    // Second friction gate — type the filename to confirm. Matches the
    // GitHub "type repo name to delete" pattern: specific, friction-ful,
    // prevents accidental clicks on a similar-sounding other DB.
    const QString basename = main_fi.fileName();
    bool ok = false;
    const QString typed = QInputDialog::getText(this,
        QStringLiteral("Confirm Delete"),
        QStringLiteral("Type the database filename exactly to confirm:\n\n<b>%1</b>")
            .arg(basename),
        QLineEdit::Normal, QString{}, &ok);
    if (!ok || typed.trimmed() != basename) {
        statusLabel_->setText(QStringLiteral(
            "Delete cancelled — filename did not match."));
        return;
    }

    // Close the DB first — releases the SQLite locks + WAL handles so
    // the files on disk are safe to unlink.
    const QStringList to_unlink = files_to_delete;
    closeDatabase();

    QStringList failed;
    for (const auto& p : to_unlink) {
        if (QFileInfo::exists(p) && !QFile::remove(p)) {
            failed << p;
        }
    }
    if (!failed.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Delete Database"),
            QStringLiteral("Some files could not be deleted:\n%1")
                .arg(failed.join('\n')));
    } else {
        statusLabel_->setText(QStringLiteral(
            "Deleted %1 (and %2 sidecar file(s)).")
            .arg(main_fi.absoluteFilePath())
            .arg(to_unlink.size() - 1));
    }

    // Clear last-used DB path so next launch doesn't try to reopen it.
    QSettings s(kSettingsOrg, kSettingsApp);
    s.remove(kKeyLastDb);
}

// --- UI construction -------------------------------------------------------

void MainWindow::buildUi() {
    model_ = new DiskTableModel(*db_, this);
    proxy_ = new FilterProxy(this);
    proxy_->setSourceModel(model_);

    table_ = new QTableView(this);
    table_->setModel(proxy_);
    table_->setSortingEnabled(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->verticalHeader()->setVisible(false);

    // All columns resize independently — no Stretch mode (which would make
    // one column absorb/release width whenever any other column was
    // resized, causing the "opposite side moves" behavior). Sensible
    // default widths; user can resize any of them freely.
    auto* hh = table_->horizontalHeader();
    hh->setSectionResizeMode(QHeaderView::Interactive);
    hh->setStretchLastSection(false);
    hh->resizeSection(DiskTableModel::Id,          60);
    hh->resizeSection(DiskTableModel::Filename,    240);
    hh->resizeSection(DiskTableModel::Title,       240);
    hh->resizeSection(DiskTableModel::Publisher,   160);
    hh->resizeSection(DiskTableModel::Year,        60);
    hh->resizeSection(DiskTableModel::Format,      70);
    hh->resizeSection(DiskTableModel::VolumeLabel, 120);
    hh->resizeSection(DiskTableModel::Tags,        160);
    hh->resizeSection(DiskTableModel::Contents,    320);
    hh->resizeSection(DiskTableModel::Identified,  90);
    hh->resizeSection(DiskTableModel::Notes,       220);

    table_->sortByColumn(DiskTableModel::Title, Qt::AscendingOrder);
    setCentralWidget(table_);

    connect(table_, &QWidget::customContextMenuRequested,
            this,   &MainWindow::onContextMenu);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection&, const QItemSelection&){ onSelectionChanged(); });
}

void MainWindow::buildToolbar() {
    auto* tb = addToolBar(QStringLiteral("Main"));
    tb->setObjectName(QStringLiteral("MainToolbar"));
    tb->setMovable(false);

    actQuickScan_ = tb->addAction(QStringLiteral("Quick Scan…"));
    actQuickScan_->setToolTip(QStringLiteral(
        "Fast scan: image hash + file listing + catalog match. "
        "Skips per-file hashing, cracker-group detection, byte-level "
        "game detection, and scrolltext extraction."));
    connect(actQuickScan_, &QAction::triggered, this, &MainWindow::onQuickScanFolder);

    actDeepScan_ = tb->addAction(QStringLiteral("Deep Scan…"));
    actDeepScan_->setToolTip(QStringLiteral(
        "Full scan: everything Quick does + per-file SHA1 hashes, "
        "cracker-group signatures, byte-level game detection, "
        "and scrolltext capture."));
    connect(actDeepScan_, &QAction::triggered, this, &MainWindow::onDeepScanFolder);

    actRescan_ = tb->addAction(QStringLiteral("Rescan"));
    connect(actRescan_, &QAction::triggered, this, &MainWindow::onRescan);

    tb->addSeparator();

    actLaunch_ = tb->addAction(QStringLiteral("Launch in Hatari"));
    connect(actLaunch_, &QAction::triggered, this, &MainWindow::onLaunchSelected);

    actShowInFiles_ = tb->addAction(QStringLiteral("Show in Files"));
    connect(actShowInFiles_, &QAction::triggered, this, &MainWindow::onShowInFilesSelected);

    auto* spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);

    search_ = new QLineEdit(tb);
    search_->setPlaceholderText(QStringLiteral("Search title / publisher / label / tags"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(320);
    connect(search_, &QLineEdit::textChanged, this, &MainWindow::onFilterChanged);
    tb->addWidget(search_);

    // Ctrl+F focuses the search box.
    auto* actFocusSearch = new QAction(this);
    actFocusSearch->setShortcut(QKeySequence::Find);
    addAction(actFocusSearch);
    connect(actFocusSearch, &QAction::triggered, this, [this]{
        search_->setFocus();
        search_->selectAll();
    });
}

void MainWindow::buildMenus() {
    auto* mFile = menuBar()->addMenu(QStringLiteral("&File"));

    actNewDb_ = mFile->addAction(QStringLiteral("New Database…"));
    actNewDb_->setShortcut(QKeySequence::New);        // Ctrl+N
    connect(actNewDb_, &QAction::triggered, this, &MainWindow::onNewDatabase);

    auto* actOpenDb = mFile->addAction(QStringLiteral("Open Database…"));
    actOpenDb->setShortcut(QKeySequence::Open);       // Ctrl+O
    connect(actOpenDb, &QAction::triggered, this, &MainWindow::onOpenDatabase);

    actSaveDbAs_ = mFile->addAction(QStringLiteral("Save Database As…"));
    actSaveDbAs_->setShortcut(QKeySequence::SaveAs);  // Ctrl+Shift+S
    connect(actSaveDbAs_, &QAction::triggered, this, &MainWindow::onSaveDatabaseAs);

    mFile->addSeparator();

    actCloseDb_ = mFile->addAction(QStringLiteral("Close Database"));
    actCloseDb_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    connect(actCloseDb_, &QAction::triggered, this, &MainWindow::onCloseDatabase);

    actDeleteDb_ = mFile->addAction(QStringLiteral("Delete Database…"));
    connect(actDeleteDb_, &QAction::triggered, this, &MainWindow::onDeleteDatabase);

    mFile->addSeparator();
    auto* actQuit = mFile->addAction(QStringLiteral("&Quit"));
    actQuit->setShortcut(QKeySequence::Quit);     // Ctrl+Q
    connect(actQuit, &QAction::triggered, this, &QWidget::close);

    auto* mEdit = menuBar()->addMenu(QStringLiteral("&Edit"));
    auto* actEditNote = mEdit->addAction(QStringLiteral("Edit Note for Selected Disk…"));
    actEditNote->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(actEditNote, &QAction::triggered, this, &MainWindow::onEditNote);

    auto* mScan = menuBar()->addMenu(QStringLiteral("&Scan"));
    actQuickScan_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    actDeepScan_ ->setShortcut(QKeySequence(Qt::Key_F6));
    actRescan_   ->setShortcut(QKeySequence(Qt::Key_F5));
    mScan->addAction(actQuickScan_);
    mScan->addAction(actDeepScan_);
    mScan->addAction(actRescan_);

    auto* mView = menuBar()->addMenu(QStringLiteral("&View"));
    actToggleIdentified_ = mView->addAction(QStringLiteral("Show Identified column"));
    actToggleIdentified_->setCheckable(true);
    actToggleIdentified_->setChecked(true);
    connect(actToggleIdentified_, &QAction::toggled,
            this, &MainWindow::onToggleIdentifiedColumn);

    actToggleContents_ = mView->addAction(QStringLiteral("Show 'Games on this disk' column"));
    actToggleContents_->setCheckable(true);
    actToggleContents_->setChecked(true);
    connect(actToggleContents_, &QAction::toggled,
            this, &MainWindow::onToggleContentsColumn);

    auto* mHelp = menuBar()->addMenu(QStringLiteral("&Help"));
    auto* actInstructions = mHelp->addAction(QStringLiteral("Instructions"));
    actInstructions->setShortcut(QKeySequence::HelpContents);   // F1
    connect(actInstructions, &QAction::triggered,
            this, &MainWindow::onShowInstructions);
    mHelp->addSeparator();
    auto* actAbout = mHelp->addAction(QStringLiteral("About ManifeST"));
    connect(actAbout, &QAction::triggered, this, [this]{
        QMessageBox::about(this, QStringLiteral("About ManifeST"),
            QStringLiteral(
              "<h3>%1 — version %2</h3>"
              "<p>Atari ST disk image cataloguer.</p>"
              "<p>%3</p>"
              "<p>Author: %4<br>License: %5</p>")
            .arg(QString::fromLatin1(kProjectName),
                 QString::fromLatin1(kVersion),
                 QString::fromLatin1(kEngineNote),
                 QString::fromLatin1(kAuthor),
                 QString::fromLatin1(kLicense)));
    });
}

void MainWindow::buildSidebarDock() {
    auto* dock = new QDockWidget(QStringLiteral("Filters"), this);
    dock->setObjectName(QStringLiteral("SidebarDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    sidebar_ = new QTreeWidget(dock);
    sidebar_->setHeaderHidden(true);
    sidebar_->setRootIsDecorated(true);
    sidebar_->setMinimumWidth(220);
    connect(sidebar_, &QTreeWidget::itemClicked,
            this,     &MainWindow::onSidebarItemClicked);
    dock->setWidget(sidebar_);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
}

namespace {

// Item kinds stored in Qt::UserRole.
constexpr int kKindAll   = 0;
constexpr int kKindDupes = 1;
constexpr int kKindTag   = 2;
constexpr int kKindSet   = 3;

QTreeWidgetItem* makeItem(const QString& text, int kind, const QVariant& payload = {}) {
    auto* it = new QTreeWidgetItem(QStringList{text});
    it->setData(0, Qt::UserRole,     kind);
    it->setData(0, Qt::UserRole + 1, payload);
    return it;
}

} // namespace

void MainWindow::refreshSidebar() {
    if (!sidebar_) return;
    sidebar_->clear();

    // No DB open → nothing to populate. Leave the sidebar empty.
    if (!db_) return;

    auto* all = makeItem(QStringLiteral("All Disks (%1)").arg(model_->rowCount()), kKindAll);
    sidebar_->addTopLevelItem(all);

    const auto dupes = db_->listDuplicates();
    std::size_t dupe_count = 0;
    for (const auto& g : dupes) dupe_count += g.disks.size();
    auto* dupesItem = makeItem(QStringLiteral("Duplicates (%1)").arg(dupe_count), kKindDupes);
    sidebar_->addTopLevelItem(dupesItem);

    auto* tagsRoot = makeItem(QStringLiteral("Tags"), kKindAll);
    sidebar_->addTopLevelItem(tagsRoot);
    for (const auto& tc : db_->listAllTags()) {
        auto* it = makeItem(
            QStringLiteral("%1 (%2)").arg(QString::fromStdString(tc.tag)).arg(tc.count),
            kKindTag, QString::fromStdString(tc.tag));
        tagsRoot->addChild(it);
    }
    tagsRoot->setExpanded(true);

    auto* setsRoot = makeItem(QStringLiteral("Multi-disk Sets"), kKindAll);
    sidebar_->addTopLevelItem(setsRoot);
    const auto sets = db_->listDiskSets();
    for (const auto& set : sets) {
        QList<QVariant> ids;
        for (const auto& [disk_id, _num] : set.members) {
            ids.append(QVariant(qlonglong(disk_id)));
        }
        auto* it = makeItem(
            QStringLiteral("%1 [%2 disks]").arg(QString::fromStdString(set.title))
                                            .arg(set.members.size()),
            kKindSet, ids);
        setsRoot->addChild(it);
    }
    setsRoot->setExpanded(true);

    // Default selection — All Disks.
    sidebar_->setCurrentItem(all);
}

void MainWindow::onSidebarItemClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) return;
    const int kind = item->data(0, Qt::UserRole).toInt();
    const QVariant payload = item->data(0, Qt::UserRole + 1);

    switch (kind) {
        case kKindAll:
            proxy_->clearIdFilter();
            break;
        case kKindDupes: {
            QSet<qint64> ids;
            for (const auto& g : db_->listDuplicates()) {
                for (const auto& d : g.disks) ids.insert(d.id);
            }
            proxy_->setIdFilter(ids);
            break;
        }
        case kKindTag: {
            QSet<qint64> ids;
            for (auto id : db_->idsWithTag(payload.toString().toStdString())) {
                ids.insert(id);
            }
            proxy_->setIdFilter(ids);
            break;
        }
        case kKindSet: {
            QSet<qint64> ids;
            for (const auto& v : payload.toList()) ids.insert(v.toLongLong());
            proxy_->setIdFilter(ids);
            break;
        }
    }

    statusLabel_->setText(
        QStringLiteral("%1 / %2 disks").arg(proxy_->rowCount()).arg(model_->rowCount()));
}

void MainWindow::buildDetailDock() {
    auto* dock = new QDockWidget(QStringLiteral("Details"), this);
    dock->setObjectName(QStringLiteral("DetailDock"));
    dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);

    detail_ = new QTextBrowser(dock);
    detail_->setOpenExternalLinks(true);   // box-art / screenshot URLs → browser
    dock->setWidget(detail_);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
}

void MainWindow::buildStatusBar() {
    statusLabel_ = new QLabel(this);
    statusBar()->addWidget(statusLabel_, 1);

    if (model_->rowCount() == 0) {
        statusLabel_->setText(QStringLiteral(
            "Empty catalog — use Scan ▸ Scan Folder… (Ctrl+S) to get started."));
    } else {
        statusLabel_->setText(QStringLiteral("Ready — %1 disks").arg(model_->rowCount()));
    }

    // Scan-progress widgets (hidden until a scan starts).
    progressLabel_ = new QLabel(this);
    progressLabel_->setVisible(false);
    statusBar()->addPermanentWidget(progressLabel_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setTextVisible(false);
    progressBar_->setMaximumWidth(200);
    progressBar_->setVisible(false);
    statusBar()->addPermanentWidget(progressBar_);

    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancelButton_->setVisible(false);
    connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::onCancelScan);
    statusBar()->addPermanentWidget(cancelButton_);
}

void MainWindow::buildScannerThread() {
    scanThread_ = new QThread(this);
    scanner_    = new Scanner(*db_, *identifier_);   // no parent — moved to thread
    if (menu_catalog_ && menu_catalog_->loaded()) {
        scanner_->setMenuCatalog(menu_catalog_.get());
    }
    if (ss_cache_ && ss_cache_->loaded()) {
        scanner_->setScreenScraperCache(ss_cache_.get());
    }
    scanner_->moveToThread(scanThread_);

    // We drive scans by emitting startScanRequested — the scanner performs
    // its work on the worker thread and emits signals back.
    connect(this, &MainWindow::startScanRequested, scanner_,
            [this](const QString& root, bool incremental, bool quick) {
                scanner_->scan(std::filesystem::path(root.toStdString()),
                               incremental, quick);
            });

    connect(scanner_, &Scanner::progress,   this, &MainWindow::onScanProgress);
    connect(scanner_, &Scanner::imageDone,  this, &MainWindow::onScanImageDone);
    connect(scanner_, &Scanner::finished,   this, &MainWindow::onScanFinished);

    // Clean up when the app shuts down — the thread waits for the current
    // image to finish; requestCancel makes the loop exit at next iteration.
    connect(scanThread_, &QThread::finished, scanner_, &QObject::deleteLater);

    scanThread_->start();
}

// --- Filtering / selection / detail pane ----------------------------------

void MainWindow::onFilterChanged(const QString& text) {
    proxy_->setSearchText(text);
    statusLabel_->setText(
        QStringLiteral("%1 / %2 disks").arg(proxy_->rowCount()).arg(model_->rowCount()));
}

int MainWindow::selectedModelRow() const {
    const auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return -1;
    return proxy_->mapToSource(sel.first()).row();
}

void MainWindow::onSelectionChanged() {
    const int model_row = selectedModelRow();
    const bool has_sel  = model_row >= 0 && db_;
    actLaunch_->setEnabled(has_sel);
    actShowInFiles_->setEnabled(has_sel);

    if (!db_) {
        detail_->setHtml(QStringLiteral("<i>No database open. "
            "Use <b>File ▸ Open Database…</b> (Ctrl+O) to get started.</i>"));
        return;
    }
    if (!has_sel) {
        detail_->setHtml(QStringLiteral("<i>No selection</i>"));
        return;
    }

    const int64_t id = model_->idAtRow(model_row);
    auto opt = db_->queryById(id);
    if (!opt) {
        detail_->setHtml(QStringLiteral("<i>record %1 not found</i>").arg(id));
        return;
    }
    const auto& r = *opt;

    QString html;
    html += QStringLiteral("<h3>%1</h3>").arg(
        r.identified_title ? htmlEscape(*r.identified_title)
                           : QStringLiteral("<i>(unidentified)</i>"));

    // User note — prominent, pre-formatted (preserves line breaks).
    if (r.notes && !r.notes->empty()) {
        html += QStringLiteral(
            "<div style='background:#fffbd6; color:#333; padding:8px; "
            "border-left:4px solid #e0b84c; margin:6px 0; "
            "white-space:pre-wrap; font-family:sans-serif;'>"
            "<b>Note:</b><br>%1</div>")
            .arg(htmlEscape(*r.notes));
    }
    html += QStringLiteral("<table cellpadding='2'>");
    auto row = [&](const QString& k, const QString& v){
        html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(k, v);
    };
    row(QStringLiteral("Path"),     htmlEscape(r.path));
    row(QStringLiteral("SHA1"),     htmlEscape(r.image_hash));
    row(QStringLiteral("Format"),   htmlEscape(r.format));
    row(QStringLiteral("Label"),    htmlEscape(r.volume_label));
    row(QStringLiteral("OEM"),      htmlEscape(r.oem_name));
    row(QStringLiteral("Geometry"), QStringLiteral("%1 sides / %2 tracks / %3 spt / %4 bps")
            .arg(r.sides).arg(r.tracks).arg(r.sectors_per_track).arg(r.bytes_per_sector));
    if (r.genre)     row(QStringLiteral("Genre"),     htmlEscape(*r.genre));
    if (r.developer) row(QStringLiteral("Developer"), htmlEscape(*r.developer));
    if (r.boxart_url) {
        row(QStringLiteral("Box art"),
            QStringLiteral("<a href='%1'>%1</a>").arg(htmlEscape(*r.boxart_url)));
    }
    if (r.screenshot_url) {
        row(QStringLiteral("Screenshot"),
            QStringLiteral("<a href='%1'>%1</a>").arg(htmlEscape(*r.screenshot_url)));
    }
    if (!r.tags.empty()) {
        QStringList t;
        for (const auto& s : r.tags) t << htmlEscape(s);
        row(QStringLiteral("Tags"), t.join(", "));
    }
    html += QStringLiteral("</table>");

    if (r.synopsis) {
        html += QStringLiteral("<h4>Synopsis</h4><p>%1</p>")
                    .arg(htmlEscape(*r.synopsis));
    }

    if (!r.menu_games.empty()) {
        html += QStringLiteral("<h4>Games on this menu — from catalog (%1)</h4><ol>")
                    .arg(r.menu_games.size());
        for (const auto& g : r.menu_games) {
            html += QStringLiteral("<li>%1</li>").arg(htmlEscape(g.name));
        }
        html += QStringLiteral("</ol>");
    }

    if (!r.detected_games.empty()) {
        html += QStringLiteral("<h4>Games detected on disk — from byte scan (%1)</h4>"
                               "<table cellpadding='2'>")
                    .arg(r.detected_games.size());
        for (const auto& g : r.detected_games) {
            html += QStringLiteral("<tr><td>%1</td><td><tt>%2</tt></td></tr>")
                .arg(htmlEscape(g.name), htmlEscape(g.evidence));
        }
        html += QStringLiteral("</table>");
    }

    if (!r.text_fragments.empty()) {
        html += QStringLiteral("<h4>Scrolltext / boot strings (%1)</h4>"
                               "<div style='font-family: monospace; "
                               "background: #111; color: #c8c8c8; padding: 6px; "
                               "white-space: pre-wrap; word-break: break-word;'>")
                    .arg(r.text_fragments.size());
        for (const auto& f : r.text_fragments) {
            const QString tag = f.source == "boot"
                ? QStringLiteral("<span style='color:#f8c870;'>[boot]</span>")
                : QStringLiteral("<span style='color:#70c0f8;'>[deep]</span>");
            html += tag + QStringLiteral(" ") + htmlEscape(f.text)
                  + QStringLiteral("<br>");
        }
        html += QStringLiteral("</div>");
    }

    html += QStringLiteral("<h4>Files (%1)</h4><table cellpadding='2'>").arg(r.files.size());
    for (const auto& f : r.files) {
        const QString prefix = f.is_launcher ? QStringLiteral("&#9733; ") : QString{};
        html += QStringLiteral("<tr><td>%1%2</td><td align='right'>%3</td><td><tt>%4</tt></td></tr>")
                    .arg(prefix, htmlEscape(f.filename))
                    .arg(f.size_bytes)
                    .arg(htmlEscape(f.file_hash));
    }
    html += QStringLiteral("</table>");

    detail_->setHtml(html);
}

// --- Context menu / row actions -------------------------------------------

void MainWindow::onContextMenu(const QPoint& pos) {
    const QModelIndex vidx = table_->indexAt(pos);
    if (!vidx.isValid()) return;
    table_->selectRow(vidx.row());

    QMenu menu(this);
    menu.addAction(QStringLiteral("Launch in Hatari"), this, &MainWindow::onLaunchSelected);
    menu.addAction(QStringLiteral("Show in Files"),    this, &MainWindow::onShowInFilesSelected);
    menu.addAction(QStringLiteral("Copy Path"),        this, &MainWindow::onCopyPathSelected);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Edit Note…"),       this, &MainWindow::onEditNote);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Remove from Catalog…"),
                   this, &MainWindow::onRemoveSelected);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

namespace {

// Resolve INSTRUCTIONS.md across three realistic install layouts:
//   1. AppImage / system install: <appdir>/../share/manifest/INSTRUCTIONS.md
//   2. Build directory next to source: <cwd>/INSTRUCTIONS.md
//   3. CWD above build (dev builds often launched from build/): ../INSTRUCTIONS.md
QString locateInstructionsFile() {
    const QString app = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        app + "/../share/manifest/INSTRUCTIONS.md",
        QDir::currentPath() + "/INSTRUCTIONS.md",
        QDir::currentPath() + "/../INSTRUCTIONS.md",
        app + "/INSTRUCTIONS.md",
    };
    for (const auto& p : candidates) {
        QFileInfo fi(p);
        if (fi.exists() && fi.isFile()) return fi.canonicalFilePath();
    }
    return {};
}

} // namespace

void MainWindow::onShowInstructions() {
    const QString path = locateInstructionsFile();
    if (path.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Instructions"),
            QStringLiteral("Could not find INSTRUCTIONS.md. If this is a "
                           "dev build, launch ManifeST from the repo root."));
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Instructions"),
            QStringLiteral("Could not open %1").arg(path));
        return;
    }
    const QString md = QString::fromUtf8(f.readAll());
    f.close();

    // Non-modal dialog so the user can keep it open while using the
    // main window. deleteOnClose makes the shortcut reusable.
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QStringLiteral("ManifeST — Instructions"));
    dlg->resize(900, 700);

    auto* browser = new QTextBrowser(dlg);
    browser->setOpenExternalLinks(true);
    // GitHub dialect gives us tables, strikethrough, and task lists —
    // matching how INSTRUCTIONS.md is actually written. The dialect
    // option is on QTextDocument, not QTextBrowser, so apply via the
    // browser's underlying document.
    browser->document()->setMarkdown(md, QTextDocument::MarkdownDialectGitHub);

    // Little status line at the top so the user can see exactly which
    // file was loaded (helps diagnose "am I reading the AppImage copy
    // or the repo copy?").
    auto* pathLabel = new QLabel(QStringLiteral("<small>Source: <tt>%1</tt></small>")
                                     .arg(path.toHtmlEscaped()), dlg);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* openExt = new QPushButton(QStringLiteral("Open in External Viewer"), dlg);
    openExt->setToolTip(QStringLiteral(
        "Opens INSTRUCTIONS.md with your system's default markdown viewer — "
        "useful if the in-app renderer misformats the content."));
    connect(openExt, &QPushButton::clicked, dlg, [path]{
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    auto* close = new QPushButton(QStringLiteral("Close"), dlg);
    connect(close, &QPushButton::clicked, dlg, &QDialog::close);

    auto* layout = new QVBoxLayout(dlg);
    layout->addWidget(pathLabel);
    layout->addWidget(browser);
    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(openExt);
    btnRow->addStretch();
    btnRow->addWidget(close);
    layout->addLayout(btnRow);

    dlg->show();
}

void MainWindow::onSaveDatabaseAs() {
    if (!db_) {
        QMessageBox::information(this, QStringLiteral("Save Database As"),
            QStringLiteral("No database is currently open."));
        return;
    }
    const QString start = QFileInfo(dbPath_).absolutePath();

    QFileDialog dlg(this, QStringLiteral("Save Database As"), start);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setNameFilter(QStringLiteral("ManifeST catalog (*.db);;All files (*)"));
    dlg.setDefaultSuffix(QStringLiteral("db"));
    if (dlg.exec() != QDialog::Accepted) return;
    const QString chosen = dlg.selectedFiles().value(0);
    if (chosen.isEmpty()) return;

    // Same path as current? Nothing to do — just ensure an on-disk commit.
    if (QFileInfo(chosen).canonicalFilePath() ==
        QFileInfo(dbPath_).canonicalFilePath()) {
        QMessageBox::information(this, QStringLiteral("Save Database As"),
            QStringLiteral("Target is the currently open database; nothing to do."));
        return;
    }

    // Flush the WAL so the main .db file is self-contained before copy.
    try { db_->checkpoint(); }
    catch (const std::exception& e) {
        QMessageBox::warning(this, QStringLiteral("Save Database As"),
            QStringLiteral("Checkpoint failed: %1").arg(e.what()));
        return;
    }

    std::error_code ec;
    std::filesystem::copy_file(
        dbPath_.toStdString(), chosen.toStdString(),
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        QMessageBox::warning(this, QStringLiteral("Save Database As"),
            QStringLiteral("Copy failed: %1").arg(QString::fromStdString(ec.message())));
        return;
    }
    // Switch to the new file — worker thread restart + model reload
    // are handled by openDatabase().
    openDatabase(chosen);
    statusLabel_->setText(QStringLiteral("Saved catalog to %1").arg(chosen));
}

void MainWindow::onNewDatabase() {
    const QString start = dbPath_.isEmpty()
        ? QDir::homePath()
        : QFileInfo(dbPath_).absolutePath();

    // Construct a QFileDialog so we can set a default suffix — ensures
    // `.db` gets appended when the user types "my-collection" without an
    // extension. The static getSaveFileName overload doesn't expose this.
    QFileDialog dlg(this, QStringLiteral("New Database"), start);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setNameFilter(QStringLiteral("ManifeST catalog (*.db);;All files (*)"));
    dlg.setDefaultSuffix(QStringLiteral("db"));
    // Block Qt's own overwrite confirmation — we do our own explicit
    // "target already exists" check below.
    dlg.setOption(QFileDialog::DontConfirmOverwrite);

    if (dlg.exec() != QDialog::Accepted) return;
    const QString chosen = dlg.selectedFiles().value(0);
    if (chosen.isEmpty()) return;

    if (QFileInfo::exists(chosen)) {
        QMessageBox::warning(this, QStringLiteral("New Database"),
            QStringLiteral(
                "<b>A file already exists at this path.</b><br><br>"
                "<tt>%1</tt><br><br>"
                "To avoid accidentally wiping an existing catalogue, ManifeST "
                "won't overwrite it here. If you want to open the existing "
                "one use <b>File ▸ Open Database…</b>; if you want to replace "
                "it, delete it first via <b>File ▸ Delete Database…</b> and "
                "then create a new one.")
                .arg(chosen.toHtmlEscaped()));
        return;
    }

    // openDatabase() creates the SQLite file via the Database ctor
    // (SQLite opens-or-creates by default) and applies all schema
    // migrations. The resulting DB starts empty at schema v7.
    openDatabase(chosen);
    statusLabel_->setText(QStringLiteral("Created new catalogue: %1").arg(chosen));
}

void MainWindow::onOpenDatabase() {
    const QString start = dbPath_.isEmpty()
        ? QDir::homePath()
        : QFileInfo(dbPath_).absolutePath();
    // Open-existing picker (button reads "Open"). To create a fresh DB
    // use File ▸ New Database… instead.
    const QString chosen = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Database"), start,
        QStringLiteral("ManifeST catalog (*.db);;All files (*)"));
    if (chosen.isEmpty()) return;
    openDatabase(chosen);
}

void MainWindow::onLaunchSelected() {
    const int row = selectedModelRow();
    if (row < 0) return;
    const auto path = model_->pathAtRow(row);
    if (path.empty()) return;
    const auto r = HatariLauncher::launch(path);
    if (!r.launched) {
        QMessageBox::warning(this, QStringLiteral("Launch failed"),
            QString::fromStdString(r.error));
    }
}

void MainWindow::onShowInFilesSelected() {
    const int row = selectedModelRow();
    if (row < 0) return;
    const QString qpath = QString::fromStdString(model_->pathAtRow(row));
    if (qpath.isEmpty()) return;
    const QString dir = QFileInfo(qpath).absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::onCopyPathSelected() {
    const int row = selectedModelRow();
    if (row < 0) return;
    QApplication::clipboard()->setText(QString::fromStdString(model_->pathAtRow(row)));
}

void MainWindow::onEditNote() {
    const int row = selectedModelRow();
    if (row < 0) {
        statusLabel_->setText(QStringLiteral("Select a disk first to edit its note."));
        return;
    }
    const int64_t id = model_->idAtRow(row);

    auto rec = db_->queryById(id);
    if (!rec) return;

    const QString label = rec->identified_title
        ? QString::fromStdString(*rec->identified_title)
        : QString::fromStdString(rec->filename);
    const QString current = rec->notes
        ? QString::fromStdString(*rec->notes) : QString{};

    bool ok = false;
    const QString edited = QInputDialog::getMultiLineText(
        this,
        QStringLiteral("Edit Note"),
        QStringLiteral("Note for: %1").arg(label),
        current,
        &ok);
    if (!ok) return;

    try {
        db_->setNotes(id, edited.trimmed().toStdString());
    } catch (const std::exception& e) {
        QMessageBox::warning(this, QStringLiteral("Edit Note"),
            QString::fromStdString(e.what()));
        return;
    }

    // Refresh both the main-table Notes cell (via upsertRow) and the
    // detail pane so the change is visible immediately without a reload.
    auto fresh = db_->queryById(id);
    if (fresh) model_->upsertRow(*fresh);
    onSelectionChanged();
    statusLabel_->setText(edited.trimmed().isEmpty()
        ? QStringLiteral("Note cleared.")
        : QStringLiteral("Note saved."));
}

void MainWindow::onRemoveSelected() {
    const int row = selectedModelRow();
    if (row < 0) return;
    const int64_t id = model_->idAtRow(row);
    const QString path = QString::fromStdString(model_->pathAtRow(row));

    const auto answer = QMessageBox::question(this,
        QStringLiteral("Remove from Catalog"),
        QStringLiteral("Remove this disk from the catalog?\n\n%1\n\n"
                       "The image file on disk is NOT deleted.").arg(path),
        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    db_->removeDisk(id);
    model_->reload();
    refreshSidebar();
    statusLabel_->setText(QStringLiteral("%1 disks").arg(model_->rowCount()));
}

void MainWindow::onToggleIdentifiedColumn(bool visible) {
    table_->setColumnHidden(DiskTableModel::Identified, !visible);
}

void MainWindow::onToggleContentsColumn(bool visible) {
    table_->setColumnHidden(DiskTableModel::Contents, !visible);
}

// --- Scanning --------------------------------------------------------------

void MainWindow::setScanUiRunning(bool running) {
    actQuickScan_->setEnabled(!running);
    actDeepScan_ ->setEnabled(!running);
    actRescan_   ->setEnabled(!running && !lastScanRoot_.isEmpty());
    progressLabel_->setVisible(running);
    progressBar_->setVisible(running);
    cancelButton_->setVisible(running);
    if (running) {
        progressBar_->setRange(0, 0);      // indeterminate until first progress()
        progressLabel_->setText(QStringLiteral("Scanning…"));
    }
}

void MainWindow::onQuickScanFolder() {
    const QString start =
        lastScanRoot_.isEmpty() ? QDir::homePath() : lastScanRoot_;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Quick Scan Folder"), start);
    if (dir.isEmpty()) return;
    startScan(dir, /*incremental=*/true, /*quick=*/true);
}

void MainWindow::onDeepScanFolder() {
    const QString start =
        lastScanRoot_.isEmpty() ? QDir::homePath() : lastScanRoot_;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Deep Scan Folder"), start);
    if (dir.isEmpty()) return;
    startScan(dir, /*incremental=*/true, /*quick=*/false);
}

void MainWindow::onRescan() {
    if (lastScanRoot_.isEmpty()) return;
    // Re-run in whichever mode the user last chose.
    startScan(lastScanRoot_, /*incremental=*/false, lastScanWasQuick_);
}

void MainWindow::startScan(const QString& root, bool incremental, bool quick) {
    if (!db_ || !scanner_) {
        QMessageBox::information(this, QStringLiteral("Scan"),
            QStringLiteral("Open a database first (File ▸ Open Database…)."));
        return;
    }
    lastScanRoot_     = root;
    lastScanWasQuick_ = quick;
    setScanUiRunning(true);
    progressLabel_->setText(
        quick ? QStringLiteral("Quick scan starting…")
              : QStringLiteral("Deep scan starting…"));
    emit startScanRequested(root, incremental, quick);
}

void MainWindow::onCancelScan() {
    scanner_->requestCancel();
    cancelButton_->setEnabled(false);
    progressLabel_->setText(QStringLiteral("Cancelling…"));
}

void MainWindow::onScanProgress(int index, int total, const QString& currentPath) {
    if (progressBar_->maximum() != total) {
        progressBar_->setRange(0, total);
    }
    progressBar_->setValue(index);

    const QString name = QFileInfo(currentPath).fileName();
    progressLabel_->setText(QStringLiteral("[%1/%2] %3").arg(index).arg(total).arg(name));
}

void MainWindow::onScanImageDone(const manifest::DiskRecord& rec) {
    model_->upsertRow(rec);
}

void MainWindow::onScanFinished(const manifest::Scanner::Summary& s) {
    setScanUiRunning(false);
    cancelButton_->setEnabled(true);

    // Post-scan grouping pass.
    MultiDiskDetector::detectAndPersist(*db_);
    refreshSidebar();

    statusLabel_->setText(QStringLiteral(
        "Scan done — %1 scanned, %2 added, %3 updated, %4 skipped, %5 failed  (%6 disks total)")
        .arg(s.scanned).arg(s.added).arg(s.updated).arg(s.skipped).arg(s.failed)
        .arg(model_->rowCount()));
}

// --- Settings / lifecycle --------------------------------------------------

void MainWindow::restoreSettings() {
    QSettings s(kSettingsOrg, kSettingsApp);
    if (s.contains(kKeyGeometry)) restoreGeometry(s.value(kKeyGeometry).toByteArray());
    if (s.contains(kKeyState))    restoreState   (s.value(kKeyState).toByteArray());
    const bool show_ident = s.value(kKeyShowIdent, true).toBool();
    actToggleIdentified_->setChecked(show_ident);
    table_->setColumnHidden(DiskTableModel::Identified, !show_ident);

    const bool show_contents = s.value(kKeyShowContents, true).toBool();
    actToggleContents_->setChecked(show_contents);
    table_->setColumnHidden(DiskTableModel::Contents, !show_contents);

    lastScanRoot_ = s.value(kKeyLastScan).toString();
    actRescan_->setEnabled(!lastScanRoot_.isEmpty());
}

void MainWindow::saveSettings() {
    QSettings s(kSettingsOrg, kSettingsApp);
    s.setValue(kKeyGeometry,  saveGeometry());
    s.setValue(kKeyState,     saveState());
    s.setValue(kKeyShowIdent,    actToggleIdentified_->isChecked());
    s.setValue(kKeyShowContents, actToggleContents_->isChecked());
    s.setValue(kKeyLastScan,  lastScanRoot_);
    s.setValue(kKeyLastDb,    dbPath_);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

} // namespace manifest::gui
