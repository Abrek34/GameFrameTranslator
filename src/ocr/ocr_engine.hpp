#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/config.hpp"
#include "common/types.hpp"

namespace gft {

class OcrError : public std::runtime_error {
public:
    explicit OcrError(const std::string& msg) : std::runtime_error(msg) {}
};

class OcrEngine {
public:
    virtual ~OcrEngine() = default;
    virtual std::string name() const = 0;
    virtual std::vector<OcrLine> recognize(const Frame& frame,
                                           const std::optional<Rect>& roi = std::nullopt,
                                           int psm = 11, double upscale = 1.0) = 0;
};

std::unique_ptr<OcrEngine> createOcr(const Config& cfg);

}  // namespace gft
