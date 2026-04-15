#include "manifest/gui/MainWindow.hpp"

#include "manifest/Database.hpp"
#include "manifest/HatariLauncher.hpp"
#include "manifest/Identifier.hpp"
#include "manifest/MenuDiskCatalog.hpp"
#include "manifest/MultiDiskDetector.hpp"
#include "manifest/Scanner.hpp"
#include "manifest/Version.hpp"
#include "manifest/gui/DiskTableModel.hpp"
#include "manifest/gui/FilterProxy.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
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
}

MainWindow::~MainWindow() {
    if (scanThread_) {
        scanner_->requestCancel();
        scanThread_->quit();
        scanThread_->wait();
    }
}

void MainWindow::openDatabase(const QString& path) {
    if (path == dbPath_) return;

    // Stop the worker; it holds the old Database reference.
    scanner_->requestCancel();
    scanThread_->quit();
    scanThread_->wait();

    std::unique_ptr<Database> new_db;
    try {
        new_db = std::make_unique<Database>(path.toStdString());
    } catch (const std::exception& e) {
        QMessageBox::warning(this, QStringLiteral("Open Database failed"),
            QString::fromStdString(e.what()));
        // Restart scanner on the OLD db before returning.
        buildScannerThread();
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
    statusLabel_->setText(QStringLiteral("Opened %1 — %2 disks")
        .arg(dbPath_).arg(model_->rowCount()));
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
    hh->resizeSection(DiskTableModel::Title,       260);
    hh->resizeSection(DiskTableModel::Publisher,   160);
    hh->resizeSection(DiskTableModel::Year,        60);
    hh->resizeSection(DiskTableModel::Format,      70);
    hh->resizeSection(DiskTableModel::VolumeLabel, 120);
    hh->resizeSection(DiskTableModel::Tags,        160);
    hh->resizeSection(DiskTableModel::Contents,    320);
    hh->resizeSection(DiskTableModel::Identified,  90);

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

    actScanFolder_ = tb->addAction(QStringLiteral("Scan Folder…"));
    connect(actScanFolder_, &QAction::triggered, this, &MainWindow::onScanFolder);

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
    auto* actOpenDb = mFile->addAction(QStringLiteral("Open Database…"));
    actOpenDb->setShortcut(QKeySequence::Open);   // Ctrl+O
    connect(actOpenDb, &QAction::triggered, this, &MainWindow::onOpenDatabase);
    mFile->addSeparator();
    auto* actQuit = mFile->addAction(QStringLiteral("&Quit"));
    actQuit->setShortcut(QKeySequence::Quit);     // Ctrl+Q
    connect(actQuit, &QAction::triggered, this, &QWidget::close);

    auto* mScan = menuBar()->addMenu(QStringLiteral("&Scan"));
    actScanFolder_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    actRescan_->setShortcut(QKeySequence(Qt::Key_F5));
    mScan->addAction(actScanFolder_);
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
    detail_->setOpenExternalLinks(false);
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
    scanner_->moveToThread(scanThread_);

    // We drive scans by emitting startScanRequested — the scanner performs
    // its work on the worker thread and emits signals back.
    connect(this, &MainWindow::startScanRequested, scanner_,
            [this](const QString& root, bool incremental) {
                scanner_->scan(std::filesystem::path(root.toStdString()), incremental);
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
    const bool has_sel  = model_row >= 0;
    actLaunch_->setEnabled(has_sel);
    actShowInFiles_->setEnabled(has_sel);

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
    if (!r.tags.empty()) {
        QStringList t;
        for (const auto& s : r.tags) t << htmlEscape(s);
        row(QStringLiteral("Tags"), t.join(", "));
    }
    html += QStringLiteral("</table>");

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
    menu.addAction(QStringLiteral("Remove from Catalog…"),
                   this, &MainWindow::onRemoveSelected);
    menu.exec(table_->viewport()->mapToGlobal(pos));
}

void MainWindow::onOpenDatabase() {
    const QString start = QFileInfo(dbPath_).absolutePath();
    const QString chosen = QFileDialog::getSaveFileName(
        this, QStringLiteral("Open or Create Database"), start,
        QStringLiteral("ManifeST catalog (*.db);;All files (*)"),
        nullptr, QFileDialog::DontConfirmOverwrite);
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
    actScanFolder_->setEnabled(!running);
    actRescan_->setEnabled(!running && !lastScanRoot_.isEmpty());
    progressLabel_->setVisible(running);
    progressBar_->setVisible(running);
    cancelButton_->setVisible(running);
    if (running) {
        progressBar_->setRange(0, 0);      // indeterminate until first progress()
        progressLabel_->setText(QStringLiteral("Scanning…"));
    }
}

void MainWindow::onScanFolder() {
    const QString start =
        lastScanRoot_.isEmpty() ? QDir::homePath() : lastScanRoot_;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Scan Folder"), start);
    if (dir.isEmpty()) return;
    startScan(dir, /*incremental=*/true);
}

void MainWindow::onRescan() {
    if (lastScanRoot_.isEmpty()) return;
    startScan(lastScanRoot_, /*incremental=*/false);
}

void MainWindow::startScan(const QString& root, bool incremental) {
    lastScanRoot_ = root;
    setScanUiRunning(true);
    emit startScanRequested(root, incremental);   // queued → worker thread
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
