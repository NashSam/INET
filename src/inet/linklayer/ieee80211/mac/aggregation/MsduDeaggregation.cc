//
// Copyright (C) 2016 OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see http://www.gnu.org/licenses/.
//

#include "inet/linklayer/ieee80211/mac/aggregation/MsduDeaggregation.h"

namespace inet {
namespace ieee80211 {

Register_Class(MsduDeaggregation);

void MsduDeaggregation::setExplodedFrameAddress(const Ptr<Ieee80211DataHeader>& header, const Ptr<const Ieee80211MsduSubframeHeader>& subframeHeader, const Ptr<const Ieee80211DataHeader>& aMsduHeader)
{
    bool toDS = aMsduHeader->getToDS();
    bool fromDS = aMsduHeader->getFromDS();
    if (fromDS == 0 && toDS == 0) // STA to STA
    {
        header->setTransmitterAddress(aMsduHeader->getTransmitterAddress());
        header->setReceiverAddress(aMsduHeader->getReceiverAddress());
    }
    else if (fromDS == 1 && toDS == 0) // AP to STA
    {
        header->setTransmitterAddress(header->getTransmitterAddress());
        header->setReceiverAddress(subframeHeader->getDa());
        header->setAddress3(subframeHeader->getSa());
    }
    else if (fromDS == 0 && toDS == 1) // STA to AP
    {
        header->setTransmitterAddress(subframeHeader->getSa());
        header->setReceiverAddress(aMsduHeader->getReceiverAddress());
        header->setAddress3(subframeHeader->getDa());
    }
    else if (fromDS == 1 && toDS == 1) // AP to AP
    {
        header->setReceiverAddress(aMsduHeader->getReceiverAddress());
        header->setTransmitterAddress(aMsduHeader->getTransmitterAddress());
        header->setAddress3(subframeHeader->getDa());
        header->setAddress4(subframeHeader->getSa());
    }
}

std::vector<Packet *> *MsduDeaggregation::deaggregateFrame(Packet *aggregatedFrame)
{
    std::vector<Packet *> *frames = new std::vector<Packet *>();
    const auto& amsduHeader = aggregatedFrame->popHeader<Ieee80211DataHeader>();
    aggregatedFrame->popTrailer<Ieee80211MacTrailer>();
    int tid = amsduHeader->getTid();
    int paddingLength = 0;
    while (aggregatedFrame->getDataLength() > bit(0))
    {
        aggregatedFrame->setHeaderPopOffset(aggregatedFrame->getHeaderPopOffset() + byte(paddingLength == 4 ? 0 : paddingLength));
        const auto& msduSubframeHeader = aggregatedFrame->popHeader<Ieee80211MsduSubframeHeader>();
        const auto& msdu = aggregatedFrame->peekDataAt(bit(0), byte(msduSubframeHeader->getLength()));
        paddingLength = 4 - byte(msduSubframeHeader->getChunkLength() + msdu->getChunkLength()).get() % 4;
        aggregatedFrame->setHeaderPopOffset(aggregatedFrame->getHeaderPopOffset() + msdu->getChunkLength());
        // TODO: review, restore snap header, see Ieee80211MsduSubframeHeader
        auto frame = new Packet();
        frame->append(msdu);
        auto header = std::make_shared<Ieee80211DataHeader>();
        // dataFrame->setType(ST_DATA_WITH_QOS); FIXME:
        header->setType(ST_DATA);
        header->setToDS(amsduHeader->getToDS());
        header->setFromDS(amsduHeader->getFromDS());
        header->setTid(tid);
        setExplodedFrameAddress(header, msduSubframeHeader, amsduHeader);
        frame->insertHeader(header);
        frame->insertTrailer(std::make_shared<Ieee80211MacTrailer>());
        frames->push_back(frame);
    }
    delete aggregatedFrame;
    return frames;
}

} /* namespace ieee80211 */
} /* namespace inet */

