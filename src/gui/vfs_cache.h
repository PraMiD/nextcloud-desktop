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

#ifndef VFS_CACHE_H
#define VFS_CACHE_H

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QSharedPointer>
#include <QStringList>

#include <exception>
#include <string>

#include "accountstate.h"
#include "discoveryphase.h"
#include "syncengine.h"

namespace OCC {
class VfsCacheException : public std::exception
{
private:
    std::string _what;

public:
    VfsCacheException(std::string msg = "")
        : _what(msg) {};

    const char *what() const throw()
    {
        return _what.c_str();
    }
};

class VfsCacheNoSuchElementException : public VfsCacheException
{
private:
    std::string _what;

public:
    VfsCacheNoSuchElementException(std::string path = "")
        : _what("No element with path: " + path) {};

    const char *what() const throw()
    {
        return _what.c_str();
    }
};

class VfsCacheOpNotSuccessfulException : public VfsCacheException
{
private:
    std::string _what;
    int _err;

public:
    VfsCacheOpNotSuccessfulException(int err)
        : _what("Operation was not successful: " + err)
        , _err(err) {};

    const char *what() const throw()
    {
        return _what.c_str();
    }

    int err()
    {
        return _err;
    }
};


struct VfsCacheFileInfo
{
    QString onlinePath;
    QDateTime accessTime;
    QDateTime modTime;
    ItemType type;
    off_t size;
};

enum class JournalOperation {
    CREATE,
    REMOVE
};

struct VfsCacheJournalFile
{
    QString onlinePath;
    ItemType type;
    JournalOperation op;
    QString journalSurrogateFilePath;
    bool opPerformed;
    bool syncToRemote;
    QStringList dependsOn;

    friend QDataStream &operator<<(QDataStream &out, const VfsCacheJournalFile &journalFile)
    {
        out << journalFile.onlinePath << qint8(journalFile.type) << qint8(journalFile.op) << journalFile.journalSurrogateFilePath << journalFile.opPerformed << journalFile.syncToRemote << journalFile.dependsOn;
        return out;
    }

    friend QDataStream &operator>>(QDataStream &in, VfsCacheJournalFile &journalFile)
    {
        qint8 intType, intOp;
        in >> journalFile.onlinePath >> intType >> intOp >> journalFile.journalSurrogateFilePath >> journalFile.opPerformed >> journalFile.syncToRemote >> journalFile.dependsOn;
        journalFile.type = ItemType(intType);
        journalFile.op = JournalOperation(intOp);
        return in;
    }
};

struct VfsCacheFile
{
    QDateTime lastAccess;
    QDateTime lastModification;
    QDateTime download;
    QString onlinePath;
    QString offlinePath;

    friend QDataStream &operator<<(QDataStream &out, const VfsCacheFile &cacheFile)
    {
        out << cacheFile.lastAccess << cacheFile.lastModification << cacheFile.download << cacheFile.onlinePath << cacheFile.offlinePath;
        return out;
    }

    friend QDataStream &operator>>(QDataStream &in, VfsCacheFile &cacheFile)
    {
        in >> cacheFile.lastAccess >> cacheFile.lastModification >> cacheFile.download >> cacheFile.onlinePath >> cacheFile.offlinePath;
        return in;
    }
};


class VfsCache : public QObject
{
    Q_OBJECT

private:
    QPointer<AccountState> _accState;
    QString _cacheDir;
    QString _metadataCacheState;
    QString _metadataCacheJournalState;
    QString _fileCacheDir;
    QString _surrogateDir;
    QThread _cacheThread;
    int _refreshTime;

    QStringList _excludedItems;

    DiscoveryFolderFileList _dictWalker;

    bool _threadRunning;
    QMutex _fileListMut;
    QWaitCondition _updateCondVar;

    SyncJournalDb *_journal;
    SyncEngine *_eng;
    bool _fastSync;

    QMap<QString, QSharedPointer<OCC::DiscoveryDirectoryResult>> _fileMap;
    QMap<QString, QSharedPointer<VfsCacheFile>> _cachedFiles;

    // Use for create and remove
    QMap<QString, QSharedPointer<VfsCacheJournalFile>> _cacheJournal;
    qint64 _cacheSecs;

    QMap<QString, QPair<QSharedPointer<QMutex>, QSharedPointer<QWaitCondition>>> _waitForSyncFiles;

    // Used to protect file updates (read, write) and sync operations
    // -> Recursive mutex
    QMutex _syncOpRunning;
    bool _syncStarted;

    // Used to protect operations that modify/access the cache
    QMutex _cacheOpRunning;

    void loadCacheState();
    void storeCacheState();
    void loadJournalState();
    void storeJournalState();

    void updateCurFileList();
    void loadFileList(QString);

    QSharedPointer<VfsCacheFile> cacheFile(QString, bool);
    bool createItem(const QString, const QString, bool);
    bool removeItem(const QString, const QString, bool);

    void setSyncOptions();
    bool doSync();
    void doSyncForFile(QString path);

    void buildExcludeList();
    bool checkCacheFiles();

    QStringList getDirsInDir(QString path);
    QStringList getFilesInDir(QString path);
    bool canIgnoreFile(QString path);
    bool canIgnoreDir(QString path);

    bool isFile(QString path);
    bool isDir(QString path);
    QSharedPointer<OCC::DiscoveryDirectoryResult> getIntDirInfo(QString);
    VfsCacheFileInfo fillFileInfo(const std::unique_ptr<csync_file_stat_t> &, QString);

    void processCacheJournal();
    QStringList getDirListingJournalFiles(QString);
    void doFastSync();


private slots:
    void handleDirectoryUpdate(OCC::DiscoveryDirectoryResult *);
    void syncFinished(bool);
    void syncStarted();
    void etagReceived(const QString &);
    void syncUnavailable();
    void syncError(const QString &, ErrorCategory category = ErrorCategory::Normal);
    void engineItemProcessed(const SyncFileItemPtr &);

signals:
    void loadFolderContent(QString);

public:
    VfsCache(QString cacheDir, AccountState *accState, int refreshTimeMs = 10000);
    ~VfsCache();

    QStringList getDirListing(QString, bool = false);
    VfsCacheFileInfo getFileInfo(QString);
    const QString readFile(const QString, off_t, size_t);
    void createDirectory(const QString onlinePath);
    void removeDirectory(const QString onlinePath);
    void removeFile(const QString onlinePath);
    void createFile(const QString);
    void writeFile(const QString, const QString, off_t);
};
}

#endif