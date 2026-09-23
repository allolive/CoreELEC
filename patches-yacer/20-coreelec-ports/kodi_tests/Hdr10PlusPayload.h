/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Builds an ST 2094-40 payload the parser accepts, so suites can start from
 *  metadata that is real rather than random.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace yacer
{
// A bit writer mirroring ST 2094-40's field order, so a payload can be built
// that the parser accepts. Without one every case here feeds malformed input
// and the whole nominal path goes unexecuted.
class Bits
{
public:
  void Put(uint32_t value, int width)
  {
    for (int bit = width - 1; bit >= 0; --bit)
    {
      if (m_used == 0)
      {
        m_bytes.push_back(0);
        m_used = 8;
      }
      --m_used;
      m_bytes.back() |= static_cast<uint8_t>(((value >> bit) & 1u) << m_used);
    }
  }

  std::vector<uint8_t> Take() const { return m_bytes; }

private:
  std::vector<uint8_t> m_bytes;
  int m_used = 0;
};

// The smallest payload the parser accepts in full: one window, no actual-peak
// grids, a nine-anchor Bezier curve and no colour-saturation weight.
std::vector<uint8_t> WellFormed(uint8_t windows = 1, uint32_t maxscl = 0)
{
  Bits bits;
  bits.Put(0xB5, 8);   // itu_t_t35_country_code
  bits.Put(0x003C, 16); // terminal provider code
  bits.Put(0x0001, 16); // provider oriented code
  bits.Put(4, 8);       // application_identifier
  bits.Put(0, 8);       // application_version
  bits.Put(windows, 2);
  for (uint8_t window = 1; window < windows; ++window)
  {
    for (int field = 0; field < 6; ++field)
      bits.Put(100 * window, 16);
    bits.Put(0, 8);          // rotation_angle
    bits.Put(10, 16);        // semimajor internal
    bits.Put(20, 16);        // semimajor external
    bits.Put(30, 16);        // semiminor external
    bits.Put(0, 1);          // overlap_process_option
  }
  bits.Put(500, 27);         // targeted_system_display_maximum_luminance
  bits.Put(0, 1);            // no actual targeted peak grid
  for (uint8_t window = 0; window < windows; ++window)
  {
    // maxscl 0 keeps the distinct default values, so a case can assert each
    // channel separately; a value given makes the content uniformly brighter.
    bits.Put(maxscl ? maxscl : 1000, 17);        // maxscl[0]
    bits.Put(maxscl ? maxscl : 2000, 17);        // maxscl[1]
    bits.Put(maxscl ? maxscl : 3000, 17);        // maxscl[2]
    bits.Put(maxscl ? maxscl / 2 : 1500, 17);    // average_maxrgb
    bits.Put(9, 4);          // num_distribution_maxrgb_percentiles
    for (int index = 0; index < 9; ++index)
    {
      bits.Put(static_cast<uint32_t>(index + 1), 7); // percentage
      bits.Put(static_cast<uint32_t>(100 * (index + 1)), 17); // percentile
    }
    bits.Put(512, 10);       // fraction_bright_pixels
  }
  bits.Put(0, 1);            // no actual mastering peak grid
  for (uint8_t window = 0; window < windows; ++window)
  {
    bits.Put(1, 1);          // tone_mapping_flag
    bits.Put(2048, 12);      // knee_point_x
    bits.Put(1024, 12);      // knee_point_y
    bits.Put(9, 4);          // num_bezier_curve_anchors
    for (int index = 0; index < 9; ++index)
      bits.Put(static_cast<uint32_t>(64 * (index + 1)), 10);
    bits.Put(0, 1);          // colour saturation mapping off
  }
  return bits.Take();
}
} // namespace yacer
