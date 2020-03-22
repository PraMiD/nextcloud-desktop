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

#include "vfs_utils.h"

namespace OCC {
QString VfsUtils::getDirectory(QString path)
{
    if (path.endsWith("/"))
        path.chop(1);

    auto lastSep = path.lastIndexOf("/");
    if (lastSep == -1)
        return "/";

    return path.left(lastSep + 1);
}

QString VfsUtils::getFile(QString path)
{
    if (path.endsWith("/"))
        return QString();

    auto lastSep = path.lastIndexOf("/");
    if (lastSep == -1)
        return path;

    return path.right(path.length() - lastSep - 1);
}

QString VfsUtils::pathJoin(QString dir, QString file)
{
#if defined(Q_OS_WIN)
    QString separator = "\\";
#else
    QString separator = "/";
#endif

    return dir.endsWith(separator) ? QString(dir + file) : QString(dir + separator + file);
}
}