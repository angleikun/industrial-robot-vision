#include "JsonSettings.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QDebug>
#include <functional>

JsonSettings::JsonSettings(const QString &filePath, QObject *parent)
    : QObject(parent), m_filePath(filePath)
{
    load();
}

bool JsonSettings::load()
{
    QMutexLocker lock(&m_mutex);
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "JsonSettings: cannot open" << m_filePath << ", using defaults";
        return false;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "JsonSettings: parse error" << err.errorString();
        return false;
    }
    m_root = doc.object();
    return true;
}

QVariant JsonSettings::value(const QString &dottedKey, const QVariant &defaultValue) const
{
    QMutexLocker lock(&m_mutex);

    QStringList parts = dottedKey.split('.');
    QJsonObject current = m_root;

    for (int i = 0; i < parts.size() - 1; i++) {
        if (!current.contains(parts[i]) || !current[parts[i]].isObject())
            return defaultValue;
        current = current[parts[i]].toObject();
    }

    QString last = parts.last();
    if (!current.contains(last))
        return defaultValue;

    return current[last].toVariant();
}

void JsonSettings::setValue(const QString &dottedKey, const QVariant &v)
{
    QMutexLocker lock(&m_mutex);

    QStringList parts = dottedKey.split('.');

    std::function<void(QJsonObject&, const QStringList&, int, const QVariant&)> setRecursive;
    setRecursive = [&](QJsonObject &obj, const QStringList &keys, int depth, const QVariant &val) {
        if (depth == keys.size() - 1) {
            obj[keys.last()] = QJsonValue::fromVariant(val);
            return;
        }
        const QString &key = keys[depth];
        QJsonObject sub;
        if (obj.contains(key) && obj[key].isObject()) {
            sub = obj[key].toObject();
        }
        setRecursive(sub, keys, depth + 1, val);
        obj[key] = sub;
    };

    setRecursive(m_root, parts, 0, v);
    emit valueChanged(dottedKey, v);
}

bool JsonSettings::sync()
{
    QMutexLocker lock(&m_mutex);
    QFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "JsonSettings: cannot write" << m_filePath;
        return false;
    }
    QJsonDocument doc(m_root);
    f.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

QStringList JsonSettings::allKeys() const
{
    QMutexLocker lock(&m_mutex);

    QStringList result;
    std::function<void(const QJsonObject&, const QString&)> collect;
    collect = [&](const QJsonObject &obj, const QString &prefix) {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            QString fullKey = prefix.isEmpty() ? it.key() : prefix + "." + it.key();
            if (it.value().isObject()) {
                collect(it.value().toObject(), fullKey);
            } else {
                result << fullKey;
            }
        }
    };
    collect(m_root, QString());
    result.sort();
    return result;
}
