#include "manifest/gui/DiskTableModel.hpp"

#include "manifest/Database.hpp"

#include <QString>
#include <QStringList>

namespace manifest::gui {

namespace {

QString joinTags(const std::vector<std::string>& tags) {
    QStringList out;
    out.reserve(static_cast<int>(tags.size()));
    for (const auto& t : tags) out << QString::fromStdString(t);
    return out.join(", ");
}

QString optString(const std::optional<std::string>& v) {
    return v ? QString::fromStdString(*v) : QString{};
}

} // namespace

DiskTableModel::DiskTableModel(Database& db, QObject* parent)
    : QAbstractTableModel(parent), db_(&db) {
    reload();
}

int DiskTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int DiskTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant DiskTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const auto row = static_cast<std::size_t>(index.row());
    if (row >= rows_.size()) return {};
    const auto& r = rows_[row];

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case Id:          return static_cast<qlonglong>(r.id);
            case Title:       return optString(r.identified_title);
            case Publisher:   return optString(r.publisher);
            case Year:        return r.year ? QVariant(*r.year) : QVariant{};
            case Format:      return QString::fromStdString(r.format);
            case VolumeLabel: return QString::fromStdString(r.volume_label);
            case Tags:        return joinTags(r.tags);
            case Identified:  return r.identified_title.has_value()
                                  ? QStringLiteral("\u2713")    // ✓
                                  : QStringLiteral("\u2717");   // ✕
        }
    }

    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
            case Id:
            case Year:
            case Identified:
                return QVariant(int(Qt::AlignCenter));
            default:
                return QVariant(int(Qt::AlignLeft | Qt::AlignVCenter));
        }
    }

    if (role == Qt::ToolTipRole) {
        if (index.column() == Identified) {
            return r.identified_title.has_value()
                ? QStringLiteral("Identified")
                : QStringLiteral("Not identified — no TOSEC name, volume label, or hash match");
        }
        return QString::fromStdString(r.path);
    }

    return {};
}

QVariant DiskTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;

    switch (section) {
        case Id:          return QStringLiteral("ID");
        case Title:       return QStringLiteral("Title");
        case Publisher:   return QStringLiteral("Publisher");
        case Year:        return QStringLiteral("Year");
        case Format:      return QStringLiteral("Format");
        case VolumeLabel: return QStringLiteral("Volume Label");
        case Tags:        return QStringLiteral("Tags");
        case Identified:  return QStringLiteral("Identified?");
    }
    return {};
}

void DiskTableModel::reload() {
    beginResetModel();
    rows_ = db_ ? db_->listAll() : std::vector<DiskRecord>{};
    endResetModel();
}

void DiskTableModel::setDatabase(Database& db) {
    db_ = &db;
}

void DiskTableModel::upsertRow(const DiskRecord& record) {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].id == record.id) {
            rows_[i] = record;
            const auto top    = index(static_cast<int>(i), 0);
            const auto bottom = index(static_cast<int>(i), ColumnCount - 1);
            emit dataChanged(top, bottom);
            return;
        }
    }
    const int row = static_cast<int>(rows_.size());
    beginInsertRows(QModelIndex(), row, row);
    rows_.push_back(record);
    endInsertRows();
}

int64_t DiskTableModel::idAtRow(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return 0;
    return rows_[static_cast<std::size_t>(row)].id;
}

std::string DiskTableModel::pathAtRow(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) return {};
    return rows_[static_cast<std::size_t>(row)].path;
}

} // namespace manifest::gui
