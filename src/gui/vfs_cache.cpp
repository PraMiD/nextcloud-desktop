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

    _excludeFilesPath = file_info.absoluteFilePath() + "/ignore.lst";
    _eng->excludedFiles().addExcludeFilePath(_excludeFilesPath);
    // TODO: Load cache!
    buildExcludeList();

    _threadRunning = true;
    _cacheThread.setObjectName("VfsCacheThread");
    connect(&_cacheThread, &QThread::started,
        [this]() {
            qInfo() << Q_FUNC_INFO << "Started VFS thread cache at: " + this->_cacheDir;

            _cacheThread.msleep(2000);

            // Loop
            _syncStarted = false;
            while (_threadRunning) {
                qDebug() << Q_FUNC_INFO << "Refresh the directory listings of all currently cached directories";
                _eng->setLocalDiscoveryOptions(LocalDiscoveryStyle::FilesystemOnly);
                if (!_eng->isSyncRunning() && !_syncStarted) {
                    qDebug() << Q_FUNC_INFO << "NO SYNC RUNNING";
                    if (_accState->isConnected()) {
                        qDebug() << Q_FUNC_INFO << "STARTING SYNC";
                        setSyncOptions();
                        _syncStarted = true;
                        QMetaObject::invokeMethod(_eng, "startSync", Qt::QueuedConnection);
                    } else {
                        qDebug() << Q_FUNC_INFO << "Not connected";
                    }
                }

                updateCurFileList();
                buildExcludeList();
                _cacheThread.msleep(_refreshTime);
            }
        });
    _cacheThread.start();
}

void VfsCache::syncExcludedFiles()
{
    QFile f(_excludeFilesPath);
    f.open(QIODevice::Unbuffered | QIODevice::Truncate | QIODevice::WriteOnly);

    // We have to replace leading / for the sync engine
    QStringList exportList;
    std::transform(_excludedItems.begin(), _excludedItems.end(), std::back_inserter(exportList), [](auto &path) {
        if (path == "/")
            return QString("*");
        if (path.startsWith("/"))
            return path.remove(0, 1);
        return path;
    });
    auto data = QString(exportList.join("\n") + "\n");
    f.write(data.toUtf8().data(), data.length());
    f.close();
    if (_eng)
        _eng->excludedFiles().reloadExcludeFiles();
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
    auto file = item->_file;
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
        default:
            qDebug() << Q_FUNC_INFO << "I don't know.." << item->_instruction;
        }
        break;
    case SyncFileItem::Status::FileIgnored:
        qDebug() << Q_FUNC_INFO << "file" << file << "ignored";
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
            _updateCondVar.wait(&_fileListMut);
        }
    } catch (VfsCacheNoSuchElementException &e) {
        qCritical() << Q_FUNC_INFO << "Path does not exist:" << e.what();
    } catch (VfsCacheException &e) {
        qCritical() << Q_FUNC_INFO << "Could not finish update:" << e.what();
    }
}

void VfsCache::handleDirectoryUpdate(DiscoveryDirectoryResult *res)
{
    qDebug()
        << Q_FUNC_INFO << "File update returned";

    QString path = res->path;
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

QSharedPointer<OCC::DiscoveryDirectoryResult> VfsCache::getDirListing(QString path)
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
                _excludedItems.append(path);
            } else {
                // Check if single files can be ignored
                for (auto fPath : getFilesInDir(path)) {
                    if (canIgnoreFile(fPath))
                        _excludedItems.append(fPath);
                }

                // Add subdirectries to new check list
                newVisitPaths += getDirsInDir(path);
            }
        }

        toVisitPaths = newVisitPaths;
    }

    syncExcludedFiles();
}

QStringList VfsCache::getDirsInDir(QString dir)
{
    QStringList subdirs;

    for (auto &elem : getDirListing(dir)->list) {
        if (elem->type == ItemTypeDirectory)
            subdirs.append(VfsUtils::pathJoin(dir, elem->path));
    }

    qDebug() << Q_FUNC_INFO << subdirs;
    return subdirs;
}

QStringList VfsCache::getFilesInDir(QString dir)
{
    QStringList files;

    for (auto &elem : getDirListing(dir)->list) {
        if (elem->type == ItemTypeFile)
            files.append(VfsUtils::pathJoin(dir, elem->path));
    }

    qDebug() << Q_FUNC_INFO << files;
    return files;
}

bool VfsCache::canIgnoreFile(QString path)
{
    // We are allowed to ignore files if they are not explicitly cached
    return !_cachedFiles.contains(path);
}

bool VfsCache::canIgnoreDir(QString path)
{
    if (_cachedFiles.contains(path)) // Explicitly cached directory
        return false;

    // Check if the current path is a subpath of any cached file
    auto cachedPaths = _cachedFiles.keys();
    auto match = std::find_if(
        cachedPaths.cbegin(), cachedPaths.cend(), [path](auto &cacheF) { return cacheF.startsWith(path); });

    return match == cachedPaths.cend();
}

QString VfsCache::cacheFile(QString onlinePath)
{
    QString onlineDir = VfsUtils::getDirectory(onlinePath);
    QString filename = VfsUtils::getFile(onlinePath);

    // Check if the file exists
    auto &files = getDirListing(onlineDir)->list; // Throws an exception if the directory is not known
    if (std::find_if(
            files.cbegin(), files.cend(), [filename](auto &f) { return filename == f->path; })
        == files.cend()) {
        qWarning() << Q_FUNC_INFO << "Client requested not existing file: " << onlinePath;
        throw VfsCacheNoSuchElementException(onlinePath.toUtf8().constData());
    }

    // Check if the file is already cached
    if (_cachedFiles.contains(onlinePath))
        return _cachedFiles.value(onlinePath)->offlinePath;

    // Download file and add it to the cache
    qInfo() << Q_FUNC_INFO << "Add" << onlinePath << "to cache";
    QSharedPointer<VfsCacheFile> newFileInfo = QSharedPointer<VfsCacheFile>(new VfsCacheFile());
    newFileInfo->onlinePath = onlinePath;
    newFileInfo->offlinePath = VfsUtils::pathJoin(_fileCacheDir, onlinePath.right(onlinePath.length() - 1));
    qInfo() << Q_FUNC_INFO << newFileInfo->offlinePath;
    _cachedFiles.insert(onlinePath, newFileInfo);
    buildExcludeList(); // Tell the syncEngine to not ignore the file


    // TODO: Handle symlinks correctly: Do we have to follow the link or is this
    //          done by the server

    return newFileInfo->offlinePath;
}

QString VfsCache::readFile(QString onlinePath, off_t offset, size_t noBytes)
{
    // Cache file (if not already cached)
    auto offlinePath = cacheFile(onlinePath);
}
}
