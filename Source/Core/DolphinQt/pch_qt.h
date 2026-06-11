#pragma once

#include "pch.h"

// Must precede any Qt header: reprovides stdext array-iterator factories that
// newer MSVC STLs removed but Qt 6.5.1 still references. No-op on non-MSVC.
#include "MSVCStdextCompat.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QObject>
#include <QString>
#include <QTableWidget>
#include <QWidget>
