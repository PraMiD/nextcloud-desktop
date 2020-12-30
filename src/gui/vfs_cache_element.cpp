#include <QDebug>
#include <QDir>

#include "vfs_cache_element.h"

namespace OCC {

VfsCacheElement::VfsCacheElement(QString name, VfsCache &cache, QSharedPointer<VfsCacheDirectory> parent)
    : _name(name), _cache(cache), _parentDirectory(parent) {}

VfsCacheDirectory::VfsCacheDirectory(QString name, VfsCache &cache, QSharedPointer<VfsCacheDirectory> parent)
    : VfsCacheElement(name, cache, parent)
{
    // Load files/directories contained in this directory from server
    QStringList subdirs;

    qDebug() << Q_FUNC_INFO
                << "Loading subfiles for directory at online path:"
                << name;
}

VfsCacheDirectory::VfsCacheDirectory(VfsCache &cache)
    : VfsCacheDirectory("/", cache, nullptr) {}


QString VfsCacheElement::getOnlinePath() const
{
    QString parentPath = _parentDirectory ? _parentDirectory->getOnlinePath() : "/";
    return QDir(parentPath).filePath(_name);
}

}