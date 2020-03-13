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

#ifndef VFS_H
#define VFS_H

#include <QObject>

namespace OCC {
class VfsMountException : public std::exception
{
private:
    std::string _what;

public:
    VfsMountException(std::string msg = "")
        : _what(msg){};

    const char *what() const throw()
    {
        return _what.c_str();
    }
};

class Vfs : public QObject
{
    Q_OBJECT
public:
    virtual void mount() = 0;
    virtual void unmount() = 0;
};

} // namespace OCC

#endif