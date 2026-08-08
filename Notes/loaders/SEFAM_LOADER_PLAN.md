# SEFAM Loader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a SEFAM CPAP loader to OSCAR that imports flow, pressure, total leak, respiratory events and therapy settings from SEFAM SD cards.

**Architecture:** Two units. `sefamDataParsing` decodes the card format into plain structs and contains no OSCAR types, so it can be verified independently. `sefam_loader` maps those structs onto OSCAR `Machine`/`Session`/`EventList` objects. This mirrors `bmc_loader.cpp` / `bmcDataParsing.cpp`.

**Tech Stack:** C++17, Qt 6.10.2, QtCreator incremental builds.

**Design spec:** `Notes/loaders/SEFAM_LOADER_DESIGN.md`
**Format reference:** `Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md`

## Global Constraints

- C++17; must compile clean under Qt 6.10.2.
- Qt6-only idioms. No `#if QT_VERSION` guards.
- No platform-specific APIs — the build targets ~25 environments.
- Every new `.h` and `.cpp` carries a brief header description and `Copyright (c) 2026 The OSCAR Team`.
- Doxygen-style documentation on all new files, written for a programmer reading the code.
- New files must be added to `oscar/oscar.pro` (`SOURCES` and `HEADERS`).
- Never modify anything under `oscar/SleepLib/thirdparty`.
- **This loader must not change the behaviour of any other loader.** No edits to shared files beyond the additive `oscar.pro` entries and the `main.cpp` registration line.
- No unit tests — the QtTest harness is not used in this workflow. Each task ends with a manual verification step run in QtCreator.
- Log non-trivial fixes to `Notes/Developer Notes/BUG_FIXES.md` (applies to defects found during implementation, not to the initial feature work).

## Build note

QtCreator's incremental build was observed **silently going stale** during this
work — new `qDebug` output from an edited `.cpp` simply did not appear, with no
error. If a verification step shows output from the *previous* task but not the
current one, do a clean rebuild before investigating the code. This cost one
debugging cycle on Task 2.

## Verification data

The sample card and its manufacturer report live outside the repo:

- Card: `C:/OSCAR/TestFiles/Sefam_Sanrai_Reve_Skeletal8663/SD-Card-Root-Directory`
- Report: `C:/OSCAR/TestFiles/Sefam_Analyze_Reports_Skeletal8663/Sefam_Analyze_Reports_Skeletal8663_Apneaboard`

Golden numbers, already confirmed against the manufacturer's analyzer:

| Check | Expected |
|---|---|
| Sessions | 31 (`DATA_031` skipped — log only, no `.INI`) |
| `DATA_001` records | 3075 |
| Checksum failures | 0 |
| Total recorded time | 159.0 h |
| Events (as imported) | OA 243, CA 190, Hypopnea 306, Snore 1402, FL 955 |
| Apnea mean duration | OA 16.6 s, CA 12.8 s |
| Settings | min 4.0, max 20.0, ramp 45 min, ramp pressure 4.0 |
| AHI for the period | ≈ 4.7 |

## File Structure

| File | Responsibility |
|---|---|
| `oscar/SleepLib/loader_plugins/sefamDataParsing.h` | Structs and free-function declarations for the decode layer. |
| `oscar/SleepLib/loader_plugins/sefamDataParsing.cpp` | XOR descramble, header parse, `.INI` parse, channel record reader, `.LOG` reader, settings decode. |
| `oscar/SleepLib/loader_plugins/sefam_loader.h` | `SefamLoader : public CPAPLoader` declaration. |
| `oscar/SleepLib/loader_plugins/sefam_loader.cpp` | Detect, Open, machine/session creation, channel and event mapping, backup. |
| `oscar/oscar.pro` | Add the four files (additive only). |
| `oscar/main.cpp` | Add include and `SefamLoader::Register();` (additive only). |

---

### Task 1: Skeleton, registration and Detect

Gets a compiling, registered loader that recognises a SEFAM card. Everything after this is observable by running OSCAR, which is why registration comes first rather than last.

**Files:**
- Create: `oscar/SleepLib/loader_plugins/sefamDataParsing.h`
- Create: `oscar/SleepLib/loader_plugins/sefamDataParsing.cpp`
- Create: `oscar/SleepLib/loader_plugins/sefam_loader.h`
- Create: `oscar/SleepLib/loader_plugins/sefam_loader.cpp`
- Modify: `oscar/oscar.pro`
- Modify: `oscar/main.cpp`

**Interfaces:**
- Consumes: `CPAPLoader`, `RegisterLoader()`, `MachineInfo` from `SleepLib/machine_loader.h`.
- Produces:
  - `SefamParsing::descramble(QByteArray &)`
  - `SefamParsing::FileHeader` (fields `serial`, `localStart`, `utcEpoch`, `length`)
  - `SefamParsing::parseHeader(const QByteArray &, FileHeader &) -> bool`
  - `SefamLoader::Detect(const QString &) -> bool`
  - `SefamLoader::PeekInfo(const QString &) -> MachineInfo`, which sets `serial`,
    `model`/`modelnumber`, and the properties `"ModelCode"` and `"Firmware"`.
    Task 3 reads `properties["ModelCode"]` for the untested-device check.
  - `SefamLoader::findSerialDir(const QString &) -> QString`
  - Constants `sefam_data_version`, `sefam_class_name`, `sefam_validated_model`

- [ ] **Step 1: Create `sefamDataParsing.h` with the header-parsing surface**

```cpp
/* SEFAM SD Card Data Parsing Header
 *
 * Decodes the SEFAM CPAP SD card container: an XOR-0xBF obfuscated fixed-length
 * text header followed by a plaintext body of 10-second sample records, each
 * carrying a checksum and sequence number, plus 49-byte .LOG event records.
 *
 * Note that the obfuscation covers the HEADER ONLY. Sample and log bodies are
 * stored in the clear and their checksums are computed over the bytes as
 * stored, so descrambling a body corrupts it and fails every checksum.
 *
 * This module deliberately contains no OSCAR types. The card format is fully
 * specified in Notes/loaders/SEFAM_REVE_CARD_ANALYSIS.md and validated against
 * the manufacturer's analyzer output; keeping the decode layer free of SleepLib
 * dependencies lets it be reasoned about on its own.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef SEFAM_DATA_PARSING_H
#define SEFAM_DATA_PARSING_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

/*! \namespace SefamParsing
    \brief Pure decoding of the SEFAM SD card format, free of OSCAR types. */
namespace SefamParsing {

//! Every .LOG and channel data file on the card is XORed with this constant.
constexpr quint8 kObfuscationKey = 0xBF;

//! Header length for channel data files: "#03/" + 20 serial + "/" + 12 date + "/" + 32 hex + "/".
constexpr int kChannelHeaderLength = 71;

//! Header length for .LOG files: as above but without the 32-hex field.
constexpr int kLogHeaderLength = 38;

//! Each sample record holds exactly this many seconds of data.
constexpr int kRecordSeconds = 10;

//! Bytes following the samples in each record: 1 checksum + 2 big-endian sequence.
constexpr int kRecordTrailerBytes = 3;

//! Sample value meaning "no valid data" on FLW, PRE and LK.
constexpr quint8 kInvalidSample = 0xFF;

/*! \struct FileHeader
    \brief The decoded fixed-length text header at the start of every card file. */
struct FileHeader
{
    QString   serial;                  //!< Full serial, pad spaces trimmed.
    QDateTime localStart;              //!< Device local wall-clock start time.
    qint64    utcEpoch = 0;            //!< UTC epoch seconds; 0 when the short header is used.
    int       length   = 0;            //!< 71 or 38, whichever variant was detected.
};

/*! \brief XOR every byte with kObfuscationKey, in place. Self-inverse. */
void descramble(QByteArray &data);

/*! \brief Parse a descrambled file header.
    \param decoded Descrambled bytes; at least kChannelHeaderLength are examined.
    \param out     Populated on success.
    \return true if the header carries the "#03/" tag and a parseable timestamp.

    The header length is detected, never assumed: after the 12-character
    timestamp and its delimiter, 32 hex digits followed by '/' indicate the
    71-byte channel variant; anything else means the 38-byte .LOG variant.
    Do not locate the end of the header by scanning for the fourth '/' — a
    payload byte can descramble to '/' and shift the parse. */
bool parseHeader(const QByteArray &decoded, FileHeader &out);

}  // namespace SefamParsing

#endif  // SEFAM_DATA_PARSING_H
```

