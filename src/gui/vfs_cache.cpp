/*
* Copyright(C) 2020 by Marko Dorfhuber <markodorfhuber@gmx.de>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
*(at your option) any later version.
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
* or FITNESS FOR A PARTICULAR PURPOSE.See the GNU General Public License
* for more details.
*/

#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QMutexLocker>

#include "vfs_cache.h"
#include "vfs_utils.h"

namespace OCC {
VfsCache::VfsCache(QString cacheDir, AccountState *accState, int refreshTimeMs)
    : _accState(accState)
    , _cacheDir(cacheDir)
    , _refreshTime(refreshTimeMs)
    , _threadRunning(false)
{
    // Check if the cache directory exists
    auto file_info = QFileInfo(this->_cacheDir);
    if (!file_info.exists()) {
        // Cache directory does not exist -> Create
        if (!QDir::root().mkpath(this->_cacheDir)) {
            // Cannot create directory
            auto msg = "Cannot create cache directory: " + this->_cacheDir;
            qCritical() << Q_FUNC_INFO << msg;
            throw VfsCacheException(msg.toUtf8().constData());
        }

        qInfo() << Q_FUNC_INFO << "Created cache directory: " + this->_cacheDir;
    }

    if (!file_info.isDir()) {
        // Cache directory is a file..
        auto msg = "Cache directory is not a directory: " + this->_cacheDir;
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toUtf8().constData());
    }

    QString fileCacheDir = file_info.absoluteFilePath() + "/files";
    if (!QDir::root().mkpath(fileCacheDir)) {
        // Cache directory is a file..
        auto msg = "Could not create directory for cached files: " + fileCacheDir;
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toUtf8().constData());
    }

    if (QFileInfo(file_info.absoluteFilePath() + "/vfs-metadata").exists())
        loadCacheState();

    //_fileListWorker = new OCC::DiscoveryFolderFileList(_accState->account());
    //_fileListWorker->setParent(this);
    //connect(this, &VfsCache::startFileUpdate, _fileListWorker, &OCC::DiscoveryFolderFileList::doGetFolderContent);
    //connect(_fileListWorker, &OCC::DiscoveryFolderFileList::gotDataSignal, this, &VfsCache::fileUpdateFinished);


    _threadRunning = true;
    _cacheThread.setObjectName("VfsCacheThread");
    connect(&_cacheThread, &QThread::started,
        [this]() {
            qInfo() << Q_FUNC_INFO << "Started VFS thread cache at: " + this->_cacheDir;

            _cacheThread.msleep(2000);

            /*_fileMap.insert("/", nullptr);*/
            // Loop
            while (_threadRunning) {
                qDebug() << Q_FUNC_INFO << "Refresh the directory listings of all currently cached directories";
                updateCurFileList();
                _cacheThread.msleep(_refreshTime);
            }
        });
    _cacheThread.start();
}

void VfsCache::updateCurFileList()
{
    // Ensure thread safety
    /*auto curPaths = _fileMap.keys();
    for (auto p : curPaths)
        loadFileList(p);*/
}

void VfsCache::loadFileList(QString path)
{
    _fileListMut.lock();
    emit startFileUpdate(path);

    // Wait for the results
    _updateCondVar.wait(&_fileListMut);
    _fileListMut.unlock();
}

void VfsCache::fileUpdateFinished(DiscoveryDirectoryResult *res)
{
    /*_fileListMut.lock();
    qDebug() << Q_FUNC_INFO << "File update returned";

    QString path = res->path;

    // Only delete the old cached information if we got a valid result or the
    // entry does no longer exist
    if (res->code == ENOENT || res->code == 0) {
        // Old element is deleted -> Shared Pointer is no longer referenced
        _fileMap.remove(path);
    } else {
        // Error on update..
        qWarning() << Q_FUNC_INFO << "Update for path '" << res->path << "' failed";
    }

    // Only update the cached information if we got a valid result
    if (res->code == 0)
        _fileMap.insert(path, QSharedPointer<OCC::DiscoveryDirectoryResult>(res));
    _fileListMut.unlock();

    qDebug() << Q_FUNC_INFO << "Updated file/directory listing in VfsCache for " + path;
    _updateCondVar.wakeAll();*/
}

VfsCache::~VfsCache()
{
    storeCacheState();
}

void VfsCache::loadCacheState()
{
    qDebug() << Q_FUNC_INFO << "Load cache state from disk";
}

void VfsCache::storeCacheState()
{
    qDebug() << Q_FUNC_INFO << "Store cache state to disk";
}

QSharedPointer<OCC::DiscoveryDirectoryResult> VfsCache::getDirListing(QString path)
{
    /*
    {
        QMutexLocker locker(&_fileListMut);
        if (_fileMap.contains(path)) {
            return _fileMap.value(path);
        }
    }

    loadFileList(path);

    {
        QMutexLocker locker(&_fileListMut);
        if (_fileMap.contains(path)) {
            return _fileMap.value(path);
        } else {
            qWarning() << Q_FUNC_INFO << "Could not load content of requested directory: " + path;
            throw VfsCacheNoSuchElementException(path.toUtf8().constData());
        }
    }*/
}

QString VfsCache::cacheFile(QString onlinePath)
{
    QString onlineDir = VfsUtils::getDirectory(onlinePath);
    QString filename = VfsUtils::getFile(onlinePath);

    // Check if the file exists
    auto &files = getDirListing(onlineDir)->list; // Throws an exception if the directory is not known
    if (find_if(
            files.cbegin(), files.cend(), [filename](auto &f) { return filename == f->path; })
        == files.cend()) {
        auto file = VfsUtils::pathJoin(onlineDir, filename);
        qWarning() << Q_FUNC_INFO << "Client requested not existing file: " << onlinePath;
        throw VfsCacheNoSuchElementException(onlinePath.toUtf8().constData());
    }

    // Check if the file is already cached
    /*if (_cachedFiles.contains(onlinePath))
        return _cachedFiles.value(onlinePath)->offlinePath;*/

    // Download file and add it to the cache


    // TODO: Handle symlinks correctly: Do we have to follow the link or is this
    //          done by the server
}

QString VfsCache::readFile(QString onlinePath, off_t offset, size_t noBytes)
{
    // Cache file (if not already cached)
    auto offlinePath = cacheFile(onlinePath);
}
}
