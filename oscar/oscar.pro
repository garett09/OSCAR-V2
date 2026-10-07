#-------------------------------------------------
#
# Project created by QtCreator 2011-06-20T22:05:30
#
#-------------------------------------------------

message(Platform is $$QMAKESPEC )
#message("CONFIG: $$CONFIG")

# the debug option is set by default by qmake
# to enable debug builds from the command line a new option "crash" is enable
# crash was used since debug mode is always used to identify the cause of crashes.
# a better name might be possible.
# this can be enabled by CONFIG += crash in the qmake make.
# qmake <buldFolderPath> OSCAR-code/OSCAR_QT.pro "CONFIG+=crash"

contains(CONFIG, crash) {
    message("DEBUG BUILDS - set by OPTION  'crash' ")
}

lessThan(QT_MAJOR_VERSION,6) {
    error("You need Qt 6 or newer to build OSCAR. 6.10 is recommended.");
}

# get rid of the help browser, at least for now
DEFINES += helpless

QT += core gui network xml printsupport serialport sql widgets help concurrent
contains(DEFINES, helpless) {
    QT -= help
}

DEFINES += QT_DEPRECATED_WARNINGS

# Enable this to turn off Check for Updates feature
# DEFINES += NO_CHECKUPDATES

#OSCAR requires OpenGL 2.0 support to run smoothly
#On platforms where it's not available, it can still be built to work
#provided the BrokenGL DEFINES flag is passed to qmake (eg, qmake [specs] /path/to/OSCAR_QT.pro DEFINES+=BrokenGL) (hint, Projects button on the left)
contains(DEFINES, NoGL) {
    message("Building with QWidget gGraphView to support systems without ANY OpenGL")
    DEFINES += BROKEN_OPENGL_BUILD
    DEFINES += NO_OPENGL_BUILD
} else:contains(DEFINES, BrokenGL) {
    DEFINES += BROKEN_OPENGL_BUILD
    message("Building with QWidget gGraphView to support systems with legacy graphics")
    DEFINES-=BrokenGL
} else {
    QT += opengl
    QT += openglwidgets
    message("Building with regular OpenGL gGraphView")
}

## qt6 requires c++17
greaterThan(QT_MAJOR_VERSION, 5) {
    CONFIG += c++17
## Added for Qt6-Clang compiler - Crimson Nape 25-11-17
    CONFIG += warn_off
    QMAKE_CXXFLAGS += -Wno-nonportable-include-path

}

DEFINES += LOCK_RESMED_SESSIONS
CONFIG += rtti
!contains(CONFIG, crash) {
    CONFIG -= debug_and_release
} else {
    # This requires CONFIG = debug set in qmake command
    #message("DEBUG BUILDS - set by OPTION  'crash' ")

    CONFIG -= crash  # Enable debug mode
    CONFIG += debug  # Enable debug mode
    # Debugging Configurations
    CONFIG -= release  # Ensure release mode is disabled

    # Debugging Symbols
    QMAKE_CXXFLAGS += -g3  # Most comprehensive debugging symbols
    QMAKE_CXXFLAGS += -ggdb  # GDB-specific debug information

    # Optimization Level
    #QMAKE_CXXFLAGS_DEBUG += -O0  # Disable optimizations completely

    # Additional Debugging Flags
    DEFINES += QT_ENABLE_GLIB  # Enable additional debugging support
    DEFINES += QT_MESSAGELOGCONTEXT  # Include source file and line number in debug messages
}

contains(DEFINES, STATIC) {
    static {
        CONFIG += static
        QTPLUGIN += qgif qpng

        message("Static build.")
    }
}

DEFINES += STEADY_BREATHING
#### STEADY_BREATHING_ENHANCED_TESTING should NOT be defined for offical builds.
#### STEADY_BREATHING_ENHANCED_TESTING requires STEADY_BREATHING
####  DEFINES += STEADY_BREATHING_ENHANCED_TESTING


win32 {
    TARGET = OSCAR
} else {
    TARGET = OSCAR20
}

unix:!macx:!haiku {
    TARGET.path=/usr/bin
}

TEMPLATE = app

# The rule's target must be spelled exactly as qmake spells version.o's dependency on
# git_info.h: relative to the build directory (e.g. ../../git_info.h in a shadow build).
# Otherwise make treats them as different files, judges version.o against the old
# git_info.h, and the version tag lags one build behind a change in git state.
GIT_INFO_FILE = $$relative_path($$_PRO_FILE_PWD_/git_info.h, $$OUT_PWD)
gitinfotarget.target = $$GIT_INFO_FILE
gitinfotarget.depends = FORCE

