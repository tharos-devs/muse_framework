/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
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

import QtQuick

import Muse.Ui

//! NOTE: base for DockWindow.secondaryWindowComponent; DockWindow shows and hides it itself
Window {
    id: root

    //! NOTE: please, don't rename: used in c++ (DockWindow), it holds the docks
    readonly property Item dockArea: dockAreaItem

    width: 1000
    height: 600

    minimumWidth: 300
    minimumHeight: 200

    visible: false

    color: ui.theme.backgroundPrimaryColor

    onClosing: function(close) {
        //! NOTE: only hide it, so it keeps its docks; DockWindow then records it as closed
        close.accepted = false
        root.visible = false
    }

    Item {
        id: dockAreaItem

        anchors.fill: parent
    }
}
