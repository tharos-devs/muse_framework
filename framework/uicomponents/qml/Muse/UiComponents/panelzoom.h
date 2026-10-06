/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QList>
#include <QPointer>
#include <QString>

#include "async/asyncable.h"
#include "async/notification.h"
#include "settings.h"

namespace muse::uicomponents {
class MenuItem;

//! NOTE: a panel's zoom, changed from the "Zoom" submenu of its "…" menu (Zoom +, Zoom -, Reset zoom): 50% to 200%
//! in 10% steps, persisted. See ZoomableMenuModel (a "…" menu with it) and ZoomContainer.qml (applies it)
class PanelZoom : public async::Asyncable
{
public:
    static const QString ZOOM_IN_ID;
    static const QString ZOOM_OUT_ID;
    static const QString RESET_ZOOM_ID;

    //! NOTE: the submenu alone, also for a panel zooming its own way (e.g. a timeline's horizontal zoom): the menu
    //! model handles its items' ids. items: its 3 items, in that order
    static MenuItem* makeZoomMenu(QObject* parent, QList<QPointer<MenuItem> >* items = nullptr);

    PanelZoom(const Settings::Key& key, double defaultZoom = 1.0);

    double zoom() const;
    async::Notification zoomChanged() const;

    //! NOTE: the submenu, whose items get disabled at the limits (and Reset zoom at the default)
    MenuItem* makeMenu(QObject* parent);

    //! NOTE: true when it was one of the submenu's items
    bool handleMenuItem(const QString& itemId);

private:
    int percent() const;
    int steppedPercent(int direction) const;
    void setPercent(int zoomPercent);
    void updateMenuState();

    const Settings::Key m_key;
    const double m_defaultZoom = 1.0;
    QList<QPointer<MenuItem> > m_menuItems;
    async::Notification m_zoomChanged;
};
}
