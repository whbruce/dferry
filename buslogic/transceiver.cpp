/*
   Copyright (C) 2013 Andreas Hartmetz <ahartmetz@gmail.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LGPL.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.

   Alternatively, this file is available under the Mozilla Public License
   Version 1.1.  You may obtain a copy of the License at
   http://www.mozilla.org/MPL/
*/

#include "transceiver.h"
#include "transceiver_p.h"

#include "argumentlist.h"
#include "authnegotiator.h"
#include "icompletionclient.h"
#include "itransceiverclient.h"
#include "localsocket.h"
#include "message.h"
#include "pendingreply.h"
#include "pendingreply_p.h"

#include <cassert>
#include <iostream>

using namespace std;

class HelloReceiver : public ICompletionClient
{
public:
    void notifyCompletion(void *task) override;
    PendingReply m_helloReply; // keep it here so it conveniently goes away when it's done
    TransceiverPrivate *m_parent;
};

void HelloReceiver::notifyCompletion(void *)
{
    m_parent->processHello();
}

TransceiverPrivate::TransceiverPrivate(EventDispatcher *dispatcher, const PeerAddress &peer)
   : m_client(nullptr),
     m_receivingMessage(nullptr),
     m_sendSerial(1),
     m_defaultTimeout(25000),
     m_connection(nullptr),
     m_mainThreadTransceiver(nullptr),
     m_helloReceiver(nullptr),
     m_eventDispatcher(dispatcher),
     m_peerAddress(peer),
     m_authNegotiator(nullptr)
{
}

Transceiver::Transceiver(EventDispatcher *dispatcher, const PeerAddress &peer)
   : d(new TransceiverPrivate(dispatcher, peer))
{
    cout << "session bus socket type: " << peer.socketType() << '\n';
    cout << "session bus path: " << peer.path() << '\n';
    d->m_connection = IConnection::create(peer);
    d->m_connection->setEventDispatcher(dispatcher);
    cout << "connection is " << (d->m_connection->isOpen() ? "open" : "closed") << ".\n";
    d->authAndHello(this);
}

Transceiver::~Transceiver()
{
    delete d->m_connection;
    delete d->m_authNegotiator;
    delete d->m_helloReceiver;
    // TODO delete d->m_receivingMessage ?

    delete d;
    d = nullptr;
}

void TransceiverPrivate::authAndHello(Transceiver *parent)
{
    m_authNegotiator = new AuthNegotiator(m_connection);
    m_authNegotiator->setCompletionClient(this);

    // Announce our presence to the bus and have it send some introductory information of its own
    Message *hello = new Message();
    hello->setType(Message::MethodCallMessage);
    hello->setExpectsReply(false);
    hello->setDestination(std::string("org.freedesktop.DBus"));
    hello->setInterface(std::string("org.freedesktop.DBus"));
    hello->setPath(std::string("/org/freedesktop/DBus"));
    hello->setMethod(std::string("Hello"));

    m_helloReceiver = new HelloReceiver;
    m_helloReceiver->m_helloReply = parent->send(hello);
    m_helloReceiver->m_helloReply.setCompletionClient(m_helloReceiver);
    m_helloReceiver->m_parent = this;
}

void TransceiverPrivate::processHello()
{
    assert(m_helloReceiver->m_helloReply.hasNonErrorReply()); // TODO real error handling (more below)
    // ### following line is ugly and slow!! Indicates a need for better API.
    ArgumentList argList = m_helloReceiver->m_helloReply.reply().argumentList();
    delete m_helloReceiver;
    m_helloReceiver = nullptr;

    ArgumentList::Reader reader = argList.beginRead();
    cstring busName = reader.readString();
    assert(reader.state() == ArgumentList::Finished);
    cout << "teh bus name is:" << busName.begin << endl;
}

void Transceiver::setDefaultReplyTimeout(int msecs)
{
    d->m_defaultTimeout = msecs;
}

int Transceiver::defaultReplyTimeout() const
{
    return d->m_defaultTimeout;
}

