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

#ifndef VFS_LINUX_H
#define VFS_LINUX_H

#define FUSE_USE_VERSION 32

#include "accountstate.h"
#include "vfs.h"
#include "vfs_cache.h"

#include <fuse.h>
#include <time.h>

#include <QString>
#include <QThread>
#include <QPointer>

namespace OCC {

class VfsLinux : public Vfs
{
    Q_OBJECT
public:
    VfsLinux(QString &, QString &, AccountState *);

    void mount();
    void unmount();

private:
    QString _mountpath;
    QString _cachepath;
    QPointer<AccountState> _accState;
    QPointer<VfsCache> _cache;

    struct fuse_chan *_fuseChan;
    struct fuse *_fuseFs;
    QThread _fuseThread;

    time_t _mountTime;
    uid_t _mountOwner;
    uid_t _mountGroup;

    int getattr(std::string, struct stat *, struct fuse_context *);
    int readdir(std::string, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *, struct fuse_context *);
    int read(const char *, char *, size_t, off_t, struct fuse_file_info *, struct fuse_context *);
    int write(const char *, const char *, size_t, off_t, struct fuse_file_info *, struct fuse_context *);
    int mkdir(const char *, mode_t mode, struct fuse_context *);

    static bool fuse_initialized;
    static struct fuse_operations _ops;

    static void initFuseStatic();
    static int doGetattr(const char *, struct stat *);
    static int doReaddir(const char *, void *, fuse_fill_dir_t, off_t,
        struct fuse_file_info *);
    static int doRead(const char *, char *, size_t, off_t,
        struct fuse_file_info *);
    static int doWrite(const char *, const char *, size_t, off_t, struct fuse_file_info *);
    static int doMkdir(const char *, mode_t);
};
} // namespace OCC

#endif