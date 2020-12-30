#ifndef VFSCACHE_H
#define VFSCACHE_H

#include <QObject>
#include <QScopedPointer>

#include "accountstate.h"
#include "vfs_cache_element.h"

namespace OCC {

class VfsCache : public QObject
{
    Q_OBJECT

    private:
        QScopedPointer<VfsCacheDirectory> _root;
        AccountState *_accountState;

    public:
        VfsCache(AccountState *accState);
};

}

#endif