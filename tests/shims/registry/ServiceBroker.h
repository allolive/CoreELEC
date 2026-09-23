/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

// Kodi's service registry, without the registry.
//
// The real header declares the broker as a global reference at namespace
// scope, so every translation unit that includes it instantiates the singleton
// and needs CServiceBroker's constructor - and so every service type it owns,
// and spdlog behind those. None of that says anything about the code under
// test, and a suite that wants one service should not link the whole of Kodi's
// startup to get it.
//
// This declares only the accessors a suite needs; the test binary defines the
// ones its subject actually calls. That makes a suite's dependence on a
// service visible in the suite rather than buried in a link line.
//
// This directory is opt-in: a suite asks for it by name, so no suite gets a
// shimmed registry by accident.

class IAE;

class CServiceBroker
{
public:
  static IAE* GetActiveAE();
};
