/***************************************************************************
                          krecentfileitem.cpp  -  description
                             -------------------
    begin                : Wed Jul 30 2003
    copyright            : (C) 2000-2003 by Michael Edwardes
    email                : mte@users.sourceforge.net
                           Javier Campos Morales <javi_c@users.sourceforge.net>
                           Felix Rodriguez <frodriguez@users.sourceforge.net>
                           John C <thetacoturtle@users.sourceforge.net>
                           Thomas Baumgart <ipwizard@users.sourceforge.net>
                           Kevin Tambascio <ktambascio@users.sourceforge.net>
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "krecentfileitem.h"

// ----------------------------------------------------------------------------
// QT Includes

#include <QToolTip>
#include <qwidget.h>
#include <QRect>
#include <qpoint.h>
#include <QPixmap>

// ----------------------------------------------------------------------------
// KDE Includes

// ----------------------------------------------------------------------------
// Project Includes


KRecentFileItem::KRecentFileItem(const QString& url, Q3IconView* parent, const QString& text, const QPixmap& icon)
    : Q3IconViewItem(parent, text, icon),
    m_url(url),
    m_parent(parent)
{
  parent->setToolTip(url);
  // avoid moving this item around
  setDragEnabled(false);
}

KRecentFileItem::~KRecentFileItem()
{
}

bool KRecentFileItem::move(int x, int y)
{
  m_parent->setToolTip(m_url);
  return Q3IconViewItem::move(x, y);
}

