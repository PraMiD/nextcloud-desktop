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
#include <QScopedPointer>

#include "config.h"
#include "configfile.h"

#include "vfs_cache.h"
#include "vfs_utils.h"
#include "folderman.h"
#include "folder.h"
#include "syncengine.h"


namespace OCC {
VfsCache::VfsCache(QString cacheDir, AccountState *accState, int refreshTimeMs)
    : _accState(accState)
    , _cacheDir(cacheDir)
    , _refreshTime(refreshTimeMs)
    , _dictWalker(accState->account())
    , _threadRunning(false)
    , _cacheSecs(60)
    , _cacheOpRunning(QMutex::Recursive)
    , _syncOpRunning(QMutex::Recursive)
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

    _fileCacheDir = file_info.absoluteFilePath() + "/files/";
    if (!QDir::root().mkpath(_fileCacheDir)) {
        // Cache directory is a file..
        auto msg = "Could not create directory for cached files: " + _fileCacheDir;
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toUtf8().constData());
    }

    QString journalCacheDir = file_info.absoluteFilePath() + "/journal/";
    if (!QDir::root().mkpath(journalCacheDir)) {
        // Cache directory is a file..
        auto msg = "Could not create directory for journal of cached files: " + journalCacheDir;
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toUtf8().constData());
    }

    QFileInfo cacheMetaData(file_info.absoluteFilePath() + "/cache.store");
    _metadataPath = cacheMetaData.absoluteFilePath();
    if (cacheMetaData.exists()) {
        if (!cacheMetaData.isFile()) {
            // Cache directory is a file..
            auto msg = "Cache metadata path exists, but is no file: " + _metadataPath;
            qCritical() << Q_FUNC_INFO << msg;
            throw VfsCacheException(msg.toUtf8().constData());
        }

        loadCacheState();
    }

    _dictWalker.setParent(this);
    connect(this, &VfsCache::loadFolderContent, &_dictWalker, &OCC::DiscoveryFolderFileList::doGetFolderContent);
    connect(&_dictWalker, &OCC::DiscoveryFolderFileList::gotDataSignal, this, &VfsCache::handleDirectoryUpdate);

    _journal = new SyncJournalDb(journalCacheDir + "cache.journal", this);
    _eng = new SyncEngine(accState->account(), _fileCacheDir, "/", _journal);
    connect(_eng, &SyncEngine::started, this, &VfsCache::syncStarted);
    connect(_eng, &SyncEngine::finished, this, &VfsCache::syncFinished);
    connect(_eng, &SyncEngine::rootEtag, this, &VfsCache::etagReceived);
    connect(_eng, &SyncEngine::csyncUnavailable, this, &VfsCache::syncUnavailable);
    connect(_eng, &SyncEngine::syncError, this, &VfsCache::syncError);
    connect(_eng, &SyncEngine::itemCompleted, this, &VfsCache::engineItemProcessed);

    _threadRunning = true;
    _cacheThread.setObjectName("VfsCacheThread");
    connect(&_cacheThread, &QThread::started,
        [this]() {
            qInfo() << Q_FUNC_INFO << "Started VFS thread cache at: " + this->_cacheDir;

            _cacheThread.msleep(2000);

            // Loop
            _syncStarted = false;
            while (_threadRunning) {
                if (_accState->isConnected()) {
                    qDebug() << Q_FUNC_INFO << "Refresh the directory listings of all currently cached directories";
                    _eng->setLocalDiscoveryOptions(LocalDiscoveryStyle::FilesystemOnly);


                    updateCurFileList();
                    if (checkCacheFiles()) // Remote discovery if files were removed from cache
                        _eng->journal()->forceRemoteDiscoveryNextSync();
                    buildExcludeList();
                    if (_syncOpRunning.try_lock())
                        doSync(); // Another sync is running -> fine
                }
                _cacheThread.msleep(_refreshTime);
            }
        });
    _cacheThread.start();
}

