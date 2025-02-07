/***************************************************************************
 *   Copyright (C) 2005-2015 by the Quassel Project                        *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
 ***************************************************************************/

#include "coresessioneventprocessor.h"

#include "coreirclisthelper.h"
#include "corenetwork.h"
#include "coresession.h"
#include "coretransfer.h"
#include "coretransfermanager.h"
#include "ctcpevent.h"
#include "ircevent.h"
#include "ircuser.h"
#include "logger.h"
#include "messageevent.h"
#include "netsplit.h"
#include "quassel.h"

#ifdef HAVE_QCA2
#  include "keyevent.h"
#endif

CoreSessionEventProcessor::CoreSessionEventProcessor(CoreSession *session)
    : BasicHandler("handleCtcp", session),
    _coreSession(session)
{
    connect(coreSession(), SIGNAL(networkDisconnected(NetworkId)), this, SLOT(destroyNetsplits(NetworkId)));
    connect(this, SIGNAL(newEvent(Event *)), coreSession()->eventManager(), SLOT(postEvent(Event *)));
}


bool CoreSessionEventProcessor::checkParamCount(IrcEvent *e, int minParams)
{
    if (e->params().count() < minParams) {
        if (e->type() == EventManager::IrcEventNumeric) {
            qWarning() << "Command " << static_cast<IrcEventNumeric *>(e)->number() << " requires " << minParams << "params, got: " << e->params();
        }
        else {
            QString name = coreSession()->eventManager()->enumName(e->type());
            qWarning() << qPrintable(name) << "requires" << minParams << "params, got:" << e->params();
        }
        e->stop();
        return false;
    }
    return true;
}


void CoreSessionEventProcessor::tryNextNick(NetworkEvent *e, const QString &errnick, bool erroneus)
{
    QStringList desiredNicks = coreSession()->identity(e->network()->identity())->nicks();
    int nextNickIdx = desiredNicks.indexOf(errnick) + 1;
    QString nextNick;
    if (nextNickIdx > 0 && desiredNicks.size() > nextNickIdx) {
        nextNick = desiredNicks[nextNickIdx];
    }
    else {
        if (erroneus) {
            // FIXME Make this an ErrorEvent or something like that, so it's translated in the client
            MessageEvent *msgEvent = new MessageEvent(Message::Error, e->network(),
                tr("No free and valid nicks in nicklist found. use: /nick <othernick> to continue"),
                QString(), QString(), Message::None, e->timestamp());
            emit newEvent(msgEvent);
            return;
        }
        else {
            nextNick = errnick + "_";
        }
    }
    // FIXME Use a proper output event for this
    coreNetwork(e)->putRawLine("NICK " + coreNetwork(e)->encodeServerString(nextNick));
}


void CoreSessionEventProcessor::processIrcEventNumeric(IrcEventNumeric *e)
{
    switch (e->number()) {
    // SASL authentication replies
    // See: http://ircv3.net/specs/extensions/sasl-3.1.html
    // TODO Handle errors to stop connection if appropriate
    case 903:  // RPL_SASLSUCCESS
    case 904:  // ERR_SASLFAIL
    case 905:  // ERR_SASLTOOLONG
    case 906:  // ERR_SASLABORTED
    case 907:  // ERR_SASLALREADY
        // Move on to the next capability
        sendNextCap(e->network());
        break;

    default:
        break;
    }
}


void CoreSessionEventProcessor::processIrcEventAuthenticate(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    if (e->params().at(0) != "+") {
        qWarning() << "Invalid AUTHENTICATE" << e;
        return;
    }

    CoreNetwork *net = coreNetwork(e);

#ifdef HAVE_SSL
    if (net->identityPtr()->sslCert().isNull()) {
#endif
        QString construct = net->saslAccount();
        construct.append(QChar(QChar::Null));
        construct.append(net->saslAccount());
        construct.append(QChar(QChar::Null));
        construct.append(net->saslPassword());
        QByteArray saslData = QByteArray(construct.toLatin1().toBase64());
        saslData.prepend("AUTHENTICATE ");
        net->putRawLine(saslData);
#ifdef HAVE_SSL
    } else {
        net->putRawLine("AUTHENTICATE +");
    }
#endif
}

void CoreSessionEventProcessor::sendNextCap(Network *net)
{
    CoreNetwork *coreNet = qobject_cast<CoreNetwork *>(net);
    if (coreNet->capNegotiationInProgress()) {
        // Request the next capability and remove it from the list
        // Handle one at a time so one capability failing won't NAK all of 'em
        coreNet->putRawLine(coreNet->serverEncode(QString("CAP REQ :%1").arg(coreNet->takeQueuedCap())));
    } else {
        // If SASL requested but not available, print a warning
        if (coreNet->networkInfo().useSasl && !coreNet->useCapSASL())
            emit newEvent(new MessageEvent(Message::Error, net, tr("SASL authentication not supported by server, continuing without"), QString(), QString(), Message::None, QDateTime::currentDateTimeUtc()));

        // No pending desired capabilities, end negotiation
        coreNet->putRawLine(coreNet->serverEncode(QString("CAP END")));
        emit newEvent(new MessageEvent(Message::Server, net, tr("Capability negotiation finished"), QString(), QString(), Message::None, QDateTime::currentDateTimeUtc()));
    }
}

