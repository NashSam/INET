//
// Copyright (C) 2010 Zoltan Bojthe
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//

#include "inet/transportlayer/tcp_lwip/TCP_lwIP.h"

//#include "headers/defs.h"   // for endian macros
//#include "headers/in_systm.h"
#include "lwip/lwip_ip.h"
#include "lwip/lwip_tcp.h"

#ifdef WITH_IPv4
#include "inet/networklayer/ipv4/ICMPHeader_m.h"
#endif // ifdef WITH_IPv4

#ifdef WITH_IPv6
#include "inet/networklayer/icmpv6/ICMPv6Header_m.h"
#endif // ifdef WITH_IPv6

#include "inet/applications/common/SocketTag_m.h"
#include "inet/common/IProtocolRegistrationListener.h"
#include "inet/common/ModuleAccess.h"
#include "inet/common/Protocol.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/common/lifecycle/LifecycleOperation.h"
#include "inet/common/lifecycle/NodeStatus.h"
#include "inet/common/serializer/TCPIPchecksum.h"
#include "inet/common/serializer/tcp/headers/tcphdr.h"
#include "inet/linklayer/common/InterfaceTag_m.h"
#include "inet/networklayer/contract/IL3AddressType.h"
#include "inet/networklayer/common/IPProtocolId_m.h"
#include "inet/networklayer/common/L3Address.h"
#include "inet/networklayer/common/L3AddressTag_m.h"
#include "inet/transportlayer/contract/tcp/TCPCommand_m.h"
#include "inet/transportlayer/tcp_common/TCPSegment.h"
#include "inet/transportlayer/tcp_lwip/TcpLwipConnection.h"
#include "inet/transportlayer/tcp_lwip/queues/TcpLwipQueues.h"

