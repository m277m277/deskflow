/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "gui/core/ServerConnection.h"

#include "unittests/legacytests/shared/gui/mocks/ServerConfigMock.h"

#include "gmock/gmock.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using testing::_;
using testing::NiceMock;
using namespace deskflow::gui;

class QWidget;

namespace {

struct DepsMock : public ServerConnection::Deps
{
  MOCK_METHOD(
      messages::NewClientPromptResult, showNewClientPrompt,
      (QWidget * parent, const QString &clientName, bool previousyAccepted), (const, override)
  );
};

} // namespace

class ServerConnectionTests : public testing::Test
{
public:
  std::shared_ptr<DepsMock> m_pDeps = std::make_shared<NiceMock<DepsMock>>();
  NiceMock<ServerConfigMock> m_serverConfig;
};

TEST_F(ServerConnectionTests, handleLogLine_newClient_shouldShowPrompt)
{
  ServerConnection serverConnection(nullptr, m_serverConfig, m_pDeps);

  QString clientName = "test client";
  EXPECT_CALL(*m_pDeps, showNewClientPrompt(_, clientName, true)).Times(0);

  serverConnection.handleLogLine(R"(unrecognised client name "test client")");
}

TEST_F(ServerConnectionTests, handleLogLine_ignoredClient_shouldNotShowPrompt)
{
  ServerConnection serverConnection(nullptr, m_serverConfig, m_pDeps);
  ON_CALL(*m_pDeps, showNewClientPrompt(_, _, false))
      .WillByDefault(testing::Return(messages::NewClientPromptResult::Ignore));
  serverConnection.handleLogLine(R"(unrecognised client name "stub")");

  EXPECT_CALL(*m_pDeps, showNewClientPrompt(_, _, false)).Times(0);

  serverConnection.handleLogLine(R"(unrecognised client name "stub")");
}

TEST_F(ServerConnectionTests, handleLogLine_serverConfigFull_shouldNotShowPrompt)
{
  ServerConnection serverConnection(nullptr, m_serverConfig, m_pDeps);
  ON_CALL(m_serverConfig, isFull()).WillByDefault(testing::Return(true));

  EXPECT_CALL(*m_pDeps, showNewClientPrompt(_, _, false)).Times(0);

  serverConnection.handleLogLine(R"(unrecognised client name "test client")");
}

TEST_F(ServerConnectionTests, handleLogLine_screenExists_shouldNotShowPrompt)
{
  ServerConnection serverConnection(nullptr, m_serverConfig, m_pDeps);
  ON_CALL(m_serverConfig, screenExists(_)).WillByDefault(testing::Return(true));

  EXPECT_CALL(*m_pDeps, showNewClientPrompt(_, _, false)).Times(0);

  serverConnection.handleLogLine(R"(unrecognised client name "test client")");
}