bool VfsCache::doSync()
{
    // Parent must hold _syncOpRunning
    if (!_eng->isSyncRunning() && !_syncStarted) {
        if (_accState->isConnected()) {
            setSyncOptions();
            _syncStarted = true;
            QMetaObject::invokeMethod(_eng, "startSync", Qt::QueuedConnection);
            return true;
        } else {
            qWarning() << Q_FUNC_INFO << "Not connected!";
        }
    } else {
        qDebug() << Q_FUNC_INFO << "Sync already running";
    }

    return false;
}

void VfsCache::doSyncForFile(QString path)
{
    qDebug() << Q_FUNC_INFO << "Sync and wait for file" << path << "being processed";
    QSharedPointer<QMutex> mut(new QMutex());
    QSharedPointer<QWaitCondition> waitCond(new QWaitCondition());
    QPair<QSharedPointer<QMutex>, QSharedPointer<QWaitCondition>> syncPair(mut, waitCond);

    {
        mut->lock();
        _waitForSyncFiles.insert(path, syncPair);
        _syncOpRunning.lock();
        while (!doSync()) {
            _syncOpRunning.unlock();
            mut->unlock();

            QThread::currentThread()->msleep(100);
            _syncOpRunning.lock();
            mut->lock();
        }
        waitCond->wait(mut.data());
    }
    qDebug() << Q_FUNC_INFO << "File" << path << "processed";

    _waitForSyncFiles.remove(path);
}

void VfsCache::setSyncOptions()
{
    SyncOptions opt;
    ConfigFile cfgFile;

    auto newFolderLimit = cfgFile.newBigFolderSizeLimit();
    opt._newBigFolderSizeLimit = newFolderLimit.first ? newFolderLimit.second * 1000LL * 1000LL : -1; // convert from MB to B
    opt._confirmExternalStorage = cfgFile.confirmExternalStorage();
    opt._moveFilesToTrash = cfgFile.moveToTrash();

    QByteArray chunkSizeEnv = qgetenv("OWNCLOUD_CHUNK_SIZE");
    if (!chunkSizeEnv.isEmpty()) {
        opt._initialChunkSize = chunkSizeEnv.toUInt();
    } else {
        opt._initialChunkSize = cfgFile.chunkSize();
    }
    QByteArray minChunkSizeEnv = qgetenv("OWNCLOUD_MIN_CHUNK_SIZE");
    if (!minChunkSizeEnv.isEmpty()) {
        opt._minChunkSize = minChunkSizeEnv.toUInt();
    } else {
        opt._minChunkSize = cfgFile.minChunkSize();
    }
    QByteArray maxChunkSizeEnv = qgetenv("OWNCLOUD_MAX_CHUNK_SIZE");
    if (!maxChunkSizeEnv.isEmpty()) {
        opt._maxChunkSize = maxChunkSizeEnv.toUInt();
    } else {
        opt._maxChunkSize = cfgFile.maxChunkSize();
    }

    // Previously min/max chunk size values didn't exist, so users might
    // have setups where the chunk size exceeds the new min/max default
    // values. To cope with this, adjust min/max to always include the
    // initial chunk size value.
    opt._minChunkSize = qMin(opt._minChunkSize, opt._initialChunkSize);
    opt._maxChunkSize = qMax(opt._maxChunkSize, opt._initialChunkSize);

    QByteArray targetChunkUploadDurationEnv = qgetenv("OWNCLOUD_TARGET_CHUNK_UPLOAD_DURATION");
    if (!targetChunkUploadDurationEnv.isEmpty()) {
        opt._targetChunkUploadDuration = std::chrono::milliseconds(targetChunkUploadDurationEnv.toUInt());
    } else {
        opt._targetChunkUploadDuration = cfgFile.targetChunkUploadDuration();
    }

    _eng->setSyncOptions(opt);
}