namespace inet {

namespace tcp {

using namespace serializer;

Define_Module(TCP_lwIP);

TCP_lwIP::TCP_lwIP()
    :
    pLwipFastTimerM(nullptr),
    pLwipTcpLayerM(nullptr),
    isAliveM(false),
    pCurTcpSegM(nullptr)
{
    netIf.gw.addr = L3Address();
    netIf.flags = 0;
    netIf.input = nullptr;
    netIf.ip_addr.addr = L3Address();
    netIf.linkoutput = nullptr;
    netIf.mtu = 1500;
    netIf.name[0] = 'T';
    netIf.name[1] = 'C';
    netIf.netmask.addr = L3Address();
    netIf.next = nullptr;
    netIf.num = 0;
    netIf.output = nullptr;
    netIf.state = nullptr;
}

void TCP_lwIP::initialize(int stage)
{
    cSimpleModule::initialize(stage);

    EV_TRACE << this << ": initialize stage " << stage << endl;

    if (stage == INITSTAGE_LOCAL) {
        const char *q;
        q = par("sendQueueClass");
        if (*q != '\0')
            throw cRuntimeError("Don't use obsolete sendQueueClass = \"%s\" parameter", q);

        q = par("receiveQueueClass");
        if (*q != '\0')
            throw cRuntimeError("Don't use obsolete receiveQueueClass = \"%s\" parameter", q);

        WATCH_MAP(tcpAppConnMapM);

        recordStatisticsM = par("recordStats");

        pLwipTcpLayerM = new LwipTcpLayer(*this);
        pLwipFastTimerM = new cMessage("lwip_fast_timer");
        EV_INFO << "TCP_lwIP " << this << " has stack " << pLwipTcpLayerM << "\n";
    }
    else if (stage == INITSTAGE_TRANSPORT_LAYER) {
        bool isOperational;
        NodeStatus *nodeStatus = dynamic_cast<NodeStatus *>(findContainingNode(this)->getSubmodule("status"));
        isOperational = (!nodeStatus) || nodeStatus->getState() == NodeStatus::UP;
        if (!isOperational)
            throw cRuntimeError("This module doesn't support starting in node DOWN state");
        registerProtocol(Protocol::tcp, gate("ipOut"));
        registerProtocol(Protocol::tcp, gate("appOut"));
    }
    else if (stage == INITSTAGE_LAST) {
        isAliveM = true;
    }
}

TCP_lwIP::~TCP_lwIP()
{
    EV_TRACE << this << ": destructor\n";
    isAliveM = false;

    while (!tcpAppConnMapM.empty()) {
        auto i = tcpAppConnMapM.begin();
        delete i->second;
        tcpAppConnMapM.erase(i);
    }

    if (pLwipFastTimerM)
        cancelAndDelete(pLwipFastTimerM);

    if (pLwipTcpLayerM)
        delete pLwipTcpLayerM;
}

void TCP_lwIP::handleIpInputMessage(Packet *packet)
{
    L3Address srcAddr, destAddr;
    int interfaceId = -1;

    auto tcpsegP = packet->peekHeader<TcpHeader>();
    srcAddr = packet->getMandatoryTag<L3AddressInd>()->getSrcAddress();
    destAddr = packet->getMandatoryTag<L3AddressInd>()->getDestAddress();
    interfaceId = (packet->getMandatoryTag<InterfaceInd>())->getInterfaceId();

    switch(tcpsegP->getCrcMode()) {
        case CRC_DECLARED_INCORRECT:
            EV_WARN << "CRC error, packet dropped\n";
            delete packet;
            return;
        case CRC_DECLARED_CORRECT: {
            // modify to calculated, for serializing
            packet->removePoppedHeaders();
            const auto& newTcpsegP = packet->removeHeader<TcpHeader>();
            newTcpsegP->setCrcMode(CRC_COMPUTED);
            newTcpsegP->setCrc(0);
            packet->insertHeader(newTcpsegP);
            tcpsegP = newTcpsegP;
            break;
        }
        default:
            break;
    }

    // process segment
    size_t ipHdrLen = sizeof(ip_hdr);
    size_t const maxBufferSize = 4096;
    char *data = new char[maxBufferSize];
    memset(data, 0, maxBufferSize);

    ip_hdr *ih = (ip_hdr *)data;
    tcphdr *tcph = (tcphdr *)(data + ipHdrLen);

    // set the modified lwip IP header:
    ih->_hl = ipHdrLen / 4;
    ASSERT((ih->_hl) * 4 == ipHdrLen);
    ih->_chksum = 0;
    ih->src.addr = srcAddr;
    ih->dest.addr = destAddr;

    size_t totalTcpLen = maxBufferSize - ipHdrLen;

    const auto& bytes = packet->peekDataBytes();
    totalTcpLen = bytes->copyToBuffer((uint8_t *)data + ipHdrLen, totalTcpLen);

    size_t totalIpLen = ipHdrLen + totalTcpLen;
    ih->_chksum = 0;

    // search unfilled local addr in pcb-s for this connection.
    L3Address laddr = ih->dest.addr;
    L3Address raddr = ih->src.addr;
    u16_t lport = tcpsegP->getDestPort();
    u16_t rport = tcpsegP->getSrcPort();

    if (tcpsegP->getSynBit() && tcpsegP->getAckBit()) {
        for (auto & elem : tcpAppConnMapM) {
            LwipTcpLayer::tcp_pcb *pcb = elem.second->pcbM;
            if (pcb) {
                if ((pcb->state == LwipTcpLayer::SYN_SENT)
                    && (pcb->local_ip.addr.isUnspecified())
                    && (pcb->local_port == lport)
                    && (pcb->remote_ip.addr == raddr)
                    && (pcb->remote_port == rport)
                    )
                {
                    pcb->local_ip.addr = laddr;
                }
            }
        }
    }

    ASSERT(pCurTcpSegM == nullptr);
    pCurTcpSegM = packet;
    // receive msg from network
    pLwipTcpLayerM->if_receive_packet(interfaceId, data, totalIpLen);
    // lwip call back the notifyAboutIncomingSegmentProcessing() for store incoming messages
    pCurTcpSegM = nullptr;

    // LwipTcpLayer will call the tcp_event_recv() / tcp_event_err() and/or send a packet to sender

    delete[] data;
    delete packet;
}

void TCP_lwIP::notifyAboutIncomingSegmentProcessing(LwipTcpLayer::tcp_pcb *pcb, uint32 seqNo,
        const void *dataptr, int len)
{
    TcpLwipConnection *conn = (pcb != nullptr) ? (TcpLwipConnection *)(pcb->callback_arg) : nullptr;
    if (conn) {
        conn->receiveQueueM->notifyAboutIncomingSegmentProcessing(pCurTcpSegM, seqNo, dataptr, len);
    }
    else {
        const auto& tcpHdr = pCurTcpSegM->peekHeader<TcpHeader>();
        if (pCurTcpSegM->getByteLength() > tcpHdr->getHeaderLength())
            throw cRuntimeError("conn is null, and received packet has data");

        EV_WARN << "notifyAboutIncomingSegmentProcessing: conn is null\n";
    }
}

void TCP_lwIP::lwip_free_pcb_event(LwipTcpLayer::tcp_pcb *pcb)
{
    TcpLwipConnection *conn = (TcpLwipConnection *)(pcb->callback_arg);
    if (conn != nullptr) {
        if (conn->pcbM == pcb) {
            // conn->sendIndicationToApp(TCP_I_????); // TODO send some indication when need
            removeConnection(*conn);
        }
    }
}

err_t TCP_lwIP::lwip_tcp_event(void *arg, LwipTcpLayer::tcp_pcb *pcb,
        LwipTcpLayer::lwip_event event, struct pbuf *p, u16_t size, err_t err)
{
    TcpLwipConnection *conn = (TcpLwipConnection *)arg;
    ASSERT(conn != nullptr);

    switch (event) {
        case LwipTcpLayer::LWIP_EVENT_ACCEPT:
            err = tcp_event_accept(*conn, pcb, err);
            break;

        case LwipTcpLayer::LWIP_EVENT_SENT:
            ASSERT(conn->pcbM == pcb);
            err = tcp_event_sent(*conn, size);
            break;

        case LwipTcpLayer::LWIP_EVENT_RECV:
            ASSERT(conn->pcbM == pcb);
            err = tcp_event_recv(*conn, p, err);
            break;

        case LwipTcpLayer::LWIP_EVENT_CONNECTED:
            ASSERT(conn->pcbM == pcb);
            err = tcp_event_conn(*conn, err);
            break;

        case LwipTcpLayer::LWIP_EVENT_POLL:
            // it's called also when conn->pcbM point to a LISTEN pcb, and pcb point to a SYN_RCVD
            if (conn->pcbM == pcb)
                err = tcp_event_poll(*conn);
            break;

        case LwipTcpLayer::LWIP_EVENT_ERR:
            err = tcp_event_err(*conn, err);
            break;

        default:
            throw cRuntimeError("Invalid lwip_event: %d", event);
            break;
    }

    return err;
}

err_t TCP_lwIP::tcp_event_accept(TcpLwipConnection& conn, LwipTcpLayer::tcp_pcb *pcb, err_t err)
{
    int newConnId = getEnvir()->getUniqueNumber();
    TcpLwipConnection *newConn = new TcpLwipConnection(conn, newConnId, pcb);
    // add into appConnMap
    tcpAppConnMapM[newConnId] = newConn;

    newConn->sendAvailableIndicationToApp(conn.connIdM);

    EV_DETAIL << this << ": TCP_lwIP: got accept!\n";
    return err;
}

err_t TCP_lwIP::tcp_event_sent(TcpLwipConnection& conn, u16_t size)
{
    conn.do_SEND();
    return ERR_OK;
}

err_t TCP_lwIP::tcp_event_recv(TcpLwipConnection& conn, struct pbuf *p, err_t err)
{
    if (p == nullptr) {
        // Received FIN:
        EV_DETAIL << this << ": tcp_event_recv(" << conn.connIdM
                  << ", pbuf[nullptr], " << (int)err << "):FIN\n";
        TcpStatusInd ind = (conn.pcbM->state == LwipTcpLayer::TIME_WAIT) ? TCP_I_CLOSED : TCP_I_PEER_CLOSED;
        EV_INFO << "Connection " << conn.connIdM << ((ind == TCP_I_CLOSED) ? " closed" : "closed by peer") << endl;
        conn.sendIndicationToApp(ind);
        // TODO is it good?
        pLwipTcpLayerM->tcp_recved(conn.pcbM, 0);
    }
    else {
        EV_DETAIL << this << ": tcp_event_recv(" << conn.connIdM << ", pbuf[" << p->len << ", "
                  << p->tot_len << "], " << (int)err << ")\n";
        conn.receiveQueueM->enqueueTcpLayerData(p->payload, p->tot_len);
        pLwipTcpLayerM->tcp_recved(conn.pcbM, p->tot_len);
        pbuf_free(p);
    }

    conn.sendUpData();
    conn.do_SEND();
    return err;
}

err_t TCP_lwIP::tcp_event_conn(TcpLwipConnection& conn, err_t err)
{
    conn.sendEstablishedMsg();
    conn.do_SEND();
    return err;
}

void TCP_lwIP::removeConnection(TcpLwipConnection& conn)
{
    conn.pcbM->callback_arg = nullptr;
    conn.pcbM = nullptr;
    tcpAppConnMapM.erase(conn.connIdM);
    delete &conn;
}

err_t TCP_lwIP::tcp_event_err(TcpLwipConnection& conn, err_t err)
{
    switch (err) {
        case ERR_ABRT:
            EV_INFO << "Connection " << conn.connIdM << " aborted, closed\n";
            conn.sendIndicationToApp(TCP_I_CLOSED);
            removeConnection(conn);
            break;

        case ERR_RST:
            EV_INFO << "Connection " << conn.connIdM << " reset\n";
            conn.sendIndicationToApp(TCP_I_CONNECTION_RESET);
            removeConnection(conn);
            break;

        default:
            throw cRuntimeError("Invalid LWIP error code: %d", err);
    }

    return err;
}

err_t TCP_lwIP::tcp_event_poll(TcpLwipConnection& conn)
{
    conn.do_SEND();
    return ERR_OK;
}

struct netif *TCP_lwIP::ip_route(L3Address const& ipAddr)
{
    return &netIf;
}

void TCP_lwIP::handleAppMessage(cMessage *msgP)
{
    int connId = msgP->getMandatoryTag<SocketReq>()->getSocketId();

    TcpLwipConnection *conn = findAppConn(connId);

    if (!conn) {
        TCPOpenCommand *openCmd = check_and_cast<TCPOpenCommand *>(msgP->getControlInfo());
        TCPDataTransferMode dataTransferMode = (TCPDataTransferMode)(openCmd->getDataTransferMode());

        // add into appConnMap
        conn = new TcpLwipConnection(*this, connId, dataTransferMode);
        tcpAppConnMapM[connId] = conn;

        EV_INFO << this << ": TCP connection created for " << msgP << "\n";
    }

    processAppCommand(*conn, msgP);
}

simtime_t roundTime(const simtime_t& timeP, int secSlicesP)
{
    int64_t scale = timeP.getScale() / secSlicesP;
    simtime_t ret = timeP;
    ret /= scale;
    ret *= scale;
    return ret;
}

void TCP_lwIP::handleMessage(cMessage *msgP)
{
    if (msgP->isSelfMessage()) {
        // timer expired
        if (msgP == pLwipFastTimerM) {    // lwip fast timer
            EV_TRACE << "Call tcp_fasttmr()\n";
            pLwipTcpLayerM->tcp_fasttmr();
            if (simTime() == roundTime(simTime(), 2)) {
                EV_TRACE << "Call tcp_slowtmr()\n";
                pLwipTcpLayerM->tcp_slowtmr();
            }
        }
        else {
            throw cRuntimeError("Unknown self message");
        }
    }
    else if (msgP->arrivedOn("ipIn")) {
        // must be a Packet
        Packet *pk = check_and_cast<Packet *>(msgP);
        auto protocol = msgP->getMandatoryTag<PacketProtocolTag>()->getProtocol();
        if (protocol == &Protocol::tcp) {
            EV_TRACE << this << ": handle tcp segment: " << msgP->getName() << "\n";
            handleIpInputMessage(pk);
        }
        else if (protocol == &Protocol::icmpv4 || protocol == &Protocol::icmpv6) {
            EV_WARN << "ICMP error received -- discarding\n";    // FIXME can ICMP packets really make it up to TCP???
            delete msgP;
        }
        else
            throw cRuntimeError("Unknown protocol: %s(%d)", protocol->getName(), protocol->getId());
    }
    else {    // must be from app
        EV_TRACE << this << ": handle msg: " << msgP->getName() << "\n";
        handleAppMessage(msgP);
    }

    if (!pLwipFastTimerM->isScheduled()) {    // lwip fast timer
        if (nullptr != pLwipTcpLayerM->tcp_active_pcbs || nullptr != pLwipTcpLayerM->tcp_tw_pcbs)
            scheduleAt(roundTime(simTime() + 0.250, 4), pLwipFastTimerM);
    }
}

void TCP_lwIP::refreshDisplay() const
{
    if (getEnvir()->isExpressMode()) {
        // in express mode, we don't bother to update the display
        // (std::map's iteration is not very fast if map is large)
        getDisplayString().setTagArg("t", 0, "");
        return;
    }

    int numINIT = 0, numCLOSED = 0, numLISTEN = 0, numSYN_SENT = 0, numSYN_RCVD = 0,
        numESTABLISHED = 0, numCLOSE_WAIT = 0, numLAST_ACK = 0, numFIN_WAIT_1 = 0,
        numFIN_WAIT_2 = 0, numCLOSING = 0, numTIME_WAIT = 0;

    for (auto & elem : tcpAppConnMapM) {
        LwipTcpLayer::tcp_pcb *pcb = (elem).second->pcbM;

        if (nullptr == pcb) {
            numINIT++;
        }
        else {
            enum LwipTcpLayer::tcp_state state = pcb->state;

            switch (state) {
                case LwipTcpLayer::CLOSED:
                    numCLOSED++;
                    break;

                case LwipTcpLayer::LISTEN:
                    numLISTEN++;
                    break;

                case LwipTcpLayer::SYN_SENT:
                    numSYN_SENT++;
                    break;

                case LwipTcpLayer::SYN_RCVD:
                    numSYN_RCVD++;
                    break;

                case LwipTcpLayer::ESTABLISHED:
                    numESTABLISHED++;
                    break;

                case LwipTcpLayer::CLOSE_WAIT:
                    numCLOSE_WAIT++;
                    break;

                case LwipTcpLayer::LAST_ACK:
                    numLAST_ACK++;
                    break;

                case LwipTcpLayer::FIN_WAIT_1:
                    numFIN_WAIT_1++;
                    break;

                case LwipTcpLayer::FIN_WAIT_2:
                    numFIN_WAIT_2++;
                    break;

                case LwipTcpLayer::CLOSING:
                    numCLOSING++;
                    break;

                case LwipTcpLayer::TIME_WAIT:
                    numTIME_WAIT++;
                    break;
            }
        }
    }

    char buf2[200];
    buf2[0] = '\0';
    if (numINIT > 0)
        sprintf(buf2 + strlen(buf2), "init:%d ", numINIT);
    if (numCLOSED > 0)
        sprintf(buf2 + strlen(buf2), "closed:%d ", numCLOSED);
    if (numLISTEN > 0)
        sprintf(buf2 + strlen(buf2), "listen:%d ", numLISTEN);
    if (numSYN_SENT > 0)
        sprintf(buf2 + strlen(buf2), "syn_sent:%d ", numSYN_SENT);
    if (numSYN_RCVD > 0)
        sprintf(buf2 + strlen(buf2), "syn_rcvd:%d ", numSYN_RCVD);
    if (numESTABLISHED > 0)
        sprintf(buf2 + strlen(buf2), "estab:%d ", numESTABLISHED);
    if (numCLOSE_WAIT > 0)
        sprintf(buf2 + strlen(buf2), "close_wait:%d ", numCLOSE_WAIT);
    if (numLAST_ACK > 0)
        sprintf(buf2 + strlen(buf2), "last_ack:%d ", numLAST_ACK);
    if (numFIN_WAIT_1 > 0)
        sprintf(buf2 + strlen(buf2), "fin_wait_1:%d ", numFIN_WAIT_1);
    if (numFIN_WAIT_2 > 0)
        sprintf(buf2 + strlen(buf2), "fin_wait_2:%d ", numFIN_WAIT_2);
    if (numCLOSING > 0)
        sprintf(buf2 + strlen(buf2), "closing:%d ", numCLOSING);
    if (numTIME_WAIT > 0)
        sprintf(buf2 + strlen(buf2), "time_wait:%d ", numTIME_WAIT);

    getDisplayString().setTagArg("t", 0, buf2);
}

TcpLwipConnection *TCP_lwIP::findAppConn(int connIdP)
{
    auto i = tcpAppConnMapM.find(connIdP);
    return i == tcpAppConnMapM.end() ? nullptr : (i->second);
}

void TCP_lwIP::finish()
{
    isAliveM = false;
}

void TCP_lwIP::printConnBrief(TcpLwipConnection& connP)
{
    EV_TRACE << this << ": connId=" << connP.connIdM;
}

void TCP_lwIP::ip_output(LwipTcpLayer::tcp_pcb *pcb, L3Address const& srcP, L3Address const& destP, void *dataP, int lenP)
{
    TcpLwipConnection *conn = (pcb != nullptr) ? (TcpLwipConnection *)(pcb->callback_arg) : nullptr;

    Packet *packet = nullptr;

    if (conn) {
        packet = conn->sendQueueM->createSegmentWithBytes(dataP, lenP);
    }
    else {
        const auto& bytes = std::make_shared<BytesChunk>((const uint8_t*)dataP, lenP);
        bytes->markImmutable();
        packet = new Packet(nullptr, bytes);
        const auto& tcpHdr = packet->popHeader<TcpHeader>();
        packet->removePoppedHeaders();
        int64_t numBytes = packet->getByteLength();
        ASSERT(numBytes == 0);
        packet->pushHeader(tcpHdr);
    }

    const auto& tcpHdr = packet->peekHeader<TcpHeader>();
    ASSERT(tcpHdr);
    ASSERT(packet);

    EV_TRACE << this << ": Sending: conn=" << conn << ", data: " << dataP << " of len " << lenP
             << " from " << srcP << " to " << destP << "\n";

    IL3AddressType *addressType = destP.getAddressType();

    packet->ensureTag<PacketProtocolTag>()->setProtocol(&Protocol::tcp);
    packet->ensureTag<TransportProtocolInd>()->setProtocol(&Protocol::tcp);
    packet->ensureTag<DispatchProtocolReq>()->setProtocol(addressType->getNetworkProtocol());
    auto addresses = packet->ensureTag<L3AddressReq>();
    addresses->setSrcAddress(srcP);
    addresses->setDestAddress(destP);
    if (conn) {
        conn->notifyAboutSending(*tcpHdr);
    }

    EV_INFO << this << ": Send segment: conn ID=" << conn->connIdM << " from " << srcP
            << " to " << destP << " SEQ=" << tcpHdr->getSequenceNo();
    if (tcpHdr->getSynBit())
        EV_INFO << " SYN";
    if (tcpHdr->getAckBit())
        EV_INFO << " ACK=" << tcpHdr->getAckNo();
    if (tcpHdr->getFinBit())
        EV_INFO << " FIN";
    if (tcpHdr->getRstBit())
        EV_INFO << " RST";
    if (tcpHdr->getPshBit())
        EV_INFO << " PSH";
    if (tcpHdr->getUrgBit())
        EV_INFO << " URG";
    EV_INFO << " len=" << packet->getByteLength() - tcpHdr->getHeaderLength() << "\n";

    send(packet, "ipOut");
}

void TCP_lwIP::processAppCommand(TcpLwipConnection& connP, cMessage *msgP)
{
    printConnBrief(connP);

    // first do actions
    TCPCommand *tcpCommand = check_and_cast_nullable<TCPCommand *>(msgP->removeControlInfo());

    switch (msgP->getKind()) {
        case TCP_C_OPEN_ACTIVE:
            process_OPEN_ACTIVE(connP, check_and_cast<TCPOpenCommand *>(tcpCommand), msgP);
            break;

        case TCP_C_OPEN_PASSIVE:
            process_OPEN_PASSIVE(connP, check_and_cast<TCPOpenCommand *>(tcpCommand), msgP);
            break;

        case TCP_C_ACCEPT:
            process_ACCEPT(connP, check_and_cast<TCPAcceptCommand *>(tcpCommand), msgP);
            break;

        case TCP_C_SEND:
            process_SEND(connP, check_and_cast<Packet *>(msgP));
            break;

        case TCP_C_CLOSE:
            ASSERT(tcpCommand);
            process_CLOSE(connP, tcpCommand, msgP);
            break;

        case TCP_C_ABORT:
            ASSERT(tcpCommand);
            process_ABORT(connP, tcpCommand, msgP);
            break;

        case TCP_C_STATUS:
            ASSERT(tcpCommand);
            process_STATUS(connP, tcpCommand, msgP);
            break;

        default:
            throw cRuntimeError("Wrong command from app: %d", msgP->getKind());
    }
}

void TCP_lwIP::process_OPEN_ACTIVE(TcpLwipConnection& connP, TCPOpenCommand *tcpCommandP,
        cMessage *msgP)
{
    if (tcpCommandP->getRemoteAddr().isUnspecified() || tcpCommandP->getRemotePort() == -1)
        throw cRuntimeError("Error processing command OPEN_ACTIVE: remote address and port must be specified");

    ASSERT(pLwipTcpLayerM);

    int localPort = tcpCommandP->getLocalPort();
    if (localPort == -1)
        localPort = 0;

    EV_INFO << this << ": OPEN: "
            << tcpCommandP->getLocalAddr() << ":" << localPort << " --> "
            << tcpCommandP->getRemoteAddr() << ":" << tcpCommandP->getRemotePort() << "\n";
    connP.connect(tcpCommandP->getLocalAddr(), localPort,
            tcpCommandP->getRemoteAddr(), tcpCommandP->getRemotePort());

    delete tcpCommandP;
    delete msgP;
}

void TCP_lwIP::process_OPEN_PASSIVE(TcpLwipConnection& connP, TCPOpenCommand *tcpCommandP,
        cMessage *msgP)
{
    ASSERT(pLwipTcpLayerM);

    ASSERT(tcpCommandP->getFork() == true);

    if (tcpCommandP->getLocalPort() == -1)
        throw cRuntimeError("Error processing command OPEN_PASSIVE: local port must be specified");

    EV_INFO << this << "Starting to listen on: " << tcpCommandP->getLocalAddr() << ":"
            << tcpCommandP->getLocalPort() << "\n";

    /*
       process passive open request
     */

    connP.listen(tcpCommandP->getLocalAddr(), tcpCommandP->getLocalPort());

    delete tcpCommandP;
    delete msgP;
}

void TCP_lwIP::process_ACCEPT(TcpLwipConnection& connP, TCPAcceptCommand *tcpCommand, cMessage *msg)
{
    connP.accept();
    delete tcpCommand;
    delete msg;
}

void TCP_lwIP::process_SEND(TcpLwipConnection& connP, Packet *msgP)
{
    EV_INFO << this << ": processing SEND command, len=" << msgP->getByteLength() << endl;

    connP.send(msgP);
}

void TCP_lwIP::process_CLOSE(TcpLwipConnection& connP, TCPCommand *tcpCommandP, cMessage *msgP)
{
    EV_INFO << this << ": processing CLOSE(" << connP.connIdM << ") command\n";

    delete tcpCommandP;
    delete msgP;

    connP.close();
}

void TCP_lwIP::process_ABORT(TcpLwipConnection& connP, TCPCommand *tcpCommandP, cMessage *msgP)
{
    EV_INFO << this << ": processing ABORT(" << connP.connIdM << ") command\n";

    delete tcpCommandP;
    delete msgP;

    connP.abort();
}

void TCP_lwIP::process_STATUS(TcpLwipConnection& connP, TCPCommand *tcpCommandP, cMessage *msgP)
{
    EV_INFO << this << ": processing STATUS(" << connP.connIdM << ") command\n";

    delete tcpCommandP;    // but we'll reuse msg for reply

    TCPStatusInfo *statusInfo = new TCPStatusInfo();
    connP.fillStatusInfo(*statusInfo);
    msgP->setControlInfo(statusInfo);
    msgP->setKind(TCP_I_STATUS);
    send(msgP, "appOut");
}

TcpLwipSendQueue *TCP_lwIP::createSendQueue(TCPDataTransferMode transferModeP)
{
    return new TcpLwipSendQueue();
}

TcpLwipReceiveQueue *TCP_lwIP::createReceiveQueue(TCPDataTransferMode transferModeP)
{
    return new TcpLwipReceiveQueue();
}

bool TCP_lwIP::handleOperationStage(LifecycleOperation *operation, int stage, IDoneCallback *doneCallback)
{
    Enter_Method_Silent();

    throw cRuntimeError("Unsupported lifecycle operation '%s'", operation->getClassName());
    return true;
}

} // namespace tcp

} // namespace inet