void CoreSessionEventProcessor::processIrcEventCap(IrcEvent *e)
{
    // Handle capability negotiation
    // See: http://ircv3.net/specs/core/capability-negotiation-3.2.html
    // And: http://ircv3.net/specs/core/capability-negotiation-3.1.html
    if (e->params().count() >= 3) {
        CoreNetwork *coreNet = coreNetwork(e);
        if (e->params().at(1).compare("LS", Qt::CaseInsensitive) == 0) {
            // Server: CAP * LS * :multi-prefix extended-join account-notify batch invite-notify tls
            // Server: CAP * LS * :cap-notify server-time example.org/dummy-cap=dummyvalue example.org/second-dummy-cap
            // Server: CAP * LS :userhost-in-names sasl=EXTERNAL,DH-AES,DH-BLOWFISH,ECDSA-NIST256P-CHALLENGE,PLAIN
            bool capListFinished;
            QStringList availableCaps;
            if (e->params().count() == 4) {
                // Middle of multi-line reply, ignore the asterisk
                capListFinished = false;
                availableCaps = e->params().at(3).split(' ');
            } else {
                // Single line reply
                capListFinished = true;
                availableCaps = e->params().at(2).split(' ');
            }
            // We know what capabilities are available, request what we want.
            QStringList availableCapPair;
            bool queueCurrentCap;
            for (int i = 0; i < availableCaps.count(); ++i) {
                // Capability may include values, e.g. CAP * LS :multi-prefix sasl=EXTERNAL
                availableCapPair = availableCaps[i].trimmed().split('=');
                queueCurrentCap = false;
                if (availableCapPair.at(0).startsWith("sasl")) {
                    // Only request SASL if it's enabled
                    if (coreNet->networkInfo().useSasl)
                        queueCurrentCap = true;
                } else if (availableCapPair.at(0).startsWith("away-notify") ||
                           availableCapPair.at(0).startsWith("account-notify") ||
                           availableCapPair.at(0).startsWith("extended-join") ||
                           availableCapPair.at(0).startsWith("userhost-in-names") ||
                           availableCapPair.at(0).startsWith("multi-prefix")) {
                    // Always request these capabilities if available
                    queueCurrentCap = true;
                }
                if (queueCurrentCap) {
                    if(availableCapPair.count() >= 2)
                        coreNet->queuePendingCap(availableCapPair.at(0).trimmed(), availableCapPair.at(1).trimmed());
                    else
                        coreNet->queuePendingCap(availableCapPair.at(0).trimmed());
                }
            }
            // Begin capability requests when capability listing complete
            if (capListFinished) {
                emit newEvent(new MessageEvent(Message::Server, e->network(), tr("Negotiating capabilities..."), QString(), QString(), Message::None, e->timestamp()));
                sendNextCap(coreNet);
            }
        } else if (e->params().at(1).compare("ACK", Qt::CaseInsensitive) == 0) {
            // Server: CAP * ACK :multi-prefix sasl
            // Got the capability we want, enable, handle as needed.
            // As only one capability is requested at a time, no need to split
            // Lower-case the list to make later comparisons easier
            // Capability may include values, e.g. CAP * LS :multi-prefix sasl=EXTERNAL
            QStringList acceptedCap = e->params().at(2).trimmed().split('=');

            // Mark this cap as accepted
            if(acceptedCap.count() >= 2)
                coreNet->addCap(acceptedCap.at(0), acceptedCap.at(1));
            else
                coreNet->addCap(acceptedCap.at(0));

            // Handle special cases
            if (acceptedCap.at(0).startsWith("sasl")) {
                // Freenode (at least) sends "sasl " with a trailing space for some reason!
                // if the current identity has a cert set, use SASL EXTERNAL
                // FIXME use event
                // TODO If value of sasl capability is not empty, limit to accepted
#ifdef HAVE_SSL
                if (!coreNet->identityPtr()->sslCert().isNull()) {
                    coreNet->putRawLine(coreNet->serverEncode("AUTHENTICATE EXTERNAL"));
                } else {
#endif
                    // Only working with PLAIN atm, blowfish later
                    coreNet->putRawLine(coreNet->serverEncode("AUTHENTICATE PLAIN"));
#ifdef HAVE_SSL
                }
#endif
            } else {
                // Special handling not needed, move on to next cap
                sendNextCap(coreNet);
            }
        } else if (e->params().at(1).compare("NAK", Qt::CaseInsensitive) == 0) {
            // Something went wrong with this capability, disable, go to next cap
            // As only one capability is requested at a time, no need to split
            // Lower-case the list to make later comparisons easier
            QString deniedCap = e->params().at(2).trimmed();
            coreNet->removeCap(deniedCap);
            sendNextCap(coreNet);
        }
    }
}

/* IRCv3 account-notify
 * Log in:  ":nick!user@host ACCOUNT accountname"
 * Log out: ":nick!user@host ACCOUNT *" */
void CoreSessionEventProcessor::processIrcEventAccount(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    IrcUser *ircuser = e->network()->updateNickFromMask(e->prefix());
    if (ircuser) {
        // FIXME Keep track of authed user account, requires adding support to ircuser.h/cpp
        /*
        if (e->params().at(0) != "*") {
            // Account logged in
            qDebug() << "account-notify:" << ircuser->nick() << "logged in to" << e->params().at(0);
        } else {
            // Account logged out
            qDebug() << "account-notify:" << ircuser->nick() << "logged out";
        }
        */
    } else {
        qDebug() << "Received account-notify data for unknown user" << e->prefix();
    }
}

/* IRCv3 away-notify - ":nick!user@host AWAY [:message]" */
void CoreSessionEventProcessor::processIrcEventAway(IrcEvent *e)
{
    if (!checkParamCount(e, 2))
        return;

    // Nick is sent as part of parameters in order to split user/server decoding
    IrcUser *ircuser = e->network()->ircUser(e->params().at(0));
    if (ircuser) {
        if (!e->params().at(1).isEmpty()) {
            ircuser->setAway(true);
            ircuser->setAwayMessage(e->params().at(1));
        } else {
            ircuser->setAway(false);
        }
    } else {
        qDebug() << "Received away-notify data for unknown user" << e->params().at(0);
    }
}

