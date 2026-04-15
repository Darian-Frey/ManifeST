#include "manifest/gui/DiskTableModel.hpp"

#include "manifest/Database.hpp"

namespace manifest::gui {

DiskTableModel::DiskTableModel(Database& db, QObject* parent)
    : QAbstractTableModel(parent), db_(db) {}

int DiskTableModel::rowCount(const QModelIndex&) const {
    return static_cast<int>(rows_.size());
}

int DiskTableModel::columnCount(const QModelIndex&) const {
    return ColumnCount;
}

QVariant DiskTableModel::data(const QModelIndex&, int) const {
    return {};
}

QVariant DiskTableModel::headerData(int, Qt::Orientation, int) const {
    return {};
}

void DiskTableModel::reload() {
}

} // namespace manifest::gui