- [ ] **Step 2: Create `sefamDataParsing.cpp` implementing descramble and parseHeader**

```cpp
/* SEFAM SD Card Data Parsing Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "sefamDataParsing.h"

#include <QRegularExpression>

namespace SefamParsing {

void descramble(QByteArray &data)
{
    for (int i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(static_cast<quint8>(data.at(i)) ^ kObfuscationKey);
    }
}

bool parseHeader(const QByteArray &decoded, FileHeader &out)
{
    // Minimum viable header: "#03/" + 20 + "/" + 12 + "/"
    if (decoded.size() < kLogHeaderLength) { return false; }
    if (!decoded.startsWith("#03/")) { return false; }

    const int serialStart = 4;
    const int serialLen   = 20;
    const int stampStart  = serialStart + serialLen + 1;   // skip the '/' after the serial
    const int stampLen    = 12;

    if (decoded.at(serialStart + serialLen) != '/') { return false; }
    if (decoded.size() < stampStart + stampLen + 1) { return false; }
    if (decoded.at(stampStart + stampLen) != '/') { return false; }

    out.serial = QString::fromLatin1(decoded.mid(serialStart, serialLen)).trimmed();

    const QString stamp = QString::fromLatin1(decoded.mid(stampStart, stampLen));
    out.localStart = QDateTime::fromString(stamp, "yyMMddHHmmss");
    if (!out.localStart.isValid()) { return false; }
    // QDateTime maps a two-digit year to 1900+yy; the card is post-2000.
    if (out.localStart.date().year() < 2000) {
        out.localStart = out.localStart.addYears(100);
    }

    // Detect the variant: 32 hex digits then '/' means the long channel header.
    const int hexStart = stampStart + stampLen + 1;
    const int hexLen   = 32;
    out.utcEpoch = 0;
    out.length   = kLogHeaderLength;

    if (decoded.size() >= hexStart + hexLen + 1 && decoded.at(hexStart + hexLen) == '/') {
        const QByteArray hex = decoded.mid(hexStart, hexLen);
        static const QRegularExpression hexOnly("\\A[0-9A-Fa-f]{32}\\z");
        if (hexOnly.match(QString::fromLatin1(hex)).hasMatch()) {
            bool ok = false;
            const qint64 epoch = hex.left(16).toLongLong(&ok, 16);
            if (ok) {
                out.utcEpoch = epoch;
                out.length   = kChannelHeaderLength;
            }
        }
    }
    return true;
}

}  // namespace SefamParsing
```

- [ ] **Step 3: Create `sefam_loader.h`**

```cpp
/* SEFAM Loader Header
 *
 * Imports SEFAM CPAP SD cards. The card layout is
 * <modelcode>/<serial>/DATA_nnn/, one directory per session.
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#ifndef SEFAM_LOADER_H
#define SEFAM_LOADER_H

#include "SleepLib/machine.h"
#include "SleepLib/machine_loader.h"

//! Bump when a format change requires reimporting existing SEFAM data.
const int sefam_data_version = 1;

const QString sefam_class_name = "SefamLoader";

//! The one model validated end-to-end against a manufacturer report.
const QString sefam_validated_model = "1279R";

/*! \class SefamLoader
    \brief Imports SEFAM CPAP SD cards.

    Any card matching <digits><letter>/<digits>/DATA_nnn/ is accepted. Sample
    rates and channel names come from each session's .INI, and header length is
    detected rather than assumed, so models other than the validated one import
    on a best-effort basis and raise deviceIsUntested(). */
class SefamLoader : public CPAPLoader
{
    Q_OBJECT
  public:
    SefamLoader();
    virtual ~SefamLoader();

    static void Register();

    virtual bool Detect(const QString &path) override;
    virtual int  Open(const QString &path) override;
    virtual int  Version() override { return sefam_data_version; }
    virtual const QString &loaderName() override { return sefam_class_name; }
    virtual MachineInfo PeekInfo(const QString &path) override;

    virtual MachineInfo newInfo() override {
        return MachineInfo(MT_CPAP, 0, sefam_class_name, QObject::tr("Sefam"),
                           QString(), QString(), QString(), QObject::tr("Sefam"),
                           QDateTime::currentDateTime(), sefam_data_version);
    }

    //! Copy the card to the machine's backup folder, honouring the user preference.
    bool backupData(Machine *mach, const QString &path);

  protected:
    bool rebuild_from_backups = false;
    bool create_backups = true;

    /*! \brief Locate the <modelcode>/<serial> directory beneath a card root.
        \return Absolute path, or an empty string if the tree does not match. */
    QString findSerialDir(const QString &path);
};

#endif  // SEFAM_LOADER_H
```

- [ ] **Step 4: Create `sefam_loader.cpp` with Register, Detect and a stub Open**