void CoreSessionEventProcessor::processIrcEventInvite(IrcEvent *e)
{
    if (checkParamCount(e, 2)) {
        e->network()->updateNickFromMask(e->prefix());
    }
}


void CoreSessionEventProcessor::processIrcEventJoin(IrcEvent *e)
{
    if (e->testFlag(EventManager::Fake)) // generated by handleEarlyNetsplitJoin
        return;

    if (!checkParamCount(e, 1))
        return;

    CoreNetwork *net = coreNetwork(e);
    QString channel = e->params()[0];
    IrcUser *ircuser = net->updateNickFromMask(e->prefix());

    if (net->useCapExtendedJoin()) {
        if (!checkParamCount(e, 3))
            return;
        // If logged in, :nick!user@host JOIN #channelname accountname :Real Name
        // If logged out, :nick!user@host JOIN #channelname * :Real Name
        // See:  http://ircv3.net/specs/extensions/extended-join-3.1.html
        // FIXME Keep track of authed user account, requires adding support to ircuser.h/cpp
        ircuser->setRealName(e->params()[2]);
    }
    // Else :nick!user@host JOIN #channelname

    bool handledByNetsplit = false;
    foreach(Netsplit* n, _netsplits.value(e->network())) {
        handledByNetsplit = n->userJoined(e->prefix(), channel);
        if (handledByNetsplit)
            break;
    }

    // If using away-notify, check new users.  Works around buggy IRC servers
    // forgetting to send :away messages for users who join channels when away.
    if (coreNetwork(e)->useCapAwayNotify()) {
        coreNetwork(e)->queueAutoWhoOneshot(ircuser->nick());
    }

    if (!handledByNetsplit)
        ircuser->joinChannel(channel);
    else
        e->setFlag(EventManager::Netsplit);

    if (net->isMe(ircuser)) {
        net->setChannelJoined(channel);
        // FIXME use event
        net->putRawLine(net->serverEncode("MODE " + channel)); // we want to know the modes of the channel we just joined, so we ask politely
    }
}


void CoreSessionEventProcessor::lateProcessIrcEventKick(IrcEvent *e)
{
    if (checkParamCount(e, 2)) {
        e->network()->updateNickFromMask(e->prefix());
        IrcUser *victim = e->network()->ircUser(e->params().at(1));
        if (victim) {
            victim->partChannel(e->params().at(0));
            //if(e->network()->isMe(victim)) e->network()->setKickedFromChannel(channel);
        }
    }
}


void CoreSessionEventProcessor::processIrcEventMode(IrcEvent *e)
{
    if (!checkParamCount(e, 2))
        return;

    if (e->network()->isChannelName(e->params().first())) {
        // Channel Modes

        IrcChannel *channel = e->network()->ircChannel(e->params()[0]);
        if (!channel) {
            // we received mode information for a channel we're not in. that means probably we've just been kicked out or something like that
            // anyways: we don't have a place to store the data --> discard the info.
            return;
        }

        QString modes = e->params()[1];
        bool add = true;
        int paramOffset = 2;
        for (int c = 0; c < modes.length(); c++) {
            if (modes[c] == '+') {
                add = true;
                continue;
            }
            if (modes[c] == '-') {
                add = false;
                continue;
            }

            if (e->network()->prefixModes().contains(modes[c])) {
                // user channel modes (op, voice, etc...)
                if (paramOffset < e->params().count()) {
                    IrcUser *ircUser = e->network()->ircUser(e->params()[paramOffset]);
                    if (!ircUser) {
                        qWarning() << Q_FUNC_INFO << "Unknown IrcUser:" << e->params()[paramOffset];
                    }
                    else {
                        if (add) {
                            bool handledByNetsplit = false;
                            QHash<QString, Netsplit *> splits = _netsplits.value(e->network());
                            foreach(Netsplit* n, _netsplits.value(e->network())) {
                                handledByNetsplit = n->userAlreadyJoined(ircUser->hostmask(), channel->name());
                                if (handledByNetsplit) {
                                    n->addMode(ircUser->hostmask(), channel->name(), QString(modes[c]));
                                    break;
                                }
                            }
                            if (!handledByNetsplit)
                                channel->addUserMode(ircUser, QString(modes[c]));
                        }
                        else
                            channel->removeUserMode(ircUser, QString(modes[c]));
                    }
                }
                else {
                    qWarning() << "Received MODE with too few parameters:" << e->params();
                }
                ++paramOffset;
            }
            else {
                // regular channel modes
                QString value;
                Network::ChannelModeType modeType = e->network()->channelModeType(modes[c]);
                if (modeType == Network::A_CHANMODE || modeType == Network::B_CHANMODE || (modeType == Network::C_CHANMODE && add)) {
                    if (paramOffset < e->params().count()) {
                        value = e->params()[paramOffset];
                    }
                    else {
                        qWarning() << "Received MODE with too few parameters:" << e->params();
                    }
                    ++paramOffset;
                }

                if (add)
                    channel->addChannelMode(modes[c], value);
                else
                    channel->removeChannelMode(modes[c], value);
            }
        }
    }
    else {
        // pure User Modes
        IrcUser *ircUser = e->network()->newIrcUser(e->params().first());
        QString modeString(e->params()[1]);
        QString addModes;
        QString removeModes;
        bool add = false;
        for (int c = 0; c < modeString.count(); c++) {
            if (modeString[c] == '+') {
                add = true;
                continue;
            }
            if (modeString[c] == '-') {
                add = false;
                continue;
            }
            if (add)
                addModes += modeString[c];
            else
                removeModes += modeString[c];
        }
        if (!addModes.isEmpty())
            ircUser->addUserModes(addModes);
        if (!removeModes.isEmpty())
            ircUser->removeUserModes(removeModes);

        if (e->network()->isMe(ircUser)) {
            coreNetwork(e)->updatePersistentModes(addModes, removeModes);
        }
    }
}


