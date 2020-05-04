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

#ifndef VFS_UTILS_H
#define VFS_UTILS_H

#include <QObject>

namespace OCC {
class VfsUtils : public QObject
{
    Q_OBJECT

public:
    static QString pathSeparator;

    static QString getDirectory(QString);
    static QString getFile(QString);
    static QString pathJoin(QString, QString);
};
}

#endif