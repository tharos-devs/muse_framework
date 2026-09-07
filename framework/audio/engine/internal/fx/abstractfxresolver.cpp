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

#include "abstractfxresolver.h"

#include "log.h"

using namespace muse::audio;
using namespace muse::audio::fx;

std::vector<IFxProcessorPtr> AbstractFxResolver::resolveFxList(const TrackId trackId, const AudioFxChain& fxChain,
                                                               const OutputSpec& outputSpec)
{
    if (fxChain.empty()) {
        LOGE() << "invalid fx chain for trackId: " << trackId;
        return {};
    }

    FxMap& fxMap = m_tracksFxMap[trackId];
    updateTrackFxMap(fxMap, trackId, fxChain, outputSpec);

    return muse::values(fxMap);
}

std::vector<IFxProcessorPtr> AbstractFxResolver::resolveMasterFxList(const AudioFxChain& fxChain, const OutputSpec& outputSpec)
{
    if (fxChain.empty()) {
        LOGE() << "invalid master fx params";
        return {};
    }

    updateMasterFxMap(fxChain, outputSpec);

    return muse::values(m_masterFxMap);
}

void AbstractFxResolver::refresh()
{
}

void AbstractFxResolver::clearAllFx()
{
    m_tracksFxMap.clear();
    m_masterFxMap.clear();
}

void AbstractFxResolver::updateTrackFxMap(FxMap& fxMap, const audio::TrackId trackId, const AudioFxChain& newFxChain,
                                          const OutputSpec& outputSpec)
{
    AudioFxChain currentFxChain;
    for (const auto& pair : fxMap) {
        currentFxChain.emplace(pair.first, pair.second->params());
    }

    FxMap relocatedFx = relocateMovedFx(fxMap, currentFxChain, newFxChain);

    audio::AudioFxChain fxToRemove;
    fxChainToRemove(currentFxChain, newFxChain, fxToRemove);

    for (const auto& pair : fxToRemove) {
        removeTrackFx(trackId, pair.second.resourceMeta.id, pair.second.chainOrder);

        currentFxChain.erase(pair.first);
        fxMap.erase(pair.first);
    }

    audio::AudioFxChain fxToCreate;
    fxChainToCreate(currentFxChain, newFxChain, fxToCreate);

    for (const auto& pair : fxToCreate) {
        if (relocatedFx.find(pair.first) != relocatedFx.cend()) {
            continue;
        }

        IFxProcessorPtr fxPtr = createTrackFx(trackId, pair.second, outputSpec);
        if (fxPtr) {
            fxMap.emplace(pair.first, std::move(fxPtr));
        }
    }

    for (auto& pair : relocatedFx) {
        fxMap.emplace(pair.first, std::move(pair.second));
    }
}

void AbstractFxResolver::updateMasterFxMap(const AudioFxChain& newFxChain, const OutputSpec& outputSpec)
{
    AudioFxChain currentFxChain;
    for (const auto& pair : m_masterFxMap) {
        currentFxChain.emplace(pair.first, pair.second->params());
    }

    FxMap relocatedFx = relocateMovedFx(m_masterFxMap, currentFxChain, newFxChain);

    audio::AudioFxChain fxToRemove;
    fxChainToRemove(currentFxChain, newFxChain, fxToRemove);

    for (const auto& pair : fxToRemove) {
        removeMasterFx(pair.second.resourceMeta.id, pair.second.chainOrder);

        currentFxChain.erase(pair.first);
        m_masterFxMap.erase(pair.first);
    }

    audio::AudioFxChain fxToCreate;
    fxChainToCreate(currentFxChain, newFxChain, fxToCreate);

    for (const auto& pair : fxToCreate) {
        if (relocatedFx.find(pair.first) != relocatedFx.cend()) {
            continue;
        }

        IFxProcessorPtr fx = createMasterFx(pair.second, outputSpec);
        if (fx) {
            m_masterFxMap.emplace(pair.first, fx);
        }
    }

    for (auto& pair : relocatedFx) {
        m_masterFxMap.emplace(pair.first, std::move(pair.second));
    }
}

void AbstractFxResolver::removeMasterFx(const AudioResourceId&, AudioFxChainOrder)
{
}

void AbstractFxResolver::removeTrackFx(const audio::TrackId, const AudioResourceId&, AudioFxChainOrder)
{
}

void AbstractFxResolver::fxChainToRemove(const AudioFxChain& currentFxChain,
                                         const AudioFxChain& newFxChain,
                                         AudioFxChain& resultChain)
{
    for (auto it = currentFxChain.cbegin(); it != currentFxChain.cend(); ++it) {
        auto newIt = newFxChain.find(it->first);

        if (newIt == newFxChain.cend()) {
            resultChain.insert({ it->first, it->second });
            continue;
        }

        if (it->second.resourceMeta != newIt->second.resourceMeta) {
            resultChain.insert({ it->first, it->second });
        }
    }
}

void AbstractFxResolver::fxChainToCreate(const AudioFxChain& currentFxChain,
                                         const AudioFxChain& newFxChain,
                                         AudioFxChain& resultChain)
{
    for (auto it = newFxChain.cbegin(); it != newFxChain.cend(); ++it) {
        if (currentFxChain.find(it->first) != currentFxChain.cend()) {
            continue;
        }

        if (it->second.isValid()) {
            resultChain.insert({ it->first, it->second });
        }
    }
}

AbstractFxResolver::FxMap AbstractFxResolver::relocateMovedFx(FxMap& fxMap, AudioFxChain& currentFxChain, const AudioFxChain& newFxChain)
{
    std::vector<std::pair<AudioFxChainOrder, AudioFxChainOrder> > moves;

    for (const auto& pair : fxMap) {
        AudioFxChainOrder oldOrder = pair.first;
        const AudioResourceMeta& resourceMeta = pair.second->params().resourceMeta;

        auto sameSpotIt = newFxChain.find(oldOrder);
        if (sameSpotIt != newFxChain.cend() && sameSpotIt->second.resourceMeta == resourceMeta) {
            continue; // still at the same position, nothing to relocate
        }

        for (const auto& newPair : newFxChain) {
            AudioFxChainOrder newOrder = newPair.first;
            if (newOrder == oldOrder || newPair.second.resourceMeta != resourceMeta) {
                continue;
            }

            bool newOrderAlreadyClaimed = false;
            for (const auto& move : moves) {
                if (move.second == newOrder) {
                    newOrderAlreadyClaimed = true;
                    break;
                }
            }
            if (newOrderAlreadyClaimed) {
                continue;
            }

            // That position may already be correctly occupied by another
            // live instance of the same resource (e.g. two identical fx of
            // the same type sitting right next to each other) -- leave it
            // alone rather than relocating on top of it.
            auto occupantIt = currentFxChain.find(newOrder);
            if (occupantIt != currentFxChain.cend() && occupantIt->second.resourceMeta == resourceMeta) {
                continue;
            }

            moves.emplace_back(oldOrder, newOrder);
            break;
        }
    }

    FxMap relocated;

    for (const auto& move : moves) {
        auto it = fxMap.find(move.first);
        IF_ASSERT_FAILED(it != fxMap.end()) {
            continue;
        }

        relocated.emplace(move.second, std::move(it->second));
        fxMap.erase(it);
        currentFxChain.erase(move.first);
    }

    return relocated;
}