void CoreSessionEventProcessor::lateProcessIrcEventNick(IrcEvent *e)
{
    if (checkParamCount(e, 1)) {
        IrcUser *ircuser = e->network()->updateNickFromMask(e->prefix());
        if (!ircuser) {
            qWarning() << Q_FUNC_INFO << "Unknown IrcUser!";
            return;
        }
        QString newnick = e->params().at(0);
        QString oldnick = ircuser->nick();

        // the order is cruicial
        // otherwise the client would rename the buffer, see that the assigned ircuser doesn't match anymore
        // and remove the ircuser from the querybuffer leading to a wrong on/offline state
        ircuser->setNick(newnick);
        coreSession()->renameBuffer(e->networkId(), newnick, oldnick);
    }
}


void CoreSessionEventProcessor::lateProcessIrcEventPart(IrcEvent *e)
{
    if (checkParamCount(e, 1)) {
        IrcUser *ircuser = e->network()->updateNickFromMask(e->prefix());
        if (!ircuser) {
            qWarning() << Q_FUNC_INFO<< "Unknown IrcUser!";
            return;
        }
        QString channel = e->params().at(0);
        ircuser->partChannel(channel);
        if (e->network()->isMe(ircuser))
            qobject_cast<CoreNetwork *>(e->network())->setChannelParted(channel);
    }
}


void CoreSessionEventProcessor::processIrcEventPing(IrcEvent *e)
{
    QString param = e->params().count() ? e->params().first() : QString();
    // FIXME use events
    coreNetwork(e)->putRawLine("PONG " + coreNetwork(e)->serverEncode(param));
}


void CoreSessionEventProcessor::processIrcEventPong(IrcEvent *e)
{
    // the server is supposed to send back what we passed as param. and we send a timestamp
    // but using quote and whatnought one can send arbitrary pings, so we have to do some sanity checks
    if (checkParamCount(e, 2)) {
        QString timestamp = e->params().at(1);
        QTime sendTime = QTime::fromString(timestamp, "hh:mm:ss.zzz");
        if (sendTime.isValid())
            e->network()->setLatency(sendTime.msecsTo(QTime::currentTime()) / 2);
    }
}


void CoreSessionEventProcessor::processIrcEventQuit(IrcEvent *e)
{
    IrcUser *ircuser = e->network()->updateNickFromMask(e->prefix());
    if (!ircuser)
        return;

    QString msg;
    if (e->params().count() > 0)
        msg = e->params()[0];

    // check if netsplit
    if (Netsplit::isNetsplit(msg)) {
        Netsplit *n;
        if (!_netsplits[e->network()].contains(msg)) {
            n = new Netsplit(e->network(), this);
            connect(n, SIGNAL(finished()), this, SLOT(handleNetsplitFinished()));
            connect(n, SIGNAL(netsplitJoin(Network*, QString, QStringList, QStringList, QString)),
                this, SLOT(handleNetsplitJoin(Network*, QString, QStringList, QStringList, QString)));
            connect(n, SIGNAL(netsplitQuit(Network*, QString, QStringList, QString)),
                this, SLOT(handleNetsplitQuit(Network*, QString, QStringList, QString)));
            connect(n, SIGNAL(earlyJoin(Network*, QString, QStringList, QStringList)),
                this, SLOT(handleEarlyNetsplitJoin(Network*, QString, QStringList, QStringList)));
            _netsplits[e->network()].insert(msg, n);
        }
        else {
            n = _netsplits[e->network()][msg];
        }
        // add this user to the netsplit
        n->userQuit(e->prefix(), ircuser->channels(), msg);
        e->setFlag(EventManager::Netsplit);
    }
    // normal quit is handled in lateProcessIrcEventQuit()
}


void CoreSessionEventProcessor::lateProcessIrcEventQuit(IrcEvent *e)
{
    if (e->testFlag(EventManager::Netsplit))
        return;

    IrcUser *ircuser = e->network()->updateNickFromMask(e->prefix());
    if (!ircuser)
        return;

    ircuser->quit();
}


void CoreSessionEventProcessor::processIrcEventTopic(IrcEvent *e)
{
    if (checkParamCount(e, 2)) {
        e->network()->updateNickFromMask(e->prefix());
        IrcChannel *channel = e->network()->ircChannel(e->params().at(0));
        if (channel)
            channel->setTopic(e->params().at(1));
    }
}


