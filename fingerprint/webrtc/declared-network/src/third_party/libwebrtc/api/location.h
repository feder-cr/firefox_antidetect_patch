#ifndef API_LOCATION_H_
#define API_LOCATION_H_

namespace webrtc {
class Location {
 public:
  static constexpr Location Current() { return Location(); }
  constexpr Location() = default;
  constexpr Location(const Location&) = default;
  constexpr Location& operator=(const Location&) = default;
};
}  // namespace webrtc

#endif  // API_LOCATION_H_
