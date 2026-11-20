#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hsvj {

struct CaptureInputMode {
  int width = 0;
  int height = 0;
  double refreshRate = 0;
};

// One chip-wide setting, independent of layer geometry and input selection.
struct CaptureEdidState {
  bool custom = false;
  bool singleMode = false;
  bool applied = false;
  CaptureInputMode requested;
  CaptureInputMode preferred;
  CaptureInputMode input;
};

// Injectable device boundary permits transaction/readback tests without hardware.
class CaptureEdidDevice {
public:
  virtual ~CaptureEdidDevice() = default;
  virtual std::vector<uint8_t> read() = 0;
  virtual void write(const std::vector<uint8_t>& data) = 0;
  virtual CaptureInputMode inputMode() = 0;
};

class Rk628Edid {
public:
  explicit Rk628Edid(std::string configDirectory,
                     std::shared_ptr<CaptureEdidDevice> device = {});
  CaptureEdidState get();
  CaptureEdidState set(bool custom, int width = 0, int height = 0,
                       int refreshRate = 0, bool singleMode = false);
  // With no explicit saved setting this does not open/write the chip.
  void restoreSaved();

  static void validate(const std::vector<uint8_t>& edid);
  static std::array<uint8_t, 18> customTiming(int width, int height, int refreshRate);
  static std::vector<uint8_t> withPreferred(const std::vector<uint8_t>& original,
                                           int width, int height, int refreshRate);
  static std::vector<uint8_t> withSingleMode(const std::vector<uint8_t>& original,
                                             int width, int height, int refreshRate);
private:
  std::string directory_;
  std::shared_ptr<CaptureEdidDevice> device_;
};

} // namespace hsvj