#ifdef HAVE_QCA2
void CoreSessionEventProcessor::processKeyEvent(KeyEvent *e)
{
    if (!Cipher::neededFeaturesAvailable()) {
        emit newEvent(new MessageEvent(Message::Error, e->network(), tr("Unable to perform key exchange, missing qca-ossl plugin."), e->prefix(), e->target(), Message::None, e->timestamp()));
        return;
    }
    CoreNetwork *net = qobject_cast<CoreNetwork*>(e->network());
    Cipher *c = net->cipher(e->target());
    if (!c) // happens when there is no CoreIrcChannel for the target (i.e. never?)
        return;

    if (e->exchangeType() == KeyEvent::Init) {
        QByteArray pubKey = c->parseInitKeyX(e->key());
        if (pubKey.isEmpty()) {
            emit newEvent(new MessageEvent(Message::Error, e->network(), tr("Unable to parse the DH1080_INIT. Key exchange failed."), e->prefix(), e->target(), Message::None, e->timestamp()));
            return;
        } else {
            net->setCipherKey(e->target(), c->key());
            emit newEvent(new MessageEvent(Message::Info, e->network(), tr("Your key is set and messages will be encrypted."), e->prefix(), e->target(), Message::None, e->timestamp()));
            QList<QByteArray> p;
            p << net->serverEncode(e->target()) << net->serverEncode("DH1080_FINISH ")+pubKey;
            net->putCmd("NOTICE", p);
        }
    } else {
        if (c->parseFinishKeyX(e->key())) {
            net->setCipherKey(e->target(), c->key());
            emit newEvent(new MessageEvent(Message::Info, e->network(), tr("Your key is set and messages will be encrypted."), e->prefix(), e->target(), Message::None, e->timestamp()));
        } else {
            emit newEvent(new MessageEvent(Message::Info, e->network(), tr("Failed to parse DH1080_FINISH. Key exchange failed."), e->prefix(), e->target(), Message::None, e->timestamp()));
        }
    }
}
#endif


/* RPL_WELCOME */
void CoreSessionEventProcessor::processIrcEvent001(IrcEventNumeric *e)
{
    e->network()->setCurrentServer(e->prefix());
    e->network()->setMyNick(e->target());
}


/* RPL_ISUPPORT */
// TODO Complete 005 handling, also use sensible defaults for non-sent stuff
void CoreSessionEventProcessor::processIrcEvent005(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    QString key, value;
    for (int i = 0; i < e->params().count() - 1; i++) {
        QString key = e->params()[i].section("=", 0, 0);
        QString value = e->params()[i].section("=", 1);
        e->network()->addSupport(key, value);
    }

    /* determine our prefixes here to get an accurate result */
    e->network()->determinePrefixes();
}


/* RPL_UMODEIS - "<user_modes> [<user_mode_params>]" */
void CoreSessionEventProcessor::processIrcEvent221(IrcEvent *)
{
    // TODO: save information in network object
}


/* RPL_STATSCONN - "Highest connection cout: 8000 (7999 clients)" */
void CoreSessionEventProcessor::processIrcEvent250(IrcEvent *)
{
    // TODO: save information in network object
}


/* RPL_LOCALUSERS - "Current local user: 5024  Max: 7999 */
void CoreSessionEventProcessor::processIrcEvent265(IrcEvent *)
{
    // TODO: save information in network object
}


/* RPL_GLOBALUSERS - "Current global users: 46093  Max: 47650" */
void CoreSessionEventProcessor::processIrcEvent266(IrcEvent *)
{
    // TODO: save information in network object
}


/*
WHOIS-Message:
   Replies 311 - 313, 317 - 319 are all replies generated in response to a WHOIS message.
  and 301 (RPL_AWAY)
              "<nick> :<away message>"
WHO-Message:
   Replies 352 and 315 paired are used to answer a WHO message.

WHOWAS-Message:
   Replies 314 and 369 are responses to a WHOWAS message.

*/

/* RPL_AWAY - "<nick> :<away message>" */
void CoreSessionEventProcessor::processIrcEvent301(IrcEvent *e)
{
    if (!checkParamCount(e, 2))
        return;

    IrcUser *ircuser = e->network()->ircUser(e->params().at(0));
    if (ircuser) {
        ircuser->setAway(true);
        ircuser->setAwayMessage(e->params().at(1));
        //ircuser->setLastAwayMessage(now);
    }
}


/* RPL_UNAWAY - ":You are no longer marked as being away" */
void CoreSessionEventProcessor::processIrcEvent305(IrcEvent *e)
{
    IrcUser *me = e->network()->me();
    if (me)
        me->setAway(false);

    if (e->network()->autoAwayActive()) {
        e->network()->setAutoAwayActive(false);
        e->setFlag(EventManager::Silent);
    }
}


/* RPL_NOWAWAY - ":You have been marked as being away" */
void CoreSessionEventProcessor::processIrcEvent306(IrcEvent *e)
{
    IrcUser *me = e->network()->me();
    if (me)
        me->setAway(true);
}


/* RPL_WHOISSERVICE - "<user> is registered nick" */
void CoreSessionEventProcessor::processIrcEvent307(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    IrcUser *ircuser = e->network()->ircUser(e->params().at(0));
    if (ircuser)
        ircuser->setWhoisServiceReply(e->params().join(" "));
}


/* RPL_SUSERHOST - "<user> is available for help." */
void CoreSessionEventProcessor::processIrcEvent310(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    IrcUser *ircuser = e->network()->ircUser(e->params().at(0));
    if (ircuser)
        ircuser->setSuserHost(e->params().join(" "));
}


/*  RPL_WHOISUSER - "<nick> <user> <host> * :<real name>" */
void CoreSessionEventProcessor::processIrcEvent311(IrcEvent *e)
{
    if (!checkParamCount(e, 3))
        return;

    IrcUser *ircuser = e->network()->ircUser(e->params().at(0));
    if (ircuser) {
        ircuser->setUser(e->params().at(1));
        ircuser->setHost(e->params().at(2));
        ircuser->setRealName(e->params().last());
    }
}


/*  RPL_WHOISSERVER -  "<nick> <server> :<server info>" */
void CoreSessionEventProcessor::processIrcEvent312(IrcEvent *e)
{
    if (!checkParamCount(e, 2))
        return;

    IrcUser *ircuser = e->network()->ircUser(e->params().at(0));
    if (ircuser)
        ircuser->setServer(e->params().at(1));
}


