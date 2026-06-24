// Placeholder until the JIT back end lands.
#include "engine.h"

namespace tsq {

std::unique_ptr<Executable> make_jit_executable(const BoundQuery&, const CompileOptions&, CompileStats&,
                                                std::string&, std::string&) {
  throw std::runtime_error("JIT backend not implemented yet");
}

} // namespace tsq
