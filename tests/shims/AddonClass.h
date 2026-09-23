/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Stands in for a Kodi dependency that is built rather than carried in
 *  the source tree. It replaces infrastructure, never the code under test.
 */

#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#define XBMC_TRACE do {} while (false)
using CCriticalSection = std::recursive_mutex;
namespace XBMCAddon {
class AddonClass : public CCriticalSection {
public:
  virtual ~AddonClass() = default;
  void deallocating() { m_deallocating = true; }
  bool isDeallocating() const { return m_deallocating; }
  void acquire() { ++m_refs; }
  void release() { if (--m_refs == 0) delete this; }
  template<class T> class Ref {
  public:
    Ref(T* value = nullptr) : m_value(value) { if (m_value) m_value->acquire(); }
    Ref(const Ref& other) : Ref(other.m_value) {}
    Ref(Ref&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
    Ref& operator=(Ref other) { std::swap(m_value, other.m_value); return *this; }
    ~Ref() { if (m_value) m_value->release(); }
    operator T*() const { return m_value; }
    T* get() const { return m_value; }
    T* operator->() const { return m_value; }
  private:
    T* m_value;
  };
private:
  std::atomic<unsigned int> m_refs{0};
  bool m_deallocating = false;
};
}