```cpp
/* SEFAM Loader Implementation
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "sefam_loader.h"
#include "sefamDataParsing.h"

#include "SleepLib/importcontext.h"
#include "SleepLib/session.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>

static bool sefam_initialised = false;

SefamLoader::SefamLoader()  { m_type = MT_CPAP; }
SefamLoader::~SefamLoader() = default;

void SefamLoader::Register()
{
    if (sefam_initialised) { return; }
    qDebug() << "Registering SefamLoader";
    RegisterLoader(new SefamLoader());
    sefam_initialised = true;
}

QString SefamLoader::findSerialDir(const QString &path)
{
    // Card layout: <root>/<digits><letter>/<digits>/DATA_nnn/
    // Accept the caller pointing at the root, the model directory, or the
    // serial directory itself, so import works from any of the three.
    static const QRegularExpression modelRe("\\A\\d+[A-Za-z]\\z");
    static const QRegularExpression serialRe("\\A\\d+\\z");

    QDir dir(path);
    if (!dir.exists()) { return QString(); }

    // Already the serial directory?
    if (!dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
        return dir.absolutePath();
    }

    const QStringList level1 = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &a : level1) {
        QDir sub(dir.absoluteFilePath(a));
        const bool aIsModel = modelRe.match(a).hasMatch();

        // <root>/<model>/<serial>/DATA_nnn
        if (aIsModel) {
            const QStringList level2 = sub.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &b : level2) {
                if (!serialRe.match(b).hasMatch()) { continue; }
                QDir leaf(sub.absoluteFilePath(b));
                if (!leaf.entryList(QStringList("DATA_*"),
                                    QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
                    return leaf.absolutePath();
                }
            }
        }
        // <model>/<serial>/DATA_nnn — caller pointed at the model directory
        if (serialRe.match(a).hasMatch()
            && !sub.entryList(QStringList("DATA_*"),
                              QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            return sub.absolutePath();
        }
    }
    return QString();
}

bool SefamLoader::Detect(const QString &path)
{
    const QString serialDir = findSerialDir(path);
    if (serialDir.isEmpty()) { return false; }

    QDir dir(serialDir);
    const QStringList sessions =
        dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    // Confirm the "#03/" tag in at least one channel header. A directory tree of
    // the right shape from another vendor must not produce a false positive.
    for (const QString &s : sessions) {
        QDir sess(dir.absoluteFilePath(s));
        if (!QFile::exists(sess.absoluteFilePath(s + ".INI"))) { continue; }

        QFile f(sess.absoluteFilePath(s + ".FLW"));
        if (!f.open(QIODevice::ReadOnly)) { continue; }
        QByteArray head = f.read(SefamParsing::kChannelHeaderLength);
        f.close();

        SefamParsing::descramble(head);
        SefamParsing::FileHeader hdr;
        if (SefamParsing::parseHeader(head, hdr)) {
            qDebug() << "SefamLoader::Detect matched" << serialDir
                     << "serial" << hdr.serial;
            return true;
        }
    }
    return false;
}

/*! \brief Turn an .INI "Created By" value into a display model name.
    \return e.g. "REVE_AUTO " -> "Rêve Auto"; empty if unrecognised.

    Only names actually observed on a card are translated. Anything else falls
    back to the raw trimmed value so an unknown model still shows something
    meaningful rather than a blank. */
static QString sefamModelName(const QString &createdBy)
{
    const QString key = createdBy.trimmed().toUpper();
    if (key.isEmpty())          { return QString(); }
    if (key == "REVE_AUTO")     { return QString::fromUtf8("Rêve Auto"); }
    if (key == "SBOX_AUTO")     { return QStringLiteral("S.Box Auto"); }
    return createdBy.trimmed();
}

MachineInfo SefamLoader::PeekInfo(const QString &path)
{
    MachineInfo info = newInfo();
    const QString serialDir = findSerialDir(path);
    if (serialDir.isEmpty()) { return info; }

    QDir dir(serialDir);

    // The hardware model code (e.g. "1279R") is the parent directory name. It is
    // kept as a property rather than in `series`, because `series` drives the
    // device-pixmap lookup in MachineLoader::getPixmap() and must stay "Sefam".
    const QString modelCode = QFileInfo(dir.absolutePath()).dir().dirName();
    info.properties[QStringLiteral("ModelCode")] = modelCode;

    const QStringList sessions =
        dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &s : sessions) {
        QDir sess(dir.absoluteFilePath(s));
        QFile f(sess.absoluteFilePath(s + ".FLW"));
        if (!f.open(QIODevice::ReadOnly)) { continue; }
        QByteArray head = f.read(SefamParsing::kChannelHeaderLength);
        f.close();

        SefamParsing::descramble(head);
        SefamParsing::FileHeader hdr;
        if (!SefamParsing::parseHeader(head, hdr)) { continue; }
        info.serial = hdr.serial;

        // Model name comes from the .INI, which is plain text and not obfuscated.
        QSettings ini(sess.absoluteFilePath(s + ".INI"), QSettings::IniFormat);
        const QString model = sefamModelName(ini.value("Create Info/Created By").toString());
        info.modelnumber = model.isEmpty() ? modelCode : model;
        info.model       = info.modelnumber;

        // Firmware version, for diagnostics on untested models.
        const QString fw = ini.value("Create Info/Version").toString().trimmed();
        if (!fw.isEmpty()) { info.properties[QStringLiteral("Firmware")] = fw; }
        break;
    }
    if (info.modelnumber.isEmpty()) {
        info.modelnumber = modelCode;
        info.model       = modelCode;
    }
    return info;
}

int SefamLoader::Open(const QString &path)
{
    Q_ASSERT(m_ctx);
    if (!Detect(path)) { return -1; }
    qDebug() << "SefamLoader::Open stub — detection succeeded for" << path;
    return 0;
}

bool SefamLoader::backupData(Machine *mach, const QString &path)
{
    Q_UNUSED(mach); Q_UNUSED(path);
    return true;
}
```

- [ ] **Step 5: Add the four files to `oscar.pro`**

In `SOURCES`, after the `resvent_loader.cpp` line (keeping alphabetical order):

```
    SleepLib/loader_plugins/sefamDataParsing.cpp \
    SleepLib/loader_plugins/sefam_loader.cpp \
```

In `HEADERS`, after the `resvent_loader.h` line:

```
    SleepLib/loader_plugins/sefamDataParsing.h \
    SleepLib/loader_plugins/sefam_loader.h \
```

- [ ] **Step 6: Register the loader in `main.cpp`**

Add after the `yuwell_loader.h` include (around line 70):

```cpp
#include "SleepLib/loader_plugins/sefam_loader.h"
```

Add after the `YuwellLoader::Register();` line (around line 1019):

```cpp
    SefamLoader::Register();
```

- [ ] **Step 7: Build and verify detection**

Build in QtCreator. Then run OSCAR and use Import (Shift+F2), pointing at
`C:/OSCAR/TestFiles/Sefam_Sanrai_Reve_Skeletal8663/SD-Card-Root-Directory`.

Expected in the debug log:
```
Registering SefamLoader
SefamLoader::Detect matched .../1279R/<serial> serial "1279R<serial>"
SefamLoader::Open stub — brand "Sefam" model "Rêve Auto" serial "1279R<serial>"
                          modelcode "1279R" firmware "VER :A010500"
```

Model must read **Rêve Auto** (translated from the `.INI` `Created By` value), not
the raw `REVE_AUTO`. A garbled `ê` means the source file was not saved as UTF-8.

> **Do not expect a machine record in the database yet.** `Open()` is a stub that
> creates nothing; machines and sessions arrive in Task 3. A database containing
> only the `journal` machine is the correct result here.
>
> **Do not expect the model name in the import dialog either.** `PeekInfo()` feeds
> that dialog only on the auto-scan path (`mainwindow.cpp:1415`); browsing to a
> folder manually goes `Detect()` → `Open()` and never calls it. The other call
> site (`mainwindow.cpp:1604`) is gated on the path being a removable drive. That
> is why `Open()` logs the identity itself.

Also confirm no other loader's behaviour changed: import a card for any loader you already have data for and confirm it still imports.

- [ ] **Step 8: Commit**

```bash
git add oscar/SleepLib/loader_plugins/sefamDataParsing.h \
        oscar/SleepLib/loader_plugins/sefamDataParsing.cpp \
        oscar/SleepLib/loader_plugins/sefam_loader.h \
        oscar/SleepLib/loader_plugins/sefam_loader.cpp \
        oscar/oscar.pro oscar/main.cpp
git commit -m "Add SEFAM loader skeleton with card detection"
```

---