/*  RPL_WHOISOPERATOR - "<nick> :is an IRC operator" */
void CoreSessionEventProcessor::processIrcEvent313(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    IrcUser *ircuser = e->network()->ircUser(e->params().at(0));
    if (ircuser)
        ircuser->setIrcOperator(e->params().last());
}


/*  RPL_ENDOFWHO: "<name> :End of WHO list" */
void CoreSessionEventProcessor::processIrcEvent315(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    if (coreNetwork(e)->setAutoWhoDone(e->params()[0]))
        e->setFlag(EventManager::Silent);
}


/*  RPL_WHOISIDLE - "<nick> <integer> :seconds idle"
   (real life: "<nick> <integer> <integer> :seconds idle, signon time) */
void CoreSessionEventProcessor::processIrcEvent317(IrcEvent *e)
{
    if (!checkParamCount(e, 2))
        return;

    QDateTime loginTime;

    int idleSecs = e->params()[1].toInt();
    if (e->params().count() > 3) { // if we have more then 3 params we have the above mentioned "real life" situation
        int logintime = e->params()[2].toInt();
        loginTime = QDateTime::fromTime_t(logintime);
    }

    IrcUser *ircuser = e->network()->ircUser(e->params()[0]);
    if (ircuser) {
        ircuser->setIdleTime(e->timestamp().addSecs(-idleSecs));
        if (loginTime.isValid())
            ircuser->setLoginTime(loginTime);
    }
}


/* RPL_LIST -  "<channel> <# visible> :<topic>" */
void CoreSessionEventProcessor::processIrcEvent322(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    QString channelName;
    quint32 userCount = 0;
    QString topic;

    switch (e->params().count()) {
    case 3:
        topic = e->params()[2];
    case 2:
        userCount = e->params()[1].toUInt();
    case 1:
        channelName = e->params()[0];
    default:
        break;
    }
    if (coreSession()->ircListHelper()->addChannel(e->networkId(), channelName, userCount, topic))
        e->stop();  // consumed by IrcListHelper, so don't further process/show this event
}


/* RPL_LISTEND ":End of LIST" */
void CoreSessionEventProcessor::processIrcEvent323(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    if (coreSession()->ircListHelper()->endOfChannelList(e->networkId()))
        e->stop();  // consumed by IrcListHelper, so don't further process/show this event
}


/* RPL_CHANNELMODEIS - "<channel> <mode> <mode params>" */
void CoreSessionEventProcessor::processIrcEvent324(IrcEvent *e)
{
    processIrcEventMode(e);
}


/* RPL_NOTOPIC */
void CoreSessionEventProcessor::processIrcEvent331(IrcEvent *e)
{
    if (!checkParamCount(e, 1))
        return;

    IrcChannel *chan = e->network()->ircChannel(e->params()[0]);
    if (chan)
        chan->setTopic(QString());
}


/* RPL_TOPIC */
void CoreSessionEventProcessor::processIrcEvent332(IrcEvent *e)
{
    if (!checkParamCount(e, 2))
        return;

    IrcChannel *chan = e->network()->ircChannel(e->params()[0]);
    if (chan)
        chan->setTopic(e->params()[1]);
}


/*  RPL_WHOREPLY: "<channel> <user> <host> <server> <nick>
              ( "H" / "G" > ["*"] [ ( "@" / "+" ) ] :<hopcount> <real name>" */
void CoreSessionEventProcessor::processIrcEvent352(IrcEvent *e)
{
    if (!checkParamCount(e, 6))
        return;

    QString channel = e->params()[0];
    IrcUser *ircuser = e->network()->ircUser(e->params()[4]);
    if (ircuser) {
        ircuser->setUser(e->params()[1]);
        ircuser->setHost(e->params()[2]);

        bool away = e->params()[5].contains("G", Qt::CaseInsensitive);
        ircuser->setAway(away);
        ircuser->setServer(e->params()[3]);
        ircuser->setRealName(e->params().last().section(" ", 1));

        if (coreNetwork(e)->useCapMultiPrefix()) {
            // If multi-prefix is enabled, all modes will be sent in WHO replies.
            // :kenny.chatspike.net 352 guest #test grawity broken.symlink *.chatspike.net grawity H@%+ :0 Mantas M.
            // See: http://ircv3.net/specs/extensions/multi-prefix-3.1.html
            QString uncheckedModes = e->params()[5];
            QString validModes = QString();
            while (!uncheckedModes.isEmpty()) {
                // Mode found in 1 left-most character, add it to the list
                if (e->network()->prefixes().contains(uncheckedModes[0])) {
                    validModes.append(e->network()->prefixToMode(uncheckedModes[0]));
                }
                // Remove this mode from the list of unchecked modes
                uncheckedModes = uncheckedModes.remove(0, 1);
            }

            // Some IRC servers decide to not follow the spec, returning only -some- of the user
            // modes in WHO despite listing them all in NAMES.  For now, assume it can only add
            // and not take away.  *sigh*
            if (!validModes.isEmpty())
                ircuser->addUserModes(validModes);
        }
    }

    // Check if channel name has a who in progress.
    // If not, then check if user nick exists and has a who in progress.
    if (coreNetwork(e)->isAutoWhoInProgress(channel) ||
        (ircuser && coreNetwork(e)->isAutoWhoInProgress(ircuser->nick()))) {
        e->setFlag(EventManager::Silent);
    }
}


