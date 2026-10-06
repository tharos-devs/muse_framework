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

#include "panelzoom.h"

#include <algorithm>
#include <cmath>

#include "types/translatablestring.h"

#include "menuitem.h"

using namespace muse;
using namespace muse::uicomponents;

const QString PanelZoom::ZOOM_IN_ID = "panel-zoom-in";
const QString PanelZoom::ZOOM_OUT_ID = "panel-zoom-out";
const QString PanelZoom::RESET_ZOOM_ID = "panel-zoom-reset";

static constexpr int MIN_PERCENT = 50;
static constexpr int MAX_PERCENT = 200;
static constexpr int STEP_PERCENT = 10;

MenuItem* PanelZoom::makeZoomMenu(QObject* parent, QList<QPointer<MenuItem> >* items)
{
    auto makeItem = [parent, items](const QString& id, const TranslatableString& title) {
        MenuItem* item = new MenuItem(parent);
        item->setId(id);
        item->setTitle(title);
        item->setEnabled(true);
        if (items) {
            items->push_back(item);
        }
        return item;
    };

    MenuItem* menu = new MenuItem(parent);
    menu->setId("panel-zoom-menu");
    menu->setTitle(TranslatableString("uicomponents", "Zoom"));
    menu->setSubitems({
        makeItem(ZOOM_IN_ID, TranslatableString("uicomponents", "Zoom +")),
        makeItem(ZOOM_OUT_ID, TranslatableString("uicomponents", "Zoom -")),
        makeItem(RESET_ZOOM_ID, TranslatableString("uicomponents", "Reset zoom")),
    });
    menu->setEnabled(true);

    return menu;
}

PanelZoom::PanelZoom(const Settings::Key& key, double defaultZoom)
    : m_key(key), m_defaultZoom(defaultZoom)
{
    settings()->setDefaultValue(m_key, Val(m_defaultZoom));
    settings()->valueChanged(m_key).onReceive(this, [this](const Val&) {
        updateMenuState();
        m_zoomChanged.notify();
    });
}

double PanelZoom::zoom() const
{
    //! NOTE: a missing or broken value (not a positive number) is the default
    const double value = settings()->value(m_key).toDouble();
    if (!(value > 0)) {
        return m_defaultZoom;
    }

    return std::clamp(value, MIN_PERCENT / 100.0, MAX_PERCENT / 100.0);
}

async::Notification PanelZoom::zoomChanged() const
{
    return m_zoomChanged;
}

MenuItem* PanelZoom::makeMenu(QObject* parent)
{
    m_menuItems.clear();
    MenuItem* menu = makeZoomMenu(parent, &m_menuItems);
    updateMenuState();
    return menu;
}

bool PanelZoom::handleMenuItem(const QString& itemId)
{
    if (itemId == ZOOM_IN_ID) {
        setPercent(steppedPercent(+1));
    } else if (itemId == ZOOM_OUT_ID) {
        setPercent(steppedPercent(-1));
    } else if (itemId == RESET_ZOOM_ID) {
        settings()->setSharedValue(m_key, Val(m_defaultZoom));
    } else {
        return false;
    }

    return true;
}

int PanelZoom::percent() const
{
    return static_cast<int>(std::lround(zoom() * 100));
}

//! NOTE: whole percents, so repeated steps land exactly on the grid (no floating-point drift)
int PanelZoom::steppedPercent(int direction) const
{
    const int current = percent();
    const int next = direction > 0
                     ? (current / STEP_PERCENT + 1) * STEP_PERCENT
                     : ((current + STEP_PERCENT - 1) / STEP_PERCENT - 1) * STEP_PERCENT;
    return std::clamp(next, MIN_PERCENT, MAX_PERCENT);
}

void PanelZoom::setPercent(int zoomPercent)
{
    if (zoomPercent != percent()) {
        settings()->setSharedValue(m_key, Val(zoomPercent / 100.0));
    }
}

void PanelZoom::updateMenuState()
{
    const int current = percent();
    const int defaultPercent = static_cast<int>(std::lround(m_defaultZoom * 100));
    const bool enabled[] = { current < MAX_PERCENT, current > MIN_PERCENT, current != defaultPercent };

    for (int i = 0; i < m_menuItems.size() && i < 3; ++i) {
        if (m_menuItems.at(i)) {
            m_menuItems.at(i)->setEnabled(enabled[i]);
        }
    }
}