### Task 2: Parse the .INI schema and read channel records

Adds the decode layer that turns a session directory into verified sample arrays. Verification is by debug output, since nothing is imported yet.

**Files:**
- Modify: `oscar/SleepLib/loader_plugins/sefamDataParsing.h`
- Modify: `oscar/SleepLib/loader_plugins/sefamDataParsing.cpp`
- Modify: `oscar/SleepLib/loader_plugins/sefam_loader.cpp`

**Interfaces:**
- Consumes: `FileHeader`, `descramble()`, `parseHeader()` from Task 1.
- Produces: `ChannelSpec`, `SessionData`, `parseIni()`, `readChannel()`, `readSession()`.

- [ ] **Step 1: Add the schema and session structures to `sefamDataParsing.h`**

Insert before the closing `}  // namespace SefamParsing`:

```cpp
/*! \struct ChannelSpec
    \brief One [ChanN] block from a session's .INI manifest. */
struct ChannelSpec
{
    QString name;             //!< FLW, PRE, LK, DET, NSD, SPO, ...
    int     freq = 0;         //!< Samples per second.
    int     bits = 8;         //!< Bits per sample; .PLS declares 16.
    int     min  = 0;         //!< Declared physical minimum.
    int     max  = 0;         //!< Declared physical maximum.

    //! Bytes per 10-second record, including the 3-byte trailer.
    int recordBytes() const {
        return freq * kRecordSeconds * (bits / 8) + kRecordTrailerBytes;
    }
};

/*! \struct SessionData
    \brief Everything decoded from one DATA_nnn directory. */
struct SessionData
{
    QString                          dirName;        //!< e.g. "DATA_000"
    FileHeader                       header;
    int                              recordCount = 0;
    QHash<QString, ChannelSpec>      channels;       //!< Declared schema.
    QHash<QString, QVector<quint8>>  samples;        //!< Raw bytes per populated channel.
};

/*! \brief Parse a session's .INI manifest.
    \param path  Absolute path to DATA_nnn.INI.
    \param specs Populated with one entry per declared [ChanN] block.
    \param start Populated from [Start Record] (device local time).
    \return false if the file cannot be read or declares no channels.

    The .INI is plain text and is NOT obfuscated. It is the device's schema, so
    channel names not mapped by this loader are expected, not erroneous. */
bool parseIni(const QString &path, QHash<QString, ChannelSpec> &specs, QDateTime &start);

/*! \brief Read one channel data file, verifying every record.
    \param path     Absolute path to DATA_nnn.<EXT>.
    \param spec     Declared rate and width for this channel.
    \param out      Concatenated sample bytes (8-bit channels only).
    \param records  Number of records accepted.
    \param error    Human-readable reason on failure.
    \return false if the file is unreadable, the header is bad, or the payload
            size is not a whole number of records.

    A record is [samples][1-byte 8-bit sum checksum][2-byte big-endian sequence].
    On a checksum or sequence mismatch the channel is truncated at the last good
    record and true is returned — a card unplugged mid-write leaves a torn final
    record and everything before it is valid. */
bool readChannel(const QString &path, const ChannelSpec &spec,
                 QVector<quint8> &out, int &records, QString &error);

/*! \brief Read one DATA_nnn directory into a SessionData.
    \return false if the .INI is missing or no channel could be read. */
bool readSession(const QString &dirPath, SessionData &out, QString &error);
```

- [ ] **Step 2: Implement parseIni in `sefamDataParsing.cpp`**

Add `#include <QSettings>` to the includes, then append inside the namespace:

```cpp
bool parseIni(const QString &path, QHash<QString, ChannelSpec> &specs, QDateTime &start)
{
    if (!QFile::exists(path)) { return false; }

    QSettings ini(path, QSettings::IniFormat);

    const int year  = ini.value("Start Record/Year").toInt();
    const int month = ini.value("Start Record/Month").toInt();
    const int day   = ini.value("Start Record/Day").toInt();
    const int hour  = ini.value("Start Record/Hour").toInt();
    const int min   = ini.value("Start Record/Min").toInt();
    const int sec   = ini.value("Start Record/Sec").toInt();
    start = QDateTime(QDate(year, month, day), QTime(hour, min, sec));

    // Channels are declared as [Chan0]..[ChanN] with no count field; walk until
    // a group is missing rather than assuming how many a firmware declares.
    for (int i = 0; ; ++i) {
        const QString group = QString("Chan%1").arg(i);
        const QString name  = ini.value(group + "/Name").toString().trimmed();
        if (name.isEmpty()) { break; }

        ChannelSpec spec;
        spec.name = name;
        spec.freq = ini.value(group + "/Freq").toInt();
        spec.bits = ini.value(group + "/Bit").toInt();
        spec.min  = ini.value(group + "/Min").toInt();
        spec.max  = ini.value(group + "/Max").toInt();

        if (spec.freq <= 0 || (spec.bits != 8 && spec.bits != 16)) {
            qWarning() << "Sefam: ignoring channel" << name
                       << "with freq" << spec.freq << "bits" << spec.bits;
            continue;
        }
        specs.insert(spec.name, spec);
    }
    return !specs.isEmpty();
}
```

Add `#include <QDebug>` and `#include <QFile>` to the includes if not already present.

- [ ] **Step 3: Implement readChannel**

```cpp
bool readChannel(const QString &path, const ChannelSpec &spec,
                 QVector<quint8> &out, int &records, QString &error)
{
    records = 0;
    out.clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        error = QString("cannot open %1").arg(path);
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    // Only the header is obfuscated. The sample body is plaintext, and its
    // checksums are computed over the bytes exactly as stored — descrambling it
    // would both corrupt the samples and fail every checksum.
    QByteArray head = raw.left(kChannelHeaderLength);
    descramble(head);

    FileHeader hdr;
    if (!parseHeader(head, hdr)) {
        error = "bad or missing #03/ header";
        return false;
    }

    const int payload = raw.size() - hdr.length;
    if (payload < 0) { error = "file shorter than its header"; return false; }
    if (payload == 0) { return true; }        // header-only stub: channel not recorded

    const int recLen = spec.recordBytes();
    if (recLen <= kRecordTrailerBytes) { error = "invalid record length"; return false; }
    if (payload % recLen != 0) {
        // Catches a wrong assumed sample rate. Must not be silently tolerated.
        error = QString("payload %1 is not a multiple of record size %2")
                    .arg(payload).arg(recLen);
        return false;
    }

    const int total       = payload / recLen;
    const int sampleBytes = recLen - kRecordTrailerBytes;

    // 16-bit channels are sized correctly above so the file is not wrongly
    // rejected, but this loader maps no 16-bit channel to OSCAR.
    if (spec.bits != 8) { records = total; return true; }

    out.reserve(total * sampleBytes);
    for (int i = 0; i < total; ++i) {
        const char *rec = raw.constData() + hdr.length + i * recLen;

        quint32 sum = 0;
        for (int b = 0; b < sampleBytes; ++b) {
            sum += static_cast<quint8>(rec[b]);
        }
        const quint8  checksum = static_cast<quint8>(rec[sampleBytes]);
        const quint16 sequence = (static_cast<quint8>(rec[sampleBytes + 1]) << 8)
                               |  static_cast<quint8>(rec[sampleBytes + 2]);

        if (checksum != static_cast<quint8>(sum & 0xFF)
            || sequence != static_cast<quint16>((i + 1) & 0xFFFF)) {
            qWarning() << "Sefam: truncating" << path << "at record" << i
                       << "of" << total << "(checksum or sequence mismatch)";
            break;
        }
        for (int b = 0; b < sampleBytes; ++b) {
            out.append(static_cast<quint8>(rec[b]));
        }
        ++records;
    }
    return true;
}
```

