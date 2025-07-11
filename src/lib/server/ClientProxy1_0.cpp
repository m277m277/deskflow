/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_0.h"

#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/XDeskflow.h"
#include "io/IStream.h"

#include <cstring>

//
// ClientProxy1_0
//

ClientProxy1_0::ClientProxy1_0(const std::string &name, deskflow::IStream *stream, IEventQueue *events)
    : ClientProxy(name, stream),
      m_events(events)
{
  // install event handlers
  m_events->addHandler(EventTypes::StreamInputReady, stream->getEventTarget(), [this](const auto &) { handleData(); });
  m_events->addHandler(EventTypes::StreamOutputError, stream->getEventTarget(), [this](const auto &) {
    handleWriteError();
  });
  m_events->addHandler(EventTypes::StreamInputShutdown, stream->getEventTarget(), [this](const auto &) {
    handleDisconnect();
  });
  m_events->addHandler(EventTypes::StreamInputFormatError, stream->getEventTarget(), [this](const auto &) {
    handleDisconnect();
  });
  m_events->addHandler(EventTypes::StreamOutputShutdown, stream->getEventTarget(), [this](const auto &) {
    handleWriteError();
  });
  m_events->addHandler(EventTypes::Timer, this, [this](const auto &) { handleFlatline(); });

  setHeartbeatRate(kHeartRate, kHeartRate * kHeartBeatsUntilDeath);

  LOG((CLOG_DEBUG1 "querying client \"%s\" info", getName().c_str()));
  ProtocolUtil::writef(getStream(), kMsgQInfo);
}

ClientProxy1_0::~ClientProxy1_0()
{
  removeHandlers();
}

void ClientProxy1_0::disconnect()
{
  removeHandlers();
  getStream()->close();
  m_events->addEvent(Event(EventTypes::ClientProxyDisconnected, getEventTarget()));
}

void ClientProxy1_0::removeHandlers()
{
  using enum EventTypes;
  // uninstall event handlers
  m_events->removeHandler(StreamInputReady, getStream()->getEventTarget());
  m_events->removeHandler(StreamOutputError, getStream()->getEventTarget());
  m_events->removeHandler(StreamInputShutdown, getStream()->getEventTarget());
  m_events->removeHandler(StreamOutputShutdown, getStream()->getEventTarget());
  m_events->removeHandler(StreamInputFormatError, getStream()->getEventTarget());
  m_events->removeHandler(Timer, this);

  // remove timer
  removeHeartbeatTimer();
}

void ClientProxy1_0::addHeartbeatTimer()
{
  if (m_heartbeatAlarm > 0.0) {
    m_heartbeatTimer = m_events->newOneShotTimer(m_heartbeatAlarm, this);
  }
}

void ClientProxy1_0::removeHeartbeatTimer()
{
  if (m_heartbeatTimer != nullptr) {
    m_events->deleteTimer(m_heartbeatTimer);
    m_heartbeatTimer = nullptr;
  }
}

void ClientProxy1_0::resetHeartbeatTimer()
{
  // reset the alarm
  removeHeartbeatTimer();
  addHeartbeatTimer();
}

void ClientProxy1_0::resetHeartbeatRate()
{
  setHeartbeatRate(kHeartRate, kHeartRate * kHeartBeatsUntilDeath);
}

void ClientProxy1_0::setHeartbeatRate(double, double alarm)
{
  m_heartbeatAlarm = alarm;
}

void ClientProxy1_0::handleData()
{
  // handle messages until there are no more.  first read message code.
  uint8_t code[4];
  uint32_t n = getStream()->read(code, 4);
  while (n != 0) {
    // verify we got an entire code
    if (n != 4) {
      LOG((CLOG_ERR "incomplete message from \"%s\": %d bytes", getName().c_str(), n));
      disconnect();
      return;
    }

    // parse message
    try {
      LOG((CLOG_DEBUG2 "msg from \"%s\": %c%c%c%c", getName().c_str(), code[0], code[1], code[2], code[3]));
      if (!(this->*m_parser)(code)) {
        LOG(
            (CLOG_ERR "invalid message from client \"%s\": %c%c%c%c", getName().c_str(), code[0], code[1], code[2],
             code[3])
        );
        // not possible to determine message boundaries
        // read the whole stream to discard unkonwn data
        while (getStream()->read(nullptr, 4))
          ;
      }
    } catch (const XBadClient &e) {
      LOG((CLOG_ERR "protocol error from client \"%s\": %s", getName().c_str(), e.what()));
      disconnect();
      return;
    }

    // next message
    n = getStream()->read(code, 4);
  }

  // restart heartbeat timer
  resetHeartbeatTimer();
}

bool ClientProxy1_0::parseHandshakeMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgCNoop, 4) == 0) {
    // discard no-ops
    LOG((CLOG_DEBUG2 "no-op from", getName().c_str()));
    return true;
  } else if (memcmp(code, kMsgDInfo, 4) == 0) {
    // future messages get parsed by parseMessage
    m_parser = &ClientProxy1_0::parseMessage;
    if (recvInfo()) {
      m_events->addEvent(Event(EventTypes::ClientProxyReady, getEventTarget()));
      addHeartbeatTimer();
      return true;
    }
  }
  return false;
}

