/* OSCAR ZIP archive creation
 * Provides a Qt-convenient wrapper around miniz, see https://github.com/richgel999/miniz
 *
 * Copyright (c) 2020-2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "zip.h"
#include <QDebug>
#include <QDateTime>
#include <QCoreApplication>
#include "SleepLib/progressdialog.h"

static const quint64 PROGRESS_SCALE = 1024;  // QProgressBar only holds an int, so report progress in KiB.

// Static functions to abstract the details of miniz from the primary logic.
static void* zip_init();
static bool zip_open(void* ctx, QFile & file);
static bool zip_add(void* ctx, const QString & archive_name, const QByteArray & data, const QDateTime & modified);
static bool zip_add_file(void* ctx, const QString & archive_name, QFile & file, const QDateTime & modified, ZipFile* zip, quint64 fileStart);
static void zip_close(void* ctx);
static void zip_done(void* ctx);


ZipFile::ZipFile()
    : m_abort(false)
    , m_progress(0)
    , m_lastNotified(0)
{
    m_ctx = zip_init();
}

ZipFile::~ZipFile()
{
    Close();
    zip_done(m_ctx);
}

bool ZipFile::Open(const QString & filepath)
{
    m_file.setFileName(filepath);
    bool ok = m_file.open(QIODevice::WriteOnly);
    if (!ok) {
        qWarning() << "Could not open" << m_file.fileName() << "for writing, error code" << m_file.error() << m_file.errorString();
//        qWarning() << "unable to open" << m_file.fileName();
        return false;
    }
    ok = zip_open(m_ctx, m_file);
    return ok;
}

void ZipFile::Close()
{
    if (m_file.isOpen()) {
        zip_close(m_ctx);
        m_file.close();
    }
}

bool ZipFile::AddDirectory(const QString & path, ProgressDialog* progress)
{
    return AddDirectory(path, "", progress);
}

bool ZipFile::AddDirectory(const QString & path, const QString & prefix, ProgressDialog* progress)
{
    bool ok;
    FileQueue queue;
    queue.AddDirectory(path, prefix);
    ok = AddFiles(queue, progress);
    return ok;
}

bool ZipFile::AddFiles(FileQueue & queue, ProgressDialog* progress)
{
    bool ok;
    
    // Exclude the zip file that's being created (if it happens to be in the list).
    queue.Remove(QFileInfo(m_file).canonicalFilePath());

    qDebug().noquote() << "Adding" << queue.toString();
    m_abort = false;
    m_progress = 0;
    m_lastNotified = 0;

    if (progress) {
        progress->addAbortButton();
        progress->setWindowModality(Qt::ApplicationModal);
        progress->open();
        connect(this, SIGNAL(setProgressMax(int)), progress, SLOT(setProgressMax(int)));
        connect(this, SIGNAL(setProgressValue(int)), progress, SLOT(setProgressValue(int)));
        connect(progress, SIGNAL(abortClicked()), this, SLOT(abort()));
    }

    // Always emit, since the caller may have configured and connected a progress dialog manually.
    emit setProgressValue(m_progress/PROGRESS_SCALE);
    emit setProgressMax((queue.byteCount() + queue.dirCount())/PROGRESS_SCALE);
    QCoreApplication::processEvents();

    for (auto & entry : queue.files()) {
        ok = AddFile(entry.path, entry.name);
        if (!ok || m_abort) {
            break;
        }
    }
    
    if (progress) {
        disconnect(progress, SIGNAL(abortClicked()), this, SLOT(abort()));
        disconnect(this, SIGNAL(setProgressMax(int)), progress, SLOT(setProgressMax(int)));
        disconnect(this, SIGNAL(setProgressValue(int)), progress, SLOT(setProgressValue(int)));
        progress->close();
        progress->deleteLater();
    }

    if (!ok) {
        qWarning().noquote() << "Unable to create" << m_file.fileName();
        Close();
        m_file.remove();
    } else if (aborted()) {
        qDebug().noquote() << "User canceled zip creation.";
        Close();
        m_file.remove();
    } else {
        qDebug().noquote() << "Created" << m_file.fileName() << m_file.size() << "bytes";
    }

    return ok;
}

bool ZipFile::AddFile(const QString & path, const QString & name)
{
    if (!m_file.isOpen()) {
        qWarning() << m_file.fileName() << "has not been opened for writing";
        return false;
    }

    QFileInfo fi(path);
    QString archive_name = name;
    if (archive_name.isEmpty()) archive_name = fi.fileName();

    bool ok;
    if (fi.isDir()) {
        archive_name += "/";
        m_progress += 1;
        ok = zip_add(m_ctx, archive_name, QByteArray(), fi.lastModified());
    } else {
        // Stream file through miniz without loading it into RAM.
        // The read callback updates m_progress incrementally as bytes are compressed.
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << path << "can't open";
            return false;
        }
        quint64 fileStart = m_progress;
        ok = zip_add_file(m_ctx, archive_name, f, fi.lastModified(), this, fileStart);
    }

    emit setProgressValue(m_progress/PROGRESS_SCALE);
    QCoreApplication::processEvents();

    return ok;
}


// ==================================================================================================


bool FileQueue::AddDirectory(const QString & path, const QString & prefix)
{
    QDir dir(path);
    if (!dir.exists() || !dir.isReadable()) {
        qWarning() << dir.canonicalPath() << "can't read directory";
#if defined(Q_OS_MACOS)
        // If this is a directory known to be protected by macOS "Full Disk Access" permissions,
        // skip it but don't consider it an error.
        static const QSet<QString> s_macProtectedDirs = { ".fseventsd", ".Spotlight-V100", ".Trashes" };
        if (s_macProtectedDirs.contains(dir.dirName())) {
            return true;
        }
#endif
        return false;
    }
    QString base = prefix;
    if (base.isEmpty()) base = dir.dirName();

    // Add directory entry
    bool ok = AddFile(dir.canonicalPath(), base);
    if (!ok) {
        return false;
    }

    dir.setFilter(QDir::NoDotAndDotDot | QDir::Dirs | QDir::Files | QDir::Hidden);
    dir.setSorting(QDir::Name);
    QFileInfoList flist = dir.entryInfoList();

    for (auto & fi : flist) {
        QString canonicalPath = fi.canonicalFilePath();
        QString relative_path = base + "/" + fi.fileName();
        if (fi.isSymLink()) {
            qWarning() << "skipping symlink" << canonicalPath << fi.symLinkTarget();
        } else if (fi.isDir()) {
            // Descend and recurse
            ok &= AddDirectory(canonicalPath, relative_path);
        } else {
            // Add the file to the zip
            ok &= AddFile(canonicalPath, relative_path);
        }
        // Don't stop in our tracks when we hit an error.
    }

    return ok;
}

bool FileQueue::AddFile(const QString & path, const QString & prefix)
{
    QFileInfo fi(path);
    QString canonicalPath = fi.canonicalFilePath();
    QString archive_name = prefix;

    if (archive_name.isEmpty()) archive_name = fi.fileName();

    if (fi.isDir()) {
        m_dir_count++;
    } else if (fi.exists()) {
        m_file_count++;
        m_byte_count += fi.size();
    } else {
        qWarning() << "file doesn't exist" << canonicalPath;
        return false;
    }
    Entry entry = { canonicalPath, archive_name };
    m_files.append(entry);
    QCoreApplication::processEvents();
    return true;
}

int FileQueue::Remove(const QString & path, QString* outName)
{
    QFileInfo fi(path);
    QString canonicalPath = fi.canonicalFilePath();
    int removed = 0;

    QMutableListIterator<Entry> i(m_files);
    while (i.hasNext()) {
        Entry & entry = i.next();
        if (entry.path == canonicalPath) {
            if (outName) {
                // If the caller cares about the name, it will most likely be re-added later rather than skipped.
                *outName = entry.name;
            } else {
                qDebug().noquote() << "skipping file:" << path;
            }

            if (fi.isDir()) {
                m_dir_count--;
            } else {
                m_file_count--;
                m_byte_count -= fi.size();
            }
            i.remove();
            removed++;
        }
    }
    
    if (removed > 1) {
        qWarning().noquote() << removed << "copies found in zip queue:" << path;
    }
    return removed;
}

const QString FileQueue::toString() const
{
    return QString("%1 directories, %2 files, %3 bytes").arg(m_dir_count).arg(m_file_count).arg(m_byte_count);
}


// ==================================================================================================
// Static functions to abstract the details of miniz from the primary logic.

#include "SleepLib/thirdparty/miniz.h"

// Callback for miniz to write compressed data
static size_t zip_write(void *pOpaque, mz_uint64 /*file_ofs*/, const void *pBuf, size_t n)
{
    if (pOpaque == nullptr) {
        qCritical() << "null pointer passed to ZipFile::Write!";
        return 0;
    }
    QFile* file = (QFile*) pOpaque;
    size_t written = file->write((const char*) pBuf, n);
    if (written < n) {
        qWarning() << "error writing to" << file->fileName();
    }
    return written;
}