void VfsCache::engineItemProcessed(const SyncFileItemPtr &item)
{
    auto file = QString("/" + item->_file);

    qDebug() << Q_FUNC_INFO << "Keys:" << _waitForSyncFiles.keys() << "and current file" << file;
    if (_waitForSyncFiles.contains(file)) {
        qDebug() << Q_FUNC_INFO << "Processing wait file:" << file;
        auto mut = _waitForSyncFiles.value(file).first;
        auto condVar = _waitForSyncFiles.value(file).second;

        QMutexLocker locker(mut.data());
        condVar->wakeAll();
    }
    qDebug() << Q_FUNC_INFO << file << item->_status << item->_instruction;

    switch (item->_status) {
    case SyncFileItem::Status::Success:
        qDebug() << Q_FUNC_INFO << "Processed" << file << "successfully";
        switch (item->_instruction) {
        case CSYNC_INSTRUCTION_NONE:
            qDebug() << Q_FUNC_INFO << "CSYNC_INSTRUCTION_NONE";
            break;
        case CSYNC_INSTRUCTION_EVAL:
            qDebug() << Q_FUNC_INFO << "CSYNC_INSTRUCTION_EVAL";
            break;
        case CSYNC_INSTRUCTION_RENAME:
            qDebug() << Q_FUNC_INFO << "CSYNC_INSTRUCTION_RENAME";
            break;
        case CSYNC_INSTRUCTION_NEW:
            qDebug() << Q_FUNC_INFO << "CSYNC_INSTRUCTION_NEW";
            break;
        case CSYNC_INSTRUCTION_CONFLICT:
            qDebug() << Q_FUNC_INFO << "CSYNC_INSTRUCTION_CONFLICT";
            break;
        case CSYNC_INSTRUCTION_IGNORE:
            qDebug() << Q_FUNC_INFO << "CSYNC_INSTRUCTION_IGNORE";
            break;
        case CSYNC_INSTRUCTION_SYNC:
            qDebug() << Q_FUNC_INFO << "CSYNC_INSTRUCTION_SYNC";
            break;
        case CSYNC_INSTRUCTION_REMOVE:
            qDebug() << Q_FUNC_INFO << "CSYNC_INSTRUCTION_REMOVE";
            break;
        default:
            qDebug() << Q_FUNC_INFO << "I don't know.." << item->_instruction;
        }
        break;
    case SyncFileItem::Status::FileIgnored:
        qDebug() << Q_FUNC_INFO << "file" << file << "ignored:" << item->_instruction;
        break;
    default:
        qDebug() << Q_FUNC_INFO << "file" << file << "different status:" << item->_status;
    }
}

void VfsCache::syncError(const QString &msg, ErrorCategory category)
{
    qInfo() << Q_FUNC_INFO << "SYNC ERROR:" << msg;
    _syncStarted = false;
}

void VfsCache::syncUnavailable()
{
    qInfo() << Q_FUNC_INFO << "CANNOT SYNC";
    _syncStarted = false;
}

void VfsCache::etagReceived(const QString &etag)
{
    qInfo() << Q_FUNC_INFO << "Got etag:" << etag;
}

void VfsCache::syncStarted()
{
    qInfo() << Q_FUNC_INFO << "Sync started";
}

void VfsCache::syncFinished(bool success)
{
    qInfo() << Q_FUNC_INFO << "Sync finished with result:" << success;
    _syncStarted = false;
    _syncOpRunning.unlock();
}

void VfsCache::updateCurFileList()
{
    // TODO: Ensure thread safety
    auto curPaths = _fileMap.keys();
    for (auto p : curPaths)
        loadFileList(p);
}

void VfsCache::loadFileList(QString path)
{
    try {
        qDebug() << Q_FUNC_INFO << "Starting directory update for path:" << path;
        {
            QMutexLocker locker(&_fileListMut);
            emit loadFolderContent(path);
            if (!_updateCondVar.wait(&_fileListMut, 3000)) {
                qWarning() << Q_FUNC_INFO << "Timeout while loading file list";
            }

            QStringList dirListing;
            for (auto &intFi : _fileMap.value(path)->list)
                dirListing.append(intFi->path);

            qDebug() << "New folder content of" << path << ":" << dirListing;
        }
    } catch (VfsCacheNoSuchElementException &e) {
        qCritical() << Q_FUNC_INFO << "Path does not exist:" << e.what();
    } catch (VfsCacheException &e) {
        qCritical() << Q_FUNC_INFO << "Could not finish update:" << e.what();
    }
}