- [ ] **Step 4: Implement readSession**

```cpp
bool readSession(const QString &dirPath, SessionData &out, QString &error)
{
    QDir dir(dirPath);
    out.dirName = dir.dirName();

    const QString iniPath = dir.absoluteFilePath(out.dirName + ".INI");
    QDateTime iniStart;
    if (!parseIni(iniPath, out.channels, iniStart)) {
        error = "missing or empty .INI";
        return false;
    }

    bool haveHeader = false;
    for (auto it = out.channels.constBegin(); it != out.channels.constEnd(); ++it) {
        const ChannelSpec &spec = it.value();
        const QString path = dir.absoluteFilePath(out.dirName + "." + spec.name);
        if (!QFile::exists(path)) { continue; }

        if (!haveHeader) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray head = f.read(kChannelHeaderLength);
                f.close();
                descramble(head);
                if (parseHeader(head, out.header)) { haveHeader = true; }
            }
        }

        QVector<quint8> samples;
        int records = 0;
        QString chanError;
        if (!readChannel(path, spec, samples, records, chanError)) {
            qWarning() << "Sefam:" << out.dirName << spec.name << "skipped —" << chanError;
            continue;
        }
        if (samples.isEmpty()) { continue; }     // stub file: channel not recorded

        out.samples.insert(spec.name, samples);
        out.recordCount = qMax(out.recordCount, records);
    }

    if (!haveHeader) { error = "no readable channel header"; return false; }
    if (out.samples.isEmpty()) { error = "no populated channels"; return false; }
    return true;
}
```

Add `#include <QDir>` to the includes.

- [ ] **Step 5: Call readSession from Open and dump the results**

Replace the body of `SefamLoader::Open` with:

```cpp
int SefamLoader::Open(const QString &path)
{
    Q_ASSERT(m_ctx);
    if (!Detect(path)) { return -1; }

    const QString serialDir = findSerialDir(path);
    QDir dir(serialDir);
    const QStringList dirs =
        dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    int ok = 0, skipped = 0, records = 0;
    for (const QString &d : dirs) {
        SefamParsing::SessionData data;
        QString error;
        if (!SefamParsing::readSession(dir.absoluteFilePath(d), data, error)) {
            qWarning() << "Sefam:" << d << "skipped —" << error;
            ++skipped;
            continue;
        }
        ++ok;
        records += data.recordCount;
        qDebug() << "Sefam" << data.dirName
                 << "records" << data.recordCount
                 << "channels" << data.samples.keys()
                 << "start" << data.header.localStart.toString("yyyy-MM-dd HH:mm:ss");
    }
    qDebug() << "Sefam TOTAL sessions" << ok << "skipped" << skipped
             << "hours" << (records * 10.0 / 3600.0);
    return ok;
}
```

- [ ] **Step 6: Build and verify against the golden numbers**

Import the sample card. Expected in the debug log:

```
Sefam DATA_001 records 3075 channels ("DET","FLW","LK","NSD","PRE","Y17") start "2026-07-10 22:03:34"
...
Sefam DATA_031 skipped — missing or empty .INI
Sefam TOTAL sessions 31 skipped 1 hours 159
```

Three things must hold: **31 sessions**, **`DATA_001` = 3075 records**, **total ≈ 159 hours**. No "truncating" warnings should appear — the sample card has zero checksum failures.

- [ ] **Step 7: Commit**

```bash
git add oscar/SleepLib/loader_plugins/sefamDataParsing.h \
        oscar/SleepLib/loader_plugins/sefamDataParsing.cpp \
        oscar/SleepLib/loader_plugins/sefam_loader.cpp
git commit -m "Read SEFAM .INI schema and verify channel records"
```

---

### Task 3: Import waveforms with sentinel-gap splitting

First task producing visible data in OSCAR.

> **This task's code below was superseded during execution.** Two problems only
> surfaced once data reached the screen, and the shipped implementation in
> `sefam_loader.cpp` differs from the listing here. Both are explained in
> `SEFAM_LOADER_DESIGN.md` §6:
>
> 1. **`EventList` offsets are applied inconsistently by OSCAR** — `gLineChart`
>    renders `(raw + offset) * gain`, while `EventList::data()` and
>    `FlowParser::openFlow()` ignore the offset. Passing `offset = -180` was
>    silently discarded and flow read ~204 L/min. Values are now pre-scaled and
>    `offset` is always 0.
> 2. **`FLW` is total flow, not patient flow.** It sits ~22 L/min above zero, and
>    `FlowParser::calcPeaks()` detects breaths by crossings of a hard-coded zero
>    line, so nothing was detected and OSCAR produced no respiratory rate, tidal
>    volume, minute ventilation or Ti/Te. The loader now imports `FLW − LK`.
>
> The `importWaveform` helper consequently takes `QVector<qint16>` values plus a
> validity mask rather than raw bytes. Read the implementation, not this listing.

**Files:**
- Modify: `oscar/SleepLib/loader_plugins/sefam_loader.cpp`

**Interfaces:**
- Consumes: `readSession()`, `SessionData`, `ChannelSpec` from Task 2.
- Produces: `SefamLoader::importWaveforms()`; sessions registered on the machine.

- [ ] **Step 1: Declare the waveform helper in `sefam_loader.h`**

Add to the `protected:` section:

```cpp
    /*! \brief Add one SEFAM channel to a session as one or more waveform EventLists.
        \param session OSCAR session to populate.
        \param samples Raw 8-bit samples, already verified.
        \param chan    Target OSCAR channel.
        \param gain    Multiplier applied to the raw byte.
        \param offset  Added after the gain.
        \param rateMs  Milliseconds per sample.
        \param startMs Session start, milliseconds since epoch.

        Runs of the 0xFF invalid sentinel are excluded. Each contiguous run of
        valid samples becomes its own EventList, because one EventList holds one
        contiguous span. Imported naively the sentinel would read as 153 L/min
        leak or 25.5 cmH2O and corrupt the statistics. */
    void importWaveform(Session *session, const QVector<quint8> &samples,
                        ChannelID chan, EventDataType gain, EventDataType offset,
                        int rateMs, qint64 startMs);
```

- [ ] **Step 2: Implement importWaveform in `sefam_loader.cpp`**

```cpp
void SefamLoader::importWaveform(Session *session, const QVector<quint8> &samples,
                                 ChannelID chan, EventDataType gain, EventDataType offset,
                                 int rateMs, qint64 startMs)
{
    const int n = samples.size();
    int i = 0;
    while (i < n) {
        // Skip a run of sentinel values.
        while (i < n && samples.at(i) == SefamParsing::kInvalidSample) { ++i; }
        if (i >= n) { break; }

        const int runStart = i;
        while (i < n && samples.at(i) != SefamParsing::kInvalidSample) { ++i; }
        const int runLen = i - runStart;

        EventList *el = session->AddEventList(chan, EVL_Waveform, gain, offset,
                                              0.0, 0.0, rateMs);
        // AddWaveform takes a mutable pointer; samples is const, so copy the run.
        QVector<quint8> run = samples.mid(runStart, runLen);
        el->AddWaveform(startMs + static_cast<qint64>(runStart) * rateMs,
                        run.data(), runLen,
                        static_cast<qint64>(runLen) * rateMs);
    }
}
```

