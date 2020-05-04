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
#include <QUuid>

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
    , _fastSync(false)
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

    _surrogateDir = file_info.absoluteFilePath() + "/surrogates/";
    if (!QDir::root().mkpath(_surrogateDir)) {
        // Surrogate directory is a file..
        auto msg = "Could not create directory for surrogate files: " + _fileCacheDir;
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
    _metadataCacheState = cacheMetaData.absoluteFilePath();
    if (cacheMetaData.exists()) {
        if (!cacheMetaData.isFile()) {
            auto msg = "Cache metadata path exists, but is no file: " + _metadataCacheState;
            qCritical() << Q_FUNC_INFO << msg;
            throw VfsCacheException(msg.toUtf8().constData());
        }

        loadCacheState();
    }

    QFileInfo journalMetaData(file_info.absoluteFilePath() + "/cachejournal.store");
    _metadataCacheJournalState = journalMetaData.absoluteFilePath();
    if (journalMetaData.exists()) {
        if (!journalMetaData.isFile()) {
            auto msg = "Cache journal metadata path exists, but is no file: " + _metadataCacheJournalState;
            qCritical() << Q_FUNC_INFO << msg;
            throw VfsCacheException(msg.toUtf8().constData());
        }

        loadJournalState();
    }

    _dictWalker.setParent(this);
    connect(this, &VfsCache::loadFolderContent, &_dictWalker, &OCC::DiscoveryFolderFileList::doGetFolderContent);
    connect(&_dictWalker, &OCC::DiscoveryFolderFileList::gotDataSignal, this, &VfsCache::handleDirectoryUpdate);

    _journal = new SyncJournalDb(journalCacheDir + "cache.journal", this);
    _eng = new SyncEngine(accState->account(), _fileCacheDir, "/", _journal);
    _eng->setIgnoreHiddenFiles(false);
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
                    processCacheJournal();
                }
                _cacheThread.msleep(_fastSync ? 200 : _refreshTime);
                _fastSync = false;
            }
        });
    _cacheThread.start();
}

