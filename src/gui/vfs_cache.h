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

struct VfsCacheFile : public QObject
{
    Q_OBJECT

public:
    QDateTime lastAccess;
    QDateTime download;
    QString onlinePath;
    QString offlinePath;
    QSharedPointer<QFile> fh;
};

class VfsCache : public QObject
{
    Q_OBJECT

private:
    QPointer<AccountState> _accState;
    QString _cacheDir;
    QString _fileCacheDir;
    QThread _cacheThread;
    int _refreshTime;

    QStringList _excludedItems;

    DiscoveryFolderFileList _dictWalker;

    bool _threadRunning;
    QMutex _fileListMut;
    QWaitCondition _updateCondVar;

    SyncJournalDb *_journal;
    SyncEngine *_eng;

    QMap<QString, QSharedPointer<OCC::DiscoveryDirectoryResult>> _fileMap;
    QMap<QString, QSharedPointer<VfsCacheFile>> _cachedFiles;
    qint64 _cacheSecs;

    QMap<QString, QPair<QSharedPointer<QMutex>, QSharedPointer<QWaitCondition>>> _waitForSyncFiles;

    bool _syncStarted;

    void updateCurFileList();
    void loadFileList(QString);

    QSharedPointer<VfsCacheFile> cacheFile(QString);

    void setSyncOptions();
    void doSync();
    void doSyncForFile(QString path);

    void buildExcludeList();
    bool checkCacheFiles();

    QStringList getDirsInDir(QString path);
    QStringList getFilesInDir(QString path);
    bool canIgnoreFile(QString path);
    bool canIgnoreDir(QString path);

    bool isFile(QString path);


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

    QSharedPointer<OCC::DiscoveryDirectoryResult> getDirListing(QString);
    const QString readFile(QString, off_t, size_t);
};
}

#endif