- [ ] **Step 3: Replace Open with real session creation**

```cpp
int SefamLoader::Open(const QString &path)
{
    Q_ASSERT(m_ctx);
    if (!Detect(path)) { return -1; }

    const QString serialDir = findSerialDir(path);
    QDir dir(serialDir);

    const MachineInfo info = PeekInfo(path);
    m_ctx->CreateMachineFromInfo(info);
    Machine *mach = p_profile->CreateMachine(info);

    // Only one model has been validated end-to-end against a manufacturer report.
    // Others import on a best-effort basis but must announce themselves.
    if (info.properties.value(QStringLiteral("ModelCode")) != sefam_validated_model) {
        MachineInfo untested = info;      // the signal takes a non-const reference
        emit deviceIsUntested(untested);
    }

    backupData(mach, path);

    const QStringList dirs =
        dir.entryList(QStringList("DATA_*"), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    emit updateMessage(QObject::tr("Reading SEFAM card..."));
    emit setProgressMax(dirs.size());
    emit setProgressValue(0);

    int imported = 0, skipped = 0, progress = 0;

    for (const QString &d : dirs) {
        if (isAborted()) { break; }
        emit setProgressValue(++progress);
        QCoreApplication::processEvents();

        SefamParsing::SessionData data;
        QString error;
        if (!SefamParsing::readSession(dir.absoluteFilePath(d), data, error)) {
            qWarning() << "Sefam:" << d << "skipped —" << error;
            ++skipped;
            continue;
        }

        const SessionID sid = data.header.utcEpoch != 0
                            ? static_cast<SessionID>(data.header.utcEpoch)
                            : static_cast<SessionID>(data.header.localStart.toSecsSinceEpoch());
        if (mach->SessionExists(sid)) { continue; }

        const qint64 startMs = data.header.localStart.toMSecsSinceEpoch();
        const qint64 endMs   = startMs
                             + static_cast<qint64>(data.recordCount)
                               * SefamParsing::kRecordSeconds * 1000LL;

        Session *session = new Session(mach, sid);
        session->SetChanged(true);
        session->really_set_first(startMs);
        session->really_set_last(endMs);

        for (auto it = data.samples.constBegin(); it != data.samples.constEnd(); ++it) {
            const ChannelSpec &spec = data.channels.value(it.key());
            const int rateMs = 1000 / spec.freq;

            if (it.key() == "FLW") {
                importWaveform(session, it.value(), CPAP_FlowRate,
                               460.0f / 255.0f, -180.0f, rateMs, startMs);
            } else if (it.key() == "PRE") {
                importWaveform(session, it.value(), CPAP_Pressure,
                               0.1f, 0.0f, rateMs, startMs);
            } else if (it.key() == "LK") {
                importWaveform(session, it.value(), CPAP_LeakTotal,
                               0.6f, 0.0f, rateMs, startMs);
            }
            // DET, NSD and Y17 are deliberately not imported — see the design spec.
        }

        if (session->eventlist.isEmpty()) {
            qWarning() << "Sefam:" << d << "skipped — no valid samples";
            delete session;
            ++skipped;
            continue;
        }

        session->UpdateSummaries();
        mach->AddSession(session);
        ++imported;
    }

    mach->Save();
    finishAddingSessions();

    if (skipped > 0) {
        qWarning() << "Sefam: imported" << imported << "sessions," << skipped << "skipped";
    }
    return imported;
}
```

Add these includes at the top of `sefam_loader.cpp`:

```cpp
#include <QCoreApplication>
#include "SleepLib/profiles.h"
```

Also add `using SefamParsing::ChannelSpec;` beneath the includes — `Open` refers to
`ChannelSpec` unqualified when looking up each channel's rate.

If `sefam_loader.h` does not compile because `Session`, `EventList`, `ChannelID` or
`EventDataType` are unknown at the point `importWaveform` is declared, add
`#include "SleepLib/session.h"` to `sefam_loader.h`. `machine.h` pulls in most of
SleepLib but the declaration order is worth confirming rather than assuming.

- [ ] **Step 4: Build and verify the waveforms**

Import the sample card, then open Daily view on **2026-07-10**.

- Flow Rate, Pressure and Total Leak graphs all render.
- **Leak graph shows gaps, not spikes to 153 L/min.** This is the check that sentinel splitting worked; a spike means Step 2 is wrong.
- Pressure for the 22:03 session averages ≈ 5.2 cmH₂O (report value). Session details show it.
- Respiratory Rate, Tidal Volume, Minute Vent and Ti/Te appear even though we never imported them — `UpdateSummaries()` derives them from flow.

- [ ] **Step 5: Commit**

```bash
git add oscar/SleepLib/loader_plugins/sefam_loader.h \
        oscar/SleepLib/loader_plugins/sefam_loader.cpp
git commit -m "Import SEFAM flow, pressure and leak waveforms"
```

---

### Task 4: Parse the event log and import respiratory events

**Files:**
- Modify: `oscar/SleepLib/loader_plugins/sefamDataParsing.h`
- Modify: `oscar/SleepLib/loader_plugins/sefamDataParsing.cpp`
- Modify: `oscar/SleepLib/loader_plugins/sefam_loader.cpp`

**Interfaces:**
- Consumes: `SessionData`, `FileHeader` from Tasks 1-2.
- Produces: `LogRecord`, `readLog()`, `SessionData::log`, and event channels on each session.

- [ ] **Step 1: Add LogRecord to `sefamDataParsing.h`**

Add the constant beside the others:

```cpp
//! Fixed size of one .LOG event record.
constexpr int kLogRecordLength = 49;
```

Add before `SessionData`:

```cpp
/*! \struct LogRecord
    \brief One 49-byte .LOG event record.

    Layout: uint32 LE UTC epoch, uint32 zero (upper half of a 64-bit time_t),
    uint8 event code, uint16 big-endian argument, then 38 bytes that are zero
    except on the settings records (codes 2 and 13). */
struct LogRecord
{
    qint64     utcSeconds = 0;
    quint8     code       = 0;
    quint16    arg        = 0;
    QByteArray payload;             //!< Bytes 11..48 inclusive.
};

//! Event codes, confirmed against the manufacturer's analyzer output.
enum LogCode {
    kLogSettingsChange   = 2,       //!< Carries therapy settings in payload.
    kLogObstructiveApnea = 3,       //!< arg = duration in 0.1 s.
    kLogCentralApnea     = 4,       //!< arg = duration in 0.1 s.
    kLogObstructiveHypop = 5,       //!< arg is zero in ~97% of records.
    kLogCentralHypopnea  = 6,       //!< arg is zero in ~97% of records.
    kLogSnore            = 7,
    kLogFlowLimitation   = 8,
    kLogSettingsSnapshot = 13       //!< Also carries settings; fallback for code 2.
};
```

Add to `SessionData`, after `samples`:

```cpp
    QVector<LogRecord>               log;            //!< Decoded .LOG records.
```

