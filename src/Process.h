#ifndef PROCESS_H
#define PROCESS_H

#include <QtGlobal>

#ifdef Q_OS_WIN
#include "win32/Win32Process.h"
#include "win32/Win32PtyProcess.h"
using Process = Win32Process;
using PtyProcess = Win32PtyProcess;
#else
#include "unix/UnixProcess.h"
#include "unix/UnixPtyProcess.h"
using Process = UnixProcess;
using PtyProcess = UnixPtyProcess;
#endif

#endif // PROCESS_H