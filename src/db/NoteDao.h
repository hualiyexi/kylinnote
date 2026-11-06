#ifndef KYLINNOTE_NOTEDAO_H
#define KYLINNOTE_NOTEDAO_H

#include "../Common.h"

#include <QVector>

class QSqlQuery;   // 全局作用域前向声明，避免与 kn 命名空间混淆

namespace kn {

// notes 表的数据访问层：全部返回 bool 表示是否写库成功，失败时不影响内存状态
class NoteDao
{
public:
    QVector<NoteRecord> fetchAll(bool includeDeleted = false);
    bool fetch(qint64 id, NoteRecord &out);

    // 插入后回填 rec.id
    bool insert(NoteRecord &rec);
    bool update(const NoteRecord &rec);
    bool updateGeometry(qint64 id, const QRect &geometry);
    bool updateZOrder(qint64 id, int zOrder);

    // 软删：仅置 deleted=1，行数据保留
    bool softDelete(qint64 id);

    int maxZOrder();

private:
    static NoteRecord recordFromQuery(QSqlQuery &q);
};

} // namespace kn

#endif // KYLINNOTE_NOTEDAO_H