static void* zip_init()
{
    mz_zip_archive* pZip = new mz_zip_archive();  // zero-initializes struct
    pZip->m_pWrite = zip_write;
    return pZip;
}

static void zip_done(void* ctx)
{
    Q_ASSERT(ctx);
    mz_zip_archive* pZip = (mz_zip_archive*) ctx;
    delete pZip;
}

static bool zip_open(void* ctx, QFile & file)
{
    Q_ASSERT(ctx);
    mz_zip_archive* pZip = (mz_zip_archive*) ctx;

    pZip->m_pIO_opaque = &file;
    bool ok = mz_zip_writer_init_v2(pZip, 0, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (!ok) {
        mz_zip_error mz_err = mz_zip_get_last_error(pZip);
        qWarning() << "unable to initialize miniz writer" << MZ_VERSION << mz_zip_get_error_string(mz_err);
    }
    return ok;
}

static bool zip_add(void* ctx, const QString & archive_name, const QByteArray & data, const QDateTime & modified)
{
    Q_ASSERT(ctx);
    mz_zip_archive* pZip = (mz_zip_archive*) ctx;

    // Add to .zip
    time_t last_modified = modified.toSecsSinceEpoch();  // technically deprecated, but miniz expects a time_t
    bool ok = mz_zip_writer_add_mem_ex_v2(pZip, archive_name.toLocal8Bit(), data.constData(), data.size(),
                                          nullptr, 0,  // no comment
                                          MZ_DEFAULT_COMPRESSION,
                                          0, 0,  // not used when compressing data
                                          &last_modified,
                                          nullptr, 0,  // no user extra data
                                          nullptr, 0   // no user extra data central
                                         );
    if (!ok) {
        mz_zip_error mz_err = mz_zip_get_last_error(pZip);
        qWarning() << "unable to add" << archive_name << ":" << data.size() << "bytes" << mz_zip_get_error_string(mz_err);
    }
    return ok;
}

struct ZipReadCtx {
    QFile*   file;
    ZipFile* zip;
    quint64  fileStart;  // m_progress value at the start of this file
};

// Read callback for mz_zip_writer_add_read_buf_callback.
// miniz reads sequentially so no seek is needed. Updates progress and processes
// events every 4 MB to keep the UI responsive without excessive syscall overhead.
static const quint64 ZIP_PROGRESS_INTERVAL = 4 * 1024 * 1024;  // 4 MB

static size_t zip_file_read(void* pOpaque, mz_uint64 file_ofs, void* pBuf, size_t n)
{
    ZipReadCtx* ctx = static_cast<ZipReadCtx*>(pOpaque);
    if (ctx->zip->aborted()) return 0;
    qint64 nread = ctx->file->read(static_cast<char*>(pBuf), static_cast<qint64>(n));
    if (nread > 0) {
        quint64 newProgress = ctx->fileStart + file_ofs + static_cast<quint64>(nread);
        ctx->zip->notifyReadProgress(newProgress, ZIP_PROGRESS_INTERVAL);
    }
    return nread < 0 ? 0 : static_cast<size_t>(nread);
}

// Stream a file into the zip via read callback — no whole-file heap allocation.
static bool zip_add_file(void* ctx, const QString & archive_name, QFile & file, const QDateTime & modified, ZipFile* zip, quint64 fileStart)
{
    Q_ASSERT(ctx);
    mz_zip_archive* pZip = (mz_zip_archive*) ctx;
    time_t last_modified = modified.toSecsSinceEpoch();
    mz_uint64 file_size = static_cast<mz_uint64>(file.size());
    ZipReadCtx readCtx = { &file, zip, fileStart };
    bool ok = mz_zip_writer_add_read_buf_callback(pZip, archive_name.toLocal8Bit(),
                                                  zip_file_read, &readCtx, file_size,
                                                  &last_modified,
                                                  nullptr, 0,   // no comment
                                                  MZ_BEST_SPEED,
                                                  nullptr, 0,   // no user extra data local
                                                  nullptr, 0);  // no user extra data central
    if (!ok) {
        mz_zip_error mz_err = mz_zip_get_last_error(pZip);
        qWarning() << "unable to add" << archive_name << ":" << file_size << "bytes" << mz_zip_get_error_string(mz_err);
    }
    return ok;
}

void ZipFile::notifyReadProgress(quint64 p, quint64 interval)
{
    if (interval > 0 && p - m_lastNotified < interval)
        return;
    m_lastNotified = p;
    m_progress = p;
    emit setProgressValue(static_cast<int>(m_progress / PROGRESS_SCALE));
    QCoreApplication::processEvents();
}

static void zip_close(void* ctx)
{
    Q_ASSERT(ctx);
    mz_zip_archive* pZip = (mz_zip_archive*) ctx;
    mz_zip_writer_finalize_archive(pZip);
    mz_zip_writer_end(pZip);
}


// ==================================================================================================
// UnzipFile — ZIP extraction wrapper

// miniz read callback: seeks QFile to file_ofs and reads n bytes into pBuf.
// This avoids loading the entire archive into RAM, allowing backups of any size.
static size_t unzip_qfile_read(void* pOpaque, mz_uint64 file_ofs, void* pBuf, size_t n)
{
    QFile* f = static_cast<QFile*>(pOpaque);
    if (!f->seek(static_cast<qint64>(file_ofs))) return 0;
    const qint64 nread = f->read(static_cast<char*>(pBuf), static_cast<qint64>(n));
    return nread < 0 ? 0 : static_cast<size_t>(nread);
}

// miniz write callback: streams decompressed chunks directly into an open QFile.
// file_ofs always increases monotonically so no seeking is needed.
static size_t unzip_qfile_write(void* pOpaque, mz_uint64 /*file_ofs*/, const void* pBuf, size_t n)
{
    QFile* f = static_cast<QFile*>(pOpaque);
    const qint64 written = f->write(static_cast<const char*>(pBuf), static_cast<qint64>(n));
    return written < 0 ? 0 : static_cast<size_t>(written);
}

struct UnzipProgressSink
{
    QFile* file;
    qint64 total;
    const std::function<void(qint64, qint64)>* progress;
};

// Same as unzip_qfile_write, but file_ofs is monotonic so file_ofs+written is the running total.
static size_t unzip_qfile_write_progress(void* pOpaque, mz_uint64 file_ofs, const void* pBuf, size_t n)
{
    UnzipProgressSink* sink = static_cast<UnzipProgressSink*>(pOpaque);
    const qint64 written = sink->file->write(static_cast<const char*>(pBuf), static_cast<qint64>(n));
    if (written < 0) {
        return 0;
    }
    if (*sink->progress) {
        (*sink->progress)(static_cast<qint64>(file_ofs) + written, sink->total);
    }
    return static_cast<size_t>(written);
}

/*!
 * \brief Construct an UnzipFile and allocate the internal miniz context.
 */
UnzipFile::UnzipFile()
    : m_open(false)
{
    mz_zip_archive* pZip = new mz_zip_archive();
    memset(pZip, 0, sizeof(*pZip));
    m_ctx = pZip;
}

UnzipFile::~UnzipFile()
{
    Close();
    delete static_cast<mz_zip_archive*>(m_ctx);
    m_ctx = nullptr;
}

/*!
 * \brief Open a ZIP archive for reading.
 *
 * Opens the file via QFile (handles Unicode paths on all platforms) and
 * initialises the miniz reader with a seek+read callback.  The file handle
 * is kept open in m_file until Close() is called.  This approach works for
 * archives of any size without loading them fully into RAM.
 *
 * After a successful Open(), call ExtractAll() then Close().
 */
bool UnzipFile::Open(const QString& filepath)
{
    if (m_open) {
        Close();
    }

    m_file.setFileName(filepath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        qWarning() << "UnzipFile::Open: cannot open" << filepath;
        return false;
    }

    mz_zip_archive* pZip = static_cast<mz_zip_archive*>(m_ctx);
    memset(pZip, 0, sizeof(*pZip));
    pZip->m_pRead       = unzip_qfile_read;
    pZip->m_pIO_opaque  = &m_file;

    if (!mz_zip_reader_init(pZip, static_cast<mz_uint64>(m_file.size()), 0)) {
        qWarning() << "UnzipFile::Open: not a valid ZIP:" << filepath;
        m_file.close();
        return false;
    }

    m_open = true;
    return true;
}

/*!
 * \brief Extract all entries in the archive to \a destDir.
 *
 * Directory entries are recreated; file entries are streamed via QFile so
 * that Unicode destination paths are handled correctly on all platforms.
 *
 * \param destDir  Root directory for extraction (created if absent).
 * \return true on success; false on any I/O or decompression error.
 */
bool UnzipFile::ExtractAll(const QString& destDir)
{
    if (!m_open) {
        qWarning() << "UnzipFile::ExtractAll: archive not open";
        return false;
    }

    mz_zip_archive* pZip = static_cast<mz_zip_archive*>(m_ctx);
    const int n = static_cast<int>(mz_zip_reader_get_num_files(pZip));

    if (!QDir().mkpath(destDir)) {
        qWarning() << "UnzipFile::ExtractAll: cannot create destination:" << destDir;
        return false;
    }

    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(pZip, static_cast<mz_uint>(i), &stat)) {
            qWarning() << "UnzipFile::ExtractAll: file_stat failed for index" << i;
            continue;
        }

        const QString archiveName = QString::fromUtf8(stat.m_filename);

        // Zip Slip guard: reject absolute paths and any entry whose canonical
        // destination escapes the extraction root.
        if (QFileInfo(archiveName).isAbsolute()) {
            qWarning() << "UnzipFile::ExtractAll: rejecting absolute entry path:" << archiveName;
            return false;
        }
        const QString destPath    = QDir(destDir).filePath(archiveName);
        const QString canonDest   = QFileInfo(destPath).canonicalFilePath();
        const QString canonRoot   = QFileInfo(destDir).canonicalFilePath();
        // canonicalFilePath() returns "" for paths that don't exist yet, so
        // fall back to the cleaned absolute path for new files/directories.
        const QString safeDest    = canonDest.isEmpty()
                                        ? QDir::cleanPath(QFileInfo(destPath).absoluteFilePath())
                                        : canonDest;
        const QString safeRoot    = canonRoot.isEmpty()
                                        ? QDir::cleanPath(QFileInfo(destDir).absoluteFilePath())
                                        : canonRoot;
        if (!safeDest.startsWith(safeRoot + "/") && safeDest != safeRoot) {
            qWarning() << "UnzipFile::ExtractAll: rejecting entry that escapes extraction root:"
                       << archiveName;
            return false;
        }

        if (stat.m_is_directory) {
            QDir().mkpath(destPath);
            continue;
        }

        // Ensure the parent directory exists.
        QDir().mkpath(QFileInfo(destPath).absolutePath());

        // Stream-decompress directly into the output file via callback.
        // This avoids allocating the entire uncompressed entry in RAM, which would
        // fail for large files (e.g. event_data.sql can exceed several GB).
        QFile outFile(destPath);
        if (!outFile.open(QIODevice::WriteOnly)) {
            qWarning() << "UnzipFile::ExtractAll: cannot write" << destPath;
            return false;
        }
        const bool ok = mz_zip_reader_extract_to_callback(
            pZip, static_cast<mz_uint>(i), unzip_qfile_write, &outFile, 0);
        outFile.close();
        if (!ok) {
            QFile::remove(destPath);
            qWarning() << "UnzipFile::ExtractAll: decompression failed for" << archiveName;
            return false;
        }
    }

    return true;
}

