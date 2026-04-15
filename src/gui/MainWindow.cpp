#include "manifest/gui/MainWindow.hpp"

namespace manifest::gui {

MainWindow::MainWindow(Database& db, QWidget* parent)
    : QMainWindow(parent), db_(db) {}

MainWindow::~MainWindow() = default;

} // namespace manifest::gui
