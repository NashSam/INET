//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
//

#include "inet/common/packet/serializer/ChunkSerializerRegistry.h"
#include "inet/common/serializer/ppp/PPPHeaderSerializer.h"
#include "inet/linklayer/ppp/PPPFrame_m.h"

namespace inet {

namespace serializer {

Register_Serializer(PppHeader, PppHeaderSerializer);
Register_Serializer(PppTrailer, PppTrailerSerializer);

void PppHeaderSerializer::serialize(MemoryOutputStream& stream, const Ptr<const Chunk>& chunk) const
{
    const auto& pppHeader = std::static_pointer_cast<const PppHeader>(chunk);
    stream.writeUint8(pppHeader->getFlag());
    stream.writeUint8(pppHeader->getAddress());
    stream.writeUint8(pppHeader->getControl());
    stream.writeUint16Be(pppHeader->getProtocol());
}

Ptr<Chunk> PppHeaderSerializer::deserialize(MemoryInputStream& stream) const
{
    auto pppHeader = std::make_shared<PppHeader>();
    pppHeader->setFlag(stream.readUint8());
    pppHeader->setAddress(stream.readUint8());
    pppHeader->setControl(stream.readUint8());
    pppHeader->setProtocol(stream.readUint16Be());
    return pppHeader;
}

void PppTrailerSerializer::serialize(MemoryOutputStream& stream, const Ptr<const Chunk>& chunk) const
{
    const auto& pppTrailer = std::static_pointer_cast<const PppTrailer>(chunk);
    stream.writeUint16Be(pppTrailer->getFcs());
//    stream.writeUint8(pppTrailer->getFlag()); //FIXME KLUDGE length is currently 2 bytes instead of 3 bytes
}

Ptr<Chunk> PppTrailerSerializer::deserialize(MemoryInputStream& stream) const
{
    auto pppTrailer = std::make_shared<PppTrailer>();
    pppTrailer->setFcs(stream.readUint16Be());
//    pppTrailer->setFlag(stream.readUint8()); //FIXME KLUDGE length is currently 2 bytes instead of 3 bytes
    return pppTrailer;
}

} // namespace serializer

} // namespace inet

