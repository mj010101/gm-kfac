#pragma once
#include "../model/order.h"
#include <functional>
#include <future>
#include <string>

namespace oem {

class IExchangeGateway {
public:
    virtual ~IExchangeGateway() = default;

    virtual ExchangeType exchange_type() const = 0;
    virtual Exchange exchange() const = 0;

    virtual std::future<OrderResult> send_order(const Order& order) = 0;
    virtual std::future<CancelResult> cancel_order(const std::string& exchange_order_id) = 0;
    virtual std::future<OrderStatusResult> query_order(const std::string& exchange_order_id) = 0;

    virtual void subscribe_fills(std::function<void(const Fill&)> callback) = 0;
    virtual bool is_connected() const = 0;
};

} // namespace oem
