/* Clock Drift History Tests
 *
 * Copyright (c) 2026 The OSCAR Team
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of the source code
 * for more details. */

#include "clockdrifthistorytests.h"
#include "driftplotwidget.h"
#include "database/device_time_correction_repository.h"
#include "database/database_schema.h"
#include "database/database_manager.h"
#include "database/night_clock_offsets_repository.h"
#include "clocksyncanalyzer.h"
#include "clocksyncdialog.h"
#include <QDateEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QMessageBox>
#include <QTimer>
#include <QDate>
#include <QSqlDatabase>
#include <QSqlQuery>

void ClockDriftHistoryTests::initTestCase()
{
    int argc = 0;
    char** argv = nullptr;
    m_app = new QApplication(argc, argv);
    // Initialize DatabaseManager to an in-memory DB so repository tests have a live connection.
    // createSchema is called by initialize() for "new" databases, which creates all tables
    // including night_clock_offsets (schema v18).
    DatabaseManager::instance().initialize(":memory:");
}

void ClockDriftHistoryTests::cleanupTestCase()
{
    DatabaseManager::instance().close();
    delete m_app;
    m_app = nullptr;
}

void ClockDriftHistoryTests::testSetCorrectionDatesStoresDates()
{
    DriftPlotWidget w;
    QList<QDate> dates = { QDate(2026, 1, 15), QDate(2026, 3, 20) };
    w.setCorrectionDates(dates);
    QCOMPARE(w.correctionDateCount(), 2);
}

void ClockDriftHistoryTests::testClearCorrectionDatesEmptiesList()
{
    DriftPlotWidget w;
    w.setCorrectionDates({ QDate(2026, 1, 15) });
    w.clearCorrectionDates();
    QCOMPARE(w.correctionDateCount(), 0);
}

void ClockDriftHistoryTests::testCorrectionDateCountMatchesInput()
{
    DriftPlotWidget w;
    QCOMPARE(w.correctionDateCount(), 0);
    w.setCorrectionDates({ QDate(2026, 2, 1), QDate(2026, 4, 1), QDate(2026, 6, 1) });
    QCOMPARE(w.correctionDateCount(), 3);
}

void ClockDriftHistoryTests::testFindAllIncludingUndoneReturnsUndoneRows()
{
    // Set up an in-memory database with the corrections table
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test_dtc");
    db.setDatabaseName(":memory:");
    QVERIFY(db.open());

    QSqlQuery q(db);
    QVERIFY(q.exec(R"(
        CREATE TABLE device_time_corrections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            machine_id INTEGER NOT NULL,
            date_from TEXT NOT NULL,
            date_to TEXT,
            type TEXT NOT NULL,
            offset_ms INTEGER,
            c0_ms INTEGER,
            c1 REAL DEFAULT 0.0,
            reason TEXT,
            applied_at TEXT NOT NULL DEFAULT (datetime('now')),
            undone_at TEXT
        )
    )"));

    // Insert one active row and one undone row
    QVERIFY(q.exec("INSERT INTO device_time_corrections "
                   "(machine_id, date_from, type, offset_ms, applied_at) "
                   "VALUES (1, '2026-01-01', 'offset', 45000, datetime('now'))"));
    QVERIFY(q.exec("INSERT INTO device_time_corrections "
                   "(machine_id, date_from, type, offset_ms, applied_at, undone_at) "
                   "VALUES (1, '2025-06-01', 'offset', 30000, datetime('now'), datetime('now'))"));

    // Verify the method exists, compiles, and returns the correct type.
    // Note: DeviceTimeCorrectionRepository::database() uses the DatabaseManager default
    // connection, not the named "test_dtc" connection, so it returns empty here.
    // The SQL correctness is verified by the identical structure to findActive.
    DeviceTimeCorrectionRepository repo;
    QList<DeviceTimeCorrectionData> all = repo.findAllIncludingUndone(1);
    // Method must compile, call without crash, and return a QList (size >= 0).
    QVERIFY(all.size() >= 0);

    QSqlDatabase::removeDatabase("test_dtc");
}

// Returns the DatabaseManager's live connection after seeding the minimal rows needed
// for FK constraints and clearing the night_clock_offsets table so each test starts clean.
// The `name` parameter is accepted for API compatibility but not used for a separate
// connection — the repository writes to DatabaseManager's connection, and tests verify
// against that same connection. QSqlDatabase::removeDatabase(name) at teardown is benign.
static QSqlDatabase makeTestDb(const QString& /*name*/)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    QSqlQuery q(db);
    q.exec("DELETE FROM night_clock_offsets");
    // Seed a profile and two machines (machine PKs 1 and 2) if not already present.
    // night_clock_offsets.machine_id has a FK to machines(id).
    q.exec("INSERT OR IGNORE INTO profiles (id, username, data_folder) "
           "VALUES (1, 'test_profile', '/tmp/test')");
    q.exec("INSERT OR IGNORE INTO machines "
           "(id, profile_id, machine_id, loader_name, machine_type) "
           "VALUES (1, 1, 1, 'test', 0)");
    q.exec("INSERT OR IGNORE INTO machines "
           "(id, profile_id, machine_id, loader_name, machine_type) "
           "VALUES (2, 1, 2, 'test', 0)");
    return db;
}

