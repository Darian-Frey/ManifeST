#include "manifest/HatariLauncher.hpp"

#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace manifest {

HatariLauncher::Result HatariLauncher::launch(const std::filesystem::path& image_path) {
    const QString hatari = QStandardPaths::findExecutable("hatari");
    if (hatari.isEmpty()) {
        return {false, "hatari not found on $PATH"};
    }

    const QString qpath = QString::fromStdString(image_path.string());
    const bool ok = QProcess::startDetached(hatari, QStringList{qpath});
    if (!ok) {
        return {false, "QProcess::startDetached failed for " + image_path.string()};
    }
    return {true, {}};
}

} // namespace manifest