/*!
 * \brief Extract the single entry \a entryName to \a destPath.
 */
bool UnzipFile::ExtractEntry(const QString& entryName, const QString& destPath,
                             const std::function<void(qint64, qint64)>& progress)
{
    if (!m_open) {
        qWarning() << "UnzipFile::ExtractEntry: archive not open";
        return false;
    }

    mz_zip_archive* pZip = static_cast<mz_zip_archive*>(m_ctx);
    const QByteArray archiveName = entryName.toUtf8();
    const int index = mz_zip_reader_locate_file(
        pZip, archiveName.constData(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    if (index < 0) {
        qWarning() << "UnzipFile::ExtractEntry: entry not found:" << entryName;
        return false;
    }

    if (!QDir().mkpath(QFileInfo(destPath).absolutePath())) {
        qWarning() << "UnzipFile::ExtractEntry: cannot create destination directory for" << destPath;
        return false;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(pZip, static_cast<mz_uint>(index), &stat)) {
        qWarning() << "UnzipFile::ExtractEntry: file_stat failed for" << entryName;
        return false;
    }

    QFile outFile(destPath);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "UnzipFile::ExtractEntry: cannot write" << destPath;
        return false;
    }
    UnzipProgressSink sink { &outFile, static_cast<qint64>(stat.m_uncomp_size), &progress };
    const bool ok = mz_zip_reader_extract_to_callback(
        pZip, static_cast<mz_uint>(index), unzip_qfile_write_progress, &sink, 0);
    outFile.close();
    if (!ok) {
        QFile::remove(destPath);
        qWarning() << "UnzipFile::ExtractEntry: decompression failed for" << entryName;
        return false;
    }

    return true;
}

/*!
 * \brief List the file entries in the archive.
 */
QVector<UnzipEntry> UnzipFile::ListEntries()
{
    QVector<UnzipEntry> entries;
    if (!m_open) {
        qWarning() << "UnzipFile::ListEntries: archive not open";
        return entries;
    }

    mz_zip_archive* pZip = static_cast<mz_zip_archive*>(m_ctx);
    const int count = static_cast<int>(mz_zip_reader_get_num_files(pZip));
    entries.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(pZip, static_cast<mz_uint>(i))) {
            continue;
        }
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(pZip, static_cast<mz_uint>(i), &stat)) {
            qWarning() << "UnzipFile::ListEntries: file_stat failed for index" << i;
            continue;
        }
        entries.append(UnzipEntry{QString::fromUtf8(stat.m_filename),
                                  static_cast<qint64>(stat.m_uncomp_size)});
    }
    return entries;
}

/*!
 * \brief Close the archive and release the miniz reader state.
 */
void UnzipFile::Close()
{
    if (m_open) {
        mz_zip_archive* pZip = static_cast<mz_zip_archive*>(m_ctx);
        mz_zip_reader_end(pZip);
        memset(pZip, 0, sizeof(*pZip));
        m_open = false;
    }
    m_file.close();
}

/*!
 * \brief Return the number of entries (files + directories) in the archive.
 */
int UnzipFile::entryCount() const
{
    if (!m_open) return 0;
    // mz_zip_reader_get_num_files takes a non-const pointer even though it is
    // logically a read-only query, so cast away const here.
    return static_cast<int>(
        mz_zip_reader_get_num_files(static_cast<mz_zip_archive*>(m_ctx)));
}