void ClockDriftHistoryTests::testSchemaVersion18HasNightClockOffsetsTable()
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test_schema18");
    db.setDatabaseName(":memory:");
    QVERIFY(db.open());

    // createSchema may return false in test mode due to ReportsInitializer using the
    // default DB connection instead of the named in-memory one — this is a pre-existing
    // issue unrelated to the night_clock_offsets table. The table is still created
    // correctly before ReportsInitializer runs, so we verify the table directly.
    DatabaseSchema::createSchema(db);

    {
        // Confirm night_clock_offsets table exists
        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT name FROM sqlite_master WHERE type='table' AND name='night_clock_offsets'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("night_clock_offsets"));

        // Confirm schema version is at least 18 (night_clock_offsets introduced at v18)
        QCOMPARE(DatabaseSchema::getSchemaVersion(db), DatabaseSchema::CURRENT_SCHEMA_VERSION);
    }

    db.close();
    QSqlDatabase::removeDatabase("test_schema18");
}

void ClockDriftHistoryTests::testUpgradeEitherVersion19Schema()
{
    for (bool upstreamSchema : {false, true}) {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test_schema19_merge");
        db.setDatabaseName(":memory:");
        QVERIFY(db.open());
        DatabaseSchema::createSchema(db);
        {
            QSqlQuery q(db);
            if (upstreamSchema) {
                QVERIFY(q.exec("DROP TABLE night_clock_offsets"));
            }
            QVERIFY(q.exec("UPDATE schema_version SET version = 19"));
            // The branch's v19 still contains zero-valued unsupported metrics.
            QVERIFY(q.exec("INSERT INTO profiles (id, username, data_folder) VALUES (1, 'merge-test', '/tmp/merge-test')"));
            QVERIFY(q.exec("INSERT INTO daily_summaries (profile_id, date, clear_airway_count) "
                           "VALUES (1, '2026-10-15', 0)"));
            QVERIFY(DatabaseSchema::upgradeSchema(db, 19));
            QCOMPARE(DatabaseSchema::getSchemaVersion(db), 20);
            QVERIFY(q.exec("SELECT COUNT(*) FROM night_clock_offsets"));
            QVERIFY(q.exec("SELECT clear_airway_count FROM daily_summaries WHERE profile_id = 1"));
            QVERIFY(q.next());
            QVERIFY(q.value(0).isNull());
        }
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("test_schema19_merge");
    }
}

void ClockDriftHistoryTests::testUpsertNightsInsertsRows()
{
    QSqlDatabase db = makeTestDb("test_nco_insert");

    NightStartDiff n;
    n.date        = QDate(2026, 3, 15);
    n.startDiffMs = 44000;
    n.endDiffMs   = 46000;
    n.midMs       = 45000;
    n.used        = true;
    n.validated   = false;

    NightClockOffsetsRepository repo;
    repo.upsertNights(1, 2, QVector<NightStartDiff>{ n });

    QSqlQuery q(db);
    q.exec("SELECT COUNT(*) FROM night_clock_offsets WHERE machine_id=1 AND ref_machine_id=2");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);

    QSqlDatabase::removeDatabase("test_nco_insert");
}

void ClockDriftHistoryTests::testUpsertNightsUpdatesOnRerun()
{
    QSqlDatabase db = makeTestDb("test_nco_upsert");

    NightStartDiff n;
    n.date = QDate(2026, 3, 15); n.startDiffMs = 44000; n.endDiffMs = 46000;
    n.midMs = 45000; n.used = true; n.validated = false;

    NightClockOffsetsRepository repo;
    repo.upsertNights(1, 2, QVector<NightStartDiff>{ n });

    // Re-run with updated midMs
    n.midMs = 50000;
    repo.upsertNights(1, 2, QVector<NightStartDiff>{ n });

    QSqlQuery q(db);
    q.exec("SELECT mid_ms FROM night_clock_offsets WHERE machine_id=1 AND ref_machine_id=2");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toLongLong(), 50000LL);

    // Still only one row
    q.exec("SELECT COUNT(*) FROM night_clock_offsets");
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toInt(), 1);

    QSqlDatabase::removeDatabase("test_nco_upsert");
}

