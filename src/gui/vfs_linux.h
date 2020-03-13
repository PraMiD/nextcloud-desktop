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

#include <fuse.h>

#include <QString>
#include <QThread>

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
    AccountState *_accState;

    struct fuse_chan *_fuse_chan;
    struct fuse *_fuse_fs;
    QThread _fuse_thread;

    static bool fuse_initialized;
    static struct fuse_operations _ops;

    static void initFuseStatic();
    static int doGetattr(const char *, struct stat *);
    static int doReaddir(const char *, void *, fuse_fill_dir_t, off_t,
        struct fuse_file_info *);
    static int doRead(const char *, char *, size_t, off_t,
        struct fuse_file_info *);
};
} // namespace OCC

#endif