void VfsCache::handleDirectoryUpdate(DiscoveryDirectoryResult *res)
{
    QString path = res->path;
    qDebug()
        << Q_FUNC_INFO << "File update returned:" << res->path;
    {
        QMutexLocker locker(&_fileListMut);

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
    }

    qDebug() << Q_FUNC_INFO << "Updated file/directory listing in VfsCache for " + path;
    _updateCondVar.wakeAll();
}

VfsCache::~VfsCache()
{
}

QSharedPointer<OCC::DiscoveryDirectoryResult> VfsCache::getIntDirInfo(QString path)
{
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
    }
}

QStringList VfsCache::getDirListing(QString path)
{
    auto intDirInfo = getIntDirInfo(path);
    QStringList dirListing;

    for (auto &intFi : intDirInfo->list)
        dirListing.append(intFi->path);

    return dirListing;
}

VfsCacheFileInfo VfsCache::getFileInfo(QString path)
{
    auto dirPath = VfsUtils::getDirectory(path);
    auto file = VfsUtils::getFile(path);
    auto &dirListing = getIntDirInfo(dirPath)->list;

    auto intFiIt = std::find_if(dirListing.cbegin(), dirListing.cend(), [file](auto &intFi) { return intFi->path == file; });
    if (intFiIt == dirListing.cend()) {
        qWarning() << Q_FUNC_INFO << "File info of non-existing file" << path << "requested";
        throw VfsCacheNoSuchElementException(path.toUtf8().constData());
    }

    return fillFileInfo(*intFiIt, path);
}

VfsCacheFileInfo VfsCache::fillFileInfo(const std::unique_ptr<csync_file_stat_t> &intFi, QString path)
{
    VfsCacheFileInfo fi;

    fi.onlinePath = path;
    fi.type = intFi->type;

    {
        QMutexLocker locker(&_cacheOpRunning);
        if (_cachedFiles.contains(path)) {
            // File cached -> Load values from local copy as they might not be synced to remote
            auto cacheFile = _cachedFiles.value(path);
            fi.accessTime = cacheFile->lastAccess;
            fi.modTime = cacheFile->lastModification;
            fi.size = QFile(cacheFile->offlinePath).size();
        } else {
            fi.accessTime = QDateTime();
            fi.accessTime.setSecsSinceEpoch(intFi->modtime);
            fi.modTime = fi.accessTime;
            fi.size = intFi->size;
        }
    }

    return fi;
}

void VfsCache::buildExcludeList()
{
    /*
     * Compare the list of files that shall be cached and the current list of
     * known online files -> Build an ignore list
     * Therefore, we iterate over all known files and directories (ensuring that
     * we visit a directory before its files) and check if we can ignore the
     * items. All directries/files that are not known to our _fileMap will be
     * ignored as their parent directory will be ignored by this method!
     */
    QStringList toVisitPaths({ "/" });
    _excludedItems.clear();
    while (toVisitPaths.size() > 0) {
        QStringList newVisitPaths;

        for (auto path : toVisitPaths) {
            if (canIgnoreDir(path)) {
                if (path != "/")
                    path = path.right(path.length() - 1);
                _excludedItems.append(path);
            } else {
                // Check if single files can be ignored
                for (auto fPath : getFilesInDir(path)) {
                    if (canIgnoreFile(fPath))
                        if (fPath != "/")
                            fPath = fPath.right(fPath.length() - 1);
                    _excludedItems.append(fPath);
                }

                // Add subdirectries to new check list
                newVisitPaths += getDirsInDir(path);
            }
        }

        toVisitPaths = newVisitPaths;
    }

    storeCacheState();

    qDebug() << Q_FUNC_INFO << "Excluded items:" << _excludedItems;

    // TODO: Ensure that file is synced before it is deleted
    _journal->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, _excludedItems);
}

QStringList VfsCache::getDirsInDir(QString dir)
{
    QStringList subdirs;

    for (auto &elem : getDirListing(dir)) {
        auto fullElemPath = VfsUtils::pathJoin(dir, elem);
        if (getFileInfo(fullElemPath).type == ItemTypeDirectory)
            subdirs.append(fullElemPath);
    }

    qDebug() << Q_FUNC_INFO << subdirs;
    return subdirs;
}

