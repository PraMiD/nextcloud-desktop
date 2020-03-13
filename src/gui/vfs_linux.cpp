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

#include "vfs_linux.h"

#include <fuse.h>

#include <QFileInfo>
#include <QDir>

namespace OCC {

bool VfsLinux::fuse_initialized = false;
struct fuse_operations VfsLinux::_ops;
void VfsLinux::initFuseStatic()
{
    if (fuse_initialized)
        return;
    _ops.getattr = VfsLinux::doGetattr;
    _ops.readdir = VfsLinux::doReaddir;
    _ops.read = VfsLinux::doRead;

    fuse_initialized = true;
}


int VfsLinux::doGetattr(const char *, struct stat *)
{
    return -1;
}
int VfsLinux::doReaddir(const char *, void *, fuse_fill_dir_t, off_t, struct fuse_file_info *)
{
    return -1;
}
int VfsLinux::doRead(const char *, char *, size_t, off_t, struct fuse_file_info *)
{
    return -1;
}

VfsLinux::VfsLinux(QString &mountPath, QString &cachePath, AccountState *accState)
    : _mountpath(mountPath)
    , _cachepath(cachePath)
    , _accState(accState)
{
}

void VfsLinux::mount()
{
    initFuseStatic();

    auto mountPointFi = QFileInfo(this->_mountpath);
    if (!mountPointFi.exists()) {
        // Mount path does not exist
        if (!QDir::root().mkpath(this->_mountpath)) {
            // Cannot create directory
            auto msg = "Cannot create mountpath: " + this->_mountpath;
            qCritical() << msg;
            throw VfsMountException(msg.toUtf8().constData());
        }

        qInfo() << "Created mount directory: " + this->_mountpath;
    }

    if (!mountPointFi.isDir()) {
        // Cannot mount on files
        auto msg = "Mountpath is not a directory: " + this->_mountpath;
        qCritical() << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }

    if (!QDir(this->_mountpath).isEmpty()) {
        // Cannot mount on non-empty directories
        auto msg = "Mountpoint not empty: " + this->_mountpath;
        qCritical() << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }


    char *argv[0] = {};
    struct fuse_args args = FUSE_ARGS_INIT(0, argv);
    if ((
            _fuse_chan = fuse_mount(
                this->_mountpath.toUtf8().constData(), &args))
        == NULL) {
        auto msg = "Could not mount FUSE: " + this->_mountpath;
        qCritical() << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }

    if ((
            _fuse_fs = fuse_new(_fuse_chan, &args, &_ops, sizeof(_ops), this))
        == NULL) {
        auto msg = "Could not setup FUSE: " + this->_mountpath;
        qCritical() << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }

    // Enter the FUSE loop in another thread
    moveToThread(&_fuse_thread);
    connect(&_fuse_thread, &QThread::started,
        [this]() {
            fuse_loop(_fuse_fs);
            fuse_unmount(this->_mountpath.toUtf8().constData(), _fuse_chan);
            fuse_destroy(_fuse_fs);

            qInfo() << "Mounted fuse FS at: " + this->_mountpath;
        });
    _fuse_thread.start();
}

void VfsLinux::unmount()
{
    // TODO: Wait until FUSE is actually unmounted
    fuse_exit(_fuse_fs);
}

} // namespace OCC