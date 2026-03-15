#pragma once
#include "i_exchange_gateway.h"

namespace oem {

class ICexGateway : public IExchangeGateway {
public:
    ExchangeType exchange_type() const override { return ExchangeType::CEX; }
};

} // namespace oem