PendingReply Transceiver::send(Message *m, int timeoutMsecs)
{
    if (timeoutMsecs == DefaultTimeout) {
        timeoutMsecs = d->m_defaultTimeout;
    }

    PendingReplyPrivate *pendingPriv = new PendingReplyPrivate(d->m_eventDispatcher, timeoutMsecs);
    pendingPriv->m_transceiver = d;
    pendingPriv->m_client = nullptr;
    pendingPriv->m_serial = d->m_sendSerial;
    d->m_pendingReplies.emplace(d->m_sendSerial, pendingPriv);

    sendNoReply(m);

    return PendingReply(pendingPriv);
}

PendingReply::Error Transceiver::sendNoReply(Message *m)
{
    // ### (when not called from send()) warn if sending a message without the noreply flag set?
    //     doing that is wasteful, but might be common. needs investigation.
    m->setSerial(d->m_sendSerial++);
    d->m_sendQueue.push_back(m);
    m->setCompletionClient(d);
    if (!d->m_authNegotiator && d->m_sendQueue.size() == 1) {
        m->send(d->m_connection);
    }
    return PendingReply::Error::None;// ###
}

IConnection *Transceiver::connection() const
{
    return d->m_connection;
}

EventDispatcher *Transceiver::eventDispatcher() const
{
    return d->m_eventDispatcher;
}

ITransceiverClient *Transceiver::client() const
{
    return d->m_client;
}

void Transceiver::setClient(ITransceiverClient *client)
{
    d->m_client = client;
}

void TransceiverPrivate::notifyCompletion(void *task)
{
    if (m_authNegotiator) {
        assert(task == m_authNegotiator);
        delete m_authNegotiator;
        m_authNegotiator = nullptr;
        // cout << "Authenticated.\n";
        assert(!m_sendQueue.empty()); // the hello message should be in the queue
        m_sendQueue.front()->send(m_connection);
        receiveNextMessage();
    } else {
        if (!m_sendQueue.empty() && task == m_sendQueue.front()) {
            // cout << "Sent message.\n";
            delete m_sendQueue.front();
            m_sendQueue.pop_front();
            if (!m_sendQueue.empty()) {
                m_sendQueue.front()->send(m_connection);
            }
        } else {
            assert(task == m_receivingMessage);
            Message *const receivedMessage = m_receivingMessage;
            cout << "Received message; type:" << receivedMessage->type() << "\n";

            receiveNextMessage();

            bool replyDispatched = false;

            if (receivedMessage->type() == Message::MethodReturnMessage ||
                receivedMessage->type() == Message::ErrorMessage) {
                uint32 rs = receivedMessage->replySerial();
                cout << "Received message: looking for pending reply for serial " << rs << "\n";
                auto it = m_pendingReplies.find(rs);
                if (it != m_pendingReplies.end()) {
                    replyDispatched = true;
                    PendingReplyPrivate *pr = it->second;
                    m_pendingReplies.erase(it);

                    cout << "Received message: dispatching to PendingReply.\n";
                    cout << "Received message: argumentList A " << receivedMessage->argumentList().prettyPrint() << '\n';
                    pr->m_reply = receivedMessage;
                    cout << "Received message: argumentList B " << pr->m_reply->argumentList().prettyPrint() << '\n';
                    pr->notifyDone();
                }
            }

            if (!replyDispatched) {
                if (m_client) {
                    cout << "Received message: dispatching to catch-all.\n";
                    m_client->messageReceived(receivedMessage);
                } else {
                    cerr << "warning, dropping message on the floor because no client is registered.\n";
                    delete receivedMessage;
                }
            }

        }
    }
}

void TransceiverPrivate::receiveNextMessage()
{
    m_receivingMessage = new Message;
    m_receivingMessage->setCompletionClient(this);
    m_receivingMessage->receive(m_connection);
}

void TransceiverPrivate::unregisterPendingReply(PendingReplyPrivate *p)
{
#ifndef NDEBUG
    auto it = m_pendingReplies.find(p->m_serial);
    assert(it != m_pendingReplies.end());
    assert(it->second == p);
#endif
    m_pendingReplies.erase(p->m_serial);
}