Add the declaration beside `readChannel`:

```cpp
/*! \brief Read a session's .LOG file.
    \return false if the file is missing, unreadable, or has a bad header.

    Trailing bytes that do not form a whole 49-byte record are ignored. */
bool readLog(const QString &path, QVector<LogRecord> &out);
```

- [ ] **Step 2: Implement readLog in `sefamDataParsing.cpp`**

```cpp
bool readLog(const QString &path, QVector<LogRecord> &out)
{
    out.clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { return false; }
    const QByteArray raw = f.readAll();
    f.close();

    // Header only — the log records that follow are plaintext.
    QByteArray head = raw.left(kChannelHeaderLength);
    descramble(head);

    FileHeader hdr;
    if (!parseHeader(head, hdr)) { return false; }

    const char *p = raw.constData();
    for (int off = hdr.length; off + kLogRecordLength <= raw.size(); off += kLogRecordLength) {
        const char *r = p + off;

        LogRecord rec;
        rec.utcSeconds =  static_cast<quint8>(r[0])
                       | (static_cast<quint8>(r[1]) << 8)
                       | (static_cast<quint8>(r[2]) << 16)
                       | (static_cast<qint64>(static_cast<quint8>(r[3])) << 24);
        rec.code = static_cast<quint8>(r[8]);
        rec.arg  = static_cast<quint16>((static_cast<quint8>(r[9]) << 8)
                                       | static_cast<quint8>(r[10]));
        rec.payload = QByteArray(r + 11, kLogRecordLength - 11);
        out.append(rec);
    }
    return true;
}
```

- [ ] **Step 3: Call readLog from readSession**

In `readSession`, immediately before the final `return true;`:

```cpp
    const QString logPath = dir.absoluteFilePath(out.dirName + ".LOG");
    if (QFile::exists(logPath) && !readLog(logPath, out.log)) {
        qWarning() << "Sefam:" << out.dirName << "log unreadable — events skipped";
    }
```

- [ ] **Step 4: Import events in `sefam_loader.cpp`**

Insert into `Open`, after the waveform loop and before the `session->eventlist.isEmpty()` check:

```cpp
        // Events are placed relative to the session start so no timezone
        // arithmetic is needed. This requires the UTC epoch, which the short
        // header variant does not carry.
        if (!data.log.isEmpty() && data.header.utcEpoch != 0) {
            EventList *oa    = session->AddEventList(CPAP_Obstructive, EVL_Event);
            EventList *ca    = session->AddEventList(CPAP_ClearAirway, EVL_Event);
            EventList *hyp   = session->AddEventList(CPAP_Hypopnea,    EVL_Event);
            EventList *snore = session->AddEventList(CPAP_VSnore,      EVL_Event);
            EventList *fl    = session->AddEventList(CPAP_FlowLimit,   EVL_Event);

            for (const SefamParsing::LogRecord &rec : data.log) {
                const qint64 offsetMs =
                    (rec.utcSeconds - data.header.utcEpoch) * 1000LL;
                if (offsetMs < 0 || offsetMs > (endMs - startMs)) { continue; }
                const qint64 when = startMs + offsetMs;

                // Timestamps are treated as the event END, matching bmc_loader.
                switch (rec.code) {
                case SefamParsing::kLogObstructiveApnea:
                    oa->AddEvent(when, rec.arg / 10.0f); break;
                case SefamParsing::kLogCentralApnea:
                    ca->AddEvent(when, rec.arg / 10.0f); break;
                case SefamParsing::kLogObstructiveHypop:
                case SefamParsing::kLogCentralHypopnea:
                    // OSCAR has no central hypopnea channel; bmcG3xDataParsing and
                    // prisma_loader both fold both kinds into CPAP_Hypopnea.
                    // Duration is not in the log, so zero is recorded rather than
                    // a fabricated value.
                    hyp->AddEvent(when, 0); break;
                case SefamParsing::kLogSnore:
                    snore->AddEvent(when, 0); break;
                case SefamParsing::kLogFlowLimitation:
                    fl->AddEvent(when, 0); break;
                default:
                    break;      // administrative codes are parsed but not imported
                }
            }
        } else if (!data.log.isEmpty()) {
            qWarning() << "Sefam:" << d
                       << "has no UTC epoch in its header — events skipped";
        }
```

- [ ] **Step 5: Build and verify the event counts**