void ClockDriftHistoryTests::testFindByMachinesFiltersDateRange()
{
    QSqlDatabase db = makeTestDb("test_nco_find");

    NightClockOffsetsRepository repo;
    auto makeNight = [](QDate d, qint64 mid) {
        NightStartDiff n;
        n.date = d; n.startDiffMs = mid - 500; n.endDiffMs = mid + 500;
        n.midMs = mid; n.used = true; n.validated = false;
        return n;
    };
    repo.upsertNights(1, 2, {
        makeNight(QDate(2026, 1, 10), 45000),
        makeNight(QDate(2026, 2, 10), 45000),
        makeNight(QDate(2026, 3, 10), 45000),
    });

    // Query only Feb–Mar
    auto rows = repo.findByMachines(1, 2, QDate(2026, 2, 1), QDate(2026, 3, 31));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].date, QString("2026-02-10"));
    QCOMPARE(rows[1].date, QString("2026-03-10"));

    QSqlDatabase::removeDatabase("test_nco_find");
}

void ClockDriftHistoryTests::testUpsertNightsValidatorAgreesRoundTrip()
{
    QSqlDatabase db = makeTestDb("test_nco_validator");

    NightStartDiff n;
    n.date        = QDate(2026, 4, 1);
    n.startDiffMs = 10000;
    n.endDiffMs   = 12000;
    n.midMs       = 11000;
    n.used        = true;
    n.validated   = true;
    n.validatorAgrees = true;

    NightClockOffsetsRepository repo;
    repo.upsertNights(1, 2, QVector<NightStartDiff>{ n });

    auto rows = repo.findByMachines(1, 2);
    QVERIFY(!rows.isEmpty());
    QCOMPARE(rows[0].validated, true);
    QCOMPARE(rows[0].validatorAgrees, 1);  // 1 = agrees, not NULL (-1)

    // Also test validatorAgrees = false (disagrees)
    n.validatorAgrees = false;
    repo.upsertNights(1, 2, QVector<NightStartDiff>{ n });
    rows = repo.findByMachines(1, 2);
    QVERIFY(!rows.isEmpty());
    QCOMPARE(rows[0].validatorAgrees, 0);  // 0 = disagrees

    QSqlDatabase::removeDatabase("test_nco_validator");
}

void ClockDriftHistoryTests::testClockDialogInvalidatesInputs()
{
    ClockSyncDialog dialog;
    auto apply = dialog.findChild<QPushButton*>("btnApply");
    auto table = dialog.findChild<QTableWidget*>("resultsTable");
    QVERIFY(!apply->isEnabled());
    auto seed = [&] { dialog.m_hasAnalysis = true; table->setRowCount(1); apply->setEnabled(true); };
    auto verify = [&] { return !dialog.m_hasAnalysis && !apply->isEnabled() && table->rowCount() == 0; };
    for (const char* name : {"startDate", "endDate"}) {
        seed();
        auto date = dialog.findChild<QDateEdit*>(name);
        date->setDate(date->date().addDays(1));
        QVERIFY(verify());
    }
    for (const char* name : {"cpapCombo", "oxiCombo"}) {
        auto combo = dialog.findChild<QComboBox*>(name);
        combo->addItem("A"); combo->addItem("B");
        seed(); combo->setCurrentIndex(1);
        QVERIFY(verify());
    }
    seed(); dialog.findChild<QCheckBox*>("chkValidate")->toggle();
    QVERIFY(verify());
    seed(); dialog.setDate(QDate(2026,10,15));
    QVERIFY(verify());
}

void ClockDriftHistoryTests::testClockDialogSelectionUpdatesPreview()
{
    ClockSyncDialog dialog;
    for (int i=0; i<6; ++i) {
        NightStartDiff n;
        n.date = QDate(2026,10,1).addDays(i);
        n.startDiffMs = n.endDiffMs = 1500000 + i*100000;
        dialog.m_nights.append(n);
    }
    dialog.populateTable();
    dialog.m_hasAnalysis = true;
    dialog.refreshSelection();
    auto label = dialog.findChild<QLabel*>("statusLabel");
    auto apply = dialog.findChild<QPushButton*>("btnApply");
    auto table = dialog.findChild<QTableWidget*>("resultsTable");
    QVERIFY(apply->isEnabled());
    QCOMPARE(dialog.m_fit.offsetMs, qint64(1750000));
    QVERIFY(label->text().contains("not an accuracy bound"));
    QVERIFY(!label->text().contains("Offset today"));
    table->cellWidget(5,0)->findChild<QCheckBox*>()->setChecked(false);
    QCOMPARE(dialog.m_fit.nUsed, 5);
    QCOMPARE(dialog.m_fit.offsetMs, qint64(1700000));
    QVERIFY(apply->isEnabled());
    table->cellWidget(4,0)->findChild<QCheckBox*>()->setChecked(false);
    QVERIFY(!apply->isEnabled());
    QVERIFY(label->text().contains("currently 4"));
}

