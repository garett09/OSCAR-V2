/* SleepLib Apple Health Data Parsing Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "applehealthDataParsing.h"
#include "SleepLib/machine_common.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSet>
#include <QStringView>
#include <QTime>
#include <QtNumeric>
#include <QXmlStreamReader>

#include <utility>

namespace {

enum class RecordKind
{
    Unknown,
    SleepAnalysis,
    HeartRate,
    OxygenSaturation,
    RespiratoryRate,
    HeartRateVariability,
    BreathingDisturbances,
    WristTemperature,
    BodyMass
};

static RecordKind recordKind(QStringView type)
{
    if (type == QStringLiteral("HKCategoryTypeIdentifierSleepAnalysis")) {
        return RecordKind::SleepAnalysis;
    }
    if (type == QStringLiteral("HKQuantityTypeIdentifierHeartRate")) {
        return RecordKind::HeartRate;
    }
    if (type == QStringLiteral("HKQuantityTypeIdentifierOxygenSaturation")) {
        return RecordKind::OxygenSaturation;
    }
    if (type == QStringLiteral("HKQuantityTypeIdentifierRespiratoryRate")) {
        return RecordKind::RespiratoryRate;
    }
    if (type == QStringLiteral("HKQuantityTypeIdentifierHeartRateVariabilitySDNN")) {
        return RecordKind::HeartRateVariability;
    }
    if (type == QStringLiteral("HKQuantityTypeIdentifierAppleSleepingBreathingDisturbances")) {
        return RecordKind::BreathingDisturbances;
    }
    if (type == QStringLiteral("HKQuantityTypeIdentifierAppleSleepingWristTemperature")) {
        return RecordKind::WristTemperature;
    }
    if (type == QStringLiteral("HKQuantityTypeIdentifierBodyMass")) {
        return RecordKind::BodyMass;
    }
    return RecordKind::Unknown;
}

static QString shortTypeName(RecordKind kind)
{
    switch (kind) {
    case RecordKind::SleepAnalysis:         return QStringLiteral("SleepAnalysis");
    case RecordKind::HeartRate:             return QStringLiteral("HeartRate");
    case RecordKind::OxygenSaturation:      return QStringLiteral("OxygenSaturation");
    case RecordKind::RespiratoryRate:       return QStringLiteral("RespiratoryRate");
    case RecordKind::HeartRateVariability:  return QStringLiteral("HeartRateVariabilitySDNN");
    case RecordKind::BreathingDisturbances: return QStringLiteral("AppleSleepingBreathingDisturbances");
    case RecordKind::WristTemperature:      return QStringLiteral("AppleSleepingWristTemperature");
    case RecordKind::BodyMass:              return QStringLiteral("BodyMass");
    case RecordKind::Unknown:               break;
    }
    return QString();
}

static qint64 parseAppleDate(QStringView text)
{
    if (text.size() != 25
        || text.at(4) != QLatin1Char('-') || text.at(7) != QLatin1Char('-')
        || text.at(10) != QLatin1Char(' ') || text.at(13) != QLatin1Char(':')
        || text.at(16) != QLatin1Char(':') || text.at(19) != QLatin1Char(' ')
        || (text.at(20) != QLatin1Char('+') && text.at(20) != QLatin1Char('-'))) {
        return -1;
    }

    bool ok = true;
    const auto number = [&text, &ok](int offset, int length) {
        int value = 0;
        for (int i = 0; i < length; ++i) {
            const ushort digit = text.at(offset + i).unicode();
            if (digit < '0' || digit > '9') {
                ok = false;
                return 0;
            }
            value = value * 10 + digit - '0';
        }
        return value;
    };

    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    const int hour = number(11, 2);
    const int minute = number(14, 2);
    const int second = number(17, 2);
    const int offsetHour = number(21, 2);
    const int offsetMinute = number(23, 2);
    if (!ok || offsetHour > 23 || offsetMinute > 59) {
        return -1;
    }

    const QDate date(year, month, day);
    const QTime time(hour, minute, second);
    if (!date.isValid() || !time.isValid()) {
        return -1;
    }

    qint64 offsetMs = (offsetHour * 60LL + offsetMinute) * 60LL * 1000LL;
    if (text.at(20) == QLatin1Char('-')) {
        offsetMs = -offsetMs;
    }
    return QDateTime(date, time, Qt::UTC).toMSecsSinceEpoch() - offsetMs;
}

} // namespace

bool isAppleWatchSleepSource(const QString &sourceName)
{
    QString normalized = sourceName;
    normalized.replace(QChar(0x00A0), QLatin1Char(' '));
    return normalized.contains(QStringLiteral("Apple"))
           && normalized.contains(QStringLiteral("Watch"));
}

QString autoMatchedSleepSource(const QHash<QString, int> &sourceCounts)
{
    QString matchedSource;
    int matchedCount = -1;
    for (auto it = sourceCounts.cbegin(); it != sourceCounts.cend(); ++it) {
        if (!isAppleWatchSleepSource(it.key())) {
            continue;
        }
        if (it.value() > matchedCount
            || (it.value() == matchedCount
                && it.key().compare(matchedSource, Qt::CaseInsensitive) < 0)) {
            matchedSource = it.key();
            matchedCount = it.value();
        }
    }
    if (matchedSource.isEmpty() && sourceCounts.size() == 1) {
        return sourceCounts.cbegin().key();
    }
    return matchedSource;
}

AppleHealthParser::AppleHealthParser() = default;

void AppleHealthParser::setCutoff(qint64 epochMsUtc)
{
    m_cutoffMs = epochMsUtc;
    m_coarseCutoffDate.clear();
    if (m_cutoffMs != 0) {
        m_coarseCutoffDate = QDateTime::fromMSecsSinceEpoch(m_cutoffMs, Qt::UTC)
                                 .date().addDays(-2).toString(Qt::ISODate);
    }
}

void AppleHealthParser::setProgressCallback(
    std::function<void(qint64 bytesRead, qint64 bytesTotal)> cb)
{
    m_progressCallback = std::move(cb);
}

QString AppleHealthParser::errorString() const
{
    return m_error;
}

bool AppleHealthParser::parse(const QString &path, AppleHealthData &out)
{
    out = AppleHealthData();
    m_error.clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = QStringLiteral("Could not open %1: %2").arg(path, file.errorString());
        return false;
    }

    const qint64 bytesTotal = file.size();
    qint64 lastProgressBytes = 0;
    if (m_progressCallback) {
        m_progressCallback(0, bytesTotal);
    }
    const auto reportProgress = [&]() {
        static constexpr qint64 progressInterval = 2LL * 1024LL * 1024LL;
        const qint64 bytesRead = file.pos();
        if (m_progressCallback && bytesRead - lastProgressBytes >= progressInterval) {
            lastProgressBytes = bytesRead;
            m_progressCallback(bytesRead, bytesTotal);
        }
    };

    QXmlStreamReader xml(&file);
    QSet<QString> unknownSleepValues;
    int malformedWarnings = 0;
    const auto warnMalformed = [&malformedWarnings](const QString &message) {
        if (malformedWarnings < 10) {
            qWarning().noquote() << "AppleHealthParser:" << message;
        }
        ++malformedWarnings;
    };

    const auto parseRecord = [&](const QXmlStreamAttributes &attributes) {
        ++out.recordsSeen;
        if ((out.recordsSeen % 4096) == 0) {
            reportProgress();
        }

        const QStringView fullType = attributes.value(QStringLiteral("type"));
        const RecordKind kind = recordKind(fullType);
        if (kind == RecordKind::Unknown) {
            return;
        }

        const QStringView endText = attributes.value(QStringLiteral("endDate"));
        if (m_cutoffMs != 0 && !m_coarseCutoffDate.isEmpty()) {
            if (endText.size() < 10) {
                warnMalformed(QStringLiteral("skipping %1 with malformed endDate")
                                  .arg(shortTypeName(kind)));
                return;
            }
            if (endText.left(10).compare(QStringView(m_coarseCutoffDate)) < 0) {
                return;
            }
        }

        const qint64 endMs = parseAppleDate(endText);
        if (endMs == -1) {
            warnMalformed(QStringLiteral("skipping %1 with malformed endDate")
                              .arg(shortTypeName(kind)));
            return;
        }
        if (m_cutoffMs != 0 && endMs < m_cutoffMs) {
            return;
        }

        if (kind == RecordKind::SleepAnalysis) {
            const QString sourceName = attributes.value(QStringLiteral("sourceName")).toString();
            // Sleep source totals include every SleepAnalysis record that passes the cutoff.
            ++out.sleepSourceCounts[sourceName];

            const QStringView value = attributes.value(QStringLiteral("value"));
            SleepStageValue stage = Stage_None;
            if (value.endsWith(QStringLiteral("Awake"))) {
                stage = Stage_Awake;
            } else if (value.endsWith(QStringLiteral("AsleepREM"))) {
                stage = Stage_REM;
            } else if (value.endsWith(QStringLiteral("AsleepCore"))
                       || value.endsWith(QStringLiteral("AsleepUnspecified"))) {
                stage = Stage_Light;
            } else if (value.endsWith(QStringLiteral("AsleepDeep"))) {
                stage = Stage_Deep;
            } else if (value.endsWith(QStringLiteral("Asleep"))) {
                // Pre-iOS 16 exports use plain "Asleep" with no stage breakdown.
                stage = Stage_Light;
            } else if (value.endsWith(QStringLiteral("InBed"))) {
                return;
            } else {
                const QString unknownValue = value.toString();
                if (!unknownSleepValues.contains(unknownValue)) {
                    unknownSleepValues.insert(unknownValue);
                    qWarning() << "AppleHealthParser: unknown SleepAnalysis value" << unknownValue;
                }
                return;
            }

            const qint64 startMs = parseAppleDate(attributes.value(QStringLiteral("startDate")));
            if (startMs == -1) {
                warnMalformed(QStringLiteral("skipping SleepAnalysis with malformed startDate"));
                return;
            }
            out.sleepStages.append(AppleHealthInterval{startMs, endMs, stage, sourceName});
            ++out.typeCounts[shortTypeName(kind)];
            return;
        }

        const qint64 startMs = parseAppleDate(attributes.value(QStringLiteral("startDate")));
        if (startMs == -1) {
            warnMalformed(QStringLiteral("skipping %1 with malformed startDate")
                              .arg(shortTypeName(kind)));
            return;
        }

        const QStringView valueText = attributes.value(QStringLiteral("value"));
        bool ok = false;
        switch (kind) {
        case RecordKind::HeartRate: {
            const float value = valueText.toFloat(&ok);
            if (ok) {
                if (!qIsFinite(value) || value <= 0.0f || value > 3000.0f) {
                    warnMalformed(QStringLiteral("skipping HeartRate with out-of-range value %1")
                                      .arg(valueText.toString()));
                    return;
                }
                out.heartRate.append(AppleHealthSample{startMs, value});
            }
            break;
        }
        case RecordKind::OxygenSaturation: {
            float value = valueText.toFloat(&ok);
            if (ok) {
                const QStringView unit = attributes.value(QStringLiteral("unit"));
                if (unit != QStringLiteral("%")) {
                    warnMalformed(QStringLiteral("skipping OxygenSaturation with unknown unit %1")
                                      .arg(unit.toString()));
                    return;
                }
                // Apple writes fractions (0.96) under unit "%"; third-party apps may write 96.
                if (value >= 0.0f && value <= 1.0f) {
                    value *= 100.0f;
                }
                // 0 and negatives are no-reading sentinels, not measurements.
                if (!qIsFinite(value) || value <= 0.0f || value > 100.0f) {
                    warnMalformed(QStringLiteral("skipping OxygenSaturation with out-of-range value %1")
                                      .arg(valueText.toString()));
                    return;
                }
                out.spo2.append(AppleHealthSample{startMs, value});
            }
            break;
        }
        case RecordKind::RespiratoryRate: {
            const float value = valueText.toFloat(&ok);
            if (ok) {
                if (!qIsFinite(value) || value <= 0.0f || value > 3000.0f) {
                    warnMalformed(QStringLiteral("skipping RespiratoryRate with out-of-range value %1")
                                      .arg(valueText.toString()));
                    return;
                }
                out.respRate.append(AppleHealthSample{startMs, value});
            }
            break;
        }
        case RecordKind::HeartRateVariability: {
            const float value = valueText.toFloat(&ok);
            if (ok) {
                if (!qIsFinite(value) || value <= 0.0f || value > 3000.0f) {
                    warnMalformed(QStringLiteral("skipping HeartRateVariabilitySDNN with out-of-range value %1")
                                      .arg(valueText.toString()));
                    return;
                }
                out.hrv.append(AppleHealthSample{startMs, value});
            }
            break;
        }
        case RecordKind::BreathingDisturbances: {
            const double value = valueText.toDouble(&ok);
            if (ok) {
                if (!qIsFinite(value) || value < 0.0) {
                    warnMalformed(QStringLiteral("skipping AppleSleepingBreathingDisturbances with out-of-range value %1")
                                      .arg(valueText.toString()));
                    return;
                }
                out.breathingDisturbances.append(AppleHealthNightScalar{startMs, endMs, value});
            }
            break;
        }
        case RecordKind::WristTemperature: {
            double value = valueText.toDouble(&ok);
            if (ok) {
                const QStringView unit = attributes.value(QStringLiteral("unit"));
                if (unit == QStringLiteral("degF")) {
                    value = (value - 32.0) * 5.0 / 9.0;
                } else if (unit == QStringLiteral("K")) {
                    value -= 273.15;
                } else if (unit != QStringLiteral("degC")) {
                    warnMalformed(QStringLiteral("skipping AppleSleepingWristTemperature with unknown unit %1")
                                      .arg(unit.toString()));
                    return;
                }
                if (!qIsFinite(value)) {
                    warnMalformed(QStringLiteral("skipping AppleSleepingWristTemperature with out-of-range value %1")
                                      .arg(valueText.toString()));
                    return;
                }
                out.wristTemp.append(AppleHealthNightScalar{startMs, endMs, value});
            }
            break;
        }
        case RecordKind::BodyMass: {
            double value = valueText.toDouble(&ok);
            if (ok) {
                const QStringView unit = attributes.value(QStringLiteral("unit"));
                if (unit == QStringLiteral("lb")) {
                    value *= 0.45359237;
                } else if (unit == QStringLiteral("st")) {
                    value *= 6.35029318;
                } else if (unit == QStringLiteral("g")) {
                    value /= 1000.0;
                } else if (unit == QStringLiteral("oz")) {
                    value *= 0.028349523125;
                } else if (unit != QStringLiteral("kg")) {
                    warnMalformed(QStringLiteral("skipping BodyMass with unknown unit %1")
                                      .arg(unit.toString()));
                    return;
                }
                if (!qIsFinite(value) || value <= 0.0 || value > 500.0) {
                    warnMalformed(QStringLiteral("skipping BodyMass with out-of-range value %1")
                                      .arg(valueText.toString()));
                    return;
                }
                out.weights.append(AppleHealthWeight{startMs, value});
            }
            break;
        }
        case RecordKind::Unknown:
        case RecordKind::SleepAnalysis:
            break;
        }

        if (!ok) {
            warnMalformed(QStringLiteral("skipping %1 with malformed value")
                              .arg(shortTypeName(kind)));
            return;
        }
        ++out.typeCounts[shortTypeName(kind)];
    };

    if (!xml.readNextStartElement()) {
        if (xml.hasError()) {
            m_error = xml.errorString();
        } else {
            m_error = QStringLiteral("XML contains no root element");
        }
        return false;
    }

    if (xml.name() == QStringLiteral("HealthData")) {
        while (xml.readNextStartElement()) {
            if (xml.name() != QStringLiteral("Record")) {
                xml.skipCurrentElement();
                continue;
            }
            parseRecord(xml.attributes());
            xml.skipCurrentElement();
        }
    } else {
        m_error = QStringLiteral("Root element is %1, not HealthData").arg(xml.name());
        return false;
    }

    while (!xml.atEnd()) {
        xml.readNext();
    }
    if (xml.hasError()) {
        m_error = QStringLiteral("XML error at line %1, column %2: %3")
                      .arg(xml.lineNumber()).arg(xml.columnNumber()).arg(xml.errorString());
        // Keep records parsed before malformed XML available to the caller.
        return false;
    }
    if (file.error() != QFileDevice::NoError) {
        m_error = QStringLiteral("Error reading %1: %2").arg(path, file.errorString());
        return false;
    }
    if (m_progressCallback && lastProgressBytes < bytesTotal) {
        m_progressCallback(bytesTotal, bytesTotal);
    }
    return true;
}
