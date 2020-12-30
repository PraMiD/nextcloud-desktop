#ifndef VFSCACHEELEMENT_H
#define VFSCACHEELEMENT_H

#include <QObject>

namespace OCC {

class VfsCache;
class VfsCacheDirectory;

class VfsCacheElement : public QObject
{
    Q_OBJECT

    private:
        QString _name;
        VfsCache &_cache;

        QSharedPointer<VfsCacheDirectory> _parentDirectory;

    protected:
        VfsCacheElement(QString name, VfsCache &cache, QSharedPointer<VfsCacheDirectory> parent);
    
        QString getOnlinePath() const;
};

class VfsCacheDirectory : public VfsCacheElement
{
    Q_OBJECT

    public:
        VfsCacheDirectory(VfsCache &cache);

    protected:
        VfsCacheDirectory(QString name, VfsCache &cache, QSharedPointer<VfsCacheDirectory> parent);
};

class VfsCacheFile : public VfsCacheElement
{
    Q_OBJECT
};

}

#endif