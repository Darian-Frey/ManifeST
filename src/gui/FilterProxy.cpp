#include "manifest/gui/FilterProxy.hpp"

#include "manifest/gui/DiskTableModel.hpp"

#include <QModelIndex>
#include <QVariant>

namespace manifest::gui {

FilterProxy::FilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void FilterProxy::setSearchText(const QString& text) {
    if (search_ == text) return;
    search_ = text;
    invalidateFilter();
}

void FilterProxy::clearIdFilter() {
    if (!use_ids_) return;
    use_ids_ = false;
    ids_.clear();
    invalidateFilter();
}

void FilterProxy::setIdFilter(const QSet<qint64>& ids) {
    use_ids_ = true;
    ids_     = ids;
    invalidateFilter();
}

bool FilterProxy::filterAcceptsRow(int source_row, const QModelIndex& parent) const {
    auto* src = sourceModel();
    if (!src) return false;

    if (use_ids_) {
        const QVariant id = src->data(src->index(source_row, DiskTableModel::Id, parent),
                                      Qt::DisplayRole);
        if (!ids_.contains(id.toLongLong())) return false;
    }

    if (search_.isEmpty()) return true;

    const int cols = src->columnCount(parent);
    for (int c = 0; c < cols; ++c) {
        const QVariant v = src->data(src->index(source_row, c, parent), Qt::DisplayRole);
        if (v.toString().contains(search_, Qt::CaseInsensitive)) return true;
    }
    return false;
}

} // namespace manifest::gui
