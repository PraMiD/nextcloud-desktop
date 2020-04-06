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
};

class VfsCache : public QObject
{
    Q_OBJECT

private:
    QPointer<AccountState> _accState;
    //QPointer<DiscoveryFolderFileList> _fileListWorker;
    QString _cacheDir;
    QThread _cacheThread;
    int _refreshTime;

    DiscoveryFolderFileList _dictWalker;

    bool _threadRunning;
    QMutex _fileListMut;
    QWaitCondition _updateCondVar;

    QMap<QString, QSharedPointer<OCC::DiscoveryDirectoryResult>> _fileMap;
    //QMap<QString, QSharedPointer<VfsCacheFile>> _cachedFiles;

    void
    loadCacheState();
    void storeCacheState();

    void updateCurFileList();
    void loadFileList(QString);

    QString cacheFile(QString);


private slots:
    void handleDirectoryUpdate(OCC::DiscoveryDirectoryResult *);

signals:
    void loadFolderContent(QString path);

public:
    VfsCache(QString cacheDir, AccountState *accState, int refreshTimeMs = 10000);
    ~VfsCache();

    QSharedPointer<OCC::DiscoveryDirectoryResult> getDirListing(QString);
    QString readFile(QString, off_t, size_t);
};
}

#endif