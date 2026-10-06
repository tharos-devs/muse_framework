/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2022 MuseScore Limited and others
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
import QtQuick

//! NOTE: shows its content at a zoom (e.g. a panel's, see ZoomableMenuModel), as one Qt Quick scale: the content is
//! sized so that once scaled it exactly fills this container, and laid out in it as usual (anchors.fill: parent...)
Item {
    id: root

    property real zoom: 1.0

    default property alias contentData: zoomedContent.data
    readonly property alias contentItem: zoomedContent

    clip: true

    Item {
        id: zoomedContent

        width: root.width / scale
        height: root.height / scale

        scale: root.zoom > 0 ? root.zoom : 1
        transformOrigin: Item.TopLeft
    }
}
