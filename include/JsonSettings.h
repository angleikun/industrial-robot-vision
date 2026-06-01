#ifndef JSONSETTINGS_H
#define JSONSETTINGS_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QJsonObject>
#include <QMutex>

class JsonSettings : public QObject {
    Q_OBJECT

public:
    explicit JsonSettings(const QString &filePath, QObject *parent = nullptr);

    QVariant value(const QString &dottedKey, const QVariant &defaultValue = {}) const;
    void setValue(const QString &dottedKey, const QVariant &v);
    bool sync();

    bool load();
    QString filePath() const { return m_filePath; }
    QStringList allKeys() const;

signals:
    void valueChanged(const QString &key, const QVariant &newValue);

private:
    QJsonObject findParent(const QString &dottedKey, QString &lastKey) const;
    QJsonObject findOrCreateParent(const QString &dottedKey, QString &lastKey);

    QString m_filePath;
    QJsonObject m_root;
    mutable QMutex m_mutex;
};

#endif // JSONSETTINGS_H