QStringList VfsCache::getFilesInDir(QString dir)
{
    QStringList files;

    for (auto &elem : getDirListing(dir)) {
        auto fullElemPath = VfsUtils::pathJoin(dir, elem);
        if (getFileInfo(fullElemPath).type == ItemTypeFile)
            files.append(fullElemPath);
    }

    qDebug() << Q_FUNC_INFO << files;
    return files;
}

bool VfsCache::canIgnoreFile(QString path)
{
    // We are allowed to ignore files if they are not explicitly cached
    QMutexLocker locker(&_cacheOpRunning);
    return !_cachedFiles.contains(path);
}

bool VfsCache::canIgnoreDir(QString path)
{
    QMutexLocker locker(&_cacheOpRunning);
    if (_cachedFiles.contains(path)) // Explicitly cached directory
        return false;

    // Check if the current path is a subpath of any cached file
    auto cachedPaths = _cachedFiles.keys();
    auto match = std::find_if(
        cachedPaths.cbegin(), cachedPaths.cend(), [path](auto &cacheF) { return cacheF.startsWith(path); });

    return match == cachedPaths.cend();
}

void VfsCache::cacheNewItem(const QString parentDirOnlinePath, const QString elemName, bool isDir)
{
    qDebug() << Q_FUNC_INFO << "Cache new element " << elemName << "at path:" << parentDirOnlinePath;
    QSharedPointer<VfsCacheFile> newFileInfo = QSharedPointer<VfsCacheFile>(new VfsCacheFile());
    auto onlinePath = VfsUtils::pathJoin(parentDirOnlinePath, elemName);
    newFileInfo->onlinePath = onlinePath;
    newFileInfo->offlinePath = VfsUtils::pathJoin(_fileCacheDir, onlinePath.right(onlinePath.length() - 1));
    newFileInfo->download = newFileInfo->lastAccess = newFileInfo->lastModification = QDateTime::currentDateTime();
    {
        QMutexLocker locker(&_cacheOpRunning);
        _cachedFiles.insert(onlinePath, newFileInfo);
        buildExcludeList(); // Update ignore list -> Does not affect the new file as it is not known to the cloud

        // Ensure that the parent directory is created locally
        // -> Sync if it is not currently available
        QDir parentDir(VfsUtils::getDirectory(newFileInfo->offlinePath));
        if (!parentDir.exists()) {
            _eng->journal()
                ->forceRemoteDiscoveryNextSync();
            doSyncForFile(parentDirOnlinePath);
            qDebug() << Q_FUNC_INFO << "Cached" << parentDirOnlinePath << "locally";
        }

        // Create the new element
        qDebug() << "Create new" << (isDir ? "directory" : "file") << "called" << elemName << "at cache path" << parentDir.absolutePath();
        if (isDir) {
            parentDir.mkdir(elemName);
        } else {
            QFile newFile(newFileInfo->offlinePath);
            newFile.open(QIODevice::ReadWrite);
            newFile.flush();
            newFile.close();
        }


        // Blocks until the new item is processed by the engine
        _eng->journal()
            ->forceRemoteDiscoveryNextSync();
        doSyncForFile(onlinePath);

        // Load the new directory/path into our list of remote items
        qDebug() << Q_FUNC_INFO << "Load file list for directory" << parentDirOnlinePath;
        loadFileList(parentDirOnlinePath); // I'm sorry for this..
    }
}

