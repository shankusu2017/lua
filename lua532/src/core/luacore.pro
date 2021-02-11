#-------------------------------------------------
#
# Project created by QtCreator 2019-05-24T14:48:56
#
#-------------------------------------------------

QT       -= core gui

TARGET = luacore
TEMPLATE = lib
#base pro dir
DESTDIR = $$(PWD)../../lib

#include指令直接搜索的路径
INCLUDEPATH += ../core/include
INCLUDEPATH += ../core/src

#解决included头文件中包含其它头文件时的路径引用
DEPENDPATH += .

SOURCES += \
    ../core/src/lapi.c \
    ../core/src/lcode.c \
    ../core/src/lctype.c \
    ../core/src/ldebug.c \
    ../core/src/ldo.c \
    ../core/src/ldump.c \
    ../core/src/lfunc.c \
    ../core/src/lgc.c \
    ../core/src/llex.c \
    ../core/src/lmem.c \
    ../core/src/lobject.c \
    ../core/src/lopcodes.c \
    ../core/src/lparser.c \
    ../core/src/lstate.c \
    ../core/src/lstring.c \
    ../core/src/ltable.c \
    ../core/src/ltm.c \
    ../core/src/lundump.c \
    ../core/src/lvm.c \
    ../core/src/lzio.c

HEADERS += lapi.h \
    lcode.h \
    lctype.h \
    ldebug.h \
    ldo.h \
    lfunc.h \
    lgc.h \
    llex.h \
    llimits.h \
    lmem.h \
    lobject.h \
    lopcodes.h \
    lparser.h \
    lstate.h \
    lstring.h \
    ltable.h \
    ltm.h \
    lundump.h \
    lvm.h \
    lzio.h

unix {
    target.path = /usr/lib
    INSTALLS += target
}
