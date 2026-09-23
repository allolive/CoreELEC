/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Stands in for a Kodi dependency that is built rather than carried in
 *  the source tree. It replaces infrastructure, never the code under test.
 */

#pragma once
namespace XBMCAddonUtils {
template<class Lock> class InvertSingleLockGuard {
public:
  explicit InvertSingleLockGuard(Lock& lock) : m_lock(lock) { m_lock.unlock(); }
  ~InvertSingleLockGuard() { m_lock.lock(); }
private:
  Lock& m_lock;
};
}