win32 {
    system("cmd /c \"$$_PRO_FILE_PWD_/update_gitinfo.bat\"")
    message("Updating gitinfo.h for Windows build")
    gitinfotarget.commands = "cmd /c \"$$_PRO_FILE_PWD_/update_gitinfo.bat\""
#   system("$$_PRO_FILE_PWD_/update_gitinfo.bat");
#   message("Updating gitinfo.h for Windows build")
#   gitinfotarget.commands = "$$_PRO_FILE_PWD_/update_gitinfo.bat"
} else {
    system("/bin/bash $$_PRO_FILE_PWD_/update_gitinfo.sh");
    message("Updating gitinfo.h for non-Windows build")
    gitinfotarget.commands = "/bin/bash $$_PRO_FILE_PWD_/update_gitinfo.sh"
}

PRE_TARGETDEPS += $$GIT_INFO_FILE
QMAKE_EXTRA_TARGETS += gitinfotarget

# Regenerate exports/system_report_strings.cpp from docs/system_reports.orf when the .orf changes.
orf_strings.target  = $$_PRO_FILE_PWD_/exports/system_report_strings.cpp
orf_strings.depends = $$_PRO_FILE_PWD_/docs/system_reports.orf
win32 {
    orf_strings.commands = python $$_PRO_FILE_PWD_/../Tools/gen_system_report_strings.py
} else {
    orf_strings.commands = python3 $$_PRO_FILE_PWD_/../Tools/gen_system_report_strings.py
}
QMAKE_EXTRA_TARGETS += orf_strings
PRE_TARGETDEPS += $$_PRO_FILE_PWD_/exports/system_report_strings.cpp

!contains(DEFINES, helpless) {
#Build the help documentation
    message("Generating help files");
    qtPrepareTool(QCOLGENERATOR, qcollectiongenerator)

    command=$$QCOLGENERATOR $$PWD/help/index.qhcp -o $$PWD/help/index.qhc
    system($$command)|error("Failed to run: $$command")
    message("Finished generating help files");
}

QMAKE_TARGET_PRODUCT = OSCAR
QMAKE_TARGET_COMPANY = The OSCAR Team
QMAKE_TARGET_COPYRIGHT = © 2019-2026 The OSCAR Team
QMAKE_TARGET_DESCRIPTION = "OpenSource CPAP Analysis Reporter"
_VERSION_FILE = $$cat(./VERSION)
VERSION = $$section(_VERSION_FILE, '"', 1, 1)
win32 {
    VERSION = $$section(VERSION, '-', 0, 0)
}
RC_ICONS = ./icons/logo.ico

macx  {
  QMAKE_TARGET_BUNDLE_PREFIX = "org.oscar-team"
# QMAKE_MACOSX_DEPLOYMENT_TARGET = 10.13
  LIBS             += -lz
  ICON              = icons/OSCAR.icns
} else:haiku {
    LIBS            += -lz -lGLU
    DEFINES         += _TTY_POSIX_
} else:unix {
    LIBS            += -lX11 -lz -lGLU
    DEFINES         += _TTY_POSIX_
} else:win32 {
    DEFINES          += WINVER=0x0501 # needed for mingw to pull in appropriate dbt business...probably a better way to do this
    LIBS             += -lsetupapi
    LIBS             += -ldbghelp # needed for call stack with names rather than addresses

    INCLUDEPATH += $$PWD
    INCLUDEPATH += $$[QT_INSTALL_PREFIX]/../src/qtbase/src/3rdparty/zlib

    if (*-msvc*):!equals(TEMPLATE_PREFIX, "vc") {
        LIBS += -ladvapi32
    } else {
        # MingW needs this
        LIBS += -lz
    }

    if (*-msvc*) {
        CONFIG += precompile_header
        PRECOMPILED_HEADER = pch.h
        HEADERS += pch.h

    }

    CONFIG(release, debug|release) {
        contains(DEFINES, OfficialBuild) {
           QMAKE_POST_LINK += "$$PWD/../../scripts/release_tool.sh --testing --source \"$$PWD/..\" --binary \"$${OUT_PWD}/$${TARGET}.exe\""
        }
    }
}

