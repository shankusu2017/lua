TEMPLATE = app
TARGET   = luademon

CONFIG  += console
CONFIG  -= app_bundle qt

QMAKE_CFLAGS += -Wunused-variable

#path base build_dir not project_dir
TARGET_DIR = ../..
DESTDIR = $${TARGET_DIR}/bin

#Specifies the #include directories which should be searched when compiling the project.
INCLUDEPATH  = ../core/include
INCLUDEPATH += ../syslib/include

#Specifies a list of all directories to look in to resolve dependencies.
#This variable is used when crawling through included files.
#DEPENDPATH
DEPENDPATH = ../syslib/include
DEPENDPATH += ../core/include
DEPENDPATH += ../core/src


LIBS += -L$${TARGET_DIR}/lib -lluasyslib -lluacore

SOURCES +=  \
    conversion.c \
    main.c \
    operator.c \
    mail.c \
    mail2.c \
    mail3.c

HEADERS +=  lua.h \
            lauxlib.h \
            lualib.h \
    conversion.h \
    operator.h \
    mail.h