bool ClientProxy1_0::parseMessage(const uint8_t *code)
{
  if (memcmp(code, kMsgDInfo, 4) == 0) {
    if (recvInfo()) {
      m_events->addEvent(Event(EventTypes::ScreenShapeChanged, getEventTarget()));
      return true;
    }
    return false;
  } else if (memcmp(code, kMsgCNoop, 4) == 0) {
    // discard no-ops
    LOG((CLOG_DEBUG2 "no-op from", getName().c_str()));
    return true;
  } else if (memcmp(code, kMsgCClipboard, 4) == 0) {
    return recvGrabClipboard();
  } else if (memcmp(code, kMsgDClipboard, 4) == 0) {
    return recvClipboard();
  }
  return false;
}

void ClientProxy1_0::handleDisconnect()
{
  LOG((CLOG_NOTE "client \"%s\" has disconnected", getName().c_str()));
  disconnect();
}

void ClientProxy1_0::handleWriteError()
{
  LOG((CLOG_WARN "error writing to client \"%s\"", getName().c_str()));
  disconnect();
}

void ClientProxy1_0::handleFlatline()
{
  // didn't get a heartbeat fast enough.  assume client is dead.
  LOG((CLOG_NOTE "client \"%s\" is dead", getName().c_str()));
  disconnect();
}

bool ClientProxy1_0::getClipboard(ClipboardID id, IClipboard *clipboard) const
{
  Clipboard::copy(clipboard, &m_clipboard[id].m_clipboard);
  return true;
}

void ClientProxy1_0::getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const
{
  x = m_info.m_x;
  y = m_info.m_y;
  w = m_info.m_w;
  h = m_info.m_h;
}

void ClientProxy1_0::getCursorPos(int32_t &x, int32_t &y) const
{
  // note -- this returns the cursor pos from when we last got client info
  x = m_info.m_mx;
  y = m_info.m_my;
}

void ClientProxy1_0::enter(int32_t xAbs, int32_t yAbs, uint32_t seqNum, KeyModifierMask mask, bool)
{
  LOG((CLOG_DEBUG1 "send enter to \"%s\", %d,%d %d %04x", getName().c_str(), xAbs, yAbs, seqNum, mask));
  ProtocolUtil::writef(getStream(), kMsgCEnter, xAbs, yAbs, seqNum, mask);
}

bool ClientProxy1_0::leave()
{
  LOG((CLOG_DEBUG1 "send leave to \"%s\"", getName().c_str()));
  ProtocolUtil::writef(getStream(), kMsgCLeave);

  // we can never prevent the user from leaving
  return true;
}

void ClientProxy1_0::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // ignore -- deprecated in protocol 1.0
}

void ClientProxy1_0::grabClipboard(ClipboardID id)
{
  LOG((CLOG_DEBUG "send grab clipboard %d to \"%s\"", id, getName().c_str()));
  ProtocolUtil::writef(getStream(), kMsgCClipboard, id, 0);

  // this clipboard is now dirty
  m_clipboard[id].m_dirty = true;
}

void ClientProxy1_0::setClipboardDirty(ClipboardID id, bool dirty)
{
  m_clipboard[id].m_dirty = dirty;
}

void ClientProxy1_0::keyDown(KeyID key, KeyModifierMask mask, KeyButton, const std::string &)
{
  LOG((CLOG_DEBUG1 "send key down to \"%s\" id=%d, mask=0x%04x", getName().c_str(), key, mask));
  ProtocolUtil::writef(getStream(), kMsgDKeyDown1_0, key, mask);
}

void ClientProxy1_0::keyRepeat(KeyID key, KeyModifierMask mask, int32_t count, KeyButton, const std::string &)
{
  LOG((CLOG_DEBUG1 "send key repeat to \"%s\" id=%d, mask=0x%04x, count=%d", getName().c_str(), key, mask, count));
  ProtocolUtil::writef(getStream(), kMsgDKeyRepeat1_0, key, mask, count);
}

void ClientProxy1_0::keyUp(KeyID key, KeyModifierMask mask, KeyButton)
{
  LOG((CLOG_DEBUG1 "send key up to \"%s\" id=%d, mask=0x%04x", getName().c_str(), key, mask));
  ProtocolUtil::writef(getStream(), kMsgDKeyUp1_0, key, mask);
}

void ClientProxy1_0::mouseDown(ButtonID button)
{
  LOG((CLOG_DEBUG1 "send mouse down to \"%s\" id=%d", getName().c_str(), button));
  ProtocolUtil::writef(getStream(), kMsgDMouseDown, button);
}

void ClientProxy1_0::mouseUp(ButtonID button)
{
  LOG((CLOG_DEBUG1 "send mouse up to \"%s\" id=%d", getName().c_str(), button));
  ProtocolUtil::writef(getStream(), kMsgDMouseUp, button);
}