TRANSLATIONS = $$files($$PWD/../Translations/*.ts)
# EXTRA_TRANSLATIONS are compiled by lrelease but excluded from lupdate scanning.
# The qt/ files are pre-translated Qt-internal strings that must not be modified by lupdate.
# Note: qmake only acts on EXTRA_TRANSLATIONS when "CONFIG += lrelease" is enabled, which
# this project does not use.  The loop below therefore compiles and deploys them explicitly,
# alongside TRANSLATIONS.  Without it the oscar_qt_*.qm catalogues are never produced and
# every Qt-internal string (QFileDialog labels, standard dialog buttons) stays in English.
EXTRA_TRANSLATIONS = $$files($$PWD/../Translations/qt/*.ts)

# qtPrepareTool(LRELEASE, lrelease)
LRELEASE = $$[QT_INSTALL_BINS]/lrelease
win32: LRELEASE = $${LRELEASE}.exe

ALL_TRANSLATIONS = $${TRANSLATIONS} $${EXTRA_TRANSLATIONS}

for(file, ALL_TRANSLATIONS) {

    qmfile = $$absolute_path($$basename(file), $$PWD/translations/)
    qmfile ~= s,.ts$,.qm,

    qmdir = $$PWD/translations
    !exists($$qmdir) {
        mkpath($$qmdir)|error("Aborting.")
    }
    qmout = $$qmfile
    command = $$quote($$LRELEASE) -removeidentical $$quote($$file) -qm $$quote($$qmfile)
    system($$command)|error("Failed to run: $$command")
    TRANSLATIONS_FILES += $$qmfile
}

HTML_FILES = $$files($$PWD/../Htmldocs/*.html)

#copy the Translation and Help files to where the test binary wants them
message("Setting up Translations & Help Transfers")
macx {
    !contains(DEFINES, helpless) {
        HelpFiles.files = $$files($$PWD/help/*.qch)
        HelpFiles.path = Contents/Resources/Help
        QMAKE_BUNDLE_DATA += HelpFiles
    }

# Removed because we are not using QT's translation files
#    QtTransFiles.files = $$files($$[QT_INSTALL_TRANSLATIONS]/qt*.qm)
#    QtTransFiles.path = Contents/translations
#    QMAKE_BUNDLE_DATA += QtTransFiles

    TransFiles.files = $${TRANSLATIONS_FILES}
    TransFiles.path = Contents/Resources/translations
    QMAKE_BUNDLE_DATA += TransFiles

    HtmlFiles.files = $${HTML_FILES}
    HtmlFiles.path = Contents/Resources/html
    QMAKE_BUNDLE_DATA += HtmlFiles
} else {
    !contains(DEFINES, helpless) {
        HELPDIR = $$OUT_PWD/Help
        HELP_FILES += $$PWD/help/*.qch
    }
    DDIR = $$OUT_PWD/Translations
    HTMLDIR = $$OUT_PWD/Html

    html_copies.files = $$HTML_FILES
    html_copies.path = $$HTMLDIR
    COPIES += html_copies

    TRANS_FILES = $${TRANSLATIONS_FILES}

    win32 {
        TRANS_FILES_WIN = $${TRANS_FILES}
        TRANS_FILES_WIN ~= s,/,\\,g
        DDIR ~= s,/,\\,g
        !exists($$quote($$DDIR)): system(mkdir $$quote($$DDIR))
        for(FILE,TRANS_FILES_WIN) {
            system(xcopy /y $$quote($$FILE) $$quote($$DDIR))
        }

        !contains(DEFINES, helpless) {
            HELP_FILES_WIN = $${HELP_FILES}
            HELP_FILES_WIN ~= s,/,\\,g
            HELPDIR ~= s,/,\\,g
            !exists($$quote($$HELPDIR)): system(mkdir $$quote($$HELPDIR))
            for(FILE,HELP_FILES_WIN) {
                system(xcopy /y $$quote($$FILE) $$quote($$HELPDIR))
            }
        }
    } else {
        system(mkdir -p $$quote($$DDIR))
        for(FILE,TRANS_FILES) {
            system(cp $$quote($$FILE) $$quote($$DDIR))
        }

        !contains(DEFINES, helpless) {
            system(mkdir -p $$quote($$HELPDIR))
            for(FILE,HELP_FILES) {
                system(cp $$quote($$FILE) $$quote($$HELPDIR))
            }
        }
    }
}

SOURCES += \
    aboutdialog.cpp \
    checkupdates.cpp \
    common_gui.cpp \
    cprogressbar.cpp \
    csv.cpp \
    daily.cpp \
    dailySearchTab.cpp \
    devicetimecorrectiondialog.cpp \
    timealignmentwelcomedialog.cpp \
    driftanalysisdialog.cpp \
    driftplotwidget.cpp \
    backupdialog.cpp \
    restoredialog.cpp \
    sharedialog.cpp \
    purgerangedaysdialog.cpp \
    sqleditor.cpp \
    reportmanager.cpp \
    reportvarietyeditor.cpp \
    datafolderdialog.cpp \
    importprofile.cpp \
    profileimporter.cpp \
    logger.cpp \
    main.cpp \
    mainwindow.cpp \
    newprofile.cpp \
    notifyMessageBox.cpp \
    overview.cpp \
    oximeterimport.cpp \
    preferencesdialog.cpp \
    profileselector.cpp \
    rawdata.cpp \
    reports.cpp \
    saveGraphLayoutSettings.cpp \
    sessionbar.cpp \
    speedcheck.cpp \
    staticQMessageBox.cpp \
    statistics.cpp \
    translation.cpp \
    version.cpp \
    welcome.cpp \
    zip.cpp \
    Graphs/gAHIChart.cpp \
    Graphs/gdailysummary.cpp \
    Graphs/gFlagsLine.cpp \
    Graphs/gSessionBarLayer.cpp \
    Graphs/gFooBar.cpp \
    Graphs/gGraph.cpp \
    Graphs/gGraphView.cpp \
    Graphs/glcommon.cpp \
    Graphs/gLineChart.cpp \
    Graphs/gLineOverlay.cpp \
    Graphs/gOverviewGraph.cpp \
    Graphs/gPressureChart.cpp \
    Graphs/gSegmentChart.cpp \
    Graphs/gSessionTimesChart.cpp \
    Graphs/gSleepStageChart.cpp \
    Graphs/gspacer.cpp \
    Graphs/gStatsLine.cpp \
    Graphs/gSummaryChart.cpp \
    Graphs/gTTIAChart.cpp \
    Graphs/gUsageChart.cpp \
    Graphs/gXAxis.cpp \
    Graphs/gYAxis.cpp \
    Graphs/layer.cpp \
    Graphs/MinutesAtPressure.cpp \
    SleepLib/appsettings.cpp \
    SleepLib/calcs.cpp \
    SleepLib/common.cpp \
    SleepLib/day.cpp \
    SleepLib/deviceconnection.cpp \
    SleepLib/event.cpp \
    SleepLib/importcontext.cpp \
    SleepLib/journal.cpp \
    SleepLib/loader_plugins/applehealthDataParsing.cpp \
    SleepLib/loader_plugins/applehealth_loader.cpp \
    SleepLib/loader_plugins/apexDataParsing.cpp \
    SleepLib/loader_plugins/apex_loader.cpp \
    SleepLib/loader_plugins/bmcDataParsing.cpp \
    SleepLib/loader_plugins/bmcG3xDataParsing.cpp \
    SleepLib/loader_plugins/bmc_loader.cpp \
    SleepLib/loader_plugins/bmcg3x_loader.cpp \
    SleepLib/loader_plugins/cms50f37_loader.cpp \
    SleepLib/loader_plugins/cms50_loader.cpp \
    SleepLib/loader_plugins/dreem_loader.cpp \
    SleepLib/loader_plugins/edfparser.cpp \
    SleepLib/loader_plugins/icon_loader.cpp \
    SleepLib/loader_plugins/intellipap_loader.cpp \
    SleepLib/loader_plugins/md300w1_loader.cpp \
    SleepLib/loader_plugins/mseries_loader.cpp \
    SleepLib/loader_plugins/prisma_loader.cpp \
    SleepLib/loader_plugins/vrem_loader.cpp \
    SleepLib/loader_plugins/prs1_loader.cpp \
    SleepLib/loader_plugins/prs1_parser_asv.cpp \
    SleepLib/loader_plugins/prs1_parser.cpp \
    SleepLib/loader_plugins/prs1_parser_vent.cpp \
    SleepLib/loader_plugins/prs1_parser_xpap.cpp \
    SleepLib/loader_plugins/resmed_EDFinfo.cpp \
    SleepLib/loader_plugins/resmed_loader.cpp \
    SleepLib/loader_plugins/resvent_loader.cpp \
    SleepLib/loader_plugins/sefamDataParsing.cpp \
    SleepLib/loader_plugins/sefam_loader.cpp \
    SleepLib/loader_plugins/sleepstyle_EDFinfo.cpp \
    SleepLib/loader_plugins/sleepstyle_loader.cpp \
    SleepLib/loader_plugins/somnopose_loader.cpp \
    SleepLib/loader_plugins/viatom_loader.cpp \
    SleepLib/loader_plugins/weinmann_loader.cpp \
    SleepLib/loader_plugins/yuwell_loader.cpp \
    SleepLib/loader_plugins/zeo_loader.cpp \
    SleepLib/machine_common.cpp \
    SleepLib/machine.cpp \
    SleepLib/machine_loader.cpp \
    SleepLib/preferences.cpp \
    SleepLib/profiles.cpp \
    SleepLib/progressdialog.cpp \
    SleepLib/schema.cpp \
    SleepLib/serialoximeter.cpp \
    SleepLib/session.cpp \
    SleepLib/thirdparty/miniz.c \
    SleepLib/xmlreplay.cpp \
    database/database_manager.cpp \
    database/database_schema.cpp \
    database/profile_repository.cpp \
    database/machine_repository.cpp \
    database/migration_manager.cpp \
    database/user_info_repository.cpp \
    database/doctor_info_repository.cpp \
    database/preferences_repository.cpp \
    database/app_preferences_repository.cpp \
    database/graph_layouts_repository.cpp \
    database/session_repository.cpp \
    database/session_settings_repository.cpp \
    database/session_channels_repository.cpp \
    database/session_channel_values_repository.cpp \
    database/session_slices_repository.cpp \
    database/session_summaries_repository.cpp \
    database/channel_repository.cpp \
    database/channel_options_repository.cpp \
    database/daily_summary_repository.cpp \
    database/event_list_repository.cpp \
    database/event_data_repository.cpp \
    database/respiratory_events_repository.cpp \
    database/device_time_correction_repository.cpp \
    database/report_repository.cpp \
    database/reports_initializer.cpp \
    database/report_contents_repository.cpp \
    database/report_tree_repository.cpp \
    database/report_tree_model.cpp \
    exports/report_exporter.cpp \
    exports/system_report_strings.cpp \
    exports/journalnotesdialog.cpp \
    exports/exportcsv.cpp \
    database/orf_file_io.cpp \
    database/backup/backup_manifest.cpp \
    database/backup/sql_exporter.cpp \
    database/backup/profile_backup.cpp \
    database/backup/profile_restore.cpp \
    database/recent_databases.cpp \
    database/database_delete_dialog.cpp \
    network/cloud_downloader.cpp \
    network/cloud_uploader.cpp \
    network/oauth2_handler.cpp \
    network/dropbox_uploader.cpp \
    network/googledrive_uploader.cpp \
    network/onedrive_uploader.cpp
!contains(DEFINES, helpless) {
    SOURCES += help.cpp
}

# The crypto libraries need to be optimized to avoid a 5x slowdown or worse.
# Don't use this for everything, as it interferes with debugging.
SOURCES_OPTIMIZE = \
    SleepLib/thirdparty/botan_all.cpp \
    SleepLib/crypto.cpp
optimize.name = optimize
optimize.input = SOURCES_OPTIMIZE
optimize.dependency_type = TYPE_C
optimize.variable_out = OBJECTS
optimize.output = ${QMAKE_FILE_IN_BASE}$${first(QMAKE_EXT_OBJ)}
optimize.commands = $${QMAKE_CXX} -c $(CXXFLAGS) -O3 $(INCPATH) -o ${QMAKE_FILE_OUT} ${QMAKE_FILE_IN}
QMAKE_EXTRA_COMPILERS += optimize

HEADERS  += \
    SleepLib/performance_timer.h \
    checkupdates.h \
    database/reports_initializer.h \
    notifyMessageBox.h \
    dailySearchTab.h \
    daily.h \
    combocheckdelegate.h \
    saveGraphLayoutSettings.h \
    overview.h \
    common_gui.h \
    cprogressbar.h \
    borrowingtimeedit.h \
    devicetimecorrectiondialog.h \
    timealignmentwelcomedialog.h \
    driftanalysisdialog.h \
    driftplotwidget.h \
    backupdialog.h \
    restoredialog.h \
    sharedialog.h \
    purgerangedaysdialog.h \
    sqleditor.h \
    reportmanager.h \
    reportvarietyeditor.h \
    datafolderdialog.h \
    importprofile.h \
    profileimporter.h \
    mainwindow.h \
    newprofile.h \
    preferencesdialog.h \
#    psettings.h \
    reports.h \
    sessionbar.h \
    speedcheck.h \
#    updateparser.h \
    version.h \
    VERSION \
    Graphs/gFlagsLine.h \
    Graphs/gSessionBarLayer.h \
    Graphs/gFooBar.h \
    Graphs/gGraph.h \
    Graphs/gGraphView.h \
    Graphs/glcommon.h \
    Graphs/gLineChart.h \
    Graphs/gLineOverlay.h \
    Graphs/gSegmentChart.h\
    Graphs/gspacer.h \
    Graphs/gStatsLine.h \
    Graphs/gSummaryChart.h \
    Graphs/gAHIChart.h \
    Graphs/gTTIAChart.h \
    Graphs/gUsageChart.h \
    Graphs/gSessionTimesChart.h \
    Graphs/gSleepStageChart.h \
    Graphs/gPressureChart.h \
    Graphs/gOverviewGraph.h \
    Graphs/gXAxis.h \
    Graphs/gYAxis.h \
    Graphs/layer.h \
    SleepLib/calcs.h \
    SleepLib/common.h \
    SleepLib/day.h \
    SleepLib/event.h \
    SleepLib/machine.h \
    SleepLib/machine_common.h \
    SleepLib/machine_loader.h \
    SleepLib/importcontext.h \
    SleepLib/preferences.h \
    SleepLib/profiles.h \
    SleepLib/schema.h \
    SleepLib/session.h \
    SleepLib/loader_plugins/applehealthDataParsing.h \
    SleepLib/loader_plugins/applehealth_loader.h \
    SleepLib/loader_plugins/apexDataParsing.h \
    SleepLib/loader_plugins/apex_loader.h \
    SleepLib/loader_plugins/bmcDataParsing.h \
    SleepLib/loader_plugins/bmcG3xDataParsing.h \
    SleepLib/loader_plugins/bmc_loader.h \
    SleepLib/loader_plugins/bmcg3x_loader.h \
    SleepLib/loader_plugins/cms50_loader.h \
    SleepLib/loader_plugins/dreem_loader.h \
    SleepLib/loader_plugins/icon_loader.h \
    SleepLib/loader_plugins/sleepstyle_loader.h \
    SleepLib/loader_plugins/sleepstyle_EDFinfo.h \
    SleepLib/loader_plugins/intellipap_loader.h \
    SleepLib/loader_plugins/mseries_loader.h \
    SleepLib/loader_plugins/prisma_loader.h \
    SleepLib/loader_plugins/vrem_loader.h \
    SleepLib/loader_plugins/prs1_loader.h \
    SleepLib/loader_plugins/prs1_parser.h \
    SleepLib/loader_plugins/resmed_loader.h \
    SleepLib/loader_plugins/resmed_EDFinfo.h \
    SleepLib/loader_plugins/somnopose_loader.h \
    SleepLib/loader_plugins/viatom_loader.h \
    SleepLib/loader_plugins/yuwell_loader.h \
    SleepLib/loader_plugins/zeo_loader.h \
    SleepLib/loader_plugins/resvent_loader.h \
    SleepLib/loader_plugins/sefamDataParsing.h \
    SleepLib/loader_plugins/sefam_loader.h \
    SleepLib/thirdparty/botan_all.h \
    SleepLib/thirdparty/botan_windows.h \
    SleepLib/thirdparty/botan_linux.h \
    SleepLib/thirdparty/botan_macos.h \
    SleepLib/crypto.h \
    zip.h \
    SleepLib/thirdparty/miniz.h \
    csv.h \
    rawdata.h \
    translation.h \
    statistics.h \
    oximeterimport.h \
    SleepLib/deviceconnection.h \
    SleepLib/xmlreplay.h \
    SleepLib/serialoximeter.h \
    SleepLib/loader_plugins/md300w1_loader.h \
    logger.h \
    SleepLib/loader_plugins/weinmann_loader.h \
    Graphs/gdailysummary.h \
    Graphs/MinutesAtPressure.h \
    SleepLib/journal.h \
    SleepLib/progressdialog.h \
    SleepLib/loader_plugins/cms50f37_loader.h \
    profileselector.h \
    SleepLib/appsettings.h \
    SleepLib/loader_plugins/edfparser.h \
    aboutdialog.h \
    welcome.h \
    mytextbrowser.h \
    staticQMessageBox.h \
    git_info.h \
    database/database_manager.h \
    database/database_schema.h \
    database/reports_initializer.h \
    database/profile_repository.h \
    database/machine_repository.h \
    database/migration_manager.h \
    database/user_info_repository.h \
    database/doctor_info_repository.h \
    database/preferences_repository.h \
    database/app_preferences_repository.h \
    database/graph_layouts_repository.h \
    database/session_repository.h \
    database/session_settings_repository.h \
    database/session_channels_repository.h \
    database/session_channel_values_repository.h \
    database/session_slices_repository.h \
    database/session_summaries_repository.h \
    database/channel_repository.h \
    database/channel_options_repository.h \
    database/daily_summary_repository.h \
    database/event_list_repository.h \
    database/event_data_repository.h \
    database/respiratory_events_repository.h \
    database/device_time_correction_repository.h \
    database/report_repository.h \
    database/report_contents_repository.h \
    database/report_tree_repository.h \
    database/report_tree_model.h \
    exports/report_exporter.h \
    exports/journalnotesdialog.h \
    exports/exportcsv.h \
    database/orf_file_io.h \
    database/backup/backup_manifest.h \
    database/backup/sql_exporter.h \
    database/backup/profile_backup.h \
    database/backup/profile_restore.h \
    database/recent_databases.h \
    database/database_delete_dialog.h \
    network/cloud_downloader.h \
    network/cloud_uploader.h \
    network/oauth2_handler.h \
    network/dropbox_uploader.h \
    network/googledrive_uploader.h \
    network/onedrive_uploader.h
!contains(DEFINES, helpless) {
    HEADERS += help.h
}

FORMS += \
    daily.ui \
    overview.ui \
    mainwindow.ui \
    oximetry.ui \
    preferencesdialog.ui \
    newprofile.ui \
    devicetimecorrectiondialog.ui \
    driftanalysisdialog.ui \
    backupdialog.ui \
    restoredialog.ui \
    sharedialog.ui \
    sqleditor.ui \
    reportmanager.ui \
    reportvarietyeditor.ui \
#    UpdaterWindow.ui \
    importprofile.ui \
    oximeterimport.ui \
    profileselector.ui \
    aboutdialog.ui \
    welcome.ui \
    exports/journalnotesdialog.ui \
    exports/exportcsv.ui
!contains(DEFINES, helpless) {
    FORMS += help.ui
}

RESOURCES += \
    Resources.qrc

OTHER_FILES += \
    docs/index.html \
    docs/schema.xml \
    docs/graphs.xml \
    docs/channels.xml \
    docs/startup_tips.txt \
    docs/countries.txt \
    docs/system_reports.orf \
    docs/tz.txt \
    ../LICENSE.txt \
    docs/tooltips.css \
    docs/script.js \
    ../update.xml \
    docs/changelog.txt \
    docs/intro.html \
    docs/statistics.xml \
    update_gitinfo.bat \
    update_gitinfo.sh

DISTFILES += \
    ../README
!contains(DEFINES, helpless) {
DISTFILES += help/default.css \
    help/help_en/daily.html \
    help/help_en/glossary.html \
    help/help_en/import.html \
    help/help_en/index.html \
    help/help_en/overview.html \
    help/help_en/oximetry.html \
    help/help_en/statistics.html \
    help/help_en/supported.html \
    help/help_en/gettingstarted.html \
    help/help_en/tipsntricks.html \
    help/help_en/faq.html \
    help/help_nl/daily.html \
    help/help_nl/faq.html \
    help/help_nl/gettingstarted.html \
    help/help_nl/glossary.html \
    help/help_nl/import.html \
    help/help_nl/index.html \
    help/help_nl/overview.html \
    help/help_nl/oximetry.html \
    help/help_nl/statistics.html \
    help/help_nl/supported.html \
    help/help_nl/tipsntricks.html \
    help/help_en/reportingbugs.html \
    help/help_nl/OSCAR_Guide_nl.qhp \
    help/help_en/OSCAR_Guide_en.qhp \
    help/index.qhcp
}

message("CXXFLAGS pre-mods $$QMAKE_CXXFLAGS ")

# Always treat warnings as errors, even (especially!) in release
QMAKE_CFLAGS += -Werror
QMAKE_CXXFLAGS += -Werror

gcc | clang {
    COMPILER_VERSION = $$system($$QMAKE_CXX " -dumpversion")
    COMPILER_MAJOR = $$split(COMPILER_VERSION, ".")
    COMPILER_MAJOR = $$first(COMPILER_MAJOR)

    message("$$QMAKE_CXX major version $$COMPILER_MAJOR")
}

if (false) {
  if (equals(QT_MAJOR_VERSION,6)) {
   if (greaterThan(QT_MINOR_VERSION,8)) {
    ## allow deprecated warings for TESTING
    message("ALLOW DEPRECATED WARNINGS FOR TESTING ")
    # Make deprecation warnings just warnings
    QMAKE_CFLAGS += -Wno-error=deprecated-declarations
    QMAKE_CXXFLAGS += -Wno-error=deprecated-declarations
} } }

message("CXXFLAGS post-mods $$QMAKE_CXXFLAGS ")
message("CXXFLAGS_WARN_ON $$QMAKE_CXXFLAGS_WARN_ON")

lessThan(QT_MAJOR_VERSION,5)|lessThan(QT_MINOR_VERSION,9) {
    QMAKE_CFLAGS += -Wno-error=strict-aliasing
    QMAKE_CXXFLAGS += -Wno-error=strict-aliasing
}

# Create a debug GUI build by adding "CONFIG+=memdebug" to your qmake command
memdebug {
    CONFIG += debug
    ## there is an error in qt. qlist.h uses an implicitly defined operator=
    ## allow this for debug
    QMAKE_CFLAGS += -Wno-error=deprecated-copy
    QMAKE_CXXFLAGS += -Wno-error=deprecated-copy
    !win32 {  # add memory checking on Linux and macOS debug builds
        QMAKE_CFLAGS += -g -Werror -fsanitize=address -fno-omit-frame-pointer -fno-common -fsanitize-address-use-after-scope
        lessThan(QT_MAJOR_VERSION,5)|lessThan(QT_MINOR_VERSION,9) {
            QMAKE_CFLAGS -= -fsanitize-address-use-after-scope
        }
        QMAKE_CXXFLAGS += -g -Werror -fsanitize=address -fno-omit-frame-pointer -fno-common -fsanitize-address-use-after-scope
        lessThan(QT_MAJOR_VERSION,5)|lessThan(QT_MINOR_VERSION,9) {
            QMAKE_CXXFLAGS -= -fsanitize-address-use-after-scope
        }
        QMAKE_LFLAGS += -fsanitize=address
    }
}

# Turn on unit testing by adding "CONFIG+=test" to your qmake command
test {
    TARGET = test
    DEFINES += UNITTEST_MODE

    QT += testlib
    QT -= gui
    CONFIG += console debug
    CONFIG -= app_bundle
    !win32 {  # add memory checking on Linux and macOS test builds
        QMAKE_CFLAGS += -Werror -fsanitize=address -fno-omit-frame-pointer -fno-common -fsanitize-address-use-after-scope
        lessThan(QT_MAJOR_VERSION,5)|lessThan(QT_MINOR_VERSION,9) {
            QMAKE_CFLAGS -= -fsanitize-address-use-after-scope
        }
        QMAKE_CXXFLAGS += -Werror -fsanitize=address -fno-omit-frame-pointer -fno-common -fsanitize-address-use-after-scope
        lessThan(QT_MAJOR_VERSION,5)|lessThan(QT_MINOR_VERSION,9) {
            QMAKE_CXXFLAGS -= -fsanitize-address-use-after-scope
        }
        QMAKE_LFLAGS += -fsanitize=address
    }

    SOURCES += \
        tests/apextests.cpp \
        tests/prs1tests.cpp \
        tests/rawdatatests.cpp \
        tests/resmedtests.cpp \
        tests/sessiontests.cpp \
        tests/versiontests.cpp \
        tests/viatomtests.cpp \
        tests/deviceconnectiontests.cpp \
        tests/cryptotests.cpp \
        tests/dreemtests.cpp \
        tests/eventstabtests.cpp \
        tests/zeotests.cpp \
        tests/applehealthtests.cpp \
        tests/machinetests.cpp

    HEADERS += \
        tests/AutoTest.h \
        tests/apextests.h \
        tests/prs1tests.h \
        tests/rawdatatests.h \
        tests/resmedtests.h \
        tests/sessiontests.h \
        tests/versiontests.h \
        tests/viatomtests.h \
        tests/deviceconnectiontests.h \
        tests/cryptotests.h \
        tests/dreemtests.h \
        tests/eventstabtests.h \
        tests/zeotests.h \
        tests/applehealthtests.h \
        tests/machinetests.h
}

macx {
    app_bundle {
        # On macOS put a custom Info.plist into the bundle that disables dark mode on Mojave.
        QMAKE_INFO_PLIST = "../Building/MacOS/Info.plist.in"

        # Add the git revision to the Info.plist.
        Info_plist.target = Info.plist
        Info_plist.depends = $${TARGET}.app/Contents/Info.plist
        Info_plist.commands = $$_PRO_FILE_PWD_/../Building/MacOS/finalize_plist $$_PRO_FILE_PWD_ $${TARGET}.app/Contents/Info.plist
        QMAKE_EXTRA_TARGETS += Info_plist
        PRE_TARGETDEPS += $$Info_plist.target
    }

    # Add a dist-mac target to build the distribution .dmg.
    QMAKE_EXTRA_TARGETS += dist-mac
    dist-mac.commands = QT_BIN=$$[QT_INSTALL_PREFIX]/bin $$_PRO_FILE_PWD_/../Building/MacOS/create_dmg $${TARGET} $${TARGET}.app $$_PRO_FILE_PWD_/../Building/MacOS/README.rtfd $$_PRO_FILE_PWD_/../Building/MacOS/background.png
    dist-mac.depends = $${TARGET}.app/Contents/MacOS/$${TARGET}
}
