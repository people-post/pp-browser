#pragma once

namespace pbr {

// App-side libp2p host wrapper (integration glue; transport wiring lands separately).
class Libp2pHost {
public:
  Libp2pHost();
  ~Libp2pHost();

  Libp2pHost(const Libp2pHost&) = delete;
  Libp2pHost& operator=(const Libp2pHost&) = delete;

  bool IsAvailable() const { return available_; }

private:
  bool available_ = false;
};

} // namespace pbr