void ClientProxy1_0::mouseMove(int32_t xAbs, int32_t yAbs)
{
  LOG((CLOG_DEBUG2 "send mouse move to \"%s\" %d,%d", getName().c_str(), xAbs, yAbs));
  ProtocolUtil::writef(getStream(), kMsgDMouseMove, xAbs, yAbs);
}

void ClientProxy1_0::mouseRelativeMove(int32_t, int32_t)
{
  // ignore -- not supported in protocol 1.0
}

void ClientProxy1_0::mouseWheel(int32_t, int32_t yDelta)
{
  // clients prior to 1.3 only support the y axis
  LOG((CLOG_DEBUG2 "send mouse wheel to \"%s\" %+d", getName().c_str(), yDelta));
  ProtocolUtil::writef(getStream(), kMsgDMouseWheel1_0, yDelta);
}

void ClientProxy1_0::sendDragInfo(uint32_t fileCount, const char *info, size_t size)
{
  // ignore -- not supported in protocol 1.0
  LOG((CLOG_DEBUG "draggingInfoSending not supported"));
}

void ClientProxy1_0::fileChunkSending(uint8_t mark, char *data, size_t dataSize)
{
  // ignore -- not supported in protocol 1.0
  LOG((CLOG_DEBUG "fileChunkSending not supported"));
}

std::string ClientProxy1_0::getSecureInputApp() const
{
  // ignore -- not supported on clients
  LOG((CLOG_DEBUG "getSecureInputApp not supported"));
  return "";
}

void ClientProxy1_0::secureInputNotification(const std::string &app) const
{
  // ignore -- not supported in protocol 1.0
  LOG((CLOG_DEBUG "secureInputNotification not supported"));
}

void ClientProxy1_0::screensaver(bool on)
{
  LOG((CLOG_DEBUG1 "send screen saver to \"%s\" on=%d", getName().c_str(), on ? 1 : 0));
  ProtocolUtil::writef(getStream(), kMsgCScreenSaver, on ? 1 : 0);
}

void ClientProxy1_0::resetOptions()
{
  LOG((CLOG_DEBUG1 "send reset options to \"%s\"", getName().c_str()));
  ProtocolUtil::writef(getStream(), kMsgCResetOptions);

  // reset heart rate and death
  resetHeartbeatRate();
  removeHeartbeatTimer();
  addHeartbeatTimer();
}

void ClientProxy1_0::setOptions(const OptionsList &options)
{
  LOG((CLOG_DEBUG1 "send set options to \"%s\" size=%d", getName().c_str(), options.size()));
  ProtocolUtil::writef(getStream(), kMsgDSetOptions, &options);

  // check options
  for (uint32_t i = 0, n = (uint32_t)options.size(); i < n; i += 2) {
    if (options[i] == kOptionHeartbeat) {
      double rate = 1.0e-3 * static_cast<double>(options[i + 1]);
      if (rate <= 0.0) {
        rate = -1.0;
      }
      setHeartbeatRate(rate, rate * kHeartBeatsUntilDeath);
      removeHeartbeatTimer();
      addHeartbeatTimer();
    }
  }
}

bool ClientProxy1_0::recvInfo()
{
  // parse the message
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  int16_t dummy1;
  int16_t mx;
  int16_t my;
  if (!ProtocolUtil::readf(getStream(), kMsgDInfo + 4, &x, &y, &w, &h, &dummy1, &mx, &my)) {
    return false;
  }
  LOG((CLOG_DEBUG "received client \"%s\" info shape=%d,%d %dx%d at %d,%d", getName().c_str(), x, y, w, h, mx, my));

  // validate
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (mx < x || mx >= x + w || my < y || my >= y + h) {
    mx = x + w / 2;
    my = y + h / 2;
  }

  // save
  m_info.m_x = x;
  m_info.m_y = y;
  m_info.m_w = w;
  m_info.m_h = h;
  m_info.m_mx = mx;
  m_info.m_my = my;

  // acknowledge receipt
  LOG((CLOG_DEBUG1 "send info ack to \"%s\"", getName().c_str()));
  ProtocolUtil::writef(getStream(), kMsgCInfoAck);
  return true;
}

bool ClientProxy1_0::recvClipboard()
{
  // deprecated in protocol 1.0
  return false;
}

bool ClientProxy1_0::recvGrabClipboard()
{
  // parse message
  ClipboardID id;
  uint32_t seqNum;
  if (!ProtocolUtil::readf(getStream(), kMsgCClipboard + 4, &id, &seqNum)) {
    return false;
  }
  LOG((CLOG_DEBUG "received client \"%s\" grabbed clipboard %d seqnum=%d", getName().c_str(), id, seqNum));

  // validate
  if (id >= kClipboardEnd) {
    return false;
  }

  // notify
  auto *info = new ClipboardInfo;
  info->m_id = id;
  info->m_sequenceNumber = seqNum;
  m_events->addEvent(Event(EventTypes::ClipboardGrabbed, getEventTarget(), info));

  return true;
}
