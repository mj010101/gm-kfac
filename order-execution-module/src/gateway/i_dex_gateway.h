#pragma once
#include "i_exchange_gateway.h"

namespace oem {

class IDexGateway : public IExchangeGateway {
public:
    ExchangeType exchange_type() const override { return ExchangeType::DEX; }

    virtual int64_t estimated_block_time_ms() const = 0;
    virtual uint64_t next_nonce() = 0;
    virtual int64_t signing_latency_ns() const = 0;
};

} // namespace oem