Import the sample card into a **fresh profile** (so counts aren't doubled), then check the Statistics page over 2026-07-10 to 2026-07-30:

| Channel | Expected | Vendor CSV | Note |
|---|---|---|---|
| Obstructive Apnea | 243 | 242 | |
| Clear Airway | 190 | 190 | exact |
| Hypopnea | 306 | 307 | OH 74 + CH 232 |
| Vibratory Snore | 1402 | 1399 | |
| Flow Limitation | 955 | — | vendor CSV column is internally inconsistent; its synthesis index agrees |
| **AHI** | **≈ 4.7** | 4.7 | |

These are **windowed** counts. The loader discards log records whose timestamp
falls outside their own session's recording span — 114 across this card, which
belong to adjacent sessions. Filtering moves every count *closer* to the
manufacturer's per-session figures, so the window test is doing real work rather
than merely trimming.

Then confirm event placement against the manufacturer's rendering: open
`Reports/Waveforms/2026-July-10_21H44_8h52min.pdf` from the report folder and compare
an apnea's position against the same time in OSCAR's Daily view. If OSCAR's events sit
one event-duration later than the vendor's, timestamps are starts not ends — change
`AddEvent(when, ...)` to `AddEvent(when + duration, ...)` for codes 3 and 4 and note it
in `Notes/Developer Notes/BUG_FIXES.md`.

- [ ] **Step 6: Commit**

```bash
git add oscar/SleepLib/loader_plugins/sefamDataParsing.h \
        oscar/SleepLib/loader_plugins/sefamDataParsing.cpp \
        oscar/SleepLib/loader_plugins/sefam_loader.cpp
git commit -m "Import SEFAM respiratory events from the session log"
```

---

### Task 5: Therapy settings

**Files:**
- Modify: `oscar/SleepLib/loader_plugins/sefamDataParsing.h`
- Modify: `oscar/SleepLib/loader_plugins/sefamDataParsing.cpp`
- Modify: `oscar/SleepLib/loader_plugins/sefam_loader.cpp`

**Interfaces:**
- Consumes: `LogRecord`, `SessionData` from Task 4.
- Produces: `Settings`, `parseSettings()`, `SessionData::settings`.

- [ ] **Step 1: Add Settings to `sefamDataParsing.h`**

Add before `SessionData`:

```cpp
/*! \struct Settings
    \brief Therapy settings decoded from a log settings record.

    Only fields confirmed against the manufacturer's printed settings are
    represented. The humidifier and comfort-level bytes are positional guesses
    that a single card cannot vary, so they are deliberately absent — an
    unverified byte displayed as a therapy setting is worse than showing
    nothing. */
struct Settings
{
    bool  valid        = false;
    float minPressure  = 0.0f;      //!< cmH2O
    float maxPressure  = 0.0f;      //!< cmH2O
    float rampPressure = 0.0f;      //!< cmH2O
    int   rampMinutes  = 0;
};
```

Add to `SessionData`, after `log`:

```cpp
    Settings                         settings;
```

Add the declaration:

```cpp
/*! \brief Decode therapy settings from a log record.
    \param rec A record with code kLogSettingsChange or kLogSettingsSnapshot.
    \param out Populated and marked valid on success.
    \return false if the record is not a settings record or the payload is short.

    Payload offsets are relative to record byte 11, which is payload index 0.
    Code 2:  index 0 = min pressure, 1 = ramp minutes, 4 = max pressure,
             5 = ramp pressure. Pressures are tenths of a cmH2O.
    Code 13: index 3 = min pressure, index 5 = max pressure; it carries no ramp
             fields, so those stay zero. */
bool parseSettings(const LogRecord &rec, Settings &out);
```

- [ ] **Step 2: Implement parseSettings**

```cpp
bool parseSettings(const LogRecord &rec, Settings &out)
{
    const auto byteAt = [&rec](int index) -> int {
        return static_cast<quint8>(rec.payload.at(index));
    };

    if (rec.code == kLogSettingsChange) {
        if (rec.payload.size() < 6) { return false; }
        out.minPressure  = byteAt(0) / 10.0f;
        out.rampMinutes  = byteAt(1);
        out.maxPressure  = byteAt(4) / 10.0f;
        out.rampPressure = byteAt(5) / 10.0f;
        out.valid = true;
        return true;
    }
    if (rec.code == kLogSettingsSnapshot) {
        if (rec.payload.size() < 6) { return false; }
        out.minPressure = byteAt(3) / 10.0f;
        out.maxPressure = byteAt(5) / 10.0f;
        out.valid = true;
        return true;
    }
    return false;
}
```

- [ ] **Step 3: Populate settings in readSession**

In `readSession`, after the `readLog` block and before `return true;`:

```cpp
    // Prefer a full settings-change record; fall back to a snapshot, which
    // carries pressures but no ramp fields.
    for (const LogRecord &rec : out.log) {
        if (rec.code == kLogSettingsChange && parseSettings(rec, out.settings)) { break; }
    }
    if (!out.settings.valid) {
        for (const LogRecord &rec : out.log) {
            if (rec.code == kLogSettingsSnapshot && parseSettings(rec, out.settings)) { break; }
        }
    }
```

- [ ] **Step 4: Write settings onto the session**

In `Open`, immediately after `session->really_set_last(endMs);`:

```cpp
        if (data.settings.valid) {
            // A-PAP is the only mode observed on any SEFAM card examined.
            session->settings[CPAP_Mode]        = MODE_APAP;
            session->settings[CPAP_PressureMin] = data.settings.minPressure;
            session->settings[CPAP_PressureMax] = data.settings.maxPressure;
            if (data.settings.rampMinutes > 0) {
                session->settings[CPAP_RampTime]     = data.settings.rampMinutes;
                session->settings[CPAP_RampPressure] = data.settings.rampPressure;
            }
        }
        // A session with no settings record carries no settings rather than
        // inheriting a neighbour's.
```

- [ ] **Step 5: Build and verify**

Reimport into a fresh profile. In Daily view for 2026-07-10, the Device Settings panel must show:

```
Mode           APAP
Min Pressure   4.0 cmH2O
Max Pressure   20.0 cmH2O
Ramp Time      45 minutes
Ramp Pressure  4.0 cmH2O
```

Only five sessions on this card carry a code-2 record, so most sessions will fall back
to the code-13 snapshot and show min/max but no ramp. That is expected.

- [ ] **Step 6: Commit**

```bash
git add oscar/SleepLib/loader_plugins/sefamDataParsing.h \
        oscar/SleepLib/loader_plugins/sefamDataParsing.cpp \
        oscar/SleepLib/loader_plugins/sefam_loader.cpp
git commit -m "Import SEFAM therapy settings from the session log"
```

---

### Task 6: Card backup

**Files:**
- Modify: `oscar/SleepLib/loader_plugins/sefam_loader.cpp`

**Interfaces:**
- Consumes: `Machine::getBackupPath()`, `copyPath()` from `SleepLib/common.h`, `p_profile->session->backupCardData()`.
- Produces: a populated backup folder that `Detect()` accepts.

- [ ] **Step 1: Implement backupData**

Replace the stub:

```cpp
bool SefamLoader::backupData(Machine *mach, const QString &path)
{
    // Compare QDir objects rather than strings: separators differ on Windows,
    // and mistaking the backup folder for a different path would copy it onto
    // itself.
    QDir ipath(path);
    QDir bpath(mach->getBackupPath());

    if (ipath == bpath) {
        rebuild_from_backups = true;
        create_backups = false;
    } else {
        rebuild_from_backups = false;
        create_backups = p_profile->session->backupCardData();
    }

    if (rebuild_from_backups || !create_backups) { return true; }

    QDir dir;
    if (!dir.exists(bpath.absolutePath()) && !dir.mkpath(bpath.absolutePath())) {
        qWarning() << "Sefam: could not create backup directory" << bpath.absolutePath();
        return false;
    }

    emit updateMessage(QObject::tr("Creating data backup..."));
    QCoreApplication::processEvents();

    // The whole card is copied, including the encrypted .RAM/.BKP images. They
    // are not read by this loader, but they are the only place the format's
    // remaining unknowns could ever be answered from, so a backup that dropped
    // them would destroy the one artefact a future investigation would need.
    copyPath(ipath.absolutePath(), bpath.absolutePath(), true);
    return true;
}
```

Add `#include "SleepLib/common.h"` to the includes.

- [ ] **Step 2: Build and verify the backup round-trip**

1. In Preferences, confirm "Backup Card Data" is enabled.
2. Import the sample card into a fresh profile.
3. Confirm the backup folder now contains `1279R/<serial>/DATA_000` … `DATA_031` plus the two `.RAM`/`.BKP` files, roughly 31 MB.
4. Create another fresh profile and import **from the backup folder**. All 31 sessions must import, and the debug log must not show a second backup being created.

- [ ] **Step 3: Commit**

```bash
git add oscar/SleepLib/loader_plugins/sefam_loader.cpp
git commit -m "Back up SEFAM card data on import"
```

---

## Final acceptance

Run once after Task 6, against a fresh profile:

- 31 sessions across 2026-07-10 to 2026-07-30, 159.0 h total.
- Event totals: OA 248, CA 191, Hypopnea 309, Snore 1408, FL 961. AHI ≈ 4.7.
- Settings show APAP 4.0-20.0, ramp 45 min at 4.0.
- Leak graph shows gaps, never 153 L/min spikes.
- Event placement agrees with a manufacturer waveform PDF.
- At least one other loader still imports correctly.

Note that OSCAR's event counts run about 2% above the manufacturer's per-session
figures over long periods. Their analyzer applies ramp and leak inclusion filtering
that the raw log does not. This is a scoring-policy difference and must **not** be
"fixed" by inventing filters to match.

## Deferred

Recorded so they are not mistaken for oversights:

- Hypopnea durations (not in the log; `Y17` is the suspected source).
- Central hypopnea as its own channel — a cross-loader schema change affecting
  BMC G3X and Prisma as well, deliberately not attempted here.
- Humidifier and comfort settings (unverified payload bytes).
- Log code 10, 1.64/h, unidentified.
- `Htmldocs/release_notes.html` — a new loader is release-note material, but that
  file is maintained by the project owner.
