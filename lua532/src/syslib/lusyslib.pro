#-------------------------------------------------
#
# Project created by QtCreator 2019-05-24T14:59:11
#
#-------------------------------------------------

QT          -= core gui

TARGET      = luasyslib
TEMPLATE    = lib
DESTDIR     = $$PWD/../../lib

INCLUDEPATH += ../core/include
INCLUDEPATH += ../syslib/include

LIBS += -L$$PWD/../../lib/ \
        -lluacore

SOURCES += \
    ../syslib/src/lbitlib.c \
    ../syslib/src/lcorolib.c \
    ../syslib/src/ldblib.c \
    ../syslib/src/liolib.c \
    ../syslib/src/lmathlib.c \
    ../syslib/src/loadlib.c \
    ../syslib/src/loslib.c \
    ../syslib/src/lstrlib.c \
    ../syslib/src/ltablib.c \
    ../syslib/src/lutf8lib.c \
    ../syslib/src/lbaselib.c \
    ../syslib/src/lauxlib.c \
    ../syslib/src/linit.c