QSharedPointer<VfsCacheFile> VfsCache::cacheFile(QString onlinePath)
{
    QString onlineDir = VfsUtils::getDirectory(onlinePath);
    QString filename = VfsUtils::getFile(onlinePath);

    // Check if the file exists
    auto files = getDirListing(onlineDir); // Throws an exception if the directory is not known
    if (!files.contains(filename)) {
        qWarning() << Q_FUNC_INFO << "Client requested not existing file: " << onlinePath;
        throw VfsCacheNoSuchElementException(onlinePath.toUtf8().constData());
    }

    {
        QMutexLocker locker(&_cacheOpRunning);
        // Check if the file is already cached
        if (_cachedFiles.contains(onlinePath))
            return _cachedFiles.value(onlinePath);
    }

    // Download file and add it to the cache
    qInfo() << Q_FUNC_INFO << "Add" << onlinePath << "to cache";
    QSharedPointer<VfsCacheFile> newFileInfo = QSharedPointer<VfsCacheFile>(new VfsCacheFile());
    newFileInfo->onlinePath = onlinePath;
    newFileInfo->offlinePath = VfsUtils::pathJoin(_fileCacheDir, onlinePath.right(onlinePath.length() - 1));
    newFileInfo->download = newFileInfo->lastAccess = QDateTime::currentDateTime();
    newFileInfo->lastModification = getFileInfo(onlinePath).modTime;
    {
        QMutexLocker locker(&_cacheOpRunning);
        _cachedFiles.insert(onlinePath, newFileInfo);
        buildExcludeList(); // Tell the syncEngine to not ignore the file

        // Blocks until the file is processed by the engine
        _eng->journal()->forceRemoteDiscoveryNextSync();
        doSyncForFile(onlinePath);
    }


    // TODO: Handle symlinks correctly: Do we have to follow the link or is this
    //          done by the server

    return newFileInfo;
}

const QString VfsCache::readFile(const QString onlinePath, off_t offset, size_t noBytes)
{
    if (!isFile(onlinePath)) {
        qCritical() << Q_FUNC_INFO << "FUSE requested unknown existing file:" << onlinePath;
        throw VfsCacheNoSuchElementException(onlinePath.toStdString());
    }

    qDebug()
        << Q_FUNC_INFO << "FUSE read request:" << onlinePath;

    // Cache file (if not already cached)
    auto cachedFile = cacheFile(onlinePath);
    cachedFile->lastAccess = QDateTime::currentDateTime();


    //auto fh = cachedFile->fh;
    auto offlinePath = cachedFile->offlinePath;
    //if (!fh) {
    auto fh = QSharedPointer<QFile>(new QFile(offlinePath));
    if (!fh->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        auto msg = QString("Could not open file (offline: " + offlinePath + "; online: " + onlinePath + " for reading");
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toStdString());
    }
    //}

    if (!fh->seek(offset)) {
        auto msg = QString("Could not seek to offset in file (offline: " + offlinePath + "; online: " + onlinePath);
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toStdString());
    }

    auto data = QString(fh->read(noBytes));
    fh->close();
    return data;
}

void VfsCache::createDirectory(const QString onlinePath)
{
    auto dirName = VfsUtils::getFile(onlinePath);
    auto parentDir = VfsUtils::getDirectory(onlinePath);

    qDebug() << Q_FUNC_INFO << "Create directory" << dirName << "in:" << parentDir;
    cacheNewItem(parentDir, dirName, true);
}

void VfsCache::removeDirectory(const QString onlinePath)
{
    auto dirName = VfsUtils::getFile(onlinePath);

    if (onlinePath == "/") {
        auto msg = "Cannot remove root directory";
        qDebug() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg);
    }

    qDebug() << Q_FUNC_INFO << "Removing directory '" << dirName << "' in:" << VfsUtils::getDirectory(onlinePath);

    auto cacheFileInfo = cacheFile(onlinePath); // Ensure that changes are synced!
    QDir parentDir = QDir(VfsUtils::getDirectory(cacheFileInfo->offlinePath));
    parentDir.rmdir(dirName);

    {
        qDebug() << Q_FUNC_INFO << "A";
        QMutexLocker locker(&_cacheOpRunning);
        qDebug() << Q_FUNC_INFO << "B";
        _eng->journal()->forceRemoteDiscoveryNextSync();
        doSyncForFile(onlinePath);
        qDebug() << Q_FUNC_INFO << "C";

        // Remove from cache -> File is removed
        _cachedFiles.remove(onlinePath);
        buildExcludeList();
        qDebug() << Q_FUNC_INFO << "D";

        // Sync file list with remote
        loadFileList(VfsUtils::getDirectory(onlinePath));
        qDebug() << Q_FUNC_INFO << "E";
    }
}

