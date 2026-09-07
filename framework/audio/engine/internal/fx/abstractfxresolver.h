/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2023 MuseScore Limited and others
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

#include <map>

#include "../../ifxresolver.h"
#include "../../ifxprocessor.h"

namespace muse::audio::fx {
class AbstractFxResolver : public IFxResolver::IResolver
{
public:
    std::vector<IFxProcessorPtr> resolveFxList(const TrackId trackId, const AudioFxChain& fxChain, const OutputSpec& outputSpec) override;
    std::vector<IFxProcessorPtr> resolveMasterFxList(const AudioFxChain& fxChain, const OutputSpec& outputSpec) override;

    void refresh() override;
    void clearAllFx() override;

protected:
    virtual IFxProcessorPtr createMasterFx(const AudioFxParams& fxParams, const OutputSpec& outputSpec) const = 0;
    virtual IFxProcessorPtr createTrackFx(const TrackId trackId, const AudioFxParams& fxParams, const OutputSpec& outputSpec) const = 0;

    virtual void removeMasterFx(const AudioResourceId& resoureId, AudioFxChainOrder order);
    virtual void removeTrackFx(const TrackId trackId, const AudioResourceId& resoureId, AudioFxChainOrder order);

private:
    using FxMap = std::map<AudioFxChainOrder, IFxProcessorPtr>;

    void updateMasterFxMap(const AudioFxChain& newFxChain, const OutputSpec& outputSpec);
    void updateTrackFxMap(FxMap& fxMap, const TrackId trackId, const AudioFxChain& newFxChain, const audio::OutputSpec& outputSpec);

    void fxChainToRemove(const AudioFxChain& currentFxChain, const AudioFxChain& newFxChain, AudioFxChain& resultChain);
    void fxChainToCreate(const AudioFxChain& currentFxChain, const AudioFxChain& newFxChain, AudioFxChain& resultChain);

    //! NOTE An effect that's simply moved to a different chainOrder (same
    //! resourceMeta, e.g. after a Mixer FX drag-and-drop reorder) would
    //! otherwise look, to the purely position-keyed fxChainToRemove/
    //! fxChainToCreate diff above, like one effect disappearing and an
    //! unrelated one appearing at each affected position -- destroying and
    //! recreating the live plugin instance (costly and glitch-prone for a
    //! real-time VST/VSTi) for what's really just a reorder. Detects that
    //! case and relocates the existing instance(s) to their new key in
    //! fxMap/currentFxChain instead, so the diff below sees no change for
    //! them at all. Returns the relocated instances, keyed by their NEW
    //! chainOrder -- callers should skip creating anew at those positions
    //! and insert these back into fxMap only after the normal remove pass
    //! has vacated any conflicting occupant there.
    FxMap relocateMovedFx(FxMap& fxMap, AudioFxChain& currentFxChain, const AudioFxChain& newFxChain);

    std::map<TrackId, FxMap> m_tracksFxMap;
    FxMap m_masterFxMap;
};
}
