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

#include "zoomablemenumodel.h"

using namespace muse;
using namespace muse::uicomponents;

ZoomableMenuModel::ZoomableMenuModel(const Settings::Key& zoomKey, double defaultZoom, QObject* parent)
    : AbstractMenuModel(parent), m_zoom(zoomKey, defaultZoom)
{
    m_zoom.zoomChanged().onNotify(this, [this]() {
        emit zoomChanged();
    });
}

qreal ZoomableMenuModel::zoom() const
{
    return m_zoom.zoom();
}

void ZoomableMenuModel::handleMenuItem(const QString& itemId)
{
    if (!handleZoomMenuItem(itemId)) {
        AbstractMenuModel::handleMenuItem(itemId);
    }
}

MenuItem* ZoomableMenuModel::makeZoomMenu()
{
    return m_zoom.makeMenu(this);
}

bool ZoomableMenuModel::handleZoomMenuItem(const QString& itemId)
{
    return m_zoom.handleMenuItem(itemId);
}
