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
#include "vfs_utils.h"
#include "vfs_cache.h"

#include <fuse.h>

#include <QFileInfo>
#include <QDir>

/* Hints:
 * void SyncJournalDb::forceRemoteDiscoveryNextSyncLocked()
 */

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
    _ops.write = VfsLinux::doWrite;

    fuse_initialized = true;
}

int VfsLinux::getattr(std::string path, struct stat *st, struct fuse_context *)
{
    // TODO: Check access rights
    auto err = ENOENT;

    if (path == "/") {
        st->st_mode = S_IFDIR | 0740;
        st->st_nlink = 2;
        st->st_uid = _mountOwner;
        st->st_gid = _mountGroup;
        st->st_atime = _mountTime;
        st->st_mtime = _mountTime;

        err = 0;
    } else {
        try {
            // Get directory listing of parent directory of the requested path
            auto qpath = QString::fromStdString(path);
            auto dir = VfsUtils::getDirectory(qpath);
            auto file = VfsUtils::getFile(qpath);
            auto data = _cache->getDirListing(dir);

            err = ENOENT;
            for (auto &f : data->list) {
                if (f->path == file) {
                    err = 0;
                    // File found
                    switch (f->type) {
                    case ItemType::ItemTypeSkip:
                        err = ENOENT;
                    case ItemType::ItemTypeDirectory:
                        st->st_mode = S_IFDIR;
                        err = 0;
                        break;
                    case ItemType::ItemTypeSoftLink:
                        st->st_mode = S_IFLNK;
                        err = 0;
                        break;
                    case ItemType::ItemTypeFile:
                        st->st_mode = S_IFREG;
                        st->st_mode |= 0740;
                        err = 0;
                        break;
                    }

                    if (err == 0) {
                        st->st_nlink = 2;
                        st->st_uid = _mountOwner;
                        st->st_gid = _mountGroup;
                        st->st_atime = f->modtime;
                        st->st_mtime = f->modtime;
                        st->st_size = f->size;
                        st->st_blksize = 1;
                        st->st_blocks = f->size / 512 + ((f->size % 512) > 1);
                    }
                }
            }
        } catch (VfsCacheNoSuchElementException &e) {
            // No such element
            err = ENOENT;
        }
    }

    return -err;
}

int VfsLinux::readdir(std::string path, void *buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info *, struct fuse_context *)
{
    int err = ENOENT;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    try {
        auto data = _cache->getDirListing(QString::fromStdString(path));
        for (auto &e : data->list)
            filler(buf, e->path, NULL, 0);
        err = 0;
    } catch (VfsCacheNoSuchElementException &e) {
        // No such element
        err = ENOENT;
    }
    return -err;
}

int VfsLinux::write(const char *c_path, const char *buf, size_t size, off_t off, struct fuse_file_info *, struct fuse_context *)
{
    try {
        _cache->writeFile(QString(c_path), QString::fromStdString(std::string(buf, size)), off);
        return size;
    } catch (VfsCacheNoSuchElementException &e) {
        // No such element
        qWarning() << Q_FUNC_INFO << "File" << c_path << "does not exist";
        return -ENOENT;
    }
}

int VfsLinux::read(const char *c_path, char *buf, size_t size, off_t off, struct fuse_file_info *fi, struct fuse_context *)
{
    try {
        auto data = _cache->readFile(QString(c_path), off, size);
        strncpy(buf, data.toUtf8().data(), size);

        return data.length() > size ? size : data.length();
    } catch (VfsCacheNoSuchElementException &e) {
        // No such element
        qWarning() << Q_FUNC_INFO << "File" << c_path << "does not exist";
        return -ENOENT;
    }
}

int VfsLinux::doGetattr(const char *c_path, struct stat *st)
{
    qDebug() << Q_FUNC_INFO;

    auto ctx = fuse_get_context();
    return static_cast<VfsLinux *>(ctx->private_data)->getattr(c_path, st, ctx);
}
int VfsLinux::doReaddir(const char *c_path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *file_info)
{
    qDebug() << Q_FUNC_INFO;

    auto ctx = fuse_get_context();
    return static_cast<VfsLinux *>(ctx->private_data)->readdir(c_path, buf, filler, offset, file_info, ctx);
}
int VfsLinux::doRead(const char *c_path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    qDebug() << Q_FUNC_INFO;

    auto ctx = fuse_get_context();
    return static_cast<VfsLinux *>(ctx->private_data)->read(c_path, buf, size, off, fi, ctx);
}
int VfsLinux::doWrite(const char *c_path, const char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
    qDebug() << Q_FUNC_INFO;

    auto ctx = fuse_get_context();
    return static_cast<VfsLinux *>(ctx->private_data)->write(c_path, buf, size, off, fi, ctx);
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
            qCritical() << Q_FUNC_INFO << msg;
            throw VfsMountException(msg.toUtf8().constData());
        }

        qInfo() << "Created mount directory: " + this->_mountpath;
    }

    if (!mountPointFi.isDir()) {
        // Cannot mount on files
        auto msg = "Mountpath is not a directory: " + this->_mountpath;
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }

    if (!QDir(this->_mountpath).isEmpty()) {
        // Cannot mount on non-empty directories
        auto msg = "Mountpoint not empty: " + this->_mountpath;
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }


    char *argv[0] = {};
    struct fuse_args args = FUSE_ARGS_INIT(0, argv);
    if ((
            _fuseChan = fuse_mount(
                this->_mountpath.toUtf8().constData(), &args))
        == NULL) {
        auto msg = "Could not mount FUSE: " + this->_mountpath;
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }

    if ((
            _fuseFs = fuse_new(_fuseChan, &args, &_ops, sizeof(_ops), this))
        == NULL) {
        auto msg = "Could not setup FUSE: " + this->_mountpath;
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }

    _mountTime = time(NULL);
    _mountOwner = getuid();
    _mountGroup = getgid();

    // Setup our cache
    try {
        _cache = new VfsCache(_cachepath, _accState);
        qDebug() << Q_FUNC_INFO << "Cache for VFS at " << this->_mountpath << "set up";
    } catch (VfsCacheException &e) {
        auto msg = "Cannot setup VFS (" + this->_mountpath + "). Cache could not be created: " + e.what();
        qCritical() << Q_FUNC_INFO << msg;
        throw VfsMountException(msg.toUtf8().constData());
    }

    // Enter the FUSE loop in another thread
    _fuseThread.setObjectName("VfsFuseThread");
    connect(&_fuseThread, &QThread::started,
        [this]() {
            qInfo() << Q_FUNC_INFO << "Mounting fuse FS/VFS at: " + this->_mountpath;
            fuse_loop(_fuseFs);

            qInfo() << Q_FUNC_INFO << "Unmounting fuse FS/VFS at: " + this->_mountpath;
            fuse_unmount(this->_mountpath.toUtf8().constData(), _fuseChan);
            fuse_destroy(_fuseFs);
        });
    _fuseThread.start();
}

void VfsLinux::unmount()
{
    // TODO: Wait until FUSE is actually unmounted
    fuse_exit(_fuseFs);
    qInfo() << Q_FUNC_INFO << "Unmounted FUSE/VFS: " + this->_mountpath;
}

} // namespace OCC