/* RPL_NAMREPLY */
void CoreSessionEventProcessor::processIrcEvent353(IrcEvent *e)
{
    if (!checkParamCount(e, 3))
        return;

    // param[0] is either "=", "*" or "@" indicating a public, private or secret channel
    // we don't use this information at the time beeing
    QString channelname = e->params()[1];

    IrcChannel *channel = e->network()->ircChannel(channelname);
    if (!channel) {
        qWarning() << Q_FUNC_INFO << "Received unknown target channel:" << channelname;
        return;
    }

    QStringList nicks;
    QStringList modes;

    // Cache result of multi-prefix to avoid unneeded casts and lookups with each iteration.
    bool _useCapMultiPrefix = coreNetwork(e)->useCapMultiPrefix();

    foreach(QString nick, e->params()[2].split(' ', QString::SkipEmptyParts)) {
        QString mode;

        if (_useCapMultiPrefix) {
            // If multi-prefix is enabled, all modes will be sent in NAMES replies.
            // :hades.arpa 353 guest = #tethys :~&@%+aji &@Attila @+alyx +KindOne Argure
            // See: http://ircv3.net/specs/extensions/multi-prefix-3.1.html
            while (e->network()->prefixes().contains(nick[0])) {
                // Mode found in 1 left-most character, add it to the list.
                // Note: sending multiple modes may cause a warning in older clients.
                // In testing, the clients still seemed to function fine.
                mode.append(e->network()->prefixToMode(nick[0]));
                // Remove this mode from the nick
                nick = nick.remove(0, 1);
            }
        } else if (e->network()->prefixes().contains(nick[0])) {
            // Multi-prefix is disabled and a mode prefix was found.
            mode = e->network()->prefixToMode(nick[0]);
            nick = nick.mid(1);
        }

        // If userhost-in-names capability is enabled, the following will be
        // in the form "nick!user@host" rather than "nick".  This works without
        // special handling as the following use nickFromHost() as needed.
        // See: http://ircv3.net/specs/extensions/userhost-in-names-3.2.html

        nicks << nick;
        modes << mode;
    }

    channel->joinIrcUsers(nicks, modes);
}


/* ERR_ERRONEUSNICKNAME */
void CoreSessionEventProcessor::processIrcEvent432(IrcEventNumeric *e)
{
    if (!checkParamCount(e, 1))
        return;

    QString errnick;
    if (e->params().count() < 2) {
        // handle unreal-ircd bug, where unreal ircd doesnt supply a TARGET in ERR_ERRONEUSNICKNAME during registration phase:
        // nick @@@
        // :irc.scortum.moep.net 432  @@@ :Erroneous Nickname: Illegal characters
        // correct server reply:
        // :irc.scortum.moep.net 432 * @@@ :Erroneous Nickname: Illegal characters
        e->params().prepend(e->target());
        e->setTarget("*");
    }
    errnick = e->params()[0];

    tryNextNick(e, errnick, true /* erroneus */);
}


/* ERR_NICKNAMEINUSE */
void CoreSessionEventProcessor::processIrcEvent433(IrcEventNumeric *e)
{
    if (!checkParamCount(e, 1))
        return;

    QString errnick = e->params().first();

    // if there is a problem while connecting to the server -> we handle it
    // but only if our connection has not been finished yet...
    if (!e->network()->currentServer().isEmpty())
        return;

    tryNextNick(e, errnick);
}


/* ERR_UNAVAILRESOURCE */
void CoreSessionEventProcessor::processIrcEvent437(IrcEventNumeric *e)
{
    if (!checkParamCount(e, 1))
        return;

    QString errnick = e->params().first();

    // if there is a problem while connecting to the server -> we handle it
    // but only if our connection has not been finished yet...
    if (!e->network()->currentServer().isEmpty())
        return;

    if (!e->network()->isChannelName(errnick))
        tryNextNick(e, errnick);
}


/* template
void CoreSessionEventProcessor::processIrcEvent(IrcEvent *e) {
  if(!checkParamCount(e, 1))
    return;

}
*/

/* Handle signals from Netsplit objects  */

void CoreSessionEventProcessor::handleNetsplitJoin(Network *net,
    const QString &channel,
    const QStringList &users,
    const QStringList &modes,
    const QString &quitMessage)
{
    IrcChannel *ircChannel = net->ircChannel(channel);
    if (!ircChannel) {
        return;
    }
    QList<IrcUser *> ircUsers;
    QStringList newModes = modes;
    QStringList newUsers = users;

    foreach(const QString &user, users) {
        IrcUser *iu = net->ircUser(nickFromMask(user));
        if (iu)
            ircUsers.append(iu);
        else { // the user already quit
            int idx = users.indexOf(user);
            newUsers.removeAt(idx);
            newModes.removeAt(idx);
        }
    }

    ircChannel->joinIrcUsers(ircUsers, newModes);
    NetworkSplitEvent *event = new NetworkSplitEvent(EventManager::NetworkSplitJoin, net, channel, newUsers, quitMessage);
    emit newEvent(event);
}


void CoreSessionEventProcessor::handleNetsplitQuit(Network *net, const QString &channel, const QStringList &users, const QString &quitMessage)
{
    NetworkSplitEvent *event = new NetworkSplitEvent(EventManager::NetworkSplitQuit, net, channel, users, quitMessage);
    emit newEvent(event);
    foreach(QString user, users) {
        IrcUser *iu = net->ircUser(nickFromMask(user));
        if (iu)
            iu->quit();
    }
}