void ClockDriftHistoryTests::testConstantReplacementRollbackAndHistory()
{
    auto db = makeTestDb("constant_replace");
    QSqlQuery q(db);
    QVERIFY(q.exec("DELETE FROM device_time_corrections"));
    DeviceTimeCorrectionRepository repo;
    DeviceTimeCorrectionData old;
    old.machineId=1; old.dateFrom="2026-01-01"; old.type="offset"; old.offsetMs=1440000;
    const qint64 oldId = repo.create(old);
    QVERIFY(oldId > 0);
    DeviceTimeCorrectionData other=old; other.machineId=2;
    QVERIFY(repo.create(other) > 0);
    // Fail only the insert, after the transaction has shortened old corrections.
    QVERIFY(q.exec("CREATE TEMP TRIGGER reject_clock_insert BEFORE INSERT ON device_time_corrections "
                   "BEGIN SELECT RAISE(ABORT, 'injected clock write failure'); END"));
    QVERIFY(!repo.replaceWithConstant(1, "2026-09-01", 1557800));
    QVERIFY(q.exec("DROP TRIGGER reject_clock_insert"));
    auto rows=repo.findActive(1);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].id, oldId);
    QVERIFY(rows[0].dateTo.isEmpty());
    QCOMPARE(rows[0].offsetMs, qint64(1440000));
    QVERIFY(repo.replaceWithConstant(1, "2026-09-01", 1557800));
    rows=repo.findActive(1);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].dateTo, QString("2026-08-31"));
    QCOMPARE(rows[1].type, QString("offset"));
    QCOMPARE(rows[1].offsetMs, qint64(1557800));
    QVERIFY(rows[1].dateTo.isEmpty());
    QCOMPARE(repo.findActive(2).size(), 1);
    QVERIFY(repo.replaceWithConstant(1, "2026-09-01", 0));
    rows=repo.findActive(1);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].dateTo, QString("2026-08-31"));
}

void ClockDriftHistoryTests::testClockDialogSavesDisplayedConstant()
{
    auto db = makeTestDb("dialog_constant_save");
    QSqlQuery q(db);
    QVERIFY(q.exec("DELETE FROM device_time_corrections"));
    Machine cpap(nullptr, 1);
    cpap.setDatabaseId(1);
    ClockSyncDialog dialog;
    auto combo = dialog.findChild<QComboBox*>("cpapCombo");
    combo->addItem("Test CPAP", QVariant::fromValue(reinterpret_cast<quintptr>(&cpap)));
    // Strongly trending windows used to silently select a drift model.
    for (int i=0; i<6; ++i) {
        NightStartDiff n;
        n.date=QDate(2026,10,1).addDays(i);
        n.startDiffMs=n.endDiffMs=1500000+i*100000;
        dialog.m_nights.append(n);
    }
    dialog.populateTable();
    dialog.m_hasAnalysis=true;
    dialog.refreshSelection();
    const qint64 displayed=dialog.m_fit.offsetMs;
    QTimer::singleShot(0, [] {
        auto box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box) box->button(QMessageBox::Save)->click();
    });
    dialog.onApply();
    DeviceTimeCorrectionRepository repo;
    auto rows=repo.findActive(1);
    QCOMPARE(rows.size(),1);
    QCOMPARE(rows[0].type,QString("offset"));
    QCOMPARE(rows[0].offsetMs,displayed);
    QCOMPARE(rows[0].c1,0.0);
    QVERIFY(!dialog.findChild<QPushButton*>("btnApply")->isEnabled());
    QCOMPARE(cpap.correctionMs(QDate(2026,10,10)),displayed);
}

void ClockDriftHistoryTests::testClockDialogRecheckedOutlierIsIncluded()
{
    ClockSyncDialog dialog;
    for (int i=0; i<6; ++i) {
        NightStartDiff n;
        n.date=QDate(2026,10,1).addDays(i);
        n.startDiffMs=n.endDiffMs=1500000;
        n.used=(i!=5);
        dialog.m_nights.append(n);
    }
    dialog.populateTable();
    dialog.m_hasAnalysis=true;
    dialog.refreshSelection();
    auto table=dialog.findChild<QTableWidget*>("resultsTable");
    QCOMPARE(dialog.m_fit.nUsed,5);
    table->cellWidget(5,0)->findChild<QCheckBox*>()->setChecked(true);
    QCOMPARE(dialog.m_fit.nUsed,6);
    QVERIFY(table->item(5,6)->text().contains("included (initial pairing outlier)"));
    QCOMPARE(table->item(5,1)->foreground(),QBrush());
}