void VfsCache::doFastSync()
{
    _fastSync = true;
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

    if (_cacheJournal.contains(file)) {
        qDebug() << Q_FUNC_INFO << "Processing journal file:" << file;

        auto journalFile = _cacheJournal.value(file);
        if (!journalFile->opPerformed) {
            qDebug() << Q_FUNC_INFO << "Operation not yet performed";
        } else {
            if (journalFile->op == JournalOperation::CREATE && item->_instruction == CSYNC_INSTRUCTION_NEW) {
                qDebug() << Q_FUNC_INFO << "Item created";
                journalFile->syncToRemote = true;
                storeJournalState();
            } else if (journalFile->op == JournalOperation::REMOVE && item->_instruction == CSYNC_INSTRUCTION_REMOVE) {
                qDebug() << Q_FUNC_INFO << "Item removed";
                journalFile->syncToRemote = true;
                storeJournalState();
            } else {
                qDebug() << Q_FUNC_INFO << "What?? Journal:" << ((int)journalFile->op) << ". Engine: " << item->_instruction;
            }
        }
    }

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

            if (!_fileMap.contains(path))
                throw VfsCacheNoSuchElementException(path.toStdString());

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

QStringList VfsCache::getDirListingJournalFiles(QString path)
{
    QStringList dirListing;
    if (!isDir(path))
        throw VfsCacheNoSuchElementException(path.toUtf8().constData());

    for (auto &journalElem : _cacheJournal.values()) {
        auto fPath = journalElem->onlinePath;
        auto fPathParent = VfsUtils::getDirectory(fPath);
        if (journalElem->op == JournalOperation::CREATE && fPathParent == path)
            dirListing.append(VfsUtils::getFile(fPath));
    }

    return dirListing;
}

QStringList VfsCache::getDirListing(QString path, bool remoteOnly)
{
    if (!remoteOnly && _cacheJournal.contains(path) && _cacheJournal.value(path)->op == JournalOperation::REMOVE) {
        // File/Directory marked for removal
        throw VfsCacheNoSuchElementException(path.toUtf8().constData());
    }

    if (!remoteOnly && _cacheJournal.contains(path)) {
        auto journalFile = _cacheJournal.value(path);
        if (journalFile->op == JournalOperation::CREATE && !journalFile->syncToRemote) {
            // Directory currently only exists locally
            return getDirListingJournalFiles(path);
        }
    }

    try {
        // Local + remote
        QStringList dirListing = remoteOnly ? QStringList() : getDirListingJournalFiles(path);
        auto intDirInfo = getIntDirInfo(path);

        for (auto &intFi : intDirInfo->list) {
            auto onlinePath = VfsUtils::pathJoin(path, intFi->path);
            if (remoteOnly || !_cacheJournal.contains(onlinePath) || _cacheJournal.value(onlinePath)->op != JournalOperation::REMOVE)
                dirListing.append(intFi->path);
        }

        return dirListing;
    } catch (VfsCacheNoSuchElementException &e) {
        // Maybe this file only exists in our journal?
        if (remoteOnly || !(_cacheJournal.contains(path) && _cacheJournal.value(path)->op != JournalOperation::REMOVE))
            throw;
        return getDirListingJournalFiles(path);
    }
}

VfsCacheFileInfo VfsCache::getFileInfo(QString path)
{
    auto dirPath = VfsUtils::getDirectory(path);
    auto file = VfsUtils::getFile(path);
    try {
        auto &dirListing = getIntDirInfo(dirPath)->list;

        auto intFiIt = std::find_if(dirListing.cbegin(), dirListing.cend(), [file](auto &intFi) { return intFi->path == file; });
        if (intFiIt != dirListing.cend())
            return fillFileInfo(*intFiIt, path);
    } catch (VfsCacheNoSuchElementException &) {
        // Try journal
    }


    // File is not on remote server -> Maybe it is in our journal?
    if (_cacheJournal.contains(path)) {
        auto journalFile = _cacheJournal.value(path);
        if (journalFile->op != JournalOperation::REMOVE) {
            QFileInfo surrogateFile(journalFile->journalSurrogateFilePath);

            VfsCacheFileInfo fi;
            fi.onlinePath = path;
            fi.type = journalFile->type;
            fi.size = surrogateFile.size();
            fi.modTime = surrogateFile.lastModified();
            fi.accessTime = surrogateFile.lastRead();

            return fi;
        }
    }
    qWarning() << Q_FUNC_INFO << "File info of non-existing file" << path << "requested";
    throw VfsCacheNoSuchElementException(path.toUtf8().constData());
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

bool VfsCache::createItem(const QString parentDirOnlinePath, const QString elemName, bool isDir)
{
    qDebug() << Q_FUNC_INFO << "Cache new element " << elemName << "at path:" << parentDirOnlinePath;

    if (!this->isDir(parentDirOnlinePath))
        throw VfsCacheNoSuchElementException(parentDirOnlinePath.toStdString());


    auto newJournalFile = QSharedPointer<VfsCacheJournalFile>(new VfsCacheJournalFile());
    auto onlinePath = VfsUtils::pathJoin(parentDirOnlinePath, elemName);

    if (_cacheJournal.contains(onlinePath))
        throw VfsCacheOpNotSuccessfulException(EAGAIN);

    newJournalFile->onlinePath = onlinePath;
    newJournalFile->type = isDir ? ItemType::ItemTypeDirectory : ItemType::ItemTypeFile;
    newJournalFile->op = JournalOperation::CREATE;
    newJournalFile->opPerformed = false;
    newJournalFile->syncToRemote = false;

    QString uuid = QUuid::createUuid().toString();
    newJournalFile->journalSurrogateFilePath = VfsUtils::pathJoin(_surrogateDir, uuid);
    bool created = false;
    QDir surrogateFilesDir(_surrogateDir);
    if (isDir) {
        created = surrogateFilesDir.mkdir(uuid);
    } else {
        QFile newFile(newJournalFile->journalSurrogateFilePath);
        if (newFile.open(QIODevice::ReadWrite)) {
            created = true;
            newFile.flush();
            newFile.close();
        }
    }

    if (!created)
        throw VfsCacheOpNotSuccessfulException(EACCES);

    for (auto &jf : _cacheJournal.values()) {
        // A create operation depends on creation of parent directories
        if (jf->onlinePath == VfsUtils::getDirectory(onlinePath))
            newJournalFile->dependsOn.append(jf->onlinePath);
    }

    _cacheJournal.insert(newJournalFile->onlinePath, newJournalFile);
    storeJournalState();
    return true;

    QSharedPointer<VfsCacheFile> newFileInfo = QSharedPointer<VfsCacheFile>(new VfsCacheFile());
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
        bool created = false;
        if (isDir) {
            created = parentDir.mkdir(elemName);
        } else {
            QFile newFile(newFileInfo->offlinePath);
            if (newFile.open(QIODevice::ReadWrite)) {
                created = true;
                newFile.flush();
                newFile.close();
            }
        }

        if (!created)
            throw VfsCacheOpNotSuccessfulException(EACCES);


        // Blocks until the new item is processed by the engine
        _eng->journal()
            ->forceRemoteDiscoveryNextSync();
        doSyncForFile(onlinePath);

        // Load the new directory/path into our list of remote items
        qDebug() << Q_FUNC_INFO << "Load file list for directory" << parentDirOnlinePath;
        loadFileList(parentDirOnlinePath); // I'm sorry for this..
    }

    return true;
}

void VfsCache::processCacheJournal()
{
    for (auto onlinePath : _cacheJournal.keys()) {
        // Only create and remove operations are currently stored in the
        // cache journal
        auto journalFile = _cacheJournal.value(onlinePath);
        if (journalFile->syncToRemote) {
            // Fully handled
            _cacheJournal.remove(onlinePath);

            try {
                cacheFile(onlinePath, false);
            } catch (VfsCacheNoSuchElementException &) {
                // Maybe not synched to our server file list
            }
            continue;
        }
        if (journalFile->opPerformed)
            continue;

        for (auto &dependJf : journalFile->dependsOn) {
            if (_cacheJournal.contains(dependJf) && !_cacheJournal.value(dependJf)->opPerformed) {
                qDebug() << "Wait for dependency" << dependJf << "of" << onlinePath;
                continue;
            }
        }

        auto parentOnlinePath = VfsUtils::getDirectory(onlinePath);

        // Ensure that the parent directory is marked to be synced
        auto parentOfflinePath = _fileCacheDir;
        QSharedPointer<VfsCacheFile> parentCacheFile;
        if (parentOnlinePath != "/") {
            try {
                parentCacheFile = cacheFile(parentOnlinePath, false);
            } catch (VfsCacheNoSuchElementException &) {
                if (!isDir(parentOnlinePath)) {
                    // Parent directory deleted before?
                    // We can ignore it when we want to delete the current element,
                    // but this should not happen on CREATEs
                    if (journalFile->op == JournalOperation::CREATE) {
                        qCritical() << "Parent element" << parentOnlinePath << "does not exist anymore, but we want to create" << onlinePath;
                    }
                    _cacheJournal.remove(onlinePath);
                }
                continue;
            }
            parentOfflinePath = parentCacheFile->offlinePath;

            // Check if the parent directory is already synced to our local copy
            QFileInfo parentFileInfo(parentCacheFile->offlinePath);
            if (!(parentFileInfo.exists() && parentFileInfo.isDir())) {
                // Wait for remote sync
                qDebug() << Q_FUNC_INFO << "Waiting for sync of parent dir of" << onlinePath;
                _eng->journal()
                    ->forceRemoteDiscoveryNextSync();
                doFastSync();
                continue;
            }
        }

        auto isDir = journalFile->type == ItemType::ItemTypeDirectory;
        QDir parentDir(parentOfflinePath);
        auto elemName = VfsUtils::getFile(onlinePath);

        switch (journalFile->op) {
        case JournalOperation::CREATE: {
            // Just create the file or directory
            qDebug() << "Create new" << (isDir ? "directory" : "file") << "called" << elemName << "at cache path" << parentDir.absolutePath();


            // Move the surrogate to its new location
            bool created = false;
            QDir surrogateFilesDir(VfsUtils::getDirectory(journalFile->journalSurrogateFilePath));
            if (!QFile::exists(journalFile->journalSurrogateFilePath)) {
                qWarning() << "Surrogate does not exist anymore.. Creating element directly";
                if (isDir) {
                    created = surrogateFilesDir.mkdir(VfsUtils::getFile(journalFile->journalSurrogateFilePath));
                } else {
                    QFile newFile(journalFile->journalSurrogateFilePath);
                    if (newFile.open(QIODevice::ReadWrite)) {
                        created = true;
                        newFile.flush();
                        newFile.close();
                    }
                }
            } else {
                created = QFile::rename(journalFile->journalSurrogateFilePath, VfsUtils::pathJoin(_fileCacheDir, journalFile->onlinePath));
            }

            journalFile->opPerformed = created;
        } break;
        case JournalOperation::REMOVE: {
            // Cache the file itself and remove it
            try {
                auto elemCacheFile = cacheFile(onlinePath, false);

                // Check if the file itself already exists locally
                QFileInfo fi(elemCacheFile->offlinePath);
                if (!(fi.exists() && (isDir ? fi.isDir() : fi.isFile()))) {
                    qDebug() << Q_FUNC_INFO << "Waiting for sync of" << onlinePath;
                    _eng->journal()
                        ->forceRemoteDiscoveryNextSync();
                    doFastSync();
                    continue;
                }

                if (isDir ? parentDir.rmdir(elemName) : parentDir.remove(elemName))
                    journalFile->opPerformed = true;
            } catch (VfsCacheNoSuchElementException &e) {
                // Journal got out of sync?
                qWarning() << "Non-existing element should be deleted by journal. Is the journal cache out of sync?";

                // The sync engine will never process this element anymore
                _cacheJournal.remove(onlinePath);
            }
        } break;
        }
    }
    storeJournalState();
}

QSharedPointer<VfsCacheFile> VfsCache::cacheFile(QString onlinePath, bool block)
{
    QString onlineDir = VfsUtils::getDirectory(onlinePath);
    QString filename = VfsUtils::getFile(onlinePath);

    // Check if the file exists
    auto files = getDirListing(onlineDir, true); // Throws an exception if the directory is not known
    if (!files.contains(filename)) {
        // The file does not exist 'officially' -> Maybe it exists, but is marked for removal?
        if (_cacheJournal.contains(onlinePath) && _cacheJournal.value(onlinePath)->op == JournalOperation::REMOVE) {
            qWarning() << Q_FUNC_INFO << "Client requested not existing file: " << onlinePath;
            throw VfsCacheNoSuchElementException(onlinePath.toUtf8().constData());
        }
    }

    {
        QMutexLocker locker(&_cacheOpRunning);
        // Check if the file is already cached
        if (_cachedFiles.contains(onlinePath)) {
            auto cacheFile = _cachedFiles.value(onlinePath);
            if (!QFileInfo(cacheFile->offlinePath).exists()) {
                _eng->journal()->forceRemoteDiscoveryNextSync();
                if (block)
                    doSyncForFile(onlinePath);
            }
            return cacheFile;
        }
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
        if (block)
            doSyncForFile(onlinePath);
    }


    // TODO: Handle symlinks correctly: Do we have to follow the link or is this done by the server?

    return newFileInfo;
}

const QString VfsCache::readFile(const QString onlinePath, off_t offset, size_t noBytes)
{
    if (!isFile(onlinePath)) {
        qCritical() << Q_FUNC_INFO << "FUSE requested non-existing file:" << onlinePath;
        throw VfsCacheNoSuchElementException(onlinePath.toStdString());
    }

    qDebug()
        << Q_FUNC_INFO << "FUSE read request:" << onlinePath;

    QString offlinePath = "";
    if (_cacheJournal.contains(onlinePath)) {
        // We checked before if the file exists -> Cache operation is not REMOVE
        offlinePath = _cacheJournal.value(onlinePath)->journalSurrogateFilePath;
        qDebug()
            << Q_FUNC_INFO << "Serve from journal:" << offlinePath;
    } else {
        // Cache file (if not already cached)
        auto cachedFile = cacheFile(onlinePath, true);
        cachedFile->lastAccess = QDateTime::currentDateTime();
        offlinePath = cachedFile->offlinePath;
        qDebug()
            << Q_FUNC_INFO << "Serve from remove:" << offlinePath;
    }

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
    createItem(parentDir, dirName, true);
}

void VfsCache::createFile(const QString onlinePath)
{
    auto fileName = VfsUtils::getFile(onlinePath);
    auto parentDir = VfsUtils::getDirectory(onlinePath);

    qDebug() << Q_FUNC_INFO << "Create file" << fileName << "in:" << parentDir;
    createItem(parentDir, fileName, false);
}

void VfsCache::removeDirectory(const QString onlinePath)
{
    auto dirName = VfsUtils::getFile(onlinePath);
    auto parentDir = VfsUtils::getDirectory(onlinePath);

    qDebug() << Q_FUNC_INFO << "Remove directory" << dirName << "in:" << parentDir;
    removeItem(parentDir, dirName, true);
}

void VfsCache::removeFile(const QString onlinePath)
{
    auto fileName = VfsUtils::getFile(onlinePath);
    auto parentDir = VfsUtils::getDirectory(onlinePath);

    qDebug() << Q_FUNC_INFO << "Remove file" << fileName << "in:" << parentDir;
    removeItem(parentDir, fileName, false);
}

bool VfsCache::removeItem(const QString parentDirOnlinePath, const QString elemName, bool isDir)
{
    if (elemName.isEmpty()) {
        auto msg = "Cannot remove root directory";
        qDebug() << Q_FUNC_INFO << msg;
        throw VfsCacheOpNotSuccessfulException(EPERM);
    }

    auto onlinePath = VfsUtils::pathJoin(parentDirOnlinePath, elemName);

    if ((isDir && !this->isDir(onlinePath)) || (!isDir && !isFile(onlinePath)))
        throw VfsCacheNoSuchElementException(onlinePath.toStdString());

    qDebug() << Q_FUNC_INFO << "Removing" << (isDir ? "directory" : "file") << elemName << "in:" << parentDirOnlinePath;

    auto newJournalFile = QSharedPointer<VfsCacheJournalFile>(new VfsCacheJournalFile());

    if (_cacheJournal.contains(onlinePath)) {
        // Another operation for the file is stored in the journal
        auto journalFile = _cacheJournal.value(onlinePath);
        if (journalFile->op == JournalOperation::REMOVE)
            return true; // Already marked for removal

        // We shall create the file
        if (journalFile->opPerformed) {
            _cacheJournal.remove(onlinePath); // Simply rewind the creation
            return true;
        }

        // To late -> Sync and remove afterwards
        throw VfsCacheOpNotSuccessfulException(EAGAIN);
    }

    newJournalFile->onlinePath = onlinePath;
    newJournalFile->type = isDir ? ItemType::ItemTypeDirectory : ItemType::ItemTypeFile;
    newJournalFile->op = JournalOperation::REMOVE;
    newJournalFile->opPerformed = false;
    newJournalFile->syncToRemote = false;

    for (auto &jf : _cacheJournal.values()) {
        // A remove operation depends on the removal of all subfiles/subdirs
        if (onlinePath == VfsUtils::getDirectory(jf->onlinePath))
            newJournalFile->dependsOn.append(jf->onlinePath);
    }

    _cacheJournal.insert(onlinePath, newJournalFile);
    storeJournalState();
    return true;

    auto cacheFileInfo = cacheFile(onlinePath, true); // Ensure that changes are synced!
    auto parentDirOfflinePath = VfsUtils::getDirectory(cacheFileInfo->offlinePath);
    QDir parentDir = QDir(parentDirOfflinePath);
    bool removed = true;
    if (isDir)
        removed = parentDir.rmdir(elemName);
    else
        removed = parentDir.remove(elemName);

    if (!removed)
        throw VfsCacheOpNotSuccessfulException(EACCES);

    {
        QMutexLocker locker(&_cacheOpRunning);
        _eng->journal()->forceRemoteDiscoveryNextSync();
        doSyncForFile(onlinePath);

        // Remove from cache -> File is removed
        _cachedFiles.remove(onlinePath);
        buildExcludeList();

        // Sync file list with remote
        loadFileList(VfsUtils::getDirectory(onlinePath));
    }

    return true;
}

void VfsCache::writeFile(const QString onlinePath, const QString data, off_t offset)
{
    if (!isFile(onlinePath)) {
        qCritical() << Q_FUNC_INFO << "FUSE requested non-existing file:" << onlinePath;
        throw VfsCacheNoSuchElementException(onlinePath.toStdString());
    }

    qDebug() << Q_FUNC_INFO << "FUSE write request:" << onlinePath;

    QString offlinePath = "";
    if (_cacheJournal.contains(onlinePath)) {
        // We checked before if the file exists -> Cache operation is not REMOVE
        offlinePath = _cacheJournal.value(onlinePath)->journalSurrogateFilePath;
        qDebug()
            << Q_FUNC_INFO << "Serve from journal:" << offlinePath;
    } else {
        // Cache file (if not already cached)
        auto cachedFile = cacheFile(onlinePath, true);
        cachedFile->lastAccess = QDateTime::currentDateTime();
        offlinePath = cachedFile->offlinePath;
        qDebug()
            << Q_FUNC_INFO << "Serve from remove:" << offlinePath;
    }

    auto fh = QSharedPointer<QFile>(new QFile(offlinePath));
    if (!fh->open(QIODevice::ReadWrite | QIODevice::Unbuffered)) { // RW for seeking ;)
        auto msg = QString("Could not open file (offline: " + offlinePath + "; online: " + onlinePath + " for writing");
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsCacheException(msg.toStdString());
    }

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

void VfsCache::truncateFile(const QString onlinePath, off_t newLen)
{
    if (!isFile(onlinePath)) {
        qCritical() << Q_FUNC_INFO << "FUSE requested non-existing file:" << onlinePath;
        throw VfsCacheNoSuchElementException(onlinePath.toStdString());
    }

    qDebug() << Q_FUNC_INFO << "FUSE truncate request:" << onlinePath;

    QString offlinePath = "";
    if (_cacheJournal.contains(onlinePath)) {
        // We checked before if the file exists -> Cache operation is not REMOVE
        offlinePath = _cacheJournal.value(onlinePath)->journalSurrogateFilePath;
        qDebug()
            << Q_FUNC_INFO << "Serve from journal:" << offlinePath;
    } else {
        // Cache file (if not already cached)
        auto cachedFile = cacheFile(onlinePath, true);
        cachedFile->lastAccess = QDateTime::currentDateTime();
        offlinePath = cachedFile->offlinePath;
        qDebug()
            << Q_FUNC_INFO << "Serve from remove:" << offlinePath;
    }

    auto fh = QSharedPointer<QFile>(new QFile(offlinePath));
    fh->resize(newLen);
    // Thread loop syncs the file
}


bool VfsCache::isFile(QString path)
{
    auto intFi = getFileInfo(path);
    return intFi.type == ItemType::ItemTypeFile || intFi.type == ItemType::ItemTypeSoftLink;
}

bool VfsCache::isDir(QString path)
{
    if (path == "/")
        return true;
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

void VfsCache::loadJournalState()
{
    qDebug() << "Load cache journal file: " << _metadataCacheJournalState;

    QFile journalStateFile(_metadataCacheJournalState);

    if (!journalStateFile.exists()) {
        QString msg = "Cannot read cache journal state, file does not exist: " + _metadataCacheJournalState;
        qCritical() << msg;
        throw VfsCacheException(msg.toStdString());
    }

    if (!journalStateFile.open(QIODevice::ReadOnly)) {
        QString msg = "Cannot read cache journal state, cannot open file: " + _metadataCacheJournalState;
        qCritical() << msg;
        throw VfsCacheException(msg.toStdString());
    }

    QList<VfsCacheJournalFile> cacheJournalFileList;
    QDataStream instream(&journalStateFile);
    instream >> cacheJournalFileList;
    journalStateFile.close();

    for (auto &jf : cacheJournalFileList)
        _cacheJournal.insert(jf.onlinePath, QSharedPointer<VfsCacheJournalFile>(new VfsCacheJournalFile(jf)));
}

void VfsCache::storeJournalState()
{
    qDebug() << "Store cache journal file: " << _metadataCacheJournalState;

    QFile journalStateFile(_metadataCacheJournalState);

    if (!journalStateFile.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        QString msg = "Cannot write cache state, cannot open file: " + _metadataCacheJournalState;
        qCritical() << msg;
        throw VfsCacheException(msg.toStdString());
    }

    QList<VfsCacheJournalFile> cacheJournalFileList;
    for (auto &journalFile : _cacheJournal.values())
        cacheJournalFileList.append(*journalFile);
    QDataStream outstream(&journalStateFile);
    outstream << cacheJournalFileList;
    journalStateFile.close();
}

void VfsCache::loadCacheState()
{
    qDebug() << "Load cache file: " << _metadataCacheState;

    QFile cacheStateFile(_metadataCacheState);

    if (!cacheStateFile.exists()) {
        QString msg = "Cannot read cache state, file does not exist: " + _metadataCacheState;
        qCritical() << msg;
        throw VfsCacheException(msg.toStdString());
    }

    if (!cacheStateFile.open(QIODevice::ReadOnly)) {
        QString msg = "Cannot read cache state, cannot open file: " + _metadataCacheState;
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
    qDebug() << "Store cache file: " << _metadataCacheState;

    QFile cacheStateFile(_metadataCacheState);

    if (!cacheStateFile.open(QIODevice::WriteOnly | QIODevice::Unbuffered)) {
        QString msg = "Cannot write cache state, cannot open file: " + _metadataCacheState;
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
