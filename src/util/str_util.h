#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <qchar.h>

namespace StrUtil
{
  inline bool isBlank(QString str)
  {
    if (str.isEmpty())
    {
      return true;
    }
    for (int i = 0; i < str.length(); i++)
    {
      if (str.at(i) != ' ')
      {
        return false;
      }
    }
    return true;
  }
  inline QString sub(QString str, int fromIndex, int toIndex)
  {
    if (isBlank(str))
    {
      return QString();
    }
    else
    {
      int len = str.length();
      if (fromIndex < 0)
      {
        fromIndex += len;
        if (fromIndex < 0)
        {
          fromIndex = 0;
        }
      }
      else if (fromIndex > len)
      {
        fromIndex = len;
      }

      if (toIndex < 0)
      {
        toIndex += len;
        if (toIndex < 0)
        {
          toIndex = len;
        }
      }
      else if (toIndex > len)
      {
        toIndex = len;
      }

      if (toIndex < fromIndex)
      {
        int tmp = fromIndex;
        fromIndex = toIndex;
        toIndex = tmp;
      }
      if (fromIndex == toIndex)
      {
        return QString();
      }
      else
      {
        QString res = str.mid(fromIndex, toIndex - fromIndex);
        return res;
      }
    }
  }
  inline QString subBetween(QString str, QString before, QString after)
  {
    if (isBlank(str) || isBlank(before) || isBlank(after))
    {
      return QString();
    }
    int start = str.indexOf(before);
    if (start != -1)
    {
      int end = str.indexOf(after, start + before.length());
      if (end != -1)
      {
        int subStart = start + before.length();
        QString res = str.mid(subStart, end - subStart);
        return res;
      }
    }
    return QString();
  }
  inline QString subBefore(QString str, QString before)
  {
    if (isBlank(str) || isBlank(before))
    {
      return QString();
    }
    int end = str.indexOf(before);
    if (end != -1)
    {
      QString res = str.mid(0, end);
      return res;
    }
    return QString();
  }

  /* 去除重复结尾块 */
  inline QString removeRepeatedTrailingBlock(const QString &s)
  {
    const int n = s.size();
    if (n <= 50)
      return s;

    int bestPeriod = 0;
    int maxRepeatCount = 0;

    /* 检测重复的结尾块，最长不超过n/2 */
    for (int period = 1; period <= n / 2; ++period)
    {
      int repeatCount = 0;
      while (repeatCount * period + period <= n)
      {
        QString block1 = s.mid(n - (repeatCount + 1) * period, period);
        QString block2 = s.mid(n - (repeatCount + 2) * period, period);
        if (block1 == block2)
        {
          ++repeatCount;
        }
        else
        {
          break;
        }
      }
      if (repeatCount > maxRepeatCount)
      {
        maxRepeatCount = repeatCount;
        bestPeriod = period;
      }
    }

    QString result = s;
    if (maxRepeatCount >= 2)
    {
      result = s.left(n - maxRepeatCount * bestPeriod);
    }
    while (result.contains("\n\n"))
    {
      result = result.replace("\n\n", "\n");
    }
    while (result.contains("\r\r"))
    {
      result = result.replace("\r\r", "\r");
    }
    return result;
  }

  /* 去除json所有字段的重复结尾块 */
  inline QJsonValue sanitizeJsonValue(const QJsonValue &v)
  {
    if (v.isString())
    {
      const QString s = v.toString();
      return QJsonValue(removeRepeatedTrailingBlock(s));
    }
    else if (v.isArray())
    {
      const QJsonArray arr = v.toArray();
      QJsonArray out;
      for (const auto &item : arr)
      {
        out.append(sanitizeJsonValue(item));
      }
      return out;
    }
    else if (v.isObject())
    {
      const QJsonObject o = v.toObject();
      QJsonObject out;
      QStringList keys = o.keys();
      for (const QString &key : keys)
      {
        out.insert(key, sanitizeJsonValue(o.value(key)));
      }
      return out;
    }
    return v;
  }
}; /* namespace StrUtil */