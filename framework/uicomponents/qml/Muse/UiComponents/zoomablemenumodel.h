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

#include <qqmlintegration.h>

#include "abstractmenumodel.h"
#include "panelzoom.h"

namespace muse::uicomponents {
//! NOTE: a panel's "…" menu with the panel's zoom (see PanelZoom): put makeZoomMenu() among its items, and apply
//! zoom to the panel's content (see ZoomContainer.qml). A subclass handling menu items itself calls
//! handleZoomMenuItem() first
class ZoomableMenuModel : public AbstractMenuModel
{
    Q_OBJECT

    Q_PROPERTY(qreal zoom READ zoom NOTIFY zoomChanged)

    QML_ELEMENT;
    QML_UNCREATABLE("Base class of the panels' menu models")

public:
    ZoomableMenuModel(const Settings::Key& zoomKey, double defaultZoom = 1.0, QObject* parent = nullptr);

    qreal zoom() const;

    Q_INVOKABLE void handleMenuItem(const QString& itemId) override;

signals:
    void zoomChanged();

protected:
    MenuItem* makeZoomMenu();
    bool handleZoomMenuItem(const QString& itemId);

private:
    PanelZoom m_zoom;
};
}