void VfsCache::writeFile(const QString onlinePath, const QString data, off_t offset)
{
    if (!isFile(onlinePath)) {
        qCritical() << Q_FUNC_INFO << "FUSE requested unknown existing file:" << onlinePath;
        throw VfsCacheNoSuchElementException(onlinePath.toStdString());
    }

    qDebug() << Q_FUNC_INFO << "FUSE write request:" << onlinePath;

    // Cache file if not already cached
    auto cachedFile = cacheFile(onlinePath);
    cachedFile->lastAccess = cachedFile->lastModification = QDateTime::currentDateTime();


    //auto fh = cachedFile->fh;
    auto offlinePath = cachedFile->offlinePath;
    //if (!fh) {
    auto fh = QSharedPointer<QFile>(new QFile(offlinePath));
    if (!fh->open(QIODevice::ReadWrite | QIODevice::Unbuffered)) { // RW for seeking ;)
        auto msg = QString("Could not open file (offline: " + offlinePath + "; online: " + onlinePath + " for writing");
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toStdString());
    }
    //}

    if (!fh->seek(offset)) {
        auto msg = QString("Could not seek to offset in file (offline: " + offlinePath + "; online: " + onlinePath);
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toStdString());
    }

    fh->write(data.toStdString().c_str(), data.length());
    fh->flush();
    fh->close();
    // Thread loop syncs the file
}

bool VfsCache::isFile(QString path)
{
    auto intFi = getFileInfo(path);
    return intFi.type == ItemType::ItemTypeFile || intFi.type == ItemType::ItemTypeSoftLink;
}

bool VfsCache::isDir(QString path)
{
    return getFileInfo(path).type == ItemType::ItemTypeDirectory;
}

bool VfsCache::checkCacheFiles()
{
    auto curTime = QDateTime::QDateTime::currentDateTime();
    QStringList toRemove;

    for (auto &cacheItem : _cachedFiles) {
        if (cacheItem->lastAccess.secsTo(curTime) > _cacheSecs) {
            qDebug() << Q_FUNC_INFO << "Delete from cache (" << cacheItem->lastAccess.secsTo(curTime) << ")" << cacheItem->onlinePath;
            toRemove.append(cacheItem->onlinePath);
        } else {
            qDebug() << Q_FUNC_INFO << "Keep in cache (" << cacheItem->lastAccess.secsTo(curTime) << ")" << cacheItem->onlinePath;
        }
    }

    {
        QMutexLocker locker(&_cacheOpRunning);
        for (auto &remItem : toRemove) {
            qDebug() << Q_FUNC_INFO << "Removing file from cache:" << remItem;
            _cachedFiles.remove(remItem);
        }
    }

    return !toRemove.empty();
}

void VfsCache::loadCacheState()
{
    qDebug() << "Load cache file: " << _metadataPath;

    QFile cacheStateFile(_metadataPath);

    if (!cacheStateFile.exists()) {
        QString msg = "Cannot read cache state, file does not exist: " + _metadataPath;
        qCritical() << msg;
        throw VfsCacheException(msg.toStdString());
    }

    if (!cacheStateFile.open(QIODevice::ReadOnly)) {
        QString msg = "Cannot read cache state, cannot open file: " + _metadataPath;
        qCritical() << msg;
        throw VfsCacheException(msg.toStdString());
    }

    QList<VfsCacheFile> cacheFileList;
    QDataStream instream(&cacheStateFile);
    instream >> cacheFileList;

    {
        QMutexLocker locker(&_cacheOpRunning);
        for (auto &cacheFile : cacheFileList)
            _cachedFiles.insert(cacheFile.onlinePath, QSharedPointer<VfsCacheFile>(new VfsCacheFile(cacheFile)));
    }

    cacheStateFile.close();
}

void VfsCache::storeCacheState()
{
    qDebug() << "Store cache file: " << _metadataPath;

    QFile cacheStateFile(_metadataPath);

    if (!cacheStateFile.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        QString msg = "Cannot write cache state, cannot open file: " + _metadataPath;
        qCritical() << msg;
        throw VfsCacheException(msg.toStdString());
    }

    QList<VfsCacheFile> cacheFileList;
    {
        QMutexLocker locker(&_cacheOpRunning);
        for (auto cacheFile : _cachedFiles.values())
            cacheFileList.append(*cacheFile);
    }
    QDataStream outstream(&cacheStateFile);
    outstream << cacheFileList;

    cacheStateFile.close();
}
}