void CoreSessionEventProcessor::handleEarlyNetsplitJoin(Network *net, const QString &channel, const QStringList &users, const QStringList &modes)
{
    IrcChannel *ircChannel = net->ircChannel(channel);
    if (!ircChannel) {
        qDebug() << "handleEarlyNetsplitJoin(): channel " << channel << " invalid";
        return;
    }
    QList<NetworkEvent *> events;
    QList<IrcUser *> ircUsers;
    QStringList newModes = modes;

    foreach(QString user, users) {
        IrcUser *iu = net->updateNickFromMask(user);
        if (iu) {
            ircUsers.append(iu);
            // fake event for scripts that consume join events
            events << new IrcEvent(EventManager::IrcEventJoin, net, iu->hostmask(), QStringList() << channel);
        }
        else {
            newModes.removeAt(users.indexOf(user));
        }
    }
    ircChannel->joinIrcUsers(ircUsers, newModes);
    foreach(NetworkEvent *event, events) {
        event->setFlag(EventManager::Fake); // ignore this in here!
        emit newEvent(event);
    }
}


void CoreSessionEventProcessor::handleNetsplitFinished()
{
    Netsplit *n = qobject_cast<Netsplit *>(sender());
    Q_ASSERT(n);
    QHash<QString, Netsplit *> splithash  = _netsplits.take(n->network());
    splithash.remove(splithash.key(n));
    if (splithash.count())
        _netsplits[n->network()] = splithash;
    n->deleteLater();
}


void CoreSessionEventProcessor::destroyNetsplits(NetworkId netId)
{
    Network *net = coreSession()->network(netId);
    if (!net)
        return;

    QHash<QString, Netsplit *> splits = _netsplits.take(net);
    qDeleteAll(splits);
}


/*******************************/
/******** CTCP HANDLING ********/
/*******************************/

void CoreSessionEventProcessor::processCtcpEvent(CtcpEvent *e)
{
    if (e->testFlag(EventManager::Self))
        return;  // ignore ctcp events generated by user input

    if (e->type() != EventManager::CtcpEvent || e->ctcpType() != CtcpEvent::Query)
        return;

    handle(e->ctcpCmd(), Q_ARG(CtcpEvent *, e));
}


void CoreSessionEventProcessor::defaultHandler(const QString &ctcpCmd, CtcpEvent *e)
{
    // This handler is only there to avoid warnings for unknown CTCPs
    Q_UNUSED(e);
    Q_UNUSED(ctcpCmd);
}


void CoreSessionEventProcessor::handleCtcpAction(CtcpEvent *e)
{
    // This handler is only there to feed CLIENTINFO
    Q_UNUSED(e);
}


void CoreSessionEventProcessor::handleCtcpClientinfo(CtcpEvent *e)
{
    QStringList supportedHandlers;
    foreach(QString handler, providesHandlers())
    supportedHandlers << handler.toUpper();
    qSort(supportedHandlers);
    e->setReply(supportedHandlers.join(" "));
}


// http://www.irchelp.org/irchelp/rfc/ctcpspec.html
// http://en.wikipedia.org/wiki/Direct_Client-to-Client
void CoreSessionEventProcessor::handleCtcpDcc(CtcpEvent *e)
{
    // DCC support is unfinished, experimental and potentially dangerous, so make it opt-in
    if (!Quassel::isOptionSet("enable-experimental-dcc")) {
        quInfo() << "DCC disabled, start core with --enable-experimental-dcc if you really want to try it out";
        return;
    }

    // normal:  SEND <filename> <ip> <port> [<filesize>]
    // reverse: SEND <filename> <ip> 0 <filesize> <token>
    QStringList params = e->param().split(' ');
    if (params.count()) {
        QString cmd = params[0].toUpper();
        if (cmd == "SEND") {
            if (params.count() < 4) {
                qWarning() << "Invalid DCC SEND request:" << e;  // TODO emit proper error to client
                return;
            }
            QString filename = params[1];
            QHostAddress address;
            quint16 port = params[3].toUShort();
            quint64 size = 0;
            QString numIp = params[2]; // this is either IPv4 as a 32 bit value, or IPv6 (which always contains a colon)
            if (numIp.contains(':')) { // IPv6
                if (!address.setAddress(numIp)) {
                    qWarning() << "Invalid IPv6:" << numIp;
                    return;
                }
            }
            else {
                address.setAddress(numIp.toUInt());
            }

            if (port == 0) { // Reverse DCC is indicated by a 0 port
                emit newEvent(new MessageEvent(Message::Error, e->network(), tr("Reverse DCC SEND not supported"), e->prefix(), e->target(), Message::None, e->timestamp()));
                return;
            }
            if (port < 1024) {
                qWarning() << "Privileged port requested:" << port; // FIXME ask user if this is ok
            }


            if (params.count() > 4) { // filesize is optional
                size = params[4].toULong();
            }

            // TODO: check if target is the right thing to use for the partner
            CoreTransfer *transfer = new CoreTransfer(Transfer::Receive, e->target(), filename, address, port, size, this);
            coreSession()->signalProxy()->synchronize(transfer);
            coreSession()->transferManager()->addTransfer(transfer);
        }
        else {
            emit newEvent(new MessageEvent(Message::Error, e->network(), tr("DCC %1 not supported").arg(cmd), e->prefix(), e->target(), Message::None, e->timestamp()));
            return;
        }
    }
}


void CoreSessionEventProcessor::handleCtcpPing(CtcpEvent *e)
{
    e->setReply(e->param().isNull() ? "" : e->param());
}


void CoreSessionEventProcessor::handleCtcpTime(CtcpEvent *e)
{
    e->setReply(QDateTime::currentDateTime().toString());
}


void CoreSessionEventProcessor::handleCtcpVersion(CtcpEvent *e)
{
    e->setReply(QString("Quassel IRC %1 (built on %2) -- http://www.quassel-irc.org")
        .arg(Quassel::buildInfo().plainVersionString).arg(Quassel::buildInfo().buildDate));
}
