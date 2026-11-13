#
# DOS-XC /qt/qpa/dosxc.pro
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

TARGET = qdosxc

QT += core-private gui-private eventdispatcher_support-private fontdatabase_support-private

DEFINES += QT_NO_FOREACH
SOURCES = main.cpp qdosxc.cpp gfx.c gui.c widgets.c dialogs.c image.c inflate.c
HEADERS = qdosxc.h
OTHER_FILES += dosxc.json
QMAKE_CFLAGS += -std=gnu11

qtConfig(freetype): QMAKE_USE_PRIVATE += freetype

PLUGIN_TYPE = platforms
PLUGIN_CLASS_NAME = QDosxcIntegrationPlugin
!equals(TARGET, $$QT_DEFAULT_QPA_PLUGIN): PLUGIN_EXTENDS = -
load(qt_plugin)
